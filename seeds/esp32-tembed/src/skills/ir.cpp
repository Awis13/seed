/*
 * skills/ir.cpp — infrared transmitter skill for the T-Embed CC1101 seed
 *
 * A TV-B-Gone style power-off blast plus a raw one-shot sender, both driving
 * the onboard IR LED on GPIO2.
 *
 * Endpoints:
 *   POST /ir/tvbgone        — start a blast {region, repeat} or one code {code}
 *   GET  /ir/tvbgone/status — progress of the running (or last) job
 *   POST /ir/tvbgone/stop   — abort mid-run
 *   POST /ir/send           — send one raw frame {khz, duty, raw[], repeat}
 *
 * All four are registered with AsyncURIMatcher::exact(). The library's default
 * is BackwardCompatible, which matches ^{uri}(/.*)?$ — under it /ir/tvbgone
 * also answers /ir/tvbgone/stop, and being registered first it wins.
 *
 * Code table provenance
 * ---------------------
 * The blast draws on two tables, and they do not have the same licence.
 *
 * The bulk of it is the TV-B-Gone power-code database by Mitch Altman and
 * Limor Fried — 270 captured codes, split into the NA and EU sets the original
 * device ships — copied from Arduino-TV-B-Gone and kept in ir_codes_tvbgone.h.
 * That file is under Creative Commons Attribution-ShareAlike, the licence its
 * upstream carries, with the text bundled as LICENSE-CC-BY-SA; the rest of
 * this repository is MIT. Keeping the data in a file of its own, with no logic
 * in it, is what bounds the share-alike obligation to the table. See the
 * header of ir_codes_tvbgone.h for the full attribution and for which licence
 * version applies, and seeds/README.md for the same note at repository level.
 *
 * The other table is nine codes this file encodes itself, from the published
 * descriptions of six consumer protocols — NEC, Samsung, Sony SIRC, Philips
 * RC-5, Philips RC-6 mode 0 and Panasonic/Kaseikyo. They are MIT like the rest
 * of the seed, and they go out at the head of every blast. They earn that on
 * two counts: the Samsung entry is the one frame in this file confirmed
 * against a real television, and they are the only path here that can build a
 * frame from an address and a command rather than replay a capture — the
 * TV-B-Gone table is packed timings, and POST /ir/send is raw microseconds.
 *
 * Transmission
 * ------------
 * The ESP32-S3 RMT peripheral generates the carrier and clocks out the
 * mark/space stream in hardware: one TX channel is claimed at boot and kept.
 * Bit-banging was rejected: a blast is minutes of microsecond timing, and
 * holding the CPU for that starves the WiFi and AsyncTCP tasks.
 *
 * The WS2812 ring (ring.cpp) is the other RMT TX consumer on this board, and
 * the two share the peripheral rather than compete for it: the Arduino core's
 * wrapper allocates one channel, one copy encoder and one mutex per *pin*, so
 * neither can stall or corrupt the other's stream. What they do share is the
 * chip's four TX memory blocks, of which this file takes two — so it is
 * initialised first, and ring.cpp takes what is left. See the header comment
 * in ring.cpp for the arithmetic.
 *
 * Carrier frequency and duty are per code, not global. The hand-written codes
 * use 38 kHz for NEC and Samsung, 40 kHz for Sony, 36 kHz at 25% duty for the
 * two Philips protocols and 37 kHz for Kaseikyo; the TV-B-Gone codes carry a
 * frequency each, anywhere from 25.6 to 83.3 kHz, at the 33% duty upstream
 * uses. Eighteen of them are baseband — the mark is the LED on with no carrier
 * under it — which is why the carrier can be switched off entirely.
 */

/* --- Limits --- */

#define IR_MAX_SYMBOLS 320    /* rmt_data_t holds two runs, so 640 mark/space runs */
#define IR_MAX_TICKS   32767  /* the duration field is 15 bits, and a tick is 1us */
#define IR_MAX_RAW     512    /* mark/space entries accepted by /ir/send */
#define IR_REPEAT_MAX  10
#define IR_TVB_DUTY    33     /* upstream's carrier duty: OCR2B = OCR2A / 3 */

/*
 * Two gaps, because they do two different jobs.
 *
 * Between repeats of one code, the silence only has to be long enough for the
 * receiver to see two frames rather than one held key. One SIRC frame period
 * is the tightest thing that satisfies every protocol here.
 *
 * Between two different codes, the receiver's AGC has to settle before it will
 * lock onto a frame it has never seen. Upstream nominally asks for 205ms, but
 * it asks with delay_ten_us() — an uncalibrated AVR NOP loop, counted in
 * instructions on a part whose instruction rate has nothing to do with this
 * one, that ports carry over verbatim. Whatever it comes to here it is not
 * 205ms, so the number is not evidence of a requirement. Nothing in this file
 * executes it in any case: the gap is the constant below, in milliseconds off a
 * real clock. 100ms is a real number, chosen here: it is long enough to be a
 * gap and it keeps a region under 40 seconds, which is the only budget that
 * matters for something used while standing in front of a television.
 *
 * This gap is not what makes a television toggle back on, and shortening it
 * would not help. A receiver ignores IR for something like 0.5-2s after
 * accepting a power command, but the duplicate commands that were causing
 * toggling sit SECONDS apart in the run — 3.9s and 38.0s for the Samsung pair
 * at this gap — with dozens of other codes between them, so no gap this file
 * could plausibly be set to hides them inside a lockout. Shortening the gap
 * moves them closer together and leaves them seconds apart; the separation is
 * mostly the codes themselves. The AVR original settles it from the other
 * direction: its delay_ten_us(25000) is 250ms on the part it was written for,
 * slower than this, and it does not toggle, because within one region it sends
 * each command once. Duplicate suppression is the fix; see ir_dup_groups below.
 */
#define IR_CODE_GAP_MS    100
#define IR_REPEAT_GAP_MS  45

/* --- Protocols --- */

enum {
    IR_NEC = 0,   /* 9ms header, pulse distance, 32 bits MSB first */
    IR_SAMSUNG,   /* as NEC but with a 4.5ms header mark */
    IR_SONY,      /* SIRC, pulse width, 12/15/20 bits */
    IR_RC5,       /* Philips biphase, 14 bits */
    IR_RC6,       /* Philips biphase mode 0, 16 payload bits */
    IR_KASEIKYO   /* Panasonic and the rest of the Kaseikyo family, 48 bits */
};

#define IR_REGION_NA  0x01
#define IR_REGION_EU  0x02
#define IR_REGION_ALL (IR_REGION_NA | IR_REGION_EU)

struct IrCode {
    const char *brand;
    /* Extra name {"code":...} accepts, or NULL. The brand is what the menu and
       the status line show, so a name that is worth typing but not worth
       displaying goes here instead of being wedged into the brand string. */
    const char *alias;
    uint64_t value;   /* pulse-distance/SIRC: the frame, MSB first.
                         RC-5/RC-6: (address << 8) | command. */
    uint8_t proto;
    uint8_t bits;     /* payload bits; unused for RC-5/RC-6 */
    uint8_t khz;      /* carrier frequency */
    uint8_t duty;     /* carrier duty, percent */
    uint8_t sends;    /* frames the protocol needs before a receiver acts */
    uint8_t regions;
};

