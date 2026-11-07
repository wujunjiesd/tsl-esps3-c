/**
 * @file ui_draw.h
 * @brief 轻量绘图基元（RGB565 画布 + 4bpp 抗锯齿字体）
 */

#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_RGB(r, g, b) \
    ((uint16_t)(((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | \
                 ((uint16_t)(b) >> 3))))

#define UI_PI 3.14159265358979f

/** 角度转弧度 */
static inline float ui_rad(float deg)
{
    return deg * (UI_PI / 180.0f);
}

/** 极坐标落到画布：deg 为角度（0° 指向 +x，顺时针增大），r 为半径 */
static inline void ui_polar(int cx, int cy, int r, float deg, int *x, int *y)
{
    float a = ui_rad(deg);
    *x = cx + (int)lroundf(cosf(a) * (float)r);
    *y = cy + (int)lroundf(sinf(a) * (float)r);
}

typedef struct {
    uint16_t *buf;  // RGB565，行优先
    int16_t w;
    int16_t h;
} ui_canvas_t;

void ui_fill(ui_canvas_t *c, uint16_t color);
void ui_fill_rect(ui_canvas_t *c, int x, int y, int w, int h, uint16_t color);
void ui_fill_round_rect(ui_canvas_t *c, int x, int y, int w, int h, int r, uint16_t color);
void ui_card(ui_canvas_t *c, int x, int y, int w, int h, uint16_t fill, uint16_t border);
void ui_thick_line(ui_canvas_t *c, int x0, int y0, int x1, int y1, int thickness, uint16_t color);
void ui_circle(ui_canvas_t *c, int cx, int cy, int r, uint16_t color);

/**
 * @brief 画一段圆弧
 * @param r         圆弧中心线半径
 * @param thickness 线宽（像素）
 * @param start_deg 起始角度，0°=右，90°=下（屏幕坐标系），135°=左下
 * @param end_deg   结束角度，需大于起始角
 */
void ui_arc(ui_canvas_t *c, int cx, int cy, int r, int thickness, int start_deg, int end_deg,
            uint16_t color);


/**
 * @brief 画一段椭圆弧（横屏表盘用，宽高比大时比正圆更贴合屏幕）
 * @param a         水平半轴
 * @param b         垂直半轴
 * @param thickness 线宽
 */
void ui_ellipse_arc(ui_canvas_t *c, int cx, int cy, int a, int b, int thickness, int start_deg,
                    int end_deg, uint16_t color);

/**
 * @brief 椭圆刻度线：从椭圆外沿沿法线向内画 len 像素
 * @note  用固定像素长度而不是按半轴比例缩放，否则长轴/短轴方向刻度长度差太多
 */
void ui_ellipse_tick(ui_canvas_t *c, int cx, int cy, int a, int b, int deg, int len,
                     int thickness, uint16_t color);

int ui_text_width(const app_font_t *f, const char *s);
void ui_text(ui_canvas_t *c, int x, int y, const app_font_t *f, uint16_t color, const char *s);
void ui_text_center(ui_canvas_t *c, int cx, int y, const app_font_t *f, uint16_t color,
                    const char *s);
void ui_text_right(ui_canvas_t *c, int right_x, int y, const app_font_t *f, uint16_t color,
                   const char *s);

/** 按最大宽度截断 UTF-8 字符串（保证不在多字节字符中间截断） */


#ifdef __cplusplus
}
#endif

#endif // UI_DRAW_H
