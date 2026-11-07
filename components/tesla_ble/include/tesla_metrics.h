/**
 * @file tesla_metrics.h
 * @brief 车辆状态指标表：仪表盘 6 个格子从这里选，配置页也按这张表给出选项
 *
 * 每个指标负责"从 tesla_vehicle_state_t 取出一个值 + 中文格式化 + 语义色"，
 * UI 层只拿到字符串和颜色，不需要知道协议细节。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tesla_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 指标语义色（UI 层映射到具体 RGB） */
typedef enum {
    TESLA_TONE_NORMAL = 0,  // 普通（白）
    TESLA_TONE_GOOD = 1,    // 正常/开启（绿）
    TESLA_TONE_WARN = 2,    // 注意（黄）
    TESLA_TONE_BAD = 3,     // 异常/关闭（红）
    TESLA_TONE_DIM = 4,     // 次要（灰）
} tesla_tone_t;

/**
 * @brief 可展示指标
 *
 * 顺序即配置页下拉框的顺序；不要随意调整已有值（NVS 里存的是这些数字）。
 */
typedef enum {
    TESLA_METRIC_NONE = 0,

    // 行驶
    TESLA_METRIC_SPEED,          // 车速
    TESLA_METRIC_POWER,          // 功率（+ 输出 / - 回收）
    TESLA_METRIC_GEAR,           // 档位
    TESLA_METRIC_ODOMETER,       // 总里程
    TESLA_METRIC_ROUTE_DEST,     // 目的地
    TESLA_METRIC_ROUTE_ETA,      // 剩余时间
    TESLA_METRIC_ROUTE_DIST,     // 剩余距离

    // 电池 / 充电
    TESLA_METRIC_BATTERY,        // 电量
    TESLA_METRIC_RANGE,          // 额定续航
    TESLA_METRIC_RANGE_EST,      // 估算续航
    TESLA_METRIC_CHARGE_STATE,   // 充电状态
    TESLA_METRIC_CHARGE_POWER,   // 充电功率
    TESLA_METRIC_CHARGE_VOLT,    // 充电电压
    TESLA_METRIC_CHARGE_AMP,     // 充电电流
    TESLA_METRIC_CHARGE_TIME,    // 充满用时
    TESLA_METRIC_CHARGE_LIMIT,   // 充电限值
    TESLA_METRIC_CHARGE_ADDED,   // 已充电量
    TESLA_METRIC_CHARGE_PORT,    // 充电口

    // 空调
    TESLA_METRIC_INSIDE_TEMP,    // 车内温度
    TESLA_METRIC_OUTSIDE_TEMP,   // 车外温度
    TESLA_METRIC_TEMP_SET,       // 设定温度
    TESLA_METRIC_CLIMATE,        // 空调开关
    TESLA_METRIC_FAN,            // 风量
    TESLA_METRIC_SEAT_HEAT_L,    // 主驾加热
    TESLA_METRIC_SEAT_HEAT_R,    // 副驾加热
    TESLA_METRIC_SEAT_FAN_L,     // 主驾通风
    TESLA_METRIC_STEERING_HEAT,  // 方向盘加热
    TESLA_METRIC_BIOWEAPON,      // 生化防御
    TESLA_METRIC_COP,            // 过热保护
    TESLA_METRIC_DEFROST,        // 除霜

    // 胎压
    TESLA_METRIC_TP_FL,
    TESLA_METRIC_TP_FR,
    TESLA_METRIC_TP_RL,
    TESLA_METRIC_TP_RR,

    // 车身
    TESLA_METRIC_LOCK,           // 车锁
    TESLA_METRIC_DOORS,          // 车门汇总
    TESLA_METRIC_TRUNK,          // 后备箱
    TESLA_METRIC_FRUNK,          // 前备箱
    TESLA_METRIC_WINDOWS,        // 车窗
    TESLA_METRIC_SUNROOF,        // 天窗
    TESLA_METRIC_SENTRY,         // 哨兵
    TESLA_METRIC_DISPLAY,        // 中控屏
    TESLA_METRIC_VALET,          // 代客模式
    TESLA_METRIC_USER_PRESENT,   // 是否有人在车
    TESLA_METRIC_REMOTE_START,   // 远程启动

    // 四个门单独上屏：底层本来就是分开上报的（door_open[0..3]）。
    // 刻意追加在末尾而不是插进车身组——插进去会让所有后续取值 +4，
    // 已经存在 NVS 里的格子配置就会整体错位、指向别的指标。
    TESLA_METRIC_DOOR_FL,        // 左前门
    TESLA_METRIC_DOOR_FR,        // 右前门
    TESLA_METRIC_DOOR_RL,        // 左后门
    TESLA_METRIC_DOOR_RR,        // 右后门

    // 媒体
    TESLA_METRIC_MEDIA_TITLE,    // 曲名
    TESLA_METRIC_MEDIA_ARTIST,   // 艺术家
    TESLA_METRIC_MEDIA_STATUS,   // 播放状态
    TESLA_METRIC_VOLUME,         // 音量

    // 软件 / 位置
    TESLA_METRIC_VERSION,        // 软件更新
    TESLA_METRIC_OTA,            // 升级状态
    TESLA_METRIC_OTA_PROGRESS,   // 升级进度
    TESLA_METRIC_LOCATION_NAME,  // 位置
    TESLA_METRIC_HEADING,        // 车头朝向

    // 胎压状态：四轮最低值，低于 TP_HEALTH_MIN_BAR 判为不健康。
    // 同样追加在末尾（同 DOOR_FL 的说明），不能插进胎压组，否则会让已保存的槽位整体错位。
    TESLA_METRIC_TP_HEALTH,

    TESLA_METRIC_MAX
} tesla_metric_t;

/** 胎压健康阈值：四轮最低胎压低于此值（bar）判为不健康 */
#define TP_HEALTH_MIN_BAR 2.6f

/** 指标描述（配置页用） */
typedef struct {
    tesla_metric_t id;
    const char *name;    // 中文名称
} tesla_metric_info_t;

/** 指标值 */
typedef struct {
    char text[28];
    uint8_t tone;        // tesla_tone_t
} tesla_metric_value_t;

/** 指标表（含首项"不显示"） */
const tesla_metric_info_t *tesla_metric_table(int *count);

/** 取指标中文名，越界返回空串 */
const char *tesla_metric_name(tesla_metric_t id);

/**
 * @brief 格式化一个指标
 * @param out 输出文本与语义色
 */
void tesla_metric_format(tesla_metric_t id, const tesla_vehicle_state_t *st, tesla_metric_value_t *out);

#ifdef __cplusplus
}
#endif
