// Encoder via ESP32 pulse-counter (same approach as tembed ui.h).

#include "hw_input.h"
#include "board_pins.h"

#include <ESP32Encoder.h>

// Full-quad PCNT: 4 edges per mechanical detent on this board. Folding by 2
// (tembed's value) made one click land as two menu steps — measured 2026-08-07.
#define INPUT_COUNTS_PER_DETENT 4
// Measured 2026-08-07 on T-Lora: +1 matches "wheel up = menu up".
#define INPUT_DIRECTION         (1)
// Menu feels one-click-one-step; if a slow SPI frame lets two detents pile up
// in pending_steps, still deliver at most one per poll read.
#define INPUT_MAX_STEPS_PER_READ 1
#define INPUT_DEBOUNCE_MS       30
#define INPUT_CLICK_MAX_MS      800
#define INPUT_LONG_MS          1000

static ESP32Encoder enc;
static int64_t enc_raw = 0;
static int32_t enc_accum = 0;
static int pending_steps = 0;

static bool key_level = false;
static bool key_stable = false;
static unsigned long key_changed = 0;
static unsigned long key_down_at = 0;
static bool pending_click = false;
static bool pending_long = false;
static bool long_fired = false;

void hw_input_begin() {
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    enc.attachFullQuad(PIN_ROTARY_A, PIN_ROTARY_B);
    enc.setFilter(1023);
    enc.clearCount();
    enc_raw = enc.getCount();
    enc_accum = 0;
    pending_steps = 0;

    pinMode(PIN_ROTARY_C, INPUT_PULLUP);  // active low
    key_level = digitalRead(PIN_ROTARY_C) == LOW;
    key_stable = key_level;
    key_changed = millis();
    pending_click = false;
    pending_long = false;
    long_fired = false;

    Serial.printf("[input] encoder A=%d B=%d C=%d\n",
                  PIN_ROTARY_A, PIN_ROTARY_B, PIN_ROTARY_C);
}

void hw_input_poll() {
    int64_t raw = enc.getCount();
    enc_accum += (int32_t)(raw - enc_raw);
    enc_raw = raw;

    int steps = 0;
    while (enc_accum >= INPUT_COUNTS_PER_DETENT) {
        enc_accum -= INPUT_COUNTS_PER_DETENT;
        steps++;
    }
    while (enc_accum <= -INPUT_COUNTS_PER_DETENT) {
        enc_accum += INPUT_COUNTS_PER_DETENT;
        steps--;
    }
    pending_steps += steps * INPUT_DIRECTION;

    bool now = digitalRead(PIN_ROTARY_C) == LOW;
    unsigned long t = millis();
    if (now != key_level) {
        key_level = now;
        key_changed = t;
    }
    if ((t - key_changed) >= INPUT_DEBOUNCE_MS && key_stable != key_level) {
        bool was = key_stable;
        key_stable = key_level;
        if (key_stable && !was) {
            key_down_at = t;
            long_fired = false;
        } else if (!key_stable && was) {
            if (!long_fired && (t - key_down_at) < INPUT_CLICK_MAX_MS) pending_click = true;
        }
    }
    if (key_stable && !long_fired && (t - key_down_at) >= INPUT_LONG_MS) {
        long_fired = true;
        pending_long = true;
        pending_click = false;
    }
}

int hw_input_steps() {
    int s = pending_steps;
    if (s > INPUT_MAX_STEPS_PER_READ) {
        pending_steps = s - INPUT_MAX_STEPS_PER_READ;
        s = INPUT_MAX_STEPS_PER_READ;
    } else if (s < -INPUT_MAX_STEPS_PER_READ) {
        pending_steps = s + INPUT_MAX_STEPS_PER_READ;
        s = -INPUT_MAX_STEPS_PER_READ;
    } else {
        pending_steps = 0;
    }
    return s;
}

bool hw_input_click() {
    if (!pending_click) return false;
    pending_click = false;
    return true;
}

bool hw_input_held() {
    return key_stable;
}

bool hw_input_long_press() {
    if (!pending_long) return false;
    pending_long = false;
    return true;
}
