#include "display_task.h"
#include "ssd1306.h"
#include "protocol_state.h"
#include "arduino_compat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include <stdio.h>
#include <string.h>

#ifndef UI_I2C_PORT
#define UI_I2C_PORT I2C_NUM_0
#endif

// Simple interim UI: show speed (km/h integer), battery %, current (A with sign) cycling every 1s
__attribute__((unused)) static void format_number(char *buf, size_t sz, int val) {
    snprintf(buf, sz, "%d", val);
}

static void draw_center_text(uint8_t page, const char *text) {
    // crude centering for 8x16 font, width 8*strlen
    size_t len = strlen(text);
    if (len > 16) len = 16; // clamp
    uint8_t px = (128 - (len * 8)) / 2;
    ssd1306_draw_text8x16(UI_I2C_PORT, px, page, text);
}

static void ui_task(void *arg) {
    uint8_t mode = 0; // 0 speed,1 battery,2 current
    while (1) {
        ssd1306_clear(UI_I2C_PORT);
        char line[24];
        switch (mode) {
            case 0: {
                // speed from g_S23CB0.speed (raw *0.1 km/h approx after wheel factor). We'll just show raw/100.
                int raw = g_S23CB0.speed; if (raw < 0) raw = -raw; int sp = raw / 100; // rough
                snprintf(line, sizeof(line), "SPD %d", sp);
                draw_center_text(0, line);
                break; }
            case 1: {
                snprintf(line, sizeof(line), "BAT %u%%", (unsigned)g_S25C31.remainPercent);
                draw_center_text(0, line);
                break; }
            case 2: {
                int16_t c = protocol_total_current_cA();
                int a = c / 100; if (a < -99) a = -99; if (a > 999) a = 999;
                snprintf(line, sizeof(line), "CUR %dA", a);
                draw_center_text(0, line);
                break; }
        }
        mode = (mode + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ui_start(void) {
    xTaskCreatePinnedToCore(ui_task, "ui_task", 2048, NULL, 4, NULL, 0);
}
