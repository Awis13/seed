/*
 * sound.cpp — the pager's third output channel: a cue you can hear.
 *
 * The screen says what happened, the ring says that something happened from
 * across a room, and this says it from another room entirely. Same division of
 * labour, one step further out.
 *
 *
 * The hardware, established rather than assumed
 * ---------------------------------------------
 * Nothing in this project had touched audio before, and the pin map it had
 * inherited was a sibling project's. What is below was read off LilyGO's own
 * sources for this exact board, and the schematic settles two things the code
 * would otherwise have had to guess at.
 *
 *   Amplifier    MAX98357A, "3.2W I2S MONO AMP" (U27), speaker on the 2-pin
 *                MX1.25 header J4 through an L-C output filter.
 *   BCLK  GPIO46   \
 *   LRCLK GPIO40    > ESP32-S3 -> MAX98357A
 *   DIN   GPIO7    /
 *
 * Sources: hardware/T-Embed-CC1101 V1.0 24-07-29.pdf (page 2, the U27 block),
 * docs/pinmap_cn.md section 5, and examples/utilities.h — all in
 * github.com/Xinyuan-LilyGO/T-Embed-CC1101. The three numbers agree with what
 * /capabilities already advertised, so those were right; the two findings that
 * follow were not knowable from a pin list.
 *
 * 1. There is no amplifier enable GPIO.
 *
 *    SD_MODE (pin 4) appears exactly twice in the whole schematic: as the pin
 *    name and as a net tied to SPK_VDD through R43, 1M. It reaches no ESP32
 *    pin. So the part cannot be shut down by driving a line, and the LilyGO
 *    speaker example (examples/test_mic_speaker/test_mic_speaker.ino) touches
 *    no such pin either.
 *
 *    What that resistor also does is choose the channel. Per the datasheet
 *    (MAX98357A/MAX98357B, Table 5 and Figure 4) SD_MODE has a 100k +-8%
 *    internal pulldown, and the divider decides the mode:
 *
 *        > 1.4V              left
 *        0.77V .. 1.4V       right
 *        0.16V .. 0.77V      (left + right) / 2
 *        < 0.16V             shutdown
 *
 *    1M against 100k on a 3.3V rail is 0.30V, and 0.28V..0.32V across the
 *    pulldown's tolerance — the (left+right)/2 band, comfortably clear of the
 *    0.16V shutdown trip. The board therefore SUMS the two slots and halves
 *    them, which is why this file transmits in stereo and writes each mono
 *    sample into both slots: (s + s) / 2 == s, full amplitude, with no
 *    dependence on how the I2S driver chooses to fill the unused slot in mono
 *    mode. Sending only the left slot would be 6dB down for no reason.
 *
 * 2. The amplifier is on the switched rail, and the way to release it is to
 *    stop the clock.
 *
 *    SPK_VDD comes from VCC3V3 through FB3 (600R ferrite) — VCC3V3 being the
 *    switched 3.3V domain that BOARD_PWR_EN, GPIO15, controls, the same rail
 *    the CC1101 and the WS2812 ring live on. setup() already drives GPIO15
 *    HIGH as the first thing it does (that is the board's power hold), so the
 *    amp has power for the same reason the ring lights. There is no separate
 *    audio rail to bring up, and there is no way to cut the amp's power alone
 *    without taking the radio and the ring down with it.
 *
 *    The datasheet's Standby Mode section is what makes "release the amp when
 *    idle" achievable anyway: "The ICs automatically enter standby mode when
 *    BCLK is removed... the Class D speaker is turned off and the outputs go
 *    into a high-impedance state", at 340uA against 0.6uA for true shutdown.
 *    So snd_stop() calls i2s_channel_disable(), which stops BCLK and WS, and
 *    the part parks itself. 340uA is what an amplifier with no enable pin
 *    costs; it is three orders of magnitude below the LED ring and it is the
 *    floor available on this board.
 *
 *    Disabling the channel stops both clocks together, which matters: the same
 *    datasheet page warns "Do not remove LRCLK while BCLK is present. Removing
 *    LRCLK while BCLK is present can cause unexpected output behavior
 *    including a large DC output voltage." Nothing here ever stops one line on
 *    its own, and nothing should be added that does.
 *
 * 3. The amplifier does not accept every sample rate.
 *
 *    "LRCLK ONLY supports 8kHz, 16kHz, 32kHz, 44.1kHz, 48kHz, 88.2kHz and
 *    96kHz frequencies. LRCLK clocks at 11.025kHz, 12kHz, 22.05kHz and 24kHz
 *    are NOT supported." That is a hardware constraint, not a policy, and it
 *    is why the upload validator has a rate allowlist rather than a range: a
 *    22.05kHz WAV — an entirely ordinary thing to be handed — would otherwise
 *    upload cleanly, play silently, and look like a firmware bug.
 *
 * Click and pop are handled inside the part ("comprehensive click-and-pop
 * suppression... reduces audible transients on startup and shutdown"), so no
 * ramp circuit is needed here. What this file adds is cheap insurance on top:
 * the DMA is preloaded with silence before the channel is enabled, so the
 * first frames the amp ever clocks are zeros rather than whatever was left in
 * the buffer, every cue opens with SND_HEAD_MS of silence, and every
 * synthesised beep is enveloped so that no waveform starts or ends on a step.
 *
 *
 * The I2S API this core actually has
 * ----------------------------------
 * arduino-esp32 3.3.9 (IDF 5). LilyGO's example uses `driver/i2s.h` —
 * i2s_driver_install(), i2s_set_pin(), i2s_write() — and that header still
 * exists here only under include/driver/deprecated/, which is the legacy
 * driver the 3.x line deprecated. It is not what to build on.
 *
 * The Arduino-level replacement is the ESP_I2S library's I2SClass
 * (libraries/ESP_I2S/, present in this core), and this file deliberately does
 * not use it, because I2SClass is a thin wrapper over exactly the calls below
 * and hides the three things this use needs:
 *
 *   - DMA depth. I2S_DEFAULT_CFG in ESP_I2S.cpp hardcodes dma_desc_num 6 and
 *     dma_frame_num 240. This file sets its own (see SND_DMA_*) because the
 *     buffer depth is the entire underrun margin, and the margin has to cover
 *     a full-screen TFT repaint.
 *   - i2s_channel_preload_data(), which is what puts silence in front of the
 *     first sample.
 *   - a non-blocking write. I2SClass::write() passes its own _timeout to
 *     i2s_channel_write() and loops until everything is sent, which is a
 *     blocking call on the loop task by construction.
 *
 * So: driver/i2s_std.h directly — i2s_new_channel(),
 * i2s_channel_init_std_mode(), i2s_channel_reconfig_std_clock(),
 * i2s_channel_preload_data(), i2s_channel_enable()/disable() and
 * i2s_channel_write() with a zero timeout. One TX channel, pinned to I2S1 —
 * see skill_sound_init() for why the port is named rather than left to the
 * driver.
 *
 *
 * Why nothing here can block
 * --------------------------
 * The hard constraint is that audio must not stall the IR state machine, the
 * ring's 40Hz frame or AsyncTCP. Three things together guarantee it.
 *
 *   1. Every write is i2s_channel_write(..., timeout_ms = 0). The IDF contract
 *      is that it copies what fits into the free DMA descriptors, reports it
 *      in bytes_written and returns ESP_ERR_TIMEOUT the moment it would have
 *      to wait. snd_pump() advances its staging cursor by exactly
 *      bytes_written and returns; the remainder waits for the next loop pass.
 *      There is no retry loop and no wait of any kind.
 *
 *   2. The work per pass is bounded by construction, not by timing. snd_pump()
 *      runs two fixed iterations over a two-slot staging buffer: at most two
 *      writes of SND_CHUNK_FRAMES frames and at most two refills of the same
 *      size. A refill is either a memcpy-and-expand from one SPIFFS read of
 *      512 bytes, or 256 table lookups. No path scales with anything.
 *
 *   3. The DMA is the buffer that absorbs a slow pass. SND_DMA_DESCS *
 *      SND_DMA_FRAMES is 2048 frames, which at 16kHz is 128ms of audio already
 *      in the peripheral's hands. loop() idles at 10ms and its worst pass is a
 *      full-screen repaint, 170*320*2 bytes over 40MHz SPI — around 22ms plus
 *      overhead. The margin is roughly five times the worst case, and it
 *      shrinks with sample rate: 43ms at 48kHz, still twice a repaint. That
 *      ratio is the reason the upload validator stops at 48kHz rather than
 *      going up to the 96kHz the amplifier would accept.
 *
 * If the margin is ever blown anyway, auto_clear is set on the channel, so an
 * underrun transmits zeros rather than replaying the stale descriptor: the
 * failure mode is a gap in a beep, not a buzz that repeats until the cue ends.
 *
 * Nothing here reads or writes the filesystem from the web-server task except
 * an upload's body, which streams to a temp name nothing else touches — the
 * same exception POST /firmware/upload already makes, for the same reason.
 * Settings go to flash the way the ring's do: marked dirty on the web task,
 * written from loop() after a coalescing window.
 *
 *
 * Where a cue comes from
 * ----------------------
 * Two sources feed one pipeline.
 *
 * The firmware ships only synthesised cues: one beep for info, two for warn,
 * three for crit, so the levels are told apart by RHYTHM before pitch — which
 * is what survives a closed door, a running dishwasher and a listener who was
 * not paying attention. They are generated from a 256-entry sine table with a
 * short raised envelope on each edge; a square wave would be louder for free
 * but it is also harsh through a class-D part into a 1-inch speaker, and its
 * harmonics land above the amplifier's reconstruction filter.
 *
 * Real cues are uploaded, never compiled in. This is a public MIT repository
 * and third-party audio must not enter it: the mechanism ships, the files do
 * not. POST /sound/upload takes a WAV, validates it strictly and stores it in
 * SPIFFS (7.9MB here, against a 96KB ceiling per file), and the owner's own
 * cues live on the device rather than in git.
 *
 * PCM, not MP3. A decoder is real complexity — flash, RAM, a frame loop that
 * has to run on time — bought for nothing at this scale: half a second at
 * 16kHz mono 16-bit is 16KB and goes straight out of the buffer it was read
 * into.
 *
 *
 * Night
 * -----
 * Inside the night window this skill is silent, including for a critical, and
 * that is the default. The reasoning is the owner's and belongs in the code:
 * being woken at 3am by a DNS alert is the specific failure this device must
 * not have, and a critical already breathes on the screen for whoever is awake
 * to see it. `night_sound` turns it back on for anyone who wants a pager that
 * can wake them, without a reflash.
 *
 * The window itself is the ring's — ring_night_from/ring_night_to, set through
 * POST /ring. One device, one idea of night. Duplicating it would mean two
 * places to change and one of them silently forgotten.
 */

