#include "u8g2_port.h"
#include "i2c_v2.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

// Minimal subset of U8g2-like API we need to start porting display_fsm logic.
// We keep a 128x64 1bpp buffer (1024 bytes) and offer clear/draw text (8x8) and commit.

#define DISP_W 128
#define DISP_H 64
static uint8_t s_buf[DISP_W * DISP_H / 8];
static const char *TAG = "U8G2PORT";
static uint8_t s_cur_x_px = 0, s_cur_y_px = 0; // pixel cursor
static uint8_t s_scale = 1; // 1 or 2
static uint8_t s_draw_color = 1;

void u8g2p_clear(void) { memset(s_buf, 0, sizeof(s_buf)); }
void u8g2p_set_scale(uint8_t s) { s_scale = (s==2)?2:1; }
void u8g2p_set_draw_color(uint8_t c) { s_draw_color = c ? 1 : 0; }
void u8g2p_set_cursor(uint8_t col_chars, uint8_t row_chars) { s_cur_x_px = col_chars * 8; s_cur_y_px = row_chars * 8; }
uint8_t u8g2p_cursor_col_px(void){ return s_cur_x_px; }
uint8_t u8g2p_cursor_row_px(void){ return s_cur_y_px; }

// Compact 6x8 monospaced glyphs for required subset (space, digits, '.', '%', ':', 'A'-'Z', 'S', 's').
// Public domain style 5x7 patterns expanded to 6 columns (last column blank).
typedef struct { char ch; uint8_t col[6]; } glyph_t;
static const glyph_t s_glyphs[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00}},
    {'0',{0x3e,0x51,0x49,0x45,0x3e,0x00}},
    {'1',{0x00,0x42,0x7f,0x40,0x00,0x00}},
    {'2',{0x42,0x61,0x51,0x49,0x46,0x00}},
    {'3',{0x21,0x41,0x45,0x4b,0x31,0x00}},
    {'4',{0x18,0x14,0x12,0x7f,0x10,0x00}},
    {'5',{0x27,0x45,0x45,0x45,0x39,0x00}},
    {'6',{0x3c,0x4a,0x49,0x49,0x30,0x00}},
    {'7',{0x01,0x71,0x09,0x05,0x03,0x00}},
    {'8',{0x36,0x49,0x49,0x49,0x36,0x00}},
    {'9',{0x06,0x49,0x49,0x29,0x1e,0x00}},
    {'.',{0x00,0x60,0x60,0x00,0x00,0x00}},
    {':',{0x00,0x36,0x36,0x00,0x00,0x00}},
    {'%',{0x62,0x64,0x08,0x13,0x23,0x00}},
    {'A',{0x7e,0x11,0x11,0x11,0x7e,0x00}},
    {'B',{0x7f,0x49,0x49,0x49,0x36,0x00}},
    {'C',{0x3e,0x41,0x41,0x41,0x22,0x00}},
    {'D',{0x7f,0x41,0x41,0x22,0x1c,0x00}},
    {'E',{0x7f,0x49,0x49,0x49,0x41,0x00}},
    {'F',{0x7f,0x09,0x09,0x09,0x01,0x00}},
    {'G',{0x3e,0x41,0x49,0x49,0x7a,0x00}},
    {'H',{0x7f,0x08,0x08,0x08,0x7f,0x00}},
    {'I',{0x00,0x41,0x7f,0x41,0x00,0x00}},
    {'J',{0x20,0x40,0x41,0x3f,0x01,0x00}},
    {'K',{0x7f,0x08,0x14,0x22,0x41,0x00}},
    {'L',{0x7f,0x40,0x40,0x40,0x40,0x00}},
    {'M',{0x7f,0x02,0x04,0x02,0x7f,0x00}},
    {'N',{0x7f,0x04,0x08,0x10,0x7f,0x00}},
    {'O',{0x3e,0x41,0x41,0x41,0x3e,0x00}},
    {'P',{0x7f,0x09,0x09,0x09,0x06,0x00}},
    {'Q',{0x3e,0x41,0x51,0x21,0x5e,0x00}},
    {'R',{0x7f,0x09,0x19,0x29,0x46,0x00}},
    {'S',{0x46,0x49,0x49,0x49,0x31,0x00}},
    {'T',{0x01,0x01,0x7f,0x01,0x01,0x00}},
    {'U',{0x3f,0x40,0x40,0x40,0x3f,0x00}},
    {'V',{0x1f,0x20,0x40,0x20,0x1f,0x00}},
    {'W',{0x7f,0x20,0x18,0x20,0x7f,0x00}},
    {'X',{0x63,0x14,0x08,0x14,0x63,0x00}},
    {'Y',{0x07,0x08,0x70,0x08,0x07,0x00}},
    {'Z',{0x61,0x51,0x49,0x45,0x43,0x00}},
    {'s',{0x26,0x49,0x49,0x49,0x32,0x00}}, // lowercase s (used in 's' seconds)
};

