#!/usr/bin/env bash
#
# Host-side regression test for the reply chips in src/ui.h: the row of labels
# drawn under a notification card, the knob's position on it, and the two
# decisions that turn that position into an answer — where the knob starts when
# a card opens, and whether a click records anything.
#
# Why this exists: the chip row is the only thing on this device that turns a
# question into an answer, and every way it can be wrong is silent. A row built
# wider than the band it is drawn in does not fail a build and does not throw —
# it draws off the edge of a 320px panel, or worse, sits inside a padding that
# no longer covers it and leaves the previous row's tail behind as ghosting. A
# row that brackets a chip nobody has chosen invites a click that answers a
# question the user was only dismissing, and a stored index past the labels that
# are there hands notify_choose_id() an answer nobody gave. None of that is
# visible from the outside, and the only witness is somebody standing in front
# of the device with the knob in their hand.
#
# What is NOT here any more, and where it went: the knob no longer feeds these
# functions. The card scrolls, so one turn of it is a position in the body first
# and a chip only past the end of the body, and that whole decision lives in
# ui_card_step() — pinned by tools/test_card.sh, including the two properties
# this file used to claim about a detent. Checks were left here describing
# detents that reached ui_chip_step() directly; they would have gone on passing
# while describing nothing the firmware does, which is worse than no coverage at
# all, so they were re-pointed rather than kept green.
#
# The measuring is real. TFT_eSPI's textWidth() for fonts 2..8 is the plain sum
# of that font's width table, and the stub below reads the table for font 2 out
# of the shipping library rather than carrying a copy of it — so "DETAILS fits
# and ROLLBACK does not" is a claim about the font the panel actually draws
# with, not about an average glyph width somebody guessed.
#
# What this DOES NOT cover, said plainly rather than papered over: the drawing
# itself and the wiring between the input and the decisions. Whether the row
# lands at MSG_HINT_Y, whether draw_field's padding erases what was there, which
# colour it comes out in, and whether a click reaches notify_choose_id() at all
# need TFT_eSPI, a panel and a hand. What ui_enter_card() and ui_poll() DO with
# the answers they get from ui_chip_start() and ui_chip_answers() is theirs, and
# a call site that ignored one would not be caught here — which is exactly why
# each of them is called in one place and does the deciding there, rather than
# being repeated as a condition somebody can quietly write differently.
# No coverage is claimed for any of that. Nor for one guard that no input
# reaches: the `if (budget < 1) budget = 1;` in ui_chip_row(), unreachable at a
# 300px row — four chips, the narrowest share this row can be cut into, still
# leave 61px each — which would only stop a division result of zero or less from
# reaching ui_ellipsis() if the band were ever made tiny.
#
# The code is sliced straight out of src/ui.h between its `host-test:begin fit`,
# `host-test:begin chips` and `host-test:end` markers, and the option types come
# out of src/skills/notify.cpp the same way, so this runs the real
# implementation rather than a copy that could drift.
#
# Usage: tools/test_chips.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ui="$here/../src/ui.h"
notify="$here/../src/skills/notify.cpp"
font="${TFT_ESPI_DIR:-$here/../.pio/libdeps/tembed/TFT_eSPI}/Fonts/Font16.c"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -f "$ui" ]     || { echo "cannot find $ui"; exit 1; }
[ -f "$notify" ] || { echo "cannot find $notify"; exit 1; }
if [ ! -f "$font" ]; then
    echo "cannot find Font16.c at $font"
    echo "run 'pio run -e tembed' once to fetch TFT_eSPI, or set TFT_ESPI_DIR"
    exit 1
fi

slice() {
    awk -v tag="$2" '
        $0 ~ ("host-test:begin " tag) { grab = 1; next }
        grab && /host-test:end/       { grab = 0; next }
        grab                          { print }
    ' "$1"
}

