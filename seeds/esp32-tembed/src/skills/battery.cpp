/*
 * skills/battery.cpp — the BQ27220 fuel gauge, and what it thinks of itself
 *
 * Endpoints:
 *   GET /battery — every register this firmware reads, raw and decoded
 *
 * Two readings from the device, taken minutes apart on a known-good charger:
 * 3.95 V reported 52%, and 4.20 V reported 76%. 4.20 V is a full lithium cell.
 * A gauge that calls a full cell 76% is not describing this battery, and the
 * error is not a scale error either — the span between those two points is
 * roughly right and the offset is very nearly constant (-26, -24 points). That
 * is the shape of a RemainingCapacity that was seeded from the wrong place,
 * not of a FullChargeCapacity in the wrong units.
 *
 * "Roughly right" and "very nearly" are as far as two data points reach, and
 * that is the whole reason this file exists.
 *
 * WHAT THIS SKILL DOES NOT DO
 * ---------------------------
 * It does not fix it. It writes NOTHING to the gauge — not an unseal, not a
 * CONFIG UPDATE, not a chemistry profile, not a design capacity. The only byte
 * this file ever puts on the wire is a register pointer, which is how a read
 * is addressed on this part and is not a write in any sense that matters.
 *
 * That restraint is the point. Correcting a gauge means unsealing it and
 * rewriting data-flash parameters on the owner's live pager, and the correct
 * value to write is exactly what nobody knows yet. So this reads out the
 * gauge's own view — what it thinks it holds, what it thinks full is, what it
 * was told the pack was designed for, whether it is sealed, whether it has
 * finished initialising, whether it has ever seen a discharge it trusts — and
 * puts all of it behind one GET. Then the fix can be aimed at a number instead
 * of at a theory.
 *
 * Because the theory that is easy to reach for does not survive the readings.
 * "The gauge defaults to 3000 mAh and the pack is 1300 mAh" is the popular
 * diagnosis for this family, and it predicts a hard ceiling around 43%: with
 * the wrong FullChargeCapacity, a full pack can never report more. This device
 * reports 76%. Whatever is wrong here, it is not that, and GET /battery is how
 * the next commit finds out what it is instead.
 *
 * The registers
 * -------------
 * All seven are STANDARD COMMANDS: write the command code, read two bytes,
 * little-endian. No sealing gets in the way of any of them, which is why the
 * whole diagnostic is available without touching the security state.
 *
 *   0x08 Voltage              mV
 *   0x0A BatteryStatus        flags, decoded below
 *   0x10 RemainingCapacity    mAh
 *   0x12 FullChargeCapacity   mAh
 *   0x2C RelativeStateOfCharge  %
 *   0x3A OperationStatus      flags, decoded below
 *   0x3C DesignCapacity       mAh
 *
 * The first and the fifth are the two this firmware has always read; the other
 * five are new here and go nowhere near the clock face.
 *
 * A word about ITPOR, because somebody will look for it
 * ----------------------------------------------------
 * ITPOR is the bit you want when you suspect the gauge came up on ROM defaults
 * — it means "the RAM configuration has been reset and needs reprogramming".
 * It is NOT DECODED BELOW, and not by oversight: the BQ27220 does not expose
 * it. The TRM (SLUUBD4A) names Flags()[ITPOR] in its state-machine prose,
 * inherited from older parts in the family, but on this device Flags() *is*
 * BatteryStatus() (Table 2-1, Standard Commands, which lists both names
 * against the one pair of command codes 0x0A/0x0B) and Table 2-6 gives its
 * sixteen bits with no ITPOR among them — low-byte bit 7 is reserved. It is
 * absent from OperationStatus (Table 2-7), from CONTROL_STATUS (Table 2-3)
 * and from GaugingStatus (Table 2-4) as well.
 *
 * Naming a decoded flag ITPOR here would mean guessing a bit position, and a
 * guessed diagnostic that reads "itpor: false" is worse than no diagnostic at
 * all — it answers the question wrongly and confidently. So both status words
 * are also reported RAW, in hex, reserved bits and all. If TI populates
 * something undocumented, the hex shows it and a human decides what it means.
 *
 * Cadence: with the other two, every 60 seconds
 * --------------------------------------------
 * The five new registers are read on the SAME 60 s tick as Voltage and
 * StateOfCharge, in one pass, from loop(); GET /battery serves the cache and
 * reports how old it is. Reading them only on request would be cheaper still,
 * and cost is not the reason.
 *
 * The reason is that loop() must stay the only task that touches the I2C bus.
 * Arduino's TwoWire is not re-entrant and there is exactly one bus here, with
 * the charger on it as well. Serving GET /battery with an I2C transaction
 * would run it on the AsyncTCP task, where a request landing during the 60 s
 * refresh interleaves two transactions on one bus — a fault with no witness,
 * on a device with no console, in the diagnostic that exists to be trusted.
 *
 * The freshness given up is worth nothing here. Every one of the five is a
 * configuration or state fact: a capacity in mAh, a security mode, whether
 * initialisation finished. They change on the order of a charge cycle or a
 * reset, not of a second. The response carries `age_s` so the reader never has
 * to assume.
 *
 * Where the values go
 * -------------------
 * hw.battery_v and hw.battery_soc are set here exactly as before, and the
 * three surfaces that read them — the clock header, the status screen and
 * /capabilities — are untouched. Their sanity gates are unchanged too: an
 * implausible voltage leaves the previous reading alone rather than putting a
 * wrong number on the panel, and a state of charge above 100 becomes -1, which
 * those surfaces already draw as "--".
 *
 * Testing
 * -------
 * tools/test_battery.sh compiles the decode below on the host, straight out of
 * this file: the little-endian word assembly, both status decoders, the sanity
 * gates and the RM/FCC cross-check.
 *
 * What is NOT covered, plainly: the I2C transaction itself. bq27220_read16()
 * is four calls into TwoWire and its failure modes are a missing pull-up, a
 * NAK and a bus somebody else is already driving. None of that has a host
 * equivalent, no test below pretends otherwise, and the only proof it works is
 * a reading off the device.
 */

