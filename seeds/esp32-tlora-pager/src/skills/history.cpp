/*
 * skills/history.cpp — hardware glue for the persistent history ARCHIVE
 * (ticket TLORA-HISTORY / C1). The pure codec + index + reassembly live in
 * micron/history_store.h (host-tested); this file is the on-device seam:
 *
 *   1. history_mount()   — mount the append-only archive on removable microSD,
 *                          with a graceful SPIFFS fallback (mirrors agents.cpp).
 *   2. a WRITE QUEUE      — a dedicated FreeRTOS task drains a queue and does the
 *                          actual SD append, so enqueue() returns in well under a
 *                          millisecond and NO producer ever writes SD on the loop
 *                          task. This is the whole point of C1: the firmware's
 *                          bug history is loop-blocking I/O (RETICULUM's
 *                          -DRNS_PERSIST_PATHS=0 was the same class of stall); an
 *                          SD append on the 8 ms loop repeats it, so from the very
 *                          first commit the append runs OFF the loop.
 *   3. a CHUNKED READER   — a whole-file scan that holds the SPI bus for at most
 *                          HISTORY_SCAN_BUS_CHUNK bytes per burst, releasing and
 *                          yielding between chunks; reassembly of a record that
 *                          straddles a chunk boundary is done by the pure
 *                          history_reader.
 *
 * NO PRODUCER IS WIRED YET. history_begin() mounts + starts the task; C2 attaches
 * the micron page store's eviction seam to history_enqueue(), C3 the notify
 * cards. history_enqueue()/history_read_newest() are the forward seam and are
 * [[maybe_unused]] until then.
 *
 * SHARED FSPI BUS (same rules as agents.cpp / hw_ui.cpp):
 *   - SD is mounted on the SHARED FSPI SPIClass (hw_ui_spi()), never a second
 *     SPIClass — a second instance on these pins hangs the SX1262.
 *   - Every g_hist_store I/O burst holds HwSpiBusGuard for the whole logical op.
 *   - LOCK ORDER: wherever BOTH are taken (history_read_at, history_scan_chunked)
 *     g_hist_mux (archive state) is taken FIRST, bus lock SECOND, released in
 *     reverse. Nothing takes g_hist_mux while holding the bus lock. The write task
 *     does NOT nest them: its SD append takes ONLY the bus (an EOF append is
 *     correct against readers without the mux), then AFTER close() it takes ONLY
 *     g_hist_mux to publish the record's offset into the RAM index — so a loop-task
 *     nav lookup waits on that tiny index update, never on the writer's SD I/O.
 */

#include <SD.h>
#include <SPI.h>

#include "../micron/history_store.h"
#include "../psram_alloc.h"   // psram_calloc_pref: index + queue storage in PSRAM

/* Single flat, short archive path (SPIFFS 32-byte object-name limit): no
 * directory, no extension games — "/hist.log" is 9 bytes. */
#define HISTORY_PATH               "/hist.log"

/* Off-loop write queue. By-value FreeRTOS queue: depth 16 x sizeof(item)
 * (~549 B) ~= 8.8 KB DRAM, the same scale as the agents viewport. */
#define HISTORY_WRITE_QUEUE_DEPTH  16
#define HISTORY_WRITE_TASK_STACK   8192
#define HISTORY_WRITE_TASK_PRIO    1

/* Bus-hold budget for a whole-file archive scan: read at most this many bytes
 * under the SPI bus lock, then release + yield so the loop task can service the
 * SX1262 FIFO / repaint. 4 KB ~= 12 ms at ~350 KB/s — well under the ~40 ms that
 * would hitch the radio. Identical budget to agents.cpp's AGENT_SCAN_BUS_CHUNK. */
#define HISTORY_SCAN_BUS_CHUNK     4096u

/* One queued append. Fixed size so the FreeRTOS queue copies it by value; the
 * producer hands over (ns, key, payload) and returns immediately. */
