/**
 * @file tesla_protocol.h
 * @brief Tesla Protocol Message Handling
 *
 * 使用 protobuf-c 处理与 vehicle-command Go SDK 一致的 Protocol Buffer 消息编解码。
 */

#ifndef TESLA_PROTOCOL_H
#define TESLA_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "tesla_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== 协议标签（来自 signatures.proto）==========

/** request_id 固定长度：1 字节 signature_type + 16 字节 GCM tag */
#define TESLA_REQUEST_ID_LEN 17
/** epoch 固定长度（16 字节） */
#define TESLA_EPOCH_LEN 16
/** 未压缩 P-256 公钥长度（0x04 || X || Y） */
#define TESLA_PUBLIC_KEY_LEN 65
/** 私钥 / ECDH 共享密钥长度 */
#define TESLA_PRIVATE_KEY_LEN 32
/** 会话密钥长度（AES-128） */
#define TESLA_SESSION_KEY_LEN 16
/** GCM nonce / tag / routing address 长度 */
#define TESLA_NONCE_LEN 12
#define TESLA_TAG_FIELD_LEN 16
#define TESLA_ROUTING_ADDR_LEN 16
/** 单条消息负载上限（解密输出缓冲至少要这么大） */
#define TESLA_MAX_PLAINTEXT 512

typedef enum {
    TESLA_TAG_SIGNATURE_TYPE = 0,     // 签名类型
    TESLA_TAG_DOMAIN = 1,              // 域
    TESLA_TAG_PERSONALIZATION = 2,     // 个性化（VIN）
    TESLA_TAG_EPOCH = 3,               // 时期
    TESLA_TAG_EXPIRES_AT = 4,          // 过期时间
    TESLA_TAG_COUNTER = 5,             // 计数器
    TESLA_TAG_CHALLENGE = 6,           // 挑战
    TESLA_TAG_FLAGS = 7,               // 标志
    TESLA_TAG_REQUEST_HASH = 8,        // 请求哈希
    TESLA_TAG_FAULT = 9,               // 故障
    TESLA_TAG_END = 255                // 结束标记
} tesla_tag_t;

// ========== 签名类型 ==========

typedef enum {
    TESLA_SIG_AES_GCM = 0,                  // AES-GCM 加密
    TESLA_SIG_AES_GCM_PERSONALIZED = 5,     // 个性化 AES-GCM 加密
    TESLA_SIG_HMAC = 6,                     // HMAC 签名
    TESLA_SIG_HMAC_PERSONALIZED = 8,        // 个性化 HMAC 签名
    TESLA_SIG_AES_GCM_RESPONSE = 9          // AES-GCM 响应
} tesla_signature_type_t;

// ========== 消息标志 ==========

typedef enum {
    TESLA_FLAG_USER_COMMAND = 0,         // 用户命令
    TESLA_FLAG_ENCRYPT_RESPONSE = 1      // 加密响应
} tesla_message_flag_t;

// ========== 消息故障 ==========

