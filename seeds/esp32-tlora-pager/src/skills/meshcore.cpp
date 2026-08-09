/*
 * skills/meshcore.cpp — MeshCore private-link transport into notify
 *
 * Home Heltec gateway (daemon :8325) sends:
 *   P1|level|source|title|body[|key]
 * as a MeshCore private DM (never Public). `key` is the card's client id and is
 * optional; see notify_ingest_p1() for how it is told apart from a body that is
 * allowed to contain separators of its own.
 *
 * The pager answers on the same link with one frame:
 *   R1|key|reply
 * built in main.cpp (reply_upstream_mesh) when WiFi cannot reach the gateway.
 * M1 carries no key: its last field is an arbitrary chunk, so a key sitting
 * between the title and the chunk could not be told from the chunk itself.
 *
 * Pair identity (Ed25519) lives on SPIFFS:
 *   /mesh_identity.id  — pub(32)+prv(64) MeshCore LocalIdentity blob
 *   /mesh_pair.json    — public meta for status UI
 *   /mesh_probe_s.txt  — sparse keepalive interval seconds (default 900)
 *
 * Clock face "M" glyph = private-path health (not a beacon flood):
 *   - outbound: rare private DM "MC|k" to Heltec, MeshCore ACK is enough
 *   - multi-hop: MeshCore stores path after first contact; ACKs ride it
 *   - inbound P1 from gateway also marks the link OK
 *
 * Radio: SX1262 + BaseChatMesh in src/mesh/ (private DM only).
 */

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <string.h>
#include <stdio.h>
#include "mesh/mc_client.h"
#include "mesh/utf8_chunk.h"

#define MESH_ID_PATH       "/mesh_identity.id"
#define MESH_PAIR_PATH     "/mesh_pair.json"
#define MESH_PROBE_PATH    "/mesh_probe_s.txt"
#define MESH_PUB_LEN       32
#define MESH_PRV_LEN       64
#define MESH_ID_BLOB_LEN   (MESH_PUB_LEN + MESH_PRV_LEN)
/* Sparse keepalive — do not spam the mesh. Override via SPIFFS. */
#define MESH_PROBE_DEFAULT_S   300u   /* 5 min — counter stays honest */
#define MESH_PROBE_MIN_S       120u   /* 2 min floor */
#define MESH_PROBE_MAX_S       7200u  /* 2 h cap */
/* After this many failed probes (or silence), icon → DOWN. */
#define MESH_FAIL_DOWN         2
/* Wire keepalive: daemon silent bot; optional app pong MC|a for alive counter. */
#define MESH_KEEPALIVE_TEXT    "MC|k"
#define MESH_ALIVE_PONG_TEXT   "MC|a"

struct MeshPairInfo {
    bool     has_identity;
    bool     has_meta;
    bool     radio_ready;      /* BaseChatMesh up — probe can TX */
    char     name[16];
    char     public_key_hex[65];
    char     heltec_pk_hex[65];
    float    freq;
    uint8_t  sf;
    float    bw;
    char     radio_state[24];
    uint32_t dm_rx_count;
    uint32_t last_dm_ms;
    /* Sparse private-path health (clock "M") */
    uint32_t probe_interval_s;
    uint32_t last_ok_ms;       /* last ACK or inbound from GW */
    uint32_t last_probe_ms;    /* last outbound attempt */
    uint32_t last_rtt_ms;      /* 0 = unknown */
    uint8_t  fail_streak;
    uint32_t probe_ok_count;
    uint32_t probe_fail_count;
};

static MeshPairInfo g_mesh;

