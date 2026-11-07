/**
 * @file tesla_protocol.c
 * @brief Tesla 协议消息编码/解码
 *
 * 使用 protobuf-c 实现 Protocol Buffer 消息编解码，与 vehicle-command Go SDK 的 proto 定义一致。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesla_protocol.h"

/* protobuf-c 生成头（proto-c 由 proto/gen_proto.sh 生成） */
#include "universal_message.pb-c.h"
#include "vcsec.pb-c.h"
#include "signatures.pb-c.h"
#include "car_server_slim.pb-c.h"

static const char *TAG = "TESLA_PROTO";

/* 用于 unpack/free_unpacked 的默认分配器（若 lib 未导出 protobuf_c_default_allocator） */
static void *default_alloc_alloc(void *allocator_data, size_t size)
{
    (void)allocator_data;
    return malloc(size);
}
static void default_alloc_free(void *allocator_data, void *pointer)
{
    (void)allocator_data;
    free(pointer);
}
static ProtobufCAllocator s_default_allocator = {
    .alloc = default_alloc_alloc,
    .free = default_alloc_free,
    .allocator_data = NULL
};

// ========== 故障码名称 ==========

const char *tesla_fault_str(tesla_fault_t fault)
{
    switch (fault) {
    case TESLA_FAULT_NONE:                        return "NONE";
    case TESLA_FAULT_BUSY:                        return "BUSY";
    case TESLA_FAULT_TIMEOUT:                     return "TIMEOUT";
    case TESLA_FAULT_UNKNOWN_KEY_ID:              return "UNKNOWN_KEY_ID";
    case TESLA_FAULT_INACTIVE_KEY:                return "INACTIVE_KEY";
    case TESLA_FAULT_INVALID_SIGNATURE:           return "INVALID_SIGNATURE";
    case TESLA_FAULT_INVALID_TOKEN:               return "INVALID_TOKEN";
    case TESLA_FAULT_INSUFFICIENT_PRIVILEGES:     return "INSUFFICIENT_PRIVILEGES";
    case TESLA_FAULT_INVALID_DOMAINS:             return "INVALID_DOMAINS";
    case TESLA_FAULT_INVALID_COMMAND:             return "INVALID_COMMAND";
    case TESLA_FAULT_DECODING:                    return "DECODING";
    case TESLA_FAULT_INTERNAL:                    return "INTERNAL";
    case TESLA_FAULT_WRONG_VIN:                   return "WRONG_VIN";
    case TESLA_FAULT_BAD_PARAMETER:               return "BAD_PARAMETER";
    case TESLA_FAULT_KEYCHAIN_FULL:               return "KEYCHAIN_FULL";
    case TESLA_FAULT_INCORRECT_EPOCH:             return "INCORRECT_EPOCH";
    case TESLA_FAULT_IV_INCORRECT_LENGTH:         return "IV_INCORRECT_LENGTH";
    case TESLA_FAULT_TIME_EXPIRED:                return "TIME_EXPIRED";
    case TESLA_FAULT_NOT_PROVISIONED:             return "NOT_PROVISIONED";
    case TESLA_FAULT_COULD_NOT_HASH_METADATA:     return "COULD_NOT_HASH_METADATA";
    case TESLA_FAULT_TTL_TOO_LONG:                return "TTL_TOO_LONG";
    case TESLA_FAULT_REMOTE_ACCESS_DISABLED:      return "REMOTE_ACCESS_DISABLED";
    case TESLA_FAULT_REMOTE_SERVICE_DISABLED:     return "REMOTE_SERVICE_DISABLED";
    case TESLA_FAULT_NEEDS_ACCOUNT_CREDENTIALS:   return "NEEDS_ACCOUNT_CREDENTIALS";
    case TESLA_FAULT_REQUEST_MTU_EXCEEDED:        return "REQUEST_MTU_EXCEEDED";
    case TESLA_FAULT_RESPONSE_MTU_EXCEEDED:       return "RESPONSE_MTU_EXCEEDED";
    case TESLA_FAULT_REPEATED_COUNTER:            return "REPEATED_COUNTER";
    case TESLA_FAULT_INVALID_KEY_HANDLE:          return "INVALID_KEY_HANDLE";
    case TESLA_FAULT_REQUIRES_RESPONSE_ENCRYPTION: return "REQUIRES_RESPONSE_ENCRYPTION";
    default:                                      return "UNKNOWN";
    }
}

// ========== 消息构建（protobuf-c）==========

/**
 * @brief 构建会话信息请求消息（使用 protobuf-c）
 */
uint32_t tesla_build_session_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *public_key,
    const uint8_t *routing_address)
{
    uint8_t challenge[16];
    uint8_t uuid[16];
    esp_fill_random(challenge, sizeof(challenge));
    esp_fill_random(uuid, sizeof(uuid));

    UniversalMessage__Destination to_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    to_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN;
    to_dest.domain = (UniversalMessage__Domain)domain;

    UniversalMessage__Destination from_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    // data 为 NULL 时必须保持 len 为 0：protobuf-c pack 会对 len>0 的 bytes 做 memcpy
    if (routing_address) {
        from_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS;
        from_dest.routing_address.data = (uint8_t *)routing_address;
        from_dest.routing_address.len = TESLA_ROUTING_ADDR_LEN;
    }

    UniversalMessage__SessionInfoRequest session_req = UNIVERSAL_MESSAGE__SESSION_INFO_REQUEST__INIT;
    if (public_key) {
        session_req.public_key.data = (uint8_t *)public_key;
        session_req.public_key.len = TESLA_PUBLIC_KEY_LEN;
    }
    session_req.challenge.data = challenge;
    session_req.challenge.len = sizeof(challenge);

    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.to_destination = &to_dest;
    msg.from_destination = &from_dest;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO_REQUEST;
    msg.session_info_request = &session_req;
    msg.uuid.data = uuid;
    msg.uuid.len = sizeof(uuid);

    size_t packed_size = universal_message__routable_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)universal_message__routable_message__pack(&msg, buf);
}

/**
 * @brief 构建信息请求（VCSEC）消息（UnsignedMessage，protobuf-c）
 */
uint32_t tesla_build_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_info_request_type_t type)
{
    VCSEC__InformationRequest info_req = VCSEC__INFORMATION_REQUEST__INIT;
    info_req.informationrequesttype = (VCSEC__InformationRequestType)type;

    VCSEC__UnsignedMessage msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_INFORMATION_REQUEST;
    msg.informationrequest = &info_req;

    size_t packed_size = vcsec__unsigned_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__unsigned_message__pack(&msg, buf);
}

/**
 * @brief 构建 RKE 动作消息（UnsignedMessage，protobuf-c）
 */
uint32_t tesla_build_rke_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_rke_action_t action)
{
    VCSEC__UnsignedMessage msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_RKEACTION;
    msg.rkeaction = (VCSEC__RKEActionE)action;

    size_t packed_size = vcsec__unsigned_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__unsigned_message__pack(&msg, buf);
}


uint32_t tesla_build_closure_target(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_closure_target_t target,
    tesla_closure_move_t action)
{
    VCSEC__ClosureMoveRequest request = VCSEC__CLOSURE_MOVE_REQUEST__INIT;
    switch (target) {
    case TESLA_CLOSURE_TARGET_FRUNK:
        request.fronttrunk = (VCSEC__ClosureMoveTypeE)action;
        break;
    case TESLA_CLOSURE_TARGET_TRUNK:
        request.reartrunk = (VCSEC__ClosureMoveTypeE)action;
        break;
    case TESLA_CLOSURE_TARGET_CHARGE_PORT:
        request.chargeport = (VCSEC__ClosureMoveTypeE)action;
        break;
    default:
        return 0;
    }

    VCSEC__UnsignedMessage msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_CLOSURE_MOVE_REQUEST;
    msg.closuremoverequest = &request;

    size_t packed_size = vcsec__unsigned_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__unsigned_message__pack(&msg, buf);
}

/**
 * @brief 构建添加密钥请求消息（ToVCSECMessage，protobuf-c）
 */
