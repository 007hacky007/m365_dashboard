#include "m365_port.h"
#include "arduino_compat.h"
#include "comms_port.h"
#include "protocol_state.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "COMMS";

// UART configuration (moved up so poller helpers can use UART_PORT)
#include <driver/uart.h>
#define UART_PORT UART_NUM_1
#define UART_RX   20
#define UART_TX   21
#define UART_BAUD 115200

// Safety: ensure macro visible (some build variants showed undefined)
#ifndef UART_PORT
#define UART_PORT UART_NUM_1
#endif

// =============================
// Integrated 0x31 BMS SoC poller (non-blocking) per new requirements
// Query frame: 55 AA 03 22 01 31 0A 9E FF (precomputed checksum bytes 0x0A 0x9E)
// Response expected: addr(src)=0x25, dst=0x01, cmd(id)=0x31
// Timing: base 4 Hz (250ms), back off to 1 Hz after 3 consecutive timeouts.
// Retry: up to 2 retries (total 3 attempts) with 80ms timeout each, inter-TX gap >=20ms.
// Staleness: UI may treat data stale if (now - g_bms31_lastOkMs) > 1500ms.
#ifndef PASSIVE_ONLY
#define PASSIVE_ONLY 1  // ensure legacy dynamic query system stays disabled
#endif

#define BMS_SOC_BASE_PERIOD_MS   250UL
#define BMS_SOC_BACKOFF_PERIOD_MS 1000UL
#define BMS_SOC_INTER_TX_GAP_MS   20UL
#define BMS_SOC_REPLY_TIMEOUT_MS  80UL
#define BMS_SOC_MAX_RETRIES       2   // additional retries (total attempts = 1 + 2)

#ifndef BMS_POLL_DEBUG
#define BMS_POLL_DEBUG 0
#endif

static const uint8_t s_bmsReq31[] = { 0x55,0xAA,0x03,0x22,0x01,0x31,0x0A,0x9E,0xFF };

typedef struct {
    uint8_t  pending;            // awaiting reply
    uint8_t  retries;            // retries already used
    uint8_t  consecutiveTimeouts;// consecutive cycles with no valid reply
    uint32_t lastSendMs;         // last scheduled send start (for period)
    uint32_t lastTxMs;           // last actual TX (for inter-frame gap)
    uint32_t pendingSendMs;      // ms when current attempt sent
    uint32_t currentPeriodMs;    // adaptive current period
} bms_soc_poll_state_t;
static bms_soc_poll_state_t g_bmsPoll = {0,0,0,0,0,0,BMS_SOC_BASE_PERIOD_MS};

static void bms_soc_poll_handle_valid_reply(uint32_t nowMs){
    extern volatile uint32_t g_bms31_lastOkMs; g_bms31_lastOkMs = nowMs;
    g_bmsPoll.pending = 0; g_bmsPoll.retries = 0; g_bmsPoll.consecutiveTimeouts = 0;
    g_bmsPoll.currentPeriodMs = BMS_SOC_BASE_PERIOD_MS;
#if BMS_POLL_DEBUG
    ESP_LOGI(TAG, "BMS31 ok (period=%lu)", (unsigned long)g_bmsPoll.currentPeriodMs);
#endif
}

static void bms_soc_poll_send(uint32_t nowMs){
    uart_write_bytes(UART_PORT, (const char*)s_bmsReq31, sizeof(s_bmsReq31));
    g_bmsPoll.lastTxMs = nowMs; g_bmsPoll.pendingSendMs = nowMs;
#if BMS_POLL_DEBUG
    ESP_LOGI(TAG, "TX 0x31 req (try %u)", (unsigned)g_bmsPoll.retries);
#endif
}

