/**
 * @file remote.h
 * @brief WiFi 连接 + HTTPS POST 上报到 afl.cn/api/ingest
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* 共享给 main.c + remote.c 用的继电器接口 (main.c 定义实现) */
void relay_write(bool on);

/* ==== 用户配置 (按需修改, 烧一次固件即可生效) ==== */
/* 这些值在 remote.c 里定义, 修改后重新编译烧录 */
#define DEVICE_ID        "esp32_jh_01"                        /* 设备唯一 ID */
#define INGEST_URL       "https://afl.cn/api/ingest"          /* 后端 POST 地址 (HTTPS, 无 .php) */
#define CMD_POLL_URL     "https://afl.cn/api/cmd"             /* 后端 GET 地址: 拉取待执行命令 */
#define CONFIG_URL       "https://afl.cn/api/config"          /* 后端 GET 地址: 拉取阈值 + 推送周期 */
#define PUMP_EVENT_URL   "https://afl.cn/api/pump_event"      /* 后端 POST: 浇水启停审计 */
#define FW_VERSION_URL   "https://afl.cn/api/firmware/version"/* 后端 GET: 最新固件版本 */
#define FW_DOWNLOAD_URL  "https://afl.cn/api/firmware/latest" /* 后端 GET: 最新 .bin (OTA) */

/* ==== 设备阈值/周期 (云端可改, remote.c 拉到后写这里, main.c 读) ==== */
/* main.c 定义 volatile 全局, remote.c 引用更新 */
extern volatile int      g_pump_on_pct;
extern volatile int      g_pump_off_pct;
extern volatile uint32_t g_push_period_ms;

/* ==== 水泵 web 路径控制 (在 main.c 实现, 内部打 pump_events 点) ==== */
void web_pump_on(void);
void web_pump_off(void);

/* ==== API ==== */

/**
 * @brief 初始化网络栈: NVS + netif (STA & AP) + event loop + esp_wifi_init.
 *        只做一次初始化, 重复调用直接返回 ESP_OK.
 *        不发起连接 —— 连接是 remote_sta_connect() 的事.
 */
esp_err_t remote_netif_init(void);

/**
 * @brief 以 STA 模式连接 WiFi, 阻塞直到成功或超时 30s.
 *
 *        凭据优先级: NVS 里配网门户保存的 > remote.c 里硬编码的回退值.
 *        超时后会停止后台自动重连 (把射频让给配网 AP).
 *
 * @return ESP_OK 已连上; ESP_ERR_TIMEOUT 超时未连上
 */
esp_err_t remote_sta_connect(void);

/**
 * @brief 初始化 WiFi 并连接 (= remote_netif_init + remote_sta_connect + SNTP).
 *
 *        ⚠️ 返回值语义已变更: 现在会返回 ESP_ERR_TIMEOUT 表示没连上,
 *        调用方 (app_main) 据此决定是否启动配网门户.
 *        无论返回什么, 本地浇水逻辑都不受影响.
 */
esp_err_t remote_init(void);

/**
 * @brief 把当前读数推送到 INGEST_URL.
 *        推送失败时串口打 E 级别日志, 不影响本地控制.
 */
esp_err_t remote_post_reading(int adc, int pct, const char *pump_state, bool sensor_err);

/**
 * @brief 当前是否已连上 WiFi.
 */
bool remote_is_connected(void);

/**
 * @brief 上报水泵启动事件 (写入 pump_events.start_ts)
 * @param trigger  "auto" (hysteresis) / "web" (浏览器按钮) / "manual" (预留)
 * @param start_pct 启泵时的土壤湿度
 * @param start_ts_ms 本地 epoch 毫秒 (用作 stop 事件的关联 key)
 */
void remote_pump_event_start(const char *trigger, int start_pct, int64_t start_ts_ms);

/**
 * @brief 上报水泵停止事件 (UPDATE pump_events SET end_ts, duration_sec, end_pct)
 *        用 start_ts_ms 找到 start 行.
 */
void remote_pump_event_stop(int end_pct, int64_t start_ts_ms, uint32_t duration_ms);

/**
 * @brief 当前 epoch 毫秒. SNTP 同步前返回 0 (调用方应跳过审计日志).
 *        同步后由 esp_sntp 维护, gettimeofday 持续返回真实时间.
 */
int64_t now_epoch_ms(void);

/**
 * @brief 检查云端是否有新版固件, 有就下载刷写并重启.
 *        从 cmd_execute_from_json (OTA_UPDATE 命令) 或 app_main 启动时调用.
 *        无新版 / 已是最新 → 啥也不做, 不重启.
 */
void remote_ota_check_and_apply(void);