/*
 * skills/backlight.cpp — AW9364 level + idle dim/blank (port of tembed policy).
 *
 * GPIO42 is the AW9364 one-wire brightness pin on T-Lora Pager (same part as
 * T-Embed GPIO21). Pulse count 1..16, 0 = off. Policy:
 *   - under 30s idle → level the person set
 *   - past 30s → dimmest preset (4 / "night"), never brighter than set
 *   - past 120s → off (crit unread floors at dim; progress bar exempt)
 * Any input or an arriving notify stamps the idle clock in main.
 *
 * Policy decisions never overwrite the stored setting: turning idle off
 * restores what was asked for within one poll.
 */

#include <driver/gpio.h>

#define BL_HI_US        1
#define BL_LO_US        1
#define BL_ON_US        30
#define BL_OFF_MS       3

#define BL_CFG_DELAY_MS 1500
#define BL_CFG_FILE     "/backlight.json"
#define BL_CFG_TMP      "/backlight.tmp"

#define BL_STEPS        16
#define BL_CHANNELS     4
#define BL_STEP_UA      1250
#define BL_DEFAULT      BL_STEPS

#define BL_IDLE_DIM_S   30
#define BL_IDLE_OFF_S   120
#define BL_IDLE_DIM_MS  (BL_IDLE_DIM_S * 1000UL)
#define BL_IDLE_OFF_MS  (BL_IDLE_OFF_S * 1000UL)
#define BL_IDLE_LEVEL   4
#define BL_IDLE_DEFAULT true

/* host-test region: pure arithmetic helpers */
static uint8_t bl_edge_of(uint8_t level) {
    return (uint8_t)(BL_STEPS + 1 - level);
}
static uint32_t bl_current_ua(uint8_t level) {
    if (level == 0 || level > BL_STEPS) return 0;
    return (uint32_t)level * BL_STEP_UA;
}
static uint32_t bl_total_ua(uint8_t level) {
    return bl_current_ua(level) * BL_CHANNELS;
}
static uint8_t bl_pulses(uint8_t from, uint8_t to) {
    if (from == 0 || to == 0 || from > BL_STEPS || to > BL_STEPS) return 0;
    return (uint8_t)((BL_STEPS + (int)from - (int)to) % BL_STEPS);
}
static bool bl_level_valid(int level) {
    return level >= 0 && level <= BL_STEPS;
}

struct BacklightPreset {
    uint8_t level;
    const char *name;
};
static const BacklightPreset bl_preset_table[] = {
    {16, "full"},
    {11, "day"},
    { 7, "room"},
    { 4, "night"},
};
#define BL_PRESET_COUNT ((int)(sizeof(bl_preset_table) / sizeof(bl_preset_table[0])))

static const char *bl_preset_name(uint8_t level) {
    for (int i = 0; i < BL_PRESET_COUNT; i++)
        if (bl_preset_table[i].level == level) return bl_preset_table[i].name;
    return NULL;
}
static uint8_t bl_preset_next(uint8_t level) {
    for (int i = 0; i < BL_PRESET_COUNT; i++)
        if (bl_preset_table[i].level < level) return bl_preset_table[i].level;
    return bl_preset_table[0].level;
}

#define BL_LABEL_MAX 9
static void bl_level_label(uint8_t level, char *out, size_t n) {
    if (level == 0) {
        snprintf(out, n, "off");
        return;
    }
    const char *name = bl_preset_name(level);
    if (name) snprintf(out, n, "%s", name);
    else      snprintf(out, n, "step %u", (unsigned)level);
}

struct BacklightIdle {
    bool on;
    unsigned long dim_ms;
    unsigned long off_ms;
    uint8_t dim_level;
};
static BacklightIdle bl_idle_policy(bool on) {
    BacklightIdle p;
    p.on = on;
    p.dim_ms = BL_IDLE_DIM_MS;
    p.off_ms = BL_IDLE_OFF_MS;
    p.dim_level = BL_IDLE_LEVEL;
    return p;
}

