#pragma once
/*
 * feed_view.h — the unified Messages feed model (pure, host-testable).
 *
 * The pager is a MESSENGER, and its Messages section is ONE feed: notification
 * cards and chat conversations shown TOGETHER, ordered by time, newest first.
 * The two kinds used to live on two screens — MESSAGES drew cards from the
 * notify queue, AGENTS drew conversations from the conversation table — so a
 * card that just arrived and a chat that just moved could not be compared side
 * by side, which is exactly what a messenger has to do.
 *
 * This header MERGES the two, at the VIEW layer only. Neither store is touched:
 * the caller (main.cpp) snapshots cards out of notify_view() and conversations
 * out of the agents_* accessors, hands both arrays here, and feed_build_rows()
 * interleaves them into one time-ordered list of FeedRow.
 *
 * WHAT IS PURE HERE, AND WHY IT MATTERS. Ordering is a decision; drawing is not.
 * Interleaving two sources by time is the one part a screenshot cannot confirm
 * and a person cannot eyeball once there are many rows from two origins — so it
 * is done in a function with no Arduino and no panel, and proved in
 * tools/test_feed_view.cpp by VALUE. This mirrors inbox_view.h, whose glyphs,
 * unread mark and CONV_ID_LEN it reuses rather than re-declaring.
 *
 * THE MERGE KEY IS THE UNIX EPOCH, NOT THE MONOTONIC COUNTER. A conversation
 * carries last_use (a boot-relative counter used only to pick an eviction
 * victim) AND last.ts (the unix second of its newest message); a card carries
 * created_epoch (the unix second it arrived). Only the unix seconds are
 * comparable ACROSS the two stores, so the feed orders by those. An entry whose
 * clock was unset when it was stamped has epoch 0; 0 is smaller than any real
 * second, so such entries sink to the bottom (oldest) under the same
 * descending comparator — a defined, tested ordering, stable among themselves.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "inbox_view.h"   /* inbox_transport_glyph, INBOX_UNREAD_MARK, CONV_ID_LEN */
#include "utf8_text.h"

/* Where a row came from. Persisted nowhere — a pure view tag. */
enum FeedOrigin {
    FEED_CARD = 0,   /* a notification card; handle is a notify id            */
    FEED_CONV = 1,   /* a chat conversation; handle is a table slot + conv id */
};

/* A card title is at most 60 source bytes. Keep all of it here; the renderer
 * applies its own cell limit without ever cutting a UTF-8 code point. */
#define FEED_LABEL_LEN   61

/* Input caps. Cards come from a 40-deep queue but the caller only ever offers
 * the newest handful; conversations are bounded by the table. The internal sort
 * buffer holds both at once. */
#define FEED_MAX_CARDS   12
#define FEED_MAX_CONVS   INBOX_MAX_ROWS               /* 8 */
#define FEED_ORDER_MAX   (FEED_MAX_CARDS + FEED_MAX_CONVS)

/* A notification card reduced to what the feed needs. `sev` is the one-letter
 * severity ('I'/'W'/'C') the caller already derives from the level name — kept
 * here as a char so this header need not know the notify level enum, exactly as
 * inbox_view.h stays free of the store. */
struct FeedCardView {
    uint32_t    id;       /* notify id — the handle a click reopens the card by */
    uint32_t    epoch;    /* created_epoch (unix secs); 0 = clock was unset      */
    const char *title;
    char        sev;      /* severity letter, caller-supplied                    */
    uint8_t     unread;   /* 0/1                                                  */
};

/* A conversation reduced to what the feed needs. Mirrors InboxConvView, but the
 * merge key is last.ts (unix secs of the newest message), NOT last_use. */
struct FeedConvView {
    uint8_t     slot;     /* index in the live table — the handle to open        */
    const char *id;       /* stable identity; revalidated at click time          */
    const char *label;
    uint32_t    epoch;    /* last.ts (unix secs); 0 = no dated message yet        */
    uint8_t     transport;
    uint16_t    unread;
    uint8_t     has_rooms;
};

/* One merged, drawn row. Carries the origin tag, the merge key that placed it,
 * and BOTH handle shapes — only the one named by `origin` is meaningful. */
struct FeedRow {
    uint8_t  origin;                  /* FEED_CARD or FEED_CONV                   */
    uint32_t epoch;                   /* the unix second this row sorted on       */
    uint32_t card_id;                 /* FEED_CARD handle (0 for a conv row)      */
    uint8_t  slot;                    /* FEED_CONV handle (0 for a card row)      */
    char     glyph;                   /* card: severity letter; conv: transport   */
    char     mark;                    /* INBOX_UNREAD_MARK or ' '                 */
    uint16_t unread;
    uint8_t  has_rooms;               /* conv only: opens a room picker vs a chat */
    char     label[FEED_LABEL_LEN];
    /* The conversation identity a conv row DISPLAYED, so a click can prove the
     * slot still holds it (see inbox_row_matches). Empty for a card row. */
    char     id[CONV_ID_LEN];
};

