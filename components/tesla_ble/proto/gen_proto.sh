#!/bin/sh
# 使用 protoc-c 从 .proto 生成 protobuf-c 的 C 代码
# 需要已安装 protobuf-c：brew install protobuf-c（或系统包）
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROTO_DIR="${SCRIPT_DIR}"
OUT_DIR="${SCRIPT_DIR}/../proto-c"
mkdir -p "$OUT_DIR"
protoc-c --c_out="$OUT_DIR" -I "$PROTO_DIR" \
    "$PROTO_DIR/keys.proto" \
    "$PROTO_DIR/errors.proto" \
    "$PROTO_DIR/signatures.proto" \
    "$PROTO_DIR/vcsec.proto" \
    "$PROTO_DIR/universal_message.proto" \
    "$PROTO_DIR/car_server_slim.proto"
echo "Generated files in $OUT_DIR"
