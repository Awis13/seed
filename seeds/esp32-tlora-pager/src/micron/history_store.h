#pragma once

/*
 * micron/history_store.h — the PURE core of the unified persistent history
 * archive (ticket TLORA-HISTORY / C1). This is the infrastructure tier that
 * sits UNDER the micron page store (micron_store.h): where that keeps a small
 * hot set of live pages in RAM, this defines the append-only ARCHIVE those
 * pages (and, from C3, notify cards) are flushed to on microSD.
 *
 * C1 delivers only the codec + index here and the off-loop write path in the
 * hw glue (skills/history.cpp). NO producer is wired yet — C2 attaches the
 * micron page store's eviction seam, C3 brings in notify cards.
 *
 * PURE / HOST-TESTABLE (the same discipline as micron_store.h / micron_layout.h)
 * ----------------------------------------------------------------------------
 * Everything in this file is plain integer/byte logic: no Arduino, no `SD`,
 * no `File`, no `millis()`. The monotonic sequence and the timestamp are
 * INJECTED by the caller (the hw glue reads the real clock / a counter at the
 * call site and passes them in); byte I/O is abstracted behind a `history_sink`
 * write callback for encode and a plain byte buffer for decode, so the whole
 * codec + reassembly + index is driven end to end by tools/test_history_store.
 *
 * KEY IDENTITY MODEL (forward-looking to C2/C3)
 * ---------------------------------------------
 * A record is identified by (ns, key), exactly the micron_store identity:
 *   - ns   : an opaque uint8_t namespace / delivery account (MICRON_NS_SYSTEM,
 *            MICRON_NS_FOREIGN, ...); never parsed from a record body.
 *   - key  : 1..MICRON_STORE_KEY_MAX bytes of strict printable ASCII
 *            (0x21..0x7E). We reuse micron_store_key_valid() verbatim so the
 *            archive and the live store share one validation discipline — a key
 *            a slot would reject can never enter the archive either.
 * plus a monotonic `seq` and an injected `stamp_ms`, and a bounded payload blob.
 *
 * APPEND-ONLY, NEWEST-WINS
 * ------------------------
 * The archive is append-only: an "upsert" of (ns,key) is a NEW record appended
 * with a higher seq that SUPERSEDES the older one on read. Nothing is rewritten
 * in place — that is what preserves the flash/SD wear behaviour of a log. The
 * index (history_index) resolves reads: for each (ns,key) it remembers the
 * offset of the newest record seen; a later record with seq > the stored seq
 * wins (equal seq falls back to append order, i.e. the later-observed record).
 *
 * The monotonic seq is the persistent identity of "newest", so it must not reset
 * across a reboot: g_hist_seq in the hw glue starts at 0 each boot, so on mount
 * the glue re-seeds it to the MAXIMUM stored seq via one bounded chunked scan
 * (history_seq_scan, below) before the first append. Without that reseed a fresh
 * post-reboot append (seq=1) would be shadowed by a higher-seq pre-reboot record
 * for the same (ns,key); with it, the next append is always max+1 and outranks
 * everything older. An empty/absent archive leaves the counter at 0.
 *
 * ON-DISK RECORD FRAME (all multi-byte fields little-endian, fixed, explicit,
 * so a record written on the device decodes byte-for-byte on the host test):
 *
 *   off  size  field
 *   0    1     magic0  = 'H'      (0x48)  resync marker
 *   1    1     magic1  = '1'      (0x31)
 *   2    1     ver     = 1
 *   3    1     ns
 *   4    4     seq        (LE)
 *   8    4     stamp_ms   (LE)
 *   12   1     keylen     (1..MICRON_STORE_KEY_MAX)
 *   13   2     paylen     (LE, 0..HISTORY_PAYLOAD_CAP)
 *   15   4     crc32      (LE) over bytes [2..14] + key + payload
 *   19   klen  key bytes  (NOT NUL-terminated on disk)
 *   ..   plen  payload
 *
 * The two magic bytes and the crc field itself are excluded from the crc so a
 * reader can RESYNC after a garbage region by scanning forward one byte at a
 * time for the next valid magic+crc, without a corrupt length ever being
 * trusted. That is the hostile-input property the host test pins.
 *
 * PAYLOAD CAP
 * -----------
 * HISTORY_PAYLOAD_CAP is 512 B, matched to MICRON_STORE_SRC_CAP: the largest
 * current producer record is a micron page source (512 B), and a notify card's
 * persisted fields fit the same budget. 512 keeps the off-loop write queue
 * (FreeRTOS by-value copy, depth HISTORY_WRITE_QUEUE_DEPTH) at ~9 KB of plain
 * DRAM — the same scale as the agents viewport — and bounds one on-disk record
 * to HISTORY_REC_MAX (564 B), comfortably under the 4096 B chunked-read bus
 * budget so a single record is always one bus burst. Bump it here, in one
 * place, when a real producer needs more.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "micron_store.h"   /* MICRON_STORE_KEY_MAX/_CAP + micron_store_key_valid */