/*
 * Power-key codes for the vendors with the widest installed base. The Sony
 * entries are the same command (21) and TV address (1) in the three SIRC frame
 * lengths, worked out from the protocol's LSB-first bit order; the Philips
 * entries are address 0x00, command 0x0C (standby).
 *
 * Every code here is sold worldwide, so all nine are in both regions. They are
 * sent first, ahead of the TV-B-Gone table, so that the one frame confirmed
 * against real hardware goes out while the user is still pointing the thing.
 *
 * Three of them — LG, Toshiba and Samsung — are commands the upstream table
 * also carries. Going first means they win their duplicate group and the
 * table's copies are skipped, which is the right way round: the frame that has
 * been verified is the one that gets sent.
 *
 * `sends` is a property of the protocol, not a preference. A Sony receiver
 * discards a SIRC frame it has seen fewer than about three times, so the three
 * Sony entries are worth nothing sent once. Nothing else here needs repeating,
 * and repeating it anyway is how a blast turns into minutes.
 */
static const IrCode ir_codes[] = {
    {"LG/Zenith/Vizio", NULL,     0x20DF10EFULL,     IR_NEC,      32, 38, 33, 1, IR_REGION_ALL},
    {"Toshiba",         NULL,     0x02FD48B7ULL,     IR_NEC,      32, 38, 33, 1, IR_REGION_ALL},
    {"Samsung",         NULL,     0xE0E040BFULL,     IR_SAMSUNG,  32, 38, 33, 1, IR_REGION_ALL},
    /* "Sony" is the obvious thing to type and the three SIRC lengths are not.
       It resolves to 12-bit: it is the original SIRC frame and the one every
       Sony receiver understands, where 15 and 20 are extensions. */
    {"Sony 12-bit",     "Sony",   0x00000A90ULL,     IR_SONY,     12, 40, 33, 3, IR_REGION_ALL},
    {"Sony 15-bit",     NULL,     0x00005480ULL,     IR_SONY,     15, 40, 33, 3, IR_REGION_ALL},
    {"Sony 20-bit",     NULL,     0x000A9000ULL,     IR_SONY,     20, 40, 33, 3, IR_REGION_ALL},
    {"Philips RC-5",    "Philips",0x0000000CULL,     IR_RC5,       0, 36, 25, 1, IR_REGION_ALL},
    {"Philips RC-6",    NULL,     0x0000000CULL,     IR_RC6,       0, 36, 25, 1, IR_REGION_ALL},
    {"Panasonic",       NULL,     0x40040100BCBDULL, IR_KASEIKYO, 48, 37, 33, 1, IR_REGION_ALL},
};
static const int ir_code_count = (int)(sizeof(ir_codes) / sizeof(ir_codes[0]));

/* ir_describe() lists these by name, and the on-device by-brand menu is sized
   from this count. A tenth entry needs the markdown updated with it. */
static_assert(ir_code_count == 9, "ir_describe() lists the named codes by hand");

/*
 * Index of the named code, or -1. Comparison is case-insensitive, against the
 * whole brand, against each of its slash-separated alternatives, and against
 * the alias, so an entry covering three vendors answers to any one of them:
 * "lg", "Zenith" and "LG/Zenith/Vizio" all select the same code, and "Sony"
 * reaches the 12-bit entry that "Sony 12-bit" also names.
 *
 * Only the hand-written table is addressable this way. The TV-B-Gone entries
 * are numbered captures, not brands — their names are "NA 0031", and picking
 * one by hand is not a thing anybody standing in front of a television can do.
 */
static int ir_find_code(const char *name) {
    if (!name || !*name) return -1;
    size_t len = strlen(name);
    for (int i = 0; i < ir_code_count; i++) {
        const char *brand = ir_codes[i].brand;
        if (strcasecmp(name, brand) == 0) return i;
        if (ir_codes[i].alias && strcasecmp(name, ir_codes[i].alias) == 0) return i;
        for (const char *p = brand; p; ) {
            const char *sep = strchr(p, '/');
            size_t n = sep ? (size_t)(sep - p) : strlen(p);
            if (n == len && strncasecmp(name, p, n) == 0) return i;
            p = sep ? sep + 1 : NULL;
        }
    }
    return -1;
}

/* The TV-B-Gone database. Data only, and under a different licence from the
   rest of the seed — see the file header there. It needs IR_REGION_NA and
   IR_REGION_EU, hence the include sitting here rather than at the top. */
#include "ir_codes_tvbgone.h"

/*
 * The blast walks one address space: 0..ir_code_count-1 are the hand-written
 * codes above, the rest are ir_tvb_codes[]. Nothing materialises the filtered
 * list — with 279 entries that would be a few hundred bytes of RAM to say what
 * a comparison already says.
 */
#define IR_ENTRY_COUNT (ir_code_count + IR_TVB_COUNT)

static uint8_t ir_entry_regions(int i) {
    return (i < ir_code_count) ? ir_codes[i].regions
                               : ir_tvb_codes[i - ir_code_count].regions;
}

static const char *ir_entry_label(int i) {
    return (i < ir_code_count) ? ir_codes[i].brand
                               : ir_tvb_names[i - ir_code_count];
}

/* Frames this entry needs before a receiver will act on it. The TV-B-Gone
   codes are always 1: whatever repetition their protocol wants is already
   inside the index stream, which is why some of them run to 136 pairs. */
static uint8_t ir_entry_sends(int i) {
    return (i < ir_code_count) ? ir_codes[i].sends : 1;
}

/*
 * Entries that carry the same command as another entry.
 *
 * These codes are power TOGGLES. Sending one command twice in a run therefore
 * turns a television off and back on, and the database does contain the same
 * command more than once: it is a set of captures, so one command captured
 * from two remotes is two entries with slightly different timings that decode
 * identically. The NA and EU lists also overlap by more than the five entries
 * upstream merged — that merge was by identical byte stream, which misses a
 * capture pair that differs in the third decimal place and not in what a
 * receiver reads off it.
 *
 * Decoding the table on a host found 15 commands appearing more than once
 * across the 279 entries, covering 36 of them. This is that result: an entry's
 * group, or 0 when its command is unique or its frame was not decoded. A blast
 * sends the first entry of each group and skips the rest, so every receiver
 * sees every command exactly once and ends up off rather than back on.
 *
 * Scope, so nobody reads this table as exhaustive. It covers the three
 * pulse-distance families whose frames carry an address and command in the
 * clear — NEC (9ms/4.5ms header), Samsung (4.5ms/4.5ms) and Kaseikyo (3.4ms/
 * 1.7ms, 48 bits) — and Sony SIRC, whose 2.4ms header and value-carrying mark
 * widths read just as plainly. It does NOT cover Philips RC-5 or RC-6, and 160
 * of the 270 upstream entries decode as none of the four and are therefore
 * ungrouped whatever they carry. A duplicate in those two protocols is still
 * possible and would still toggle a set back on.
 *
 * Group 15 is the SIRC one, and it is why the decode was extended to that
 * protocol. 12-bit 0xA90 is the hand-written Sony 12-bit entry, na000 (entry
 * 9) and na041 (entry 50), and because na000 carries both region bits the
 * command landed twice in eu — an even count, which is the case that ends with
 * the set back on. Entry 3 sorts first and so is the one that survives, which
 * costs no coverage: it sends three frames, and each of the two it displaces
 * packs two into its index stream, so neither delivered more than it does. A
 * SIRC receiver wants about three frames before it acts, which is what
 * sends = 3 on that entry is for. na041 is a 76.9kHz capture besides, well
 * outside what a receiver tuned for SIRC's 40kHz can hear.
 *
 * Derived here rather than in ir_codes_tvbgone.h on purpose: this is analysis
 * of the data, not the data, so it stays MIT and the table stays untouched.
 *
 * By-brand sends are not filtered — an explicit request for one code is not a
 * blast, and asking for the same code twice is the caller's business.
 */
