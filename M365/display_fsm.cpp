#include "defines.h"
#include "comms.h"
#include "battery_display.h"
#include "aht10.h"
// Custom ESP32-only big-digit renderers
#if defined(ARDUINO_ARCH_ESP32)
#include "fonts/odroid_dotmatrix_u8g2.h"
#include "fonts/odroid_sevenseg_u8g2.h"
#endif

// Main display function - handles all screen modes and user input
void displayFSM() {
  struct {
    uint16_t curh; uint16_t curl; uint16_t pwh; uint16_t pwl;
    uint16_t vh; uint16_t vl; uint32_t sph; uint16_t spl;
    uint16_t milh; uint16_t mill; uint16_t Min; uint16_t Sec; uint16_t temp;
  } m365_info;

  int brakeVal = -1;
  int throttleVal = -1;
  int tmp_0, tmp_1;
  long _speed;
  long c_speed;

  // Persist big screen activation across frames (ESP32 only to save AVR flash)
#if defined(ARDUINO_ARCH_ESP32)
  static bool s_bigActive = false;
  static uint32_t s_bigReleaseAt = 0; // millis() timestamp when we may release big screen
#endif

  if (S23CB0.speed < -10000) {
    c_speed = S23CB0.speed + 32768 + 32767;
  } else {
    c_speed = abs(S23CB0.speed);
  }

  if (WheelSize) _speed = (long)c_speed * 10 / 8.5; else _speed = c_speed;
  m365_info.sph = (uint32_t)abs(_speed) / 1000L;
  m365_info.spl = (uint16_t)(abs(_speed) % 1000L) / 100;
#ifdef US_Version
  m365_info.sph = m365_info.sph/1.609; m365_info.spl = m365_info.spl/1.609;
#endif

  // Update big screen state with hysteresis to avoid flicker near the threshold
#if defined(ARDUINO_ARCH_ESP32)
  {
    const int ENTER_SPEED = 220; // raw speed units; >200 used elsewhere as stationary threshold
    const int EXIT_SPEED  = 180; // lower than enter to add hysteresis
    const uint32_t HOLD_MS = 1500;
    if (!autoBig) {
      s_bigActive = false; s_bigReleaseAt = 0;
    } else {
      if (c_speed > ENTER_SPEED) { s_bigActive = true; s_bigReleaseAt = 0; }
      else if (s_bigActive) {
        if (s_bigReleaseAt == 0) s_bigReleaseAt = millis() + HOLD_MS;
        if ((millis() >= s_bigReleaseAt) && (c_speed <= EXIT_SPEED)) { s_bigActive = false; s_bigReleaseAt = 0; }
      }
    }
  }
#else
  bool s_bigActive = autoBig && (m365_info.sph > 1);
#endif
  int16_t cur_cA_raw = totalCurrent_cA();
  m365_info.curh = abs(cur_cA_raw) / 100;
  m365_info.curl = abs(cur_cA_raw) % 100;
  m365_info.vh = abs(S25C31.voltage) / 100;
  m365_info.vl = abs(S25C31.voltage) % 100;
  {
    uint32_t ai = (uint32_t)abs(cur_cA_raw);
    uint32_t vi = (uint32_t)abs(S25C31.voltage);
    uint32_t m = (ai * vi + 50) / 100;
    m365_info.pwh = (uint16_t)(m / 100);
    m365_info.pwl = (uint16_t)(m % 100);
  }

  if ((m365_info.sph > 1) && Settings) { ShowBattInfo = false; M365Settings = false; Settings = false; }

  // Update per-trip aggregates (since power-on)
  {
    // Track energy: power (W) = current(A) * voltage(V). We have centi-units for I and V.
    // Compute W*100 from m365_info.pwh/pwl => integer watts and fractional 0-99.
    uint32_t P_Wx100 = (uint32_t)m365_info.pwh * 100UL + (uint32_t)m365_info.pwl;
    // Integrate energy in Wh*100 using elapsed seconds since last sample of powerOnTime
    uint32_t pot_s = S23C3A.powerOnTime; // seconds
    // Detect wrap or reset (e.g. after power cycle)
    if (lastPowerOnTime_s == 0 || pot_s < lastPowerOnTime_s) {
      lastPowerOnTime_s = pot_s;
      // Reset per-trip metrics when starting fresh
      tripEnergy_Wh_x100 = 0;
      tripMaxCurrent_cA = 0; tripMaxPower_Wx100 = 0;
      tripMinVoltage_cV = 0xFFFF; tripMaxVoltage_cV = 0;
    }
    uint32_t dt = (pot_s >= lastPowerOnTime_s) ? (pot_s - lastPowerOnTime_s) : 0;
    if (dt > 0) {
  // Wh = W * h; Wh*100 = (W*100) * (dt/3600)
  tripEnergy_Wh_x100 += (P_Wx100 * dt) / 3600UL;
      lastPowerOnTime_s = pot_s;
    }

    // Track max current and power (absolute discharge only)
  uint16_t cur_cA = (uint16_t)abs(cur_cA_raw); // centi-amps (scaled total)
    if (cur_cA > tripMaxCurrent_cA) tripMaxCurrent_cA = cur_cA;
    uint32_t Pw_x100 = P_Wx100; // already W*100
    if (Pw_x100 > tripMaxPower_Wx100) tripMaxPower_Wx100 = Pw_x100;

    // Track min/max voltage
    uint16_t v_cV = (uint16_t)abs(S25C31.voltage);
    if (v_cV < tripMinVoltage_cV) tripMinVoltage_cV = v_cV;
    if (v_cV > tripMaxVoltage_cV) tripMaxVoltage_cV = v_cV;
  }

  if ((c_speed <= 200) || Settings) {
    if (S20C00HZ65.brake > 60) brakeVal = 1; else if (S20C00HZ65.brake < 50) brakeVal = -1; else brakeVal = 0;
    if (S20C00HZ65.throttle > 150) throttleVal = 1; else if (S20C00HZ65.throttle < 50) throttleVal = -1; else throttleVal = 0;

    if (((brakeVal == 1) && (throttleVal == 1) && !Settings) && ((oldBrakeVal != 1) || (oldThrottleVal != 1))) {
      uiAltScreen = 0; // ensure we return to main after exiting settings
      menuPos = 0; timer = millis() + LONG_PRESS; Settings = true;
      displayClear(2, true); // force clear when entering settings
    }

    // Handle screen cycling when stationary:
    // - Throttle press: next screen
    // - Brake press: previous screen
    // Only when not in any modal screen
    if (!Settings && !M365Settings && !ShowBattInfo) {
      bool stationary = (c_speed <= 200);
      if (stationary) {
        // Determine number of screens by platform (ESP32 has extra temperatures screen)
        uint8_t totalScreens =
#if defined(ARDUINO_ARCH_ESP32)
          4
#else
          3
#endif
        ;
        // Edge handlers based on config
        bool thEdge = (throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal <= 0);
        bool brEdge = (brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal <= 0);
#if CFG_NAV_THROTTLE_NEXT
        if (thEdge) { // throttle -> next
          uiAltScreen = (uiAltScreen + 1) % totalScreens;
          timer = millis() + LONG_PRESS;
        } else if (brEdge) { // brake -> previous
          uiAltScreen = (uiAltScreen == 0) ? (totalScreens - 1) : (uiAltScreen - 1);
          timer = millis() + LONG_PRESS;
        }
#else
        if (brEdge) { // brake -> next
          uiAltScreen = (uiAltScreen + 1) % totalScreens;
          timer = millis() + LONG_PRESS;
        } else if (thEdge) { // throttle -> previous
          uiAltScreen = (uiAltScreen == 0) ? (totalScreens - 1) : (uiAltScreen - 1);
          timer = millis() + LONG_PRESS;
        }
#endif
      }
    }

    if (M365Settings) {
  // On ESP32/U8g2, clear buffer each frame to avoid overdraw artifacts in menus
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#endif
      if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal == -1) && (oldBrakeVal == -1))
      switch (sMenuPos) {
        case 0: cfgCruise = !cfgCruise; EEPROM.put(6, cfgCruise); EEPROM_COMMIT(); break;
        case 1: if (cfgCruise) prepareCommand(CMD_CRUISE_ON); else prepareCommand(CMD_CRUISE_OFF); break;
        case 2: cfgTailight = !cfgTailight; EEPROM.put(7, cfgTailight); EEPROM_COMMIT(); break;
        case 3: if (cfgTailight) prepareCommand(CMD_LED_ON); else prepareCommand(CMD_LED_OFF); break;
        case 4: switch (cfgKERS) { case 1: cfgKERS = 2; break; case 2: cfgKERS = 0; break; default: cfgKERS = 1; } EEPROM.put(8, cfgKERS); EEPROM_COMMIT(); break;
        case 5: switch (cfgKERS) { case 1: prepareCommand(CMD_MEDIUM); break; case 2: prepareCommand(CMD_STRONG); break; default: prepareCommand(CMD_WEAK); } break;
        case 6: WheelSize = !WheelSize; EEPROM.put(5, WheelSize); EEPROM_COMMIT(); break;
        case 7: oldBrakeVal = brakeVal; oldThrottleVal = throttleVal; timer = millis() + LONG_PRESS; M365Settings = false; break;
      } else if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal == -1) && (oldThrottleVal == -1)) { if (sMenuPos < 7) sMenuPos++; else sMenuPos = 0; timer = millis() + LONG_PRESS; }

  if (displayClear(7)) sMenuPos = 0;
  display.set1X(); display.setFont(defaultFont); display.setCursor(0, 0);
  if (sMenuPos == 0) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr1);
      display.print(cfgCruise ? (const __FlashStringHelper *) l_On : (const __FlashStringHelper *) l_Off);

      display.setCursor(0, 1);
  if (sMenuPos == 1) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr2);

      display.setCursor(0, 2);
  if (sMenuPos == 2) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr3);
      display.print(cfgTailight ? (const __FlashStringHelper *) l_Yes : (const __FlashStringHelper *) l_No);

      display.setCursor(0, 3);
  if (sMenuPos == 3) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr4);

      display.setCursor(0, 4);
  if (sMenuPos == 4) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr5);
      switch (cfgKERS) { case 1: display.print((const __FlashStringHelper *) l_Medium); break; case 2: display.print((const __FlashStringHelper *) l_Strong); break; default: display.print((const __FlashStringHelper *) l_Weak); break; }

      display.setCursor(0, 5);
  if (sMenuPos == 5) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr6);

      display.setCursor(0, 6);
  if (sMenuPos == 6) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr7);
      if (WheelSize) display.print((const __FlashStringHelper *) l_10inch); else display.print((const __FlashStringHelper *) l_85inch);

      display.setCursor(0, 7);
  if (sMenuPos == 7) display.print('>'); else display.print(" ");
      display.print((const __FlashStringHelper *) M365CfgScr8);

      oldBrakeVal = brakeVal; oldThrottleVal = throttleVal;
      return;
    } else if (ShowBattInfo) {
      if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal == -1) && (oldThrottleVal == -1)) { oldBrakeVal = brakeVal; oldThrottleVal = throttleVal; timer = millis() + LONG_PRESS; ShowBattInfo = false; return; }
      fsBattInfo(); display.setCursor(0, 7); display.print((const __FlashStringHelper *) battScr); oldBrakeVal = brakeVal; oldThrottleVal = throttleVal; return;
    } else if (Settings) {
  // Disable long-press auto-exit from Settings to avoid unintended exits
  // (Use explicit Save/Exit menu item instead)

      // Apply action on throttle press when brake is released (neutral or below)
      if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal <= 0) && (oldBrakeVal <= 0)) switch (menuPos) {
  case 0: autoBig = !autoBig; break;
  case 1: bigMode = (bigMode == 1) ? 0 : 1; break;
  case 2: switch (warnBatteryPercent) { case 0: warnBatteryPercent = 5; break; case 5: warnBatteryPercent = 10; break; case 10: warnBatteryPercent = 15; break; default: warnBatteryPercent = 0; } break;
  case 3: bigWarn = !bigWarn; break;
  case 4: ShowBattInfo = true; break;
  case 5: M365Settings = true; break;
#if defined(ARDUINO_ARCH_ESP32)
  case 6: WiFiSettings = true; wifiMenuPos = 0; break;
  case 7: showPower = !showPower; break;
  case 8: showVoltageMain = !showVoltageMain; break;
  case 9: bigFontStyle = (bigFontStyle == 0) ? 1 : 0; break;
  case 10: hibernateOnBoot = !hibernateOnBoot; break;
  case 11: {
    // Cycle main temperature source (ESP32: 0..3)
    uint8_t maxSrc =
#if CFG_AHT10_ENABLE
  3
#else
  2
#endif
  ;
    if (mainTempSource >= maxSrc) mainTempSource = 0; else mainTempSource++;
    break;
  }
  case 12: EEPROM.put(1, autoBig); EEPROM.put(2, warnBatteryPercent); EEPROM.put(3, bigMode); EEPROM.put(4, bigWarn); EEPROM.put(9, hibernateOnBoot); EEPROM.put(10, showPower); EEPROM.put(11, wifiEnabled); EEPROM.put(12, showVoltageMain); EEPROM.put(13, bigFontStyle); EEPROM.put(14, mainTempSource); EEPROM_COMMIT(); Settings = false; break;
#else
  case 6: showPower = !showPower; break;
  case 7: showVoltageMain = !showVoltageMain; break;
  case 8: bigFontStyle = (bigFontStyle == 0) ? 1 : 0; break;
  case 9: hibernateOnBoot = !hibernateOnBoot; break;
  case 10: EEPROM.put(1, autoBig); EEPROM.put(2, warnBatteryPercent); EEPROM.put(3, bigMode); EEPROM.put(4, bigWarn); EEPROM.put(9, hibernateOnBoot); EEPROM.put(10, showPower); EEPROM.put(12, showVoltageMain); EEPROM.put(13, bigFontStyle); EEPROM.put(14, mainTempSource); EEPROM_COMMIT(); Settings = false; break;
#endif
      } else if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal <= 0) && (oldThrottleVal <= 0)) {
#if defined(ARDUINO_ARCH_ESP32)
  if (menuPos < 12)
#else
  if (menuPos < 10)
#endif
    menuPos++;
  else
    menuPos = 0;
  timer = millis() + LONG_PRESS;
      }

