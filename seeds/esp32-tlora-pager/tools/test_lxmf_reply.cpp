/*
 * Host test for the pure LXMF-reply helpers in src/lxmf_reply.h (TLORA-LXMF C4).
 *
 * No firmware / FreeRTOS / SD / RNS / crypto. Two pure pieces:
 *   1. lxmf_reply_build() — builds a single-packet reply message: dest/source
 *      set, empty title, content bounded to LXMF_REPLY_CONTENT_MAX on a UTF-8
 *      boundary, no fields; then it must encode (C1) and parse back with the
 *      content intact. Cases: short body round-trips; an over-long body is cut
 *      on a codepoint boundary (never mid-character) and flagged; the sign-input
 *      payload equals the packed payload (the C1 sign-injection contract).
 *   2. LxmfOriginTable — the room -> sender map the reply routing decides on:
 *      set then lookup returns the sender; a different room misses; clear makes
 *      it miss; a re-set updates the sender; an over-cap name is ignored; the
 *      table evicts when full but keeps the most recent rooms findable.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../src/lxmf_reply.h"

/* A fake 64-byte signature; the codec is crypto-free, so any bytes will do to
 * prove the reply PACKS and PARSES (the real Ed25519 sign lives in firmware). */
static void fill_sig(uint8_t sig[LXMF_SIG_LEN]) {
    for (int i = 0; i < LXMF_SIG_LEN; i++) sig[i] = (uint8_t)(0x40 + i);
}

static void test_build_short_roundtrip() {
    uint8_t dest[16], src[16];
    for (int i = 0; i < 16; i++) { dest[i] = (uint8_t)(0xA0 + i); src[i] = (uint8_t)(0xB0 + i); }

    LxmfMsg m;
    int trunc = lxmf_reply_build(&m, dest, src, 1699999999.0, "hello back");
    assert(trunc == 0);
    assert(memcmp(m.dest_hash, dest, 16) == 0);
    assert(memcmp(m.source_hash, src, 16) == 0);
    assert(m.title_len == 0);
    assert(m.content_len == 10 && strcmp(m.content, "hello back") == 0);
    /* a reply carries no fields */
    assert(!m.has_thread && !m.has_type && !m.has_data && !m.has_meta && !m.has_renderer);

    /* sign-input: dest(16) + source(16) + msgpack(payload) */
    uint8_t sin[512]; size_t sin_len = 0;
    assert(lxmf_encode_sign_input(&m, sin, sizeof(sin), &sin_len) == LXMF_OK);
    assert(memcmp(sin, dest, 16) == 0 && memcmp(sin + 16, src, 16) == 0);

    uint8_t sig[LXMF_SIG_LEN]; fill_sig(sig);
    uint8_t wire[512]; size_t wlen = 0;
    assert(lxmf_encode_packed(&m, sig, wire, sizeof(wire), &wlen) == LXMF_OK);
    /* the C1 sign-injection contract: packed payload (after the 96 header) is
     * the SAME bytes as the sign-input payload (after its 32 dest+source). */
    assert(wlen - LXMF_HEADER_LEN == sin_len - 2 * LXMF_HASH_LEN);
    assert(memcmp(wire + LXMF_HEADER_LEN, sin + 2 * LXMF_HASH_LEN,
                  wlen - LXMF_HEADER_LEN) == 0);

    LxmfMsg out;
    assert(lxmf_parse(wire, wlen, &out) == LXMF_OK);
    assert(memcmp(out.dest_hash, dest, 16) == 0);
    assert(memcmp(out.source_hash, src, 16) == 0);
    assert(out.content_len == 10 && strcmp(out.content, "hello back") == 0);
    assert(out.title_len == 0);
}

