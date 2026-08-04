/*
 * mic.cpp — the pager's first INPUT channel: the onboard microphone.
 *
 * Everything before this file was output. The screen, the ring and the speaker
 * are three ways of saying the same thing outwards; this is the first thing on
 * the device that takes something in. Hold the knob, talk, let go, and the
 * take is a WAV you can fetch over HTTP.
 *
 *
 * Why capture lands before upload
 * -------------------------------
 * The obvious shape for this feature is "hold the button, the recording goes
 * somewhere". That shape has one failure mode and no way to bisect it: if what
 * arrives is silence, the microphone, the I2S binding, the DC offset, the WAV
 * header and the transport are all equally plausible culprits, and every one
 * of them is invisible from the far end.
 *
 * So capture ships alone and hands the take BACK — `GET /mic/last` returns the
 * exact bytes that were captured, and you listen to them. Whatever comes next
 * gets to start from a recording that is already known to be good, and if the
 * far end then hears nothing, the fault is provably in the new half.
 *
 *
 * The hardware
 * ------------
 * A PDM microphone: a one-bit oversampled bitstream on a data line, clocked by
 * the ESP32-S3. The firmware already advertised the two pins in `/capabilities`
 * (`mic_pins`: DATA=42, CLK=39) and those are the numbers used here.
 *
 *   CLK  GPIO39   ESP32-S3 -> mic   (1.024MHz at the settings below)
 *   DIN  GPIO42   mic -> ESP32-S3   (the PDM bitstream)
 *
 * The part number could not be established from LilyGO's published material,
 * which matters for exactly one decision below (see "Sensitivity" at the end):
 * the sensible knobs are known, the right positions are not, and they are
 * measured on a board rather than guessed at here.
 *
 * The chip does the hard part. The ESP32-S3 has a hardware PDM-to-PCM filter
 * (`SOC_I2S_SUPPORTS_PDM2PCM`), so with `I2S_PDM_DATA_FMT_PCM` what comes out
 * of `i2s_channel_read()` is ordinary signed 16-bit PCM at the requested
 * sample rate. No software decimation, no sinc filter to write.
 *
 *
 * The port, and why it was free
 * -----------------------------
 * PDM RX is hard-rejected on any controller but I2S0 — the driver refuses a
 * PDM channel that did not land there. The ESP32-S3 has two controllers and
 * the speaker was the only I2S user in this firmware, so it had taken I2S0 by
 * default and this file could not have existed. skill_sound_init() now names
 * I2S1 explicitly for exactly that reason, which is what makes the
 * `I2S_NUM_0` below available.
 *
 * Both sides now name their port, so the initialisation ORDER of the two
 * skills does not matter and neither can quietly take the other's controller.
 * That is the whole point of naming them; the alternative — one AUTO and one
 * fixed — would have made the microphone's port a function of skill init
 * order, which is not a promise anybody should have to keep.
 *
 *
 * Clock
 * -----
 * `sample_rate_hz` is the PCM OUTPUT rate on this chip, not the pin rate. At
 * 16kHz with the default `I2S_PDM_DSR_8S` down-sampling the CLK pin runs at
 * 1.024MHz, and the division is exact — there is no divider error to reason
 * about. 16kHz mono 16-bit is 32KB per second, which is speech bandwidth and
 * costs a tenth of what CD-rate stereo would.
 *
 * There is deliberately NO rate allowlist here. sound.cpp has one, and it
 * belongs to the MAX98357A: that part physically cannot clock 22.05kHz on its
 * LRCLK, so a WAV at that rate would play silently. Nothing about that table
 * applies to a microphone, and reusing `snd_rate_ok()` here would have been a
 * constraint borrowed from an unrelated peripheral.
 *
 *
 * The trap: an overrun is SILENT
 * ------------------------------
 * This is the one thing about RX that has no equivalent on the transmit side,
 * and it is the failure most likely to be misread.
 *
 * When the DMA's message queue is full, the RX interrupt DROPS THE OLDEST
 * DESCRIPTOR to make room. Samples vanish. No error code is raised,
 * `i2s_channel_read()` keeps returning ESP_OK, the byte counter keeps climbing
 * and the recording keeps looking healthy — it is simply missing pieces, which
 * on playback sounds like a stutter or a click and reads as "the microphone is
 * bad" rather than "the loop was late".
 *
 * So `on_recv_q_ovf` is registered and counted, and `overruns` is in
 * `GET /mic`. A recording with a non-zero overrun count is one to distrust.
 *
 * The callback is `IRAM_ATTR` and it has to be: this core is built with
 * `CONFIG_I2S_ISR_IRAM_SAFE=y`, so the I2S interrupt carries
 * `ESP_INTR_FLAG_IRAM` and may run with the flash cache disabled. A callback
 * in flash would fault, and so would anything it called. It therefore does one
 * thing — increment a counter that lives in internal SRAM — and says nothing.
 * In particular it must NOT call event_add(): that formats, takes a buffer and
 * lives in flash. The counter is set in the ISR and read in the poll, which is
 * the only division of labour available.
 *
 *
 * DC offset is ours to remove
 * ---------------------------
 * The ESP32-S3's PDM receiver has no high-pass filter — `hp_en`,
 * `hp_cut_off_freq_hz` and `amplify_num` are guarded by
 * `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER`, which this SoC does not define, so
 * those fields do not merely have no effect, they do not exist and writing
 * them does not compile. Whatever standing offset the microphone and the
 * decimator produce arrives in the samples.
 *
 * A constant offset is not audible in itself, but it eats headroom — it moves
 * the whole waveform towards one rail, so the loud half clips early — and it
 * makes any peak measurement a lie, which matters here because the peak is the
 * number the diagnosis leans on. So every sample goes through a one-pole DC
 * blocker, `y[n] = x[n] - x[n-1] + a*y[n-1]` with a = 0.995, which at 16kHz is
 * a high-pass at about 12.7Hz — two octaves below anything a voice produces
 * and far below what the speaker could reproduce anyway.
 *
 * The offset that was removed is reported (`dc_offset` in `GET /mic`, the mean
 * RAW sample over the take) rather than assumed, because its magnitude was not
 * knowable in advance and a large one is itself a finding.
 *
 *
 * The startup window is a guess, and is labelled as one
 * ----------------------------------------------------
 * MIC_SETTLE_MS of audio is thrown away at the head of every recording. The
 * microphone has to power up and the PDM2PCM filter has to fill, and until
 * both have settled the samples are a transient rather than the room.
 *
 * No primary source gives a settle time for either — not the SoC's technical
 * reference for the filter, and not a datasheet for a part whose number could
 * not be established. 80ms is a conservative guess, chosen to be comfortably
 * longer than any plausible answer at a cost of a tenth of a second of audio.
 * It is not a datasheet value and should be MEASURED: record silence, look at
 * the head of the WAV, and shrink this to what the transient actually needs.
 *
 *
 * Why nothing here can block, and the arithmetic
 * ---------------------------------------------
 * Same hard constraint as sound.cpp, pointed the other way: capture must not
 * stall the IR state machine, the ring's frame or AsyncTCP.
 *
 *   1. Every read is `i2s_channel_read(..., timeout_ms = 0)`. Read and write
 *      share one loop in the IDF's i2s_common.c, and `*bytes_read` accumulates
 *      BEFORE the timeout break, so a zero timeout yields whatever is already
 *      in the DMA plus ESP_ERR_TIMEOUT. That is ordinary backpressure and not
 *      an error, exactly as it is for snd_pump(). Two answers are RX-specific:
 *      ESP_ERR_INVALID_STATE at timeout 0 means the channel is not enabled,
 *      and a read cut short by a concurrent disable returns ESP_OK with a
 *      SHORT COUNT — so a short read ends the pass whatever the status was,
 *      rather than only when it came with ESP_ERR_TIMEOUT.
 *
 *   2. Work per pass is bounded by construction. MIC_PUMP_ROUNDS reads of
 *      MIC_CHUNK_FRAMES, a DC-blocker pass over what came back, and a memcpy
 *      of the same into PSRAM. No path scales with the length of the
 *      recording, the size of the buffer or anything a user controls.
 *
 *   3. The DMA absorbs a slow pass, and here is the margin. 8 descriptors of
 *      256 frames is 2048 frames NOMINAL, but the driver creates the message
 *      queue with `desc_num - 1` entries and the drop fires one slot early, so
 *      the honest figure is 7 * 256 = 1792 frames — 112ms at 16kHz. loop()
 *      idles at 10ms and its worst pass is a full-screen repaint, 170*320*2
 *      bytes over 40MHz SPI, around 22ms plus overhead. That is a margin of
 *      roughly five, and the recording screen's own entry repaint is one such
 *      pass at the start of a take rather than a repeating cost.
 *
 *      Drain rate is the other half: 4 rounds * 256 frames is 1024 frames per
 *      pass, and at a 10ms idle cadence that is 102k frames per second against
 *      16k produced — six times real time. Two rounds would still have been
 *      three times over; four is chosen so that a pass which manages only one
 *      short read still catches up on the next.
 *
 * Note what is NOT bounded by this and does not need to be: a 10-second
 * recording is 320KB, and none of it is ever moved, scanned or copied as a
 * whole. It is written once, 512 bytes at a time, and read out by the HTTP
 * response in whatever slices AsyncTCP asks for.
 *
 *
 * Where the audio lives
 * ---------------------
 * PSRAM, allocated once at init and held for the life of the boot. 320KB is
 * two orders of magnitude more than this firmware had ever put on a heap —
 * everything before it fits in a few kilobytes of .bss — and it does not
 * belong in internal SRAM next to the WiFi stack and an async web server.
 *
 * The allocation is CHECKED rather than assumed: `esp_ptr_external_ram()` says
 * where it actually landed and `psram` in `GET /mic` reports the answer. A
 * heap_caps_malloc(MALLOC_CAP_SPIRAM) that silently fell back would work right
 * up until the day it cost somebody their WiFi buffers.
 *
 * The DMA descriptors are a different matter and are NOT here: `I2S_DMA_ALLOC_CAPS`
 * includes MALLOC_CAP_INTERNAL, so the driver's own buffers are internal RAM
 * whatever this file does. PSRAM can only ever be the destination read into,
 * which is 4KB of internal RAM for the ring — mono halves what the speaker's
 * stereo frames would have cost.
 *
 * Ten seconds is a cap, not a target. A held button is a physical thing that
 * can be sat on, wedged or forgotten, and the recording stops itself at the
 * ceiling rather than growing until something else fails.
 *
 *
 * Who owns what
 * -------------
 * The loop task owns the I2S channel, the buffer's write cursor and every
 * counter. The endpoints read; they never enable a channel, never allocate and
 * never write flash. `GET /mic/last` streams from the buffer on the AsyncTCP
 * task, and voice.cpp's uploader reads it from a third task, which is the one
 * genuine sharing in the file. It is handled by refusing to start a recording
 * while any transfer is in flight — see mic_rec_start(), and mic_serving, which
 * counts holders rather than flagging one because two of them can overlap.
 * Memory safety does not rest on that count: every offset a reader computes is
 * bounded by a length snapshot it took for itself, and the allocation is fixed,
 * so the worst a lost race can produce is a garbled take rather than a read out
 * of bounds.
 *
 *
 * Sensitivity, honestly
 * ---------------------
 * If recordings come back quiet or noisy, the documented knob is
 * `I2S_PDM_DSR_16S`, which doubles the PDM clock at the same output rate and
 * gives the decimator twice as many bits to average. That is a HYPOTHESIS to
 * try on hardware, not a fix: without the microphone's part number there is no
 * way to know its sensitivity, its acoustic overload point or the clock rate
 * it was specified at. Measure first — `peak` and `dc_offset` in `GET /mic`
 * exist so that "quiet" is a number rather than an impression.
 */

