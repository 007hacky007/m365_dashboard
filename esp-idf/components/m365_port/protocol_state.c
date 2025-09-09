#include "protocol_state.h"
#include <string.h>
#include <stdio.h>

// Shared timestamp for last strong (authoritative) voltage update (0x47 frame or full battery frame)
// Currently not used (heuristic disabled); retain for future use and silence warning.
static uint32_t s_lastStrongVoltageMs __attribute__((unused)) = 0;

// Enable verbose decode logging (set to 0 to disable). Logs each received frame with interpreted fields.
#ifndef M365_DECODE_DEBUG
#define M365_DECODE_DEBUG 0   // Turn off when doing raw full-bus sniff to reduce duplication
#endif

// Compile-time master TX disable (receive-only sniff mode).
// Default now 0 (active querying) so battery percent & temps arrive from BUS again.
#ifndef M365_DISABLE_TX
#define M365_DISABLE_TX 0     // Passive sniffing default
#endif

// If TX disabled force passive-only (don't even prepare queries) so we observe *only* native bus traffic
#if M365_DISABLE_TX
#ifndef PASSIVE_ONLY
#define PASSIVE_ONLY 1
#endif
#endif

// Disable heuristic voltage fallback (parsing 0xAF / 0x47 to synthesize voltage before full frame) by default.
#ifndef M365_DISABLE_VOLTAGE_HEURISTIC
#define M365_DISABLE_VOLTAGE_HEURISTIC 1
#endif

// Full raw frame sniff (after checksum OK) handled in comms_port.c
#ifndef M365_FULL_SNIFF
#define M365_FULL_SNIFF 1
#endif

// Fixed mapping mode to avoid adaptive mis-lock causing false UI screen cycles.
#ifndef M365_INPUT_MAP_FIXED
#define M365_INPUT_MAP_FIXED 1
#endif
#ifndef M365_DEBUG_RAW_SNIFF
#define M365_DEBUG_RAW_SNIFF 1
#endif
// Optional raw byte range discovery for throttle/brake frame (addr 0x20 cmd 0x00) to help adjust indices without guessing.
// Prints once per second the min/max observed for first up to 8 bytes of payload (length-dependent) plus current values.
#ifndef M365_INPUT_DISCOVER_DEBUG
#define M365_INPUT_DISCOVER_DEBUG 0
#endif
// Always-on lightweight raw input tracing (set 0 to disable). Prints every addr 0x20/0x21 cmd 0x00 frame with raw bytes & mapped value.
#ifndef M365_INPUT_TRACE_RAW
#define M365_INPUT_TRACE_RAW 0
#endif
// Allow temporary fallback to secondary realtime address (0x21) for throttle/brake mapping
// if primary (0x20) frames stop arriving (some scooters only broadcast one source).
#ifndef M365_INPUT_ALLOW_SECONDARY_FALLBACK
#define M365_INPUT_ALLOW_SECONDARY_FALLBACK 1
#endif

// Emit per-value decoded logs (SoC, pack voltage, temps) when they change
#ifndef M365_VALUE_LOG
#define M365_VALUE_LOG 1
#endif

#if M365_INPUT_MAP_FIXED
// Frame example (idle): 06 20 20 00 00 74 00  (hz=0x64 variant, rawLen=7)
// Observation: byte[1] varies with brake (0x20 idle -> ~0x65 pressed).
// Throttle byte still provisional; capture a log while moving throttle with TX enabled to confirm and then update THR_BYTE_INDEX.
#define THR_BYTE_INDEX 1           // Observed throttle changing at payload[1]
#define THR_RAW_MIN    0x20        // Idle baseline seen in passive sniff
#define THR_RAW_MAX    0xD5        // Peak observed (~0xD2) add small headroom
#define BR_BYTE_INDEX  2           // Brake (still unobserved changing; placeholder)
#define BR_RAW_MIN     0x20
#define BR_RAW_MAX     0x70        // Placeholder until real brake capture
#endif

// ================= Global Definitions =================
QUERY_t g_Query = {0};
volatile uint8_t  g_NewDataFlag = 0;
volatile bool     g_Hibernate = false; // future use (sleep)

ANSWER_HEADER g_AnswerHeader = {0};
A20C00HZ65  g_S20C00HZ65 = {0};
A25C31      g_S25C31 = {0};
A23C3E      g_S23C3E = {0};
A23CB0      g_S23CB0 = {{0},0,0,0,0,0,0,{0}};
A23C23      g_S23C23 = {0,0,0,0,0};
A23C3A      g_S23C3A = {0,0};
A25C40      g_S25C40 = {0};
// Newly added decoded structures
A23C3B      g_S23C3B = {0};
A23C1A      g_S23C1A = {0};
A23C67      g_S23C67 = {0};
A23C69      g_S23C69 = {0};
A23C7B      g_S23C7B = {0};
A23C7C      g_S23C7C = {0};
A23C7D      g_S23C7D = {0};
A23C10      g_S23C10 = {{0},0};
A23CAF      g_S23CAF = {{0},0};
A25C10      g_S25C10 = {{0},0};
A25C17      g_S25C17 = {0};
A25C18      g_S25C18 = {0};
A25C20      g_S25C20 = {0};
A25C1B      g_S25C1B = {0,0};
A25C32      g_S25C32 = {0};
A25C33      g_S25C33 = {0};
A25C34      g_S25C34 = {0};
A25C35      g_S25C35 = {0,0};
A25C3B      g_S25C3B = {0};
A25C30      g_S25C30 = {0};
CMD_t       g_cmd = {0};
END20T_t    g_end20t = {0,0,0};
volatile bool g_batteryDataValid = false;
volatile ANSWER_HEADER g_lastDiagHeader = {0};
volatile uint8_t g_lastDiagPayload[8] = {0};
volatile bool g_batteryFullFrame = false;
volatile int16_t g_mainTempC10_AF = 0;
volatile bool g_mainTempAFValid = false;
volatile uint32_t g_afTripDistance_m = 0;
volatile uint32_t g_bms31_lastOkMs = 0;

