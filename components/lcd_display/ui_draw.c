/**
 * @file ui_draw.c
 * @brief 轻量绘图基元实现
 */

#include <math.h>
#include <string.h>

#include "ui_draw.h"

// ============ 基础图形 ============

void ui_fill(ui_canvas_t *c, uint16_t color)
{
    if (!c || !c->buf) {
        return;
    }
    uint32_t n = (uint32_t)c->w * c->h;
    uint32_t v = ((uint32_t)color << 16) | color;
    uint32_t *p = (uint32_t *)c->buf;
    for (uint32_t i = 0; i < n / 2; i++) {
        p[i] = v;
    }
    if (n & 1) {
        c->buf[n - 1] = color;
    }
}

void ui_fill_rect(ui_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (!c || !c->buf || w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int row = y0; row < y1; row++) {
        uint16_t *p = c->buf + (uint32_t)row * c->w;
        for (int col = x0; col < x1; col++) {
            p[col] = color;
        }
    }
}



void ui_fill_round_rect(ui_canvas_t *c, int x, int y, int w, int h, int r, uint16_t color)
{
    if (r <= 0) {
        ui_fill_rect(c, x, y, w, h, color);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    // 中间横条
    ui_fill_rect(c, x, y + r, w, h - 2 * r, color);
    // 上下两个圆角矩形（用圆填充 + 横条近似）
    for (int i = 0; i < r; i++) {
        // 由圆心向上/向下每行计算宽度
        int dy = r - i;
        int dx = r;
        // 求 sqrt(r^2 - dy^2) 的整数近似
        while (dx > 0 && dx * dx + dy * dy > r * r) {
            dx--;
        }
        int row_top = y + i;
        int row_bottom = y + h - 1 - i;
        ui_fill_rect(c, x + r - dx, row_top, w - 2 * (r - dx), 1, color);
        if (row_bottom != row_top) {
            ui_fill_rect(c, x + r - dx, row_bottom, w - 2 * (r - dx), 1, color);
        }
    }
}

void ui_circle(ui_canvas_t *c, int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = r;
        while (dx > 0 && dx * dx + dy * dy > r * r) {
            dx--;
        }
        ui_fill_rect(c, cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

void ui_thick_line(ui_canvas_t *c, int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    if (!c || !c->buf || thickness <= 0) {
        return;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) {
        steps = 1;
    }
    int half = thickness / 2;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;
        ui_fill_rect(c, x - half, y - half, thickness, thickness, color);
    }
}

void ui_arc(ui_canvas_t *c, int cx, int cy, int r, int thickness, int start_deg, int end_deg,
            uint16_t color)
{
    // 圆弧就是长短半轴相等的椭圆弧，直接转发，别再各自维护一份采样循环
    ui_ellipse_arc(c, cx, cy, r, r, thickness, start_deg, end_deg, color);
}


void ui_ellipse_arc(ui_canvas_t *c, int cx, int cy, int a, int b, int thickness, int start_deg,
                    int end_deg, uint16_t color)
{
    if (!c || !c->buf || a <= 0 || b <= 0 || thickness <= 0) {
        return;
    }
    if (end_deg < start_deg) {
        int t = start_deg;
        start_deg = end_deg;
        end_deg = t;
    }
    if (end_deg - start_deg > 360) {
        end_deg = start_deg + 360;
    }

    int span = end_deg - start_deg;
    int steps = span * 2;
    if (steps < 1) {
        steps = 1;
    }
    int half = thickness / 2;

    for (int i = 0; i <= steps; i++) {
        float deg = (float)start_deg + (float)span * (float)i / (float)steps;
        float ang = ui_rad((float)deg);
        int x = cx + (int)lroundf(cosf(ang) * (float)a);
        int y = cy + (int)lroundf(sinf(ang) * (float)b);
        ui_fill_rect(c, x - half, y - half, thickness, thickness, color);
    }
}

void ui_ellipse_tick(ui_canvas_t *c, int cx, int cy, int a, int b, int deg, int len,
                     int thickness, uint16_t color)
{
    if (!c || !c->buf || len <= 0) {
        return;
    }
    float ang = ui_rad((float)deg);
    float ca = cosf(ang);
    float sa = sinf(ang);

    // 外沿点
    float ox = (float)cx + ca * (float)a;
    float oy = (float)cy + sa * (float)b;

    // 椭圆在该点的外法线方向：切线为 (-a*sin, b*cos)，法线取 (b*cos, a*sin)
    float nx = (float)b * ca;
    float ny = (float)a * sa;
    float nl = sqrtf(nx * nx + ny * ny);
    if (nl < 0.001f) {
        nl = 0.001f;
    }
    nx /= nl;
    ny /= nl;

    int x0 = (int)lroundf(ox);
    int y0 = (int)lroundf(oy);
    int x1 = (int)lroundf(ox - nx * (float)len);
    int y1 = (int)lroundf(oy - ny * (float)len);
    ui_thick_line(c, x0, y0, x1, y1, thickness, color);
}

void ui_card(ui_canvas_t *c, int x, int y, int w, int h, uint16_t fill, uint16_t border)
{
    ui_fill_round_rect(c, x, y, w, h, 8, fill);
    if (border) {
        // 顶边高亮线条，视觉上更接近“卡片”
        ui_fill_rect(c, x + 8, y, w - 16, 1, border);
    }
}

// ============ 文本 ============

static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp;
    if (p[0] < 0x80) {
        cp = p[0];
        *s += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
             ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *s += 4;
    } else {
        cp = '?';
        *s += 1;
    }
    return cp;
}

const app_glyph_t *app_font_lookup(const app_font_t *font, uint32_t cp)
{
    if (!font || !font->glyphs) {
        return NULL;
    }
    if (cp == '\t') {
        cp = ' ';
    }
    // 字形表按码点升序排列，二分查找
    int lo = 0;
    int hi = (int)font->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int32_t v = font->glyphs[mid].cp;
        if (v == (int32_t)cp) {
            return &font->glyphs[mid];
        }
        if (v < (int32_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

int ui_text_width(const app_font_t *f, const char *s)
{
    if (!f || !s) {
        return 0;
    }
    int x = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const app_glyph_t *g = app_font_lookup(f, cp);
        x += g ? g->adv : (f->height / 2);
    }
    return x;
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    if (a == 0) {
        return dst;
    }
    if (a >= 15) {
        return src;
    }
    uint16_t dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    uint16_t sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    uint16_t r = (uint16_t)((dr * (15 - a) + sr * a) / 15);
    uint16_t g = (uint16_t)((dg * (15 - a) + sg * a) / 15);
    uint16_t b = (uint16_t)((db * (15 - a) + sb * a) / 15);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_glyph(ui_canvas_t *c, int x, int y, const app_font_t *f, const app_glyph_t *g,
                       uint16_t color)
{
    if (!g || g->w == 0 || g->h == 0) {
        return;
    }
    int gx = x + g->bx;
    int gy = y + g->by;

    for (int row = 0; row < g->h; row++) {
        int py = gy + row;
        if (py < 0 || py >= c->h) {
            continue;
        }
        uint16_t *line = c->buf + (uint32_t)py * c->w;
        const uint8_t *src = f->bitmap + g->off + (uint32_t)row * g->stride;
        for (int col = 0; col < g->w; col++) {
            int px = gx + col;
            if (px < 0 || px >= c->w) {
                continue;
            }
            // 4bpp 打包方式（见 tools/gen_fonts.py emit()）：
            //   偶数像素 -> 低 nibble，奇数像素 -> 高 nibble
            // 早期这里写反了，导致每个 2 像素对左右互换，笔画错位、字发虚
            uint8_t byte = src[col >> 1];
            uint8_t a = (col & 1) ? (byte >> 4) : (byte & 0x0F);
            if (a) {
                line[px] = blend565(line[px], color, a);
            }
        }
    }
}

void ui_text(ui_canvas_t *c, int x, int y, const app_font_t *f, uint16_t color, const char *s)
{
    if (!c || !c->buf || !f || !s) {
        return;
    }
    const char *p = s;
    int pen = x;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const app_glyph_t *g = app_font_lookup(f, cp);
        if (!g) {
            pen += f->height / 2;
            continue;
        }
        draw_glyph(c, pen, y, f, g, color);
        pen += g->adv;
    }
}

void ui_text_center(ui_canvas_t *c, int cx, int y, const app_font_t *f, uint16_t color,
                    const char *s)
{
    ui_text(c, cx - ui_text_width(f, s) / 2, y, f, color, s);
}

void ui_text_right(ui_canvas_t *c, int right_x, int y, const app_font_t *f, uint16_t color,
                   const char *s)
{
    ui_text(c, right_x - ui_text_width(f, s), y, f, color, s);
}


