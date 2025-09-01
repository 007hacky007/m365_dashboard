# M365 Xiaomi Scooter OLED Dashboard Documentation

## Overview
Custom OLED dashboard for Xiaomi M365 that reads the scooter bus and shows real‑time data, trip stats, and a learned range estimate. Supports AVR (Pro Mini) and ESP32 (incl. ESP32‑C3), with optional OTA on ESP32.

## Hardware Targets
- Arduino Pro Mini (8/16 MHz) — ultra‑small build, tight flash budget
- ESP32 DevKit (ESP32)
- ESP32‑C3 DevKit (RISC‑V)
- 0.96" or 1.3" SSD1306 I2C OLED
- FTD1232 (or similar) USB‑TTL programmer
- 1N4148 diode, 120 Ω 0.25 W resistor, 3D‑printed case/bracket

Notes
- ESP32 uses 3.3 V logic; Pro Mini can run 5 V or 3.3 V versions.
- Bus connection: UART @ 115200 baud to scooter main bus.

## Main Display Views

### 1) Normal view (default)
Shows core ride info:
- Speed (km/h or mph)
- Trip distance (km)
- Riding time (MM:SS)
- Current or Power (toggle in settings)
- Temperature (°C or °F)
- Battery bar at the bottom (blinks during regen)
- Small remaining range (km)

### 2) Big view (auto when speed > 1 km/h if enabled)
- Speed mode: large speed with decimal
- Current/Power mode: large A or W readout with unit
- Battery bar kept at the bottom; regen “R” indicator near top right

### 3) Battery info view
- Voltage (V) and current (A)
- Remaining capacity (mAh)
- Battery temperatures T1/T2
- Optional per‑cell voltages (0–14) if available

### 4) Trip stats view
- Avg energy per km (Wh/km)
- Max current (A) or Max power (W)
- Umin and Umax (V)

## Range Estimator
- Learns a single “km per 1% SoC” from SoC drop vs. odometer delta (EMA with end‑of‑discharge correction).
- Only uses SoC, odometer, and riding time (for a ≥3 km/h gate). It does not use current.
- Persisted to EEPROM with a 10‑slot wear‑leveled ring and CRC; auto‑recovered at boot.
- Configurable initial value and bounds in `M365/config.h`.

## Navigation & Controls
- Enter Settings: hold Brake + Throttle (both max) when speed ≤ 1 km/h
- In Settings, move/select with the same brake/throttle combos as before.
- When stationary, a short brake press cycles secondary screens.
- All settings are saved to EEPROM and persist.

## Configuration Menus (highlights)
Display settings
- Auto big view (Yes/No)
- Big view mode: Speed or Current/Power
- Low battery warning: Off/5%/10%/15%
- Big battery warning screen: Yes/No
- Show battery info
- Show current (A) or power (W)
- Show voltage on main screen instead of speed
- Big font style: STD or DIGIT
- Hibernate on boot: Yes/No
- Save & Exit

M365 scooter settings
- Cruise control: toggle and apply
- Taillight: toggle and apply
- KERS: Weak/Medium/Strong and apply
- Wheel size: 8.5" or 10"
- Exit

ESP32 Wi‑Fi/OTA (ESP32 only)
- Toggle Wi‑Fi/OTA AP
- View/edit AP SSID/PASS (default SSID is derived from MAC; PASS: m365ota123)
- OTA page: http://192.168.4.1/

## Data Sources
- Current/Voltage/SoC: BMS frame (S25C31). Displays use total scaled current if parallel packs are configured.
- Speed/Odometer/Times/Temp: DRV frames (S23Cxx).
- DRV current is not exposed in the stock frames we parse.

## Parallel Battery Packs
If you run two packs in parallel, the dashboard can scale the displayed current/power/energy to total pack current:
- Edit `PACK1_MAH` and `PACK2_MAH` in `M365/config.h`.
- Set `PACK2_MAH` to 0 to disable scaling (stock, single pack).
- Range learning is independent of current and continues to use SoC and odometer only.

## Central Configuration
All user‑tunable options live in `M365/config.h`:
- PACK1_MAH, PACK2_MAH
- RANGE_KM_PER_PCT_INIT, MIN/MAX, EMA_ALPHA, EOD_BETA
- UI defaults (autoBig, bigMode, bigFontStyle, warnings, etc.)
- OLED I2C address and ESP32 UART pins

## Build & Flash
Project includes a macOS‑friendly build script using Arduino CLI: `scripts/build_local.sh`

Common usage
- Build all default targets: `scripts/build_local.sh`
- Include SIM variants: `SIM=1 scripts/build_local.sh`
- Filter targets: `TARGETS="ESP32-Dev ESP32-C3-Dev" scripts/build_local.sh`
- Clean before build: `CLEAN=1 scripts/build_local.sh`

ESP32 partitions and OTA
- By default OTA is enabled for ESP32 with a dual‑app “min_spiffs” scheme (FlashSize=4M).
- To maximize single‑app size (no OTA): `OTA=0 scripts/build_local.sh` (uses “huge_app”).

Supported targets (examples)
- AVR: ProMini‑16MHz, ProMini‑8MHz (+ SIM variants)
- ESP32: ESP32‑Dev (+ SIM)
- ESP32‑C3: ESP32‑C3‑Dev (+ SIM)

## Languages
Multiple languages are available (see `M365/language.h`). On AVR, you can remove some to save flash. Default set includes: English, French, German, Spanish, Czech.

## Technical Details
Communication
- UART 115200 baud on the scooter bus.
- BMS (0x25C31) provides SoC/voltage/current; DRV (0x23xx) provides speed/odo/time/temp.

Display
- I2C (Wire) by default; SPI also supported (compile‑time option).
- Custom fonts included; big view supports STD and DIGIT styles.

Power
- Pro Mini can be powered from 5 V; ESP32 from 3.3 V (on‑board regulator options vary by devkit).

## Special Modes
Hibernation mode (for scooter firmware updates)
- Hold Brake + Throttle during power‑on to pause bus communication.
- Update scooter firmware, then power‑cycle to resume dashboard.

ESP32 OTA mode
- Enable Wi‑Fi in Settings (ESP32), connect to AP, open http://192.168.4.1/ and upload new firmware.

## Troubleshooting
- No data: check UART wiring to the scooter bus.
- Wrong speed: verify wheel size in settings.
- Sketch too large on Pro Mini: disable languages/features or prefer ESP32 target.
- Random glyphs: confirm correct font and language set; trim languages on AVR to save space.

This project enhances the M365 ride with clear real‑time metrics, a robust learned range estimate, and convenient OTA (ESP32).
