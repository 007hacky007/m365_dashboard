/*
 * M365 OLED Dashboard - Main Program
 * 
 * This program creates a custom dashboard for Xiaomi M365 scooters using an OLED display.
 * It communicates with the scooter's internal bus to read data and send commands.
 * 
 * Features:
 * - Real-time speed, battery, current, temperature display
 * - Multiple display modes (normal, big speedometer, battery details)
 * - Configuration menus for display and scooter settings
 * - Multi-language support
 * - Settings persistence in EEPROM
 */

#define M365_DEFINE_GLOBALS
#include "defines.h"
#include "oled_utils.h"
#include "battery_display.h"
#include "comms.h"
#include "display_fsm.h"
#include "range_estimator.h"
#include "aht10.h"
#ifdef SIM_MODE
#include "sim.h"
#endif

#if defined(ARDUINO_ARCH_AVR) && defined(SIM_MODE)
#include <avr/wdt.h>
#endif

// Optional simulator for Wokwi/desktop runs: feeds synthetic scooter data
#ifdef SIM_MODE
// Simulator helpers are declared in sim.h
#endif

#if defined(ARDUINO_ARCH_ESP32)
// ESP32 WiFi AP + OTA web update
WebServer otaServer(80);
bool otaRunning = false;
String otaSSID;
String otaPASS;
void otaBegin();
void otaEnd();
void otaService();
#endif

// ============================================================================
// DISPLAY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Clear display only when switching between different screen modes
 * This prevents unnecessary flicker during updates
 * @param ID - Screen ID to identify different display modes
 * @param force - Force clear regardless of ID
 * @return true if display was cleared, false if no action taken
 */
bool displayClear(byte ID, bool force) {
  volatile static uint8_t oldID = 0;

  if ((oldID != ID) || force) {
    display.clear();
    oldID = ID;
    return true;
  } else return false;
}

// ============================================================================
// WATCHDOG INTERRUPT HANDLER
// ============================================================================

/**
 * Watchdog timer interrupt handler
 * Resets the system if it gets stuck (after 3 timeouts)
 * This prevents the dashboard from freezing permanently
 */
void WDTint_() {
  if (WDTcounts > 2) {
    WDTcounts = 0;
    resetFunc();  // Software reset
  } else WDTcounts++;
}

// ============================================================================
// SYSTEM INITIALIZATION
// ============================================================================