uint32_t tesla_build_add_key_request(
    uint8_t *buf,
    uint32_t buf_size,
    const uint8_t *public_key,
    tesla_key_form_factor_t form_factor)
{
    if (!public_key) {
        ESP_LOGE(TAG, "添加密钥请求缺少公钥");
        return 0;
    }

    VCSEC__PublicKey pub_key = VCSEC__PUBLIC_KEY__INIT;
    pub_key.publickeyraw.data = (uint8_t *)public_key;
    pub_key.publickeyraw.len = TESLA_PUBLIC_KEY_LEN;

    VCSEC__PermissionChange perm_change = VCSEC__PERMISSION_CHANGE__INIT;
    perm_change.key = &pub_key;
    perm_change.keyrole = KEYS__ROLE__ROLE_OWNER;

    VCSEC__KeyMetadata meta = VCSEC__KEY_METADATA__INIT;
    meta.keyformfactor = (VCSEC__KeyFormFactor)form_factor;

    VCSEC__WhitelistOperation whitelist_op = VCSEC__WHITELIST_OPERATION__INIT;
    whitelist_op.sub_message_case = VCSEC__WHITELIST_OPERATION__SUB_MESSAGE_ADD_KEY_TO_WHITELIST_AND_ADD_PERMISSIONS;
    whitelist_op.addkeytowhitelistandaddpermissions = &perm_change;
    whitelist_op.metadataforkey = &meta;

    VCSEC__UnsignedMessage unsigned_msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    unsigned_msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_WHITELIST_OPERATION;
    unsigned_msg.whitelistoperation = &whitelist_op;

    // 先问大小再 pack，否则溢出已经发生了，事后检查没有意义
    uint8_t unsigned_buf[256];
    size_t unsigned_len = vcsec__unsigned_message__get_packed_size(&unsigned_msg);
    if (unsigned_len == 0 || unsigned_len > sizeof(unsigned_buf)) {
        ESP_LOGE(TAG, "WhitelistOperation 过长: %u", (unsigned)unsigned_len);
        return 0;
    }
    unsigned_len = vcsec__unsigned_message__pack(&unsigned_msg, unsigned_buf);

    VCSEC__SignedMessage signed_msg = VCSEC__SIGNED_MESSAGE__INIT;
    signed_msg.protobufmessageasbytes.data = unsigned_buf;
    signed_msg.protobufmessageasbytes.len = unsigned_len;
    signed_msg.signaturetype = VCSEC__SIGNATURE_TYPE__SIGNATURE_TYPE_PRESENT_KEY;

    VCSEC__ToVCSECMessage to_vcsec = VCSEC__TO_VCSECMESSAGE__INIT;
    to_vcsec.signedmessage = &signed_msg;

    size_t packed_size = vcsec__to_vcsecmessage__get_packed_size(&to_vcsec);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__to_vcsecmessage__pack(&to_vcsec, buf);
}



// 元数据（请求/响应）的最大可能长度：
// sig(3) + domain(3) + vin(19) + epoch(18) + expires(6) + counter(6) + flags(6)
// + request_hash(19) + fault(6) + end(1) = 87，取 96 作为静态断言上限。
#define TESLA_METADATA_MAX_LEN 96
/** 调用方给元数据缓冲区的大小 */
#define TESLA_METADATA_BUF_LEN 128
/** 会话可用时长的合理上限（秒），用于钳制 tick 换算异常 */
#define TESLA_MAX_SESSION_AGE_SEC (24u * 3600u)

/** 写入 "TAG + len + 4 字节大端值" 的 TLV，返回新的写指针 */
static uint8_t *put_tlv_be32(uint8_t *p, uint8_t tag, uint32_t v)
{
    *p++ = tag;
    *p++ = 4;
    *p++ = (uint8_t)((v >> 24) & 0xFF);
    *p++ = (uint8_t)((v >> 16) & 0xFF);
    *p++ = (uint8_t)((v >> 8) & 0xFF);
    *p++ = (uint8_t)(v & 0xFF);
    return p;
}

