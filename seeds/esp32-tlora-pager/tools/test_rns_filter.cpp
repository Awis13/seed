/*
 * Host tests for the Reticulum inbound prefilter's keep/drop decision.
 *
 * rns_filter_keep_packet() is the one part of the filter that can be exercised
 * without a stack, a socket or a board, and it is the part where a mistake is
 * expensive in both directions: keep too much and the loop task pays ~220 ms of
 * Ed25519 per announce (and the uncapped path table grows), drop too much and
 * the node silently stops hearing traffic it needs. So the whole truth table
 * is pinned here.
 *
 * The constants come from rns/pktfilter.h, which mirrors
 * RNS::Type::Packet::types and ::context_types; skills/rns.cpp static_asserts
 * them against the library enums, so this file does not need the library.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/rns/pktfilter.h"

/* RNS::Type::Packet::types */
#define PKT_DATA        0x00
#define PKT_ANNOUNCE    0x01
#define PKT_LINKREQUEST 0x02
#define PKT_PROOF       0x03

/* A sample of RNS::Type::Packet::context_types, low and high ends. */
#define CTX_NONE          0x00
#define CTX_RESOURCE      0x01
#define CTX_REQUEST       0x09
#define CTX_RESPONSE      0x0A
#define CTX_PATH_RESPONSE 0x0B
#define CTX_CHANNEL       0x0E
#define CTX_KEEPALIVE     0xFA
#define CTX_LRPROOF       0xFF

/* Every assertion goes through this wrapper, which calls the function TWICE and
 * insists the two answers agree. A dropped packet never reaches Transport's
 * packet hashlist, so the same announce arriving twice is judged twice — a
 * decision that depended on any hidden state would be a filter that flip-flops
 * on retransmissions. */
static bool keep(uint8_t type, uint8_t context,
                 const uint8_t *dest = nullptr, size_t dest_len = 0,
                 const uint8_t *allowed = nullptr, size_t n_allowed = 0) {
    bool first = rns_filter_keep_packet(type, context, dest, dest_len,
                                        allowed, n_allowed);
    bool second = rns_filter_keep_packet(type, context, dest, dest_len,
                                         allowed, n_allowed);
    assert(first == second);
    return first;
}

int main() {
    uint8_t dest_a[RNS_DEST_HASH_LEN];
    uint8_t dest_b[RNS_DEST_HASH_LEN];
    uint8_t dest_c[RNS_DEST_HASH_LEN];
    uint8_t allow_two[2 * RNS_DEST_HASH_LEN];

    memset(dest_a, 0x11, sizeof(dest_a));
    memset(dest_b, 0x22, sizeof(dest_b));
    memset(dest_c, 0x33, sizeof(dest_c));
    memcpy(allow_two, dest_a, RNS_DEST_HASH_LEN);
    memcpy(allow_two + RNS_DEST_HASH_LEN, dest_b, RNS_DEST_HASH_LEN);

    /* 1. A plain announce is dropped: nothing on this node reads what an
     * accepted one would fill in, and accepting it costs an Ed25519 verify. */
    assert(!keep(PKT_ANNOUNCE, CTX_NONE));
    assert(!keep(PKT_ANNOUNCE, CTX_NONE, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 1));

    /* 2. PATH_RESPONSE is kept only for a dest on the allow-list. */
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_a, RNS_DEST_HASH_LEN,
                 nullptr, 0));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 0));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_a, 15, dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_a, 17, dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, nullptr, RNS_DEST_HASH_LEN,
                 dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_c, RNS_DEST_HASH_LEN,
                 allow_two, 2));
    assert(keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_a, RNS_DEST_HASH_LEN,
                dest_a, 1));
    assert(keep(PKT_ANNOUNCE, CTX_PATH_RESPONSE, dest_b, RNS_DEST_HASH_LEN,
                allow_two, 2));

    /* 3. Data and proof packets are untouched, in every context. */
    assert(keep(PKT_DATA, CTX_NONE));
    assert(keep(PKT_DATA, CTX_REQUEST));
    assert(keep(PKT_DATA, CTX_RESPONSE));
    assert(keep(PKT_DATA, CTX_CHANNEL));
    assert(keep(PKT_PROOF, CTX_NONE));
    assert(keep(PKT_PROOF, CTX_LRPROOF));
    assert(keep(PKT_LINKREQUEST, CTX_NONE));
    assert(keep(PKT_DATA, CTX_NONE, dest_c, RNS_DEST_HASH_LEN, dest_a, 1));
    assert(keep(PKT_PROOF, CTX_PATH_RESPONSE, nullptr, 0, nullptr, 0));

    /* 4. Fail-open on anything unrecognised. A packet type this build has never
     * heard of is not a packet we may drop: the filter's whole warrant is that
     * we know an announce is useless HERE, and that argument does not extend to
     * types that do not exist yet. The context is irrelevant for these. */
    for (unsigned t = 0; t < 256; t++) {
        if (t == PKT_ANNOUNCE) continue;
        assert(keep((uint8_t)t, CTX_NONE));
        assert(keep((uint8_t)t, CTX_PATH_RESPONSE));
        assert(keep((uint8_t)t, 0x5A));
        assert(keep((uint8_t)t, 0xFF));
        assert(keep((uint8_t)t, CTX_PATH_RESPONSE, dest_c, RNS_DEST_HASH_LEN,
                    dest_a, 1));
        assert(keep((uint8_t)t, CTX_NONE, nullptr, 0, nullptr, 0));
    }

    /* 5. The announce row in full: only PATH_RESPONSE can be true, and only
     * with a matching allow. Neighbours (RESPONSE 0x0A, CHANNEL 0x0E) stay
     * dropped even when dest is listed. */
    for (unsigned c = 0; c < 256; c++) {
        bool listed = keep(PKT_ANNOUNCE, (uint8_t)c, dest_a, RNS_DEST_HASH_LEN,
                           dest_a, 1);
        bool empty = keep(PKT_ANNOUNCE, (uint8_t)c);
        if (c == CTX_PATH_RESPONSE) {
            assert(listed);
            assert(!empty);
        } else {
            assert(!listed);
            assert(!empty);
        }
    }
    assert(!keep(PKT_ANNOUNCE, CTX_RESOURCE, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_RESPONSE, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_CHANNEL, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 1));
    assert(!keep(PKT_ANNOUNCE, CTX_KEEPALIVE, dest_a, RNS_DEST_HASH_LEN,
                 dest_a, 1));

    /* 6. The constants themselves, so a careless edit to the header shows up
     * here rather than as an unexplained change in device behaviour. */
    assert(RNS_PKT_TYPE_ANNOUNCE == PKT_ANNOUNCE);
    assert(RNS_PKT_CONTEXT_PATH_RESPONSE == CTX_PATH_RESPONSE);
    assert(RNS_DEST_HASH_LEN == 16);

    return 0;
}
