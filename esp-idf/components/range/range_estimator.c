// Range estimator with persistence (NVS ring) ported from Arduino version.
// Keeps an exponential moving average model (km per percent SOC) and stores
// sparse checkpoints in a small wear-levelled ring in NVS.

#include "range_estimator.h"
#include "protocol_state.h"
#include "config.h"
#include "arduino_compat.h" // for millis()
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

// Ported from Arduino range_estimator.cpp (EEPROM persistence removed for now).
// Persistence TODO: use NVS namespace 'range' with a small blob storing ring slots.

// Config constants
static const float KM_PER_PCT_INIT = RANGE_KM_PER_PCT_INIT;
static const float KM_PER_PCT_MIN  = RANGE_KM_PER_PCT_MIN;
static const float KM_PER_PCT_MAX  = RANGE_KM_PER_PCT_MAX;
static const float EMA_ALPHA       = RANGE_EMA_ALPHA;
static const float EOD_BETA        = RANGE_EOD_BETA;

// Persistence ring parameters (mirrors Arduino design)
#define RANGE_RING_SLOTS 10
// Slot layout (packed) : km_per_pct(float,4) last_soc(u8,1) last_odo_m(u32,4) seq(u16,2) crc(u16,2) => 13 bytes
typedef struct __attribute__((packed)) {
    float    km_per_pct;
    uint8_t  last_soc;
    uint32_t last_odo_m;
    uint16_t seq;
    uint16_t crc; // CRC16-CCITT (FALSE) over preceding bytes
} range_slot_t;

_Static_assert(sizeof(range_slot_t)==13, "range_slot_t size mismatch");

// Stored blob format in NVS:
// struct { uint16_t next_seq; range_slot_t slots[RANGE_RING_SLOTS]; } with CRC over payload?
// For simplicity and atomicity we just store the raw array of slots and reconstruct next_seq by scanning.
// Key: "slots" namespace: "range"

static const char *TAG = "RANGE";
static const char *NVS_NAMESPACE = "range";
static const char *NVS_KEY_SLOTS = "slots"; // binary blob of range_slot_t[RANGE_RING_SLOTS]

// SRAM mirror of slots
static range_slot_t s_slots[RANGE_RING_SLOTS];
static uint16_t g_next_seq = 1; // next sequence number to use when writing

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
static uint8_t g_midride_written = 0; // guard for one mid-ride write
static uint32_t g_last_power_on_time_s = 0; // for detection of reset

// ---------------- CRC ----------------
static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len, uint16_t crc){
    for (size_t i=0;i<len;i++){
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b=0;b<8;b++){
            if (crc & 0x8000) crc = (crc<<1) ^ 0x1021; else crc <<=1;
        }
    }
    return crc;
}

static bool slot_valid(const range_slot_t *s){
    if (!s) return false;
    uint16_t calc = crc16_ccitt_false((const uint8_t*)s, sizeof(range_slot_t)-2, 0xFFFF);
    if (calc != s->crc) return false;
    if (s->km_per_pct < KM_PER_PCT_MIN || s->km_per_pct > KM_PER_PCT_MAX) return false;
    return true;
}

static int find_newest_valid_slot(uint16_t *out_seq){
    int newest = -1; uint16_t bestSeq = 0;
    for (int i=0;i<RANGE_RING_SLOTS;i++){
        if (slot_valid(&s_slots[i])){
            if (newest < 0 || (uint16_t)(s_slots[i].seq - bestSeq) < 0x8000){ // handle wrap
                newest = i; bestSeq = s_slots[i].seq;
            }
        }
    }
    if (out_seq) *out_seq = bestSeq;
    return newest;
}

