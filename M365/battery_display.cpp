#include "battery_display.h"
#include "range_estimator.h"

void showBatt(int percent, bool blinkIt) {
  display.set1X();
  display.setFont(defaultFont);
  display.setCursor(0, 7);

  if (bigWarn || (warnBatteryPercent == 0) || (percent > warnBatteryPercent) || ((warnBatteryPercent != 0) && (millis() % 1000 < 500))) {
    display.print((char)0x81);
    for (int i = 0; i < 19; i++) {
      display.setCursor(5 + i * 5, 7);
      if (blinkIt && (millis() % 1000 < 500))
        display.print((char)0x83);
      else if (float(19) / 100 * percent > i)
        display.print((char)0x82);
      else
        display.print((char)0x83);
    }
    display.setCursor(99, 7);
    display.print((char)0x84);
    if (percent < 100) display.print(' ');
    if (percent < 10) display.print(' ');
    display.print(percent);
    display.print('%');
  } else {
    for (int i = 0; i < 34; i++) {
      display.setCursor(i * 5, 7);
      display.print(' ');
    }
  }
}

void showRangeSmall() {
  // Print remaining km as small label on last row, right-aligned
  float km = rangeGetEstimateKm();
  if (km < 0) km = 0;
  // Format with one decimal, cap to 999.9 for layout
  if (km > 999.9f) km = 999.9f;
  uint16_t i = (uint16_t)km;
  uint16_t f = (uint16_t)((km - (float)i) * 10.0f + 0.5f);
  display.set1X(); display.setFont(defaultFont);
  // Reserve roughly 8 chars: "123.4km"
  const uint8_t colStart = 86; // near right edge but before battery %
  display.setCursor(colStart, 6);
  // Clear area
  for (uint8_t c = 0; c < 12; ++c) display.print(' ');
  display.setCursor(colStart, 6);
  if (i < 100) display.print(' ');
  if (i < 10) display.print(' ');
  display.print(i);
  display.print('.'); display.print(f);
  display.print((const __FlashStringHelper *) l_km);
}

void fsBattInfo() {
  displayClear(6);
  int16_t tmp_0, tmp_1;
  display.setCursor(0, 0);
  display.set1X();

  tmp_0 = abs(S25C31.voltage) / 100;
  tmp_1 = abs(S25C31.voltage) % 100;
  if (tmp_0 < 10) display.print(' ');
  display.print(tmp_0);
  display.print('.');
  if (tmp_1 < 10) display.print('0');
  display.print(tmp_1);
  display.print((const __FlashStringHelper *) l_v);
  display.print(' ');

  if (!showPower) {
    int16_t cur_cA = totalCurrent_cA();
    tmp_0 = abs(cur_cA) / 100;
    tmp_1 = abs(cur_cA) % 100;
    if (tmp_0 < 10) display.print(' ');
    display.print(tmp_0);
    display.print('.');
    if (tmp_1 < 10) display.print('0');
    display.print(tmp_1);
    display.print((const __FlashStringHelper *) l_a);
  } else {
  uint32_t ai = (uint32_t)abs(totalCurrent_cA());
    uint32_t vi = (uint32_t)abs(S25C31.voltage);
    uint32_t m = (ai * vi + 50) / 100; // centi-watts
    tmp_0 = (int16_t)(m / 100);
    tmp_1 = (int16_t)(m % 100);
    if (tmp_0 < 10) display.print(' ');
    display.print(tmp_0);
    display.print('.');
    if (tmp_1 < 10) display.print('0');
    display.print(tmp_1);
    display.print((const __FlashStringHelper *) l_w);
  }
  display.print(' ');

  if (S25C31.remainCapacity < 1000) display.print(' ');
  if (S25C31.remainCapacity < 100) display.print(' ');
  if (S25C31.remainCapacity < 10) display.print(' ');
  display.print(S25C31.remainCapacity);
  display.print((const __FlashStringHelper *) l_mah);

  int temp;
  temp = S25C31.temp1 - 20;
  display.setCursor(9, 1);
  display.print((const __FlashStringHelper *) l_t);
  display.print("1: ");
  if (temp < 10) display.print(' ');
  display.print(temp);
  display.print((char)0x80);
  display.print("C");

  display.setCursor(74, 1);
  display.print((const __FlashStringHelper *) l_t);
  display.print("2: ");
  temp = S25C31.temp2 - 20;
  if (temp < 10) display.print(' ');
  display.print(temp);
  display.print((char)0x80);
  display.print("C");

  int16_t v;
  int16_t * ptr;
  int16_t * ptr2;
  ptr = (int16_t*)&S25C40;
  ptr2 = ptr + 5;

  for (uint8_t i = 0; i < 5; i++) {
    display.setCursor(5, 2 + i);
    display.print(i);
    display.print(": ");
    v = *ptr / 1000;
    display.print(v);
    display.print('.');
    v = *ptr % 1000;
    if (v < 100) display.print('0');
    if (v < 10) display.print('0');
    display.print(v);
    display.print((const __FlashStringHelper *) l_v);

    display.setCursor(70, 2 + i);
    display.print(i + 5);
    display.print(": ");
    v = *ptr2 / 1000;
    display.print(v);
    display.print('.');
    v = *ptr2 % 1000;
    if (v < 100) display.print('0');
    if (v < 10) display.print('0');
    display.print(v);
    display.print((const __FlashStringHelper *) l_v);

    ptr++;
    ptr2++;
  }
}
