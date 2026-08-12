/*
 * skills/agents.cpp — pocket chat, on top of the CONVERSATION store
 *
 * WHAT CHANGED AND WHY. This used to be a table of exactly two AGENTS
 * (`claude`, `hermes`), each addressable only by a room name and answerable only
 * by handing its own id back to the bridge. Notification cards are the device's
 * main feature and must be deliverable over EVERY transport, so a thread has to
 * be able to arrive from an LXMF sender or a mesh peer too — and such a thread
 * must remember WHERE to answer, which an agent id cannot express. The record is
 * therefore a Conversation (see ../conv_store.h): it carries a `transport`, an
 * opaque `reply_addr`, and a display `label` that no longer has to be the id.
 * The two agents are now SEEDED conversations of transport CONV_AGENT, so this
 * commit is a pure refactor — same screens, same API, same wire behaviour.
 *
 * Pager is the thin terminal. A bridge on the LAN does the real work:
 *   POST {bridge}/v1/chat  { "agent","session","text" }
 * Answers come back as ordinary /notify
 * (source=claude|hermes) or, when
 * the bridge is missing, as a local stub line in the thread.
 *
 * SPIFFS:
 *   /agent_bridge.txt  — base URL, e.g. http://192.168.1.138:8090  (one line)
 *
 * History:
 *   Non-transient per-(conversation, session) history lives on removable SD as
 *   an append-only JSONL file, one message per line:  {ts, from_me, text}.
 *   Only a small viewport window (the last AGENT_VIEW_MAX messages of the
 *   current session) is kept in RAM as an index; the rest stays on disk, so a
 *   "very long history" costs no extra static RAM. Multiple sessions exist per
 *   conversation under the key (conversation, session); one is active and
 *   selectable.
 *   The chat screen scrolls the whole history page-by-page (older pages are
 *   loaded from disk on demand) and shows the message timestamp with the
 *   sender marker.
 *
 *   Store selection is graceful: SD is mounted on the SHARED FSPI bus
 *   (hw_ui_spi(), same instance the ST7796 and SX1262 use — NOT a second
 *   SPIClass). If the card is absent/fails, the store falls back to SPIFFS.
 *   Either way persistence is append-only JSONL, so behaviour is identical and
 *   there is no hardlock when the SD is missing.
 *
 *   Thread registry is a tiny manifest  /conversations.txt  (one
 *   "key\tlabel\ttransport\treply_hex\tdead" line each) in the same store, so
 *   the room list survives reboots without needing SD directory iteration. The
 *   legacy /agents_sessions.txt is still READ (2- and 3-field lines) so an
 *   existing card keeps its rooms; only the new file is ever written.
 *
 * HTTP API on the seed:
 *   GET  /agents             — list + bridge status + per-agent sessions/history
 *   POST /agents/send        — { "agent":"hermes", "text":"…", "session":"?" }
 *   POST /agents/bridge      — raw text body = bridge URL (empty clears)
 *   POST /agents/session     — { "agent":"hermes", "session":"work" } select
 *   POST /agents/session/new — { "agent":"hermes", "session":"?" } create/switch
 */

#include <SD.h>
#include <SPI.h>

#include "../agents_chat_route.h"  // chat-route seam: claude_route_incoming + pure planner
#include "../conv_store.h"         // the conversation record: keys, manifest, transports
#include "../transport.h"          // the backend contract: transport_send + the inbox doors
#include "../rns/outbox.h"         // the OTHER half of that seam: rns_send_envelope + rns_peer_addr
#include "../mesh/mc_client.h"    // the mesh backend: peer send + known-contact test

/* Conversation slots. A slot used to carry its own AGENT_VIEW_MAX viewport, so
 * the table cost 12.6 KB per conversation and could not grow without spending
 * static RAM the board does not have. The window is shared now (see ConvWindow)
 * and a slot is ~840 bytes, so eight of them cost less than the two used to.
 * g_conv_n is the LIVE count; slots above it are free for minted peers. */
#define CONV_MAX            8   /* seeded + minted peers; ~840 B each now */
#define CONV_SEEDED_N       2       /* claude + hermes, pre-created below */
/* Kept as the capacity spelling the rest of the tree already uses (main.cpp
 * sizes its UI copies with it; meshcore.cpp sizes its per-agent TX flags). */
#define AGENTS_N            CONV_MAX
#define AGENT_ID_LEN        CONV_ID_LEN
/* One chat line / viewport cell. 512 stays: bridge + mesh multi-part chunks. */
#define AGENT_TEXT_LEN      512
/* RAM viewport window: the last N messages of the ACTIVE session per agent.
 * Was a full 40-slot ring (~61 KB); now the disk holds everything and this is
 * only an on-screen tail. 24 is ~3 screens of wrapped 7-row text at scale 2. */
#define AGENT_VIEW_MAX      24
/* Keep the old name for callers (main.cpp sizes its UI copy with it). */
#define AGENT_THREAD_MAX    AGENT_VIEW_MAX
#define AGENT_BRIDGE_LEN    96
#define AGENT_BRIDGE_FILE   "/agent_bridge.txt"
#define AGENT_BRIDGE_TMP    "/agent_bridge.tmp"
#define AGENT_SESSION_LEN   CONV_SESSION_LEN
#define AGENT_SESSIONS_MAX  CONV_SESSIONS_MAX  /* rooms per conversation (cap) */
/* JSONL line worst case: 2x escaped text + wrapper + newline. */
#define AGENT_JSONL_MAX     (AGENT_TEXT_LEN * 2 + 64)
/* Bus-hold budget for a whole-file session scan. A months-old session JSONL
 * has no rotation cap, so a single unbroken scan under the shared SPI bus lock
 * would freeze the SX1262 servicing and the clock tick for the whole read
 * (~300 ms at ~350 KB/s for ~100 KB). Instead every scan below reads at most
 * this many bytes under the bus lock, then releases it and yields so the loop
 * task can drain the radio FIFO / repaint, then re-takes and continues. At
 * ~350 KB/s effective SD throughput 4 KB ≈ 12 ms — well under the ~40 ms that
 * would still hitch the radio. The scan holds agents_mux across the whole
 * call, which keeps the file immutable between chunks (see agents_scan_chunked
 * and the LOCK ORDER note below): only the *bus* is chunked, never the mux. */
#define AGENT_SCAN_BUS_CHUNK  4096u

struct AgentLine {
    bool from_me;
    uint32_t ts;            // unix seconds (C1 JSONL "ts"); 0 = unknown
    char text[AGENT_TEXT_LEN];
};

/* One conversation: a thread source with its own rooms, wire and return
 * address. `id` is the storage id (no '.', it separates id from room in the
 * on-disk key); `label` is what the screen shows; `reply_addr` is OPAQUE — its
 * meaning is the transport's (agent-id bytes / LXMF source hash / mesh pubkey)
 * and nothing in this file interprets it. */
struct Conversation {
    char id[CONV_ID_LEN];             // "claude" / "hermes"; later 8 hex of a peer
    char label[CONV_LABEL_LEN];       // UI display name
    uint8_t transport;                // CONV_AGENT / CONV_LXMF / CONV_MESH
    uint8_t reply_len;                // bytes used in reply_addr (0 = unknown)
    uint8_t reply_addr[CONV_REPLY_MAX];
    /* Compiled in rather than discovered. A seeded conversation's ROUTE is
     * re-derived at every load and never taken from the manifest — see
     * conv_route_resolve() in ../transport.h for why that matters now that
     * transport chooses the wire a reply leaves on. */
    uint8_t seeded;
    char sessions[AGENT_SESSIONS_MAX][AGENT_SESSION_LEN];
    uint8_t n_sessions;
    uint8_t active_idx;         // into sessions[]
    uint32_t session_lines[AGENT_SESSIONS_MAX];  // persisted msg count per session
    uint32_t lines;             // total messages in the active session
    /* Newest line of the active room, kept for the picker and GET /agents. This
     * is ALL the RAM a conversation nobody is looking at needs: the scrollable
     * window lives in g_win (one, shared) because only one conversation is ever
     * on screen. Holding a 24-message window per conversation cost 12,480 of
     * the record's 12,796 bytes and bought nothing — see ConvWindow below. */
    AgentLine last;
    /* Monotonic use stamp, for choosing an eviction victim (see conv_mint). */
    uint32_t last_use;
    /* Per-session liveness (live-room roster, README). 0 = alive/shown,
     * 1 = dead/hidden from the room picker. MARK-NOT-DELETE: a dead room's
     * JSONL history file is never removed — hiding is reversible (a later
     * roster can un-mark it), deletion is not and is user-only. */
    uint8_t session_dead[AGENT_SESSIONS_MAX];
};

/*
 * THE ONE SCROLLBACK WINDOW.
 *
 * Every viewport caller in main.cpp passes agent_focus — the chat screen draws
 * exactly one conversation — so the window is a property of the SCREEN, not of
 * each conversation. It used to be per-conversation, which meant every slot
 * carried a 24-message buffer that only mattered while that slot was the one
 * being looked at, and made each new conversation cost 12.5 KB of static RAM.
 * With one shared window a conversation costs ~840 bytes, so the table can hold
 * real peers for LESS total RAM than two conversations used to take.
 *
 * `owner` is the conversation the window is currently loaded for. It changes
 * only on an explicit focus (agents_thread_goto_tail / _goto_page, and the
 * lazy focus in agents_thread_view), never as a side effect of a message
 * arriving — an inbound line for a conversation nobody is reading must not
 * blank the chat that IS open. Those conversations take the line into `last`
 * and their counters; the full window is rebuilt from the JSONL when the user
 * actually opens them, which is the same disk work the old code did on every
 * room switch anyway. The disk is the store; this is only a display cache.
 */
struct ConvWindow {
    int      owner;             // conversation index loaded here, -1 = none
    uint32_t file_sync;         // JSONL bytes already folded into view
    uint8_t  vn;                // messages currently in the window
    uint32_t win_start;         // absolute line index of view[0] (tail or page)
    AgentLine view[AGENT_VIEW_MAX];
};
static ConvWindow g_win = { -1, 0, 0, 0, {} };

/* Bumped on every append/select so eviction can find the least-recently-used
 * conversation without a timestamp (the clock is not trustworthy at boot). */
static uint32_t g_conv_clock = 0;

/* The two bridge/C1 agents, pre-created as CONV_AGENT conversations. Their
 * return address is the agent id's own bytes, which is exactly what the bridge
 * and the MeshCore C1 DM have always used to answer them. Fields not named here
 * are value-initialised (empty room list, empty viewport). */
static Conversation g_convs[CONV_MAX] = {
    { .id = "claude", .label = "CLAUDE", .transport = CONV_AGENT, .reply_len = 6,
      .reply_addr = { 'c', 'l', 'a', 'u', 'd', 'e' }, .seeded = 1 },
    { .id = "hermes", .label = "HERMES", .transport = CONV_AGENT, .reply_len = 6,
      .reply_addr = { 'h', 'e', 'r', 'm', 'e', 's' }, .seeded = 1 },
};
/* Live conversations in g_convs[]. A constant today; the seam a later commit
 * uses to append a peer conversation without touching every loop below. */
static uint8_t g_conv_n = CONV_SEEDED_N;

static char g_bridge[AGENT_BRIDGE_LEN] = "";
static char g_session[AGENT_SESSION_LEN] = "pager";
static FS *g_store = nullptr;        // &SD (primary) or &SPIFFS (fallback)
static bool g_store_is_sd = false;

/* Recursive mutex: agents_push_line() locks itself (mesh task calls it without
 * a lock) while send/inbound/view also take the same lock (nested). File IO
 * happens under the mutex, never under portENTER_CRITICAL — SD writes block.
 *
 * TWO locks, two concerns: agents_mux guards store STATE (g_convs, view
 * windows, session registry); the shared SPI bus lock (HwSpiBusGuard /
 * hw_spi_bus_lock, hw_ui.h) guards the BUS the SD card shares with the
 * ST7796 and SX1262. These routes run on the AsyncTCP task while the loop
 * task paints and services the radio, so every g_store/SD I/O burst below is
 * wrapped in the bus lock. (The SPIFFS fallback store is internal flash, not
 * on the FSPI bus — wrapping it too is harmless and keeps one invariant:
 * ALL g_store I/O holds the bus lock.)
 * LOCK ORDER: agents_mux FIRST, bus lock SECOND, release in reverse. The bus
 * lock is non-recursive, so the guarded regions never call anything that
 * takes it again (pure FS calls + parsing only), and nothing anywhere takes
 * agents_mux while holding the bus lock. */
static SemaphoreHandle_t agents_mux = nullptr;

