/*
 * Host tests for the pure secret store in src/secret_store.h.
 *
 * The device half (secret_store.cpp) owns NVS and the filesystem; everything
 * that DECIDES what is migrated -- the {nvs_key <-> spiffs_path} mapping, the
 * value-size guard, the per-key migrate decision, and the migration DRIVER --
 * lives in the header and is exercised here with no Arduino, no NVS and no FS.
 *
 * The driver is not re-encoded: this test injects a FAKE key-value store and a
 * FAKE file source into SecretMigrateOps and calls the SAME secret_migrate_run()
 * the firmware calls, so the migrate decision proved here is the one that ships.
 *
 * Covered:
 *   - the mapping is well formed: 5 rows, NVS keys 1..15 chars and distinct,
 *     absolute SPIFFS paths, sentinel key within the NVS limit;
 *   - the migrate decision as a full truth table (sentinel / existing key /
 *     missing file);
 *   - byte-exact round-trip: the bytes on SPIFFS are the bytes in NVS, for every
 *     mapped secret, driven end to end through the fake store;
 *   - an existing NVS key is never overwritten;
 *   - an oversize file is rejected, not truncated;
 *   - the sentinel makes a second run a pure no-op (idempotent);
 *   - a transient put failure WITHHOLDS the sentinel and the next run retries
 *     only the missing key (the C1-review self-heal guard);
 *   - the DEVICE wrappers put/get/has/del (secret_store.cpp, compiled against
 *     the tools/host_arduino Preferences mock): put -> del -> get returns 0,
 *     and del of an absent key is a successful no-op (GW-TOKEN-401).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/secret_store.h"

/* --- a fake NVS key-value store + a fake SPIFFS, injected into the driver ---- */

#define FAKE_KV_MAX   16
#define FAKE_FILE_MAX 16

struct FakeEntry {
    char    key[SECRET_NVS_KEY_MAX + 1];
    uint8_t val[SECRET_VALUE_MAX];
    size_t  len;
    bool    used;
};

struct FakeFile {
    const char    *path;
    const uint8_t *data;
    size_t         len;      /* TRUE length, may exceed the driver's cap */
    bool           present;
};

struct FakeWorld {
    FakeEntry kv[FAKE_KV_MAX];
    FakeFile  files[FAKE_FILE_MAX];
    int       n_files;
    int       put_calls;     /* observability for the no-op assertions */
    /* Injected transient write failure: while fail_key matches and fail_times>0,
     * fake_put refuses the write (and consumes one fail_times) to model a flaky
     * NVS. Lets the test prove the sentinel is withheld and the retry recovers. */
    const char *fail_key;
    int         fail_times;
};

static FakeEntry *fake_find(FakeWorld *w, const char *key) {
    for (int i = 0; i < FAKE_KV_MAX; i++)
        if (w->kv[i].used && strcmp(w->kv[i].key, key) == 0) return &w->kv[i];
    return NULL;
}

static bool fake_has(void *ctx, const char *key) {
    FakeWorld *w = (FakeWorld *)ctx;
    return fake_find(w, key) != NULL;
}

static size_t fake_read_file(void *ctx, const char *path, uint8_t *buf,
                             size_t cap, bool *present) {
    FakeWorld *w = (FakeWorld *)ctx;
    if (present) *present = false;
    for (int i = 0; i < w->n_files; i++) {
        if (strcmp(w->files[i].path, path) != 0) continue;
        if (!w->files[i].present) return 0;
        if (present) *present = true;
        size_t copy = w->files[i].len < cap ? w->files[i].len : cap;
        if (w->files[i].data && copy) memcpy(buf, w->files[i].data, copy);
        return w->files[i].len;   /* the TRUE length, so oversize is visible */
    }
    return 0;
}