/* --- frame constants ------------------------------------------------------- */

#define HISTORY_REC_VER      1
#define HISTORY_MAGIC0       0x48u   /* 'H' */
#define HISTORY_MAGIC1       0x31u   /* '1' */

/* Per-record payload cap (see header note). Matched to MICRON_STORE_SRC_CAP. */
#define HISTORY_PAYLOAD_CAP  512

/* Fixed header: magic(2) ver(1) ns(1) seq(4) stamp(4) keylen(1) paylen(2) crc(4). */
#define HISTORY_HDR_SIZE     19

/* The largest a single framed record can be: header + longest key + full
 * payload. The streaming reader sizes its reassembly buffer to exactly this, so
 * any one record is guaranteed to fit whole. */
#define HISTORY_REC_MAX      (HISTORY_HDR_SIZE + MICRON_STORE_KEY_MAX + HISTORY_PAYLOAD_CAP)

/* --- one decoded record ---------------------------------------------------- */

typedef struct {
    uint8_t  ns;
    uint32_t seq;                        /* monotonic, injected by the producer */
    uint32_t stamp_ms;                   /* injected clock at append time */
    uint16_t len;                        /* payload bytes in use (<= CAP) */
    char     key[MICRON_STORE_KEY_CAP];  /* NUL-terminated, validated */
    uint8_t  payload[HISTORY_PAYLOAD_CAP];
} history_record;

/* --- little-endian scalar helpers ------------------------------------------ */

static inline void history_put_u16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static inline void history_put_u32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline uint16_t history_get_u16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t history_get_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* --- CRC-32 (IEEE 802.3, poly 0xEDB88320) ---------------------------------- */

/* Running update; init with 0xFFFFFFFF, finalize by XOR 0xFFFFFFFF. Bitwise so
 * there is no 1 KB table in flash — records are small and this runs on the
 * write task, off the loop, so the per-byte cost is invisible. */
static inline uint32_t history_crc32_upd(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc;
}

/* --- encode: stream a record to a sink ------------------------------------- */

/* Byte sink: write() returns >= 0 on success, < 0 on failure. The device passes
 * a sink that appends to an open File; the host test appends to a buffer. This
 * is what keeps the codec Arduino-free. */
typedef struct {
    int (*write)(void *ctx, const uint8_t *p, size_t n);
    void *ctx;
} history_sink;

/*
 * Encode `rec` to `sink`. Returns 1 on success, 0 on a rejected record (bad key,
 * over-cap payload, or a sink write error). The header goes onto a 19-byte stack
 * buffer; the key and payload stream straight from the record — no full-record
 * buffer is ever built, keeping the write task's stack lean.
 */
