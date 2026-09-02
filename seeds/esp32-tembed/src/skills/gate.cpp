/*
 * skills/gate.cpp — sub-GHz OOK transmitter skill for the T-Embed CC1101
 *
 * The gate-remote skill, C1: a raw 433 MHz OOK sender. A fixed-code gate or
 * garage remote is a sequence of mark/space durations on the air; this skill
 * takes such a sequence over HTTP and keys the CC1101's carrier to it. The
 * named encoders (CAME and friends), the stored code and the /gate/open hook
 * land on top of this in C2/C3 — everything they need (a validated raw
 * buffer, a job machine, a stop switch) is already here.
 *
 * Endpoints:
 *   POST /gate/send    — send one raw OOK frame {raw[], repeat}
 *   GET  /gate/status  — progress of the running (or last) job
 *   POST /gate/stop    — abort mid-job
 *
 * All three are registered with AsyncURIMatcher::exact() — see the note at
 * the top of skills/ir.cpp: under the library's default BackwardCompatible
 * matcher /gate/send would also answer /gate/stop, and being registered
 * first it wins.
 *
 * The SPI bus, honestly — and how the chip is actually reached
 * -----------------------------------------------------------
 * The CC1101 sits on the same HSPI as the ST7789 (pins 11/9/10), and after
 * tft.init() the display library owns that bus. Three roadmaps were measured
 * on this exact hardware, and only one of them works end to end:
 *
 *   1. Borrowing the display's SPI instance (tft.getSPIinstance()) at 1 and
 *      4 MHz, MODE0 and MODE3: config-register reads and writes return sane
 *      values (PKTCTRL0 write -> readback round-trips), but STATUS-register
 *      reads (the 0xC0 header — PARTNUM, VERSION, MARCSTATE) come back as a
 *      constant 0x0F, and some config writes silently miss (MDMCFG2 read
 *      back 0x02 where the reset default is 0x73). The chip is answering,
 *      the path is not trustworthy for register access.
 *   2. A second SPIClass begun after the display: on arduino-esp32 3.x the
 *      second begin() re-assigns pins 9/10/11 through the peripheral manager
 *      and tears the display's bus out from under TFT_eSPI — the boot hangs
 *      in display_status(). (The official LilyGO example does begin a second
 *      SPIClass after initTFT(), but on the older 2.x core, where no
 *      peripheral-manager reassignment happens.)
 *   3. A dedicated SPIClass for the gate, begun ONCE in the boot probe —
 *      before display_init() claims the bus, exactly like probe_cc1101(),
 *      which provably works — and never ended. Every gate access afterwards
 *      re-programs the same peripheral per transaction (clock 1 MHz, MODE0),
 *      and the display's draws re-program it for themselves on every
 *      transaction of their own. Safety rests on the single-task argument,
 *      which holds for both drivers: TFT_eSPI draws only from the loop task
 *      (web handlers set display_force, main.cpp:609-612), and every CC1101
 *      access below happens from gate_poll()/the job machine on the same
 *      loop task. Between draws TFT_eSPI raises its own CS
 *      (TFT_eSPI.cpp:103-106), so the panel is never listening while the
 *      gate drives the bus.
 *
 * Roadmap 3 is what this file implements. The instance is begun once, at
 * hw_probe() time, and CS on GPIO12 (parked HIGH by setup()) gates every
 * access.
 *
 * Transmission
 * ------------
 * The chip runs in asynchronous serial mode (PKTCTRL0 = 0x32: async serial,
 * infinite length — the same recipe RadioLib's directMode(false) and the
 * Flipper async presets use): GDO0 becomes the data input, and the modulator
 * keys the carrier to whatever level the pin carries, through PA_TABLE[0]
 * (0x00 = no power) for low and PA_TABLE[1] (0xC0 = 10 dBm at 433 MHz) for
 * high. The waveform is produced by bit-banging GDO0 from the loop task with
 * delayMicroseconds — NOT by RMT, because the S3 has four RMT TX memory
 * blocks and both are already taken (ir.cpp takes two, ring.cpp the other
 * two; a third rmtInit would fail rather than steal, per the arithmetic in
 * ring.cpp's header). A gate frame is 10-50 ms of 512-us-scale symbols, so a
 * CPU-held burst inside gate_poll is precise enough and costs the loop one
 * frame per pass; WiFi, lwIP and async_tcp all live on other tasks and do
 * not notice.
 *
 * Between jobs the ESP32 side keeps GDO0 driven LOW: in ASK/OOK a low data
 * input means PA_TABLE[0], i.e. no carrier, and an INPUT pin would float and
 * could key the carrier spuriously. GPIO3 doubles as a boot strap, which the
 * board accepts by wiring GDO0 there — strapping is sampled only at reset.
 *
 * The register set is deliberately small, and every value in it is sourced
 * from the RadioLib driver source and the CC1101 datasheet (checked out in
 * tembed-pager/firmware/.pio/libdeps/cardputer/RadioLib), not from memory:
 * reset defaults cover the FSCAL and TEST registers (RadioLib's begin()
 * touches neither), MCSM0 gets FS_AUTOCAL_IDLE_TO_RXTX + pin control off
 * (CC1101.cpp:1148-1151), IOCFG0 = GDOX_SERIAL_DATA_ASYNC 0x0D, IOCFG2 =
 * HIGH_Z 0x2E, PKTCTRL0 = 0x32, PKTLEN = 0xFF, MOD_FORMAT = ASK/OOK
 * (setOOK, CC1101.cpp:810-837) with FREND0.PA_POWER = 1 (CC1101.cpp:819 —
 * without it a '1' keys PA_TABLE[0] and nothing leaves the chip), and
 * PA_TABLE = {0x00, 0xC0} via setOutputPower's ASK branch (CC1101.cpp:
 * 628-635). The frequency is computed from the 26 MHz crystal, which is what
 * RadioLib assumes (CC1101.h:13) and what Bruce ships for this board. The
 * data rate is 250 kbaud — the async sampling grid is 8x the data rate
 * (datasheet 27.1), so this bounds raw edge error to ~1 us, where 4.8 kbaud
 * would give 26 us. And before any of that, the board's RF band switch
 * (SW1/SW0 on GPIO47/48, both HIGH for 434 MHz per the LilyGO example and
 * Bruce's board map) is driven, or the transmit radiates through whatever
 * band the floating pins pick.
 */

