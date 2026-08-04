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
 * Radio-state persistence. The last-tuned mode/freq/volume/bfo are stashed in a
 * tiny JSON file on SPIFFS so a reboot lands back where the user left off. Writes
 * are debounced: a real change marks the state dirty and, once the knob has been
 * idle for RADIO_STORE_IDLE_MS, radio_tick() flushes one snapshot to flash. This
 * coalesces a fast encoder sweep (dozens of freq changes) into a single write and
 * keeps the blocking flash write off the settle-critical tuning path.
 */
#define RADIO_STATE_FILE    "/radio.json"
/* v2 adds the selected band-plan index; a v1 file (no "band") is rejected by the
 * version check in radio_restore_state() and falls back to the FM defaults. */
#define RADIO_STATE_VERSION 2
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

/* --- Receiver + state (file-local) --- */
static SI4735 rx;
static uint16_t radio_freq;              /* current frequency (FM: 10 kHz units, AM/SSB: kHz) */
static uint8_t  radio_mode;              /* RADIO_MODE_FM | _LSB | _USB | _AM */
static uint8_t  radio_volume = 40;       /* 0..63 */
static bool     radio_ok = false;        /* true once the SI4732 answered on I2C */
static bool     radio_ssb_loaded = false;/* true while the SSB patch is live in chip RAM */
static int      radio_bfo = 0;           /* current BFO offset in Hz (SSB only) */

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
enum MenuLevel { MENU_MAIN, MENU_SETTINGS, MENU_BAND };
static uint8_t menu_level = MENU_MAIN;
static int     menu_idx = 0;      /* cursor in the MAIN list */
static int     menu_settings_idx = 0; /* cursor in the SETTINGS sublist */
/* Cursor in the BAND picker (indexes bands[]). Re-seeded from radio_get_band_idx()
 * each time MENU_BAND is entered, so the list always opens on the active band. */
static uint8_t menu_band_idx = 0;

/*
 * MAIN-list dispatch keys. The order MUST match menu_main_items[] below so the
 * click handler can switch on the raw cursor (menu_idx) instead of strcmp-ing the
 * label — the label is display text, the index is the contract.
 */
enum MainItem { MI_BAND = 0, MI_MODE, MI_STEP, MI_BW, MI_SETTINGS };

