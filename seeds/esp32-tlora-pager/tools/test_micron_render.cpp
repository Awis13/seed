/*
 * Host tests for the micron RENDER model (ticket C2): the colour palette and
 * the frame diff. These are the PURE parts that decide what gets painted — the
 * glyph blit itself is hardware and lives in hw_ui.cpp, not here. Pinning the
 * palette (color -> index -> rgb565, nearest-match, the default sentinel) and
 * the diff (identical/one-changed/all-changed/attr-only/forced) keeps a future
 * refactor from silently repainting the whole 456-cell grid or losing a change.
 * No Arduino, no radio, no board.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/micron/micron_render.h"

static micron_color rgb(uint8_t r, uint8_t g, uint8_t b) {
    micron_color c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.is_default = 0;
    return c;
}

int main() {
    /* --- palette: the default colour maps to the sentinel, consistently --- */
    assert(micron_palette_index(micron_color_default()) == MICRON_PAL_DEFAULT);
    /* every call is the same value — no state, pure */
    for (int i = 0; i < 100; i++)
        assert(micron_palette_index(micron_color_default()) == MICRON_PAL_DEFAULT);

    /* --- exact palette colours round-trip to themselves -------------------- */
    /* An exact table entry must pick its own index (distance 0), and rgb565 of
     * that index must equal rgb565 of the source colour. */
    for (uint8_t i = 1; i < MICRON_PALETTE_COUNT; i++) {
        micron_color c = rgb(MICRON_PALETTE_RGB[i][0],
                             MICRON_PALETTE_RGB[i][1],
                             MICRON_PALETTE_RGB[i][2]);
        uint8_t idx = micron_palette_index(c);
        /* distance 0 to entry i; ties resolve to the lowest index, so idx is
         * the first entry equal to this RGB (some greys/darks are unique). */
        assert(MICRON_PALETTE_RGB[idx][0] == c.r &&
               MICRON_PALETTE_RGB[idx][1] == c.g &&
               MICRON_PALETTE_RGB[idx][2] == c.b);
        uint16_t via_idx = micron_palette_rgb565(idx);
        uint16_t via_src = micron__rgb565(c.r, c.g, c.b);
        assert(via_idx == via_src);
    }

    /* --- nearest match: an off-table colour lands on the closest entry ------ */
    /* Bright red-ish with noise snaps to pure red (index 3), not to a dark. */
    assert(micron_palette_index(rgb(250, 8, 6)) == 3);   /* ~red */
    assert(micron_palette_index(rgb(8, 250, 4)) == 4);   /* ~green */
    assert(micron_palette_index(rgb(4, 6, 248)) == 5);   /* ~blue */
    /* A near-black snaps to black (index 1), NOT the default sentinel (0): an
     * explicit dark colour is not the same as "use the plane default". */
    assert(micron_palette_index(rgb(2, 1, 3)) == 1);
    /* A mid grey snaps into the grey ramp, not to a saturated primary. */
    {
        uint8_t idx = micron_palette_index(rgb(130, 130, 130));
        assert(idx >= 16 && idx < MICRON_PALETTE_COUNT);  /* grey ramp band */
    }

    /* --- nearest match is deterministic ----------------------------------- */
    for (int r = 0; r < 256; r += 37)
        for (int g = 0; g < 256; g += 41)
            for (int b = 0; b < 256; b += 43) {
                micron_color c = rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
                uint8_t a = micron_palette_index(c);
                uint8_t bb = micron_palette_index(c);
                assert(a == bb);
                assert(a < MICRON_PALETTE_COUNT);
            }

    /* --- rgb565 clamps an out-of-range index instead of reading past end --- */
    assert(micron_palette_rgb565(200) == micron_palette_rgb565(MICRON_PAL_DEFAULT));

    /* --- diff: identical grids -> zero dirty ------------------------------- */
    static micron_grid a, b;
    static uint8_t dirty[MICRON_GRID_CELLS];
    micron_grid_clear(&a);
    micron_grid_clear(&b);
    assert(micron_grid_diff(&a, &b, dirty, 0) == 0);
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) assert(dirty[i] == 0);

    /* --- diff: one changed cell -> exactly one dirty, at that index -------- */
    b.cells[100].cp = 'X';
    assert(micron_grid_diff(&a, &b, dirty, 0) == 1);
    assert(dirty[100] == 1);
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++)
        if (i != 100) assert(dirty[i] == 0);

    /* --- diff detects an attribute-only change (same cp/fg/bg) ------------- */
    micron_grid_clear(&b);
    b.cells[7].attr = MICRON_ATTR_BOLD;
    assert(micron_grid_diff(&a, &b, dirty, 0) == 1);
    assert(dirty[7] == 1);
    /* ...and an fg-only change */
    micron_grid_clear(&b);
    b.cells[8].fg = 3;  /* red vs default */
    assert(micron_grid_diff(&a, &b, dirty, 0) == 1);
    assert(dirty[8] == 1);
    /* ...and a bg-only change */
    micron_grid_clear(&b);
    b.cells[9].bg = 5;  /* blue vs default */
    assert(micron_grid_diff(&a, &b, dirty, 0) == 1);
    assert(dirty[9] == 1);

    /* --- diff: every cell changed -> all 456 dirty ------------------------ */
    micron_grid_clear(&b);
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) b.cells[i].cp = '#';
    assert(micron_grid_diff(&a, &b, dirty, 0) == MICRON_GRID_CELLS);
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) assert(dirty[i] == 1);

    /* --- forced diff (screen re-entry / invalidate) -> all dirty even when
     *     the two grids are identical -------------------------------------- */
    micron_grid_clear(&b);
    assert(micron_grid_diff(&a, &b, dirty, 1) == MICRON_GRID_CELLS);
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) assert(dirty[i] == 1);

    /* --- a typical page update touches a handful of cells, not 456 -------- */
    /* Model a status line changing on one row (38 cells) — the diff must scope
     * the repaint to that row, proving the freeze-class full blit is avoided. */
    micron_grid_clear(&b);
    for (int col = 0; col < MICRON_GRID_COLS; col++)
        b.cells[3 * MICRON_GRID_COLS + col].cp = 'a' + (col % 26);
    int n = micron_grid_diff(&a, &b, dirty, 0);
    assert(n == MICRON_GRID_COLS);          /* one row, 38 cells */
    assert(n < MICRON_GRID_CELLS);          /* far less than a full frame */

    /* geometry sanity: the locked grid dims */
    assert(MICRON_GRID_COLS == 38);
    assert(MICRON_GRID_ROWS == 12);
    assert(MICRON_GRID_CELLS == 456);

    return 0;
}