static void agents_lock() {
    if (agents_mux) xSemaphoreTakeRecursive(agents_mux, portMAX_DELAY);
}
static void agents_unlock() {
    if (agents_mux) xSemaphoreGiveRecursive(agents_mux);
}

static unsigned long agents_now_s() {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000)
        return (unsigned long)tv.tv_sec;
    return millis() / 1000;
}

/* Forward decls (defined later in this TU; store section uses them early). */
static void agents_view_append(ConvWindow &w, bool from_me, uint32_t ts,
                               const char *text);
static int agents_find(const char *id);
static int agents_session_add(int idx, const char *name);

/* Public UI accessors (non-static on purpose — main.cpp needs them cleanly).
 * Session list, timestamps and the long-scroll paging contract for the chat. */
int    agents_session_count(int idx);
const char *agents_session_name(int idx, int i);
bool   agents_session_is_active(int idx, int i);
int    agents_session_msg_count(int idx, int i);
bool   agents_session_is_dead(int idx, int i);   // live-room roster: hidden?
int    agents_session_visible_count(int idx);    // non-dead session rows
bool   agents_session_refresh_counts(int idx);   // recount all store sessions
const char *agents_active_session(int idx);
bool   agents_session_select(int idx, const char *name);
int    agents_session_create(int idx, const char *wanted, bool &created);
uint32_t agents_thread_total(int idx);           // messages in active session
uint32_t agents_thread_start(int idx);           // absolute line of view[0]
bool   agents_thread_is_tail(int idx);           // window pinned at latest?
uint32_t agents_thread_line_ts(int idx, int i);  // ts of view[i] (0 unknown)
void   agents_thread_goto_tail(int idx);         // rewind to latest (sync)
void   agents_thread_goto_page(int idx, uint32_t end_line);  // load older page
int    agents_thread_view(int idx, char out[][AGENT_TEXT_LEN], bool *from_me,
                          uint32_t *ts_out, int max_lines);

/* GPS location collaboration. gps.cpp is #include'd AFTER this file in the
 * same TU, so these are prototypes resolved to the gps.cpp section below.
 * agents.cpp answers a "where are you?" with the freshest fix instead of
 * forwarding it to the bridge. */
bool gps_get_position(double *lat, double *lon, uint32_t *ts, bool *fix,
                      int *sats);
long gps_fix_age_s(void);
float gps_get_hdop(void);
void gps_set_on_fix(void (*cb)(void));
void gps_request_fix(void);

/* Defined later in this file (used by the GPS interception below). */
static void agents_on_inbound(const char *agent_id, const char *text,
                              bool real_inbound);

/* A genuine arrival (mesh C1 RX, HTTP /agents/inbound, a GPS answer landing)
 * pushed a line into a room. loop() consumes it together with display_force:
 * if the room is open on screen, the repaint wakes the panel once. Synthetic
 * error lines never set this. Set BEFORE display_force so the loop cannot
 * consume the repaint without seeing the arrival. */
static volatile bool g_agents_real_inbound = false;

/* ---- store ---------------------------------------------------------------- */

static const char *agents_store_name() { return g_store_is_sd ? "sd" : "spiffs"; }

static bool agents_store_ready() { return g_store != nullptr; }

/* Flat key path (no directory on SPIFFS; FAT root on SD) for one thread of one
 * conversation: /conv.<conv_id>[.<session>]. The old form was
 * /agent.<id>.<session>, which had no guard on the SPIFFS object-name limit —
 * a long room name produced a path the store silently refused to create, i.e.
 * an empty thread and a black chat screen (the same trap that already cost the
 * ".jsonl" extension). conv_log_path() now HOLDS that budget, folding an
 * over-long key onto a fingerprinted name instead of overflowing it, and
 * returns "" if it cannot — an empty path opens no file, so every caller
 * degrades the way a missing store already does. Content stays JSONL. */
static String agents_log_path(const char *conv_id, const char *session) {
    char path[CONV_PATH_LEN];
    if (!conv_log_path(path, sizeof(path), conv_id, session)) return String("");
    return String(path);
}

/* Legacy key of the same thread, read once at boot so an existing card's
 * history follows it into the new namespace (see agents_migrate_logs). */
static String agents_legacy_log_path(const char *conv_id, const char *session) {
    return String("/agent.") + conv_id + "." + session;
}

static void agents_session_sanitize(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    if (!out || out_n == 0) return;
    if (in) {
        for (const char *p = in; *p && j + 1 < out_n; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
                out[j++] = c;
        }
    }
    out[j] = '\0';
}

/* Append one message to the JSONL file for (agent, active session) and fold it
 * into the RAM viewport index. On a full/broken store it silently keeps the
 * last persisted window (no hardlock).
 *
 * Written STREAMING (field by field, small stack): this runs on the 8 KB
 * loop task from the keyboard path, and the old esc[1088]+line[1088] buffers
 * overran it (Stack canary / loopTask panic on Enter). */
static void agents_store_append(int idx, bool from_me, const char *text) {
    Conversation &a = g_convs[idx];
    uint32_t ts = agents_now_s();
    /* The picker's cache is updated for EVERY conversation; the scrollback
     * window only when this conversation is the one on screen. A line arriving
     * for a conversation nobody is reading must not disturb the open chat. */
    a.last.from_me = from_me;
    a.last.ts = ts;
    snprintf(a.last.text, sizeof(a.last.text), "%s", text ? text : "");
    a.last_use = ++g_conv_clock;
    if (g_win.owner == idx) agents_view_append(g_win, from_me, ts, text);
    if (!agents_store_ready()) return;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    /* Caller holds agents_mux (agents_push_line); bus lock second — order. */
    HwSpiBusGuard bus;
    File f = g_store->open(path.c_str(), "a");
    if (!f) return;
    uint32_t written = 0;
    {
        char pfx[96];
        int pn = snprintf(pfx, sizeof(pfx), "{\"ts\":%lu,\"from_me\":%s,\"text\":\"",
                          (unsigned long)ts, from_me ? "true" : "false");
        if (pn > 0 && (size_t)pn < sizeof(pfx))
            written += (uint32_t)f.write((const uint8_t *)pfx, (size_t)pn);
        char esc[64];
        const char *p = text;
        size_t ej = 0;
        while (p && *p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                if (ej + 2 > sizeof(esc)) { written += (uint32_t)f.write((const uint8_t *)esc, ej); ej = 0; }
                esc[ej++] = '\\'; esc[ej++] = (char)c;
            } else if (c < 0x20 || c == 0x7f) {
                if (ej + 6 > sizeof(esc)) { written += (uint32_t)f.write((const uint8_t *)esc, ej); ej = 0; }
                ej += (size_t)snprintf(esc + ej, sizeof(esc) - ej, "\\u%04x", c);
            } else {
                if (ej + 1 > sizeof(esc)) { written += (uint32_t)f.write((const uint8_t *)esc, ej); ej = 0; }
                esc[ej++] = (char)c;
            }
            p++;
        }
        if (ej > 0) written += (uint32_t)f.write((const uint8_t *)esc, ej);
        char sfx[4] = "\"}\n";
        written += (uint32_t)f.write((const uint8_t *)sfx, 3);
    }
    f.flush();
    f.close();
    if (written > 0) {
        a.lines++;
        a.session_lines[a.active_idx]++;
        if (g_win.owner == idx) g_win.file_sync += written;
    }
}

static void agents_view_append(ConvWindow &w, bool from_me, uint32_t ts,
                               const char *text) {
    if (w.vn >= AGENT_VIEW_MAX) {
        memmove(&w.view[0], &w.view[1], (AGENT_VIEW_MAX - 1) * sizeof(AgentLine));
        w.view[AGENT_VIEW_MAX - 1].from_me = from_me;
        w.view[AGENT_VIEW_MAX - 1].ts = ts;
        snprintf(w.view[AGENT_VIEW_MAX - 1].text, sizeof(w.view[0].text),
                 "%s", text ? text : "");
        w.win_start++;
    } else {
        w.view[w.vn].from_me = from_me;
        w.view[w.vn].ts = ts;
        snprintf(w.view[w.vn].text, sizeof(w.view[0].text), "%s", text ? text : "");
        w.vn++;
    }
}

/* Hand the window to `idx` and empty it, so the next sync rebuilds from byte 0.
 * The conversation's own line count is reset with it: the two are read back
 * together by agents_sync_view and must not disagree. */
static void agents_window_take(int idx) {
    g_win.owner = idx;
    g_win.file_sync = 0;
    g_win.vn = 0;
    g_win.win_start = 0;
    if (idx >= 0 && idx < CONV_MAX) g_convs[idx].lines = 0;
}

/* Buffered JSONL line scanner. Reads the file in 512-byte blocks instead of
 * one f.read() per byte — the long-history page load / delta-sync / tail read
 * paths run on the shared SPI bus and must not freeze the UI on long files. */
struct AgentJScanner {
    explicit AgentJScanner(File &file)
        : f(file), pos(0), len(0), total_read(0), eof(false) {}
    File f;
    uint8_t buf[512];
    size_t pos;      // next unconsumed byte in buf
    size_t len;      // valid bytes in buf (0 → refill)
    size_t total_read;  // bytes pulled from f so far (bus-hold budgeting)
    bool eof;

    /* One line into [out, n-1]; false at end of file. Strips trailing \r. */
    bool next(char *out, size_t n) {
        if (!out || n == 0) return false;
        size_t i = 0;
        while (true) {
            if (pos >= len) {
                if (eof) break;
                len = f.read(buf, sizeof(buf));
                pos = 0;
                total_read += len;
                if (len == 0) { eof = true; break; }
            }
            uint8_t c = buf[pos++];
            if (c == '\n') break;
            if (i + 1 >= n) {   // out buffer full mid-line → drain the rest
                while (true) {
                    if (pos >= len) {
                        if (eof) break;
                        len = f.read(buf, sizeof(buf));
                        pos = 0;
                        total_read += len;
                        if (len == 0) { eof = true; break; }
                    }
                    if (buf[pos++] == '\n') break;
                }
                break;
            }
            out[i++] = (char)c;
        }
        if (i == 0) return false;
        out[i] = '\0';
        while (i > 0 && out[i - 1] == '\r') out[--i] = '\0';
        return true;
    }
};

static bool agents_jsonl_parse(char *buf, bool &from_me, uint32_t &ts,
                               char *text, size_t text_n) {
    JsonDocument doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) return false;
    from_me = doc["from_me"] | false;
    ts = doc["ts"] | 0u;
    const char *t = doc["text"] | "";
    snprintf(text, text_n, "%s", t);
    return true;
}

/* Scan an already-open JSONL file line by line while BOUNDING the SPI-bus
 * hold: read at most AGENT_SCAN_BUS_CHUNK bytes under the bus lock, then
 * release it and yield (so the loop task can drain the SX1262 FIFO / repaint),
 * then re-take and continue. on_line(char*) is invoked per line with the bus
 * held; it must do only CPU work (parse), never bus I/O. The File is NOT
 * opened or closed here — the caller owns its lifetime.
 *
 * CONCURRENCY: the caller MUST hold agents_mux for the whole scan. That — and
 * only that — is what keeps the file immutable across the release windows:
 * every writer to a session file (agents_store_append via agents_push_line,
 * agents_clear's remove, the window rebuilds in agents_session_select /
 * _goto_tail / _goto_page / _sync_view) also takes agents_mux first, so while
 * we hold it no append, truncate, remove or rewrite can land between chunks.
 * The open File's cursor and size therefore stay valid; there is no
 * "file shrank / changed under us" case to detect MID-scan — it is prevented,
 * not merely handled. (agents_sync_view's size<file_sync full-rebuild guards
 * EXTERNAL truncation ACROSS calls — reboot, manual SD edit — not a concurrent
 * writer, which the mux excludes.) We release only the bus, never agents_mux:
 * releasing the mux would let agents_clear remove the file under our open
 * handle, and is impossible anyway for the callers that hold it recursively.
 * LOCK ORDER stays intact — we never take agents_mux while holding the bus. */
template <typename LineFn>
static void agents_scan_chunked(AgentJScanner &sc, char *lbuf, size_t lbuf_n,
                                LineFn on_line) {
    for (;;) {
        bool more = false;
        {
            HwSpiBusGuard bus;  /* one bounded chunk = one bus burst */
            size_t chunk_start = sc.total_read;
            while ((more = sc.next(lbuf, lbuf_n)) == true) {
                on_line(lbuf);
                if (sc.total_read - chunk_start >= AGENT_SCAN_BUS_CHUNK) break;
            }
        }  /* bus released here */
        if (!more) break;   // scanner reached EOF inside this chunk
        taskYIELD();        // let the loop task grab the freed bus
    }
}

