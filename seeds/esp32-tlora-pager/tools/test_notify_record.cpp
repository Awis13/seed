/*
 * Host tests for the notify CARD <-> archive RECORD codec and the archive-index
 * boot-restore ordering (ticket TLORA-HISTORY / C3).
 *
 * Pure logic — no Arduino, no SD, no clock. Covers:
 *   - notify_rec encode/decode round-trip: all fields, unread/ttl preserved,
 *     options are NOT part of the record (there is no field for them);
 *   - bounding: an over-long body and an embedded NUL are truncated to the field
 *     cap, and the whole blob never exceeds NOTIFY_REC_MAX (< 512);
 *   - hostile/short/wrong-version blobs are refused, never overrun;
 *   - the archive-index newest-first walk (history_index_ns_at) that the boot
 *     restorer drives, incl. newest-wins on an updated card (unread flips);
 *   - namespace isolation NOTIFY vs SYSTEM: the same key under two namespaces is
 *     two distinct identities, neither surfaces under the other's walk;
 *   - graceful empty: an empty index yields nothing.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "../src/micron/notify_record.h"
#include "../src/micron/history_store.h"

/* --- codec round-trip: all fields, unread/ttl preserved -------------------- */

static void test_roundtrip(void) {
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 0xDEADBEEF;
    r.ttl_s = 3600;
    r.created_epoch = 1765432100u;
    r.event_id.epoch_hi = UINT64_C(0x12345678ABCDEF01);
    r.event_id.epoch_lo = UINT64_C(0x0FEDCBA987654321);
    r.event_id.counter = 77;
    r.event_distinct = 1;
    r.level = 2;          /* crit */
    r.unread = 1;
    snprintf(r.source, sizeof(r.source), "home-rig");
    snprintf(r.title, sizeof(r.title), "RAID degraded");
    snprintf(r.body, sizeof(r.body), "disk 3 of 4 in pool tank went offline");
    snprintf(r.key, sizeof(r.key), "raid-tank");

    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > 0 && n <= NOTIFY_REC_MAX);
    assert(NOTIFY_REC_MAX < 512);   /* fits HISTORY_PAYLOAD_CAP with margin */

    notify_rec o;
    assert(notify_rec_decode(buf, n, &o) == 1);
    assert(o.id == r.id);
    assert(o.ttl_s == r.ttl_s);
    assert(o.created_epoch == r.created_epoch);
    assert(notify_event_id_equal(&o.event_id, &r.event_id));
    assert(o.event_distinct == 1);
    assert(o.level == r.level);
    assert(o.unread == 1);
    assert(strcmp(o.source, "home-rig") == 0);
    assert(strcmp(o.title, "RAID degraded") == 0);
    assert(strcmp(o.body, "disk 3 of 4 in pool tank went offline") == 0);
    assert(strcmp(o.key, "raid-tank") == 0);
    /* The record type has NO options/chosen member at all (see notify_record.h):
     * the reply options are ephemeral UI and are structurally un-persistable — a
     * historical card round-trips as a plain notification. */
}

static void test_v1_backward_compatibility(void) {
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 42;
    r.ttl_s = 60;
    r.created_epoch = 1765432100u;
    r.level = 1;
    r.unread = 1;
    snprintf(r.title, sizeof(r.title), "legacy");

    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > NOTIFY_REC_HEAD);

    notify_rec out;
    assert(notify_rec_decode(buf, n, &out) == 1);
    assert(out.id == 42);
    assert(!notify_event_id_valid(&out.event_id));
    assert(strcmp(out.title, "legacy") == 0);
}

