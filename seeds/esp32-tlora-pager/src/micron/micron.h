#pragma once

/*
 * micron/micron.h — a parser for NomadNet's "micron" page markup.
 *
 * micron is the inline markup NomadNet pages carry over Reticulum: everything
 * hangs off a backtick, plus a handful of line-level prefixes (section headers,
 * dividers, comments). This turns ONE source line, together with the small
 * amount of state that is sticky between lines, into an ordered list of styled
 * text fragments a renderer can lay out. It is pure logic — no Arduino, no
 * heap, no floats — so the same code compiles on the host for
 * tools/test_micron.cpp and links into the firmware later.
 *
 * The grammar was understood from NomadNet's MicronParser.py (author markqvist,
 * restrictive non-OSI license) as a FORMAT SPEC only; this is a clean-room
 * implementation — no code was copied, transliterated, or structurally mirrored
 * from that parser. The behaviour differs deliberately in three places, each
 * flagged "DIVERGENCE" below, because this layer parses untrusted network data
 * and must degrade visibly and safely rather than silently or fatally.
 *
 * Markers (after a backtick):
 *   `_  toggle underline      `!  toggle bold        `*  toggle italic
 *   `f  reset foreground      `b  reset background   ``  reset colours+attrs
 *   `F + three colour chars   `B + three colour chars
 *   `c/`l/`r/`a  align centre/left/right/default (last one in a line wins)
 *   `=  literal-mode toggle, but ONLY when the whole line is exactly "`="
 * A colour is three hex nibbles, each doubled per channel (f00 -> ff0000), or a
 * grey written "g" + two DECIMAL digits as a percentage (g50 -> mid grey). Any
 * colour that fails to parse falls back to the default (a reset of that plane).
 *
 * Line-level, at column 0 and only outside literal mode:
 *   >   run of '>' — the count is the section depth, sticky until a later line
 *       changes it. '-'  a horizontal divider (optional following fill char).
 *   #   a comment line — not rendered.
 *
 * Escaping: a backslash removes itself and prints the next byte verbatim, so
 * "\`" prints a backtick, "\\" prints a backslash, and "\x" prints "x". A
 * trailing backslash with nothing after it simply disappears.
 *
 * DIVERGENCES from MicronParser.py (see the numbered notes at the call sites):
 *   1. Unknown marker after a backtick is printed LITERALLY (backtick + char)
 *      instead of being silently swallowed — visible garbage beats silent loss
 *      for untrusted input.
 *   2. Section depth is CLAMPED to MICRON_MAX_SECTION_DEPTH. The reference
 *      parser raises on depth >= 4 and replaces the whole page with an error;
 *      we clamp and keep rendering.
 *   3. A line longer than MICRON_MAX_LINE_BYTES is truncated at a UTF-8
 *      code-point boundary at parse entry, never accumulated. The per-line
 *      working buffer lives on a 16 KB loop-task stack with a stack-canary
 *      history, so kilobyte lines are unsafe; 512 bytes is the budget.
 */

#include <stddef.h>
#include <stdint.h>

#define MICRON_MAX_LINE_BYTES     512
#define MICRON_MAX_SECTION_DEPTH  8
/* Defensive cap on styled runs per line. A realistic line has a handful; only
 * adversarial "toggle every byte" input approaches it, and past the cap the
 * excess text is dropped rather than growing the buffer or crashing. */
#define MICRON_MAX_FRAGMENTS      96

/* Attribute bitmask (a fragment can carry any combination). */
#define MICRON_ATTR_BOLD       0x01u
#define MICRON_ATTR_ITALIC     0x02u
#define MICRON_ATTR_UNDERLINE  0x04u

/* Line alignment. Sticky across lines until a marker or reset changes it. */
typedef enum {
    MICRON_ALIGN_DEFAULT = 0,
    MICRON_ALIGN_LEFT,
    MICRON_ALIGN_CENTER,
    MICRON_ALIGN_RIGHT
} micron_align;

/* A portable colour. C1 stays free of RGB565/hardware; C2's palette indexes
 * these RGB triples. is_default means "use the renderer's default plane". */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t is_default;
} micron_color;

static inline micron_color micron_color_default(void) {
    micron_color c;
    c.r = 0;
    c.g = 0;
    c.b = 0;
    c.is_default = 1;
    return c;
}

/* One styled run of text. text_off/len index into micron_line::text (an offset,
 * not a pointer, so the whole struct can be copied or moved freely). */
typedef struct {
    size_t text_off;
    size_t len;
    micron_color fg;
    micron_color bg;
    uint8_t attr;           /* MICRON_ATTR_* bitmask */
    uint8_t section_depth;  /* clamped depth this run renders at */
} micron_fragment;

/* Parser state carried between lines. Everything else (colours, attributes)
 * starts fresh on each line; only these three are sticky, matching the format:
 * a section header holds until re-stated, literal mode toggles, and the last
 * alignment marker holds until changed. */
typedef struct {
    uint8_t section_depth;
    uint8_t literal;
    uint8_t align;  /* micron_align */
} micron_state;