#include "driver/i2s_std.h"

/* --- Wiring (see the header comment for the sources) --- */

#define SND_PIN_BCLK   46
#define SND_PIN_LRCLK  40
#define SND_PIN_DIN     7

/* --- Rates ---
 *
 * The channel is created at this rate and reconfigured to a file's own rate
 * when one differs. Every value here is in the amplifier's supported set. */
#define SND_RATE_DEFAULT  16000

/* The rates POST /sound/upload accepts. The amplifier also clocks 88.2k and
   96k; they are refused because the DMA cushion below is a fixed frame count,
   so its duration halves as the rate doubles, and a notification cue has
   nothing to gain from either. */
/* Sliced out by BOTH tools/test_wav_parse.sh and tools/test_wav_header.sh —
   the second one arrived when the microphone's header writer had to be
   round-tripped through the parser below. Each compiles this region verbatim,
   so the begin marker has to stay on a line of its own: the slicer drops the
   marker line and copies everything after it, and a comment that spilled onto
   a second line would land in the generated source as code. */
/* host-test:begin rates */
static const uint32_t snd_rates_ok[] = {8000, 16000, 32000, 44100, 48000};
static const int snd_rates_ok_count =
    (int)(sizeof(snd_rates_ok) / sizeof(snd_rates_ok[0]));
/* host-test:end */

/* --- DMA ---
 *
 * 8 descriptors of 256 frames. A frame is two 16-bit slots, so a descriptor is
 * 1024 bytes — well inside the 4092-byte per-descriptor ceiling — and the
 * whole ring is 8KB of heap, held for the life of the boot. See the header for
 * why the depth is what it is. */
#define SND_DMA_DESCS    8
#define SND_DMA_FRAMES 256

/* --- Staging ---
 *
 * Two buffers, filled from the source and drained into the DMA. 256 frames is
 * 16ms at 16kHz; two of them are 2KB of .bss and one loop pass can fill both.
 * Deliberately not "read the file into RAM": a 3-second cue is 96KB, and this
 * device runs at 23.8% RAM with a WiFi stack and an async web server in it. */
#define SND_CHUNK_FRAMES 256
#define SND_FRAME_BYTES  4   /* two slots, 16 bits each */

/* Silence in front of every cue, and how long the channel stays enabled after
   the last sample is written.
 *
 * The linger has to outlast the DMA: audio written is not audio played, and up
 * to SND_DMA_DESCS * SND_DMA_FRAMES of it is still in the peripheral when the
 * last write returns — 128ms at 16kHz. 400ms clears that at every accepted
 * rate and doubles as hysteresis, so two notifications a moment apart do not
 * stop and restart the amplifier between them. */
#define SND_HEAD_MS    24
#define SND_LINGER_MS 400

/* How long a cue may make no progress into the DMA before it is written off as
   wedged. Generous on purpose: the DMA hands a descriptor back roughly every
   SND_DMA_FRAMES/rate seconds (16ms at 16kHz), so a healthy cue never goes
   more than a few tens of milliseconds without a successful write, and this is
   two orders of magnitude above that. It exists to catch a channel that
   refuses every write, not to police timing. */
#define SND_STALL_MS 3000

/* --- Synthesised cues --- */

#define SND_TONE_AMP   9000   /* of 32767, before volume */
#define SND_RAMP_MS       6   /* envelope edge; without it every beep clicks */
#define SND_BEEPS_MAX     4

/* --- Files --- */

#define SND_NAME_LEN     17    /* 16 chars */
/* 96KB is 3.07s at 16kHz mono, which is longer than any notification cue has
   any business being. SPIFFS is 0x7E0000 (7.9MB) on this partition table, so
   the ceiling is about restraint, not space. */
#define SND_DATA_MAX     (96UL * 1024UL)
#define SND_HDR_SCAN     256   /* how far into a WAV the data chunk may start */
#define SND_UPLOAD_MAX   (SND_DATA_MAX + SND_HDR_SCAN)
#define SND_MAP_MAX      8     /* source -> file assignments */

#define SND_CFG_FILE  "/sound.json"
#define SND_CFG_TMP   "/sound.tmp"
#define SND_CFG_DELAY_MS 1500

/* Maximum by default, at the owner's request. Only the compiled fallback: a
   device with /sound.json on it keeps whatever is stored there, and this is
   what a fresh flash or a wiped filesystem comes up with. Coming up quiet is
   the worse failure for a pager — an unheard cue looks identical to a broken
   amplifier, and the volume is a single API call away either way. */
#define SND_VOL_DEFAULT 100

/* --- State --- */

static bool snd_ready = false;
static i2s_chan_handle_t snd_tx = NULL;
static uint32_t snd_rate = SND_RATE_DEFAULT;      /* what the current cue wants */
static uint32_t snd_hw_rate = SND_RATE_DEFAULT;   /* what the channel is clocked at */
static bool snd_enabled_hw = false;               /* enabled, so BCLK is running */

/* Settings. Volatile for the same reason the ring's are: written on the
   web-server task, read on the loop task, every one a single aligned scalar. */
static volatile bool snd_on = true;
static volatile uint8_t snd_volume = SND_VOL_DEFAULT;   /* percent */
static volatile bool snd_night_sound = false;

static volatile bool snd_cfg_dirty = false;
static volatile unsigned long snd_cfg_save_at = 0;

/* Assignments. A notification's source wins over its level, because a source
   is the more specific statement of the two. */
static char snd_level_file[3][SND_NAME_LEN];
struct SndSrcMap { char source[NOTIFY_SOURCE_LEN]; char file[SND_NAME_LEN]; };
static SndSrcMap snd_src_map[SND_MAP_MAX];

/* Playback. All of it is owned by the loop task. */
enum { SND_SRC_NONE = 0, SND_SRC_TONE, SND_SRC_FILE };
static uint8_t snd_src = SND_SRC_NONE;
static unsigned long snd_off_at = 0;      /* when to release the amplifier */
static unsigned long snd_progress_ms = 0; /* last time a write actually moved */
static uint16_t snd_head_frames = 0;      /* silence still owed at the head */
static char snd_playing[SND_NAME_LEN];    /* file being played, "" for a tone */

static File snd_fh;
static uint32_t snd_file_left = 0;        /* bytes of PCM still unread */

struct SndBeep { uint16_t freq_hz; uint16_t on_ms; uint16_t gap_ms; };
static SndBeep snd_beep[SND_BEEPS_MAX];
static uint8_t snd_beep_count = 0;
static uint8_t snd_beep_i = 0;
static uint32_t snd_beep_frame = 0;       /* position within the current beep */
static uint32_t snd_phase = 0;            /* 16.16 into snd_sine[] */

struct SndBuf {
    uint16_t frames;   /* valid frames */
    uint16_t pos;      /* frames already handed to the DMA */
    int16_t s[SND_CHUNK_FRAMES * 2];
};
static SndBuf snd_buf[2];
static uint8_t snd_play_i = 0;

/* Mono scratch for one SPIFFS read, expanded into a staging buffer. */
static int16_t snd_mono[SND_CHUNK_FRAMES];

/* Counters, so a cue that does not come out of the speaker can still be
   diagnosed over the API.
 *
 * snd_played on its own was not enough, and 0.6.0 proved it: it counted four
 * cues started while the amplifier was being disabled before it had clocked
 * more than a fraction of the first one, and nothing in the API contradicted
 * "four cues played". What distinguishes a working device from a silent one is
 * how many bytes actually reached the DMA, so that is counted too — both for
 * the current cue and for the life of the boot. A cue at 16kHz costs 4 bytes
 * per frame, so a 150ms beep is about 9600 bytes and anything far below that
 * means the audio never left the chip, whatever `played` says. */
static uint32_t snd_played = 0;
static uint32_t snd_suppressed = 0;
static uint32_t snd_dma_bytes = 0;    /* accepted by i2s_channel_write, total */
static uint32_t snd_cue_bytes = 0;    /* the same, for the current cue */
static uint32_t snd_zero_writes = 0;  /* writes the DMA had no room for */
static uint32_t snd_stalls = 0;
static int snd_last_err = 0;          /* last non-OK esp_err from a write */

