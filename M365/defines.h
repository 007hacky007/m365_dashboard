// Rewritten clean header
#ifndef M365_DEFINES_H
#define M365_DEFINES_H

#include <Arduino.h>
#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#else
  #include <pgmspace.h>
#endif

// Watchdog abstraction
#if defined(ARDUINO_ARCH_AVR)
  #include "WatchDog.h"
#else
  namespace WatchDog { inline void init(void (*)(void), uint32_t) {} }
#endif

// Display config: choose one
//#define DISPLAY_SPI
#define DISPLAY_I2C

#include "SSD1306Ascii.h"
#ifdef DISPLAY_SPI
  #include <SPI.h>
  #include "SSD1306AsciiSpi.h"
  #define PIN_CS  10
  #define PIN_RST 9
  #define PIN_DC  8
  #define PIN_D0 13
  #define PIN_D1 11
#endif
#ifdef DISPLAY_I2C
  #include <Wire.h>
  #include "SSD1306AsciiWire.h"
  // Default SSD1306 I2C address
  #ifndef OLED_I2C_ADDRESS
  #define OLED_I2C_ADDRESS 0x3C
  #endif
#endif

// Fonts
#include "fonts/m365.h"
#include "fonts/System5x7mod.h"
#include "fonts/stdNumb.h"
#include "fonts/bigNumb.h"

// System
#include <EEPROM.h>
#include "language.h"
#include "messages.h"

#ifdef M365_DEFINE_GLOBALS
  MessagesClass Message;
#else
  extern MessagesClass Message;
#endif

// Shared prototypes
void oledInit(bool showLogo = true);
void oledService();
bool i2cCheckAndRecover();
bool displayClear(byte ID = 1, bool force = false);

// Config/state
#define LONG_PRESS 2000

#ifdef M365_DEFINE_GLOBALS
  uint8_t warnBatteryPercent = 5;
  bool autoBig = true;
  uint8_t bigMode = 1;
  bool bigWarn = true;
  bool hibernateOnBoot = false;
  bool showPower = true;
  bool showVoltageMain = true;
#else
  extern uint8_t warnBatteryPercent; extern bool autoBig; extern uint8_t bigMode;
  extern bool bigWarn; extern bool hibernateOnBoot; extern bool showPower; extern bool showVoltageMain;
#endif

#if defined(ARDUINO_ARCH_ESP32)
  #ifdef M365_DEFINE_GLOBALS
    bool wifiEnabled = false;
  #else
    extern bool wifiEnabled;
  #endif
#else
  static const bool wifiEnabled = false;
#endif

#ifdef M365_DEFINE_GLOBALS
  bool Settings = false, ShowBattInfo = false, M365Settings = false, WiFiSettings = false;
  uint8_t menuPos = 0, sMenuPos = 0, wifiMenuPos = 0;
#else
  extern bool Settings, ShowBattInfo, M365Settings, WiFiSettings;
  extern uint8_t menuPos, sMenuPos, wifiMenuPos;
#endif

#ifdef M365_DEFINE_GLOBALS
  bool cfgCruise = false, cfgTailight = true; uint8_t cfgKERS = 0;
#else
  extern bool cfgCruise, cfgTailight; extern uint8_t cfgKERS;
#endif

#ifdef M365_DEFINE_GLOBALS
  volatile int16_t oldBrakeVal = -1, oldThrottleVal = -1; volatile bool btnPressed = false;
  bool bAlarm = false; uint32_t timer = 0; volatile bool oledBusy = false;
#else
  extern volatile int16_t oldBrakeVal, oldThrottleVal; extern volatile bool btnPressed; extern bool bAlarm; extern uint32_t timer; extern volatile bool oledBusy;
#endif

#ifdef DISPLAY_SPI
  #ifdef M365_DEFINE_GLOBALS
    SSD1306AsciiSpi display;
  #else
    extern SSD1306AsciiSpi display;
  #endif