static inline int history_encode(const history_sink *sink,
                                 const history_record *rec) {
    if (!sink || !sink->write || !rec) return 0;
    if (!micron_store_key_valid(rec->key)) return 0;
    if (rec->len > HISTORY_PAYLOAD_CAP) return 0;
    size_t keylen = strlen(rec->key);            /* <= MICRON_STORE_KEY_MAX */

    uint8_t hdr[HISTORY_HDR_SIZE];
    hdr[0] = (uint8_t)HISTORY_MAGIC0;
    hdr[1] = (uint8_t)HISTORY_MAGIC1;
    hdr[2] = (uint8_t)HISTORY_REC_VER;
    hdr[3] = rec->ns;
    history_put_u32(hdr + 4, rec->seq);
    history_put_u32(hdr + 8, rec->stamp_ms);
    hdr[12] = (uint8_t)keylen;
    history_put_u16(hdr + 13, rec->len);

    uint32_t c = 0xFFFFFFFFu;
    c = history_crc32_upd(c, hdr + 2, 13);                          /* ver..paylen */
    c = history_crc32_upd(c, (const uint8_t *)rec->key, keylen);
    c = history_crc32_upd(c, rec->payload, rec->len);
    c ^= 0xFFFFFFFFu;
    history_put_u32(hdr + 15, c);

    if (sink->write(sink->ctx, hdr, HISTORY_HDR_SIZE) < 0) return 0;
    if (keylen && sink->write(sink->ctx, (const uint8_t *)rec->key, keylen) < 0)
        return 0;
    if (rec->len && sink->write(sink->ctx, rec->payload, rec->len) < 0) return 0;
    return 1;
}

/* --- decode: parse one record from a byte buffer --------------------------- */

typedef enum {
    HISTORY_DECODE_OK = 0,
    HISTORY_DECODE_NEED_MORE,   /* fewer than one whole record available yet */
    HISTORY_DECODE_BAD_MAGIC,   /* buf[0..1] is not a record start */
    HISTORY_DECODE_BAD_HEADER,  /* magic ok, but ver/keylen/paylen out of range */
    HISTORY_DECODE_BAD_CRC,     /* header or payload corrupted */
    HISTORY_DECODE_BAD_KEY,     /* key bytes failed printable-ASCII validation */
} history_decode_status;

/*
 * Decode the record at the FRONT of [buf, buf+avail). On OK, *consumed is the
 * whole record length and *out (if non-NULL) is filled. NEED_MORE means the
 * buffer holds a valid-so-far prefix but not the whole record (a truncated tail
 * — feed more bytes). Every other status means the front is not a valid record;
 * the caller resyncs by dropping one byte and retrying (see history_reader).
 * A hostile length can never be trusted: paylen > CAP and keylen out of range
 * are rejected as BAD_HEADER before any CRC or copy, and the CRC gates the
 * bytes before the key is even validated.
 */
static inline history_decode_status history_decode(const uint8_t *buf, size_t avail,
                                                   history_record *out,
                                                   size_t *consumed) {
    if (consumed) *consumed = 0;
    if (avail < HISTORY_HDR_SIZE) return HISTORY_DECODE_NEED_MORE;
    if (buf[0] != HISTORY_MAGIC0 || buf[1] != HISTORY_MAGIC1)
        return HISTORY_DECODE_BAD_MAGIC;
    if (buf[2] != HISTORY_REC_VER) return HISTORY_DECODE_BAD_HEADER;

    uint8_t  ns     = buf[3];
    uint32_t seq    = history_get_u32(buf + 4);
    uint32_t stamp  = history_get_u32(buf + 8);
    uint8_t  keylen = buf[12];
    uint16_t paylen = history_get_u16(buf + 13);

    if (keylen < 1 || keylen > MICRON_STORE_KEY_MAX) return HISTORY_DECODE_BAD_HEADER;
    if (paylen > HISTORY_PAYLOAD_CAP) return HISTORY_DECODE_BAD_HEADER;

    size_t total = (size_t)HISTORY_HDR_SIZE + keylen + paylen;
    if (avail < total) return HISTORY_DECODE_NEED_MORE;

    uint32_t stored = history_get_u32(buf + 15);
    uint32_t c = 0xFFFFFFFFu;
    c = history_crc32_upd(c, buf + 2, 13);
    c = history_crc32_upd(c, buf + HISTORY_HDR_SIZE, keylen);
    c = history_crc32_upd(c, buf + HISTORY_HDR_SIZE + keylen, paylen);
    c ^= 0xFFFFFFFFu;
    if (c != stored) return HISTORY_DECODE_BAD_CRC;

    /* Key validation reuses the live store's exact discipline. */
    char key[MICRON_STORE_KEY_CAP];
    memcpy(key, buf + HISTORY_HDR_SIZE, keylen);
    key[keylen] = '\0';
    if (!micron_store_key_valid(key)) return HISTORY_DECODE_BAD_KEY;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->ns = ns;
        out->seq = seq;
        out->stamp_ms = stamp;
        out->len = paylen;
        memcpy(out->key, key, (size_t)keylen + 1);
        if (paylen) memcpy(out->payload, buf + HISTORY_HDR_SIZE + keylen, paylen);
    }
    if (consumed) *consumed = total;
    return HISTORY_DECODE_OK;
}

