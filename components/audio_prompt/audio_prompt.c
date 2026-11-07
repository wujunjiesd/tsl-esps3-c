/**
 * @file audio_prompt.c
 * @brief ES8311 + I2S 语音提示播放
 *
 * 硬件（Waveshare ESP32-S3-Touch-LCD-3.49-V2）：
 *   I2S: MCLK=GPIO7 BCLK=GPIO15 LRCK=GPIO46 DOUT=GPIO45
 *   Codec: ES8311（I2C 0x18，挂在主 I2C 总线上）
 *
 * 语音资源：assets 目录下的 wav（16kHz / 单声道 / 16bit），编译期嵌入固件。
 * 播放：独立任务 + 队列，业务侧调用 audio_prompt_play() 立即返回。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

#include "audio_prompt.h"
#include "board.h"
#include "board_config.h"

static const char *TAG = "AUDIO";

// ============ 内嵌语音资源 ============
extern const uint8_t connected_wav_start[] asm("_binary_connected_wav_start");
extern const uint8_t connected_wav_end[]   asm("_binary_connected_wav_end");
extern const uint8_t reverse_wav_start[]   asm("_binary_reverse_wav_start");
extern const uint8_t reverse_wav_end[]     asm("_binary_reverse_wav_end");
extern const uint8_t vin_saved_wav_start[] asm("_binary_vin_saved_wav_start");
extern const uint8_t vin_saved_wav_end[]   asm("_binary_vin_saved_wav_end");
extern const uint8_t key_mode_wav_start[]  asm("_binary_key_mode_wav_start");
extern const uint8_t key_mode_wav_end[]    asm("_binary_key_mode_wav_end");

static const uint8_t *const s_wav_start[AUDIO_PROMPT_COUNT] = {
    [AUDIO_PROMPT_VIN_SAVED] = vin_saved_wav_start,
    [AUDIO_PROMPT_CONNECTED] = connected_wav_start,
    [AUDIO_PROMPT_REVERSE]   = reverse_wav_start,
    [AUDIO_PROMPT_KEY_MODE]  = key_mode_wav_start,
};

static const uint8_t *const s_wav_end[AUDIO_PROMPT_COUNT] = {
    [AUDIO_PROMPT_VIN_SAVED] = vin_saved_wav_end,
    [AUDIO_PROMPT_CONNECTED] = connected_wav_end,
    [AUDIO_PROMPT_REVERSE]   = reverse_wav_end,
    [AUDIO_PROMPT_KEY_MODE]  = key_mode_wav_end,
};

// ============ 音频句柄 ============
static i2s_chan_handle_t s_tx = NULL;
static esp_codec_dev_handle_t s_dev = NULL;
static const audio_codec_data_if_t *s_data_if = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if = NULL;
static const audio_codec_if_t *s_codec_if = NULL;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static bool s_ready = false;

static audio_prompt_id_t s_last_id = AUDIO_PROMPT_NONE;
static TickType_t s_last_tick = 0;

// 循环播放状态：由 audio_prompt_play_loop() 打开，stop_loop() 关闭。
// 播放任务每播完一句看一眼这里，决定要不要再排一句。
static volatile bool s_loop = false;
static volatile audio_prompt_id_t s_loop_id = AUDIO_PROMPT_NONE;

// 循环时两句之间的停顿。语音结尾本来有一段静音，
// 但完全不留间隔连着重播，听起来像卡碟
#define LOOP_GAP_MS 400

// 一次写入 512 帧，mono -> stereo 后 2KB，放内部 RAM 足够
#define FRAMES_PER_WRITE 512
static int16_t s_stereo[FRAMES_PER_WRITE * 2];

// ============ WAV 解析 ============

/** 在 RIFF 里找到 data 块（afconvert 生成的文件带 LIST 块，不能直接跳过 44 字节） */
static const uint8_t *wav_payload(const uint8_t *wav, size_t len, size_t *out_len)
{
    if (!wav || len < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return NULL;
    }

    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t sz = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8) |
                      ((uint32_t)wav[off + 6] << 16) | ((uint32_t)wav[off + 7] << 24);
        if (memcmp(wav + off, "data", 4) == 0) {
            size_t avail = len - off - 8;
            // 块长越界时按实际可用长度截断
            *out_len = ((size_t)sz < avail) ? (size_t)sz : avail;
            return wav + off + 8;
        }
        // 声称的块长超出文件剩余空间时不能再前进，否则 off 会回绕成死循环
        if (sz > len - off - 8) {
            break;
        }
        off += 8 + (size_t)sz + (sz & 1);
    }
    return NULL;
}

// ============ 播放 ============

