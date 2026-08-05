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
 *   GET  /radio/status  — current freq/mode/RSSI/SNR (+bfo in SSB, +RDS in FM)
 */

#include <SI4735.h>
#include "../patch_init.h"
#include "../Rotary.h"
#include "../Button.h"

/*
 * Display-theme accessors, defined in display.cpp — included AFTER radio.cpp in the
 * same translation unit, so the persist path here needs the prototypes up front (same
 * arrangement as display_show_status(), forward-declared in main.cpp). The theme is a
 * global display setting persisted alongside the radio state (v7); radio.cpp only reads
 * and restores the index, display.cpp owns the palette.
 */
void        display_set_theme(uint8_t idx);
uint8_t     display_get_theme();
uint8_t     display_theme_count();
const char *display_theme_name(uint8_t i);

/* Backlight brightness accessors, also defined in display.cpp. The backlight duty
 * is a global display setting persisted alongside the radio state (v8); radio.cpp
 * only reads and restores the level, display.cpp owns the ledc channel. Guarded by
 * display_mtx inside display_set_brightness — never touch radio_mtx for it. */
void        display_set_brightness(uint8_t v);
uint8_t     display_get_brightness();

/* Screen-layout accessors, also defined in display.cpp. The layout (Default vs.
 * S-Meter) is a global display setting persisted alongside the radio state in the
 * same v8 (read tolerantly, no bump). radio.cpp only reads and restores the index;
 * display.cpp owns the draw. Guarded by display_mtx inside display_set_layout —
 * never touch radio_mtx for it. */
void        display_set_layout(uint8_t idx);
uint8_t     display_get_layout();
uint8_t     display_layout_count();
const char *display_layout_name(uint8_t i);

/* Idle-dim ("sleep") accessors, also in display.cpp. sleep_timeout (seconds, 0=off)
 * is a global display setting persisted in the same v8 (read tolerantly, no bump);
 * the dim state machine and the backlight PWM live in display.cpp — this is idle
 * BACKLIGHT dimming only, never a real sleep (WiFi/HTTP/mesh stay up). radio.cpp
 * only reads/restores the threshold and reports the live dim stage in /radio/status.
 * Guarded by display_mtx inside display_set_sleep — never touch radio_mtx for it. */
void        display_set_sleep(uint16_t secs);
uint16_t    display_get_sleep();
const char *display_dim_state();

/*
 * EIBI schedule helpers, defined later in eibi.cpp (included right after this file in
 * the unity build, so these forward decls let radio_tick / the menu reach forward to
 * them). All run on the loopTask only: the tick polls the loaded offline schedule to
 * show "who is broadcasting" on AM/SSB, and the "EIBI Load" menu action kicks a
 * background download. eibi_now_name / eibi_lookup do SPIFFS reads serialised on the
 * skill's own eibi_mtx — never called from an ISR or under radio_mtx/display_mtx.
 */
static bool eibi_available();
static bool eibi_utc_now(int &h, int &m);
static bool eibi_now_name(uint16_t freq, int utc_h, int utc_m, char *out, size_t len);
static bool eibi_start_load();
static void eibi_menu_label(char *buf, size_t len);

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
/* SSB calibration trim (per-band/sideband BFO offset) limit in Hz and detent step.
 * |bfo| <= 14000 and |cal| <= 2000 sum to <= 16000, which setSSBBfo(int) accepts. */
#define RADIO_CAL_MAX  2000
#define RADIO_CAL_STEP 10

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
 * RDS poll cadence (FM only). getRdsStatus() is an I2C round-trip taken under
 * radio_mtx from radio_tick(); 250 ms keeps the station name/RadioText fresh
 * without hammering the bus or fighting the squelch poll for the mutex.
 */
#define RADIO_RDS_POLL_MS 250

/*
 * EIBI "now broadcasting" poll cadence (AM/SSB only). eibi_now_name() does a SPIFFS
 * binary search + forward scan — a flash read, an order of magnitude costlier than
 * the RDS I2C poll — so it runs far less often: 2.5 s keeps the station name
 * responsive to a retune without hammering the filesystem.
 */
#define RADIO_EIBI_POLL_MS 2500

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
 * seven per-mode DSP scalars (AGC/AVC/SoftMute), all global (not per-band). v6 adds
 * the active band's two SSB calibration slots (usb_cal/lsb_cal), per-band like freq.
 * v7 adds the global display theme index (owned by display.cpp; not per-band).
 * v8 adds the global backlight brightness (also display.cpp); layout/sleep keys are
 * to follow in the same v8 — new keys are read tolerantly (doc["key"] | default) so a
 * later addition needs no version bump, only a v7-or-older file resets to defaults. */
#define RADIO_STATE_VERSION 8
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
/*
 * Thin subclass over the vendor driver to fix two RDS getters that are broken in
 * this library version (2.1.8). getRdsPI() is the real bug: it returns only the
 * low byte of the Program Identification. getRdsProgramType() reads the raw union
 * bitfield, so we recompute PTY explicitly from Block B bits [9:5] for portability
 * rather than trust the compiler/endianness-dependent union layout. Both need the
 * full Block A/B words, which live in the protected currentRdsStatus struct: we
 * reach into it directly (public inheritance, the field is protected) and assemble
 * the correct 16-bit PI and 5-bit PTY. No IRAM footprint: this is plain flash/DRAM
 * code, no IRAM_ATTR. */
class RadioSI4735 : public SI4735 {
public:
    /* Full 16-bit Program Identification from Block A (valid only once Block A has
     * been received; 0 otherwise so callers can treat 0 as "no PI yet"). */
    uint16_t getRdsPIFixed() {
        return (getRdsReceived() && getRdsNewBlockA())
            ? (uint16_t)((currentRdsStatus.resp.BLOCKAH << 8) | currentRdsStatus.resp.BLOCKAL)
            : 0;
    }
    /* 5-bit Program Type from bits 4..0 of the group-type field in Block B. */
    uint8_t getRdsPTYFixed() {
        uint16_t b = (uint16_t)((currentRdsStatus.resp.BLOCKBH << 8) | currentRdsStatus.resp.BLOCKBL);
        return (uint8_t)((b >> 5) & 0x1F);
    }
    /* Wipe the chip's group-0A (station name) and 2A (RadioText) buffers. The base
     * clearRdsBuffer* are protected; expose them so a retune can drop stale text. */
    void clearRdsTextFixed() { clearRdsBuffer0A(); clearRdsBuffer2A(); }
};
static RadioSI4735 rx;
static uint16_t radio_freq;              /* current frequency (FM: 10 kHz units, AM/SSB: kHz) */
static uint8_t  radio_mode;              /* RADIO_MODE_FM | _LSB | _USB | _AM */
static uint8_t  radio_volume = 40;       /* 0..63 */
static bool     radio_ok = false;        /* true once the SI4732 answered on I2C */
static bool     radio_ssb_loaded = false;/* true while the SSB patch is live in chip RAM */
static int      radio_bfo = 0;           /* current BFO offset in Hz (SSB only) */

/*
 * RDS decode state (FM only; ephemeral, never persisted). Filled by the throttled
 * RDS poll in radio_tick() under radio_mtx, read out by radio_get_rds() and
 * /radio/status. rds_ps is the 8-char station name, rds_rt the 64-char RadioText.
 * got_rds latches true once a PS name segment (group 0A) has been captured on the
 * current station;
 * all fields are cleared on every retune / band change / mode change via
 * rds_reset_locked() so a stale name can never survive a move off the frequency.
 * Written only under radio_mtx (or single-tasked at boot). */
static char     rds_ps[9]  = "";         /* PS station name (8 chars + NUL) */
static char     rds_rt[65] = "";         /* RadioText (64 chars + NUL) */
static uint16_t rds_pi     = 0;          /* Program Identification (16-bit) */
static uint8_t  rds_pty    = 0;          /* Program Type (5-bit) */
static bool     got_rds    = false;      /* set once a PS name segment (group 0A) has been captured since the last reset */

/* Clear the local RDS snapshot. Caller holds radio_mtx (or is single-tasked at
 * boot), same contract as apply_*_locked — never nests another lock. */
static void rds_reset_locked() {
    got_rds = false;
    rds_ps[0] = 0;
    rds_rt[0] = 0;
    rds_pi = 0;
    rds_pty = 0;
}

/* EIBI "now broadcasting" cache (AM/SSB only; ephemeral, never persisted). Filled by
 * the throttled EIBI poll in radio_tick() from the loaded offline schedule, read out
 * by radio_get_eibi_now() for the display. Both the poll and the accessor run on the
 * loopTask, so no lock guards these (the schedule file itself is serialised on
 * eibi_mtx inside the lookup). The poll clears them whenever its gate fails (FM, no
 * schedule, no NTP, or no match on the current frequency), so a stale name can never
 * survive a retune or a mode change — mirroring rds_reset_locked() for the PS name. */
