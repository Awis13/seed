#pragma once
/*
 * inbox_view.h — shared conversation-row helpers (pure, host-testable).
 *
 * The retired standalone inbox and the live unified feed share transport
 * glyphs, unread markers, and the stable-identity guard used before opening a
 * snapshotted conversation row. Keep those small decisions host-testable.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "conv_store.h"

/* Maximum conversation rows in the unified feed. */
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

/* Stable identity copied into a snapshotted conversation row. */
struct InboxRow {
    /* The id, not the label: a peer may rename itself between messages, and a
     * renamed conversation is still the same conversation. */
    char     id[CONV_ID_LEN];
};

/*
 * Does a row still point at the conversation it was drawn for?
 *
 * The unified feed is snapshotted when it opens, but
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
