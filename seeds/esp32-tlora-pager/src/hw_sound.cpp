// ES8311 (I2C 0x18) + I2S std TX → NS4150B amp (XL9555 EXP_AMP_EN).
//
// Clocking: I2S master @ 16 kHz, MCLK = 256×fs = 4.096 MHz on GPIO10.
// Codec init condensed from Espressif es8311 component for that pair.
// Beep patterns mirror tembed sound.cpp (info 1×, warn 2×, crit 3× chirps).

#include "hw_sound.h"
#include "board_pins.h"
#include "hw_ui.h"

#include <Wire.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#define ES_ADDR     0x18
#define SND_RATE    16000
#define SND_VOL     70          // 0..100 into DAC reg
#define CHUNK       128         // mono frames per poll write

// ES8311 regs we touch
#define ES_RESET    0x00
#define ES_CLK1     0x01
#define ES_CLK2     0x02
#define ES_CLK3     0x03
#define ES_CLK4     0x04
#define ES_CLK5     0x05
#define ES_CLK6     0x06
#define ES_CLK7     0x07
#define ES_CLK8     0x08
#define ES_SDPIN    0x09
#define ES_SDPOUT   0x0A
#define ES_SYS0D    0x0D
#define ES_SYS0E    0x0E
#define ES_SYS12    0x12
#define ES_SYS13    0x13
#define ES_DAC31    0x31
#define ES_DAC32    0x32
#define ES_DAC37    0x37

static bool snd_ok = false;
static i2s_chan_handle_t i2s_tx = nullptr;

// Beep queue: up to 4 chirps
struct Beep { uint16_t hz; uint16_t on_ms; uint16_t gap_ms; };
static Beep beeps[4];
static uint8_t beep_n = 0;
static uint8_t beep_i = 0;
static uint32_t beep_frame = 0;
static uint32_t phase = 0;          // 16.16 into 256-sine
static bool playing = false;
static unsigned long amp_hold_until = 0;

// 256-point sine, amplitude ~28000 (headroom under int16).
static int16_t SINE[256];
static void sine_build() {
    static bool done = false;
    if (done) return;
    for (int i = 0; i < 256; i++) {
        SINE[i] = (int16_t)(sinf((float)i * 6.2831853f / 256.0f) * 28000.0f);
    }
    done = true;
}

