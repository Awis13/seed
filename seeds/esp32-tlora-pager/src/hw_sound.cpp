// ES8311 (I2C 0x18) + I2S std TX → NS4150B amp (XL9555 EXP_AMP_EN).
//
// Clocking: I2S master @ 16 kHz, MCLK = 256×fs = 4.096 MHz on GPIO10.
// Codec init condensed from Espressif es8311 component for that pair.
//
// Beeps: same 1/2/3 pattern as tembed. Soft edges (ramp) so the PA does not
// click; PA stays powered after begin — toggling EXP_AMP_EN after the boot
// blip left subsequent notify cues silent (amp never came back cleanly).

#include "hw_sound.h"
#include "board_pins.h"
#include "hw_ui.h"

#include <Wire.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#define ES_ADDR     0x18
#define SND_RATE    16000
#define SND_VOL     85          // 0..100 into DAC reg
#define CHUNK       256         // frames per poll write (~16 ms @ 16 kHz)

#define SND_TONE_AMP   20000    // of 32767
#define SND_RAMP_MS        6    // envelope edge
#define SND_HEAD_MS       12    // short lead-in silence (zero-cross)
#define SND_PUMP_ROUNDS    4    // refill DMA several times per loop pass

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
static uint32_t head_left = 0;      // leading silence frames
static bool playing = false;
static bool amp_on = false;

// Full-scale 256-point sine; loudness is SND_TONE_AMP in the fill path.
static int16_t SINE[256];
static void sine_build() {
    static bool done = false;
    if (done) return;
    for (int i = 0; i < 256; i++) {
        SINE[i] = (int16_t)(sinf((float)i * 6.2831853f / 256.0f) * 32767.0f);
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

// PA on and leave it on. Cutting EXP_AMP_EN after the boot cue made the next
// notify silent on this board (rail did not recover for the cue window).
static void amp_ensure() {
    if (amp_on) return;
    hw_xl_pin(EXP_AMP_EN, true);
    amp_on = true;
    es_write(ES_DAC31, 0x00);  // unmute DAC
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

static void queue_cue(const Beep *seq, uint8_t n, uint16_t extra_head_ms) {
    beep_n = 0;
    beep_i = 0;
    beep_frame = 0;
    phase = 0;
    for (uint8_t i = 0; i < n && i < 4; i++) beeps[beep_n++] = seq[i];
    head_left = (uint32_t)(SND_HEAD_MS + extra_head_ms) * SND_RATE / 1000;
    playing = true;
    amp_ensure();
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
    snd_ok = true;
    Serial.println("[sound] ES8311 + I2S ok");
    amp_ensure();
    delay(30);
    // Soft boot identity (A5)
    const Beep boot[] = {{880, 120, 0}};
    queue_cue(boot, 1, 20);
    return true;
}

bool hw_sound_ok() { return snd_ok; }

void hw_sound_notify(uint8_t level) {
    if (!snd_ok) return;
    // Count = severity (tembed). Mid-range pitches for the pager speaker.
    if (level >= 2) {
        const Beep seq[] = {{1175, 70, 55}, {1397, 70, 55}, {1760, 120, 0}};
        queue_cue(seq, 3, 0);
    } else if (level == 1) {
        const Beep seq[] = {{988, 100, 85}, {988, 110, 0}};
        queue_cue(seq, 2, 0);
    } else {
        const Beep seq[] = {{880, 140, 0}};
        queue_cue(seq, 1, 0);
    }
    Serial.printf("[sound] cue level=%u beeps=%u\n", (unsigned)level, (unsigned)beep_n);
}

// One DMA refill. Returns true if a cue is still in progress.
static bool sound_fill_once() {
    if (!playing) return false;
    if (beep_i >= beep_n && head_left == 0) {
        playing = false;
        return false;
    }

    int16_t stereo[CHUNK * 2];
    uint16_t made = 0;

    while (made < CHUNK && head_left > 0) {
        stereo[made * 2] = stereo[made * 2 + 1] = 0;
        made++;
        head_left--;
    }

    const uint32_t ramp = (uint32_t)SND_RAMP_MS * SND_RATE / 1000;
    while (made < CHUNK && beep_i < beep_n) {
        const Beep &b = beeps[beep_i];
        uint32_t on_f  = (uint32_t)b.on_ms  * SND_RATE / 1000;
        uint32_t gap_f = (uint32_t)b.gap_ms * SND_RATE / 1000;
        uint32_t total = on_f + gap_f;
        if (total == 0) { beep_i++; beep_frame = 0; continue; }

        uint32_t step = ((uint32_t)b.hz << 16) / SND_RATE;

        while (made < CHUNK && beep_frame < total) {
            int16_t s = 0;
            if (beep_frame < on_f) {
                int32_t raw = SINE[(phase >> 8) & 0xFF];
                int32_t env = 255;
                if (ramp > 0) {
                    if (beep_frame < ramp)
                        env = (int32_t)(beep_frame * 255 / ramp);
                    else if (beep_frame + ramp > on_f)
                        env = (int32_t)((on_f - beep_frame) * 255 / ramp);
                    if (env < 0) env = 0;
                    if (env > 255) env = 255;
                }
                s = (int16_t)((raw * SND_TONE_AMP / 32767) * env / 255);
                phase += step;
            } else {
                phase = 0;
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

    if (made == 0) return playing;
    size_t want = (size_t)made * 4;
    size_t off = 0;
    while (off < want) {
        size_t n = 0;
        if (i2s_channel_write(i2s_tx, ((uint8_t *)stereo) + off, want - off,
                              &n, pdMS_TO_TICKS(20)) != ESP_OK) break;
        if (n == 0) break;
        off += n;
    }
    return playing || head_left > 0 || beep_i < beep_n;
}

void hw_sound_poll() {
    if (!snd_ok) return;
    // Several refills per loop so a heavy UI paint cannot starve the cue.
    for (int r = 0; r < SND_PUMP_ROUNDS; r++) {
        if (!sound_fill_once()) break;
    }
}
