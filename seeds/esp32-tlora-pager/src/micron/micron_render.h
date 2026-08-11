#pragma once

/*
 * micron/micron_render.h — the cell model, palette and diff for the micron
 * page renderer (ticket C2). C1's micron.h turns a source line into styled
 * fragments; a later commit (C3) lays those fragments onto a fixed cell GRID;
 * this file owns that grid type, the colour palette that shrinks a full RGB
 * fragment colour to a one-byte cell index, and the PURE diff that decides
 * which cells changed between two frames.
 *
 * Why a cell grid at all: a full 456-cell repaint pushed over the shared SPI
 * bus is the freeze class the STAB work fought — it holds the bus long enough
 * to starve the SX1262 and drop an inbound LoRa packet. A retained previous
 * frame plus a diff turns a typical page update (a clock digit, a new status
 * line, a scrolled row) into a handful of dirty cells instead of 456.
 *
 * Everything here is Arduino-free, heap-free and float-free — pure integer
 * logic — so tools/test_micron_render.cpp exercises the palette and the diff
 * on the host, and hw_ui.cpp includes the same header to paint on target. The
 * DRAWING (glyph blit, bus lock) lives in hw_ui.cpp; this file never touches
 * hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include "micron.h"  /* micron_color, MICRON_ATTR_* */

/* Locked grid geometry (see the ticket): 38 columns x 12 rows of 5x7 glyphs
 * drawn at scale 2 (10x14 px each) -> 380x168 px, centred in the 480x222
 * panel. These are a hard decision, not a tuning knob. */
#define MICRON_GRID_COLS   38
#define MICRON_GRID_ROWS   12
#define MICRON_GRID_CELLS  (MICRON_GRID_COLS * MICRON_GRID_ROWS)  /* 456 */

/*
 * One painted cell: a UTF-8 code point plus one-byte fg/bg palette indices and
 * the C1 attribute bitmask. 6 bytes; a whole grid is 456*6 = 2736 B, and the
 * renderer keeps two (retained front + incoming back) in plain static RAM — no
 * PSRAM, a hard decision.
 */
typedef struct {
    uint16_t cp;    /* Unicode code point (0x20 = blank) */
    uint8_t  fg;    /* palette index (MICRON_PAL_DEFAULT = plane default) */
    uint8_t  bg;    /* palette index */
    uint8_t  attr;  /* MICRON_ATTR_* bitmask */
} micron_cell;

typedef struct {
    micron_cell cells[MICRON_GRID_CELLS];
} micron_grid;

/* --- palette ----------------------------------------------------------------
 * A cell stores an 8-bit index, not a full RGB triple, so the grid stays small
 * and the diff compares scalars. Micron colours are arbitrary RGB (any hex
 * nibble tripled, or a grey percentage), so the mapping is nearest-match into
 * a small fixed table: the common primaries, a few darks, an orange, and a
 * grey ramp. Index 0 is the DEFAULT sentinel — it means "use the renderer's
 * plane default" (foreground white / background black), resolved at draw time
 * in hw_ui.cpp, so a single index can stand for both planes' defaults while
 * the diff still treats it as one distinct value.
 */
#define MICRON_PAL_DEFAULT  0u

/* RGB888 table. Index 0 is the default sentinel; its RGB is unused by the
 * renderer (which substitutes the plane default) but kept black so the pure
 * rgb565 of the sentinel is deterministic for the host test. */