void setup() {
  // Initialize serial communication with M365 scooter at 115200 baud
  SERIAL_BEGIN(115200);

#if defined(ARDUINO_ARCH_AVR) && defined(SIM_MODE)
  // In Wokwi/Nano simulation, disable hardware WDT if previously enabled
  // to avoid unexpected resets during simulated long operations.
  MCUSR = 0;
  wdt_disable();
#endif

  // ============================================================================
  // LOAD SETTINGS FROM EEPROM
  // ============================================================================
  
  // Prepare EEPROM (ESP32 needs begin/commit). We need extra space for range ring (~64 + 13*10 = 194 bytes). Round up.
#if defined(ARDUINO_ARCH_ESP32)
  EEPROM.begin(256);
#else
  EEPROM_START(64);
#endif

  // Check if valid configuration exists (magic number 128)
  uint8_t cfgID = EEPROM.read(0);
  if (cfgID == 128) {
    // Load all saved settings from EEPROM
    autoBig = EEPROM.read(1);              // Auto big speedometer
    warnBatteryPercent = EEPROM.read(2);   // Battery warning threshold
    bigMode = EEPROM.read(3);              // Big display mode (speed/current)
    bigWarn = EEPROM.read(4);              // Big battery warning
    WheelSize = EEPROM.read(5);            // Wheel size (8.5" or 10")
    cfgCruise = EEPROM.read(6);            // Cruise control setting
    cfgTailight = EEPROM.read(7);          // Taillight setting
    cfgKERS = EEPROM.read(8);              // Regenerative braking setting
    hibernateOnBoot = EEPROM.read(9);      // One-time hibernation trigger
  showPower = EEPROM.read(10);           // Display power instead of current
#if defined(ARDUINO_ARCH_ESP32)
  showVoltageMain = EEPROM.read(12);     // Show voltage on main instead of speed
  wifiEnabled = EEPROM.read(11);         // WiFi/OTA toggle
#endif
  bigFontStyle = EEPROM.read(13);        // Big font style: 0=STD,1=DIGIT
  mainTempSource = EEPROM.read(14);      // Main temp source
  } else {
    // First time setup - save default values to EEPROM
    EEPROM.put(0, 128);                    // Magic number to indicate valid config
    EEPROM.put(1, autoBig);
    EEPROM.put(2, warnBatteryPercent);
    EEPROM.put(3, bigMode);
    EEPROM.put(4, bigWarn);
    EEPROM.put(5, WheelSize);
    EEPROM.put(6, cfgCruise);
    EEPROM.put(7, cfgTailight);
    EEPROM.put(8, cfgKERS);
  EEPROM.put(9, hibernateOnBoot);
  EEPROM.put(10, showPower);
#if defined(ARDUINO_ARCH_ESP32)
  EEPROM.put(11, wifiEnabled);
#endif
  EEPROM.put(12, showVoltageMain);
    EEPROM.put(13, bigFontStyle);
    EEPROM.put(14, mainTempSource);
  EEPROM_COMMIT();
  }

  // ============================================================================
  // INITIALIZE DISPLAY
  // ============================================================================
  
  oledInit(false); // Centralized OLED init (no splash/logo here)
  // Initialize range estimator after EEPROM is ready and before loop
  rangeInit();

#if defined(ARDUINO_ARCH_ESP32) && CFG_AHT10_ENABLE
  // Initialize optional AHT10 on I2C (same bus as OLED)
  aht10Init();
#endif
  
  // Optional: Invert display for better visibility on yellow/blue OLEDs
  // display.displayRemap(true);
  
  // Show M365 logo/text briefly during startup
  displayClear(255, true);
#if defined(ARDUINO_ARCH_ESP32)
  display.setFont(stdNumb);
  display.setCursor(0, 2);
  display.print("M365");
  display.setFont(defaultFont);
  display.setCursor(0, 4);
  display.print("Dashboard");
  displayCommit();
#else
  display.setFont(m365);
  display.setCursor(0, 0);
  display.print((char)0x20);              // M365 logo character
  display.setFont(defaultFont);
#endif

#ifdef SIM_MODE
  // Initialize simulated telemetry values before first frame
  SERIAL_BEGIN(115200); // open USB serial for SIM control input
  simInit();
#endif

  // ============================================================================
  // HIBERNATION MODE DETECTION (Throttle + Brake during logo display OR EEPROM setting)
  // ============================================================================
  
#ifndef SIM_MODE
  bool hibernationDetected = hibernateOnBoot;  // Check EEPROM hibernation setting first
  
  // If hibernation was set via menu, clear the EEPROM flag immediately
  if (hibernateOnBoot) {
    hibernateOnBoot = false;
    EEPROM.put(9, hibernateOnBoot);        // Clear hibernation flag in EEPROM
    EEPROM_COMMIT();
  }
  
  // Also check for manual hibernation trigger during logo display (2 second window)
  uint32_t hibernationWindow = millis() + 2000;
  
  while (millis() < hibernationWindow && !hibernationDetected) {
    dataFSM();                             // Process incoming data to get throttle/brake values
    if (_Query.prepared == 0) prepareNextQuery();  // Prepare next data request
    Message.Process();                     // Handle messaging
    
    // Check if both throttle and brake are engaged during logo display
    if ((S20C00HZ65.throttle > 150) && (S20C00HZ65.brake > 60)) {
      hibernationDetected = true;
      break;  // Exit loop immediately when hibernation is triggered
    }
  }
  
  // If hibernation was detected (either via EEPROM or manual trigger), enable it
  if (hibernationDetected) {
    _Hibernate = true;                     // Enable hibernation mode
    // Show a simple status so users know it's intentional
    displayClear(0, true);
    display.setFont(defaultFont);
    display.set1X();
    display.setCursor(0, 0);
    display.print("Hibernation");
    display.setCursor(0, 1);
    display.print("mode enabled");
  // Orderly shutdown: persist learned range if changed
  rangeCheckpointIfNeeded();
  }
#endif // SIM_MODE  // end hibernation detection (skipped in SIM)

#if defined(ARDUINO_ARCH_ESP32)
  if (wifiEnabled) otaBegin();
#endif

  // Ensure splash is fully cleared before the first main frame draws
  display.clear();
  displayCommit();
}

