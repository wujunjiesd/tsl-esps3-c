#!/bin/bash
# 编译脚本：使用本机 ESP-IDF v5.5.3 编译本项目
# 用法: ./tools/build.sh build | ./tools/build.sh menuconfig | ./tools/build.sh -p /dev/ttyUSB0 flash monitor
set -o pipefail

IDF_VERSION="${IDF_VERSION:-v5.5.3}"
IDF_ROOT="$HOME/.espressif/$IDF_VERSION/esp-idf"
PYTHON="$HOME/.espressif/tools/python/$IDF_VERSION/venv/bin/python3"
ACTIVATE="$HOME/.espressif/tools/activate_idf_${IDF_VERSION}.sh"
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$IDF_ROOT" ]; then
    echo "找不到 ESP-IDF: $IDF_ROOT"
    exit 1
fi

args=""
for a in "$@"; do
    args="$args '$a'"
done

# 说明：
#  - activate 脚本只能被 source，但它通过 $0 判断，因此这里用 bash -c 让 $0=bash
#  - 同时剔除 IDE 注入的 safe-bin shim（其 rm 包装在本机不可用）
bash -c "source '$ACTIVATE' >/dev/null 2>&1
export PATH=\$(echo \"\$PATH\" | tr ':' '\n' | grep -v 'safe-bin' | paste -sd: -)
export IDF_PATH='$IDF_ROOT'
cd '$PROJECT' || exit 1
exec '$PYTHON' '$IDF_ROOT/tools/idf.py' $args"
