#pragma once
/*
 * lxmf_codec.h — pure, header-only LXMF (v1.1.1) message codec.
 *
 * This is host-testable code with NO Arduino / FreeRTOS / SD / RNS / crypto
 * dependency (only <string.h>/<stddef.h>/<stdint.h>), modelled on the shape of
 * agents_chat_route.h: everything is `static inline`, no dynamic allocation, no
 * global state. It parses an inbound LXMF message out of hostile network bytes
 * and encodes a reply payload; the Ed25519 signing itself stays OUT of here (see
 * the sign-injection contract below).
 *
 * WIRE FORMAT (verified against LXMF 1.1.1 reference LXMessage.py)
 * ---------------------------------------------------------------
 *   packed = dest_hash(16) + source_hash(16) + signature(64) + msgpack(payload)
 *   payload = [ timestamp(double), title(bytes), content(bytes), fields(map) ]
 *
 *   - dest/source are RNS truncated identity hashes: TRUNCATED_HASHLENGTH/8 = 16
 *     bytes each (RNS/Reticulum.py TRUNCATED_HASHLENGTH = 128 bits).
 *   - signature is an Ed25519 sig: SIGLENGTH/8 = KEYSIZE/8 = 512/8 = 64 bytes
 *     (RNS/Identity.py KEYSIZE = 256*2, SIGLENGTH = KEYSIZE).
 *   - payload is a msgpack 4-element array (LXMessage.py:362 pack,
 *     :765-768 unpack); an OPTIONAL 5th element "stamp" may be appended and MUST
 *     be stripped back to 4 before use (LXMessage.py:755-758). We accept a 4- or
 *     5-element array and ignore the 5th.
 *   - unpack offsets (LXMessage.py:748-751): [0:16]=dest, [16:32]=source,
 *     [32:96]=Ed25519 signature, [96:]=msgpack payload.
 *   - overhead is 112 bytes (16 dest + 16 source + 64 sig + 8 timestamp + 8
 *     msgpack struct, LXMessage.py:55-63); the single-packet content ceiling is
 *     295 bytes (LXMessage.py:70-79 ENCRYPTED_PACKET_MAX_CONTENT). Inbound wire
 *     bytes are therefore <= ~391 (single encrypted packet MDU). We do NOT trust
 *     that bound: EVERY read is bounded against the supplied len.
 *
 * FIELD CONSTANTS (verified verbatim from LXMF.py:1-101)
 * -----------------------------------------------------
 *   FIELD_THREAD      = 0x08   FIELD_RENDERER    = 0x0F
 *   FIELD_CUSTOM_TYPE = 0xFB   FIELD_CUSTOM_DATA = 0xFC   FIELD_CUSTOM_META = 0xFD
 *   RENDERER_PLAIN = 0x00   RENDERER_MICRON = 0x01   RENDERER_MARKDOWN = 0x02
 *
 * There is NO FIELD_TYPE in this version. Our notification "type" therefore rides
 * in FIELD_CUSTOM_TYPE (0xFB, a short string/bytes), its opaque body in
 * FIELD_CUSTOM_DATA (0xFC, bytes), and severity/source/key/ttl metadata in
 * FIELD_CUSTOM_META (0xFD), which we define as a msgpack map with string keys:
 *   { "sev": uint, "src": str, "key": str, "ttl": uint }
 * Standard clients (e.g. Retichat) send a plain message with NO custom fields;
 * that MUST parse OK on title/content alone. Unknown/unhandled fields are NEVER a
 * failure — they are skipped, and the message still parses OK.
 *
 * msgpack: HAND-ROLLED here (not the hideakitai/MsgPack lib). That library is
 * Arduino-coupled (pulls DebugLog / ArxContainer / ArxTypeTraits and the
 * `arduino::` namespace) and would not compile in a bare host test, nor keep this
 * header Arduino-free. We implement only the shapes LXMF actually uses:
 * fixarray/array16, float32/float64, positive/uint8..64 ints, str/bin (8/16/32
 * and fixstr), fixmap/map16/map32, nil; plus a bounded generic skip for any other
 * (unknown-field) value, with a hard recursion-depth guard against hostile
 * nesting. No dynamic allocation.
 *
 * SIGN-INJECTION CONTRACT (the codec is crypto-free)
 * --------------------------------------------------
 * Encoding is split so no hashing/signing lives here:
 *   1. lxmf_encode_sign_input()  -> emits hashed_part = dest + source +
 *      msgpack(payload). The CALLER computes message_hash = full_hash(hashed_part)
 *      (RNS full_hash = SHA-256, 32 bytes), forms signed_part = hashed_part +
 *      message_hash, and Ed25519-signs it to obtain sig[64] (LXMessage.py:364-378).
 *   2. lxmf_encode_packed(sig)   -> emits the final wire message dest + source +
 *      sig + msgpack(payload), building the SAME payload as step 1 so the bytes
 *      the caller signed match the bytes that ship.
 * All crypto (SHA-256 + Ed25519) is the caller's; this file never touches keys.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* --- LXMF field / renderer constants (LXMF.py:1-101) --------------------- */
