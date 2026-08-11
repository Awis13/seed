#pragma once

/*
 * box_glyphs.h — 5x7 box-drawing and block glyphs for the panel font.
 *
 * Foreign NomadNet / micron pages draw tables, frames and separators with the
 * Unicode box-drawing range (U+2500..) and the block elements (U+2588 family).
 * The base FONT5X7 in hw_ui.cpp only covers ASCII, so every such glyph used to
 * fall through font_glyph() to '?', turning a framed page into a wall of
 * question marks. This header adds those glyphs and a PURE lookup so the render
 * path resolves them, and so a host test can pin every code point without
 * dragging in Arduino.
 *
 * Bitmap convention — identical to FONT5X7 and the Cyrillic tables: each glyph
 * is 5 bytes = 5 COLUMNS, and each byte's bits are the 7 ROWS with bit0 (LSB)
 * the TOP row and bit6 the BOTTOM row. Row bit values: row0=0x01 .. row6=0x40.
 * The 5x7 cell is columns 0..4, rows 0..6; centre column = 2, centre row = 3
 * (bit mask 0x08). Getting the bit order wrong renders glyphs upside-down.
 *
 * Connectivity — the micron grid has NO inter-glyph gap (10px pitch = 5 cols x
 * scale 2), so a stroke that spans the FULL width or height of the cell touches
 * its neighbour and the lines join:
 *   - horizontal '-' strokes set row 3 across ALL 5 columns (full width);
 *   - vertical   '|' strokes set column 2 across ALL 7 rows (full height);
 *   - corners / tees are half-strokes meeting at the centre (col 2, row 3).
 *
 * Block shades are a monotonic dither: density(U+2591) < U+2592 < U+2593 <
 * U+2588 (full), so they read as increasing grey at scale 2.
 *
 * hw_ui.cpp includes this after Arduino.h (PROGMEM defined); the host test
 * includes it standalone, where the guard below makes PROGMEM a no-op and the
 * bytes are read directly.
 */

#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

/*
 * Bitmaps in a flat PROGMEM byte array, 5 bytes per glyph, indexed by the
 * enum below. font_glyph() returns a pointer into this array and the panel
 * blit reads it with pgm_read_byte, exactly like FONT5X7.
 */
static const uint8_t BOX_BITMAPS[] PROGMEM = {
    /*  0 U+2500 --  horizontal: row 3, all cols (full width -> joins) */
    0x08, 0x08, 0x08, 0x08, 0x08,
    /*  1 U+2502 |   vertical: col 2, all rows (full height -> joins) */
    0x00, 0x00, 0x7F, 0x00, 0x00,
    /*  2 U+250C ,-  down+right: col2 rows3..6 + row3 cols2..4 */
    0x00, 0x00, 0x78, 0x08, 0x08,
    /*  3 U+2510 -,  down+left: col2 rows3..6 + row3 cols0..2 */
    0x08, 0x08, 0x78, 0x00, 0x00,
    /*  4 U+2514 '-  up+right: col2 rows0..3 + row3 cols2..4 */
    0x00, 0x00, 0x0F, 0x08, 0x08,
    /*  5 U+2518 -'  up+left: col2 rows0..3 + row3 cols0..2 */
    0x08, 0x08, 0x0F, 0x00, 0x00,
    /*  6 U+251C |-  vertical full + right */
    0x00, 0x00, 0x7F, 0x08, 0x08,
    /*  7 U+2524 -|  vertical full + left */
    0x08, 0x08, 0x7F, 0x00, 0x00,
    /*  8 U+252C -,- horizontal full + down */
    0x08, 0x08, 0x78, 0x08, 0x08,
    /*  9 U+2534 -'- horizontal full + up */
    0x08, 0x08, 0x0F, 0x08, 0x08,
    /* 10 U+253C -+- cross: horizontal full + vertical full */
    0x08, 0x08, 0x7F, 0x08, 0x08,
    /* 11 U+2588 full block: every pixel */
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
    /* 12 U+2593 dark shade: inverse of the light dither (density 26/35) */
    0x6E, 0x77, 0x3B, 0x5D, 0x6E,
    /* 13 U+2592 medium shade: checkerboard (density 18/35) */
    0x55, 0x2A, 0x55, 0x2A, 0x55,
    /* 14 U+2591 light shade: sparse diagonal dither (density 9/35) */
    0x11, 0x08, 0x44, 0x22, 0x11,
    /* 15 U+2550 = double horizontal: rows 2 and 4, all cols */
    0x14, 0x14, 0x14, 0x14, 0x14,
    /* 16 U+2551 || double vertical: cols 1 and 3, all rows */
    0x00, 0x7F, 0x00, 0x7F, 0x00,
    /* 17 U+2554 double down+right */
    0x00, 0x7C, 0x04, 0x74, 0x14,
    /* 18 U+2557 double down+left */
    0x14, 0x74, 0x04, 0x7C, 0x00,
    /* 19 U+255A double up+right */
    0x00, 0x1F, 0x10, 0x17, 0x14,
    /* 20 U+255D double up+left */
    0x14, 0x17, 0x10, 0x1F, 0x00,
    /* 21 U+2580 upper half block: rows 0..2, all cols */
    0x07, 0x07, 0x07, 0x07, 0x07,
    /* 22 U+2584 lower half block: rows 4..6, all cols */
    0x70, 0x70, 0x70, 0x70, 0x70,
};

/*
 * Map a code point to its glyph index in BOX_BITMAPS, or -1 if this header does
 * not cover it. Rounded corners (U+256D..2570) reuse the sharp light corners —
 * the 5x7 cell is too coarse for a visible radius, and reusing keeps the table
 * small. Pure integer switch: no state, host-safe.
 */
static inline int box_glyph_index(uint32_t cp) {
    switch (cp) {
        /* light box-drawing */
        case 0x2500: return 0;
        case 0x2502: return 1;
        case 0x250C: return 2;
        case 0x2510: return 3;
        case 0x2514: return 4;
        case 0x2518: return 5;
        case 0x251C: return 6;
        case 0x2524: return 7;
        case 0x252C: return 8;
        case 0x2534: return 9;
        case 0x253C: return 10;
        /* block elements */
        case 0x2588: return 11;
        case 0x2593: return 12;
        case 0x2592: return 13;
        case 0x2591: return 14;
        /* double-line box-drawing */
        case 0x2550: return 15;
        case 0x2551: return 16;
        case 0x2554: return 17;
        case 0x2557: return 18;
        case 0x255A: return 19;
        case 0x255D: return 20;
        /* half blocks (progress bar / decrypt effect) */
        case 0x2580: return 21;
        case 0x2584: return 22;
        /* rounded corners -> reuse the matching sharp light corner */
        case 0x256D: return 2;   /* .- like U+250C */
        case 0x256E: return 3;   /* -. like U+2510 */
        case 0x256F: return 5;   /* -' like U+2518 */
        case 0x2570: return 4;   /* '- like U+2514 */
        default:     return -1;
    }
}

/* Resolve a code point to a 5-byte PROGMEM bitmap, or nullptr if not a box/
 * block glyph. */
static inline const uint8_t *box_glyph(uint32_t cp) {
    const int idx = box_glyph_index(cp);
    return idx < 0 ? (const uint8_t *)0 : &BOX_BITMAPS[idx * 5];
}