struct BacklightPanel {
    bool on_bar;       /* progress band on clock */
    bool crit_unread;  /* floor blank, still allow dim */
};

static uint8_t bl_idle_level(const BacklightIdle &p, uint8_t manual,
                             unsigned long since_ms, const BacklightPanel &s) {
    if (!p.on) return manual;
    if (s.on_bar) return manual;
    if (since_ms >= p.off_ms && !s.crit_unread) return 0;
    if (since_ms >= p.dim_ms) return p.dim_level < manual ? p.dim_level : manual;
    return manual;
}

/* --- state --- */
static volatile uint8_t bl_wanted = BL_DEFAULT;
static uint8_t bl_applied = 0;
static bool bl_ready = false;
static volatile bool bl_idle_on = BL_IDLE_DEFAULT;
static volatile uint8_t bl_shown = BL_DEFAULT;
static unsigned long bl_off_at = 0;
static volatile bool bl_cfg_dirty = false;
static volatile unsigned long bl_cfg_save_at = 0;
static portMUX_TYPE bl_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void bl_pin_high() {
    gpio_set_level((gpio_num_t)PIN_DISP_BL, 1);
}
static inline void bl_pin_low() {
    gpio_set_level((gpio_num_t)PIN_DISP_BL, 0);
}

static bool bl_drive(uint8_t want) {
    if (want == 0) {
        bl_pin_low();
        bl_off_at = millis();
        bl_applied = 0;
        return true;
    }
    if (bl_applied == 0) {
        if (millis() - bl_off_at < BL_OFF_MS) return false;
        bl_pin_high();
        delayMicroseconds(BL_ON_US);
        bl_applied = BL_STEPS;
    }
    uint8_t n = bl_pulses(bl_applied, want);
    if (n > 0) {
        portENTER_CRITICAL(&bl_mux);
        for (uint8_t i = 0; i < n; i++) {
            bl_pin_low();
            delayMicroseconds(BL_LO_US);
            bl_pin_high();
            delayMicroseconds(BL_HI_US);
        }
        portEXIT_CRITICAL(&bl_mux);
    }
    bl_applied = want;
    return true;
}

static void bl_cfg_save() {
    JsonDocument doc;
    doc["level"] = bl_wanted;
    doc["idle"] = bl_idle_on;
    String out;
    serializeJson(doc, out);
    write_spiffs_file_atomic(BL_CFG_FILE, BL_CFG_TMP, out);
}

static void bl_cfg_mark_dirty() {
    unsigned long at = millis() + BL_CFG_DELAY_MS;
    if (!bl_cfg_dirty || (long)(at - bl_cfg_save_at) < 0) bl_cfg_save_at = at;
    bl_cfg_dirty = true;
}

