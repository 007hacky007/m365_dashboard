#include "aht10.h"

#if defined(ARDUINO_ARCH_ESP32) && CFG_AHT10_ENABLE

float g_ahtTempC = NAN;
float g_ahtHum = NAN;
bool  g_ahtPresent = false;

static bool aht10WriteCmd(uint8_t c0, uint8_t c1, uint8_t c2) {
  Wire.beginTransmission(AHT10_I2C_ADDRESS);
  Wire.write(c0); Wire.write(c1); Wire.write(c2);
  return Wire.endTransmission() == 0;
}

bool aht10Init() {
#ifdef SIM_MODE
  g_ahtPresent = true; // always present in SIM
  g_ahtTempC = 22.5f;
  g_ahtHum = 45.0f;
  return true;
#else
  // Soft reset
  Wire.beginTransmission(AHT10_I2C_ADDRESS);
  Wire.write(0xBA);
  if (Wire.endTransmission() != 0) {
    g_ahtPresent = false;
    return false;
  }
  delay(20);

  // Init/Calibrate: 0xE1, 0x08, 0x00
  if (!aht10WriteCmd(0xE1, 0x08, 0x00)) {
    g_ahtPresent = false;
    return false;
  }
  delay(10);
  g_ahtPresent = true;
  return true;
#endif
}

bool aht10Read(float &tempC, float &rh) {
  if (!g_ahtPresent) return false;
#ifdef SIM_MODE
  // Generate gentle variations over time
  uint32_t t = millis();
  float baseT = 22.0f + 3.0f * sinf((float)t / 7000.0f);
  float baseH = 40.0f + 10.0f * sinf((float)t / 11000.0f + 1.0f);
  g_ahtTempC = baseT;
  g_ahtHum = baseH;
  tempC = g_ahtTempC;
  rh = g_ahtHum;
  return true;
#else
  // Trigger measurement: 0xAC, 0x33, 0x00
  if (!aht10WriteCmd(0xAC, 0x33, 0x00)) {
    g_ahtPresent = false;
    return false;
  }
  delay(85); // typical 80ms

  Wire.requestFrom(AHT10_I2C_ADDRESS, (uint8_t)6);
  if (Wire.available() < 6) return false;
  uint8_t s0 = Wire.read();
  uint8_t s1 = Wire.read();
  uint8_t s2 = Wire.read();
  uint8_t s3 = Wire.read();
  uint8_t s4 = Wire.read();
  uint8_t s5 = Wire.read();
  // status bit7 of s0: 1=busy
  if (s0 & 0x80) return false;

  uint32_t rawHum = ((uint32_t)(s0 & 0x3F) << 16) | ((uint32_t)s1 << 8) | s2;
  uint32_t rawTemp = ((uint32_t)s3 << 16) | ((uint32_t)s4 << 8) | s5;
  // temp is 20-bit; top 4 bits of s3 are high bits
  rawTemp = rawTemp >> 4;

  rh = (rawHum * 100.0f) / 1048576.0f; // 2^20
  tempC = ((rawTemp * 200.0f) / 1048576.0f) - 50.0f;
  g_ahtTempC = tempC;
  g_ahtHum = rh;
  return true;
#endif
}

#endif
