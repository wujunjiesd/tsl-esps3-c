/**
 * @file ui_pages.c
 * @brief 仪表界面渲染：驾驶页 / 车钥匙页 / 配网引导页
 *
 * 逻辑分辨率 640x172（横屏）。面板物理是 172x640 竖条，
 * 由 lcd_display.c 在推屏前做整屏转置。
 *
 * 驾驶页布局（左表盘 + 右 3x2 状态卡）：
 *   ┌───────────┬──────────────────────────────────┐
 *   │   速度表盘 │  车门  │ 后备箱 │ 前备箱          │
 *   │   盘心档位 │  充电口│ 胎压   │ 告警            │
 *   └───────────┴──────────────────────────────────┘
 *
 * 功率不再单独占一行读数：只在表盘进度弧上体现（回收时弧变蓝，见 draw_speed_dial）。
 *
 * 注：里程按要求不展示；盘心平时显示实时速度，档位变化时临时显示档位 5 秒。
 *     档位用 app_font_big（字符集需含 PRND），速度两位以内也用 big、三位数换
 *     app_font_mid 以免压到指针（见 tools/gen_fonts.py 的 big_charset）。
 *     连接状态不再单独占一行——能进驾驶页就说明已经连上车机。
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "ui_pages.h"

#define SCR_W BOARD_LCD_H_RES
#define SCR_H BOARD_LCD_V_RES

// ---- 驾驶页：左表盘 + 右信息栏 ----
// 表盘右缘(164) 与分隔线之间留 16px，分隔线再距右侧文字 16px，
// 视觉上左右两块不再挤在一起。
#define SEP_X        180    // 左右分隔线
#define RIGHT_X      196    // 右侧栏左边界
#define RIGHT_R      636    // 右侧栏右边界


#define C_BG        UI_RGB(10, 12, 16)
#define C_CARD      UI_RGB(22, 26, 34)
#define C_CARD_EDGE UI_RGB(38, 46, 58)
#define C_TEXT      UI_RGB(232, 238, 245)
#define C_TEXT_2    UI_RGB(138, 152, 168)
#define C_TEXT_3    UI_RGB(78, 88, 102)
#define C_GREEN     UI_RGB(46, 204, 113)
#define C_BLUE      UI_RGB(74, 158, 255)
#define C_ORANGE    UI_RGB(255, 176, 32)
#define C_RED       UI_RGB(255, 84, 84)
#define C_DIVIDER   UI_RGB(40, 48, 60)
#define C_TRACK     UI_RGB(34, 40, 50)

// ============ 小工具 ============

static uint16_t link_color(lcd_link_state_t link)
{
    switch (link) {
    case LCD_LINK_AUTHENTICATED:
        return C_GREEN;
    case LCD_LINK_CONNECTED:
    case LCD_LINK_CONNECTING:
    case LCD_LINK_SCANNING:
        return C_ORANGE;
    case LCD_LINK_PAIRING:
        return C_BLUE;
    case LCD_LINK_ERROR:
        return C_RED;
    default:
        return C_TEXT_3;
    }
}

/** 档位字母配色：R 提醒、D 正常、P/N 中性（无数据显示灰色） */
static uint16_t gear_color(char g)
{
    switch (g) {
    case 'R':
        return C_ORANGE;
    case 'D':
        return C_GREEN;
    case 'P':
    case 'N':
        return C_TEXT;
    default:
        return C_TEXT_3;
    }
}

static const char *link_text(lcd_link_state_t link)
{
    switch (link) {
    case LCD_LINK_SCANNING:
        return "扫描中";
    case LCD_LINK_CONNECTING:
        return "连接中";
    case LCD_LINK_CONNECTED:
        return "已连接";
    case LCD_LINK_AUTHENTICATED:
        return "已连接";
    case LCD_LINK_PAIRING:
        return "配对中";
    case LCD_LINK_ERROR:
        return "未连接";
    default:
        return "未连接";
    }
}


// ============ 横向椭圆速度表盘 ============

