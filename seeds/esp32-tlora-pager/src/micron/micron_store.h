#pragma once

/*
 * micron/micron_store.h — the system layer (ticket C4). C1 (micron.h) parses a
 * line, C2 (micron_render.h) owns the cell grid + diff, C3 (micron_layout.h)
 * lays a whole page source into the grid. This file is the STORE that sits in
 * front of all of them: a small fixed set of pages the wheel can flip through
 * from the home screen.
 *
 * Everything here is PURE integer/byte logic — no Arduino, no heap, no clock
 * call (the monotonic "now" is INJECTED as now_ms so the host test drives expiry
 * deterministically; hw_ui.cpp reads the real clock at the call site and passes
 * it in). tools/test_micron_store.cpp exercises the whole store on the host.
 *
 * SECURITY BOUNDARY (the point of this layer)
 * -------------------------------------------
 * A page is namespaced by the DELIVERY ACCOUNT — the transport identity that
 * delivered it (our own trusted boot/data code for the system UI; the
 * untrusted browser transport for a foreign page). The namespace is an argument
 * the CALLER supplies from that transport identity; it is NEVER a field the
 * page's own body can set. A foreign page is therefore structurally unable to
 * land in the system namespace: (namespace, key) is the slot identity, and a
 * lookup/paging call that asks for the system namespace can only ever see slots
 * that were stored under it. The store treats `ns` as an opaque qualifier — it
 * never inspects page content to decide a namespace.
 *
 * INPUT DISCIPLINE (same rigor as the notify control-byte filter / C3 budgets)
 * ---------------------------------------------------------------------------
 * The key is validated at the store boundary and a bad key mutates NOTHING (it
 * is REJECTED, not truncated — a truncated key could alias another key and so
 * cross a slot boundary). The stored source is bounded per slot; a page longer
 * than the cap is cut on a whole UTF-8 code point (the layout decoder is
 * already robust to a truncated tail, but we keep the boundary honest anyway).
 *
 * ARCHIVE-TIER SEAM (a later ticket, NOT implemented here)
 * -------------------------------------------------------
 * The board has PSRAM and a microSD (agents.cpp already uses SD as a graceful
 * overflow store). A future tier may keep a hot set in RAM and flush the rest
 * to an SD archive behind THIS SAME API. So the API does not assume
 * evicted == gone-forever: micron_store_put() reports the displaced slot's
 * identity in its result, and the eviction point is marked so a pre-overwrite
 * "flush to archive" hook can be added there without an API change.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Eight slots — a single named constant so the count is trivially tunable; 8 is
 * this commit's value. Never spell a bare 8 anywhere the slot count is meant. */
#define MICRON_STORE_SLOTS   8

/* Key: strict printable ASCII, 1..33 bytes.
 *
 * 33 is not arbitrary: this store is unified (it holds notify cards as well as
 * micron pages), and real backend keys already exceed 16 —  `sched-<12hex>` is
 * 18, `hook-<12hex>` is 17, and an LXMF dedup identity is a reserved `~` + 32
 * hex = 33. A 16-byte cap would silently collide two distinct cards into one
 * slot (data loss), so the cap holds the longest real key now. */
#define MICRON_STORE_KEY_MAX 33
#define MICRON_STORE_KEY_CAP (MICRON_STORE_KEY_MAX + 1)  /* + NUL = 34 */

/* Stored page source per slot, in bytes.
 *
 * RAM budget: MICRON_STORE_SLOTS * (KEY_CAP + SRC_CAP + ~16 B metadata) with
 * SRC_CAP = 512 is 8 * ~562 = ~4.5 KB of plain static RAM (no PSRAM). Against a
 * C3 build sitting at 58.3% of the ~320 KB DRAM that is ~1.3% — frugal, and in
 * the same scale as the notify queue's per-entry strings. 512 B is a screenful
 * and change of a compact system data page (~13 full 38-column rows), which is
 * what the system layer stores; a page longer than that is cut on a code-point
 * boundary at store time. Bump SRC_CAP (or move to the SD archive tier) when a
 * real page needs more — the cap lives here, in one place. */
#define MICRON_STORE_SRC_CAP 512