struct HistoryWriteItem {
    uint8_t  ns;
    uint16_t len;                         /* payload bytes (<= HISTORY_PAYLOAD_CAP) */
    char     key[MICRON_STORE_KEY_CAP];   /* NUL-terminated, validated at enqueue */
    uint8_t  payload[HISTORY_PAYLOAD_CAP];
};

static FS               *g_hist_store   = nullptr;   /* &SD (primary) or &SPIFFS */
static bool              g_hist_is_sd    = false;
static QueueHandle_t     g_hist_q        = nullptr;
static TaskHandle_t      g_hist_task     = nullptr;
static SemaphoreHandle_t g_hist_mux      = nullptr;  /* archive state; taken before the bus */
static uint32_t          g_hist_seq      = 0;        /* monotonic; re-seeded from archive on mount, only the write task advances it */
static uint32_t          g_hist_drops    = 0;        /* records dropped on a full write queue (data-loss visibility) */
/* RAM read/navigation index (ticket C2): newest record offset per (ns,key), a
 * bounded MRU-by-seq window over the archive (history_store.h owns the policy).
 * Seeded from the mount scan, advanced by the write task on each append, all
 * mutation under g_hist_mux. The wheel navigates it in O(1) instead of scanning
 * the whole archive per detent; a page body is fetched from SD only on open. */
/* Parked in PSRAM (~3 KB, HISTORY_INDEX_MAX entries): allocated once by
 * history_begin() before the write task or any reader runs; internal-DRAM
 * fallback if PSRAM is absent. Mutated by the write task, read by nav/getters,
 * all under g_hist_mux — task context only, never an ISR. */
static history_index    *g_hist_index   = nullptr;

static const char *history_store_name() { return g_hist_is_sd ? "sd" : "spiffs"; }

/* --- mount: SD on the shared bus, graceful SPIFFS fallback ----------------- */

/* Mirrors agents_store_init(): the SAME SD singleton and "/sd" mountpoint the
 * agents store already mounts (one card, one mount, different files). Last arg
 * false = never format on a failed probe — a true there would wipe the card. */
static void history_mount() {
    g_hist_store = nullptr;
    g_hist_is_sd = false;
    if (hw_ui_spi()) {
        pinMode(PIN_SD_CS, OUTPUT);
        digitalWrite(PIN_SD_CS, HIGH);
        HwSpiBusGuard bus;  /* card init probes the shared bus */
        if (SD.begin(PIN_SD_CS, *hw_ui_spi(), 4000000, "/sd", 5, false)) {
            g_hist_store = &SD;
            g_hist_is_sd = true;
        }
    }
    if (!g_hist_store) {
        g_hist_store = &SPIFFS;   /* graceful fallback — identical append behaviour */
        g_hist_is_sd = false;
    }
    Serial.printf("[history] archive store=%s (%s)\n", history_store_name(),
                  g_hist_is_sd ? "SD" : "SPIFFS fallback");
}

/* --- the File byte sink the codec streams into ----------------------------- */

static int history_file_sink_write(void *ctx, const uint8_t *p, size_t n) {
    File *f = (File *)ctx;
    size_t w = f->write(p, n);
    return (w == n) ? (int)n : -1;
}

/* --- the off-loop write task ----------------------------------------------- */

/*
 * Drains the queue forever and does the ONLY SD append in this file. The append
 * is the loop-blocking I/O C1 exists to move off the loop task: it costs a file
 * open + a streamed record + flush + close, all here on this task's own 8 KB
 * stack, never on the 16 KB loop task. seq/stamp are assigned HERE (the pure
 * core takes them injected) so append order and monotonic seq are owned by the
 * single writer.
 */
