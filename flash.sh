#!/bin/bash
#
# 一键：编译 -> 烧写 -> 监视
#
# 用途：绕开 IDE 的 "Build, Flash and Monitor" 任务。IDE 的 monitor 需要真 TTY，
# 在集成终端里经常拿不到 raw 模式，任务会被判为失败（其实编译和烧写都成功了）。
#
# 用法：
#   ./flash.sh                  # 自动找端口，烧写后监视
#   ./flash.sh /dev/cu.usbmodem2101
#   ./flash.sh /dev/cu.usbmodem2101 nobuild   # 只烧写+监视，不重新编译
#   ./flash.sh -                # 只编译，不烧写
#
set -e
cd "$(dirname "$0")"

IDF_PATH=${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "找不到 ESP-IDF: $IDF_PATH"
    echo "请先设置 IDF_PATH 环境变量，或修改本脚本里的默认路径"
    exit 1
fi
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1

# ---- 端口 ----
PORT="$1"
if [ -z "$PORT" ]; then
    # macOS 上优先选 usbmodem（原生 USB-JTAG），其次 usbserial（CH343 等）
    for p in /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
        if [ -e "$p" ]; then
            PORT="$p"
            break
        fi
    done
fi

if [ "$PORT" = "-" ]; then
    echo "== 只编译 =="
    idf.py build
    exit 0
fi

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "没找到串口设备，请插好 USB 或用参数指定，例如："
    echo "  ./flash.sh /dev/cu.usbmodem2101"
    exit 1
fi

echo "== 端口: $PORT =="

# ---- 编译 ----
if [ "$2" != "nobuild" ]; then
    echo "== 编译 =="
    idf.py build
fi

# ---- 烧写 ----
# USB-JTAG（usbmodem*）必须用 usb_reset 才能正确复位进下载模式，
# 否则 esptool 会报 "Failed to connect"。
case "$PORT" in
    *usbmodem*)
        BAUD=${BAUD:-115200}
        BEFORE="usb_reset"
        ;;
    *)
        BAUD=${BAUD:-460800}
        BEFORE="default_reset"
        ;;
esac

echo "== 烧写 (baud=$BAUD, before=$BEFORE) =="
python -m esptool --chip esp32s3 -p "$PORT" -b "$BAUD" --before "$BEFORE" \
    --after hard_reset write_flash @build/flash_args

# ---- 监视 ----
echo "== 监视（Ctrl+] 退出）=="
idf.py -p "$PORT" monitor
