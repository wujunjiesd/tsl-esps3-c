/**
 * @file tesla_ble.h
 * @brief Tesla BLE Connection Library for ESP32
 *
 * 通过蓝牙连接特斯拉车辆并获取车辆参数
 */

#ifndef TESLA_BLE_H
#define TESLA_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Tesla BLE UUIDs ==========

// Tesla Vehicle Service UUID: 00000211-b2d1-43f0-9b88-960cebf8b91e
#define TESLA_SERVICE_UUID_128 \
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, \
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00

// To Vehicle Characteristic UUID (Write): 00000212-b2d1-43f0-9b88-960cebf8b91e
#define TESLA_CHAR_WRITE_UUID_128 \
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, \
    0xf0, 0x43, 0xd1, 0xb2, 0x12, 0x02, 0x00, 0x00

// From Vehicle Characteristic UUID (Read/Notify): 00000213-b2d1-43f0-9b88-960cebf8b91e
#define TESLA_CHAR_READ_UUID_128 \
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, \
    0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00

// ========== Protocol Constants ==========

#define TESLA_MAX_MESSAGE_SIZE      1024
#define TESLA_MAX_MTU               512
#define TESLA_RX_TIMEOUT_MS         1000
#define TESLA_SESSION_TIMEOUT_SEC   30

/**
 * @brief Domain Types
 * @note 域类型
 */
typedef enum {
    TESLA_DOMAIN_BROADCAST = 0, // 广播
    TESLA_DOMAIN_VEHICLE_SECURITY = 2,  // 车辆安全（车机休眠可以获取）
    TESLA_DOMAIN_INFOTAINMENT = 3 // 信息娱乐（车机休眠无法获取）
} tesla_domain_t;

/**
 * @brief Connection State
 * @note 连接状态
 */
typedef enum {
    TESLA_STATE_DISCONNECTED = 0, // 断开连接
    TESLA_STATE_SCANNING, // 扫描中
    TESLA_STATE_CONNECTING, // 连接中
    TESLA_STATE_CONNECTED, // 已连接
    TESLA_STATE_SERVICE_DISCOVERED, // 服务已发现
    TESLA_STATE_AUTHENTICATED // 已认证
} tesla_conn_state_t;

/**
 * @brief Vehicle Lock State
 * @note 车辆锁定状态
 */
typedef enum {
    TESLA_LOCK_UNLOCKED = 0, // 未锁定
    TESLA_LOCK_LOCKED = 1, // 锁定
    TESLA_LOCK_INTERNAL_LOCKED = 2, // 内部锁定
    TESLA_LOCK_SELECTIVE_UNLOCKED = 3 // 选择性解锁
} tesla_lock_state_t;

/**
 * @brief Vehicle Sleep State
 * @note 车辆睡眠状态
 */
typedef enum {
    TESLA_SLEEP_UNKNOWN = 0, // 未知
    TESLA_SLEEP_AWAKE = 1, // 唤醒
    TESLA_SLEEP_ASLEEP = 2 // 休眠
} tesla_sleep_state_t;

/**
 * @brief Closure State
 * @note 关闭状态
 */
typedef enum {
    TESLA_CLOSURE_CLOSED = 0, // 关闭
    TESLA_CLOSURE_OPEN = 1, // 打开
    TESLA_CLOSURE_AJAR = 2, // 松动
    TESLA_CLOSURE_UNKNOWN = 3, // 未知
    TESLA_CLOSURE_FAILED_UNLATCH = 4, // 解锁失败
    TESLA_CLOSURE_OPENING = 5, // 打开中
    TESLA_CLOSURE_CLOSING = 6 // 关闭中
} tesla_closure_state_t;

/**
 * @brief Shift State
 * @note 档位状态
 */
typedef enum {
    TESLA_GEAR_INVALID = 0, // 无效
    TESLA_GEAR_P = 2,
    TESLA_GEAR_R = 3, // 倒档
    TESLA_GEAR_N = 4, // 空档
    TESLA_GEAR_D = 5, // 前进档
    TESLA_GEAR_SNA = 6
} tesla_gear_state_t;

/**
 * @brief Vehicle State Structures
 * @note 车辆状态结构体
 */