static void history_write_task(void *arg) {
    (void)arg;
    HistoryWriteItem item;
    for (;;) {
        if (xQueueReceive(g_hist_q, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!g_hist_store) continue;   /* no store — drop (mount always sets one) */

        history_record rec;
        memset(&rec, 0, sizeof(rec));
        rec.ns = item.ns;
        rec.len = item.len;
        memcpy(rec.key, item.key, sizeof(rec.key));
        rec.key[MICRON_STORE_KEY_CAP - 1] = '\0';
        if (item.len) memcpy(rec.payload, item.payload, item.len);

        /* seq/stamp are WRITER-PRIVATE: g_hist_seq is advanced only by this single
         * task (re-seeded at boot before the task starts) and read by no one else,
         * so it needs no lock. It must be assigned HERE, before history_encode,
         * because the seq is part of the record streamed to SD. */
        rec.seq = ++g_hist_seq;
        rec.stamp_ms = millis();

        /* THE SD APPEND TAKES ONLY THE BUS — NOT g_hist_mux. HwSpiBusGuard already
         * serialises this burst against every other bus user, and mode "a" appends
         * at EOF, so the write is correct against concurrent readers without the
         * archive mutex. Holding g_hist_mux across this open/write/flush/close (as
         * the pre-fix code did) would re-couple the loop task's pure-RAM nav
         * lookups (history_nav_page_*, mux-only) to the writer's SD I/O — the exact
         * loop-blocking coupling C1 kept off the loop. So the burst is bus-only,
         * and rec_off (the pre-append EOF offset) is captured as part of it. */
        uint32_t rec_off = 0;
        bool wrote = false;
        {
            HwSpiBusGuard bus;  /* one record (<= HISTORY_REC_MAX) = one bus burst */
            File f = g_hist_store->open(HISTORY_PATH, "a");
            if (f) {
                rec_off = (uint32_t)f.size();  /* start offset of this append (for the index) */
                history_sink sink;
                sink.write = history_file_sink_write;
                sink.ctx = &f;
                if (history_encode(&sink, &rec)) wrote = true;
                f.flush();
                f.close();
            }
        }

        /* PUBLISH into the RAM read/nav index ONLY AFTER close(), under g_hist_mux.
         * The nav readers (history_nav_page_*) and history_read_at take g_hist_mux
         * to read the index; publishing rec_off only after the record is fully
         * written+closed guarantees they can only ever observe an offset whose
         * record is COMPLETE — never a torn/partial append. This tiny in-RAM
         * critical section (no bus, no I/O) is the ONLY thing a loop-task lookup
         * can now wait on; it never waits on the SD burst above. The append burst
         * takes only the bus and this publish takes only the mux, so the two are
         * no longer nested and cannot invert the mux->bus order held elsewhere. */
        if (wrote && g_hist_index) {
            if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
            history_index_observe(g_hist_index, &rec, rec_off);
            if (g_hist_mux) xSemaphoreGive(g_hist_mux);
        }
    }
}

/* --- producer-facing seam: enqueue an append (off-loop) -------------------- */

/*
 * Hand one record to the write task and return immediately (xQueueSend with a 0
 * tick timeout: no producer ever blocks on SD, and a full queue drops rather
 * than stalls the loop). This is the ONLY write entry point — there is no
 * synchronous SD-append API by construction, which is what keeps the write off
 * the loop task. Wired by C2/C3; unused in C1.
 */
[[maybe_unused]]
static bool history_enqueue(uint8_t ns, const char *key,
                            const uint8_t *payload, size_t len) {
    /* No write queue means the ~8.8 KB alloc failed at boot (history_begin) and
     * this record can NEVER be persisted. Count it as a drop before returning,
     * exactly like a full-queue drop: otherwise a dead queue looks healthy on
     * /health (history_drops stays 0) while the archive silently writes nothing.
     * A plain ++ matches the full-queue path below; the counter is monotonic and
     * a torn RMW only loses a count, never fabricates one. */
    if (!g_hist_q) {
        g_hist_drops++;
        return false;
    }
    if (!micron_store_key_valid(key)) return false;
    if (len > HISTORY_PAYLOAD_CAP) return false;

    HistoryWriteItem item;
    memset(&item, 0, sizeof(item));
    item.ns = ns;
    item.len = (uint16_t)len;
    snprintf(item.key, sizeof(item.key), "%s", key);
    if (len && payload) memcpy(item.payload, payload, len);

    /* 0-tick send: a producer NEVER blocks on SD. A full queue DROPS this record
     * (counted in g_hist_drops, surfaced via history_drops() for a future C4
     * /health) rather than silently vanishing. The 0-tick non-blocking send is
     * deliberate — blocking the loop task is the stall the queue exists to avoid;
     * queue depth / backpressure is a later decision (C2/C3) once real producer
     * rates are known, not something to paper over with a bigger buffer now. */
    if (xQueueSend(g_hist_q, &item, 0) != pdTRUE) {
        g_hist_drops++;
        return false;
    }
    return true;
}

/* --- health seam: drop counter + queue occupancy (wired to /health by C4) --- */

/* Monotonic count of records dropped because the write queue was full — the data
 * loss C1 exists to make VISIBLE instead of silent. Pure accessor; not wired yet. */
[[maybe_unused]]
static uint32_t history_drops(void) { return g_hist_drops; }

/* Current occupancy of the write queue (0 before it is created). Wired to
 * /health by C4 as an EARLY warning: the queue fills (queued climbs toward
 * HISTORY_WRITE_QUEUE_DEPTH) BEFORE it overflows and drops start. uxQueueMessages-
 * Waiting is a lock-free FreeRTOS read, safe from the AsyncTCP /health task. */
static uint32_t history_queued(void) {
    return g_hist_q ? (uint32_t)uxQueueMessagesWaiting(g_hist_q) : 0;
}

/* Is the write queue live (true) or did its boot alloc fail (false)? When false
 * every history_enqueue drops (counted in g_hist_drops) and the archive persists
 * NOTHING, so /health must be able to see a dead queue directly rather than infer
 * it. Pure lock-free read of the queue handle, latched ONCE by history_begin() at
 * boot before any handler runs (same discipline as history_on_sd) — no lock, no
 * bus, safe straight from the AsyncTCP /health handler. Wired to /health by C4. */
static bool history_ready(void) { return g_hist_q != nullptr; }

/* Is the archive on the removable microSD (true) or the SPIFFS fallback (false)?
 * Pure read of the mount result latched ONCE by history_mount() at boot, before
 * any reader runs and never mutated afterwards — so it needs no lock and no bus,
 * and is safe to call straight from the AsyncTCP /health handler. Wired to
 * /health by C4 so the store tier ("did the card mount?") is verifiable live. */
static bool history_on_sd(void) { return g_hist_is_sd; }

/* How many distinct (ns,key) identities the RAM read/nav index currently holds —
 * the cheapest HONEST "records known" number for /health. This is INDEXED
 * IDENTITIES (the bounded MRU-by-seq window, HISTORY_INDEX_MAX max), NOT the
 * whole-archive record count: a true archive total would need a full SD scan,
 * which the /health handler must never do. Taken under g_hist_mux only — the SAME
 * brief, bus-free critical section the nav readers (history_nav_page_*) use, since
 * the write task mutates the index; NO bus is touched, so no lock-order/deadlock
 * risk on the AsyncTCP task (mux-only, never mux->bus here). Wired to /health by C4. */
static uint32_t history_index_live_count(void) {
    uint32_t n = 0;
    if (!g_hist_index) return 0;
    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
    n = (uint32_t)g_hist_index->count;
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);
    return n;
}