/* --- Upload staging ---
 *
 * The body streams to a temp name on the web-server task. The rename into
 * place, and any delete, happen on the loop task instead: loop() is the task
 * that owns the playing file handle, so it is the only one that can close a
 * file before the name under it changes. Doing it on the web task would race a
 * cue that started microseconds earlier. */
static File snd_up_file;
static bool snd_up_active = false;
static bool snd_up_error = false;
static char snd_up_msg[64];
static char snd_up_name[SND_NAME_LEN];
/* The parsed level, not the word: -1 for "no level given". Keeping the string
   would mean a bounded copy of a value that has already been validated into
   three possibilities, which is both larger and harder for the compiler to
   convince itself about. */
static int8_t snd_up_level = -1;
static char snd_up_source[NOTIFY_SOURCE_LEN];
static uint8_t snd_up_hdr[SND_HDR_SCAN];
static size_t snd_up_hdr_len = 0;
static bool snd_up_hdr_done = false;
static uint32_t snd_up_data_left = 0;
static uint32_t snd_up_written = 0;
static uint32_t snd_up_rate = 0;
static volatile unsigned long snd_up_last_ms = 0;
/* Same watchdog, and the same number, as the OTA upload in main.cpp: a
   connection that dies mid-body reaches none of the paths that clear
   snd_up_active, and without this every later upload is refused for the life
   of the boot. 30s is roughly double the worst gap a struggling-but-alive TCP
   transfer produces. */
#define SND_UPLOAD_STALL_MS 30000

static volatile bool snd_commit_pending = false;
static volatile bool snd_delete_pending = false;
static char snd_delete_name[SND_NAME_LEN];

/* --- Sine table ---
 *
 * 256 entries of one period, so the phase accumulator's top byte indexes it
 * directly. 512 bytes of flash against a sinf() per sample; the table also
 * makes the generator's cost per frame a constant, which is what the bounded
 * work argument above depends on. */
static const int16_t snd_sine[256] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278,  9512,  8739,  7962,  7179,
     6393,  5602,  4808,  4011,  3212,  2410,  1608,   804,
        0,  -804, -1608, -2410, -3212, -4011, -4808, -5602,
    -6393, -7179, -7962, -8739, -9512,-10278,-11039,-11793,
   -12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,
   -18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,
   -23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,
   -27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,
   -30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,
   -32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,
   -32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,
   -32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,
   -30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,
   -27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,
   -23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,
   -18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,
   -12539,-11793,-11039,-10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608,  -804,
};

/* --- Cue patterns ---
 *
 * One beep, two, three. The count is the message: pitch alone is what a
 * listener has to have learned, a count is what they can hear cold. */
static void snd_pattern_for(uint8_t level) {
    snd_beep_count = 0;
    switch (level) {
        case NOTIFY_CRIT:
            for (int i = 0; i < 3; i++)
                snd_beep[snd_beep_count++] = (SndBeep){1319, 70, 60};
            break;
        case NOTIFY_WARN:
            snd_beep[snd_beep_count++] = (SndBeep){988, 100, 90};
            snd_beep[snd_beep_count++] = (SndBeep){988, 100, 0};
            break;
        default:
            snd_beep[snd_beep_count++] = (SndBeep){880, 150, 0};
            break;
    }
}

/* --- Night ---
 *
 * ring_night_now() rather than a second window: see the header. When the clock
 * has never been set this returns false and the device stays audible, which is
 * the same direction the ring errs in — a node that has not reached NTP is
 * usually a freshly booted one, and going quiet because the time is unknown
 * fails in the direction that loses messages. */
static bool snd_muted_now() {
    if (!snd_ready || !snd_on) return true;
    if (!snd_night_sound && ring_night_now()) return true;
    return false;
}

/* --- Filenames --- */

static bool snd_name_valid(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n >= SND_NAME_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* "/s_<name>.wav". SPIFFS caps a full path at 32 bytes including the null, so
   the prefix is short on purpose: 3 + 16 + 4 + 1 is 24. */
static void snd_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "/s_%s.wav", name);
}

static void snd_path_tmp(const char *name, char *out, size_t n) {
    snprintf(out, n, "/s_%s.tmp", name);
}

/* Give up on the transfer in progress and leave nothing behind. A rejected
   upload that kept its temp file would accumulate one dead file per bad WAV,
   under a name no endpoint lists and nothing ever reclaims. Called from the
   web-server task on every rejection, and from loop() when the stall watchdog
   fires. */
static void snd_up_abandon() {
    if (snd_up_file) snd_up_file.close();
    snd_up_active = false;
    char tmp[40];
    snd_path_tmp(snd_up_name, tmp, sizeof(tmp));
    SPIFFS.remove(tmp);
}

/* --- WAV header ---
 *
 * A strict reader. Everything it refuses, it refuses outright rather than
 * accepting a truncated or resampled version: a cue that plays back at half
 * speed or cuts off mid-word is worse than one that never uploaded, because
 * only the second kind gets noticed. */

/* Everything between the marker below and the matching end marker is compiled
   verbatim on the host by tools/test_wav_parse.sh, so the regression test runs
   against this code rather than a copy of it that could drift. Keep the two
   markers on lines of their own. */
/* host-test:begin wav */
struct SndWav {
    uint32_t rate;
    uint32_t data_off;
    uint32_t data_len;
};

static uint16_t snd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t snd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool snd_rate_ok(uint32_t rate) {
    for (int i = 0; i < snd_rates_ok_count; i++)
        if (snd_rates_ok[i] == rate) return true;
    return false;
}

static bool snd_wav_parse(const uint8_t *p, size_t n, SndWav &out,
                          const char **err) {
    if (n < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        *err = "not a RIFF/WAVE file";
        return false;
    }

    bool have_fmt = false;
    size_t at = 12;
    /* Chunk walk. Every step is bounds-checked against the bytes actually in
       hand, so a file that lies about a chunk size runs off the end of the
       scan window and is rejected there rather than being trusted. */
    while (at + 8 <= n) {
        const uint8_t *id = p + at;
        uint32_t size = snd_le32(p + at + 4);
        size_t body = at + 8;

        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || body + 16 > n) { *err = "truncated fmt chunk"; return false; }
            uint16_t format = snd_le16(p + body);
            uint16_t channels = snd_le16(p + body + 2);
            uint32_t rate = snd_le32(p + body + 4);
            uint16_t bits = snd_le16(p + body + 14);
            /* WAVE_FORMAT_EXTENSIBLE (0xFFFE) carries the real format in a
               GUID and is not worth parsing for a beep; say so plainly. */
            if (format != 1) { *err = "not uncompressed PCM (WAVE format 1)"; return false; }
            if (channels != 1) { *err = "must be mono"; return false; }
            if (bits != 16) { *err = "must be 16-bit"; return false; }
            if (!snd_rate_ok(rate)) {
                *err = "sample rate must be 8000, 16000, 32000, 44100 or 48000";
                return false;
            }
            out.rate = rate;
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) { *err = "data chunk before fmt chunk"; return false; }
            if (size == 0 || (size & 1)) { *err = "data chunk length must be even and non-zero"; return false; }
            if (size > SND_DATA_MAX) { *err = "audio longer than 96KB"; return false; }
            out.data_off = (uint32_t)body;
            out.data_len = size;
            return true;
        }

        /* Advance past this chunk, padded to an even length as RIFF requires.
         *
         * Computed in 64 bits and required to move strictly forward, and both
         * halves of that are the fix for a hang rather than tidiness. `size`
         * is 32 bits of attacker-controlled input and size_t is 32 bits on
         * this chip, so the obvious `at = body + size + (size & 1)` is modular
         * arithmetic on hostile data. Exactly two values satisfy
         * 8 + size + (size & 1) == 0 (mod 2^32) — 0xFFFFFFF8, which is even,
         * and 0xFFFFFFF7, which is odd and picks up the pad byte — and either
         * one leaves `at` exactly where it was while `at + 8 <= n` stays true.
         * That is an unbreakable loop, and a whole range of other values moves
         * `at` backwards, which can cycle between two offsets just as
         * permanently.
         *
         * It mattered because of where this runs. snd_upload_body() calls this
         * on the AsyncTCP task, so a single ~20-byte POST /sound/upload whose
         * first chunk is neither `fmt ` nor `data` would spin the web server
         * forever: no notifications, no API, no OTA, nothing but a power
         * cycle. snd_open_file() calls it on the loop task, which would take
         * the screen, the IR machine and the ring with it.
         *
         * In 64 bits the sum cannot wrap, so `next` is always at least at + 8.
         * The check below is therefore an assertion rather than a filter, and
         * it is kept because it is what makes the guarantee local: this loop
         * terminates because `at` strictly increases towards `n`, and that is
         * true by inspection of these four lines rather than by reasoning
         * about every value `size` can take. */
        uint64_t next = (uint64_t)body + (uint64_t)size + (uint64_t)(size & 1);
        if (next <= (uint64_t)at) { *err = "malformed chunk size"; return false; }
        if (next > (uint64_t)n) break;   /* runs past the scan window */
        at = (size_t)next;
    }

    *err = have_fmt ? "no data chunk within the first 256 bytes"
                    : "no fmt chunk within the first 256 bytes";
    return false;
}
/* host-test:end */

/* --- Source: file --- */

static void snd_close_file() {
    if (snd_fh) snd_fh.close();
    snd_file_left = 0;
}

/* Opens and re-validates. The header is parsed again on every play rather than
   trusted from upload time, because SPIFFS can be replaced wholesale by a
   filesystem image — a stored file is no more trustworthy than a POST body,
   which is the same conclusion notify_load() reached about its own snapshot. */