#endif
#ifdef DISPLAY_I2C
  #ifdef M365_DEFINE_GLOBALS
    SSD1306AsciiWire display;
  #else
    extern SSD1306AsciiWire display;
  #endif
#endif

#ifdef M365_DEFINE_GLOBALS
  bool WheelSize = true; uint8_t WDTcounts = 0; void(* resetFunc) (void) = 0;
#else
  extern bool WheelSize; extern uint8_t WDTcounts; extern void(* resetFunc) (void);
#endif

// Comm defs
#if defined(ARDUINO_ARCH_ESP32)
  #include <HardwareSerial.h>
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Update.h>
  #define XIAOMI_PORT Serial1
  #ifndef M365_UART_RX_PIN
    #define M365_UART_RX_PIN 16
  #endif
  #ifndef M365_UART_TX_PIN
    #define M365_UART_TX_PIN 17
  #endif
  #define SERIAL_BEGIN(baud) XIAOMI_PORT.begin((baud), SERIAL_8N1, M365_UART_RX_PIN, M365_UART_TX_PIN)
  // Expose OTA globals from main sketch
  extern WebServer otaServer;
  extern bool otaRunning;
  extern String otaSSID;
  extern String otaPASS;
  void otaBegin();
  void otaEnd();
  void otaService();
#else
  #define XIAOMI_PORT Serial
  #define SERIAL_BEGIN(baud) XIAOMI_PORT.begin((baud))
#endif

#if defined(UCSR0B) && defined(RXEN0)
  #define RX_DISABLE UCSR0B &= ~_BV(RXEN0);
  #define RX_ENABLE  UCSR0B |=  _BV(RXEN0);
#else
  #define RX_DISABLE
  #define RX_ENABLE
#endif

#if defined(ARDUINO_ARCH_ESP32)
  #define EEPROM_START(sz) EEPROM.begin((sz))
  #define EEPROM_COMMIT()  EEPROM.commit()
#else
  #define EEPROM_START(sz) ((void)0)
  #define EEPROM_COMMIT()  ((void)0)
#endif

struct QUERY_t { uint8_t prepared, DataLen; uint8_t buf[16]; uint16_t cs; uint8_t _dynQueries[5]; uint8_t _dynSize; };
#ifdef M365_DEFINE_GLOBALS
  QUERY_t _Query = {0};
#else
  extern QUERY_t _Query;
#endif

#ifdef M365_DEFINE_GLOBALS
  volatile uint8_t _NewDataFlag = 0; volatile bool _Hibernate = false;
#else
  extern volatile uint8_t _NewDataFlag; extern volatile bool _Hibernate;
#endif

enum {CMD_CRUISE_ON, CMD_CRUISE_OFF, CMD_LED_ON, CMD_LED_OFF, CMD_WEAK, CMD_MEDIUM, CMD_STRONG};
struct __attribute__((packed)) CMD{ uint8_t len, addr, rlen, param; int16_t value; };
#ifdef M365_DEFINE_GLOBALS
  CMD _cmd = {0};
#else
  extern CMD _cmd;
#endif

#ifdef M365_DEFINE_GLOBALS
  extern const uint8_t _commandsWeWillSend[3] = {1, 8, 10};
#else
  extern const uint8_t _commandsWeWillSend[3];
#endif

#ifdef M365_DEFINE_GLOBALS
  extern const uint8_t _q[15] PROGMEM = {0x3B, 0x31, 0x20, 0x1B, 0x10, 0x1A, 0x69, 0x3E, 0xB0, 0x23, 0x3A, 0x7B, 0x7C, 0x7D, 0x40};
  extern const uint8_t _l[15] PROGMEM = {   2,   10,    6,    4,   18,   12,    2,    2,   32,    6,    4,    2,    2,    2,   30};
  extern const uint8_t _f[15] PROGMEM = {   1,    1,    1,    1,    1,    2,    2,    2,    2,    2,    2,    2,    2,    2,    1};
