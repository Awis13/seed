/*
 * skills/radio.cpp — SI4732 radio skill for the ATS-Mini seed (C1 + C2)
 *
 * Drives the SI4732 receiver that hangs off the I2C bus (SDA=18, SCL=17) with
 * its RESET on GPIO16. This cut does FM + AM + SSB (LSB/USB) tuning, a
 * signal-quality status readout and a blocking single-shot band scan — no band
 * presets, no live background scan, no RDS (those grow later).
 *
 * The receiver is brought up once, at boot, from skill_radio_init(). Wire and
 * board power are already up by then (hw_probe() runs first in setup()), so this
 * skill never touches Wire.begin() or the power-hold line.
 *
 * SSB needs a firmware patch (ssb_patch_content, from patch_init.h) uploaded to
 * the SI4732 RAM. That patch is volatile — it is dropped on any power-down and,
 * in practice, whenever we drive the chip back to FM/AM. So we load it lazily on
 * the first SSB tune and clear radio_ssb_loaded whenever we leave SSB, forcing a
 * fresh reload next time. The boot path stays FM and never touches the patch.
 *
 * Endpoints:
 *   POST /radio/tune    — tune: {mode:"FM"|"AM"|"LSB"|"USB", freq:<int>, bfo?:<int>}
 *   POST /radio/band    — jump to a band-plan preset: {idx:<int>}
 *   POST /radio/scan    — blocking band sweep in the current mode: {from,to,step,min_rssi?}
 *   GET  /radio/status  — current freq/mode/RSSI/SNR (+bfo in SSB)
 */

#include <SI4735.h>
#include "../patch_init.h"
#include "../Rotary.h"
#include "../Button.h"

/*
 * Mode constants follow the ref ats-mini numbering so the SSB mode value doubles
 * as setSSB()'s usblsb argument (1=LSB, 2=USB) — no separate mapping needed.
 * FM stays 0 (SI4735 defaultFunction for setup()); AM moved 1 -> 3. Modes travel
 * over HTTP as strings, so this internal renumbering is not visible to callers.
 */
#define RADIO_MODE_FM  0
#define RADIO_MODE_LSB 1
#define RADIO_MODE_USB 2
#define RADIO_MODE_AM  3

/* FM band limits in 10 kHz units: 6400=64.0 MHz .. 10800=108.0 MHz */
#define RADIO_FM_MIN 6400
#define RADIO_FM_MAX 10800
/* AM band limits in kHz: 520 kHz .. 1710 kHz (MW) */
#define RADIO_AM_MIN 520
#define RADIO_AM_MAX 1710
/* SSB band window in kHz: 1.8 MHz .. 30 MHz (whole HF range) */
#define RADIO_SSB_MIN 1800
#define RADIO_SSB_MAX 30000
/* SSB fine-tuning (BFO) limit in Hz, applied symmetrically (-MAX..+MAX) */
#define RADIO_BFO_MAX 14000

/*
 * Scan step ceiling. A blocking sweep re-tunes the receiver on every step; each
 * setFrequency() + signal-quality read settles in ~33 ms, so 64 steps take
 * ~2.1 s — safely under the async task's 3 s ACK timeout. Larger sweeps must use
 * a coarser step or be paginated across several requests.
 */
#define RADIO_SCAN_MAX_STEPS 64

/*
 * Squelch poll cadence. The signal-gate re-reads RSSI/SNR (an I2C round-trip taken
 * under radio_mtx) on this interval from radio_tick(), NOT every loop — a per-tick
 * read would spam the bus and fight the mutex. 250 ms is fast enough to gate speech
 * pauses without audible chatter.
 */
#define RADIO_SQUELCH_POLL_MS 250

/*
 * Radio-state persistence. The last-tuned mode/freq/volume/bfo are stashed in a
 * tiny JSON file on SPIFFS so a reboot lands back where the user left off. Writes
 * are debounced: a real change marks the state dirty and, once the knob has been
 * idle for RADIO_STORE_IDLE_MS, radio_tick() flushes one snapshot to flash. This
 * coalesces a fast encoder sweep (dozens of freq changes) into a single write and
 * keeps the blocking flash write off the settle-critical tuning path.
 */
#define RADIO_STATE_FILE    "/radio.json"
/* v2 added the selected band-plan index; v3 adds the active band's live demod mode
 * and its step/bandwidth cursors so a mode switch and tuning-step/bandwidth choice
 * survive a reboot; v4 adds the packed squelch byte (threshold + metric select) so
 * the signal gate survives too. An older-version file is rejected by the version
 * check in radio_restore_state() and falls back to the FM defaults. v5 adds the
 * seven per-mode DSP scalars (AGC/AVC/SoftMute), all global (not per-band). */
#define RADIO_STATE_VERSION 5
#define RADIO_STORE_IDLE_MS 10000

/*
 * Band plan. A flat list of presets covering the FM broadcast band, MW, the SW
 * broadcast segments and the amateur SSB allocations. The table is `static const`
 * so it lives in flash (.rodata) — it is never mutated at runtime; the per-band
 * "where was I last" frequency lives in the separate band_freq[] RAM array.
 *
 * Frequency units follow the receiver's own convention, so a preset feeds the
 * chip set-call directly: FM in 10 kHz units (10390 = 103.9 MHz); everything
 * else (MW/SW/SSB) in kHz, which uint16_t holds up to 30000 = 30 MHz. `mode`
 * reuses the RADIO_MODE_* constants; `type` only picks the AM tuning step and
 * groups rows for the menu that lands in the next cut.
 *
 * Frequencies are portered from the reference ats-mini band table, rounded to the
 * broadcast/amateur segment edges. Editing this list changes both the menu order
 * (C2) and the persisted-index meaning, so append rather than reorder once state
 * files exist in the wild.
 */
#define BAND_TYPE_FM  0
#define BAND_TYPE_MW  1
#define BAND_TYPE_SW  2
#define BAND_TYPE_SSB 3

struct RadioBand {
    const char *name;   /* short label shown in the menu / band response */
    uint8_t     type;   /* BAND_TYPE_* — selects AM step, groups the menu */
    uint8_t     mode;   /* RADIO_MODE_FM | _LSB | _USB | _AM */
    uint16_t    minFreq;/* lower edge (FM: 10 kHz units, else kHz) */
    uint16_t    maxFreq;/* upper edge (same units as minFreq) */
    uint16_t    defFreq;/* power-on frequency for this band (same units) */
};

static const RadioBand bands[] = {
    /* FM broadcast: 10 kHz units. */
    {"VHF",    BAND_TYPE_FM,  RADIO_MODE_FM,   6400, 10800, 10390},
    /* MW (AM), kHz. */
    {"MW",     BAND_TYPE_MW,  RADIO_MODE_AM,    520,  1710,   900},
    /* SW broadcast segments (AM), kHz. */
    {"49M",    BAND_TYPE_SW,  RADIO_MODE_AM,   5900,  6200,  6050},
    {"41M",    BAND_TYPE_SW,  RADIO_MODE_AM,   7200,  7450,  7325},
    {"31M",    BAND_TYPE_SW,  RADIO_MODE_AM,   9400,  9900,  9650},
    {"25M",    BAND_TYPE_SW,  RADIO_MODE_AM,  11600, 12100, 11850},
    {"22M",    BAND_TYPE_SW,  RADIO_MODE_AM,  13570, 13870, 13720},
    {"19M",    BAND_TYPE_SW,  RADIO_MODE_AM,  15100, 15830, 15465},
    {"16M",    BAND_TYPE_SW,  RADIO_MODE_AM,  17480, 17900, 17690},
    {"13M",    BAND_TYPE_SW,  RADIO_MODE_AM,  21450, 21850, 21650},
    {"SW-ALL", BAND_TYPE_SW,  RADIO_MODE_AM,   1710, 30000,  9600},
    /* Amateur SSB allocations, kHz. LSB below 10 MHz, USB above (convention). */
    {"160M",   BAND_TYPE_SSB, RADIO_MODE_LSB,  1800,  2000,  1900},
    {"80M",    BAND_TYPE_SSB, RADIO_MODE_LSB,  3500,  4000,  3750},
    {"40M",    BAND_TYPE_SSB, RADIO_MODE_LSB,  7000,  7300,  7150},
    {"30M",    BAND_TYPE_SSB, RADIO_MODE_USB, 10100, 10150, 10125},
    {"20M",    BAND_TYPE_SSB, RADIO_MODE_USB, 14000, 14350, 14175},
    {"17M",    BAND_TYPE_SSB, RADIO_MODE_USB, 18068, 18168, 18118},
    {"15M",    BAND_TYPE_SSB, RADIO_MODE_USB, 21000, 21450, 21225},
    {"10M",    BAND_TYPE_SSB, RADIO_MODE_USB, 28000, 29700, 28850},
};
#define BAND_COUNT ((uint8_t)(sizeof(bands) / sizeof(bands[0])))

/*
 * Tuning-step and channel-bandwidth tables, one set per mode family. All are
 * `static const` so they live in flash (.rodata) — never IRAM, never mutated.
 * The per-band "which row is selected" cursor lives in the band_step_idx[] /
 * band_bw_idx[] RAM arrays below; these tables are just the fixed menus.
 *
 * Numbers are ported from the reference ats-mini step/bandwidth tables (units and
 * hardware filter indices are the chip's, so a row feeds the SI4735 set-call
 * directly); the code and layout here are our own.
 */

/* One selectable tuning step. `step` is in the receiver's own frequency units for
 * the mode it belongs to (FM: 10 kHz units, so 100 = 1 MHz; AM/SSB: kHz). `desc`
 * is the menu label. It is fed straight to setFM/setAM's step arg and to
 * setFrequencyStep(), which sets the chip's currentStep used by frequencyUp/Down. */
struct RadioStep { uint16_t step; const char *desc; };

/* FM steps, 10 kHz units: 1 = 10 kHz .. 100 = 1 MHz. */
static const RadioStep fmSteps[] = {
    {1, "10k"}, {5, "50k"}, {10, "100k"}, {20, "200k"}, {100, "1M"},
};
/* AM steps, kHz: also reused for SSB in this cut (SSB fine-tune via BFO is out of
 * scope, so SSB just borrows a reasonable step set — see stepsForMode). */
static const RadioStep amSteps[] = {
    {1, "1k"}, {5, "5k"}, {9, "9k"}, {10, "10k"},
    {50, "50k"}, {100, "100k"}, {1000, "1M"},
};
#define FM_STEP_COUNT ((uint8_t)(sizeof(fmSteps) / sizeof(fmSteps[0])))
#define AM_STEP_COUNT ((uint8_t)(sizeof(amSteps) / sizeof(amSteps[0])))

/* One selectable channel bandwidth. `hwIdx` is the SI473X filter index the chip
 * expects — the rows are deliberately re-sorted by real audio width, so hwIdx is
 * NOT the row position (e.g. the narrowest SSB filter is hwIdx 4). `desc` is the
 * menu label. Feed .hwIdx (never the row index) to the set-bandwidth calls. */
struct RadioBw { uint8_t hwIdx; const char *desc; };

/* FM bandwidth (setFmBandwidth): 0 = Auto, then fixed IF widths. */
static const RadioBw fmBw[] = {
    {0, "Auto"}, {1, "110k"}, {2, "84k"}, {3, "60k"}, {4, "40k"},
};
/* SSB audio bandwidth (setSSBAudioBandwidth), rows sorted narrow -> wide. */
static const RadioBw ssbBw[] = {
    {4, "0.5k"}, {5, "1.0k"}, {0, "1.2k"}, {1, "2.2k"}, {2, "3.0k"}, {3, "4.0k"},
};
/* AM channel-filter bandwidth (setBandwidth), rows sorted narrow -> wide. */
static const RadioBw amBw[] = {
    {4, "1.0k"}, {5, "1.8k"}, {3, "2.0k"}, {6, "2.5k"},
    {2, "3.0k"}, {1, "4.0k"}, {0, "6.0k"},
};
#define FM_BW_COUNT  ((uint8_t)(sizeof(fmBw) / sizeof(fmBw[0])))
#define SSB_BW_COUNT ((uint8_t)(sizeof(ssbBw) / sizeof(ssbBw[0])))
#define AM_BW_COUNT  ((uint8_t)(sizeof(amBw) / sizeof(amBw[0])))

/* Default table row per mode, indexed by RADIO_MODE_* (FM=0/LSB=1/USB=2/AM=3).
 * Step: FM -> 100k, SSB -> 10k (amSteps), AM -> 5k. Bandwidth: FM -> Auto,
 * SSB/AM -> "3.0k" (row index 4 in ssbBw/amBw). Seeded into the per-band arrays. */
static const uint8_t defaultStepIdx[4] = {2, 3, 3, 1};
static const uint8_t defaultBwIdx[4]   = {0, 4, 4, 4};

/* Dispatch: pick the step/bandwidth table (and its length) for a mode. FM and AM
 * get their own; LSB/USB share ssbBw for bandwidth and borrow amSteps for step. */
static const RadioStep *stepsForMode(uint8_t mode) {
    return (mode == RADIO_MODE_FM) ? fmSteps : amSteps;
}
static uint8_t stepCount(uint8_t mode) {
    return (mode == RADIO_MODE_FM) ? FM_STEP_COUNT : AM_STEP_COUNT;
}
static const RadioBw *bwForMode(uint8_t mode) {
    if (mode == RADIO_MODE_FM) return fmBw;
    if (mode == RADIO_MODE_LSB || mode == RADIO_MODE_USB) return ssbBw;
    return amBw;
}
static uint8_t bwCount(uint8_t mode) {
    if (mode == RADIO_MODE_FM) return FM_BW_COUNT;
    if (mode == RADIO_MODE_LSB || mode == RADIO_MODE_USB) return SSB_BW_COUNT;
    return AM_BW_COUNT;
}

