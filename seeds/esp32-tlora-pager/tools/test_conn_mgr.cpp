/*
 * Host tests for the connection coordinator in src/conn_mgr.h — the pure policy
 * the device half (conn_mgr_service() in main.cpp) drives. These exercise the
 * SAME functions the firmware calls (conn_mgr_step, conn_mgr_watchdog), with the
 * real transport accessors replaced by a plain input snapshot, so the decision
 * logic is checked by value rather than re-encoded here.
 *
 * What is pinned:
 *   - happy path is a strict no-op (no action while every wanted layer is up);
 *   - attach bring-up ordering: clock -> WG -> RNS, and RNS is held until WG up;
 *   - clock gate: WG is never started/restarted before the wall clock is valid;
 *   - network-lost quiesce and clean re-attach;
 *   - WG-flap debounce: rapid WG toggles inside the settle window emit no
 *     restart; only a sustained down does, at most once per window;
 *   - two-speed watchdog: a grace before the first nudge, a fast cadence while
 *     down, and PARKED (no nudge) while up.
 *
 * Mutation checks (run by hand: edit src/conn_mgr.h, rebuild, expect RED,
 * restore) that this suite is written to catch:
 *   M1 break WG-before-RNS   : set `a.rns_gate_open = true;` unconditionally
 *                              -> test_bringup_order / test_rns_gated FAIL.
 *   M2 remove the debounce    : replace the settle compare with `!in->wg_up`
 *                              (restart immediately) -> test_wg_flap_debounce FAIL.
 *   M3 flip the two-speed     : make conn_mgr_watchdog return true when `up`
 *                              -> test_watchdog_two_speed / no-op tests FAIL.
 *   M4 drop the clock gate    : emit start_wg in WAIT_CLOCK without time_valid
 *                              -> test_clock_gate FAIL.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/conn_mgr.h"

/* A fully-healthy snapshot: everything wanted and up, clock valid. */
static ConnMgrInputs all_up(uint32_t now) {
    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.wifi_wanted = true;
    in.wifi_connected = true;
    in.time_valid = true;
    in.wg_wanted = true;
    in.wg_up = true;
    in.rns_wanted = true;
    in.rns_up = true;
    in.mesh_wanted = true;
    in.mesh_up = true;
    return in;
}

static bool any_action(const ConnMgrActions &a) {
    return a.on_attach || a.on_lost || a.nudge_wifi || a.start_wg ||
           a.restart_wg || a.nudge_rns || a.nudge_mesh;
}

/* Settle a fresh state into the steady UP phase so later ticks are the true
 * happy path (attach already consumed, bring-up complete). */
static void settle_up(ConnMgrState *st, uint32_t *now) {
    ConnMgrInputs in = all_up(*now);
    conn_mgr_step(st, &in);   /* attach tick */
    *now += CONN_MGR_TICK_MS;
    in = all_up(*now);
    conn_mgr_step(st, &in);   /* WAIT_CLOCK -> (clock ok, wg up) -> ... */
    *now += CONN_MGR_TICK_MS;
    in = all_up(*now);
    conn_mgr_step(st, &in);   /* reach UP */
    *now += CONN_MGR_TICK_MS;
    assert(st->phase == CONN_MGR_PHASE_UP);
}

/* --- 1. happy path is a strict no-op --------------------------------------- */
static void test_happy_path_noop(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 100000;
    settle_up(&st, &now);

    /* Many steady ticks with everything up: not a single action. */
    for (int i = 0; i < 1000; i++) {
        ConnMgrInputs in = all_up(now);
        ConnMgrActions a = conn_mgr_step(&st, &in);
        assert(!any_action(a));
        assert(a.rns_gate_open);   /* gate stays open, but that is not an action */
        now += CONN_MGR_TICK_MS;
    }
    printf("  happy-path no-op: OK\n");
}