/* --- chunked archive scan over the real FS --------------------------------- */

/*
 * Scan the whole archive, invoking `cb` per reassembled record. The bus is held
 * for at most HISTORY_SCAN_BUS_CHUNK bytes per read, then released and yielded,
 * so a months-old archive never freezes the radio/clock for the whole read. The
 * cross-chunk reassembly is the pure history_reader's job (a record may straddle
 * any boundary). g_hist_mux is held for the WHOLE scan (only the bus is chunked),
 * which keeps the file immutable between chunks: the write task also takes
 * g_hist_mux before it appends, so no append can land under the open handle.
 */
static bool history_scan_chunked(history_on_record cb, void *ctx) {
    if (!g_hist_store) return false;

    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);

    File f;
    bool opened = false;
    {
        HwSpiBusGuard bus;
        if (g_hist_store->exists(HISTORY_PATH)) {
            f = g_hist_store->open(HISTORY_PATH, "r");
            opened = (bool)f;
        }
    }
    if (!opened) {
        if (g_hist_mux) xSemaphoreGive(g_hist_mux);
        return false;
    }

    history_reader r;
    history_reader_init(&r);
    /* One shared 4 KB scan buffer: only one scan runs at a time (g_hist_mux). */
    static uint8_t chunk[HISTORY_SCAN_BUS_CHUNK];
    for (;;) {
        size_t got = 0;
        {
            HwSpiBusGuard bus;                 /* one bounded chunk = one bus burst */
            got = f.read(chunk, sizeof(chunk));
        }                                       /* bus released here */
        if (got == 0) break;                    /* EOF */
        history_reader_push(&r, chunk, got, cb, ctx);
        taskYIELD();                            /* let the loop task grab the freed bus */
    }
    {
        HwSpiBusGuard bus;
        f.close();
    }
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);
    return true;
}