/* Bring the RAM index in line with the JSONL file: read only the bytes after
 * file_sync (delta), append them to the window. On external truncation/resync
 * the window is rebuilt from zero. Caller holds the lock. */
static void agents_sync_view(int idx) {
    Conversation &a = g_convs[idx];
    if (!agents_store_ready()) return;
    /* Syncing a conversation the window is not loaded for would fold its lines
     * into someone else's scrollback, so the window is taken first. Taking it
     * empties the window, which turns the delta-sync below into a full rebuild
     * — the same work the old per-conversation code did on every room switch. */
    if (g_win.owner != idx) agents_window_take(idx);
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    /* Open + size/seek under one short bus burst; the line scan below bounds
     * its own bus hold (mux held throughout — see agents_scan_chunked). */
    File f;
    uint32_t final_size = 0;
    bool do_scan = false;
    {
        HwSpiBusGuard bus;
        if (!g_store->exists(path.c_str())) {
            agents_window_take(idx);
            return;
        }
        f = g_store->open(path.c_str(), "r");
        if (!f) { agents_window_take(idx); return; }
        size_t sz = f.size();
        if (sz < g_win.file_sync)    // file shrank between calls → full rebuild
            agents_window_take(idx);
        final_size = (uint32_t)sz;
        do_scan = (g_win.file_sync < sz && f.seek(g_win.file_sync));
    }
    if (do_scan) {
        char lbuf[AGENT_JSONL_MAX];
        char text[AGENT_TEXT_LEN];
        AgentJScanner sc(f);
        agents_scan_chunked(sc, lbuf, sizeof(lbuf), [&](char *line) {
            bool from_me;
            uint32_t ts;
            if (line[0] && agents_jsonl_parse(line, from_me, ts, text, sizeof(text))) {
                agents_view_append(g_win, from_me, ts, text);
                a.lines++;
                /* The newest line read is also the picker's cached line. */
                a.last.from_me = from_me;
                a.last.ts = ts;
                snprintf(a.last.text, sizeof(a.last.text), "%s", text);
            }
        });
    }
    g_win.file_sync = final_size;
    a.session_lines[a.active_idx] = a.lines;
    { HwSpiBusGuard bus; f.close(); }
}

/*
 * Find a conversation by id, or MINT one.
 *
 * This is what C1's fixed table could not do and what a transport needs the
 * moment a stranger's first message arrives: a peer has no slot until it
 * speaks. The id is the caller's (for a peer, the first hex of its address);
 * label, transport and return address come from the wire that met it.
 *
 * WHEN THE TABLE IS FULL, the least-recently-used NON-SEEDED conversation is
 * evicted. Seeded conversations are the device's own doors — the user must
 * always be able to reach them — so they are never candidates, and a table of
 * nothing but seeded slots refuses (-1) instead of throwing one out.
 *
 * EVICTION IS NOT DELETION, which is what makes it safe: the slot is a RAM
 * cache of a conversation whose history lives in its own /conv.<id> file, and
 * that file is never touched here. If the evicted peer speaks again it is
 * re-minted onto the same id, reopens the same log, and its history is still
 * there — exactly the MARK-NOT-DELETE rule the dead-room roster already
 * follows. Losing the slot costs a rebuild, not a conversation.
 *
 * Caller holds agents_mux. Returns the slot index, or -1.
 */
static int conv_mint(const char *id, const char *label, uint8_t transport,
                     const uint8_t *addr, uint8_t addr_len) {
    if (!id || !id[0]) return -1;
    int idx = agents_find(id);
    if (idx >= 0) return idx;                    /* find, before mint */

    uint8_t seeded_of[CONV_MAX];
    uint32_t use_of[CONV_MAX];
    for (int i = 0; i < g_conv_n; i++) {
        seeded_of[i] = g_convs[i].seeded;
        use_of[i] = g_convs[i].last_use;
    }
    /* g_win.owner IS the conversation on screen — window ownership changes only
     * on an explicit focus — so it is the slot the reader would be swapped out
     * of, and it is protected. (Between boot and the first draw it is whichever
     * conversation the init loop synced last, which is harmless: nothing can
     * mint that early, and protecting the wrong slot only costs a different
     * victim.) */
    idx = conv_slot_plan(g_conv_n, CONV_MAX, seeded_of, use_of, g_win.owner);
    if (idx < 0) return -1;                      /* nothing may be evicted */
    if (idx >= g_conv_n) {
        g_conv_n = (uint8_t)(idx + 1);           /* took a free slot */
    } else {
        event_add("conv evict %s for %s", g_convs[idx].id, id);
        if (g_win.owner == idx) agents_window_take(-1);
    }

    Conversation &a = g_convs[idx];
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "%s", id);
    snprintf(a.label, sizeof(a.label), "%s", (label && label[0]) ? label : id);
    a.transport = transport;
    if (addr && addr_len) {
        a.reply_len = (addr_len > CONV_REPLY_MAX) ? CONV_REPLY_MAX : addr_len;
        memcpy(a.reply_addr, addr, a.reply_len);
    }
    a.seeded = 0;
    a.last_use = ++g_conv_clock;
    return idx;
}

/* Apply the conversation-level half of a manifest line: the fields that belong
 * to the conversation rather than to one of its rooms. Repeating across a
 * conversation's rooms is intentional and idempotent. */
static void agents_conv_apply(Conversation &a, const ConvManifestLine &ml) {
    if (ml.label[0]) snprintf(a.label, sizeof(a.label), "%s", ml.label);
    /* The ROUTE is resolved, not assigned: a seeded conversation keeps the
     * transport and return address the firmware compiled in, whatever the card
     * says. /conversations.txt is user-editable and transport now chooses the
     * wire a reply leaves on — see conv_route_resolve() in ../transport.h. */
    conv_route_resolve(a.seeded != 0,
                       a.transport, a.reply_addr, a.reply_len,
                       ml.transport, ml.reply, ml.reply_len,
                       &a.transport, a.reply_addr, &a.reply_len);
}

/* Fold one parsed manifest line into the live registry. A line naming an
 * unknown conversation is skipped rather than minting one: C1 has a fixed
 * seeded table, and a room invented from a stale id would be a room nothing can
 * ever answer.
 *
 * A ROOM-LESS LINE IS A REAL LINE, not a malformed one. A conversation that has
 * no rooms — the shape a peer gets, keyed on its address alone — still has to
 * survive a reboot, so its line carries an empty session field and restores the
 * conversation's own fields without adding a room. Dropping such a line (which
 * this used to do) meant the store could describe a peer conversation on disk
 * and then silently forget it on the next boot. */
static void agents_manifest_apply(const ConvManifestLine &ml) {
    /* THE LOADER CREATES NOTHING. It resolves against the conversations this
     * build already has, and a line naming any other is skipped.
     *
     * WHY, given the store can now mint: /conversations.txt lives on a card a
     * user can pull and edit, and a minted conversation is not seeded — so
     * conv_route_resolve() would take its transport and return address from
     * that file verbatim. Minting here would therefore let a hand-written line
     * conjure a conversation whose replies go wherever the line says, which is
     * exactly the re-pointing the seeded-route rule was written to stop, just
     * through a different door. The legacy manifest makes it concrete: it can
     * still name retired agent ids, and minting those would resurrect rooms
     * that post to the bridge.
     *
     * A non-AGENT line is skipped for the same reason and one more: no peer
     * conversation is created anywhere in this build, so a peer line on disk
     * can only have been written by hand. conv_mint() and the room-less
     * manifest line are built and tested for the receiver that will need them;
     * loading a route from the card is a trust decision that belongs with the
     * transport that can actually verify a peer. */
    if (ml.transport != CONV_AGENT) return;
    int idx = agents_find(ml.conv);
    if (idx < 0) return;
    Conversation &a = g_convs[idx];
    agents_conv_apply(a, ml);
    if (!ml.session[0]) return;         /* conversation-level line: no room */
    int si = agents_session_add(idx, ml.session);
    if (si < 0) return;
    a.session_dead[si] = ml.dead;
}

/* Read one manifest file through the shared parser. Both the new 5-field lines
 * and the legacy 2/3-field ones are handled there, so this is format-blind. */
static void agents_manifest_read(const char *path) {
    if (!g_store || !g_store->exists(path)) return;
    File f = g_store->open(path, "r");
    if (!f) return;
    char ln[CONV_MANIFEST_LINE_LEN];
    ConvManifestLine ml;
    AgentJScanner sc(f);
    while (sc.next(ln, sizeof(ln))) {
        if (conv_manifest_parse(ln, &ml)) agents_manifest_apply(ml);
    }
    f.close();
}

/* Load the room registry.
 *
 * MIGRATION: READ BOTH, WRITE ONE. The legacy manifest is read first and the
 * new one on top, so a card that has both ends up with the new file's flags
 * winning; every persist after that writes only /conversations.txt. This beats
 * a rewrite-in-place migration because there is no half-migrated state to
 * recover from — the legacy file is never edited or removed (deleting a user's
 * data is not this code's call), it simply stops being authoritative once the
 * new one exists. */
static void agents_manifest_load() {
    HwSpiBusGuard bus;
    agents_manifest_read(CONV_MANIFEST_LEGACY);
    agents_manifest_read(CONV_MANIFEST);
}

static void agents_manifest_persist() {
    if (!agents_store_ready()) return;
    String out;
    char line[CONV_MANIFEST_LINE_LEN];
    for (int i = 0; i < g_conv_n; i++) {
        Conversation &a = g_convs[i];
        if (a.n_sessions == 0) {
            /* A conversation with no rooms still needs a line, or it vanishes
             * on the next boot — see agents_manifest_apply. The session field
             * is empty and the dead flag is 0: liveness is a property of a
             * room, and this conversation has none to mark. */
            if (conv_manifest_format(line, sizeof(line), a.id, "", a.label,
                                     a.transport, a.reply_addr, a.reply_len, 0)) {
                out += line;
                out += '\n';
            }
            continue;
        }
        for (int j = 0; j < a.n_sessions; j++) {
            if (!conv_manifest_format(line, sizeof(line), a.id, a.sessions[j],
                                      a.label, a.transport, a.reply_addr,
                                      a.reply_len, a.session_dead[j]))
                continue;
            out += line;
            out += '\n';
        }
    }
    HwSpiBusGuard bus;  /* write burst only — manifest text built above */
    File f = g_store->open(CONV_MANIFEST, "w");
    if (!f) return;
    f.print(out);
    f.close();
}

/* ---- agents --------------------------------------------------------------- */

static int agents_find(const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < g_conv_n; i++) {
        if (strcmp(g_convs[i].id, id) == 0) return i;
    }
    return -1;
}

static int agents_session_exists(int idx, const char *name) {
    Conversation &a = g_convs[idx];
    for (int i = 0; i < a.n_sessions; i++)
        if (strcmp(a.sessions[i], name) == 0) return i;
    return -1;
}

static int agents_session_add(int idx, const char *name) {
    Conversation &a = g_convs[idx];
    if (!name || !name[0]) return -1;
    int ex = agents_session_exists(idx, name);
    if (ex >= 0) return ex;
    if (a.n_sessions >= AGENT_SESSIONS_MAX) return -1;
    snprintf(a.sessions[a.n_sessions], AGENT_SESSION_LEN, "%s", name);
    a.n_sessions++;
    return a.n_sessions - 1;
}

const char *agents_active_session(int idx) {
    if (idx < 0 || idx >= g_conv_n) return "";
    return g_convs[idx].sessions[g_convs[idx].active_idx];
}

int agents_session_count(int idx) {
    if (idx < 0 || idx >= g_conv_n) return 0;
    return g_convs[idx].n_sessions;
}

const char *agents_session_name(int idx, int i) {
    if (idx < 0 || idx >= g_conv_n || i < 0 || i >= g_convs[idx].n_sessions)
        return "";
    return g_convs[idx].sessions[i];
}

bool agents_session_is_active(int idx, int i) {
    if (idx < 0 || idx >= g_conv_n || i < 0 || i >= g_convs[idx].n_sessions)
        return false;
    return (i == g_convs[idx].active_idx);
}

int agents_session_msg_count(int idx, int i) {
    if (idx < 0 || idx >= g_conv_n || i < 0 || i >= g_convs[idx].n_sessions)
        return 0;
    return (int)g_convs[idx].session_lines[i];
}