/* Internal sort record: the merge key plus enough to fetch the source back. */
struct FeedSortItem {
    uint32_t epoch;
    uint16_t seq;      /* global insertion order: cards first, then convs        */
    uint8_t  origin;
    uint8_t  idx;      /* index into the matching input array                    */
};

static inline void feed_emit_card(const FeedCardView &c, FeedRow &r) {
    r.origin = FEED_CARD;
    r.epoch = c.epoch;
    r.card_id = c.id;
    r.slot = 0;
    r.glyph = c.sev;
    r.unread = c.unread ? 1 : 0;
    r.mark = c.unread ? INBOX_UNREAD_MARK : ' ';
    r.has_rooms = 0;
    r.id[0] = '\0';
    utf8_text_copy(r.label, sizeof(r.label), c.title, SIZE_MAX, false);
}

static inline void feed_emit_conv(const FeedConvView &c, FeedRow &r) {
    r.origin = FEED_CONV;
    r.epoch = c.epoch;
    r.card_id = 0;
    r.slot = c.slot;
    r.glyph = inbox_transport_glyph(c.transport);
    r.unread = c.unread;
    r.mark = c.unread ? INBOX_UNREAD_MARK : ' ';
    r.has_rooms = c.has_rooms;
    size_t k = 0;
    if (c.id)
        for (; c.id[k] && k + 1 < sizeof(r.id); k++) r.id[k] = c.id[k];
    r.id[k] = '\0';
    utf8_text_copy(r.label, sizeof(r.label), c.label, SIZE_MAX, false);
}

/*
 * Build the merged, ordered feed.
 *
 * NEWEST FIRST, by unix epoch, cards and conversations interleaved. Two entries
 * from different stores that share a second are ordered by a stable global
 * sequence — every card before every conversation, and within one store the
 * order the caller supplied — so the same two inputs always merge to the same
 * list and it never flickers between redraws. epoch 0 (clock unset) is the
 * smallest key and therefore sorts last, stable among its peers.
 *
 * Copies label and id into each row, so the result is self-contained: the
 * caller may drop the store lock the moment this returns even though the input
 * pointers reach into the live tables.
 *
 * Returns the number of rows written (bounded by max and by FEED_ORDER_MAX).
 */
static inline int feed_build_rows(const FeedCardView *cards, int nc,
                                  const FeedConvView *convs, int nv,
                                  FeedRow *out, int max) {
    if (!out || max <= 0) return 0;
    if (!cards) nc = 0;
    if (!convs) nv = 0;
    if (nc < 0) nc = 0;
    if (nv < 0) nv = 0;
    if (nc > FEED_MAX_CARDS) nc = FEED_MAX_CARDS;
    if (nv > FEED_MAX_CONVS) nv = FEED_MAX_CONVS;

    /* Firmware calls this on the loop task. Keep the merge workspace out of
     * its small stack; callers consume the completed rows before the next call. */
    static FeedSortItem it[FEED_ORDER_MAX];
    int count = 0;
    for (int i = 0; i < nc && count < FEED_ORDER_MAX; i++) {
        it[count].epoch = cards[i].epoch;
        it[count].seq = (uint16_t)count;
        it[count].origin = FEED_CARD;
        it[count].idx = (uint8_t)i;
        count++;
    }
    for (int i = 0; i < nv && count < FEED_ORDER_MAX; i++) {
        it[count].epoch = convs[i].epoch;
        it[count].seq = (uint16_t)count;
        it[count].origin = FEED_CONV;
        it[count].idx = (uint8_t)i;
        count++;
    }

    /* Insertion sort, newest epoch first. STRICTLY greater shifts, so equal
     * epochs keep their global seq order — the stability the list depends on. */
    for (int i = 1; i < count; i++) {
        FeedSortItem cur = it[i];
        int j = i - 1;
        while (j >= 0 && it[j].epoch < cur.epoch) {
            it[j + 1] = it[j];
            j--;
        }
        it[j + 1] = cur;
    }

    int written = 0;
    for (int i = 0; i < count && written < max; i++) {
        FeedRow &r = out[written++];
        if (it[i].origin == FEED_CARD) feed_emit_card(cards[it[i].idx], r);
        else                           feed_emit_conv(convs[it[i].idx], r);
    }
    return written;
}
