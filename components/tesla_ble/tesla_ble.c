/**
 * @file tesla_ble.c
 * @brief Tesla BLE 连接实现
 * 
 * 实现与 Tesla 车辆的蓝牙 BLE 连接，包括扫描、连接、认证和命令发送
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "tesla_ble.h"
#include "tesla_protocol.h"

static const char *TAG = "TESLA_BLE";

// Tesla 服务 UUID（从车辆日志获取）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 11 02 00 00
static const ble_uuid128_t tesla_svc_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00
);

// 写特征 UUID（发送到车辆）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 12 02 00 00
static const ble_uuid128_t tesla_write_chr_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x12, 0x02, 0x00, 0x00
);

// 读特征 UUID（从车辆接收）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 13 02 00 00
static const ble_uuid128_t tesla_read_chr_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00
);

// 全局连接状态
static tesla_connection_t g_conn = {0};
static tesla_vehicle_state_t g_vehicle_state = {0};
static SemaphoreHandle_t g_mutex = NULL;         // 互斥锁
static SemaphoreHandle_t g_rx_sem = NULL;        // 接收信号量
static SemaphoreHandle_t g_tx_sem = NULL;        // 写完成信号量
static SemaphoreHandle_t g_tx_lock = NULL;       // 串行化整条消息的分块写入
static SemaphoreHandle_t g_transact_lock = NULL; // 串行化"清缓冲→发→等→解密"整条事务

// 本次连接是否需要把 GetVehicleData 拆成单类请求：
// 一次要 3~4 类状态时响应装不进协商的 MTU，车机会整体拒绝（fault=25）。
// 见过一次就整条连接都改用逐类读取，结果按 present_mask 累加。
static bool s_states_split = false;

// 上一次收到通知的时刻。官方 SDK（pkg/connector/ble/ble.go 的 rx()）在每次收包时
// 都会检查："距上次收包超过 rxTimeout(1s) 就清空缓冲"——因为一条消息的分块如果没到齐，
// 残帧会一直留在缓冲里，之后新数据只能拼在它后面，下一次读长度头就变成
// 26624/54428 这种垃圾值（日志里那几条"长度头异常"就是这么来的）。
// TESLA_RX_TIMEOUT_MS 早就定义好了，只是从来没被用上。
static uint32_t s_last_rx_ms = 0;

// 最近的 request_id 历史。
// 车辆的响应可能晚于请求到达（请求超时重发、通知排队、ui 任务插队发送命令），
// 只保留"最后一条"request_id 会把响应配到错误的请求上：request_hash 对不上，
// GCM 认证必然失败（日志表现为 Decryption failed + counter/domain 都正常）。
// 历史长度决定了能容忍多晚的响应：主循环约 400ms 一轮，16 条约覆盖 6 秒；
// 之前是 6 条（≈2.4 秒），车机稍一排队响应就再也配不上，
// 日志表现为"每条候选 request_id 都 GCM 失败"。
#define TESLA_REQID_HISTORY 16
static uint8_t g_reqid_hist[TESLA_REQID_HISTORY][TESLA_REQUEST_ID_LEN];
static int g_reqid_write = 0;   // 下一个写入位置
static int g_reqid_valid = 0;   // 有效条数（≤ TESLA_REQID_HISTORY）

// 各类超时统一具名，避免同一批数字散落在文件里改一处漏一处
#define TESLA_WAIT_SESSION_MS    10000   // 等待会话信息响应
#define TESLA_WAIT_BODY_MS       600     // 等待车身状态：正常 300ms 内，失败快速放行别拖住速度轮
#define TESLA_WAIT_STATES_MS     1500    // 等待 GetVehicleData 响应：失败时尽快返回，避免速度停 4 秒
#define TESLA_WAIT_CMD_MS        3000    // 等待车控命令响应

// ATT Write 每帧的协议开销：1 字节 opcode + 2 字节 handle
#define TESLA_ATT_WRITE_OVERHEAD 3
// CCCD 描述符相对特征值句柄的偏移。Tesla 的读特征后面紧跟的就是它的 CCCD，
// 这里先具名，避免 read_handle + 1 这种裸数字散落在代码里
#define TESLA_CCCD_HANDLE_OFFSET 1
// GetVehicleData 可拆分类别的位数上限
#define TESLA_STATE_CATEGORY_CNT 9
// GetVehicleData 里可逐类拆分的类别数量（present_mask 的低 9 位）
#define TESLA_STATE_CATEGORY_CNT 9

// 回调函数
static tesla_state_callback_t g_state_cb = NULL;
static void *g_state_cb_arg = NULL;

// 目标 VIN 用于扫描
static char g_target_vin[18] = {0};
static char g_target_local_name[20] = {0};
static ble_addr_t g_target_addr = {0};

// 前向声明
static int tesla_gap_event(struct ble_gap_event *event, void *arg);

// ========== 工具函数 ==========

/**
 * @brief 通知状态变化
 * @param state 新的连接状态
 */
static void notify_state_change(tesla_conn_state_t state)
{
    g_conn.state = state;
    if (g_state_cb) {
        g_state_cb(state, g_state_cb_arg);
    }
}

/**
 * @brief 生成随机路由地址
 * @param addr 16 字节的地址缓冲区
 */
static void generate_routing_address(uint8_t *addr)
{
    esp_fill_random(addr, 16);
}

// ========== BLE 扫描 ==========

/**
 * @brief 检查 BLE 广播数据是否为目标 Tesla 车辆
 * @param data 广播数据
 * @param len 数据长度
 * @param target_name 目标设备名称
 * @return true 如果找到目标设备
 */
static bool check_tesla_adv(const uint8_t *data, uint8_t len, const char *target_name)
{
    uint8_t offset = 0;
    while (offset < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + field_len >= len) break;

        uint8_t field_type = data[offset + 1];

        // 检查完整本地名称（0x09）或缩短本地名称（0x08）
        if (field_type == 0x09 || field_type == 0x08) {
            char name[32] = {0};
            uint8_t name_len = field_len - 1;
            if (name_len > sizeof(name) - 1) name_len = sizeof(name) - 1;
            memcpy(name, &data[offset + 2], name_len);

            // ESP_LOGI(TAG, "Found device: %s", name);

            if (strcmp(name, target_name) == 0) {
                return true;
            }
        }

        offset += field_len + 1;
    }
    return false;
}

/**
 * @brief 启动 BLE 扫描
 */
static void tesla_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return;
    }

    disc_params.filter_duplicates = 0;  // 不过滤重复设备
    disc_params.passive = 0;             // 主动扫描
    disc_params.itvl = 0;                // 使用默认扫描间隔
    disc_params.window = 0;              // 使用默认扫描窗口
    disc_params.filter_policy = 0;       // 不使用白名单
    disc_params.limited = 0;             // 一般发现模式

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, tesla_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
    } else {
        ESP_LOGI(TAG, "Scanning for Tesla vehicle: %s", g_target_local_name);
        notify_state_change(TESLA_STATE_SCANNING);
    }
}

// ========== GATT 客户端 ==========

/**
 * @brief MTU 交换完成回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param mtu 协商后的 MTU 值
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_mtu_exchange(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  uint16_t mtu, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU exchange complete, MTU=%d", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed: %d, using default MTU", error->status);
    }
    // 无论 MTU 交换是否成功，都通知服务发现完成
    notify_state_change(TESLA_STATE_SERVICE_DISCOVERED);
    return 0;
}

/**
 * @brief 订阅通知回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param attr 属性信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to notifications");
        // 订阅成功后，发起 MTU 交换（与 Go SDK 一致）
        int rc = ble_gattc_exchange_mtu(conn_handle, tesla_on_mtu_exchange, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Failed to initiate MTU exchange: %d", rc);
            // 即使 MTU 交换失败，也继续
            notify_state_change(TESLA_STATE_SERVICE_DISCOVERED);
        }
    } else {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
    }
    return 0;
}

/**
 * @brief 订阅通知
 * @param conn_handle 连接句柄
 * @param cccd_handle CCCD 句柄
 */
static void tesla_subscribe_notifications(uint16_t conn_handle, uint16_t cccd_handle)
{
    uint8_t value[2] = {0x01, 0x00};  // 启用通知
    int rc = ble_gattc_write_flat(conn_handle, cccd_handle, value, sizeof(value),
                                   tesla_on_subscribe, NULL);
    if (rc != 0) {
        // 订阅失败意味着一条通知都收不到，继续等响应只会等到超时。
        // 明确回到断连态，让上层立刻重连，而不是干等 CONNECT_TIMEOUT_MS
        ESP_LOGE(TAG, "Failed to subscribe: %d", rc);
        notify_state_change(TESLA_STATE_DISCONNECTED);
    }
}

// 简单的对等设备/服务跟踪
static uint16_t g_svc_start_handle = 0;
static uint16_t g_svc_end_handle = 0;

