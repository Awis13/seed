#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WIFI_PROFILE_MAX 6
#define WIFI_PROFILE_SSID_CAP 33
#define WIFI_PROFILE_PASS_CAP 65
#define WIFI_PROFILE_BLOB_MAX  608

struct WifiProfileEntry {
    char ssid[WIFI_PROFILE_SSID_CAP];
    char pass[WIFI_PROFILE_PASS_CAP];
};

struct WifiProfileSet {
    WifiProfileEntry entries[WIFI_PROFILE_MAX];
    uint8_t count;
    uint8_t active;
    uint32_t generation;
};

static inline uint32_t wifi_profile_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static inline void wifi_profile_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t wifi_profile_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline size_t wifi_profile_encode(const WifiProfileSet *set,
                                         uint8_t *out, size_t cap) {
    if (!set || !out || set->count > WIFI_PROFILE_MAX ||
        (set->count && set->active >= set->count)) return 0;
    if (cap < 16) return 0;
    size_t pos = 0;
    out[pos++] = 'W'; out[pos++] = 'F'; out[pos++] = 'N'; out[pos++] = '1';
    out[pos++] = 1;
    out[pos++] = set->count;
    out[pos++] = set->count ? set->active : 0;
    out[pos++] = 0;
    wifi_profile_put_u32(out + pos, set->generation); pos += 4;
    for (uint8_t i = 0; i < set->count; i++) {
        size_t sn = strnlen(set->entries[i].ssid, WIFI_PROFILE_SSID_CAP);
        size_t pn = strnlen(set->entries[i].pass, WIFI_PROFILE_PASS_CAP);
        if (sn == 0 || sn > 32 || pn > 64 || pos + 2 + sn + pn + 4 > cap) return 0;
        out[pos++] = (uint8_t)sn;
        memcpy(out + pos, set->entries[i].ssid, sn); pos += sn;
        out[pos++] = (uint8_t)pn;
        memcpy(out + pos, set->entries[i].pass, pn); pos += pn;
    }
    uint32_t crc = wifi_profile_crc32(out, pos);
    wifi_profile_put_u32(out + pos, crc);
    return pos + 4;
}

static inline bool wifi_profile_decode(WifiProfileSet *set,
                                       const uint8_t *data, size_t len) {
    if (!set || !data || len < 16 || memcmp(data, "WFN1", 4) != 0 || data[4] != 1)
        return false;
    uint8_t count = data[5], active = data[6];
    if (count > WIFI_PROFILE_MAX || (count && active >= count) || data[7] != 0)
        return false;
    if (wifi_profile_get_u32(data + len - 4) != wifi_profile_crc32(data, len - 4))
        return false;
    WifiProfileSet next = {};
    next.count = count;
    next.active = count ? active : 0;
    next.generation = wifi_profile_get_u32(data + 8);
    size_t pos = 12;
    for (uint8_t i = 0; i < count; i++) {
        if (pos >= len - 4) return false;
        uint8_t sn = data[pos++];
        if (!sn || sn > 32 || pos + sn >= len - 4) return false;
        memcpy(next.entries[i].ssid, data + pos, sn); pos += sn;
        next.entries[i].ssid[sn] = '\0';
        uint8_t pn = data[pos++];
        if (pn > 64 || pos + pn > len - 4) return false;
        memcpy(next.entries[i].pass, data + pos, pn); pos += pn;
        next.entries[i].pass[pn] = '\0';
    }
    if (pos != len - 4) return false;
    *set = next;
    return true;
}

static inline bool wifi_profile_newer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static inline bool wifi_profile_equal(const WifiProfileSet *a,
                                      const WifiProfileSet *b) {
    if (!a || !b || a->count != b->count || a->active != b->active) return false;
    for (uint8_t i = 0; i < a->count; i++) {
        if (strcmp(a->entries[i].ssid, b->entries[i].ssid) != 0 ||
            strcmp(a->entries[i].pass, b->entries[i].pass) != 0) return false;
    }
    return true;
}

static inline bool wifi_profile_select(WifiProfileSet *out,
                                       const uint8_t *a_raw, size_t a_len,
                                       const uint8_t *b_raw, size_t b_len) {
    WifiProfileSet a = {}, b = {};
    bool a_ok = wifi_profile_decode(&a, a_raw, a_len);
    bool b_ok = wifi_profile_decode(&b, b_raw, b_len);
    if (!a_ok && !b_ok) return false;
    *out = (!a_ok || (b_ok && wifi_profile_newer(b.generation, a.generation))) ? b : a;
    return true;
}

bool wifi_profile_nvs_load(WifiProfileSet *set);
bool wifi_profile_nvs_save(const WifiProfileSet *set);
