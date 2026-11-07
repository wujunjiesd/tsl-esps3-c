# protobuf-c 生成源（proto 目录下执行 ./proto/gen_proto.sh 生成）
set(PROTO_C_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto-c")
set(PROTO_C_SRCS
    "${PROTO_C_DIR}/keys.pb-c.c"
    "${PROTO_C_DIR}/errors.pb-c.c"
    "${PROTO_C_DIR}/signatures.pb-c.c"
    "${PROTO_C_DIR}/vcsec.pb-c.c"
    "${PROTO_C_DIR}/universal_message.pb-c.c"
    "${PROTO_C_DIR}/car_server_slim.pb-c.c"
)

idf_component_register(
    SRCS
        "tesla_ble.c"
        "tesla_protocol.c"
        "tesla_crypto.c"
        "tesla_metrics.c"
        ${PROTO_C_SRCS}
    INCLUDE_DIRS
        "include"
        "proto-c"
    PRIV_REQUIRES
        bt
        nvs_flash
        mbedtls
        protobuf-c
)
