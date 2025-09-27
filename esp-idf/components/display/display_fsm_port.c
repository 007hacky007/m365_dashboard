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

// Suppress noisy format-truncation warnings for constrained UI formatting
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

// Integrate legacy Arduino numeric font (stdNumb) for nicer large digits (definition in fonts_stdnum.c).
#include "fonts_stdnum.h"

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
static const font_stdnum_t g_stdnum = { stdNumb, 10, 14, ' ', 27 };
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

// Reusable width helper for std numeric font
static int stdnum_text_w(const char *s){ int n=0; while(*s){ ++n; ++s; } return n? n*(g_stdnum.w+1)-1 : 0; }

// Draw a large 'C' roughly matching std numeric font height/weight
static void draw_big_C(u8g2_t *u, int x, int y){
    int h = g_stdnum.h; // typically 14
    int thick = 2;      // stroke thickness
    int innerW = g_stdnum.w - 2; if (innerW < 6) innerW = 6; // approximate width
    // Top bar
    for(int dy=0; dy<thick; ++dy){ u8g2_DrawHLine(u, x+1, y+dy, innerW-1); }
    // Bottom bar
    for(int dy=0; dy<thick; ++dy){ u8g2_DrawHLine(u, x+1, y + h-1 - dy, innerW-1); }
    // Left vertical
    for(int dx=0; dx<thick; ++dx){ u8g2_DrawVLine(u, x+dx, y+thick-1, h - 2*thick +2); }
    // (Optional) small corner anti-alias pixels (leave open right side)
}

// Draw a small lowercase unit string (e.g. "km") preserving lowercase using a u8g2 font with lowercase glyphs.
static u8g2_t *get_u(); // forward declaration
static void draw_lower_unit(int x, int baselineY, const char *s){
    u8g2_t *u = get_u();
    // Choose a tiny readable font (5x8) that has lowercase
    u8g2_SetFont(u, u8g2_font_5x8_tr);
    u8g2_DrawStr(u, x, baselineY, s);
    // No restore needed; std numeric drawing sets pixels directly.
}
// Clamp temperature (C or F) to displayable range; 0x7FFF means missing
static inline int16_t clamp_temp_c(int16_t v){ if (v==0x7FFF) return v; if (v<-99) return -99; if (v>199) return 199; return v; }
// Draw temperature (signed) with leading space for -9..9 using std numeric font
static void draw_temp_num(u8g2_t *u,int x,int y,int16_t v){
    char buf[12];
    if (v==0x7FFF) {
        strcpy(buf,"--");
    } else {
        int single = (v>-10 && v<10);
        // Max absolute value after clamp is 199 so buffer of 8 is plenty.
        if (single) snprintf(buf,sizeof(buf)," %d", (int)v); else snprintf(buf,sizeof(buf),"%d", (int)v);
    }
    stdnum_draw_string(u,x,y,buf);
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
    u8g2_t *u = get_u();
    u8g2_ClearBuffer(u);
    // --- Large speed ---
    int speedAbs = speedRaw < 0 ? -speedRaw : speedRaw; // raw in 1/1000 km/h (?) adapt as used elsewhere
    int sp_whole = speedAbs / 1000; if (sp_whole > 99) sp_whole = 99; int sp_tenths = (speedAbs % 1000)/100;
    char line[16];
    snprintf(line,sizeof(line),"%2d.%d", sp_whole, sp_tenths);
    stdnum_draw_string(u,0,0,line);
    int right = stdnum_text_w(line);
    sys57_draw(u, right+2, 0+7, "KMH");
    // --- Battery percent (top-right) ---
    int pct = get_filtered_percent();
    char bbuf[16];
    if (pct>=0){
        if (pct<0) pct=0;
        if (pct>100) pct=100; // clamp defensively
        // Format with leading spaces to keep width stable (up to 100%)
        snprintf(bbuf,sizeof(bbuf),"%3u%%", (unsigned)pct);
    } else {
        strcpy(bbuf," --%");
    }
    int bLen = (int)strlen(bbuf); sys57_draw(u,128 - bLen*6, 0, bbuf);
    // --- Voltage (row 2) ---
    int vAbs = g_S25C31.voltage < 0 ? -g_S25C31.voltage : g_S25C31.voltage; bool vValid = (vAbs >= 2000 && vAbs <= 5000);
    if (vValid){ int vw=vAbs/100; int vf=vAbs%100; snprintf(line,sizeof(line),"%2d.%02dV",vw,vf);} else strcpy(line,"--.--V");
    sys57_draw(u, 0, 2*8+1, line);
    // --- Driver temp (row 3) ---
    extern volatile bool g_mainTempAFValid; extern volatile int16_t g_mainTempC10_AF;
    int16_t drv_c10 = g_mainTempAFValid ? g_mainTempC10_AF : g_S23CB0.mainframeTemp; int16_t tdrv = drv_c10/10; tdrv = clamp_temp_c(tdrv);
    char tbuf[8]; if (tdrv==0x7FFF) strcpy(tbuf,"--"); else snprintf(tbuf,sizeof(tbuf),"%d", (int)tdrv);
    stdnum_draw_string(u,0,3*8,tbuf); sys57_draw(u,stdnum_text_w(tbuf)+2,3*8+7,"C");
    // --- Current / Power (row 4) ---
    ui_settings_t *cfg = settings_ui(); int16_t c_cA = protocol_total_current_cA(); int cAbs = c_cA<0?-c_cA:c_cA; int ch=cAbs/100; int cf=cAbs%100;
    if (!cfg->showPower) snprintf(line,sizeof(line),"%2d.%02dA", ch, cf); else { int W=(int)(((float)c_cA/100.0f)*((float)g_S25C31.voltage/100.0f)); if (W<0) W=-W; if (W>9999) W=9999; snprintf(line,sizeof(line),"%4dW", W);} 
    stdnum_draw_string(u,0,4*8,line);
    // --- Battery bar bottom ---
    if (pct>=0){
        int barW=120; int bx=(128-barW)/2; int by=56; int fill=(pct*(barW-4))/100;
        u8g2_DrawFrame(u,bx,by,barW,8); u8g2_DrawBox(u,bx+2,by+1,fill,6);
    }
}