typedef struct {
    tesla_closure_state_t front_driver_door; // 前驾驶员门
    tesla_closure_state_t front_passenger_door; // 前乘客门
    tesla_closure_state_t rear_driver_door; // 后驾驶员门
    tesla_closure_state_t rear_passenger_door; // 后乘客门
    tesla_closure_state_t rear_trunk; // 后行李箱
    tesla_closure_state_t front_trunk; // 前行李箱
    tesla_closure_state_t charge_port; // 充电口
    tesla_closure_state_t tonneau; // 车顶
} tesla_closure_status_t;

/**
 * @brief Body State
 * @note 车身状态
 */
typedef struct {
    tesla_lock_state_t lock_state; // 锁定状态
    tesla_sleep_state_t sleep_status; // 睡眠状态
    tesla_closure_status_t closures; // 关闭状态
    bool user_present; // 用户是否存在
} tesla_body_state_t;

/**
 * @brief Drive State
 * @note 驾驶状态
 */
typedef struct {
    tesla_gear_state_t gear; // 档位
    float speed_kmh; // 速度
    int32_t power_kw; // 功率
    float odometer_km; // 里程
    char destination[64]; // 目的地
    float minutes_to_arrival; // 到达时间
    float km_to_arrival; // 到达距离
} tesla_drive_state_t;

/**
 * @brief 充电状态枚举（对应 ChargeState.charging_state 的 oneof）
 */
typedef enum {
    TESLA_CHARGING_UNKNOWN = 0,
    TESLA_CHARGING_DISCONNECTED = 2,
    TESLA_CHARGING_NO_POWER = 3,
    TESLA_CHARGING_STARTING = 4,
    TESLA_CHARGING_CHARGING = 5,
    TESLA_CHARGING_COMPLETE = 6,
    TESLA_CHARGING_STOPPED = 7,
    TESLA_CHARGING_CALIBRATING = 8,
} tesla_charging_state_t;

/**
 * @brief Charge State
 * @note 充电状态
 */
typedef struct {
    int32_t battery_level;          // 0-100%
    int32_t usable_battery_level; // 可用电池电量
    float battery_range_km; // 电池续航里程（rated）
    float est_battery_range_km; // 估算续航
    float ideal_range_km; // 理想续航里程
    int32_t charge_limit_soc; // 充电限制SOC
    tesla_charging_state_t charging_state; // 充电状态
    bool fast_charger_present;   // 快充中
    int32_t charger_power_kw; // 充电功率
    int32_t charger_voltage; // 充电电压
    int32_t charger_current_a; // 充电电流
    int32_t charger_phases;      // 相数
    int32_t minutes_to_full; // 充满时间
    float charge_rate_kmh; // 充电速度
    float energy_added_kwh; // 本次充入电量
    bool charge_port_open; // 充电口是否打开
    bool charge_port_latched; // 充电口是否锁定
    int32_t charge_current_request;      // 当前请求电流
    int32_t charge_current_request_max;  // 最大请求电流
} tesla_charge_state_t;

/**
 * @brief Climate State
 * @note 气候状态
 */
typedef struct {
    bool climate_on;
    float inside_temp_c; // 车内温度
    float outside_temp_c; // 车外温度
    float driver_temp_setting; // 驾驶员温度设置
    float passenger_temp_setting; // 乘客温度设置
    int32_t fan_status;          // 风量 0-10
    int32_t seat_heater_left; // 左座椅加热
    int32_t seat_heater_right; // 右座椅加热
    int32_t seat_heater_rear_left; // 后左座椅加热
    int32_t seat_heater_rear_right; // 后右座椅加热
    int32_t seat_fan_left;        // 前左座椅通风
    int32_t seat_fan_right;       // 前右座椅通风
    bool steering_wheel_heater; // 方向盘加热
    bool preconditioning; // 预加热
    bool battery_heater; // 电池加热
    bool front_defroster; // 前挡风玻璃加热
    bool rear_defroster; // 后挡风玻璃加热
    bool bioweapon_mode;         // 生化防御
    int32_t keeper_mode;         // 保持空调模式 2=off 3=on 4=dog 5=party
    int32_t cabin_overheat;      // 座舱过热保护 0=off 1=on 2=fan only
} tesla_climate_state_t;

/**
 * @brief Location State
 * @note 位置状态
 */
typedef struct {
    double latitude; // 纬度
    double longitude; // 经度
    float heading; // 方向
    char name[48]; // 模糊地名
} tesla_location_state_t;

