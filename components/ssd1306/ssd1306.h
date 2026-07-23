#pragma once

/**
 * @file ssd1306.h
 * @brief SSD1306 128x32 单色 OLED 驱动 (I2C 接口)
 *
 * 硬件接线 (绿深 ESP32-E + ESP32-WROOM-32E):
 *   模块        扩展板丝印     ESP32
 *   ----        ---------     -----
 *   GND    <->  GND           —
 *   VDD    <->  3V3 (5V 也可) —
 *   SCK    <->  P22           GPIO22 (I2C SCL)
 *   SDA    <->  P21           GPIO21 (I2C SDA)
 *
 * 说明: 该模块仅引出 SCK/SDA 两根信号线, 为标准 I2C 接口。
 * 经串口 I2C 扫描确认地址为 0x3C (SSD1306 默认)。
 * 若换用地址跳线为 0x3D 的模块, 修改 SSD1306_I2C_ADDR 即可。
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 可配置参数 (按需修改)                                                */
/* ------------------------------------------------------------------ */
#define SSD1306_I2C_PORT     I2C_NUM_0   /* I2C 控制器编号            */
#define SSD1306_PIN_SCL      22          /* SCL -> GPIO22            */
#define SSD1306_PIN_SDA      21          /* SDA -> GPIO21            */
#define SSD1306_I2C_ADDR     0x3C        /* 7 位地址, 备选 0x3D       */
#define SSD1306_I2C_FREQ_HZ  400000      /* 400kHz fast mode         */

#define SSD1306_WIDTH        128         /* 水平像素                  */
#define SSD1306_HEIGHT       32          /* 垂直像素                  */
#define SSD1306_PAGES        (SSD1306_HEIGHT / 8) /* = 4 个 page      */

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化 OLED: 建立 I2C 总线/设备, 写入初始化序列(含电荷泵), 清屏刷新。
 *        初始化过程中会扫描 I2C 总线并打印检测到的地址, 便于排错。
 *        使用任何绘制函数前必须先调用一次。
 * @return ESP_OK 成功; 其它为 I2C/参数错误。
 */
esp_err_t ssd1306_init(void);

/** @brief 清空帧缓冲 (全灭)。需调用 ssd1306_refresh() 才会显示。 */
void ssd1306_clear(void);

/** @brief 用单一状态填充整屏帧缓冲。@param on true=点亮 false=熄灭 */
void ssd1306_fill(bool on);

/**
 * @brief 在帧缓冲中设置单个像素 (带边界检查)。
 * @param x  0..SSD1306_WIDTH-1
 * @param y  0..SSD1306_HEIGHT-1
 * @param on true=点亮 false=熄灭
 */
void ssd1306_draw_pixel(int x, int y, bool on);

/**
 * @brief 使用 6x8 字库在帧缓冲绘制 ASCII 字符串 (可打印字符 0x20..0x7E)。
 * @param x    左上角 X (像素)
 * @param y    左上角 Y (像素, 建议 8 的倍数)
 * @param text NUL 结尾字符串
 */
void ssd1306_draw_string(int x, int y, const char *text);

/** @brief 将帧缓冲刷新到屏幕。@return ESP_OK 成功。 */
esp_err_t ssd1306_refresh(void);

/** @brief 设置对比度, 范围 0x00..0xFF。 */
esp_err_t ssd1306_set_contrast(uint8_t value);

/** @brief 开/关显示。@param on true=开显示 */
esp_err_t ssd1306_display_on(bool on);

#ifdef __cplusplus
}
#endif
