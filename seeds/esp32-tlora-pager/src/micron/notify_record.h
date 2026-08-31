#pragma once

/*
 * micron/notify_record.h — the PURE, host-testable serialization of a notify
 * CARD into a history-archive payload (ticket TLORA-HISTORY / C3).
 *
 * C3 migrates notify persistence off the old loop-task /notify.json SPIFFS
 * snapshot and onto the unified append-only archive (history_store.h): a card
 * is written through the OFF-LOOP history_enqueue() queue at create/update, and
 * the RAM ring is rebuilt from the archive at boot. This file is the codec that
 * turns a card's persisted fields into the bounded blob a history_record carries
 * as its payload, and back.
 *
 * PURE / ARDUINO-FREE (same discipline as history_store.h / micron_store.h):
 * plain integer/byte logic, no Arduino, no String, no clock. The device maps
 * its Notification struct onto notify_rec and calls encode/decode; the host test
 * drives the exact same functions. The archive record's identity (ns,key) and
 * its seq/stamp are owned by the archive layer — NONE of that is in this payload.
 *
 * WHAT IS ARCHIVED, AND WHAT IS NOT
 * ---------------------------------
 * Persisted: id, event identity, level, unread, ttl_s, created_epoch, source,
 * title, body, key.
 * Those are exactly the fields that must survive a reboot — the card's identity,
 * its unread badge, its ttl clock (aged against created_epoch), and its text.
 *
 * DELIBERATELY NOT archived: the reply OPTIONS table and the chosen index. The
 * options are EPHEMERAL UI — a historical card restored after a reboot needs no
 * buttons, the question it asked is stale, and carrying a chosen index without
 * its labels is meaningless. The free-text reply was never persisted either (it
 * is parallel-array UI state), and that is unchanged. So a card comes back read/
 * unread with its text intact but as a plain notification, not a live prompt.
 *
 * created_ms is device-relative (millis, resets to 0 each boot) and is therefore
 * NOT stored; the device reconstructs it at restore from created_epoch and the
 * wall clock, exactly as the old /notify.json path did.
 *
 * SIZE BUDGET (fits HISTORY_PAYLOAD_CAP = MICRON_STORE_SRC_CAP = 512, with room):
 *   head 15  = ver(1) id(4) ttl(4) epoch(4) level(1) unread(1)
 *   + 1 + source(<=16)   = 17
 *   + 1 + title (<=60)   = 61
 *   + 2 + body  (<=240)  = 242
 *   + 1 + key   (<=24)   = 25
 *   + tagged event identity       = 26
 *   ------------------------------ NOTIFY_REC_MAX = 386 bytes  (< 512)
 *
 * Strings are length-prefixed and bounded to their field caps, so an over-long
 * body or an embedded NUL is truncated at the field boundary and the whole blob
 * can never exceed NOTIFY_REC_MAX. A NUL inside a field ends that field's string
 * at encode (C-string semantics) — documented, harmless.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "micron_store.h"   /* micron_store_key_valid — shared key-validity rule */

/* Field capacities — MUST match notify.cpp's NOTIFY_*_LEN. notify.cpp pins the
 * equality with static_asserts so the two cannot drift. */
#define NR_SOURCE_CAP  17   /* 16 chars + NUL */
#define NR_TITLE_CAP   61   /* 60 chars + NUL */
#define NR_BODY_CAP   241   /* 240 chars + NUL */
#define NR_KEY_CAP     25   /* 24 chars + NUL */

#define NOTIFY_REC_VER    1
#define NOTIFY_REC_HEAD  15   /* ver id ttl epoch level unread */
#define NOTIFY_REC_EVENT_TAG 0xE1u
#define NOTIFY_REC_EVENT_SUFFIX 26 /* tag + 128-bit epoch + u64 counter + flags */

/* Worst-case encoded size (see the budget note). The device sizes its enqueue
 * buffer to this; it is well under HISTORY_PAYLOAD_CAP (512). */
#define NOTIFY_REC_MAX (NOTIFY_REC_HEAD + 1 + (NR_SOURCE_CAP - 1) \
                        + 1 + (NR_TITLE_CAP - 1) + 2 + (NR_BODY_CAP - 1) \
                        + 1 + (NR_KEY_CAP - 1) + NOTIFY_REC_EVENT_SUFFIX)

