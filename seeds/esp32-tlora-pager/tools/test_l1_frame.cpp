/*
 * test_l1_frame.cpp — host test for the MeshCore-LXMF receive rung's pure parts:
 *   - base64url decode (src/base64url.h): known answers, invalid alphabet,
 *     overflow, empty, impossible length, non-canonical pad bits, round-trip;
 *   - L1 frame parse (src/l1_frame.h): field extraction and every rejection;
 *   - the sequential byte reassembler: 2-part exact reassembly, duplicate,
 *     forward gap, n-change, overflow;
 *   - end-to-end: a REAL LXMF opportunistic wire (LXMF_FULL minus its dest hash),
 *     base64url'd and split into two "L1|" frames, reassembles to the exact bytes
 *     and — with the dest hash prepended, exactly as lxmf_ingest_wire() does —
 *     parses through the codec into the expected title/content.
 *
 * Pure, no Arduino/RTOS: compiles the SAME headers the firmware includes.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <string>

#include "../src/base64url.h"
#include "../src/l1_frame.h"
#include "../src/lxmf_codec.h"

#include "lxmf_vectors.inc"

/* Test-only canonical base64url encoder (zero pad bits), the inverse the gateway
 * fragmenter performs — used to build frames from raw bytes. */
static std::string b64url_encode(const uint8_t *d, size_t n) {
    static const char *A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | d[i];
        bits += 8;
        while (bits >= 6) { bits -= 6; s += A[(acc >> bits) & 0x3F]; }
    }
    if (bits > 0) s += A[(acc << (6 - bits)) & 0x3F];
    return s;
}

/* ---------------------------------------------------------------- base64url */

static void test_b64_known() {
    uint8_t out[64];
    size_t  n = 0;

    assert(b64url_decode("aGVsbG8", 7, out, sizeof(out), &n));
    assert(n == 5 && memcmp(out, "hello", 5) == 0);

    /* RFC 4648 test progression f/fo/foo/foob/fooba/foobar */
    struct { const char *b64; const char *raw; } v[] = {
        {"Zg", "f"}, {"Zm8", "fo"}, {"Zm9v", "foo"},
        {"Zm9vYg", "foob"}, {"Zm9vYmE", "fooba"}, {"Zm9vYmFy", "foobar"},
    };
    for (auto &e : v) {
        assert(b64url_decode(e.b64, strlen(e.b64), out, sizeof(out), &n));
        assert(n == strlen(e.raw) && memcmp(out, e.raw, n) == 0);
    }

    /* url-safe alphabet: bytes that force '-' (62) and '_' (63) */
    uint8_t hi[] = {0xFB, 0xEF, 0xBE};   /* -> "----" family; check round-trip below */
    std::string e = b64url_encode(hi, 3);
    assert(e.find('+') == std::string::npos && e.find('/') == std::string::npos);
    assert(b64url_decode(e.c_str(), e.size(), out, sizeof(out), &n));
    assert(n == 3 && memcmp(out, hi, 3) == 0);
}

static void test_b64_empty() {
    uint8_t out[4];
    size_t  n = 123;
    assert(b64url_decode("", 0, out, sizeof(out), &n));
    assert(n == 0);
}

static void test_b64_invalid_alphabet() {
    uint8_t out[16];
    size_t  n = 0;
    assert(!b64url_decode("aGVs=G8", 7, out, sizeof(out), &n));  /* '=' padding */
    assert(!b64url_decode("aGV bG8", 7, out, sizeof(out), &n));  /* space       */
    assert(!b64url_decode("aG|sbG8", 7, out, sizeof(out), &n));  /* pipe        */
    assert(!b64url_decode("aGVs+G8", 7, out, sizeof(out), &n));  /* '+' (std)   */
    assert(!b64url_decode("aGVs/G8", 7, out, sizeof(out), &n));  /* '/' (std)   */
}

static void test_b64_bad_length() {
    uint8_t out[16];
    size_t  n = 0;
    assert(!b64url_decode("a", 1, out, sizeof(out), &n));       /* len%4==1     */
    assert(!b64url_decode("aaaaa", 5, out, sizeof(out), &n));   /* len%4==1     */
}

static void test_b64_noncanonical_pad() {
    uint8_t out[16];
    size_t  n = 0;
    /* "Zg" (canonical 'f', pad bits zero) OK; "Zh" has nonzero pad bits. */
    assert(b64url_decode("Zg", 2, out, sizeof(out), &n) && n == 1 && out[0] == 'f');
    assert(!b64url_decode("Zh", 2, out, sizeof(out), &n));
}

