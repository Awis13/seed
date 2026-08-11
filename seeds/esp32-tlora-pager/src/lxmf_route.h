#pragma once
/*
 * lxmf_route.h — pure, header-only routing decision for a parsed LXMF message.
 *
 * Given a message the C1 codec has already parsed (src/lxmf_codec.h), decide
 * WHERE it goes on the pager: a notification card, or a chat room. This is the
 * one piece of the receive path that is a pure function of the message's fields,
 * so — exactly like agents_chat_route.h and rns/inbox.h — it lives in a header
 * with NO Arduino / FreeRTOS / SD / RNS dependency (only <string.h> and the
 * fixed-integer types), holds no state, allocates nothing, and is exercised by a
 * host test (tools/test_lxmf_route.cpp).
 *
 * What is decided HERE (pure): card-vs-room, the severity string, the card
 * source label, the card dedup/replace key, and — for a room — the room name.
 * What is NOT here: the text itself. Title/content are sanitised through
 * rns_text_sanitize() by the caller (rns_lxmf_inbox_poll in skills/rns.cpp) into
 * the same card/room buffers the seed.pager path uses, so the two receive paths
 * strip control bytes and bound length identically.
 *
 * ROUTING RULES (see TLORA-LXMF C3)
 * ---------------------------------
 *   - THREAD (FIELD 0x08) present  -> ROOM. The message belongs to a running
 *     conversation; it is routed into that room INSTEAD of raising a card. The
 *     room name is the thread bytes, NUL-terminated and bounded; the chat router
 *     (claude_route_incoming) sanitises the name itself.
 *   - otherwise                    -> CARD. The DEFAULT, and the milestone path:
 *     a plain client (Retichat) sends only title+content and no custom fields,
 *     and it MUST land as a card.
 *   - SEVERITY: meta.sev (0xFD) -> 0=info, 1=warn, >=2=crit; absent -> info.
 *   - SOURCE:   meta.src (0xFD) if present, else the constant "lxmf".
 *   - KEY (card replace-in-place): meta.key (0xFD) if present, else a DERIVED
 *     stable-but-unique default (see lxmf_route_derive_key below).
 *   - RENDERER (0x0F) does NOT affect routing. v1 renders every card as plain;
 *     RENDERER_MICRON is treated as plain here — a micron-page body is a
 *     separate later ticket, not built in this commit.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "lxmf_codec.h"

/* Card-vs-room outcome. */
typedef enum {
    LXMF_ROUTE_CARD = 0,   /* raise a notification card                          */
    LXMF_ROUTE_ROOM = 1    /* route into a chat room (a thread was present)      */
} LxmfRouteKind;

/* Fixed caps for the derived strings. These mirror the notify field sizes
 * (NOTIFY_SOURCE_LEN 17, NOTIFY_KEY_LEN 25 in skills/notify.cpp) so a value that
 * fits here fits the card; notify_copy_text() bounds again on ingest regardless.
 * The room cap follows the codec's thread cap. */
#define LXMF_ROUTE_LEVEL_CAP   8                    /* "info"/"warn"/"crit"       */
#define LXMF_ROUTE_SOURCE_CAP  17                   /* 16 chars + NUL             */
#define LXMF_ROUTE_KEY_CAP     25                   /* 24 chars + NUL             */
#define LXMF_ROUTE_ROOM_CAP    (LXMF_THREAD_CAP + 1)

typedef struct {
    LxmfRouteKind kind;
    char level[LXMF_ROUTE_LEVEL_CAP];   /* card: severity string                 */
    char source[LXMF_ROUTE_SOURCE_CAP]; /* card: source label                    */
    char key[LXMF_ROUTE_KEY_CAP];       /* card: dedup / replace-in-place key     */
    char room[LXMF_ROUTE_ROOM_CAP];     /* room: the conversation name           */
} LxmfRoute;

/* FNV-1a 32-bit over a byte range, chained through an accumulator so several
 * fields fold into one hash. Deterministic and dependency-free — the whole
 * reason the derived key is stable across identical repeats. */
static inline uint32_t lxmf_route_fnv1a(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static inline void lxmf_route_hex_u32(char *dst, uint32_t v) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) dst[i] = H[(v >> ((7 - i) * 4)) & 0xF];
}