typedef struct {
    uint64_t epoch_hi;
    uint64_t epoch_lo;
    uint64_t counter;
} NotifyEventId;

typedef struct {
    uint64_t first;
    uint64_t limit;
} NotifyEventReservation;

static inline int notify_event_reservation_plan(uint64_t old_limit,
                                                uint64_t block,
                                                NotifyEventReservation *out) {
    if (!out || block == 0 || old_limit > UINT64_MAX - block) return 0;
    out->first = old_limit + 1;
    out->limit = old_limit + block;
    return 1;
}

static inline int notify_event_epoch_must_rotate(int any_key,
                                                 int ready_u8, uint8_t state,
                                                 int epoch_hi_u64,
                                                 int epoch_lo_u64,
                                                 int counter_hi_u64,
                                                 uint64_t epoch_hi,
                                                 uint64_t epoch_lo,
                                                 uint64_t counter_hi) {
    (void)counter_hi;
    if (!any_key) return 1; /* pristine initialization */
    return !ready_u8 || state != 2 || !epoch_hi_u64 || !epoch_lo_u64 ||
           !counter_hi_u64 || epoch_hi == 0 || epoch_lo == 0;
}

static inline int notify_event_id_valid(const NotifyEventId *id) {
    return id && id->epoch_hi != 0 && id->epoch_lo != 0 && id->counter != 0;
}

/* Consume an already-reserved identity from RAM. The device wraps this tiny
 * operation in a critical section; Preferences/NVS is deliberately absent
 * from the live producer path. Exhaustion fails closed with a zero identity. */
static inline int notify_event_ram_take(NotifyEventId *next, uint64_t limit,
                                        NotifyEventId *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!notify_event_id_valid(next) || next->counter > limit) return 0;
    *out = *next;
    next->counter++;
    return 1;
}

static inline int notify_event_id_equal(const NotifyEventId *a,
                                        const NotifyEventId *b) {
    return notify_event_id_valid(a) && notify_event_id_valid(b) &&
           a->epoch_hi == b->epoch_hi && a->epoch_lo == b->epoch_lo &&
           a->counter == b->counter;
}

#define NOTIFY_EVENT_HEX_CAP 49

static inline int notify_event_id_format(const NotifyEventId *id,
                                         char *out, size_t out_n) {
    if (!notify_event_id_valid(id) || !out || out_n < NOTIFY_EVENT_HEX_CAP)
        return 0;
    int n = snprintf(out, out_n, "%016llx%016llx%016llx",
                     (unsigned long long)id->epoch_hi,
                     (unsigned long long)id->epoch_lo,
                     (unsigned long long)id->counter);
    return n == NOTIFY_EVENT_HEX_CAP - 1;
}

static inline int notify_event_origin_matches(uint32_t stored_id,
                                              const char *stored_key,
                                              const char *stored_event,
                                              uint32_t wanted_id,
                                              const char *wanted_key,
                                              const NotifyEventId *wanted_event) {
    char expected[NOTIFY_EVENT_HEX_CAP];
    return stored_id == wanted_id && stored_key && wanted_key && stored_event &&
           strcmp(stored_key, wanted_key) == 0 &&
           notify_event_id_format(wanted_event, expected, sizeof(expected)) &&
           strcmp(stored_event, expected) == 0;
}

/* The persisted projection of a Notification. POD, no Arduino types: created_ms
 * is intentionally absent (device-relative), options/chosen absent (ephemeral). */
typedef struct {
    uint32_t id;
    uint32_t ttl_s;
    uint32_t created_epoch;   /* seconds since epoch, 0 when the clock was unset */
    NotifyEventId event_id;   /* stable identity for one raised event */
    uint8_t event_distinct;   /* archive/replacement identity follows chat plan */
    uint8_t  level;
    uint8_t  unread;          /* 0 / 1 */
    char     source[NR_SOURCE_CAP];
    char     title[NR_TITLE_CAP];
    char     body[NR_BODY_CAP];
    char     key[NR_KEY_CAP];
} notify_rec;