static void render_main_screen(int speedRaw){
    u8g2_t *u = get_u();
    u8g2_ClearBuffer(u);
    char line[32];
    ui_settings_t *cfg = settings_ui();
    int speedAbs = speedRaw < 0 ? -speedRaw : speedRaw;
    int sp_whole = speedAbs / 1000; if (sp_whole > 99) sp_whole = 99; int sp_tenths = (speedAbs % 1000) / 100;
    int vAbs = g_S25C31.voltage < 0 ? -g_S25C31.voltage : g_S25C31.voltage; bool voltageValid=(vAbs>=2000 && vAbs<=5000);
    // Primary (left top) either speed or voltage
    if (cfg->showVoltageMain){
        if (voltageValid){ int vw=vAbs/100; int vf=vAbs%100; snprintf(line,sizeof(line),"%d.%02d", vw, vf); }
        else strcpy(line,"00.00");
    } else {
        snprintf(line,sizeof(line),"%c%d.%01d", (sp_whole<10?' ':(sp_whole/10)+'0'), sp_whole%10, sp_tenths);
        if (speedAbs < 50) strcpy(line," 00.0");
    }
    stdnum_draw_string(u,0,0,line);
    int primaryRight = stdnum_text_w(line);
    sys57_draw(u, primaryRight+2, 0+7, cfg->showVoltageMain?"V":"KMH");

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
    int tW=stdnum_text_w(tbuf); int tX=128 - tW - 14; if (tX < primaryRight + 8) tX = primaryRight + 8; stdnum_draw_string(u, tX, 0, tbuf);
    // superscript degree + large C (same height as digits)
    int unitBaseX = tX + tW + 2; int unitBaseY = 0;
    u8g2_DrawCircle(u, unitBaseX + 2, unitBaseY + 2, 2, U8G2_DRAW_ALL); // degree symbol
    draw_big_C(u, unitBaseX + 6, unitBaseY); // big 'C'

    // Distance large left (row 2 top=16) fixed 0.00 format (cap 99.99) with leading space for <10 to shift one digit right
    extern volatile uint32_t g_afTripDistance_m;
    uint32_t mCurrRaw = g_S23CB0.mileageCurrent; // 0.01 km units
    if (g_afTripDistance_m > 0 && g_afTripDistance_m < 1000000UL) mCurrRaw = g_afTripDistance_m / 10U;
    uint32_t kmInt = mCurrRaw / 100U; uint32_t hundredths = mCurrRaw % 100U; if (kmInt > 99) kmInt = 99;
    char distbuf[12];
    if (kmInt < 10) snprintf(distbuf,sizeof(distbuf)," %1lu.%02lu", (unsigned long)kmInt, (unsigned long)hundredths); else snprintf(distbuf,sizeof(distbuf),"%02lu.%02lu", (unsigned long)kmInt, (unsigned long)hundredths);
    stdnum_draw_string(u,0,16,distbuf);
    int distW = stdnum_text_w(distbuf);
    int unitX = distW + 2; // small gap after digits
    int unitBaseline = 16 + g_stdnum.h - 1; // align bottom (subscript style)
    draw_lower_unit(unitX, unitBaseline, "km");

    // Ride time large left (row 4 top=32)
    uint32_t ride_s = g_S23C3A.ridingTime;
    if (ride_s >= 3600) { uint32_t hh = ride_s / 3600U; if (hh > 99) hh = 99; uint32_t mm = (ride_s / 60U) % 60U; snprintf(line,sizeof(line),"%02lu:%02lu",(unsigned long)hh,(unsigned long)mm); }
    else { uint32_t mm = ride_s / 60U; if (mm > 99) mm = 99; uint32_t ss = ride_s % 60U; snprintf(line,sizeof(line),"%02lu:%02lu",(unsigned long)mm,(unsigned long)ss); }
    stdnum_draw_string(u,0,32,line);

    // Current/power large right with baseline unit (small font) right-aligned
    int16_t c_cA=protocol_total_current_cA(); int cAbs=c_cA<0?-c_cA:c_cA; int cur_h=cAbs/100; int cur_l=cAbs%100;
    char unitBuf[2]; unitBuf[1]='\0';
    if(!cfg->showPower){
        snprintf(line,sizeof(line),"%2d.%02d",cur_h,cur_l); unitBuf[0]='A';
    } else {
        int Wp=(int)(((float)c_cA/100.0f)*((float)g_S25C31.voltage/100.0f)); if(Wp<0)Wp=-Wp; if(Wp>9999)Wp=9999; snprintf(line,sizeof(line),"%4d",Wp); unitBuf[0]='W';
    }
    int pw=stdnum_text_w(line);
    // Measure unit width using u8g2 font 5x8
    u8g2_SetFont(u, u8g2_font_5x8_tr);
    int unitW = u8g2_GetStrWidth(u, unitBuf);
    int gap = 2;
    int totalW = pw + gap + unitW;
    int px = 128 - totalW - 1; if (px < 0) px = 0;
    stdnum_draw_string(u, px,32,line);
    int baselineCurr = 32 + g_stdnum.h - 1;
    draw_lower_unit(px + pw + gap, baselineCurr, unitBuf);

    // Range small (row 6)
    float rem_km=range_get_estimate_km(); if(rem_km<0)rem_km=0; if(rem_km>999.9f) rem_km=999.9f; uint16_t r_int=(uint16_t)rem_km; uint16_t r_dec=(uint16_t)((rem_km-(float)r_int)*10.0f+0.5f); snprintf(line,sizeof(line),"%3u.%1u",(unsigned)r_int,(unsigned)r_dec);
    int smallW=strlen(line)*6; int startX=128 - smallW - 12; if(startX<0) startX=0; sys57_draw(u, startX, 6*8+1, line); draw_lower_unit(startX+smallW+2, 6*8+1+7, "km");

    // Battery bar bottom + percent small
    int pct = get_filtered_percent(); int percent=pct; if(percent<0)percent=0; if(percent>100)percent=100;
    const uint8_t baseY=56, segW=4, segH=6, step=5, baseX=5; for(int i=0;i<19;i++){ int bx=baseX+i*step; if ((float)19/100*percent>i) u8g2_DrawBox(u,bx,baseY+1,segW,segH); else u8g2_DrawFrame(u,bx,baseY+1,segW,segH);} 
    if(pct>=0) snprintf(line,sizeof(line),"%3d%%",pct); else snprintf(line,sizeof(line)," --%%"); int len=strlen(line); sys57_draw(u,128-len*6,7*8+1,line);
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
    // Parity with original Arduino logic: always accumulate using absolute values (no voltage validity gate).
    int16_t cur_cA_raw = protocol_total_current_cA();
    uint16_t cur_cA_abs = (uint16_t)(cur_cA_raw < 0 ? -cur_cA_raw : cur_cA_raw); // centi-amps
    uint16_t v_cV = (uint16_t)(g_S25C31.voltage < 0 ? -g_S25C31.voltage : g_S25C31.voltage); // centi-volts
    // Power W*100 = (I_cA * V_cV)/100 with rounding (+50)/100
    uint32_t P_Wx100 = ((uint32_t)cur_cA_abs * (uint32_t)v_cV + 50U) / 100U;
    if (dt > 0){
        s_tripEnergy_Wh_x100 += (P_Wx100 * dt) / 3600UL; // Wh*100 += (W*100 * dt)/3600
        s_lastPowerOnTime_s = pot_s;
    }
    if (cur_cA_abs > s_tripMaxCurrent_cA) s_tripMaxCurrent_cA = cur_cA_abs;
    if (P_Wx100 > s_tripMaxPower_Wx100) s_tripMaxPower_Wx100 = P_Wx100;
    if (v_cV < s_tripMinVoltage_cV) s_tripMinVoltage_cV = v_cV;
    if (v_cV > s_tripMaxVoltage_cV) s_tripMaxVoltage_cV = v_cV;
}