/* --- Limits --- */

#define GATE_MAX_RAW    512    /* mark/space entries accepted by /gate/send */
#define GATE_ENTRY_MIN  10     /* microseconds; below this the loop cost of
                                  digitalWrite + delayMicroseconds starts to
                                  bend the symbol */
#define GATE_ENTRY_MAX  50000  /* one run must stay far under anything the
                                  watchdog cares about; 50 ms covers every
                                  fixed-code header with room to spare */
#define GATE_FRAME_MAX_US 250000  /* sum of one frame; bounds each loop-pass
                                     block to a quarter second */
#define GATE_REPEAT_MAX 10
#define GATE_REPEAT_GAP_MS 20   /* between repeats: long enough for the
                                   receiver to count two frames, short enough
                                   that ten repeats stay under three seconds */
#define GATE_TX_READY_MS 5      /* STX -> GDO2 (PLL lock): calibration ~724us
                                   plus settle; MEASURED: a 5 ms window gave
                                   10/10 locked frames where 20 ms never saw
                                   the lock on the first attempt, so the
                                   tight window is not a guess — it is the
                                   configuration that worked */

/* The RF path on this board runs through a band switch, not a TX/RX switch:
   SW1 (GPIO47) and SW0 (GPIO48) select the antenna filter band, and both go
   HIGH for 434 MHz — per the LilyGO CC1101+NFC shield example (setFreq,
   FREQ_434MHZ) and Bruce's board map for this exact hardware. Without
   driving them the transmit is radiated through whatever band the pins
   float to. */
#define PIN_RF_SW1      47
#define PIN_RF_SW0      48

/* --- Register map (only what this driver touches) --- */

#define CC1101_SRES     0x30
#define CC1101_SFTX     0x3B
#define CC1101_SIDLE    0x36
#define CC1101_STX      0x35
#define CC1101_SNOP     0x3D

#define CC1101_IOCFG2   0x00
#define CC1101_IOCFG0   0x02
#define CC1101_PKTLEN   0x06
#define CC1101_PKTCTRL0 0x08
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_MCSM0    0x18
#define CC1101_FREND0   0x22
#define CC1101_PARTNUM  0x30   /* status regs 0x30-0x3D read with the burst bit */
#define CC1101_VERSION  0x31
#define CC1101_MARCSTATE 0x35
#define CC1101_PATABLE  0x3E

#define CC1101_SPI_READ   0x80
#define CC1101_SPI_BURST  0x40
#define CC1101_SPI_STATUS 0x40

/* Values sourced from RadioLib's CC1101.h and the CC1101 datasheet
   (SWRS061I), see the file header above. */
#define CC1101_GDO0_ASYNC_DATA 0x0D  /* GDO0 is the async serial data input */
#define CC1101_GDOX_PLL_LOCK   0x0A  /* GDO2 drives HIGH while the PLL is locked —
                                        the frame verifier, read as a PIN */
#define CC1101_GDOX_HIGH_Z     0x2E
#define CC1101_MOD_ASK_OOK     0x30  /* MDMCFG2 [6:4] = ASK/OOK */
#define CC1101_PKTCTRL0_ASYNC  0x32  /* format async [5:4]=11, infinite len [1:0]=10 */
#define CC1101_MCSM0_AUTOCAL   0x10  /* [5:4] = calibrate on every idle->RX/TX */
#define CC1101_MCSM0_PINCTRL   0x02  /* [1:0] = disable pin control */
#define CC1101_PA_POWER_1      0x01  /* FREND0 [2:0] = 1: a '1' keys PA_TABLE[1].
                                        RadioLib's setOOK writes exactly this
                                        (CC1101.cpp:819) and the Flipper async
                                        preset ships FREND0=0x11 — with
                                        PA_POWER=0 both levels index
                                        PA_TABLE[0] and nothing leaves the
                                        chip. */
#define CC1101_PA_OFF          0x00  /* PA_TABLE[0]: what a '0' transmits */
#define CC1101_PA_ON           0xC0  /* PA_TABLE[1]: 10 dBm at 433 MHz */
#define CC1101_MARC_TX         0x13  /* MARCSTATE 0x12 is FSTXON, 0x13 is TX */

#define CC1101_XTAL_HZ   26000000UL
#define GATE_FREQ_HZ     433920000UL

/* --- SPI access: write-only at runtime, configured at boot-probe time ---
 *
 * Measured on this hardware: through the display's SPI instance the chip's
 * WRITES land (a PKTCTRL0 write -> readback round-trips), but STATUS reads
 * (the 0xC0 header — PARTNUM, VERSION, MARCSTATE) return a constant 0x0F
 * and some reads lie (MDMCFG2 read 0x02 where the reset default is 0x73).
 * The boot-probe instance, begun before the display claims the bus, is the
 * one path that reads and writes truthfully — so the chip is configured
 * completely there (gate_configure, called from probe_cc1101 before
 * spi.end()), and at runtime the job machine sends WRITE-ONLY strobes
 * through the display's instance and verifies every frame through the GDO2
 * PIN (IOCFG2 = 0x0A, PLL in lock — a digitalRead, not an SPI read).
 * Nothing at runtime depends on reading a register.
 *
 * Task safety: TFT_eSPI draws only from the loop task (web handlers set
 * display_force, main.cpp:609-612), and every gate access below happens on
 * the same loop task — no interleaving. Between draws TFT_eSPI raises its
 * own CS (TFT_eSPI.cpp:103-106), so the panel is never listening. */

static SPIClass *gate_bus = nullptr;   /* boot: probe's instance; runtime: the display's */
static bool gate_ready = false;

/* The runtime bus: the display's instance, lazily taken on the first
 * strobe — by then display_init() has long run and begun this object. */
static SPIClass *gate_runtime_bus() {
    static SPIClass *b = nullptr;
    if (!b) b = &tft.getSPIinstance();
    return b;
}

/* --- Configure-time helpers, bound to the probe bus ---
 *
 * gate_bus points at the instance passed into gate_configure() for exactly
 * the duration of that call — it is the probe's stack object, alive only
 * while probe_cc1101() is on the stack. Everything here is boot-time only.
 * The runtime strobes go through gate_runtime_bus() instead. */

