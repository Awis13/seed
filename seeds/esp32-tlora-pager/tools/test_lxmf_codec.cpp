/*
 * Host test for the pure LXMF codec in src/lxmf_codec.h.
 *
 * No firmware / FreeRTOS / SD / RNS / crypto. Exercises the parser against REAL
 * reference vectors (tools/lxmf_vectors.inc, generated with RNS.vendor.umsgpack
 * — the exact msgpack LXMF ships with), plus the crypto-free encoder and a set
 * of hostile-input cases. Assertions check extracted VALUES, not just "no crash".
 *
 * Cases:
 *   - a full real-format LXMF message parses (title/content/type/data/renderer/
 *     thread + meta sev/src/key/ttl all correct)
 *   - a plain message with NO custom fields parses OK (the Retichat case)
 *   - a 5-element payload (with stamp) is stripped to 4 and parses
 *   - an unknown/unhandled extra field is ignored, message still OK
 *   - round-trip encode -> parse preserves every field
 *   - truncated buffers -> LXMF_TRUNCATED, never an OOB read (ASAN/UBSAN on)
 *   - malformed msgpack -> LXMF_BAD_MSGPACK
 *   - wrong payload shape (not a 4/5-array) -> LXMF_BAD_SHAPE
 *   - oversized content -> LXMF_TOO_LONG (bounds beat cap: short buffer -> TRUNCATED)
 *   - the sign-injection contract: sign-input payload == packed payload
 *   - encoder overflow -> LXMF_TOO_LONG with the needed length reported
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../src/lxmf_codec.h"
#include "lxmf_vectors.inc"

static void test_full() {
    LxmfMsg m;
    LxmfReason r = lxmf_parse(LXMF_FULL, LXMF_FULL_len, &m);
    assert(r == LXMF_OK);

    /* header hashes come straight off the wire */
    for (int i = 0; i < 16; i++) assert(m.dest_hash[i] == (uint8_t)i);
    for (int i = 0; i < 16; i++) assert(m.source_hash[i] == (uint8_t)(16 + i));

    assert(fabs(m.timestamp - 1723382400.5) < 1e-6);

    assert(m.title_len == 10 && strcmp(m.title, "Title Here") == 0);
    assert(m.content_len == 20 && strcmp(m.content, "Hello, content body!") == 0);

    assert(m.has_type && m.type_len == 5 && strcmp(m.type, "alert") == 0);

    assert(m.has_data && m.data_len == 3);
    assert(m.data[0] == 0x01 && m.data[1] == 0x02 && m.data[2] == 0x03);

    assert(m.has_renderer && m.renderer == LXMF_RENDERER_MARKDOWN);

    assert(m.has_thread && m.thread_len == 4);
    assert(m.thread[0] == 0xDE && m.thread[1] == 0xAD &&
           m.thread[2] == 0xBE && m.thread[3] == 0xEF);

    assert(m.has_meta);
    assert(m.meta_has_sev && m.meta_sev == 7);
    assert(m.meta_has_ttl && m.meta_ttl == 3600);
    assert(strcmp(m.meta_src, "watchdog") == 0);
    assert(strcmp(m.meta_key, "disk-full") == 0);
}

static void test_plain() {
    /* Retichat case: fields = {} (empty map), no custom fields at all. */
    LxmfMsg m;
    LxmfReason r = lxmf_parse(LXMF_PLAIN, LXMF_PLAIN_len, &m);
    assert(r == LXMF_OK);
    assert(m.title_len == 0 && m.title[0] == '\0');
    assert(m.content_len == 10 && strcmp(m.content, "plain body") == 0);
    assert(!m.has_type && !m.has_data && !m.has_renderer && !m.has_thread && !m.has_meta);
}

static void test_stamp() {
    /* 5-element payload: the trailing stamp must be ignored, still OK. */
    LxmfMsg m;
    LxmfReason r = lxmf_parse(LXMF_STAMP, LXMF_STAMP_len, &m);
    assert(r == LXMF_OK);
    assert(m.content_len == 12 && strcmp(m.content, "stamped body") == 0);
    assert(m.has_renderer && m.renderer == LXMF_RENDERER_PLAIN);
    assert(!m.has_meta && !m.has_data && !m.has_type);
}

