/**
 * @file wifi_portal.c
 * @brief VIN 配网门户实现
 *
 * 组成：
 *  - WiFi SoftAP（默认无密码，SSID = TeslaDash-XXXX）
 *  - HTTP 服务：配置页面 + /api/status + /api/vin
 *  - DNS 劫持：任意域名解析到 192.168.4.1，手机连上后一般会自动弹出配置页
 */

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "app_settings.h"
#include "tesla_ble.h"
#include "tesla_metrics.h"
#include "wifi_portal.h"

static const char *TAG = "PORTAL";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;
// 界面配置保存成功后回调业务层，让它立刻重新套用（不用等重启）
static void (*s_config_cb)(void) = NULL;
static TaskHandle_t s_dns_task = NULL;
static volatile bool s_running = false;
static volatile bool s_saved = false;
static char s_ap_ssid[33] = {0};

// ============ 页面与接口 ============

static esp_err_t send_page(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_page(req);
}

static esp_err_t captive_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_PORTAL_URL "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char vin[18] = {0};
    char json[192];

    if (app_settings_get_vin(vin) != ESP_OK) {
        vin[0] = '\0';
    }
    snprintf(json, sizeof(json),
             "{\"vin\":\"%s\",\"paired\":%s,\"ap_ssid\":\"%s\"}",
             vin, app_settings_is_paired() ? "true" : "false", s_ap_ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/** 从请求体中提取 17 位 VIN（兼容 JSON 与表单编码） */
static bool extract_vin(const char *body, char out[18])
{
    const char *p = body ? body : "";
    // 定位 "vin" 标识。条件里要检查三个字符都存在：body 以 'v'/'vi' 结尾时
    // p[1]/p[2] 会读到字符串结束符之后的位置
    while (*p) {
        if ((p[0] == 'v' || p[0] == 'V') && (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'n' || p[2] == 'N')) {
            p += 3;
            break;
        }
        p++;
    }
    // 跳过分隔符，读取连续的字母数字
    while (*p && !isalnum((unsigned char)*p)) {
        p++;
    }
    int n = 0;
    while (*p && isalnum((unsigned char)*p) && n < 17) {
        out[n++] = (char)toupper((unsigned char)*p);
        p++;
    }
    out[n] = '\0';
    return n == 17;
}

static void restart_task(void *arg)
{
    // 留出时间播放“设置成功，正在重启”的语音提示（约 2.7 秒）
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "配置已保存，重启设备");
    esp_restart();
}

// ============ 公共响应与请求体读取（后面的接口都从这里取用） ============

static esp_err_t json_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t json_err(httpd_req_t *req, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/** 读取请求体到缓冲，返回长度；失败返回 -1 */
static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len <= 0 || req->content_len >= (int)buf_len) {
        return -1;
    }
    int received = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            return -1;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    return received;
}

/** 延时重启，留出时间播报“设置成功，正在重启” */
static void schedule_restart(void)
{
    xTaskCreate(restart_task, "portal_restart", 2560, NULL, 3, NULL);
}

static esp_err_t vin_post(httpd_req_t *req)
{
    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_err(req, "请求内容为空");
    }
    char vin[18] = {0};
    if (!extract_vin(body, vin) || !app_settings_validate_vin(vin)) {
        ESP_LOGW(TAG, "收到非法 VIN: %s", body);
        return json_err(req, "VIN 必须为 17 位字母或数字");
    }

    // 换了车就该重新配对：旧密钥对新 VIN 没用，留着只会让配对一直失败
    char old_vin[18] = {0};
    bool had_vin = (app_settings_get_vin(old_vin) == ESP_OK);
    if (had_vin && strcmp(old_vin, vin) != 0) {
        ESP_LOGW(TAG, "VIN 变更 %s -> %s，删除旧密钥并重新配对", old_vin, vin);
        app_settings_clear_key();
        app_settings_set_paired(false);
    }

    esp_err_t err = app_settings_set_vin(vin);
    if (err != ESP_OK) {
        return json_err(req, "写入存储失败");
    }

    ESP_LOGI(TAG, "VIN 已设置: %s", vin);
    s_saved = true;

    esp_err_t send = json_ok(req);

    // 给网页留出显示“保存成功”的时间，然后重启进入正常模式
    schedule_restart();
    return send;
}

// ============ 界面配置接口（6 格 + 4 按钮 + 清除密钥/VIN） ============