#if defined(ARDUINO_ARCH_ESP32)
  // WiFi settings sub-menu takes over rendering when active
      if (WiFiSettings) {
        if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal <= 0) && (oldThrottleVal <= 0)) { if (wifiMenuPos < 4) wifiMenuPos++; else wifiMenuPos = 0; timer = millis() + LONG_PRESS; }
        if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal <= 0) && (oldBrakeVal <= 0)) {
          switch (wifiMenuPos) {
            case 0: wifiEnabled = !wifiEnabled; if (wifiEnabled) otaBegin(); else otaEnd(); EEPROM.put(11, wifiEnabled); EEPROM_COMMIT(); break;
            case 4: WiFiSettings = false; break;
            default: break;
          }
        }
        if (otaSSID.length() == 0) {
          uint8_t mac[6]; WiFi.macAddress(mac); char ssidBuf[24]; snprintf(ssidBuf, sizeof(ssidBuf), "M365-OTA-%02X%02X", mac[4], mac[5]); otaSSID = ssidBuf;
        }
        if (otaPASS.length() == 0) otaPASS = "m365ota123";

  // Full clear on ESP32 to avoid overdraw; AVR uses ID-based clear
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(12);
#endif
  display.set1X(); display.setFont(defaultFont); display.setCursor(0, 0);
  display.print(wifiMenuPos == 0 ? '>' : ' ');
        display.print((const __FlashStringHelper *) wifiMenu1);
        display.print(wifiEnabled ? (const __FlashStringHelper *) l_On : (const __FlashStringHelper *) l_Off);

        display.setCursor(0, 2);
  display.print(wifiMenuPos == 1 ? '>' : ' ');
        display.print((const __FlashStringHelper *) wifiMenu2); display.print(' '); display.print(otaSSID);

        display.setCursor(0, 3);
  display.print(wifiMenuPos == 2 ? '>' : ' ');
        display.print((const __FlashStringHelper *) wifiMenu3); display.print(' '); display.print(otaPASS);

        display.setCursor(0, 5);
  display.print(wifiMenuPos == 3 ? '>' : ' ');
        display.print((const __FlashStringHelper *) wifiMenu4); display.print("192.168.4.1");

        display.setCursor(0, 7);
  display.print(wifiMenuPos == 4 ? '>' : ' ');
        display.print((const __FlashStringHelper *) wifiMenu5);

        oldBrakeVal = brakeVal; oldThrottleVal = throttleVal; return;
      }