// 屏高只有 172，正圆直径上限 172，占不到屏幕宽度的 3/5；
// 因此用横向椭圆：宽 380（约 640 的 3/5），高 124，居中放置。
// 表盘移到左侧并放到最大：正圆，半径 76（直径 152），
// 纵向 10~162，上下各留 10px；横向 12~164，与右侧分隔线(180)留 16px。
// 扫描角 300°（120° 起 -> 420°=60° 止），底部开口 60°。
#define DIAL_CX         88
#define DIAL_CY         86
#define DIAL_A          76      // 水平半轴
#define DIAL_B          76      // 垂直半轴（与 A 相同 = 正圆）
#define DIAL_SPEED_MAX  200
#define DIAL_ANG_START  120
#define DIAL_ANG_SWEEP  300

static int dial_angle(int v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > DIAL_SPEED_MAX) {
        v = DIAL_SPEED_MAX;
    }
    return DIAL_ANG_START + v * DIAL_ANG_SWEEP / DIAL_SPEED_MAX;
}

static void draw_speed_dial(ui_canvas_t *cv, const lcd_drive_info_t *d,
                            bool center_show_gear)
{
    const int track_w = 5;

    // 轨道
    ui_ellipse_arc(cv, DIAL_CX, DIAL_CY, DIAL_A, DIAL_B, track_w, DIAL_ANG_START,
                   DIAL_ANG_START + DIAL_ANG_SWEEP, C_TRACK);

    // 速度进度弧
    int spd = d->valid ? d->speed_kmh : 0;
    uint16_t acolor = C_GREEN;
    if (d->valid && d->power_kw < 0) {
        acolor = C_BLUE;
    } else if (spd > 150) {
        acolor = C_ORANGE;
    }
    if (spd > 0) {
        ui_ellipse_arc(cv, DIAL_CX, DIAL_CY, DIAL_A, DIAL_B, track_w, DIAL_ANG_START,
                       dial_angle(spd), acolor);
    }

    // 刻度：每 5km/h 一格，每 20km/h 加长加粗
    for (int v = 0; v <= DIAL_SPEED_MAX; v += 5) {
        int deg = dial_angle(v);
        int is_major = (v % 20 == 0);
        ui_ellipse_tick(cv, DIAL_CX, DIAL_CY, DIAL_A, DIAL_B, deg, is_major ? 13 : 7,
                        is_major ? 2 : 1, is_major ? C_TEXT_2 : C_TEXT_3);
    }

    // 刻度数字：每 40km/h 一个，落在椭圆内侧 0.72 处
    for (int v = 0; v <= DIAL_SPEED_MAX; v += 40) {
        int deg = dial_angle(v);
        float a = ui_rad((float)deg);
        int lx = DIAL_CX + (int)lroundf(cosf(a) * (float)DIAL_A * 0.72f);
        int ly = DIAL_CY + (int)lroundf(sinf(a) * (float)DIAL_B * 0.72f);
        char lb[5];
        snprintf(lb, sizeof(lb), "%d", v);
        ui_text_center(cv, lx, ly - 7, &app_font_small, C_TEXT_3, lb);
    }

    // ---- 指针：从盘心外侧往外指，起点避开盘心的档位大字 ----
    {
        int deg = dial_angle(spd);
        float a = ui_rad((float)deg);
        int r_in = (int)(DIAL_A * 0.42f);   // 档位字母的包围盒半径约 26，起点留出空隙
        int r_out = (int)(DIAL_A * 0.66f);
        int x1 = DIAL_CX + (int)lroundf(cosf(a) * (float)r_in);
        int y1 = DIAL_CY + (int)lroundf(sinf(a) * (float)r_in);
        int x2 = DIAL_CX + (int)lroundf(cosf(a) * (float)r_out);
        int y2 = DIAL_CY + (int)lroundf(sinf(a) * (float)r_out);
        uint16_t pcolor = d->valid ? C_RED : C_TEXT_3;
        ui_thick_line(cv, x1, y1, x2, y2, 5, pcolor);
        ui_circle(cv, x2, y2, 3, pcolor);
    }

    // ---- 盘心：档位变化后的 5 秒提示；其余时间显示实时速度 ----
    if (center_show_gear && d->valid && d->gear && d->gear != '-') {
        // 用字形自身的度量居中：按墨水包围盒而不是行高对中，否则字母会偏上/偏下。
        const char gs[2] = {d->gear, '\0'};
        const app_glyph_t *g = app_font_lookup(&app_font_big, (uint32_t)gs[0]);
        int gw = g ? g->w : 24;
        int gh = g ? g->h : 34;
        int gx = DIAL_CX - gw / 2 - (g ? g->bx : 0);
        int gy = DIAL_CY - gh / 2 - (g ? g->by : 0);
        ui_text(cv, gx, gy, &app_font_big, gear_color(gs[0]), gs);
    } else {
        // 实时速度：每帧都取当前 speed_kmh，速度一变盘心数字就跟着刷新。
        // big 是等宽 34px/字，三位数（102px）会压到指针上；两位以内用 big 保持醒目，
        // 三位数切到 mid（18px/字，54px），数字才能完整落在半径 32 的盘心里。
        char sp[6];
        int shown = spd < 0 ? 0 : (spd > 999 ? 999 : spd);
        snprintf(sp, sizeof(sp), "%d", shown);
        const char *txt = d->valid ? sp : "--";
        const app_font_t *f = (shown >= 100) ? &app_font_mid : &app_font_big;
        // 按该字体数字的墨水高度居中：ui_text 的行顶是 y，glyph 画在 y+by。
        const app_glyph_t *g = app_font_lookup(f, (uint32_t)'0');
        int sy = DIAL_CY - (g ? g->h : 22) / 2 - (g ? g->by : 0);
        uint16_t color = !d->valid ? C_TEXT_3 : (spd > 150 ? C_ORANGE : C_TEXT);
        ui_text_center(cv, DIAL_CX, sy, f, color, txt);
    }
}