/*
 * Derive a card key for a message that carried none of its own (meta.key).
 *
 * The key both DEDUPES and REPLACES: notify_ingest() replaces a stored card that
 * shares the key. So the requirement is "same message -> same key" (a client
 * that re-sends the identical notification updates in place rather than stacking
 * forever) AND "different message -> different key" (two distinct messages must
 * not clobber each other). The derivation meets both by folding two independent
 * axes into the key:
 *   - the first 4 bytes of the SOURCE hash (who sent it), as 8 hex; and
 *   - an FNV-1a hash of TITLE then CONTENT (what it said), as 8 hex.
 * so "lxmf-<8 hex source>-<8 hex title+content>" (22 chars, inside the 24 cap).
 * Two messages collide only if BOTH the source prefix and the title+content hash
 * collide, which an ordinary sender never provokes; a byte-identical resend maps
 * to the identical key and updates the card, which is the intent.
 */
static inline void lxmf_route_derive_key(const LxmfMsg *m, char *dst, size_t cap) {
    /* "lxmf-" + 8 + "-" + 8 + NUL = 23 bytes; guarded against a smaller cap. */
    char buf[24];
    memcpy(buf, "lxmf-", 5);
    lxmf_route_hex_u32(buf + 5, lxmf_route_fnv1a(m->source_hash, 4, 2166136261u));
    buf[13] = '-';
    uint32_t h = lxmf_route_fnv1a(m->title, m->title_len, 2166136261u);
    h = lxmf_route_fnv1a(m->content, m->content_len, h);
    lxmf_route_hex_u32(buf + 14, h);
    buf[22] = '\0';
    if (cap == 0) return;
    size_t n = strlen(buf);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, buf, n);
    dst[n] = '\0';
}

/* Copy a possibly-untrusted label into a bounded, NUL-terminated buffer, mapping
 * control bytes (< 0x20 or 0x7F) to '.' the way rns_text_sanitize does for a
 * card body — the source label is drawn on the card header just the same. */
static inline void lxmf_route_copy_label(char *dst, size_t cap,
                                         const char *src, size_t src_len) {
    size_t o = 0;
    if (cap == 0) return;
    for (size_t i = 0; i < src_len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c < 0x20 || c == 0x7F) ? '.' : (char)c;
    }
    dst[o] = '\0';
}

/*
 * Plan the route for a parsed message. `out` is fully written on every call.
 * Pure: reads only `m`, touches no I/O and no globals.
 */
static inline void lxmf_route_plan(const LxmfMsg *m, LxmfRoute *out) {
    memset(out, 0, sizeof(*out));

    /* Severity first — it is used only by the card, but computing it always
     * keeps the mapping in one place. 0=info, 1=warn, >=2=crit; absent=info. */
    const char *lvl = "info";
    if (m->has_meta && m->meta_has_sev) {
        if (m->meta_sev == 1)      lvl = "warn";
        else if (m->meta_sev >= 2) lvl = "crit";
    }
    memcpy(out->level, lvl, strlen(lvl) + 1);

    /* THREAD present -> room. A thread of zero length is treated as absent: it
     * names no conversation, so it falls through to the card. */
    if (m->has_thread && m->thread_len > 0) {
        out->kind = LXMF_ROUTE_ROOM;
        size_t n = m->thread_len;
        if (n > LXMF_ROUTE_ROOM_CAP - 1) n = LXMF_ROUTE_ROOM_CAP - 1;
        memcpy(out->room, m->thread, n);
        out->room[n] = '\0';
        return;
    }

    /* CARD — the default, and the milestone path. */
    out->kind = LXMF_ROUTE_CARD;

    /* Source label: meta.src if present, else the "lxmf" constant. */
    if (m->has_meta && m->meta_src_len > 0)
        lxmf_route_copy_label(out->source, sizeof(out->source),
                              m->meta_src, m->meta_src_len);
    else
        memcpy(out->source, "lxmf", 5);

    /* Replace-in-place key: meta.key if present, else the derived default. */
    if (m->has_meta && m->meta_key_len > 0)
        lxmf_route_copy_label(out->key, sizeof(out->key),
                              m->meta_key, m->meta_key_len);
    else
        lxmf_route_derive_key(m, out->key, sizeof(out->key));
}