/* --- index / manifest: newest (ns,key) wins -------------------------------- */

/* Distinct (ns,key) identities held in the RAM read index. 64 covers the live
 * producers (8 micron page slots + notify cards) with headroom; each entry is
 * ~48 B, so the table is ~3 KB of plain DRAM. The archive on disk is unbounded;
 * this is only the resolution index for the newest record per identity.
 *
 * FINDING 1 — SHARED WINDOW ACROSS ALL NAMESPACES (accept + document, tunable).
 * This one window is shared across EVERY namespace (SYSTEM / FOREIGN / NOTIFY):
 * it is a single MRU-by-seq set of the newest HISTORY_INDEX_MAX distinct
 * identities IN TOTAL, not a per-namespace budget. So boot-restore
 * (history_restore_at) and wheel-navigation (history_nav_*) can only ever see the
 * newest HISTORY_INDEX_MAX identities summed over all namespaces — a burst of
 * pages in one namespace can push another namespace's older identities out of the
 * window. Records beyond the window are NOT lost: they stay on the SD archive and
 * remain fetchable by key via history_read_newest() (a full scan); they are just
 * not auto-restored at boot nor reachable by wheel detent. This is a deliberate,
 * tunable trade governed by exactly this one constant. FOLLOW-UP (only if page
 * churn ever starves notify restore): give each namespace its own budget instead
 * of one shared table — a small, local change to this index, not a redesign. */
#define HISTORY_INDEX_MAX 64

typedef struct {
    uint8_t  used;
    uint8_t  ns;
    uint32_t seq;                        /* newest seq seen for (ns,key) */
    uint32_t offset;                     /* file offset of that newest record */
    char     key[MICRON_STORE_KEY_CAP];
} history_index_entry;

typedef struct {
    history_index_entry e[HISTORY_INDEX_MAX];
    int count;                           /* live entries */
} history_index;

static inline void history_index_init(history_index *ix) { memset(ix, 0, sizeof(*ix)); }

static inline int history_index__find(const history_index *ix, uint8_t ns,
                                      const char *key) {
    for (int i = 0; i < HISTORY_INDEX_MAX; i++) {
        if (!ix->e[i].used || ix->e[i].ns != ns) continue;
        if (strncmp(ix->e[i].key, key, MICRON_STORE_KEY_CAP) == 0) return i;
    }
    return -1;
}

