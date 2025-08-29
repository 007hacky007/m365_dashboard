#pragma once
#include "defines.h"

// Initialize estimator (load from EEPROM ring)
void rangeInit();

// Call frequently (e.g., each loop) to update learning logic
void rangeTick();

// Optional explicit checkpoint (e.g., on orderly shutdown/hibernate)
void rangeCheckpointIfNeeded();

// Accessors
float rangeGetKmPerPct();
float rangeGetEstimateKm();
