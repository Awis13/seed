/*
 * skills/battery.cpp — the BQ27220 fuel gauge, read out and then told what the
 * charger sitting next to it actually does
 *
 * Endpoints:
 *   GET /battery — every register this firmware reads, raw and decoded, and
 *                  the six data-memory parameters it writes at boot
 *
 * THE PROMISE AT THE HEAD OF THIS FILE HAS CHANGED, AND SAYING SO IS THE POINT
 * ---------------------------------------------------------------------------
 * Until this commit this header argued, at length, that the skill wrote NOTHING
 * to the gauge — "not an unseal, not a CONFIG UPDATE, not a chemistry profile,
 * not a design capacity" — and that "the restraint is the point". It was, and
 * the reason it gave was a good one: correcting a gauge means rewriting data
 * memory on the owner's live pager, and "the correct value to write is exactly
 * what nobody knows yet". So the file read the gauge out and left the fix to
 * whoever could aim at a number instead of at a theory.
 *
 * The numbers arrived, off this endpoint, and they are below. This file now
 * writes six of them. Leaving the old argument in place above code that
 * contradicts it would make the file lie about itself, so it is replaced rather
 * than qualified — the same thing charger.cpp did when its own "write nothing"
 * argument stopped being true.
 *
 * What has NOT changed: every register named under "The registers" below is
 * still read and never written, the endpoint still serves a cache and never
 * touches the bus, and the six writes are the only writes. They go out once, at
 * boot, through a guard, and are then verified rather than assumed.
 *
 * What the readings said
 * ----------------------
 * Earlier commits on this branch made the gauge readable and then fixed the
 * charger, which had been terminating a charge at 256 mA and leaving the cell
 * 296 mAh short. Measured on this device after that fix, on a charge the
 * charger itself declared complete:
 *
 *   charge state          termination_done, 4.2 V, tapered down to 128 mA
 *   remaining             1035 mAh
 *   full_charge_capacity  1300 mAh   <- what the gauge divides by
 *   design_capacity       1300 mAh
 *   battery_status.fc     false      <- on a cell that IS full
 *   operation_status.vdq  false      <- no discharge has ever qualified
 *
 * A textbook complete charge, and a genuinely full cell reading 80%.
 *
 * The gauge is dividing a believable RemainingCapacity by the DESIGN capacity,
 * because it has never learned this pack's true FullChargeCapacity. And it
 * cannot learn one, because FCC is only updated across a charge the gauge
 * believes finished and a discharge it is willing to trust — and it has never
 * once noticed that a charge finished. FC is false on a full cell. That is the
 * whole defect, and every number above is downstream of it.
 *
 * Which is not a fault in the gauge. It declares a pack full when it observes
 * the charge current taper below ITS OWN taper threshold while the cell is above
 * ITS OWN charge-termination voltage (TRM SLUUBD4A section 4.9.47). Both of
 * those live in its data memory, both are still at the ROM defaults, and neither
 * has ever been told what the BQ25896 six millimetres away is doing. The
 * condition it waits for never arrives.
 *
 * So the fix is not to correct a capacity — writing a FullChargeCapacity would
 * be inventing the very number the gauge is supposed to measure. It is to
 * describe the charger to the gauge and let it find the capacity itself.
 *
 * WHAT IS WRITTEN, AND WHERE THE VALUES CAME FROM
 * ----------------------------------------------
 * Six data-memory parameters. Two of them are the charger's own settings,
 * repeated to the gauge so that the two parts agree about what a charge in
 * progress looks like. The third is the one that must NOT be a copy:
 *
 *   0x91FB  Charging Current       512 mA   = the charger's ICHG   (REG04)
 *   0x91FD  Charging Voltage      4208 mV   = the charger's VREG   (REG06)
 *   0x9201  Taper Current          100 mA   ABOVE the charger's ITERM (REG05,
 *                                           64 mA) — the one that is NOT a copy
 *   0x92A5  CEDV Charge Termination Voltage  100 mV
 *   0x922A  Charge Detection Threshold        75 mA
 *   0x922C  Quit Current                      40 mA
 *
 * 0x9201 WAS CLAIMED HERE TO BE "THE ONE THAT MATTERS", on the argument that
 * the charger stops at 128 mA while the gauge watched for its 100 mA default,
 * so a taper that stops at 128 never crosses 100. THAT ARGUMENT WAS RIGHT ABOUT
 * THE MECHANISM AND WRONG ABOUT THE DIRECTION, and the device said so: with
 * 0x9201 = 128 applied and verified, the next completed charge still ended with
 * fc false.
 *
 * The correction is one sentence: TRM 4.4.1 condition 2 is a floor on the
 * current, so termination needs the current INSIDE a band, and setting Taper
 * Current equal to the charger's termination current makes that band the one
 * range the charger refuses to operate in. The number that satisfies the
 * condition is a number ABOVE the charger's ITERM, not equal to it.
 *
 * THAT IS WHAT THIS VERSION WRITES, and it is the first change to the table
 * since 0.2.0. Three plausible theories were acted on before being measured;
 * the version before this one added Current(), AverageCurrent() and
 * GaugingStatus and changed nothing, so that a real charge profile could be
 * watched. It was. Across one full cycle, sampled once a minute: ZERO samples
 * between the floor and the ceiling, fc never set, 1259 mAh delivered into a
 * cell that finished full and read 97%, and a CV-phase decay of about
 * 12 mA/min. The band was empty exactly as predicted.
 *
 * So two numbers moved, in two files, and they moved APART:
 *
 *   charger.cpp REG05  0x11 -> 0x10   termination 128 mA -> 64 mA (C/20)
 *   0x9201 Taper Current  128 -> 100  now 1.56x the charger's threshold
 *
 * The charger had to move first: while it terminated at 128 mA, "above the
 * charger's ITERM" and TI's "below C/10" (130 mA on this cell) left a window
 * two milliamps wide. Dropping the charger to 64 mA is what made room. The
 * reasoning behind 100 rather than 90 is on the table entry itself, because
 * that is where somebody will be standing when they consider changing it.
 *
 * SAID PLAINLY, BECAUSE THE TABLE BELOW FLATTERS THIS COMMIT OTHERWISE: 100 mA
 * is also Taper Current's own ROM default. Against a factory-default gauge the
 * 0x9201 write changes nothing, and the behaviour that actually changes is the
 * charger's. The gauge write earns its place by PINNING the value rather than
 * inheriting it — LilyGO's firmware writes 128 here, so a part holding the
 * wrong number is the expected case — but it is not the half doing the work.
 *
 * 0x92A5 is not an absolute voltage despite its name — the TRM calls it the
 * taper voltage in prose, and it is a delta BELOW Charging Voltage: termination
 * needs Voltage() > 4208 - 100 = 4108 mV. The other two bound what the gauge
 * treats as a charge at all rather than as noise on the shunt.
 *
 * Every address was read out of the TRM's own data memory table (SLUUBD4A
 * Table 3-2) and then found to agree, address for address, with LilyGO's
 * production firmware for this exact board — examples/test_battery_bq27220_
 * bq25896, whose verifyGaugeChargeParameters() writes these same six
 * parameters. The addresses are checked against the primary source rather than
 * inherited, because a wrong data-memory address does not fail, it writes a
 * different parameter.
 *
 * FIVE OF THE SIX VALUES ARE THEIRS. THE TAPER CURRENT IS NOT, and this is the
 * one place the vendor is deliberately not followed: they write 128 mA there,
 * equal to the 128 mA their charger configuration terminates at, and that
 * equality is exactly the defect described above. Matching them on that row
 * would mean keeping it.
 *
 * One address in LilyGO's table IS wrong, recorded here so nobody copies it in
 * later: their GasGaugingCEDVProfile1EMF is 0x92A3, which the TRM gives as
 * Design Voltage. EMF is at 0x92A7. The bug comes from Flipper Zero's driver
 * and was copied downstream. None of the six above is affected, and the guard
 * refuses 0x92A3 along with every other address not in the table.
 *
 * How the write goes out
 * ----------------------
 * Data memory is not a register. One parameter is a block transfer:
 *
 *   1. UNSEAL if the part is sealed — 0x0414 then 0x3672 to Control(), both
 *      fixed in ROM. (TRM section 3.3 says the key is 0x8000 0x8000; that is a
 *      documentation error. Section 6.1 of the same document gives 0x0414 and
 *      0x3672, and those are what the shipping vendor driver uses and what this
 *      silicon answers to.)
 *   2. FULL ACCESS — 0xFFFF twice. Section 6.1: "The BQ27220 boots up in
 *      UNSEAL, but not in FULL ACCESS. Enter FULL ACCESS to gain access to the
 *      Data Memory." LilyGO's comment that this step "is totally unnecessary"
 *      is not followed.
 *   3. ENTER_CFG_UPDATE (0x0090), then wait for OperationStatus[CFGUPDATE].
 *   4. Per parameter: address little-endian and value BIG-endian to 0x3E, then
 *      checksum and length to 0x60 as one two-byte write.
 *   5. EXIT_CFG_UPDATE_REINIT — 0x0091, WITH the re-initialisation. 0x0092
 *      also exits and applies NOTHING, silently, which is the single easiest
 *      way to write this whole file and have it do nothing at all.
 *
 * The endianness is mixed and it is not a typo: the ADDRESS goes out low byte
 * first (TRM 6.1 writes 0x9F to 0x3E and 0x92 to 0x3F for address 0x929F) and
 * the VALUE goes out high byte first (the same section writes 0x04 then 0xB0
 * for 1200). Getting either backwards writes a plausible wrong number to a
 * plausible wrong place.
 *
 * THE CHECKSUM, AND THE VENDOR BUG NOT INHERITED HERE
 * --------------------------------------------------
 * checksum = 0xFF - (sum of the two address bytes and the value bytes), and
 * MACDataLen = 2 address + N value + 1 checksum + 1 length. The TRM's own
 * worked example is the proof and is a test case below: address 0x9221 with one
 * data byte 0x00 gives 0xFF - (0x21+0x92+0x00) = 0x4C and a length of 5.
 *
 * ⚠️ THAT EXAMPLE HAS A TYPO IN IT AND THE CODE IS RIGHT WHERE THEY DIFFER, so
 * it is recorded here rather than left for somebody to "correct". TRM section
 * 4.6 prints the pair as "Write (hex) 4C 05, starting at 0x61". The checksum
 * 0x4C belongs to MACDataSum, and section 2.30 puts MACDataSum at 0x60 with
 * MACDataLen at 0x61 (Table 2-1 lists both). Starting the pair at 0x61 would
 * write the checksum into the length register and the length into 0x62. The
 * arithmetic in that example is sound and is the oracle this file uses; only
 * the start address is wrong, and this file writes the pair from 0x60.
 *
 * LilyGO's driver computes both correctly and then transfers the wrong number
 * of bytes: `i2cWriteBytes(CommandMACDataSum, buffer, size + 2)`, where the
 * length must be 2. For a two-byte parameter that pushes two stale value bytes
 * past MACDataLen into 0x62 and 0x63. Flipper Zero's driver, which LilyGO's is
 * derived from, writes 2. This file writes 2.
 *
 * THIS IS APPLY-AT-BOOT, NOT CALIBRATION
 * --------------------------------------
 * Data memory here is RAM. TRM section 8.1.4: these updates "need to be
 * re-programmed any time the device loses power". Making them permanent means
 * burning OTP, which needs 7.4 V applied to GPOUT from a bench supply and TI's
 * own tooling — it cannot happen from a 3.3 V I2C session and is not attempted.
 *
 * So the configuration is applied at boot, every boot, exactly as charger.cpp
 * applies its four registers and for exactly the same reason. In practice the
 * gauge is powered by the cell and keeps its RAM across a reflash, so the full
 * write sequence runs on the first boot after this firmware and then finds
 * everything already in place and skips it — see gauge_configure().
 *
 * SAFETY, WHICH IS A GENUINELY SMALLER QUESTION HERE THAN ON THE CHARGER
 * ---------------------------------------------------------------------
 * The BQ27220 has no FET drivers and no charge control. It cannot start, stop,
 * or alter a charge; it watches one. A wrong value in this table produces wrong
 * READINGS, never a wrong charge, which is a different category of harm from
 * the one bq25896_write_allowed() exists to prevent — that guard stands between
 * a typo and 4.6 V into a lithium cell, and this one does not.
 *
 * It is still a guard, built the same way: an allowlist of the six addresses,
 * each with the TRM's own range, and `default: refuse`. Nothing outside the
 * table reaches the wire. The reason is not fire, it is that a data-memory
 * address is four hex digits with no structure and no feedback — 0x929F is
 * DesignCapacity and 0x92A3 is Design Voltage and both are one keystroke from
 * addresses in this table, and a write to either would corrupt the gauge's
 * model of the pack while looking exactly like a success.
 *
 * Nothing here is irreversible: every value is volatile, a power cycle restores
 * the ROM defaults, and the unseal keys are fixed in ROM so the part cannot be
 * locked out. The one irreversible path on this device — OTP — requires an
 * externally applied 7.4 V and is physically unreachable from here.
 *
 * The part is left in FULL ACCESS rather than re-sealed. It arrived unsealed,
 * sealing it would be a state change nobody asked for, and the 60 s read-back
 * that keeps this endpoint honest needs data-memory access to work.
 *
 * The registers
 * -------------
 * Ten are STANDARD COMMANDS: write the command code, read two bytes,
 * little-endian. No sealing gets in the way of any of them, which is why the
 * whole diagnostic is available without touching the security state.
 *
 *   0x00 Control              reads CONTROL_STATUS, decoded below
 *   0x08 Voltage              mV
 *   0x0A BatteryStatus        flags, decoded below
 *   0x0C Current              mA, SIGNED
 *   0x10 RemainingCapacity    mAh
 *   0x12 FullChargeCapacity   mAh
 *   0x14 AverageCurrent       mA, SIGNED
 *   0x2C RelativeStateOfCharge  %
 *   0x3A OperationStatus      flags, decoded below
 *   0x3C DesignCapacity       mAh
 *
 * One is not a register at all. GaugingStatus has no command code: it is MAC
 * subcommand 0x0056, written to 0x3E and read back as a four-byte frame whose
 * first two bytes echo the subcommand. See the MAC SUBCOMMAND READS section.
 *
 * Voltage and RelativeStateOfCharge are the two the clock face uses; everything
 * else is diagnostic and goes nowhere near it. All of them are read and none is
 * written — the six writes are to data memory, which is a separate space
 * reached through a different mechanism entirely, and no command above is
 * touched by any of them.
 *
 * WHY THE LAST FOUR WERE ADDED, WHICH IS A DIAGNOSIS AND NOT A FIX
 * ---------------------------------------------------------------
 * The commit above this one wrote the six parameters and predicted that FC
 * would set on the next completed charge. It did not. Measured after it, on a
 * charge the charger declared complete: 1256 mAh, 4171 mV, soc 97, fc false,
 * vdq false, and all six parameters reading back exactly as written.
 *
 * Two theories were worth money and both are now closed by measurement rather
 * than by argument:
 *
 * 1. "We are reading the wrong FC." GaugingStatus has its own FC and its own
 *    TC. It is not a second opinion — TRM Table 2-4 says of each bit that it
 *    "is identical to" its BatteryStatus counterpart. Reading it settles the
 *    question rather than answering it, which is worth doing once and is why
 *    it is here.
 *
 * 2. "The gauge cannot see current at all." It can. The board schematic
 *    (T-Embed-CC1101 V1.0 24-07-29, page 1) fits R36 = 0.01R in series between
 *    the charger's BAT net and the cell, with SRP and SRN across it. The band
 *    that now matters runs from the charger's 64 mA termination up to the
 *    gauge's 100 mA taper, which across 0.01R develops 0.64 to 1.00 mV — small,
 *    but a measurement rather than noise, and the whole span is resolved.
 *    Current() and AverageCurrent() report it directly, so a charge produces a
 *    reading instead of another inference from capacities.
 *
 * What the arithmetic says instead is in bq_taper_floor_ua() and is served as
 * `charge_termination`. In one line: TRM 4.4.1's second condition is a FLOOR on
 * the current, so termination needs the current to sit inside a band for eighty
 * seconds — and Taper Current was set equal to the charger's termination
 * current, which makes that band exactly the range the charger refuses to
 * operate in. NOTHING IS CHANGED HERE ON THE STRENGTH OF THAT. This commit adds
 * reads and says what they mean; the table is untouched.
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
 * all — it answers the question wrongly and confidently. So all four status
 * words are also reported RAW, in hex, reserved bits and all. If TI populates
 * something undocumented, the hex shows it and a human decides what it means.
 *
 * That paragraph used to name CONTROL_STATUS and GaugingStatus as places ITPOR
 * is absent from without this firmware having read either of them. Both are
 * read now, and it is still absent from both. The claim was right; it is no
 * longer only a claim.
 *
 * Cadence: everything on one 60-second tick
 * -----------------------------------------
 * Every register above, the GaugingStatus subcommand, and the read-back of all
 * six data-memory parameters are read on the SAME 60 s tick, in one pass, from
 * loop(); GET /battery serves the cache and reports how old it is. Reading them
 * only on request would be cheaper still, and cost is not the reason.
 *
 * ⚠️ Current() is the one reading in this file for which 60 s is genuinely
 * coarse. The TRM updates it every second, and a taper lasts minutes — so a
 * one-minute sample sees the shape of a taper but will not catch its last few
 * seconds. That is a real limit of this diagnostic and it is stated rather than
 * fixed, because fixing it means either an I2C transaction on the request path,
 * which the rule below forbids for good reason, or a faster tick for every
 * other register that does not need one.
 *
 * 🔴 The reason is that loop() must stay the only task that touches the I2C
 * bus. Arduino's TwoWire is not re-entrant and there is exactly one bus here,
 * with the charger on it as well. Serving GET /battery with an I2C transaction
 * would run it on the AsyncTCP task, where a request landing during the 60 s
 * refresh interleaves two transactions on one bus — a fault with no witness,
 * on a device with no console, in the diagnostic that exists to be trusted.
 *
 * THE WRITES OBEY THE SAME RULE AND IT MATTERS MORE FOR THEM. Every byte this
 * file puts on the wire goes out from gauge_configure(), called by
 * battery_probe() from hw_probe() at boot, or from the drift path in
 * battery_refresh(), called by loop(). There is no path from a request to Wire
 * in this file and there must not be one added: a multi-step block transfer
 * interleaved by another task does not fail cleanly, it writes half a parameter
 * with a checksum computed over the other half.
 *
 * The freshness given up is worth nothing here. Every one of the five extra
 * registers is a configuration or state fact — a capacity in mAh, a security
 * mode, whether initialisation finished — and the data-memory values change
 * only when something rewrites them. They move on the order of a charge cycle
 * or a reset, not of a second. The response carries `age_s` so the reader never
 * has to assume.
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
 * tools/test_battery.sh compiles the block below on the host, straight out of
 * this file: the little-endian word assembly, all four status decoders, the
 * signed current conversion, the sanity gates, the RM/FCC cross-check, the
 * data-memory block builder, its checksum and length arithmetic against the
 * TRM's own worked example, the address allowlist, the subcommand allowlist,
 * the MAC echo check, the taper floor, the parameter table, and the rule that
 * decides whether the configuration is still live.
 *
 * It also slices charger.cpp's table into the same binary and asserts that the
 * gauge's taper current equals the charger's termination current. That single
 * equality is what this entire commit rests on, it spans two files, and it is
 * exactly the kind of pairing that rots the first time somebody changes one of
 * them alone.
 *
 * What is NOT covered, plainly: the I2C transactions, and that is a larger
 * uncovered share here than it was. bq27220_read16(), bq27220_dm_write() and
 * the access and CONFIG UPDATE sequences are calls into TwoWire wrapped around
 * timing the host has no equivalent for — a NAK, a missing pull-up, a bus
 * somebody else is driving, a part that wanted another 200 ms. Nothing below
 * simulates any of it, no test pretends otherwise, and the only proof that half
 * of this file works is a reading taken off the device: after a completed
 * charge, battery_status.fc must go true.
 */

