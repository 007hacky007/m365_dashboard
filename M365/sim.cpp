#include "sim.h"

#ifdef SIM_MODE
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
}

void simTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if ((int32_t)(now - last) < 100) return;
  last = now;

  uint16_t phase = (now / 50) % 200;
  uint16_t up = (phase <= 100) ? phase : (200 - phase);
  int16_t speed_mmpkh = (int16_t)(up * 250);
  bool braking = ((now / 5000) % 2) == 1;

  uint32_t cycle = now % 20000UL;
  bool dwell = (cycle < 3000UL) || (cycle >= 10000UL && cycle < 13000UL);

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

  S23CB0.speed = speed_mmpkh;
  if (S23CB0.mainframeTemp < 320) S23CB0.mainframeTemp++;

  S23C3A.powerOnTime++;
  S23C3A.ridingTime++;
  if (!dwell) {
    S23CB0.mileageCurrent += (uint16_t)(up / 10);
  }

  int16_t v = S25C31.voltage;
  int16_t sag = dwell ? 0 : (braking ? -5 : (up > 0 ? -2 : 0));
  v += sag;
  if ((now % 4000) < 100) v--;
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