# Font 2's width table, taken from the library. Its one #ifdef is on
# TFT_ESPI_GRAVE_IS_DEGREE, which Font16.c #defines a few lines above the table
# itself and unconditionally — nothing in platformio.ini has to switch it on and
# nothing there can switch it off. So the #ifdef arm is the one the firmware
# compiles, and it is the one taken here; the #else arm is skipped. The two
# differ in one glyph, the backtick, at 5px rather than 4px, and a backtick is a
# character a label may legally carry.
widths="$(awk '
    /widtbl_f16\[96\]/         { grab = 1; next }
    !grab                      { next }
    /^[[:space:]]*#ifdef/      { skip = 0; next }
    /^[[:space:]]*#else/       { skip = 1; next }
    /^[[:space:]]*#endif/      { skip = 0; next }
    /^[[:space:]]*};/          { grab = 0; next }
    /^[[:space:]]*{/           { next }
    skip                       { next }
                               { print }
' "$font")"

n_widths=$(printf '%s\n' "$widths" | sed 's#//.*##' | tr ',' '\n' | grep -c '[0-9]' || true)
if [ "$n_widths" -ne 96 ]; then
    echo "read $n_widths widths out of $font, expected 96 — the table's shape changed"
    exit 1
fi

{
    cat <<PRELUDE
/* Generated by tools/test_chips.sh — do not edit. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

/* Font 2's width table, read out of TFT_eSPI's Fonts/Font16.c by the script
   that generated this file. Index 0 is char 32. */
static const unsigned char widtbl_f16[96] = {
${widths}
};
PRELUDE
    cat <<'STUB'

/* The panel, reduced to the one thing the code under test asks of it.
   TFT_eSPI::textWidth() for fonts 2..8 sums that font's width table and charges
   anything outside 32..127 as a space; this does the same, for font 2, which is
   the only font the chip row is ever drawn in. */
struct TftStub {
    int16_t textWidth(const char *string, uint8_t font) {
        (void)font;
        int32_t w = 0;
        for (const unsigned char *p = (const unsigned char *)string; *p; p++)
            w += widtbl_f16[(*p > 31 && *p < 128) ? *p - 32 : 0];
        return (int16_t)w;
    }
};
static TftStub tft;

STUB
    slice "$notify" types
    # The band the row is drawn into comes out of the card's own geometry now
    # rather than being read off a literal: MSG_HINT_W is the card's width, so
    # the budget the fitting divides up is compiled here exactly as the firmware
    # compiles it, and a card that changed width would move this test with it.
    slice "$ui" cardgeom
    slice "$ui" fit
    slice "$ui" chips
    cat <<'MAIN'

/* ---- test scaffolding ---- */

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
    else       { printf("  ok:   %s\n", what); }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %s — got \"%s\", wanted \"%s\"\n", what, got, want);
        failures++;
    } else {
        printf("  ok:   %s — \"%s\"\n", what, got);
    }
}

/* One option set, as the store would hold it. */
static uint8_t fill(NotifyOptions &op, const char *const *labels, uint8_t n) {
    memset(&op, 0, sizeof(op));
    for (uint8_t i = 0; i < n && i < NOTIFY_OPT_MAX; i++)
        snprintf(op.label[i], NOTIFY_OPT_LEN, "%s", labels[i]);
    return n;
}

/* The row, built into a buffer sized the way ui_draw_card() sizes its own, with
   a sentinel past the end so an overrun is caught rather than inferred. */
static char row_buf[UI_CHIP_ROW_LEN + 8];

static const char *row_of(const NotifyOptions &op, uint8_t n, int sel, int chosen) {
    memset(row_buf, '#', sizeof(row_buf));
    ui_chip_row(row_buf, UI_CHIP_ROW_LEN, op, n, sel, chosen, MSG_HINT_W, 2);
    for (size_t i = UI_CHIP_ROW_LEN; i < sizeof(row_buf); i++) {
        if (row_buf[i] != '#') {
            printf("  FAIL: ui_chip_row wrote past the end of its buffer\n");
            failures++;
            break;
        }
    }
    return row_buf;
}

