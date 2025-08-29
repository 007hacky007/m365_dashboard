#pragma once
#include "defines.h"

// Draw bottom battery bar and percent
void showBatt(int percent, bool blinkIt);

// Draw the detailed battery information page
void fsBattInfo();

// Draw small range estimate text on the right side of the battery bar
void showRangeSmall();
