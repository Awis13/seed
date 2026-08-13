#pragma once
/*
 * card_ack.h — pure, host-testable delivery ACK for a notification card
 * (ticket CARD-DELIVERY / C2).
 *
 * WHY THIS EXISTS
 * ---------------
 * A store-and-forward gateway that pushes a card to the pager over an ASYNC wire
 * (a MeshCore DM, an LXMF message) gets NO delivery confirmation: the frame is
 * fire-and-forget, so the gateway cannot know the card landed and must keep
 * retrying it forever. The WiFi lane does not have this problem — POST /notify
 * answers 200 synchronously and that 200 IS the ack — so only the async lanes
 * need one.
 *
 * This ticket adds an app-level uplink ACK, keyed by the card's stable dedup KEY
 * (the same key C1 made agree across every ingress, see notify_wire.h):
 *
 *   A1|key        device -> gateway, "card `key` was admitted, you may dequeue it"
 *
 * It is analogous to the user-reply frame R1|key|reply (main.cpp) and sits in the
 * same '|'-delimited wire family as this project's L1/C1/P/M/R tags. `key` never
 * contains '|' (its alphabet excludes it — see notify_wire_key_plausible), so the
 * single '|' after the tag unambiguously ends the tag and begins the key. An empty
 * key is not a valid ack: a keyless card cannot be dequeued by key, so it gets no
 * A1 at all.
 *
 * WHAT IS PURE HERE (so tools/test_card_ack.cpp drives the real code, not a copy):
 *   - card_ack_lane_for / card_ack_decide  — arrival transport -> ack lane.
 *   - card_ack_encode / card_ack_parse     — the A1|key wire, round-trippable.
 *   - CardAckSeen + its two ops            — the recent-acked dedup ring that
 *                                            stops an ACK storm when the same card
 *                                            is admitted twice.
 * The staging slots and the per-lane emit (which touch the radio / the LXMF
 * outbox and need millis()/crypto) live in skills/meshcore.cpp and skills/rns.cpp;
 * they DRIVE these functions.
 *
 * OFF-LOOP DISCIPLINE (why there is a "stage" and an "emit" at all)
 * ----------------------------------------------------------------
 * An ACK must never be sent from inside the ingress: an LXMF receive callback runs
 * in the 8 ms drain with the jobs-lock held, a mesh DM callback runs on the loop
 * task, and an HTTP handler runs on AsyncTCP — none may take the notify lock,
 * touch the radio, or block. So admission only STAGES the key (a bounded memcpy)
 * and a later poll on the loop task EMITS it, exactly as R1 (reply_upstream_poll)
 * and the peer send (mesh_peer_tx_poll) already do. This header holds no state
 * that a callback mutates; it is decisions and formatting only.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "conv_store.h"   /* ConvTransport (CONV_AGENT/CONV_LXMF/CONV_MESH) */
#include "notify_wire.h"  /* NW_KEY_CAP — the card key width, shared by every ingress */

/* Which uplink an admitted card's ACK leaves on. NONE means "send no A1": either
 * the card is keyless, or it arrived on a lane that already confirmed delivery
 * (HTTP's synchronous 200), so a second ack would be redundant. */
enum {
    CARD_ACK_NONE = 0,
    CARD_ACK_MESH = 1,   /* A1|key as a MeshCore private DM to the gateway */
    CARD_ACK_LXMF = 2,   /* A1|key back over LXMF to the card's sender     */
};

/* The A1 frame is "A1|" + key + NUL. Bounded by the shared key width. */
#define CARD_ACK_FRAME_CAP (3 + NW_KEY_CAP)

/*
 * Map an ARRIVAL transport (a ConvTransport value) to the ack lane its cards
 * should confirm on. Mesh -> mesh DM, LXMF -> LXMF. CONV_AGENT (the bridge/chat
 * wire) gets NONE: it is not a store-and-forward card lane. HTTP has no
 * ConvTransport value at all — it simply never calls the stager — and would map
 * to NONE here too, which is why "an HTTP card is never A1'd" holds by both the
 * absence of a call site AND this mapping.
 */