/*
 * Observe one decoded record at `offset` during a scan. NEWEST-WINS: if (ns,key)
 * is already indexed, the entry is advanced to this record only when its seq is
 * >= the stored seq (append order breaks a tie; monotonic seq means ties do not
 * occur in practice). A brand new (ns,key) takes a free entry.
 *
 * SATURATION = bounded MRU-by-seq window (see the DECISION note above). When the
 * table is full and a NEW distinct identity arrives, it evicts the smallest-seq
 * entry IFF it is strictly newer than that minimum — so the index keeps the
 * newest HISTORY_INDEX_MAX distinct identities. A record OLDER than the whole
 * window is not indexable and returns 0 (its page is beyond wheel-navigation but
 * still on the card; NOT a silent loss). Returns 1 when recorded/updated.
 * The key is assumed already validated by history_decode.
 */
static inline int history_index_observe(history_index *ix, const history_record *rec,
                                        uint32_t offset) {
    int idx = history_index__find(ix, rec->ns, rec->key);
    if (idx >= 0) {
        if (rec->seq >= ix->e[idx].seq) {
            ix->e[idx].seq = rec->seq;
            ix->e[idx].offset = offset;
        }
        return 1;
    }
    for (int i = 0; i < HISTORY_INDEX_MAX; i++) {
        if (ix->e[i].used) continue;
        ix->e[i].used = 1;
        ix->e[i].ns = rec->ns;
        ix->e[i].seq = rec->seq;
        ix->e[i].offset = offset;
        memcpy(ix->e[i].key, rec->key, MICRON_STORE_KEY_CAP);
        ix->count++;
        return 1;
    }
    /* Full: keep the newest window. Evict the smallest-seq entry iff this record
     * is strictly newer than it; otherwise this identity is older than the whole
     * window and is not indexable (returns 0). count is unchanged — one identity
     * is swapped for another. */
    int minidx = 0;
    for (int i = 1; i < HISTORY_INDEX_MAX; i++)
        if (ix->e[i].seq < ix->e[minidx].seq) minidx = i;
    if (rec->seq <= ix->e[minidx].seq) return 0;   /* older than the whole window */
    ix->e[minidx].ns = rec->ns;
    ix->e[minidx].seq = rec->seq;
    ix->e[minidx].offset = offset;
    memcpy(ix->e[minidx].key, rec->key, MICRON_STORE_KEY_CAP);
    return 1;
}

/* The newest indexed record for (ns,key), or NULL. Namespace must match, so an
 * identical key string under a different namespace is a different identity and
 * is never returned — the same isolation property the live store proves. */
static inline const history_index_entry *history_index_get(const history_index *ix,
                                                           uint8_t ns,
                                                           const char *key) {
    if (!micron_store_key_valid(key)) return NULL;
    int idx = history_index__find(ix, ns, key);
    return idx >= 0 ? &ix->e[idx] : NULL;
}

/* --- boot-restore of a producer's RAM ring from the archive (ticket C3) ------
 *
 * Rebuilding a producer's RAM set at boot (the notify card ring) walks the
 * indexed identities under one namespace, NEWEST-FIRST by seq — the same axis
 * the wheel uses, but WITHOUT the live-store dedup (history_nav_archive_at needs
 * a micron_store to skip RAM-resident pages; a fresh boot ring has none yet).
 * NAMESPACE ISOLATION holds: only entries whose ns matches are counted or
 * returned, so restoring MICRON_NS_NOTIFY can never pull a SYSTEM/FOREIGN page
 * into the notify ring. Selection is the same array-free "largest seq strictly
 * below the previous pick" walk as history_nav_archive_at; seqs are distinct
 * (one entry per identity, monotonic writer seq), so there are no ties. */

/* Distinct indexed (ns,key) under `ns`. */
static inline int history_index_ns_count(const history_index *ix, uint8_t ns) {
    if (!ix) return 0;
    int n = 0;
    for (int i = 0; i < HISTORY_INDEX_MAX; i++)
        if (ix->e[i].used && ix->e[i].ns == ns) n++;
    return n;
}

