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
#include <math.h>

/* host-test:begin core — sliced out by tools/test_panel.sh */
#define PANEL_MAX          8
#define PANEL_KEY_LEN     17
#define PANEL_TITLE_LEN   65
#define PANEL_BODY_LEN   257
#define PANEL_KV_MAX       5
#define PANEL_BAR_MAX      4
#define PANEL_SPARK_MAX   48
#define PANEL_LABEL_LEN   17
#define PANEL_VALUE_LEN   33
#define PANEL_STATUS_LEN  25
#define PANEL_UNIT_LEN     9
#define PANEL_DETAIL_LEN  65

enum PanelKind : uint8_t {
    PANEL_KIND_TEXT = 0,
    PANEL_KIND_KV,
    PANEL_KIND_BARS,
    PANEL_KIND_SPARKLINE,
    PANEL_KIND_STATUS
};

struct PanelKvItem {
    char label[PANEL_LABEL_LEN];
    char value[PANEL_VALUE_LEN];
};

struct PanelBarItem {
    char label[PANEL_LABEL_LEN];
    float value;
    float max;
    char unit[PANEL_UNIT_LEN];
};

struct PanelKvPayload {
    uint8_t count;
    PanelKvItem items[PANEL_KV_MAX];
};

struct PanelBarsPayload {
    uint8_t count;
    PanelBarItem items[PANEL_BAR_MAX];
};

struct PanelSparkPayload {
    uint8_t count;
    char unit[PANEL_UNIT_LEN];
    float values[PANEL_SPARK_MAX];
};

struct PanelStatusPayload {
    char value[PANEL_STATUS_LEN];
    char unit[PANEL_UNIT_LEN];
    char detail[PANEL_DETAIL_LEN];
};

union PanelPayload {
    char body[PANEL_BODY_LEN];
    PanelKvPayload kv;
    PanelBarsPayload bars;
    PanelSparkPayload sparkline;
    PanelStatusPayload status;
};

struct Panel {
    uint64_t arrived_ms;
    uint32_t ttl_s;
    int32_t pos;
    char key[PANEL_KEY_LEN];
    char title[PANEL_TITLE_LEN];
    PanelKind kind;
    PanelPayload payload;
};

static_assert(sizeof(PanelPayload) == 260,
              "Panel payload must retain its fixed 260-byte allocation");

static Panel panel_slot[PANEL_MAX];

static bool panel_kind_valid(PanelKind kind) {
    return kind >= PANEL_KIND_TEXT && kind <= PANEL_KIND_STATUS;
}

static const char *panel_kind_name(PanelKind kind) {
    if (!panel_kind_valid(kind)) return NULL;
    switch (kind) {
        case PANEL_KIND_TEXT:      return "text";
        case PANEL_KIND_KV:        return "kv";
        case PANEL_KIND_BARS:      return "bars";
        case PANEL_KIND_SPARKLINE: return "sparkline";
        case PANEL_KIND_STATUS:    return "status";
        default:                   return NULL;
    }
}

static bool panel_kind_parse(const char *name, PanelKind *kind) {
    if (!name || !kind) return false;
    for (uint8_t value = PANEL_KIND_TEXT; value <= PANEL_KIND_STATUS; value++) {
        PanelKind candidate = (PanelKind)value;
        if (strcmp(name, panel_kind_name(candidate)) == 0) {
            *kind = candidate;
            return true;
        }
    }
    return false;
}

static bool panel_field_allowed(PanelKind kind, const char *name) {
    if (!name) return false;
    if (strcmp(name, "key") == 0 || strcmp(name, "title") == 0 ||
        strcmp(name, "kind") == 0 || strcmp(name, "ttl_s") == 0 ||
        strcmp(name, "pos") == 0) return true;
    switch (kind) {
        case PANEL_KIND_TEXT:      return strcmp(name, "body") == 0;
        case PANEL_KIND_KV:        return strcmp(name, "items") == 0;
        case PANEL_KIND_BARS:      return strcmp(name, "items") == 0;
        case PANEL_KIND_SPARKLINE: return strcmp(name, "values") == 0 ||
                                           strcmp(name, "unit") == 0;
        case PANEL_KIND_STATUS:    return strcmp(name, "value") == 0 ||
                                           strcmp(name, "unit") == 0 ||
                                           strcmp(name, "detail") == 0;
        default:                   return false;
    }
}

static bool panel_nested_field_allowed(PanelKind kind, const char *name) {
    if (!name) return false;
    if (kind == PANEL_KIND_KV)
        return strcmp(name, "label") == 0 || strcmp(name, "value") == 0;
    if (kind == PANEL_KIND_BARS)
        return strcmp(name, "label") == 0 || strcmp(name, "value") == 0 ||
               strcmp(name, "max") == 0 || strcmp(name, "unit") == 0;
    return false;
}

