/**
 * @file main.c
 * @brief ESP32 自动浇花系统: OLED 显示 + 土壤湿度 + 继电器控水泵
 *
 * 硬件接线 (绿深 ESP32-E 扩展板 + ESP32-WROOM-32E):
 *   OLED          扩展板丝印     ESP32
 *   ----          ---------     -----
 *   GND      <->  GND           —
 *   VDD      <->  3V3           —
 *   SCK      <->  P22           GPIO22 (I2C SCL)
 *   SDA      <->  P21           GPIO21 (I2C SDA)
 *
 *   土壤湿度模块    扩展板丝印     ESP32
 *   ---------      ---------     -----
 *   VCC      <->  3V3           —   ⚠️ 必须 3V3
 *   GND      <->  GND           —
 *   AO       <->  P33           GPIO33 (ADC1_CH5)
 *
 *   继电器模块      扩展板丝印     ESP32
 *   ---------      ---------     -----
 *   VCC      <->  5V            —   ⚠️ 必须 5V
 *   GND      <->  GND           —
 *   IN       <->  P5            GPIO5  (低电平触发)
 *   COM-NO   <->  水泵电源回路
 *
 * 控制逻辑 (hysteresis 防抖):
 *   湿度 < PUMP_ON_PCT (30%)  → 启泵
 *   湿度 ≥ PUMP_OFF_PCT (70%) → 关泵
 *   死区 30%~70% 内保持当前状态 (防抖)
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"

#include "ssd1306.h"

static const char *TAG = "app";

/* ==== 土壤湿度传感器 (P33/GPIO33/ADC1_CH5, VCC 用 GPIO18 控制) ==== */
#define SOIL_ADC_UNIT         ADC_UNIT_1
#define SOIL_ADC_CHANNEL      ADC_CHANNEL_5
#define SOIL_ADC_ATTEN        ADC_ATTEN_DB_12
#define SOIL_SAMPLES          64                  /* 平均采样次数 */
#define SOIL_POWER_GPIO       GPIO_NUM_18         /* 给传感器 VCC 供电控制 (防腐蚀) */
#define SOIL_POWER_STABLE_MS  10                  /* 上电后等待稳定时间 */

/* 标定值: 在「空气中」和「水中」实测后改写 */
/* 实测: 空气 adc=4095, 水中 adc=1612 */
#define SOIL_ADC_DRY        4095
#define SOIL_ADC_WET        1612

/* ==== 继电器 (P5/GPIO5, 低电平触发) ==== */
#define RELAY_GPIO          GPIO_NUM_5
#define RELAY_ACTIVE_LOW    1

/* ==== 自动浇花阈值 (百分比) ==== */
#define PUMP_ON_PCT         30      /* 低于此值启泵 */
#define PUMP_OFF_PCT        70      /* 高于此值关泵 */
#define SAMPLE_PERIOD_MS    2000    /* 采样周期 */

/* ==== 安全机制 (防止传感器故障 / 接线松脱导致水泵一直开) ==== */
#define PUMP_MAX_RUNTIME_MS 60000   /* 水泵最长连续运行 60s 后强制关闭 */
#define SENSOR_MIN_VALID    200     /* adc < 此值视为短路/接线错误 */
#define SENSOR_MAX_VALID    4095    /* adc > 此值视为断路 (上限 = 满量程) */

static adc_oneshot_unit_handle_t s_adc_handle;
static bool s_pump_on = false;          /* 当前水泵状态 */
static bool s_sensor_err = false;       /* 传感器故障状态 */
static uint32_t s_pump_on_since_ms = 0; /* 水泵启动时刻 (ms) */

/* ============================================================
 *  ADC
 * ============================================================ */

static esp_err_t soil_adc_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SOIL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = SOIL_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, SOIL_ADC_CHANNEL, &chan_cfg));

    /* 传感器 VCC 控制脚: 默认关, 仅在读数时打开 (防电化学腐蚀) */
    gpio_reset_pin(SOIL_POWER_GPIO);
    gpio_set_direction(SOIL_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SOIL_POWER_GPIO, 0);
    return ESP_OK;
}

static int soil_adc_read_avg(void)
{
    /* 上电 -> 等稳定 -> 连续采样 -> 断电 */
    gpio_set_level(SOIL_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(SOIL_POWER_STABLE_MS));

    int raw = 0;
    int sum = 0;
    int valid = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc_handle, SOIL_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            valid++;
        }
    }

    gpio_set_level(SOIL_POWER_GPIO, 0);  /* 立即断电, 防止长时间通电腐蚀探针 */
    return (valid > 0) ? (sum / valid) : 0;
}

