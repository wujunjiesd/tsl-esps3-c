/**
 * @file wifi_portal.h
 * @brief VIN 配网门户：设备开启 WiFi 热点，用户用手机/电脑网页设置 VIN
 */

#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PORTAL_URL "192.168.4.1"

/**
 * @brief 启动配网热点与 HTTP 服务
 * @param ap_ssid 热点名称，NULL 时自动生成（TeslaDash-XXXX）
 * @return ESP_OK 成功
 * @note 启动后设备会一直保持在配网模式，用户提交 VIN 后自动重启
 */
esp_err_t wifi_portal_start(const char *ap_ssid);

/**
 * @brief 注册「界面配置已保存」回调
 *
 * 保存成功后立刻触发，业务层据此重新加载 6 格 / 4 按钮配置，屏幕即时更新。
 * 回调在 httpd 线程里执行，不要在里面做耗时操作。
 */
void wifi_portal_set_config_saved_cb(void (*cb)(void));




/** 是否收到并保存了新的 VIN（用于界面提示） */
bool wifi_portal_take_saved_flag(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PORTAL_H
