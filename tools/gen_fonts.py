#!/usr/bin/env python3
"""
字库生成工具（4bpp 抗锯齿位图字体）

用法:
    python3 tools/gen_fonts.py

生成:
    components/lcd_display/fonts/app_fonts.h
    components/lcd_display/fonts/font_data.c

说明:
    从系统 TTF 渲染 ASCII / 中文子集，打包为 4bpp 灰度位图（0=透明, 15=实心）。
    中文字符集 = CJK_CHARS + 自动扫描 components/ 下 C 源码字符串中出现的所有中文字符，
    以控制 Flash 占用的同时避免漏字。
"""

import glob
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT_DIR, "components", "lcd_display", "fonts")

# ---- 字体候选路径（macOS 常见字体，按顺序尝试） ----
MONO_CANDIDATES = [
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/Library/Fonts/Courier New.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
]
CJK_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
]

ASCII = "".join(chr(c) for c in range(32, 127))

# 额外保底字符（一般留空：中文字符会自动扫描源码收集）
CJK_CHARS = ""


COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def collect_source_chars():
    """扫描 C 源码（去掉注释）中出现的中文字符，避免界面漏字"""
    chars = set()
    for path in glob.glob(os.path.join(ROOT_DIR, "components", "**", "*.[ch]"), recursive=True):
        try:
            with open(path, encoding="utf-8", errors="ignore") as f:
                src = f.read()
        except OSError:
            continue
        src = COMMENT_RE.sub("", src)
        for ch in src:
            if ord(ch) > 127:
                chars.add(ch)
    return chars


def load_font(candidates, size):
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size), path
            except OSError:
                continue
    raise SystemExit("找不到可用字体: %s" % candidates)


def render_glyph(font, ch, pad=8):
    """返回 (w, h, bx, by, rows) —— rows 为 4bpp 灰度行数据 (nibble 列表)"""
    adv = max(1, int(round(font.getlength(ch))))
    ascent, descent = font.getmetrics()
    canvas_w = adv + 2 * pad + 4
    canvas_h = ascent + descent + 2 * pad + 4
    img = Image.new("L", (canvas_w, canvas_h), 0)
    ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=255, anchor="la")
    bbox = img.getbbox()
    if not bbox:
        return 0, 0, 0, 0, []
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    bx, by = x0 - pad, y0 - pad
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            v = img.getpixel((x0 + x, y0 + y))
            # 轻微增强对比度，让细笔画在低分辨率屏上更清晰
            a = v * 15 // 255
            if a < 3:
                a = 0
            row.append(a)
        rows.append(row)
    return w, h, bx, by, rows


class FontBuild:
    def __init__(self, name, font, charset):
        self.name = name
        self.font = font
        self.charset = charset
        self.glyphs = []  # (cp, w, h, bx, by, adv, rows)

    def build(self):
        ascent, descent = self.font.getmetrics()
        for ch in self.charset:
            w, h, bx, by, rows = render_glyph(self.font, ch)
            adv = int(round(self.font.getlength(ch)))
            if adv <= 0:
                adv = w
            self.glyphs.append([ord(ch), w, h, bx, by, adv, rows])
        self.line_height = ascent + descent
        return self