// ---------- Alt screen renderers ----------
static void render_trip_stats_screen(void){
    u8g2_ClearBuffer(get_u());
    char buf[32];
    ui_settings_t *cfg = settings_ui();
    // Average Wh/km (distance in centi-km from mileageCurrent)
    uint32_t dist_km_x100 = g_S23CB0.mileageCurrent; // km*100
    uint16_t avg_whkm_x100 = 0;
    if (dist_km_x100 > 0){
        uint32_t avg = (s_tripEnergy_Wh_x100 * 100UL) / dist_km_x100; // (Wh/km)*100
        if (avg > 65535UL) avg = 65535UL;
        avg_whkm_x100 = (uint16_t)avg;
    }
    uint16_t av_i = avg_whkm_x100 / 100; uint16_t av_f = avg_whkm_x100 % 100;
    // Row 0: AVG label + value aligned to column ~65px (col 11)
    draw_text(0,0,"AVG:");
    snprintf(buf,sizeof(buf),"%3u.%02uWh/km", av_i, av_f); draw_text(11,0,buf);
    // Row 2: Max current or power
    if (cfg->showPower){
        uint32_t w100 = s_tripMaxPower_Wx100; uint16_t w_i = w100/100; uint16_t w_f = w100%100;
        draw_text(0,2,"Pmax:");
        snprintf(buf,sizeof(buf),"%4u.%02uW", w_i, w_f); draw_text(10,2,buf);
    } else {
        uint16_t c_i = s_tripMaxCurrent_cA/100; uint16_t c_f = s_tripMaxCurrent_cA%100;
        draw_text(0,2,"Imax:");
        snprintf(buf,sizeof(buf),"%3u.%02uA", c_i, c_f); draw_text(10,2,buf);
    }
    // Row 4: Umin
    uint16_t vmin_i = (s_tripMinVoltage_cV==0xFFFF)?0:(s_tripMinVoltage_cV/100); uint16_t vmin_f = (s_tripMinVoltage_cV==0xFFFF)?0:(s_tripMinVoltage_cV%100);
    draw_text(0,4,"Umin:");
    snprintf(buf,sizeof(buf),"%2u.%02uV", vmin_i, vmin_f); draw_text(12,4,buf);
    // Row 6: Umax
    uint16_t vmax_i = s_tripMaxVoltage_cV/100; uint16_t vmax_f = s_tripMaxVoltage_cV%100;
    draw_text(0,6,"Umax:");
    snprintf(buf,sizeof(buf),"%2u.%02uV", vmax_i, vmax_f); draw_text(12,6,buf);
}

