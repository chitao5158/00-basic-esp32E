/**
 * @file remote.c
 * @brief WiFi 连接 + HTTPS POST 推送土壤湿度到 afl.cn
 *
 * TLS 验证: 使用 ESP-IDF 内置 CA bundle (mbedtls/esp_crt_bundle)
 *   通过 menuconfig 启用 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
 *   默认 full 模式包含 Let's Encrypt 全部 CA, 验证 afl.cn 自动通过.
 *
 * ⚠️ 首次使用必须在下面 USER CONFIG 区域填入:
 *   1. WIFI_SSID / WIFI_PASS    你的 WiFi 名字 + 密码
 *   2. DEVICE_API_KEY           必须与 Supabase/后端 API_KEY 一致
 *
 * 设计:
 *   - WiFi 失败不阻塞本地自动浇水 (远程是辅助, 本地是核心)
 *   - HTTP 推送失败仅打 E 级别日志, 下次循环重试
 *   - 推送周期由 main.c 控制
 */

#include "remote.h"

#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "remote";

/* ============================================================
 *  USER CONFIG — 填你自己的值, 然后重新编译烧录
 * ============================================================ */
#define WIFI_SSID         "MERCURY_19F9"          /* ← 你的 WiFi 名字 */
#define WIFI_PASS         "abc@1234567"           /* ← 你的 WiFi 密码 */
#define DEVICE_API_KEY    "05579823c3b7a34d986e9ac93cde58999e05c5498b12576843a4a0db22b901c3"

/* WiFi 连接超时 (ms) */
#define WIFI_TIMEOUT_MS   30000

/* HTTP POST 超时 (ms) */
#define HTTP_TIMEOUT_MS   10000

/* ============================================================ */

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;
static volatile bool      s_time_synced = false;  /* SNTP 是否同步成功 */

/* forward decl: pump_event 队列初始化 (在本文件后半定义, remote_init 里用到) */
static esp_err_t pump_event_post_init(void);

/* SNTP 同步回调: 在 SNTP 任务里跑, 设标志让 main 知道可以打真实时间戳 */
static void sntp_sync_cb(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP 同步成功, epoch=%lld", (long long)((int64_t)tv->tv_sec));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断开, 自动重连...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi 已连接, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t remote_init(void)
{
    /* NVS (WiFi 驱动需要) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid,     WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "正在连接 WiFi: %s ...", WIFI_SSID);

    /* 阻塞等待连接 (最多 30s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi 连接超时 (%d ms), 远程推送将不可用, 本地浇水照常工作",
                 WIFI_TIMEOUT_MS);
    }

    /* 启动 SNTP 同步真实时间 (用于 pump_events 审计日志). 同步失败不阻塞. */
    if (s_time_synced == false) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("cn.pool.ntp.org");
        cfg.sync_cb = sntp_sync_cb;
        cfg.wait_for_sync = false;  /* 非阻塞: 等 app_main 后台等 */
        esp_netif_sntp_init(&cfg);
        ESP_LOGI(TAG, "SNTP 已启动, 等 cn.pool.ntp.org 同步...");
    }

    /* 启动后台 pump_event 发送队列 (避免 web_pump_on/off 被 HTTPS POST 阻塞 9s) */
    pump_event_post_init();

    return ESP_OK;
}

/* forward decl: 命令执行 / GET 拉命令 (定义在本文件后半段) */
static void cmd_execute_from_json(const char *p);
static void cmd_poll_and_execute(void);
static void config_poll_and_apply(void);

esp_err_t remote_post_reading(int adc, int pct, const char *pump_state, bool sensor_err)
{
    if (!remote_is_connected()) {
        ESP_LOGE(TAG, "推送失败: WiFi 未连接");
        return ESP_FAIL;
    }

    /* 组装 JSON body */
    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"key\":\"%s\",\"device_id\":\"%s\",\"adc\":%d,\"pct\":%d,"
        "\"pump\":\"%s\",\"sensor_err\":%s}",
        DEVICE_API_KEY, DEVICE_ID, adc, pct, pump_state,
        sensor_err ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "JSON 构造失败");
        return ESP_FAIL;
    }

    /* HTTPS + 内置 CA bundle 验证 (Let's Encrypt 自动信任) */
    esp_http_client_config_t config = {
        .url              = INGEST_URL,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = HTTP_TIMEOUT_MS,
        .transport_type   = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP POST 返回非 200: status=%d", status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "推送成功 (status=200)");

    /* 不再读 POST 响应 (ESP-IDF v6 bug, GET /api/cmd 才是主路径) */

    /* 主路径: GET /api/cmd 拉取挂起命令 + GET /api/config 拉取阈值/周期 */
    cmd_poll_and_execute();
    config_poll_and_apply();

    return ESP_OK;
}

