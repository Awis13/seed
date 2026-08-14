/*
 * skills/wg.cpp — WireGuard client to home gateway (pager beta)
 *
 * SPIFFS /wg.json (never commit secrets):
 * {
 *   "address": "10.66.0.2",
 *   "private_key": "<44 b64>",
 *   "enabled": true,
 *   "peer": {
 *     "public_key": "<44 b64 home-rig>",
 *     "endpoint": "37.188.198.199",
 *     "endpoint_lan": "192.168.1.138",
 *     "port": 51821,
 *     "home_ssid": "AWIS"
 *   }
 * }
 *
 * When STA is on home_ssid → endpoint_lan (if set), else public endpoint.
 * WireGuard-ESP32 is full-tunnel by default; home-rig NATs 10.66.0.0/24.
 *
 * API:
 *   GET  /wg/status
 *   POST /wg/config   {address, private_key, enabled?}
 *   POST /wg/peer     {public_key, endpoint, endpoint_lan?, port?, home_ssid?}
 *   POST /wg/start | /wg/stop | /wg/restart
 */

#include <WireGuard-ESP32.h>
#include <mbedtls/base64.h>

extern "C" {
#include "crypto/refc/x25519.h"
}

#define WG_CONFIG_FILE "/wg.json"
#define WG_HOME_SSID_DEFAULT "AWIS"
#define WG_PORT_DEFAULT 51821
#define WG_TICK_MS 5000UL
#define WG_HANDSHAKE_PROBE_MS 30000UL
/* Settle gap between stop and start of a deferred /wg/restart. It used to be
 * an inline 200 ms wait on the AsyncTCP task; now the loop task defers on
 * millis() instead. */
#define WG_RESTART_GAP_MS 200UL

static WireGuard g_wg;
static bool g_wg_running = false;
/* Written by three HTTP handlers on the AsyncTCP task, read by skill_wg_poll
 * on the loop task — volatile like the rest of the deferral cluster below. */
static volatile bool g_wg_want = false;
static bool g_wg_cfg_ok = false;
static char g_wg_pub[48] = "";
static char g_wg_addr[20] = "";
static char g_wg_endpoint_used[64] = "";
static uint32_t g_wg_up_ms = 0;
static uint32_t g_wg_last_ok_ms = 0;
static uint32_t g_wg_last_try_ms = 0;
static uint32_t g_wg_fail_streak = 0;
/* Deferred restart request (raised on the AsyncTCP task, served by the poll). */
static volatile bool g_wg_restart_req = false;
static bool g_wg_restart_armed = false;
static uint32_t g_wg_restart_stop_ms = 0;
/* Deferred start/stop requests — same shape as the restart deferral:
 * wg_start_now()/wg_stop_now() block in the WG library and must never run on
 * the AsyncTCP task. Handlers only save intent and raise a flag. */
static volatile bool g_wg_start_req = false;
static volatile bool g_wg_stop_req = false;

/* Clock chrome uses HwWgUi from hw_ui.h (included by main before this skill). */

static bool wg_validate_key(const char *key) {
    if (!key) return false;
    int len = (int)strlen(key);
    if (len != 44) return false;
    for (int i = 0; i < len; i++) {
        char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    }
    return true;
}

static bool wg_validate_ip(const char *ip) {
    if (!ip || !ip[0]) return false;
    for (int i = 0; ip[i]; i++) {
        char c = ip[i];
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    }
    return true;
}

static bool wg_derive_public_key(const char *private_key_b64) {
    uint8_t privkey[32];
    size_t privkey_len = 0;
    int ret = mbedtls_base64_decode(privkey, sizeof(privkey), &privkey_len,
                                    (const unsigned char *)private_key_b64,
                                    strlen(private_key_b64));
    if (ret != 0 || privkey_len != 32) {
        g_wg_pub[0] = '\0';
        return false;
    }
    uint8_t pubkey[32];
    x25519(pubkey, privkey, X25519_BASE_POINT, 1);
    memset(privkey, 0, sizeof(privkey));
    size_t out_len = 0;
    ret = mbedtls_base64_encode((unsigned char *)g_wg_pub, sizeof(g_wg_pub),
                                &out_len, pubkey, 32);
    memset(pubkey, 0, sizeof(pubkey));
    if (ret != 0) {
        g_wg_pub[0] = '\0';
        return false;
    }
    g_wg_pub[out_len] = '\0';
    return true;
}