// ============ 扫描等待页 ============

/**
 * @brief 未连上车机时的首页
 *
 * 配置了 VIN 只代表拿到了车辆标识，并不等于已经连上车辆。
 * 上电后先停在这一页：左边转圈提示“扫描中”，右边给出 VIN 与操作引导；
 * 搜索到车辆 -> 配对/刷卡授权 -> 建立连接之后才切到仪表盘。
 */
#define WAIT_CX     88
#define WAIT_CY     86
#define WAIT_R      58
#define WAIT_THICK  7
#define WAIT_SPAN   100    // 旋转高亮弧长度（度）

static const char *wait_phase_text(lcd_link_state_t link)
{
    switch (link) {
    case LCD_LINK_CONNECTING:
        return "正在连接车辆";
    case LCD_LINK_PAIRING:
        return "请在车机上刷卡授权";
    case LCD_LINK_CONNECTED:
        return "正在读取车辆数据";
    default:
        return "正在搜索车辆蓝牙";
    }
}

static const char *wait_action_text(lcd_link_state_t link)
{
    switch (link) {
    case LCD_LINK_PAIRING:
        return "中控屏点确认后，把卡片放到中控台";
    case LCD_LINK_CONNECTING:
        return "请确认车辆蓝牙已开启";
    default:
        return "连接成功后自动进入仪表盘";
    }
}

static void draw_wait_page(ui_canvas_t *cv, const ui_state_t *st)
{
    // ---- 左侧：保留完整的速度表盘骨架 ----
    // 没连上车机时没有真实数据，但表盘本身要一直在（用户要的是"仪表盘"，不是空白页）。
    // draw_speed_dial 在 valid=false 时画灰指针 + 盘心 "--"，正好当占位。
    lcd_drive_info_t empty = {0};
    empty.valid = false;
    draw_speed_dial(cv, &empty, false);

    // 与驾驶页一致的分隔线
    ui_fill_rect(cv, SEP_X, 10, 1, 152, C_DIVIDER);

    // ---- 右侧第一行：连接阶段 / 配对状态（圆点做呼吸动画表示在扫描）----
    uint16_t lc = link_color(st->link);
    int dot_r = 3 + (int)((st->spin / 30) % 3);
    ui_circle(cv, RIGHT_X + 4, 24, dot_r, lc);
    ui_text(cv, RIGHT_X + 16, 13, &app_font_cjk, lc, link_text(st->link));
    ui_text_right(cv, RIGHT_R, 14, &app_font_cjk, st->paired ? C_GREEN : C_TEXT_3,
                  st->paired ? "已配对" : "未配对");

    // ---- VIN ----
    ui_text(cv, RIGHT_X, 44, &app_font_small, C_TEXT_3, "VIN");
    ui_text(cv, RIGHT_X + 32, 42, &app_font_small, C_TEXT, st->vin[0] ? st->vin : "--");

    // ---- 底部引导卡片 ----
    const int card_x = RIGHT_X;
    const int card_w = RIGHT_R - RIGHT_X;
    ui_card(cv, card_x, 84, card_w, 72, C_CARD, C_CARD_EDGE);

    // 配对阶段用醒目颜色提示去车机刷卡；其余阶段显示当前动作
    uint16_t hint_color = (st->link == LCD_LINK_PAIRING) ? C_ORANGE : C_TEXT;
    const char *hint = st->hint[0] ? st->hint : wait_phase_text(st->link);
    ui_text_center(cv, card_x + card_w / 2, 94, &app_font_cjk, hint_color, hint);
    ui_text_center(cv, card_x + card_w / 2, 126, &app_font_cjk, C_TEXT_3,
                   wait_action_text(st->link));
}

