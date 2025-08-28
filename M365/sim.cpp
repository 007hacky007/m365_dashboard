#include "sim.h"

#ifdef SIM_MODE
static bool simManual = false;
static uint8_t simThrottle = 30;
static uint8_t simBrake = 40;
static uint16_t filtThrottle = 0;
static uint16_t filtBrake = 0;
static uint16_t filtSpeed = 0;
static bool speedPotZero = false;

void simSetManual(bool on) { simManual = on; }
void simSetThrottle(uint8_t v) { simThrottle = v; }
void simSetBrake(uint8_t v) { simBrake = v; }
void simInit() {
  memset(&S21C00HZ64, 0, sizeof(S21C00HZ64));
  S21C00HZ64.state = 1;
  S21C00HZ64.ledBatt = 6;

  memset(&S20C00HZ65, 0, sizeof(S20C00HZ65));

  memset(&S25C31, 0, sizeof(S25C31));
  S25C31.voltage = 4150;
  S25C31.current = 0;
  S25C31.remainPercent = 78;
  S25C31.remainCapacity = 6000;
  S25C31.temp1 = 45;
  S25C31.temp2 = 46;

  memset(&S23CB0, 0, sizeof(S23CB0));
  S23CB0.speed = 0;
  S23CB0.mileageTotal = 1234;
  S23CB0.mileageCurrent = 0;
  S23CB0.mainframeTemp = 240;

  memset(&S23C3A, 0, sizeof(S23C3A));
  S23C3A.powerOnTime = 0;
  S23C3A.ridingTime = 0;

  memset(&S25C40, 0, sizeof(S25C40));
  int16_t* c = (int16_t*)&S25C40;
  for (uint8_t i = 0; i < 10; i++) c[i] = 4150 + (i % 3);

  _NewDataFlag = 1;
  // Initialize filters
  filtThrottle = 0; filtBrake = 0; filtSpeed = 0;

  // Configure stationary switch (Wokwi)
  #ifdef SIM_MODE
  #if defined(ARDUINO_ARCH_ESP32)
    pinMode(SIM_STATIONARY_PIN, INPUT_PULLUP);
  #else
    pinMode(SIM_STATIONARY_PIN, INPUT_PULLUP);
    // Use default AVcc reference for stable ADC reads in Wokwi
    analogReference(DEFAULT);
  #endif
  #endif
}

void simTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if ((int32_t)(now - last) < 100) return;
  last = now;

  // Tiny serial command parser for SIM control:
  //  Tnnn -> set throttle (0-200), Bnnn -> set brake (0-200)
  //  M0/1 -> manual off/on,  A -> auto (manual off), R -> reset anim state
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'T') {
      int v = Serial.parseInt(); if (v < 0) v = 0; if (v > 255) v = 255; simThrottle = (uint8_t)v; simManual = true;
    } else if (c == 'B') {
      int v = Serial.parseInt(); if (v < 0) v = 0; if (v > 255) v = 255; simBrake = (uint8_t)v; simManual = true;
    } else if (c == 'M') {
      int v = Serial.parseInt(); simManual = (v != 0);
    } else if (c == 'A') {
      simManual = false;
    } else if (c == 'R') {
      // no persistent state yet; could extend to reset timers in future
    }
  }

  uint16_t phase = (now / 50) % 200;
  uint16_t up = (phase <= 100) ? phase : (200 - phase);
  int16_t speed_mmpkh = (int16_t)(up * 250);
  bool braking = ((now / 5000) % 2) == 1;

  uint32_t cycle = now % 20000UL;
  bool dwell = (cycle < 3000UL) || (cycle >= 10000UL && cycle < 13000UL);

  // Read stationary switch (active low). When ON, force stationary regardless of inputs
  bool stationary = false;
  #ifdef SIM_MODE
    stationary = (digitalRead(SIM_STATIONARY_PIN) == LOW);
    gSimStationary = stationary;
  #endif

  // Optional: analog pot control (Wokwi) maps to manual throttle/brake and speed override
