/**
 * @file tesla_metrics.c
 * @brief 车辆状态指标表实现
 */
#include "tesla_metrics.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static const tesla_metric_info_t s_metric_table[] = {
    {TESLA_METRIC_NONE, "不显示"},

    {TESLA_METRIC_SPEED, "车速"},
    {TESLA_METRIC_POWER, "功率"},
    {TESLA_METRIC_GEAR, "档位"},
    {TESLA_METRIC_ODOMETER, "总里程"},
    {TESLA_METRIC_ROUTE_DEST, "目的地"},
    {TESLA_METRIC_ROUTE_ETA, "剩余时间"},
    {TESLA_METRIC_ROUTE_DIST, "剩余距离"},

    {TESLA_METRIC_BATTERY, "电量"},
    {TESLA_METRIC_RANGE, "额定续航"},
    {TESLA_METRIC_RANGE_EST, "估算续航"},
    {TESLA_METRIC_CHARGE_STATE, "充电状态"},
    {TESLA_METRIC_CHARGE_POWER, "充电功率"},
    {TESLA_METRIC_CHARGE_VOLT, "充电电压"},
    {TESLA_METRIC_CHARGE_AMP, "充电电流"},
    {TESLA_METRIC_CHARGE_TIME, "充满用时"},
    {TESLA_METRIC_CHARGE_LIMIT, "充电限值"},
    {TESLA_METRIC_CHARGE_ADDED, "已充电量"},
    {TESLA_METRIC_CHARGE_PORT, "充电口"},

    {TESLA_METRIC_INSIDE_TEMP, "车内温度"},
    {TESLA_METRIC_OUTSIDE_TEMP, "车外温度"},
    {TESLA_METRIC_TEMP_SET, "设定温度"},
    {TESLA_METRIC_CLIMATE, "空调开关"},
    {TESLA_METRIC_FAN, "风量"},
    {TESLA_METRIC_SEAT_HEAT_L, "主驾加热"},
    {TESLA_METRIC_SEAT_HEAT_R, "副驾加热"},
    {TESLA_METRIC_SEAT_FAN_L, "主驾通风"},
    {TESLA_METRIC_STEERING_HEAT, "方向盘加热"},
    {TESLA_METRIC_BIOWEAPON, "生化防御"},
    {TESLA_METRIC_COP, "过热保护"},
    {TESLA_METRIC_DEFROST, "除霜"},

    {TESLA_METRIC_TP_FL, "左前胎压"},
    {TESLA_METRIC_TP_FR, "右前胎压"},
    {TESLA_METRIC_TP_RL, "左后胎压"},
    {TESLA_METRIC_TP_RR, "右后胎压"},

    {TESLA_METRIC_LOCK, "车锁"},
    {TESLA_METRIC_DOORS, "车门汇总"},
    {TESLA_METRIC_DOOR_FL, "左前门"},
    {TESLA_METRIC_DOOR_FR, "右前门"},
    {TESLA_METRIC_DOOR_RL, "左后门"},
    {TESLA_METRIC_DOOR_RR, "右后门"},
    {TESLA_METRIC_TRUNK, "后备箱"},
    {TESLA_METRIC_FRUNK, "前备箱"},
    {TESLA_METRIC_WINDOWS, "车窗"},
    {TESLA_METRIC_SUNROOF, "天窗"},
    {TESLA_METRIC_SENTRY, "哨兵模式"},
    {TESLA_METRIC_DISPLAY, "中控屏"},
    {TESLA_METRIC_VALET, "代客模式"},
    {TESLA_METRIC_USER_PRESENT, "车内有人"},
    {TESLA_METRIC_REMOTE_START, "远程启动"},

    {TESLA_METRIC_MEDIA_TITLE, "曲名"},
    {TESLA_METRIC_MEDIA_ARTIST, "艺术家"},
    {TESLA_METRIC_MEDIA_STATUS, "播放状态"},
    {TESLA_METRIC_VOLUME, "音量"},

    {TESLA_METRIC_VERSION, "软件更新"},
    {TESLA_METRIC_OTA, "升级状态"},
    {TESLA_METRIC_OTA_PROGRESS, "升级进度"},
    {TESLA_METRIC_LOCATION_NAME, "位置"},
    {TESLA_METRIC_HEADING, "车头朝向"},

    {TESLA_METRIC_TP_HEALTH, "胎压状态"},
};