static void test_unknown_field() {
    /* Field 0x06 (IMAGE) is not one we handle -> ignored, message still OK. */
    LxmfMsg m;
    LxmfReason r = lxmf_parse(LXMF_UNKNOWN, LXMF_UNKNOWN_len, &m);
    assert(r == LXMF_OK);
    assert(m.content_len == 11 && strcmp(m.content, "has unknown") == 0);
    assert(m.has_type && strcmp(m.type, "info") == 0);
}

static void test_roundtrip() {
    LxmfMsg in;
    memset(&in, 0, sizeof(in));
    for (int i = 0; i < 16; i++) { in.dest_hash[i] = (uint8_t)(0xA0 + i); in.source_hash[i] = (uint8_t)(0xB0 + i); }
    in.timestamp = 1699999999.25;
    strcpy(in.title, "hi"); in.title_len = 2;
    strcpy(in.content, "round trip body"); in.content_len = 15;
    in.has_type = 1; strcpy(in.type, "notify"); in.type_len = 6;
    in.has_data = 1; in.data[0] = 0xCA; in.data[1] = 0xFE; in.data_len = 2;
    in.has_renderer = 1; in.renderer = LXMF_RENDERER_MICRON;
    in.has_thread = 1; in.thread[0] = 0x11; in.thread[1] = 0x22; in.thread_len = 2;
    in.has_meta = 1;
    in.meta_has_sev = 1; in.meta_sev = 3;
    in.meta_has_ttl = 1; in.meta_ttl = 42;
    strcpy(in.meta_src, "sensor"); in.meta_src_len = 6;
    strcpy(in.meta_key, "temp-hi"); in.meta_key_len = 7;

    uint8_t sig[LXMF_SIG_LEN];
    for (int i = 0; i < LXMF_SIG_LEN; i++) sig[i] = (uint8_t)(0x40 + i);

    uint8_t wire[512]; size_t wlen = 0;
    LxmfReason er = lxmf_encode_packed(&in, sig, wire, sizeof(wire), &wlen);
    assert(er == LXMF_OK);
    assert(wlen > LXMF_HEADER_LEN);

    /* header laid out exactly as the reference expects */
    assert(memcmp(wire, in.dest_hash, 16) == 0);
    assert(memcmp(wire + 16, in.source_hash, 16) == 0);
    assert(memcmp(wire + 32, sig, 64) == 0);

    LxmfMsg out;
    LxmfReason r = lxmf_parse(wire, wlen, &out);
    assert(r == LXMF_OK);
    assert(memcmp(out.dest_hash, in.dest_hash, 16) == 0);
    assert(memcmp(out.source_hash, in.source_hash, 16) == 0);
    assert(fabs(out.timestamp - in.timestamp) < 1e-6);
    assert(out.title_len == 2 && strcmp(out.title, "hi") == 0);
    assert(out.content_len == 15 && strcmp(out.content, "round trip body") == 0);
    assert(out.has_type && strcmp(out.type, "notify") == 0);
    assert(out.has_data && out.data_len == 2 && out.data[0] == 0xCA && out.data[1] == 0xFE);
    assert(out.has_renderer && out.renderer == LXMF_RENDERER_MICRON);
    assert(out.has_thread && out.thread_len == 2 && out.thread[0] == 0x11 && out.thread[1] == 0x22);
    assert(out.has_meta);
    assert(out.meta_sev == 3 && out.meta_ttl == 42);
    assert(strcmp(out.meta_src, "sensor") == 0);
    assert(strcmp(out.meta_key, "temp-hi") == 0);
}