/**
 * @brief 特征发现回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param chr 特征信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        // 打印 UUID 用于调试
        if (chr->uuid.u.type == BLE_UUID_TYPE_128) {
            const uint8_t *u = chr->uuid.u128.value;
            ESP_LOGI(TAG, "Found chr: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, def=%d, val=%d",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                     chr->def_handle, chr->val_handle);
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "Found 16-bit chr: 0x%04x, def=%d, val=%d",
                     chr->uuid.u16.value, chr->def_handle, chr->val_handle);
        }

        // 检查是否为写特征
        if (ble_uuid_cmp(&chr->uuid.u, &tesla_write_chr_uuid.u) == 0) {
            g_conn.write_handle = chr->val_handle;
            ESP_LOGI(TAG, "Write characteristic MATCHED: %d", g_conn.write_handle);
        }
        // 检查是否为读特征
        else if (ble_uuid_cmp(&chr->uuid.u, &tesla_read_chr_uuid.u) == 0) {
            g_conn.read_handle = chr->val_handle;
            ESP_LOGI(TAG, "Read characteristic MATCHED: %d", g_conn.read_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete");

        // 现在发现描述符以查找 CCCD
        if (g_conn.read_handle != 0) {
            // 订阅通知 - 首先查找 CCCD
            tesla_subscribe_notifications(conn_handle,
                                          g_conn.read_handle + TESLA_CCCD_HANDLE_OFFSET);
        }
    }
    return 0;
}

/**
 * @brief 服务发现回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param svc 服务信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc != NULL) {
        // 打印 UUID 用于调试
        if (svc->uuid.u.type == BLE_UUID_TYPE_128) {
            const uint8_t *u = svc->uuid.u128.value;
            ESP_LOGI(TAG, "Found 128-bit service: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, start=%d, end=%d",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                     svc->start_handle, svc->end_handle);

            const uint8_t *e = tesla_svc_uuid.value;
            ESP_LOGI(TAG, "Expected Tesla svc: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
                     e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15]);
        } else if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "Found 16-bit service: 0x%04x, start=%d, end=%d",
                     svc->uuid.u16.value, svc->start_handle, svc->end_handle);
        }

        if (ble_uuid_cmp(&svc->uuid.u, &tesla_svc_uuid.u) == 0) {
            ESP_LOGI(TAG, "Tesla service MATCHED!");
            g_svc_start_handle = svc->start_handle;
            g_svc_end_handle = svc->end_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery complete");

        if (g_svc_start_handle != 0) {
            // 发现特征
            int rc = ble_gattc_disc_all_chrs(conn_handle, g_svc_start_handle,
                                              g_svc_end_handle, tesla_on_chr_disc, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover characteristics: %d", rc);
            }
        } else {
            ESP_LOGE(TAG, "Tesla service not found!");
        }
    }
    return 0;
}

// ========== GAP 事件处理器 ==========

/**
 * @brief GAP 事件处理器
 * @param event GAP 事件
 * @param arg 用户参数
 * @return 0
 */
static int tesla_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        // 检查是否为目标 Tesla
        if (check_tesla_adv(event->disc.data, event->disc.length_data, g_target_local_name)) {
            ESP_LOGI(TAG, "Found target Tesla! RSSI=%d", event->disc.rssi);
            g_target_addr = event->disc.addr;

            // 停止扫描
            ble_gap_disc_cancel();

            // 连接
            uint8_t own_addr_type;
            ble_hs_id_infer_auto(0, &own_addr_type);

            rc = ble_gap_connect(own_addr_type, &g_target_addr, 30000, NULL,
                                tesla_gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to connect: %d", rc);
            } else {
                notify_state_change(TESLA_STATE_CONNECTING);
            }
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected!");
            g_conn.conn_handle = event->connect.conn_handle;
            g_conn.mtu = 23;  // 交换前的默认 BLE MTU

            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "Peer addr: %02x:%02x:%02x:%02x:%02x:%02x",
                         desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                         desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                         desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            }

            // 请求更快的连接参数以获得更好的吞吐量
            // min_interval=6 (7.5ms), max_interval=12 (15ms), latency=0, timeout=200 (2s)
            struct ble_gap_upd_params conn_params = {
                .itvl_min = 6,
                .itvl_max = 12,
                .latency = 0,
                .supervision_timeout = 200,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            rc = ble_gap_update_params(event->connect.conn_handle, &conn_params);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to request conn param update: %d", rc);
            }

            notify_state_change(TESLA_STATE_CONNECTED);

            // 启动服务发现
            rc = ble_gattc_disc_all_svcs(event->connect.conn_handle,
                                          tesla_on_svc_disc, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover services: %d", rc);
            }
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            notify_state_change(TESLA_STATE_DISCONNECTED);
            // 重试扫描
            tesla_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        g_conn.conn_handle = 0;
        g_conn.write_handle = 0;
        g_conn.read_handle = 0;

        // MTU 是每条连接各自协商的，断线重连后要重新判定是否还能一次读多类状态
        s_states_split = false;

        // 关键：会话必须整条作废。
        // 重连后上层只会重建 VCSEC 会话（session[0]），若这里保留 established=1，
        // 车机会话（session[1]）就永远停留在上一次连接的 key/epoch/counter 上，
        // 后续所有车机请求都用失效密钥发出 —— 表现为重连后车机功能永久失效，只能重启。
        memset(g_conn.session, 0, sizeof(g_conn.session));
        g_conn.rx_len = 0;
        s_last_rx_ms = 0;
        memset(g_conn.last_request_id, 0, sizeof(g_conn.last_request_id));
        g_reqid_write = 0;
        g_reqid_valid = 0;
        notify_state_change(TESLA_STATE_DISCONNECTED);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            ESP_LOGD(TAG, "Notification received, handle=%d, len=%d", event->notify_rx.attr_handle, len);

            // 累积数据到接收缓冲区（多个通知可能形成一条消息）
            // 这里是 NimBLE 主机任务上下文，长时间阻塞会让所有 GATT 过程停摆，
            // 之前给了 1 秒，最坏情况下整条连接卡死一次。拷贝本身很快，给 100ms 足够。
            BaseType_t got_mutex = xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100));
            if (got_mutex == pdTRUE) {
                uint32_t now_ms = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
                // 距上一次收包超过 1 秒，说明缓冲里剩下的是一条没到齐的半消息
                //（分块丢了 / 车机中断），这一包是新消息的开头，残帧必须丢掉，
                // 否则新数据只会拼在残帧后面，长度头读出来必然是垃圾值
                if (g_conn.rx_len > 0 && s_last_rx_ms != 0 &&
                    (now_ms - s_last_rx_ms) > TESLA_RX_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "收包间隔 %u ms 超时，丢弃 %u 字节残帧重新同步",
                             (unsigned)(now_ms - s_last_rx_ms), (unsigned)g_conn.rx_len);
                    g_conn.rx_len = 0;
                }
                s_last_rx_ms = now_ms;

                if (g_conn.rx_len + len > sizeof(g_conn.rx_buffer)) {
                    // 溢出时不能只是"丢弃这一包"：留在缓冲里的半条消息会和新数据拼在一起，
                    // 下一次 wait_for_response 会把拼出来的字节当长度头，
                    // 于是读出 26624/54428 这种根本不可能的消息长度。整段复位重新同步。
                    ESP_LOGW(TAG, "RX 缓冲溢出(%u+%u)，复位重新同步",
                             (unsigned)g_conn.rx_len, (unsigned)len);
                    g_conn.rx_len = 0;
                }
                if (len <= sizeof(g_conn.rx_buffer)) {
                    os_mbuf_copydata(event->notify_rx.om, 0, len, g_conn.rx_buffer + g_conn.rx_len);
                    g_conn.rx_len += len;
                }
                xSemaphoreGive(g_mutex);

                if (g_rx_sem) {
                    xSemaphoreGive(g_rx_sem);
                }
            } else {
                ESP_LOGE(TAG, "Failed to get mutex in notification handler!");
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        g_conn.mtu = event->mtu.value;
        if (event->mtu.value < 128) {
            // 车机是按协商出的 MTU 判断响应能不能发回来的，MTU 太小所有 GetVehicleData
            // 都会被拒（fault=25 RESPONSE_MTU_EXCEEDED），这里把原因直接说出来
            ESP_LOGW(TAG, "协商的 MTU 偏小(%d)，车机可能拒绝返回车辆状态",
                     event->mtu.value);
        }
        return 0;

    default:
        return 0;
    }
}

// ========== NimBLE 主机任务 ==========

/**
 * @brief BLE 主机重置回调
 * @param reason 重置原因
 */
static void tesla_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

/**
 * @brief BLE 主机同步回调
 */
static void tesla_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
    }
    ESP_LOGI(TAG, "BLE host synced");
}

/**
 * @brief BLE 主机任务
 * @param param 任务参数
 */
static void tesla_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ========== 公共 API ==========

esp_err_t tesla_ble_init(void)
{
    esp_err_t ret;

    // 创建信号量和互斥锁
    g_mutex = xSemaphoreCreateMutex();
    g_rx_sem = xSemaphoreCreateBinary();
    g_tx_sem = xSemaphoreCreateBinary();
    g_tx_lock = xSemaphoreCreateMutex();
    // 递归锁：infotainment_transact 持有它时可能还要进去建车机会话，
    // 普通互斥锁在这种嵌套下会直接自锁
    g_transact_lock = xSemaphoreCreateRecursiveMutex();
    if (!g_mutex || !g_rx_sem || !g_tx_sem || !g_tx_lock || !g_transact_lock) {
        return ESP_ERR_NO_MEM;
    }

    // 初始化 NimBLE 端口
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %d", ret);
        return ret;
    }

    // NimBLE 每发一条消息都会打印 "GATT procedure initiated"/"att_handle=..."，
    // 轮询时会把串口刷满，掩盖真正的错误日志。只在有告警以上时才输出。
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // 设置回调函数
    ble_hs_cfg.reset_cb = tesla_on_reset;
    ble_hs_cfg.sync_cb = tesla_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    // 初始化 GAP 服务（仅用于外设角色）
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("Tesla-ESP32");
#endif

    // 启动 NimBLE 主机任务
    nimble_port_freertos_init(tesla_host_task);

    // 生成路由地址
    generate_routing_address(g_conn.routing_address);

    ESP_LOGI(TAG, "Tesla BLE initialized");
    return ESP_OK;
}


/**
 * @brief 设置状态变化回调函数
 * @param callback 回调函数指针
 * @param arg 用户参数
 */
void tesla_ble_set_state_callback(tesla_state_callback_t callback, void *arg)
{
    g_state_cb = callback;
    g_state_cb_arg = arg;
}