/** 解析 "dash=1,2,3,8,9,19" 这种逗号分隔的 id 列表 */
static int parse_id_list(const char *body, const char *key, uint8_t *out, int max)
{
    if (!body || !key || !out) {
        return -1;
    }
    const char *p = strstr(body, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, '=');
    if (!p) {
        return -1;
    }
    p++;

    int n = 0;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!isdigit((unsigned char)*p)) {
            break;
        }
        int v = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++;
        }
        out[n++] = (uint8_t)(v > 255 ? 255 : v);
    }
    return n;
}

// ============ 选项分类 ============
//
// 指标/命令各有几十项，平铺在下拉框里很难找。这里按 tesla_metrics.h
// 与 tesla_ble.h 枚举里的分组给出分类名，配置页据此渲染 optgroup。

static const char *metric_category(tesla_metric_t id)
{
    if (id == TESLA_METRIC_NONE) {
        return "通用";
    }
    // 四个门追加在枚举末尾（为了不挪动老配置的 ID），不能靠区间判断，单独归类
    if (id == TESLA_METRIC_DOOR_FL || id == TESLA_METRIC_DOOR_FR ||
        id == TESLA_METRIC_DOOR_RL || id == TESLA_METRIC_DOOR_RR) {
        return "车身与安防";
    }
    if (id <= TESLA_METRIC_ROUTE_DIST) {
        return "行驶与导航";
    }
    if (id <= TESLA_METRIC_CHARGE_PORT) {
        return "电池与充电";
    }
    if (id <= TESLA_METRIC_DEFROST) {
        return "空调与座椅";
    }
    if (id <= TESLA_METRIC_TP_RR) {
        return "胎压";
    }
    if (id <= TESLA_METRIC_REMOTE_START) {
        return "车身与安防";
    }
    if (id <= TESLA_METRIC_VOLUME) {
        return "媒体";
    }
    return "软件与位置";
}

static const char *command_category(tesla_command_t cmd)
{
    if (cmd == TESLA_CMD_NONE) {
        return "通用";
    }
    if (cmd <= TESLA_CMD_WAKE) {
        return "常用（车机休眠可用）";
    }
    if (cmd <= TESLA_CMD_HONK_HORN) {
        return "灯光与喇叭";
    }
    if (cmd <= TESLA_CMD_PRECOND_MAX_TOGGLE) {
        return "空调与座椅";
    }
    if (cmd <= TESLA_CMD_CHARGE_LIMIT_DOWN) {
        return "充电";
    }
    if (cmd <= TESLA_CMD_SUNROOF_TOGGLE) {
        return "车窗与天窗";
    }
    if (cmd <= TESLA_CMD_VOLUME_DOWN) {
        return "媒体";
    }
    return "安全与车机";
}