#endif

  // Windowed rendering: only 8 visible lines, scroll as menuPos changes
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(2);
#endif
      display.set1X(); display.setFont(defaultFont);
      // Determine total items depending on platform
      uint8_t totalItems =
#if defined(ARDUINO_ARCH_ESP32)
        13; // indices 0..12 (extra main temp source + save)
#else
        11; // indices 0..10
#endif
      // Compute top index so selection stays within window
      uint8_t top = 0;
      if (menuPos > 3) {
        uint8_t maxTop = (totalItems > 8) ? (totalItems - 8) : 0;
        uint8_t desired = menuPos - 3;
        top = (desired > maxTop) ? maxTop : desired;
      }

      // Helper lambda to draw one item at a given row
      auto drawItem = [&](uint8_t idx, uint8_t row){
        display.setCursor(0, row);
  if (menuPos == idx) display.print('>'); else display.print(' ');
        switch (idx) {
          case 0: display.print((const __FlashStringHelper *) confScr1);
            display.print(autoBig ? (const __FlashStringHelper *) l_Yes : (const __FlashStringHelper *) l_No); break;
          case 1: display.print((const __FlashStringHelper *) confScr2);
            switch (bigMode) { case 1: display.print((const __FlashStringHelper *) confScr2b); break; default: display.print((const __FlashStringHelper *) confScr2a); } break;
          case 2: display.print((const __FlashStringHelper *) confScr3);
            switch (warnBatteryPercent) { case 5: display.print(" 5%"); break; case 10: display.print("10%"); break; case 15: display.print("15%" ); break; default: display.print((const __FlashStringHelper *) l_Off); } break;
          case 3: display.print((const __FlashStringHelper *) confScr4);
            display.print(bigWarn ? (const __FlashStringHelper *) l_Yes : (const __FlashStringHelper *) l_No); break;
          case 4: display.print((const __FlashStringHelper *) confScr5); break;
          case 5: display.print((const __FlashStringHelper *) confScr6); break;
#if defined(ARDUINO_ARCH_ESP32)
          case 6: display.print((const __FlashStringHelper *) confScr10); break;
          case 7: display.print((const __FlashStringHelper *) confScr9); display.print(showPower ? (const __FlashStringHelper *) l_w : (const __FlashStringHelper *) l_a); break;
          case 8: display.print((const __FlashStringHelper *) confScr11); display.print(showVoltageMain ? (const __FlashStringHelper *) confScr11b : (const __FlashStringHelper *) confScr11a); break;
          case 9: display.print((const __FlashStringHelper *) confScr12); display.print(bigFontStyle ? (const __FlashStringHelper *) confScr12b : (const __FlashStringHelper *) confScr12a); break;
          case 10: display.print((const __FlashStringHelper *) confScr7); display.print(hibernateOnBoot ? (const __FlashStringHelper *) l_Yes : (const __FlashStringHelper *) l_No); break;
          case 11: display.print((const __FlashStringHelper *) confScr13);
            switch (mainTempSource) {
              case 0: display.print((const __FlashStringHelper *) confScr13a); break;
              case 1: display.print((const __FlashStringHelper *) confScr13b); break;
              case 2: display.print((const __FlashStringHelper *) confScr13c); break;
#if CFG_AHT10_ENABLE
              case 3: display.print((const __FlashStringHelper *) confScr13d); break;
#endif
              default: display.print((const __FlashStringHelper *) confScr13a); break;
            }
            break;
          case 12: display.print((const __FlashStringHelper *) confScr8); break;
#else
          case 6: display.print((const __FlashStringHelper *) confScr9); display.print(showPower ? (const __FlashStringHelper *) l_w : (const __FlashStringHelper *) l_a); break;
          case 7: display.print((const __FlashStringHelper *) confScr11); display.print(showVoltageMain ? (const __FlashStringHelper *) confScr11b : (const __FlashStringHelper *) confScr11a); break;
          case 8: display.print((const __FlashStringHelper *) confScr12); display.print(bigFontStyle ? (const __FlashStringHelper *) confScr12b : (const __FlashStringHelper *) confScr12a); break;
          case 9: display.print((const __FlashStringHelper *) confScr7); display.print(hibernateOnBoot ? (const __FlashStringHelper *) l_Yes : (const __FlashStringHelper *) l_No); break;
          case 10: display.print((const __FlashStringHelper *) confScr8); break;
#endif
          default: break;
        }
      };

      for (uint8_t line = 0; line < 8; ++line) {
        uint8_t idx = top + line;
        if (idx >= totalItems) break;
        drawItem(idx, line);
      }

      // Latch current input states for edge detection on next frame
      oldBrakeVal = brakeVal; oldThrottleVal = throttleVal; return;

    }

    oldBrakeVal = brakeVal; oldThrottleVal = throttleVal;
  }

  // Driving/normal rendering
  // 1) Blink big warning when enabled and battery percent below threshold
  if (bigWarn && (warnBatteryPercent > 0) && (S25C31.remainPercent <= warnBatteryPercent) && (millis() % 2000 < 700)) {
  // Always clear on ESP32 to avoid overdraw
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  (void)displayClear(4);
#endif
#if !defined(ARDUINO_ARCH_ESP32)
      display.setFont(m365); display.setCursor(0, 0); display.print((char)0x21); display.setFont(defaultFont);
  #else
    // Use a guaranteed glyph on ESP32 (default font, 2x)
    display.setFont(defaultFont); display.set2X();
    display.setCursor(0, 2); display.print("LOW BATT");
    display.set1X();
  #endif
    return;
  }

  // 2) Auto big screen when active (with hysteresis)
  if (s_bigActive) {
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(5);
#endif
  display.set1X();
    switch (bigMode) {
      case 1: // current/power big mode
      {
#if defined(ARDUINO_ARCH_ESP32)
        // ESP32: render with MATRIX (dot-matrix) or DIGITAL (seven-seg) blitters
        auto drawLargeDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawLarge(x, y, (uint8_t)d); else OdroidSevenSeg::drawLarge(x, y, (uint8_t)d); };
        auto drawSmallDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawSmall(x, y, (uint8_t)d); else OdroidSevenSeg::drawSmall(x, y, (uint8_t)d); };
  // Style-aware positioning
  // MATRIX now uses smaller digits (14px wide); add 3px more spacing -> step 19; SEVENSEG keeps large 24 step
  const uint8_t stepMain = (bigFontStyle == 0) ? 20 : 24;
  const uint8_t X0 = 0, X1 = stepMain, X2 = uint8_t(stepMain * 2), X3 = uint8_t(stepMain * 3);
  const uint8_t Y0 = 0;
  const uint8_t yOffset = (bigFontStyle == 1) ? 15 : 15; // align MATRIX baseline with 2x 'W'
        if (showPower) {
          uint16_t W = m365_info.pwh; if (W > 9999) W = 9999;
          int d0 = (W >= 1000) ? ((W / 1000) % 10) : -1; int d1 = (W >= 100) ? ((W / 100) % 10) : -1; int d2 = (W >= 10) ? ((W / 10) % 10) : -1; int d3 = (W % 10);
          // For MATRIX, use smaller glyphs; for SEVENSEG, large as before
          auto drawMainDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawSmall(x, y, (uint8_t)d); else OdroidSevenSeg::drawLarge(x, y, (uint8_t)d); };
          // Right-align MATRIX digits with respect to the 'W' unit
          const uint8_t digitW = (bigFontStyle == 0) ? 14 : 17; // glyph widths
          const uint8_t unitX = 112; // fixed position for 'W' to the right
          const uint8_t gapUnit = (bigFontStyle == 0) ? 8 : 8; // gap between last digit and unit
          uint8_t ux = unitX; // 'W' X
          // Determine how many digits will be drawn
          uint8_t nDigits = (W >= 1000) ? 4 : (W >= 100) ? 3 : (W >= 10) ? 2 : 1;
          // Compute the X where the last digit's right edge should land
          int16_t digitsRight = (int16_t)ux - gapUnit;
          int16_t startX = digitsRight - digitW - (int16_t)(stepMain) * (nDigits - 1);
          // Draw present digits starting from startX
          int idx = 0;
          if (d0 >= 0) { drawMainDigit((uint8_t)(startX + stepMain * idx), Y0 + yOffset, d0); idx++; }
          if (d1 >= 0) { drawMainDigit((uint8_t)(startX + stepMain * idx), Y0 + yOffset, d1); idx++; }
          if (d2 >= 0) { drawMainDigit((uint8_t)(startX + stepMain * idx), Y0 + yOffset, d2); idx++; }
          drawMainDigit((uint8_t)(startX + stepMain * idx), Y0 + yOffset, d3);
          // Align unit baseline with digits: MATRIX uses row 3, DIGITAL keeps row 4
          uint8_t yUnit = (bigFontStyle == 0) ? 3 : 4;
          display.setFont(defaultFont); display.setCursor(ux, yUnit); display.set2X(); display.print((const __FlashStringHelper *) l_w); display.set1X();
        } else {
          // Current in A: two large integer digits + two small fractional digits
          int c_i_tens = m365_info.curh / 10; int c_i_ones = m365_info.curh % 10;
          auto drawMainDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawSmall(x, y, (uint8_t)d); else OdroidSevenSeg::drawLarge(x, y, (uint8_t)d); };
          if (bigFontStyle == 0) {
            const uint8_t digitW = 14; const uint8_t unitX = 120; const uint8_t gapUnit = 4;
            uint8_t nDigits = (c_i_tens > 0) ? 2 : 1;
            int16_t digitsRight = (int16_t)unitX - gapUnit;
            int16_t startX = digitsRight - digitW - (int16_t)(stepMain) * (nDigits - 1);
            int idx = 0;
            if (c_i_tens > 0) { drawMainDigit((uint8_t)(startX + stepMain * idx), yOffset, c_i_tens); idx++; }
            drawMainDigit((uint8_t)(startX + stepMain * idx), yOffset, c_i_ones);
          } else {
            if (c_i_tens > 0) drawMainDigit(X0, yOffset, c_i_tens);
            drawMainDigit(X1, yOffset, c_i_ones);
          }
          // Fractional: position relative to integer area; keep small glyphs for both styles
          int c_f_tens = m365_info.curl / 10; int c_f_ones = m365_info.curl % 10;
          uint8_t fx1 = uint8_t(stepMain * 3); uint8_t fx2 = uint8_t(stepMain * 4);
          if (fx2 > 120) fx2 = 120; if (fx1 > 120) fx1 = 120;
          drawSmallDigit(fx1, 0, c_f_tens);
          drawSmallDigit(fx2, 0, c_f_ones);
          uint8_t ax = uint8_t(fx2 + 20); if (ax > 120) ax = 120;
          if ((cur_cA_raw >= 0) || ((cur_cA_raw < 0) && (millis() % 1000 < 500))) {
            display.set2X(); display.setCursor(ax, (bigFontStyle == 0) ? 3 : 4); display.print((const __FlashStringHelper *) l_a); display.set1X();
          }
        }
        display.setFont(defaultFont); display.set1X();
        if (cur_cA_raw < 0) { display.setCursor(120, 0); display.print('R'); } else { display.setCursor(120, 0); display.print(' '); }
