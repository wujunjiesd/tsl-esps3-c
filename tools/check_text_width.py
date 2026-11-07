#!/usr/bin/env python3
"""
检查界面文案宽度：解析生成的字体表，计算各字符串的渲染宽度，
用于在无硬件时确认文字不会超出卡片/屏幕。
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_C = os.path.join(ROOT, "components", "lcd_display", "fonts", "font_data.c")

SCREEN_W = 172
CARD_INNER = 148   # 卡片可用宽度（CARD_W - 2*8）


def parse_fonts(path):
    src = open(path, encoding="utf-8").read()
    fonts = {}
    for name in ("big", "mid", "small", "cjk", "addr"):
        m = re.search(r"static const app_glyph_t s_glyphs_%s\[\] = \{(.*?)\};" % name, src, re.S)
        if not m:
            continue
        table = {}
        for line in m.group(1).splitlines():
            g = re.match(r"\s*\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),", line)
            if g:
                cp, off, w, h, stride, adv = (int(g.group(i)) for i in range(1, 7))
                table[cp] = (w, h, adv)
        font = re.search(r"const app_font_t app_font_%s = \{(.*?)\};" % name, src, re.S)
        height = int(re.search(r"\.height = (\d+)", font.group(1)).group(1))
        fonts[name] = {"glyphs": table, "height": height}
    return fonts


def text_width(font, s):
    w = 0
    for ch in s:
        g = font["glyphs"].get(ord(ch))
        w += g[2] if g else font["height"] // 2
    return w


def main():
    fonts = parse_fonts(FONT_C)
    if not fonts:
        print("无法解析字体表")
        return 1

    print("字体高度:", {k: v["height"] for k, v in fonts.items()})

    # 只检查其中含中文（或明显偏长）的字符串，使用 cjk 字体（最常用的 UI 字体）
    pattern = re.compile(r'"([^"\\]*[\u4e00-\u9fff][^"\\]*)"')
    problems = 0
    seen = set()
    for path in glob.glob(os.path.join(ROOT, "components", "**", "*.[ch]"), recursive=True) + \
            glob.glob(os.path.join(ROOT, "main", "*.c")):
        if "font_data.c" in path or "tools" in path:
            continue
        src = open(path, encoding="utf-8", errors="ignore").read()
        src = re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)
        for s in pattern.findall(src):
            if s in seen:
                continue
            seen.add(s)
            w = text_width(fonts["cjk"], s)
            flag = "  <== 超出" if w > CARD_INNER else ""
            if w > CARD_INNER:
                problems += 1
            print("%4d px  %-30s%s" % (w, s, flag))

    # ASCII 长字符串（单位、地址等）
    for s in ["192.168.4.1", "kWh/100km", "TeslaDash-1A2B", "12.3 kWh/100km"]:
        print("%4d px  %-30s (mid/small)" % (text_width(fonts["mid"], s), s))

    print("\n超出卡片宽度（%dpx）的文案：%d 条" % (CARD_INNER, problems))
    return 0


if __name__ == "__main__":
    sys.exit(main())