#include <atomic>

#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

/* --- Wiring (the pins /capabilities already advertises) --- */

#define MIC_PIN_CLK 39
#define MIC_PIN_DIN 42

/* --- Format ---
 *
 * 16kHz mono 16-bit. Speech bandwidth, and the rate every other part of this
 * firmware already speaks: it is sound.cpp's default and the one a synthesised
 * cue plays at, so a take can be handed straight back to the speaker without a
 * resample step existing anywhere. */
#define MIC_RATE        16000
#define MIC_FRAME_BYTES 2      /* one slot, 16 bits — half the speaker's frame */
#define MIC_BYTES_PER_MS ((MIC_RATE * MIC_FRAME_BYTES) / 1000)   /* 32, exact */

/* --- DMA ---
 *
 * 8 descriptors of 256 frames. A frame is one 16-bit slot, so a descriptor is
 * 512 bytes — well inside the 4092-byte per-descriptor ceiling, which at this
 * frame size would allow 2046 frames — and the whole ring is 4KB of internal
 * RAM held for the life of the boot. See the header for the margin this buys
 * and why the honest figure is 7 descriptors rather than 8. */
#define MIC_DMA_DESCS   8
#define MIC_DMA_FRAMES  256

/* --- Per-pass work --- */

/* One read's worth: 512 bytes of internal-RAM scratch, matched to a
   descriptor so a read either empties one or reports the buffer dry. */
