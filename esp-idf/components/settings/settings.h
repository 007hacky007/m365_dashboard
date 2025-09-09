#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#ifdef __cplusplus
extern "C" { 
#endif

typedef struct {
    uint8_t warnBatteryPercent; // 0 disables
    bool    autoBig;
    bool    showPower;
    bool    showVoltageMain;
    bool    bigWarn;
    uint8_t bigMode; // 0=speed,1=current/power when big screen active
    uint8_t mainTempSource; // 0=DRV,1=T1,2=T2,3=AMB
} ui_settings_t;

void settings_init_defaults(void);
ui_settings_t * settings_ui(void);

#ifdef __cplusplus
}
#endif