/* Namespaces. The store treats this as an OPAQUE uint8_t qualifier; these names
 * are for the callers. SYSTEM is our own trusted data UI; FOREIGN is untrusted
 * browser pages. The security property is proved by the store never letting a
 * lookup in one namespace see a slot stored in another (see the header). */
typedef enum {
    MICRON_NS_SYSTEM  = 0,  /* our own data UI (trusted producer) */
    MICRON_NS_FOREIGN = 1,  /* untrusted browser pages */
} micron_namespace;

/* One stored page. */
typedef struct {
    uint8_t  used;                       /* 0 = free slot */
    uint8_t  ns;                         /* opaque namespace qualifier */
    uint32_t seq;                        /* insertion order → sticky wheel position */
    uint32_t stored_ms;                  /* last store time (injected now_ms):
                                            drives age/expiry and "oldest" */
    uint32_t ttl_ms;                     /* 0 = never expires */
    uint16_t len;                        /* bytes of src in use (<= SRC_CAP) */
    char     key[MICRON_STORE_KEY_CAP];  /* NUL-terminated, validated */
    char     src[MICRON_STORE_SRC_CAP];  /* raw page source (NOT NUL-required) */
} micron_slot;

/* The whole store: a fixed slot array and the next position/sequence to hand
 * out. `next_seq` only ever grows (per NEW insert); an upsert reuses the slot's
 * existing seq so the page keeps its place in the wheel order. */
typedef struct {
    micron_slot slot[MICRON_STORE_SLOTS];
    uint32_t    next_seq;
} micron_store;

/* Outcome of a store. REJECTED means the key failed validation and NOTHING in
 * the store changed. */
typedef enum {
    MICRON_PUT_REJECTED = 0,
    MICRON_PUT_INSERTED,   /* new (ns,key) landed in a slot */
    MICRON_PUT_UPDATED,    /* existing (ns,key) replaced in place (sticky) */
} micron_put_status;

/* Result of micron_store_put(). `evicted` is set when a NEW insert displaced a
 * still-occupied slot (all slots were full and none free/expired-cheap), and
 * then evicted_ns/evicted_key carry the displaced page's identity — the hook a
 * future SD-archive tier would use to flush it. */
typedef struct {
    micron_put_status status;
    int      slot;                          /* index used, -1 when rejected */
    int      evicted;                       /* 1 = a NEW insert overwrote a live slot */
    uint8_t  evicted_ns;
    char     evicted_key[MICRON_STORE_KEY_CAP];
} micron_put_result;

/* --- init ------------------------------------------------------------------ */

static inline void micron_store_init(micron_store *st) {
    memset(st, 0, sizeof(*st));
    st->next_seq = 1;  /* seq 0 is never handed out, so it can mark "unset" */
}

/* --- key validation -------------------------------------------------------- */

/*
 * A key is usable iff it is 1..MICRON_STORE_KEY_MAX bytes and every byte is
 * strict printable ASCII EXCLUDING space (0x21..0x7E). The cap holds the
 * longest real key (LXMF `~`+32hex = 33, backend `sched-`/`hook-` keys), so a
 * key that fits is never rejected for length. Space is refused on purpose even
 * though it is technically printable: a leading/trailing/all-space key would alias
 * ("a" vs "a ") or be blank, the same reason the notify option filter refuses
 * an all-space label. Control bytes, DEL, and any high (>=0x80) byte are out —
 * the panel has no glyphs for them and a key is an identifier, not display
 * text. This is the store's "validate on input" boundary: a key that fails
 * here never reaches a slot.
 */
static inline int micron_store_key_valid(const char *key) {
    if (!key) return 0;
    size_t n = 0;
    for (; key[n]; n++) {
        unsigned char c = (unsigned char)key[n];
        if (c < 0x21 || c > 0x7E) return 0;
        if (n >= MICRON_STORE_KEY_MAX) return 0;  /* > MICRON_STORE_KEY_MAX: reject, never cut */
    }
    return n >= 1 && n <= MICRON_STORE_KEY_MAX;
}

/* --- expiry ---------------------------------------------------------------- */

/*
 * Is this slot expired at `now_ms`? A slot with ttl_ms 0 never expires. The age
 * is (now_ms - stored_ms) in UNSIGNED arithmetic so it stays correct across a
 * millis() rollover, exactly like notify_age_of(). "Expired" is the whole
 * definition of age used by eviction: TTL since the last store.
 */