#else
        // AVR: keep legacy font path to save flash
        if (bigFontStyle == 0) { display.setFont(bigNumb); display.set1X(); } else { display.setFont(segNumb); display.set2X(); }
        if (showPower) {
          uint16_t W = m365_info.pwh; if (W > 9999) W = 9999;
          char buf[5]; buf[0] = (W >= 1000) ? char('0' + (W / 1000) % 10) : ' ';
          buf[1] = (W >= 100)  ? char('0' + (W / 100) % 10)  : ' ';
          buf[2] = (W >= 10)   ? char('0' + (W / 10) % 10)   : ' ';
          buf[3] = char('0' + (W % 10)); buf[4] = 0;
          if (bigFontStyle == 1) { for (uint8_t i = 0; i < 3; ++i) if (buf[i] == ' ') buf[i] = (char)0x3B; }
          display.setCursor(0, 0);  display.print(buf[0]);
          display.setCursor(26, 0); display.print(buf[1]);
          display.setCursor(54, 0); display.print(buf[2]);
          display.setCursor(84, 0); display.print(buf[3]);
          uint8_t ux = display.col(); if (ux > 112) ux = 112; uint8_t yUnit = 4;
          display.setFont(defaultFont); display.setCursor(ux, yUnit); display.set2X(); display.print((const __FlashStringHelper *) l_w); display.set1X();
        } else {
          tmp_0 = m365_info.curh / 10; tmp_1 = m365_info.curh % 10;
          display.setCursor(2, 0); if (tmp_0 > 0) display.print(tmp_0); else display.print(bigFontStyle == 1 ? (char)0x3B : ' ');
          display.setCursor(32, 0); display.print(tmp_1);
          tmp_0 = m365_info.curl / 10; tmp_1 = m365_info.curl % 10;
          display.setCursor(75, 0); display.print(tmp_0);
          display.setCursor(108, 0); if (bigFontStyle == 0) { display.setFont(bigNumb); display.set1X(); } else { display.setFont(segNumb); display.set2X(); } display.print(tmp_1); display.setFont(defaultFont);
          if ((cur_cA_raw >= 0) || ((cur_cA_raw < 0) && (millis() % 1000 < 500))) { display.set2X(); display.setCursor(108, (bigFontStyle == 0) ? 3 : 4); display.print((const __FlashStringHelper *) l_a); }
        }
        display.setFont(defaultFont); display.set1X();
        if (cur_cA_raw < 0) { display.setCursor(120, 0); display.print('R'); } else { display.setCursor(120, 0); display.print(' '); }