static void bl_cfg_load() {
    String raw = read_spiffs_file(BL_CFG_FILE);
    if (raw.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
    if (doc["level"].is<int>()) {
        int lv = doc["level"].as<int>();
        if (bl_level_valid(lv)) bl_wanted = (uint8_t)lv;
    }
    if (doc["idle"].is<bool>()) bl_idle_on = doc["idle"].as<bool>();
}

static void backlight_idle(unsigned long since_input_ms,
                           const BacklightPanel &panel) {
    bl_shown = bl_idle_level(bl_idle_policy(bl_idle_on), bl_wanted,
                             since_input_ms, panel);
}

/* Dark because of idle policy (not because level was set to 0 by hand). */
static bool backlight_blanked() { return bl_shown == 0 && bl_wanted != 0; }

/* Defined in main.cpp after the skill includes (same TU): full redraw of the
 * current face after tft_wake(), before the backlight rises. */
static void ui_blank_wake_repaint();

/* Keyboard backlight (GPIO46) follows the panel: a dark screen with a lit
 * keypad is a pocket flashlight. Saved across one blank/wake so a user who
 * turned the keys off stays off. */
static uint8_t kb_bl_saved = 0;
static bool kb_bl_parked = false;

static void kb_bl_follow_blank() {
    if (!kb_bl_parked) {
        kb_bl_saved = hw_kb_get_backlight();
        kb_bl_parked = true;
    }
    hw_kb_set_backlight(0);
}

static void kb_bl_follow_wake() {
    if (!kb_bl_parked) return;
    hw_kb_set_backlight(kb_bl_saved);
    kb_bl_parked = false;
}

static void backlight_poll() {
    if (!bl_ready) return;
    uint8_t shown = bl_shown;
    if (shown != bl_applied) {
        if (shown == 0) {
            /* Blank: both lights to 0 first, then sleep the ST7796
               (vendor setBrightness(0) auto-sleeps the panel the same way). */
            kb_bl_follow_blank();
            bl_drive(0);
            tft_sleep();
        } else if (bl_applied == 0) {
            /* Wake from blank, vendor order: panel out of sleep (SLPOUT +
               120 ms inside tft_wake), repaint the face, and only then raise
               the backlight — the lit panel never shows a stale frame. */
            tft_wake();
            ui_blank_wake_repaint();
            bl_drive(shown);
            kb_bl_follow_wake();
        } else {
            bl_drive(shown);
        }
    }
    if (bl_cfg_dirty && (long)(millis() - bl_cfg_save_at) >= 0) {
        bl_cfg_dirty = false;
        bl_cfg_save();
    }
}

static void backlight_menu_click() {
    uint8_t next = bl_preset_next(bl_wanted);
    if (next == bl_wanted) return;
    bl_wanted = next;
    bl_cfg_mark_dirty();
    char label[BL_LABEL_MAX];
    bl_level_label(next, label, sizeof(label));
    event_add("backlight: %s (%u/%u, %lu mA)", label, (unsigned)next,
              (unsigned)BL_STEPS, (unsigned long)(bl_total_ua(next) / 1000));
}

static const char *bl_idle_word() {
    return bl_idle_on ? "on" : "off";
}

static void backlight_idle_set(bool on) {
    bl_idle_on = on;
    bl_cfg_mark_dirty();
    event_add("backlight: auto-dim %s (%lus, off %lus)", bl_idle_word(),
              (unsigned long)BL_IDLE_DIM_S, (unsigned long)BL_IDLE_OFF_S);
}

static void backlight_idle_toggle() { backlight_idle_set(!bl_idle_on); }

static uint8_t backlight_wanted() { return bl_wanted; }
static uint8_t backlight_shown() { return bl_shown; }

static void backlight_state_json(JsonDocument &doc) {
    uint8_t level = bl_wanted;
    doc["ready"] = bl_ready;
    doc["pin"] = PIN_DISP_BL;
    doc["level"] = level;
    doc["steps"] = BL_STEPS;
    doc["on"] = level > 0;
    if (level > 0) doc["chip_step"] = bl_edge_of(level);
    const char *name = bl_preset_name(level);
    if (name) doc["preset"] = name;
    doc["led_ma"] = bl_current_ua(level) / 1000.0;
    doc["total_ma"] = bl_total_ua(level) / 1000.0;
    JsonArray presets = doc["presets"].to<JsonArray>();
    for (int i = 0; i < BL_PRESET_COUNT; i++) {
        JsonObject p = presets.add<JsonObject>();
        p["name"] = bl_preset_table[i].name;
        p["level"] = bl_preset_table[i].level;
    }
    doc["shown"] = bl_shown;
    doc["idle"] = bl_idle_on;
    JsonObject policy = doc["idle_policy"].to<JsonObject>();
    policy["dim_after_ms"] = BL_IDLE_DIM_MS;
    policy["off_after_ms"] = BL_IDLE_OFF_MS;
    policy["dim_level"] = BL_IDLE_LEVEL;
    const char *dim_name = bl_preset_name(BL_IDLE_LEVEL);
    if (dim_name) policy["dim_preset"] = dim_name;
}

#define BL_STR_(x) #x
#define BL_STR(x)  BL_STR_(x)

static const SkillEndpoint backlight_endpoints[] = {
    {"GET",  "/backlight", "Backlight level, presets, idle policy"},
    {"POST", "/backlight", "Set {level} 0..16 and/or {idle} auto-dim"},
    {NULL, NULL, NULL}
};

static const char *backlight_describe() {
    return "## Skill: backlight\n\n"
           "AW9364 pulse-count brightness on GPIO" BL_STR(PIN_DISP_BL) ".\n"
           "Level 16 = full, 1 = dimmest, 0 = off. Four presets on the device:\n"
           "full/day/room/night. Full 0..16 over HTTP.\n\n"
           "### Idle auto-dim\n\n"
           "No input → dim step (level " BL_STR(BL_IDLE_LEVEL) ") after **"
           BL_STR(BL_IDLE_DIM_S) "s**, off after **" BL_STR(BL_IDLE_OFF_S)
           "s**. Knob, key, or arriving notify wakes. Crit unread floors at\n"
           "dim (no full blank). Progress bar keeps the panel readable.\n"
           "`{\"idle\":false}` hands brightness back to the manual level.\n\n"
           "| GET/POST | /backlight |\n";
}

static void backlight_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/backlight"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        backlight_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/backlight"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON with level and/or idle");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }
        bool has_level = !input["level"].isNull();
        bool has_idle = !input["idle"].isNull();
        if (!has_level && !has_idle) {
            notify_send_error(req, 400, "body must carry level, idle, or both");
            return;
        }
        int level = 0;
        if (has_level) {
            if (!input["level"].is<int>()) {
                notify_send_error(req, 400, "level must be 0..16");
                return;
            }
            level = input["level"].as<int>();
            if (!bl_level_valid(level)) {
                notify_send_error(req, 400, "level must be 0..16");
                return;
            }
        }
        if (has_idle && !input["idle"].is<bool>()) {
            notify_send_error(req, 400, "idle must be true or false");
            return;
        }
        if (has_level && (uint8_t)level != bl_wanted) {
            bl_wanted = (uint8_t)level;
            bl_cfg_mark_dirty();
            char label[BL_LABEL_MAX];
            bl_level_label((uint8_t)level, label, sizeof(label));
            event_add("backlight: %s (%u/%u, %lu mA)", label, (unsigned)level,
                      (unsigned)BL_STEPS,
                      (unsigned long)(bl_total_ua((uint8_t)level) / 1000));
        }
        if (has_idle && input["idle"].as<bool>() != bl_idle_on) {
            backlight_idle_set(input["idle"].as<bool>());
        }
        JsonDocument doc;
        doc["ok"] = true;
        backlight_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, handle_body_collect);
}

static const Skill backlight_skill = {
    .name = "backlight",
    .version = "0.1.0",
    .describe = backlight_describe,
    .endpoints = backlight_endpoints,
    .register_routes = backlight_register_routes,
    .tick = nullptr
};

/* Call after the panel is up (and any early boot bl_set). Reclaims the pin. */
static void backlight_begin() {
    pinMode(PIN_DISP_BL, OUTPUT);
    bl_pin_low();
    bl_off_at = millis();
    bl_ready = true;
    bl_applied = 0;
    bl_cfg_load();
    bl_shown = bl_wanted;
    while (!bl_drive(bl_wanted)) delay(1);
    /* A saved level 0 must not leave the panel controller awake under a dark
       screen: sleep it at boot exactly as the idle blanker would. */
    if (bl_wanted == 0) tft_sleep();
    char label[BL_LABEL_MAX];
    bl_level_label(bl_wanted, label, sizeof(label));
    event_add("backlight: %s (%u/%u, %lu mA), auto-dim %s", label,
              (unsigned)bl_wanted, (unsigned)BL_STEPS,
              (unsigned long)(bl_total_ua(bl_wanted) / 1000), bl_idle_word());
}

static void skill_backlight_init() {
    skill_register(&backlight_skill);
}
