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

#include "defines.h"

// Optional simulator for Wokwi/desktop runs: feeds synthetic scooter data
#ifdef SIM_MODE
static void simInit();
static void simTick();
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
bool displayClear(byte ID = 1, bool force = false) {
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

  // ============================================================================
  // LOAD SETTINGS FROM EEPROM
  // ============================================================================
  
  // Prepare EEPROM (ESP32 needs begin/commit)
  EEPROM_START(64);

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
  EEPROM_COMMIT();
  }

  // ============================================================================
  // INITIALIZE DISPLAY
  // ============================================================================
  
  oledInit(false); // Centralized OLED init (no splash/logo here)
  
  // Optional: Invert display for better visibility on yellow/blue OLEDs
  // display.displayRemap(true);
  
  // Show M365 logo briefly during startup
  display.setFont(m365);
  displayClear(0, true);
  display.setCursor(0, 0);
  display.print((char)0x20);              // M365 logo character
  display.setFont(defaultFont);

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
    
    // Display hibernation status message
    displayClear(0, true);
    display.set2X();                       // Large text for hibernation message
    display.setCursor(0, 0);
    display.println(F("HIBERNATE"));
    display.println(F("MODE"));
    display.set1X();
    display.setCursor(0, 48);
    display.println(F("Dashboard disabled"));
    display.println(F("Power cycle to reset"));
    
    // Enter infinite loop - hibernation mode active
    // Communication queries are disabled via _Hibernate flag in processPacket()
    while (true) {
      dataFSM();                           // Still process any incoming data
      Message.Process();                   // Handle messaging
      WDTcounts = 0;                       // Reset watchdog to prevent reset
      delay(100);                          // Small delay to reduce CPU usage
    }
  }
#else
  // In simulator mode, initialize synthetic data and skip hibernation logic
  simInit();
#endif

  // ============================================================================
  // WAIT FOR SCOOTER DATA OR DETECT NO CONNECTION
  // ============================================================================
  
  // Wait for scooter data (skip when simulating)
#ifndef SIM_MODE
  uint32_t wait = millis() + 2000;
  while ((wait > millis()) || ((wait - 1000 > millis()) && (S25C31.current != 0) && (S25C31.voltage != 0) && (S25C31.remainPercent != 0))) {
    dataFSM();                             // Process incoming data
    if (_Query.prepared == 0) prepareNextQuery();  // Prepare next data request
    Message.Process();                     // Handle messaging
  }

  // Check if we received any data from the scooter
  if ((S25C31.current == 0) && (S25C31.voltage == 0) && (S25C31.remainPercent == 0)) {
    // No data received - display error message
    displayClear(1);
    display.set2X();                       // Large text for error message
    display.setCursor(0, 0);
    display.println((const __FlashStringHelper *) noBUS1);  // "BUS not"
    display.println((const __FlashStringHelper *) noBUS2);  // "connected!"
    display.println((const __FlashStringHelper *) noBUS3);  // "No data to"
    display.println((const __FlashStringHelper *) noBUS4);  // "display!"
    display.set1X();
  } else displayClear(1);
#else
  // Ensure first screen clears in simulator
  displayClear(1);
#endif

  // ============================================================================
  // START WATCHDOG TIMER
  // ============================================================================
  
  WDTcounts = 0;
  WatchDog::init(WDTint_, 500);           // 500ms watchdog interval
}

// ============================================================================
// MAIN PROGRAM LOOP
// ============================================================================

/**
 * Main loop - runs continuously
 * Cycle time without data exchange is approximately 8 microseconds
 */
void loop() {
  // Drive synthetic data when simulating
#ifdef SIM_MODE
  simTick();
#endif
  // Process incoming data from M365 scooter
  dataFSM();

  // Prepare next query if current one has been sent
  if (_Query.prepared == 0) prepareNextQuery();

  // Update display when new data arrives
  if (_NewDataFlag == 1) {
    _NewDataFlag = 0;
  oledService();               // Ensure I2C is healthy before heavy drawing
  oledBusy = true;
  displayFSM();                // Handle all display logic
  oledBusy = false;
  }

  // Process communication messages
  Message.Process();
  Message.ProcessBroadcast();

  // Reset watchdog counter to prevent system reset
  WDTcounts = 0;

  // Periodic I2C bus health check and auto-recovery
  oledService();
}

// =============================================================================
// I2C/OLED AUTO-RECOVERY HELPERS
// =============================================================================

// I2C speed management state
#ifdef DISPLAY_I2C
static bool s_i2cFast = false;     // false: 100kHz, true: 400kHz
static uint8_t s_consecOK = 0;     // consecutive OK pings
#endif

// Attempt to detect and recover a stuck I2C bus, then verify OLED presence
bool i2cCheckAndRecover() {
#ifdef DISPLAY_I2C
  // Quick ping: is OLED responding?
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  uint8_t err = Wire.endTransmission();
  if (err == 0) return true; // All good

  // Try to unstick the bus by toggling SCL and issuing a STOP
  // Use SDA/SCL pin defines provided by core
  #if defined(SDA) && defined(SCL)
    pinMode(SDA, INPUT_PULLUP);
    pinMode(SCL, INPUT_PULLUP);

  // If SDA is low, clock SCL up to 20 cycles to free the line
    if (digitalRead(SDA) == LOW) {
      pinMode(SCL, OUTPUT);
      for (uint8_t i = 0; i < 20 && digitalRead(SDA) == LOW; i++) {
        digitalWrite(SCL, HIGH);
        delayMicroseconds(5);
        digitalWrite(SCL, LOW);
        delayMicroseconds(5);
      }
      pinMode(SCL, INPUT_PULLUP);
    }

  // Send a STOP condition manually
    pinMode(SDA, OUTPUT);
    digitalWrite(SDA, LOW);
    delayMicroseconds(5);
    pinMode(SCL, OUTPUT);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(SDA, HIGH);
    delayMicroseconds(5);
    pinMode(SDA, INPUT_PULLUP);
    pinMode(SCL, INPUT_PULLUP);
  #endif

  // Reinit I2C peripheral
  #if defined(WIRE_HAS_END)
  Wire.end();
  #endif
  delay(1);
  Wire.begin();
  Wire.setClock(100000L); // start slow for stability
  #ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(25000, true); // 25ms, reset hardware on timeout
  #endif
  delay(1);

  // Re-check device presence
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  err = Wire.endTransmission();
  s_i2cFast = false;
  s_consecOK = 0;
  return err == 0;
#else
  return true;
#endif
}

