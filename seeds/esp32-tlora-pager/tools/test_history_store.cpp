/*
 * Host tests for the persistent history ARCHIVE pure core (ticket
 * TLORA-HISTORY / C1): the append-only record codec, the newest-wins (ns,key)
 * index, and the streaming chunk-reassembly reader.
 *
 * Pure logic — no Arduino, no SD, no real clock: seq and stamp are injected by
 * the caller, byte I/O is a sink callback / plain buffer. Covers codec
 * round-trip (all fields, empty + max payload, a ~+32hex key, hostile keys
 * rejected), newest-record-wins resolution, namespace isolation, hostile input
 * (oversized payload, truncated record, garbage bytes leave neighbours intact),
 * the chunked-read reassembly of a record spanning a 4 KB boundary, and the
 * seq-reseed-on-mount that keeps newest-wins correct across a reboot.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "../src/micron/history_store.h"

/* --- a buffer sink for encode ---------------------------------------------- */

static int vec_write(void *ctx, const uint8_t *p, size_t n) {
    std::vector<uint8_t> *v = (std::vector<uint8_t> *)ctx;
    v->insert(v->end(), p, p + n);
    return (int)n;
}

static std::vector<uint8_t> encode_one(uint8_t ns, const char *key, uint32_t seq,
                                       uint32_t stamp, const uint8_t *pay,
                                       uint16_t len) {
    history_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.ns = ns;
    rec.seq = seq;
    rec.stamp_ms = stamp;
    rec.len = len;
    snprintf(rec.key, sizeof(rec.key), "%s", key);
    if (len) memcpy(rec.payload, pay, len);

    std::vector<uint8_t> out;
    history_sink sink;
    sink.write = vec_write;
    sink.ctx = &out;
    int ok = history_encode(&sink, &rec);
    assert(ok == 1);
    return out;
}

/* --- codec round-trip: all fields ------------------------------------------ */

static void test_codec_roundtrip(void) {
    const char *body = "hello archive";
    std::vector<uint8_t> enc =
        encode_one(7, "sched-0123456789ab", 0xDEADBEEF, 0x11223344,
                   (const uint8_t *)body, (uint16_t)strlen(body));

    /* On-disk size is exactly header + key + payload. */
    assert(enc.size() == (size_t)HISTORY_HDR_SIZE + strlen("sched-0123456789ab") +
                             strlen(body));

    history_record got;
    size_t consumed = 0;
    history_decode_status st = history_decode(enc.data(), enc.size(), &got, &consumed);
    assert(st == HISTORY_DECODE_OK);
    assert(consumed == enc.size());
    assert(got.ns == 7);
    assert(got.seq == 0xDEADBEEF);
    assert(got.stamp_ms == 0x11223344);
    assert(strcmp(got.key, "sched-0123456789ab") == 0);
    assert(got.len == strlen(body));
    assert(memcmp(got.payload, body, got.len) == 0);
    printf("  codec round-trip (all fields): OK\n");
}

/* --- codec: empty payload, max payload, ~+32hex key ------------------------ */

static void test_codec_bounds(void) {
    /* Empty payload. */
    std::vector<uint8_t> e0 = encode_one(0, "a", 1, 1, NULL, 0);
    history_record g0;
    size_t c0 = 0;
    assert(history_decode(e0.data(), e0.size(), &g0, &c0) == HISTORY_DECODE_OK);
    assert(g0.len == 0 && strcmp(g0.key, "a") == 0);

    /* Max payload. */
    uint8_t big[HISTORY_PAYLOAD_CAP];
    for (int i = 0; i < HISTORY_PAYLOAD_CAP; i++) big[i] = (uint8_t)(i & 0xFF);
    std::vector<uint8_t> em =
        encode_one(1, "big", 2, 2, big, (uint16_t)HISTORY_PAYLOAD_CAP);
    history_record gm;
    size_t cm = 0;
    assert(history_decode(em.data(), em.size(), &gm, &cm) == HISTORY_DECODE_OK);
    assert(gm.len == HISTORY_PAYLOAD_CAP);
    assert(memcmp(gm.payload, big, HISTORY_PAYLOAD_CAP) == 0);

    /* LXMF dedup identity: reserved '~' + 32 hex = 33 (the longest real key). */
    const char *lxmf = "~0123456789abcdef0123456789abcdef";
    assert(strlen(lxmf) == 33);
    std::vector<uint8_t> el = encode_one(1, lxmf, 3, 3, (const uint8_t *)"x", 1);
    history_record gl;
    size_t cl = 0;
    assert(history_decode(el.data(), el.size(), &gl, &cl) == HISTORY_DECODE_OK);
    assert(strcmp(gl.key, lxmf) == 0);
    printf("  codec bounds (empty / max payload / ~+32hex key): OK\n");
}