static const char *const menu_main_items[] = {
    "Band", "Mode", "Step", "Bandwidth", "Settings"
};
static const char *const menu_settings_items[] = {
    "Brightness", "Theme", "About", "Back"
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
        default:            return menu_idx;
    }
}
/* Length of the active level's list. */
int radio_get_menu_count() {
    switch (menu_level) {
        case MENU_SETTINGS: return MENU_SETTINGS_COUNT;
        case MENU_BAND:     return BAND_COUNT;
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
        default:            return "Menu";
    }
}
const char *radio_get_menu_item(int i) {
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

/* AM tuning step for a band: MW keeps 10 kHz channel spacing, SW broadcast uses
 * 5 kHz. FM is fixed at 10 (10 kHz units) and SSB at 0 (BFO does the fine work). */
static uint8_t band_am_step(uint8_t type) {
    return (type == BAND_TYPE_MW) ? 10 : 5;
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
    uint8_t mode = b->mode;

    if (radio_is_ssb(mode)) {
        /* Lazily upload the SSB patch (audiobw=1 -> 2.2 kHz) on first SSB use. */
        if (!radio_ssb_loaded) {
            rx.loadPatch(ssb_patch_content, sizeof(ssb_patch_content), 1);
            radio_ssb_loaded = true;
        }
        /* mode value doubles as usblsb (1=LSB, 2=USB); step 0 in SSB. */
        rx.setSSB(b->minFreq, b->maxFreq, freq, 0, mode);
        rx.setSSBAutomaticVolumeControl(1);
        radio_bfo = 0;
        rx.setSSBBfo(0);
    } else if (mode == RADIO_MODE_FM) {
        rx.setFM(b->minFreq, b->maxFreq, freq, 10);
        /* Match /radio/tune's post-setFM config: 50 us de-emphasis + FM AGC. */
        rx.setFMDeEmphasis(1);
        rx.setAutomaticGainControl(0, 0);
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
    } else {  /* AM (MW / SW broadcast) */
        rx.setAM(b->minFreq, b->maxFreq, freq, band_am_step(b->type));
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
    }
    /* A mode change can reset the chip volume to its power-on default; restore it. */
    rx.setVolume(radio_volume);

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
             * De-emphasis 1 = 50 us (EU/JP/AU); AGC enabled with no attenuation
             * (AGCDIS=0, AGCIDX=0) mirrors the ref's doAgc(0) at FM AGC index 0. */
            rx.setFMDeEmphasis(1);
            rx.setAutomaticGainControl(0, 0);
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
        rx.setVolume(radio_volume);
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
        xSemaphoreGive(radio_mtx);

        char freq_display[24];
        radio_format_freq(radio_freq, radio_mode, freq_display, sizeof(freq_display));

        JsonDocument doc;
        doc["mode"] = radio_mode_str(radio_mode);
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        if (radio_is_ssb(radio_mode)) doc["bfo"] = radio_bfo;
        doc["rssi"] = rssi;
        doc["snr"] = snr;
        doc["volume"] = radio_volume;
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
                    default:
                        /* MI_MODE/MI_STEP/MI_BW are placeholders — real parameter
                         * editing is wired in SEED-ATS-8. Bounce to the VFO. */
                        ui_mode = UI_VFO;
                        break;
                }
            } else if (menu_level == MENU_BAND) {
                /* Selecting a band IS the exit: apply it and drop back to the VFO on
                 * the new band. radio_select_band takes radio_mtx itself. */
                radio_select_band(menu_band_idx);
                ui_mode = UI_VFO;
                menu_level = MENU_MAIN;  /* next menu entry opens at the top level */
            } else {
                /* SETTINGS: "Back" and every other placeholder return to MAIN. */
                menu_level = MENU_MAIN;
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
            if (menu_level == MENU_SETTINGS)
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
                rx.setVolume(radio_volume);
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
        uint16_t f; uint8_t m, v, bnd; int b;
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        f = radio_freq; m = radio_mode; v = radio_volume; b = radio_bfo; bnd = radio_band_idx;
        radio_dirty = false;
        xSemaphoreGive(radio_mtx);
        JsonDocument doc;
        doc["v"] = RADIO_STATE_VERSION;
        doc["mode"] = m;
        doc["freq"] = f;
        doc["volume"] = v;
        doc["bfo"] = b;
        doc["band"] = bnd;
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
 * v2 stores the selected band index rather than the raw mode, so the reapply runs
 * through the same per-band path (apply_band_locked) as /radio/band: the saved
 * frequency is validated against that band's own min/max — which is what lets an
 * SW or SSB frequency survive a reboot (the old fixed-window check rejected them
 * and fell back to FM). A v1 file has no "band" and is rejected by the version
 * check above, resetting to the FM defaults.
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
    int bfo    = doc["bfo"]     | 0;

    /* Reject anything malformed — a single bad field falls back to the defaults. */
    if (band < 0 || band >= BAND_COUNT) return;
    if (volume < 0 || volume > 63) return;
    if (freq < 0) return;

    const RadioBand *b = &bands[band];
    /* Clip the saved frequency to the band's window (per-band, not fixed limits). */
    uint16_t f = (uint16_t)freq;
    if (f < b->minFreq) f = b->minFreq;
    if (f > b->maxFreq) f = b->maxFreq;

    /* Volume must be set before apply_band_locked, which reapplies it to the chip. */
    radio_volume  = (uint8_t)volume;
    band_freq[band] = f;
    apply_band_locked((uint8_t)band, f);

    /* apply_band_locked zeroes the BFO on every band change; a valid saved offset
     * on an SSB band is reapplied here so fine-tuning survives the reboot. */
    if (radio_is_ssb(b->mode) && bfo >= -RADIO_BFO_MAX && bfo <= RADIO_BFO_MAX) {
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
     * de-emphasis 1 = 50 us (EU/JP/AU), AGC enabled with no attenuation. */
    rx.setFMDeEmphasis(1);
    rx.setAutomaticGainControl(0, 0);
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
    for (uint8_t i = 0; i < BAND_COUNT; i++) band_freq[i] = bands[i].defFreq;

    radio_ok = true;

    /* Overlay any saved band/freq/volume/bfo on top of the FM defaults. Runs
     * single-tasked here (before server.begin), so no mutex is required. */
    radio_restore_state();

    Serial.println("[radio] SI4732 up: FM 100.0 MHz");
    skill_register(&radio_skill);
}
