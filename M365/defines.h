#include <Arduino.h>
#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#else
  #include <pgmspace.h>
#endif

// Watchdog: real library on AVR, lightweight stub elsewhere
#if defined(ARDUINO_ARCH_AVR)
  #include "WatchDog.h"
#else
  namespace WatchDog {
    inline void init(void (*)(void), uint32_t) { /* no-op on non-AVR */ }
  }
#endif

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================

// Select either SPI or I2C(Wire) Display Mode
// Only one should be enabled at a time
//#define DISPLAY_SPI    // Use SPI communication for OLED
#define DISPLAY_I2C     // Use I2C communication for OLED (default)

// OLED I2C address (common values: 0x3C or 0x3D)
#define OLED_I2C_ADDRESS 0x3C

// Uncomment to enable US units (mph, Fahrenheit) instead of metric (km/h, Celsius)
//#define US_Version

// ============================================================================
// DISPLAY LIBRARY INCLUDES AND PIN DEFINITIONS
// ============================================================================

#include "SSD1306Ascii.h"
#ifdef DISPLAY_SPI
  #include <SPI.h>
  #include "SSD1306AsciiSpi.h"
  // SPI pin definitions for OLED display
  #define PIN_CS  10    // Chip Select
  #define PIN_RST 9     // Reset
  #define PIN_DC  8     // Data/Command
  #define PIN_D0 13     // Clock (SCK)
  #define PIN_D1 11     // Data (MOSI)
#endif
#ifdef DISPLAY_I2C
  #include <Wire.h>
  #include "SSD1306AsciiWire.h"  // I2C version of SSD1306 library
#endif

// ============================================================================
// FONT INCLUDES
// ============================================================================

#include "fonts/m365.h"        // Custom M365 icons and symbols
#include "fonts/System5x7mod.h"  // Modified system font
#include "fonts/stdNumb.h"     // Standard numbers font
#include "fonts/bigNumb.h"     // Large numbers font for speedometer

// ============================================================================
// SYSTEM INCLUDES AND MESSAGING
// ============================================================================

#include <EEPROM.h>            // For storing settings persistently
#include "language.h"          // Multi-language support
#include "messages.h"          // Communication protocol handling

MessagesClass Message;         // Global message handler instance

// ============================================================================
// CONFIGURATION VARIABLES AND DEFAULTS
// ============================================================================

// Enable to run without a real scooter/bus. Feeds synthetic frames so the UI works in simulators.
// You can define this at compile time (e.g., in Wokwi or via build flags).
// #define SIM_MODE

const uint16_t LONG_PRESS = 2000;    // Duration for long press detection (ms)

// Display and warning settings (stored in EEPROM)
uint8_t warnBatteryPercent = 5;       // Battery warning threshold (5%, 10%, 15%, or 0=off)
bool autoBig = true;                  // Auto-enable big speedometer when moving
uint8_t bigMode = 1;                  // Big display mode: 0=speed, 1=current
bool bigWarn = true;                  // Show full-screen battery warning
bool hibernateOnBoot = false;         // One-time hibernation trigger on next boot
bool showPower = true;                // Show power (W) instead of current (A)
// ESP32-only: WiFi AP + OTA toggle (persisted). Ignored on AVR builds.
#if defined(ARDUINO_ARCH_ESP32)
bool wifiEnabled = false;             // Start WiFi AP and OTA server
#else
const bool wifiEnabled = false;       // Not supported on AVR
#endif

// Menu state variables
bool Settings = false;                // True when in settings menu
bool ShowBattInfo = false;            // True when showing battery details
bool M365Settings = false;            // True when in M365 scooter settings
bool WiFiSettings = false;            // ESP32: WiFi/OTA submenu active

uint8_t menuPos = 0;                  // Current position in main settings menu
uint8_t sMenuPos = 0;                 // Current position in M365 settings menu
uint8_t wifiMenuPos = 0;              // ESP32: WiFi submenu position

// M365 scooter configuration (stored in EEPROM)
bool cfgCruise = false;                // Cruise control enabled
bool cfgTailight = true;             // Rear light configuration
uint8_t cfgKERS = 0;                  // Regenerative braking: 0=weak, 1=medium, 2=strong

// Input handling variables
volatile int16_t oldBrakeVal = -1;    // Previous brake lever state
volatile int16_t oldThrottleVal = -1; // Previous throttle state
volatile bool btnPressed = false;     // Button press flag (unused)
bool bAlarm = false;                  // Alarm state (unused)

uint32_t timer = 0;                   // General purpose timer for UI timeouts 

// Forward declarations for I2C/OLED recovery helpers
void oledInit(bool showLogo = true);
void oledService();
bool i2cCheckAndRecover();

// Simple guard to avoid OLED re-init in the middle of drawing
volatile bool oledBusy = false;

// ============================================================================
// DISPLAY OBJECT INITIALIZATION
// ============================================================================

#ifdef DISPLAY_SPI
SSD1306AsciiSpi display;             // SPI display object
#endif
#ifdef DISPLAY_I2C
SSD1306AsciiWire display;            // I2C display object
#endif