/**
 * @brief 加载或生成密钥对
 * @param key_data 私钥数据（32 字节），传入 NULL 则生成新密钥
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_load_key(const uint8_t *key_data)
{
    if (key_data) {
        // 加载现有私钥
        memcpy(g_conn.private_key, key_data, 32);
        // 从私钥派生公钥
        if (!tesla_crypto_derive_public_key(g_conn.private_key, g_conn.public_key)) {
            ESP_LOGE(TAG, "Failed to derive public key from private key");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Loaded private key and derived public key");
    } else {
        // 生成新的密钥对
        if (!tesla_crypto_generate_keypair(g_conn.private_key, g_conn.public_key)) {
            ESP_LOGE(TAG, "Failed to generate key pair");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Generated new key pair");
    }

    return ESP_OK;
}


/**
 * @brief 获取私钥（用于保存到 NVS）
 * @param key_data 私钥缓冲区（32 字节）
 * @return ESP_OK 成功
 */
esp_err_t tesla_ble_get_private_key(uint8_t *key_data)
{
    if (!key_data) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(key_data, g_conn.private_key, TESLA_PRIVATE_KEY_LEN);
    return ESP_OK;
}

/**
 * @brief 从 VIN 计算车辆的 BLE 本地名称
 * @param vin 车辆识别码（17 个字符）
 * @param local_name 输出本地名称的缓冲区（至少 20 字节）
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t tesla_ble_get_local_name(const char *vin, char *local_name)
{
    if (!vin || strlen(vin) != 17 || !local_name) {
        return ESP_ERR_INVALID_ARG;
    }

    // 计算 VIN 的 SHA1 并取前 8 字节作为十六进制
    uint8_t hash[20];
    tesla_crypto_sha1((const uint8_t *)vin, 17, hash);

    // 格式：S + SHA1 前 8 字节的十六进制 + C
    sprintf(local_name, "S%02x%02x%02x%02x%02x%02x%02x%02xC",
            hash[0], hash[1], hash[2], hash[3],
            hash[4], hash[5], hash[6], hash[7]);

    return ESP_OK;
}

/**
 * @brief 开始扫描 Tesla 车辆
 * @param vin 目标车辆的 VIN 码
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t tesla_ble_scan(const char *vin)
{
    if (!vin || strlen(vin) != 17) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(g_target_vin, vin, sizeof(g_target_vin) - 1);
    strncpy(g_conn.vin, vin, sizeof(g_conn.vin) - 1);

    tesla_ble_get_local_name(vin, g_target_local_name);
    ESP_LOGI(TAG, "Target local name: %s", g_target_local_name);

    tesla_scan();

    return ESP_OK;
}

/**
 * @brief 停止扫描
 */
void tesla_ble_scan_stop(void)
{
    ble_gap_disc_cancel();
}


/**
 * @brief 断开与车辆的连接
 */
void tesla_ble_disconnect(void)
{
    if (g_conn.conn_handle != 0) {
        ble_gap_terminate(g_conn.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/**
 * @brief 获取当前连接状态
 * @return 当前连接状态
 */
tesla_conn_state_t tesla_ble_get_state(void)
{
    return g_conn.state;
}

/**
 * @brief 获取当前车辆状态
 * @param state 输出车辆状态结构体
 * @return ESP_OK 状态有效，ESP_ERR_INVALID_STATE 状态无效，ESP_ERR_TIMEOUT 超时
 */
esp_err_t tesla_ble_get_vehicle_state(tesla_vehicle_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(state, &g_vehicle_state, sizeof(tesla_vehicle_state_t));
        xSemaphoreGive(g_mutex);
        return g_vehicle_state.valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_TIMEOUT;
}

// ========== 会话管理 ==========

/**
 * @brief 从接收缓冲里取出的单条完整消息
 *
 * g_conn.rx_buffer 是持续被 BLE 主机任务追加的共享缓冲，直接把它的指针交给
 * 调用方再解锁，就等于任由数据在解析过程中被改写。取出的消息先拷到这里，
 * 调用方一律读 g_rx_msg / g_rx_msg_len。
 */
static uint8_t g_rx_msg[TESLA_MAX_MESSAGE_SIZE];
static uint32_t g_rx_msg_len = 0;

static void rx_discard(void);

/**
 * @brief 等待响应
 * @param timeout_ms 超时时间（毫秒）
 * @return true 接收到完整响应（内容在 g_rx_msg / g_rx_msg_len 中）
 * 注意：调用者应在发送请求前清除接收缓冲区
 * 不要在此处清除接收缓冲区，因为响应可能已经到达
 */
static bool wait_for_response(uint32_t timeout_ms)
{
    ESP_LOGD(TAG, "Waiting for response (timeout=%lu ms)...", (unsigned long)timeout_ms);

    // 等待完整消息（可能分多个通知到达）
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout) {
        // 首先检查是否已有完整消息
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_conn.rx_len >= 2) {
                uint16_t msg_len = (g_conn.rx_buffer[0] << 8) | g_conn.rx_buffer[1];
                if (msg_len > sizeof(g_conn.rx_buffer) - 2) {
                    // 长度头明显不是合法消息：多半是上次残留的字节被当成了新消息开头，
                    // 丢掉整段重新同步，否则后面每一次等待都会卡在同一处
                    ESP_LOGW(TAG, "长度头异常(%u)，丢弃缓冲重新同步", msg_len);
                    g_conn.rx_len = 0;
                } else if (msg_len > 0 && g_conn.rx_len >= 2 + msg_len) {
                    if (msg_len > sizeof(g_rx_msg)) {
                        ESP_LOGW(TAG, "消息 %u 字节超出接收缓冲，丢弃", msg_len);
                        g_conn.rx_len = 0;
                        xSemaphoreGive(g_mutex);
                        return false;
                    }
                    // 先把这条消息拷成私有副本再放开锁：
                    // 调用方拿到的是 g_rx_msg，不再直接读 g_conn.rx_buffer ——
                    // 后者会被 BLE 主机任务在解锁后立刻改写。
                    uint16_t remain = g_conn.rx_len - (uint16_t)(2 + msg_len);
                    memcpy(g_rx_msg, g_conn.rx_buffer + 2, msg_len);
                    g_rx_msg_len = msg_len;
                    // 后续字节（下一条响应 / VCSEC 广播）必须整体前移到缓冲开头。
                    // 接收端永远从 rx_len 处追加，若把保留字节留在原处，
                    // 下一次通知就会从 rx_len 写起、把它们覆盖掉，只剩残帧。
                    if (remain > 0) {
                        memmove(g_conn.rx_buffer, g_conn.rx_buffer + 2 + msg_len, remain);
                    }
                    g_conn.rx_len = remain;
                    xSemaphoreGive(g_mutex);
                    ESP_LOGD(TAG, "Complete message received: %d bytes", msg_len);
                    return true;
                }
            }
            xSemaphoreGive(g_mutex);
        }

        // 等待新数据，使用较短超时以快速反应
        xSemaphoreTake(g_rx_sem, pdMS_TO_TICKS(20));
    }

    // 超时：残留的半条消息必须丢掉。留着它，下一次 wait_for_response 会把它的
    // 长度头当成新消息的开头（日志里的 "Final rx_len=374"），之后永远等不齐。
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ESP_LOGW(TAG, "Timeout! Final rx_len=%d", g_conn.rx_len);
        g_conn.rx_len = 0;
        xSemaphoreGive(g_mutex);
    }
    return false;
}

/**
 * @brief 把刚发出的 request_id 记入历史
 *
 * 响应可能晚于请求到达（请求超时重发、通知排队、ui 任务插队发送），
 * 到时必须能把响应和更早的请求配对，否则 request_hash 对不上、GCM 认证失败。
 */
static void remember_request_id(const uint8_t *id)
{
    if (!id || !g_mutex) return;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    memcpy(g_reqid_hist[g_reqid_write], id, 17);
    g_reqid_write = (g_reqid_write + 1) % TESLA_REQID_HISTORY;
    if (g_reqid_valid < TESLA_REQID_HISTORY) g_reqid_valid++;

    xSemaphoreGive(g_mutex);
}

/**
 * @brief 解密响应：按发送方域选会话，再依次尝试最近的 request_id
 *
 * 响应的 AAD 里带请求的 request_hash，用错 request_id 一定解不开；
 * 而响应的加密密钥是"发送方域"的会话密钥，所以先用 from_destination 定域：
 * 域与本次请求不符的消息直接丢弃（等真正的响应），不要拿去试探解密——
 * 否则 VCSEC 的车身状态响应会被 DriveState 流程"成功"解密，最后报
 * "DriveState not found"。
 *
 * @param expect_domain 本次请求的目标域
 */
