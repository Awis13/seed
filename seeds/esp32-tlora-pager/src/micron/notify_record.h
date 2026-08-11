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
 * Persisted: id, level, unread, ttl_s, created_epoch, source, title, body, key.
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
 *   ------------------------------ NOTIFY_REC_MAX = 360 bytes  (< 512)
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

/* Worst-case encoded size (see the budget note). The device sizes its enqueue
 * buffer to this; it is well under HISTORY_PAYLOAD_CAP (512). */
#define NOTIFY_REC_MAX (NOTIFY_REC_HEAD + 1 + (NR_SOURCE_CAP - 1) \
                        + 1 + (NR_TITLE_CAP - 1) + 2 + (NR_BODY_CAP - 1) \
                        + 1 + (NR_KEY_CAP - 1))

/* The persisted projection of a Notification. POD, no Arduino types: created_ms
 * is intentionally absent (device-relative), options/chosen absent (ephemeral). */
typedef struct {
    uint32_t id;
    uint32_t ttl_s;
    uint32_t created_epoch;   /* seconds since epoch, 0 when the clock was unset */
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
static inline uint16_t notify_rec_get_u16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t notify_rec_get_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
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
