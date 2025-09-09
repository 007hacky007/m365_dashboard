#include <stdio.h>
// Ultra minimal mode OFF to enable OLED functionality
#define ULTRA_MIN_MODE 0
// Fallback disabled to use full u8g2 rendering path
// #define SIMPLE_DISPLAY_FALLBACK 1
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h" // kept temporarily for types
#include "i2c_v2.h" // retained for future sensors; not used for OLED now
#include "driver/uart.h"
#include "esp_timer.h"
#include "protocol_state.h"
#include "esp_log.h"
#include "ssd1306.h" // legacy minimal driver
#include "u8g2_integration.h" // external u8g2 component
#include <m365_port.h>
#include "display_fsm_port.h"
#include "range_estimator.h"
#include "aht10.h"
#include "settings.h"

static const char *TAG = "m365_idf_min";

// I2C pins (match Arduino build which used Wire.begin(8, 9))
#define TEST_I2C_PORT I2C_NUM_0
#define TEST_I2C_SDA 8
#define TEST_I2C_SCL 9


static void init_i2c(void) {
    // For display we now rely on u8g2 HAL which sets up I2C itself (and deletes any prior driver).
    // Keep this disabled unless other peripherals require high-level API.
    ESP_LOGI(TAG, "Skipping i2c_v2 init for OLED (handled by u8g2 HAL)");
    // ESP_ERROR_CHECK(i2c_v2_init(TEST_I2C_SDA, TEST_I2C_SCL, 400000)); // enable if needed for other devices
}

// probe_oled removed (new v2 API would require device handle; unnecessary for now)

void app_main(void) {
    ESP_LOGI(TAG, "Starting app main");

    // UART initialized inside comms_port_init() (avoid double install warning)
    init_i2c();

    // Optional subsystems (disabled in ultra minimal mode)
#if !ULTRA_MIN_MODE && !SIMPLE_DISPLAY_FALLBACK
    if (u8g2_int_init()) {
        u8g2_int_boot_screen();
    }
#endif

    m365_port_init();
    m365_port_start_tasks();
    range_init();
    settings_init_defaults();
#if CFG_AHT10_ENABLE
    // Initialize AHT10 (ambient sensor). Requires i2c_v2 if not using HAL bus; here we rely on HAL I2C lines already set up.
    aht10_init();
#endif
#if !ULTRA_MIN_MODE
    display_port_start();
#endif

    // Heartbeat + heap monitor task (in-line simple loop here)
    uint32_t lastLog = 0; unsigned hb=0; 
    while (1) { 
        vTaskDelay(pdMS_TO_TICKS(100)); 
        uint32_t now = (uint32_t) (esp_timer_get_time()/1000ULL); 
        if (now - lastLog > 1000) { 
            lastLog = now; 
            ESP_LOGI(TAG, "HB %u free_heap=%u lastAddr=%02X cmd=%02X len=%u", ++hb, (unsigned)esp_get_free_heap_size(), g_lastDiagHeader.addr, g_lastDiagHeader.cmd, g_lastDiagHeader.len); 
        } 
    }
}
