/*
 * Host tests for the unified Messages feed model in src/feed_view.h.
 *
 * The feed interleaves two stores — notification cards and chat conversations —
 * into one time-ordered list. That interleave is the half of the UI that can be
 * proved: drawing needs the panel and a pair of eyes, but the ORDER across two
 * sources, the tie rule, the epoch==0 sink, and which handle each row carries
 * are decisions. They are checked here by VALUE, not by shape.
 *
 * Covered: newest-first ordering with cards and convs interleaved by unix
 * epoch; the tagged handle a click travels on (card id vs conv slot+id); glyph
 * and unread mark per origin; tie stability (equal epochs, deterministic across
 * two identical builds, cards-before-convs); the epoch==0 ordering choice
 * (treated as oldest, stable among peers); the top-N cap keeping the newest;
 * and the bounds / NULL / truncation cases.
 *
 * Also covered: a routed chat door stays a feed card beside its conversation.
 * The card is the notification; the conversation is the thread. Section 9
 * models that scan by VALUE. test_feed_view.sh goes RED if the old hide-the-
 * door filter (`if (notify_is_chat(v)) continue;`) comes back.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/feed_view.h"

static FeedCardView card(uint32_t id, uint32_t epoch, const char *title,
                         char sev, uint8_t unread) {
    FeedCardView c;
    c.id = id;
    c.epoch = epoch;
    c.title = title;
    c.sev = sev;
    c.unread = unread;
    return c;
}

static FeedConvView conv(uint8_t slot, const char *id, const char *label,
                         uint32_t epoch, uint8_t transport, uint16_t unread,
                         uint8_t rooms) {
    FeedConvView v;
    v.slot = slot;
    v.id = id;
    v.label = label;
    v.epoch = epoch;
    v.transport = transport;
    v.unread = unread;
    v.has_rooms = rooms;
    return v;
}

/* --- 1. cards and conversations interleave by unix epoch, newest first ------ */

static void test_interleaved_newest_first(void) {
    FeedCardView cards[2] = {
        card(101, 1000, "boiler alarm", 'C', 1),   /* newest overall */
        card(102, 400,  "backup done",  'I', 0),   /* oldest overall */
    };
    FeedConvView convs[2] = {
        conv(3, "a1b2c3d4", "night walk", 700, CONV_MESH, 0, 0),
        conv(0, "claude",   "CLAUDE",     900, CONV_AGENT, 2, 1),
    };
    FeedRow out[FEED_ORDER_MAX];
    int n = feed_build_rows(cards, 2, convs, 2, out, FEED_ORDER_MAX);
    assert(n == 4);

    /* 1000 card, 900 conv, 700 conv, 400 card */
    assert(out[0].origin == FEED_CARD && out[0].card_id == 101);
    assert(out[1].origin == FEED_CONV && out[1].slot == 0);
    assert(strcmp(out[1].label, "CLAUDE") == 0);
    assert(out[2].origin == FEED_CONV && out[2].slot == 3);
    assert(strcmp(out[2].label, "night walk") == 0);
    assert(out[3].origin == FEED_CARD && out[3].card_id == 102);

    /* The merge key travelled with the row. */
    assert(out[0].epoch == 1000 && out[1].epoch == 900 &&
           out[2].epoch == 700 && out[3].epoch == 400);
}

/* --- 2. the tagged handle a click opens ------------------------------------ */

static void test_tagged_handles(void) {
    FeedCardView cards[1] = { card(77, 500, "page", 'W', 1) };
    FeedConvView convs[1] = { conv(5, "ff00ff00", "peer", 600, CONV_LXMF, 3, 0) };
    FeedRow out[FEED_ORDER_MAX];
    int n = feed_build_rows(cards, 1, convs, 1, out, FEED_ORDER_MAX);
    assert(n == 2);

    /* conv is newer -> row 0. Its handle is the slot AND the identity, so a
     * recycled slot can be revalidated before opening a stranger's thread. */
    assert(out[0].origin == FEED_CONV);
    assert(out[0].slot == 5);
    assert(strcmp(out[0].id, "ff00ff00") == 0);
    assert(out[0].card_id == 0);           /* card handle unused on a conv row */

    /* card row carries the notify id, no conv identity. */
    assert(out[1].origin == FEED_CARD);
    assert(out[1].card_id == 77);
    assert(out[1].id[0] == '\0');
    assert(out[1].slot == 0);              /* conv handle unused on a card row */
}