/**
 * @brief Tire Pressure State
 * @note 轮胎压力状态
 */
typedef struct {
    float front_left_bar; // 前左轮胎压力
    float front_right_bar; // 前右轮胎压力
    float rear_left_bar; // 后左轮胎压力
    float rear_right_bar; // 后右轮胎压力
    bool warning_fl; // 前左轮胎压力警告
    bool warning_fr; // 前右轮胎压力警告
    bool warning_rl; // 后左轮胎压力警告
    bool warning_rr; // 后右轮胎压力警告
    float recommend_front_bar;   // 前轮推荐胎压
    float recommend_rear_bar;    // 后轮推荐胎压
} tesla_tire_pressure_t;

/**
 * @brief Software State
 * @note 软件状态
 */
typedef struct {
    char version[32]; // 版本
    int32_t status;     // 1=unknown 2=installing 3=scheduled 4=available 5=wait wifi 6=downloading
    uint32_t download_percent; // 下载进度
    uint32_t install_percent; // 安装进度
} tesla_software_state_t;

/**
 * @brief Media State
 * @note 媒体状态
 */
typedef struct {
    int32_t playback_status; // 0=stopped 1=playing 2=paused
    float audio_volume; // 音频音量
    float audio_volume_max; // 最大音量
    int32_t source;         // MediaSourceType
    char artist[48]; // 艺术家
    char title[64]; // 标题
    char album[48];         // 专辑
} tesla_media_state_t;

/**
 * @brief 车机通道的闭件状态（ClosuresState）
 */
typedef struct {
    bool door_open[4];      // 0=前左 1=前右 2=后左 3=后右
    bool window_open[4];    // 同上顺序
    bool trunk_front_open;
    bool trunk_rear_open;
    bool locked;
    bool user_present;
    bool remote_start;
    bool valet_mode;
    bool sentry_available;
    int32_t sentry_state;   // 1=off 2=idle 3=armed 4=aware 5=panic 6=quiet
    int32_t display_state;  // 1=off 2=dim 3=accessory 4=on 5=driving 6=charging 7=lock 8=sentry 9=dog 10=entertainment
    int32_t sunroof_percent;
} tesla_closures_state_t;

/**
 * @brief 状态类别位掩码（用于 tesla_ble_refresh_states）
 */
#define TESLA_STATE_BIT_CHARGE      (1u << 0)
#define TESLA_STATE_BIT_CLIMATE     (1u << 1)
#define TESLA_STATE_BIT_DRIVE       (1u << 2)
#define TESLA_STATE_BIT_LOCATION    (1u << 3)
#define TESLA_STATE_BIT_CLOSURES    (1u << 4)
#define TESLA_STATE_BIT_TIRE        (1u << 5)
#define TESLA_STATE_BIT_MEDIA       (1u << 6)
#define TESLA_STATE_BIT_MEDIA_DETAIL (1u << 7)
#define TESLA_STATE_BIT_SOFTWARE    (1u << 8)
#define TESLA_STATE_BIT_ALL         (0x1FFu)

/**
 * @brief Complete vehicle state
 * @note 完整车辆状态
 */
typedef struct {
    tesla_body_state_t body; // 车身状态（VCSEC，车机休眠也能读）
    tesla_drive_state_t drive; // 驾驶状态
    tesla_charge_state_t charge; // 充电状态
    tesla_climate_state_t climate; // 气候状态
    tesla_location_state_t location; // 位置状态
    tesla_tire_pressure_t tires; // 轮胎压力状态
    tesla_software_state_t software; // 软件状态
    tesla_media_state_t media; // 媒体状态
    tesla_closures_state_t closures; // 车机通道闭件状态
    uint32_t present_mask; // 哪些类别解析成功（TESLA_STATE_BIT_*）
    uint32_t stamp_ms;     // 最近一次刷新时刻（tick，毫秒）
    bool valid; // 是否有效
} tesla_vehicle_state_t;

/**
 * @brief Session Info
 * @note 会话信息
 */
typedef struct {
    uint8_t epoch[16];
    uint32_t counter; // 计数器
    uint32_t clock_time;          // 会话建立时车辆时钟时间
    uint32_t local_time_sec;      // 会话建立时本地时间（秒）
    uint8_t vehicle_public_key[65]; // 车辆公钥
    uint8_t shared_secret[32]; // 共享密钥
    uint8_t session_key[16]; // 会话密钥
    bool established; // 是否建立
} tesla_session_t;

