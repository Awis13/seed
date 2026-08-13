/*
 * Host test for the per-card delivery ACK (ticket CARD-DELIVERY / C2).
 *
 * Drives the PURE pieces the device calls — the ack decision, the A1|key wire, and
 * the recent-acked dedup ring in src/card_ack.h — with hand-built inputs and
 * asserts the VALUES. These are the same functions skills/meshcore.cpp
 * (mesh_card_ack_stage/poll) and skills/rns.cpp (lxmf_card_ack_stage/poll) call,
 * so this exercises the real logic, not a copy.
 *
 * MUTATION CHECKS (run by hand, must go RED, then restore):
 *   - card_ack.h card_ack_decide: weaken the keyless guard to `if (!key)` (so an
 *     empty-string key returns the lane) -> test_decide (keyless -> none) FAILS.
 *     "ack a keyless card."
 *   - card_ack.h card_ack_lane_for: make the default (HTTP/agent) return
 *     CARD_ACK_MESH -> test_lane_mapping / test_decide (http -> none) FAIL.
 *     "ack an HTTP card."
 *   - card_ack.h card_ack_seen_has: always return 0 -> test_seen_ring
 *     (already-acked -> skip) FAILS. "double-ack."
 *   - card_ack.h card_ack_encode: emit "A1|" without the key -> the round-trip in
 *     test_wire_roundtrip FAILS.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/card_ack.h"

/* --- 1. arrival transport -> ack lane -------------------------------------- */

static void test_lane_mapping(void) {
    assert(card_ack_lane_for(CONV_MESH)  == CARD_ACK_MESH);
    assert(card_ack_lane_for(CONV_LXMF)  == CARD_ACK_LXMF);
    /* CONV_AGENT is the bridge/chat wire, not a store-and-forward card lane; HTTP
     * has no ConvTransport value and simply never calls the stager — both are
     * "no A1". */
    assert(card_ack_lane_for(CONV_AGENT) == CARD_ACK_NONE);
    assert(card_ack_lane_for(0xEE)       == CARD_ACK_NONE);   /* unknown -> none */
}

/* --- 2. the decision: keyed+async -> ack; keyless / http -> none ------------ */

static void test_decide(void) {
    /* keyed + async arrival -> that lane */
    assert(card_ack_decide(CONV_MESH, "job-backup") == CARD_ACK_MESH);
    assert(card_ack_decide(CONV_LXMF, "pingani-x1") == CARD_ACK_LXMF);

    /* keyless -> none, on every lane (a keyless card cannot be dequeued by key) */
    assert(card_ack_decide(CONV_MESH, "")   == CARD_ACK_NONE);
    assert(card_ack_decide(CONV_LXMF, NULL) == CARD_ACK_NONE);

    /* HTTP-arrival -> none (represented as the agent/bridge lane; the 200 acked) */
    assert(card_ack_decide(CONV_AGENT, "job-backup") == CARD_ACK_NONE);
}

/* --- 3. the A1|key wire: encode/parse round-trip --------------------------- */

static void test_wire_roundtrip(void) {
    char frame[CARD_ACK_FRAME_CAP];
    size_t n = card_ack_encode("k1c.print", frame, sizeof(frame));
    assert(n == strlen("A1|k1c.print"));
    assert(strcmp(frame, "A1|k1c.print") == 0);

    char key[NW_KEY_CAP];
    assert(card_ack_parse(frame, key, sizeof(key)) == 1);
    assert(strcmp(key, "k1c.print") == 0);

    /* a key at the width cap round-trips (24 chars) */
    const char *maxkey = "abcdefghijklmnopqrstuvwx";   /* 24 */
    assert(strlen(maxkey) == NW_KEY_CAP - 1);
    char f2[CARD_ACK_FRAME_CAP], k2[NW_KEY_CAP];
    assert(card_ack_encode(maxkey, f2, sizeof(f2)) == strlen("A1|") + 24);
    assert(card_ack_parse(f2, k2, sizeof(k2)) == 1);
    assert(strcmp(k2, maxkey) == 0);
}

