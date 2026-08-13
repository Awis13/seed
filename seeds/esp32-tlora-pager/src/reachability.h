#pragma once
/*
 * reachability.h — cached "can I actually reach home right now?" signal
 * (pure, host-testable). Foundation for HERMES-LADDER C1.
 *
 * WHY THIS EXISTS. The agent-send ladder (C2, later) has to choose a transport
 * for an outbound card. Today the only cheap WiFi signal is the driver link
 * state (WL_CONNECTED) — but a link that is UP is not the same as a route that
 * WORKS. A captive-portal or no-internet WiFi answers the association instantly
 * yet swallows the actual GET, so a send that trusts link-up stalls the full
 * connect window (~4 s) before falling back. What the ladder actually needs is
 * "is the HOME GATEWAY reachable over WiFi", proven by a real request, and it
 * needs that answer ALREADY CACHED so the send path never blocks to find out.
 *
 * A bounded route-home probe already exists on the device: the manual net-status
 * screen does GET {mesh_gw_url}/ping with a 1 s connect cap and treats ANY HTTP
 * status (even 401/403) as "route to the gateway proven". But it runs only when
 * a human opens that screen. This header adds the missing half: the CADENCE and
 * STALENESS policy that lets a background loop keep that verdict fresh, plus the
 * derived tri-state the ladder consumes.
 *
 * WHAT IS PURE HERE, AND WHY. Two things are decisions, not I/O: WHEN to spend a
 * probe, and HOW STALE a past result may be before it stops counting. Both are
 * time math that a screenshot cannot confirm and that is easy to get subtly
 * wrong (off-by-one at the staleness edge, forgetting to probe faster while
 * down). So they live in pure functions over a tiny state, proven by value in
 * tools/test_reachability.cpp. The device half (main.cpp) owns only the socket:
 * it asks reach_should_probe() whether to act, does the bounded GET, and feeds
 * the outcome back through reach_record(). Same functions, host and target.
 *
 * TWO-SPEED CADENCE. When the last verdict is UP we re-probe lazily
 * (REACH_UP_INTERVAL_MS) — the route is known good, spend little. When it is
 * DOWN or UNKNOWN we re-probe eagerly (REACH_DOWN_INTERVAL_MS) so recovery is
 * noticed fast and the ladder is not stuck avoiding WiFi longer than reality
 * warrants. The interval is chosen from the CURRENT derived status (which folds
 * in staleness), so a verdict that has gone stale is re-probed at the fast rate.
 *
 * STALENESS. A cached UP is only trusted for REACH_STALE_MS after the probe that
 * set it; past that the status decays to UNKNOWN rather than lying "up" from an
 * ancient success. The window is comfortably wider than the up-cadence so a
 * healthy device that keeps probing on time never flickers to UNKNOWN between
 * probes.
 */

#include <stdbool.h>
#include <stdint.h>

// ---- Tunables (pinned by tools/test_reachability.cpp) -----------------------
// Re-probe cadence while the route is known good.
#define REACH_UP_INTERVAL_MS 10000UL
// Re-probe cadence while the route is down or unknown (recover fast).
#define REACH_DOWN_INTERVAL_MS 3000UL
// How long a probe result is trusted before the status decays to UNKNOWN. Must
// stay wider than REACH_UP_INTERVAL_MS so on-time probing never flickers.
#define REACH_STALE_MS 30000UL

// Derived tri-state the send ladder (C2) consumes.
enum ReachStatus {
    REACH_UNKNOWN = 0,  // never probed, or the last result has gone stale
    REACH_UP,           // route to the home gateway proven, still fresh
    REACH_DOWN          // last probe failed (or WiFi link is down), still fresh
};

// Persisted cache. Zero-initialise before first use (have_result=false → the
// status starts UNKNOWN and the first tick probes immediately).
struct ReachState {
    uint32_t last_probe_ms;  // millis() of the last recorded probe
    bool     last_up;        // outcome of that probe (true = route proven)
    bool     have_result;    // false until the first probe is recorded
};

// Record a probe outcome. The only mutator; the device half calls it after each
// bounded GET (up = any HTTP status) and after a no-network shortcut (up=false
// when the WiFi link is down — no request needed to know home is unreachable).
static inline void reach_record(ReachState *st, uint32_t now, bool up) {
    st->last_probe_ms = now;
    st->last_up = up;
    st->have_result = true;
}

// Derive the current status, folding in staleness. UNKNOWN if never probed or if
// the last result is older than stale_ms; otherwise UP/DOWN per that result.
static inline ReachStatus reach_status(const ReachState *st, uint32_t now,
                                       uint32_t stale_ms) {
    if (!st->have_result) return REACH_UNKNOWN;
    if ((uint32_t)(now - st->last_probe_ms) >= stale_ms) return REACH_UNKNOWN;
    return st->last_up ? REACH_UP : REACH_DOWN;
}

// Decide whether to spend a probe now. Probe if never probed, or if the cadence
// for the CURRENT status has elapsed. Two-speed: the interval is up_interval_ms
// only while the status is UP; UNKNOWN (incl. a stale UP) and DOWN use the
// faster down_interval_ms. Pure read of *st; call reach_record() with the
// outcome after acting.
static inline bool reach_should_probe(const ReachState *st, uint32_t now,
                                      uint32_t up_interval_ms,
                                      uint32_t down_interval_ms,
                                      uint32_t stale_ms) {
    if (!st->have_result) return true;
    ReachStatus s = reach_status(st, now, stale_ms);
    uint32_t interval = (s == REACH_UP) ? up_interval_ms : down_interval_ms;
    return (uint32_t)(now - st->last_probe_ms) >= interval;
}