#define LXMF_FIELD_THREAD       0x08
#define LXMF_FIELD_RENDERER     0x0F
#define LXMF_FIELD_CUSTOM_TYPE  0xFB
#define LXMF_FIELD_CUSTOM_DATA  0xFC
#define LXMF_FIELD_CUSTOM_META  0xFD

#define LXMF_RENDERER_PLAIN     0x00
#define LXMF_RENDERER_MICRON    0x01
#define LXMF_RENDERER_MARKDOWN  0x02

/* --- Fixed wire geometry (LXMessage.py) ---------------------------------- */
#define LXMF_HASH_LEN     16   /* dest / source truncated identity hash        */
#define LXMF_SIG_LEN      64   /* Ed25519 signature                            */
#define LXMF_HEADER_LEN   (2 * LXMF_HASH_LEN + LXMF_SIG_LEN)   /* 96 bytes     */
#define LXMF_OVERHEAD     112  /* header + 8 timestamp + 8 msgpack struct       */
#define LXMF_CONTENT_CEIL 295  /* single-packet content ceiling                 */
#define LXMF_WIRE_MAX     391  /* single encrypted-packet MDU (informational)   */

/* --- Fixed output buffer caps (documented ceilings) ---------------------- *
 * content is sized above the 295-byte single-packet ceiling for headroom; a
 * value declared longer than its cap is rejected with LXMF_TOO_LONG, never a
 * truncated copy or an OOB write. */
#define LXMF_CONTENT_CAP  320
#define LXMF_TITLE_CAP    160
#define LXMF_TYPE_CAP     64
#define LXMF_DATA_CAP     320
#define LXMF_META_STR_CAP 64
#define LXMF_THREAD_CAP   32

/* Hard recursion-depth guard for the generic skip of unknown/nested values. */
#define LXMF_MAX_DEPTH    8

/* Parse/encode outcome. Any nonzero value is "malformed" for a later counter. */
typedef enum {
    LXMF_OK          = 0,  /* parsed / encoded cleanly                          */
    LXMF_TRUNCATED   = 1,  /* ran out of bytes (short header or short value)     */
    LXMF_BAD_MSGPACK = 2,  /* malformed msgpack type byte / over-deep nesting    */
    LXMF_BAD_SHAPE   = 3,  /* payload not a 4- or 5-element array (or bad field) */
    LXMF_TOO_LONG    = 4   /* a value / output exceeds a fixed buffer cap        */
} LxmfReason;

/*
 * Parsed / encodable message. On parse, dest_hash and source_hash always come
 * from the header; title/content are raw bytes (NOT NUL-guaranteed UTF-8, but we
 * always NUL-terminate for convenience) with their true byte length; each
 * has_* flag says whether that field was present. On encode, the caller fills the
 * same struct and presence flags select which fields are emitted.
 */
typedef struct {
    uint8_t  dest_hash[LXMF_HASH_LEN];
    uint8_t  source_hash[LXMF_HASH_LEN];

    double   timestamp;

    char     title[LXMF_TITLE_CAP + 1];
    size_t   title_len;
    char     content[LXMF_CONTENT_CAP + 1];
    size_t   content_len;

    int      has_type;                       /* FIELD_CUSTOM_TYPE 0xFB          */
    char     type[LXMF_TYPE_CAP + 1];
    size_t   type_len;

    int      has_data;                       /* FIELD_CUSTOM_DATA 0xFC          */
    uint8_t  data[LXMF_DATA_CAP];
    size_t   data_len;

    int      has_renderer;                   /* FIELD_RENDERER 0x0F             */
    uint8_t  renderer;

    int      has_thread;                     /* FIELD_THREAD 0x08               */
    uint8_t  thread[LXMF_THREAD_CAP];
    size_t   thread_len;

    int      has_meta;                       /* FIELD_CUSTOM_META 0xFD (a map)  */
    int      meta_has_sev;
    uint32_t meta_sev;
    int      meta_has_ttl;
    uint32_t meta_ttl;
    char     meta_src[LXMF_META_STR_CAP + 1];
    size_t   meta_src_len;
    char     meta_key[LXMF_META_STR_CAP + 1];
    size_t   meta_key_len;
} LxmfMsg;