static inline int micron_slot_expired(const micron_slot *s, uint32_t now_ms) {
    if (!s->used || s->ttl_ms == 0) return 0;
    return (uint32_t)(now_ms - s->stored_ms) >= s->ttl_ms;
}

/* --- internal: find an existing (ns,key) slot ------------------------------ */

/* Returns the slot index holding (ns,key), or -1. The namespace MUST match:
 * this is the enforcement point for isolation — an identical key string under a
 * different namespace is a different slot and is not found here. */
static inline int micron_store__find(const micron_store *st, uint8_t ns,
                                     const char *key) {
    for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
        const micron_slot *s = &st->slot[i];
        if (!s->used || s->ns != ns) continue;
        if (strncmp(s->key, key, MICRON_STORE_KEY_CAP) == 0) return i;
    }
    return -1;
}

/* --- internal: pick a slot for a NEW insert: free -> expired -> oldest ------ */

/*
 * Eviction order, exactly as the ticket asks:
 *   1. a FREE slot (lowest index), else
 *   2. the oldest EXPIRED slot (smallest stored_ms among expired), else
 *   3. the OLDEST slot overall (smallest stored_ms).
 *
 * "Oldest" is least-recently-stored: an upsert refreshes stored_ms, so an
 * actively-updated page is not the one that goes. SLOTS >= 1, so this always
 * returns a valid index. This is the marked EVICTION POINT — a future archive
 * tier flushes st->slot[return value] before micron_store_put() overwrites it.
 */
static inline int micron_store__pick(const micron_store *st, uint32_t now_ms) {
    for (int i = 0; i < MICRON_STORE_SLOTS; i++)
        if (!st->slot[i].used) return i;

    int best = -1;
    uint32_t best_age = 0;
    for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
        if (!micron_slot_expired(&st->slot[i], now_ms)) continue;
        uint32_t age = (uint32_t)(now_ms - st->slot[i].stored_ms);
        if (best < 0 || age > best_age) { best = i; best_age = age; }
    }
    if (best >= 0) return best;

    best = 0;
    best_age = (uint32_t)(now_ms - st->slot[0].stored_ms);
    for (int i = 1; i < MICRON_STORE_SLOTS; i++) {
        uint32_t age = (uint32_t)(now_ms - st->slot[i].stored_ms);
        if (age > best_age) { best = i; best_age = age; }
    }
    return best;
}

/* --- internal: bounded key copy (key already validated <= MICRON_STORE_KEY_MAX) --- */

/* A plain bounded copy — the length was already proven <= MICRON_STORE_KEY_MAX
 * by micron_store_key_valid(), so this cannot truncate a valid key. */
static inline void micron_store__copy_key(char *dst, const char *key) {
    size_t i = 0;
    for (; key[i] && i < MICRON_STORE_KEY_MAX; i++) dst[i] = key[i];
    dst[i] = '\0';
}

/* --- internal: copy a bounded, code-point-aligned source into a slot -------- */