/* ============================================================
 *  命令执行: 解析 JSON 中的 cmd 块 + 执行 + ACK
 *  复用于 piggyback (POST 响应) 和 GET /api/cmd 两条路径
 * ============================================================ */

static void remote_send_ack(int cmd_id)
{
    char ack_body[128];
    snprintf(ack_body, sizeof(ack_body),
        "{\"key\":\"%s\",\"id\":%d,\"status\":\"done\"}",
        DEVICE_API_KEY, cmd_id);

    esp_http_client_config_t ack_config = {
        .url              = "https://afl.cn/api/poll_ack",
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = 5000,
        .transport_type   = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t ack_client = esp_http_client_init(&ack_config);
    if (!ack_client) {
        ESP_LOGE(TAG, "ACK 客户端初始化失败");
        return;
    }
    esp_http_client_set_header(ack_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(ack_client, ack_body, strlen(ack_body));
    esp_err_t ack_err = esp_http_client_perform(ack_client);
    if (ack_err != ESP_OK) {
        ESP_LOGE(TAG, "ACK 失败: %s", esp_err_to_name(ack_err));
    } else {
        ESP_LOGI(TAG, "ACK 成功 (id=%d)", cmd_id);
    }
    esp_http_client_cleanup(ack_client);
}

/* 从 cmd 块起始位置解析 id/name/sec, 然后执行.
 * 接受两种 JSON 形态:
 *   - piggyback:   "cmd":{"id":1,"name":"WATER","payload":{"sec":5}}
 *   - GET /api/cmd: 同样的子结构
 * 这里传入的 p 指向 "cmd":{ 之后的位置或整个响应 body, 都行. */
static void cmd_execute_from_json(const char *p)
{
    if (!p) return;

    int cmd_id = 0;
    const char *id_p = strstr(p, "\"id\":");
    if (id_p) cmd_id = atoi(id_p + 5);

    char cmd_name[32] = {0};
    const char *name_p = strstr(p, "\"name\":\"");
    if (name_p) {
        name_p += 8;
        int i = 0;
        while (*name_p && *name_p != '"' && i < (int)sizeof(cmd_name) - 1) {
            cmd_name[i++] = *name_p++;
        }
    }

    int sec = 5;  /* 默认 5s, payload.sec 缺失时用此值 */
    const char *sec_p = strstr(p, "\"sec\":");
    if (sec_p) {
        int v = atoi(sec_p + 6);
        if (v > 0 && v <= 600) sec = v;  /* 限幅 1s..10min */
    }

    if (cmd_id <= 0 || cmd_name[0] == '\0') {
        return;  /* 不是命令结构, 忽略 */
    }

    ESP_LOGI(TAG, "执行命令: id=%d name=%s sec=%d", cmd_id, cmd_name, sec);

    if (strcmp(cmd_name, "WATER") == 0) {
        ESP_LOGI(TAG, "→ 启动水泵 %d 秒", sec);
        web_pump_on();
        vTaskDelay(pdMS_TO_TICKS((uint32_t)sec * 1000));
        web_pump_off();
        ESP_LOGI(TAG, "→ 水泵停止");
    } else if (strcmp(cmd_name, "STOP") == 0) {
        web_pump_off();
        ESP_LOGI(TAG, "→ 立即停泵");
    } else if (strcmp(cmd_name, "REBOOT") == 0) {
        ESP_LOGW(TAG, "→ 3 秒后重启 ESP32");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return;  /* 重启了不 ACK */
    } else {
        ESP_LOGW(TAG, "→ 未知命令: %s", cmd_name);
    }

    remote_send_ack(cmd_id);
}

/* GET /api/cmd?device_id=X&key=Y — 主动拉取挂起命令
 *
 * ESP-IDF v6 esp_http_client_perform() 在 mbedTLS HTTPS 上对响应 body
 * 读取有已知 bug (perform() 提前返回, body 留在 SSL 缓冲里).
 *
 * 绕过方案: 用 streaming 模式 — open() + fetch_headers() + 读循环,
 * 并加 Connection: close 让 server 关连接触发 mbedTLS flush.
 */
static void cmd_poll_and_execute(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s?device_id=%s&key=%s",
             CMD_POLL_URL, DEVICE_ID, DEVICE_API_KEY);

    esp_http_client_config_t config = {
        .url              = url,
        .method           = HTTP_METHOD_GET,
        .timeout_ms       = HTTP_TIMEOUT_MS,
        .transport_type   = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 4096,
        .buffer_size_tx   = 1024,
        .keep_alive_enable = false,  /* 强制 server 响应后关连接, 触发 mbedTLS flush */
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "GET /api/cmd 客户端初始化失败");
        return;
    }
    esp_http_client_set_header(client, "Connection", "close");

    /* streaming 模式: open() 替代 perform(), 拿到 SSL 连接后手动读响应 */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET /api/cmd open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    /* 等 server 响应头 */
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET /api/cmd 返回 status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    ESP_LOGI(TAG, "GET /api/cmd status=200 content_length=%d", content_length);

    /* 读响应 body — 流式循环直到拿够或超时 */
    char resp_buf[512] = {0};
    int total = 0;
    int target = (content_length > 0 && (size_t)content_length < sizeof(resp_buf))
                 ? content_length
                 : (int)sizeof(resp_buf) - 1;

    int retries = 30;  /* 30 × 50ms = 1.5s */
    while (total < target && retries-- > 0) {
        int read = esp_http_client_read_response(client, resp_buf + total, target - total);
        if (read > 0) {
            total += read;
        } else if (read == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            ESP_LOGE(TAG, "GET /api/cmd read 错误: %d", read);
            break;
        }
    }
    resp_buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGW(TAG, "GET /api/cmd 响应仍读不到 (streaming + close 也失败)");
        return;
    }
    ESP_LOGI(TAG, "GET /api/cmd 响应[%d] 字节: %s", total, resp_buf);

    /* {"ok":true,"cmd":{"id":1,"name":"WATER","payload":{"sec":5}}}
     * cmd: null 时表示没有挂起命令 */
    const char *cmd_null = strstr(resp_buf, "\"cmd\":null");
    if (cmd_null) {
        ESP_LOGD(TAG, "无挂起命令");
        return;
    }
    const char *cmd_p = strstr(resp_buf, "\"cmd\":{");
    if (!cmd_p) {
        return;
    }
    cmd_execute_from_json(cmd_p);
}