// Initialize the OLED display and I2C/SPI bus
void oledInit(bool showLogo) {
#ifdef DISPLAY_I2C
  #if defined(WIRE_HAS_END)
    Wire.end();
  #endif
  Wire.begin();
  Wire.setClock(100000L); // default to 100kHz at boot
  #ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(25000, true);
  #endif
  // Init controller (default: SSD1306 128x64)
  display.begin(&Adafruit128x64, OLED_I2C_ADDRESS);
  //display.begin(&SH1106_128x64, 0x3C);       // Alternative: 1.3" OLED
  s_i2cFast = false;
  s_consecOK = 0;
#endif
#ifdef DISPLAY_SPI
  display.begin(&Adafruit128x64, PIN_CS, PIN_DC, PIN_RST);
  //display.begin(&SH1106_128x64, PIN_CS, PIN_DC, PIN_RST); // Alternative: 1.3" OLED
#endif
  display.setFont(defaultFont);
  if (showLogo) {
    display.clear();
    display.setFont(m365);
    display.setCursor(0, 0);
    display.print((char)0x20);
    display.setFont(defaultFont);
  }
}

// Lightweight periodic health check; re-inits OLED upon hiccup
void oledService() {
#ifdef DISPLAY_I2C
  static uint32_t nextCheck = 0;
  uint32_t now = millis();
  if ((int32_t)(now - nextCheck) >= 0) {
    nextCheck = now + 500; // check every 500ms
    // Avoid re-initialization during drawing to prevent tearing
    if (oledBusy) return;
    if (i2cCheckAndRecover()) {
      // Stability-based promotion to 400kHz
      if (!s_i2cFast) {
        if (s_consecOK < 255) s_consecOK++;
        if (s_consecOK >= 8) { // ~4 seconds of OK at 500ms cadence
          Wire.setClock(400000L);
          s_i2cFast = true;
        }
      }
    } else {
      // Failure: demote to 100kHz and re-init OLED
      #ifdef DISPLAY_I2C
        Wire.setClock(100000L);
      #endif
      s_i2cFast = false;
      s_consecOK = 0;
      if (!oledBusy) {
        oledInit(false);
      }
    }
  }
#endif
}

// ============================================================================
// BATTERY DISPLAY FUNCTION
// ============================================================================

/**
 * Display battery percentage bar at bottom of screen
 * @param percent - Battery percentage (0-100)
 * @param blinkIt - Make battery bar blink (when regenerating)
 */
void showBatt(int percent, bool blinkIt) {
  display.set1X();
  display.setFont(defaultFont);
  display.setCursor(0, 7);                // Bottom row of display
  
  // Show battery bar if not in warning mode or warning is disabled
  if (bigWarn || (warnBatteryPercent == 0) || (percent > warnBatteryPercent) || ((warnBatteryPercent != 0) && (millis() % 1000 < 500))) {
    // Draw battery icon and bar
    display.print((char)0x81);            // Battery left cap
    
    // Draw 19 segments of battery bar
    for (int i = 0; i < 19; i++) {
      display.setCursor(5 + i * 5, 7);
      if (blinkIt && (millis() % 1000 < 500))
        display.print((char)0x83);        // Empty segment (blinking)
      else
        if (float(19) / 100 * percent > i)
          display.print((char)0x82);      // Filled segment
        else
          display.print((char)0x83);      // Empty segment
    }
    
    // Draw battery right cap and percentage text
    display.setCursor(99, 7);
    display.print((char)0x84);            // Battery right cap
    if (percent < 100) display.print(' '); // Right-align percentage
    if (percent < 10) display.print(' ');
    display.print(percent);
    display.print('%');
  } else {
    // Clear battery area when in warning mode
    for (int i = 0; i < 34; i++) {
      display.setCursor(i * 5, 7);
      display.print(' ');
    }
  }
}

// ============================================================================
// BATTERY INFORMATION DISPLAY
// ============================================================================

/**
 * Display detailed battery information screen
 * Shows voltage, current, capacity, temperatures, and individual cell voltages
 */
void fsBattInfo() {
  displayClear(6);
  int16_t tmp_0, tmp_1;
  display.setCursor(0, 0);
  display.set1X();

  // ============================================================================
  // TOP LINE: VOLTAGE, CURRENT, CAPACITY
  // ============================================================================
  
  // Display battery voltage (XX.XX V)
  tmp_0 = abs(S25C31.voltage) / 100;         
  tmp_1 = abs(S25C31.voltage) % 100;
  if (tmp_0 < 10) display.print(' ');       // Right align
  display.print(tmp_0);
  display.print('.');
  if (tmp_1 < 10) display.print('0');       // Leading zero for decimals
  display.print(tmp_1);
  display.print((const __FlashStringHelper *) l_v);
  display.print(' ');

  // Display current or power depending on setting
  if (!showPower) {
    // Current (XX.XX A)
    tmp_0 = abs(S25C31.current) / 100;       
    tmp_1 = abs(S25C31.current) % 100;
    if (tmp_0 < 10) display.print(' ');
    display.print(tmp_0);
    display.print('.');
    if (tmp_1 < 10) display.print('0');
    display.print(tmp_1);
    display.print((const __FlashStringHelper *) l_a);
  } else {
    // Power (XXX.XX W) computed from voltage and current
    uint32_t ai = (uint32_t)abs(S25C31.current);
    uint32_t vi = (uint32_t)abs(S25C31.voltage);
    uint32_t m = (ai * vi + 50) / 100; // centi-watts
    tmp_0 = (int16_t)(m / 100);
    tmp_1 = (int16_t)(m % 100);
    if (tmp_0 < 10) display.print(' ');
    display.print(tmp_0);
    display.print('.');
    if (tmp_1 < 10) display.print('0');
    display.print(tmp_1);
    display.print((const __FlashStringHelper *) l_w);
  }
  display.print(' ');
  
  // Display remaining capacity (XXXX mAh)
  if (S25C31.remainCapacity < 1000) display.print(' ');
  if (S25C31.remainCapacity < 100) display.print(' ');
  if (S25C31.remainCapacity < 10) display.print(' ');
  display.print(S25C31.remainCapacity);
  display.print((const __FlashStringHelper *) l_mah);
  
  // ============================================================================
  // SECOND LINE: BATTERY TEMPERATURES
  // ============================================================================
  
  int temp;
  temp = S25C31.temp1 - 20;                // Convert to actual temperature
  display.setCursor(9, 1);
  display.print((const __FlashStringHelper *) l_t);
  display.print("1: ");
  if (temp < 10) display.print(' ');
  display.print(temp);
  display.print((char)0x80);               // Degree symbol
  display.print("C");
  
  display.setCursor(74, 1);
  display.print((const __FlashStringHelper *) l_t);
  display.print("2: ");
  temp = S25C31.temp2 - 20;
  if (temp < 10) display.print(' ');
  display.print(temp);
  display.print((char)0x80);
  display.print("C");

  // ============================================================================
  // DISPLAY INDIVIDUAL CELL VOLTAGES (10 cells in 2 columns)
  // ============================================================================
  
  int16_t v;
  int16_t * ptr;
  int16_t * ptr2;
  ptr = (int16_t*)&S25C40;                 // Pointer to cell data
  ptr2 = ptr + 5;                          // Pointer to cells 5-9
  
  // Display cells 0-4 and 5-9 in two columns
  for (uint8_t i = 0; i < 5; i++) {
    // Left column: Cells 0-4
    display.setCursor(5, 2 + i);
    display.print(i);
    display.print(": ");
    v = *ptr / 1000;                       // Convert to volts
    display.print(v);
    display.print('.');
    v = *ptr % 1000;                       // Get decimal part
    if (v < 100) display.print('0');
    if (v < 10) display.print('0');
    display.print(v);
    display.print((const __FlashStringHelper *) l_v);

    // Right column: Cells 5-9
    display.setCursor(70, 2 + i);
    display.print(i + 5); 
    display.print(": ");
    v = *ptr2 / 1000;
    display.print(v);
    display.print('.');
    v = *ptr2 % 1000;
    if (v < 100) display.print('0');
    if (v < 10) display.print('0');
    display.print(v);
    display.print((const __FlashStringHelper *) l_v);

    ptr++;                                 // Move to next cell
    ptr2++;
  }
}

