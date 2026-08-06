/*
 * skills/panel.cpp — transient, agent-owned home-page panels
 *
 * Endpoints:
 *   GET  /panel — list the live panels
 *   POST /panel — create, update, or delete one panel by key
 *
 * This is only the bounded data plane for later home pages. It deliberately
 * has no drawing, screen selection, wake, input, or backlight behaviour. A
 * successful POST invalidates the normal display cache; loop() remains the
 * only owner of the display.
 *
 * Eight fixed slots make the RAM cost constant. Updates happen in place and
 * keep their original position. A new key uses a free slot, then an expired
 * slot, then evicts the entry with the oldest arrival time. Expiry is polled
 * from loop(), while GET also filters expired entries from its snapshot.
 *
 * The core below is compiled directly on the host by tools/test_panel.sh.
 */

#include <esp_timer.h>

/* host-test:begin core — sliced out by tools/test_panel.sh */
#define PANEL_MAX          8
#define PANEL_KEY_LEN     17
#define PANEL_TITLE_LEN   65
#define PANEL_BODY_LEN   257

struct Panel {
    uint64_t arrived_ms;
    uint32_t ttl_s;
    int32_t pos;
    char key[PANEL_KEY_LEN];
    char title[PANEL_TITLE_LEN];
    char body[PANEL_BODY_LEN];
};

static Panel panel_slot[PANEL_MAX];

static void panel_prepare(Panel &panel, const char *key, const char *title,
                          const char *body, uint32_t ttl_s, int32_t pos) {
    memset(&panel, 0, sizeof(panel));
    snprintf(panel.key, sizeof(panel.key), "%s", key);
    notify_copy_text(panel.title, sizeof(panel.title), title);
    notify_copy_text(panel.body, sizeof(panel.body), body);
    panel.ttl_s = ttl_s;
    panel.pos = pos;
}

static bool panel_key_check(const char *key, const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;
    if (!key || !key[0]) {
        *err = "key is required and must not be empty";
        return false;
    }
    size_t n = strlen(key);
    if (n >= PANEL_KEY_LEN) {
        *err = "key is too long: 16 bytes at most";
        return false;
    }
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x20 || c > 0x7E) {
            *err = "key must be printable ASCII (32..126)";
            return false;
        }
        if (c != ' ') any = true;
    }
    if (!any) {
        *err = "key must not be blank";
        return false;
    }
    return true;
}

static bool panel_expired(const Panel &panel, uint64_t now) {
    if (!panel.key[0] || panel.ttl_s == 0) return false;
    if (now < panel.arrived_ms) return false;
    return now - panel.arrived_ms >= (uint64_t)panel.ttl_s * 1000ULL;
}

static uint64_t panel_age(const Panel &panel, uint64_t now) {
    return now < panel.arrived_ms ? 0ULL : now - panel.arrived_ms;
}

static int panel_find(const char *key) {
    for (int i = 0; i < PANEL_MAX; i++) {
        if (panel_slot[i].key[0] && strcmp(panel_slot[i].key, key) == 0) return i;
    }
    return -1;
}

static int panel_admit(uint64_t now) {
    for (int i = 0; i < PANEL_MAX; i++) if (!panel_slot[i].key[0]) return i;
    for (int i = 0; i < PANEL_MAX; i++) if (panel_expired(panel_slot[i], now)) return i;

    int oldest = 0;
    for (int i = 1; i < PANEL_MAX; i++) {
        uint64_t age = panel_age(panel_slot[i], now);
        uint64_t oldest_age = panel_age(panel_slot[oldest], now);
        if (age > oldest_age) oldest = i;
    }
    return oldest;
}

static void panel_sort(Panel *panels, int count) {
    for (int i = 1; i < count; i++) {
        Panel value = panels[i];
        int j = i;
        while (j > 0 && (panels[j - 1].pos > value.pos ||
               (panels[j - 1].pos == value.pos &&
                strcmp(panels[j - 1].key, value.key) > 0))) {
            panels[j] = panels[j - 1];
            j--;
        }
        panels[j] = value;
    }
}