static void test_event_identity_and_reservation(void) {
    NotifyEventReservation first, rebooted;
    assert(notify_event_reservation_plan(0, 65536, &first));
    assert(first.first == 1 && first.limit == 65536);
    assert(notify_event_reservation_plan(first.limit, 65536, &rebooted));
    assert(rebooted.first == 65537 && rebooted.limit == 131072);
    assert(!notify_event_reservation_plan(UINT64_MAX - 1, 2, &rebooted));

    NotifyEventId next = {11, 22, 65535}, issued = {};
    assert(notify_event_ram_take(&next, 65536, &issued));
    assert(issued.counter == 65535 && next.counter == 65536);
    assert(notify_event_ram_take(&next, 65536, &issued));
    assert(issued.counter == 65536 && next.counter == 65537);
    assert(!notify_event_ram_take(&next, 65536, &issued));
    assert(!notify_event_id_valid(&issued));

    /* Pristine NVS and every partial/corrupt tuple rotate to a fresh epoch.
     * Only a complete typed tuple with a nonzero reserved high-water may
     * continue the existing epoch. */
    assert(notify_event_epoch_must_rotate(0, 0, 0, 0, 0, 0, 0, 0, 0));
    assert(notify_event_epoch_must_rotate(1, 1, 1, 1, 1, 1, 11, 22, 0));
    assert(notify_event_epoch_must_rotate(1, 1, 2, 1, 1, 0, 11, 22, 0));
    assert(notify_event_epoch_must_rotate(1, 1, 2, 1, 0, 1, 11, 22, 65536));
    assert(!notify_event_epoch_must_rotate(1, 1, 2, 1, 1, 1, 11, 22, 0));
    assert(!notify_event_epoch_must_rotate(1, 1, 2, 1, 1, 1, 11, 22, 65536));

    /* Numeric ring ids and client keys may repeat after archive/NVS loss. The
     * independently persisted epoch makes the new event distinct. */
    NotifyEventId old_event = {
        UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222), 7
    };
    NotifyEventId same_event = old_event;
    NotifyEventId after_nvs_loss = {
        UINT64_C(0x3333333333333333), UINT64_C(0x4444444444444444), 7
    };
    char old_hex[NOTIFY_EVENT_HEX_CAP];
    assert(notify_event_id_format(&old_event, old_hex, sizeof(old_hex)));
    assert(notify_event_id_equal(&old_event, &same_event));
    assert(!notify_event_id_equal(&old_event, &after_nvs_loss));
    assert(notify_event_origin_matches(9, "hermes-chat", old_hex,
                                       9, "hermes-chat", &same_event));
    assert(!notify_event_origin_matches(9, "hermes-chat", old_hex,
                                        9, "hermes-chat", &after_nvs_loss));

    char akey[NOTIFY_ARCHIVE_EVENTKEY_CAP], bkey[NOTIFY_ARCHIVE_EVENTKEY_CAP];
    const char *a = notify_rec_archive_event_key(
        "hermes-chat", 9, &old_event, true, akey, sizeof(akey));
    const char *b = notify_rec_archive_event_key(
        "hermes-chat", 10, &after_nvs_loss, true, bkey, sizeof(bkey));
    assert(strcmp(a, b) != 0); /* same logical door, distinct archived events */
    assert(strlen(a) == 32 && strlen(b) == 32); /* injective 192-bit base64url */

    char source_only_a[NOTIFY_ARCHIVE_EVENTKEY_CAP];
    char source_only_b[NOTIFY_ARCHIVE_EVENTKEY_CAP];
    assert(strcmp(notify_rec_archive_event_key(
                      "opencode-pager", 9, &old_event, true,
                      source_only_a, sizeof(source_only_a)),
                  notify_rec_archive_event_key(
                      "opencode-pager", 10, &after_nvs_loss, true,
                      source_only_b, sizeof(source_only_b))) != 0);
    NotifyEventId no_event = {};
    char fail_safe[NOTIFY_ARCHIVE_EVENTKEY_CAP];
    assert(strcmp(notify_rec_archive_event_key(
                      "opencode-pager", 99, &no_event, true,
                      fail_safe, sizeof(fail_safe)), "#99") == 0);
    assert(!notify_rec_key_replaces("hermes-chat"));
    assert(notify_rec_key_replaces("raid-tank"));
}

/* --- unread flip survives: the acked state is what round-trips ------------- */