/* --- Receiver + state (file-local) --- */
static SI4735 rx;
static uint16_t radio_freq;              /* current frequency (FM: 10 kHz units, AM/SSB: kHz) */
static uint8_t  radio_mode;              /* RADIO_MODE_FM | _LSB | _USB | _AM */
static uint8_t  radio_volume = 40;       /* 0..63 */
static bool     radio_ok = false;        /* true once the SI4732 answered on I2C */
static bool     radio_ssb_loaded = false;/* true while the SSB patch is live in chip RAM */
static int      radio_bfo = 0;           /* current BFO offset in Hz (SSB only) */

/*
 * Layered audio mute + squelch state. See radio_set_mute() for the OR layering.
 * radio_squelch packs the signal gate: the low 7 bits (&0x7f) are the threshold
 * 0..127 (0 = squelch off), the top bit (0x80) selects the metric (1 = SNR, else
 * RSSI). All are written only under radio_mtx (mute_squelch also from the loop-task
 * squelch poll), same discipline as the receiver state above. radio_squelch (the
 * threshold + metric) is persisted as of v4; mute_main is not — a reboot lands with
 * manual mute off but the saved squelch gate restored.
 */
enum MuteLayer { MUTE_MAIN, MUTE_SQUELCH, MUTE_TEMP };
static bool     mute_main = false;       /* user/HTTP mute toggle (MUTE_MAIN layer) */
static bool     mute_squelch = false;    /* signal-gate mute (MUTE_SQUELCH layer) */
static bool     audio_muted = false;     /* current physical DSP soft-mute state */
static uint8_t  radio_squelch = 0;       /* packed: [7]=metric(1=SNR), [6:0]=threshold */

/*
 * Per-mode DSP tuning: AGC (attenuator index), AVC (max gain) and SoftMute (max
 * attenuation). One scalar per mode family, so switching mode automatically picks
 * up that family's stored value (apply_*_locked read the live mode). All persisted
 * as of v5 (global, not per-band). Written only under radio_mtx (or single-tasked
 * at boot), same discipline as the receiver state above.
 *
 *  - AGC index: 0 = AGC on (no attenuation); >0 = manual attenuator step. Range is
 *    per mode (ref doAgc): FM 0..27, AM 0..37, SSB 0..1 — see agc_max_for_mode().
 *    Applies to every mode, FM included; a default of 0 reproduces the old FM AGC.
 *  - AVC (AM/SSB only, never FM): setAvcAmMaxGain max gain, even values 12..90.
 *  - SoftMute (AM/SSB only, never FM): max attenuation 0..32.
 */
static int8_t   fm_agc  = 0;             /* FM AGC/attenuator index, 0..27 (0 = AGC on) */
static int8_t   am_agc  = 0;             /* AM AGC/attenuator index, 0..37 (0 = AGC on) */
static int8_t   ssb_agc = 0;             /* SSB AGC/attenuator index, 0..1 (0 = AGC on) */
static int8_t   am_avc  = 48;            /* AM AVC max gain, even 12..90 */
static int8_t   ssb_avc = 48;            /* SSB AVC max gain, even 12..90 */
static int8_t   am_sm   = 4;             /* AM soft-mute max attenuation, 0..32 */
static int8_t   ssb_sm  = 4;             /* SSB soft-mute max attenuation, 0..32 */

/*
 * Band-plan cursor and the per-band "where was I" store. radio_band_idx indexes
 * bands[]; band_freq[i] remembers the last frequency tuned on band i so a
 * round-trip through the band list lands each one back where the user left it.
 * band_freq[] is seeded from each band's defFreq in skill_radio_init() before the
 * saved state is restored. Both are written only under radio_mtx (or single-tasked
 * at boot), same as radio_freq.
 */
static uint8_t  radio_band_idx = 0;      /* index into bands[]; 0 = FM VHF */
static uint16_t band_freq[BAND_COUNT];   /* per-band saved frequency (band's units) */
/* Per-band live demodulation mode, seeded from each band's native bands[i].mode in
 * skill_radio_init() (before restore). The Mode menu / POST /radio/config can move a
 * non-FM band between AM/LSB/USB, so this — not bands[i].mode — is the source of truth
 * for the band's demod and for which step/bandwidth table the indices below belong to.
 * A native-FM band (VHF broadcast) stays RADIO_MODE_FM: mode switching is locked there.
 * In RAM (mutable); written only under radio_mtx (or single-tasked at boot). */
static uint8_t  band_mode[BAND_COUNT];
/* Per-band selected step / bandwidth, as row indices into stepsForMode(mode) /
 * bwForMode(mode) for that band's mode. Seeded from defaultStepIdx/defaultBwIdx in
 * skill_radio_init() before the saved state is restored. Written only under
 * radio_mtx (or single-tasked at boot), same as band_freq[]. Not persisted yet. */
static uint8_t  band_step_idx[BAND_COUNT];
static uint8_t  band_bw_idx[BAND_COUNT];

/*
 * Canonical mode of the active band's step/bandwidth tables. The stored
 * band_step_idx/band_bw_idx are row indices into THIS mode's table — the band's
 * own mode (bands[radio_band_idx].mode), NOT the live radio_mode. /radio/tune
 * moves radio_mode to another mode without touching radio_band_idx, so the two
 * diverge; reading the per-band step/bw index under radio_mode could index a
 * shorter table (e.g. FM steps=5) with an index validated for a longer one
 * (AM=7) and run off the end. Always trust the band's mode for these tables.
 * Single-thread reader (display/loop task); callers that also need radio_band_idx
 * under radio_mtx derive the mode from their locked local instead (see below).
 *
 * Reads band_mode[] (the live per-band demod), NOT bands[].mode: after a mode switch
 * the two diverge, and the step/bandwidth indices always belong to the live mode's
 * table. This keeps step/bw menus, /radio/status and the chip set-calls consistent.
 */
static uint8_t band_table_mode() { return band_mode[radio_band_idx]; }

/* Defensive clamp of a stored table row into [0, count-1]. Tables are never empty
 * (count >= 1), so count-1 is always valid. Used at every table dereference so a
 * broken invariant degrades to the last row instead of an out-of-bounds read. */
static uint8_t clamp_idx(uint8_t v, uint8_t count) {
    return (v >= count) ? (uint8_t)(count - 1) : v;
}

/*
 * The demod modes the Mode menu cycles through, in display order. FM is deliberately
 * excluded: it is the native mode of the VHF broadcast band only and cannot be
 * selected on the AM/SW/ham bands (nor can a VHF band leave FM). mode_cycle_pos maps
 * a mode to its position here (0 if absent, e.g. FM) for the cursor/wrap arithmetic.
 */
static const uint8_t modeCycle[] = { RADIO_MODE_AM, RADIO_MODE_LSB, RADIO_MODE_USB };
#define MODE_CYCLE_COUNT ((uint8_t)(sizeof(modeCycle) / sizeof(modeCycle[0])))

static uint8_t mode_cycle_pos(uint8_t mode) {
    for (uint8_t i = 0; i < MODE_CYCLE_COUNT; i++)
        if (modeCycle[i] == mode) return i;
    return 0;
}

/*
 * Persistence bookkeeping. radio_dirty is set on a genuine state change (a tune,
 * a volume change, an encoder step); radio_last_change_ms timestamps it so the
 * flush in radio_tick() can wait out the idle window before touching flash.
 * Restore on boot must NOT set these — it is reapplying already-saved state.
 */
static bool     radio_dirty = false;
static uint32_t radio_last_change_ms = 0;

static void radio_mark_dirty() {
    radio_dirty = true;
    radio_last_change_ms = millis();
}

/*
 * Every I2C access to `rx` now runs from two contexts: the async HTTP task
 * (tune/scan/status) and loop()'s radio_tick() (encoder-driven tuning + the
 * display readout that snapshots signal quality). Concurrent SI4735 transfers
 * on the shared Wire bus corrupt each other and hang the receiver, so a mutex
 * serialises them. Held only around the rx calls themselves — never across a
 * screen repaint.
 */
static SemaphoreHandle_t radio_mtx = nullptr;

/*
 * Rotary encoder on PIN_ENC_A/PIN_ENC_B (Ben Buxton's decoder). The constructor
 * takes (pin1, pin2) = (B, A): passing them in this order makes a clockwise turn
 * read as DIR_CW, matching "clockwise tunes up". An interrupt on both pins feeds
 * process(); each detent nudges enc_delta, drained by radio_tick().
 */
static Rotary encoder = Rotary(PIN_ENC_B, PIN_ENC_A);
/* Two parallel detent counters, both accumulated by rotary_isr and drained
 * together in radio_tick. enc_delta is the raw count (+/-1 per detent, used for
 * the precise volume nudge); enc_delta_accel is the acceleration-weighted count
 * (a fast spin multiplies each detent, used for the coarse frequency sweep). */
static volatile int8_t  enc_delta = 0;
static volatile int16_t enc_delta_accel = 0;
/* Guards the read-and-clear of the two counters in radio_tick against the ISR
 * that increments them, so a step can't be dropped between the read and the
 * reset (and raw/accel are snapshotted as one consistent pair). */
static portMUX_TYPE enc_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Turn a raw detent direction into an acceleration-weighted delta. Ported from
 * ats-mini's accelerateEncoder (integer-only, EMA-smoothed inter-detent time).
 * A slow turn returns +/-1; the faster you spin, the larger the multiplier
 * (up to 16x), so the frequency jumps in bigger strides on a fast sweep.
 *
 * Runs from rotary_isr (ISR context). Defense-in-depth against the flash cache:
 * the function itself is IRAM_ATTR and its lookup tables are DRAM_ATTR, so nothing
 * on this path reads flash. millis() is already an IRAM function on ESP32 Arduino.
 * (encoder.process() in rotary_isr still reads its Rotary table from flash, but
 * that is pre-existing and bracketed by detach — see radio_encoder_pause.) Both
 * flash-write paths — OTA Update.write AND the SPIFFS write_spiffs_file — detach
 * the encoder first, so there is no race; keeping this path IRAM/DRAM just removes
 * the flash surface from the ISR entirely. Keep it free of any flash-resident calls.
 */
static int16_t IRAM_ATTR accelerate_encoder(int8_t dir) {
    static const DRAM_ATTR uint32_t th[] = {350, 60, 45, 35, 25};  // ms between detents
    static const DRAM_ATTR uint16_t fac[] = {1, 2, 4, 8, 16};      // matching multipliers
    static uint32_t last_t = 0, last_speed = 350;
    static uint16_t last_fac = 1;
    static int8_t last_dir = 0;

    uint32_t now = millis();
    uint32_t dt = now - last_t;
    if (dt > th[0]) dt = th[0];  // clamp idle gap so dt*7 can't overflow uint32
    last_speed = (dt * 7 + last_speed * 3) / 10;  // EMA 70/30
    if (last_speed > th[0] || last_dir != dir) {
        last_speed = th[0];
        last_fac = 1;
    } else {
        for (int8_t i = 4; i >= 0; i--) {
            if (last_speed <= th[i] && last_fac < fac[i]) { last_fac = fac[i]; break; }
        }
    }
    last_t = now;
    last_dir = dir;
    return (int16_t)dir * (int16_t)last_fac;
}

static void IRAM_ATTR rotary_isr() {
    uint8_t s = encoder.process();
    int8_t dir = 0;
    if (s == DIR_CW) dir = 1;
    else if (s == DIR_CCW) dir = -1;
    if (dir != 0) {
        /* Backpressure: if radio_tick isn't draining (raw count backing up),
         * stop accumulating so raw and accel counters stay in step. */
        if (abs((int)enc_delta) < 5) {
            enc_delta = enc_delta + dir;
            enc_delta_accel = enc_delta_accel + accelerate_encoder(dir);
        }
    }
}

/*
 * Detach / re-attach the encoder interrupts. rotary_isr() calls encoder.process(),
 * whose Rotary state table lives in flash; while the flash cache is disabled (an
 * OTA Update.write or a SPIFFS write) any code that reads flash from an ISR
 * crashes the chip. main.cpp brackets those flash writes with these so no encoder
 * edge can fire mid-write. Non-static: forward-declared in main.cpp.
 *
 * detachInterrupt on an unattached pin is a safe no-op, so calling these before
 * skill_radio_init() has run (e.g. the token write on first boot) does no harm.
 */
void radio_encoder_pause() {
    detachInterrupt(digitalPinToInterrupt(PIN_ENC_A));
    detachInterrupt(digitalPinToInterrupt(PIN_ENC_B));
}

void radio_encoder_resume() {
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), rotary_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), rotary_isr, CHANGE);
}

/*
 * The handheld input router's mode: what the encoder currently adjusts. A click
 * on the push button (tracked in radio_tick) toggles between tuning frequency
 * and setting volume — the base handheld UX. A long-press opens the modal menu
 * (UI_MENU). UI_VFO/UI_VOLUME keep the old 0/1 values so the display's
 * active-target highlight (which reads radio_get_tune_target()) needs no change;
 * UI_MENU is appended last so those two are untouched.
 */
enum UiMode { UI_VFO, UI_VOLUME, UI_MENU };
static uint8_t ui_mode = UI_VFO;

/*
 * Modal menu state. Long-press drops the router into UI_MENU; the encoder then
 * scrolls a small on-screen list, a click selects, and inactivity/back returns to
 * UI_VFO. Three levels: a top MAIN list, a SETTINGS sublist and the BAND picker.
 * MAIN and SETTINGS each remember their own cursor so a round-trip through
 * Settings lands back on the same MAIN row; the BAND cursor is (re)seeded from the
 * active band on every entry so the list opens on the band you are on. "Band" now
 * drives real selection (via radio_select_band); "Settings" opens the sublist;
 * Mode/Step/Bandwidth stay placeholders that bounce back to the VFO until later
 * tickets wire real parameter editing onto them.
 */