#endif
        break;
      }
      default: // speed big mode
      {
#if defined(ARDUINO_ARCH_ESP32)
    auto drawLargeDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawLarge(x, y, (uint8_t)d); else OdroidSevenSeg::drawLarge(x, y, (uint8_t)d); };
        auto drawSmallDigit = [&](uint8_t x, uint8_t y, int d){ if (d > 9) return; if (bigFontStyle == 0) OdroidDotMatrix::drawSmall(x, y, (uint8_t)d); else OdroidSevenSeg::drawSmall(x, y, (uint8_t)d); };
  int sp_tens = m365_info.sph / 10; int sp_ones = m365_info.sph % 10;
  const uint8_t stepMain = (bigFontStyle == 0) ? 16 : 24; // MATRIX small spacing; SEVENSEG large spacing
  const uint8_t yOffset = (bigFontStyle == 1) ? 2 : 0;
  // MATRIX uses smaller glyphs for speed; SEVENSEG keeps large
  if (bigFontStyle == 0) {
    if (sp_tens > 0) OdroidDotMatrix::drawSmall(0, yOffset, (uint8_t)sp_tens);
    OdroidDotMatrix::drawSmall(stepMain, yOffset, (uint8_t)sp_ones);
  } else {
    if (sp_tens > 0) OdroidSevenSeg::drawLarge(0, yOffset, (uint8_t)sp_tens);
    OdroidSevenSeg::drawLarge(stepMain, yOffset, (uint8_t)sp_ones);
  }
        drawSmallDigit(75, 0, m365_info.spl);
        display.setCursor(106, 0); display.print((char)0x3A);