static void test_unread_and_empty_fields(void) {
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 12;
    r.ttl_s = 0;                 /* never expires */
    r.unread = 0;                /* acked */
    snprintf(r.title, sizeof(r.title), "hi");
    /* source/body/key empty */

    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > 0);
    notify_rec o;
    assert(notify_rec_decode(buf, n, &o) == 1);
    assert(o.id == 12);
    assert(o.ttl_s == 0);
    assert(o.unread == 0);       /* read state preserved across the reboot */
    assert(o.source[0] == '\0');
    assert(o.body[0] == '\0');
    assert(o.key[0] == '\0');
    assert(strcmp(o.title, "hi") == 0);
}

/* --- bounding: over-long body + embedded NUL, blob stays <= NOTIFY_REC_MAX -- */

static void test_bounding(void) {
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 1;
    /* fill every text field to the brim (memset would leave no NUL, so set the
     * last byte NUL to keep them valid C strings at their cap). */
    memset(r.source, 'S', sizeof(r.source) - 1);
    memset(r.title,  'T', sizeof(r.title) - 1);
    memset(r.body,   'B', sizeof(r.body) - 1);
    memset(r.key,    'K', sizeof(r.key) - 1);

    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > 0 && n <= NOTIFY_REC_MAX);
    notify_rec o;
    assert(notify_rec_decode(buf, n, &o) == 1);
    assert(strlen(o.source) == NR_SOURCE_CAP - 1);
    assert(strlen(o.title)  == NR_TITLE_CAP - 1);
    assert(strlen(o.body)   == NR_BODY_CAP - 1);
    assert(strlen(o.key)    == NR_KEY_CAP - 1);

    /* An embedded NUL ends that field's string at encode (C-string semantics). */
    notify_rec r2;
    memset(&r2, 0, sizeof(r2));
    r2.id = 2;
    snprintf(r2.title, sizeof(r2.title), "keep");
    memcpy(r2.body, "AB\0CD", 5);   /* NUL after "AB" */
    size_t n2 = notify_rec_encode(&r2, buf, sizeof(buf));
    assert(n2 > 0);
    notify_rec o2;
    assert(notify_rec_decode(buf, n2, &o2) == 1);
    assert(strcmp(o2.body, "AB") == 0);   /* truncated at the NUL */
}

/* --- hostile / short / wrong-version blobs are refused, never overrun ------ */

static void test_hostile(void) {
    notify_rec o;
    /* empty / short */
    assert(notify_rec_decode((const uint8_t *)"", 0, &o) == 0);
    uint8_t tiny[3] = {NOTIFY_REC_VER, 1, 2};
    assert(notify_rec_decode(tiny, sizeof(tiny), &o) == 0);

    /* a valid record, then corrupt the version byte */
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 5;
    snprintf(r.title, sizeof(r.title), "x");
    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > 0);
    uint8_t bad = buf[0];
    buf[0] = (uint8_t)(bad + 1);
    assert(notify_rec_decode(buf, n, &o) == 0);   /* wrong version refused */
    buf[0] = bad;

    /* truncate the tail (a length prefix now runs past the buffer) */
    assert(notify_rec_decode(buf, n - 1, &o) == 0);

    /* A tagged identity extension is all-or-nothing; a torn suffix is corrupt. */
    buf[n] = NOTIFY_REC_EVENT_TAG;
    assert(notify_rec_decode(buf, n + 1, &o) == 0);

    /* a lying title length that overruns: hand-build head + title_len = 200 with
     * no bytes behind it. head is 15 bytes, then source_len(0), then title_len. */
    uint8_t evil[NOTIFY_REC_HEAD + 3];
    memset(evil, 0, sizeof(evil));
    evil[0] = NOTIFY_REC_VER;
    evil[NOTIFY_REC_HEAD + 0] = 0;     /* source_len = 0 */
    evil[NOTIFY_REC_HEAD + 1] = 200;   /* title_len = 200, but no bytes follow */
    assert(notify_rec_decode(evil, sizeof(evil), &o) == 0);
}

