/*
 * Host tests for the route-home reachability policy in src/reachability.h — the
 * pure cadence + staleness logic the device half (reachability_service() in
 * main.cpp) drives. These exercise the SAME functions the firmware calls
 * (reach_should_probe, reach_status, reach_record), so the decision logic is
 * checked by value rather than re-encoded here.
 *
 * What is pinned:
 *   - never probed  : status UNKNOWN, and should_probe fires immediately;
 *   - fresh UP/DOWN : status derives from the recorded outcome;
 *   - staleness      : a UP older than the window decays to UNKNOWN, with the
 *                      boundary pinned at exactly REACH_STALE_MS;
 *   - cadence        : no probe inside the interval, a probe once it elapses,
 *                      boundary pinned at exactly the interval;
 *   - two-speed      : a DOWN/UNKNOWN status re-probes at the FAST cadence, in
 *                      the gap where the slow UP cadence would still be waiting.
 *
 * The actual pinned windows (10s up / 3s down / 30s stale) are asserted against
 * the header constants so a silent retune trips the suite.
 *
 * Mutation checks (edit src/reachability.h, rebuild, expect RED, restore):
 *   M1 drop the stale decay : in reach_status, delete the stale_ms check (always
 *      return UP/DOWN once have_result) -> test_status_stale + test_two_speed
 *      (stale-UP branch) FAIL.
 *   M2 flip the two-speed   : in reach_should_probe, always use up_interval_ms
 *      (ignore status) -> test_two_speed FAIL.
 *   M3 break the cadence    : use `>` instead of `>=` at the interval boundary
 *      -> test_cadence_boundary FAIL.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/reachability.h"

static int checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// Convenience wrappers that always pass the pinned windows, exactly as the
// device half does.
static bool should(const ReachState *st, uint32_t now) {
    return reach_should_probe(st, now, REACH_UP_INTERVAL_MS,
                              REACH_DOWN_INTERVAL_MS, REACH_STALE_MS);
}
static ReachStatus status(const ReachState *st, uint32_t now) {
    return reach_status(st, now, REACH_STALE_MS);
}

static int test_pinned_windows() {
    // If these ever change, the cadence/staleness assertions below must be
    // re-reasoned; pin the values so a retune is a conscious edit.
    CHECK(REACH_UP_INTERVAL_MS == 10000UL);
    CHECK(REACH_DOWN_INTERVAL_MS == 3000UL);
    CHECK(REACH_STALE_MS == 30000UL);
    // Two-speed only means anything if down is strictly faster than up, and the
    // stale window must outlast the up cadence or a healthy device flickers.
    CHECK(REACH_DOWN_INTERVAL_MS < REACH_UP_INTERVAL_MS);
    CHECK(REACH_STALE_MS > REACH_UP_INTERVAL_MS);
    return 0;
}

static int test_never_probed() {
    ReachState st;
    memset(&st, 0, sizeof(st));
    CHECK(status(&st, 0) == REACH_UNKNOWN);
    CHECK(status(&st, 999999) == REACH_UNKNOWN);
    CHECK(should(&st, 0) == true);       // first tick must probe
    CHECK(should(&st, 50000) == true);
    return 0;
}

static int test_fresh_results() {
    ReachState st;
    memset(&st, 0, sizeof(st));

    reach_record(&st, 1000, true);
    CHECK(status(&st, 1000) == REACH_UP);
    CHECK(status(&st, 1000 + 5000) == REACH_UP);   // still inside stale window

    reach_record(&st, 2000, false);
    CHECK(status(&st, 2000) == REACH_DOWN);
    CHECK(status(&st, 2000 + 5000) == REACH_DOWN);
    return 0;
}

static int test_status_stale() {
    ReachState st;
    memset(&st, 0, sizeof(st));
    reach_record(&st, 1000, true);

    // One ms before the window closes: still UP. At the window: UNKNOWN.
    CHECK(status(&st, 1000 + REACH_STALE_MS - 1) == REACH_UP);
    CHECK(status(&st, 1000 + REACH_STALE_MS) == REACH_UNKNOWN);
    CHECK(status(&st, 1000 + REACH_STALE_MS + 1) == REACH_UNKNOWN);

    // A stale DOWN likewise decays to UNKNOWN (both branches, not just UP).
    reach_record(&st, 1000, false);
    CHECK(status(&st, 1000 + REACH_STALE_MS) == REACH_UNKNOWN);
    return 0;
}

static int test_cadence_up() {
    ReachState st;
    memset(&st, 0, sizeof(st));
    reach_record(&st, 1000, true);   // fresh UP -> slow cadence

    CHECK(should(&st, 1000 + 3000) == false);    // down cadence must NOT apply
    CHECK(should(&st, 1000 + 9999) == false);
    CHECK(should(&st, 1000 + 10000) == true);    // up interval elapsed
    return 0;
}

static int test_cadence_boundary() {
    ReachState st;
    memset(&st, 0, sizeof(st));

    // DOWN -> fast cadence: boundary at exactly REACH_DOWN_INTERVAL_MS.
    reach_record(&st, 1000, false);
    CHECK(should(&st, 1000 + REACH_DOWN_INTERVAL_MS - 1) == false);
    CHECK(should(&st, 1000 + REACH_DOWN_INTERVAL_MS) == true);

    // UP -> slow cadence: boundary at exactly REACH_UP_INTERVAL_MS.
    reach_record(&st, 1000, true);
    CHECK(should(&st, 1000 + REACH_UP_INTERVAL_MS - 1) == false);
    CHECK(should(&st, 1000 + REACH_UP_INTERVAL_MS) == true);
    return 0;
}

static int test_two_speed() {
    // The gap that proves two-speed: between the down interval (3s) and the up
    // interval (10s). A DOWN status must re-probe there; an UP status must not.
    const uint32_t gap = 5000;   // 3000 < gap < 10000
    CHECK(REACH_DOWN_INTERVAL_MS < gap && gap < REACH_UP_INTERVAL_MS);

    ReachState down;
    memset(&down, 0, sizeof(down));
    reach_record(&down, 1000, false);
    CHECK(should(&down, 1000 + gap) == true);    // DOWN: fast cadence fires

    ReachState up;
    memset(&up, 0, sizeof(up));
    reach_record(&up, 1000, true);
    CHECK(should(&up, 1000 + gap) == false);      // UP: slow cadence still waits

    // A STALE UP is UNKNOWN, so it must also take the FAST cadence. Probe at
    // stale + a hair, where the up cadence (measured from last_probe) elapsed
    // long ago anyway — so make the point with a state whose up cadence has NOT
    // elapsed but which is stale... impossible (stale=30s > up=10s). Instead
    // pin: a stale UP re-probes (status is UNKNOWN, not UP).
    ReachState staleup;
    memset(&staleup, 0, sizeof(staleup));
    reach_record(&staleup, 1000, true);
    CHECK(status(&staleup, 1000 + REACH_STALE_MS) == REACH_UNKNOWN);
    CHECK(should(&staleup, 1000 + REACH_STALE_MS) == true);
    return 0;
}

int main() {
    if (test_pinned_windows()) return 1;
    if (test_never_probed()) return 1;
    if (test_fresh_results()) return 1;
    if (test_status_stale()) return 1;
    if (test_cadence_up()) return 1;
    if (test_cadence_boundary()) return 1;
    if (test_two_speed()) return 1;
    printf("reachability: %d checks passed\n", checks);
    return 0;
}
