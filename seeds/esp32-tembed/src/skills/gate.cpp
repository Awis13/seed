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
#define GATE_TX_READY_MS 5      /* STX -> MARCSTATE==TX: calibration (~724us,
                                    datasheet Table 35) plus PLL settle; the
                                    same poll Flipper runs before its stream */

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

static void gate_reg_write(uint8_t reg, uint8_t val) {
    gate_bus->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    gate_bus->transfer(reg);
    gate_bus->transfer(val);
    digitalWrite(PIN_CC1101_CS, HIGH);
    gate_bus->endTransaction();
}

static uint8_t gate_reg_read(uint8_t reg) {
    gate_bus->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    gate_bus->transfer(reg | CC1101_SPI_READ);
    uint8_t val = gate_bus->transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);
    gate_bus->endTransaction();
    return val;
}

/* Status registers (0x30-0x3D) answer to address | burst | status. */
static uint8_t gate_status_read(uint8_t reg) {
    gate_bus->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    gate_bus->transfer(reg | CC1101_SPI_BURST | CC1101_SPI_STATUS);
    uint8_t val = gate_bus->transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);
    gate_bus->endTransaction();
    return val;
}

static void gate_patable_write(const uint8_t *vals, uint8_t count) {
    gate_bus->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    gate_bus->transfer(CC1101_PATABLE | CC1101_SPI_BURST);
    for (uint8_t i = 0; i < count; i++) gate_bus->transfer(vals[i]);
    digitalWrite(PIN_CC1101_CS, HIGH);
    gate_bus->endTransaction();
}

/* Strobe = one header byte, write-only. The status byte MISO carries back
 * is not trustworthy on this bus (see above) and is not used. */
