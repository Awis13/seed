/*
 * Host tests for the micron markup parser.
 *
 * micron pages arrive over the radio as untrusted text, so the parser is where
 * a malformed page turns into either a clean fallback or a crash. These cases
 * pin every marker, both colour forms, the escape rules, literal mode, and the
 * three deliberate divergences from NomadNet's reference parser (literal
 * unknown marker, clamped section depth, 512-byte line budget). No Arduino, no
 * radio, no board — the whole thing is exercised on the host.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/micron/micron.h"

/* Parse a NUL-terminated C string with a fresh state. */
static void parse(micron_state *st, const char *s, micron_line *out) {
    micron_parse_line(st, s, strlen(s), out);
}

/* Concatenate all fragment text back into one string (rendered output). */
static void rendered(const micron_line *ln, char *buf, size_t cap) {
    size_t n = 0;
    for (size_t f = 0; f < ln->count; f++) {
        const char *t = micron_fragment_text(ln, &ln->frags[f]);
        for (size_t k = 0; k < ln->frags[f].len && n + 1 < cap; k++)
            buf[n++] = t[k];
    }
    buf[n] = '\0';
}

/* Find the fragment covering the first occurrence of `needle`, or NULL. */
static const micron_fragment *frag_with(const micron_line *ln,
                                        const char *needle) {
    size_t nlen = strlen(needle);
    for (size_t f = 0; f < ln->count; f++) {
        const char *t = micron_fragment_text(ln, &ln->frags[f]);
        if (ln->frags[f].len >= nlen && memcmp(t, needle, nlen) == 0)
            return &ln->frags[f];
    }
    return NULL;
}