struct IrDupGroup {
    uint16_t entry;
    uint8_t group;
};

static constexpr IrDupGroup ir_dup_groups[] = {
    {  0,  1},  /* LG/Zenith/Vizio  NEC 0x20DF10EF */
    {  1,  2},  /* Toshiba          NEC 0x02FD48B7 */
    {  2,  3},  /* Samsung      SAMSUNG 0xE0E040BF */
    {  3, 15},  /* Sony 12-bit     SIRC 0xA90 */
    {  8, 14},  /* Panasonic   KASEIKYO 0x40040100BCBD */
    {  9, 15},  /* na000 */
    { 11, 14},  /* na002 */
    { 13,  2},  /* na004 */
    { 15,  3},  /* na006 */
    { 20,  4},  /* na011            NEC 0x0AF5E817 */
    { 26,  5},  /* na017            NEC 0x1CE348B7 */
    { 27,  6},  /* na018            NEC 0x55AA38C7 */
    { 29,  1},  /* na020 */
    { 50, 15},  /* na041 */
    { 51,  7},  /* na042            NEC 0xC15E10EF */
    { 96,  8},  /* na087            NEC 0x18E710EF */
    {101,  9},  /* na092        SAMSUNG 0xA0A040BF */
    {112, 10},  /* na103        SAMSUNG 0x1010F00F */
    {113, 11},  /* na104            NEC 0x8E7110EF */
    {128,  1},  /* na119 */
    {132, 12},  /* na123            NEC 0x1CE338C7 */
    {134,  5},  /* na125 */
    {145, 13},  /* na136            NEC 0xDE010FC0 */
    {149, 14},  /* eu004 */
    {151,  3},  /* eu006 */
    {153,  4},  /* eu008 */
    {156, 13},  /* eu013 */
    {157,  6},  /* eu015 */
    {160, 12},  /* eu018 */
    {163,  7},  /* eu021 */
    {171,  1},  /* eu030 */
    {175,  8},  /* eu034 */
    {176, 11},  /* eu036 */
    {249,  9},  /* eu110 */
    {251, 10},  /* eu112 */
    {264,  7},  /* eu125 */
};
static const int ir_dup_group_count =
    (int)(sizeof(ir_dup_groups) / sizeof(ir_dup_groups[0]));

/* 15 groups, so the "already sent" set is a uint16_t bitmask. A 17th group
   would shift off the top of it and silently stop de-duplicating.

   Read out of the table rather than written down beside it: a number kept by
   hand only catches whoever raises the number, and the way this breaks is
   somebody adding a group and touching nothing else. */
static constexpr uint8_t ir_dup_group_max() {
    uint8_t max = 0;
    for (const IrDupGroup &g : ir_dup_groups) if (g.group > max) max = g.group;
    return max;
}
static_assert(ir_dup_group_max() <= 16,
              "ir_state.groups_sent is a uint16_t: more groups need a wider mask");

static uint8_t ir_entry_group(int i) {
    for (int k = 0; k < ir_dup_group_count; k++) {
        if (ir_dup_groups[k].entry == (uint16_t)i) return ir_dup_groups[k].group;
        if (ir_dup_groups[k].entry > (uint16_t)i) break;  /* sorted by entry */
    }
    return 0;
}

/*
 * Next entry after `after` that this job should send, or -1 at the end. Pass
 * -1 to get the first.
 *
 * `seen` carries the duplicate groups the job has already used and is updated
 * when an entry is taken, so the walk itself does the de-duplication and
 * counting the job is the same walk as running it.
 */
static int ir_next_entry(int after, uint8_t regions, uint16_t *seen) {
    for (int i = after + 1; i < IR_ENTRY_COUNT; i++) {
        if (!(ir_entry_regions(i) & regions)) continue;
        uint8_t g = ir_entry_group(i);
        if (g) {
            uint16_t bit = (uint16_t)(1u << (g - 1));
            if (*seen & bit) continue;
            *seen |= bit;
        }
        return i;
    }
    return -1;
}

/* --- Symbol builder --- */

static rmt_data_t ir_sym[IR_MAX_SYMBOLS];
static size_t ir_run_count = 0;

static void ir_build_reset() {
    ir_run_count = 0;
}

/* Append one run of the carrier being on (level 1) or off (level 0). */
static bool ir_add(uint8_t level, uint32_t us) {
    if (us == 0) return true;

    if (ir_run_count > 0) {
        rmt_data_t *s = &ir_sym[(ir_run_count - 1) / 2];
        bool second = ((ir_run_count - 1) & 1) != 0;
        uint32_t prev_us = second ? s->duration1 : s->duration0;
        uint8_t prev_level = second ? s->level1 : s->level0;
        if (prev_level == level && prev_us < IR_MAX_TICKS) {
            /* Biphase codes put two same-level half-bits back to back; the
               RMT stream wants one longer run, not two adjacent ones. */
            uint32_t room = IR_MAX_TICKS - prev_us;
            uint32_t take = (us < room) ? us : room;
            if (second) s->duration1 = prev_us + take;
            else        s->duration0 = prev_us + take;
            us -= take;
        }
    }

    /* Anything left over spills into further runs at the same level, which the
       peripheral clocks out back to back and so are indistinguishable from one
       long run. The TV-B-Gone table needs this: its trailing gaps reach 128ms,
       four times what the 15-bit duration field can hold. */
    while (us > 0) {
        uint32_t take = (us > IR_MAX_TICKS) ? IR_MAX_TICKS : us;
        if (ir_run_count / 2 >= IR_MAX_SYMBOLS) return false;
        rmt_data_t *s = &ir_sym[ir_run_count / 2];
        if (ir_run_count & 1) {
            s->duration1 = take;
            s->level1 = level;
        } else {
            s->duration0 = take;
            s->level0 = level;
            s->duration1 = 0;
            s->level1 = 0;
        }
        ir_run_count++;
        us -= take;
    }
    return true;
}

static size_t ir_symbol_count() {
    return (ir_run_count + 1) / 2;
}

/* --- Protocol encoders --- */

/* NEC, Samsung and Kaseikyo all encode a bit as a fixed mark plus a space
   whose length carries the value. */
static bool ir_encode_pulse_distance(uint32_t hdr_mark, uint32_t hdr_space,
                                     uint32_t bit_mark, uint32_t zero_space,
                                     uint32_t one_space, uint64_t value,
                                     uint8_t bits, uint32_t stop_mark) {
    if (!ir_add(1, hdr_mark) || !ir_add(0, hdr_space)) return false;
    for (int i = (int)bits - 1; i >= 0; i--) {
        bool one = ((value >> i) & 1ULL) != 0;
        if (!ir_add(1, bit_mark)) return false;
        if (!ir_add(0, one ? one_space : zero_space)) return false;
    }
    return ir_add(1, stop_mark);
}