/**
 * @brief 构建元数据 (用于请求认证与响应解密)
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小（不足时返回 0，避免越界）
 * @param sig_type 签名类型
 * @param domain 域
 * @param vin 车辆 VIN
 * @param epoch 时期
 * @param expires_at 过期时间
 * @param counter 计数器
 * @param flags 标志
 * @return 元数据长度
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
    uint32_t flags)
{
    if (!buf || buf_size < TESLA_METADATA_MAX_LEN) return 0;

    uint8_t *p = buf;

    // TAG_SIGNATURE_TYPE
    *p++ = TESLA_TAG_SIGNATURE_TYPE;
    *p++ = 1;
    *p++ = sig_type;

    // TAG_DOMAIN
    *p++ = TESLA_TAG_DOMAIN;
    *p++ = 1;
    *p++ = domain;

    // TAG_PERSONALIZATION (VIN)
    if (vin) {
        *p++ = TESLA_TAG_PERSONALIZATION;
        *p++ = 17;
        memcpy(p, vin, 17);
        p += 17;
    }

    // TAG_EPOCH
    if (epoch) {
        *p++ = TESLA_TAG_EPOCH;
        *p++ = 16;
        memcpy(p, epoch, 16);
        p += 16;
    }

    // TAG_EXPIRES_AT (Big-endian, as per Go SDK)
    p = put_tlv_be32(p, TESLA_TAG_EXPIRES_AT, expires_at);

    // TAG_COUNTER（计数器，大端序，与 Go SDK 一致）
    p = put_tlv_be32(p, TESLA_TAG_COUNTER, counter);

    // TAG_FLAGS（标志，大端序，与 Go SDK 一致）
    if (flags) {
        p = put_tlv_be32(p, TESLA_TAG_FLAGS, flags);
    }

    // TAG_END（结束标记）
    *p++ = TESLA_TAG_END;

    return (uint32_t)(p - buf);
}

// 构建用于响应解密的元数据（与请求元数据不同）
// 响应元数据格式：sig_type, domain, VIN, counter, flags, request_hash, fault
// request_id 是 17 字节：signature_type(1) + GCM_tag(16) 来自原始请求
static uint32_t tesla_build_response_metadata(
    uint8_t *buf,
    size_t buf_size,
    tesla_domain_t domain,
    const char *vin,
    uint32_t counter,
    uint32_t flags,
    const uint8_t *request_id,
    uint32_t request_id_len,
    uint32_t fault)
{
    if (!buf || buf_size < TESLA_METADATA_MAX_LEN) return 0;

    uint8_t *p = buf;

    // TAG_SIGNATURE_TYPE = AES_GCM_RESPONSE (9)
    *p++ = TESLA_TAG_SIGNATURE_TYPE;
    *p++ = 1;
    *p++ = TESLA_SIG_AES_GCM_RESPONSE;

    // TAG_DOMAIN（域）
    *p++ = TESLA_TAG_DOMAIN;
    *p++ = 1;
    *p++ = domain;

    // TAG_PERSONALIZATION (VIN)
    if (vin) {
        *p++ = TESLA_TAG_PERSONALIZATION;
        *p++ = 17;
        memcpy(p, vin, 17);
        p += 17;
    }

    // TAG_COUNTER (Big-endian)
    p = put_tlv_be32(p, TESLA_TAG_COUNTER, counter);

    // TAG_FLAGS（标志，大端序） - 总是包含，即使为 0
    p = put_tlv_be32(p, TESLA_TAG_FLAGS, flags);

    // TAG_REQUEST_HASH（request_id = 请求的 signature_type + GCM tag）
    // request_id 固定为 17 字节（1 字节 sig_type + 16 字节 tag），长度字节必须保持一致
    if (request_id && request_id_len > 0) {
        if (request_id_len != TESLA_REQUEST_ID_LEN) {
            ESP_LOGW(TAG, "request_id 长度异常: %lu，期望 %d",
                     (unsigned long)request_id_len, TESLA_REQUEST_ID_LEN);
        }
        *p++ = TESLA_TAG_REQUEST_HASH;
        *p++ = (uint8_t)request_id_len;
        memcpy(p, request_id, request_id_len);
        p += request_id_len;
    }

    // TAG_FAULT (Big-endian)
    p = put_tlv_be32(p, TESLA_TAG_FAULT, fault);

    // TAG_END（结束标记）
    *p++ = TESLA_TAG_END;

    return (uint32_t)(p - buf);
}

/**
 * @brief 构建已认证的可路由消息（protobuf-c）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 域
 * @param payload 负载
 * @param payload_len 负载长度
 * @param session 会话
 * @param routing_address 路由地址
 * @param public_key 公钥
 * @param vin 车辆 VIN
 * @param flags 标志
 * @param request_id_out 输出请求 ID
 * @return 消息长度
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
    uint8_t *request_id_out)
{
    if (!session || !buf) return 0;

    // tick 先按 64 位换算再截断，避免 32 位乘法溢出（100Hz 时约 57 天就会回绕，
    // 回绕后 elapsed_sec 变成一个巨大的数，expires_at 离谱导致车辆回 TIME_EXPIRED）
    uint32_t current_local_sec = (uint32_t)(((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000);
    uint32_t elapsed_sec = current_local_sec - session->local_time_sec;
    // 与上次会话建立相隔太久（例如跨天未重连）时钳制，避免 expires_at 无意义地跳很远
    if (elapsed_sec > TESLA_MAX_SESSION_AGE_SEC) {
        ESP_LOGW(TAG, "会话已建立 %lu 秒，超出合理范围，钳制为 %d 秒",
                 (unsigned long)elapsed_sec, TESLA_MAX_SESSION_AGE_SEC);
        elapsed_sec = TESLA_MAX_SESSION_AGE_SEC;
    }
    uint32_t current_vehicle_clock = session->clock_time + elapsed_sec;
    uint32_t expires_at = current_vehicle_clock + 30;

    uint8_t metadata[TESLA_METADATA_BUF_LEN];
    uint32_t metadata_len = tesla_build_metadata(metadata, sizeof(metadata),
        TESLA_SIG_AES_GCM_PERSONALIZED,
        domain, vin, session->epoch, expires_at,
        session->counter, flags);
    if (metadata_len == 0) {
        ESP_LOGE(TAG, "请求元数据构建失败");
        return 0;
    }

    ESP_LOGD(TAG, "Metadata for AAD: sig=%d domain=%d expires=%lu counter=%lu",
             TESLA_SIG_AES_GCM_PERSONALIZED, domain,
             (unsigned long)expires_at, (unsigned long)session->counter);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, metadata, metadata_len, ESP_LOG_DEBUG);

    uint8_t aad_hash[32];
    tesla_crypto_sha256(metadata, metadata_len, aad_hash);

    uint8_t nonce[12];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t ciphertext[TESLA_MAX_PLAINTEXT];
    uint8_t tag[TESLA_TAG_FIELD_LEN];

    // 输出是定长栈缓冲，必须先校验，否则大 payload 直接写穿栈帧
    if (!payload || payload_len == 0 || payload_len > sizeof(ciphertext)) {
        ESP_LOGE(TAG, "payload 长度非法: %lu（上限 %u）",
                 (unsigned long)payload_len, (unsigned)sizeof(ciphertext));
        return 0;
    }

    if (!tesla_crypto_encrypt_gcm(session->session_key, nonce, aad_hash, 32,
                                   payload, payload_len, ciphertext, tag)) {
        ESP_LOGE(TAG, "Encryption failed");
        return 0;
    }

    if (request_id_out) {
        request_id_out[0] = TESLA_SIG_AES_GCM_PERSONALIZED;
        memcpy(request_id_out + 1, tag, 16);
    }

    Signatures__KeyIdentity key_identity = SIGNATURES__KEY_IDENTITY__INIT;
    key_identity.identity_type_case = SIGNATURES__KEY_IDENTITY__IDENTITY_TYPE_PUBLIC_KEY;
    // data 为 NULL 时必须保持 len 为 0：protobuf-c pack 会对 len>0 的 bytes 做 memcpy
    if (public_key) {
        key_identity.public_key.data = (uint8_t *)public_key;
        key_identity.public_key.len = TESLA_PUBLIC_KEY_LEN;
    }

    Signatures__AESGCMPersonalizedSignatureData gcm_data = SIGNATURES__AES__GCM__PERSONALIZED__SIGNATURE__DATA__INIT;
    gcm_data.epoch.data = (uint8_t *)session->epoch;
    gcm_data.epoch.len = 16;
    gcm_data.nonce.data = nonce;
    gcm_data.nonce.len = 12;
    gcm_data.counter = session->counter;
    gcm_data.expires_at = expires_at;
    gcm_data.tag.data = tag;
    gcm_data.tag.len = 16;

    Signatures__SignatureData sig_data = SIGNATURES__SIGNATURE_DATA__INIT;
    sig_data.signer_identity = &key_identity;
    sig_data.sig_type_case = SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__PERSONALIZED_DATA;
    sig_data.aes_gcm_personalized_data = &gcm_data;

    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));

    UniversalMessage__Destination to_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    to_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN;
    to_dest.domain = (UniversalMessage__Domain)domain;

    UniversalMessage__Destination from_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    if (routing_address) {
        from_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS;
        from_dest.routing_address.data = (uint8_t *)routing_address;
        from_dest.routing_address.len = TESLA_ROUTING_ADDR_LEN;
    }

    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.to_destination = &to_dest;
    msg.from_destination = &from_dest;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES;
    msg.protobuf_message_as_bytes.data = ciphertext;
    msg.protobuf_message_as_bytes.len = payload_len;
    msg.sub_sig_data_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__SUB_SIG_DATA_SIGNATURE_DATA;
    msg.signature_data = &sig_data;
    msg.uuid.data = uuid;
    msg.uuid.len = sizeof(uuid);
    msg.flags = flags;

    size_t packed_size = universal_message__routable_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)universal_message__routable_message__pack(&msg, buf);
}


// ========== 消息解析（protobuf-c）==========

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
    tesla_session_t *session)
{
    if (len < 2) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg) return false;

    bool ok = false;
    if (rmsg->payload_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO &&
        rmsg->session_info.data && rmsg->session_info.len > 0) {
        Signatures__SessionInfo *info = signatures__session_info__unpack(NULL,
            rmsg->session_info.len, rmsg->session_info.data);
        if (info) {
            session->counter = info->counter;
            session->clock_time = info->clock_time;

            // 车辆公钥必须是 65 字节的未压缩点（0x04 开头）。
            // 若长度不足就 memcpy，缓冲区尾部会残留 0，首字节也不再是 0x04，
            // mbedtls 只会抛一个晦涩的 ECP_BAD_INPUT_DATA(-20352)。
            const uint8_t *pk = info->publickey.data;
            size_t pk_len = info->publickey.len;
            bool pk_ok = false;

            memset(session->vehicle_public_key, 0, sizeof(session->vehicle_public_key));
            if (pk != NULL && pk_len == 65 && pk[0] == 0x04) {
                memcpy(session->vehicle_public_key, pk, 65);
                pk_ok = true;
            } else if (pk != NULL && pk_len == 64) {
                // 少数固件只返回 X||Y，需要自己补未压缩前缀
                session->vehicle_public_key[0] = 0x04;
                memcpy(session->vehicle_public_key + 1, pk, 64);
                pk_ok = true;
                ESP_LOGW(TAG, "车辆公钥为 64 字节，已补 0x04 前缀");
            }

            ESP_LOGI(TAG, "SessionInfo: pk_len=%u first=0x%02x counter=%lu clock=%lu",
                     (unsigned)pk_len, (pk_len > 0 && pk) ? pk[0] : 0,
                     (unsigned long)info->counter, (unsigned long)info->clock_time);

            // epoch 必须正好 16 字节：短了会在尾部残留上一次会话的数据，导致
            // AAD 里 epoch 与车辆不一致（回 INCORRECT_EPOCH）；长了说明协议异常。
            memset(session->epoch, 0, sizeof(session->epoch));
            if (info->epoch.data && info->epoch.len == TESLA_EPOCH_LEN) {
                memcpy(session->epoch, info->epoch.data, TESLA_EPOCH_LEN);
            } else {
                ESP_LOGW(TAG, "epoch 长度异常: %u（期望 %d），本次会话可能不可用",
                         (unsigned)info->epoch.len, TESLA_EPOCH_LEN);
            }
            signatures__session_info__free_unpacked(info, NULL);

            if (!pk_ok) {
                ESP_LOGE(TAG, "车辆公钥格式异常（len=%u），放弃本次会话", (unsigned)pk_len);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len > 64 ? 64 : len, ESP_LOG_ERROR);
            }
            ok = pk_ok;
        }
    }
    universal_message__routable_message__free_unpacked(rmsg, NULL);
    return ok;
}

/** 把 protobuf 的 VCSEC.VehicleStatus 填进本地结构 */
static void fill_body_state(VCSEC__VehicleStatus *vs, tesla_body_state_t *state)
{
    state->lock_state = (tesla_lock_state_t)vs->vehiclelockstate;
    state->sleep_status = (tesla_sleep_state_t)vs->vehiclesleepstatus;
    state->user_present = (vs->userpresence == VCSEC__USER_PRESENCE__E__VEHICLE_USER_PRESENCE_PRESENT);

    // 关键：先把闭件全部复位成 CLOSED(0)。
    // 车门关好后车辆返回的 closureStatuses 里所有字段都是 0，proto3 不会序列化，
    // 于是 vs->closurestatuses == NULL。若这里直接跳过，结构体里会残留上一次的
    // "打开"状态，表现就是"门关了但界面一直显示未关闭"。
    memset(&state->closures, 0, sizeof(state->closures));

    if (vs->closurestatuses) {
        state->closures.front_driver_door = (tesla_closure_state_t)vs->closurestatuses->frontdriverdoor;
        state->closures.front_passenger_door = (tesla_closure_state_t)vs->closurestatuses->frontpassengerdoor;
        state->closures.rear_driver_door = (tesla_closure_state_t)vs->closurestatuses->reardriverdoor;
        state->closures.rear_passenger_door = (tesla_closure_state_t)vs->closurestatuses->rearpassengerdoor;
        state->closures.rear_trunk = (tesla_closure_state_t)vs->closurestatuses->reartrunk;
        state->closures.front_trunk = (tesla_closure_state_t)vs->closurestatuses->fronttrunk;
        state->closures.charge_port = (tesla_closure_state_t)vs->closurestatuses->chargeport;
        state->closures.tonneau = (tesla_closure_state_t)vs->closurestatuses->tonneau;
    }
}