/* Standard commands. Every one is a two-byte little-endian read. */
#define BQ_CMD_CONTROL            0x00   /* reads CONTROL_STATUS, see below */
#define BQ_CMD_VOLTAGE            0x08
#define BQ_CMD_BATTERY_STATUS     0x0A
#define BQ_CMD_CURRENT            0x0C   /* SIGNED mA */
#define BQ_CMD_REMAINING_CAP      0x10
#define BQ_CMD_FULL_CHARGE_CAP    0x12
#define BQ_CMD_AVERAGE_CURRENT    0x14   /* SIGNED mA */
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

/*
 * The same two bytes read as a SIGNED value, which is what Current() 0x0C and
 * AverageCurrent() 0x14 are — TRM 2.8: "a signed integer value that is the
 * instantaneous current flow through the sense resistor", positive into the
 * cell and negative out of it.
 *
 * A one-line conversion with a comment on it because getting it wrong is not
 * visible: an 80 mA discharge is 0xFFB0 on the wire, and read as unsigned that
 * is 65456 mA — a number no reader would believe. But a 5 mA discharge is
 * 65531, and a gauge that has stopped measuring reads 0 either way. The
 * interesting readings on this device are the small ones, which are exactly the
 * ones where an unsigned mistake still looks like a current.
 */
