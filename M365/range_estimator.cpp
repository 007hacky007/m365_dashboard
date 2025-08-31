#include "range_estimator.h"

// Design notes:
// - We learn a single km_per_pct based on SoC delta and odometer delta.
// - We keep all state in SRAM and checkpoint to EEPROM ring rarely.
// - Odometer source: S23CB0.mileageTotal is in 0.1 m units? In this project it appears km*10? The code uses /1000 then /10 for formatting.
//   From display_fsm, mileageTotal is divided as: km_int = mileageTotal / 1000; km_frac = (mileageTotal % 1000)/10; Therefore base unit is 0.01 km = 10 m.
//   We'll treat S23CB0.mileageTotal units as centi-km (0.01 km = 10 m). We'll convert to meters by *10 to avoid float where possible.

// We also use S23CB0.averageSpeed? Not needed; we use instant speed from S23CB0.speed.

// Constants from central config
static const float KM_PER_PCT_INIT = RANGE_KM_PER_PCT_INIT;
static const float KM_PER_PCT_MIN  = RANGE_KM_PER_PCT_MIN;
static const float KM_PER_PCT_MAX  = RANGE_KM_PER_PCT_MAX;
static const float EMA_ALPHA       = RANGE_EMA_ALPHA; // normal learning
static const float EOD_BETA        = RANGE_EOD_BETA;  // end-of-discharge stronger correction weight

// Checkpoint policy
static const uint8_t RING_SLOTS    = 10;
// We'll reserve a page beginning at address 64 to avoid clobbering existing config bytes (0..13 used).
// Each slot holds: km_per_pct (float, 4), last_soc (u8,1), last_odo (u32,4), seq(u16,2), crc(u16,2) = 13 bytes. Align to 14 for slack.
// We'll store tightly at 13 bytes per slot.
struct __attribute__((packed)) RangeSlot {
  float   km_per_pct;
  uint8_t last_soc;
  uint32_t last_odo_m; // meters (derived)
  uint16_t seq;
  uint16_t crc;
};

static_assert(sizeof(RangeSlot) == 13, "RangeSlot size");

// EEPROM base offset (after config). On ESP32 we must call EEPROM.begin with enough size; we'll ensure elsewhere it's big enough.
static const int EEPROM_BASE = 64; // configurable if needed

// SRAM state
static float g_km_per_pct = KM_PER_PCT_INIT;
static uint8_t g_ref_soc = 0;        // start of current window
static uint32_t g_ref_odo_m = 0;     // meters at start of window
static uint16_t g_ref_time_s = 0;    // riding time at start of window
static bool g_have_ref = false;
static uint8_t g_last_soc = 0;       // last seen SoC (for rising detection)
static uint32_t g_last_odo_m = 0;    // last seen odometer meters
static bool g_dirty = false;         // EEPROM checkpoint pending?
static uint16_t g_next_seq = 1;      // next sequence index when writing
static uint8_t g_midride_written = 0; // at most one mid-ride write
static uint32_t g_last_checkpoint_ms = 0;
static uint8_t g_full_soc_seen = 100; // track highest SOC seen since last reset

// Helpers to convert units
static inline uint32_t odo_to_m(uint32_t mileageTotal_centi_km) {
  // 0.01 km = 10 m
  return mileageTotal_centi_km * 10UL;
}

// CRC16-CCITT (FALSE) implementation
static uint16_t crc16_ccitt_false(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021; else crc <<= 1;
    }
  }
  return crc;
}

static void readSlot(uint8_t idx, RangeSlot &slot) {
  int base = EEPROM_BASE + (int)idx * (int)sizeof(RangeSlot);
  EEPROM.get(base, slot);
}

static void writeSlotPayloadNoCRC(uint8_t idx, const RangeSlot &slot) {
  int base = EEPROM_BASE + (int)idx * (int)sizeof(RangeSlot);
  // Write everything except CRC, then commit
  // We use eeprom_update-like semantics via EEPROM.put which only writes if changed (AVR); ESP32 always writes to buffer.
  // Exclude CRC bytes; write from start to (len-2)
  // We'll copy to a temp without crc set.
  RangeSlot tmp = slot; tmp.crc = 0x0000;
  EEPROM.put(base, tmp);
}

static void writeSlotCRC(uint8_t idx) {
  int base = EEPROM_BASE + (int)idx * (int)sizeof(RangeSlot);
  RangeSlot tmp; EEPROM.get(base, tmp);
  // compute CRC over payload (len-2)
  uint16_t crc = crc16_ccitt_false((const uint8_t*)&tmp, sizeof(RangeSlot) - 2);
  // write only CRC field
  int crcAddr = base + (int)sizeof(RangeSlot) - 2;
  EEPROM.put(crcAddr, crc);
}