/* Standard commands. Every one is a two-byte little-endian read. */
#define BQ_CMD_VOLTAGE            0x08
#define BQ_CMD_BATTERY_STATUS     0x0A
#define BQ_CMD_REMAINING_CAP      0x10
#define BQ_CMD_FULL_CHARGE_CAP    0x12
#define BQ_CMD_SOC                0x2C
#define BQ_CMD_OPERATION_STATUS   0x3A
#define BQ_CMD_DESIGN_CAP         0x3C

/* --- The decode ---
 *
 * Everything from here to the end marker is compiled verbatim on the host by
 * tools/test_battery.sh. Keep the two markers on lines of their own, and keep
 * every comment inside fully closed: the slicer copies from the marker without
 * understanding what it copies. */
/* host-test:begin gauge — sliced out by tools/test_battery.sh */

/* The wire order, in one place. Every register on this part is little-endian
   and the low byte arrives first, so this is the whole of it — but it is the
   piece a future reader is most likely to "obviously" have backwards, and it
   is the piece that turns 4200 mV into 26640 mV when it is. */
static uint16_t bq_word(uint8_t lo, uint8_t hi) {
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

/* The gates the older code applied inline, unchanged in meaning. Both bounds
   are exclusive because that is how they were written: 2000 mV is below any
   living lithium cell and 6000 mV is above any single one, and a reading at
   either is the bus talking, not the battery. */
static bool bq_voltage_ok(uint16_t mv) {
    return mv > 2000 && mv < 6000;
}

static bool bq_soc_ok(uint16_t soc) {
    return soc <= 100;
}

/*
 * OperationStatus() 0x3A, TRM SLUUBD4A Table 2-7. Bit numbering below is of
 * the assembled 16-bit word, so the table's "high byte bit 2" is bit 10 here.
 */
#define BQ_OP_CALMD      (1u << 0)   /* calibration mode                      */
#define BQ_OP_SEC_SHIFT  1           /* SEC[1:0] at bits 2:1                  */
#define BQ_OP_SEC_MASK   0x3u
#define BQ_OP_EDV2       (1u << 3)   /* cell below the EDV2 threshold         */
#define BQ_OP_VDQ        (1u << 4)   /* this discharge qualifies for FCC      */
#define BQ_OP_INITCOMP   (1u << 5)   /* initialisation finished               */
#define BQ_OP_SMTH       (1u << 6)   /* RemainingCapacity is being smoothed   */
#define BQ_OP_BTPINT     (1u << 7)   /* a BTP threshold was crossed           */
#define BQ_OP_CFGUPDATE  (1u << 10)  /* in CONFIG UPDATE, gauging suspended   */

/*
 * BatteryStatus() 0x0A, TRM SLUUBD4A Table 2-6. This is the register the TRM
 * elsewhere calls Flags().
 */
#define BQ_BS_DSG        (1u << 0)   /* discharging                           */
#define BQ_BS_SYSDWN     (1u << 1)   /* the system should shut down           */
#define BQ_BS_TDA        (1u << 2)   /* terminate discharge alarm             */
#define BQ_BS_BATTPRES   (1u << 3)   /* a battery is present                  */
#define BQ_BS_AUTH_GD    (1u << 4)   /* an inserted battery was detected      */
#define BQ_BS_OCVGD      (1u << 5)   /* a good open-circuit reading was taken */
#define BQ_BS_TCA        (1u << 6)   /* terminate charge alarm                */
#define BQ_BS_CHGINH     (1u << 8)   /* charging inhibited, out of temp range */
#define BQ_BS_FC         (1u << 9)   /* FULL CHARGE detected                  */
#define BQ_BS_OTD        (1u << 10)  /* over-temperature while discharging    */
#define BQ_BS_OTC        (1u << 11)  /* over-temperature while charging       */
#define BQ_BS_SLEEP      (1u << 12)  /* the gauge is in SLEEP                 */
#define BQ_BS_OCVFAIL    (1u << 13)  /* an open-circuit reading failed        */
#define BQ_BS_OCVCOMP    (1u << 14)  /* an open-circuit update completed      */
#define BQ_BS_FD         (1u << 15)  /* full discharge detected               */

/*
 * The security mode, as a word rather than as two bits.
 *
 * It is the first thing to look at and the one that decides what commit 2 is
 * even allowed to attempt: a SEALED gauge accepts no configuration until it is
 * unsealed with the right key, so "sealed" and "full" are two different pieces
 * of work rather than two spellings of the same one.
 *
 * The TRM defines three of the four codes. 00 is left undefined there, so it
 * is reported as unknown rather than folded into one of the others — a gauge
 * answering something the datasheet does not list is news, and quietly calling
 * it "sealed" would bury it.
 */
static const char *bq_sec_name(uint16_t op_status) {
    switch ((op_status >> BQ_OP_SEC_SHIFT) & BQ_OP_SEC_MASK) {
        case 0x3: return "sealed";
        case 0x2: return "unsealed";
        case 0x1: return "full";
        default:  return "unknown";
    }
}

/* The raw two bits, so a caller can see what produced the name above. */
static uint8_t bq_sec_bits(uint16_t op_status) {
    return (uint8_t)((op_status >> BQ_OP_SEC_SHIFT) & BQ_OP_SEC_MASK);
}

/* One named flag out of either status word. Written out rather than left at
   the call sites so that every test below exercises the same function the
   endpoint does. */
static bool bq_flag(uint16_t word, uint16_t mask) {
    return (word & mask) != 0;
}

/*
 * What the gauge's own numbers say its state of charge should be.
 *
 * This is the cross-check the whole file was built for. RelativeStateOfCharge
 * is meant to be RemainingCapacity over FullChargeCapacity; if the reported
 * percentage agrees with this figure, then the gauge is doing its arithmetic
 * correctly on wrong inputs and the fix is in the capacities. If it disagrees,
 * something else is producing the percentage and the capacities are a red
 * herring.
 *
 * Rounds to nearest rather than truncating, so a one-point disagreement is
 * this function's own rounding and not evidence. Returns -1 when
 * FullChargeCapacity is zero — a gauge that has not established a full charge
 * is a finding in itself, and dividing by it would only turn that finding into
 * a crash.
 */
static int bq_soc_from_capacity(uint16_t remaining_mah, uint16_t full_mah) {
    if (full_mah == 0) return -1;
    return (int)(((uint32_t)remaining_mah * 100u + full_mah / 2u) / full_mah);
}
/* host-test:end */

/* --- The bus --- */

/*
 * One register, two bytes. The write is the register pointer and nothing else:
 * see the head of this file for why that is the only thing this skill is
 * allowed to put on the wire.
 *
 * endTransmission(false) holds the bus for the repeated START the part expects
 * between the pointer write and the read.
 */
static bool bq27220_read16(uint8_t reg, uint16_t &val) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_ADDR, 2) != 2) return false;
    uint8_t lo = (uint8_t)Wire.read();
    uint8_t hi = (uint8_t)Wire.read();
    val = bq_word(lo, hi);
    return true;
}

/* --- The cache --- */

/*
 * The five diagnostic registers, with a validity flag each rather than a
 * sentinel value: 0 mAh of remaining capacity and "the read failed" are
 * different findings and a reader chasing this bug must be able to tell them
 * apart.
 */
struct BatteryGauge {
    /* The two the rest of the firmware already reads, kept here as well and in
       the units the gauge sent them: hw.battery_v is a float built from these
       millivolts for the panel, and reporting the diagnostic back out of it
       would mean answering "what does the register say" with a round trip
       through a display value. `soc` is -1 when the register was unreadable or
       said something a percentage cannot mean. */
    uint16_t voltage_mv;
    int soc;
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t design_mah;
    uint16_t op_status;
    uint16_t batt_status;
    bool remaining_ok;
    bool full_ok;
    bool design_ok;
    bool op_ok;
    bool batt_ok;
    unsigned long read_ms;   /* millis() at the end of the pass; 0 = never */
};

static BatteryGauge battery_gauge;
static portMUX_TYPE battery_mux = portMUX_INITIALIZER_UNLOCKED;

/* --- Reading --- */

/*
 * One pass over every register, from the loop task and from nowhere else.
 *
 * `at_boot` is what allows the gauge to be declared present, so a bus glitch
 * during a later refresh cannot make a device that has no fuel gauge start
 * claiming one. Everything below that line behaves exactly as the two
 * functions this replaced did: an implausible voltage abandons the pass and
 * leaves the previous reading in place, and a state of charge the part cannot
 * mean becomes -1.
 *
 * The I2C happens into a local, and only the finished local is copied into the
 * shared cache under the lock. A critical section around a bus transaction
 * would block the other task for milliseconds at a time.
 */
static void battery_read(bool at_boot) {
    if (!at_boot && !hw.has_battery) return;

    uint16_t mv = 0;
    if (!bq27220_read16(BQ_CMD_VOLTAGE, mv) || !bq_voltage_ok(mv)) return;
    if (at_boot) hw.has_battery = true;
    hw.battery_v = mv / 1000.0f;

    uint16_t soc = 0;
    hw.battery_soc = (bq27220_read16(BQ_CMD_SOC, soc) && bq_soc_ok(soc)) ? (int)soc : -1;

    BatteryGauge g;
    memset(&g, 0, sizeof(g));
    g.voltage_mv   = mv;
    g.soc          = hw.battery_soc;
    g.remaining_ok = bq27220_read16(BQ_CMD_REMAINING_CAP, g.remaining_mah);
    g.full_ok      = bq27220_read16(BQ_CMD_FULL_CHARGE_CAP, g.full_mah);
    g.design_ok    = bq27220_read16(BQ_CMD_DESIGN_CAP, g.design_mah);
    g.op_ok        = bq27220_read16(BQ_CMD_OPERATION_STATUS, g.op_status);
    g.batt_ok      = bq27220_read16(BQ_CMD_BATTERY_STATUS, g.batt_status);
    g.read_ms      = millis();

    portENTER_CRITICAL(&battery_mux);
    battery_gauge = g;
    portEXIT_CRITICAL(&battery_mux);
}

/* Called once from hw_probe(), before the display claims the SPI bus. */
static void battery_probe() {
    battery_read(true);
}

/* Called from loop() every 60 s. Without it the panel would show the boot-time
   snapshot until the next reboot. */
static void battery_refresh() {
    battery_read(false);
}

/* --- Endpoints --- */

static const SkillEndpoint battery_endpoints[] = {
    {"GET", "/battery", "Fuel gauge registers, raw and decoded (read-only)"},
    {NULL, NULL, NULL}
};

static const char *battery_describe() {
    return "## Skill: battery\n\n"
           "A BQ27220 CEDV fuel gauge at I2C 0x55 (SDA=8, SCL=18). This skill\n"
           "reads it and reports it. **It never writes to it** — no unseal, no\n"
           "CONFIG UPDATE, no configuration of any kind. The only byte it puts\n"
           "on the wire is a register pointer.\n\n"
           "For charging — why a charge stopped, at what current it terminates,\n"
           "faults — see `GET /charger`. The BQ25896 charger shares this I2C\n"
           "bus but is a separate skill, because that one writes and this one\n"
           "does not.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /battery | Every register read here, raw and decoded |\n\n"
           "### Why it exists\n\n"
           "The reported state of charge is wrong on this device: 3.95 V read\n"
           "52% and 4.20 V read 76%, and 4.20 V is a full cell. The firmware\n"
           "has never configured the gauge, so it runs on ROM defaults. This\n"
           "endpoint exists so the gauge's own view can be read before anything\n"
           "is changed. It diagnoses; it does not correct.\n\n"
           "### What you get\n\n"
           "Seven standard commands, each a two-byte little-endian read:\n\n"
           "| Field | Command | Meaning |\n"
           "|-------|---------|---------|\n"
           "| `voltage_mv` | 0x08 | cell voltage, mV |\n"
           "| `soc_percent` | 0x2C | RelativeStateOfCharge, % |\n"
           "| `remaining_mah` | 0x10 | RemainingCapacity |\n"
           "| `full_charge_mah` | 0x12 | FullChargeCapacity |\n"
           "| `design_mah` | 0x3C | DesignCapacity |\n"
           "| `operation_status` | 0x3A | security mode and gauging state |\n"
           "| `battery_status` | 0x0A | charge/discharge flags |\n\n"
           "The last five are objects: each carries `ok`, and a value only when\n"
           "`ok` is true, so a failed bus transaction never looks like a\n"
           "reading of zero. `voltage_mv` and `soc_percent` are plain numbers\n"
           "and are absent when there is nothing to report — an unreadable\n"
           "voltage abandons the whole pass, which is why it also leaves\n"
           "`age_s` growing.\n\n"
           "`soc_from_capacity` is DERIVED, not read: it is\n"
           "`round(100 * remaining / full_charge)`. Compare it with\n"
           "`soc_percent`. If they agree, the gauge is doing correct arithmetic\n"
           "on wrong capacities. If they disagree, the percentage is coming\n"
           "from somewhere else and the capacities are not the fault. Like\n"
           "`voltage_mv` and `soc_percent` it is absent when there is nothing\n"
           "to report — when either capacity read failed, and when\n"
           "FullChargeCapacity is zero and the figure cannot be computed at\n"
           "all.\n\n"
           "### The two status words\n\n"
           "Both are reported as `raw` (hex) and as decoded flags. Decoded from\n"
           "`operation_status`: `sec` (`sealed` / `unsealed` / `full` /\n"
           "`unknown`, with `sec_bits` alongside it), `cfgupdate`, `vdq`,\n"
           "`initcomp`, `smth`, `edv2`, `btpint`, `calmd`. From\n"
           "`battery_status`: `fc` (full charge), `fd`, `dsg`, `battpres`,\n"
           "`auth_gd`, `ocvgd`, `ocvcomp`, `ocvfail`, `chginh`, `tca`, `tda`,\n"
           "`sysdwn`, `otc`, `otd`, `sleep`.\n\n"
           "`sec` decides what is possible at all: a SEALED gauge accepts no\n"
           "configuration until it is unsealed with the right key.\n\n"
           "**There is no `itpor` flag, and its absence is deliberate.** ITPOR\n"
           "— \"the RAM configuration was reset to defaults\" — is named in the\n"
           "BQ27220 TRM's prose as `Flags()[ITPOR]`, but on this part `Flags()`\n"
           "is `BatteryStatus()` and its bit table has no such bit; nor does\n"
           "OperationStatus, CONTROL_STATUS or GaugingStatus. Decoding it would\n"
           "mean guessing a bit position and answering the question wrongly.\n"
           "The `raw` hex of both words is reported for exactly this reason —\n"
           "reserved bits included, so an undocumented one is at least visible.\n\n"
           "### Freshness\n\n"
           "Every register is read together on one 60-second tick in the main\n"
           "loop, and this endpoint serves that cache rather than touching the\n"
           "bus itself: the loop task is the only task allowed on the I2C bus,\n"
           "because two interleaved transactions on one bus would be a fault\n"
           "with no witness. `age_s` is how many seconds ago the pass ran, and\n"
           "`fresh` is false when nothing has been read yet. Capacities and\n"
           "security modes change on the order of a charge cycle, so the delay\n"
           "costs nothing.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" http://seed.local:8080/battery\n"
           "```\n";
}

static void battery_register_routes(AsyncWebServer &server) {

    /* GET /battery */
    server.on(AsyncURIMatcher::exact("/battery"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        /* One copy of the cache under the lock, reported outside it: building
           the JSON allocates and must not happen in a critical section. */
        BatteryGauge g;
        portENTER_CRITICAL(&battery_mux);
        g = battery_gauge;
        portEXIT_CRITICAL(&battery_mux);

        /* Sampled AFTER the copy, and that ordering is the whole of the
           correctness of age_s. Taken before it, this task could read the
           clock, be preempted, and come back to a snapshot the loop task
           stamped later — now - g.read_ms would then borrow on unsigned long
           and report 4294967 seconds in the endpoint whose one job is to be
           believed. Read second, g.read_ms is always at or behind `now`. */
        unsigned long now = millis();

        JsonDocument doc;
        doc["present"] = hw.has_battery;
        doc["address"] = "0x55";
        doc["part"] = "BQ27220";
        doc["writes"] = false;

        doc["fresh"] = (g.read_ms != 0);
        if (g.read_ms != 0) doc["age_s"] = (unsigned long)(now - g.read_ms) / 1000;

        /* Out of the same snapshot as everything else, so one age_s covers the
           whole response rather than most of it. */
        if (g.read_ms != 0) {
            doc["voltage_mv"] = g.voltage_mv;
            if (g.soc >= 0) doc["soc_percent"] = g.soc;
        }

        char hex[8];

        JsonObject rem = doc["remaining_mah"].to<JsonObject>();
        rem["ok"] = g.remaining_ok;
        if (g.remaining_ok) rem["value"] = g.remaining_mah;

        JsonObject full = doc["full_charge_mah"].to<JsonObject>();
        full["ok"] = g.full_ok;
        if (g.full_ok) full["value"] = g.full_mah;

        JsonObject design = doc["design_mah"].to<JsonObject>();
        design["ok"] = g.design_ok;
        if (g.design_ok) design["value"] = g.design_mah;

        /* Derived, and only when both inputs are real. */
        if (g.remaining_ok && g.full_ok) {
            int derived = bq_soc_from_capacity(g.remaining_mah, g.full_mah);
            if (derived >= 0) doc["soc_from_capacity"] = derived;
        }

        JsonObject op = doc["operation_status"].to<JsonObject>();
        op["ok"] = g.op_ok;
        if (g.op_ok) {
            snprintf(hex, sizeof(hex), "0x%04X", g.op_status);
            op["raw"] = String(hex);
            op["sec"] = bq_sec_name(g.op_status);
            op["sec_bits"] = bq_sec_bits(g.op_status);
            op["cfgupdate"] = bq_flag(g.op_status, BQ_OP_CFGUPDATE);
            op["vdq"]       = bq_flag(g.op_status, BQ_OP_VDQ);
            op["initcomp"]  = bq_flag(g.op_status, BQ_OP_INITCOMP);
            op["smth"]      = bq_flag(g.op_status, BQ_OP_SMTH);
            op["edv2"]      = bq_flag(g.op_status, BQ_OP_EDV2);
            op["btpint"]    = bq_flag(g.op_status, BQ_OP_BTPINT);
            op["calmd"]     = bq_flag(g.op_status, BQ_OP_CALMD);
        }

        JsonObject bs = doc["battery_status"].to<JsonObject>();
        bs["ok"] = g.batt_ok;
        if (g.batt_ok) {
            snprintf(hex, sizeof(hex), "0x%04X", g.batt_status);
            bs["raw"] = String(hex);
            bs["fc"]       = bq_flag(g.batt_status, BQ_BS_FC);
            bs["fd"]       = bq_flag(g.batt_status, BQ_BS_FD);
            bs["dsg"]      = bq_flag(g.batt_status, BQ_BS_DSG);
            bs["battpres"] = bq_flag(g.batt_status, BQ_BS_BATTPRES);
            bs["auth_gd"]  = bq_flag(g.batt_status, BQ_BS_AUTH_GD);
            bs["ocvgd"]    = bq_flag(g.batt_status, BQ_BS_OCVGD);
            bs["ocvcomp"]  = bq_flag(g.batt_status, BQ_BS_OCVCOMP);
            bs["ocvfail"]  = bq_flag(g.batt_status, BQ_BS_OCVFAIL);
            bs["chginh"]   = bq_flag(g.batt_status, BQ_BS_CHGINH);
            bs["tca"]      = bq_flag(g.batt_status, BQ_BS_TCA);
            bs["tda"]      = bq_flag(g.batt_status, BQ_BS_TDA);
            bs["sysdwn"]   = bq_flag(g.batt_status, BQ_BS_SYSDWN);
            bs["otc"]      = bq_flag(g.batt_status, BQ_BS_OTC);
            bs["otd"]      = bq_flag(g.batt_status, BQ_BS_OTD);
            bs["sleep"]    = bq_flag(g.batt_status, BQ_BS_SLEEP);
        }

        /* Said in the payload as well as in describe(): whoever is chasing the
           wrong percentage will read this response long before they read the
           manual, and looking for a flag that does not exist is exactly the
           hour this line is here to save. */
        doc["itpor"] = "not readable on the BQ27220: no such bit in "
                       "BatteryStatus, OperationStatus, CONTROL_STATUS or "
                       "GaugingStatus (TRM SLUUBD4A). See the raw words.";

        notify_send_json(req, 200, doc);
    });
}

static const Skill battery_skill = {
    .name = "battery",
    .version = "0.1.0",
    .describe = battery_describe,
    .endpoints = battery_endpoints,
    .register_routes = battery_register_routes
};

static void skill_battery_init() {
    /* Nothing to bring up: battery_probe() has already run from hw_probe(),
       which is where it has to run — before tft.init() claims the SPI bus and
       before the first clock face is drawn. */
    skill_register(&battery_skill);
}
