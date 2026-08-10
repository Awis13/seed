/*
 * skills/agents.cpp — pocket chat with remote agents (Grok / Claude / Hermes)
 *
 * Pager is the thin terminal. A bridge on the LAN does the real work:
 *   POST {bridge}/v1/chat  { "agent","session","text" }
 * Answers come back as ordinary /notify (source=grok|claude|hermes) or, when
 * the bridge is missing, as a local stub line in the thread.
 *
 * SPIFFS:
 *   /agent_bridge.txt  — base URL, e.g. http://192.168.1.138:8090  (one line)
 *
 * HTTP API on the seed:
 *   GET  /agents           — list + bridge status + last lines
 *   POST /agents/send      — { "agent":"hermes", "text":"…" }
 *   POST /agents/bridge    — raw text body = bridge URL (empty clears)
 */

#define AGENTS_N            3
#define AGENT_ID_LEN        12
#define AGENT_NAME_LEN      16
/* Long bot / mesh multi-part: bigger ring, UTF-8 safe split still applies.
 * 0.9.22: 512 * 40 * 3 ≈ 60 KB static — fine on S3 with 8 MB PSRAM free for other. */
#define AGENT_TEXT_LEN      512
#define AGENT_THREAD_MAX    40
#define AGENT_BRIDGE_LEN    96
#define AGENT_BRIDGE_FILE   "/agent_bridge.txt"
#define AGENT_SESSION_LEN   24

struct AgentLine {
    bool from_me;
    char text[AGENT_TEXT_LEN];
};

struct AgentSlot {
    const char *id;     // "grok" / "claude" / "hermes"
    const char *name;   // UI label
    AgentLine thread[AGENT_THREAD_MAX];
    uint8_t n;          // 0..AGENT_THREAD_MAX
    uint8_t head;       // next write (ring)
};

static AgentSlot g_agents[AGENTS_N] = {
    { "grok",   "GROK",   {}, 0, 0 },
    { "claude", "CLAUDE", {}, 0, 0 },
    { "hermes", "HERMES", {}, 0, 0 },
};

static char g_bridge[AGENT_BRIDGE_LEN] = "";
static char g_session[AGENT_SESSION_LEN] = "pager";
static portMUX_TYPE agents_mux = portMUX_INITIALIZER_UNLOCKED;

static int agents_find(const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < AGENTS_N; i++) {
        if (strcmp(g_agents[i].id, id) == 0) return i;
    }
    return -1;
}

/* Push one chunk (already <= AGENT_TEXT_LEN-1). Caller holds no lock. */
static void agents_push_one(int idx, bool from_me, const char *chunk) {
    AgentSlot &a = g_agents[idx];
    AgentLine &L = a.thread[a.head];
    L.from_me = from_me;
    snprintf(L.text, sizeof(L.text), "%s", chunk ? chunk : "");
    a.head = (uint8_t)((a.head + 1) % AGENT_THREAD_MAX);
    if (a.n < AGENT_THREAD_MAX) a.n++;
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

/* Push arbitrary-length text: split on word boundaries into ring slots.
   Long bot walls become several sequential lines, same speaker.
   Never cuts mid-UTF-8 sequence (Cyrillic is 2 bytes). */
static void agents_push_line(int idx, bool from_me, const char *text) {
    if (idx < 0 || idx >= AGENTS_N || !text || !text[0]) return;

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
            if (n + k > cap) break;  // don't start a char we can't finish
            if (k == 1 && p[n] == ' ') last_sp = n;
            n += k;
        }
        if (p[n] && last_sp > 8) n = last_sp;
        if (n <= 0) {
            // hard cut one codepoint (or byte if garbage)
            n = agents_utf8_len(p);
            if (n <= 0) n = 1;
            if (n > cap) n = cap;
        }

        char chunk[AGENT_TEXT_LEN];
        if (n >= (int)sizeof(chunk)) n = (int)sizeof(chunk) - 1;
        memcpy(chunk, p, n);
        chunk[n] = '\0';
        while (n > 0 && chunk[n - 1] == ' ') chunk[--n] = '\0';
        if (n > 0) agents_push_one(idx, from_me, chunk);
        p += n;
    }
}

/* Chronological view (oldest first) for scrollable chat. Returns count. */
static int agents_thread_view(int idx, char out[][AGENT_TEXT_LEN], bool *from_me,
                              int max_lines) {
    if (idx < 0 || idx >= AGENTS_N || max_lines <= 0) return 0;
    AgentSlot &a = g_agents[idx];
    int n = a.n < max_lines ? a.n : max_lines;
    // Take the newest `n` messages, emit oldest→newest for top-down reading.
    for (int i = 0; i < n; i++) {
        int age = n - 1 - i;  // age 0 = newest
        int slot = (a.head - 1 - age + AGENT_THREAD_MAX * 2) % AGENT_THREAD_MAX;
        snprintf(out[i], AGENT_TEXT_LEN, "%s", a.thread[slot].text);
        if (from_me) from_me[i] = a.thread[slot].from_me;
    }
    return n;
}

