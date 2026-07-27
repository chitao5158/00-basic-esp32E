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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
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
    return ESP_OK;
}

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

    /* piggyback: 试读响应, 解析 piggyback cmd 字段.
     * 如果读不到 (esp_http_client 已知 bug), 推送本身已成功, 不报错. */
    char resp_buf[512] = {0};
    if (status == 200) {
        int len = esp_http_client_get_content_length(client);
        int target = (len > 0 && (size_t)len < sizeof(resp_buf)) ? len : (int)sizeof(resp_buf) - 1;
        /* 重试 + 短暂延迟, 等 mbedTLS 把 body 喂进 esp_http_client 内部 buffer */
        int total = 0;
        for (int retry = 0; retry < 20 && total < target; retry++) {
            int read = esp_http_client_read_response(client, resp_buf + total, target - total);
            if (read > 0) {
                total += read;
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        resp_buf[total] = '\0';
        if (total > 0) {
            ESP_LOGI(TAG, "推送成功 + 响应[%d] 字节", total);
        } else {
            ESP_LOGI(TAG, "推送成功 (响应未读到, ESP-IDF 已知 bug)");
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST 返回非 200: status=%d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    /* 解析响应里的 cmd 字段 (piggyback) */
    if (resp_buf[0] != '\0') {
        const char *cmd_p = strstr(resp_buf, "\"cmd\":{");
        if (cmd_p) {
            int cmd_id = 0;
            const char *id_p = strstr(cmd_p, "\"id\":");
            if (id_p) cmd_id = atoi(id_p + 5);

            char cmd_name[32] = {0};
            const char *name_p = strstr(cmd_p, "\"name\":\"");
            if (name_p) {
                name_p += 8;  /* skip "name":" */
                int i = 0;
                while (*name_p && *name_p != '"' && i < (int)sizeof(cmd_name) - 1) {
                    cmd_name[i++] = *name_p++;
                }
            }

            ESP_LOGI(TAG, "收到 piggyback 命令: id=%d name=%s", cmd_id, cmd_name);

            if (cmd_id > 0 && cmd_name[0] != '\0') {
                /* 执行命令 */
                if (strcmp(cmd_name, "WATER") == 0) {
                    int sec = 5;
                    ESP_LOGI(TAG, "→ 启动水泵 %d 秒", sec);
                    relay_write(true);
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)sec * 1000));
                    relay_write(false);
                    ESP_LOGI(TAG, "→ 水泵停止");
                } else if (strcmp(cmd_name, "STOP") == 0) {
                    relay_write(false);
                    ESP_LOGI(TAG, "→ 立即停泵");
                } else if (strcmp(cmd_name, "REBOOT") == 0) {
                    ESP_LOGW(TAG, "→ 3 秒后重启 ESP32");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                } else {
                    ESP_LOGW(TAG, "→ 未知命令: %s", cmd_name);
                }

                /* ACK 给后端: 把这条命令标 done */
                char ack_body[128];
                snprintf(ack_body, sizeof(ack_body),
                    "{\"key\":\"%s\",\"id\":%d,\"status\":\"done\"}",
                    DEVICE_API_KEY, cmd_id);
                /* 单独发一次 ack (失败也无所谓) */
                esp_http_client_config_t ack_config = {
                    .url = "https://afl.cn/api/poll_ack",
                    .method = HTTP_METHOD_POST,
                    .timeout_ms = 5000,
                    .transport_type = HTTP_TRANSPORT_OVER_SSL,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                };
                esp_http_client_handle_t ack_client = esp_http_client_init(&ack_config);
                if (ack_client) {
                    esp_http_client_set_header(ack_client, "Content-Type", "application/json");
                    esp_http_client_set_post_field(ack_client, ack_body, strlen(ack_body));
                    esp_err_t ack_err = esp_http_client_perform(ack_client);
                    if (ack_err != ESP_OK) {
                        ESP_LOGE(TAG, "ACK 失败: %s", esp_err_to_name(ack_err));
                    }
                    esp_http_client_cleanup(ack_client);
                }
            }
        }
    }

    return ESP_OK;
}

bool remote_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}