/*
 * Host test for the attach flush beacon (ticket CARD-DELIVERY / C3).
 *
 * Drives the PURE pieces the device calls — the beacon decision + rate-limit
 * floor, the {gw}/flush endpoint builder, the {"id":…} body builder, and the
 * H1|id mesh frame — in src/attach_beacon.h, with hand-built inputs, asserting
 * the VALUES. attach_beacon_decide is the exact function conn_mgr_service()
 * calls on the attach edge, and the builders are what flush_beacon_http() calls,
 * so this exercises the real logic, not a copy.
 *
 * MUTATION CHECKS (run by hand, must go RED, then restore):
 *   - attach_beacon.h attach_beacon_decide: invert the edge guard (`!attach_edge`
 *     -> `attach_edge`, so a steady tick stages and an attach does not) ->
 *     test_attach_stages / test_steady_state_nothing FAIL. "beacon on steady
 *     state."
 *   - attach_beacon.h attach_beacon_decide: drop the rate-limit (delete the
 *     `st->armed && ... < FLOOR` early return) -> test_rate_limit_floor /
 *     test_flap_suppressed FAIL. "a flap spams the gateway."
 *   - attach_beacon.h attach_beacon_endpoint: emit "%s" without "/flush" ->
 *     test_endpoint FAILS.
 *   - attach_beacon.h attach_beacon_body: emit {"id":""} (drop %s) ->
 *     test_body FAILS.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/attach_beacon.h"

/* --- 1. the decision: attach edge stages; steady state stages nothing ------- */

static void test_attach_stages(void) {
    AttachBeaconState st;
    memset(&st, 0, sizeof(st));

    /* First genuine attach: stage, and pending is raised for the emit poll. */
    assert(attach_beacon_decide(&st, true, 1000) == true);
    assert(st.pending == true);
    assert(st.armed == true);
    assert(st.last_beacon_ms == 1000);
}

static void test_steady_state_nothing(void) {
    AttachBeaconState st;
    memset(&st, 0, sizeof(st));

    /* No attach edge -> no beacon, no state change, on any number of ticks. */
    assert(attach_beacon_decide(&st, false, 1000) == false);
    assert(attach_beacon_decide(&st, false, 2000) == false);
    assert(attach_beacon_decide(&st, false, 99999) == false);
    assert(st.pending == false);
    assert(st.armed == false);
}

/* --- 2. the rate-limit floor: one beacon per genuine attach ----------------- */

static void test_rate_limit_floor(void) {
    AttachBeaconState st;
    memset(&st, 0, sizeof(st));

    /* Attach at t=1000 stages. */
    assert(attach_beacon_decide(&st, true, 1000) == true);

    /* A re-attach one floor-minus-1 ms later is a flap: suppressed. */
    assert(attach_beacon_decide(&st, true, 1000 + ATTACH_BEACON_FLOOR_MS - 1) == false);
    assert(st.last_beacon_ms == 1000);   /* anchor unmoved by the suppressed attach */

    /* Exactly at the floor boundary: a genuine reconnect earns a beacon. */
    uint32_t t = 1000 + ATTACH_BEACON_FLOOR_MS;
    assert(attach_beacon_decide(&st, true, t) == true);
    assert(st.last_beacon_ms == t);

    /* And the floor re-arms from the new anchor. */
    assert(attach_beacon_decide(&st, true, t + 1) == false);
    assert(attach_beacon_decide(&st, true, t + ATTACH_BEACON_FLOOR_MS) == true);
}

static void test_flap_suppressed(void) {
    AttachBeaconState st;
    memset(&st, 0, sizeof(st));

    /* A burst: attach, then five rapid re-attaches all inside the floor. Only
     * the first beacons — the flap collapses to a single POST, not six. */
    int beacons = 0;
    if (attach_beacon_decide(&st, true, 5000)) beacons++;
    for (uint32_t dt = 1000; dt <= 5000; dt += 1000)
        if (attach_beacon_decide(&st, true, 5000 + dt)) beacons++;
    assert(beacons == 1);
}