static bool decrypt_response(const uint8_t *data, uint32_t len,
                             tesla_domain_t expect_domain,
                             uint8_t *plaintext, size_t plaintext_cap,
                             uint32_t *plaintext_len,
                             tesla_fault_t *fault)
{
    if (!plaintext || !plaintext_len || !fault) return false;

    uint8_t ids[TESLA_REQID_HISTORY + 1][TESLA_REQUEST_ID_LEN];
    uint8_t hist[TESLA_REQID_HISTORY][TESLA_REQUEST_ID_LEN];
    int n = 0, hw = 0, hv = 0;

    // 先按域过滤：多域同时有未完成请求时，缓冲区里可能是另一个域的响应
    tesla_domain_t from_domain;
    if (tesla_routable_from_domain(data, len, &from_domain) &&
        from_domain != expect_domain) {
        ESP_LOGD(TAG, "忽略来自域 %d 的消息（期望域 %d）",
                 (int)from_domain, (int)expect_domain);
        return false;
    }

    // session[0] = VCSEC，session[1] = Infotainment
    tesla_session_t *session = &g_conn.session[expect_domain == TESLA_DOMAIN_INFOTAINMENT ? 1 : 0];
    if (!session->established) {
        return false;
    }

    // 候选 1：当前 request_id
    memcpy(ids[n++], g_conn.last_request_id, TESLA_REQUEST_ID_LEN);

    if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // 拷贝到栈上后立刻解锁：下面逐个尝试解密可能跑好几轮 GCM，
        // 一直持锁会把 NimBLE 主机任务的通知处理挡在外面
        for (int k = 0; k < TESLA_REQID_HISTORY; k++) {
            memcpy(hist[k], g_reqid_hist[k], TESLA_REQUEST_ID_LEN);
        }
        hw = g_reqid_write;
        hv = g_reqid_valid;
        xSemaphoreGive(g_mutex);
    }

    // 候选 2..N：历史 request_id，从新到旧
    for (int k = 0; k < hv && n < TESLA_REQID_HISTORY + 1; k++) {
        int idx = (hw - 1 - k + 2 * TESLA_REQID_HISTORY) % TESLA_REQID_HISTORY;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (memcmp(hist[idx], ids[j], TESLA_REQUEST_ID_LEN) == 0) { dup = true; break; }
        }
        if (!dup) memcpy(ids[n++], hist[idx], TESLA_REQUEST_ID_LEN);
    }

    *fault = TESLA_FAULT_NONE;
    for (int i = 0; i < n; i++) {
        tesla_fault_t f = TESLA_FAULT_NONE;
        if (tesla_parse_and_decrypt_response(data, len, session,
                                             g_conn.vin, ids[i],
                                             plaintext, (uint32_t)plaintext_cap,
                                             plaintext_len,
                                             // 只有 VCSEC 的周期性广播可以是明文，
                                             // 车机（Infotainment）响应必须带 GCM 认证
                                             expect_domain == TESLA_DOMAIN_VEHICLE_SECURITY,
                                             &f)) {
            *fault = f;
            if (i > 0) {
                ESP_LOGD(TAG, "响应配对到更早的请求（回退 %d 个 request_id）", i);
            }
            return true;
        }
        *fault = f;
    }

    ESP_LOGW(TAG, "解密失败：域=%d，已尝试 %d 个 request_id（消息 %lu 字节）",
             (int)expect_domain, n, (unsigned long)len);
    return false;
}

/**
 * @brief 发送消息
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return ESP_OK 成功
 */
