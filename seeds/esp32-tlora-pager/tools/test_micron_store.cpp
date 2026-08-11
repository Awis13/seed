/*
 * Host tests for the micron system-layer page STORE (ticket C4): an eight-slot
 * store with strict-ASCII keys, namespace isolation by delivery account, sticky
 * upsert, free -> expired -> oldest eviction, and stable wheel-paging order.
 *
 * Pure logic — no Arduino, no radio, no board, no real clock: "now" is injected
 * as now_ms so expiry is deterministic. The security property under test is that
 * a page stored under one namespace can NEVER be read or paged as another
 * namespace's page, even with an identical key string.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/micron/micron_store.h"

/* A page source of `n` bytes, all the same printable char, into `buf`. */
static void fill_src(char *buf, size_t n, char c) {
    for (size_t i = 0; i < n; i++) buf[i] = c;
}

/* --- key validation -------------------------------------------------------- */

static void test_key_validation(void) {
    /* Empty rejected. */
    assert(!micron_store_key_valid(""));
    assert(!micron_store_key_valid(NULL));

    /* Exactly 33 accepted; 34 rejected (reject, never truncate). */
    char k33[] = "012345678901234567890123456789012";   /* 33 */
    char k34[] = "0123456789012345678901234567890123";  /* 34 */
    assert(strlen(k33) == 33 && micron_store_key_valid(k33));
    assert(strlen(k34) == 34 && !micron_store_key_valid(k34));

    /* Real backend keys the unified store must hold without collision. */
    assert(micron_store_key_valid("sched-0123456789ab"));  /* 18: sched-<12hex> */
    assert(micron_store_key_valid("hook-0123456789ab"));   /* 17: hook-<12hex>  */
    /* LXMF dedup identity: reserved '~' + 32 hex = 33. */
    assert(micron_store_key_valid("~0123456789abcdef0123456789abcdef"));

    /* Ordinary short keys the pager already uses. */
    assert(micron_store_key_valid("hermes-chat"));
    assert(micron_store_key_valid("k1c.print"));
    assert(micron_store_key_valid("a"));

    /* Non-printable / high / space rejected. */
    assert(!micron_store_key_valid("has space"));
    char ctrl[] = {'a', 0x01, 'b', 0};
    assert(!micron_store_key_valid(ctrl));
    char del[] = {'a', 0x7F, 0};
    assert(!micron_store_key_valid(del));
    char high[] = {'a', (char)0xC3, (char)0xA9, 0};  /* UTF-8 'e-acute' */
    assert(!micron_store_key_valid(high));

    printf("  key validation: OK\n");
}

/* --- a rejected store mutates nothing --------------------------------------- */

static void test_reject_does_not_corrupt(void) {
    micron_store st;
    micron_store_init(&st);

    /* Fill all eight slots with valid system pages. */
    for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "sys%d", i);
        char body[4] = {(char)('A' + i), '!', 0, 0};
        micron_put_result r = micron_store_put(&st, MICRON_NS_SYSTEM, key,
                                               body, 2, 0, 1000);
        assert(r.status == MICRON_PUT_INSERTED);
    }

    /* Snapshot the raw store. */
    micron_store snap = st;

    /* A hostile key (34 bytes — one over the cap — and one with a control byte)
     * must be refused and leave every byte of the store identical. */
    char bad_src[64];
    fill_src(bad_src, sizeof(bad_src), 'X');
    micron_put_result r1 = micron_store_put(&st, MICRON_NS_SYSTEM,
                                            "0123456789012345678901234567890123",
                                            bad_src, 64, 0, 2000);
    assert(r1.status == MICRON_PUT_REJECTED && r1.slot == -1);

    char ctrl_key[] = {'b', 'a', 'd', 0x02, 0};
    micron_put_result r2 = micron_store_put(&st, MICRON_NS_FOREIGN, ctrl_key,
                                            bad_src, 64, 0, 2000);
    assert(r2.status == MICRON_PUT_REJECTED && r2.slot == -1);

    /* Byte-identical: not one neighbouring slot moved. */
    assert(memcmp(&snap, &st, sizeof(st)) == 0);

    printf("  rejected store leaves all slots byte-identical: OK\n");
}