#ifdef SIM_MODE
  {
#if defined(ARDUINO_ARCH_ESP32)
    // Configure attenuation for full 0..3.3V range on ADC1
    analogSetPinAttenuation(SIM_THROTTLE_PIN, ADC_11db);
    analogSetPinAttenuation(SIM_BRAKE_PIN,    ADC_11db);
    analogSetPinAttenuation(SIM_SPEED_PIN,    ADC_11db);
#endif
    int at = analogRead(SIM_THROTTLE_PIN);
    int ab = analogRead(SIM_BRAKE_PIN);
    int as = analogRead(SIM_SPEED_PIN);
  // Simple IIR smoothing to reduce jitter
  filtThrottle = (filtThrottle * 3 + at) / 4;
  filtBrake    = (filtBrake * 3 + ab) / 4;
  filtSpeed    = (filtSpeed * 3 + as) / 4;
    at = filtThrottle; ab = filtBrake; as = filtSpeed;
  if (at >= 0 && ab >= 0 && as >= 0) {
      // Map 0..4095 (ESP32) or 0..1023 (AVR) to 0..200 range used by UI thresholds
      int maxv = 4095;
      #if defined(ARDUINO_ARCH_AVR)
        maxv = 1023;
      #endif
      simThrottle = (uint8_t)((long)at * 200 / maxv);
      simBrake    = (uint8_t)((long)ab * 200 / maxv);
      // Speed override pot mapped to 0..500 (same scale as target below)
  int speedOverride = (int)((long)as * 500 / maxv);
  speedPotZero = (speedOverride == 0);
      // If either pot is moved, switch to manual mode automatically
      // If either pot is moved, switch to manual mode automatically
      if (simThrottle > 5 || simBrake > 5) simManual = true;
      // Apply speed override regardless of auto/manual when the speed pot is non-zero
  if (!stationary && (speedOverride > 0 || (simThrottle > 50 && simBrake > 50))) {
        dwell = false;
        // Move speed towards override smoothly to avoid jumps
        int d = speedOverride - speed_mmpkh;
        speed_mmpkh += (int16_t)(d / 2);
      }
    }
  }
#endif

  if (simManual && !stationary && !speedPotZero) {
    // Manual mode: accept direct throttle/brake; compute a plausible speed/current
    dwell = false;
    S20C00HZ65.throttle = simThrottle;
    S20C00HZ65.brake = simBrake;
    // Simple model: target speed proportional to throttle, reduced by brake
    int t = simThrottle; int b = simBrake;
    int target = (t * 5) - (b * 3); if (target < 0) target = 0; if (target > 500) target = 500;
    // Smooth towards target
    int delta = target - speed_mmpkh;
    speed_mmpkh += (int16_t)(delta / 3);
    // Current: positive with throttle, negative with brake
    int cur = (t * 18) - (b * 10); if (cur < -1500) cur = -1500; if (cur > 2000) cur = 2000;
    S25C31.current = (int16_t)cur;
  } else {
    if (dwell) {
      speed_mmpkh = 0;
      S20C00HZ65.throttle = 30;
      S20C00HZ65.brake = 40;
      S25C31.current = 0;
      braking = false;
    } else {
  if (braking) {
        S20C00HZ65.throttle = 30;
        S20C00HZ65.brake = 180;
      } else {
        S20C00HZ65.brake = 40;
        S20C00HZ65.throttle = (uint8_t)(50 + (up * 2));
      }

      if (braking) {
        S25C31.current = (int16_t)(-700 - up * 8);
      } else {
        S25C31.current = (int16_t)(up * 20);
      }
    }
  }

  // Force zero speed/mileage when stationary switch is ON or speed pot is at zero
  if (stationary || speedPotZero) {
    speed_mmpkh = 0;
    S25C31.current = 0;
  // Preserve user inputs so UI can detect simultaneous brake+throttle
  S20C00HZ65.throttle = simThrottle;
  S20C00HZ65.brake = simBrake;
  }

  S23CB0.speed = speed_mmpkh;
  if (S23CB0.mainframeTemp < 320) S23CB0.mainframeTemp++;

  S23C3A.powerOnTime++;
  S23C3A.ridingTime++;
  if (!dwell && !(stationary || speedPotZero)) {
    S23CB0.mileageCurrent += (uint16_t)(up / 10);
  }

  int16_t v = S25C31.voltage;
  // Hold voltage stable when stationary or speed pot requests zero speed
  bool holdVoltage = stationary || speedPotZero;
  int16_t sag = holdVoltage ? 0 : (dwell ? 0 : (braking ? -5 : (up > 0 ? -2 : 0)));
  v += sag;
  if (!holdVoltage && (now % 4000) < 100) v--;
  if (v < 3600) v = 3600;
  if (v > 4200) v = 4200;
  S25C31.voltage = v;

  int16_t* c2 = (int16_t*)&S25C40;
  int16_t target = v / 10;
  for (uint8_t i = 0; i < 10; i++) {
    int16_t jitter = (i * 3) % 7;
    c2[i] = target + jitter;
  }

  uint8_t soc = (uint8_t)constrain((v - 3600) / 6, 0, 100);
  S25C31.remainPercent = soc;
  S25C31.remainCapacity = (uint16_t)(6500UL * soc / 100);

  _NewDataFlag = 1;
  _Query.prepared = 0;
}
#endif
