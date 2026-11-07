/**
 * @file power_ctrl.h
 * @brief 电源键处理：短按息屏/亮屏、长按关机
 *
 * 硬件说明（ESP32-S3-Touch-LCD-3.49-V2）：
 *  - 电源键接 GPIO16，按下为低电平
 *  - TCA9554 的 EXIO6 是电源自锁：拉高维持供电，拉低后松开按键即断电
 *  - 关机流程：关背光 -> 拉低自锁 -> 进入深度睡眠（USB 供电时不会断电，
 *    靠深度睡眠停机；再次按下电源键通过 EXT1 唤醒，等效“开机”）
 */

#ifndef POWER_CTRL_H
#define POWER_CTRL_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动电源键扫描任务 */
esp_err_t power_ctrl_init(void);


/** 执行关机（长按电源键或软件触发） */
void power_ctrl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_CTRL_H