static bool fake_put(void *ctx, const char *key, const uint8_t *buf,
                     size_t len) {
    FakeWorld *w = (FakeWorld *)ctx;
    w->put_calls++;
    if (w->fail_key && w->fail_times > 0 && strcmp(key, w->fail_key) == 0) {
        w->fail_times--;
        return false;                 /* transient write failure */
    }
    if (len > SECRET_VALUE_MAX) return false;
    FakeEntry *e = fake_find(w, key);
    if (!e) {
        for (int i = 0; i < FAKE_KV_MAX; i++)
            if (!w->kv[i].used) { e = &w->kv[i]; break; }
    }
    if (!e) return false;
    e->used = true;
    snprintf(e->key, sizeof(e->key), "%s", key);
    if (buf && len) memcpy(e->val, buf, len);
    e->len = len;
    return true;
}

static void ops_bind(SecretMigrateOps *ops, FakeWorld *w) {
    ops->has = fake_has;
    ops->read_file = fake_read_file;
    ops->put = fake_put;
    ops->ctx = w;
}

/* --- 1. the mapping table is well formed ------------------------------------ */

static void test_mapping_shape(void) {
    size_t n = 0;
    const SecretMap *m = secret_map(&n);
    assert(n == 5);

    /* every key is a usable, bounded NVS key; every path is absolute */
    for (size_t i = 0; i < n; i++) {
        size_t klen = strlen(m[i].nvs_key);
        assert(klen >= 1 && klen <= (size_t)SECRET_NVS_KEY_MAX);
        assert(m[i].spiffs_path[0] == '/');
        /* keys are distinct from one another */
        for (size_t j = i + 1; j < n; j++)
            assert(strcmp(m[i].nvs_key, m[j].nvs_key) != 0);
    }

    /* the sentinel is itself a legal NVS key, and not one of the secrets */
    assert(strlen(SECRET_SENTINEL_KEY) >= 1);
    assert(strlen(SECRET_SENTINEL_KEY) <= (size_t)SECRET_NVS_KEY_MAX);
    for (size_t i = 0; i < n; i++)
        assert(strcmp(m[i].nvs_key, SECRET_SENTINEL_KEY) != 0);

    /* the exact mapping is the contract the five read sites depend on */
    assert(strcmp(m[0].nvs_key, "rns_id") == 0);
    assert(strcmp(m[0].spiffs_path, "/rns_identity.id") == 0);
    assert(strcmp(m[1].nvs_key, "mesh_id") == 0);
    assert(strcmp(m[1].spiffs_path, "/mesh_identity.id") == 0);
    assert(strcmp(m[2].nvs_key, "mesh_pair") == 0);
    assert(strcmp(m[2].spiffs_path, "/mesh_pair.json") == 0);
    assert(strcmp(m[3].nvs_key, "gw_tok") == 0);
    assert(strcmp(m[3].spiffs_path, "/gw_token.txt") == 0);
    assert(strcmp(m[4].nvs_key, "auth_tok") == 0);
    assert(strcmp(m[4].spiffs_path, "/auth_token.txt") == 0);
}

/* --- 2. the migrate decision as a full truth table -------------------------- */

static void test_should_copy_truth_table(void) {
    /* sentinel set => never copy, whatever else is true */
    assert(secret_should_copy(true,  false, true)  == false);
    assert(secret_should_copy(true,  true,  true)  == false);
    assert(secret_should_copy(true,  false, false) == false);

    /* no sentinel, key already present => never overwrite */
    assert(secret_should_copy(false, true,  true)  == false);
    assert(secret_should_copy(false, true,  false) == false);

    /* no sentinel, empty key, but no file => nothing to copy */
    assert(secret_should_copy(false, false, false) == false);

    /* the ONE case that copies: first run, empty key, file present */
    assert(secret_should_copy(false, false, true)  == true);
}

/* --- 2b. the value-size guard ----------------------------------------------- */