/* Keep /wg.json in RAM. After socket exhaustion, SPIFFS fopen aborts
 * (lock_init_generic) — serial 2026-08-14: probe fail → stop → start → crash. */
#define WG_JSON_CAP 768
static char g_wg_json[WG_JSON_CAP];
static bool g_wg_json_ok = false;

static bool wg_load_config(JsonDocument &doc) {
    if (g_wg_json_ok)
        return deserializeJson(doc, g_wg_json) == DeserializationError::Ok;
    String json = read_spiffs_file(WG_CONFIG_FILE);
    if (json.length() == 0 || json.length() >= WG_JSON_CAP) return false;
    memcpy(g_wg_json, json.c_str(), json.length() + 1);
    g_wg_json_ok = true;
    return deserializeJson(doc, g_wg_json) == DeserializationError::Ok;
}

static bool wg_save_config(const JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    if (json.length() > 0 && json.length() < WG_JSON_CAP) {
        memcpy(g_wg_json, json.c_str(), json.length() + 1);
        g_wg_json_ok = true;
    }
    return write_spiffs_file(WG_CONFIG_FILE, json);
}

static bool wg_config_complete(const JsonDocument &doc) {
    /* Use String (owned) — const char* from JsonVariant can dangle after
     * other accesses on some ArduinoJson versions. */
    String address = doc["address"].as<String>();
    String private_key = doc["private_key"].as<String>();
    if (address.length() == 0 || private_key.length() == 0) return false;
    if (!doc["peer"].is<JsonObject>() && !doc["peer"].is<JsonObjectConst>())
        return false;
    String pk = doc["peer"]["public_key"].as<String>();
    String ep = doc["peer"]["endpoint"].as<String>();
    return pk.length() > 0 && ep.length() > 0;
}

/* Debug helper: which field is missing (never returns secrets). */
static const char *wg_config_missing(const JsonDocument &doc) {
    if (doc["address"].as<String>().length() == 0) return "address";
    if (doc["private_key"].as<String>().length() == 0) return "private_key";
    if (!doc["peer"].is<JsonObject>() && !doc["peer"].is<JsonObjectConst>())
        return "peer";
    if (doc["peer"]["public_key"].as<String>().length() == 0)
        return "peer.public_key";
    if (doc["peer"]["endpoint"].as<String>().length() == 0)
        return "peer.endpoint";
    return nullptr;
}

/* Pick LAN endpoint when on home SSID (or endpoint_lan empty → public only). */
static void wg_pick_endpoint(const JsonDocument &doc, char *out, size_t out_n,
                             uint16_t *port_out) {
    out[0] = '\0';
    uint16_t port = (uint16_t)(doc["peer"]["port"] | WG_PORT_DEFAULT);
    if (port == 0) port = WG_PORT_DEFAULT;
    *port_out = port;

    const char *home_ssid = doc["peer"]["home_ssid"] | WG_HOME_SSID_DEFAULT;
    const char *ep_pub = doc["peer"]["endpoint"] | "";
    const char *ep_lan = doc["peer"]["endpoint_lan"] | "";

    String cur = WiFi.SSID();
    bool at_home = (cur.length() > 0 && home_ssid && home_ssid[0] &&
                    cur.equals(home_ssid));
    if (at_home && ep_lan && ep_lan[0]) {
        snprintf(out, out_n, "%s", ep_lan);
    } else if (ep_pub && ep_pub[0]) {
        snprintf(out, out_n, "%s", ep_pub);
    } else if (ep_lan && ep_lan[0]) {
        snprintf(out, out_n, "%s", ep_lan);
    }
}

/* Library sets WG as default netif (full tunnel). Keep that for beta — home
 * NATs 10.66.0.0/24 so general internet still works via gateway. */