static bool snd_open_file(const char *name) {
    char path[40];
    snd_path(name, path, sizeof(path));

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) return false;

    uint8_t hdr[SND_HDR_SCAN];
    size_t n = f.read(hdr, sizeof(hdr));
    SndWav w;
    const char *err = NULL;
    if (!snd_wav_parse(hdr, n, w, &err)) {
        f.close();
        event_add("sound: %s unplayable (%s)", name, err ? err : "bad header");
        return false;
    }

    uint32_t size = (uint32_t)f.size();
    if (w.data_off >= size) { f.close(); return false; }
    uint32_t avail = size - w.data_off;
    if (!f.seek(w.data_off)) { f.close(); return false; }

    snd_fh = f;
    /* Whichever is smaller: a header may claim more than the file holds. */
    snd_file_left = (w.data_len < avail) ? w.data_len : avail;
    snd_file_left &= ~1UL;             /* whole samples only */
    snd_rate = w.rate;
    return snd_file_left > 0;
}

/* --- Fill ---
 *
 * One mono sample becomes one stereo frame carrying it twice, because the
 * board sums the slots and halves them (see the header). Volume is applied
 * here rather than at the source, so it is one multiply on the only path both
 * sources share, and a change takes effect on the next buffer rather than the
 * next cue. */
static void snd_expand(int16_t *dst, const int16_t *mono, uint16_t frames) {
    int32_t vol = snd_volume;
    for (uint16_t i = 0; i < frames; i++) {
        int16_t s = (int16_t)(((int32_t)mono[i] * vol) / 100);
        dst[i * 2] = s;
        dst[i * 2 + 1] = s;
    }
}

static uint16_t snd_fill_tone(int16_t *mono, uint16_t max_frames) {
    uint16_t made = 0;
    while (made < max_frames && snd_beep_i < snd_beep_count) {
        const SndBeep &b = snd_beep[snd_beep_i];
        uint32_t on = (uint32_t)b.on_ms * snd_rate / 1000;
        uint32_t total = on + (uint32_t)b.gap_ms * snd_rate / 1000;
        uint32_t ramp = (uint32_t)SND_RAMP_MS * snd_rate / 1000;
        if (ramp * 2 > on) ramp = on / 2;

        uint32_t step = (uint32_t)(((uint64_t)b.freq_hz << 16) / snd_rate);

        while (made < max_frames && snd_beep_frame < total) {
            int16_t v = 0;
            if (snd_beep_frame < on) {
                int32_t s = snd_sine[(snd_phase >> 8) & 0xFF];
                snd_phase += step;
                /* Linear attack and release. The point is only that the
                   waveform starts and ends at zero: a beep that begins at full
                   amplitude is a step, and a step is a click. */
                int32_t env = 255;
                if (ramp > 0) {
                    if (snd_beep_frame < ramp)
                        env = (int32_t)(snd_beep_frame * 255 / ramp);
                    else if (snd_beep_frame > on - ramp)
                        env = (int32_t)((on - snd_beep_frame) * 255 / ramp);
                }
                v = (int16_t)((s * SND_TONE_AMP / 32767) * env / 255);
            } else {
                snd_phase = 0;   /* every beep opens on a zero crossing */
            }
            mono[made++] = v;
            snd_beep_frame++;
        }

        if (snd_beep_frame >= total) {
            snd_beep_frame = 0;
            snd_beep_i++;
        }
    }
    return made;
}

static uint16_t snd_fill_file(int16_t *mono, uint16_t max_frames) {
    if (!snd_fh || snd_file_left == 0) return 0;
    uint32_t want = (uint32_t)max_frames * 2;
    if (want > snd_file_left) want = snd_file_left;
    /* WAV PCM is little-endian and so is this chip, so the bytes on flash are
       already int16_t samples and no swap is needed. */
    int got = snd_fh.read((uint8_t *)mono, want);
    if (got <= 0) { snd_file_left = 0; return 0; }
    snd_file_left -= (uint32_t)got;
    return (uint16_t)(got / 2);
}

/* Returns frames written, 0 when the cue is over. May return short at a source
   boundary — the head-silence handover, or the end of the audio — which is why
   snd_fill() below loops over it. */
static uint16_t snd_fill_once(int16_t *dst, uint16_t max_frames) {
    /* Silence first, always: the amplifier has just left standby and the file
       is not trusted to start at zero. */
    if (snd_head_frames > 0) {
        uint16_t n = (snd_head_frames < max_frames) ? snd_head_frames : max_frames;
        memset(dst, 0, (size_t)n * SND_FRAME_BYTES);
        snd_head_frames -= n;
        return n;
    }

    uint16_t frames = 0;
    if (snd_src == SND_SRC_TONE) frames = snd_fill_tone(snd_mono, max_frames);
    else if (snd_src == SND_SRC_FILE) frames = snd_fill_file(snd_mono, max_frames);

    if (frames == 0) {
        if (snd_src == SND_SRC_FILE) snd_close_file();
        snd_src = SND_SRC_NONE;
        return 0;
    }
    snd_expand(dst, snd_mono, frames);
    return frames;
}

/*
 * Fill the staging buffer as completely as the source allows.
 *
 * This matters more than it looks. A staging buffer is exactly one DMA
 * descriptor (SND_CHUNK_FRAMES == SND_DMA_FRAMES), and i2s_channel_write()
 * holds a partly-filled descriptor open across calls — but it also drops that
 * descriptor and fetches a fresh one as soon as its queue runs close to full
 * (esp-idf v5.5.4, i2s_common.c: the `uxQueueSpacesAvailable(...) <= 1` arm of
 * the acquire condition). So handing it a short buffer risks committing a
 * half-written descriptor, whose remainder transmits as the silence auto_clear
 * left there. Filling completely keeps every write descriptor-sized, and short
 * buffers then only happen at the very end of a cue, where the tail is silence
 * anyway.
 */
static uint16_t snd_fill(int16_t *dst, uint16_t max_frames) {
    uint16_t made = 0;
    while (made < max_frames) {
        uint16_t got = snd_fill_once(dst + (size_t)made * 2, (uint16_t)(max_frames - made));
        if (got == 0) break;   /* source exhausted; snd_fill_once cleared snd_src */
        made = (uint16_t)(made + got);
    }
    return made;
}

/* --- Transport --- */

static void snd_hw_stop() {
    if (!snd_enabled_hw) return;
    /* Stops BCLK and WS together, which is both what puts the MAX98357A into
       standby and what avoids the datasheet's "LRCLK removed while BCLK is
       present" hazard. Never disable one line on its own. */
    i2s_channel_disable(snd_tx);
    snd_enabled_hw = false;
}

/*
 * Bring the clock up at `rate`, or leave it alone if it is already there.
 *
 * The early return is what makes a second cue arriving inside the linger free:
 * it appends to a channel that never stopped, so there is no disable/enable
 * pair in the middle of an audible stretch and nothing for the amplifier to
 * click about. Only a cue at a different sample rate pays for a restart, and
 * it has to — i2s_channel_reconfig_std_clock() and i2s_channel_preload_data()
 * are both only legal in the READY state.
 */
static bool snd_hw_start(uint32_t rate) {
    if (snd_enabled_hw && rate == snd_hw_rate) return true;
    if (snd_enabled_hw) snd_hw_stop();

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    if (i2s_channel_reconfig_std_clock(snd_tx, &clk) != ESP_OK) return false;
    snd_hw_rate = rate;

    /* Silence into the DMA before the first clock edge, so the amplifier's
       first frames out of standby are zeros. The part suppresses its own
       click, this is what stops it having anything to suppress. */
    static const int16_t quiet[64] = {0};
    size_t loaded = 0;
    i2s_channel_preload_data(snd_tx, quiet, sizeof(quiet), &loaded);

    if (i2s_channel_enable(snd_tx) != ESP_OK) return false;
    snd_enabled_hw = true;
    return true;
}

/* Drop the current cue without touching the hardware. Note what this does not
   do: audio already handed to the DMA still plays out, up to the buffer depth.
   Nothing here can un-write it, and truncating mid-waveform is exactly the
   discontinuity every other part of this file exists to avoid. */
static void snd_reset_source() {
    snd_src = SND_SRC_NONE;
    snd_close_file();
    snd_playing[0] = '\0';
    snd_buf[0].frames = snd_buf[0].pos = 0;
    snd_buf[1].frames = snd_buf[1].pos = 0;
    snd_head_frames = 0;
}

static void snd_stop() {
    snd_reset_source();
    snd_hw_stop();
    snd_off_at = 0;
}

/*
 * Fill both staging slots, push the leading one, repeat a fixed number of
 * times. Nothing here can wait, and nothing scales with anything.
 *
 * SND_PUMP_ROUNDS is what decides whether the device can keep ahead of its own
 * playback, and it is sized against the fastest rate accepted rather than the
 * one that will actually be used. Each round moves at most SND_CHUNK_FRAMES,
 * so a pass supplies 1024 frames; loop() idles at 10ms, which is 102k frames
 * per second against 48000 consumed — a little over twice real time at the
 * worst accepted rate, and six times over at the 16kHz a synthesised cue uses.
 *
 * Two rounds would have been 1.07x at 48kHz. That is not a margin: a single
 * slow pass would put the device behind and it would never catch up, and the
 * symptom would be a cue that stutters only for whoever uploaded a 48kHz file.
 */
#define SND_PUMP_ROUNDS 4

