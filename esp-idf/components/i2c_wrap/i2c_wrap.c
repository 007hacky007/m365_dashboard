#include "i2c_wrap.h"
#include "esp_log.h"

esp_err_t i2c_wrap_init_legacy(i2c_port_t port,int sda,int scl,uint32_t hz){
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg,&bus_handle));
    // Attach a generic device (SSD1306) at 0x3C to validate bus; callers will create their own device objects if needed.
    // Just return success; higher-level code retains legacy API for now.
    return ESP_OK;
}