/* --- 2. attach bring-up ordering: clock -> WG -> RNS ----------------------- */
static void test_bringup_order(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 50000;

    /* Tick 1: WiFi attaches, clock not yet valid. */
    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.wifi_wanted = true;
    in.wifi_connected = true;
    in.time_valid = false;
    in.wg_wanted = true;
    in.wg_up = false;
    in.rns_wanted = true;
    in.rns_up = false;
    ConnMgrActions a = conn_mgr_step(&st, &in);
    assert(a.on_attach);
    assert(!a.start_wg);        /* clock gate: no WG before the clock */
    assert(!a.restart_wg);
    assert(!a.rns_gate_open);   /* RNS held */
    assert(!a.nudge_rns);
    assert(st.phase == CONN_MGR_PHASE_WAIT_CLOCK);

    /* Tick 2: clock becomes valid -> start WG, still hold RNS. */
    now += CONN_MGR_TICK_MS;
    in.now_ms = now;
    in.time_valid = true;
    a = conn_mgr_step(&st, &in);
    assert(!a.on_attach);
    assert(a.start_wg);         /* WG started exactly once clock is valid */
    assert(!a.rns_gate_open);   /* RNS still held: WG not up yet */
    assert(!a.nudge_rns);
    assert(st.phase == CONN_MGR_PHASE_WAIT_WG);

    /* Tick 3: WG still coming up -> RNS stays held, no second start. */
    now += CONN_MGR_TICK_MS;
    in.now_ms = now;
    a = conn_mgr_step(&st, &in);
    assert(!a.start_wg);
    assert(!a.rns_gate_open);
    assert(!a.nudge_rns);
    assert(st.phase == CONN_MGR_PHASE_WAIT_WG);

    /* Tick 4: WG up -> gate opens, RNS may now proceed. */
    now += CONN_MGR_TICK_MS;
    in.now_ms = now;
    in.wg_up = true;
    a = conn_mgr_step(&st, &in);
    assert(a.rns_gate_open);
    assert(st.phase == CONN_MGR_PHASE_UP);
    printf("  bring-up order clock->WG->RNS: OK\n");
}

/* --- 3. clock gate: no WG start/restart until the clock is valid ----------- */
static void test_clock_gate(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 10000;

    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.wifi_wanted = true;
    in.wifi_connected = true;
    in.wg_wanted = true;
    in.wg_up = false;      /* WG wanted but down the whole time */
    in.time_valid = false; /* clock never valid... */

    /* ...for a long stretch (well past the WG settle window): no WG action. */
    for (uint32_t t = 0; t < CONN_MGR_WG_SETTLE_MS * 4; t += CONN_MGR_TICK_MS) {
        in.now_ms = now + t;
        ConnMgrActions a = conn_mgr_step(&st, &in);
        assert(!a.start_wg);
        assert(!a.restart_wg);
    }
    /* Clock turns valid -> WG is started (bring-up), not before. */
    now += CONN_MGR_WG_SETTLE_MS * 4;
    in.now_ms = now;
    in.time_valid = true;
    ConnMgrActions a = conn_mgr_step(&st, &in);
    assert(a.start_wg);
    printf("  clock gate: OK\n");
}

/* --- 4. network-lost quiesce and clean re-attach --------------------------- */
static void test_lost_quiesce(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 200000;
    settle_up(&st, &now);

    /* WiFi drops. */
    ConnMgrInputs in = all_up(now);
    in.wifi_connected = false;
    in.wg_up = false;
    in.rns_up = false;
    ConnMgrActions a = conn_mgr_step(&st, &in);
    assert(a.on_lost);
    assert(st.phase == CONN_MGR_PHASE_DOWN);
    assert(!a.nudge_rns);       /* dependent layers are not nudged while offline */
    assert(!a.restart_wg);
    assert(st.wg_bad_since == 0);

    /* Stay offline a while: still no WG/RNS churn (WiFi wedge threshold is far
     * out, so no wifi nudge yet either). */
    for (int i = 0; i < 10; i++) {
        now += CONN_MGR_TICK_MS;
        in.now_ms = now;
        a = conn_mgr_step(&st, &in);
        assert(!a.nudge_rns);
        assert(!a.start_wg);
        assert(!a.restart_wg);
        assert(!a.nudge_wifi);
    }

    /* Re-attach: bring-up sequence restarts from the clock gate. */
    now += CONN_MGR_TICK_MS;
    in = all_up(now);
    in.wg_up = false;
    in.rns_up = false;
    in.time_valid = false;      /* clock lost too (worst case) */
    a = conn_mgr_step(&st, &in);
    assert(a.on_attach);
    assert(st.phase == CONN_MGR_PHASE_WAIT_CLOCK);
    assert(!a.start_wg);        /* clock gate re-applies */
    printf("  network-lost quiesce + re-attach: OK\n");
}