/* --- archive-index newest-first walk + newest-wins on update --------------- */

static void obs(history_index *ix, uint8_t ns, const char *key, uint32_t seq,
                uint32_t off) {
    history_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.ns = ns;
    rec.seq = seq;
    snprintf(rec.key, sizeof(rec.key), "%s", key);
    history_index_observe(ix, &rec, off);
}

static void test_index_newest_first(void) {
    history_index ix;
    history_index_init(&ix);

    /* three notify cards appended oldest->newest (seq 10,20,30) */
    obs(&ix, MICRON_NS_NOTIFY, "card-a", 10, 100);
    obs(&ix, MICRON_NS_NOTIFY, "card-b", 20, 200);
    obs(&ix, MICRON_NS_NOTIFY, "card-c", 30, 300);

    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 3);

    /* newest-first: c (30), b (20), a (10) — the order the boot restorer fills */
    const history_index_entry *e0 = history_index_ns_at(&ix, MICRON_NS_NOTIFY, 0);
    const history_index_entry *e1 = history_index_ns_at(&ix, MICRON_NS_NOTIFY, 1);
    const history_index_entry *e2 = history_index_ns_at(&ix, MICRON_NS_NOTIFY, 2);
    assert(e0 && strcmp(e0->key, "card-c") == 0 && e0->offset == 300);
    assert(e1 && strcmp(e1->key, "card-b") == 0 && e1->offset == 200);
    assert(e2 && strcmp(e2->key, "card-a") == 0 && e2->offset == 100);
    assert(history_index_ns_at(&ix, MICRON_NS_NOTIFY, 3) == NULL);   /* past the last */

    /* UPDATE card-a (e.g. it was acked): a newer record supersedes it. The index
     * now points at the new offset, and card-a becomes the newest ordinal. This
     * is exactly how an acked card restores in its NEW (read) state. */
    obs(&ix, MICRON_NS_NOTIFY, "card-a", 40, 400);
    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 3);   /* still 3 identities */
    const history_index_entry *top = history_index_ns_at(&ix, MICRON_NS_NOTIFY, 0);
    assert(top && strcmp(top->key, "card-a") == 0 && top->offset == 400);
}

/* --- namespace isolation: NOTIFY vs SYSTEM, same key ----------------------- */

static void test_namespace_isolation(void) {
    history_index ix;
    history_index_init(&ix);

    /* the SAME key string under two namespaces = two distinct identities */
    obs(&ix, MICRON_NS_NOTIFY, "help", 10, 100);
    obs(&ix, MICRON_NS_SYSTEM, "help", 20, 200);

    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 1);
    assert(history_index_ns_count(&ix, MICRON_NS_SYSTEM) == 1);

    /* the NOTIFY walk sees only the NOTIFY "help" (offset 100), never SYSTEM's */
    const history_index_entry *nn = history_index_ns_at(&ix, MICRON_NS_NOTIFY, 0);
    assert(nn && nn->ns == MICRON_NS_NOTIFY && nn->offset == 100);
    assert(history_index_ns_at(&ix, MICRON_NS_NOTIFY, 1) == NULL);

    /* and the SYSTEM walk sees only SYSTEM's (offset 200) */
    const history_index_entry *ss = history_index_ns_at(&ix, MICRON_NS_SYSTEM, 0);
    assert(ss && ss->ns == MICRON_NS_SYSTEM && ss->offset == 200);

    /* keyed lookup honours the namespace too — no cross-surface */
    assert(history_index_get(&ix, MICRON_NS_NOTIFY, "help")->offset == 100);
    assert(history_index_get(&ix, MICRON_NS_SYSTEM, "help")->offset == 200);
    assert(history_index_get(&ix, MICRON_NS_FOREIGN, "help") == NULL);
}