bool remote_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

/* ============================================================
 *  GET /api/config 拉取设备阈值 + 推送周期
 *  用 streaming + Connection: close (跟 cmd_poll_and_execute 同模式)
 * ============================================================ */
static void config_poll_and_apply(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s?device_id=%s&key=%s",
             CONFIG_URL, DEVICE_ID, DEVICE_API_KEY);

    esp_http_client_config_t config = {
        .url              = url,
        .method           = HTTP_METHOD_GET,
        .timeout_ms       = HTTP_TIMEOUT_MS,
        .transport_type   = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 4096,
        .buffer_size_tx   = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "GET /api/config 客户端初始化失败");
        return;
    }
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET /api/config open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET /api/config 返回 status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    char resp_buf[256] = {0};
    int total = 0;
    int target = (content_length > 0 && (size_t)content_length < sizeof(resp_buf))
                 ? content_length
                 : (int)sizeof(resp_buf) - 1;
    int retries = 30;
    while (total < target && retries-- > 0) {
        int read = esp_http_client_read_response(client, resp_buf + total, target - total);
        if (read > 0) total += read;
        else if (read == 0) vTaskDelay(pdMS_TO_TICKS(50));
        else break;
    }
    resp_buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGW(TAG, "GET /api/config 响应读不到, 沿用本地阈值");
        return;
    }

    /* 响应: {"ok":true,"config":{"on_pct":30,"off_pct":70,"push_period_ms":300000}} */
    int new_on = -1, new_off = -1;
    uint32_t new_period = 0;

    const char *p = strstr(resp_buf, "\"on_pct\":");
    if (p) new_on = atoi(p + 9);
    p = strstr(resp_buf, "\"off_pct\":");
    if (p) new_off = atoi(p + 10);
    p = strstr(resp_buf, "\"push_period_ms\":");
    if (p) new_period = (uint32_t)atoi(p + 17);

    /* 校验 (服务端已校过, 这里再防一次) */
    if (new_on < 0 || new_on > 99
        || new_off < 1 || new_off > 100
        || new_off - new_on < 5
        || new_period < 5000 || new_period > 3600000) {
        ESP_LOGW(TAG, "config 字段越界, 沿用本地: %s", resp_buf);
        return;
    }

    /* 检测到变更才打日志, 避免每次都刷屏 */
    if (new_on != g_pump_on_pct || new_off != g_pump_off_pct || new_period != g_push_period_ms) {
        ESP_LOGW(TAG, "config 更新: on<%d -> on<%d, off>=%d -> off>=%d, period %ums -> %ums",
                 g_pump_on_pct, new_on,
                 g_pump_off_pct, new_off,
                 (unsigned)g_push_period_ms, (unsigned)new_period);
        g_pump_on_pct    = new_on;
        g_pump_off_pct   = new_off;
        g_push_period_ms = new_period;
    }
}