/* --- upsert replaces content and keeps position (sticky) -------------------- */

static void test_upsert_sticky(void) {
    micron_store st;
    micron_store_init(&st);

    micron_store_put(&st, MICRON_NS_SYSTEM, "alpha", "one", 3, 0, 100);
    micron_store_put(&st, MICRON_NS_SYSTEM, "beta",  "two", 3, 0, 200);
    micron_store_put(&st, MICRON_NS_SYSTEM, "gamma", "three", 5, 0, 300);

    /* Order is insertion order: alpha, beta, gamma. */
    assert(micron_store_ns_count(&st, MICRON_NS_SYSTEM) == 3);
    assert(strcmp(micron_store_ns_at(&st, MICRON_NS_SYSTEM, 0)->key, "alpha") == 0);
    assert(strcmp(micron_store_ns_at(&st, MICRON_NS_SYSTEM, 1)->key, "beta") == 0);
    assert(strcmp(micron_store_ns_at(&st, MICRON_NS_SYSTEM, 2)->key, "gamma") == 0);

    const micron_slot *beta_before = micron_store_get(&st, MICRON_NS_SYSTEM, "beta");
    uint32_t beta_seq = beta_before->seq;

    /* Upsert beta with new, longer content much later. */
    micron_put_result r = micron_store_put(&st, MICRON_NS_SYSTEM, "beta",
                                           "TWO-UPDATED", 11, 0, 9999);
    assert(r.status == MICRON_PUT_UPDATED);

    /* Content replaced in place... */
    const micron_slot *beta_after = micron_store_get(&st, MICRON_NS_SYSTEM, "beta");
    assert(beta_after->len == 11);
    assert(memcmp(beta_after->src, "TWO-UPDATED", 11) == 0);
    /* ...seq unchanged, count unchanged, and it did NOT jump in wheel order. */
    assert(beta_after->seq == beta_seq);
    assert(micron_store_ns_count(&st, MICRON_NS_SYSTEM) == 3);
    assert(strcmp(micron_store_ns_at(&st, MICRON_NS_SYSTEM, 1)->key, "beta") == 0);

    printf("  upsert replaces content, keeps sticky position: OK\n");
}

/* --- namespace isolation: same key, two namespaces, two slots --------------- */