static esp_err_t config_get(httpd_req_t *req)
{
    char vin[18] = {0};
    uint8_t dash[APP_DASH_SLOT_COUNT] = {0};
    uint8_t keys[APP_KEY_SLOT_COUNT] = {0};
    uint8_t key_buf[32] = {0};

    if (app_settings_get_vin(vin) != ESP_OK) {
        vin[0] = '\0';
    }
    app_settings_get_dash_slots(dash);
    app_settings_get_key_slots(keys);

    char head[320];
    int n = snprintf(head, sizeof(head),
                     "{\"vin\":\"%s\",\"paired\":%s,\"has_key\":%s,\"ap_ssid\":\"%s\","
                     "\"dash\":[%u,%u,%u,%u,%u,%u],\"keys\":[%u,%u,%u,%u],",
                     vin,
                     app_settings_is_paired() ? "true" : "false",
                     app_settings_load_key(key_buf) == ESP_OK ? "true" : "false",
                     s_ap_ssid,
                     dash[0], dash[1], dash[2], dash[3], dash[4], dash[5],
                     keys[0], keys[1], keys[2], keys[3]);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n > 0 && n < (int)sizeof(head)) {
        httpd_resp_send_chunk(req, head, n);
    }

    // 指标表：配置页的下拉选项（带分类名，前端按 optgroup 分组展示）
    char item[128];
    int count = 0;
    const tesla_metric_info_t *mt = tesla_metric_table(&count);
    httpd_resp_send_chunk(req, "\"metrics\":[", 11);
    for (int i = 0; i < count; i++) {
        n = snprintf(item, sizeof(item), "%s{\"id\":%u,\"name\":\"%s\",\"cat\":\"%s\"}",
                     i ? "," : "", (unsigned)mt[i].id, mt[i].name,
                     metric_category(mt[i].id));
        if (n > 0 && n < (int)sizeof(item)) {
            httpd_resp_send_chunk(req, item, n);
        }
    }
    httpd_resp_send_chunk(req, "],", 2);

    // 命令表：钥匙页 4 个按钮的选项
    count = 0;
    const tesla_command_info_t *ct = tesla_command_table(&count);
    httpd_resp_send_chunk(req, "\"commands\":[", 12);
    for (int i = 0; i < count; i++) {
        n = snprintf(item, sizeof(item), "%s{\"id\":%u,\"name\":\"%s\",\"cat\":\"%s\"}",
                     i ? "," : "", (unsigned)ct[i].cmd, ct[i].name,
                     command_category(ct[i].cmd));
        if (n > 0 && n < (int)sizeof(item)) {
            httpd_resp_send_chunk(req, item, n);
        }
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_err(req, "请求内容为空");
    }

    uint8_t dash[APP_DASH_SLOT_COUNT];
    uint8_t keys[APP_KEY_SLOT_COUNT];

    int nd = parse_id_list(body, "dash", dash, APP_DASH_SLOT_COUNT);
    int nk = parse_id_list(body, "keys", keys, APP_KEY_SLOT_COUNT);

    bool want_dash = (nd == APP_DASH_SLOT_COUNT);
    bool want_keys = (nk == APP_KEY_SLOT_COUNT);
    if (!want_dash && !want_keys) {
        return json_err(req, "没有可保存的内容");
    }

    // 先整体校验再统一落盘：原来的写法先写 dash、再校验 keys，
    // keys 校验失败时 dash 已经进 NVS 了，返回错误却实际应用了一半，
    // 而且不会触发配置回调，界面和 NVS 里的内容不一致。
    if (want_dash) {
        for (int i = 0; i < APP_DASH_SLOT_COUNT; i++) {
            if (dash[i] >= TESLA_METRIC_MAX) {
                return json_err(req, "仪表盘指标不合法");
            }
        }
    }
    if (want_keys) {
        for (int i = 0; i < APP_KEY_SLOT_COUNT; i++) {
            if (keys[i] == 0 || keys[i] >= TESLA_CMD_MAX) {
                return json_err(req, "按钮命令不合法");
            }
        }
    }

    if (want_dash) {
        app_settings_set_dash_slots(dash);
    }
    if (want_keys) {
        app_settings_set_key_slots(keys);
    }

    // 通知业务层立刻套用新配置：否则屏幕要等重启才变，
    // 用户会以为"保存没生效"。（NVS 里已经有了，重启照常恢复）
    if (s_config_cb) {
        s_config_cb();
    }
    return json_ok(req);
}

static esp_err_t clear_post(httpd_req_t *req)
{
    char body[128] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_err(req, "请求内容为空");
    }

    if (strstr(body, "target=key")) {
        app_settings_clear_key();
        ESP_LOGW(TAG, "已删除车机密钥（需重新配对）");
    } else if (strstr(body, "target=vin")) {
        // 换 VIN 等于换车：旧密钥对新车辆无效，必须一起删掉
        app_settings_clear_key();
        app_settings_clear_vin();
        ESP_LOGW(TAG, "已删除 VIN 与密钥");
    } else if (strstr(body, "target=all")) {
        app_settings_factory_reset();
        ESP_LOGW(TAG, "已恢复出厂（VIN / 密钥 / 配对状态）");
    } else {
        return json_err(req, "未知的清除目标");
    }
    return json_ok(req);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    esp_err_t send = json_ok(req);
    schedule_restart();
    return send;
}