static void test_b64_overflow() {
    uint8_t out[3];
    size_t  n = 0;
    /* 8 chars -> 6 bytes, but cap is 3 */
    assert(!b64url_decode("Zm9vYmFy", 8, out, sizeof(out), &n));
}

/* ---------------------------------------------------------------- l1_parse  */

static void test_l1_parse_valid() {
    uint8_t chunk[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F};
    std::string b = b64url_encode(chunk, sizeof(chunk));
    std::string frame = "L1|ab12|1|2|" + b;

    L1Frame f;
    assert(l1_parse(frame.c_str(), &f));
    assert(strcmp(f.mid, "ab12") == 0);
    assert(f.part == 1 && f.n_parts == 2);
    assert(f.chunk_len == sizeof(chunk) && memcmp(f.chunk, chunk, sizeof(chunk)) == 0);
}

static void test_l1_parse_rejects() {
    L1Frame f;
    assert(!l1_parse("P1|foo", &f));            /* wrong prefix               */
    assert(!l1_parse("L1||1|2|AA", &f));         /* empty mid                  */
    assert(!l1_parse("L1|abcdefgh|1|2|AA", &f)); /* mid too long (8 chars)     */
    assert(!l1_parse("L1|m|0|2|AA", &f));        /* i == 0                     */
    assert(!l1_parse("L1|m|1|0|AA", &f));        /* n == 0                     */
    assert(!l1_parse("L1|m|3|2|AA", &f));        /* i > n                      */
    assert(!l1_parse("L1|m|1|33|AA", &f));       /* n > L1_MAX_PARTS (32)      */
    assert(!l1_parse("L1|m|x|2|AA", &f));        /* non-decimal i              */
    assert(!l1_parse("L1|m|1|2", &f));           /* missing chunk separator    */
    assert(!l1_parse("L1|m|1|2|@@@", &f));       /* undecodable chunk          */

    /* n exactly at the ceiling parses; one over rejects. */
    assert(l1_parse("L1|m|32|32|AA", &f) && f.n_parts == 32 && f.part == 32);
}

/* ------------------------------------------------------------ reassembler   */

/* Split `wire` at `cut`, encode each half, feed both parts, assert exact bytes. */
static void test_reasm_two_parts() {
    const uint8_t wire[] = {
        1,2,3,4,5,6,7,8,9,10, 200,201,202,203,204,205, 0,0,0,255, 42
    };
    const size_t wlen = sizeof(wire);
    const size_t cut = 9;

    std::string b1 = b64url_encode(wire, cut);
    std::string b2 = b64url_encode(wire + cut, wlen - cut);
    std::string f1 = "L1|xy|1|2|" + b1;
    std::string f2 = "L1|xy|2|2|" + b2;

    L1Frame p1, p2;
    assert(l1_parse(f1.c_str(), &p1));
    assert(l1_parse(f2.c_str(), &p2));

    L1Reasm r;
    memset(&r, 0, sizeof(r));
    assert(l1_reasm_feed(&r, &p1) == 0);     /* part 1: more expected */
    assert(l1_reasm_feed(&r, &p2) == 1);     /* part 2: complete      */
    assert(r.len == wlen && memcmp(r.buf, wire, wlen) == 0);
}

static void test_reasm_duplicate() {
    uint8_t chunk[] = {0xAA, 0xBB};
    std::string b = b64url_encode(chunk, 2);
    std::string f1 = "L1|d|1|2|" + b;
    L1Frame p1;
    assert(l1_parse(f1.c_str(), &p1));

    L1Reasm r;
    memset(&r, 0, sizeof(r));
    assert(l1_reasm_feed(&r, &p1) == 0);
    uint16_t len_after_first = r.len;
    assert(l1_reasm_feed(&r, &p1) == 0);     /* duplicate part 1: ignored */
    assert(r.len == len_after_first);        /* not appended twice        */
}

static void test_reasm_gap_rejected() {
    /* n = 3, feed part 1 then part 3 (skipping 2): forward gap -> reject. */
    uint8_t c1[] = {1}, c3[] = {3};
    std::string f1 = "L1|g|1|3|" + b64url_encode(c1, 1);
    std::string f3 = "L1|g|3|3|" + b64url_encode(c3, 1);
    L1Frame p1, p3;
    assert(l1_parse(f1.c_str(), &p1) && l1_parse(f3.c_str(), &p3));

    L1Reasm r;
    memset(&r, 0, sizeof(r));
    assert(l1_reasm_feed(&r, &p1) == 0);
    assert(l1_reasm_feed(&r, &p3) == -1);
}