/* The `rank`-th indexed (ns,key) under `ns`, newest-first by seq, or NULL. */
static inline const history_index_entry *history_index_ns_at(const history_index *ix,
                                                             uint8_t ns, int rank) {
    if (!ix || rank < 0) return NULL;
    const history_index_entry *pick = NULL;
    int have_prev = 0;
    uint32_t prev_seq = 0;
    for (int step = 0; step <= rank; step++) {
        pick = NULL;
        for (int i = 0; i < HISTORY_INDEX_MAX; i++) {
            const history_index_entry *e = &ix->e[i];
            if (!e->used || e->ns != ns) continue;
            if (have_prev && e->seq >= prev_seq) continue;   /* already taken / newer */
            if (!pick || e->seq > pick->seq) pick = e;
        }
        if (!pick) return NULL;   /* ran out before reaching rank */
        prev_seq = pick->seq;
        have_prev = 1;
    }
    return pick;
}

/* --- index saturation policy: a bounded MRU-by-seq window (ticket C2) -------
 *
 * DECISION. history_index is a bounded window of the NEWEST HISTORY_INDEX_MAX
 * distinct (ns,key) in the archive, not a first-64 snapshot. history_index_observe
 * (below) evicts the smallest-seq entry when a strictly-newer distinct identity
 * arrives and the table is full. The mount scan replays the archive oldest->newest,
 * so after boot the window holds the most recent HISTORY_INDEX_MAX identities —
 * exactly the pages nearest the RAM hot window, which is what wheel-paging walks
 * into first.
 *
 * JUSTIFICATION vs. the alternatives. Growing the index unboundedly would put an
 * archive-sized table in DRAM (the whole point of the SD tier is that the archive
 * is unbounded and RAM is not). A first-64 snapshot would index the OLDEST pages
 * and leave the ones next to the hot set unreachable — backwards for paging.
 * The MRU window gives O(1) navigation for RAM_hot + up to 64 archived distinct
 * keys (72 pages of back-paging), far past any realistic wheel walk. A page OLDER
 * than the 64th-newest archived identity is NOT navigable from the wheel — this is
 * DOCUMENTED, not silent: history_index_observe returns 0 for such a record and
 * navigation past the window stops cleanly (history_nav_* below), never OOB. Deep
 * archive retrieval (older than the window) is a future search/export concern, not
 * a wheel-paging one; the record is still on the card and still resolvable by a
 * full history_read_newest() scan.
 */

/* --- wheel navigation across the RAM hot-set + the archive index (C2) --------
 *
 * The wheel pages one namespace as a single ordinal axis:
 *   ordinals [0 .. R-1]      the R live pages in the RAM hot-set, in the store's
 *                            own stable seq order (micron_store_ns_at) — the fast
 *                            hot window, rendered straight from RAM.
 *   ordinals [R .. R+A-1]    the A distinct (ns,key) held ONLY in the archive
 *                            index (not currently live in RAM), NEWEST-FIRST by
 *                            seq — older pages whose body is fetched from SD when
 *                            the ordinal is opened/rendered (one bounded record
 *                            read, never a full scan).
 * DEDUP: a (ns,key) that is live in RAM is NEVER also an archive ordinal — RAM
 * holds its newest content, the archive only a superseded copy. NAMESPACE
 * ISOLATION holds end to end: only entries whose ns matches are counted or
 * resolved, so a FOREIGN archived page can never surface under a SYSTEM walk,
 * exactly the property micron_store_ns_at proves for the hot set. A NULL index
 * (archive absent / nothing flushed) reduces the axis to the RAM hot-set alone.
 */

/* Distinct archived (ns,key) under `ns` that are NOT currently live in `st`. */
static inline int history_nav_archive_count(const history_index *ix,
                                            const micron_store *st, uint8_t ns) {
    if (!ix || !st) return 0;
    int n = 0;
    for (int i = 0; i < HISTORY_INDEX_MAX; i++) {
        const history_index_entry *e = &ix->e[i];
        if (!e->used || e->ns != ns) continue;
        if (micron_store__find(st, ns, e->key) >= 0) continue;  /* live in RAM: shown from the hot set */
        n++;
    }
    return n;
}

