#pragma once

/*
 * micron/micron_layout.h — the layout bridge (ticket C3). C1 (micron.h) turns
 * one source line into styled fragments; C2 (micron_render.h) owns the cell
 * grid, the palette and the diff; this file turns a WHOLE micron page (multi-
 * line source text) plus a scroll offset into a filled cell grid. It is the
 * step between "parsed lines" and "hw_ui_show_page(grid)".
 *
 * Everything here is PURE integer logic — no Arduino, no heap, no floats — so
 * tools/test_micron_layout.cpp exercises source-text -> grid on the host, and
 * hw_ui.cpp calls micron_layout_page() to fill hw_ui_page_back() before handing
 * it to hw_ui_show_page(). The layout NEVER touches the bus; only the final
 * hw_ui_show_page() does (it holds the one C4 bus lock — there is no second
 * lock site).
 *
 * Layout rules (from the ticket recon), implemented exactly:
 *   1. WRAP at MICRON_GRID_COLS (38). Long source lines break; the SOURCE does
 *      not — the renderer inserts the breaks. Hard wrap at the column budget.
 *   2. SECTION INDENT on BOTH sides = (depth-1)*2 columns (depth is C1's sticky
 *      section state, already clamped to MICRON_MAX_SECTION_DEPTH). The gotcha:
 *      a HEADER's indent is SPACES INSERTED INTO THE TEXT (wrap stays at the
 *      full 38, the leading spaces only push the first row's content right); a
 *      PARAGRAPH's indent is WIDGET PADDING (the wrap width shrinks by 2*indent
 *      and the content sits inside [indent, 38-indent)). So at the same depth a
 *      header and a paragraph have a DIFFERENT working width — reproduced here.
 *   3. ALIGNMENT: the last alignment marker in a line wins for the WHOLE line
 *      (C1 already resolves this into micron_line::align); it is applied when
 *      placing each wrapped run left/centre/right inside the working width.
 *   4. HEADERS ('>' lines) render as section headers; their depth sets the
 *      sticky indent for the lines that follow (C1 carries the depth).
 *   5. SCROLL: a page taller than MICRON_GRID_ROWS (12) scrolls. The scroll
 *      offset is a top-visible VISUAL row. The wheel yields at most one detent
 *      per poll (INPUT_MAX_STEPS_PER_READ, a measured hardware constant we do
 *      NOT touch), so a long page would crawl one cell-row per detent; instead
 *      the page view applies an IN-SCREEN step multiplier — one detent advances
 *      MICRON_PAGE_SCROLL_STEP visual rows (micron_layout_apply_scroll()) — so
 *      the multiplier lives in the page view, never in the input constant.
 *
 * UNTRUSTED-PAGE BUDGETS (this is the untrusted layer — same rigor as the STAB
 * body-guard / notify control-byte filter: BOUND THE INPUT, don't "render what
 * came"). Every budget is enforced while consuming input, so a hostile page
 * (10k lines, all '>', a huge-width line) yields a bounded grid and never
 * overruns:
 *   - MICRON_LAYOUT_MAX_SRC_LINES  caps how many source lines are consumed.
 *   - MICRON_LAYOUT_MAX_ROWS       caps produced visual rows (bounds scroll).
 *   - MICRON_LAYOUT_MAX_LINE_CELLS caps cells decoded per source line; the cut
 *     falls on a whole code point (we only ever append a fully decoded cp).
 *   - section depth is clamped by C1 (MICRON_MAX_SECTION_DEPTH); we only read
 *     the clamped value, so indent/width stay bounded and the wrap width is
 *     floored at 1.
 *   - the 512-byte per-line budget is enforced by C1 at parse entry.
 *   - the grid is a fixed MICRON_GRID_CELLS array and every placed column is
 *     range-checked, so total cells are bounded and no write lands out of grid.
 */

#include <stddef.h>
#include <stdint.h>

#include "micron.h"         /* micron_state, micron_parse_line, decode helpers */
#include "micron_render.h"  /* micron_grid, micron_cell, palette, grid_clear   */

/* One detent of the wheel advances this many visual rows inside a page (the
 * in-screen step multiplier). NOT the input constant INPUT_MAX_STEPS_PER_READ
 * (that stays 1 — a measured hardware value); this multiplies AFTER the poll,
 * in the page view, so a long page scrolls at a usable pace. */
