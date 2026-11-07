/**
 * @file main.c
 * @brief Tesla BLE 仪表 - Waveshare ESP32-S3-Touch-LCD-3.49-V2
 *
 * 运行逻辑：
 *  1. 开机自检：板级初始化（电源自锁）、屏幕、按键
 *  2. 若 NVS 中没有 VIN -> 配网模式：开热点 + 网页设置 VIN
 *  3. 有 VIN -> 加载/生成密钥，蓝牙扫描并连接车辆
 *  4. 未配对时进入配对流程，配对成功后循环读取驾驶数据并刷新仪表
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "audio_prompt.h"
#include "board.h"
#include "boot_button.h"
#include "lcd_display.h"
#include "power_ctrl.h"
#include "tesla_ble.h"
#include "tesla_metrics.h"
#include "wifi_portal.h"

static const char *TAG = "TESLA_MAIN";

// ========== 时间参数 ==========
#define POLL_INTERVAL_MS      20      // 轮尾等待：只留够事件检查，让速度轮尽量密
// 慢速状态类别每几轮取一组。3 = 每 3 轮里有 2 轮只发 DRIVE 这一次往返，
// 速度刷新最快；其余 8 类降到约 2 秒一圈。想让慢数据更快就调回 1。
#define SLOW_STATES_EVERY     3
#define CONNECT_TIMEOUT_MS    30000   // 连接/服务发现超时
#define RECONNECT_DELAY_MS    5000    // 重连延迟
#define MAX_CONNECT_FAILURES  3       // 连续失败次数阈值（超过后重新配对）
// 等待用户在车机上确认配对。车机确认后车辆不会主动通知（官方 SDK 也不等待响应），
// 只能等这段时间结束再断开重连去验证，所以不宜过长，否则用户会觉得"确认了没反应"。
#define PAIRING_WAIT_SEC      20

// ========== 事件 ==========
static EventGroupHandle_t s_events = NULL;

#define EVT_CONNECTED       BIT0
#define EVT_AUTHENTICATED   BIT1
#define EVT_DISCONNECTED    BIT2
#define EVT_SVC_DISCOVERED  BIT3

static volatile bool s_paired = false;

// ========== 工具 ==========

static char gear_to_char(tesla_gear_state_t gear)
{
    switch (gear) {
    case TESLA_GEAR_P: return 'P';
    case TESLA_GEAR_R: return 'R';
    case TESLA_GEAR_N: return 'N';
    case TESLA_GEAR_D: return 'D';
    default: return '-';
    }
}

// VCSEC 车身状态 / TPMS 最近一次读取是否成功。
// 读取失败时结构体里的闭件值全是 0（= TESLA_CLOSURE_CLOSED），
// 不能直接当成"已关闭"显示，必须用这两个标志区分。
static bool s_body_ok = false;
// 仪表盘 6 格显示哪些指标（由 NVS 配置决定，load_ui_config 里填充）
static uint8_t s_dash_ids[APP_DASH_SLOT_COUNT];
// 长按 BOOT 请求进入 WiFi 配置界面（由轮询循环检测后切模式）
static volatile bool s_setup_request = false;
// 页面模式落盘任务（避免触摸任务里直接写 flash）
static TaskHandle_t s_ui_mode_save_task = NULL;
// 想要落盘的页面模式（只由切页回调写，落盘任务读）
static volatile uint8_t s_ui_mode_want = APP_UI_MODE_DASHBOARD;
// 切页落盘的静默窗口：最后一次切页之后这么久才真正写 flash。
// 连续滑动间隔通常 500~700ms，取 1s 能把"一直滑"的整段合成一次提交；
// 代价只是"滑完立刻断电"可能少记住最后一次页面，不影响功能。
#define UI_MODE_SAVE_QUIET_MS 1000

static void run_setup_mode(void);

// 当前是否正在循环播倒车提示。放在文件级是因为轮询退出时（进钥匙页 / 断连）
// 也要能复位它，否则下次回到倒挡会因为标志还挂着而不再起播。
static volatile bool s_reverse_alert = false;

/**
 * @brief 倒挡提示音
 *
 * 挂入倒挡立刻开始循环播放"倒车请注意"，只要还在倒挡就一直播；
 * 挂到别的档位（或车辆状态失效）马上停，下次挂倒挡重新开始。
 */
static void check_reverse_alert(const tesla_vehicle_state_t *st)
{
    // 档位不是 R、或这一帧数据不可信，都算"不在倒挡"
    if (!st->valid || st->drive.gear != TESLA_GEAR_R) {
        if (s_reverse_alert) {
            s_reverse_alert = false;
            ESP_LOGW(TAG, "离开倒挡，停止倒车提示");
            audio_prompt_stop_loop();
        }
        return;
    }

    if (!s_reverse_alert) {
        s_reverse_alert = true;
        ESP_LOGW(TAG, "挂入倒挡，循环播放倒车提示");
        audio_prompt_play_loop(AUDIO_PROMPT_REVERSE);
    }
}

