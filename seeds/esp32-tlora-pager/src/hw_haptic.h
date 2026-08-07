// DRV2605 haptic (I2C 0x5A). Rails raised via XL9555 EXP_DRV_EN at boot.
// Register sequence from Adafruit_DRV2605 + LilyGo initDrv().

#pragma once

#include <Arduino.h>

// Probe + configure. Safe if chip missing (returns false).
bool hw_haptic_begin();

bool hw_haptic_ok();

// Fire a ROM library effect and return immediately (GO bit, non-blocking).
// level: 0=info, 1=warn, 2=crit (matches NOTIFY_*).
void hw_haptic_notify(uint8_t level);

// Raw effect id 1..123 from datasheet table, 0 = stop.
void hw_haptic_effect(uint8_t effect_id);