// ============================================================================
// MAIN DISPLAY STATE MACHINE
// ============================================================================

/**
 * Main display function - handles all screen modes and user input
 * This is the heart of the user interface system
 */
void displayFSM() {
  // Local structure to hold processed display data
  struct {
    uint16_t curh;                       // Current high part (before decimal)
    uint16_t curl;                       // Current low part (after decimal)
  uint16_t pwh;                        // Power high part (W integer)
  uint16_t pwl;                        // Power low part (W decimal, 0-99)
    uint16_t vh;                         // Voltage high part
    uint16_t vl;                         // Voltage low part
    uint32_t sph;                        // Speed high part
    uint16_t spl;                        // Speed low part (decimal)
    uint16_t milh;                       // Mileage high part
    uint16_t mill;                       // Mileage low part
    uint16_t Min;                        // Minutes
    uint16_t Sec;                        // Seconds
    uint16_t temp;                       // Temperature
  } m365_info;

  // Input state variables
  int brakeVal = -1;                     // -1=min, 0=neutral, 1=max
  int throttleVal = -1;                  // -1=min, 0=neutral, 1=max

  int tmp_0, tmp_1;
  long _speed;
  long c_speed;                          // Current speed (processed)
  
  // ============================================================================
  // SPEED CALCULATION WITH HIGH SPEED SUPPORT
  // ============================================================================
  
  // Handle speeds over 32.767 km/h (16-bit signed integer overflow)
  if (S23CB0.speed < -10000) {
    // For speeds above 32.767 km/h, the value wraps to negative
    // Convert back to positive by adding the overflow amount
    c_speed = S23CB0.speed + 32768 + 32767;
  } else {
    c_speed = abs(S23CB0.speed);         // Normal speed range
  }
  
  // ============================================================================
  // WHEEL SIZE COMPENSATION
  // ============================================================================
  
  // Adjust speed reading for 10" wheels vs stock 8.5" wheels
  if (WheelSize) {
    _speed = (long) c_speed * 10 / 8.5;  // 10" wheel correction
  } else {
    _speed = c_speed;                    // 8.5" wheel (stock)
  }
 
  // Split speed into integer and decimal parts
  m365_info.sph = (uint32_t) abs(_speed) / 1000L;   // Speed integer part
  m365_info.spl = (uint16_t) c_speed % 1000 / 100;  // Speed decimal part
  
  // ============================================================================
  // UNIT CONVERSION FOR US VERSION
  // ============================================================================
  
  #ifdef US_Version
     // Convert km/h to mph
     m365_info.sph = m365_info.sph/1.609;
     m365_info.spl = m365_info.spl/1.609;
  #endif
  
  // Process other values for display
  m365_info.curh = abs(S25C31.current) / 100;       // Current integer part
  m365_info.curl = abs(S25C31.current) % 100;       // Current decimal part
  m365_info.vh = abs(S25C31.voltage) / 100;         // Voltage integer part
  m365_info.vl = abs(S25C31.voltage) % 100;         // Voltage decimal part

  // Compute power in Watts from raw units (current/100 A * voltage/100 V)
  // Multiply first to keep precision, then divide by 10000
  {
    uint32_t ai = (uint32_t)abs(S25C31.current);
    uint32_t vi = (uint32_t)abs(S25C31.voltage);
    uint32_t m = (ai * vi + 50) / 100; // scale to /100 to get centi-watts rounded
    // Now m is in centi-watts (W * 100)
    m365_info.pwh = (uint16_t)(m / 100);
    m365_info.pwl = (uint16_t)(m % 100);
  }

  // ============================================================================
  // AUTO-EXIT SETTINGS WHEN MOVING
  // ============================================================================
  
  // Exit all menus when speed exceeds 1 km/h for safety
  if ((m365_info.sph > 1) && Settings) {
    ShowBattInfo = false;
    M365Settings = false;
    Settings = false;
  }

  // ============================================================================
  // INPUT PROCESSING (Only when stopped or in settings)
  // ============================================================================
  
  // Process brake and throttle inputs only when speed is low or in settings mode
  if ((c_speed <= 200) || Settings) {
    // ============================================================================
    // BRAKE INPUT PROCESSING
    // ============================================================================
    
    // Convert analog brake value to digital states
    if (S20C00HZ65.brake > 60)
      brakeVal = 1;                      // Brake pressed (max)
    else if (S20C00HZ65.brake < 50)
      brakeVal = -1;                     // Brake released (min)
    else
      brakeVal = 0;                      // Brake neutral
      
    // ============================================================================
    // THROTTLE INPUT PROCESSING
    // ============================================================================
    
    // Convert analog throttle value to digital states
    if (S20C00HZ65.throttle > 150)
      throttleVal = 1;                   // Throttle pressed (max)
    else if (S20C00HZ65.throttle < 50)
      throttleVal = -1;                  // Throttle released (min)
    else
      throttleVal = 0;                   // Throttle neutral

    // ============================================================================
    // MAIN MENU ENTRY (Brake + Throttle both pressed)
    // ============================================================================
    
    // Enter settings menu when both brake and throttle are at maximum
    if (((brakeVal == 1) && (throttleVal == 1) && !Settings) && 
        ((oldBrakeVal != 1) || (oldThrottleVal != 1))) {
      menuPos = 0;                       // Start at first menu item
      timer = millis() + LONG_PRESS;     // Set timeout timer
      Settings = true;                   // Enter settings mode
    }

    if (M365Settings) {
      if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal == -1) && (oldBrakeVal == -1))                // brake min + throttle max = change menu value
      switch (sMenuPos) {
        case 0:
          cfgCruise = !cfgCruise;
          EEPROM.put(6, cfgCruise);
          EEPROM_COMMIT();
          break;
        case 1:
          if (cfgCruise)
            prepareCommand(CMD_CRUISE_ON);
            else
            prepareCommand(CMD_CRUISE_OFF);
          break;
        case 2:
          cfgTailight = !cfgTailight;
          EEPROM.put(7, cfgTailight);
          EEPROM_COMMIT();
          break;
        case 3:
          if (cfgTailight)
            prepareCommand(CMD_LED_ON);
            else
            prepareCommand(CMD_LED_OFF);
          break;
        case 4:
          switch (cfgKERS) {
            case 1:
              cfgKERS = 2;
              EEPROM.put(8, cfgKERS);
              EEPROM_COMMIT();
              break;
            case 2:
              cfgKERS = 0;
              EEPROM.put(8, cfgKERS);
              EEPROM_COMMIT();
              break;
            default:
              cfgKERS = 1;
              EEPROM.put(8, cfgKERS);
              EEPROM_COMMIT();
          }
          break;
        case 5:
          switch (cfgKERS) { 
            case 1:
              prepareCommand(CMD_MEDIUM);
              break;
            case 2:
              prepareCommand(CMD_STRONG);
              break;
            default: 
              prepareCommand(CMD_WEAK);
          }
          break;
        case 6:
          WheelSize = !WheelSize;
          EEPROM.put(5, WheelSize);
          EEPROM_COMMIT();
	  break;
        case 7:
          oldBrakeVal = brakeVal;
          oldThrottleVal = throttleVal;
          timer = millis() + LONG_PRESS;
          M365Settings = false;
          break;
      } else
      if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal == -1) && (oldThrottleVal == -1)) {               // brake max + throttle min = change menu position
        if (sMenuPos < 7)
          sMenuPos++;
          else
          sMenuPos = 0;
        timer = millis() + LONG_PRESS;
      }

      if (displayClear(7)) sMenuPos = 0;
      display.set1X();
      display.setCursor(0, 0);

      if (sMenuPos == 0)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr1);
      if (cfgCruise)
        display.print((const __FlashStringHelper *) l_On);
        else
        display.print((const __FlashStringHelper *) l_Off);

      display.setCursor(0, 1);

      if (sMenuPos == 1)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr2);

      display.setCursor(0, 2);

      if (sMenuPos == 2)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr3);
      if (cfgTailight)
        display.print((const __FlashStringHelper *) l_Yes);
        else
        display.print((const __FlashStringHelper *) l_No);

      display.setCursor(0, 3);

      if (sMenuPos == 3)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr4);

      display.setCursor(0, 4);

      if (sMenuPos == 4)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr5);
      switch (cfgKERS) {
        case 1:
          display.print((const __FlashStringHelper *) l_Medium);
          break;
        case 2:
          display.print((const __FlashStringHelper *) l_Strong);
          break;
        default:
          display.print((const __FlashStringHelper *) l_Weak);
          break;
      }

    display.setCursor(0, 5);

      if (sMenuPos == 5)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr6);

    display.setCursor(0, 6);
    
    if (sMenuPos == 6)
        display.print((char)0x7E);
        else
        display.print(" ");
  
      display.print((const __FlashStringHelper *) M365CfgScr7);
       if(WheelSize) {
          display.print((const __FlashStringHelper *) l_10inch);
     }else{
          display.print((const __FlashStringHelper *) l_85inch);
      }  
      //display.setCursor(0, 7);

      /*for (int i = 0; i < 25; i++) {
        display.setCursor(i * 5, 6);
        display.print('-');
      }*/

      display.setCursor(0, 7);
      
      if (sMenuPos == 7)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) M365CfgScr8);

      oldBrakeVal = brakeVal;
      oldThrottleVal = throttleVal;
 
      return;
    } else
    if (ShowBattInfo) {
      if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal == -1) && (oldThrottleVal == -1)) {
        oldBrakeVal = brakeVal;
        oldThrottleVal = throttleVal;
        timer = millis() + LONG_PRESS;
        ShowBattInfo = false;
        return;
      }

      fsBattInfo();

      display.setCursor(0, 7);
      display.print((const __FlashStringHelper *) battScr);

      oldBrakeVal = brakeVal;
      oldThrottleVal = throttleVal;
 
      return;
    } else
    if (Settings) {
      if ((brakeVal == 1) && (oldBrakeVal == 1) && (throttleVal == -1) && (oldThrottleVal == -1) && (timer != 0))
        if (millis() > timer) {
          Settings = false;
          return;
        }

      if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal == -1) && (oldBrakeVal == -1))                // brake min + throttle max = change menu value
      switch (menuPos) {
        case 0:
          autoBig = !autoBig;
          break;
        case 1:
          switch (bigMode) {
            case 0:
              bigMode = 1;
              break;
            default:
              bigMode = 0;
          }
          break;
        case 2:
          switch (warnBatteryPercent) {
            case 0:
              warnBatteryPercent = 5;
              break;
            case 5:
              warnBatteryPercent = 10;
              break;
            case 10:
              warnBatteryPercent = 15;
              break;
            default:
              warnBatteryPercent = 0;
          }
          break;
        case 3:
          bigWarn = !bigWarn;
          break;
        case 4:
          ShowBattInfo = true;
          break;
        case 5:
          M365Settings = true;
          break;
        case 6:
          showPower = !showPower;          // Toggle unit A/W
          break;
        case 7:
          hibernateOnBoot = !hibernateOnBoot;
          break;
        case 8:
          EEPROM.put(1, autoBig);
          EEPROM.put(2, warnBatteryPercent);
          EEPROM.put(3, bigMode);
          EEPROM.put(4, bigWarn);
          EEPROM.put(9, hibernateOnBoot);
          EEPROM.put(10, showPower);
          EEPROM_COMMIT();
          Settings = false;
          break;
      } else
      if ((brakeVal == 1) && (oldBrakeVal != 1) && (throttleVal == -1) && (oldThrottleVal == -1)) {               // brake max + throttle min = change menu position
        if (menuPos < 8)
          menuPos++;
          else
          menuPos = 0;
        timer = millis() + LONG_PRESS;
      }

      displayClear(2);

      display.set1X();
      display.setCursor(0, 0);

      if (menuPos == 0)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr1);
      if (autoBig)
        display.print((const __FlashStringHelper *) l_Yes);
        else
        display.print((const __FlashStringHelper *) l_No);

      display.setCursor(0, 1);

      if (menuPos == 1)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr2);
      switch (bigMode) {
        case 1:
          display.print((const __FlashStringHelper *) confScr2b);
          break;
        default:
          display.print((const __FlashStringHelper *) confScr2a);
      }

      display.setCursor(0, 2);

      if (menuPos == 2)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr3);
      switch (warnBatteryPercent) {
        case 5:
          display.print(" 5%");
          break;
        case 10:
          display.print("10%");
          break;
        case 15:
          display.print("15%");
          break;
        default:
          display.print((const __FlashStringHelper *) l_Off);
          break;
      }

      display.setCursor(0, 3);

      if (menuPos == 3)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr4);
      if (bigWarn)
        display.print((const __FlashStringHelper *) l_Yes);
        else
        display.print((const __FlashStringHelper *) l_No);

      display.setCursor(0, 4);

      if (menuPos == 4)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr5);

      display.setCursor(0, 5);

      if (menuPos == 5)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr6);

      display.setCursor(0, 6);

      if (menuPos == 6)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr9);
      if (showPower)
        display.print((const __FlashStringHelper *) l_w);
        else
        display.print((const __FlashStringHelper *) l_a);

      display.setCursor(0, 7);

      if (menuPos == 7)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr7);
      if (hibernateOnBoot)
        display.print((const __FlashStringHelper *) l_Yes);
        else
        display.print((const __FlashStringHelper *) l_No);

      display.setCursor(0, 8);

      if (menuPos == 8)
        display.print((char)0x7E);
        else
        display.print(" ");

      display.print((const __FlashStringHelper *) confScr8);

      oldBrakeVal = brakeVal;
      oldThrottleVal = throttleVal;
 
      return;
    } else
    if ((throttleVal == 1) && (oldThrottleVal != 1) && (brakeVal == -1) && (oldBrakeVal == -1)) {
      displayClear(3);

      display.set1X();
      display.setFont(defaultFont);
      display.setCursor(0, 0);
      display.print((const __FlashStringHelper *) infoScr1);
      display.print(':');
      display.setFont(stdNumb);
      display.setCursor(15, 1);
      tmp_0 = S23CB0.mileageTotal / 1000;
      tmp_1 = (S23CB0.mileageTotal % 1000) / 10;
      if (tmp_0 < 1000) display.print(' ');
      if (tmp_0 < 100) display.print(' ');
      if (tmp_0 < 10) display.print(' ');
      display.print(tmp_0);
      display.print('.');
      if (tmp_1 < 10) display.print('0');
      display.print(tmp_1);
      display.setFont(defaultFont);
      display.print((const __FlashStringHelper *) l_km);

      display.setCursor(0, 5);
      display.print((const __FlashStringHelper *) infoScr2);
      display.print(':');
      display.setFont(stdNumb);
      display.setCursor(15, 6);
      tmp_0 = S23C3A.powerOnTime / 60;
      tmp_1 = S23C3A.powerOnTime % 60;
      if (tmp_0 < 100) display.print(' '); 
      if (tmp_0 < 10) display.print(' ');
      display.print(tmp_0);
      display.print(':');
      if (tmp_1 < 10) display.print('0');
      display.print(tmp_1);

      return;
    }

    oldBrakeVal = brakeVal;
    oldThrottleVal = throttleVal;
  }

  if (bigWarn && (((warnBatteryPercent > 0) && (S25C31.remainPercent <= warnBatteryPercent)) && (millis() % 2000 < 700))) {
    if (displayClear(4)) {
      display.setFont(m365);
      display.setCursor(0, 0);
      display.print((char)0x21);
      display.setFont(defaultFont);
    }
  } else
    if ((m365_info.sph > 1) && (autoBig)) {
      displayClear(5);
      display.set1X();

      switch (bigMode) {
        case 1:
          display.setFont(bigNumb);
          tmp_0 = m365_info.curh / 10;
          tmp_1 = m365_info.curh % 10;
          display.setCursor(2, 0);
          if (tmp_0 > 0)
            display.print(tmp_0);
            else
            display.print((char)0x3B);
          display.setCursor(32, 0);
          display.print(tmp_1);
          tmp_0 = m365_info.curl / 10;
          tmp_1 = m365_info.curl % 10;
          display.setCursor(75, 0);
          display.print(tmp_0);
          display.setCursor(108, 0);
          display.setFont(stdNumb);
          display.print(tmp_1);
          display.setFont(defaultFont);
          if ((S25C31.current >= 0) || ((S25C31.current < 0) && (millis() % 1000 < 500))) {
            display.set2X();
            display.setCursor(108, 4);
            display.print((const __FlashStringHelper *) l_a);
          }
          display.set1X();
          display.setCursor(64, 5);
          display.print((char)0x85);
          break;
        default:
          display.setFont(bigNumb);
          tmp_0 = m365_info.sph / 10;
          tmp_1 = m365_info.sph % 10;
          display.setCursor(2, 0);
          if (tmp_0 > 0)
            display.print(tmp_0);
            else
            display.print((char)0x3B);
          display.setCursor(32, 0);
          display.print(tmp_1);
          display.setCursor(75, 0);
          display.print(m365_info.spl);
          display.setCursor(106, 0);
          display.print((char)0x3A);
          display.setFont(defaultFont);
          display.set1X();
          display.setCursor(64, 5);
          display.print((char)0x85);
      }
      showBatt(S25C31.remainPercent, S25C31.current < 0);
    } else {
      if ((S25C31.current < -100) && (c_speed <= 200)) {
        fsBattInfo();
      } else {
        displayClear(0);

        m365_info.milh = S23CB0.mileageCurrent / 100;   //mileage
        m365_info.mill = S23CB0.mileageCurrent % 100;
        m365_info.Min = S23C3A.ridingTime / 60;         //riding time
        m365_info.Sec = S23C3A.ridingTime % 60;
        m365_info.temp = S23CB0.mainframeTemp / 10;     //temperature
	#ifdef US_Version
          m365_info.milh = m365_info.milh/1.609;
          m365_info.mill = m365_info.mill/1.609;
          m365_info.temp = m365_info.temp*9/5+32;
  #endif
        display.set1X();
        display.setFont(stdNumb);
        display.setCursor(0, 0);

        if (m365_info.sph < 10) display.print(' ');
        display.print(m365_info.sph);
        display.print('.');
        display.print(m365_info.spl);
        display.setFont(defaultFont);
        display.print((const __FlashStringHelper *) l_kmh);
        display.setFont(stdNumb);

        display.setCursor(95, 0);

        if (m365_info.temp < 10) display.print(' ');
        display.print(m365_info.temp);
        display.setFont(defaultFont);
        display.print((char)0x80);
        display.print((const __FlashStringHelper *) l_c);
        display.setFont(stdNumb);

        display.setCursor(0, 2);

        if (m365_info.milh < 10) display.print(' ');
        display.print(m365_info.milh);
        display.print('.');
        if (m365_info.mill < 10) display.print('0');
        display.print(m365_info.mill);
        display.setFont(defaultFont);
        display.print((const __FlashStringHelper *) l_km);
        display.setFont(stdNumb);

        display.setCursor(0, 4);

        if (m365_info.Min < 10) display.print('0');
        display.print(m365_info.Min);
        display.print(':');
        if (m365_info.Sec < 10) display.print('0');
        display.print(m365_info.Sec);

        display.setCursor(68, 4);

        if (!showPower) {
          if (m365_info.curh < 10) display.print(' ');
          display.print(m365_info.curh);
          display.print('.');
          if (m365_info.curl < 10) display.print('0');
          display.print(m365_info.curl);
          display.setFont(defaultFont);
          display.print((const __FlashStringHelper *) l_a);
        } else {
          if (m365_info.pwh < 10) display.print(' ');
          display.print(m365_info.pwh);
          display.print('.');
          if (m365_info.pwl < 10) display.print('0');
          display.print(m365_info.pwl);
          display.setFont(defaultFont);
          display.print((const __FlashStringHelper *) l_w);
        }
      }

      showBatt(S25C31.remainPercent, S25C31.current < 0);
    }
}

