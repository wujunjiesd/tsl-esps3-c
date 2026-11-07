/**
 * @file boot_button.c
 * @brief 复位键（GPIO0）长按清除配置
 *
 * 长按 5 秒：清除 VIN、BLE 私钥与配对状态，然后重启；
 * 重启后由于 VIN 为空，设备会自动开启配网热点提示用户重新设置 VIN。
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#include "app_settings.h"
#include "board_config.h"
#include "boot_button.h"
#include "lcd_display.h"

static const char *TAG = "BOOT_KEY";

#define SCAN_PERIOD_MS      20
#define DEBOUNCE_MS         50
#define WARN_MS             2000   // 提示“继续长按将清除”
#define LONG_PRESS_MS       5000   // 清除配置

static bool s_initialized = false;
static bool s_busy = false;
static boot_button_long_press_cb_t s_long_press_cb = NULL;

static void do_factory_reset(void)
{
    s_busy = true;
    ESP_LOGW(TAG, "长按 BOOT 键 -> 清除 VIN 与车机连接密钥");

    // 息屏状态下也要看得到提示，先点屏
    lcd_display_set_backlight(true);
    lcd_display_set_hint("正在清除配置");
    lcd_display_refresh();
    vTaskDelay(pdMS_TO_TICKS(600));

    app_settings_factory_reset();

    // 立即切到配网引导页，提示用户用手机连 WiFi 重新设置 VIN
    char ssid[33] = {0};
    app_settings_get_ap_name(ssid, sizeof(ssid));
    lcd_display_set_vin(NULL);
    lcd_display_set_paired(false);
    lcd_display_set_link(LCD_LINK_DISCONNECTED);
    lcd_display_set_setup_mode(true, ssid);
    lcd_display_set_hint("已清除，请配网");
    lcd_display_refresh();

    ESP_LOGW(TAG, "配置已清除，请松开按键（松开后自动重启）");

    // 关键：必须等按键松开再重启。
    // 否则复位瞬间 BOOT 仍被按住，芯片会进入 ROM 下载模式（表现为黑屏、无任何反应）。
    for (int i = 0; i < 1500; i++) {   // 最多等 30 秒
        if (gpio_get_level(BOARD_BUTTON_BOOT) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGW(TAG, "重启进入配网模式");
    esp_restart();
}

static void boot_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool pressed = false;
    bool fired = false;
    bool warned = false;
    int held_ms = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));

        if (s_busy) {
            continue;
        }

        bool level_low = (gpio_get_level(BOARD_BUTTON_BOOT) == 0);

        if (level_low) {
            if (!pressed) {
                pressed = true;
                fired = false;
                warned = false;
                held_ms = 0;
            } else {
                held_ms += SCAN_PERIOD_MS;

                if (!warned && held_ms >= WARN_MS) {
                    warned = true;
                    // 业务层接管长按后，行为不再是"清除配置"，提示要跟着变
                    lcd_display_set_hint(s_long_press_cb ? "继续按住进入配置模式"
                                                         : "按住将清除配置");
                    lcd_display_refresh();
                }
                if (!fired && held_ms >= LONG_PRESS_MS) {
                    fired = true;
                    if (s_long_press_cb) {
                        // 业务层接管：进入配置界面，不再清配置。
                        // 这里只置标志，不用 s_busy 锁死按键：重复触发已由 fired 挡住，
                        // 万一业务层没能进配置，按键也不至于从此失效。
                        ESP_LOGW(TAG, "长按 BOOT 键 -> 进入配置界面");
                        s_long_press_cb();
                    } else {
                        do_factory_reset();
                    }
                }
            }
        } else {
            if (pressed) {
                pressed = false;
                if (!fired && held_ms >= DEBOUNCE_MS && warned) {
                    // 中途松手：恢复提示
                    lcd_display_set_hint(NULL);
                    lcd_display_refresh();
                }
            }
        }
    }
}

esp_err_t boot_button_init(boot_button_long_press_cb_t long_press_cb)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;
    // 必须在建任务之前装好回调：任务一启动就开始扫按键，
    // 回调晚一步装的话，开机时按住 BOOT 会走默认的 do_factory_reset() 清掉配置
    s_long_press_cb = long_press_cb;

    BaseType_t ok = xTaskCreate(boot_task, "boot_key", 3072, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "复位键任务已启动（长按 5 秒清除配置）");
    return ESP_OK;
}