static void mesh_hex_of(const uint8_t *bin, size_t n, char *out, size_t out_n) {
    if (!out || out_n < n * 2 + 1) return;
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[(bin[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[bin[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static void mesh_pair_clear() {
    memset(&g_mesh, 0, sizeof(g_mesh));
    snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "idle");
    g_mesh.freq = 869.618f;
    g_mesh.sf = 8;
    g_mesh.bw = 62.5f;
    g_mesh.probe_interval_s = MESH_PROBE_DEFAULT_S;
}

static void mesh_load_probe_interval() {
    g_mesh.probe_interval_s = MESH_PROBE_DEFAULT_S;
    if (!SPIFFS.exists(MESH_PROBE_PATH)) return;
    File f = SPIFFS.open(MESH_PROBE_PATH, "r");
    if (!f) return;
    long v = f.parseInt();
    f.close();
    if (v < (long)MESH_PROBE_MIN_S) v = MESH_PROBE_MIN_S;
    if (v > (long)MESH_PROBE_MAX_S) v = MESH_PROBE_MAX_S;
    g_mesh.probe_interval_s = (uint32_t)v;
}

/* Mark private path alive (MeshCore ACK or inbound DM from gateway). */
static void mesh_link_mark_ok(uint32_t rtt_ms) {
    g_mesh.last_ok_ms = millis();
    g_mesh.last_rtt_ms = rtt_ms;
    g_mesh.fail_streak = 0;
    if (rtt_ms) g_mesh.probe_ok_count++;
}

/* Seconds since last private-path alive; -1 if never. For clock "M12m". */
static int mesh_alive_age_s() {
    if (!g_mesh.has_identity || g_mesh.last_ok_ms == 0) return -1;
    return (int)((millis() - g_mesh.last_ok_ms) / 1000UL);
}

/* Clock chrome: see HwMeshUi in hw_ui.h */
static int mesh_ui_state() {
    if (!g_mesh.has_identity) return MESH_UI_OFF;
    if (!g_mesh.radio_ready) {
        /* Keys on board, stack not TX/RX yet — dim M, not a false green. */
        if (g_mesh.last_ok_ms == 0) return MESH_UI_WAIT;
    }
    if (g_mesh.last_ok_ms == 0) {
        return g_mesh.fail_streak >= MESH_FAIL_DOWN ? MESH_UI_DOWN : MESH_UI_WAIT;
    }
    unsigned long age = millis() - g_mesh.last_ok_ms;
    unsigned long stale_after = (unsigned long)g_mesh.probe_interval_s * 1000UL * 2UL;
    unsigned long down_after  = (unsigned long)g_mesh.probe_interval_s * 1000UL * 4UL;
    if (g_mesh.fail_streak >= MESH_FAIL_DOWN && age > stale_after)
        return MESH_UI_DOWN;
    if (age > down_after) return MESH_UI_DOWN;
    if (age > stale_after) return MESH_UI_STALE;
    return MESH_UI_OK;
}

/* Pending probe: send returns true; ACK callback sets last_ok. */
static bool mesh_probe_awaiting_ack = false;
static uint32_t mesh_on_private_text(const char *text);  /* fwd */

/*
 * One sparse private DM to Heltec. MeshCore path (incl. repeaters) is used
 * when the contact has out_path; protocol ACK is enough — no chatty reply.
 * Returns true if stack accepted TX (ACK may arrive later via callback).
 */
static bool mesh_probe_gateway(uint32_t *rtt_ms_out) {
    if (rtt_ms_out) *rtt_ms_out = 0;
    if (!g_mesh.has_identity || !g_mesh.heltec_pk_hex[0]) return false;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return false;
    uint32_t ack = 0, est = 0;
    if (!mesh_client_send_to_gateway(MESH_KEEPALIVE_TEXT, &ack, &est))
        return false;
    mesh_probe_awaiting_ack = true;
    /* RTT filled asynchronously in mesh_client on_ack callback. */
    return true;
}

static void mesh_cb_dm(const char *from_name, const char *text) {
    (void)from_name;
    if (!text) return;
    /* App-level alive pong from gateway (or echo) — refresh M counter, no card. */
    if (strcmp(text, "K") == 0 || strcmp(text, MESH_KEEPALIVE_TEXT) == 0 ||
        strcmp(text, MESH_ALIVE_PONG_TEXT) == 0 ||
        strncmp(text, "MC|k", 4) == 0 || strncmp(text, "MC|a", 4) == 0) {
        mesh_link_mark_ok(1);
        event_add("mesh alive pong");
        return;
    }
    mesh_on_private_text(text);
}

static void mesh_cb_ack(uint32_t rtt_ms) {
    mesh_probe_awaiting_ack = false;
    mesh_link_mark_ok(rtt_ms ? rtt_ms : 1);
    event_add("mesh ACK rtt=%lu", (unsigned long)rtt_ms);
}

static void mesh_cb_log(const char *msg) {
    if (msg && msg[0]) event_add("mesh %s", msg);
}

static bool mesh_load_identity() {
    g_mesh.has_identity = false;
    g_mesh.public_key_hex[0] = '\0';
    if (!SPIFFS.exists(MESH_ID_PATH)) return false;
    File f = SPIFFS.open(MESH_ID_PATH, "r");
    if (!f) return false;
    uint8_t blob[MESH_ID_BLOB_LEN];
    size_t n = f.read(blob, sizeof(blob));
    f.close();
    if (n != MESH_ID_BLOB_LEN) {
        event_add("mesh identity size %u (want %u)",
                  (unsigned)n, (unsigned)MESH_ID_BLOB_LEN);
        return false;
    }
    mesh_hex_of(blob, MESH_PUB_LEN, g_mesh.public_key_hex, sizeof(g_mesh.public_key_hex));
    snprintf(g_mesh.name, sizeof(g_mesh.name), "%.8s", g_mesh.public_key_hex);
    for (int i = 0; g_mesh.name[i]; i++) {
        if (g_mesh.name[i] >= 'a' && g_mesh.name[i] <= 'f')
            g_mesh.name[i] = (char)(g_mesh.name[i] - 'a' + 'A');
    }
    g_mesh.has_identity = true;
    snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "keys_ok");
    return true;
}

static bool mesh_load_meta() {
    g_mesh.has_meta = false;
    if (!SPIFFS.exists(MESH_PAIR_PATH)) return false;
    File f = SPIFFS.open(MESH_PAIR_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    g_mesh.has_meta = true;
    if (doc["name"].is<const char *>()) {
        snprintf(g_mesh.name, sizeof(g_mesh.name), "%s", doc["name"].as<const char *>());
    }
    if (doc["public_key"].is<const char *>() && !g_mesh.has_identity) {
        snprintf(g_mesh.public_key_hex, sizeof(g_mesh.public_key_hex),
                 "%s", doc["public_key"].as<const char *>());
    }
    if (doc["heltec_public_key"].is<const char *>()) {
        snprintf(g_mesh.heltec_pk_hex, sizeof(g_mesh.heltec_pk_hex),
                 "%s", doc["heltec_public_key"].as<const char *>());
    }
    if (doc["freq"].is<float>()) g_mesh.freq = doc["freq"].as<float>();
    if (doc["sf"].is<int>()) g_mesh.sf = (uint8_t)doc["sf"].as<int>();
    if (doc["bw"].is<float>()) g_mesh.bw = doc["bw"].as<float>();
    return true;
}

/* ---- Multi-part reassembly (M1 notify / C1 chat) ----------------------------
 * Wire (gateway meshcore_daemon.encode_mesh_frames):
 *   P1|level|source|title|body
 *   M1|mid|i|n|level|source|title|chunk
 *   C1|agent|mid|i|n|side|chunk   side=a|u
 * Truncate is per-frame; append is here until i==n, then push to inbox/thread.
 */
#define MESH_REASM_SLOTS   4
#define MESH_REASM_MAX    1800   /* cap assembled body (notify 240 + chat larger) */
#define MESH_REASM_TTL_MS 45000

struct MeshReasm {
    bool     used;
    char     kind;           /* 'M' or 'C' */
    char     mid[8];
    char     agent[16];
    char     level[8];
    char     source[NOTIFY_SOURCE_LEN];
    char     title[NOTIFY_TITLE_LEN];
    char     side;           /* 'a' or 'u' for C1 */
    uint8_t  n_parts;
    uint8_t  got_mask;       /* bit i-1 set when part i received (max 8 parts in mask;
                                for >8 parts we still append in order if sequential) */
    uint8_t  got_count;
    uint16_t len;
    uint32_t started_ms;
    char     buf[MESH_REASM_MAX];
};

static MeshReasm g_reasm[MESH_REASM_SLOTS];

static void mesh_reasm_gc() {
    uint32_t now = millis();
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (g_reasm[i].used && (now - g_reasm[i].started_ms) > MESH_REASM_TTL_MS)
            g_reasm[i].used = false;
    }
}

static MeshReasm *mesh_reasm_get(char kind, const char *mid) {
    mesh_reasm_gc();
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (g_reasm[i].used && g_reasm[i].kind == kind &&
            strcmp(g_reasm[i].mid, mid) == 0)
            return &g_reasm[i];
    }
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (!g_reasm[i].used) {
            memset(&g_reasm[i], 0, sizeof(g_reasm[i]));
            g_reasm[i].used = true;
            g_reasm[i].kind = kind;
            snprintf(g_reasm[i].mid, sizeof(g_reasm[i].mid), "%s", mid);
            g_reasm[i].started_ms = millis();
            return &g_reasm[i];
        }
    }
    /* steal oldest */
    int victim = 0;
    for (int i = 1; i < MESH_REASM_SLOTS; i++)
        if (g_reasm[i].started_ms < g_reasm[victim].started_ms) victim = i;
    memset(&g_reasm[victim], 0, sizeof(g_reasm[victim]));
    g_reasm[victim].used = true;
    g_reasm[victim].kind = kind;
    snprintf(g_reasm[victim].mid, sizeof(g_reasm[victim].mid), "%s", mid);
    g_reasm[victim].started_ms = millis();
    return &g_reasm[victim];
}

static void mesh_reasm_append(MeshReasm *r, uint8_t part_i, uint8_t n_parts,
                              const char *chunk) {
    if (!r || !chunk) return;
    if (n_parts == 0 || n_parts > 64 || part_i == 0 || part_i > n_parts) return;
    if (r->n_parts == 0) r->n_parts = n_parts;
    /* sequential append: only accept next expected part (got_count+1) to keep
     * order without a per-part buffer — gateway sends in order with gaps. */
    if (part_i != (uint8_t)(r->got_count + 1)) {
        if (part_i <= r->got_count) return;  // dup
        /* gap: still append if we can (best effort) */
    }
    size_t cl = strlen(chunk);
    size_t room = (size_t)(MESH_REASM_MAX - 1 - r->len);
    if (cl > room) cl = room;
    if (cl > 0) {
        memcpy(r->buf + r->len, chunk, cl);
        r->len = (uint16_t)(r->len + cl);
        r->buf[r->len] = '\0';
    }
    if (part_i > r->got_count) r->got_count = part_i;
}

/* Called when MeshCore stack delivers a private text.
 * Also used by POST /mesh/inject for dry-run of the notify path. */
static uint32_t mesh_on_private_text(const char *text) {
    if (!text || !text[0]) return 0;
    uint32_t id = 0;

    if (strncmp(text, "P1|", 3) == 0) {
        id = notify_ingest_p1(text);
    } else if (strncmp(text, "M1|", 3) == 0) {
        /* M1|mid|i|n|level|source|title|chunk */
        char mid[8] = {0}, level[8] = {0}, source[NOTIFY_SOURCE_LEN] = {0};
        char title[NOTIFY_TITLE_LEN] = {0};
        int part_i = 0, n_parts = 0;
        const char *p = text + 3;
        const char *a = strchr(p, '|');
        if (!a) return 0;
        size_t n = (size_t)(a - p);
        if (n >= sizeof(mid)) n = sizeof(mid) - 1;
        memcpy(mid, p, n);
        p = a + 1;
        part_i = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        n_parts = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        a = strchr(p, '|');
        if (!a) return 0;
        n = (size_t)(a - p);
        if (n >= sizeof(level)) n = sizeof(level) - 1;
        memcpy(level, p, n);
        p = a + 1;
        a = strchr(p, '|');
        if (!a) return 0;
        n = (size_t)(a - p);
        if (n >= sizeof(source)) n = sizeof(source) - 1;
        memcpy(source, p, n);
        p = a + 1;
        a = strchr(p, '|');
        if (!a) return 0;
        n = (size_t)(a - p);
        if (n >= sizeof(title)) n = sizeof(title) - 1;
        memcpy(title, p, n);
        const char *chunk = a + 1;

        MeshReasm *r = mesh_reasm_get('M', mid);
        if (r->got_count == 0) {
            snprintf(r->level, sizeof(r->level), "%s", level);
            snprintf(r->source, sizeof(r->source), "%s", source);
            snprintf(r->title, sizeof(r->title), "%s", title);
        }
        mesh_reasm_append(r, (uint8_t)part_i, (uint8_t)n_parts, chunk);
        if (r->got_count >= r->n_parts && r->n_parts > 0) {
            id = notify_ingest(r->level, r->source, r->title, r->buf, NULL);
            r->used = false;
        }
    } else if (strncmp(text, "C1|", 3) == 0) {
        /* C1|agent|mid|i|n|side|chunk */
        char agent[16] = {0}, mid[8] = {0};
        int part_i = 0, n_parts = 0;
        char side = 'a';
        const char *p = text + 3;
        const char *a = strchr(p, '|');
        if (!a) return 0;
        size_t n = (size_t)(a - p);
        if (n >= sizeof(agent)) n = sizeof(agent) - 1;
        memcpy(agent, p, n);
        p = a + 1;
        a = strchr(p, '|');
        if (!a) return 0;
        n = (size_t)(a - p);
        if (n >= sizeof(mid)) n = sizeof(mid) - 1;
        memcpy(mid, p, n);
        p = a + 1;
        part_i = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        n_parts = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        if (*p) side = *p;
        a = strchr(p, '|');
        if (!a) return 0;
        const char *chunk = a + 1;

        MeshReasm *r = mesh_reasm_get('C', mid);
        if (r->got_count == 0) {
            snprintf(r->agent, sizeof(r->agent), "%s", agent);
            r->side = side;
        }
        mesh_reasm_append(r, (uint8_t)part_i, (uint8_t)n_parts, chunk);
        if (r->got_count >= r->n_parts && r->n_parts > 0) {
            int idx = agents_find(r->agent);
            if (idx >= 0) {
                bool from_me = (r->side == 'u');
                agents_push_line(idx, from_me, r->buf);
                display_force = true;
                id = 1;  /* non-zero = handled */
            } else {
                /* unknown agent → notify card */
                id = notify_ingest("info", r->agent, "CHAT", r->buf, NULL);
            }
            r->used = false;
        }
    } else {
        id = notify_ingest("info", "mesh", "MESH", text, NULL);
    }

    if (id) {
        g_mesh.dm_rx_count++;
        g_mesh.last_dm_ms = millis();
        mesh_link_mark_ok(0);
    }
    return id;
}

static void mesh_status_json(JsonDocument &doc) {
    doc["ok"] = true;
    doc["paired"] = g_mesh.has_identity;
    doc["has_meta"] = g_mesh.has_meta;
    doc["name"] = g_mesh.name;
    doc["public_key"] = g_mesh.public_key_hex;
    if (g_mesh.has_identity && g_mesh.public_key_hex[0]) {
        char pk8[9];
        memcpy(pk8, g_mesh.public_key_hex, 8);
        pk8[8] = '\0';
        doc["public_key8"] = pk8;
    } else {
        doc["public_key8"] = "";
    }
    if (g_mesh.heltec_pk_hex[0]) {
        char h8[9];
        memcpy(h8, g_mesh.heltec_pk_hex, 8);
        h8[8] = '\0';
        doc["heltec_pk8"] = h8;
    } else {
        doc["heltec_pk8"] = "";
    }
    doc["freq"] = g_mesh.freq;
    doc["sf"] = g_mesh.sf;
    doc["bw"] = g_mesh.bw;
    doc["radio"] = g_mesh.radio_state;
    doc["radio_ready"] = g_mesh.radio_ready;
    doc["dm_rx"] = g_mesh.dm_rx_count;
    doc["policy"] = "private_dm_only";
    doc["link"] = mesh_ui_state();
    doc["probe_interval_s"] = g_mesh.probe_interval_s;
    doc["probe_ok"] = g_mesh.probe_ok_count;
    doc["probe_fail"] = g_mesh.probe_fail_count;
    doc["last_rtt_ms"] = g_mesh.last_rtt_ms;
    doc["fail_streak"] = g_mesh.fail_streak;
    if (g_mesh.last_ok_ms)
        doc["last_ok_age_s"] = (millis() - g_mesh.last_ok_ms) / 1000UL;
    else
        doc["last_ok_age_s"] = -1;
    doc["keepalive"] = MESH_KEEPALIVE_TEXT;
    doc["note"] = g_mesh.has_identity
                      ? (g_mesh.radio_ready
                             ? "sparse private MC|k probe + P1 notify"
                             : "keys ready; sparse probe waits for radio stack")
                      : "need /mesh_identity.id on SPIFFS";
}

/* Fill short status lines for MESHCORE → STATUS screen (caller owns buffers). */
static int mesh_status_lines(char lines[][40], int max_lines) {
    int n = 0;
    auto push = [&](const char *s) {
        if (n >= max_lines) return;
        snprintf(lines[n], 40, "%s", s);
        n++;
    };
    char buf[40];
    if (g_mesh.has_identity) {
        snprintf(buf, sizeof(buf), "NODE %s", g_mesh.name);
        push(buf);
        snprintf(buf, sizeof(buf), "PK %.12s…", g_mesh.public_key_hex);
        push(buf);
    } else {
        push("NO IDENTITY");
        push("flash SPIFFS keys");
    }
    if (g_mesh.heltec_pk_hex[0]) {
        snprintf(buf, sizeof(buf), "GW %.8s…", g_mesh.heltec_pk_hex);
        push(buf);
    }
    snprintf(buf, sizeof(buf), "RF %.3f SF%u", (double)g_mesh.freq, (unsigned)g_mesh.sf);
    push(buf);
    snprintf(buf, sizeof(buf), "RADIO %s", g_mesh.radio_state);
    push(buf);
    {
        const char *lk = "?";
        switch (mesh_ui_state()) {
            case MESH_UI_OFF: lk = "off"; break;
            case MESH_UI_WAIT: lk = "wait"; break;
            case MESH_UI_OK: lk = "OK"; break;
            case MESH_UI_STALE: lk = "stale"; break;
            case MESH_UI_DOWN: lk = "DOWN"; break;
        }
        snprintf(buf, sizeof(buf), "LINK %s", lk);
        push(buf);
    }
    snprintf(buf, sizeof(buf), "PROBE every %lus",
             (unsigned long)g_mesh.probe_interval_s);
    push(buf);
    if (g_mesh.last_rtt_ms)
        snprintf(buf, sizeof(buf), "RTT %lu ms", (unsigned long)g_mesh.last_rtt_ms);
    else
        snprintf(buf, sizeof(buf), "RTT --");
    push(buf);
    snprintf(buf, sizeof(buf), "DM RX %lu", (unsigned long)g_mesh.dm_rx_count);
    push(buf);
    push("private DM only");
    return n;
}

static void skill_meshcore_poll() {
    /* Deferred radio start (~5s after boot): seed UI/WiFi already running. */
    if (g_mesh.has_identity && !g_mesh.radio_ready &&
        strcmp(g_mesh.radio_state, "radio_fail") != 0 &&
        millis() > 5000UL) {
        static bool tried = false;
        if (!tried) {
            tried = true;
            if (mesh_client_begin()) {
                g_mesh.radio_ready = true;
                snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "rx_on");
                event_add("mesh radio RX/TX up");
            } else {
                snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "radio_fail");
                event_add("mesh radio init FAILED (UI stays up)");
            }
        }
    }

    /* MeshCore stack — same loop task as display (shared SPI). */
    if (g_mesh.radio_ready) mesh_client_loop();

    /* Sparse private keepalive — only when radio stack can TX. */
    if (!g_mesh.has_identity || !g_mesh.radio_ready) return;
    if (!g_mesh.heltec_pk_hex[0]) return;

    unsigned long now = millis();
    unsigned long interval_ms = (unsigned long)g_mesh.probe_interval_s * 1000UL;
    if (g_mesh.last_probe_ms != 0 &&
        (now - g_mesh.last_probe_ms) < interval_ms) {
        return;
    }
    /* First probe ~20s after boot (radio settle + Heltec hear advert). */
    if (g_mesh.last_probe_ms == 0 && now < 20000UL) return;

    g_mesh.last_probe_ms = now;
    uint32_t rtt = 0;
    bool sent = mesh_probe_gateway(&rtt);
    if (!sent) {
        g_mesh.fail_streak++;
        g_mesh.probe_fail_count++;
        if (g_mesh.fail_streak == MESH_FAIL_DOWN)
            event_add("mesh probe DOWN (fail x%u)", (unsigned)g_mesh.fail_streak);
    } else {
        event_add("mesh probe TX MC|k");
        /* Success counted when ACK arrives (mesh_cb_ack). Timeout = fail. */
    }
    /* If previous probe never ACKed, count as fail once next probe fires. */
    static bool had_pending = false;
    if (had_pending && mesh_probe_awaiting_ack) {
        mesh_probe_awaiting_ack = false;
        g_mesh.fail_streak++;
        g_mesh.probe_fail_count++;
    }
    had_pending = sent;
}