#define MIC_CHUNK_FRAMES 256
/* See the drain-rate arithmetic in the header: four rounds is six times real
   time at the idle cadence, and leaves room for a pass that reads short. */
#define MIC_PUMP_ROUNDS  4

/* --- Capacity ---
 *
 * 320KB. A pager message is a sentence, not a monologue, and the ceiling is
 * about what a stuck button may cost rather than what anybody would want to
 * send. */
#define MIC_MAX_SEC   10
#define MIC_MAX_BYTES ((uint32_t)MIC_RATE * MIC_FRAME_BYTES * MIC_MAX_SEC)

/* --- The gesture ---
 *
 * How long the encoder key must be down before it means "record".
 *
 * This is a real trade and it is worth naming. The number has to be long
 * enough that an ordinary click cannot reach it, because ui_poll() suppresses
 * any click held at least this long — otherwise letting go would ALSO open the
 * menu, on top of ending the recording. And it has to be short enough that
 * holding it does not feel like a hang. The user key's AP gesture is 3s, which
 * is right for something you do once a year and wrong for something you do
 * mid-sentence.
 *
 * 700ms is that compromise: about twice a deliberate click and a fifth of the
 * AP hold. A press slower than this is swallowed rather than silently doing
 * something else — and it is not silent, because the recording screen comes up
 * immediately, so the device always says what it thought you meant. */
#define MIC_HOLD_MS 700

/* Audio discarded at the head of every take. A conservative GUESS at the
   combined microphone turn-on and PDM2PCM filter settling time, not a
   datasheet figure — see the header, and measure it. */
#define MIC_SETTLE_MS     80
#define MIC_SETTLE_FRAMES ((uint32_t)MIC_RATE * MIC_SETTLE_MS / 1000)

/* How long a recording may read nothing at all before it is written off as
   wedged. A healthy channel hands back a descriptor every 16ms, so this is two
   orders of magnitude above normal and exists to catch a channel that refuses
   every read — not to police timing. Without it a dead channel would hold the
   recording screen with a frozen timer until the key came up. */
#define MIC_STALL_MS 2000

/* How long a download may go without asking for another slice before the
   buffer is considered released. A connection that dies mid-response never
   reaches the filler's last call, and without this the first such download
   would block every later recording for the life of the boot — the same shape,
   and the same remedy, as the abandoned-upload watchdogs in main.cpp and
   sound.cpp. Generous: AsyncTCP asks for the next slice as soon as the last
   one is acknowledged, so a live transfer never comes near it. */
#define MIC_SERVE_STALL_MS 3000

/* One-pole DC blocker, a = 0.995 in Q15. 0.995 * 32768 = 32604.16, and the
   integer below is a pole of 0.995000 — a high-pass at (1-a)*rate/(2*pi), or
   about 12.7Hz at 16kHz. */
#define MIC_DC_POLE 32604

/* --- State ---
 *
 * All of it owned by the loop task unless marked volatile, and the volatile
 * ones are single aligned scalars read or written by the web-server task. */

static bool mic_ready = false;
static i2s_chan_handle_t mic_rx = NULL;
static bool mic_enabled_hw = false;      /* channel enabled, so CLK is running */

static uint8_t *mic_buf = NULL;          /* MIC_MAX_BYTES in PSRAM */
static bool mic_buf_external = false;    /* did the allocation actually land there */

/* Recording. `mic_len` is the write cursor and only the loop task moves it;
   `mic_take_len` is what a finished take published, and is what the download
   is allowed to read. */
static volatile bool mic_rec = false;
static uint32_t mic_len = 0;
static volatile uint32_t mic_take_len = 0;
static uint32_t mic_settle_left = 0;     /* frames still to discard at the head */
static unsigned long mic_progress_ms = 0;

/* Scratch for one read: internal RAM, and the only place samples are touched
   individually. Reading straight into PSRAM would work — only the DMA's own
   descriptors are required to be internal — but the DC blocker would then run
   over the slower memory for no reason, and the settle-window discard would
   have to shuffle bytes inside the take instead of dropping them before it. */
static int16_t mic_scratch[MIC_CHUNK_FRAMES];

/* DC blocker state, and the offset it removed. The sum is of RAW samples, so
   `dc_offset` reports what the hardware actually produced rather than what is
   left after the filter. */
static int32_t mic_dc_x1 = 0;
static int32_t mic_dc_y1 = 0;
static int64_t mic_dc_sum = 0;
static uint32_t mic_dc_n = 0;

/* Peak absolute sample after DC removal. `mic_take_peak` covers the whole
   take and is what the API reports; `mic_peak_live` is reset every time the
   level meter reads it, so the meter shows the peak since the last frame
   rather than since the beginning. */
static int32_t mic_take_peak = 0;
static int32_t mic_peak_live = 0;

/* Serving: how many readers hold the buffer right now, not whether one does.
 *
 * A bool was wrong as soon as two transfers could overlap — two downloads, or a
 * download and voice.cpp's upload. Whichever finished first cleared the flag
 * while the other was still reading, so mic_rec_start() would then accept a
 * gesture over a buffer being sent and its refusal became a lie. Memory safety
 * never depended on it (every reader carries its own length snapshot and the
 * allocation is fixed), but a refusal that is not true is worse than no refusal
 * at all, because it is believed.
 *
 * Atomic rather than volatile because this one has WRITERS ON TWO TASKS — the
 * web server's fillers and the uploader's task. `mic_serving++` on a volatile is
 * exactly the read-modify-write the overrun callback below explains the standard
 * will not define, and unlike that counter this one would genuinely lose a
 * decrement and hold the buffer for the life of the boot.
 *
 * The stamp stays a plain volatile scalar and stays SHARED: it is the last time
 * ANY holder made progress, so the watchdog releases the buffer only once every
 * holder has gone quiet. Written on the reading tasks, read on the loop task,
 * which is why the timeout below is compared as a signed difference. */