static void bms_soc_poll_tick(uint32_t nowMs){
    // Timeout / retry logic
    if (g_bmsPoll.pending){
        if ((int32_t)(nowMs - g_bmsPoll.pendingSendMs) >= (int32_t)BMS_SOC_REPLY_TIMEOUT_MS){
            if (g_bmsPoll.retries < BMS_SOC_MAX_RETRIES){
                if ((int32_t)(nowMs - g_bmsPoll.lastTxMs) >= (int32_t)BMS_SOC_INTER_TX_GAP_MS){
                    g_bmsPoll.retries++;
                    bms_soc_poll_send(nowMs);
                }
            } else {
                // Give up this cycle
                g_bmsPoll.pending = 0;
                g_bmsPoll.consecutiveTimeouts++;
                g_bmsPoll.currentPeriodMs = (g_bmsPoll.consecutiveTimeouts >= 3)? BMS_SOC_BACKOFF_PERIOD_MS : BMS_SOC_BASE_PERIOD_MS;
#if BMS_POLL_DEBUG
                ESP_LOGW(TAG, "BMS31 timeout (consec=%u -> period=%lu)", (unsigned)g_bmsPoll.consecutiveTimeouts, (unsigned long)g_bmsPoll.currentPeriodMs);
#endif
            }
        }
    }
    // Schedule next request if no pending
    if (!g_bmsPoll.pending){
        if ((int32_t)(nowMs - g_bmsPoll.lastSendMs) >= (int32_t)g_bmsPoll.currentPeriodMs){
            if ((int32_t)(nowMs - g_bmsPoll.lastTxMs) >= (int32_t)BMS_SOC_INTER_TX_GAP_MS){
                g_bmsPoll.pending = 1; g_bmsPoll.retries = 0; g_bmsPoll.lastSendMs = nowMs;
                bms_soc_poll_send(nowMs);
            }
        }
    }
}

// Ensure full sniff enabled even if not pulled in from other TU
#ifndef M365_FULL_SNIFF
#define M365_FULL_SNIFF 1
#endif

// Enable extended diagnostics for invalid (discarded) frames (len too large/small)
#ifndef DEBUG_INVALID_FRAME
#define DEBUG_INVALID_FRAME 0  // disable heavy invalid-frame diagnostics for performance
#endif

// Only compile invalid frame history helpers when enabled
#if DEBUG_INVALID_FRAME
// Enable full capture of oversize (invalid length) frames for analysis
#ifndef DEBUG_CAPTURE_OVERSIZE
#define DEBUG_CAPTURE_OVERSIZE 1
#endif
// Expanded ring buffer (512 bytes) for broader context around invalid frames.
static uint8_t g_rxHistory[512];
static uint16_t g_rxHistIdx = 0; // modulo length (power-of-two size)
static inline void rxhist_push(uint8_t b){ g_rxHistory[g_rxHistIdx++ & (sizeof(g_rxHistory)-1)] = b; }
static void dump_invalid_frame_context(uint8_t badLen, uint8_t hdrBytes){
    // Print last up to 128 bytes (or fewer if not yet filled)
    const uint16_t DUMP = 128;
    char line[3*16+32];
    ESP_LOGW(TAG, "Invalid frame len=%u hdrBytes=%u (dumping last %u bytes)", badLen, hdrBytes, DUMP);
    uint16_t start = (g_rxHistIdx >= DUMP)? (g_rxHistIdx - DUMP) : 0;
    for(uint16_t i=start;i<g_rxHistIdx;i++){
        uint8_t b = g_rxHistory[i & (sizeof(g_rxHistory)-1)];
        int pos = (i-start)%16;
        if(pos==0){
            snprintf(line,sizeof(line),"%04u:", (unsigned)(i-start));
        }
        int off = strlen(line);
        snprintf(line+off,sizeof(line)-off," %02X", b);
        if (pos==15){ ESP_LOGW(TAG, "%s", line); line[0]='\0'; }
    }
    if(line[0]) ESP_LOGW(TAG, "%s", line);
}
#endif