static bool es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es_read(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(ES_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ES_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

static void amp(bool on) {
    hw_xl_pin(EXP_AMP_EN, on);
    if (on) amp_hold_until = millis() + 400;
}

// 16 kHz, 16-bit, MCLK 4.096 MHz (256×) — coeffs from esp-bsp es8311.c table.
static bool es8311_init_16k() {
    uint8_t id = 0;
    if (!es_read(0xFD, &id) && !es_read(ES_RESET, &id)) {
        // Presence: any ACK on a known reg
    }
    // Try a no-op write path probe
    if (!es_write(ES_RESET, 0x1F)) return false;
    delay(20);
    es_write(ES_RESET, 0x00);
    es_write(ES_RESET, 0x80);  // power on

    // Clocks from MCLK pin, all internal clocks on
    es_write(ES_CLK1, 0x3F);

    // 4096000 / 16000: pre_div=1→0, pre_multi=0, adc_div=1, dac_div=1,
    // fs_mode=0, lrck=0x00ff, bclk_div=4, osr=0x10
    es_write(ES_CLK2, 0x00);               // pre_div-1=0, pre_multi=0
    es_write(ES_CLK3, 0x10);               // fs_mode | adc_osr
    es_write(ES_CLK4, 0x10);               // dac_osr
    es_write(ES_CLK5, 0x00);               // (adc_div-1)<<4 | (dac_div-1)
    es_write(ES_CLK6, 0x03);               // bclk_div-1 = 3 → div 4; SCLK not inverted
    es_write(ES_CLK7, 0x00);               // lrck high
    es_write(ES_CLK8, 0xFF);               // lrck low

    // Slave, I2S, 16-bit in/out (res code 3 << 2)
    es_write(ES_SDPIN,  0x0C);
    es_write(ES_SDPOUT, 0x0C);

    es_write(ES_SYS0D, 0x01);  // analog power
    es_write(ES_SYS0E, 0x02);  // PGA / ADC path (harmless for DAC-only)
    es_write(ES_SYS12, 0x00);  // DAC power up
    es_write(ES_SYS13, 0x10);  // HP drive
    es_write(ES_DAC37, 0x08);  // EQ bypass

    // Volume 0..100 → reg32
    int reg32 = (SND_VOL <= 0) ? 0 : ((SND_VOL * 256 / 100) - 1);
    if (reg32 > 255) reg32 = 255;
    es_write(ES_DAC32, (uint8_t)reg32);
    es_write(ES_DAC31, 0x00);  // unmute

    return true;
}

static bool i2s_init() {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &i2s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SND_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PIN_I2S_MCLK,
            .bclk = (gpio_num_t)PIN_I2S_SCK,
            .ws   = (gpio_num_t)PIN_I2S_WS,
            .dout = (gpio_num_t)PIN_I2S_SDOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (i2s_channel_init_std_mode(i2s_tx, &cfg) != ESP_OK) return false;
    if (i2s_channel_enable(i2s_tx) != ESP_OK) return false;
    return true;
}

bool hw_sound_begin() {
    sine_build();
    if (!es8311_init_16k()) {
        Serial.println("[sound] ES8311 missing");
        snd_ok = false;
        return false;
    }
    if (!i2s_init()) {
        Serial.println("[sound] I2S init failed");
        snd_ok = false;
        return false;
    }
    amp(true);   // rail up; held by poll
    snd_ok = true;
    Serial.println("[sound] ES8311 + I2S ok");
    // Quiet identity chirp
    hw_sound_notify(0);
    return true;
}

bool hw_sound_ok() { return snd_ok; }

void hw_sound_notify(uint8_t level) {
    if (!snd_ok) return;
    beep_n = 0;
    beep_i = 0;
    beep_frame = 0;
    phase = 0;
    // Same musical idea as tembed: info one, warn two, crit three rising.
    if (level >= 2) {
        beeps[beep_n++] = {1319, 70, 60};
        beeps[beep_n++] = {1568, 70, 60};
        beeps[beep_n++] = {2093, 120, 0};
    } else if (level == 1) {
        beeps[beep_n++] = {988, 100, 90};
        beeps[beep_n++] = {988, 100, 0};
    } else {
        beeps[beep_n++] = {880, 150, 0};
    }
    playing = true;
    amp(true);
}

void hw_sound_poll() {
    if (!snd_ok) return;

    // Drop amp a bit after the last cue so the PA does not idle-hiss forever.
    if (!playing && amp_hold_until && (long)(millis() - amp_hold_until) > 0) {
        amp(false);
        amp_hold_until = 0;
    }
    if (!playing || beep_i >= beep_n) {
        if (playing) {
            playing = false;
            amp_hold_until = millis() + 300;
        }
        return;
    }

    int16_t stereo[CHUNK * 2];
    uint16_t made = 0;
    while (made < CHUNK && beep_i < beep_n) {
        const Beep &b = beeps[beep_i];
        uint32_t on_f  = (uint32_t)b.on_ms  * SND_RATE / 1000;
        uint32_t gap_f = (uint32_t)b.gap_ms * SND_RATE / 1000;
        uint32_t total = on_f + gap_f;
        if (total == 0) { beep_i++; beep_frame = 0; continue; }

        // phase step: freq / rate * 256 << 8  → 16.16 into 256 table
        uint32_t step = ((uint32_t)b.hz << 16) / SND_RATE;

        while (made < CHUNK && beep_frame < total) {
            int16_t s = 0;
            if (beep_frame < on_f) {
                s = SINE[(phase >> 8) & 0xFF];
                // short ramp in/out ~2 ms
                uint32_t ramp = SND_RATE / 500;
                if (ramp < 1) ramp = 1;
                int32_t env = 255;
                if (beep_frame < ramp)
                    env = (int32_t)(beep_frame * 255 / ramp);
                else if (beep_frame > on_f - ramp)
                    env = (int32_t)((on_f - beep_frame) * 255 / ramp);
                s = (int16_t)((s * env) / 255);
                phase += step;
            }
            stereo[made * 2]     = s;
            stereo[made * 2 + 1] = s;
            made++;
            beep_frame++;
        }
        if (beep_frame >= total) {
            beep_frame = 0;
            beep_i++;
            phase = 0;
        }
    }

    size_t wrote = 0;
    i2s_channel_write(i2s_tx, stereo, made * 4, &wrote, 0);
}