static std::atomic<int> mic_serving{0};
static volatile unsigned long mic_serve_at = 0;

/* Take a hold. The stamp goes first: mic_poll() only looks at the stamp once
   the count is non-zero, so stamping afterwards leaves a window in which a
   brand-new reader is judged against whenever the PREVIOUS one last moved. */
static void mic_serve_acquire(unsigned long now) {
    mic_serve_at = now;
    mic_serving.fetch_add(1);
}

/* Release one, and never below zero. The watchdog can zero the count out from
   under a reader that then finishes anyway; a negative count would leave the
   buffer permanently held against every later recording, which is the exact
   failure the watchdog exists to prevent. */
static void mic_serve_release() {
    int n = mic_serving.load();
    while (n > 0 && !mic_serving.compare_exchange_weak(n, n - 1)) { }
}

/* Counters. sound.cpp keeps its set because 0.6.0 shipped silent behind a
   healthy-looking counter; this set is that discipline pointed at the input,
   where the equivalent lie is a recording of nothing.
 *
 *   records      takes started
 *   rx_bytes     bytes i2s_channel_read() actually returned, for the boot
 *   overruns     descriptors the ISR dropped — samples lost, no error raised
 *   zero_reads   reads the DMA had nothing for: backpressure, not a fault
 *   truncations  takes that hit the ten-second ceiling
 *   stalls       takes abandoned because nothing was read for MIC_STALL_MS
 *   refusals     gestures declined because a download held the buffer
 *   last_err     last unexpected esp_err_t from a read or an enable
 */
static uint32_t mic_records = 0;
static uint32_t mic_rx_bytes = 0;
static volatile uint32_t mic_overruns = 0;   /* written from the ISR */
static uint32_t mic_zero_reads = 0;
static uint32_t mic_truncations = 0;
static uint32_t mic_stalls = 0;
static uint32_t mic_refusals = 0;
static uint32_t mic_serves = 0;
static int mic_last_err = 0;

/* --- WAV header ---
 *
 * A writer, and the exact mirror of the reader in sound.cpp: the same 44-byte
 * canonical layout, so anything this produces parses there by construction.
 * The two are deliberately separate code — this file is included before ui.h
 * and after sound.cpp, and the firmware is one translation unit so calling
 * across would compile, but a writer that depended on a reader's private
 * helpers would make the include order load-bearing for no gain. Forty-four
 * bytes of little-endian integers is not a thing to share.
 *
 * Everything between the marker below and the matching end marker is compiled
 * verbatim on the host by tools/test_wav_header.sh, alongside sound.cpp's
 * parser, so the round trip is tested against both shipping sources rather
 * than copies of them. Keep the two markers on lines of their own. */
/* host-test:begin wavhdr */
#define MIC_WAV_HDR_LEN 44

static void mic_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void mic_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * Canonical 44-byte PCM WAV header, all little-endian.
 *
 *   0  "RIFF"        12  "fmt "        36  "data"
 *   4  36 + data     16  16            40  data
 *   8  "WAVE"        20  1  (PCM)
 *                    22  channels
 *                    24  rate
 *                    28  rate * block align   (byte rate)
 *                    32  block align
 *                    34  bits
 *
 * Mono 16-bit is fixed here rather than parameterised: it is what the PDM
 * channel is configured for, and a header that could describe a format the
 * capture path cannot produce would be a way to lie about the file.
 *
 * `data_len` is written verbatim. The caller is what bounds it — MIC_MAX_BYTES
 * is 320000, so neither the 36+n at offset 4 nor the byte rate can approach
 * the 32-bit ceiling this format leaves them in.
 */
static void mic_wav_header(uint8_t *h, uint32_t rate, uint32_t data_len) {
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint16_t block_align = (uint16_t)(channels * (bits / 8));

    memcpy(h + 0, "RIFF", 4);
    mic_put_le32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);

    memcpy(h + 12, "fmt ", 4);
    mic_put_le32(h + 16, 16);            /* PCM fmt chunk body length */
    mic_put_le16(h + 20, 1);             /* WAVE_FORMAT_PCM, uncompressed */
    mic_put_le16(h + 22, channels);
    mic_put_le32(h + 24, rate);
    mic_put_le32(h + 28, rate * block_align);
    mic_put_le16(h + 32, block_align);
    mic_put_le16(h + 34, bits);

    memcpy(h + 36, "data", 4);
    mic_put_le32(h + 40, data_len);
}
/* host-test:end */

/* --- Duration ---
 *
 * Derived from bytes captured, not from wall clock, and that is deliberate: if
 * the DMA stops delivering, the elapsed time on the recording screen stops
 * with it instead of counting up over audio that does not exist. A timer that
 * lags the hand holding the button is the loss being visible. */
static uint32_t mic_ms_for(uint32_t bytes) {
    return bytes / MIC_BYTES_PER_MS;
}

/* --- Overrun ---
 *
 * Runs in the I2S interrupt with the flash cache possibly disabled, so: IRAM,
 * one internal-RAM counter, no calls, no logging. See the header for why
 * event_add() here would be a fault rather than a style choice. The return
 * value is "did this wake a higher-priority task", which it cannot have. */
static IRAM_ATTR bool mic_on_recv_ovf(i2s_chan_handle_t handle,
                                      i2s_event_data_t *event, void *user_ctx) {
    /* Spelled out rather than ++: a read-modify-write on a volatile is
       deprecated in C++20 because the standard will not say how many accesses
       it is. This is one load and one store, which is what was meant, and the
       ISR is the counter's only writer so nothing can be lost between them. */
    mic_overruns = mic_overruns + 1;
    return false;
}

