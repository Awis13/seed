#pragma once
/*
 * notify_wire.h — pure, header-only parse of the notify wire frames the home
 * MeshCore gateway (daemon :8325) sends to the pager, isolating the ONE thing
 * every ingress must agree on: the card's stable dedup KEY.
 *
 * Host-testable, Arduino/RTOS/clock free (only libc string/stdio/stdlib):
 * `static inline`, no allocation, no globals. The TTL/slot reassembly lifecycle
 * (which needs millis()) stays in skills/meshcore.cpp; the per-frame FIELD parse —
 * including how the key is told apart from an arbitrary body/chunk — lives here so
 * the host test (tools/test_notify_wire.cpp) compiles the real code, not a copy.
 *
 * WHY A KEY FIELD AT ALL (ticket CARD-DELIVERY / C1)
 * -------------------------------------------------
 * A card that arrives over two transports must dedup to ONE archive entry. The
 * archive identity is (NS_NOTIFY, notify_rec_archive_key(key,id)), so the SAME
 * logical card must carry the SAME gateway-stamped key on every wire — P-series,
 * M-series and LXMF alike. LXMF already carries meta.key; HTTP carries JSON `id`.
 * This header brings the two MeshCore text wires up to the same contract.
 *
 * WIRE FORMATS (the gateway fragmenter MUST match these byte-for-byte)
 * -------------------------------------------------------------------
 * Single-frame notify (P-series):
 *   P1|level|source|title|body[|key]         LEGACY. key OPTIONAL and recovered
 *                                            by HEURISTIC (notify_wire_key_plausible):
 *                                            the last '|'-segment is taken as a key
 *                                            only when it looks like one. A body that
 *                                            genuinely ends in "|word-like-this" loses
 *                                            that tail. This is the fragility C1 fixes.
 *   P2|level|source|title|body|key           EXPLICIT. The v2 tag GUARANTEES a
 *                                            trailing key field, so the LAST '|'
 *                                            unambiguously ends the body and begins
 *                                            the key — NO heuristic. `body` may hold
 *                                            '|' freely; `key` never contains '|'
 *                                            (its alphabet excludes it). An empty key
 *                                            ("...|body|") means "keyless".
 *
 * Multi-part notify (M-series), one frame per fragment, reassembled by mid:
 *   M1|mid|i|n|level|source|title|chunk          LEGACY. Always KEYLESS: the last
 *                                                field is an arbitrary chunk, so a
 *                                                key sitting before it could not be
 *                                                told from the chunk itself — this is
 *                                                the main gap C1 closes.
 *   M2|mid|i|n|level|source|title|chunk|key      EXPLICIT. The v2 tag GUARANTEES a
 *                                                trailing key field, so the LAST '|'
 *                                                ends the chunk and begins the key —
 *                                                NO heuristic. `chunk` may hold '|'
 *                                                freely; `key` never contains '|'.
 *                                                The gateway repeats the SAME key on
 *                                                every fragment; the device keeps the
 *                                                one from the first fragment it sees.
 *                                                An empty key ("...|chunk|") = keyless.
 *
 * WHY A VERSION TAG RATHER THAN AN OPTIONAL IN-BAND FIELD
 * ------------------------------------------------------
 * On a '|'-delimited TEXT wire whose trailing field is arbitrary (a body / a chunk
 * that may itself contain '|'), an OPTIONAL extra field cannot be added
 * unambiguously: a new keyed frame is indistinguishable from an old keyless one
 * whose last field happens to end in a key-shaped token — which is exactly the
 * heuristic P1 already pays for. Bumping the version digit (P1->P2, M1->M2, in the
 * same family as this project's L1/C1/R1 tags) is the cleanest backward-compatible
 * form: legacy senders keep working (P1 heuristic, M1 keyless) and a v2 sender is
 * parsed with ZERO ambiguity. See tools/test_notify_wire.cpp.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* atoi     */

/* Field caps — mirror NOTIFY_*_LEN in skills/notify.cpp. notify.cpp pins the
 * equality with static_asserts so the two cannot drift. */
#define NW_LEVEL_CAP    8    /* "info"/"warn"/"crit" + NUL */
#define NW_SOURCE_CAP  17    /* 16 chars + NUL             */
#define NW_TITLE_CAP   61    /* 60 chars + NUL             */
#define NW_KEY_CAP     25    /* 24 chars + NUL             */
#define NW_MID_CAP      8    /* short multipart id + NUL   */

/* A parsed P-series frame. `body` points INTO the source string (not owned); the
 * caller copies it through notify_copy_text(), which bounds and UTF-8-trims it. */
typedef struct {
    char        level[NW_LEVEL_CAP];
    char        source[NW_SOURCE_CAP];
    char        title[NW_TITLE_CAP];
    char        key[NW_KEY_CAP];    /* "" when the frame carried none */
    const char *body;
    size_t      body_len;
} NotifyPFrame;

/* A parsed M-series fragment. `chunk` points INTO the source string (not owned). */
typedef struct {
    char        mid[NW_MID_CAP];
    int         part;               /* i, 1-based           */
    int         n_parts;            /* n                    */
    char        level[NW_LEVEL_CAP];
    char        source[NW_SOURCE_CAP];
    char        title[NW_TITLE_CAP];
    char        key[NW_KEY_CAP];    /* "" when the frame carried none */
    const char *chunk;
    size_t      chunk_len;
} NotifyMFrame;