// ============ 驾驶页 ============

// 右侧状态网格：3 列 × 2 行（6 格：车门/后备箱/前备箱/充电口/胎压/告警）。
// 每格做成一张小卡片：左侧状态圆点 + 名称一行 + 状态一行。
// 列宽 142、间距 10：190 + 3*142 + 2*10 = 636，与右边界对齐；
// 卡片高 70、行距 76、起点 6，第二行底边 152，屏高 172 留出下边距。
#define INFO_X0      190
#define INFO_COL_W   142
#define INFO_COL_GAP 10
#define INFO_ROW_H   76
#define INFO_Y0      6
#define INFO_CARD_H  70

/** 语义色 -> 屏幕色（与 tesla_tone_t 对应） */
static uint16_t tone_color(uint8_t tone)
{
    switch (tone) {
    case 1: return C_GREEN;
    case 2: return C_ORANGE;
    case 3: return C_RED;
    case 4: return C_TEXT_3;
    default: return C_TEXT;
    }
}

/**
 * @brief 按显示宽度截断 UTF-8 文本（中文按 2 个宽度算）
 *
 * 卡片只有 142px，像曲名、位置名这种长值不截断会压到隔壁格子。
 */
static const char *fit_text(const char *src, int max_units, char *buf, size_t buf_len)
{
    if (!src || !buf || buf_len == 0) {
        return src;
    }
    int units = 0;
    size_t i = 0;
    while (src[i] && units < max_units) {
        unsigned char c = (unsigned char)src[i];
        int n = 1;
        int w = 1;
        if ((c & 0x80) == 0) {
            n = 1;
            w = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
            w = 1;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
            w = 2;
        } else {
            n = 4;
            w = 2;
        }
        if (units + w > max_units) {
            break;
        }
        // 超长时提前留一个省略号的位置
        if (src[i + n] && units + w + 1 > max_units) {
            break;
        }
        for (int k = 0; k < n && src[i + k]; k++) {
            if (i + k + 1 < buf_len) {
                buf[i + k] = src[i + k];
            }
        }
        i += n;
        units += w;
    }
    if (src[i] && i + 4 < buf_len) {
        buf[i++] = '.';
        buf[i++] = '.';
        buf[i++] = '.';
    }
    buf[i] = '\0';
    return buf;
}

/** 单个状态格：卡片底 + 左侧状态圆点 + 名称 + 数值 */
static void draw_status_cell(ui_canvas_t *cv, int x, int y,
                             const char *name, const char *state, uint16_t color)
{
    char nb[32];
    char vb[40];

    ui_card(cv, x, y, INFO_COL_W, INFO_CARD_H, C_CARD, C_CARD_EDGE);
    ui_circle(cv, x + 13, y + 17, 4, color);
    // 标题必须用中文字库：app_font_small 只打包了 ASCII（见 tools/gen_fonts.py 的
    // FontBuild("small", ..., ASCII)），用它画中文时 app_font_lookup 返回 NULL、
    // 每个字都被跳过，格子看上去就是"只有数值没有标题"。
    // 中文字形 adv≈19px：标题起点 x+25，卡片宽 142，可用约 113px ≈ 12 个单位；
    // 指标名已统一压到 5 个汉字以内，标题和数值都能完整落在卡片里。
    ui_text(cv, x + 25, y + 6, &app_font_cjk, C_TEXT_2,
            fit_text(name, 12, nb, sizeof(nb)));
    // 数值起点更靠左（x+12），可用约 124px，给到 12 个单位比标题略宽一点
    ui_text(cv, x + 12, y + 36, &app_font_cjk, color,
            fit_text(state, 12, vb, sizeof(vb)));
}

