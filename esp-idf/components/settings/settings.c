#include "settings.h"
static ui_settings_t g_ui;

void settings_init_defaults(void){
    g_ui.warnBatteryPercent = CFG_WARN_BATTERY_PERCENT_DEFAULT;
    g_ui.autoBig            = CFG_AUTOBIG_DEFAULT;
    g_ui.showPower          = CFG_SHOW_POWER_DEFAULT;
    g_ui.showVoltageMain    = CFG_SHOW_VOLTAGE_MAIN_DEFAULT;
    g_ui.bigWarn            = CFG_BIGWARN_DEFAULT;
    g_ui.bigMode            = CFG_BIGMODE_DEFAULT;
    g_ui.mainTempSource     = CFG_MAIN_TEMP_SOURCE_DEFAULT;
}

ui_settings_t * settings_ui(void){ return &g_ui; }
