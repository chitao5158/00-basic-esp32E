/**
 * @file wifi_prov.h
 * @brief WiFi 配置门户 (Captive Portal) — AP 模式 + DNS 诱捕 + HTTP 配置页
 *
 * 触发路径 (见 main.c):
 *   STA 连接失败 (或 NVS force_ap=1)  ->  wifi_prov_start_portal()
 *     -> 开 SoftAP "PlantSetup-XXXX" (密码 plant1234)
 *     -> DNS 把所有域名解析到 192.168.4.1, 手机自动弹出配置页
 *     -> 用户填 SSID/密码 -> 存 NVS -> 重启 -> 用新凭据连接
 *
 * 设计约束: 门户全程【不阻塞】本地自动浇水循环。
 *   httpd 和 dns_server 各自跑独立 task, app_main 照常 2s 一轮采样。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS 布局: namespace "wifi", keys "ssid" / "pass" / "force_ap" */
#define WIFI_PROV_NVS_NS        "wifi"
#define WIFI_PROV_NVS_KEY_SSID  "ssid"
#define WIFI_PROV_NVS_KEY_PASS  "pass"
#define WIFI_PROV_NVS_KEY_FORCE "force_ap"

/* 802.11 上限: SSID 32 字节, PSK 64 字节 (都不含结尾 '\0') */
#define WIFI_PROV_SSID_MAX      32
#define WIFI_PROV_PASS_MAX      64

/* 配网 AP 的固定密码。WPA2-PSK 要求 >= 8 字符 */
#define WIFI_PROV_AP_PASS       "plant1234"
#define WIFI_PROV_AP_IP         "192.168.4.1"

/**
 * @brief 从 NVS 读取已保存的 WiFi 凭据
 *
 * @param[out] ssid       输出缓冲区
 * @param      ssid_size  缓冲区大小 (建议 WIFI_PROV_SSID_MAX + 1)
 * @param[out] pass       输出缓冲区
 * @param      pass_size  缓冲区大小 (建议 WIFI_PROV_PASS_MAX + 1)
 *
 * @return ESP_OK 读到了; ESP_ERR_NVS_NOT_FOUND 没存过 (调用方应回退到硬编码默认值)
 */
esp_err_t wifi_prov_load_creds(char *ssid, size_t ssid_size,
                               char *pass, size_t pass_size);

/**
 * @brief 是否请求了强制进入配网模式 (NVS force_ap == 1)
 *
 * 这是一个逃生口: 用外部工具往 NVS 写 force_ap=1 就能让设备下次开机直接进门户,
 * 不用等 WiFi 超时。读到 1 之后应尽快调 wifi_prov_clear_force_ap()。
 */
bool wifi_prov_force_ap_requested(void);

/** @brief 清除 force_ap 标志 (进入门户时立刻调用, 防止永久卡在 AP 模式) */
void wifi_prov_clear_force_ap(void);

/**
 * @brief 扫描周边 WiFi 并缓存结果, 供配置页的 /scan 使用
 *
 * ⚠️ 必须在【切换到 AP 模式之前】调用 (此时还是 STA 模式)。
 *    ESP32 的 SoftAP 与 STA 共用射频、必须同信道; AP 起来之后再扫描会导致
 *    信道跳变, 把正连着配置页的手机踢下线。所以只扫这一次并缓存。
 *
 * 失败不致命 —— 页面仍可手动输入 SSID。
 */
void wifi_prov_cache_scan(void);

/**
 * @brief 启动配网门户: SoftAP + DNS 诱捕 + HTTP 服务器
 *
 * 非阻塞, 立即返回。调用方 (app_main) 应继续跑本地浇水循环。
 * 重复调用是安全的 (第二次直接返回 ESP_OK)。
 *
 * @return ESP_OK 门户已就绪
 */
esp_err_t wifi_prov_start_portal(void);

/** @brief 门户当前是否在运行 (main.c 用来切换 OLED 显示) */
bool wifi_prov_is_active(void);

/**
 * @brief 门户 AP 的 SSID, 形如 "PlantSetup-A1B2"
 * @return 门户未启动时返回 NULL
 */
const char *wifi_prov_ap_ssid(void);

#ifdef __cplusplus
}
#endif