// ============================================================================
// SCOOTER CONFIGURATION
// ============================================================================

bool WheelSize = true;              // false = 8.5" wheels, true = 10" wheels

// ============================================================================
// WATCHDOG AND RESET FUNCTIONALITY
// ============================================================================

uint8_t WDTcounts = 0;               // Watchdog counter to prevent system freeze
void(* resetFunc) (void) = 0;        // Function pointer for software reset

// ============================================================================
// COMMUNICATION PROTOCOL DEFINITIONS
// ============================================================================

// UART selection for M365 BUS
#if defined(ARDUINO_ARCH_ESP32)
  #include <HardwareSerial.h>
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Update.h>
  #ifndef M365_UART_NUM
    #define M365_UART_NUM 1
  #endif
  #ifndef M365_UART_RX_PIN
    #define M365_UART_RX_PIN 16
  #endif
  #ifndef M365_UART_TX_PIN
    #define M365_UART_TX_PIN 17
  #endif
  #define XIAOMI_PORT Serial1        // Use UART1 by default on ESP32
  #define SERIAL_BEGIN(baud) XIAOMI_PORT.begin((baud), SERIAL_8N1, M365_UART_RX_PIN, M365_UART_TX_PIN)
#else
  #define XIAOMI_PORT Serial           // Use hardware serial for M365 communication
  #define SERIAL_BEGIN(baud) XIAOMI_PORT.begin((baud))
#endif
// Safely guard AVR-specific UART RX enable/disable for portability with newer cores
#if defined(UCSR0B) && defined(RXEN0)
  #define RX_DISABLE UCSR0B &= ~_BV(RXEN0);
  #define RX_ENABLE  UCSR0B |=  _BV(RXEN0);
#else
  // Non-AVR or cores without UCSR0B: no-op to keep build working
  #define RX_DISABLE
  #define RX_ENABLE
#endif

// EEPROM helpers: required on ESP32 to start/commit emulated EEPROM
#if defined(ARDUINO_ARCH_ESP32)
  #define EEPROM_START(sz) EEPROM.begin((sz))
  #define EEPROM_COMMIT()  EEPROM.commit()
#else
  #define EEPROM_START(sz) ((void)0)
  #define EEPROM_COMMIT()  ((void)0)
#endif

// Query structure for sending commands to M365
struct {
  uint8_t prepared;                  // 1 if query is ready to send, 0 after sending
  uint8_t DataLen;                   // Length of data to send
  uint8_t buf[16];                   // Buffer containing the query
  uint16_t cs;                       // Checksum of the data
  uint8_t _dynQueries[5];            // Array of dynamic query indices
  uint8_t _dynSize = 0;              // Number of dynamic queries
} _Query;

// Communication flags
volatile uint8_t _NewDataFlag = 0;   // Set to 1 when new data received, triggers display update
volatile bool _Hibernate = false;   // Hibernation mode: disables M365 queries for firmware updates
                                     // Activated by holding throttle+brake during startup logo
                                     // Allows firmware flashing without physical disconnection

// ============================================================================
// COMMAND DEFINITIONS AND STRUCTURES
// ============================================================================

// Command enumeration for M365 scooter control
enum {CMD_CRUISE_ON, CMD_CRUISE_OFF, CMD_LED_ON, CMD_LED_OFF, CMD_WEAK, CMD_MEDIUM, CMD_STRONG};

// Structure for sending control commands to M365
struct __attribute__((packed)) CMD{
  uint8_t  len;                      // Length of command
  uint8_t  addr;                     // Target address
  uint8_t  rlen;                     // Response length expected
  uint8_t  param;                    // Parameter to modify
  int16_t  value;                    // Value to set
}_cmd;

// Array of command indices that will be sent cyclically for data requests
const uint8_t _commandsWeWillSend[] = {1, 8, 10}; // Indexes: 1=battery, 8=speed/mileage, 10=times

// Command lookup tables (stored in program memory to save RAM)
        // INDEX                     0     1     2     3     4     5     6     7     8     9    10    11    12    13    14
const uint8_t _q[] PROGMEM = {0x3B, 0x31, 0x20, 0x1B, 0x10, 0x1A, 0x69, 0x3E, 0xB0, 0x23, 0x3A, 0x7B, 0x7C, 0x7D, 0x40}; //commands
const uint8_t _l[] PROGMEM = {   2,   10,    6,    4,   18,   12,    2,    2,   32,    6,    4,    2,    2,    2,   30}; //expected answer length
const uint8_t _f[] PROGMEM = {   1,    1,    1,    1,    1,    2,    2,    2,    2,    2,    2,    2,    2,    2,    1}; //packet format type

// ============================================================================
// PROTOCOL PACKET HEADERS AND STRUCTURES
// ============================================================================

// Protocol packet headers (stored in program memory)
const uint8_t _h0[]    PROGMEM = {0x55, 0xAA};     // Standard packet preamble
const uint8_t _h1[]    PROGMEM = {0x03, 0x22, 0x01}; // Header type 1
const uint8_t _h2[]    PROGMEM = {0x06, 0x20, 0x61}; // Header type 2
const uint8_t _hc[]    PROGMEM = {0x04, 0x20, 0x03}; // Control command header