// ============================================================================
// DATA COMMUNICATION STATE MACHINE
// ============================================================================

/**
 * Finite State Machine for processing incoming data from M365 scooter
 * Handles packet reception, validation, and parsing
 */
void dataFSM() {
  static uint8_t   step = 0, _step = 0, entry = 1;
  static uint32_t   beginMillis;
  static uint8_t   Buf[RECV_BUFLEN];     // Receive buffer
  static uint8_t* _bufPtr;
  _bufPtr = (uint8_t*)&Buf;

  switch (step) {
    // ============================================================================
    // STATE 0: SEARCH FOR PACKET HEADER (0x55 0xAA)
    // ============================================================================
    case 0:
      // Look for the standard M365 packet header sequence
      while (XIAOMI_PORT.available() >= 2)
        if (XIAOMI_PORT.read() == 0x55 && XIAOMI_PORT.peek() == 0xAA) {
          XIAOMI_PORT.read();            // Discard second header byte
          step = 1;                      // Move to data reception state
          break;
        }
      break;
      
    // ============================================================================
    // STATE 1: RECEIVE PACKET BODY AND VALIDATE
    // ============================================================================
    case 1:
      static uint8_t   readCounter;
      static uint16_t    _cs;            // Checksum
      static uint8_t* bufPtr;
      static uint8_t* asPtr;             // Answer structure pointer
      uint8_t bt;
      
      if (entry) {
        // Initialize variables for new packet
        memset((void*)&AnswerHeader, 0, sizeof(AnswerHeader));
        bufPtr = _bufPtr;
        readCounter = 0;
        beginMillis = millis();
        asPtr = (uint8_t*)&AnswerHeader; // Pointer to header structure
        _cs = 0xFFFF;                    // Initialize checksum
      }
      
      // Check for buffer overflow
      if (readCounter >= RECV_BUFLEN) {
        step = 2;                        // End reception
        break;
      }
      
      // Check for timeout
      if (millis() - beginMillis >= RECV_TIMEOUT) {
        step = 2;                        // End reception
        break;
      }

      // Read available bytes from serial buffer
      while (XIAOMI_PORT.available()) {
        bt = XIAOMI_PORT.read();
        readCounter++;
        
        // Store header bytes separately
        if (readCounter <= sizeof(AnswerHeader)) {
          *asPtr++ = bt;
          _cs -= bt;                     // Update checksum
        }
        
        // Store data bytes
        if (readCounter > sizeof(AnswerHeader)) {
          *bufPtr++ = bt;
          if(readCounter < (AnswerHeader.len + 3)) _cs -= bt;
        }
        
        beginMillis = millis();          // Reset timeout
      }

      // Check if complete packet received
      if (AnswerHeader.len == (readCounter - 4)) {
        uint16_t cs;
        uint16_t* ipcs;
        ipcs = (uint16_t*)(bufPtr-2);
        cs = *ipcs;
        
        // Validate checksum
        if(cs != _cs) {
          step = 2;                      // Invalid checksum, discard packet
          break;
        }
        
        // Packet is valid - process it
        processPacket(_bufPtr, readCounter);
        step = 2;
        break;
      }
      break;
      
    // ============================================================================
    // STATE 2: END RECEPTION, RESET FOR NEXT PACKET
    // ============================================================================
    case 2:
      step = 0;                          // Return to header search
      break;
  }

  // Track state changes for entry detection
  if (_step != step) {
    _step = step;
    entry = 1;
  } else entry = 0;
}