static const SkillEndpoint meshcore_endpoints[] = {
    {"GET",  "/mesh/status", "MeshCore pair + sparse link health"},
    {"POST", "/mesh/inject", "Dev: inject P1|… as if private DM arrived"},
    {NULL, NULL, NULL}
};

static const char *meshcore_describe() {
    return "## Skill: meshcore\n\n"
           "Private MeshCore link to home Heltec gateway.\n"
           "Notify: `P1|level|source|title|body[|key]` → same cards as WiFi `/notify`.\n"
           "Reply: card reply goes up as `R1|key|reply` when WiFi is down.\n"
           "Clock `M`: sparse private keepalive `MC|k` (default every 15 min),\n"
           "MeshCore ACK via stored path/repeaters — never Public flood.\n"
           "SPIFFS: `/mesh_identity.id`, `/mesh_pair.json`, `/mesh_probe_s.txt`.\n";
}

static void meshcore_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/mesh/status"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        mesh_status_json(doc);
        notify_send_json(req, 200, doc);
    });

    server.on(AsyncURIMatcher::exact("/mesh/inject"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body || !body[0]) {
            free(body);
            notify_send_error(req, 400, "body required (P1|… or JSON text)");
            return;
        }
        char wire_buf[320];
        wire_buf[0] = '\0';
        if (body[0] == '{') {
            JsonDocument doc;
            if (deserializeJson(doc, body) != DeserializationError::Ok) {
                free(body);
                notify_send_error(req, 400, "bad json");
                return;
            }
            const char *t = nullptr;
            if (doc["text"].is<const char *>()) t = doc["text"].as<const char *>();
            else if (doc["body"].is<const char *>()) t = doc["body"].as<const char *>();
            free(body);
            if (!t || !t[0]) {
                notify_send_error(req, 400, "need text or body field");
                return;
            }
            snprintf(wire_buf, sizeof(wire_buf), "%s", t);
        } else {
            snprintf(wire_buf, sizeof(wire_buf), "%s", body);
            free(body);
        }
        uint32_t id = mesh_on_private_text(wire_buf);
        JsonDocument out;
        out["ok"] = id != 0;
        out["id"] = id;
        out["dm_rx"] = g_mesh.dm_rx_count;
        notify_send_json(req, id ? 200 : 400, out);
    }, NULL, handle_body_collect);
}

