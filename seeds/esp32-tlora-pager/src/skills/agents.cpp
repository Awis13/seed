/*
 * skills/agents.cpp — pocket chat with remote agents (Grok / Claude / Hermes)
 *
 * Pager is the thin terminal. A bridge on the LAN does the real work:
 *   POST {bridge}/v1/chat  { "agent","session","text" }
 * Answers come back as ordinary /notify
 * (source=grok|claude|hermes|opencode|codex) or, when
 * the bridge is missing, as a local stub line in the thread.
 *
 * SPIFFS:
 *   /agent_bridge.txt  — base URL, e.g. http://192.168.1.138:8090  (one line)
 *
 * History:
 *   Non-transient per-(agent, session) history lives on removable SD as an
 *   append-only JSONL file, one message per line:  {ts, from_me, text}.
 *   Only a small viewport window (the last AGENT_VIEW_MAX messages of the
 *   current session) is kept in RAM as an index; the rest stays on disk, so a
 *   "very long history" costs no extra static RAM. Multiple sessions exist per
 *   agent under the key (agent, session); one is active and selectable.
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
 *   Session registry is a tiny manifest  /agents_sessions.txt  (one
 *   "agent\tsession" line each) in the same store, so the session list
 *   survives reboots without needing SD directory iteration.
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

#define AGENTS_N            5
#define AGENT_ID_LEN        12
#define AGENT_NAME_LEN      16
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
#define AGENT_SESSION_LEN   24
#define AGENT_SESSIONS_MAX  8       /* max sessions per agent (registry cap) */
#define AGENT_MANIFEST      "/agents_sessions.txt"
#define AGENT_LOG_PREFIX    "agent" /* flat store files /agent.<id>.<s>.jsonl */
/* JSONL line worst case: 2x escaped text + wrapper + newline. */
#define AGENT_JSONL_MAX     (AGENT_TEXT_LEN * 2 + 64)

struct AgentLine {
    bool from_me;
    uint32_t ts;            // unix seconds (C1 JSONL "ts"); 0 = unknown
    char text[AGENT_TEXT_LEN];
};

struct AgentSlot {
    const char *id;     // "grok" / "claude" / "hermes" / "opencode" / "codex"
    const char *name;   // UI label
    char sessions[AGENT_SESSIONS_MAX][AGENT_SESSION_LEN];
    uint8_t n_sessions;
    uint8_t active_idx;         // into sessions[]
    uint32_t session_lines[AGENT_SESSIONS_MAX];  // persisted msg count per session
    uint32_t file_sync;         // JSONL bytes already folded into view+lines
    uint32_t lines;             // total messages in the active session
    uint8_t vn;                 // messages currently in the view window
    uint32_t win_start;         // absolute line index of view[0] (tail or loaded page)
    AgentLine view[AGENT_VIEW_MAX];   // window over the active session
};

static AgentSlot g_agents[AGENTS_N] = {
    { "grok",     "GROK",     {}, 0, 0, {}, 0, 0, 0, 0, {} },
    { "claude",   "CLAUDE",   {}, 0, 0, {}, 0, 0, 0, 0, {} },
    { "hermes",   "HERMES",   {}, 0, 0, {}, 0, 0, 0, 0, {} },
    { "opencode", "OPENCODE", {}, 0, 0, {}, 0, 0, 0, 0, {} },
    { "codex",    "CODEX",    {}, 0, 0, {}, 0, 0, 0, 0, {} },
};

static char g_bridge[AGENT_BRIDGE_LEN] = "";
static char g_session[AGENT_SESSION_LEN] = "pager";
static FS *g_store = nullptr;        // &SD (primary) or &SPIFFS (fallback)
static bool g_store_is_sd = false;

/* Recursive mutex: agents_push_line() locks itself (mesh task calls it without
 * a lock) while send/inbound/view also take the same lock (nested). File IO
 * happens under the mutex, never under portENTER_CRITICAL — SD writes block. */
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
static void agents_view_append(AgentSlot &a, bool from_me, uint32_t ts,
                               const char *text);
static int agents_find(const char *id);
static int agents_session_add(int idx, const char *name);

/* Public UI accessors (non-static on purpose — main.cpp needs them cleanly).
 * Session list, timestamps and the long-scroll paging contract for the chat. */
int    agents_session_count(int idx);
const char *agents_session_name(int idx, int i);
bool   agents_session_is_active(int idx, int i);
int    agents_session_msg_count(int idx, int i);
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