static void snd_pump() {
    if (!snd_enabled_hw) return;

    for (int round = 0; round < SND_PUMP_ROUNDS; round++) {
        for (int n = 0; n < 2; n++) {
            SndBuf &b = snd_buf[(snd_play_i + n) & 1];
            if (b.frames != 0 || snd_src == SND_SRC_NONE) continue;
            b.pos = 0;
            b.frames = snd_fill(b.s, SND_CHUNK_FRAMES);
        }

        SndBuf &b = snd_buf[snd_play_i];
        if (b.pos >= b.frames) break;      /* nothing staged: source is done */

        size_t want = (size_t)(b.frames - b.pos) * SND_FRAME_BYTES;
        size_t wrote = 0;
        /* timeout 0: copies what fits in the free descriptors, reports it in
           `wrote` and returns ESP_ERR_TIMEOUT rather than waiting for more. A
           short write is the normal case, not an error — the rest goes on the
           next pass, and there is no retry loop anywhere. */
        esp_err_t err = i2s_channel_write(snd_tx, &b.s[(size_t)b.pos * 2], want, &wrote, 0);
        /* ESP_ERR_TIMEOUT is the ordinary "the DMA is full right now" answer at
           this timeout and says nothing is wrong; anything else is worth
           keeping, because a channel that refuses every write is the one
           failure that is otherwise indistinguishable from a dead speaker. */
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) snd_last_err = (int)err;
        if (wrote > 0) {
            snd_progress_ms = millis();
            snd_dma_bytes += (uint32_t)wrote;
            snd_cue_bytes += (uint32_t)wrote;
        } else {
            snd_zero_writes++;
        }
        b.pos = (uint16_t)(b.pos + wrote / SND_FRAME_BYTES);

        if (b.pos >= b.frames) {
            b.frames = 0;
            b.pos = 0;
            snd_play_i ^= 1;
        }
        if (wrote < want) break;   /* DMA is full; nothing more to do this pass */
    }
}

/* --- Starting a cue --- */

static const char *snd_pick(uint8_t level, const char *source) {
    if (source && source[0]) {
        for (int i = 0; i < SND_MAP_MAX; i++)
            if (snd_src_map[i].source[0] && strcmp(snd_src_map[i].source, source) == 0)
                return snd_src_map[i].file;
    }
    if (level <= NOTIFY_CRIT && snd_level_file[level][0]) return snd_level_file[level];
    return NULL;
}

/*
 * Play the cue for a level, optionally the one a source has claimed.
 *
 * An assigned file that will not open falls back to the synthesised cue rather
 * than to silence. A missing or corrupt file is a configuration mistake, and
 * the device staying audible is what lets somebody notice it.
 */
static bool snd_play(uint8_t level, const char *source) {
    if (snd_muted_now()) { snd_suppressed++; return false; }

    snd_reset_source();

    const char *file = snd_pick(level, source);
    if (file && file[0] && snd_open_file(file)) {
        snd_src = SND_SRC_FILE;
        snprintf(snd_playing, sizeof(snd_playing), "%s", file);
    } else {
        snd_rate = SND_RATE_DEFAULT;
        snd_pattern_for(level);
        snd_beep_i = 0;
        snd_beep_frame = 0;
        snd_phase = 0;
        snd_src = SND_SRC_TONE;
        snd_playing[0] = '\0';
    }

    snd_head_frames = (uint16_t)((uint32_t)SND_HEAD_MS * snd_rate / 1000);

    if (!snd_hw_start(snd_rate)) {
        snd_stop();
        event_add("sound: I2S start failed");
        return false;
    }
    snd_progress_ms = millis();
    snd_cue_bytes = 0;
    snd_played++;
    return true;
}

/* --- Persistence --- */

static void snd_cfg_save() {
    JsonDocument doc;
    doc["on"] = snd_on;
    doc["v"] = snd_volume;
    doc["ns"] = snd_night_sound;
    JsonArray lv = doc["lv"].to<JsonArray>();
    for (int i = 0; i <= NOTIFY_CRIT; i++) lv.add(snd_level_file[i]);
    JsonArray sm = doc["sm"].to<JsonArray>();
    for (int i = 0; i < SND_MAP_MAX; i++) {
        if (!snd_src_map[i].source[0]) continue;
        JsonObject o = sm.add<JsonObject>();
        o["s"] = snd_src_map[i].source;
        o["f"] = snd_src_map[i].file;
    }
    String out;
    serializeJson(doc, out);
    write_spiffs_file_atomic(SND_CFG_FILE, SND_CFG_TMP, out);
}

static void snd_cfg_mark_dirty() {
    unsigned long at = millis() + SND_CFG_DELAY_MS;
    if (!snd_cfg_dirty || (long)(at - snd_cfg_save_at) < 0) snd_cfg_save_at = at;
    snd_cfg_dirty = true;
}

/* A file that is missing, truncated or nonsense leaves the compiled defaults
   standing, exactly as the ring's does. */