const uint8_t g_commandsWeWillSend[3] = {1,8,10};
// Query tables (flattened from PROGMEM arrays)
const uint8_t g_q[15]  = {0x3B,0x31,0x20,0x1B,0x10,0x1A,0x69,0x3E,0xB0,0x23,0x3A,0x7B,0x7C,0x7D,0x40};
const uint8_t g_l[15]  = {   2,  10,   6,   4,  18,  12,   2,   2,  32,   6,   4,   2,   2,   2,  30};
const uint8_t g_f[15]  = {   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,   2,   2,   1};
const uint8_t g_h0[2]  = {0x55,0xAA};
const uint8_t g_h1[3]  = {0x03,0x22,0x01};
const uint8_t g_h2[3]  = {0x06,0x20,0x61};
const uint8_t g_hc[3]  = {0x04,0x20,0x03};

// Battery pack scaling (default single pack). Could be made configurable later.
#ifndef PACK1_MAH
#define PACK1_MAH 7800u
#endif
#ifndef PACK2_MAH
#define PACK2_MAH 14000u
#endif

// ================= Helpers =================
uint16_t protocol_calc_cs(const uint8_t *data, uint8_t len) {
    uint16_t cs = 0xFFFF; while (len--) cs -= *data++; return cs;
}

int16_t protocol_total_current_cA(void) {
    if (PACK2_MAH == 0) return g_S25C31.current; // centi-amps raw
    float scale = ((float)PACK1_MAH + (float)PACK2_MAH)/(float)PACK1_MAH;
    float sc = (float)g_S25C31.current * scale;
    if (sc > 32767.f) {
        sc = 32767.f;
    }
    if (sc < -32768.f) {
        sc = -32768.f;
    }
    return (int16_t)sc;
}