static void test_build_truncates_on_utf8_boundary() {
    uint8_t dest[16] = {0}, src[16] = {0};
    /* Build a body that is all 2-byte UTF-8 chars (U+00E9 'é' = 0xC3 0xA9), long
     * enough that the cut at LXMF_REPLY_CONTENT_MAX would land INSIDE a char if
     * it did not back off. MAX=256 is even, so a run of 2-byte chars puts the cut
     * exactly on a boundary; offset the run by one ASCII byte so the naive cut
     * would split a character and the backoff is actually exercised. */
    char body[1024];
    size_t o = 0;
    body[o++] = 'x';                              /* one ASCII byte up front */
    while (o + 2 < sizeof(body) - 1) { body[o++] = (char)0xC3; body[o++] = (char)0xA9; }
    body[o] = '\0';
    assert(o > LXMF_REPLY_CONTENT_MAX);

    LxmfMsg m;
    int trunc = lxmf_reply_build(&m, dest, src, 0.0, body);
    assert(trunc == 1);
    assert(m.content_len <= LXMF_REPLY_CONTENT_MAX);
    /* the cut must be a full character: with 'x' + 2-byte chars, an even offset
     * would split one, so the backoff drops to an odd length (1 + 2k). */
    assert(m.content_len % 2 == 1);
    /* and the last byte kept is a full char's trailing byte, never a lead byte
     * left dangling: the byte just past the end is NOT a continuation byte. */
    assert(((unsigned char)body[m.content_len] & 0xC0) != 0x80);

    /* it still round-trips as a valid message */
    uint8_t sig[LXMF_SIG_LEN]; fill_sig(sig);
    uint8_t wire[512]; size_t wlen = 0;
    assert(lxmf_encode_packed(&m, sig, wire, sizeof(wire), &wlen) == LXMF_OK);
    LxmfMsg out;
    assert(lxmf_parse(wire, wlen, &out) == LXMF_OK);
    assert(out.content_len == m.content_len);
    assert(memcmp(out.content, m.content, m.content_len) == 0);
}

static void test_build_exact_ceiling_not_truncated() {
    uint8_t dest[16] = {0}, src[16] = {0};
    char body[LXMF_REPLY_CONTENT_MAX + 1];
    memset(body, 'a', LXMF_REPLY_CONTENT_MAX);
    body[LXMF_REPLY_CONTENT_MAX] = '\0';
    LxmfMsg m;
    int trunc = lxmf_reply_build(&m, dest, src, 0.0, body);
    assert(trunc == 0);
    assert(m.content_len == LXMF_REPLY_CONTENT_MAX);
    /* the packed message fits a single outbox slot (<= 383) for the ceiling */
    uint8_t sig[LXMF_SIG_LEN]; fill_sig(sig);
    uint8_t wire[512]; size_t wlen = 0;
    assert(lxmf_encode_packed(&m, sig, wire, sizeof(wire), &wlen) == LXMF_OK);
    assert(wlen <= 383);
}

/* TLORA-LXMF C5 — OPPORTUNISTIC WIRE INTEROP (strip on send / prepend on recv).
 *
 * A real client (Retichat/Sideband) delivering opportunistically puts
 * source(16)+sig(64)+msgpack on the wire, WITHOUT the leading destination hash
 * (LXMessage.py:633-634); the receiver reconstructs the full-packed message by
 * prepending its own delivery-destination hash (LXMRouter.py:1930-1934) before
 * parsing at the fixed offsets. These tests pin both halves against the pure
 * codec helpers the RNS layer uses. */

/* The Retichat case: a wire payload that never carried a dest, plus the
 * receiver's own dest, reconstructs and parses to the right title/content. */
