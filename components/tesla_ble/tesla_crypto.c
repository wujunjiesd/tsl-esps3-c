/**
 * @file tesla_crypto.c
 * @brief 使用 mbedTLS 实现的 Tesla 加密函数
 * 
 * 提供 ECDH 密钥交换、AES-GCM 加解密、SHA 哈希等加密功能
 */

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "tesla_protocol.h"

static const char *TAG = "TESLA_CRYPTO";

// 随机数生成器上下文
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static bool rng_initialized = false;

/**
 * @brief 初始化随机数生成器
 * @return 0 成功，其他值失败
 */
static int init_rng(void)
{
    if (rng_initialized) return 0;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)"tesla", 5);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to seed RNG: %d", ret);
        // 失败也要释放已初始化的上下文，否则下次调用会重复 init+seed
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ret;
    }

    rng_initialized = true;
    return 0;
}

/**
 * @brief 生成 ECDSA P-256 密钥对
 * @param private_key 输出私钥（32 字节）
 * @param public_key 输出公钥（65 字节，未压缩格式）
 * @return true 成功，false 失败
 */
bool tesla_crypto_generate_keypair(uint8_t *private_key, uint8_t *public_key)
{
    if (!private_key || !public_key) return false;
    if (init_rng() != 0) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi d;  // 私钥
    mbedtls_ecp_point Q;  // 公钥

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int ret;

    // 加载 NIST P-256 曲线
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load curve: %d", ret);
        goto cleanup;
    }

    // 生成密钥对
    ret = mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to generate keypair: %d", ret);
        goto cleanup;
    }

    // 导出私钥（32 字节）
    ret = mbedtls_mpi_write_binary(&d, private_key, 32);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to export private key: %d", ret);
        goto cleanup;
    }

    // 以未压缩格式导出公钥（65 字节：0x04 || X || Y）
    // 注意：olen != 65 时 ret 仍是 0，必须显式置错，否则会把未写满的公钥当成功返回
    size_t olen;
    ret = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &olen, public_key, TESLA_PUBLIC_KEY_LEN);
    if (ret != 0 || olen != TESLA_PUBLIC_KEY_LEN) {
        ESP_LOGE(TAG, "Failed to export public key: ret=%d olen=%u", ret, (unsigned)olen);
        if (ret == 0) ret = -1;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Generated ECDSA P-256 keypair");

cleanup:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);

    return ret == 0;
}

/**
 * @brief 计算 ECDH 共享密钥
 * @param private_key 本地私钥（32 字节）
 * @param peer_public_key 对方公钥（65 字节，未压缩格式）
 * @param shared_secret 输出共享密钥（32 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_ecdh(
    const uint8_t *private_key,
    const uint8_t *peer_public_key,
    uint8_t *shared_secret)
{
    if (!private_key || !peer_public_key || !shared_secret) return false;
    if (init_rng() != 0) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi d;  // 我们的私钥
    mbedtls_ecp_point Q;  // 对等方的公钥
    mbedtls_mpi z;  // 共享密钥

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&z);

    int ret;

    // 加载曲线
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) goto cleanup;

    // 导入私钥
    ret = mbedtls_mpi_read_binary(&d, private_key, 32);
    if (ret != 0) goto cleanup;

    // 公钥必须是 0x04 开头的未压缩点，否则 mbedtls 只会返回晦涩的 -0x4F80
    if (peer_public_key[0] != 0x04) {
        ESP_LOGE(TAG, "Peer public key 格式非法: first=0x%02x", peer_public_key[0]);
        ret = -1;
        goto cleanup;
    }

    // 导入对等方的公钥
    ret = mbedtls_ecp_point_read_binary(&grp, &Q, peer_public_key, TESLA_PUBLIC_KEY_LEN);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to import peer public key: %d (first=0x%02x)",
                 ret, peer_public_key[0]);
        goto cleanup;
    }

    // 上面只保证"能解析成坐标"，不保证点真的落在 P-256 曲线上。
    // 缺这一步就是典型的无效曲线攻击面：畸形点参与标量乘，得到的共享密钥可被推导。
    ret = mbedtls_ecp_check_pubkey(&grp, &Q);
    if (ret != 0) {
        ESP_LOGE(TAG, "Peer public key 不在曲线上: %d", ret);
        goto cleanup;
    }

    // 计算共享密钥
    ret = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d,
                                       mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "ECDH failed: %d", ret);
        goto cleanup;
    }

    // 导出共享密钥（32 字节）
    ret = mbedtls_mpi_write_binary(&z, shared_secret, 32);
    if (ret != 0) goto cleanup;

    ESP_LOGI(TAG, "ECDH completed");

cleanup:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&z);

    return ret == 0;
}

/**
 * @brief 从私钥派生公钥
 * @param private_key 私钥（32 字节）
 * @param public_key 输出公钥（65 字节，未压缩格式）
 * @return true 成功，false 失败
 */
