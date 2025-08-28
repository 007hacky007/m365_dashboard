#pragma once
#include "defines.h"

#ifdef SIM_MODE
void simInit();
void simTick();
// Optional control helpers for SIM
void simSetManual(bool on);
void simSetThrottle(uint8_t v);
void simSetBrake(uint8_t v);
#endif
