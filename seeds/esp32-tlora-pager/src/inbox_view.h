#pragma once
/*
 * inbox_view.h — the inbox list model (pure, host-testable).
 *
 * The chat menu used to be three fixed strings baked into the renderer:
 * CLAUDE, HERMES, BACK. The store behind it now holds conversations from every
 * transport, minted as peers appear, so the list has to be built rather than
 * declared — ordered by when each thread last moved, tagged with the wire it
 * lives on, and marked when it has something the user has not read.
 *
 * WHAT IS PURE HERE, AND WHY IT MATTERS. Ordering, the transport tag and the
 * unread mark are decisions; drawing is not. Keeping the decisions in a
 * function with no Arduino and no panel means the ORDER OF THE ROWS can be
 * tested — and order is exactly the part a screenshot cannot confirm and a
 * person cannot eyeball reliably once there are eight of them.
 *
 * The caller (main.cpp) snapshots the live table under the store lock into
 * InboxConvView records and calls inbox_build_rows(). It must be a snapshot:
 * the off-loop drain mints and evicts conversations, so a renderer walking the
 * live array could read a slot mid-rewrite.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "conv_store.h"

/* Rows this model will order at once. It is the conversation table's capacity,
 * but named separately because this header is pure and CONV_MAX belongs to the
 * firmware store; main.cpp static_asserts the two agree. */
#define INBOX_MAX_ROWS 8

/* One transport, one letter. Letters rather than symbols because the panel font
 * is a 5x7 bitmap where a glyph has 35 pixels to be recognisable in, and
 * because the clock already speaks this language: "M" is mesh health there and
 * "W" is WireGuard. A row reading "M peer-name" is the same alphabet. */
#define INBOX_GLYPH_AGENT 'A'
#define INBOX_GLYPH_MESH  'M'
#define INBOX_GLYPH_LXMF  'L'
#define INBOX_GLYPH_NONE  '?'   /* a transport this build does not know */

/* Marks a row with unread arrivals. One character, because the row also has to
 * fit a label and a glyph at text scale 2. */
#define INBOX_UNREAD_MARK '*'

static inline char inbox_transport_glyph(uint8_t transport) {
    switch (transport) {
        case CONV_AGENT: return INBOX_GLYPH_AGENT;
        case CONV_MESH:  return INBOX_GLYPH_MESH;
        case CONV_LXMF:  return INBOX_GLYPH_LXMF;
        default:         return INBOX_GLYPH_NONE;
    }
}

/* A conversation reduced to what the list needs. `slot` is its index in the
 * live table, carried through so a chosen row can be opened without the
 * renderer knowing anything about the store. */
struct InboxConvView {
    uint8_t     slot;
    const char *id;          /* stable identity; the label may change */
    const char *label;
    uint8_t     transport;
    uint32_t    last_use;
    uint16_t    unread;
    uint8_t     has_rooms;   /* seeded agents have rooms; peers do not */
};

/* One drawn row. */
struct InboxRow {
    uint8_t  slot;
    char     glyph;
    char     mark;                    /* INBOX_UNREAD_MARK or ' ' */
    uint16_t unread;
    uint8_t  has_rooms;
    char     label[CONV_LABEL_LEN];
    /* The identity of what this row DISPLAYED, so a click can prove the slot
     * still holds it. The id, not the label: a peer may rename itself between
     * messages, and a renamed conversation is still the same conversation. */
    char     id[CONV_ID_LEN];
};

/*
 * Does a row still point at the conversation it was drawn for?
 *
 * The list is snapshotted when the inbox opens and rebuilt only on scroll, but
 * the off-loop drain keeps running: a peer that speaks while the user is
 * reading the list can mint, and minting on a full table recycles the
 * least-recently-used slot. The row then still shows the old name while its
 * slot holds someone else — so opening it would put the user in a stranger's
 * thread under the label of a correspondent they meant to read. Comparing the
 * id at click time costs one string compare and closes it.
 */
static inline bool inbox_row_matches(const InboxRow *row, const char *live_id) {
    if (!row || !live_id) return false;
    if (!row->id[0] || !live_id[0]) return false;
    return strcmp(row->id, live_id) == 0;
}

/*
 * Build the ordered row list.
 *
 * NEWEST FIRST, by last_use. That is the only ordering a messenger can defend:
 * a fixed order buries whoever just wrote to you under whoever was compiled in
 * first, which is precisely the failure the two-name menu had. Ties keep table
 * order so the result is deterministic — two conversations can share a stamp
 * (nothing has touched either since boot), and a comparison that reordered
 * equal keys would make the list flicker between redraws.
 *
 * A conversation with no label falls back to nothing rather than to a
 * placeholder: the caller supplies the id as the label when a name is unknown,
 * and inventing a second fallback here would hide that.
 *
 * Returns the number of rows written (bounded by max).
 */
static inline int inbox_build_rows(const InboxConvView *in, int n,
                                   InboxRow *out, int max) {
    if (!in || !out || max <= 0) return 0;
    if (n < 0) n = 0;

    /* Insertion sort on the index list: n is CONV_MAX (8), so this is a few
     * dozen comparisons on a list the user is about to read. */
    int order[INBOX_MAX_ROWS];
    int count = 0;
    for (int i = 0; i < n && count < INBOX_MAX_ROWS; i++) order[count++] = i;
    for (int i = 1; i < count; i++) {
        int cur = order[i];
        int j = i - 1;
        /* STRICTLY greater: equal stamps must not swap, or the order is
         * unstable between two identical builds. */
        while (j >= 0 && in[order[j]].last_use < in[cur].last_use) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }

    int written = 0;
    for (int i = 0; i < count && written < max; i++) {
        const InboxConvView &c = in[order[i]];
        InboxRow &r = out[written++];
        r.slot = c.slot;
        r.glyph = inbox_transport_glyph(c.transport);
        r.unread = c.unread;
        r.mark = c.unread ? INBOX_UNREAD_MARK : ' ';
        r.has_rooms = c.has_rooms;
        size_t k = 0;
        if (c.id)
            for (; c.id[k] && k + 1 < sizeof(r.id); k++) r.id[k] = c.id[k];
        r.id[k] = '\0';
        size_t j = 0;
        if (c.label)
            for (; c.label[j] && j + 1 < sizeof(r.label); j++) r.label[j] = c.label[j];
        r.label[j] = '\0';
    }
    return written;
}