static bool wg_start_now() {
    JsonDocument doc;
    if (!wg_load_config(doc) || !wg_config_complete(doc)) {
        Serial.println("[wg] no complete config");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wg] WiFi not up");
        return false;
    }

    const char *address = doc["address"];
    const char *private_key = doc["private_key"];
    const char *peer_pubkey = doc["peer"]["public_key"];

    char endpoint[64];
    uint16_t port = WG_PORT_DEFAULT;
    wg_pick_endpoint(doc, endpoint, sizeof(endpoint), &port);
    if (!endpoint[0]) {
        Serial.println("[wg] no endpoint");
        return false;
    }

    wg_derive_public_key(private_key);
    snprintf(g_wg_addr, sizeof(g_wg_addr), "%s", address);
    snprintf(g_wg_endpoint_used, sizeof(g_wg_endpoint_used), "%s:%u",
             endpoint, (unsigned)port);

    IPAddress local_ip;
    if (!local_ip.fromString(address)) {
        Serial.printf("[wg] bad address %s\n", address);
        return false;
    }

    if (g_wg_running) return true;

    Serial.printf("[wg] start %s -> %s\n", address, g_wg_endpoint_used);
    g_wg_last_try_ms = millis();
    bool ok = g_wg.begin(local_ip, private_key, endpoint, peer_pubkey, port);
    if (!ok) {
        g_wg_fail_streak++;
        event_add("wg start failed -> %s", g_wg_endpoint_used);
        return false;
    }

    g_wg_running = true;
    g_wg_up_ms = millis();
    g_wg_last_ok_ms = millis(); /* library has no handshake probe; mark up */
    g_wg_fail_streak = 0;
    event_add("wg up %s -> %s", address, g_wg_endpoint_used);
    return true;
}

static void wg_stop_now() {
    if (g_wg_running) {
        g_wg.end();
        g_wg_running = false;
        event_add("wg stopped");
        Serial.println("[wg] stopped");
    }
}

static int wg_ui_state() {
    if (!g_wg_cfg_ok) return WG_UI_OFF;
    if (!g_wg_want) return WG_UI_OFF;
    if (!g_wg_running) {
        if (WiFi.status() != WL_CONNECTED) return WG_UI_WAIT;
        return g_wg_fail_streak > 0 ? WG_UI_DOWN : WG_UI_WAIT;
    }
    if (!g_wg.is_initialized()) return WG_UI_DOWN;
    /* No native handshake age — treat running+init as OK; stale if WiFi lost. */
    if (WiFi.status() != WL_CONNECTED) return WG_UI_STALE;
    return WG_UI_OK;
}

static int wg_alive_age_s() {
    if (!g_wg_running || g_wg_last_ok_ms == 0) return -1;
    return (int)((millis() - g_wg_last_ok_ms) / 1000UL);
}

static bool wg_is_up() {
    return g_wg_running && g_wg.is_initialized();
}

/* Prefer WG gateway address when tunnel is up (for agents/mesh URL builders). */
static const char *wg_gateway_ip() {
    return "10.66.0.1";
}

static void wg_status_json(JsonDocument &doc) {
    JsonDocument cfg;
    bool loaded = wg_load_config(cfg);
    /* Re-evaluate every status (SPIFFS is source of truth). */
    if (loaded) g_wg_cfg_ok = wg_config_complete(cfg);

    doc["configured"] = g_wg_cfg_ok;
    doc["enabled"] = (bool)g_wg_want;
    doc["up"] = g_wg_running;
    doc["initialized"] = g_wg_running && g_wg.is_initialized();
    doc["link"] = wg_ui_state();
    doc["address"] = g_wg_addr;
    doc["public_key"] = g_wg_pub;
    doc["endpoint"] = g_wg_endpoint_used;
    doc["fail_streak"] = g_wg_fail_streak;
    doc["has_private_key"] = loaded && ((cfg["private_key"] | "")[0] != '\0');
    /* RAM only — re-reading SPIFFS under low DRAM aborts in lock_init. */
    doc["cfg_bytes"] = g_wg_json_ok ? (int)strlen(g_wg_json) : 0;
    if (g_wg_running && g_wg_up_ms)
        doc["up_age_s"] = (unsigned long)((millis() - g_wg_up_ms) / 1000UL);
    else
        doc["up_age_s"] = 0;
    doc["alive_age_s"] = wg_alive_age_s();
    doc["wifi"] = (WiFi.status() == WL_CONNECTED);
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_ip"] = WiFi.localIP().toString();
    }

    if (loaded && cfg["peer"].is<JsonObject>()) {
        doc["peer_public_key"] = cfg["peer"]["public_key"].as<String>();
        doc["peer_endpoint"] = cfg["peer"]["endpoint"].as<String>();
        if (cfg["peer"]["endpoint_lan"])
            doc["peer_endpoint_lan"] = cfg["peer"]["endpoint_lan"].as<String>();
        doc["peer_port"] = cfg["peer"]["port"] | WG_PORT_DEFAULT;
        doc["home_ssid"] = cfg["peer"]["home_ssid"] | WG_HOME_SSID_DEFAULT;
    }
}