/* --- 5. WG-flap debounce --------------------------------------------------- */
static void test_wg_flap_debounce(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 300000;
    settle_up(&st, &now);

    ConnMgrInputs in = all_up(now);

    /* Rapid flapping: WG toggles down/up every few seconds, always recovering
     * inside the settle window. No restart must ever be emitted. */
    bool wg = true;
    for (uint32_t t = 0; t < CONN_MGR_WG_SETTLE_MS * 6; t += 4000) {
        wg = !wg;                       /* toggle every 4 s (< 20 s settle) */
        in.now_ms = now + t;
        in.wg_up = wg;
        in.rns_up = wg;                 /* RNS follows WG in reality */
        ConnMgrActions a = conn_mgr_step(&st, &in);
        assert(!a.restart_wg);          /* debounced: flap never restarts WG */
    }

    /* Now a SUSTAINED down. Restart fires once, only after the settle window,
     * and then not again until another full window has elapsed. */
    uint32_t base = now + CONN_MGR_WG_SETTLE_MS * 6 + CONN_MGR_TICK_MS;
    in.wg_up = false;
    in.rns_up = false;
    int restarts = 0;
    uint32_t first_restart_at = 0;
    for (uint32_t t = 0; t <= CONN_MGR_WG_SETTLE_MS * 2 + CONN_MGR_TICK_MS;
         t += CONN_MGR_TICK_MS) {
        in.now_ms = base + t;
        ConnMgrActions a = conn_mgr_step(&st, &in);
        if (a.restart_wg) {
            if (restarts == 0) first_restart_at = t;
            restarts++;
        }
    }
    /* Exactly two restarts across two settle windows, the first no earlier than
     * one settle window in. */
    assert(first_restart_at >= CONN_MGR_WG_SETTLE_MS);
    assert(restarts == 2);
    printf("  WG-flap debounce: OK\n");
}

/* --- 6. two-speed watchdog (grace, fast-while-down, parked-while-up) ------- */
static void test_watchdog_two_speed(void) {
    /* Drive the shared helper directly with explicit thresholds. */
    const uint32_t STALE = 30000, FAST = 15000;
    uint32_t dl = 0;
    uint32_t now = 1000;

    /* Down: no nudge before the staleness grace elapses. */
    bool n = conn_mgr_watchdog(&dl, /*wanted*/true, /*up*/false, now, STALE, FAST);
    assert(!n);                         /* first call only arms the grace */
    for (uint32_t t = CONN_MGR_TICK_MS; t < STALE; t += CONN_MGR_TICK_MS) {
        n = conn_mgr_watchdog(&dl, true, false, now + t, STALE, FAST);
        assert(!n);                     /* still inside the grace window */
    }
    /* Exactly at the deadline: the first nudge. */
    n = conn_mgr_watchdog(&dl, true, false, now + STALE, STALE, FAST);
    assert(n);

    /* Then a fast cadence: no nudge until FAST more, one exactly at FAST. */
    uint32_t base = now + STALE;
    for (uint32_t t = CONN_MGR_TICK_MS; t < FAST; t += CONN_MGR_TICK_MS) {
        n = conn_mgr_watchdog(&dl, true, false, base + t, STALE, FAST);
        assert(!n);
    }
    n = conn_mgr_watchdog(&dl, true, false, base + FAST, STALE, FAST);
    assert(n);

    /* Up: PARKED. Never nudges, however long, and it disarms the deadline. */
    for (uint32_t t = 0; t < STALE * 4; t += CONN_MGR_TICK_MS) {
        n = conn_mgr_watchdog(&dl, /*wanted*/true, /*up*/true, base + t, STALE, FAST);
        assert(!n);
    }
    assert(dl == 0);                    /* disarmed while up */

    /* Not wanted: also parked. */
    n = conn_mgr_watchdog(&dl, /*wanted*/false, /*up*/false, now, STALE, FAST);
    assert(!n);
    assert(dl == 0);
    printf("  watchdog two-speed: OK\n");
}