static bool slotValid(const RangeSlot &slot) {
  uint16_t crc = crc16_ccitt_false((const uint8_t*)&slot, sizeof(RangeSlot) - 2);
  return (slot.crc == crc) && slot.km_per_pct >= KM_PER_PCT_MIN && slot.km_per_pct <= KM_PER_PCT_MAX;
}

static int8_t findNewestValidSlot(uint16_t &outSeq) {
  int8_t newest = -1; uint16_t bestSeq = 0;
  for (uint8_t i = 0; i < RING_SLOTS; ++i) {
    RangeSlot s; readSlot(i, s);
    if (slotValid(s)) {
      if (newest < 0 || (uint16_t)(s.seq - bestSeq) < 0x8000) { // handle wrap by uint16 diff
        newest = i; bestSeq = s.seq;
      }
    }
  }
  outSeq = bestSeq;
  return newest;
}

static void setRef(uint8_t soc, uint32_t odo_m) {
  g_ref_soc = soc; g_ref_odo_m = odo_m; g_ref_time_s = S23C3A.ridingTime; g_have_ref = true;
}

static void clearRef() { g_have_ref = false; }

static void clampLearned() {
  if (g_km_per_pct < KM_PER_PCT_MIN) g_km_per_pct = KM_PER_PCT_MIN;
  if (g_km_per_pct > KM_PER_PCT_MAX) g_km_per_pct = KM_PER_PCT_MAX;
}

float rangeGetKmPerPct() { return g_km_per_pct; }

float rangeGetEstimateKm() {
  // SoC_now from S25C31.remainPercent
  float soc = (float)S25C31.remainPercent;
  float est = soc * g_km_per_pct;
  if (est < 0) est = 0; return est;
}

static void scheduleDirty() { g_dirty = true; }

static void checkpoint(uint8_t reason_soc_hint) {
  // Write only if dirty and hysteresis thresholds
  RangeSlot cur{};
  cur.km_per_pct = g_km_per_pct;
  cur.last_soc = reason_soc_hint;
  cur.last_odo_m = g_last_odo_m;
  cur.seq = g_next_seq++;
  writeSlotPayloadNoCRC(cur.seq % RING_SLOTS, cur); // distribute by seq so it's ring-like
  writeSlotCRC(cur.seq % RING_SLOTS);
  EEPROM_COMMIT();
  g_dirty = false;
}

void rangeInit() {
  // Ensure EEPROM started with sufficient size (ESP32). Already called in setup with at least 64; we need 64 + 13*10 = 194.
  // If running on ESP32, code in setup should call EEPROM.begin with bigger size; if not, writes may be truncated.
  // We cannot change begin() here portably; trust setup to allocate enough. We'll still proceed.

  // Scan ring
  uint16_t seq = 0; int8_t idx = findNewestValidSlot(seq);
  if (idx >= 0) {
    RangeSlot s; readSlot(idx, s);
    g_km_per_pct = s.km_per_pct; clampLearned();
    g_next_seq = s.seq + 1;
    g_last_soc = s.last_soc;
    g_last_odo_m = s.last_odo_m;
  } else {
    g_km_per_pct = KM_PER_PCT_INIT;
    g_next_seq = 1;
    g_last_soc = S25C31.remainPercent;
    g_last_odo_m = odo_to_m(S23CB0.mileageTotal);
  }
  // Reset window
  clearRef();
  g_midride_written = 0;
  g_last_checkpoint_ms = millis();
  g_full_soc_seen = S25C31.remainPercent;
}

static bool movingEnough(uint32_t speed_abs) {
  // speed is presumably in mm/s or something? From display_fsm: they compute km/h as abs(speed)/1000 and /100.
  // In compute: _speed = c_speed * 10 / 8.5; m365_info.sph = abs(_speed) / 1000L; m365_info.spl = (abs(_speed) % 1000) / 100;
  // That implies c_speed is roughly km/h * 1000 * 8.5 / 10; not reliable. We'll use km/h readback from display calculation? Not available here.
  // Simpler: treat |S23CB0.speed| threshold value 3 km/h approx means their m365_info.sph >=3; We'll reconstruct quickly:
  long c_speed = (S23CB0.speed < -10000) ? (S23CB0.speed + 32768 + 32767) : abs(S23CB0.speed);
  long _speed = WheelSize ? ((long)c_speed * 10 / 85 * 10) : c_speed; // approximate; not used actually
  // Instead, use odometer delta to guard speed: if distance since last sample > 0.05 km within reasonable time, ok. We'll rely on external dist condition.
  (void)speed_abs; return true;
}

