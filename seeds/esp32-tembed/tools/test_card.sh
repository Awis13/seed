#!/usr/bin/env bash
#
# Host-side regression test for the scrolling notification card in src/ui.h:
# how a body is broken into lines, how many of those lines the card shows, where
# one turn of the knob leaves a reader, and where the scrollbar's thumb goes.
#
# Why this exists: the wrap was the one piece of this screen nothing covered.
# It sat between the two `fit` regions and was reachable only through
# ui_draw_card(), which needs TFT_eSPI and a panel, so every way it could be
# wrong was a thing somebody had to notice by eye — and most of them do not
# look like faults. A line one character too long is drawn past the edge of the
# card. A wrap that consumes nothing does not return. A scroll position that
# runs one line past the end shows a blank band and no way to tell whether the
# message ended there. A thumb normalised over the wrong length stops short of
# the bottom, which says "there is more" on the last line of the message.
#
# The line COUNTS are the point of the file. How many lines of a message a
# reader gets is the number this screen turns on, and until the offsets inside
# ui_draw_card() were promoted to the constants sliced in below, that number
# existed only in a comment. Here it is arithmetic over the real geometry and
# the real font.
#
# The measuring is real, in the same way tools/test_chips.sh is: the stub below
# reads font 2's 96-entry width table out of the shipping TFT_eSPI rather than
# carrying a copy, so "this line is 276px wide" is a claim about the font the
# panel draws with. The two glyph extremes the geometry leans on — 3px for the
# narrowest and 10px for the widest — are read back out of that same table and
# checked against the constants, because both are load-bearing: the narrow one
# sizes the line cache and the wide one bounds the line count.
#
# What this DOES NOT cover, said plainly: the drawing. Whether a row lands where
# the arithmetic says, whether draw_field's padding erases what was there,
# whether the thumb is visible against the tint, and whether a detent reaches
# ui_card_step() at all need TFT_eSPI, a panel and a hand. Nor the cache
# poisoning, beyond the one property that makes it work — that the byte cannot
# occur in a stored message — which is checked here against notify.cpp's own
# filter. No coverage is claimed for the rest of it.
#
# Nor for the guards no input from this file reaches. There are eleven of them,
# one of which is only half a statement, and they are LISTED and not merely
# counted: a count is a claim a reader has to take, and a list is one they can
# check. Every guard in the three sliced regions that is not below was removed on
# its own and a named check went red, which is what makes the list a boundary
# rather than an estimate. They are of two kinds.
#
# Arguments this file never passes, on functions it only ever calls properly:
# `!src || !out || max_lines <= 0` in ui_wrap_lines(), and `!dst` and `!src` in
# ui_line_text(). The `n == 0` half of ui_line_text()'s first guard IS reached —
# a zero-length buffer is a named case below — which is why it is a half
# statement that is listed and not the whole of it.
#
# Range clamps that a later clamp on the same variable already subsumes, so
# removing one changes no answer this file can ask for. In ui_card_step():
# `max_first < 0`, `chips < 0`, the two lines that reduce `steps` to the length
# of the axis, the clamp of an incoming chip index before it is added, and
# `at < 0` in the arm that starts from a scroll position — the clamp of `at`
# after the addition gives the same result in every one of those cases, and what
# they buy is that the addition stays in range for any int a spun encoder can
# hand in. In ui_scroll_thumb(): `visible < 1`, which stands between a band of no
# lines and a division by zero — the card only ever asks for two lines or four,
# and on a host whose divide returns zero instead of trapping, its absence is not
# observable at all; and `h > track_h`, which needs a track shorter than the
# smallest thumb, where the card's is 74px and nothing here asks for one under
# MSG_SCROLL_MIN_H.
#
# Usage: tools/test_card.sh

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

# Font 2's width table, taken from the library. The #ifdef arm is the one the
# firmware compiles — Font16.c #defines TFT_ESPI_GRAVE_IS_DEGREE itself, a few
# lines above the table and unconditionally — so it is the arm taken here, the
# same reading tools/test_chips.sh makes.
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
/* Generated by tools/test_card.sh — do not edit. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

/* Font 2's width table, read out of TFT_eSPI's Fonts/Font16.c by the script
   that generated this file. Index 0 is char 32. */
