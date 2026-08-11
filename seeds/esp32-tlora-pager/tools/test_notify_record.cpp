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

#include "../src/micron/notify_record.h"
#include "../src/micron/history_store.h"

/* --- codec round-trip: all fields, unread/ttl preserved -------------------- */

static void test_roundtrip(void) {
    notify_rec r;
    memset(&r, 0, sizeof(r));
    r.id = 0xDEADBEEF;
    r.ttl_s = 3600;
    r.created_epoch = 1765432100u;
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

/* --- graceful empty: nothing indexed => nothing restored ------------------- */

static void test_graceful_empty(void) {
    history_index ix;
    history_index_init(&ix);
    assert(history_index_ns_count(&ix, MICRON_NS_NOTIFY) == 0);
    assert(history_index_ns_at(&ix, MICRON_NS_NOTIFY, 0) == NULL);
}

int main(void) {
    test_roundtrip();
    test_unread_and_empty_fields();
    test_bounding();
    test_hostile();
    test_index_newest_first();
    test_namespace_isolation();
    test_archive_key_no_collision();
    test_graceful_empty();
    printf("notify record + restore tests: OK\n");
    return 0;
}