/* --- Raw SPI access, transaction-agnostic ---
 *
 * MEASURED ON THIS HARDWARE (2026-09-02, five build-test cycles): the first
 * byte(s) of a FRESH SPI transaction — PARTNUM/VERSION/MARCSTATE reads,
 * some strobes — arrive at the chip corrupted (status reads return a
 * constant 0x0F, ~10-30% of write-only strobes are dropped), no matter which
 * SPIClass object owns them, at 1 MHz and 4 MHz, MODE0 and MODE3. The boot
 * probe never sees any of this, because ALL of its accesses live inside ONE
 * open transaction — the first transaction the bus ever sees is clean, and
 * everything inside it is clean. So the design rule of this skill is:
 *
 *   - the whole boot configuration runs inside ONE transaction (the probe's
 *     proven shape);
 *   - one runtime frame = ONE transaction, opened with a sacrificial SNOP
 *     that absorbs whatever the boundary corrupts (SNOP is a no-op by
 *     definition);
 *   - all commands of a frame are clocked inside that one transaction, with
 *     CS cycled per access like the probe does.
 *
 * The CHIP_RDYn wait on MISO after CS-low was also tried (the datasheet's
 * own protocol) and made things strictly worse — with it the first attempt
 * of every frame never saw the PLL lock, and the immediate retry always
 * did. Parked, documented, not retried. */

/* CS handoff with the datasheet's CHIP_RDYn wait: after CS-low the chip
   drives SO (MISO) with ready-status, and commands clocked while it is HIGH
   are ignored. MEASURED (build B, 2026-09-02): SO-wait + a 5 ms lock window
   gave 10/10 locked frames; widening the window to 20 ms inverted the
   behaviour (every first attempt then failed and only the immediate retry
   locked) — so the wait stays and the window stays tight. */

static void gate_cs_ready() {
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(20);
    uint32_t t0 = millis();
    while (digitalRead(PIN_SPI_MISO) == HIGH) {
        if (millis() - t0 > 2) break;
    }
}

static void gate_raw_write(uint8_t reg, uint8_t val) {
    gate_cs_ready();
    gate_bus->transfer(reg);
    gate_bus->transfer(val);
    digitalWrite(PIN_CC1101_CS, HIGH);
}

static uint8_t gate_raw_read(uint8_t reg) {
    gate_cs_ready();
    gate_bus->transfer(reg | CC1101_SPI_READ);
    uint8_t val = gate_bus->transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);
    return val;
}

static uint8_t gate_raw_status_read(uint8_t reg) {
    gate_cs_ready();
    gate_bus->transfer(reg | CC1101_SPI_BURST | CC1101_SPI_STATUS);
    uint8_t val = gate_bus->transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);
    return val;
}

static void gate_raw_patable(const uint8_t *vals, uint8_t count) {
    gate_cs_ready();
    gate_bus->transfer(CC1101_PATABLE | CC1101_SPI_BURST);
    for (uint8_t i = 0; i < count; i++) gate_bus->transfer(vals[i]);
    digitalWrite(PIN_CC1101_CS, HIGH);
}

/* Bit rate as exponent+mantissa — the same arithmetic as RadioLib's
   getExpMant(target, mantOffset=256, divExp=28, expMax=14)
   (CC1101.cpp:1189-1213). In async mode the modulator samples the GDO0 pin
   at 8x the programmed data rate (datasheet SWRS061I 27.1), so the data
   rate sets the edge grid: at 250 kbaud — the datasheet maximum for OOK —
   the grid is 0.5 us and a raw microsecond figure lands within ~1 us, where
   a conservative 4.8 kbaud would give a 26 us grid. 250 kbaud is what the
   Flipper async presets do for exactly this reason. */
static void gate_bitrate_250k(uint8_t *e_out, uint8_t *m_out) {
    float origin = 256.0f * 26000000.0f / 268435456.0f;  /* 26e6 / 2^28 */
    for (int8_t e = 14; e >= 0; e--) {
        float start = (float)((uint32_t)1 << e) * origin;
        if (250000.0f >= start) {
            float step = start / 256.0f;
            *e_out = (uint8_t)e;
            *m_out = (uint8_t)((250000.0f - start) / step);
            return;
        }
    }
    *e_out = 0;
    *m_out = 0;
}

/* --- The frame, as the real remote speaks it ---
 *
 * Format from ADR-001 (subghz-rx capture analysis, 2026-08-03): a ~2360 us
 * HIGH preamble, then 52 bits, one high pulse per bit with the low as its
 * exact complement — a `1` is long-high (~2Te) + short-low, a `0` is
 * short-high + long-low, ~3Te per bit total, Te ~ 410 us. 36 bits are fixed
 * (serial + button), bit 41 is the A/B flag the remote flips between blocks
 * while a button is held. Every number here is tunable over HTTP
 * (POST /gate/config, persisted to SPIFFS) — the field experiment at the
 * gate must be able to iterate timings and the serial without a reflash.
 */

struct GateCfg {
    uint16_t te_us;          /* 1Te high for a 0, 2Te for a 1 */
    uint16_t preamble_us;    /* the HIGH preamble mark */
    uint16_t post_us;        /* trailing low */
    uint8_t  nbits;          /* payload bits */
    uint8_t  ab_bit;         /* index of the A/B alternation flag */
    uint8_t  button_shift;   /* where the 2-bit button code sits */
    uint64_t fixed_bits;     /* the fixed part as transmitted */
    uint8_t  frames_per_block;
    uint8_t  blocks;
    uint16_t frame_gap_ms;
    uint16_t block_gap_ms;
};

static GateCfg gate_cfg = {
    .te_us = 410, .preamble_us = 2360, .post_us = 410,
    .nbits = 52, .ab_bit = 41, .button_shift = 42,
    .fixed_bits = 0x1234567800ULL,
    .frames_per_block = 5, .blocks = 2,
    .frame_gap_ms = 20, .block_gap_ms = 300,
};

#define GATE_CFG_FILE   "/gate.json"

