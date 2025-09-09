#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_wrap_init_legacy(i2c_port_t port,int sda,int scl,uint32_t hz);

#ifdef __cplusplus
}
#endif