/**
 * @brief Connection Handle
 * @note 连接句柄
 */
typedef struct {
    uint16_t conn_handle; // 连接句柄
    uint16_t write_handle; // 写句柄
    uint16_t read_handle; // 读句柄
    uint16_t mtu;  // Negotiated MTU value // 协商的MTU值
    tesla_conn_state_t state; // 连接状态
    tesla_session_t session[2];  // [0]=VCSEC, [1]=Infotainment // 会话信息
    uint8_t routing_address[16]; // 路由地址
    char vin[18]; // 车辆识别码
    uint8_t private_key[32]; // 私钥
    uint8_t public_key[65]; // 公钥
    uint8_t rx_buffer[TESLA_MAX_MESSAGE_SIZE]; // 接收缓冲区
    uint16_t rx_len; // 接收长度
    uint8_t last_request_id[17];  // For response decryption: signature_type(1) + GCM_tag(16) // 用于响应解密：签名类型(1) + GCM标签(16)
} tesla_connection_t;

// ========== Callback Types ==========

typedef void (*tesla_state_callback_t)(tesla_conn_state_t state, void *arg);
typedef void (*tesla_vehicle_state_callback_t)(const tesla_vehicle_state_t *state, void *arg);

// ========== API Functions ==========

/**
 * @brief Initialize Tesla BLE module
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_init(void);


/**
 * @brief 设置状态变化回调函数
 * @param callback 状态变化回调函数
 * @param arg 用户参数
 */
void tesla_ble_set_state_callback(tesla_state_callback_t callback, void *arg);


/**
 * @brief 加载或生成私钥
 * @param key_data 私钥数据缓冲区 (32 字节), NULL 生成新密钥
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_load_key(const uint8_t *key_data);


/**
 * @brief 获取私钥 (用于 NVS 存储)
 * @param key_data 存储私钥的缓冲区 (32 字节)
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_get_private_key(uint8_t *key_data);

/**
 * @brief 从 VIN 计算车辆的 BLE 本地名称
 * @param vin 车辆识别码 (17 个字符)
 * @param local_name 输出本地名称的缓冲区 (至少 20 字节)
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t tesla_ble_get_local_name(const char *vin, char *local_name);

/**
 * @brief 开始扫描特斯拉车辆
 * @param vin 扫描目标车辆的 VIN 码
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_scan(const char *vin);

/**
 * @brief 停止扫描
 */
void tesla_ble_scan_stop(void);


/**
 * @brief 断开与车辆的连接
 */
void tesla_ble_disconnect(void);

/**
 * @brief 启动会话与车辆 (密钥交换)
 * @param domain 目标域 (VCSEC 或 Infotainment)
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_start_session(tesla_domain_t domain);

/**
 * @brief 发送添加密钥请求进行配对
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_add_key_request(void);

/**
 * @brief 获取车辆车身状态 (VCSEC)
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_get_body_state(void);







/**
 * @brief 唤醒车辆
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_wake_vehicle(void);

/**
 * @brief 锁定车辆
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_lock(void);

/**
 * @brief 解锁车辆
 * @return ESP_OK on success
 */
esp_err_t tesla_ble_unlock(void);


/**
 * @brief 控制命令（开/关合并成一条，按当前状态取反）
 *
 * 车钥匙页的 4 个按钮就从这个表里选；配置页按名称展示。
 * 命名里带 toggle 的都是"按当前状态取反"，不拆成开、关两条。
 */