#else
        if (bigFontStyle == 0) { display.setFont(bigNumb); display.set1X(); } else { display.setFont(segNumb); display.set2X(); }
        tmp_0 = m365_info.sph / 10; tmp_1 = m365_info.sph % 10;
        display.setCursor(2, 0); if (tmp_0 > 0) display.print(tmp_0); else display.print(bigFontStyle == 1 ? (char)0x3B : ' ');
        display.setCursor(32, 0); display.print(tmp_1);
        display.setCursor(75, 0); display.print(m365_info.spl);
        display.setCursor(106, 0); display.print((char)0x3A);
#endif
        break;
      }
    }
    showBatt(S25C31.remainPercent, cur_cA_raw < 0);
    showRangeSmall();
    return;
  }

  // 3) Stationary/alt screens and default main screen
  // Note: Battery/cell details screen is only accessible from Settings now (no auto popup)
  {
    // Decide which alt screen to render
    uint8_t screenToShow = uiAltScreen; // 0 main, 1 trip stats, 2 odometer, 3 temperatures (ESP32 only)
    if (screenToShow == 2) {
      // Odometer/power-on time screen
  // Full clear on ESP32 to avoid digit ghosting
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(3);
#endif
      display.set1X(); display.setFont(defaultFont); display.setCursor(0, 0);
      display.print((const __FlashStringHelper *) infoScr1); display.print(':');
      display.setFont(stdNumb); display.setCursor(15, 1);
      tmp_0 = S23CB0.mileageTotal / 1000; tmp_1 = (S23CB0.mileageTotal % 1000) / 10;
      if (tmp_0 < 1000) display.print(' '); if (tmp_0 < 100) display.print(' '); if (tmp_0 < 10) display.print(' ');
      display.print(tmp_0); display.print('.'); if (tmp_1 < 10) display.print('0'); display.print(tmp_1);
      display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_km);

      display.setCursor(0, 5); display.print((const __FlashStringHelper *) infoScr2); display.print(':');
      display.setFont(stdNumb); display.setCursor(25, 6);
      tmp_0 = S23C3A.powerOnTime / 60; tmp_1 = S23C3A.powerOnTime % 60;
      if (tmp_0 < 100) display.print(' '); if (tmp_0 < 10) display.print(' ');
      display.print(tmp_0); display.print(':'); if (tmp_1 < 10) display.print('0'); display.print(tmp_1);
      return;
    } else if (screenToShow == 1) {
      // Trip stats
  // Full clear on ESP32 to avoid digit ghosting
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(6);
#endif
      display.set1X(); display.setFont(defaultFont);
      uint32_t mCurr = S23CB0.mileageCurrent; // km*100
      uint16_t avg_whkm_x100 = 0;
      if (mCurr > 0) {
        uint32_t dist_km_x100 = mCurr;
        uint32_t avg_u32 = (tripEnergy_Wh_x100 * 100UL) / dist_km_x100; // Wh/km *100
        if (avg_u32 > 65535UL) avg_u32 = 65535UL;
        avg_whkm_x100 = (uint16_t)avg_u32;
      }
      display.setCursor(0, 0); display.print((const __FlashStringHelper *) statsAvgWhKm); display.print(':');
      display.setCursor(65, 0);
      uint16_t av_i = avg_whkm_x100 / 100; uint16_t av_f = avg_whkm_x100 % 100;
      if (av_i < 100) display.print(' '); if (av_i < 10) display.print(' ');
      display.print(av_i); display.print('.'); if (av_f < 10) display.print('0'); display.print(av_f); display.print(' '); display.print(F("Wh/km"));

      display.setCursor(0, 2);
      if (!showPower) {
        display.print((const __FlashStringHelper *) statsMaxA); display.print(' '); display.setCursor(59, 2);
        uint16_t c_i = tripMaxCurrent_cA / 100; uint16_t c_f = tripMaxCurrent_cA % 100;
        if (c_i < 100) display.print(' '); if (c_i < 10) display.print(' ');
        display.print(c_i); display.print('.'); if (c_f < 10) display.print('0'); display.print(c_f); display.print(' '); display.print((const __FlashStringHelper *) l_a);
      } else {
        display.print((const __FlashStringHelper *) statsMaxW); display.print(' '); display.setCursor(59, 2);
        uint32_t w100 = tripMaxPower_Wx100; uint16_t w_i = w100 / 100; uint16_t w_f = w100 % 100;
        if (w_i < 1000) display.print(' '); if (w_i < 100) display.print(' '); if (w_i < 10) display.print(' ');
        display.print(w_i); display.print('.'); if (w_f < 10) display.print('0'); display.print(w_f); display.print(' '); display.print((const __FlashStringHelper *) l_w);
      }

      display.setCursor(0, 4); display.print((const __FlashStringHelper *) statsUmin); display.print(' ');
      display.setCursor(72, 4);
      uint16_t vmin_i = (tripMinVoltage_cV == 0xFFFF) ? 0 : (tripMinVoltage_cV / 100);
      uint16_t vmin_f = (tripMinVoltage_cV == 0xFFFF) ? 0 : (tripMinVoltage_cV % 100);
      if (vmin_i < 10) display.print(' ');
      display.print(vmin_i); display.print('.'); if (vmin_f < 10) display.print('0'); display.print(vmin_f); display.print(' '); display.print((const __FlashStringHelper *) l_v);

      display.setCursor(0, 6); display.print((const __FlashStringHelper *) statsUmax); display.print(' ');
      display.setCursor(72, 6);
      uint16_t vmax_i = tripMaxVoltage_cV / 100; uint16_t vmax_f = tripMaxVoltage_cV % 100;
      if (vmax_i < 10) display.print(' ');
      display.print(vmax_i); display.print('.'); if (vmax_f < 10) display.print('0'); display.print(vmax_f); display.print(' '); display.print((const __FlashStringHelper *) l_v);
      return;
    }