/* --- Sample processing ---
 *
 * DC removal, the offset measurement and the peak, in one pass over the
 * scratch buffer. Runs at most MIC_PUMP_ROUNDS * MIC_CHUNK_FRAMES times a
 * loop pass and does a handful of integer operations per sample.
 *
 * Ranges, so the arithmetic can be checked rather than trusted: y is clamped
 * to int16 BEFORE it is stored, so mic_dc_y1 * MIC_DC_POLE stays inside
 * +-1.07e9 and cannot overflow int32; the shifted term is therefore within
 * +-32768 and y itself within +-98303 before clamping. The +16384 rounds the
 * shift instead of truncating it, which otherwise biases every sample by up to
 * one LSB in one direction — a DC offset introduced by the DC blocker. */
static void mic_process(int16_t *s, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t x = s[i];
        mic_dc_sum += x;
        mic_dc_n++;

        int32_t y = x - mic_dc_x1 + ((mic_dc_y1 * MIC_DC_POLE + 16384) >> 15);
        mic_dc_x1 = x;
        if (y > 32767) y = 32767;
        else if (y < -32768) y = -32768;
        mic_dc_y1 = y;
        s[i] = (int16_t)y;

        int32_t a = (y < 0) ? -y : y;
        if (a > mic_take_peak) mic_take_peak = a;
        if (a > mic_peak_live) mic_peak_live = a;
    }
}

/* --- Transport --- */

static void mic_hw_stop() {
    if (!mic_enabled_hw) return;
    i2s_channel_disable(mic_rx);
    mic_enabled_hw = false;
}

/*
 * End the take and publish it.
 *
 * The channel is disabled first, so the microphone's clock stops and nothing
 * can arrive between the last read and the length being published. Only then
 * does mic_take_len change, and it is the single value the download depends
 * on — the header it serves is built from that snapshot on the serving task,
 * so there is no second thing to publish and no order to get wrong.
 */
static void mic_rec_stop() {
    if (!mic_rec) return;
    mic_rec = false;
    mic_hw_stop();
    mic_take_len = mic_len;

    long dc = mic_dc_n ? (long)(mic_dc_sum / (int64_t)mic_dc_n) : 0;
    event_add("mic: recorded %lums, peak %ld, dc %ld, overruns %lu",
              (unsigned long)mic_ms_for(mic_len), (long)mic_take_peak, dc,
              (unsigned long)mic_overruns);
}

/*
 * Begin a take.
 *
 * The channel is enabled here and nowhere else, so the microphone is clocked
 * only while something is actually being recorded — the same discipline the
 * speaker's amplifier gets, and for a better reason than power: a microphone
 * that is clocked whenever the device is on is a microphone that is listening
 * whenever the device is on, and this one is not.
 */
static void mic_rec_start(unsigned long now) {
    if (!mic_ready || mic_rec) return;

    /* Somebody is reading the buffer this would overwrite — a download, an
       upload, or several at once. Declining is the only honest answer: there is
       one buffer, and truncating somebody's in-flight take to start another is
       worse than making them press again. */
    if (mic_serving.load() > 0) {
        mic_refusals++;
        event_add("mic: recording declined, a transfer still holds the buffer");
        return;
    }

    esp_err_t err = i2s_channel_enable(mic_rx);
    if (err != ESP_OK) {
        mic_last_err = (int)err;
        event_add("mic: could not enable the channel (err %d)", (int)err);
        return;
    }
    mic_enabled_hw = true;

    /* The previous take is gone the moment a new one starts: mic_take_len goes
       to zero before a single byte is overwritten, so GET /mic/last answers
       "nothing recorded" rather than serving a length that no longer matches
       what is under it. */
    mic_take_len = 0;
    mic_len = 0;
    mic_settle_left = MIC_SETTLE_FRAMES;
    mic_dc_x1 = 0;
    mic_dc_y1 = 0;
    mic_dc_sum = 0;
    mic_dc_n = 0;
    mic_take_peak = 0;
    mic_peak_live = 0;
    mic_progress_ms = now;
    mic_records++;
    mic_rec = true;
}

/*
 * Drain the DMA into the buffer. Bounded, non-blocking, no retry loop.
 *
 * `now` is the caller's single millis() reading, per the rule this file
 * follows everywhere: a poll samples the clock once and passes it down.
 */
static void mic_pump(unsigned long now) {
    for (int round = 0; round < MIC_PUMP_ROUNDS; round++) {
        if (mic_len >= MIC_MAX_BYTES) return;   /* the ceiling; mic_poll() ends it */

        size_t want = sizeof(mic_scratch);
        size_t got = 0;
        /* timeout 0: hands back whatever the DMA already holds and returns
           ESP_ERR_TIMEOUT rather than waiting for the rest. That status is the
           ordinary "nothing more right now" answer and says nothing is wrong;
           anything else is kept, because a channel that refuses every read is
           otherwise indistinguishable from a dead microphone. */
        esp_err_t err = i2s_channel_read(mic_rx, mic_scratch, want, &got, 0);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) mic_last_err = (int)err;

        if (got == 0) {
            mic_zero_reads++;
            break;
        }
        mic_rx_bytes += (uint32_t)got;
        mic_progress_ms = now;

        uint32_t frames = (uint32_t)(got / MIC_FRAME_BYTES);

        /* The settle window, dropped before anything else sees it — including
           the DC blocker and the peak, which would both be skewed by a
           turn-on transient they were never meant to describe. */
        if (mic_settle_left > 0) {
            uint32_t drop = (frames < mic_settle_left) ? frames : mic_settle_left;
            mic_settle_left -= drop;
            frames -= drop;
            if (frames > 0)
                memmove(mic_scratch, mic_scratch + drop, frames * MIC_FRAME_BYTES);
        }

        if (frames > 0) {
            mic_process(mic_scratch, frames);

            uint32_t n = frames * MIC_FRAME_BYTES;
            uint32_t room = MIC_MAX_BYTES - mic_len;
            if (n > room) n = room;
            memcpy(mic_buf + mic_len, mic_scratch, n);
            mic_len += n;
        }

        /* A short read ends the pass whatever the status was. ESP_ERR_TIMEOUT
           is the usual reason, but a read cut short by a concurrent disable
           comes back ESP_OK with a short count, and looping on that would spin
           against a channel that has nothing left to give. */
        if (got < want) break;
    }
}