/* --- little-endian scalar helpers (mirrors history_store.h) ----------------- */

static inline void notify_rec_put_u16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static inline void notify_rec_put_u32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline void notify_rec_put_u64(uint8_t *b, uint64_t v) {
    notify_rec_put_u32(b, (uint32_t)v);
    notify_rec_put_u32(b + 4, (uint32_t)(v >> 32));
}
static inline uint16_t notify_rec_get_u16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t notify_rec_get_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t notify_rec_get_u64(const uint8_t *b) {
    return (uint64_t)notify_rec_get_u32(b) |
           ((uint64_t)notify_rec_get_u32(b + 4) << 32);
}

/* Bounded C-string length: stop at the first NUL or at cap-1, whichever comes
 * first. cap includes the NUL, so the returned length is <= cap-1. */
static inline size_t notify_rec__slen(const char *s, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap && s[n]) n++;
    return n;
}

/*
 * Encode `r` into `out` (capacity `cap`). Returns the number of bytes written,
 * or 0 if `out` is too small or an argument is NULL. The output is always <=
 * NOTIFY_REC_MAX, so a NOTIFY_REC_MAX buffer never fails on size.
 */
static inline size_t notify_rec_encode(const notify_rec *r, uint8_t *out, size_t cap) {
    if (!r || !out) return 0;

    size_t sl = notify_rec__slen(r->source, NR_SOURCE_CAP);
    size_t tl = notify_rec__slen(r->title,  NR_TITLE_CAP);
    size_t bl = notify_rec__slen(r->body,   NR_BODY_CAP);
    size_t kl = notify_rec__slen(r->key,    NR_KEY_CAP);

    size_t need = (size_t)NOTIFY_REC_HEAD + 1 + sl + 1 + tl + 2 + bl + 1 + kl;
    if (notify_event_id_valid(&r->event_id)) need += NOTIFY_REC_EVENT_SUFFIX;
    if (need > cap) return 0;

    size_t o = 0;
    out[o++] = (uint8_t)NOTIFY_REC_VER;
    notify_rec_put_u32(out + o, r->id);            o += 4;
    notify_rec_put_u32(out + o, r->ttl_s);         o += 4;
    notify_rec_put_u32(out + o, r->created_epoch); o += 4;
    out[o++] = r->level;
    out[o++] = (uint8_t)(r->unread ? 1 : 0);

    out[o++] = (uint8_t)sl; if (sl) { memcpy(out + o, r->source, sl); o += sl; }
    out[o++] = (uint8_t)tl; if (tl) { memcpy(out + o, r->title,  tl); o += tl; }
    notify_rec_put_u16(out + o, (uint16_t)bl); o += 2;
    if (bl) { memcpy(out + o, r->body, bl); o += bl; }
    out[o++] = (uint8_t)kl; if (kl) { memcpy(out + o, r->key, kl); o += kl; }

    if (notify_event_id_valid(&r->event_id)) {
        out[o++] = NOTIFY_REC_EVENT_TAG;
        notify_rec_put_u64(out + o, r->event_id.epoch_hi); o += 8;
        notify_rec_put_u64(out + o, r->event_id.epoch_lo); o += 8;
        notify_rec_put_u64(out + o, r->event_id.counter);  o += 8;
        out[o++] = r->event_distinct ? 1 : 0;
    }

    return o;
}

/* Read one length-prefixed field (`plen` bytes of prefix) into a fixed dst of
 * capacity `dstcap` (incl. NUL). Advances *off. Returns 0 on a malformed frame
 * (prefix or bytes run past `avail`, or a length beyond the field cap). */
static inline int notify_rec__get_field(const uint8_t *buf, size_t avail, size_t *off,
                                        int plen, char *dst, size_t dstcap) {
    if (*off + (size_t)plen > avail) return 0;
    size_t len = (plen == 2) ? notify_rec_get_u16(buf + *off)
                             : (size_t)buf[*off];
    *off += (size_t)plen;
    if (len > dstcap - 1) return 0;         /* declared length beyond the field cap */
    if (*off + len > avail) return 0;       /* body runs past the buffer */
    if (len) memcpy(dst, buf + *off, len);
    dst[len] = '\0';
    *off += len;
    return 1;
}