static void update_display(const tesla_vehicle_state_t *st)
{
    check_reverse_alert(st);

    lcd_drive_info_t info = {0};

    info.valid = st->valid;
    // 车辆上报的速度单位是 mph，界面显示 km/h
    int kmh = (int)(st->drive.speed_kmh * 1.60934f + 0.5f);
    info.speed_kmh = kmh;
    info.power_kw = (int)st->drive.power_kw;
    info.gear = gear_to_char(st->drive.gear);
    info.odometer_km = (int)st->drive.odometer_km;
    info.eta_min = st->drive.minutes_to_arrival > 0
                       ? (int)st->drive.minutes_to_arrival
                       : -1;
    strncpy(info.destination, st->drive.destination, sizeof(info.destination) - 1);

    // ---- 闭件状态 ----
    info.body_valid = s_body_ok;
    if (s_body_ok) {
        const tesla_closure_status_t *c = &st->body.closures;
        info.doors_closed = (c->front_driver_door == TESLA_CLOSURE_CLOSED) &&
                            (c->front_passenger_door == TESLA_CLOSURE_CLOSED) &&
                            (c->rear_driver_door == TESLA_CLOSURE_CLOSED) &&
                            (c->rear_passenger_door == TESLA_CLOSURE_CLOSED);
        info.frunk_closed = (c->front_trunk == TESLA_CLOSURE_CLOSED);
        info.trunk_closed = (c->rear_trunk == TESLA_CLOSURE_CLOSED);
        info.charge_port_closed = (c->charge_port == TESLA_CLOSURE_CLOSED);
    }

    // ---- 胎压（bar）----
    // 胎压（bar）：必须四轮都有像样的读数才算有效。
    // 原来只用左前轮做判据，个别车轮 TPMS 没上报时会把 0 当成有效值画出来
    info.tires_valid = (st->present_mask & TESLA_STATE_BIT_TIRE) &&
                       st->tires.front_left_bar > 0.1f &&
                       st->tires.front_right_bar > 0.1f &&
                       st->tires.rear_left_bar > 0.1f &&
                       st->tires.rear_right_bar > 0.1f;
    if (info.tires_valid) {
        info.tire_fl = st->tires.front_left_bar;
        info.tire_fr = st->tires.front_right_bar;
        info.tire_rl = st->tires.rear_left_bar;
        info.tire_rr = st->tires.rear_right_bar;
    }

    // ---- 异常告警汇总 ----
    // 协议没有"车辆告警列表"这类字段，只能由已读到的状态派生：
    //   闭件未关（车门/前备箱/后备箱/充电口） + 轮胎硬告警位（TirePressureState.tpms_hard_warning_*）
    info.alert_valid = info.body_valid || info.tires_valid;
    if (info.alert_valid) {
        bool alert = info.body_valid &&
                     (!info.doors_closed || !info.frunk_closed || !info.trunk_closed ||
                      !info.charge_port_closed);
        if (st->present_mask & TESLA_STATE_BIT_TIRE) {
            const tesla_tire_pressure_t *t = &st->tires;
            alert = alert || t->warning_fl || t->warning_fr || t->warning_rl || t->warning_rr;
        }
        info.alert = alert;
    }

    // 百公里能耗 = 功率 / 车速 * 100
    if (kmh > 5) {
        int cons = info.power_kw * 1000 / kmh;
        if (cons < 0) cons = 0;
        if (cons > 999) cons = 999;
        info.consumption_10 = cons;
    } else {
        info.consumption_10 = -1;
    }

    lcd_display_set_drive(&info);

    // ---- 6 个格子：按用户配置的指标渲染 ----
    const char *names[LCD_CELL_COUNT];
    const char *values[LCD_CELL_COUNT];
    uint8_t tones[LCD_CELL_COUNT];
    tesla_metric_value_t mv[LCD_CELL_COUNT];

    for (int i = 0; i < LCD_CELL_COUNT; i++) {
        tesla_metric_t id = (tesla_metric_t)s_dash_ids[i];
        tesla_metric_format(id, st, &mv[i]);
        names[i] = tesla_metric_name(id);
        values[i] = mv[i].text;
        tones[i] = mv[i].tone;
    }
    lcd_display_set_cells(names, values, tones);
}

static void print_state(const tesla_vehicle_state_t *st)
{
    const char *gear_names[] = {"-", "-", "P", "R", "N", "D", "SNA"};
    int g = st->drive.gear;
    if (g < 0 || g > 6) g = 0;

    ESP_LOGI(TAG, "档位=%s 速度=%.0f mph (%.0f km/h) 功率=%" PRId32 " kW 里程=%.0f km",
             gear_names[g], (double)st->drive.speed_kmh,
             (double)(st->drive.speed_kmh * 1.60934f),
             st->drive.power_kw, (double)st->drive.odometer_km);

    if (st->drive.destination[0]) {
        ESP_LOGI(TAG, "导航: %s 剩余 %.0f km / %.0f 分钟", st->drive.destination,
                 (double)st->drive.km_to_arrival, (double)st->drive.minutes_to_arrival);
    }

    // 软件更新诊断：version 是 OTA 待装版本，车机平时不上报。把留存的最终值
    // 和 OTA 状态一起打出来，便于确认"软件更新"格子为什么显示"无更新"。
    ESP_LOGI(TAG, "软件: 版本='%s' 状态=%ld 下载=%lu%% 安装=%lu%% (软状态位=%d)",
             st->software.version, (long)st->software.status,
             (unsigned long)st->software.download_percent,
             (unsigned long)st->software.install_percent,
             (st->present_mask & TESLA_STATE_BIT_SOFTWARE) ? 1 : 0);
}

// ========== 状态回调 ==========

