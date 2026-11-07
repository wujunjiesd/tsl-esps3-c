/**
 * @file lcd_display.h
 * @brief 仪表显示模块（AXS15231B 172x640 QSPI）
 */

#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 仪表盘格子数 / 车钥匙按钮数 */
#define LCD_CELL_COUNT 6
#define LCD_KEY_SLOTS  4

/** 与车辆的连接状态（用于状态栏显示） */
typedef enum {
    LCD_LINK_DISCONNECTED = 0,
    LCD_LINK_SCANNING,
    LCD_LINK_CONNECTING,
    LCD_LINK_CONNECTED,
    LCD_LINK_AUTHENTICATED,
    LCD_LINK_PAIRING,
    LCD_LINK_ERROR,
} lcd_link_state_t;

/** 驾驶数据（-1 表示未知） */
typedef struct {
    int power_kw;
    int speed_kmh;
    char gear;           // 'P' / 'R' / 'N' / 'D' / '-'
    bool valid;          // 数据是否有效
    int odometer_km;     // 总里程
    int consumption_10;  // 能耗 kWh/100km * 10
    int eta_min;         // 剩余到达时间（分钟）
    char destination[40];

    // ---- 车身闭件状态（VCSEC，车机休眠也能读）----
    bool body_valid;          // 闭件数据是否有效
    bool doors_closed;        // 四门全部关闭
    bool frunk_closed;        // 前备箱关闭
    bool trunk_closed;        // 后备箱关闭
    bool charge_port_closed;  // 充电口关闭

    // ---- 胎压（bar）----
    bool tires_valid;
    float tire_fl;
    float tire_fr;
    float tire_rl;
    float tire_rr;

    // ---- 异常告警（由闭件未关 + 轮胎硬告警汇总而来）----
    bool alert_valid;  // 告警数据是否有效
    bool alert;        // true = 存在异常
} lcd_drive_info_t;

/** 初始化屏幕、触摸与背光 */
esp_err_t lcd_display_init(void);

/** 设置连接状态 */
void lcd_display_set_link(lcd_link_state_t state);

/** 更新驾驶数据（非阻塞） */
void lcd_display_set_drive(const lcd_drive_info_t *info);

/** 设置 VIN（用于等待页显示） */
void lcd_display_set_vin(const char *vin);

/** 设置配对状态（用于等待页显示） */
void lcd_display_set_paired(bool paired);

/** 设置底部提示文字（支持 UTF-8 中文） */
void lcd_display_set_hint(const char *hint);

/** 配网模式：显示热点名称与访问地址 */
void lcd_display_set_setup_mode(bool enable, const char *ap_ssid);

/** 开关背光（息屏/亮屏） */
void lcd_display_set_backlight(bool on);

/** 背光当前是否点亮 */
bool lcd_display_backlight_is_on(void);

/** 屏幕工作模式 */
typedef enum {
    LCD_MODE_DASHBOARD = 0,   // 仪表盘（默认）
    LCD_MODE_KEY,             // 车钥匙按键页（4 个按钮可配置）
} lcd_ui_mode_t;

/** 切换工作模式（双击/左右滑动屏幕时由触摸任务调用） */
void lcd_display_set_mode(lcd_ui_mode_t mode);

/** 当前工作模式 */
lcd_ui_mode_t lcd_display_get_mode(void);

/**
 * @brief 注册模式变化回调（双击或左右滑动切模式时触发）
 *
 * 业务层在这里把模式写进 NVS，下次开机用以恢复上次停留的页面。
 */
void lcd_display_set_mode_change_cb(void (*cb)(lcd_ui_mode_t mode));

/**
 * @brief 取走一次车钥匙按键动作
 * @return 0..LCD_KEY_SLOTS-1 表示被按下的按钮槽位，-1 表示没有待处理动作
 *
 * 槽位对应的具体命令由业务层按配置决定，显示层不关心命令语义。
 */
int lcd_display_take_key_slot(void);

/**
 * @brief 设置仪表盘 6 个格子显示什么
 * @param names  每格标题（可传 NULL 表示用内置默认内容）
 * @param values 每格数值文本
 * @param tones  每格语义色 0 普通 1 好 2 警告 3 坏 4 次要
 *
 * 传入 NULL 恢复内置的"车门/后备箱/前备箱/充电口/胎压/告警"六格。
 */
void lcd_display_set_cells(const char *names[LCD_CELL_COUNT],
                           const char *values[LCD_CELL_COUNT],
                           const uint8_t tones[LCD_CELL_COUNT]);

/** 车钥匙页按钮图标 */
typedef enum {
    LCD_ICON_GENERIC = 0,   // 通用（闪电）
    LCD_ICON_LOCK,          // 车锁
    LCD_ICON_TRUNK,         // 后备箱
    LCD_ICON_FRUNK,         // 前备箱
    LCD_ICON_CLIMATE,       // 空调
    LCD_ICON_LIGHT,         // 灯光/喇叭
    LCD_ICON_CHARGE,        // 充电
    LCD_ICON_MEDIA,         // 媒体
} lcd_icon_t;

/**
 * @brief 设置车钥匙页 4 个按钮的图标与名字
 * @param icons  图标类型（lcd_icon_t），可传 NULL 表示都用通用图标
 * @param labels 按钮文字
 */
void lcd_display_set_key_buttons(const uint8_t icons[LCD_KEY_SLOTS],
                                 const char *labels[LCD_KEY_SLOTS]);

/**
 * @brief 取走双击事件计数（自上次调用以来发生了几次双击）
 * @return 0 表示没有新的双击；业务层据此切换“仪表盘 / 车钥匙”模式
 */
uint32_t lcd_display_take_double_tap(void);

/** 强制刷新一次 */
void lcd_display_refresh(void);


#ifdef __cplusplus
}
#endif

#endif // LCD_DISPLAY_H
