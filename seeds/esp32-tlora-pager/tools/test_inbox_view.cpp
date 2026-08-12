/*
 * Host tests for the inbox list model in src/inbox_view.h.
 *
 * This is the half of the UI that can actually be proved. Drawing needs the
 * panel and a pair of eyes; ORDER, the transport tag and the unread mark are
 * decisions, and order in particular is the thing nobody can verify by looking
 * — a person can see that a list rendered, not that the third and fourth rows
 * are the right way round.
 *
 * Covered: newest-first ordering, stability on equal stamps, the slot mapping a
 * chosen row is opened through, glyph per transport (and an unknown one),
 * unread marking, label bounding, the rooms flag that decides whether a row
 * opens a room picker or a chat, and the bounds/NULL cases.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/inbox_view.h"

static InboxConvView mk(uint8_t slot, const char *label, uint8_t transport,
                        uint32_t last_use, uint16_t unread, uint8_t rooms) {
    InboxConvView v;
    v.slot = slot;
    v.id = label;          /* tests use the label as the id unless stated */
    v.label = label;
    v.transport = transport;
    v.last_use = last_use;
    v.unread = unread;
    v.has_rooms = rooms;
    return v;
}

/* --- 1. newest first, and the slot travels with the row -------------------- */

static void test_order_is_newest_first(void) {
    InboxConvView in[4] = {
        mk(0, "CLAUDE", CONV_AGENT, 10, 0, 1),
        mk(1, "HERMES", CONV_AGENT, 40, 0, 1),
        mk(2, "a1b2c3d4", CONV_LXMF, 20, 0, 0),
        mk(3, "night walk", CONV_MESH, 30, 0, 0),
    };
    InboxRow out[INBOX_MAX_ROWS];
    int n = inbox_build_rows(in, 4, out, INBOX_MAX_ROWS);
    assert(n == 4);
    /* 40, 30, 20, 10 */
    assert(strcmp(out[0].label, "HERMES") == 0);
    assert(strcmp(out[1].label, "night walk") == 0);
    assert(strcmp(out[2].label, "a1b2c3d4") == 0);
    assert(strcmp(out[3].label, "CLAUDE") == 0);
    /* The slot is what the UI opens, and it must follow the row, not the
     * position — sorting that lost it would open the wrong conversation. */
    assert(out[0].slot == 1 && out[1].slot == 3 &&
           out[2].slot == 2 && out[3].slot == 0);
    /* Ordering must be by the stamp, not by the table: a compiled-in agent
     * must be able to fall below a peer that just wrote. */
    assert(out[3].slot == 0);
}

/* --- 2. equal stamps keep table order, deterministically ------------------- */

static void test_ties_are_stable(void) {
    /* Nothing has been said since boot: every stamp is the same. A comparison
     * that reordered equal keys would make the list reshuffle between two
     * identical redraws. */
    InboxConvView in[3] = {
        mk(0, "one", CONV_AGENT, 7, 0, 1),
        mk(1, "two", CONV_AGENT, 7, 0, 1),
        mk(2, "three", CONV_MESH, 7, 0, 0),
    };
    InboxRow a[INBOX_MAX_ROWS], b[INBOX_MAX_ROWS];
    int na = inbox_build_rows(in, 3, a, INBOX_MAX_ROWS);
    int nb = inbox_build_rows(in, 3, b, INBOX_MAX_ROWS);
    assert(na == 3 && nb == 3);
    for (int i = 0; i < 3; i++) {
        assert(a[i].slot == (uint8_t)i);        /* table order preserved */
        assert(a[i].slot == b[i].slot);         /* and the same every time */
    }
}

/* --- 3. one letter per transport ------------------------------------------- */

