#pragma once
#include <stdint.h>
#include "driver/i2c.h"

#define SSD1306_I2C_ADDR 0x3C

esp_err_t ssd1306_init(i2c_port_t port, int reset_gpio);
void ssd1306_clear(i2c_port_t port);
void ssd1306_draw_text8x16(i2c_port_t port, uint8_t col, uint8_t page, const char *text);