static void draw_drive_page(ui_canvas_t *cv, const ui_state_t *st)
{
    const lcd_drive_info_t *d = &st->drive;

    draw_speed_dial(cv, d, st->center_show_gear);

    // 左右分隔线（收短到 150，给底部功率行留白）
    ui_fill_rect(cv, SEP_X, 10, 1, 140, C_DIVIDER);

    // 盘心平时显示实时速度，档位变化时临时显示档位 5 秒；
    // 连接状态不再单独占一行（能进这一页就说明已连上），顶部整行留给状态网格。

    // ---- 状态网格：优先用用户配置的 6 个指标 ----
    const char *cell_names[6];
    const char *cell_states[6];
    uint16_t cell_colors[6];

    if (st->cells_custom) {
        for (int i = 0; i < 6; i++) {
            cell_names[i] = st->cell_name[i];
            cell_states[i] = st->cell_value[i];
            cell_colors[i] = tone_color(st->cell_tone[i]);
        }
    } else {
        // 没配置时的兜底内容：闭件 + 胎压 + 告警
        const char *door_state = !d->body_valid ? "--" : (d->doors_closed ? "正常" : "异常");
        uint16_t door_color = !d->body_valid ? C_TEXT_3 : (d->doors_closed ? C_GREEN : C_ORANGE);

        const char *trunk_state = !d->body_valid ? "--" : (d->trunk_closed ? "已关闭" : "未关闭");
        uint16_t trunk_color = !d->body_valid ? C_TEXT_3 : (d->trunk_closed ? C_GREEN : C_ORANGE);

        const char *frunk_state = !d->body_valid ? "--" : (d->frunk_closed ? "已关闭" : "未关闭");
        uint16_t frunk_color = !d->body_valid ? C_TEXT_3 : (d->frunk_closed ? C_GREEN : C_ORANGE);

        const char *port_state = !d->body_valid ? "--" : (d->charge_port_closed ? "已关闭" : "未关闭");
        uint16_t port_color = !d->body_valid ? C_TEXT_3 : (d->charge_port_closed ? C_GREEN : C_ORANGE);

        // 胎压：取四轮最低值，低于 2.5 bar 视为偏低
        // 必须是栈上缓冲：static 会让 ui_render 不可重入，一旦渲染被多条路径
        // （渲染任务 / 落盘任务）同时调用，字符串会互相覆盖
        char tire[20];
        const char *tire_state = "--";
        uint16_t tire_color = C_TEXT_3;
        if (d->tires_valid) {
            float minv = d->tire_fl;
            if (d->tire_fr < minv) minv = d->tire_fr;
            if (d->tire_rl < minv) minv = d->tire_rl;
            if (d->tire_rr < minv) minv = d->tire_rr;
            bool normal = (minv >= 2.5f);
            snprintf(tire, sizeof(tire), "%.1f %s", (double)minv, normal ? "正常" : "偏低");
            tire_state = tire;
            tire_color = normal ? C_GREEN : C_ORANGE;
        }

        const char *alert_state = !d->alert_valid ? "--" : (d->alert ? "有异常" : "正常");
        uint16_t alert_color = !d->alert_valid ? C_TEXT_3 : (d->alert ? C_RED : C_GREEN);

        static const char *const default_names[6] = {"车门", "后备箱", "前备箱",
                                                     "充电口", "胎压", "告警"};
        cell_names[0] = default_names[0];
        cell_names[1] = default_names[1];
        cell_names[2] = default_names[2];
        cell_names[3] = default_names[3];
        cell_names[4] = default_names[4];
        cell_names[5] = default_names[5];
        cell_states[0] = door_state;
        cell_states[1] = trunk_state;
        cell_states[2] = frunk_state;
        cell_states[3] = port_state;
        cell_states[4] = tire_state;
        cell_states[5] = alert_state;
        cell_colors[0] = door_color;
        cell_colors[1] = trunk_color;
        cell_colors[2] = frunk_color;
        cell_colors[3] = port_color;
        cell_colors[4] = tire_color;
        cell_colors[5] = alert_color;
    }

    // 3 列 × 2 行
    for (int i = 0; i < 6; i++) {
        int x = INFO_X0 + (i % 3) * (INFO_COL_W + INFO_COL_GAP);
        int y = INFO_Y0 + (i / 3) * INFO_ROW_H;
        draw_status_cell(cv, x, y, cell_names[i], cell_states[i], cell_colors[i]);
    }
}