def emit(fonts, out_c, out_h):
    bitmap = bytearray()
    tables = []

    for fb in fonts:
        glyphs = [g for g in fb.glyphs]
        # 统一到紧致包围盒：所有字形向上平移 min_y
        ys = [g[4] for g in glyphs if g[1] > 0 and g[2] > 0]
        min_y = min(ys) if ys else 0
        max_y = max(g[4] + g[2] for g in glyphs if g[1] > 0 and g[2] > 0) if ys else 0
        height = max_y - min_y

        entries = []
        for cp, w, h, bx, by, adv, rows in sorted(glyphs, key=lambda g: g[0]):
            off = len(bitmap)
            stride = (w + 1) // 2
            for y in range(h):
                row = rows[y]
                for xb in range(stride):
                    lo = row[2 * xb] if 2 * xb < w else 0
                    hi = row[2 * xb + 1] if 2 * xb + 1 < w else 0
                    bitmap.append((lo & 0xF) | ((hi & 0xF) << 4))
            entries.append((cp, off, w, h, stride, adv, bx, by - min_y))
        tables.append((fb, entries, height))

    lines = []
    lines.append('/* 自动生成，请勿手工修改 —— 由 tools/gen_fonts.py 生成 */')
    lines.append('')
    lines.append('#include "app_fonts.h"')
    lines.append('')
    lines.append('static const uint8_t s_bitmap[] = {')
    for i in range(0, len(bitmap), 16):
        chunk = ", ".join("0x%02x" % b for b in bitmap[i:i + 16])
        lines.append("    " + chunk + ",")
    lines.append('};')
    lines.append('')

    for idx, (fb, entries, height) in enumerate(tables):
        lines.append('static const app_glyph_t s_glyphs_%s[] = {' % fb.name)
        for cp, off, w, h, stride, adv, bx, by in entries:
            lines.append(
                "    {%d, %d, %d, %d, %d, %d, %d, %d},  /* %s */"
                % (cp, off, w, h, stride, adv, bx, by,
                   chr(cp) if 32 <= cp < 127 else "U+%04X" % cp)
            )
        lines.append('};')
        lines.append('')

    for idx, (fb, entries, height) in enumerate(tables):
        lines.append(
            'const app_font_t app_font_%s = {\n'
            '    .bitmap = s_bitmap,\n'
            '    .glyphs = s_glyphs_%s,\n'
            '    .count = %d,  /* glyphs */\n'
            '    .height = %d,\n'
            '    .line_height = %d,\n'
            '};\n' % (fb.name, fb.name, len(entries), height, fb.line_height)
        )

    with open(out_c, "w") as f:
        f.write("\n".join(lines) + "\n")

    header = """/* 自动生成，请勿手工修改 —— 由 tools/gen_fonts.py 生成 */
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

%s
const app_glyph_t *app_font_lookup(const app_font_t *font, uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif /* APP_FONTS_H */
""" % "\n".join("extern const app_font_t app_font_%s;" % fb.name for fb, _, _ in tables)

    with open(out_h, "w") as f:
        f.write(header)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    big_font, big_path = load_font(MONO_CANDIDATES, 56)
    mid_font, _ = load_font(MONO_CANDIDATES, 30)
    addr_font, _ = load_font(MONO_CANDIDATES, 20)
    small_font, small_path = load_font(MONO_CANDIDATES, 16)
    cjk_font, cjk_path = load_font(CJK_CANDIDATES, 19)

    # PRND 是档位字母：档位是用 big 字体画的，字符集里没有就整段空白
    big_charset = "0123456789-. %kmWh/PRND"
    # 档位字母也要出现在中号字体里（仪表盘顶部用 mid 画 P/R/N/D）
    mid_charset = "0123456789.-PRND"
    cjk_charset = "".join(sorted(set(ASCII) | set(CJK_CHARS) | collect_source_chars(), key=ord))

    fonts = [
        FontBuild("big", big_font, big_charset).build(),
        FontBuild("mid", mid_font, mid_charset).build(),
        FontBuild("addr", addr_font, "0123456789.:/- ").build(),
        FontBuild("small", small_font, ASCII).build(),
        FontBuild("cjk", cjk_font, cjk_charset).build(),
    ]

    out_c = os.path.join(OUT_DIR, "font_data.c")
    out_h = os.path.join(OUT_DIR, "app_fonts.h")
    emit(fonts, out_c, out_h)

    print("mono :", big_path)
    print("small:", small_path)
    print("cjk  :", cjk_path, "chars:", len(cjk_charset))
    print("写入:", out_c)
    print("写入:", out_h)
    print("位图大小: %.1f KB" % (os.path.getsize(out_c) / 1024.0))


if __name__ == "__main__":
    sys.exit(main())