/* --- archive-key derivation: client key vs synthetic id-key never collide ---
 *
 * The silent-data-loss edge this guards: a card with client key "5" and a keyless
 * card whose id is 5 must NOT land on one archive identity. The '#' sentinel on the
 * synthetic id-key keeps the two sources disjoint. We drive the exact derivation
 * the device uses (notify_rec_archive_key) and then a simulated restore proving
 * both survive with their real id/dedup key from the PAYLOAD, neither upserting. */
static void test_archive_key_no_collision(void) {
    char b1[NOTIFY_ARCHIVE_IDKEY_CAP], b2[NOTIFY_ARCHIVE_IDKEY_CAP];

    /* Card A: client dedup key "5", id 100 -> archived raw under "5". */
    const char *kA = notify_rec_archive_key("5", 100, b1, sizeof(b1));
    /* Card B: no dedup key, id 5 -> synthetic "#5", NOT "5". */
    const char *kB = notify_rec_archive_key("", 5, b2, sizeof(b2));

    assert(strcmp(kA, "5") == 0);
    assert(strcmp(kB, "#5") == 0);
    assert(strcmp(kA, kB) != 0);            /* the collision is gone */
    assert(micron_store_key_valid(kB));     /* the '#' key is a valid archive key */

    /* They upsert as two DISTINCT identities in the archive index, not one. */
    history_index ix;
    history_index_init(&ix);
    obs(&ix, MICRON_NS_NOTIFY, kA, 10, 100);
    obs(&ix, MICRON_NS_NOTIFY, kB, 20, 200);
    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 2);   /* both, not one */
    assert(history_index_get(&ix, MICRON_NS_NOTIFY, "5")->offset == 100);
    assert(history_index_get(&ix, MICRON_NS_NOTIFY, "#5")->offset == 200);

    /* Simulated restore: the id and dedup key come from the PAYLOAD, not the
     * archive key, so card B restores as id=5 with an EMPTY dedup key even though
     * its handle was "#5" — the archive key is just the upsert handle. */
    notify_rec rA;
    memset(&rA, 0, sizeof(rA));
    rA.id = 100;
    snprintf(rA.title, sizeof(rA.title), "A");
    snprintf(rA.key, sizeof(rA.key), "5");
    notify_rec rB;
    memset(&rB, 0, sizeof(rB));
    rB.id = 5;
    snprintf(rB.title, sizeof(rB.title), "B");
    /* rB.key stays empty */

    uint8_t buf[NOTIFY_REC_MAX];
    notify_rec o;
    size_t n = notify_rec_encode(&rA, buf, sizeof(buf));
    assert(n > 0 && notify_rec_decode(buf, n, &o) == 1);
    assert(o.id == 100 && strcmp(o.key, "5") == 0);   /* A intact */
    n = notify_rec_encode(&rB, buf, sizeof(buf));
    assert(n > 0 && notify_rec_decode(buf, n, &o) == 1);
    assert(o.id == 5 && o.key[0] == '\0');            /* B: real id 5, empty key, from payload */

    /* A client key that itself begins with the sentinel falls back to the id-key
     * rather than being used raw (never collides, never drops the card). */
    const char *kH = notify_rec_archive_key("#5", 77, b1, sizeof(b1));
    assert(strcmp(kH, "#77") == 0);

    /* Longest synthetic key ("#" + max uint32 decimal) fits the buffer/cap. */
    const char *kMax = notify_rec_archive_key("", 4294967295u, b1, sizeof(b1));
    assert(strcmp(kMax, "#4294967295") == 0);
    assert(strlen(kMax) == 11 && micron_store_key_valid(kMax));
}

/* --- delete tombstone: a deleted card stays deleted across a reboot ---------
 *
 * This drives the EXACT device path end to end on the host: records are
 * history_encode()d into one append-only archive buffer, the mount index is
 * built from a streaming scan of it (history_reader), then the boot restore is
 * replayed — newest identity first, decode the record at the resolved offset,
 * and apply notify_restore_from_archive()'s decision:
 *   - a tombstone (notify_rec_is_tombstone) is SKIPPED, so a deleted card does
 *     not come back;
 *   - a data record is restored.
 * Newest-wins is what makes this correct: the tombstone is appended AFTER the
 * card, so it outranks it; a later re-add outranks the tombstone in turn.
 */

