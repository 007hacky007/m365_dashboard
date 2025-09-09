#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void range_init(void);      // load persisted model (stubbed currently)
void range_tick(void);      // call periodically (each display frame)
void range_checkpoint_if_needed(void); // forced persist (stub)
float range_get_km_per_pct(void);
float range_get_estimate_km(void);     // remaining km estimate

#ifdef __cplusplus
}
#endif
