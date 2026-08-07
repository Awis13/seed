// ES8311 codec + NS4150B amp — short pager beeps on notify.
// Pins from lilygo_tlora_pager pins_arduino.h; amp rail = XL9555 EXP_AMP_EN.

#pragma once

#include <Arduino.h>

bool hw_sound_begin();
bool hw_sound_ok();

// Queue the same beep pattern tembed uses for info/warn/crit (0/1/2).
// Non-blocking; call hw_sound_poll() from loop().
void hw_sound_notify(uint8_t level);

// Pump I2S. Cheap when idle.
void hw_sound_poll();