/* Total pageable pages under `ns`: RAM hot-set + archive-only. */
static inline int history_nav_count(const history_index *ix,
                                    const micron_store *st, uint8_t ns) {
    return micron_store_ns_count(st, ns) + history_nav_archive_count(ix, st, ns);
}

/* The `rank`-th archive-only (ns,key) under `ns`, newest-first by seq, or NULL.
 * Selection without materialising an array: pick the largest seq strictly below
 * the previous pick, `rank + 1` times. The archive index holds one entry per
 * (ns,key) and the writer's seq is monotonic, so seqs are distinct — no ties. */
static inline const history_index_entry *history_nav_archive_at(
        const history_index *ix, const micron_store *st, uint8_t ns, int rank) {
    if (!ix || !st || rank < 0) return NULL;
    const history_index_entry *pick = NULL;
    int have_prev = 0;
    uint32_t prev_seq = 0;
    for (int step = 0; step <= rank; step++) {
        pick = NULL;
        for (int i = 0; i < HISTORY_INDEX_MAX; i++) {
            const history_index_entry *e = &ix->e[i];
            if (!e->used || e->ns != ns) continue;
            if (micron_store__find(st, ns, e->key) >= 0) continue;   /* deduped: live in RAM */
            if (have_prev && e->seq >= prev_seq) continue;           /* already taken / newer */
            if (!pick || e->seq > pick->seq) pick = e;
        }
        if (!pick) return NULL;   /* ran out before reaching rank */
        prev_seq = pick->seq;
        have_prev = 1;
    }
    return pick;
}

typedef enum {
    HISTORY_NAV_NONE = 0,   /* ordinal out of range */
    HISTORY_NAV_RAM,        /* live in the hot-set: render from RAM (slot) */
    HISTORY_NAV_ARCHIVE,    /* older page: fetch its body from SD at offset */
} history_nav_kind;

typedef struct {
    history_nav_kind   kind;
    const micron_slot *slot;                 /* set when kind == RAM */
    uint8_t  ns;                             /* archive identity to fetch */
    char     key[MICRON_STORE_KEY_CAP];
    uint32_t seq;
    uint32_t offset;                         /* file offset of the newest record */
} history_nav_result;

/* Resolve `ordinal` on the combined axis. RAM ordinals return the slot pointer;
 * archive ordinals return the identity + file offset for a bounded SD read. */
static inline history_nav_result history_nav_at(const history_index *ix,
                                                const micron_store *st,
                                                uint8_t ns, int ordinal) {
    history_nav_result r;
    memset(&r, 0, sizeof(r));
    r.kind = HISTORY_NAV_NONE;
    if (!st || ordinal < 0) return r;

    int ram = micron_store_ns_count(st, ns);
    if (ordinal < ram) {
        const micron_slot *s = micron_store_ns_at(st, ns, ordinal);
        if (s) { r.kind = HISTORY_NAV_RAM; r.slot = s; }
        return r;
    }
    const history_index_entry *e = history_nav_archive_at(ix, st, ns, ordinal - ram);
    if (e) {
        r.kind = HISTORY_NAV_ARCHIVE;
        r.ns = e->ns;
        memcpy(r.key, e->key, MICRON_STORE_KEY_CAP);
        r.seq = e->seq;
        r.offset = e->offset;
    }
    return r;
}

/* --- seq reseed: the maximum stored seq across the whole archive ------------ */

/*
 * A tiny scan accumulator so the hw glue can re-seed its monotonic sequence on
 * mount (see the APPEND-ONLY / NEWEST-WINS note above). It is namespace/key
 * agnostic — the writer owns ONE global counter, so what matters is the single
 * highest seq ever appended, not the max per identity. The glue drives one
 * bounded chunked scan (history_reader) with history_seq_scan_observe as the
 * per-record callback, then sets g_hist_seq = max_seq (0 if the archive is
 * empty/absent). Pure integer logic so the host test exercises the exact reseed
 * the device performs.
 */