/* Shorthand for the sets used over and over below. */
static uint8_t two(NotifyOptions &op) {
    const char *l[2] = { "Yes", "No" };
    return fill(op, l, 2);
}

static uint8_t four(NotifyOptions &op) {
    const char *l[4] = { "Yes", "No", "Later", "Details" };
    return fill(op, l, 4);
}

/* Every chip filled with the widest glyph font 2 has, at the longest a label is
   allowed to be — the worst case the BAND has to survive. */
static uint8_t widest(NotifyOptions &op, uint8_t n) {
    char m[NOTIFY_OPT_LEN];
    memset(m, 'M', NOTIFY_OPT_LEN - 1);
    m[NOTIFY_OPT_LEN - 1] = '\0';
    const char *l[NOTIFY_OPT_MAX] = { m, m, m, m };
    return fill(op, l, n);
}

/* The worst case the BUFFER has to survive, which is a different set and NOT
   the widest one: a label wide enough to be cut is what costs the most bytes,
   because the ellipsis adds three of them on top of everything kept. So the
   byte-worst label is the one that keeps the most characters and still gets
   cut — narrow glyphs to keep the count up, then enough wide ones on the end to
   push the whole label over its share of the band. Twelve '!' and three 'M' is
   that label at MSG_HINT_W 300: 66px against a 61px share, cut after the 'M'
   that fits, so 13 characters plus an ellipsis, 16 bytes of text in a chip that
   the whole-glyph fixture below would have filled with 15. */
#define LONGEST_LABEL "!!!!!!!!!!!!MMM"
#define LONGEST_CHIP  "!!!!!!!!!!!!M..."

static uint8_t longest(NotifyOptions &op, uint8_t n) {
    const char *l[NOTIFY_OPT_MAX] = { LONGEST_LABEL, LONGEST_LABEL,
                                      LONGEST_LABEL, LONGEST_LABEL };
    return fill(op, l, n);
}

/* Occurrences of `what` in `s`, overlapping ones included. */
static int occurrences(const char *s, const char *what) {
    int n = 0;
    for (const char *p = s; (p = strstr(p, what)) != NULL; p += 1) n++;
    return n;
}

static int width_of(const char *s) { return tft.textWidth(s, 2); }

/* How many chips a row holds, counted off the wrappers rather than off the
   labels: every chip opens with '[' or ' ' and closes with ']' or ' '. */
static int brackets(const char *s, char c) {
    int n = 0;
    for (const char *p = s; *p; p++) if (*p == c) n++;
    return n;
}

