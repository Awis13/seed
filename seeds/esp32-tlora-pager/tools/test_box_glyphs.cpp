/*
 * Host tests for the box-drawing + block glyph table (ticket C5). The panel
 * font in hw_ui.cpp used to fall through to '?' for every code point outside
 * ASCII + Cyrillic, so foreign NomadNet tables/frames rendered as a wall of
 * question marks. box_glyphs.h adds the light box-drawing set, block shades,
 * double lines and half blocks with a PURE lookup — this pins that:
 *   - every added code point resolves to a real glyph (not the '?' fallback);
 *   - the connectivity geometry holds: '-' fills the whole centre row across
 *     all 5 columns, '|' fills the whole centre column across all 7 rows, and
 *     corners meet at the centre — the no-gap 10px cell then joins the lines;
 *   - the block shades are strictly monotonic in density.
 * No Arduino, no board — the table and lookup are Arduino-free by design.
 */

#include <assert.h>
#include <stdint.h>

#include "../src/box_glyphs.h"

/* Bit r (0..6) of column c set? bit0 = top row, bit6 = bottom row. */
static int px(const uint8_t *g, int col, int row) {
    return (g[col] >> row) & 1;
}

/* Count set pixels in a 5x7 glyph (density, out of 35). */
static int density(const uint8_t *g) {
    int n = 0;
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            n += px(g, c, r);
    return n;
}

int main() {
    /* The full set this commit adds. Every one must resolve to a NON-'?'
     * glyph, i.e. box_glyph() returns non-null for it. */
    const uint32_t added[] = {
        /* light box-drawing */
        0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
        0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
        /* block elements */
        0x2588, 0x2593, 0x2592, 0x2591,
        /* double-line box-drawing */
        0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D,
        /* half blocks */
        0x2580, 0x2584,
        /* rounded corners (reuse sharp light corners) */
        0x256D, 0x256E, 0x256F, 0x2570,
    };
    const int N = (int)(sizeof(added) / sizeof(added[0]));
    for (int i = 0; i < N; i++) {
        const uint8_t *g = box_glyph(added[i]);
        assert(g != 0 && "added code point must resolve to a box glyph");
    }

    /* Code points OUTSIDE the added set must NOT be claimed here (they stay on
     * the ASCII/Cyrillic/'?' path in font_glyph): ASCII 'A', a gap in the box
     * range, and a code point just past the table. */
    assert(box_glyph('A') == 0);
    assert(box_glyph(0x2501) == 0);   /* heavy horizontal — not added */
    assert(box_glyph(0x2600) == 0);

    /* --- connectivity: horizontal fills the centre row, all 5 columns ------- */
    const uint8_t *h = box_glyph(0x2500);
    for (int c = 0; c < 5; c++) {
        assert(px(h, c, 3) == 1 && "centre row must be set in every column");
        for (int r = 0; r < 7; r++)
            if (r != 3) assert(px(h, c, r) == 0 && "only the centre row is set");
    }

    /* --- connectivity: vertical fills the centre column, all 7 rows -------- */
    const uint8_t *v = box_glyph(0x2502);
    for (int r = 0; r < 7; r++) {
        assert(px(v, 2, r) == 1 && "centre column must be set in every row");
        for (int c = 0; c < 5; c++)
            if (c != 2) assert(px(v, c, r) == 0 && "only the centre column is set");
    }

    /* --- corners/tees meet at the centre (col 2, row 3) -------------------- */
    const uint32_t joints[] = {
        0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
    };
    for (int i = 0; i < (int)(sizeof(joints) / sizeof(joints[0])); i++) {
        const uint8_t *g = box_glyph(joints[i]);
        assert(px(g, 2, 3) == 1 && "joint must be lit at the centre cell");
    }

    /* The cross is exactly the union of horizontal and vertical. */
    const uint8_t *plus = box_glyph(0x253C);
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            assert(px(plus, c, r) == (px(h, c, r) | px(v, c, r)));

    /* A tee is the vertical plus a half of the centre row: '|-' spans the
     * centre row from the centre to the right edge, and the whole column. */
    const uint8_t *tee_r = box_glyph(0x251C);   /* |- */
    assert(px(tee_r, 2, 3) && px(tee_r, 3, 3) && px(tee_r, 4, 3));
    assert(px(tee_r, 0, 3) == 0 && px(tee_r, 1, 3) == 0);
    for (int r = 0; r < 7; r++) assert(px(tee_r, 2, r) == 1);

    /* --- block shade monotonicity: ' ' < light < medium < dark < full ------ */
    const int d_light  = density(box_glyph(0x2591));
    const int d_medium = density(box_glyph(0x2592));
    const int d_dark   = density(box_glyph(0x2593));
    const int d_full   = density(box_glyph(0x2588));
    assert(0 < d_light);
    assert(d_light < d_medium);
    assert(d_medium < d_dark);
    assert(d_dark < d_full);
    assert(d_full == 35 && "full block lights every pixel");

    /* Rounded corners reuse their sharp light counterparts. */
    assert(box_glyph(0x256D) == box_glyph(0x250C));
    assert(box_glyph(0x256E) == box_glyph(0x2510));
    assert(box_glyph(0x256F) == box_glyph(0x2518));
    assert(box_glyph(0x2570) == box_glyph(0x2514));

    return 0;
}