static void render_odometer_screen(void){
    // Arduino parity: Only show ODO (total) and power-on time with large numeric font.
    u8g2_ClearBuffer(get_u());
    uint32_t tot = g_S23CB0.mileageTotal; // same raw units (1/1000 km)
    uint32_t km_whole = tot / 1000U; uint32_t km_tenths = (tot % 1000U) / 10U; // one decimal
    // Build fixed-width 4-digit field with one decimal; leading spaces to stabilize layout
    char odoDigits[16]; snprintf(odoDigits, sizeof(odoDigits), "%4lu.%01lu", (unsigned long)km_whole, (unsigned long)km_tenths);
    // Label row (small font) at top left
    draw_text(0,0,"ODO:");
    // Draw large odo value starting at y=8 (row 1). Position roughly after label (x=24 px)
    int odoX = 24; stdnum_draw_string(get_u(), odoX, 8, odoDigits);
    int odoRight = odoX + stdnum_text_w(odoDigits);
    // Unit "KM" in small font baseline aligned with large digits (use +7)
    sys57_draw(get_u(), odoRight + 2, 8+7, "KM");
    // Power-on time (total powered seconds) below: label row 5, digits row 6 like Arduino
    uint32_t pot = g_S23C3A.powerOnTime; uint32_t mm = pot/60U; uint32_t ss = pot%60U; if (mm>999) mm=999; 
    char timeDigits[12]; snprintf(timeDigits, sizeof(timeDigits), "%3lu:%02lu", (unsigned long)mm, (unsigned long)ss);
    draw_text(0,5,"ON:");
    // Large time digits at x=25 (~align with Arduino cursor 25) and y=6*8
    stdnum_draw_string(get_u(), 25, 6*8, timeDigits);
}