/* Sony SIRC: 2.4ms header, then the value in the mark widths. */
static bool ir_encode_sony(uint64_t value, uint8_t bits) {
    if (!ir_add(1, 2400) || !ir_add(0, 600)) return false;
    for (int i = (int)bits - 1; i >= 0; i--) {
        bool one = ((value >> i) & 1ULL) != 0;
        if (!ir_add(1, one ? 1200 : 600)) return false;
        if (!ir_add(0, 600)) return false;
    }
    return true;
}

/* RC-5: 889us half-bits, biphase, a '1' is a space then a mark. The frame is
   14 bits: two start bits (both 1), the toggle, 5 address bits and 6 command
   bits, so the address sits above the command's 6 bits, not above 5. */
static bool ir_encode_rc5(uint8_t addr, uint8_t cmd, bool toggle) {
    const uint32_t half = 889;
    uint16_t frame = (uint16_t)((1u << 13) | (1u << 12) |
                                ((toggle ? 1u : 0u) << 11) |
                                ((addr & 0x1Fu) << 6) | (cmd & 0x3Fu));
    for (int i = 13; i >= 0; i--) {
        bool one = ((frame >> i) & 1u) != 0;
        if (!ir_add(one ? 0 : 1, half)) return false;
        if (!ir_add(one ? 1 : 0, half)) return false;
    }
    return true;
}

/* RC-6 mode 0: 444us unit, biphase the other way up ('1' is a mark then a
   space). Leader, start bit, three mode bits, a double-width toggle, then 8
   address and 8 command bits. */
static bool ir_encode_rc6(uint8_t addr, uint8_t cmd, bool toggle) {
    const uint32_t t = 444;
    if (!ir_add(1, 6 * t) || !ir_add(0, 2 * t)) return false;
    if (!ir_add(1, t) || !ir_add(0, t)) return false;              /* start bit '1' */
    for (int i = 0; i < 3; i++) {                                   /* mode 0 = 000 */
        if (!ir_add(0, t) || !ir_add(1, t)) return false;
    }
    if (toggle) {
        if (!ir_add(1, 2 * t) || !ir_add(0, 2 * t)) return false;
    } else {
        if (!ir_add(0, 2 * t) || !ir_add(1, 2 * t)) return false;
    }
    uint16_t data = (uint16_t)(((uint16_t)addr << 8) | cmd);
    for (int i = 15; i >= 0; i--) {
        bool one = ((data >> i) & 1u) != 0;
        if (!ir_add(one ? 1 : 0, t)) return false;
        if (!ir_add(one ? 0 : 1, t)) return false;
    }
    return true;
}

static bool ir_encode(const IrCode *c, bool toggle) {
    switch (c->proto) {
        case IR_NEC:
            return ir_encode_pulse_distance(9000, 4500, 560, 560, 1690,
                                            c->value, c->bits, 560);
        case IR_SAMSUNG:
            return ir_encode_pulse_distance(4500, 4500, 560, 560, 1690,
                                            c->value, c->bits, 560);
        case IR_KASEIKYO:
            return ir_encode_pulse_distance(3456, 1728, 432, 432, 1296,
                                            c->value, c->bits, 432);
        case IR_SONY:
            return ir_encode_sony(c->value, c->bits);
        case IR_RC5:
            return ir_encode_rc5((uint8_t)((c->value >> 8) & 0x1F),
                                 (uint8_t)(c->value & 0x3F), toggle);
        case IR_RC6:
            return ir_encode_rc6((uint8_t)((c->value >> 8) & 0xFF),
                                 (uint8_t)(c->value & 0xFF), toggle);
    }
    return false;
}

/*
 * A TV-B-Gone code is not a protocol, it is a capture: a small table of on/off
 * duration pairs plus a stream of fixed-width indices into it, MSB first. It
 * expands here, one frame at a time, rather than being stored expanded — 270
 * codes as microsecond arrays would be roughly ten times the flash and buy
 * nothing, since only one frame is ever in the symbol buffer.
 *
 * Durations are in units of 10us. The longest single run in the table is 128ms,
 * which ir_add() splits; the longest frame is 136 pairs, so 272 runs against
 * the 640 the buffer holds.
 */
static bool ir_encode_tvb(const IrTvbCode *c) {
    uint16_t bitpos = 0;

    for (uint8_t i = 0; i < c->count; i++) {
        uint8_t idx = 0;
        for (uint8_t b = 0; b < c->index_bits; b++) {
            idx = (uint8_t)((idx << 1) |
                            ((c->bits[bitpos >> 3] >> (7 - (bitpos & 7))) & 1u));
            bitpos++;
        }
        if (!ir_add(1, (uint32_t)c->pairs[idx * 2] * 10)) return false;
        if (!ir_add(0, (uint32_t)c->pairs[idx * 2 + 1] * 10)) return false;
    }
    return true;
}

/* --- Job state ---
 *
 * Ownership: only loop() (through ir_poll) writes ir_state and touches the RMT
 * channel. An HTTP handler that wants a job started fills ir_request and sets
 * ir_request_pending; a full blast is tens of seconds, and no part of that may
 * happen on the web server task.
 */

enum { IR_JOB_IDLE = 0, IR_JOB_TVBGONE, IR_JOB_RAW };

static struct {
    uint8_t kind;
    uint16_t job_id;
    uint8_t regions;
    uint8_t repeat;
    int16_t only;           /* entry this job sends, or -1 for the whole region */
    int codes;              /* distinct codes in this job */
    int total;              /* transmissions in this job */
    int step;               /* transmissions started */
    int sent;               /* transmissions completed */
    int cursor;             /* entry being sent, -1 once the region runs out */
    uint16_t groups_sent;   /* duplicate groups this blast has already used */
    uint8_t rep;            /* frames of the entry at cursor already sent */
    uint8_t rep_target;     /* frames that entry needs: sends * repeat */
    unsigned long started_ms;
    unsigned long next_at;  /* earliest millis() for the next frame */
    char label[24];         /* code being sent, or the last one sent */
    char result[16];        /* how the last finished job ended */
} ir_state;

static struct {
    uint8_t kind;
    uint8_t regions;
    uint8_t repeat;
    int16_t only;
    uint16_t job_id;
} ir_request;

static volatile bool ir_request_pending = false;
static volatile bool ir_stop_requested = false;
static bool ir_tx_busy = false;
static bool ir_ready = false;
static uint16_t ir_job_seq = 0;

/* Raw frame staged by POST /ir/send */
static uint16_t ir_raw_us[IR_MAX_RAW];
static int ir_raw_count = 0;
static uint8_t ir_raw_khz = 38;
static uint8_t ir_raw_duty = 33;

/* Carrier currently programmed into the channel. 0 Hz means it is switched
   off, which is a state a baseband code can legitimately leave it in. */
static uint32_t ir_carrier_hz = 0;
static uint8_t ir_carrier_duty = 0;
static bool ir_carrier_set = false;

/*
 * What a blast over this region will actually do. It walks with ir_next_entry()
 * rather than counting the region mask directly, so the duplicate groups it
 * skips are exactly the ones the run will skip and the reported totals cannot
 * drift from the job.
 *
 * Frames are the per-entry protocol requirement times whatever multiplier the
 * caller asked for. Not codes * repeat — almost every entry needs exactly one
 * frame, and pretending otherwise is what made this slow.
 */
static void ir_count_job(uint8_t regions, uint8_t repeat, int *codes, int *frames) {
    uint16_t seen = 0;
    int c = 0, f = 0;
    for (int i = ir_next_entry(-1, regions, &seen); i >= 0;
         i = ir_next_entry(i, regions, &seen)) {
        c++;
        f += ir_entry_sends(i) * repeat;
    }
    if (codes) *codes = c;
    if (frames) *frames = f;
}