// Forward declarations for new drawing helpers
static const uint8_t *lookup_glyph(char c);
static inline void put_pixel(int x,int y,uint8_t on);
static void draw_glyph_scaled(int x,int y,const uint8_t *g,uint8_t scale);
// Basic glyph lookup (reuse font6x8 ordering for digits only currently)
static const uint8_t *lookup_glyph(char c){
    if (c>='a' && c<='z' && c!='s') c = (char)(c - 'a' + 'A'); // promote to uppercase except custom 's'
    for (size_t i=0;i<sizeof(s_glyphs)/sizeof(s_glyphs[0]);++i){ if (s_glyphs[i].ch == c) return s_glyphs[i].col; }
    // fallback space
    return s_glyphs[0].col;
}
static inline void put_pixel(int x,int y,uint8_t on){ if(x<0||x>=DISP_W||y<0||y>=DISP_H)return; if(on) s_buf[(y/8)*DISP_W + x] |= (1<<(y&7)); else s_buf[(y/8)*DISP_W + x] &= ~(1<<(y&7)); }
static void draw_glyph_scaled(int x,int y,const uint8_t *g,uint8_t scale){
    if(!g){ return; }
    if(scale==0 || scale>3) scale=1; // sanity
    for(int col=0;col<6;++col){
        int xx=x+col*scale; if(xx>=DISP_W) break; if(xx<0) continue; uint8_t bits=g[col];
        for(int row=0;row<8;++row){ if(bits&(1<<row)){ for(int dx=0;dx<scale;++dx) for(int dy=0;dy<scale;++dy) put_pixel(xx+dx,y+row*scale+dy,s_draw_color); } }
    }
}

// Push whole buffer via I2C using SSD1306 RAM write sequence.
static i2c_master_dev_handle_t s_dev;
static esp_err_t write_cmd(uint8_t cmd) {
    uint8_t buf[2]={0x00,cmd};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100/portTICK_PERIOD_MS);
}
static esp_err_t write_block(const uint8_t *data, size_t len) {
    uint8_t tmp[1+16];
    while(len){
        size_t chunk=len>16?16:len; tmp[0]=0x40; memcpy(&tmp[1],data,chunk);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev,tmp,chunk+1,100/portTICK_PERIOD_MS),TAG,"tx fail");
        data+=chunk; len-=chunk;
    }
    return ESP_OK;
}

esp_err_t u8g2p_init(void) {
    if(!s_dev){ s_dev = i2c_v2_get_dev(U8G2P_I2C_ADDR); if(!s_dev) return ESP_FAIL; }
    // Use PAGE addressing (0x20,0x02). Horizontal mode caused Wokwi partial refresh (only first page) due to emulator quirk.
    const uint8_t init_seq[] = {0xAE,0x20,0x02,0xC8,0x00,0x10,0x40,0x81,0x7F,0xA1,0xA6,0xA8,0x3F,0xA4,0xD3,0x00,0xD5,0x80,0xD9,0xF1,0xDA,0x12,0xDB,0x40,0x8D,0x14,0xAF};
    for (size_t i=0;i<sizeof(init_seq);++i) {
        ESP_ERROR_CHECK(write_cmd(init_seq[i]));
    }
    u8g2p_clear();
    // Immediately push a cleared buffer to ensure controller RAM is clean (avoid ghost on Wokwi)
    u8g2p_commit();
    return ESP_OK;
}

void u8g2p_commit(void) {
    // Page-wise push so Wokwi (and most SSD1306 clones) refresh reliably.
    for(uint8_t page=0; page<8; ++page){
        write_cmd(0xB0 | page); // set page address
        write_cmd(0x00);        // lower column = 0
        write_cmd(0x10);        // higher column = 0
        write_block(&s_buf[page*DISP_W], DISP_W);
    }
}

void u8g2p_print_char(char c){ const uint8_t *g = lookup_glyph(c); draw_glyph_scaled(s_cur_x_px, s_cur_y_px, g, s_scale); s_cur_x_px += 6 * s_scale; }
void u8g2p_print(const char *t){
    while(*t){
        if (s_cur_x_px + 6*s_scale > DISP_W){ // wrap to next row
            s_cur_x_px = 0;
            s_cur_y_px += 8 * s_scale;
            if (s_cur_y_px >= DISP_H) break; // stop if no space
        }
        u8g2p_print_char(*t++);
    }
}
void u8g2p_draw_box(uint8_t x,uint8_t y,uint8_t w,uint8_t h){ for(uint8_t yy=0; yy<h; ++yy) for(uint8_t xx=0; xx<w; ++xx) put_pixel(x+xx,y+yy,s_draw_color); }
void u8g2p_draw_frame(uint8_t x,uint8_t y,uint8_t w,uint8_t h){ for(uint8_t xx=0;xx<w;++xx){ put_pixel(x+xx,y,s_draw_color); put_pixel(x+xx,y+h-1,s_draw_color);} for(uint8_t yy=0;yy<h;++yy){ put_pixel(x,y+yy,s_draw_color); put_pixel(x+w-1,y+yy,s_draw_color);} }

void u8g2p_draw_bitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *bits){
    // Rows packed left-to-right, 8 horizontal pixels per byte, MSB first (common XBM style).
    // We previously treated LSB-first which produced garbled/mirrored graphics in Wokwi.
    uint16_t idx=0; uint8_t byte=0;
    for(uint8_t yy=0; yy<h; ++yy){
        for(uint8_t xx=0; xx<w; ++xx){
            if((xx & 7)==0){ byte = bits[idx++]; }
            uint8_t bit = 0x80 >> (xx & 7);
            if (byte & bit) put_pixel(x+xx, y+yy, s_draw_color); else if(!s_draw_color) put_pixel(x+xx,y+yy,0);
        }
    }
}