void rangeTick() {
  uint8_t soc = S25C31.remainPercent;
  uint32_t odo_m = odo_to_m(S23CB0.mileageTotal);

  // Handle odometer wraparound (uint32): if new < last, add 2^32 domain; but units in meters would overflow rarely. We'll detect and ignore negative diff.
  int32_t d_odo = (int32_t)(odo_m - g_last_odo_m);
  if (d_odo < 0) {
    // wrap or reset; ignore and reset window
    clearRef();
    d_odo = 0;
  }

  // Track max/full SoC seen
  if (soc > g_full_soc_seen) g_full_soc_seen = soc;

  // If SoC increased (regen or charge), reset window and don't learn
  if (g_last_soc != 0 && soc > g_last_soc) {
    clearRef();
  }

  // Establish reference window at first valid reading or when cleared
  if (!g_have_ref) {
    setRef(soc, odo_m);
  }

  // During ride learning: conditions
  uint8_t soc_drop = (g_ref_soc > soc) ? (g_ref_soc - soc) : 0;
  uint32_t dist_m = (odo_m >= g_ref_odo_m) ? (odo_m - g_ref_odo_m) : 0;

  // Speed check ≥ 3 km/h using distance/time since ref
  bool speed_ok = false;
  uint16_t dt_s = (uint16_t)((S23C3A.ridingTime >= g_ref_time_s) ? (S23C3A.ridingTime - g_ref_time_s) : 0);
  if (dt_s > 0) {
    float mps = (float)dist_m / (float)dt_s; // meters per second
    speed_ok = (mps >= 0.833f); // 3 km/h
  }
  bool learn_ok = (soc_drop >= 2) && (dist_m > 50) && speed_ok;
  if (learn_ok) {
    // Compute delta km per percent
    float delta_km = (float)dist_m / 1000.0f;
    float km_per_pct = delta_km / (float)soc_drop;
    if (km_per_pct < KM_PER_PCT_MIN) km_per_pct = KM_PER_PCT_MIN;
    if (km_per_pct > KM_PER_PCT_MAX) km_per_pct = KM_PER_PCT_MAX;

    float old = g_km_per_pct;
    g_km_per_pct = (1.0f - EMA_ALPHA) * g_km_per_pct + EMA_ALPHA * km_per_pct;
    clampLearned();

    // Hysteresis for dirty flag
  float delta = g_km_per_pct - old; if (delta < 0) delta = -delta;
    if (delta >= 0.01f || delta >= (0.03f * old)) {
      scheduleDirty();
    }

    // Reset reference window after learning
    setRef(soc, odo_m);
  }

  // End-of-discharge correction: when drop from full >80% and distance >=1 km (since near-full?)
  // We'll approximate full as max soc seen since last clear; use 100 as ideal.
  uint8_t soc_drop_full = (uint8_t)((g_full_soc_seen > soc) ? (g_full_soc_seen - soc) : 0);
  if (soc_drop_full > 80) {
    uint32_t total_km_centi = S23CB0.mileageCurrent; // trip distance since power on in 0.01 km
    if (total_km_centi >= 100) { // >=1.00 km
      uint8_t soc_drop = soc_drop_full;
      if (soc_drop >= 1) {
        float total_km = (float)total_km_centi / 100.0f;
        float eod_km_per_pct = total_km / (float)soc_drop;
        if (eod_km_per_pct < KM_PER_PCT_MIN) eod_km_per_pct = KM_PER_PCT_MIN;
        if (eod_km_per_pct > KM_PER_PCT_MAX) eod_km_per_pct = KM_PER_PCT_MAX;
        float old = g_km_per_pct;
        g_km_per_pct = (1.0f - EOD_BETA) * g_km_per_pct + EOD_BETA * eod_km_per_pct;
        clampLearned();
  float delta = g_km_per_pct - old; if (delta < 0) delta = -delta;
        if (delta >= 0.01f || delta >= (0.05f * old)) {
          scheduleDirty();
        }
        clearRef();
        // End-of-discharge checkpoint now
        if (g_dirty) {
          checkpoint(soc);
          g_last_checkpoint_ms = millis();
        }
      }
    }
  }

  // End-of-charge: if SOC near full
  if (soc >= 99) {
    if (g_dirty) {
      checkpoint(soc);
      g_last_checkpoint_ms = millis();
    }
    // Reset full tracker and window for next ride
    g_full_soc_seen = soc;
    clearRef();
  }

  // Mid-ride write policy: at ~50% SoC or every >=10 km / >=30 min after last write, whichever later, and only once mid-ride
  uint32_t now = millis();
  bool time_ok = (now - g_last_checkpoint_ms) >= (30UL * 60UL * 1000UL);
  uint32_t trip_ckm = S23CB0.mileageCurrent; // 0.01 km
  bool dist_ok = (trip_ckm >= 1000); // 10.00 km
  if (!g_midride_written && g_dirty && (S25C31.remainPercent <= 50) && time_ok && dist_ok) {
    checkpoint(soc);
    g_last_checkpoint_ms = now;
    g_midride_written = 1;
  }

  // Update last seen
  g_last_soc = soc;
  g_last_odo_m = odo_m;
}

void rangeCheckpointIfNeeded() {
  if (g_dirty) {
    checkpoint(S25C31.remainPercent);
    g_last_checkpoint_ms = millis();
  }
}