static int ir_count_region(uint8_t regions) {
    int codes = 0;
    ir_count_job(regions, 1, &codes, NULL);
    return codes;
}

static const char *ir_region_name(uint8_t regions) {
    if ((regions & IR_REGION_ALL) == IR_REGION_ALL) return "all";
    if (regions & IR_REGION_NA) return "na";
    if (regions & IR_REGION_EU) return "eu";
    return "none";
}

static bool ir_busy() {
    return ir_state.kind != IR_JOB_IDLE || ir_request_pending || ir_tx_busy;
}

/* hz == 0 sends the frame baseband: the mark is the LED held on with nothing
   modulating it, which is what eighteen of the TV-B-Gone codes want. */
static bool ir_set_carrier(uint32_t hz, uint8_t duty) {
    if (ir_carrier_set && hz == ir_carrier_hz && duty == ir_carrier_duty) return true;
    /* carrier_level goes straight into rmt_carrier_config_t.flags.
       polarity_active_low (esp32-hal-rmt.c), so false is what puts the burst
       on the mark. The header comment above rmtSetCarrier() describes this
       argument the other way round from what its own implementation does —
       do not "correct" this to true.

       The frequency still has to be a sane number when the carrier is off: the
       driver divides by it either way. */
    if (!rmtSetCarrier(PIN_IR_TX, hz != 0, false,
                       hz != 0 ? hz : 38000, duty / 100.0f)) {
        return false;
    }
    ir_carrier_hz = hz;
    ir_carrier_duty = duty;
    ir_carrier_set = true;
    return true;
}

static void ir_finish(const char *result) {
    snprintf(ir_state.result, sizeof(ir_state.result), "%s", result);
    event_add("ir: job %u %s, %d/%d frames", (unsigned)ir_state.job_id,
              result, ir_state.sent, ir_state.total);
    ir_state.kind = IR_JOB_IDLE;
    ir_stop_requested = false;
}

/* Build and hand off one frame. Returns false if it could not be encoded or
   the channel refused the write. */
static bool ir_start_step() {
    uint32_t hz;
    uint8_t duty;

    ir_build_reset();
    if (ir_state.kind == IR_JOB_RAW) {
        hz = (uint32_t)ir_raw_khz * 1000;
        duty = ir_raw_duty;
        for (int i = 0; i < ir_raw_count; i++) {
            if (!ir_add((i & 1) ? 0 : 1, ir_raw_us[i])) return false;
        }
        snprintf(ir_state.label, sizeof(ir_state.label), "raw");
    } else {
        if (ir_state.rep >= ir_state.rep_target) {
            ir_state.cursor = ir_next_entry(ir_state.cursor, ir_state.regions,
                                            &ir_state.groups_sent);
            if (ir_state.cursor < 0) return false;
            ir_state.rep = 0;
            ir_state.rep_target =
                (uint8_t)(ir_entry_sends(ir_state.cursor) * ir_state.repeat);
        }

        if (ir_state.cursor < ir_code_count) {
            const IrCode *c = &ir_codes[ir_state.cursor];
            /* RC-5 and RC-6 flip a toggle bit between key presses; a receiver
               that sees two identical frames reads the second as a held key. */
            if (!ir_encode(c, (ir_state.rep & 1) != 0)) return false;
            hz = (uint32_t)c->khz * 1000;
            duty = c->duty;
        } else {
            const IrTvbCode *c = &ir_tvb_codes[ir_state.cursor - ir_code_count];
            if (!ir_encode_tvb(c)) return false;
            hz = c->hz;
            duty = IR_TVB_DUTY;
        }
        snprintf(ir_state.label, sizeof(ir_state.label), "%s",
                 ir_entry_label(ir_state.cursor));
    }

    if (!ir_set_carrier(hz, duty)) return false;
    if (!rmtWriteAsync(PIN_IR_TX, ir_sym, ir_symbol_count())) return false;

    if (ir_state.kind == IR_JOB_TVBGONE) ir_state.rep++;
    ir_state.step++;
    ir_tx_busy = true;
    return true;
}

static void ir_begin_request() {
    memset(&ir_state, 0, sizeof(ir_state));
    /* The stop flag is not part of ir_state. A stop that raced the end of the
       previous job would otherwise still be set here and kill this one on its
       first pass. */
    ir_stop_requested = false;
    ir_state.kind = ir_request.kind;
    ir_state.job_id = ir_request.job_id;
    ir_state.repeat = ir_request.repeat;
    ir_state.regions = ir_request.regions;
    ir_state.only = ir_request.only;
    ir_state.started_ms = millis();
    ir_state.next_at = millis();

    if (ir_state.kind == IR_JOB_RAW) {
        ir_state.codes = 1;
        ir_state.total = ir_state.repeat;
        event_add("ir: job %u raw, %d entries at %ukHz",
                  (unsigned)ir_state.job_id, ir_raw_count, (unsigned)ir_raw_khz);
    } else if (ir_state.only >= 0) {
        /* One code, so the cursor is parked on it from the start and never
           advances: total is exactly the frames that entry needs, and ir_poll()
           ends the job on reaching it before ir_start_step() looks for a next
           entry. Everything downstream — encoder, carrier, gaps, stop — is the
           blast's, unchanged. */
        ir_state.cursor = ir_state.only;
        ir_state.rep = 0;
        ir_state.rep_target =
            (uint8_t)(ir_entry_sends(ir_state.only) * ir_state.repeat);
        ir_state.codes = 1;
        ir_state.total = ir_state.rep_target;
        event_add("ir: job %u code %s x%u", (unsigned)ir_state.job_id,
                  ir_entry_label(ir_state.only), (unsigned)ir_state.repeat);
    } else {
        /* cursor sits before the first match with its repeat quota spent, so
           that the first ir_start_step() advances onto it like any other, and
           groups_sent starts empty so the walk skips the same duplicates the
           count below just simulated. */
        ir_state.cursor = -1;
        ir_state.rep = 0;
        ir_state.rep_target = 0;
        ir_state.groups_sent = 0;
        ir_count_job(ir_state.regions, ir_state.repeat,
                     &ir_state.codes, &ir_state.total);
        event_add("ir: job %u tvbgone region=%s, %d codes x%u",
                  (unsigned)ir_state.job_id, ir_region_name(ir_state.regions),
                  ir_state.codes, (unsigned)ir_state.repeat);
    }

    if (ir_state.total == 0) ir_finish("empty");
}

/*
 * Called from loop(). Runs until the job is waiting on something real — the
 * peripheral, the gap clock, or a request that has not arrived — and then
 * hands the CPU back. Nothing here blocks or spins: every iteration either
 * makes progress or returns.
 *
 * The loop matters. Collecting a completion and queueing the next frame used
 * to be two passes of loop(), which meant the cadence of a 300-frame blast was
 * set by unrelated work elsewhere in the pass. Now a completion that lands with
 * its gap already elapsed starts the next frame immediately.
 */