static esp_err_t send_message(const uint8_t *data, uint16_t len)
{
    if (g_conn.write_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // 构建带 2 字节长度头的消息（大端序）
    uint8_t msg[1024];
    if (len + 2 > sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    msg[0] = (len >> 8) & 0xFF;
    msg[1] = len & 0xFF;
    memcpy(msg + 2, data, len);

    uint16_t total_len = len + 2;
    uint16_t offset = 0;
    // 使用协商的 MTU，如果未设置则使用默认值 20（最小 BLE MTU）
    // 减去 3 以考虑 ATT 头开销
    uint16_t mtu = (g_conn.mtu > 3) ? (g_conn.mtu - 3) : 20;

    ESP_LOGD(TAG, "Sending message: payload=%d, total=%d, mtu=%d", len, total_len, mtu);
    ESP_LOGD(TAG, "原始消息 (%d 字节):", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
    ESP_LOGD(TAG, "带长度前缀 (%d 字节):", total_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, total_len, ESP_LOG_DEBUG);

    // 分块写入必须整体串行化：tesla_task 与 ui_action_task 同时发送时，
    // 两条消息的块会交错写入同一个特征值，车辆收到的就是两条残缺消息。
    if (g_tx_lock && xSemaphoreTake(g_tx_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "TX busy, message dropped (%d bytes)", len);
        return ESP_ERR_TIMEOUT;
    }

    // 如需分块发送
    // 使用无响应写入（写命令）以获得更快的吞吐量
    // 这样可以避免等待车辆对每个块的确认
    while (offset < total_len) {
        uint16_t chunk_len = (total_len - offset > mtu) ? mtu : (total_len - offset);

        int rc = ble_gattc_write_no_rsp_flat(g_conn.conn_handle, g_conn.write_handle,
                                              msg + offset, chunk_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "Write failed: %d", rc);
            if (g_tx_lock) xSemaphoreGive(g_tx_lock);
            return ESP_FAIL;
        }

        offset += chunk_len;
    }

    if (g_tx_lock) xSemaphoreGive(g_tx_lock);

    // 每条请求都成功发送，成功日志会把串口刷满，只在 DEBUG 级别保留
    ESP_LOGD(TAG, "Message sent successfully (%d bytes)", len);
    return ESP_OK;
}

/**
 * @brief 与车辆建立加密会话（密钥交换）
 * @param domain 目标域（VCSEC 或 Infotainment）
 * @return ESP_OK 成功，其他值失败
 */
static esp_err_t start_session_impl(tesla_domain_t domain)
{
    uint8_t buf[256];
    uint32_t len;

    int idx = (domain == TESLA_DOMAIN_INFOTAINMENT) ? 1 : 0;
    tesla_session_t *session = &g_conn.session[idx];

    // 在发送会话请求前清除所有待处理的通知
    ESP_LOGI(TAG, "Starting session for domain %d...", domain);
    rx_discard();

    // 构建会话信息请求
    len = tesla_build_session_info_request(buf, sizeof(buf), domain,
                                            g_conn.public_key,
                                            g_conn.routing_address);
    if (len == 0) {
        return ESP_FAIL;
    }

    // 发送请求
    esp_err_t ret = send_message(buf, len);
    if (ret != ESP_OK) {
        return ret;
    }

    // 等待响应，使用重试逻辑跳过 VCSEC 广播
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TESLA_WAIT_SESSION_MS);

    while ((xTaskGetTickCount() - start) < timeout) {
        // 等待消息
        if (!wait_for_response(2000)) {
            ESP_LOGW(TAG, "No response yet, continuing to wait...");
            continue;
        }

        // 检查这是否是 VCSEC 广播（字段 7 from_destination = 域 2）
        // 而不是会话信息响应
        if (g_rx_msg_len >= 8) {
            const uint8_t *p = g_rx_msg;
            // 检查字段 6 (to_destination): 32 02 08 XX
            // 检查字段 7 (from_destination): 3a 02 08 XX
            if (p[0] == 0x32 && p[4] == 0x3a && p[6] == 0x08) {
                uint8_t from_domain = p[7];
                ESP_LOGI(TAG, "Received message from domain %d", from_domain);

                // 检查这是否是会话信息响应（有字段 15，或字段 7a = 0x7a）
                // 会话信息响应将有 session_info 字段（15 = protobuf 中的 0x7a）
                bool has_session_info = false;
                for (int i = 0; i < (int)g_rx_msg_len - 1; i++) {
                    if (g_rx_msg[i] == 0x7a) {  // 字段 15, wire type 2 = (15 << 3) | 2 = 122 = 0x7a
                        has_session_info = true;
                        break;
                    }
                }

                if (!has_session_info) {
                    ESP_LOGW(TAG, "Received VCSEC broadcast (no session_info), skipping...");
                    continue;
                }
            }
        }

        // 尝试解析会话信息
        if (tesla_parse_session_info(g_rx_msg, g_rx_msg_len, session)) {
            // 成功！计算 ECDH 共享密钥
            if (!tesla_crypto_ecdh(g_conn.private_key, session->vehicle_public_key,
                                   session->shared_secret)) {
                ESP_LOGE(TAG, "ECDH failed");
                return ESP_FAIL;
            }

            // 派生会话密钥
            tesla_crypto_derive_key(session->shared_secret, session->session_key);

            // 调试：会话信息仅 DEBUG 级别输出，避免泄露密钥
            ESP_LOGD(TAG, "=== Session Debug Info ===");
            ESP_LOGD(TAG, "  Shared secret:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->shared_secret, 32, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Session key (16 bytes):");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->session_key, 16, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Vehicle public key:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->vehicle_public_key, 65, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Epoch:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->epoch, 16, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Counter=%lu, Clock=%lu",
                     (unsigned long)session->counter, (unsigned long)session->clock_time);

            // 记录会话建立时的本地时间（用于 expires_at 计算）
            session->local_time_sec = (uint32_t)(((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000);

            session->established = true;
            ESP_LOGI(TAG, "Session established for domain %d", domain);

            if (domain == TESLA_DOMAIN_VEHICLE_SECURITY) {
                notify_state_change(TESLA_STATE_AUTHENTICATED);
            }

            return ESP_OK;
        }

        ESP_LOGW(TAG, "Failed to parse session info, waiting for correct response...");
        rx_discard();
    }

    ESP_LOGE(TAG, "Session info request timeout after retries");
    return ESP_ERR_TIMEOUT;
}

esp_err_t tesla_ble_start_session(tesla_domain_t domain)
{
    // 建会话同样要独占整条接收通路：它要在共享缓冲里等并解析会话响应。
    // 递归锁保证从 infotainment_transact 内部嵌套调用时不会自锁。
    if (g_transact_lock &&
        xSemaphoreTakeRecursive(g_transact_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "等待事务锁超时，无法建立域 %d 会话", (int)domain);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = start_session_impl(domain);
    xSemaphoreGiveRecursive(g_transact_lock);
    return ret;
}

// ========== 车辆命令 ==========

/**
 * @brief 发送添加密钥请求（配对）
 * @return ESP_OK 成功，其他值失败
 * 
 * 向车辆发送配对请求，需要用户在车辆屏幕上确认并使用 NFC 卡验证
 */
esp_err_t tesla_ble_add_key_request(void)
{
    uint8_t buf[512];  // 增加缓冲区大小以容纳包装的消息
    uint32_t len;

    // 清除车辆的待处理通知（重要！）
    // 订阅后车辆可能发送非请求消息
    ESP_LOGI(TAG, "Clearing pending notifications before sending add key request...");
    while (xSemaphoreTake(g_rx_sem, 0) == pdTRUE) {}  // 清空信号量
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_conn.rx_len = 0;
        xSemaphoreGive(g_mutex);
    }
    // 等待一段时间让正在传输的通知到达并被丢弃
    vTaskDelay(pdMS_TO_TICKS(200));
    while (xSemaphoreTake(g_rx_sem, 0) == pdTRUE) {}
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_conn.rx_len = 0;
        xSemaphoreGive(g_mutex);
    }

    ESP_LOGI(TAG, "Building add key request...");
    ESP_LOGD(TAG, "Public key (65 bytes):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, g_conn.public_key, 65, ESP_LOG_DEBUG);

    // 验证公钥格式（未压缩格式应以 0x04 开头）
    if (g_conn.public_key[0] != 0x04) {
        ESP_LOGE(TAG, "Invalid public key format! First byte should be 0x04, got 0x%02x",
                 g_conn.public_key[0]);
        return ESP_FAIL;
    }

    len = tesla_build_add_key_request(buf, sizeof(buf), g_conn.public_key,
                                       TESLA_KEY_CLOUD_KEY);
    if (len == 0) {
        ESP_LOGE(TAG, "Failed to build add key request");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Add key request built, length=%lu", (unsigned long)len);
    ESP_LOGD(TAG, "Full message hex:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, len, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Sending add key request to vehicle...");

    esp_err_t ret = send_message(buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send add key request: %d", ret);
        return ret;
    }

    // 与 Go SDK 一样，我们不在此处等待响应。
    // 添加密钥请求会触发车辆屏幕上的用户确认。
    // 成功通过稍后尝试启动会话来验证。
    ESP_LOGI(TAG, "Add key request sent successfully");
    ESP_LOGI(TAG, "Note: Pairing requires user confirmation on vehicle screen with NFC card");

    return ESP_OK;
}

/**
 * @brief 获取车身控制器状态（VCSEC）
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 超时，ESP_FAIL 失败
 * 
 * 获取车辆锁定状态、车门状态等信息
 */
/**
 * @brief 一次完整的「清缓冲 → 发送 → 等响应 → 解密」事务
 *
 * 单接收缓冲 + 单 request_id 历史决定了这条链路不能被别的任务插进来：
 * 两个任务（轮询 / UI 控制）如果交错走到 rx_discard()，会把对方刚收到的响应清掉。
 * 所以整条事务锁在递归锁里。VCSEC 与车机两个域的流程除了下面这几个参数外
 * 完全一样，统一在这里维护，改一处两域都生效。
 */
typedef struct {
    tesla_domain_t domain;      // 请求发往哪个域
    int session_idx;            // 用哪个会话槽（0=VCSEC，1=车机）
    bool ensure_session;        // 会话未建立时是否自动建立
    uint32_t timeout_ms;
    int attempts;               // 含首次，至少 1
    bool check_domain;          // 是否丢弃域不匹配的响应再重试
} tesla_transact_t;

/**
 * 官方 SDK（internal/authentication/error.go 的 retriableErrors + protocol.ShouldRetry）
 * 把这些故障码归为"可重试"：车端认为 counter / epoch / token 失步，响应里通常还会
 * 附带一份新的 session info，客户端同步后用新 counter 重新签名重发
 * （signer.go 的 UpdateSessionInfo / Encrypt）。
 * 本项目没有解析 piggyback session info，等价兜底是重新握手拿一份全新会话，
 * 否则旧会话会一直被拒到 BLE 重连/重启（日志里 fault=6 连着出现就是这么来的）。
 */
static bool is_auth_desync_fault(tesla_fault_t f)
{
    switch (f) {
    case TESLA_FAULT_INVALID_SIGNATURE:   // 5  签名/MAC 错，用 session info 更新后重试
    case TESLA_FAULT_INVALID_TOKEN:       // 6  防重放计数已用过，更新 session 后重试
    case TESLA_FAULT_INCORRECT_EPOCH:     // 15 车端换了 epoch
    case TESLA_FAULT_TIME_EXPIRED:        // 17 命令已过期
    case TESLA_FAULT_REPEATED_COUNTER:    // 26 计数器重复
        return true;
    default:
        return false;
    }
}

static esp_err_t transact(const tesla_transact_t *t,
                          const uint8_t *payload, uint32_t payload_len,
                          uint8_t *out, size_t out_cap, uint32_t *out_len,
                          tesla_fault_t *fault_out)
{
    if (fault_out) {
        *fault_out = TESLA_FAULT_NONE;
    }
    if (!t || !payload || payload_len == 0 || !out || !out_len || out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    tesla_session_t *session = &g_conn.session[t->session_idx];
    if (!session->established) {
        if (!t->ensure_session) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t ret = tesla_ble_start_session(t->domain);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "建立域 %d 会话失败: %s", (int)t->domain, esp_err_to_name(ret));
            return ret;
        }
    }

    if (g_transact_lock &&
        xSemaphoreTakeRecursive(g_transact_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "等待事务锁超时");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t tx[768];
    const uint32_t flags = (1u << TESLA_FLAG_ENCRYPT_RESPONSE);
    esp_err_t result = ESP_FAIL;
    // t->attempts 是调用方给的常规重试次数；认证失步时允许再追加一次
    // "重新握手 + 重新签名"的重试，且最多一次（见 is_auth_desync_fault）。
    int attempts = t->attempts;
    bool auth_retried = false;

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) {
            // 退避一下再重试：立刻重发常常还是撞在同一个 MTU/超时上
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        // 发送前清空残留通知，避免把上一条响应当成本次结果
        rx_discard();

        session->counter++;
        uint32_t tx_len = tesla_build_routable_message(tx, sizeof(tx), t->domain,
                                                       payload, payload_len, session,
                                                       g_conn.routing_address,
                                                       g_conn.public_key, g_conn.vin,
                                                       flags, g_conn.last_request_id);
        if (tx_len == 0) {
            result = ESP_FAIL;
            break;
        }
        remember_request_id(g_conn.last_request_id);

        esp_err_t ret = send_message(tx, (uint16_t)tx_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "发送请求失败: %s", esp_err_to_name(ret));
            // 保持真实错误码：上层要能区分"写不进去"和"没等到响应"
            result = ret;
            continue;
        }
        if (!wait_for_response(t->timeout_ms)) {
            ESP_LOGW(TAG, "域 %d 响应超时 %lums", (int)t->domain,
                     (unsigned long)t->timeout_ms);
            result = ESP_ERR_TIMEOUT;
            continue;
        }
        if (t->check_domain) {
            tesla_domain_t got = t->domain;
            if (tesla_routable_from_domain(g_rx_msg, g_rx_msg_len, &got) &&
                got != t->domain) {
                ESP_LOGD(TAG, "收到域 %d 的消息，丢弃后重试", (int)got);
                result = ESP_ERR_INVALID_RESPONSE;
                continue;
            }
        }

        uint32_t dec_len = 0;
        tesla_fault_t fault = TESLA_FAULT_NONE;
        if (!decrypt_response(g_rx_msg, g_rx_msg_len, t->domain,
                              out, out_cap, &dec_len, &fault)) {
            ESP_LOGW(TAG, "域 %d 响应解密失败(fault=%d)", (int)t->domain, (int)fault);
            result = ESP_FAIL;
            continue;
        }
        // 回了故障码就说明请求被拒，明文里一定没有有效数据。
        // 原来这里照样返回 ESP_OK，上层只能在 parse 失败后空转轮询。
        if (fault != TESLA_FAULT_NONE) {
            ESP_LOGW(TAG, "域 %d 拒绝请求 fault=%d(%s)", (int)t->domain, (int)fault,
                     tesla_fault_str(fault));
            if (fault_out) {
                *fault_out = fault;
            }
            *out_len = 0;
            result = (fault == TESLA_FAULT_RESPONSE_MTU_EXCEEDED)
                     ? ESP_ERR_INVALID_SIZE : ESP_FAIL;

            // 官方 SDK 遇到认证失步会先用响应里的 session info 同步 counter/epoch，
            // 再用新 counter 重新签名重发。本项目没解析 piggyback session info，
            // 退一步重新握手：拿一份全新的 counter/epoch/会话密钥，下一轮循环
            // 自会 counter++ 并重新打包发送。
            if (!auth_retried && is_auth_desync_fault(fault)) {
                auth_retried = true;
                attempts++;                 // 追加一次重试
                // 不手动清 session->established：start_session_impl 成功时会整体刷新
                // 会话并置位，失败时旧会话保持原样，下一轮事务还能再触发一次恢复，
                // 不会把 get_body_state(ensure_session=false) 卡在"会话未建立"上。
                ESP_LOGW(TAG, "域 %d 认证失步，重新握手后再试一次", (int)t->domain);
                esp_err_t sr = tesla_ble_start_session(t->domain);
                if (sr != ESP_OK) {
                    ESP_LOGE(TAG, "重建域 %d 会话失败: %s", (int)t->domain,
                             esp_err_to_name(sr));
                    break;
                }
                continue;
            }
            break;
        }
        rx_discard();
        *out_len = dec_len;
        if (fault_out) {
            *fault_out = TESLA_FAULT_NONE;   // 重试成功后别把上一轮的故障码留给上层
        }
        result = ESP_OK;
        break;
    }

    xSemaphoreGiveRecursive(g_transact_lock);
    return result;
}

esp_err_t tesla_ble_get_body_state(void)
{
    uint8_t payload[64];
    uint32_t payload_len = tesla_build_info_request(payload, sizeof(payload),
                                                    TESLA_INFO_GET_STATUS);
    if (payload_len == 0) {
        return ESP_FAIL;
    }

    // 必须走已建立的 VCSEC 域会话（加密）。
    // 早期版本用的是"未认证明文"方式，车机在已有加密会话时不会回这个请求，
    // 结果就是闭件状态永远读不到，界面只能一直显示 "--"。
    const tesla_transact_t t = {
        .domain = TESLA_DOMAIN_VEHICLE_SECURITY,
        .session_idx = 0,
        .ensure_session = false,
        .timeout_ms = TESLA_WAIT_BODY_MS,   // 正常响应 300ms 内，1 秒不至于拖慢主轮询
        .attempts = 1,
        .check_domain = false,
    };

    uint8_t decrypted[TESLA_MAX_PLAINTEXT];
    uint32_t decrypted_len = 0;
    esp_err_t ret = transact(&t, payload, payload_len, decrypted, sizeof(decrypted),
                             &decrypted_len, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VCSEC body state 失败: %s", esp_err_to_name(ret));
        return (ret == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    bool parsed = false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        parsed = tesla_parse_vehicle_status(decrypted, decrypted_len,
                                            &g_vehicle_state.body);
        if (parsed) {
            g_vehicle_state.valid = true;
        }
        xSemaphoreGive(g_mutex);
    }

    // 解析失败必须明确返回失败：否则闭件值全 0（=CLOSED），界面会误显示"已关闭"
    if (!parsed) {
        ESP_LOGW(TAG, "VCSEC body state parse failed");
    }
    return parsed ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 唤醒车辆
 * @return ESP_OK 成功，其他值失败
 * 
 * 从睡眠状态唤醒车辆
 */
/**
 * @brief 取 VCSEC 域会话，未建立就先建立
 *
 * 原先 lock/unlock/闭件在会话没建立时直接返回 ESP_ERR_INVALID_STATE，
 * 而唤醒命令会自己建会话。一旦会话中途失效（例如重连后只建了车机会话），
 * 这几个按键就再也发不出去，屏幕上除了"失败"看不出任何原因。这里统一处理。
 *
 * @return 已建立的会话；建立失败返回 NULL
 */
static tesla_session_t *vcsec_session(void)
{
    tesla_session_t *session = &g_conn.session[0];
    if (!session->established) {
        esp_err_t ret = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "建立 VCSEC 会话失败: %s", esp_err_to_name(ret));
            return NULL;
        }
    }
    return session;
}

/**
 * @brief 把一条 payload 包成 VCSEC 域消息发出去（不等待响应）
 *
 * @param payload 已构建好的 VCSEC payload
 * @param payload_len payload 长度
 * @return ESP_OK 成功，其他值失败
 */
static esp_err_t vcsec_send(const uint8_t *payload, uint32_t payload_len)
{
    tesla_session_t *session = vcsec_session();
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }

    // 官方 SDK 的 Signer.Encrypt/AuthorizeHMAC 对**每一条**认证消息无条件 counter++
    // （signer.go:174/188），counter 的分配与发送由 session 锁串行化；规范还要求
    // "VCSEC requires that messages arrive in counter order"（protocol.md:547-549）。
    // 以前这里既不递增 counter 也不拿事务锁：get_body_state 用 counter=N 发过之后，
    // 锁车/解锁仍用 counter=N 再发一条，车端按重放直接回 fault=6(INVALID_TOKEN)；
    // 而且它和 transact 能交错，双方的响应还会被对方的 rx_discard 清掉。
    if (g_transact_lock &&
        xSemaphoreTakeRecursive(g_transact_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "等待事务锁超时，无法发送 VCSEC 命令");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t tx[256];
    session->counter++;
    uint32_t tx_len = tesla_build_routable_message(tx, sizeof(tx),
                                                   TESLA_DOMAIN_VEHICLE_SECURITY,
                                                   payload, payload_len,
                                                   session,
                                                   g_conn.routing_address,
                                                   g_conn.public_key,
                                                   g_conn.vin,
                                                   0,
                                                   NULL);   // 不解密响应
    esp_err_t ret = ESP_FAIL;
    if (tx_len != 0) {
        rx_discard();   // 单缓冲：发之前清掉上一条事务的残留通知
        ret = send_message(tx, (uint16_t)tx_len);
    }
    xSemaphoreGiveRecursive(g_transact_lock);
    return ret;
}

/**
 * @brief 发一条 RKE 动作（唤醒 / 闭锁 / 解锁）到 VCSEC 域
 *
 * 三个动作除了 rke_action 不同其余完全一样，原来复制了三遍，
 * 改一处就要记得改另外两处。
 *
 * @param action RKE 动作
 * @return ESP_OK 成功，其他值失败
 */
static esp_err_t rke_send(tesla_rke_action_t action)
{
    uint8_t payload[64];
    uint32_t payload_len = tesla_build_rke_action(payload, sizeof(payload), action);
    if (payload_len == 0) {
        return ESP_FAIL;
    }
    return vcsec_send(payload, payload_len);
}

/**
 * @brief 唤醒车辆
 * @return ESP_OK 成功，其他值失败
 *
 * 从睡眠状态唤醒车辆
 */
esp_err_t tesla_ble_wake_vehicle(void)
{
    return rke_send(TESLA_RKE_WAKE);
}

/**
 * @brief 锁定车辆
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_FAIL 失败
 */
esp_err_t tesla_ble_lock(void)
{
    return rke_send(TESLA_RKE_LOCK);
}

/**
 * @brief 解锁车辆
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_FAIL 失败
 */
esp_err_t tesla_ble_unlock(void)
{
    return rke_send(TESLA_RKE_UNLOCK);
}




// ========== 状态打印 ==========


/* ==================== 车机域通用事务 ==================== */

static void rx_discard(void)
{
    while (xSemaphoreTake(g_rx_sem, 0) == pdTRUE) {
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_conn.rx_len = 0;
        xSemaphoreGive(g_mutex);
    }
    g_rx_msg_len = 0;
}

/**
 * @brief 向车机域发一条 CarServer 负载并取回解密后的响应
 *
 * g_conn.rx_buffer 是单缓冲，所有请求必须串行；这个函数把
 * "清缓冲 -> 打路由消息 -> 发送 -> 等响应 -> 按域过滤 -> 解密"收在一起，
 * 各状态读取与各控制动作共用。
 *
 * @param fault_out 可选，输出车机在 signedMessageStatus 里给的故障码
 * @return ESP_OK 车机接受请求并回了负载；车机回故障码时返回 ESP_FAIL，
 *         其中 fault=25（响应超过 MTU）返回 ESP_ERR_INVALID_SIZE，
 *         方便上层判断该把请求拆小重试。
 */
static esp_err_t infotainment_transact(const uint8_t *payload, uint32_t payload_len,
                                       uint8_t *out, size_t out_cap, uint32_t *out_len,
                                       uint32_t timeout_ms,
                                       tesla_fault_t *fault_out)
{
    const tesla_transact_t t = {
        .domain = TESLA_DOMAIN_INFOTAINMENT,
        .session_idx = 1,
        .ensure_session = true,     // 会话失效时自动重建，否则重连后车机功能会一直拿不到数据
        .timeout_ms = timeout_ms,
        .attempts = 2,
        .check_domain = true,
    };
    return transact(&t, payload, payload_len, out, out_cap, out_len, fault_out);
}

static esp_err_t refresh_states_once(uint32_t mask, tesla_fault_t *fault_out)
{
    uint8_t payload[64];
    uint32_t payload_len = tesla_build_get_states(payload, sizeof(payload), mask);
    if (payload_len == 0) {
        ESP_LOGE(TAG, "构建 GetVehicleData 失败");
        return ESP_FAIL;
    }

    uint8_t dec[TESLA_MAX_PLAINTEXT];
    uint32_t dec_len = 0;
    esp_err_t ret = infotainment_transact(payload, payload_len, dec, sizeof(dec),
                                          &dec_len, TESLA_WAIT_STATES_MS, fault_out);
    if (ret != ESP_OK) {
        return ret;
    }

    bool parsed = false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        // 只覆盖本次返回的那几类，present_mask 是累加的，逐类读不会互相清掉
        parsed = tesla_parse_vehicle_data(dec, dec_len, &g_vehicle_state, NULL);
        if (parsed) {
            g_vehicle_state.valid = true;
            g_vehicle_state.stamp_ms = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
        }
        xSemaphoreGive(g_mutex);
    }
    if (!parsed) {
        ESP_LOGW(TAG, "车机响应里没有可用状态（mask=0x%lx，明文 %u 字节）",
                 (unsigned long)mask, (unsigned)dec_len);
        if (dec_len > 0) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, dec, dec_len > 48 ? 48 : dec_len, ESP_LOG_WARN);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tesla_ble_refresh_states(uint32_t mask)
{
    if (mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 只清本次要读取的类别位，其它类别保留上一轮的结果。
    // 上层是分组轮询的（每轮只读 2~4 类，三组一轮），如果这里整块清零，
    // 本轮没读到的类别就会被当成"无数据"，格子在真实值和 "--" 之间来回闪。
    // 某类连续读不到时，它对应的位会在下一次轮询到它那轮被清掉，照样能及时降级。
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_vehicle_state.present_mask &= ~mask;
        xSemaphoreGive(g_mutex);
    }

    if (!s_states_split) {
        tesla_fault_t fault = TESLA_FAULT_NONE;
        esp_err_t ret = refresh_states_once(mask, &fault);
        if (ret != ESP_ERR_INVALID_SIZE || fault != TESLA_FAULT_RESPONSE_MTU_EXCEEDED) {
            return ret;
        }
        // 一次要多类时响应装不进 MTU，车机整体拒绝；改成一次一类逐个取。
        ESP_LOGW(TAG, "一次读 0x%lx 响应超过 MTU，本连接改为逐类读取",
                 (unsigned long)mask);
        s_states_split = true;
    }

    esp_err_t ret = ESP_FAIL;
    for (uint32_t bit = 0; bit < TESLA_STATE_CATEGORY_CNT; bit++) {
        uint32_t one = mask & (1u << bit);
        if (one == 0) {
            continue;
        }
        esp_err_t r = refresh_states_once(one, NULL);
        if (r == ESP_OK) {
            ret = ESP_OK;               // 至少拿到一类就算这一轮成功
        } else if (ret != ESP_OK) {
            ret = r;
        }
    }
    return ret;
}

/* ==================== 控制命令 ==================== */

static const tesla_command_info_t s_command_table[] = {
    {TESLA_CMD_NONE, "无"},
    {TESLA_CMD_LOCK_TOGGLE, "车锁开关"},
    {TESLA_CMD_TRUNK_TOGGLE, "后备箱开关"},
    {TESLA_CMD_FRUNK_TOGGLE, "前备箱开关"},
    {TESLA_CMD_WAKE, "唤醒车辆"},
    {TESLA_CMD_FLASH_LIGHTS, "闪灯"},
    {TESLA_CMD_HONK_HORN, "鸣笛"},
    {TESLA_CMD_CLIMATE_TOGGLE, "空调开关"},
    {TESLA_CMD_TEMP_UP, "温度 +"},
    {TESLA_CMD_TEMP_DOWN, "温度 -"},
    {TESLA_CMD_SEAT_HEATER_TOGGLE, "主驾座椅加热"},
    {TESLA_CMD_STEERING_HEAT_TOGGLE, "方向盘加热"},
    {TESLA_CMD_BIOWEAPON_TOGGLE, "生化防御"},
    {TESLA_CMD_COP_TOGGLE, "座舱过热保护"},
    {TESLA_CMD_PRECOND_MAX_TOGGLE, "最大预热/预冷"},
    {TESLA_CMD_CHARGE_TOGGLE, "充电开关"},
    {TESLA_CMD_CHARGE_PORT_TOGGLE, "充电口开关"},
    {TESLA_CMD_CHARGE_AMPS_UP, "充电电流 +"},
    {TESLA_CMD_CHARGE_AMPS_DOWN, "充电电流 -"},
    {TESLA_CMD_CHARGE_LIMIT_UP, "充电限值 +5%"},
    {TESLA_CMD_CHARGE_LIMIT_DOWN, "充电限值 -5%"},
    {TESLA_CMD_WINDOWS_CLOSE, "关闭车窗"},
    {TESLA_CMD_WINDOWS_VENT, "车窗通风"},
    {TESLA_CMD_SUNROOF_TOGGLE, "天窗开关"},
    {TESLA_CMD_MEDIA_PLAY_PAUSE, "播放/暂停"},
    {TESLA_CMD_MEDIA_NEXT, "下一曲"},
    {TESLA_CMD_MEDIA_PREV, "上一曲"},
    {TESLA_CMD_VOLUME_UP, "音量 +"},
    {TESLA_CMD_VOLUME_DOWN, "音量 -"},
    {TESLA_CMD_SENTRY_TOGGLE, "哨兵模式"},
    {TESLA_CMD_VALET_TOGGLE, "代客模式"},
    {TESLA_CMD_VALET_RESET_PIN, "重置代客 PIN"},
    {TESLA_CMD_LOW_POWER_TOGGLE, "低功耗模式(开)"},
    {TESLA_CMD_ACCESSORY_POWER_TOGGLE, "附件电源(开)"},
    {TESLA_CMD_OTA_CANCEL, "取消软件更新"},
    {TESLA_CMD_PING, "车机探活"},
    {TESLA_CMD_ERASE_USER_DATA, "清除访客数据(危险)"},
};

const tesla_command_info_t *tesla_command_table(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_command_table) / sizeof(s_command_table[0]));
    }
    return s_command_table;
}