// ============================================================================
// PACKET PROCESSING AND DATA EXTRACTION
// ============================================================================

/**
 * Process received packet and extract data into appropriate structures
 * @param data - Pointer to packet data
 * @param len - Length of packet data
 */
void processPacket(uint8_t* data, uint8_t len) {
  uint8_t RawDataLen;
  RawDataLen = len - sizeof(AnswerHeader) - 2;  // Subtract header and CRC

  // Route packet data based on address and command
  switch (AnswerHeader.addr) {
    // ============================================================================
    // ADDRESS 0x20: CONTROLLER DATA
    // ============================================================================
    case 0x20:
      switch (AnswerHeader.cmd) {
        case 0x00:
          switch (AnswerHeader.hz) {
            case 0x64: // BLE request from controller
              break;
            case 0x65: // Throttle and brake data
              // Send next query if hibernation is not active
              if (_Query.prepared == 1 && !_Hibernate) writeQuery();
              // Store throttle/brake data
              memcpy((void*)& S20C00HZ65, (void*)data, RawDataLen);
              break;
            default:
              break;
          }
          break;
        // Other commands (unused in current implementation)
        case 0x1A: case 0x69: case 0x3E: case 0xB0: case 0x23: case 0x3A: case 0x7C:
        default:
          break;
      }
      break;
      
    // ============================================================================
    // ADDRESS 0x21: DASHBOARD/BLE DATA
    // ============================================================================
    case 0x21:
      switch (AnswerHeader.cmd) {
        case 0x00:
        switch(AnswerHeader.hz) {
          case 0x64: // Dashboard status data
            memcpy((void*)& S21C00HZ64, (void*)data, RawDataLen);
            break;
          }
          break;
      default:
        break;
      }
      break;
      
    // ============================================================================
    // ADDRESS 0x22: MOTOR CONTROLLER DATA (unused)
    // ============================================================================
    case 0x22:
      // Various motor controller commands (not implemented)
      break;
      
    // ============================================================================
    // ADDRESS 0x23: SCOOTER SYSTEM DATA
    // ============================================================================
    case 0x23:
      switch (AnswerHeader.cmd) {
        case 0x3E: // Mainframe temperature
          if (RawDataLen == sizeof(A23C3E)) 
            memcpy((void*)& S23C3E, (void*)data, RawDataLen);
          break;
        case 0xB0: // Speed, mileage, and system data
          if (RawDataLen == sizeof(A23CB0)) 
            memcpy((void*)& S23CB0, (void*)data, RawDataLen);
          break;
        case 0x23: // Remaining mileage estimate
          if (RawDataLen == sizeof(A23C23)) 
            memcpy((void*)& S23C23, (void*)data, RawDataLen);
          break;
        case 0x3A: // Power on time and riding time
          if (RawDataLen == sizeof(A23C3A)) 
            memcpy((void*)& S23C3A, (void*)data, RawDataLen);
          break;
        // Other commands (unused)
        case 0x17: case 0x1A: case 0x69: case 0x7C: case 0x7B: case 0x7D:
        default:
          break;
      }
      break;
      
    // ============================================================================
    // ADDRESS 0x25: BATTERY MANAGEMENT SYSTEM DATA
    // ============================================================================        
    case 0x25:
      switch (AnswerHeader.cmd) {
        case 0x40: // Individual cell voltages
          if(RawDataLen == sizeof(A25C40)) 
            memcpy((void*)& S25C40, (void*)data, RawDataLen);
          break;
        case 0x31: // Battery capacity, percentage, current, voltage
          if (RawDataLen == sizeof(A25C31)) 
            memcpy((void*)& S25C31, (void*)data, RawDataLen);
          break;
        // Other commands (unused)
        case 0x3B: case 0x20: case 0x1B: case 0x10:
        default:
          break;
        }
        break;
      default:
        break;
  }

  // Set flag to update display if this was a command we're actively querying
  for (uint8_t i = 0; i < sizeof(_commandsWeWillSend); i++)
    if (AnswerHeader.cmd == pgm_read_byte_near(&_q[_commandsWeWillSend[i]])) {
      _NewDataFlag = 1;                  // Trigger display update
      break;
    }
}