/* Liveness of session i (live-room roster). true => dead/hidden from picker. */
bool agents_session_is_dead(int idx, int i) {
    if (idx < 0 || idx >= g_conv_n || i < 0 || i >= g_convs[idx].n_sessions)
        return false;
    return g_convs[idx].session_dead[i] != 0;
}

/* Count of live (non-dead) sessions — the number of rows the picker shows. */
int agents_session_visible_count(int idx) {
    if (idx < 0 || idx >= g_conv_n) return 0;
    Conversation &a = g_convs[idx];
    int c = 0;
    for (int i = 0; i < a.n_sessions; i++) if (!a.session_dead[i]) c++;
    return c;
}

/* Recount every session's persisted lines for the agent (cache the counts so
 * the session-list screen is honest about message numbers without holding the
 * full history). Opens ≤ AGENT_SESSIONS_MAX files; runs only on screen open.
 * The active session's window index is not clobbered: if its file count moved
 * away from the RAM index, re-sync (delta or full) so win_start/lines stay
 * consistent with view[]. */
bool agents_session_refresh_counts(int idx) {
    if (idx < 0 || idx >= g_conv_n || !agents_store_ready()) return false;
    agents_lock();
    Conversation &a = g_convs[idx];
    for (int j = 0; j < a.n_sessions; j++) {
        String path = agents_log_path(a.id, a.sessions[j]);
        uint32_t cnt = 0;
        File f;
        { HwSpiBusGuard bus; f = g_store->open(path.c_str(), "r"); }
        if (f) {
            /* Count newlines in bounded bus chunks so a large session file
             * never freezes the radio here. mux is held across the whole
             * refresh, so the file cannot change between chunks; the bus is
             * released each chunk (non-recursive: also freed before the
             * agents_sync_view re-anchor below). */
            uint8_t chunk[256];
            for (;;) {
                bool eof = false;
                {
                    HwSpiBusGuard bus;
                    size_t chunk_read = 0;
                    size_t r = 0;
                    while ((r = f.read(chunk, sizeof(chunk))) > 0) {
                        for (size_t k = 0; k < r; k++)
                            if (chunk[k] == '\n') cnt++;
                        chunk_read += r;
                        if (chunk_read >= AGENT_SCAN_BUS_CHUNK) break;
                    }
                    if (r == 0) eof = true;
                }
                if (eof) break;
                taskYIELD();
            }
            { HwSpiBusGuard bus; f.close(); }
        }
        a.session_lines[j] = cnt;
        if (j == a.active_idx && a.lines != cnt) {
            /* Re-anchor the window to the file only when this conversation IS
             * the window — otherwise taking it would blank the open chat, and
             * an unfocused conversation needs nothing but an honest count. */
            if (g_win.owner == idx) agents_sync_view(idx);
            else                    a.lines = cnt;
        }
    }
    agents_unlock();
    return true;
}

/* Select an existing session. Reloads the scrollback from SD when this
 * conversation is the one on screen; for any other conversation only the active
 * room and its count move, because reloading would hand the window away from
 * the chat the user is actually reading. The off-loop route drain selects rooms
 * on conversations nobody is looking at, which is exactly that case. */
bool agents_session_select(int idx, const char *name) {
    if (idx < 0 || idx >= g_conv_n || !name || !name[0]) return false;
    agents_lock();
    int in_list = agents_session_exists(idx, name);
    if (in_list < 0) { agents_unlock(); return false; }
    Conversation &a = g_convs[idx];
    a.active_idx = (uint8_t)in_list;
    a.last_use = ++g_conv_clock;
    if (g_win.owner == idx) {
        agents_window_take(idx);            // empty it: a different room now
        agents_sync_view(idx);
        g_win.win_start = (a.lines > g_win.vn) ? (a.lines - g_win.vn) : 0;
    } else {
        a.lines = a.session_lines[in_list];
    }
    agents_unlock();
    return true;
}

/* Create a fresh session (or reuse an existing name) and make it active.
 * Returns the index in the session list, or -1 on full/empty. */
int agents_session_create(int idx, const char *wanted, bool &created) {
    if (idx < 0 || idx >= g_conv_n) return -1;
    agents_lock();
    Conversation &a = g_convs[idx];
    char name[AGENT_SESSION_LEN];
    if (wanted && wanted[0]) {
        agents_session_sanitize(wanted, name, sizeof(name));
        if (!name[0]) { agents_unlock(); return -1; }
    } else {
        unsigned long s = agents_now_s();
        for (int k = 0;; k++) {
            if (k == 0) snprintf(name, sizeof(name), "s%lu", s);
            else         snprintf(name, sizeof(name), "s%lu%c", s, (char)('a' + k - 1));
            if (agents_session_exists(idx, name) < 0) break;
        }
    }
    int in_list = agents_session_exists(idx, name);
    created = (in_list < 0);
    if (created) {
        if (a.n_sessions >= AGENT_SESSIONS_MAX) { agents_unlock(); return -1; }
        in_list = agents_session_add(idx, name);
    }
    a.active_idx = (uint8_t)in_list;
    a.last_use = ++g_conv_clock;
    a.lines = 0;
    if (g_win.owner == idx) agents_window_take(idx);
    if (created) agents_manifest_persist();
    event_add("agent %s session %s %s", a.id,
              created ? "new" : "select", name);
    agents_unlock();
    return in_list;
}

/* ---- viewport / long-scroll ----------------------------------------------- */

/* Build window = the last AGENT_VIEW_MAX lines of the active session ending at
 * end_line (exclusive), by one forward scan (keeps a sliding window). Used for
 * scrolling back through very long history. Caller holds no lock (internal). */
static void agents_load_page(int idx, uint32_t end_line) {
    Conversation &a = g_convs[idx];
    if (!agents_store_ready()) return;
    g_win.owner = idx;                  /* paging is an explicit focus */
    if (end_line > a.lines) end_line = a.lines;
    uint32_t keep_from = (end_line > AGENT_VIEW_MAX) ? (end_line - AGENT_VIEW_MAX) : 0;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    /* Page scan runs from byte 0; bound its bus hold in chunks (mux held). */
    File f;
    { HwSpiBusGuard bus; f = g_store->open(path.c_str(), "r"); }
    if (!f) { g_win.vn = 0; g_win.win_start = 0; return; }
    char lbuf[AGENT_JSONL_MAX];
    char text[AGENT_TEXT_LEN];
    uint32_t line_idx = 0;
    g_win.vn = 0;
    g_win.win_start = keep_from;
    AgentJScanner sc(f);
    agents_scan_chunked(sc, lbuf, sizeof(lbuf), [&](char *line) {
        bool from_me;
        uint32_t ts;
        if (line[0] && line_idx >= keep_from && line_idx < end_line &&
            agents_jsonl_parse(line, from_me, ts, text, sizeof(text))) {
            agents_view_append(g_win, from_me, ts, text);
        }
        line_idx++;
    });
    { HwSpiBusGuard bus; f.close(); }
}

/* Re-pin the window to the newest messages of the active session. If the user
 * was browsing an old page the window is stale, so force a full rebuild; if it
 * is already the tail, a cheap delta-sync folds any new arrivals. */
void agents_thread_goto_tail(int idx) {
    if (idx < 0 || idx >= g_conv_n) return;
    agents_lock();
    Conversation &a = g_convs[idx];
    /* This is THE focus point: opening a chat calls it, so it is where the
     * window changes hands. A window loaded for someone else is stale by
     * definition and rebuilds; so does a window parked on an older page. */
    bool at_tail = (g_win.owner == idx) &&
                   (((uint32_t)g_win.win_start + g_win.vn) >= a.lines);
    if (!at_tail) agents_window_take(idx);
    a.last_use = ++g_conv_clock;
    agents_sync_view(idx);
    g_win.win_start = (a.lines > g_win.vn) ? (a.lines - g_win.vn) : 0;
    agents_unlock();
}

/* Load an older page: the window becomes the last AGENT_VIEW_MAX lines ending
 * at end_line (usually the current window's win_start) so wheel-up browsing
 * walks a very long history. If end_line ≥ total, falls back to the tail.
 * Page-load rescans the file from byte 0 (O(up-to-end)); realistic session
 * sizes are fast, and the newest page is always O(delta). */
void agents_thread_goto_page(int idx, uint32_t end_line) {
    if (idx < 0 || idx >= g_conv_n) return;
    agents_lock();
    Conversation &a = g_convs[idx];
    if (end_line >= a.lines) {
        g_win.owner = idx; g_win.vn = 0; g_win.win_start = a.lines;
    } else {
        agents_load_page(idx, end_line);
    }
    agents_unlock();
}

uint32_t agents_thread_total(int idx) {
    return (idx >= 0 && idx < g_conv_n) ? g_convs[idx].lines : 0;
}

/* The three below describe the WINDOW, so they answer for the conversation the
 * window is loaded for. Asked about any other conversation they report the
 * harmless "nothing scrolled, at the tail" state rather than another
 * conversation's scroll position. */
uint32_t agents_thread_start(int idx) {
    return (idx >= 0 && idx < g_conv_n && g_win.owner == idx) ? g_win.win_start : 0;
}

bool agents_thread_is_tail(int idx) {
    if (idx < 0 || idx >= g_conv_n || g_win.owner != idx) return true;
    return ((uint32_t)g_win.win_start + g_win.vn) >= g_convs[idx].lines;
}

uint32_t agents_thread_line_ts(int idx, int i) {
    if (idx < 0 || idx >= g_conv_n || g_win.owner != idx) return 0;
    return (i >= 0 && i < g_win.vn) ? g_win.view[i].ts : 0;
}

/* Read the very last message of the active session straight from the store
 * (used by GET /agents when the RAM window is a rewound page, not the tail). */
static bool agents_file_last(int idx, char *text, size_t text_n,
                             bool *from_me, uint32_t *ts) {
    Conversation &a = g_convs[idx];
    if (!agents_store_ready()) return false;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    /* GET /agents runs on the AsyncTCP task; scan the whole file for its last
     * line in bounded bus chunks so it cannot freeze the radio (mux held). */
    File f;
    { HwSpiBusGuard bus; f = g_store->open(path.c_str(), "r"); }
    if (!f) return false;
    char lbuf[AGENT_JSONL_MAX];
    char t2[AGENT_TEXT_LEN];
    bool fm = false;
    uint32_t t = 0;
    bool found = false;
    AgentJScanner sc(f);
    agents_scan_chunked(sc, lbuf, sizeof(lbuf), [&](char *line) {
        if (line[0] && agents_jsonl_parse(line, fm, t, t2, sizeof(t2))) found = true;
    });
    { HwSpiBusGuard bus; f.close(); }
    if (!found) return false;
    if (text) snprintf(text, text_n, "%s", t2);
    if (from_me) *from_me = fm;
    if (ts) *ts = t;
    return true;
}

/* UTF-8 lead length (1..4). Invalid lead → 1 so we never stall. */
static int agents_utf8_len(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Push arbitrary-length text: split on word boundaries into <AGENT_TEXT_LEN
 * chunks, store each as one JSONL line. Never cuts mid-UTF-8. Thread-safe
 * (locks itself; callers that already hold the recursive lock are fine). */
static void agents_push_line(int idx, bool from_me, const char *text) {
    agents_lock();
    if (idx < 0 || idx >= g_conv_n || !text || !text[0]) {
        agents_unlock();
        return;
    }

    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        int cap = AGENT_TEXT_LEN - 1;
        int n = 0;
        int last_sp = -1;
        while (p[n] && n < cap) {
            int k = agents_utf8_len(p + n);
            if (k <= 0) break;
            if (n + k > cap) break;
            if (k == 1 && p[n] == ' ') last_sp = n;
            n += k;
        }
        if (p[n] && last_sp > 8) n = last_sp;
        if (n <= 0) {
            n = agents_utf8_len(p);
            if (n <= 0) n = 1;
            if (n > cap) n = cap;
        }

        char chunk[AGENT_TEXT_LEN];
        if (n >= (int)sizeof(chunk)) n = (int)sizeof(chunk) - 1;
        memcpy(chunk, p, (size_t)n);
        chunk[n] = '\0';
        while (n > 0 && chunk[n - 1] == ' ') chunk[--n] = '\0';
        if (n > 0) agents_store_append(idx, from_me, chunk);
        p += n;
    }
    agents_unlock();
}

/* Chronological view (oldest first) for the scrollable chat. Synced from SD
 * first. Returns count. Caller may hold the lock (recursive + sync locks). */
