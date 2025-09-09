#include "range_estimator.h"
#include "protocol_state.h"
#include "config.h"
#include "arduino_compat.h" // for millis()
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"

// Ported from Arduino range_estimator.cpp.
// Persistence implemented via NVS (namespace: "range") storing a ring of slots
// analogous to the EEPROM layout on Arduino. Each slot carries a CRC so we can
// recover after partial writes.

// Config constants
static const float KM_PER_PCT_INIT = RANGE_KM_PER_PCT_INIT;
static const float KM_PER_PCT_MIN  = RANGE_KM_PER_PCT_MIN;
static const float KM_PER_PCT_MAX  = RANGE_KM_PER_PCT_MAX;
static const float EMA_ALPHA       = RANGE_EMA_ALPHA;
static const float EOD_BETA        = RANGE_EOD_BETA;

// Runtime model state
static float   g_km_per_pct = KM_PER_PCT_INIT;
static uint8_t g_ref_soc = 0;        // window start SOC
static uint32_t g_ref_odo_m = 0;     // meters at start
static uint16_t g_ref_time_s = 0;    // riding time at start
static bool g_have_ref = false;
static uint8_t g_last_soc = 0;       // last seen SOC
static uint32_t g_last_odo_m = 0;    // last seen odo (m)
static bool g_dirty = false;         // need checkpoint
static uint32_t g_last_checkpoint_ms = 0;
static uint8_t g_full_soc_seen = 100; // highest SOC seen this cycle
static uint8_t g_midride_written = 0; // only one mid-ride write
static uint32_t g_last_power_on_time_s = 0; // for detection of reset

// ---------------- Persistence (ring slots in NVS) -----------------
// Match Arduino layout: float km_per_pct; uint8_t last_soc; uint32_t last_odo_m; uint16_t seq; uint16_t crc;
typedef struct __attribute__((packed)) {
    float km_per_pct;
    uint8_t last_soc;
    uint32_t last_odo_m;
    uint16_t seq;
    uint16_t crc; // CRC16-CCITT over first (sizeof-2) bytes
} RangeSlot;

#define RANGE_RING_SLOTS 10
static RangeSlot g_slots[RANGE_RING_SLOTS];
static uint16_t g_next_seq = 1; // next sequence index
static bool g_persist_loaded = false;

// CRC16-CCITT (FALSE)
static uint16_t crc16_ccitt_false(const uint8_t* data, size_t len, uint16_t crc){
    crc = (crc==0)?0xFFFF:crc;
    for(size_t i=0;i<len;i++){
        crc ^= (uint16_t)data[i] << 8;
        for(uint8_t b=0;b<8;b++){
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021; else crc <<= 1;
        }
    }
    return crc;
}

static bool slot_valid(const RangeSlot *s){
    if (!s) return false;
    uint16_t c = crc16_ccitt_false((const uint8_t*)s, sizeof(RangeSlot)-2, 0xFFFF);
    if (c != s->crc) return false;
    if (s->km_per_pct < KM_PER_PCT_MIN || s->km_per_pct > KM_PER_PCT_MAX) return false;
    return true;
}

// Forward declaration for helper used before its definition
static inline void clampLearned(void);

static esp_err_t nvs_ensure_init(void){
    static bool inited=false; if (inited) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND){
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err==ESP_OK) inited=true;
    return err;
}