// ============ 配网引导页 ============

static void draw_setup_page(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_text(cv, 12, 8, &app_font_cjk, C_ORANGE, "配置你的设备");

    const int card_y = 34;
    const int card_h = 108;
    const int card_w = 200;
    const int xs[3] = {12, 220, 428};

    for (int i = 0; i < 3; i++) {
        ui_card(cv, xs[i], card_y, card_w, card_h, C_CARD, C_CARD_EDGE);
    }

    // 步骤 1
    ui_text(cv, xs[0] + 10, card_y + 10, &app_font_cjk, C_TEXT_3, "1");
    ui_text(cv, xs[0] + 26, card_y + 10, &app_font_cjk, C_TEXT, "连接热点");
    ui_text_center(cv, xs[0] + card_w / 2, card_y + 44, &app_font_small, C_BLUE,
                   st->ap_ssid[0] ? st->ap_ssid : "ESP32-Setup");
    ui_text_center(cv, xs[0] + card_w / 2, card_y + 72, &app_font_cjk, C_TEXT_3, "连接上述wifi");

    // 步骤 2
    ui_text(cv, xs[1] + 10, card_y + 10, &app_font_cjk, C_TEXT_3, "2");
    ui_text(cv, xs[1] + 26, card_y + 10, &app_font_cjk, C_TEXT, "浏览器访问");
    ui_text_center(cv, xs[1] + card_w / 2, card_y + 44, &app_font_addr, C_GREEN, "192.168.4.1");
    ui_text_center(cv, xs[1] + card_w / 2, card_y + 76, &app_font_cjk, C_TEXT_3, "在网页填写 VIN");

    // 步骤 3
    ui_text(cv, xs[2] + 10, card_y + 10, &app_font_cjk, C_TEXT_3, "3");
    ui_text(cv, xs[2] + 26, card_y + 10, &app_font_cjk, C_TEXT, "等待同步");
    ui_text_center(cv, xs[2] + card_w / 2, card_y + 44, &app_font_cjk, C_TEXT_3, "VIN 共 17 位");
    ui_text_center(cv, xs[2] + card_w / 2, card_y + 64, &app_font_cjk, C_TEXT_3, "可在行驶证");
    ui_text_center(cv, xs[2] + card_w / 2, card_y + 84, &app_font_cjk, C_TEXT_3, "或中控查看");

    // 底部提示只在有实际反馈时出现（保存成功 / 热点启动失败），平时不占位
    if (st->hint[0]) {
        ui_text(cv, 12, 148, &app_font_cjk, C_GREEN, st->hint);
    }
}

// ============ 车钥匙页 ============

// 640 宽：4 + 4*152 + 3*8 + 4 = 640
// 172 高：24 + 124 + 24 = 172，按钮上下留白相等
// 按键反馈不再在按钮下方写字，改成按钮自身的下沉/回弹动效（见 draw_key_page）
#define KEY_BTN_W    152
#define KEY_BTN_H    124
#define KEY_BTN_Y    24
#define KEY_BTN_X0   4
#define KEY_BTN_GAP  8

// 按下动效：卡片整体下沉的像素数（屏高只有 172，下沉再多会盖掉上下留白）
#define KEY_BTN_SINK 5

// 按下时卡片底色/描边提亮，和静止态形成明确对比（一帧就能看出来）
#define C_CARD_PRESS UI_RGB(38, 56, 82)
#define C_PRESS_EDGE UI_RGB(110, 176, 255)

// 按钮数量必须与 lcd_display 的槽位数一致，不要在这里另起一份常量，
// 否则改一边另一边会悄悄越界
_Static_assert(LCD_KEY_SLOTS == 4, "KEY_BTN_* 布局按 4 个按钮排布");

/** 按钮在屏幕上的左边界 */
static int ui_key_button_x(int index)
{
    return KEY_BTN_X0 + index * (KEY_BTN_W + KEY_BTN_GAP);
}

