/**
 * @file wifi_prov.c
 * @brief WiFi 配置门户实现 — SoftAP + DNS 诱捕 + HTTP 配置页
 *
 * 关键实现细节 (踩过/绕过的坑, 改代码前先读):
 *
 * 1. 重定向必须是 303 + 【带 response body】。iOS 靠响应体内容判定
 *    "这是个 captive portal" 才弹窗; 空的 302/303 iOS 不认。
 *    (这是 ESP-IDF 官方 captive_portal example 注释里明说的)
 *
 * 2. httpd_query_key_value() 【不做 URL 解码】。WiFi 密码里的
 *    @ # & + % 空格 会原样带进去或被 & 截断 -> 必须自己 url_decode()。
 *
 * 3. AP 开着时不能扫描。SoftAP 和 STA 共用射频必须同信道, 扫描会跳信道
 *    把手机踢下线 -> 进门户【之前】扫一次并缓存, /scan 只读缓存。
 *
 * 4. 不能 httpd_resp_send() 完直接 esp_restart(), 响应还没冲出 TCP 栈,
 *    手机看到的是连接重置 -> 起一个延迟 task 再重启。
 */

#include "wifi_prov.h"
#include "remote.h"     /* relay_write(): 重启前要切断水泵 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "dns_server.h"

static const char *TAG = "wifi_prov";

/* ============================================================
 *  常量
 * ============================================================ */

#define SCAN_CACHE_MAX      20      /* 配置页最多列这么多个 WiFi */
#define SCAN_FETCH_MAX      40      /* 一次最多从驱动取多少条 (给 calloc 封顶) */
#define AP_MAX_CONNECTION   4       /* 同时允许几台手机连配网热点 */
#define AP_CHANNEL          1
#define REBOOT_DELAY_MS     1500    /* 保证成功页发出去了再重启 */
#define REBOOT_TASK_STACK   2048

#define HTTPD_MAX_SOCKETS   7       /* 另有 3 个 socket 由 httpd 内部保留 */
#define HTTPD_STACK_SIZE    8192
#define HTTPD_SOCK_TIMEOUT  5       /* 秒 */

#define FORM_BUF_MAX        512     /* POST /save 表单体上限 */
/* URL 编码后最坏是原长的 3 倍 ("%41"), 缓冲区要按 3x 留 */
#define ENC_SSID_MAX        (WIFI_PROV_SSID_MAX * 3 + 1)
#define ENC_PASS_MAX        (WIFI_PROV_PASS_MAX * 3 + 1)

/* 嵌入的配置页 (见 main/CMakeLists.txt 的 EMBED_TXTFILES) */
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[]   asm("_binary_portal_html_end");

/* 结果页。内容很短, 直接内联; 主配置页才值得单独放 portal.html */
static const char PAGE_SAVED[] =
    "<!DOCTYPE html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;background:#10241b;color:#e8f5ee;padding:24px'>"
    "<h2>✅ 保存成功</h2>"
    "<p>设备正在重启并连接新的 WiFi,约需 30 秒。</p>"
    "<p>此配网热点即将关闭,请把手机连回家里的 WiFi。</p>"
    "<p style='color:#7fae95;font-size:13px'>如果 1 分钟后设备又开出 "
    "PlantSetup 热点,说明密码不对,请重新配置。</p></body>";

static const char PAGE_SAVE_FAILED[] =
    "<!DOCTYPE html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;background:#10241b;color:#e8f5ee;padding:24px'>"
    "<h2>保存失败</h2><p>请检查 WiFi 名称和密码后重试。</p>"
    "<p><a style='color:#4ade80' href='/'>返回</a></p></body>";

/* ============================================================
 *  模块状态
 * ============================================================ */

typedef struct {
    char    ssid[WIFI_PROV_SSID_MAX + 1];
    int8_t  rssi;
    uint8_t auth;      /* wifi_auth_mode_t, 0 = OPEN */
} prov_ap_t;

static prov_ap_t          s_scan_cache[SCAN_CACHE_MAX];
static int                s_scan_count;
static char               s_ap_ssid[sizeof("PlantSetup-XXXX")];
static httpd_handle_t     s_httpd;
static dns_server_handle_t s_dns;
static bool               s_active;
static volatile bool      s_rebooting;   /* 防止重复提交起多个重启 task */