/*
 * Decode the blob in [buf, buf+avail) into *out. Returns 1 on success, 0 on a
 * malformed/short frame or wrong version. Every field is bounded, so a hostile
 * or truncated payload can never overrun — it is refused. *out is fully written
 * on success (memset first), untouched on failure.
 */
static inline int notify_rec_decode(const uint8_t *buf, size_t avail, notify_rec *out) {
    if (!buf || !out) return 0;
    if (avail < (size_t)NOTIFY_REC_HEAD) return 0;
    if (buf[0] != NOTIFY_REC_VER) return 0;

    notify_rec tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t o = 1;
    tmp.id            = notify_rec_get_u32(buf + o); o += 4;
    tmp.ttl_s         = notify_rec_get_u32(buf + o); o += 4;
    tmp.created_epoch = notify_rec_get_u32(buf + o); o += 4;
    tmp.level  = buf[o++];
    tmp.unread = buf[o++] ? 1 : 0;

    if (!notify_rec__get_field(buf, avail, &o, 1, tmp.source, NR_SOURCE_CAP)) return 0;
    if (!notify_rec__get_field(buf, avail, &o, 1, tmp.title,  NR_TITLE_CAP))  return 0;
    if (!notify_rec__get_field(buf, avail, &o, 2, tmp.body,   NR_BODY_CAP))   return 0;
    if (!notify_rec__get_field(buf, avail, &o, 1, tmp.key,    NR_KEY_CAP))    return 0;

    /* Version 1 readers ignored trailing bytes, so the tagged suffix extends
     * the existing frame without invalidating any archived v1 card. A missing
     * suffix is a legacy zero identity; a partial or unknown suffix is corrupt. */
    if (o < avail) {
        size_t suffix_n = avail - o;
        if ((suffix_n != NOTIFY_REC_EVENT_SUFFIX && suffix_n != 25) ||
            buf[o++] != NOTIFY_REC_EVENT_TAG) return 0;
        tmp.event_id.epoch_hi = notify_rec_get_u64(buf + o); o += 8;
        tmp.event_id.epoch_lo = notify_rec_get_u64(buf + o); o += 8;
        tmp.event_id.counter  = notify_rec_get_u64(buf + o); o += 8;
        if (!notify_event_id_valid(&tmp.event_id)) return 0;
        if (suffix_n == NOTIFY_REC_EVENT_SUFFIX)
            tmp.event_distinct = buf[o++] ? 1 : 0;
        else {
            size_t key_n = strlen(tmp.key);
            tmp.event_distinct = key_n > 5 &&
                strcmp(tmp.key + key_n - 5, "-chat") == 0;
        }
    }

    *out = tmp;
    return 1;
}

/* --- archive-key derivation (the (NS_NOTIFY,key) upsert handle) -------------
 *
 * A card is archived under either its client-supplied dedup key OR, when it has
 * none, a synthetic key derived from its numeric id. Those two sources share ONE
 * archive key space, so a raw "%u" id-key could COLLIDE with a client key that
 * happens to be the same decimal string: client key "5" (some card) and a keyless
 * card whose id is 5 would both map to (NS_NOTIFY,"5"), and the later write would
 * upsert over the earlier — silently losing one card's restorable copy on reboot.
 *
 * The fix: prefix the SYNTHETIC id-key with a reserved sentinel char that a client
 * dedup key can never occupy, so the two sources live in DISJOINT subspaces
 * (client keys vs "#<id>"). '#' (0x23) is printable ASCII (valid per
 * micron_store_key_valid) and is used by no real producer. "#" + max uint32
 * decimal = 12 bytes incl. NUL, well under MICRON_STORE_KEY_MAX(33). */
#define NOTIFY_ARCHIVE_IDKEY_SENTINEL '#'

/* The synthetic-id-key needs '#' + up to 10 digits of uint32 + NUL = 12 bytes. */
#define NOTIFY_ARCHIVE_IDKEY_CAP 12