static void gate_strobe(uint8_t cmd) {
    SPIClass *spi = gate_runtime_bus();
    spi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    spi->transfer(cmd);
    digitalWrite(PIN_CC1101_CS, HIGH);
    spi->endTransaction();
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

/* --- Job machine: the ir.cpp shape, with the waveform on the CPU --- */

enum {
    GATE_JOB_IDLE = 0,
    GATE_JOB_RAW
};

static struct {
    uint8_t kind;
    uint16_t job_id;
    uint8_t repeat;
    uint8_t total;      /* frames: one raw frame repeated */
    uint8_t sent;
    uint32_t started_ms;
    uint32_t next_at;
    char result[32];
} gate_state;

/* Staging buffer. Written by the web-server task under the same contract as
   ir's staging buffers — a job is started only between polls, and only
   gate_poll() reads it once gate_request_pending is set. */
static struct {
    uint16_t job_id;
    uint8_t repeat;
    uint32_t raw_us[GATE_MAX_RAW];
    uint16_t raw_count;
} gate_request;

static volatile bool gate_request_pending = false;
static volatile bool gate_stop_requested = false;
static uint16_t gate_job_seq = 0;

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

/* One frame, synchronously on the loop task. Everything the frame needs is
   staged in gate_request; the CPU holds GDO0 for the wait-for-PLL poll plus
   the frame itself and hands the loop back after. */
static bool gate_send_frame() {
    /* IDLE first: FS_AUTOCAL fires on the idle->TX transition we are about
       to make, so every frame starts from a fresh calibration. */
    gate_strobe(CC1101_SIDLE);
    digitalWrite(PIN_CC1101_GDO0, LOW);

    /* Carrier up. GDO0 is LOW, so the carrier is keyed off (PA_TABLE[0]);
       calibration runs under it. */
    gate_strobe(CC1101_STX);

    /* Wait for the PLL to lock, signalled on the GDO2 PIN (IOCFG2 = 0x0A):
       a digitalRead, not an SPI status read — the shared-bus status path is
       the one that lies (see the SPI note above). The burst has not started
       yet — GDO0 is still low — so the poll costs a small hole before the
       first symbol, the same hole Flipper's furi_hal_subghz accepts before
       its stream. */
    uint32_t t0 = millis();
    for (;;) {
        if (digitalRead(PIN_CC1101_GDO2) == HIGH) break;
        if (millis() - t0 > GATE_TX_READY_MS) {
            event_add("gate: PLL did not lock after STX");
            gate_strobe(CC1101_SIDLE);
            return false;
        }
    }

    /* Alternating mark/space, starting with a mark — the /ir/send contract.
       Even entries key the carrier on, odd entries off. */
    for (uint16_t i = 0; i < gate_request.raw_count; i++) {
        digitalWrite(PIN_CC1101_GDO0, (i & 1) ? LOW : HIGH);
        delayMicroseconds(gate_request.raw_us[i]);
    }
    digitalWrite(PIN_CC1101_GDO0, LOW);

    /* End the burst; the PLL drops GDO2 back low. The next frame
       calibrates again on its own STX. */
    gate_strobe(CC1101_SIDLE);
    return true;
}

static void gate_begin_request() {
    memset(&gate_state, 0, sizeof(gate_state));
    gate_stop_requested = false;
    gate_state.kind = GATE_JOB_RAW;
    gate_state.job_id = gate_request.job_id;
    gate_state.repeat = gate_request.repeat;
    gate_state.total = gate_request.repeat;
    gate_state.started_ms = millis();
    gate_state.next_at = millis();

    event_add("gate: job %u raw, %u entries x%u",
              (unsigned)gate_request.job_id,
              (unsigned)gate_request.raw_count,
              (unsigned)gate_request.repeat);
}

/*
 * Called from loop(). One frame per pass: the frame itself is a CPU-held
 * burst (wait-for-TX poll + symbols, bounded by GATE_FRAME_MAX_US), the gap
 * between frames is waited across passes the way ir_poll waits. Nothing
 * here spins waiting for anything but its own symbols.
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
        if (!gate_send_frame()) {
            gate_finish("tx failed");
            return;
        }
        gate_state.sent++;
        gate_state.next_at = millis() + GATE_REPEAT_GAP_MS;
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
    uint8_t sent, total, repeat;
    uint16_t entries;
    unsigned long elapsed_ms;
    const char *result;
};

static GateProgress gate_progress() {
    GateProgress p = {};
    p.running = (gate_state.kind != GATE_JOB_IDLE);
    p.job_id = gate_state.job_id;
    p.sent = gate_state.sent;
    p.total = gate_state.total;
    p.repeat = gate_state.repeat;
    p.entries = gate_request.raw_count;
    p.elapsed_ms = millis() - gate_state.started_ms;
    p.result = gate_state.result;
    return p;
}

/* --- Endpoints --- */

static const SkillEndpoint gate_endpoints[] = {
    {"POST", "/gate/send",   "Send one raw OOK frame {raw[], repeat}"},
    {"GET",  "/gate/status", "Progress of the running (or last) job"},
    {"POST", "/gate/stop",   "Abort the running job"},
    {NULL, NULL, NULL}
};

static const char *gate_describe() {
    return "## Skill: gate\n\n"
           "Sub-GHz OOK transmitter on the onboard CC1101 at 433.92 MHz.\n"
           "Shares the SPI bus with the display — see the file header for the\n"
           "arbitration contract.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /gate/send | One raw OOK frame: `{\"raw\":[11000,512,...],\"repeat\":4}` |\n"
           "| GET | /gate/status | Running/idle, frames sent, elapsed, result |\n"
           "| POST | /gate/stop | Abort the running job |\n\n"
           "### Behaviour\n\n"
           "`raw` is alternating mark/space durations in microseconds,\n"
           "starting with a mark (carrier on), exactly like /ir/send without\n"
           "a carrier: the CC1101 keys its 433.92 MHz carrier to the pin, so\n"
           "there is no khz/duty here. 2 to 512 entries, each 10-50000 us,\n"
           "one frame at most 250000 us in total. `repeat` 1-10, 20 ms\n"
           "between repeats.\n\n"
           "Every call returns immediately and nothing blocks beyond one\n"
           "frame per loop pass: the symbols are clocked out on the CPU (the\n"
           "RMT peripheral is fully claimed by ir and ring), one frame per\n"
           "pass, and the chip is left in IDLE between frames. A job can be\n"
           "stopped at any point and stops within a frame.\n\n"
           "C1 scope: raw replay only. Named encoders (CAME 12-bit and\n"
           "friends), a stored code and the /gate/open webhook are the next\n"
           "commits on the same job machine.\n";
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
           the staging buffer is what gate_poll() transmits from. */
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
            gate_request.raw_us[i++] = (uint32_t)us;
        }
        gate_request.raw_count = (uint16_t)i;
        gate_request.repeat = (uint8_t)repeat;
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

    /* GET /gate/status */
    server.on(AsyncURIMatcher::exact("/gate/status"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        GateProgress p = gate_progress();
        JsonDocument doc;
        doc["running"] = p.running;
        doc["job"] = p.job_id;
        doc["sent"] = p.sent;
        doc["total"] = p.total;
        doc["repeat"] = p.repeat;
        doc["entries"] = p.entries;
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

    /* Chip present? The probe already read VERSION truthfully on this very
       instance (it is what hw.has_cc1101 is based on) — do not re-gate on a
       fresh-transaction re-read here: measured on this hardware, the FIRST
       status read in a NEW transaction returns a stale byte (0x0F) while the
       probe's reads inside its own long transaction were truthful. Drain the
       path with two throwaway reads and log what comes back, then configure.
       The runtime truth-teller is the GDO2 pin, not any register read. */
    uint8_t version = 0x00;
    for (int i = 0; i < 2; i++) gate_status_read(CC1101_VERSION);  /* drain */
    version = gate_status_read(CC1101_VERSION);
    event_add("gate: probe-bus version re-read 0x%02X", version);

    /* Carrier: 433.92 MHz on a 26 MHz crystal, FRF = f * 2^16 / f_xtal. */
    uint32_t frf = (uint32_t)(((uint64_t)GATE_FREQ_HZ << 16) / CC1101_XTAL_HZ);
    gate_reg_write(CC1101_FREQ2, (frf >> 16) & 0xFF);
    gate_reg_write(CC1101_FREQ1, (frf >> 8) & 0xFF);
    gate_reg_write(CC1101_FREQ0, frf & 0xFF);

    /* Modulation: ASK/OOK, async serial, infinite length. Sync words and
       packet machinery are meaningless in this mode. */
    gate_reg_write(CC1101_PKTLEN, 0xFF);
    gate_reg_write(CC1101_PKTCTRL0, CC1101_PKTCTRL0_ASYNC);
    gate_reg_write(CC1101_MDMCFG2, CC1101_MOD_ASK_OOK);

    /* Data rate 250 kbaud: the async sampling grid is 8x the data rate, so
       this puts raw edges within ~1 us (see gate_bitrate_250k). The
       RX-bandwidth bits of MDMCFG4 keep their reset values
       (read-modify-write, no baked defaults). */
    uint8_t e, m;
    gate_bitrate_250k(&e, &m);
    gate_reg_write(CC1101_MDMCFG4, (gate_reg_read(CC1101_MDMCFG4) & 0xF0) | (e & 0x0F));
    gate_reg_write(CC1101_MDMCFG3, m);

    /* Calibrate the frequency synthesizer on every idle->TX transition, so a
       job's later frames are as accurate as its first. Pin control off: the
       waveform comes from this code, not from a GDO pin. */
    {
        uint8_t mcsm0 = gate_reg_read(CC1101_MCSM0);
        mcsm0 = (mcsm0 & ~0x30) | CC1101_MCSM0_AUTOCAL;
        mcsm0 = (mcsm0 & ~0x03) | CC1101_MCSM0_PINCTRL;
        gate_reg_write(CC1101_MCSM0, mcsm0);
    }

    /* GDO0 = async data input; GDO2 = PLL in lock — the frame verifier. */
    gate_reg_write(CC1101_IOCFG0, CC1101_GDO0_ASYNC_DATA);
    gate_reg_write(CC1101_IOCFG2, CC1101_GDOX_PLL_LOCK);

    /* OOK power: FREND0.PA_POWER = 1 (a '1' keys PA_TABLE[1]), and
       PA_TABLE[0] = off, PA_TABLE[1] = full. */
    gate_reg_write(CC1101_FREND0, (gate_reg_read(CC1101_FREND0) & ~0x07) | CC1101_PA_POWER_1);
    const uint8_t pa[2] = { CC1101_PA_OFF, CC1101_PA_ON };
    gate_patable_write(pa, 2);

    /* Flush and idle, on the probe bus (the runtime strobes are not up
       yet — the display has not been initialised). */
    gate_bus->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    gate_bus->transfer(CC1101_SFTX);
    gate_bus->transfer(CC1101_SIDLE);
    digitalWrite(PIN_CC1101_CS, HIGH);
    gate_bus->endTransaction();

    gate_ready = true;
    event_add("gate: CC1101 OOK ready (version 0x%02X, partnum 0x%02X)",
              version, gate_status_read(CC1101_PARTNUM));

    /* The probe bus dies with probe_cc1101()'s stack frame; runtime access
       goes through gate_runtime_bus() (the display's instance). */
    gate_bus = nullptr;
    skill_register(&gate_skill);
}