/* Defined later in this file (used by the GPS interception below). */
static void agents_on_inbound(const char *agent_id, const char *text);

/* ---- store ---------------------------------------------------------------- */

static const char *agents_store_name() { return g_store_is_sd ? "sd" : "spiffs"; }

static bool agents_store_ready() { return g_store != nullptr; }

/* Flat key path (no directory on SPIFFS; FAT root on SD). Session is already
 * sanitised to [A-Za-z0-9._-]. */
static String agents_log_path(const char *agent_id, const char *session) {
    /* SPIFFS object-name limit is 32 bytes (31 usable incl. NUL). A suffix of
     * ".jsonl" pushed the opencode agent's path past it — file never created,
     * empty thread, black chat screen. Drop the extension; content stays JSONL. */
    return String("/") + AGENT_LOG_PREFIX + "." + agent_id + "." + session;
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
    AgentSlot &a = g_agents[idx];
    uint32_t ts = agents_now_s();
    agents_view_append(a, from_me, ts, text);
    if (!agents_store_ready()) return;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
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
        a.file_sync += written;
    }
}

static void agents_view_append(AgentSlot &a, bool from_me, uint32_t ts,
                               const char *text) {
    if (a.vn >= AGENT_VIEW_MAX) {
        memmove(&a.view[0], &a.view[1], (AGENT_VIEW_MAX - 1) * sizeof(AgentLine));
        a.view[AGENT_VIEW_MAX - 1].from_me = from_me;
        a.view[AGENT_VIEW_MAX - 1].ts = ts;
        snprintf(a.view[AGENT_VIEW_MAX - 1].text, sizeof(a.view[0].text),
                 "%s", text ? text : "");
        a.win_start++;
    } else {
        a.view[a.vn].from_me = from_me;
        a.view[a.vn].ts = ts;
        snprintf(a.view[a.vn].text, sizeof(a.view[0].text), "%s", text ? text : "");
        a.vn++;
    }
}

/* Buffered JSONL line scanner. Reads the file in 512-byte blocks instead of
 * one f.read() per byte — the long-history page load / delta-sync / tail read
 * paths run on the shared SPI bus and must not freeze the UI on long files. */
struct AgentJScanner {
    explicit AgentJScanner(File &file) : f(file), pos(0), len(0), eof(false) {}
    File f;
    uint8_t buf[512];
    size_t pos;      // next unconsumed byte in buf
    size_t len;      // valid bytes in buf (0 → refill)
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

/* Bring the RAM index in line with the JSONL file: read only the bytes after
 * file_sync (delta), append them to the window. On external truncation/resync
 * the window is rebuilt from zero. Caller holds the lock. */
static void agents_sync_view(int idx) {
    AgentSlot &a = g_agents[idx];
    if (!agents_store_ready()) return;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    if (!g_store->exists(path.c_str())) {
        a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
        return;
    }
    File f = g_store->open(path.c_str(), "r");
    if (!f) { a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0; return; }

    size_t sz = f.size();
    if (sz < a.file_sync) {          // file shrank under us → full rebuild
        a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
    }
    if (a.file_sync < f.size() && f.seek(a.file_sync)) {
        char lbuf[AGENT_JSONL_MAX];
        char text[AGENT_TEXT_LEN];
        bool from_me;
        uint32_t ts;
        AgentJScanner sc(f);
        while (sc.next(lbuf, sizeof(lbuf))) {
            if (lbuf[0] && agents_jsonl_parse(lbuf, from_me, ts, text, sizeof(text))) {
                agents_view_append(a, from_me, ts, text);
                a.lines++;
            }
        }
    }
    a.file_sync = (uint32_t)f.size();
    a.session_lines[a.active_idx] = a.lines;
    f.close();
}

/* Load the session registry from the store manifest (one agent\tsession/line). */
static void agents_manifest_load() {
    if (!g_store || !g_store->exists(AGENT_MANIFEST)) return;
    File f = g_store->open(AGENT_MANIFEST, "r");
    if (!f) return;
    char ln[AGENT_ID_LEN + 1 + AGENT_SESSION_LEN + 2];
    AgentJScanner sc(f);
    while (sc.next(ln, sizeof(ln))) {
        if (!ln[0]) continue;
        char *tab = strchr(ln, '\t');
        if (!tab || !tab[1]) continue;
        *tab = '\0';
        int idx = agents_find(ln);
        if (idx >= 0) agents_session_add(idx, tab + 1);
    }
    f.close();
}

static void agents_manifest_persist() {
    if (!agents_store_ready()) return;
    String out;
    for (int i = 0; i < AGENTS_N; i++) {
        AgentSlot &a = g_agents[i];
        for (int j = 0; j < a.n_sessions; j++) {
            out += a.id; out += '\t'; out += a.sessions[j]; out += '\n';
        }
    }
    File f = g_store->open(AGENT_MANIFEST, "w");
    if (!f) return;
    f.print(out);
    f.close();
}

/* ---- agents --------------------------------------------------------------- */

static int agents_find(const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < AGENTS_N; i++) {
        if (strcmp(g_agents[i].id, id) == 0) return i;
    }
    return -1;
}