/* --- 3. glyph and unread mark per origin ----------------------------------- */

static void test_glyph_and_mark(void) {
    FeedCardView cards[2] = {
        card(1, 90, "crit", 'C', 1),       /* card glyph = its severity letter */
        card(2, 80, "quiet", 'I', 0),
    };
    FeedConvView convs[3] = {
        conv(0, "x", "agent", 70, CONV_AGENT, 0, 1),
        conv(1, "y", "mesh",  60, CONV_MESH,  4, 0),
        conv(2, "z", "lxmf",  50, CONV_LXMF,  1, 0),
    };
    FeedRow out[FEED_ORDER_MAX];
    int n = feed_build_rows(cards, 2, convs, 3, out, FEED_ORDER_MAX);
    assert(n == 5);

    /* order: 90c 80c 70A 60M 50L */
    assert(out[0].glyph == 'C' && out[0].mark == INBOX_UNREAD_MARK);
    assert(out[1].glyph == 'I' && out[1].mark == ' ');
    assert(out[2].glyph == INBOX_GLYPH_AGENT && out[2].mark == ' ');
    assert(out[3].glyph == INBOX_GLYPH_MESH  && out[3].mark == INBOX_UNREAD_MARK);
    assert(out[3].unread == 4);
    assert(out[4].glyph == INBOX_GLYPH_LXMF  && out[4].mark == INBOX_UNREAD_MARK);

    /* rooms flag survives so the click can pick chat vs room picker. */
    assert(out[2].has_rooms == 1);
    assert(out[3].has_rooms == 0);
}

/* --- 4. equal epochs: stable, cards before convs, same every build --------- */

static void test_ties_are_stable(void) {
    /* Everything shares one second. The rule: cards keep their input order and
     * come before conversations (which keep theirs), and the result is
     * identical on a second build so the list does not reshuffle on redraw. */
    FeedCardView cards[2] = {
        card(10, 5, "c-first",  'I', 0),
        card(11, 5, "c-second", 'I', 0),
    };
    FeedConvView convs[2] = {
        conv(0, "v0", "v-first",  5, CONV_AGENT, 0, 1),
        conv(1, "v1", "v-second", 5, CONV_MESH,  0, 0),
    };
    FeedRow a[FEED_ORDER_MAX], b[FEED_ORDER_MAX];
    int na = feed_build_rows(cards, 2, convs, 2, a, FEED_ORDER_MAX);
    int nb = feed_build_rows(cards, 2, convs, 2, b, FEED_ORDER_MAX);
    assert(na == 4 && nb == 4);

    assert(a[0].origin == FEED_CARD && a[0].card_id == 10);
    assert(a[1].origin == FEED_CARD && a[1].card_id == 11);
    assert(a[2].origin == FEED_CONV && a[2].slot == 0);
    assert(a[3].origin == FEED_CONV && a[3].slot == 1);

    for (int i = 0; i < 4; i++) {
        assert(a[i].origin == b[i].origin);
        assert(a[i].card_id == b[i].card_id);
        assert(a[i].slot == b[i].slot);
    }
}

/* --- 5. epoch==0 (clock unset) is oldest, stable among peers ---------------- */

static void test_zero_epoch_is_oldest(void) {
    /* A card and a conv stamped before the clock was set (epoch 0) must fall
     * BELOW everything dated, and hold their own relative order. */
    FeedCardView cards[2] = {
        card(1, 0,   "undated card", 'I', 0),   /* epoch 0 -> oldest */
        card(2, 300, "dated card",   'I', 0),
    };
    FeedConvView convs[2] = {
        conv(0, "u", "undated conv", 0,   CONV_MESH, 0, 0),   /* epoch 0 */
        conv(1, "d", "dated conv",   200, CONV_MESH, 0, 0),
    };
    FeedRow out[FEED_ORDER_MAX];
    int n = feed_build_rows(cards, 2, convs, 2, out, FEED_ORDER_MAX);
    assert(n == 4);

    /* Dated entries first, newest down: 300 card, 200 conv. */
    assert(out[0].card_id == 2 && out[0].epoch == 300);
    assert(out[1].slot == 1 && out[1].epoch == 200);
    /* Then the epoch==0 pair, cards-before-convs by the stable seq rule. */
    assert(out[2].origin == FEED_CARD && out[2].card_id == 1 && out[2].epoch == 0);
    assert(out[3].origin == FEED_CONV && out[3].slot == 0 && out[3].epoch == 0);
}