// ============================================================================
// QUERY PREPARATION AND MANAGEMENT
// ============================================================================

/**
 * Prepare the next data query to send to the M365 scooter
 * Cycles through different data requests to keep information current
 */
void prepareNextQuery() {
  static uint8_t index = 0;

  // Define which queries to send cyclically
  _Query._dynQueries[0] = 1;             // Battery data (0x31)
  _Query._dynQueries[1] = 8;             // Speed/mileage data (0xB0)
  _Query._dynQueries[2] = 10;            // Time data (0x3A)
  _Query._dynQueries[3] = 14;            // Cell data (0x40)
  _Query._dynSize = 4;

  // Prepare query from lookup table
  if (preloadQueryFromTable(_Query._dynQueries[index]) == 0) _Query.prepared = 1;

  // Move to next query for next cycle
  index++;
  if (index >= _Query._dynSize) index = 0;
}

/**
 * Build a query packet from the command lookup tables
 * @param index - Index into the command tables
 * @return 0 if successful, error code otherwise
 */
uint8_t preloadQueryFromTable(unsigned char index) {
  uint8_t* ptrBuf;
  uint8_t* pp;                           // Pointer to preamble
  uint8_t* ph;                           // Pointer to header
  uint8_t* pe;                           // Pointer to ending

  uint8_t cmdFormat;
  uint8_t hLen;                          // Header length
  uint8_t eLen;                          // Ending length

  // Validate index
  if (index >= sizeof(_q)) return 1;     // Unknown command index

  // Check if previous query was sent
  if (_Query.prepared != 0) return 2;    // Previous query not sent yet

  // Get packet format from lookup table
  cmdFormat = pgm_read_byte_near(_f + index);

  // Initialize pointers
  pp = (uint8_t*)&_h0;                   // Standard preamble
  ph = NULL;
  pe = NULL;

  // Select header and ending based on format
  switch(cmdFormat) {
    case 1: // Simple header only
      ph = (uint8_t*)&_h1;
      hLen = sizeof(_h1);
      pe = NULL;
      break;
    case 2: // Header with throttle/brake data ending
      ph = (uint8_t*)&_h2;
      hLen = sizeof(_h2);

      // Include current throttle & brake values in query
      _end20t.hz = 0x02;
      _end20t.th = S20C00HZ65.throttle;
      _end20t.br = S20C00HZ65.brake;
      pe = (uint8_t*)&_end20t;
      eLen = sizeof(_end20t);
      break;
  }

  // Build packet in buffer
  ptrBuf = (uint8_t*)&_Query.buf;

  // Copy preamble (0x55 0xAA)
  memcpy_P((void*)ptrBuf, (void*)pp, sizeof(_h0));
  ptrBuf += sizeof(_h0);

  // Copy header
  memcpy_P((void*)ptrBuf, (void*)ph, hLen);
  ptrBuf += hLen;
  
  // Copy command byte
  memcpy_P((void*)ptrBuf, (void*)(_q + index), 1);
  ptrBuf++;
  
  // Copy expected response length
  memcpy_P((void*)ptrBuf, (void*)(_l + index), 1);
  ptrBuf++;

  // Copy ending if needed
  if (pe != NULL) {
    memcpy((void*)ptrBuf, (void*)pe, eLen);
    ptrBuf+= eLen;                       // Fixed: was using hLen instead of eLen
  }

  // Calculate packet length and checksum
  _Query.DataLen = ptrBuf - (uint8_t*)&_Query.buf[2]; // Length without preamble
  _Query.cs = calcCs((uint8_t*)&_Query.buf[2], _Query.DataLen);

  return 0;                              // Success
}