static void test_value_fits(void) {
    assert(secret_value_fits(0));
    assert(secret_value_fits(1));
    assert(secret_value_fits(SECRET_VALUE_MAX));
    assert(!secret_value_fits(SECRET_VALUE_MAX + 1));
    assert(!secret_value_fits((size_t)SECRET_VALUE_MAX + 1000));
}

/* --- 3. byte-exact round trip through the driver ---------------------------- */

/* Distinct payloads for the five secrets, sizes mirroring the real files. */
static void make_payload(int idx, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)((idx * 37 + i * 101 + 7) & 0xff);
}

static void test_byte_exact_migration(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* Sizes chosen near the real ones: rns 128, mesh_id 96, mesh_pair 200,
     * gw_tok 40, auth_tok 32 -- all within SECRET_VALUE_MAX. */
    const size_t sizes[5] = {128, 96, 200, 40, 32};
    static uint8_t payloads[5][SECRET_VALUE_MAX];
    for (size_t i = 0; i < n; i++) {
        make_payload((int)i, payloads[i], sizes[i]);
        w.files[w.n_files].path    = m[i].spiffs_path;
        w.files[w.n_files].data    = payloads[i];
        w.files[w.n_files].len     = sizes[i];
        w.files[w.n_files].present = true;
        w.n_files++;
    }

    SecretMigrateOps ops;
    ops_bind(&ops, &w);

    int copied = secret_migrate_run(&ops);
    assert(copied == 5);                       /* all five secrets migrated */

    /* the bytes in NVS are the bytes that were on SPIFFS, verbatim */
    for (size_t i = 0; i < n; i++) {
        FakeEntry *e = fake_find(&w, m[i].nvs_key);
        assert(e != NULL);
        assert(e->len == sizes[i]);
        assert(memcmp(e->val, payloads[i], sizes[i]) == 0);
    }

    /* the sentinel was set, so a second run copies nothing and writes nothing */
    assert(fake_has(&w, SECRET_SENTINEL_KEY));
    int before = w.put_calls;
    int again = secret_migrate_run(&ops);
    assert(again == 0);
    assert(w.put_calls == before);             /* pure no-op: not even a re-set */
}

/* --- 4. an existing NVS key is never overwritten ---------------------------- */