/* Copy the current window (oldest→newest) for the chat renderer. goto_tail /
 * goto_page own the sync, so this is normally a pure RAM copy. ts_out[i] is the
 * message unix ts (0 = unknown). Returns count. Caller may hold the lock.
 *
 * The one disk case is a SAFETY NET: if the renderer asks for a conversation
 * the window is not loaded for, it is loaded here rather than drawing an empty
 * chat. Opening a chat calls agents_thread_goto_tail first, so this costs
 * nothing in the normal path — and when it does fire it is the same scan that
 * goto_tail would have run, on the same task. A blank thread is not an
 * acceptable alternative. */
int agents_thread_view(int idx, char out[][AGENT_TEXT_LEN], bool *from_me,
                       uint32_t *ts_out, int max_lines) {
    if (idx < 0 || idx >= g_conv_n || max_lines <= 0) return 0;
    int n = 0;
    agents_lock();
    if (g_win.owner != idx) agents_thread_goto_tail(idx);
    n = g_win.vn < max_lines ? g_win.vn : max_lines;
    for (int i = 0; i < n; i++) {
        snprintf(out[i], AGENT_TEXT_LEN, "%s", g_win.view[i].text);
        if (from_me) from_me[i] = g_win.view[i].from_me;
        if (ts_out)  ts_out[i] = g_win.view[i].ts;
    }
    agents_unlock();
    return n;
}

static int agents_thread_count(int idx) {
    if (idx < 0 || idx >= g_conv_n) return 0;
    return g_convs[idx].lines;
}

static void agents_bridge_load() {
    g_bridge[0] = '\0';
    if (!SPIFFS.exists(AGENT_BRIDGE_FILE)) return;
    File f = SPIFFS.open(AGENT_BRIDGE_FILE, "r");
    if (!f) return;
    String s = f.readString();
    f.close();
    s.trim();
    if (s.length() == 0 || s.length() >= AGENT_BRIDGE_LEN) return;
    if (!s.startsWith("http://")) return;
    snprintf(g_bridge, sizeof(g_bridge), "%s", s.c_str());
}

static bool agents_bridge_save(const char *url) {
    if (!url || !url[0]) {
        SPIFFS.remove(AGENT_BRIDGE_FILE);
        g_bridge[0] = '\0';
        return true;
    }
    if (strncmp(url, "http://", 7) != 0) return false;
    if (strlen(url) >= AGENT_BRIDGE_LEN) return false;
    /* Atomic (C8): a power cut mid-write must never leave an empty bridge
     * file — agents_bridge_load treats it as "no bridge configured". */
    if (!write_spiffs_file_atomic(AGENT_BRIDGE_FILE, AGENT_BRIDGE_TMP,
                                  String(url)))
        return false;
    snprintf(g_bridge, sizeof(g_bridge), "%s", url);
    return true;
}

/* Best-effort POST to the bridge. Runs on the loop/web task — short timeout.
   Returns true if the bridge accepted the message (2xx). */
static bool agents_bridge_post(const char *agent_id, const char *session,
                               const char *text) {
    if (!g_bridge[0] || !agent_id || !session || !text || !text[0]) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[AGENT_BRIDGE_LEN + 16];
    size_t bl = strlen(g_bridge);
    while (bl > 0 && g_bridge[bl - 1] == '/') bl--;
    snprintf(url, sizeof(url), "%.*s/v1/chat", (int)bl, g_bridge);

    JsonDocument doc;
    doc["agent"] = agent_id;
    doc["session"] = session;
    doc["text"] = text;
    doc["from"] = "tlora-pager";
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.setTimeout(4000);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (auth_token.length() > 0) {
        http.addHeader("Authorization", String("Bearer ") + auth_token);
    }
    int code = http.POST(body);
    http.end();
    return code >= 200 && code < 300;
}

/* Keep printable ASCII + valid UTF-8 (Cyrillic). Tabs/newlines → space. */
static void agents_clean_text(const char *in, char *out, size_t out_n) {
    if (!out || out_n == 0) return;
    size_t j = 0;
    if (in) {
        for (size_t i = 0; in[i] && j + 1 < out_n; ) {
            unsigned char c = (unsigned char)in[i];
            if (c == '\n' || c == '\r' || c == '\t') {
                if (j == 0 || out[j - 1] != ' ') out[j++] = ' ';
                i++;
                continue;
            }
            if (c < 0x20) { i++; continue; }
            if (c < 0x80) {
                out[j++] = (char)c;
                i++;
                continue;
            }
            int need = 0;
            if ((c & 0xE0) == 0xC0) need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            else { i++; continue; }
            bool ok = true;
            for (int k = 1; k < need; k++) {
                if (((unsigned char)in[i + k] & 0xC0) != 0x80) { ok = false; break; }
            }
            if (!ok) { i++; continue; }
            if (j + (size_t)need >= out_n) break;
            for (int k = 0; k < need; k++) out[j++] = in[i + k];
            i += (size_t)need;
        }
    }
    out[j] = '\0';
}

/* Optional mesh uplink (registered by meshcore skill). Same C1 framing as downlink. */
typedef bool (*AgentsMeshUplinkFn)(const char *agent_id, const char *text);
static AgentsMeshUplinkFn g_agents_mesh_uplink = nullptr;

static void agents_set_mesh_uplink(AgentsMeshUplinkFn fn) {
    g_agents_mesh_uplink = fn;
}

/* ---- GPS location door-card ("where are you?" interception) ---------------
 * Intercepted in agents_send() — NOT in inbound — so an agent that asks "где
 * ты?" gets the pager's real position in its own thread and the query never
 * hits the bridge. Fresh fix (< AGENT_GPS_FRESH_S) answers immediately; no
 * fresh fix answers with a "no fix yet" card and arms g_agents_gps_pending,
 * which gps.cpp's on-fix hook (registered in skill_agents_init) resolves the
 * moment the first RMC 'A' lands. Non-blocking throughout: every path here
 * returns immediately and the late answer arrives via the hook. */

#define AGENT_GPS_FRESH_S 60    /* mirror gps.cpp GPS_FRESH_S */

static bool g_agents_gps_pending[CONV_MAX];

/* Case-insensitive substring (lowercase only ASCII; needles are ASCII/Cyrillic). */
static bool agents_ci_has(const char *hay, const char *needle) {
    char lower[AGENT_TEXT_LEN];
    size_t n = strlen(hay ? hay : "");
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++)
        lower[i] = (hay[i] >= 'A' && hay[i] <= 'Z') ? (char)(hay[i] + 32) : hay[i];
    lower[n] = '\0';
    return strstr(lower, needle) != NULL;
}

static bool agents_wants_gps(const char *text) {
    if (!text || !text[0]) return false;
    if (strncmp(text, "/gps", 4) == 0) return true;
    static const char *kws[] = { "where are you", "where are u", "where r u",
                                 "где ты", "где я" };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (agents_ci_has(text, kws[i])) return true;
    return false;
}

/* Post the reply line into the agent's thread, like an inbound agent message.
 * Returns true when the position was fresh, false when it armed the pending. */
static bool agents_loc_card(int idx) {
    double lat = 0, lon = 0;
    uint32_t ts = 0;
    bool fix = false;
    int sats = 0;
    gps_get_position(&lat, &lon, &ts, &fix, &sats);
    long age = gps_fix_age_s();
    float hdop = gps_get_hdop();
    bool fresh = fix && age >= 0 && age < AGENT_GPS_FRESH_S;
    char line[AGENT_TEXT_LEN];
    if (fresh) {
        if (hdop >= 0)
            snprintf(line, sizeof(line), "GPS: %.5f, %.5f | sats %d hdop %.1f | age %lds",
                     lat, lon, sats, (double)hdop, age);
        else
            snprintf(line, sizeof(line), "GPS: %.5f, %.5f | sats %d | age %lds",
                     lat, lon, sats, age);
    } else {
        snprintf(line, sizeof(line),
                 "GPS: no fix yet - I will reply as soon as the first fix lands");
    }
    /* real_inbound: the fix can land minutes after the question — from the
     * user's side this is an answer arriving in the room, not an error line. */
    agents_on_inbound(g_convs[idx].id, line, true);
    event_add("agent %s gps %s", g_convs[idx].id, fresh ? "located" : "pending");
    return fresh;
}

/* Match in agents_send. Returns true when the message was consumed locally. */
static bool agents_gps_intercept(int idx, const char *text) {
    if (!agents_wants_gps(text)) return false;
    agents_push_line(idx, true, text);        /* keep the query in the thread */
    bool fresh = agents_loc_card(idx);
    g_agents_gps_pending[idx] = !fresh;
    if (!fresh) gps_request_fix();
    return true;
}

/* gps.cpp on-fix hook: resolve every pending "where are you?" agent. Cheap
 * no-op when nothing is pending (runs once per RMC 'A', ~1/s). */
static void agents_gps_on_fix(void) {
    for (int i = 0; i < g_conv_n; i++) {
        if (g_agents_gps_pending[i]) {
            g_agents_gps_pending[i] = false;
            agents_loc_card(i);
        }
    }
}

/* Public (used by POST /gps/fix in gps.cpp): arm a reply for the agent on the
 * next fix. */
void agents_gps_pending(const char *agent_id) {
    int idx = agents_find(agent_id);
    if (idx >= 0) {
        g_agents_gps_pending[idx] = true;
        gps_request_fix();
    }
}

/* Try to put this line on the wire as a Reticulum envelope. `claude` only.
 *
 * WHY `claude` AND NOT EVERY AGENT. The room this closes the loop for is the
 * one whose INBOUND half already works: claude_route_incoming() above lands a
 * peer's envelope in a `claude` room by session name, and this is the reply
 * path for exactly that. `hermes`, the only other agent left in the registry,
 * has its own chat door and no inbound RNS half at all; giving it an RNS uplink
 * would be a second, ambiguous consumer answering into a room the peer never
 * routes back to.
 *
 * THE SESSION FIELD IS THE ROOM'S OWN NAME, which is the whole reason this is
 * not just "send some text": the peer routes its answer back by that name, so a
 * message typed in room `claude-pager-channel` is answered into
 * `claude-pager-channel` and not into whichever room happened to be newest.
 * Room names are already sanitised to [A-Za-z0-9._-] within 23 usable bytes,
 * which is exactly what the envelope accepts; a name that somehow is not comes
 * back as "bad session name" rather than going out mislabelled.
 *
 * NOTHING HERE BLOCKS AND NOTHING HERE TOUCHES THE STACK. rns_send_envelope()
 * validates, builds and enqueues under the producer spinlock and returns; the
 * loop task resolves the path and encrypts. That is what makes it safe to call
 * from here, which is the keyboard path on the loop task in one caller
 * (ui_reply_submit) and the AsyncTCP task in the other (POST /agents/send).
 *
 * Returns true when the message is QUEUED, not when it is delivered — the
 * outcome is in GET /rns/status. On false, *why is a short static literal
 * naming the one thing that is wrong. */
static bool agents_rns_uplink(const char *agent_id, const char *session,
                              const char *text, const char **why) {
    char peer[RNS_OUTBOX_ADDR_HEX + 1];
    *why = nullptr;
    if (strcmp(agent_id, "claude") != 0) return false;
    /* The failure the user can actually fix, so it is named on its own rather
     * than folded into whatever rns_send_envelope() would say about an empty
     * destination. */
    if (!rns_peer_addr(peer, sizeof(peer))) {
        *why = "no peer address";
        return false;
    }
    /* ASKED HERE AND NOT INSIDE THE SEND, because a room is the one caller that
     * has somewhere else to go. The ring's retry ladder is built to hold a
     * message while a path resolves, so POST /rns/send is right to accept one
     * over a link that is momentarily down; a room would rather fall back to
     * the bridge and say which path it used. */
    if (!rns_link_up()) {
        *why = "link down";
        return false;
    }
    return rns_send_envelope(peer, session, text, why);
}

/* ---- transport backends --------------------------------------------------- *
 *
 * One backend per wire, each reached only through transport_send() below. The
 * ladder that used to sit inline in agents_send() lives in the CONV_AGENT
 * backend now, unchanged — which is the point: agents_send() no longer knows
 * that `claude` has a Reticulum uplink and `hermes` does not. That is a
 * property of the agent wire, and when the mesh messenger generalises it, only
 * this backend changes. */

/* CONV_AGENT: the existing ladder, byte for byte.
 *
 * The `claude` tests inside it are NOT dispatch — dispatch is done by the time
 * we get here. They are this backend's internal routing, and both are load
 * bearing: the LXMF origin table is keyed by ROOM NAME ALONE (lxmf_reply.h), so
 * dropping the check would let a `hermes` room that merely shares a name with a
 * `claude` room inherit its LXMF sender; and the Reticulum peer is a single
 * configured address, so offering it to every agent would put `hermes` traffic
 * on a link the user configured for something else. */