/** 命中测试：逻辑坐标 (x, y) 落在哪个按钮上，返回 -1 表示没命中 */
int ui_key_button_hit(int x, int y)
{
    if (y < KEY_BTN_Y || y > KEY_BTN_Y + KEY_BTN_H) {
        return -1;
    }
    for (int i = 0; i < LCD_KEY_SLOTS; i++) {
        int bx = ui_key_button_x(i);
        if (x >= bx && x <= bx + KEY_BTN_W) {
            return i;
        }
    }
    return -1;
}

/** 锁形图标（closed=false 时锁梁留缺口，表示解锁） */
static void draw_lock_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color, bool closed)
{
    const int w = 34;
    const int h = 26;
    const int body_y = cy + 3;

    ui_fill_round_rect(cv, cx - w / 2, body_y, w, h, 4, color);
    // 锁梁：0°=右，90°=下；180°→360° 是上半圆
    ui_arc(cv, cx, body_y, 12, 3, 180, closed ? 360 : 320, color);
}

/** 前/后备箱图标：向上箭头 + 车体示意 */
static void draw_hatch_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color, bool rear)
{
    // 车体
    if (rear) {
        ui_thick_line(cv, cx - 20, cy + 14, cx + 20, cy + 14, 2, color);
        ui_thick_line(cv, cx - 20, cy + 14, cx - 20, cy + 4, 2, color);
        ui_thick_line(cv, cx + 20, cy + 14, cx + 20, cy + 4, 2, color);
    } else {
        ui_thick_line(cv, cx - 20, cy + 14, cx + 20, cy + 14, 2, color);
        ui_thick_line(cv, cx - 20, cy + 14, cx - 14, cy + 2, 2, color);
        ui_thick_line(cv, cx + 20, cy + 14, cx + 14, cy + 2, 2, color);
    }

    // 掀起箭头
    ui_thick_line(cv, cx, cy + 6, cx, cy - 14, 2, color);
    ui_thick_line(cv, cx - 8, cy - 6, cx, cy - 14, 2, color);
    ui_thick_line(cv, cx + 8, cy - 6, cx, cy - 14, 2, color);
}

/** 通用图标：圆环 + 闪电，用于没有专属图形的命令 */
static void draw_generic_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color)
{
    for (int r = 14; r <= 15; r++) {
        for (int a = 0; a < 360; a += 6) {
            int x, y;
            ui_polar(cx, cy, r, (float)a, &x, &y);
            ui_fill_rect(cv, x, y, 2, 2, color);
        }
    }
    // 闪电
    ui_thick_line(cv, cx + 3, cy - 10, cx - 4, cy + 1, 2, color);
    ui_thick_line(cv, cx - 4, cy + 1, cx + 2, cy + 1, 2, color);
    ui_thick_line(cv, cx + 2, cy + 1, cx - 3, cy + 11, 2, color);
}

/** 空调图标：三条波浪线 */
static void draw_air_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color)
{
    for (int i = 0; i < 3; i++) {
        int y = cy - 8 + i * 8;
        ui_thick_line(cv, cx - 12, y, cx - 4, y - 4, 2, color);
        ui_thick_line(cv, cx - 4, y - 4, cx + 4, y + 4, 2, color);
        ui_thick_line(cv, cx + 4, y + 4, cx + 12, y, 2, color);
    }
}

/** 灯光图标：星芒 */
static void draw_light_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color)
{
    ui_circle(cv, cx, cy, 5, color);
    for (int a = 0; a < 360; a += 45) {
        int x1, y1, x2, y2;
        ui_polar(cx, cy, 10, (float)a, &x1, &y1);
        ui_polar(cx, cy, 15, (float)a, &x2, &y2);
        ui_thick_line(cv, x1, y1, x2, y2, 2, color);
    }
}

/** 充电图标：插头 */
static void draw_plug_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color)
{
    ui_thick_line(cv, cx - 8, cy - 12, cx - 8, cy - 4, 2, color);
    ui_thick_line(cv, cx + 8, cy - 12, cx + 8, cy - 4, 2, color);
    ui_thick_line(cv, cx - 11, cy - 4, cx + 11, cy - 4, 2, color);
    ui_thick_line(cv, cx, cy - 4, cx, cy + 12, 3, color);
}

