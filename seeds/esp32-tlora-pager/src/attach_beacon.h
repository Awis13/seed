#pragma once
/*
 * attach_beacon.h — pure, host-testable "I'm back, flush my cards" beacon
 * (ticket CARD-DELIVERY / C3, the device slice's last commit).
 *
 * WHY THIS EXISTS
 * ---------------
 * A store-and-forward gateway holds a card for a pager that was offline and
 * releases it when the pager is next seen. Today the device only advertises its
 * return SLOWLY: the mesh keepalive probe (300 s) and the RNS announce cadence.
 * So a device that just reattached to WiFi can sit for minutes before the
 * gateway notices and drains its backlog.
 *
 * C3 adds the FAST proactive signal: the instant the connection coordinator sees
 * the WiFi attach edge (conn_mgr.h on_attach), the device beacons the gateway
 * "I'm here — flush whatever you queued for me," so the backlog arrives at once
 * instead of waiting for the next probe/announce.
 *
 * THE WIRE (what the SEPARATE gateway ticket consumes)
 * ----------------------------------------------------
 *   HTTP (primary — WiFi is the uplink that just came up):
 *       POST {gw}/flush           Authorization: Bearer <gw_token>
 *       body: {"id":"<node id>"}   Content-Type: application/json
 *     The gateway looks up its store-and-forward outbox for <node id> and
 *     pushes every queued card now. Any 2xx means "seen, will flush"; the
 *     device treats non-2xx / no-link as "not delivered" and drops the beacon
 *     (presence is still carried by the slow probe/announce — see the emit
 *     poll in main.cpp).
 *
 *   mesh (H1|id) — DEFINED HERE, emit is a follow-up:
 *       H1|<node id>   a "here/flush" MeshCore private DM to the gateway,
 *     in the same '|'-delimited wire family as R1 (reply) and A1 (card ack).
 *     The WiFi attach edge's natural uplink is HTTP, and mesh already carries a
 *     presence probe, so the firmware wires HTTP only (see main.cpp
 *     flush_beacon_poll); this encoder lets the gateway ticket define the mesh
 *     side and a later commit emit it under the single-ACK-slot discipline.
 *
 * OFF-LOOP DISCIPLINE (why decide + emit are split)
 * -------------------------------------------------
 * conn_mgr_service() runs on the loop task and must not block; the attach edge
 * is detected there. So the edge only STAGES a beacon (a flag flip, this
 * header's attach_beacon_decide), and a later poll on the loop task
 * (flush_beacon_poll in main.cpp) does the blocking HTTP POST — exactly as R1
 * (reply_upstream_poll) and A1 (card_ack) already defer their uplinks. Nothing
 * here reads a clock, a socket, or the radio; it is decisions and formatting
 * only, so tools/test_attach_beacon.cpp drives the real code, not a copy.
 *
 * RATE LIMIT
 * ----------
 * conn_mgr already edge-detects WiFi (on_attach fires only false->true), so a
 * steady link never beacons. On top of that, attach_beacon_decide enforces a
 * floor between beacons: a rapid attach/detach/attach flap inside the window
 * stages exactly one beacon, not one per bounce, so a flapping link cannot spam
 * the gateway.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

/* Minimum gap between two staged beacons. A genuine reconnect after a real
 * outage is far apart; a link that bounces attach->detach->attach in seconds is
 * a flap, and only its first attach inside this window earns a beacon. Chosen
 * shorter than the mesh keepalive (300 s) so a real quick round-trip still gets
 * the fast path, but long enough that a burst of flaps collapses to one POST. */
#define ATTACH_BEACON_FLOOR_MS 30000UL

/* "seed-" + MAC suffix, with room to spare. */
#define ATTACH_BEACON_ID_CAP   32
/* {"id":"<id>"} plus the JSON scaffolding and NUL. */
#define ATTACH_BEACON_BODY_CAP (ATTACH_BEACON_ID_CAP + 16)
/* "H1|" + id + NUL, the mesh here/flush frame. */
#define ATTACH_BEACON_MESH_CAP (3 + ATTACH_BEACON_ID_CAP)

/* Persisted beacon state. Zero-initialise before the first decide().
 *   pending        : a beacon is staged, waiting for the emit poll to send it.
 *   armed          : last_beacon_ms holds a real timestamp (a beacon has been
 *                    staged at least once) — distinguishes "never beaconed" from
 *                    a millis() value of 0.
 *   last_beacon_ms : millis() when the last beacon was staged; the floor anchor. */
struct AttachBeaconState {
    bool     pending;
    bool     armed;
    uint32_t last_beacon_ms;
};

/*
 * The beacon decision. Called every coordinator tick with the WiFi attach edge
 * (conn_mgr's on_attach) and millis(). Returns true, and stages a beacon
 * (pending=true), on a genuine attach that is outside the rate-limit floor;
 * returns false and mutates nothing on a steady link (no edge) or on an attach
 * that lands inside the floor after a recent beacon (flap suppression).
 *
 * The floor is measured from the last STAGED beacon, not from a successful send:
 * a flap that fails to reach the gateway still cost nothing to spam-guard, and
 * the presence probe/announce remain the slow backstop for a genuinely missed
 * flush.
 */
static inline bool attach_beacon_decide(AttachBeaconState *st, bool attach_edge,
                                        uint32_t now) {
    if (!st || !attach_edge) return false;                 /* steady state: nothing */
    if (st->armed &&
        (uint32_t)(now - st->last_beacon_ms) < ATTACH_BEACON_FLOOR_MS)
        return false;                                      /* flap inside floor: suppress */
    st->pending = true;
    st->armed = true;
    st->last_beacon_ms = now;
    return true;
}

/*
 * Build the flush endpoint {gw}/flush into out, tolerating trailing slashes on
 * the configured gateway URL. Returns true on success, false (with out[0]='\0')
 * on an empty/oversized URL — the caller then skips the POST.
 */
static inline bool attach_beacon_endpoint(const char *gw, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!gw || !gw[0] || !out || cap == 0) return false;
    size_t bl = strlen(gw);
    while (bl > 0 && gw[bl - 1] == '/') bl--;
    if (bl == 0) return false;
    int n = snprintf(out, cap, "%.*s/flush", (int)bl, gw);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}

/*
 * Build the JSON body {"id":"<id>"} into out. Returns true on success. The node
 * id alphabet is "seed-" + hex, which never contains a quote or backslash; a id
 * that somehow carries one is refused rather than emitted as body that would
 * break the JSON the gateway parses.
 */
static inline bool attach_beacon_body(const char *id, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!id || !id[0] || !out || cap == 0) return false;
    if (strchr(id, '"') || strchr(id, '\\')) return false;
    int n = snprintf(out, cap, "{\"id\":\"%s\"}", id);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}

/*
 * Encode the mesh here/flush frame H1|id into out. Returns the string length, or
 * 0 (with out[0]='\0') for an empty id, a id carrying '|' (which would make the
 * frame unparsable), or a too-small buffer. Defined for the gateway wire and a
 * future mesh emit; the firmware wires HTTP only (see attach_beacon.h header).
 */
static inline size_t attach_beacon_mesh_encode(const char *id, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!id || !id[0] || !out || cap < 4) return 0;
    if (strchr(id, '|')) return 0;
    int n = snprintf(out, cap, "H1|%s", id);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}
