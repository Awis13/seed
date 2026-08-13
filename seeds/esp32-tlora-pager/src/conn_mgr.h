#pragma once
//
// Pure, host-testable decision logic for the connection COORDINATOR.
//
// This device carries four independent connection layers, each of which already
// self-drives and self-recovers on its own timers:
//
//   - WiFi   : background retry ladder 30 s -> 20 min (main.cpp), driver
//              auto-reconnect deliberately off.
//   - WG      : skill_wg_poll() (wg.cpp) — retry start every 15 s, HTTP health
//              probe, 3 fails -> stop + auto-restart.
//   - RNS TCP : RnsTcpInterface::loop() (rns.cpp) — connect state machine with
//              exponential backoff 3 s -> 60 s, staleness watchdogs.
//   - mesh    : skill_meshcore_poll() (meshcore.cpp) — sparse keepalive probe.
//
// Those layers work. This coordinator does NOT replace them and does NOT own
// any socket. It sits on top and adds only the three things no single transport
// can do for itself, because they are cross-cutting:
//
//   1. A shared network-attach / network-lost event. Today every transport
//      polls WiFi.status() on its own; nobody owns the WiFi false->true edge,
//      so bring-up is uncoordinated.
//   2. Ordered bring-up on attach: the WG handshake needs the wall clock set
//      (TAI64N), and RNS TCP rides the WG tunnel — so the correct order is
//      clock -> WG -> RNS. Starting WG before NTP, or hammering RNS before the
//      tunnel is up, only burns failed attempts.
//   3. Damping. RNS tears its link down on any wg_is_up() transition, so a
//      flapping WG bounces RNS. A settle window before propagating a WG-down
//      into a restart keeps a brief flap from cascading.
//
// Plus a per-transport BACKUP watchdog: if a layer is wanted but stays down far
// longer than its own recovery cadence, nudge its existing recover entrypoint
// once. These thresholds are deliberately LONGER than each transport's own
// timers, so the coordinator only acts when a layer is genuinely wedged — it
// backs the transports up, it does not race them.
//
// EVERYTHING here is a pure function over an observation snapshot and a small
// persisted state; there is no Arduino, no socket, no clock read. The device
// half (in main.cpp) fills the snapshot from the real accessors and dispatches
// the returned actions to the real recover paths. That keeps the whole policy
// unit-testable on the host and keeps the happy path a strict no-op: when
// everything wanted is up, conn_mgr_step() returns all-false and the device
// half writes nothing.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ---- Tunables ---------------------------------------------------------------
// Cadence at which the device half calls conn_mgr_step(). Only documents intent;
// the math below uses whatever now_ms it is handed.
#define CONN_MGR_TICK_MS 1000UL

// WG flap debounce: once the clock is valid and WG is wanted but down, wait this
// long before the coordinator issues a restart. wg.cpp already retries its own
// start every 15 s, so this is set past that: the coordinator only steps in if
// WG's own retry has not brought it back. A WG that flaps up within the window
// resets the timer and never triggers a coordinator restart.
#define CONN_MGR_WG_SETTLE_MS 20000UL

// Backup watchdog thresholds (staleness grace, then fast re-nudge cadence while
// still down). Each is longer than the transport's own recovery loop so the
// coordinator is a backstop, not a competitor.
#define CONN_MGR_RNS_STALE_MS 90000UL    // > RNS 60 s max backoff
#define CONN_MGR_RNS_FAST_MS 90000UL
#define CONN_MGR_MESH_STALE_MS 300000UL  // mesh keepalive is sparse by design
#define CONN_MGR_MESH_FAST_MS 300000UL
#define CONN_MGR_WIFI_STALE_MS 1800000UL // > WiFi ladder 20 min top rung
#define CONN_MGR_WIFI_FAST_MS 1200000UL

