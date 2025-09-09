#include "ssd1306.h"
#include <string.h>
#include "esp_log.h"
#include "i2c_v2.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "SSD1306";

static i2c_master_dev_handle_t s_dev;
static esp_err_t write_cmd(uint8_t cmd) { uint8_t b[2]={0x00,cmd}; return i2c_master_transmit(s_dev,b,sizeof(b), 100/portTICK_PERIOD_MS); }
static esp_err_t write_data(const uint8_t *bytes, size_t len) {
    uint8_t buf[1+16];
    while(len){ size_t chunk=len>16?16:len; buf[0]=0x40; memcpy(&buf[1],bytes,chunk); ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev,buf,chunk+1, 100/portTICK_PERIOD_MS),TAG,"tx fail"); bytes+=chunk; len-=chunk; }
    return ESP_OK;
}

esp_err_t ssd1306_init(i2c_port_t port, int reset_gpio) {
    (void)reset_gpio; // TODO: handle reset pin if provided
    const uint8_t init_seq[] = {
        0xAE,0x20,0x00,0xB0,0xC8,0x00,0x10,0x40,0x81,0x7F,0xA1,0xA6,0xA8,0x3F,0xA4,0xD3,0x00,0xD5,0x80,0xD9,0xF1,0xDA,0x12,0xDB,0x40,0x8D,0x14,0xAF
    };
    if(!s_dev){ s_dev = i2c_v2_get_dev(SSD1306_I2C_ADDR); if(!s_dev) return ESP_FAIL; }
    for (size_t i = 0; i < sizeof(init_seq); ++i) {
        esp_err_t err = write_cmd(init_seq[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init cmd %u failed %s", (unsigned)i, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

void ssd1306_clear(i2c_port_t port) {
    for (uint8_t page = 0; page < 8; ++page) {
        write_cmd(0xB0 | page);
        write_cmd(0x00);
        write_cmd(0x10);
        uint8_t zeros[128] = {0};
        write_data(zeros, sizeof(zeros));
    }
}

static const uint8_t font8x16[][16] = {
    {0},
    {0x00,0x00,0xFE,0x92,0x92,0x92,0x82,0x00, 0x00,0x00,0xFF,0x89,0x89,0x89,0x81,0x00},
    {0x00,0x00,0xFE,0x10,0x10,0x10,0xFE,0x00, 0x00,0x00,0xFF,0x08,0x08,0x08,0xFF,0x00},
    {0x00,0x00,0xFE,0x02,0x02,0x02,0x02,0x00, 0x00,0x00,0xFF,0x01,0x01,0x01,0x01,0x00},
    {0x00,0x00,0x7C,0x82,0x82,0x82,0x7C,0x00, 0x00,0x00,0x3E,0x41,0x41,0x41,0x3E,0x00},
};

static const char font_map[] = " EHL0"; // '0' used to depict 'O'

static const uint8_t *glyph_for(char c) {
    if (c == ' ') return font8x16[0];
    for (size_t i=1;i<sizeof(font_map)-1;i++) {
        if (c == font_map[i]) return font8x16[i];
    }
    if (c == 'O') return font8x16[4];
    return font8x16[0];
}

void ssd1306_draw_text8x16(i2c_port_t port, uint8_t col, uint8_t page, const char *text) {
    while (*text) {
        const uint8_t *g = glyph_for(*text++);
        write_cmd(0xB0 | page);
        write_cmd(0x00 | (col & 0x0F));
        write_cmd(0x10 | (col >> 4));
        write_data(g, 8);
        write_cmd(0xB0 | (page+1));
        write_cmd(0x00 | (col & 0x0F));
        write_cmd(0x10 | (col >> 4));
        write_data(g+8, 8);
        col += 8;
    }
}
