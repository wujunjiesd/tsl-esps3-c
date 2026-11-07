/**
 * @file board.c
 * @brief Waveshare ESP32-S3-Touch-LCD-3.49 (V2) 板级初始化
 *
 * 关键点：
 *  - 上电第一件事是拉高 TCA9554 EXIO6（电源自锁），否则松开电源键立即断电
 *  - EXIO1 为屏幕背光使能，EXIO5 为屏幕复位（V2）
 *  - 所有失败都只记录日志，尽量不影响后续功能，便于定位问题
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_io_expander_tca9554.h"

static const char *TAG = "BOARD";

static int s_hw_ver = BOARD_HW_VER;      // 实际生效的硬件版本
static bool s_use_exio = (BOARD_HW_VER == 2);

static i2c_master_bus_handle_t s_i2c_main = NULL;
static i2c_master_bus_handle_t s_i2c_touch = NULL;
static esp_io_expander_handle_t s_expander = NULL;

#define EXIO_PIN(n) ((uint32_t)1 << (n))
#define EXIO_OUT_MASK \
    (EXIO_PIN(BOARD_EXIO_LCD_BL_EN) | EXIO_PIN(BOARD_EXIO_LCD_RST) | \
     EXIO_PIN(BOARD_EXIO_PWR_HOLD) | EXIO_PIN(BOARD_EXIO_AUX))

static void lcd_rst_gpio_release(void);

static esp_err_t create_i2c(int port, gpio_num_t sda, gpio_num_t scl,
                            i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };
    return i2c_new_master_bus(&cfg, out);
}

/** 扫描总线上的 I2C 设备，用于诊断硬件连接 */
static void scan_i2c(i2c_master_bus_handle_t bus, const char *name)
{
    if (!bus) {
        return;
    }
    char found[24] = {0};
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            char one[6];
            snprintf(one, sizeof(one), " %02X", addr);
            strncat(found, one, sizeof(found) - strlen(found) - 1);
        }
    }
    ESP_LOGI(TAG, "%s 上的 I2C 设备:%s", name, found[0] ? found : " 无");
}

