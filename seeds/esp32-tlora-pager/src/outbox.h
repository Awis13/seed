#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OUTBOX_SLOTS 8
#define OUTBOX_TARGET_LEN 64
#define OUTBOX_SESSION_LEN 24
#define OUTBOX_KEY_LEN 25
#define OUTBOX_TEXT_LEN 512
#define OUTBOX_SNAPSHOT_MAX 5200

enum OutboxKind : uint8_t {
    OUTBOX_KIND_REPLY = 1,
    OUTBOX_KIND_AGENT = 2,
};

enum OutboxState : uint8_t {
    OUTBOX_STATE_QUEUED = 1,
    OUTBOX_STATE_SENDING,
    OUTBOX_STATE_SENT,
    OUTBOX_STATE_AUTH,
    OUTBOX_STATE_FAIL,
};

struct OutboxItem {
    uint32_t id;
    uint8_t kind;
    uint8_t state;
    uint8_t attempts;
    uint32_t next_try_ms;
    char target[OUTBOX_TARGET_LEN];
    char session[OUTBOX_SESSION_LEN];
    char delivery_key[OUTBOX_KEY_LEN];
    char text[OUTBOX_TEXT_LEN];
};

struct OutboxStore {
    OutboxItem items[OUTBOX_SLOTS];
    uint32_t next_id;
};

static inline void outbox_init(OutboxStore *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
    store->next_id = 1;
}

static inline bool outbox_terminal(uint8_t state) {
    return state == OUTBOX_STATE_SENT || state == OUTBOX_STATE_AUTH ||
           state == OUTBOX_STATE_FAIL;
}

static inline int outbox_find_id(const OutboxStore *store, uint32_t id) {
    if (!store || !id) return -1;
    for (int i = 0; i < OUTBOX_SLOTS; ++i)
        if (store->items[i].id == id) return i;
    return -1;
}

static inline int outbox_pick_slot(const OutboxStore *store) {
    int terminal = -1;
    for (int i = 0; i < OUTBOX_SLOTS; ++i) {
        if (!store->items[i].id) return i;
        if (outbox_terminal(store->items[i].state) &&
            (terminal < 0 || store->items[i].id < store->items[terminal].id))
            terminal = i;
    }
    return terminal;
}

static inline uint32_t outbox_enqueue(OutboxStore *store, uint8_t kind,
                                      const char *target, const char *session,
                                      const char *delivery_key,
                                      const char *text) {
    if (!store || (kind != OUTBOX_KIND_REPLY && kind != OUTBOX_KIND_AGENT) ||
        !target || !target[0] || !text || !text[0]) return 0;
    if (strlen(target) >= OUTBOX_TARGET_LEN ||
        (session && strlen(session) >= OUTBOX_SESSION_LEN) ||
        (delivery_key && strlen(delivery_key) >= OUTBOX_KEY_LEN) ||
        strlen(text) >= OUTBOX_TEXT_LEN) return 0;
    int slot = outbox_pick_slot(store);
    if (slot < 0) return 0;
    uint32_t id = store->next_id++;
    if (!id) id = store->next_id++;
    OutboxItem &item = store->items[slot];
    memset(&item, 0, sizeof(item));
    item.id = id;
    item.kind = kind;
    item.state = OUTBOX_STATE_QUEUED;
    snprintf(item.target, sizeof(item.target), "%s", target);
    snprintf(item.session, sizeof(item.session), "%s", session ? session : "");
    snprintf(item.delivery_key, sizeof(item.delivery_key), "%s",
             delivery_key ? delivery_key : "");
    snprintf(item.text, sizeof(item.text), "%s", text);
    return id;
}

static inline bool outbox_set_state(OutboxStore *store, uint32_t id,
                                    uint8_t state) {
    int slot = outbox_find_id(store, id);
    if (slot < 0 || state < OUTBOX_STATE_QUEUED || state > OUTBOX_STATE_FAIL)
        return false;
    store->items[slot].state = state;
    return true;
}

static inline OutboxItem *outbox_oldest_pending(OutboxStore *store) {
    OutboxItem *best = nullptr;
    if (!store) return nullptr;
    for (int i = 0; i < OUTBOX_SLOTS; ++i) {
        OutboxItem *item = &store->items[i];
        if (!item->id || (item->state != OUTBOX_STATE_QUEUED &&
                          item->state != OUTBOX_STATE_SENDING)) continue;
        if (!best || item->id < best->id) best = item;
    }
    return best;
}

static inline uint32_t outbox_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static inline bool outbox_put_u16(uint8_t *out, size_t cap, size_t *at,
                                  uint16_t value) {
    if (*at + 2 > cap) return false;
    out[(*at)++] = (uint8_t)value;
    out[(*at)++] = (uint8_t)(value >> 8);
    return true;
}