static int16_t bq_signed(uint16_t raw) {
    return (int16_t)raw;
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
 * GaugingStatus, TRM SLUUBD4A Table 2-4 — AND WHY IT IS NOT A SECOND OPINION
 * ON BatteryStatus[FC].
 *
 * It was worth reading precisely because it might have been one: this word has
 * its own FC bit and its own TC bit, and "we are reading the wrong FC" was a
 * live theory. The TRM closes it in Table 2-4's own footnotes, which say of
 * each bit that it "is identical to" its BatteryStatus counterpart — FC to
 * BatteryStatus[FC], TC to [TCA], TD to [TDA], FD to [FD]. Section 2.2.18 says
 * the same thing from the other end: "the most often checked flags from this
 * register are copied to the OperationStatus() direct read register for easier
 * access". One state, two windows onto it. A reading of GaugingStatus[FC]
 * disagreeing with BatteryStatus[FC] would be news about the silicon, not the
 * answer to why a full cell reads 97%.
 *
 * What it adds that nothing else exposes is CF, DSG, EDV and EDV1 — and DSG is
 * the useful one here, because it distinguishes CHARGING from DISCHARGE and
 * RELAXATION, which is the state machine the taper conditions run inside.
 *
 * Bit numbering is of the assembled 16-bit word, so Table 2-4's "high byte bit
 * 7" is bit 15 here. Five of the sixteen are reserved and are not named: 4, 8,
 * 9, 11 and 12. They are in the raw hex like every other reserved bit here.
 */
#define BQ_GS_FD         (1u << 0)   /* full discharge — same bit of state as
                                        BatteryStatus[FD]                     */
#define BQ_GS_FC         (1u << 1)   /* FULL CHARGE — the same state as
                                        BatteryStatus[FC], not a second one   */
#define BQ_GS_TD         (1u << 2)   /* terminate discharge = [TDA]           */
#define BQ_GS_TC         (1u << 3)   /* terminate charge = [TCA]              */
#define BQ_GS_EDV        (1u << 5)   /* cell below the EDV0 threshold         */
#define BQ_GS_DSG        (1u << 6)   /* set in DISCHARGE or RELAXATION, clear
                                        in CHARGING — not exposed elsewhere   */
#define BQ_GS_CF         (1u << 7)   /* battery conditioning is needed        */
#define BQ_GS_FCCX       (1u << 10)  /* coulomb-counter clock: 0 = 1 Hz,
                                        1 = 16 Hz                             */
#define BQ_GS_EDV1       (1u << 13)  /* cell below the EDV1 threshold         */
#define BQ_GS_EDV2       (1u << 14)  /* cell below the EDV2 threshold         */
#define BQ_GS_VDQ        (1u << 15)  /* this discharge qualifies for an FCC
                                        update — the same state as
                                        OperationStatus[VDQ]                  */

/*
 * CONTROL_STATUS, TRM SLUUBD4A Table 2-3.
 *
 * The whole high byte is reserved and so are bits 6 and 7 of the low byte, so
 * three flags and a three-bit field is all there is. It is read here for CCA:
 * the coulomb counter's calibration routine runs about a minute after
 * initialisation and again as conditions change, and a gauge caught mid
 * calibration is a gauge whose current readings are not to be argued from.
 */
#define BQ_CS_BATT_ID_SHIFT 0        /* BATT_ID[2:0] at bits 2:0              */
#define BQ_CS_BATT_ID_MASK  0x7u
#define BQ_CS_SNOOZE     (1u << 3)   /* SNOOZE power mode enabled             */
#define BQ_CS_BCA        (1u << 4)   /* board calibration routine active      */
#define BQ_CS_CCA        (1u << 5)   /* coulomb counter calibration active    */

/* The chemistry-profile selector, as a number rather than as three bits. Read
   out for the same reason sec_bits is: a caller looking at a decoded name
   should be able to see what produced it. */
static uint8_t bq_batt_id(uint16_t control_status) {
    return (uint8_t)((control_status >> BQ_CS_BATT_ID_SHIFT) & BQ_CS_BATT_ID_MASK);
}

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
#define BQ_SEC_SEALED    0x3u
#define BQ_SEC_UNSEALED  0x2u
#define BQ_SEC_FULL      0x1u

static const char *bq_sec_name(uint16_t op_status) {
    switch ((op_status >> BQ_OP_SEC_SHIFT) & BQ_OP_SEC_MASK) {
        case BQ_SEC_SEALED:   return "sealed";
        case BQ_SEC_UNSEALED: return "unsealed";
        case BQ_SEC_FULL:     return "full";
        default:              return "unknown";
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

/* ---------------------------------------------------------------------------
 * WHAT THE GAUGE IS WAITING FOR, as arithmetic rather than as prose
 *
 * TRM SLUUBD4A section 4.4.1 gives three conditions, ALL required, for the
 * primary charge termination that sets BatteryStatus[FC], quoted rather than
 * paraphrased because two of the three have been misread here before:
 *
 *   1. "During two consecutive periods of 40 seconds, the AverageCurrent() <
 *      Taper Current."
 *   2. "During the same two periods, the accumulated change in capacity must
 *      be > 0.25 mAh."
 *   3. "Voltage() > Charging Voltage - Taper Voltage."
 *
 * CONDITION 1 IS ON AverageCurrent(), NOT Current(). That is a filtered value
 * and it LAGS a decaying taper, so the instantaneous reading served as
 * `current_ma` can already be inside the band while the value the gauge is
 * actually testing is still above the ceiling. Both are read for that reason.
 *
 * CONDITION 2 IS A FLOOR ON THE CURRENT AND READS LIKE A FOOTNOTE. That is the
 * reason it is computed here instead of being left in the manual: it turns the
 * termination test into a BAND rather than a threshold. The gauge is not
 * waiting for the current to fall below Taper Current; it is waiting for the
 * current to spend two whole windows BETWEEN the capacity floor and Taper
 * Current, above the taper voltage. A charge that jumps from above the ceiling
 * to zero passes through the band without ever being in it, and never
 * terminates.
 *
 * THE 0.25 mAh IS PER QUALIFICATION, NOT PER WINDOW — "during the same two
 * periods", which is 80 s of accumulation and not 40. Reading it as per-window
 * doubles the floor, and this file did read it that way. In microamps, exactly
 * and with no rounding to argue about:
 *
 *   250 uAh * 3600 s/h / 80 s = 11250 uA
 *
 * It makes no difference to what the device waits for, because
 * bq_termination_floor_ua() takes the maximum against QuitCurrent's 40 mA and
 * that dominates either reading. It is corrected because the figure is SERVED
 * to agents as fact, and a number published as the TRM's should be the TRM's.
 * ------------------------------------------------------------------------- */
#define BQ_TAPER_WINDOW_S       40    /* TRM 4.4.1, not configurable on this
                                         part — there is no Current Taper
                                         Window row in Table 3-2             */
#define BQ_TAPER_MIN_CAP_UAH   250    /* 0.25 mAh across BOTH windows, likewise
                                         fixed — see the derivation above     */
#define BQ_TAPER_WINDOWS       2      /* "two consecutive periods"           */

/* Returned in microamps because 11.25 mA is not an integer and this figure is
   the edge of a band somebody will compare a reading against. Rounding it
   either way here would be inventing a bound the TRM does not state.

   The divisor is the WHOLE qualification, both windows, because that is the
   span the TRM accumulates the 0.25 mAh over. Dividing by one window is the
   easy misreading and it doubles the answer. */
static long bq_taper_floor_ua(void) {
    return (long)BQ_TAPER_MIN_CAP_UAH * 3600L /
           ((long)BQ_TAPER_WINDOW_S * (long)BQ_TAPER_WINDOWS);
}

/*
 * THE CAPACITY FLOOR IS NOT ALWAYS THE FLOOR THAT BINDS, and this is a second
 * mechanism rather than a restatement of the first.
 *
 * Qualifying a termination takes two 40 s windows, so eighty seconds. But the
 * gauge leaves CHARGE mode for RELAXATION when the current stays below Quit
 * Current for Chg Relax Time — TRM 4.9.60, and Chg Relax Time's ROM default is
 * SIXTY seconds (Table 3-2, 0x9230). Sixty is less than eighty. So a current
 * that settles below Quit Current has the gauge out of CHARGE mode, and out of
 * the termination algorithm entirely, twenty seconds before the second window
 * would have closed.
 *
 * Below Quit Current the qualification therefore cannot complete no matter what
 * the capacity condition says. The real floor is whichever of the two is
 * higher, and on this device it is Quit Current at 40 mA rather than the
 * capacity condition's 11.25 mA — it would still be Quit Current even on the
 * doubled misreading of that condition, which is why correcting the capacity
 * floor changes nothing about what the device waits for.
 *
 * The comparison is written out rather than assumed because both figures are
 * ROM defaults that a later commit may move: raise Chg Relax Time above eighty
 * seconds and the relax exit stops pre-empting anything, at which point the
 * capacity floor is the floor again and this function says so.
 */
#define BQ_CHG_RELAX_TIME_S    60     /* Table 3-2, 0x9230, ROM default      */

static bool bq_relax_pre_empts_taper(void) {
    return BQ_CHG_RELAX_TIME_S < (BQ_TAPER_WINDOW_S * BQ_TAPER_WINDOWS);
}

static long bq_termination_floor_ua(int16_t quit_current_ma) {
    long capacity_floor = bq_taper_floor_ua();
    if (!bq_relax_pre_empts_taper()) return capacity_floor;
    long quit_floor = (long)quit_current_ma * 1000L;
    return quit_floor > capacity_floor ? quit_floor : capacity_floor;
}

/* ===========================================================================
 * MAC SUBCOMMAND READS — how GaugingStatus is reached at all
 * =========================================================================*/

/*
 * GaugingStatus is NOT a standard command. There is no register to point at:
 * it is a Control()/ManufacturerAccessControl() subcommand, and reading it
 * means writing the subcommand code and then reading the answer back.
 *
 * TRM section 2.2 gives the procedure with DEVICE_NUMBER as the worked example:
 * "Write the data bytes 0x01 0x00 to the device address 0xAA starting at
 * command 0x00. Then read the response using an incremental read... starting at
 * command 0x3E, read four bytes. The result would be 0x01 0x00 0x20 0x03 with
 * the first two bytes reflecting subcommand, and the second two bytes
 * representing the device type in little endian order."
 *
 * So the frame is four bytes: the subcommand echoed back, then the value. The
 * echo is the whole reason this is worth doing carefully — see
 * bq_mac_echo_ok().
 *
 * CONTROL_STATUS does not need any of this. Section 2.2 again: "Reading the
 * Control() registers will always report the CONTROL_STATUS() data field",
 * and "Writing a 0x0000 to Control() is no longer necessary to read the
 * CONTROL_STATUS(), although it is okay if it is done." So it is a plain
 * two-byte read of register 0x00 with nothing written at all, and this file
 * does it that way: the genuinely read-only path is available, so it is taken.
 */
#define BQ_MAC_CONTROL_STATUS    0x0000
#define BQ_MAC_OPERATION_STATUS  0x0054
#define BQ_MAC_GAUGING_STATUS    0x0056

/*
 * THE GUARD ON SUBCOMMANDS, and this one IS about damage in a way the
 * data-memory guard explicitly is not.
 *
 * bq_dm_write_allowed() stands between a typo and a silently corrupted gauge
 * model. This stands between a typo and a gauge that has thrown the model away:
 * the subcommand space puts RESET (0x0041) seven hex digits from GaugingStatus
 * (0x0056), with SEALED (0x0030), ENTER_CFG_UPDATE (0x0090) and RETURN_TO_ROM
 * (0x0F00) in the same small space. RESET on this part discards the six
 * parameters, the accumulated learning and the FULL ACCESS state in one
 * transaction, acknowledged, with no error anywhere.
 *
 * So the read path takes an allowlist rather than a subcommand, and the list
 * holds exactly the status words: both are read-only by definition, both are
 * available while SEALED (Table 2-2), and neither changes any state. Everything
 * else is refused before a byte reaches the wire.
 *
 * OPERATION_STATUS is on the list and is not currently read through it — the
 * standard command at 0x3A returns the same word for one transaction instead of
 * two. It is here because it is the one other status subcommand that is safe by
 * the same argument, and leaving it out would invite the next reader to widen
 * the guard rather than to use it.
 */
static bool bq_mac_read_allowed(uint16_t subcmd) {
    switch (subcmd) {
        case BQ_MAC_GAUGING_STATUS:   return true;
        case BQ_MAC_OPERATION_STATUS: return true;
        default:                      return false;
    }
}

/*
 * Does this four-byte frame answer the subcommand that was asked?
 *
 * This is the difference between a diagnostic and a plausible zero, and it is
 * the same call the file already made about ITPOR. A MAC read has three ways to
 * come back wrong that all look like data: the part may not have finished
 * processing the subcommand and still hold the previous one's frame; another
 * transaction may have re-pointed the MAC register between the write and the
 * read; and a part that does not implement the subcommand at all answers
 * something. In every one of those cases bytes arrive, bq_word() assembles them
 * happily, and the endpoint prints sixteen named flags that are about a
 * different question.
 *
 * The echo makes it checkable. The first two bytes must be the subcommand, low
 * byte first — TRM 2.2's example reads 0x01 0x00 back from subcommand 0x0001.
 * If they are not, the read did not happen and the endpoint says so instead of
 * decoding whatever did arrive.
 */
static bool bq_mac_echo_ok(uint16_t subcmd, const uint8_t *frame) {
    return frame[0] == (uint8_t)(subcmd & 0xFFu) &&
           frame[1] == (uint8_t)((subcmd >> 8) & 0xFFu);
}

/* ===========================================================================
 * DATA MEMORY — the six parameters this file writes
 * =========================================================================*/

/*
 * The addresses, from TRM SLUUBD4A Table 3-2, with that table's own range and
 * ROM default alongside each one.
 *
 * These are the RAM addresses, reached through the 0x3E address pointer. The
 * TRM lists a SECOND set of rows for the same parameters in the 0x40xx range,
 * labelled "Calibration/Configuration (Present OTP)" — that is the OTP shadow
 * image used by BQStudio's flashstream programming and it is not what a
 * runtime write addresses. Using one of those here would be a plausible-looking
 * write to nothing at all.
 */
#define BQ_DM_CHARGING_CURRENT    0x91FB  /* mA  range 0..1000  default  200 */
#define BQ_DM_CHARGING_VOLTAGE    0x91FD  /* mV  range 0..4600  default 4200 */
#define BQ_DM_TAPER_CURRENT       0x9201  /* mA  range 0..1000  default  100 */
#define BQ_DM_CHARGE_DETECT       0x922A  /* mA  range 0..2000  default   75 */
#define BQ_DM_QUIT_CURRENT        0x922C  /* mA  range 0..1000  default   40 */
#define BQ_DM_CEDV_TAPER_VOLTAGE  0x92A5  /* mV  range 0..1000  default  100 */

/*
 * TWO MORE ADDRESSES THAT ARE READ AND NEVER WRITTEN, and the reason they are
 * here at all.
 *
 * Setting BatteryStatus[FC] is what the taper change above is for, and it is
 * the outcome to expect. What it does NOT promise is that FullChargeCapacity
 * is learned down from the 1300 mAh design value — and the percentage a user
 * looks at is computed against FullChargeCapacity, not against the flag. The
 * gauge only updates FCC across a QUALIFIED DISCHARGE, which among other
 * things needs the current at the end of the discharge to be at least
 * 3C/32 = 122 mA on this cell. A pager that idles at single-digit milliamps
 * will not produce one, possibly ever.
 *
 * What would make the display read 100% anyway is CSYNC: with it set, the
 * gauge assigns RemainingCapacity = FullChargeCapacity at a valid charge
 * termination, so the reported percentage snaps to 100 without any learning
 * having happened.
 *
 * CSYNC IS IN GAUGING CONFIGURATION, 0x929B. NOT IN SOC FLAG CONFIG A. An
 * earlier version of this comment had it the other way round, which is why the
 * attribution is now written out with its proof — reading bit 1 of the wrong
 * word yields a bit that means something else and answers this question
 * confidently and wrongly, which is the failure mode the ITPOR paragraph above
 * refuses for exactly the same reason.
 *
 *   TRM 4.4.1: "if the CEDV Configuration [CSYNC] bit is set, then
 *   RemainingCapacity() is set equal to FullChargeCapacity()". Section 1.1
 *   names it in full: "if [CSYNC] is set in CEDV Gauging Configuration".
 *   Table 4-11 is titled "CEDV Gauging Configuration Register Bit Definitions"
 *   and CSYNC is its low-byte bit 1. Table 3-2 gives 0x929B as Gauging
 *   Configuration, in the Gas Gauging / CEDV Profile 1 class.
 *
 *   SOC Flag Config A has its own bit table, Table 4-9, and there is no CSYNC
 *   anywhere in it: its bits are FCSETVCT, TCSETVCT, TCCLEARRSOC, TCSETRSOC,
 *   TCCLEARV, TCSETV and the four TD equivalents.
 *
 *   The arithmetic settles it independently of any table title. Table 3-2
 *   gives 0x927F a max of 0x0FFF, so 0x102A is not a legal value for it at
 *   all, let alone its default.
 *
 * SO THE MANUAL'S SELF-CONTRADICTION IS ABOUT 0x929B, AND IT IS REAL. Table
 * 3-2 gives Gauging Configuration a default of 0x102A. Table 4-11 prints that
 * same register's per-bit defaults as all zeros. Both are in the same
 * document, no amount of arguing settles it, and reading the part does — so
 * the part is read.
 *
 * ⚠️ WHAT 0x102A WOULD MEAN, said plainly because it is the outcome actually
 * being waited on and burying it would be the same failure as guessing it. Low
 * byte 0x2A is bits 1, 3 and 5: CSYNC, EDV_CMP and FIXED_EDV0. So if Table 3-2
 * is the correct reading, CSYNC IS ALREADY SET FROM THE FACTORY and this whole
 * problem is smaller than it looks — the display would reach 100% the moment
 * the full-charge flag sets, with no capacity learning needed at all, and the
 * taper change above would be the entire fix. If Table 4-11 is correct, CSYNC
 * is clear and the percentage stays short of 100 until an FCC learning cycle
 * this device may never perform. That is a large difference and it is NOT
 * asserted either way here. It is what the read is for.
 *
 * SOC Flag Config A is read alongside it, and not as an afterthought: FCSETVCT
 * is the bit that enables BatteryStatus[FC] to be set on primary charge
 * termination at all (TRM 4.4.1, "the [FC] and [TCA] bits are set depending on
 * the SOC Flag Config A [FCSETVCT] and [TCSETVCT] options"). If that bit were
 * clear, no taper current would ever produce the flag and every measurement in
 * this file would be chasing the wrong register. Its default is 0x0C8C, which
 * has FCSETVCT set — and unlike 0x929B there is no dispute about it, since
 * Table 4-9's own per-bit defaults print 0x0C8C too, matching Table 3-2
 * exactly. The read confirms it rather than resolving it.
 *
 * Both are served as raw hex rather than decoded. For 0x929B that is because
 * the value is the open question; for 0x927F it is so the two are reported the
 * same way and a reader can check either against the tables above.
 *
 * ⚠️ NEITHER IS WRITABLE AND NEITHER MAY BECOME SO. bq_dm_write_allowed()
 * refuses 0x929B by name and tools/test_battery.sh asserts that refusal.
 * bq27220_dm_read() is deliberately unguarded because reading an address is
 * harmless; that asymmetry is the whole safety argument here, and adding
 * either of these to the write allow-list would destroy it.
 */
#define BQ_DM_GAUGING_CONFIG      0x929B  /* H2  CSYNC and the CEDV mode bits;
                                             default disputed, see above     */
#define BQ_DM_SOC_FLAG_CONFIG_A   0x927F  /* H2  FCSETVCT and the flag-set
                                             options; default 0x0C8C         */

/* Every row in the table below is type I2 in TRM Table 3-2 — a signed 16-bit
   value — so the block is always the two address bytes plus two value bytes.
   The functions take the length as a parameter anyway, because the TRM's own
   worked example of the checksum uses a one-byte parameter and that example is
   the only independent oracle this arithmetic has. */
#define BQ_DM_VALUE_BYTES   2
#define BQ_DM_BLOCK_BYTES   (2 + BQ_DM_VALUE_BYTES)

/*
 * The bytes of one data-memory write, in the order they go onto the wire
 * starting at register 0x3E.
 *
 * THE TWO HALVES HAVE DIFFERENT ENDIANNESS AND THAT IS NOT A MISTAKE. The
 * address goes out LOW byte first: TRM section 6.1 writes 0x9F to 0x3E and 0x92
 * to 0x3F to address 0x929F. The value goes out HIGH byte first: the same
 * section writes 0x04 then 0xB0 to store 1200. (Section 6.1's own prose labels
 * those two address bytes "MSB" and "LSB", which contradicts the hex it prints
 * beside them; the hex is unambiguous and the vendor and Flipper drivers both
 * follow the hex.)
 *
 * Swap either half and the write still succeeds, still checksums, and still
 * reads back consistent with itself. 4208 mV becomes 28688, or lands at 0xFD91
 * which is not a parameter at all. There is no failure signal — which is why
 * this is a function with a test rather than four lines at a call site.
 */
static int bq_dm_block(uint16_t addr, int16_t value, uint8_t *out) {
    out[0] = (uint8_t)(addr & 0xFFu);
    out[1] = (uint8_t)((addr >> 8) & 0xFFu);
    out[2] = (uint8_t)(((uint16_t)value >> 8) & 0xFFu);
    out[3] = (uint8_t)((uint16_t)value & 0xFFu);
    return BQ_DM_BLOCK_BYTES;
}

/* The value out of the two bytes MACData returns, which arrive in the same
   big-endian order they were written in. Signed, because every parameter in the
   table is I2 and a range check that cannot see a negative cannot refuse one. */
static int16_t bq_dm_decode(const uint8_t *value_be) {
    return (int16_t)(((uint16_t)value_be[0] << 8) | (uint16_t)value_be[1]);
}

/*
 * The checksum: 0xFF minus the sum of every byte of the block — both address
 * bytes and every value byte. TRM section 2.30 defines it as "the complement of
 * the sum of the ManufacturerAccessControl() and the MACData() bytes", which
 * reads ambiguously enough that the worked example is what settles it.
 *
 * That example is in the TRM's Hibernate note: address 0x9221 with one data
 * byte 0x00 is followed by 0x4C, and 0xFF - (0x21 + 0x92 + 0x00) = 0x4C. The
 * address bytes are in. It is a test case below.
 *
 * A wrong checksum is not an error. The gauge simply does not commit the block,
 * and every read-back afterwards reports the old value — which looks exactly
 * like a part that refused the parameter rather than like arithmetic that was
 * wrong here.
 */
static uint8_t bq_dm_checksum(const uint8_t *bytes, int n) {
    uint8_t sum = 0;
    for (int i = 0; i < n; i++) sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(0xFFu - sum);
}

/*
 * MACDataLen: two address bytes, the value bytes, the checksum byte and this
 * length byte itself. Five for a one-byte parameter, six for ours — and five is
 * what the TRM's worked example writes, which is the other half of the same
 * oracle.
 *
 * This is NOT the number of bytes transferred to 0x60. That is always two, the
 * checksum and this length written together as a word; TRM section 2.31 is
 * explicit that the block is not executed until "MACDataSum() and MACDataLen()
 * are written together". LilyGO's driver passes `size + 2` as the transfer
 * length there and pushes two stale value bytes into 0x62 and 0x63 past the end
 * of the pair. See bq27220_dm_write().
 */
static uint8_t bq_dm_len(int value_bytes) {
    return (uint8_t)(2 + value_bytes + 1 + 1);
}

/*
 * THE GUARD. Every data-memory write goes through this and there is no other
 * path to the bus — bq27220_dm_write() calls it and refuses on false.
 *
 * An allowlist of addresses first and a range check second, the same shape and
 * for a related but not identical reason to bq25896_write_allowed()'s. That
 * guard stands between a typo and 4.6 V into a lithium cell. This one cannot:
 * the BQ27220 has no FET drivers and no charge control, so the worst a wrong
 * value here achieves is a wrong reading.
 *
 * What it stands between is a typo and a SILENTLY corrupted gauge model. A
 * data-memory address is four hex digits with no structure, no acknowledgement
 * and no plausibility check on the part's side. 0x929F is DesignCapacity,
 * 0x929D is FullChargeCapacity and 0x92A3 is Design Voltage — each within one
 * keystroke of an address in the table, each fully writable, and each capable
 * of making every number this endpoint reports wrong in a way that looks like a
 * successful configuration. 0x92A3 in particular is the address LilyGO's own
 * header mislabels as EMF, so it is not a hypothetical keystroke; it is a real
 * one somebody already made upstream.
 *
 * The ranges are the TRM's own, with one exception. Charging Voltage is bounded
 * at 4208 mV rather than at the TRM's 4600, because this value is what the gauge
 * is told to expect the charger to do and charger.cpp's guard will not let the
 * charger exceed 4208. The two bounds are the same number on purpose: telling
 * the gauge to wait for a voltage this board is forbidden to produce would
 * recreate exactly the never-terminates bug this commit fixes, one file over.
 */
#define BQ_DM_CHARGING_CURRENT_MAX     1000
#define BQ_DM_CHARGING_VOLTAGE_MAX     4208
#define BQ_DM_TAPER_CURRENT_MAX        1000
#define BQ_DM_CHARGE_DETECT_MAX        2000
#define BQ_DM_QUIT_CURRENT_MAX         1000
#define BQ_DM_CEDV_TAPER_VOLTAGE_MAX   1000

static bool bq_dm_write_allowed(uint16_t addr, int16_t value) {
    /* Every row in Table 3-2 that this file writes has a minimum of zero, and
       all six are signed, so a negative is out of range for all of them and is
       refused once here rather than six times below. */
    if (value < 0) return false;

    switch (addr) {
        case BQ_DM_CHARGING_CURRENT:   return value <= BQ_DM_CHARGING_CURRENT_MAX;
        case BQ_DM_CHARGING_VOLTAGE:   return value <= BQ_DM_CHARGING_VOLTAGE_MAX;
        case BQ_DM_TAPER_CURRENT:      return value <= BQ_DM_TAPER_CURRENT_MAX;
        case BQ_DM_CHARGE_DETECT:      return value <= BQ_DM_CHARGE_DETECT_MAX;
        case BQ_DM_QUIT_CURRENT:       return value <= BQ_DM_QUIT_CURRENT_MAX;
        case BQ_DM_CEDV_TAPER_VOLTAGE: return value <= BQ_DM_CEDV_TAPER_VOLTAGE_MAX;
        default:                       return false;
    }
}

/*
 * The table.
 *
 * UNLIKE charger_config[], THE ORDER HERE IS NOT A CORRECTNESS PROPERTY, and
 * that difference is worth one line because the two tables otherwise look
 * alike. There, REG07 has to go first so that the watchdog window is closed
 * before the writes that matter land. Here everything goes out inside one
 * CONFIG UPDATE session and none of it takes effect until EXIT_CFG_UPDATE_REINIT
 * — the gauge is not gauging while these land, so nothing can act on a
 * half-applied table. The order below is for a reader: the three that mirror
 * the charger, then the three that shape how the gauge interprets them.
 *
 * `why` is served in the endpoint, because a caller looking at 0x9201 = 100 has
 * no way to know from the number that it is the one that fixes the bug, still
 * less that its distance from the charger's 64 mA is the whole point of it.
 */
struct GaugeParam {
    uint16_t addr;
    int16_t value;
    const char *name;
    const char *units;
    const char *why;
};

static const GaugeParam gauge_config[] = {
    /*
     * ⚠️ THIS VALUE MUST STAY ABOVE charger.cpp's ITERM. DO NOT "TIDY" IT INTO
     * AGREEMENT WITH THE CHARGER. THAT AGREEMENT WAS THE BUG.
     *
     * WHAT WAS HERE, AND WHY IT NEVER WORKED
     * --------------------------------------
     * This entry read 128 for two commits, and charger.cpp's ITERM read 128
     * with it. They looked like a matching pair, and that was exactly the
     * trap: the two files were consistent, and the consistency is what stopped
     * FC ever setting. Every instinct a reader has on arriving at these two
     * numbers is to preserve their equality. TI's guidance is the opposite of
     * that instinct, and so was the device.
     *
     * The gauge does not wait for the current to fall BELOW its taper
     * threshold. TRM 4.4.1 condition 2 is a floor as well, so it waits for the
     * current to sit INSIDE a band — see bq_termination_floor_ua() above — for
     * two consecutive 40 s windows, above 4108 mV. With the ceiling equal to
     * the charger's cut-off the band is empty by construction: above 128 mA
     * the charger was still delivering, below it the charger had stopped and
     * the current was zero.
     *
     * THE MEASUREMENT, which is why this commit is not a fourth guess. A full
     * charge cycle sampled once a minute recorded ZERO samples inside the
     * band. fc never set. 1259 mAh went in, the cell finished genuinely full,
     * and it read 97% — because FullChargeCapacity is never learned down from
     * the 1300 mAh design value while fc stays false. The observed CV-phase
     * decay was about 12 mA per minute.
     *
     * WHY 100, AND NOT THE 90 THAT WAS PROPOSED FIRST
     * ----------------------------------------------
     * This is the part a later reader will otherwise undo, so it is the part
     * written out. The charger now terminates at 64 mA (REG05 = 0x10), and
     * 90 mA would satisfy every rule below just as 100 does. It was rejected
     * on the charger's tolerance:
     *
     *   The BQ25896 datasheet specifies termination accuracy ONLY at
     *   ITERM = 256 mA. Both of its rows are at that current and differ on
     *   ICHG: ±12% for ICHG <= 1344 mA and ±20% above it. This board charges
     *   at 512 mA, so ±12% is the applicable row and real termination lands
     *   near 72 mA. At the 64 mA this actually programs there is no figure at
     *   all, so a part cutting 20% high — worse than any published row — is
     *   also carried below, at 77 mA.
     *
     *   HOW LONG THE QUALIFICATION REALLY NEEDS IS ~120 s, NOT 80. The two
     *   40 s periods must be CONSECUTIVE, so entering the band part-way
     *   through a period wastes the remainder of it and the clock starts at
     *   the next boundary. Eighty seconds is the best case, 120 the worst, and
     *   the worst case is the one a margin should be sized against.
     *
     *   The band is only occupied while the charger is still delivering, so
     *   the travel that counts is from the taper ceiling down to wherever the
     *   charger actually stops, at about 12 mA/min:
     *
     *     taper 100, termination 64 (nominal):       36 mA -> ~180 s  clears
     *     taper 100, termination 72 (spec, ±12%):    28 mA -> ~140 s  clears
     *     taper 100, termination 77 (20%, off-spec): 23 mA -> ~115 s  marginal
     *     taper  90, termination 77 (20%, off-spec): 13 mA ->  ~65 s  FAILS
     *
     *   So the honest summary is that 100 clears the real 120 s requirement at
     *   the specified tolerance with about 20 seconds to spare, and only goes
     *   marginal on an assumption deliberately worse than anything TI
     *   publishes. 90 fails on that same assumption by a wide margin, which is
     *   what ruled it out.
     *
     *   TWO THINGS MAKE EVEN THE MARGINAL ROW LIKELY TO PASS, and they are
     *   worth knowing before somebody reads ~115 against ~120 and panics. The
     *   12 mA/min was measured near 128 mA; CV-phase current decays roughly
     *   exponentially, so between 100 and 64 mA it is SLOWER than that, which
     *   buys more time in the band rather than less. And 77 mA is off-spec
     *   pessimism in the first place.
     *
     * That 12 mA/min is a measured property of this cell at this charge
     * voltage, not a property of the code, which is why the arithmetic lives
     * in this comment and NOT in an assertion. A colder cell, an older cell or
     * a different VREG moves it, and a test pinned to it would go red on a
     * device that is working perfectly well.
     *
     * 100 keeps margin under every rule that applies at once:
     *
     *   above the charger's threshold   100 / 64 = 1.56x. TI say "slightly
     *     higher"; their worked example is 70 mA against a 50 mA charger, a
     *     ratio of 1.4, chosen because chargers hold their threshold to about
     *     ±10%. This charger's is unspecified at 64 mA, so the ratio is larger
     *     than theirs on purpose.
     *   below C/10                      130 mA on this 1300 mAh cell.
     *   above C/100                     13 mA. Below that the taper is inside
     *     the noise the shunt reads at rest.
     *   ordering                        Taper > ChargeDetect > Quit, which is
     *     100 > 75 > 40. A ceiling at or under ChargeDetectThreshold would
     *     mean the gauge stops calling it a charge before it can qualify one.
     *
     * SLUA777 3.2, SLUA903 2.2 and SLUA917, in identical words across three
     * independent application reports: "It is very important to set the taper
     * current programmed in the data flash of the gauge SLIGHTLY HIGHER than
     * the taper current threshold of the charger. This ensures that the gauge
     * detects the battery is fully charged BEFORE the charger cuts off
     * charge."
     *
     * WHAT A WRONG CHOICE LOOKS LIKE ON THE DEVICE, kept so it is recognised
     * rather than re-diagnosed a fifth time:
     *
     *   TOO LOW (at or below the charger's ITERM — THE STATE THIS FIXES)
     *     fc stays false through a charge the charger calls complete.
     *     FullChargeCapacity stays at DesignCapacity, soc_percent reads a few
     *     points short of 100 on a genuinely full cell, and vdq stays false.
     *     The band is empty: above the ceiling the charger is still
     *     delivering, below it the charger has stopped and the current is
     *     zero. Nothing ever occupies it.
     *
     *   TOO HIGH (far above ITERM, into the constant-current phase)
     *     fc sets EARLY, while the cell is still meaningfully charging. That
     *     is worse than it looks, because it is silent: fc goes true, the
     *     percentage snaps to 100, and everything reads healthy. The damage
     *     is that FullChargeCapacity then learns SHORT — the gauge records a
     *     smaller pack than exists and every later percentage is computed
     *     against it. The symptom is a device that charges to 100% and then
     *     runs longer than 100% should last, or an FCC that drifts downward
     *     across cycles. Condition 3 bounds this: termination is only
     *     attempted above ChargingVoltage - TaperVoltage, so the window is
     *     inside the CV phase — but a taper current set high enough reaches
     *     into the start of CV, where real charge is still going in.
     *
     * THE CONFLICT THAT USED TO BLOCK THIS, and how it was resolved: the same
     * TI notes advise keeping taper current below C/10, which is 130 mA here.
     * While the charger terminated at 128 mA, "above the charger's ITERM" and
     * "below C/10" left a window two milliamps wide. One of the two had to
     * give, and it was the CHARGER: REG05 moved from 0x11 to 0x10, dropping
     * termination to 64 mA, which is C/20 and still inside the textbook C/10
     * to C/20 range for a lithium polymer cell. That is what opened room
     * between the two rules for a taper current to sit in.
     *
     * 100 IS ALSO THIS PARAMETER'S ROM DEFAULT, and that is worth saying out
     * loud because it changes what this line does. Table 3-2 gives Taper
     * Current a default of 100 mA — the same number now written. So against a
     * factory-default gauge this write is a no-op, and the behaviour change in
     * this commit lives entirely on the charger side, where termination moved
     * from 128 mA to 64 mA.
     *
     * It is written anyway, and not dropped, for two reasons. It PINS the
     * value instead of inheriting it: a part that arrives holding something
     * else is corrected rather than trusted, and LilyGO's own firmware writes
     * 128 here, so parts holding the wrong value are the expected case rather
     * than a hypothetical. And it keeps the number visible in one table beside
     * the charger's, which is where the relationship between the two is
     * legible at all.
     *
     * The read-back consequence is real and is asserted in
     * tools/test_battery.sh: four of these six now equal their ROM defaults,
     * and what still makes a wiped gauge detectable is Charging Current
     * (512 against the ROM's 200) and Charging Voltage (4208 against 4200).
     * Move either of those to its default and `applied: true` would start
     * being reported for a part that had lost everything.
     *
     * WHAT THIS DOES NOT PROMISE. Setting fc is the expected outcome. Learning
     * FullChargeCapacity DOWN from 1300 mAh is not: the gauge only updates it
     * across a qualified discharge, which needs the current at the end to be
     * at least 3C/32 = 122 mA, and a pager idling at single-digit milliamps
     * will never satisfy that. See the diagnostic read of Gauging
     * Configuration below for CSYNC, the bit that would make the display read
     * 100% regardless.
     */
    {BQ_DM_TAPER_CURRENT, 100, "TaperCurrent", "mA",
     "THE FIX. Deliberately ABOVE the charger's ITERM (REG05 = 0x10, 64 mA) "
     "and not equal to it: TI SLUA777/903/917 say the gauge's taper must be "
     "set SLIGHTLY HIGHER than the charger's threshold so the gauge sees full "
     "before the charger cuts off. Equal — which is what shipped, at 128 mA on "
     "both sides — means the gauge waits for a current band the charger will "
     "not produce, and a full measured charge cycle recorded zero samples in "
     "it. 100 mA is 1.56x the charger's 64 mA, below C/10 (130 mA), above "
     "C/100, and above ChargeDetectThreshold. See charge_termination."},
    {BQ_DM_CHARGING_CURRENT, 512, "ChargingCurrent", "mA",
     "matches the charger's ICHG (REG04), so the gauge's idea of a charge in "
     "progress matches what the charger delivers"},
    {BQ_DM_CHARGING_VOLTAGE, 4208, "ChargingVoltage", "mV",
     "matches the charger's VREG (REG06); termination also requires the cell "
     "to be within the taper voltage below this figure"},
    {BQ_DM_CEDV_TAPER_VOLTAGE, 100, "CEDVChargeTerminationVoltage", "mV",
     "a delta BELOW ChargingVoltage despite the name, so termination needs the "
     "cell above 4108 mV — the TRM's own suggested value"},
    {BQ_DM_CHARGE_DETECT, 75, "ChargeDetectThreshold", "mA",
     "above this the gauge calls it a charge rather than noise on the shunt"},
    {BQ_DM_QUIT_CURRENT, 40, "QuitCurrent", "mA",
     "below this the gauge calls the pack at rest, which is what lets it take "
     "the open-circuit reading a capacity update needs"},
};

#define GAUGE_CONFIG_COUNT ((int)(sizeof(gauge_config) / sizeof(gauge_config[0])))

/*
 * The read-only diagnostics, in a SEPARATE table from gauge_config[] and not
 * merely flagged inside it.
 *
 * The separation is the safety property. gauge_write_table() walks
 * gauge_config[] and writes every row; bq_dm_write_allowed() permits exactly
 * the addresses in it, and tools/test_battery.sh sweeps all 65536 addresses to
 * assert that the writable set and the table are the same set. Putting a
 * read-only address in that table with a `writable: false` field beside it
 * would make the guard depend on a boolean somebody has to remember to check.
 * Two tables means the write path cannot reach these addresses at all — there
 * is no flag to get wrong.
 *
 * No value and no `why` about what should be there, because nothing should be:
 * these rows exist to report what IS there.
 */
struct GaugeDiagnostic {
    uint16_t addr;
    const char *name;
    const char *question;
};

static const GaugeDiagnostic gauge_diagnostics[] = {
    {BQ_DM_GAUGING_CONFIG, "GaugingConfiguration",
     "CARRIES CSYNC — low byte bit 1, TRM SLUUBD4A Table 4-11, 'CEDV Gauging "
     "Configuration Register Bit Definitions'. CSYNC assigns RemainingCapacity "
     "= FullChargeCapacity at a valid charge termination (TRM 4.4.1), which is "
     "what would make the percentage read 100 without FullChargeCapacity ever "
     "being learned down — and learning it needs a qualified discharge "
     "reaching 3C/32 = 122 mA at the end, which a pager idling in single "
     "digits may never produce. THE TRM CONTRADICTS ITSELF ON THIS REGISTER'S "
     "DEFAULT: Table 3-2 gives 0x102A, Table 4-11 prints its per-bit defaults "
     "as all zeros. That is why it is read rather than assumed. If 0x102A is "
     "right, low byte 0x2A is bits 1, 3 and 5 — CSYNC, EDV_CMP, FIXED_EDV0 — "
     "so CSYNC is already set from the factory and the display should reach "
     "100 as soon as the full-charge flag does. Not asserted; that is what "
     "this read is for"},
    {BQ_DM_SOC_FLAG_CONFIG_A, "SOCFlagConfigA",
     "NO CSYNC HERE — its bit table is TRM Table 4-9 and CSYNC is not in it. "
     "What this register decides is whether BatteryStatus[FC] gets set at all: "
     "TRM 4.4.1 says [FC] and [TCA] are set on primary charge termination "
     "'depending on the SOC Flag Config A [FCSETVCT] and [TCSETVCT] options'. "
     "With FCSETVCT clear no taper current would ever produce the flag. Its "
     "default is 0x0C8C, which has FCSETVCT set, and Table 3-2 and Table 4-9 "
     "agree on that figure — so this read confirms rather than settles. Its "
     "max is 0x0FFF, which is also the proof that the 0x102A the TRM disputes "
     "belongs to GaugingConfiguration and not to this address"},
};

#define GAUGE_DIAG_COUNT ((int)(sizeof(gauge_diagnostics) / sizeof(gauge_diagnostics[0])))

/* The table entry at an address, or NULL. By address rather than by index,
   because the comment above says the order here is explicitly free to change
   and an index would quietly turn that freedom into a bug. */
static const GaugeParam *gauge_param_at(uint16_t addr) {
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (gauge_config[i].addr == addr) return &gauge_config[i];
    }
    return NULL;
}

/*
 * Which parameters are not holding what the table says, as a bitmask over it.
 *
 * Asked the same question twice, exactly as charger_verify_mask() is: once by
 * gauge_configure() about what it found before writing and what it reads back
 * after, and once by every 60 s pass about what the part is holding now. A
 * write that was acknowledged on the bus is not a write that took effect — a
 * checksum the part rejected leaves the old value in place and NAKs nothing.
 */
static uint8_t gauge_verify_mask(const int16_t *readback) {
    uint8_t mask = 0;
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (readback[i] != gauge_config[i].value) mask |= (uint8_t)(1u << i);
    }
    return mask;
}

/* Every bit set, which is what "we could not check" is recorded as. Never zero,
   so an unread pass can never be mistaken for a clean one. */
#define GAUGE_MASK_ALL ((uint8_t)((1u << GAUGE_CONFIG_COUNT) - 1u))

/*
 * What a pass concludes, derived in one place and copied nowhere — the shape
 * charger_derive() was refactored into after a review found the boot verdict
 * being served forever underneath live values that disagreed with it.
 *
 * `applied` is the summary the endpoint leads with, and it is re-derived every
 * pass rather than latched at boot: a gauge that lost its RAM at 03:00 must not
 * still be reporting a configuration as applied at noon.
 */
struct GaugeVerdict {
    uint8_t live_mask;        /* bit i set = gauge_config[i] no longer holds */
    bool config_still_live;   /* every parameter still reads back as written */
    bool applied;             /* and the boot write is what put it there     */
};

static void gauge_derive(GaugeVerdict &v, bool read_ok, bool boot_applied,
                         const int16_t *live) {
    /* A pass that could not read is recorded as everything disagreeing rather
       than as nothing, so "we could not check" can never read as "we checked
       and it was fine". */
    v.live_mask         = read_ok ? gauge_verify_mask(live) : GAUGE_MASK_ALL;
    v.config_still_live = read_ok && (v.live_mask == 0);
    /* Both halves required. A configuration that never landed is not made live
       by the part happening to agree with it later, and a boot write that
       verified is not still live if the values have since gone. */
    v.applied           = boot_applied && v.config_still_live;
}
/* host-test:end */

/* --- The bus --- */

/*
 * The registers the access and data-memory sequences are conducted through.
 * Standard commands (BQ_CMD_*, above) are a different space entirely; these are
 * the control and block-transfer path.
 */
#define BQ_REG_CONTROL    0x00   /* Control(): subcommands, the unseal keys    */
#define BQ_REG_MAC        0x3E   /* ManufacturerAccessControl(): subcommands,
                                    and the data-memory address pointer        */
#define BQ_REG_MAC_DATA   0x40   /* MACData(): the 32-byte block               */
#define BQ_REG_MAC_SUM    0x60   /* MACDataSum(), with MACDataLen() at 0x61     */

/* The keys, both fixed in ROM. See the header on why these and not the 0x8000
   pair section 3.3 of the same TRM names. */
#define BQ_KEY_UNSEAL_1   0x0414
#define BQ_KEY_UNSEAL_2   0x3672
#define BQ_KEY_FULL       0xFFFF

#define BQ_CTRL_ENTER_CFG_UPDATE  0x0090
/* 0x0091 EXITS AND RE-INITIALISES. 0x0092 also exits and applies nothing,
   silently — it is the one-digit edit that turns this whole file into a
   no-operation with every check still green. */
#define BQ_CTRL_EXIT_CFG_REINIT   0x0091

/*
 * Timing. None of this is in the TRM as a number except the two noted; the
 * rest are the vendor driver's empirical constants, which carry their own
 * measured failure thresholds in its source ("Fails at ~120us", "Fails at
 * ~500us"). They are used here rounded up to whole milliseconds, because this
 * runs once at boot and there is nothing to win by running it close to the
 * edge of what the part tolerates.
 */
#define BQ_DELAY_CONTROL_MS      5   /* between two control operations         */
#define BQ_DELAY_MAC_WRITE_MS    1   /* block, then its checksum               */
#define BQ_DELAY_MAC_SELECT_MS  15   /* address pointer to MACData valid;
                                        BQStudio's own figure                  */
#define BQ_DELAY_DM_COMMIT_MS   10   /* after the checksum lands               */
#define BQ_DELAY_CFG_APPLY_MS 2000   /* after the exit: TRM's re-init time,
                                        below which a DM read returns garbage  */
#define BQ_CFG_POLL_MS          50
#define BQ_CFG_TIMEOUT_MS     3000   /* TRM: entering "may take up to 1 second";
                                        section 2.2.21 says wait 2 s to read
                                        the flag. Three is that with margin.   */

/*
 * One register, two bytes. The write is the register pointer and nothing else,
 * which is how a read is addressed on this part.
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

/* n bytes to one register, in one transaction. The block transfer needs this;
   nothing else does. */
static bool bq27220_write_bytes(uint8_t reg, const uint8_t *buf, int n) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    for (int i = 0; i < n; i++) Wire.write(buf[i]);
    return Wire.endTransmission() == 0;
}