// Dynamic packet ending structure for queries that include throttle/brake data
struct __attribute__ ((packed)){ 
  uint8_t hz;                        // Frequency/mode byte
  uint8_t th;                        // Current throttle value
  uint8_t br;                        // Current brake value
}_end20t;

// ============================================================================
// COMMUNICATION PARAMETERS
// ============================================================================

const uint8_t RECV_TIMEOUT =  5;     // Receive timeout in milliseconds
const uint8_t RECV_BUFLEN  = 64;     // Maximum receive buffer length

// ============================================================================
// DATA STRUCTURES FOR M365 RESPONSES
// ============================================================================

// Header structure for incoming packets
struct __attribute__((packed)) ANSWER_HEADER{
  uint8_t len;                       // Packet length
  uint8_t addr;                      // Source address
  uint8_t hz;                        // Frequency/mode
  uint8_t cmd;                       // Command ID
} AnswerHeader;

// ============================================================================
// M365 SCOOTER DATA STRUCTURES
// ============================================================================

// BLE/Dashboard status data (Address 0x21, Command 0x00, Hz 0x64)
struct __attribute__ ((packed)) {
  uint8_t state;                     // 0=stall, 1=drive, 2=eco stall, 3=eco drive
  uint8_t ledBatt;                   // Battery LED status: 0=min, 7/8=max
  uint8_t headLamp;                  // Headlight: 0=off, 0x64=on
  uint8_t beepAction;                // Beep/alarm actions
} S21C00HZ64;

// Throttle and brake data (Address 0x20, Command 0x00, Hz 0x65)
struct __attribute__((packed))A20C00HZ65 {
  uint8_t hz1;                       // Unknown frequency byte
  uint8_t throttle;                  // Throttle position (0-255)
  uint8_t brake;                     // Brake lever position (0-255)
  uint8_t hz2;                       // Unknown frequency byte
  uint8_t hz3;                       // Unknown frequency byte
} S20C00HZ65;

// Battery data (Address 0x25, Command 0x31)
struct __attribute__((packed))A25C31 {
  uint16_t remainCapacity;           // Remaining capacity in mAh
  uint8_t  remainPercent;            // Battery charge percentage
  uint8_t  u4;                       // Battery status (unknown purpose)
  int16_t  current;                  // Current in units/100 = Amperes
  int16_t  voltage;                  // Voltage in units/100 = Volts
  uint8_t  temp1;                    // Temperature 1 (subtract 20 for °C)
  uint8_t  temp2;                    // Temperature 2 (subtract 20 for °C)
} S25C31;

// Individual battery cells data (Address 0x25, Command 0x40)
struct __attribute__((packed))A25C40 {
  int16_t c1;  // Cell 1 voltage /1000 = Volts
  int16_t c2;  // Cell 2 voltage
  int16_t c3;  // Cell 3 voltage
  int16_t c4;  // Cell 4 voltage
  int16_t c5;  // Cell 5 voltage
  int16_t c6;  // Cell 6 voltage
  int16_t c7;  // Cell 7 voltage
  int16_t c8;  // Cell 8 voltage
  int16_t c9;  // Cell 9 voltage
  int16_t c10; // Cell 10 voltage
  int16_t c11; // Cell 11 voltage
  int16_t c12; // Cell 12 voltage
  int16_t c13; // Cell 13 voltage
  int16_t c14; // Cell 14 voltage
  int16_t c15; // Cell 15 voltage
} S25C40;

// Mainframe temperature (Address 0x23, Command 0x3E)
struct __attribute__((packed))A23C3E {
  int16_t i1;                        // Mainframe temperature
} S23C3E;

// Speed, mileage and system data (Address 0x23, Command 0xB0)
struct __attribute__((packed))A23CB0 {
  uint8_t u1[10];                    // Unknown data (32 bytes total)
  int16_t  speed;                    // Current speed /1000 = km/h
  uint16_t averageSpeed;             // Average speed /1000 = km/h
  uint32_t mileageTotal;             // Total odometer /1000 = km
  uint16_t mileageCurrent;           // Trip distance /100 = km
  uint16_t elapsedPowerOnTime;       // Time since power on (seconds)
  int16_t  mainframeTemp;            // Mainframe temperature /10 = °C
  uint8_t u2[8];                     // Unknown data
} S23CB0;

// Remaining mileage estimate (Address 0x23, Command 0x23)
struct __attribute__((packed))A23C23 {
  uint8_t u1;                        // Unknown
  uint8_t u2;                        // Unknown
  uint8_t u3;                        // Usually 0x30
  uint8_t u4;                        // Usually 0x09
  uint16_t remainMileage;            // Estimated remaining range /100 = km
} S23C23;

// Time data (Address 0x23, Command 0x3A)
struct __attribute__((packed))A23C3A {
  uint16_t powerOnTime;              // Total power on time
  uint16_t ridingTime;               // Current ride time in seconds
} S23C3A;