static const SkillEndpoint wg_endpoints[] = {
    {"GET",  "/wg/status",  "WireGuard tunnel status"},
    {"POST", "/wg/setup",   "Atomic full /wg.json (address+key+peer)"},
    {"POST", "/wg/config",  "Set address + private_key (+enabled)"},
    {"POST", "/wg/peer",    "Set home peer endpoint/keys"},
    {"POST", "/wg/start",   "Start tunnel (deferred; poll /wg/status)"},
    {"POST", "/wg/stop",    "Stop tunnel (deferred; poll /wg/status)"},
    {"POST", "/wg/restart", "Restart tunnel (deferred; poll /wg/status)"},
    {NULL, NULL, NULL}
};

static const char *wg_describe() {
    return "## Skill: wireguard\n\n"
           "Private tunnel pager → home-rig gateway (`10.66.0.1`).\n"
           "SPIFFS `/wg.json` (secrets, not in git).\n"
           "On home SSID uses `endpoint_lan`, else public `endpoint`.\n"
           "Gateway services: `http://10.66.0.1:8325/notify-out`, `:8091`.\n"
           "Clock `W`: off / wait / ok / down.\n";
}

static void wg_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/wg/status"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        wg_status_json(doc);
        notify_send_json(req, 200, doc);
    });

    /* Atomic full config — body is the whole /wg.json (owned String). */
    server.on(AsyncURIMatcher::exact("/wg/setup"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body || !body[0]) {
            free(body);
            notify_send_error(req, 400, "body required (full wg.json)");
            return;
        }
        String raw(body);
        free(body);

        JsonDocument cfg;
        DeserializationError err = deserializeJson(cfg, raw);
        if (err) {
            char msg[48];
            snprintf(msg, sizeof(msg), "bad json: %s", err.c_str());
            notify_send_error(req, 400, msg);
            return;
        }
        const char *miss = wg_config_missing(cfg);
        if (miss) {
            char msg[64];
            snprintf(msg, sizeof(msg), "missing field: %s (len=%u)",
                     miss, (unsigned)raw.length());
            notify_send_error(req, 400, msg);
            return;
        }
        String priv = cfg["private_key"].as<String>();
        String addr = cfg["address"].as<String>();
        if (!wg_validate_key(priv.c_str()) || !wg_validate_ip(addr.c_str())) {
            notify_send_error(req, 400, "bad address or private_key");
            return;
        }
        if (!cfg["enabled"].is<bool>()) cfg["enabled"] = true;

        /* Prefer the raw body (already valid JSON) — avoid re-serialize drift. */
        if (!write_spiffs_file(WG_CONFIG_FILE, raw)) {
            notify_send_error(req, 500, "save failed");
            return;
        }

        g_wg_cfg_ok = true;
        g_wg_want = cfg["enabled"] | true;
        snprintf(g_wg_addr, sizeof(g_wg_addr), "%s", addr.c_str());
        wg_derive_public_key(priv.c_str());
        event_add("wg setup ok %s", g_wg_addr);

        JsonDocument resp;
        resp["ok"] = true;
        resp["configured"] = true;
        resp["public_key"] = g_wg_pub;
        resp["bytes"] = (int)raw.length();
        notify_send_json(req, 200, resp);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/wg/config"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) {
            notify_send_error(req, 400, "body required");
            return;
        }
        /* ArduinoJson zero-copies from char* — keep body until fields are
         * owned by cfg (String copies), then free. */
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "bad json");
            return;
        }

        JsonDocument cfg;
        wg_load_config(cfg);

        if (!input["address"].isNull()) {
            String a = input["address"].as<String>();
            if (!wg_validate_ip(a.c_str())) {
                free(body);
                notify_send_error(req, 400, "invalid address");
                return;
            }
            cfg["address"] = a;
            snprintf(g_wg_addr, sizeof(g_wg_addr), "%s", a.c_str());
        }
        if (!input["private_key"].isNull()) {
            String k = input["private_key"].as<String>();
            if (!wg_validate_key(k.c_str())) {
                free(body);
                notify_send_error(req, 400, "private_key must be 44 base64 chars");
                return;
            }
            cfg["private_key"] = k;
            wg_derive_public_key(k.c_str());
        }
        free(body);

        if (input["enabled"].is<bool>()) {
            cfg["enabled"] = input["enabled"].as<bool>();
            g_wg_want = input["enabled"].as<bool>();
        } else if (!cfg["enabled"].is<bool>()) {
            cfg["enabled"] = true;
            g_wg_want = true;
        }

        if (!wg_save_config(cfg)) {
            notify_send_error(req, 500, "save failed");
            return;
        }
        g_wg_cfg_ok = wg_config_complete(cfg);
        event_add("wg config saved cfg_ok=%d", (int)g_wg_cfg_ok);

        JsonDocument out;
        out["ok"] = true;
        out["configured"] = g_wg_cfg_ok;
        out["public_key"] = g_wg_pub;
        out["enabled"] = (bool)g_wg_want;
        notify_send_json(req, 200, out);
    }, NULL, handle_body_collect);

    server.on(AsyncURIMatcher::exact("/wg/peer"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body) {
            notify_send_error(req, 400, "body required");
            return;
        }
        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "bad json");
            return;
        }

        String pk = input["public_key"] | "";
        String ep = input["endpoint"] | "";
        String ep_lan = input["endpoint_lan"] | "";
        String home = input["home_ssid"] | "";
        int port = input["port"] | WG_PORT_DEFAULT;
        free(body);

        if (!wg_validate_key(pk.c_str())) {
            notify_send_error(req, 400, "public_key required (44 b64)");
            return;
        }
        if (ep.length() == 0) {
            notify_send_error(req, 400, "endpoint required");
            return;
        }
        for (unsigned i = 0; i < ep.length(); i++) {
            char c = ep.charAt(i);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) {
                notify_send_error(req, 400, "invalid endpoint");
                return;
            }
        }
        if (port < 1 || port > 65535) {
            notify_send_error(req, 400, "bad port");
            return;
        }

        JsonDocument cfg;
        wg_load_config(cfg);
        JsonObject peer = cfg["peer"].to<JsonObject>();
        peer["public_key"] = pk;
        peer["endpoint"] = ep;
        peer["port"] = port;
        if (ep_lan.length()) peer["endpoint_lan"] = ep_lan;
        if (home.length()) peer["home_ssid"] = home;
        else if (!peer["home_ssid"]) peer["home_ssid"] = WG_HOME_SSID_DEFAULT;

        if (!wg_save_config(cfg)) {
            notify_send_error(req, 500, "save failed");
            return;
        }
        g_wg_cfg_ok = wg_config_complete(cfg);
        event_add("wg peer %s:%d cfg_ok=%d", ep.c_str(), port, (int)g_wg_cfg_ok);

        JsonDocument out;
        out["ok"] = true;
        out["configured"] = g_wg_cfg_ok;
        notify_send_json(req, 200, out);
    }, NULL, handle_body_collect);

    /* Deferred like /wg/restart: the handler only saves intent and raises a
     * flag; skill_wg_poll (loop task) starts the tunnel. Outcome is
     * observable via GET /wg/status. */
    server.on(AsyncURIMatcher::exact("/wg/start"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        g_wg_want = true;
        {
            JsonDocument cfg;
            if (wg_load_config(cfg)) {
                cfg["enabled"] = true;
                wg_save_config(cfg);
            }
        }
        g_wg_start_req = true;
        JsonDocument out;
        out["ok"] = true;
        out["starting"] = true;
        out["up"] = g_wg_running;
        notify_send_json(req, 200, out);
    });

    /* Deferred like /wg/restart — same reason, same observation point. */
    server.on(AsyncURIMatcher::exact("/wg/stop"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        g_wg_want = false;
        {
            JsonDocument cfg;
            if (wg_load_config(cfg)) {
                cfg["enabled"] = false;
                wg_save_config(cfg);
            }
        }
        g_wg_stop_req = true;
        JsonDocument out;
        out["ok"] = true;
        out["stopping"] = true;
        out["up"] = g_wg_running;
        notify_send_json(req, 200, out);
    });

    /* Deferred: this handler runs on the AsyncTCP task, where the old
     * stop → 200 ms wait → start sequence stalled every socket on the box.
     * It now only raises a request; the poll (loop task) stops the tunnel,
     * waits WG_RESTART_GAP_MS on millis(), and starts it again. Outcome is
     * observable via GET /wg/status. */
    server.on(AsyncURIMatcher::exact("/wg/restart"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        g_wg_want = true;
        g_wg_restart_req = true;
        JsonDocument out;
        out["ok"] = true;
        out["restarting"] = true;
        out["up"] = g_wg_running;
        notify_send_json(req, 200, out);
    });
}

