#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "lvgl.h"
#include "custom_lcd_display.h"
#include "settings.h"

#include <ctime>
#include <cstdio>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include "dashboard/dashboard_data.h"
#include "dashboard/dashboard_json.h"
#include "dashboard/shtc3.h"

#define TAG "waveshare_rlcd_4_2"

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    CustomLcdDisplay *display_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;
    i2c_master_dev_handle_t shtc3_dev_ = nullptr;   // 板载温湿度传感器

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() { 
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH,RLCD_HEIGHT,DISPLAY_OFFSET_X,DISPLAY_OFFSET_Y,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y,DISPLAY_SWAP_XY,spi_config);
    }

    // ===== M3：真实数据服务（时钟 / 温湿度 / 数据桥拉取）=====

    // 拉一次 dashboard.json 并解析。成功返回 true 并填 out。
    bool FetchDashboard(const std::string& url, DashboardData& out) {
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = 6000;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == nullptr) return false;

        bool ok = false;
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            std::string body;
            char buf[512];
            int r;
            while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                body.append(buf, r);
                if (body.size() > 16384) break;   // 安全上限，dashboard.json 很小
            }
            int status = esp_http_client_get_status_code(client);
            esp_http_client_close(client);
            if (status == 200 && !body.empty()) {
                DashboardData parsed;
                if (ParseDashboardJson(body.c_str(), parsed)) { out = parsed; ok = true; }
            } else {
                ESP_LOGW(TAG, "dashboard http status=%d len=%d", status, (int)body.size());
            }
        } else {
            ESP_LOGW(TAG, "dashboard http open failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        return ok;
    }

    // 时钟任务：每秒刷本地时间（时间由服务器 OTA 校准，TZ=东八区）
    static void ClockTask(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        setenv("TZ", "CST-8", 1);
        tzset();
        static const char* kWeekday[7] = {"周日","周一","周二","周三","周四","周五","周六"};
        for (;;) {
            time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            if (tm_now.tm_year + 1900 >= 2024) {   // 时间已校准才显示
                char hhmm[8];
                snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
                char dw[40];
                snprintf(dw, sizeof(dw), "%d/%d %s", tm_now.tm_mon + 1, tm_now.tm_mday,
                         kWeekday[tm_now.tm_wday]);
                self->display_->SetClock(hhmm, dw);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 温湿度任务：每 30s 读板载 SHTC3
    static void Shtc3Task(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        if (Shtc3AddDevice(self->i2c_bus_, &self->shtc3_dev_) != ESP_OK) {
            ESP_LOGW(TAG, "SHTC3 add device failed; indoor temp disabled");
            vTaskDelete(nullptr);
            return;
        }
        for (;;) {
            float t = 0, rh = 0;
            if (Shtc3Read(self->shtc3_dev_, &t, &rh) == ESP_OK) {
                self->display_->SetIndoor(t, rh);
                ESP_LOGI(TAG, "SHTC3 %.1fC %.0f%%", t, rh);
            } else {
                ESP_LOGW(TAG, "SHTC3 read failed");
            }
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }

    // 数据桥任务：每 5min 拉 dashboard.json；失败保留缓存 + stale 角标；
    // 未配置 URL 时屏上提示用语音设置。
    static void DashboardClientTask(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        DashboardData cache;
        bool have_data = false;
        int64_t last_ok_us = 0;
        vTaskDelay(pdMS_TO_TICKS(8000));   // 等 WiFi / 系统起来
        for (;;) {
            std::string url;
            { Settings s("dashboard", false); url = s.GetString("url", ""); }

            if (url.empty()) {
                DashboardData d;
                d.lunch = "（语音说「设置信息板地址」配置数据源）";
                self->display_->UpdateDashboard(d);
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }

            DashboardData fetched;
            if (self->FetchDashboard(url, fetched)) {
                cache = fetched;
                cache.stale_minutes = 0;
                have_data = true;
                last_ok_us = esp_timer_get_time();
                self->display_->UpdateDashboard(cache);
                ESP_LOGI(TAG, "dashboard updated");
            } else if (have_data) {
                cache.stale_minutes = (int)((esp_timer_get_time() - last_ok_us) / 60000000LL);
                self->display_->UpdateDashboard(cache);
                ESP_LOGW(TAG, "dashboard fetch failed, stale %d min", cache.stale_minutes);
            }
            vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        }
    }

    void InitializeDashboardServices() {
        // MCP 工具：运行时设置数据源地址（存 NVS）
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.dashboard.set_url",
            "设置冰箱信息板的数据源地址（数据桥 dashboard.json 的完整 URL）",
            PropertyList({ Property("url", kPropertyTypeString) }),
            [](const PropertyList& props) -> ReturnValue {
                auto url = props["url"].value<std::string>();
                Settings s("dashboard", true);
                s.SetString("url", url);
                return true;
            });

        xTaskCreate(ClockTask,           "dash_clock", 3072, this, 3, nullptr);
        xTaskCreate(Shtc3Task,           "dash_shtc3", 3072, this, 3, nullptr);
        xTaskCreate(DashboardClientTask, "dash_http",  8192, this, 4, nullptr);
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage =  raw_voltage * 3;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }

        return 0;
    }

    uint8_t BatterygetPercent() {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }

        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        // ESP_LOGI(TAG, "voltage: %dmV, percentage: %d%%", voltage, percent);
        return (uint8_t)percent;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO) {    
        InitializeI2c();
        InitializeButtons();
        InitializeTools();
        InitializeLcdDisplay();
        InitializeDashboardServices();
   }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();

        return true;
    }
};

DECLARE_BOARD(CustomBoard);