bool tesla_crypto_derive_public_key(const uint8_t *private_key, uint8_t *public_key)
{
    if (!private_key || !public_key) return false;
    if (init_rng() != 0) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi d;  // 私钥
    mbedtls_ecp_point Q;  // 公钥

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int ret;

    // 加载 NIST P-256 曲线
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load curve: %d", ret);
        goto cleanup;
    }

    // 导入私钥
    ret = mbedtls_mpi_read_binary(&d, private_key, 32);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to import private key: %d", ret);
        goto cleanup;
    }

    // 计算公钥：Q = d * G
    ret = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to compute public key: %d", ret);
        goto cleanup;
    }

    // 以未压缩格式导出公钥（65 字节：0x04 || X || Y）
    // 同 generate_keypair：olen != 65 时 ret 仍是 0，必须显式置错
    size_t olen;
    ret = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &olen, public_key, TESLA_PUBLIC_KEY_LEN);
    if (ret != 0 || olen != TESLA_PUBLIC_KEY_LEN) {
        ESP_LOGE(TAG, "Failed to export public key: %d, olen=%u", ret, (unsigned)olen);
        if (ret == 0) ret = -1;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Derived public key from private key");

cleanup:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);

    return ret == 0;
}

/**
 * @brief 从共享密钥派生会话密钥
 * @param shared_secret 共享密钥（32 字节）
 * @param session_key 输出会话密钥（16 字节）
 * 
 * K = SHA1(S.x_coordinates)[:16]
 * shared_secret 是 x 坐标（32 字节）
 */
void tesla_crypto_derive_key(
    const uint8_t *shared_secret,
    uint8_t *session_key)
{
    uint8_t hash[20];
    tesla_crypto_sha1(shared_secret, 32, hash);

    // 取前 16 字节
    memcpy(session_key, hash, 16);

    ESP_LOGI(TAG, "Session key derived");
}

/**
 * @brief 使用 AES-GCM 加密消息
 * @param key 加密密钥（16 字节）
 * @param nonce 随机数/初始化向量（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param plaintext 明文数据
 * @param plaintext_len 明文数据长度
 * @param ciphertext 输出密文（与明文等长）
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
    uint8_t *tag)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "GCM setkey failed: %d", ret);
        mbedtls_gcm_free(&gcm);
        return false;
    }

    ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                     plaintext_len,
                                     nonce, 12,
                                     aad, aad_len,
                                     plaintext, ciphertext,
                                     16, tag);

    mbedtls_gcm_free(&gcm);

    if (ret != 0) {
        ESP_LOGE(TAG, "GCM encrypt failed: %d", ret);
        return false;
    }

    return true;
}

/**
 * @brief 使用 AES-GCM 解密消息
 * @param key 解密密钥（16 字节）
 * @param nonce 随机数/初始化向量（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param ciphertext 密文数据
 * @param ciphertext_len 密文数据长度
 * @param tag 认证标签（16 字节）
 * @param plaintext 输出明文（与密文等长）
 * @return true 成功，false 失败（认证失败或其他错误）
 */
bool tesla_crypto_decrypt_gcm(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_len,
    const uint8_t *ciphertext,
    uint32_t ciphertext_len,
    const uint8_t *tag,
    uint8_t *plaintext)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "GCM setkey failed: %d", ret);
        mbedtls_gcm_free(&gcm);
        return false;
    }

    ret = mbedtls_gcm_auth_decrypt(&gcm,
                                    ciphertext_len,
                                    nonce, 12,
                                    aad, aad_len,
                                    tag, 16,
                                    ciphertext, plaintext);

    mbedtls_gcm_free(&gcm);

    if (ret != 0) {
        // 响应配对（试探不同 request_id）时认证失败是预期结果，交给上层汇总成
        // 一条 WARN，这里只留 DEBUG，否则每次轮询会刷出成片的 E 日志。
        ESP_LOGD(TAG, "GCM decrypt failed: %d", ret);
        return false;
    }

    return true;
}

/**
 * @brief 计算 SHA1 哈希值
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希值（20 字节）
 */
void tesla_crypto_sha1(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash)
{
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, data, data_len);
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
}

/**
 * @brief 计算 SHA256 哈希值
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希值（32 字节）
 */
void tesla_crypto_sha256(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256
    mbedtls_sha256_update(&ctx, data, data_len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
}