static void skill_wg_poll() {
    /* Deferred /wg/stop and /wg/start — raised on the AsyncTCP task, run
     * here on the loop task, ahead of the tick throttle. Stop first, so a
     * stop+start pair racing one poll behaves like a restart. */
    if (g_wg_stop_req) {
        g_wg_stop_req = false;
        wg_stop_now();
    }
    if (g_wg_start_req) {
        g_wg_start_req = false;
        if (g_wg_want) wg_start_now();
    }

    /* Deferred /wg/restart — ahead of the tick throttle so the stop→start
     * gap is honoured promptly, and on millis() so nothing blocks. */
    if (g_wg_restart_req) {
        g_wg_restart_req = false;
        wg_stop_now();
        g_wg_restart_stop_ms = millis();
        g_wg_restart_armed = true;
    }
    if (g_wg_restart_armed) {
        if (millis() - g_wg_restart_stop_ms < WG_RESTART_GAP_MS) return;
        g_wg_restart_armed = false;
        if (g_wg_want) wg_start_now();
    }

    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < WG_TICK_MS) return;
    last = now;

    /* WiFi lost → stop tunnel (library timer must not keep handshaking). */
    if (WiFi.status() != WL_CONNECTED) {
        if (g_wg_running) wg_stop_now();
        return;
    }

    /* EMERGENCY 0.9.106: do NOT auto-start WireGuard.
     * Live crash: NetworkClient "socket: 105" ×15 → wireguardif_tmr
     * handshake → lock_init_generic → abort() → reboot loop.
     * Days-stable builds either had no WG load or sockets free.
     * Tunnel only via explicit POST /wg/start (g_wg_start_req above).
     * Mesh + LAN WiFi keep chat alive at home. */
    (void)g_wg_want;
    (void)g_wg_cfg_ok;
}