static void on_state_change(tesla_conn_state_t state, void *arg)
{
    switch (state) {
    case TESLA_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "连接断开");
        xEventGroupSetBits(s_events, EVT_DISCONNECTED);
        xEventGroupClearBits(s_events, EVT_CONNECTED | EVT_AUTHENTICATED);
        lcd_display_set_link(LCD_LINK_DISCONNECTED);
        break;

    case TESLA_STATE_SCANNING:
        ESP_LOGI(TAG, "扫描车辆...");
        lcd_display_set_link(LCD_LINK_SCANNING);
        break;

    case TESLA_STATE_CONNECTING:
        ESP_LOGI(TAG, "正在连接...");
        lcd_display_set_link(LCD_LINK_CONNECTING);
        break;

    case TESLA_STATE_CONNECTED:
        ESP_LOGI(TAG, "已连接");
        xEventGroupSetBits(s_events, EVT_CONNECTED);
        lcd_display_set_link(LCD_LINK_CONNECTED);
        break;

    case TESLA_STATE_SERVICE_DISCOVERED:
        ESP_LOGI(TAG, "服务发现完成");
        xEventGroupSetBits(s_events, EVT_SVC_DISCOVERED);
        break;

    case TESLA_STATE_AUTHENTICATED:
        ESP_LOGI(TAG, "认证成功");
        xEventGroupSetBits(s_events, EVT_AUTHENTICATED);
        lcd_display_set_link(LCD_LINK_AUTHENTICATED);
        // 真正可以下发命令了，播报“已连接车辆”
        // 60 秒节流：连接不稳反复重连时不会一直重复播报
        {
            static TickType_t last_prompt = 0;
            TickType_t now = xTaskGetTickCount();
            if (last_prompt == 0 || (now - last_prompt) > pdMS_TO_TICKS(60000)) {
                audio_prompt_play(AUDIO_PROMPT_CONNECTED);
                last_prompt = now;
            }
        }
        break;

    default:
        break;
    }
}

// ========== 连接流程 ==========

/** 可被长按打断的延时：车不在旁边时不用干等整个重连间隔 */
static void delay_or_setup(uint32_t ms)
{
    uint32_t waited = 0;
    while (waited < ms && !s_setup_request) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
}

/**
 * @brief 分片等待事件位，长按 BOOT 可提前打断
 *
 * 一次 xEventGroupWaitBits 等满 CONNECT_TIMEOUT_MS 的话，车不在旁边时按下
 * BOOT 要等几十秒才有反应。这里切成 200ms 一片，中途检查是否请求进配置模式。
 *
 * fail_mask 里的位一旦置起就立刻判失败：连接过程中对端断开时，
 * 只把它放进 wait_bits 不判的话，会一直空等到整个超时才重连。
 *
 * @return true 拿到了 need 指定的位；false 超时、被长按打断或出现 fail 位
 */
static bool wait_bits_or_setup(uint32_t wait_bits, uint32_t need, uint32_t fail_mask,
                               uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (s_setup_request) {
            return false;
        }
        uint32_t slice = (timeout_ms - waited) < 200 ? (timeout_ms - waited) : 200;
        EventBits_t bits = xEventGroupWaitBits(s_events, wait_bits | fail_mask,
                                              pdFALSE, pdFALSE, pdMS_TO_TICKS(slice));
        if (bits & fail_mask) {
            return false;
        }
        if (bits & need) {
            return true;
        }
        waited += slice;
    }
    return false;
}

static esp_err_t wait_connected_and_ready(void)
{
    if (!wait_bits_or_setup(EVT_CONNECTED, EVT_CONNECTED, EVT_DISCONNECTED,
                            CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "连接超时或已断开");
        tesla_ble_scan_stop();
        return ESP_ERR_TIMEOUT;
    }

    if (!wait_bits_or_setup(EVT_SVC_DISCOVERED, EVT_SVC_DISCOVERED, EVT_DISCONNECTED,
                            CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "服务发现超时或已断开");
        tesla_ble_disconnect();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t connect_and_start_session(const char *vin)
{
    xEventGroupClearBits(s_events, EVT_CONNECTED | EVT_AUTHENTICATED |
                                       EVT_DISCONNECTED | EVT_SVC_DISCOVERED);

    // 进入扫描阶段：清掉上一次流程残留的提示，界面回到“扫描中”
    lcd_display_set_hint(NULL);

    esp_err_t ret = tesla_ble_scan(vin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动扫描失败: %d", ret);
        return ret;
    }

    ret = wait_connected_and_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "建立认证会话...");
    ret = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "会话建立失败: %d", ret);
        tesla_ble_disconnect();
        return ret;
    }
    return ESP_OK;
}