/*
 * The gesture: hold the encoder key to record, let go to stop.
 *
 * A separate poller reading the pin level directly, exactly like
 * ap_key_poll(), rather than a branch inside ui_poll(). The two state machines
 * share nothing but the pin: ui_poll() sees debounced press-then-release
 * CLICKS and knows nothing about recording, this sees LEVELS and knows nothing
 * about screens. The one place they have to agree is that a press long enough
 * to mean "record" must not also count as a click, which ui_poll() enforces
 * with the same MIC_HOLD_MS threshold the user key's AP hold uses.
 *
 * `fired` is what makes the ceiling safe: a take that stopped itself at ten
 * seconds leaves it set, so nothing restarts until the key has actually come
 * back up. Contact bounce reads as a release and restarts the timer, which is
 * debounce enough for a hold of this length.
 */
static void mic_key_poll(unsigned long now) {
    static unsigned long held_since = 0;
    static bool fired = false;

    if (digitalRead(PIN_ENC_KEY) != LOW) {   /* active low */
        held_since = 0;
        if (fired) {
            fired = false;
            mic_rec_stop();   /* a no-op if the ceiling already ended it */
        }
        return;
    }

    if (held_since == 0) held_since = now;
    if (fired) return;
    /* Signed, as everything in this file is. Both stamps come from the loop
       task here so unsigned would also be correct, but the idiom is uniform on
       purpose: the four defects this project has spent on elapsed-time
       subtraction all looked locally safe too. */
    if ((long)(now - held_since) < (long)MIC_HOLD_MS) return;

    fired = true;
    mic_rec_start(now);
}

/* --- Poll ---
 *
 * Called every loop() pass. When nothing is recording this is one comparison
 * plus a digitalRead: no allocation, no I/O, nothing that scales. */
static void mic_poll() {
    if (!mic_ready) return;

    unsigned long now = millis();

    /* A transfer whose connection died never reaches its last call and would
       hold the buffer against every later recording. The stamp is shared by
       every holder, so this fires only when they have ALL gone quiet — one
       stalled reader alongside a healthy one keeps the buffer held, which is
       the conservative answer and the right one.
       Signed, because mic_serve_at is stamped on the reading tasks: a slice
       served between the millis() reading and the subtraction makes the stamp
       NEWER than the reading, and in unsigned arithmetic those few milliseconds
       read as about 4.29 billion — which would clear the timeout instantly and
       release the buffer out from under a perfectly healthy transfer. That is
       the same shape as the bug that made 0.6.0 silent. */
    if (mic_serving.load() > 0 && (long)(now - mic_serve_at) > (long)MIC_SERVE_STALL_MS) {
        mic_serving.store(0);
        event_add("mic: transfer abandoned, buffer released");
    }

    mic_key_poll(now);

    if (!mic_rec) return;

    mic_pump(now);

    if (mic_len >= MIC_MAX_BYTES) {
        mic_truncations++;
        event_add("mic: recording stopped at the %us ceiling", (unsigned)MIC_MAX_SEC);
        mic_rec_stop();
        return;
    }

    /* The hazard this shape exists for, and why it cannot bite here.
     *
     * sound.cpp's equivalent watchdog compared a `now` sampled at the top of
     * the poll against a progress stamp written LATER in the same pass. That
     * stamp was therefore in the future relative to the reading, an unsigned
     * subtraction turned a few milliseconds into about 4.29 billion, and every
     * cue was written off as stalled on its first pass — which is what shipped
     * silent in 0.6.0.
     *
     * The fix applied here is at the source rather than at the comparison:
     * this poll samples millis() ONCE and hands `now` down to mic_pump(), so
     * the stamp is that same reading and can never be ahead of it. The signed
     * cast is kept anyway, because it costs nothing and because one idiom for
     * every elapsed-time subtraction in this firmware is worth more than each
     * site being separately argued about. */
    if ((long)(now - mic_progress_ms) > (long)MIC_STALL_MS) {
        mic_stalls++;
        event_add("mic: no samples for %ums, recording abandoned after %lu bytes",
                  (unsigned)MIC_STALL_MS, (unsigned long)mic_len);
        mic_rec_stop();
    }
}

/* --- What the on-device UI asks ---
 *
 * ui.h is included after this file and reads these; nothing here knows a
 * screen exists. */

static bool mic_is_recording() {
    return mic_rec;
}

/* Milliseconds of audio actually captured in the current or last take. */
static uint32_t mic_elapsed_ms() {
    return mic_ms_for(mic_rec ? mic_len : mic_take_len);
}

static uint32_t mic_last_take_ms() {
    return mic_ms_for(mic_take_len);
}

static int32_t mic_last_take_peak() {
    return mic_take_peak;
}

/* Peak since the last call, 0-100 of full scale. RESET ON READ, so it is the
   level meter's alone — anything else reading it would steal frames from the
   meter and see a level that depends on who asked last. */
static int mic_level_pct() {
    int32_t p = mic_peak_live;
    mic_peak_live = 0;
    int pct = (int)((p * 100) / 32767);
    return pct > 100 ? 100 : pct;
}

/* --- Endpoints --- */

static const SkillEndpoint mic_endpoints[] = {
    {"GET", "/mic",      "Microphone state, capture counters and I2S channel facts"},
    {"GET", "/mic/last", "The last recording as a 16kHz mono 16-bit WAV"},
    {NULL, NULL, NULL}
};

