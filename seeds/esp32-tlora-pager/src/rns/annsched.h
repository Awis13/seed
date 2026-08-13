#pragma once

/*
 * rns/annsched.h — when this node's destination should announce itself, as a
 * pure function of the clock and the interface state.
 *
 * WHY THIS EXISTS. Destination::announce() is fully synchronous on the calling
 * task and signs the announce with software Ed25519 — the same rweather library
 * whose verify measured 221 ms on this ESP32-S3, which has no ECC accelerator
 * and no Ed25519 in mbedTLS. It runs on the loop task, inside the same tick that
 * owns the 8 ms socket drain. So the question "may we announce now?" is worth
 * getting exactly right, and it is worth being able to test without a board.
 *
 * THE POLICY, and the reason for each half of it:
 *
 *   - NEVER while the interface is offline. Transport::outbound() on a node
 *     with no online interface fails the send and logs an ERROR, and Log.cpp
 *     ends every emitted line with a blocking Serial.flush() on the loop task.
 *     An announce we cannot deliver therefore costs a signature AND a UART
 *     stall, and buys nothing.
 *   - The FIRST announce waits RNS_ANNOUNCE_BOOT_DELAY_MS after the interface
 *     reports online. A TCP socket that has just completed its handshake has not
 *     necessarily been accepted by the peer's Reticulum yet, and announcing into
 *     that gap wastes the one announce anybody is watching for. (Shipped
 *     reference: rsPager on this same board announces ~5 s after boot.)
 *   - Then every RNS_ANNOUNCE_INTERVAL_MS while it STAYS online. Same reference
 *     defaults to 30 min. An announce is 148 bytes plus app_data; the cost that
 *     matters is the signature, not the wire.
 *   - On an offline->online transition, once we have announced at all,
 *     PROMPTLY BUT NOT UNCONDITIONALLY. A peer that lost our socket also lost
 *     the path to us, and waiting out the remainder of a 30 min interval would
 *     leave us unaddressable for no reason — but "announce on every edge" is a
 *     denial of service against ourselves, so the edge is floored at
 *     RNS_ANNOUNCE_RECONNECT_MIN_MS. Floored, not filtered: an edge the floor
 *     refuses is DEFERRED to last_announce + the floor, never discarded. See
 *     the DEFER note below, which is the whole reason the latch exists.
 *     Before the first announce the transition is not special: the boot delay
 *     applies to a reconnect exactly as it applies to the first connect.
 *
 * WHY THE RECONNECT EDGE NEEDS A FLOOR AND THE INTERVAL DOES NOT. The
 * reconnect ladder in skills/rns.cpp does not climb across a flap: RnsTcpInterface
 * ::loop() resets g_rns_backoff_ms to RNS_TCP_BACKOFF_MIN_MS (3 s) on every
 * SUCCESSFUL connect, and link_down() re-arms from that reset value. A peer that
 * accepts the socket and immediately drops it — or WiFi or the WireGuard netif
 * flapping underneath one — therefore produces an offline->online edge about
 * every 3 seconds, indefinitely. Without a floor each of those edges is a
 * software Ed25519 signature on the loop task, so a flapping link would cost
 * more signatures per minute than a healthy one costs per hour. The floor is
 * one minute: it is 20x the flap period, so a stuck link cannot sign more than
 * once a minute, and it is 30x SHORTER than the steady-state interval, so a
 * genuine outage still re-announces promptly rather than serving out the rest of
 * a half-hour. It is measured from the last announce, not from the edge, which
 * is what makes a burst of edges cost exactly one announce.
 *
 * WRAPAROUND. Every comparison is an unsigned subtraction of two uint32_t
 * millisecond stamps, which is correct across the ~49.7-day millis() rollover
 * for any interval shorter than 2^31 ms. Do not rewrite these as
 * `now >= last + interval`: that form is the bug this note exists to prevent —
 * it goes permanently true or permanently false for one rollover's worth of
 * time. tools/test_rns_annsched.sh pins the behaviour near 2^32.
 *
 * DEFER, DO NOT DROP — and this is the correction that shaped the interface.
 * The reconnect edge is a ONE-TICK event: the interface is offline on one
 * evaluation and online on the next, and after that there is nothing left to
 * observe. An earlier revision fed the raw `was_online` into the decision, so
 * an edge the floor refused was simply CONSUMED: the next tick saw
 * was_online == true, fell through to the steady-state branch, and the announce
 * did not move to `last + 60 s` — it moved to `last + 30 min`. Any reconnect
 * within a minute of any announce was silently lost, and the first minute of a
 * link's life is exactly when it is least stable, which is the reason the boot
 * delay exists at all. Nothing reported it: /rns/status would show ready,
 * announced:true, online:true and a climbing up_age_s while the node sat
 * unreachable for half an hour.
 *
 * So the edge is LATCHED rather than sampled. rns_announce_edge_latch() turns
 * the transition into a sticky "an announce is owed" flag, the decision reads
 * that flag instead of was_online, and the flag is cleared by nothing except an
 * announce actually firing. A refused edge stays owed and lands the moment the
 * floor expires. The latch lives here, next to the decision that consumes it,
 * because the defect was never in either piece on its own — it was in the seam
 * between them, and a pure function nobody can drive end to end is a pure
 * function whose caller can still get it wrong.
 *
 * PURITY. No Arduino, no clock of its own, no state: the caller passes every
 * millis() stamp and every flag in, so the same inputs always give the same
 * answer and the whole schedule compiles on the host for
 * tools/test_rns_annsched.cpp, which drives both functions in the caller's
 * order over simulated timelines.
 */

