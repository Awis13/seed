// Minimal DRV2605 driver — no SensorsLib dependency.
// Refs: Adafruit_DRV2605 init(); LilyGo_LoRa_Pager::initDrv().

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
#define DRV_REG_FEEDBACK   0x1A
#define DRV_REG_CONTROL3   0x1D

// ROM library effects (datasheet §11.2) — short, distinct, not endless rumbles.
#define FX_INFO   1    // Strong Click 100%
#define FX_WARN  12    // Sharp Click 100%
#define FX_CRIT  16    // Strong Buzz 1 (attention)

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

    uint8_t fb = 0, c3 = 0;
    if (drv_read(DRV_REG_FEEDBACK, &fb))
        drv_write(DRV_REG_FEEDBACK, fb & 0x7F);          // N_ERM_LRA = 0 → ERM
    if (drv_read(DRV_REG_CONTROL3, &c3))
        drv_write(DRV_REG_CONTROL3, (uint8_t)(c3 | 0x20)); // ERM_OPEN_LOOP

    drv_ok = true;
    Serial.printf("[haptic] DRV2605 ok status=0x%02X\n", status);

    // Quiet self-test click so a dead motor is obvious at first boot.
    hw_haptic_effect(FX_INFO);
    return true;
}

bool hw_haptic_ok() { return drv_ok; }

void hw_haptic_effect(uint8_t effect_id) {
    if (!drv_ok) return;
    if (effect_id == 0) {
        drv_write(DRV_REG_GO, 0x00);
        return;
    }
    drv_write(DRV_REG_WAVESEQ1 + 0, effect_id);
    drv_write(DRV_REG_WAVESEQ1 + 1, 0);  // end
    drv_write(DRV_REG_GO, 0x01);
}

void hw_haptic_notify(uint8_t level) {
    if (!drv_ok) return;
    uint8_t fx = FX_INFO;
    if (level == 2) fx = FX_CRIT;       // NOTIFY_CRIT
    else if (level == 1) fx = FX_WARN;  // NOTIFY_WARN
    hw_haptic_effect(fx);
}