// Communication and query helpers moved to comms.{h,cpp}

#if defined(ARDUINO_ARCH_ESP32)
// ========================= OTA WiFi AP helpers =========================
void otaRoutes() {
  static const char indexHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>M365 OTA</title></head><body><h2>M365 Dashboard OTA Update</h2>"
    "<p>Build target: ESP32</p>"
    "<p><a href=\"/update\">Upload new firmware</a></p>"
    "</body></html>";
  static const char updateHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Update</title></head><body><h2>Upload .bin</h2>"
    "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">"
    "<input type=\"file\" name=\"update\"><input type=\"submit\" value=\"Upload\"></form>"
    "</body></html>";

  otaServer.on("/", HTTP_GET, [=]() { otaServer.send(200, "text/html", indexHtml); });
  otaServer.on("/update", HTTP_GET, [=]() { otaServer.send(200, "text/html", updateHtml); });
  otaServer.on("/update", HTTP_POST,
    [](){
      bool ok = !Update.hasError();
      otaServer.send(200, "text/plain", ok ? "OK. Rebooting..." : "Update failed");
      delay(500);
      if (ok) ESP.restart();
    },
    [](){
      HTTPUpload& upload = otaServer.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
      }
    }
  );
}

void otaBegin() {
  if (otaRunning) return;
  // Build SSID from MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssidBuf[24];
  snprintf(ssidBuf, sizeof(ssidBuf), "M365-OTA-%02X%02X", mac[4], mac[5]);
  otaSSID = ssidBuf;
  if (otaPASS.length() < 8) otaPASS = "m365ota123"; // 8+ chars

  WiFi.mode(WIFI_AP);
  WiFi.softAP(otaSSID.c_str(), otaPASS.c_str());
  delay(100);
  otaRoutes();
  otaServer.begin();
  otaRunning = true;
}

void otaEnd() {
  if (!otaRunning) return;
  otaServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  otaRunning = false;
}

void otaService() {
  otaServer.handleClient();
}
#endif

// Simulator helpers moved to sim.{h,cpp}


// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
#ifdef SIM_MODE
  simTick();
#else
  dataFSM();
  if (_Query.prepared == 0 && !_Hibernate) prepareNextQuery();
  if (_NewDataFlag) { _NewDataFlag = 0; Message.Process(); }
#endif

  // Update display according to current state and inputs
  displayFSM();
  displayCommit();
  // Update range learner regularly
  rangeTick();

#if defined(ARDUINO_ARCH_ESP32) && CFG_AHT10_ENABLE
  // Opportunistically refresh AHT10 at ~2 Hz without blocking UI too much
  static uint32_t nextAht = 0; uint32_t nowA = millis();
  if ((int32_t)(nowA - nextAht) >= 0) {
    nextAht = nowA + 500;
    float t, h; (void)aht10Read(t, h);
  }
#endif

  // Service OLED (I2C recovery / yield)
  oledService();

#if defined(ARDUINO_ARCH_ESP32)
  if (wifiEnabled) otaService();
#endif
}