static int soil_pct(int adc)
{
    int pct = (SOIL_ADC_DRY - adc) * 100 / (SOIL_ADC_DRY - SOIL_ADC_WET);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ============================================================
 *  继电器
 * ============================================================ */

static void relay_init(void)
{
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
#if RELAY_ACTIVE_LOW
    gpio_set_level(RELAY_GPIO, 1);  /* 初始 OFF = HIGH */
#else
    gpio_set_level(RELAY_GPIO, 0);  /* 初始 OFF = LOW */
#endif
}

static void relay_write(bool on)
{
#if RELAY_ACTIVE_LOW
    gpio_set_level(RELAY_GPIO, on ? 0 : 1);
#else
    gpio_set_level(RELAY_GPIO, on ? 1 : 0);
#endif
}

/* ============================================================
 *  控制逻辑: hysteresis
 * ============================================================ */

/* 仅在跨阈值时翻转状态, 30%~70% 死区内保持 */
/* 还含两层安全: 传感器合理性 + 水泵最长运行时间 */
static void control_pump(int adc, int pct)
{
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* 安全 1: 传感器合理性检查 */
    if (adc < SENSOR_MIN_VALID || adc > SENSOR_MAX_VALID) {
        if (!s_sensor_err) {
            ESP_LOGE(TAG, "传感器读数越界: adc=%d (允许范围 %d~%d), 判定为故障",
                     adc, SENSOR_MIN_VALID, SENSOR_MAX_VALID);
            ESP_LOGE(TAG, "可能原因: 探头拔出 / 接线松脱 / VCC 未接");
        }
        s_sensor_err = true;
        if (s_pump_on) {
            s_pump_on = false;
            relay_write(false);
            ESP_LOGE(TAG, "传感器故障 -> 水泵强制关闭");
        }
        return;
    }
    s_sensor_err = false;

    /* 计算期望状态 (hysteresis) */
    bool want_on;
    if (pct < PUMP_ON_PCT) {
        want_on = true;                /* 太干 -> 开泵 */
    } else if (pct >= PUMP_OFF_PCT) {
        want_on = false;               /* 够湿 -> 关泵 */
    } else {
        want_on = s_pump_on;           /* 死区 -> 保持现状 */
    }

    /* 安全 2: 水泵最长连续运行时间 */
    if (s_pump_on && want_on) {
        const uint32_t on_ms = now_ms - s_pump_on_since_ms;
        if (on_ms > PUMP_MAX_RUNTIME_MS) {
            ESP_LOGE(TAG, "水泵已连续运行 %lu ms (上限 %d ms), 强制关闭",
                     (unsigned long)on_ms, PUMP_MAX_RUNTIME_MS);
            ESP_LOGE(TAG, "可能原因: 传感器卡在干值 / 水泵故障 / 水源已空");
            want_on = false;
        }
    }

    /* 应用新状态 */
    if (want_on != s_pump_on) {
        s_pump_on = want_on;
        relay_write(s_pump_on);
        if (s_pump_on) {
            s_pump_on_since_ms = now_ms;
        }
        ESP_LOGW(TAG, "Pump %s (pct=%d%%, 阈值 on<%d  off>=%d)",
                 s_pump_on ? "ON" : "OFF", pct, PUMP_ON_PCT, PUMP_OFF_PCT);
    }
}

/* ============================================================
 *  显示
 * ============================================================ */

static void render(int adc, int pct)
{
    if (s_sensor_err) {
        /* 传感器故障界面 */
        ssd1306_clear();
        ssd1306_draw_string(0,  0, "!! SENSOR ERR !!");
        ssd1306_draw_string(0,  8, "Check probe");
        ssd1306_draw_string(0, 16, "Pump LOCKED OFF");
        char line_adc[24];
        snprintf(line_adc, sizeof(line_adc), "ADC: %4d", adc);
        ssd1306_draw_string(0, 24, line_adc);
        ssd1306_refresh();
        return;
    }

    char line_pct[24];
    char line_bar[24];
    char line_pump[24];
    char line_dbg[24];

    snprintf(line_pct,  sizeof(line_pct),  "Soil: %3d %%", pct);

    const int BAR_LEN = 20;
    int filled = pct * BAR_LEN / 100;
    for (int i = 0; i < filled; i++) line_bar[i] = '#';
    for (int i = filled; i < BAR_LEN; i++) line_bar[i] = '-';
    line_bar[BAR_LEN] = '\0';

    snprintf(line_pump, sizeof(line_pump), "Pump: %s", s_pump_on ? "ON " : "OFF");
    snprintf(line_dbg,  sizeof(line_dbg),  "ADC: %4d", adc);

    ssd1306_clear();
    ssd1306_draw_string(0,  0, line_pct);   /* 土壤湿度 %  */
    ssd1306_draw_string(0,  8, line_bar);   /* 进度条      */
    ssd1306_draw_string(0, 16, line_pump);  /* 水泵状态    */
    ssd1306_draw_string(0, 24, line_dbg);   /* 原始 ADC 值 */
    ssd1306_refresh();
}

/* ============================================================
 *  主循环
 * ============================================================ */

void app_main(void)
{
    ESP_ERROR_CHECK(soil_adc_init());
    relay_init();

    esp_err_t err = ssd1306_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    ssd1306_clear();
    ssd1306_draw_string(0,  0, "Auto-Watering");
    ssd1306_draw_string(0, 12, "starting...");
    ssd1306_refresh();

    ESP_LOGI(TAG, "自动浇花已启动: pct<%d -> 启泵, pct>=%d -> 关泵",
             PUMP_ON_PCT, PUMP_OFF_PCT);

    while (true) {
        int adc = soil_adc_read_avg();
        int pct = soil_pct(adc);
        control_pump(adc, pct);    /* hysteresis + 安全检查, 仅在跨阈值时翻转 */
        ESP_LOGI(TAG, "soil: adc=%4d  pct=%3d%%  pump=%s",
                 adc, pct, s_pump_on ? "ON " : "OFF");
        render(adc, pct);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}