/**
 * 解析 VCSEC 车身状态。
 *
 * 支持两种输入：
 *  1) 完整 RoutableMessage（明文响应，payload 为 protobuf_message_as_bytes）
 *  2) 加密响应解密后的负载，本身就是一个 VCSEC.FromVCSECMessage
 *
 * 第 2 种是当前实际路径：请求带 FLAG_ENCRYPT_RESPONSE，解密出来直接就是
 * FromVCSECMessage，再按 RoutableMessage 解会直接失败。
 */
bool tesla_parse_vehicle_status(
    const uint8_t *data,
    uint32_t len,
    tesla_body_state_t *state)
{
    if (!data || len < 2 || !state) return false;

    // ---- 1) 明文路径：完整 RoutableMessage ----
    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (rmsg) {
        if (rmsg->payload_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES &&
            rmsg->protobuf_message_as_bytes.data) {
            VCSEC__FromVCSECMessage *fv = vcsec__from_vcsecmessage__unpack(NULL,
                rmsg->protobuf_message_as_bytes.len, rmsg->protobuf_message_as_bytes.data);
            if (fv && fv->sub_message_case == VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_VEHICLE_STATUS &&
                fv->vehiclestatus) {
                fill_body_state(fv->vehiclestatus, state);
                vcsec__from_vcsecmessage__free_unpacked(fv, NULL);
                universal_message__routable_message__free_unpacked(rmsg, NULL);
                return true;
            }
            if (fv) vcsec__from_vcsecmessage__free_unpacked(fv, NULL);
        }
        universal_message__routable_message__free_unpacked(rmsg, NULL);
    }

    // ---- 2) 加密路径：解密结果本身是 FromVCSECMessage ----
    VCSEC__FromVCSECMessage *from_vcsec = vcsec__from_vcsecmessage__unpack(NULL, len, data);
    if (from_vcsec &&
        from_vcsec->sub_message_case == VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_VEHICLE_STATUS &&
        from_vcsec->vehiclestatus) {
        fill_body_state(from_vcsec->vehiclestatus, state);
        vcsec__from_vcsecmessage__free_unpacked(from_vcsec, NULL);
        return true;
    }
    if (from_vcsec) vcsec__from_vcsecmessage__free_unpacked(from_vcsec, NULL);

    return false;
}

/**
 * @brief 读取 RoutableMessage 的发送方域（from_destination）
 *
 * 响应是用"发送方域"的会话密钥加密的，先判断域就能选对 session；
 * 在多个域同时有未完成请求时，也能把别的域的响应直接丢掉，
 * 而不是拿去做一遍必然失败的解密尝试。
 *
 * @param data 原始消息
 * @param len 消息长度
 * @param domain 输出域
 * @return true 表示 from_destination 是域，*domain 有效
 */
bool tesla_routable_from_domain(
    const uint8_t *data,
    uint32_t len,
    tesla_domain_t *domain)
{
    if (!data || len < 2 || !domain) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg) return false;

    bool ok = false;
    if (rmsg->from_destination &&
        rmsg->from_destination->sub_destination_case ==
            UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN) {
        *domain = (tesla_domain_t)rmsg->from_destination->domain;
        ok = true;
    }

    universal_message__routable_message__free_unpacked(rmsg, NULL);
    return ok;
}

/**
 * 解析并解密加密的 RoutableMessage 响应（protobuf-c）
 * @param data 响应数据
 * @param len 响应数据长度
 * @param session 会话
 * @param vin 车辆 VIN
 * @param request_id 请求 ID
 * @param plaintext 解密后的负载
 * @param plaintext_len 解密后的负载长度
 * @param fault 故障码
 * @return true 成功，false 失败
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
    tesla_fault_t *fault)
{
    if (!data || !session || !fault || !plaintext || !plaintext_len) return false;

    *fault = TESLA_FAULT_NONE;
    *plaintext_len = 0;
    if (len < 2) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg) return false;

    uint32_t from_domain = TESLA_DOMAIN_INFOTAINMENT;
    bool from_is_domain = false;
    if (rmsg->from_destination) {
        from_is_domain = (rmsg->from_destination->sub_destination_case ==
                          UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN);
        if (from_is_domain) {
            from_domain = rmsg->from_destination->domain;
        }
    }

    if (rmsg->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES ||
        !rmsg->protobuf_message_as_bytes.data || rmsg->protobuf_message_as_bytes.len == 0) {
        if (rmsg->signedmessagestatus) {
            *fault = (tesla_fault_t)rmsg->signedmessagestatus->signed_message_fault;
        }
        universal_message__routable_message__free_unpacked(rmsg, NULL);
        return *fault != TESLA_FAULT_NONE;
    }

    if (rmsg->signedmessagestatus) {
        *fault = (tesla_fault_t)rmsg->signedmessagestatus->signed_message_fault;
    }

    uint32_t counter = 0;
    uint8_t nonce[12] = {0};
    uint8_t tag[16] = {0};
    bool have_gcm = false;
    if (rmsg->sub_sig_data_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__SUB_SIG_DATA_SIGNATURE_DATA &&
        rmsg->signature_data &&
        rmsg->signature_data->sig_type_case == SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__RESPONSE_DATA &&
        rmsg->signature_data->aes_gcm_response_data) {
        Signatures__AESGCMResponseSignatureData *gcm = rmsg->signature_data->aes_gcm_response_data;
        counter = gcm->counter;
        if (gcm->nonce.len == 12) {
            memcpy(nonce, gcm->nonce.data, 12);
        }
        if (gcm->tag.len == 16) {
            memcpy(tag, gcm->tag.data, 16);
        }
        have_gcm = (gcm->nonce.len == 12 && gcm->tag.len == 16);
    }

    uint32_t ciphertext_len = (uint32_t)rmsg->protobuf_message_as_bytes.len;
    uint32_t response_flags = rmsg->flags;
    uint8_t ciphertext_buf[TESLA_MAX_PLAINTEXT];
    if (ciphertext_len > sizeof(ciphertext_buf) || ciphertext_len > plaintext_cap) {
        // 输出缓冲区装不下就拒绝，绝不越过 plaintext_cap 写入
        ESP_LOGE(TAG, "响应负载 %lu 字节超出上限（密文上限 %u，明文缓冲 %lu）",
                 (unsigned long)ciphertext_len, (unsigned)sizeof(ciphertext_buf),
                 (unsigned long)plaintext_cap);
        universal_message__routable_message__free_unpacked(rmsg, NULL);
        return false;
    }
    memcpy(ciphertext_buf, rmsg->protobuf_message_as_bytes.data, ciphertext_len);
    universal_message__routable_message__free_unpacked(rmsg, NULL);

    if (!have_gcm) {
        // 明文响应：车辆在没有会话、拒绝加密或周期性广播（如 VCSEC 的 VehicleStatus
        // 通知）时会回未加密负载。这类消息没有 nonce/tag，GCM 无从解密，但
        // protobuf_message_as_bytes 本身就是可解析的应用负载（FromVCSECMessage /
        // CarServer.Response）。
        // 注意：该路径没有任何认证，任意 BLE 对端都能伪造车况。只有明确声明接受
        // 明文（VCSEC 广播场景）的调用方才能拿到内容，其余一律拒绝。
        if (!allow_plaintext) {
            ESP_LOGW(TAG, "响应未加密且调用方不接受明文，丢弃（from_domain=%lu flags=0x%lx payload=%lu）",
                     (unsigned long)from_domain, (unsigned long)response_flags,
                     (unsigned long)ciphertext_len);
            return false;
        }
        ESP_LOGW(TAG, "响应未加密（缺 nonce/tag）：from_domain=%lu flags=0x%lx payload=%lu fault=%d，按明文处理",
                 (unsigned long)from_domain, (unsigned long)response_flags,
                 (unsigned long)ciphertext_len, (int)*fault);
        memcpy(plaintext, ciphertext_buf, ciphertext_len);
        *plaintext_len = ciphertext_len;
        return true;
    }

    uint8_t metadata[128];
    uint32_t metadata_len = tesla_build_response_metadata(metadata, sizeof(metadata),
        (tesla_domain_t)from_domain, vin, counter, response_flags,
        request_id, request_id ? TESLA_REQUEST_ID_LEN : 0, (uint32_t)*fault);
    if (metadata_len == 0) {
        ESP_LOGE(TAG, "响应元数据构建失败");
        return false;
    }

    uint8_t aad_hash[32];
    tesla_crypto_sha256(metadata, metadata_len, aad_hash);

    if (!tesla_crypto_decrypt_gcm(session->session_key, nonce, aad_hash, 32,
                                   ciphertext_buf, ciphertext_len, tag, plaintext)) {
        // 失败时把 AAD 的每个输入都打出来，便于区分是域、计数器、flags、
        // fault 还是 request_id 对不上（这几项任意一个不同都会 GCM 认证失败）。
        // 调用方会按 request_id 历史逐个试探，这里的失败大多数是"试探错了"，
        // 属于正常流程，所以降到 DEBUG；真正的失败由调用方汇总后打 WARN。
        ESP_LOGD(TAG, "Decryption failed: from_domain=%lu(src=%s) counter=%lu flags=0x%lx "
                      "fault=%d cipher=%lu reqid=%02x%02x..%02x%02x vin=%s",
                 (unsigned long)from_domain, from_is_domain ? "domain" : "routing_addr",
                 (unsigned long)counter, (unsigned long)response_flags, (int)*fault,
                 (unsigned long)ciphertext_len,
                 request_id ? request_id[0] : 0, request_id ? request_id[1] : 0,
                 request_id ? request_id[15] : 0, request_id ? request_id[16] : 0,
                 vin ? vin : "(null)");
        ESP_LOGD(TAG, "AAD metadata (%lu bytes):", (unsigned long)metadata_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, metadata, metadata_len, ESP_LOG_DEBUG);
        return false;
    }
    *plaintext_len = ciphertext_len;
    return true;
}




/* ==================== 多类状态：请求 ==================== */