enum MenuLevel { MENU_MAIN, MENU_SETTINGS, MENU_BAND, MENU_ADJUST };
static uint8_t menu_level = MENU_MAIN;
static int     menu_idx = 0;      /* cursor in the MAIN list */
static int     menu_settings_idx = 0; /* cursor in the SETTINGS sublist */

/*
 * MENU_ADJUST is a value-editing leaf reached from the MAIN "Step"/"Bandwidth"
 * rows. There is no separate cursor: the encoder scrolls the value itself (the
 * active band's band_step_idx/band_bw_idx, or its band_mode), which the render
 * accessors expose so the same draw_menu_screen window shows the choices.
 * adjust_target says which of the four we are editing. ADJ_MODE is reachable only
 * on a non-FM band (mode is locked on VHF). ADJ_SQUELCH is a numeric 0..127 gate
 * threshold — clamped at the ends, not a table wrap like the others. ADJ_AGC/AVC/
 * SOFTMUTE are likewise numeric (cursor IS the value): AGC on every mode, AVC and
 * SoftMute on AM/SSB only (a no-op editor showing blanks on an FM band). */
enum AdjustTarget { ADJ_STEP, ADJ_BW, ADJ_MODE, ADJ_SQUELCH, ADJ_AGC, ADJ_AVC, ADJ_SOFTMUTE };
static uint8_t adjust_target = ADJ_STEP;
/* Cursor in the BAND picker (indexes bands[]). Re-seeded from radio_get_band_idx()
 * each time MENU_BAND is entered, so the list always opens on the active band. */
static uint8_t menu_band_idx = 0;

/*
 * MAIN-list dispatch keys. The order MUST match menu_main_items[] below so the
 * click handler can switch on the raw cursor (menu_idx) instead of strcmp-ing the
 * label — the label is display text, the index is the contract.
 */
enum MainItem { MI_BAND = 0, MI_MODE, MI_STEP, MI_BW, MI_SQUELCH, MI_MUTE, MI_SETTINGS };

/*
 * SETTINGS-sublist dispatch keys. The order MUST match menu_settings_items[] below,
 * exactly like MainItem/menu_main_items[]: the click handler switches on the raw
 * cursor (menu_settings_idx), never the label text. AGC/AVC/SoftMute drop into a
 * numeric adjust editor; Brightness/Theme/About are not leaves yet; Back exits.
 */
enum SettingsItem { SI_AGC = 0, SI_AVC, SI_SOFTMUTE, SI_BRIGHTNESS, SI_THEME, SI_ABOUT, SI_BACK };

static const char *const menu_main_items[] = {
    "Band", "Mode", "Step", "Bandwidth", "Squelch", "Mute", "Settings"
};
static const char *const menu_settings_items[] = {
    "AGC", "AVC", "SoftMute", "Brightness", "Theme", "About", "Back"
};
#define MENU_MAIN_COUNT     ((int)(sizeof(menu_main_items) / sizeof(menu_main_items[0])))
#define MENU_SETTINGS_COUNT ((int)(sizeof(menu_settings_items) / sizeof(menu_settings_items[0])))

/*
 * Circular menu-cursor step. Adds delta (which may be several detents or negative)
 * and wraps modulo count so scrolling loops top-to-bottom instead of clamping. The
 * double-mod keeps the result non-negative for a downward wrap past zero.
 */
static int menu_wrap(int idx, int delta, int count) {
    if (count <= 0) return 0;
    return (((idx + delta) % count) + count) % count;
}

/* --- Endpoints --- */
static const SkillEndpoint radio_endpoints[] = {
    {"POST", "/radio/tune",   "Tune: {mode:\"FM\"|\"AM\"|\"LSB\"|\"USB\", freq:<int>, bfo?:<int>}"},
    {"POST", "/radio/band",   "Jump to a band-plan preset: {idx:<int>}"},
    {"POST", "/radio/config", "Set mode/step/bandwidth/squelch/mute/DSP: {mode?:\"AM\"|\"LSB\"|\"USB\", step_idx?:<int>, bw_idx?:<int>, squelch?:0-127, squelch_snr?:<bool>, mute?:<bool>, agc?:<int>, avc?:<even 12-90>, softmute?:0-32}"},
    {"POST", "/radio/volume", "Set volume: {volume:0-63}"},
    {"POST", "/radio/scan",   "Sweep current-mode band: {from,to,step,min_rssi?} -> RSSI/SNR per step"},
    {"GET",  "/radio/status", "Current freq/mode/RSSI/SNR (+bfo in SSB)"},
    {NULL, NULL, NULL}
};

static const char *radio_describe() {
    return "## Skill: radio\n\n"
           "SI4732 receiver — FM, AM and SSB (LSB/USB) tuning over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /radio/tune | Tune: `{\"mode\":\"FM|AM|LSB|USB\",\"freq\":<int>,\"bfo\":<int>}` |\n"
           "| POST | /radio/band | Jump to a band-plan preset: `{\"idx\":<int>}` |\n"
           "| POST | /radio/config | Set mode/step/bandwidth + squelch/mute + DSP: `{\"mode\":\"AM|LSB|USB\",\"step_idx\":<int>,\"bw_idx\":<int>,\"squelch\":<0..127>,\"squelch_snr\":<bool>,\"mute\":<bool>,\"agc\":<int>,\"avc\":<even 12..90>,\"softmute\":<0..32>}` |\n"
           "| POST | /radio/volume | Set volume: `{\"volume\":<0..63>}` |\n"
           "| POST | /radio/scan | Blocking band sweep in the current mode: `{\"from\":<int>,\"to\":<int>,\"step\":<int>,\"min_rssi\":<int>}` |\n"
           "| GET | /radio/status | Current freq, mode, RSSI, SNR (+bfo in SSB) |\n\n"
           "### Frequency units\n\n"
           "- FM: 10 kHz steps, so `10000` = 100.0 MHz (range 6400..10800)\n"
           "- AM: kHz, so `1000` = 1000 kHz (range 520..1710)\n"
           "- LSB/USB: kHz, so `14074` = 14074 kHz (range 1800..30000)\n\n"
           "### SSB fine-tuning\n\n"
           "- `bfo` (optional, SSB only): BFO offset in Hz, range -14000..14000, default 0.\n"
           "  Sending `bfo` in FM or AM is a 400 — it only applies in LSB/USB.\n\n"
           "### Band presets\n\n"
           "- `POST /radio/band` `{\"idx\":<int>}` jumps to a preset from the band plan\n"
           "  (FM VHF, MW, the SW broadcast segments and the amateur SSB bands). It\n"
           "  sets the mode, band edges and tuning step for you and restores the last\n"
           "  frequency used on that band. `idx` runs `0`..(band count - 1); an\n"
           "  out-of-range index is a 400. Response: `{\"ok\",\"band\",\"mode\",\"freq\",\"freq_display\"}`.\n\n"
           "### Volume\n\n"
           "- `POST /radio/volume` `{\"volume\":<0..63>}` sets the receiver volume\n"
           "  (same value reported by `/radio/status` and driven by the encoder).\n\n"
           "### DSP tuning (AGC / AVC / SoftMute)\n\n"
           "Set per-mode via `POST /radio/config`; each is stored per mode family and\n"
           "reported by `/radio/status` for the active mode.\n\n"
           "- `agc` (all modes): 0 = AGC on (no attenuation); higher = manual attenuator.\n"
           "  Range is per mode: FM 0..27, AM 0..37, SSB 0..1.\n"
           "- `avc` (AM/SSB only): AVC max gain, even values 12..90. Rejected in FM.\n"
           "- `softmute` (AM/SSB only): soft-mute max attenuation 0..32. Rejected in FM.\n\n"
           "### Scan\n\n"
           "`POST /radio/scan` sweeps `from`..`to` (inclusive) with the given `step`,\n"
           "measuring RSSI/SNR at each point. It stays in the current mode and does\n"
           "not change it; `from`/`to`/`step` use that mode's frequency units and\n"
           "must fall inside its band limits. The sweep is blocking (~33 ms/step) and\n"
           "restores the prior frequency when done.\n\n"
           "- `from`, `to`, `step` (required): `step > 0`, `from < to`\n"
           "- `min_rssi` (optional, default 0): only points with `rssi >= min_rssi` are returned\n"
           "- Step ceiling: `(to - from) / step + 1` must be <= 64 (a sweep runs under\n"
           "  the 3 s request ACK timeout); use a coarser step or paginate otherwise\n"
           "- Response: `{\"mode\",\"from\",\"to\",\"step\",\"count\",\"points\":[{\"freq\",\"rssi\",\"snr\"}]}`\n\n"
           "### Examples\n\n"
           "```\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"FM\",\"freq\":10000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"AM\",\"freq\":1000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"USB\",\"freq\":14074}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"LSB\",\"freq\":3750,\"bfo\":500}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' http://<host>:8080/radio/status\n"
           "# Sweep the FM broadcast band (in the current FM mode) in 200 kHz steps\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"from\":8700,\"to\":9000,\"step\":20,\"min_rssi\":20}' http://<host>:8080/radio/scan\n"
           "```\n\n"
           "The current mode/freq/volume/bfo persist across a reboot (saved to flash).\n\n"
           "Band presets, live background scan and RDS are not driven yet.\n";
}

/* Return true when the mode is one of the two SSB sidebands. */
static bool radio_is_ssb(uint8_t mode) {
    return mode == RADIO_MODE_LSB || mode == RADIO_MODE_USB;
}

/* Per-mode AGC index ceiling (ref doAgc): FM 0..27, AM 0..37, SSB 0..1. The index
 * is an attenuator step, 0 meaning "AGC on, no attenuation". */
static uint8_t agc_max_for_mode(uint8_t mode) {
    if (mode == RADIO_MODE_FM) return 27;
    if (radio_is_ssb(mode))    return 1;
    return 37;  /* AM */
}

/* Map a mode constant to its wire string (FM|LSB|USB|AM). */
static const char *radio_mode_str(uint8_t mode) {
    switch (mode) {
        case RADIO_MODE_FM:  return "FM";
        case RADIO_MODE_LSB: return "LSB";
        case RADIO_MODE_USB: return "USB";
        default:             return "AM";
    }
}

/* Build a human-readable frequency string for the current mode. */
static void radio_format_freq(uint16_t freq, uint8_t mode, char *out, size_t len) {
    if (mode == RADIO_MODE_FM) {
        snprintf(out, len, "%.2f MHz", freq / 100.0f);
    } else if (radio_is_ssb(mode)) {
        snprintf(out, len, "%u kHz %s", (unsigned)freq, radio_mode_str(mode));
    } else {
        snprintf(out, len, "%u kHz", (unsigned)freq);
    }
}

/* --- State accessors for the display skill (avoids reaching into rx/state) ---
 * Called from both the HTTP task and loop()'s radio_tick(), so the ones that
 * touch the receiver take radio_mtx around the I2C read. The plain scalar reads
 * (freq/mode/volume/target) are single aligned words and need no lock. */
uint16_t radio_get_freq() { return radio_freq; }

const char *radio_get_mode_str() { return radio_mode_str(radio_mode); }

uint8_t radio_get_volume() { return radio_volume; }

/* Kept named radio_get_tune_target() (returning the UI_VFO/UI_VOLUME value) so
 * display.cpp's active-target highlight needs no change. */
uint8_t radio_get_tune_target() { return ui_mode; }

/* Menu accessors for the display skill. All plain aligned-word reads off the same
 * loop-task thread that mutates them (radio_tick), so no lock is needed. */
uint8_t radio_get_ui_mode() { return ui_mode; }
uint8_t radio_get_menu_level() { return menu_level; }

/* Cursor of whichever level is active. Generalising this to the active cursor
 * (MAIN / SETTINGS / BAND) is what lets the display's change-detection notice a
 * band-list scroll: prev_menu_idx tracks this value, so paging bands forces a
 * repaint without any extra prev_* field. */
int radio_get_menu_idx() {
    switch (menu_level) {
        case MENU_SETTINGS: return menu_settings_idx;
        case MENU_BAND:     return menu_band_idx;
        case MENU_ADJUST: {
            /* Index the band's own table mode, and clamp so a cursor read never
             * exceeds the current table (belt-and-suspenders). */
            uint8_t m = band_table_mode();
            if (adjust_target == ADJ_STEP)
                return clamp_idx(band_step_idx[radio_band_idx], stepCount(m));
            if (adjust_target == ADJ_BW)
                return clamp_idx(band_bw_idx[radio_band_idx], bwCount(m));
            /* ADJ_SQUELCH: cursor IS the numeric threshold (0..127), low 7 bits. */
            if (adjust_target == ADJ_SQUELCH)
                return radio_squelch & 0x7f;
            /* ADJ_AGC/AVC/SOFTMUTE: cursor IS the per-mode value. On an FM band AVC/
             * SoftMute are inapplicable — the value is still returned but item()
             * blanks every row, so the editor reads as empty. */
            if (adjust_target == ADJ_AGC)
                return (m == RADIO_MODE_FM) ? fm_agc : (radio_is_ssb(m) ? ssb_agc : am_agc);
            if (adjust_target == ADJ_AVC)
                return radio_is_ssb(m) ? ssb_avc : am_avc;
            if (adjust_target == ADJ_SOFTMUTE)
                return radio_is_ssb(m) ? ssb_sm : am_sm;
            /* ADJ_MODE: cursor is the band's mode position within modeCycle. */
            return mode_cycle_pos(band_mode[radio_band_idx]);
        }
        default:            return menu_idx;
    }
}
/* Length of the active level's list. */
int radio_get_menu_count() {
    switch (menu_level) {
        case MENU_SETTINGS: return MENU_SETTINGS_COUNT;
        case MENU_BAND:     return BAND_COUNT;
        case MENU_ADJUST:
            if (adjust_target == ADJ_STEP) return stepCount(band_table_mode());
            if (adjust_target == ADJ_BW)   return bwCount(band_table_mode());
            if (adjust_target == ADJ_SQUELCH) return 128;  /* 0..127 threshold */
            if (adjust_target == ADJ_AGC) return agc_max_for_mode(band_table_mode()) + 1;
            if (adjust_target == ADJ_AVC) return 91;   /* 0..90 window; even-only in item()/scroll */
            if (adjust_target == ADJ_SOFTMUTE) return 33;  /* 0..32 */
            return MODE_CYCLE_COUNT;
        default:            return MENU_MAIN_COUNT;
    }
}