static const Skill meshcore_skill = {
    .name = "meshcore",
    .version = "0.1.0",
    .describe = meshcore_describe,
    .endpoints = meshcore_endpoints,
    .register_routes = meshcore_register_routes,
    .tick = skill_meshcore_poll
};

/* Encode user chat as C1 frames and TX private DM to Heltec (same wire as downlink). */
static bool mesh_chat_uplink(const char *agent_id, const char *text) {
    if (!agent_id || !text || !text[0]) return false;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return false;

    char agent[13];
    snprintf(agent, sizeof(agent), "%.12s", agent_id);
    char mid[8];
    snprintf(mid, sizeof(mid), "%04x", (unsigned)(millis() & 0xFFFF));

    const uint8_t *raw = (const uint8_t *)text;
    size_t raw_len = strlen(text);
    /* Per-frame budget ~150 bytes total (MeshCore MTU). */
    const int limit = 150;
    char hdr_sample[48];
    snprintf(hdr_sample, sizeof(hdr_sample), "C1|%s|%s|99|99|u|", agent, mid);
    int room = limit - (int)strlen(hdr_sample);
    if (room < 16) room = 16;

    /* Count parts */
    int n_parts = 0;
    size_t i = 0;
    while (i < raw_len) {
        size_t take = mesh_utf8_chunk_len(raw, raw_len, i, (size_t)room);
        if (take == 0) return false;
        n_parts++;
        i += take;
        if (n_parts > 32) break;
    }
    if (n_parts == 0) n_parts = 1;

    i = 0;
    int part = 0;
    while (i < raw_len && part < n_parts) {
        size_t take = mesh_utf8_chunk_len(raw, raw_len, i, (size_t)room);
        if (take == 0) return false;
        part++;
        char frame[160];
        int n = snprintf(frame, sizeof(frame), "C1|%s|%s|%d|%d|u|", agent, mid, part,
                         n_parts);
        if (n < 0 || n >= (int)sizeof(frame)) return false;
        size_t room2 = sizeof(frame) - 1U - (size_t)n;
        if (take > room2) return false;
        memcpy(frame + n, text + i, take);
        frame[n + take] = '\0';
        uint32_t ack = 0, est = 0;
        if (!mesh_client_send_to_gateway(frame, &ack, &est)) return false;
        i += take;
        if (i < raw_len) delay(350);
    }
    return part > 0;
}

static void skill_meshcore_init() {
    mesh_pair_clear();
    mesh_load_identity();
    mesh_load_meta();
    mesh_load_probe_interval();
    g_mesh.radio_ready = false;

    mesh_client_set_callbacks(mesh_cb_dm, mesh_cb_ack, mesh_cb_log);
    agents_set_mesh_uplink(mesh_chat_uplink);

    if (g_mesh.has_identity) {
        /* Defer radio bring-up to poll() so WiFi/HTTP always come up first.
         * A hard radio fail must not brick the seed UI after OTA. */
        snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "radio_deferred");
        event_add("mesh pair %s probe %lus (radio deferred)",
                  g_mesh.name, (unsigned long)g_mesh.probe_interval_s);
    } else {
        snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "no_keys");
        event_add("mesh: no identity on SPIFFS");
    }
    skill_register(&meshcore_skill);
    Serial.printf("[meshcore] paired=%d name=%s probe=%lus radio=%s\n",
                  (int)g_mesh.has_identity, g_mesh.name,
                  (unsigned long)g_mesh.probe_interval_s, g_mesh.radio_state);
}