static void test_truncation_fuzz() {
    /* Every prefix of a valid message must be handled without OOB; short of the
     * 96-byte header it is TRUNCATED, and no prefix ever returns OK by accident
     * except the exact full length. */
    for (size_t k = 0; k < LXMF_FULL_len; k++) {
        LxmfMsg m;
        LxmfReason r = lxmf_parse(LXMF_FULL, k, &m);
        assert(r != LXMF_OK);                 /* only the full buffer parses OK */
        if (k < LXMF_HEADER_LEN) assert(r == LXMF_TRUNCATED);
    }
    LxmfMsg m;
    assert(lxmf_parse(LXMF_FULL, LXMF_FULL_len, &m) == LXMF_OK);
    assert(lxmf_parse(NULL, 10, &m) == LXMF_TRUNCATED);
    assert(lxmf_parse(LXMF_FULL, 0, &m) == LXMF_TRUNCATED);
}

static void test_bad_msgpack() {
    /* 96-byte header + array(4) + float64 ts + a 0xc1 (never-used) byte where the
     * title should be -> malformed msgpack. */
    uint8_t buf[LXMF_HEADER_LEN + 32];
    memset(buf, 0, sizeof(buf));
    size_t o = LXMF_HEADER_LEN;
    buf[o++] = 0x94;                          /* fixarray(4) */
    buf[o++] = 0xcb;                          /* float64 */
    for (int i = 0; i < 8; i++) buf[o++] = 0x00;
    buf[o++] = 0xc1;                          /* reserved / never used */
    LxmfMsg m;
    LxmfReason r = lxmf_parse(buf, o, &m);
    assert(r == LXMF_BAD_MSGPACK);
}

static void test_bad_shape() {
    /* payload is a 3-element array -> BAD_SHAPE */
    uint8_t a[LXMF_HEADER_LEN + 1];
    memset(a, 0, sizeof(a));
    a[LXMF_HEADER_LEN] = 0x93;                /* fixarray(3) */
    LxmfMsg m;
    assert(lxmf_parse(a, sizeof(a), &m) == LXMF_BAD_SHAPE);

    /* payload is a map, not an array -> BAD_SHAPE */
    uint8_t b[LXMF_HEADER_LEN + 1];
    memset(b, 0, sizeof(b));
    b[LXMF_HEADER_LEN] = 0x80;                /* fixmap(0) */
    assert(lxmf_parse(b, sizeof(b), &m) == LXMF_BAD_SHAPE);
}

static void test_oversized_content() {
    /* content declared 400 bytes (all present) exceeds LXMF_CONTENT_CAP -> TOO_LONG,
     * cleanly, no OOB. */
    const size_t big = 400;
    uint8_t buf[LXMF_HEADER_LEN + 16 + 400];
    memset(buf, 0x41, sizeof(buf));
    size_t o = LXMF_HEADER_LEN;
    buf[o++] = 0x94;                          /* array(4) */
    buf[o++] = 0xcb; for (int i = 0; i < 8; i++) buf[o++] = 0x00;  /* ts */
    buf[o++] = 0xc4; buf[o++] = 0x00;         /* title bin8 len 0 */
    buf[o++] = 0xc5; buf[o++] = (uint8_t)(big >> 8); buf[o++] = (uint8_t)(big & 0xff); /* content bin16 400 */
    /* the 400 content bytes are already 0x41 filler in buf */
    o += big;
    LxmfMsg m;
    assert(lxmf_parse(buf, o, &m) == LXMF_TOO_LONG);

    /* content declares 65535 bytes but the buffer is short -> bounds beat cap:
     * TRUNCATED, and (ASAN) no read past the end. */
    uint8_t sbuf[LXMF_HEADER_LEN + 16];
    memset(sbuf, 0, sizeof(sbuf));
    size_t so = LXMF_HEADER_LEN;
    sbuf[so++] = 0x94;
    sbuf[so++] = 0xcb; for (int i = 0; i < 8; i++) sbuf[so++] = 0x00;
    sbuf[so++] = 0xc4; sbuf[so++] = 0x00;     /* title len 0 */
    sbuf[so++] = 0xc5; sbuf[so++] = 0xff; sbuf[so++] = 0xff;  /* content bin16 65535, no bytes follow */
    assert(lxmf_parse(sbuf, so, &m) == LXMF_TRUNCATED);
}