static void test_namespace_isolation(void) {
    micron_store st;
    micron_store_init(&st);

    /* Identical key string under two different delivery accounts. */
    micron_store_put(&st, MICRON_NS_SYSTEM,  "status", "SYS-BODY", 8, 0, 100);
    micron_store_put(&st, MICRON_NS_FOREIGN, "status", "FOR-BODY", 8, 0, 200);

    /* Two distinct slots. */
    const micron_slot *sysp = micron_store_get(&st, MICRON_NS_SYSTEM,  "status");
    const micron_slot *forp = micron_store_get(&st, MICRON_NS_FOREIGN, "status");
    assert(sysp && forp && sysp != forp);
    assert(memcmp(sysp->src, "SYS-BODY", 8) == 0);
    assert(memcmp(forp->src, "FOR-BODY", 8) == 0);

    /* SECURITY: the foreign page can never be reached through the system
     * namespace, nor the system page through the foreign one. There is exactly
     * one page in each namespace and it is the right one. */
    assert(micron_store_ns_count(&st, MICRON_NS_SYSTEM)  == 1);
    assert(micron_store_ns_count(&st, MICRON_NS_FOREIGN) == 1);
    assert(micron_store_ns_at(&st, MICRON_NS_SYSTEM, 0) == sysp);
    assert(micron_store_ns_at(&st, MICRON_NS_FOREIGN, 0) == forp);

    /* Overwriting the foreign page (upsert) must not touch the system slot. */
    micron_slot snap_sys = *sysp;
    micron_store_put(&st, MICRON_NS_FOREIGN, "status", "FOR-HOSTILE", 11, 0, 300);
    const micron_slot *sys_after = micron_store_get(&st, MICRON_NS_SYSTEM, "status");
    assert(memcmp(&snap_sys, sys_after, sizeof(micron_slot)) == 0);

    /* A foreign store can never displace a system page even when it evicts:
     * fill the FOREIGN namespace to capacity and keep storing — the lone system
     * page survives because eviction only ever reuses a slot the pick returns,
     * and a full store still keeps the system page as the newest survivor is
     * not the point; the point is that a system lookup keeps working. */
    for (int i = 0; i < MICRON_STORE_SLOTS + 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "f%d", i);
        micron_store_put(&st, MICRON_NS_FOREIGN, key, "x", 1, 0,
                         (uint32_t)(400 + i));
    }
    /* The store is only 8 slots, so the system page MAY be evicted by foreign
     * churn — that is expected (namespace is a qualifier, not a reservation).
     * What must ALWAYS hold: whatever a system lookup returns is a system page,
     * never a foreign one. */
    for (int o = 0; o < micron_store_ns_count(&st, MICRON_NS_SYSTEM); o++)
        assert(micron_store_ns_at(&st, MICRON_NS_SYSTEM, o)->ns == MICRON_NS_SYSTEM);
    for (int o = 0; o < micron_store_ns_count(&st, MICRON_NS_FOREIGN); o++)
        assert(micron_store_ns_at(&st, MICRON_NS_FOREIGN, o)->ns == MICRON_NS_FOREIGN);
    /* And "status" resolved in SYSTEM is never the FOREIGN body. */
    const micron_slot *sres = micron_store_get(&st, MICRON_NS_SYSTEM, "status");
    if (sres) assert(sres->ns == MICRON_NS_SYSTEM);

    printf("  namespace isolation (same key, no cross-retrieval): OK\n");
}

/* --- eviction: free -> expired -> oldest ----------------------------------- */

static void test_eviction_prefers_free(void) {
    micron_store st;
    micron_store_init(&st);
    /* Seven of eight full; a new page must take the one free slot, evicting
     * nothing. */
    for (int i = 0; i < MICRON_STORE_SLOTS - 1; i++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        micron_store_put(&st, MICRON_NS_SYSTEM, key, "b", 1, 0,
                         (uint32_t)(100 + i));
    }
    micron_put_result r = micron_store_put(&st, MICRON_NS_SYSTEM, "new", "b", 1,
                                           0, 5000);
    assert(r.status == MICRON_PUT_INSERTED && r.evicted == 0);
    assert(micron_store_ns_count(&st, MICRON_NS_SYSTEM) == MICRON_STORE_SLOTS);
    printf("  eviction prefers a free slot (no eviction): OK\n");
}

static void test_eviction_prefers_expired(void) {
    micron_store st;
    micron_store_init(&st);

    /* Fill all eight. Slot "p3" gets a short ttl; the rest never expire. p0 is
     * the OLDEST by stored_ms. */
    for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        uint32_t ttl = (i == 3) ? 50 : 0;  /* only p3 can expire */
        micron_store_put(&st, MICRON_NS_SYSTEM, key, "b", 1, ttl,
                         (uint32_t)(100 + i));
    }

    /* now_ms far enough that p3 (stored at 103, ttl 50) is expired but nothing
     * else is (they never expire). A new page must evict the EXPIRED p3, NOT
     * the older p0. */
    micron_put_result r = micron_store_put(&st, MICRON_NS_SYSTEM, "fresh", "b",
                                           1, 0, 1000);
    assert(r.status == MICRON_PUT_INSERTED);
    assert(r.evicted == 1 && strcmp(r.evicted_key, "p3") == 0);
    /* p0 (older) is still present; p3 is gone. */
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "p0") != NULL);
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "p3") == NULL);
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "fresh") != NULL);
    printf("  eviction prefers the oldest expired slot over an older live one: OK\n");
}