/* --- codec: hostile keys are refused at encode ----------------------------- */

static void test_codec_rejects_bad_keys(void) {
    history_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.ns = 0;
    rec.len = 1;
    rec.payload[0] = 'x';

    std::vector<uint8_t> out;
    history_sink sink;
    sink.write = vec_write;
    sink.ctx = &out;

    /* Control byte. */
    rec.key[0] = 'a'; rec.key[1] = 0x01; rec.key[2] = 'b'; rec.key[3] = '\0';
    assert(history_encode(&sink, &rec) == 0);
    /* Space. */
    snprintf(rec.key, sizeof(rec.key), "has space");
    assert(history_encode(&sink, &rec) == 0);
    /* High byte. */
    rec.key[0] = 'a'; rec.key[1] = (char)0xC3; rec.key[2] = (char)0xA9; rec.key[3] = '\0';
    assert(history_encode(&sink, &rec) == 0);
    /* Empty key. */
    rec.key[0] = '\0';
    assert(history_encode(&sink, &rec) == 0);
    /* An over-cap payload is refused by the encoder too (len > CAP). */
    snprintf(rec.key, sizeof(rec.key), "okkey");
    rec.len = HISTORY_PAYLOAD_CAP + 1;
    assert(history_encode(&sink, &rec) == 0);

    /* Nothing was ever emitted for any rejected record. */
    assert(out.empty());
    printf("  codec refuses control/space/high-byte/empty keys + over-cap payload: OK\n");
}

/* --- newest-record-wins resolution for a repeated (ns,key) ----------------- */

static void test_newest_wins(void) {
    history_index ix;
    history_index_init(&ix);

    /* Three appends of the same (ns,key) at growing seq/offset. */
    history_record r;
    memset(&r, 0, sizeof(r));
    r.ns = 0;
    snprintf(r.key, sizeof(r.key), "k1c.print");
    r.len = 3;

    memcpy(r.payload, "aaa", 3); r.seq = 10; assert(history_index_observe(&ix, &r, 100));
    memcpy(r.payload, "bbb", 3); r.seq = 20; assert(history_index_observe(&ix, &r, 200));
    memcpy(r.payload, "ccc", 3); r.seq = 30; assert(history_index_observe(&ix, &r, 300));

    const history_index_entry *e = history_index_get(&ix, 0, "k1c.print");
    assert(e && e->seq == 30 && e->offset == 300);

    /* An OLDER seq observed later must NOT supersede the winner (append-only
     * replay / out-of-order garbage cannot roll the resolution backwards). */
    r.seq = 15;
    history_index_observe(&ix, &r, 999);
    e = history_index_get(&ix, 0, "k1c.print");
    assert(e && e->seq == 30 && e->offset == 300);

    assert(ix.count == 1);
    printf("  newest-record-wins for a repeated (ns,key): OK\n");
}

/* --- namespace isolation: same key, two namespaces, two identities --------- */

static void test_namespace_isolation(void) {
    history_index ix;
    history_index_init(&ix);

    history_record r;
    memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "status");
    r.len = 8;

    r.ns = MICRON_NS_SYSTEM;  r.seq = 1; memcpy(r.payload, "SYS-BODY", 8);
    history_index_observe(&ix, &r, 10);
    r.ns = MICRON_NS_FOREIGN; r.seq = 2; memcpy(r.payload, "FOR-BODY", 8);
    history_index_observe(&ix, &r, 20);

    const history_index_entry *sysp = history_index_get(&ix, MICRON_NS_SYSTEM,  "status");
    const history_index_entry *forp = history_index_get(&ix, MICRON_NS_FOREIGN, "status");
    assert(sysp && forp && sysp != forp);
    assert(sysp->offset == 10 && sysp->ns == MICRON_NS_SYSTEM);
    assert(forp->offset == 20 && forp->ns == MICRON_NS_FOREIGN);
    assert(ix.count == 2);

    /* Churning the FOREIGN identity never moves the SYSTEM one. */
    r.ns = MICRON_NS_FOREIGN; r.seq = 99; memcpy(r.payload, "FOR-NEW!", 8);
    history_index_observe(&ix, &r, 30);
    sysp = history_index_get(&ix, MICRON_NS_SYSTEM, "status");
    assert(sysp && sysp->offset == 10);
    printf("  namespace isolation (same key, two identities): OK\n");
}