// ============ DNS 劫持（强制门户） ============

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket 创建失败");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS 端口绑定失败");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[320];
    uint8_t resp[512];
    const uint8_t answer_ip[4] = {192, 168, 4, 1};

    ESP_LOGI(TAG, "DNS 劫持已启动");

    while (s_running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len < 0) {
            // 超时（EAGAIN）正常；其它错误说明 socket 坏了，继续空转会 100% 占住 CPU
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "DNS recvfrom 失败(errno=%d)，停止 DNS 劫持任务", errno);
                break;
            }
            continue;
        }
        if (len < 13) {
            continue;
        }
        // 仅处理标准查询（QR=0，QDCOUNT=1，OPCODE=0）
        if (buf[2] & 0x80) {
            continue;
        }
        uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qdcount != 1) {
            continue;
        }

        // 查找问题段结束位置
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            if ((buf[pos] & 0xC0) == 0xC0) {  // 压缩指针
                pos += 2;
                goto question_done;
            }
            pos += buf[pos] + 1;
        }
        if (pos >= len) {
            continue;
        }
        pos++;  // 跳过结尾 0x00
    question_done:
        pos += 4;  // QTYPE + QCLASS
        if (pos > len) {
            continue;
        }

        int qlen = pos;
        memcpy(resp, buf, qlen);
        resp[2] = 0x81;            // QR=1, OPCODE=0, AA=0, TC=0, RD=1
        resp[3] = 0x80;            // RA=1, RCODE=0
        resp[6] = 0x00;            // ANCOUNT = 1
        resp[7] = 0x01;
        resp[8] = 0x00;            // NSCOUNT = 0
        resp[9] = 0x00;
        resp[10] = 0x00;           // ARCOUNT = 0
        resp[11] = 0x00;
        if (qlen + 16 > (int)sizeof(resp)) {
            continue;
        }
        int o = qlen;
        resp[o++] = 0xC0;          // NAME 指向问题段
        resp[o++] = 0x0C;
        resp[o++] = 0x00;          // TYPE = A
        resp[o++] = 0x01;
        resp[o++] = 0x00;          // CLASS = IN
        resp[o++] = 0x01;
        resp[o++] = 0x00;          // TTL = 60
        resp[o++] = 0x00;
        resp[o++] = 0x00;
        resp[o++] = 0x3C;
        resp[o++] = 0x00;          // RDLENGTH = 4
        resp[o++] = 0x04;
        memcpy(&resp[o], answer_ip, 4);
        o += 4;

        sendto(sock, resp, o, 0, (struct sockaddr *)&from, fromlen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ============ 启动 / 停止 ============

static esp_err_t wifi_ap_start(const char *ssid)
{
    static bool netif_inited = false;
    static bool event_inited = false;
    static bool wifi_inited = false;

    if (!netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        netif_inited = true;
    }
    if (!event_inited) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        event_inited = true;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        return ESP_FAIL;
    }

    if (!wifi_inited) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        wifi_inited = true;
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    // 先停掉可能正在运行的 STA/旧 AP（忽略"未启动"的错误），再切到 AP 模式
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "热点已启动: %s (无密码)", ssid);
    return ESP_OK;
}

void wifi_portal_set_config_saved_cb(void (*cb)(void))
{
    s_config_cb = cb;
}

esp_err_t wifi_portal_start(const char *ap_ssid)
{
    if (s_running) {
        return ESP_OK;
    }

    if (ap_ssid) {
        strncpy(s_ap_ssid, ap_ssid, sizeof(s_ap_ssid) - 1);
    } else {
        app_settings_get_ap_name(s_ap_ssid, sizeof(s_ap_ssid));
    }

    esp_err_t err = wifi_ap_start(s_ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 6;
    // 默认是 8，而下面要注册 11 个 URI；设小了 httpd_register_uri_handler 会失败，
    // 结果是强制门户探测地址落到 404，手机就不会自动弹出配置页
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL},
        {.uri = "/index.html", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get, .user_ctx = NULL},
        {.uri = "/api/vin", .method = HTTP_POST, .handler = vin_post, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post, .user_ctx = NULL},
        {.uri = "/api/clear", .method = HTTP_POST, .handler = clear_post, .user_ctx = NULL},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post, .user_ctx = NULL},
        // 常见强制门户探测地址，统一跳转到配置页
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_get, .user_ctx = NULL},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get,
         .user_ctx = NULL},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_get,
         .user_ctx = NULL},
        // Android 旧版 / Windows / iOS 旧版还会探测这些地址
        {.uri = "/mobile/status.php", .method = HTTP_GET, .handler = captive_get,
         .user_ctx = NULL},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_get, .user_ctx = NULL},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_get,
         .user_ctx = NULL},
    };

    // 逐个注册并报错：注册失败时 httpd 不会有任何提示，只会表现为该地址 404，
    // 排查起来非常费劲（曾经就因为 max_uri_handlers 不够而全部丢失）
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "注册 URI %s 失败: %s", uris[i].uri, esp_err_to_name(rerr));
        }
    }

    s_running = true;
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, &s_dns_task);

    ESP_LOGI(TAG, "配网门户已启动: http://%s/", WIFI_PORTAL_URL);
    return ESP_OK;
}




bool wifi_portal_take_saved_flag(void)
{
    bool v = s_saved;
    s_saved = false;
    return v;
}