/** 媒体图标：音符 */
static void draw_note_icon(ui_canvas_t *cv, int cx, int cy, uint16_t color)
{
    ui_thick_line(cv, cx + 2, cy - 12, cx + 2, cy + 6, 2, color);
    ui_thick_line(cv, cx + 2, cy - 12, cx + 12, cy - 8, 2, color);
    ui_thick_line(cv, cx + 12, cy - 8, cx + 12, cy + 4, 2, color);
    ui_circle(cv, cx - 2, cy + 8, 5, color);
    ui_circle(cv, cx + 8, cy + 6, 5, color);
}

/** 按钮图标（cx, cy 为图标中心）；lit = 按钮正处于按下态，次要图形一并提亮 */
static void draw_key_icon(ui_canvas_t *cv, uint8_t icon, int cx, int cy, bool lit)
{
    uint16_t secondary = lit ? C_TEXT : C_TEXT_2;

    switch (icon) {
    case LCD_ICON_LOCK:
        draw_lock_icon(cv, cx, cy, C_TEXT, true);
        break;
    case LCD_ICON_TRUNK:
        draw_hatch_icon(cv, cx, cy, secondary, true);
        break;
    case LCD_ICON_FRUNK:
        draw_hatch_icon(cv, cx, cy, secondary, false);
        break;
    case LCD_ICON_CLIMATE:
        draw_air_icon(cv, cx, cy, C_TEXT);
        break;
    case LCD_ICON_LIGHT:
        draw_light_icon(cv, cx, cy, C_TEXT);
        break;
    case LCD_ICON_CHARGE:
        draw_plug_icon(cv, cx, cy, C_GREEN);
        break;
    case LCD_ICON_MEDIA:
        draw_note_icon(cv, cx, cy, C_TEXT);
        break;
    default:
        draw_generic_icon(cv, cx, cy, secondary);
        break;
    }
}

static void draw_key_page(ui_canvas_t *cv, const ui_state_t *st)
{
    // 按键反馈不再在下方写一行字（"锁车 已发送" 之类），改成按钮自身的
    // 下沉 + 提亮动效：按下的那一个按钮整块往下压，底色和描边亮起来，下一帧弹回原位。
    // （双击/左右滑动屏幕可返回仪表盘）
    for (int i = 0; i < LCD_KEY_SLOTS; i++) {
        int x = ui_key_button_x(i);
        bool lit = (st->key_press == i);

        // 下沉后卡片顶边让出的缝隙露出的就是页面底色，视觉上等于"按进去了"；
        // 底部多出来的几像素落在按钮下方的留白里，不会碰到别的内容。
        int y = lit ? KEY_BTN_Y + KEY_BTN_SINK : KEY_BTN_Y;
        ui_card(cv, x, y, KEY_BTN_W, KEY_BTN_H, lit ? C_CARD_PRESS : C_CARD,
                lit ? C_PRESS_EDGE : C_CARD_EDGE);

        int cx = x + KEY_BTN_W / 2;
        draw_key_icon(cv, st->key_icon[i], cx, y + KEY_BTN_H / 2 - 18, lit);

        char lb[16];
        ui_text_center(cv, cx, y + KEY_BTN_H - 34, &app_font_cjk,
                       i == 0 ? C_ORANGE : C_TEXT,
                       fit_text(st->key_label[i], 10, lb, sizeof(lb)));
    }
}

// ============ 入口 ============

void ui_render(ui_canvas_t *cv, const ui_state_t *st)
{
    ui_fill(cv, C_BG);

    if (st->setup_mode) {
        draw_setup_page(cv, st);
        return;
    }

    // 车钥匙页：双击切换进入，任何连接状态下都可用
    if (st->mode == LCD_MODE_KEY) {
        draw_key_page(cv, st);
        return;
    }

    // 只有真正连上车机后才进入仪表盘；
    // 没连上之前一律停在扫描等待页（有 VIN 也不显示表盘）。
    if (st->link != LCD_LINK_CONNECTED && st->link != LCD_LINK_AUTHENTICATED) {
        draw_wait_page(cv, st);
        return;
    }

    // 仪表盘只有驾驶页一屏：切模式靠左右滑动，轻触不再翻页
    draw_drive_page(cv, st);
}