// Bring-up phase: strictly clock -> WG -> RNS.
enum {
    CONN_MGR_PHASE_DOWN = 0,   // WiFi not attached
    CONN_MGR_PHASE_WAIT_CLOCK, // WiFi up, waiting for a valid wall clock
    CONN_MGR_PHASE_WAIT_WG,    // clock valid, WG asked to start, waiting for it
    CONN_MGR_PHASE_UP          // bring-up complete; RNS may proceed
};

// Persisted coordinator state. Zero-initialise before the first step().
struct ConnMgrState {
    bool     wifi_was_up;   // WiFi.status() from the previous step (edge detect)
    uint8_t  phase;         // CONN_MGR_PHASE_*
    uint32_t wg_bad_since;  // millis WG first seen down (0 = not armed) — debounce
    uint32_t wifi_next;     // watchdog next-nudge deadline (0 = unarmed/parked)
    uint32_t rns_next;
    uint32_t mesh_next;
};

// Observation snapshot for one step. The device half fills this from the real
// accessors (WiFi.status(), time(NULL), wg_is_up(), g_rns_cs, g_mesh, ...).
struct ConnMgrInputs {
    uint32_t now_ms;         // millis()
    bool     wifi_wanted;    // have credentials and the user has not turned WiFi off
    bool     wifi_connected; // WL_CONNECTED
    bool     time_valid;     // wall clock past the sanity epoch (NTP has synced)
    bool     wg_wanted;      // WG enabled and configured
    bool     wg_up;          // wg_is_up()
    bool     rns_wanted;     // RNS enabled and a usable host is configured
    bool     rns_up;         // RNS TCP link connected
    bool     mesh_wanted;    // mesh has an identity, radio ready, peer known
    bool     mesh_up;        // mesh link not in the DOWN state
};

// Actions the device half must apply. Every field defaults false: on the happy
// path (all wanted layers up, steady state) none are set and the device writes
// nothing. Each field maps to nudging an EXISTING recover path, never to owning
// the transport.
struct ConnMgrActions {
    bool on_attach;    // WiFi just came up (false->true edge)
    bool on_lost;      // WiFi just went down (true->false edge)
    bool nudge_wifi;   // wedge escape: restart the WiFi STA ladder
    bool start_wg;     // clock is valid: bring WG up now (bring-up ordering)
    bool restart_wg;   // WG down past the settle window: restart it (debounced)
    bool nudge_rns;    // rearm the RNS connect retry now
    bool nudge_mesh;   // force a mesh keepalive probe now
    bool rns_gate_open;// informational: true once clock+WG bring-up is complete,
                       // i.e. RNS is allowed to proceed (WG-before-RNS)
};

// Two-speed backup watchdog for one transport.
//   not wanted / up : PARKED — deadline cleared, never nudges (the happy-path
//                     no-op; the transport maintains itself).
//   wanted & down   : arm a staleness grace of stale_ms, then nudge once every
//                     fast_ms it stays down.
// Returns true on the tick a nudge should be emitted. deadline==0 is the
// "unarmed" sentinel; an armed deadline is always > 0 (stale_ms/fast_ms > 0),
// and a millis() wrap that lands exactly on 0 merely re-arms one grace window.
static inline bool conn_mgr_watchdog(uint32_t *deadline, bool wanted, bool up,
                                     uint32_t now, uint32_t stale_ms,
                                     uint32_t fast_ms) {
    if (!wanted || up) {
        *deadline = 0;
        return false;
    }
    if (*deadline == 0) {
        *deadline = now + stale_ms;
        if (*deadline == 0) *deadline = 1;
        return false;
    }
    if ((int32_t)(now - *deadline) >= 0) {
        *deadline = now + fast_ms;
        if (*deadline == 0) *deadline = 1;
        return true;
    }
    return false;
}

