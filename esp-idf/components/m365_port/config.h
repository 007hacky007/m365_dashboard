#pragma once
// Ported config constants (subset) for ESP-IDF build.
#ifndef RANGE_KM_PER_PCT_INIT
#define RANGE_KM_PER_PCT_INIT 0.35f
#endif
#ifndef RANGE_KM_PER_PCT_MIN
#define RANGE_KM_PER_PCT_MIN 0.05f
#endif
#ifndef RANGE_KM_PER_PCT_MAX
#define RANGE_KM_PER_PCT_MAX 0.60f
#endif
#ifndef RANGE_EMA_ALPHA
#define RANGE_EMA_ALPHA 0.10f
#endif
#ifndef RANGE_EOD_BETA
#define RANGE_EOD_BETA 0.30f
#endif

// UI Defaults (mirroring Arduino config.h)
#ifndef CFG_WARN_BATTERY_PERCENT_DEFAULT
#define CFG_WARN_BATTERY_PERCENT_DEFAULT 5
#endif
#ifndef CFG_AUTOBIG_DEFAULT
#define CFG_AUTOBIG_DEFAULT 1
#endif
#ifndef CFG_SHOW_POWER_DEFAULT
#define CFG_SHOW_POWER_DEFAULT 1
#endif
#ifndef CFG_SHOW_VOLTAGE_MAIN_DEFAULT
#define CFG_SHOW_VOLTAGE_MAIN_DEFAULT 1
#endif
#ifndef CFG_BIGWARN_DEFAULT
#define CFG_BIGWARN_DEFAULT 1
#endif
#ifndef CFG_BIGMODE_DEFAULT
#define CFG_BIGMODE_DEFAULT 0 /* 0 = big speed, 1 = big current/power */
#endif
#ifndef CFG_MAIN_TEMP_SOURCE_DEFAULT
#define CFG_MAIN_TEMP_SOURCE_DEFAULT 0
#endif
