#!/usr/bin/env bash
#
# Host-side regression test for the BQ27220 decode and the data-memory
# configuration in src/skills/battery.cpp.
#
# Why this exists: GET /battery is a diagnostic, and a diagnostic that reports
# confidently and wrongly is worse than no diagnostic. The whole point of the
# endpoint is that somebody will read "sec: sealed" or "fc: true" off it and
# decide what to write to a live fuel gauge on the strength of it. A shift by
# one bit in a mask does not fail a build, does not throw and does not look
# broken — it looks like an answer.
#
# That was the original argument, and the writes have made it sharper rather
# than replacing it. Everything in the data-memory path fails SILENTLY when it
# is wrong:
#
#   - A wrong checksum is not signalled on the bus. The gauge declines to commit
#     the block and keeps the old value, which is indistinguishable from a part
#     that refused the parameter.
#   - A wrong address is not signalled either. It writes a different parameter,
#     successfully, and 0x929F (DesignCapacity) and 0x92A3 (Design Voltage) are
#     each one keystroke from an address in the table.
#   - The address goes out little-endian and the value big-endian. Swap either
#     and the write succeeds, checksums, and reads back consistent with itself.
#
# None of that has a failure signal to test against, so what is tested is the
# arithmetic that produces it. The pieces below are pure computation and are
# therefore the pieces a host can hold to account:
#
#   1. The two-byte little-endian word assembly. Get it backwards and 4200 mV
#      reads as 26640 mV — which the voltage gate then rejects, so the panel
#      silently stops updating and nobody is told why.
#   2. The status bit fields. Every mask below is checked against the position
#      the TRM (SLUUBD4A, Tables 2-6 and 2-7) puts it at, and every named flag
#      is checked to respond to ITS OWN bit and to no other.
#   3. The data-memory block builder, byte for byte, both endiannesses.
#   4. The checksum and length arithmetic, against the TRM's OWN WORKED EXAMPLE
#      — address 0x9221 with one data byte, checksum 0x4C, length 5 — which is
#      an oracle from the primary source rather than a restatement of the code.
#   5. The address allowlist, swept across all 65536 addresses: exactly six are
#      writable and every one of them is in the table.
#   6. The parameter table itself, against the addresses TRM Table 3-2 gives.
#   7. The verify mask and the verdict that decides what the endpoint claims.
#   8. The one fact that spans two files: the gauge's TaperCurrent equals the
#      charger's termination current. See the charger object built below.
#
# WHAT THIS DOES NOT COVER, said plainly rather than papered over: the I2C
# transactions, and that is a larger uncovered share than it was before the
# writes. bq27220_read16(), bq27220_dm_write(), bq27220_dm_read(),
# gauge_open_access() and gauge_wait_cfgupdate() are calls into Arduino's
# TwoWire wrapped around timing this host has no equivalent for — a NAK, a
# missing pull-up, a second task on the bus, a part that wanted another 200 ms.
# Nothing below simulates any of it. Neither is gauge_configure()'s own control
# flow: the read-first rule, the CONFIG UPDATE bracket, and the decision not to
# write when the part already holds the table all sit above the slice markers on
# the far side of Wire, and reaching them from the host would mean rearranging
# the shipping source to suit the test.
#
# The only proof that half of this skill works is a reading taken off the
# device, and it is a specific one: after a completed charge,
# `battery_status.fc` must go true. It was false on a full cell before this.
#
# The endpoint layer is not here either — tools/test_routes.sh gates the exact
# matcher and the body-handler wiring, which is the failure mode that layer
# actually has.
#
# Nor is battery_read()'s at_boot gate: the rule that only the boot pass may set
# hw.has_battery, so that a bus glitch during a later refresh cannot make a
# device without a fuel gauge start claiming one. It is left uncovered on
# purpose and named here so that nobody reads this file's green as covering it.
#
# The logic is sliced straight out of the shipping source between its
# `host-test:begin gauge` and `host-test:end` markers, the same way
# tools/test_progress.sh and tools/test_voice_url.sh slice theirs, so this runs
# the real implementation rather than a copy that could drift.
#
# Usage: tools/test_battery.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src/skills/battery.cpp"
charger_src="$here/../src/skills/charger.cpp"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -f "$src" ] || { echo "cannot find $src"; exit 1; }
[ -f "$charger_src" ] || { echo "cannot find $charger_src"; exit 1; }

slice() {
    awk -v tag="$2" '
        $0 ~ ("host-test:begin " tag) { grab = 1; next }
        grab && /host-test:end/       { grab = 0; next }
        grab                          { print }
    ' "$1"
}

{
    cat <<'PRELUDE'
/* Generated by tools/test_battery.sh — do not edit. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

PRELUDE
    slice "$src" gauge
    cat <<'MAIN'

/* ---- test scaffolding ---- */

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
    else       { printf("  ok:   %s\n", what); }
}

static void eq_int(int got, int want, const char *what) {
    if (got != want) {
        printf("  FAIL: %s — got %d, wanted %d\n", what, got, want);
        failures++;
    } else {
        printf("  ok:   %s = %d\n", what, got);
    }
}

static void eq_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %s — got \"%s\", wanted \"%s\"\n", what, got, want);
        failures++;
    } else {
        printf("  ok:   %s = \"%s\"\n", what, got);
    }
}

/* One named flag, with the bit the datasheet puts it at. The position is
   written here as a NUMBER rather than as the macro, so that this table is the
   datasheet and the source is the thing under test — comparing a macro to
   itself would pass whatever it was changed to. */
struct FlagCase {
    const char *name;
    uint16_t mask;
    int bit;
};

static const FlagCase op_flags[] = {
    {"CALMD",     BQ_OP_CALMD,      0},
    {"EDV2",      BQ_OP_EDV2,       3},
    {"VDQ",       BQ_OP_VDQ,        4},
    {"INITCOMP",  BQ_OP_INITCOMP,   5},
    {"SMTH",      BQ_OP_SMTH,       6},
    {"BTPINT",    BQ_OP_BTPINT,     7},
    {"CFGUPDATE", BQ_OP_CFGUPDATE, 10},
};

static const FlagCase batt_flags[] = {
    {"DSG",      BQ_BS_DSG,       0},
    {"SYSDWN",   BQ_BS_SYSDWN,    1},
    {"TDA",      BQ_BS_TDA,       2},
    {"BATTPRES", BQ_BS_BATTPRES,  3},
    {"AUTH_GD",  BQ_BS_AUTH_GD,   4},
    {"OCVGD",    BQ_BS_OCVGD,     5},
    {"TCA",      BQ_BS_TCA,       6},
    {"CHGINH",   BQ_BS_CHGINH,    8},
    {"FC",       BQ_BS_FC,        9},
    {"OTD",      BQ_BS_OTD,      10},
    {"OTC",      BQ_BS_OTC,      11},
    {"SLEEP",    BQ_BS_SLEEP,    12},
    {"OCVFAIL",  BQ_BS_OCVFAIL,  13},
    {"OCVCOMP",  BQ_BS_OCVCOMP,  14},
    {"FD",       BQ_BS_FD,       15},
};

/*
 * GaugingStatus, TRM SLUUBD4A Table 2-4. Bit numbers are of the assembled
 * 16-bit word, so the table's "high byte bit 7" is 15 here — the conversion
 * done by hand from the datasheet, which is the point.
 *
 * The four reserved positions (1, 3, 4, 12) are absent on purpose and the body
 * of the test asserts their absence.
 */