int main(void) {
    /* Any hang is a failure, not a hung test run. */
    alarm(60);

    NotifyOptions op;
    uint8_t n;

    printf("a message with nothing to pick draws no row at all\n");
    {
        n = fill(op, NULL, 0);
        check_str(row_of(op, 0, 0, -1), "", "no options is an empty string, not a stray bracket");
        (void)n;
        /* A zero-length buffer is not written to at all. */
        char tiny[1] = { '#' };
        ui_chip_row(tiny, 0, op, 2, 0, -1, MSG_HINT_W, 2);
        check(tiny[0] == '#', "a zero-length buffer is left alone");
    }

    printf("the selected chip is bracketed, and only that one\n");
    {
        n = two(op);
        check_str(row_of(op, n, 0, -1), "[YES] NO ", "the first of two is selected");
        check_str(row_of(op, n, 1, -1), " YES [NO]", "the second of two is selected");
        check(brackets(row_of(op, n, 1, -1), '[') == 1,
              "exactly one chip is bracketed");
        check_str(row_of(op, n, -1, -1), " YES  NO ",
                  "a selection past the end brackets nothing rather than the first");
        check_str(row_of(op, n, 5, -1), " YES  NO ",
                  "and neither does one past the other end");
    }

    printf("a card nobody has turned the knob on yet answers nothing\n");
    {
        /* The decision itself, in the two functions it was extracted into so
           that it could be checked here at all: where the knob starts when a
           card opens, and whether a click on it records an answer.

           This is the one thing on the device that turns somebody's reflex into
           data. A card can arrive in front of a person who never asked for it —
           ui_poll() opens one on arrival while the device sits on its clock —
           and the click that comes next has meant "dismiss" on every firmware
           before this one. Start the knob on a chip and that click is filed as
           a deliberate answer, and by convention the first chip is the one that
           says yes. There is no way to tell the two apart afterwards, which is
           why it is checked here rather than left to the eye. */
        check(ui_chip_start(-1, 4) == -1,
              "an unanswered message opens with nothing selected");
        check(ui_chip_start(0, 4) == 0,
              "and one answered on the first chip opens on that chip, not on nothing");
        check(ui_chip_start(2, 4) == 2, "an answer in the middle opens on itself");
        check(ui_chip_start(3, 4) == 3, "and one on the last chip too");
        check(ui_chip_start(4, 4) == -1,
              "an answer just past the labels there are opens on nothing");
        check(ui_chip_start(9, 4) == -1, "however far past it points");
        check(ui_chip_start(0, 0) == -1,
              "and a message carrying no options at all has nothing to open on");

        check(!ui_chip_answers(-1, 4),
              "a click with nothing selected does not answer, it acknowledges");
        check(ui_chip_answers(0, 4),
              "a click on the first chip does answer, so one detent is enough to reply");
        check(ui_chip_answers(3, 4), "and so does one on the last");
        check(!ui_chip_answers(0, 0),
              "a click on a message that asks nothing never answers");
        check(!ui_chip_answers(-1, 0), "with or without a selection");

        /* The property the pair leans on that IS this file's: -1 has to DRAW
           as no selection, or the panel is lying about what the click would do.
           What a detent does about it is ui_card_step()'s, and is checked in
           tools/test_card.sh — including that the knob cannot reach a chip
           until the body has been read to the end, which is the same invariant
           these two functions hold at entry and at click. */
        n = four(op);
        check(brackets(row_of(op, n, -1, -1), '[') == 0,
              "nothing selected draws no bracket anywhere on the row");
        check_str(row_of(op, n, -1, -1), " YES  NO  LATER  DETAILS ",
                  "which is the whole row, unbracketed");
    }

    printf("the answered chip is starred, and it survives coming back to it\n");
    {
        n = two(op);
        check_str(row_of(op, n, 0, 1), "[YES] *NO ",
                  "the answer is marked even when the knob sits elsewhere");
        check_str(row_of(op, n, 1, 1), " YES [*NO]",
                  "and both marks land on the same chip when they agree");
        check(strchr(row_of(op, n, 0, -1), '*') == NULL,
              "an unanswered message has no star anywhere on it");
        check(strchr(row_of(op, n, 0, 9), '*') == NULL,
              "and neither does one whose stored answer is past the end");
    }

    printf("labels are drawn in capitals, whatever was posted\n");
    {
        const char *l[2] = { "rollback", "Ship It" };
        n = fill(op, l, 2);
        check_str(row_of(op, n, 0, -1), "[ROLLBACK] SHIP IT ",
                  "lower and mixed case both come out as service text");
    }

    printf("the row fits the band it is drawn in, wherever the knob sits\n");
    {
        /* If this is ever false the row draws outside draw_field's padding, and
           the padding is the only thing erasing the previous row. Every
           selection is measured, not just the first: it is the widest row any
           detent can produce that the band has to hold.

           No separate check is made that the width is the SAME for every
           selection. It is — exactly one chip is bracketed at a time and every
           chip pays for both its wrappers — but that holds for any formatting
           that depends only on `i == sel`, so a check for it could not be made
           to fail and would only look like coverage. */
        for (uint8_t k = 1; k <= NOTIFY_OPT_MAX; k++) {
            int worst = 0;
            n = widest(op, k);
            for (int sel = 0; sel < (int)k; sel++) {
                int w = width_of(row_of(op, n, sel, sel));
                if (w > worst) worst = w;
            }
            if (worst > MSG_HINT_W) {
                printf("  FAIL: %u labels of %d widest characters: %dpx > %dpx\n",
                       (unsigned)k, NOTIFY_OPT_LEN - 1, worst, MSG_HINT_W);
                failures++;
            } else {
                printf("  ok:   %u labels of %d widest characters fit in %dpx (%dpx)\n",
                       (unsigned)k, NOTIFY_OPT_LEN - 1, MSG_HINT_W, worst);
            }
        }
    }

    printf("a label wider than its share is cut, not clipped\n");
    {
        /* The measured claim: at four options a chip has about seven capitals,
           so DETAILS survives whole and ROLLBACK does not. */
        printf("  ..   font 2 widths: DETAILS %dpx, ROLLBACK %dpx\n",
               width_of("DETAILS"), width_of("ROLLBACK"));

        const char *l[4] = { "Details", "Rollback", "Yes", "No" };
        n = fill(op, l, 4);
        const char *r = row_of(op, n, 0, -1);
        printf("  ..   row: \"%s\"\n", r);
        check(strstr(r, "DETAILS") != NULL,
              "DETAILS fits its quarter of the row and is drawn whole");
        check(strstr(r, "ROLLBACK") == NULL,
              "ROLLBACK does not fit and is not drawn whole");
        check(strstr(r, "...") != NULL,
              "and what is drawn instead ends in an ellipsis rather than mid-glyph");

        /* The band is a budget being divided by however many chips share it,
           not a fixed cap on a label: fewer chips means a longer label
           survives. Checked with one that is too wide for a quarter of the band
           and comfortable in a half, so the two answers cannot both be right. */
        printf("  ..   font 2 width: ROLLBACK NOW %dpx\n", width_of("ROLLBACK NOW"));
        const char *l4[4] = { "Rollback now", "Yes", "No", "Later" };
        n = fill(op, l4, 4);
        check(strstr(row_of(op, n, 0, -1), "ROLLBACK NOW") == NULL,
              "a label too wide for a quarter of the band is cut there");
        const char *l2[2] = { "Rollback now", "Ship" };
        n = fill(op, l2, 2);
        check(strstr(row_of(op, n, 0, -1), "ROLLBACK NOW") != NULL,
              "and the very same label is drawn whole when two chips share it");
    }

    printf("the row cache is big enough for the longest row that can be built\n");
    {
        /* ui_draw_card() builds into UI_CHIP_ROW_LEN and draw_field caches into
           the same size. A row too long for it loses a chip on the panel, and —
           worse, because it is silent — two rows that differ only past the end
           of the cache compare equal and the repaint is skipped. */

        /* Built into a buffer deliberately WIDER than the header's, so that the
           row is measured rather than cut down to whatever the header says and
           then found to fit it. */
        n = longest(op, NOTIFY_OPT_MAX);
        char big[UI_CHIP_ROW_LEN * 2];
        ui_chip_row(big, sizeof(big), op, n, NOTIFY_OPT_MAX - 1, 0, MSG_HINT_W, 2);
        printf("  ..   longest row: %u bytes, buffer %u bytes\n",
               (unsigned)strlen(big), (unsigned)UI_CHIP_ROW_LEN);
        /* The fixture is only the byte-worst case if it really is cut here: an
           ellipsis that stopped appearing — a wider band, a narrower glyph
           table — would quietly turn this back into the whole-label case that
           costs three bytes less a chip. */
        check(occurrences(big, LONGEST_CHIP) == NOTIFY_OPT_MAX,
              "the byte-worst label is one that ellipsises, and every chip carries it");
        check(strlen(big) < UI_CHIP_ROW_LEN,
              "the longest row fits the buffer with its terminator");

        const char *r = row_of(op, n, NOTIFY_OPT_MAX - 1, 0);
        check(occurrences(r, LONGEST_CHIP) == NOTIFY_OPT_MAX,
              "and the row built at the header's own size holds all four chips, "
              "none dropped for want of room");
        check(brackets(r, '[') == 1 && brackets(r, ']') == 1,
              "brackets and all");
    }

    printf("a buffer too small stops rather than running past the end\n");
    {
        /* Not a case that can happen from ui_draw_card(), which sizes its
           buffer from the same header. It is checked because the alternative to
           stopping is snprintf's return value — the length it WOULD have
           written — walking `out` past the end of the caller's buffer, and the
           next chip writing there. */
        const size_t small_n = 12;
        char small[small_n + 8];

        n = four(op);
        memset(small, '#', sizeof(small));
        ui_chip_row(small, small_n, op, n, 0, -1, MSG_HINT_W, 2);
        {
            int past = 0;
            for (size_t i = small_n; i < sizeof(small); i++)
                if (small[i] != '#') past = 1;
            check(!past, "nothing is written past the end of a short buffer");
        }
        /* Staying inside the buffer is only half of it. The chip that does not
           fit is rolled back whole, so what is left is a shorter row and not a
           row with half a label and an unclosed bracket in it — LATER is the
           chip that does not fit, and none of it is there. */
        check_str(small, "[YES] NO ",
                  "and what is left is whole chips, not a label cut in half");
    }

    printf("more options than the store can hold are ignored, not read\n");
    {
        n = four(op);
        char want[UI_CHIP_ROW_LEN];
        snprintf(want, sizeof(want), "%s", row_of(op, NOTIFY_OPT_MAX, 0, -1));
        check_str(row_of(op, 7, 0, -1), want,
                  "a count past NOTIFY_OPT_MAX draws exactly the labels there are");
    }

    printf("the click clamps a selection the message no longer has\n");
    {
        /* This is the clamp the click depends on: ui_chip_clamp() is what the
           card runs the stored selection through before handing it to
           notify_choose_id() as the answer. An index above the labels there are
           is one that did not come from a turn of the knob on this row — a
           `chosen` read back out of a snapshot is the way in — and it is
           clamped rather than trusted, here and again in notify_choose_id(). */
        check(ui_chip_clamp(0, 4) == 0, "a selection on the first chip is the first chip");
        check(ui_chip_clamp(3, 4) == 3, "and one on the last is the last");
        check(ui_chip_clamp(3, 2) == 1, "a selection past the end comes back to the last chip");
        check(ui_chip_clamp(9, 4) == 3, "however far past it is");
        /* On a message with no options both of these are zero, and zero is not
           a chip there — there is no chip to be. It is the only value the
           function can return when there is nothing to index, and what stops it
           being handed over as an answer is ui_chip_answers(), not this. So
           both names call it the same thing. */
        check(ui_chip_clamp(0, 0) == 0, "a message with no options clamps to zero");
        check(ui_chip_clamp(5, 0) == 0, "and so does a selection past the end of one");
        /* Never out of range on the way out, which is the only thing
           notify_choose_id() is being trusted with. */
        {
            int in_range = 1;
            for (uint8_t c = 1; c <= NOTIFY_OPT_MAX; c++)
                for (int s = -9; s <= 9; s++) {
                    int r = ui_chip_clamp(s, c);
                    if (r < 0 || r >= (int)c) in_range = 0;
                }
            check(in_range, "no stored selection produces an index off the labels");
        }
    }

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
MAIN
} > "$work/test.cpp"

cxx="${CXX:-c++}"
# -Wno-unused-function and -Wno-unused-variable for the same reason: the sliced
# regions are whole regions, and they carry functions and constants whose only
# callers live in the parts of the firmware this test does not slice.
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -Wno-unused-variable -o "$work/test" "$work/test.cpp"
"$work/test"