static void gate_cfg_load() {
    String s = read_spiffs_file(GATE_CFG_FILE);
    if (s.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, s) != DeserializationError::Ok) return;
    if (doc["te_us"].is<int>())            gate_cfg.te_us = doc["te_us"];
    if (doc["preamble_us"].is<int>())      gate_cfg.preamble_us = doc["preamble_us"];
    if (doc["post_us"].is<int>())          gate_cfg.post_us = doc["post_us"];
    if (doc["nbits"].is<int>())            gate_cfg.nbits = doc["nbits"];
    if (doc["ab_bit"].is<int>())           gate_cfg.ab_bit = doc["ab_bit"];
    if (doc["button_shift"].is<int>())     gate_cfg.button_shift = doc["button_shift"];
    if (doc["fixed_bits"].is<const char*>())
        gate_cfg.fixed_bits = strtoull(doc["fixed_bits"].as<const char*>(), NULL, 16);
    if (doc["frames_per_block"].is<int>()) gate_cfg.frames_per_block = doc["frames_per_block"];
    if (doc["blocks"].is<int>())           gate_cfg.blocks = doc["blocks"];
    if (doc["frame_gap_ms"].is<int>())     gate_cfg.frame_gap_ms = doc["frame_gap_ms"];
    if (doc["block_gap_ms"].is<int>())     gate_cfg.block_gap_ms = doc["block_gap_ms"];
}

/* The frame for one button: the fixed word, the 2-bit button code and the
   A/B flag folded in, then the mark/space stream. Preamble HIGH, the gap it
   ends with, 52 [high, low] bit pairs, trailing low — 2 + 2*nbits + 1 runs. */
static uint16_t gate_build_frame(uint8_t button, bool ab, uint32_t *raw) {
    uint64_t word = gate_cfg.fixed_bits;
    word &= ~(3ULL << gate_cfg.button_shift);
    word |= ((uint64_t)(button & 3)) << gate_cfg.button_shift;
    if (ab) word |= (1ULL << gate_cfg.ab_bit);
    else    word &= ~(1ULL << gate_cfg.ab_bit);

    uint16_t n = 0;
    raw[n++] = gate_cfg.preamble_us;
    raw[n++] = gate_cfg.post_us;
    for (int8_t b = (int8_t)gate_cfg.nbits - 1; b >= 0; b--) {
        uint32_t high = (word >> b) & 1 ? 2u * gate_cfg.te_us : gate_cfg.te_us;
        raw[n++] = high;
        raw[n++] = 3u * gate_cfg.te_us - high;
    }
    raw[n++] = gate_cfg.post_us;
    return n;
}

/* --- Job machine: the ir.cpp shape, with the waveform on the CPU --- */

enum {
    GATE_JOB_IDLE = 0,
    GATE_JOB_FRAME,      /* one staged frame, repeated (/gate/send raw) */
    GATE_JOB_BUTTON,     /* a button's block sequence */
    GATE_JOB_PAIR        /* the pairing sequence: A block then B block */
};

static struct {
    uint8_t kind;
    uint16_t job_id;
    uint8_t button;     /* 0 = raw */
    uint8_t blocks, frames_per_block;
    uint8_t frame_i, block_i;
    uint8_t sent, total;
    uint32_t started_ms;
    uint32_t next_at;
    char result[32];
} gate_state;

/* Staging buffer. Written by the web-server task under the same contract as
   ir's staging buffers — a job is started only between polls, and only
   gate_poll() reads it once gate_request_pending is set. Two frame buffers,
   because a held remote alternates A and B blocks. */
static struct {
    uint16_t job_id;
    uint8_t kind;
    uint8_t button;
    uint8_t blocks, frames_per_block;
    uint32_t raw_a[GATE_MAX_RAW];
    uint32_t raw_b[GATE_MAX_RAW];
    uint16_t count_a, count_b;
} gate_request;

static volatile bool gate_request_pending = false;
static volatile bool gate_stop_requested = false;
static uint16_t gate_job_seq = 0;
static bool gate_ab_parity = false;   /* which block flavour the next press emits */

static bool gate_busy() {
    return gate_ready && gate_state.kind != GATE_JOB_IDLE;
}

static void gate_finish(const char *result) {
    snprintf(gate_state.result, sizeof(gate_state.result), "%s", result);
    event_add("gate: job %u %s, %u/%u frames", (unsigned)gate_state.job_id,
              result, (unsigned)gate_state.sent, (unsigned)gate_state.total);
    gate_state.kind = GATE_JOB_IDLE;
    gate_stop_requested = false;
}

/* One frame, synchronously on the loop task: strobes as separate
   transactions, the GDO0 bit-bang between them. MEASURED 2026-09-02, five
   build-test cycles: per-strobe transactions + a retry on the lock poll
   cover the shared-bus flakiness better than any single-transaction shape
   (one frame per transaction alternated good/bad 25/25; a CHIP_RDYn wait
   made it worse). The lock poll gates the burst: nothing goes on the air
   until the PLL is seen locked, so retries cost airtime only in the gap. */
static bool gate_send_frame(const uint32_t *raw, uint16_t count) {
    /* Up to three attempts: the shared-bus strobes drop occasionally, and
       one lost STX leaves the PLL off and GDO2 low. A fresh transaction
       recovers it — the burst never starts until the lock is seen, so a
       retry costs nothing on the air. */
    for (int attempt = 0; attempt < 3; attempt++) {
        SPIClass *spi = gate_runtime_bus();
        spi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        gate_cs_ready();
        spi->transfer(CC1101_SIDLE);      /* fresh state for a fresh calibration */

        digitalWrite(PIN_CC1101_GDO0, LOW);   /* PA_TABLE[0]: carrier keyed off */
        spi->transfer(CC1101_STX);            /* carrier up, calibration runs */

        /* Wait for the PLL to lock, signalled on the GDO2 PIN (IOCFG2 =
           0x0A): a digitalRead, not an SPI status read — the shared-bus
           register path is the one that lies. The burst has not started yet
           — GDO0 is still low — so the poll costs a small hole before the
           first symbol, the same hole Flipper's furi_hal_subghz accepts
           before its stream. */
        uint32_t t0 = millis();
        bool locked = false;
        while (millis() - t0 <= GATE_TX_READY_MS) {
            if (digitalRead(PIN_CC1101_GDO2) == HIGH) { locked = true; break; }
        }

        if (locked) {
            /* Alternating mark/space, starting with a mark — the /ir/send
               contract. Even entries key the carrier on, odd entries off. */
            for (uint16_t i = 0; i < count; i++) {
                digitalWrite(PIN_CC1101_GDO0, (i & 1) ? LOW : HIGH);
                delayMicroseconds(raw[i]);
            }
            digitalWrite(PIN_CC1101_GDO0, LOW);
        }

        spi->transfer(CC1101_SIDLE);      /* close the burst, or back out */
        digitalWrite(PIN_CC1101_CS, HIGH);
        spi->endTransaction();

        if (locked) return true;
    }
    event_add("gate: PLL did not lock after STX");
    return false;
}