static const FlagCase gauging_flags[] = {
    {"FD",   BQ_GS_FD,    0},
    {"FC",   BQ_GS_FC,    1},
    {"TD",   BQ_GS_TD,    2},
    {"TC",   BQ_GS_TC,    3},
    {"EDV",  BQ_GS_EDV,   5},
    {"DSG",  BQ_GS_DSG,   6},
    {"CF",   BQ_GS_CF,    7},
    {"FCCX", BQ_GS_FCCX, 10},
    {"EDV1", BQ_GS_EDV1, 13},
    {"EDV2", BQ_GS_EDV2, 14},
    {"VDQ",  BQ_GS_VDQ,  15},
};

/* CONTROL_STATUS, Table 2-3. Three flags; BATT_ID is a three-bit field and is
   checked separately, because flag_table's "reads false off all fifteen other
   bits" rule is exactly what a field does not obey. */
static const FlagCase control_flags[] = {
    {"SNOOZE", BQ_CS_SNOOZE, 3},
    {"BCA",    BQ_CS_BCA,    4},
    {"CCA",    BQ_CS_CCA,    5},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/*
 * The six parameters, with the address and value written out as LITERALS taken
 * from TRM SLUUBD4A Table 3-2 — not as the macros the source uses, for the same
 * reason FlagCase above spells out bit positions as numbers. Comparing a macro
 * to itself passes whatever it is changed to.
 *
 * Looked up by NAME rather than by index, so reordering gauge_config[] (which
 * the source explicitly says is free, unlike the charger's table) does not fail
 * this, while changing an address or a value does.
 */
struct ParamCase {
    const char *name;
    uint16_t addr;
    int16_t value;
    uint8_t checksum;   /* 0xFF - (addr_lo + addr_hi + val_hi + val_lo), by hand */
};

static const ParamCase trm_params[] = {
    /* 0x01+0x92+0x00+0x80 = 0x113 -> 0x13; 0xFF-0x13 = 0xEC */
    {"TaperCurrent",                 0x9201,  128, 0xEC},
    /* 0xFB+0x91+0x02+0x00 = 0x18E -> 0x8E; 0xFF-0x8E = 0x71 */
    {"ChargingCurrent",              0x91FB,  512, 0x71},
    /* 0xFD+0x91+0x10+0x70 = 0x20E -> 0x0E; 0xFF-0x0E = 0xF1 */
    {"ChargingVoltage",              0x91FD, 4208, 0xF1},
    /* 0xA5+0x92+0x00+0x64 = 0x19B -> 0x9B; 0xFF-0x9B = 0x64 */
    {"CEDVChargeTerminationVoltage", 0x92A5,  100, 0x64},
    /* 0x2A+0x92+0x00+0x4B = 0x107 -> 0x07; 0xFF-0x07 = 0xF8 */
    {"ChargeDetectThreshold",        0x922A,   75, 0xF8},
    /* 0x2C+0x92+0x00+0x28 = 0xE6;         0xFF-0xE6 = 0x19 */
    {"QuitCurrent",                  0x922C,   40, 0x19},
};

/* The entry in the shipping table with this name, or NULL. */
static const GaugeParam *param_named(const char *name) {
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (strcmp(gauge_config[i].name, name) == 0) return &gauge_config[i];
    }
    return NULL;
}

/* Its index, for the bit it owns in the verify mask. */
static int param_index(const char *name) {
    for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
        if (strcmp(gauge_config[i].name, name) == 0) return i;
    }
    return -1;
}

/* Three facts out of charger.cpp's own configuration table, compiled into a
   separate object from the same shipping source. See the build below. */
extern "C" int charger_table_iterm_ma(void);
extern "C" int charger_table_ichg_ma(void);
extern "C" int charger_table_vreg_mv(void);

/*
 * bq_soc_from_capacity() with its arguments laundered through volatiles, and
 * every capacity check below goes through this rather than calling directly.
 *
 * Not decoration. Removing the function's `full_mah == 0` guard does not
 * produce a wrong answer, it produces UNDEFINED BEHAVIOUR, and at -O2 the
 * compiler is entitled to resolve that however it likes — measured here, it
 * folded the comparison in the test's favour and the check for the guard
 * PASSED on a build that would divide by zero on the device. A check that
 * cannot fail when the thing it names is deleted is not a check.
 *
 * The volatiles take the constants away from the optimiser, so the division
 * genuinely happens: a trap on x86, a zero result on ARM, and a failure on
 * either. Verified by deleting the guard — red with this in place, green
 * without it.
 */
static int soc_from(uint16_t remaining, uint16_t full) {
    volatile uint16_t r = remaining, f = full;
    return bq_soc_from_capacity(r, f);
}

/* Each mask sits where the datasheet says, responds to its own bit, and stays
   clear for every other bit in the word. The last part is what catches a mask
   that was widened or duplicated: a flag reading true off somebody else's bit
   is the exact defect this endpoint must not have. */
static void flag_table(const FlagCase *cases, int n, const char *word_name) {
    char what[128];

    for (int i = 0; i < n; i++) {
        snprintf(what, sizeof(what), "%s.%s is bit %d", word_name, cases[i].name, cases[i].bit);
        check(cases[i].mask == (uint16_t)(1u << cases[i].bit), what);

        snprintf(what, sizeof(what), "%s.%s reads true off its own bit", word_name, cases[i].name);
        check(bq_flag((uint16_t)(1u << cases[i].bit), cases[i].mask), what);

        int leaked = -1;
        for (int b = 0; b < 16; b++) {
            if (b == cases[i].bit) continue;
            if (bq_flag((uint16_t)(1u << b), cases[i].mask)) { leaked = b; break; }
        }
        if (leaked >= 0) {
            printf("  FAIL: %s.%s also reads true off bit %d\n",
                   word_name, cases[i].name, leaked);
            failures++;
        } else {
            printf("  ok:   %s.%s reads false off all fifteen other bits\n",
                   word_name, cases[i].name);
        }
    }

    /* No two flags claim the same bit — the copy-paste mistake a table this
       long invites.

       Stated honestly: this one never fails ALONE. Duplicating a mask also
       moves it off the position the loop above checks, so that check fires
       too, and no mutation of the source has been found that trips this and
       nothing else. It is kept because it names the failure in the words a
       reader would use, not because it adds detection. */
    int clash = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (cases[i].mask == cases[j].mask) clash = 1;
    snprintf(what, sizeof(what), "no two %s flags claim the same bit", word_name);
    check(!clash, what);
}

