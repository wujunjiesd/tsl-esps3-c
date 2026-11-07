# Tesla BLE 仪表 - Waveshare ESP32-S3-Touch-LCD-3.49-V2
# 基于 NimBLE 的特斯拉蓝牙连接 + 172x640 竖屏仪表

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tesla_ble_dash)