/* --- 3. stage -> emit -> re-stage across the pending flag ------------------- *
 * Mirrors the device: decide() raises pending, flush_beacon_poll() lowers it on
 * send. A later genuine attach can stage again once the floor has passed. */
static void test_pending_lifecycle(void) {
    AttachBeaconState st;
    memset(&st, 0, sizeof(st));

    assert(attach_beacon_decide(&st, true, 1000) == true);
    assert(st.pending == true);

    st.pending = false;   /* flush_beacon_poll() sent it */

    /* Next attach well past the floor re-stages. */
    uint32_t t = 1000 + ATTACH_BEACON_FLOOR_MS + 1;
    assert(attach_beacon_decide(&st, true, t) == true);
    assert(st.pending == true);
}

/* --- 4. the HTTP endpoint: {gw}/flush, trailing slashes tolerated ---------- */

static void test_endpoint(void) {
    char url[128];
    assert(attach_beacon_endpoint("http://10.0.0.1:8080", url, sizeof(url)) == true);
    assert(strcmp(url, "http://10.0.0.1:8080/flush") == 0);

    /* trailing slashes are stripped so we never emit a "//flush" */
    assert(attach_beacon_endpoint("http://gw///", url, sizeof(url)) == true);
    assert(strcmp(url, "http://gw/flush") == 0);

    /* empty / all-slash / oversize -> refuse (out cleared) */
    assert(attach_beacon_endpoint("", url, sizeof(url)) == false);
    assert(url[0] == '\0');
    assert(attach_beacon_endpoint("///", url, sizeof(url)) == false);
    assert(attach_beacon_endpoint(NULL, url, sizeof(url)) == false);
    char tiny[8];
    assert(attach_beacon_endpoint("http://gw", tiny, sizeof(tiny)) == false);
}

/* --- 5. the JSON body: {"id":"<id>"} --------------------------------------- */

static void test_body(void) {
    char body[ATTACH_BEACON_BODY_CAP];
    assert(attach_beacon_body("seed-a1b2c3", body, sizeof(body)) == true);
    assert(strcmp(body, "{\"id\":\"seed-a1b2c3\"}") == 0);

    /* empty id / a id that would break the JSON -> refuse */
    assert(attach_beacon_body("", body, sizeof(body)) == false);
    assert(body[0] == '\0');
    assert(attach_beacon_body(NULL, body, sizeof(body)) == false);
    assert(attach_beacon_body("bad\"id", body, sizeof(body)) == false);
    assert(attach_beacon_body("bad\\id", body, sizeof(body)) == false);
    char tiny[8];
    assert(attach_beacon_body("seed-a1b2c3", tiny, sizeof(tiny)) == false);
}

/* --- 6. the mesh here/flush frame H1|id (defined for the gateway wire) ------ */

static void test_mesh_frame(void) {
    char f[ATTACH_BEACON_MESH_CAP];
    size_t n = attach_beacon_mesh_encode("seed-a1b2c3", f, sizeof(f));
    assert(n == strlen("H1|seed-a1b2c3"));
    assert(strcmp(f, "H1|seed-a1b2c3") == 0);

    /* empty / a id with '|' (unparsable) / too-small buffer -> 0, out cleared */
    assert(attach_beacon_mesh_encode("", f, sizeof(f)) == 0);
    assert(f[0] == '\0');
    assert(attach_beacon_mesh_encode("bad|id", f, sizeof(f)) == 0);
    char tiny[4];
    assert(attach_beacon_mesh_encode("seed-a1b2c3", tiny, sizeof(tiny)) == 0);
}

int main(void) {
    test_attach_stages();
    test_steady_state_nothing();
    test_rate_limit_floor();
    test_flap_suppressed();
    test_pending_lifecycle();
    test_endpoint();
    test_body();
    test_mesh_frame();
    printf("attach beacon tests: OK\n");
    return 0;
}