static void snd_cfg_load() {
    String raw = spiffs_read_file(SND_CFG_FILE, SND_CFG_TMP);
    if (raw.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

    if (doc["on"].is<bool>()) snd_on = doc["on"].as<bool>();
    if (doc["ns"].is<bool>()) snd_night_sound = doc["ns"].as<bool>();
    if (doc["v"].is<int>()) {
        int v = doc["v"].as<int>();
        if (v >= 0 && v <= 100) snd_volume = (uint8_t)v;
    }
    int i = 0;
    for (JsonVariantConst v : doc["lv"].as<JsonArrayConst>()) {
        if (i > NOTIFY_CRIT) break;
        const char *s = v.as<const char *>();
        /* Validated on the way in, not only on the way out: this file can be
           replaced wholesale, and a name with a slash in it would otherwise
           become a path. */
        if (s && snd_name_valid(s)) snprintf(snd_level_file[i], SND_NAME_LEN, "%s", s);
        i++;
    }
    int m = 0;
    for (JsonObjectConst o : doc["sm"].as<JsonArrayConst>()) {
        if (m >= SND_MAP_MAX) break;
        const char *s = o["s"].as<const char *>();
        const char *f = o["f"].as<const char *>();
        if (!s || !f || !s[0] || !snd_name_valid(f)) continue;
        snprintf(snd_src_map[m].source, NOTIFY_SOURCE_LEN, "%s", s);
        snprintf(snd_src_map[m].file, SND_NAME_LEN, "%s", f);
        m++;
    }
}

/* --- Deferred filesystem work ---
 *
 * Runs on the loop task, which is the task that holds any open playback
 * handle. See the upload staging comment for why this is not done where the
 * upload happens. */
static void snd_commit_poll() {
    char path[40], tmp[40];

    if (snd_delete_pending) {
        snd_delete_pending = false;
        if (snd_src == SND_SRC_FILE && strcmp(snd_playing, snd_delete_name) == 0)
            snd_stop();
        snd_path(snd_delete_name, path, sizeof(path));
        SPIFFS.remove(path);
        /* Assignments pointing at a file that no longer exists would silently
           fall back to a beep forever; clear them so GET /sound tells the
           truth. */
        for (int i = 0; i <= NOTIFY_CRIT; i++)
            if (strcmp(snd_level_file[i], snd_delete_name) == 0) snd_level_file[i][0] = '\0';
        for (int i = 0; i < SND_MAP_MAX; i++)
            if (strcmp(snd_src_map[i].file, snd_delete_name) == 0) {
                snd_src_map[i].source[0] = '\0';
                snd_src_map[i].file[0] = '\0';
            }
        snd_cfg_mark_dirty();
        event_add("sound: removed %s", snd_delete_name);
    }

    if (snd_commit_pending) {
        snd_commit_pending = false;
        if (snd_src == SND_SRC_FILE && strcmp(snd_playing, snd_up_name) == 0)
            snd_stop();
        snd_path(snd_up_name, path, sizeof(path));
        snd_path_tmp(snd_up_name, tmp, sizeof(tmp));
        SPIFFS.remove(path);
        if (!SPIFFS.rename(tmp, path)) {
            SPIFFS.remove(tmp);
            event_add("sound: could not store %s", snd_up_name);
        } else {
            if (snd_up_level >= 0 && snd_up_level <= NOTIFY_CRIT)
                snprintf(snd_level_file[snd_up_level], SND_NAME_LEN, "%s", snd_up_name);
            if (snd_up_source[0]) {
                int slot = -1;
                for (int i = 0; i < SND_MAP_MAX && slot < 0; i++)
                    if (strcmp(snd_src_map[i].source, snd_up_source) == 0) slot = i;
                for (int i = 0; i < SND_MAP_MAX && slot < 0; i++)
                    if (!snd_src_map[i].source[0]) slot = i;
                if (slot >= 0) {
                    snprintf(snd_src_map[slot].source, NOTIFY_SOURCE_LEN, "%s", snd_up_source);
                    snprintf(snd_src_map[slot].file, SND_NAME_LEN, "%s", snd_up_name);
                }
            }
            snd_cfg_mark_dirty();
            event_add("sound: stored %s, %u bytes", snd_up_name, (unsigned)snd_up_written);
        }
    }
}

/* --- Poll ---
 *
 * Called every loop() pass. Does nothing at all when idle: no allocation, no
 * file I/O, one comparison. */
static void snd_poll() {
    if (!snd_ready) return;

    unsigned long now = millis();

    if (snd_cfg_dirty && (long)(now - snd_cfg_save_at) >= 0) {
        snd_cfg_dirty = false;
        snd_cfg_save();
    }

    /* An upload whose connection died still holds the single file handle and
       the single commit slot. Closing a File from this task while the web
       server task might still be inside the body handler is the same bet
       Update.abort() makes in loop() — and it is only taken once the transfer
       has been silent for thirty seconds, by which point there is no handler
       to race. */
    /* Signed for the same reason as the playback watchdog below. The window is
       far narrower here — millis() is read fresh and snd_up_last_ms is stamped
       on the web-server task — but a preemption between those two instructions
       would abandon a healthy upload for nothing, and the file now documents
       this idiom as the one to use. */
    if (snd_up_active && (long)(millis() - snd_up_last_ms) > (long)SND_UPLOAD_STALL_MS) {
        snd_up_error = true;
        snprintf(snd_up_msg, sizeof(snd_up_msg), "upload abandoned after %us without data",
                 (unsigned)(SND_UPLOAD_STALL_MS / 1000));
        snd_up_abandon();
        event_add("sound: upload abandoned after %u bytes", (unsigned)snd_up_written);
    }

    snd_commit_poll();

    /* Arrivals are consumed whether or not anything will be played, so that a
       night's worth of them cannot queue up and all sound at eight in the
       morning — the same rule, for the same reason, as the ring's. */
    uint8_t level;
    char source[NOTIFY_SOURCE_LEN];
    if (notify_take_sound_arrival(&level, source, sizeof(source)))
        snd_play(level, source);

    if (snd_src != SND_SRC_NONE || snd_buf[0].frames || snd_buf[1].frames) {
        snd_pump();
        /* Still busy: push the release out. */
        if (snd_src != SND_SRC_NONE || snd_buf[0].frames || snd_buf[1].frames) {
            snd_off_at = now + SND_LINGER_MS;
            /* A cue that has staged audio but has not handed a single byte to
               the DMA for SND_STALL_MS is not playing, it is wedged — a channel
               left in a state i2s_channel_write() refuses, say. Without this
               the amplifier would keep its clock and the API would keep
               reporting `playing` for the rest of the boot.
             *
             * Read against a FRESH millis() and compared as a signed value, and
             * both halves of that are load-bearing. `now` was sampled at the
             * top of this function, before snd_play() and snd_pump() ran, and
             * both of those stamp snd_progress_ms with a LATER millis(). The
             * first version of this compared `now - snd_progress_ms > 3000` in
             * unsigned arithmetic, so a progress stamp one millisecond in the
             * future underflowed to about 4.29 billion, cleared the threshold
             * instantly and disabled the amplifier on the first pass of every
             * cue — which is exactly what shipped in 0.6.0 and made the device
             * silent. A signed difference reads a future stamp as a small
             * negative number, which is not a stall, and it stays correct
             * across the millis() rollover as well. */
            if ((long)(millis() - snd_progress_ms) > (long)SND_STALL_MS) {
                snd_stalls++;
                event_add("sound: playback stalled after %lu DMA bytes, releasing the amplifier",
                          (unsigned long)snd_cue_bytes);
                snd_stop();
            }
        }
        return;
    }

    /* Everything written. The DMA still holds up to its own depth of audio, so
       the amplifier keeps its clock until the linger expires. */
    if (snd_enabled_hw && snd_off_at != 0 && (long)(now - snd_off_at) >= 0) {
        snd_hw_stop();
        snd_off_at = 0;
    }
}

/* --- Endpoints --- */

static const SkillEndpoint sound_endpoints[] = {
    {"GET",  "/sound",        "Sound state and settings: volume, mute, night, assigned cues"},
    {"POST", "/sound",        "Set {enabled, volume, night_sound, assign} — any subset"},
    {"POST", "/sound/test",   "Play a cue now {level, source} without queueing a message"},
    {"POST", "/sound/upload", "Upload a WAV cue: raw body, ?name=&level=&source="},
    {"GET",  "/sound/files",  "List stored cues with their sizes"},
    {"POST", "/sound/delete", "Remove a stored cue {\"name\":\"uhoh\"}"},
    {NULL, NULL, NULL}
};

static const char *sound_describe() {
    return "## Skill: sound\n\n"
           "The pager out loud. A MAX98357A class-D amplifier on I2S\n"
           "(BCLK=46, LRCLK=40, DIN=7) driving the speaker on header J4.\n"
           "A notification plays a cue; the screen and the ring say the rest.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /sound | `{\"enabled\":true,\"volume\":60,\"night_sound\":false,\"night_now\":false,\"playing\":false,\"dma_bytes\":9600,\"cues\":{...}}` |\n"
           "| POST | /sound | `{\"enabled\":true,\"volume\":45,\"night_sound\":false,\"assign\":{\"level\":\"warn\",\"file\":\"uhoh\"}}` — every field optional |\n"
           "| POST | /sound/test | `{\"level\":\"crit\",\"source\":\"k1c\"}` |\n"
           "| POST | /sound/upload | raw WAV body, `?name=uhoh&level=warn` or `?name=uhoh&source=k1c` |\n"
           "| GET | /sound/files | `{\"files\":[{\"name\":\"uhoh\",\"bytes\":16044}],\"free_bytes\":N}` |\n"
           "| POST | /sound/delete | `{\"name\":\"uhoh\"}` |\n\n"
           "A POST that names a setting it cannot parse changes nothing at\n"
           "all, rather than applying the fields it understood.\n\n"
           "### What it plays\n\n"
           "Out of the box, synthesised beeps: one for info, two for warn,\n"
           "three for crit. The **count** is the message, so the levels are\n"
           "told apart through a closed door rather than only by pitch.\n\n"
           "Real cues are uploaded, never shipped: this is a public\n"
           "repository and third-party audio does not belong in it. A cue\n"
           "assigned to a `source` wins over one assigned to a `level`,\n"
           "because a source is the more specific statement. An assigned file\n"
           "that has gone missing falls back to the beep rather than to\n"
           "silence — a broken assignment should be noticeable.\n\n"
           "### Uploading\n\n"
           "`POST /sound/upload` takes the WAV as the raw request body, with\n"
           "the name and any assignment in the query string. The file must be\n"
           "uncompressed PCM, **mono**, **16-bit**, at 8000, 16000, 32000,\n"
           "44100 or 48000 Hz, and at most 96KB of audio (3 seconds at\n"
           "16kHz). Anything else is rejected with a reason and nothing is\n"
           "stored — never truncated or resampled to fit.\n\n"
           "The rate list is the amplifier's, not a preference: the MAX98357A\n"
           "does not clock 11.025k, 12k, 22.05k or 24k at all, so a WAV at\n"
           "one of those would upload cleanly and then play silently. 88.2k\n"
           "and 96k the part does accept, but they are refused here because\n"
           "the playback buffer is a fixed frame count and its safety margin\n"
           "halves as the rate doubles.\n\n"
           "Names are 1-16 characters of `a-z`, `0-9`, `_` and `-`. Uploading\n"
           "over an existing name replaces it. The store lands on flash on the\n"
           "next main-loop pass, a few milliseconds after the response.\n\n"
           "### Telling silence from failure\n\n"
           "`GET /sound` reports `dma_bytes` and `cue_bytes` beside `played`,\n"
           "and the difference between them is the whole diagnostic. `played`\n"
           "counts cues **started**; `dma_bytes` counts audio the I2S\n"
           "peripheral actually **accepted**. A cue costs `rate * 4` bytes per\n"
           "second, so a 150ms beep at 16kHz is about 9600 bytes. If `played`\n"
           "climbs while `dma_bytes` barely moves, the audio never left the\n"
           "chip and the speaker is not the thing to check. If `dma_bytes`\n"
           "grows correctly and there is still no sound, the fault is after\n"
           "the pins: the speaker on J4, or the amplifier itself.\n\n"
           "`zero_writes` counts writes the DMA had no room for, which is\n"
           "ordinary backpressure and not an error. `stalls` counts cues\n"
           "abandoned because nothing reached the DMA for three seconds, and\n"
           "`last_err` holds the last unexpected `esp_err_t` from a write.\n\n"
           "### Volume and mute\n\n"
           "`volume` is 0-100, applied in software to every sample, and it is\n"
           "meant to be dialled in live against the actual room rather than\n"
           "guessed at. `enabled` false mutes everything. Both survive a\n"
           "reboot. The amplifier has no gain pin under software control, so\n"
           "volume is the only control there is.\n\n"
           "### Night silence\n\n"
           "Between the ring's `night_from` and `night_to` the device is\n"
           "silent, **including for a critical** — being woken at 3am by a\n"
           "DNS alert is the failure this defaults against, and a critical\n"
           "still breathes on the screen. Set `night_sound` true for a pager\n"
           "that can wake you.\n\n"
           "The window is deliberately the ring's, set through `POST /ring`:\n"
           "one device, one idea of night. If the clock has never been set\n"
           "there is no night window and the device stays audible.\n\n"
           "Which also means the **Quiet** row in the on-screen menu silences\n"
           "this skill as well as the ring, and that `night_now` here can turn\n"
           "false because somebody switched the window off from the knob. The\n"
           "hours it was switched off from are in `GET /ring` as\n"
           "`night_saved_from` / `night_saved_to`.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" \\\n"
           "  --data-binary @uhoh.wav -H 'Content-Type: audio/wav' \\\n"
           "  'http://seed.local:8080/sound/upload?name=uhoh&level=crit'\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"volume\":45}' http://seed.local:8080/sound\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"level\":\"crit\"}' http://seed.local:8080/sound/test\n"
           "```\n";
}

static void snd_state_json(JsonDocument &doc) {
    doc["ready"] = snd_ready;
    doc["enabled"] = snd_on;
    doc["volume"] = snd_volume;
    doc["night_sound"] = snd_night_sound;
    doc["night_now"] = ring_night_now();
    /* Same distinction GET /ring draws: a false night_now with an unset clock
       means "no window", not "outside the window". */
    struct tm t;
    doc["clock_set"] = clock_local_time(t);
    doc["muted_now"] = snd_muted_now();
    doc["playing"] = (snd_src != SND_SRC_NONE);
    doc["amp_active"] = snd_enabled_hw;
    doc["rate"] = snd_enabled_hw ? snd_hw_rate : snd_rate;
    doc["played"] = snd_played;
    doc["suppressed"] = snd_suppressed;
    /* The counters that tell "playing" apart from "audible". `played` counts
       cues started; `dma_bytes` counts audio the peripheral actually accepted.
       If the second is not growing by roughly rate*4 bytes per second of cue
       while the first climbs, the sound never left the chip and the speaker is
       not the thing to check. */
    doc["dma_bytes"] = snd_dma_bytes;
    doc["cue_bytes"] = snd_cue_bytes;
    doc["zero_writes"] = snd_zero_writes;
    doc["stalls"] = snd_stalls;
    doc["last_err"] = snd_last_err;

    JsonObject cues = doc["cues"].to<JsonObject>();
    for (int i = 0; i <= NOTIFY_CRIT; i++)
        cues[notify_level_name((uint8_t)i)] = snd_level_file[i][0] ? snd_level_file[i] : "beep";
    JsonObject src = doc["sources"].to<JsonObject>();
    for (int i = 0; i < SND_MAP_MAX; i++)
        if (snd_src_map[i].source[0]) src[snd_src_map[i].source] = snd_src_map[i].file;
}

/*
 * The upload body.
 *
 * Runs on the web-server task and writes to a temp name only. It is the same
 * exception POST /firmware/upload makes: a body has to go somewhere as it
 * arrives, and buffering 96KB in RAM to avoid it would cost more than the
 * write does. Nothing reads the temp name, so no reader can be surprised by
 * it, and the rename into place is left to loop().
 */
static void snd_upload_body(AsyncWebServerRequest *req, uint8_t *data, size_t len,
                            size_t index, size_t total) {
    /* Taken before anything below advances `data` or shortens `len`: the test
       for "this was the last chunk" has to be about the chunk as it arrived,
       not about whatever is left of it after the header was carved off. */
    const bool is_last = (index + len >= total);

    if (index == 0) {
        snd_up_error = false;
        snd_up_msg[0] = '\0';
        snd_up_hdr_len = 0;
        snd_up_hdr_done = false;
        snd_up_written = 0;
        snd_up_data_left = 0;
        snd_up_rate = 0;
        snd_up_level = -1;
        snd_up_source[0] = '\0';

        if (!check_auth(req)) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "auth required");
            return;
        }
        /* One at a time. snd_up_file is a single handle and the commit slot is
           a single name, so a second concurrent upload would write one file
           through the other's handle. The watchdog in snd_poll() is what stops
           an abandoned transfer from holding this forever. */
        if (snd_up_active) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "another upload is in progress");
            return;
        }
        if (snd_commit_pending) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "an upload is still being stored");
            return;
        }
        if (total == 0 || total > SND_UPLOAD_MAX) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "body must be 1..%lu bytes",
                     (unsigned long)SND_UPLOAD_MAX);
            return;
        }

        const char *name = req->hasParam("name") ? req->getParam("name")->value().c_str() : NULL;
        if (!snd_name_valid(name)) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg),
                     "?name= must be 1-16 chars of a-z, 0-9, _ or -");
            return;
        }
        snprintf(snd_up_name, sizeof(snd_up_name), "%s", name);

        if (req->hasParam("level")) {
            uint8_t lv;
            if (!notify_level_parse(req->getParam("level")->value().c_str(), lv)) {
                snd_up_error = true;
                snprintf(snd_up_msg, sizeof(snd_up_msg), "?level= must be info, warn or crit");
                return;
            }
            snd_up_level = (int8_t)lv;
        }
        if (req->hasParam("source")) {
            String v = req->getParam("source")->value();
            if (v.length() == 0 || v.length() >= NOTIFY_SOURCE_LEN) {
                snd_up_error = true;
                snprintf(snd_up_msg, sizeof(snd_up_msg), "?source= must be 1-16 chars");
                return;
            }
            snprintf(snd_up_source, sizeof(snd_up_source), "%s", v.c_str());
        }

        char tmp[40];
        snd_path_tmp(snd_up_name, tmp, sizeof(tmp));
        snd_up_file = SPIFFS.open(tmp, FILE_WRITE);
        if (!snd_up_file) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "could not open storage");
            return;
        }
        snd_up_active = true;
    }

    if (snd_up_error || !snd_up_active) return;
    snd_up_last_ms = millis();

    /* The header is collected before anything is believed about the file. Only
       once it parses does a byte of PCM reach flash, so a rejected upload
       leaves nothing behind but an empty temp file. */
    if (!snd_up_hdr_done) {
        size_t take = SND_HDR_SCAN - snd_up_hdr_len;
        if (take > len) take = len;
        memcpy(snd_up_hdr + snd_up_hdr_len, data, take);
        snd_up_hdr_len += take;

        /* Wait for a full scan window unless the body ends first. */
        if (snd_up_hdr_len < SND_HDR_SCAN && !is_last) return;

        SndWav w;
        const char *err = NULL;
        if (!snd_wav_parse(snd_up_hdr, snd_up_hdr_len, w, &err)) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "%s", err);
            snd_up_abandon();
            return;
        }
        snd_up_hdr_done = true;
        snd_up_rate = w.rate;
        snd_up_data_left = w.data_len;

        /* Everything scanned past the data chunk's start is already PCM. */
        size_t have = snd_up_hdr_len - w.data_off;
        if (have > snd_up_data_left) have = snd_up_data_left;
        if (have > 0) {
            snd_up_file.write(snd_up_hdr + w.data_off, have);
            snd_up_written += have;
            snd_up_data_left -= have;
        }
        /* Whatever of this chunk did not fit in the scan window is ordinary
           PCM and is streamed by the block below. */
        data += take;
        len -= take;
    }

    if (snd_up_data_left > 0 && len > 0) {
        size_t take = len;
        if (take > snd_up_data_left) take = snd_up_data_left;
        if (snd_up_file.write(data, take) != take) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg), "write failed, filesystem may be full");
            snd_up_abandon();
            return;
        }
        snd_up_written += take;
        snd_up_data_left -= take;
    }

    if (is_last) {
        snd_up_file.close();
        snd_up_active = false;
        if (snd_up_data_left > 0) {
            snd_up_error = true;
            snprintf(snd_up_msg, sizeof(snd_up_msg),
                     "body ended %lu bytes short of the declared data chunk",
                     (unsigned long)snd_up_data_left);
            snd_up_abandon();
            return;
        }
        /* Handed to loop(), which owns any open playback handle. */
        snd_commit_pending = true;
    }
}