uint32_t tesla_build_get_states(
    uint8_t *buf,
    uint32_t buf_size,
    uint32_t mask)
{
    CarServer__GetChargeState g_charge = CAR_SERVER__GET_CHARGE_STATE__INIT;
    CarServer__GetClimateState g_climate = CAR_SERVER__GET_CLIMATE_STATE__INIT;
    CarServer__GetDriveState g_drive = CAR_SERVER__GET_DRIVE_STATE__INIT;
    CarServer__GetLocationState g_loc = CAR_SERVER__GET_LOCATION_STATE__INIT;
    CarServer__GetClosuresState g_close = CAR_SERVER__GET_CLOSURES_STATE__INIT;
    CarServer__GetTirePressureState g_tire = CAR_SERVER__GET_TIRE_PRESSURE_STATE__INIT;
    CarServer__GetMediaState g_media = CAR_SERVER__GET_MEDIA_STATE__INIT;
    CarServer__GetMediaDetailState g_mdetail = CAR_SERVER__GET_MEDIA_DETAIL_STATE__INIT;
    CarServer__GetSoftwareUpdateState g_sw = CAR_SERVER__GET_SOFTWARE_UPDATE_STATE__INIT;

    CarServer__GetVehicleData gvd = CAR_SERVER__GET_VEHICLE_DATA__INIT;
    if (mask & TESLA_STATE_BIT_CHARGE) {
        gvd.getchargestate = &g_charge;
    }
    if (mask & TESLA_STATE_BIT_CLIMATE) {
        gvd.getclimatestate = &g_climate;
    }
    if (mask & TESLA_STATE_BIT_DRIVE) {
        gvd.getdrivestate = &g_drive;
    }
    if (mask & TESLA_STATE_BIT_LOCATION) {
        gvd.getlocationstate = &g_loc;
    }
    if (mask & TESLA_STATE_BIT_CLOSURES) {
        gvd.getclosuresstate = &g_close;
    }
    if (mask & TESLA_STATE_BIT_TIRE) {
        gvd.gettirepressurestate = &g_tire;
    }
    if (mask & TESLA_STATE_BIT_MEDIA) {
        gvd.getmediastate = &g_media;
    }
    if (mask & TESLA_STATE_BIT_MEDIA_DETAIL) {
        gvd.getmediadetailstate = &g_mdetail;
    }
    if (mask & TESLA_STATE_BIT_SOFTWARE) {
        gvd.getsoftwareupdatestate = &g_sw;
    }

    CarServer__VehicleAction va = CAR_SERVER__VEHICLE_ACTION__INIT;
    va.getvehicledata = &gvd;

    CarServer__Action action = CAR_SERVER__ACTION__INIT;
    action.vehicleaction = &va;

    size_t packed_size = car_server__action__get_packed_size(&action);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    car_server__action__pack(&action, buf);
    return (uint32_t)packed_size;
}

/* ==================== 多类状态：解析 ==================== */

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void parse_charge_state(const CarServer__ChargeState *cs, tesla_charge_state_t *out)
{
    out->charge_limit_soc = cs->charge_limit_soc;
    out->fast_charger_present = cs->fast_charger_present;
    out->battery_range_km = cs->battery_range * 1.60934f;
    out->est_battery_range_km = cs->est_battery_range * 1.60934f;
    out->ideal_range_km = cs->ideal_battery_range * 1.60934f;
    out->battery_level = cs->battery_level;
    out->usable_battery_level = cs->usable_battery_level;
    out->energy_added_kwh = cs->charge_energy_added;
    out->charger_voltage = cs->charger_voltage;
    out->charger_current_a = cs->charger_actual_current;
    out->charger_power_kw = cs->charger_power;
    out->minutes_to_full = cs->minutes_to_full_charge;
    out->charge_rate_kmh = (cs->charge_rate_mph_float != 0.0f)
                               ? cs->charge_rate_mph_float * 1.60934f
                               : (float)cs->charge_rate_mph * 1.60934f;
    out->charge_port_open = cs->charge_port_door_open;
    out->charger_phases = cs->charger_phases;
    out->charge_current_request = cs->charge_current_request;
    out->charge_current_request_max = cs->charge_current_request_max;

    if (cs->charge_port_latch) {
        out->charge_port_latched =
            (cs->charge_port_latch->type_case ==
             CAR_SERVER__CHARGE_STATE__CHARGE_PORT_LATCH_STATE__TYPE_ENGAGED);
    }
    if (cs->charging_state) {
        switch (cs->charging_state->type_case) {
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_DISCONNECTED:
            out->charging_state = TESLA_CHARGING_DISCONNECTED;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_NO_POWER:
            out->charging_state = TESLA_CHARGING_NO_POWER;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_STARTING:
            out->charging_state = TESLA_CHARGING_STARTING;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_CHARGING:
            out->charging_state = TESLA_CHARGING_CHARGING;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_COMPLETE:
            out->charging_state = TESLA_CHARGING_COMPLETE;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_STOPPED:
            out->charging_state = TESLA_CHARGING_STOPPED;
            break;
        case CAR_SERVER__CHARGE_STATE__CHARGING_STATE__TYPE_CALIBRATING:
            out->charging_state = TESLA_CHARGING_CALIBRATING;
            break;
        default:
            out->charging_state = TESLA_CHARGING_UNKNOWN;
            break;
        }
    }
}