static char eibi_now[24]     = "";
static bool eibi_now_valid   = false;

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
 * Per-band, per-sideband SSB calibration: a frequency-trim offset (Hz) folded into
 * the BFO so a given band/sideband can be nudged onto zero-beat. USB and LSB keep
 * separate slots (the ref's usbCal/lsbCal) because the two sidebands mistune in
 * opposite senses. Range +/-RADIO_CAL_MAX Hz, always a multiple of RADIO_CAL_STEP.
 * Seeded to 0 in skill_radio_init() alongside band_freq[]; written only under
 * radio_mtx (or single-tasked at boot). Applied by apply_cal_locked(). SSB-only —
 * an FM/AM band never touches these. Persisted as of v6 (active band only). */
static int16_t  band_usb_cal[BAND_COUNT];
static int16_t  band_lsb_cal[BAND_COUNT];

/*
 * Memory channels. A flat bank of MEMORY_COUNT preset slots, each a full snapshot of a
 * tuned station: frequency (in the band's own units), the band index it belongs to, the
 * live demod mode and the step/bandwidth cursors, plus the SSB sideband calibration trim
 * (0 for FM/AM). freq == 0 marks an empty slot (as in the ref), so the zero-initialised
 * .bss array starts entirely empty. Touched only from the HTTP handlers (save/recall/
 * clear/list); the two that drive the receiver (save/recall) hold radio_mtx around the
 * state/rx access. Persisted to a separate SPIFFS file (MEMORY_STATE_FILE), independent
 * of radio.json — the radio-state schema/version is not affected.
 */
struct MemorySlot {
    uint16_t freq;   /* tuned frequency in the band's units; 0 => empty slot */
    uint8_t  band;   /* band index into bands[] (0..BAND_COUNT-1) */
    uint8_t  mode;   /* RADIO_MODE_FM | _LSB | _USB | _AM */
    uint8_t  step;   /* band_step_idx row for that band's mode */
    uint8_t  bw;     /* band_bw_idx row for that band's mode */
    int16_t  cal;    /* SSB sideband calibration trim (Hz); 0 in FM/AM */
};
#define MEMORY_COUNT 99
#define MEMORY_STATE_FILE    "/memory.json"
#define MEMORY_STATE_VERSION 1
static MemorySlot memories[MEMORY_COUNT];  /* zero-init in .bss -> all slots empty */

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
 * Millisecond timestamp of the last user input. Written on every encoder detent,
 * button event, and HTTP control request (radio_mark_activity), and read for two
 * things: the UI command-timeout in radio_tick() and the idle backlight dimming
 * (radio_idle_ms, consumed by display.cpp). A lone 32-bit word touched from both
 * loopTask and the async HTTP task — an aligned 32-bit load/store is atomic on the
 * ESP32, so no lock guards it (mirrors the other cross-task byte/word atomics here).
 */
static uint32_t last_input_ms = 0;

/*
 * Mark user activity: reset the idle timer so the idle-dimming state machine wakes
 * the backlight. Called from the HTTP control handlers (tune/band/volume/config) so
 * driving the receiver from the web counts as activity and un-dims the screen, the
 * same way an encoder detent or a button press does on the device. A single atomic
 * store — safe to call from the async HTTP task with no lock.
 */
void radio_mark_activity() { last_input_ms = millis(); }

/*
 * Milliseconds since the last user input. Backs display.cpp's idle backlight
 * dimming (full -> dim -> off) — see display_apply_dimming. A lone millis()
 * subtraction over the atomic word above, no lock.
 */
uint32_t radio_idle_ms() { return millis() - last_input_ms; }

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
 * Rotary encoder on PIN_ENC_A/PIN_ENC_B (own MIT quadrature decoder, see Rotary.h). The constructor
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
enum MenuLevel { MENU_MAIN, MENU_SETTINGS, MENU_BAND, MENU_ADJUST, MENU_MEMORY };
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
 * SoftMute on AM/SSB only (a no-op editor showing blanks on an FM band). ADJ_CAL is
 * the SSB calibration trim: numeric +/-2000 Hz in 10 Hz detents, SSB-only (blank +
 * no-op on FM/AM). Its cursor is a 0..400 INDEX (Hz = idx*10 - 2000), not the raw Hz,
 * so the five-row window steps one detent per row (a raw-Hz cursor with a 10 Hz step
 * would leave four of every five rows blank). */
enum AdjustTarget { ADJ_STEP, ADJ_BW, ADJ_MODE, ADJ_SQUELCH, ADJ_AGC, ADJ_AVC, ADJ_SOFTMUTE, ADJ_CAL, ADJ_THEME, ADJ_BRIGHTNESS, ADJ_LAYOUT };
static uint8_t adjust_target = ADJ_STEP;
/* Cursor in the BAND picker (indexes bands[]). Re-seeded from radio_get_band_idx()
 * each time MENU_BAND is entered, so the list always opens on the active band. */
static uint8_t menu_band_idx = 0;

/*
 * MENU_MEMORY is a scrolling list over the 99 memory slots (memories[]). Like the
 * BAND picker it is a cursor list, not a value editor: the encoder moves memory_idx
 * (0..MEMORY_COUNT-1) with no live preview (the receiver is untouched per detent —
 * recall only fires on click), and a click acts on the highlighted slot. An empty
 * slot (freq == 0) is SAVED to (snapshot of the current tuning); an occupied slot is
 * RECALLED onto the receiver. Re-seeded to 0 on entry so the list opens at slot 1. */
static int memory_idx = 0;

/*
 * MAIN-list dispatch keys. The order MUST match menu_main_items[] below so the
 * click handler can switch on the raw cursor (menu_idx) instead of strcmp-ing the
 * label — the label is display text, the index is the contract.
 */
enum MainItem { MI_BAND = 0, MI_MEMORY, MI_MODE, MI_STEP, MI_BW, MI_SQUELCH, MI_MUTE, MI_SETTINGS };

/*
 * SETTINGS-sublist dispatch keys. The order MUST match menu_settings_items[] below,
 * exactly like MainItem/menu_main_items[]: the click handler switches on the raw
 * cursor (menu_settings_idx), never the label text. AGC/AVC/SoftMute/Calibration and
 * Brightness/Theme/Layout drop into their adjust editors; About is not a leaf yet;
 * Back exits.
 */
enum SettingsItem { SI_AGC = 0, SI_AVC, SI_SOFTMUTE, SI_CAL, SI_BRIGHTNESS, SI_THEME, SI_LAYOUT, SI_EIBI, SI_ABOUT, SI_BACK };

static const char *const menu_main_items[] = {
    "Band", "Memory", "Mode", "Step", "Bandwidth", "Squelch", "Mute", "Settings"
};
static const char *const menu_settings_items[] = {
    "AGC", "AVC", "SoftMute", "Calibration", "Brightness", "Theme", "Layout", "EIBI Load", "About", "Back"
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
    {"GET",  "/radio/bands",  "List band-plan presets for UI: {bands:[{idx,name}],current}"},
    {"GET",  "/radio/themes", "List colour themes for UI: {themes:[{idx,name}],current}"},
    {"GET",  "/radio/layouts","List screen layouts for UI: {layouts:[{idx,name}],current}"},
    {"POST", "/radio/config", "Set mode/step/bandwidth/squelch/mute/DSP/theme/layout/sleep: {mode?:\"AM\"|\"LSB\"|\"USB\", step_idx?:<int>, bw_idx?:<int>, squelch?:0-127, squelch_snr?:<bool>, mute?:<bool>, agc?:<int>, avc?:<even 12-90>, softmute?:0-32, cal?:<-2000-2000 SSB>, theme?:<int>, layout?:0-1, sleep?:<0-3600 s, 0=off>}"},
    {"POST", "/radio/volume", "Set volume: {volume:0-63}"},
    {"POST", "/radio/scan",   "Sweep current-mode band: {from,to,step,min_rssi?} -> RSSI/SNR per step"},
    {"GET",  "/radio/status", "Current freq/mode/RSSI/SNR (+bfo in SSB, +RDS ps/rt/pi/pty in FM)"},
    {"GET",  "/radio/memory", "List occupied memory slots: {count, slots:[{slot,band,freq,mode,freq_display}]}"},
    {"POST", "/radio/memory", "Save/recall/clear a memory slot: {slot:1-99, action:\"save\"|\"recall\"|\"clear\"}"},
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
           "| GET | /radio/bands | List band-plan presets (name + index) for UI dropdowns |\n"
           "| GET | /radio/themes | List colour themes (name + index) for the UI selector |\n"
           "| GET | /radio/layouts | List screen layouts (name + index) for the UI selector |\n"
           "| POST | /radio/config | Set mode/step/bandwidth + squelch/mute + DSP + theme + layout + sleep: `{\"mode\":\"AM|LSB|USB\",\"step_idx\":<int>,\"bw_idx\":<int>,\"squelch\":<0..127>,\"squelch_snr\":<bool>,\"mute\":<bool>,\"agc\":<int>,\"avc\":<even 12..90>,\"softmute\":<0..32>,\"cal\":<-2000..2000>,\"theme\":<int>,\"layout\":<0..1>,\"sleep\":<0..3600 s, 0=off>}` |\n"
           "| POST | /radio/volume | Set volume: `{\"volume\":<0..63>}` |\n"
           "| POST | /radio/scan | Blocking band sweep in the current mode: `{\"from\":<int>,\"to\":<int>,\"step\":<int>,\"min_rssi\":<int>}` |\n"
           "| GET | /radio/status | Current freq, mode, RSSI, SNR (+bfo in SSB; +RDS `rds_ps`/`rds_rt`/`pi`/`pty` in FM) |\n"
           "| GET | /radio/memory | List occupied memory slots |\n"
           "| POST | /radio/memory | Save/recall/clear a memory slot |\n\n"
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
           "  out-of-range index is a 400. Response: `{\"ok\",\"band\",\"mode\",\"freq\",\"freq_display\"}`.\n"
           "- `GET /radio/bands` returns the preset list for building a UI selector:\n"
           "  `{\"bands\":[{\"idx\":<int>,\"name\":<str>}],\"current\":<idx>}`.\n\n"
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
           "### SSB calibration (`cal`)\n\n"
           "`POST /radio/config` `{\"cal\":<-2000..2000>}` sets a per-band, per-sideband\n"
           "frequency trim (Hz) folded into the BFO, so each band/sideband can be nudged\n"
           "onto zero-beat. SSB-only (rejected in FM/AM); kept separately for LSB and USB\n"
           "and per band; reported by `/radio/status` for the active sideband.\n\n"
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
           "### Memory channels\n\n"
           "A bank of 99 preset slots, numbered `1`..`99`, each a full snapshot of a\n"
           "tuned station (band, frequency, mode, step, bandwidth and SSB calibration).\n"
           "The bank persists to flash (separate from the tuning state).\n\n"
           "- `GET /radio/memory` lists the occupied slots only:\n"
           "  `{\"count\":<int>,\"slots\":[{\"slot\":<1..99>,\"band\":<name>,\"freq\":<int>,\"mode\":<str>,\"freq_display\":<str>}]}`.\n"
           "- `POST /radio/memory` `{\"slot\":<1..99>,\"action\":\"save|recall|clear\"}`:\n"
           "  - `save` stores the current tuning into the slot (overwrites).\n"
           "  - `recall` retunes the receiver to the slot; an empty slot is a 409.\n"
           "  - `clear` empties the slot.\n"
           "  An out-of-range `slot` or an unknown `action` is a 400.\n\n"
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

/* True once the SI4732 answered on I2C at boot (see skill_radio_init). The display
 * gates the frequency/mode draw on this: with no chip, radio_freq/radio_mode are
 * still their file-static zero (== FM 0 kHz), so an ungated draw paints a bogus
 * "FM 0.00 MHz". A lone bool read, no lock. */
bool radio_chip_present() { return radio_ok; }

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
        case MENU_MEMORY:   return memory_idx;
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
            if (adjust_target == ADJ_CAL) {
                /* Cursor is a 0..400 INDEX into the +/-RADIO_CAL_MAX Hz / RADIO_CAL_STEP
                 * grid (Hz = idx*step - max). SSB-only: on FM/AM item() blanks every
                 * row, so 0 is a harmless placeholder cursor there. */
                if (!radio_is_ssb(m)) return 0;
                int16_t cal = (m == RADIO_MODE_USB) ? band_usb_cal[radio_band_idx]
                                                    : band_lsb_cal[radio_band_idx];
                return (cal + RADIO_CAL_MAX) / RADIO_CAL_STEP;
            }
            /* ADJ_THEME: cursor IS the global theme index (display owns it). */
            if (adjust_target == ADJ_THEME)
                return display_get_theme();
            /* ADJ_BRIGHTNESS: cursor IS the backlight duty 10..255 (display owns it). */
            if (adjust_target == ADJ_BRIGHTNESS)
                return display_get_brightness();
            /* ADJ_LAYOUT: cursor IS the global layout index (display owns it). */
            if (adjust_target == ADJ_LAYOUT)
                return display_get_layout();
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
        case MENU_MEMORY:   return MEMORY_COUNT;
        case MENU_ADJUST:
            if (adjust_target == ADJ_STEP) return stepCount(band_table_mode());
            if (adjust_target == ADJ_BW)   return bwCount(band_table_mode());
            if (adjust_target == ADJ_SQUELCH) return 128;  /* 0..127 threshold */
            if (adjust_target == ADJ_AGC) return agc_max_for_mode(band_table_mode()) + 1;
            if (adjust_target == ADJ_AVC) return 91;   /* 0..90 window; even-only in item()/scroll */
            if (adjust_target == ADJ_SOFTMUTE) return 33;  /* 0..32 */
            /* +/-RADIO_CAL_MAX Hz in RADIO_CAL_STEP detents -> 0..400 index, 401 rows. */
            if (adjust_target == ADJ_CAL) return 2 * RADIO_CAL_MAX / RADIO_CAL_STEP + 1;
            if (adjust_target == ADJ_THEME) return display_theme_count();
            if (adjust_target == ADJ_LAYOUT) return display_layout_count();
            /* Brightness spans 0..255 nominal; item() blanks below the 10 floor. */
            if (adjust_target == ADJ_BRIGHTNESS) return 256;
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
        case MENU_MEMORY:   return "Memory";
        case MENU_ADJUST:
            if (adjust_target == ADJ_STEP) return "Step";
            if (adjust_target == ADJ_BW)   return "Bandwidth";
            if (adjust_target == ADJ_SQUELCH) return "Squelch";
            if (adjust_target == ADJ_AGC) return "AGC";
            if (adjust_target == ADJ_AVC) return "AVC";
            if (adjust_target == ADJ_SOFTMUTE) return "SoftMute";
            if (adjust_target == ADJ_CAL) return "Cal Hz";
            if (adjust_target == ADJ_THEME) return "Theme";
            if (adjust_target == ADJ_BRIGHTNESS) return "Brightness";
            if (adjust_target == ADJ_LAYOUT) return "Layout";
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
        if (adjust_target == ADJ_CAL) {
            /* `i` is the absolute 0..400 index candidate; map it back to signed Hz
             * (idx*step - max) and show it with a sign. Blank past the index ends and
             * on any non-SSB band (cal is SSB-only). */
            static char buf[8];
            int last = 2 * RADIO_CAL_MAX / RADIO_CAL_STEP;  /* 400 */
            if (!radio_is_ssb(band_table_mode()) || i < 0 || i > last) return "";
            snprintf(buf, sizeof(buf), "%+d", i * RADIO_CAL_STEP - RADIO_CAL_MAX);
            return buf;
        }
        if (adjust_target == ADJ_THEME) {
            /* Theme names come from display.cpp; wrap the window index modulo the
             * count like the band list so the caller can ask cursor-2..cursor+2. */
            int n = display_theme_count();
            return display_theme_name((uint8_t)(((i % n) + n) % n));
        }
        if (adjust_target == ADJ_BRIGHTNESS) {
            /* Absolute duty candidate; render it directly, blanking rows below the
             * 10 floor or past 255 (same window style as ADJ_SQUELCH). */
            static char buf[8];
            if (i < 10 || i > 255) return "";
            snprintf(buf, sizeof(buf), "%d", i);
            return buf;
        }
        if (adjust_target == ADJ_LAYOUT) {
            /* Layout names come from display.cpp; wrap the window index modulo the
             * count like the theme list so the caller can ask cursor-2..cursor+2. */
            int n = display_layout_count();
            return display_layout_name((uint8_t)(((i % n) + n) % n));
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
        int wi = ((i % n) + n) % n;
        /* The EIBI row reflects load state (idle/loading/loaded-count/error) instead
         * of a fixed label, so the offline-of-HTTP user gets feedback on the action.
         * eibi_menu_label reads only RAM (no flash), safe under the caller's display_mtx.
         * The window shows <=5 rows over a 10-entry list, so SI_EIBI appears at most
         * once per draw — one static buffer cannot be clobbered mid-frame. */
        if (wi == SI_EIBI) {
            static char eibi_label[16];
            eibi_menu_label(eibi_label, sizeof(eibi_label));
            return eibi_label;
        }
        return menu_settings_items[wi];
    }
    if (menu_level == MENU_MEMORY) {
        /* Slot row: "NN <freq>" for an occupied slot (freq reuses radio_format_freq,
         * which folds the SSB sideband into the string), "NN ---" for an empty one.
         * Slots are shown 1-based (index + 1) to match the HTTP API's 1..99 numbering.
         * `i` is the absolute window candidate; wrap it modulo the bank like the other
         * lists. One static buffer is safe: draw_menu_screen consumes each returned
         * string before asking for the next row. */
        int n = MEMORY_COUNT;
        int slot = ((i % n) + n) % n;
        static char buf[32];
        const MemorySlot &ms = memories[slot];
        if (ms.freq == 0) {
            snprintf(buf, sizeof(buf), "%2d ---", slot + 1);
        } else {
            char fd[24];
            radio_format_freq(ms.freq, ms.mode, fd, sizeof(fd));
            snprintf(buf, sizeof(buf), "%2d %s", slot + 1, fd);
        }
        return buf;
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
 * Copy out the current RDS snapshot for the display (FM only; consumed by C2). Takes
 * radio_mtx briefly to copy a coherent snapshot, mirroring radio_get_signal — the
 * caller (the TFT task) must grab this BEFORE any display_mtx to keep the lock order
 * radio_mtx -> display_mtx. Any out pointer may be NULL. *valid reports whether any
 * RDS has been decoded on the current station; off FM the snapshot is empty/false. */
void radio_get_rds(char *ps, size_t ps_len, char *rt, size_t rt_len,
                   uint16_t *pi, uint8_t *pty, bool *valid) {
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    if (ps && ps_len) { strncpy(ps, rds_ps, ps_len - 1); ps[ps_len - 1] = 0; }
    if (rt && rt_len) { strncpy(rt, rds_rt, rt_len - 1); rt[rt_len - 1] = 0; }
    if (pi)    *pi    = rds_pi;
    if (pty)   *pty   = rds_pty;
    if (valid) *valid = got_rds;
    xSemaphoreGive(radio_mtx);
}

/*
 * Copy out the current EIBI "now broadcasting" station name for the display (AM/SSB
 * only). Unlike radio_get_rds this takes NO lock: eibi_now is filled/cleared solely
 * by radio_tick's throttled poll, and this accessor is called from the same loopTask
 * (display_tick_render), so the read is single-threaded. *valid is true only when a
 * schedule match was resolved on the current frequency. buf may be NULL.
 */
void radio_get_eibi_now(char *buf, size_t len, bool *valid) {
    if (buf && len) { strncpy(buf, eibi_now, len - 1); buf[len - 1] = 0; }
    if (valid) *valid = eibi_now_valid;
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
 * The calibration slot for the CURRENTLY-RUNNING sideband on the active band: USB
 * reads band_usb_cal[], every other mode reads band_lsb_cal[] (only meaningful in
 * LSB — callers gate on radio_is_ssb(radio_mode)). Keyed off the live radio_mode
 * because cal folds into the BFO of the demod actually on air. Plain aligned read;
 * mutating callers hold radio_mtx.
 */
static int16_t *radio_current_cal() {
    return (radio_mode == RADIO_MODE_USB) ? &band_usb_cal[radio_band_idx]
                                          : &band_lsb_cal[radio_band_idx];
}

/*
 * Fold the active band/sideband's calibration trim into the BFO and push it to the
 * chip: setSSBBfo(-(radio_bfo + cal)). This is the ONE place setSSBBfo is called —
 * every SSB set-up/tune/restore/edit routes through here so the sign convention and
 * the cal term stay in a single spot. A no-op outside SSB (cal is meaningless in
 * FM/AM). Reads the LIVE radio_mode/radio_band_idx/radio_bfo, so the caller must
 * have those already updated to the intended target. Caller holds radio_mtx.
 */
static void apply_cal_locked() {
    if (!radio_is_ssb(radio_mode)) return;
    int16_t cal = *radio_current_cal();
    rx.setSSBBfo(-(radio_bfo + cal));
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
        /* Zero the BFO here; the calibration trim is folded in by apply_cal_locked()
         * at the tail, once radio_mode/radio_band_idx below name the new target. */
        radio_bfo = 0;
        rds_reset_locked();  /* no RDS off FM: drop any stale snapshot */
    } else if (mode == RADIO_MODE_FM) {
        rx.setFM(b->minFreq, b->maxFreq, freq, stepsForMode(mode)[sidx].step);
        /* Match /radio/tune's post-setFM config: 50 us de-emphasis. The FM AGC is
         * (re)applied from the per-mode store in the unified DSP block below. */
        rx.setFMDeEmphasis(1);
        /* Bring up RDS on this FM tune. RdsInit clears the chip's RDS buffers;
         * setRdsConfig(RDSEN=1, block-error thresholds 2 = accept up to ~5 bit
         * errors, matching the reference) enables decode. Then wipe our snapshot. */
        rx.RdsInit();
        rx.setRdsConfig(1, 2, 2, 2, 2);
        rds_reset_locked();
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
    } else {  /* AM (MW / SW broadcast) */
        rx.setAM(b->minFreq, b->maxFreq, freq, stepsForMode(mode)[sidx].step);
        radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        radio_bfo = 0;
        rds_reset_locked();  /* no RDS off FM: drop any stale snapshot */
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

    /* SSB only: now that the three live-state words above name the new band/sideband,
     * fold in that slot's calibration trim (BFO was zeroed to 0 in the SSB branch, so
     * this applies -(0 + cal)). setSSB reset the BFO, so it must be re-pushed here. */
    if (radio_is_ssb(mode)) apply_cal_locked();
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

/* --- Memory channels: snapshot / recall / clear + SPIFFS persistence --- */

/*
 * Snapshot the current tuning into memory slot i (0..MEMORY_COUNT-1). Under radio_mtx
 * so the read of the live band/freq/step/bw/cal is coherent with a concurrent encoder
 * tune. The stored mode is the band's live demod (band_mode[]), and the cal is the
 * matching sideband slot for SSB (0 otherwise). Does not touch flash — the caller flushes
 * the bank via memory_store_save() after the lock is released.
 */
static void radio_memory_save(uint8_t i) {
    if (i >= MEMORY_COUNT) return;
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    uint8_t b = radio_band_idx;
    uint8_t m = band_mode[b];
    int16_t cal = 0;
    if (radio_is_ssb(m))
        cal = (m == RADIO_MODE_USB) ? band_usb_cal[b] : band_lsb_cal[b];
    memories[i].freq = radio_freq;
    memories[i].band = b;
    memories[i].mode = m;
    memories[i].step = band_step_idx[b];
    memories[i].bw   = band_bw_idx[b];
    memories[i].cal  = cal;
    xSemaphoreGive(radio_mtx);
}

/*
 * Recall memory slot i onto the receiver. Returns false when the slot is empty or holds
 * a corrupt band/mode. Mirrors radio_select_band: saves the outgoing band's live freq
 * first, seeds the target band's mode/step/bw/cal from the slot, clips the stored freq to
 * the band's window and drives the chip through apply_band_locked (which handles the SSB
 * patch load/unload and folds the cal into the BFO). Under radio_mtx; marks state dirty.
 */
static bool radio_recall_memory(uint8_t i) {
    if (i >= MEMORY_COUNT) return false;
    MemorySlot m = memories[i];
    if (m.freq == 0) return false;                 /* empty slot */
    if (m.band >= BAND_COUNT) return false;        /* corrupt band index */
    if (m.mode != RADIO_MODE_FM && m.mode != RADIO_MODE_LSB &&
        m.mode != RADIO_MODE_USB && m.mode != RADIO_MODE_AM) return false;

    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    /* Remember where we were on the band we are leaving. */
    band_freq[radio_band_idx] = radio_freq;
    /* Seed the target band's live state from the slot before apply_band_locked reads it. */
    band_mode[m.band]     = m.mode;
    band_step_idx[m.band] = clamp_idx(m.step, stepCount(m.mode));
    band_bw_idx[m.band]   = clamp_idx(m.bw, bwCount(m.mode));
    if (radio_is_ssb(m.mode)) {
        if (m.mode == RADIO_MODE_USB) band_usb_cal[m.band] = m.cal;
        else                          band_lsb_cal[m.band] = m.cal;
    }
    /* Clip the stored frequency to the target band's window. */
    uint16_t f = m.freq;
    if (f < bands[m.band].minFreq) f = bands[m.band].minFreq;
    if (f > bands[m.band].maxFreq) f = bands[m.band].maxFreq;
    apply_band_locked(m.band, f);
    radio_mark_dirty();
    xSemaphoreGive(radio_mtx);
    return true;
}

/*
 * Clear memory slot i (mark it empty). freq == 0 is the empty sentinel; the radio_mtx
 * hold is only for consistency with the other memories[] writers (a single aligned store
 * would be atomic anyway). The flash flush is the caller's job (memory_store_save()).
 */
static void radio_memory_clear(uint8_t i) {
    if (i >= MEMORY_COUNT) return;
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    memories[i].freq = 0;
    xSemaphoreGive(radio_mtx);
}

/*
 * Serialise the occupied slots to MEMORY_STATE_FILE. Compact schema: only non-empty
 * slots, keyed by slot index "0".."98", each a [freq,band,mode,step,bw,cal] array. The
 * bank is snapshotted into the JSON under a short radio_mtx hold so a concurrent save/
 * clear can't tear a slot; the blocking flash write (write_spiffs_file, which detaches the
 * encoder itself) then runs OUTSIDE the lock. Called after every save/clear.
 */
static void memory_store_save() {
    JsonDocument doc;
    doc["v"] = MEMORY_STATE_VERSION;
    JsonObject slots = doc["slots"].to<JsonObject>();
    char key[4];
    xSemaphoreTake(radio_mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < MEMORY_COUNT; i++) {
        if (memories[i].freq == 0) continue;
        snprintf(key, sizeof(key), "%u", (unsigned)i);
        JsonArray a = slots[key].to<JsonArray>();
        a.add(memories[i].freq);
        a.add(memories[i].band);
        a.add(memories[i].mode);
        a.add(memories[i].step);
        a.add(memories[i].bw);
        a.add(memories[i].cal);
    }
    xSemaphoreGive(radio_mtx);
    String out;
    serializeJson(doc, out);
    write_spiffs_file(MEMORY_STATE_FILE, out);  /* blocking flash write + encoder detach */
}

/*
 * Load the memory bank from MEMORY_STATE_FILE over the empty default. Runs once at boot
 * from skill_radio_init(), single-tasked (server not up yet), so no lock. A missing/
 * unparseable file or a version mismatch leaves every slot empty. Each slot is validated:
 * a bad band, mode or a zero/oversized freq drops that slot to empty, the freq is clipped
 * to the band's window and the cal is zeroed on a non-SSB mode.
 */
static void memory_store_load() {
    String raw = read_spiffs_file(MEMORY_STATE_FILE);
    if (raw.length() == 0) return;  /* no saved bank -> keep all slots empty */

    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
    if ((int)(doc["v"] | 0) != MEMORY_STATE_VERSION) return;

    JsonObjectConst slots = doc["slots"].as<JsonObjectConst>();
    if (slots.isNull()) return;

    for (JsonPairConst kv : slots) {
        int idx = atoi(kv.key().c_str());
        if (idx < 0 || idx >= MEMORY_COUNT) continue;
        JsonArrayConst a = kv.value().as<JsonArrayConst>();
        if (a.isNull() || a.size() < 6) continue;
        long freq = a[0] | 0L;
        int  band = a[1] | -1;
        int  mode = a[2] | -1;
        int  step = a[3] | 0;
        int  bw   = a[4] | 0;
        int  cal  = a[5] | 0;
        /* Validate; a bad slot stays empty rather than poisoning the bank. */
        if (freq <= 0 || freq > 0xFFFF) continue;
        if (band < 0 || band >= BAND_COUNT) continue;
        if (mode != RADIO_MODE_FM && mode != RADIO_MODE_LSB &&
            mode != RADIO_MODE_USB && mode != RADIO_MODE_AM) continue;
        /* Clip the stored freq to the band's window. */
        uint16_t f = (uint16_t)freq;
        if (f < bands[band].minFreq) f = bands[band].minFreq;
        if (f > bands[band].maxFreq) f = bands[band].maxFreq;
        if (cal < -RADIO_CAL_MAX) cal = -RADIO_CAL_MAX;
        if (cal >  RADIO_CAL_MAX) cal =  RADIO_CAL_MAX;
        memories[idx].freq = f;
        memories[idx].band = (uint8_t)band;
        memories[idx].mode = (uint8_t)mode;
        memories[idx].step = clamp_idx((uint8_t)step, stepCount((uint8_t)mode));
        memories[idx].bw   = clamp_idx((uint8_t)bw, bwCount((uint8_t)mode));
        memories[idx].cal  = radio_is_ssb((uint8_t)mode) ? (int16_t)cal : 0;
    }
}

static void radio_register_routes(AsyncWebServer &server) {

    /* POST /radio/tune */
    server.on("/radio/tune", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) {
            /* Body callback already ran; free the collected buffer before bailing. */
            if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
            return;
        }
        radio_mark_activity();  /* web control counts as input -> wake the backlight */

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
            /* Enable RDS decode for this FM station (see apply_band_locked). */
            rx.RdsInit();
            rx.setRdsConfig(1, 2, 2, 2, 2);
            rds_reset_locked();
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
            rds_reset_locked();  /* no RDS off FM: drop any stale snapshot */
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
            /* Stash the requested BFO; the actual setSSBBfo (with the band/sideband
             * calibration folded in) runs in the common block below, once radio_mode
             * names the new sideband so apply_cal_locked() picks the right cal slot. */
            radio_bfo = req_bfo;
            rds_reset_locked();  /* no RDS off FM: drop any stale snapshot */
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
        /* SSB: apply the BFO now, with the current band's per-sideband cal folded in,
         * now that radio_mode names the sideband apply_cal_locked() reads. */
        if (radio_is_ssb(mode)) apply_cal_locked();
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
        radio_mark_activity();  /* web control counts as input -> wake the backlight */

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

    /* GET /radio/bands — the band-plan preset list for UI dropdowns: each name with
     * its index, plus the currently selected band. Uses the public band accessors;
     * no receiver access, so no radio_ok gate. */
    server.on("/radio/bands", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc["bands"].to<JsonArray>();
        uint8_t n = radio_get_band_count();
        for (uint8_t i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["idx"] = i;
            o["name"] = radio_get_band_name_at(i);
        }
        doc["current"] = radio_get_band_idx();
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* GET /radio/themes — the colour-theme list for the UI selector: each name with
     * its index, plus the current theme. The palette lives in display.cpp; this reads
     * it through the public accessors, so no receiver access and no radio_ok gate. */
    server.on("/radio/themes", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc["themes"].to<JsonArray>();
        uint8_t n = display_theme_count();
        for (uint8_t i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["idx"] = i;
            o["name"] = display_theme_name(i);
        }
        doc["current"] = display_get_theme();
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* GET /radio/layouts — the screen-layout list for the UI selector: each name with
     * its index, plus the current layout. Mirrors /radio/themes; the layout lives in
     * display.cpp and is read through the public accessors (no receiver access). */
    server.on("/radio/layouts", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc["layouts"].to<JsonArray>();
        uint8_t n = display_layout_count();
        for (uint8_t i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["idx"] = i;
            o["name"] = display_layout_name(i);
        }
        doc["current"] = display_get_layout();
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

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
        radio_mark_activity();  /* web control counts as input -> wake the backlight */

        /* No radio_ok gate here: the "theme" field is a global DISPLAY property and
         * must work even when the SI4732 was never detected. The receiver-affecting
         * fields are gated on radio_ok further down, once we know one was requested. */
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

        /* Optional SSB calibration trim: "cal" (Hz, -RADIO_CAL_MAX..RADIO_CAL_MAX,
         * SSB-only). Range and SSB gating validated under the lock once the effective
         * mode is known. */
        bool has_cal = !input["cal"].isNull();
        int  cal_v   = input["cal"] | 0;

        /* Optional global colour theme: "theme" (0..count-1). A DISPLAY property, not a
         * receiver setting — applied without radio_ok and without radio_mtx (the palette
         * is guarded by display_mtx inside display_set_theme). */
        bool has_theme = !input["theme"].isNull();
        int  theme_v   = input["theme"] | -1;

        /* Optional global screen layout: "layout" (0..count-1). A DISPLAY property like
         * the theme — applied without radio_ok and without radio_mtx (the layout is
         * guarded by display_mtx inside display_set_layout). */
        bool has_layout = !input["layout"].isNull();
        int  layout_v   = input["layout"] | -1;

        /* Optional idle-dim threshold: "sleep" (seconds, 0 = never dim; the backlight
         * steps to ~20% at this idle time and off at 3x it). A DISPLAY property like
         * theme/layout — applied without radio_ok and without radio_mtx (guarded by
         * display_mtx inside display_set_sleep). Never a real sleep: WiFi/HTTP/mesh
         * stay up, only the backlight PWM changes. */
        bool has_sleep = !input["sleep"].isNull();
        int  sleep_v   = input["sleep"] | -1;

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
            !has_agc && !has_avc && !has_sm && !has_cal && !has_theme && !has_layout &&
            !has_sleep) {
            req->send(400, "application/json",
                "{\"error\":\"provide step_idx, bw_idx, mode, squelch, mute, agc, avc, softmute, cal, theme, layout and/or sleep\"}");
            return;
        }
        if (mode_str_bad) {
            req->send(400, "application/json",
                "{\"error\":\"mode must be AM, LSB or USB\"}");
            return;
        }

        /* Apply the global theme BEFORE the radio_ok gate: it is a display property, so
         * a theme change must take effect even when the receiver is absent. Validate the
         * range first, then set it under display_mtx (inside display_set_theme) — never
         * under radio_mtx, to avoid nesting the two locks. Persisted (v7). */
        if (has_theme) {
            if (theme_v < 0 || theme_v >= (int)display_theme_count()) {
                char err[48];
                snprintf(err, sizeof(err),
                    "{\"error\":\"theme out of range (0..%d)\"}", display_theme_count() - 1);
                req->send(400, "application/json", err);
                return;
            }
            display_set_theme((uint8_t)theme_v);
            radio_mark_dirty();
        }

        /* Apply the global layout BEFORE the radio_ok gate too — a display property,
         * so it must take effect even with the receiver absent. Validate the range,
         * then set it under display_mtx (inside display_set_layout), never under
         * radio_mtx. Persisted (v8). */
        if (has_layout) {
            if (layout_v < 0 || layout_v >= (int)display_layout_count()) {
                char err[52];
                snprintf(err, sizeof(err),
                    "{\"error\":\"layout out of range (0..%d)\"}", display_layout_count() - 1);
                req->send(400, "application/json", err);
                return;
            }
            display_set_layout((uint8_t)layout_v);
            radio_mark_dirty();
        }

        /* Apply the idle-dim threshold BEFORE the radio_ok gate too — a display
         * property, so it must take effect even with the receiver absent. Validate the
         * range (0..3600 s), then set it under display_mtx (inside display_set_sleep),
         * never under radio_mtx. Persisted (v8, tolerant). */
        if (has_sleep) {
            if (sleep_v < 0 || sleep_v > 3600) {
                req->send(400, "application/json",
                    "{\"error\":\"sleep out of range (0..3600 s, 0=off)\"}");
                return;
            }
            display_set_sleep((uint16_t)sleep_v);
            radio_mark_dirty();
        }

        /* If the only fields were display ones (theme/layout/sleep), respond now
         * without touching the receiver — this path succeeds regardless of radio_ok.
         * Any receiver-affecting field falls through to the radio_ok gate below. */
        bool has_radio_field = has_step || has_bw || has_mode || has_squelch ||
                               has_mute || has_agc || has_avc || has_sm || has_cal;
        if (!has_radio_field) {
            JsonDocument doc;
            doc["ok"] = true;
            doc["theme"] = display_get_theme();
            doc["layout"] = display_get_layout();
            doc["sleep"] = display_get_sleep();
            doc["dim_state"] = display_dim_state();
            String response;
            serializeJson(doc, response);
            req->send(200, "application/json", response);
            return;
        }
        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
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
        /* Cal is SSB-only (never FM/AM) and clamped to +/-RADIO_CAL_MAX. */
        bool cal_notssb = has_cal && !radio_is_ssb(mode);
        bool cal_bad    = has_cal && (cal_v < -RADIO_CAL_MAX || cal_v > RADIO_CAL_MAX);
        if (!mode_locked && !step_bad && !bw_bad && !squelch_bad &&
            !agc_bad && !avc_fm && !avc_bad && !sm_fm && !sm_bad &&
            !cal_notssb && !cal_bad) {
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
            /* SSB calibration: store into the effective sideband's slot and fold it into
             * the BFO now. Gated to SSB above (cal_notssb), so `mode` is LSB/USB here. */
            if (has_cal) {
                if (mode == RADIO_MODE_USB) band_usb_cal[bnd] = (int16_t)cal_v;
                else                        band_lsb_cal[bnd] = (int16_t)cal_v;
                apply_cal_locked();
            }
            /* Step/bw/mode, the squelch gate (threshold + metric), the per-mode DSP
             * scalars and the SSB cal are persisted (v4/v5/v6); MAIN mute is ephemeral,
             * so only a change to a persisted parameter schedules a flash write. */
            if (has_step || has_bw || has_mode || has_squelch || has_sqmetric ||
                has_agc || has_avc || has_sm || has_cal) radio_mark_dirty();
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
        bool mode_is_ssb = radio_is_ssb(mode);
        int16_t cal_snap = mode_is_ssb ? (mode == RADIO_MODE_USB ? band_usb_cal[bnd]
                                                                 : band_lsb_cal[bnd])
                                       : 0;
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
        if (cal_notssb) {
            req->send(400, "application/json",
                "{\"error\":\"cal only in SSB (LSB/USB)\"}");
            return;
        }
        if (cal_bad) {
            req->send(400, "application/json",
                "{\"error\":\"cal out of range (-2000..2000)\"}");
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
        /* Cal is SSB-only; report it only in LSB/USB. */
        if (mode_is_ssb) doc["cal"] = cal_snap;
        /* Theme, layout and idle-dim are global; always report the current values. */
        doc["theme"] = display_get_theme();
        doc["layout"] = display_get_layout();
        doc["sleep"] = display_get_sleep();
        doc["dim_state"] = display_dim_state();
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
        radio_mark_activity();  /* web control counts as input -> wake the backlight */

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
        /* SSB calibration for the live sideband on the active band (SSB-only). */
        int16_t cal_snap = radio_is_ssb(lmode)
                               ? (lmode == RADIO_MODE_USB ? band_usb_cal[radio_band_idx]
                                                          : band_lsb_cal[radio_band_idx])
                               : 0;
        /* RDS snapshot, copied under the same hold (FM only; reported below). char[]
         * copies keep ArduinoJson from aliasing the file-local buffers. */
        char rds_ps_snap[9]; char rds_rt_snap[65];
        strncpy(rds_ps_snap, rds_ps, sizeof(rds_ps_snap)); rds_ps_snap[8] = 0;
        strncpy(rds_rt_snap, rds_rt, sizeof(rds_rt_snap)); rds_rt_snap[64] = 0;
        uint16_t rds_pi_snap = rds_pi;
        uint8_t  rds_pty_snap = rds_pty;
        xSemaphoreGive(radio_mtx);

        char freq_display[24];
        radio_format_freq(radio_freq, radio_mode, freq_display, sizeof(freq_display));

        JsonDocument doc;
        doc["mode"] = radio_mode_str(radio_mode);
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        if (radio_is_ssb(radio_mode)) doc["bfo"] = radio_bfo;
        if (radio_is_ssb(radio_mode)) doc["cal"] = cal_snap;
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
        /* Global colour theme (a display property, not gated by receiver mode). */
        doc["theme"] = display_get_theme();
        doc["theme_name"] = display_theme_name(display_get_theme());
        doc["layout"] = display_get_layout();
        doc["layout_name"] = display_layout_name(display_get_layout());
        /* Idle backlight dimming: the configured threshold (seconds, 0=off) and the
         * live stage ("full"/"dim"/"off"). The physical backlight is invisible over
         * HTTP, so dim_state is how an agent verifies the state machine. */
        doc["sleep"] = display_get_sleep();
        doc["dim_state"] = display_dim_state();
        /* RDS is an FM-broadcast feature; report the decoded fields only in FM. pi is
         * the numeric 16-bit Program Identification, pty the 5-bit Program Type. */
        if (lmode_fm) {
            doc["rds_ps"] = rds_ps_snap;
            doc["rds_rt"] = rds_rt_snap;
            doc["pi"] = rds_pi_snap;
            doc["pty"] = rds_pty_snap;
        }
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* GET /radio/memory — list the occupied memory slots, compact (only non-empty).
     * Slot numbers are exposed 1..MEMORY_COUNT (the internal index + 1). Reads the bank
     * under radio_mtx; no receiver access, so no radio_ok gate. */
    server.on("/radio/memory", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc["slots"].to<JsonArray>();
        int count = 0;
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        for (uint8_t i = 0; i < MEMORY_COUNT; i++) {
            if (memories[i].freq == 0) continue;
            MemorySlot m = memories[i];
            JsonObject o = arr.add<JsonObject>();
            o["slot"] = i + 1;  /* 1..MEMORY_COUNT in the API */
            o["band"] = (m.band < BAND_COUNT) ? bands[m.band].name : "?";
            o["freq"] = m.freq;
            o["mode"] = radio_mode_str(m.mode);
            char fd[24];
            radio_format_freq(m.freq, m.mode, fd, sizeof(fd));
            o["freq_display"] = fd;
            count++;
        }
        xSemaphoreGive(radio_mtx);
        doc["count"] = count;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* POST /radio/memory — save/recall/clear a memory slot: {slot:1..MEMORY_COUNT,
     * action:"save"|"recall"|"clear"}. save/clear rewrite the SPIFFS bank (outside
     * radio_mtx); recall drives the receiver through radio_recall_memory. */
    server.on("/radio/memory", HTTP_POST, [](AsyncWebServerRequest *req) {
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

        int slot = input["slot"] | -1;
        const char *action = input["action"] | (const char*)nullptr;

        free(body); req->_tempObject = nullptr;

        if (slot < 1 || slot > MEMORY_COUNT) {
            req->send(400, "application/json",
                "{\"error\":\"slot out of range (1..99)\"}");
            return;
        }
        if (!action) {
            req->send(400, "application/json",
                "{\"error\":\"action required (save|recall|clear)\"}");
            return;
        }
        uint8_t idx = (uint8_t)(slot - 1);

        if (strcmp(action, "save") == 0) {
            radio_memory_save(idx);
            memory_store_save();  /* flush the bank to flash (outside radio_mtx) */
            event_add("radio: memory %d saved", slot);
            JsonDocument doc;
            doc["ok"] = true;
            doc["slot"] = slot;
            doc["action"] = "save";
            String response;
            serializeJson(doc, response);
            req->send(200, "application/json", response);
        } else if (strcmp(action, "recall") == 0) {
            if (!radio_recall_memory(idx)) {
                req->send(409, "application/json", "{\"error\":\"slot empty\"}");
                return;
            }
            event_add("radio: memory %d recalled", slot);
            display_show_status();
            char fd[24];
            radio_format_freq(radio_freq, radio_mode, fd, sizeof(fd));
            JsonDocument doc;
            doc["ok"] = true;
            doc["slot"] = slot;
            doc["band"] = bands[radio_band_idx].name;
            doc["mode"] = radio_mode_str(radio_mode);
            doc["freq"] = radio_freq;
            doc["freq_display"] = fd;
            String response;
            serializeJson(doc, response);
            req->send(200, "application/json", response);
        } else if (strcmp(action, "clear") == 0) {
            radio_memory_clear(idx);
            memory_store_save();  /* flush the bank to flash (outside radio_mtx) */
            event_add("radio: memory %d cleared", slot);
            JsonDocument doc;
            doc["ok"] = true;
            doc["slot"] = slot;
            doc["action"] = "clear";
            String response;
            serializeJson(doc, response);
            req->send(200, "application/json", response);
        } else {
            req->send(400, "application/json",
                "{\"error\":\"action must be save, recall or clear\"}");
        }
    }, NULL, handle_body_collect);
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
    /* last_input_ms is a file-scope static (declared with radio_mark_activity above)
     * so an HTTP control request can reset it too, not just on-device input. */

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
                    case MI_MEMORY:
                        /* Open the 99-slot memory list; cursor opens at slot 1. */
                        menu_level = MENU_MEMORY;
                        memory_idx = 0;
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
            } else if (menu_level == MENU_MEMORY) {
                /* Act on the highlighted slot. An empty slot is SAVED (snapshot of the
                 * current tuning) and we stay in the list so the now-occupied slot is
                 * visible; an occupied slot is RECALLED onto the receiver and we drop to
                 * the VFO on that station. The memory helpers own radio_mtx themselves
                 * (save/recall take it for the snapshot/reapply); memory_store_save()
                 * flushes the bank to flash OUTSIDE the lock (it detaches the encoder for
                 * the blocking SPIFFS write), so it is called here with no lock held.
                 * TODO: device-side clear is deferred — long-press is already claimed by
                 * menu-open, so clearing a slot stays HTTP-only (POST /radio/memory). */
                if (memories[memory_idx].freq == 0) {
                    radio_memory_save(memory_idx);
                    memory_store_save();
                    memory_idx = menu_wrap(memory_idx, 1, MEMORY_COUNT);  /* advance -> instant repaint + ready for next save */
                } else {
                    if (radio_recall_memory(memory_idx)) {
                        ui_mode = UI_VFO;
                        menu_level = MENU_MAIN;
                    }
                    /* else: corrupt slot, stay in the list */
                }
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
                    case SI_CAL:
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_CAL;
                        break;
                    case SI_THEME:
                        /* Descend into the theme list editor. The palette is a global
                         * display property (not per-radio), edited live under display_mtx. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_THEME;
                        break;
                    case SI_BRIGHTNESS:
                        /* Descend into the numeric backlight editor. Brightness is a
                         * global display property (not per-radio), edited live under
                         * display_mtx inside display_set_brightness. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_BRIGHTNESS;
                        break;
                    case SI_LAYOUT:
                        /* Descend into the layout list editor. The layout is a global
                         * display property (not per-radio), edited live under display_mtx
                         * inside display_set_layout. */
                        menu_level = MENU_ADJUST;
                        adjust_target = ADJ_LAYOUT;
                        break;
                    case SI_EIBI:
                        /* ACTION, not a value editor (mirrors the Memory save/recall
                         * clicks): kick the EIBI schedule download on a background
                         * worker task and stay in the Settings list. eibi_start_load()
                         * returns immediately — false if WiFi is down or a load is
                         * already running — and progress is observable via
                         * /radio/eibi/status. Menu state is single-threaded on the
                         * loopTask, so no lock is taken here. */
                        eibi_start_load();
                        break;
                    default:
                        /* SI_ABOUT placeholder (TODO: wire a real leaf) and SI_BACK
                         * both step back up to the MAIN list. */
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
                } else if (adjust_target == ADJ_CAL) {
                    /* Numeric SSB calibration edit (SSB-only; no-op on FM/AM). One detent
                     * = RADIO_CAL_STEP Hz, clamped to +/-RADIO_CAL_MAX, folded into the BFO
                     * immediately via apply_cal_locked. Keyed off the band's live demod so
                     * the right per-sideband slot is edited. Persisted (v6). */
                    xSemaphoreTake(radio_mtx, portMAX_DELAY);
                    uint8_t m = band_table_mode();
                    if (radio_is_ssb(m)) {
                        int16_t *slot = (m == RADIO_MODE_USB)
                                            ? &band_usb_cal[radio_band_idx]
                                            : &band_lsb_cal[radio_band_idx];
                        int v = (int)*slot + (int)d * RADIO_CAL_STEP;
                        if (v < -RADIO_CAL_MAX) v = -RADIO_CAL_MAX;
                        if (v >  RADIO_CAL_MAX) v =  RADIO_CAL_MAX;
                        *slot = (int16_t)v;
                        if (radio_ok) apply_cal_locked();
                        radio_mark_dirty();
                    }
                    xSemaphoreGive(radio_mtx);
                } else if (adjust_target == ADJ_THEME) {
                    /* Global theme select. The palette lives in display.cpp and is
                     * guarded by display_mtx, NOT radio_mtx — so this branch takes no
                     * radio lock (nesting radio_mtx <-> display_mtx is forbidden).
                     * display_set_theme forces a full repaint, so scrolling previews
                     * each theme live. Persisted (v7); mark_dirty triggers the flush. */
                    int v = menu_wrap(display_get_theme(), d, display_theme_count());
                    display_set_theme((uint8_t)v);
                    radio_mark_dirty();
                } else if (adjust_target == ADJ_BRIGHTNESS) {
                    /* Backlight brightness. Owned by display.cpp and guarded by
                     * display_mtx inside display_set_brightness — NOT radio_mtx, so
                     * this branch takes no radio lock (nesting radio_mtx <-> display_mtx
                     * is forbidden, same as the theme branch above). Step 5 per detent,
                     * clamped to the 10..255 floor so this control never blacks the
                     * panel out. Live: the backlight tracks the knob. Persisted (v8);
                     * mark_dirty triggers the flush. */
                    int v = (int)display_get_brightness() + d * 5;
                    if (v < 10)  v = 10;
                    if (v > 255) v = 255;
                    display_set_brightness((uint8_t)v);
                    radio_mark_dirty();
                } else if (adjust_target == ADJ_LAYOUT) {
                    /* Global layout select. Owned by display.cpp and guarded by
                     * display_mtx inside display_set_layout — NOT radio_mtx, so this
                     * branch takes no radio lock (nesting radio_mtx <-> display_mtx is
                     * forbidden, same as the theme/brightness branches above).
                     * display_set_layout forces a full repaint, so scrolling previews
                     * each layout live. Persisted (v8); mark_dirty triggers the flush. */
                    int v = menu_wrap(display_get_layout(), d, display_layout_count());
                    display_set_layout((uint8_t)v);
                    radio_mark_dirty();
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
            else if (menu_level == MENU_MEMORY)
                /* Move the slot cursor only — no live preview (the receiver is
                 * reprogrammed on click, not per detent). Pure UI state, no lock. */
                memory_idx = menu_wrap(memory_idx, d, MEMORY_COUNT);
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
                /* Leaving the old frequency: the station name/RadioText no longer
                 * apply. Clear our snapshot and the chip's group buffers so a stale
                 * PS can't linger while the new station's RDS refills. */
                if (radio_mode == RADIO_MODE_FM) {
                    rds_reset_locked();
                    rx.clearRdsTextFixed();
                }
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

    /* 3c) RDS: throttled decode poll, FM only. Hard-gated on radio_mode == FM so
     * getRdsStatus/get* are never issued on AM/SSB (RDS is an FM-broadcast feature).
     * One radio_mtx hold, no delay(): read signal quality, skip the whole poll on a
     * weak signal (SNR < 12 dB) — cheap RDS on noise is garbage and wastes I2C — then
     * pull the RDS status and, only when the decoder reports sync + a fresh group,
     * copy out PS / RadioText / PI / PTY into the local snapshot. */
    static uint32_t last_rds_ms = 0;
    if (radio_ok && radio_mode == RADIO_MODE_FM && millis() - last_rds_ms > RADIO_RDS_POLL_MS) {
        last_rds_ms = millis();
        xSemaphoreTake(radio_mtx, portMAX_DELAY);
        rx.getCurrentReceivedSignalQuality();
        if (rx.getCurrentSNR() >= 12) {  /* gate: weak signal => skip (saves I2C, cuts garbage) */
            rx.getRdsStatus();
            if (rx.getRdsReceived() && rx.getRdsSync() && rx.getRdsSyncFound()) {
                const char *ps = rx.getRdsStationName();
                if (ps) { strncpy(rds_ps, ps, 8); rds_ps[8] = 0; got_rds = true; }
                /* RadioText: version B carries it in getRdsText2B, version A in the
                 * program-information (2A) buffer. */
                const char *rt = rx.getRdsVersionCode() ? rx.getRdsText2B()
                                                        : rx.getRdsProgramInformation();
                if (rt) { strncpy(rds_rt, rt, 64); rds_rt[64] = 0; }
                rds_pi  = rx.getRdsPIFixed();
                rds_pty = rx.getRdsPTYFixed();
            }
        }
        xSemaphoreGive(radio_mtx);
    }

    /* 3d) EIBI "now broadcasting": throttled offline-schedule lookup on AM/SSB (FM
     * uses RDS instead, gated out here). Gated on a loaded schedule AND a non-FM mode
     * AND a valid NTP clock; when any gate fails the cache is cleared so no stale name
     * lingers past a retune or mode change. Throttled to RADIO_EIBI_POLL_MS because
     * eibi_now_name() reads SPIFFS. This takes NO radio_mtx: freq/mode come from the
     * lock-free file-statics (the same values the eibi_handle_get accessors expose),
     * and the file read serialises on eibi_mtx inside eibi_now_name. loopTask only. */
    static uint32_t last_eibi_ms = 0;
    if (millis() - last_eibi_ms > RADIO_EIBI_POLL_MS) {
        last_eibi_ms = millis();
        int uh, um;
        if (radio_mode != RADIO_MODE_FM && eibi_available() && eibi_utc_now(uh, um)) {
            uint16_t f = radio_freq;
            char name[sizeof(eibi_now)];
            if (eibi_now_name(f, uh, um, name, sizeof(name))) {
                strncpy(eibi_now, name, sizeof(eibi_now) - 1);
                eibi_now[sizeof(eibi_now) - 1] = 0;
                eibi_now_valid = true;
            } else {
                eibi_now[0] = 0;
                eibi_now_valid = false;
            }
        } else {
            eibi_now[0] = 0;
            eibi_now_valid = false;
        }
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
        int16_t ucal, lcal;
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
        /* v6: the active band's two SSB calibration slots (per-band, like freq/step). */
        ucal = band_usb_cal[bnd]; lcal = band_lsb_cal[bnd];
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
        doc["usb_cal"] = ucal;
        doc["lsb_cal"] = lcal;
        /* v7: the global display theme index. Owned by display.cpp; read here through
         * the accessor OUTSIDE radio_mtx (already released above) — it takes no radio
         * lock and a byte read is atomic. */
        doc["theme"] = display_get_theme();
        /* v8: the global backlight brightness. Owned by display.cpp; read here through
         * the accessor OUTSIDE radio_mtx (already released above) — it takes no radio
         * lock and a byte read is atomic. */
        doc["brt"] = display_get_brightness();
        /* v8 (same schema, no bump): the global screen layout index. Owned by
         * display.cpp; read here through the accessor OUTSIDE radio_mtx (already
         * released above) — it takes no radio lock and a byte read is atomic. A
         * v8 file written before this key existed simply lacks it and restores as 0
         * (Default), so no version bump is needed. */
        doc["layout"] = display_get_layout();
        /* v8 (same schema, no bump): the global idle-dim threshold (seconds, 0=off).
         * Owned by display.cpp; read here through the accessor OUTSIDE radio_mtx (a
         * lone aligned 16-bit read is atomic). A v8 file written before this key
         * existed simply lacks it and restores as 0 (dimming off), so no bump. */
        doc["sleep"] = display_get_sleep();
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

    /* v7: the global display theme. Orthogonal to the radio state, so it is restored
     * here up front — a malformed radio field below must not cost the saved theme. The
     * index is validated against the live table; anything missing/out of range leaves
     * the default theme 0. At boot display_mtx is not yet created (skill_display_init
     * runs after us), so display_set_theme just stores the index and the boot paint in
     * skill_display_init() picks it up. */
    int th = doc["theme"] | -1;
    if (th >= 0 && th < display_theme_count()) display_set_theme((uint8_t)th);

    /* v8: the global backlight brightness, restored up front alongside the theme for
     * the same reason (a malformed radio field below must not cost it). Validated to
     * the 10..255 range display_set_brightness enforces; anything missing/out of range
     * leaves the default 255. Like the theme, this runs before skill_display_init (the
     * ledc channel is not up yet), so display_set_brightness just stores the level and
     * the boot paint applies it. Read tolerantly so future v8 keys need no bump. */
    int brt = doc["brt"] | -1;
    if (brt >= 10 && brt <= 255) display_set_brightness((uint8_t)brt);

    /* v8 (same schema, no bump): the global screen layout. Restored up front with the
     * theme/brightness for the same reason — a malformed radio field below must not
     * cost it. Absent (older v8 file) defaults to 0 (Default) via the | 0; out-of-range
     * is ignored by display_set_layout's own clamp, but gate on the live count too so a
     * future extra layout dropped from the table can never select a stale index. Runs
     * before skill_display_init (panel not up), so display_set_layout just stores the
     * index and the boot paint applies it. */
    int ly = doc["layout"] | 0;
    if (ly >= 0 && ly < display_layout_count()) display_set_layout((uint8_t)ly);

    /* v8 (same schema, no bump): the global idle-dim threshold. Restored up front with
     * the other display props — a malformed radio field below must not cost it. Absent
     * (older v8 file) defaults to 0 (dimming off) via the | 0; validated to the same
     * 0..3600 s range display_set_sleep clamps to. Runs before skill_display_init (the
     * ledc channel is not up yet), so it just stores the threshold; the first dim tick
     * after boot applies it. */
    int slp = doc["sleep"] | 0;
    if (slp >= 0 && slp <= 3600) display_set_sleep((uint16_t)slp);

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

    /* v6: the active band's two SSB calibration slots. Clamp to +/-RADIO_CAL_MAX; a
     * missing key (older file, already rejected by the version check) leaves the 0
     * seed. Set BEFORE apply_band_locked so its tail apply_cal_locked() folds the trim
     * into the restored BFO. Only the saved band's slots are stored — the rest keep 0. */
    if (!doc["usb_cal"].isNull()) {
        int uc = doc["usb_cal"] | 0;
        if (uc < -RADIO_CAL_MAX) uc = -RADIO_CAL_MAX;
        if (uc >  RADIO_CAL_MAX) uc =  RADIO_CAL_MAX;
        band_usb_cal[band] = (int16_t)uc;
    }
    if (!doc["lsb_cal"].isNull()) {
        int lc = doc["lsb_cal"] | 0;
        if (lc < -RADIO_CAL_MAX) lc = -RADIO_CAL_MAX;
        if (lc >  RADIO_CAL_MAX) lc =  RADIO_CAL_MAX;
        band_lsb_cal[band] = (int16_t)lc;
    }

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
        /* Re-push with the restored band/sideband cal folded in (radio_mode/band_idx
         * were set to the restored target by apply_band_locked above). */
        apply_cal_locked();
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
    /* Enable RDS decode for the boot FM band (see apply_band_locked). Single-tasked
     * here, so rds_reset_locked() runs without the mutex. radio_restore_state() below
     * re-runs apply_band_locked, which re-inits RDS if the saved band is FM. */
    rx.RdsInit();
    rx.setRdsConfig(1, 2, 2, 2, 2);
    rds_reset_locked();
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
        band_usb_cal[i]  = 0;  /* SSB calibration trim starts at zero-beat */
        band_lsb_cal[i]  = 0;
    }

    radio_ok = true;

    /* Overlay any saved band/freq/volume/bfo on top of the FM defaults. Runs
     * single-tasked here (before server.begin), so no mutex is required. */
    radio_restore_state();

    /* Overlay the saved memory bank on top of the empty default. Also single-tasked
     * here (before server.begin), so memory_store_load takes no lock. Independent of
     * radio_restore_state — a corrupt/missing memory file leaves the slots empty. */
    memory_store_load();

    Serial.println("[radio] SI4732 up: FM 100.0 MHz");
    skill_register(&radio_skill);
}