/* A tiny stand-in for the device's append-only archive + its RAM index. */
struct MockArchive {
    std::vector<uint8_t> bytes;   /* the /hist.log image */
    history_index        ix;
    uint32_t             seq;     /* the writer's monotonic seq (g_hist_seq) */
};

static int mock_sink_write(void *ctx, const uint8_t *p, size_t n) {
    std::vector<uint8_t> *v = (std::vector<uint8_t> *)ctx;
    v->insert(v->end(), p, p + n);
    return (int)n;
}

/* Append one record (payload already built) and index it, exactly as the device
 * write task does: assign the next seq, capture the pre-append offset, encode,
 * then observe. */
static void mock_append(MockArchive &a, uint8_t ns, const char *key,
                        const uint8_t *payload, uint16_t len) {
    history_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.ns = ns;
    rec.seq = ++a.seq;
    snprintf(rec.key, sizeof(rec.key), "%s", key);
    rec.len = len;
    if (len) memcpy(rec.payload, payload, len);

    uint32_t off = (uint32_t)a.bytes.size();
    history_sink sink;
    sink.write = mock_sink_write;
    sink.ctx = &a.bytes;
    assert(history_encode(&sink, &rec) == 1);
    history_index_observe(&a.ix, &rec, off);
}

static void mock_append_card(MockArchive &a, const char *akey,
                             const notify_rec *r) {
    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_encode(r, buf, sizeof(buf));
    assert(n > 0);
    mock_append(a, MICRON_NS_NOTIFY, akey, buf, (uint16_t)n);
}

static void mock_append_tombstone(MockArchive &a, const char *akey) {
    uint8_t buf[NOTIFY_REC_MAX];
    size_t n = notify_rec_tombstone_encode(buf, sizeof(buf));
    assert(n == 1);
    mock_append(a, MICRON_NS_NOTIFY, akey, buf, (uint16_t)n);
}

/* Replay notify_restore_from_archive() over the mock: return the ids restored,
 * newest-first, tombstoned identities skipped. */
static std::vector<uint32_t> mock_restore(MockArchive &a) {
    std::vector<uint32_t> ids;
    for (int rank = 0;; rank++) {
        const history_index_entry *e =
            history_index_ns_at(&a.ix, MICRON_NS_NOTIFY, rank);
        if (!e) break;
        /* decode the single record that starts at the resolved offset */
        history_record rec;
        size_t consumed = 0;
        history_decode_status st = history_decode(a.bytes.data() + e->offset,
                                                  a.bytes.size() - e->offset,
                                                  &rec, &consumed);
        assert(st == HISTORY_DECODE_OK);
        if (notify_rec_is_tombstone(rec.payload, rec.len)) continue;  /* deleted */
        notify_rec nr;
        assert(notify_rec_decode(rec.payload, rec.len, &nr) == 1);
        ids.push_back(nr.id);
    }
    return ids;
}

static bool mock_has(const std::vector<uint32_t> &v, uint32_t id) {
    for (uint32_t x : v) if (x == id) return true;
    return false;
}

static void test_tombstone_codec(void) {
    /* The marker is a one-byte payload that is recognised as a tombstone, is NOT
     * a decodable notify_rec, and cannot be produced by a real card (whose first
     * byte is always NOTIFY_REC_VER). */
    uint8_t buf[NOTIFY_REC_MAX];
    assert(notify_rec_tombstone_encode(buf, sizeof(buf)) == 1);
    assert(buf[0] == NOTIFY_REC_TOMBSTONE);
    assert((unsigned)NOTIFY_REC_TOMBSTONE != (unsigned)NOTIFY_REC_VER);
    assert(notify_rec_is_tombstone(buf, 1));

    notify_rec o;
    assert(notify_rec_decode(buf, 1, &o) == 0);   /* not a card */

    /* A real card is never mistaken for a tombstone. */
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 9;
    snprintf(r.title, sizeof(r.title), "hi");
    size_t n = notify_rec_encode(&r, buf, sizeof(buf));
    assert(n > 0);
    assert(!notify_rec_is_tombstone(buf, n));

    assert(notify_rec_tombstone_encode(buf, 0) == 0);   /* buffer too small */
    assert(!notify_rec_is_tombstone(NULL, 1));
    assert(!notify_rec_is_tombstone(buf, 0));
}