static void parse_climate_state(const CarServer__ClimateState *cs, tesla_climate_state_t *out)
{
    out->inside_temp_c = cs->inside_temp_celsius;
    out->outside_temp_c = cs->outside_temp_celsius;
    out->driver_temp_setting = cs->driver_temp_setting;
    out->passenger_temp_setting = cs->passenger_temp_setting;
    out->fan_status = cs->fan_status;
    out->climate_on = cs->is_climate_on;
    out->seat_heater_left = cs->seat_heater_left;
    out->seat_heater_right = cs->seat_heater_right;
    out->seat_heater_rear_left = cs->seat_heater_rear_left;
    out->seat_heater_rear_right = cs->seat_heater_rear_right;
    out->seat_fan_left = cs->seat_fan_front_left;
    out->seat_fan_right = cs->seat_fan_front_right;
    out->steering_wheel_heater = cs->steering_wheel_heater;
    out->preconditioning = cs->is_preconditioning;
    out->battery_heater = cs->battery_heater;
    out->front_defroster = cs->is_front_defroster_on;
    out->rear_defroster = cs->is_rear_defroster_on;
    out->bioweapon_mode = cs->bioweapon_mode_on;
    out->cabin_overheat = cs->cabin_overheat_protection;
    if (cs->climate_keeper_mode) {
        out->keeper_mode = (int32_t)cs->climate_keeper_mode->type_case;
    }
}

static void parse_closures_state(const CarServer__ClosuresState *cs, tesla_closures_state_t *out)
{
    out->door_open[0] = cs->door_open_driver_front;
    out->door_open[1] = cs->door_open_passenger_front;
    out->door_open[2] = cs->door_open_driver_rear;
    out->door_open[3] = cs->door_open_passenger_rear;
    out->window_open[0] = cs->window_open_driver_front;
    out->window_open[1] = cs->window_open_passenger_front;
    out->window_open[2] = cs->window_open_driver_rear;
    out->window_open[3] = cs->window_open_passenger_rear;
    out->trunk_front_open = cs->door_open_trunk_front;
    out->trunk_rear_open = cs->door_open_trunk_rear;
    out->locked = cs->locked;
    out->user_present = cs->is_user_present;
    out->remote_start = cs->remote_start;
    out->valet_mode = cs->valet_mode;
    out->sentry_available = cs->sentry_mode_available;
    out->sunroof_percent = cs->sun_roof_percent_open;
    if (cs->sentry_mode_state) {
        out->sentry_state = (int32_t)cs->sentry_mode_state->type_case;
    }
    if (cs->center_display_state) {
        out->display_state = (int32_t)cs->center_display_state->type_case;
    }
}

static void parse_media_state(const CarServer__MediaState *ms, tesla_media_state_t *out)
{
    copy_str(out->artist, sizeof(out->artist), ms->now_playing_artist);
    copy_str(out->title, sizeof(out->title), ms->now_playing_title);
    out->audio_volume = ms->audio_volume;
    out->audio_volume_max = ms->audio_volume_max;
    out->source = ms->now_playing_source;
    out->playback_status = ms->media_playback_status;
}

static void parse_media_detail_state(const CarServer__MediaDetailState *md, tesla_media_state_t *out)
{
    copy_str(out->album, sizeof(out->album), md->now_playing_album);
}

static void parse_software_state(const CarServer__SoftwareUpdateState *sw, tesla_software_state_t *out)
{
    // version 是静态值，车机不一定每次都带（多数固件只在有 OTA 时上报）。
    // 只有明确给了才覆盖，否则沿用上一次读到的版本，否则每次轮询都会把
    // 版本清成空串，界面上的"软件更新"就一直停在"无更新"。
    if (sw->version && sw->version[0]) {
        copy_str(out->version, sizeof(out->version), sw->version);
    }
    out->download_percent = sw->download_perc;
    out->install_percent = sw->install_perc;
    if (sw->status) {
        out->status = (int32_t)sw->status->type_case;
    }
}

bool tesla_parse_vehicle_data(
    const uint8_t *data,
    uint32_t len,
    tesla_vehicle_state_t *out,
    uint32_t *present_mask)
{
    if (!data || len < 2 || !out) {
        return false;
    }

    CarServer__Response *resp = car_server__response__unpack(
        &s_default_allocator, (size_t)len, data);
    if (!resp || !resp->vehicledata) {
        if (resp) {
            car_server__response__free_unpacked(resp, &s_default_allocator);
        }
        return false;
    }

    CarServer__VehicleData *vd = resp->vehicledata;
    uint32_t mask = 0;

    if (vd->drive_state) {
        tesla_drive_state_t *d = &out->drive;
        CarServer__DriveState *ds = vd->drive_state;
        memset(d, 0, sizeof(*d));
        switch (ds->shift_state ? ds->shift_state->type_case
                                : CAR_SERVER__SHIFT_STATE__TYPE__NOT_SET) {
        case CAR_SERVER__SHIFT_STATE__TYPE_P: d->gear = TESLA_GEAR_P; break;
        case CAR_SERVER__SHIFT_STATE__TYPE_R: d->gear = TESLA_GEAR_R; break;
        case CAR_SERVER__SHIFT_STATE__TYPE_N: d->gear = TESLA_GEAR_N; break;
        case CAR_SERVER__SHIFT_STATE__TYPE_D: d->gear = TESLA_GEAR_D; break;
        case CAR_SERVER__SHIFT_STATE__TYPE_SNA: d->gear = TESLA_GEAR_SNA; break;
        default: d->gear = TESLA_GEAR_INVALID; break;
        }
        d->speed_kmh = (ds->speed_float != 0.0f) ? ds->speed_float : (float)ds->speed;
        d->power_kw = ds->power;
        d->odometer_km = (float)ds->odometer_in_hundredths_of_a_mile / 100.0f * 1.60934f;
        d->minutes_to_arrival = ds->active_route_minutes_to_arrival;
        d->km_to_arrival = ds->active_route_miles_to_arrival * 1.60934f;
        copy_str(d->destination, sizeof(d->destination), ds->active_route_destination);
        mask |= TESLA_STATE_BIT_DRIVE;
    }

    if (vd->charge_state) {
        memset(&out->charge, 0, sizeof(out->charge));
        parse_charge_state(vd->charge_state, &out->charge);
        mask |= TESLA_STATE_BIT_CHARGE;
    }

    if (vd->climate_state) {
        memset(&out->climate, 0, sizeof(out->climate));
        parse_climate_state(vd->climate_state, &out->climate);
        mask |= TESLA_STATE_BIT_CLIMATE;
    }

    if (vd->closures_state) {
        memset(&out->closures, 0, sizeof(out->closures));
        parse_closures_state(vd->closures_state, &out->closures);
        mask |= TESLA_STATE_BIT_CLOSURES;
    }

    if (vd->location_state) {
        memset(&out->location, 0, sizeof(out->location));
        out->location.latitude = vd->location_state->latitude;
        out->location.longitude = vd->location_state->longitude;
        out->location.heading = (float)vd->location_state->heading;
        copy_str(out->location.name, sizeof(out->location.name),
                 vd->location_state->location_name);
        mask |= TESLA_STATE_BIT_LOCATION;
    }

    if (vd->tire_pressure_state) {
        CarServer__TirePressureState *ts = vd->tire_pressure_state;
        memset(&out->tires, 0, sizeof(out->tires));
        out->tires.front_left_bar = ts->tpms_pressure_fl;
        out->tires.front_right_bar = ts->tpms_pressure_fr;
        out->tires.rear_left_bar = ts->tpms_pressure_rl;
        out->tires.rear_right_bar = ts->tpms_pressure_rr;
        out->tires.warning_fl = ts->tpms_hard_warning_fl || ts->tpms_soft_warning_fl;
        out->tires.warning_fr = ts->tpms_hard_warning_fr || ts->tpms_soft_warning_fr;
        out->tires.warning_rl = ts->tpms_hard_warning_rl || ts->tpms_soft_warning_rl;
        out->tires.warning_rr = ts->tpms_hard_warning_rr || ts->tpms_soft_warning_rr;
        out->tires.recommend_front_bar = ts->tpms_rcp_front_value;
        out->tires.recommend_rear_bar = ts->tpms_rcp_rear_value;
        mask |= TESLA_STATE_BIT_TIRE;
    }

    // media_state 才是曲名/艺术家的来源，media_detail_state 只补专辑等字段。
    // 响应超 MTU 时上层会把多类拆成"一次一类"逐个请求：若 detail 分支也把整个
    // media 清零，就会把上一次 media_state 刚读到的 title 抹掉，曲名恒为空。
    // 所以只在 media_state 到来时清零，detail 分支只覆盖自己的字段。
    if (vd->media_state) {
        memset(&out->media, 0, sizeof(out->media));
        parse_media_state(vd->media_state, &out->media);
        mask |= TESLA_STATE_BIT_MEDIA;
    }

    if (vd->media_detail_state) {
        parse_media_detail_state(vd->media_detail_state, &out->media);
        mask |= TESLA_STATE_BIT_MEDIA_DETAIL;
    }

    if (vd->software_update_state) {
        // status / 下载进度是瞬时状态，车机不回时必须归零；version 是静态值，
        // 要沿用已读到的。用临时结构解析后整体替换，既刷新了瞬时字段又保住版本。
        tesla_software_state_t sw = {0};
        memcpy(sw.version, out->software.version, sizeof(sw.version));
        parse_software_state(vd->software_update_state, &sw);
        out->software = sw;
        mask |= TESLA_STATE_BIT_SOFTWARE;

        // 诊断：官方 vehicle.proto 里 version 属于 oneof optional_version，车机
        // 没有 OTA 时不会设置它。把车机这次实际带回的字段和留存结果打出来
        // （内容变化才打，避免每轮轮询刷屏），用来区分"车机压根没上报"和
        // "解析/显示环节把值弄丢了"。
        const CarServer__SoftwareUpdateState *raw = vd->software_update_state;
        static char s_last_sw_diag[96];
        char diag[96];
        snprintf(diag, sizeof(diag), "ver=%s status=%d dl=%lu ins=%lu",
                 raw->version ? raw->version : "(null)",
                 raw->status ? (int)raw->status->type_case : -1,
                 (unsigned long)raw->download_perc,
                 (unsigned long)raw->install_perc);
        if (strcmp(diag, s_last_sw_diag) != 0) {
            snprintf(s_last_sw_diag, sizeof(s_last_sw_diag), "%s", diag);
            ESP_LOGI(TAG, "OTA状态 %s -> 留存版本='%s'", diag, sw.version);
        }
    }

    car_server__response__free_unpacked(resp, &s_default_allocator);

    out->present_mask |= mask;
    if (mask) {
        out->valid = true;
    }
    if (present_mask) {
        *present_mask = mask;
    }
    return mask != 0;
}