// ============================================================================
// COMMAND PREPARATION AND TRANSMISSION
// ============================================================================

/**
 * Prepare a control command to send to the M365 scooter
 * @param cmd - Command type from enum (CMD_CRUISE_ON, CMD_LED_ON, etc.)
 */
void prepareCommand(uint8_t cmd) {
  uint8_t* ptrBuf;

  // Set standard command header
  _cmd.len  = 4;                         // Command length
  _cmd.addr = 0x20;                      // Target address (controller)
  _cmd.rlen = 0x03;                      // Expected response length

  // Set parameter and value based on command type
  switch(cmd){
    case CMD_CRUISE_ON:                  // Enable cruise control
      _cmd.param = 0x7C;
      _cmd.value = 1;
      break;
    case CMD_CRUISE_OFF:                 // Disable cruise control
      _cmd.param = 0x7C;
      _cmd.value = 0;
      break;
    case CMD_LED_ON:                     // Turn on rear light
      _cmd.param = 0x7D;  
      _cmd.value = 2;
      break;
    case CMD_LED_OFF:                    // Turn off rear light
      _cmd.param = 0x7D;
      _cmd.value = 0;
      break;
    case CMD_WEAK:                       // Weak regenerative braking
      _cmd.param = 0x7B;
      _cmd.value = 0;
      break;
    case CMD_MEDIUM:                     // Medium regenerative braking
      _cmd.param = 0x7B;
      _cmd.value = 1;
      break;
    case CMD_STRONG:                     // Strong regenerative braking
      _cmd.param = 0x7B;
      _cmd.value = 2;
      break;
    default:
      return;                            // Unknown command - do nothing
      break;
  }
  
  // Build command packet
  ptrBuf = (uint8_t*)&_Query.buf;

  // Copy preamble
  memcpy_P((void*)ptrBuf, (void*)_h0, sizeof(_h0));
  ptrBuf += sizeof(_h0);

  // Copy command structure
  memcpy((void*)ptrBuf, (void*)&_cmd, sizeof(_cmd));
  ptrBuf += sizeof(_cmd);

  // Calculate length and checksum
  _Query.DataLen = ptrBuf - (uint8_t*)&_Query.buf[2];
  _Query.cs = calcCs((uint8_t*)&_Query.buf[2], _Query.DataLen);

  _Query.prepared = 1;                   // Mark as ready to send
}