static bool transport_send_agent(int idx, const char *conv_id, const char *session,
                                 const char *text) {
    /* LXMF-ORIGIN ROOMS ANSWER AS LXMF, and BEFORE the seed.pager uplink below.
     * If this room last received an LXMF message, its reply goes back to THAT
     * sender over lxmf.delivery, not to the configured seed.pager peer — which
     * is a different node. A build/enqueue failure does NOT fall through to
     * seed.pager: that peer is not the LXMF sender and would misdeliver, so the
     * fault is put in the room (one short line) and the reply stops here. */
    if (strcmp(conv_id, "claude") == 0) {
        uint8_t lxmf_dest[16];
        if (rns_lxmf_reply_target(session, lxmf_dest)) {
            const char *lx_why = nullptr;
            if (rns_send_lxmf_reply(lxmf_dest, text, &lx_why)) {
                event_add("agent %s lxmf reply", conv_id);
            } else {
                char line[64];
                snprintf(line, sizeof(line), "(lxmf: %s)",
                         lx_why ? lx_why : "not sent");
                agents_push_line(idx, false, line);
                display_force = true;
            }
            return true;
        }
    }

    /* RETICULUM FIRST FOR `claude`, and it is a real first: the bridge below is
     * the fallback now, not the primary. `rns_why` doubles as "RNS was tried
     * and did not take it", which is what makes the fallback visible instead of
     * a silent downgrade to a path the user did not choose. */
    const char *rns_why = nullptr;
    if (agents_rns_uplink(conv_id, session, text, &rns_why)) {
        event_add("agent %s rns uplink", conv_id);
        return true;
    }

    /* Everything that reaches here tries the bridge first and the C1 DM after,
     * which is the ladder the remaining pair has always wanted. */
    bool wifi_ok = agents_bridge_post(conv_id, session, text);
    bool mesh_ok = false;
    if (!wifi_ok && g_agents_mesh_uplink) {
        mesh_ok = g_agents_mesh_uplink(conv_id, text);
        if (mesh_ok) event_add("agent %s mesh uplink", conv_id);
    }
    if (rns_why) {
        /* ONE SHORT LINE, ALWAYS, and it replaces the bridge's own complaint
         * rather than joining it: for `claude` the peer address is the path the
         * user configured, so naming what is wrong with THAT is more use than
         * naming what is wrong with the path they did not pick. Two lines for
         * one keystroke would also push the message they just typed off a
         * seven-row screen. */
        char line[64];
        snprintf(line, sizeof(line), "(rns: %s%s)", rns_why,
                 (wifi_ok || mesh_ok) ? " - sent via bridge" : "");
        agents_push_line(idx, false, line);
        display_force = true;
    } else if (!wifi_ok && !mesh_ok) {
        if (g_bridge[0] && WiFi.status() != WL_CONNECTED)
            agents_push_line(idx, false, "(offline - mesh failed)");
        else if (g_bridge[0])
            agents_push_line(idx, false, "(bridge offline)");
        else if (g_agents_mesh_uplink)
            agents_push_line(idx, false, "(mesh failed)");
        else
            agents_push_line(idx, false, "(no bridge / mesh)");
        display_force = true;
    }
    return true;
}

/* CONV_LXMF: answer the sender this conversation was opened by. The address is
 * the one the planner handed back — the conversation's own stored source hash,
 * not a room-name lookup and not the configured Reticulum peer. */
static bool transport_send_lxmf(const uint8_t *addr, const char *text,
                                const char **why) {
    return rns_send_lxmf_reply(addr, text, why);
}

/* CONV_MESH: a DM straight to the peer's public key.
 *
 * The address is the planner's output — this conversation's own stored key —
 * and mesh_client_send_to_peer() resolves THAT peer's contact. It is not built
 * on the gateway send and must never fall back to it: the gateway is a
 * different node, so a fallback would deliver a private reply to a third party
 * rather than fail. A refusal (radio down, peer not a known contact, or a
 * private send already awaiting its ACK) comes back with a reason that the
 * room shows, and the user can retry. */
static bool transport_send_mesh(const uint8_t *addr, uint8_t addr_len,
                                const char *text, const char **why) {
    if (addr_len != TRANSPORT_MESH_ADDR_LEN) {
        if (why) *why = "bad peer key";
        return false;
    }
    return mesh_client_send_to_peer(addr, text, why);
}

/*
 * THE one outbound seam. Chosen by conv->transport, never by a conversation's
 * name: adding a wire means adding a backend above, not another branch in the
 * chat path. The active room is read off the conversation so no caller has to
 * pass — or pick — one.
 */
bool transport_send(const struct Conversation *conv, const char *text) {
    if (!conv || !text || !text[0]) return false;
    int idx = agents_find(conv->id);
    if (idx < 0) return false;

    TransportTarget tgt;
    tgt.transport = conv->transport;
    tgt.reply_addr = conv->reply_addr;
    tgt.reply_len = conv->reply_len;

    uint8_t backend = TRANSPORT_BACKEND_NONE;
    const uint8_t *addr = nullptr;
    uint8_t addr_len = 0;
    int plan = transport_plan(&tgt, &backend, &addr, &addr_len);

    const char *why = nullptr;
    bool ok = false;
    if (plan != TRANSPORT_OK) {
        why = (plan == TRANSPORT_NO_ADDRESS) ? "no return address"
                                             : "unknown transport";
    } else if (backend == TRANSPORT_BACKEND_AGENT) {
        /* Owns its own in-room reporting (the bridge/RNS ladder's error lines). */
        return transport_send_agent(idx, conv->id,
                                    conv->sessions[conv->active_idx], text);
    } else if (backend == TRANSPORT_BACKEND_LXMF) {
        ok = transport_send_lxmf(addr, text, &why);
        if (ok) event_add("conv %s lxmf reply", conv->id);
    } else if (backend == TRANSPORT_BACKEND_MESH_PEER) {
        ok = transport_send_mesh(addr, addr_len, text, &why);
    }

    if (!ok) {
        /* One short line in the room, same shape the agent ladder uses: a
         * seven-row screen cannot afford two, and a silent failure is how a
         * message the user believes they sent disappears. */
        char line[64];
        snprintf(line, sizeof(line), "(send: %s)", why ? why : "not sent");
        agents_push_line(idx, false, line);
        display_force = true;
    }
    return ok;
}

/* Public: send a line as the user, into the conversation's ACTIVE room.
 * Path: local thread/history → transport_send(), which picks the wire from the
 * conversation record. Downlink replies arrive on the same wire (or WiFi
 * /agents/inbound). One chat loop. */