static void gate_begin_request() {
    memset(&gate_state, 0, sizeof(gate_state));
    gate_stop_requested = false;
    gate_state.kind = gate_request.kind;
    gate_state.job_id = gate_request.job_id;
    gate_state.button = gate_request.button;
    gate_state.blocks = gate_request.blocks;
    gate_state.frames_per_block = gate_request.frames_per_block;
    gate_state.total = gate_request.blocks * gate_request.frames_per_block;
    gate_state.started_ms = millis();
    gate_state.next_at = millis();

    event_add("gate: job %u %s, %u frames in %u blocks",
              (unsigned)gate_request.job_id,
              gate_state.kind == GATE_JOB_PAIR   ? "pair"   :
              gate_state.kind == GATE_JOB_BUTTON ? "button" : "raw",
              (unsigned)gate_state.total, (unsigned)gate_state.blocks);
}

/*
 * Called from loop(). One frame per pass: the frame itself is a CPU-held
 * burst (wait-for-PLL poll + symbols, bounded by GATE_FRAME_MAX_US), the
 * gaps are waited across passes the way ir_poll waits. Inside a block the
 * gap is frame_gap_ms; between the A and B blocks it is block_gap_ms.
 */
static void gate_poll() {
    if (!gate_ready) return;

    for (;;) {
        if (gate_state.kind == GATE_JOB_IDLE) {
            if (gate_request_pending) {
                gate_begin_request();
                gate_request_pending = false;
                continue;
            }
            return;
        }

        if (gate_stop_requested) {
            gate_finish("stopped");
            return;
        }
        if ((long)(millis() - gate_state.next_at) < 0) return;
        if (gate_state.sent >= gate_state.total) {
            gate_finish("done");
            return;
        }

        /* The frame this position sends: even blocks send frame A, odd
           blocks frame B — the A/B alternation of a held remote; a raw job
           has both buffers holding the same staged frame. */
        const uint32_t *raw = (gate_state.block_i & 1) ? gate_request.raw_b
                                                       : gate_request.raw_a;
        uint16_t count = (gate_state.block_i & 1) ? gate_request.count_b
                                                  : gate_request.count_a;
        if (!gate_send_frame(raw, count)) {
            gate_finish("tx failed");
            return;
        }
        gate_state.sent++;
        gate_state.frame_i++;
        if (gate_state.frame_i >= gate_state.frames_per_block) {
            gate_state.frame_i = 0;
            gate_state.block_i++;
            gate_state.next_at = millis() + gate_cfg.block_gap_ms;
        } else {
            gate_state.next_at = millis() + gate_cfg.frame_gap_ms;
        }
        return;  /* the frame owned the CPU; the rest of the pass gets it back */
    }
}

/* --- Job control, independent of the transport ---
 *
 * The endpoint validates into the staging buffer and sets
 * gate_request_pending; gate_poll() picks it up from the loop task. */

static bool gate_stop_job() {
    if (!gate_ready || gate_state.kind == GATE_JOB_IDLE) return false;
    gate_stop_requested = true;
    return true;
}

struct GateProgress {
    bool running;
    uint16_t job_id;
    uint8_t sent, total, block, blocks;
    unsigned long elapsed_ms;
    const char *result;
};

static GateProgress gate_progress() {
    GateProgress p = {};
    p.running = (gate_state.kind != GATE_JOB_IDLE);
    p.job_id = gate_state.job_id;
    p.sent = gate_state.sent;
    p.total = gate_state.total;
    p.block = gate_state.block_i + 1;
    p.blocks = gate_state.blocks;
    p.elapsed_ms = millis() - gate_state.started_ms;
    p.result = gate_state.result;
    return p;
}

/* Stage a start: build the frames for this kind and hand the job to
   gate_poll(). Returns the job id, or 0 when a job is already running or the
   radio is not up. A BUTTON job emits one block (the current AB flavour) and
   flips the flavour, so successive presses alternate like a held remote; the
   PAIR sequence emits an A block and then a B block, the hold pattern. */
static uint16_t gate_start_code(uint8_t kind, uint8_t button) {
    if (!gate_ready || gate_state.kind != GATE_JOB_IDLE) return 0;

    gate_request.kind = kind;
    gate_request.button = button;
    gate_request.frames_per_block = gate_cfg.frames_per_block;
    if (kind == GATE_JOB_PAIR) {
        gate_request.count_a = gate_build_frame(button, false, gate_request.raw_a);
        gate_request.count_b = gate_build_frame(button, true,  gate_request.raw_b);
        gate_request.blocks = 2;
    } else {
        gate_request.count_a = gate_build_frame(button, gate_ab_parity, gate_request.raw_a);
        gate_request.count_b = gate_build_frame(button, gate_ab_parity, gate_request.raw_b);
        gate_request.blocks = 1;
        gate_ab_parity = !gate_ab_parity;
    }
    gate_request.job_id = ++gate_job_seq;
    gate_request_pending = true;
    return gate_request.job_id;
}

/* --- Endpoints --- */

static const SkillEndpoint gate_endpoints[] = {
    {"POST", "/gate/send",    "Send one raw OOK frame {raw[], repeat}"},
    {"POST", "/gate/pair",    "The pairing sequence: A block then B block"},
    {"POST", "/gate/button/1","Fire button 1 as a paired code"},
    {"POST", "/gate/button/2","Fire button 2 as a paired code"},
    {"POST", "/gate/button/3","Fire button 3 as a paired code"},
    {"POST", "/gate/button/4","Fire button 4 as a paired code"},
    {"GET",  "/gate/config",  "The frame parameters (Te, preamble, bits, serial)"},
    {"POST", "/gate/config",  "Set frame parameters, persisted to SPIFFS"},
    {"GET",  "/gate/status",  "Progress of the running (or last) job"},
    {"POST", "/gate/stop",    "Abort the running job"},
    {NULL, NULL, NULL}
};