static bool bq27220_read_bytes(uint8_t reg, uint8_t *buf, int n) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_ADDR, n) != n) return false;
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
    return true;
}

/*
 * A control subcommand: a 16-bit code, LOW BYTE FIRST, to whichever of the two
 * registers accepts it.
 *
 * Both are used, and which one is not a matter of taste — each follows the
 * worked procedure the TRM gives for that particular command. The unseal and
 * full-access keys go to Control() 0x00, because section 6.1 writes them there
 * (`wr 0x00 0x14 0x04`). ENTER and EXIT_CFG_UPDATE go to 0x3E, because the
 * TRM's own CONFIG UPDATE example writes them there and section 2.2 recommends
 * 0x3E generally, "as this allows performing the full command in a single I2C
 * transaction".
 *
 * Note the byte order is the opposite of a data-memory VALUE and the same as a
 * data-memory ADDRESS. Three orderings in one file, all from the primary
 * source, none of them guessable.
 */
static bool bq27220_subcommand(uint8_t reg, uint16_t subcmd) {
    uint8_t buf[2] = {(uint8_t)(subcmd & 0xFFu), (uint8_t)((subcmd >> 8) & 0xFFu)};
    return bq27220_write_bytes(reg, buf, 2);
}

/*
 * One status word out of the MAC subcommand space. Read-only, guarded, and
 * false unless the part answered the question that was asked.
 *
 * The guard is inside this function rather than at its call site for the same
 * reason bq27220_dm_write()'s is: adding a call site must not be able to add a
 * way around it. A refused subcommand returns false without opening a
 * transaction.
 *
 * The frame is read from 0x3E rather than from MACData() at 0x40 because that
 * is what TRM 2.2's worked example does, and because it is what makes the echo
 * available — a read starting at 0x40 gets the value with no way to tell which
 * subcommand produced it.
 *
 * BQ_DELAY_MAC_SELECT_MS between the write and the read is the same 15 ms
 * bq27220_dm_read() waits for the address pointer to become valid. The TRM
 * gives no figure for a subcommand; this is BQStudio's for the analogous step,
 * and the echo check is what turns "not long enough" into a reported failure
 * rather than into a decoded stale frame.
 */