static const unsigned char widtbl_f16[96] = {
${widths}
};
PRELUDE
    cat <<'STUB'

/* The panel, reduced to the one thing the code under test asks of it, exactly
   as tools/test_chips.sh reduces it: textWidth() for fonts 2..8 sums that
   font's width table and charges anything outside 32..127 as a space. */
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
    slice "$notify" text
    slice "$ui" cardgeom
    slice "$ui" fit
    slice "$ui" wrap
    slice "$ui" scroll
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

static int width_of(const char *s) { return tft.textWidth(s, 2); }

/* The track the scrollbar is drawn down: the body band of a card that scrolls,
   which is always the untitled one — a body with more lines than the band holds
   is a body that gave its title up for them. Written as the arithmetic
   ui_draw_card() does rather than as the number it comes to, so that moving a
   row moves this too instead of leaving the test measuring a track the card no
   longer draws. */
#define CARD_TRACK (MSG_BODY_BOTTOM - MSG_BODY_DY_TOP + 1)

/* Wrap into a fixture the size the firmware wraps into, and hand back the
   count. A sentinel past the end catches an overrun rather than inferring it. */
static UiLine lines[MSG_BODY_MAX_LINES + 4];

static int wrap_of(const char *body, int max_px) {
    memset(lines, 0xEE, sizeof(lines));
    int n = ui_wrap_lines(body, lines, MSG_BODY_MAX_LINES, 2, max_px);
    for (int i = MSG_BODY_MAX_LINES; i < MSG_BODY_MAX_LINES + 4; i++) {
        if (lines[i].start != 0xEEEE || lines[i].len != 0xEEEE) {
            printf("  FAIL: ui_wrap_lines wrote past the end of its array\n");
            failures++;
            break;
        }
    }
    return n;
}

/* One line, copied out the way ui_draw_card() copies it. */
static char linebuf[MSG_LINE_LEN];

static const char *line_of(const char *body, int i) {
    ui_line_text(linebuf, sizeof(linebuf), body, lines[i]);
    return linebuf;
}

/* A body built by repeating `unit` until it is `n` bytes long. */
static char bodybuf[NOTIFY_BODY_LEN];

static const char *repeat_to(const char *unit, size_t n) {
    if (n > sizeof(bodybuf) - 1) n = sizeof(bodybuf) - 1;
    size_t u = strlen(unit), i = 0;
    for (; i < n; i++) bodybuf[i] = unit[i % u];
    bodybuf[i] = '\0';
    return bodybuf;
}