static const char *gate_describe() {
    return "## Skill: gate\n\n"
           "Sub-GHz OOK transmitter on the onboard CC1101 at 433.92 MHz: a\n"
           "spare gate remote that pairs through the receiver's own program\n"
           "button. Frame format per ADR-001 (subghz-rx capture analysis):\n"
           "~2360us HIGH preamble, 52 PWM bits at Te~410us, 2-bit button\n"
           "code, bit 41 = A/B alternation. All parameters are tunable via\n"
           "/gate/config without a reflash.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /gate/pair | The pairing sequence: A block then B block |\n"
           "| POST | /gate/button/1..4 | Fire one button's block sequence |\n"
           "| POST | /gate/send | One raw OOK frame: `{\"raw\":[11000,512,...],\"repeat\":4}` |\n"
           "| GET | /gate/config | Frame parameters |\n"
           "| POST | /gate/config | Set frame parameters (persisted) |\n"
           "| GET | /gate/status | Running/idle, frames sent, elapsed, result |\n"
           "| POST | /gate/stop | Abort the running job |\n\n"
           "### Behaviour\n\n"
           "A button press emits one block of frames_per_block identical\n"
           "frames and flips the A/B flavour; the pair sequence emits an A\n"
           "block then a B block with block_gap_ms between them — the hold\n"
           "pattern of a real remote. Every call returns immediately; the\n"
           "symbols are clocked on the CPU (RMT is fully claimed by ir and\n"
           "ring), one frame per loop pass, and every frame is verified by\n"
           "the GDO2 PLL-lock pin. A job can be stopped within a frame.\n\n"
           "/gate/send stays the raw debugging path: alternating mark/space\n"
           "microseconds, no encoder, no config.\n";
}

/* Body-carrying handlers must release the collected body on every exit
   path, including the unauthenticated one — see ir.cpp:1037-1044. */
static char *gate_take_body(AsyncWebServerRequest *req) {
    char *body = (char*)req->_tempObject;
    req->_tempObject = nullptr;
    return body;
}

