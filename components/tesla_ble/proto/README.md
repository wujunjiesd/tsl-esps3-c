# Tesla 协议 Proto 定义

本目录包含与 [vehicle-command](https://github.com/teslamotors/vehicle-command) Go SDK 一致的 Protocol Buffer 定义子集，用于 BLE 与车辆通信。

## 生成 C 代码（protobuf-c）

需要已安装 **protobuf-c**（含 `protoc-c`）：

```bash
# macOS
brew install protobuf-c

# 在组件目录下执行
./proto/gen_proto.sh
```

生成文件输出到 `proto-c/`（`*.pb-c.h`、`*.pb-c.c`），已纳入 CMake 编译。

## 依赖

- 组件构建依赖 ESP-IDF 的 **protobuf-c** 组件（`PRIV_REQUIRES protobuf-c`），标准 ESP-IDF 已包含。
