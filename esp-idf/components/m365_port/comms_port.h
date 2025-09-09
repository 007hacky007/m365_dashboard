#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bus activity timestamps (ms since boot) updated on each valid frame
extern volatile uint32_t g_lastBusDataMs;
extern volatile uint32_t g_firstBusDataMs;
extern volatile bool g_busEverSeen;

// Init + task start
void comms_port_init(void);
void start_comms_task(void);

#ifdef __cplusplus
}
#endif