static const Skill wg_skill = {
    .name = "wireguard",
    .version = "0.1.0",
    .describe = wg_describe,
    .endpoints = wg_endpoints,
    .register_routes = wg_register_routes,
    .tick = skill_wg_poll
};

static void skill_wg_init() {
    g_wg_running = false;
    g_wg_want = false;
    g_wg_cfg_ok = false;
    g_wg_pub[0] = g_wg_addr[0] = g_wg_endpoint_used[0] = '\0';

    JsonDocument cfg;
    if (wg_load_config(cfg)) {
        g_wg_cfg_ok = wg_config_complete(cfg);
        g_wg_want = cfg["enabled"] | true;
        if (!cfg["enabled"].is<bool>()) g_wg_want = g_wg_cfg_ok;
        if (cfg["address"])
            snprintf(g_wg_addr, sizeof(g_wg_addr), "%s",
                     cfg["address"].as<const char *>());
        if (cfg["private_key"])
            wg_derive_public_key(cfg["private_key"]);
    }

    skill_register(&wg_skill);
    Serial.printf("[wg] cfg_ok=%d want=%d pub=%.12s…\n",
                  (int)g_wg_cfg_ok, (int)g_wg_want, g_wg_pub);
    event_add("wg skill cfg_ok=%d", (int)g_wg_cfg_ok);

    /* Defer tunnel start to poll (need WiFi fully settled). */
}
