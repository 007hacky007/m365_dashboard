#pragma once
#include "defines.h"

// Attempt to detect and recover a stuck I2C bus, then verify OLED presence
bool i2cCheckAndRecover();

// Initialize the OLED display and I2C/SPI bus
void oledInit(bool showLogo);

// Lightweight periodic health check; re-inits OLED upon hiccup
void oledService();