// ================= Packet Processing =================
void protocol_process_packet(const uint8_t *frameBuf, uint8_t totalLen, uint32_t nowMs) {
    // frameBuf points to start of payload (after header?) In our UART FSM we'll pass start of payload.
    // We already have g_AnswerHeader filled by FSM while reading.
    // g_AnswerHeader.len includes payload + 2 checksum bytes (as per original Arduino implementation)
    if (g_AnswerHeader.len < 2) return; // must at least contain checksum
    uint8_t rawLen = g_AnswerHeader.len - 2; // pure payload length
    if (rawLen > 60) return; // sanity guard
    uint8_t addr = g_AnswerHeader.addr;
    uint8_t cmd  = g_AnswerHeader.cmd;
    // hz field available as g_AnswerHeader.hz (local copy omitted to reduce unused variable warnings)
    // Optional raw sniff (prints everything including unknown addresses)
#if M365_DEBUG_RAW_SNIFF
    {
        char bytes[3*32]; bytes[0]='\0'; int n=0; uint8_t show = (g_AnswerHeader.len-2); if (show>30) show=30; for(uint8_t i=0;i<show;i++) n+=snprintf(bytes+n,sizeof(bytes)-n,"%02X ", frameBuf[i]); if(show < (g_AnswerHeader.len-2)) snprintf(bytes+n,sizeof(bytes)-n,"...");
        printf("[SNIFF] len=%u addr=%02X hz=%02X cmd=%02X rawLen=%u %s\n", g_AnswerHeader.len, g_AnswerHeader.addr, g_AnswerHeader.hz, g_AnswerHeader.cmd, rawLen, bytes);
    }
#endif
    // Whitelist of expected device addresses from spec.
    bool addrKnown = (addr==0x20 || addr==0x21 || addr==0x22 || addr==0x23 || addr==0x25);
    if (!addrKnown){
    // Ignore unknown address frames except optionally log minimal diagnostic.
#if M365_DECODE_DEBUG
    printf("[DECODE-IGN] unknown addr=%02X cmd=%02X hz=%02X len=%u rawLen=%u\n", addr, cmd, hz, g_AnswerHeader.len, rawLen);
#endif
    goto store_diag_only; // still record last header & payload snippet
    }

    switch (addr) {
    case 0x20: // realtime control frames (primary on many firmwares)
    case 0x21: // mirrored realtime control frames (secondary / alt firmware)
                    // Unified throttle/brake acquisition:
                    // Address: 0x20 primary (preferred), 0x21 secondary (fallback after >400ms gap).
                    // Command: cmd == 0x00 only.
                    // Two observed payload variants (rawLen includes ALL payload bytes, not checksum):
                    //   Variant A (5 bytes): 04 <thr> <br> 00 00
                    //   Variant B (7 bytes): 06 <thr> <br> 00 00 74 00
                    // First byte is a count marker equal to (rawLen - 1): 0x04 for 5-byte frame, 0x06 for 7-byte frame.
                    // Signals:
                    //   throttle raw  = payload[1] (idle ≈ 0x20, high ≈ 0xD2–0xD3)
                    //   brake raw     = payload[2] (idle ≈ 0x20, high ≈ 0x70)
                    // Remaining bytes currently unused (00 00 [74 00]).
                    // Layout assumption (payload indexes): [0]=lenMarker, [1]=throttleRaw, [2]=brakeRaw.
#if M365_INPUT_DISCOVER_DEBUG
                    if (addr == 0x20 && cmd == 0x00) {
                        static uint8_t minB[8];
                        static uint8_t maxB[8];
                        static uint8_t lastB[8];
                        static int init = 0;
                        if (!init) { for (int i=0;i<8;i++){ minB[i]=0xFF; maxB[i]=0; lastB[i]=0; } init=1; }
                        uint8_t show = rawLen > 8 ? 8 : rawLen;
                        int changed = 0;
                        for (uint8_t i=0;i<show;i++) {
                            uint8_t v = frameBuf[i];
                            if (v < minB[i]) minB[i] = v;
                            if (v > maxB[i]) maxB[i] = v;
                            if (v != lastB[i]) { changed = 1; lastB[i] = v; }
                        }
                        static uint32_t lastPrintMs = 0;
                        if (changed || (nowMs - lastPrintMs) > 1000) {
                            lastPrintMs = nowMs;
                            char cur[3*8+1]; char min[3*8+1]; char max[3*8+1]; int n1=0,n2=0,n3=0;
                            for (uint8_t i=0;i<show;i++){ n1+=snprintf(cur+n1,sizeof(cur)-n1,"%02X ", lastB[i]); n2+=snprintf(min+n2,sizeof(min)-n2,"%02X ", minB[i]); n3+=snprintf(max+n3,sizeof(max)-n3,"%02X ", maxB[i]); }
                            printf("[THDBG] rawLen=%u cur=%s | min=%s | max=%s\n", rawLen, cur, min, max);
                        }
                    }
#endif // M365_INPUT_DISCOVER_DEBUG
                    #if M365_INPUT_MAP_FIXED
                    static uint32_t s_lastPrimaryInputMs = 0; // ms timestamp of last addr 0x20 processed
                    bool isPrimary = (addr == 0x20);
                    if (isPrimary) s_lastPrimaryInputMs = nowMs;
#if M365_INPUT_ALLOW_SECONDARY_FALLBACK
                    // If no primary frame for >400ms, accept secondary as temporary source
                    if (!isPrimary && (nowMs - s_lastPrimaryInputMs) > 400) {
                        isPrimary = true;
                    }
#endif
                    if (!isPrimary) {
                        // Skip mapping this frame (still may log) – primary or timed‑out fallback only.
                    } else if (cmd == 0x00 && (rawLen == 5 || rawLen == 7)) {
                        // Validate length marker (payload[0]) matches (rawLen - 1). If not, ignore frame.
                        uint8_t lenMarker = frameBuf[0];
                        if (lenMarker != (uint8_t)(rawLen - 1)) {
#if M365_INPUT_TRACE_RAW
                            printf("[THBR-INV] rawLen=%u marker=0x%02X expected=0x%02X\n", rawLen, lenMarker, (uint8_t)(rawLen - 1));
#endif
                        } else {
                            // Copy first 5 bytes (legacy struct has 5 fields: hz1, throttle, brake, hz2, hz3)
                            memcpy(&g_S20C00HZ65, frameBuf, 5);
                        
                        // Throttle
                        if (rawLen > THR_BYTE_INDEX) {
                            uint8_t rawThr = frameBuf[THR_BYTE_INDEX];
                            if (rawThr < THR_RAW_MIN) rawThr = THR_RAW_MIN;
                            if (rawThr > THR_RAW_MAX) rawThr = THR_RAW_MAX;
                            uint16_t thrSpan = (uint16_t)(THR_RAW_MAX - THR_RAW_MIN); if (!thrSpan) thrSpan = 1;
                            uint8_t normThr = (uint8_t)(((uint32_t)(rawThr - THR_RAW_MIN) * 100u + thrSpan/2) / thrSpan);
#ifndef M365_INPUT_DISABLE_SMOOTH
                            static uint8_t lastThr=0; lastThr = normThr; g_S20C00HZ65.throttle = lastThr;
#else
                            g_S20C00HZ65.throttle = normThr;
#endif
#if M365_INPUT_TRACE_RAW
                            static uint8_t prevRawThr = 0xFF;
                            if (rawThr != prevRawThr) {
                                int d = (int)rawThr - (int)prevRawThr;
                                if (prevRawThr == 0xFF || d > 1 || d < -1) {
                                    printf("[THEDGE] t=%lu src=%02X rawThr=0x%02X(%3u) norm=%u\n",
                                           (unsigned long)nowMs, addr, rawThr, rawThr, (unsigned)g_S20C00HZ65.throttle);
                                }
                                prevRawThr = rawThr;
                            }
#endif
                        }
                        // Brake
                        if (rawLen > BR_BYTE_INDEX) {
                            uint8_t rawBr = frameBuf[BR_BYTE_INDEX];
                            if (rawBr < BR_RAW_MIN) rawBr = BR_RAW_MIN;
                            if (rawBr > BR_RAW_MAX) rawBr = BR_RAW_MAX;
                            uint8_t span = (BR_RAW_MAX - BR_RAW_MIN); if (!span) span = 1;
                            uint8_t normBr = (uint8_t)(((uint32_t)(rawBr - BR_RAW_MIN) * 100u + span/2) / span);
#ifndef M365_INPUT_DISABLE_SMOOTH
                            static uint8_t lastBr=0; lastBr = normBr; g_S20C00HZ65.brake = lastBr;
#else
                            g_S20C00HZ65.brake = normBr;
#endif
                        }
                        static int once=0; if(!once){ once=1; printf("[THFIX] unified addr (0x20/0x21) pattern active cmd=0x00 PLEN=4|6 thrIdx=%d brIdx=%d\n", THR_BYTE_INDEX, BR_BYTE_INDEX); }
                        }
                    }
                    #elif M365_DECODE_DEBUG
                    // (Adaptive mapping code removed under fixed mode.)
                    #endif
        #if (M365_DECODE_DEBUG && M365_INPUT_MAP_FIXED) || M365_INPUT_TRACE_RAW
                    {
                        char hb[48]; int nn=0; uint8_t show = rawLen>8?8:rawLen; for(uint8_t i=0;i<show;i++) nn+=snprintf(hb+nn,sizeof(hb)-nn,"%02X ", frameBuf[i]);
                        if (addr == 0x20) {
                            printf("[THBR] (P) rawLen=%u bytes=%s| th=%u br=%u\n", rawLen, hb, g_S20C00HZ65.throttle, g_S20C00HZ65.brake);
                        } else {
                            printf("[THBR] (%c) rawLen=%u bytes=%s| th=%u br=%u%s\n", ( (nowMs - s_lastPrimaryInputMs) > 400 ? 'F':'S'), rawLen, hb, g_S20C00HZ65.throttle, g_S20C00HZ65.brake, ((nowMs - s_lastPrimaryInputMs) > 400?" Fallback":""));
                        }
                    }
        #endif
            break;
    case 0x22: // Variant: battery percent / temp short frames (cmd 0x31 / 0x40) appear here
            if (g_AnswerHeader.cmd == 0x31) {
                        if (rawLen == sizeof(A25C31)) { memcpy(&g_S25C31, frameBuf, rawLen); g_batteryDataValid = true; g_batteryFullFrame = true; }
                        else if (rawLen == 1) { 
                            uint8_t v = frameBuf[0]; 
                            if (v<=100) {
                                // Glitch filter: ignore isolated 10% (or massive drop >30) if we already have a higher validated value and a full frame received.
                                if (g_batteryDataValid && g_batteryFullFrame) {
                                    uint8_t prev = g_S25C31.remainPercent;
                                    if ((v == 10 && prev > 20) || (prev > v && (prev - v) > 30)) {
                                        // reject glitch
                                    } else {
                                        g_S25C31.remainPercent = v; g_batteryDataValid = true; 
                                    }
                                } else { g_S25C31.remainPercent = v; g_batteryDataValid = true; }
                            }
                        }
            } else if (g_AnswerHeader.cmd == 0x40) {
                if (rawLen == sizeof(A25C40)) { 
                    memcpy(&g_S25C40, frameBuf, rawLen); 
#if M365_VALUE_LOG
                    // Per‑cell voltage logging (only when any cell changes)
                    static int16_t prev[15] = {0};
                    int16_t cur[15] = { g_S25C40.c1,g_S25C40.c2,g_S25C40.c3,g_S25C40.c4,g_S25C40.c5,
                                        g_S25C40.c6,g_S25C40.c7,g_S25C40.c8,g_S25C40.c9,g_S25C40.c10,
                                        g_S25C40.c11,g_S25C40.c12,g_S25C40.c13,g_S25C40.c14,g_S25C40.c15};
                    int changed = 0; for(int i=0;i<15;i++){ if(cur[i]!=prev[i]) { changed=1; break; } }
                    if (changed) {
                        // Compute stats over non‑zero cells
                        int count=0; int16_t min=0x7FFF,max=0, sum=0; 
                        for(int i=0;i<15;i++){ if(cur[i]>0){ if(cur[i]<min) min=cur[i]; if(cur[i]>max) max=cur[i]; sum+=cur[i]; count++; } }
                        int16_t avg = (count? (int16_t)(sum / count):0);
                        printf("[VAL] Cells mV:");
                        for(int i=0;i<15;i++){ if(cur[i]>0) printf(" %d", cur[i]); }
                        if(count){ printf(" | min=%d max=%d avg=%d delta=%d n=%d\n", min, max, avg, (int)(max-min), count); }
                        else { printf(" (all zero)\n"); }
                        for(int i=0;i<15;i++) prev[i]=cur[i];
                    }
#endif
                }
                else if (rawLen == 1) { if (!g_batteryDataValid) { uint8_t rawT = frameBuf[0]; g_S25C31.temp1 = (rawT>=20)? (rawT-20):0; } }
            }
            break;
        case 0x23: // speed / mileage etc
            if (g_AnswerHeader.cmd == 0xB0) {
                if (rawLen == sizeof(A23CB0)) {
                    memcpy(&g_S23CB0, frameBuf, rawLen);
                } else if (rawLen >= 22) {
                    // Accept shorter variant (some firmwares omit trailing reserved bytes).
                    // Offsets: 0-9 u1[10], 10-11 speed, 12-13 avg, 14-17 mileageTotal, 18-19 mileageCurrent, 20-21 elapsedPowerOnTime, 22-23 mainframeTemp (needs >=24)
                    const uint8_t *p = frameBuf;
                    if (rawLen >= 12) g_S23CB0.speed = (int16_t)( (uint16_t)p[10] | ((uint16_t)p[11] << 8));
                    if (rawLen >= 14) g_S23CB0.averageSpeed = (uint16_t)p[12] | ((uint16_t)p[13] << 8);
                    if (rawLen >= 18) g_S23CB0.mileageTotal = ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16) | ((uint32_t)p[16] << 8) | (uint32_t)p[17];
                    if (rawLen >= 20) g_S23CB0.mileageCurrent = (uint16_t)p[18] | ((uint16_t)p[19] << 8);
                    if (rawLen >= 22) g_S23CB0.elapsedPowerOnTime = (uint16_t)p[20] | ((uint16_t)p[21] << 8);
                    if (rawLen >= 24) g_S23CB0.mainframeTemp = (int16_t)((uint16_t)p[22] | ((uint16_t)p[23] << 8));
#if M365_DECODE_DEBUG
                    static int onceB0=0; if(!onceB0){ onceB0=1; printf("[B0-PARTIAL] Accepted partial 0xB0 frame rawLen=%u (<%u)\n", rawLen, (unsigned)sizeof(A23CB0)); }
#endif
                }
            }
            else if (g_AnswerHeader.cmd == 0x23) { if (rawLen == sizeof(A23C23)) memcpy(&g_S23C23, frameBuf, rawLen); }
            else if (g_AnswerHeader.cmd == 0x3A) {
                if (rawLen == sizeof(A23C3A)) {
                    memcpy(&g_S23C3A, frameBuf, rawLen);
                } else if (rawLen >= 4) {
                    // Some firmwares return longer (e.g., 0x0A) frame; take first 4 bytes.
                    g_S23C3A.powerOnTime = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1] << 8);
                    g_S23C3A.ridingTime  = (uint16_t)frameBuf[2] | ((uint16_t)frameBuf[3] << 8);
#if M365_DECODE_DEBUG
                    static int once3A=0; if(!once3A){ once3A=1; printf("[3A-PARTIAL] Accepted partial 0x3A frame rawLen=%u\n", rawLen); }
#endif
                }
            }
            else if (g_AnswerHeader.cmd == 0x3E) { if (rawLen == sizeof(A23C3E)) memcpy(&g_S23C3E, frameBuf, rawLen); }
            else if (g_AnswerHeader.cmd == 0x3B) { if (rawLen == 2) { g_S23C3B.tripTimeSeconds = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x1A) { if (rawLen == 2) { g_S23C1A.firmware = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x67) { if (rawLen >= 4) { g_S23C67.bmsVer = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); g_S23C67.var104 = (uint16_t)frameBuf[2] | ((uint16_t)frameBuf[3]<<8); } }
            else if (g_AnswerHeader.cmd == 0x69) { if (rawLen == 2) { g_S23C69.value = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x7B) { if (rawLen == 2) { g_S23C7B.kers = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x7C) { if (rawLen == 2) { g_S23C7C.cruise = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x7D) { if (rawLen == 2) { g_S23C7D.tail = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x10) { // serial string (up to 14 bytes, may include slash)
                uint8_t copy = rawLen; if (copy > 14) copy = 14; memcpy(g_S23C10.serial, frameBuf, copy); g_S23C10.serial[copy] = '\0'; g_S23C10.len = copy; }
            else if (g_AnswerHeader.cmd == 0xAF) {
                if (rawLen == 26) {
                    // Parse explicit fields
                    const uint8_t *p = frameBuf;
                    g_S23CAF.distance_m     = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                    g_S23CAF.mode_index     = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | (uint32_t)p[7];
                    g_S23CAF.speed_limit1_dms = (uint16_t)p[8] | ((uint16_t)p[9] << 8);
                    g_S23CAF.speed_limit2_dms = (uint16_t)p[10] | ((uint16_t)p[11] << 8);
                    g_S23CAF.reserved       = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) | ((uint32_t)p[14] << 8) | (uint32_t)p[15];
                    g_S23CAF.flags_ts_raw   = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) | ((uint32_t)p[18] << 8) | (uint32_t)p[19];
                    g_S23CAF.ticks          = ((uint32_t)p[20] << 24) | ((uint32_t)p[21] << 16) | ((uint32_t)p[22] << 8) | (uint32_t)p[23];
                    g_S23CAF.controller_temp_dC = (uint16_t)p[24] | ((uint16_t)p[25] << 8);
                    memcpy(g_S23CAF.raw, p, 26);
                    g_S23CAF.frameCount++;
                    // Distance feed for trip metrics
                    g_afTripDistance_m = g_S23CAF.distance_m;
                    // Map controller temperature to generic AF temp (tenths C)
                    if (g_S23CAF.controller_temp_dC <= 1200) {
                        g_mainTempC10_AF = (int16_t)g_S23CAF.controller_temp_dC;
                        g_mainTempAFValid = true;
                    }
#if M365_VALUE_LOG
                    static uint32_t prevDist=0; static uint16_t prevLim1=0, prevLim2=0; static int prevTmp=-10000;
                    if (g_S23CAF.distance_m != prevDist){ printf("[AF] dist=%lu m\n", (unsigned long)g_S23CAF.distance_m); prevDist = g_S23CAF.distance_m; }
                    if (g_S23CAF.speed_limit1_dms != prevLim1){ float kmh = 0.36f * g_S23CAF.speed_limit1_dms; printf("[AF] spdLim1=%.1f km/h\n", kmh); prevLim1 = g_S23CAF.speed_limit1_dms; }
                    if (g_S23CAF.speed_limit2_dms != prevLim2){ float kmh = 0.36f * g_S23CAF.speed_limit2_dms; printf("[AF] spdLim2=%.1f km/h\n", kmh); prevLim2 = g_S23CAF.speed_limit2_dms; }
                    if (g_mainTempAFValid && g_mainTempC10_AF != prevTmp){ printf("[AF] CtrlTemp=%.1fC\n", g_mainTempC10_AF/10.0f); prevTmp = g_mainTempC10_AF; }
#endif
                } else {
                    // Unexpected length; copy raw for reference (truncate/pad)
                    uint8_t copy = rawLen < 26 ? rawLen : 26; memset(g_S23CAF.raw, 0, 26); memcpy(g_S23CAF.raw, frameBuf, copy); g_S23CAF.frameCount++;
                }
            }
            else if (g_AnswerHeader.cmd == 0x47) {
#if !M365_DISABLE_VOLTAGE_HEURISTIC
                if (!g_batteryFullFrame && rawLen == 4) {
                    uint16_t v1 = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1] << 8);
                    uint16_t v2 = (uint16_t)frameBuf[2] | ((uint16_t)frameBuf[3] << 8);
                    uint16_t candidate = 0; if (v1>=3000 && v1<=5000) candidate = v1; if (v2>=3000 && v2<=5000 && v2>candidate) candidate = v2;
                    if (candidate) { g_S25C31.voltage = (int16_t)candidate; if (!g_batteryDataValid) g_batteryDataValid = true; s_lastStrongVoltageMs = nowMs; }
                }
#endif
            }
            break;
        case 0x25: // battery / current / temps
            if (g_AnswerHeader.cmd == 0x30) { // Extended battery status (alternative to 0x31)
                if (rawLen == sizeof(A25C30)) {
                    memcpy(&g_S25C30, frameBuf, rawLen);
                    // Adopt voltage & temps if plausible; 0x30 appears even when 0x31 absent.
                    uint16_t v_cV = g_S25C30.voltage_cV; // already centivolts
                    if (v_cV >= 3000 && v_cV <= 5000) {
                        if (!g_batteryFullFrame) { g_S25C31.voltage = (int16_t)v_cV; }
                        if (!g_batteryDataValid) g_batteryDataValid = true;
#if M365_VALUE_LOG
                        static int prevV30=-1; if (g_S25C31.voltage != prevV30){ printf("[VAL] PackV=%d.%02dV (0x30)\n", g_S25C31.voltage/100, g_S25C31.voltage%100); prevV30 = g_S25C31.voltage; }
#endif
                    }
                    // Temps (raw +20)
                    uint8_t t1r = g_S25C30.temp1_raw; uint8_t t2r = g_S25C30.temp2_raw;
                    uint8_t t1 = (t1r>=20)? (t1r-20):0; uint8_t t2 = (t2r>=20)? (t2r-20):0;
                    if (t1 != g_S25C31.temp1){ g_S25C31.temp1 = t1; }
                    if (t2 != g_S25C31.temp2){ g_S25C31.temp2 = t2; }
#if M365_VALUE_LOG
                    static int prevT1_30=-1000, prevT2_30=-1000; if (t1 != prevT1_30){ printf("[VAL] Temp1=%dC (0x30)\n", t1); prevT1_30=t1; } if (t2 != prevT2_30){ printf("[VAL] Temp2=%dC (0x30)\n", t2); prevT2_30=t2; }
#endif
                }
            }
            if (g_AnswerHeader.cmd == 0x31) { if (rawLen == sizeof(A25C31)) { 
                static int prevPct=-1, prevVolt=-1, prevT1=-1000, prevT2=-1000;
                memcpy(&g_S25C31, frameBuf, rawLen); g_batteryDataValid = true; g_batteryFullFrame = true; 
                // Apply offset correction: temps encoded as (real+20)
                g_S25C31.temp1 = (g_S25C31.temp1 >= 20) ? (g_S25C31.temp1 - 20) : 0;
                g_S25C31.temp2 = (g_S25C31.temp2 >= 20) ? (g_S25C31.temp2 - 20) : 0;
#if M365_VALUE_LOG
                if (g_S25C31.remainPercent <= 100 && g_S25C31.remainPercent != prevPct){ printf("[VAL] SoC=%u%%\n", (unsigned)g_S25C31.remainPercent); prevPct = g_S25C31.remainPercent; }
                if (g_S25C31.voltage >= 3000 && g_S25C31.voltage <= 5000 && g_S25C31.voltage != prevVolt){ printf("[VAL] PackV=%d.%02dV\n", g_S25C31.voltage/100, g_S25C31.voltage%100); prevVolt = g_S25C31.voltage; }
                if (g_S25C31.temp1 != prevT1){ printf("[VAL] Temp1=%dC\n", g_S25C31.temp1); prevT1 = g_S25C31.temp1; }
                if (g_S25C31.temp2 != prevT2){ printf("[VAL] Temp2=%dC\n", g_S25C31.temp2); prevT2 = g_S25C31.temp2; }
#endif
            } }
            else if (g_AnswerHeader.cmd == 0x40) { if (rawLen == sizeof(A25C40)) memcpy(&g_S25C40, frameBuf, rawLen); }
            else if (g_AnswerHeader.cmd == 0x40 && rawLen == sizeof(A25C40)) { /* handled above */ }
            else if (g_AnswerHeader.cmd == 0x10) { uint8_t copy=rawLen; if(copy>14)copy=14; memcpy(g_S25C10.serial, frameBuf, copy); g_S25C10.serial[copy]='\0'; g_S25C10.len=copy; }
            else if (g_AnswerHeader.cmd == 0x17) { if (rawLen == 2) { g_S25C17.bmsVersion = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x18) { if (rawLen == 2) { g_S25C18.factoryCapacity_mAh = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x20) { if (rawLen == 2) { g_S25C20.dateRaw = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            else if (g_AnswerHeader.cmd == 0x1B) { if (rawLen >= 4) { g_S25C1B.cycles1 = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); g_S25C1B.cycles2 = (uint16_t)frameBuf[2] | ((uint16_t)frameBuf[3]<<8); } }
            else if (g_AnswerHeader.cmd == 0x32) { 
                if (rawLen == 2) { 
                    g_S25C32.percent = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8);
                    // Treat 0x32 as direct SoC (%). Use it to update main remainPercent when plausible.
                    uint16_t soc = g_S25C32.percent; 
                    if (soc <= 100) { 
                        // If we don't yet have a full frame, or only partial data so far, adopt it.
                        if (!g_batteryDataValid || (g_batteryDataValid && !g_batteryFullFrame)) {
                            g_S25C31.remainPercent = (uint8_t)soc; 
                            g_batteryDataValid = true; /* partial */
#if M365_VALUE_LOG
                            static int prevPctP=-1; if (soc != prevPctP){ printf("[VAL] SoC=%u%% (0x32)\n", (unsigned)soc); prevPctP = soc; }
#endif
                        }
                    }
                }
            }
            else if (g_AnswerHeader.cmd == 0x33) { if (rawLen == 2) { g_S25C33.current_cA = (int16_t)((uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8)); } }
            else if (g_AnswerHeader.cmd == 0x34) { if (rawLen == 2) { 
                    // 0x34 exposes pack voltage in 10 mV units (centivolts). Reuse for main voltage if valid.
                    g_S25C34.voltage_dV = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); 
                    uint16_t v_cV = g_S25C34.voltage_dV; // centivolts
                    if (v_cV >= 3000 && v_cV <= 5000) { 
                        // Update primary voltage when we do not yet have a full battery frame OR to refresh.
                        if (!g_batteryFullFrame) { g_S25C31.voltage = (int16_t)v_cV; if (!g_batteryDataValid) g_batteryDataValid = true; 
#if M365_VALUE_LOG
                            static int prevVoltP=-1; if (g_S25C31.voltage != prevVoltP){ printf("[VAL] PackV=%d.%02dV (0x34)\n", g_S25C31.voltage/100, g_S25C31.voltage%100); prevVoltP = g_S25C31.voltage; }
#endif
                        }
                    }
                } }
            else if (g_AnswerHeader.cmd == 0x35) { if (rawLen >= 2) { g_S25C35.t1 = frameBuf[0]; g_S25C35.t2 = frameBuf[1]; } }
            else if (g_AnswerHeader.cmd == 0x3B) { if (rawLen == 2) { g_S25C3B.health = (uint16_t)frameBuf[0] | ((uint16_t)frameBuf[1]<<8); } }
            break;
        default:
            break;
    }
#if M365_DECODE_DEBUG
    do {
        // Print frame summary and decoded high-level values of interest.
        // Limit payload print to first 12 bytes to reduce spam.
    char payHex[3*13]; payHex[0]='\0'; int n=0; uint8_t show = rawLen; if (show>12) show=12; for(uint8_t i=0;i<show;i++) n+=snprintf(payHex+n,sizeof(payHex)-n,"%02X ", frameBuf[i]); if (rawLen==0){ snprintf(payHex,sizeof(payHex),"--"); } else if (show<rawLen) snprintf(payHex+n,sizeof(payHex)-n,"...");
        // Common metrics
        int throttle = g_S20C00HZ65.throttle;
        int brake    = g_S20C00HZ65.brake;
        int speedRaw = g_S23CB0.speed;
        int voltage  = g_S25C31.voltage;
        int current  = g_S25C31.current;
        int pct      = g_S25C31.remainPercent;
        int temp1    = g_S25C31.temp1;
        int temp2    = g_S25C31.temp2;
        int mainTemp = g_S23CB0.mainframeTemp;
        uint32_t milTot = g_S23CB0.mileageTotal;
        uint16_t milCur = g_S23CB0.mileageCurrent;
        uint16_t pOn    = g_S23C3A.powerOnTime;
        uint16_t rTime  = g_S23C3A.ridingTime;
    // Skip noisy empty secondary realtime frames (addr 0x21 rawLen 0) to reduce log spam
    if (!(addr==0x21 && rawLen==0)) {
    printf("[DECODE] addr=%02X cmd=%02X hz=%02X rawLen=%u pay=%s | thr=%d br=%d sp=%d volt=%d cur=%d pct=%d t1=%d t2=%d mTemp=%d mTot=%lu mCur=%u pOn=%u rTime=%u fw=%u kers=%u cruise=%u tail=%u health=%u%s\n",
           addr, cmd, hz, rawLen, payHex, throttle, brake, speedRaw, voltage, current, pct, temp1, temp2, mainTemp,
           (unsigned long)milTot, (unsigned)milCur, (unsigned)pOn, (unsigned)rTime,
           (unsigned)g_S23C1A.firmware, (unsigned)g_S23C7B.kers, (unsigned)g_S23C7C.cruise, (unsigned)g_S23C7D.tail, (unsigned)g_S25C3B.health,
           (rawLen==0?" (empty)":""));
    }
    // Partial speed frame diagnostic: if we see addr 0x23 cmd 0xB0 with unexpected length
    if (addr==0x23 && cmd==0xB0 && rawLen>0 && rawLen != sizeof(A23CB0)) {
        printf("[SPEEDPART] rawLen=%u expected=%u (waiting for full speed frame)\n", rawLen, (unsigned)sizeof(A23CB0));
    }
    } while(0);
#endif
store_diag_only:
    // Store diagnostics (non-atomic copy acceptable). Limit payload copy to 8 bytes.
    g_lastDiagHeader = g_AnswerHeader;
    for(uint8_t i=0;i<8;i++){ g_lastDiagPayload[i] = (i<rawLen)?frameBuf[i]:0; }
    // Flag new data if it's one of the commands we monitor
    for (uint8_t i=0;i<3;i++) {
        if (g_AnswerHeader.cmd == g_q[g_commandsWeWillSend[i]]) { g_NewDataFlag = 1; break; }
    }
    (void)nowMs; // Placeholder (UI overlay will use bus activity timestamps elsewhere)
}