static void test_never_overwrite_existing(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* Pre-seed NVS with a DIFFERENT value for the first secret. */
    const uint8_t sacred[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    assert(fake_put(&w, m[0].nvs_key, sacred, sizeof(sacred)));

    /* And put a file on SPIFFS for that same secret (the tempting overwrite). */
    static uint8_t tempting[64];
    make_payload(99, tempting, sizeof(tempting));
    w.files[0].path = m[0].spiffs_path;
    w.files[0].data = tempting;
    w.files[0].len = sizeof(tempting);
    w.files[0].present = true;
    w.n_files = 1;

    SecretMigrateOps ops;
    ops_bind(&ops, &w);
    int copied = secret_migrate_run(&ops);
    assert(copied == 0);                       /* nothing else had a file */

    /* the pre-existing NVS value is intact -- the file did not clobber it */
    FakeEntry *e = fake_find(&w, m[0].nvs_key);
    assert(e != NULL);
    assert(e->len == sizeof(sacred));
    assert(memcmp(e->val, sacred, sizeof(sacred)) == 0);
}

/* --- 5. a missing file is skipped, a present one alongside it is copied ------ */

static void test_missing_file_skipped(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* Only the third secret has a file; the rest are absent. */
    static uint8_t only[50];
    make_payload(3, only, sizeof(only));
    w.files[0].path = m[2].spiffs_path;
    w.files[0].data = only;
    w.files[0].len = sizeof(only);
    w.files[0].present = true;
    w.n_files = 1;
    /* one path that exists in the table but is marked absent, to prove !present */
    w.files[1].path = m[0].spiffs_path;
    w.files[1].present = false;
    w.n_files = 2;

    SecretMigrateOps ops;
    ops_bind(&ops, &w);
    int copied = secret_migrate_run(&ops);
    assert(copied == 1);                        /* exactly the present one */
    assert(fake_has(&w, m[2].nvs_key));
    assert(!fake_has(&w, m[0].nvs_key));        /* absent file => not migrated */
    assert(!fake_has(&w, m[1].nvs_key));
}

/* --- 6. an oversize file is refused, not truncated -------------------------- */

static void test_oversize_rejected(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* A file one byte past the cap, reported present with its TRUE length. */
    static uint8_t big[SECRET_VALUE_MAX + 1];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i & 0xff);
    w.files[0].path = m[0].spiffs_path;
    w.files[0].data = big;
    w.files[0].len = SECRET_VALUE_MAX + 1;
    w.files[0].present = true;
    w.n_files = 1;

    SecretMigrateOps ops;
    ops_bind(&ops, &w);
    int copied = secret_migrate_run(&ops);
    assert(copied == 0);                        /* oversize: guarded out */
    assert(!fake_has(&w, m[0].nvs_key));        /* nothing stored, not a prefix */

    /* a value exactly at the cap is fine -- the boundary is inclusive */
    memset(&w, 0, sizeof(w));
    static uint8_t exact[SECRET_VALUE_MAX];
    for (size_t i = 0; i < sizeof(exact); i++) exact[i] = (uint8_t)((i * 3) & 0xff);
    w.files[0].path = m[0].spiffs_path;
    w.files[0].data = exact;
    w.files[0].len = SECRET_VALUE_MAX;
    w.files[0].present = true;
    w.n_files = 1;
    ops_bind(&ops, &w);
    assert(secret_migrate_run(&ops) == 1);
    FakeEntry *e = fake_find(&w, m[0].nvs_key);
    assert(e != NULL && e->len == SECRET_VALUE_MAX);
    assert(memcmp(e->val, exact, SECRET_VALUE_MAX) == 0);
}

/* --- 7. the sentinel short-circuits the whole migration --------------------- */

static void test_sentinel_short_circuits(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* Sentinel already set, and every secret has a file waiting. */
    const uint8_t one = 1;
    assert(fake_put(&w, SECRET_SENTINEL_KEY, &one, 1));
    static uint8_t data[5][32];
    for (size_t i = 0; i < n; i++) {
        make_payload((int)i + 10, data[i], sizeof(data[i]));
        w.files[w.n_files].path = m[i].spiffs_path;
        w.files[w.n_files].data = data[i];
        w.files[w.n_files].len = sizeof(data[i]);
        w.files[w.n_files].present = true;
        w.n_files++;
    }

    SecretMigrateOps ops;
    ops_bind(&ops, &w);
    int before = w.put_calls;
    int copied = secret_migrate_run(&ops);
    assert(copied == 0);                        /* sentinel => copy nothing */
    assert(w.put_calls == before);              /* and not a single write */
    for (size_t i = 0; i < n; i++)
        assert(!fake_has(&w, m[i].nvs_key));
}

/* --- 7b. a failed copy withholds the sentinel; the next run retries --------- */