/* ============================================================
 *  NVS 凭据读写
 * ============================================================ */

esp_err_t wifi_prov_load_creds(char *ssid, size_t ssid_size,
                               char *pass, size_t pass_size)
{
    if (!ssid || !pass || ssid_size == 0 || pass_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;   /* namespace 还没建过 = 从没配过网 */
    }

    size_t len = ssid_size;
    err = nvs_get_str(nvs, WIFI_PROV_NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* 密码允许缺失 (开放网络), 缺了就当空串 */
    len = pass_size;
    if (nvs_get_str(nvs, WIFI_PROV_NVS_KEY_PASS, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, WIFI_PROV_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_PROV_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        /* 顺手清掉强制配网标志, 否则重启后又进门户 */
        nvs_set_u8(nvs, WIFI_PROV_NVS_KEY_FORCE, 0);
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入凭据失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool wifi_prov_force_ap_requested(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_PROV_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(nvs, WIFI_PROV_NVS_KEY_FORCE, &flag);
    nvs_close(nvs);

    return (err == ESP_OK) && (flag == 1);
}

void wifi_prov_clear_force_ap(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_PROV_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u8(nvs, WIFI_PROV_NVS_KEY_FORCE, 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ============================================================
 *  扫描缓存
 * ============================================================ */

/**
 * @brief 把一条扫描结果并入缓存, 同名 SSID 只保留第一条
 *
 * esp_wifi_scan_get_ap_records() 的文档明确保证结果【已按 RSSI 降序排列】,
 * 所以同名 SSID (中继/多 AP) 的第一条就是信号最强的那条, 直接跳过后面的即可,
 * 不需要再排序或比较 RSSI。
 */
static void cache_upsert(const wifi_ap_record_t *rec)
{
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_cache[i].ssid, (const char *)rec->ssid) == 0) {
            return;     /* 已有同名且信号更强的, 忽略 */
        }
    }

    if (s_scan_count >= SCAN_CACHE_MAX) {
        return;
    }

    prov_ap_t *slot = &s_scan_cache[s_scan_count++];
    snprintf(slot->ssid, sizeof(slot->ssid), "%s", (const char *)rec->ssid);
    slot->rssi = rec->rssi;
    slot->auth = rec->authmode;
}

void wifi_prov_cache_scan(void)
{
    s_scan_count = 0;

    ESP_LOGI(TAG, "扫描周边 WiFi (进 AP 模式前的唯一一次)...");

    /* 可能还有一次没收敛的连接尝试在跑, 会让 scan 返回 ESP_ERR_WIFI_STATE。
     * 先断干净 (已经断开时返回错误, 无所谓)。 */
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_scan_start(NULL, true);   /* 阻塞式扫描 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s (配置页仍可手动输入 SSID)", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        ESP_LOGW(TAG, "没扫到任何 WiFi");
        return;
    }

    /* 密集住宅区可能扫到几十个 AP。结果已按 RSSI 降序, 取前面这些就够填满缓存,
     * 顺便给 calloc 封个顶, 不让环境决定我们分配多大内存。 */
    const uint16_t want = (found > SCAN_FETCH_MAX) ? SCAN_FETCH_MAX : found;

    wifi_ap_record_t *records = calloc(want, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "扫描结果内存不足 (%u 条)", want);
        esp_wifi_clear_ap_list();
        return;
    }

    uint16_t got = want;
    if (esp_wifi_scan_get_ap_records(&got, records) == ESP_OK) {
        for (uint16_t i = 0; i < got; i++) {
            if (records[i].ssid[0] != '\0') {   /* 跳过隐藏 SSID */
                cache_upsert(&records[i]);
            }
        }
    } else {
        /* 取失败时内部列表可能没被释放, 手动清掉防止泄漏 */
        esp_wifi_clear_ap_list();
    }

    free(records);
    ESP_LOGI(TAG, "扫描完成: 共 %u 个, 取 %u 个 -> 缓存 %d 个", found, got, s_scan_count);
}

/* ============================================================
 *  表单解析 (URL decode)
 * ============================================================ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief application/x-www-form-urlencoded 解码
 *
 * httpd_query_key_value() 只负责按 & 和 = 切分, 不解码, 所以必须补这一步。
 * 处理 "%XX" 转义 和 '+' -> 空格。非法的 % 序列原样保留。
 *
 * @return 解码后长度; 输出总是以 '\0' 结尾
 */
static size_t url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t w = 0;

    for (size_t r = 0; src[r] != '\0' && w + 1 < dst_size; r++) {
        if (src[r] == '+') {
            dst[w++] = ' ';
            continue;
        }
        if (src[r] == '%') {
            int hi = hex_val(src[r + 1]);
            int lo = (hi >= 0) ? hex_val(src[r + 2]) : -1;
            if (lo >= 0) {
                dst[w++] = (char)((hi << 4) | lo);
                r += 2;
                continue;
            }
        }
        dst[w++] = src[r];
    }

    dst[w] = '\0';
    return w;
}

/** 取出一个表单字段并解码。字段缺失时输出空串并返回 ESP_OK */
static esp_err_t form_get(const char *body, const char *key,
                          char *out, size_t out_size,
                          char *scratch, size_t scratch_size)
{
    esp_err_t err = httpd_query_key_value(body, key, scratch, scratch_size);
    if (err == ESP_ERR_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;     /* 含 ESP_ERR_HTTPD_RESULT_TRUNC: 输入超长 */
    }

    url_decode(scratch, out, out_size);
    return ESP_OK;
}

/* ============================================================
 *  HTTP handlers
 * ============================================================ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t len = portal_html_end - portal_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* 配置页会随固件变, 别让手机缓存住旧版本 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_html_start, len);
}

/** JSON 字符串转义: SSID 里可能有引号/反斜杠/控制字符 */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t w = 0;

    for (size_t r = 0; src[r] != '\0'; r++) {
        unsigned char c = (unsigned char)src[r];
        char esc[7];
        const char *piece = esc;
        size_t need;

        if (c == '"' || c == '\\') {
            esc[0] = '\\'; esc[1] = (char)c; esc[2] = '\0';
            need = 2;
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            need = 6;
        } else {
            esc[0] = (char)c; esc[1] = '\0';
            need = 1;
        }

        if (w + need + 1 > dst_size) {
            break;
        }
        memcpy(dst + w, piece, need);
        w += need;
    }

    dst[w] = '\0';
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* 分块发送, 避免为整个 JSON 准备一个大缓冲区 */
    httpd_resp_sendstr_chunk(req, "[");

    for (int i = 0; i < s_scan_count; i++) {
        char escaped[WIFI_PROV_SSID_MAX * 6 + 1];
        char item[sizeof(escaped) + 64];

        json_escape(s_scan_cache[i].ssid, escaped, sizeof(escaped));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%u}",
                 (i == 0) ? "" : ",",
                 escaped, s_scan_cache[i].rssi, s_scan_cache[i].auth);

        httpd_resp_sendstr_chunk(req, item);
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);   /* 结束分块 */
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    /* 安全第一: 重启前先切断水泵。
     * 配网时可能正好在浇水; esp_restart() 期间 GPIO 回到复位态, 不能指望
     * 它一定保持 OFF (RELAY_ACTIVE_LOW, 复位瞬间电平不确定) —— 显式关掉。 */
    relay_write(false);

    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "配网完成, 重启中...");
    esp_restart();
}