static esp_err_t do_pairing(const char *vin)
{
    xEventGroupClearBits(s_events, EVT_CONNECTED | EVT_AUTHENTICATED |
                                       EVT_DISCONNECTED | EVT_SVC_DISCOVERED);

    // 每次进入配对流程都先回到“扫描中”：否则上一次配对留下的 PAIRING 状态会一直
    // 挂在屏上（等待页文案是由 link 状态推导的），看起来就像“确认了还在要求刷卡”。
    lcd_display_set_link(LCD_LINK_SCANNING);
    lcd_display_set_hint(NULL);

    esp_err_t ret = tesla_ble_scan(vin);
    if (ret != ESP_OK) {
        lcd_display_set_link(LCD_LINK_DISCONNECTED);
        return ret;
    }

    ret = wait_connected_and_ready();
    if (ret != ESP_OK) {
        lcd_display_set_link(LCD_LINK_DISCONNECTED);
        return ret;
    }

    // 车机会弹出“添加钥匙”的确认框，这才是需要用户去中控台刷卡的时机
    lcd_display_set_link(LCD_LINK_PAIRING);
    lcd_display_set_hint("请在车机上确认并刷卡");

    ESP_LOGW(TAG, "请在中控屏点击确认，并将 NFC 卡放在中控台");
    ret = tesla_ble_add_key_request();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配对请求发送失败: %d", ret);
        tesla_ble_disconnect();
        lcd_display_set_hint(NULL);
        lcd_display_set_link(LCD_LINK_DISCONNECTED);
        return ret;
    }

    // 车辆确认后不会回包，这里倒数等待；一旦车辆断开连接就立刻结束去重连验证
    for (int i = 0; i < PAIRING_WAIT_SEC; i++) {
        // 长按 BOOT 要能随时打断配对等待，否则这 20 秒里按键毫无反应
        if (s_setup_request) {
            ESP_LOGW(TAG, "长按进入配置模式，放弃配对等待");
            tesla_ble_disconnect();
            lcd_display_set_hint(NULL);
            lcd_display_set_link(LCD_LINK_DISCONNECTED);
            return ESP_ERR_TIMEOUT;
        }

        EventBits_t bits = xEventGroupWaitBits(s_events, EVT_DISCONNECTED, pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(1000));
        if (bits & EVT_DISCONNECTED) {
            ESP_LOGI(TAG, "车辆已断开，提前结束配对等待");
            break;
        }
        if ((i + 1) % 5 == 0) {
            char msg[40];
            snprintf(msg, sizeof(msg), "已发送，等待车机确认 %ds", PAIRING_WAIT_SEC - (i + 1));
            lcd_display_set_hint(msg);
            ESP_LOGI(TAG, "等待车机确认... %d/%d 秒", i + 1, PAIRING_WAIT_SEC);
        }
    }

    tesla_ble_disconnect();
    lcd_display_set_hint(NULL);
    lcd_display_set_link(LCD_LINK_DISCONNECTED);
    return ESP_OK;
}

static void status_loop(void)
{
    ESP_LOGI(TAG, "开始轮询驾驶状态");
    lcd_display_set_hint(NULL);

    int tick = 0;
    int slow = 0;
    while (tesla_ble_get_state() == TESLA_STATE_AUTHENTICATED) {
        // 长按 BOOT：先退出轮询，由主任务切到配置模式
        if (s_setup_request) {
            ESP_LOGW(TAG, "进入 WiFi 配置模式，暂停数据轮询");
            break;
        }

        // 车钥匙页下暂停数据轮询：既省电，也避免和按键命令抢占同一条 BLE 链路
        if (lcd_display_get_mode() == LCD_MODE_KEY) {
            EventBits_t key_bits = xEventGroupWaitBits(s_events, EVT_DISCONNECTED, pdTRUE,
                                                       pdFALSE, pdMS_TO_TICKS(300));
            if (key_bits & EVT_DISCONNECTED) {
                break;
            }
            continue;
        }

        // 车机状态分「快/慢」两条通道。原先 9 类按 3 组轮转，DriveState 只在
        // 第 0 组里，每 3 轮（约 1.2 秒）才更新一次速度，肉眼明显滞后。
        // 快通道：DRIVE 每轮单独读一次，拿到立刻刷屏；慢通道：其余 8 类按
        // SLOW_STATES_EVERY 降频后再 3 组轮转，2/3 的轮次只有一次 BLE 往返。
        // 逐类读取模式下一次 refresh 就是一次往返，所以快通道必须单独调用，
        // 不能和慢组并进同一个 mask —— 否则又要等慢组全部返回才刷屏。
        esp_err_t ret = tesla_ble_refresh_states(TESLA_STATE_BIT_DRIVE);
        if (ret == ESP_OK) {
            tesla_vehicle_state_t state;
            if (tesla_ble_get_vehicle_state(&state) == ESP_OK) {
                update_display(&state);
                if (++tick >= 50) {
                    tick = 0;
                    print_state(&state);
                }
            }
        } else {
            ESP_LOGW(TAG, "读取车速失败: %s", esp_err_to_name(ret));
        }

        uint32_t mask = 0;
        if (slow % SLOW_STATES_EVERY == 0) {
            switch ((slow / SLOW_STATES_EVERY) % 3) {
            case 0:
                mask = TESLA_STATE_BIT_CLIMATE | TESLA_STATE_BIT_LOCATION;
                break;
            case 1:
                mask = TESLA_STATE_BIT_CHARGE | TESLA_STATE_BIT_CLOSURES;
                break;
            default:
                mask = TESLA_STATE_BIT_TIRE | TESLA_STATE_BIT_MEDIA |
                       TESLA_STATE_BIT_MEDIA_DETAIL | TESLA_STATE_BIT_SOFTWARE;
                break;
            }
        }
        if (mask) {
            ret = tesla_ble_refresh_states(mask);
            if (ret == ESP_OK) {
                tesla_vehicle_state_t state;
                if (tesla_ble_get_vehicle_state(&state) == ESP_OK) {
                    update_display(&state);
                }
            } else {
                ESP_LOGW(TAG, "读取车辆状态失败: %s", esp_err_to_name(ret));
            }
        }

        // 车身闭件（车门/前后备箱/充电口）：VCSEC 通道，车机休眠也能读。
        // 6 是 SLOW_STATES_EVERY(3) 的倍数，保证它只落在"本来就要读慢组"的轮次，
        // 不再单独占用一个纯速度轮；超时也缩短到 600ms。
        if (slow % 6 == 0) {
            s_body_ok = (tesla_ble_get_body_state() == ESP_OK);
        }

        slow++;

        EventBits_t bits = xEventGroupWaitBits(s_events, EVT_DISCONNECTED, pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(POLL_INTERVAL_MS));
        if (bits & EVT_DISCONNECTED) {
            break;
        }
    }

    // 退出轮询后档位不会再刷新，倒挡循环就失去了停下来的依据
    //（进钥匙页、长按进配网、BLE 断连三个出口都从这里走），统一收尾
    if (s_reverse_alert) {
        s_reverse_alert = false;
        ESP_LOGW(TAG, "轮询结束，停止倒车提示");
        audio_prompt_stop_loop();
    }
}

