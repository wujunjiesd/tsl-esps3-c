/**
 * @file ui_pages.h
 * @brief 仪表界面状态与渲染（组件内部使用）
 */

#ifndef UI_PAGES_H
#define UI_PAGES_H

#include <stdbool.h>
#include <stdint.h>

#include "lcd_display.h"
#include "ui_draw.h"

typedef struct {
    lcd_link_state_t link;
    lcd_drive_info_t drive;
    char vin[18];
    char hint[48];
    bool setup_mode;
    char ap_ssid[33];
    bool paired;
    uint16_t spin;       // 等待页旋转指示环的角度（由显示任务按时间推进）
    lcd_ui_mode_t mode;  // 仪表盘 / 车钥匙页
    // ---- 车钥匙页按下动效（由显示任务按帧推进）----
    int8_t key_press;   // 这一帧要画成"按下"的槽位，-1 = 无
    // ---- 车钥匙页 4 个按钮（可在配置页任意指定命令）----
    char key_label[LCD_KEY_SLOTS][16];
    uint8_t key_icon[LCD_KEY_SLOTS];   // lcd_icon_t
    // ---- 仪表盘 6 个格子（可在配置页任意指定指标）----
    // 名称按 UTF-8 存放：5 个汉字就是 15 字节，16 字节会截掉第 6 个字，
    // 值也留出比来源 text[28] 更宽的余量，避免 UI 层二次截断。
    bool cells_custom;                // true = 用下面的自定义内容
    char cell_name[LCD_CELL_COUNT][28];
    char cell_value[LCD_CELL_COUNT][32];
    uint8_t cell_tone[LCD_CELL_COUNT];
    // 表盘盘心：true = 档位变化后的 5 秒提示，false = 实时速度
    bool center_show_gear;
} ui_state_t;

/** 渲染当前页面到画布 */
void ui_render(ui_canvas_t *cv, const ui_state_t *st);

/**
 * @brief 车钥匙页按钮命中测试（逻辑坐标）
 * @return 0..3 = 锁车/解锁/前备箱/后备箱，-1 = 未命中
 */
int ui_key_button_hit(int x, int y);

/**
 * @brief 车钥匙页“返回”按钮命中测试（逻辑坐标）
 * @return true 表示点在返回按钮上，应切回仪表盘
 */

#endif // UI_PAGES_H