static void sound_register_routes(AsyncWebServer &server) {

    server.on(AsyncURIMatcher::exact("/sound"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        snd_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/sound"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        /* Into locals first: a body that names three settings and gets one of
           them wrong applies none of them, rather than half. */
        bool on = snd_on;
        uint8_t volume = snd_volume;
        bool night_sound = snd_night_sound;

        if (input["enabled"].is<bool>()) on = input["enabled"].as<bool>();
        else if (!input["enabled"].isNull()) {
            notify_send_error(req, 400, "enabled must be true or false");
            return;
        }
        if (input["night_sound"].is<bool>()) night_sound = input["night_sound"].as<bool>();
        else if (!input["night_sound"].isNull()) {
            notify_send_error(req, 400, "night_sound must be true or false");
            return;
        }
        if (input["volume"].is<int>()) {
            int v = input["volume"].as<int>();
            if (v < 0 || v > 100) {
                notify_send_error(req, 400, "volume must be 0..100 (percent)");
                return;
            }
            volume = (uint8_t)v;
        } else if (!input["volume"].isNull()) {
            notify_send_error(req, 400, "volume must be a number, 0..100");
            return;
        }

        /* An assignment is validated fully before any of it is applied, on the
           same principle. `file` null clears whatever was there. */
        int assign_level = -1, assign_slot = -1;
        char assign_file[SND_NAME_LEN] = "";
        char assign_source[NOTIFY_SOURCE_LEN] = "";
        if (!input["assign"].isNull()) {
            JsonObjectConst a = input["assign"].as<JsonObjectConst>();
            if (a.isNull()) {
                notify_send_error(req, 400, "assign must be an object");
                return;
            }
            bool has_level = !a["level"].isNull();
            bool has_source = !a["source"].isNull();
            if (has_level == has_source) {
                notify_send_error(req, 400, "assign needs exactly one of level or source");
                return;
            }
            if (!a["file"].isNull()) {
                const char *f = a["file"].as<const char *>();
                if (!snd_name_valid(f)) {
                    notify_send_error(req, 400,
                                      "assign.file must be 1-16 chars of a-z, 0-9, _ or -, or null to clear");
                    return;
                }
                snprintf(assign_file, sizeof(assign_file), "%s", f);
            }
            if (has_level) {
                uint8_t lv;
                if (!a["level"].is<const char *>() ||
                    !notify_level_parse(a["level"].as<const char *>(), lv)) {
                    notify_send_error(req, 400, "assign.level must be info, warn or crit");
                    return;
                }
                assign_level = (int)lv;
            } else {
                const char *s = a["source"].as<const char *>();
                if (!s || !s[0] || strlen(s) >= NOTIFY_SOURCE_LEN) {
                    notify_send_error(req, 400, "assign.source must be 1-16 chars");
                    return;
                }
                snprintf(assign_source, sizeof(assign_source), "%s", s);
                for (int i = 0; i < SND_MAP_MAX && assign_slot < 0; i++)
                    if (strcmp(snd_src_map[i].source, assign_source) == 0) assign_slot = i;
                if (assign_slot < 0 && assign_file[0]) {
                    for (int i = 0; i < SND_MAP_MAX && assign_slot < 0; i++)
                        if (!snd_src_map[i].source[0]) assign_slot = i;
                    if (assign_slot < 0) {
                        notify_send_error(req, 400, "no free source slot (8 maximum)");
                        return;
                    }
                }
            }
        }

        bool changed = (on != snd_on) || (volume != snd_volume) ||
                       (night_sound != snd_night_sound);
        snd_on = on;
        snd_volume = volume;
        snd_night_sound = night_sound;

        /* Written straight to RAM from this task, the way the ring's settings
           are. The worst a collision can cost is one cue picked from a
           half-written name, which fails to open and falls back to the beep.
           The flash write is deferred to loop() either way. */
        if (assign_level >= 0) {
            snprintf(snd_level_file[assign_level], SND_NAME_LEN, "%s", assign_file);
            changed = true;
        } else if (assign_slot >= 0) {
            if (assign_file[0]) {
                snprintf(snd_src_map[assign_slot].source, NOTIFY_SOURCE_LEN, "%s", assign_source);
                snprintf(snd_src_map[assign_slot].file, SND_NAME_LEN, "%s", assign_file);
            } else {
                snd_src_map[assign_slot].source[0] = '\0';
                snd_src_map[assign_slot].file[0] = '\0';
            }
            changed = true;
        }

        if (changed) {
            snd_cfg_mark_dirty();
            event_add("sound: %s, volume %u%%, night %s",
                      snd_on ? "on" : "off", (unsigned)snd_volume,
                      snd_night_sound ? "audible" : "silent");
        }

        JsonDocument doc;
        doc["ok"] = true;
        snd_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/sound/test"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        uint8_t level = NOTIFY_INFO;
        char source[NOTIFY_SOURCE_LEN] = "";
        if (body) {
            JsonDocument input;
            DeserializationError err = deserializeJson(input, body);
            free(body);
            if (err != DeserializationError::Ok) {
                notify_send_error(req, 400, "invalid JSON");
                return;
            }
            if (!input["level"].isNull() &&
                (!input["level"].is<const char *>() ||
                 !notify_level_parse(input["level"].as<const char *>(), level))) {
                notify_send_error(req, 400, "level must be info, warn or crit");
                return;
            }
            if (input["source"].is<const char *>())
                snprintf(source, sizeof(source), "%s", input["source"].as<const char *>());
        }

        /* Staged rather than played here: loop() owns the I2S channel and the
           file handle, exactly as it owns the panel and the ring. */
        notify_request_sound(level, source);

        JsonDocument doc;
        doc["ok"] = true;
        doc["level"] = notify_level_name(level);
        const char *file = snd_pick(level, source);
        doc["cue"] = (file && file[0]) ? file : "beep";
        /* Said out loud rather than left a mystery: night silences a test too,
           and the way out is in the same object. */
        bool muted = snd_muted_now();
        doc["suppressed"] = muted;
        if (muted) {
            doc["reason"] = !snd_ready ? "audio unavailable"
                          : !snd_on ? "sound disabled"
                          : "inside the night window (night_sound is false)";
        }
        snd_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/sound/upload"), HTTP_POST, [](AsyncWebServerRequest *req) {
        /* The body handler authenticates the upload itself, but a POST with no
           body never reaches it; checking here gates every path through. */
        if (!require_auth(req)) return;
        /* A POST with no body never reaches the body handler at all, so
           without this it would be answered from whatever the *previous*
           upload left in these variables — reporting somebody else's file as
           successfully stored. */
        if (req->contentLength() == 0) {
            notify_send_error(req, 400, "no audio uploaded: send the WAV as the request body");
            return;
        }
        if (snd_up_error) {
            notify_send_error(req, 400, snd_up_msg);
            return;
        }
        if (!snd_commit_pending) {
            notify_send_error(req, 400, "upload did not complete");
            return;
        }
        JsonDocument doc;
        doc["ok"] = true;
        doc["name"] = snd_up_name;
        doc["bytes"] = snd_up_written;
        doc["rate"] = snd_up_rate;
        doc["seconds"] = serialized(String((float)snd_up_written / 2.0f /
                                           (float)(snd_up_rate ? snd_up_rate : 1), 2));
        if (snd_up_level >= 0) doc["level"] = notify_level_name((uint8_t)snd_up_level);
        if (snd_up_source[0]) doc["source"] = snd_up_source;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, snd_upload_body);

    server.on(AsyncURIMatcher::exact("/sound/files"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        JsonArray arr = doc["files"].to<JsonArray>();
        File root = SPIFFS.open("/");
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            const char *n = f.name();
            /* SPIFFS hands names back with or without the leading slash
               depending on the core; skip past one if it is there. */
            if (n && n[0] == '/') n++;
            if (!n || strncmp(n, "s_", 2) != 0) continue;
            size_t len = strlen(n);
            if (len < 7 || strcmp(n + len - 4, ".wav") != 0) continue;
            char name[SND_NAME_LEN];
            size_t copy = len - 6;   /* strip "s_" and ".wav" */
            if (copy >= sizeof(name)) continue;
            memcpy(name, n + 2, copy);
            name[copy] = '\0';
            JsonObject o = arr.add<JsonObject>();
            o["name"] = name;
            o["bytes"] = (unsigned long)f.size();
        }
        doc["free_bytes"] = (unsigned long)(SPIFFS.totalBytes() - SPIFFS.usedBytes());
        doc["max_bytes"] = (unsigned long)SND_DATA_MAX;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/sound/delete"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be {\"name\":\"...\"}");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }
        const char *name = input["name"].as<const char *>();
        if (!snd_name_valid(name)) {
            notify_send_error(req, 400, "name must be 1-16 chars of a-z, 0-9, _ or -");
            return;
        }
        char path[40];
        snd_path(name, path, sizeof(path));
        if (!SPIFFS.exists(path)) {
            notify_send_error(req, 404, "no cue with that name");
            return;
        }
        if (snd_delete_pending) {
            notify_send_error(req, 409, "a delete is still being applied");
            return;
        }
        snprintf(snd_delete_name, sizeof(snd_delete_name), "%s", name);
        snd_delete_pending = true;

        JsonDocument doc;
        doc["ok"] = true;
        doc["name"] = name;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, handle_body_collect);
}

