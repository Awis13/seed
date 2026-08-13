#pragma once
/*
 * Host mock of Arduino Preferences (NVS) — just enough surface for
 * src/secret_store.cpp to compile and RUN in tools/test_secret_store.sh, so
 * the thin device wrappers (put / get / has / del) execute on the host instead
 * of shipping untested. One process-wide in-memory store, shared across
 * Preferences instances exactly like the real NVS partition is; namespaced
 * keys mirror nvs_open()'s namespace argument.
 *
 * Semantics pinned to the real library where secret_store.cpp depends on them:
 *   - begin(ns, read_only) gates writes: putBytes/remove fail on a read-only
 *     handle;
 *   - getBytesLength / getBytes return 0 for an absent key;
 *   - remove() FAILS on an absent key (secret_store_del papers over that).
 */

#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class Preferences {
public:
    bool begin(const char *ns, bool read_only = false) {
        if (!ns || !ns[0]) return false;
        ns_ = ns;
        read_only_ = read_only;
        open_ = true;
        return true;
    }

    void end() { open_ = false; }

    bool isKey(const char *key) {
        if (!open_ || !key) return false;
        return store().count(full(key)) > 0;
    }

    size_t getBytesLength(const char *key) {
        if (!open_ || !key) return 0;
        auto it = store().find(full(key));
        return it == store().end() ? 0 : it->second.size();
    }

    size_t getBytes(const char *key, void *buf, size_t len) {
        if (!open_ || !key || !buf) return 0;
        auto it = store().find(full(key));
        if (it == store().end()) return 0;
        size_t n = it->second.size() < len ? it->second.size() : len;
        if (n) std::memcpy(buf, it->second.data(), n);
        return n;
    }

    size_t putBytes(const char *key, const void *buf, size_t len) {
        if (!open_ || read_only_ || !key) return 0;
        std::vector<unsigned char> v;
        if (buf && len) {
            const unsigned char *p = static_cast<const unsigned char *>(buf);
            v.assign(p, p + len);
        }
        store()[full(key)] = v;
        return len;
    }

    bool remove(const char *key) {
        if (!open_ || read_only_ || !key) return false;
        return store().erase(full(key)) > 0;   // absent key: false, like NVS
    }

private:
    std::string ns_;
    bool read_only_ = false;
    bool open_ = false;

    std::string full(const char *key) { return ns_ + "/" + key; }

    static std::map<std::string, std::vector<unsigned char>> &store() {
        static std::map<std::string, std::vector<unsigned char>> s;
        return s;
    }
};