static bool agents_send(const char *agent_id, const char *text) {
    int idx = agents_find(agent_id);
    if (idx < 0 || !text || !text[0]) return false;

    char cleaned[AGENT_TEXT_LEN];
    agents_clean_text(text, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return false;

    /* "where are you?" never leaves the device: answer with the GPS fix. */
    if (agents_gps_intercept(idx, cleaned)) return true;

    agents_push_line(idx, true, cleaned);   // persists + updates viewport
    agents_lock();
    const char *session = agents_active_session(idx);
    event_add("agent %s<<%s %s", agent_id, session, cleaned);
    agents_unlock();
    display_force = true;

    transport_send(&g_convs[idx], cleaned);
    /* The room already carries the outcome (the backend puts one line in it on
     * failure), and the caller's contract has always been "accepted into the
     * thread", not "delivered" — GET /agents and the transport status routes
     * are where delivery is reported. */
    return true;
}

/* Inject an agent reply into the active session of the agent (long texts split).
 * real_inbound: true only for genuine arrivals (HTTP /agents/inbound, GPS
 * answer) — they may wake the panel when the room is open on screen. Synthetic
 * lines (mesh_chat_tx_fail) pass false and never restart the idle countdown. */
static void agents_on_inbound(const char *agent_id, const char *text,
                              bool real_inbound) {
    int idx = agents_find(agent_id);
    if (idx < 0 || !text || !text[0]) return;
    char cleaned[2048];
    agents_clean_text(text, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return;
    agents_push_line(idx, false, cleaned);
    if (real_inbound) g_agents_real_inbound = true;
    display_force = true;
}

/*
 * THE one inbound message door (declared in ../transport.h). A transport that
 * has received a chat line hands it here instead of reaching into the store.
 *
 * `via` is CHECKED, not decoration: a conversation is only fed by the wire it
 * lives on, so a sender on one transport cannot land a line in another's room
 * by guessing its id. Creating a conversation for a peer nobody has met is the
 * receive half and belongs with the receivers that need it, so an unknown
 * peer_id is refused here rather than silently minted.
 */
bool inbox_deliver_msg(uint8_t via, const char *peer_id, const char *label,
                       const char *text) {
    if (!peer_id || !peer_id[0] || !text || !text[0]) return false;
    int idx = agents_find(peer_id);
    if (idx < 0) return false;
    agents_lock();
    Conversation &a = g_convs[idx];
    bool wire_ok = (a.transport == via);
    /* A sender's display name may change between messages; the id may not. */
    if (wire_ok && label && label[0] && !a.seeded)
        snprintf(a.label, sizeof(a.label), "%s", label);
    agents_unlock();
    if (!wire_ok) return false;
    agents_on_inbound(a.id, text, true);
    return true;
}

/* ---- chat-route seam: claude_route_incoming() ----------------------------- *
 *
 * A transport session (skills/rns.cpp) parses an incoming chat reply out of its
 * envelope and hands it here to land in the correct on-screen "room" (session)
 * of the `claude` agent, selected by name. The declaration + the pure planner
 * (agents_route_plan / agents_route_sanitize) live in ../agents_chat_route.h.
 *
 * LOOP-SAFETY (non-negotiable): the transport calls this on the LOOP task right
 * after rns_stack.loop(). A synchronous SD write there would seize the shared
 * FSPI bus the history writer and the panel paint also live on. So the caller
 * does ONLY RAM work (sanitise + registry snapshot + reason code under
 * agents_mux) and hands the cleaned message to a dedicated off-loop FreeRTOS
 * drain task via a by-value queue, then returns. The drain task
 * (agents_route_task) is the ONLY place the route path touches SD: it
 * selects/creates the room (manifest write), reloads the room view from SD, and
 * appends the message (JSONL append) — all off the loop task. The screen
 * updates when the drain task folds the line into the RAM view and raises
 * display_force; it is never gated on the SD write. */

/* One queued incoming reply, copied into the FreeRTOS queue by value so the
 * caller never shares memory with the drain task. text is already cleaned and
 * bounded (agents_clean_text) — the third untrusted-input barrier after the mac
 * and the transport receiver. */
struct AgentRouteItem {
    uint8_t kind;                       /* 0 = chat message, 1 = roster, 2 = mesh peer message */
    bool newest;                        /* empty name => newest-active room */
    char session[AGENT_SESSION_LEN];    /* kind 0/1: room; kind 2: sender display name */
    char text[AGENT_TEXT_LEN];          /* kind 0/2: cleaned message; kind 1: roster payload */
    /* kind 2 only: the peer's public key — its identity and its return address.
     * Carried by value like everything else here, so the drain task never
     * shares memory with the radio callback that produced it. */
    uint8_t peer[CONV_REPLY_MAX];
    uint8_t peer_len;
};

#define AGENT_ROUTE_QUEUE_DEPTH  8      /* by-value queue: 8 x ~537 B ~= 4.3 KB */
#define AGENT_ROUTE_TASK_STACK   8192   /* drain task does SD scans (sync_view lbuf) */
#define AGENT_ROUTE_TASK_PRIO    1      /* same low prio as the history write task */

static QueueHandle_t g_route_q    = nullptr;
static TaskHandle_t  g_route_task = nullptr;

/* Off-loop roster apply (kind==1): the ONLY place the roster path touches SD.
 * Runs the pure reconcile against the claude agent's sessions[] and REPLACES
 * the liveness view (full snapshot): listed rooms un-mark alive, unlisted rooms
 * go dead only on a complete list (!incomplete); an incomplete list (+N) leaves
 * unlisted rooms untouched. Never creates rooms for unknown labels (V1 does not
 * auto-create — the roster reports liveness of KNOWN rooms). MARK-NOT-DELETE:
 * the JSONL history is never removed. Persists the flags and repaints. */
static void agents_roster_apply(int idx, const char *payload) {
    if (idx < 0) return;
    int nlive = 0, ndead = 0, incomplete = 0;
    agents_lock();
    Conversation &a = g_convs[idx];
    const char *names[AGENT_SESSIONS_MAX];
    int n = a.n_sessions;
    if (n > AGENT_SESSIONS_MAX) n = AGENT_SESSIONS_MAX;
    for (int i = 0; i < n; i++) names[i] = a.sessions[i];
    uint8_t action[AGENT_SESSIONS_MAX];
    agents_roster_reconcile(payload, names, n, action, &incomplete);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        uint8_t want = a.session_dead[i];       /* UNCHANGED keeps current view */
        if (action[i] == AGENT_ROSTER_ALIVE)      want = 0;
        else if (action[i] == AGENT_ROSTER_DEAD)  want = 1;
        if (want != a.session_dead[i]) { a.session_dead[i] = want; changed = true; }
    }
    for (int i = 0; i < n; i++) { if (a.session_dead[i]) ndead++; else nlive++; }
    if (changed) agents_manifest_persist();
    agents_unlock();
    event_add("agent claude roster live=%d dead=%d%s", nlive, ndead,
              incomplete ? " +N" : "");
    display_force = true;                        /* repaint the room picker */
}

/* Short, filename-safe id for a peer: the first 8 hex of its public key. Long
 * enough that two peers colliding is not a practical concern, short enough that
 * /conv.<id> is 14 bytes — well inside the object-name budget. */
static void conv_peer_id(const uint8_t *pubkey, uint8_t len, char *out,
                         size_t out_n) {
    if (!out || out_n == 0) return;
    out[0] = '\0';
    if (!pubkey || len < 4) return;
    conv_hex_encode(pubkey, 4, out, out_n);
}

/* Off-loop half of a mesh peer message (kind 2): mint the conversation if this
 * peer is new, persist it, and append the line. All of it is SD work, which is
 * exactly why it happens here and not in the radio callback. */
static void agents_route_mesh(const AgentRouteItem &item) {
    char id[CONV_ID_LEN];
    conv_peer_id(item.peer, item.peer_len, id, sizeof(id));
    if (!id[0] || !item.text[0]) return;

    agents_lock();
    bool existed = (agents_find(id) >= 0);
    int idx = conv_mint(id, item.session, CONV_MESH, item.peer, item.peer_len);
    if (idx < 0) {
        /* Every slot is seeded or on screen. Dropping is the honest outcome:
         * there is nowhere to put the conversation and the alternative is
         * evicting the chat the user is reading. */
        agents_unlock();
        Serial.printf("[agents] mesh route drop: no slot for %s\n", id);
        return;
    }
    Conversation &a = g_convs[idx];
    if (existed && item.session[0])
        snprintf(a.label, sizeof(a.label), "%s", item.session);  /* name may change */
    if (!existed) {
        /* NO MANIFEST WRITE. A peer conversation is live-only by design: its
         * ROUTE is never read back from the card, so writing it there would be
         * state nothing consumes — and worse, it would leave /conversations.txt
         * carrying CONV_MESH lines, with peer-chosen display names, that this
         * build authored and no later reader ever decided to trust. The
         * history is the part that persists (the JSONL append below, keyed
         * /conv.<id>), so a peer that returns re-mints onto the same id and
         * reopens the same thread. Persisting peers is a deliberate decision
         * of its own, not an inherited side effect of minting one. */
        event_add("conv new mesh %s", id);
    }
    agents_push_line(idx, false, item.text);
    g_agents_real_inbound = true;
    display_force = true;
    agents_unlock();
}

/* The off-loop drain: the ONLY SD I/O on the route path. Drains the queue
 * forever, resolves the target room, reloads its view from SD and appends the
 * reply — everything the caller must NOT do on the loop task. */
static void agents_route_task(void *arg) {
    (void)arg;
    AgentRouteItem item;
    int idx = agents_find("claude");
    for (;;) {
        if (xQueueReceive(g_route_q, &item, portMAX_DELAY) != pdTRUE) continue;
        if (idx < 0) continue;
        if (item.kind == 1) {              /* live-room roster: reconcile + persist */
            agents_roster_apply(idx, item.text);
            continue;
        }
        if (item.kind == 2) {              /* mesh peer message: mint + append */
            agents_route_mesh(item);
            continue;
        }
        if (!item.text[0]) continue;
        agents_lock();
        Conversation &a = g_convs[idx];
        bool created = false;
        if (item.newest) {
            /* newest-active room. active_idx already points at the most-recently
             * selected/created room; if the registry is somehow empty, mint a
             * default. Re-pin to the tail ONLY when this conversation already
             * holds the shared window: goto_tail is the focus point, so calling
             * it here unconditionally would hand the window to an arriving
             * reply and take it away from whatever the user is reading —
             * losing their scroll position mid-read and costing a full JSONL
             * rebuild per message under a burst. An unfocused conversation
             * needs nothing here; the append below keeps its cached line and
             * counters current, and the window is rebuilt when it is opened. */
            if (a.n_sessions == 0) agents_session_create(idx, nullptr, created);
            else if (g_win.owner == idx) agents_thread_goto_tail(idx);
        } else {
            /* Named room: switch to it (reloading its view from SD) if it
             * exists, else create it. Both run here off the loop task, so the
             * manifest write and the view scan are safe. A brand-new name with
             * the registry full returns -1 and leaves active_idx unchanged; the
             * header contract says DROP the line then rather than mis-route it
             * into the previously-active room. */
            if (agents_session_exists(idx, item.session) >= 0) {
                agents_session_select(idx, item.session);
            } else if (agents_session_create(idx, item.session, created) < 0) {
                Serial.printf("[agents] chat-route drop: registry full for %s\n",
                              item.session);
                agents_unlock();
                continue;
            }
        }
        agents_push_line(idx, false, item.text);   /* view append + SD append */
        g_agents_real_inbound = true;
        display_force = true;
        agents_unlock();
    }
}

/* Transport entry point (declared in ../agents_chat_route.h). Land `text` into
 * the `claude` agent's room `session` (empty/NULL => newest-active room).
 * Loop-safe: RAM-only here, SD deferred to agents_route_task. `reason` may be
 * NULL. Returns true when the reply was queued for the off-loop persist; false
 * on a bad name (reason 2), a full registry (reason 1), an empty message, or a
 * dead/full route queue (the bool is authoritative; reason carries 1/2 only for
 * the two resolution rejects — never a synchronous SD fallback). */
bool claude_route_incoming(const char *session, const char *text, int *reason) {
    if (reason) *reason = AGENT_ROUTE_OK;
    int idx = agents_find("claude");
    if (idx < 0) return false;

    /* LIVE-ROOM ROSTER CONTROL FRAME — field 3 == "*" (exactly one byte 0x2A).
     * This test MUST come FIRST, before any sanitising: agents_session_sanitize
     * strips '*' (outside [A-Za-z0-9._-]) to an empty name, which would deliver
     * the room list into a room as a normal message reading e.g. "main,sonata"
     * (README's #1 firmware rule). RAM-only on the caller (loop) task: copy the
     * RAW payload into a queue item and hand it to the off-loop drain; the SD
     * reconcile happens in agents_roster_task/apply. An empty payload
     * (1|<addr>|*|) is a LEGAL frame meaning "no live sessions", so it is
     * enqueued too (unlike the message path, which drops empty text). */
    if (session && session[0] == '*' && session[1] == '\0') {
        AgentRouteItem item;
        memset(&item, 0, sizeof(item));
        item.kind = 1;                                  /* roster, not a message */
        snprintf(item.text, sizeof(item.text), "%s", text ? text : "");
        if (!g_route_q || xQueueSend(g_route_q, &item, 0) != pdTRUE) return false;
        return true;
    }

    /* Resolve name + reason against a RAM snapshot of the registry (no I/O). */
    char resolved[AGENT_ROUTE_NAME_CAP];
    int newest = 0;
    int r;
    agents_lock();
    {
        Conversation &a = g_convs[idx];
        const char *names[AGENT_SESSIONS_MAX];
        int n = a.n_sessions;
        if (n > AGENT_SESSIONS_MAX) n = AGENT_SESSIONS_MAX;
        for (int i = 0; i < n; i++) names[i] = a.sessions[i];
        r = agents_route_plan(session, names, n, AGENT_SESSIONS_MAX,
                              resolved, &newest);
    }
    agents_unlock();
    if (r != AGENT_ROUTE_OK) { if (reason) *reason = r; return false; }

    /* Clean the untrusted reply (third barrier) and bound it. Nothing left to
     * show => nothing to land. */
    AgentRouteItem item;
    memset(&item, 0, sizeof(item));
    agents_clean_text(text, item.text, sizeof(item.text));
    if (!item.text[0]) return false;
    item.newest = (newest != 0);
    if (!item.newest) snprintf(item.session, sizeof(item.session), "%s", resolved);

    /* Hand off to the drain task; 0-tick send never blocks the loop. A dead or
     * full queue drops (false) rather than falling back to a synchronous SD
     * write on the caller's task. */
    if (!g_route_q || xQueueSend(g_route_q, &item, 0) != pdTRUE) return false;
    return true;
}

/*
 * A MeshCore peer's message (declared in ../transport.h). RAM ONLY: the radio
 * callback that calls this runs on the loop task, so the message is cleaned,
 * copied by value and handed to the off-loop drain — the mint, the manifest
 * write and the JSONL append all happen there (agents_route_mesh). Returns
 * false when there is nothing to land or the queue is dead/full, and the caller
 * falls back to a card so the message is never simply lost.
 */
bool inbox_deliver_msg_mesh(const uint8_t *pubkey, uint8_t pubkey_len,
                            const char *name, const char *text) {
    if (!pubkey || pubkey_len != TRANSPORT_MESH_ADDR_LEN) return false;
    if (!text || !text[0]) return false;
    AgentRouteItem item;
    memset(&item, 0, sizeof(item));
    item.kind = 2;
    item.peer_len = pubkey_len;
    memcpy(item.peer, pubkey, pubkey_len);
    /* The sender's name is untrusted display text like any other inbound
     * string — cleaned here, and bounded to the label field by the store. */
    agents_clean_text(name, item.session, sizeof(item.session));
    agents_clean_text(text, item.text, sizeof(item.text));
    if (!item.text[0]) return false;
    if (!g_route_q || xQueueSend(g_route_q, &item, 0) != pdTRUE) return false;
    return true;
}

/* Clear a session's history. Single agent → the ACTIVE session only (UI path);
 * "*"/empty → every session file + per-session counter for an agent (or all
 * agents). The session stays registered either way; the active one restarts
 * with the "chat cleared" line. */
static bool agents_clear(const char *agent_id) {
    agents_lock();
    bool any = false;
    bool all = (!agent_id || !agent_id[0] || strcmp(agent_id, "*") == 0);
    auto clear_agent = [&](int i) -> void {
        Conversation &a = g_convs[i];
        if (all) {
            for (int j = 0; j < a.n_sessions; j++) {
                if (agents_store_ready()) {
                    String path = agents_log_path(a.id, a.sessions[j]);
                    HwSpiBusGuard bus;  /* remove only — released before
                                         * agents_push_line re-enters I/O */
                    g_store->remove(path.c_str());
                }
                a.session_lines[j] = 0;
            }
            a.lines = 0;
            if (g_win.owner == i) agents_window_take(i);
            agents_push_line(i, false, "chat cleared - type to talk");
        } else {
            if (agents_store_ready()) {
                String path = agents_log_path(a.id, a.sessions[a.active_idx]);
                HwSpiBusGuard bus;
                g_store->remove(path.c_str());
            }
            a.lines = 0;
            if (g_win.owner == i) agents_window_take(i);
            a.session_lines[a.active_idx] = 0;
            agents_push_line(i, false, "chat cleared - type to talk");
        }
        any = true;
    };
    if (all) {
        for (int i = 0; i < g_conv_n; i++) clear_agent(i);
    } else {
        int idx = agents_find(agent_id);
        if (idx >= 0) clear_agent(idx);
    }
    agents_unlock();
    display_force = true;
    return any;
}

static int agents_count() { return g_conv_n; }

static const char *agents_id(int i) {
    return (i >= 0 && i < g_conv_n) ? g_convs[i].id : "";
}
static const char *agents_name(int i) {
    return (i >= 0 && i < g_conv_n) ? g_convs[i].label : "";
}
static bool agents_bridge_ok() { return g_bridge[0] != '\0'; }
static const char *agents_bridge_url() { return g_bridge; }

static const char *agents_describe() {
    return
        "# agents\n\n"
        "Pocket chat with Claude / Hermes.\n"
        "Uplink: WiFi bridge /v1/chat, else MeshCore C1|agent|…|u|… private DM\n"
        "  (agent = claude|hermes).\n"
        "Downlink: same C1 side=a (or WiFi /agents/inbound) — one loop.\n"
        "A \"where are you?\" / \"где ты\" message is answered locally with the\n"
        "GNSS fix (POST /gps/fix semantics) and never hits the bridge.\n\n"
        "History: append-only JSONL on SD (fallback SPIFFS) at\n"
        "`/conv.<conversation>[.<session>]`; only a 24-message viewport is kept\n"
        "in RAM. Room registry: `/conversations.txt`.\n\n"
        "SPIFFS `/agent_bridge.txt` = base URL (http://host:port).\n";
}

static const SkillEndpoint agents_endpoints[] = {
    {"GET",  "/agents",         "List agents, bridge, sessions, history counts"},
    {"POST", "/agents/send",    "Send {agent, text, session?} into a thread (+ bridge)"},
    {"POST", "/agents/inbound", "Inject agent reply {agent, text} into thread"},
    {"POST", "/agents/clear",   "Clear active session {agent} or {\"all\":true}"},
    {"POST", "/agents/bridge",  "Set bridge base URL (raw body), empty clears"},
    {"POST", "/agents/session", "Select session {agent, session}"},
    {"POST", "/agents/session/new", "Create/switch session {agent, session?}"},
    {NULL, NULL, NULL}
};

static void agents_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/agents"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        doc["bridge"] = g_bridge[0] ? g_bridge : "";
        doc["session"] = g_session;
        doc["store"] = agents_store_ready() ? agents_store_name() : "none";
        JsonArray arr = doc["agents"].to<JsonArray>();
        agents_lock();
        for (int i = 0; i < g_conv_n; i++) {
            Conversation &a = g_convs[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"] = a.id;
            o["name"] = a.label;
            o["messages"] = a.lines;
            o["active"] = a.sessions[a.active_idx];
            JsonArray sess = o["sessions"].to<JsonArray>();
            JsonArray smsg = o["session_msgs"].to<JsonArray>();
            for (int j = 0; j < a.n_sessions; j++) {
                sess.add(a.sessions[j]);
                smsg.add(a.session_lines[j]);
            }
            /* The per-conversation cached line, which is the newest one
             * appended whether or not this conversation holds the window —
             * strictly more honest than reading a scrollback that may be
             * parked on an older page. */
            if (a.last.text[0]) {
                o["last"] = a.last.text;
                o["last_from_me"] = a.last.from_me;
                o["last_ts"] = a.last.ts;
            }
            /* Honest tail for GET even when the RAM window is a rewound page. */
            if (!agents_thread_is_tail(i)) {
                bool lfm = false;
                uint32_t lts = 0;
                char lt[AGENT_TEXT_LEN];
                if (agents_file_last(i, lt, sizeof(lt), &lfm, &lts)) {
                    o["last"] = lt;
                    o["last_from_me"] = lfm;
                    o["last_ts"] = lts;
                }
            }
        }
        agents_unlock();
        notify_send_json(req, 200, doc);
    });

    server.on(AsyncURIMatcher::exact("/agents/send"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) { notify_send_error(req, 400, "JSON body required"); return; }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        const char *agent  = input["agent"]  | "";
        const char *text   = input["text"]   | "";
        const char *sess   = input["session"] | "";
        int idx = agents_find(agent);
        if (idx < 0) { notify_send_error(req, 400, "agent must be claude or hermes"); return; }
        if (!text[0]) { notify_send_error(req, 400, "text required"); return; }
        if (sess[0]) {
            agents_lock();
            bool ok_sel = agents_session_select(idx, sess);
            agents_unlock();
            if (!ok_sel) { notify_send_error(req, 400, "unknown session"); return; }
        }
        bool ok = agents_send(agent, text);
        agents_lock();
        const char *act = agents_active_session(idx);
        JsonDocument doc;
        doc["ok"] = ok;
        doc["agent"] = agent;
        doc["session"] = act;
        doc["bridge"] = g_bridge[0] ? true : false;
        agents_unlock();
        notify_send_json(req, ok ? 200 : 500, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/clear"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) { notify_send_error(req, 400, "JSON body required"); return; }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        bool all = input["all"] | false;
        const char *agent = input["agent"] | "";
        bool ok = all ? agents_clear("*") : agents_clear(agent);
        if (!ok) { notify_send_error(req, 400, "unknown agent"); return; }
        event_add("agent clear %s", all ? "*" : agent);
        JsonDocument doc;
        doc["ok"] = true;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/inbound"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) { notify_send_error(req, 400, "JSON body required"); return; }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        const char *agent = input["agent"] | "";
        const char *text  = input["text"]  | "";
        if (agents_find(agent) < 0) { notify_send_error(req, 400, "agent must be claude or hermes"); return; }
        if (!text[0]) { notify_send_error(req, 400, "text required"); return; }
        agents_on_inbound(agent, text, true);
        event_add("agent %s >> %s", agent, text);
        JsonDocument doc;
        doc["ok"] = true;
        doc["agent"] = agent;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/bridge"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        String url = body ? String(body) : String("");
        free(body);
        req->_tempObject = nullptr;
        url.trim();
        if (url.length() > 0 && !url.startsWith("http://")) {
            notify_send_error(req, 400, "URL must start with http://"); return;
        }
        if (!agents_bridge_save(url.c_str())) {
            notify_send_error(req, 500, "failed to save bridge URL"); return;
        }
        event_add("agent bridge %s", g_bridge[0] ? g_bridge : "(cleared)");
        JsonDocument doc;
        doc["ok"] = true;
        doc["bridge"] = g_bridge;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/session"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) { notify_send_error(req, 400, "JSON body required"); return; }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        const char *agent = input["agent"] | "";
        const char *sess  = input["session"] | "";
        int idx = agents_find(agent);
        if (idx < 0) { notify_send_error(req, 400, "agent must be claude or hermes"); return; }
        if (!sess[0]) { notify_send_error(req, 400, "session required"); return; }
        agents_lock();
        bool ok = agents_session_select(idx, sess);
        agents_unlock();
        if (!ok) { notify_send_error(req, 400, "unknown session"); return; }
        event_add("agent %s select session %s", agent, sess);
        JsonDocument doc;
        doc["ok"] = true;
        doc["agent"] = agent;
        doc["active"] = agents_active_session(idx);
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/session/new"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) { notify_send_error(req, 400, "JSON body required"); return; }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        const char *agent = input["agent"] | "";
        const char *sess  = input["session"] | "";
        int idx = agents_find(agent);
        if (idx < 0) { notify_send_error(req, 400, "agent must be claude or hermes"); return; }
        agents_lock();
        bool created = false;
        int r = agents_session_create(idx, sess[0] ? sess : nullptr, created);
        agents_unlock();
        if (r < 0) { notify_send_error(req, 400, "session registry full / bad name"); return; }
        JsonDocument doc;
        doc["ok"] = true;
        doc["created"] = created;
        doc["agent"] = agent;
        doc["active"] = agents_active_session(idx);
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);
}