int main(void) {
    /* Any hang is a failure, not a hung test run. A wrap that consumes nothing
       is the way this file hangs, and it is a real fault of the shape here. */
    alarm(60);

    printf("the geometry says what the font says\n");
    {
        /* Both constants are read back out of the table rather than trusted:
           the narrow one is what sizes a line cache — and a cache narrower than
           the line drawn through it makes two different lines compare equal in
           draw_field, which leaves stale text under a moved scroll position —
           and the wide one is what bounds the line count. */
        int lo = 255, hi = 0;
        for (int i = 0; i < 96; i++) {
            if (widtbl_f16[i] < lo) lo = widtbl_f16[i];
            if (widtbl_f16[i] > hi) hi = widtbl_f16[i];
        }
        printf("  ..   font 2 glyphs run %dpx to %dpx\n", lo, hi);
        check(lo == MSG_GLYPH_W_MIN, "the narrowest glyph is what MSG_GLYPH_W_MIN says");
        check(hi == MSG_GLYPH_W_MAX, "the widest glyph is what MSG_GLYPH_W_MAX says");
    }

    printf("the card shows two lines with its title and four without\n");
    {
        /* The number this whole screen turns on. Stated as the arithmetic
           rather than as a constant, so that moving a row or the pitch moves
           the answer instead of moving a comment. */
        printf("  ..   body band %d..%d, line %dpx at pitch %d\n",
               MSG_BODY_DY, MSG_BODY_BOTTOM, MSG_LINE_H, MSG_BODY_PITCH);
        check(MSG_BODY_ROWS == 2, "two lines fit under the title");
        check(MSG_BODY_ROWS_MAX == 4, "and four fit when the title is not drawn");

        /* Each of the two layouts, checked from both sides: the last line it
           claims has to be inside the card, and one more must not be. */
        int dy[2] = { MSG_BODY_DY, MSG_BODY_DY_TOP };
        int rows[2] = { MSG_BODY_ROWS, MSG_BODY_ROWS_MAX };
        for (int k = 0; k < 2; k++) {
            int last = dy[k] + (rows[k] - 1) * MSG_BODY_PITCH + MSG_LINE_H - 1;
            int over = dy[k] + rows[k] * MSG_BODY_PITCH + MSG_LINE_H - 1;
            check(last <= MSG_BODY_BOTTOM,
                  k == 0 ? "the second line ends inside the card"
                         : "the fourth line ends inside the card");
            check(over > MSG_BODY_BOTTOM,
                  k == 0 ? "and a third would not"
                         : "and a fifth would not");
        }
        check(MSG_BODY_DY_TOP >= MSG_HDR_DY + MSG_LINE_H,
              "the untitled body starts below the source and age row");
        check(MSG_BODY_DY >= MSG_TITLE_DY + MSG_LINE_H,
              "and the titled body starts below the title");
    }

    printf("a body that fits is one line and is not cut\n");
    {
        const char *b = "the charger stopped";
        check(wrap_of(b, MSG_BODY_W) == 1, "one line");
        check_str(line_of(b, 0), b, "and all of it");
        check(wrap_of("", MSG_BODY_W) == 0, "an empty body is no lines at all");
        check(wrap_of("   ", MSG_BODY_W) == 0, "and neither is one of nothing but gaps");
    }

    printf("a long body breaks on spaces\n");
    {
        const char *b = "charge stopped at 77 percent because the pack reached "
                        "the temperature the charger gives up at, which is what "
                        "the register says and not what the label says. the "
                        "fuel gauge agrees with neither of them and has said so "
                        "since the last full cycle, which was four days ago.";
        int n = wrap_of(b, MSG_BODY_W);
        printf("  ..   %d lines\n", n);
        check(n > MSG_BODY_ROWS_MAX, "a body this long needs more than the band holds");
        /* Counted into a local and asserted below rather than incremented
           straight into `failures`: a loop that only ever adds failures leaves
           the check after it printing "ok" whatever it found. */
        int bad = 0;
        for (int i = 0; i < n; i++) {
            const char *l = line_of(b, i);
            if (width_of(l) > MSG_BODY_W) {
                printf("  ..   line %d is %dpx of %dpx: \"%s\"\n",
                       i, width_of(l), MSG_BODY_W, l);
                bad++;
            }
            if (l[0] == ' ' || l[0] == '\0') {
                printf("  ..   line %d starts on a gap or has nothing on it\n", i);
                bad++;
            }
            if (strchr(l, ' ') != NULL && l[strlen(l) - 1] == ' ') {
                printf("  ..   line %d ends on the gap it broke at\n", i);
                bad++;
            }
        }
        check(!bad, "every line fits the band, starts on a character and ends on one");

        /* Nothing is lost and nothing is shown twice: the lines run forwards
           through the body and only spaces fall between them. */
        int ordered = 1, dropped_only_spaces = 1;
        for (int i = 1; i < n; i++) {
            size_t end = lines[i - 1].start + lines[i - 1].len;
            if (lines[i].start < end) ordered = 0;
            for (size_t p = end; p < lines[i].start; p++)
                if (b[p] != ' ') dropped_only_spaces = 0;
        }
        check(ordered, "the lines run forwards through the body without overlapping");
        check(dropped_only_spaces, "and the only bytes between them are the gaps broken at");
        check(lines[n - 1].start + lines[n - 1].len == strlen(b),
              "the last line ends where the body does");
    }

    printf("a word wider than the line is cut rather than lost\n");
    {
        /* No space to break on, so the break is where the pixels run out. */
        const char *b = repeat_to("M", 200);
        int n = wrap_of(b, MSG_BODY_W);
        printf("  ..   %d lines of %d M's\n", n, 200);
        check(n > 1, "it takes more than one line");
        check(width_of(line_of(b, 0)) <= MSG_BODY_W, "the first line fits");
        check((int)lines[0].len == MSG_BODY_W / MSG_GLYPH_W_MAX,
              "and holds every M that fits, which is the widest glyph's share of the band");
        check(lines[n - 1].start + lines[n - 1].len == strlen(b),
              "and the last one ends where the word does");
    }

    printf("the wrap always consumes something\n");
    {
        /* The one way this function fails to return: a line that takes no
           bytes. A band narrower than a single glyph is the case that gets
           there, and it cannot arise from MSG_BODY_W — but a wrap that does not
           terminate takes the loop task with it, so it is floored rather than
           argued about. */
        const char *b = "MMMM";
        int n = wrap_of(b, 1);
        check(n == 4, "a band too narrow for any glyph still takes one character a line");
        check(lines[0].len == 1 && lines[3].len == 1, "one character each");
    }

    printf("no body can wrap into more lines than the array holds\n");
    {
        /* The bound MSG_BODY_MAX_LINES is derived from, tried against the
           patterns that are actually expensive: a short word followed by one
           too wide to break, which is what forces a cheap line, and runs of the
           narrowest and widest glyphs. The array is sized for the worst case,
           so a body that reached it would be a body silently cut short. */
        struct { const char *unit; const char *what; } fixtures[] = {
            { "a MMMMMMMMMMMMMMMMMMMMMMMMMMMMMM ", "a short word, then one too wide to break" },
            { "a ", "nothing but the shortest words there are" },
            { "M ", "the widest single letters" },
            { "!!!!!!!!!! ", "runs of the narrowest glyph" },
            { "M", "one unbreakable word of the widest glyph" },
            { "!", "one unbreakable word of the narrowest" },
            { " ", "nothing but gaps" },
            { "aa bb  cc   dd    ee ", "runs of gaps between short words" },
        };
        int worst = 0;
        const char *worst_what = "";
        for (size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); f++) {
            const char *b = repeat_to(fixtures[f].unit, NOTIFY_BODY_LEN - 1);
            int n = wrap_of(b, MSG_BODY_W);
            if (n > worst) { worst = n; worst_what = fixtures[f].what; }
            if (n >= MSG_BODY_MAX_LINES) {
                printf("  FAIL: %s wrapped into %d lines, the array holds %d\n",
                       fixtures[f].what, n, MSG_BODY_MAX_LINES);
                failures++;
            }
        }
        printf("  ..   worst of the fixtures: %d lines (%s), array %d\n",
               worst, worst_what, MSG_BODY_MAX_LINES);
        check(worst < MSG_BODY_MAX_LINES, "every fixture stays inside the array");

        /* And the same claim made the other way, over bodies nobody chose:
           random text out of an alphabet built to be awkward. */
        unsigned seed = 12345;
        int rand_worst = 0;
        for (int t = 0; t < 4000; t++) {
            static const char alpha[] = "MW! aiMM  W!";
            size_t len = 1 + (size_t)(seed % (NOTIFY_BODY_LEN - 1));
            for (size_t i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                bodybuf[i] = alpha[(seed >> 16) % (sizeof(alpha) - 1)];
            }
            bodybuf[len] = '\0';
            int n = wrap_of(bodybuf, MSG_BODY_W);
            if (n > rand_worst) rand_worst = n;
        }
        printf("  ..   worst of 4000 random bodies: %d lines\n", rand_worst);
        check(rand_worst < MSG_BODY_MAX_LINES,
              "and so does every random body tried");
    }

    printf("a line always fits the cache it is drawn through\n");
    {
        /* draw_field compares cache_size - 1 bytes, so a line longer than the
           cache makes two different lines compare equal and the repaint is
           skipped — stale text under a moved scroll position, which is exactly
           the fault ui_row[] at 69 bytes would have reintroduced. */
        const char *b = repeat_to("!", NOTIFY_BODY_LEN - 1);
        int n = wrap_of(b, MSG_BODY_W);
        size_t longest = 0;
        for (int i = 0; i < n; i++) if (lines[i].len > longest) longest = lines[i].len;
        printf("  ..   longest line of the narrowest glyph: %u bytes, cache %u\n",
               (unsigned)longest, (unsigned)MSG_LINE_LEN);
        check(longest + 1 <= MSG_LINE_LEN,
              "the widest line the band can hold fits the cache with its terminator");
        check(strlen(line_of(b, 0)) == lines[0].len,
              "and is copied out whole rather than cut on the way");
    }

    printf("a line that fills the band exactly keeps its last word\n");
    {
        /* One of the two faults these tests found rather than confirmed, and it
           had been there since the card had a body at all. The wrap measured a
           character before deciding whether it was a gap, so the space a full
           line runs out ON was not a break point: the break fell back to the
           space before it and the last word of the line went to the next one.
           A band exactly as wide as "alpha bravo" is the case — the space after
           "bravo" is the character the pixels run out on.
           Its own heading, because the property is the wrap's and not the copy
           out's: the case below happens to use the same fixture, and this must
           not go with it if somebody tidies that one away. */
        const char *b = "alpha bravo charlie";
        int n = wrap_of(b, width_of("alpha bravo"));
        check(n == 2, "the body breaks into two lines and not three");
        check_str(line_of(b, 0), "alpha bravo",
                  "the word before the gap the line ends on stays on that line");
        check_str(line_of(b, 1), "charlie", "and the next line starts after the gap");
    }

    printf("a line is copied out of the body it was measured in\n");
    {
        const char *b = "alpha bravo charlie";
        wrap_of(b, width_of("alpha bravo"));
        check_str(line_of(b, 0), "alpha bravo", "the first line");
        check_str(line_of(b, 1), "charlie", "and the second");

        /* A short destination cuts rather than overruns. Written into a
           buffer wider than the length handed over, with a sentinel past that
           length, so the overrun is a named check here rather than whatever
           the stack protector happens to say. */
        char small[24];
        memset(small, '#', sizeof(small));
        ui_line_text(small, 6, b, lines[0]);
        check(small[6] == '#' && small[23] == '#',
              "a short buffer is not written past the length it was given");
        check_str(small, "alpha", "and takes what fits and terminates");
        char none[4];
        memset(none, '#', sizeof(none));
        ui_line_text(none, 0, b, lines[0]);
        check(none[0] == '#', "a zero-length buffer is left alone");
    }

    printf("the knob stops at both ends of the text, and does not wrap\n");
    {
        /* The decision this whole screen rests on, and the one a later reader
           is most likely to "fix": every other knob on this device wraps. Text
           does not, because a reader who rolls off the last line and lands in
           the middle of the first sentence has lost their place with nothing on
           the panel to say so. Of the four shipped firmwares read for this,
           none wraps scrolled text. */
        UiCardPos p = { 0, -1 };

        p = ui_card_step(p, -1, 6, 0);
        check(p.first == 0 && p.chip == -1, "the top is a stop, not a way round to the bottom");
        p = ui_card_step(p, 3, 6, 0);
        check(p.first == 3, "three detents move three lines");
        p = ui_card_step(p, 99, 6, 0);
        check(p.first == 6 && p.chip == -1, "the bottom is a stop too");
        p = ui_card_step(p, 1, 6, 0);
        check(p.first == 6 && p.chip == -1,
              "and one more detent at the bottom stays there rather than starting again");
        p = ui_card_step(p, -99, 6, 0);
        check(p.first == 0, "and the whole way back is one jump");
    }

    printf("the whole backlog of detents is one move\n");
    {
        /* ui_encoder_steps() returns the net detents since the last pass, so a
           spin arrives as one number. Applying it a step at a time would be a
           redraw per detent. */
        UiCardPos a = { 0, -1 };
        UiCardPos b = { 0, -1 };
        a = ui_card_step(a, 4, 9, 0);
        for (int i = 0; i < 4; i++) b = ui_card_step(b, 1, 9, 0);
        check(a.first == b.first && a.first == 4,
              "four at once and four one at a time land on the same line");
    }

    printf("past the last line is the chip row, and nowhere else\n");
    {
        /* The chips are drawn under the body, and this is the motion that
           matches: rolling off the end of the text arrives on them. It is also
           the only way onto them, which is what keeps a reflex click from
           answering — see the card-nobody-opened case below. */
        UiCardPos p = { 0, -1 };
        p = ui_card_step(p, 2, 2, 3);
        check(p.first == 2 && p.chip == -1, "the last line is still the text");
        p = ui_card_step(p, 1, 2, 3);
        check(p.first == 2 && p.chip == 0, "one more is the first chip");
        p = ui_card_step(p, 1, 2, 3);
        check(p.chip == 1, "then the next");
        p = ui_card_step(p, 9, 2, 3);
        check(p.chip == 2, "and the last chip is where it stops");
        p = ui_card_step(p, -1, 2, 3);
        check(p.chip == 1, "back walks the row the same way");
        p = ui_card_step(p, -2, 2, 3);
        check(p.chip == -1 && p.first == 2,
              "and off the first chip is the last line of the text, not a wrap to the end of the row");
        p = ui_card_step(p, -1, 2, 3);
        check(p.chip == -1 && p.first == 1, "which keeps scrolling back up");
    }

    printf("a card nobody deliberately read cannot have a chip selected\n");
    {
        /* The invariant ui_chip_start() exists for, restated for a card that
           scrolls: a message can arrive in front of somebody who never asked
           for it, and the click that follows has meant "dismiss" on every
           firmware this device has run. The click may only answer what a
           deliberate turn of the knob selected — and past the whole body is how
           far that turn now has to go. */
        UiCardPos p = { 0, -1 };
        check(ui_card_step(p, 0, 5, 4).chip == -1, "no detent selects nothing");
        check(ui_card_step(p, -3, 5, 4).chip == -1, "and neither does turning back");
        int reached = 0;
        for (int s = 1; s <= 5; s++)
            if (ui_card_step(p, s, 5, 4).chip != -1) {
                printf("  ..   %d detents reached a chip with 5 lines still unread\n", s);
                reached++;
            }
        check(!reached, "no detent that leaves the body unread reaches a chip");
        check(ui_card_step(p, 6, 5, 4).chip == 0,
              "the first chip is one detent past the last line");

        /* And the case that is not a change: a message short enough not to
           scroll is answered in one detent, exactly as it was before this
           screen scrolled at all. */
        check(ui_card_step(p, 1, 0, 4).chip == 0,
              "on a card with nothing to scroll the first detent lands on the first chip");
        check(ui_card_step(p, -1, 0, 4).chip == -1,
              "and turning the other way selects nothing, because the top is a stop");
    }

    printf("the knob goes nowhere when there is nowhere to go\n");
    {
        UiCardPos p = { 0, -1 };
        UiCardPos q = ui_card_step(p, 5, 0, 0);
        check(q.first == 0 && q.chip == -1, "a short message with no options does not move");
        q = ui_card_step(p, -5, 0, 0);
        check(q.first == 0 && q.chip == -1, "in either direction");
    }

    printf("a position the message no longer supports is clamped, not trusted\n");
    {
        /* ui_chip_start() opens an answered card on its own answer, and that
           answer comes out of a snapshot — which is only as trustworthy as the
           file it came from. */
        UiCardPos stale = { 0, 9 };
        UiCardPos q = ui_card_step(stale, 0, 3, 2);
        check(q.chip == 1, "a chip past the labels there are comes back to the last one");
        stale.chip = 9;
        q = ui_card_step(stale, 0, 3, 0);
        check(q.chip == -1 && q.first == 3,
              "and a chip on a message that carries none is the bottom of the text");
        UiCardPos far = { 99, -1 };
        q = ui_card_step(far, 0, 3, 2);
        check(q.first == 3 && q.chip == -1, "a scroll past the end comes back to the end");
        UiCardPos neg = { -4, -1 };
        q = ui_card_step(neg, 0, 3, 2);
        check(q.first == 0, "and one before the start comes back to the start");
    }

    printf("no turn of the knob produces a position off the card\n");
    {
        int off = 0;
        for (int max_first = 0; max_first <= 6; max_first++)
            for (int chips = 0; chips <= 4; chips++)
                for (int f = -2; f <= 8; f++)
                    for (int c = -2; c <= 6; c++)
                        for (int s = -9; s <= 9; s++) {
                            UiCardPos in = { f, c };
                            UiCardPos out = ui_card_step(in, s, max_first, chips);
                            if (out.first < 0 || out.first > max_first) off = 1;
                            if (out.chip < -1 || out.chip >= chips) off = 1;
                            if (out.chip >= 0 && out.first != max_first) off = 1;
                        }
        check(!off, "every position is a line on the card or a chip the message has");
    }

    printf("the scrollbar's thumb says where in the body a reader is\n");
    {
        int y = 0, h = 0;
        const int track = CARD_TRACK;
        printf("  ..   the card's own track is %dpx\n", track);

        ui_scroll_thumb(0, 10, 4, track, &y, &h);
        printf("  ..   10 lines, 4 visible, %dpx track: thumb %dpx\n", track, h);
        check(y == 0, "at the top the thumb is at the top");
        check(h == track * 4 / 10, "and is as much of the track as the band is of the body");

        ui_scroll_thumb(6, 10, 4, track, &y, &h);
        check(y + h == track,
              "at the bottom it is flush with the bottom, which is what says the message ended");

        ui_scroll_thumb(3, 10, 4, track, &y, &h);
        check(y > 0 && y + h < track, "and in between it is in between");

        ui_scroll_thumb(0, 200, 4, track, &y, &h);
        check(h == MSG_SCROLL_MIN_H,
              "a body far longer than the band still leaves a thumb big enough to see");
        ui_scroll_thumb(196, 200, 4, track, &y, &h);
        check(y + h == track, "and it still reaches the bottom");

        ui_scroll_thumb(0, 4, 4, track, &y, &h);
        check(h == track && y == 0, "a body that fits fills the track");
    }

    printf("the thumb is never drawn outside its track\n");
    {
        int outside = 0;
        for (int total = -1; total <= 40; total++)
            for (int visible = 0; visible <= 6; visible++)
                for (int first = -2; first <= total + 2; first++)
                    for (int track = MSG_SCROLL_MIN_H; track <= 80; track += 7) {
                        int y = -1, h = -1;
                        ui_scroll_thumb(first, total, visible, track, &y, &h);
                        if (h < 1 || y < 0 || y + h > track) outside = 1;
                    }
        check(!outside, "over every body, band, position and track tried");
        /* Including a body of no lines, which is the one input that divides by
           it. The card does not draw a bar for a body that fits, so nothing
           calls this with one — but "nothing calls it that way today" is not a
           reason for the arithmetic to be undefined when something does. */
        {
            int y = -1, h = -1;
            ui_scroll_thumb(0, 0, 4, CARD_TRACK, &y, &h);
            check(y == 0 && h == CARD_TRACK,
                  "a body of no lines fills the track rather than dividing by it");
        }
    }

    printf("the cache poison is a byte no message can carry\n");
    {
        /* This is what makes dropping a body row's cache work without
           display_force — which on the card would re-erase and repaint the
           whole thing, a visible flash on every detent. draw_field compares the
           cache against the new text, so an EMPTIED cache matches a row with
           nothing to show and the repaint is skipped, leaving the previous
           line on the panel. A poisoned one cannot match anything, and the
           reason it cannot is notify.cpp's own filter, run here rather than
           cited. */
        char cache[8];
        memset(cache, 'x', sizeof(cache));
        ui_cache_drop(cache);
        check(strcmp(cache, "") != 0, "a dropped cache does not match a row with nothing on it");
        check(strlen(cache) == 1, "and is one byte, not a string somebody has to keep in step");

        char stored[8];
        char poison[2] = { UI_CACHE_POISON, '\0' };
        notify_copy_text(stored, sizeof(stored), poison);
        check(strcmp(stored, cache) != 0,
              "the byte does not survive being stored, so no line can equal it");
        check_str(stored, " ", "it is stored as a space, which is what the filter does with it");
    }

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
MAIN
} > "$work/test.cpp"

cxx="${CXX:-c++}"
# -Wno-unused-function and -Wno-unused-variable for the same reason
# tools/test_chips.sh gives: the sliced regions are whole regions, and they
# carry functions and constants whose only callers live in the parts of the
# firmware this test does not slice.
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -Wno-unused-variable -o "$work/test" "$work/test.cpp"
"$work/test"