/* ======================================================================== *
 *  minimal msgpack reader (bounded; hostile input safe)
 * ======================================================================== */

typedef struct { const uint8_t *p; size_t len; size_t off; } lxmf_mp_reader;

static inline int lxmf_mp_avail(const lxmf_mp_reader *r, size_t n) {
    /* NON-WRAPPING: never form r->off + n (an attacker-controlled length prefix
     * can carry n up to 0xFFFFFFFF, and on a 32-bit size_t that sum wraps and the
     * check wrongly passes). The invariant off <= len holds throughout (every
     * advance is gated by a prior lxmf_mp_avail / lxmf_mp_peek), so len - off is a
     * safe non-negative remaining-bytes count and this form cannot overflow. */
    return n <= r->len - r->off;
}

/* Read a big-endian unsigned of `n` bytes (n in 1,2,4,8) into *v. */
static inline int lxmf_mp_be(lxmf_mp_reader *r, size_t n, uint64_t *v) {
    if (!lxmf_mp_avail(r, n)) return 0;
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) acc = (acc << 8) | r->p[r->off + i];
    r->off += n;
    *v = acc;
    return 1;
}

/* Peek the next type byte without consuming it. */
static inline int lxmf_mp_peek(const lxmf_mp_reader *r, uint8_t *b) {
    if (!lxmf_mp_avail(r, 1)) return 0;
    *b = r->p[r->off];
    return 1;
}

/* Read an array header; on success *n = element count. */
static inline LxmfReason lxmf_mp_array(lxmf_mp_reader *r, size_t *n) {
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;
    if (b >= 0x90 && b <= 0x9f) { r->off += 1; *n = (size_t)(b & 0x0f); return LXMF_OK; }
    if (b == 0xdc) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 2, &v)) return LXMF_TRUNCATED; *n = (size_t)v; return LXMF_OK; }
    if (b == 0xdd) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 4, &v)) return LXMF_TRUNCATED; *n = (size_t)v; return LXMF_OK; }
    return LXMF_BAD_SHAPE;
}

/* Read a map header; on success *n = pair count. nil (0xc0) reads as 0 pairs. */
static inline LxmfReason lxmf_mp_map(lxmf_mp_reader *r, size_t *n) {
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;
    if (b == 0xc0)              { r->off += 1; *n = 0; return LXMF_OK; }  /* nil => no fields */
    if (b >= 0x80 && b <= 0x8f) { r->off += 1; *n = (size_t)(b & 0x0f); return LXMF_OK; }
    if (b == 0xde) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 2, &v)) return LXMF_TRUNCATED; *n = (size_t)v; return LXMF_OK; }
    if (b == 0xdf) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 4, &v)) return LXMF_TRUNCATED; *n = (size_t)v; return LXMF_OK; }
    return LXMF_BAD_SHAPE;
}

/* Read any unsigned integer (positive fixint or uint8..64) into *v. */
static inline LxmfReason lxmf_mp_uint(lxmf_mp_reader *r, uint64_t *v) {
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;
    if (b <= 0x7f)  { r->off += 1; *v = b; return LXMF_OK; }         /* positive fixint */
    if (b == 0xcc)  { r->off += 1; return lxmf_mp_be(r, 1, v) ? LXMF_OK : LXMF_TRUNCATED; }
    if (b == 0xcd)  { r->off += 1; return lxmf_mp_be(r, 2, v) ? LXMF_OK : LXMF_TRUNCATED; }
    if (b == 0xce)  { r->off += 1; return lxmf_mp_be(r, 4, v) ? LXMF_OK : LXMF_TRUNCATED; }
    if (b == 0xcf)  { r->off += 1; return lxmf_mp_be(r, 8, v) ? LXMF_OK : LXMF_TRUNCATED; }
    return LXMF_BAD_MSGPACK;
}

/* Read a numeric value (float32/float64 or any signed/unsigned int) as double. */
static inline LxmfReason lxmf_mp_double(lxmf_mp_reader *r, double *out) {
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;
    if (b == 0xcb) {                                   /* float64 */
        r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 8, &v)) return LXMF_TRUNCATED;
        double d; memcpy(&d, &v, 8); *out = d; return LXMF_OK;
    }
    if (b == 0xca) {                                   /* float32 */
        r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 4, &v)) return LXMF_TRUNCATED;
        uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); *out = (double)f; return LXMF_OK;
    }
    if (b <= 0x7f)              { r->off += 1; *out = (double)b; return LXMF_OK; }        /* +fixint */
    if (b >= 0xe0)              { r->off += 1; *out = (double)(int8_t)b; return LXMF_OK; }/* -fixint */
    if (b == 0xcc || b == 0xcd || b == 0xce || b == 0xcf) {
        uint64_t v; LxmfReason rr = lxmf_mp_uint(r, &v); if (rr != LXMF_OK) return rr;
        *out = (double)v; return LXMF_OK;
    }
    if (b == 0xd0) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 1, &v)) return LXMF_TRUNCATED; *out = (double)(int8_t)v;  return LXMF_OK; }
    if (b == 0xd1) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 2, &v)) return LXMF_TRUNCATED; *out = (double)(int16_t)v; return LXMF_OK; }
    if (b == 0xd2) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 4, &v)) return LXMF_TRUNCATED; *out = (double)(int32_t)v; return LXMF_OK; }
    if (b == 0xd3) { r->off += 1; uint64_t v; if (!lxmf_mp_be(r, 8, &v)) return LXMF_TRUNCATED; *out = (double)(int64_t)v; return LXMF_OK; }
    return LXMF_BAD_MSGPACK;
}

