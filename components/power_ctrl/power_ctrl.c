/**
 * @file power_ctrl.c
 * @brief 电源键处理实现
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "board.h"
#include "board_config.h"
#include "lcd_display.h"
#include "power_ctrl.h"

static const char *TAG = "PWR";

#define SCAN_PERIOD_MS      20
#define DEBOUNCE_MS         40
#define LONG_PRESS_MS       2500

static bool s_initialized = false;


static void power_ctrl_toggle_screen(void)
{
    bool on = lcd_display_backlight_is_on();
    lcd_display_set_backlight(!on);
    ESP_LOGI(TAG, "短按电源键 -> %s", on ? "息屏" : "亮屏");
}

void power_ctrl_shutdown(void)
{
    ESP_LOGW(TAG, "开始关机流程");

    lcd_display_set_hint("关机中");
    lcd_display_refresh();
    vTaskDelay(pdMS_TO_TICKS(700));   // 让提示先显示出来

    lcd_display_set_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 释放电源自锁：电池供电时硬件随后断电
    board_power_hold(false);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 配置电源键（GPIO16）为唤醒源，按下（低电平）唤醒
    esp_err_t rc = esp_sleep_enable_ext1_wakeup(1ULL << BOARD_BUTTON_PWR,
                                                ESP_EXT1_WAKEUP_ANY_LOW);
    if (rc != ESP_OK) {
        // 没有唤醒源就睡下去 = 再也起不来，宁可不睡也不能把设备变砖
        ESP_LOGE(TAG, "配置唤醒源失败(%s)，放弃进入深度睡眠", esp_err_to_name(rc));
        board_power_hold(true);
        lcd_display_set_backlight(true);
        lcd_display_set_hint("关机失败");
        return;
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    ESP_LOGW(TAG, "进入深度睡眠，按电源键开机");
    esp_deep_sleep_start();
}

static void power_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_PWR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool pressed = false;
    bool long_fired = false;
    int pressed_ms = 0;

    // 如果是“按住电源键开机”，按键在启动时仍处于按下状态：
    // 忽略这一次长按，避免刚开机就被判成关机
    if (gpio_get_level(BOARD_BUTTON_PWR) == 0) {
        pressed = true;
        long_fired = true;
        ESP_LOGI(TAG, "检测到开机按键仍被按住，忽略本次长按");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
        bool level_low = (gpio_get_level(BOARD_BUTTON_PWR) == 0);

        if (level_low) {
            if (!pressed) {
                pressed = true;
                pressed_ms = 0;
                long_fired = false;
            } else {
                pressed_ms += SCAN_PERIOD_MS;
                if (!long_fired && pressed_ms >= LONG_PRESS_MS) {
                    long_fired = true;
                    ESP_LOGW(TAG, "长按电源键 -> 关机");
                    power_ctrl_shutdown();
                }
            }
        } else {
            if (pressed) {
                pressed = false;
                if (!long_fired && pressed_ms >= DEBOUNCE_MS) {
                    power_ctrl_toggle_screen();
                }
            }
        }
    }
}

esp_err_t power_ctrl_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;

    BaseType_t ok = xTaskCreate(power_task, "power_key", 3072, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "电源键任务已启动");
    return ESP_OK;
}
