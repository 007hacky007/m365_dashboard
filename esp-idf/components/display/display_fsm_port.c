// Fallback simple display build switch (1 = use minimal direct SSD1306 writer, skip u8g2 buffer)
#ifndef SIMPLE_DISPLAY_FALLBACK
#define SIMPLE_DISPLAY_FALLBACK 0
#endif

#include "display_fsm_port.h"
#if !SIMPLE_DISPLAY_FALLBACK
#include "u8g2_integration.h" // Only when full path active
#endif
#include "ssd1306.h"
#include "protocol_state.h"
#include "comms_port.h" // for g_busEverSeen / g_lastBusDataMs externs
#include "arduino_compat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "font_system5x7mod.h"
#include "range_estimator.h"
#include "aht10.h"

// Integrate legacy Arduino numeric font (stdNumb) for nicer large digits.
// Original header lives under M365/fonts/stdNumb.h. Provide minimal macro so it compiles in C.
#ifndef GLCDFONTDECL
#define GLCDFONTDECL(name) static const uint8_t name[]
#endif
#include "stdNumb.h"

// Force-enable input debug as requested (supports legacy/typo macro DUI_INPUT_DEBUG)
#if defined(DUI_INPUT_DEBUG) && !defined(UI_INPUT_DEBUG)
#define UI_INPUT_DEBUG DUI_INPUT_DEBUG
#endif
#ifndef UI_INPUT_DEBUG
#define UI_INPUT_DEBUG 1
#endif
#ifndef UI_INPUT_FAST_TAP_EDGE
#define UI_INPUT_FAST_TAP_EDGE 0   // Set to 1 for immediate edge on threshold crossing (less filtering)
#endif
// Choose serial logging only (no on-screen overlay)
#if UI_INPUT_DEBUG
#include "esp_log.h"
static const char *TAG_UI_IN __attribute__((unused)) = "UIIN";
#ifndef UI_INPUT_DEBUG_DRAW
#define UI_INPUT_DEBUG_DRAW 0
#endif
#ifndef UI_INPUT_DEBUG_SERIAL
#define UI_INPUT_DEBUG_SERIAL 1
#endif
#endif

// Fallback: allow switching to a built-in u8g2 font for diagnostics if custom bitmap font not showing
#ifndef USE_U8G2_BUILTIN_NUM_FONT
#define USE_U8G2_BUILTIN_NUM_FONT 0  // revert to custom legacy numeric font
#endif

// stdNumb format (interpreted):
// Bytes: 0,1 (unused), width (w), height (h), firstChar, count, then for each glyph: w*2 bytes (low byte column bits, then high byte column bits) => supports up to 16 rows; stdNumb uses 14.
// We implement a simple renderer returning glyph pointer and drawing it column-wise.
typedef struct { const uint8_t *base; uint8_t w; uint8_t h; uint8_t first; uint8_t count; } font_stdnum_t;
static const font_stdnum_t g_stdnum = { stdNumb, stdNumb[2], stdNumb[3], stdNumb[4], stdNumb[5] };
static inline const uint8_t* stdnum_glyph(char c){
    if ((uint8_t)c < g_stdnum.first || (uint8_t)c >= (uint8_t)(g_stdnum.first + g_stdnum.count)) c = ' ';
    uint8_t idx = (uint8_t)c - g_stdnum.first;
    size_t header = 6; // per definition
    size_t stride = g_stdnum.w * 2; // low + high bytes per column
    return g_stdnum.base + header + (size_t)idx * stride;
}
static void stdnum_draw_string(u8g2_t *u, int x, int y, const char *s){
#if USE_U8G2_BUILTIN_NUM_FONT
    // Diagnostic path: use a known big font (digits only) to prove region updates
    u8g2_SetFont(u, u8g2_font_logisoso24_tn);
    // y passed here is top; u8g2 uses baseline so shift baseline by font ascent (~24px height -> baseline at y+24)
    int baseline = y + 24;
    u8g2_DrawStr(u, x, baseline, s);
#else
    while(*s){
        const uint8_t *g = stdnum_glyph(*s++);
        for(int col=0; col<g_stdnum.w; ++col){
            uint16_t bits = g[col] | ((uint16_t)g[col + g_stdnum.w] << 8);
            for(int row=0; row<g_stdnum.h; ++row){ if(bits & (1u<<row)) u8g2_DrawPixel(u, x+col, y+row); }
        }
        x += g_stdnum.w + 1;
    }
#endif
}

// Battery percent: use raw remainPercent (0..100) only when valid frame received; no voltage fallback
static int get_filtered_percent(void){
    extern volatile bool g_batteryDataValid;
    if (g_batteryDataValid && g_S25C31.remainPercent <= 100) return g_S25C31.remainPercent;
#if M365_DISABLE_TX
    // Passive heuristic: approximate percent from voltage range 3300-4200mV
    int v = g_S25C31.voltage;
    if (v >= 3300 && v <= 4300) {
        if (v > 4200) v = 4200; if (v < 3300) v = 3300;
        int pct = (int)((v - 3300) * 100 / 900); if (pct < 0) pct = 0; if (pct>100) pct=100; return pct;
    }
#endif
    return -1;
}

