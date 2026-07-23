/**
 * @file ssd1306.c
 * @brief SSD1306 128x32 单色 OLED 驱动实现 (I2C, ESP-IDF 6.x 新 i2c_master API)
 */

#include "ssd1306.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "font6x8.h"

/* ------------------------------------------------------------------ */
/* 内部常量                                                            */
/* ------------------------------------------------------------------ */
#define SSD1306_CTRL_CMD    0x00   /* 控制字: 后续字节为命令流 */
#define SSD1306_CTRL_DATA   0x40   /* 控制字: 后续字节为数据流 */
#define SSD1306_COL_OFFSET  0      /* SSD1306 128 宽无列偏移 (SH1106 才需 +2) */
#define SSD1306_I2C_TIMEOUT_MS 100
#define SSD1306_FB_SIZE     (SSD1306_WIDTH * SSD1306_PAGES) /* 512 字节 */
#define SSD1306_PROBE_TIMEOUT_MS 20

static const char *TAG = "ssd1306";

/* 帧缓冲: 每字节代表某列 8 个垂直像素 (bit0 顶). 布局 [page][col]. */
static uint8_t s_fb[SSD1306_FB_SIZE];

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* ------------------------------------------------------------------ */
/* I2C 传输助手                                                        */
/* ------------------------------------------------------------------ */

/** 发送一串命令: {0x00, cmd0, cmd1, ...} */
static esp_err_t write_cmds(const uint8_t *cmds, size_t len)
{
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = SSD1306_CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(s_dev, buf, len + 1, SSD1306_I2C_TIMEOUT_MS);
}

/** 发送单个命令 */
static esp_err_t write_cmd(uint8_t cmd)
{
    return write_cmds(&cmd, 1);
}

/** 发送一行数据 (最多 128 字节): {0x40, d0, d1, ...} */
static esp_err_t write_data(const uint8_t *data, size_t len)
{
    uint8_t buf[1 + SSD1306_WIDTH];
    if (len > SSD1306_WIDTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = SSD1306_CTRL_DATA;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, SSD1306_I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static void i2c_bus_scan(void)
{
    ESP_LOGI(TAG, "I2C 总线扫描 (SCL=%d SDA=%d)...", SSD1306_PIN_SCL, SSD1306_PIN_SDA);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, SSD1306_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  检测到设备: 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  未检测到任何 I2C 设备, 请检查接线/上拉/供电");
    }
}

static esp_err_t ssd1306_send_init_seq(void)
{
    /* SSD1306 128x32 标准初始化序列 (含电荷泵使能, page 寻址模式) */
    static const uint8_t init_cmds[] = {
        0xAE,        /* display off                    */
        0xD5, 0x80,  /* clock divide / osc freq        */
        0xA8, 0x1F,  /* multiplex ratio = 31 (32 行)   */
        0xD3, 0x00,  /* display offset = 0             */
        0x40,        /* start line = 0                 */
        0x8D, 0x14,  /* charge pump 使能 (点亮关键!)   */
        0x20, 0x02,  /* memory addressing = page 模式  */
        0xA1,        /* segment remap (左右)           */
        0xC8,        /* COM scan 反向 (上下)           */
        0xDA, 0x02,  /* COM pins config (128x32 用 02) */
        0x81, 0x8F,  /* contrast                       */
        0xD9, 0xF1,  /* pre-charge period              */
        0xDB, 0x40,  /* VCOMH deselect level           */
        0xA4,        /* 恢复显示 RAM 内容              */
        0xA6,        /* 正常显示 (非反色)              */
        0xAF,        /* display on                     */
    };
    return write_cmds(init_cmds, sizeof(init_cmds));
}

esp_err_t ssd1306_init(void)
{
    esp_err_t err;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = SSD1306_I2C_PORT,
        .sda_io_num = SSD1306_PIN_SDA,
        .scl_io_num = SSD1306_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* 模块通常自带上拉 */
    };
    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2C 总线失败: %s", esp_err_to_name(err));
        return err;
    }

    i2c_bus_scan();

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SSD1306_I2C_ADDR,
        .scl_speed_hz = SSD1306_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 I2C 设备(0x%02X)失败: %s", SSD1306_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    /* 上电稳定 */
    vTaskDelay(pdMS_TO_TICKS(20));

    err = ssd1306_send_init_seq();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写初始化序列失败: %s (地址是否为 0x%02X?)",
                 esp_err_to_name(err), SSD1306_I2C_ADDR);
        return err;
    }

    s_ready = true;
    ssd1306_clear();
    err = ssd1306_refresh();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "初始化完成 (%dx%d @0x%02X)", SSD1306_WIDTH, SSD1306_HEIGHT, SSD1306_I2C_ADDR);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 帧缓冲绘制                                                          */
/* ------------------------------------------------------------------ */

void ssd1306_clear(void)
{
    memset(s_fb, 0x00, sizeof(s_fb));
}

void ssd1306_fill(bool on)
{
    memset(s_fb, on ? 0xFF : 0x00, sizeof(s_fb));
}

void ssd1306_draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    const int page = y / 8;
    const uint8_t mask = (uint8_t)(1u << (y % 8));
    uint8_t *cell = &s_fb[page * SSD1306_WIDTH + x];
    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

void ssd1306_draw_string(int x, int y, const char *text)
{
    if (text == NULL) {
        return;
    }
    int cursor_x = x;
    for (const char *p = text; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < FONT6X8_FIRST_CHAR || ch > FONT6X8_LAST_CHAR) {
            ch = ' ';
        }
        const uint8_t *glyph = font6x8[ch - FONT6X8_FIRST_CHAR];
        for (int col = 0; col < FONT6X8_WIDTH; col++) {
            const uint8_t bits = glyph[col];
            for (int row = 0; row < FONT6X8_HEIGHT; row++) {
                if (bits & (1u << row)) {
                    ssd1306_draw_pixel(cursor_x + col, y + row, true);
                }
            }
        }
        cursor_x += FONT6X8_WIDTH;
        if (cursor_x >= SSD1306_WIDTH) {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 刷新与显示控制                                                      */
/* ------------------------------------------------------------------ */

esp_err_t ssd1306_refresh(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int page = 0; page < SSD1306_PAGES; page++) {
        const uint8_t col = SSD1306_COL_OFFSET;
        const uint8_t addr_cmds[] = {
            (uint8_t)(0xB0 | page),                 /* 设置 page 地址        */
            (uint8_t)(0x00 | (col & 0x0F)),         /* 列地址低 4 位         */
            (uint8_t)(0x10 | ((col >> 4) & 0x0F)),  /* 列地址高 4 位         */
        };
        esp_err_t err = write_cmds(addr_cmds, sizeof(addr_cmds));
        if (err != ESP_OK) {
            return err;
        }
        err = write_data(&s_fb[page * SSD1306_WIDTH], SSD1306_WIDTH);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t ssd1306_set_contrast(uint8_t value)
{
    const uint8_t cmds[] = {0x81, value};
    return write_cmds(cmds, sizeof(cmds));
}

esp_err_t ssd1306_display_on(bool on)
{
    return write_cmd(on ? 0xAF : 0xAE);
}
