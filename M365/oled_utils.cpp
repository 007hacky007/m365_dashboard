#include "oled_utils.h"

#ifdef DISPLAY_I2C
static bool s_i2cFast = false;     // false: 100kHz, true: 400kHz
static uint8_t s_consecOK = 0;     // consecutive OK pings
#endif

bool i2cCheckAndRecover() {
#ifdef DISPLAY_I2C
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  uint8_t err = Wire.endTransmission();
  if (err == 0) return true;

  #if defined(SDA) && defined(SCL)
    pinMode(SDA, INPUT_PULLUP);
    pinMode(SCL, INPUT_PULLUP);

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

  #if defined(WIRE_HAS_END)
  Wire.end();
  #endif
  delay(1);
  Wire.begin();
  Wire.setClock(100000L);
  #ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(25000, true);
  #endif
  delay(1);

  Wire.beginTransmission(OLED_I2C_ADDRESS);
  err = Wire.endTransmission();
  s_i2cFast = false;
  s_consecOK = 0;
  return err == 0;
#else
  return true;
#endif
}

void oledInit(bool showLogo) {
#ifdef DISPLAY_I2C
  #if defined(WIRE_HAS_END)
    Wire.end();
  #endif
  Wire.begin();
  Wire.setClock(100000L);
  #ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(25000, true);
  #endif
  display.begin(&Adafruit128x64, OLED_I2C_ADDRESS);
  s_i2cFast = false;
  s_consecOK = 0;
#endif
#ifdef DISPLAY_SPI
  display.begin(&Adafruit128x64, PIN_CS, PIN_DC, PIN_RST);
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

void oledService() {
#ifdef DISPLAY_I2C
  static uint32_t nextCheck = 0;
  uint32_t now = millis();
  if ((int32_t)(now - nextCheck) >= 0) {
    nextCheck = now + 500;
    if (oledBusy) return;
    if (i2cCheckAndRecover()) {
      if (!s_i2cFast) {
        if (s_consecOK < 255) s_consecOK++;
        if (s_consecOK >= 8) {
          Wire.setClock(400000L);
          s_i2cFast = true;
        }
      }
    } else {
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