/* --- reader-facing seam: resolve the newest record for (ns,key) ------------ */

struct HistoryNewest {
    uint8_t        ns;
    const char    *key;
    history_record best;
    bool           found;
};

static void history_newest_cb(void *ctx, const history_record *rec, uint32_t offset) {
    (void)offset;
    HistoryNewest *h = (HistoryNewest *)ctx;
    if (rec->ns != h->ns) return;
    if (strncmp(rec->key, h->key, MICRON_STORE_KEY_CAP) != 0) return;
    /* Newest wins: a later record with seq >= the best supersedes it. */
    if (!h->found || rec->seq >= h->best.seq) {
        h->best = *rec;
        h->found = true;
    }
}

/*
 * The newest archived record for (ns,key), or false. Append-only + newest-wins:
 * a single chunked pass keeps the highest-seq match, so no in-place rewrite is
 * needed to "update" a key. Wired by C2/C3; unused in C1.
 */
[[maybe_unused]]
static bool history_read_newest(uint8_t ns, const char *key, history_record *out) {
    if (!micron_store_key_valid(key)) return false;
    HistoryNewest h;
    h.ns = ns;
    h.key = key;
    h.found = false;
    memset(&h.best, 0, sizeof(h.best));
    history_scan_chunked(history_newest_cb, &h);
    if (h.found && out) *out = h.best;
    return h.found;
}

/* --- bounded single-record read at a known offset (C2 page-body fetch) ------ */

/* Grab the first whole record from a bounded buffer via the PURE reader (no new
 * parse path): the record at an index offset is guaranteed to start there and to
 * fit in HISTORY_REC_MAX, so exactly one push yields it. */
struct HistoryOneRec {
    history_record rec;
    bool           found;
};

static void history_one_cb(void *ctx, const history_record *rec, uint32_t offset) {
    (void)offset;
    HistoryOneRec *o = (HistoryOneRec *)ctx;
    if (!o->found) { o->rec = *rec; o->found = true; }
}

/*
 * Read the single archived record that STARTS at `offset` (a value handed out by
 * the nav index) into *out. One bounded burst: seek + read at most HISTORY_REC_MAX
 * (<= 564 B, one bus burst), then decode via the pure history_reader — NOT a
 * whole-archive scan, so a wheel detent onto an archive page costs one record
 * read, not a file walk. LOCK ORDER holds: g_hist_mux FIRST, bus SECOND. Reads
 * with mode "r", so the off-loop-write pin (only the write task appends) is
 * untouched. Returns false with no store / bad offset / decode miss.
 */
