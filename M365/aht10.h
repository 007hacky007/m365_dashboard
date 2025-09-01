#pragma once
#include "defines.h"

#if defined(ARDUINO_ARCH_ESP32) && CFG_AHT10_ENABLE
// Minimal AHT10 driver (I2C). Returns true on success.
bool aht10Init();
// Perform a measurement; outputs temperature in Celsius (float) and humidity percentage (float).
// Returns true if fresh data was read, false otherwise.
bool aht10Read(float &tempC, float &rh);
// Cached last values (updated by aht10Read). Useful for UI without heap.
extern float g_ahtTempC;
extern float g_ahtHum;
extern bool  g_ahtPresent;
#endif