/** 取一份状态快照，避免读结构体时被刷新任务改到一半 */
static void state_snapshot(tesla_vehicle_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(out, &g_vehicle_state, sizeof(*out));
        xSemaphoreGive(g_mutex);
    }
}

/** 发一条 VCSEC 未签名命令（车锁/闭件/唤醒，车机休眠也收得到） */
static esp_err_t vcsec_closure(tesla_closure_target_t target, tesla_closure_move_t move)
{
    uint8_t payload[64];
    uint32_t payload_len = tesla_build_closure_target(payload, sizeof(payload), target, move);
    if (payload_len == 0) {
        return ESP_FAIL;
    }
    return vcsec_send(payload, payload_len);
}

/**
 * @brief 取反类命令依赖哪一类状态
 *
 * 进了车钥匙页，主循环会暂停状态轮询（避免与按键命令抢同一条 BLE 链路），
 * 于是这里拿到的快照可能还是切页前、甚至几分钟前的旧值。直接按旧值取反，
 * 连点两下只会朝同一个方向发同一条指令——表现为
 * "空调按一下开了，再按一下还是开"、"车锁按下去没反应"。
 * 所以取反前必须先把依赖的那类状态读回来，读失败就退化成老行为并告警。
 */
static uint32_t toggle_dependent_bits(tesla_command_t cmd)
{
    switch (cmd) {
    case TESLA_CMD_CLIMATE_TOGGLE:
    case TESLA_CMD_TEMP_UP:
    case TESLA_CMD_TEMP_DOWN:
    case TESLA_CMD_SEAT_HEATER_TOGGLE:
    case TESLA_CMD_STEERING_HEAT_TOGGLE:
    case TESLA_CMD_BIOWEAPON_TOGGLE:
    case TESLA_CMD_COP_TOGGLE:
    case TESLA_CMD_PRECOND_MAX_TOGGLE:
        return TESLA_STATE_BIT_CLIMATE;
    case TESLA_CMD_CHARGE_TOGGLE:
    case TESLA_CMD_CHARGE_PORT_TOGGLE:
    case TESLA_CMD_CHARGE_AMPS_UP:
    case TESLA_CMD_CHARGE_AMPS_DOWN:
    case TESLA_CMD_CHARGE_LIMIT_UP:
    case TESLA_CMD_CHARGE_LIMIT_DOWN:
        return TESLA_STATE_BIT_CHARGE;
    case TESLA_CMD_SUNROOF_TOGGLE:
    case TESLA_CMD_SENTRY_TOGGLE:
    case TESLA_CMD_VALET_TOGGLE:
        return TESLA_STATE_BIT_CLOSURES;
    default:
        return 0;
    }
}