static esp_err_t send_save_error(httpd_req_t *req, const char *msg)
{
    ESP_LOGW(TAG, "配网表单被拒: %s", msg);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, PAGE_SAVE_FAILED);
    return ESP_OK;
}

/** 读取完整 POST body 到 buf。成功返回 ESP_OK 并保证 '\0' 结尾 */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0 || req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }

    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[FORM_BUF_MAX];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return send_save_error(req, "读取表单失败或超长");
    }

    char scratch[ENC_PASS_MAX];
    char ssid[WIFI_PROV_SSID_MAX + 1];
    char pass[WIFI_PROV_PASS_MAX + 1];

    if (form_get(body, "ssid", ssid, sizeof(ssid), scratch, sizeof(scratch)) != ESP_OK) {
        return send_save_error(req, "ssid 字段无效");
    }
    if (form_get(body, "pass", pass, sizeof(pass), scratch, sizeof(scratch)) != ESP_OK) {
        return send_save_error(req, "pass 字段无效");
    }

    if (ssid[0] == '\0') {
        return send_save_error(req, "ssid 为空");
    }
    /* 长度已由缓冲区大小截断保证, 这里只做最终确认 */
    if (strlen(ssid) > WIFI_PROV_SSID_MAX || strlen(pass) > WIFI_PROV_PASS_MAX) {
        return send_save_error(req, "ssid/pass 超长");
    }

    if (save_creds(ssid, pass) != ESP_OK) {
        return send_save_error(req, "NVS 写入失败");
    }

    ESP_LOGI(TAG, "已保存新凭据, SSID=\"%s\" (密码 %d 字节)", ssid, (int)strlen(pass));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, PAGE_SAVED);

    /* 用户重复提交 / 浏览器重发 POST 时只起一个重启 task */
    if (!s_rebooting) {
        s_rebooting = true;
        xTaskCreate(reboot_task, "prov_reboot", REBOOT_TASK_STACK, NULL, 5, NULL);
    }
    return ESP_OK;
}

