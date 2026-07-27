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

/* ==== API ==== */

/**
 * @brief 初始化 WiFi (STA 模式), 阻塞直到连接成功或超时 30s.
 *        WiFi 连接失败时不返回 ESP_FAIL, 而是返回 ESP_OK 但日志警告.
 *        这样本地浇水逻辑不依赖网络.
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