static const Skill sound_skill = {
    .name = "sound",
    .version = "0.1.0",
    .describe = sound_describe,
    .endpoints = sound_endpoints,
    .register_routes = sound_register_routes
};

/*
 * The channel is created here and left in READY state — initialised, clocks
 * not running — so the amplifier sits in its 340uA standby from boot until the
 * first cue. Nothing enables it except snd_hw_start().
 *
 * GPIO46 is one of this chip's strapping pins. It is read at reset and is an
 * ordinary output afterwards, which is why claiming it here is safe and why it
 * must not be driven before the reset completes. Nothing in setup() touches
 * it, and skills_init() runs long after.
 */
static void skill_sound_init() {
    memset(snd_level_file, 0, sizeof(snd_level_file));
    memset(snd_src_map, 0, sizeof(snd_src_map));
    snd_playing[0] = '\0';
    snd_cfg_load();

    /* I2S1, named rather than I2S_NUM_AUTO. The ESP32-S3 has two controllers
       (SOC_I2S_NUM 2) and the board's PDM microphone can only use one of them:
       the driver rejects a PDM channel that did not land on I2S0 outright —
       "This channel handle is registered on I2S1, but PDM is only supported on
       I2S0". AUTO takes the first free controller, which for the only I2S user
       in the firmware means I2S0, i.e. exactly the one the microphone needs.
       It is also not a promise: what AUTO returns depends on allocation order,
       so leaving it would make the microphone's port a function of skill init
       order. Taking I2S1 here keeps I2S0 free for the microphone. The two
       controllers have independent clock dividers, so the speaker's clock is
       unaffected by anything the RX side later does. */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = SND_DMA_DESCS;
    chan.dma_frame_num = SND_DMA_FRAMES;
    /* An underrun then transmits zeros instead of replaying the stale
       descriptor: a gap in a beep rather than a buzz that repeats. */
    chan.auto_clear = true;

    if (i2s_new_channel(&chan, &snd_tx, NULL) != ESP_OK) {
        event_add("sound: no I2S channel available");
        skill_register(&sound_skill);
        return;
    }

    /* Stereo, not mono: the board's SD_MODE divider puts the MAX98357A in
       (left+right)/2 mode, so both slots carry the same sample and the sum is
       the sample. See the header. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SND_RATE_DEFAULT),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* the part needs no MCLK, by design */
            .bclk = (gpio_num_t)SND_PIN_BCLK,
            .ws   = (gpio_num_t)SND_PIN_LRCLK,
            .dout = (gpio_num_t)SND_PIN_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    if (i2s_channel_init_std_mode(snd_tx, &cfg) != ESP_OK) {
        i2s_del_channel(snd_tx);
        snd_tx = NULL;
        event_add("sound: I2S init failed on BCLK=%d LRCLK=%d DIN=%d",
                  SND_PIN_BCLK, SND_PIN_LRCLK, SND_PIN_DIN);
        skill_register(&sound_skill);
        return;
    }

    snd_ready = true;
    skill_register(&sound_skill);
}
