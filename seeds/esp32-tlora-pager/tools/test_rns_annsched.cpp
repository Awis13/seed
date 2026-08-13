/*
 * Host tests for the announce schedule.
 *
 * rns_announce_due() decides when the loop task pays for a software Ed25519
 * signature, and it is the one part of the announce path that can be exercised
 * without a stack, a socket or a board. A mistake is expensive in both
 * directions: announce too often and the drain budget is spent on signatures,
 * announce too rarely (or never, or into an offline interface) and the node is
 * unaddressable while looking healthy. So the whole state space is pinned here.
 *
 * TWO KINDS OF TEST LIVE IN THIS FILE, and the second kind exists because the
 * first kind was not enough. Sections 1-7 pin the decision function one call at
 * a time. Section 8 drives BOTH header functions through a replica of the
 * caller's tick loop over a simulated timeline, because the defect that got
 * past the first kind was not in either function: it was in the seam between
 * the decision and the state machine feeding it, where a refused reconnect was
 * consumed instead of deferred. A per-call test cannot see that, and no mutant
 * of a correct header reproduces it.
 *
 * The constants come from rns/annsched.h, which is a pure header with no
 * Arduino dependency, so this file needs neither the library nor the framework.
 */

#include <assert.h>
#include <stdint.h>

#include "../src/rns/annsched.h"

#define BOOT  (uint32_t)RNS_ANNOUNCE_BOOT_DELAY_MS
#define IVAL  (uint32_t)RNS_ANNOUNCE_INTERVAL_MS
#define RECON (uint32_t)RNS_ANNOUNCE_RECONNECT_MIN_MS

/* Every assertion goes through this wrapper, which calls the function TWICE and
 * insists the two answers agree. The caller evaluates it once per tick and acts
 * on the answer; a decision that depended on hidden state would fire an extra
 * signature on the tick after the one it was asked about.
 *
 * The fifth argument is EDGE_PENDING — "a reconnect is owed an announce" — and
 * not the raw was_online sample it used to be. See rns/annsched.h. */
static bool due(uint32_t now, uint32_t last, bool announced, bool online,
                bool edge_pending, uint32_t online_since) {
    bool first = rns_announce_due(now, last, announced, online, edge_pending,
                                  online_since);
    bool second = rns_announce_due(now, last, announced, online, edge_pending,
                                   online_since);
    assert(first == second);
    return first;
}

/* ---- the caller, in miniature ----
 *
 * A replica of skills/rns.cpp's rns_announce_poll(), assembled from the
 * header's OWN two functions so that the only thing modelled here is the
 * caller's ORDERING: stamp online_since on the edge (RnsTcpInterface::loop()
 * does this via g_rns_up_ms in the same tick), latch, decide, and on a decision
 * to announce stamp the clock and pay off the latch. Every field maps to one
 * file-scope variable in rns.cpp and is named for it.
 *
 * This is the harness that catches seam defects. It is deliberately small
 * enough to check by eye against rns_announce_poll(), and
 * tools/test_task_unblock.py pins the correspondence in the other direction. */
typedef struct {
    uint32_t last;    /* g_rns_ann_last_ms */
    uint32_t since;   /* g_rns_up_ms */
    bool announced;   /* g_rns_announced */
    bool was_online;  /* g_rns_ann_was_online */
    bool pending;     /* g_rns_ann_edge_pending */
} sched;

static bool step(sched *s, uint32_t now, bool online) {
    if (online && !s->was_online) s->since = now;
    s->pending = rns_announce_edge_latch(s->pending, online, s->was_online);
    bool fire = rns_announce_due(now, s->last, s->announced, online, s->pending,
                                 s->since);
    if (fire) {
        s->last = now;
        s->announced = true;
        s->pending = false;   /* the ONLY thing that clears the debt */
    }
    s->was_online = online;
    return fire;
}

/* The real skill tick. RNS_TICK_MS in skills/rns.cpp. */
#define TICK 20UL
#define HORIZON 4000000UL   /* > BOOT + IVAL, so a dropped edge is observable */