static inline bool outbox_put_u32(uint8_t *out, size_t cap, size_t *at,
                                  uint32_t value) {
    if (*at + 4 > cap) return false;
    for (int i = 0; i < 4; ++i) out[(*at)++] = (uint8_t)(value >> (8 * i));
    return true;
}

static inline bool outbox_put_text(uint8_t *out, size_t cap, size_t *at,
                                   const char *text) {
    size_t len = strlen(text);
    if (len > UINT16_MAX || !outbox_put_u16(out, cap, at, (uint16_t)len) ||
        *at + len > cap) return false;
    memcpy(out + *at, text, len);
    *at += len;
    return true;
}

static inline size_t outbox_encode(const OutboxStore *store, uint8_t *out,
                                   size_t cap) {
    if (!store || !out || cap < 13) return 0;
    size_t at = 0;
    memcpy(out + at, "OBX1", 4); at += 4;
    if (!outbox_put_u32(out, cap, &at, store->next_id)) return 0;
    size_t count_at = at++;
    uint8_t count = 0;
    for (int i = 0; i < OUTBOX_SLOTS; ++i) {
        const OutboxItem &item = store->items[i];
        if (!item.id) continue;
        if (!outbox_put_u32(out, cap, &at, item.id) || at + 3 > cap) return 0;
        out[at++] = item.kind;
        out[at++] = item.state;
        out[at++] = item.attempts;
        if (!outbox_put_u32(out, cap, &at, item.next_try_ms) ||
            !outbox_put_text(out, cap, &at, item.target) ||
            !outbox_put_text(out, cap, &at, item.session) ||
            !outbox_put_text(out, cap, &at, item.delivery_key) ||
            !outbox_put_text(out, cap, &at, item.text)) return 0;
        ++count;
    }
    out[count_at] = count;
    uint32_t crc = outbox_crc32(out, at);
    if (!outbox_put_u32(out, cap, &at, crc)) return 0;
    return at;
}

static inline bool outbox_get_u16(const uint8_t *in, size_t len, size_t *at,
                                  uint16_t *value) {
    if (*at + 2 > len) return false;
    *value = (uint16_t)in[*at] | ((uint16_t)in[*at + 1] << 8);
    *at += 2;
    return true;
}

static inline bool outbox_get_u32(const uint8_t *in, size_t len, size_t *at,
                                  uint32_t *value) {
    if (*at + 4 > len) return false;
    *value = 0;
    for (int i = 0; i < 4; ++i) *value |= (uint32_t)in[(*at)++] << (8 * i);
    return true;
}

static inline bool outbox_get_text(const uint8_t *in, size_t len, size_t *at,
                                   char *out, size_t cap) {
    uint16_t n = 0;
    if (!outbox_get_u16(in, len, at, &n) || n >= cap || *at + n > len)
        return false;
    memcpy(out, in + *at, n);
    out[n] = '\0';
    *at += n;
    return true;
}

static inline bool outbox_decode(OutboxStore *store, const uint8_t *in,
                                 size_t len) {
    if (!store || !in || len < 13 || memcmp(in, "OBX1", 4) != 0) return false;
    uint32_t stored_crc = 0;
    size_t crc_at = len - 4;
    size_t crc_pos = crc_at;
    if (!outbox_get_u32(in, len, &crc_pos, &stored_crc) ||
        stored_crc != outbox_crc32(in, crc_at)) return false;

    OutboxStore next;
    outbox_init(&next);
    size_t at = 4;
    uint8_t count = 0;
    if (!outbox_get_u32(in, crc_at, &at, &next.next_id) || at >= crc_at)
        return false;
    count = in[at++];
    if (count > OUTBOX_SLOTS || !next.next_id) return false;
    for (uint8_t i = 0; i < count; ++i) {
        OutboxItem &item = next.items[i];
        if (!outbox_get_u32(in, crc_at, &at, &item.id) || at + 3 > crc_at)
            return false;
        item.kind = in[at++];
        item.state = in[at++];
        item.attempts = in[at++];
        if (!item.id || (item.kind != OUTBOX_KIND_REPLY &&
                         item.kind != OUTBOX_KIND_AGENT) ||
            item.state < OUTBOX_STATE_QUEUED || item.state > OUTBOX_STATE_FAIL ||
            !outbox_get_u32(in, crc_at, &at, &item.next_try_ms) ||
            !outbox_get_text(in, crc_at, &at, item.target, sizeof(item.target)) ||
            !outbox_get_text(in, crc_at, &at, item.session, sizeof(item.session)) ||
            !outbox_get_text(in, crc_at, &at, item.delivery_key,
                             sizeof(item.delivery_key)) ||
            !outbox_get_text(in, crc_at, &at, item.text, sizeof(item.text)) ||
            !item.target[0] || !item.text[0]) return false;
    }
    if (at != crc_at) return false;
    *store = next;
    return true;
}