/* --- 6. the top-N cap keeps the newest across BOTH stores ------------------- */

static void test_cap_keeps_newest(void) {
    /* Six entries, room for three: the three newest survive regardless of which
     * store they came from, and the dropped ones are the oldest. */
    FeedCardView cards[3] = {
        card(1, 100, "c100", 'I', 0),
        card(2, 500, "c500", 'I', 0),   /* newest */
        card(3, 150, "c150", 'I', 0),
    };
    FeedConvView convs[3] = {
        conv(0, "a", "v400", 400, CONV_MESH, 0, 0),
        conv(1, "b", "v050", 50,  CONV_MESH, 0, 0),   /* oldest */
        conv(2, "c", "v300", 300, CONV_MESH, 0, 0),
    };
    FeedRow out[3];
    int n = feed_build_rows(cards, 3, convs, 3, out, 3);
    assert(n == 3);
    assert(out[0].card_id == 2 && out[0].epoch == 500);
    assert(out[1].slot == 0 && out[1].epoch == 400);
    assert(out[2].slot == 2 && out[2].epoch == 300);
}

/* --- 7. the display capacity does not cut the merged feed back to eight ---- */

static void test_full_feed_exceeds_legacy_eight(void) {
    FeedCardView cards[FEED_MAX_CARDS];
    FeedConvView convs[FEED_MAX_CONVS];
    FeedRow out[FEED_ORDER_MAX];
    for (int i = 0; i < FEED_MAX_CARDS; i++)
        cards[i] = card((uint32_t)(100 + i), (uint32_t)(1000 - i),
                        "card", 'I', 0);
    for (int i = 0; i < FEED_MAX_CONVS; i++)
        convs[i] = conv((uint8_t)i, "peer", "chat",
                        (uint32_t)(500 - i), CONV_MESH, 0, 0);

    int n = feed_build_rows(cards, FEED_MAX_CARDS, convs, FEED_MAX_CONVS,
                            out, FEED_ORDER_MAX);
    assert(FEED_ORDER_MAX > 8);
    assert(n == FEED_ORDER_MAX);
    assert(out[0].card_id == 100);
    assert(out[FEED_MAX_CARDS].origin == FEED_CONV);
    assert(out[n - 1].slot == FEED_MAX_CONVS - 1);
}

/* --- 8. one store empty ----------------------------------------------------- */

static void test_single_source(void) {
    FeedCardView cards[2] = {
        card(1, 20, "a", 'I', 0),
        card(2, 40, "b", 'W', 1),
    };
    FeedRow out[FEED_ORDER_MAX];

    /* Cards only. */
    int n = feed_build_rows(cards, 2, NULL, 0, out, FEED_ORDER_MAX);
    assert(n == 2);
    assert(out[0].card_id == 2 && out[1].card_id == 1);

    /* Convs only. */
    FeedConvView convs[2] = {
        conv(0, "p", "peer0", 30, CONV_LXMF, 0, 0),
        conv(1, "q", "peer1", 60, CONV_MESH, 0, 0),
    };
    n = feed_build_rows(NULL, 0, convs, 2, out, FEED_ORDER_MAX);
    assert(n == 2);
    assert(out[0].slot == 1 && out[1].slot == 0);
}

/* --- 9. bounds and degenerate input ---------------------------------------- */