static bool bq27220_mac_read16(uint16_t subcmd, uint16_t &val) {
    if (!bq_mac_read_allowed(subcmd)) return false;

    uint8_t sel[2] = {(uint8_t)(subcmd & 0xFFu), (uint8_t)((subcmd >> 8) & 0xFFu)};
    if (!bq27220_write_bytes(BQ_REG_MAC, sel, 2)) return false;
    delay(BQ_DELAY_MAC_SELECT_MS);

    uint8_t frame[4];
    if (!bq27220_read_bytes(BQ_REG_MAC, frame, 4)) return false;
    if (!bq_mac_echo_ok(subcmd, frame)) return false;

    val = bq_word(frame[2], frame[3]);
    return true;
}

/*
 * ONE data-memory parameter, and the ONLY way a data-memory byte reaches this
 * part.
 *
 * The guard is inside this function rather than at its call sites, so that
 * adding a call site cannot add a way around it. A refused write returns false
 * without opening a transaction: nothing reaches the wire at all. Called from
 * gauge_write_table() and from nowhere else, which runs from the loop task —
 * see the I2C rule in the header.
 *
 * THE SECOND WRITE IS TWO BYTES. Not `n + 2`, which is what LilyGO's driver
 * passes and what would push block[2] and block[3] past MACDataLen into 0x62
 * and 0x63. The pair at 0x60 is the checksum and the length and nothing else;
 * TRM section 2.31 has the block execute when they are "written together as a
 * word". Flipper Zero's driver, which LilyGO's derives from, writes 2.
 *
 * Nothing here verifies. A rejected checksum is not signalled on the bus — the
 * part simply keeps the old value — so verification is a read-back, and it
 * happens in gauge_configure() and again on every 60 s pass.
 */
static bool bq27220_dm_write(uint16_t addr, int16_t value) {
    if (!bq_dm_write_allowed(addr, value)) return false;

    uint8_t block[BQ_DM_BLOCK_BYTES];
    int n = bq_dm_block(addr, value, block);
    if (!bq27220_write_bytes(BQ_REG_MAC, block, n)) return false;
    delay(BQ_DELAY_MAC_WRITE_MS);

    uint8_t tail[2] = {bq_dm_checksum(block, n), bq_dm_len(BQ_DM_VALUE_BYTES)};
    if (!bq27220_write_bytes(BQ_REG_MAC_SUM, tail, 2)) return false;
    delay(BQ_DELAY_DM_COMMIT_MS);
    return true;
}

/*
 * One data-memory parameter back out. Point the address register at it, wait,
 * and read the value from MACData in the big-endian order it was written.
 *
 * No CONFIG UPDATE is needed to read — only data-memory access, which is what
 * the part is left in. This is not guarded: reading an address is harmless and
 * every caller reads out of the table anyway.
 */
static bool bq27220_dm_read(uint16_t addr, int16_t &value) {
    uint8_t sel[2] = {(uint8_t)(addr & 0xFFu), (uint8_t)((addr >> 8) & 0xFFu)};
    if (!bq27220_write_bytes(BQ_REG_MAC, sel, 2)) return false;
    delay(BQ_DELAY_MAC_SELECT_MS);

    uint8_t data[BQ_DM_VALUE_BYTES];
    if (!bq27220_read_bytes(BQ_REG_MAC_DATA, data, BQ_DM_VALUE_BYTES)) return false;
    value = bq_dm_decode(data);
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
    /* The two status words that are not standard commands, and the two signed
       currents. `gauging_ok` false means the subcommand did not answer with its
       own echo — not that every flag in it is clear. */
    uint16_t gauging_status;
    uint16_t control_status;
    int16_t current_ma;
    int16_t avg_current_ma;
    bool remaining_ok;
    bool full_ok;
    bool design_ok;
    bool op_ok;
    bool batt_ok;
    bool gauging_ok;
    bool control_ok;
    bool current_ok;
    bool avg_current_ok;
    /* The data-memory read-back, in table order, and what this pass concluded
       from it. Re-derived every pass rather than latched at boot, so a gauge
       that lost its RAM cannot go on being reported as configured. */
    int16_t dm_live[GAUGE_CONFIG_COUNT];
    bool dm_read_ok;
    /* The read-only diagnostics, with a flag EACH rather than one flag for the
       set. They answer independent questions — a Gauging Configuration that
       reads and a SOC Flag Config A that does not is a useful half-answer, and
       collapsing the two would throw it away. Nothing is derived from these and
       no verdict depends on them; they are reported and nothing else. */
    int16_t dm_diag[GAUGE_DIAG_COUNT];
    bool dm_diag_ok[GAUGE_DIAG_COUNT];
    GaugeVerdict verdict;
    unsigned long read_ms;   /* millis() at the end of the pass; 0 = never */
};

/*
 * What the boot configuration did, kept separate from BatteryGauge because it
 * is a record of one event rather than a reading that refreshes. The same split
 * charger.cpp keeps between `boot_verified` and its per-pass verdict, and for
 * the same reason a review forced it there: the two answer different questions
 * and collapsing them is how a stale success ends up printed over live values
 * that contradict it.
 */
struct GaugeConfigState {
    bool attempted;          /* gauge_configure() has run, however it went     */
    bool access_ok;          /* the part reached FULL ACCESS                   */
    uint8_t sec_before;      /* SEC bits found before anything was written     */
    uint8_t sec_after;       /* SEC bits after the access sequence             */
    bool needed;             /* the table was NOT already in place             */
    bool cfgupdate_entered;  /* the part confirmed CONFIG UPDATE mode          */
    bool cfgupdate_exited;   /* and confirmed leaving it                       */
    int writes_ok;           /* data-memory writes acknowledged on the bus     */
    bool verify_read_ok;     /* the read-back pass itself completed            */
    int16_t verified[GAUGE_CONFIG_COUNT];   /* what each read back as          */
    uint8_t verify_mask;     /* bit i set = gauge_config[i] wrong after all it */
    bool applied;            /* every parameter verified in place at boot      */
    unsigned int reapplied;  /* times the whole sequence was run again on drift */
    bool reapply_capped;     /* and the point at which it stopped trying        */
};

static BatteryGauge battery_gauge;
static GaugeConfigState gauge_cfg;
static portMUX_TYPE battery_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * How many times the whole sequence may be re-run after boot, over the life of
 * one boot.
 *
 * charger.cpp re-issues its table on EVERY pass that finds a disagreement, for
 * ever, and argues correctly that four register writes a minute is a bounded
 * cost and the right resting behaviour for a part it cannot put into a known
 * state. That argument does not carry over. Re-running this sequence means
 * entering CONFIG UPDATE, which SUSPENDS GAUGING, and leaving it costs the two
 * seconds the part needs to re-initialise — call it four seconds of a loop task
 * that is also drawing the panel and servicing the buttons.
 *
 * Paying that once to recover from a gauge that lost its RAM is obviously
 * right. Paying it every sixty seconds for ever, on a part that is refusing the
 * table for a reason we cannot see, would be a pager that stutters once a
 * minute for the rest of its uptime — and would repeatedly interrupt the very
 * learning this commit exists to enable. Three attempts, then it stops trying
 * and says so; a reboot is the escalation, and `reapply_capped` is how a caller
 * knows to reach for one.
 */
#define GAUGE_REAPPLY_MAX 3

/* --- Configuring --- */

/*
 * Bring the part to FULL ACCESS, which is what data memory needs.
 *
 * Both branches are conditional because the part's state on arrival is not a
 * datasheet fact — this device reports `sec: unsealed` today, having been left
 * that way by whatever ran on the board before us, and after a power-on reset
 * it would be unsealed again by default. So the sealed case is handled and not
 * assumed, and so is the case where there is nothing to do.
 *
 * SEC is read back after each step rather than trusted, and the return value is
 * "we are in FULL ACCESS", not "the writes were acknowledged". A key that the
 * part rejects NAKs nothing; it just leaves SEC where it was.
 */
static bool gauge_open_access(GaugeConfigState &s) {
    uint16_t op = 0;
    if (!bq27220_read16(BQ_CMD_OPERATION_STATUS, op)) return false;
    s.sec_before = bq_sec_bits(op);
    s.sec_after  = s.sec_before;

    /* The two keys must arrive consecutively with nothing else written to
       Control() in between (TRM 3.3), which is why nothing is read between
       them. */
    if (bq_sec_bits(op) == BQ_SEC_SEALED) {
        bq27220_subcommand(BQ_REG_CONTROL, BQ_KEY_UNSEAL_1);
        delay(BQ_DELAY_CONTROL_MS);
        bq27220_subcommand(BQ_REG_CONTROL, BQ_KEY_UNSEAL_2);
        delay(BQ_DELAY_CONTROL_MS);
        if (!bq27220_read16(BQ_CMD_OPERATION_STATUS, op)) return false;
    }

    /* Unsealed is not enough. TRM 6.1: "The BQ27220 boots up in UNSEAL, but not
       in FULL ACCESS. Enter FULL ACCESS to gain access to the Data Memory." */
    if (bq_sec_bits(op) != BQ_SEC_FULL) {
        bq27220_subcommand(BQ_REG_CONTROL, BQ_KEY_FULL);
        delay(BQ_DELAY_CONTROL_MS);
        bq27220_subcommand(BQ_REG_CONTROL, BQ_KEY_FULL);
        delay(BQ_DELAY_CONTROL_MS);
        if (!bq27220_read16(BQ_CMD_OPERATION_STATUS, op)) return false;
    }

    s.sec_after = bq_sec_bits(op);
    return s.sec_after == BQ_SEC_FULL;
}

/*
 * Wait for OperationStatus[CFGUPDATE] to reach `want`.
 *
 * Polled rather than slept through, because the TRM gives two different figures
 * for how long entering takes — "may take up to 1 second" in section 6.1 and
 * "the host must wait at least 2 seconds" in 2.2.21 — and a fixed sleep would
 * have to be the larger of them on every boot. Returns false on timeout, and
 * the caller treats that as "do not write", because writing outside CONFIG
 * UPDATE mode is how a table lands nowhere.
 */
static bool gauge_wait_cfgupdate(bool want) {
    for (int waited = 0; waited < BQ_CFG_TIMEOUT_MS; waited += BQ_CFG_POLL_MS) {
        uint16_t op = 0;
        if (bq27220_read16(BQ_CMD_OPERATION_STATUS, op) &&
            bq_flag(op, BQ_OP_CFGUPDATE) == want) {
            return true;
        }
        delay(BQ_CFG_POLL_MS);
    }
    return false;
}

/* Every parameter in the table read back, in table order. A read that fails
   leaves a zero behind and clears the result — no table value is zero, so the
   mask would catch it anyway, but "we could not read" is a different finding
   from "it holds the wrong number" and the caller is told which. */
static bool gauge_read_dm(int16_t *out) {
    bool ok = true;
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        out[i] = 0;
        if (!bq27220_dm_read(gauge_config[i].addr, out[i])) ok = false;
    }
    return ok;
}

/* The read-only diagnostics, same pass, same bus rule, separate result per row.
   Failure here is reported and acted on by nobody: these answer an open
   question about the part's defaults, and a gauge that will not talk about them
   still charges exactly as well. */
static void gauge_read_diagnostics(int16_t *out, bool *ok) {
    for (int i = 0; i < GAUGE_DIAG_COUNT; i++) {
        out[i] = 0;
        ok[i] = bq27220_dm_read(gauge_diagnostics[i].addr, out[i]);
    }
}

/* The table, written out. One function so that "what we send" has exactly one
   definition. Every write goes through bq27220_dm_write() and therefore through
   the guard, so an address edited into the table that is not on the allowlist
   does not reach the wire: it fails here and shows up as a short writes_ok. */
static int gauge_write_table() {
    int ok_count = 0;
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (bq27220_dm_write(gauge_config[i].addr, gauge_config[i].value)) ok_count++;
    }
    return ok_count;
}

/*
 * The whole sequence: get access, look, write only if the part is not already
 * holding the table, then verify.
 *
 * READING FIRST IS THE POINT, not an optimisation. The gauge is powered by the
 * cell and keeps its RAM across a reflash and a reboot of the ESP32, so on
 * every boot after the first this finds all six parameters already in place and
 * never enters CONFIG UPDATE at all — which matters because entering it
 * suspends gauging and leaving it re-initialises, and doing that on every boot
 * would throw away the accumulated learning this commit exists to enable, once
 * per reboot, for ever. `needed` is false on those boots and the endpoint says
 * so.
 *
 * Called from battery_probe() at boot, and again from the drift path in
 * battery_refresh() — where re-running the whole thing, access sequence
 * included, is exactly right: the reason a re-run is wanted is a power-on reset,
 * and a power-on reset takes the part out of FULL ACCESS as well as clearing
 * the values.
 */