static int panel_snapshot(Panel *snapshot, uint64_t now) {
    int count = 0;
    for (int i = 0; i < PANEL_MAX; i++) {
        if (panel_slot[i].key[0] && !panel_expired(panel_slot[i], now))
            snapshot[count++] = panel_slot[i];
    }
    return count;
}

static bool panel_reap(uint64_t now) {
    bool changed = false;
    for (int i = 0; i < PANEL_MAX; i++) {
        if (panel_expired(panel_slot[i], now)) {
            memset(&panel_slot[i], 0, sizeof(panel_slot[i]));
            changed = true;
        }
    }
    return changed;
}

static bool panel_core_apply(const Panel &incoming, bool pos_given,
                             uint64_t now, bool *created, bool *deleted,
                             const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;
    if (created) *created = false;
    if (deleted) *deleted = false;

    int slot = panel_find(incoming.key);
    if (!incoming.body[0]) {
        if (slot < 0) {
            *err = "cannot delete a missing key";
            return false;
        }
        memset(&panel_slot[slot], 0, sizeof(panel_slot[slot]));
        if (deleted) *deleted = true;
        return true;
    }

    bool is_create = slot < 0;
    if (is_create) slot = panel_admit(now);
    Panel &panel = panel_slot[slot];
    if (is_create) {
        panel = incoming;
        if (!pos_given) panel.pos = 0;
    } else {
        memcpy(panel.title, incoming.title, sizeof(panel.title));
        memcpy(panel.body, incoming.body, sizeof(panel.body));
        panel.ttl_s = incoming.ttl_s;
    }
    panel.arrived_ms = now;
    if (created) *created = is_create;
    return true;
}
/* host-test:end */

static_assert(sizeof(Panel) == 360,
              "Panel changed size: the store costs PANEL_MAX times this");