#include <stdbool.h>
#include <stdint.h>

/* Delay from "interface online" to the first announce on that link. */
#define RNS_ANNOUNCE_BOOT_DELAY_MS 5000UL
/* Steady-state re-announce period while the link stays up. */
#define RNS_ANNOUNCE_INTERVAL_MS 1800000UL
/* Floor on the offline->online re-announce. See the note above: the reconnect
 * backoff resets on every successful connect, so a flapping peer offers an edge
 * roughly every RNS_TCP_BACKOFF_MIN_MS (3 s) and this is what stops each one
 * costing a signature. */
#define RNS_ANNOUNCE_RECONNECT_MIN_MS 60000UL

/*
 * Latch the offline->online transition into a sticky "an announce is owed"
 * flag. Call it once per evaluation, BEFORE rns_announce_due(), with the
 * previous flag and the two online samples the caller already keeps.
 *
 * The caller owns exactly one rule about the result, and it is the rule that
 * makes the floor a deferral instead of a filter: CLEAR IT ONLY WHEN AN
 * ANNOUNCE ACTUALLY FIRES. Not when the floor refuses one, not when the link
 * goes down again, not on a timer. An edge that was refused is still owed, and
 * an outage that begins before the debt is paid does not cancel it.
 *
 * pending      the flag as it stood after the previous evaluation
 * iface_online the interface reports online right now
 * was_online   what it reported at the previous evaluation
 */
static inline bool rns_announce_edge_latch(bool pending, bool iface_online,
                                           bool was_online) {
    if (iface_online && !was_online) return true;
    return pending;
}

/*
 * now_ms           millis() now
 * last_announce_ms millis() at the last announce ATTEMPT (see skills/rns.cpp:
 *                  a throw still stamps it, so a failing announce backs off to
 *                  the interval instead of retrying every tick). BOTH the
 *                  interval and the reconnect floor are measured from it.
 * have_announced   an announce has been attempted at least once since boot
 * iface_online     the interface reports online right now
 * edge_pending     rns_announce_edge_latch()'s flag: an offline->online
 *                  transition is owed an announce and has not been paid yet.
 *                  NOT the raw was_online sample — see the DEFER note above for
 *                  what reading that directly cost.
 * online_since_ms  millis() when it last went offline -> online
 */
static inline bool rns_announce_due(uint32_t now_ms, uint32_t last_announce_ms,
                                    bool have_announced, bool iface_online,
                                    bool edge_pending, uint32_t online_since_ms) {
    if (!iface_online) return false;
    if (!have_announced)
        return (uint32_t)(now_ms - online_since_ms) >= RNS_ANNOUNCE_BOOT_DELAY_MS;
    if (edge_pending)
        return (uint32_t)(now_ms - last_announce_ms) >=
               RNS_ANNOUNCE_RECONNECT_MIN_MS;
    return (uint32_t)(now_ms - last_announce_ms) >= RNS_ANNOUNCE_INTERVAL_MS;
}