static void gauge_configure() {
    GaugeConfigState s;
    memset(&s, 0, sizeof(s));
    s.attempted = true;

    /* Carried across, because they belong to the device's life and not to this
       attempt. Everything else above is deliberately zeroed: a re-run's verdict
       is its own. */
    portENTER_CRITICAL(&battery_mux);
    s.reapplied      = gauge_cfg.reapplied;
    s.reapply_capped = gauge_cfg.reapply_capped;
    portEXIT_CRITICAL(&battery_mux);

    s.access_ok = gauge_open_access(s);

    /* Zeroed before the short-circuit below can skip filling it. Without FULL
       ACCESS gauge_read_dm() never runs, and the `else` branch still copies
       this buffer into s.verified — reading uninitialised stack, which is
       undefined behaviour even though verify_read_ok would be false and nothing
       would report it. */
    int16_t found[GAUGE_CONFIG_COUNT];
    memset(found, 0, sizeof(found));
    bool found_ok = s.access_ok && gauge_read_dm(found);
    /* A read that did not complete counts as everything disagreeing, so an
       unreadable gauge is configured rather than assumed to be fine. */
    s.needed = !found_ok || (gauge_verify_mask(found) != 0);

    if (s.access_ok && s.needed) {
        bq27220_subcommand(BQ_REG_MAC, BQ_CTRL_ENTER_CFG_UPDATE);
        s.cfgupdate_entered = gauge_wait_cfgupdate(true);

        /* Writing outside CONFIG UPDATE mode does not fail, it just does not
           take, so a part that never confirmed the mode is left alone rather
           than written to hopefully. */
        if (s.cfgupdate_entered) {
            s.writes_ok = gauge_write_table();
            bq27220_subcommand(BQ_REG_MAC, BQ_CTRL_EXIT_CFG_REINIT);
            /* The re-initialisation genuinely takes this long; a data-memory
               read before it finishes returns garbage, which would present as
               a verification failure on a configuration that landed correctly. */
            delay(BQ_DELAY_CFG_APPLY_MS);
            s.cfgupdate_exited = gauge_wait_cfgupdate(false);
        }

        s.verify_read_ok = gauge_read_dm(s.verified);
    } else {
        /* Nothing was written, so the read that decided that IS the read-back.
           Doing it twice would cost six more transactions to learn what is
           already in `found`. */
        s.verify_read_ok = found_ok;
        memcpy(s.verified, found, sizeof(s.verified));
    }

    s.verify_mask = s.verify_read_ok ? gauge_verify_mask(s.verified) : GAUGE_MASK_ALL;
    s.applied     = s.verify_read_ok && (s.verify_mask == 0);

    portENTER_CRITICAL(&battery_mux);
    gauge_cfg = s;
    portEXIT_CRITICAL(&battery_mux);

    Serial.printf("[gauge] sec %u->%u, %s, wrote %d, verified %s\n",
                  s.sec_before, s.sec_after,
                  s.needed ? "config needed" : "config already in place",
                  s.writes_ok, s.applied ? "yes" : "NO");
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (s.verify_mask & (1u << i)) {
            Serial.printf("[gauge] %s (0x%04X): wanted %d, read %d\n",
                          gauge_config[i].name, gauge_config[i].addr,
                          gauge_config[i].value,
                          s.verify_read_ok ? s.verified[i] : 0);
        }
    }

    /* Serial is a console this device does not have, and the mask is a field in
       an endpoint nobody polls until something is already wrong. A gauge that
       did not take its configuration belongs in the event ring beside the boot
       and OTA lines — and the fact that it did not need configuring is worth a
       line too, because that is the normal case and its absence is the signal. */
    if (!s.applied) {
        event_add("gauge not configured: access %s, wrote %d/%d, mask 0x%02X",
                  s.access_ok ? "ok" : "FAILED", s.writes_ok,
                  GAUGE_CONFIG_COUNT, s.verify_mask);
    } else if (s.needed) {
        event_add("gauge configured: charge parameters written to data memory");
    }
}

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

    /* The two signed ones. Read through the same 16-bit path as everything
       else and reinterpreted, because the wire format is identical and only the
       meaning of the top bit differs. */
    uint16_t raw = 0;
    g.current_ok = bq27220_read16(BQ_CMD_CURRENT, raw);
    if (g.current_ok) g.current_ma = bq_signed(raw);
    g.avg_current_ok = bq27220_read16(BQ_CMD_AVERAGE_CURRENT, raw);
    if (g.avg_current_ok) g.avg_current_ma = bq_signed(raw);

    /* CONTROL_STATUS is a plain read of register 0x00 — nothing is written to
       get it. GaugingStatus is a subcommand and cannot be had that way.

       BOTH ARE READ BEFORE THE DATA-MEMORY READ-BACK BELOW, AND THAT ORDER IS
       DELIBERATE. The subcommand write re-points the MAC register at 0x3E,
       which is the same register bq27220_dm_read() points at an address. Each
       re-points before it reads, so either order works today; doing the
       subcommand first means the data-memory read-back — the thing this file
       makes claims about — is never the one running on a pointer somebody else
       set. */
    g.control_ok = bq27220_read16(BQ_CMD_CONTROL, g.control_status);
    g.gauging_ok = bq27220_mac_read16(BQ_MAC_GAUGING_STATUS, g.gauging_status);

    /*
     * The data-memory read-back, on the same pass and under the same rule as
     * everything else: this is the only task allowed on the bus.
     *
     * Gated on the boot pass having reached FULL ACCESS, because without it
     * these six reads are six transactions that cannot succeed, once a minute
     * for ever. The gate is not a claim that they WILL succeed — a gauge that
     * has since reset is in FULL ACCESS no longer, the reads fail, and that
     * failure is exactly what battery_refresh() acts on.
     */
    bool access, boot_applied;
    portENTER_CRITICAL(&battery_mux);
    access       = gauge_cfg.access_ok;
    boot_applied = gauge_cfg.applied;
    portEXIT_CRITICAL(&battery_mux);

    g.dm_read_ok = access && gauge_read_dm(g.dm_live);

    /* The two read-only diagnostics, on the same gate and for the same reason:
       without FULL ACCESS they are two more transactions that cannot succeed,
       once a minute for ever. They are read AFTER the configuration read-back
       rather than before it, so that the parameters this file makes claims
       about are never the ones running on an address pointer left by a read
       that is only advisory. */
    if (access) {
        gauge_read_diagnostics(g.dm_diag, g.dm_diag_ok);
    }

    /* One call, no field computed twice and none copied out and reassigned:
       the whole verdict comes back from the one function the host test drives.
       Deleting this line leaves the verdict zeroed by the memset above, which
       is `applied` false and a mask claiming nothing — never a stale success. */
    gauge_derive(g.verdict, g.dm_read_ok, boot_applied, g.dm_live);

    g.read_ms      = millis();

    portENTER_CRITICAL(&battery_mux);
    battery_gauge = g;
    portEXIT_CRITICAL(&battery_mux);
}

/*
 * Called once from hw_probe(), before the display claims the SPI bus.
 *
 * Read, configure, read again. The second read is what makes the very first
 * GET /battery report the post-configuration state rather than an empty
 * data-memory cache — the first pass runs before FULL ACCESS exists, so it
 * cannot have read a single parameter back.
 *
 * Nothing is written to a gauge that did not answer: hw.has_battery is set by
 * the first read and gates everything after it.
 */
static void battery_probe() {
    battery_read(true);
    if (!hw.has_battery) return;
    gauge_configure();
    battery_read(false);
}

/*
 * Called from loop() every 60 s. Without it the panel would show the boot-time
 * snapshot until the next reboot.
 *
 * It also acts on the data-memory read-back, and that is the one thing here
 * that can put bytes on the wire after boot. The trigger is deliberately BOTH
 * "a parameter reads wrong" and "the read-back failed at all", because the
 * event being recovered from is a power-on reset of the gauge — which clears
 * the values AND drops the part out of FULL ACCESS, so on this device it
 * presents as failed reads rather than as wrong ones.
 *
 * Bounded by GAUGE_REAPPLY_MAX, for the reason argued where that is defined:
 * unlike the charger's four register writes, this sequence suspends gauging and
 * costs seconds, and a part that will not take the table must not be allowed to
 * turn that into a permanent once-a-minute stutter.
 */
static void battery_refresh() {
    battery_read(false);

    if (!hw.has_battery) return;

    bool attempted, dm_ok, capped;
    uint8_t live_mask;
    unsigned int reapplied;
    portENTER_CRITICAL(&battery_mux);
    attempted = gauge_cfg.attempted;
    capped    = gauge_cfg.reapply_capped;
    reapplied = gauge_cfg.reapplied;
    dm_ok     = battery_gauge.dm_read_ok;
    live_mask = battery_gauge.verdict.live_mask;
    portEXIT_CRITICAL(&battery_mux);

    if (!attempted) return;

    /* The healthy case is handled BEFORE the cap, so that a device which
       recovers after the cap has been spent still says so. Returning early on
       `capped` would leave `drifted` latched true for ever and swallow the one
       event that tells a reader the problem went away by itself. */
    static bool drifted = false;
    if (dm_ok && live_mask == 0) {
        if (drifted) {
            drifted = false;
            event_add("gauge configuration restored");
        }
        return;
    }

    if (!drifted) {
        drifted = true;
        event_add("gauge configuration lost: %s",
                  dm_ok ? "values changed" : "data memory unreadable");
    }

    if (capped) return;

    reapplied++;
    bool now_capped = (reapplied >= (unsigned int)GAUGE_REAPPLY_MAX);
    portENTER_CRITICAL(&battery_mux);
    gauge_cfg.reapplied      = reapplied;
    gauge_cfg.reapply_capped = now_capped;
    portEXIT_CRITICAL(&battery_mux);

    /* Counted BEFORE the attempt, not after, so that a sequence which wedges
       part-way through has still spent its budget. A counter incremented only
       on the way out is a counter that never caps the one failure mode a cap is
       for. gauge_configure() carries both fields across, which is why they are
       written here first rather than after it. */
    gauge_configure();

    /* And re-read, so the endpoint reflects what the part holds now rather than
       the sixty-second-old snapshot that triggered the re-apply. Without this a
       successful recovery goes on reporting `applied: false` until the next
       tick — the same stale-verdict shape this file is careful about
       everywhere else. */
    battery_read(false);

    if (now_capped) {
        event_add("gauge reapply capped after %d attempts; reboot to retry",
                  GAUGE_REAPPLY_MAX);
    }
}

/* --- Endpoints --- */

static const SkillEndpoint battery_endpoints[] = {
    {"GET", "/battery", "Fuel gauge registers and all four status words raw and "
                        "decoded, the measured current, what charge termination "
                        "requires, the six parameters written at boot, and two "
                        "read-only data-memory diagnostics"},
    {NULL, NULL, NULL}
};