static void test_sign_contract() {
    /* The bytes the caller signs (sign-input, minus the 32-byte dest+source
     * prefix) must equal the payload the packed wire message carries after the
     * 96-byte header. That is the whole point of the split. */
    LxmfMsg in;
    memset(&in, 0, sizeof(in));
    for (int i = 0; i < 16; i++) { in.dest_hash[i] = (uint8_t)i; in.source_hash[i] = (uint8_t)(0x80 + i); }
    in.timestamp = 12345.0;
    strcpy(in.content, "sign me"); in.content_len = 7;

    uint8_t sinput[256]; size_t slen = 0;
    assert(lxmf_encode_sign_input(&in, sinput, sizeof(sinput), &slen) == LXMF_OK);
    assert(slen > 32);
    assert(memcmp(sinput, in.dest_hash, 16) == 0);
    assert(memcmp(sinput + 16, in.source_hash, 16) == 0);

    uint8_t sig[LXMF_SIG_LEN]; memset(sig, 0x5A, sizeof(sig));
    uint8_t wire[256]; size_t wlen = 0;
    assert(lxmf_encode_packed(&in, sig, wire, sizeof(wire), &wlen) == LXMF_OK);

    /* payload region matches byte-for-byte */
    assert(slen - 32 == wlen - LXMF_HEADER_LEN);
    assert(memcmp(sinput + 32, wire + LXMF_HEADER_LEN, slen - 32) == 0);
}

static void test_encode_overflow() {
    LxmfMsg in;
    memset(&in, 0, sizeof(in));
    in.timestamp = 1.0;
    strcpy(in.content, "too big for the tiny buffer"); in.content_len = 27;

    uint8_t tiny[8]; size_t need = 0;
    LxmfReason r = lxmf_encode_packed(&in, (const uint8_t *)"0123456789012345678901234567890123456789012345678901234567890123",
                                      tiny, sizeof(tiny), &need);
    assert(r == LXMF_TOO_LONG);
    assert(need > sizeof(tiny));              /* reports the length it needed */
}

/* Build a message: 96-byte zero header + the given raw msgpack payload bytes.
 * Returns the total length. `buf` must have room for LXMF_HEADER_LEN + plen. */
static size_t mk_msg(uint8_t *buf, const uint8_t *payload, size_t plen) {
    memset(buf, 0, LXMF_HEADER_LEN);
    memcpy(buf + LXMF_HEADER_LEN, payload, plen);
    return LXMF_HEADER_LEN + plen;
}

/* A well-formed float64-zero timestamp, as raw msgpack bytes. */
#define TS0 0xcb, 0, 0, 0, 0, 0, 0, 0, 0
/* An empty bin8 (len 0) — used as a valid title/content placeholder. */
#define BIN0 0xc4, 0x00

/*
 * str32 / bin32 length prefixes of 0xFFFFFFFF (and a mid 0x00100000) in the
 * title and content positions must be rejected (TRUNCATED here — the declared
 * bytes run off the end), never wrap-accepted. On a 32-bit size_t the old
 * `r->off + n <= r->len` check wrapped for n≈2^32 and wrongly passed; the
 * non-wrapping `n <= len - off` form rejects it by construction.
 */