static void gate_send_json(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

static void gate_send_error(AsyncWebServerRequest *req, int code, const char *msg) {
    JsonDocument doc;
    doc["error"] = msg;
    gate_send_json(req, code, doc);
}

static void gate_register_routes(AsyncWebServer &server) {

    /* POST /gate/send — body: {"raw":[...],"repeat":1..10} */
    server.on(AsyncURIMatcher::exact("/gate/send"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = gate_take_body(req);
        if (!check_auth(req)) {
            free(body);
            gate_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!gate_ready) {
            free(body);
            gate_send_error(req, 503, "sub-GHz transmitter unavailable");
            return;
        }
        if (!body) {
            gate_send_error(req, 400, "body required: {\"raw\":[...]}");
            return;
        }

        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            gate_send_error(req, 400, "invalid JSON");
            return;
        }

        JsonArray raw = input["raw"].as<JsonArray>();
        if (raw.isNull()) {
            gate_send_error(req, 400, "raw array required");
            return;
        }
        size_t n = raw.size();
        if (n < 2 || n > GATE_MAX_RAW) {
            gate_send_error(req, 400, "raw must have 2-512 entries");
            return;
        }

        int repeat = 1;
        if (input["repeat"].is<int>()) {
            repeat = input["repeat"].as<int>();
            if (repeat < 1 || repeat > GATE_REPEAT_MAX) {
                gate_send_error(req, 400, "repeat must be 1-10");
                return;
            }
        }

        if (gate_busy()) {
            gate_send_error(req, 409, "a job is already running");
            return;
        }

        /* Validated into the staging buffer before the job is handed over;
           the staging buffer is what gate_poll() transmits from. Both frame
           buffers hold the same frame — a raw job has no A/B alternation. */
        uint16_t i = 0;
        uint32_t duration = 0;
        for (JsonVariant v : input["raw"].as<JsonArray>()) {
            if (!v.is<int>()) {
                gate_send_error(req, 400, "raw entries must be integers");
                return;
            }
            int us = v.as<int>();
            if (us < GATE_ENTRY_MIN || us > GATE_ENTRY_MAX) {
                gate_send_error(req, 400, "raw entries must be 10-50000 microseconds");
                return;
            }
            duration += (uint32_t)us;
            if (duration > GATE_FRAME_MAX_US) {
                gate_send_error(req, 400, "frame longer than 250000 us");
                return;
            }
            gate_request.raw_a[i] = (uint32_t)us;
            i++;
        }
        memcpy(gate_request.raw_b, gate_request.raw_a, i * sizeof(uint32_t));
        gate_request.count_a = i;
        gate_request.count_b = i;
        gate_request.kind = GATE_JOB_FRAME;
        gate_request.button = 0;
        gate_request.blocks = 1;
        gate_request.frames_per_block = (uint8_t)repeat;
        gate_request.job_id = ++gate_job_seq;
        gate_request_pending = true;

        JsonDocument doc;
        doc["ok"] = true;
        doc["job"] = gate_request.job_id;
        doc["entries"] = i;
        doc["repeat"] = repeat;
        doc["frame_us"] = (unsigned long)duration;
        doc["freq_mhz"] = 433.92;
        gate_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    /* POST /gate/pair and POST /gate/button/N — the paired-code sequences.
       A button job emits one block in the current A/B flavour and flips it;
       the pair job emits the A block and then the B block, which is what a
       held remote does while a receiver listens. */
    for (int kind = 0; kind < 2; kind++) {
        const char *path = (kind == 0) ? "/gate/pair" : "/gate/button";
        server.on(AsyncURIMatcher::exact(path), HTTP_POST, [kind](AsyncWebServerRequest *req) {
            if (!require_auth(req)) return;
            uint16_t job = gate_start_code(kind == 0 ? GATE_JOB_PAIR : GATE_JOB_BUTTON, 0);
            JsonDocument doc;
            if (job == 0) {
                doc["error"] = gate_ready ? "a job is already running"
                                          : "sub-GHz transmitter unavailable";
                gate_send_json(req, gate_ready ? 409 : 503, doc);
                return;
            }
            doc["ok"] = true;
            doc["job"] = job;
            gate_send_json(req, 200, doc);
        });
    }
    for (uint8_t b = 1; b <= 4; b++) {
        char path[20];
        snprintf(path, sizeof(path), "/gate/button/%u", b);
        server.on(AsyncURIMatcher::exact(path), HTTP_POST, [b](AsyncWebServerRequest *req) {
            if (!require_auth(req)) return;
            uint16_t job = gate_start_code(GATE_JOB_BUTTON, b);
            JsonDocument doc;
            if (job == 0) {
                doc["error"] = gate_ready ? "a job is already running"
                                          : "sub-GHz transmitter unavailable";
                gate_send_json(req, gate_ready ? 409 : 503, doc);
                return;
            }
            doc["ok"] = true;
            doc["job"] = job;
            doc["button"] = b;
            gate_send_json(req, 200, doc);
        });
    }

    /* GET /gate/config — the frame parameters, so the field experiment reads
       what it is about to iterate. */
    server.on(AsyncURIMatcher::exact("/gate/config"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        doc["te_us"] = gate_cfg.te_us;
        doc["preamble_us"] = gate_cfg.preamble_us;
        doc["post_us"] = gate_cfg.post_us;
        doc["nbits"] = gate_cfg.nbits;
        doc["ab_bit"] = gate_cfg.ab_bit;
        doc["button_shift"] = gate_cfg.button_shift;
        char hex[24];
        snprintf(hex, sizeof(hex), "%llX", (unsigned long long)gate_cfg.fixed_bits);
        doc["fixed_bits"] = hex;
        doc["frames_per_block"] = gate_cfg.frames_per_block;
        doc["blocks"] = gate_cfg.blocks;
        doc["frame_gap_ms"] = gate_cfg.frame_gap_ms;
        doc["block_gap_ms"] = gate_cfg.block_gap_ms;
        gate_send_json(req, 200, doc);
    });

    /* POST /gate/config — set and persist; applied immediately (a running
       job finishes with the old parameters, which is fine: gaps and block
       sizes are read live, the frame buffers were already built). */
    server.on(AsyncURIMatcher::exact("/gate/config"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = gate_take_body(req);
        if (!check_auth(req)) {
            free(body);
            gate_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            gate_send_error(req, 400, "body required");
            return;
        }

        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        if (err != DeserializationError::Ok) {
            free(body);
            gate_send_error(req, 400, "invalid JSON");
            return;
        }

        /* Range checks before anything is applied: a nonsense Te would put
           symbol edges outside the entry limits the transmitter enforces. */
        if (input["te_us"].is<int>()) {
            int v = input["te_us"];
            if (v < 50 || v > 5000) { free(body); gate_send_error(req, 400, "te_us must be 50-5000"); return; }
            gate_cfg.te_us = v;
        }
        if (input["preamble_us"].is<int>()) {
            int v = input["preamble_us"];
            if (v < 100 || v > 50000) { free(body); gate_send_error(req, 400, "preamble_us must be 100-50000"); return; }
            gate_cfg.preamble_us = v;
        }
        if (input["post_us"].is<int>()) {
            int v = input["post_us"];
            if (v < 50 || v > 50000) { free(body); gate_send_error(req, 400, "post_us must be 50-50000"); return; }
            gate_cfg.post_us = v;
        }
        if (input["nbits"].is<int>()) {
            int v = input["nbits"];
            if (v < 8 || v > 64) { free(body); gate_send_error(req, 400, "nbits must be 8-64"); return; }
            gate_cfg.nbits = v;
        }
        if (input["ab_bit"].is<int>()) {
            int v = input["ab_bit"];
            if (v < 0 || v > 63) { free(body); gate_send_error(req, 400, "ab_bit must be 0-63"); return; }
            gate_cfg.ab_bit = v;
        }
        if (input["button_shift"].is<int>()) {
            int v = input["button_shift"];
            if (v < 0 || v > 62) { free(body); gate_send_error(req, 400, "button_shift must be 0-62"); return; }
            gate_cfg.button_shift = v;
        }
        if (input["fixed_bits"].is<const char*>())
            gate_cfg.fixed_bits = strtoull(input["fixed_bits"].as<const char*>(), NULL, 16);
        if (input["frames_per_block"].is<int>()) {
            int v = input["frames_per_block"];
            if (v < 1 || v > 20) { free(body); gate_send_error(req, 400, "frames_per_block must be 1-20"); return; }
            gate_cfg.frames_per_block = v;
        }
        if (input["blocks"].is<int>()) {
            int v = input["blocks"];
            if (v < 1 || v > 8) { free(body); gate_send_error(req, 400, "blocks must be 1-8"); return; }
            gate_cfg.blocks = v;
        }
        if (input["frame_gap_ms"].is<int>()) {
            int v = input["frame_gap_ms"];
            if (v < 5 || v > 5000) { free(body); gate_send_error(req, 400, "frame_gap_ms must be 5-5000"); return; }
            gate_cfg.frame_gap_ms = v;
        }
        if (input["block_gap_ms"].is<int>()) {
            int v = input["block_gap_ms"];
            if (v < 10 || v > 10000) { free(body); gate_send_error(req, 400, "block_gap_ms must be 10-10000"); return; }
            gate_cfg.block_gap_ms = v;
        }
        free(body);

        String out;
        JsonDocument doc;
        doc["te_us"] = gate_cfg.te_us;
        doc["preamble_us"] = gate_cfg.preamble_us;
        doc["post_us"] = gate_cfg.post_us;
        doc["nbits"] = gate_cfg.nbits;
        doc["ab_bit"] = gate_cfg.ab_bit;
        doc["button_shift"] = gate_cfg.button_shift;
        char hex[24];
        snprintf(hex, sizeof(hex), "%llX", (unsigned long long)gate_cfg.fixed_bits);
        doc["fixed_bits"] = hex;
        doc["frames_per_block"] = gate_cfg.frames_per_block;
        doc["blocks"] = gate_cfg.blocks;
        doc["frame_gap_ms"] = gate_cfg.frame_gap_ms;
        doc["block_gap_ms"] = gate_cfg.block_gap_ms;
        serializeJson(doc, out);
        bool saved = write_spiffs_file(GATE_CFG_FILE, out);
        event_add("gate: config %s", saved ? "saved" : "save FAILED");

        JsonDocument resp;
        resp["ok"] = saved;
        gate_send_json(req, saved ? 200 : 500, resp);
    });

    /* GET /gate/status */
    server.on(AsyncURIMatcher::exact("/gate/status"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        GateProgress p = gate_progress();
        JsonDocument doc;
        doc["running"] = p.running;
        doc["job"] = p.job_id;
        doc["sent"] = p.sent;
        doc["total"] = p.total;
        doc["block"] = p.block;
        doc["blocks"] = p.blocks;
        doc["elapsed_ms"] = (unsigned long)p.elapsed_ms;
        if (!p.running && p.result[0]) doc["result"] = p.result;
        gate_send_json(req, 200, doc);
    });

    /* POST /gate/stop */
    server.on(AsyncURIMatcher::exact("/gate/stop"), HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        bool running = gate_stop_job();

        JsonDocument doc;
        doc["ok"] = true;
        doc["stopped"] = running;
        doc["job"] = gate_state.job_id;
        doc["sent"] = gate_state.sent;
        gate_send_json(req, 200, doc);
    });
}

static const Skill gate_skill = {
    .name = "gate",
    .version = "0.1.0",
    .describe = gate_describe,
    .endpoints = gate_endpoints,
    .register_routes = gate_register_routes
};

/*
 * Boot-time hardware bring-up, called from probe_cc1101() with THE SAME live
 * instance the probe just used to read VERSION truthfully — the one bus path
 * that has been measured reliable on this hardware. The chip is configured
 * completely here, before spi.end() returns the bus and before display_init()
 * claims it. At runtime the job machine only sends write-only strobes and
 * verifies frames through the GDO2 pin; no runtime access reads a register.
 */
static void gate_configure(SPIClass &spi) {
    gate_bus = &spi;
    gate_cfg_load();   /* SPIFFS is already up — see setup() order */

    /* The RF band switch goes to the 434 MHz position before the chip can
       ever transmit — see the note on PIN_RF_SW1/SW0 above. */
    pinMode(PIN_RF_SW1, OUTPUT);
    digitalWrite(PIN_RF_SW1, HIGH);
    pinMode(PIN_RF_SW0, OUTPUT);
    digitalWrite(PIN_RF_SW0, HIGH);

    /* The data input idles LOW = PA_TABLE[0] = no carrier. Never INPUT: a
       floating pin keys the transmitter spuriously. GDO2 watches the PLL
       (IOCFG2 below) — ESP32 side input, the chip drives it during TX. */
    pinMode(PIN_CC1101_GDO0, OUTPUT);
    digitalWrite(PIN_CC1101_GDO0, LOW);
    pinMode(PIN_CC1101_GDO2, INPUT);

    /* The whole bring-up lives inside ONE transaction — the probe's own
       shape, the only one this bus has been measured to run clean. SRES
       restarts the crystal, then the register set, PATABLE, flush and idle,
       all before spi.end() hands the bus to the display. */
    spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    gate_cs_ready();
    spi.transfer(CC1101_SRES);
    digitalWrite(PIN_CC1101_CS, HIGH);
    delay(2);

    uint8_t version = gate_raw_status_read(CC1101_VERSION);

    /* Carrier: 433.92 MHz on a 26 MHz crystal, FRF = f * 2^16 / f_xtal. */
    uint32_t frf = (uint32_t)(((uint64_t)GATE_FREQ_HZ << 16) / CC1101_XTAL_HZ);
    gate_raw_write(CC1101_FREQ2, (frf >> 16) & 0xFF);
    gate_raw_write(CC1101_FREQ1, (frf >> 8) & 0xFF);
    gate_raw_write(CC1101_FREQ0, frf & 0xFF);

    /* Modulation: ASK/OOK, async serial, infinite length. Sync words and
       packet machinery are meaningless in this mode. */
    gate_raw_write(CC1101_PKTLEN, 0xFF);
    gate_raw_write(CC1101_PKTCTRL0, CC1101_PKTCTRL0_ASYNC);
    gate_raw_write(CC1101_MDMCFG2, CC1101_MOD_ASK_OOK);

    /* Data rate 250 kbaud: the async sampling grid is 8x the data rate, so
       this puts raw edges within ~1 us (see gate_bitrate_250k). The
       RX-bandwidth bits of MDMCFG4 keep their reset values
       (read-modify-write, no baked defaults). */
    uint8_t e, m;
    gate_bitrate_250k(&e, &m);
    gate_raw_write(CC1101_MDMCFG4, (gate_raw_read(CC1101_MDMCFG4) & 0xF0) | (e & 0x0F));
    gate_raw_write(CC1101_MDMCFG3, m);

    /* Calibrate the frequency synthesizer on every idle->TX transition, so a
       job's later frames are as accurate as its first. Pin control off: the
       waveform comes from this code, not from a GDO pin. */
    {
        uint8_t mcsm0 = gate_raw_read(CC1101_MCSM0);
        mcsm0 = (mcsm0 & ~0x30) | CC1101_MCSM0_AUTOCAL;
        mcsm0 = (mcsm0 & ~0x03) | CC1101_MCSM0_PINCTRL;
        gate_raw_write(CC1101_MCSM0, mcsm0);
    }

    /* GDO0 = async data input; GDO2 = PLL in lock — the frame verifier. */
    gate_raw_write(CC1101_IOCFG0, CC1101_GDO0_ASYNC_DATA);
    gate_raw_write(CC1101_IOCFG2, CC1101_GDOX_PLL_LOCK);

    /* OOK power: FREND0.PA_POWER = 1 (a '1' keys PA_TABLE[1]), and
       PA_TABLE[0] = off, PA_TABLE[1] = full. */
    gate_raw_write(CC1101_FREND0, (gate_raw_read(CC1101_FREND0) & ~0x07) | CC1101_PA_POWER_1);
    const uint8_t pa[2] = { CC1101_PA_OFF, CC1101_PA_ON };
    gate_raw_patable(pa, 2);

    gate_raw_status_read(CC1101_SFTX);   /* flush, then idle */
    gate_raw_status_read(CC1101_SIDLE);
    spi.endTransaction();

    gate_ready = true;
    event_add("gate: CC1101 OOK ready");

    /* The probe bus dies with probe_cc1101()'s stack frame; runtime access
       goes through gate_runtime_bus() (the display's instance). */
    gate_bus = nullptr;
    skill_register(&gate_skill);
}