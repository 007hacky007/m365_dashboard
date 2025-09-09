#pragma once
#include <stdint.h>
#include <unistd.h>
#include "esp_timer.h"

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
static inline void delay(uint32_t ms) {
    usleep(ms * 1000U);
}
