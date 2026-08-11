#pragma once

/*
 * rns/pktfilter.h — the keep/drop decision for Transport's inbound packet
 * filter, as a pure function of two header fields.
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
 * instead of a signature verification.
 *
 * THE POLICY. This node is a leaf: zero destinations, zero announce handlers,
 * transport_enabled(false). An accepted announce only fills a path table and a
 * known-destinations table that nothing on this board reads. So:
 *
 *   - ANNOUNCE with context PATH_RESPONSE is KEPT. A path response is the
 *     answer to a path request we sent; dropping it would break the one
 *     announce-shaped exchange a leaf actually initiates.
 *   - Any other ANNOUNCE is DROPPED.
 *   - Everything else is KEPT, untouched — including packet types and contexts
 *     this function does not recognise. Fail-open is deliberate: never drop
 *     what we do not understand.
 *
 * IF THIS NODE EVER REGISTERS A DESTINATION OF ITS OWN, RE-READ THIS FILE.
 * Passive key learning is gone with the dropped announces: a peer's public key
 * is remembered only inside Identity::validate_announce(), which is exactly
 * what a dropped announce never reaches. After this change a peer can be
 * addressed only by asking for it — an explicit Transport::request_path() and
 * the PATH_RESPONSE announce it draws, which this filter keeps for that reason.
 * Nothing reports the difference: a send to a peer whose announce was dropped
 * simply finds no path and no identity to encrypt to, so the failure would be
 * silent rather than loud.
 *
 * The mark this policy turns on is also chosen by the sender. Any peer can flag
 * an ordinary announce as PATH_RESPONSE and still cost us the full Ed25519.
 * That is the status quo of the protocol and not something this filter makes
 * worse — what it buys is the ordinary announce flood, which is the traffic
 * that was actually measured — but it is not a defence, and nothing here should
 * be read as one.
 *
 * IDEMPOTENCE. A dropped packet never reaches Transport's packet hashlist, so
 * the same packet arriving twice is judged twice. This function reads nothing
 * but its two arguments and writes nothing at all, so the second judgement is
 * always the first one. Keep it that way — no counters, no caches, no clock.
 *
 * Free function, no Arduino and no C++ runtime, so the same code compiles on
 * the host for tools/test_rns_filter.cpp. The two constants below mirror
 * RNS::Type::Packet::types and RNS::Type::Packet::context_types; skills/rns.cpp
 * static_asserts them against the library enums, so a value drifting in a
 * future microReticulum fails the firmware build rather than the device.
 */

#include <stdint.h>

/* RNS::Type::Packet::types::ANNOUNCE */
#define RNS_PKT_TYPE_ANNOUNCE 0x01
/* RNS::Type::Packet::context_types::PATH_RESPONSE */
#define RNS_PKT_CONTEXT_PATH_RESPONSE 0x0B

/* True to hand the packet on to the rest of Transport::inbound(), false to drop
 * it before any signature work. Pure: no state, no allocation, no throw. */
static inline bool rns_filter_keep_packet(uint8_t packet_type, uint8_t context) {
    if (packet_type != RNS_PKT_TYPE_ANNOUNCE) return true;
    return context == RNS_PKT_CONTEXT_PATH_RESPONSE;
}
