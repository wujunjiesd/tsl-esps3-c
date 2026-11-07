/**
 * @file boot_button.h
 * @brief BOOT/RESET 按键：长按 5 秒清除 VIN 与车机连接密钥
 */

#ifndef BOOT_BUTTON_H
#define BOOT_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 长按（5 秒）回调类型 */
typedef void (*boot_button_long_press_cb_t)(void);

/**
 * @brief 启动复位键扫描任务
 *
 * @param long_press_cb 长按回调。注册后长按不再直接"清除 VIN 并重启"，
 *                      而是交给业务层决定（本工程用它进入 WiFi 配置界面）。
 *                      传 NULL 保持旧的恢复出厂行为。
 * @note 回调必须在建任务之前给出，所以只能走 init 参数，不能用后置注册。
 */
esp_err_t boot_button_init(boot_button_long_press_cb_t long_press_cb);

#ifdef __cplusplus
}
#endif

#endif // BOOT_BUTTON_H