// ================= High‑performance streaming parser configuration =================
// Large accumulation buffer (must hold bursts); requirement ≥2048
#ifndef M365_RX_BUFFER_SIZE
#define M365_RX_BUFFER_SIZE 4096
#endif
// Maximum acceptable declared frame length (payload+2cs). Real frames observed < 64; keep generous to avoid discard cascades.
#ifndef M365_MAX_FRAME_LEN
#define M365_MAX_FRAME_LEN 192
#endif
// Optional verbose per-frame logging (disabled for performance)
#ifndef M365_FRAME_LOG
#define M365_FRAME_LOG 0
#endif

static uint8_t g_rxAccum[M365_RX_BUFFER_SIZE];
static size_t  g_rxCount = 0;          // bytes currently in accumulation buffer

// Parser stats (extended)
typedef struct {
    uint32_t frames_ok;
    uint32_t frames_len_invalid;
    uint32_t frames_checksum_fail;
    uint32_t frames_partial_retained; // times loop exited with incomplete frame
    uint32_t resync_slides;           // slide steps taken (salvage attempts)
} fast_stats_t;
static fast_stats_t g_fast = {0};

// We reuse global g_AnswerHeader from protocol_state
volatile uint32_t g_lastBusDataMs = 0;
volatile uint32_t g_firstBusDataMs = 0;
volatile bool g_busEverSeen = false; // activity flags

static void note_bus_activity(uint32_t now) {
    g_lastBusDataMs = now;
    if (!g_busEverSeen) { g_busEverSeen = true; g_firstBusDataMs = now; }
}

static uint32_t g_lastStatsLogMs = 0;

// M365 Bus Frame Parser
static void parse_m365_frame(uint32_t timestamp_us, uint8_t len, uint8_t src, uint8_t dst, uint8_t cmd, const uint8_t *payload) {
#if M365_FRAME_LOG
    (void)timestamp_us; (void)len; (void)src; (void)dst; (void)cmd; (void)payload;
#endif
}

// Drain already buffered UART bytes into accumulation buffer
static void uart_drain_into_buffer(void){
    size_t avail = 0;
    while (uart_get_buffered_data_len(UART_PORT, &avail) == ESP_OK && avail > 0) {
        if (g_rxCount >= M365_RX_BUFFER_SIZE) {
            // Overflow: drop oldest half to make room (salvage strategy)
            size_t drop = M365_RX_BUFFER_SIZE/2;
            memmove(g_rxAccum, g_rxAccum + drop, M365_RX_BUFFER_SIZE - drop);
            g_rxCount -= drop;
        }
        size_t space = M365_RX_BUFFER_SIZE - g_rxCount;
        if (!space) break;
        size_t toRead = (avail < space) ? avail : space;
        int r = uart_read_bytes(UART_PORT, g_rxAccum + g_rxCount, toRead, 0);
        if (r > 0) { g_rxCount += (size_t)r; }
        else break; // nothing read
        // Loop again to capture newly arrived burst bytes
    }
}

