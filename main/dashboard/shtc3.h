#ifndef SHTC3_H
#define SHTC3_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_err.h>

// 板载 SHTC3 温湿度传感器（I2C 地址 0x70），挂在板子的 i2c_master 总线上。
// header-only：把已建好的 i2c_master_bus 句柄传进来加设备 + 读数。

// 在已有总线上注册 SHTC3 设备。成功返回 ESP_OK 并填 dev。
inline esp_err_t Shtc3AddDevice(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t* dev) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = 0x70;          // SHTC3 固定地址
    cfg.scl_speed_hz    = 100000;
    return i2c_master_bus_add_device(bus, &cfg, dev);
}

// 读一次温湿度。成功返回 ESP_OK，t=摄氏度，rh=相对湿度%。
// 流程：唤醒(0x3517，容忍 NACK) → 时钟拉伸测量(0x7CA2, T 先) 组合事务读6字节。
// 不休眠（设备常供电，省电无意义；sleep↔wake 切换易 NACK）。时钟拉伸下
// 传感器自己 hold 住 SCL 直到转换完成，无需固定延时，最稳。
// 不做 CRC，改用量程合理性兜底（异常值返回 ESP_ERR_INVALID_RESPONSE，不刷屏）。
inline esp_err_t Shtc3ReadOnce(i2c_master_dev_handle_t dev, float* t, float* rh) {
    uint8_t wake[2] = {0x35, 0x17};
    i2c_master_transmit(dev, wake, sizeof(wake), 100);   // 已唤醒时会 ACK，失败也继续
    vTaskDelay(pdMS_TO_TICKS(2));                          // 唤醒后 ≥240us

    uint8_t meas[2] = {0x78, 0x66};      // normal mode, clock stretch off, T first
    esp_err_t err = i2c_master_transmit(dev, meas, sizeof(meas), 100);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));        // normal 转换 ~12.1ms

    uint8_t rx[6] = {0};
    err = i2c_master_receive(dev, rx, sizeof(rx), 100);
    if (err != ESP_OK) return err;
    // 不休眠：常供电，sleep↔wake 切换会导致下次读 NACK（实测）。

    uint16_t raw_t  = (uint16_t)((rx[0] << 8) | rx[1]);
    uint16_t raw_rh = (uint16_t)((rx[3] << 8) | rx[4]);
    float temp = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    float hum  = 100.0f * (float)raw_rh / 65536.0f;

    if (temp < -40.0f || temp > 125.0f || hum < 0.0f || hum > 100.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *t = temp;
    *rh = hum;
    return ESP_OK;
}

// 读一次温湿度（带一次重试）。成功 ESP_OK，t=摄氏度，rh=湿度%。
inline esp_err_t Shtc3Read(i2c_master_dev_handle_t dev, float* t, float* rh) {
    if (dev == nullptr) return ESP_ERR_INVALID_ARG;
    esp_err_t err = Shtc3ReadOnce(dev, t, rh);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = Shtc3ReadOnce(dev, t, rh);   // 一次重试
    }
    return err;
}

#endif // SHTC3_H
