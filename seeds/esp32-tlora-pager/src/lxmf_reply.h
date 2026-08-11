#pragma once
/*
 * lxmf_reply.h — pure helpers for replying to an LXMF-origin message
 * (TLORA-LXMF C4). Two pieces the firmware wraps with a lock, the software
 * crypto and the existing send ladder; both are pure, hold no I/O and are
 * host-tested (tools/test_lxmf_reply.cpp) the way lxmf_route.h and
 * agents_chat_route.h are:
 *
 *   1. lxmf_reply_build() — turn a typed reply into an LxmfMsg addressed BACK
 *      to the sender: dest = the sender's lxmf.delivery hash, source = ours,
 *      title EMPTY, content = the reply bounded to a single packet on a UTF-8
 *      boundary, no fields. The caller signs it (C1 sign-injection contract)
 *      and packs it with lxmf_encode_packed().
 *
 *   2. LxmfOriginTable — a tiny fixed map room-name -> the last inbound LXMF
 *      sender's source_hash, so a reply typed in a room that received an LXMF
 *      message goes back to THAT sender instead of the configured seed.pager
 *      peer. A later seed.pager inbound into the same room CLEARS the entry
 *      (origin reverted to seed.pager); a later LXMF inbound UPDATES it. The
 *      firmware owns one instance and guards it with the outbox spinlock,
 *      because the lookup runs on the AsyncTCP task (POST /agents/send) while
 *      the set/clear run on the loop task (the receive poll).
 *
 * No Arduino / FreeRTOS / SD / RNS / crypto dependency — only <string.h> and
 * the C1 codec header for LxmfMsg and the fixed geometry.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "lxmf_codec.h"

/*
 * Single-packet reply content ceiling.
 *
 * The reply ships FULL-PACKED (dest hash included), because that is exactly
 * what this port's C1 codec emits and its C3 receive parser consumes:
 * Destination::receive() hands the packet callback the raw decrypted plaintext
 * and nothing on this port prepends the destination hash. So the wire budget is
 * the outbox slot (RNS_OUTBOX_PAYLOAD_MAX = 383) minus the 96-byte header and
 * ~16 bytes of msgpack framing (array + float64 timestamp + empty title bin +
 * content bin header + empty fields map): 383 - 96 - 16 = 271. 256 keeps a
 * comfortable margin and is a round value. A reply longer than this is cut here
 * (a marker/link/continuation reply is a later ticket).
 */
#define LXMF_REPLY_CONTENT_MAX 256

/*
 * Build a single-packet reply message. Returns 1 when the text was truncated to
 * fit the ceiling above (cut on a UTF-8 boundary, never mid-character), else 0.
 * `m` is fully zeroed first; dest/src are the two 16-byte lxmf.delivery hashes.
 */
static inline int lxmf_reply_build(LxmfMsg *m,
                                   const uint8_t dest[LXMF_HASH_LEN],
                                   const uint8_t src[LXMF_HASH_LEN],
                                   double ts, const char *text) {
    if (!m) return 0;
    memset(m, 0, sizeof(*m));
    if (!dest || !src) return 0;
    memcpy(m->dest_hash, dest, LXMF_HASH_LEN);
    memcpy(m->source_hash, src, LXMF_HASH_LEN);
    m->timestamp = ts;

    /* Empty title, no fields: a plain single-packet reply. */
    m->title[0] = '\0';
    m->title_len = 0;

    size_t n = text ? strlen(text) : 0;
    int truncated = 0;
    if (n > LXMF_REPLY_CONTENT_MAX) {
        n = LXMF_REPLY_CONTENT_MAX;
        /* text[n] is the first byte left behind; a UTF-8 continuation byte
         * (10xxxxxx) there means the cut split a character, so give the whole
         * character back. Mirrors reply_upstream_mesh()'s cut in main.cpp. */
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
        truncated = 1;
    }
    if (n) memcpy(m->content, text, n);
    m->content[n] = '\0';
    m->content_len = n;
    return truncated;
}

/* ======================================================================== *
 *  origin table: room name -> last inbound LXMF sender source_hash
 * ======================================================================== */

/* Mirror AGENT_SESSION_LEN / AGENT_SESSIONS_MAX in skills/agents.cpp: the key
 * is a resolved (sanitised) room name and there are at most that many rooms. */
#ifndef LXMF_ORIGIN_ROOM_CAP
#define LXMF_ORIGIN_ROOM_CAP 24
#endif
#ifndef LXMF_ORIGIN_SLOTS
#define LXMF_ORIGIN_SLOTS 8
#endif

typedef struct {
    char    room[LXMF_ORIGIN_ROOM_CAP];
    uint8_t src[LXMF_HASH_LEN];
    uint8_t valid;
} LxmfOriginEntry;

typedef struct {
    LxmfOriginEntry e[LXMF_ORIGIN_SLOTS];
    uint32_t        cursor;   /* round-robin eviction when full */
} LxmfOriginTable;

static inline void lxmf_origin_init(LxmfOriginTable *t) {
    if (t) memset(t, 0, sizeof(*t));
}

static inline int lxmf_origin_find(const LxmfOriginTable *t, const char *room) {
    if (!t || !room || !room[0]) return -1;
    for (int i = 0; i < LXMF_ORIGIN_SLOTS; i++)
        if (t->e[i].valid && strcmp(t->e[i].room, room) == 0) return i;
    return -1;
}

/* Record (or update) the sender of an LXMF message routed into `room`. A name
 * that does not fit the key cap is ignored rather than truncated — a truncated
 * key would answer a DIFFERENT room. */
static inline void lxmf_origin_set(LxmfOriginTable *t, const char *room,
                                   const uint8_t src[LXMF_HASH_LEN]) {
    if (!t || !room || !room[0] || !src) return;
    if (strlen(room) >= LXMF_ORIGIN_ROOM_CAP) return;
    int i = lxmf_origin_find(t, room);
    if (i < 0) {
        for (int k = 0; k < LXMF_ORIGIN_SLOTS; k++)
            if (!t->e[k].valid) { i = k; break; }
        if (i < 0) { i = (int)(t->cursor % LXMF_ORIGIN_SLOTS); t->cursor++; }
    }
    memset(&t->e[i], 0, sizeof(t->e[i]));
    memcpy(t->e[i].room, room, strlen(room) + 1);
    memcpy(t->e[i].src, src, LXMF_HASH_LEN);
    t->e[i].valid = 1;
}

/* Forget a room's LXMF origin — called when a seed.pager message lands in it,
 * so its reply reverts to the seed.pager path. A no-op when nothing is stored. */
static inline void lxmf_origin_clear(LxmfOriginTable *t, const char *room) {
    int i = lxmf_origin_find(t, room);
    if (i >= 0) t->e[i].valid = 0;
}

/* True + the 16-byte sender hash into `out` when `room` has an LXMF origin. */
static inline int lxmf_origin_lookup(const LxmfOriginTable *t, const char *room,
                                     uint8_t out[LXMF_HASH_LEN]) {
    int i = lxmf_origin_find(t, room);
    if (i < 0) return 0;
    if (out) memcpy(out, t->e[i].src, LXMF_HASH_LEN);
    return 1;
}