static void ir_poll() {
    if (!ir_ready) return;

    for (;;) {
        /* A frame in flight owns the channel and the symbol buffer until the
           peripheral says it is done. */
        if (ir_tx_busy) {
            if (!rmtTransmitCompleted(PIN_IR_TX)) return;
            ir_tx_busy = false;
            ir_state.sent++;
            /* More frames of the same code need only enough silence to be
               counted separately; moving to a different code needs the
               receiver's AGC to settle first. A raw job is one frame repeated,
               so it is always the former. */
            bool same_code = (ir_state.kind == IR_JOB_RAW) ||
                             (ir_state.rep < ir_state.rep_target);
            ir_state.next_at =
                millis() + (same_code ? IR_REPEAT_GAP_MS : IR_CODE_GAP_MS);
            continue;
        }

        if (ir_state.kind == IR_JOB_IDLE) {
            if (ir_request_pending) {
                ir_begin_request();
                ir_request_pending = false;
                continue;
            }
            return;
        }

        if (ir_stop_requested) {
            ir_finish("stopped");
            return;
        }
        if ((long)(millis() - ir_state.next_at) < 0) return;
        if (ir_state.step >= ir_state.total) {
            ir_finish("done");
            return;
        }
        if (!ir_start_step()) {
            ir_finish("tx failed");
            return;
        }
        /* Queued. The peripheral now owns it for a hundred-odd milliseconds,
           which is everyone else's turn. */
        return;
    }
}

/* This file used to also format its own line for the clock face's note row,
   which loop() drew whenever that row was free. That was the single-supplier
   arrangement: one job worth showing, one line, no arbitration. The blast now
   publishes a percentage into skills/progress.cpp like any other supplier and
   the note row draws whichever job wins, so the line is gone from here. The
   frame counts it carried are still on the blast screen in ui.h and in
   GET /ir/tvbgone/status; what the clock face shows instead is a labelled bar,
   which is the thing that is legible across a room. */

/* --- Job control, independent of the transport ---
 *
 * The on-device menu and POST /ir/tvbgone start the same job through these two
 * functions. Neither knows about HTTP; both are called from the loop() task
 * (the UI) and from the web server task (the endpoints), which is safe for the
 * same reason the endpoints were already safe — they only stage a request that
 * ir_poll() picks up.
 */

/* Returns the job id, or 0 if a job is already running or the region is
   empty. */
static uint16_t ir_start_tvbgone(uint8_t regions, uint8_t repeat) {
    if (!ir_ready || ir_busy()) return 0;
    if (ir_count_region(regions) == 0) return 0;

    ir_request.kind = IR_JOB_TVBGONE;
    ir_request.regions = regions;
    ir_request.repeat = repeat;
    ir_request.only = -1;
    ir_request.job_id = ++ir_job_seq;
    ir_request_pending = true;
    return ir_request.job_id;
}

/*
 * One named code, same machinery. It exists because the database is mostly
 * POWER TOGGLE rather than a discrete off: a full blast hits a given
 * television with every code it answers to, and each one flips it again, so a
 * set that contains three codes a Samsung recognises leaves it off, on, off.
 * Upstream behaves the same way — it is the data, not the port. Sending one
 * code is what "turn off the thing in front of me" actually needs.
 *
 * The frame is built by the same encoder from the same table entry as it is
 * inside a blast; only the walk over the table is skipped.
 */
static uint16_t ir_start_code(int index, uint8_t repeat) {
    if (!ir_ready || ir_busy()) return 0;
    if (index < 0 || index >= ir_code_count) return 0;

    ir_request.kind = IR_JOB_TVBGONE;
    ir_request.regions = ir_codes[index].regions;
    ir_request.repeat = repeat;
    ir_request.only = (int16_t)index;
    ir_request.job_id = ++ir_job_seq;
    ir_request_pending = true;
    return ir_request.job_id;
}

/* Returns whether there was anything to stop. loop() ends the job on its next
   pass; a frame already handed to the peripheral finishes first. */
static bool ir_stop_job() {
    if (ir_state.kind == IR_JOB_IDLE) return false;
    ir_stop_requested = true;
    return true;
}

/* Snapshot of the running (or last finished) job, for the on-device progress
   screen. Copied out rather than exposing ir_state, which ir_poll() owns. */
struct IrProgress {
    bool running;
    int sent;
    int total;
    unsigned long elapsed_ms;
    const char *label;   /* code being sent, "" before the first frame */
    const char *result;  /* how the last job ended, "" if none has */
};

static IrProgress ir_progress() {
    IrProgress p;
    p.running = (ir_state.kind != IR_JOB_IDLE);
    p.sent = ir_state.sent;
    p.total = ir_state.total;
    p.elapsed_ms = millis() - ir_state.started_ms;
    p.label = ir_state.label;
    p.result = ir_state.result;
    return p;
}

/* --- Endpoints --- */

static const SkillEndpoint ir_endpoints[] = {
    {"POST", "/ir/tvbgone",        "Start a TV power-off blast {region, repeat} or one code {code}"},
    {"GET",  "/ir/tvbgone/status", "Blast progress: running, index/total, last code"},
    {"POST", "/ir/tvbgone/stop",   "Abort a running blast"},
    {"POST", "/ir/send",           "Send one raw frame {khz, duty, raw[], repeat}"},
    {NULL, NULL, NULL}
};

