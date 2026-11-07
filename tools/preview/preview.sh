#!/bin/bash
# 在 PC 上渲染仪表界面，生成 tools/preview/preview_*.png
set -e

cd "$(dirname "$0")/../.." || exit 1
OUT=tools/preview
CC="${CC:-cc}"

$CC -O1 -Wall -o "$OUT/ui_preview" \
    "$OUT/ui_preview.c" \
    components/lcd_display/ui_draw.c \
    components/lcd_display/ui_pages.c \
    components/lcd_display/fonts/font_data.c \
    -Icomponents/lcd_display/include \
    -Icomponents/lcd_display \
    -Icomponents/lcd_display/fonts \
    -Icomponents/board/include \
    -I"$OUT/stub"

cd "$OUT" || exit 1
./ui_preview
/opt/homebrew/bin/python3 - <<'EOF'
from PIL import Image
import glob
for p in glob.glob('preview_*.ppm'):
    Image.open(p).save(p.replace('.ppm', '.png'))
    print('->', p.replace('.ppm', '.png'))
EOF
