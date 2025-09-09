#pragma once
#include <stdint.h>
#include <stdbool.h>

// ================= Protocol Data Structures (ported from Arduino defines.h) =================

// Query building structure
typedef struct {
    uint8_t prepared;      // 0/1 flag
    uint8_t DataLen;       // length of frame body (from buf[2] .. before checksum) used for CS
    uint8_t buf[16];       // raw frame buffer (includes sync 0x55 0xAA at [0],[1])
    uint16_t cs;           // checksum (0xFFFF - sum of bytes buf[2..(2+DataLen-1)])
    uint8_t _dynQueries[5];
    uint8_t _dynSize;      // number of dynamic queries
} QUERY_t;

// Answer header from scooter (len counts payload + 2 checksum bytes)
// Answer header byte order on-wire (per original Arduino logic): len, addr, hz, cmd
typedef struct __attribute__((packed)) {
    uint8_t len, addr, hz, cmd;
} ANSWER_HEADER;

// Incoming data frames (packed to match on-wire layouts)
typedef struct __attribute__((packed)) { uint8_t hz1, throttle, brake, hz2, hz3; } A20C00HZ65;
typedef struct __attribute__((packed)) { uint16_t remainCapacity; uint8_t remainPercent, u4; int16_t current, voltage; uint8_t temp1, temp2; } A25C31;
typedef struct __attribute__((packed)) { int16_t i1; } A23C3E;
typedef struct __attribute__((packed)) { uint8_t u1[10]; int16_t speed; uint16_t averageSpeed; uint32_t mileageTotal; uint16_t mileageCurrent; uint16_t elapsedPowerOnTime; int16_t mainframeTemp; uint8_t u2[8]; } A23CB0;
typedef struct __attribute__((packed)) { uint8_t u1,u2,u3,u4; uint16_t remainMileage; } A23C23;
typedef struct __attribute__((packed)) { uint16_t powerOnTime, ridingTime; } A23C3A;
typedef struct __attribute__((packed)) { uint16_t tripTimeSeconds; } A23C3B; // register 0x3B (trip time)
typedef struct __attribute__((packed)) { uint16_t firmware; } A23C1A;       // 0x1A
typedef struct __attribute__((packed)) { uint16_t bmsVer, var104; } A23C67;  // 0x67 (two uint16)
typedef struct __attribute__((packed)) { uint16_t value; } A23C69;          // 0x69 unknown (2 bytes)
typedef struct __attribute__((packed)) { uint16_t kers; } A23C7B;           // 0x7B KERS strength
typedef struct __attribute__((packed)) { uint16_t cruise; } A23C7C;         // 0x7C Cruise status
typedef struct __attribute__((packed)) { uint16_t tail; } A23C7D;           // 0x7D Tail light state
typedef struct { char serial[15]; uint8_t len; } A23C10;                    // 0x10 serial (up to 14 chars)
// Long realtime block (addr 0x23 cmd 0xAF) – observed len byte 0x1C (26 payload bytes). Raw exploratory container.
// 0xAF long realtime frame (reply to 0x20 / cmd 0x23 request)
// Layout (26 byte payload):
// [0..3]   BE u32 distance_m
// [4..7]   BE u32 mode_index (constant 8 observed)
// [8..9]   LE u16 speed_limit_1_dms (deci-m/s) -> m/s = /10, km/h = *3.6
// [10..11] LE u16 speed_limit_2_dms
// [12..15] BE u32 reserved
// [16..19] BE u32 flags_ts_raw (opaque)
// [20..23] BE u32 ticks (timebase counter)
// [24..25] LE u16 controller_temp_deciC (0.1 C)
typedef struct __attribute__((packed)) {
    uint32_t distance_m;
    uint32_t mode_index;
    uint16_t speed_limit1_dms; // deci m/s
    uint16_t speed_limit2_dms;
    uint32_t reserved;
    uint32_t flags_ts_raw;
    uint32_t ticks;
    uint16_t controller_temp_dC; // deci C
    uint32_t frameCount;         // internal counter (not from frame)
    uint8_t  raw[26];            // preserve original bytes (debug)
} A23CAF;