static void persist_load(void){
    if (g_persist_loaded) return; // idempotent
    if (nvs_ensure_init()!=ESP_OK) return;
    nvs_handle_t h; if (nvs_open("range", NVS_READWRITE, &h)!=ESP_OK){ return; }
    size_t need = sizeof(g_slots);
    memset(g_slots, 0, sizeof(g_slots));
    esp_err_t err = nvs_get_blob(h, "slots", g_slots, &need);
    if (err != ESP_OK || need != sizeof(g_slots)){
        // First run or size mismatch: leave defaults
        nvs_close(h); g_persist_loaded = true; return;
    }
    // Find newest valid slot (handle wrap-around). Use highest seq considering uint16 diff.
    int best = -1; uint16_t bestSeq = 0;
    for(int i=0;i<RANGE_RING_SLOTS;i++){
        if (slot_valid(&g_slots[i])){
            if (best < 0) { best = i; bestSeq = g_slots[i].seq; }
            else {
                uint16_t diff = (uint16_t)(g_slots[i].seq - bestSeq);
                if (diff < 0x8000) { best = i; bestSeq = g_slots[i].seq; }
            }
        }
    }
    if (best >= 0){
        g_km_per_pct = g_slots[best].km_per_pct; clampLearned();
        g_last_soc = g_slots[best].last_soc;
        g_last_odo_m = g_slots[best].last_odo_m;
        g_next_seq = bestSeq + 1;
    }
    nvs_close(h);
    g_persist_loaded = true;
}