/*
 * Could `s` be a client dedup key rather than the tail of a body?
 *
 * Used ONLY on the LEGACY P1 path, where the key is an optional trailing field and
 * telling it from a body that may hold separators of its own means guessing. The
 * alphabet is the one every key this pager is sent already uses — "pingani-a1b2",
 * "hermes-chat", "k1c.print" — and it deliberately excludes the space and '|', which
 * is what keeps ordinary prose from qualifying. The v2 (P2/M2) tags need no guess.
 */
static inline int notify_wire_key_plausible(const char *s) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n >= NW_KEY_CAP) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':')
            continue;
        return 0;
    }
    return 1;
}

/* Copy up to `cap`-1 bytes of a '|'-terminated (or NUL-terminated) field into a
 * fixed buffer and advance *pp past the terminator. Returns 1 if a '|' terminated
 * the field, 0 if the string ended first (a malformed frame for a non-final
 * field). The copy is bounded; an over-long field is truncated to the cap. */
static inline int notify_wire__field(const char **pp, char *dst, size_t cap) {
    const char *p = *pp;
    const char *bar = strchr(p, '|');
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    size_t c = n; if (c > cap - 1) c = cap - 1;
    memcpy(dst, p, c);
    dst[c] = '\0';
    if (!bar) { *pp = p + n; return 0; }
    *pp = bar + 1;
    return 1;
}

/*
 * Parse a P-series notify frame (P1 legacy / P2 explicit) into *out. Returns 1 on
 * success, 0 if the string is not a P-frame or a fixed field (level/source/title)
 * is missing. `out` is zeroed first; out->body points into `wire`.
 */
static inline int notify_wire_parse_p(const char *wire, NotifyPFrame *out) {
    if (!wire || !out) return 0;
    memset(out, 0, sizeof(*out));
    /* Guard wire[1]!='\0' before reading wire[2]: a single-char frame ("P") would
     * otherwise read one byte past the NUL terminator (OOB). */
    if (wire[0] != 'P' || wire[1] == '\0' || wire[2] != '|') return 0;
    char ver = wire[1];
    if (ver != '1' && ver != '2') return 0;

    const char *p = wire + 3;
    if (!notify_wire__field(&p, out->level,  NW_LEVEL_CAP))  return 0;
    if (!notify_wire__field(&p, out->source, NW_SOURCE_CAP)) return 0;
    if (!notify_wire__field(&p, out->title,  NW_TITLE_CAP))  return 0;

    /* `p` now points at the remainder: the body, plus (for P2 always, for P1
     * maybe) a trailing "|key". */
    const char *rest = p;
    size_t rest_len = strlen(rest);
    const char *last = strrchr(rest, '|');

    if (ver == '2') {
        /* v2 GUARANTEES a trailing key field: the last '|' ends the body and
         * begins the key, whatever the body holds. No heuristic. */
        if (last) {
            snprintf(out->key, sizeof(out->key), "%s", last + 1);
            out->body = rest;
            out->body_len = (size_t)(last - rest);
        } else {
            /* No '|' at all: tolerate as a keyless body rather than dropping it. */
            out->body = rest;
            out->body_len = rest_len;
        }
        return 1;
    }

    /* v1 LEGACY: recover an optional trailing key only when it looks like one. */
    if (last && notify_wire_key_plausible(last + 1)) {
        snprintf(out->key, sizeof(out->key), "%s", last + 1);
        out->body = rest;
        out->body_len = (size_t)(last - rest);
    } else {
        out->body = rest;
        out->body_len = rest_len;
    }
    return 1;
}

/*
 * Parse ONE M-series fragment frame (M1 legacy / M2 explicit) into *out. Returns 1
 * on success, 0 on any malformation (wrong prefix, missing separator, non-decimal
 * i/n). `out` is zeroed first; out->chunk points into `frame`.
 */
static inline int notify_wire_parse_m(const char *frame, NotifyMFrame *out) {
    if (!frame || !out) return 0;
    memset(out, 0, sizeof(*out));
    /* Guard frame[1]!='\0' before reading frame[2]: a single-char frame ("M") would
     * otherwise read one byte past the NUL terminator (OOB). */
    if (frame[0] != 'M' || frame[1] == '\0' || frame[2] != '|') return 0;
    char ver = frame[1];
    if (ver != '1' && ver != '2') return 0;

    const char *p = frame + 3;
    if (!notify_wire__field(&p, out->mid, NW_MID_CAP)) return 0;

    /* i, n: decimal, each '|'-terminated. */
    char num[8];
    if (!notify_wire__field(&p, num, sizeof(num))) return 0;
    out->part = atoi(num);
    if (!notify_wire__field(&p, num, sizeof(num))) return 0;
    out->n_parts = atoi(num);

    if (!notify_wire__field(&p, out->level,  NW_LEVEL_CAP))  return 0;
    if (!notify_wire__field(&p, out->source, NW_SOURCE_CAP)) return 0;
    if (!notify_wire__field(&p, out->title,  NW_TITLE_CAP))  return 0;

    /* Remainder: the chunk, plus (M2 only) a trailing "|key". */
    const char *rest = p;
    size_t rest_len = strlen(rest);

    if (ver == '2') {
        const char *last = strrchr(rest, '|');
        if (last) {
            snprintf(out->key, sizeof(out->key), "%s", last + 1);
            out->chunk = rest;
            out->chunk_len = (size_t)(last - rest);
        } else {
            out->chunk = rest;
            out->chunk_len = rest_len;
        }
    } else {
        /* M1: the whole remainder is the chunk; the frame carries no key. */
        out->chunk = rest;
        out->chunk_len = rest_len;
    }
    return 1;
}