static void test_delete_survives_reboot(void) {
    MockArchive a;
    a.bytes.clear();
    history_index_init(&a.ix);
    a.seq = 0;

    /* Two cards land: A (client key "raid", id 100) and B (keyless, id 5 ->
     * archive key "#5"). */
    notify_rec rA;
    memset(&rA, 0, sizeof(rA));
    rA.id = 100;
    snprintf(rA.title, sizeof(rA.title), "A");
    snprintf(rA.key, sizeof(rA.key), "raid");
    char kbufA[NOTIFY_ARCHIVE_IDKEY_CAP];
    const char *kA = notify_rec_archive_key(rA.key, rA.id, kbufA, sizeof(kbufA));

    notify_rec rB;
    memset(&rB, 0, sizeof(rB));
    rB.id = 5;
    snprintf(rB.title, sizeof(rB.title), "B");
    char kbufB[NOTIFY_ARCHIVE_IDKEY_CAP];
    const char *kB = notify_rec_archive_key(rB.key, rB.id, kbufB, sizeof(kbufB));

    mock_append_card(a, kA, &rA);
    mock_append_card(a, kB, &rB);

    /* Boot restore sees both. */
    std::vector<uint32_t> r1 = mock_restore(a);
    assert(r1.size() == 2);
    assert(mock_has(r1, 100) && mock_has(r1, 5));

    /* DELETE card A: a tombstone lands on A's OWN archive identity (kA). It is
     * appended after A, so newest-wins resolves kA to it. */
    mock_append_tombstone(a, kA);
    assert(history_index_get(&a.ix, MICRON_NS_NOTIFY, kA)->seq == a.seq);

    /* Reboot: A is skipped (the delete survived), B still restores. */
    std::vector<uint32_t> r2 = mock_restore(a);
    assert(r2.size() == 1);
    assert(!mock_has(r2, 100));   /* the tombstoned key is skipped */
    assert(mock_has(r2, 5));

    /* RE-ADD the same key later (a fresh card under "raid", id 200): its data
     * record outranks the tombstone (newest-wins again), so the card comes back. */
    notify_rec rA2;
    memset(&rA2, 0, sizeof(rA2));
    rA2.id = 200;
    snprintf(rA2.title, sizeof(rA2.title), "A2");
    snprintf(rA2.key, sizeof(rA2.key), "raid");
    mock_append_card(a, kA, &rA2);

    std::vector<uint32_t> r3 = mock_restore(a);
    assert(r3.size() == 2);
    assert(mock_has(r3, 200));    /* re-add overrode the tombstone */
    assert(mock_has(r3, 5));
    assert(!mock_has(r3, 100));   /* the old id stays gone; the key now holds 200 */
}

/* --- graceful empty: nothing indexed => nothing restored ------------------- */

static void test_graceful_empty(void) {
    history_index ix;
    history_index_init(&ix);
    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 0);
    assert(history_index_ns_at(&ix, MICRON_NS_NOTIFY, 0) == NULL);
}

int main(void) {
    test_roundtrip();
    test_v1_backward_compatibility();
    test_event_identity_and_reservation();
    test_unread_and_empty_fields();
    test_bounding();
    test_hostile();
    test_index_newest_first();
    test_namespace_isolation();
    test_archive_key_no_collision();
    test_tombstone_codec();
    test_delete_survives_reboot();
    test_graceful_empty();
    printf("notify record + restore tests: OK\n");
    return 0;
}