static const char *ir_describe() {
    return "## Skill: ir\n\n"
           "Infrared transmitter on the onboard LED (GPIO2), driven by the RMT\n"
           "peripheral. Carrier and duty come from the code being sent.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /ir/tvbgone | Start a blast: `{\"region\":\"eu\"\\|\"na\"\\|\"all\",\"repeat\":1-10}` (defaults `all`, 1), or one code: `{\"code\":\"samsung\"}` |\n"
           "| GET | /ir/tvbgone/status | Running/idle, frames sent, elapsed, last code |\n"
           "| POST | /ir/tvbgone/stop | Abort the running job |\n"
           "| POST | /ir/send | One raw frame: `{\"khz\":38,\"duty\":33,\"raw\":[9000,4500,...],\"repeat\":1}` |\n\n"
           "### Behaviour\n\n"
           "Every call returns immediately and nothing blocks: frames are\n"
           "queued into the RMT peripheral and collected when it reports them\n"
           "done. Only one job runs at a time — starting another while one is\n"
           "in flight returns 409. A blast can be stopped at any point, from\n"
           "the API or by clicking the knob, and stops within a frame.\n\n"
           "Duration, on the default `repeat` of 1:\n\n"
           "| Region | Codes | Frames | Airtime | Gaps | Total |\n"
           "|--------|-------|--------|---------|------|-------|\n"
           "| na | 138 | 144 | 23.6s | 14.1s | **37.7s** |\n"
           "| eu | 141 | 147 | 23.2s | 14.4s | **37.5s** |\n"
           "| all | 258 | 264 | 43.7s | 26.1s | **69.8s** |\n\n"
           "Those code counts are lower than the table size because a blast\n"
           "sends each distinct command once: see \"Duplicate commands\" below.\n\n"
           "Airtime is fixed by the codes themselves. The rest is two gaps:\n"
           "100ms between two different codes, so the receiver's AGC settles\n"
           "before a frame it has not seen, and 45ms between repeats of one\n"
           "code, which only has to be long enough for them to count as two.\n\n"
           "`repeat` is a multiplier over what each code's protocol already\n"
           "requires, and the default of 1 does not mean one frame each. The\n"
           "three Sony entries need three frames before a SIRC receiver acts on\n"
           "them, and they get three. Everything else needs one — the\n"
           "TV-B-Gone codes carry any repetition they need inside the frame,\n"
           "which is why some run to 136 pairs. Raising `repeat` multiplies the\n"
           "whole job and is rarely what you want.\n\n"
           "Progress appears on the device screen whenever the bottom line is\n"
           "not already carrying setup information.\n\n"
           "### Duplicate commands\n\n"
           "Every code here is a power *toggle*, so a receiver that is sent the\n"
           "same command twice in one run ends up back on. The database does\n"
           "hold repeats: it is a set of captures, and one command captured\n"
           "from two remotes is two entries whose timings differ slightly and\n"
           "whose decoded content does not. The NA and EU lists overlap by more\n"
           "than the five entries upstream merged, because that merge compared\n"
           "byte streams rather than what a receiver reads off them.\n\n"
           "Decoding the table found 15 commands appearing more than once among\n"
           "the 279 entries, covering 36 of them — including Samsung\n"
           "`0xE0E040BF` and Panasonic `0x40040100BCBD`, each of which is in\n"
           "the NA list, the EU list *and* the hand-written set, and Sony SIRC\n"
           "`0xA90`, which is in the NA list twice and hand-written besides. A\n"
           "blast now sends the first entry of each group and skips the rest, so\n"
           "`all` drops 21 redundant entries and no receiver is toggled twice.\n"
           "Single-code sends are never filtered.\n\n"
           "The decode covers the three pulse-distance families that carry an\n"
           "address and command in the clear — NEC, Samsung and Kaseikyo — plus\n"
           "Sony SIRC, and not RC-5 or RC-6; 160 of the 270 upstream entries\n"
           "decode as none of the four. So this is not a proof that nothing\n"
           "repeats, only that nothing repeats in the protocols that can be\n"
           "read.\n\n"
           "### One code at a time\n\n"
           "`{\"code\":\"<name>\"}` sends that code and nothing else, through the\n"
           "same encoder and the same job the blast uses. It exists because\n"
           "these are power *toggles*, not discrete off commands: a blast hits\n"
           "a given television with every code it answers to, and each one\n"
           "flips it again, so a set holding three codes one receiver knows\n"
           "leaves it off, on, off. To turn off the device in front of you,\n"
           "name it.\n\n"
           "`code` and `region` cannot be combined. `repeat` still applies, and\n"
           "the per-code requirement still holds — a Sony entry sends three\n"
           "frames at `repeat` 1. Only the nine hand-written codes have names;\n"
           "the TV-B-Gone entries are captures numbered `NA 0031`, not brands.\n\n"
           "| Name | Protocol | Frames |\n"
           "|------|----------|--------|\n"
           "| LG/Zenith/Vizio | NEC | 1 |\n"
           "| Toshiba | NEC | 1 |\n"
           "| Samsung | Samsung | 1 |\n"
           "| Sony 12-bit | SIRC | 3 |\n"
           "| Sony 15-bit | SIRC | 3 |\n"
           "| Sony 20-bit | SIRC | 3 |\n"
           "| Philips RC-5 | RC-5 | 1 |\n"
           "| Philips RC-6 | RC-6 mode 0 | 1 |\n"
           "| Panasonic | Kaseikyo | 1 |\n\n"
           "Matching is case-insensitive, and an entry naming several vendors\n"
           "answers to each of them alone: `lg`, `Zenith` and `LG/Zenith/Vizio`\n"
           "all select the first row. `Sony` on its own resolves to the 12-bit\n"
           "entry (the original SIRC frame; 15 and 20 are extensions) and\n"
           "`Philips` to RC-5. The same nine are on the device under\n"
           "TV-B-Gone > By brand.\n\n"
           "### Code table\n\n"
           "279 codes, in two parts.\n\n"
           "Nine are encoded here from the published descriptions of six\n"
           "protocols (NEC, Samsung, Sony SIRC, Philips RC-5, Philips RC-6,\n"
           "Panasonic/Kaseikyo), covering LG, Zenith, Vizio, Toshiba, Samsung,\n"
           "Sony, Philips and Panasonic. They are sent first, in every region.\n\n"
           "The other 270 are the TV-B-Gone power-code database by Mitch\n"
           "Altman and Limor Fried, taken from Arduino-TV-B-Gone and kept in\n"
           "`ir_codes_tvbgone.h` under Creative Commons Attribution-ShareAlike\n"
           "— the one file in this repository that is not MIT. Its licence is\n"
           "bundled as `LICENSE-CC-BY-SA`.\n\n"
           "The region filter is real, and it is upstream's split: `na` covers\n"
           "North America, Asia and everywhere `eu` does not reach; `eu` covers\n"
           "Europe, the Middle East, Australia, New Zealand, parts of Africa\n"
           "and South America. After duplicate commands are dropped they select\n"
           "138, 141 and 258 codes. Note that upstream never sends both lists\n"
           "in one run — the original reads a region switch and the Bruce port\n"
           "asks — so `all` is this port's own option, and the de-duplication\n"
           "above is what makes it safe.\n\n"
           "### Raw frames\n\n"
           "`raw` is alternating mark/space durations in microseconds, starting\n"
           "with a mark. 3 to 512 entries, each 1-32767us (the RMT duration\n"
           "field is 15 bits at a 1us tick). `khz` 30-60, `duty` 10-50 percent,\n"
           "`repeat` 1-10.\n";
}

/* Body-carrying handlers must release the collected body on every exit path,
   including the unauthenticated one, so they take the body first and check the
   token with check_auth() rather than require_auth()'s early return. */
static char *ir_take_body(AsyncWebServerRequest *req) {
    char *body = (char*)req->_tempObject;
    req->_tempObject = nullptr;
    return body;
}