static int agents_thread_count(int idx) {
    if (idx < 0 || idx >= AGENTS_N) return 0;
    return g_agents[idx].n;
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
    // Only allow http:// on LAN — no https gymnastics in the seed.
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
static bool agents_bridge_post(const char *agent_id, const char *text) {
    if (!g_bridge[0] || !agent_id || !text || !text[0]) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[AGENT_BRIDGE_LEN + 16];
    // strip trailing slash
    size_t bl = strlen(g_bridge);
    while (bl > 0 && g_bridge[bl - 1] == '/') bl--;
    snprintf(url, sizeof(url), "%.*s/v1/chat", (int)bl, g_bridge);

    JsonDocument doc;
    doc["agent"] = agent_id;
    doc["session"] = g_session;
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

/* Public: send a line as the user.
 * Path: local thread always → WiFi bridge if up → else MeshCore C1 uplink.
 * Downlink reply uses the same C1 (or WiFi /agents/inbound). One chat loop. */
static bool agents_send(const char *agent_id, const char *text) {
    int idx = agents_find(agent_id);
    if (idx < 0 || !text || !text[0]) return false;

    char cleaned[AGENT_TEXT_LEN];
    agents_clean_text(text, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return false;

    portENTER_CRITICAL(&agents_mux);
    agents_push_line(idx, true, cleaned);
    portEXIT_CRITICAL(&agents_mux);

    event_add("agent %s << %s", agent_id, cleaned);
    display_force = true;

    bool wifi_ok = agents_bridge_post(agent_id, cleaned);
    bool mesh_ok = false;
    if (!wifi_ok && g_agents_mesh_uplink) {
        mesh_ok = g_agents_mesh_uplink(agent_id, cleaned);
        if (mesh_ok) event_add("agent %s mesh uplink", agent_id);
    }
    if (!wifi_ok && !mesh_ok) {
        portENTER_CRITICAL(&agents_mux);
        if (g_bridge[0] && WiFi.status() != WL_CONNECTED)
            agents_push_line(idx, false, "(offline - mesh failed)");
        else if (g_bridge[0])
            agents_push_line(idx, false, "(bridge offline)");
        else if (g_agents_mesh_uplink)
            agents_push_line(idx, false, "(mesh failed)");
        else
            agents_push_line(idx, false, "(no bridge / mesh)");
        portEXIT_CRITICAL(&agents_mux);
        display_force = true;
    }
    return true;
}

/* Inject an agent reply into the thread (long texts are split across slots). */
static void agents_on_inbound(const char *agent_id, const char *text) {
    int idx = agents_find(agent_id);
    if (idx < 0 || !text || !text[0]) return;
    // Allow up to ~2KB inbound wall of text (split into ring slots).
    char cleaned[2048];
    agents_clean_text(text, cleaned, sizeof(cleaned));
    if (!cleaned[0]) return;
    portENTER_CRITICAL(&agents_mux);
    agents_push_line(idx, false, cleaned);
    portEXIT_CRITICAL(&agents_mux);
    display_force = true;
}

/* Wipe the session thread for one agent (or all if id is null/"*"). */
static bool agents_clear(const char *agent_id) {
    portENTER_CRITICAL(&agents_mux);
    if (!agent_id || !agent_id[0] || strcmp(agent_id, "*") == 0) {
        for (int i = 0; i < AGENTS_N; i++) {
            g_agents[i].n = 0;
            g_agents[i].head = 0;
            memset(g_agents[i].thread, 0, sizeof(g_agents[i].thread));
            char intro[AGENT_TEXT_LEN];
            snprintf(intro, sizeof(intro), "chat cleared - type to talk");
            agents_push_line(i, false, intro);
        }
    } else {
        int idx = agents_find(agent_id);
        if (idx < 0) {
            portEXIT_CRITICAL(&agents_mux);
            return false;
        }
        g_agents[idx].n = 0;
        g_agents[idx].head = 0;
        memset(g_agents[idx].thread, 0, sizeof(g_agents[idx].thread));
        agents_push_line(idx, false, "chat cleared - type to talk");
    }
    portEXIT_CRITICAL(&agents_mux);
    display_force = true;
    return true;
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
        "Pocket chat with Grok / Claude / Hermes.\n"
        "Uplink: WiFi bridge /v1/chat, else MeshCore C1|agent|…|u|… private DM.\n"
        "Downlink: same C1 side=a (or WiFi /agents/inbound) — one loop.\n\n"
        "SPIFFS `/agent_bridge.txt` = base URL (http://host:port).\n";
}

static const SkillEndpoint agents_endpoints[] = {
    {"GET",  "/agents",         "List agents, bridge URL, thread tails"},
    {"POST", "/agents/send",    "Send {agent, text} into a thread (+ bridge)"},
    {"POST", "/agents/inbound", "Inject agent reply {agent, text} into thread"},
    {"POST", "/agents/clear",   "Clear session {agent} or {\"all\":true}"},
    {"POST", "/agents/bridge",  "Set bridge base URL (raw body), empty clears"},
    {NULL, NULL, NULL}
};

static void agents_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/agents"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        doc["bridge"] = g_bridge[0] ? g_bridge : "";
        doc["session"] = g_session;
        JsonArray arr = doc["agents"].to<JsonArray>();
        portENTER_CRITICAL(&agents_mux);
        for (int i = 0; i < AGENTS_N; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"] = g_agents[i].id;
            o["name"] = g_agents[i].name;
            o["messages"] = g_agents[i].n;
            if (g_agents[i].n > 0) {
                int slot = (g_agents[i].head - 1 + AGENT_THREAD_MAX) % AGENT_THREAD_MAX;
                o["last"] = g_agents[i].thread[slot].text;
                o["last_from_me"] = g_agents[i].thread[slot].from_me;
            }
        }
        portEXIT_CRITICAL(&agents_mux);
        notify_send_json(req, 200, doc);
    });

    server.on(AsyncURIMatcher::exact("/agents/send"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) {
            notify_send_error(req, 400, "JSON body required");
            return;
        }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }
        const char *agent = input["agent"] | "";
        const char *text  = input["text"]  | "";
        if (agents_find(agent) < 0) {
            notify_send_error(req, 400, "agent must be grok, claude or hermes");
            return;
        }
        if (!text[0]) {
            notify_send_error(req, 400, "text required");
            return;
        }
        bool ok = agents_send(agent, text);
        JsonDocument doc;
        doc["ok"] = ok;
        doc["agent"] = agent;
        doc["bridge"] = g_bridge[0] ? true : false;
        notify_send_json(req, ok ? 200 : 500, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/clear"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) {
            notify_send_error(req, 400, "JSON body required");
            return;
        }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }
        bool all = input["all"] | false;
        const char *agent = input["agent"] | "";
        bool ok = all ? agents_clear("*") : agents_clear(agent);
        if (!ok) {
            notify_send_error(req, 400, "unknown agent");
            return;
        }
        event_add("agent clear %s", all ? "*" : agent);
        JsonDocument doc;
        doc["ok"] = true;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/agents/inbound"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) {
            notify_send_error(req, 400, "JSON body required");
            return;
        }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }
        const char *agent = input["agent"] | "";
        const char *text  = input["text"]  | "";
        if (agents_find(agent) < 0) {
            notify_send_error(req, 400, "agent must be grok, claude or hermes");
            return;
        }
        if (!text[0]) {
            notify_send_error(req, 400, "text required");
            return;
        }
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
        url.trim();
        if (url.length() > 0 && !url.startsWith("http://")) {
            notify_send_error(req, 400, "URL must start with http://");
            return;
        }
        if (!agents_bridge_save(url.c_str())) {
            notify_send_error(req, 500, "failed to save bridge URL");
            return;
        }
        event_add("agent bridge %s", g_bridge[0] ? g_bridge : "(cleared)");
        JsonDocument doc;
        doc["ok"] = true;
        doc["bridge"] = g_bridge;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);
}

static const Skill agents_skill = {
    .name = "agents",
    .version = "0.1.0",
    .describe = agents_describe,
    .endpoints = agents_endpoints,
    .register_routes = agents_register_routes
};

static void skill_agents_init() {
    // Session tag includes last 4 of chip for multi-pager later.
    uint64_t mac = ESP.getEfuseMac();
    snprintf(g_session, sizeof(g_session), "pager-%04x",
             (unsigned)(mac & 0xFFFF));
    agents_bridge_load();
    // Seed one intro line so the chat is not a blank void.
    for (int i = 0; i < AGENTS_N; i++) {
        char intro[AGENT_TEXT_LEN];
        snprintf(intro, sizeof(intro), "hi - type to talk to %s", g_agents[i].name);
        agents_push_line(i, false, intro);
    }
    skill_register(&agents_skill);
    Serial.printf("[agents] session=%s bridge=%s\n",
                  g_session, g_bridge[0] ? g_bridge : "(none)");
}