/* --- hostile input at decode ----------------------------------------------- */

static void test_hostile_decode(void) {
    /* Oversized paylen in a hand-built header must be rejected as BAD_HEADER
     * BEFORE any length is trusted (cut on the boundary — the cap gates it). */
    uint8_t h[HISTORY_HDR_SIZE];
    memset(h, 0, sizeof(h));
    h[0] = HISTORY_MAGIC0; h[1] = HISTORY_MAGIC1; h[2] = HISTORY_REC_VER;
    h[12] = 1;                                   /* keylen = 1 */
    history_put_u16(h + 13, HISTORY_PAYLOAD_CAP + 1);  /* paylen over the cap */
    size_t consumed = 12345;
    assert(history_decode(h, sizeof(h), NULL, &consumed) == HISTORY_DECODE_BAD_HEADER);
    assert(consumed == 0);

    /* A truncated record (valid header, body cut short) is NEED_MORE, not
     * corruption — the reader waits for the rest. */
    std::vector<uint8_t> full =
        encode_one(0, "trunc", 1, 1, (const uint8_t *)"payload", 7);
    assert(history_decode(full.data(), full.size() - 3, NULL, &consumed) ==
           HISTORY_DECODE_NEED_MORE);

    /* A single flipped payload byte fails the CRC. */
    std::vector<uint8_t> corrupt = full;
    corrupt[corrupt.size() - 1] ^= 0xFF;
    assert(history_decode(corrupt.data(), corrupt.size(), NULL, &consumed) ==
           HISTORY_DECODE_BAD_CRC);

    /* Garbage that is not a magic is BAD_MAGIC. */
    uint8_t junk[HISTORY_HDR_SIZE] = {0};
    junk[0] = 0x00; junk[1] = 0xFF;
    assert(history_decode(junk, sizeof(junk), NULL, &consumed) ==
           HISTORY_DECODE_BAD_MAGIC);
    printf("  hostile decode (oversize / truncated / crc / magic): OK\n");
}

/* --- chunked-read reassembly ------------------------------------------------ */

struct Collected {
    std::vector<uint8_t> ns;
    std::vector<uint32_t> seq;
    std::vector<uint32_t> offset;
    std::vector<std::vector<uint8_t>> payload;
    std::vector<std::string> key;
};

static void collect_cb(void *ctx, const history_record *rec, uint32_t offset) {
    Collected *c = (Collected *)ctx;
    c->ns.push_back(rec->ns);
    c->seq.push_back(rec->seq);
    c->offset.push_back(offset);
    c->payload.push_back(std::vector<uint8_t>(rec->payload, rec->payload + rec->len));
    c->key.push_back(std::string(rec->key));
}

/* Feed a byte stream to the reader in fixed-size chunks and collect records. */
static Collected drive_reader(const std::vector<uint8_t> &stream, size_t chunk) {
    history_reader r;
    history_reader_init(&r);
    Collected c;
    for (size_t off = 0; off < stream.size(); off += chunk) {
        size_t n = stream.size() - off;
        if (n > chunk) n = chunk;
        history_reader_push(&r, stream.data() + off, n, collect_cb, &c);
    }
    return c;
}