static void ir_send_json(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

static void ir_send_error(AsyncWebServerRequest *req, int code, const char *msg) {
    JsonDocument doc;
    doc["error"] = msg;
    ir_send_json(req, code, doc);
}

static void ir_register_routes(AsyncWebServer &server) {

    /* POST /ir/tvbgone — body optional:
       {"region":"eu"|"na"|"all","repeat":N} or {"code":"samsung","repeat":N}

       Exact matching, or this route would swallow its own children: see the
       note at the top of this file. */
    server.on(AsyncURIMatcher::exact("/ir/tvbgone"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = ir_take_body(req);
        if (!check_auth(req)) {
            free(body);
            ir_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!ir_ready) {
            free(body);
            ir_send_error(req, 503, "IR transmitter unavailable");
            return;
        }

        uint8_t regions = IR_REGION_ALL;
        /* 1 means "what each code's protocol requires", not "one frame each" —
           the Sony entries still go out three times. Anything higher is a
           multiplier on top, for the rare receiver that wants more. */
        int repeat = 1;
        /* -1 is the blast; anything else is the one named code to send. */
        int only = -1;
        bool has_region = false;

        if (body) {
            JsonDocument input;
            DeserializationError err = deserializeJson(input, body);
            free(body);
            if (err != DeserializationError::Ok) {
                ir_send_error(req, 400, "invalid JSON");
                return;
            }
            if (input["region"].is<const char*>()) {
                const char *r = input["region"].as<const char*>();
                if (strcmp(r, "na") == 0) regions = IR_REGION_NA;
                else if (strcmp(r, "eu") == 0) regions = IR_REGION_EU;
                else if (strcmp(r, "all") == 0) regions = IR_REGION_ALL;
                else {
                    ir_send_error(req, 400, "region must be na, eu or all");
                    return;
                }
                has_region = true;
            }
            if (input["code"].is<const char*>()) {
                only = ir_find_code(input["code"].as<const char*>());
                if (only < 0) {
                    ir_send_error(req, 400, "unknown code; GET /skill lists the names");
                    return;
                }
                /* A region selects a set and a code selects one entry, so a
                   request carrying both is asking for two different jobs. */
                if (has_region) {
                    ir_send_error(req, 400, "code and region cannot be combined");
                    return;
                }
            }
            if (input["repeat"].is<int>()) {
                repeat = input["repeat"].as<int>();
                if (repeat < 1 || repeat > IR_REPEAT_MAX) {
                    ir_send_error(req, 400, "repeat must be 1-10");
                    return;
                }
            }
        }

        if (ir_busy()) {
            ir_send_error(req, 409, "a job is already running");
            return;
        }

        int codes = 1, frames = ir_entry_sends(only >= 0 ? only : 0) * repeat;
        if (only < 0) ir_count_job(regions, (uint8_t)repeat, &codes, &frames);
        if (codes == 0) {
            ir_send_error(req, 400, "no codes for that region");
            return;
        }

        /* Hand the job to loop(): the transmission itself must not happen on
           the web server task. The two rejections above are tested here as
           well as inside the start functions because they answer with
           different status codes; the call can still come back empty if a
           request from the knob won the race, which is the same 409. */
        uint16_t job = (only >= 0) ? ir_start_code(only, (uint8_t)repeat)
                                   : ir_start_tvbgone(regions, (uint8_t)repeat);
        if (job == 0) {
            ir_send_error(req, 409, "a job is already running");
            return;
        }

        JsonDocument doc;
        doc["ok"] = true;
        doc["job"] = job;
        if (only >= 0) doc["code"] = ir_codes[only].brand;
        else           doc["region"] = ir_region_name(regions);
        doc["repeat"] = repeat;
        doc["codes"] = codes;
        doc["transmissions"] = frames;
        ir_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    /* GET /ir/tvbgone/status */
    server.on(AsyncURIMatcher::exact("/ir/tvbgone/status"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        bool running = (ir_state.kind != IR_JOB_IDLE);
        doc["running"] = running;
        doc["kind"] = (ir_state.kind == IR_JOB_RAW) ? "raw"
                    : (ir_state.kind == IR_JOB_TVBGONE ? "tvbgone" : "idle");
        doc["job"] = ir_state.job_id;
        doc["sent"] = ir_state.sent;
        doc["index"] = ir_state.step;
        doc["total"] = ir_state.total;
        doc["codes"] = ir_state.codes;
        doc["repeat"] = ir_state.repeat;
        if (ir_state.only >= 0) doc["code"] = ir_entry_label(ir_state.only);
        else                    doc["region"] = ir_region_name(ir_state.regions);
        doc["elapsed_ms"] = (unsigned long)(millis() - ir_state.started_ms);
        if (ir_state.label[0]) doc["last"] = ir_state.label;
        if (!running && ir_state.result[0]) doc["result"] = ir_state.result;
        ir_send_json(req, 200, doc);
    });

    /* POST /ir/tvbgone/stop */
    server.on(AsyncURIMatcher::exact("/ir/tvbgone/stop"), HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        bool running = ir_stop_job();

        JsonDocument doc;
        doc["ok"] = true;
        doc["stopped"] = running;
        doc["job"] = ir_state.job_id;
        doc["sent"] = ir_state.sent;
        ir_send_json(req, 200, doc);
    });

    /* POST /ir/send — body: {"khz":38,"duty":33,"raw":[...],"repeat":1} */
    server.on(AsyncURIMatcher::exact("/ir/send"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = ir_take_body(req);
        if (!check_auth(req)) {
            free(body);
            ir_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!ir_ready) {
            free(body);
            ir_send_error(req, 503, "IR transmitter unavailable");
            return;
        }
        if (!body) {
            ir_send_error(req, 400, "no body");
            return;
        }

        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            ir_send_error(req, 400, "invalid JSON");
            return;
        }

        if (!input["raw"].is<JsonArray>()) {
            ir_send_error(req, 400, "raw must be an array of microsecond durations");
            return;
        }
        JsonArray raw = input["raw"].as<JsonArray>();
        if (raw.size() < 3 || raw.size() > IR_MAX_RAW) {
            ir_send_error(req, 400, "raw must hold 3-512 entries");
            return;
        }

        int khz = input["khz"].is<int>() ? input["khz"].as<int>() : 38;
        int duty = input["duty"].is<int>() ? input["duty"].as<int>() : 33;
        int repeat = input["repeat"].is<int>() ? input["repeat"].as<int>() : 1;
        if (khz < 30 || khz > 60) {
            ir_send_error(req, 400, "khz must be 30-60");
            return;
        }
        if (duty < 10 || duty > 50) {
            ir_send_error(req, 400, "duty must be 10-50 percent");
            return;
        }
        if (repeat < 1 || repeat > IR_REPEAT_MAX) {
            ir_send_error(req, 400, "repeat must be 1-10");
            return;
        }

        if (ir_busy()) {
            ir_send_error(req, 409, "a job is already running");
            return;
        }

        /* Validated into the staging buffer before the job is handed over; a
           run longer than the 15-bit duration field cannot be transmitted. */
        int n = 0;
        uint32_t duration = 0;
        for (JsonVariant v : raw) {
            if (!v.is<int>()) {
                ir_send_error(req, 400, "raw entries must be integers");
                return;
            }
            int us = v.as<int>();
            if (us < 1 || us > IR_MAX_TICKS) {
                ir_send_error(req, 400, "raw entries must be 1-32767 microseconds");
                return;
            }
            ir_raw_us[n++] = (uint16_t)us;
            duration += (uint32_t)us;
        }
        ir_raw_count = n;
        ir_raw_khz = (uint8_t)khz;
        ir_raw_duty = (uint8_t)duty;

        ir_request.kind = IR_JOB_RAW;
        ir_request.regions = 0;
        ir_request.repeat = (uint8_t)repeat;
        ir_request.only = -1;
        ir_request.job_id = ++ir_job_seq;
        ir_request_pending = true;

        JsonDocument doc;
        doc["ok"] = true;
        doc["job"] = ir_request.job_id;
        doc["entries"] = n;
        doc["khz"] = khz;
        doc["duty"] = duty;
        doc["repeat"] = repeat;
        doc["duration_us"] = (unsigned long)duration;
        ir_send_json(req, 200, doc);
    }, NULL, handle_body_collect);
}

static const Skill ir_skill = {
    .name = "ir",
    .version = "0.3.0",
    .describe = ir_describe,
    .endpoints = ir_endpoints,
    .register_routes = ir_register_routes
};

static void skill_ir_init() {
    memset(&ir_state, 0, sizeof(ir_state));
    /* 0 is a valid entry index, so idle has to be spelled out rather than left
       to the memset. */
    ir_state.only = -1;

    /* 1MHz tick, so a duration count is microseconds. Two memory blocks rather
       than one: the copy encoder refills from an interrupt, and WiFi plus
       AsyncTCP make that interrupt's latency worth padding against. */
    ir_ready = rmtInit(PIN_IR_TX, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_2, 1000000);
    if (!ir_ready) {
        event_add("ir: RMT init failed on pin %d", PIN_IR_TX);
    } else {
        /* The channel idles low between frames, which is the LED off. */
        rmtSetEOT(PIN_IR_TX, LOW);
    }

    skill_register(&ir_skill);
}
