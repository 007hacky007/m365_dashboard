// Central configuration for user-tunable options
// Edit values here to customize behavior across the project.
#pragma once

// =========================
// Battery and Energy Model
// =========================
// Reported current comes from pack 1’s BMS. If you run packs in parallel, set PACK2_MAH
// to scale displayed current/power/energy to the total.
// Set PACK2_MAH to 0 to disable scaling.
#ifndef PACK1_MAH
#define PACK1_MAH 7800u   // Pack 1 nominal capacity (mAh)
#endif
#ifndef PACK2_MAH
#define PACK2_MAH 14000u  // Pack 2 nominal capacity (mAh); 0 = no extra pack
#endif

// =========================
// Range Estimator Settings
// =========================
// Initial learned km per 1% SoC.
// Examples:
// - 784 Wh total (280 + 504 extra) and ~35 km per full: 0.35 km/%
// - Stock 280 Wh and ~20 km per full: 0.20 km/%
#ifndef RANGE_KM_PER_PCT_INIT
#define RANGE_KM_PER_PCT_INIT 0.35f
#endif
// Bounds clamp for the learned value
#ifndef RANGE_KM_PER_PCT_MIN
#define RANGE_KM_PER_PCT_MIN 0.05f
#endif
#ifndef RANGE_KM_PER_PCT_MAX
#define RANGE_KM_PER_PCT_MAX 0.60f
#endif
// Learning rates
#ifndef RANGE_EMA_ALPHA
#define RANGE_EMA_ALPHA 0.10f     // main EMA smoothing
#endif
#ifndef RANGE_EOD_BETA
#define RANGE_EOD_BETA 0.30f      // stronger end-of-discharge correction
#endif

// =========================
// UI Defaults
// =========================
// These are power-on defaults (can be changed in menu and saved to EEPROM).
#ifndef CFG_WARN_BATTERY_PERCENT_DEFAULT
#define CFG_WARN_BATTERY_PERCENT_DEFAULT 5
#endif
#ifndef CFG_AUTOBIG_DEFAULT
#define CFG_AUTOBIG_DEFAULT true
#endif
#ifndef CFG_BIGMODE_DEFAULT
#define CFG_BIGMODE_DEFAULT 1        // 0=speed, 1=current/power
#endif
#ifndef CFG_BIGFONTSTYLE_DEFAULT
#define CFG_BIGFONTSTYLE_DEFAULT 1   // 0=STD, 1=DIGIT
#endif
#ifndef CFG_BIGWARN_DEFAULT
#define CFG_BIGWARN_DEFAULT true
#endif
#ifndef CFG_HIBERNATE_ON_BOOT_DEFAULT
#define CFG_HIBERNATE_ON_BOOT_DEFAULT false
#endif
#ifndef CFG_SHOW_POWER_DEFAULT
#define CFG_SHOW_POWER_DEFAULT true
#endif
#ifndef CFG_SHOW_VOLTAGE_MAIN_DEFAULT
#define CFG_SHOW_VOLTAGE_MAIN_DEFAULT true
#endif

// =========================
// Hardware Pins / Addresses
// =========================
// OLED I2C address
#ifndef OLED_I2C_ADDRESS
#define OLED_I2C_ADDRESS 0x3C
#endif

// ESP32 UART pins for Xiaomi port (ignored on AVR)
#ifndef M365_UART_RX_PIN
#define M365_UART_RX_PIN 16
#endif
#ifndef M365_UART_TX_PIN
#define M365_UART_TX_PIN 17
#endif

// =========================
// Regional / Units
// =========================
// Define US_Version externally if you want mph/°F conversion (kept as-is here).
