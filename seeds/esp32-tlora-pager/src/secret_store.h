#pragma once
/*
 * secret_store.h -- format-safe per-device SECRETS in NVS (foundation only).
 *
 * WHY THIS EXISTS
 * ---------------
 * Every per-device secret today lives ONLY on the SPIFFS data partition:
 *   /rns_identity.id   -- the Reticulum identity; the node's RNS address is
 *                         DERIVED from these key bytes (skills/rns.cpp), so
 *                         losing them changes the address, not just the keys.
 *   /mesh_identity.id  -- the MeshCore identity blob (public + private key).
 *   /mesh_pair.json    -- the MeshCore public meta (name, keys, radio params).
 *   /gw_token.txt      -- the home-gateway bearer token.
 *   /auth_token.txt    -- this device's own HTTP auth token.
 *
 * A SPIFFS.format() -- which the boot path can reach on a mount error, and which
 * a foreign firmware has done to this very board -- wipes all five at once, and
 * the device comes up a stranger: new mesh keys, a new RNS address, a rotated
 * OTA token that locks out WiFi flashing. NVS is a SEPARATE ~20KB partition at
 * 0x9000 that a SPIFFS format does NOT touch (the panic counter already relies
 * on this, main.cpp). Mirroring the five secrets into NVS makes identity survive
 * a data-partition wipe.
 *
 * THIS HEADER IS PURE. No Arduino, no FreeRTOS, no FS -- only the parts that
 * DECIDE what happens: the {nvs_key <-> spiffs_path} mapping, the value-size
 * guard, the per-key migrate decision, and the migration DRIVER, which walks the
 * mapping over an injected key-value store and file source (SecretMigrateOps).
 * secret_store.cpp backs those ops with Preferences/NVS + fs::FS; the host test
 * (tools/test_secret_store.cpp) backs them with a fake map + fake files and so
 * drives the SAME driver, not a re-encode of it.
 *
 * BYTE-EXACT is the whole point. The migration copies each file's bytes verbatim
 * into NVS and the driver never transforms them -- if the bytes in are not the
 * bytes out, the RNS address moves. The round-trip is pinned by value in the
 * test.
 *
 * FOUNDATION ONLY (C1). Nothing calls this yet: the boot-time migration and the
 * NVS-first load at each of the five sites are a later commit. Unused symbols
 * here are GC-eligible, exactly like the contacts foundation commit.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* NVS key names are bounded by the underlying nvs layer: at most 15 characters
 * (the 16th byte is the NUL). Every mapping key below is held to this, and the
 * test asserts it by value so a rename that overflows fails loudly on the host. */
#define SECRET_NVS_KEY_MAX 15

/* Largest secret blob we mirror. The real files are small: /rns_identity.id is
 * 128 bytes of hex text, /mesh_identity.id is 96 bytes, /mesh_pair.json is a
 * ~200-byte JSON, both tokens are short. 512 fits all five with headroom and
 * stays far under the NVS single-value ceiling; anything larger is refused
 * rather than truncated (a truncated identity is a changed identity). */
#define SECRET_VALUE_MAX 512

/* Sentinel key: present (=1) once the one-time migration has run. Its presence
 * makes a second migrate a pure no-op. Also <= SECRET_NVS_KEY_MAX. */
#define SECRET_SENTINEL_KEY "migrated"

/* One row of the secret mapping: the short NVS key and the SPIFFS file it
 * mirrors. */
struct SecretMap {
    const char *nvs_key;
    const char *spiffs_path;
};

/* THE single source of truth for the five secrets. `static inline` so the header
 * is self-contained for the host test and identical in every translation unit.
 * The keys are short (<= SECRET_NVS_KEY_MAX) and distinct; the paths are the
 * literal read sites in rns.cpp / meshcore.cpp / mc_client.cpp / main.cpp. */
static inline const SecretMap *secret_map(size_t *count) {
    static const SecretMap map[] = {
        {"rns_id",    "/rns_identity.id"},
        {"mesh_id",   "/mesh_identity.id"},
        {"mesh_pair", "/mesh_pair.json"},
        {"gw_tok",    "/gw_token.txt"},
        {"auth_tok",  "/auth_token.txt"},
    };
    if (count) *count = sizeof(map) / sizeof(map[0]);
    return map;
}

/* Value-size guard, pure. A blob at most SECRET_VALUE_MAX bytes may be stored;
 * anything larger is refused so a secret is never silently truncated. */
static inline bool secret_value_fits(size_t len) {
    return len <= (size_t)SECRET_VALUE_MAX;
}