typedef enum {
    TESLA_FAULT_NONE = 0,                    // 无故障
    TESLA_FAULT_BUSY = 1,                    // 忙碌
    TESLA_FAULT_TIMEOUT = 2,                 // 超时
    TESLA_FAULT_UNKNOWN_KEY_ID = 3,          // 未知密钥 ID
    TESLA_FAULT_INACTIVE_KEY = 4,            // 非活动密钥
    TESLA_FAULT_INVALID_SIGNATURE = 5,       // 无效签名
    TESLA_FAULT_INVALID_TOKEN = 6,           // 无效令牌
    TESLA_FAULT_INSUFFICIENT_PRIVILEGES = 7, // 权限不足
    TESLA_FAULT_INVALID_DOMAINS = 8,         // 无效域
    TESLA_FAULT_INVALID_COMMAND = 9,         // 无效命令
    TESLA_FAULT_DECODING = 10,               // 解码错误
    TESLA_FAULT_INTERNAL = 11,               // 内部错误
    TESLA_FAULT_WRONG_VIN = 12,              // 错误的 VIN
    TESLA_FAULT_BAD_PARAMETER = 13,          // 错误的参数
    TESLA_FAULT_KEYCHAIN_FULL = 14,          // 密钥链已满
    TESLA_FAULT_INCORRECT_EPOCH = 15,        // 错误的时期
    // 16 之后原先漏定义了，车机回的 fault=25 打印出来只是个裸数字。
    // 实际它是 RESPONSE_MTU_EXCEEDED：车机收到并执行了请求，
    // 但组装出的响应装不进当前 ATT MTU，只能回故障码。
    TESLA_FAULT_IV_INCORRECT_LENGTH = 16,    // IV 长度错误
    TESLA_FAULT_TIME_EXPIRED = 17,           // 命令已过期
    TESLA_FAULT_NOT_PROVISIONED = 18,        // 车辆未配置 VIN
    TESLA_FAULT_COULD_NOT_HASH_METADATA = 19, // 车辆内部错误
    TESLA_FAULT_TTL_TOO_LONG = 20,           // 过期时间过远
    TESLA_FAULT_REMOTE_ACCESS_DISABLED = 21, // 车主已禁用远程访问
    TESLA_FAULT_REMOTE_SERVICE_DISABLED = 22, // 服务密钥不允许远程服务命令
    TESLA_FAULT_NEEDS_ACCOUNT_CREDENTIALS = 23, // 需 Tesla 账户凭证
    TESLA_FAULT_REQUEST_MTU_EXCEEDED = 24,   // 请求超过 MTU
    TESLA_FAULT_RESPONSE_MTU_EXCEEDED = 25,  // 响应超过 MTU
    TESLA_FAULT_REPEATED_COUNTER = 26,       // 计数器重复
    TESLA_FAULT_INVALID_KEY_HANDLE = 27,     // 无效密钥句柄
    TESLA_FAULT_REQUIRES_RESPONSE_ENCRYPTION = 28 // 需要加密响应
} tesla_fault_t;

/**
 * @brief 故障码的可读名称（用于日志）
 * @return 静态字符串，未知码返回 "UNKNOWN"
 */
const char *tesla_fault_str(tesla_fault_t fault);

// ========== VCSEC 消息类型 ==========

typedef enum {
    TESLA_INFO_GET_STATUS = 0,            // 获取状态
    TESLA_INFO_GET_WHITELIST = 5,         // 获取白名单
    TESLA_INFO_GET_WHITELIST_ENTRY = 6    // 获取白名单条目
} tesla_info_request_type_t;

typedef enum {
    TESLA_RKE_UNLOCK = 0,          // 解锁
    TESLA_RKE_LOCK = 1,            // 锁定
    TESLA_RKE_REMOTE_DRIVE = 20,   // 远程驾驶
    TESLA_RKE_AUTO_SECURE = 29,    // 自动安全
    TESLA_RKE_WAKE = 30            // 唤醒
} tesla_rke_action_t;

typedef enum {
    TESLA_CLOSURE_MOVE_NONE = 0,   // 无动作
    TESLA_CLOSURE_MOVE_MOVE = 1,   // 移动
    TESLA_CLOSURE_MOVE_STOP = 2,   // 停止
    TESLA_CLOSURE_MOVE_OPEN = 3,   // 打开
    TESLA_CLOSURE_MOVE_CLOSE = 4   // 关闭
} tesla_closure_move_t;

// ========== 车载服务器分类 ==========

typedef enum {
    TESLA_STATE_DRIVE = 0,         // 驾驶状态
    TESLA_STATE_CHARGE = 1,        // 充电状态
    TESLA_STATE_CLIMATE = 2,       // 气候状态
    TESLA_STATE_CLOSURES = 3,      // 关闭状态
    TESLA_STATE_LOCATION = 4,      // 位置
    TESLA_STATE_TIRE_PRESSURE = 5, // 胎压
    TESLA_STATE_SOFTWARE = 6,      // 软件
    TESLA_STATE_MEDIA = 7          // 媒体
} tesla_state_category_t;

// ========== 密钥形态 ==========

typedef enum {
    TESLA_KEY_UNKNOWN = 0,           // 未知
    TESLA_KEY_NFC_CARD = 1,          // NFC 卡
    TESLA_KEY_IOS_DEVICE = 6,        // iOS 设备
    TESLA_KEY_ANDROID_DEVICE = 7,    // Android 设备
    TESLA_KEY_CLOUD_KEY = 9          // 云密钥
} tesla_key_form_factor_t;