const tesla_metric_info_t *tesla_metric_table(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_metric_table) / sizeof(s_metric_table[0]));
    }
    return s_metric_table;
}

const char *tesla_metric_name(tesla_metric_t id)
{
    for (size_t i = 0; i < sizeof(s_metric_table) / sizeof(s_metric_table[0]); i++) {
        if (s_metric_table[i].id == id) {
            return s_metric_table[i].name;
        }
    }
    return "";
}

static void set_text(tesla_metric_value_t *out, const char *text, tesla_tone_t tone)
{
    snprintf(out->text, sizeof(out->text), "%s", text);
    out->tone = (uint8_t)tone;
}

static const char *charging_state_text(tesla_charging_state_t s)
{
    switch (s) {
    case TESLA_CHARGING_DISCONNECTED: return "未连接";
    case TESLA_CHARGING_NO_POWER: return "无供电";
    case TESLA_CHARGING_STARTING: return "启动中";
    case TESLA_CHARGING_CHARGING: return "充电中";
    case TESLA_CHARGING_COMPLETE: return "已充满";
    case TESLA_CHARGING_STOPPED: return "已停止";
    case TESLA_CHARGING_CALIBRATING: return "校准中";
    default: return "未知";
    }
}

static const char *seat_heat_text(int32_t level)
{
    switch (level) {
    case 0: return "关";
    case 1: return "低";
    case 2: return "中";
    case 3: return "高";
    default: return "关";
    }
}

static const char *sentry_text(int32_t s)
{
    switch (s) {
    case 1: return "关闭";
    case 2: return "待命";
    case 3: return "已布防";
    case 4: return "警戒";
    case 5: return "警报";
    case 6: return "静默";
    default: return "--";
    }
}

static const char *display_text(int32_t s)
{
    switch (s) {
    case 1: return "熄屏";
    case 2: return "暗屏";
    case 3: return "附件";
    case 4: return "亮屏";
    case 5: return "行驶";
    case 6: return "充电";
    case 7: return "锁屏";
    case 8: return "哨兵";
    case 9: return "宠物";
    case 10: return "娱乐";
    default: return "--";
    }
}

static const char *media_status_text(int32_t s, bool playing)
{
    if (playing) {
        return "播放中";
    }
    switch (s) {
    case 0: return "已停止";
    case 1: return "播放中";
    case 2: return "已暂停";
    default: return "--";
    }
}

static const char *gear_text(tesla_gear_state_t g)
{
    switch (g) {
    case TESLA_GEAR_P: return "P";
    case TESLA_GEAR_R: return "R";
    case TESLA_GEAR_N: return "N";
    case TESLA_GEAR_D: return "D";
    case TESLA_GEAR_SNA: return "SNA";
    default: return "--";
    }
}