/*
 * Derive the archive key for a card into `out` (cap must be >= NOTIFY_ARCHIVE_IDKEY_CAP)
 * and return the key to store under. INVARIANT: client dedup keys and synthetic
 * id-keys occupy disjoint archive subspaces.
 *
 * Uses the card's own dedup `key` when it is a usable archive key that does NOT
 * begin with the sentinel; otherwise the synthetic "#<id>". A (hostile/degenerate)
 * client key that DOES begin with the sentinel falls back to the id-key rather
 * than being used raw — chosen over rejecting the key at the boundary because the
 * fall-back never drops the card, and real client keys never start with '#'. The
 * archive key is only the dedup/upsert handle; the card's real id and dedup key
 * travel in the payload, so restore is unaffected by which handle was chosen.
 */
static inline const char *notify_rec_archive_key(const char *key, uint32_t id,
                                                 char *out, size_t cap) {
    if (key && key[0] && key[0] != NOTIFY_ARCHIVE_IDKEY_SENTINEL &&
        micron_store_key_valid(key)) {
        return key;
    }
    snprintf(out, cap, "%c%u", NOTIFY_ARCHIVE_IDKEY_SENTINEL, (unsigned)id);
    return out;
}

static inline int notify_rec_is_chat_door_key(const char *key) {
    if (!key) return 0;
    size_t n = strlen(key);
    return n > 5 && strcmp(key + n - 5, "-chat") == 0;
}

/* A keyed card replaces the previous one with the same key. Chat doors
 * (`hermes-chat`) are one doorbell, not a stack of inbox rows — the thread
 * holds the messages. */
static inline int notify_rec_key_replaces(const char *key) {
    return key && key[0];
}

static inline int notify_rec_delete_shadows_older(const char *key,
                                                  int event_distinct) {
    return notify_rec_key_replaces(key) && !event_distinct;
}

/* Restore walks archive identities newest-first. A newer ordinary keyed card
 * therefore shadows every older record carrying the same key, including
 * records archived under per-event identities by older firmware. A distinct
 * event never shadows earlier chronology, and keyless cards never shadow one
 * another. */
static inline int notify_rec_restore_shadowed(const char *older_key,
                                              const char *newer_key,
                                              int newer_event_distinct) {
    return notify_rec_key_replaces(newer_key) && !newer_event_distinct &&
           older_key && strcmp(older_key, newer_key) == 0;
}

#define NOTIFY_ARCHIVE_EVENTKEY_CAP 33

static inline const char *notify_rec_archive_event_key(
        const char *key, uint32_t id, const NotifyEventId *event,
        int event_distinct,
        char *out, size_t out_n) {
    if (event_distinct && notify_event_id_valid(event) &&
        out && out_n >= NOTIFY_ARCHIVE_EVENTKEY_CAP) {
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        uint8_t raw[24];
        notify_rec_put_u64(raw, event->epoch_hi);
        notify_rec_put_u64(raw + 8, event->epoch_lo);
        notify_rec_put_u64(raw + 16, event->counter);
        size_t o = 0;
        for (size_t i = 0; i < sizeof(raw); i += 3) {
            uint32_t v = ((uint32_t)raw[i] << 16) |
                         ((uint32_t)raw[i + 1] << 8) | raw[i + 2];
            out[o++] = alphabet[(v >> 18) & 63u];
            out[o++] = alphabet[(v >> 12) & 63u];
            out[o++] = alphabet[(v >> 6) & 63u];
            out[o++] = alphabet[v & 63u];
        }
        out[o] = '\0';
        return out;
    }
    if (event_distinct && out && out_n >= NOTIFY_ARCHIVE_IDKEY_CAP) {
        snprintf(out, out_n, "%c%lu", NOTIFY_ARCHIVE_IDKEY_SENTINEL,
                 (unsigned long)id);
        return out;
    }
    return notify_rec_archive_key(key, id, out, out_n);
}

