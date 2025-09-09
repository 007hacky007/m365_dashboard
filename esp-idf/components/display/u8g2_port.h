#pragma once
#include "esp_err.h"

#ifndef U8G2P_I2C_PORT
#define U8G2P_I2C_PORT I2C_NUM_0
#endif
#ifndef U8G2P_I2C_ADDR
#define U8G2P_I2C_ADDR 0x3C
#endif

esp_err_t u8g2p_init(void);
void u8g2p_clear(void);
void u8g2p_commit(void);
void u8g2p_set_cursor(uint8_t col_chars, uint8_t row_chars); // character grid (8px units)
void u8g2p_set_scale(uint8_t s); // 1 or 2 (2 = 2x vertical+horizontal)
void u8g2p_print(const char *txt);
void u8g2p_print_char(char c);
uint8_t u8g2p_cursor_col_px(void);
uint8_t u8g2p_cursor_row_px(void);
void u8g2p_draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void u8g2p_draw_frame(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void u8g2p_set_draw_color(uint8_t c); // 0=clear,1=set
void u8g2p_commit(void);
// Draw monochrome bitmap (1bpp) packed MSB first per byte, row-major.
void u8g2p_draw_bitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *bits);