static void test_glyphs(void) {
    assert(inbox_transport_glyph(CONV_AGENT) == INBOX_GLYPH_AGENT);
    assert(inbox_transport_glyph(CONV_MESH) == INBOX_GLYPH_MESH);
    assert(inbox_transport_glyph(CONV_LXMF) == INBOX_GLYPH_LXMF);
    /* A transport a later build writes and this one does not know must not
     * borrow another wire's letter. */
    assert(inbox_transport_glyph(99) == INBOX_GLYPH_NONE);
    assert(inbox_transport_glyph(99) != INBOX_GLYPH_AGENT);
    /* The three must be distinguishable, or the column says nothing. */
    assert(INBOX_GLYPH_AGENT != INBOX_GLYPH_MESH);
    assert(INBOX_GLYPH_MESH != INBOX_GLYPH_LXMF);
    assert(INBOX_GLYPH_AGENT != INBOX_GLYPH_LXMF);

    InboxConvView in[3] = {
        mk(0, "a", CONV_LXMF, 3, 0, 0),
        mk(1, "b", CONV_MESH, 2, 0, 0),
        mk(2, "c", CONV_AGENT, 1, 0, 1),
    };
    InboxRow out[INBOX_MAX_ROWS];
    assert(inbox_build_rows(in, 3, out, INBOX_MAX_ROWS) == 3);
    assert(out[0].glyph == INBOX_GLYPH_LXMF);
    assert(out[1].glyph == INBOX_GLYPH_MESH);
    assert(out[2].glyph == INBOX_GLYPH_AGENT);
}

/* --- 4. the unread mark ----------------------------------------------------- */

static void test_unread_mark(void) {
    InboxConvView in[3] = {
        mk(0, "quiet", CONV_AGENT, 3, 0, 1),
        mk(1, "one", CONV_MESH, 2, 1, 0),
        mk(2, "many", CONV_LXMF, 1, 250, 0),
    };
    InboxRow out[INBOX_MAX_ROWS];
    assert(inbox_build_rows(in, 3, out, INBOX_MAX_ROWS) == 3);
    assert(out[0].mark == ' ' && out[0].unread == 0);
    assert(out[1].mark == INBOX_UNREAD_MARK && out[1].unread == 1);
    assert(out[2].mark == INBOX_UNREAD_MARK && out[2].unread == 250);
    /* The count survives, so a later row could show it; the mark is only the
     * one-character form the narrow row has space for. */
    assert(INBOX_UNREAD_MARK != ' ');
}

/* --- 5. rooms vs no rooms decides what a row opens ------------------------- */

static void test_rooms_flag(void) {
    /* A seeded agent opens its room picker; a minted peer has no rooms and
     * goes straight to its thread. Getting this backwards would strand a peer
     * on an empty picker. */
    InboxConvView in[2] = {
        mk(0, "CLAUDE", CONV_AGENT, 2, 0, 1),
        mk(1, "a1b2c3d4", CONV_LXMF, 1, 0, 0),
    };
    InboxRow out[INBOX_MAX_ROWS];
    assert(inbox_build_rows(in, 2, out, INBOX_MAX_ROWS) == 2);
    assert(out[0].has_rooms == 1);
    assert(out[1].has_rooms == 0);
}

/* --- 6. bounds and degenerate input ---------------------------------------- */