/* Title of the active level, and its item text at an arbitrary index. The index
 * is wrapped modulo the count so the caller can ask for idx-2..idx+2 (the visible
 * window) without bounds-checking each row itself. */
const char *radio_get_menu_title() {
    switch (menu_level) {
        case MENU_SETTINGS: return "Settings";
        case MENU_BAND:     return "Band";
        case MENU_ADJUST:
            if (adjust_target == ADJ_STEP) return "Step";
            if (adjust_target == ADJ_BW)   return "Bandwidth";
            if (adjust_target == ADJ_SQUELCH) return "Squelch";
            if (adjust_target == ADJ_AGC) return "AGC";
            if (adjust_target == ADJ_AVC) return "AVC";
            if (adjust_target == ADJ_SOFTMUTE) return "SoftMute";
            return "Mode";
        default:            return "Menu";
    }
}
const char *radio_get_menu_item(int i) {
    if (menu_level == MENU_ADJUST) {
        if (adjust_target == ADJ_MODE) {
            int n = MODE_CYCLE_COUNT;
            return radio_mode_str(modeCycle[((i % n) + n) % n]);
        }
        if (adjust_target == ADJ_SQUELCH) {
            /* Numeric threshold window. `i` is the absolute candidate value
             * (draw_menu_screen passes cursor+offset, cursor = the threshold),
             * so render it directly, blanking the edges past the 0..127 clamp
             * instead of wrapping. draw_menu_screen consumes each returned string
             * immediately before asking for the next, so one static buffer is safe. */
            static char buf[8];
            if (i < 0 || i > 127) return "";
            snprintf(buf, sizeof(buf), "%d", i);
            return buf;
        }
        if (adjust_target == ADJ_AGC) {
            /* Numeric AGC index window; blank past the per-mode ceiling. */
            static char buf[8];
            if (i < 0 || i > (int)agc_max_for_mode(band_table_mode())) return "";
            snprintf(buf, sizeof(buf), "%d", i);
            return buf;
        }
        if (adjust_target == ADJ_AVC) {
            /* Even values 12..90; blank odd neighbours and the FM band (no AVC there). */
            static char buf[8];
            if (band_table_mode() == RADIO_MODE_FM || i < 12 || i > 90 || (i % 2)) return "";
            snprintf(buf, sizeof(buf), "%d", i);
            return buf;
        }
        if (adjust_target == ADJ_SOFTMUTE) {
            /* Values 0..32; blank past the ends and the FM band (no soft-mute there). */
            static char buf[8];
            if (band_table_mode() == RADIO_MODE_FM || i < 0 || i > 32) return "";
            snprintf(buf, sizeof(buf), "%d", i);
            return buf;
        }
        /* Menu window items come from the band's own table mode, not radio_mode. */
        uint8_t m = band_table_mode();
        if (adjust_target == ADJ_STEP) {
            int n = stepCount(m);
            return stepsForMode(m)[((i % n) + n) % n].desc;
        }
        int n = bwCount(m);
        return bwForMode(m)[((i % n) + n) % n].desc;
    }
    if (menu_level == MENU_BAND) {
        int n = BAND_COUNT;
        return bands[((i % n) + n) % n].name;
    }
    if (menu_level == MENU_SETTINGS) {
        int n = MENU_SETTINGS_COUNT;
        return menu_settings_items[((i % n) + n) % n];
    }
    int n = MENU_MAIN_COUNT;
    return menu_main_items[((i % n) + n) % n];
}

uint8_t radio_get_rssi() {
    if (!radio_ok) return 0;
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    rx.getCurrentReceivedSignalQuality();
    uint8_t rssi = rx.getCurrentRSSI();
    xSemaphoreGive(radio_mtx);
    return rssi;
}

/* RSSI and SNR from one signal-quality read under a single mutex hold, so the
 * display can show both without taking the receiver twice. */
void radio_get_signal(uint8_t *rssi, uint8_t *snr) {
    if (!radio_ok) {
        if (rssi) *rssi = 0;
        if (snr)  *snr  = 0;
        return;
    }
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    rx.getCurrentReceivedSignalQuality();
    if (rssi) *rssi = rx.getCurrentRSSI();
    if (snr)  *snr  = rx.getCurrentSNR();
    xSemaphoreGive(radio_mtx);
}

/*
 * Layered audio mute. Three request layers — MAIN (the user/HTTP mute toggle),
 * SQUELCH (the signal gate) and TEMP (a one-shot transient mute) — are OR-ed: the
 * audio is muted whenever ANY layer wants it, a pared-down take on the reference
 * firmware's mute bitmask. Only the DSP soft-mute (rx.setAudioMute) is driven —
 * deliberately never PIN_AMP_EN (the class-D amp) nor PIN_AUDIO_MUTE (already owned
 * by the SI4735 driver via setAudioMuteMcuPin). No delay() lives on this path.
 *
 * Caller MUST hold radio_mtx (same contract as apply_*_locked): this touches the
 * receiver and the shared layer flags. MAIN/SQUELCH update their sticky flag; TEMP
 * is a one-shot request with no stored flag. The chip is toggled ONLY on a real
 * edge (audio_muted != want) so the 250 ms squelch poll cannot spam I2C. The
 * library signature is setAudioMute(bool off): off=true mutes, off=false unmutes,
 * so `want` maps straight through.
 */
static void radio_set_mute(uint8_t layer, bool on) {
    if (layer == MUTE_MAIN)         mute_main = on;
    else if (layer == MUTE_SQUELCH) mute_squelch = on;
    bool temp = (layer == MUTE_TEMP) ? on : false;
    bool want = mute_main || mute_squelch || temp;
    if (audio_muted != want) {
        rx.setAudioMute(want);  /* off=true -> mute */
        audio_muted = want;
    }
}

/*
 * Query a mute layer: MAIN/SQUELCH report their sticky flag, anything else (incl.
 * the overall query) reports the physical soft-mute state. Plain aligned-bool reads;
 * callers that need coherence with a concurrent write hold radio_mtx anyway.
 */
static bool radio_is_muted(uint8_t layer) {
    if (layer == MUTE_MAIN)    return mute_main;
    if (layer == MUTE_SQUELCH) return mute_squelch;
    return audio_muted;
}

/*
 * Apply a step-table row to the chip's currentStep, on which frequencyUp/Down act.
 * SSB is intentionally skipped in this cut: SSB fine-tuning is done through the BFO
 * (out of scope here), and setSSB() was called with step 0 so encoder tuning stays
 * a no-op there. The row index is stored per band regardless; it just does not
 * reach the chip while the band is in SSB. Caller must hold radio_mtx.
 */
static void apply_step_locked(uint8_t mode, uint8_t sidx) {
    if (radio_is_ssb(mode)) return;
    rx.setFrequencyStep(stepsForMode(mode)[sidx].step);
}

/*
 * Apply a bandwidth-table row to the chip. Three different SI4735 calls depending
 * on the mode family, each fed the row's hardware filter index (.hwIdx, not the row
 * position). For SSB the sideband cutoff filter is disabled for the narrow voice
 * widths (hwIdx 0/4/5) and enabled otherwise, mirroring the reference firmware.
 * Caller must hold radio_mtx.
 */
static void apply_bandwidth_locked(uint8_t mode, uint8_t bidx) {
    uint8_t hw = bwForMode(mode)[bidx].hwIdx;
    if (mode == RADIO_MODE_FM) {
        rx.setFmBandwidth(hw);
    } else if (radio_is_ssb(mode)) {
        rx.setSSBAudioBandwidth(hw);
        rx.setSSBSidebandCutoffFilter((hw == 0 || hw == 4 || hw == 5) ? 0 : 1);
    } else {
        rx.setBandwidth(hw, 1);
    }
}

/*
 * Push the mode's stored AGC index to the chip. Ref doAgc: index 0 => AGC on with no
 * attenuation (AGCDIS=0, AGCIDX=0); index n>0 => AGC disabled with attenuator n-1.
 * One setAutomaticGainControl call covers every mode — the driver routes it to the FM
 * or AM AGC property by the current opmode. Caller must hold radio_mtx. A default
 * index of 0 reproduces the receiver's power-on AGC exactly (so replacing the old
 * hardcoded setAutomaticGainControl(0,0) is behaviour-preserving).
 */
static void apply_agc_locked(uint8_t mode) {
    int8_t idx = (mode == RADIO_MODE_FM) ? fm_agc
               : (radio_is_ssb(mode) ? ssb_agc : am_agc);
    uint8_t dis = idx > 0 ? 1 : 0;
    uint8_t ndx = idx > 1 ? (uint8_t)(idx - 1) : 0;
    rx.setAutomaticGainControl(dis, ndx);
}

/*
 * Push the mode's stored AVC (automatic volume control) max gain to the chip. AVC is
 * an AM/SSB feature — FM has none, so this is a no-op there. The SI4735 driver uses
 * the AM property for SSB too (ref calls setAvcAmMaxGain in both). Caller holds
 * radio_mtx. SSB AVC enablement (setSSBAutomaticVolumeControl) stays in the SSB
 * set-up branch; this only sets the max-gain ceiling.
 */
static void apply_avc_locked(uint8_t mode) {
    if (mode == RADIO_MODE_FM) return;
    rx.setAvcAmMaxGain((uint8_t)(radio_is_ssb(mode) ? ssb_avc : am_avc));
}

/*
 * Push the mode's stored soft-mute max attenuation to the chip. AM and SSB have
 * separate SI4735 properties; FM soft-mute is left alone (no-op). Caller holds
 * radio_mtx.
 */
static void apply_softmute_locked(uint8_t mode) {
    if (mode == RADIO_MODE_FM) return;
    if (radio_is_ssb(mode)) rx.setSsbSoftMuteMaxAttenuation((uint8_t)ssb_sm);
    else                    rx.setAmSoftMuteMaxAttenuation((uint8_t)am_sm);
}

/*
 * Drive the receiver onto band `idx` at frequency `freq` (already clipped to the
 * band's window, in that band's units). Shared by radio_select_band() and the
 * boot restore so the chip-config sequence lives in one place.
 *
 * The caller must hold radio_mtx (or be single-tasked at boot) — this touches the
 * SI4735 directly and does not lock. It applies the band's own min/max/step (not
 * the fixed RADIO_*_MIN/MAX that manual /radio/tune uses), lazily uploads/clears
 * the SSB patch through the shared radio_ssb_loaded flag, resets the BFO to 0 on
 * every band change, and reapplies the current volume (a mode switch can reset the
 * chip's volume). It updates radio_band_idx/radio_freq/radio_mode to match; it does
 * NOT mark the state dirty — that is the caller's call.
 */
static void apply_band_locked(uint8_t idx, uint16_t freq) {
    const RadioBand *b = &bands[idx];
    /* Live demod mode (band_mode[], not b->mode): a mode switch must survive reapply.
     * Band edges/type/defFreq still come from bands[idx] — only the demod changes. */
    uint8_t mode = band_mode[idx];
    /* The band's selected step/bandwidth rows (seeded from the mode defaults, or
     * changed via the adjust menu / POST /radio/config). Clamp defensively to this
     * band mode's table so a stale/broken index can never index out of bounds. */
    uint8_t sidx = clamp_idx(band_step_idx[idx], stepCount(mode));
    uint8_t bidx = clamp_idx(band_bw_idx[idx], bwCount(mode));

    if (radio_is_ssb(mode)) {
        /* Lazily upload the SSB patch (audiobw=1 -> 2.2 kHz) on first SSB use. */
        if (!radio_ssb_loaded) {
            rx.loadPatch(ssb_patch_content, sizeof(ssb_patch_content), 1);
            radio_ssb_loaded = true;
        }
        /* mode value doubles as usblsb (1=LSB, 2=USB); step 0 in SSB (BFO tunes). */
        rx.setSSB(b->minFreq, b->maxFreq, freq, 0, mode);
        rx.setSSBAutomaticVolumeControl(1);
        radio_bfo = 0;
        rx.setSSBBfo(0);
    } else if (mode == RADIO_MODE_FM) {
        rx.setFM(b->minFreq, b->maxFreq, freq, stepsForMode(mode)[sidx].step);
        /* Match /radio/tune's post-setFM config: 50 us de-emphasis. The FM AGC is
         * (re)applied from the per-mode store in the unified DSP block below. */
        rx.setFMDeEmphasis(1);
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
    } else {  /* AM (MW / SW broadcast) */
        rx.setAM(b->minFreq, b->maxFreq, freq, stepsForMode(mode)[sidx].step);
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
    }
    /* A mode change can reset the chip volume to its power-on default AND clear the
     * DSP soft-mute. Skip pushing volume to the chip while MAIN-muted; the value is
     * kept and re-pushed on unmute (deferring the write avoids a needless RX_VOLUME
     * round-trip, not because it would unmute — mute is RX_HARD_MUTE via setAudioMute,
     * independent of RX_VOLUME). Always reassert the physical mute — setFM/setAM/setSSB
     * drop the soft-mute, so it must be reapplied to keep the layers honoured. */
    if (!mute_main) rx.setVolume(radio_volume);
    rx.setAudioMute(audio_muted);

    /* Apply the band's channel bandwidth (and, for non-SSB, its tuning step) on top
     * of the mode set-call. setFM/setAM already loaded the step above; this keeps
     * currentStep aligned for AM/FM and is a no-op for SSB. */
    apply_step_locked(mode, sidx);
    apply_bandwidth_locked(mode, bidx);

    /* Reapply the per-mode DSP tuning: setFM/setAM/setSSB reset AGC/AVC/soft-mute to
     * chip defaults, so — like volume/mute above — push the stored values back on top.
     * AGC covers every mode (index 0 reproduces the old FM default); AVC/soft-mute are
     * no-ops on FM. */
    apply_agc_locked(mode);
    apply_avc_locked(mode);
    apply_softmute_locked(mode);

    radio_band_idx = idx;
    radio_freq = freq;
    radio_mode = mode;
}