int main() {
    micron_state st;
    micron_line ln;
    char buf[MICRON_MAX_LINE_BYTES + 1];

    /* --- attribute markers toggle and scope correctly ------------------- */
    micron_state_init(&st);
    parse(&st, "a`!b`!c", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "abc") == 0);
    {
        const micron_fragment *a = frag_with(&ln, "a");
        const micron_fragment *b = frag_with(&ln, "b");
        const micron_fragment *c = frag_with(&ln, "c");
        assert(a && b && c);
        assert((a->attr & MICRON_ATTR_BOLD) == 0);
        assert(b->attr & MICRON_ATTR_BOLD);          /* bold on */
        assert((c->attr & MICRON_ATTR_BOLD) == 0);   /* toggled back off */
    }

    /* underline and italic, combined */
    micron_state_init(&st);
    parse(&st, "`_`*x", &ln);
    {
        const micron_fragment *x = frag_with(&ln, "x");
        assert(x);
        assert(x->attr & MICRON_ATTR_UNDERLINE);
        assert(x->attr & MICRON_ATTR_ITALIC);
        assert((x->attr & MICRON_ATTR_BOLD) == 0);
    }

    /* backtick-backtick resets every attribute and colour */
    micron_state_init(&st);
    parse(&st, "`!`_`Ff00hi``bye", &ln);
    {
        const micron_fragment *hi = frag_with(&ln, "hi");
        const micron_fragment *bye = frag_with(&ln, "bye");
        assert(hi && bye);
        assert(hi->attr == (MICRON_ATTR_BOLD | MICRON_ATTR_UNDERLINE));
        assert(!hi->fg.is_default && hi->fg.r == 0xff);
        assert(bye->attr == 0);
        assert(bye->fg.is_default && bye->bg.is_default);
    }

    /* --- colour formats -------------------------------------------------- */
    /* hex nibbles doubled: f00 -> ff0000 */
    micron_state_init(&st);
    parse(&st, "`Ff00X", &ln);
    {
        const micron_fragment *x = frag_with(&ln, "X");
        assert(x && !x->fg.is_default);
        assert(x->fg.r == 0xff && x->fg.g == 0x00 && x->fg.b == 0x00);
    }
    /* a mixed value, each channel doubled independently */
    micron_state_init(&st);
    parse(&st, "`B1a2Y", &ln);
    {
        const micron_fragment *y = frag_with(&ln, "Y");
        assert(y && !y->bg.is_default);
        assert(y->bg.r == 0x11 && y->bg.g == 0xaa && y->bg.b == 0x22);
    }
    /* grey via decimal percent: g50 -> mid grey, equal channels */
    micron_state_init(&st);
    parse(&st, "`Fg50Z", &ln);
    {
        const micron_fragment *z = frag_with(&ln, "Z");
        assert(z && !z->fg.is_default);
        assert(z->fg.r == z->fg.g && z->fg.g == z->fg.b);
        assert(z->fg.r == 128);  /* (50*255+50)/100 */
    }
    /* a colour that fails to parse falls back to default */
    micron_state_init(&st);
    parse(&st, "`FzzzW", &ln);
    {
        const micron_fragment *w = frag_with(&ln, "W");
        assert(w && w->fg.is_default);
    }
    /* `f / `b reset a plane back to default */
    micron_state_init(&st);
    parse(&st, "`Ff00`Bg20`fA`bB", &ln);
    {
        const micron_fragment *a = frag_with(&ln, "A");
        const micron_fragment *b = frag_with(&ln, "B");
        assert(a && a->fg.is_default && !a->bg.is_default);
        assert(b && b->fg.is_default && b->bg.is_default);
    }

    /* --- escaping -------------------------------------------------------- */
    micron_state_init(&st);
    parse(&st, "\\`", &ln);          /* backslash-backtick prints a backtick */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "`") == 0);

    micron_state_init(&st);
    parse(&st, "\\\\", &ln);         /* backslash-backslash prints a backslash */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "\\") == 0);

    micron_state_init(&st);
    parse(&st, "\\x", &ln);          /* backslash-x prints x (backslash gone) */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "x") == 0);

    micron_state_init(&st);
    parse(&st, "a\\`!b", &ln);       /* escaped backtick is not a marker */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "a`!b") == 0);
    assert(ln.count == 1 && ln.frags[0].attr == 0);

    /* --- literal mode ---------------------------------------------------- */
    /* "`=" exactly toggles literal mode and renders nothing */
    micron_state_init(&st);
    parse(&st, "`=", &ln);
    assert(st.literal == 1 && ln.count == 0);
    /* now markers are inert data */
    parse(&st, "`!not bold `Ff00", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "`!not bold `Ff00") == 0);
    assert(ln.count == 1 && ln.frags[0].attr == 0);
    /* a second "`=" toggles back out */
    parse(&st, "`=", &ln);
    assert(st.literal == 0);
    parse(&st, "`!bold", &ln);
    assert(ln.count == 1 && (ln.frags[0].attr & MICRON_ATTR_BOLD));

    /* a longer line containing "`=" is NOT a toggle; inline `= is unknown and
     * prints literally (our divergence), leaving literal mode untouched */
    micron_state_init(&st);
    parse(&st, "x`=y", &ln);
    assert(st.literal == 0);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "x`=y") == 0);

    /* --- truncated colour at end of line does not corrupt the rest ------- */
    /* "`F" at the very end with no colour chars: preceding text intact */
    micron_state_init(&st);
    parse(&st, "hello`F", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "hello") == 0);
    /* an invalid 3-char colour consumes exactly three and keeps going: the
     * tail after the (failed) colour survives, not eaten or shifted */
    micron_state_init(&st);
    parse(&st, "hello`Fabworld", &ln);   /* a,b hex but w is not -> failure */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "helloorld") == 0);

    /* --- unknown marker prints literally (divergence 1) ------------------ */
    micron_state_init(&st);
    parse(&st, "a`Zb", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "a`Zb") == 0);

    /* --- alignment: last marker in the line wins, and it is sticky ------- */
    micron_state_init(&st);
    parse(&st, "`ltext`rmore`cend", &ln);
    assert(ln.align == MICRON_ALIGN_CENTER);
    parse(&st, "next line", &ln);        /* carries the previous alignment */
    assert(ln.align == MICRON_ALIGN_CENTER);

    /* --- section depth: sticky, and clamped (divergence 2) --------------- */
    /* depth >= 4 is the reference parser's crash point: must clamp, not die */
    micron_state_init(&st);
    parse(&st, ">>>>Header", &ln);       /* exactly 4 */
    assert(ln.section_depth == 4);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "Header") == 0);
    parse(&st, "body under header", &ln);  /* depth sticks across lines */
    assert(ln.section_depth == 4);
    /* far past the max clamps to MICRON_MAX_SECTION_DEPTH */
    {
        char deep[40];
        for (int k = 0; k < 20; k++) deep[k] = '>';
        strcpy(deep + 20, "Deep");
        micron_state_init(&st);
        parse(&st, deep, &ln);
        assert(ln.section_depth == MICRON_MAX_SECTION_DEPTH);
        rendered(&ln, buf, sizeof(buf));
        assert(strcmp(buf, "Deep") == 0);
    }

    /* --- comment and divider lines -------------------------------------- */
    micron_state_init(&st);
    parse(&st, "# a comment", &ln);
    assert(ln.is_comment && ln.count == 0);
    parse(&st, "-", &ln);
    assert(ln.is_divider && ln.count == 0 && ln.divider_fill == 0x2500);
    parse(&st, "-=", &ln);               /* explicit fill char */
    assert(ln.is_divider && ln.divider_fill == '=');

    /* --- UTF-8 payload survives (Cyrillic bytes, no literal Cyrillic src) */
    micron_state_init(&st);
    parse(&st, "`!\xD0\xB3\xD0\xB4", &ln);   /* two 2-byte code points, bold */
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "\xD0\xB3\xD0\xB4") == 0);
    assert(ln.count == 1 && (ln.frags[0].attr & MICRON_ATTR_BOLD));

    /* --- line budget (divergence 3) ------------------------------------- */
    /* An over-budget line is truncated at a code-point boundary at entry. A
     * multi-byte code point straddling the 512 boundary is dropped whole, so
     * text_len never exceeds the budget and never splits a code point. */
    {
        char big[MICRON_MAX_LINE_BYTES + 64];
        size_t k = 0;
        /* fill up to one byte short of the budget with ASCII 'a' */
        for (; k < MICRON_MAX_LINE_BYTES - 1; k++) big[k] = 'a';
        /* then a 2-byte code point that straddles the boundary, then more */
        big[k++] = (char)0xD0;
        big[k++] = (char)0xB3;
        for (int j = 0; j < 20; j++) big[k++] = 'z';
        micron_parse_line(&st, big, k, &ln);
        assert(ln.truncated == 1);
        rendered(&ln, buf, sizeof(buf));
        /* kept 511 'a', dropped the straddling code point and everything after */
        assert(strlen(buf) == MICRON_MAX_LINE_BYTES - 1);
        for (size_t j = 0; j < MICRON_MAX_LINE_BYTES - 1; j++)
            assert(buf[j] == 'a');
    }
    /* a purely ASCII over-budget line keeps exactly the budget */
    {
        char big[MICRON_MAX_LINE_BYTES + 64];
        for (size_t k = 0; k < sizeof(big); k++) big[k] = 'b';
        micron_state_init(&st);
        micron_parse_line(&st, big, sizeof(big), &ln);
        assert(ln.truncated == 1);
        rendered(&ln, buf, sizeof(buf));
        assert(strlen(buf) == MICRON_MAX_LINE_BYTES);
    }

    /* --- hostile inputs: bounds must hold, never OOB or crash ----------- */
    /* Fragment cap: "toggle bold, one char" over and over drives count to the
     * cap. It must stop at the cap, not run off frags[]. Dropping the
     * `count < MICRON_MAX_FRAGMENTS` guard turns this into an OOB write that
     * ASan catches and that this <= assertion also fails. */
    {
        char many[MICRON_MAX_LINE_BYTES + 1];
        size_t k = 0;
        while (k + 3 <= MICRON_MAX_LINE_BYTES) {
            many[k++] = '`';
            many[k++] = '!';
            many[k++] = 'a';   /* one styled run per triple */
        }
        micron_state_init(&st);
        micron_parse_line(&st, many, k, &ln);
        assert(ln.count == MICRON_MAX_FRAGMENTS);  /* driven to the cap */
        assert(ln.count <= MICRON_MAX_FRAGMENTS);
        /* every fragment stays inside the text buffer */
        for (size_t f = 0; f < ln.count; f++)
            assert(ln.frags[f].text_off + ln.frags[f].len <= ln.text_len);
    }

    /* All-continuation-byte over-budget line: the UTF-8 backoff at the budget
     * has to back up past every 0x80 to a boundary, which here is offset 0.
     * The cut-to-zero must be sane, not underflow. */
    {
        char cont[MICRON_MAX_LINE_BYTES + 32];
        for (size_t k = 0; k < sizeof(cont); k++) cont[k] = (char)0x80;
        micron_state_init(&st);
        micron_parse_line(&st, cont, sizeof(cont), &ln);
        assert(ln.truncated == 1);
        assert(ln.text_len == 0 && ln.count == 0);
    }

    /* A line of nothing but backticks: every pair is a reset, no text, no
     * fragments, and no read past the end on the final byte. */
    {
        char ticks[600];
        for (size_t k = 0; k < sizeof(ticks); k++) ticks[k] = '`';
        micron_state_init(&st);
        micron_parse_line(&st, ticks, sizeof(ticks), &ln);
        assert(ln.count == 0 && ln.text_len == 0);
    }

    /* A huge '>' run clamps depth instead of overflowing it. */
    {
        char gts[5000];
        for (size_t k = 0; k < sizeof(gts); k++) gts[k] = '>';
        micron_state_init(&st);
        micron_parse_line(&st, gts, sizeof(gts), &ln);
        assert(ln.section_depth == MICRON_MAX_SECTION_DEPTH);
    }

    /* An embedded NUL is just a data byte (length is explicit, not strlen). */
    {
        const char nul_line[] = {'a', '\0', 'b'};
        micron_state_init(&st);
        micron_parse_line(&st, nul_line, sizeof(nul_line), &ln);
        assert(ln.count == 1);
        assert(ln.frags[0].text_off == 0 && ln.frags[0].len == 3);
    }

    /* `F with exactly one and exactly two trailing bytes: both are truncated
     * colours, consumed without skipping past the line and without corrupting
     * the text that came before. */
    micron_state_init(&st);
    parse(&st, "x`Fa", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "x") == 0);
    micron_state_init(&st);
    parse(&st, "x`Fab", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "x") == 0);

    /* A lone trailing backslash simply disappears. */
    micron_state_init(&st);
    parse(&st, "ab\\", &ln);
    rendered(&ln, buf, sizeof(buf));
    assert(strcmp(buf, "ab") == 0);

    /* --- defensive arguments -------------------------------------------- */
    micron_state_init(&st);
    micron_parse_line(&st, NULL, 5, &ln);
    assert(ln.count == 0);
    micron_parse_line(NULL, "x", 1, &ln);
    assert(ln.count == 0);
    micron_parse_line(&st, "x", 1, NULL);  /* must not crash */

    return 0;
}