/*
 * Read the length of a str/bin value and point *data at its bytes, consuming
 * them. Accepts fixstr, str8/16/32 and bin8/16/32. nil (0xc0) yields len 0.
 * The byte range is bounds-checked against the buffer (TRUNCATED if it runs off
 * the end); the cap check is left to the caller (so it can pick TOO_LONG).
 */
static inline LxmfReason lxmf_mp_raw(lxmf_mp_reader *r, const uint8_t **data, size_t *len) {
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;
    uint64_t n = 0;
    if (b == 0xc0)                    { r->off += 1; *data = r->p + r->off; *len = 0; return LXMF_OK; }
    else if (b >= 0xa0 && b <= 0xbf)  { r->off += 1; n = (uint64_t)(b & 0x1f); }         /* fixstr */
    else if (b == 0xd9 || b == 0xc4)  { r->off += 1; if (!lxmf_mp_be(r, 1, &n)) return LXMF_TRUNCATED; } /* str8/bin8 */
    else if (b == 0xda || b == 0xc5)  { r->off += 1; if (!lxmf_mp_be(r, 2, &n)) return LXMF_TRUNCATED; } /* str16/bin16 */
    else if (b == 0xdb || b == 0xc6)  { r->off += 1; if (!lxmf_mp_be(r, 4, &n)) return LXMF_TRUNCATED; } /* str32/bin32 */
    else return LXMF_BAD_MSGPACK;
    if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED;
    *data = r->p + r->off;
    *len  = (size_t)n;
    r->off += (size_t)n;
    return LXMF_OK;
}

/* Copy a str/bin value into a fixed byte buffer; NUL-terminate when nul_term. */
static inline LxmfReason lxmf_mp_copy(lxmf_mp_reader *r, void *dst, size_t cap,
                                      size_t *out_len, int nul_term) {
    const uint8_t *d; size_t n;
    LxmfReason rr = lxmf_mp_raw(r, &d, &n);
    if (rr != LXMF_OK) return rr;
    if (n > cap) return LXMF_TOO_LONG;
    if (n) memcpy(dst, d, n);
    if (nul_term) ((char *)dst)[n] = '\0';
    *out_len = n;
    return LXMF_OK;
}