/* Run a timeline with exactly one outage and return the millis() stamp of the
 * nth announce, or 0 if it never arrives inside the horizon. */
static uint32_t announce_at(unsigned nth, uint32_t down_at, uint32_t up_at) {
    sched s = {0, 0, false, false, false};
    unsigned seen = 0;
    for (uint32_t t = 0; t <= HORIZON; t += TICK) {
        bool online = !(t >= down_at && t < up_at);
        if (step(&s, t, online) && ++seen == nth) return t;
    }
    return 0;
}

int main() {
    /* 1. OFFLINE IS ABSOLUTE. No combination of clock or history may announce
     * into an interface that is down: the send fails, Transport logs an ERROR
     * and Log.cpp flushes the UART synchronously on the loop task. This is the
     * first statement of the function and the loop below is the reason it is. */
    assert(!due(0, 0, false, false, false, 0));
    assert(!due(BOOT, 0, false, false, false, 0));
    assert(!due(BOOT * 100, 0, false, false, false, 0));
    assert(!due(IVAL * 10, 0, true, false, false, 0));
    /* Including — especially — an interface that is down while an announce is
     * already OWED. The debt survives the outage; paying it does not. */
    assert(!due(IVAL * 10, 0, true, false, true, 0));
    assert(!due(1000, 1000, true, false, true, 500));

    /* 2. THE BOOT DELAY. Nothing announces in the first
     * RNS_ANNOUNCE_BOOT_DELAY_MS after the link comes up — a TCP socket that
     * has just connected has not necessarily been accepted by the peer's
     * Reticulum yet. The boundary is inclusive: at exactly BOOT it is due. */
    assert(!due(0, 0, false, true, false, 0));
    assert(!due(1, 0, false, true, false, 0));
    assert(!due(BOOT - 1, 0, false, true, false, 0));
    assert(due(BOOT, 0, false, true, false, 0));
    assert(due(BOOT + 1, 0, false, true, false, 0));
    /* ...measured from when the link came up, not from boot. */
    assert(!due(100000 + BOOT - 1, 0, false, true, false, 100000));
    assert(due(100000 + BOOT, 0, false, true, false, 100000));
    /* Still true once the link has been up a while and we somehow never
     * announced: the answer is "yes, now", not "the moment has passed". */
    assert(due(100000 + IVAL, 0, false, true, false, 100000));
    /* And the pending latch does not reach this branch either way: before the
     * first announce there is no last_announce for a floor to measure from. */
    assert(!due(BOOT - 1, 0, false, true, true, 0));
    assert(due(BOOT, 0, false, true, true, 0));

    /* 3. THE FIRST ANNOUNCE IS NOT SPECIAL ABOUT THE TRANSITION. Before we have
     * announced at all, a pending reconnect still serves the boot delay rather
     * than firing immediately — otherwise every flap before the first announce
     * would cost a signature. */
    assert(!due(200, 0, false, true, true, 100));
    assert(due(100 + BOOT, 0, false, true, true, 100));

    /* 4. THE STEADY-STATE INTERVAL. Having announced, with no reconnect owed, a
     * link that stays up re-announces once per RNS_ANNOUNCE_INTERVAL_MS. */
    assert(!due(BOOT + 1, BOOT, true, true, false, 0));
    assert(!due(BOOT + IVAL - 1, BOOT, true, true, false, 0));
    assert(due(BOOT + IVAL, BOOT, true, true, false, 0));
    assert(due(BOOT + IVAL * 3, BOOT, true, true, false, 0));
    /* It is measured from the last announce, not from the last online edge:
     * a link up for hours with a recent announce is not due. */
    assert(!due(10000000UL, 10000000UL - 60000UL, true, true, false, 0));

    /* 5. THE RECONNECT. A pending offline->online transition fires well ahead
     * of the interval — a peer that lost our socket also lost the path to us,
     * and 30 min of being unaddressable for no reason is the point of this
     * branch — but NOT unconditionally, and this section is written the way it
     * is because an earlier revision of this file asserted exactly that and
     * pinned a real defect in place.
     *
     * The defect: the reconnect ladder in skills/rns.cpp resets
     * g_rns_backoff_ms to RNS_TCP_BACKOFF_MIN_MS (3 s) on every SUCCESSFUL
     * connect, so it never climbs across a flap. A peer that accepts the socket
     * and drops it, or a WiFi/WireGuard netif going up and down, offers an
     * offline->online edge roughly every 3 seconds forever. An unconditional
     * `return true` here turns each one into a software Ed25519 signature on
     * the loop task — more signatures per minute on a broken link than a
     * healthy one pays per hour. */
    {
        /* 5a. A genuine outage re-announces. In normal operation the last
         * announce is minutes to half an hour old, so the floor is long past
         * and the edge fires on the tick the link comes back. */
        assert(due(RECON, 0, true, true, true, RECON));
        assert(due(600000UL, 0, true, true, true, 600000UL));
        assert(due(IVAL, IVAL - RECON, true, true, true, IVAL));

        /* 5b. A FLAP DOES NOT. A reconnect whose last announce is younger than
         * the floor is refused. These are the exact assertions the previous
         * revision had inverted. */
        assert(!due(BOOT + 1, BOOT, true, true, true, BOOT + 1));
        assert(!due(1000000UL, 999999UL, true, true, true, 1000000UL));
        assert(!due(RECON - 1, 0, true, true, true, RECON - 1));

        /* 5c. The floor is measured from the last ANNOUNCE, not from the edge,
         * which is what makes a burst of edges cost exactly one announce. */
        assert(due(RECON, 0, true, true, true, RECON - 1));
        assert(due(RECON, 0, true, true, true, 0));

        /* 5d. Paying the debt returns us to the interval: with the latch
         * cleared, the tick after an announce is an ordinary interval tick. */
        assert(!due(1000001UL, 1000000UL, true, true, false, 1000000UL));
    }

    /* 6. MILLIS() WRAPAROUND — the reason every comparison in the header is an
     * unsigned subtraction rather than `now >= last + interval`. millis()
     * rolls over every ~49.7 days and a device that stays up that long must
     * neither stop announcing (stuck) nor announce every tick (spurious).
     *
     * 6a. Boot delay straddling the wrap: the link came up just before 2^32 and
     * `now` has already wrapped past zero.
     *
     * WHERE THE NAIVE FORM ACTUALLY DIVERGES, because an earlier version of
     * this comment got the mechanism wrong and the assertions that followed it
     * were therefore worthless. `now >= (uint32_t)(online_since + BOOT)` does
     * compute a deadline that wraps to a small number (here 4744) — but on the
     * FAR side of the rollover, where `now` is also small, comparing against
     * that small deadline gives the RIGHT answer for every value below. All
     * four assertions in the original block lived there, so the naive form
     * passed every one of them.
     *
     * The divergence is on the NEAR side: for `now` still up near 2^32 — the
     * entire stretch from online_since to the rollover, i.e. the first 256 ms
     * of this link — `now` is enormous and the wrapped deadline is tiny, so the
     * naive comparison is true throughout and reports the boot delay satisfied
     * before any of it has elapsed. The extreme case is the tick the link comes
     * up, with zero milliseconds elapsed. That is the assertion that kills the
     * mutant, and it is the first one below. */
    {
        const uint32_t before_wrap = 0xFFFFFF00UL;   /* 256 ms before the wrap */
        /* Zero elapsed. Correct: not due. Naive: 0xFFFFFF00 >= 4744, due. */
        assert(!due(before_wrap, 0, false, true, false, before_wrap));
        assert(!due(before_wrap + 1, 0, false, true, false, before_wrap));
        assert(!due(0xFFFFFFFFUL, 0, false, true, false, before_wrap)); /* 255 ms */
        /* Elapsed is 256 + now once past the wrap, so the boundary sits 4744 ms
         * past zero. These four agree under both forms; they pin the boundary,
         * not the rollover. */
        assert(!due(0x00000000UL, 0, false, true, false, before_wrap)); /* 256 ms */
        assert(!due(4743UL, 0, false, true, false, before_wrap));       /* 4999 ms */
        assert(due(4744UL, 0, false, true, false, before_wrap));        /* 5000 ms */
        assert(due(60000UL, 0, false, true, false, before_wrap));
    }

    /* 6a-bis. SIGNED subtraction is the other tempting rewrite, and it fails in
     * the opposite direction: `(int32_t)(now - x) >= INTERVAL` reads any gap
     * wider than 2^31 ms as negative and therefore never due, which is the
     * stuck announcer. Pin all three branches against it. */
    assert(due(0x90000000UL, 0, false, true, false, 0));        /* boot delay */
    assert(due(0x90000000UL, 0, true, true, false, 0));         /* interval */
    assert(due(0x90000000UL, 0, true, true, true, 0));          /* reconnect */

    /* 6b. Interval straddling the wrap. last_announce is 1 s before 2^32; a
     * `now` a few hundred ms past zero is ~1.3 s later, nowhere near 30 min, and
     * must NOT be read as due — that is the spurious announce (and, with the
     * naive form, a spurious announce every single tick for half an hour). */
    {
        const uint32_t last = 0xFFFFFC18UL;          /* 1000 ms before the wrap */
        assert(!due(0x00000064UL, last, true, true, false, 0)); /* +1100 ms */
        assert(!due(999999UL, last, true, true, false, 0));     /* +1001 s */
        /* ...and it IS due once a full interval has actually elapsed across
         * the boundary: last + 1000 lands at 0, so +IVAL lands at IVAL-1000. */
        assert(due(IVAL - 1000UL, last, true, true, false, 0));
        assert(due(IVAL, last, true, true, false, 0));
    }

    /* 6c. The stuck direction, stated on its own: a last_announce close to the
     * top of the range must not suppress announces forever once `now` wraps.
     * Sweep the whole wrap window at 1 s steps and insist the answer flips
     * exactly once, at the interval, and then stays true. */
    {
        const uint32_t last = 0xFFFFFFFFUL - (IVAL / 2);
        bool seen_true = false;
        for (uint32_t k = 0; k <= IVAL + 60000UL; k += 1000UL) {
            uint32_t now = last + k;                 /* wraps mid-sweep */
            bool d = due(now, last, true, true, false, 0);
            assert(d == (k >= IVAL));
            if (d) seen_true = true;
        }
        assert(seen_true);
    }

    /* 6d. The reconnect floor straddling the wrap, in both directions. Its
     * stamp is last_announce, which after a rollover is numerically LARGER than
     * now — so this is the branch where a naive comparison would either
     * announce on every edge or never announce again. */
    {
        /* 32 ms since the last announce, across the wrap: a flap, refused. */
        assert(!due(0x00000010UL, 0xFFFFFFF0UL, true, true, true, 0x00000010UL));
        /* ~131 s since the last announce, across the wrap: a real outage. */
        assert(due(0x00010000UL, 0xFFFF0000UL, true, true, true, 0x00010000UL));
        /* And the boundary itself, placed so the subtraction must wrap: with
         * last_announce 1 ms before the rollover, elapsed is now + 1. */
        assert(!due(RECON - 2, 0xFFFFFFFFUL, true, true, true, 0));
        assert(due(RECON - 1, 0xFFFFFFFFUL, true, true, true, 0));
    }

    /* 7. The constants themselves, so a careless edit to the header shows up
     * here rather than as an unexplained change in device behaviour. */
    assert(RNS_ANNOUNCE_BOOT_DELAY_MS == 5000UL);
    assert(RNS_ANNOUNCE_INTERVAL_MS == 1800000UL);
    assert(RNS_ANNOUNCE_RECONNECT_MIN_MS == 60000UL);
    /* And the shape they have to keep: every interval must stay well under
     * 2^31, which is the bound that makes the unsigned-subtraction comparisons
     * above correct across a rollover. */
    assert(RNS_ANNOUNCE_INTERVAL_MS < 0x80000000UL);
    assert(RNS_ANNOUNCE_BOOT_DELAY_MS < RNS_ANNOUNCE_INTERVAL_MS);
    /* The floor has to sit strictly between the two to be worth anything: at or
     * below the flap period it stops nothing, and at or above the interval it
     * would swallow the reconnect it exists to permit. It must also clear
     * RNS_TCP_BACKOFF_MIN_MS (3000) by a wide margin — that is the rate a
     * flapping peer offers edges at. */
    assert(RNS_ANNOUNCE_RECONNECT_MIN_MS > 3000UL * 4UL);
    assert(RNS_ANNOUNCE_RECONNECT_MIN_MS < RNS_ANNOUNCE_INTERVAL_MS);

    /* ---------------------------------------------------------------------
     * 8. THE SEAM: the schedule driven through the caller's own tick loop.
     *
     * Sections 1-7 ask the decision function one question at a time, and a
     * correct decision function answers all of them correctly even when the
     * caller around it throws the answer away. That is not hypothetical — it is
     * what shipped: the floor was applied to the RAW offline->online sample,
     * the caller stamped was_online on every tick whether or not an announce
     * fired, and so a refused edge was CONSUMED. The next tick saw
     * was_online == true and fell through to the 30 min branch. The floor was
     * behaving as a filter when it was documented as a rate limiter.
     *
     * Nothing above could see it. Every per-call assertion still passed, and no
     * mutant of the header reproduced it, because the header was right — the
     * caller was wrong, and no test drove the two together. These do. */

    /* 8a. THE REGRESSION, exactly as reported: one 3 s outage, 10 s after the
     * boot announce, on a link that then stays up forever. */
    {
        const uint32_t first = announce_at(1, 15000UL, 18000UL);
        const uint32_t second = announce_at(2, 15000UL, 18000UL);
        assert(first == BOOT);                 /* boot announce at 5 000 ms */
        /* Deferred to last + floor... */
        assert(second == BOOT + RECON);        /* 65 000 ms */
        /* ...and specifically NOT dropped into the next interval, which is the
         * shape of the defect: 29.8 minutes unreachable while /rns/status
         * reported ready, announced:true, online:true and a climbing
         * up_age_s. */
        assert(second != BOOT + IVAL);         /* not 1 805 000 ms */
    }

    /* 8b. The same thing swept across the gap between the last announce and the
     * reconnect. Below the floor the announce is DEFERRED to last + floor;
     * at or above it, it lands on the tick the link returns. Nothing in the
     * sweep is ever postponed to the interval. */
    {
        const uint32_t gaps[] = {1000UL, 5000UL, 30000UL, 59000UL,
                                 RECON, 61000UL, 120000UL};
        for (unsigned i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
            const uint32_t g = gaps[i];
            const uint32_t up = BOOT + g;      /* reconnect, g after announce */
            const uint32_t down = up - 500UL;  /* a short outage before it */
            const uint32_t got = announce_at(2, down, up);
            const uint32_t want = BOOT + (g < RECON ? RECON : g);
            assert(got == want);
            assert(got != BOOT + IVAL);
            /* However the gap falls, the two announces are never closer than
             * the floor: the deferral must not become an early announce. */
            assert((uint32_t)(got - BOOT) >= RECON);
        }
    }

    /* 8c. The debt survives a SECOND outage that begins before it is paid.
     * A reconnect at 6 s is refused; the link drops again at 20 s and returns
     * at 30 s, still inside the floor. The announce is still owed and still
     * lands at last + floor — not cancelled by the second outage, and not
     * doubled by it either.
     *
     * This is a readability case, not an independent one: it states the
     * "an outage does not cancel the debt" rule in its own right, but against
     * every non-equivalent mutant tried so far it fails wherever 8a fails.
     * Kept because the rule is worth writing down where someone will read it;
     * do not count it as separate coverage. */
    {
        sched s = {0, 0, false, false, false};
        unsigned seen = 0;
        uint32_t first = 0, second = 0;
        for (uint32_t t = 0; t <= HORIZON; t += TICK) {
            bool online = !((t >= 5500UL && t < 6000UL) ||
                            (t >= 20000UL && t < 30000UL));
            if (step(&s, t, online)) {
                if (++seen == 1) first = t;
                else if (seen == 2) { second = t; break; }
            }
        }
        assert(first == BOOT);
        assert(second == BOOT + RECON);
    }

    /* 8d. SUSTAINED FLAPPING, through the same loop: a link that comes up
     * cleanly, announces, and then starts dropping every 3 s for an hour —
     * which is what a peer that accepts and immediately drops the socket looks
     * like once the node is already running. ~1200 edges must not buy ~1200
     * signatures, and no two announces may ever land closer together than the
     * floor.
     *
     * THE DUTY CYCLE HERE IS THE INVERSE OF THE DEVICE'S, and that is a
     * deliberate simplification rather than an oversight. Modelled below: online
     * for 2 980 ms of every 3 000, offline for one tick. On the board it is the
     * other way round — link_down() arms RNS_TCP_BACKOFF_MIN_MS, so the
     * interface sits DOWN for ~3 s and is up only for the moment between a
     * successful connect and the peer dropping it again. What the schedule
     * actually consumes is the EDGE RATE, which is one per 3 s either way, and
     * both shapes were run: same 60 announces, same 60 000 ms minimum gap. The
     * assertions below therefore hold for the device; only the picture is
     * simpler than the device's. */
    {
        sched s = {0, 0, false, false, false};
        uint32_t count = 0;
        uint32_t prev = 0;
        bool have_prev = false;
        for (uint32_t t = 0; t <= 3600000UL; t += TICK) {
            /* Healthy long enough to make the boot announce, then flapping. */
            bool online = (t < 10000UL) ? true : ((t % 3000UL) != 0);
            if (step(&s, t, online)) {
                if (have_prev) assert((uint32_t)(t - prev) >= RECON);
                prev = t;
                have_prev = true;
                count++;
            }
        }
        /* An hour at one announce per minute, give or take the boot announce
         * and where the last one falls. */
        assert(count >= 3600000UL / RECON - 1);
        assert(count <= 3600000UL / RECON + 1);
        /* And an order of magnitude below the number of edges offered. */
        assert(count < (3600000UL / 3000UL) / 10UL);
    }

    /* 8d-bis. A link flapping FROM BOOT never announces at all, and that is
     * correct rather than a gap in 8d. online_since is re-stamped on every
     * edge, so a link that is never up for RNS_ANNOUNCE_BOOT_DELAY_MS
     * continuously never completes the boot delay — and announcing into a
     * socket that keeps dying is the case the offline rule exists to avoid,
     * since a failed outbound logs an ERROR and Log.cpp flushes the UART
     * synchronously on the loop task. Pinned because it is easy to "fix" by
     * accident when someone decides the boot delay should be measured from
     * boot. */
    {
        sched s = {0, 0, false, false, false};
        for (uint32_t t = 0; t <= HORIZON; t += TICK) {
            assert(!step(&s, t, (t % 3000UL) != 0));
        }
        assert(!s.announced);
        /* The debt is owed the whole time and simply never becomes payable. */
        assert(s.pending);
    }

    /* 8e. A HEALTHY LINK IS UNAFFECTED. No outage at all: the boot announce,
     * then the interval, and nothing the latch does may add one. */
    {
        sched s = {0, 0, false, false, false};
        uint32_t count = 0;
        uint32_t at[4] = {0, 0, 0, 0};
        for (uint32_t t = 0; t <= BOOT + IVAL * 2; t += TICK) {
            if (step(&s, t, true)) {
                if (count < 4) at[count] = t;
                count++;
            }
        }
        assert(count == 3);
        assert(at[0] == BOOT);
        assert(at[1] == BOOT + IVAL);
        assert(at[2] == BOOT + IVAL * 2);
    }

    /* 8f. A LINK THAT NEVER COMES UP NEVER ANNOUNCES, driven the same way —
     * the one failure that would put a blocking Serial.flush() in the drain. */
    {
        sched s = {0, 0, false, false, false};
        for (uint32_t t = 0; t <= HORIZON; t += TICK) {
            assert(!step(&s, t, false));
        }
        assert(!s.announced);
    }

    return 0;
}
