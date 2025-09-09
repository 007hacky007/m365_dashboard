#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Minimal public API for IDF port
void m365_port_init(void);
void m365_port_start_tasks(void);

#ifdef __cplusplus
}
#endif