// ========== 主业务任务 ==========

static void tesla_task(void *arg)
{
    char vin[18] = {0};
    if (app_settings_get_vin(vin) != ESP_OK) {
        ESP_LOGE(TAG, "VIN 缺失，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "VIN: %s", vin);
    char local_name[20] = {0};
    if (tesla_ble_get_local_name(vin, local_name) == ESP_OK) {
        ESP_LOGI(TAG, "车辆 BLE 名称: %s", local_name);
    }

    bool paired = s_paired;
    int failures = 0;

    while (true) {
        // 长按 BOOT：任何阶段都要能立刻响应。原先只在"连接成功且轮询结束"之后判断，
        // 于是车不在旁边时连接一直失败、根本走不到那里，按键看上去毫无反应。
        if (s_setup_request) {
            break;
        }

        if (!paired) {
            ESP_LOGW(TAG, "未配对，开始配对流程");
            if (do_pairing(vin) == ESP_OK) {
                app_settings_set_paired(true);
                s_paired = true;
                paired = true;
                lcd_display_set_paired(true);
                lcd_display_set_link(LCD_LINK_DISCONNECTED);
            } else {
                delay_or_setup(RECONNECT_DELAY_MS);
                continue;
            }
        }

        ESP_LOGI(TAG, "连接车辆...");
        if (connect_and_start_session(vin) != ESP_OK) {
            failures++;
            ESP_LOGW(TAG, "连接失败 (%d/%d)", failures, MAX_CONNECT_FAILURES);
            if (failures >= MAX_CONNECT_FAILURES) {
                ESP_LOGW(TAG, "连续失败 %d 次，重新配对", failures);
                app_settings_set_paired(false);
                s_paired = false;
                paired = false;
                lcd_display_set_paired(false);
                failures = 0;
            }
            delay_or_setup(RECONNECT_DELAY_MS);
            continue;
        }

        failures = 0;
        status_loop();

        lcd_display_set_link(LCD_LINK_DISCONNECTED);
        delay_or_setup(RECONNECT_DELAY_MS);
    }

    // 长按 BOOT 进来的配置模式：断开车辆、开热点，保存后自动重启（不会返回）
    ESP_LOGW(TAG, "切换到 WiFi 配置模式");
    tesla_ble_disconnect();
    run_setup_mode();
    vTaskDelete(NULL);
}

// ========== 触摸交互：双击切换模式 / 车钥匙按钮 ==========

// 用户配置：车钥匙 4 个按钮是哪几条命令（s_dash_ids 定义在文件前部）
static tesla_command_t s_key_cmds[APP_KEY_SLOT_COUNT];

// 首次使用（NVS 里没有保存过槽位配置）时的默认布局
static const uint8_t k_default_dash[APP_DASH_SLOT_COUNT] = {
    TESLA_METRIC_POWER,       // 功率
    TESLA_METRIC_INSIDE_TEMP, // 车内温度
    TESLA_METRIC_DOORS,       // 车门汇总
    TESLA_METRIC_TP_HEALTH,   // 胎压状态（最低胎压 < 2.6 bar 判不健康）
    TESLA_METRIC_RANGE,       // 额定续航
    TESLA_METRIC_CHARGE_PORT, // 充电口
};

static const uint8_t k_default_keys[APP_KEY_SLOT_COUNT] = {
    TESLA_CMD_LOCK_TOGGLE,   // 车锁开关
    TESLA_CMD_FRUNK_TOGGLE,  // 前备箱开关
    TESLA_CMD_HONK_HORN,     // 鸣笛
    TESLA_CMD_CLIMATE_TOGGLE,// 空调
};

/** 命令中文名（从命令表里查，配置页与按钮标签共用） */
static const char *command_name(tesla_command_t cmd)
{
    int count = 0;
    const tesla_command_info_t *table = tesla_command_table(&count);
    for (int i = 0; i < count; i++) {
        if (table[i].cmd == cmd) {
            return table[i].name;
        }
    }
    return "未知";
}

/** 命令 -> 按钮图标 */
static lcd_icon_t command_icon(tesla_command_t cmd)
{
    switch (cmd) {
    case TESLA_CMD_LOCK_TOGGLE: return LCD_ICON_LOCK;
    case TESLA_CMD_TRUNK_TOGGLE: return LCD_ICON_TRUNK;
    case TESLA_CMD_FRUNK_TOGGLE: return LCD_ICON_FRUNK;
    case TESLA_CMD_CLIMATE_TOGGLE:
    case TESLA_CMD_TEMP_UP:
    case TESLA_CMD_TEMP_DOWN:
    case TESLA_CMD_SEAT_HEATER_TOGGLE:
    case TESLA_CMD_STEERING_HEAT_TOGGLE:
    case TESLA_CMD_BIOWEAPON_TOGGLE:
    case TESLA_CMD_COP_TOGGLE:
    case TESLA_CMD_PRECOND_MAX_TOGGLE:
        return LCD_ICON_CLIMATE;
    case TESLA_CMD_FLASH_LIGHTS:
    case TESLA_CMD_HONK_HORN:
        return LCD_ICON_LIGHT;
    case TESLA_CMD_CHARGE_TOGGLE:
    case TESLA_CMD_CHARGE_PORT_TOGGLE:
    case TESLA_CMD_CHARGE_AMPS_UP:
    case TESLA_CMD_CHARGE_AMPS_DOWN:
    case TESLA_CMD_CHARGE_LIMIT_UP:
    case TESLA_CMD_CHARGE_LIMIT_DOWN:
        return LCD_ICON_CHARGE;
    case TESLA_CMD_MEDIA_PLAY_PAUSE:
    case TESLA_CMD_MEDIA_NEXT:
    case TESLA_CMD_MEDIA_PREV:
    case TESLA_CMD_VOLUME_UP:
    case TESLA_CMD_VOLUME_DOWN:
        return LCD_ICON_MEDIA;
    default:
        return LCD_ICON_GENERIC;
    }
}

/**
 * @brief 从 NVS 读界面配置并推给显示层
 *
 * 存的是指标/命令的枚举数值，读回来必须做范围校验：
 * 固件升级后枚举可能变长，旧值落在表外会让 UI 显示成空白。
 */
static void load_ui_config(void)
{
    // 固件改过默认布局（APP_LAYOUT_VER）之后，让老设备也套用一次新默认值，
    // 并立刻把版本号落盘——之后用户自己在配网页改的配置不会被再覆盖。
    uint8_t saved_ver = 0;
    bool use_default = (app_settings_get_layout_ver(&saved_ver) != ESP_OK) ||
                       (saved_ver != APP_LAYOUT_VER);
    if (use_default) {
        ESP_LOGW(TAG, "默认布局版本 %u -> %u，套用新的默认仪表盘/按钮配置",
                 (unsigned)saved_ver, (unsigned)APP_LAYOUT_VER);
        app_settings_set_layout_ver(APP_LAYOUT_VER);
    }

    uint8_t dash[APP_DASH_SLOT_COUNT];
    if (use_default || app_settings_get_dash_slots(dash) != ESP_OK) {
        memcpy(dash, k_default_dash, sizeof(dash));
        app_settings_set_dash_slots(dash);
    }
    for (int i = 0; i < APP_DASH_SLOT_COUNT; i++) {
        if (dash[i] >= TESLA_METRIC_MAX) {
            dash[i] = k_default_dash[i];
        }
        s_dash_ids[i] = dash[i];
    }

    uint8_t keys[APP_KEY_SLOT_COUNT];
    if (use_default || app_settings_get_key_slots(keys) != ESP_OK) {
        memcpy(keys, k_default_keys, sizeof(keys));
        app_settings_set_key_slots(keys);
    }
    const char *labels[APP_KEY_SLOT_COUNT];
    uint8_t icons[APP_KEY_SLOT_COUNT];
    for (int i = 0; i < APP_KEY_SLOT_COUNT; i++) {
        if (keys[i] == 0 || keys[i] >= TESLA_CMD_MAX) {
            keys[i] = k_default_keys[i];
        }
        s_key_cmds[i] = (tesla_command_t)keys[i];
        labels[i] = command_name(s_key_cmds[i]);
        icons[i] = (uint8_t)command_icon(s_key_cmds[i]);
    }
    lcd_display_set_key_buttons(icons, labels);

    ESP_LOGI(TAG, "界面配置: 仪表盘 %u/%u/%u/%u/%u/%u, 按钮 %d/%d/%d/%d",
             s_dash_ids[0], s_dash_ids[1], s_dash_ids[2],
             s_dash_ids[3], s_dash_ids[4], s_dash_ids[5],
             s_key_cmds[0], s_key_cmds[1], s_key_cmds[2], s_key_cmds[3]);
}

/** 执行车钥匙页第 slot 个按钮 */
static void handle_key_slot(int slot)
{
    if (slot < 0 || slot >= APP_KEY_SLOT_COUNT) {
        return;
    }

    tesla_command_t cmd = s_key_cmds[slot];
    const char *name = command_name(cmd);
    esp_err_t ret = tesla_ble_execute(cmd);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s 指令已发送", name);
    } else {
        ESP_LOGE(TAG, "%s 指令失败: %s", name, esp_err_to_name(ret));
    }

    // 屏幕反馈由显示层自己做（按下的按钮下沉 + 提亮），这里只留日志
}

/**
 * @brief 页面模式落盘任务
 *
 * 触摸任务运行在优先级 6，直接在这里写 NVS 会让 flash 擦写期间
 * 关掉 cache 数十毫秒，10ms 的触摸轮询直接停摆（点不动、滑动判不出来）。
 * 这里把这些写操作挪到一个低优先级任务里串行处理。
 */
static void ui_mode_save_task(void *arg)
{
    for (;;) {
        // 等一次"切页"通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 连滑几下会连着切好几次页。每次落盘都是一次 NVS 提交（擦写期间关 cache
        // 几十毫秒，触摸/BLE 全停摆），而且同一把 NVS 里每写一次就多占一个扇区，
        // 滑快了会很快堆到垃圾回收。所以等手停下来再写，且只写最后那一个值。
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_MODE_SAVE_QUIET_MS)) != 0) {
            /* 又切页了：继续等静默 */
        }
        app_settings_set_ui_mode(s_ui_mode_want);
    }
}