// ================= Query Preparation =================
static uint8_t preloadQueryFromTable(unsigned char index) {
    if (index >= sizeof(g_q)) return 1; // OOB
    if (g_Query.prepared) return 2;     // Already prepared
    uint8_t format = g_f[index];
    const uint8_t *ph = NULL; uint8_t hLen = 0; const uint8_t *pe = NULL; uint8_t eLen = 0;
    if (format == 1) { ph = g_h1; hLen = sizeof(g_h1); }
    else if (format == 2) {
        ph = g_h2; hLen = sizeof(g_h2);
        // Populate dynamic end20t
        g_end20t.hz = 0x02; g_end20t.th = g_S20C00HZ65.throttle; g_end20t.br = g_S20C00HZ65.brake; pe = (uint8_t*)&g_end20t; eLen = sizeof(g_end20t);
    }
    uint8_t *ptr = g_Query.buf;
    memcpy(ptr, g_h0, sizeof(g_h0)); ptr += sizeof(g_h0);
    memcpy(ptr, ph, hLen); ptr += hLen;
    *ptr++ = g_q[index];
    *ptr++ = g_l[index];
    if (pe) { memcpy(ptr, pe, eLen); ptr += eLen; }
    g_Query.DataLen = (uint8_t)(ptr - &g_Query.buf[2]);
    g_Query.cs = protocol_calc_cs(&g_Query.buf[2], g_Query.DataLen);
    return 0;
}