#if defined(ARDUINO_ARCH_ESP32)
    else if (screenToShow == 3) {
      // Temperatures screen: Batt T1/T2 and DRV temp
  // Full clear on ESP32 to avoid overdraw
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(13);
#endif
      display.set1X(); display.setFont(defaultFont);
      int16_t t1 = (int16_t)S25C31.temp1;
      int16_t t2 = (int16_t)S25C31.temp2;
      int16_t tdrv10 = S23CB0.mainframeTemp; int16_t tdrv = tdrv10 / 10;
#ifdef US_Version
      auto c2f = [](int16_t c){ return (int16_t)(c * 9 / 5 + 32); };
      t1 = c2f(t1); t2 = c2f(t2); tdrv = c2f(tdrv);
#endif
      display.setCursor(0, 0); display.print((const __FlashStringHelper *) tempBatt);
      display.setFont(stdNumb);
      display.setCursor(0, 1); if (t1 < 10 && t1 > -10) display.print(' '); display.print(t1);
#ifdef US_Version
      display.setFont(defaultFont); display.print('F');
#else
      display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_c);
#endif
      display.setFont(stdNumb);
      display.setCursor(87, 1); if (t2 < 10 && t2 > -10) display.print(' '); display.print(t2);
#ifdef US_Version
      display.setFont(defaultFont); display.print('F');
#else
      display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_c);
#endif
      display.setFont(defaultFont); display.setCursor(0, 5); display.print((const __FlashStringHelper *) tempDrv);
      display.setFont(stdNumb); display.setCursor(0, 6); if (tdrv < 10 && tdrv > -10) display.print(' '); display.print(tdrv);
#ifdef US_Version
      display.setFont(defaultFont); display.print('F');
#else
      display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_c);
#endif
#if CFG_AHT10_ENABLE
      if (g_ahtPresent) {
        display.setFont(defaultFont);
  // Ambient on row 3 (right side)
  display.setCursor(64, 3); display.print("Amb:");
  display.setFont(stdNumb); display.setCursor(87, 3);
  int16_t ta = (int16_t)(g_ahtTempC + (g_ahtTempC >= 0 ? 0.5f : -0.5f));
#ifdef US_Version
  ta = (int16_t)(ta * 9 / 5 + 32);
#endif
  if (ta < 10 && ta > -10) display.print(' '); display.print(ta);
#ifdef US_Version
  display.setFont(defaultFont); display.print('F');
#else
  display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_c);
#endif
  // RH on row 5 (right side), under Ambient
  display.setFont(defaultFont);
  display.setCursor(64, 6); display.print("RH:");
  display.setFont(stdNumb); display.setCursor(87, 6);
  uint16_t rh_i = (uint16_t)(g_ahtHum + 0.5f); if (rh_i > 100) rh_i = 100; display.print(rh_i);
  uint8_t endCol = display.col(); if (endCol > 116) endCol = 116;
  display.setFont(defaultFont); display.set2X(); display.setCursor(endCol, 6); display.print('%'); display.set1X();
      }
#endif // CFG_AHT10_ENABLE
      return;
    }
#endif // ESP32 temps screen
  }

  // Default main screen
  // Full clear on ESP32 for main screen to avoid residual glyphs when digits shrink
#if defined(ARDUINO_ARCH_ESP32)
  display.clear();
#else
  displayClear(0);
#endif
  m365_info.milh = S23CB0.mileageCurrent / 100;
  m365_info.mill = S23CB0.mileageCurrent % 100;
  m365_info.Min = S23C3A.ridingTime / 60;
  m365_info.Sec = S23C3A.ridingTime % 60;
  // Select main temperature source
  int16_t tempC = S23CB0.mainframeTemp / 10; // DRV default
  switch (mainTempSource) {
    case 1: tempC = (int16_t)S25C31.temp1; break; // Batt T1
    case 2: tempC = (int16_t)S25C31.temp2; break; // Batt T2
#if defined(ARDUINO_ARCH_ESP32) && CFG_AHT10_ENABLE
    case 3: tempC = (int16_t)(g_ahtTempC + (g_ahtTempC >= 0 ? 0.5f : -0.5f)); break; // Ambient
#endif
    default: break; // DRV
  }
  m365_info.temp = tempC;
#ifdef US_Version
  m365_info.milh = m365_info.milh/1.609; m365_info.mill = m365_info.mill/1.609; m365_info.temp = m365_info.temp*9/5+32;
