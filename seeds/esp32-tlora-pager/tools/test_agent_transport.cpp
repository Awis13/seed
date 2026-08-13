/*
 * Host tests for the agent-send rung selection in src/agent_transport.h — the
 * pure decision the device half (transport_send_agent() in skills/agents.cpp)
 * drives. These exercise the SAME function the firmware calls
 * (agent_pick_transport), so the ladder choice is checked BY VALUE rather than
 * re-encoded here.
 *
 * The ladder, highest rung first:
 *   1. WiFi/gateway — ONLY when the route home is PROVEN (REACH_UP) and a bridge
 *      is configured. REACH_UNKNOWN and REACH_DOWN both skip it (that skip is the
 *      whole point of C2: a merely-up link that swallows the POST must not stall
 *      the send).
 *   2. Reticulum   — when WiFi is not taken and the RNS rung is usable.
 *   3. MeshCore    — last resort.
 *   -  nothing usable -> AGENT_RUNG_NONE.
 *
 * Mutation checks (edit src/agent_transport.h, rebuild, expect RED, restore):
 *   M1 treat UNKNOWN as UP : in agent_pick_transport, use
 *      `(reach == REACH_UP || reach == REACH_UNKNOWN)` for the WiFi guard ->
 *      test_unknown_skips_wifi + test_down_skips_wifi's UNKNOWN sibling FAIL
 *      (an UNKNOWN link is wrongly sent to WiFi).
 *   M2 drop the RNS rung   : delete the `if (rns_avail) return AGENT_RUNG_RNS;`
 *      line -> test_down_takes_rns + test_unknown_takes_rns FAIL (a down route
 *      with RNS available falls all the way to MeshCore).
 */

#include <assert.h>
#include <stdio.h>

#include "../src/agent_transport.h"

static int checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// A proven route home takes the WiFi rung, ahead of both lower rungs.
static int test_up_takes_wifi() {
    CHECK(agent_pick_transport(REACH_UP, true, true, true) == AGENT_RUNG_WIFI);
    CHECK(agent_pick_transport(REACH_UP, true, false, false) == AGENT_RUNG_WIFI);
    // ...but only if a bridge is actually configured; otherwise fall past it.
    CHECK(agent_pick_transport(REACH_UP, false, true, true) == AGENT_RUNG_RNS);
    CHECK(agent_pick_transport(REACH_UP, false, false, true) == AGENT_RUNG_MESHCORE);
    return 0;
}

// A DOWN route must NOT take WiFi even though a bridge is configured: it drops to
// the Reticulum rung when that is usable. This is the captive/no-route case C2
// exists to fix.
static int test_down_takes_rns() {
    CHECK(agent_pick_transport(REACH_DOWN, true, true, true) == AGENT_RUNG_RNS);
    CHECK(agent_pick_transport(REACH_DOWN, true, true, false) == AGENT_RUNG_RNS);
    return 0;
}

// UNKNOWN (never probed, or a stale verdict) is treated like DOWN for the WiFi
// rung — proof of a route home is required, absence of proof is not enough.
static int test_unknown_takes_rns() {
    CHECK(agent_pick_transport(REACH_UNKNOWN, true, true, true) == AGENT_RUNG_RNS);
    return 0;
}

// With WiFi skipped and no RNS, the send lands on MeshCore.
static int test_falls_to_meshcore() {
    CHECK(agent_pick_transport(REACH_DOWN, true, false, true) == AGENT_RUNG_MESHCORE);
    CHECK(agent_pick_transport(REACH_UNKNOWN, true, false, true) == AGENT_RUNG_MESHCORE);
    CHECK(agent_pick_transport(REACH_DOWN, false, false, true) == AGENT_RUNG_MESHCORE);
    return 0;
}

// Nothing configured/usable -> a defined no-op the caller reports, never a block.
static int test_nothing_available() {
    CHECK(agent_pick_transport(REACH_UP, false, false, false) == AGENT_RUNG_NONE);
    CHECK(agent_pick_transport(REACH_DOWN, false, false, false) == AGENT_RUNG_NONE);
    CHECK(agent_pick_transport(REACH_UNKNOWN, false, false, false) == AGENT_RUNG_NONE);
    // WiFi-reachable but no bridge and no other rung is still nothing usable.
    CHECK(agent_pick_transport(REACH_UP, false, false, false) == AGENT_RUNG_NONE);
    return 0;
}

// The specific bug guard: an UNKNOWN or DOWN link with a bridge present must not
// be routed to WiFi (that is the ~6s captive stall). Pinned on its own so a
// regression names itself.
static int test_unknown_down_skip_wifi() {
    CHECK(agent_pick_transport(REACH_UNKNOWN, true, false, false) != AGENT_RUNG_WIFI);
    CHECK(agent_pick_transport(REACH_DOWN, true, false, false) != AGENT_RUNG_WIFI);
    // With no lower rung either, they degrade to NONE rather than to WiFi.
    CHECK(agent_pick_transport(REACH_UNKNOWN, true, false, false) == AGENT_RUNG_NONE);
    CHECK(agent_pick_transport(REACH_DOWN, true, false, false) == AGENT_RUNG_NONE);
    return 0;
}

int main() {
    if (test_up_takes_wifi()) return 1;
    if (test_down_takes_rns()) return 1;
    if (test_unknown_takes_rns()) return 1;
    if (test_falls_to_meshcore()) return 1;
    if (test_nothing_available()) return 1;
    if (test_unknown_down_skip_wifi()) return 1;
    printf("agent_transport: %d checks passed\n", checks);
    return 0;
}