static bool history_read_at(uint32_t offset, history_record *out) {
    if (!g_hist_store) return false;

    /* One scratch record buffer, reused across detents; only the loop task calls
     * this (paging), never re-entrantly, so a file-scope static is safe. */
    static uint8_t buf[HISTORY_REC_MAX];
    size_t got = 0;
    bool opened = false;

    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
    {
        HwSpiBusGuard bus;   /* one bounded record = one bus burst */
        if (g_hist_store->exists(HISTORY_PATH)) {
            File f = g_hist_store->open(HISTORY_PATH, "r");
            if (f) {
                if (f.seek(offset)) got = f.read(buf, sizeof(buf));
                f.close();
                opened = true;
            }
        }
    }
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);

    if (!opened || got == 0) return false;

    HistoryOneRec o;
    o.found = false;
    history_reader r;
    history_reader_init(&r);
    history_reader_push(&r, buf, got, history_one_cb, &o);
    if (o.found && out) *out = o.rec;
    return o.found;
}

/* --- boot-restore seam: rank-th newest archived record under a namespace ----- */

/*
 * Fetch the rank-th newest archived record under `ns` (newest-first by seq),
 * reading its body from SD, for rebuilding a producer's RAM ring at boot — the
 * notify card ring (ticket C3). The RAM index (seeded by the mount scan) resolves
 * the identity and its file offset under g_hist_mux; the body is then read with
 * ONE bounded record read (history_read_at, mode "r"), never a whole-archive scan.
 * The mux is released BEFORE history_read_at so the two mux takes never nest.
 * Returns false past the last indexed identity (rank out of range) or on a read
 * miss — the caller stops paging in cleanly. Runs in setup() single-threaded
 * (before any producer enqueues), so the index cannot shift under the walk.
 */
static bool history_restore_at(uint8_t ns, int rank, history_record *out) {
    uint32_t offset = 0;
    bool have = false;
    if (!g_hist_index) return false;

    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
    const history_index_entry *e = history_index_ns_at(g_hist_index, ns, rank);
    if (e) { offset = e->offset; have = true; }
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);

    if (!have) return false;
    return history_read_at(offset, out);   /* takes g_hist_mux itself — not nested */
}

/* --- wheel navigation seam (mutex-guarded reads of the RAM nav index) -------- */

/*
 * The pure navigation core (history_nav_* in history_store.h) is fed the RAM
 * hot-set (the caller's micron_store) and this file's archive index. The index
 * is mutated by the write task, so these thin wrappers hold g_hist_mux for the
 * O(1) lookup — a wheel detent never reads a torn index entry. The returned
 * result is by value (a stable RAM slot pointer, or an archive identity+offset
 * copied out), so it stays valid after the mutex is released.
 */
static int history_nav_page_count(const micron_store *st, uint8_t ns) {
    int n;
    if (!g_hist_index) return 0;
    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
    n = history_nav_count(g_hist_index, st, ns);
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);
    return n;
}

static history_nav_result history_nav_page_at(const micron_store *st, uint8_t ns,
                                              int ordinal) {
    history_nav_result r;
    if (!g_hist_index) { memset(&r, 0, sizeof(r)); return r; }
    if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
    r = history_nav_at(g_hist_index, st, ns, ordinal);
    if (g_hist_mux) xSemaphoreGive(g_hist_mux);
    return r;
}

/* --- seq reseed on mount: newest-wins must survive a reboot ---------------- */

/* One mount-scan pass feeds BOTH the seq reseed and the read/nav index, so the
 * archive is walked exactly once at boot (C1 reseed + C2 index in one scan). */
struct HistorySeedCtx {
    history_seq_scan seq;
    history_index   *ix;
};

static void history_seed_cb(void *ctx, const history_record *rec, uint32_t offset) {
    HistorySeedCtx *c = (HistorySeedCtx *)ctx;
    history_seq_scan_observe(&c->seq, rec);
    if (c->ix) history_index_observe(c->ix, rec, offset);   /* build the nav index in the same pass */
}