static const char *battery_describe() {
    return "## Skill: battery\n\n"
           "A BQ27220 CEDV fuel gauge at I2C 0x55 (SDA=8, SCL=18). This skill\n"
           "reads every register it reports, and **writes six parameters to the\n"
           "gauge's data memory at boot** — the charge current, voltage and\n"
           "taper current the charger on this board actually uses, plus three\n"
           "thresholds that shape how the gauge interprets them. Nothing else\n"
           "is written: no register in this response is ever written, no\n"
           "capacity is written, and no chemistry profile is loaded.\n\n"
           "Earlier versions of this skill wrote nothing at all and said so at\n"
           "length. That was true until the values to write were known; they\n"
           "are now, and this text says what is true instead.\n\n"
           "For charging — why a charge stopped, at what current it terminates,\n"
           "faults — see `GET /charger`. The BQ25896 charger shares this I2C\n"
           "bus and is configured by its own skill. Two of the values above are\n"
           "deliberately the same numbers as its own, because the gauge's job\n"
           "is to recognise what that charger does. **The taper current is\n"
           "deliberately NOT the same number**, and that difference is the\n"
           "whole fix — see below.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /battery | Every register read here raw and decoded, all "
           "four status words, the measured current, `charge_termination`, and "
           "`gauge_config`: what was written, read back, and whether it is "
           "still there |\n\n"
           "### Why the writes exist\n\n"
           "A full cell read 80%. On a charge the charger itself declared\n"
           "complete — `termination_done` at 4.2 V, tapering to 128 mA — the\n"
           "gauge held 1035 mAh against a FullChargeCapacity of 1300 mAh that\n"
           "was simply the design capacity, with `battery_status.fc` **false**\n"
           "and `operation_status.vdq` **false**.\n\n"
           "The gauge divides by the design capacity because it has never\n"
           "learned this pack's real one, and it cannot learn one because it\n"
           "has never noticed a charge finish. It declares a pack full when the\n"
           "charge current tapers below **its own** taper threshold at **its\n"
           "own** termination voltage — both in data memory, both still at ROM\n"
           "defaults, neither describing this board.\n\n"
           "The fix is not to write a capacity — that would be inventing the\n"
           "number the gauge is supposed to measure. It is to describe the\n"
           "charger to the gauge and let it find the capacity itself.\n\n"
           "**That much still holds. The specific number did not.** This text\n"
           "used to end: \"the charger stops at 128 mA, the gauge was waiting\n"
           "for 100 mA, that condition never arrives\" — and concluded that\n"
           "setting the gauge's taper current to the charger's 128 mA would\n"
           "make `fc` set on the next completed charge. It was applied, it\n"
           "verified, and the next completed charge ended at 1256 mAh, 4171 mV,\n"
           "97%, with `fc` still **false**.\n\n"
           "Why, from TRM 4.4.1 and served as `charge_termination` below: the\n"
           "second condition requires accumulated capacity above 0.25 mAh\n"
           "across the two 40 s periods, which is a **floor** on the current of\n"
           "11 250 uA. So the gauge waits for `AverageCurrent()` to sit\n"
           "*between* that floor and the taper current for two consecutive\n"
           "windows — 80 s at best and 120 s if the band is entered part-way\n"
           "through a window. Setting the taper current equal\n"
           "to the charger's termination current makes that band exactly the\n"
           "range the charger refuses to operate in: above 128 mA the charger\n"
           "is still delivering, below it the charger has stopped and the\n"
           "current is zero. The band is empty by construction, which is why\n"
           "all three attempts on this device failed the same way.\n\n"
           "### What this version changes, and what measured it\n\n"
           "The previous version changed nothing and added the readings that\n"
           "would show a taper actually happening — `current_ma`,\n"
           "`average_current_ma`, `gauging_status`. They did. Across one full\n"
           "charge cycle, sampled once a minute: **zero samples inside the\n"
           "band**, `fc` never set, 1259 mAh delivered into a cell that\n"
           "finished genuinely full and reported 97%, and a CV-phase decay of\n"
           "about 12 mA per minute. The band was empty exactly as predicted.\n\n"
           "So two numbers moved, in two files, and they moved **apart**:\n\n"
           "| | was | now |\n"
           "|---|---|---|\n"
           "| charger REG05 termination | 128 mA | **64 mA** (C/20) |\n"
           "| gauge Taper Current 0x9201 | 128 mA | **100 mA** |\n\n"
           "The charger had to move first. While it terminated at 128 mA, TI's\n"
           "\"above the charger's threshold\" and their \"below C/10\" — 130 mA\n"
           "on this 1300 mAh cell — left a window two milliamps wide.\n\n"
           "100 rather than 90, which was the first proposal. The BQ25896's\n"
           "termination accuracy is specified **only at ITERM = 256 mA**, in\n"
           "two rows that differ on `ICHG`: ±12% for ICHG <= 1344 mA and ±20%\n"
           "above it. This board charges at 512 mA, so **±12% is the applicable\n"
           "row** and real termination lands near 72 mA. At the 64 mA actually\n"
           "programmed there is no figure at all, so a part cutting 20% high —\n"
           "worse than any published row — is carried as the pessimistic case\n"
           "at 77 mA.\n\n"
           "The travel that counts runs from the ceiling down to wherever the\n"
           "charger really stops, and at the measured 12 mA/min it comes to\n"
           "~140 s at the specified tolerance and ~115 s at the off-spec one.\n"
           "Against a worst-case requirement of ~120 s, a taper of 100 clears\n"
           "the specified case with margin and is marginal only on the\n"
           "pessimistic one; a taper of 90 leaves ~65 s and fails it outright.\n"
           "Two things push the marginal row the right way: 12 mA/min was\n"
           "measured near 128 mA and CV decay is roughly exponential, so\n"
           "between 100 and 64 mA it is *slower* — more time in the band, not\n"
           "less — and 77 mA is beyond anything TI publishes to begin with.\n\n"
           "100 also stays below C/10, above C/100, and above\n"
           "`ChargeDetectThreshold`, keeping the required ordering\n"
           "Taper > ChargeDetect > Quit at 100 > 75 > 40.\n\n"
           "**Which half is doing the work.** 100 mA is also Taper Current's\n"
           "own ROM default, so against a factory-default gauge the 0x9201\n"
           "write changes nothing and the behaviour that actually moves is the\n"
           "charger's termination. The gauge write earns its place by pinning\n"
           "the value rather than inheriting it — LilyGO's firmware writes 128\n"
           "there, so parts holding the wrong number are the expected case, not\n"
           "a hypothetical.\n\n"
           "One consequence, because it is the sort of thing that rots quietly:\n"
           "four of the six parameters now equal their ROM defaults. What still\n"
           "makes a gauge that lost its RAM detectable is Charging Current (512\n"
           "against the ROM's 200) and Charging Voltage (4208 against 4200).\n\n"
           "**What this promises and what it does not.** `fc` setting is the\n"
           "expected outcome. `FullChargeCapacity` learning down from the\n"
           "1300 mAh design value is **not promised**: the gauge only updates\n"
           "it across a qualified discharge, which needs at least\n"
           "3C/32 = 122 mA at the end, and this device idles at single-digit\n"
           "milliamps. What would make the percentage read 100 regardless is\n"
           "the `CSYNC` bit in **Gauging Configuration** (0x929B) — and TI's\n"
           "own manual contradicts itself on that register's default, so this\n"
           "version **reads** it rather than assuming it. See\n"
           "`gauge_diagnostics`.\n\n"
           "### What is written\n\n"
           "Six data-memory parameters, at the addresses TRM SLUUBD4A\n"
           "Table 3-2 gives. Five carry the same values as LilyGO's own\n"
           "production firmware for this board. **The taper current does\n"
           "not** — the vendor writes 128 mA there, equal to the 128 mA their\n"
           "charger configuration terminates at, and that equality is the\n"
           "defect described above. Their firmware has it too; matching them\n"
           "here would mean keeping it.\n\n"
           "| Address | Parameter | Value | Why |\n"
           "|---------|-----------|-------|-----|\n"
           "| 0x9201 | Taper Current | 100 mA | **the fix** — deliberately "
           "*above* the charger's 64 mA ITERM (REG05), at a ratio of 1.56 |\n"
           "| 0x91FB | Charging Current | 512 mA | equals the charger's ICHG "
           "(REG04) |\n"
           "| 0x91FD | Charging Voltage | 4208 mV | equals the charger's VREG "
           "(REG06) |\n"
           "| 0x92A5 | CEDV Charge Termination Voltage | 100 mV | a delta "
           "*below* Charging Voltage, despite the name |\n"
           "| 0x922A | Charge Detection Threshold | 75 mA | above this it is a "
           "charge, not shunt noise |\n"
           "| 0x922C | Quit Current | 40 mA | below this the pack is at rest |\n\n"
           "**These live in RAM, not flash.** The gauge loses them on a\n"
           "power-on reset, so they are applied at every boot. Making them\n"
           "permanent would mean burning OTP, which needs 7.4 V applied to\n"
           "GPOUT from a bench supply and TI's own tooling — not reachable from\n"
           "a 3.3 V I2C session, and not attempted.\n\n"
           "In practice the gauge is powered by the cell and keeps its RAM\n"
           "across a reflash, so the write sequence runs on the **first** boot\n"
           "after this firmware and afterwards finds everything already in\n"
           "place and skips it. `needed_writing` is false on those boots, and\n"
           "`writes_ok: 0` alongside `applied: true` is the healthy reading,\n"
           "not a failure. Skipping matters: entering CONFIG UPDATE suspends\n"
           "gauging and leaving it re-initialises, which would discard the\n"
           "learning these writes exist to enable — once per reboot, for ever.\n\n"
           "### How to read `gauge_config`\n\n"
           "`applied` is the summary and is **re-derived on every 60 s pass**,\n"
           "not latched: it is true only when the boot pass verified every\n"
           "parameter *and* the latest read-back still finds them in place. A\n"
           "gauge that lost its RAM at 03:00 reads `applied: false` at noon.\n"
           "`applied_at_boot` is the boot record on its own, `still_matches`\n"
           "the latest comparison on its own, and `readback_ok` says whether\n"
           "that comparison could be made at all — a read that did not happen\n"
           "counts as unverified, never as verified.\n\n"
           "`params[]` gives each parameter its `address`, `value`, `units`,\n"
           "`readback` and `matches` from boot, `live` and `still_matches` from\n"
           "the latest pass, and a `why`. `full_access`, `sec_before`,\n"
           "`sec_after`, `cfgupdate_entered` and `cfgupdate_exited` expose the\n"
           "access sequence, because a write that never entered CONFIG UPDATE\n"
           "lands nowhere and should not be indistinguishable from one that\n"
           "did.\n\n"
           "If the read-back ever fails or disagrees, the whole sequence is run\n"
           "again from the loop task — at most `reapply_max` (3) times per\n"
           "boot, after which `reapply_capped` goes true and it stops. That cap\n"
           "is not caution about damage; it is that re-running costs seconds of\n"
           "suspended gauging, and a part that will not take the table must not\n"
           "turn that into a permanent once-a-minute stutter. A reboot is the\n"
           "escalation.\n\n"
           "### `gauge_diagnostics` — read, never written\n\n"
           "Two data-memory addresses are read on every pass and served as raw\n"
           "hex. Nothing is written to either, and they are kept in a table of\n"
           "their own so that the write path cannot reach them at all.\n\n"
           "| Address | Parameter | Why it is read |\n"
           "|---------|-----------|----------------|\n"
           "| 0x929B | Gauging Configuration | **carries `CSYNC`** (low byte\n"
           "bit 1, TRM Table 4-11), which assigns RemainingCapacity =\n"
           "FullChargeCapacity at a valid termination |\n"
           "| 0x927F | SOC Flag Config A | **no `CSYNC` in it** (its bit table\n"
           "is Table 4-9); it holds `FCSETVCT`, which is what allows `fc` to be\n"
           "set on primary termination at all |\n\n"
           "`CSYNC` matters because `fc` setting does not by itself make the\n"
           "percentage read 100 — that needs `FullChargeCapacity` to come down\n"
           "from the design value, which needs a qualified discharge this\n"
           "device may never perform. With `CSYNC` set the percentage snaps to\n"
           "100 at termination regardless.\n\n"
           "It is read rather than assumed because **TRM SLUUBD4A contradicts\n"
           "itself on Gauging Configuration's ROM default**: Table 3-2 gives\n"
           "0x102A, and Table 4-11 prints that same register's per-bit defaults\n"
           "as all zeros. Both are in the same document.\n\n"
           "**If 0x102A is the correct reading, `CSYNC` is already set from the\n"
           "factory**: low byte 0x2A is bits 1, 3 and 5 — `CSYNC`, `EDV_CMP`,\n"
           "`FIXED_EDV0`. That would mean the display reaches 100% as soon as\n"
           "the full-charge flag sets, with no capacity learning involved, and\n"
           "the taper change is the whole fix. It is **not asserted** — that is\n"
           "what the read is for — but it is the outcome to look for first in\n"
           "the value this endpoint returns.\n\n"
           "SOC Flag Config A has no such dispute: Table 3-2 and Table 4-9 both\n"
           "give 0x0C8C, which has `FCSETVCT` set. It is read to confirm that,\n"
           "because with `FCSETVCT` clear no taper current would ever produce\n"
           "the flag. Its documented max of 0x0FFF is also the plainest proof\n"
           "that the disputed 0x102A belongs to 0x929B and not to it.\n\n"
           "Both values are served undecoded — for 0x929B because the value is\n"
           "the open question, and for 0x927F so that the two are reported the\n"
           "same way and either can be checked against the tables above.\n\n"
           "0x929B is on the write guard's **refused** list by name and stays\n"
           "there. Reading any data-memory address is harmless; writing these\n"
           "is not.\n\n"
           "### Safety\n\n"
           "Writes go through one guard: an allowlist of exactly those six\n"
           "addresses, each bounded by the TRM's own range, and everything else\n"
           "refused. The BQ27220 has **no FET drivers and no charge control**,\n"
           "so a wrong value here produces wrong readings and never a wrong\n"
           "charge — the guard is not standing between a typo and a fire, it is\n"
           "standing between a typo and a silently corrupted gauge model.\n"
           "0x929F is DesignCapacity and 0x92A3 is Design Voltage, each one\n"
           "keystroke from an address above, each fully writable, and neither\n"
           "would report an error. Charging Voltage is additionally bounded at\n"
           "4208 mV rather than the TRM's 4600, to the same ceiling the charger\n"
           "guard enforces: telling the gauge to wait for a voltage this board\n"
           "may not produce would recreate this exact bug one file over.\n\n"
           "Everything written is volatile, a power cycle restores the ROM\n"
           "defaults, and the unseal keys are fixed in ROM, so the part cannot\n"
           "be locked out. It is left in FULL ACCESS rather than re-sealed: it\n"
           "arrived unsealed, and the 60 s read-back needs data-memory access.\n\n"
           "### What you get from the registers\n\n"
           "Ten standard commands, each a two-byte little-endian read:\n\n"
           "| Field | Command | Meaning |\n"
           "|-------|---------|---------|\n"
           "| `voltage_mv` | 0x08 | cell voltage, mV |\n"
           "| `soc_percent` | 0x2C | RelativeStateOfCharge, % |\n"
           "| `remaining_mah` | 0x10 | RemainingCapacity |\n"
           "| `full_charge_mah` | 0x12 | FullChargeCapacity |\n"
           "| `design_mah` | 0x3C | DesignCapacity |\n"
           "| `current_ma` | 0x0C | instantaneous current, **signed**, "
           "positive into the cell |\n"
           "| `average_current_ma` | 0x14 | the same filtered, ~14.5 s time "
           "constant |\n"
           "| `operation_status` | 0x3A | security mode and gauging state |\n"
           "| `battery_status` | 0x0A | charge/discharge flags |\n"
           "| `control_status` | 0x00 | calibration and profile bits |\n\n"
           "`gauging_status` is **not** in that list because it is not a\n"
           "register: it is MAC subcommand 0x0056. See below.\n\n"
           "The last seven are objects: each carries `ok`, and a value only when\n"
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
           "### The four status words\n\n"
           "All four are reported as `raw` (hex) and as decoded flags. From\n"
           "`operation_status`: `sec` (`sealed` / `unsealed` / `full` /\n"
           "`unknown`, with `sec_bits` alongside it), `cfgupdate`, `vdq`,\n"
           "`initcomp`, `smth`, `edv2`, `btpint`, `calmd`. From\n"
           "`battery_status`: `fc` (full charge), `fd`, `dsg`, `battpres`,\n"
           "`auth_gd`, `ocvgd`, `ocvcomp`, `ocvfail`, `chginh`, `tca`, `tda`,\n"
           "`sysdwn`, `otc`, `otd`, `sleep`. From `control_status`: `cca`\n"
           "(coulomb-counter calibration running), `bca`, `snooze`, `batt_id`.\n"
           "From `gauging_status`: `fc`, `fd`, `tc`, `td`, `dsg`, `cf`, `edv`,\n"
           "`edv1`, `edv2`, `vdq`, `fccx`.\n\n"
           "`sec` decides what is possible at all: a SEALED gauge accepts no\n"
           "configuration until it is unsealed with the right key.\n\n"
           "### gauging_status is not a second opinion\n\n"
           "It has its own `fc` and its own `tc`, and the obvious reading —\n"
           "that the firmware was watching the wrong FC all along — is wrong.\n"
           "TRM SLUUBD4A Table 2-4 says of each of these bits that it **\"is\n"
           "identical to\"** its counterpart: `fc` to `battery_status.fc`, `tc`\n"
           "to `tca`, `td` to `tda`, `fd` to `fd`, and `vdq` to\n"
           "`operation_status.vdq`. Section 2.2.18 puts it the other way round:\n"
           "the most-checked flags of this register are *copied* to the direct\n"
           "read registers. One state, four windows. If a pair ever disagrees,\n"
           "that is news about the silicon and not the answer to a percentage\n"
           "question.\n\n"
           "What is **only** here: `cf` (conditioning needed), `dsg` (set in\n"
           "DISCHARGE or RELAXATION, clear in CHARGING — the state machine the\n"
           "taper conditions run inside), `edv` and `edv1`.\n\n"
           "Reaching it costs a subcommand rather than a register read: 0x0056\n"
           "is written to 0x3E and a four-byte frame read back, of which the\n"
           "first two bytes echo the subcommand. **If that echo does not match,\n"
           "nothing is decoded** and `ok` is false with a `note` saying why —\n"
           "a frame that cannot be shown to answer this question is not served\n"
           "as sixteen plausible flags. Only status subcommands are reachable\n"
           "by this path at all; the allowlist exists because `RESET` (0x0041)\n"
           "lives in the same small space and would discard the configuration,\n"
           "the learning and the access state in one acknowledged transaction.\n\n"
           "`control_status` needs no subcommand: TRM 2.2 makes reading\n"
           "register 0x00 sufficient, so nothing at all is written to get it.\n\n"
           "### charge_termination — what FC is waiting for\n\n"
           "TRM 4.4.1 gives three conditions, all required, and this object\n"
           "substitutes this build's own numbers into them. **The second\n"
           "condition is a floor on the current, not a footnote**, and that is\n"
           "the whole reason the object exists: accumulated capacity must\n"
           "exceed 0.25 mAh **across the two periods together** — TRM 4.4.1\n"
           "says \"during the same two periods\", not per window — which is\n"
           "`capacity_floor_ua` = 11 250 uA. So termination needs the current\n"
           "to stay *between* `current_floor_ua` and `current_ceiling_ma` for\n"
           "`windows_required` consecutive `window_s` periods, with the cell\n"
           "above `min_voltage_mv`. It is a **band**, not a threshold. A charge\n"
           "that leaves the ceiling and reaches zero without spending that time\n"
           "in between passes through the band without ever being in it, and\n"
           "never terminates.\n\n"
           "**Two details of condition 1 that the numbers here do not carry.**\n"
           "The TRM tests `AverageCurrent()`, not `Current()` — a filtered\n"
           "value that lags a decaying taper, so `current_ma` can be inside the\n"
           "band while the value the gauge tests is not; both are served for\n"
           "that reason. And the two periods must be *consecutive*, so entering\n"
           "the band part-way through one wastes the remainder of it: 80 s is\n"
           "the best case and **~120 s the worst**, which is the figure a\n"
           "margin should be sized against. `measured_on`, `timing_note` and\n"
           "`worst_case_seconds` carry both in the response.\n\n"
           "**There are two floors and the higher one wins.** Below\n"
           "`QuitCurrent` the gauge leaves CHARGE mode for RELAXATION after\n"
           "`chg_relax_time_s` (TRM 4.9.60), and 60 s is *shorter* than the\n"
           "80 s two windows take — so the qualification is abandoned before it\n"
           "can finish. `relax_exit_pre_empts_taper` says whether that is true\n"
           "for this build, `quit_current_floor_ua` is that floor,\n"
           "`current_floor_ua` is the one that actually binds, and\n"
           "`floor_set_by` names which rule produced it. On this device it is\n"
           "QuitCurrent at 40 mA, not the capacity condition at 11.25 mA.\n\n"
           "`ti_guidance` carries TI's own recommendation verbatim in summary,\n"
           "including the part that conflicts with it on this board. Nothing in\n"
           "this object is written to the gauge and nothing in it is applied.\n"
           "It reports what the manual requires and what this firmware has\n"
           "configured, so the two can be compared without a PDF.\n\n"
           "**There is no `itpor` flag, and its absence is deliberate.** ITPOR\n"
           "— \"the RAM configuration was reset to defaults\" — is named in the\n"
           "BQ27220 TRM's prose as `Flags()[ITPOR]`, but on this part `Flags()`\n"
           "is `BatteryStatus()` and its bit table has no such bit; nor does\n"
           "OperationStatus, CONTROL_STATUS or GaugingStatus. Decoding it would\n"
           "mean guessing a bit position and answering the question wrongly.\n"
           "The `raw` hex of both words is reported for exactly this reason —\n"
           "reserved bits included, so an undocumented one is at least visible.\n\n"
           "### Freshness\n\n"
           "Every register, and the read-back of all six parameters, is read\n"
           "together on one 60-second tick in the main loop, and this endpoint\n"
           "serves that cache rather than touching the bus itself: the loop\n"
           "task is the only task allowed on the I2C bus, because two\n"
           "interleaved transactions on one bus would be a fault with no\n"
           "witness. The same rule covers the writes, and matters more for\n"
           "them — a block transfer interleaved by another task does not fail\n"
           "cleanly, it writes half a parameter under a checksum computed over\n"
           "the other half. There is no path from a request to the bus in this\n"
           "skill. `age_s` is how many seconds ago the pass ran, and `fresh` is\n"
           "false when nothing has been read yet. Capacities, security modes\n"
           "and data-memory values change on the order of a charge cycle or a\n"
           "reset, so the delay costs nothing.\n\n"
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
        GaugeConfigState cfg;
        portENTER_CRITICAL(&battery_mux);
        g = battery_gauge;
        cfg = gauge_cfg;
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
        /* This said false until the commit that added gauge_config below, and
           a caller may well have been relying on it. It is now true and means
           what it says: six data-memory parameters, at boot, and nothing else
           — every register in this response is still read and never written. */
        doc["writes"] = true;
        doc["writes_scope"] = "six data-memory charge parameters at boot; see "
                              "gauge_config. No standard command reported here "
                              "is ever written.";

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

        /* Signed, positive into the cell. These are the readings the taper
           argument below is made of, and until this commit the firmware had
           never looked at either — every claim about what current the gauge
           does or does not see was inference from capacities. */
        JsonObject cur = doc["current_ma"].to<JsonObject>();
        cur["ok"] = g.current_ok;
        if (g.current_ok) cur["value"] = g.current_ma;

        JsonObject avg = doc["average_current_ma"].to<JsonObject>();
        avg["ok"] = g.avg_current_ok;
        if (g.avg_current_ok) avg["value"] = g.avg_current_ma;

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

        /*
         * GaugingStatus, MAC subcommand 0x0056.
         *
         * `ok` false here means something specific and stronger than a failed
         * bus transaction: the part did not echo the subcommand back, so
         * whatever arrived is not known to be an answer to this question. No
         * flag is decoded in that case. A word of sixteen plausible false
         * flags, printed off a frame that belonged to something else, is the
         * exact failure the ITPOR line further down refuses to commit.
         */
        JsonObject gs = doc["gauging_status"].to<JsonObject>();
        gs["ok"] = g.gauging_ok;
        if (g.gauging_ok) {
            snprintf(hex, sizeof(hex), "0x%04X", g.gauging_status);
            gs["raw"] = String(hex);
            gs["fc"]   = bq_flag(g.gauging_status, BQ_GS_FC);
            gs["fd"]   = bq_flag(g.gauging_status, BQ_GS_FD);
            gs["tc"]   = bq_flag(g.gauging_status, BQ_GS_TC);
            gs["td"]   = bq_flag(g.gauging_status, BQ_GS_TD);
            gs["dsg"]  = bq_flag(g.gauging_status, BQ_GS_DSG);
            gs["cf"]   = bq_flag(g.gauging_status, BQ_GS_CF);
            gs["edv"]  = bq_flag(g.gauging_status, BQ_GS_EDV);
            gs["edv1"] = bq_flag(g.gauging_status, BQ_GS_EDV1);
            gs["edv2"] = bq_flag(g.gauging_status, BQ_GS_EDV2);
            gs["vdq"]  = bq_flag(g.gauging_status, BQ_GS_VDQ);
            gs["fccx"] = bq_flag(g.gauging_status, BQ_GS_FCCX);
            /* Said next to the bits rather than only in describe(), because the
               reader most likely to open this object is one who has just been
               told there are two FC bits and is about to conclude something
               from the pair. */
            gs["note"] = "fc/tc/td/fd and vdq are the SAME state as "
                         "battery_status.fc/tca/tda/fd and operation_status.vdq "
                         "(TRM SLUUBD4A Table 2-4: each 'is identical to' its "
                         "counterpart), not a second opinion on it. What is "
                         "only here: cf, dsg, edv, edv1.";
        } else {
            gs["note"] = "the gauge did not echo subcommand 0x0056 back, so "
                         "nothing is decoded; a frame that cannot be shown to "
                         "answer this question is not reported as flags.";
        }

        /* CONTROL_STATUS, read straight off register 0x00 with nothing written
           — TRM 2.2 makes the 0x0000 subcommand unnecessary. Three flags and a
           three-bit field is the whole of it; the rest of the word is
           reserved, and is in `raw`. */
        JsonObject cs = doc["control_status"].to<JsonObject>();
        cs["ok"] = g.control_ok;
        if (g.control_ok) {
            snprintf(hex, sizeof(hex), "0x%04X", g.control_status);
            cs["raw"] = String(hex);
            cs["cca"]     = bq_flag(g.control_status, BQ_CS_CCA);
            cs["bca"]     = bq_flag(g.control_status, BQ_CS_BCA);
            cs["snooze"]  = bq_flag(g.control_status, BQ_CS_SNOOZE);
            cs["batt_id"] = bq_batt_id(g.control_status);
        }

        /*
         * The three conditions FC actually waits on, with this build's own
         * numbers substituted into them.
         *
         * Served because the two status words above answer "did it terminate"
         * and this answers "what would have to be true for it to". Every figure
         * comes from the shipping table or from TRM 4.4.1; nothing here is a
         * recommendation and nothing here is written to the part.
         */
        JsonObject term = doc["charge_termination"].to<JsonObject>();
        term["source"] = "TRM SLUUBD4A section 4.4.1, all three required";
        term["window_s"] = BQ_TAPER_WINDOW_S;
        term["windows_required"] = BQ_TAPER_WINDOWS;
        /* Named for the whole qualification and not for one window, because
           TRM 4.4.1 accumulates it "during the same two periods". The old key
           said per_window and was wrong by a factor of two. */
        term["min_capacity_per_qualification_uah"] = BQ_TAPER_MIN_CAP_UAH;
        /* The floor condition 2 implies, on its own. In microamps: 11250 uA is
           11.25 mA and does not round. */
        term["capacity_floor_ua"] = bq_taper_floor_ua();
        /* Condition 1 names AverageCurrent(), which lags a decaying taper, and
           the two periods must be consecutive — so entering the band part-way
           through one costs the rest of it. Both are easy to lose when the
           conditions are paraphrased, so both are stated rather than implied. */
        term["measured_on"] = "AverageCurrent(), not Current(): TRM 4.4.1 "
                              "condition 1 tests the filtered value, which "
                              "lags the instantaneous one during a taper";
        term["worst_case_seconds"] = BQ_TAPER_WINDOW_S * (BQ_TAPER_WINDOWS + 1);
        term["timing_note"] = "the two periods must be CONSECUTIVE, so the "
                              "best case is window_s * windows_required and "
                              "the worst adds one more window for a band "
                              "entered part-way through one";

        const GaugeParam *taper = gauge_param_at(BQ_DM_TAPER_CURRENT);
        const GaugeParam *cvolt = gauge_param_at(BQ_DM_CHARGING_VOLTAGE);
        const GaugeParam *tvolt = gauge_param_at(BQ_DM_CEDV_TAPER_VOLTAGE);
        const GaugeParam *quit  = gauge_param_at(BQ_DM_QUIT_CURRENT);

        /* And the OTHER floor, which is a different mechanism and on this
           device is the one that actually binds: below Quit Current the gauge
           leaves CHARGE mode after Chg Relax Time, and 60 s is less than the
           80 s the qualification needs. Reported alongside rather than instead,
           because a reader who only sees the larger number cannot tell which
           of the two rules produced it. */
        term["chg_relax_time_s"] = BQ_CHG_RELAX_TIME_S;
        term["relax_exit_pre_empts_taper"] = bq_relax_pre_empts_taper();
        if (quit) {
            term["quit_current_floor_ua"] = (long)quit->value * 1000L;
            term["current_floor_ua"] = bq_termination_floor_ua(quit->value);
            term["floor_set_by"] =
                bq_termination_floor_ua(quit->value) > bq_taper_floor_ua()
                    ? "QuitCurrent: below it the gauge leaves CHARGE mode after "
                      "chg_relax_time_s, which is shorter than the two windows "
                      "the qualification needs"
                    : "the 0.25 mAh per window capacity condition";
        }

        if (taper) term["current_ceiling_ma"] = taper->value;
        if (cvolt && tvolt) term["min_voltage_mv"] = cvolt->value - tvolt->value;
        term["condition"] = "AverageCurrent() must stay between "
                            "current_floor_ua and current_ceiling_ma for "
                            "windows_required consecutive window_s periods "
                            "while voltage_mv exceeds min_voltage_mv. A charge "
                            "that leaves the ceiling and reaches zero without "
                            "spending that time in between passes through the "
                            "band without ever being in it.";
        /* The vendor guidance, now acted on rather than merely cited. See the
           comment on the TaperCurrent row of gauge_config[] for the tolerance
           argument that picked 100 over the 90 first proposed. */
        term["ti_guidance"] = "TI SLUA777 3.2, SLUA903 2.2 and SLUA917, in "
                              "identical words: set the gauge's taper current "
                              "SLIGHTLY HIGHER than the charger's taper "
                              "threshold, so the gauge sees full before the "
                              "charger cuts off. Their worked example is 70 mA "
                              "for a 50 mA charger, a ratio of 1.4. They also "
                              "advise taper current below C/10, which for this "
                              "1300 mAh cell is 130 mA. Both are satisfied "
                              "here: the charger was lowered to terminate at "
                              "64 mA and the gauge's taper is 100 mA, a ratio "
                              "of 1.56 and comfortably under 130. The larger "
                              "ratio is deliberate — TI size theirs against a "
                              "charger held to about 10%, and the BQ25896's "
                              "termination accuracy is specified only at "
                              "256 mA, with no figure at all at 64 mA.";
        /* Served because it is the question a reader asks next, and the honest
           answer is not the reassuring one. */
        term["fc_sets_but_fcc_may_not_learn"] =
            "setting BatteryStatus[FC] is what this configuration is for and "
            "is the expected outcome. It does NOT follow that "
            "FullChargeCapacity is learned down from the design capacity: that "
            "needs a qualified discharge, whose end-of-discharge current must "
            "reach 3C/32 = 122 mA on this cell, and a device idling at "
            "single-digit milliamps may never produce one. See "
            "gauge_diagnostics for the CSYNC bit of GaugingConfiguration "
            "(0x929B), which would set RemainingCapacity = FullChargeCapacity "
            "at termination and make the percentage read 100 without any "
            "learning.";

        /* Said in the payload as well as in describe(): whoever is chasing the
           wrong percentage will read this response long before they read the
           manual, and looking for a flag that does not exist is exactly the
           hour this line is here to save. */
        doc["itpor"] = "not readable on the BQ27220: no such bit in "
                       "BatteryStatus, OperationStatus, CONTROL_STATUS or "
                       "GaugingStatus (TRM SLUUBD4A). See the raw words.";

        /*
         * What was written, whether it took, and whether it is still there.
         *
         * This block is the reason the file's contract changed, so it reports
         * enough to audit the change rather than enough to reassure: the
         * security state before and after, whether CONFIG UPDATE was actually
         * entered and left, and per parameter the address, the value, the boot
         * read-back and the live read-back. A capability an agent cannot
         * discover does not exist here.
         */
        JsonObject gc = doc["gauge_config"].to<JsonObject>();
        gc["attempted"] = cfg.attempted;
        /* The summary, and therefore the one that is RE-DERIVED rather than
           remembered: false the moment a parameter stops reading back, not
           merely when the boot write failed. */
        gc["applied"] = g.verdict.applied;
        gc["applied_at_boot"] = cfg.applied;
        gc["still_matches"] = g.verdict.config_still_live;
        gc["readback_ok"] = g.dm_read_ok;
        /* False on every boot after the first, and that is the healthy case:
           the gauge keeps its RAM across a reflash, so there was nothing to
           write. It is reported because "wrote 0 of 6" would otherwise read
           like a failure. */
        gc["needed_writing"] = cfg.needed;
        gc["full_access"] = cfg.access_ok;
        gc["sec_before"] = bq_sec_name((uint16_t)(cfg.sec_before << BQ_OP_SEC_SHIFT));
        gc["sec_after"] = bq_sec_name((uint16_t)(cfg.sec_after << BQ_OP_SEC_SHIFT));
        gc["cfgupdate_entered"] = cfg.cfgupdate_entered;
        gc["cfgupdate_exited"] = cfg.cfgupdate_exited;
        gc["writes_ok"] = cfg.writes_ok;
        gc["writes_expected"] = GAUGE_CONFIG_COUNT;
        gc["reapplied"] = cfg.reapplied;
        gc["reapply_capped"] = cfg.reapply_capped;
        gc["reapply_max"] = GAUGE_REAPPLY_MAX;
        gc["persistence"] = "RAM, not flash: the gauge loses these on a "
                            "power-on reset and they are re-applied at every "
                            "boot. Making them permanent needs 7.4 V on GPOUT "
                            "and TI tooling, which is not reachable from here.";

        JsonArray params = gc["params"].to<JsonArray>();
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            JsonObject p = params.add<JsonObject>();
            p["name"] = gauge_config[i].name;
            snprintf(hex, sizeof(hex), "0x%04X", gauge_config[i].addr);
            p["address"] = String(hex);
            p["value"] = gauge_config[i].value;
            p["units"] = gauge_config[i].units;
            if (cfg.verify_read_ok) p["readback"] = cfg.verified[i];
            p["matches"] = cfg.verify_read_ok &&
                           ((cfg.verify_mask & (1u << i)) == 0);
            if (g.dm_read_ok) p["live"] = g.dm_live[i];
            p["still_matches"] = g.dm_read_ok &&
                                 ((g.verdict.live_mask & (1u << i)) == 0);
            p["why"] = gauge_config[i].why;
        }

        /*
         * The read-only diagnostics, as HEX and undecoded.
         *
         * Hex because the question these are here to answer is what the bits
         * ARE. For GaugingConfiguration the VALUE is what TRM SLUUBD4A
         * disagrees with itself about — Table 3-2 says 0x102A, Table 4-11
         * prints its per-bit defaults as zero — and CSYNC is bit 1 of its low
         * byte, so a decoded answer would look authoritative while resting on
         * the half of the manual somebody picked. The raw word is checkable
         * against either. SOCFlagConfigA is reported the same way for
         * consistency rather than because its default is in doubt; it is not.
         *
         * Outside gauge_config so that nothing here can be mistaken for
         * something this firmware wrote. Nothing is written to these addresses
         * and 0x929B is refused by name in the write guard.
         */
        JsonObject diag = doc["gauge_diagnostics"].to<JsonObject>();
        diag["written"] = false;
        diag["encoding"] = "raw hex, not decoded: GaugingConfiguration's ROM "
                           "default is the open question these reads exist to "
                           "settle — TRM Table 3-2 says 0x102A, Table 4-11 "
                           "prints zeros — and CSYNC is bit 1 of its low byte, "
                           "so decoding it here would assume the answer";
        JsonArray dregs = diag["registers"].to<JsonArray>();
        for (int i = 0; i < GAUGE_DIAG_COUNT; i++) {
            JsonObject d = dregs.add<JsonObject>();
            d["name"] = gauge_diagnostics[i].name;
            snprintf(hex, sizeof(hex), "0x%04X", gauge_diagnostics[i].addr);
            d["address"] = String(hex);
            d["read_ok"] = g.dm_diag_ok[i];
            if (g.dm_diag_ok[i]) {
                snprintf(hex, sizeof(hex), "0x%04X", (unsigned)(uint16_t)g.dm_diag[i]);
                d["value"] = String(hex);
            }
            d["question"] = gauge_diagnostics[i].question;
        }

        notify_send_json(req, 200, doc);
    });
}

static const Skill battery_skill = {
    .name = "battery",
    /* 0.4.0: this one changes what is WRITTEN, which 0.3.0 explicitly did not.
       TaperCurrent goes out as 100 mA instead of 128, so the gauge's idea of a
       finished charge is a different number than it was, and the response also
       gains gauge_diagnostics. Either alone would earn the minor bump; a caller
       pinned to `params[TaperCurrent].value == 128` is looking at a real
       behaviour change and should see the version move. */
    .version = "0.4.0",
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