/*
 * Select band `idx` from the plan and retune to that band's remembered frequency.
 * Takes radio_mtx around the whole receiver access. Saves the current frequency
 * back into the outgoing band's slot first, then clips the incoming band's saved
 * frequency to its window and applies it. Returns the band name, or nullptr if idx
 * is out of range (caller validates before calling, so this is defence-in-depth).
 */
static const char *radio_select_band(uint8_t idx) {
    if (idx >= BAND_COUNT) return nullptr;

    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    /* Remember where we were on the band we are leaving. */
    band_freq[radio_band_idx] = radio_freq;
    /* Restore the incoming band's last frequency, clipped to its edges. */
    uint16_t f = band_freq[idx];
    if (f < bands[idx].minFreq) f = bands[idx].minFreq;
    if (f > bands[idx].maxFreq) f = bands[idx].maxFreq;
    apply_band_locked(idx, f);
    radio_mark_dirty();  /* real band change -> schedule a debounced persist */
    xSemaphoreGive(radio_mtx);

    return bands[idx].name;
}

/*
 * Cycle the active band's demod mode one detent in `dir` (+1/-1) through {AM,LSB,USB}
 * — never FM — then reapply the band so the new demod takes effect. Ported from the
 * ref's doMode. A native-FM band (VHF broadcast) is mode-locked: FM is a broadcast
 * demod that does not belong on MW/SW/ham, so this is a no-op there.
 *
 * Switching mode resets the band's step/bandwidth cursors to the new mode's defaults
 * (the old row indexes a different table and would be out of range) and zeroes the
 * BFO, then apply_band_locked handles the SSB patch load/unload and the setAM/setSSB
 * set-call. The current frequency is kept. Takes radio_mtx and marks the state dirty;
 * a no-op returns false, a real switch true.
 */
static bool radio_cycle_mode(int8_t dir) {
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    uint8_t bnd = radio_band_idx;
    if (bands[bnd].mode == RADIO_MODE_FM) {  /* native-FM band -> mode locked */
        xSemaphoreGive(radio_mtx);
        return false;
    }
    uint8_t pos = mode_cycle_pos(band_mode[bnd]);
    pos = (uint8_t)menu_wrap(pos, dir, MODE_CYCLE_COUNT);
    uint8_t newmode = modeCycle[pos];
    band_mode[bnd] = newmode;
    /* The old step/bw indices belong to the old mode's tables; reset to the new
     * mode's defaults before apply_band_locked reads them. */
    band_step_idx[bnd] = defaultStepIdx[newmode];
    band_bw_idx[bnd]   = defaultBwIdx[newmode];
    radio_bfo = 0;
    apply_band_locked(bnd, radio_freq);
    radio_mark_dirty();
    xSemaphoreGive(radio_mtx);
    return true;
}

/* --- Band accessors for the menu/display skill (plain aligned reads, no lock) --- */
uint8_t radio_get_band_idx() { return radio_band_idx; }
const char *radio_get_band_name() { return bands[radio_band_idx].name; }
uint8_t radio_get_band_count() { return BAND_COUNT; }
const char *radio_get_band_name_at(uint8_t i) {
    return (i < BAND_COUNT) ? bands[i].name : "";
}