static bool panel_string_check(const char *value, size_t min_bytes,
                               size_t max_bytes, const char **err,
                               const char *message) {
    if (!value) {
        if (err) *err = message;
        return false;
    }
    size_t length = strlen(value);
    if (length < min_bytes || length > max_bytes) {
        if (err) *err = message;
        return false;
    }
    return true;
}

static bool panel_bar_values_check(double value, double maximum) {
    return isfinite(value) && isfinite(maximum) && value >= 0.0 &&
           maximum > 0.0 && value <= maximum && maximum <= 1.0e9;
}

static bool panel_spark_value_check(double value) {
    return isfinite(value) && value >= -1.0e9 && value <= 1.0e9;
}

static void panel_prepare(Panel &panel, const char *key, const char *title,
                          const char *body, uint32_t ttl_s, int32_t pos) {
    memset(&panel, 0, sizeof(panel));
    snprintf(panel.key, sizeof(panel.key), "%s", key);
    notify_copy_text(panel.title, sizeof(panel.title), title);
    panel.kind = PANEL_KIND_TEXT;
    notify_copy_text(panel.payload.body, sizeof(panel.payload.body), body);
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

static bool panel_core_apply(const Panel &incoming, bool deleting, bool pos_given,
                             uint64_t now, bool *created, bool *deleted,
                             const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;
    if (created) *created = false;
    if (deleted) *deleted = false;

    int slot = panel_find(incoming.key);
    if (deleting) {
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
        panel.kind = incoming.kind;
        memcpy(&panel.payload, &incoming.payload, sizeof(panel.payload));
        panel.ttl_s = incoming.ttl_s;
    }
    panel.arrived_ms = now;
    if (created) *created = is_create;
    return true;
}
/* host-test:end */

static_assert(sizeof(Panel) == 360,
              "Panel changed size: the store costs PANEL_MAX times this");
static_assert(sizeof(panel_slot) == 2880,
              "Panel store must remain a fixed 2880-byte allocation");

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
           "ASCII), `title` (string), optional `kind`, optional `ttl_s`, and "
           "optional `pos`. Missing `kind` means `text`; accepted kinds are "
           "`text`, `kv`, `bars`, `sparkline`, and `status`. Text panels use "
           "`body`. Title holds 64 bytes and body holds 256 bytes; "
           "both are cut only at a complete UTF-8 character. `ttl_s` is an "
           "unsigned 32-bit integer in 0..4294967295 seconds; 0 never expires. "
           "Expiry uses the ESP timer's 64-bit monotonic milliseconds, so the "
           "32-bit millis() rollover is irrelevant. `pos` is a signed "
           "32-bit integer and defaults to 0.\n\n"
           "Structured strings are rejected when they exceed their documented "
           "limits. `kv` has 1..5 `{label,value}` items. `bars` has 1..4 "
           "`{label,value,max,unit?}` items. `sparkline` has 2..48 numeric "
           "`values` and an optional `unit`. `status` has `value` and optional "
           "`unit` and `detail`. Unknown fields are rejected.\n\n"
           "Posting an existing key updates its title, content, kind, ttl and arrival "
           "time in place. Its original `pos` is sticky: a `pos` supplied on "
           "update is ignored. Posting an empty text body deletes an existing key; "
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

static void panel_serialize(JsonObject out, const Panel &panel) {
    out["key"] = panel.key;
    out["title"] = panel.title;
    out["kind"] = panel_kind_name(panel.kind) ? panel_kind_name(panel.kind) : "invalid";
    switch (panel.kind) {
        case PANEL_KIND_TEXT:
            out["body"] = panel.payload.body;
            break;
        case PANEL_KIND_KV: {
            JsonArray items = out["items"].to<JsonArray>();
            for (uint8_t j = 0; j < panel.payload.kv.count; j++) {
                JsonObject item = items.add<JsonObject>();
                item["label"] = panel.payload.kv.items[j].label;
                item["value"] = panel.payload.kv.items[j].value;
            }
            break;
        }
        case PANEL_KIND_BARS: {
            JsonArray items = out["items"].to<JsonArray>();
            for (uint8_t j = 0; j < panel.payload.bars.count; j++) {
                const PanelBarItem &stored = panel.payload.bars.items[j];
                JsonObject item = items.add<JsonObject>();
                item["label"] = stored.label;
                item["value"] = stored.value;
                item["max"] = stored.max;
                if (stored.unit[0]) item["unit"] = stored.unit;
            }
            break;
        }
        case PANEL_KIND_SPARKLINE: {
            JsonArray values = out["values"].to<JsonArray>();
            for (uint8_t j = 0; j < panel.payload.sparkline.count; j++)
                values.add(panel.payload.sparkline.values[j]);
            if (panel.payload.sparkline.unit[0]) out["unit"] = panel.payload.sparkline.unit;
            break;
        }
        case PANEL_KIND_STATUS:
            out["value"] = panel.payload.status.value;
            if (panel.payload.status.unit[0]) out["unit"] = panel.payload.status.unit;
            if (panel.payload.status.detail[0]) out["detail"] = panel.payload.status.detail;
            break;
        default:
            break;
    }
    out["ttl_s"] = panel.ttl_s;
    out["pos"] = panel.pos;
}

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
        panel_serialize(out, snapshot[i]);
    }
    notify_send_json(req, 200, doc);
}