// ========== 消息构建器（均使用 protobuf-c 生成代码）==========

/**
 * @brief 构建会话信息请求消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 目标域
 * @param public_key 公钥（65 字节）
 * @param routing_address 路由地址（16 字节）
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_session_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *public_key,
    const uint8_t *routing_address);

/**
 * @brief 构建信息请求（VCSEC）消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param type 请求类型
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_info_request_type_t type);

/**
 * @brief 构建 RKE 动作消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param action 动作类型
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_rke_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_rke_action_t action);


/** 闭件目标（前后备箱、充电口） */
typedef enum {
    TESLA_CLOSURE_TARGET_FRUNK = 0,
    TESLA_CLOSURE_TARGET_TRUNK = 1,
    TESLA_CLOSURE_TARGET_CHARGE_PORT = 2,
} tesla_closure_target_t;

/**
 * @brief 构建任意闭件动作请求（VCSEC UnsignedMessage）
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_closure_target(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_closure_target_t target,
    tesla_closure_move_t action);

/**
 * @brief 构建添加密钥请求消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param public_key 公钥（65 字节）
 * @param form_factor 密钥形态
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_add_key_request(
    uint8_t *buf,
    uint32_t buf_size,
    const uint8_t *public_key,
    tesla_key_form_factor_t form_factor);



/**
 * @brief 构建一次性读取多类状态的 GetVehicleData 请求（CarServer Action）
 * @param mask TESLA_STATE_BIT_* 组合，可同时请求多类
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_get_states(
    uint8_t *buf,
    uint32_t buf_size,
    uint32_t mask);

/**
 * @brief 解析 CarServer 响应里的 VehicleData（可含多类状态）
 * @param present_mask 输出本次解析到的类别位掩码，可传 NULL
 * @return true 至少解析出一类
 */
bool tesla_parse_vehicle_data(
    const uint8_t *data,
    uint32_t len,
    tesla_vehicle_state_t *out,
    uint32_t *present_mask);

/**
 * @brief 构建车机域控制动作（CarServer Action）
 * @param cmd 命令
 * @param param 目标值/幅度（由调用方按当前状态算好）
 * @return 消息长度，0 表示失败或该命令不属于车机域
 */
uint32_t tesla_build_carserver_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_command_t cmd,
    int32_t param);

/**
 * @brief 构建已认证的可路由消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 目标域
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param session 会话信息
 * @param routing_address 路由地址
 * @param public_key 公钥
 * @param vin 车辆识别码
 * @param flags 标志
 * @param request_id_out 输出请求 ID（17 字节：签名类型 + GCM 标签），可为 NULL
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_routable_message(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *payload,
    uint32_t payload_len,
    const tesla_session_t *session,
    const uint8_t *routing_address,
    const uint8_t *public_key,
    const char *vin,
    uint32_t flags,
    uint8_t *request_id_out);


// ========== 消息解析器 ==========

/**
 * @brief 解析会话信息响应
 * @param data 响应数据
 * @param len 数据长度
 * @param session 输出会话信息
 * @return true 成功，false 失败
 */
bool tesla_parse_session_info(
    const uint8_t *data,
    uint32_t len,
    tesla_session_t *session);

/**
 * @brief 解析车辆状态（VCSEC）响应
 * @param data 响应数据
 * @param len 数据长度
 * @param state 输出车身状态
 * @return true 成功，false 失败
 */
bool tesla_parse_vehicle_status(
    const uint8_t *data,
    uint32_t len,
    tesla_body_state_t *state);

/**
 * @brief 读取可路由消息的发送方域（from_destination）
 * @param data 原始消息
 * @param len 消息长度
 * @param domain 输出域（VCSEC / INFOTAINMENT）
 * @return true 表示 from_destination 是域，*domain 有效；false 表示是路由地址或无法解析
 * @note 响应用发送方域的会话密钥加密，据此选择 session 并丢弃别的域的响应
 */
bool tesla_routable_from_domain(
    const uint8_t *data,
    uint32_t len,
    tesla_domain_t *domain);