/* Per-key migrate decision, as a complete truth table:
 *   sentinel_present -> the whole migration already ran: copy NOTHING;
 *   nvs_has_key      -> a value is already there: never overwrite it;
 *   !file_present    -> nothing on SPIFFS to copy.
 * Only a first-ever run, an empty NVS key and an existing file yield a copy. */
static inline bool secret_should_copy(bool sentinel_present, bool nvs_has_key,
                                      bool file_present) {
    if (sentinel_present) return false;
    if (nvs_has_key) return false;
    if (!file_present) return false;
    return true;
}

/*
 * The injected seam the migration DRIVER runs over, so the same driver serves
 * the device (Preferences + fs::FS) and the host test (fake map + fake files).
 *
 *   has(ctx, key)                       -> true if `key` already has a value.
 *   read_file(ctx, path, buf, cap, &p)  -> fills buf (up to cap) with the file's
 *                                          bytes, sets *p when the file EXISTS,
 *                                          and returns the file's TRUE byte
 *                                          length (which may exceed cap -- the
 *                                          driver's size guard rejects that).
 *   put(ctx, key, buf, len)             -> stores len bytes under key; true on
 *                                          success.
 */
struct SecretMigrateOps {
    bool (*has)(void *ctx, const char *key);
    size_t (*read_file)(void *ctx, const char *path, uint8_t *buf, size_t cap,
                        bool *present);
    bool (*put)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    void *ctx;
};

/*
 * One-time migration driver, pure. If the sentinel is set, returns 0 without a
 * single write (idempotent). Otherwise, for each mapping row: read the file,
 * and if secret_should_copy() says so AND the bytes fit, copy them VERBATIM into
 * NVS; an existing key is never overwritten and an oversize/absent file is
 * skipped.
 *
 * The sentinel is set ONLY when every copy that SHOULD have happened did. If a
 * put of a present, copyable secret FAILS (a transient NVS write error), the
 * sentinel is left unset so the next boot retries that key rather than skipping
 * it forever — the secrets that already landed are guarded by nvs_has, so the
 * retry touches only what is still missing. A skip for an oversize or absent
 * file is NOT a failure (it is a permanent condition, and blocking the sentinel
 * on it would retry every boot with no hope of success), so it does not hold the
 * sentinel back. Returns the number of secrets copied, or -1 if ops are malformed.
 */
static inline int secret_migrate_run(const SecretMigrateOps *ops) {
    if (!ops || !ops->has || !ops->read_file || !ops->put) return -1;
    if (ops->has(ops->ctx, SECRET_SENTINEL_KEY)) return 0;

    size_t n = 0;
    const SecretMap *map = secret_map(&n);
    int copied = 0;
    bool copy_failed = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t buf[SECRET_VALUE_MAX];
        bool present = false;
        bool nvs_has = ops->has(ops->ctx, map[i].nvs_key);
        size_t len =
            ops->read_file(ops->ctx, map[i].spiffs_path, buf, sizeof(buf), &present);
        if (!secret_should_copy(false, nvs_has, present)) continue;
        if (!secret_value_fits(len)) continue;   /* oversize: guard, never cut */
        if (ops->put(ops->ctx, map[i].nvs_key, buf, len)) copied++;
        else copy_failed = true;                 /* transient: retry next boot */
    }
    /* Do not seal the migration if a copyable secret failed to write. */
    if (!copy_failed) {
        const uint8_t one = 1;
        ops->put(ops->ctx, SECRET_SENTINEL_KEY, &one, 1);
    }
    return copied;
}

/* ------------------------------------------------------------------------- *
 * Device API (implemented in secret_store.cpp over Preferences/NVS + fs::FS).
 * Declared here; not called anywhere yet (C2 wires the boot migration and the
 * NVS-first loads). fs::FS is device-bound, so it is only forward-declared --
 * this keeps the header host-compilable for the test.
 * ------------------------------------------------------------------------- */
namespace fs { class FS; }

/* True if a secret is present in NVS under `name`. */
bool secret_store_has(const char *name);

/* Copy the secret `name` into buf (capacity cap). Returns its byte length, or 0
 * if absent, unreadable, or larger than cap / SECRET_VALUE_MAX. */
size_t secret_store_get(const char *name, uint8_t *buf, size_t cap);

/* Store len bytes under `name`. Refuses an oversize value (returns false). */
bool secret_store_put(const char *name, const uint8_t *buf, size_t len);

/* One-time SPIFFS->NVS migration for the five mapped secrets. Idempotent: a
 * second call after the sentinel is set does nothing. Returns true if it copied
 * at least one secret this call. */
bool secret_store_migrate_from_spiffs(fs::FS &fs);