typedef struct {
    uint32_t max_seq;   /* highest seq observed so far */
    int      any;       /* 0 until the first record is observed */
} history_seq_scan;

static inline void history_seq_scan_init(history_seq_scan *s) {
    s->max_seq = 0;
    s->any = 0;
}

static inline void history_seq_scan_observe(history_seq_scan *s,
                                            const history_record *rec) {
    if (!s->any || rec->seq > s->max_seq) {
        s->max_seq = rec->seq;
        s->any = 1;
    }
}

/* The counter value to seed after a scan: the max stored seq, or 0 if empty. */
static inline uint32_t history_seq_scan_result(const history_seq_scan *s) {
    return s->any ? s->max_seq : 0;
}

/* --- streaming reassembly reader (the chunked-read core) ------------------- */

/*
 * The bus-safe archive scan reads the file in bounded chunks (<= 4096 B under
 * the SPI bus lock, released between chunks — see skills/history.cpp). A record
 * can straddle any chunk boundary, so reassembly cannot live in the per-chunk
 * loop; it lives HERE, in the pure core, where the host test drives it with
 * hostile chunk splits. This is exactly the agents.cpp bug class (a line/record
 * spanning a chunk edge) and it is tested explicitly.
 *
 * The reader keeps a carry buffer of exactly one maximum record. push() tops the
 * buffer up from the incoming chunk, then drains every WHOLE record from the
 * front, invoking the callback per record with its stream offset. A corrupt
 * front (bad magic/header/crc/key) drops ONE byte and resyncs, so a garbage span
 * can never consume or corrupt the valid records around it.
 */
typedef void (*history_on_record)(void *ctx, const history_record *rec,
                                  uint32_t offset);

typedef struct {
    uint8_t  buf[HISTORY_REC_MAX];   /* carry: at most one whole record */
    size_t   fill;                   /* valid bytes in buf */
    uint32_t rec_start;              /* stream offset of buf[0] */
    history_record scratch;          /* decode target (keeps it off the callback stack) */
} history_reader;

static inline void history_reader_init(history_reader *r) {
    r->fill = 0;
    r->rec_start = 0;
}

static inline void history_reader_push(history_reader *r, const uint8_t *data,
                                       size_t n, history_on_record cb, void *ctx) {
    size_t off = 0;
    for (;;) {
        /* Top the carry buffer up from the incoming chunk. */
        while (off < n && r->fill < HISTORY_REC_MAX) r->buf[r->fill++] = data[off++];

        /* Drain every whole record (and every droppable garbage byte) from the
         * front of the carry. */
        int drained = 0;
        for (;;) {
            size_t consumed = 0;
            history_decode_status st =
                history_decode(r->buf, r->fill, &r->scratch, &consumed);
            if (st == HISTORY_DECODE_OK) {
                if (cb) cb(ctx, &r->scratch, r->rec_start);
                r->fill -= consumed;
                if (r->fill) memmove(r->buf, r->buf + consumed, r->fill);
                r->rec_start += (uint32_t)consumed;
                drained = 1;
                continue;
            }
            if (st == HISTORY_DECODE_NEED_MORE) break;   /* need more bytes */
            /* Corrupt front byte: drop it and resync. */
            if (r->fill == 0) break;
            r->fill -= 1;
            memmove(r->buf, r->buf + 1, r->fill);
            r->rec_start += 1;
            drained = 1;
        }

        if (off >= n) break;   /* whole chunk consumed */
        /* Defensive anti-livelock: buffer full, nothing drained, input left.
         * Unreachable (a full buffer always decodes OK or yields a droppable
         * byte), but drop one byte rather than spin if invariants ever drift. */
        if (!drained && r->fill >= HISTORY_REC_MAX) {
            r->fill -= 1;
            memmove(r->buf, r->buf + 1, r->fill);
            r->rec_start += 1;
        }
    }
}