static void test_opportunistic_ingest_prepends_dest() {
    uint8_t dest[16], src[16];
    for (int i = 0; i < 16; i++) { dest[i] = (uint8_t)(0xC0 + i); src[i] = (uint8_t)(0x30 + i); }

    /* Build the sender's full-packed bytes, then STRIP the dest to get exactly
     * what a real client transmits: source(16)+sig(64)+msgpack, no dest. */
    LxmfMsg m;
    assert(lxmf_reply_build(&m, dest, src, 1699999999.0, "from a real client") == 0);
    uint8_t sig[LXMF_SIG_LEN]; fill_sig(sig);
    uint8_t packed[512]; size_t plen = 0;
    assert(lxmf_encode_packed(&m, sig, packed, sizeof(packed), &plen) == LXMF_OK);

    const uint8_t *wire = NULL; size_t wire_len = 0;
    assert(lxmf_wire_strip_dest(packed, plen, &wire, &wire_len) == LXMF_OK);
    assert(wire_len == plen - LXMF_HASH_LEN);
    assert(wire == packed + LXMF_HASH_LEN);
    /* the wire begins with the SOURCE hash, not the dest — the dest is gone */
    assert(memcmp(wire, src, 16) == 0);

    /* Receiver prepends its OWN delivery-destination hash (== the message's dest,
     * because the client addressed the packet to us) and parses. */
    uint8_t recon[LXMF_HASH_LEN + 383]; size_t rlen = 0;
    assert(lxmf_wire_prepend_dest(dest, wire, wire_len, recon, sizeof(recon), &rlen) == LXMF_OK);
    assert(rlen == wire_len + LXMF_HASH_LEN);
    assert(memcmp(recon, dest, 16) == 0);          /* dest restored at offset 0 */

    LxmfMsg out;
    assert(lxmf_parse(recon, rlen, &out) == LXMF_OK);
    assert(memcmp(out.dest_hash, dest, 16) == 0);
    assert(memcmp(out.source_hash, src, 16) == 0);
    assert(out.content_len == 18 && strcmp(out.content, "from a real client") == 0);
    assert(out.title_len == 0);
}

/* Device<->device stays self-consistent under the new wire: our encode, then
 * strip-on-send, then prepend-on-receive with the SAME dest, round-trips. This
 * is the C4 device<->device reply flow, now over the stripped opportunistic wire
 * instead of the old full-packed one. */
static void test_stripped_wire_roundtrip() {
    uint8_t dest[16], src[16];
    for (int i = 0; i < 16; i++) { dest[i] = (uint8_t)(0xA0 + i); src[i] = (uint8_t)(0xB0 + i); }

    LxmfMsg m;
    assert(lxmf_reply_build(&m, dest, src, 1700000000.0, "hello back") == 0);
    uint8_t sig[LXMF_SIG_LEN]; fill_sig(sig);
    uint8_t packed[512]; size_t plen = 0;
    assert(lxmf_encode_packed(&m, sig, packed, sizeof(packed), &plen) == LXMF_OK);

    const uint8_t *wire = NULL; size_t wire_len = 0;
    assert(lxmf_wire_strip_dest(packed, plen, &wire, &wire_len) == LXMF_OK);

    uint8_t recon[LXMF_HASH_LEN + 383]; size_t rlen = 0;
    assert(lxmf_wire_prepend_dest(dest, wire, wire_len, recon, sizeof(recon), &rlen) == LXMF_OK);
    /* the reconstruction is byte-identical to the original full-packed bytes */
    assert(rlen == plen);
    assert(memcmp(recon, packed, plen) == 0);

    LxmfMsg out;
    assert(lxmf_parse(recon, rlen, &out) == LXMF_OK);
    assert(memcmp(out.dest_hash, dest, 16) == 0);
    assert(memcmp(out.source_hash, src, 16) == 0);
    assert(out.content_len == 10 && strcmp(out.content, "hello back") == 0);
}

/* The bounds the RNS layer relies on: a scratch smaller than 16 + wire refuses
 * rather than overrunning, and a packed buffer shorter than a dest hash cannot
 * be stripped. */