int main(void) {
    /* Any hang is a failure, not a hung test run. */
    alarm(60);

    printf("the wire order\n");
    {
        /* 0x1068 = 4200 mV, a full cell — the reading the whole ticket is
           about, and the one that becomes 26640 if the bytes are swapped. */
        eq_int(bq_word(0x68, 0x10), 4200, "low byte first: 0x68,0x10 is 4200 mV");
        eq_int(bq_word(0x10, 0x68), 26640, "and the other way round is not");
        eq_int(bq_word(0x00, 0x00), 0, "both bytes zero");
        eq_int(bq_word(0xFF, 0xFF), 65535, "both bytes set, and no sign extension");
        eq_int(bq_word(0xFF, 0x00), 255, "a high byte of zero keeps the low byte");
        eq_int(bq_word(0x00, 0x01), 256, "a low byte of zero shifts the high one");
    }

    printf("the voltage gate, bounds exclusive as they always were\n");
    {
        check(bq_voltage_ok(3950), "3.95 V, one of the two live readings");
        check(bq_voltage_ok(4200), "4.20 V, the other one");
        check(bq_voltage_ok(2001), "2001 mV is inside");
        check(bq_voltage_ok(5999), "5999 mV is inside");
        check(!bq_voltage_ok(2000), "2000 mV is out: the bound is exclusive");
        check(!bq_voltage_ok(6000), "6000 mV is out: so is that one");
        check(!bq_voltage_ok(0), "a dead bus reads zero and is refused");
        check(!bq_voltage_ok(26640), "and a byte-swapped 4.20 V is refused too");
    }

    printf("the state-of-charge gate\n");
    {
        check(bq_soc_ok(0), "0% is a value");
        check(bq_soc_ok(52), "52%, as reported at 3.95 V");
        check(bq_soc_ok(76), "76%, as reported at a full 4.20 V");
        check(bq_soc_ok(100), "100% is inside: this bound is inclusive");
        check(!bq_soc_ok(101), "101% is not a percentage");
        check(!bq_soc_ok(65535), "nor is an all-ones read");
    }

    printf("OperationStatus flags (TRM Table 2-7)\n");
    {
        flag_table(op_flags, COUNT(op_flags), "op");

        /* Named separately because it is the one whose position is easiest to
           get wrong: the table puts CFGUPDATE at HIGH byte bit 2, which is bit
           10 of the assembled word and not bit 2 and not bit 15. */
        check(bq_flag(0x0400, BQ_OP_CFGUPDATE), "CFGUPDATE responds to 0x0400");
        check(!bq_flag(0x0004, BQ_OP_CFGUPDATE), "and not to 0x0004, the low-byte bit 2");
        check(!bq_flag(0x8000, BQ_OP_CFGUPDATE), "and not to 0x8000, the top of the word");

        /* The bits SEC occupies belong to no named flag: it is a two-bit field
           and a flag reading off one half of it would be nonsense. */
        for (int i = 0; i < COUNT(op_flags); i++) {
            check(!bq_flag(0x0002, op_flags[i].mask) && !bq_flag(0x0004, op_flags[i].mask),
                  "no op flag claims a SEC bit");
        }
    }

    printf("the security mode\n");
    {
        eq_str(bq_sec_name(0x0006), "sealed",   "SEC 11");
        eq_str(bq_sec_name(0x0004), "unsealed", "SEC 10");
        eq_str(bq_sec_name(0x0002), "full",     "SEC 01");
        eq_str(bq_sec_name(0x0000), "unknown",  "SEC 00, which the TRM does not define");

        eq_int(bq_sec_bits(0x0006), 3, "the raw bits behind sealed");
        eq_int(bq_sec_bits(0x0004), 2, "the raw bits behind unsealed");
        eq_int(bq_sec_bits(0x0002), 1, "the raw bits behind full");
        eq_int(bq_sec_bits(0x0000), 0, "the raw bits behind unknown");

        /* The field is read out of a whole word, so every other bit must be
           ignored. 0xFFF9 is "everything set except SEC", which must still
           read as SEC 00. */
        eq_str(bq_sec_name(0xFFF9), "unknown", "SEC ignores every bit outside the field");
        eq_str(bq_sec_name(0xFFFF), "sealed",  "and reads the field when all bits are set");
        eq_int(bq_sec_bits(0xFFF9), 0, "and so do the raw bits");

        /* A sealed gauge with initialisation done and no CONFIG UPDATE in
           progress. NOT the state this device is in — it reads SEC 10,
           unsealed, confirmed off the live gauge — but sealed is the state
           that would block commit 2 from writing anything, so it is the one
           worth being sure the decode does not report by accident. It must
           come out as all three of those things at once rather than one of
           them swallowing the others. */
        uint16_t sealed_idle = 0x0006 | BQ_OP_INITCOMP;
        eq_str(bq_sec_name(sealed_idle), "sealed", "a sealed, initialised gauge");
        check(bq_flag(sealed_idle, BQ_OP_INITCOMP), "reports initialisation complete");
        check(!bq_flag(sealed_idle, BQ_OP_CFGUPDATE), "and is not in CONFIG UPDATE");
        check(!bq_flag(sealed_idle, BQ_OP_VDQ), "and has qualified no discharge");
    }

    printf("BatteryStatus flags (TRM Table 2-6)\n");
    {
        flag_table(batt_flags, COUNT(batt_flags), "batt");

        /* FC is what says "this pack is full", which is the claim the reported
           76% contradicts. High byte bit 1 is bit 9 of the word. */
        check(bq_flag(0x0200, BQ_BS_FC), "FC responds to 0x0200");
        check(!bq_flag(0x0002, BQ_BS_FC), "and not to 0x0002, the low-byte bit 1");
        check(!bq_flag(0x0000, BQ_BS_FC), "and an empty word is not full charge");

        /* Low-byte bit 7 is reserved on this part, and is where somebody
           looking for ITPOR would put it. Nothing may claim it. */
        int claimed = 0;
        for (int i = 0; i < COUNT(batt_flags); i++)
            if (bq_flag(0x0080, batt_flags[i].mask)) claimed = 1;
        check(!claimed, "the reserved low-byte bit 7 is claimed by no flag");
    }

    printf("state of charge derived from the capacities\n");
    {
        /* The pack this device is believed to carry. */
        eq_int(soc_from(676, 1300), 52, "676 of 1300 mAh is 52%");
        eq_int(soc_from(988, 1300), 76, "988 of 1300 mAh is 76%");
        eq_int(soc_from(1300, 1300), 100, "a full pack is 100%");
        eq_int(soc_from(0, 1300), 0, "an empty one is 0%");

        /* Rounds to nearest, so that a single point of disagreement with the
           gauge's own percentage is evidence rather than this function's
           truncation. */
        eq_int(soc_from(1, 3), 33, "1 of 3 rounds down to 33");
        eq_int(soc_from(2, 3), 67, "2 of 3 rounds up to 67");
        eq_int(soc_from(5, 1000), 1, "5 of 1000 rounds up to 1, not down to 0");
        eq_int(soc_from(4, 1000), 0, "4 of 1000 rounds down to 0");

        /* No overflow: the intermediate is remaining * 100, which leaves a
           uint16 at 6553 mAh and does not leave the uint32 the code uses. */
        eq_int(soc_from(65535, 65535), 100, "the largest pair either register can hold");
        eq_int(soc_from(65535, 65534), 100, "and one just over it does not wrap");

        /* A gauge that has never established a full charge divides by zero.
           That is a finding, and it is reported as one rather than crashed on. */
        eq_int(soc_from(676, 0), -1, "a FullChargeCapacity of zero is refused");
        eq_int(soc_from(0, 0), -1, "and so is a pair of zeroes");

        /* The reason the endpoint reports this figure at all: it is what tells
           a wrong FullChargeCapacity apart from a wrong RemainingCapacity. The
           popular diagnosis for this family — a 3000 mAh default against a
           1300 mAh pack — caps a full pack here, and the device reports 76%,
           which is how that theory was ruled out. */
        eq_int(soc_from(1300, 3000), 43, "a full 1300 mAh pack against a 3000 mAh default");
        check(soc_from(1300, 3000) < 76,
              "which is below the 76% the device actually reports");
    }

    /* ================= the data-memory configuration ================= */

    printf("the data-memory block, byte for byte\n");
    {
        uint8_t blk[BQ_DM_BLOCK_BYTES];

        /*
         * ChargingVoltage is the case worth leading with because all four of
         * its bytes are different: address 0x91FD, value 4208 = 0x1070. Any
         * swap of either half moves a byte that no other check would catch.
         *
         * Verified by swapping each half of bq_dm_block() in turn. Writing the
         * address high byte first turns blk[0] into 0x91 and reddens ten checks
         * here, starting with "the address goes out LOW byte first". Writing
         * the value low byte first turns blk[2] into 0x70 and reddens twelve,
         * starting with "the value goes out HIGH byte first" — twelve rather
         * than four because the round trip and the checksums below are computed
         * over the same block. Restored, green both times.
         */
        eq_int(bq_dm_block(0x91FD, 4208, blk), 4, "a block is four bytes");
        eq_int(blk[0], 0xFD, "the address goes out LOW byte first");
        eq_int(blk[1], 0x91, "then its high byte");
        eq_int(blk[2], 0x10, "the value goes out HIGH byte first");
        eq_int(blk[3], 0x70, "then its low byte");

        /* The two orders are OPPOSITE, which is the part that reads like a bug
           and is not. Said as its own check so that "surely both are
           little-endian" fails here rather than on the device. */
        bq_dm_block(0x91FB, 512, blk);
        eq_int(blk[0], 0xFB, "0x91FB: address low byte");
        eq_int(blk[1], 0x91, "0x91FB: address high byte");
        eq_int(blk[2], 0x02, "512 = 0x0200: value high byte first");
        eq_int(blk[3], 0x00, "and its low byte second — NOT the address order");

        /* A value whose two bytes are equal proves nothing about order, so the
           table's own 128 is checked for what it does prove: the low byte is
           where the magnitude went. 128 little-endian would be 0x80,0x00 and
           the gauge would store 32768. */
        bq_dm_block(0x9201, 128, blk);
        eq_int(blk[2], 0x00, "128 = 0x0080: high byte is zero");
        eq_int(blk[3], 0x80, "and the magnitude is in the low byte");

        /* Every parameter in the shipping table encodes to what the TRM
           addresses say it should. */
        for (int i = 0; i < COUNT(trm_params); i++) {
            char what[128];
            bq_dm_block(trm_params[i].addr, trm_params[i].value, blk);
            snprintf(what, sizeof(what), "%s addresses 0x%04X",
                     trm_params[i].name, trm_params[i].addr);
            check(blk[0] == (uint8_t)(trm_params[i].addr & 0xFF) &&
                  blk[1] == (uint8_t)(trm_params[i].addr >> 8), what);
        }
    }

    printf("the value decode, which must undo the encode exactly\n");
    {
        uint8_t blk[BQ_DM_BLOCK_BYTES];

        eq_int(bq_dm_decode((const uint8_t[]){0x10, 0x70}), 4208, "0x10,0x70 is 4208 mV");
        /* The swap, named so that its absence from the encode is not the only
           thing standing between 4208 and 28688. */
        eq_int(bq_dm_decode((const uint8_t[]){0x70, 0x10}), 28688, "and the other way round is not");
        eq_int(bq_dm_decode((const uint8_t[]){0x00, 0x00}), 0, "both bytes zero");

        /* Signed, because every parameter is I2 and the guard's `value < 0`
           refusal cannot fire on a value the decode cannot represent. */
        eq_int(bq_dm_decode((const uint8_t[]){0xFF, 0xFF}), -1, "0xFFFF decodes as -1, not 65535");
        eq_int(bq_dm_decode((const uint8_t[]){0x80, 0x00}), -32768, "and 0x8000 as the lowest int16");
        eq_int(bq_dm_decode((const uint8_t[]){0x7F, 0xFF}), 32767, "0x7FFF as the highest");

        /* Round trip, over the real table. */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            bq_dm_block(gauge_config[i].addr, gauge_config[i].value, blk);
            snprintf(what, sizeof(what), "%s survives encode then decode",
                     gauge_config[i].name);
            eq_int(bq_dm_decode(&blk[2]), gauge_config[i].value, what);
        }
    }

    printf("the checksum, against the TRM's own worked example\n");
    {
        /*
         * SLUUBD4A, the Hibernate note in section 3: address 0x9221, one data
         * byte 0x00, then "Write (hex) 4C 05". This is the ONLY independent
         * oracle this arithmetic has — every other case below is derived from
         * the same formula the code uses, and would agree with a wrong formula.
         *
         * It settles two questions the TRM's prose leaves open: whether the
         * address bytes are in the sum (they are — 0xFF - 0x00 would be 0xFF,
         * not 0x4C), and what MACDataLen counts.
         *
         * Verified by summing from index 2 in bq_dm_checksum(), which drops the
         * address bytes: this check goes red at 0xFF instead of 0x4C, and takes
         * fourteen others with it — every hand-computed checksum below. It is
         * the one of the fifteen that is evidence rather than agreement, since
         * the others could all be regenerated from a wrong formula. Restored,
         * green.
         */
        const uint8_t trm_block[3] = {0x21, 0x92, 0x00};
        eq_int(bq_dm_checksum(trm_block, 3), 0x4C, "TRM 0x9221 + one 0x00 byte checksums to 0x4C");
        eq_int(bq_dm_len(1), 0x05, "and its MACDataLen is 5");

        /* And the address bytes named explicitly, because the check above is
           the only thing that proves they are counted and it is worth two
           lines to say what it proves. */
        const uint8_t data_only[1] = {0x00};
        check(bq_dm_checksum(trm_block, 3) != bq_dm_checksum(data_only, 1),
              "the address bytes change the checksum, so they are in the sum");

        /* Every parameter in the shipping table against a checksum computed by
           hand in the trm_params table above. */
        uint8_t blk[BQ_DM_BLOCK_BYTES];
        for (int i = 0; i < COUNT(trm_params); i++) {
            char what[160];
            int n = bq_dm_block(trm_params[i].addr, trm_params[i].value, blk);
            snprintf(what, sizeof(what), "%s (0x%04X = %d) checksums to 0x%02X",
                     trm_params[i].name, trm_params[i].addr,
                     trm_params[i].value, trm_params[i].checksum);
            eq_int(bq_dm_checksum(blk, n), trm_params[i].checksum, what);
        }

        /*
         * The defining property, over every block the table produces: the
         * checksum and the sum of the block are complements. A checksum
         * function that returned a constant, or dropped the wrap to 8 bits,
         * fails here even if somebody also "fixed" the expected values above.
         */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            int n = bq_dm_block(gauge_config[i].addr, gauge_config[i].value, blk);
            uint8_t sum = 0;
            for (int b = 0; b < n; b++) sum = (uint8_t)(sum + blk[b]);
            snprintf(what, sizeof(what), "%s: checksum + sum = 0xFF",
                     gauge_config[i].name);
            eq_int((uint8_t)(sum + bq_dm_checksum(blk, n)), 0xFF, what);
        }

        /* It wraps at 8 bits rather than saturating or overflowing an int:
           four 0xFF bytes sum to 0x3FC, which is 0xFC in a byte. */
        const uint8_t all_ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        eq_int(bq_dm_checksum(all_ones, 4), 0x03, "four 0xFF bytes checksum to 0x03");
        const uint8_t zeroes[4] = {0, 0, 0, 0};
        eq_int(bq_dm_checksum(zeroes, 4), 0xFF, "and four zero bytes to 0xFF");

        /* A checksum that ignored its input would pass a table of expected
           values that had been regenerated from it. This is the check that
           does not. */
        const uint8_t a[4] = {0x01, 0x92, 0x00, 0x80};
        const uint8_t b[4] = {0x01, 0x92, 0x00, 0x81};
        check(bq_dm_checksum(a, 4) != bq_dm_checksum(b, 4),
              "one bit of difference in the value changes the checksum");
    }

    printf("MACDataLen, which is not the transfer length\n");
    {
        /*
         * 2 address + N value + 1 checksum + 1 length. Five for the TRM's
         * one-byte example, six for everything this file writes.
         *
         * Verified by changing bq_dm_len() to `2 + value_bytes` — LilyGO's
         * transfer-length bug transplanted into the length field: all three
         * below go red, and so does the TRM oracle above, which is the one that
         * makes 5 a fact rather than a preference. Restored, green.
         */
        eq_int(bq_dm_len(1), 5, "one data byte");
        eq_int(bq_dm_len(BQ_DM_VALUE_BYTES), 6, "the two this file writes");
        eq_int(bq_dm_len(4), 8, "and a four-byte parameter, if one were added");

        /* The number of bytes transferred to 0x60 is TWO — the checksum and the
           length — and is a different quantity entirely. This is the vendor bug
           the source declines to inherit, asserted as an inequality because
           there is nothing else to assert it against. */
        check(bq_dm_len(BQ_DM_VALUE_BYTES) != 2,
              "MACDataLen is not the two bytes written to MACDataSum");
        check(bq_dm_len(BQ_DM_VALUE_BYTES) != BQ_DM_VALUE_BYTES + 2,
              "nor is it LilyGO's size + 2");
    }

    printf("the address allowlist, swept over every address there is\n");
    {
        /*
         * The whole 16-bit space. Exactly six addresses are writable and every
         * one of them is in the table — which is what `default: refuse` means,
         * asserted rather than read.
         *
         * Verified by changing the guard's `default: return false` to
         * `default: return true`: the count below reports 65536 instead of 6,
         * the per-address check names 0x0000 as the first stranger, and
         * twenty-one checks go red in all. Restored, green.
         */
        int allowed = 0;
        int stranger = -1;
        for (int a = 0; a <= 0xFFFF; a++) {
            if (!bq_dm_write_allowed((uint16_t)a, 0)) continue;
            allowed++;
            int in_table = 0;
            for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
                if (gauge_config[i].addr == (uint16_t)a) in_table = 1;
            }
            if (!in_table && stranger < 0) stranger = a;
        }
        eq_int(allowed, GAUGE_CONFIG_COUNT,
               "exactly six addresses in the whole 16-bit space are writable");
        if (stranger >= 0) {
            printf("  FAIL: 0x%04X is writable and is not in the table\n", stranger);
            failures++;
        } else {
            printf("  ok:   every writable address is one this file writes\n");
        }

        /* Named refusals, because a count does not say WHICH six, and these
           are the specific addresses a mistake would land on. Each is real,
           writable on the part, and would corrupt the gauge silently. */
        check(!bq_dm_write_allowed(0x929F, 1300), "0x929F DesignCapacity is refused");
        check(!bq_dm_write_allowed(0x929D, 1300), "0x929D FullChargeCapacity is refused");
        check(!bq_dm_write_allowed(0x929B, 0),    "0x929B GaugingConfig is refused");
        /* The address LilyGO's own header mislabels as EMF. Not a hypothetical
           keystroke — a real one, already made upstream. */
        check(!bq_dm_write_allowed(0x92A3, 3743), "0x92A3 Design Voltage is refused");
        check(!bq_dm_write_allowed(0x92A7, 3743), "0x92A7 EMF is refused too");
        check(!bq_dm_write_allowed(0x0000, 0),    "address zero is refused");
        check(!bq_dm_write_allowed(0xFFFF, 0),    "and an all-ones address");

        /* One either side of every allowed address: the off-by-one that a
           two-byte address invites, and the reason no two entries in the table
           are adjacent. */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            uint16_t a = gauge_config[i].addr;
            snprintf(what, sizeof(what), "0x%04X, one below %s, is refused",
                     (unsigned)(a - 1), gauge_config[i].name);
            check(!bq_dm_write_allowed((uint16_t)(a - 1), gauge_config[i].value), what);
            snprintf(what, sizeof(what), "0x%04X, one above %s, is refused",
                     (unsigned)(a + 1), gauge_config[i].name);
            check(!bq_dm_write_allowed((uint16_t)(a + 1), gauge_config[i].value), what);
        }
    }

    printf("the value bounds on the six addresses that are allowed\n");
    {
        /* Every shipping value passes its own guard. A table edited to
           something the guard refuses would otherwise present on the device as
           a silently short writes_ok. */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            snprintf(what, sizeof(what), "%s = %d is allowed",
                     gauge_config[i].name, gauge_config[i].value);
            check(bq_dm_write_allowed(gauge_config[i].addr, gauge_config[i].value), what);
        }

        /* Negative is refused everywhere: every row of TRM Table 3-2 this file
           writes has a minimum of zero, and all six are signed. */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            snprintf(what, sizeof(what), "%s refuses -1", gauge_config[i].name);
            check(!bq_dm_write_allowed(gauge_config[i].addr, -1), what);
            snprintf(what, sizeof(what), "%s refuses the lowest int16", gauge_config[i].name);
            check(!bq_dm_write_allowed(gauge_config[i].addr, -32768), what);
        }

        /* The TRM's maxima, travelled from both sides.
           Verified on TaperCurrent by changing its `<=` to `<`: "TaperCurrent
           at its 1000 mA maximum" goes red ALONE — one failing check in the
           whole suite, which is what an off-by-one in a bound looks like and
           why each of the five is asked separately. Restored, green. */
        check(bq_dm_write_allowed(0x9201, 1000),  "TaperCurrent at its 1000 mA maximum");
        check(!bq_dm_write_allowed(0x9201, 1001), "and one above it is refused");
        check(bq_dm_write_allowed(0x91FB, 1000),  "ChargingCurrent at its maximum");
        check(!bq_dm_write_allowed(0x91FB, 1001), "and one above it is refused");
        check(bq_dm_write_allowed(0x922A, 2000),  "ChargeDetectThreshold at its 2000 mA maximum");
        check(!bq_dm_write_allowed(0x922A, 2001), "and one above it is refused");
        check(bq_dm_write_allowed(0x922C, 1000),  "QuitCurrent at its maximum");
        check(!bq_dm_write_allowed(0x922C, 1001), "and one above it is refused");
        check(bq_dm_write_allowed(0x92A5, 1000),  "CEDV taper voltage at its maximum");
        check(!bq_dm_write_allowed(0x92A5, 1001), "and one above it is refused");

        /*
         * ChargingVoltage is the one bound that is NOT the TRM's. The TRM
         * allows 4600 mV; this guard stops at 4208, the same ceiling
         * bq25896_write_allowed() enforces on the charger. Telling the gauge to
         * wait for a voltage the charger may not produce would recreate the
         * never-terminates bug this commit fixes, one file over.
         *
         * Travelled from below, which makes it the bound most likely to rot —
         * the shipping value sits exactly on it. Verified by raising
         * BQ_DM_CHARGING_VOLTAGE_MAX to the TRM's 4600: the last two below go
         * red, and so does the cross-file ceiling check at the end of this
         * file, which is the one that says why the number is 4208. Restored,
         * green.
         */
        check(bq_dm_write_allowed(0x91FD, 4208),  "ChargingVoltage at 4208 mV, the value written");
        check(!bq_dm_write_allowed(0x91FD, 4209), "4209 mV is refused: one above the ceiling");
        check(!bq_dm_write_allowed(0x91FD, 4600), "and so is the TRM's own 4600 mV maximum");
    }

    printf("the parameter table against TRM Table 3-2\n");
    {
        eq_int(GAUGE_CONFIG_COUNT, COUNT(trm_params), "six parameters");
        /* The mask is a uint8_t. A seventh parameter is fine; a ninth silently
           stops being tracked. */
        check(GAUGE_CONFIG_COUNT <= 8, "and few enough to fit the verify mask");
        eq_int(GAUGE_MASK_ALL, 0x3F, "so GAUGE_MASK_ALL is six bits");

        for (int i = 0; i < COUNT(trm_params); i++) {
            char what[160];
            const GaugeParam *p = param_named(trm_params[i].name);
            snprintf(what, sizeof(what), "%s is in the table", trm_params[i].name);
            check(p != NULL, what);
            if (!p) continue;

            snprintf(what, sizeof(what), "%s is at 0x%04X",
                     trm_params[i].name, trm_params[i].addr);
            check(p->addr == trm_params[i].addr, what);

            snprintf(what, sizeof(what), "%s is %d", trm_params[i].name, trm_params[i].value);
            eq_int(p->value, trm_params[i].value, what);

            snprintf(what, sizeof(what), "%s says what it is for", trm_params[i].name);
            check(p->why != NULL && p->why[0] != '\0' &&
                  p->units != NULL && p->units[0] != '\0', what);
        }

        /* No address written twice. Two entries at one address means the second
           silently wins on the device and the first is a lie in the endpoint. */
        int clash = 0;
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            for (int j = i + 1; j < GAUGE_CONFIG_COUNT; j++) {
                if (gauge_config[i].addr == gauge_config[j].addr) clash = 1;
            }
        }
        check(!clash, "no two parameters claim the same address");
    }

    printf("the verify mask\n");
    {
        int16_t live[GAUGE_CONFIG_COUNT];
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) live[i] = gauge_config[i].value;

        eq_int(gauge_verify_mask(live), 0, "a gauge holding the whole table masks clean");

        /* Each parameter owns its own bit and no other's — the same shape as
           the flag tables above, and the same defect it is looking for. */
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            char what[128];
            int16_t was = live[i];
            live[i] = (int16_t)(was + 1);
            snprintf(what, sizeof(what), "%s wrong sets bit %d and nothing else",
                     gauge_config[i].name, i);
            eq_int(gauge_verify_mask(live), 1 << i, what);
            live[i] = was;
        }

        /* Off by one in either direction, on the parameter that matters: 100 is
           the ROM default this whole commit is about, and it must not read as
           agreement. */
        {
            int taper = param_index("TaperCurrent");
            check(taper >= 0, "TaperCurrent has an index");
            live[taper] = 100;
            eq_int(gauge_verify_mask(live), 1 << taper,
                   "a TaperCurrent still at its 100 mA ROM default is flagged");
            live[taper] = gauge_config[taper].value;
        }

        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) live[i] = 0;
        eq_int(gauge_verify_mask(live), GAUGE_MASK_ALL, "an all-zero read-back masks everything");
    }

    printf("the verdict the endpoint leads with\n");
    {
        int16_t good[GAUGE_CONFIG_COUNT];
        int16_t bad[GAUGE_CONFIG_COUNT];
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) {
            good[i] = gauge_config[i].value;
            bad[i] = (int16_t)(gauge_config[i].value + 1);
        }
        GaugeVerdict v;

        gauge_derive(v, true, true, good);
        eq_int(v.live_mask, 0, "read ok, boot applied, values right: clean mask");
        check(v.config_still_live, "and the configuration is still live");
        check(v.applied, "and the endpoint may say applied");

        /*
         * "We could not check" must never read as "we checked and it was
         * fine", and the buffer handed in here is PERFECT — so a derive that
         * ignored read_ok would come out clean and applied.
         *
         * Verified by dropping the read_ok term from live_mask: the first of
         * the three goes red, alone. The other two survive that mutation
         * because config_still_live carries its own read_ok term — which is
         * why the mask is asserted here separately rather than trusted to fall
         * out of `applied`. Restored, green.
         */
        gauge_derive(v, false, true, good);
        eq_int(v.live_mask, GAUGE_MASK_ALL, "a failed read-back masks everything");
        check(!v.applied, "and cannot be reported as applied");
        check(!v.config_still_live, "nor as still live");

        /* A configuration that never landed is not made live by the part
           agreeing with it later. */
        gauge_derive(v, true, false, good);
        eq_int(v.live_mask, 0, "values right but the boot write never verified");
        check(v.config_still_live, "the comparison itself still passes");
        check(!v.applied, "but applied is false, because boot did not verify");

        /* And the case the whole re-derivation exists for: verified at boot,
           gone now. This is a gauge that lost its RAM at 03:00 being reported
           honestly at noon. */
        gauge_derive(v, true, true, bad);
        eq_int(v.live_mask, GAUGE_MASK_ALL, "every value changed since boot");
        check(!v.config_still_live, "so nothing is still live");
        check(!v.applied, "and a boot success does not survive it");

        /* One parameter gone is enough. */
        int16_t one_off[GAUGE_CONFIG_COUNT];
        for (int i = 0; i < GAUGE_CONFIG_COUNT; i++) one_off[i] = gauge_config[i].value;
        one_off[0] = (int16_t)(one_off[0] + 1);
        gauge_derive(v, true, true, one_off);
        check(!v.applied, "a single parameter out of place is enough to lose applied");
    }

    printf("the security field the endpoint renders from bits\n");
    {
        /* GET /battery reports sec_before and sec_after by synthesising a word
           from two stored bits — `bits << BQ_OP_SEC_SHIFT` — and handing it to
           the same decoder the live register goes through. If that synthesis
           and bq_sec_bits() ever disagree, the endpoint reports the wrong
           security state for the write it just performed. */
        for (uint8_t bits = 0; bits <= 3; bits++) {
            char what[128];
            uint16_t word = (uint16_t)(bits << BQ_OP_SEC_SHIFT);
            snprintf(what, sizeof(what), "SEC %u survives the round trip through a word", bits);
            eq_int(bq_sec_bits(word), bits, what);
        }
        eq_str(bq_sec_name((uint16_t)(BQ_SEC_SEALED << BQ_OP_SEC_SHIFT)), "sealed",
               "BQ_SEC_SEALED is the sealed code");
        eq_str(bq_sec_name((uint16_t)(BQ_SEC_UNSEALED << BQ_OP_SEC_SHIFT)), "unsealed",
               "BQ_SEC_UNSEALED is the unsealed code");
        /* The state gauge_open_access() drives to and returns on. */
        eq_str(bq_sec_name((uint16_t)(BQ_SEC_FULL << BQ_OP_SEC_SHIFT)), "full",
               "BQ_SEC_FULL is the full-access code");
        check(BQ_SEC_SEALED != BQ_SEC_UNSEALED && BQ_SEC_UNSEALED != BQ_SEC_FULL &&
              BQ_SEC_SEALED != BQ_SEC_FULL, "and the three codes are distinct");
    }

    printf("GaugingStatus, the word that is reached by subcommand\n");
    {
        flag_table(gauging_flags, COUNT(gauging_flags), "gauging_status");

        /*
         * The five bits Table 2-4 marks RSVD.
         *
         * Stated honestly, because this one is weaker than it looks: it reads
         * the BIT NUMBERS in the table above, not the masks in the source, so
         * moving a mask onto bit 12 does NOT trip it — verified, the position
         * checks in flag_table() fire instead and this stays green.
         *
         * What it catches is the other direction, which is the one that
         * matters here: a future reader ADDING a decoded flag at a reserved
         * position. That is precisely the ITPOR mistake the source file
         * refuses to make — inventing a name for a bit the datasheet does not
         * define — and it would otherwise sail through, because a brand-new
         * mask at bit 12 is internally consistent and every other check in
         * this file would pass it.
         */
        static const int reserved[] = {4, 8, 9, 11, 12};
        for (int i = 0; i < COUNT(reserved); i++) {
            int claimed = 0;
            for (int j = 0; j < COUNT(gauging_flags); j++) {
                if (gauging_flags[j].bit == reserved[i]) claimed = 1;
            }
            char what[96];
            snprintf(what, sizeof(what),
                     "gauging_status bit %d is reserved and is not decoded",
                     reserved[i]);
            check(!claimed, what);
        }
    }

    printf("CONTROL_STATUS\n");
    {
        flag_table(control_flags, COUNT(control_flags), "control_status");

        /* BATT_ID is a field, not a flag, so flag_table cannot speak for it. */
        eq_int(bq_batt_id(0x0000), 0, "batt_id of an empty word");
        eq_int(bq_batt_id(0x0007), 7, "batt_id reads all three of its bits");
        eq_int(bq_batt_id(0x0005), 5, "and reads them in the right order");
        /* The bits immediately above the field must not leak into it. CCA,
           BCA and SNOOZE are bits 5, 4 and 3 and a mask of 0xF or wider would
           swallow SNOOZE — which would report a gauge in SNOOZE as chemistry
           profile 8 and never look wrong. */
        eq_int(bq_batt_id(0xFFF8), 0, "and nothing above bit 2 leaks into it");
        eq_int(bq_batt_id((uint16_t)BQ_CS_SNOOZE), 0, "SNOOZE is not batt_id");
    }

    printf("the MAC echo, which is what stops a stale frame being decoded\n");
    {
        /* TRM 2.2's own worked example: subcommand 0x0001 answers with a frame
           beginning 0x01 0x00. An oracle from the primary source rather than a
           restatement of the code. */
        const uint8_t device_number_frame[4] = {0x01, 0x00, 0x20, 0x03};
        check(bq_mac_echo_ok(0x0001, device_number_frame),
              "the TRM's DEVICE_NUMBER frame echoes 0x0001");
        eq_int(bq_word(device_number_frame[2], device_number_frame[3]), 0x0320,
               "and its payload is the device type, little-endian");

        const uint8_t gauging_frame[4] = {0x56, 0x00, 0x84, 0x00};
        check(bq_mac_echo_ok(BQ_MAC_GAUGING_STATUS, gauging_frame),
              "a GaugingStatus frame echoes 0x0056 low byte first");
        eq_int(bq_word(gauging_frame[2], gauging_frame[3]), 0x0084,
               "and its payload assembles little-endian too");

        /* The failures this exists for. A frame left over from another
           subcommand, and a frame with the echo bytes swapped — both carry
           sixteen perfectly decodable bits that are about something else. */
        const uint8_t stale[4] = {0x54, 0x00, 0x84, 0x00};
        check(!bq_mac_echo_ok(BQ_MAC_GAUGING_STATUS, stale),
              "a frame left over from OPERATION_STATUS is refused");
        const uint8_t swapped[4] = {0x00, 0x56, 0x84, 0x00};
        check(!bq_mac_echo_ok(BQ_MAC_GAUGING_STATUS, swapped),
              "and so is one with the echo bytes the wrong way round");
        const uint8_t empty[4] = {0x00, 0x00, 0x00, 0x00};
        check(!bq_mac_echo_ok(BQ_MAC_GAUGING_STATUS, empty),
              "a dead bus reads all zeroes and is refused");
    }

    printf("the subcommand allowlist, swept\n");
    {
        /* The same sweep the data-memory allowlist gets, and for a sharper
           reason: this space contains RESET. */
        int allowed = 0;
        for (long s = 0; s <= 0xFFFF; s++) {
            if (bq_mac_read_allowed((uint16_t)s)) allowed++;
        }
        eq_int(allowed, 2, "exactly two subcommands are readable");

        check(bq_mac_read_allowed(BQ_MAC_GAUGING_STATUS), "GaugingStatus 0x0056");
        check(bq_mac_read_allowed(BQ_MAC_OPERATION_STATUS), "OperationStatus 0x0054");

        /* Every destructive subcommand in TRM Table 2-2, by name, because
           "everything else is refused" is a sentence and these are the ones
           that would cost something. */
        check(!bq_mac_read_allowed(0x0041), "RESET 0x0041 is refused");
        check(!bq_mac_read_allowed(0x0030), "SEALED 0x0030 is refused");
        check(!bq_mac_read_allowed(0x0090), "ENTER_CFG_UPDATE 0x0090 is refused");
        check(!bq_mac_read_allowed(0x0091), "EXIT_CFG_UPDATE_REINIT is refused");
        check(!bq_mac_read_allowed(0x0F00), "RETURN_TO_ROM 0x0F00 is refused");
        check(!bq_mac_read_allowed(0x000A), "CC_OFFSET 0x000A is refused");
        check(!bq_mac_read_allowed(0x0081), "ENTER_CAL 0x0081 is refused");
        /* One digit from GaugingStatus in each direction. */
        check(!bq_mac_read_allowed(0x0055), "0x0055 is refused");
        check(!bq_mac_read_allowed(0x0057), "0x0057 is refused");
    }

    printf("the signed currents\n");
    {
        eq_int(bq_signed(0x0000), 0, "zero is zero");
        eq_int(bq_signed(0x0200), 512, "512 mA into the cell");
        eq_int(bq_signed(0x0080), 128, "128 mA, the charger's termination current");
        eq_int(bq_signed(0xFFB0), -80, "an 80 mA discharge is negative");
        /* The small readings are the ones an unsigned mistake still flatters:
           65531 does not look like a current, but it does not look like -5
           either, and this device idles in exactly that range. */
        eq_int(bq_signed(0xFFFB), -5, "and so is a 5 mA one");
        eq_int(bq_signed(0x7FFF), 32767, "the largest positive value");
        eq_int(bq_signed(0x8000), -32768, "and the most negative one");
    }

    printf("the taper band, which is what FC is actually waiting for\n");
    {
        /* TRM 4.4.1 conditions 1 and 2 read together. The floor is the half of
           it that reads like a footnote and is not one. */
        eq_int((int)bq_taper_floor_ua(), 22500,
               "0.25 mAh per 40 s window is a floor of 22500 uA");
        eq_int(BQ_TAPER_WINDOW_S, 40, "the window is the TRM's 40 s");
        eq_int(BQ_TAPER_MIN_CAP_UAH, 250, "and the capacity is its 0.25 mAh");

        /*
         * The second floor, which is a different mechanism: below Quit Current
         * the gauge leaves CHARGE mode after Chg Relax Time, and the TRM's
         * default 60 s is SHORTER than the 80 s two windows take. So the
         * qualification is pre-empted before it can complete, and the floor
         * that binds is whichever is higher.
         */
        check(bq_relax_pre_empts_taper(),
              "60 s of Chg Relax Time is shorter than two 40 s windows");
        eq_int(BQ_CHG_RELAX_TIME_S, 60, "Chg Relax Time is the TRM's 60 s");
        eq_int(BQ_TAPER_WINDOWS, 2, "and two windows are required");

        /* QuitCurrent 40 mA is above the 22.5 mA capacity floor, so on THIS
           device it is the binding one. */
        eq_int((int)bq_termination_floor_ua(40), 40000,
               "QuitCurrent 40 mA binds over the capacity floor");
        /* And below it, the capacity condition takes over again. */
        eq_int((int)bq_termination_floor_ua(10), 22500,
               "a QuitCurrent of 10 mA does not, so the capacity floor binds");
        eq_int((int)bq_termination_floor_ua(0), 22500,
               "nor does a QuitCurrent of zero");

        /* The shipping QuitCurrent is the one the endpoint reports against. */
        const GaugeParam *quit_p = param_named("QuitCurrent");
        check(quit_p != NULL, "QuitCurrent is in the table");
        if (quit_p) {
            eq_int((int)bq_termination_floor_ua(quit_p->value), 40000,
                   "so the floor this build actually waits on is 40000 uA");
        }

        /*
         * THE CHECK THIS WHOLE COMMIT IS ABOUT, stated as a fact rather than
         * as an assertion about what the values should be.
         *
         * Termination needs the current between the floor and the ceiling for
         * two windows. The charger stops delivering at its termination
         * current. So the band is only reachable while charging if the gauge's
         * ceiling is ABOVE the charger's cut-off — and today it is not, it is
         * equal to it, which is why this reports rather than asserts.
         *
         * Deliberately NOT written as check(taper->value > iterm): that would
         * be red on the shipping configuration, and this commit was asked to
         * diagnose without changing behaviour. When the taper current is
         * raised, this check is the one to invert.
         */
        const GaugeParam *taper_p = param_named("TaperCurrent");
        check(taper_p != NULL, "TaperCurrent is in the table");
        if (taper_p) {
            check((long)taper_p->value * 1000L > bq_taper_floor_ua(),
                  "the taper ceiling is above the capacity floor at all");
            eq_int(taper_p->value, charger_table_iterm_ma(),
                   "TODAY the ceiling EQUALS the charger's cut-off, so the "
                   "band the gauge watches is the range the charger will not "
                   "operate in — this is the defect, pinned, not endorsed");
        }
    }

    printf("the gauge and the charger agree about what a finished charge is\n");
    {
        /*
         * The one fact that spans two files, and the reason this whole commit
         * works. charger.cpp sets the BQ25896 to terminate at 128 mA; the gauge
         * declares a pack full when it sees the current taper below ITS taper
         * threshold. If those two numbers ever stop being equal, the gauge goes
         * back to never seeing a charge finish — silently, with every other
         * check in both suites still green.
         *
         * Both sides are read out of the shipping tables, so a change to either
         * file alone fails here. Stated honestly, though: a change to either
         * file alone ALSO fails that file's own suite, which pins both numbers
         * — editing charger.cpp's REG05 to 0x12 turns tools/test_charger.sh red
         * as well, so this check is not what catches that.
         *
         * What it catches, and nothing else does, is the two moving APART while
         * each file's own expectations are kept consistent. Verified: the
         * gauge's TaperCurrent moved to 192 mA and the trm_params pin above
         * moved with it, charger.cpp untouched. tools/test_charger.sh stays
         * green, every other check in this file stays green, and the equality
         * below goes red alone. Restored, green.
         *
         * The other half of its value is the message. A pin failing says a
         * number changed; this says the gauge and the charger no longer agree,
         * which is the sentence somebody needs to read at that moment.
         */
        const GaugeParam *taper = param_named("TaperCurrent");
        const GaugeParam *volts = param_named("ChargingVoltage");
        const GaugeParam *amps  = param_named("ChargingCurrent");
        check(taper && volts && amps, "the three mirrored parameters are in the table");

        eq_int(charger_table_iterm_ma(), 128, "the charger terminates at 128 mA");
        if (taper) {
            eq_int(taper->value, charger_table_iterm_ma(),
                   "and the gauge's TaperCurrent is that same number");
        }

        eq_int(charger_table_vreg_mv(), 4208, "the charger regulates at 4208 mV");
        if (volts) {
            eq_int(volts->value, charger_table_vreg_mv(),
                   "and the gauge's ChargingVoltage is that same number");
        }

        eq_int(charger_table_ichg_ma(), 512, "the charger charges at 512 mA");
        if (amps) {
            eq_int(amps->value, charger_table_ichg_ma(),
                   "and the gauge's ChargingCurrent is that same number");
        }

        /* The gauge's ceiling and the charger's are one number, not two that
           happen to agree today. */
        check(!bq_dm_write_allowed(0x91FD, (int16_t)(charger_table_vreg_mv() + 1)),
              "the gauge may not be told to expect more than the charger's ceiling");
    }

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
MAIN
} > "$work/test.cpp"

