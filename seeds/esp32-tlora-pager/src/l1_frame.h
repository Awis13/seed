#pragma once
/*
 * l1_frame.h — pure, header-only parser + reassembler for the "L1" MeshCore frame
 * that carries a fragmented LXMF opportunistic wire.
 *
 * Host-testable, Arduino/RTOS-free (only base64url.h + <string.h>/<stddef.h>/
 * <stdint.h>): `static inline`, no dynamic allocation, no clock. The TTL/slot
 * lifecycle (which needs millis()) stays in skills/meshcore.cpp; the PARSE and the
 * byte ACCUMULATION — the untrusted-input surface — live here so the host test
 * compiles the real code, not a copy.
 *
 * WIRE FORMAT (the gateway fragmenter MUST match this byte-for-byte)
 * -----------------------------------------------------------------
 *   L1|mid|i|n|<base64url-chunk>
 *     mid  short message id, 1..7 chars, opaque, groups the parts of one message
 *     i    part index, 1-based, decimal, 1 <= i <= n
 *     n    total parts, decimal, 1 <= n <= L1_MAX_PARTS (32)
 *     <base64url-chunk>  this part's slice of the LXMF opportunistic wire
 *                        (source(16)+sig(64)+msgpack), base64url per base64url.h
 *   The chunk is everything after the fourth '|' to the end of the frame, so it
 *   may not itself contain '|' — base64url guarantees it does not.
 *
 * The frames of one message are sent IN ORDER (i = 1, 2, … n). Reassembly is
 * strictly sequential: it accepts only the next expected part, silently ignores a
 * duplicate of an already-seen part, and REJECTS a forward gap. A byte-exact LXMF
 * wire cannot tolerate a missing middle chunk — a gap must fail cleanly here
 * rather than silently corrupt the bytes handed to the codec (this is stricter
 * than the M1/C1 text reassembler, which best-effort appends across a gap because
 * a dropped line of a chat is survivable and a dropped 16 bytes of a signature is
 * not).
 */

#include "base64url.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define L1_MAX_PARTS        32     /* n ceiling: ≤32 parts, per the transport budget */
#define L1_MID_CAP          8      /* mid buffer incl NUL — mid is 1..7 chars        */
/* One frame's DECODED chunk ceiling. A frame's text budget is ~150 B (meshcore's
 * per-frame limit), whose base64url decodes to ≤ ~112 B; 160 is comfortable slack
 * and every decode is still bounded against it. */
#define L1_CHUNK_DECODED_MAX 160
/* The reassembled LXMF opportunistic wire ceiling. The single-packet LXMF wire is
 * ≤ ~391 B and the opportunistic form drops the leading 16-byte dest hash, so the
 * true bytes are ≤ ~375; 512 clears that with margin and stays far under the M1/C1
 * MESH_REASM_MAX (1800). The codec's own reconstruction buffer (16 + 383) is the
 * final gate: a wire that somehow overshoots is rejected there, never written OOB. */
#define L1_REASM_MAX        512

/* One parsed L1 frame: header fields plus this part's decoded chunk bytes. */
typedef struct {
    char    mid[L1_MID_CAP];
    int     part;                       /* i, 1..n_parts                      */
    int     n_parts;                    /* n, 1..L1_MAX_PARTS                 */
    uint8_t chunk[L1_CHUNK_DECODED_MAX];
    size_t  chunk_len;
} L1Frame;

/* Sequential byte reassembler for one message id. Zero-initialise before first
 * feed (n_parts == 0 marks "empty"). */
typedef struct {
    uint8_t  buf[L1_REASM_MAX];
    uint16_t len;
    uint8_t  n_parts;
    uint8_t  got_count;                 /* highest contiguous part accepted   */
} L1Reasm;

/* Parse a bounded non-negative decimal field terminated by `term`. Advances *pp
 * past the terminator on success. Requires at least one digit and guards against
 * an absurd magnitude. */
static inline bool l1_uint_field(const char **pp, char term, int *out_v) {
    const char *p = *pp;
    if (*p < '0' || *p > '9') return false;         /* need at least one digit */
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 100000) return false;               /* absurd magnitude guard  */
        p++;
    }
    if (*p != term) return false;                   /* stray non-digit / early end */
    *pp = p + 1;
    *out_v = v;
    return true;
}

/* Parse one "L1|mid|i|n|<b64>" frame from a NUL-terminated string into `out`.
 * Returns false on any malformation: wrong prefix, empty/oversize mid, a missing
 * separator, a non-decimal i/n, i/n out of range (n in 1..L1_MAX_PARTS, i in
 * 1..n), or an undecodable / oversize base64url chunk. `out` is zeroed first;
 * every write is bounded. */
static inline bool l1_parse(const char *text, L1Frame *out) {
    if (!text || !out) return false;
    memset(out, 0, sizeof(*out));
    if (strncmp(text, "L1|", 3) != 0) return false;

    const char *p = text + 3;

    /* mid: up to the first '|' */
    const char *bar = strchr(p, '|');
    if (!bar) return false;
    size_t midlen = (size_t)(bar - p);
    if (midlen == 0 || midlen >= L1_MID_CAP) return false;
    memcpy(out->mid, p, midlen);
    out->mid[midlen] = '\0';
    p = bar + 1;

    /* i, n */
    int i_part = 0, n_parts = 0;
    if (!l1_uint_field(&p, '|', &i_part)) return false;
    if (!l1_uint_field(&p, '|', &n_parts)) return false;
    if (n_parts < 1 || n_parts > L1_MAX_PARTS) return false;
    if (i_part < 1 || i_part > n_parts) return false;

    /* the rest of the string is the base64url chunk (no '|' inside it) */
    size_t clen = strlen(p);
    if (!b64url_decode(p, clen, out->chunk, sizeof(out->chunk), &out->chunk_len))
        return false;

    out->part = i_part;
    out->n_parts = n_parts;
    return true;
}

/* Feed one parsed frame into the reassembler. Returns:
 *    1  message complete — r->buf[0 .. r->len) is the whole LXMF wire
 *    0  accepted, more parts expected (or a harmless duplicate ignored)
 *   -1  rejected — inconsistent n, a forward gap, or an overflow; the caller
 *       should drop this reassembly slot and count a drop.
 */
static inline int l1_reasm_feed(L1Reasm *r, const L1Frame *f) {
    if (!r || !f) return -1;
    if (f->n_parts < 1 || f->n_parts > L1_MAX_PARTS) return -1;
    if (f->part < 1 || f->part > f->n_parts) return -1;

    if (r->n_parts == 0) r->n_parts = (uint8_t)f->n_parts;
    if ((uint8_t)f->n_parts != r->n_parts) return -1;      /* n changed mid-stream */

    if (f->part != (int)(r->got_count + 1)) {
        if (f->part <= r->got_count) return 0;             /* duplicate: ignore    */
        return -1;                                         /* forward gap: reject  */
    }
    if (f->chunk_len > (size_t)(L1_REASM_MAX - r->len)) return -1;  /* overflow */
    if (f->chunk_len) {
        memcpy(r->buf + r->len, f->chunk, f->chunk_len);
        r->len = (uint16_t)(r->len + f->chunk_len);
    }
    r->got_count = (uint8_t)f->part;
    return (r->got_count >= r->n_parts) ? 1 : 0;
}
