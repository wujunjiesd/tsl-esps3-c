/**
 * @file board_config.h
 * @brief Waveshare ESP32-S3-Touch-LCD-3.49 板级引脚定义
 *
 * 支持两个硬件版本，默认 V2：
 *
 *   | 项目        | V1 (3.49)            | V2 (3.49-V2)        |
 *   |-------------|----------------------|---------------------|
 *   | LCD 复位    | GPIO21               | TCA9554 EXIO5       |
 *   | LCD 背光    | GPIO8（低有效）      | GPIO42（低有效）    |
 *   | 背光使能    | -                    | TCA9554 EXIO1       |
 *
 * 其余引脚（QSPI、触摸 I2C、按键、扩展 IO）两版相同。
 * 若手上的板子是旧版，编译时把 BOARD_HW_VER 改成 1：
 *   idf.py -DBOARD_HW_VER=1 build
 * 或在 sdkconfig.defaults 里加 CONFIG_xxx，或直接修改下面的默认值。
 *
 * 参考 xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ===== 硬件版本：1 = 3.49，2 = 3.49-V2（默认）=====
#ifndef BOARD_HW_VER
#define BOARD_HW_VER 2
#endif

// ===== 屏幕（AXS15231B QSPI）=====
// 面板物理是竖条：172 列 x 640 行。
#define BOARD_LCD_PANEL_W       172
#define BOARD_LCD_PANEL_H       640

// 显示方向：1 = 横屏（逻辑 640x172，靠 MADCTL 交换 XY 实现，无软件旋转开销）
//           0 = 竖屏（逻辑 172x640）
#define BOARD_LCD_LANDSCAPE     1

#if BOARD_LCD_LANDSCAPE
#define BOARD_LCD_H_RES         640
#define BOARD_LCD_V_RES         172
#else
#define BOARD_LCD_H_RES         172
#define BOARD_LCD_V_RES         640
#endif

#define BOARD_LCD_BIT_PER_PIXEL 16

#define BOARD_LCD_CS            GPIO_NUM_9
#define BOARD_LCD_PCLK          GPIO_NUM_10
#define BOARD_LCD_D0            GPIO_NUM_11
#define BOARD_LCD_D1            GPIO_NUM_12
#define BOARD_LCD_D2            GPIO_NUM_13
#define BOARD_LCD_D3            GPIO_NUM_14

// 背光亮度脚（两版都是低电平点亮）
#define BOARD_LCD_BL_ON_LEVEL   0
#define BOARD_LCD_BL_GPIO_V1    GPIO_NUM_8    // 3.49（旧版）
#define BOARD_LCD_BL_GPIO_V2    GPIO_NUM_42   // 3.49-V2

// V1 的屏幕复位脚
#define BOARD_LCD_RST_GPIO_V1   GPIO_NUM_21

#if BOARD_HW_VER == 1
#define BOARD_LCD_RST_ON_EXIO   0   // 复位用普通 GPIO
#define BOARD_LCD_BL_EN_ON_EXIO 0   // 没有背光使能
#else
#define BOARD_LCD_RST_ON_EXIO   1   // 复位走 TCA9554 EXIO5
#define BOARD_LCD_BL_EN_ON_EXIO 1   // 背光使能走 TCA9554 EXIO1
#endif

// ===== I2C =====
// 主总线：TCA9554 扩展 IO（音频 codec 也在该总线上）
#define BOARD_I2C0_PORT         0
#define BOARD_I2C0_SDA          GPIO_NUM_47
#define BOARD_I2C0_SCL          GPIO_NUM_48

// 触摸总线
#define BOARD_I2C1_PORT         1
#define BOARD_I2C1_SDA          GPIO_NUM_17
#define BOARD_I2C1_SCL          GPIO_NUM_18
#define BOARD_TOUCH_ADDR        0x3B

// ===== TCA9554 扩展 IO =====
#define BOARD_EXIO_TOUCH_INT    0   // 触摸中断（输入，官方例程显式设为 INPUT）
#define BOARD_EXIO_LCD_BL_EN    1   // 背光使能（高有效）
#define BOARD_EXIO_LCD_RST      5   // 屏幕复位（低有效）
#define BOARD_EXIO_PWR_HOLD     6   // 电源自锁（高=保持供电）
#define BOARD_EXIO_AUX          7   // 其它板载使能（保持高）

// ===== 音频（ES8311 输出 / ES7210 输入，控制走主 I2C 总线）=====
// 引脚取自官方 08_Audio_Test 例程的 codec_board/board_cfg.txt（Board: S3_LCD_3_49）
#define BOARD_AUDIO_I2S_MCLK    GPIO_NUM_7    // ESP32 -> ES8311
#define BOARD_AUDIO_I2S_BCLK    GPIO_NUM_15
#define BOARD_AUDIO_I2S_LRCK    GPIO_NUM_46
#define BOARD_AUDIO_I2S_DOUT    GPIO_NUM_45   // ESP32 -> ES8311
#define BOARD_AUDIO_I2S_DIN     GPIO_NUM_6    // ES7210 -> ESP32（本项目暂未使用）
// esp_codec_dev 内部会做 addr >> 1，这里要给 8 位写地址
#define BOARD_ES8311_I2C_ADDR   0x30
#define BOARD_AUDIO_SAMPLE_RATE 16000

// ===== 按键 =====
#define BOARD_BUTTON_PWR        GPIO_NUM_16  // 电源键，低有效
#define BOARD_BUTTON_BOOT       GPIO_NUM_0   // BOOT 键（标注 BOOT/IO0），低有效
// 注意：板上标注 RESET 的按键直接接芯片 EN，按下即硬件复位，软件完全无法感知，
//       长按它只会让芯片持续复位，不会清除任何配置。
//       清除配置请用 BOOT 键（GPIO0）运行中长按 5 秒。
//       切勿“按住 BOOT 再点 RESET”：复位瞬间 GPIO0 为低会被 ROM 判为下载模式，
//       芯片直接停在串口下载状态，固件根本不会运行。

#endif // BOARD_CONFIG_H
