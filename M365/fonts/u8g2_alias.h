// Aliases for legacy font identifiers used in code, mapped to U8g2 via adapter
#pragma once

// We use sentinel pointer values to represent font choices.
// The adapter interprets these values and selects a concrete U8g2 font.
static const uint8_t defaultFont[] = {0x01};
static const uint8_t stdNumb[]     = {0x02};
static const uint8_t bigNumb[]     = {0x03};
static const uint8_t segNumb[]     = {0x04};
// Original code used a custom icon font 'm365' for logo and battery glyphs.
// With U8g2 we draw shapes directly; this alias is retained for compatibility.
static const uint8_t m365[]        = {0x05};
