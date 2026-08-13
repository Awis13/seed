#include "wifi_profile_store.h"

#include <Preferences.h>

#define WIFI_PROFILE_NVS_NS "wifi_profiles"
#define WIFI_PROFILE_KEY_A  "blob0"
#define WIFI_PROFILE_KEY_B  "blob1"

static bool wifi_profile_read_key(const char *key, WifiProfileSet *set,
                                  uint8_t *raw, size_t *raw_len) {
    Preferences prefs;
    if (!prefs.begin(WIFI_PROFILE_NVS_NS, true)) return false;
    size_t len = prefs.getBytesLength(key);
    bool ok = len >= 16 && len <= WIFI_PROFILE_BLOB_MAX;
    if (ok) ok = prefs.getBytes(key, raw, len) == len;
    prefs.end();
    if (!ok || !wifi_profile_decode(set, raw, len)) return false;
    if (raw_len) *raw_len = len;
    return true;
}

bool wifi_profile_nvs_load(WifiProfileSet *set) {
    if (!set) return false;
    uint8_t a_raw[WIFI_PROFILE_BLOB_MAX], b_raw[WIFI_PROFILE_BLOB_MAX];
    WifiProfileSet ignored = {};
    size_t a_len = 0, b_len = 0;
    bool a_ok = wifi_profile_read_key(WIFI_PROFILE_KEY_A, &ignored, a_raw, &a_len);
    bool b_ok = wifi_profile_read_key(WIFI_PROFILE_KEY_B, &ignored, b_raw, &b_len);
    return wifi_profile_select(set, a_ok ? a_raw : NULL, a_ok ? a_len : 0,
                              b_ok ? b_raw : NULL, b_ok ? b_len : 0);
}

bool wifi_profile_nvs_save(const WifiProfileSet *set) {
    if (!set) return false;
    WifiProfileSet current = {}, next = *set;
    bool have_current = wifi_profile_nvs_load(&current);
    if (have_current && wifi_profile_equal(&current, set)) return true;
    next.generation = have_current ? current.generation + 1u : 1u;
    if (!next.generation) next.generation = 1;
    uint8_t raw[WIFI_PROFILE_BLOB_MAX], verify[WIFI_PROFILE_BLOB_MAX];
    size_t len = wifi_profile_encode(&next, raw, sizeof(raw));
    if (!len) return false;
    const char *key = (next.generation & 1u) ? WIFI_PROFILE_KEY_A : WIFI_PROFILE_KEY_B;
    Preferences prefs;
    if (!prefs.begin(WIFI_PROFILE_NVS_NS, false)) return false;
    bool ok = prefs.putBytes(key, raw, len) == len;
    size_t got = ok ? prefs.getBytes(key, verify, sizeof(verify)) : 0;
    prefs.end();
    WifiProfileSet decoded = {};
    return got == len && memcmp(raw, verify, len) == 0 &&
           wifi_profile_decode(&decoded, verify, got) &&
           decoded.generation == next.generation;
}
