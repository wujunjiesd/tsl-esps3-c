/**
 * @file app_settings.c
 * @brief 设备配置持久化实现
 */

#include <ctype.h>
#include <string.h>

#include "app_settings.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SETTINGS";

#define NVS_NAMESPACE    "tesla_cfg"
#define KEY_VIN          "vin"
#define KEY_PRIVATE      "priv_key"
#define KEY_PAIRED       "paired"
#define KEY_AP_NAME      "ap_name"
#define KEY_UI_MODE      "ui_mode"
#define KEY_LAYOUT_VER   "layout_ver"
#define KEY_DASH_SLOTS   "dash_slots"
#define KEY_KEY_SLOTS    "key_slots"

static char s_vin[APP_VIN_LEN + 1] = {0};
static bool s_vin_valid = false;

// s_vin 的读写跨多个任务：配网 httpd 线程在提交表单时写，BLE 任务在拼消息时读。
// 没有这把锁，读者可能拿到新旧字节拼在一起的 17 位字符串。
static SemaphoreHandle_t s_vin_lock = NULL;

static inline void vin_lock(void)
{
    if (s_vin_lock) xSemaphoreTake(s_vin_lock, portMAX_DELAY);
}

static inline void vin_unlock(void)
{
    if (s_vin_lock) xSemaphoreGive(s_vin_lock);
}

static esp_err_t open_nvs(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(NVS_NAMESPACE, mode, out);
}

/* ---------------------------------------------------------------------------
 * NVS 薄封装
 *
 * 下面每个配置项原本都要写一遍「打开命名空间 → 读/写一项 → 提交 → 关闭」，
 * 全文件重复 17 次，样板里漏掉 commit 或 close 都不会报错，只是静默丢数据 /
 * 泄漏句柄。统一收口到这一组函数后，调用点一律只剩一行。
 * ------------------------------------------------------------------------- */

static esp_err_t cfg_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_get_u8(const char *key, uint8_t *val)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(handle, key, val);
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_set_str(const char *key, const char *str)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, str);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_get_str(const char *key, char *buf, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, key, buf, len);
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_set_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_get_blob(const char *key, void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = len;
    err = nvs_get_blob(handle, key, data, &size);
    nvs_close(handle);
    return err;
}