esp_err_t board_init(void)
{
    esp_err_t ret;

    if (s_expander) {
        return ESP_OK;  // 已初始化
    }

    ret = create_i2c(BOARD_I2C0_PORT, BOARD_I2C0_SDA, BOARD_I2C0_SCL, &s_i2c_main);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "主 I2C 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = create_i2c(BOARD_I2C1_PORT, BOARD_I2C1_SDA, BOARD_I2C1_SCL, &s_i2c_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "触摸 I2C 总线初始化失败: %s", esp_err_to_name(ret));
        // 第一条总线已经建好了，不回收就一直占着一组 GPIO
        if (s_i2c_main) {
            i2c_del_master_bus(s_i2c_main);
            s_i2c_main = NULL;
        }
        return ret;
    }

    // 诊断：打印总线上挂的设备（V2 正常应能看到 TCA9554=0x20，可能还有 0x18/0x40 音频）
    scan_i2c(s_i2c_main, "I2C0(47/48)");
    scan_i2c(s_i2c_touch, "I2C1(17/18)");

    // TCA9554：失败重试几次（上电瞬间扩展 IO 可能还没准备好）
    for (int i = 0; i < 3; i++) {
        ret = esp_io_expander_new_i2c_tca9554(s_i2c_main, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                              &s_expander);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "TCA9554 初始化失败(%d/3): %s", i + 1, esp_err_to_name(ret));
        s_expander = NULL;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_expander) {
        ESP_LOGE(TAG, "TCA9554 不可用");
        // 兜底：如果按 V2 编译却找不到扩展 IO，说明很可能插的是旧版（3.49）板，
        // 或者扩展 IO 硬件异常。旧版的背光/复位是独立 GPIO，此时直接切到 V1 引脚，
        // 至少能让屏幕点亮（电源自锁随之失效，USB 供电下仍可工作）。
        if (BOARD_HW_VER != 1) {
            ESP_LOGW(TAG, "按 V1 引脚继续：背光 GPIO%d，复位 GPIO%d",
                     BOARD_LCD_BL_GPIO_V1, BOARD_LCD_RST_GPIO_V1);
            s_hw_ver = 1;
            s_use_exio = false;
            gpio_config_t bl = {
                .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO_V1,
                .mode = GPIO_MODE_OUTPUT,
            };
            gpio_config(&bl);
            gpio_set_level(BOARD_LCD_BL_GPIO_V1, !BOARD_LCD_BL_ON_LEVEL);
            lcd_rst_gpio_release();   // RST=GPIO21，先保持非复位
            return ESP_OK;
        }
        // 走到这里两条总线都没用了，回收掉再报错，避免资源悬挂
        if (s_i2c_touch) {
            i2c_del_master_bus(s_i2c_touch);
            s_i2c_touch = NULL;
        }
        if (s_i2c_main) {
            i2c_del_master_bus(s_i2c_main);
            s_i2c_main = NULL;
        }
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_io_expander_set_dir(s_expander, EXIO_OUT_MASK, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 方向设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 触摸 INT 挂在 EXIO0，官方例程显式设为输入。这里跟随设置一次，
    // 避免扩展 IO 上电默认值不确定时把中断线配成输出（会直接影响触摸初始化）。
    esp_io_expander_set_dir(s_expander, EXIO_PIN(BOARD_EXIO_TOUCH_INT), IO_EXPANDER_INPUT);

    // 先拉高电源自锁，保证松开电源键后继续供电
    esp_io_expander_set_level(s_expander, EXIO_PIN(BOARD_EXIO_PWR_HOLD), 1);
    ESP_LOGW(TAG, "电源自锁已开启 (EXIO%d=1)", BOARD_EXIO_PWR_HOLD);

    // 背光先关闭，等屏幕初始化完成后再打开
    if (BOARD_LCD_BL_EN_ON_EXIO) {
        esp_io_expander_set_level(s_expander, EXIO_PIN(BOARD_EXIO_LCD_BL_EN), 0);
    }
    // 复位脚保持高（非复位状态）
    if (BOARD_LCD_RST_ON_EXIO) {
        esp_io_expander_set_level(s_expander, EXIO_PIN(BOARD_EXIO_LCD_RST), 1);
    }
    // 另一路候选复位脚（GPIO21，R69 焊接时生效）：上拉到高，避免悬空被拉低
    lcd_rst_gpio_release();
    esp_io_expander_set_level(s_expander, EXIO_PIN(BOARD_EXIO_AUX), 1);

    s_hw_ver = BOARD_HW_VER;
    s_use_exio = (BOARD_HW_VER == 2);

    // 回读扩展 IO 实际电平，用于确认电源自锁 / 背光使能是否真的生效
    uint32_t lvl = 0;
    if (esp_io_expander_get_level(s_expander, EXIO_OUT_MASK, &lvl) == ESP_OK) {
        ESP_LOGI(TAG, "扩展IO电平: 0x%02" PRIx32 " (BL_EN(EXIO%d)=%d RST(EXIO%d)=%d "
                      "PWR_HOLD(EXIO%d)=%d AUX(EXIO%d)=%d)",
                 (uint32_t)lvl, BOARD_EXIO_LCD_BL_EN, (int)((lvl >> BOARD_EXIO_LCD_BL_EN) & 1),
                 BOARD_EXIO_LCD_RST, (int)((lvl >> BOARD_EXIO_LCD_RST) & 1),
                 BOARD_EXIO_PWR_HOLD, (int)((lvl >> BOARD_EXIO_PWR_HOLD) & 1),
                 BOARD_EXIO_AUX, (int)((lvl >> BOARD_EXIO_AUX) & 1));
    }

    ESP_LOGI(TAG, "板级初始化完成 (HW_VER=%d, 背光 GPIO%d)", s_hw_ver,
             (int)board_backlight_gpio());
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_main(void)
{
    return s_i2c_main;
}

i2c_master_bus_handle_t board_i2c_touch(void)
{
    return s_i2c_touch;
}

gpio_num_t board_backlight_gpio(void)
{
    return (s_hw_ver == 1) ? BOARD_LCD_BL_GPIO_V1 : BOARD_LCD_BL_GPIO_V2;
}

esp_err_t board_backlight_enable(bool enable)
{
    esp_err_t ret = ESP_OK;

    // V2：EXIO1 是背光供电使能
    if (s_use_exio) {
        if (!s_expander) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = esp_io_expander_set_level(s_expander,
                                        EXIO_PIN(BOARD_EXIO_LCD_BL_EN), enable ? 1 : 0);
    }

    // 亮度脚：反相 PWM 引脚，直接推电平（0=最亮）
    gpio_set_level(board_backlight_gpio(),
                   enable ? BOARD_LCD_BL_ON_LEVEL : !BOARD_LCD_BL_ON_LEVEL);
    return ret;
}

/**
 * @brief 把 GPIO21 释放为“高阻 + 上拉”
 *
 * 原理图上 LCD_RST 与 LCD_TE 是成对二选一的 0R 焊盘：
 *   R69 焊 / R73 NC -> LCD_RST = GPIO21，LCD_TE 悬空
 *   R73 焊 / R69 NC -> LCD_RST = EXIO5， LCD_TE  = GPIO21
 *
 * 因此 GPIO21 既可能是复位脚、也可能是 TE 输出脚。复位做完后把它切成
 * 输入 + 内部上拉：若它是复位脚，上拉保证不处于复位态（悬空时会被漏电
 * 拉低，屏幕就一直停在未初始化的灰白状态）；若它是 TE，则高阻不冲突。
 */
static void lcd_rst_gpio_release(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RST_GPIO_V1,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in);
}

/**
 * @brief 屏幕硬件复位
 *
 * 不依赖“这块板到底是 V1 还是 V2 接线”：EXIO5 与 GPIO21 同时输出
 * 复位脉冲（两根线里只有一根真正焊到面板 RSTN，另一根无副作用或为 TE）。
 * 这样无论 R69 / R73 哪颗 0R 焊上都能复位屏幕。
 */
esp_err_t board_lcd_reset(void)
{
    const uint32_t exio_rst = EXIO_PIN(BOARD_EXIO_LCD_RST);
    const bool has_exio = (s_expander != NULL);

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RST_GPIO_V1,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    // 释放复位
    if (has_exio) {
        esp_io_expander_set_level(s_expander, exio_rst, 1);
    }
    gpio_set_level(BOARD_LCD_RST_GPIO_V1, 1);
    vTaskDelay(pdMS_TO_TICKS(30));

    // 拉低复位
    if (has_exio) {
        esp_io_expander_set_level(s_expander, exio_rst, 0);
    }
    gpio_set_level(BOARD_LCD_RST_GPIO_V1, 0);
    vTaskDelay(pdMS_TO_TICKS(250));

    // 结束复位
    if (has_exio) {
        esp_io_expander_set_level(s_expander, exio_rst, 1);
    }
    gpio_set_level(BOARD_LCD_RST_GPIO_V1, 1);
    // 官方例程给 30ms；面板复位释放后内部需要起振 + 加载 OTP，留足余量
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_rst_gpio_release();
    return ESP_OK;
}

esp_err_t board_power_hold(bool hold)
{
    if (!s_expander) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "电源自锁 -> %d", hold ? 1 : 0);
    return esp_io_expander_set_level(s_expander, EXIO_PIN(BOARD_EXIO_PWR_HOLD), hold ? 1 : 0);
}