// Battery extra frames
typedef struct { char serial[15]; uint8_t len; } A25C10;                    // batt serial 0x10
typedef struct __attribute__((packed)) { uint16_t bmsVersion; } A25C17;     // 0x17
typedef struct __attribute__((packed)) { uint16_t factoryCapacity_mAh; } A25C18; // 0x18
typedef struct __attribute__((packed)) { uint16_t dateRaw; } A25C20;        // 0x20 encoded Y/M/D
typedef struct __attribute__((packed)) { uint16_t cycles1, cycles2; } A25C1B;// 0x1B two counters
typedef struct __attribute__((packed)) { uint16_t percent; } A25C32;        // 0x32 percent (alt)
typedef struct __attribute__((packed)) { int16_t current_cA; } A25C33;      // 0x33 current *100 (signed)
typedef struct __attribute__((packed)) { uint16_t voltage_dV; } A25C34;     // 0x34 voltage *10 (decivolts)
typedef struct __attribute__((packed)) { uint8_t t1, t2; } A25C35;          // 0x35 temps raw (offset +20)
typedef struct __attribute__((packed)) { uint16_t health; } A25C3B;         // 0x3B battery health
typedef struct __attribute__((packed)) { int16_t c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15; } A25C40;
// 0x30 extended battery status frame (observed len=24 payload bytes)
typedef struct __attribute__((packed)) {
    uint16_t mode;                 // [0..1]
    uint16_t remainCapacity_mAh;   // [2..3]
    uint16_t speedLimitRaw;        // [4..5] (units 0.1 m/s -> value/10 = m/s)
    int16_t  current_mA;           // [6..7] signed mA (hypothesis)
    uint16_t voltage_cV;           // [8..9] centivolts (value/100 -> V)
    uint8_t  temp1_raw;            // [10] raw (+20 offset)
    uint8_t  temp2_raw;            // [11] raw (+20 offset)
    uint8_t  reserved[6];          // [12..17]
    uint16_t capDup1_mAh;          // [18..19] duplicate capacity
    uint16_t capDup2_mAh;          // [20..21] duplicate capacity
    uint16_t speedLimitDup;        // [22..23] duplicate speed limit
} A25C30;

// Command structure
typedef struct __attribute__((packed)) { uint8_t len, addr, rlen, param; int16_t value; } CMD_t;

// END20T structure (dynamic part for type-2 queries)
typedef struct __attribute__((packed)) { uint8_t hz, th, br; } END20T_t;

// ================= Globals =================

extern QUERY_t      g_Query;
extern volatile uint8_t   g_NewDataFlag;
extern volatile bool      g_Hibernate;
extern ANSWER_HEADER g_AnswerHeader;
extern A20C00HZ65    g_S20C00HZ65;
extern A25C31        g_S25C31;
extern A23C3E        g_S23C3E;
extern A23CB0        g_S23CB0;
extern A23C23        g_S23C23;
extern A23C3A        g_S23C3A;
extern A25C40        g_S25C40;
extern A23C3B        g_S23C3B;
extern A23C1A        g_S23C1A;
extern A23C67        g_S23C67;
extern A23C69        g_S23C69;
extern A23C7B        g_S23C7B;
extern A23C7C        g_S23C7C;
extern A23C7D        g_S23C7D;
extern A23C10        g_S23C10;
extern A23CAF        g_S23CAF;
extern A25C10        g_S25C10;
extern A25C17        g_S25C17;
extern A25C18        g_S25C18;
extern A25C20        g_S25C20;
extern A25C1B        g_S25C1B;
extern A25C32        g_S25C32;
extern A25C33        g_S25C33;
extern A25C34        g_S25C34;
extern A25C35        g_S25C35;
extern A25C3B        g_S25C3B;
extern A25C30        g_S25C30;
extern volatile bool g_batteryDataValid; // set true once first battery frame (even partial) parsed
extern volatile bool g_batteryFullFrame; // set true only after full A25C31 (len=10) parsed
// Mainframe temperature (tenths °C) decoded from addr 0x23 cmd 0xAF payload bytes 24-25 (little-endian)
extern volatile int16_t g_mainTempC10_AF;
extern volatile bool    g_mainTempAFValid;
// Trip distance (meters) extracted from A23CAF bytes [0..3] (big-endian)
extern volatile uint32_t g_afTripDistance_m;
// Last successful decoded 0x31 battery status response (ms). 0 when none yet.
extern volatile uint32_t g_bms31_lastOkMs;
extern CMD_t         g_cmd;
extern END20T_t      g_end20t;
// Diagnostics: last processed frame header + first 8 bytes payload
extern volatile ANSWER_HEADER g_lastDiagHeader;
extern volatile uint8_t g_lastDiagPayload[8];

extern const uint8_t g_commandsWeWillSend[3];
extern const uint8_t g_q[15];
extern const uint8_t g_l[15];
extern const uint8_t g_f[15];
extern const uint8_t g_h0[2];
extern const uint8_t g_h1[3];
extern const uint8_t g_h2[3];
extern const uint8_t g_hc[3];

// ================= API =================

// Called each time a full frame (header+payload+cs) is assembled.
void protocol_process_packet(const uint8_t *frameBuf, uint8_t totalLen, uint32_t nowMs);

// Prepare dynamic next query (rotates indices 1,8,10,14 per original logic)
void protocol_prepare_next_query(void);

// Prepare command frame (cruise, lights, KERS strength). Returns 0 on success.
int protocol_prepare_command(uint8_t cmdId);

// Attempt to write prepared query (including checksum) to UART.
// Returns number of bytes written or <0 error.
int protocol_write_query(int (*uart_write_fn)(const uint8_t *data, int len));

// Exposed checksum helper
uint16_t protocol_calc_cs(const uint8_t *data, uint8_t len);

// Command IDs (mirror Arduino enum)
enum { CMD_CRUISE_ON, CMD_CRUISE_OFF, CMD_LED_ON, CMD_LED_OFF, CMD_WEAK, CMD_MEDIUM, CMD_STRONG };

// Helper: total current scaled for multi-pack (returns centi-amps)
int16_t protocol_total_current_cA(void);