static bool panel_parse_input(JsonObjectConst object, Panel *incoming,
                              bool *deleting, bool *pos_given,
                              const char **err) {
    if (!incoming || !deleting || !pos_given || !err) return false;
    *deleting = false;
    *pos_given = false;

    JsonVariantConst kind_value = object["kind"];
    PanelKind kind = PANEL_KIND_TEXT;
    if (!kind_value.isUnbound()) {
        if (!kind_value.is<const char*>() ||
            !panel_kind_parse(kind_value.as<const char*>(), &kind)) {
            *err = "kind must be one of text, kv, bars, sparkline, status";
            return false;
        }
    }
    for (JsonPairConst field : object) {
        if (!panel_field_allowed(kind, field.key().c_str())) {
            *err = "unknown field for panel kind";
            return false;
        }
    }

    JsonVariantConst key_value = object["key"];
    JsonVariantConst title_value = object["title"];
    JsonVariantConst ttl_value = object["ttl_s"];
    JsonVariantConst pos_value = object["pos"];
    if (!key_value.is<const char*>()) {
        *err = "key is required and must be a string";
        return false;
    }
    const char *key = key_value.as<const char*>();
    if (!panel_key_check(key, err)) return false;
    if (!ttl_value.isUnbound() && !ttl_value.is<uint32_t>()) {
        *err = "ttl_s must be an integer in 0..4294967295";
        return false;
    }
    *pos_given = !pos_value.isUnbound();
    if (*pos_given && !pos_value.is<int32_t>()) {
        *err = "pos must be a signed 32-bit integer";
        return false;
    }

    panel_prepare(*incoming, key,
                  title_value.is<const char*>() ? title_value.as<const char*>() : "",
                  "", ttl_value | 0U, pos_value | 0);
    incoming->kind = kind;

    if (kind == PANEL_KIND_TEXT) {
        JsonVariantConst body_value = object["body"];
        if (!body_value.is<const char*>()) {
            *err = "body is required and must be a string";
            return false;
        }
        const char *body = body_value.as<const char*>();
        *deleting = body[0] == '\0';
        if (!*deleting && !title_value.is<const char*>()) {
            *err = "title is required and must be a string";
            return false;
        }
        if (*deleting && !title_value.isUnbound() && !title_value.is<const char*>()) {
            *err = "title must be a string when supplied";
            return false;
        }
        notify_copy_text(incoming->title, sizeof(incoming->title),
                         *deleting ? "" : title_value.as<const char*>());
        notify_copy_text(incoming->payload.body, sizeof(incoming->payload.body), body);
        return true;
    }

    if (!title_value.is<const char*>()) {
        *err = "title is required and must be a string";
        return false;
    }
    notify_copy_text(incoming->title, sizeof(incoming->title),
                     title_value.as<const char*>());

    if (kind == PANEL_KIND_KV || kind == PANEL_KIND_BARS) {
        JsonVariantConst items_value = object["items"];
        if (!items_value.is<JsonArrayConst>()) {
            *err = "items is required and must be an array";
            return false;
        }
        JsonArrayConst items = items_value.as<JsonArrayConst>();
        size_t limit = kind == PANEL_KIND_KV ? PANEL_KV_MAX : PANEL_BAR_MAX;
        if (items.size() < 1 || items.size() > limit) {
            *err = kind == PANEL_KIND_KV ? "kv items must contain 1..5 entries"
                                         : "bars items must contain 1..4 entries";
            return false;
        }
        uint8_t index = 0;
        for (JsonVariantConst entry : items) {
            if (!entry.is<JsonObjectConst>()) {
                *err = "each item must be an object";
                return false;
            }
            JsonObjectConst item = entry.as<JsonObjectConst>();
            for (JsonPairConst field : item) {
                if (!panel_nested_field_allowed(kind, field.key().c_str())) {
                    *err = "unknown item field for panel kind";
                    return false;
                }
            }
            JsonVariantConst label_value = item["label"];
            if (!label_value.is<const char*>() ||
                !panel_string_check(label_value.as<const char*>(), 1, 16, err,
                                    "label must be a string of 1..16 bytes"))
                return false;
            if (kind == PANEL_KIND_KV) {
                JsonVariantConst value = item["value"];
                if (!value.is<const char*>() ||
                    !panel_string_check(value.as<const char*>(), 1, 32, err,
                                        "value must be a string of 1..32 bytes"))
                    return false;
                notify_copy_text(incoming->payload.kv.items[index].label,
                                 PANEL_LABEL_LEN, label_value.as<const char*>());
                notify_copy_text(incoming->payload.kv.items[index].value,
                                 PANEL_VALUE_LEN, value.as<const char*>());
            } else {
                JsonVariantConst value = item["value"];
                JsonVariantConst maximum = item["max"];
                JsonVariantConst unit = item["unit"];
                if (!value.is<double>() || !maximum.is<double>()) {
                    *err = "bar value and max must be numbers";
                    return false;
                }
                double number = value.as<double>();
                double max_number = maximum.as<double>();
                if (!panel_bar_values_check(number, max_number)) {
                    *err = "bar values require 0 <= value <= max <= 1e9 and max > 0";
                    return false;
                }
                if (!unit.isUnbound() && (!unit.is<const char*>() ||
                    !panel_string_check(unit.as<const char*>(), 0, 8, err,
                                        "unit must be a string of at most 8 bytes")))
                    return false;
                PanelBarItem &bar = incoming->payload.bars.items[index];
                notify_copy_text(bar.label, sizeof(bar.label),
                                 label_value.as<const char*>());
                if (!unit.isUnbound())
                    notify_copy_text(bar.unit, sizeof(bar.unit), unit.as<const char*>());
                bar.value = (float)number;
                bar.max = (float)max_number;
            }
            index++;
        }
        if (kind == PANEL_KIND_KV) incoming->payload.kv.count = index;
        else incoming->payload.bars.count = index;
        return true;
    }

    JsonVariantConst unit_value = object["unit"];
    if (!unit_value.isUnbound() && (!unit_value.is<const char*>() ||
        !panel_string_check(unit_value.as<const char*>(), 0, 8, err,
                            "unit must be a string of at most 8 bytes")))
        return false;

    if (kind == PANEL_KIND_SPARKLINE) {
        JsonVariantConst values_value = object["values"];
        if (!values_value.is<JsonArrayConst>()) {
            *err = "values is required and must be an array";
            return false;
        }
        JsonArrayConst values = values_value.as<JsonArrayConst>();
        if (values.size() < 2 || values.size() > PANEL_SPARK_MAX) {
            *err = "sparkline values must contain 2..48 entries";
            return false;
        }
        uint8_t index = 0;
        for (JsonVariantConst value : values) {
            if (!value.is<double>()) {
                *err = "sparkline values must be numbers";
                return false;
            }
            double number = value.as<double>();
            if (!panel_spark_value_check(number)) {
                *err = "sparkline values must be finite and within -1e9..1e9";
                return false;
            }
            incoming->payload.sparkline.values[index++] = (float)number;
        }
        incoming->payload.sparkline.count = index;
        if (!unit_value.isUnbound())
            notify_copy_text(incoming->payload.sparkline.unit, PANEL_UNIT_LEN,
                             unit_value.as<const char*>());
        return true;
    }

    JsonVariantConst value = object["value"];
    JsonVariantConst detail = object["detail"];
    if (!value.is<const char*>() ||
        !panel_string_check(value.as<const char*>(), 1, 24, err,
                            "status value must be a string of 1..24 bytes"))
        return false;
    if (!detail.isUnbound() && (!detail.is<const char*>() ||
        !panel_string_check(detail.as<const char*>(), 0, 64, err,
                            "detail must be a string of at most 64 bytes")))
        return false;
    notify_copy_text(incoming->payload.status.value, PANEL_STATUS_LEN,
                     value.as<const char*>());
    if (!unit_value.isUnbound())
        notify_copy_text(incoming->payload.status.unit, PANEL_UNIT_LEN,
                         unit_value.as<const char*>());
    if (!detail.isUnbound())
        notify_copy_text(incoming->payload.status.detail, PANEL_DETAIL_LEN,
                         detail.as<const char*>());
    return true;
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
    Panel incoming;
    bool deleting = false;
    bool pos_given = false;
    const char *parse_error = NULL;
    if (!panel_parse_input(input.as<JsonObjectConst>(), &incoming, &deleting,
                           &pos_given, &parse_error)) {
        notify_send_error(req, 400, parse_error ? parse_error : "invalid panel");
        return;
    }

    bool created = false;
    bool deleted = false;
    const char *apply_err = NULL;
    portENTER_CRITICAL(&panel_mux);
    bool ok = panel_core_apply(incoming, deleting, pos_given, panel_now_ms(),
                               &created, &deleted, &apply_err);
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
    "panel", "0.2.0", panel_describe, panel_endpoints, panel_register_routes
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