/**
 * @brief 所有未注册的 URL 都重定向到配置页 —— captive portal 的核心
 *
 * iOS(captive.apple.com/hotspot-detect.html)、Android(/generate_204)、
 * Windows(/ncsi.txt, /connecttest.txt) 的联网探测全走这里, 无需逐个特判。
 *
 * ⚠️ 必须带 response body: iOS 靠响应体内容判定这是个 captive portal,
 *    只回空重定向不会弹出配置页。
 */
static esp_err_t redirect_to_portal(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_PROV_AP_IP "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ============================================================
 *  启动
 * ============================================================ */

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = HTTPD_MAX_SOCKETS;
    cfg.lru_purge_enable  = true;      /* 手机爱留半开连接, 不开会耗尽 socket */
    cfg.stack_size        = HTTPD_STACK_SIZE;
    cfg.recv_wait_timeout = HTTPD_SOCK_TIMEOUT;
    cfg.send_wait_timeout = HTTPD_SOCK_TIMEOUT;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler,
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler,
    };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_to_portal);

    return ESP_OK;
}

static void build_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "PlantSetup-%02X%02X", mac[4], mac[5]);
}

static esp_err_t start_softap(void)
{
    build_ap_ssid();

    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", s_ap_ssid);
    ap_cfg.ap.ssid_len       = strlen(s_ap_ssid);
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONNECTION;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password),
             "%s", WIFI_PROV_AP_PASS);

    /* APSTA 而非纯 AP: 保留 STA 接口, 否则 esp_wifi_scan_* 不可用。
     * STA 侧的自动重连已被 remote.c 停掉, 不会跟 AP 抢信道。 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 配置失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_prov_start_portal(void)
{
    if (s_active) {
        return ESP_OK;
    }

    /* 一进门户就清标志: 保证任何情况下断电重启都不会永久卡在 AP 模式 */
    wifi_prov_clear_force_ap();

    esp_err_t err = start_softap();
    if (err != ESP_OK) {
        return err;
    }

    /* 重定向风暴会刷屏, 压掉 httpd 的 info 级日志 */
    esp_log_level_set("httpd_uri",  ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    err = start_httpd();
    if (err != ESP_OK) {
        return err;
    }

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (!s_dns) {
        /* 不致命: 手动访问 192.168.4.1 仍然可用, 只是不会自动弹窗 */
        ESP_LOGW(TAG, "DNS 服务器启动失败, 需手动访问 http://" WIFI_PROV_AP_IP);
    }

    s_active = true;

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " 配网模式已启动");
    ESP_LOGW(TAG, "   热点: %s", s_ap_ssid);
    ESP_LOGW(TAG, "   密码: %s", WIFI_PROV_AP_PASS);
    ESP_LOGW(TAG, "   地址: http://%s", WIFI_PROV_AP_IP);
    ESP_LOGW(TAG, " 本地自动浇水继续运行, 不受影响");
    ESP_LOGW(TAG, "========================================");

    return ESP_OK;
}

bool wifi_prov_is_active(void)
{
    return s_active;
}

const char *wifi_prov_ap_ssid(void)
{
    return s_active ? s_ap_ssid : NULL;
}