void protocol_prepare_next_query(void) {
    static uint8_t rotIndex = 0;
    // Original dynamic set: 1,8,10,14 (indexes into table) -> replicate
    g_Query._dynQueries[0] = 1; g_Query._dynQueries[1] = 8; g_Query._dynQueries[2] = 10; g_Query._dynQueries[3] = 14; g_Query._dynSize = 4;
    if (preloadQueryFromTable(g_Query._dynQueries[rotIndex]) == 0) g_Query.prepared = 1;
    rotIndex++; if (rotIndex >= g_Query._dynSize) rotIndex = 0;
}

int protocol_prepare_command(uint8_t cmdId) {
#if M365_DISABLE_TX
    (void)cmdId; return -2; // TX disabled
#else
    // Build in g_Query buffer using command header g_h0 + g_hc + CMD struct
    switch (cmdId) {
        case CMD_CRUISE_ON:  g_cmd.param=0x7C; g_cmd.value=1; break;
        case CMD_CRUISE_OFF: g_cmd.param=0x7C; g_cmd.value=0; break;
        case CMD_LED_ON:     g_cmd.param=0x7D; g_cmd.value=2; break;
        case CMD_LED_OFF:    g_cmd.param=0x7D; g_cmd.value=0; break;
        case CMD_WEAK:       g_cmd.param=0x7B; g_cmd.value=0; break;
        case CMD_MEDIUM:     g_cmd.param=0x7B; g_cmd.value=1; break;
        case CMD_STRONG:     g_cmd.param=0x7B; g_cmd.value=2; break;
        default: return -1;
    }
    g_cmd.len = 4; g_cmd.addr = 0x20; g_cmd.rlen = 0x03;
    uint8_t *ptr = g_Query.buf;
    memcpy(ptr, g_h0, sizeof(g_h0)); ptr += sizeof(g_h0);
    memcpy(ptr, g_hc, sizeof(g_hc)); ptr += sizeof(g_hc);
    memcpy(ptr, &g_cmd, sizeof(g_cmd)); ptr += sizeof(g_cmd);
    g_Query.DataLen = (uint8_t)(ptr - &g_Query.buf[2]);
    g_Query.cs = protocol_calc_cs(&g_Query.buf[2], g_Query.DataLen);
    g_Query.prepared = 1;
    return 0;
#endif
}

int protocol_write_query(int (*uart_write_fn)(const uint8_t *data, int len)) {
#if M365_DISABLE_TX
    if (g_Query.prepared) {
        // Drop prepared query silently (sniffer mode). Clear prepared so scheduler advances.
        g_Query.prepared = 0;
        static int once=0; if(!once){ once=1; printf("[TX-DISABLED] Master TX suppressed (M365_DISABLE_TX=1)\n"); }
    }
    (void)uart_write_fn; return 0;
#else
    if (!g_Query.prepared) return 0;
    int n1 = uart_write_fn(g_Query.buf, g_Query.DataLen + 2); // includes sync bytes
    if (n1 < 0) return n1;
    uint8_t csLE[2]; csLE[0] = (uint8_t)(g_Query.cs & 0xFF); csLE[1] = (uint8_t)(g_Query.cs >> 8);
    int n2 = uart_write_fn(csLE, 2);
    if (n2 < 0) return n2;
    g_Query.prepared = 0;
    return n1 + n2;
#endif
}