static void test_wire_bounds_guarded() {
    uint8_t dest[16] = {0};
    uint8_t wire[8]; memset(wire, 0x11, sizeof(wire));
    uint8_t out[LXMF_HASH_LEN + 8];
    size_t olen = 0;
    /* exactly enough fits */
    assert(lxmf_wire_prepend_dest(dest, wire, sizeof(wire), out, sizeof(out), &olen) == LXMF_OK);
    assert(olen == LXMF_HASH_LEN + sizeof(wire));
    /* one byte short refuses */
    assert(lxmf_wire_prepend_dest(dest, wire, sizeof(wire), out, sizeof(out) - 1, &olen) == LXMF_TOO_LONG);
    /* a cap below the dest hash itself refuses (no underflow) */
    assert(lxmf_wire_prepend_dest(dest, wire, sizeof(wire), out, 4, &olen) == LXMF_TOO_LONG);

    /* strip: a buffer shorter than one dest hash cannot be stripped */
    const uint8_t *w = NULL; size_t wl = 0;
    uint8_t tiny[LXMF_HASH_LEN - 1] = {0};
    assert(lxmf_wire_strip_dest(tiny, sizeof(tiny), &w, &wl) == LXMF_TRUNCATED);
}

static void test_origin_set_lookup_clear() {
    LxmfOriginTable t; lxmf_origin_init(&t);
    uint8_t a[16], b[16], out[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)(0x10 + i); b[i] = (uint8_t)(0x90 + i); }

    /* unknown room misses */
    assert(lxmf_origin_lookup(&t, "work", out) == 0);

    lxmf_origin_set(&t, "work", a);
    assert(lxmf_origin_lookup(&t, "work", out) == 1);
    assert(memcmp(out, a, 16) == 0);
    /* a different room is unaffected */
    assert(lxmf_origin_lookup(&t, "play", out) == 0);

    /* re-set updates the sender in place (a later inbound changes the origin) */
    lxmf_origin_set(&t, "work", b);
    assert(lxmf_origin_lookup(&t, "work", out) == 1);
    assert(memcmp(out, b, 16) == 0);

    /* clear makes it miss (a seed.pager message reverted the origin) */
    lxmf_origin_clear(&t, "work");
    assert(lxmf_origin_lookup(&t, "work", out) == 0);
    /* clearing an absent room is a no-op */
    lxmf_origin_clear(&t, "nope");
}

static void test_origin_overcap_name_ignored() {
    LxmfOriginTable t; lxmf_origin_init(&t);
    uint8_t a[16]; memset(a, 0x55, 16);
    char toolong[LXMF_ORIGIN_ROOM_CAP + 8];
    memset(toolong, 'z', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    lxmf_origin_set(&t, toolong, a);           /* ignored, not truncated */
    uint8_t out[16];
    assert(lxmf_origin_lookup(&t, toolong, out) == 0);
}

static void test_origin_evicts_when_full() {
    LxmfOriginTable t; lxmf_origin_init(&t);
    uint8_t src[16]; memset(src, 0x01, 16);
    char name[8];
    /* fill every slot */
    for (int i = 0; i < LXMF_ORIGIN_SLOTS; i++) {
        snprintf(name, sizeof(name), "r%d", i);
        lxmf_origin_set(&t, name, src);
    }
    for (int i = 0; i < LXMF_ORIGIN_SLOTS; i++) {
        snprintf(name, sizeof(name), "r%d", i);
        uint8_t out[16];
        assert(lxmf_origin_lookup(&t, name, out) == 1);
    }
    /* one more evicts exactly one old entry, and the new one is findable */
    lxmf_origin_set(&t, "fresh", src);
    uint8_t out[16];
    assert(lxmf_origin_lookup(&t, "fresh", out) == 1);
    int survivors = 0;
    for (int i = 0; i < LXMF_ORIGIN_SLOTS; i++) {
        snprintf(name, sizeof(name), "r%d", i);
        if (lxmf_origin_lookup(&t, name, out)) survivors++;
    }
    assert(survivors == LXMF_ORIGIN_SLOTS - 1);   /* exactly one evicted */
}

int main() {
    test_build_short_roundtrip();
    test_build_truncates_on_utf8_boundary();
    test_build_exact_ceiling_not_truncated();
    test_opportunistic_ingest_prepends_dest();
    test_stripped_wire_roundtrip();
    test_wire_bounds_guarded();
    test_origin_set_lookup_clear();
    test_origin_overcap_name_ignored();
    test_origin_evicts_when_full();
    printf("lxmf reply tests: OK\n");
    return 0;
}