// 键不存在视为已清除成功
static esp_err_t cfg_erase(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_erase_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void cache_vin_from_nvs(void)
{
    vin_lock();
    s_vin_valid = false;
    s_vin[0] = '\0';
    vin_unlock();

    char tmp[APP_VIN_LEN + 1] = {0};
    size_t len = sizeof(tmp);
    esp_err_t err = cfg_get_str(KEY_VIN, tmp, &len);
    bool ok = (err == ESP_OK) && strlen(tmp) == APP_VIN_LEN;
    ESP_LOGI(TAG, "读取 VIN: %s (err=0x%x, strlen=%u)",
             ok ? tmp : "<无效>", (unsigned)err, (unsigned)strlen(tmp));

    // 先读进临时缓冲再整体发布，避免读到一半时被别的任务看见
    vin_lock();
    if (ok) {
        memcpy(s_vin, tmp, APP_VIN_LEN + 1);
    } else {
        s_vin[0] = '\0';
    }
    s_vin_valid = ok;
    vin_unlock();
}

esp_err_t app_settings_init(void)
{
    if (!s_vin_lock) {
        s_vin_lock = xSemaphoreCreateMutex();
        if (!s_vin_lock) {
            ESP_LOGE(TAG, "创建 VIN 锁失败，读写将不加保护");
        }
    }
    cache_vin_from_nvs();
    vin_lock();
    ESP_LOGI(TAG, "VIN: %s", s_vin_valid ? s_vin : "<未设置>");
    vin_unlock();
    return ESP_OK;
}

bool app_settings_validate_vin(const char *vin)
{
    if (!vin) {
        return false;
    }
    if (strlen(vin) != APP_VIN_LEN) {
        return false;
    }
    for (int i = 0; i < APP_VIN_LEN; i++) {
        char c = (char)toupper((unsigned char)vin[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

bool app_settings_has_vin(void)
{
    vin_lock();
    bool valid = s_vin_valid;
    vin_unlock();
    return valid;
}

esp_err_t app_settings_get_vin(char *vin_out)
{
    if (!vin_out) {
        return ESP_ERR_INVALID_ARG;
    }
    vin_lock();
    if (!s_vin_valid) {
        vin_unlock();
        vin_out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(vin_out, s_vin, APP_VIN_LEN + 1);
    vin_unlock();
    return ESP_OK;
}

esp_err_t app_settings_set_vin(const char *vin)
{
    if (!app_settings_validate_vin(vin)) {
        ESP_LOGW(TAG, "VIN 格式非法");
        return ESP_ERR_INVALID_ARG;
    }

    char upper[APP_VIN_LEN + 1];
    for (int i = 0; i < APP_VIN_LEN; i++) {
        upper[i] = (char)toupper((unsigned char)vin[i]);
    }
    upper[APP_VIN_LEN] = '\0';

    esp_err_t err = cfg_set_str(KEY_VIN, upper);

    if (err == ESP_OK) {
        vin_lock();
        memcpy(s_vin, upper, sizeof(s_vin));
        s_vin_valid = true;
        ESP_LOGI(TAG, "VIN 已保存: %s", s_vin);
        vin_unlock();
    }
    return err;
}

esp_err_t app_settings_clear_vin(void)
{
    esp_err_t err = cfg_erase(KEY_VIN);

    vin_lock();
    s_vin[0] = '\0';
    s_vin_valid = false;
    vin_unlock();
    ESP_LOGI(TAG, "VIN 已清除");
    return err;
}

bool app_settings_is_paired(void)
{
    uint8_t paired = 0;
    esp_err_t err = cfg_get_u8(KEY_PAIRED, &paired);
    return (err == ESP_OK && paired == 1);
}

esp_err_t app_settings_set_paired(bool paired)
{
    return cfg_set_u8(KEY_PAIRED, paired ? 1 : 0);
}

esp_err_t app_settings_load_key(uint8_t key[32])
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_get_blob(KEY_PRIVATE, key, 32);
}

esp_err_t app_settings_save_key(const uint8_t key[32])
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_set_blob(KEY_PRIVATE, key, 32);
}

esp_err_t app_settings_clear_key(void)
{
    // 密钥和配对状态必须在同一次提交里清掉，否则中途掉电会留下"未配对但还留着旧密钥"
    // 的半状态
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t e1 = nvs_erase_key(handle, KEY_PRIVATE);
    esp_err_t e2 = nvs_erase_key(handle, KEY_PAIRED);
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) {
        err = e1;
    } else if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) {
        err = e2;
    } else {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BLE 密钥与配对状态已清除");
    }
    return err;
}

esp_err_t app_settings_factory_reset(void)
{
    esp_err_t err = cfg_erase_all();

    vin_lock();
    s_vin[0] = '\0';
    s_vin_valid = false;
    vin_unlock();
    ESP_LOGW(TAG, "配置已恢复出厂（VIN / 密钥 / 配对状态全部清除）");
    return err;
}

void app_settings_get_ap_name(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    // 优先使用已保存的自定义名称
    size_t got = len;
    if (cfg_get_str(KEY_AP_NAME, buf, &got) == ESP_OK) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "TeslaDash-%02X%02X", mac[4], mac[5]);
}

/* ==================== 界面槽位配置 ==================== */

esp_err_t app_settings_get_dash_slots(uint8_t out[APP_DASH_SLOT_COUNT])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_get_blob(KEY_DASH_SLOTS, out, APP_DASH_SLOT_COUNT);
}

esp_err_t app_settings_set_dash_slots(const uint8_t ids[APP_DASH_SLOT_COUNT])
{
    if (!ids) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "仪表盘配置: %u %u %u %u %u %u",
             ids[0], ids[1], ids[2], ids[3], ids[4], ids[5]);
    return cfg_set_blob(KEY_DASH_SLOTS, ids, APP_DASH_SLOT_COUNT);
}

/* ==================== 屏幕模式（记住最后停留在哪一页） ==================== */

esp_err_t app_settings_get_ui_mode(uint8_t *mode)
{
    if (!mode) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_get_u8(KEY_UI_MODE, mode);
}

esp_err_t app_settings_set_ui_mode(uint8_t mode)
{
    if (mode != APP_UI_MODE_DASHBOARD && mode != APP_UI_MODE_KEY) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = cfg_set_u8(KEY_UI_MODE, mode);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "记住屏幕模式: %s",
                 mode == APP_UI_MODE_KEY ? "车钥匙" : "仪表盘");
    }
    return err;
}

esp_err_t app_settings_get_key_slots(uint8_t out[APP_KEY_SLOT_COUNT])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_get_blob(KEY_KEY_SLOTS, out, APP_KEY_SLOT_COUNT);
}

esp_err_t app_settings_set_key_slots(const uint8_t ids[APP_KEY_SLOT_COUNT])
{
    if (!ids) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "车钥匙按钮配置: %u %u %u %u", ids[0], ids[1], ids[2], ids[3]);
    return cfg_set_blob(KEY_KEY_SLOTS, ids, APP_KEY_SLOT_COUNT);
}

esp_err_t app_settings_get_layout_ver(uint8_t *ver)
{
    if (!ver) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg_get_u8(KEY_LAYOUT_VER, ver) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t app_settings_set_layout_ver(uint8_t ver)
{
    return cfg_set_u8(KEY_LAYOUT_VER, ver);
}
