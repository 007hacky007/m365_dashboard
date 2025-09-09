#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "config.h" // for CFG_AHT10_ENABLE and AHT10_I2C_ADDRESS
#ifdef __cplusplus
extern "C" { 
#endif

#if CFG_AHT10_ENABLE
bool aht10_init(void);            // init & soft reset + calibrate
bool aht10_read(float *tempC, float *rh); // trigger measurement and read
extern float g_ahtTempC;
extern float g_ahtHum;
extern bool  g_ahtPresent;
#endif

#ifdef __cplusplus
}
#endif