/* ==================== 车机域控制动作 ==================== */

// oneof 里的空选项需要真实指针，protobuf-c 不会因为 case 被赋值就认为选项存在
static CarServer__Void s_void_opt = CAR_SERVER__VOID__INIT;

uint32_t tesla_build_carserver_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_command_t cmd,
    int32_t param)
{
    CarServer__Action action = CAR_SERVER__ACTION__INIT;
    CarServer__VehicleAction va = CAR_SERVER__VEHICLE_ACTION__INIT;
    action.vehicleaction = &va;

    CarServer__HvacAutoAction hvac = CAR_SERVER__HVAC_AUTO_ACTION__INIT;
    CarServer__HvacSetPreconditioningMaxAction precond =
        CAR_SERVER__HVAC_SET_PRECONDITIONING_MAX_ACTION__INIT;
    CarServer__HvacSteeringWheelHeaterAction stw =
        CAR_SERVER__HVAC_STEERING_WHEEL_HEATER_ACTION__INIT;
    CarServer__HvacBioweaponModeAction bio = CAR_SERVER__HVAC_BIOWEAPON_MODE_ACTION__INIT;
    CarServer__SetCabinOverheatProtectionAction cop =
        CAR_SERVER__SET_CABIN_OVERHEAT_PROTECTION_ACTION__INIT;
    CarServer__HvacTemperatureAdjustmentAction temp =
        CAR_SERVER__HVAC_TEMPERATURE_ADJUSTMENT_ACTION__INIT;
    CarServer__HvacTemperatureAdjustmentAction__Temperature temp_level =
        CAR_SERVER__HVAC_TEMPERATURE_ADJUSTMENT_ACTION__TEMPERATURE__INIT;
    CarServer__HvacSeatHeaterActions seats = CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__INIT;
    CarServer__HvacSeatHeaterActions__HvacSeatHeaterAction seat0 =
        CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__INIT;
    CarServer__HvacSeatHeaterActions__HvacSeatHeaterAction *seat_list[1];
    CarServer__ChargingStartStopAction chg = CAR_SERVER__CHARGING_START_STOP_ACTION__INIT;
    CarServer__ChargingSetLimitAction limit = CAR_SERVER__CHARGING_SET_LIMIT_ACTION__INIT;
    CarServer__SetChargingAmpsAction amps = CAR_SERVER__SET_CHARGING_AMPS_ACTION__INIT;
    CarServer__ChargePortDoorOpen port_open = CAR_SERVER__CHARGE_PORT_DOOR_OPEN__INIT;
    CarServer__ChargePortDoorClose port_close = CAR_SERVER__CHARGE_PORT_DOOR_CLOSE__INIT;
    CarServer__VehicleControlWindowAction win = CAR_SERVER__VEHICLE_CONTROL_WINDOW_ACTION__INIT;
    CarServer__VehicleControlSunroofOpenCloseAction roof =
        CAR_SERVER__VEHICLE_CONTROL_SUNROOF_OPEN_CLOSE_ACTION__INIT;
    CarServer__VehicleControlFlashLightsAction flash =
        CAR_SERVER__VEHICLE_CONTROL_FLASH_LIGHTS_ACTION__INIT;
    CarServer__VehicleControlHonkHornAction horn =
        CAR_SERVER__VEHICLE_CONTROL_HONK_HORN_ACTION__INIT;
    CarServer__VehicleControlSetSentryModeAction sentry =
        CAR_SERVER__VEHICLE_CONTROL_SET_SENTRY_MODE_ACTION__INIT;
    CarServer__VehicleControlSetValetModeAction valet =
        CAR_SERVER__VEHICLE_CONTROL_SET_VALET_MODE_ACTION__INIT;
    CarServer__VehicleControlResetValetPinAction valet_rst =
        CAR_SERVER__VEHICLE_CONTROL_RESET_VALET_PIN_ACTION__INIT;
    CarServer__MediaPlayAction play = CAR_SERVER__MEDIA_PLAY_ACTION__INIT;
    CarServer__MediaNextTrack next_track = CAR_SERVER__MEDIA_NEXT_TRACK__INIT;
    CarServer__MediaPreviousTrack prev_track = CAR_SERVER__MEDIA_PREVIOUS_TRACK__INIT;
    CarServer__MediaUpdateVolume vol = CAR_SERVER__MEDIA_UPDATE_VOLUME__INIT;
    CarServer__VehicleControlCancelSoftwareUpdateAction ota_cancel =
        CAR_SERVER__VEHICLE_CONTROL_CANCEL_SOFTWARE_UPDATE_ACTION__INIT;
    CarServer__SetLowPowerModeAction lowp = CAR_SERVER__SET_LOW_POWER_MODE_ACTION__INIT;
    CarServer__SetKeepAccessoryPowerModeAction accp =
        CAR_SERVER__SET_KEEP_ACCESSORY_POWER_MODE_ACTION__INIT;
    CarServer__EraseUserDataAction erase = CAR_SERVER__ERASE_USER_DATA_ACTION__INIT;
    CarServer__Ping ping = CAR_SERVER__PING__INIT;

    switch (cmd) {
    case TESLA_CMD_CLIMATE_TOGGLE:
        hvac.power_on = (param != 0);
        hvac.manual_override = true;
        va.hvacautoaction = &hvac;
        break;

    case TESLA_CMD_PRECOND_MAX_TOGGLE:
        precond.on = (param != 0);
        precond.manual_override = true;
        va.hvacsetpreconditioningmaxaction = &precond;
        break;

    case TESLA_CMD_STEERING_HEAT_TOGGLE:
        stw.power_on = (param != 0);
        va.hvacsteeringwheelheateraction = &stw;
        break;

    case TESLA_CMD_BIOWEAPON_TOGGLE:
        bio.on = (param != 0);
        bio.manual_override = true;
        va.hvacbioweaponmodeaction = &bio;
        break;

    case TESLA_CMD_COP_TOGGLE:
        cop.on = (param != 0);
        cop.fan_only = false;
        va.setcabinoverheatprotectionaction = &cop;
        break;

    case TESLA_CMD_TEMP_UP:
    case TESLA_CMD_TEMP_DOWN:
        // 严格对齐官方 ChangeClimateTemp：driver/passenger 绝对温度 + Level{TEMP_MAX}。
        // param 是目标温度的 10 倍（0.1℃）。protobuf-c 跳过零值字段，所以只编码
        // 5/6/7；之前只发 delta_celsius 且不带 Level，车机可能按最低温档处理。
        // param<=0 表示状态完全拿不到，退回增量方式，但仍带上 Level。
        if (param > 0) {
            float t = (float)param / 10.0f;
            temp.driver_temp_celsius = t;
            temp.passenger_temp_celsius = t;
        } else {
            temp.delta_celsius = (cmd == TESLA_CMD_TEMP_UP) ? 0.5f : -0.5f;
        }
        temp_level.type_case =
            CAR_SERVER__HVAC_TEMPERATURE_ADJUSTMENT_ACTION__TEMPERATURE__TYPE_TEMP__MAX;
        temp_level.temp_max = &s_void_opt;
        temp.level = &temp_level;
        va.hvactemperatureadjustmentaction = &temp;
        break;

    case TESLA_CMD_SEAT_HEATER_TOGGLE: {
        // param 直接就是 oneof 的枚举值：2=off 3=low 4=med 5=high
        CarServer__HvacSeatHeaterActions__HvacSeatHeaterAction__SeatHeaterLevelCase lv =
            CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_HEATER_LEVEL__NOT_SET;
        switch (param) {
        case 2: lv = CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_HEATER_LEVEL_SEAT__HEATER__OFF;
            seat0.seat_heater_off = &s_void_opt;
            break;
        case 3: lv = CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_HEATER_LEVEL_SEAT__HEATER__LOW;
            seat0.seat_heater_low = &s_void_opt;
            break;
        case 4: lv = CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_HEATER_LEVEL_SEAT__HEATER__MED;
            seat0.seat_heater_med = &s_void_opt;
            break;
        default: lv = CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_HEATER_LEVEL_SEAT__HEATER__HIGH;
            seat0.seat_heater_high = &s_void_opt;
            break;
        }
        seat0.seat_heater_level_case = lv;
        seat0.seat_position_case =
            CAR_SERVER__HVAC_SEAT_HEATER_ACTIONS__HVAC_SEAT_HEATER_ACTION__SEAT_POSITION_CAR__SEAT__FRONT__LEFT;
        seat0.car_seat_front_left = &s_void_opt;
        seat_list[0] = &seat0;
        seats.hvacseatheateraction = seat_list;
        seats.n_hvacseatheateraction = 1;
        va.hvacseatheateractions = &seats;
        break;
    }

    case TESLA_CMD_CHARGE_TOGGLE:
        if (param) {
            chg.charging_action_case =
                CAR_SERVER__CHARGING_START_STOP_ACTION__CHARGING_ACTION_START;
            chg.start = &s_void_opt;
        } else {
            chg.charging_action_case =
                CAR_SERVER__CHARGING_START_STOP_ACTION__CHARGING_ACTION_STOP;
            chg.stop = &s_void_opt;
        }
        va.chargingstartstopaction = &chg;
        break;

    case TESLA_CMD_CHARGE_LIMIT_UP:
    case TESLA_CMD_CHARGE_LIMIT_DOWN:
        limit.percent = param;
        va.chargingsetlimitaction = &limit;
        break;

    case TESLA_CMD_CHARGE_AMPS_UP:
    case TESLA_CMD_CHARGE_AMPS_DOWN:
        amps.charging_amps = param;
        va.setchargingampsaction = &amps;
        break;

    case TESLA_CMD_CHARGE_PORT_TOGGLE:
        if (param) {
            va.chargeportdooropen = &port_open;
        } else {
            va.chargeportdoorclose = &port_close;
        }
        break;

    case TESLA_CMD_WINDOWS_CLOSE:
        win.action_case = CAR_SERVER__VEHICLE_CONTROL_WINDOW_ACTION__ACTION_CLOSE;
        win.close = &s_void_opt;
        va.vehiclecontrolwindowaction = &win;
        break;

    case TESLA_CMD_WINDOWS_VENT:
        // param=1 通风、param=0 关闭，由 toggle_target 交替：第一次通风、再点一次关闭。
        // 之前这里恒为 ACTION_VENT，连点两次只会发两次通风。
        if (param) {
            win.action_case = CAR_SERVER__VEHICLE_CONTROL_WINDOW_ACTION__ACTION_VENT;
            win.vent = &s_void_opt;
        } else {
            win.action_case = CAR_SERVER__VEHICLE_CONTROL_WINDOW_ACTION__ACTION_CLOSE;
            win.close = &s_void_opt;
        }
        va.vehiclecontrolwindowaction = &win;
        break;

    case TESLA_CMD_SUNROOF_TOGGLE:
        if (param) {
            roof.action_case =
                CAR_SERVER__VEHICLE_CONTROL_SUNROOF_OPEN_CLOSE_ACTION__ACTION_OPEN;
            roof.open = &s_void_opt;
        } else {
            roof.action_case =
                CAR_SERVER__VEHICLE_CONTROL_SUNROOF_OPEN_CLOSE_ACTION__ACTION_CLOSE;
            roof.close = &s_void_opt;
        }
        va.vehiclecontrolsunroofopencloseaction = &roof;
        break;

    case TESLA_CMD_FLASH_LIGHTS:
        va.vehiclecontrolflashlightsaction = &flash;
        break;

    case TESLA_CMD_HONK_HORN:
        va.vehiclecontrolhonkhornaction = &horn;
        break;

    case TESLA_CMD_SENTRY_TOGGLE:
        sentry.on = (param != 0);
        va.vehiclecontrolsetsentrymodeaction = &sentry;
        break;

    case TESLA_CMD_VALET_TOGGLE:
        // 官方 DisableValetMode 用 SetValetMode(false, "")：proto3 下 false 和空串
        // 都不编码，关闭时就是一个空的 valet action。恒带 "1234" 会被车端当成改密码。
        valet.on = (param != 0);
        valet.password = param ? (char *)"1234" : NULL;
        va.vehiclecontrolsetvaletmodeaction = &valet;
        break;

    case TESLA_CMD_VALET_RESET_PIN:
        va.vehiclecontrolresetvaletpinaction = &valet_rst;
        break;

    case TESLA_CMD_MEDIA_PLAY_PAUSE:
        va.mediaplayaction = &play;
        break;

    case TESLA_CMD_MEDIA_NEXT:
        va.medianexttrack = &next_track;
        break;

    case TESLA_CMD_MEDIA_PREV:
        va.mediaprevioustrack = &prev_track;
        break;

    case TESLA_CMD_VOLUME_UP:
        vol.media_volume_case =
            CAR_SERVER__MEDIA_UPDATE_VOLUME__MEDIA_VOLUME_VOLUME_DELTA;
        vol.volume_delta = 1;   // 官方 VolumeUp：delta=1（只有符号有意义）
        va.mediaupdatevolume = &vol;
        break;

    case TESLA_CMD_VOLUME_DOWN:
        vol.media_volume_case =
            CAR_SERVER__MEDIA_UPDATE_VOLUME__MEDIA_VOLUME_VOLUME_DELTA;
        vol.volume_delta = -1;  // 官方 VolumeDown：delta=-1
        va.mediaupdatevolume = &vol;
        break;

    case TESLA_CMD_OTA_CANCEL:
        va.vehiclecontrolcancelsoftwareupdateaction = &ota_cancel;
        break;

    case TESLA_CMD_LOW_POWER_TOGGLE:
        lowp.low_power_mode = (param != 0);
        va.setlowpowermodeaction = &lowp;
        break;

    case TESLA_CMD_ACCESSORY_POWER_TOGGLE:
        accp.keep_accessory_power_mode = (param != 0);
        va.setkeepaccessorypowermodeaction = &accp;
        break;

    case TESLA_CMD_PING:
        ping.ping_id = 1;   // 官方 Ping{PingId:1} 用于响应消歧；精简版原来发的是空消息
        va.ping = &ping;
        break;

    case TESLA_CMD_ERASE_USER_DATA:
        erase.reason = (char *)"user_request";
        va.eraseuserdataaction = &erase;
        break;

    default:
        // VCSEC 域命令（车锁/后备箱/前备箱/唤醒/远程启动）由上层用 VCSEC 报文发送
        return 0;
    }

    size_t packed_size = car_server__action__get_packed_size(&action);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    car_server__action__pack(&action, buf);
    return (uint32_t)packed_size;
}