# The charger's configuration table, sliced out of ITS shipping source and
# compiled into its own object, so that one relationship can be asserted across
# the two files: the gauge's TaperCurrent must equal the charger's termination
# current, or the gauge never sees a charge finish and none of this works.
#
# A separate translation unit rather than a second slice in test.cpp because
# this test uses three values out of that block and none of its functions, and
# twenty -Wunused-function warnings would bury the real output. The block's own
# correctness is tools/test_charger.sh's job; here it is only a source of facts,
# which is why -Wno-unused-function is acceptable on this object and would not
# be on the one above.
{
    cat <<'CHARGER_PRELUDE'
/* Generated by tools/test_battery.sh — do not edit. */
#include <stdint.h>
#include <string.h>

CHARGER_PRELUDE
    slice "$charger_src" charger
    cat <<'CHARGER_FACTS'

/* Looked up by REGISTER, never by index. charger.cpp's table order IS a
   correctness property over there — REG07 has to go out first — and it is free
   to change for that reason; indexing into it here would make a legal
   reordering fail this suite in a file that has nothing to do with it. A
   register that is not in the table returns 0, whose decoded value matches none
   of the three expected figures, so a deletion fails rather than passes. */
static uint8_t charger_reg(uint8_t reg) {
    for (int i = 0; i < CHARGER_CONFIG_COUNT; i++) {
        if (charger_config[i].reg == reg) return charger_config[i].val;
    }
    return 0;
}

extern "C" int charger_table_iterm_ma(void) {
    return bq25896_iterm_ma(charger_reg(BQ25896_REG05_TERM));
}

extern "C" int charger_table_ichg_ma(void) {
    return bq25896_ichg_ma(charger_reg(BQ25896_REG04_ICHG));
}

extern "C" int charger_table_vreg_mv(void) {
    return bq25896_vreg_mv(charger_reg(BQ25896_REG06_VREG));
}
CHARGER_FACTS
} > "$work/charger_facts.cpp"

cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
       -c -o "$work/charger_facts.o" "$work/charger_facts.cpp"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
       -o "$work/test" "$work/test.cpp" "$work/charger_facts.o"
"$work/test"