/**
 * @brief 模式变化回调：双击或左右滑动切页面后记住当前模式
 *
 * 由触摸任务直接调用，不依赖 ui_action_task 是否创建成功。
 */
static void on_ui_mode_change(lcd_ui_mode_t mode)
{
    // 配网引导页里的手势不算"用户选了某个页面"，别把它记下来
    if (s_setup_request) {
        return;
    }
    s_ui_mode_want = (uint8_t)mode;
    if (s_ui_mode_save_task) {
        xTaskNotifyGive(s_ui_mode_save_task);   // 具体写什么由 s_ui_mode_want 决定
    } else {
        app_settings_set_ui_mode((uint8_t)mode);
    }
}

/** 网页上保存界面配置后立刻重新套用，屏幕不用等重启才变 */
static void on_ui_config_saved(void)
{
    load_ui_config();
}

/** 开机恢复上次停留的页面（没保存过就用默认的仪表盘） */
static void restore_ui_mode(void)
{
    uint8_t mode = APP_UI_MODE_DASHBOARD;
    // NVS 里可能是旧版本写入的越界值，直接转成枚举会落到未定义分支
    if (app_settings_get_ui_mode(&mode) == ESP_OK) {
        if (mode == APP_UI_MODE_KEY || mode == APP_UI_MODE_DASHBOARD) {
            lcd_display_set_mode((lcd_ui_mode_t)mode);
            ESP_LOGI(TAG, "恢复上次页面: %s", mode == APP_UI_MODE_KEY ? "车钥匙" : "仪表盘");
        } else {
            ESP_LOGW(TAG, "NVS 里的页面值非法(%u)，用默认仪表盘", (unsigned)mode);
            lcd_display_set_mode(LCD_MODE_DASHBOARD);
        }
    }
}

