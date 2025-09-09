// u8g2_integration.c - wrapper around external u8g2-hal-esp-idf component
// Integration using external u8g2-hal-esp-idf component
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "esp_log.h"
#include "driver/i2c.h"

static const char *TAG = "u8g2_int";
static u8g2_t u8g2; // global instance

#define PIN_SDA 8
#define PIN_SCL 9
// hal will configure I2C, we just provide pins

u8g2_t *u8g2_int(void){ return &u8g2; }

bool u8g2_int_init(void){
    // If I2C driver is already installed (from earlier custom init) uninstall it to avoid conflicts
    // The HAL installs its own driver instance on I2C_MASTER_NUM.
    i2c_driver_delete(I2C_NUM_0); // ignore error if not installed
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = PIN_SDA;
    hal.bus.i2c.scl = PIN_SCL;
    u8g2_esp32_hal_init(hal);
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
    u8g2_SetI2CAddress(&u8g2, 0x3C<<1);
    ESP_LOGD(TAG, "Calling u8g2_InitDisplay...");
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x12_tr);
    u8g2_DrawStr(&u8g2, 0,12, "U8G2 ONLINE");
    u8g2_SendBuffer(&u8g2);
    ESP_LOGD(TAG, "u8g2 init ok and test pattern sent");
    return true;
}

void u8g2_int_boot_screen(void){
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x12_tr);
    u8g2_DrawStr(&u8g2, 0,12, "M365 DASH");
    //u8g2_DrawStr(&u8g2, 0,26, "Initializing...");
    u8g2_SendBuffer(&u8g2);
}