/* --- delete tombstone: a card removed from the feed stays removed ------------
 *
 * Deleting a card writes a TOMBSTONE record to the archive under the card's
 * (NS_NOTIFY, archive-key) identity — the SAME identity notify_rec_archive_key()
 * derives for the card's data records. The archive is append-only and
 * newest-wins: the tombstone is appended AFTER the card's data record, so it
 * carries the higher seq and the index resolves that identity to it. The boot
 * restorer reads the newest record for each identity and SKIPS the ones that are
 * tombstones (notify_rec_is_tombstone), so a deleted card is never rebuilt into
 * the RAM ring — the delete survives the reboot. A later re-add of the same key
 * appends a fresh DATA record with a still-higher seq, which supersedes the
 * tombstone (newest-wins again) and the card comes back.
 *
 * A tombstone starts with a reserved marker a real notify_rec can never carry:
 * a data record's first byte is NOTIFY_REC_VER (1), so NOTIFY_REC_TOMBSTONE is
 * deliberately distinct. The original one-byte form remains valid and deletes
 * exactly its archive identity. A keyed ordinary-card delete uses the versioned
 * form below, which also carries the logical key so restore can suppress older
 * per-event identities left by legacy firmware. */
#define NOTIFY_REC_TOMBSTONE  0xEEu   /* first payload byte; != NOTIFY_REC_VER (1) */
#define NOTIFY_REC_TOMBSTONE_KEY_VER 1u
#define NOTIFY_REC_TOMBSTONE_KEY_HEAD 3u  /* marker, keyed version, key length */

/* Encode a delete tombstone into `out` (cap must be >= 1). Returns the byte
 * count written (1), or 0 if the buffer is too small. */
static inline size_t notify_rec_tombstone_encode(uint8_t *out, size_t cap) {
    if (!out || cap < 1) return 0;
    out[0] = (uint8_t)NOTIFY_REC_TOMBSTONE;
    return 1;
}

/* Encode a logical-key deletion barrier. This is used only for ordinary keyed
 * cards; distinct events and keyless cards retain identity-only tombstones. */
static inline size_t notify_rec_key_tombstone_encode(const char *key,
                                                     uint8_t *out, size_t cap) {
    if (!key || !out) return 0;
    size_t n = strnlen(key, NR_KEY_CAP);
    if (n == 0 || n >= NR_KEY_CAP || cap < NOTIFY_REC_TOMBSTONE_KEY_HEAD + n)
        return 0;
    out[0] = (uint8_t)NOTIFY_REC_TOMBSTONE;
    out[1] = (uint8_t)NOTIFY_REC_TOMBSTONE_KEY_VER;
    out[2] = (uint8_t)n;
    memcpy(out + NOTIFY_REC_TOMBSTONE_KEY_HEAD, key, n);
    return NOTIFY_REC_TOMBSTONE_KEY_HEAD + n;
}

/* Is this archived payload a delete tombstone rather than a card record? A
 * tombstone is recognised by its reserved first byte, which no notify_rec ever
 * carries — so this can never mistake a real (possibly truncated) card for one. */
static inline int notify_rec_is_tombstone(const uint8_t *buf, size_t len) {
    return buf && len >= 1 && buf[0] == (uint8_t)NOTIFY_REC_TOMBSTONE;
}

/* Extract the logical key from a versioned deletion barrier. Old one-byte
 * tombstones and unknown/malformed future forms remain tombstones, but return
 * no broad shadow key and therefore preserve their identity-only semantics. */
static inline int notify_rec_tombstone_key(const uint8_t *buf, size_t len,
                                           char *out, size_t out_n) {
    if (!notify_rec_is_tombstone(buf, len) ||
        len < NOTIFY_REC_TOMBSTONE_KEY_HEAD + 1 ||
        buf[1] != (uint8_t)NOTIFY_REC_TOMBSTONE_KEY_VER || !out)
        return 0;
    size_t n = buf[2];
    if (n == 0 || n >= NR_KEY_CAP || out_n <= n ||
        len != NOTIFY_REC_TOMBSTONE_KEY_HEAD + n ||
        memchr(buf + NOTIFY_REC_TOMBSTONE_KEY_HEAD, '\0', n))
        return 0;
    memcpy(out, buf + NOTIFY_REC_TOMBSTONE_KEY_HEAD, n);
    out[n] = '\0';
    return 1;
}
