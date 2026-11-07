/**
 * @file audio_prompt.h
 * @brief 语音提示（ES8311 + I2S 喇叭，播放内嵌的中文语音）
 *
 * 语音资源以 WAV(16kHz/单声道/16bit) 形式编译进固件，运行时按需播放。
 * 播放放在独立任务里，调用方只入队，不会阻塞蓝牙轮询等业务逻辑。
 */

#ifndef AUDIO_PROMPT_H
#define AUDIO_PROMPT_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_PROMPT_NONE = -1,
    AUDIO_PROMPT_VIN_SAVED = 0,   // "设置成功，正在重启"
    AUDIO_PROMPT_CONNECTED,       // "已连接车辆"
    AUDIO_PROMPT_REVERSE,         // "倒车请注意"
    AUDIO_PROMPT_KEY_MODE,        // "钥匙模式"
    AUDIO_PROMPT_COUNT,
} audio_prompt_id_t;

/**
 * @brief 初始化 I2S 与 ES8311，并启动播放任务
 * @return ESP_OK 成功；失败时其余接口均为空操作（不影响主功能）
 */
esp_err_t audio_prompt_init(void);


/**
 * @brief 请求播放一条语音（非阻塞）
 * @note  同一条提示在 2 秒内重复请求会被忽略，避免倒挡检测之类的循环刷屏
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t audio_prompt_play(audio_prompt_id_t id);


/**
 * @brief 循环播放某条语音，直到 audio_prompt_stop_loop()（非阻塞）
 *
 * 播完一句自动续播下一句（句间留 LOOP_GAP_MS 停顿），适合"倒挡期间一直提醒"。
 * 同一条已经在循环时重复调用不会叠加。
 *
 * @return ESP_OK 已开始循环；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t audio_prompt_play_loop(audio_prompt_id_t id);


/**
 * @brief 停止循环播放
 *
 * 会把正在播的那句立刻收声（不等它播完），并丢弃队列里待播的语音。
 */
void audio_prompt_stop_loop(void);


#ifdef __cplusplus
}
#endif

#endif // AUDIO_PROMPT_H