static void test_eviction_oldest_when_none_expired(void) {
    micron_store st;
    micron_store_init(&st);

    /* Eight pages, none with a ttl (nothing can expire), stored at strictly
     * increasing now_ms. p0 is the oldest. */
    for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        micron_store_put(&st, MICRON_NS_SYSTEM, key, "b", 1, 0,
                         (uint32_t)(1000 + i * 10));
    }
    micron_put_result r = micron_store_put(&st, MICRON_NS_SYSTEM, "ninth", "b",
                                           1, 0, 9000);
    assert(r.status == MICRON_PUT_INSERTED);
    assert(r.evicted == 1 && strcmp(r.evicted_key, "p0") == 0);
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "p0") == NULL);
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "p1") != NULL);
    assert(micron_store_get(&st, MICRON_NS_SYSTEM, "ninth") != NULL);
    printf("  eviction picks the oldest when none expired: OK\n");
}

/* --- source cap: an over-long page is cut on a code-point boundary ---------- */

static void test_src_cap(void) {
    micron_store st;
    micron_store_init(&st);

    /* A source longer than the cap is truncated to the cap. */
    char big[MICRON_STORE_SRC_CAP + 100];
    fill_src(big, sizeof(big), 'Z');
    micron_store_put(&st, MICRON_NS_SYSTEM, "big", big, sizeof(big), 0, 100);
    const micron_slot *s = micron_store_get(&st, MICRON_NS_SYSTEM, "big");
    assert(s && s->len == MICRON_STORE_SRC_CAP);

    /* A cut that would land inside a UTF-8 sequence backs off to a whole code
     * point: place a 2-byte sequence straddling the cap boundary. */
    char u[MICRON_STORE_SRC_CAP + 4];
    fill_src(u, sizeof(u), 'a');
    u[MICRON_STORE_SRC_CAP - 1] = (char)0xC3;  /* lead of a 2-byte sequence */
    u[MICRON_STORE_SRC_CAP]     = (char)0xA9;  /* continuation (past the cap) */
    micron_store_put(&st, MICRON_NS_SYSTEM, "utf", u, sizeof(u), 0, 200);
    const micron_slot *su = micron_store_get(&st, MICRON_NS_SYSTEM, "utf");
    /* The dangling lead byte was dropped, so len is one under the cap. */
    assert(su && su->len == MICRON_STORE_SRC_CAP - 1);
    assert((unsigned char)su->src[su->len - 1] == 'a');
    printf("  source cap cuts on a code-point boundary: OK\n");
}

/* --- expiry predicate is deterministic under injected now_ms ---------------- */

static void test_expiry_predicate(void) {
    micron_store st;
    micron_store_init(&st);
    micron_store_put(&st, MICRON_NS_SYSTEM, "ttl", "b", 1, 100, 1000);
    const micron_slot *s = micron_store_get(&st, MICRON_NS_SYSTEM, "ttl");
    assert(!micron_slot_expired(s, 1000));   /* age 0 */
    assert(!micron_slot_expired(s, 1099));   /* age 99 < 100 */
    assert(micron_slot_expired(s, 1100));    /* age 100 >= 100 */
    assert(micron_slot_expired(s, 5000));    /* well past */

    /* ttl 0 never expires. */
    micron_store_put(&st, MICRON_NS_SYSTEM, "forever", "b", 1, 0, 1000);
    const micron_slot *f = micron_store_get(&st, MICRON_NS_SYSTEM, "forever");
    assert(!micron_slot_expired(f, 0xFFFFFFFFu));
    printf("  expiry predicate deterministic under injected now_ms: OK\n");
}

int main(void) {
    test_key_validation();
    test_reject_does_not_corrupt();
    test_upsert_sticky();
    test_namespace_isolation();
    test_eviction_prefers_free();
    test_eviction_prefers_expired();
    test_eviction_oldest_when_none_expired();
    test_src_cap();
    test_expiry_predicate();
    return 0;
}