static int agents_session_exists(int idx, const char *name) {
    AgentSlot &a = g_agents[idx];
    for (int i = 0; i < a.n_sessions; i++)
        if (strcmp(a.sessions[i], name) == 0) return i;
    return -1;
}

static int agents_session_add(int idx, const char *name) {
    AgentSlot &a = g_agents[idx];
    if (!name || !name[0]) return -1;
    int ex = agents_session_exists(idx, name);
    if (ex >= 0) return ex;
    if (a.n_sessions >= AGENT_SESSIONS_MAX) return -1;
    snprintf(a.sessions[a.n_sessions], AGENT_SESSION_LEN, "%s", name);
    a.n_sessions++;
    return a.n_sessions - 1;
}

const char *agents_active_session(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return "";
    return g_agents[idx].sessions[g_agents[idx].active_idx];
}

int agents_session_count(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return 0;
    return g_agents[idx].n_sessions;
}

const char *agents_session_name(int idx, int i) {
    if (idx < 0 || idx >= AGENTS_N || i < 0 || i >= g_agents[idx].n_sessions)
        return "";
    return g_agents[idx].sessions[i];
}

bool agents_session_is_active(int idx, int i) {
    if (idx < 0 || idx >= AGENTS_N || i < 0 || i >= g_agents[idx].n_sessions)
        return false;
    return (i == g_agents[idx].active_idx);
}

int agents_session_msg_count(int idx, int i) {
    if (idx < 0 || idx >= AGENTS_N || i < 0 || i >= g_agents[idx].n_sessions)
        return 0;
    return (int)g_agents[idx].session_lines[i];
}

/* Recount every session's persisted lines for the agent (cache the counts so
 * the session-list screen is honest about message numbers without holding the
 * full history). Opens ≤ AGENT_SESSIONS_MAX files; runs only on screen open.
 * The active session's window index is not clobbered: if its file count moved
 * away from the RAM index, re-sync (delta or full) so win_start/lines stay
 * consistent with view[]. */
bool agents_session_refresh_counts(int idx) {
    if (idx < 0 || idx >= AGENTS_N || !agents_store_ready()) return false;
    agents_lock();
    AgentSlot &a = g_agents[idx];
    for (int j = 0; j < a.n_sessions; j++) {
        String path = agents_log_path(a.id, a.sessions[j]);
        uint32_t cnt = 0;
        File f = g_store->open(path.c_str(), "r");
        if (f) {
            uint8_t chunk[256];
            size_t r;
            while ((r = f.read(chunk, sizeof(chunk))) > 0) {
                for (size_t k = 0; k < r; k++)
                    if (chunk[k] == '\n') cnt++;
            }
            f.close();
        }
        a.session_lines[j] = cnt;
        if (j == a.active_idx && a.lines != cnt)
            agents_sync_view(idx);   // re-anchor window + win_start to the file
    }
    agents_unlock();
    return true;
}

/* Select an existing session: rewind + reload the viewport from SD. */
bool agents_session_select(int idx, const char *name) {
    if (idx < 0 || idx >= AGENTS_N || !name || !name[0]) return false;
    agents_lock();
    int in_list = agents_session_exists(idx, name);
    if (in_list < 0) { agents_unlock(); return false; }
    g_agents[idx].active_idx = (uint8_t)in_list;
    g_agents[idx].file_sync = 0;
    g_agents[idx].lines = 0;
    g_agents[idx].vn = 0;
    g_agents[idx].win_start = 0;
    agents_sync_view(idx);
    g_agents[idx].win_start = (g_agents[idx].lines > g_agents[idx].vn)
        ? (g_agents[idx].lines - g_agents[idx].vn) : 0;
    agents_unlock();
    return true;
}