/**
 * @brief 解析并解密加密的可路由消息响应
 * @param data 原始响应数据
 * @param len 响应数据长度
 * @param session 包含解密密钥的会话
 * @param vin 车辆 VIN（用于元数据）
 * @param request_id 原始请求的请求 ID（17 字节：签名类型 + GCM 标签）
 * @param plaintext 解密后负载的输出缓冲区
 * @param plaintext_cap plaintext 缓冲区容量（不足时返回 false，绝不越界）
 * @param plaintext_len 输出解密数据长度
 * @param allow_plaintext 是否允许"未加密响应按明文透传"（仅 VCSEC 广播场景应为 true）
 * @param fault 输出故障码（如有错误）
 * @return true 成功（解密成功，或允许明文时负载已原样透传），false 失败
 * @note 响应若没有 AES-GCM nonce/tag（车辆周期性广播、明文错误响应），
 *       protobuf_message_as_bytes 会原样作为明文返回，由调用方按自身消息类型解析。
 *       该路径没有认证，allow_plaintext 为 false 时会直接拒绝，避免伪造车况被当真。
 */
bool tesla_parse_and_decrypt_response(
    const uint8_t *data,
    uint32_t len,
    const tesla_session_t *session,
    const char *vin,
    const uint8_t *request_id,
    uint8_t *plaintext,
    uint32_t plaintext_cap,
    uint32_t *plaintext_len,
    bool allow_plaintext,
    tesla_fault_t *fault);




// ========== 加密函数 ==========

/**
 * @brief 生成 ECDSA P-256 密钥对
 * @param private_key 输出私钥（32 字节）
 * @param public_key 输出公钥（65 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_generate_keypair(uint8_t *private_key, uint8_t *public_key);

/**
 * @brief 从私钥派生公钥
 * @param private_key 私钥（32 字节）
 * @param public_key 输出公钥（65 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_derive_public_key(const uint8_t *private_key, uint8_t *public_key);

/**
 * @brief 计算 ECDH 共享密钥
 * @param private_key 我们的私钥（32 字节）
 * @param peer_public_key 对等方公钥（65 字节）
 * @param shared_secret 输出共享密钥（32 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_ecdh(
    const uint8_t *private_key,
    const uint8_t *peer_public_key,
    uint8_t *shared_secret);

/**
 * @brief 从共享密钥派生会话密钥
 * @param shared_secret 共享密钥（32 字节）
 * @param session_key 输出会话密钥（16 字节）
 */
void tesla_crypto_derive_key(
    const uint8_t *shared_secret,
    uint8_t *session_key);

/**
 * @brief 使用 AES-GCM 加密消息
 * @param key 密钥（16 字节）
 * @param nonce 随机数（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param plaintext 明文
 * @param plaintext_len 明文长度
 * @param ciphertext 输出密文
 * @param tag 输出认证标签（16 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_encrypt_gcm(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_len,
    const uint8_t *plaintext,
    uint32_t plaintext_len,
    uint8_t *ciphertext,
    uint8_t *tag);

/**
 * @brief 使用 AES-GCM 解密消息
 * @param key 密钥（16 字节）
 * @param nonce 随机数（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param ciphertext 密文
 * @param ciphertext_len 密文长度
 * @param tag 认证标签（16 字节）
 * @param plaintext 输出明文
 * @return true 成功，false 失败
 */
bool tesla_crypto_decrypt_gcm(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_len,
    const uint8_t *ciphertext,
    uint32_t ciphertext_len,
    const uint8_t *tag,
    uint8_t *plaintext);

/**
 * @brief 计算 SHA1 哈希
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希（20 字节）
 */
void tesla_crypto_sha1(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash);

/**
 * @brief 计算 SHA256 哈希
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希（32 字节）
 */
void tesla_crypto_sha256(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash);

/**
 * @brief 构建签名元数据
 * @param buf 输出缓冲区
 * @param sig_type 签名类型
 * @param domain 域
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小（不足时返回 0）
 * @param vin 车辆 VIN
 * @param epoch 时期（16 字节）
 * @param expires_at 过期时间
 * @param counter 计数器
 * @param flags 标志
 * @return 元数据长度，缓冲区不足返回 0
 */
uint32_t tesla_build_metadata(
    uint8_t *buf,
    size_t buf_size,
    tesla_signature_type_t sig_type,
    tesla_domain_t domain,
    const char *vin,
    const uint8_t *epoch,
    uint32_t expires_at,
    uint32_t counter,
    uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // TESLA_PROTOCOL_H
