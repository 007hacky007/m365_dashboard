#include "ssd1306.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "SSD1306";

static esp_err_t write_cmd(i2c_port_t port, uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd};
    return i2c_master_write_to_device(port, SSD1306_I2C_ADDR, data, sizeof(data), 100 / portTICK_PERIOD_MS);
}

static esp_err_t write_data(i2c_port_t port, const uint8_t *bytes, size_t len) {
    uint8_t buf[1 + 16];
    while (len) {
        size_t chunk = len > 16 ? 16 : len;
        buf[0] = 0x40; // data prefix
        memcpy(&buf[1], bytes, chunk);
        esp_err_t err = i2c_master_write_to_device(port, SSD1306_I2C_ADDR, buf, chunk + 1, 100 / portTICK_PERIOD_MS);
        if (err != ESP_OK) return err;
        bytes += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

esp_err_t ssd1306_init(i2c_port_t port, int reset_gpio) {
    // Minimal init (no external reset pin handling yet)
    const uint8_t init_seq[] = {
        0xAE,       // display off
        0x20, 0x00, // horizontal addressing
        0xB0,       // page 0
        0xC8,       // COM scan dec
        0x00,       // low col = 0
        0x10,       // high col = 0
        0x40,       // start line = 0
        0x81, 0x7F, // contrast
        0xA1,       // segment remap
        0xA6,       // normal display
        0xA8, 0x3F, // multiplex
        0xA4,       // display RAM
        0xD3, 0x00, // display offset
        0xD5, 0x80, // clock divide
        0xD9, 0xF1, // pre-charge
        0xDA, 0x12, // COM pins
        0xDB, 0x40, // VCOM detect
        0x8D, 0x14, // charge pump
        0xAF        // display on
    };
    for (size_t i = 0; i < sizeof(init_seq); ++i) {
        esp_err_t err = write_cmd(port, init_seq[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init cmd %u failed %s", (unsigned)i, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

void ssd1306_clear(i2c_port_t port) {
    for (uint8_t page = 0; page < 8; ++page) {
        write_cmd(port, 0xB0 | page);
        write_cmd(port, 0x00);
        write_cmd(port, 0x10);
        uint8_t zeros[128] = {0};
        write_data(port, zeros, sizeof(zeros));
    }
}

// Basic 8x16 font (digits + letters subset). For brevity we define only needed chars 'H','E','L','O',' '.
static const uint8_t font8x16[][16] = {
    // ' ' (space)
    {0},
    // 'E'
    {0x00,0x00,0xFE,0x92,0x92,0x92,0x82,0x00, 0x00,0x00,0xFF,0x89,0x89,0x89,0x81,0x00},
    // 'H'
    {0x00,0x00,0xFE,0x10,0x10,0x10,0xFE,0x00, 0x00,0x00,0xFF,0x08,0x08,0x08,0xFF,0x00},
    // 'L'
    {0x00,0x00,0xFE,0x02,0x02,0x02,0x02,0x00, 0x00,0x00,0xFF,0x01,0x01,0x01,0x01,0x00},
    // 'O'
    {0x00,0x00,0x7C,0x82,0x82,0x82,0x7C,0x00, 0x00,0x00,0x3E,0x41,0x41,0x41,0x3E,0x00},
};

static const char font_map[] = " EHL0"; // note: using '0' to show 'O'

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
        write_cmd(port, 0xB0 | page);
        write_cmd(port, 0x00 | (col & 0x0F));
        write_cmd(port, 0x10 | (col >> 4));
        write_data(port, g, 8);
        write_cmd(port, 0xB0 | (page+1));
        write_cmd(port, 0x00 | (col & 0x0F));
        write_cmd(port, 0x10 | (col >> 4));
        write_data(port, g+8, 8);
        col += 8;
    }
}