// One coordinator step. Pure: mutates only *st, reads only *in, returns actions.
static inline ConnMgrActions conn_mgr_step(ConnMgrState *st,
                                           const ConnMgrInputs *in) {
    ConnMgrActions a;
    memset(&a, 0, sizeof(a));
    uint32_t now = in->now_ms;

    // --- mesh: WiFi-independent, so its watchdog runs on every path ----------
    // The mesh radio does not need WiFi; keep it recovering even fully offline.
    a.nudge_mesh = conn_mgr_watchdog(&st->mesh_next, in->mesh_wanted, in->mesh_up,
                                     now, CONN_MGR_MESH_STALE_MS,
                                     CONN_MGR_MESH_FAST_MS);

    // --- WiFi attach / lost edge ---------------------------------------------
    bool attach = in->wifi_connected && !st->wifi_was_up;
    bool lost = !in->wifi_connected && st->wifi_was_up;
    st->wifi_was_up = in->wifi_connected;
    a.on_attach = attach;
    a.on_lost = lost;

    if (!in->wifi_connected) {
        // Offline: quiesce the WiFi-dependent layers so a later attach starts
        // their bring-up and grace windows fresh, and run only the WiFi wedge
        // watchdog. WG/RNS self-quiesce on WiFi loss; nothing to nudge here.
        st->phase = CONN_MGR_PHASE_DOWN;
        st->wg_bad_since = 0;
        st->rns_next = 0;
        a.rns_gate_open = false;
        a.nudge_wifi = conn_mgr_watchdog(&st->wifi_next, in->wifi_wanted, false,
                                         now, CONN_MGR_WIFI_STALE_MS,
                                         CONN_MGR_WIFI_FAST_MS);
        return a;
    }

    // WiFi is up: its wedge watchdog parks.
    st->wifi_next = 0;

    if (attach) {
        st->phase = CONN_MGR_PHASE_WAIT_CLOCK;
        st->wg_bad_since = 0;
    }

    // --- sequenced bring-up: clock -> WG -> RNS ------------------------------
    switch (st->phase) {
    case CONN_MGR_PHASE_WAIT_CLOCK:
        // Clock gate: never touch WG before NTP has set the wall clock, or the
        // WG handshake (TAI64N) fails and wastes a start.
        if (in->time_valid) {
            if (in->wg_wanted) {
                a.start_wg = true;
                st->phase = CONN_MGR_PHASE_WAIT_WG;
            } else {
                st->phase = CONN_MGR_PHASE_UP;
            }
        }
        break;
    case CONN_MGR_PHASE_WAIT_WG:
        // Hold RNS until WG is up (or no longer wanted): WG-before-RNS.
        if (!in->wg_wanted || in->wg_up) st->phase = CONN_MGR_PHASE_UP;
        break;
    case CONN_MGR_PHASE_UP:
    default:
        break;
    }
    a.rns_gate_open = (st->phase == CONN_MGR_PHASE_UP);

    // --- WG flap debounce ----------------------------------------------------
    // Once the clock is valid and WG is wanted but down, wait a settle window
    // before restarting. A flap that recovers within the window resets the
    // timer and emits nothing, so RNS is not bounced repeatedly.
    bool wg_should = in->time_valid && in->wg_wanted;
    if (wg_should && !in->wg_up) {
        if (st->wg_bad_since == 0) {
            st->wg_bad_since = now ? now : 1;
        } else if ((int32_t)(now - st->wg_bad_since) >= (int32_t)CONN_MGR_WG_SETTLE_MS) {
            a.restart_wg = true;
            // Re-arm the window: at most one restart per settle period.
            st->wg_bad_since = now ? now : 1;
        }
    } else {
        st->wg_bad_since = 0;
    }

    // --- RNS backup watchdog, gated on WG-before-RNS -------------------------
    // Only nudge RNS once bring-up is complete (rns_gate_open). This is the
    // ordering guarantee: no RNS nudge is emitted before WG is up.
    a.nudge_rns = conn_mgr_watchdog(&st->rns_next,
                                    in->rns_wanted && a.rns_gate_open, in->rns_up,
                                    now, CONN_MGR_RNS_STALE_MS,
                                    CONN_MGR_RNS_FAST_MS);
    return a;
}