/*
 * Re-seed g_hist_seq from the archive so a fresh append after a reboot outranks
 * every pre-reboot record for the same (ns,key), AND build the read/navigation
 * index from what is on the card. g_hist_seq starts at 0 each boot and is only
 * advanced by ++, while newest-wins is resolved by seq (history_newest_cb /
 * history_index_observe); without the reseed a post-reboot append (seq=1) would
 * be shadowed by a higher-seq pre-reboot record. One bounded chunked scan (the
 * SAME bus discipline as any read) finds the max stored seq and populates the
 * MRU index window; the next ++g_hist_seq then yields max+1. Empty/absent archive
 * => seq stays 0 and the index stays empty (nav then shows only the RAM hot-set).
 * Runs before the write task starts, so no concurrent append can race it.
 */
static void history_seed_seq() {
    HistorySeedCtx c;
    history_seq_scan_init(&c.seq);
    c.ix = g_hist_index;   /* may be null (alloc failed) — seed_cb skips the index then */
    history_scan_chunked(history_seed_cb, &c);
    g_hist_seq = history_seq_scan_result(&c.seq);
    Serial.printf("[history] seq reseeded from archive: max=%u, index entries=%d\n",
                  (unsigned)g_hist_seq, g_hist_index ? g_hist_index->count : 0);
}

/* --- boot: mount + create the queue and the write task --------------------- */

/* Static control block for the write queue when its storage lives in PSRAM
 * (xQueueCreateStatic). Small (~sizeof(StaticQueue_t)) and kept in internal RAM;
 * only the ~8.8 KB item storage moves to PSRAM. */
static StaticQueue_t g_hist_q_cb;

static void history_begin() {
    history_mount();
    g_hist_mux = xSemaphoreCreateMutex();
    /* Nav/read index in PSRAM (~3 KB), internal-DRAM fallback. */
    g_hist_index = (history_index *)psram_calloc_pref(sizeof(*g_hist_index));
    if (g_hist_index) history_index_init(g_hist_index);
    else Serial.println("[history] index alloc FAILED — nav index disabled");
    history_seed_seq();   /* max stored seq -> g_hist_seq + nav index, before the writer starts */

    /* Write-queue storage (~8.8 KB = depth x sizeof(item)) in PSRAM via
     * xQueueCreateStatic; the queue is task-context only (enqueue from loop/
     * AsyncTCP, drain on the write task, NEVER an ISR), so PSRAM-backed storage
     * is safe. Fall back to a heap (internal) queue if the PSRAM alloc fails. */
    const size_t q_bytes = (size_t)HISTORY_WRITE_QUEUE_DEPTH * sizeof(HistoryWriteItem);
    uint8_t *q_store = (uint8_t *)heap_caps_malloc(q_bytes, MALLOC_CAP_SPIRAM);
    if (q_store) {
        g_hist_q = xQueueCreateStatic(HISTORY_WRITE_QUEUE_DEPTH, sizeof(HistoryWriteItem),
                                      q_store, &g_hist_q_cb);
    }
    if (!g_hist_q) {   /* PSRAM absent/failed, or static create failed: heap queue */
        g_hist_q = xQueueCreate(HISTORY_WRITE_QUEUE_DEPTH, sizeof(HistoryWriteItem));
    }
    if (g_hist_q) {
        xTaskCreate(history_write_task, "hist_wq", HISTORY_WRITE_TASK_STACK,
                    nullptr, HISTORY_WRITE_TASK_PRIO, &g_hist_task);
    }
    Serial.printf("[history] write queue depth=%d task=%s store=%s seq=%u\n",
                  HISTORY_WRITE_QUEUE_DEPTH, g_hist_task ? "up" : "FAILED",
                  history_store_name(), (unsigned)g_hist_seq);
}