static void test_hostile_str32_bin32_lengths() {
    uint8_t buf[LXMF_HEADER_LEN + 64];
    LxmfMsg m;

    /* title str32, len 0xFFFFFFFF */
    { const uint8_t p[] = { 0x94, TS0, 0xdb, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
    /* title bin32, len 0xFFFFFFFF */
    { const uint8_t p[] = { 0x94, TS0, 0xc6, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
    /* title str32, mid length 0x00100000 (1 MiB) — still short of the buffer */
    { const uint8_t p[] = { 0x94, TS0, 0xdb, 0x00, 0x10, 0x00, 0x00 };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
    /* content str32, len 0xFFFFFFFF (title consumed first as empty bin) */
    { const uint8_t p[] = { 0x94, TS0, BIN0, 0xdb, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
    /* content bin32, mid length 0x00100000 */
    { const uint8_t p[] = { 0x94, TS0, BIN0, 0xc6, 0x00, 0x10, 0x00, 0x00 };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
}

/*
 * map32 / array32 with a 0xFFFFFFFF element count must be rejected without a
 * wrap and without an OOB read: the huge count never allocates or advances past
 * the buffer — each iteration hits the end and stops.
 */
static void test_hostile_map32_array32_counts() {
    uint8_t buf[LXMF_HEADER_LEN + 64];
    LxmfMsg m;

    /* fields = map32 with count 0xFFFFFFFF, no pairs follow -> TRUNCATED, no wrap. */
    { const uint8_t p[] = { 0x94, TS0, BIN0, BIN0, 0xdf, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }

    /* payload itself is an array32 with count 0xFFFFFFFF -> not 4/5 -> BAD_SHAPE. */
    { const uint8_t p[] = { 0xdd, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_BAD_SHAPE); }

    /* array32 with count 0xFFFFFFFF as an unknown field's VALUE, skipped
     * generically -> TRUNCATED (loop hits the end), no wrap. */
    { const uint8_t p[] = { 0x94, TS0, BIN0, BIN0, 0x81, 0x63,   /* fixmap(1), key uint 0x63 */
                            0xdd, 0xFF, 0xFF, 0xFF, 0xFF };
      assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED); }
}

/*
 * Nesting deeper than LXMF_MAX_DEPTH (8) in the generic skip must trip the
 * recursion guard and return BAD_MSGPACK rather than blow the stack.
 */
static void test_hostile_deep_nesting() {
    uint8_t buf[LXMF_HEADER_LEN + 64];
    LxmfMsg m;
    const uint8_t head[] = { 0x94, TS0, BIN0, BIN0, 0x81, 0x63 }; /* fixmap(1) key 0x63 */
    uint8_t p[sizeof(head) + 40];
    size_t o = 0;
    memcpy(p, head, sizeof(head)); o = sizeof(head);
    for (int i = 0; i < 40; i++) p[o++] = 0x91;                   /* 40 nested fixarray(1) */
    assert(lxmf_parse(buf, mk_msg(buf, p, o), &m) == LXMF_BAD_MSGPACK);
}

/*
 * A field key that is not a uint (a str key) is not standard LXMF but must be
 * tolerated: key and value are generic-skipped and the message still parses OK
 * (balanced consume — no desync).
 */
static void test_hostile_str_field_key() {
    uint8_t buf[LXMF_HEADER_LEN + 64];
    LxmfMsg m;
    /* fields = fixmap(1): key = fixstr "x", value = nil -> skipped, OK. */
    const uint8_t p[] = { 0x94, TS0, BIN0, BIN0, 0x81, 0xa1, 'x', 0xc0 };
    LxmfReason r = lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m);
    assert(r == LXMF_OK);
    assert(m.title_len == 0 && m.content_len == 0);
    assert(!m.has_type && !m.has_data && !m.has_meta);
}

/*
 * ext32 with a huge declared length must be TRUNCATED, not a zero-length skip.
 * The old `(size_t)n + 1` wrapped to 0 on a 32-bit size_t for n=0xFFFFFFFF and
 * silently consumed nothing; the split type-byte/payload checks reject it.
 */
static void test_hostile_ext32_huge() {
    uint8_t buf[LXMF_HEADER_LEN + 64];
    LxmfMsg m;
    /* fields fixmap(1) key 0x63, value = ext32 len 0xFFFFFFFF + 1 type byte, no
     * payload -> type byte consumed, huge payload runs off the end -> TRUNCATED. */
    const uint8_t p[] = { 0x94, TS0, BIN0, BIN0, 0x81, 0x63,
                          0xc9, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    assert(lxmf_parse(buf, mk_msg(buf, p, sizeof(p)), &m) == LXMF_TRUNCATED);
}

int main() {
    test_full();
    test_plain();
    test_stamp();
    test_unknown_field();
    test_roundtrip();
    test_truncation_fuzz();
    test_bad_msgpack();
    test_bad_shape();
    test_oversized_content();
    test_sign_contract();
    test_encode_overflow();
    test_hostile_str32_bin32_lengths();
    test_hostile_map32_array32_counts();
    test_hostile_deep_nesting();
    test_hostile_str_field_key();
    test_hostile_ext32_huge();
    printf("all lxmf codec tests passed\n");
    return 0;
}