static void persist_store_current(uint8_t reason_soc){
    if (nvs_ensure_init()!=ESP_OK) return;
    nvs_handle_t h; if (nvs_open("range", NVS_READWRITE, &h)!=ESP_OK) return;
    // Prepare slot
    RangeSlot s = {0};
    s.km_per_pct = g_km_per_pct;
    s.last_soc = reason_soc;
    s.last_odo_m = g_last_odo_m;
    s.seq = g_next_seq++;
    s.crc = crc16_ccitt_false((const uint8_t*)&s, sizeof(RangeSlot)-2, 0xFFFF);
    g_slots[s.seq % RANGE_RING_SLOTS] = s; // ring placement
    esp_err_t err = nvs_set_blob(h, "slots", g_slots, sizeof(g_slots));
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// Helpers
static inline uint32_t odo_to_m(uint32_t mileageTotal_centi_km){ return mileageTotal_centi_km * 10UL; }
static inline void clampLearned(void){
    if (g_km_per_pct < KM_PER_PCT_MIN) g_km_per_pct = KM_PER_PCT_MIN;
    if (g_km_per_pct > KM_PER_PCT_MAX) g_km_per_pct = KM_PER_PCT_MAX;
}
static inline void setRef(uint8_t soc, uint32_t odo_m){ g_ref_soc = soc; g_ref_odo_m = odo_m; g_ref_time_s = g_S23C3A.ridingTime; g_have_ref = true; }
static inline void clearRef(){ g_have_ref = false; }
static inline void scheduleDirty(){ g_dirty = true; }

void range_init(void){
    persist_load();
    if (!g_persist_loaded){
        g_km_per_pct = KM_PER_PCT_INIT;
        g_last_soc = g_S25C31.remainPercent;
        g_last_odo_m = odo_to_m(g_S23CB0.mileageTotal);
    } else if (g_last_odo_m == 0){
        // If we loaded but stored odo was zero, seed with current
        g_last_odo_m = odo_to_m(g_S23CB0.mileageTotal);
    }
    clampLearned();
    g_full_soc_seen = g_last_soc;
    g_last_checkpoint_ms = millis();
    clearRef();
}

float range_get_km_per_pct(void){ return g_km_per_pct; }
float range_get_estimate_km(void){ float soc = (float)g_S25C31.remainPercent; float est = soc * g_km_per_pct; return est < 0 ? 0.f : est; }

void range_checkpoint_if_needed(void){
    if (g_dirty){
        persist_store_current(g_S25C31.remainPercent);
        g_dirty = false; g_last_checkpoint_ms = millis();
    }
}

void range_tick(void){
    uint8_t soc = g_S25C31.remainPercent;
    uint32_t odo_m = odo_to_m(g_S23CB0.mileageTotal);

    // Power-on time wrap detection (if powerOnTime decreases -> reset trip windows) not strictly necessary.
    uint32_t pot_s = g_S23C3A.powerOnTime;
    if (g_last_power_on_time_s != 0 && pot_s < g_last_power_on_time_s){ clearRef(); }
    g_last_power_on_time_s = pot_s;

    int32_t d_odo = (int32_t)(odo_m - g_last_odo_m);
    if (d_odo < 0){ clearRef(); d_odo = 0; }

    if (soc > g_full_soc_seen) g_full_soc_seen = soc;
    if (g_last_soc != 0 && soc > g_last_soc){ clearRef(); }
    if (!g_have_ref) setRef(soc, odo_m);

    uint8_t soc_drop = (g_ref_soc > soc) ? (g_ref_soc - soc) : 0;
    uint32_t dist_m = (odo_m >= g_ref_odo_m) ? (odo_m - g_ref_odo_m) : 0;
    uint16_t dt_s = (uint16_t)((g_S23C3A.ridingTime >= g_ref_time_s) ? (g_S23C3A.ridingTime - g_ref_time_s) : 0);
    bool speed_ok = false;
    if (dt_s > 0){ float mps = (float)dist_m / (float)dt_s; speed_ok = (mps >= 0.833f); }
    bool learn_ok = (soc_drop >= 2) && (dist_m > 50) && speed_ok;
    if (learn_ok){
        float delta_km = (float)dist_m / 1000.0f;
        float km_per_pct = delta_km / (float)soc_drop;
        if (km_per_pct < KM_PER_PCT_MIN) km_per_pct = KM_PER_PCT_MIN;
        if (km_per_pct > KM_PER_PCT_MAX) km_per_pct = KM_PER_PCT_MAX;
        float old = g_km_per_pct;
        g_km_per_pct = (1.0f - EMA_ALPHA) * g_km_per_pct + EMA_ALPHA * km_per_pct;
        clampLearned();
        float delta = g_km_per_pct - old; if (delta < 0) delta = -delta;
        if (delta >= 0.01f || delta >= (0.03f * old)) scheduleDirty();
        setRef(soc, odo_m);
    }

    // End of discharge strong correction
    uint8_t soc_drop_full = (uint8_t)((g_full_soc_seen > soc) ? (g_full_soc_seen - soc) : 0);
    if (soc_drop_full > 80){
        uint32_t trip_ckm = g_S23CB0.mileageCurrent; // 0.01 km units
        if (trip_ckm >= 100){ // >=1.00 km
            float total_km = (float)trip_ckm / 100.0f;
            float eod_km_per_pct = total_km / (float)soc_drop_full;
            if (eod_km_per_pct < KM_PER_PCT_MIN) eod_km_per_pct = KM_PER_PCT_MIN;
            if (eod_km_per_pct > KM_PER_PCT_MAX) eod_km_per_pct = KM_PER_PCT_MAX;
            float old = g_km_per_pct;
            g_km_per_pct = (1.0f - EOD_BETA) * g_km_per_pct + EOD_BETA * eod_km_per_pct;
            clampLearned();
            float delta = g_km_per_pct - old; if (delta < 0) delta = -delta;
            if (delta >= 0.01f || delta >= (0.05f * old)) scheduleDirty();
            clearRef();
            if (g_dirty) range_checkpoint_if_needed();
        }
    }

    if (soc >= 99){
    if (g_dirty) range_checkpoint_if_needed();
        g_full_soc_seen = soc; clearRef();
    }

    // Mid-ride write placeholder (time + distance) — persistence pending
    uint32_t now = millis();
    bool time_ok = (now - g_last_checkpoint_ms) >= (30UL * 60UL * 1000UL);
    uint32_t trip_ckm = g_S23CB0.mileageCurrent;
    bool dist_ok = (trip_ckm >= 1000); // 10.00 km
    if (!g_midride_written && g_dirty && (soc <= 50) && time_ok && dist_ok){
        range_checkpoint_if_needed();
        g_midride_written = 1;
    }

    // Update last metrics
    g_last_soc = soc;
    g_last_odo_m = odo_m;
}