// Parse any complete frames inside accumulation buffer (sliding window salvage)
static void process_rx_buffer(void){
    size_t i = 0; uint32_t nowMs = millis();
    while (i + 2 + sizeof(ANSWER_HEADER) <= g_rxCount) {
        if (g_rxAccum[i] != 0x55 || g_rxAccum[i+1] != 0xAA) { i++; g_fast.resync_slides++; continue; }
        // Header present?
        if (i + 2 + sizeof(ANSWER_HEADER) > g_rxCount) break; // need more bytes
        uint8_t lenDecl = g_rxAccum[i+2];
        if (lenDecl < 2 || lenDecl > M365_MAX_FRAME_LEN) { i++; g_fast.frames_len_invalid++; g_fast.resync_slides++; continue; }
        size_t totalNeed = 2 + sizeof(ANSWER_HEADER) + (size_t)lenDecl;
        if (i + totalNeed > g_rxCount) { g_fast.frames_partial_retained++; break; } // partial frame waiting
        // Compute checksum (0xFFFF - sum(header bytes + payload (excluding final 2)))
        uint16_t csWork = 0xFFFF;
        const uint8_t *hdr = &g_rxAccum[i+2];
        for (int h=0; h< (int)sizeof(ANSWER_HEADER); ++h) csWork -= hdr[h];
        const uint8_t *payload = &g_rxAccum[i+2+sizeof(ANSWER_HEADER)];
        uint8_t payloadBytes = (lenDecl >=2)? (uint8_t)(lenDecl - 2) : 0; // bytes before checksum
        for (uint8_t p=0; p<payloadBytes; ++p) csWork -= payload[p];
        const uint8_t *csPtr = payload + payloadBytes; // last two bytes
        uint16_t rxcs = (uint16_t)csPtr[0] | ((uint16_t)csPtr[1] << 8);
        if (rxcs == csWork) {
            // Fill global header (len, addr, hz, cmd) order preserved
            memcpy(&g_AnswerHeader, hdr, sizeof(ANSWER_HEADER));
            note_bus_activity(nowMs);
            protocol_process_packet(payload, g_AnswerHeader.len + sizeof(ANSWER_HEADER), nowMs);
            parse_m365_frame((uint32_t)nowMs, g_AnswerHeader.len, g_AnswerHeader.hz, g_AnswerHeader.addr, g_AnswerHeader.cmd, payload);
            if (g_AnswerHeader.addr == 0x25 && g_AnswerHeader.cmd == 0x31) {
                bms_soc_poll_handle_valid_reply(nowMs);
            }
            g_fast.frames_ok++;
            i += totalNeed; // advance past frame
        } else {
            g_fast.frames_checksum_fail++;
            // Salvage: slide forward one byte (search inner sync)
            i++; g_fast.resync_slides++;
        }
    }
    if (i > 0) {
        size_t remain = g_rxCount - i;
        if (remain && i < g_rxCount) memmove(g_rxAccum, g_rxAccum + i, remain);
        g_rxCount = remain;
    }
}

static void uart_task(void *arg) {
    while (1) {
        // 1. Drain UART quickly (no prints)
        uart_drain_into_buffer();
        // 2. Parse accumulated bytes into frames
        process_rx_buffer();
        // 3. Poller tick
        bms_soc_poll_tick(millis());
        // Periodic lightweight stats (every 5s)
        uint32_t nowMs = millis();
        if (nowMs - g_lastStatsLogMs > 5000) {
            g_lastStatsLogMs = nowMs;
            ESP_LOGI(TAG, "STAT ok=%lu badLen=%lu csFail=%lu partial=%lu slides=%lu rxPending=%u",
                     (unsigned long)g_fast.frames_ok,
                     (unsigned long)g_fast.frames_len_invalid,
                     (unsigned long)g_fast.frames_checksum_fail,
                     (unsigned long)g_fast.frames_partial_retained,
                     (unsigned long)g_fast.resync_slides,
                     (unsigned)g_rxCount);
        }
        vTaskDelay(1);
    }
}

// Provide function used as callback for protocol_write_query
int uart_send_bytes(const uint8_t *d, int len) {
    return uart_write_bytes(UART_PORT, (const char*)d, len);
}

void comms_port_init(void) {
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // Only install if not already present (ESP-IDF keeps a driver context; reinstall logs error)
    if (uart_is_driver_installed(UART_PORT) == false) {
        esp_err_t r = uart_driver_install(UART_PORT, 4096, 0, 0, NULL, 0); // larger HW RX buffer
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(r));
            return;
        }
        uart_param_config(UART_PORT, &cfg);
        uart_set_pin(UART_PORT, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    } else {
        ESP_LOGW(TAG, "UART driver already present; skipping install");
    }
#ifdef BUS_TX_INVERT
    uart_set_line_inverse(UART_PORT, UART_SIGNAL_TXD_INV);
#endif
}

void start_comms_task(void) {
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 5, NULL, 0);
}