/* --- 7. RNS backup watchdog is gated on WG-before-RNS ---------------------- */
static void test_rns_gated(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 400000;

    /* Attach, clock valid, but WG never comes up: RNS wanted + down. */
    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.wifi_wanted = true;
    in.wifi_connected = true;
    in.time_valid = true;
    in.wg_wanted = true;
    in.wg_up = false;
    in.rns_wanted = true;
    in.rns_up = false;

    /* Run well past the RNS staleness threshold. Because WG never comes up the
     * gate stays shut, so RNS must NEVER be nudged. */
    bool nudged = false;
    for (uint32_t t = 0; t < CONN_MGR_RNS_STALE_MS * 3; t += CONN_MGR_TICK_MS) {
        in.now_ms = now + t;
        ConnMgrActions a = conn_mgr_step(&st, &in);
        if (a.nudge_rns) nudged = true;
        assert(!a.rns_gate_open);
    }
    assert(!nudged);

    /* Bring WG up: gate opens, and now a stale RNS eventually gets one nudge. */
    uint32_t base = now + CONN_MGR_RNS_STALE_MS * 3;
    in.wg_up = true;
    nudged = false;
    for (uint32_t t = 0; t <= CONN_MGR_RNS_STALE_MS + CONN_MGR_TICK_MS;
         t += CONN_MGR_TICK_MS) {
        in.now_ms = base + t;
        ConnMgrActions a = conn_mgr_step(&st, &in);
        if (a.nudge_rns) nudged = true;
    }
    assert(nudged);
    printf("  RNS gated on WG-before-RNS: OK\n");
}

/* --- 8. mesh watchdog runs independently of WiFi -------------------------- */
static void test_mesh_independent(void) {
    ConnMgrState st;
    memset(&st, 0, sizeof(st));
    uint32_t now = 500000;

    /* Fully offline WiFi, but mesh is wanted and down: mesh must still recover
     * (its radio does not need WiFi). */
    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.wifi_wanted = true;
    in.wifi_connected = false;
    in.mesh_wanted = true;
    in.mesh_up = false;

    bool nudged = false;
    for (uint32_t t = 0; t <= CONN_MGR_MESH_STALE_MS + CONN_MGR_TICK_MS;
         t += CONN_MGR_TICK_MS) {
        in.now_ms = now + t;
        ConnMgrActions a = conn_mgr_step(&st, &in);
        if (a.nudge_mesh) nudged = true;
    }
    assert(nudged);

    /* Mesh up: parked, no nudge. */
    ConnMgrState st2;
    memset(&st2, 0, sizeof(st2));
    in.mesh_up = true;
    nudged = false;
    for (uint32_t t = 0; t <= CONN_MGR_MESH_STALE_MS * 2; t += CONN_MGR_TICK_MS) {
        in.now_ms = now + t;
        ConnMgrActions a = conn_mgr_step(&st2, &in);
        if (a.nudge_mesh) nudged = true;
    }
    assert(!nudged);
    printf("  mesh watchdog WiFi-independent: OK\n");
}

int main(void) {
    test_happy_path_noop();
    test_bringup_order();
    test_clock_gate();
    test_lost_quiesce();
    test_wg_flap_debounce();
    test_watchdog_two_speed();
    test_rns_gated();
    test_mesh_independent();
    printf("conn mgr tests: OK\n");
    return 0;
}