/* Create a fresh session (or reuse an existing name) and make it active.
 * Returns the index in the session list, or -1 on full/empty. */
int agents_session_create(int idx, const char *wanted, bool &created) {
    if (idx < 0 || idx >= AGENTS_N) return -1;
    agents_lock();
    AgentSlot &a = g_agents[idx];
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
    a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
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
    AgentSlot &a = g_agents[idx];
    if (!agents_store_ready()) return;
    if (end_line > a.lines) end_line = a.lines;
    uint32_t keep_from = (end_line > AGENT_VIEW_MAX) ? (end_line - AGENT_VIEW_MAX) : 0;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    File f = g_store->open(path.c_str(), "r");
    if (!f) { a.vn = 0; a.win_start = 0; return; }
    char lbuf[AGENT_JSONL_MAX];
    char text[AGENT_TEXT_LEN];
    bool from_me;
    uint32_t ts;
    uint32_t line_idx = 0;
    a.vn = 0;
    a.win_start = keep_from;
    AgentJScanner sc(f);
    while (sc.next(lbuf, sizeof(lbuf))) {
        if (lbuf[0] && line_idx >= keep_from && line_idx < end_line &&
            agents_jsonl_parse(lbuf, from_me, ts, text, sizeof(text))) {
            agents_view_append(a, from_me, ts, text);
        }
        line_idx++;
    }
    f.close();
}

/* Re-pin the window to the newest messages of the active session. If the user
 * was browsing an old page the window is stale, so force a full rebuild; if it
 * is already the tail, a cheap delta-sync folds any new arrivals. */
void agents_thread_goto_tail(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return;
    agents_lock();
    AgentSlot &a = g_agents[idx];
    bool at_tail = ((uint32_t)a.win_start + a.vn) >= a.lines;
    if (!at_tail) {
        a.file_sync = 0; a.lines = 0; a.vn = 0;
    }
    agents_sync_view(idx);
    a.win_start = (a.lines > a.vn) ? (a.lines - a.vn) : 0;
    agents_unlock();
}

/* Load an older page: the window becomes the last AGENT_VIEW_MAX lines ending
 * at end_line (usually the current window's win_start) so wheel-up browsing
 * walks a very long history. If end_line ≥ total, falls back to the tail.
 * Page-load rescans the file from byte 0 (O(up-to-end)); realistic session
 * sizes are fast, and the newest page is always O(delta). */
void agents_thread_goto_page(int idx, uint32_t end_line) {
    if (idx < 0 || idx >= AGENTS_N) return;
    agents_lock();
    AgentSlot &a = g_agents[idx];
    if (end_line >= a.lines) {
        a.vn = 0; a.win_start = a.lines;
    } else {
        agents_load_page(idx, end_line);
    }
    agents_unlock();
}

uint32_t agents_thread_total(int idx) {
    return (idx >= 0 && idx < AGENTS_N) ? g_agents[idx].lines : 0;
}

uint32_t agents_thread_start(int idx) {
    return (idx >= 0 && idx < AGENTS_N) ? g_agents[idx].win_start : 0;
}

bool agents_thread_is_tail(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return true;
    const AgentSlot &a = g_agents[idx];
    return ((uint32_t)a.win_start + a.vn) >= a.lines;
}

uint32_t agents_thread_line_ts(int idx, int i) {
    if (idx < 0 || idx >= AGENTS_N) return 0;
    const AgentSlot &a = g_agents[idx];
    return (i >= 0 && i < a.vn) ? a.view[i].ts : 0;
}

/* Read the very last message of the active session straight from the store
 * (used by GET /agents when the RAM window is a rewound page, not the tail). */
