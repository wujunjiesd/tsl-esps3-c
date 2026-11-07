/**
 * @file app_settings.h
 * @brief 设备配置持久化（VIN / BLE 密钥 / 配对状态）
 *
 * 所有配置保存在 NVS 命名空间 "tesla_cfg" 中：
 *  - vin       : 车辆识别码（17 位，未设置时不存在）
 *  - priv_key  : 与车机配对使用的 P-256 私钥（32 字节）
 *  - paired    : 配对状态
 */

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VIN_LEN 17

/** 初始化配置模块（NVS 需要已初始化），并缓存 VIN */
esp_err_t app_settings_init(void);

/** VIN 校验：17 位，仅允许 0-9 / A-Z（不含 I、O、Q 之外的字符会被拒绝） */
bool app_settings_validate_vin(const char *vin);

/** 是否已设置 VIN */
bool app_settings_has_vin(void);

/** 读取 VIN，缓冲至少 18 字节；未设置返回 ESP_ERR_NOT_FOUND */
esp_err_t app_settings_get_vin(char *vin_out);

/** 写入 VIN（会做校验并转大写） */
esp_err_t app_settings_set_vin(const char *vin);

/** 删除 VIN */
esp_err_t app_settings_clear_vin(void);

/** 是否已与车机配对 */
bool app_settings_is_paired(void);

/** 设置配对状态 */
esp_err_t app_settings_set_paired(bool paired);

/** 读取私钥（32 字节），未保存返回 ESP_ERR_NOT_FOUND */
esp_err_t app_settings_load_key(uint8_t key[32]);

/** 保存私钥（32 字节） */
esp_err_t app_settings_save_key(const uint8_t key[32]);

/** 删除私钥（同时清除配对状态） */
esp_err_t app_settings_clear_key(void);

/** 恢复出厂：清空 VIN、私钥与配对状态 */
esp_err_t app_settings_factory_reset(void);

/** 生成配网热点名称，例如 TeslaDash-1A2B */
void app_settings_get_ap_name(char *buf, size_t len);

/** 仪表盘格子数 / 车钥匙按钮数 */
#define APP_DASH_SLOT_COUNT 6
#define APP_KEY_SLOT_COUNT  4

/**
 * @brief 读取仪表盘 6 格配置（存的是 tesla_metric_t 数值）
 * @return 未保存过返回 ESP_ERR_NOT_FOUND（调用方用默认值）
 */
esp_err_t app_settings_get_dash_slots(uint8_t out[APP_DASH_SLOT_COUNT]);

/** 保存仪表盘 6 格配置 */
esp_err_t app_settings_set_dash_slots(const uint8_t ids[APP_DASH_SLOT_COUNT]);

/**
 * @brief 读取车钥匙 4 个按钮配置（存的是 tesla_command_t 数值）
 * @return 未保存过返回 ESP_ERR_NOT_FOUND（调用方用默认值）
 */
esp_err_t app_settings_get_key_slots(uint8_t out[APP_KEY_SLOT_COUNT]);

/** 保存车钥匙 4 个按钮配置 */
esp_err_t app_settings_set_key_slots(const uint8_t ids[APP_KEY_SLOT_COUNT]);

/**
 * @brief 默认布局版本号
 *
 * 固件改了 k_default_dash / k_default_keys 之后把它 +1：
 * 已保存过槽位配置的设备会重新套用一次新默认布局，
 * 之后用户自己在配网页改的配置照常保留（不会再被覆盖）。
 */
#define APP_LAYOUT_VER 2

/** 读取已落地的布局版本号，未保存过返回 ESP_ERR_NOT_FOUND */
esp_err_t app_settings_get_layout_ver(uint8_t *ver);

/** 写入布局版本号 */
esp_err_t app_settings_set_layout_ver(uint8_t ver);

/** 屏幕模式取值（与 lcd_ui_mode_t 一致） */
#define APP_UI_MODE_DASHBOARD 0
#define APP_UI_MODE_KEY       1

/**
 * @brief 读取上次停留的屏幕模式
 * @param mode 输出 0=仪表盘 / 1=车钥匙
 * @return 未保存过返回 ESP_ERR_NOT_FOUND
 */
esp_err_t app_settings_get_ui_mode(uint8_t *mode);

/** 保存当前屏幕模式（双击或左右滑动切换时调用） */
esp_err_t app_settings_set_ui_mode(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif // APP_SETTINGS_H