static void radio_register_routes(AsyncWebServer &server) {

    /* POST /radio/tune */
    server.on("/radio/tune", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        const char *mode_str = input["mode"] | (const char*)nullptr;
        int freq = input["freq"] | -1;
        int req_bfo = input["bfo"] | 0;
        /* bfo is meaningful only in SSB; flag its presence so the FM/AM branches
         * can reject it instead of silently swallowing an offset that never applies. */
        bool has_bfo = !input["bfo"].isNull();

        free(body); req->_tempObject = nullptr;

        if (!mode_str) {
            req->send(400, "application/json", "{\"error\":\"mode required (FM|AM|LSB|USB)\"}");
            return;
        }
        if (freq < 0) {
            req->send(400, "application/json", "{\"error\":\"freq required\"}");
            return;
        }

        uint8_t mode;
        if (strcmp(mode_str, "FM") == 0) {
            mode = RADIO_MODE_FM;
            if (has_bfo) {
                req->send(400, "application/json",
                    "{\"error\":\"bfo is only valid in SSB (LSB/USB)\"}");
                return;
            }
            if (freq < RADIO_FM_MIN || freq > RADIO_FM_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"FM freq out of range (6400..10800)\"}");
                return;
            }
            xSemaphoreTake(radio_mtx, portMAX_DELAY);
            rx.setFM(RADIO_FM_MIN, RADIO_FM_MAX, (uint16_t)freq, 10);
            /* Match the reference firmware's post-setFM config so the chip
             * reports FM RSSI/SNR. setFM alone leaves de-emphasis and AGC at
             * power-on defaults; the SI4735 driver applies these as properties.
             * De-emphasis 1 = 50 us (EU/JP/AU); the FM AGC comes from the per-mode
             * store via apply_agc_locked (default index 0 = AGC on, no attenuation,
             * matching the ref's doAgc(0)). */
            rx.setFMDeEmphasis(1);
            apply_agc_locked(RADIO_MODE_FM);
            xSemaphoreGive(radio_mtx);
            radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        } else if (strcmp(mode_str, "AM") == 0) {
            mode = RADIO_MODE_AM;
            if (has_bfo) {
                req->send(400, "application/json",
                    "{\"error\":\"bfo is only valid in SSB (LSB/USB)\"}");
                return;
            }
            if (freq < RADIO_AM_MIN || freq > RADIO_AM_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"AM freq out of range (520..1710)\"}");
                return;
            }
            xSemaphoreTake(radio_mtx, portMAX_DELAY);
            rx.setAM(RADIO_AM_MIN, RADIO_AM_MAX, (uint16_t)freq, 10);
            xSemaphoreGive(radio_mtx);
            radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        } else if (strcmp(mode_str, "LSB") == 0 || strcmp(mode_str, "USB") == 0) {
            mode = (strcmp(mode_str, "LSB") == 0) ? RADIO_MODE_LSB : RADIO_MODE_USB;
            if (freq < RADIO_SSB_MIN || freq > RADIO_SSB_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"SSB freq out of range (1800..30000)\"}");
                return;
            }
            if (req_bfo < -RADIO_BFO_MAX || req_bfo > RADIO_BFO_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"bfo out of range (-14000..14000)\"}");
                return;
            }
            xSemaphoreTake(radio_mtx, portMAX_DELAY);
            /* Lazily upload the SSB patch (audiobw=1 -> 2.2 kHz) on first SSB use. */
            if (!radio_ssb_loaded) {
                rx.loadPatch(ssb_patch_content, sizeof(ssb_patch_content), 1);
                radio_ssb_loaded = true;
            }
            /* step=0 in SSB; mode value doubles as usblsb (1=LSB, 2=USB). */
            rx.setSSB(RADIO_SSB_MIN, RADIO_SSB_MAX, (uint16_t)freq, 0, mode);
            rx.setSSBAutomaticVolumeControl(1);
            radio_bfo = req_bfo;
            rx.setSSBBfo(-radio_bfo);  /* sign follows the ref convention */
            xSemaphoreGive(radio_mtx);
        } else {
            req->send(400, "application/json", "{\"error\":\"mode must be FM, AM, LSB or USB\"}");
            return;
        }

        /* These two are also written from radio_tick(); keep every writer under
         * the same mutex so a concurrent encoder step can't interleave. */
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        radio_mode = mode;
        radio_freq = (uint16_t)freq;
        radio_mark_dirty();  /* real tune -> schedule a debounced persist */
        xSemaphoreGive(radio_mtx);

        event_add("radio: tuned %s %d", mode_str, freq);

        /* Reflect the new tune on the status screen (defined in display.cpp,
         * forward-declared in main.cpp before this file is included). */
        display_show_status();

        JsonDocument doc;
        doc["ok"] = true;
        doc["mode"] = mode_str;
        doc["freq"] = radio_freq;
        if (radio_is_ssb(mode)) doc["bfo"] = radio_bfo;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /radio/band — jump to a preset from the band plan. Mirrors /radio/tune's
     * auth + JSON handling, but the mode/edges/step all come from bands[idx]; the
     * receiver work is done under radio_mtx inside radio_select_band(). */
    server.on("/radio/band", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        int idx = input["idx"] | -1;

        free(body); req->_tempObject = nullptr;

        if (idx < 0 || idx >= BAND_COUNT) {
            char err[80];
            snprintf(err, sizeof(err),
                "{\"error\":\"band idx out of range (0..%d)\"}", BAND_COUNT - 1);
            req->send(400, "application/json", err);
            return;
        }

        const char *name = radio_select_band((uint8_t)idx);

        event_add("radio: band %d (%s)", idx, name);
        display_show_status();

        char freq_display[24];
        radio_format_freq(radio_freq, radio_mode, freq_display, sizeof(freq_display));

        JsonDocument doc;
        doc["ok"] = true;
        doc["band"] = name;
        doc["mode"] = radio_mode_str(radio_mode);
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /radio/config — set the current band's demod mode and/or tuning step
     * and/or channel bandwidth. step_idx/bw_idx are indices into the (effective)
     * mode's step/bandwidth table, matching the desc strings reported by
     * /radio/status; mode is "AM"|"LSB"|"USB" (FM is locked to the VHF band). Mirrors
     * /radio/band's auth + JSON handling; the receiver work runs under radio_mtx. All
     * three fields are optional, but at least one must be present. */
    server.on("/radio/config", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        bool has_step = !input["step_idx"].isNull();
        bool has_bw   = !input["bw_idx"].isNull();
        bool has_mode = !input["mode"].isNull();
        int step_idx = input["step_idx"] | -1;
        int bw_idx   = input["bw_idx"]   | -1;
        const char *mode_str = input["mode"] | (const char*)nullptr;

        /* Optional squelch gate and MAIN mute toggle. "squelch" is the 0..127
         * threshold (0 = off); "squelch_snr" optionally flips the metric (default
         * keeps the current one); "mute" drives the MUTE_MAIN layer. */
        bool has_squelch  = !input["squelch"].isNull();
        bool has_sqmetric = !input["squelch_snr"].isNull();
        bool has_mute     = !input["mute"].isNull();
        int  squelch      = input["squelch"]     | -1;
        bool sq_snr       = input["squelch_snr"] | false;
        bool mute_on      = input["mute"]        | false;

        /* Optional per-mode DSP: "agc" (0..per-mode max), "avc" (even 12..90, AM/SSB
         * only), "softmute" (0..32, AM/SSB only). Ranges validated under the lock once
         * the effective mode is known. */
        bool has_agc = !input["agc"].isNull();
        bool has_avc = !input["avc"].isNull();
        bool has_sm  = !input["softmute"].isNull();
        int  agc_v   = input["agc"]      | -1;
        int  avc_v   = input["avc"]      | -1;
        int  sm_v    = input["softmute"] | -1;

        /* Parse the optional demod mode. Only AM/LSB/USB are settable here: FM is a
         * broadcast demod locked to the VHF band (a native-FM band is rejected under
         * the lock below), so it is never a valid config target. */
        uint8_t new_mode = RADIO_MODE_AM;
        bool mode_str_bad = false;
        if (has_mode) {
            if      (mode_str && strcmp(mode_str, "AM")  == 0) new_mode = RADIO_MODE_AM;
            else if (mode_str && strcmp(mode_str, "LSB") == 0) new_mode = RADIO_MODE_LSB;
            else if (mode_str && strcmp(mode_str, "USB") == 0) new_mode = RADIO_MODE_USB;
            else mode_str_bad = true;
        }

        free(body); req->_tempObject = nullptr;

        if (!has_step && !has_bw && !has_mode && !has_squelch && !has_mute &&
            !has_agc && !has_avc && !has_sm) {
            req->send(400, "application/json",
                "{\"error\":\"provide step_idx, bw_idx, mode, squelch, mute, agc, avc and/or softmute\"}");
            return;
        }
        if (mode_str_bad) {
            req->send(400, "application/json",
                "{\"error\":\"mode must be AM, LSB or USB\"}");
            return;
        }

        /* Read the active band and derive its canonical table mode UNDER the lock:
         * loopTask can switch bands between validation and apply, and the stored
         * step/bw indices are indices into the BAND's own mode table (not the live
         * radio_mode, which /radio/tune can move independently). Validate, apply and
         * snapshot the result labels all under the same hold; defer every req->send
         * until after release so no HTTP response runs while holding radio_mtx. */
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        uint8_t bnd  = radio_band_idx;
        /* A native-FM band is mode-locked: FM demod cannot be switched away from and
         * no other demod can be set on it. Reject a mode request there outright. */
        bool mode_locked = has_mode && bands[bnd].mode == RADIO_MODE_FM;
        /* Effective mode after a (valid) switch decides which step/bw table the
         * step_idx/bw_idx must validate against; band_mode[] is the live demod. */
        uint8_t mode = (has_mode && !mode_locked) ? new_mode : band_mode[bnd];
        uint8_t step_max = stepCount(mode);
        uint8_t bw_max   = bwCount(mode);
        bool step_bad = has_step && (step_idx < 0 || step_idx >= step_max);
        bool bw_bad   = has_bw   && (bw_idx   < 0 || bw_idx   >= bw_max);
        bool squelch_bad = has_squelch && (squelch < 0 || squelch > 127);
        /* DSP validation against the effective mode. AVC/SoftMute are AM/SSB-only:
         * requesting them on an FM band is rejected (like a mode lock), not ignored. */
        uint8_t agc_max = agc_max_for_mode(mode);
        bool agc_bad = has_agc && (agc_v < 0 || agc_v > (int)agc_max);
        bool avc_fm  = has_avc && mode == RADIO_MODE_FM;
        bool avc_bad = has_avc && (avc_v < 12 || avc_v > 90 || (avc_v % 2));
        bool sm_fm   = has_sm  && mode == RADIO_MODE_FM;
        bool sm_bad  = has_sm  && (sm_v < 0 || sm_v > 32);
        if (!mode_locked && !step_bad && !bw_bad && !squelch_bad &&
            !agc_bad && !avc_fm && !avc_bad && !sm_fm && !sm_bad) {
            if (has_mode) {
                /* Mode switch: reset step/bw cursors to the new mode's defaults (the
                 * old indices belong to a different table), zero the BFO, and reapply
                 * the band so the SSB patch loads/unloads and setAM/setSSB run. An
                 * explicit step_idx/bw_idx below then overrides the reset defaults. */
                band_mode[bnd] = new_mode;
                band_step_idx[bnd] = defaultStepIdx[new_mode];
                band_bw_idx[bnd]   = defaultBwIdx[new_mode];
                radio_bfo = 0;
                apply_band_locked(bnd, radio_freq);
            }
            if (has_step) {
                band_step_idx[bnd] = (uint8_t)step_idx;
                apply_step_locked(mode, band_step_idx[bnd]);
            }
            if (has_bw) {
                band_bw_idx[bnd] = (uint8_t)bw_idx;
                apply_bandwidth_locked(mode, band_bw_idx[bnd]);
            }
            if (has_squelch) {
                /* Keep the current metric bit unless squelch_snr was sent. */
                uint8_t metric_bit = has_sqmetric ? (sq_snr ? 0x80 : 0x00)
                                                  : (uint8_t)(radio_squelch & 0x80);
                radio_squelch = (uint8_t)((squelch & 0x7f) | metric_bit);
                /* Threshold cleared -> drop any active squelch mute now, not at the
                 * next poll, so the audio is not left gated. */
                if ((radio_squelch & 0x7f) == 0 && mute_squelch)
                    radio_set_mute(MUTE_SQUELCH, false);
            }
            if (has_mute) {
                radio_set_mute(MUTE_MAIN, mute_on);
                /* Leaving MAIN mute re-pushes the held volume (writes were skipped
                 * while muted); still a no-op audio-wise if squelch keeps it muted. */
                if (!mute_main) rx.setVolume(radio_volume);
            }
            /* Per-mode DSP: store into the effective mode's family slot and apply now. */
            if (has_agc) {
                if (mode == RADIO_MODE_FM)   fm_agc  = (int8_t)agc_v;
                else if (radio_is_ssb(mode)) ssb_agc = (int8_t)agc_v;
                else                         am_agc  = (int8_t)agc_v;
                apply_agc_locked(mode);
            }
            if (has_avc) {
                if (radio_is_ssb(mode)) ssb_avc = (int8_t)avc_v;
                else                    am_avc  = (int8_t)avc_v;
                apply_avc_locked(mode);
            }
            if (has_sm) {
                if (radio_is_ssb(mode)) ssb_sm = (int8_t)sm_v;
                else                    am_sm  = (int8_t)sm_v;
                apply_softmute_locked(mode);
            }
            /* Step/bw/mode, the squelch gate (threshold + metric) and the per-mode DSP
             * scalars are persisted (v4/v5); MAIN mute is ephemeral, so only a change to
             * a persisted parameter schedules a flash write. */
            if (has_step || has_bw || has_mode || has_squelch || has_sqmetric ||
                has_agc || has_avc || has_sm) radio_mark_dirty();
        }
        /* Snapshot the resulting labels under the lock, clamped defensively; the
         * .desc pointers are flash-resident constants, safe to use after release. */
        const char *step_desc = stepsForMode(mode)[clamp_idx(band_step_idx[bnd], step_max)].desc;
        const char *bw_desc   = bwForMode(mode)[clamp_idx(band_bw_idx[bnd], bw_max)].desc;
        const char *mode_desc = radio_mode_str(mode);
        uint8_t sq_snap = radio_squelch;
        bool mute_snap = mute_main;
        int8_t agc_snap = (mode == RADIO_MODE_FM) ? fm_agc
                        : (radio_is_ssb(mode) ? ssb_agc : am_agc);
        int8_t avc_snap = radio_is_ssb(mode) ? ssb_avc : am_avc;
        int8_t sm_snap  = radio_is_ssb(mode) ? ssb_sm  : am_sm;
        bool mode_is_fm = (mode == RADIO_MODE_FM);
        xSemaphoreGive(radio_mtx);

        if (mode_locked) {
            req->send(400, "application/json",
                "{\"error\":\"mode locked on FM band\"}");
            return;
        }
        if (step_bad) {
            char err[80];
            snprintf(err, sizeof(err),
                "{\"error\":\"step_idx out of range (0..%d)\"}", step_max - 1);
            req->send(400, "application/json", err);
            return;
        }
        if (bw_bad) {
            char err[80];
            snprintf(err, sizeof(err),
                "{\"error\":\"bw_idx out of range (0..%d)\"}", bw_max - 1);
            req->send(400, "application/json", err);
            return;
        }
        if (squelch_bad) {
            req->send(400, "application/json",
                "{\"error\":\"squelch out of range (0..127)\"}");
            return;
        }
        if (agc_bad) {
            char err[80];
            snprintf(err, sizeof(err),
                "{\"error\":\"agc out of range (0..%d)\"}", agc_max);
            req->send(400, "application/json", err);
            return;
        }
        if (avc_fm || sm_fm) {
            req->send(400, "application/json",
                "{\"error\":\"avc/softmute not available in FM\"}");
            return;
        }
        if (avc_bad) {
            req->send(400, "application/json",
                "{\"error\":\"avc out of range (even 12..90)\"}");
            return;
        }
        if (sm_bad) {
            req->send(400, "application/json",
                "{\"error\":\"softmute out of range (0..32)\"}");
            return;
        }

        event_add("radio: config mode=%s step=%s bw=%s", mode_desc, step_desc, bw_desc);
        display_show_status();

        JsonDocument doc;
        doc["ok"] = true;
        doc["mode"] = mode_desc;
        doc["step"] = step_desc;
        doc["bw"] = bw_desc;
        doc["squelch"] = sq_snap & 0x7f;
        doc["squelch_metric"] = (sq_snap & 0x80) ? "snr" : "rssi";
        doc["mute"] = mute_snap;
        doc["agc"] = agc_snap;
        /* AVC/SoftMute apply to AM/SSB only; report them only when not in FM. */
        if (!mode_is_fm) {
            doc["avc"] = avc_snap;
            doc["softmute"] = sm_snap;
        }
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /radio/volume — set the receiver volume (0..63) over HTTP. The encoder
     * can already drive volume from the handheld; this exposes the same control to
     * an agent, mirroring the volume field reported by /radio/status. */
    server.on("/radio/volume", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        int v = input["volume"] | -1;

        free(body); req->_tempObject = nullptr;

        if (v < 0 || v > 63) {
            req->send(400, "application/json", "{\"error\":\"volume out of range (0..63)\"}");
            return;
        }

        /* radio_volume is also written from radio_tick(); take radio_mtx so the
         * write and the setVolume() can't interleave with an encoder step. */
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        radio_volume = (uint8_t)v;
        /* Store the level always, but defer pushing it to the chip while MAIN-muted;
         * the value is kept and re-pushed on unmute. (Mute is RX_HARD_MUTE via
         * setAudioMute, independent of RX_VOLUME — setVolume does not lift it.) */
        if (!mute_main) rx.setVolume(radio_volume);
        radio_mark_dirty();  /* real volume change -> schedule a debounced persist */
        xSemaphoreGive(radio_mtx);

        event_add("radio: volume %d", v);
        display_show_status();

        JsonDocument doc;
        doc["ok"] = true;
        doc["volume"] = v;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /radio/scan — blocking sweep of the current-mode band. */
    server.on("/radio/scan", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        /* Absent numeric keys default to a sentinel we reject below. */
        int from = input["from"] | INT32_MIN;
        int to = input["to"] | INT32_MIN;
        int step = input["step"] | 0;
        int min_rssi = input["min_rssi"] | 0;

        free(body); req->_tempObject = nullptr;

        if (from == INT32_MIN || to == INT32_MIN) {
            req->send(400, "application/json", "{\"error\":\"from and to required\"}");
            return;
        }
        if (step <= 0) {
            req->send(400, "application/json", "{\"error\":\"step must be > 0\"}");
            return;
        }
        if (from >= to) {
            req->send(400, "application/json", "{\"error\":\"from must be < to\"}");
            return;
        }

        /* Sweep runs in whatever mode we are already in; pick that band's limits. */
        int band_min, band_max;
        if (radio_mode == RADIO_MODE_FM) {
            band_min = RADIO_FM_MIN; band_max = RADIO_FM_MAX;
        } else if (radio_is_ssb(radio_mode)) {
            band_min = RADIO_SSB_MIN; band_max = RADIO_SSB_MAX;
        } else {
            band_min = RADIO_AM_MIN; band_max = RADIO_AM_MAX;
        }
        if (from < band_min || to > band_max) {
            char err[96];
            snprintf(err, sizeof(err),
                "{\"error\":\"from/to out of range for %s (%d..%d)\"}",
                radio_mode_str(radio_mode), band_min, band_max);
            req->send(400, "application/json", err);
            return;
        }

        int n = (to - from) / step + 1;
        if (n > RADIO_SCAN_MAX_STEPS) {
            req->send(400, "application/json",
                "{\"error\":\"too many steps (max 64) — use a coarser step or paginate\"}");
            return;
        }

        /* Remember where we were: the sweep leaves the receiver retuned. */
        uint16_t saved_freq = radio_freq;

        JsonDocument doc;
        doc["mode"] = radio_mode_str(radio_mode);
        doc["from"] = from;
        doc["to"] = to;
        doc["step"] = step;
        JsonArray points = doc["points"].to<JsonArray>();

        /* Hold radio_mtx across the whole sweep: it monopolises the receiver for
         * ~2 s anyway, and taking/giving per step would only invite a tick-driven
         * tune to fight it mid-scan. No screen repaint happens under the lock. */
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        for (int f = from; f <= to; f += step) {
            rx.setFrequency((uint16_t)f);
            rx.getCurrentReceivedSignalQuality();
            uint8_t rssi = rx.getCurrentRSSI();
            uint8_t snr = rx.getCurrentSNR();
            if (rssi >= min_rssi) {
                JsonObject p = points.add<JsonObject>();
                p["freq"] = f;
                p["rssi"] = rssi;
                p["snr"] = snr;
            }
        }

        /* Restore the pre-scan tuning. Mode never changed, so freq is enough;
         * radio_freq was left untouched and already holds saved_freq. */
        rx.setFrequency(saved_freq);
        xSemaphoreGive(radio_mtx);
        display_show_status();

        doc["count"] = points.size();

        event_add("radio: scan %s %d..%d/%d -> %u", radio_mode_str(radio_mode),
                  from, to, step, (unsigned)points.size());

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* GET /radio/status */
    server.on("/radio/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        rx.getCurrentReceivedSignalQuality();
        uint8_t rssi = rx.getCurrentRSSI();
        uint8_t snr = rx.getCurrentSNR();
        bool mute_snap = radio_is_muted(MUTE_MAIN);
        /* Squelch-layer state, read under the same hold: `squelched` exposes whether
         * the signal gate is muting RIGHT NOW (status.mute only reports MAIN), and
         * `audio_muted` is the effective physical soft-mute (MAIN OR squelch). */
        bool squelched_snap = radio_is_muted(MUTE_SQUELCH);
        bool audio_muted_snap = audio_muted;
        uint8_t sq_snap = radio_squelch;
        /* Per-mode DSP for the live radio_mode. AVC/SoftMute are AM/SSB only. */
        uint8_t lmode = radio_mode;
        bool lmode_fm = (lmode == RADIO_MODE_FM);
        int8_t agc_snap = lmode_fm ? fm_agc : (radio_is_ssb(lmode) ? ssb_agc : am_agc);
        int8_t avc_snap = radio_is_ssb(lmode) ? ssb_avc : am_avc;
        int8_t sm_snap  = radio_is_ssb(lmode) ? ssb_sm  : am_sm;
        xSemaphoreGive(radio_mtx);

        char freq_display[24];
        radio_format_freq(radio_freq, radio_mode, freq_display, sizeof(freq_display));

        JsonDocument doc;
        doc["mode"] = radio_mode_str(radio_mode);
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        if (radio_is_ssb(radio_mode)) doc["bfo"] = radio_bfo;
        /* Step/bandwidth labels come from the BAND's live demod mode (band_mode[idx]),
         * not the live radio_mode — /radio/tune can leave radio_mode on a different
         * mode with a larger table, so reading these under radio_mode risks an OOB.
         * Clamp defensively on top. */
        uint8_t bnd = radio_band_idx;
        uint8_t bmode = band_mode[bnd];
        doc["step"] = stepsForMode(bmode)[clamp_idx(band_step_idx[bnd], stepCount(bmode))].desc;
        doc["bw"] = bwForMode(bmode)[clamp_idx(band_bw_idx[bnd], bwCount(bmode))].desc;
        doc["rssi"] = rssi;
        doc["snr"] = snr;
        doc["volume"] = radio_volume;
        doc["mute"] = mute_snap;
        doc["squelch"] = sq_snap & 0x7f;
        doc["squelch_metric"] = (sq_snap & 0x80) ? "snr" : "rssi";
        doc["squelched"] = squelched_snap;
        doc["audio_muted"] = audio_muted_snap;
        doc["agc"] = agc_snap;
        /* AVC/SoftMute apply to AM/SSB only; report them only when not in FM. */
        if (!lmode_fm) {
            doc["avc"] = avc_snap;
            doc["softmute"] = sm_snap;
        }
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });
}

/*
 * Auto-return timeout: after this long with no input the router falls back from
 * UI_VOLUME to UI_VFO, so the knob is always tuning again when you next touch it
 * (mirrors the ref's ELAPSED_COMMAND behaviour).
 */
#define RADIO_COMMAND_TIMEOUT_MS 10000

/*
 * Per-loop handheld input router, called from main.cpp's loop() via the
 * Skill.tick hook. Four jobs, all cheap: track the push button, drain the two
 * encoder counters into the receiver, apply the command timeout, and repaint the
 * live screen on a throttle.
 *
 *   - Button (PIN_ENC_KEY, active-low) goes through ButtonTracker: a click
 *     toggles what the encoder adjusts (frequency <-> volume); a long-press is a
 *     reserved no-op hook for the menu that grows in a later step.
 *   - Encoder: frequency uses the acceleration-weighted count (fast spin = bigger
 *     jumps), volume uses the raw +/-1 count (precise). Both drained in one
 *     critical section, then applied under radio_mtx so an HTTP tune/scan/status
 *     can't interleave.
 *   - Command timeout auto-returns UI_VOLUME -> UI_VFO after inactivity.
 *   - A ~10 Hz repaint keeps the display in step with the knob without hammering
 *     the panel; it is skipped while custom /display text is showing.
 */
static void radio_tick() {
    static uint32_t last_input_ms = 0;

    /* 1) Button through ButtonTracker (debounce/click/short/long handled inside). */
    static ButtonTracker key_btn;
    ButtonTracker::State key = key_btn.update(digitalRead(PIN_ENC_KEY) == LOW);
    if (key.wasClicked || key.wasShortPressed) {
        if (ui_mode == UI_MENU) {
            /* In the menu a click selects the highlighted row. */
            if (menu_level == MENU_MAIN) {
                /* Dispatch on the cursor index, not the label text: MainItem order
                 * matches menu_main_items[] so this stays in step if labels change. */
                switch (menu_idx) {
                    case MI_SETTINGS:
                        /* Descend into the Settings sublist (keeps its own cursor). */
                        menu_level = MENU_SETTINGS;
                        break;
                    case MI_BAND:
                        /* Open the band picker, seeded on the active band so the
                         * list lands on where we already are. */
                        menu_level = MENU_BAND;
                        menu_band_idx = radio_get_band_idx();
                        break;
                    case MI_STEP:
                        /* Drop into the step value-editor for the active band. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_STEP;
                        break;
                    case MI_BW:
                        /* Drop into the bandwidth value-editor for the active band. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_BW;
                        break;
                    case MI_MODE:
                        /* Mode editing is locked on the native-FM (VHF) band — FM is a
                         * broadcast-only demod. There, bounce back to the VFO; on any
                         * other band drop into the AM/LSB/USB mode value-editor. */
                        if (bands[radio_band_idx].mode == RADIO_MODE_FM) {
                            ui_mode = UI_VFO;
                        } else {
                            menu_level = MENU_ADJUST;
                            adjust_target = ADJ_MODE;
                        }
                        break;
                    case MI_SQUELCH:
                        /* Drop into the numeric squelch-threshold value-editor. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_SQUELCH;
                        break;
                    case MI_MUTE:
                        /* Manual MAIN-mute toggle (the knob/menu equivalent of the
                         * ref's clickVolume mute) — a toggle, not a value editor, so
                         * flip it under radio_mtx and drop straight back to the VFO.
                         * Re-push the held volume on unmute (writes are skipped while
                         * muted). Mute is ephemeral (not persisted): no mark_dirty. */
                        xSemaphoreTake(radio_mtx, portMAX_DELAY);
                        radio_set_mute(MUTE_MAIN, !mute_main);
                        if (!mute_main) rx.setVolume(radio_volume);
                        xSemaphoreGive(radio_mtx);
                        ui_mode = UI_VFO;
                        menu_level = MENU_MAIN;
                        break;
                    default:
                        /* No other MAIN row is a leaf yet — bounce back to the VFO. */
                        ui_mode = UI_VFO;
                        break;
                }
            } else if (menu_level == MENU_ADJUST) {
                /* A click confirms the value and steps back up to the MAIN list;
                 * the value was already applied live while scrolling. */
                menu_level = MENU_MAIN;
            } else if (menu_level == MENU_BAND) {
                /* Selecting a band IS the exit: apply it and drop back to the VFO on
                 * the new band. radio_select_band takes radio_mtx itself. */
                radio_select_band(menu_band_idx);
                ui_mode = UI_VFO;
                menu_level = MENU_MAIN;  /* next menu entry opens at the top level */
            } else {
                /* SETTINGS: dispatch on the sublist cursor (SettingsItem order matches
                 * menu_settings_items[]). AGC/AVC/SoftMute descend into their numeric
                 * value-editors; Brightness/Theme/About are not leaves yet and, with
                 * Back, return to MAIN. */
                switch (menu_settings_idx) {
                    case SI_AGC:
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_AGC;
                        break;
                    case SI_AVC:
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_AVC;
                        break;
                    case SI_SOFTMUTE:
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_SOFTMUTE;
                        break;
                    default:
                        /* SI_BRIGHTNESS/SI_THEME/SI_ABOUT placeholders (TODO: wire real
                         * editors) and SI_BACK all step back up to the MAIN list. */
                        menu_level = MENU_MAIN;
                        break;
                }
            }
        } else {
            /* Any press under 2s toggles the encoder's target: frequency <->
             * volume. Treating wasShortPressed (0.5-2s) like a click removes the
             * dead zone where a slightly-held tap did nothing. */
            ui_mode = (ui_mode == UI_VFO) ? UI_VOLUME : UI_VFO;
        }
        last_input_ms = millis();
    }
    /* Long-press (>=2s) opens the modal menu. isLongPressed is a level, held true
     * for the whole press, so an edge-guard fires the entry exactly once and rearms
     * only after the button is released. */
    static bool long_handled = false;
    if (key.isLongPressed) {
        if (!long_handled) {
            long_handled = true;
            ui_mode = UI_MENU;
            menu_level = MENU_MAIN;  /* always open at the top level */
            last_input_ms = millis();
        }
    } else {
        long_handled = false;
    }

    /* 2) Snapshot both encoder counters in one critical section so no ISR step
     * slips through and raw/accel stay a consistent pair. */
    int8_t  d;
    int16_t da;
    portENTER_CRITICAL(&enc_mux);
    d = enc_delta;
    da = enc_delta_accel;
    enc_delta = 0;
    enc_delta_accel = 0;
    portEXIT_CRITICAL(&enc_mux);

    if ((d != 0 || da != 0) || key.wasClicked) last_input_ms = millis();

    if (ui_mode == UI_MENU) {
        /* Menu scroll uses the raw +/-1 count for a precise one-row-per-detent
         * step. Pure UI state — it never touches the receiver, so it takes neither
         * radio_ok nor radio_mtx. */
        if (d != 0) {
            if (menu_level == MENU_ADJUST) {
                if (adjust_target == ADJ_MODE) {
                    /* Mode cycle takes radio_mtx itself and handles the FM-lock,
                     * step/bw reset and full band reapply (SSB patch load/unload). */
                    radio_cycle_mode(d > 0 ? 1 : -1);
                } else if (adjust_target == ADJ_SQUELCH) {
                    /* Numeric threshold edit: CLAMP the low 7 bits to 0..127 (not a
                     * table wrap), preserving the metric-select bit. The throttled
                     * squelch poll re-evaluates the gate at the new threshold within
                     * one RADIO_SQUELCH_POLL_MS, so no explicit re-apply here. The
                     * threshold is persisted (v4), so mark the state dirty. */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    int thr = (int)(radio_squelch & 0x7f) + d;
                    if (thr < 0)   thr = 0;
                    if (thr > 127) thr = 127;
                    radio_squelch = (uint8_t)((radio_squelch & 0x80) | (thr & 0x7f));
                    radio_mark_dirty();
                    xSemaphoreGive(radio_mtx);
                } else if (adjust_target == ADJ_AGC) {
                    /* Numeric AGC index edit: clamp to the live mode's 0..max and apply
                     * immediately (unlike squelch, no poll re-applies it). Persisted (v5). */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    uint8_t m = band_table_mode();
                    int8_t *slot = (m == RADIO_MODE_FM) ? &fm_agc
                                 : (radio_is_ssb(m) ? &ssb_agc : &am_agc);
                    int v = (int)*slot + d;
                    int hi = (int)agc_max_for_mode(m);
                    if (v < 0)  v = 0;
                    if (v > hi) v = hi;
                    *slot = (int8_t)v;
                    if (radio_ok) apply_agc_locked(m);
                    radio_mark_dirty();
                    xSemaphoreGive(radio_mtx);
                } else if (adjust_target == ADJ_AVC) {
                    /* Numeric AVC edit (AM/SSB only; no-op on FM). Even step of 2, clamped
                     * to 12..90, applied immediately. Persisted (v5). */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    uint8_t m = band_table_mode();
                    if (m != RADIO_MODE_FM) {
                        int8_t *slot = radio_is_ssb(m) ? &ssb_avc : &am_avc;
                        int v = (int)*slot + (int)d * 2;
                        if (v < 12) v = 12;
                        if (v > 90) v = 90;
                        *slot = (int8_t)v;
                        if (radio_ok) apply_avc_locked(m);
                        radio_mark_dirty();
                    }
                    xSemaphoreGive(radio_mtx);
                } else if (adjust_target == ADJ_SOFTMUTE) {
                    /* Numeric soft-mute edit (AM/SSB only; no-op on FM). Clamp 0..32,
                     * applied immediately. Persisted (v5). */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    uint8_t m = band_table_mode();
                    if (m != RADIO_MODE_FM) {
                        int8_t *slot = radio_is_ssb(m) ? &ssb_sm : &am_sm;
                        int v = (int)*slot + d;
                        if (v < 0)  v = 0;
                        if (v > 32) v = 32;
                        *slot = (int8_t)v;
                        if (radio_ok) apply_softmute_locked(m);
                        radio_mark_dirty();
                    }
                    xSemaphoreGive(radio_mtx);
                } else {
                    /* The value editor scrolls the parameter itself, not a cursor:
                     * bump the active band's step/bandwidth row and apply it live so
                     * the change is audible immediately. This one menu branch does
                     * touch the receiver, so it takes radio_mtx. */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    uint8_t bnd = radio_band_idx;
                    /* Scroll the band's live demod mode (band_mode[]), not radio_mode:
                     * they can differ after /radio/tune, and menu_wrap must wrap modulo
                     * the table these indices actually belong to. */
                    uint8_t mode = band_mode[bnd];
                    if (adjust_target == ADJ_STEP) {
                        band_step_idx[bnd] =
                            (uint8_t)menu_wrap(band_step_idx[bnd], d, stepCount(mode));
                        if (radio_ok) apply_step_locked(mode, band_step_idx[bnd]);
                    } else {
                        band_bw_idx[bnd] =
                            (uint8_t)menu_wrap(band_bw_idx[bnd], d, bwCount(mode));
                        if (radio_ok) apply_bandwidth_locked(mode, band_bw_idx[bnd]);
                    }
                    radio_mark_dirty();
                    xSemaphoreGive(radio_mtx);
                }
            } else if (menu_level == MENU_SETTINGS)
                menu_settings_idx = menu_wrap(menu_settings_idx, d, MENU_SETTINGS_COUNT);
            else if (menu_level == MENU_BAND)
                menu_band_idx = (uint8_t)menu_wrap(menu_band_idx, d, BAND_COUNT);
            else
                menu_idx = menu_wrap(menu_idx, d, MENU_MAIN_COUNT);
        }
    } else if (radio_ok && (d != 0 || da != 0)) {
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        if (ui_mode == UI_VFO) {
            /* Frequency follows the accelerated count so a fast sweep strides in
             * bigger jumps. frequencyUp/Down steps by the current band's step; in
             * SSB the step is 0 (set in /radio/tune) so this is a no-op there —
             * SSB fine-tuning via BFO is out of scope for this cut. */
            if (da != 0) {
                for (int k = 0; k < abs((int)da); k++) {
                    if (da > 0) rx.frequencyUp();
                    else        rx.frequencyDown();
                }
                radio_freq = rx.getFrequency();
                radio_mark_dirty();  /* encoder retune -> schedule a debounced persist */
            }
        } else {
            /* Volume follows the raw count for precise +/-1 steps. */
            if (d != 0) {
                int v = constrain((int)radio_volume + d, 0, 63);
                radio_volume = (uint8_t)v;
                /* Store always; defer the chip write while MAIN-muted, re-pushed on
                 * unmute. (Mute is RX_HARD_MUTE via setAudioMute, independent of
                 * RX_VOLUME — setVolume does not lift it. See radio_set_mute.) */
                if (!mute_main) rx.setVolume(radio_volume);
                radio_mark_dirty();  /* encoder volume change -> schedule a debounced persist */
            }
        }
        xSemaphoreGive(radio_mtx);
    }

    /* 3) Command timeout: auto-return to the VFO from volume OR the menu after
     * inactivity, so the knob is always tuning again when next touched. */
    if ((ui_mode == UI_VOLUME || ui_mode == UI_MENU) &&
        millis() - last_input_ms > RADIO_COMMAND_TIMEOUT_MS) {
        ui_mode = UI_VFO;
        menu_level = MENU_MAIN;  /* covers MENU_BAND too: next entry opens at the top */
    }

    /* 3b) Squelch: throttled signal-gate poll (every RADIO_SQUELCH_POLL_MS, not
     * every tick — a RSSI/SNR read is an I2C round-trip under radio_mtx). radio_squelch
     * packs a 0..127 threshold in its low 7 bits (0 = disabled) and a metric select in
     * the top bit (1 = SNR, else RSSI). When the chosen metric drops below the
     * threshold the SQUELCH layer mutes; at/above it unmutes. No hysteresis (matches
     * the reference). The measurement and the set_mute share ONE radio_mtx hold and no
     * delay(); the physical toggle happens only on an edge inside radio_set_mute. */
    static uint32_t last_squelch_ms = 0;
    if (radio_ok && millis() - last_squelch_ms > RADIO_SQUELCH_POLL_MS) {
        last_squelch_ms = millis();
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        uint8_t thr = radio_squelch & 0x7f;
        if (thr) {
            rx.getCurrentReceivedSignalQuality();
            uint8_t rssi = rx.getCurrentRSSI();
            uint8_t snr = rx.getCurrentSNR();
            uint8_t val = (radio_squelch & 0x80) ? snr : rssi;
            if (val >= thr && mute_squelch)       radio_set_mute(MUTE_SQUELCH, false);
            else if (val < thr && !mute_squelch)  radio_set_mute(MUTE_SQUELCH, true);
        } else if (mute_squelch) {
            radio_set_mute(MUTE_SQUELCH, false);  /* threshold cleared -> unmute */
        }
        xSemaphoreGive(radio_mtx);
    }

    /* 4) Throttled repaint (~10 Hz). display_tick_render() itself leaves a custom
     * /display screen untouched, so we never stomp user text. */
    static uint32_t last_draw = 0;
    if (millis() - last_draw > 100) {
        last_draw = millis();
        display_tick_render();
    }

    /* 5) Debounced persist: once the state has been dirty and idle long enough,
     * snapshot it under the mutex and flush one JSON blob to SPIFFS. The snapshot
     * is consistent (freq/mode/volume/bfo are also written by the HTTP task and
     * the encoder branch above), but the write itself — a blocking flash op that
     * detaches the encoder — runs OUTSIDE the mutex so it never stalls a
     * concurrent status/tune. Clearing radio_dirty inside the lock closes the
     * window where a change during the write would be lost. */
    if (radio_dirty && millis() - radio_last_change_ms > RADIO_STORE_IDLE_MS) {
        uint16_t f; uint8_t v, bnd, bmode, bstep, bbw, sq; int b;
        int8_t fagc, aagc, sagc, aavc, savc, asmv, ssmv;
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        f = radio_freq; v = radio_volume; b = radio_bfo; bnd = radio_band_idx;
        /* v3: persist the active band's live demod mode and step/bw cursors so a mode
         * switch / step / bandwidth choice survives the reboot (restore reapplies them
         * via apply_band_locked). The old "mode" key held radio_mode but restore never
         * read it; it now carries band_mode[bnd], which restore does use. */
        bmode = band_mode[bnd]; bstep = band_step_idx[bnd]; bbw = band_bw_idx[bnd];
        /* v4: the packed squelch byte (threshold + metric bit) is global, not per-band. */
        sq = radio_squelch;
        /* v5: the seven per-mode DSP scalars (AGC/AVC/SoftMute), all global. */
        fagc = fm_agc; aagc = am_agc; sagc = ssb_agc;
        aavc = am_avc; savc = ssb_avc; asmv = am_sm; ssmv = ssb_sm;
        radio_dirty = false;
        xSemaphoreGive(radio_mtx);
        JsonDocument doc;
        doc["v"] = RADIO_STATE_VERSION;
        doc["mode"] = bmode;
        doc["freq"] = f;
        doc["volume"] = v;
        doc["bfo"] = b;
        doc["band"] = bnd;
        doc["step"] = bstep;
        doc["bw"] = bbw;
        doc["squelch"] = sq;
        doc["fm_agc"] = fagc;
        doc["am_agc"] = aagc;
        doc["ssb_agc"] = sagc;
        doc["am_avc"] = aavc;
        doc["ssb_avc"] = savc;
        doc["am_sm"] = asmv;
        doc["ssb_sm"] = ssmv;
        String out;
        serializeJson(doc, out);
        write_spiffs_file(RADIO_STATE_FILE, out);  /* blocking flash write + encoder detach, outside the mutex */
    }
}

static const Skill radio_skill = {
    .name = "radio",
    .version = "0.2.0",
    .describe = radio_describe,
    .endpoints = radio_endpoints,
    .register_routes = radio_register_routes,
    .tick = radio_tick
};

/*
 * Restore the last-saved band/freq/volume/bfo from SPIFFS over the FM defaults.
 * Called once from skill_radio_init() after the receiver is up and before the
 * skill is registered, so it runs single-tasked (server not yet started) — no
 * mutex needed. Anything missing, unparseable, schema-mismatched or out of range
 * is ignored, leaving the FM defaults in place. Never marks the state dirty.
 *
 * The stored band index drives the reapply through the same per-band path
 * (apply_band_locked) as /radio/band: the saved frequency is validated against that
 * band's own min/max — which is what lets an SW or SSB frequency survive a reboot
 * (the old fixed-window check rejected them and fell back to FM). v3 additionally
 * restores the band's live demod mode and its step/bandwidth cursors (set BEFORE the
 * reapply so apply_band_locked reads them); v4 also restores the global squelch byte
 * (threshold + metric). An older-version file is rejected by the version check above,
 * resetting to the FM defaults.
 */
static void radio_restore_state() {
    String raw = read_spiffs_file(RADIO_STATE_FILE);
    if (raw.length() == 0) return;  /* no saved state -> keep FM defaults */

    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
    if ((int)(doc["v"] | 0) != RADIO_STATE_VERSION) return;  /* absent/mismatched schema */

    int band   = doc["band"]   | -1;
    int freq   = doc["freq"]   | -1;
    int volume = doc["volume"] | -1;
    int bfo    = doc["bfo"]    | 0;
    int smode  = doc["mode"]   | -1;
    int sstep  = doc["step"]   | -1;
    int sbw    = doc["bw"]     | -1;
    int ssq    = doc["squelch"] | -1;  /* v4: packed threshold + metric bit */

    /* Reject anything malformed — a single bad field falls back to the defaults. */
    if (band < 0 || band >= BAND_COUNT) return;
    if (volume < 0 || volume > 63) return;
    if (freq < 0) return;

    const RadioBand *b = &bands[band];
    /* Clip the saved frequency to the band's window (per-band, not fixed limits). */
    uint16_t f = (uint16_t)freq;
    if (f < b->minFreq) f = b->minFreq;
    if (f > b->maxFreq) f = b->maxFreq;

    /* Restore the band's live demod mode: a native-FM band is always FM (locked); any
     * other band takes a saved AM/LSB/USB, else falls back to its native default. The
     * step/bw cursors are then clamped to that mode's tables (bad -> mode default).
     * All three are set before apply_band_locked, which reads band_mode/step/bw. */
    uint8_t rmode;
    if (b->mode == RADIO_MODE_FM) {
        rmode = RADIO_MODE_FM;
    } else if (smode == RADIO_MODE_AM || smode == RADIO_MODE_LSB || smode == RADIO_MODE_USB) {
        rmode = (uint8_t)smode;
    } else {
        rmode = b->mode;
    }
    band_mode[band] = rmode;
    band_step_idx[band] = (sstep >= 0 && sstep < stepCount(rmode))
                              ? (uint8_t)sstep : defaultStepIdx[rmode];
    band_bw_idx[band]   = (sbw  >= 0 && sbw  < bwCount(rmode))
                              ? (uint8_t)sbw  : defaultBwIdx[rmode];

    /* Restore the packed squelch byte (threshold + metric bit). Any 0..255 value is
     * valid on the wire — the low 7 bits are the threshold (always <=127) and 0x80 is
     * the metric select. The throttled squelch poll applies the gate after boot; a
     * missing/negative field leaves radio_squelch at its 0 (off) default. */
    if (ssq >= 0 && ssq <= 255) radio_squelch = (uint8_t)ssq;

    /* v5: per-mode DSP scalars (AGC/AVC/SoftMute). Each is validated to its own range;
     * anything missing or out of range keeps the compiled-in default. Set BEFORE
     * apply_band_locked, which reapplies the active mode's values to the chip. */
    int j;
    j = doc["fm_agc"]  | -1; if (j >= 0  && j <= 27)               fm_agc  = (int8_t)j;
    j = doc["am_agc"]  | -1; if (j >= 0  && j <= 37)               am_agc  = (int8_t)j;
    j = doc["ssb_agc"] | -1; if (j >= 0  && j <= 1)                ssb_agc = (int8_t)j;
    j = doc["am_avc"]  | -1; if (j >= 12 && j <= 90 && !(j % 2))   am_avc  = (int8_t)j;
    j = doc["ssb_avc"] | -1; if (j >= 12 && j <= 90 && !(j % 2))   ssb_avc = (int8_t)j;
    j = doc["am_sm"]   | -1; if (j >= 0  && j <= 32)               am_sm   = (int8_t)j;
    j = doc["ssb_sm"]  | -1; if (j >= 0  && j <= 32)               ssb_sm  = (int8_t)j;

    /* Volume must be set before apply_band_locked, which reapplies it to the chip. */
    radio_volume  = (uint8_t)volume;
    band_freq[band] = f;
    apply_band_locked((uint8_t)band, f);

    /* apply_band_locked zeroes the BFO on every band change; a valid saved offset is
     * reapplied here so SSB fine-tuning survives the reboot. Gate on the RESTORED
     * demod (rmode), not the band's native mode — a mode switch may have moved the
     * band into or out of SSB. */
    if (radio_is_ssb(rmode) && bfo >= -RADIO_BFO_MAX && bfo <= RADIO_BFO_MAX) {
        radio_bfo = bfo;
        rx.setSSBBfo(-bfo);
    }
}

/*
 * Bring the SI4732 up. Blocking, runs once at boot after hw_probe() (Wire is
 * already begun). The amplifier is muted first to avoid the power-on click,
 * then re-enabled once the receiver is tuned. Boot stays FM — the SSB patch is
 * uploaded lazily on the first SSB tune, not here.
 */
static void skill_radio_init() {
    /* Guard the receiver before any code path can touch it from two tasks.
     * Created even on the chip-absent path so the accessors stay well-defined. */
    radio_mtx = xSemaphoreCreateMutex();

    /* Encoder: pull-up on the push button, interrupt on both quadrature pins.
     * Wire and the pins are otherwise free here (this is boot, single-tasked). */
    pinMode(PIN_ENC_KEY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), rotary_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), rotary_isr, CHANGE);

    /* Amplifier off first — silences the power-on click. */
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW);

    if (rx.getDeviceI2CAddress(PIN_SI4732_RST) <= 0) {
        /* Chip absent: register anyway so the routes report 503 honestly. */
        Serial.println("[radio] SI4732 not found on I2C");
        radio_ok = false;
        skill_register(&radio_skill);
        return;
    }

    rx.setup(PIN_SI4732_RST, RADIO_MODE_FM);
    rx.setAudioMuteMcuPin(PIN_AUDIO_MUTE);

    /* FM 64..108 MHz, start 100.0 MHz, 100 kHz step. */
    rx.setFM(RADIO_FM_MIN, RADIO_FM_MAX, 10000, 10);
    /* Post-setFM config to match the reference firmware (see /radio/tune):
     * de-emphasis 1 = 50 us (EU/JP/AU); FM AGC from the per-mode store (default
     * index 0 = AGC on, no attenuation). Single-tasked at boot: no lock needed. */
    rx.setFMDeEmphasis(1);
    apply_agc_locked(RADIO_MODE_FM);
    radio_freq = 10000;
    radio_mode = RADIO_MODE_FM;

    rx.setVolume(radio_volume);

    /* Let the receiver settle, then enable the amplifier. */
    delay(100);
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH);

    /* Seed each band's remembered frequency from its default before restore, so a
     * band never visited this boot still tunes to a sane spot. radio_band_idx stays
     * 0 (FM VHF); the boot FM tune above matches band 0's mode. */
    for (uint8_t i = 0; i < BAND_COUNT; i++) {
        band_freq[i] = bands[i].defFreq;
        band_mode[i] = bands[i].mode;  /* live demod starts at the band's native mode */
        band_step_idx[i] = defaultStepIdx[bands[i].mode];
        band_bw_idx[i]   = defaultBwIdx[bands[i].mode];
    }

    radio_ok = true;

    /* Overlay any saved band/freq/volume/bfo on top of the FM defaults. Runs
     * single-tasked here (before server.begin), so no mutex is required. */
    radio_restore_state();

    Serial.println("[radio] SI4732 up: FM 100.0 MHz");
    skill_register(&radio_skill);
}
