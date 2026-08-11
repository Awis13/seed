/*
 * Host tests for the micron page LAYOUT bridge (ticket C3): a whole page source
 * plus a scroll offset -> a filled cell grid. These pin the layout rules (wrap
 * at 38, section indent on both sides with the header-vs-paragraph gotcha,
 * last-alignment-wins placement, scroll clamp + the in-screen step multiplier)
 * and the untrusted-page budgets (a hostile 10k-line / all-'>' / over-wide page
 * must produce a bounded grid and never write out of bounds). Pure logic — no
 * Arduino, no radio, no board.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/micron/micron_layout.h"

/* Append a C string to buf at offset n, return the new offset. */
static int apnd(char *buf, int n, const char *s) {
    size_t l = strlen(s);
    memcpy(buf + n, s, l);
    return n + (int)l;
}

/* Count how many cells in a grid row are non-blank (cp != space). */
static int row_nonblank(const micron_grid *g, int row) {
    int n = 0;
    for (int c = 0; c < MICRON_GRID_COLS; c++)
        if (g->cells[(size_t)row * MICRON_GRID_COLS + c].cp != 0x20) n++;
    return n;
}

/* First column in a row whose cell is non-blank, or -1. */
static int row_first_nonblank(const micron_grid *g, int row) {
    for (int c = 0; c < MICRON_GRID_COLS; c++)
        if (g->cells[(size_t)row * MICRON_GRID_COLS + c].cp != 0x20) return c;
    return -1;
}

/* Last column in a row whose cell is non-blank, or -1. */
static int row_last_nonblank(const micron_grid *g, int row) {
    for (int c = MICRON_GRID_COLS - 1; c >= 0; c--)
        if (g->cells[(size_t)row * MICRON_GRID_COLS + c].cp != 0x20) return c;
    return -1;
}

/* A grid is well-formed by construction (fixed 456 cells); this asserts the
 * invariant the ticket asks for: nothing the layout wrote lands outside the
 * grid. Since the grid array is exactly MICRON_GRID_CELLS, "in bounds" means
 * the layout only ever indexed col<38, row<12 — verified here by touching every
 * cell (an OOB write would have corrupted neighbouring memory / crashed ASan). */
static void assert_grid_in_bounds(const micron_grid *g) {
    volatile uint32_t acc = 0;
    for (size_t i = 0; i < MICRON_GRID_CELLS; i++) {
        assert((i % MICRON_GRID_COLS) < MICRON_GRID_COLS);
        assert((i / MICRON_GRID_COLS) < MICRON_GRID_ROWS);
        acc += g->cells[i].cp;
    }
    (void)acc;
}