#endif
  display.set1X(); display.setFont(stdNumb); display.setCursor(0, 0);
  if (!showVoltageMain) {
    if (m365_info.sph < 10) display.print(' ');
    display.print(m365_info.sph); display.print('.'); display.print(m365_info.spl);
    display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_kmh); display.setFont(stdNumb);
  } else {
    uint16_t vh = m365_info.vh; uint16_t vl = m365_info.vl; if (vh < 10) display.print(' ');
    display.print(vh); display.print('.'); if (vl < 10) display.print('0'); display.print(vl);
    uint8_t __ux = display.col(); uint8_t __uy = display.row(); display.setFont(defaultFont); display.setCursor(__ux, __uy + 1); display.print((const __FlashStringHelper *) l_v); display.setFont(stdNumb);
  }
  display.setCursor(95, 0);
  if (m365_info.temp < 10) display.print(' '); display.print(m365_info.temp);
#ifdef US_Version
  display.setFont(defaultFont); display.print('F'); display.setFont(stdNumb);
#else
  display.setFont(defaultFont); display.print((const __FlashStringHelper *) l_c); display.setFont(stdNumb);
#endif
  display.setCursor(0, 2);
  if (m365_info.milh < 10) display.print(' '); display.print(m365_info.milh); display.print('.'); if (m365_info.mill < 10) display.print('0'); display.print(m365_info.mill);
  { uint8_t __ux = display.col(); uint8_t __uy = display.row(); display.setFont(defaultFont); display.setCursor(__ux, __uy + 1); display.print((const __FlashStringHelper *) l_km); display.setFont(stdNumb); }
  display.setCursor(0, 4);
  if (m365_info.Min < 10) display.print('0'); display.print(m365_info.Min); display.print(':'); if (m365_info.Sec < 10) display.print('0'); display.print(m365_info.Sec);
  if (!showPower) {
    display.setCursor(60, 4);
    uint8_t startCol = display.col(); if (m365_info.curh < 10) display.print(' '); display.print(m365_info.curh); display.print('.'); if (m365_info.curl < 10) display.print('0'); display.print(m365_info.curl);
    uint8_t endCol = display.col(); uint8_t printed = endCol - startCol; for (uint8_t k = printed; k < 7; k++) display.print(' ');
    { uint8_t __ux = endCol; uint8_t __uy = display.row(); display.setFont(defaultFont); display.setCursor(__ux, __uy + 1); display.print((const __FlashStringHelper *) l_a); display.setFont(stdNumb); }
  } else {
    display.setCursor(55, 4);
    char d[5]; uint16_t W = m365_info.pwh; if (W > 9999) W = 9999; uint8_t len = 0;
    if (W >= 1000) { d[len++] = '0' + (W / 1000) % 10; }
    if (W >= 100)  { d[len++] = '0' + (W / 100) % 10; }
    if (W >= 10)   { d[len++] = '0' + (W / 10) % 10; }
    d[len++] = '0' + (W % 10); d[len] = 0;
    const uint8_t FIELD = 6; for (uint8_t k = 0; k < FIELD - len; k++) display.print(' ');
    display.print(d); uint8_t endCol = display.col(); { uint8_t __ux = endCol; uint8_t __uy = display.row(); display.setFont(defaultFont); display.setCursor(__ux, __uy + 1); display.print((const __FlashStringHelper *) l_w); display.setFont(stdNumb); }
  }
  showBatt(S25C31.remainPercent, cur_cA_raw < 0);
  showRangeSmall();
  
  // -------------------------------------------------------------------------
  // BUS inactivity overlay (non-SIM). Draw AFTER main content so it appears
  // as a popup frame. Conditions:
  //  - No simulator (SIM_MODE not defined)
  //  - Either we've never seen data and uptime > 1500ms, OR last frame older than threshold
  //  - Threshold scales: show seconds counter (since first attempt or since last data)
  // -------------------------------------------------------------------------
#if !defined(SIM_MODE)
  {
    const uint32_t now = millis();
  const uint32_t NO_FIRST_THRESHOLD = 2000;   // delay before showing initially (was 1500)
  const uint32_t STALE_THRESHOLD    = 2000;   // ms gap after last frame (was 750)
    bool showOverlay = false;
    uint32_t deltaMs = 0;
    if (!g_busEverSeen) {
      if (now > NO_FIRST_THRESHOLD) { showOverlay = true; deltaMs = now; }
    } else {
      uint32_t last = g_lastBusDataMs; if (last == 0) last = now; // safety
      uint32_t gap = now - last;
      if (gap > STALE_THRESHOLD) { showOverlay = true; deltaMs = gap; }
    }
    if (showOverlay) {
  // Character-grid centered window: 12 cols x 4 rows
  const uint8_t COLS = 12;           // characters (12*8 = 96px)
  const uint8_t ROWS = 4;            // characters (4*8 = 32px)
  const uint8_t TOTAL_COLS = 128/8;  // 16
  const uint8_t TOTAL_ROWS = 64/8;   // 8
  const uint8_t xChar = (TOTAL_COLS - COLS) / 2; // 2
  const uint8_t yChar = (TOTAL_ROWS - ROWS) / 2; // 2
  const uint8_t x = xChar * 8;
  const uint8_t y = yChar * 8;
  const uint8_t w = COLS * 8;
  const uint8_t h = ROWS * 8;

#if defined(ARDUINO_ARCH_ESP32)
  // Clear background area (black) then draw frame and header bar
  display.setDrawColor(0); display.drawBox(x, y, w, h); // opaque background
  display.setDrawColor(1); display.drawFrame(x, y, w, h);
  display.drawBox(x, y, w, 8); // header bar (exact 1 text row)
#endif
  display.setFont(defaultFont);
  // Header text (center-ish inside header bar)
  display.setCursor(xChar + 20, yChar); display.print("BUS");
  // Body lines
  display.setCursor(xChar + 20, yChar + 1); display.print("no data");
  float secs = (float)deltaMs / 1000.0f; if (secs > 99.9f) secs = 99.9f;
  char buf[12]; snprintf(buf, sizeof(buf), "%.1fs", secs);
  display.setCursor(xChar + 30, yChar + 2); display.print(buf);
  // leave last row empty for spacing / future info
    }
  }
#endif // !SIM_MODE

}