static const uint8_t MICRON_PALETTE_RGB[][3] = {
    {  0,   0,   0},  /*  0 default (sentinel) */
    {  0,   0,   0},  /*  1 black */
    {255, 255, 255},  /*  2 white */
    {255,   0,   0},  /*  3 red */
    {  0, 255,   0},  /*  4 green */
    {  0,   0, 255},  /*  5 blue */
    {255, 255,   0},  /*  6 yellow */
    {  0, 255, 255},  /*  7 cyan */
    {255,   0, 255},  /*  8 magenta */
    {255, 128,   0},  /*  9 orange */
    {128,   0,   0},  /* 10 dark red */
    {  0, 128,   0},  /* 11 dark green */
    {  0,   0, 128},  /* 12 dark blue */
    {128, 128,   0},  /* 13 olive */
    {  0, 128, 128},  /* 14 teal */
    {128,   0, 128},  /* 15 purple */
    { 32,  32,  32},  /* 16 grey ramp ... */
    { 64,  64,  64},  /* 17 */
    { 96,  96,  96},  /* 18 */
    {128, 128, 128},  /* 19 */
    {160, 160, 160},  /* 20 */
    {192, 192, 192},  /* 21 */
    {224, 224, 224},  /* 22 grey ramp end */
};
#define MICRON_PALETTE_COUNT \
    ((uint8_t)(sizeof(MICRON_PALETTE_RGB) / sizeof(MICRON_PALETTE_RGB[0])))

static inline uint16_t micron__rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* RGB565 for a palette index (clamped). The renderer resolves the DEFAULT
 * sentinel to its plane colour before drawing and never asks for rgb565(0) in
 * anger, but the mapping is defined and deterministic for the host test. */
static inline uint16_t micron_palette_rgb565(uint8_t idx) {
    if (idx >= MICRON_PALETTE_COUNT) idx = MICRON_PAL_DEFAULT;
    return micron__rgb565(MICRON_PALETTE_RGB[idx][0],
                          MICRON_PALETTE_RGB[idx][1],
                          MICRON_PALETTE_RGB[idx][2]);
}

/* Map a full micron colour to the nearest palette index. A default colour maps
 * to the DEFAULT sentinel unconditionally; any other colour picks the entry
 * with the smallest squared RGB distance, scanning real entries (1..N-1) in
 * order so ties resolve to the lowest index — pure and deterministic. */
static inline uint8_t micron_palette_index(micron_color c) {
    if (c.is_default) return MICRON_PAL_DEFAULT;
    uint32_t best_d = 0xFFFFFFFFu;
    uint8_t best = 1;  /* skip 0 (the default sentinel) */
    for (uint8_t i = 1; i < MICRON_PALETTE_COUNT; i++) {
        int dr = (int)c.r - (int)MICRON_PALETTE_RGB[i][0];
        int dg = (int)c.g - (int)MICRON_PALETTE_RGB[i][1];
        int db = (int)c.b - (int)MICRON_PALETTE_RGB[i][2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* --- grid + diff ------------------------------------------------------------ */

/* Reset a grid to blank cells (space, default fg/bg, no attrs). */
static inline void micron_grid_clear(micron_grid *g) {
    if (!g) return;
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) {
        g->cells[i].cp = 0x20;  /* space */
        g->cells[i].fg = MICRON_PAL_DEFAULT;
        g->cells[i].bg = MICRON_PAL_DEFAULT;
        g->cells[i].attr = 0;
    }
}

static inline int micron_cell_eq(const micron_cell *a, const micron_cell *b) {
    return a->cp == b->cp && a->fg == b->fg && a->bg == b->bg &&
           a->attr == b->attr;
}

/*
 * Pure frame diff. Writes 1/0 into dirty[MICRON_GRID_CELLS] for each cell that
 * differs between prev and next, and returns the count of dirty cells. When
 * `force` is non-zero every cell is marked dirty and MICRON_GRID_CELLS is
 * returned — this is how a screen re-entry (micron_render_invalidate) forces a
 * full repaint. No hardware, no allocation: the renderer takes the bus lock
 * and paints only the cells this function flagged.
 */
static inline int micron_grid_diff(const micron_grid *prev,
                                   const micron_grid *next,
                                   uint8_t *dirty, int force) {
    int count = 0;
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) {
        int changed = force ||
                      !micron_cell_eq(&prev->cells[i], &next->cells[i]);
        dirty[i] = (uint8_t)(changed ? 1 : 0);
        if (changed) count++;
    }
    return count;
}