static bool agents_file_last(int idx, char *text, size_t text_n,
                             bool *from_me, uint32_t *ts) {
    AgentSlot &a = g_agents[idx];
    if (!agents_store_ready()) return false;
    String path = agents_log_path(a.id, a.sessions[a.active_idx]);
    File f = g_store->open(path.c_str(), "r");
    if (!f) return false;
    char lbuf[AGENT_JSONL_MAX];
    char t2[AGENT_TEXT_LEN];
    bool fm = false;
    uint32_t t = 0;
    bool found = false;
    AgentJScanner sc(f);
    while (sc.next(lbuf, sizeof(lbuf))) {
        if (lbuf[0] && agents_jsonl_parse(lbuf, fm, t, t2, sizeof(t2))) found = true;
    }
    f.close();
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
    if (idx < 0 || idx >= AGENTS_N || !text || !text[0]) {
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
/* Copy the current window (oldest→newest) for the chat renderer. No disk
 * reads here — goto_tail / goto_page own the sync. ts_out[i] is the message
 * unix ts (0 = unknown). Returns count. Caller may hold the recursive lock. */
int agents_thread_view(int idx, char out[][AGENT_TEXT_LEN], bool *from_me,
                       uint32_t *ts_out, int max_lines) {
    if (idx < 0 || idx >= AGENTS_N || max_lines <= 0) return 0;
    int n = 0;
    agents_lock();
    AgentSlot &a = g_agents[idx];
    n = a.vn < max_lines ? a.vn : max_lines;
    for (int i = 0; i < n; i++) {
        snprintf(out[i], AGENT_TEXT_LEN, "%s", a.view[i].text);
        if (from_me) from_me[i] = a.view[i].from_me;
        if (ts_out)  ts_out[i] = a.view[i].ts;
    }
    agents_unlock();
    return n;
}

static int agents_thread_count(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return 0;
    return g_agents[idx].lines;
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
    File f = SPIFFS.open(AGENT_BRIDGE_FILE, "w");
    if (!f) return false;
    f.print(url);
    f.close();
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
 * moment the first RMC 'A' lands. Non-blocking throughout — no gps_take_fix(). */

#define AGENT_GPS_FRESH_S 60    /* mirror gps.cpp GPS_FRESH_S */

static bool g_agents_gps_pending[AGENTS_N];

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
    agents_on_inbound(g_agents[idx].id, line);
    event_add("agent %s gps %s", g_agents[idx].id, fresh ? "located" : "pending");
    return fresh;
}

/* Match in agents_send. Returns true when the message was consumed locally. */
static bool agents_gps_intercept(int idx, const char *text) {
    if (!agents_wants_gps(text)) return false;
    agents_push_line(idx, true, text);        /* keep the query in the thread */
    bool fresh = agents_loc_card(idx);
    g_agents_gps_pending[idx] = !fresh;
    return true;
}

/* gps.cpp on-fix hook: resolve every pending "where are you?" agent. Cheap
 * no-op when nothing is pending (runs once per RMC 'A', ~1/s). */
static void agents_gps_on_fix(void) {
    for (int i = 0; i < AGENTS_N; i++) {
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
    if (idx >= 0) g_agents_gps_pending[idx] = true;
}

/* Public: send a line as the user, into the agent's ACTIVE session.
 * Path: local thread/history → WiFi bridge if up → else MeshCore C1 uplink.
 * Codex owns a dedicated gateway inbox and therefore always uses C1; sending
 * it to the legacy /v1/chat bridge while WiFi is up would bypass Codex.
 * Downlink reply uses the same C1 (or WiFi /agents/inbound). One chat loop. */
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

    bool mesh_owned = strcmp(agent_id, "codex") == 0;
    bool wifi_ok = mesh_owned
        ? false
        : agents_bridge_post(agent_id, agents_active_session(idx), cleaned);
    bool mesh_ok = false;
    if ((mesh_owned || !wifi_ok) && g_agents_mesh_uplink) {
        mesh_ok = g_agents_mesh_uplink(agent_id, cleaned);
        if (mesh_ok) event_add("agent %s mesh uplink", agent_id);
    }
    if (!wifi_ok && !mesh_ok) {
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

/* Inject an agent reply into the active session of the agent (long texts split). */
static void agents_on_inbound(const char *agent_id, const char *text) {
    int idx = agents_find(agent_id);
    if (idx < 0 || !text || !text[0]) return;
    char cleaned[2048];
    agents_clean_text(text, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return;
    agents_push_line(idx, false, cleaned);
    display_force = true;
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
        AgentSlot &a = g_agents[i];
        if (all) {
            for (int j = 0; j < a.n_sessions; j++) {
                if (agents_store_ready()) {
                    String path = agents_log_path(a.id, a.sessions[j]);
                    g_store->remove(path.c_str());
                }
                a.session_lines[j] = 0;
            }
            a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
            agents_push_line(i, false, "chat cleared - type to talk");
        } else {
            if (agents_store_ready()) {
                String path = agents_log_path(a.id, a.sessions[a.active_idx]);
                g_store->remove(path.c_str());
            }
            a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
            a.session_lines[a.active_idx] = 0;
            agents_push_line(i, false, "chat cleared - type to talk");
        }
        any = true;
    };
    if (all) {
        for (int i = 0; i < AGENTS_N; i++) clear_agent(i);
    } else {
        int idx = agents_find(agent_id);
        if (idx >= 0) clear_agent(idx);
    }
    agents_unlock();
    display_force = true;
    return any;
}

static int agents_count() { return AGENTS_N; }

static const char *agents_id(int i) {
    return (i >= 0 && i < AGENTS_N) ? g_agents[i].id : "";
}
static const char *agents_name(int i) {
    return (i >= 0 && i < AGENTS_N) ? g_agents[i].name : "";
}
static bool agents_bridge_ok() { return g_bridge[0] != '\0'; }
static const char *agents_bridge_url() { return g_bridge; }

static const char *agents_describe() {
    return
        "# agents\n\n"
        "Pocket chat with Grok / Claude / Hermes / OpenCode / Codex.\n"
        "Uplink: WiFi bridge /v1/chat, else MeshCore C1|agent|…|u|… private DM\n"
        "  (agent = grok|claude|hermes|opencode|codex).\n"
        "Downlink: same C1 side=a (or WiFi /agents/inbound) — one loop.\n"
        "A \"where are you?\" / \"где ты\" message is answered locally with the\n"
        "GNSS fix (POST /gps/fix semantics) and never hits the bridge.\n\n"
        "History: append-only JSONL on SD (fallback SPIFFS), keyed by\n"
        "(agent, session); only a 24-message viewport is kept in RAM.\n\n"
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
        for (int i = 0; i < AGENTS_N; i++) {
            AgentSlot &a = g_agents[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"] = a.id;
            o["name"] = a.name;
            o["messages"] = a.lines;
            o["active"] = a.sessions[a.active_idx];
            JsonArray sess = o["sessions"].to<JsonArray>();
            JsonArray smsg = o["session_msgs"].to<JsonArray>();
            for (int j = 0; j < a.n_sessions; j++) {
                sess.add(a.sessions[j]);
                smsg.add(a.session_lines[j]);
            }
            if (a.vn > 0) {
                o["last"] = a.view[a.vn - 1].text;
                o["last_from_me"] = a.view[a.vn - 1].from_me;
                o["last_ts"] = a.view[a.vn - 1].ts;
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
        if (idx < 0) { notify_send_error(req, 400, "agent must be grok, claude, hermes, opencode or codex"); return; }
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
        if (agents_find(agent) < 0) { notify_send_error(req, 400, "agent must be grok, claude, hermes, opencode or codex"); return; }
        if (!text[0]) { notify_send_error(req, 400, "text required"); return; }
        agents_on_inbound(agent, text);
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
        if (idx < 0) { notify_send_error(req, 400, "agent must be grok, claude, hermes, opencode or codex"); return; }
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
        if (idx < 0) { notify_send_error(req, 400, "agent must be grok, claude, hermes, opencode or codex"); return; }
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

    for (int i = 0; i < AGENTS_N; i++) {
        AgentSlot &a = g_agents[i];
        a.active_idx = 0; a.file_sync = 0; a.lines = 0; a.vn = 0; a.win_start = 0;
        for (int j = 0; j < AGENT_SESSIONS_MAX; j++) a.session_lines[j] = 0;
        agents_session_add(i, g_session);               // default always present
    }
    agents_manifest_load();                             // existing sessions
    // Guarantee at least the default survives a reboot.
    agents_manifest_persist();

    for (int i = 0; i < AGENTS_N; i++) {
        agents_sync_view(i);
        char intro[AGENT_TEXT_LEN];
        snprintf(intro, sizeof(intro), "hi - type to talk to %s", g_agents[i].name);
        if (g_agents[i].lines == 0)
            agents_push_line(i, false, intro);
        agents_thread_goto_tail(i);
    }

    skill_register(&agents_skill);
    Serial.printf("[agents] session=%s bridge=%s store=%s/%s\n",
                  g_session, g_bridge[0] ? g_bridge : "(none)",
                  agents_store_name(), agents_active_session(0));
}