static void test_bounds(void) {
    InboxRow out[INBOX_MAX_ROWS];
    InboxConvView one = mk(0, "x", CONV_AGENT, 1, 0, 1);

    assert(inbox_build_rows(NULL, 3, out, INBOX_MAX_ROWS) == 0);
    assert(inbox_build_rows(&one, 1, NULL, INBOX_MAX_ROWS) == 0);
    assert(inbox_build_rows(&one, 1, out, 0) == 0);
    assert(inbox_build_rows(&one, -5, out, INBOX_MAX_ROWS) == 0);

    /* More conversations than the model orders: bounded, not overrun. */
    InboxConvView many[INBOX_MAX_ROWS + 4];
    for (int i = 0; i < INBOX_MAX_ROWS + 4; i++)
        many[i] = mk((uint8_t)i, "z", CONV_AGENT, (uint32_t)(100 - i), 0, 0);
    assert(inbox_build_rows(many, INBOX_MAX_ROWS + 4, out, INBOX_MAX_ROWS)
           == INBOX_MAX_ROWS);
    /* A caller with a smaller output array gets only what fits. */
    assert(inbox_build_rows(many, INBOX_MAX_ROWS + 4, out, 3) == 3);

    /* A label longer than the row field is cut, not overrun, and stays NUL
     * terminated. */
    char longlabel[CONV_LABEL_LEN * 3];
    memset(longlabel, 'q', sizeof(longlabel) - 1);
    longlabel[sizeof(longlabel) - 1] = '\0';
    InboxConvView big = mk(0, longlabel, CONV_MESH, 1, 0, 0);
    assert(inbox_build_rows(&big, 1, out, INBOX_MAX_ROWS) == 1);
    assert(strlen(out[0].label) == CONV_LABEL_LEN - 1);

    /* A conversation with no label yields an empty one rather than a guess. */
    InboxConvView noname = mk(0, NULL, CONV_MESH, 1, 0, 0);
    assert(inbox_build_rows(&noname, 1, out, INBOX_MAX_ROWS) == 1);
    assert(out[0].label[0] == '\0');

    /* An empty table draws no conversation rows (the caller still adds BACK). */
    assert(inbox_build_rows(&one, 0, out, INBOX_MAX_ROWS) == 0);
}

/* --- 7. a recycled slot must not open the stranger who now holds it -------- */

static void test_row_revalidation(void) {
    /* The list is drawn once and rebuilt only on scroll, but the off-loop drain
     * keeps minting — and minting on a full table recycles a slot. A row then
     * shows the old name over a new occupant. */
    InboxConvView in[2] = {
        mk(0, "CLAUDE", CONV_AGENT, 9, 0, 1),
        mk(1, "a1b2c3d4", CONV_LXMF, 8, 0, 0),
    };
    InboxRow out[INBOX_MAX_ROWS];
    assert(inbox_build_rows(in, 2, out, INBOX_MAX_ROWS) == 2);
    assert(strcmp(out[1].id, "a1b2c3d4") == 0);

    /* Slot 1 still holds the same conversation -> open it. */
    assert(inbox_row_matches(&out[1], "a1b2c3d4"));
    /* Slot 1 was recycled for a different peer -> refuse. This is the whole
     * point: the row still SAYS a1b2c3d4. */
    assert(!inbox_row_matches(&out[1], "ff00ff00"));
    /* A seeded row is equally protected. */
    assert(inbox_row_matches(&out[0], "CLAUDE"));
    assert(!inbox_row_matches(&out[0], "HERMES"));

    /* The check is on the ID, not the label: a peer that renames itself is
     * still the same conversation and must still open. */
    InboxConvView renamed = mk(1, "night walk", CONV_MESH, 8, 0, 0);
    renamed.id = "a1b2c3d4";
    InboxRow r2[INBOX_MAX_ROWS];
    assert(inbox_build_rows(&renamed, 1, r2, INBOX_MAX_ROWS) == 1);
    assert(strcmp(r2[0].label, "night walk") == 0);
    assert(inbox_row_matches(&r2[0], "a1b2c3d4"));

    /* Degenerate inputs refuse rather than open something. */
    assert(!inbox_row_matches(NULL, "a1b2c3d4"));
    assert(!inbox_row_matches(&out[0], NULL));
    assert(!inbox_row_matches(&out[0], ""));
    InboxRow blank;
    memset(&blank, 0, sizeof(blank));
    assert(!inbox_row_matches(&blank, "anything"));
}

int main(void) {
    test_order_is_newest_first();
    test_ties_are_stable();
    test_glyphs();
    test_unread_mark();
    test_rooms_flag();
    test_bounds();
    test_row_revalidation();
    printf("inbox view tests: OK\n");
    return 0;
}