static void test_bounds(void) {
    FeedRow out[FEED_ORDER_MAX];
    FeedCardView c = card(1, 1, "x", 'I', 0);
    FeedConvView v = conv(0, "y", "y", 1, CONV_AGENT, 0, 0);

    assert(feed_build_rows(&c, 1, &v, 1, NULL, FEED_ORDER_MAX) == 0);
    assert(feed_build_rows(&c, 1, &v, 1, out, 0) == 0);
    assert(feed_build_rows(NULL, 5, NULL, 5, out, FEED_ORDER_MAX) == 0);
    assert(feed_build_rows(&c, -3, &v, -3, out, FEED_ORDER_MAX) == 0);

    /* A caller with a smaller output array gets only what fits. */
    assert(feed_build_rows(&c, 1, &v, 1, out, 1) == 1);

    /* More than the model orders at once: bounded, not overrun. */
    FeedCardView many[FEED_MAX_CARDS + 4];
    for (int i = 0; i < FEED_MAX_CARDS + 4; i++)
        many[i] = card((uint32_t)(i + 1), (uint32_t)(1000 - i), "z", 'I', 0);
    int n = feed_build_rows(many, FEED_MAX_CARDS + 4, NULL, 0, out, FEED_ORDER_MAX);
    assert(n == FEED_MAX_CARDS);   /* the excess cards were dropped, not read */

    /* A card title longer than the row field is cut, not overrun, NUL-kept. */
    char longtitle[FEED_LABEL_LEN * 3];
    memset(longtitle, 'q', sizeof(longtitle) - 1);
    longtitle[sizeof(longtitle) - 1] = '\0';
    FeedCardView big = card(9, 5, longtitle, 'I', 0);
    assert(feed_build_rows(&big, 1, NULL, 0, out, FEED_ORDER_MAX) == 1);
    assert(strlen(out[0].label) == FEED_LABEL_LEN - 1);

    /* A long multibyte title is cut before a whole code point, never between
     * UTF-8 bytes. This is the exact input the timestamped renderer receives. */
    char multibyte[64];
    for (size_t i = 0; i < sizeof(multibyte) - 2; i += 2) {
        multibyte[i] = (char)0xD0;
        multibyte[i + 1] = (char)0xA1;
    }
    multibyte[sizeof(multibyte) - 2] = '\0';
    FeedCardView utf = card(10, 5, multibyte, 'I', 0);
    assert(feed_build_rows(&utf, 1, NULL, 0, out, FEED_ORDER_MAX) == 1);
    size_t pos = 0;
    while (out[0].label[pos]) {
        size_t width = utf8_text_decode(out[0].label + pos, NULL);
        assert(width == 2);
        pos += width;
    }
    assert(pos < strlen(multibyte));

    /* A card with no title yields an empty label rather than a guess. */
    FeedCardView noname = card(9, 5, NULL, 'I', 0);
    assert(feed_build_rows(&noname, 1, NULL, 0, out, FEED_ORDER_MAX) == 1);
    assert(out[0].label[0] == '\0');

    /* Both stores empty draws nothing. */
    assert(feed_build_rows(&c, 0, &v, 0, out, FEED_ORDER_MAX) == 0);
}

/* --- 10. A routed chat door stays a feed card beside its thread ------------- */

static void test_door_card_stays_beside_thread(void) {
    FeedCardView cards[6] = {
        card(201, 950, "hermes says hi", 'I', 0),
        card(202, 940, "claude says hi", 'I', 0),
        card(205, 945, "ambiguous door", 'I', 1),
        card(206, 935, "empty door", 'I', 0),
        card(203, 930, "orphan door",    'I', 0),
        card(204, 920, "boiler alarm",   'C', 1),
    };
    int nc = 6;

    FeedConvView convs[1] = {
        conv(0, "hermes", "HERMES", 950, CONV_AGENT, 1, 0),
    };
    FeedRow out[FEED_ORDER_MAX];
    int n = feed_build_rows(cards, nc, convs, 1, out, FEED_ORDER_MAX);

    /* Routed doors sit beside the thread: card 201 ties epoch 950 with the
     * hermes conversation, cards-before-convs. */
    assert(n == 7);
    assert(out[0].origin == FEED_CARD && out[0].card_id == 201);
    assert(out[1].origin == FEED_CONV && out[1].slot == 0);
    assert(strcmp(out[1].id, "hermes") == 0);
    assert(out[2].origin == FEED_CARD && out[2].card_id == 205);
    assert(out[3].origin == FEED_CARD && out[3].card_id == 202);
    assert(out[4].origin == FEED_CARD && out[4].card_id == 206);
    assert(out[5].origin == FEED_CARD && out[5].card_id == 203);
    assert(out[6].origin == FEED_CARD && out[6].card_id == 204);
    assert(out[6].glyph == 'C' && out[6].mark == INBOX_UNREAD_MARK);
}

int main(void) {
    test_interleaved_newest_first();
    test_tagged_handles();
    test_glyph_and_mark();
    test_ties_are_stable();
    test_zero_epoch_is_oldest();
    test_cap_keeps_newest();
    test_full_feed_exceeds_legacy_eight();
    test_single_source();
    test_bounds();
    test_door_card_stays_beside_thread();
    printf("feed view tests: OK\n");
    return 0;
}
