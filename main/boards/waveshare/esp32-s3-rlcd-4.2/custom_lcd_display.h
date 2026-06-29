#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "lcd_display.h"
#include "dashboard/dashboard_data.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // ===== 冰箱信息板（dashboard）=====
    // 独立 LVGL 屏：空闲常驻信息板，唤醒切到聊天屏。
    lv_obj_t* chat_screen_      = nullptr;   // 基类 SetupUI 建的聊天屏（= 初始 active screen）
    lv_obj_t* dashboard_screen_ = nullptr;   // 我们新建的信息板屏
    lv_obj_t* dash_clock_   = nullptr;       // 顶栏左：日期 周几 时间（本地，SetClock）
    lv_obj_t* dash_weather_ = nullptr;       // 顶栏右：天气（JSON，UpdateDashboard）
    lv_obj_t* dash_indoor_  = nullptr;       // 室内温湿度（SHTC3 本地，SetIndoor）
    lv_obj_t* dash_meals_   = nullptr;       // 三餐
    lv_obj_t* dash_fridge_  = nullptr;       // 冰箱库存
    lv_obj_t* dash_hint_    = nullptr;       // 语音提示条
    lv_obj_t* dash_stale_   = nullptr;       // stale 角标（仅异常时显示）
    TaskHandle_t screen_task_ = nullptr;     // 切屏轮询任务

    void BuildDashboardUI();                 // 搭 dashboard_screen_ 的控件
    static void ScreenSwitchTask(void* arg); // 轮询设备状态切屏

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,spi_display_config_t spiconfig,spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);

    // 重写：在基类聊天屏之上，再建 dashboard 屏并设为默认（空闲）屏，
    // 末尾起切屏轮询任务。
    virtual void SetupUI() override;

    // dashboard 刷新接口（均自带 display 锁，可跨任务调用）
    void UpdateDashboard(const DashboardData& d);          // 三餐/库存/天气/每日一句/stale
    void SetIndoor(float temp_c, float humidity);          // SHTC3 本地温湿度
    void SetClock(const std::string& hhmm,
                  const std::string& date_weekday);         // 本地日期时间
    void ShowDashboard();                                   // 切到信息板屏
    void ShowChat();                                        // 切到聊天屏
};

#endif