#define MICRON_PAGE_SCROLL_STEP     3

/* Untrusted-input budgets. */
#define MICRON_LAYOUT_MAX_SRC_LINES 512
#define MICRON_LAYOUT_MAX_ROWS      256
#define MICRON_LAYOUT_MAX_LINE_CELLS 256

/* Result of laying out a page: how tall it turned out (bounded), the scroll
 * actually applied (clamped to [0,max]), and how many non-blank cells the
 * visible window received (a cheap budget/telemetry number). */
typedef struct {
    size_t total_rows;
    int    scroll;
    size_t cells_used;
} micron_layout_result;

/* --- indent / working width (the header-vs-paragraph gotcha, made testable) - */

/* Section indent for a depth: (depth-1)*2, 0 at depth 0. */
static inline int micron_layout_indent(uint8_t depth) {
    return depth >= 1 ? ((int)depth - 1) * 2 : 0;
}

/* Working (wrap) width for a line at `depth`. A HEADER wraps at the full grid
 * width (its indent is spaces baked into the text, not a narrower box); a
 * PARAGRAPH's box shrinks by the indent on BOTH sides. Floored at 1 so a
 * clamped-deep section can never produce a zero/negative width. */
static inline int micron_layout_wrap_width(uint8_t depth, int is_header) {
    int indent = micron_layout_indent(depth);
    int w = is_header ? MICRON_GRID_COLS : (MICRON_GRID_COLS - 2 * indent);
    if (w < 1) w = 1;
    return w;
}

/* --- scroll clamp + in-screen multiplier ----------------------------------- */

/* Clamp a scroll offset to [0, max] where max = total-ROWS (0 if it fits). */
static inline int micron_layout_clamp_scroll(int scroll, size_t total_rows) {
    int maxs = (total_rows > (size_t)MICRON_GRID_ROWS)
                   ? (int)(total_rows - (size_t)MICRON_GRID_ROWS)
                   : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxs) scroll = maxs;
    return scroll;
}

/* Advance `scroll` by `detents` wheel steps (each = MICRON_PAGE_SCROLL_STEP
 * visual rows) and clamp. This is the in-screen multiplier: it lives here, in
 * the page view's pure layer, NOT in the input constant. */
static inline int micron_layout_apply_scroll(int scroll, int detents,
                                             size_t total_rows) {
    long s = (long)scroll + (long)detents * (long)MICRON_PAGE_SCROLL_STEP;
    if (s < 0) s = 0;
    if (s > 0x7fffffffL) s = 0x7fffffffL;
    return micron_layout_clamp_scroll((int)s, total_rows);
}

/* --- internal helpers ------------------------------------------------------ */

/* Byte length of the UTF-8 sequence a lead byte starts (1..4); 1 for a stray
 * continuation/invalid byte. The caller additionally clamps to the bytes that
 * remain, so a truncated tail advances one byte and never reads past the run. */