static const char *mic_describe() {
    return "## Skill: mic\n\n"
           "The pager's ear. A PDM microphone on I2S0 (CLK=39, DATA=42),\n"
           "captured as 16kHz mono 16-bit PCM by the chip's own PDM-to-PCM\n"
           "filter. **Hold the encoder knob** to record, let go to stop; the\n"
           "screen shows elapsed time and a level meter while it runs.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /mic | `{\"recording\":false,\"take_ms\":2400,\"peak\":8100,\"rx_bytes\":76800,\"overruns\":0,...}` |\n"
           "| GET | /mic/last | The last take as `audio/wav`, 44-byte header + PCM |\n\n"
           "### Recording\n\n"
           "Hold the knob for 700ms and the device starts capturing; release\n"
           "and it stops. A take is capped at **10 seconds** (320KB), which is\n"
           "a ceiling against a stuck button rather than a target. The first\n"
           "80ms is discarded while the microphone and the filter settle, so a\n"
           "very short hold can produce a very short take.\n\n"
           "The microphone is clocked only while a recording is running. It is\n"
           "not enabled at boot and there is no way to start one over the\n"
           "network: recording takes a hand on the device.\n\n"
           "Each take replaces the last. There is one buffer, so starting a\n"
           "recording while a transfer is still reading it — `GET /mic/last`,\n"
           "or the voice skill's upload — is declined rather than allowed to\n"
           "overwrite what is being sent. `refusals` counts that, and pressing\n"
           "again once the transfer finishes works.\n\n"
           "### Telling silence from failure\n\n"
           "A recording of nothing and a microphone that never ran produce the\n"
           "same file, so four numbers separate them. Read them in order.\n\n"
           "`rx_bytes` is what the I2S peripheral actually handed over. Audio\n"
           "costs 32000 bytes per second, so a two-second take is about 64000.\n"
           "If this barely moves while `records` climbs, no samples are\n"
           "arriving and the microphone is not the thing to check — look at\n"
           "`i2s_port` (must be 0, PDM receive works nowhere else),\n"
           "`bclk_hz` (should read 1024000) and `last_err`.\n\n"
           "`peak` is the loudest sample in the take, 0-32767, after DC\n"
           "removal. Bytes arriving with a peak near zero means the channel\n"
           "works and the signal does not: that is a wiring or a sensitivity\n"
           "question, not a driver one.\n\n"
           "`dc_offset` is the mean raw sample before the high-pass. A large\n"
           "one is worth knowing about — it eats headroom and makes everything\n"
           "clip early — and it is reported rather than assumed because this\n"
           "SoC has no hardware high-pass and the magnitude was unknown.\n\n"
           "`overruns` is the one that lies quietly. When the loop is late the\n"
           "receive interrupt DROPS the oldest DMA descriptor: samples vanish,\n"
           "no error is raised, and the take is simply missing pieces. Any\n"
           "non-zero value means the audio has holes in it, however healthy\n"
           "everything else looks. `zero_reads` is the opposite and is\n"
           "harmless — the buffer was empty when asked, which is what an idle\n"
           "device looks like.\n\n"
           "### Fetching a take\n\n"
           "`GET /mic/last` streams the buffer with a canonical 44-byte WAV\n"
           "header in front, `Content-Length` set and a filename attached, so\n"
           "it plays in anything and saves cleanly. It answers 404 before the\n"
           "first recording and 409 while one is running.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" http://seed.local:8080/mic\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -OJ \\\n"
           "  http://seed.local:8080/mic/last\n"
           "```\n";
}

static void mic_state_json(JsonDocument &doc) {
    doc["ready"] = mic_ready;
    doc["recording"] = mic_rec;
    doc["rate"] = MIC_RATE;
    doc["max_sec"] = MIC_MAX_SEC;
    doc["hold_ms"] = MIC_HOLD_MS;

    doc["take_bytes"] = mic_take_len;
    doc["take_ms"] = mic_ms_for(mic_take_len);
    doc["elapsed_ms"] = mic_ms_for(mic_rec ? mic_len : mic_take_len);
    doc["peak"] = mic_take_peak;
    doc["dc_offset"] = mic_dc_n ? (long)(mic_dc_sum / (int64_t)mic_dc_n) : 0;

    /* Where the 320KB actually landed. Reported rather than assumed: a
       MALLOC_CAP_SPIRAM allocation that quietly came out of internal RAM would
       work until the day it cost somebody their WiFi buffers. */
    doc["buf_bytes"] = MIC_MAX_BYTES;
    doc["psram"] = mic_buf_external;

    doc["records"] = mic_records;
    doc["rx_bytes"] = mic_rx_bytes;
    doc["overruns"] = mic_overruns;
    doc["zero_reads"] = mic_zero_reads;
    doc["truncations"] = mic_truncations;
    doc["stalls"] = mic_stalls;
    doc["refusals"] = mic_refusals;
    doc["serves"] = mic_serves;
    doc["last_err"] = mic_last_err;

    /* Straight from the driver, so a microphone that failed to land on I2S0 is
       diagnosable over the API instead of only on a serial console nobody is
       watching. bclk_hz is the CLK pin: 1024000 at 16kHz with the default 8x
       down-sampling, and a different number means the clock is not what this
       file thinks it configured. */
    i2s_chan_info_t info;
    if (mic_rx && i2s_channel_get_info(mic_rx, &info) == ESP_OK) {
        doc["i2s_port"] = (int)info.id;
        doc["bclk_hz"] = info.bclk_hz;
        doc["dma_buf_bytes"] = info.total_dma_buf_size;
        doc["enabled"] = info.is_enabled;
    }
}

/*
 * Serving the take.
 *
 * This is the first binary response in the firmware — everything else ends in
 * req->send(200, "application/json", ...) — so the choice is worth writing
 * down. There were three options.
 *
 *   req->send(SPIFFS, path, "audio/wav") is the usual answer and is wrong
 *   here: the recording lives in PSRAM and has never been near the
 *   filesystem. Writing 320KB to SPIFFS on the loop task purely so a helper
 *   could read it back would add a flash write, a wear budget and a failure
 *   mode to a path that has none of them.
 *
 *   sendChunked() needs no length, which sounds convenient and costs the
 *   Content-Length header. Media players want that header — some will not
 *   seek, and a few will not play at all, without it — and we know the length
 *   exactly, so giving it up buys nothing.
 *
 *   beginResponse(contentType, len, filler) is what is used: a known length
 *   and a callback AsyncTCP drives, asking for the next slice as the previous
 *   one is acknowledged. No copy of the audio is made anywhere.
 *
 * The filler runs on the AsyncTCP task, not the loop task, so it must not
 * touch the I2S channel, must not allocate and must not depend on anything the
 * loop task is in the middle of. It does none of those: `len` is snapshotted
 * when the response is created and captured by value, the header is rebuilt
 * from that snapshot on the stack rather than read from shared memory, and
 * every offset it computes is bounded by the snapshot. mic_rec_start()
 * declines while a response is in flight, so the ordinary case never races at
 * all — and because the bounds come from the snapshot and the allocation is
 * fixed for the life of the boot, even a lost race can only produce a garbled
 * take, never a read outside the buffer.
 */