/** 循环方式播放时，中途取消要能立刻收声，不必等这句播完 */
static void play_prompt(audio_prompt_id_t id, bool loop)
{
    if (!s_ready || id < 0 || id >= AUDIO_PROMPT_COUNT) {
        return;
    }

    size_t len = 0;
    const uint8_t *pcm = wav_payload(s_wav_start[id],
                                     (size_t)(s_wav_end[id] - s_wav_start[id]), &len);
    if (!pcm || len < sizeof(int16_t)) {
        ESP_LOGW(TAG, "语音 %d 解析失败", (int)id);
        return;
    }

    const int16_t *src = (const int16_t *)pcm;
    size_t frames = len / sizeof(int16_t);
    size_t done = 0;

    ESP_LOGD(TAG, "播放语音 %d（%u 帧）", (int)id, (unsigned)frames);

    while (done < frames) {
        if (loop && !s_loop) {
            return;
        }
        size_t n = frames - done;
        if (n > FRAMES_PER_WRITE) {
            n = FRAMES_PER_WRITE;
        }
        for (size_t i = 0; i < n; i++) {
            int16_t v = src[done + i];
            s_stereo[i * 2] = v;
            s_stereo[i * 2 + 1] = v;
        }
        esp_codec_dev_write(s_dev, s_stereo, (int)(n * 2 * sizeof(int16_t)));
        done += n;
    }
}

static void audio_task(void *arg)
{
    audio_prompt_id_t id;
    while (true) {
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 只有"当前正在循环的那条"才走续播逻辑，普通提示播完就结束
        bool loop = s_loop && (s_loop_id == id);
        play_prompt(id, loop);

        if (loop && s_loop && s_loop_id == id) {
            vTaskDelay(pdMS_TO_TICKS(LOOP_GAP_MS));
            if (s_loop && s_loop_id == id) {
                xQueueSend(s_queue, &id, portMAX_DELAY);
            }
        }
    }
}


esp_err_t audio_prompt_play(audio_prompt_id_t id)
{
    if (!s_ready || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id < 0 || id >= AUDIO_PROMPT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    // 同一句在 2 秒内重复请求直接忽略（倒挡检测是 100ms 轮询）
    TickType_t now = xTaskGetTickCount();
    if (id == s_last_id && (now - s_last_tick) < pdMS_TO_TICKS(2000)) {
        return ESP_OK;
    }
    s_last_id = id;
    s_last_tick = now;

    // 队列满说明还有语音在等，丢掉本次请求即可
    if (xQueueSend(s_queue, &id, 0) != pdTRUE) {
        ESP_LOGD(TAG, "语音队列已满，忽略 %d", (int)id);
    }
    return ESP_OK;
}


esp_err_t audio_prompt_play_loop(audio_prompt_id_t id)
{
    if (!s_ready || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id < 0 || id >= AUDIO_PROMPT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    // 同一条已经在循环就别叠加，否则队列里会堆一串重复的
    if (s_loop && s_loop_id == id) {
        return ESP_OK;
    }

    s_loop_id = id;
    s_loop = true;

    // 循环提示优先级最高：丢掉排队里的其它语音
    xQueueReset(s_queue);
    if (xQueueSend(s_queue, &id, 0) != pdTRUE) {
        ESP_LOGW(TAG, "循环语音入队失败");
    }
    return ESP_OK;
}


void audio_prompt_stop_loop(void)
{
    s_loop = false;
    s_loop_id = AUDIO_PROMPT_NONE;

    // 清掉待播的续播请求；正在播的那句由 play_prompt() 里的 loop 检查收声
    if (s_queue) {
        xQueueReset(s_queue);
    }
}


esp_err_t audio_prompt_init(void)
{
    esp_err_t ret;

    // ---- I2S 输出 ----
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 通道创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK,
            .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_LRCK,
            .dout = BOARD_AUDIO_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 标准模式初始化失败: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = i2s_channel_enable(s_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 使能失败: %s", esp_err_to_name(ret));
        goto fail;
    }

    // ---- 数据接口 ----
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "创建 I2S 数据接口失败");
        ret = ESP_FAIL;
        goto fail;
    }

    // ---- 控制接口（主 I2C 总线）----
    i2c_master_bus_handle_t bus = board_i2c_main();
    if (bus == NULL) {
        // 主 I2C 未就绪时绝不能把空句柄交给 codec 驱动，否则会 assert 重启
        ESP_LOGE(TAG, "主 I2C 总线不可用，跳过语音初始化");
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C0_PORT,
        .addr = BOARD_ES8311_I2C_ADDR,
        .bus_handle = bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_ctrl_if) {
        ESP_LOGE(TAG, "创建 I2C 控制接口失败");
        ret = ESP_FAIL;
        goto fail;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (!s_gpio_if) {
        ESP_LOGE(TAG, "创建 GPIO 接口失败");
        ret = ESP_FAIL;
        goto fail;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,          // 板上没有独立功放使能脚
        .pa_reverted = false,
        .master_mode = false,  // ESP32 提供 BCLK/LRCK/MCLK
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
            .pa_gain = 6.0,
        },
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (!s_codec_if) {
        ESP_LOGE(TAG, "ES8311 未响应（检查 I2C 0x%02X 与 codec 供电）",
                 BOARD_ES8311_I2C_ADDR);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "创建 codec 设备失败");
        ret = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_set_out_vol(s_dev, 75);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
    };
    ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec 打开失败: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_queue = xQueueCreate(4, sizeof(audio_prompt_id_t));
    if (!s_queue) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    if (xTaskCreate(audio_task, "audio_prompt", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "语音提示就绪（%d Hz，ES8311）", BOARD_AUDIO_SAMPLE_RATE);
    return ESP_OK;

fail:
    // 逆序回收所有已创建的资源：原来只删了 I2S 通道，codec / 接口 / 队列 / 任务
    // 全部悬挂，失败重试时会重复创建。
    if (s_dev) {
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_ready = false;
    return ret;
}