// 点击合并窗口：触摸恢复/假触点会在几十毫秒内连投多条点击，同一窗口里的多条
// 只保留最后一条，避免变成一串真实下发的车锁/闭件指令（日志里 100ms 内三条
// "指令已发送"）。窗口不能太长，否则用户"再点一次关闭"的合法第二击会被吞掉。
#define KEY_CMD_MERGE_MS 300

static void ui_action_task(void *arg)
{
    // 初值往前推一个窗口，保证开机后第一次点击立即生效（TickType_t 无符号回绕安全）
    TickType_t last_key_cmd = xTaskGetTickCount() - pdMS_TO_TICKS(KEY_CMD_MERGE_MS);
    int pending_slot = -1;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        // 模式切换已由触摸任务完成（不依赖本任务是否创建成功），这里只补语音播报
        if (lcd_display_take_double_tap() > 0 &&
            lcd_display_get_mode() == LCD_MODE_KEY) {
            ESP_LOGI(TAG, "双击进入车钥匙模式");
            audio_prompt_play(AUDIO_PROMPT_KEY_MODE);
        }

        // 一次扫描里到达的多条点击合并成最后一条：handle_key_slot 会阻塞最长 1 秒
        // （车身状态读取超时），期间积压的假触点不会在之后被集中下发。
        int slot;
        while ((slot = lcd_display_take_key_slot()) >= 0) {
            pending_slot = slot;
        }
        if (pending_slot < 0) {
            continue;
        }

        // 合并窗口内先攒着而不是丢弃，窗口过了照常执行 —— 这样"再点一次关闭"
        // 不会被冷却吞掉，十几毫秒内的假触点连击又只算一次。
        TickType_t now = xTaskGetTickCount();
        if ((now - last_key_cmd) < pdMS_TO_TICKS(KEY_CMD_MERGE_MS)) {
            continue;
        }

        int to_run = pending_slot;
        pending_slot = -1;
        handle_key_slot(to_run);
        // 从指令"下发完成"算起，覆盖 handle_key_slot 阻塞期间的噪声
        last_key_cmd = xTaskGetTickCount();
    }
}

// ========== 配网模式 ==========

static void run_setup_mode(void)
{
    char ssid[33] = {0};
    app_settings_get_ap_name(ssid, sizeof(ssid));

    ESP_LOGW(TAG, "============ 配置模式 ============");
    ESP_LOGW(TAG, "热点: %s (无密码)", ssid);
    ESP_LOGW(TAG, "请访问 http://%s/ 设置 VIN / 配置界面", WIFI_PORTAL_URL);

    // 已配过网的设备（长按 BOOT 进来）要保留屏上的 VIN，别清成空
    if (!app_settings_has_vin()) {
        lcd_display_set_vin(NULL);
        lcd_display_set_paired(false);
    }
    lcd_display_set_link(LCD_LINK_DISCONNECTED);
    lcd_display_set_setup_mode(true, ssid);

    // 热点起不来就没有任何渠道能设置 VIN，原来的写法是照常进下面的死循环——
    // 界面停在"扫描中"，用户永远等不到配置页。重试几次后重启回到常规流程。
    esp_err_t err = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        err = wifi_portal_start(ssid);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "配网服务启动失败(%d/%d): %s", retry + 1, 3, esp_err_to_name(err));
        lcd_display_set_hint("热点启动失败，重试中");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (err != ESP_OK) {
        lcd_display_set_hint("热点不可用，重启中");
        ESP_LOGE(TAG, "热点始终无法启动，3 秒后重启");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (wifi_portal_take_saved_flag()) {
            lcd_display_set_hint("已保存，重启中");
            // 语音播报“设置成功，正在重启”；重启由 wifi_portal 延时触发，留出了播放时间
            audio_prompt_play(AUDIO_PROMPT_VIN_SAVED);
            ESP_LOGW(TAG, "VIN 已保存，播报提示后重启");
        }
    }
}

