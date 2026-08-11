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
 *   - LOCK ORDER: g_hist_mux (archive state) FIRST, bus lock SECOND, release in
 *     reverse. Nothing takes g_hist_mux while holding the bus lock.
 */

#include <SD.h>
#include <SPI.h>

#include "../micron/history_store.h"

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

        /* LOCK ORDER: archive state mutex FIRST, then the bus for the burst. */
        if (g_hist_mux) xSemaphoreTake(g_hist_mux, portMAX_DELAY);
        rec.seq = ++g_hist_seq;
        rec.stamp_ms = millis();
        {
            HwSpiBusGuard bus;  /* one record (<= HISTORY_REC_MAX) = one bus burst */
            File f = g_hist_store->open(HISTORY_PATH, "a");
            if (f) {
                history_sink sink;
                sink.write = history_file_sink_write;
                sink.ctx = &f;
                history_encode(&sink, &rec);
                f.flush();
                f.close();
            }
        }
        if (g_hist_mux) xSemaphoreGive(g_hist_mux);
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
    if (!g_hist_q) return false;
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

/* Current occupancy of the write queue (0 before it is created). For a future
 * /health to show how close the depth-HISTORY_WRITE_QUEUE_DEPTH queue runs. */
[[maybe_unused]]
static uint32_t history_queued(void) {
    return g_hist_q ? (uint32_t)uxQueueMessagesWaiting(g_hist_q) : 0;
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

/* --- seq reseed on mount: newest-wins must survive a reboot ---------------- */

static void history_seedseq_cb(void *ctx, const history_record *rec, uint32_t offset) {
    (void)offset;
    history_seq_scan_observe((history_seq_scan *)ctx, rec);
}

/*
 * Re-seed g_hist_seq from the archive so a fresh append after a reboot outranks
 * every pre-reboot record for the same (ns,key). g_hist_seq starts at 0 each
 * boot and is only advanced by ++, while newest-wins is resolved by seq
 * (history_newest_cb / history_index_observe); without this seed a post-reboot
 * append (seq=1) would be shadowed by a higher-seq pre-reboot record. One bounded
 * chunked scan (the SAME bus discipline as any read) finds the max stored seq;
 * the next ++g_hist_seq then yields max+1. Empty/absent archive => stays 0.
 * Runs before the write task starts, so no concurrent append can race it.
 */
static void history_seed_seq() {
    history_seq_scan s;
    history_seq_scan_init(&s);
    history_scan_chunked(history_seedseq_cb, &s);
    g_hist_seq = history_seq_scan_result(&s);
    Serial.printf("[history] seq reseeded from archive: max=%u\n",
                  (unsigned)g_hist_seq);
}

/* --- boot: mount + create the queue and the write task --------------------- */

static void history_begin() {
    history_mount();
    g_hist_mux = xSemaphoreCreateMutex();
    history_seed_seq();   /* max stored seq -> g_hist_seq, before the writer starts */
    g_hist_q = xQueueCreate(HISTORY_WRITE_QUEUE_DEPTH, sizeof(HistoryWriteItem));
    if (g_hist_q) {
        xTaskCreate(history_write_task, "hist_wq", HISTORY_WRITE_TASK_STACK,
                    nullptr, HISTORY_WRITE_TASK_PRIO, &g_hist_task);
    }
    Serial.printf("[history] write queue depth=%d task=%s store=%s seq=%u\n",
                  HISTORY_WRITE_QUEUE_DEPTH, g_hist_task ? "up" : "FAILED",
                  history_store_name(), (unsigned)g_hist_seq);
}