#else
  extern const uint8_t _q[15] PROGMEM; extern const uint8_t _l[15] PROGMEM; extern const uint8_t _f[15] PROGMEM;
#endif

#ifdef M365_DEFINE_GLOBALS
  extern const uint8_t _h0[2] PROGMEM = {0x55, 0xAA};
  extern const uint8_t _h1[3] PROGMEM = {0x03, 0x22, 0x01};
  extern const uint8_t _h2[3] PROGMEM = {0x06, 0x20, 0x61};
  extern const uint8_t _hc[3] PROGMEM = {0x04, 0x20, 0x03};
#else
  extern const uint8_t _h0[2] PROGMEM; extern const uint8_t _h1[3] PROGMEM; extern const uint8_t _h2[3] PROGMEM; extern const uint8_t _hc[3] PROGMEM;
#endif

struct __attribute__ ((packed)) END20T_t{ uint8_t hz, th, br; };
#ifdef M365_DEFINE_GLOBALS
  END20T_t _end20t = {0,0,0};
#else
  extern END20T_t _end20t;
#endif

#ifndef RECV_TIMEOUT
#define RECV_TIMEOUT  5
#endif
#ifndef RECV_BUFLEN
#define RECV_BUFLEN   64
#endif

struct __attribute__((packed)) ANSWER_HEADER{ uint8_t len, addr, hz, cmd; };
#ifdef M365_DEFINE_GLOBALS
  ANSWER_HEADER AnswerHeader = {0,0,0,0};
#else
  extern ANSWER_HEADER AnswerHeader;
#endif

struct __attribute__ ((packed)) S21C00HZ64_t { uint8_t state, ledBatt, headLamp, beepAction; };
#ifdef M365_DEFINE_GLOBALS
  S21C00HZ64_t S21C00HZ64 = {0,0,0,0};
#else
  extern S21C00HZ64_t S21C00HZ64;
#endif

struct __attribute__((packed)) A20C00HZ65 { uint8_t hz1, throttle, brake, hz2, hz3; };
#ifdef M365_DEFINE_GLOBALS
  A20C00HZ65 S20C00HZ65 = {0,0,0,0,0};
#else
  extern A20C00HZ65 S20C00HZ65;
#endif

struct __attribute__((packed)) A25C31 { uint16_t remainCapacity; uint8_t remainPercent, u4; int16_t current, voltage; uint8_t temp1, temp2; };
#ifdef M365_DEFINE_GLOBALS
  A25C31 S25C31 = {0};
#else
  extern A25C31 S25C31;
#endif

struct __attribute__((packed)) A25C40 { int16_t c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15; };
#ifdef M365_DEFINE_GLOBALS
  A25C40 S25C40 = {0};
#else
  extern A25C40 S25C40;
#endif

struct __attribute__((packed)) A23C3E { int16_t i1; };
#ifdef M365_DEFINE_GLOBALS
  A23C3E S23C3E = {0};
#else
  extern A23C3E S23C3E;
#endif

struct __attribute__((packed)) A23CB0 { uint8_t u1[10]; int16_t speed; uint16_t averageSpeed; uint32_t mileageTotal; uint16_t mileageCurrent; uint16_t elapsedPowerOnTime; int16_t mainframeTemp; uint8_t u2[8]; };
#ifdef M365_DEFINE_GLOBALS
  A23CB0 S23CB0 = {{0},0,0,0,0,0,0,{0}};
#else
  extern A23CB0 S23CB0;
#endif

struct __attribute__((packed)) A23C23 { uint8_t u1,u2,u3,u4; uint16_t remainMileage; };
#ifdef M365_DEFINE_GLOBALS
  A23C23 S23C23 = {0,0,0,0,0};
#else
  extern A23C23 S23C23;
#endif

struct __attribute__((packed)) A23C3A { uint16_t powerOnTime, ridingTime; };
#ifdef M365_DEFINE_GLOBALS
  A23C3A S23C3A = {0,0};
#else
  extern A23C3A S23C3A;
#endif

#endif // M365_DEFINES_H