static void test_put_failure_withholds_sentinel(void) {
    FakeWorld w;
    memset(&w, 0, sizeof(w));

    size_t n = 0;
    const SecretMap *m = secret_map(&n);

    /* Every secret has a file present. */
    static uint8_t data[5][40];
    const size_t sizes[5] = {40, 40, 40, 40, 40};
    for (size_t i = 0; i < n; i++) {
        make_payload((int)i + 20, data[i], sizes[i]);
        w.files[w.n_files].path    = m[i].spiffs_path;
        w.files[w.n_files].data    = data[i];
        w.files[w.n_files].len     = sizes[i];
        w.files[w.n_files].present = true;
        w.n_files++;
    }

    /* The third secret's write fails ONCE (a transient NVS error). */
    w.fail_key   = m[2].nvs_key;
    w.fail_times = 1;

    SecretMigrateOps ops;
    ops_bind(&ops, &w);

    int copied = secret_migrate_run(&ops);
    assert(copied == 4);                        /* four landed, one failed */
    /* The failed key did NOT land, and the sentinel was WITHHELD so the run is
     * not sealed — this is the pin: without the guard the sentinel would be set
     * and the key lost forever. */
    assert(!fake_has(&w, m[2].nvs_key));
    assert(!fake_has(&w, SECRET_SENTINEL_KEY));
    /* The four that succeeded are present. */
    for (size_t i = 0; i < n; i++)
        if (i != 2) assert(fake_has(&w, m[i].nvs_key));

    /* Re-run: the flaky write now succeeds. Only the missing key is copied
     * (the four already there are guarded by nvs_has), and the sentinel is set. */
    int again = secret_migrate_run(&ops);
    assert(again == 1);
    assert(fake_has(&w, m[2].nvs_key));
    assert(fake_has(&w, SECRET_SENTINEL_KEY));
    /* Byte-exact even on the retry path. */
    FakeEntry *e = fake_find(&w, m[2].nvs_key);
    assert(e != NULL && e->len == sizes[2]);
    assert(memcmp(e->val, data[2], sizes[2]) == 0);

    /* A third run is now a pure no-op. */
    int before = w.put_calls;
    assert(secret_migrate_run(&ops) == 0);
    assert(w.put_calls == before);
}

/* --- 8. malformed ops are refused, not crashed ------------------------------ */

static void test_ops_guard(void) {
    assert(secret_migrate_run(NULL) == -1);
    SecretMigrateOps ops;
    memset(&ops, 0, sizeof(ops));
    assert(secret_migrate_run(&ops) == -1);     /* NULL function pointers */
}

/* --- 9. device wrappers: secret_store_del (GW-TOKEN-401) --------------------- *
 * These run the REAL secret_store.cpp (linked into this binary) over the
 * tools/host_arduino Preferences mock — the delete the /gw/token clear path
 * depends on, not a fake re-encode of it. */

static void test_device_del(void) {
    uint8_t out[16];
    const uint8_t v[5] = {0xde, 0xad, 0xbe, 0xef, 0x01};

    /* del of a key that was never stored is a successful no-op */
    assert(!secret_store_has("gw_tok"));
    assert(secret_store_del("gw_tok"));
    assert(secret_store_get("gw_tok", out, sizeof(out)) == 0);

    /* put -> visible -> del -> get returns 0 (the clear-then-reboot pin) */
    assert(secret_store_put("gw_tok", v, sizeof(v)));
    assert(secret_store_has("gw_tok"));
    assert(secret_store_get("gw_tok", out, sizeof(out)) == sizeof(v));
    assert(memcmp(out, v, sizeof(v)) == 0);
    assert(secret_store_del("gw_tok"));
    assert(!secret_store_has("gw_tok"));
    assert(secret_store_get("gw_tok", out, sizeof(out)) == 0);

    /* a second del after the delete is again a no-op */
    assert(secret_store_del("gw_tok"));

    /* a delete only touches its own key */
    assert(secret_store_put("auth_tok", v, sizeof(v)));
    assert(secret_store_del("gw_tok"));
    assert(secret_store_get("auth_tok", out, sizeof(out)) == sizeof(v));
    assert(secret_store_del("auth_tok"));
    assert(!secret_store_has("auth_tok"));

    /* malformed names are refused, matching put/get/has */
    assert(!secret_store_del(NULL));
    assert(!secret_store_del(""));
}

int main(void) {
    test_mapping_shape();
    test_should_copy_truth_table();
    test_value_fits();
    test_byte_exact_migration();
    test_never_overwrite_existing();
    test_missing_file_skipped();
    test_oversize_rejected();
    test_sentinel_short_circuits();
    test_put_failure_withholds_sentinel();
    test_ops_guard();
    test_device_del();
    printf("secret store tests: OK\n");
    return 0;
}