static void render_temps_screen(void){
    u8g2_t *u = get_u();
    u8g2_ClearBuffer(u);
    extern volatile bool g_mainTempAFValid; extern volatile int16_t g_mainTempC10_AF; extern volatile bool g_batteryDataValid;
    int16_t drv_c10 = g_mainTempAFValid ? g_mainTempC10_AF : g_S23CB0.mainframeTemp; int16_t tdrv = drv_c10/10;
    int16_t t1 = g_batteryDataValid ? (int16_t)g_S25C31.temp1 : 0x7FFF;
    int16_t t2 = g_batteryDataValid ? (int16_t)g_S25C31.temp2 : 0x7FFF;
#if CFG_AHT10_ENABLE
    extern bool g_ahtPresent; extern float g_ahtTempC; extern float g_ahtHum;
    int haveAmb = (g_ahtPresent && !isnan(g_ahtTempC));
    int16_t amb = haveAmb ? (int16_t)lrintf(g_ahtTempC) : 0x7FFF;
    uint16_t rh = haveAmb && !isnan(g_ahtHum) ? (uint16_t)lrintf(g_ahtHum) : 0xFFFF; if (rh>100) rh=100;
#endif
    // Always display Celsius (US_Version removed)
    t1=clamp_temp_c(t1); t2=clamp_temp_c(t2); tdrv=clamp_temp_c(tdrv);
#if CFG_AHT10_ENABLE
    amb=clamp_temp_c(amb);
#endif
    sys57_draw(u,0,0,"BATT");
    draw_temp_num(u,0,8,t1);
    sys57_draw(u,(g_stdnum.w+1)*3,8,"C");
    draw_temp_num(u,87,8,t2);
    sys57_draw(u,87 + (g_stdnum.w+1)*3,8,"C");
    sys57_draw(u,0,5*8,"DRV"); draw_temp_num(u,0,6*8,tdrv);
    sys57_draw(u,(g_stdnum.w+1)*3,6*8,"C");
#if CFG_AHT10_ENABLE
    if (haveAmb){
    sys57_draw(u,64,3*8,"Amb:"); draw_temp_num(u,87,3*8,amb);
    sys57_draw(u,87 + (g_stdnum.w+1)*3,3*8,"C");
        sys57_draw(u,64,6*8,"RH:");
        if (rh!=0xFFFF){ char rbuf[6]; snprintf(rbuf,sizeof(rbuf),"%u", (unsigned)rh); stdnum_draw_string(u,87,6*8,rbuf); sys57_draw(u,87 + (g_stdnum.w+1)*((rh<10)?1:(rh<100?2:3))+2,6*8,"%"); }
        else sys57_draw(u,87,6*8,"--%");
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
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif