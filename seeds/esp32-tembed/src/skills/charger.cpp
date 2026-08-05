/*
 * skills/charger.cpp — the BQ25896 charger, configured once at boot
 *
 * Endpoints:
 *   GET /charger — what was written, read back, plus charge state and faults
 *
 * WHY THIS IS NOT IN battery.cpp
 * ------------------------------
 * The original reason was that battery.cpp promised, at length and in prose,
 * that it wrote NOTHING to the gauge, and putting writes into a file whose
 * header argues against writing would make that file lie about itself. That
 * reason has since expired: battery.cpp now writes six parameters to the
 * gauge's data memory, and its header says so. Both skills write.
 *
 * The split stands anyway, on the reason that was always underneath the first
 * one: they are two parts, at two addresses, with two independent failures and
 * two different guards. The BQ25896 can drive 4.6 V into a lithium cell and
 * bq25896_write_allowed() is what stands between a typo and a fire. The
 * BQ27220 has no FET drivers and no charge control at all, so the worst its
 * guard prevents is a silently corrupted gauge model. Those are not the same
 * risk and they should not share a file, a bounds check, or a reviewer's
 * attention budget.
 *
 * The two fixes ARE related and land in order, which is worth knowing when
 * reading either: this file makes the charger terminate at 64 mA, and
 * battery.cpp sets the gauge's taper threshold to 100 mA — ABOVE it, not equal
 * to it. Those two numbers being DIFFERENT is the point, and it took three
 * failed attempts on this device to establish it. The gauge qualifies a
 * termination by watching the current sit inside a band for eighty seconds; if
 * its ceiling equals the charger's cut-off, the band is exactly the range the
 * charger refuses to operate in and nothing ever occupies it.
 *
 * Change REG05's ITERM here and the gauge's TaperCurrent has to move with it,
 * keeping the ordering and the gap — tools/test_battery.sh asserts the
 * inequality and the ratio across both files, for exactly that reason.
 *
 * WHY A SEPARATE ENDPOINT RATHER THAN A SECTION OF /battery
 * --------------------------------------------------------
 * A caller thinks of all of this as "the battery", and there is a real case for
 * one endpoint. It is not taken, for one reason: serving charger data out of
 * /battery would mean battery.cpp reaching into this file's cache, and a skill
 * that reads another skill's private state has stopped being a separate skill —
 * it is the same coupling this split exists to remove, one level less visible.
 *
 * So the two are found the way every other skill here is found: both are in the
 * skill registry, both list their endpoints in /capabilities, and /battery's
 * own description points at this one by name. Nothing is hidden; a reader who
 * arrives at the gauge is told in one line where the charging answer lives.
 *
 * 🔴 THE I2C RULE, WHICH THIS FILE OBEYS AND MUST KEEP OBEYING
 * -----------------------------------------------------------
 * loop() and hw_probe() are the ONLY places allowed to touch the I2C bus.
 * Arduino's TwoWire is not re-entrant, there is one bus here, and the gauge is
 * on it too. An AsyncWebServer handler runs on the AsyncTCP task: a request
 * landing during the 60 s refresh would interleave two transactions on one bus
 * — a fault with no witness, on a device with no console.
 *
 * So EVERY byte this file puts on the wire goes out from charger_probe(),
 * called by hw_probe() at boot, or from charger_refresh(), called by loop()
 * every 60 s — reads on every pass, and the writes that re-issue a drifted
 * configuration on the passes that find one. GET /charger copies a cache under
 * a spinlock and serves that, exactly as GET /battery does, and reports how old
 * it is. There is no path from a request to Wire in this file, and there must
 * not be one added.
 *
 * What was wrong
 * --------------
 * The firmware had never written a byte to the charger. It existed as an
 * address in the I2C scan table and nowhere else, so REG05 sat at the value it
 * powers up holding, and on that value this cell never reached full.
 * Measured through GET /battery, on this device, over one charge:
 *
 *   CV phase:   4200 mV, RemainingCapacity still climbing, 940 -> 1004 mAh
 *   terminated: 4139 mV holding 1004 of 1300 mAh — 296 mAh short, 77%
 *   rested:     3996 mV, 892 of 1300 mAh, 69%
 *
 * The cause is REG05. Its POR value is 0x13, which puts the termination
 * current at 256 mA. On a 1300 mAh cell that is C/5, and the textbook figure
 * for a lithium polymer cell is C/10 to C/20. Charge is cut while the CV taper
 * is still well above the real end point, and a C/5 cutoff is exactly where
 * 296 mAh goes missing.
 *
 * Three things that look like the cause and are not, recorded so that nobody
 * spends the afternoon a second time:
 *
 *   VREG. Its default is 4.208 V, which is ABOVE 4.2 and not below. The
 *     4139 mV reading is the cell relaxing after BATFET opened at termination,
 *     not a regulation point.
 *   The safety timer. Default 12 h; the session was hours shorter.
 *   Thermal regulation. It would suppress termination, not cause it.
 *
 * What is written, and in this order
 * ----------------------------------
 *   REG07  0x8D   EN_TERM on, WATCHDOG DISABLED, safety timer on at 12 h
 *   REG04  0x08   ICHG 512 mA (0.39 C)
 *   REG05  0x10   IPRECHG 128 mA, ITERM 64 mA (C/20) — the actual fix
 *   REG06  0x5E   VREG 4.208 V, BATLOWV 3.0 V, VRECHG 100 mV
 *
 * THREE of those four are what LilyGO's own production firmware writes to this
 * board (examples/test_battery_bq27220_bq25896 and the shipped examples/factory,
 * which carry identical constants). For REG07, REG04 and REG06 this is not an
 * independent guess that happens to agree with them; it is the vendor's
 * configuration for the hardware they built, and the agreement is the reason to
 * be confident in it.
 *
 * REG05 IS THE ONE THAT NO LONGER MATCHES THE VENDOR, and that is deliberate
 * rather than drift. LilyGO write 0x11 — ITERM 128 mA — and this file wrote the
 * same for two commits. It is a defensible charger setting on its own; what it
 * is not is a setting the GAUGE can see a charge finish under, because 128 mA
 * is also where the gauge's taper threshold sat, and a gauge whose ceiling
 * equals the charger's cut-off waits for a current band the charger never
 * produces. Lowering the charger to 64 mA is what opens room for the gauge's
 * threshold to sit above it. The vendor's firmware has the same latent defect;
 * agreeing with it here would mean keeping the bug. See battery.cpp's
 * gauge_config[] for the other half and for the ratio the two now hold.
 *
 * REG07 IS FIRST AND THAT IS NOT COSMETIC. It closes the watchdog window
 * before the others land, so that the writes that matter cannot be undone by
 * a timer that was already running when we arrived.
 *
 * WHY REG06 IS WRITTEN, WHEN THE ARGUMENT USED TO BE THAT IT MUST NOT BE
 * ---------------------------------------------------------------------
 * An earlier draft of this work set VREG to 4.192 V. It does not: LilyGO ship
 * 4208 mV on this exact cell, that is also the value the part powers up
 * holding, and 4.192 V would be about 16 mV of extra margin — a real but small
 * lifetime benefit, and a variable that would have to be subtracted every time
 * this device's behaviour is compared against the vendor's. The margin is still
 * being declined; 0x5E is 4.208 V.
 *
 * What changed is the second half of that argument, which used to run: the
 * value we want is the value the part powers up holding, so the correct way to
 * arrive there is to write nothing at all. That premise is false on this
 * device, and this endpoint's own output is what disproves it.
 *
 * REG00 reads 0x3F here: EN_ILIM clear, IINLIM 3250 mA. The datasheet's
 * power-on value (SLUSC76C Table 6) is 0x48 — EN_ILIM set, IINLIM 500 mA — and
 * EN_ILIM is restored to 1 by REG_RST and by a watchdog expiry alike, so a
 * cleared EN_ILIM can only have been put there by a host write. This firmware
 * has never written REG00 and the guard refuses it, so something that is not us
 * did: LilyGO's factory firmware, a bench tool, whatever ran on this board
 * before we did.
 *
 * The charger is powered by the cell. Its registers are volatile, but they do
 * not lose power when the ESP32 is reflashed, so a value written by other
 * software outlives every build we ship. "The part is on its power-on defaults"
 * was an observation about REG05. It was never a property of the part.
 *
 * REG06[7:2] is the voltage this board drives into a lithium cell, and code 30
 * — an entirely plausible thing for another host to have left behind — is
 * 4.32 V. Reading that value and reporting it does not stop it, and with the
 * watchdog disabled nothing else would revert it either. So the register is
 * written, to the value we want anyway: it goes out through the same guard as
 * the rest, is read back and compared like the rest, and is re-issued like the
 * rest when a later pass finds it changed. One line in the table below turns
 * the register from trusted into verified.
 *
 * The read-side bound ships as well, and it is not redundant with the write: a
 * write that is refused or NAKed leaves the register holding whatever it held,
 * and `charge_voltage_in_bounds` is what stops that from being reported
 * underneath `configured: true`. See the guard section below.
 *
 * Why the watchdog is disabled rather than kicked
 * ----------------------------------------------
 * REG07[5:4] defaults to 01 = 40 seconds. On expiry the datasheet is explicit:
 * the part "returns to default mode and all registers are reset to default
 * values except IINLIM, VINDPM, VINDPM_OS, BATFET_RST_EN, BATFET_DLY,
 * BATFET_DIS". REG05 is not on that list. An expiry silently restores the
 * 256 mA termination and puts the bug back.
 *
 * The trap, spelled out because it is the kind that costs days: this firmware
 * refreshes the battery every 60 seconds, and 60 is LONGER THAN 40. Kicking
 * the watchdog from that timer would let it expire and revert on roughly every
 * cycle, which presents as "it sometimes charges properly" — the worst shape a
 * bug can have. Adding a second, faster timer to serve a watchdog is a
 * standing obligation on the loop task for no gain. Disabling it is one write
 * at boot and no periodic task at all.
 *
 * What the watchdog protects against is a host that configured the charger and
 * then died, leaving it charging on settings nobody is supervising. The
 * settings it would fall back to are 2048 mA into a 1300 mAh cell. The settings
 * we leave it on are 512 mA. Ours is the safer resting state, which is the
 * whole of the argument for turning the guard off.
 *
 * AND WHAT REPLACES IT, because that argument is only half of one on its own.
 * The watchdog was this part's single autonomous self-correction: whatever had
 * drifted, it reverted within 40 s, to defaults nobody chose but at least to
 * something known. Disabling it means nothing heals unless we heal it. So the
 * 60 s pass that re-reads the written registers also COMPARES them against the
 * table, and when they disagree it re-issues the whole table, in table order,
 * through the same guard, from the same loop task. That is the replacement: not
 * a timer being fed, but a comparison being acted on. It costs nothing while
 * everything agrees, it fires only on disagreement, and `reapplied` in the
 * endpoint counts how often it has fired.
 *
 * SAFETY: what may not be written, and why it is a guard and not a comment
 * -----------------------------------------------------------------------
 * REG06[7:2] sets the voltage this board drives into the cell:
 * V = 3.840 V + 0.016 V * code. The register accepts code 48, which is
 * 4.608 V — around 400 mV of overcharge on a lithium cell, which means plating
 * and venting and fire. The guard refuses any code above 23, which is the
 * 4.208 V we write and the most this firmware will ever put on that register
 * whatever the table says. It bounds the value we ship today and, more to the
 * point, the value somebody edits into the table next year, which is precisely
 * when nobody will be re-reading the datasheet. tools/test_charger.sh proves
 * the bound holds across all 256 bytes.
 *
 * The bound is checked on the READ side too, and that is the half a write-time
 * guard cannot cover: a write that the guard refuses, or that the bus NAKs,
 * leaves the register holding whatever another host left there. So REG06 is
 * decoded on every pass, compared against the same ceiling, and reported as
 * `charge_voltage_in_bounds` — and `configured` is false whenever it is false.
 * An out-of-bounds charge voltage cannot coexist with a response that says the
 * configuration is live.
 *
 * The second overcharge path is the one an audit of VREG alone walks straight
 * past. REG08's IR compensation makes the regulation point
 * VREG + min(ICHRG * BAT_COMP, VCLAMP), and VCLAMP reaches 224 mV, so a
 * nonzero REG08 regulates near 4.43 V with a perfectly correct VREG. The guard
 * refuses REG08 as a register, not as a value, so there is no combination of
 * bits that gets through.
 *
 * The same refusal covers the registers that would make the device look dead
 * or behave as a source: REG09[5] BATFET_DIS (ship mode — cuts the battery,
 * needs USB to recover, and survives a watchdog reset, so a stray write there
 * is stickier than any other), REG00[7] EN_HIZ, and REG03[5] OTG_CONFIG, which
 * turns the part into a 5 V boost driving VBUS outward.
 *
 * REG04 is thermal rather than incendiary: ICHG reaches 3008 mA, which is
 * 2.3 C on this cell, and the board is capped by its vendor at 600 mA. The
 * guard holds it at or below code 9, 576 mA.
 *
 * Nothing here is irreversible. Every register is volatile, there is no OTP on
 * this part, and REG14[7] REG_RST or a power cycle restores the defaults.
 *
 * Input current is reported and never written
 * -------------------------------------------
 * REG00 carries EN_HIZ, EN_ILIM and IINLIM together, and the guard refuses the
 * whole register: EN_HIZ disconnects the input, and nothing here needs the
 * other two.
 *
 * What the input limit did and did not do is worth being exact about, because
 * an earlier draft of this comment blamed the missing 296 mAh partly on it and
 * that was simply wrong. On this device REG00 reads 0x3F — EN_ILIM clear,
 * IINLIM 3250 mA — and under a live charge REG13 reports the in-force input
 * limit as 1050 mA with IDPM_STAT clear, against a 512 mA charge current. The
 * input current was never the constraint, before the fix or after it. The REG05
 * diagnosis rests on its own measurement and needs no help from this one.
 *
 * EN_ILIM is still reported, and it is still the reason the IINLIM number is
 * not an answer on its own: while it is SET the actual limit is the LOWER of
 * the register and the ILIM pin resistor. On this board it is CLEAR, so the
 * register is the limit and the pin resistor is out of the loop — which is to
 * say the decision to hand this board's hardware current limit to software has
 * already been made here, by whoever wrote REG00 before us. It is not one this
 * firmware makes or unmakes; it is one the endpoint shows.
 *
 * What this does NOT report, and why
 * ----------------------------------
 * The charger's own ADC. REG0E/0F/10/11/12 would give VBAT, VSYS, VBUS,
 * charge current and TS percentage, and at POR they all read zero because
 * REG02's CONV_START and CONV_RATE are both off. Reporting them honestly means
 * triggering a one-shot conversion and polling a self-clearing bit on the loop
 * task, or leaving 1 s continuous conversion running, which is standing
 * quiescent current on a battery-powered device. Neither is worth it here: the
 * gauge already reports the cell voltage from its own converter, and the
 * question this commit exists to answer — why did charging stop — is answered
 * by REG0B CHRG_STAT, not by a current reading. So REG0B, REG0C and REG13 are
 * reported, the ADC registers are not, and describe() says so rather than
 * leaving a reader to wonder why the current is missing.
 *
 * Testing
 * -------
 * tools/test_charger.sh compiles the block below on the host, straight out of
 * this file: the register arithmetic, the status and fault decoders, the
 * write-verification comparison, the mapping from live registers back onto the
 * table, the rule that decides whether the configuration is still live, the
 * boot table, and every safety guard.
 *
 * What is NOT covered, plainly: the I2C transactions. bq25896_write8() and
 * bq25896_read8() are calls into TwoWire and their failure modes are a NAK, a
 * missing pull-up and a second task on the bus. None of that has a host
 * equivalent, no test pretends otherwise, and the only proof that half of this
 * file works is a reading taken off the device.
 */

/* --- The registers, the decode, the guards and the values --- */
/* host-test:begin charger — sliced out by tools/test_charger.sh */

/* The eight registers this firmware touches, of the twenty-one the part has.
   Inside the slice rather than above it because the guard's allowlist is
   written in these names and the boot table is addressed by them: they are
   part of what the host test has to be able to hold to account. */
#define BQ25896_REG00_INPUT     0x00   /* EN_HIZ, EN_ILIM, IINLIM       (read) */
#define BQ25896_REG04_ICHG      0x04   /* fast charge current limit    (write) */
#define BQ25896_REG05_TERM      0x05   /* precharge and termination    (write) */
#define BQ25896_REG06_VREG      0x06   /* charge voltage limit    (read; NOT
                                          written — see the header)           */
#define BQ25896_REG07_TIMER     0x07   /* termination, watchdog, timer (write) */
#define BQ25896_REG0B_STATUS    0x0B   /* VBUS / charge / power good    (read) */
#define BQ25896_REG0C_FAULT     0x0C   /* latched faults                (read) */
#define BQ25896_REG13_DPM       0x13   /* DPM status and IDPM_LIM       (read) */

/* Field geometry, from the datasheet's own register tables (SLUSC76C, 9.4).
   Written as base and step rather than as a lookup table because that is how
   the datasheet writes them, and a table would be a transcription with 64
   opportunities to be wrong. */
#define BQ25896_ICHG_MASK        0x7Fu   /* REG04[6:0]                        */
#define BQ25896_ICHG_STEP_MA     64      /* offset 0 mA                       */
#define BQ25896_ICHG_CODE_MAX    9       /* 576 mA. The guard. See below.     */
#define BQ25896_VREG_SHIFT       2       /* REG06[7:2]                        */
#define BQ25896_VREG_BASE_MV     3840
#define BQ25896_VREG_STEP_MV     16
#define BQ25896_VREG_CODE_MAX    23      /* 4.208 V. The guard. See below.    */
#define BQ25896_ITERM_MASK       0x0Fu   /* REG05[3:0]                        */
#define BQ25896_IPRECHG_SHIFT    4       /* REG05[7:4]                        */
#define BQ25896_TERM_BASE_MA     64      /* both fields share base and step   */
#define BQ25896_TERM_STEP_MA     64
#define BQ25896_IINLIM_MASK      0x3Fu   /* REG00[5:0] and REG13[5:0] alike   */
#define BQ25896_IINLIM_BASE_MA   100
#define BQ25896_IINLIM_STEP_MA   50

/*
 * The charge voltage limit, in millivolts, out of a whole REG06.
 *
 * This is the arithmetic the safety guard is expressed in — code 23 is 4208,
 * and code 48, which the guard refuses, is 4608 — and it is also how the
 * register is reported back to a caller after it has been written.
 *
 * It is deliberately NOT clamped at the top, and that is worth one line so the
 * next reader does not "fix" it. The field is six bits, so it decodes codes 49
 * to 63 as 4624 mV up to 4848 mV, above the 4.608 V the part itself will
 * regulate to. Those readings are unreachable from real silicon: the datasheet
 * clamps the regulation point at 4.608 V, and nothing may write a code above 23
 * anyway. Clamping here would report a value the register does not hold, and
 * this decoder's job is to say what the register holds — the bound is
 * bq25896_vreg_in_bounds()'s job, one function down, and it is a separate
 * question from the decode.
 */
static int bq25896_vreg_mv(uint8_t reg06) {
    return BQ25896_VREG_BASE_MV +
           BQ25896_VREG_STEP_MV * (int)(reg06 >> BQ25896_VREG_SHIFT);
}

/*
 * Is the charge voltage the part is CURRENTLY holding one we would allow?
 *
 * The same ceiling as the write guard, asked on the read side, and the reason
 * it exists separately is the whole of finding number two in the review this
 * commit answers. A write guard bounds what this firmware sends. It says
 * nothing about what the register holds when we arrive, and this specific
 * device is the proof that "what the register holds when we arrive" is not the
 * datasheet's power-on value: REG00 came to us at 0x3F, which only a host write
 * produces. A charger left at code 30 by other software regulates at 4.32 V,
 * and if the write below were ever refused or NAKed we would read that, report
 * it, and print `configured: true` directly above it.
 *
 * So the answer folds into `configured` rather than sitting beside it as a
 * field a caller has to know to look for. See charger_config_is_live().
 */
static bool bq25896_vreg_in_bounds(uint8_t reg06) {
    return (uint8_t)(reg06 >> BQ25896_VREG_SHIFT) <= BQ25896_VREG_CODE_MAX;
}

/* The fast charge current limit, in mA, out of a whole REG04. Bit 7 is
   EN_PUMPX and is masked off rather than included: it is not part of the
   number, and a decoder that returned 8192 mA for 0x88 would be lying about
   the one field a reader came here to check. */
static int bq25896_ichg_ma(uint8_t reg04) {
    return BQ25896_ICHG_STEP_MA * (int)(reg04 & BQ25896_ICHG_MASK);
}

/*
 * The termination current, in mA, out of a whole REG05. The 256 mA this
 * returns for the POR value 0x13 is the entire defect.
 *
 * `64 + (reg & 0x0F) * 64` is also what LilyGO's own firmware computes by hand
 * when it verifies this register, because their charger library has no getter
 * for it. Two independent transcriptions of the same datasheet table agreeing
 * is worth more here than either one alone.
 */
static int bq25896_iterm_ma(uint8_t reg05) {
    return BQ25896_TERM_BASE_MA +
           BQ25896_TERM_STEP_MA * (int)(reg05 & BQ25896_ITERM_MASK);
}

/* The precharge current, in mA, out of the same register's high nibble. */
static int bq25896_iprechg_ma(uint8_t reg05) {
    return BQ25896_TERM_BASE_MA +
           BQ25896_TERM_STEP_MA * (int)((reg05 >> BQ25896_IPRECHG_SHIFT) &
                                        BQ25896_ITERM_MASK);
}

/* The input current limit, in mA. One function for two registers on purpose:
   REG00[5:0] IINLIM and REG13[5:0] IDPM_LIM carry the same six-bit field with
   the same offset and step, and giving them separate decoders would be two
   places for the same constant to drift. Neither number is the whole story
   while EN_ILIM is set — see the header, and see `en_ilim` in the response. */
static int bq25896_iinlim_ma(uint8_t reg) {
    return BQ25896_IINLIM_BASE_MA +
           BQ25896_IINLIM_STEP_MA * (int)(reg & BQ25896_IINLIM_MASK);
}

/* REG00[6]. When set — and it is set by default — the actual input current
   limit is the LOWER of the register field and the ILIM pin resistor, so the
   register alone does not tell you what the part will draw. */
static bool bq25896_ilim_pin_enabled(uint8_t reg00) {
    return (reg00 & 0x40u) != 0;
}

/*
 * THE GUARD. Every write to the charger goes through this and there is no
 * other path to the bus — bq25896_write8() calls it and refuses on false.
 *
 * It is an allowlist of registers first and a bounds check second, and that
 * order is deliberate. A bounds check on the values we happen to write leaves
 * REG08's IR compensation, REG09's BATFET_DIS and REG03's OTG_CONFIG wide
 * open, and every one of those is a way to hurt the cell or the board that
 * never touches VREG at all. Refusing the register makes the question "which
 * bits of REG08 are safe" not arise.
 *
 * The two bounds:
 *
 *   REG06  code <= 23, so at most 4.208 V. The register itself reaches code 48
 *          = 4.608 V, which on a 3.7 V lithium cell is roughly 400 mV of
 *          overcharge: plating, venting, fire. The table writes code 23 and
 *          the bound stands above it — it is here for the day somebody edits
 *          that line, which is precisely when nobody will be re-reading the
 *          datasheet. The same predicate judges what the part is found holding;
 *          a guard that only covers the values we send is half a guard.
 *   REG04  code <= 9, so at most 576 mA. The register reaches 3008 mA, which
 *          is 2.3 C here, and the board is capped by its vendor at 600 mA.
 *          Thermal rather than incendiary, and still not ours to exceed.
 *
 * REG05 and REG07 are allowed without a value bound, and that is a claim worth
 * justifying rather than assuming: no bit in either raises the cell voltage or
 * the charge current. REG05 is two current thresholds, both bounded by ICHG
 * above them; REG07 is termination, the watchdog and two timers. The worst
 * REG07 can do is hold the cell at the CV point indefinitely with EN_TERM and
 * EN_TIMER both off, which ages a cell and does not endanger one.
 */
static bool bq25896_write_allowed(uint8_t reg, uint8_t val) {
    switch (reg) {
        case BQ25896_REG04_ICHG:
            return (uint8_t)(val & BQ25896_ICHG_MASK) <= BQ25896_ICHG_CODE_MAX;
        /* The same predicate the read side uses, called rather than repeated:
           one ceiling expressed once, so a bound that is raised for a write can
           never disagree with the bound a reading is judged against. */
        case BQ25896_REG06_VREG:
            return bq25896_vreg_in_bounds(val);
        case BQ25896_REG05_TERM:
        case BQ25896_REG07_TIMER:
            return true;
        default:
            return false;
    }
}

/*
 * REG0B, the register that answers "why did it stop".
 *
 * CHRG_STAT does NOT distinguish constant current from constant voltage — both
 * are 10, "fast charging" — so it cannot say how far along a charge is. What
 * it can say is the thing that was guessed at today and cost 296 mAh: 11 means
 * the part has decided it is finished.
 */
static uint8_t bq25896_chrg_stat(uint8_t reg0b) {
    return (uint8_t)((reg0b >> 3) & 0x3u);
}

static const char *bq25896_chrg_stat_name(uint8_t reg0b) {
    switch (bq25896_chrg_stat(reg0b)) {
        case 0x0: return "not_charging";
        case 0x1: return "pre_charge";
        case 0x2: return "fast_charging";
        default:  return "termination_done";
    }
}

static uint8_t bq25896_vbus_stat(uint8_t reg0b) {
    return (uint8_t)((reg0b >> 5) & 0x7u);
}

/*
 * What the part thinks it is plugged into.
 *
 * The bq25896 defines FOUR of the eight codes — Table 17 lists 000, 001, 010
 * and 111 and nothing else. Related parts in the family fill in the gaps with
 * CDP and DCP and the rest, and borrowing their table here would put a
 * confident wrong name on a code this silicon does not document. The four are
 * named and the other four are "unknown", with the raw value reported beside
 * it so an undocumented answer is at least visible.
 */
static const char *bq25896_vbus_stat_name(uint8_t reg0b) {
    switch (bq25896_vbus_stat(reg0b)) {
        case 0x0: return "none";
        case 0x1: return "usb_host_sdp";
        case 0x2: return "adapter";
        case 0x7: return "otg";
        default:  return "unknown";
    }
}

/* Power good. The input is present and usable — which is a different fact from
   "charging", and the pair of them together is what separates "unplugged" from
   "plugged in and finished". */
static bool bq25896_power_good(uint8_t reg0b) {
    return (reg0b & 0x04u) != 0;
}

/* The system rail is being held at VSYSMIN because the cell is below it. Rare,
   and worth seeing when it happens. REG0B bit 1 is reserved and always reads
   1, so it is decoded by nothing here. */
static bool bq25896_vsys_regulating(uint8_t reg0b) {
    return (reg0b & 0x01u) != 0;
}

/* --- REG0C, the fault register --- */

#define BQ25896_FAULT_WATCHDOG   0x80u
#define BQ25896_FAULT_BOOST      0x40u
#define BQ25896_FAULT_BAT_OVP    0x08u

static uint8_t bq25896_chrg_fault(uint8_t reg0c) {
    return (uint8_t)((reg0c >> 4) & 0x3u);
}

static const char *bq25896_chrg_fault_name(uint8_t reg0c) {
    switch (bq25896_chrg_fault(reg0c)) {
        case 0x0: return "normal";
        case 0x1: return "input";
        case 0x2: return "thermal_shutdown";
        default:  return "safety_timer_expired";
    }
}

static uint8_t bq25896_ntc_fault(uint8_t reg0c) {
    return (uint8_t)(reg0c & 0x7u);
}

/*
 * The thermistor state, in buck mode — which is the only mode this device is
 * ever in, because OTG is never enabled here.
 *
 * The codes are not contiguous: 000, 010, 011, 101, 110 are defined and 001
 * and 100 are not. Written as a switch with a default rather than as an array
 * for exactly that reason.
 */
static const char *bq25896_ntc_fault_name(uint8_t reg0c) {
    switch (bq25896_ntc_fault(reg0c)) {
        case 0x0: return "normal";
        case 0x2: return "ts_warm";
        case 0x3: return "ts_cool";
        case 0x5: return "ts_cold";
        case 0x6: return "ts_hot";
        default:  return "unknown";
    }
}

/*
 * Whether the watchdog fault bit means anything yet.
 *
 * It reads 1 in default mode BY DEFINITION — the part is telling you it is
 * running unconfigured, which before our boot write is a description of the
 * situation rather than a fault. Rendering it as a fault would put a red flag
 * on the healthy state of a device nobody had configured, and the endpoint
 * would be crying wolf in its first sentence.
 *
 * THE GATE IS "WE TRIED TO CONFIGURE IT", NOT "WE SUCCEEDED", and the
 * difference is the one case this flag exists for. Gating on success reasons
 * backwards: if the REG07 write is the one that fails, the watchdog is still
 * armed, it expires, it reverts REG05 and sets bit 7 — and a flag gated on a
 * configuration that never landed would render exactly that as `false`. The
 * scenario the flag is for would be the scenario it hides.
 *
 * So once charger_configure() has run, the bit means what it says: a watchdog
 * we believe we disabled has expired, the settings have been reset underneath
 * us, and the 256 mA bug is back. `watchdog_bit` carries the raw bit either
 * way, so nothing here can lose data — only mislabel it.
 */
static bool bq25896_watchdog_fault_is_real(uint8_t reg0c, bool configure_attempted) {
    return configure_attempted && (reg0c & BQ25896_FAULT_WATCHDOG) != 0;
}

/* --- REG13 --- */

/* Input voltage regulation: the source sagged to its VINDPM point and the
   charger is backing off to hold it there. */
static bool bq25896_in_vindpm(uint8_t reg13) {
    return (reg13 & 0x80u) != 0;
}

/* Input current regulation: the charger is drawing all the input limit allows,
   which is where a 500 mA IINLIM turns into a slow charge. */
static bool bq25896_in_iindpm(uint8_t reg13) {
    return (reg13 & 0x40u) != 0;
}

/* --- The configuration --- */

/*
 * The four writes, IN THE ORDER THEY GO OUT. The array order IS the write
 * order — charger_configure() walks it from index 0, and so does the re-issue
 * on drift — and tools/test_charger.sh asserts that index 0 is REG07, because
 * that ordering is a correctness property and not a matter of taste: REG07
 * closes the 40 s watchdog window, and the writes that carry the actual fix
 * must land inside it. That matters twice over on the re-issue path, where the
 * reason we are writing at all may well be that a watchdog expired.
 *
 * REG06 IS IN THIS TABLE and it did not use to be. The value is the same one
 * the part is expected to hold and the same one the vendor ships; what changed
 * is the argument, and it changed because this device disproved it — see the
 * header. Writing it makes the charge voltage verified rather than trusted,
 * and it is bounded by the guard like every other entry.
 */
struct ChargerWrite {
    uint8_t reg;
    uint8_t val;
    const char *name;
    const char *why;
};

static const ChargerWrite charger_config[] = {
    {BQ25896_REG07_TIMER, 0x8D, "REG07",
     "EN_TERM on, watchdog disabled, safety timer on at 12 h, JEITA 20%"},
    {BQ25896_REG04_ICHG,  0x08, "REG04",
     "ICHG 512 mA, 0.39 C on this cell and what the vendor firmware uses"},
    {BQ25896_REG05_TERM,  0x10, "REG05",
     "IPRECHG 128 mA and ITERM 64 mA, C/20 — low enough that the gauge's "
     "100 mA taper threshold sits above it with margin, which is what lets a "
     "charge finish in the gauge's eyes as well as the charger's"},
    {BQ25896_REG06_VREG,  0x5E, "REG06",
     "VREG 4.208 V, BATLOWV 3.0 V, VRECHG 100 mV — written so it is verified "
     "rather than assumed, since another host has demonstrably written this "
     "part before us"},
};

#define CHARGER_CONFIG_COUNT ((int)(sizeof(charger_config) / sizeof(charger_config[0])))

/*
 * Which writes did not stick, as a bitmask over the table above.
 *
 * A write that is acknowledged on the bus is not a write that took effect: a
 * watchdog expiry between the write and the read, a register the part decided
 * to clamp, or a bus fault that NAKs in the other direction all end with the
 * device charging on something other than what this file says it charges on.
 * So every register is read back and compared, the way LilyGO's own firmware
 * does, and the endpoint reports the result rather than asserting success.
 *
 * Exact comparison is right for all four: every bit of REG05 and of REG06
 * (VREG, BATLOWV, VRECHG) is R/W, and the bits we clear in REG04 and REG07
 * (EN_PUMPX, STAT_DIS) are ours to clear. A register where the part forces bits
 * would need a mask, and none of these is one.
 *
 * Split out from the I2C paths so that the comparison — the part that can be
 * wrong without any hardware present — is testable on the host, while the bus
 * either side of it is honestly left uncovered. It is asked the same question
 * twice: once by charger_configure() about the boot read-back, and once by
 * every charger_read() about what the part is holding sixty seconds later.
 */
static uint8_t charger_verify_mask(const uint8_t *readback) {
    uint8_t mask = 0;
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        if (readback[i] != charger_config[i].val) mask |= (uint8_t)(1u << i);
    }
    return mask;
}

/* Every bit set, which is what "we could not check" is recorded as. Never zero,
   so an unread pass can never be mistaken for a clean one. */
#define CHARGER_MASK_ALL ((uint8_t)((1u << CHARGER_CONFIG_COUNT) - 1u))

/*
 * The four registers the 60 s pass reads, arranged into table order so that
 * charger_verify_mask() can be asked the same question about them that it was
 * asked at boot.
 *
 * This exists as a function, and takes the registers by name rather than as an
 * array, for one reason: the caller has them as named struct fields and the
 * mask wants them in table order, and writing `{c.reg07, c.reg04, c.reg05,
 * c.reg06}` at the call site would hardcode today's table order into a file
 * that also asserts the order is free to change. Here the mapping is derived
 * from charger_config[] itself, so reordering the table cannot silently
 * mis-pair a live register with somebody else's expected value — which would
 * present as a verification that quietly compares REG04 against REG05's byte
 * and passes.
 */
static void charger_live_readback(uint8_t reg04, uint8_t reg05, uint8_t reg06,
                                  uint8_t reg07, uint8_t *out) {
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        switch (charger_config[i].reg) {
            case BQ25896_REG04_ICHG:  out[i] = reg04; break;
            case BQ25896_REG05_TERM:  out[i] = reg05; break;
            case BQ25896_REG06_VREG:  out[i] = reg06; break;
            case BQ25896_REG07_TIMER: out[i] = reg07; break;
            /* A register in the table that this function does not know how to
               fetch cannot be verified, so it is reported as disagreeing rather
               than as matching a zero nobody read. Unreachable while the guard
               allows only these four; here so that widening the guard without
               widening this shows up as a failed verification and not as a
               silent pass. */
            default:                  out[i] = (uint8_t)~charger_config[i].val; break;
        }
    }
}

/*
 * Is the configuration live RIGHT NOW — not "did the boot write succeed".
 *
 * This is the whole of the first review finding. The boot verdict was latched
 * and served forever: a REG05 that reverted at 03:00 still read
 * `configured: true` at noon, with the decoded `termination_current_ma: 256`
 * sitting underneath it saying the opposite. The summary a caller reads first
 * has to be the one that is re-derived, and every input below comes from the
 * pass that just re-read the part:
 *
 *   boot_verified   the writes went out and read back correctly at boot. A
 *                   configuration that never landed is not made live by
 *                   agreeing with itself later.
 *   read_ok         the pass that is being judged actually completed. "We could
 *                   not look" is not "we looked and it was fine".
 *   live_mask       every written register still holds what the table sent.
 *   vreg_in_bounds  and the charge voltage is one we would have allowed, which
 *                   is a bound on what the part HOLDS and not on what we sent.
 *
 * All four, or `configured` is false.
 */
static bool charger_config_is_live(bool boot_verified, bool read_ok,
                                   uint8_t live_mask, bool vreg_in_bounds) {
    return boot_verified && read_ok && live_mask == 0 && vreg_in_bounds;
}

/*
 * Everything a pass concludes about the part, and the ONLY place any of it is
 * concluded.
 *
 * The three functions above are pure and were each proven on the host, and
 * that was not enough: charger_read() still had to wire them together itself,
 * so the one statement that turns "we re-read the registers" into "we
 * re-compared them" sat outside the slice with nothing holding it. Deleting it
 * left every suite green — which is exactly the defect the finding was about,
 * a boot verdict carried forward under an endpoint claiming it was current.
 *
 * So the derivation is one function with one output, filled through a
 * reference the way ring_quiet_flip() fills a RingQuiet, and charger_read()
 * calls it once and copies nothing. What the caller supplies is what only the
 * caller can know: the four registers it just read, whether that read
 * completed, and the boot verdict it carried in. What comes back is the whole
 * verdict. There is no arrangement of these fields that this function does not
 * produce, so a test that drives it is driving the real decision.
 */
struct ChargerVerdict {
    uint8_t live_mask;        /* bit i set = charger_config[i] no longer holds */
    bool vreg_in_bounds;      /* REG06 is at or under the guard's ceiling      */
    bool config_still_live;   /* every written register still agrees           */
    bool configured;          /* the summary the endpoint leads with           */
};

static void charger_derive(ChargerVerdict &v, bool read_ok, bool boot_verified,
                           uint8_t reg04, uint8_t reg05, uint8_t reg06,
                           uint8_t reg07) {
    uint8_t live[CHARGER_CONFIG_COUNT];
    charger_live_readback(reg04, reg05, reg06, reg07, live);

    /* A pass that could not read is recorded as every register disagreeing
       rather than as none, so "we could not check" can never read as "we
       checked and it was fine" — the same rule charger_configure() applies to
       its own read-back. */
    v.live_mask         = read_ok ? charger_verify_mask(live) : CHARGER_MASK_ALL;
    v.vreg_in_bounds    = read_ok && bq25896_vreg_in_bounds(reg06);
    v.config_still_live = read_ok && (v.live_mask == 0);
    v.configured        = charger_config_is_live(boot_verified, read_ok,
                                                 v.live_mask, v.vreg_in_bounds);
}
/* host-test:end */

/* --- The bus --- */

/*
 * A single byte, and the ONLY way a byte reaches this part.
 *
 * The guard is inside this function rather than at its call sites, so that
 * adding a call site cannot add a way around it. A refused write returns false
 * without opening a transaction: nothing reaches the wire at all.
 *
 * Called from charger_write_table() and from nowhere else, which is walked by
 * charger_configure() from hw_probe() at boot and by charger_reapply() from
 * charger_refresh() when a pass finds the part no longer holding the table.
 * Both are the loop task; there is no third caller and no path here from a
 * request. See the I2C rule at the head of this file.
 */
static bool bq25896_write8(uint8_t reg, uint8_t val) {
    if (!bq25896_write_allowed(reg, val)) return false;
    Wire.beginTransmission(BQ25896_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

/* One register, one byte. Single-byte transactions throughout, never a
   multi-read: REG0C does not support one, and the few bytes saved by treating
   the other registers differently are not worth two code paths. */
static bool bq25896_read8(uint8_t reg, uint8_t &val) {
    Wire.beginTransmission(BQ25896_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ25896_ADDR, 1) != 1) return false;
    val = (uint8_t)Wire.read();
    return true;
}

/* --- The cache --- */

/*
 * Everything GET /charger says, filled from the loop task and served from
 * here. The endpoint never touches the bus; this struct is the whole of what
 * it is allowed to know.
 *
 * Three fields carry three different claims and they are deliberately not one
 * field, because the review this commit answers found them collapsed into one:
 *
 *   boot_verified      what happened once, at boot. A record, and it stays a
 *                      record — `writes[]` in the response is the same record.
 *   verdict            what the last 60 s pass concluded when it re-read the
 *                      same registers: the live mask, the voltage bound,
 *                      whether the configuration still holds and whether the
 *                      endpoint may say `configured`. Re-derived every pass,
 *                      as one struct out of charger_derive(), so that no part
 *                      of it can be stale while another part is current.
 */
struct ChargerState {
    bool present;             /* every read in the last pass succeeded        */
    bool write_attempted;     /* charger_configure() has run, however it went */
    bool boot_verified;       /* boot writes acknowledged AND read back right */
    ChargerVerdict verdict;   /* what the last pass concluded — one unit      */
    int  writes_ok;           /* how many of the boot writes were acknowledged */
    uint8_t verify_mask;      /* bit i set = charger_config[i] failed at boot */
    uint8_t verified[CHARGER_CONFIG_COUNT];  /* what each read back as        */
    bool verify_read_ok;      /* the boot read-back pass itself completed     */
    unsigned int reapplied;   /* times the table was re-issued after drift    */
    uint8_t reg00, reg04, reg05, reg06, reg07;
    uint8_t reg0b, reg0c, reg0c_first, reg13;
    unsigned long read_ms;    /* millis() at the end of the pass; 0 = never   */
};

static ChargerState charger_state;
static portMUX_TYPE charger_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Was the part answering at boot? Mirrors battery.cpp's `hw.has_battery` gate
 * and exists for the same reason: on a board that has no charger, or a bus
 * wedged low, every transaction costs a TwoWire timeout rather than a fast NAK,
 * and this file issues nine of them per pass where the gauge issues one. A
 * device that is not there at boot is not asked again.
 *
 * Written and read only from the loop task — charger_probe() and
 * charger_refresh() — so it needs no lock. It is not in ChargerState for that
 * reason: nothing the endpoint serves depends on it.
 */
static bool charger_seen_at_boot = false;

/* --- Reading --- */

/*
 * One pass over the charger, from the loop task and from nowhere else, for the
 * reason argued at the head of this file.
 *
 * The written registers are re-read on every pass rather than once at boot,
 * AND THEY ARE COMPARED ON EVERY PASS, which is the part that was missing. The
 * re-read alone was decorative: it filled `config` with decoded live values
 * while `configured`, `verified` and `writes[].matches` all went on serving the
 * verdict latched at boot, so a REG05 that had reverted to 0x13 produced a
 * response asserting the fix was live in its summary and denying it four lines
 * further down. The comparison runs here, on the pass that re-reads, against
 * the same table and through the same function the boot write used.
 *
 * `at_boot` is what allows a later pass to happen at all, exactly as
 * battery.cpp's is: a part that did not answer at boot is not on this board,
 * and nine timing-out transactions a minute forever is a cost with no reader.
 */
static void charger_read(bool at_boot) {
    if (!at_boot && !charger_seen_at_boot) return;

    ChargerState c;
    memset(&c, 0, sizeof(c));

    portENTER_CRITICAL(&charger_mux);
    c.write_attempted = charger_state.write_attempted;
    c.boot_verified   = charger_state.boot_verified;
    c.writes_ok       = charger_state.writes_ok;
    c.verify_mask     = charger_state.verify_mask;
    c.verify_read_ok  = charger_state.verify_read_ok;
    c.reapplied       = charger_state.reapplied;
    memcpy(c.verified, charger_state.verified, sizeof(c.verified));
    portEXIT_CRITICAL(&charger_mux);

    /*
     * Short-circuit rather than `ok &=`, and that is the other half of the
     * absent-hardware gate: on a wedged bus the first read is the one that
     * costs a timeout, and there is no value in paying for eight more to learn
     * the same thing. The pass is still recorded — with present false — so the
     * endpoint reports a charger that stopped answering instead of serving an
     * ageing snapshot that still says present.
     *
     * REG0C is read TWICE and the second one is the answer. The datasheet is
     * explicit that it "keeps all the fault information from last read until
     * the host issues a new read": the first read reports whatever latched
     * since we last looked, the second reports what is true now. Both are kept
     * — the first is a real sixty-second window and is worth showing — but only
     * the second is decoded, so a fault that came and went cannot be rendered
     * as a fault that is happening. Two separate single-byte transactions,
     * because this is the one register on the part that does not support
     * multi-read.
     */
    bool ok = bq25896_read8(BQ25896_REG00_INPUT,  c.reg00)
           && bq25896_read8(BQ25896_REG04_ICHG,   c.reg04)
           && bq25896_read8(BQ25896_REG05_TERM,   c.reg05)
           && bq25896_read8(BQ25896_REG06_VREG,   c.reg06)
           && bq25896_read8(BQ25896_REG07_TIMER,  c.reg07)
           && bq25896_read8(BQ25896_REG0B_STATUS, c.reg0b)
           && bq25896_read8(BQ25896_REG13_DPM,    c.reg13)
           && bq25896_read8(BQ25896_REG0C_FAULT,  c.reg0c_first)
           && bq25896_read8(BQ25896_REG0C_FAULT,  c.reg0c);

    /*
     * The comparison the boot write ran, run again on what the part is holding
     * now — and the whole verdict with it, in ONE call that copies nothing.
     *
     * What is covered and what is not, stated here rather than left to look
     * covered: charger_derive() and everything it calls are driven directly by
     * tools/test_charger.sh, so the decision this line delegates is proven. The
     * line itself is not, and cannot be — it sits between two I2C loops on the
     * far side of Wire and portMUX. What the shape buys is that there is
     * nothing left to get subtly wrong here: no field is computed twice, none
     * is copied out and reassigned, and removing the call leaves the verdict
     * zeroed by the memset above, which is `configured` false and a clean-
     * looking mask that no longer claims anything. The failure mode that
     * started all this — a stale `configured: true` over live values saying
     * otherwise — is not reachable from any edit to this statement.
     */
    charger_derive(c.verdict, ok, c.boot_verified,
                   c.reg04, c.reg05, c.reg06, c.reg07);

    c.present = ok;
    c.read_ms = millis();

    if (at_boot) charger_seen_at_boot = ok;

    portENTER_CRITICAL(&charger_mux);
    charger_state = c;
    portEXIT_CRITICAL(&charger_mux);
}

/*
 * The table, written out in order. One function, used by the boot write and by
 * the re-issue on drift alike, so that "what we send" has exactly one
 * definition and the recovery path cannot drift from the original.
 *
 * Every write goes through bq25896_write8() and therefore through the guard,
 * so a value edited into the table above that would overcharge the cell does
 * not reach the wire: it fails here and shows up as a short writes_ok in the
 * endpoint. That is the intended behaviour. A wrong table is a bug to be seen,
 * not an exception to be thrown on a device with no console.
 */
static int charger_write_table() {
    int ok_count = 0;
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        if (bq25896_write8(charger_config[i].reg, charger_config[i].val)) ok_count++;
    }
    return ok_count;
}

/*
 * The writes, once, at boot, from hw_probe() — then read back and compared.
 *
 * `boot_verified` requires all of them to have been acknowledged AND to read
 * back as what was sent. Anything less is not a partially applied
 * configuration, it is an unknown one, and nothing downstream should be told
 * the settings are live on the strength of it. It is not, on its own, what the
 * endpoint reports as `configured`: that is re-derived by every later pass —
 * see charger_config_is_live().
 *
 * `write_attempted` is set whatever happens, because it is what the watchdog
 * fault is judged against, and the case that matters there is precisely the
 * one where the write did NOT fully succeed.
 */
static void charger_configure() {
    int ok_count = charger_write_table();

    uint8_t readback[CHARGER_CONFIG_COUNT];
    memset(readback, 0, sizeof(readback));
    bool read_ok = true;
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        if (!bq25896_read8(charger_config[i].reg, readback[i])) read_ok = false;
    }

    /* A failed read-back is not a passed verification. Left as an all-ones
       mask rather than zero so that "we could not check" can never be
       mistaken for "we checked and it was fine". */
    uint8_t mask = read_ok ? charger_verify_mask(readback) : CHARGER_MASK_ALL;
    bool verified = (ok_count == CHARGER_CONFIG_COUNT) && (mask == 0);

    portENTER_CRITICAL(&charger_mux);
    charger_state.write_attempted = true;
    charger_state.writes_ok       = ok_count;
    charger_state.verify_mask     = mask;
    charger_state.verify_read_ok  = read_ok;
    memcpy(charger_state.verified, readback, sizeof(readback));
    charger_state.boot_verified   = verified;
    /* The verdict belongs to charger_derive() and to nothing else. Cleared
       here so that the window between this write and the read pass that
       follows it claims nothing at all. */
    memset(&charger_state.verdict, 0, sizeof(charger_state.verdict));
    portEXIT_CRITICAL(&charger_mux);

    Serial.printf("[charger] wrote %d/%d, verified %s\n",
                  ok_count, CHARGER_CONFIG_COUNT,
                  verified ? "yes" : "NO");
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        if (mask & (1u << i)) {
            Serial.printf("[charger] %s: wrote 0x%02X, read 0x%02X\n",
                          charger_config[i].name, charger_config[i].val,
                          read_ok ? readback[i] : 0);
        }
    }

    /* Serial is a console this device does not have, and the bitmask is a field
       in an endpoint nobody polls until something is already wrong. The event
       ring is where the rest of this firmware records what happened while
       nobody was watching, and a charger that did not take its configuration
       belongs in it beside the boot and the OTA lines. */
    if (!verified) {
        event_add("charger not configured: %d/%d written, verify mask 0x%02X",
                  ok_count, CHARGER_CONFIG_COUNT, mask);
    }
}

/*
 * Re-issue the table because the part is no longer holding it.
 *
 * This is what the watchdog used to do and no longer does — see the header. It
 * runs from the loop task, on the same 60 s tick as everything else on this
 * bus, and ONLY when the comparison in charger_read() found a disagreement. It
 * walks the table in order, so REG07 goes out first and closes the watchdog
 * window before the rest land, which matters most in exactly the case that
 * brought us here: a watchdog that expired and re-armed itself.
 *
 * Nothing verifies inline. The next pass, sixty seconds later, re-reads and
 * re-compares like any other pass, and either the disagreement is gone or it is
 * not. A part that refuses a register goes on being re-written once a minute
 * and goes on reporting `configured: false`; that is the correct resting
 * behaviour for a charger we cannot put into a known state, and it is bounded
 * by four writes a minute. The event is logged on the TRANSITION only, so a
 * permanent disagreement cannot flood the 64-entry ring and push out the rest
 * of the device's history.
 */
static void charger_reapply(uint8_t live_mask) {
    int ok_count = charger_write_table();

    portENTER_CRITICAL(&charger_mux);
    charger_state.reapplied++;
    portEXIT_CRITICAL(&charger_mux);

    Serial.printf("[charger] drifted (mask 0x%02X), rewrote %d/%d\n",
                  live_mask, ok_count, CHARGER_CONFIG_COUNT);
}

/* Called once from hw_probe(), after the I2C scan and before the display
   claims the SPI bus. The write comes first and the status read second, in one
   call, so that the very first GET /charger already reports live values rather
   than an empty cache. */
static void charger_probe() {
    charger_configure();
    charger_read(true);
}

/*
 * Called from loop() on the same 60 s tick as battery_refresh().
 *
 * Reads, compares, and writes ONLY when the comparison failed. This is NOT a
 * watchdog kick and must never become one — the header says why at length: the
 * watchdog's default is 40 s, this tick is 60 s, and feeding it from here would
 * let it expire and revert the configuration on roughly every cycle.
 */
static void charger_refresh() {
    charger_read(false);

    bool present;
    uint8_t live_mask;
    portENTER_CRITICAL(&charger_mux);
    present   = charger_state.present;
    live_mask = charger_state.verdict.live_mask;
    portEXIT_CRITICAL(&charger_mux);

    /* A pass that could not read is not evidence of drift — live_mask is all
       ones precisely because nothing was seen — and writing to a part that just
       failed nine reads would only add transactions to a bus already in
       trouble. */
    if (!present) return;

    static bool drifted = false;
    if (live_mask != 0) {
        if (!drifted) {
            drifted = true;
            event_add("charger drifted: mask 0x%02X, rewriting", live_mask);
        }
        charger_reapply(live_mask);
    } else if (drifted) {
        drifted = false;
        event_add("charger configuration restored");
    }
}

/* --- Endpoints --- */

static const SkillEndpoint charger_endpoints[] = {
    {"GET", "/charger", "Charge state, faults, and the boot configuration "
                        "read back from the BQ25896"},
    {NULL, NULL, NULL}
};

static const char *charger_describe() {
    return "## Skill: charger\n\n"
           "A BQ25896 charger at I2C 0x6B (SDA=8, SCL=18), on the same bus as\n"
           "the BQ27220 fuel gauge that the **battery** skill reads. This skill\n"
           "**configures the charger at boot** — four register writes, listed\n"
           "below — verifies them by reading them back, and then re-reads and\n"
           "**re-compares** them every 60 s, re-issuing the whole table if the\n"
           "part is ever found holding something else.\n\n"
           "For cell voltage, state of charge and capacities, see `GET\n"
           "/battery`. That skill writes six parameters to the gauge's data\n"
           "memory, three of which are deliberately the same numbers this one\n"
           "writes to the charger — the gauge only recognises a finished charge\n"
           "if it is told what this part does. In particular its TaperCurrent\n"
           "must equal REG05's termination current below; change one and the\n"
           "other has to move with it.\n\n"
           "The two are separate skills because they are separate parts with\n"
           "separate risks: this one can drive 4.6 V into a lithium cell, and\n"
           "the gauge has no charge control at all.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /charger | The configuration read back, charge state, "
           "faults |\n\n"
           "### Why it exists\n\n"
           "The firmware had never written to the charger, so it ran on\n"
           "power-on defaults, and on those defaults this cell never reached\n"
           "full: charging terminated at 4139 mV holding 1004 of 1300 mAh —\n"
           "296 mAh short, 77%. The cause is REG05, whose default puts the\n"
           "termination current at 256 mA. On a 1300 mAh cell that is C/5,\n"
           "where a lithium cell wants C/10 to C/20, so charge is cut while the\n"
           "CV taper is still well above the real end point.\n\n"
           "### What is written at boot\n\n"
           "Four registers, in this order, from the boot probe. No other\n"
           "register is ever written.\n\n"
           "| Reg | Value | Meaning |\n"
           "|-----|-------|---------|\n"
           "| REG07 | 0x8D | termination on, **watchdog disabled**, safety "
           "timer on at 12 h |\n"
           "| REG04 | 0x08 | charge current 512 mA (0.39 C) |\n"
           "| REG05 | 0x10 | precharge 128 mA, **termination 64 mA (C/20)** — "
           "the fix |\n"
           "| REG06 | 0x5E | charge voltage 4.208 V, BATLOWV 3.0 V, VRECHG "
           "100 mV |\n\n"
           "Three of the four match LilyGO's own production firmware for this\n"
           "board. **REG05 deliberately does not.** The vendor writes 0x11,\n"
           "terminating at 128 mA, and so did this firmware until the gauge\n"
           "made the cost visible: the BQ27220 declares a pack full only after\n"
           "the current spends eighty seconds *between* a floor and its own\n"
           "taper threshold, and that threshold was also 128 mA. A gauge\n"
           "ceiling equal to the charger's cut-off is a band the charger never\n"
           "operates in — above it the charger is still delivering, below it it\n"
           "has stopped. Terminating at 64 mA leaves room for the gauge's\n"
           "threshold to sit above the cut-off, which is what `GET /battery`\n"
           "now reports as a taper of 100 mA. Matching the vendor here would\n"
           "mean keeping their latent bug.\n\n"
           "REG07 is first so that the watchdog window is closed before the\n"
           "rest land. The watchdog is **disabled rather than kicked**: its\n"
           "default is 40 s, on expiry the part resets its registers to\n"
           "defaults and restores the 256 mA bug, and this firmware's battery\n"
           "tick is 60 s — longer than 40. Kicking it from that tick would let\n"
           "it expire and revert on roughly every cycle. Disabling it is one\n"
           "write and no periodic task, and the state it leaves the part in\n"
           "(512 mA) is safer than the defaults the watchdog exists to fall\n"
           "back to (2048 mA).\n\n"
           "**What replaces the watchdog** is the comparison below. Disabling\n"
           "it removed the part's only self-correction, so the 60 s pass that\n"
           "re-reads the written registers also compares them, and re-issues\n"
           "the whole table — REG07 first — whenever they disagree. `reapplied`\n"
           "counts how many times that has happened.\n\n"
           "**REG06, the charge voltage, is written**, and it did not use to\n"
           "be. The old argument was that 4.208 V is what the part powers up\n"
           "holding, so the way to arrive there is to touch nothing. This\n"
           "device disproves the premise: its REG00 reads 0x3F, where the\n"
           "power-on value is 0x48, and that difference can only come from a\n"
           "host write — the charger is battery-powered, so registers written\n"
           "by other software survive every reflash of the ESP32. A charge\n"
           "voltage left at 4.32 V by something else would be read, reported\n"
           "and charged at. So it is written to the value we want anyway,\n"
           "verified like the rest, and bounded on the read side as well.\n\n"
           "### Verified, not assumed — and re-verified every pass\n\n"
           "Every written register is read back and compared at boot **and\n"
           "re-compared against the same table on every 60 s pass**, so this\n"
           "endpoint reports whether the configuration is live now, not whether\n"
           "it was once sent.\n\n"
           "`configured` is the summary, and it is true only when the boot\n"
           "writes were acknowledged and verified, **and** the latest pass\n"
           "re-read every written register and found it unchanged, **and** the\n"
           "charge voltage is within bounds. `verified` is that latest\n"
           "comparison on its own; `verified_at_boot` is the boot record.\n"
           "`writes_ok` of `writes_expected` counts acknowledgements. Per\n"
           "register, `writes[]` gives `.wrote`, `.readback` and `.matches`\n"
           "from boot, and `.live` and `.still_matches` from the latest pass. A\n"
           "comparison that could not be performed counts as unverified, never\n"
           "as verified.\n\n"
           "`config` also decodes what the registers currently hold —\n"
           "`charge_voltage_mv`, `charge_current_ma`, `termination_current_ma`,\n"
           "`precharge_current_ma`, `termination_enabled`, `watchdog_disabled`,\n"
           "`safety_timer_enabled` — as of `age_s` seconds ago. If the\n"
           "configuration were ever undone, `termination_current_ma` would read\n"
           "256 again here, `configured` would be false with it, and the table\n"
           "would be re-issued on that same pass.\n\n"
           "### Input current, reported and never written\n\n"
           "`input_current_limit_ma` is REG00's IINLIM and `en_ilim` is\n"
           "REG00[6]; the whole register is refused by the guard, EN_HIZ\n"
           "included. **Input current was not why the cell charged short** —\n"
           "an earlier version of this text said it was half the explanation\n"
           "and that was wrong. On this device REG00 reads 0x3F (EN_ILIM clear,\n"
           "IINLIM 3250 mA) and under a live charge REG13 reports an in-force\n"
           "input limit of 1050 mA with `in_iindpm` false, against a 512 mA\n"
           "charge current. The input was never the constraint. `en_ilim` still\n"
           "matters to reading the number: while it is **set** the actual limit\n"
           "is the lower of this register and the ILIM pin resistor. Here it is\n"
           "**clear**, so the register is the limit — a decision about this\n"
           "board's hardware current limit that was made by whoever wrote REG00\n"
           "before this firmware existed, and one this skill reports rather\n"
           "than makes. `input_current_dpm_ma` from REG13 is the limit actually\n"
           "in force.\n\n"
           "### Charge state and faults\n\n"
           "`status.charge_state` is REG0B CHRG_STAT and is what answers \"why\n"
           "did it stop\": `not_charging`, `pre_charge`, `fast_charging`,\n"
           "`termination_done`. Note that it **does not distinguish constant\n"
           "current from constant voltage** — both are `fast_charging` — so it\n"
           "says whether charging finished, not how far along it is. Beside it:\n"
           "`vbus` (`none` / `usb_host_sdp` / `adapter` / `otg` / `unknown` —\n"
           "this part documents only those four codes), `power_good`,\n"
           "`vsys_regulating`, and from REG13 `in_vindpm` and `in_iindpm`.\n\n"
           "`fault` decodes REG0C: `charge` (`normal` / `input` /\n"
           "`thermal_shutdown` / `safety_timer_expired`), `ntc` (`normal` /\n"
           "`ts_warm` / `ts_cool` / `ts_cold` / `ts_hot` / `unknown`),\n"
           "`battery_ovp`, `boost`, `watchdog`. REG0C latches faults from the\n"
           "last read, so it is read **twice** and only the second is decoded;\n"
           "the first is reported as `reg0c_since_last_read`, a real\n"
           "60-second window. `watchdog` is suppressed until the boot write has\n"
           "been **attempted**, because the bit reads 1 in default mode by\n"
           "definition and would otherwise flag an unconfigured part as faulty.\n"
           "It is gated on the attempt rather than on success on purpose: a\n"
           "failed REG07 write leaves the watchdog armed, which is the one case\n"
           "the flag exists for. `watchdog_bit` is the raw bit regardless.\n\n"
           "### What is NOT reported\n\n"
           "The charger's ADC. VBAT, VSYS, VBUS, charge current and TS\n"
           "percentage (REG0E-REG12) all read zero at power-on because REG02's\n"
           "CONV_START and CONV_RATE are both clear, and reading them honestly\n"
           "would mean either polling a one-shot conversion on the loop task or\n"
           "leaving continuous conversion running, which costs standing current\n"
           "on a battery device. The cell voltage comes from the gauge instead,\n"
           "via `GET /battery`. **There is no charge-current reading in this\n"
           "response.**\n\n"
           "### Freshness\n\n"
           "Every register is read on one 60-second tick in the main loop, and\n"
           "this endpoint serves that cache rather than touching the bus\n"
           "itself: the loop task is the only task allowed on the I2C bus,\n"
           "because two interleaved transactions on one bus would be a fault\n"
           "with no witness. `age_s` is how many seconds ago the pass ran, and\n"
           "`fresh` is false when nothing has been read yet. A charger that did\n"
           "not answer at boot is not polled again, the same way a missing fuel\n"
           "gauge is not re-probed by `GET /battery`.\n\n"
           "### Safety\n\n"
           "Writes go through one guard, which is an allowlist of registers\n"
           "first and a bounds check second. Only REG04, REG05, REG06 and REG07\n"
           "are writable at all; REG08 (IR compensation, a second overcharge\n"
           "path that bypasses a correct VREG), REG09 (BATFET_DIS ship mode),\n"
           "REG00 (EN_HIZ) and REG03 (OTG_CONFIG) are refused as registers, so\n"
           "no combination of their bits can get through. REG06 is bounded at\n"
           "4.208 V — `charge_voltage_max_mv` — and REG04 at 576 mA against a\n"
           "board the vendor caps at 600 mA. The REG06 bound is applied to what\n"
           "the part is **found holding** as well as to what is written:\n"
           "`charge_voltage_in_bounds` is false if the register ever reads\n"
           "above the ceiling, and `configured` cannot be true while it is.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" http://seed.local:8080/charger\n"
           "```\n";
}

static void charger_register_routes(AsyncWebServer &server) {

    /* GET /charger */
    server.on(AsyncURIMatcher::exact("/charger"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        /* One copy of the cache under the lock, reported outside it: building
           the JSON allocates and must not happen in a critical section. And
           note what is NOT here — a bus transaction. See the I2C rule at the
           head of this file. */
        ChargerState c;
        portENTER_CRITICAL(&charger_mux);
        c = charger_state;
        portEXIT_CRITICAL(&charger_mux);

        /* Sampled AFTER the copy, and that ordering is the whole of the
           correctness of age_s: taken before it, this task could read the
           clock, be preempted, and come back to a snapshot the loop task
           stamped later — now - read_ms would then borrow on unsigned long and
           report 4294967 seconds. */
        unsigned long now = millis();

        JsonDocument doc;
        doc["part"] = "BQ25896";
        doc["address"] = "0x6B";
        doc["present"] = c.present;
        /* The summary a caller reads first, and therefore the one that has to
           be re-derived rather than remembered: it is false the moment the
           registers stop agreeing with the table or the charge voltage leaves
           its bound, not merely when the boot write failed. */
        doc["configured"] = c.verdict.configured;
        doc["verified"] = c.verdict.config_still_live;
        doc["verified_at_boot"] = (c.verify_read_ok && c.verify_mask == 0);
        doc["charge_voltage_in_bounds"] = c.verdict.vreg_in_bounds;
        doc["writes_ok"] = c.writes_ok;
        doc["writes_expected"] = CHARGER_CONFIG_COUNT;
        doc["reapplied"] = c.reapplied;
        doc["fresh"] = (c.read_ms != 0);
        if (c.read_ms != 0) doc["age_s"] = (unsigned long)(now - c.read_ms) / 1000;

        char hex[8];

        /* Pure arithmetic over the bytes already copied out of the cache — the
           same mapping the loop task used to build the mask, so the per-register
           answers below cannot disagree with the summary above. No bus. */
        uint8_t live[CHARGER_CONFIG_COUNT];
        charger_live_readback(c.reg04, c.reg05, c.reg06, c.reg07, live);

        /* What was written, what it read back as at boot, and — the part that
           was missing — what it holds NOW. Per register, so a single
           disagreement names itself instead of collapsing into one false flag,
           and so that `matches` can go on meaning what it always meant (the
           boot read-back) without that being mistaken for a claim about the
           present. */
        JsonArray writes = doc["writes"].to<JsonArray>();
        for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
            JsonObject w = writes.add<JsonObject>();
            w["reg"] = charger_config[i].name;
            snprintf(hex, sizeof(hex), "0x%02X", charger_config[i].val);
            w["wrote"] = String(hex);
            if (c.verify_read_ok) {
                snprintf(hex, sizeof(hex), "0x%02X", c.verified[i]);
                w["readback"] = String(hex);
            }
            w["matches"] = c.verify_read_ok && ((c.verify_mask & (1u << i)) == 0);
            if (c.present) {
                snprintf(hex, sizeof(hex), "0x%02X", live[i]);
                w["live"] = String(hex);
            }
            w["still_matches"] = c.present && ((c.verdict.live_mask & (1u << i)) == 0);
            w["why"] = charger_config[i].why;
        }

        if (c.present) {
            /*
             * Decoded from the last status pass, not from what was sent. This
             * is what makes the response evidence that the configuration is
             * live rather than a restatement of intent: a watchdog reset would
             * show up here as REG05 back at 0x13.
             */
            JsonObject cfg = doc["config"].to<JsonObject>();

            snprintf(hex, sizeof(hex), "0x%02X", c.reg05);
            cfg["reg05"] = String(hex);
            cfg["termination_current_ma"] = bq25896_iterm_ma(c.reg05);
            cfg["precharge_current_ma"] = bq25896_iprechg_ma(c.reg05);

            snprintf(hex, sizeof(hex), "0x%02X", c.reg04);
            cfg["reg04"] = String(hex);
            cfg["charge_current_ma"] = bq25896_ichg_ma(c.reg04);

            snprintf(hex, sizeof(hex), "0x%02X", c.reg07);
            cfg["reg07"] = String(hex);
            cfg["termination_enabled"] = (c.reg07 & 0x80u) != 0;
            cfg["watchdog_disabled"] = ((c.reg07 >> 4) & 0x3u) == 0;
            cfg["safety_timer_enabled"] = (c.reg07 & 0x08u) != 0;

            /* The voltage this board drives into the cell. Written, verified
               and re-issued like the rest — and bounded HERE as well as at the
               write, because a write that was refused or NAKed leaves whatever
               another host put there. `charge_voltage_in_bounds` is false if
               that ever happens, and `configured` above is false with it. */
            snprintf(hex, sizeof(hex), "0x%02X", c.reg06);
            cfg["reg06"] = String(hex);
            cfg["charge_voltage_mv"] = bq25896_vreg_mv(c.reg06);
            cfg["charge_voltage_in_bounds"] = bq25896_vreg_in_bounds(c.reg06);
            cfg["charge_voltage_max_mv"] =
                BQ25896_VREG_BASE_MV + BQ25896_VREG_STEP_MV * BQ25896_VREG_CODE_MAX;
            cfg["charge_voltage_written"] = true;
            cfg["batlowv_mv"] = (c.reg06 & 0x02u) ? 3000 : 2800;
            cfg["vrecharge_mv"] = (c.reg06 & 0x01u) ? 200 : 100;

            snprintf(hex, sizeof(hex), "0x%02X", c.reg00);
            cfg["reg00"] = String(hex);
            cfg["input_current_limit_ma"] = bq25896_iinlim_ma(c.reg00);
            /* Without this the number above reads as an answer when it is only
               half of one: with EN_ILIM set, the pin resistor can be lower and
               wins. */
            cfg["en_ilim"] = bq25896_ilim_pin_enabled(c.reg00);
            cfg["input_current_written"] = false;

            JsonObject st = doc["status"].to<JsonObject>();
            snprintf(hex, sizeof(hex), "0x%02X", c.reg0b);
            st["reg0b"] = String(hex);
            st["charge_state"] = bq25896_chrg_stat_name(c.reg0b);
            st["charge_state_bits"] = bq25896_chrg_stat(c.reg0b);
            st["vbus"] = bq25896_vbus_stat_name(c.reg0b);
            st["vbus_bits"] = bq25896_vbus_stat(c.reg0b);
            st["power_good"] = bq25896_power_good(c.reg0b);
            st["vsys_regulating"] = bq25896_vsys_regulating(c.reg0b);

            snprintf(hex, sizeof(hex), "0x%02X", c.reg13);
            st["reg13"] = String(hex);
            st["in_vindpm"] = bq25896_in_vindpm(c.reg13);
            st["in_iindpm"] = bq25896_in_iindpm(c.reg13);
            st["input_current_dpm_ma"] = bq25896_iinlim_ma(c.reg13);

            JsonObject flt = doc["fault"].to<JsonObject>();
            snprintf(hex, sizeof(hex), "0x%02X", c.reg0c);
            flt["reg0c"] = String(hex);
            snprintf(hex, sizeof(hex), "0x%02X", c.reg0c_first);
            flt["reg0c_since_last_read"] = String(hex);
            flt["charge"] = bq25896_chrg_fault_name(c.reg0c);
            flt["ntc"] = bq25896_ntc_fault_name(c.reg0c);
            flt["battery_ovp"] = (c.reg0c & BQ25896_FAULT_BAT_OVP) != 0;
            flt["boost"] = (c.reg0c & BQ25896_FAULT_BOOST) != 0;
            /* The bit reads 1 in default mode by definition, so before the boot
               write it describes the situation rather than a fault. Gated on
               the write having been ATTEMPTED, not on its having succeeded: a
               failed REG07 write leaves the watchdog armed, and that is the one
               case this flag exists to show. The raw bit is alongside it. */
            flt["watchdog"] = bq25896_watchdog_fault_is_real(c.reg0c, c.write_attempted);
            flt["watchdog_bit"] = (c.reg0c & BQ25896_FAULT_WATCHDOG) != 0;
        }

        /* Asked for often enough that guessing at its absence would be its own
           support burden. */
        doc["adc"] = "not read: the charger ADC is off at power-on (REG02 "
                     "CONV_START and CONV_RATE both clear), so VBAT/VSYS/VBUS/"
                     "ICHG would all read zero. Cell voltage comes from the "
                     "fuel gauge instead — see GET /battery.";

        notify_send_json(req, 200, doc);
    });
}

static const Skill charger_skill = {
    .name = "charger",
    /* 0.2.0 rather than 0.1.1: the response is unchanged in shape, but the part
       is charged differently. REG05 goes out as 0x10 instead of 0x11, so
       termination_current_ma reads 64 where it read 128, and a caller pinned to
       the old figure is looking at a real behaviour change rather than a
       cosmetic one. */
    .version = "0.2.0",
    .describe = charger_describe,
    .endpoints = charger_endpoints,
    .register_routes = charger_register_routes
};

static void skill_charger_init() {
    /* Nothing to bring up: charger_probe() has already run from hw_probe(),
       which is where it has to run — the charger should be off its power-on
       defaults from the first second the device is powered, not from whenever
       skills come up. */
    skill_register(&charger_skill);
}