/* ============================================================
 *  浇水审计: 启/停水泵时各打一次点
 *  POST /api/pump_event { event: 'start' | 'stop', start_ts_ms, ... }
 *
 *  ESP-IDF v6 esp_http_client_perform() 对 HTTPS 响应读取有已知 bug, 同步调用
 *  会阻塞 ~9s. 所以 pump_event 的发送必须走 FreeRTOS 队列 + 后台 task, 让
 *  web_pump_on/off 立即返回, 保证水泵运行时长等于命令值.
 * ============================================================ */

typedef struct {
    char url[128];
    char body[256];
} pump_event_msg_t;

static QueueHandle_t s_pump_event_queue = NULL;

/* 后台 task: 从队列取事件, 同步 POST (允许 5s timeout). */
static void pump_event_sender_task(void *arg)
{
    ESP_LOGI(TAG, "pump event sender task 启动");
    pump_event_msg_t msg;
    while (1) {
        if (xQueueReceive(s_pump_event_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!remote_is_connected()) {
            ESP_LOGW(TAG, "pump event 丢弃 (WiFi 未连): %.80s", msg.body);
            continue;
        }
        esp_http_client_config_t config = {
            .url              = msg.url,
            .method           = HTTP_METHOD_POST,
            .timeout_ms       = 5000,
            .transport_type   = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) continue;
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, msg.body, strlen(msg.body));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "POST pump_event 失败: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
}

/* 在 remote_init() 末尾调用一次, 启动队列 + task */
static esp_err_t pump_event_post_init(void)
{
    s_pump_event_queue = xQueueCreate(8, sizeof(pump_event_msg_t));
    if (!s_pump_event_queue) {
        ESP_LOGE(TAG, "pump event 队列创建失败");
        return ESP_FAIL;
    }
    xTaskCreate(pump_event_sender_task, "pump_evt", 4096, NULL, 3, NULL);
    return ESP_OK;
}

/* web_pump_on/off 调这个, 立刻返回不阻塞 */
static void enqueue_pump_event(const char *url, const char *body)
{
    if (!s_pump_event_queue) {
        ESP_LOGW(TAG, "pump event 队列未初始化, 丢弃");
        return;
    }
    pump_event_msg_t msg;
    strncpy(msg.url, url, sizeof(msg.url) - 1);
    msg.url[sizeof(msg.url) - 1] = '\0';
    strncpy(msg.body, body, sizeof(msg.body) - 1);
    msg.body[sizeof(msg.body) - 1] = '\0';
    if (xQueueSend(s_pump_event_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pump event 队列满, 丢弃: %.80s", msg.body);
    }
}

void remote_pump_event_start(const char *trigger, int start_pct, int64_t start_ts_ms)
{
    char body[192];
    snprintf(body, sizeof(body),
        "{\"key\":\"%s\",\"device_id\":\"%s\",\"event\":\"start\","
        "\"trigger\":\"%s\",\"start_pct\":%d,\"start_ts_ms\":%lld}",
        DEVICE_API_KEY, DEVICE_ID, trigger, start_pct, (long long)start_ts_ms);
    ESP_LOGI(TAG, "pump event: start trigger=%s pct=%d ts=%lld", trigger, start_pct, (long long)start_ts_ms);
    enqueue_pump_event(PUMP_EVENT_URL, body);
}

void remote_pump_event_stop(int end_pct, int64_t start_ts_ms, uint32_t duration_ms)
{
    char body[192];
    snprintf(body, sizeof(body),
        "{\"key\":\"%s\",\"device_id\":\"%s\",\"event\":\"stop\","
        "\"end_pct\":%d,\"start_ts_ms\":%lld,\"duration_ms\":%u}",
        DEVICE_API_KEY, DEVICE_ID, end_pct, (long long)start_ts_ms, (unsigned)duration_ms);
    ESP_LOGI(TAG, "pump event: stop pct=%d duration=%ums", end_pct, (unsigned)duration_ms);
    enqueue_pump_event(PUMP_EVENT_URL, body);
}

/* 当前 epoch 毫秒 — SNTP 同步前返回 0, 调用方应跳过审计日志或用 fallback */
int64_t now_epoch_ms(void)
{
    if (!s_time_synced) {
        return 0;
    }
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}