static inline void micron_state_init(micron_state *st) {
    if (!st) return;
    st->section_depth = 0;
    st->literal = 0;
    st->align = MICRON_ALIGN_DEFAULT;
}

/* The parse result for one line: rendered text plus the fragments over it, and
 * the line-level facts (alignment, depth, divider/comment flags). */
typedef struct {
    micron_fragment frags[MICRON_MAX_FRAGMENTS];
    size_t count;
    char text[MICRON_MAX_LINE_BYTES + 1];
    size_t text_len;
    uint8_t align;          /* micron_align resolved for this line */
    uint8_t section_depth;  /* clamped depth for this line */
    uint8_t is_divider;
    uint8_t is_comment;
    uint32_t divider_fill;  /* fill code point for a divider (default U+2500) */
    uint8_t truncated;      /* line exceeded the byte budget and was cut */
} micron_line;

static inline const char *micron_fragment_text(const micron_line *ln,
                                               const micron_fragment *f) {
    return ln->text + f->text_off;
}

/* --- small character helpers ------------------------------------------- */

static inline int micron__hex(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static inline int micron__dec(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    return -1;
}

/* Parse a three-char colour into *out. Returns 1 on success, 0 on any failure
 * (too few chars, bad hex, bad decimal) — the caller then resets to default. */
static inline int micron__parse_color(const uint8_t *s, size_t avail,
                                      micron_color *out) {
    if (avail < 3) return 0;
    if (s[0] == 'g') {
        int d0 = micron__dec(s[1]);
        int d1 = micron__dec(s[2]);
        if (d0 < 0 || d1 < 0) return 0;
        int pct = d0 * 10 + d1;                 /* 00..99 percent */
        uint8_t v = (uint8_t)((pct * 255 + 50) / 100);
        out->r = v;
        out->g = v;
        out->b = v;
        out->is_default = 0;
        return 1;
    }
    int r = micron__hex(s[0]);
    int g = micron__hex(s[1]);
    int b = micron__hex(s[2]);
    if (r < 0 || g < 0 || b < 0) return 0;
    out->r = (uint8_t)(r * 17);                 /* doubled nibble: f -> ff */
    out->g = (uint8_t)(g * 17);
    out->b = (uint8_t)(b * 17);
    out->is_default = 0;
    return 1;
}

/* Decode one UTF-8 code point at s[0..len). Returns the code point, or the
 * lead byte itself on a malformed/truncated sequence. */
static inline uint32_t micron__decode_cp(const uint8_t *s, size_t len) {
    if (len == 0) return 0;
    uint8_t c = s[0];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80)
        return (uint32_t)((c & 0x1F) << 6) | (s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && len >= 3 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80)
        return (uint32_t)((c & 0x0F) << 12) | (uint32_t)((s[1] & 0x3F) << 6) |
               (s[2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && len >= 4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
        return (uint32_t)((c & 0x07) << 18) | (uint32_t)((s[1] & 0x3F) << 12) |
               (uint32_t)((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return c;
}

static inline void micron__reset_line(micron_line *out, uint8_t align,
                                      uint8_t depth) {
    out->count = 0;
    out->text_len = 0;
    out->text[0] = '\0';
    out->align = align;
    out->section_depth = depth;
    out->is_divider = 0;
    out->is_comment = 0;
    out->divider_fill = 0x2500;  /* box-drawing horizontal, the default rule */
    out->truncated = 0;
}

static inline void micron__push(micron_line *out, size_t run_start,
                                micron_color fg, micron_color bg,
                                uint8_t attr, uint8_t depth) {
    if (out->text_len > run_start && out->count < MICRON_MAX_FRAGMENTS) {
        micron_fragment *f = &out->frags[out->count++];
        f->text_off = run_start;
        f->len = out->text_len - run_start;
        f->fg = fg;
        f->bg = bg;
        f->attr = attr;
        f->section_depth = depth;
    }
}

/*
 * Parse one source line. `st` carries the sticky state and is updated in place;
 * `line`/`len` is the raw source; `out` receives the fragments and line facts.
 * Never allocates, never reads past len, never writes past the fixed buffers.
 */
static inline void micron_parse_line(micron_state *st, const char *line,
                                     size_t len, micron_line *out) {
    if (!out) return;
    uint8_t carry_align = st ? st->align : MICRON_ALIGN_DEFAULT;
    uint8_t carry_depth = st ? st->section_depth : 0;
    micron__reset_line(out, carry_align, carry_depth);
    if (!st || !line) return;

    const uint8_t *p = (const uint8_t *)line;

    /* DIVERGENCE 3: enforce the byte budget at entry. A line over the budget is
     * cut back to the last whole UTF-8 code point that fits, never buffered in
     * full — the working buffer sits on a small, canary-guarded stack. */
    size_t n = len;
    if (n > MICRON_MAX_LINE_BYTES) {
        size_t take = MICRON_MAX_LINE_BYTES;
        while (take > 0 && (p[take] & 0xC0) == 0x80) take--;
        n = take;
        out->truncated = 1;
    }

    /* Literal-mode toggle is the exact two-byte line "`=" and nothing else. It
     * toggles regardless of the current mode and renders nothing. */
    if (n == 2 && p[0] == '`' && p[1] == '=') {
        st->literal = st->literal ? 0 : 1;
        return;
    }

    /* Inside literal mode the whole (budgeted) line is one plain fragment; no
     * markers, prefixes or escapes are interpreted. */
    if (st->literal) {
        for (size_t i = 0; i < n; i++) {
            if (out->text_len < MICRON_MAX_LINE_BYTES)
                out->text[out->text_len++] = (char)p[i];
        }
        out->text[out->text_len] = '\0';
        micron__push(out, 0, micron_color_default(), micron_color_default(), 0,
                     out->section_depth);
        return;
    }

    micron_color fg = micron_color_default();
    micron_color bg = micron_color_default();
    uint8_t attr = 0;
    uint8_t depth = out->section_depth;
    uint8_t align = carry_align;
    size_t run_start = 0;

#define MICRON__EMIT(ch)                                       \
    do {                                                       \
        if (out->text_len < MICRON_MAX_LINE_BYTES)             \
            out->text[out->text_len++] = (char)(ch);           \
    } while (0)
#define MICRON__CLOSE_RUN()                                    \
    do {                                                       \
        micron__push(out, run_start, fg, bg, attr, depth);     \
        run_start = out->text_len;                             \
    } while (0)

    size_t i = 0;

    /* Line-level prefixes at column 0. */
    if (n > 0 && p[0] == '#') {
        out->is_comment = 1;  /* not rendered */
        return;
    }
    if (n > 0 && p[0] == '>') {
        size_t d = 0;
        while (i < n && p[i] == '>') {
            d++;
            i++;
        }
        /* DIVERGENCE 2: clamp instead of raising on deep nesting. */
        if (d > MICRON_MAX_SECTION_DEPTH) d = MICRON_MAX_SECTION_DEPTH;
        depth = (uint8_t)d;
        out->section_depth = depth;
        st->section_depth = depth;
        /* header text (if any) follows and is parsed like any other run */
    } else if (n > 0 && p[0] == '-') {
        out->is_divider = 1;
        if (n >= 2 && p[1] >= 0x20)
            out->divider_fill = micron__decode_cp(p + 1, n - 1);
        return;  /* a divider carries no text fragments */
    }

    while (i < n) {
        uint8_t c = p[i];

        if (c == '\\') {
            i++;
            if (i < n) {
                MICRON__EMIT(p[i]);  /* next byte prints verbatim */
                i++;
            }
            /* a trailing backslash disappears */
            continue;
        }

        if (c == '`') {
            if (i + 1 >= n) {
                i++;  /* lone trailing backtick: drop it, defensively */
                continue;
            }
            uint8_t m = p[i + 1];
            switch (m) {
                case '`':  /* reset colours and attributes */
                    MICRON__CLOSE_RUN();
                    fg = micron_color_default();
                    bg = micron_color_default();
                    attr = 0;
                    i += 2;
                    break;
                case '_':
                    MICRON__CLOSE_RUN();
                    attr ^= MICRON_ATTR_UNDERLINE;
                    i += 2;
                    break;
                case '!':
                    MICRON__CLOSE_RUN();
                    attr ^= MICRON_ATTR_BOLD;
                    i += 2;
                    break;
                case '*':
                    MICRON__CLOSE_RUN();
                    attr ^= MICRON_ATTR_ITALIC;
                    i += 2;
                    break;
                case 'f':
                    MICRON__CLOSE_RUN();
                    fg = micron_color_default();
                    i += 2;
                    break;
                case 'b':
                    MICRON__CLOSE_RUN();
                    bg = micron_color_default();
                    i += 2;
                    break;
                case 'F':
                case 'B': {
                    size_t avail = n - (i + 2);
                    micron_color col;
                    MICRON__CLOSE_RUN();
                    if (micron__parse_color(p + i + 2, avail, &col)) {
                        if (m == 'F') fg = col; else bg = col;
                        i += 5;
                    } else {
                        /* parse failure or a truncated marker at the line end:
                         * fall back to default and consume only what is there,
                         * so nothing past the line is skipped or corrupted. */
                        if (m == 'F') fg = micron_color_default();
                        else bg = micron_color_default();
                        i += 2 + (avail < 3 ? avail : 3);
                    }
                    break;
                }
                case 'c':
                    align = MICRON_ALIGN_CENTER;
                    i += 2;
                    break;
                case 'l':
                    align = MICRON_ALIGN_LEFT;
                    i += 2;
                    break;
                case 'r':
                    align = MICRON_ALIGN_RIGHT;
                    i += 2;
                    break;
                case 'a':
                    align = MICRON_ALIGN_DEFAULT;
                    i += 2;
                    break;
                default:
                    /* DIVERGENCE 1: unknown marker prints literally. */
                    MICRON__EMIT('`');
                    MICRON__EMIT(m);
                    i += 2;
                    break;
            }
            continue;
        }

        MICRON__EMIT(c);
        i++;
    }

    MICRON__CLOSE_RUN();
    out->text[out->text_len] = '\0';
    out->align = align;
    st->align = align;

#undef MICRON__EMIT
#undef MICRON__CLOSE_RUN
}