// ============ 取反方向记忆 ============
// 读一次状态并不总能成功（车机休眠、钥匙页暂停轮询、链路正忙），这时只剩旧快照；
// 而旧快照一旦恒为某个值，每次都朝同一方向发同一条指令，就成了"只能开不能关"。
// 更隐蔽的一种：车执行"开空调"要一两秒，这期间读回来还是"关"，
// 连点两下就发了两次"开"。所以这里记住每个取反命令上次下发的目标：
// 状态不可信（读失败，或刚发完还没到生效时间）时按记忆翻转，保证按两下必定一开一关。
// 状态确实新鲜时才以状态为准，并回写记忆。

// 用到 toggle_target 的命令已有 17 个（VCSEC 3 + 车机 13 + 座椅加热占 1 个槽），
// 槽位一旦占满，toggle_mem_find 返回 NULL，toggle_target 在"状态不可信"时会
// 永远返回 1 —— 车窗通风这类无状态反馈的命令就会一直发"通风"，第二次关不掉。
#define TOGGLE_MEM_SLOTS   24
#define TOGGLE_SETTLE_MS   3000    // 下发后车真正生效所需的大致时间

typedef struct {
    tesla_command_t cmd;      // TESLA_CMD_NONE 表示空槽
    int32_t         target;   // 上次下发的目标值（-1 表示还没发过）
    TickType_t      tick;     // 上次下发时刻
} toggle_mem_t;

static toggle_mem_t s_toggle_mem[TOGGLE_MEM_SLOTS];

static toggle_mem_t *toggle_mem_find(tesla_command_t cmd)
{
    toggle_mem_t *empty = NULL;
    for (int i = 0; i < TOGGLE_MEM_SLOTS; i++) {
        if (s_toggle_mem[i].cmd == cmd) {
            return &s_toggle_mem[i];
        }
        if (!empty && s_toggle_mem[i].cmd == TESLA_CMD_NONE) {
            empty = &s_toggle_mem[i];
        }
    }
    return empty;   // 槽位满了就退化成"每次按状态取反"
}

/**
 * @brief 座椅加热档位的下一步（记忆上一次下发的目标档位）
 *
 * 状态不可用时唯一能判断"现在该发哪一档"的依据就是上次发的目标，
 * 所以这里既是读取也是写入——只查一次表，避免读一次写一次。
 */
static int32_t heater_next_level(void)
{
    toggle_mem_t *m = toggle_mem_find(TESLA_CMD_SEAT_HEATER_TOGGLE);
    if (!m) {
        return 3;
    }
    m->target = (m->target >= 3 && m->target < 5) ? m->target + 1 : 3;
    m->tick = xTaskGetTickCount();
    return m->target;
}

static void toggle_mem_set(tesla_command_t cmd, int32_t target)
{
    toggle_mem_t *m = toggle_mem_find(cmd);
    if (m) {
        m->cmd = cmd;
        m->target = target;
        m->tick = xTaskGetTickCount();
    }
}

/**
 * @brief 算出本次要下发的目标（1=开方向，0=关方向）
 * @param fresh  这次确实读回了新鲜状态
 * @param cur_on fresh 时的当前状态
 */