static void mic_register_routes(AsyncWebServer &server) {

    server.on(AsyncURIMatcher::exact("/mic"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        mic_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/mic/last"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (mic_rec) {
            req->send(409, "application/json",
                      "{\"error\":\"a recording is in progress\"}");
            return;
        }
        uint32_t len = mic_take_len;
        if (len == 0 || !mic_buf) {
            req->send(404, "application/json",
                      "{\"error\":\"nothing recorded yet, hold the knob\"}");
            return;
        }

        mic_serves++;
        mic_serve_acquire(millis());

        const size_t total = (size_t)MIC_WAV_HDR_LEN + len;
        /* `held` is per-response state, which is what makes the release happen
           exactly once. The library stops calling the filler the moment the
           declared content length has been filled, so the index >= total branch
           below is defensive — but with a counter rather than a flag, releasing
           twice would take somebody else's hold with it. Mutable, because a
           std::function invokes its target as non-const. */
        AsyncWebServerResponse *res = req->beginResponse("audio/wav", total,
            [len, total, held = true](uint8_t *buf, size_t max_len,
                                      size_t index) mutable -> size_t {
                if (index >= total) {
                    if (held) { held = false; mic_serve_release(); }
                    return 0;
                }
                mic_serve_at = millis();

                size_t n;
                if (index < MIC_WAV_HDR_LEN) {
                    /* Rebuilt per call rather than held in a shared buffer.
                       Forty-four bytes on the stack, touched only by the first
                       slice, and it removes the last thing the two tasks would
                       otherwise have had to agree about. */
                    uint8_t hdr[MIC_WAV_HDR_LEN];
                    mic_wav_header(hdr, MIC_RATE, len);
                    n = MIC_WAV_HDR_LEN - index;
                    if (n > max_len) n = max_len;
                    memcpy(buf, hdr + index, n);
                } else {
                    n = total - index;
                    if (n > max_len) n = max_len;
                    memcpy(buf, mic_buf + (index - MIC_WAV_HDR_LEN), n);
                }

                if (index + n >= total && held) { held = false; mic_serve_release(); }
                return n;
            });
        res->addHeader("Content-Disposition", "attachment; filename=\"mic.wav\"");
        req->send(res);
    });
}

static const Skill mic_skill = {
    .name = "mic",
    .version = "0.1.0",
    .describe = mic_describe,
    .endpoints = mic_endpoints,
    .register_routes = mic_register_routes
};

/*
 * The channel is created here and left in READY state — initialised, clock not
 * running — so the microphone is not clocked, and therefore not listening,
 * until somebody holds the knob.
 *
 * Every failure below registers the skill anyway and leaves mic_ready false.
 * A device whose microphone did not come up should still answer GET /mic and
 * say so, with last_err in it; dropping the endpoint would make a broken
 * microphone look like an old firmware.
 */
static void skill_mic_init() {
    /* GPIO0, a strapping pin: read, never driven. ui_init() sets the same mode
       for the click state machine, but it runs after skills_init() and this
       file must not depend on that order — a pin the gesture reads should be
       configured by the code that reads it. */
    pinMode(PIN_ENC_KEY, INPUT_PULLUP);

    /* PSRAM, once, for the life of the boot. Allocating per recording would
       put a 320KB request on the critical path of a button press and make the
       failure mode "the gesture sometimes does nothing". */
    mic_buf = (uint8_t *)heap_caps_malloc(MIC_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!mic_buf) {
        event_add("mic: no PSRAM for a %luKB capture buffer",
                  (unsigned long)(MIC_MAX_BYTES / 1024));
        skill_register(&mic_skill);
        return;
    }
    mic_buf_external = esp_ptr_external_ram(mic_buf);
    if (!mic_buf_external) {
        /* Not fatal — the capture works either way — but it means 320KB just
           came out of the same internal heap WiFi and the web server live in,
           and that is worth an event rather than a surprise later. */
        event_add("mic: capture buffer is in INTERNAL ram, not PSRAM");
    }

    /* I2S0, named: PDM receive is rejected outright on any other controller.
       sound.cpp holds I2S1 for the same reason, so neither can take the
       other's port whatever order skills_init() runs them in. */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = MIC_DMA_DESCS;
    chan.dma_frame_num = MIC_DMA_FRAMES;
    /* auto_clear is a transmit-side setting — it zeroes a descriptor that was
       sent twice — and has no receive meaning. Left false. */

    /* RX is the THIRD argument. The transmit side passes its handle second,
       and the two are not interchangeable. */
    if (i2s_new_channel(&chan, NULL, &mic_rx) != ESP_OK) {
        event_add("mic: no I2S channel available on I2S0");
        skill_register(&mic_skill);
        return;
    }

    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_RATE),
        /* The PCM variant, so the chip's PDM-to-PCM filter runs and reads come
           back as ordinary signed 16-bit samples. The macro already selects
           I2S_PDM_SLOT_LEFT for mono, so there is nothing to override — and
           there is nothing to disable either: this SoC does not define
           SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER, so hp_en and its neighbours are
           not merely ineffective, they are absent from the struct. The
           high-pass is mic_process()'s job for exactly that reason. */
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)MIC_PIN_CLK,
            .din = (gpio_num_t)MIC_PIN_DIN,
            .invert_flags = { .clk_inv = false },
        },
    };

    if (i2s_channel_init_pdm_rx_mode(mic_rx, &cfg) != ESP_OK) {
        i2s_del_channel(mic_rx);
        mic_rx = NULL;
        event_add("mic: PDM init failed on CLK=%d DATA=%d",
                  MIC_PIN_CLK, MIC_PIN_DIN);
        skill_register(&mic_skill);
        return;
    }

    /* Only legal before the channel starts. A failure here is not fatal —
       capture works without it — but it costs the one signal that a dropped
       sample ever happened, so it is logged rather than ignored. */
    i2s_event_callbacks_t cbs = {
        .on_recv = NULL,
        .on_recv_q_ovf = mic_on_recv_ovf,
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };
    if (i2s_channel_register_event_callback(mic_rx, &cbs, NULL) != ESP_OK)
        event_add("mic: overrun callback not registered, dropped samples will be silent");

    mic_ready = true;
    skill_register(&mic_skill);
}