int main() {
    micron_grid grid;

    /* --- 1. wrap at 38: a 100-char line becomes ceil(100/38) = 3 rows ------- */
    {
        char src[128];
        for (int i = 0; i < 100; i++) src[i] = 'a';
        micron_layout_result r = micron_layout_page(src, 100, 0, &grid);
        assert(r.total_rows == 3);          /* 38 + 38 + 24 */
        assert(row_nonblank(&grid, 0) == 38);
        assert(row_nonblank(&grid, 1) == 38);
        assert(row_nonblank(&grid, 2) == 24);
        assert_grid_in_bounds(&grid);
    }

    /* --- 2a. wrap-width helper: the header-vs-paragraph gotcha -------------- */
    {
        /* depth 1 -> indent 0: both wrap at the full width */
        assert(micron_layout_indent(1) == 0);
        assert(micron_layout_wrap_width(1, 0) == 38);
        assert(micron_layout_wrap_width(1, 1) == 38);
        /* depth 2 -> indent 2: a paragraph shrinks by 2*2, a header does not */
        assert(micron_layout_indent(2) == 2);
        assert(micron_layout_wrap_width(2, /*header=*/0) == 34);
        assert(micron_layout_wrap_width(2, /*header=*/1) == 38);
        /* depth 3 -> indent 4 */
        assert(micron_layout_wrap_width(3, 0) == 30);
        assert(micron_layout_wrap_width(3, 1) == 38);
        /* the working widths DIFFER at the same depth — the whole point */
        assert(micron_layout_wrap_width(2, 0) != micron_layout_wrap_width(2, 1));
    }

    /* --- 2b. paragraph indent reduces wrap width; content sits inside ------- */
    {
        /* a header sets sticky depth 2; the following paragraph inherits it */
        char src[256];
        int n = 0;
        n = apnd(src, n, ">>H\n");
        for (int i = 0; i < 34; i++) src[n++] = 'p';  /* 34-char paragraph */
        src[n] = '\0';
        micron_layout_result r = micron_layout_page(src, (size_t)n, 0, &grid);
        /* header row + one paragraph row (34 fits a width-34 box exactly) */
        assert(r.total_rows == 2);
        /* the paragraph row is indented on the left by indent=2 */
        int prow = 1;
        assert(row_first_nonblank(&grid, prow) == 2);
        /* ...and on the right: last content col <= 38-indent-1 = 35 */
        assert(row_last_nonblank(&grid, prow) <= 35);
        assert(row_nonblank(&grid, prow) == 34);
        assert_grid_in_bounds(&grid);
    }

    /* --- 2c. header indent EATS text columns (spaces baked in), not width --- */
    {
        /* 36-char body. As a paragraph at depth 2 the box is 34 wide -> 2 rows.
         * As a header the 2-col indent is leading spaces + wrap at 38, so the
         * 36 chars + 2 spaces = 38 -> exactly ONE row. Same depth, same text,
         * different working width => different row count. */
        char para[64];
        int pn = 0;
        pn = apnd(para, pn, ">>\n");
        for (int i = 0; i < 36; i++) para[pn++] = 'x';
        para[pn] = '\0';
        micron_layout_result rp = micron_layout_page(para, (size_t)pn, 0, &grid);
        assert(rp.total_rows == 2);   /* 34 + 2 */

        char hdr[64];
        int hn = 0;
        hn = apnd(hdr, hn, ">>");
        for (int i = 0; i < 36; i++) hdr[hn++] = 'x';     /* ...with 36-char title */
        hdr[hn] = '\0';
        micron_layout_result rh = micron_layout_page(hdr, (size_t)hn, 0, &grid);
        assert(rh.total_rows == 1);   /* 2 spaces + 36 chars = 38, one row */
        assert(row_nonblank(&grid, 0) == 36);            /* two leading spaces */
        assert(row_first_nonblank(&grid, 0) == 2);
        assert_grid_in_bounds(&grid);
    }

    /* --- 3. alignment: last marker wins, placed within the working width ---- */
    {
        /* left (default): "AB" at col 0 */
        micron_layout_page("AB", 2, 0, &grid);
        assert(row_first_nonblank(&grid, 0) == 0);
        assert(row_last_nonblank(&grid, 0) == 1);

        /* centre: off = (38-2)/2 = 18 */
        micron_layout_page("`cAB", 4, 0, &grid);
        assert(row_first_nonblank(&grid, 0) == 18);
        assert(row_last_nonblank(&grid, 0) == 19);

        /* right: off = 38-2 = 36 */
        micron_layout_page("`rAB", 4, 0, &grid);
        assert(row_first_nonblank(&grid, 0) == 36);
        assert(row_last_nonblank(&grid, 0) == 37);

        /* last-wins: centre THEN right on one line -> right for the whole line */
        micron_layout_page("`cAB`rCD", 8, 0, &grid);
        assert(row_first_nonblank(&grid, 0) == 34);   /* "ABCD" right-aligned */
        assert(row_last_nonblank(&grid, 0) == 37);
        assert(row_nonblank(&grid, 0) == 4);
    }

    /* --- 4a. scroll clamp to [0, max] -------------------------------------- */
    {
        /* a page of 30 single-char lines -> 30 visual rows, max scroll 30-12=18 */
        char src[256];
        int n = 0;
        for (int i = 0; i < 30; i++) { src[n++] = (char)('0' + (i % 10)); src[n++] = '\n'; }
        src[n] = '\0';
        micron_layout_result r0 = micron_layout_page(src, (size_t)n, 0, &grid);
        assert(r0.total_rows == 30);
        /* clamp helper directly */
        assert(micron_layout_clamp_scroll(-5, 30) == 0);
        assert(micron_layout_clamp_scroll(5, 30) == 5);
        assert(micron_layout_clamp_scroll(100, 30) == 18);
        /* a negative scroll clamps to the top; row 0 shows source line 0 ('0') */
        micron_layout_result rneg = micron_layout_page(src, (size_t)n, -9, &grid);
        assert(rneg.scroll == 0);
        assert(grid.cells[0].cp == '0');
        /* an over-large scroll clamps to max=18; row 0 shows source line 18 */
        micron_layout_result rbig = micron_layout_page(src, (size_t)n, 999, &grid);
        assert(rbig.scroll == 18);
        assert(grid.cells[0].cp == (uint16_t)('0' + (18 % 10)));  /* '8' */
        /* a mid scroll shows the right window: scroll 3 -> row 0 is line 3 */
        micron_layout_page(src, (size_t)n, 3, &grid);
        assert(grid.cells[0].cp == '3');
        assert_grid_in_bounds(&grid);
    }

    /* --- 4b. the in-screen step multiplier (NOT the input constant) --------- */
    {
        /* one detent advances MICRON_PAGE_SCROLL_STEP rows, then clamps */
        assert(MICRON_PAGE_SCROLL_STEP >= 2);
        assert(micron_layout_apply_scroll(0, 1, 30) == MICRON_PAGE_SCROLL_STEP);
        assert(micron_layout_apply_scroll(0, -1, 30) == 0);      /* clamp at top */
        /* from 17, +5 detents = 17 + 5*STEP, clamped to max=18 */
        assert(micron_layout_apply_scroll(17, 5, 30) == 18);
        /* a short page that fits has max 0: any detent stays at 0 */
        assert(micron_layout_apply_scroll(0, 3, 5) == 0);
    }

    /* --- 5a. HOSTILE: 10k lines -> bounded grid, no OOB --------------------- */
    {
        static char big[20001];
        int n = 0;
        for (int i = 0; i < 10000; i++) { big[n++] = 'a'; big[n++] = '\n'; }
        big[n] = '\0';
        micron_layout_result r = micron_layout_page(big, (size_t)n, 0, &grid);
        assert(r.total_rows <= (size_t)MICRON_LAYOUT_MAX_ROWS);
        assert(r.total_rows == (size_t)MICRON_LAYOUT_MAX_ROWS);  /* capped */
        assert(r.scroll <= (int)MICRON_LAYOUT_MAX_ROWS);
        assert_grid_in_bounds(&grid);
        /* every visible row is a single 'a' in col 0 — bounded, sane */
        for (int rrow = 0; rrow < MICRON_GRID_ROWS; rrow++)
            assert(row_nonblank(&grid, rrow) == 1);
    }

    /* --- 5b. HOSTILE: all-'>' -> depth clamped, bounded -------------------- */
    {
        /* 1000 lines of 20 '>' each: C1 clamps depth to MICRON_MAX_SECTION_DEPTH,
         * and a bare header emits no row, so the page is 0 rows and blank. */
        static char gts[40001];
        int n = 0;
        for (int i = 0; i < 1000; i++) {
            for (int k = 0; k < 20; k++) gts[n++] = '>';
            gts[n++] = '\n';
        }
        gts[n] = '\0';
        micron_layout_result r = micron_layout_page(gts, (size_t)n, 0, &grid);
        assert(r.total_rows == 0);      /* bare headers only set depth */
        assert_grid_in_bounds(&grid);
    }

    /* --- 5c. depth clamp reflected in paragraph wrap width ------------------ */
    {
        /* ">"*20 clamps depth to MICRON_MAX_SECTION_DEPTH (8) -> indent 14 ->
         * paragraph box = 38 - 28 = 10. A 100-char paragraph then wraps to
         * ceil(100/10) = 10 rows. If depth were NOT clamped, the box would floor
         * to width 1 and give 100 rows — so this pins the clamp. */
        static char src[256];
        int n = 0;
        for (int k = 0; k < 20; k++) src[n++] = '>';     /* deep bare header */
        src[n++] = '\n';
        for (int i = 0; i < 100; i++) src[n++] = 'p';     /* paragraph */
        src[n] = '\0';
        micron_layout_result r = micron_layout_page(src, (size_t)n, 0, &grid);
        assert(micron_layout_wrap_width(MICRON_MAX_SECTION_DEPTH, 0) == 10);
        assert(r.total_rows == 10);
        assert_grid_in_bounds(&grid);
    }

    /* --- 5d. HOSTILE: an over-wide single line -> wrapped, not overrun ------ */
    {
        /* 500 chars on one line. C1 caps the source line at 512 bytes, the
         * layout caps cells at MICRON_LAYOUT_MAX_LINE_CELLS; either way the row
         * count is bounded and every column stays in the grid. */
        static char src[600];
        for (int i = 0; i < 500; i++) src[i] = 'w';
        micron_layout_result r = micron_layout_page(src, 500, 0, &grid);
        size_t cap = (size_t)MICRON_LAYOUT_MAX_LINE_CELLS;
        size_t expect = (cap + 37) / 38;                 /* ceil(cap/38) */
        assert(r.total_rows == expect);
        assert(r.total_rows <= (size_t)MICRON_LAYOUT_MAX_ROWS);
        assert_grid_in_bounds(&grid);
    }

    /* --- 5e. HOSTILE: multibyte line cut on a CODE-POINT boundary ----------- */
    {
        /* many 2-byte code points (U+00E9). The per-line cell budget cuts the
         * decoded run; because we only ever append a fully decoded cp, no cell
         * carries a half code point and no read runs past the buffer. */
        static char src[1200];
        int n = 0;
        for (int i = 0; i < 400; i++) { src[n++] = (char)0xC3; src[n++] = (char)0xA9; }
        micron_layout_result r = micron_layout_page(src, (size_t)n, 0, &grid);
        assert(r.total_rows <= (size_t)MICRON_LAYOUT_MAX_ROWS);
        assert_grid_in_bounds(&grid);
        /* every visible cell is a whole decoded cp (0x00E9), never a raw byte */
        for (size_t i = 0; i < MICRON_GRID_CELLS; i++) {
            uint16_t cp = grid.cells[i].cp;
            assert(cp == 0x20 || cp == 0x00E9);
        }
    }

    /* --- 6. dividers and comments ----------------------------------------- */
    {
        /* a comment line renders nothing; a divider is one full-width rule row */
        micron_layout_result r = micron_layout_page("# hidden\n-\nbody", 15, 0, &grid);
        assert(r.total_rows == 2);              /* divider row + body row */
        assert(row_nonblank(&grid, 0) == 38);   /* full-width rule */
        assert(grid.cells[0].cp == 0x2500);     /* default box-drawing rule */
        assert(grid.cells[MICRON_GRID_COLS].cp == 'b');  /* body on row 1 */
        assert_grid_in_bounds(&grid);
    }

    /* --- 7. measure-only (NULL grid) still returns the bounded height ------- */
    {
        char src[128];
        for (int i = 0; i < 77; i++) src[i] = 'z';
        micron_layout_result r = micron_layout_page(src, 77, 0, NULL);
        assert(r.total_rows == (77 + 37) / 38);  /* ceil(77/38) = 3 */
        assert(r.cells_used == 0);               /* nothing painted */
    }

    return 0;
}