typedef enum {
    TESLA_CMD_NONE = 0,

    // ---- VCSEC（车机休眠也可用）----
    TESLA_CMD_LOCK_TOGGLE,        // 车锁（上锁/解锁切换）
    TESLA_CMD_TRUNK_TOGGLE,       // 后备箱（开/关切换）
    TESLA_CMD_FRUNK_TOGGLE,       // 前备箱（开/关切换）
    TESLA_CMD_WAKE,               // 唤醒车辆

    // ---- 灯光 / 喇叭 ----
    TESLA_CMD_FLASH_LIGHTS,       // 闪灯
    TESLA_CMD_HONK_HORN,          // 鸣笛

    // ---- 空调 ----
    TESLA_CMD_CLIMATE_TOGGLE,     // 空调（开/关切换）
    TESLA_CMD_TEMP_UP,            // 温度 +0.5℃
    TESLA_CMD_TEMP_DOWN,          // 温度 -0.5℃
    TESLA_CMD_SEAT_HEATER_TOGGLE, // 主驾座椅加热（关→低→中→高→关）
    TESLA_CMD_STEERING_HEAT_TOGGLE, // 方向盘加热
    TESLA_CMD_BIOWEAPON_TOGGLE,   // 生化防御模式
    TESLA_CMD_COP_TOGGLE,         // 座舱过热保护
    TESLA_CMD_PRECOND_MAX_TOGGLE, // 最大预热/预冷

    // ---- 充电 ----
    TESLA_CMD_CHARGE_TOGGLE,      // 充电（开始/停止切换）
    TESLA_CMD_CHARGE_PORT_TOGGLE, // 充电口（开/关切换）
    TESLA_CMD_CHARGE_AMPS_UP,     // 充电电流 +1A
    TESLA_CMD_CHARGE_AMPS_DOWN,   // 充电电流 -1A
    TESLA_CMD_CHARGE_LIMIT_UP,    // 充电限值 +5%
    TESLA_CMD_CHARGE_LIMIT_DOWN,  // 充电限值 -5%

    // ---- 车窗 / 天窗 ----
    TESLA_CMD_WINDOWS_CLOSE,      // 关闭车窗
    TESLA_CMD_WINDOWS_VENT,       // 车窗通风
    TESLA_CMD_SUNROOF_TOGGLE,     // 天窗（开/关切换）

    // ---- 媒体 ----
    TESLA_CMD_MEDIA_PLAY_PAUSE,   // 播放/暂停
    TESLA_CMD_MEDIA_NEXT,         // 下一曲
    TESLA_CMD_MEDIA_PREV,         // 上一曲
    TESLA_CMD_VOLUME_UP,          // 音量 +
    TESLA_CMD_VOLUME_DOWN,        // 音量 -

    // ---- 安全 / 电源 / 车机 ----
    TESLA_CMD_SENTRY_TOGGLE,      // 哨兵模式
    TESLA_CMD_VALET_TOGGLE,       // 代客模式（固定 1234 PIN）
    TESLA_CMD_VALET_RESET_PIN,    // 重置代客 PIN
    TESLA_CMD_LOW_POWER_TOGGLE,   // 低功耗模式
    TESLA_CMD_ACCESSORY_POWER_TOGGLE, // 保持附件电源
    TESLA_CMD_OTA_CANCEL,         // 取消软件更新
    TESLA_CMD_PING,               // 车机探活
    TESLA_CMD_ERASE_USER_DATA,    // 清除访客数据

    TESLA_CMD_MAX
} tesla_command_t;

/** 命令描述（配置页与钥匙页取名称用） */
typedef struct {
    tesla_command_t cmd;
    const char *name;   // 中文名称
} tesla_command_info_t;

/** 命令表（首项为 NONE，供配置页列出） */
const tesla_command_info_t *tesla_command_table(int *count);

/**
 * @brief 执行一条控制命令（阻塞，最多几秒）
 * @param cmd 命令
 * @return ESP_OK 已发送，ESP_ERR_INVALID_STATE 会话未建立，其他失败
 */
esp_err_t tesla_ble_execute(tesla_command_t cmd);

/**
 * @brief 刷新指定类别的车辆状态（一次 GetVehicleData 可带多类）
 * @param mask TESLA_STATE_BIT_* 组合
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_ERR_TIMEOUT 超时
 */
esp_err_t tesla_ble_refresh_states(uint32_t mask);

/**
 * @brief 获取当前连接状态
 * @return 当前连接状态
 */
tesla_conn_state_t tesla_ble_get_state(void);

/**
 * @brief 获取当前车辆状态
 * @param state 输出车辆状态结构体
 * @return ESP_OK 状态有效，ESP_ERR_INVALID_STATE 状态无效，ESP_ERR_TIMEOUT 超时
 */
esp_err_t tesla_ble_get_vehicle_state(tesla_vehicle_state_t *state);


#ifdef __cplusplus
}
#endif

#endif // TESLA_BLE_H
