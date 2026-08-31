#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/wifi_profile_store.h"

static WifiProfileSet sample(uint32_t generation, const char *ssid) {
    WifiProfileSet set = {};
    set.count = 2;
    set.active = 1;
    set.generation = generation;
    snprintf(set.entries[0].ssid, sizeof(set.entries[0].ssid), "%s", ssid);
    snprintf(set.entries[0].pass, sizeof(set.entries[0].pass), "secret");
    snprintf(set.entries[1].ssid, sizeof(set.entries[1].ssid), "Скрытая");
    set.entries[1].pass[0] = '\0';
    return set;
}

int main(void) {
    uint8_t a[WIFI_PROFILE_BLOB_MAX], b[WIFI_PROFILE_BLOB_MAX];
    WifiProfileSet old = sample(7, "home"), fresh = sample(8, "travel");
    size_t an = wifi_profile_encode(&old, a, sizeof(a));
    size_t bn = wifi_profile_encode(&fresh, b, sizeof(b));
    assert(an && bn);

    WifiProfileSet got = {};
    assert(wifi_profile_select(&got, a, an, b, bn));
    assert(got.generation == 8 && strcmp(got.entries[0].ssid, "travel") == 0);
    assert(strcmp(got.entries[1].ssid, "Скрытая") == 0);
    assert(got.entries[1].pass[0] == '\0');

    /* A torn/corrupt new slot falls back to the previous complete generation. */
    b[bn - 1] ^= 0x80;
    assert(wifi_profile_select(&got, a, an, b, bn));
    assert(got.generation == 7 && strcmp(got.entries[0].ssid, "home") == 0);
    assert(!wifi_profile_select(&got, b, bn, b, bn));

    /* Reboot/format independence: the selected blob is self-contained; no
       SPIFFS input participates in decoding it. */
    assert(wifi_profile_decode(&got, a, an));
    assert(wifi_profile_equal(&got, &old));

    WifiProfileSet bad = old;
    memset(bad.entries[0].ssid, 'x', sizeof(bad.entries[0].ssid));
    assert(wifi_profile_encode(&bad, b, sizeof(b)) == 0);
    puts("wifi profile store tests: OK");
}