static int32_t toggle_target(tesla_command_t cmd, bool fresh, bool cur_on)
{
    toggle_mem_t *m = toggle_mem_find(cmd);

    // 状态新鲜、且距上次下发已经过了生效时间，才敢信状态；否则一律信记忆
    bool has_mem = (m != NULL && m->target >= 0);
    bool trust_state = fresh && (!has_mem ||
                                 (xTaskGetTickCount() - m->tick) >= pdMS_TO_TICKS(TOGGLE_SETTLE_MS));

    int32_t target;
    if (trust_state) {
        target = cur_on ? 0 : 1;
    } else if (has_mem) {
        target = m->target ? 0 : 1;
    } else {
        target = 1;   // 完全无从判断：先"开"
    }
    toggle_mem_set(cmd, target);
    return target;
}

esp_err_t tesla_ble_execute(tesla_command_t cmd)
{
    if (cmd == TESLA_CMD_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    // 取反前先把依赖的状态读回来；读没读成功要记下来，取反方向据此决定
    bool state_fresh = false;
    uint32_t bits = toggle_dependent_bits(cmd);
    if (bits) {
        esp_err_t rr = refresh_states_once(bits, NULL);
        state_fresh = (rr == ESP_OK);
        if (rr != ESP_OK) {
            ESP_LOGW(TAG, "取反前读状态失败(%s)，按上次下发方向取反", esp_err_to_name(rr));
        }
    }
    // 车锁/闭件取反依赖 VCSEC 车身状态（车机休眠时也能读），同样先更新一次
    bool body_valid = false;
    if (cmd == TESLA_CMD_LOCK_TOGGLE || cmd == TESLA_CMD_TRUNK_TOGGLE ||
        cmd == TESLA_CMD_FRUNK_TOGGLE) {
        // 先备好会话：get_body_state 遇到未建立的会话会直接失败返回
        if (vcsec_session()) {
            esp_err_t br = tesla_ble_get_body_state();
            if (br != ESP_OK) {
                ESP_LOGW(TAG, "取反前读车身状态失败(%s)", esp_err_to_name(br));
            }
            body_valid = (br == ESP_OK);
        }
    }

    tesla_vehicle_state_t st;
    state_snapshot(&st);

    // ---- VCSEC 域：不需要车机在线，休眠也能执行 ----
    switch (cmd) {
    case TESLA_CMD_LOCK_TOGGLE: {
        bool locked = body_valid
                          ? (st.body.lock_state == TESLA_LOCK_LOCKED ||
                             st.body.lock_state == TESLA_LOCK_INTERNAL_LOCKED)
                          : st.closures.locked;
        // 目标 1=上锁方向、0=解锁方向；body_valid 为真才敢信状态
        return toggle_target(cmd, body_valid, locked) ? tesla_ble_lock()
                                                      : tesla_ble_unlock();
    }
    case TESLA_CMD_TRUNK_TOGGLE: {
        bool open = body_valid
                        ? (st.body.closures.rear_trunk != TESLA_CLOSURE_CLOSED)
                        : st.closures.trunk_rear_open;
        return vcsec_closure(TESLA_CLOSURE_TARGET_TRUNK,
                             toggle_target(cmd, body_valid, open)
                                 ? TESLA_CLOSURE_MOVE_OPEN
                                 : TESLA_CLOSURE_MOVE_CLOSE);
    }
    case TESLA_CMD_FRUNK_TOGGLE: {
        bool open = body_valid
                        ? (st.body.closures.front_trunk != TESLA_CLOSURE_CLOSED)
                        : st.closures.trunk_front_open;
        return vcsec_closure(TESLA_CLOSURE_TARGET_FRUNK,
                             toggle_target(cmd, body_valid, open)
                                 ? TESLA_CLOSURE_MOVE_OPEN
                                 : TESLA_CLOSURE_MOVE_CLOSE);
    }
    case TESLA_CMD_WAKE:
        return tesla_ble_wake_vehicle();
    default:
        break;
    }

    // ---- 车机域：按当前状态算出目标值，再发 CarServer Action ----
    int32_t param = 0;
    switch (cmd) {
    case TESLA_CMD_CLIMATE_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.climate.climate_on);
        break;
    case TESLA_CMD_TEMP_UP:
    case TESLA_CMD_TEMP_DOWN: {
        // 官方 SDK(vehicle/climate.go ChangeClimateTemp) 用 driver/passenger 的绝对
        // 设定温度，不用 delta_celsius；后者在部分车机固件上会把设定温度压到最低
        // （点一下"温度 +"反而变成 LO）。这里按当前设定值 ±0.5℃ 算绝对目标。
        float base = st.climate.driver_temp_setting;
        // 值不合理（0 / 未初始化 / 超范围）时用安全基准，绝不拿 0 当基准下发；
        // 刷新失败但旧快照仍合理时继续用旧值，别跳回 22℃ 又白降半度。
        if (base < 15.0f || base > 30.0f) {
            base = 22.0f;
        }
        float target = base + (cmd == TESLA_CMD_TEMP_UP ? 0.5f : -0.5f);
        if (target < 15.0f) {
            target = 15.0f;
        }
        if (target > 30.0f) {
            target = 30.0f;
        }
        param = (int32_t)(target * 10.0f + 0.5f);   // 0.1℃ 整数传给 builder
        break;
    }
    case TESLA_CMD_PRECOND_MAX_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.climate.preconditioning);
        break;
    case TESLA_CMD_STEERING_HEAT_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.climate.steering_wheel_heater);
        break;
    case TESLA_CMD_BIOWEAPON_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.climate.bioweapon_mode);
        break;
    case TESLA_CMD_COP_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.climate.cabin_overheat == 1);
        break;
    case TESLA_CMD_SEAT_HEATER_TOGGLE: {
        // 状态 0..3 = 关/低/中/高；动作枚举 2=off 3=low 4=med 5=high（不是 0/1 取反，单独按档走）
        if (state_fresh) {
            int32_t level = st.climate.seat_heater_left;
            param = (level <= 0) ? 3 : (level == 1) ? 4 : (level == 2) ? 5 : 2;
        } else {
            param = heater_next_level();   // 没发过就先开低档
        }
        toggle_mem_set(cmd, param);
        break;
    }
    case TESLA_CMD_CHARGE_TOGGLE:
        param = toggle_target(cmd, state_fresh,
                              st.charge.charging_state == TESLA_CHARGING_CHARGING);
        break;
    case TESLA_CMD_CHARGE_PORT_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.charge.charge_port_open);
        break;
    case TESLA_CMD_CHARGE_LIMIT_UP:
        param = st.charge.charge_limit_soc + 5;
        break;
    case TESLA_CMD_CHARGE_LIMIT_DOWN:
        param = st.charge.charge_limit_soc - 5;
        break;
    case TESLA_CMD_CHARGE_AMPS_UP:
        param = st.charge.charge_current_request + 1;
        break;
    case TESLA_CMD_CHARGE_AMPS_DOWN:
        param = st.charge.charge_current_request - 1;
        break;
    case TESLA_CMD_SUNROOF_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.closures.sunroof_percent > 0);
        break;
    case TESLA_CMD_SENTRY_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.closures.sentry_state >= 3);
        break;
    case TESLA_CMD_VALET_TOGGLE:
        param = toggle_target(cmd, state_fresh, st.closures.valet_mode);
        break;
    case TESLA_CMD_WINDOWS_VENT:
        // 车机不给车窗通风状态，只能按上次下发方向交替：第一次通风、再点一次关闭。
        // 之前这里是固定的"通风"动作，连点两次也只是发了两次通风。
        param = toggle_target(cmd, false, false);
        break;
    case TESLA_CMD_LOW_POWER_TOGGLE:
    case TESLA_CMD_ACCESSORY_POWER_TOGGLE:
        // 车机不给这两项的状态字段，只能按上次下发方向交替
        param = toggle_target(cmd, false, false);
        break;
    default:
        param = 0;
        break;
    }

    if (cmd == TESLA_CMD_CHARGE_LIMIT_UP || cmd == TESLA_CMD_CHARGE_LIMIT_DOWN) {
        if (param < 50) {
            param = 50;
        }
        if (param > 100) {
            param = 100;
        }
    }
    if (cmd == TESLA_CMD_CHARGE_AMPS_UP || cmd == TESLA_CMD_CHARGE_AMPS_DOWN) {
        int32_t max_amps = st.charge.charge_current_request_max > 0
                               ? st.charge.charge_current_request_max
                               : 32;
        if (param < 5) {
            param = 5;
        }
        if (param > max_amps) {
            param = max_amps;
        }
    }

    uint8_t payload[128];
    uint32_t payload_len = tesla_build_carserver_action(payload, sizeof(payload), cmd, param);
    if (payload_len == 0) {
        ESP_LOGW(TAG, "命令 %d 不属于车机域或不支持", (int)cmd);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 控制动作的响应可能为空，也可能带 ActionStatus；拿到就丢，不阻塞主流程
    // 这里原本只有 256 字节，而解密上限是 512 —— 车机回一条大响应就会写穿栈帧，
    // 现在大小和解密上限一起由 TESLA_MAX_PLAINTEXT 定，且函数本身也会校验上限。
    uint8_t dec[TESLA_MAX_PLAINTEXT];
    uint32_t dec_len = 0;
    tesla_fault_t fault = TESLA_FAULT_NONE;
    esp_err_t ret = infotainment_transact(payload, payload_len, dec, sizeof(dec),
                                          &dec_len, TESLA_WAIT_CMD_MS, &fault);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL && fault != TESLA_FAULT_NONE) {
            ESP_LOGE(TAG, "命令 %d 被车机拒绝(fault=%d %s)",
                     (int)cmd, (int)fault, tesla_fault_str(fault));
            return ESP_FAIL;
        }
        // 有的动作车辆不回包，超时也算已下发
        ESP_LOGW(TAG, "命令 %d 未收到响应(%s)，按已下发处理", (int)cmd, esp_err_to_name(ret));
    }
    return ESP_OK;
}
