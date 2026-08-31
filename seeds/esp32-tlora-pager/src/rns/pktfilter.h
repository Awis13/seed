#pragma once

/*
 * rns/pktfilter.h — the keep/drop decision for Transport's inbound packet
 * filter, as a pure function of its arguments.
 *
 * WHY THIS EXISTS. Processing one accepted Reticulum announce measured 221 ms
 * on this board (drain_us_max 221398 us; the next pass after ?reset=1 gave
 * 208 ms), against a drain budget of 8 ms. Practically all of it is one
 * Ed25519::verify inside Identity::validate_announce — software Ed25519 on an
 * ESP32-S3, which has no ECC accelerator and no Ed25519 in mbedTLS. That price
 * is not negotiable for an announce we accept; the only lever is to stop paying
 * it for announces we have no use for.
 *
 * Transport::inbound() calls the filter callback ~350 lines before it reaches
 * Identity::validate_announce(), so returning false here costs two field reads
 * and a dest-hash compare instead of a signature verification.
 *
 * THE POLICY. This node has destinations of its own and no announce handlers,
 * and runs transport_enabled(false). An accepted INBOUND announce fills a path
 * table that is uncapped (TTL 7 days, no count ceiling, cull has no caller).
 * Transport::_new_path_table.put retains every dest it is shown. So:
 *
 *   - ANNOUNCE with context PATH_RESPONSE is kept ONLY when the packet's
 *     destination hash is in the caller-supplied allow-list. A path response
 *     is the reply to a path request we sent, so we keep it only for dests we
 *     asked about / are willing to reach (configured peer + this node's own
 *     IN destination hashes). Empty allow-list or dest not in the list: DROP.
 *     That starves put() of foreign dests. While the peer is unset the list
 *     is empty (or holds only our own dests), so a leaf with no outbound RNS
 *     peer cannot grow the heap path table from other people's announces.
 *   - Any other ANNOUNCE is DROPPED.
 *   - Everything else is KEPT, untouched — including packet types and contexts
 *     this function does not recognise. Fail-open is deliberate: never drop
 *     what we do not understand.
 *
 * THE RE-READ THIS FILE ASKED FOR HAS HAPPENED. The previous version of this
 * comment said "zero destinations" and ended with an instruction, in capitals,
 * to come back here the day the node registered one. It has: skills/rns.cpp
 * builds IN/SINGLE destinations so the node has an address. PATH_RESPONSE is
 * no longer unconditionally kept: that was the hole that filled the path
 * table. The rest of the policy is UNCHANGED, and this is why it is still
 * correct.
 *
 * Being addressable means answering path requests, and the worry was that this
 * filter would drop them. It does not, and it cannot, for two independent
 * reasons:
 *
 *   1. A PATH REQUEST IS NOT AN ANNOUNCE. It arrives as packet_type = DATA
 *      (0x00) with context = CONTEXT_NONE (0x00), addressed to the library's
 *      own PLAIN "path.request" destination. That is not a guess from the shape
 *      of the code: Transport::request_path() builds it as a plain Packet on a
 *      path.request Destination and the source states the defaults outright
 *      ("packet_type=DATA, context=CONTEXT_NONE, transport_type=BROADCAST,
 *      header_type=HEADER_1 are all defaults"), and upstream Python's
 *      Transport.py says the same values explicitly at its own path-request
 *      send. So the very first statement of the decision below — packet_type is
 *      not ANNOUNCE, therefore keep — returns true and nothing else is
 *      consulted. There is no PATH_REQUEST context constant in either
 *      implementation to confuse this with; the only path-shaped context that
 *      exists is PATH_RESPONSE = 0x0B, which is the REPLY.
 *   2. THE REPLY IS OUTBOUND. Transport answers a path request for a local
 *      destination by calling announce() on it, and Transport::outbound() never
 *      consults the filter callback — the hook is on the inbound path only. An
 *      announce this node emits therefore cannot be filtered by this file even
 *      in principle.
 *
 * What the reply DOES cost is a software Ed25519 signature on the loop task,
 * inside the drain, once per undeduplicated path request. That is the price of
 * having an address, not something this filter can reduce; skills/rns.cpp
 * records it where the cost is paid.
 *
 * The other consequence named in the original note still stands unchanged.
 * Passive key learning is gone with the dropped announces: a peer's public key
 * is remembered only inside Identity::validate_announce(), which is exactly
 * what a dropped announce never reaches. A peer can be addressed only by asking
 * for it — an explicit Transport::request_path() and the PATH_RESPONSE announce
 * it draws, which this filter keeps only when that dest is on the allow-list.
 * Nothing reports the difference: a send to a peer whose announce was dropped
 * simply finds no path and no identity to encrypt to, so the failure would be
 * silent rather than loud. Note the asymmetry this leaves, because it is the
 * shape of the node: others can reach US without our help (they path-request,
 * the library answers), while we must ask before we can reach THEM.
 *
 * The mark this policy turns on is also chosen by the sender. Any peer can flag
 * an ordinary announce as PATH_RESPONSE, but it now also has to name a dest we
 * already listed or it is dropped before Ed25519. That is still not a defence
 * against a dest we did list — what it buys is the ordinary announce flood and
 * the foreign PATH_RESPONSE flood that was filling the path table — and nothing
 * here should be read as one.
 *
 * IDEMPOTENCE. A dropped packet never reaches Transport's packet hashlist, so
 * the same packet arriving twice is judged twice. This function reads nothing
 * but its arguments and writes nothing at all, so the second judgement is
 * always the first one. Keep it that way — no counters, no caches, no clock.
 *
 * Free function, no Arduino and no C++ runtime, so the same code compiles on
 * the host for tools/test_rns_filter.cpp. The two constants below mirror
 * RNS::Type::Packet::types and RNS::Type::Packet::context_types; skills/rns.cpp
 * static_asserts them against the library enums, so a value drifting in a
 * future microReticulum fails the firmware build rather than the device.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* RNS::Type::Packet::types::ANNOUNCE */
#define RNS_PKT_TYPE_ANNOUNCE 0x01
/* RNS::Type::Packet::context_types::PATH_RESPONSE */
#define RNS_PKT_CONTEXT_PATH_RESPONSE 0x0B
/* Truncated destination hash: RNS::Type::Reticulum::DESTINATION_LENGTH */
#define RNS_DEST_HASH_LEN 16

/* True to hand the packet on to the rest of Transport::inbound(), false to drop
 * it before any signature work. Pure: no state, no allocation, no throw. */
static inline bool rns_filter_keep_packet(
    uint8_t packet_type, uint8_t context,
    const uint8_t *dest, size_t dest_len,
    const uint8_t *allowed, size_t n_allowed) {
    if (packet_type != RNS_PKT_TYPE_ANNOUNCE) return true;
    if (context != RNS_PKT_CONTEXT_PATH_RESPONSE) return false;
    if (dest == NULL || dest_len != RNS_DEST_HASH_LEN ||
        allowed == NULL || n_allowed == 0)
        return false;
    for (size_t i = 0; i < n_allowed; i++) {
        if (memcmp(dest, allowed + (i * RNS_DEST_HASH_LEN),
                   RNS_DEST_HASH_LEN) == 0)
            return true;
    }
    return false;
}