static inline size_t micron__utf8_adv(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Fill a row buffer with blank cells (space, default fg/bg, no attrs). */
static inline void micron__blank_row(micron_cell *row) {
    for (int c = 0; c < MICRON_GRID_COLS; c++) {
        row[c].cp = 0x20;
        row[c].fg = MICRON_PAL_DEFAULT;
        row[c].bg = MICRON_PAL_DEFAULT;
        row[c].attr = 0;
    }
}

/* Decode a parsed line's fragments into a flat cell run (cp + palette indices +
 * attrs), one cell per code point, stopping at `cap`. The cut always lands on a
 * whole code point — we only append a fully decoded cp. Returns the count. */
static inline size_t micron__line_cells(const micron_line *ln,
                                        micron_cell *out, size_t cap) {
    size_t n = 0;
    for (size_t fi = 0; fi < ln->count; fi++) {
        const micron_fragment *f = &ln->frags[fi];
        const uint8_t *t = (const uint8_t *)micron_fragment_text(ln, f);
        uint8_t fg = micron_palette_index(f->fg);
        uint8_t bg = micron_palette_index(f->bg);
        size_t i = 0;
        while (i < f->len) {
            if (n >= cap) return n;  /* per-line cell budget: cut at cp boundary */
            size_t remain = f->len - i;
            uint32_t cp = micron__decode_cp(t + i, remain);
            size_t adv = micron__utf8_adv(t[i]);
            if (adv > remain) adv = 1;
            /* the 5x7 font is BMP-only; a non-BMP cp maps to the replacement
             * glyph so the cell stays a single uint16 code point */
            out[n].cp = (uint16_t)(cp > 0xFFFFu ? 0xFFFDu : cp);
            out[n].fg = fg;
            out[n].bg = bg;
            out[n].attr = f->attr;
            n++;
            i += adv;
        }
    }
    return n;
}

/* The row sink: produced visual rows flow through here. When `grid` is NULL it
 * only counts (pass 1, to learn the total height so scroll can be clamped);
 * when `grid` is set it copies the rows inside the visible window
 * [scroll, scroll+ROWS) into the grid (pass 2). Either way `total` counts the
 * page height, capped at MICRON_LAYOUT_MAX_ROWS. */
typedef struct {
    micron_grid *grid;   /* NULL = count-only pass */
    int    scroll;
    size_t total;
    size_t cells_used;
    int    overflow;
} micron__sink;

/* Push one visual row. Returns 1 when the row budget is exhausted (the caller
 * then stops consuming input), 0 otherwise. */
static inline int micron__push_row(micron__sink *s, const micron_cell *row) {
    if (s->total >= (size_t)MICRON_LAYOUT_MAX_ROWS) {
        s->overflow = 1;
        return 1;
    }
    if (s->grid && (int)s->total >= s->scroll &&
        (int)s->total < s->scroll + MICRON_GRID_ROWS) {
        int gr = (int)s->total - s->scroll;
        for (int c = 0; c < MICRON_GRID_COLS; c++) {
            s->grid->cells[(size_t)gr * MICRON_GRID_COLS + (size_t)c] = row[c];
            if (row[c].cp != 0x20) s->cells_used++;
        }
    }
    s->total++;
    return 0;
}

/* Lay one already-decoded line (its cell run) into visual rows: wrap at the
 * working width, align each wrapped run, and push each row. Returns 1 if the
 * row budget stopped us. */
static inline int micron__layout_line(micron__sink *s, const micron_line *ln,
                                      const micron_cell *cells, size_t ncells,
                                      int is_header, uint8_t depth,
                                      uint8_t align) {
    int indent = micron_layout_indent(depth);

    /* A divider is one rule row filled across the working region. */
    if (ln->is_divider) {
        micron_cell row[MICRON_GRID_COLS];
        micron__blank_row(row);
        uint16_t fill = (uint16_t)(ln->divider_fill > 0xFFFFu ? 0x2500u
                                                              : ln->divider_fill);
        for (int c = indent; c < MICRON_GRID_COLS - indent; c++)
            if (c >= 0 && c < MICRON_GRID_COLS) row[c].cp = fill;
        return micron__push_row(s, row);
    }

    /* No content: a bare header (just '>') only sets sticky depth — no row; a
     * genuinely empty paragraph line is one blank row (paragraph spacing). */
    if (ncells == 0) {
        if (is_header) return 0;
        micron_cell row[MICRON_GRID_COLS];
        micron__blank_row(row);
        return micron__push_row(s, row);
    }

    int W = micron_layout_wrap_width(depth, is_header);
    int base_col = is_header ? 0 : indent;

    size_t start = 0;
    while (start < ncells) {
        size_t runlen = ncells - start;
        if (runlen > (size_t)W) runlen = (size_t)W;

        int off = 0;
        if (align == MICRON_ALIGN_CENTER) off = (W - (int)runlen) / 2;
        else if (align == MICRON_ALIGN_RIGHT) off = W - (int)runlen;
        if (off < 0) off = 0;
        int col0 = base_col + off;

        micron_cell row[MICRON_GRID_COLS];
        micron__blank_row(row);
        for (size_t k = 0; k < runlen; k++) {
            int gc = col0 + (int)k;
            if (gc >= 0 && gc < MICRON_GRID_COLS) row[gc] = cells[start + k];
        }
        if (micron__push_row(s, row)) return 1;
        start += runlen;
    }
    return 0;
}

/* Stream a page source through the sink. Splits on '\n' (a trailing '\r' is
 * dropped), parses each line via C1 (which enforces the depth clamp and the
 * 512-byte line budget), decodes its cells under the per-line budget, and lays
 * it out. Bounded by MICRON_LAYOUT_MAX_SRC_LINES and, via the sink, by
 * MICRON_LAYOUT_MAX_ROWS. */
static inline void micron__layout_scan(const char *src, size_t len, int scroll,
                                       micron_grid *grid, size_t *out_total,
                                       size_t *out_cells) {
    micron__sink s;
    s.grid = grid;
    s.scroll = scroll;
    s.total = 0;
    s.cells_used = 0;
    s.overflow = 0;

    micron_state st;
    micron_state_init(&st);

    size_t start = 0;
    size_t lines = 0;
    while (start <= len && lines < (size_t)MICRON_LAYOUT_MAX_SRC_LINES) {
        /* A terminating newline at the very end must not spawn an extra blank
         * line, but a genuine mid-page blank line must survive. */
        if (start == len && start != 0) break;

        size_t end = start;
        while (end < len && src[end] != '\n') end++;
        size_t rawlen = end - start;
        if (rawlen > 0 && src != NULL && src[start + rawlen - 1] == '\r') rawlen--;

        micron_line ln;
        micron_parse_line(&st, src ? src + start : NULL, rawlen, &ln);
        lines++;

        /* The literal-mode toggle line ("`=") renders nothing. */
        int toggle = (rawlen == 2 && src != NULL &&
                      src[start] == '`' && src[start + 1] == '=');

        if (!ln.is_comment && !toggle) {
            micron_cell buf[MICRON_LAYOUT_MAX_LINE_CELLS];
            size_t ncells;
            int is_header = ln.is_header ? 1 : 0;
            /* A header's indent = leading spaces baked into the text; wrap then
             * runs at the full width. A bare header (no title) stays 0 cells so
             * it emits no row (it only set the sticky depth). */
            size_t pad = is_header ? (size_t)micron_layout_indent(ln.section_depth)
                                   : 0;
            if (pad > MICRON_LAYOUT_MAX_LINE_CELLS) pad = MICRON_LAYOUT_MAX_LINE_CELLS;
            size_t content = micron__line_cells(&ln, buf + pad,
                                                (size_t)MICRON_LAYOUT_MAX_LINE_CELLS - pad);
            if (is_header && content == 0) {
                ncells = 0;
            } else {
                for (size_t k = 0; k < pad; k++) {
                    buf[k].cp = 0x20;
                    buf[k].fg = MICRON_PAL_DEFAULT;
                    buf[k].bg = MICRON_PAL_DEFAULT;
                    buf[k].attr = 0;
                }
                ncells = pad + content;
            }
            if (micron__layout_line(&s, &ln, buf, ncells, is_header,
                                    ln.section_depth, ln.align))
                break;  /* row budget hit */
        }

        if (end == len) break;  /* consumed the final line */
        start = end + 1;
    }

    if (out_total) *out_total = s.total;
    if (out_cells) *out_cells = s.cells_used;
}

/* --- public entry ---------------------------------------------------------- */

/*
 * Lay a whole micron page (`src`/`len`) into `grid` at scroll offset `scroll`.
 * Pure: no hardware, no allocation. Pass 1 counts the page height so `scroll`
 * can be clamped; pass 2 clears the grid and fills the visible window. Returns
 * the total (bounded) height, the clamped scroll actually applied, and the
 * non-blank cell count of the window. `grid` may be NULL to only measure.
 */
static inline micron_layout_result micron_layout_page(const char *src,
                                                      size_t len, int scroll,
                                                      micron_grid *grid) {
    micron_layout_result r;
    size_t total = 0;
    micron__layout_scan(src, len, 0, NULL, &total, NULL);  /* pass 1: measure */

    int cs = micron_layout_clamp_scroll(scroll, total);
    size_t cells = 0;
    if (grid) {
        micron_grid_clear(grid);
        size_t t2 = 0;
        micron__layout_scan(src, len, cs, grid, &t2, &cells);  /* pass 2: fill */
    }

    r.total_rows = total;
    r.scroll = cs;
    r.cells_used = cells;
    return r;
}
