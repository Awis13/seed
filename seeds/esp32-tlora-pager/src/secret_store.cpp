/*
 * secret_store.cpp -- device backing for secret_store.h.
 *
 * The pure parts (mapping, size guard, migrate decision + driver) live in the
 * header and are host-tested. This file is the DEVICE half only: the small NVS
 * wrappers over Arduino Preferences, and the SecretMigrateOps that back the
 * shared driver with real NVS and a real filesystem. Nothing here decides WHAT
 * to migrate -- it only performs the reads and writes the driver asks for.
 *
 * Not called from anywhere yet (C1 foundation): the boot-time migration and the
 * NVS-first loads at the five read sites are a later commit.
 */

#include "secret_store.h"
#include "mesh_secret_bridge.h"

#include <Preferences.h>
#include <FS.h>

/* Its OWN namespace, distinct from the panic counter's "bootdiag" (main.cpp). */
#define SECRET_NVS_NS "secrets"

bool secret_store_has(const char *name) {
    if (!name || !name[0]) return false;
    Preferences prefs;
    if (!prefs.begin(SECRET_NVS_NS, true)) return false;   // read-only
    bool has = prefs.isKey(name);
    prefs.end();
    return has;
}

size_t secret_store_get(const char *name, uint8_t *buf, size_t cap) {
    if (!name || !name[0] || !buf || cap == 0) return 0;
    Preferences prefs;
    if (!prefs.begin(SECRET_NVS_NS, true)) return 0;       // read-only
    size_t len = prefs.getBytesLength(name);
    size_t out = 0;
    if (len > 0 && len <= cap && secret_value_fits(len)) {
        out = prefs.getBytes(name, buf, len);
        if (out != len) out = 0;                           // partial read: reject
    }
    prefs.end();
    return out;
}

bool secret_store_put(const char *name, const uint8_t *buf, size_t len) {
    if (!name || !name[0]) return false;
    if (!buf && len) return false;
    if (!secret_value_fits(len)) return false;
    Preferences prefs;
    if (!prefs.begin(SECRET_NVS_NS, false)) return false;  // read-write
    size_t wrote = prefs.putBytes(name, buf, len);
    prefs.end();
    return wrote == len;
}

bool secret_store_del(const char *name) {
    if (!name || !name[0]) return false;
    Preferences prefs;
    if (!prefs.begin(SECRET_NVS_NS, false)) return false;  // read-write
    // Preferences::remove() fails on an absent key; an absent key is already
    // the state a delete asks for, so report success without a write.
    bool gone = prefs.isKey(name) ? prefs.remove(name) : true;
    prefs.end();
    return gone;
}

// ---- Migration ops: back the pure driver with real NVS + real FS -----------

namespace {

struct SecretRealCtx {
    fs::FS *fs;
};

bool real_has(void *ctx, const char *key) {
    (void)ctx;
    return secret_store_has(key);
}

/* Report EXISTS (*present) only when the whole file fits and was fully read, so
 * a secret is migrated only when its bytes can be preserved verbatim. An empty,
 * oversize, or partially-read file returns 0 with *present=false, which the
 * driver treats as "nothing to copy" rather than migrating a broken secret. */
size_t real_read_file(void *ctx, const char *path, uint8_t *buf, size_t cap,
                      bool *present) {
    if (present) *present = false;
    SecretRealCtx *c = static_cast<SecretRealCtx *>(ctx);
    if (!c || !c->fs || !path || !buf) return 0;
    if (!c->fs->exists(path)) return 0;
    File f = c->fs->open(path, "r");
    if (!f) return 0;
    size_t sz = (size_t)f.size();
    if (sz == 0 || sz > cap) { f.close(); return 0; }
    size_t r = f.read(buf, sz);
    f.close();
    if (r != sz) return 0;                                 // partial: not copyable
    if (present) *present = true;
    return sz;
}

bool real_put(void *ctx, const char *key, const uint8_t *buf, size_t len) {
    (void)ctx;
    return secret_store_put(key, buf, len);
}

}  // namespace

// ---- mesh_secret_bridge.h: narrow NVS window for the mc_client crypto TU -----
// Thin pass-throughs so mc_client.cpp need not include Preferences/secret_store.h.

size_t mesh_secret_get(const char *name, uint8_t *buf, size_t cap) {
    return secret_store_get(name, buf, cap);
}

bool mesh_secret_put(const char *name, const uint8_t *buf, size_t len) {
    return secret_store_put(name, buf, len);
}

bool secret_store_migrate_from_spiffs(fs::FS &fs) {
    SecretRealCtx c;
    c.fs = &fs;
    SecretMigrateOps ops;
    ops.has = real_has;
    ops.read_file = real_read_file;
    ops.put = real_put;
    ops.ctx = &c;
    return secret_migrate_run(&ops) > 0;
}
