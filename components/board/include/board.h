/**
 * @file board.h
 * @brief Waveshare ESP32-S3-Touch-LCD-3.49-V2 板级初始化
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化板级资源（I2C 总线、TCA9554 扩展 IO、电源自锁）
 * @note 必须尽早在 app_main 中调用：TCA9554 EXIO6 是电源自锁，
 *       未拉高时松开电源键设备会立即断电。
 */
esp_err_t board_init(void);

/** 主 I2C 总线（TCA9554 / codec） */
i2c_master_bus_handle_t board_i2c_main(void);

/** 触摸 I2C 总线 */
i2c_master_bus_handle_t board_i2c_touch(void);

/** 当前实际使用的背光亮度 GPIO（V1=GPIO8 / V2=GPIO42） */
gpio_num_t board_backlight_gpio(void);

/** 背光使能（V2: TCA9554 EXIO1） */
esp_err_t board_backlight_enable(bool enable);

/** 屏幕硬件复位（TCA9554 EXIO5），执行 1→0→1 序列 */
esp_err_t board_lcd_reset(void);

/** 电源自锁控制：false = 释放，设备随后断电 */
esp_err_t board_power_hold(bool hold);

#ifdef __cplusplus
}
#endif

#endif // BOARD_H
