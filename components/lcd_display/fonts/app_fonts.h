/* 自动生成，请勿手工修改 —— 由 tools/gen_fonts.py 生成 */
#ifndef APP_FONTS_H
#define APP_FONTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t cp;        /* 码点 */
    uint32_t off;      /* 位图在 bitmap 中的字节偏移 */
    uint8_t w;         /* 位图宽 */
    uint8_t h;         /* 位图高 */
    uint8_t stride;    /* 每行字节数（4bpp 打包，低 nibble 为左侧像素） */
    uint8_t adv;       /* 步进宽度 */
    int8_t bx;         /* 相对笔位置的水平偏移 */
    int8_t by;         /* 相对行顶部的垂直偏移 */
} app_glyph_t;

typedef struct {
    const uint8_t *bitmap;
    const app_glyph_t *glyphs;
    uint16_t count;
    uint16_t height;       /* 紧致包围盒高度 */
    uint16_t line_height;  /* 行高 */
} app_font_t;

extern const app_font_t app_font_big;
extern const app_font_t app_font_mid;
extern const app_font_t app_font_addr;
extern const app_font_t app_font_small;
extern const app_font_t app_font_cjk;
const app_glyph_t *app_font_lookup(const app_font_t *font, uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif /* APP_FONTS_H */