static const Skill agents_skill = {
    .name = "agents",
    .version = "0.2.0",
    .describe = agents_describe,
    .endpoints = agents_endpoints,
    .register_routes = agents_register_routes,
    .tick = nullptr
};

static void agents_store_init() {
    /* Mount SD on the SHARED FSPI instance the display/LoRa use. Explicit rule:
     * never create a second SPIClass on the same pins — that hangs the radio. */
    g_store = nullptr;
    if (hw_ui_spi()) {
        pinMode(PIN_SD_CS, OUTPUT);
        digitalWrite(PIN_SD_CS, HIGH);
        HwSpiBusGuard bus;  /* card init probes the shared bus */
        if (SD.begin(PIN_SD_CS, *hw_ui_spi(), 4000000, "/sd", 5, false)) {
            g_store = &SD;
            g_store_is_sd = true;
        }
    }
    if (!g_store) {
        g_store = &SPIFFS;   // graceful fallback — same JSONL behaviour
        g_store_is_sd = false;
    }
    Serial.printf("[agents] history store=%s (%s)\n",
                  agents_store_name(),
                  g_store_is_sd ? "SD" : "SPIFFS fallback");
    event_add("agents store %s", agents_store_name());
}

/* One-shot rename of every known thread's log from the retired
 * /agent.<id>.<session> key into /conv.<...>. Runs once at boot, AFTER the
 * registry is loaded (that is what makes the set of threads known) and BEFORE
 * the first view sync, so a card that was written by an older build shows its
 * history in the new namespace instead of appearing empty. A thread that has
 * already moved, or never had a legacy file, costs one exists() and nothing
 * else. Nothing is deleted: a rename that fails leaves the legacy file exactly
 * where it was. */
static void agents_migrate_logs() {
    if (!agents_store_ready()) return;
    int moved = 0, skipped = 0, failed = 0;
    {
        HwSpiBusGuard bus;   /* one boot burst; released before event_add */
        for (int i = 0; i < g_conv_n; i++) {
            Conversation &a = g_convs[i];
            for (int j = 0; j < a.n_sessions; j++) {
                String from = agents_legacy_log_path(a.id, a.sessions[j]);
                if (!g_store->exists(from.c_str())) continue;
                String to = agents_log_path(a.id, a.sessions[j]);
                /* BOTH files present. Reachable on the reflash workflow (a card
                 * used by a new build, then an old one, then new again): each
                 * half holds real messages and picking one silently discards
                 * the other, so neither is touched and the split is REPORTED
                 * instead — a count that appears in the event log is what turns
                 * "my history looks short" into a diagnosable state. */
                if (!to.length() || g_store->exists(to.c_str())) { skipped++; continue; }
                if (g_store->rename(from.c_str(), to.c_str())) moved++;
                else failed++;
            }
        }
    }
    if (moved || skipped || failed)
        event_add("agents store logs moved=%d split=%d failed=%d",
                  moved, skipped, failed);
}

static void skill_agents_init() {
    // Session default tag includes last 4 of chip for multi-pager later.
    uint64_t mac = ESP.getEfuseMac();
    snprintf(g_session, sizeof(g_session), "pager-%04x",
             (unsigned)(mac & 0xFFFF));
    agents_mux = xSemaphoreCreateRecursiveMutex();
    agents_bridge_load();
    agents_store_init();

    /* Answer pending "where are you?" cards the moment a GPS fix lands. */
    gps_set_on_fix(agents_gps_on_fix);

    for (int i = 0; i < g_conv_n; i++) {
        Conversation &a = g_convs[i];
        a.active_idx = 0; a.lines = 0;
        for (int j = 0; j < AGENT_SESSIONS_MAX; j++) {
            a.session_lines[j] = 0;
            a.session_dead[j] = 0;      // all rooms shown until a roster says otherwise
        }
        agents_session_add(i, g_session);               // default always present
    }
    agents_manifest_load();                             // existing rooms
    agents_migrate_logs();      // retired /agent.* keys -> /conv.* (once)
    // Guarantee at least the default survives a reboot, in the new format.
    agents_manifest_persist();

    for (int i = 0; i < g_conv_n; i++) {
        /* One scan each: this fills lines / session_lines / the cached last
         * line, which is all a conversation needs until it is opened. The
         * window is left wherever the last scan put it and is rebuilt on the
         * first draw (agents_thread_view's lazy focus), so the goto_tail that
         * used to follow here is gone — after a long-history sync it saw a
         * non-tail window and rebuilt the whole file a SECOND time, once per
         * conversation, at boot. */
        agents_sync_view(i);
        /* The greeting belongs to the compiled-in doors only. A conversation
         * minted for a peer must not have a line the device wrote to itself
         * appear in its history. */
        if (g_convs[i].seeded && g_convs[i].lines == 0) {
            char intro[AGENT_TEXT_LEN];
            snprintf(intro, sizeof(intro), "hi - type to talk to %s",
                     g_convs[i].label);
            agents_push_line(i, false, intro);
        }
    }

    /* Off-loop chat-route drain: the transport (rns.cpp) enqueues incoming
     * replies from the loop task; this task performs all the route's SD I/O. */
    g_route_q = xQueueCreate(AGENT_ROUTE_QUEUE_DEPTH, sizeof(AgentRouteItem));
    if (g_route_q)
        xTaskCreate(agents_route_task, "agt_route", AGENT_ROUTE_TASK_STACK,
                    nullptr, AGENT_ROUTE_TASK_PRIO, &g_route_task);
    Serial.printf("[agents] chat-route queue=%s depth=%d\n",
                  g_route_q ? "ok" : "FAILED", AGENT_ROUTE_QUEUE_DEPTH);

    skill_register(&agents_skill);
    Serial.printf("[agents] session=%s bridge=%s store=%s/%s\n",
                  g_session, g_bridge[0] ? g_bridge : "(none)",
                  agents_store_name(), agents_active_session(0));
}