static inline void micron_store__set_src(micron_slot *s, const char *src,
                                         size_t len) {
    if (!src) len = 0;
    if (len > MICRON_STORE_SRC_CAP) {
        len = MICRON_STORE_SRC_CAP;
        /* Back off a cut that landed inside a UTF-8 sequence: drop the
         * continuation bytes (10xxxxxx) and the lead byte whose sequence did
         * not fit, so the stored source ends on a whole code point. */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    if (len) memcpy(s->src, src, len);
    s->len = (uint16_t)len;
}

/* --- public: store one page (upsert) --------------------------------------- */

/*
 * Store (ns, key) -> src. `now_ms` is the injected monotonic clock. `ttl_ms` 0
 * means never expire.
 *
 * - A bad key is REJECTED and the store is left byte-for-byte unchanged.
 * - An existing (ns, key) is REPLACED IN PLACE: content, stored_ms and ttl_ms
 *   update, but the slot and its seq are kept, so the page does NOT jump in the
 *   wheel order (sticky position).
 * - A new (ns, key) takes a slot per free -> expired -> oldest and is handed a
 *   FRESH seq, so it appends to the end of the wheel order.
 */
static inline micron_put_result micron_store_put(micron_store *st, uint8_t ns,
                                                 const char *key,
                                                 const char *src, size_t len,
                                                 uint32_t ttl_ms,
                                                 uint32_t now_ms) {
    micron_put_result r;
    memset(&r, 0, sizeof(r));
    r.slot = -1;

    if (!micron_store_key_valid(key)) {
        r.status = MICRON_PUT_REJECTED;
        return r;  /* nothing mutated */
    }

    int idx = micron_store__find(st, ns, key);
    if (idx >= 0) {
        /* Upsert in place: keep used/ns/seq/key, refresh content + clocks. */
        micron_slot *s = &st->slot[idx];
        micron_store__set_src(s, src, len);
        s->stored_ms = now_ms;
        s->ttl_ms = ttl_ms;
        r.status = MICRON_PUT_UPDATED;
        r.slot = idx;
        return r;
    }

    /* New page: pick a slot (this is the eviction point). */
    idx = micron_store__pick(st, now_ms);
    micron_slot *s = &st->slot[idx];
    if (s->used) {
        /* A live slot is being displaced. Report its identity so a future
         * archive tier can flush it (evicted != gone-forever). */
        r.evicted = 1;
        r.evicted_ns = s->ns;
        memcpy(r.evicted_key, s->key, MICRON_STORE_KEY_CAP);
    }

    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->ns = ns;
    s->seq = st->next_seq++;
    s->stored_ms = now_ms;
    s->ttl_ms = ttl_ms;
    /* Key already validated (<= MICRON_STORE_KEY_MAX bytes of printable ASCII): a bounded copy. */
    micron_store__copy_key(s->key, key);
    micron_store__set_src(s, src, len);

    r.status = MICRON_PUT_INSERTED;
    r.slot = idx;
    return r;
}

/* --- public: lookup by (ns, key) ------------------------------------------- */

/* The page stored under (ns, key), or NULL. The namespace must match, so a
 * foreign page is never returned to a system lookup and vice-versa. */
static inline const micron_slot *micron_store_get(const micron_store *st,
                                                  uint8_t ns, const char *key) {
    if (!micron_store_key_valid(key)) return NULL;
    int idx = micron_store__find(st, ns, key);
    return idx >= 0 ? &st->slot[idx] : NULL;
}

/* --- public: wheel paging within one namespace ----------------------------- */

/*
 * How many pages are stored under `ns`. This is the count the wheel pages
 * through from home (called with MICRON_NS_SYSTEM), and it can never include a
 * foreign page.
 */
static inline int micron_store_ns_count(const micron_store *st, uint8_t ns) {
    int n = 0;
    for (int i = 0; i < MICRON_STORE_SLOTS; i++)
        if (st->slot[i].used && st->slot[i].ns == ns) n++;
    return n;
}

/*
 * The `ordinal`-th page under `ns` in STABLE position order (ascending seq), or
 * NULL if out of range. Ordering by seq is what makes an upsert keep its place
 * (sticky) while a new page appends. The wheel steps `ordinal` from 0 to
 * count-1; because the namespace is fixed by the caller, paging from home
 * (MICRON_NS_SYSTEM) can only ever surface system pages.
 */
static inline const micron_slot *micron_store_ns_at(const micron_store *st,
                                                    uint8_t ns, int ordinal) {
    if (ordinal < 0) return NULL;
    /* Selection sort by seq without materialising an index array: walk the
     * order `ordinal + 1` times, each time taking the smallest seq strictly
     * greater than the previous pick. SLOTS is 8, so this is trivially cheap. */
    uint32_t prev_seq = 0;   /* seq 0 is never issued, so nothing equals it */
    const micron_slot *pick = NULL;
    for (int step = 0; step <= ordinal; step++) {
        pick = NULL;
        for (int i = 0; i < MICRON_STORE_SLOTS; i++) {
            const micron_slot *s = &st->slot[i];
            if (!s->used || s->ns != ns) continue;
            if (s->seq <= prev_seq) continue;
            if (!pick || s->seq < pick->seq) pick = s;
        }
        if (!pick) return NULL;   /* ran out of pages before reaching ordinal */
        prev_seq = pick->seq;
    }
    return pick;
}