// ---------------- NVS I/O ----------------
static esp_err_t load_slots_from_nvs(void){
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK){
        memset(s_slots, 0, sizeof(s_slots));
        return err;
    }
    size_t size = sizeof(s_slots);
    err = nvs_get_blob(h, NVS_KEY_SLOTS, s_slots, &size);
    nvs_close(h);
    if (err != ESP_OK){
        memset(s_slots, 0, sizeof(s_slots));
        return err;
    }
    if (size != sizeof(s_slots)){
        // size mismatch -> reset
        memset(s_slots, 0, sizeof(s_slots));
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t save_slot_to_nvs(const range_slot_t *slot){
    if (!slot) return ESP_ERR_INVALID_ARG;
    // Overwrite slot index derived from seq so it forms ring
    int idx = slot->seq % RANGE_RING_SLOTS;
    s_slots[idx] = *slot; // copy into mirror
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_SLOTS, s_slots, sizeof(s_slots));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Ensure NVS initialized (idempotent). Can be called many times safely.
static void ensure_nvs_init(void){
    static bool s_inited = false;
    if (s_inited) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND){
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK){
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    s_inited = true;
}

// Helpers
static inline uint32_t odo_to_m(uint32_t mileageTotal_centi_km){ return mileageTotal_centi_km * 10UL; }
static inline void clampLearned(){ if (g_km_per_pct < KM_PER_PCT_MIN) g_km_per_pct = KM_PER_PCT_MIN; if (g_km_per_pct > KM_PER_PCT_MAX) g_km_per_pct = KM_PER_PCT_MAX; }
static inline void setRef(uint8_t soc, uint32_t odo_m){ g_ref_soc = soc; g_ref_odo_m = odo_m; g_ref_time_s = g_S23C3A.ridingTime; g_have_ref = true; }
static inline void clearRef(){ g_have_ref = false; }
static inline void scheduleDirty(){ g_dirty = true; }

void range_init(void){
    ensure_nvs_init();
    // Load ring
    esp_err_t err = load_slots_from_nvs();
    if (err != ESP_OK){
        ESP_LOGW(TAG, "No existing slots (%s) starting fresh", esp_err_to_name(err));
    }
    uint16_t seq=0; int newest = find_newest_valid_slot(&seq);
    if (newest >= 0){
        g_km_per_pct = s_slots[newest].km_per_pct; clampLearned();
        g_next_seq = s_slots[newest].seq + 1;
        g_last_soc = s_slots[newest].last_soc;
        g_last_odo_m = s_slots[newest].last_odo_m;
        ESP_LOGI(TAG, "Loaded km_per_pct=%.3f seq=%u", g_km_per_pct, s_slots[newest].seq);
    } else {
        g_km_per_pct = KM_PER_PCT_INIT;
        g_next_seq = 1;
        g_last_soc = g_S25C31.remainPercent;
        g_last_odo_m = odo_to_m(g_S23CB0.mileageTotal);
        ESP_LOGI(TAG, "Range estimator defaults km_per_pct=%.3f", g_km_per_pct);
    }
    g_full_soc_seen = g_last_soc;
    g_last_checkpoint_ms = millis();
    clearRef();
}

float range_get_km_per_pct(void){ return g_km_per_pct; }
float range_get_estimate_km(void){ float soc = (float)g_S25C31.remainPercent; float est = soc * g_km_per_pct; return est < 0 ? 0.f : est; }

static void checkpoint(uint8_t reason_soc_hint){
    range_slot_t slot = {0};
    slot.km_per_pct = g_km_per_pct;
    slot.last_soc = reason_soc_hint;
    slot.last_odo_m = g_last_odo_m;
    slot.seq = g_next_seq++;
    slot.crc = 0; // temp for CRC calc
    slot.crc = crc16_ccitt_false((const uint8_t*)&slot, sizeof(range_slot_t)-2, 0xFFFF);
    esp_err_t err = save_slot_to_nvs(&slot);
    if (err == ESP_OK){ ESP_LOGD(TAG, "Checkpoint seq=%u km_per_pct=%.3f", slot.seq, slot.km_per_pct); }
    else { ESP_LOGE(TAG, "Checkpoint save failed: %s", esp_err_to_name(err)); }
    g_dirty = false; g_last_checkpoint_ms = millis();
}

void range_checkpoint_if_needed(void){
    if (g_dirty){
        checkpoint(g_S25C31.remainPercent);
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
        checkpoint(soc);
        g_midride_written = 1;
    }

    // Update last metrics
    g_last_soc = soc;
    g_last_odo_m = odo_m;
}