static inline int card_ack_lane_for(uint8_t via) {
    switch (via) {
        case CONV_MESH: return CARD_ACK_MESH;
        case CONV_LXMF: return CARD_ACK_LXMF;
        default:        return CARD_ACK_NONE;
    }
}

/*
 * The ack decision: given the arrival transport and the card's dedup key, which
 * lane (if any) should emit an A1. Keyless -> NONE (nothing to dequeue by), else
 * the lane the transport maps to.
 */
static inline int card_ack_decide(uint8_t via, const char *key) {
    if (!key || !key[0]) return CARD_ACK_NONE;
    return card_ack_lane_for(via);
}

/*
 * Encode A1|key into `out`. Returns the string length, or 0 (with out[0]='\0')
 * when the key is empty or carries a '|' — a key with a separator would make the
 * frame unparsable, so it is refused rather than sent as a frame the gateway
 * would split wrong.
 */
static inline size_t card_ack_encode(const char *key, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!key || !key[0] || !out || cap < 4) return 0;
    if (strchr(key, '|')) return 0;
    int n = snprintf(out, cap, "A1|%s", key);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

/*
 * Parse an A1|key frame, copying the key into key_out (bounded). Returns 1 on a
 * well-formed frame with a non-empty key, 0 otherwise. The key runs to the end of
 * the frame: A1 has exactly one field after the tag, and the key alphabet has no
 * '|', so there is nothing to mis-split.
 */
static inline int card_ack_parse(const char *frame, char *key_out, size_t cap) {
    if (key_out && cap) key_out[0] = '\0';
    if (!frame || !key_out || cap == 0) return 0;
    if (frame[0] != 'A' || frame[1] != '1' || frame[2] != '|') return 0;
    const char *k = frame + 3;
    if (!k[0]) return 0;                 /* "A1|" with no key */
    if (strchr(k, '|')) return 0;        /* a key never contains '|' */
    size_t n = strlen(k);
    if (n >= cap) return 0;              /* would truncate: refuse rather than lie */
    memcpy(key_out, k, n + 1);
    return 1;
}

/* ---- recent-acked dedup ring --------------------------------------------- *
 *
 * A card can be admitted more than once — a store-and-forward gateway that never
 * saw our A1 re-delivers it, and a dup can slip a cross-transport race. Without a
 * memory of what was just acked, every re-arrival would emit another A1: an ack
 * storm on the exact card the gateway is already unsure about. This bounded ring
 * remembers the last CARD_ACK_SEEN_MAX keys that were successfully acked; a key it
 * already holds is not staged again. It is a fixed-size struct (no allocation, no
 * millis()) so it lives happily as a static on the loop task's owners.
 *
 * The ring is written on EMIT SUCCESS, not on stage: an ack that failed to leave
 * the device is NOT remembered, so the gateway's next re-delivery re-stages and
 * re-tries it. That is the whole retry story — no local retry timer, the
 * store-and-forward sender is the one that resends. */
#define CARD_ACK_SEEN_MAX 8

typedef struct {
    char    key[CARD_ACK_SEEN_MAX][NW_KEY_CAP];
    uint8_t next;   /* circular write cursor */
} CardAckSeen;

/* True if `key` was acked recently (still in the ring). */
static inline int card_ack_seen_has(const CardAckSeen *r, const char *key) {
    if (!r || !key || !key[0]) return 0;
    for (int i = 0; i < CARD_ACK_SEEN_MAX; i++)
        if (r->key[i][0] && strcmp(r->key[i], key) == 0) return 1;
    return 0;
}

/* Remember `key` as acked. A key already in the ring is not added twice, so a
 * card that keeps re-arriving cannot evict the memory of OTHER cards by filling
 * the ring with copies of itself. */
static inline void card_ack_seen_add(CardAckSeen *r, const char *key) {
    if (!r || !key || !key[0]) return;
    if (card_ack_seen_has(r, key)) return;
    snprintf(r->key[r->next], NW_KEY_CAP, "%s", key);
    r->next = (uint8_t)((r->next + 1) % CARD_ACK_SEEN_MAX);
}
