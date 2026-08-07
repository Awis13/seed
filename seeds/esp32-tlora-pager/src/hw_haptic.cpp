// Minimal DRV2605 driver — no SensorsLib dependency.
// Refs: Adafruit_DRV2605 init(); LilyGo_LoRa_Pager::initDrv().
// ROM library effect IDs: TI DRV2605 datasheet §11.2 (Library 1).

#include "hw_haptic.h"
#include "board_pins.h"

#include <Wire.h>

#define DRV_ADDR           0x5A
#define DRV_REG_STATUS     0x00
#define DRV_REG_MODE       0x01
#define DRV_REG_RTPIN      0x02
#define DRV_REG_LIBRARY    0x03
#define DRV_REG_WAVESEQ1   0x04
#define DRV_REG_GO         0x0C
#define DRV_REG_RATEDV     0x16
#define DRV_REG_CLAMPV     0x17
#define DRV_REG_FEEDBACK   0x1A
#define DRV_REG_CONTROL3   0x1D

// Stronger library-1 cues (was: 1 / 12 / 16 — too polite for a pocket beeper).
#define FX_STRONG_CLICK  1    // boot self-test only
#define FX_STRONG_BUZZ  14    // Strong Buzz 100%
#define FX_ALERT_750    15    // 750 ms Alert 100%  (LilyGo default)
#define FX_STRONG_BUZZ1 16    // Strong Buzz 1
#define FX_TRIPLE       52    // Triple Click Strong 100%

static bool drv_ok = false;

static bool drv_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(DRV_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool drv_read(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(DRV_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)DRV_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

// Play a short sequence of ROM effects, 0-terminated (max 7 slots + end).
static void drv_play_seq(const uint8_t *fx, int n) {
    if (!drv_ok || !fx || n <= 0) return;
    if (n > 7) n = 7;
    for (int i = 0; i < n; i++)
        drv_write((uint8_t)(DRV_REG_WAVESEQ1 + i), fx[i]);
    drv_write((uint8_t)(DRV_REG_WAVESEQ1 + n), 0);  // end
    drv_write(DRV_REG_GO, 0x01);
}

bool hw_haptic_begin() {
    uint8_t status = 0;
    // Presence: STATUS must ACK. Value itself is device-dependent.
    if (!drv_read(DRV_REG_STATUS, &status)) {
        Serial.println("[haptic] DRV2605 missing");
        drv_ok = false;
        return false;
    }

    // Out of standby, no RTP, library 1, ERM open-loop (LilyGo/Adafruit).
    drv_write(DRV_REG_MODE, 0x00);
    drv_write(DRV_REG_RTPIN, 0x00);
    drv_write(DRV_REG_LIBRARY, 0x01);

    // Push drive a bit harder than chip defaults (still within ERM open-loop).
    // Rated ~2.0 V, OD clamp ~3.0 V — snappier pocket buzz without continuous rumble.
    drv_write(DRV_REG_RATEDV, 0x53);
    drv_write(DRV_REG_CLAMPV, 0xA4);

    uint8_t fb = 0, c3 = 0;
    if (drv_read(DRV_REG_FEEDBACK, &fb))
        drv_write(DRV_REG_FEEDBACK, fb & 0x7F);          // N_ERM_LRA = 0 → ERM
    if (drv_read(DRV_REG_CONTROL3, &c3))
        drv_write(DRV_REG_CONTROL3, (uint8_t)(c3 | 0x20)); // ERM_OPEN_LOOP

    drv_ok = true;
    Serial.printf("[haptic] DRV2605 ok status=0x%02X (boosted)\n", status);

    // Quiet self-test click so a dead motor is obvious at first boot.
    hw_haptic_effect(FX_STRONG_CLICK);
    return true;
}

bool hw_haptic_ok() { return drv_ok; }

void hw_haptic_effect(uint8_t effect_id) {
    if (!drv_ok) return;
    if (effect_id == 0) {
        drv_write(DRV_REG_GO, 0x00);
        return;
    }
    uint8_t seq[1] = { effect_id };
    drv_play_seq(seq, 1);
}

void hw_haptic_notify(uint8_t level) {
    if (!drv_ok) return;
    // info: one strong buzz; warn: alert; crit: triple + alert (can't miss)
    if (level >= 2) {
        const uint8_t seq[] = { FX_TRIPLE, FX_ALERT_750 };
        drv_play_seq(seq, 2);
    } else if (level == 1) {
        const uint8_t seq[] = { FX_STRONG_BUZZ, FX_ALERT_750 };
        drv_play_seq(seq, 2);
    } else {
        const uint8_t seq[] = { FX_STRONG_BUZZ };
        drv_play_seq(seq, 1);
    }
}