static void test_chunked_reassembly(void) {
    /* Build a stream of three records with distinct, large-ish payloads so at
     * least one record is guaranteed to straddle small chunk boundaries. */
    std::vector<uint8_t> pa(300, 0xA1), pb(500, 0xB2), pc(50, 0xC3);
    std::vector<uint8_t> stream;
    uint32_t off_a = (uint32_t)stream.size();
    {
        std::vector<uint8_t> e = encode_one(0, "reca", 100, 1, pa.data(), (uint16_t)pa.size());
        stream.insert(stream.end(), e.begin(), e.end());
    }
    uint32_t off_b = (uint32_t)stream.size();
    {
        std::vector<uint8_t> e = encode_one(1, "recb", 200, 2, pb.data(), (uint16_t)pb.size());
        stream.insert(stream.end(), e.begin(), e.end());
    }
    uint32_t off_c = (uint32_t)stream.size();
    {
        std::vector<uint8_t> e = encode_one(0, "recc", 300, 3, pc.data(), (uint16_t)pc.size());
        stream.insert(stream.end(), e.begin(), e.end());
    }

    /* Drive with many awkward chunk sizes, including 1 (worst case), a size that
     * lands mid-record, and a size larger than one record. */
    const size_t chunks[] = {1, 2, 7, 13, 64, 200, 511, 512, 513, 1024, stream.size()};
    for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
        Collected c = drive_reader(stream, chunks[ci] ? chunks[ci] : 1);
        assert(c.seq.size() == 3);
        assert(c.seq[0] == 100 && c.seq[1] == 200 && c.seq[2] == 300);
        assert(c.offset[0] == off_a && c.offset[1] == off_b && c.offset[2] == off_c);
        assert(c.key[0] == "reca" && c.key[1] == "recb" && c.key[2] == "recc");
        assert(c.payload[0] == pa && c.payload[1] == pb && c.payload[2] == pc);
    }
    printf("  chunked reassembly across every boundary (incl. 1-byte): OK\n");
}

/* --- hostile bytes between records: neighbours stay intact ------------------ */

static void test_reader_resync_keeps_neighbours(void) {
    std::vector<uint8_t> ra(20, 0x11), rc(20, 0x33);
    std::vector<uint8_t> ea = encode_one(0, "left",  10, 1, ra.data(), (uint16_t)ra.size());
    std::vector<uint8_t> ec = encode_one(0, "right", 30, 3, rc.data(), (uint16_t)rc.size());

    /* left | <garbage> | right — garbage that never forms a valid record. */
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), ea.begin(), ea.end());
    for (int i = 0; i < 37; i++) stream.push_back((uint8_t)(0xE0 ^ i));  /* junk */
    uint32_t off_right = (uint32_t)stream.size();
    stream.insert(stream.end(), ec.begin(), ec.end());

    /* A truncated record tacked on the end must not swallow anything before it. */
    stream.insert(stream.end(), ec.begin(), ec.begin() + 5);  /* partial tail */

    for (size_t chunk : {(size_t)1, (size_t)8, (size_t)200, stream.size()}) {
        Collected c = drive_reader(stream, chunk);
        /* Exactly the two real records survive, in order, at the right offsets.
         * The garbage between them is skipped and the truncated tail is held
         * back (NEED_MORE), never mis-emitted. */
        assert(c.seq.size() == 2);
        assert(c.seq[0] == 10 && c.seq[1] == 30);
        assert(c.offset[0] == 0 && c.offset[1] == off_right);
        assert(c.key[0] == "left" && c.key[1] == "right");
        assert(c.payload[0] == ra && c.payload[1] == rc);
    }
    printf("  reader resyncs past garbage, neighbours intact: OK\n");
}

/* --- newest-wins survives a reboot: seq is re-seeded from the archive ------- */

/* Callback: accumulate the max stored seq exactly as history_seed_seq does. */
static void seedseq_cb(void *ctx, const history_record *rec, uint32_t offset) {
    (void)offset;
    history_seq_scan_observe((history_seq_scan *)ctx, rec);
}

/* Resolve the newest record for (ns,key) exactly as history_read_newest's
 * history_newest_cb does on device: highest seq wins, equal seq -> later. */
struct NewestResolve {
    uint8_t        ns;
    const char    *key;
    history_record best;
    bool           found;
};
static void newest_resolve_cb(void *ctx, const history_record *rec, uint32_t offset) {
    (void)offset;
    NewestResolve *n = (NewestResolve *)ctx;
    if (rec->ns != n->ns) return;
    if (strcmp(rec->key, n->key) != 0) return;
    if (!n->found || rec->seq >= n->best.seq) {
        n->best = *rec;
        n->found = true;
    }
}