void tesla_metric_format(tesla_metric_t id, const tesla_vehicle_state_t *st, tesla_metric_value_t *out)
{
    if (!out) {
        return;
    }
    set_text(out, "--", TESLA_TONE_DIM);
    if (!st || id == TESLA_METRIC_NONE) {
        return;
    }

    char buf[32];
    const bool drive_ok = (st->present_mask & TESLA_STATE_BIT_DRIVE) != 0;
    const bool charge_ok = (st->present_mask & TESLA_STATE_BIT_CHARGE) != 0;
    const bool climate_ok = (st->present_mask & TESLA_STATE_BIT_CLIMATE) != 0;
    const bool close_ok = (st->present_mask & TESLA_STATE_BIT_CLOSURES) != 0;
    const bool tire_ok = (st->present_mask & TESLA_STATE_BIT_TIRE) != 0;
    const bool media_ok = (st->present_mask & TESLA_STATE_BIT_MEDIA) != 0;
    const bool sw_ok = (st->present_mask & TESLA_STATE_BIT_SOFTWARE) != 0;
    const bool loc_ok = (st->present_mask & TESLA_STATE_BIT_LOCATION) != 0;

    switch (id) {
    case TESLA_METRIC_SPEED:
        if (!drive_ok) return;
        // drive.speed_kmh 里存的是车辆上报的原始 mph 值（历史字段名语义），这里换成 km/h
        snprintf(buf, sizeof(buf), "%.0f km/h", (double)(st->drive.speed_kmh * 1.60934f));
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_POWER:
        if (!drive_ok) return;
        snprintf(buf, sizeof(buf), "%+ld kW", (long)st->drive.power_kw);
        // 正值输出（费电）用黄，负值回收用绿
        set_text(out, buf, st->drive.power_kw > 0 ? TESLA_TONE_WARN : TESLA_TONE_GOOD);
        return;

    case TESLA_METRIC_GEAR:
        if (!drive_ok) return;
        set_text(out, gear_text(st->drive.gear), TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_ODOMETER:
        if (!drive_ok) return;
        snprintf(buf, sizeof(buf), "%.0f km", st->drive.odometer_km);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_ROUTE_DEST:
        if (!drive_ok || st->drive.destination[0] == '\0') return;
        set_text(out, st->drive.destination, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_ROUTE_ETA:
        if (!drive_ok || st->drive.minutes_to_arrival <= 0) return;
        snprintf(buf, sizeof(buf), "%.0f 分", (double)st->drive.minutes_to_arrival);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_ROUTE_DIST:
        if (!drive_ok || st->drive.km_to_arrival <= 0) return;
        snprintf(buf, sizeof(buf), "%.1f km", (double)st->drive.km_to_arrival);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_BATTERY:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%ld%%", (long)st->charge.battery_level);
        set_text(out, buf, st->charge.battery_level <= 20 ? TESLA_TONE_WARN : TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_RANGE:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%.0f km", (double)st->charge.battery_range_km);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_RANGE_EST:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%.0f km", (double)st->charge.est_battery_range_km);
        set_text(out, buf, TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_CHARGE_STATE:
        if (!charge_ok) return;
        set_text(out, charging_state_text(st->charge.charging_state),
                 st->charge.charging_state == TESLA_CHARGING_CHARGING ? TESLA_TONE_GOOD
                                                                      : TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_POWER:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%ld kW", (long)st->charge.charger_power_kw);
        set_text(out, buf, st->charge.charger_power_kw > 0 ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_CHARGE_VOLT:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%ld V", (long)st->charge.charger_voltage);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_AMP:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%ld A", (long)st->charge.charger_current_a);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_TIME:
        if (!charge_ok || st->charge.minutes_to_full <= 0) return;
        snprintf(buf, sizeof(buf), "%ld 分", (long)st->charge.minutes_to_full);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_LIMIT:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%ld%%", (long)st->charge.charge_limit_soc);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_ADDED:
        if (!charge_ok) return;
        snprintf(buf, sizeof(buf), "%.1f kWh", (double)st->charge.energy_added_kwh);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CHARGE_PORT:
        if (!charge_ok) return;
        set_text(out, st->charge.charge_port_open ? "已打开" : "已关闭",
                 st->charge.charge_port_open ? TESLA_TONE_WARN : TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_INSIDE_TEMP:
        if (!climate_ok) return;
        snprintf(buf, sizeof(buf), "%.1f℃", (double)st->climate.inside_temp_c);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_OUTSIDE_TEMP:
        if (!climate_ok) return;
        snprintf(buf, sizeof(buf), "%.1f℃", (double)st->climate.outside_temp_c);
        set_text(out, buf, TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_TEMP_SET:
        if (!climate_ok) return;
        snprintf(buf, sizeof(buf), "%.1f℃", (double)st->climate.driver_temp_setting);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_CLIMATE:
        if (!climate_ok) return;
        set_text(out, st->climate.climate_on ? "开" : "关",
                 st->climate.climate_on ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_FAN:
        if (!climate_ok) return;
        snprintf(buf, sizeof(buf), "%ld 档", (long)st->climate.fan_status);
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_SEAT_HEAT_L:
        if (!climate_ok) return;
        set_text(out, seat_heat_text(st->climate.seat_heater_left),
                 st->climate.seat_heater_left > 0 ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_SEAT_HEAT_R:
        if (!climate_ok) return;
        set_text(out, seat_heat_text(st->climate.seat_heater_right),
                 st->climate.seat_heater_right > 0 ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_SEAT_FAN_L:
        if (!climate_ok) return;
        set_text(out, seat_heat_text(st->climate.seat_fan_left),
                 st->climate.seat_fan_left > 0 ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_STEERING_HEAT:
        if (!climate_ok) return;
        set_text(out, st->climate.steering_wheel_heater ? "开" : "关",
                 st->climate.steering_wheel_heater ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_BIOWEAPON:
        if (!climate_ok) return;
        set_text(out, st->climate.bioweapon_mode ? "开" : "关",
                 st->climate.bioweapon_mode ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_COP:
        if (!climate_ok) return;
        set_text(out, st->climate.cabin_overheat == 1 ? "开" : "关",
                 st->climate.cabin_overheat == 1 ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_DEFROST:
        if (!climate_ok) return;
        if (st->climate.front_defroster && st->climate.rear_defroster) {
            set_text(out, "前后", TESLA_TONE_WARN);
        } else if (st->climate.front_defroster) {
            set_text(out, "前", TESLA_TONE_WARN);
        } else if (st->climate.rear_defroster) {
            set_text(out, "后", TESLA_TONE_WARN);
        } else {
            set_text(out, "关", TESLA_TONE_DIM);
        }
        return;

    case TESLA_METRIC_TP_FL:
    case TESLA_METRIC_TP_FR:
    case TESLA_METRIC_TP_RL:
    case TESLA_METRIC_TP_RR: {
        if (!tire_ok) return;
        const tesla_tire_pressure_t *t = &st->tires;
        const struct { float v; bool warn; } wheel[4] = {
            {t->front_left_bar,  t->warning_fl},
            {t->front_right_bar, t->warning_fr},
            {t->rear_left_bar,   t->warning_rl},
            {t->rear_right_bar,  t->warning_rr},
        };
        int idx = (int)(id - TESLA_METRIC_TP_FL);
        if (idx < 0 || idx > 3) return;
        float v = wheel[idx].v;
        bool warn = wheel[idx].warn;
        if (v <= 0.01f) return;
        snprintf(buf, sizeof(buf), "%.2f bar", (double)v);
        set_text(out, buf, warn ? TESLA_TONE_BAD : TESLA_TONE_NORMAL);
        return;
    }

    case TESLA_METRIC_TP_HEALTH: {
        // 四轮取最低值：任一轮胎低于阈值就判不健康（软/硬告警只作辅助，
        // 实测部分车型告警标志不上报，按数值判更可靠）
        if (!tire_ok) return;
        const tesla_tire_pressure_t *t = &st->tires;
        float min_v = 0.0f;
        const float wheels[4] = {t->front_left_bar, t->front_right_bar,
                                 t->rear_left_bar, t->rear_right_bar};
        for (int i = 0; i < 4; i++) {
            if (wheels[i] > 0.01f && (min_v <= 0.01f || wheels[i] < min_v)) {
                min_v = wheels[i];
            }
        }
        if (min_v <= 0.01f) return;

        bool healthy = (min_v >= TP_HEALTH_MIN_BAR) &&
                       !t->warning_fl && !t->warning_fr && !t->warning_rl && !t->warning_rr;
        snprintf(buf, sizeof(buf), "%.1f %s", (double)min_v, healthy ? "正常" : "偏低");
        set_text(out, buf, healthy ? TESLA_TONE_GOOD : TESLA_TONE_BAD);
        return;
    }

    case TESLA_METRIC_LOCK: {
        bool locked = (close_ok && st->closures.locked) ||
                      st->body.lock_state == TESLA_LOCK_LOCKED ||
                      st->body.lock_state == TESLA_LOCK_INTERNAL_LOCKED;
        set_text(out, locked ? "已锁" : "未锁", locked ? TESLA_TONE_GOOD : TESLA_TONE_BAD);
        return;
    }

    case TESLA_METRIC_DOOR_FL:
    case TESLA_METRIC_DOOR_FR:
    case TESLA_METRIC_DOOR_RL:
    case TESLA_METRIC_DOOR_RR: {
        if (!close_ok) return;
        int idx = (id == TESLA_METRIC_DOOR_FL) ? 0 :
                  (id == TESLA_METRIC_DOOR_FR) ? 1 :
                  (id == TESLA_METRIC_DOOR_RL) ? 2 : 3;
        bool open = st->closures.door_open[idx];
        set_text(out, open ? "未关" : "已关", open ? TESLA_TONE_WARN : TESLA_TONE_GOOD);
        return;
    }

    case TESLA_METRIC_DOORS: {
        if (!close_ok) return;
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (st->closures.door_open[i]) {
                n++;
            }
        }
        if (n == 0) {
            set_text(out, "全关", TESLA_TONE_GOOD);
        } else {
            snprintf(buf, sizeof(buf), "%d 门未关", n);
            set_text(out, buf, TESLA_TONE_WARN);
        }
        return;
    }

    case TESLA_METRIC_TRUNK: {
        bool open = st->closures.trunk_rear_open ||
                    st->body.closures.rear_trunk != TESLA_CLOSURE_CLOSED;
        set_text(out, open ? "已开" : "已关", open ? TESLA_TONE_WARN : TESLA_TONE_GOOD);
        return;
    }

    case TESLA_METRIC_FRUNK: {
        bool open = st->closures.trunk_front_open ||
                    st->body.closures.front_trunk != TESLA_CLOSURE_CLOSED;
        set_text(out, open ? "已开" : "已关", open ? TESLA_TONE_WARN : TESLA_TONE_GOOD);
        return;
    }

    case TESLA_METRIC_WINDOWS: {
        if (!close_ok) return;
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (st->closures.window_open[i]) {
                n++;
            }
        }
        if (n == 0) {
            set_text(out, "全关", TESLA_TONE_GOOD);
        } else {
            snprintf(buf, sizeof(buf), "%d 窗未关", n);
            set_text(out, buf, TESLA_TONE_WARN);
        }
        return;
    }

    case TESLA_METRIC_SUNROOF:
        if (!close_ok) return;
        if (st->closures.sunroof_percent > 0) {
            snprintf(buf, sizeof(buf), "%ld%%", (long)st->closures.sunroof_percent);
            set_text(out, buf, TESLA_TONE_WARN);
        } else {
            set_text(out, "已关", TESLA_TONE_GOOD);
        }
        return;

    case TESLA_METRIC_SENTRY:
        if (!close_ok) return;
        set_text(out, sentry_text(st->closures.sentry_state),
                 st->closures.sentry_state >= 3 ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_DISPLAY:
        if (!close_ok) return;
        set_text(out, display_text(st->closures.display_state), TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_VALET:
        if (!close_ok) return;
        set_text(out, st->closures.valet_mode ? "开" : "关",
                 st->closures.valet_mode ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_USER_PRESENT: {
        bool present = st->closures.user_present || st->body.user_present;
        set_text(out, present ? "是" : "否", present ? TESLA_TONE_GOOD : TESLA_TONE_DIM);
        return;
    }

    case TESLA_METRIC_REMOTE_START:
        if (!close_ok) return;
        set_text(out, st->closures.remote_start ? "开" : "关",
                 st->closures.remote_start ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;

    case TESLA_METRIC_MEDIA_TITLE:
        // 曲名的新鲜度由解析层保证（media_state 到来时会重写整个 media，
        // 停止播放时 title 自然变空）。这里不再用 present_mask 位二次把关：
        // 分组轮询每轮都会清掉 MEDIA 位，某轮请求抖动就会让格子闪回 "--"。
        if (st->media.title[0] == '\0') return;
        set_text(out, st->media.title, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_MEDIA_ARTIST:
        if (!media_ok) return;
        if (st->media.artist[0]) {
            set_text(out, st->media.artist, TESLA_TONE_DIM);
        } else if (st->media.album[0]) {
            set_text(out, st->media.album, TESLA_TONE_DIM);
        }
        return;

    case TESLA_METRIC_MEDIA_STATUS:
        if (!media_ok) return;
        set_text(out, media_status_text(st->media.playback_status,
                                        st->media.playback_status == 1),
                 TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_VOLUME:
        if (!media_ok) return;
        if (st->media.audio_volume_max > 0) {
            snprintf(buf, sizeof(buf), "%.0f/%.0f",
                     (double)st->media.audio_volume, (double)st->media.audio_volume_max);
        } else {
            snprintf(buf, sizeof(buf), "%.0f", (double)st->media.audio_volume);
        }
        set_text(out, buf, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_VERSION:
        // version 只在有 OTA 时上报，是"待安装版本"而非当前固件版本，车机
        // 平时整个字段都不设置。present_mask 的 SOFTWARE 位每轮被分组轮询
        // 清一次，用位把关会让读到的版本也显示不出来，这里直接按值判断。
        // 拿不到时显示"无更新"（而不是 "--"），更贴近字段的实际含义。
        // 只接受含可见字符的版本号：车机偶尔把 oneof version 设成空串或全空白，
        // 直接画出来就是一个看起来"没数据"的空白格子。
        bool usable = false;
        for (const unsigned char *p = (const unsigned char *)st->software.version; *p; p++) {
            if (*p > 0x20) {
                usable = true;
                break;
            }
        }
        // 用普通白色而不是灰色：DIM 在深色卡片上太暗，容易被看成空值。
        set_text(out, usable ? st->software.version : "无更新", TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_OTA: {
        if (!sw_ok) return;
        const char *t = "--";
        switch (st->software.status) {
        case 2: t = "安装中"; break;
        case 3: t = "已预约"; break;
        case 4: t = "有新版本"; break;
        case 5: t = "等待 WiFi"; break;
        case 6: t = "下载中"; break;
        default: t = "无更新"; break;
        }
        set_text(out, t, st->software.status >= 4 ? TESLA_TONE_WARN : TESLA_TONE_DIM);
        return;
    }

    case TESLA_METRIC_OTA_PROGRESS:
        if (!sw_ok) return;
        if (st->software.install_percent > 0) {
            snprintf(buf, sizeof(buf), "装 %lu%%", (unsigned long)st->software.install_percent);
        } else if (st->software.download_percent > 0) {
            snprintf(buf, sizeof(buf), "下 %lu%%", (unsigned long)st->software.download_percent);
        } else {
            return;
        }
        set_text(out, buf, TESLA_TONE_WARN);
        return;

    case TESLA_METRIC_LOCATION_NAME:
        if (!loc_ok || st->location.name[0] == '\0') return;
        set_text(out, st->location.name, TESLA_TONE_NORMAL);
        return;

    case TESLA_METRIC_HEADING:
        if (!loc_ok) return;
        snprintf(buf, sizeof(buf), "%.0f°", (double)st->location.heading);
        set_text(out, buf, TESLA_TONE_DIM);
        return;

    default:
        return;
    }
}