static void test_reasm_n_change_rejected() {
    uint8_t c[] = {9};
    std::string f1 = "L1|h|1|2|" + b64url_encode(c, 1);
    std::string f2 = "L1|h|2|3|" + b64url_encode(c, 1);   /* n jumps 2 -> 3 */
    L1Frame p1, p2;
    assert(l1_parse(f1.c_str(), &p1) && l1_parse(f2.c_str(), &p2));

    L1Reasm r;
    memset(&r, 0, sizeof(r));
    assert(l1_reasm_feed(&r, &p1) == 0);
    assert(l1_reasm_feed(&r, &p2) == -1);
}

static void test_reasm_overflow_rejected() {
    /* Hand-built frames (bypassing parse's 160-byte chunk cap) that together
     * exceed L1_REASM_MAX: 160-byte chunks, part after part, until the feed that
     * would overflow returns -1 rather than writing past the buffer. */
    L1Reasm r;
    memset(&r, 0, sizeof(r));
    int rc = 0;
    bool saw_reject = false;
    for (int i = 1; i <= L1_MAX_PARTS; i++) {
        L1Frame f;
        memset(&f, 0, sizeof(f));
        snprintf(f.mid, sizeof(f.mid), "o");
        f.part = i;
        f.n_parts = L1_MAX_PARTS;
        f.chunk_len = L1_CHUNK_DECODED_MAX;    /* 160 bytes each */
        memset(f.chunk, 0x5A, f.chunk_len);
        rc = l1_reasm_feed(&r, &f);
        if (rc == -1) { saw_reject = true; break; }
        assert(r.len <= L1_REASM_MAX);         /* never overran */
    }
    assert(saw_reject);                        /* 160*4 = 640 > 512 -> rejected */
}

/* ---------------------------------------------------------- end-to-end wire */

static void test_end_to_end_lxmf() {
    /* The MeshCore transport carries the OPPORTUNISTIC wire: full-packed minus the
     * leading 16-byte dest hash (what a real client puts on the wire). */
    const uint8_t *wire = LXMF_FULL + LXMF_HASH_LEN;
    const size_t   wlen = LXMF_FULL_len - LXMF_HASH_LEN;

    /* Split into two byte-halves, encode each, frame each. */
    const size_t cut = wlen / 2;
    std::string f1 = "L1|Z9|1|2|" + b64url_encode(wire, cut);
    std::string f2 = "L1|Z9|2|2|" + b64url_encode(wire + cut, wlen - cut);

    L1Frame p1, p2;
    assert(l1_parse(f1.c_str(), &p1));
    assert(l1_parse(f2.c_str(), &p2));

    L1Reasm r;
    memset(&r, 0, sizeof(r));
    assert(l1_reasm_feed(&r, &p1) == 0);
    assert(l1_reasm_feed(&r, &p2) == 1);
    assert(r.len == wlen && memcmp(r.buf, wire, wlen) == 0);

    /* Now do exactly what lxmf_ingest_wire() does: prepend our delivery dest hash
     * (here the vector's own dest, 00..0f) and parse the reconstructed message. */
    uint8_t dest[LXMF_HASH_LEN];
    for (int i = 0; i < LXMF_HASH_LEN; i++) dest[i] = (uint8_t)i;
    uint8_t recon[LXMF_HASH_LEN + L1_REASM_MAX];
    size_t  rlen = 0;
    assert(lxmf_wire_prepend_dest(dest, r.buf, r.len, recon, sizeof(recon), &rlen)
           == LXMF_OK);
    assert(rlen == LXMF_FULL_len && memcmp(recon, LXMF_FULL, LXMF_FULL_len) == 0);

    LxmfMsg m;
    assert(lxmf_parse(recon, rlen, &m) == LXMF_OK);
    assert(m.title_len == 10 && strcmp(m.title, "Title Here") == 0);
    assert(m.content_len == 20 && strcmp(m.content, "Hello, content body!") == 0);
}

int main() {
    test_b64_known();
    test_b64_empty();
    test_b64_invalid_alphabet();
    test_b64_bad_length();
    test_b64_noncanonical_pad();
    test_b64_overflow();

    test_l1_parse_valid();
    test_l1_parse_rejects();

    test_reasm_two_parts();
    test_reasm_duplicate();
    test_reasm_gap_rejected();
    test_reasm_n_change_rejected();
    test_reasm_overflow_rejected();

    test_end_to_end_lxmf();

    printf("all l1 frame tests passed\n");
    return 0;
}
