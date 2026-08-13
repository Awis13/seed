#include "wifi_profile_store.h"

#include <Preferences.h>

#define WIFI_PROFILE_NVS_NS "wifi_profiles"
#define WIFI_PROFILE_KEY_A  "blob0"
#define WIFI_PROFILE_KEY_B  "blob1"

/* These functions run only on Arduino's loop task (boot load and deferred
 * profile saves). Keeping the codec workspaces static avoids placing several
 * ~600-byte profile/blob objects on the already busy setup() stack. The old
 * nested save -> load path consumed multiple kilobytes and corrupted the NVS
 * semaphore during first-boot migration on the real ESP32-S3. */
static uint8_t wifi_profile_raw_a[WIFI_PROFILE_BLOB_MAX];
static uint8_t wifi_profile_raw_b[WIFI_PROFILE_BLOB_MAX];
static uint8_t wifi_profile_raw_write[WIFI_PROFILE_BLOB_MAX];
static uint8_t wifi_profile_raw_verify[WIFI_PROFILE_BLOB_MAX];
static WifiProfileSet wifi_profile_ignored;
static WifiProfileSet wifi_profile_current;
static WifiProfileSet wifi_profile_next;
static WifiProfileSet wifi_profile_decoded;

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
    memset(&wifi_profile_ignored, 0, sizeof(wifi_profile_ignored));
    size_t a_len = 0, b_len = 0;
    bool a_ok = wifi_profile_read_key(WIFI_PROFILE_KEY_A, &wifi_profile_ignored,
                                      wifi_profile_raw_a, &a_len);
    bool b_ok = wifi_profile_read_key(WIFI_PROFILE_KEY_B, &wifi_profile_ignored,
                                      wifi_profile_raw_b, &b_len);
    return wifi_profile_select(set,
                               a_ok ? wifi_profile_raw_a : NULL, a_ok ? a_len : 0,
                               b_ok ? wifi_profile_raw_b : NULL, b_ok ? b_len : 0);
}

bool wifi_profile_nvs_save(const WifiProfileSet *set) {
    if (!set) return false;
    memset(&wifi_profile_current, 0, sizeof(wifi_profile_current));
    wifi_profile_next = *set;
    bool have_current = wifi_profile_nvs_load(&wifi_profile_current);
    if (have_current && wifi_profile_equal(&wifi_profile_current, set)) return true;
    wifi_profile_next.generation = have_current ? wifi_profile_current.generation + 1u : 1u;
    if (!wifi_profile_next.generation) wifi_profile_next.generation = 1;
    size_t len = wifi_profile_encode(&wifi_profile_next, wifi_profile_raw_write,
                                     sizeof(wifi_profile_raw_write));
    if (!len) return false;
    const char *key = (wifi_profile_next.generation & 1u)
        ? WIFI_PROFILE_KEY_A : WIFI_PROFILE_KEY_B;
    Preferences prefs;
    if (!prefs.begin(WIFI_PROFILE_NVS_NS, false)) return false;
    bool ok = prefs.putBytes(key, wifi_profile_raw_write, len) == len;
    size_t got = ok ? prefs.getBytes(key, wifi_profile_raw_verify,
                                     sizeof(wifi_profile_raw_verify)) : 0;
    prefs.end();
    memset(&wifi_profile_decoded, 0, sizeof(wifi_profile_decoded));
    return got == len &&
           memcmp(wifi_profile_raw_write, wifi_profile_raw_verify, len) == 0 &&
           wifi_profile_decode(&wifi_profile_decoded, wifi_profile_raw_verify, got) &&
           wifi_profile_decoded.generation == wifi_profile_next.generation;
}
