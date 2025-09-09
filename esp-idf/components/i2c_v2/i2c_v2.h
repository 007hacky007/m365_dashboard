#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_v2_init(int sda,int scl,uint32_t hz);
i2c_master_dev_handle_t i2c_v2_get_dev(uint8_t addr);

#ifdef __cplusplus
}
#endif