/**
 * Send prepared query or command to the M365 scooter
 * Temporarily disables reception during transmission
 */
void writeQuery() {
  RX_DISABLE;                            // Disable UART receive during transmission
  XIAOMI_PORT.write((uint8_t*)&_Query.buf, _Query.DataLen + 2); // Send data + preamble
  XIAOMI_PORT.write((uint8_t*)&_Query.cs, 2);                   // Send checksum
  RX_ENABLE;                             // Re-enable UART receive
  _Query.prepared = 0;                   // Mark as sent
}

/**
 * Calculate checksum for packet data
 * @param data - Pointer to data bytes
 * @param len - Number of data bytes
 * @return Calculated checksum
 */
uint16_t calcCs(uint8_t* data, uint8_t len) {
  uint16_t cs = 0xFFFF;                  // Initialize checksum
  for (uint8_t i = len; i > 0; i--) cs -= *data++; // Subtract each byte
  return cs;
}

// ============================================================================
// SIMULATOR: SYNTHETIC DATA FEEDER (Wokwi/dev runs)
// ============================================================================
#ifdef SIM_MODE
static void simInit() {
  // Reasonable startup values
  memset(&S21C00HZ64, 0, sizeof(S21C00HZ64));
  S21C00HZ64.state = 1;      // drive
  S21C00HZ64.ledBatt = 6;

  memset(&S20C00HZ65, 0, sizeof(S20C00HZ65));

  memset(&S25C31, 0, sizeof(S25C31));
  S25C31.voltage = 4150;     // 41.50 V
  S25C31.current = 0;        // 0.00 A
  S25C31.remainPercent = 78; // 78%
  S25C31.remainCapacity = 6000; // mAh
  S25C31.temp1 = 45;         // 25 C
  S25C31.temp2 = 46;         // 26 C

  memset(&S23CB0, 0, sizeof(S23CB0));
  S23CB0.speed = 0;          // 0.000 km/h
  S23CB0.mileageTotal = 123450; // 123.450 km
  S23CB0.mileageCurrent = 0; // 0.00 km
  S23CB0.mainframeTemp = 240; // 24.0 C

  memset(&S23C3A, 0, sizeof(S23C3A));
  S23C3A.powerOnTime = 0;
  S23C3A.ridingTime = 0;

  // Seed some per-cell voltages ~4.15V
  memset(&S25C40, 0, sizeof(S25C40));
  int16_t* c = (int16_t*)&S25C40;
  for (uint8_t i = 0; i < 10; i++) c[i] = 4150 + (i % 3);

  _NewDataFlag = 1;
}

static void simTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if ((int32_t)(now - last) < 100) return; // 10Hz updates
  last = now;

  // Triangle wave 0..25.0 km/h over ~10s
  uint16_t phase = (now / 50) % 200; // 0..199
  uint16_t up = (phase <= 100) ? phase : (200 - phase); // 0..100
  int16_t speed_mmpkh = (int16_t)(up * 250); // 0..25000 (1/1000 km/h units)
  bool braking = ((now / 5000) % 2) == 1; // alternate every 5s

  // Throttle/brake
  if (braking) {
    S20C00HZ65.throttle = 30;  // min
    S20C00HZ65.brake = 180;    // strong braking
  } else {
    S20C00HZ65.brake = 40;     // released
    S20C00HZ65.throttle = (uint8_t)(50 + (up * 2)); // ~50..250
  }

  // Current: draw under accel, regen under braking
  if (braking) {
    S25C31.current = (int16_t)(-700 - up * 8);   // -7.00 .. -15.00 A
  } else {
    S25C31.current = (int16_t)(up * 12);         // 0 .. 12.00 A
  }

  // Speed and temps
  S23CB0.speed = speed_mmpkh;
  if (S23CB0.mainframeTemp < 320) S23CB0.mainframeTemp++; // warm slowly

  // Time and mileage
  S23C3A.powerOnTime++;
  S23C3A.ridingTime++;
  // Trip mileage: increment roughly with speed (km/100 units)
  S23CB0.mileageCurrent += (uint16_t)(up / 10); // coarse approx

  // Battery: tiny sag on accel, small recovery on brake
  int16_t v = S25C31.voltage;
  int16_t sag = braking ? -5 : (up > 0 ? -2 : 0);
  v += sag;
  if ((now % 4000) < 100) v--; // slow discharge trend
  if (v < 3600) v = 3600;
  if (v > 4200) v = 4200;
  S25C31.voltage = v;

  // Keep per-cell voltages around pack/10
  int16_t* c = (int16_t*)&S25C40;
  int16_t target = v / 10;
  for (uint8_t i = 0; i < 10; i++) {
    int16_t jitter = (i * 3) % 7; // small variation
    c[i] = target + jitter;
  }

  // SOC estimation from voltage for demo purposes
  uint8_t soc = (uint8_t)constrain((v - 3600) / 6, 0, 100);
  S25C31.remainPercent = soc;
  S25C31.remainCapacity = (uint16_t)(6500UL * soc / 100);

  // Mark new data so UI refreshes
  _NewDataFlag = 1;
  // Ensure query state machine never blocks display in sim
  _Query.prepared = 0;
}
#endif