static void test_wire_rejects(void) {
    char frame[CARD_ACK_FRAME_CAP];
    /* empty key -> nothing encoded */
    assert(card_ack_encode("", frame, sizeof(frame)) == 0);
    assert(frame[0] == '\0');
    assert(card_ack_encode(NULL, frame, sizeof(frame)) == 0);

    /* a key with '|' is refused: it would make the frame unparsable */
    assert(card_ack_encode("bad|key", frame, sizeof(frame)) == 0);
    assert(frame[0] == '\0');

    /* a too-small buffer refuses rather than truncating */
    char tiny[4];
    assert(card_ack_encode("job-x", tiny, sizeof(tiny)) == 0);

    char key[NW_KEY_CAP];
    assert(card_ack_parse(NULL, key, sizeof(key)) == 0);
    assert(card_ack_parse("R1|job|hi", key, sizeof(key)) == 0);  /* wrong tag */
    assert(card_ack_parse("A1|", key, sizeof(key)) == 0);        /* no key   */
    assert(card_ack_parse("A2|job", key, sizeof(key)) == 0);     /* wrong ver */
    /* a frame whose key would overflow the caller's buffer is refused, not cut */
    char small[4];
    assert(card_ack_parse("A1|toolongkey", small, sizeof(small)) == 0);
}

/* --- 4. recent-acked dedup ring: already-acked -> skip --------------------- */

static void test_seen_ring(void) {
    CardAckSeen r;
    memset(&r, 0, sizeof(r));

    /* Fresh key: not seen -> would be staged. */
    assert(card_ack_seen_has(&r, "job-backup") == 0);

    /* Mark it acked (as the emit-success path does), then a re-arrival is a dup
     * and must be skipped -- this is the anti-storm guarantee. */
    card_ack_seen_add(&r, "job-backup");
    assert(card_ack_seen_has(&r, "job-backup") == 1);

    /* A DIFFERENT key is unaffected. */
    assert(card_ack_seen_has(&r, "k1c.print") == 0);

    /* Adding the same key twice does not consume two slots: fill the ring with
     * distinct keys and the first one must still be remembered. */
    char k[16];
    card_ack_seen_add(&r, "job-backup");   /* dup: no-op */
    for (int i = 0; i < CARD_ACK_SEEN_MAX - 1; i++) {
        snprintf(k, sizeof(k), "key-%d", i);
        card_ack_seen_add(&r, k);
    }
    /* CARD_ACK_SEEN_MAX distinct keys now held (job-backup + MAX-1 others). */
    assert(card_ack_seen_has(&r, "job-backup") == 1);
    assert(card_ack_seen_has(&r, "key-0") == 1);

    /* One more distinct key evicts the OLDEST (job-backup), bounding the ring. */
    card_ack_seen_add(&r, "overflow");
    assert(card_ack_seen_has(&r, "overflow") == 1);
    assert(card_ack_seen_has(&r, "job-backup") == 0);   /* evicted, ring is bounded */
}

/* --- 5. the whole stage decision, end to end ------------------------------- *
 * Mirrors what mesh_card_ack_stage / lxmf_card_ack_stage decide: emit an ack iff
 * the lane matches AND the key was not acked recently. */
static int would_stage(int lane, uint8_t via, const char *key, CardAckSeen *seen) {
    if (card_ack_decide(via, key) != lane) return 0;
    if (card_ack_seen_has(seen, key)) return 0;   /* already acked: skip */
    return 1;
}

static void test_stage_decision(void) {
    CardAckSeen seen;
    memset(&seen, 0, sizeof(seen));

    /* keyed mesh card: stage on the mesh lane, not the lxmf lane */
    assert(would_stage(CARD_ACK_MESH, CONV_MESH, "job-x", &seen) == 1);
    assert(would_stage(CARD_ACK_LXMF, CONV_MESH, "job-x", &seen) == 0);

    /* once acked, a re-arrival of the same key does not stage again */
    card_ack_seen_add(&seen, "job-x");
    assert(would_stage(CARD_ACK_MESH, CONV_MESH, "job-x", &seen) == 0);

    /* keyless and http never stage on any lane */
    assert(would_stage(CARD_ACK_MESH, CONV_MESH, "",  &seen) == 0);
    assert(would_stage(CARD_ACK_LXMF, CONV_LXMF, "",  &seen) == 0);
    assert(would_stage(CARD_ACK_MESH, CONV_AGENT, "job-y", &seen) == 0);
}

int main(void) {
    test_lane_mapping();
    test_decide();
    test_wire_roundtrip();
    test_wire_rejects();
    test_seen_ring();
    test_stage_decision();
    printf("card ack delivery tests: OK\n");
    return 0;
}