/* Generic bounded skip of exactly one msgpack value (unknown/unused fields). */
static inline LxmfReason lxmf_mp_skip(lxmf_mp_reader *r, int depth) {
    if (depth > LXMF_MAX_DEPTH) return LXMF_BAD_MSGPACK;
    uint8_t b;
    if (!lxmf_mp_peek(r, &b)) return LXMF_TRUNCATED;

    if (b <= 0x7f || b >= 0xe0) { r->off += 1; return LXMF_OK; }        /* +/- fixint */
    if (b >= 0xa0 && b <= 0xbf) {                                       /* fixstr */
        size_t n = (size_t)(b & 0x1f); r->off += 1;
        if (!lxmf_mp_avail(r, n)) return LXMF_TRUNCATED; r->off += n; return LXMF_OK;
    }
    if (b >= 0x90 && b <= 0x9f) {                                       /* fixarray */
        size_t n = (size_t)(b & 0x0f); r->off += 1;
        for (size_t i = 0; i < n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; }
        return LXMF_OK;
    }
    if (b >= 0x80 && b <= 0x8f) {                                       /* fixmap */
        size_t n = (size_t)(b & 0x0f); r->off += 1;
        for (size_t i = 0; i < 2 * n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; }
        return LXMF_OK;
    }
    r->off += 1;   /* consume the type byte for the multi-byte encodings below */
    switch (b) {
        case 0xc0: case 0xc2: case 0xc3: return LXMF_OK;                /* nil/false/true */
        case 0xcc: case 0xd0: return lxmf_mp_avail(r, 1) ? (r->off += 1, LXMF_OK) : LXMF_TRUNCATED;
        case 0xcd: case 0xd1: return lxmf_mp_avail(r, 2) ? (r->off += 2, LXMF_OK) : LXMF_TRUNCATED;
        case 0xca: case 0xce: case 0xd2: return lxmf_mp_avail(r, 4) ? (r->off += 4, LXMF_OK) : LXMF_TRUNCATED;
        case 0xcb: case 0xcf: case 0xd3: return lxmf_mp_avail(r, 8) ? (r->off += 8, LXMF_OK) : LXMF_TRUNCATED;
        case 0xd4: return lxmf_mp_avail(r, 1 + 1) ? (r->off += 2,  LXMF_OK) : LXMF_TRUNCATED;   /* fixext1 */
        case 0xd5: return lxmf_mp_avail(r, 1 + 2) ? (r->off += 3,  LXMF_OK) : LXMF_TRUNCATED;   /* fixext2 */
        case 0xd6: return lxmf_mp_avail(r, 1 + 4) ? (r->off += 5,  LXMF_OK) : LXMF_TRUNCATED;   /* fixext4 */
        case 0xd7: return lxmf_mp_avail(r, 1 + 8) ? (r->off += 9,  LXMF_OK) : LXMF_TRUNCATED;   /* fixext8 */
        case 0xd8: return lxmf_mp_avail(r, 1 + 16) ? (r->off += 17, LXMF_OK) : LXMF_TRUNCATED;  /* fixext16 */
        case 0xc4: case 0xd9: { uint64_t n; if (!lxmf_mp_be(r, 1, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; }
        case 0xc5: case 0xda: { uint64_t n; if (!lxmf_mp_be(r, 2, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; }
        case 0xc6: case 0xdb: { uint64_t n; if (!lxmf_mp_be(r, 4, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; }
        /* ext8/16/32: 1 ext-type byte then n payload bytes. NEVER form (size_t)n+1
         * — with an attacker n of 0xFFFFFFFF that wraps to 0 on a 32-bit size_t,
         * turning a malformed ext into a zero-length skip instead of TRUNCATED.
         * Consume the type byte and the n payload separately, each checked against
         * the remaining bytes via the non-wrapping lxmf_mp_avail. */
        case 0xc7: { uint64_t n; if (!lxmf_mp_be(r, 1, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, 1)) return LXMF_TRUNCATED; r->off += 1; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; } /* ext8 */
        case 0xc8: { uint64_t n; if (!lxmf_mp_be(r, 2, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, 1)) return LXMF_TRUNCATED; r->off += 1; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; } /* ext16 */
        case 0xc9: { uint64_t n; if (!lxmf_mp_be(r, 4, &n)) return LXMF_TRUNCATED; if (!lxmf_mp_avail(r, 1)) return LXMF_TRUNCATED; r->off += 1; if (!lxmf_mp_avail(r, (size_t)n)) return LXMF_TRUNCATED; r->off += (size_t)n; return LXMF_OK; } /* ext32 */
        case 0xdc: { uint64_t n; if (!lxmf_mp_be(r, 2, &n)) return LXMF_TRUNCATED; for (uint64_t i = 0; i < n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; } return LXMF_OK; } /* array16 */
        case 0xdd: { uint64_t n; if (!lxmf_mp_be(r, 4, &n)) return LXMF_TRUNCATED; for (uint64_t i = 0; i < n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; } return LXMF_OK; } /* array32 */
        case 0xde: { uint64_t n; if (!lxmf_mp_be(r, 2, &n)) return LXMF_TRUNCATED; for (uint64_t i = 0; i < 2 * n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; } return LXMF_OK; } /* map16 */
        case 0xdf: { uint64_t n; if (!lxmf_mp_be(r, 4, &n)) return LXMF_TRUNCATED; for (uint64_t i = 0; i < 2 * n; i++) { LxmfReason rr = lxmf_mp_skip(r, depth + 1); if (rr != LXMF_OK) return rr; } return LXMF_OK; } /* map32 */
        default: return LXMF_BAD_MSGPACK;
    }
}

/* ======================================================================== *
 *  parse (inbound)
 * ======================================================================== */

/* Parse the FIELD_CUSTOM_META (0xFD) map: {"sev":uint,"src":str,"key":str,"ttl":uint}. */
static inline LxmfReason lxmf_parse_meta(lxmf_mp_reader *r, LxmfMsg *out) {
    size_t n;
    LxmfReason rr = lxmf_mp_map(r, &n);
    if (rr != LXMF_OK) return rr;
    out->has_meta = 1;
    for (size_t i = 0; i < n; i++) {
        char k[LXMF_META_STR_CAP + 1]; size_t kl;
        rr = lxmf_mp_copy(r, k, LXMF_META_STR_CAP, &kl, 1);
        if (rr == LXMF_TOO_LONG) {                     /* over-long key: skip pair */
            k[0] = '\0';
            rr = lxmf_mp_skip(r, 0); if (rr != LXMF_OK) return rr;
            continue;
        }
        if (rr != LXMF_OK) return rr;
        if (kl == 3 && memcmp(k, "sev", 3) == 0) {
            uint64_t v; rr = lxmf_mp_uint(r, &v); if (rr != LXMF_OK) return rr;
            out->meta_has_sev = 1; out->meta_sev = (uint32_t)v;
        } else if (kl == 3 && memcmp(k, "ttl", 3) == 0) {
            uint64_t v; rr = lxmf_mp_uint(r, &v); if (rr != LXMF_OK) return rr;
            out->meta_has_ttl = 1; out->meta_ttl = (uint32_t)v;
        } else if (kl == 3 && memcmp(k, "src", 3) == 0) {
            rr = lxmf_mp_copy(r, out->meta_src, LXMF_META_STR_CAP, &out->meta_src_len, 1);
            if (rr != LXMF_OK) return rr;
        } else if (kl == 3 && memcmp(k, "key", 3) == 0) {
            rr = lxmf_mp_copy(r, out->meta_key, LXMF_META_STR_CAP, &out->meta_key_len, 1);
            if (rr != LXMF_OK) return rr;
        } else {
            rr = lxmf_mp_skip(r, 0); if (rr != LXMF_OK) return rr;   /* unknown meta key */
        }
    }
    return LXMF_OK;
}

/*
 * Parse an inbound LXMF message. `buf`/`len` are UNTRUSTED, hostile bytes. On
 * LXMF_OK, `out` holds dest/source hashes, timestamp, title, content and any of
 * the fields we understand (unknown fields are silently ignored). On any nonzero
 * reason, `out` is left zeroed-plus-whatever-was-parsed and no read ever went
 * out of bounds. `out` is fully zeroed on entry.
 */
static inline LxmfReason lxmf_parse(const uint8_t *buf, size_t len, LxmfMsg *out) {
    if (!out) return LXMF_BAD_SHAPE;
    memset(out, 0, sizeof(*out));
    if (!buf) return LXMF_TRUNCATED;
    if (len < LXMF_HEADER_LEN) return LXMF_TRUNCATED;          /* 96-byte header  */

    memcpy(out->dest_hash,   buf,                    LXMF_HASH_LEN);
    memcpy(out->source_hash, buf + LXMF_HASH_LEN,    LXMF_HASH_LEN);
    /* signature at buf[32:96] is deliberately not consumed here (crypto-free). */

    lxmf_mp_reader r = { buf + LXMF_HEADER_LEN, len - LXMF_HEADER_LEN, 0 };

    size_t nelem;
    LxmfReason rr = lxmf_mp_array(&r, &nelem);
    if (rr != LXMF_OK) return rr == LXMF_BAD_SHAPE ? LXMF_BAD_SHAPE : rr;
    if (nelem != 4 && nelem != 5) return LXMF_BAD_SHAPE;       /* 4, or 5 w/ stamp */

    /* [0] timestamp */
    rr = lxmf_mp_double(&r, &out->timestamp); if (rr != LXMF_OK) return rr;
    /* [1] title */
    rr = lxmf_mp_copy(&r, out->title, LXMF_TITLE_CAP, &out->title_len, 1); if (rr != LXMF_OK) return rr;
    /* [2] content */
    rr = lxmf_mp_copy(&r, out->content, LXMF_CONTENT_CAP, &out->content_len, 1); if (rr != LXMF_OK) return rr;

    /* [3] fields (map or nil). Unknown keys/values are skipped, not fatal. */
    size_t nf;
    rr = lxmf_mp_map(&r, &nf); if (rr != LXMF_OK) return rr;
    for (size_t i = 0; i < nf; i++) {
        uint64_t key;
        rr = lxmf_mp_uint(&r, &key);
        if (rr != LXMF_OK) {
            /* non-integer key: not standard LXMF, but tolerate — skip k and v. */
            rr = lxmf_mp_skip(&r, 0); if (rr != LXMF_OK) return rr;
            rr = lxmf_mp_skip(&r, 0); if (rr != LXMF_OK) return rr;
            continue;
        }
        switch (key) {
            case LXMF_FIELD_THREAD:
                rr = lxmf_mp_copy(&r, out->thread, LXMF_THREAD_CAP, &out->thread_len, 0);
                if (rr != LXMF_OK) return rr;
                out->has_thread = 1;
                break;
            case LXMF_FIELD_RENDERER: {
                uint64_t v; rr = lxmf_mp_uint(&r, &v); if (rr != LXMF_OK) return rr;
                out->has_renderer = 1; out->renderer = (uint8_t)v;
                break;
            }
            case LXMF_FIELD_CUSTOM_TYPE:
                rr = lxmf_mp_copy(&r, out->type, LXMF_TYPE_CAP, &out->type_len, 1);
                if (rr != LXMF_OK) return rr;
                out->has_type = 1;
                break;
            case LXMF_FIELD_CUSTOM_DATA:
                rr = lxmf_mp_copy(&r, out->data, LXMF_DATA_CAP, &out->data_len, 0);
                if (rr != LXMF_OK) return rr;
                out->has_data = 1;
                break;
            case LXMF_FIELD_CUSTOM_META:
                rr = lxmf_parse_meta(&r, out); if (rr != LXMF_OK) return rr;
                break;
            default:
                rr = lxmf_mp_skip(&r, 0); if (rr != LXMF_OK) return rr;   /* unknown field */
                break;
        }
    }
    /* element [4] stamp (if nelem==5) is intentionally left unread: we do not
     * need it and the payload's bytes are already fully bounded above. */
    return LXMF_OK;
}

/* ======================================================================== *
 *  encode (reply) — crypto-free; see the sign-injection contract at the top
 * ======================================================================== */

typedef struct { uint8_t *p; size_t cap; size_t off; int overflow; } lxmf_mp_writer;

static inline void lxmf_w_byte(lxmf_mp_writer *w, uint8_t b) {
    if (w->off < w->cap) w->p[w->off] = b; else w->overflow = 1;
    w->off++;
}
static inline void lxmf_w_bytes(lxmf_mp_writer *w, const void *src, size_t n) {
    if (w->off + n <= w->cap) memcpy(w->p + w->off, src, n); else w->overflow = 1;
    w->off += n;
}
static inline void lxmf_w_be(lxmf_mp_writer *w, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++) lxmf_w_byte(w, (uint8_t)(v >> (8 * (n - 1 - i))));
}
/* msgpack: emit a bin (8/16/32) header + bytes. */
static inline void lxmf_w_bin(lxmf_mp_writer *w, const void *src, size_t n) {
    if (n <= 0xff)        { lxmf_w_byte(w, 0xc4); lxmf_w_be(w, n, 1); }
    else if (n <= 0xffff) { lxmf_w_byte(w, 0xc5); lxmf_w_be(w, n, 2); }
    else                  { lxmf_w_byte(w, 0xc6); lxmf_w_be(w, n, 4); }
    lxmf_w_bytes(w, src, n);
}
/* msgpack: emit a str (fixstr/8/16) header + bytes. */
static inline void lxmf_w_str(lxmf_mp_writer *w, const char *s, size_t n) {
    if (n <= 0x1f)        { lxmf_w_byte(w, (uint8_t)(0xa0 | n)); }
    else if (n <= 0xff)   { lxmf_w_byte(w, 0xd9); lxmf_w_be(w, n, 1); }
    else                  { lxmf_w_byte(w, 0xda); lxmf_w_be(w, n, 2); }
    lxmf_w_bytes(w, s, n);
}
/* msgpack: emit an unsigned int in its smallest fixint/uint8..32 form. */
static inline void lxmf_w_uint(lxmf_mp_writer *w, uint64_t v) {
    if (v <= 0x7f)          { lxmf_w_byte(w, (uint8_t)v); }
    else if (v <= 0xff)     { lxmf_w_byte(w, 0xcc); lxmf_w_be(w, v, 1); }
    else if (v <= 0xffff)   { lxmf_w_byte(w, 0xcd); lxmf_w_be(w, v, 2); }
    else if (v <= 0xffffffffu){ lxmf_w_byte(w, 0xce); lxmf_w_be(w, v, 4); }
    else                    { lxmf_w_byte(w, 0xcf); lxmf_w_be(w, v, 8); }
}
/* msgpack: emit a map key (a field id) as fixint or uint8. */
static inline void lxmf_w_key(lxmf_mp_writer *w, uint8_t k) { lxmf_w_uint(w, k); }

/* Count how many top-level fields the message wants to emit. */
static inline size_t lxmf_field_count(const LxmfMsg *m) {
    return (size_t)(!!m->has_thread) + (!!m->has_renderer) +
           (!!m->has_type) + (!!m->has_data) + (!!m->has_meta);
}

/* Write the msgpack payload array [timestamp, title, content, fields] into w. */
static inline void lxmf_write_payload(lxmf_mp_writer *w, const LxmfMsg *m) {
    lxmf_w_byte(w, 0x94);                                   /* fixarray(4) */

    /* timestamp: always float64 */
    lxmf_w_byte(w, 0xcb);
    uint64_t tb; double t = m->timestamp; memcpy(&tb, &t, 8);
    lxmf_w_be(w, tb, 8);

    /* title / content: bytes, as the reference stores them */
    lxmf_w_bin(w, m->title, m->title_len);
    lxmf_w_bin(w, m->content, m->content_len);

    /* fields map */
    size_t nf = lxmf_field_count(m);
    if (nf <= 0x0f)      lxmf_w_byte(w, (uint8_t)(0x80 | nf));
    else               { lxmf_w_byte(w, 0xde); lxmf_w_be(w, nf, 2); }

    if (m->has_thread)   { lxmf_w_key(w, LXMF_FIELD_THREAD);   lxmf_w_bin(w, m->thread, m->thread_len); }
    if (m->has_renderer) { lxmf_w_key(w, LXMF_FIELD_RENDERER); lxmf_w_uint(w, m->renderer); }
    if (m->has_type)     { lxmf_w_key(w, LXMF_FIELD_CUSTOM_TYPE); lxmf_w_str(w, m->type, m->type_len); }
    if (m->has_data)     { lxmf_w_key(w, LXMF_FIELD_CUSTOM_DATA); lxmf_w_bin(w, m->data, m->data_len); }
    if (m->has_meta) {
        lxmf_w_key(w, LXMF_FIELD_CUSTOM_META);
        size_t mn = (size_t)(!!m->meta_has_sev) + (!!m->meta_has_ttl) +
                    (m->meta_src_len ? 1 : 0) + (m->meta_key_len ? 1 : 0);
        lxmf_w_byte(w, (uint8_t)(0x80 | (mn & 0x0f)));
        if (m->meta_has_sev)   { lxmf_w_str(w, "sev", 3); lxmf_w_uint(w, m->meta_sev); }
        if (m->meta_src_len)   { lxmf_w_str(w, "src", 3); lxmf_w_str(w, m->meta_src, m->meta_src_len); }
        if (m->meta_key_len)   { lxmf_w_str(w, "key", 3); lxmf_w_str(w, m->meta_key, m->meta_key_len); }
        if (m->meta_has_ttl)   { lxmf_w_str(w, "ttl", 3); lxmf_w_uint(w, m->meta_ttl); }
    }
}

/*
 * Emit the signing input hashed_part = dest_hash(16) + source_hash(16) +
 * msgpack(payload) into `out` (cap `out_cap`), *out_len set to the true length.
 * The caller hashes this (RNS full_hash / SHA-256 -> 32 bytes), appends that hash
 * to form signed_part, and Ed25519-signs signed_part to get sig[64]. Returns
 * LXMF_TOO_LONG if it did not fit (nothing partial is trusted; *out_len is the
 * length it WOULD have needed, so the caller can size a bigger buffer).
 */
static inline LxmfReason lxmf_encode_sign_input(const LxmfMsg *m,
                                                uint8_t *out, size_t out_cap,
                                                size_t *out_len) {
    if (!m || !out) return LXMF_BAD_SHAPE;
    lxmf_mp_writer w = { out, out_cap, 0, 0 };
    lxmf_w_bytes(&w, m->dest_hash, LXMF_HASH_LEN);
    lxmf_w_bytes(&w, m->source_hash, LXMF_HASH_LEN);
    lxmf_write_payload(&w, m);
    if (out_len) *out_len = w.off;
    return w.overflow ? LXMF_TOO_LONG : LXMF_OK;
}

/*
 * Emit the final wire message packed = dest(16) + source(16) + sig(64) +
 * msgpack(payload). `sig` is the injected Ed25519 signature the caller produced
 * over the signed_part from lxmf_encode_sign_input(); this function never hashes
 * or signs. Returns LXMF_TOO_LONG if it did not fit (with *out_len = needed).
 */
static inline LxmfReason lxmf_encode_packed(const LxmfMsg *m,
                                            const uint8_t sig[LXMF_SIG_LEN],
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len) {
    if (!m || !out || !sig) return LXMF_BAD_SHAPE;
    lxmf_mp_writer w = { out, out_cap, 0, 0 };
    lxmf_w_bytes(&w, m->dest_hash, LXMF_HASH_LEN);
    lxmf_w_bytes(&w, m->source_hash, LXMF_HASH_LEN);
    lxmf_w_bytes(&w, sig, LXMF_SIG_LEN);
    lxmf_write_payload(&w, m);
    if (out_len) *out_len = w.off;
    return w.overflow ? LXMF_TOO_LONG : LXMF_OK;
}