/* Drive the pure reader over a full byte archive and resolve newest (ns,key). */
static NewestResolve resolve_newest(const std::vector<uint8_t> &archive,
                                    uint8_t ns, const char *key) {
    history_reader r;
    history_reader_init(&r);
    NewestResolve n;
    n.ns = ns;
    n.key = key;
    n.found = false;
    memset(&n.best, 0, sizeof(n.best));
    history_reader_push(&r, archive.data(), archive.size(),
                        newest_resolve_cb, &n);
    return n;
}

static void test_seq_reseed_survives_reboot(void) {
    /* Pre-reboot boot session: the write task owns ONE monotonic counter that
     * starts at 0. Append five records for (SYSTEM,"k"), building the on-disk
     * byte stream exactly as the write task would (seq assigned by the writer). */
    uint32_t seq = 0;                     /* == g_hist_seq, fresh boot */
    std::vector<uint8_t> archive;
    for (int i = 1; i <= 5; i++) {
        char body[16];
        snprintf(body, sizeof(body), "old-%d", i);
        uint32_t s = ++seq;               /* the writer's ++g_hist_seq */
        std::vector<uint8_t> e =
            encode_one(MICRON_NS_SYSTEM, "k", s, 1000u + (uint32_t)i,
                       (const uint8_t *)body, (uint16_t)strlen(body));
        archive.insert(archive.end(), e.begin(), e.end());
    }
    assert(seq == 5);

    /* --- REBOOT --- : g_hist_seq resets to 0, the RAM index is gone. A naive
     * fresh append would get seq=1 and lose to the seq=5 pre-reboot record. */
    seq = 0;

    /* The I1 fix, driven through the pure core: one scan of the archive re-seeds
     * the counter to the maximum stored seq (this is history_seed_seq's logic). */
    history_seq_scan sc;
    history_seq_scan_init(&sc);
    {
        history_reader r;
        history_reader_init(&r);
        history_reader_push(&r, archive.data(), archive.size(), seedseq_cb, &sc);
    }
    assert(sc.any && sc.max_seq == 5);
    seq = history_seq_scan_result(&sc);   /* g_hist_seq = max stored seq (5) */

    /* Fresh post-reboot append for the SAME (ns,key): seq = ++counter = 6. */
    uint32_t fresh_seq = ++seq;
    assert(fresh_seq == 6);
    std::vector<uint8_t> ef =
        encode_one(MICRON_NS_SYSTEM, "k", fresh_seq, 2000,
                   (const uint8_t *)"FRESH", 5);
    archive.insert(archive.end(), ef.begin(), ef.end());

    /* history_read_newest(SYSTEM,"k") must return the FRESH payload, not the
     * higher-seq-looking pre-reboot one. */
    NewestResolve got = resolve_newest(archive, MICRON_NS_SYSTEM, "k");
    assert(got.found);
    assert(got.best.seq == 6);
    assert(got.best.len == 5 && memcmp(got.best.payload, "FRESH", 5) == 0);

    /* Contrast: WITHOUT the reseed the counter would be 0 and the fresh append
     * would carry seq=1 — proving the reseed is load-bearing, not incidental. */
    std::vector<uint8_t> naive(archive.begin(),
                               archive.end() - (std::ptrdiff_t)ef.size());
    std::vector<uint8_t> ef_naive =
        encode_one(MICRON_NS_SYSTEM, "k", 1, 2000,
                   (const uint8_t *)"FRESH", 5);
    naive.insert(naive.end(), ef_naive.begin(), ef_naive.end());
    NewestResolve lost = resolve_newest(naive, MICRON_NS_SYSTEM, "k");
    assert(lost.found && lost.best.seq == 5 &&
           memcmp(lost.best.payload, "old-5", 5) == 0);

    printf("  seq reseed on mount: fresh post-reboot append wins (max=%u -> "
           "fresh seq=%u); naive seq=1 would lose to pre-reboot seq=5: OK\n",
           sc.max_seq, fresh_seq);
}

int main(void) {
    test_codec_roundtrip();
    test_codec_bounds();
    test_codec_rejects_bad_keys();
    test_newest_wins();
    test_namespace_isolation();
    test_hostile_decode();
    test_chunked_reassembly();
    test_reader_resync_keeps_neighbours();
    test_seq_reseed_survives_reboot();
    return 0;
}