static portMUX_TYPE panel_mux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t panel_now_ms() {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

/* One coherent view for the local UI. The timestamp is sampled once while the
   store is locked, so every age drawn in one frame has the same origin. Sorting
   under the same lock also makes this accessor a complete snapshot operation:
   callers never observe or depend on panel_slot[] after the lock is released. */
static int panel_live_snapshot(Panel *snapshot, uint64_t *sampled_ms) {
    if (!snapshot || !sampled_ms) return 0;

    portENTER_CRITICAL(&panel_mux);
    *sampled_ms = panel_now_ms();
    int count = panel_snapshot(snapshot, *sampled_ms);
    panel_sort(snapshot, count);
    portEXIT_CRITICAL(&panel_mux);
    return count;
}

static const char *panel_describe() {
    return "## panel\n\n"
           "A fixed store of eight transient panels for agent-owned home pages.\n\n"
           "### POST /panel\n\n"
           "Body: `key` (required string, 1..16 bytes of printable, non-blank "
           "ASCII), `title` (string), `body` (string), optional `ttl_s`, and "
           "optional `pos`. Title holds 64 bytes and body holds 256 bytes; "
           "both are cut only at a complete UTF-8 character. `ttl_s` is an "
           "unsigned 32-bit integer in 0..4294967295 seconds; 0 never expires. "
           "Expiry uses the ESP timer's 64-bit monotonic milliseconds, so the "
           "32-bit millis() rollover is irrelevant. `pos` is a signed "
           "32-bit integer and defaults to 0.\n\n"
           "Posting an existing key updates its title, body, ttl and arrival "
           "time in place. Its original `pos` is sticky: a `pos` supplied on "
           "update is ignored. Posting an empty body deletes an existing key; "
           "title is then optional, and deleting a missing key is an error. "
           "A new key uses a free or expired slot before evicting the oldest "
           "arrival.\n\n"
           "### GET /panel\n\n"
           "Returns `{count,capacity,panels}` for live entries, ordered by "
           "ascending `pos` and then `key`. Expired entries are omitted.\n";
}

static const SkillEndpoint panel_endpoints[] = {
    {"GET",  "/panel", "List live home-page panels"},
    {"POST", "/panel", "Create, update, or delete a home-page panel"},
    {NULL, NULL, NULL}
};

static void panel_send_list(AsyncWebServerRequest *req) {
    Panel snapshot[PANEL_MAX];
    int count;

    portENTER_CRITICAL(&panel_mux);
    uint64_t now = panel_now_ms();
    count = panel_snapshot(snapshot, now);
    portEXIT_CRITICAL(&panel_mux);

    panel_sort(snapshot, count);

    JsonDocument doc;
    doc["count"] = count;
    doc["capacity"] = PANEL_MAX;
    JsonArray panels = doc["panels"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject out = panels.add<JsonObject>();
        out["key"] = snapshot[i].key;
        out["title"] = snapshot[i].title;
        out["body"] = snapshot[i].body;
        out["ttl_s"] = snapshot[i].ttl_s;
        out["pos"] = snapshot[i].pos;
    }
    notify_send_json(req, 200, doc);
}

static void panel_send_post(AsyncWebServerRequest *req) {
    char *body_json = notify_take_body(req);
    if (!body_json) {
        notify_send_error(req, 400, "body must be JSON");
        return;
    }
    JsonDocument input;
    DeserializationError parse_err = deserializeJson(input, body_json);
    free(body_json);
    if (parse_err || !input.is<JsonObject>()) {
        notify_send_error(req, 400, "body must be a JSON object");
        return;
    }
    JsonObjectConst object = input.as<JsonObjectConst>();
    JsonVariantConst key_value = object["key"];
    JsonVariantConst title_value = object["title"];
    JsonVariantConst body_value = object["body"];
    JsonVariantConst ttl_value = object["ttl_s"];
    JsonVariantConst pos_value = object["pos"];
    if (!key_value.is<const char*>()) {
        notify_send_error(req, 400, "key is required and must be a string");
        return;
    }
    const char *key = key_value.as<const char*>();
    const char *key_err = NULL;
    if (!panel_key_check(key, &key_err)) {
        notify_send_error(req, 400, key_err);
        return;
    }
    if (!body_value.is<const char*>()) {
        notify_send_error(req, 400, "body is required and must be a string");
        return;
    }
    const char *body = body_value.as<const char*>();
    bool deleting = body[0] == '\0';
    if (!deleting && !title_value.is<const char*>()) {
        notify_send_error(req, 400, "title is required and must be a string");
        return;
    }
    if (deleting && !title_value.isUnbound() && !title_value.is<const char*>()) {
        notify_send_error(req, 400, "title must be a string when supplied");
        return;
    }
    if (!ttl_value.isUnbound() && !ttl_value.is<uint32_t>()) {
        notify_send_error(req, 400, "ttl_s must be an integer in 0..4294967295");
        return;
    }
    uint32_t ttl_s = ttl_value | 0U;
    bool pos_given = !pos_value.isUnbound();
    if (pos_given && !pos_value.is<int32_t>()) {
        notify_send_error(req, 400, "pos must be a signed 32-bit integer");
        return;
    }
    int32_t pos = pos_value | 0;

    Panel incoming;
    panel_prepare(incoming, key, deleting ? "" : title_value.as<const char*>(),
                  body, ttl_s, pos);

    bool created = false;
    bool deleted = false;
    const char *apply_err = NULL;
    portENTER_CRITICAL(&panel_mux);
    bool ok = panel_core_apply(incoming, pos_given, panel_now_ms(), &created, &deleted,
                               &apply_err);
    portEXIT_CRITICAL(&panel_mux);
    if (!ok) {
        notify_send_error(req, 404, apply_err);
        return;
    }
    display_force = true;

    JsonDocument doc;
    doc["ok"] = true;
    doc["created"] = created;
    doc["deleted"] = deleted;
    notify_send_json(req, created ? 201 : 200, doc);
}

static void panel_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/panel"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        panel_send_list(req);
    });
    server.on(AsyncURIMatcher::exact("/panel"), HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        panel_send_post(req);
    }, NULL, handle_body_collect);
}

static const Skill panel_skill = {
    "panel", "0.1.0", panel_describe, panel_endpoints, panel_register_routes
};

static void skill_panel_init() {
    skill_register(&panel_skill);
}

static void panel_poll() {
    portENTER_CRITICAL(&panel_mux);
    uint64_t now = panel_now_ms();
    bool changed = panel_reap(now);
    portEXIT_CRITICAL(&panel_mux);
    if (changed) display_force = true;
}