#if SIMPLE_DISPLAY_FALLBACK
// ----------------------------------------------------------------------------------
// Minimal direct-draw display task (no framebuffer) to isolate u8g2 related crashes
// ----------------------------------------------------------------------------------
static void simple_display_task(void *arg){
    ssd1306_init(I2C_NUM_0, -1);
    while(1){
        ssd1306_clear(I2C_NUM_0);
        char line[16];
        int speedRaw = g_S23CB0.speed; if (speedRaw < 0) speedRaw = -speedRaw; int sp = speedRaw/100; if (sp>99) sp=99;
        snprintf(line,sizeof(line),"SPD %2d", sp);
        ssd1306_draw_text8x16(I2C_NUM_0, 0, 0, line);
        bool voltageValid = (g_S25C31.voltage >= 2000 && g_S25C31.voltage <= 5000);
        if (voltageValid){ int v = g_S25C31.voltage; if (v<0) v=-v; int vw=v/100; int vf=v%100; snprintf(line,sizeof(line),"%2d.%02dV", vw, vf); }
        else { snprintf(line,sizeof(line),"--.--V"); }
        ssd1306_draw_text8x16(I2C_NUM_0, 0, 2, line);
        extern volatile bool g_batteryDataValid; 
        if (g_batteryDataValid && g_S25C31.remainPercent<=100){ snprintf(line,sizeof(line),"%3u%%", (unsigned)g_S25C31.remainPercent); }
        else { snprintf(line,sizeof(line)," --%%"); }
        // place battery percent top-right (col 10 => x=80)
        ssd1306_draw_text8x16(I2C_NUM_0, 80, 0, line);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void display_port_start(void){
    xTaskCreatePinnedToCore(simple_display_task, "disp_simple", 2048, NULL, 4, NULL, 0);
}

#else // SIMPLE_DISPLAY_FALLBACK

// Settings access
#include "settings.h"
// (legacy bigMode & font style omitted in this port for now)
// static uint8_t bigFontStyle = 0;       // placeholder (reserved for future font selection)

// Big screen hysteresis state
static bool s_bigActive = false; 
static uint32_t s_bigReleaseAt = 0; 

// -------- Multi-view / trip stats parity state --------
// 0 = main, 1 = trip stats, 2 = odometer/time, 3 = temps (if enabled)
static uint8_t s_uiAltScreen = 0; // public cycle index
// Input edge tracking
static int8_t s_oldBrakeVal = 0, s_oldThrottleVal = 0;
// Brake duration tracking (logical press time while brakeVal==1)
static uint32_t s_brakePressStartMs = 0;    // 0 when not in an active press
static uint32_t s_brakeLastDurationMs = 0;  // duration of the last completed press (ms)
// Throttle duration tracking (mirrors brake logic for user visibility)
static uint32_t s_throttlePressStartMs = 0;    // 0 when not actively pressed
static uint32_t s_throttleLastDurationMs = 0;  // last completed throttle press duration (ms)
static bool s_inputWarmup = true; // suppress first edge so we stay on main screen after boot
#if UI_INPUT_DEBUG
static int dbg_throttle=0, dbg_brake=0, dbg_speed=0, dbg_thVal=0, dbg_brVal=0;
#endif
// Trip aggregation (ported logic subset)
static uint32_t s_lastPowerOnTime_s = 0;
static uint32_t s_tripEnergy_Wh_x100 = 0;      // Wh *100
static uint16_t s_tripMaxCurrent_cA = 0;       // centi-amps
static uint32_t s_tripMaxPower_Wx100 = 0;      // W *100
static uint16_t s_tripMinVoltage_cV = 0xFFFF;  // centi-volts
static uint16_t s_tripMaxVoltage_cV = 0;       // centi-volts
// Legacy compatibility: some stale builds referenced variable 'durMs'. It mapped to last throttle press duration.
// If needed again, uncomment macro below (kept as comment to avoid non-constant initializer issues at file scope).
// #define durMs s_throttleLastDurationMs

// Forward decls for alt screens
static void render_trip_stats_screen(void);
static void render_odometer_screen(void);
static void render_temps_screen(void);
static void accumulate_trip_metrics(void);

static u8g2_t *get_u(){ return u8g2_int(); }
static void draw_text(uint8_t col, uint8_t row, const char *t){
    // System5x7 baseline top-left: row*8 for vertical spacing; add 1px top margin inside 8px cell
    sys57_draw(get_u(), col*6, row*8 + 1, t);
}

// Simple 2x scaler for the 5x7 font (nearest-neighbor) to better match original Arduino visual size
// (legacy 2x scaler kept for reference, currently unused)
#if 0
static void draw_text2x(uint8_t col, uint8_t row, const char *t){ /* unused */ }
#endif

static void render_low_batt_overlay(uint8_t percent){
    // Centered 12x4 char box (96x32)
    const uint8_t COLS=12, ROWS=4; uint8_t x=(128-96)/2; uint8_t y=(64-32)/2;
    // Simplified overlay using u8g2 primitives
    u8g2_t *u = get_u();
    u8g2_DrawBox(u,x,y,96,32);
    u8g2_SetDrawColor(u,0); u8g2_DrawBox(u,x+1,y+1,94,30); u8g2_SetDrawColor(u,1); u8g2_DrawFrame(u,x,y,96,32);
    draw_text( (16-COLS)/2 + 1, (8-ROWS)/2 + 0, "LOW" );
    char b[8]; snprintf(b,sizeof(b),"BATT"); draw_text( (16-COLS)/2 + 1, (8-ROWS)/2 + 1, b);
    char p[8]; snprintf(p,sizeof(p),"%u%%", percent); draw_text( (16-COLS)/2 + 2, (8-ROWS)/2 + 2, p);
}

static void render_bus_overlay(void){
    const uint32_t now = millis();
    const uint32_t FIRST_FRAME_GRACE = 3000;   // wait 3s after boot before alarming
    const uint32_t STALE_THRESHOLD   = 4500;   // must be >4.5s since last data (increase to avoid flicker)
    const uint32_t SHOW_DELAY        = 1500;   // stale condition must persist this long before overlay appears
    static uint32_t staleSince = 0;            // when we first noticed stale
    bool stale = false; uint32_t delta = 0;
    if (!g_busEverSeen){
        // No data ever yet
        if (now > FIRST_FRAME_GRACE){
            stale = true; delta = now; // duration displayed as time since start (approx.)
        }
    } else {
        uint32_t last = g_lastBusDataMs;
        if (last && (now - last) > STALE_THRESHOLD){
            stale = true; delta = now - last;
        }
    }
    if (!stale){ staleSince = 0; return; }
    if (staleSince == 0){ staleSince = now; return; } // start debounce period
    if (now - staleSince < SHOW_DELAY) return; // not yet
    const uint8_t w=96,h=32; uint8_t x=(128-w)/2; uint8_t y=(64-h)/2;
    u8g2_t *u = get_u();
    u8g2_SetDrawColor(u,1); u8g2_DrawFrame(u,x,y,w,h);
    draw_text( (x/8)+4, (y/8)+0, "BUS" );
    draw_text( (x/8)+1, (y/8)+1, "NO CONNECTION" );
    float s = delta/1000.f; if (s>99.9f) s=99.9f; char buf[12]; snprintf(buf,sizeof(buf),"%.1fs", s);
    draw_text( (x/8)+4, (y/8)+2, buf );
}

static void update_big_state(int speedRaw){
    ui_settings_t *cfg = settings_ui();
    if (!cfg->autoBig){ s_bigActive=false; s_bigReleaseAt=0; return; }
    const int ENTER=220; const int EXIT=180; const uint32_t HOLD=1500;
    if (speedRaw > ENTER){ s_bigActive=true; s_bigReleaseAt=0; }
    else if (s_bigActive){ if (s_bigReleaseAt==0) s_bigReleaseAt = millis()+HOLD; if (millis() >= s_bigReleaseAt && speedRaw <= EXIT){ s_bigActive=false; s_bigReleaseAt=0; } }
}

static void render_big_screen(int speedRaw){
    u8g2_ClearBuffer(get_u());
    ui_settings_t *cfg = settings_ui();
    int sp = (speedRaw<0?-speedRaw:speedRaw)/100; if (sp>199) sp=199; // integer km/h (legacy scaling)
    // Simple 16x32 digit bitmaps (0-9) stored row-major, 1bpp, 16 wide => 2 bytes/row *32 rows =64 bytes per glyph
    // Minimal subset for speed/current/power (digits only). Pattern: top aligned, bold style.
    static const uint8_t dig16x32[][64] = {
        // '0'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00},
        // '1'
        {0x38,0x00,0x3C,0x00,0x3E,0x00,0x3F,0x00,0x3F,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0xFF,0x3F,0xFF,0x3F,0x00,0x00,
         0x00,0x00,0x38,0x00,0x3C,0x00,0x3E,0x00,0x3F,0x00,0x3F,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00,0xFF,0x3F,0xFF,0x3F,0x00,0x00},
        // '2'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x00,0x70,0x00,0x78,0x00,0x3E,0x80,0x1F,0xE0,0x07,0xF0,0x03,0xF8,0x01,0x1E,0x00,0x0F,0x00,0x07,0x00,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x00,0x70,0x00,0x78,0x00,0x3E,0x80,0x1F,0xE0,0x07,0xF0,0x03,0xF8,0x01,0x1E,0x00,0x0F,0x00,0x07,0x00},
        // '3'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x00,0x70,0x80,0x7F,0x80,0x7F,0x80,0x7F,0x00,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x00,0x70,0x80,0x7F,0x80,0x7F,0x80,0x7F,0x00,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F},
        // '4'
        {0x00,0x1E,0x00,0x1F,0x80,0x1F,0xC0,0x1D,0xE0,0x1C,0x70,0x1C,0x38,0x1C,0x1C,0x1C,0x0E,0x1C,0xFF,0x7F,0xFF,0x7F,0xFF,0x7F,0x0E,0x1C,0x0E,0x1C,0x0E,0x1C,0x00,0x00,
         0x00,0x00,0x00,0x1E,0x00,0x1F,0x80,0x1F,0xC0,0x1D,0xE0,0x1C,0x70,0x1C,0x38,0x1C,0x1C,0x1C,0x0E,0x1C,0xFF,0x7F,0xFF,0x7F,0xFF,0x7F,0x0E,0x1C,0x0E,0x1C,0x0E,0x1C},
        // '5'
        {0xFF,0x3F,0xFF,0x3F,0x0E,0x00,0x0E,0x00,0xFE,0x0F,0xFE,0x3F,0x0E,0x78,0x00,0x70,0x00,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF8,0x1F,0xE0,0x07,0x00,0x00,
         0x00,0x00,0xFF,0x3F,0xFF,0x3F,0x0E,0x00,0x0E,0x00,0xFE,0x0F,0xFE,0x3F,0x0E,0x78,0x00,0x70,0x00,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF8,0x1F,0xE0,0x07},
        // '6'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x00,0x8E,0x0F,0xFE,0x3F,0xFE,0x7B,0x0E,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x00,0x8E,0x0F,0xFE,0x3F,0xFE,0x7B,0x0E,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00},
        // '7'
        {0xFF,0x7F,0xFF,0x7F,0x00,0x78,0x00,0x3C,0x80,0x1F,0xC0,0x0F,0xE0,0x07,0xF0,0x03,0xF8,0x01,0x1E,0x00,0x0F,0x00,0x07,0x00,0x07,0x00,0x03,0x00,0x03,0x00,0x00,0x00,
         0x00,0x00,0xFF,0x7F,0xFF,0x7F,0x00,0x78,0x00,0x3C,0x80,0x1F,0xC0,0x0F,0xE0,0x07,0xF0,0x03,0xF8,0x01,0x1E,0x00,0x0F,0x00,0x07,0x00,0x07,0x00,0x03,0x00,0x03,0x00},
        // '8'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x3F,0xF0,0x0F,0x00,0x00},
        // '9'
        {0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x7F,0xF8,0x7F,0xE0,0x70,0xE0,0x70,0x1E,0x70,0xFC,0x3F,0xF0,0x0F,0x00,0x00,0x00,0x00,
         0x00,0x00,0xF0,0x0F,0xFC,0x3F,0x1E,0x78,0x0E,0x70,0x0E,0x70,0x0E,0x70,0x1E,0x78,0xFC,0x7F,0xF8,0x7F,0xE0,0x70,0xE0,0x70,0x1E,0x70,0xFC,0x3F,0xF0,0x0F,0x00,0x00}
    };
    // Pre-compute electrical metrics (current & power) for possible primary/secondary use.
    int16_t c_cA = protocol_total_current_cA();
    int cA_abs = c_cA < 0 ? -c_cA : c_cA; 
    int cur_A = cA_abs / 100;               // integer amps
    int W = ( (c_cA/100.0f) * (g_S25C31.voltage/100.0f) ); if (W<0) W=-W; if (W>9999) W=9999; // instantaneous Watts

    // Decide primary value based on bigMode:
    // bigMode 0 => show SPEED (legacy)
    // bigMode 1 => show CURRENT or POWER (depends on showPower flag)
    int primaryVal = sp; // default speed
    if (cfg->bigMode == 1){
        if (cfg->showPower) primaryVal = W; else primaryVal = cur_A; // choose power or current
    }
    // Clamp displayed value range for digit buffer (allow up to 4 digits)
    if (primaryVal < 0) primaryVal = -primaryVal; // display absolute
    if (primaryVal > 9999) primaryVal = 9999;

    // Buffer large enough for any int (though we clamp to 0..9999). Larger size silences -Wformat-truncation.
    char buf[12]; snprintf(buf,sizeof(buf),"%d", primaryVal);
    int digits = strlen(buf); if (digits<1) digits=1; if (digits>4) digits=4; // safety
    int totalW = digits * 18 - 2; // 16px glyph +2px spacing
    int startX = (128 - totalW)/2; int y=0;
    for(int i=0;i<digits;i++){
        int d = buf[i]-'0'; if (d<0||d>9) d=0; u8g2_DrawXBMP(get_u(), startX + i*18, y, 16, 32, dig16x32[d]);
    }

    // Secondary line content differs by mode.
    u8g2_t *u = get_u();
    u8g2_SetDrawColor(u,1);
    char line[24];
    bool voltageValid = (g_S25C31.voltage >= 2000 && g_S25C31.voltage <= 5000);
    int volt_cV = g_S25C31.voltage; int volt_whole = (volt_cV<0?-volt_cV:volt_cV)/100; int volt_frac = (volt_cV<0?-volt_cV:volt_cV)%100;
    int pct = get_filtered_percent();
    int cursorY = 48; // baseline
    int xOff = 0;
    if (cfg->bigMode == 0){
        // Legacy: show power or current first
        if (cfg->showPower) snprintf(line,sizeof(line),"%4dW", W); else snprintf(line,sizeof(line),"%3dA", cur_A);
        u8g2_DrawStr(u,0,cursorY,line); xOff = u8g2_GetStrWidth(u,line)+2;
    } else {
        // bigMode 1: primary already current/power; show speed small at start
        snprintf(line,sizeof(line),"%3dK", sp); // simple speed w/ 'K' tag (km/h)
        u8g2_DrawStr(u,0,cursorY,line); xOff = u8g2_GetStrWidth(u,line)+2;
    }
    // Battery percent
    if (pct>=0){ snprintf(line,sizeof(line),"%3d%%", pct); u8g2_DrawStr(u,xOff,cursorY,line); xOff += u8g2_GetStrWidth(u,line)+2; }
    else { u8g2_DrawStr(u,xOff,cursorY,"--%%"); xOff += u8g2_GetStrWidth(u,"--%%")+2; }
    // Voltage
    if (voltageValid){ snprintf(line,sizeof(line),"%2d.%02dV", volt_whole, volt_frac); u8g2_DrawStr(u,xOff,cursorY,line);} else u8g2_DrawStr(u,xOff,cursorY,"--.--V");

    // Battery bar at bottom (0-100%)
    if (pct>=0){
        int barW = 100; int bx = (128-barW)/2; int by=56; int fill = (pct* (barW-4))/100;
        u8g2_DrawFrame(u,bx,by,barW,8);
        u8g2_DrawBox(u,bx+2,by+1,fill,6);
    }
}

static void render_main_screen(int speedRaw){
    u8g2_ClearBuffer(get_u());
    u8g2_t *u_verify = get_u();
    u8g2_SetDrawColor(u_verify, 1);
    char line[32];
    ui_settings_t *cfg = settings_ui();
    int speedAbs = speedRaw < 0 ? -speedRaw : speedRaw;
    int sp_whole = speedAbs / 1000; if (sp_whole > 99) sp_whole = 99; int sp_tenths = (speedAbs % 1000) / 100;
    int vAbs = g_S25C31.voltage < 0 ? -g_S25C31.voltage : g_S25C31.voltage;
    int v_whole = vAbs / 100; int v_frac = vAbs % 100; bool voltageValid = (vAbs >= 2000 && vAbs <= 5000);
    static bool fontLogOnce=false;
    // Width helper
    int stdnum_text_w(const char *s){ int n=0; while(*s){ ++n; ++s; } return n? n*(g_stdnum.w+1)-1 : 0; }
    // Unit baseline helper
    void draw_unit_after(int xRight, int yTop, const char *u){ sys57_draw(get_u(), xRight+2, yTop+7, u); }

    // Primary left
    if (cfg->showVoltageMain){ if (voltageValid) snprintf(line,sizeof(line),"%c%d.%02d", (v_whole<10?' ': (v_whole/10)+'0'), v_whole%10, v_frac); else strcpy(line," 00.00"); }
    else { snprintf(line,sizeof(line),"%c%d.%d", (sp_whole<10?' ': (sp_whole/10)+'0'), sp_whole%10, sp_tenths); if (speedAbs < 50) strcpy(line," 00.0"); }
    stdnum_draw_string(get_u(), 0, 0, line);
    int primaryRight = stdnum_text_w(line);
    draw_unit_after(primaryRight,0, cfg->showVoltageMain?"V":"KMH");

    // Temperature large right
    extern volatile bool g_mainTempAFValid; extern volatile int16_t g_mainTempC10_AF;
    int16_t tempC10=0; switch(cfg->mainTempSource){ case 1: tempC10=g_S25C31.temp1*10; break; case 2: tempC10=g_S25C31.temp2*10; break; case 3: default: {
#if CFG_AHT10_ENABLE
        if (cfg->mainTempSource==3 && g_ahtPresent) tempC10=(int16_t)(g_ahtTempC*10.0f); else if(cfg->mainTempSource==3) tempC10 = (g_mainTempAFValid? g_mainTempC10_AF : g_S23CB0.mainframeTemp); else tempC10=(g_mainTempAFValid? g_mainTempC10_AF : g_S23CB0.mainframeTemp);
#else
        tempC10 = (g_mainTempAFValid? g_mainTempC10_AF : g_S23CB0.mainframeTemp);
#endif
    } }
    int tC=tempC10/10; if(tC<-99)tC=-99; if(tC>99)tC=99; char tbuf[6]; snprintf(tbuf,sizeof(tbuf),"%2d",tC);
    int tW=stdnum_text_w(tbuf); int tX=128 - tW - 10; if (tX < primaryRight + 8) tX = primaryRight + 8; stdnum_draw_string(get_u(), tX, 0, tbuf); draw_unit_after(tX+tW,0,"C");
    if(!fontLogOnce){ fontLogOnce=true; printf("[FONTDBG] main primary='%s' t='%s'\n", line, tbuf); }

    // Distance large left (row 2 top=16)
    extern volatile uint32_t g_afTripDistance_m;
    uint32_t mCurrRaw=g_S23CB0.mileageCurrent; // fallback centi-km (0.01 km units)
    // If AF trip distance present (meters) and reasonable (< 1,000,000 m), convert to 0.01 km units
    if (g_afTripDistance_m > 0 && g_afTripDistance_m < 1000000UL) {
        // meters -> km*100 = (m *100)/1000 = m/10
        mCurrRaw = g_afTripDistance_m / 10U; // integer division
    }
    uint32_t m_whole=mCurrRaw/100; uint32_t m_frac=mCurrRaw%100; if(m_whole>9999)m_whole=9999;
    snprintf(line,sizeof(line),"%4lu.%02lu", (unsigned long)m_whole,(unsigned long)m_frac);
    stdnum_draw_string(get_u(),0,16,line); draw_unit_after(stdnum_text_w(line),16,"KM");

    // Ride time large left (row 4 top=32)
    uint32_t ride_s=g_S23C3A.ridingTime; uint32_t mm=ride_s/60; uint32_t ss=ride_s%60; if(mm>999)mm=999; snprintf(line,sizeof(line),"%03lu:%02lu",(unsigned long)mm,(unsigned long)ss); stdnum_draw_string(get_u(),0,32,line);

    // Current/power large right
    int16_t c_cA=protocol_total_current_cA(); int cAbs=c_cA<0?-c_cA:c_cA; int cur_h=cAbs/100; int cur_l=cAbs%100;
    if(!cfg->showPower) snprintf(line,sizeof(line),"%2d.%02dA",cur_h,cur_l); else { int W=(int)(((float)c_cA/100.0f)*((float)g_S25C31.voltage/100.0f)); if(W<0)W=-W; if(W>9999)W=9999; snprintf(line,sizeof(line),"%4dW",W);} 
    int pw=stdnum_text_w(line); int px=128 - pw -1; stdnum_draw_string(get_u(), px,32,line);

    // Range small (row 6 -> y=6*8+1≈49)
    float rem_km=range_get_estimate_km(); if(rem_km<0)rem_km=0; if(rem_km>999.9f) rem_km=999.9f; uint16_t r_int=(uint16_t)rem_km; uint16_t r_dec=(uint16_t)((rem_km-(float)r_int)*10.0f+0.5f); snprintf(line,sizeof(line),"%3u.%1u",(unsigned)r_int,(unsigned)r_dec);
    int smallW=strlen(line)*6; int startX=128 - smallW - 12; if(startX<0) startX=0; sys57_draw(get_u(), startX, 6*8+1, line); sys57_draw(get_u(), startX+smallW+2, 6*8+1, "KM");

    // Battery bar bottom + percent small
    int pct = get_filtered_percent(); u8g2_t *u=get_u(); int percent=pct; if(percent<0)percent=0; if(percent>100)percent=100;
    const uint8_t baseY=56, segW=4, segH=6, step=5, baseX=5; for(int i=0;i<19;i++){ int x=baseX+i*step; if ((float)19/100*percent>i) u8g2_DrawBox(u,x,baseY+1,segW,segH); else u8g2_DrawFrame(u,x,baseY+1,segW,segH);} 
    if(pct>=0) snprintf(line,sizeof(line),"%3d%%",pct); else snprintf(line,sizeof(line)," --%%"); int len=strlen(line); sys57_draw(get_u(),128-len*6,7*8+1,line);
}

// ---------- Trip metrics accumulation (invoked each frame) ----------
static void accumulate_trip_metrics(void){
    uint32_t pot_s = g_S23C3A.powerOnTime; // seconds since power on (scooter)
    if (s_lastPowerOnTime_s == 0 || pot_s < s_lastPowerOnTime_s){
        // Reset (wrap or first sample)
        s_lastPowerOnTime_s = pot_s;
        s_tripEnergy_Wh_x100 = 0;
        s_tripMaxCurrent_cA = 0; s_tripMaxPower_Wx100 = 0;
        s_tripMinVoltage_cV = 0xFFFF; s_tripMaxVoltage_cV = 0;
    }
    uint32_t dt = (pot_s >= s_lastPowerOnTime_s)? (pot_s - s_lastPowerOnTime_s) : 0;
    int16_t cur_cA_raw = protocol_total_current_cA();
    uint16_t cur_cA_abs = (uint16_t)(cur_cA_raw<0?-cur_cA_raw:cur_cA_raw);
    uint16_t v_cV = (uint16_t)(g_S25C31.voltage<0?-g_S25C31.voltage:g_S25C31.voltage);
    bool v_valid = (v_cV >= 3000 && v_cV <= 5000); // ignore obviously invalid / early zeros
    // Power (centi) => W*100 = (I_cA * V_cV)/100
    uint32_t P_Wx100 = v_valid ? ((uint32_t)cur_cA_abs * (uint32_t)v_cV + 50)/100 : 0; // rounding only when valid
    if (dt>0 && v_valid){
        // Wh*100 += (W*100 * dt)/3600
        s_tripEnergy_Wh_x100 += (P_Wx100 * dt)/3600UL;
        s_lastPowerOnTime_s = pot_s;
    }
    if (cur_cA_abs > s_tripMaxCurrent_cA) s_tripMaxCurrent_cA = cur_cA_abs;
    if (P_Wx100 > s_tripMaxPower_Wx100) s_tripMaxPower_Wx100 = P_Wx100;
    if (v_valid){
        if (v_cV < s_tripMinVoltage_cV) s_tripMinVoltage_cV = v_cV;
        if (v_cV > s_tripMaxVoltage_cV) s_tripMaxVoltage_cV = v_cV;
    }
}

// ---------- Alt screen renderers ----------
static void render_trip_stats_screen(void){
    u8g2_ClearBuffer(get_u());
    char buf[32];
    ui_settings_t *cfg = settings_ui();
    // Average Wh/km (distance in centi-km from mileageCurrent)
    uint32_t dist_km_x100 = g_S23CB0.mileageCurrent; // (km *100)
    uint16_t avg_whkm_x100 = 0;
    if (dist_km_x100 > 0){
        uint32_t avg = (s_tripEnergy_Wh_x100 * 100UL)/ dist_km_x100; // (Wh/km)*100
        if (avg > 65535UL) avg = 65535UL;
        avg_whkm_x100 = (uint16_t)avg;
    }
    uint16_t av_i = avg_whkm_x100 / 100; uint16_t av_f = avg_whkm_x100 % 100;
    snprintf(buf,sizeof(buf),"AVG:%3u.%02uWh/km", av_i, av_f);
    draw_text(0,0,buf);
    if (cfg->showPower){
        uint32_t w100 = s_tripMaxPower_Wx100; uint16_t w_i = w100/100; uint16_t w_f = w100%100;
        snprintf(buf,sizeof(buf),"Pmax:%4u.%02uW", w_i, w_f);
    } else {
        uint16_t c_i = s_tripMaxCurrent_cA/100; uint16_t c_f = s_tripMaxCurrent_cA%100;
        snprintf(buf,sizeof(buf),"Imax:%3u.%02uA", c_i, c_f);
    }
    draw_text(0,2,buf);
    uint16_t vmin_i = (s_tripMinVoltage_cV==0xFFFF)?0:(s_tripMinVoltage_cV/100); uint16_t vmin_f = (s_tripMinVoltage_cV==0xFFFF)?0:(s_tripMinVoltage_cV%100);
    snprintf(buf,sizeof(buf),"Umin:%2u.%02uV", vmin_i, vmin_f); draw_text(0,4,buf);
    uint16_t vmax_i = s_tripMaxVoltage_cV/100; uint16_t vmax_f = s_tripMaxVoltage_cV%100;
    snprintf(buf,sizeof(buf),"Umax:%2u.%02uV", vmax_i, vmax_f); draw_text(0,5,buf);
    // Energy consumed
    uint32_t e_wh100 = s_tripEnergy_Wh_x100; uint16_t e_i = e_wh100/100; uint16_t e_f = e_wh100%100;
    snprintf(buf,sizeof(buf),"E:%4u.%02uWh", e_i, e_f); draw_text(0,7,buf);
}

static void render_odometer_screen(void){
    u8g2_ClearBuffer(get_u());
    char buf[32];
    // Total mileage (mileageTotal counts 1/1000 km?) replicate Arduino: total/1000 for km, remainder/10 for one decimal
    uint32_t tot = g_S23CB0.mileageTotal; // raw units same as Arduino
    uint32_t km_whole = tot / 1000; uint32_t km_tenths = (tot % 1000)/10;
    snprintf(buf,sizeof(buf),"TOT %4lu.%01luKM", (unsigned long)km_whole, (unsigned long)km_tenths);
    draw_text(0,0,buf);
    // Power-on time (seconds -> mm:ss)
    uint32_t pot = g_S23C3A.powerOnTime; uint32_t mm = pot/60; uint32_t ss = pot%60; if (mm>999) mm=999;
    snprintf(buf,sizeof(buf),"ON  %03lu:%02lu", (unsigned long)mm, (unsigned long)ss);
    draw_text(0,2,buf);
    // Riding time (distinct from power-on)
    uint32_t rt = g_S23C3A.ridingTime; mm = rt/60; ss = rt%60; if (mm>999) mm=999;
    snprintf(buf,sizeof(buf),"RID %03lu:%02lu", (unsigned long)mm, (unsigned long)ss);
    draw_text(0,4,buf);
    // Current trip distance
    uint32_t dist = g_S23CB0.mileageCurrent; uint32_t d_whole = dist/100; uint32_t d_frac = dist%100;
    snprintf(buf,sizeof(buf),"TRP %4lu.%02luKM", (unsigned long)d_whole, (unsigned long)d_frac);
    draw_text(0,6,buf);
}

static void render_temps_screen(void){
    u8g2_ClearBuffer(get_u());
    char buf[24];
    extern volatile bool g_mainTempAFValid; extern volatile int16_t g_mainTempC10_AF; extern volatile bool g_batteryDataValid;
    // Driver/controller temp: prefer AF decoded temp if valid, else fallback to legacy mainframeTemp (already c10)
    int16_t drv_c10 = g_mainTempAFValid ? g_mainTempC10_AF : g_S23CB0.mainframeTemp; // c10
    int16_t tdrv = drv_c10 / 10; if (tdrv < -99) tdrv = -99; if (tdrv > 199) tdrv = 199;
    int16_t t1 = g_batteryDataValid ? g_S25C31.temp1 : 0x7FFF; // sentinel invalid when battery frame missing
    int16_t t2 = g_batteryDataValid ? g_S25C31.temp2 : 0x7FFF;
    if (t1 != 0x7FFF && (t1 < -99 || t1 > 199)) t1 = (t1 < -99)? -99 : 199;
    if (t2 != 0x7FFF && (t2 < -99 || t2 > 199)) t2 = (t2 < -99)? -99 : 199;
    snprintf(buf,sizeof(buf),"DRV:%3dC", tdrv); draw_text(0,0,buf);
    if (t1 == 0x7FFF) draw_text(0,2,"B1 : --C"); else { snprintf(buf,sizeof(buf),"B1 :%3dC", t1); draw_text(0,2,buf); }
    if (t2 == 0x7FFF) draw_text(0,3,"B2 : --C"); else { snprintf(buf,sizeof(buf),"B2 :%3dC", t2); draw_text(0,3,buf); }
    // Show voltage + current/power snapshot at bottom
    int16_t cur_cA = protocol_total_current_cA(); int curAbs = cur_cA<0?-cur_cA:cur_cA; int cur_i = curAbs/100; int cur_f = curAbs%100;
    uint16_t v_cV = (uint16_t)(g_S25C31.voltage<0?-g_S25C31.voltage:g_S25C31.voltage); uint16_t v_i = v_cV/100; uint16_t v_f = v_cV%100;
    snprintf(buf,sizeof(buf),"%2u.%02uV %2d.%02dA", v_i, v_f, cur_i, cur_f); draw_text(0,5,buf);
#if CFG_AHT10_ENABLE
    extern bool g_ahtPresent; extern float g_ahtTempC;
    if(g_ahtPresent && !isnan(g_ahtTempC)){
        int amb=(int)lrintf(g_ahtTempC);
        if(amb<-99) amb=-99; if(amb>199) amb=199;
        snprintf(buf,sizeof(buf),"AMB:%3dC", amb); draw_text(0,7,buf);
    } else {
        draw_text(0,7,"AMB: --C");
    }
#endif
}

// ---------- Input handling for screen cycling ----------
static void handle_screen_cycle(int speedRaw){
    // Treat speed below cutoff as stationary (allow slightly higher for smoother bench tests)
#ifndef STATIONARY_SPEED_CUTOFF
#define STATIONARY_SPEED_CUTOFF 200  // was 800 (≈0.8 km/h). 1200 gives a little more tolerance.
#endif
    int absSpeed = speedRaw < 0 ? -speedRaw : speedRaw;
    bool stationary = absSpeed <= STATIONARY_SPEED_CUTOFF;

    // Lower thresholds for easier activation.
#ifndef THROTTLE_ON_THRESHOLD
#define THROTTLE_ON_THRESHOLD  55
#endif
#ifndef THROTTLE_OFF_THRESHOLD
#define THROTTLE_OFF_THRESHOLD 30
#endif
#ifndef BRAKE_ON_THRESHOLD
#define BRAKE_ON_THRESHOLD     45
#endif
#ifndef BRAKE_OFF_THRESHOLD
#define BRAKE_OFF_THRESHOLD    25
#endif

    int throttle = g_S20C00HZ65.throttle;
    int brake    = g_S20C00HZ65.brake;

    // Debounce + hysteresis mapping:
    // Raw thresholds decide when a press/release candidate starts. We then require
    // the raw state to be stable for the debounce interval before committing the
    // logical pressed/released state used for navigation.
#ifndef THROTTLE_PRESS_DEBOUNCE_MS
#define THROTTLE_PRESS_DEBOUNCE_MS   40   // reduced from 100 to catch quick taps
#endif
#ifndef THROTTLE_RELEASE_DEBOUNCE_MS
#define THROTTLE_RELEASE_DEBOUNCE_MS 250  // reduced from 600 to allow faster re-press
#endif
#ifndef BRAKE_PRESS_DEBOUNCE_MS
#define BRAKE_PRESS_DEBOUNCE_MS      40
#endif
#ifndef BRAKE_RELEASE_DEBOUNCE_MS
#define BRAKE_RELEASE_DEBOUNCE_MS    60
#endif

    uint32_t nowMs = millis();

    // State machines
    // thrState/brkState: 0=released, 1=pressed
    static uint8_t thrState = 0, brkState = 0;
    static uint32_t thrDebStart = 0, brkDebStart = 0;

    bool thrRawPressed   = (throttle >= THROTTLE_ON_THRESHOLD);
    bool thrRawReleased  = (throttle <= THROTTLE_OFF_THRESHOLD);
    bool brkRawPressed   = (brake    >= BRAKE_ON_THRESHOLD);
    bool brkRawReleased  = (brake    <= BRAKE_OFF_THRESHOLD);

    // Throttle debounce
    int throttleVal; // final tri-state (-1 released, 0 neutral/debouncing, 1 pressed)
    if (thrState == 0){ // currently released
        if (thrRawPressed){
#if UI_INPUT_FAST_TAP_EDGE
            // Immediate edge on threshold crossing (minimal filtering)
            thrState = 1; thrDebStart = 0; throttleVal = 1;
#else
            if (thrDebStart == 0) thrDebStart = nowMs;
            if (nowMs - thrDebStart >= THROTTLE_PRESS_DEBOUNCE_MS){ thrState = 1; thrDebStart = 0; throttleVal = 1; }
            else throttleVal = 0; // debouncing
#endif
        } else { thrDebStart = 0; throttleVal = -1; }
    } else { // currently pressed
        if (thrRawReleased){
            if (thrDebStart == 0) thrDebStart = nowMs;
            if (nowMs - thrDebStart >= THROTTLE_RELEASE_DEBOUNCE_MS){ thrState = 0; thrDebStart = 0; throttleVal = -1; }
            else throttleVal = 0; // debouncing release
        } else { thrDebStart = 0; throttleVal = 1; }
    }

    // Raw pulse diagnostics for missed quick taps (only when not committed as logical press)
#if UI_INPUT_DEBUG && UI_INPUT_DEBUG_SERIAL
    static uint32_t thrPulseStart = 0; static int thrPulsePeak = 0;
    if (thrPulseStart == 0 && thrRawPressed) { thrPulseStart = nowMs; thrPulsePeak = throttle; }
    if (thrPulseStart != 0){ if (throttle > thrPulsePeak) thrPulsePeak = throttle; }
    // If raw released and we never transitioned logical state to pressed (thrState still 0 before reset)
    if (thrPulseStart != 0 && thrRawReleased && thrState == 0){
        uint32_t dur = nowMs - thrPulseStart;
        if (dur < THROTTLE_PRESS_DEBOUNCE_MS){
            printf("[UIIN] throttle short pulse ignored dur=%lu ms peak=%d onThr=%d offThr=%d\n",
                   (unsigned long)dur, thrPulsePeak, THROTTLE_ON_THRESHOLD, THROTTLE_OFF_THRESHOLD);
        }
        thrPulseStart = 0; thrPulsePeak = 0;
    }
    // Reset tracker once we commit logical press to avoid false logging
    if (throttleVal == 1 && thrPulseStart != 0){ thrPulseStart = 0; thrPulsePeak = 0; }
#endif

    // Brake debounce
    int brakeVal;
    if (brkState == 0){
        if (brkRawPressed){
            if (brkDebStart == 0) brkDebStart = nowMs;
            if (nowMs - brkDebStart >= BRAKE_PRESS_DEBOUNCE_MS){ brkState = 1; brkDebStart = 0; }
            brakeVal = 0;
        } else {
            brkDebStart = 0; brakeVal = -1;
        }
    } else {
        if (brkRawReleased){
            if (brkDebStart == 0) brkDebStart = nowMs;
            if (nowMs - brkDebStart >= BRAKE_RELEASE_DEBOUNCE_MS){ brkState = 0; brkDebStart = 0; brakeVal = -1; }
            else brakeVal = 0;
        } else { brkDebStart = 0; brakeVal = 1; }
    }

    // Single-action per press: disable hold-repeat. Keep cooldown to suppress accidental double edges.
    const uint32_t COOLDOWN_MS = 180;     // min gap between accepted edges
    static uint32_t lastScreenSwitchMs = 0;

    // Brake active duration tracking: start when logical brake goes to 1, finalize when it returns to released (-1).
    if (brakeVal == 1){
        if (s_brakePressStartMs == 0) s_brakePressStartMs = nowMs; // new press
    } else if (brakeVal == -1){
        if (s_brakePressStartMs != 0){
            s_brakeLastDurationMs = nowMs - s_brakePressStartMs;
            s_brakePressStartMs = 0;
        }
    }

    // Throttle active duration tracking + event messages.
    if (throttleVal == 1) {
        if (s_throttlePressStartMs == 0) {
            s_throttlePressStartMs = nowMs;
#if UI_INPUT_DEBUG && UI_INPUT_DEBUG_SERIAL
            printf("throttle activated\n");
#endif
        }
    } else if (throttleVal == -1) {
        if (s_throttlePressStartMs != 0) {
            s_throttleLastDurationMs = nowMs - s_throttlePressStartMs;
            // duration captured in s_throttleLastDurationMs
            s_throttlePressStartMs = 0;
#if UI_INPUT_DEBUG && UI_INPUT_DEBUG_SERIAL
            // Report in whole seconds (round to nearest) using last throttle press duration.
            uint32_t thrDurMs = s_throttleLastDurationMs;
            uint32_t secs = (thrDurMs + 500U) / 1000U; // nearest second
            if (secs == 0U) secs = 1U; // minimum 1s if it was very brief but debounced
            printf("throttle deactivated - was pressed for %lu seconds\n", (unsigned long)secs);
#endif
        }
    }

#if UI_INPUT_DEBUG
    dbg_throttle = throttle; dbg_brake = brake; dbg_speed = speedRaw; dbg_thVal = throttleVal; dbg_brVal = brakeVal;
#endif

    // Require continuous stationary period before allowing screen cycling to avoid accidental changes while moving.
    static uint32_t s_stationarySince = 0;
    if (stationary) {
        if (s_stationarySince == 0) s_stationarySince = nowMs; // mark start of stationary window
    } else {
        s_stationarySince = 0; // reset when movement resumes
    }
    const uint32_t STATIONARY_HOLD_MS = 3000; // 3 seconds requirement
    bool allowScreenCycle = (s_stationarySince != 0) && (nowMs - s_stationarySince >= STATIONARY_HOLD_MS);

    if (stationary && allowScreenCycle){
    bool thEdge = (throttleVal == 1 && s_oldThrottleVal != 1 && brakeVal <= 0);
    bool brEdge = (brakeVal    == 1 && s_oldBrakeVal    != 1 && throttleVal <= 0);
        if (s_inputWarmup) { thEdge = false; brEdge = false; s_inputWarmup = false; }
        if (thEdge || brEdge){ s_bigActive = false; s_bigReleaseAt = 0; }
        // Combo gesture: simultaneous throttle & brake -> return to main screen instantly
        if (throttleVal==1 && brakeVal==1){ s_uiAltScreen = 0; lastScreenSwitchMs = nowMs; }
        else {
            if (nowMs - lastScreenSwitchMs >= COOLDOWN_MS){
                if (thEdge){ uint8_t maxScreens = 4; s_uiAltScreen = (uint8_t)((s_uiAltScreen + 1) % maxScreens); lastScreenSwitchMs = nowMs; }
                else if (brEdge){ uint8_t maxScreens = 4; s_uiAltScreen = (uint8_t)((s_uiAltScreen == 0) ? (maxScreens - 1) : (s_uiAltScreen - 1)); lastScreenSwitchMs = nowMs; }
            }
        }
    }

    s_oldThrottleVal = throttleVal; s_oldBrakeVal = brakeVal;

}

// -----------------------------------------------------------------------------
// Boot animation: simple Pac-Man eating dots across the screen for ~2 seconds
// -----------------------------------------------------------------------------
static bool s_bootDone = false;
static uint32_t s_bootStartMs = 0;            // ms-based reference (millis)
static float   s_pacmanPosX = -16.f;          // fractional X for smooth sub-pixel motion
static uint32_t s_bootLastFrameMs = 0;        // last frame (ms) for dt logic (legacy assist)
static uint64_t s_bootNextFrameUs = 0;        // high-res scheduling target
static uint64_t s_bootFrameIntervalUs = 0;    // frame period in us
static const uint32_t BOOT_ANIM_MS = 2200;    // total duration
#ifndef DISPLAY_TARGET_FRAME_MS
#define DISPLAY_TARGET_FRAME_MS 40  // default ~25 FPS after boot
#endif
// Target higher FPS for smoother boot (50 FPS ~20ms). Real pacing adapts to render cost.
#ifndef BOOT_TARGET_FPS
#define BOOT_TARGET_FPS 50
#endif
// Optionally allow faster experimental FPS if I2C can keep up (define BOOT_TARGET_FPS=60 at compile time)
static const uint32_t BOOT_IDEAL_FRAME_MS = (1000/BOOT_TARGET_FPS);

// 16x16 Pac-Man bitmaps (open / closed mouth), 1bpp row-major, left-to-right
// Procedurally render Pac-Man instead of static (broken) bitmaps.
// We draw a 16x16 disc and carve out a wedge for the open mouth frame.
static void draw_pacman(int x, int y, float mouthAngleRad){
    // mouthAngleRad 0 (closed) .. ~0.75 (open ~43 deg)
    if (mouthAngleRad < 0) {
        mouthAngleRad = 0;
    }
    if (mouthAngleRad > 0.9f) {
        mouthAngleRad = 0.9f;
    }
    u8g2_t *u = get_u();
    const int r2 = 8*8;
    for(int yy=0; yy<16; ++yy){
        for(int xx=0; xx<16; ++xx){
            int dx = xx-8; int dy = yy-8; int d2 = dx*dx + dy*dy;
            if (d2 > r2-2) continue; // keep a crisper edge
            if (dx >= 0 && mouthAngleRad > 0){
                float ang = atan2f((float)dy, (float)dx);
                if (fabsf(ang) < mouthAngleRad) continue; // inside mouth wedge
            }
            u8g2_DrawPixel(u, x+xx, y+yy);
        }
    }
    // Eye (fixed small 2x2 square)
    u8g2_DrawPixel(u, x+10, y+4);
    u8g2_DrawPixel(u, x+11, y+4);
    u8g2_DrawPixel(u, x+10, y+5);
    u8g2_DrawPixel(u, x+11, y+5);
}

// Optional: enable partial update optimization (reduces I2C traffic during boot)
#ifndef BOOT_PARTIAL_UPDATES
#define BOOT_PARTIAL_UPDATES 1
#endif

static void render_boot_pacman(){
    uint32_t now = millis();
    if (!s_bootStartMs){ s_bootStartMs = now; s_bootLastFrameMs = now; }
    uint32_t elapsed = now - s_bootStartMs;
    if (elapsed >= BOOT_ANIM_MS){ s_bootDone = true; return; }

    // Delta time for smooth motion irrespective of frame jitter
    uint32_t dt = now - s_bootLastFrameMs; if (dt > 100) dt = 100; // clamp (suspend / break)
    s_bootLastFrameMs = now;

    // Horizontal speed: traverse screen width + sprite width over full animation time
    const float totalDistance = 128.f + 16.f; // start off-screen left to off-screen right
    const float speedPxPerMs = totalDistance / (float)BOOT_ANIM_MS; // ~0.065 px/ms
    s_pacmanPosX += speedPxPerMs * (float)dt;  // accumulate fractional
    int x = (int)(s_pacmanPosX) - 16;          // convert to draw coordinate

    // Smooth sinusoidal mouth (avoid abrupt triangle corners)
    const float mouthMax = 0.78f; // rad ~45 deg
    const uint32_t MOUTH_PERIOD = 260; // ms (slightly faster for liveliness)
    float mouthPhase = (float)(elapsed % MOUTH_PERIOD) / (float)MOUTH_PERIOD; // 0..1
    float mouthAngle = mouthMax * 0.5f * (1.f + sinf(mouthPhase * 2.f * (float)M_PI)); // 0..mouthMax sine

    u8g2_t *uDrv = get_u();
#if !BOOT_PARTIAL_UPDATES
    u8g2_ClearBuffer(uDrv);
#endif
    // Dots row constants
    const int lineY = 16;
    const int dotStart = 8; const int dotEnd = 120; const int dotSpacing = 10;
    const int eatThreshold = x + 12; // eaten boundary

#if BOOT_PARTIAL_UPDATES
    // Track last X to compute dirty rectangle
    static int lastX = -1000; int minX = 128, maxX = -1;
    // If we jumped backwards (unlikely) fallback to full clear
    bool full = false;
    if (lastX > x + 4) { full = true; }
    if (full){
        u8g2_ClearBuffer(uDrv);
        lastX = -1000;
    } else {
        // Erase only trailing Pac-Man area and next forward area where mouth changed
    // (removed unused prevRight/curRight)
        if (lastX != -1000){
            // Clear previous sprite zone
            int clearX = lastX; if (clearX < 0) clearX = 0; if (clearX < minX) minX = clearX; int clearW = 16; if (clearX+clearW>127) clearW = 127-clearX; if (clearW>0) { u8g2_SetDrawColor(uDrv,0); u8g2_DrawBox(uDrv, clearX, lineY, clearW, 16); u8g2_SetDrawColor(uDrv,1); if (clearX+clearW-1>maxX) maxX=clearX+clearW-1; }
        }
        // Clear new sprite zone (we'll redraw fresh)
        int newX = x; int newW = 16; if (newX<0){ newW += newX; newX=0; }
        if (newX+newW>127) newW = 127-newX;
        if (newW>0){ u8g2_SetDrawColor(uDrv,0); u8g2_DrawBox(uDrv,newX,lineY,newW,16); u8g2_SetDrawColor(uDrv,1); if (newX<minX) minX=newX; if (newX+newW-1>maxX) maxX=newX+newW-1; }
        // Dots: only draw those not eaten (right side)
        for (int dx = dotStart; dx <= dotEnd; dx += dotSpacing){
            if (dx < eatThreshold) continue; // eaten
            // ensure drawn dot area flagged dirty
            if (dx < minX) minX = dx;
            if (dx+1 > maxX) maxX = dx+1;
            u8g2_DrawBox(uDrv, dx, lineY + 7, 2, 2);
        }
        lastX = x;
        // Draw Pac-Man sprite
        if (x > -20 && x < 140){
            draw_pacman(x, lineY, mouthAngle);
            if (x < minX) minX = x;
            if (x+15 > maxX) maxX = x+15;
        }
        // Draw static text only once (first frame) or if we cleared fully
        if (elapsed < 60 || full){
            sys57_draw(uDrv,6,48,"M365 DASH");
            sys57_draw(uDrv,32,56,"BOOTING");
            if (6*6 < minX) minX = 6*6;
            if ((32*6 + 6*7) > maxX) maxX = 32*6 + 6*7;
        }
        // Fade/dither not applied in partial path for simplicity (avoid widening dirty area)
    if (minX < 0) minX = 0;
    if (maxX > 127) maxX = 127;
        if (maxX >= minX){
            // Send only modified horizontal band lines (lineY..lineY+16 plus text lines if drawn)
            // u8g2 library lacks native partial flush for I2C full-buffer mode; mimic by copying a sub-rectangle into temp buffer and clearing others.
            // Simpler: still send full buffer if sub-range >70% width; else convert to selective line pushes unsupported -> fallback full.
            int width = maxX - minX + 1;
            if (width > 90){
                u8g2_SendBuffer(uDrv); // large area, just flush full
            } else {
                // Narrow optimization: temporarily mask outside region to reduce I2C (still full send, but we could compress if custom driver)
                // (Future enhancement: custom u8x8 byte callback to skip pages outside region.)
                u8g2_SendBuffer(uDrv); // placeholder until custom partial driver added
            }
        } else {
            u8g2_SendBuffer(uDrv); // safety
        }
        return;
    }
#endif // BOOT_PARTIAL_UPDATES
    // Full redraw fallback path
    u8g2_ClearBuffer(uDrv);
    for (int dx = dotStart; dx <= dotEnd; dx += dotSpacing){ if (dx < eatThreshold) continue; u8g2_DrawBox(uDrv, dx, lineY + 7, 2, 2);}    
    if (x > -20 && x < 140) draw_pacman(x, lineY, mouthAngle);
    sys57_draw(uDrv,6,48,"M365 DASH");
    float fade = elapsed / (float)BOOT_ANIM_MS; if (fade>1.f) fade=1.f; if (fade > 0.85f || ((int)(elapsed/70)%2)==0) sys57_draw(uDrv,32,56,"BOOTING");
    u8g2_SendBuffer(uDrv);
}

static void display_task(void *arg){
    uint32_t lastFrameStart = 0; uint32_t frameCount=0; uint32_t lastFpsLog=0; uint32_t dynDelay = DISPLAY_TARGET_FRAME_MS;
    while(1){
        uint32_t nowStart = millis();
        // Boot animation gate
        if (!s_bootDone){
            // High resolution pacing using esp_timer (microseconds) for smoother animation.
            uint64_t nowUs = esp_timer_get_time();
            if (s_bootNextFrameUs == 0){
                s_bootFrameIntervalUs = (uint64_t)BOOT_IDEAL_FRAME_MS * 1000ULL;
                s_bootNextFrameUs = nowUs;
            }
            // Render only when due (allow slight catch-up if lagging by >1 frame)
            if (nowUs >= s_bootNextFrameUs){
                uint64_t frameStartUs = nowUs;
                render_boot_pacman();
                if (s_bootDone) {
                    // Reset pacing vars for main loop
                    s_bootNextFrameUs = 0;
                } else {
                    uint64_t renderCostUs = esp_timer_get_time() - frameStartUs;
                    // Simple adaptive: If render cost > 75% of frame budget, relax target a bit.
                    if (renderCostUs > (s_bootFrameIntervalUs * 3)/4 && s_bootFrameIntervalUs < 26000){
                        s_bootFrameIntervalUs += 1000; // lengthen period ~+1ms
                    } else if (renderCostUs < (s_bootFrameIntervalUs/3) && s_bootFrameIntervalUs > 14000){
                        s_bootFrameIntervalUs -= 500; // tighten period slightly
                    }
                    s_bootNextFrameUs += s_bootFrameIntervalUs;
                    // If we fell far behind ( > 2 frames ) fast-forward to avoid burst rendering
                    uint64_t behind = nowUs - s_bootNextFrameUs;
                    if ((int64_t)behind > (int64_t)(2*s_bootFrameIntervalUs)){
                        s_bootNextFrameUs = nowUs + s_bootFrameIntervalUs; // resync
                    }
                }
            }
            // Sleep until next due frame (convert to ticks) but cap to reasonable slice
            if (!s_bootDone){
                uint64_t now2 = esp_timer_get_time();
                if (s_bootNextFrameUs > now2){
                    uint64_t waitUs = s_bootNextFrameUs - now2;
                    // Convert to FreeRTOS ticks; ensure at least 1 tick delay to yield
                    uint32_t waitMs = (uint32_t)(waitUs / 1000ULL);
                    if (waitMs == 0) waitMs = 1;
                    vTaskDelay(pdMS_TO_TICKS(waitMs));
                } else {
                    taskYIELD();
                }
                continue; // keep boot loop separate
            }
        }
        int speedRaw = g_S23CB0.speed; if (speedRaw < -10000) speedRaw = speedRaw + 32768 + 32767; // mimic original wrap fix
    update_big_state(speedRaw);
        // Accumulate trip metrics every frame (cheap math)
        accumulate_trip_metrics();
    // Range estimator tick
    range_tick();
        // Handle user input for screen cycling when not auto-big
        handle_screen_cycle(speedRaw);
        if (s_bigActive){
            render_big_screen(speedRaw);
        } else {
            switch(s_uiAltScreen){
                case 1: render_trip_stats_screen(); break;
                case 2: render_odometer_screen(); break;
                case 3: render_temps_screen(); break;
                default: render_main_screen(speedRaw); break;
            }
        }
    // TODO: Add menu/settings state machine & WiFi/OTA screens.
        // Overlays
    extern volatile bool g_batteryDataValid;
    ui_settings_t *cfg = settings_ui();
    if (g_batteryDataValid && cfg->bigWarn && cfg->warnBatteryPercent>0 && g_S25C31.remainPercent <= cfg->warnBatteryPercent && (millis()%2000<700)){
            render_low_batt_overlay(g_S25C31.remainPercent);
        }
        render_bus_overlay();
#if UI_INPUT_DEBUG && UI_INPUT_DEBUG_SERIAL
        {
            static uint32_t lastLog=0; uint32_t nowDbg = millis();
            if (nowDbg - lastLog > 500) {
                lastLog = nowDbg;
                // Log raw and interpreted values
          uint32_t brActMs = s_brakePressStartMs ? (nowDbg - s_brakePressStartMs) : 0;
          uint32_t thrActMs = s_throttlePressStartMs ? (nowDbg - s_throttlePressStartMs) : 0;
          printf("[UIIN] thr=%d thrVal=%d br=%d brVal=%d thrAct=%lu thrLast=%lu brAct=%lu brLast=%lu speed=%d scr=%u\n",
              dbg_throttle, dbg_thVal, dbg_brake, dbg_brVal,
              (unsigned long)thrActMs, (unsigned long)s_throttleLastDurationMs,
              (unsigned long)brActMs, (unsigned long)s_brakeLastDurationMs,
              dbg_speed, (unsigned)s_uiAltScreen);
            }
        }
#endif
#if UI_INPUT_DEBUG && UI_INPUT_DEBUG_DRAW
    // (Disabled by default) On-screen debug overlay
    char dbg[40];
    snprintf(dbg,sizeof(dbg),"T%03d(%d) B%03d(%d) S%05d Scr%u", dbg_throttle, dbg_thVal, dbg_brake, dbg_brVal, dbg_speed, (unsigned)s_uiAltScreen);
    sys57_draw(get_u(), 0, 48, dbg);
#endif
    u8g2_SendBuffer(get_u());
    // Post-boot frame pacing
        // Simple adaptive pacing: aim for ~dynDelay but if render+I2C cost exceeds, skip extra delay
        uint32_t frameDur = millis() - nowStart;
        if (frameDur < dynDelay){
            vTaskDelay(pdMS_TO_TICKS(dynDelay - frameDur));
        }
        // Periodic FPS log & adjust (every 2s)
        frameCount++;
        uint32_t now = millis();
        if (now - lastFpsLog > 2000){
            uint32_t elapsed = (lastFrameStart==0)?2000:(now - lastFrameStart);
            float fps = (elapsed>0)? (1000.0f * frameCount / elapsed) : 0.f;
            // If we are comfortably below 18ms per frame at 40ms target, speed up a bit
            if (fps > 30.f && dynDelay > 25) dynDelay -= 2; // tighten delay gradually
            // If we fall below 15 FPS, loosen delay (though likely I2C bound)
            if (fps < 15.f && dynDelay < 60) dynDelay += 5;
            // (Optional) could add ESP_LOGI here; omitted to avoid log spam in release.
            lastFpsLog = now; frameCount=0; lastFrameStart = now;
        }
    }
}

void display_port_start(void){
    xTaskCreatePinnedToCore(display_task, "disp", 4096, NULL, 4, NULL, 0);
}
#endif // SIMPLE_DISPLAY_FALLBACK