/** 长按 BOOT 5 秒：只置标志，由 tesla_task 在安全点切到配置模式 */
static void on_boot_long_press(void)
{
    s_setup_request = true;
    // 息屏时也要能看到反馈
    lcd_display_set_backlight(true);
    lcd_display_set_hint("进入配置模式...");
    lcd_display_refresh();
}

// ========== 私钥准备 ==========

static esp_err_t prepare_key(void)
{
    uint8_t key[32];

    if (app_settings_load_key(key) == ESP_OK) {
        ESP_LOGI(TAG, "使用已保存的私钥");
        return tesla_ble_load_key(key);
    }

    ESP_LOGI(TAG, "生成新的密钥对");
    esp_err_t ret = tesla_ble_load_key(NULL);
    if (ret == ESP_OK && tesla_ble_get_private_key(key) == ESP_OK) {
        app_settings_save_key(key);
    }
    return ret;
}

// ========== 入口 ==========

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Tesla BLE 仪表 - ESP32-S3-Touch-LCD-3.49-V2");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 尽早建立电源自锁，避免松开电源键后断电
    ret = board_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "板级初始化失败: %s", esp_err_to_name(ret));
    }

    app_settings_init();

    ret = lcd_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败: %s", esp_err_to_name(ret));
    }

    // 切页面（双击 / 左右滑动）时落盘，开机时由 restore_ui_mode() 恢复。
    // 栈必须给足 6KB：这个任务整条路是 nvs_open -> nvs_set_u8 -> nvs_commit，
    // 而 commit 在页写满时会走垃圾回收（逐页扫描 + 页内缓冲），栈开销远超 2KB。
    // 实测 2048 时滑十几次就踩爆：
    //   ***ERROR*** A stack overflow in task ui_mode_save has been detected.
    //   rst:0xc (RTC_SW_CPU_RST)
    if (xTaskCreate(ui_mode_save_task, "ui_mode_save", 6144, NULL, 1,
                    &s_ui_mode_save_task) != pdPASS) {
        ESP_LOGW(TAG, "页面模式落盘任务创建失败，退化为直接写 NVS");
        s_ui_mode_save_task = NULL;
    }
    lcd_display_set_mode_change_cb(on_ui_mode_change);

    // 配网页保存 6 格 / 4 按钮配置后立刻生效（重启后仍从 NVS 恢复）
    wifi_portal_set_config_saved_cb(on_ui_config_saved);

    // 电源键与复位键（长按 BOOT 5 秒进 WiFi 配置界面）
    power_ctrl_init();
    boot_button_init(on_boot_long_press);

    s_events = xEventGroupCreate();
    if (!s_events) {
        ESP_LOGE(TAG, "事件组创建失败");
        return;
    }

    // 界面布局配置（6 个格子 + 4 个按钮）必须在任何分支之前推给显示层并落盘。
    // 以前放在"有 VIN"分支之后：没配对的设备走配网模式会直接 return，
    // 默认布局永远写不进 NVS —— 配网页读回来全是 0，下拉框一片"无"。
    load_ui_config();

    // 没有 VIN -> 配网模式（保存后自动重启，不会返回）
    char vin[18] = {0};
    if (app_settings_get_vin(vin) != ESP_OK) {
        // 配网页要播“设置成功，正在重启”，这里单独初始化一次
        audio_prompt_init();
        run_setup_mode();
        return;
    }

    lcd_display_set_vin(vin);
    s_paired = app_settings_is_paired();
    lcd_display_set_paired(s_paired);

    // 接着上次关机的页面继续显示
    restore_ui_mode();

    ret = tesla_ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败: %d", ret);
        lcd_display_set_hint("蓝牙初始化失败");
        return;
    }
    tesla_ble_set_state_callback(on_state_change, NULL);

    ret = prepare_key();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "密钥准备失败: %d", ret);
        lcd_display_set_hint("密钥初始化失败");
        return;
    }

    // 这里不预设提示：界面会停在“扫描中”等待页，文字由连接阶段自动决定
    lcd_display_set_hint(NULL);

    if (xTaskCreate(tesla_task, "tesla_main", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "主任务创建失败");
    }

    // 双击切模式、车钥匙按钮
    // 栈给到 8192：按键回调里要跑 protobuf 打包 + AES-GCM 加解密，4KB 容易栈溢出
    if (xTaskCreate(ui_action_task, "ui_action", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "界面交互任务创建失败");
    }

    // 语音提示放在最后：即使 ES8311 初始化卡住/失败，也不会挡住上面两个任务，
    // 双击切模式、屏幕按键照样可用（此前它排在任务创建之前，一旦卡住双击就完全失效）。
    if (audio_prompt_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音提示不可用，继续运行");
    }
}
