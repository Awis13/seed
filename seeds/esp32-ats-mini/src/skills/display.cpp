/*
 * skills/display.cpp — ST7789 status display skill for the ATS-Mini seed (C3)
 *
 * Drives the 170x320 parallel-8-bit ST7789 panel wired to the ESP32-S3 (pins
 * and driver quirks are set in platformio.ini's build_flags, not here). Two
 * jobs: paint a receiver status screen (frequency / mode / RSSI) and expose a
 * POST /display endpoint that prints arbitrary text.
 *
 * The status screen is event-driven, not polled: skill_radio_init() paints once
 * at boot and the radio's POST /radio/tune handler repaints on every retune via
 * the forward-declared display_show_status(). There is no background RSSI loop
 * (that grows later) — the RSSI shown is a snapshot taken at repaint time.
 *
 * Included AFTER radio.cpp (same translation unit) so it can read the radio
 * state through its accessors — radio_get_freq/mode/rssi — rather than poking at
 * the receiver directly.
 *
 * The backlight (PIN_LCD_BL, defined in main.cpp) is driven here through ledc,
 * on purpose kept out of TFT_eSPI's hands (no -DTFT_BL) so the panel never
 * flashes bright before tft.begin() has cleared it.
 *
 * Endpoints:
 *   POST /display   — show custom text: {text:"...", line?:<int>}
 */

#include <TFT_eSPI.h>

/* Accessors defined in radio.cpp (included just before this file). */
uint16_t radio_get_freq();
const char *radio_get_mode_str();
uint8_t radio_get_rssi();
uint8_t radio_get_volume();
uint8_t radio_get_tune_target();
void radio_get_signal(uint8_t *rssi, uint8_t *snr);
/* Menu accessors (radio.cpp). radio_get_menu_item wraps its index modulo the
 * active level's count, so the caller can ask for idx-2..idx+2 directly. */
uint8_t radio_get_ui_mode();
uint8_t radio_get_menu_level();
int radio_get_menu_idx();
int radio_get_menu_count();
const char *radio_get_menu_title();
const char *radio_get_menu_item(int i);

/* Mirror of radio.cpp's TuneTarget so the highlight can tell them apart. */
#define DISPLAY_TUNE_FREQ 0
#define DISPLAY_TUNE_VOLUME 1

/* Mirror of radio.cpp's UiMode enum value for the menu; display.cpp does not see
 * the enum, so the numeric value (UI_MENU is appended last, = 2) is pinned here. */
#define DISPLAY_UI_MENU 2

/* --- Panel + state (file-local) --- */
static TFT_eSPI tft;

/*
 * Off-screen back-buffer. Every repaint draws the whole frame into this sprite
 * and then blits it to the panel in one push (display_flush), so the eye never
 * catches a half-cleared screen — no more ~10 Hz black-frame flicker. The sprite
 * is a full-screen 320x170x16bpp buffer (~106 KB) parked in the 8 MB OPI PSRAM;
 * it is allocated once in skill_display_init(), never per frame.
 */
static TFT_eSprite spr = TFT_eSprite(&tft);

/*
 * Draw target: the back-buffer sprite once it is created, otherwise the panel
 * itself. All repaint code writes through this pointer. The sprite's drawing
 * entry points (drawChar/fillRect/drawPixel) are virtual overrides, so drawing
 * through a TFT_eSPI* routes correctly into the sprite; if the sprite failed to
 * allocate this stays &tft and we fall back to (flickery but visible) direct
 * panel drawing rather than losing the screen entirely.
 */
static TFT_eSPI *gfx = &tft;
static bool spr_ready = false;

/*
 * The draw target is now written from two tasks: display_show_status() and the
 * POST /display handler run on the async HTTP task, while display_tick_render()
 * runs on loopTask. The sprite (like the panel before it) is a shared resource:
 * interleaved writes corrupt both the pixels and the shared datum/colour/cursor
 * state, so a mutex serialises every repaint AND its push. It is only ever held
 * around the drawing itself — the radio snapshot is taken (and radio_mtx
 * released) before this lock is acquired, so display_mtx and radio_mtx are never
 * held at the same time.
 */
static SemaphoreHandle_t display_mtx = nullptr;

/*
 * Flush the back-buffer to the panel in one shot. Called at the end of every
 * repaint, inside the display_mtx critical section (the sprite is shared state).
 * No-op in the direct-to-panel fallback, where drawing already hit the screen.
 */
static void display_flush() {
    if (spr_ready) spr.pushSprite(0, 0);
}

/* What is currently on screen: the auto status readout or user custom text. */
#define DISPLAY_STATUS 0
#define DISPLAY_CUSTOM 1
static uint8_t display_mode = DISPLAY_STATUS;

/* Screen geometry after setRotation(3): 320 wide x 170 tall, so X centre = 160. */
#define DISPLAY_CX 160

/* --- Endpoints --- */
static const SkillEndpoint display_endpoints[] = {
    {"POST", "/display", "Show custom text: {text, line?}"},
    {NULL, NULL, NULL}
};

static const char *display_describe() {
    return "## Skill: display\n\n"
           "170x320 ST7789 panel — receiver status screen plus custom text.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /display | Show custom text: `{\"text\":\"...\",\"line\":<int>}` |\n\n"
           "### Behaviour\n\n"
           "- The status screen (frequency, mode, RSSI) is painted at boot and "
           "repainted on every `/radio/tune`. RSSI is a snapshot, not a live feed.\n"
           "- `POST /display` takes over the screen with custom text until the "
           "next tune repaints the status screen.\n"
           "- `line` (optional, default 0) shifts the text vertically: each unit "
           "is one row down from centre, negative moves up. Clamped to -2..2 so "
           "the text stays on the visible panel.\n"
           "- `text` is truncated at 40 chars; longer strings would run off the "
           "screen edges.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"text\":\"HELLO\",\"line\":0}' http://<host>:8080/display\n"
           "```\n";
}

/*
 * Paint the whole receiver readout into the draw target `gfx`. This is the single
 * home of the screen layout: the event-driven repaint (display_show_status) and
 * the tick repaint (display_tick_render) both call it, so the two paths can never
 * drift into overlapping rows. Rows use MC_DATUM (y = vertical centre) on the
 * 170 px-tall panel and are spaced so no two glyph boxes touch:
 *
 *   freq    FONT7  y= 48  (~48 px tall -> 24..72)
 *   unit    FONT4  y= 90  (~26 px tall -> 77..103)
 *   signal  FONT2  y=120  (~16 px tall -> 112..128)
 *   volume  FONT4  y=150  (~26 px tall -> 137..163, still inside 170)
 *
 * Frequency and volume are drawn bright (cyan) when each is the encoder's target
 * and dim (dark grey) otherwise, so the screen shows what the button selected.
 *
 * Draw-only helper: the caller owns display_mtx, display_mode and the flush/push.
 */
static void draw_radio_screen(const char *mode, uint16_t freq, uint8_t rssi,
                              uint8_t snr, uint8_t volume, uint8_t target) {
    uint16_t freq_color =
        (target == DISPLAY_TUNE_FREQ) ? TFT_CYAN : TFT_DARKGREY;
    uint16_t vol_color =
        (target == DISPLAY_TUNE_VOLUME) ? TFT_CYAN : TFT_DARKGREY;

    gfx->fillScreen(TFT_BLACK);
    gfx->setTextDatum(MC_DATUM);

    /* Big frequency: MHz for FM (freq is 10 kHz units), kHz otherwise. FONT7 is
     * the 7-segment face — digits and '.' only, all a frequency needs. */
    char freq_str[24];
    const char *unit;
    if (strcmp(mode, "FM") == 0) {
        snprintf(freq_str, sizeof(freq_str), "%.2f", freq / 100.0f);
        unit = "MHz";
    } else {
        snprintf(freq_str, sizeof(freq_str), "%u", (unsigned)freq);
        unit = "kHz";
    }
    gfx->setTextColor(freq_color, TFT_BLACK);
    gfx->drawString(freq_str, DISPLAY_CX, 48, 7);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    gfx->drawString(unit, DISPLAY_CX, 90, 4);

    /* Exactly one signal line: mode, RSSI in dBuV, S/N — no duplicate dBuV. */
    char status_line[48];
    snprintf(status_line, sizeof(status_line),
             "%s   %u dBuV  S/N %u", mode, (unsigned)rssi, (unsigned)snr);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    gfx->drawString(status_line, DISPLAY_CX, 120, 2);

    /* Volume at the foot, highlighted when it is the encoder's target. */
    char vol_line[24];
    snprintf(vol_line, sizeof(vol_line), "VOL %u", (unsigned)volume);
    gfx->setTextColor(vol_color, TFT_BLACK);
    gfx->drawString(vol_line, DISPLAY_CX, 150, 4);
}

/*
 * Paint the modal menu into the draw target `gfx`: a small bordered panel on the
 * left of the screen with a title, a divider and a five-row scrolling window
 * centred on the current cursor. The middle row (i==0) is the selection — drawn
 * dark-on-highlight — and the four neighbours are dimmer context above and below.
 * Rows wrap around the list (radio_get_menu_item wraps its index), so a short list
 * simply repeats to fill the window. Geometry is in the 320x170 landscape space.
 *
 * Draw-only helper: the caller owns display_mtx and the flush/push. The text datum
 * is left at MC_DATUM (the shared repaint default) on the way out.
 */
static void draw_menu_screen(const char *title, int idx) {
    gfx->fillScreen(TFT_BLACK);

    /* Bordered panel: a filled cyan roundrect with a black roundrect inset one
     * pixel, giving a 1 px cyan frame around the black menu body. */
    gfx->fillSmoothRoundRect(1, 19, 86, 110, 4, TFT_CYAN, TFT_BLACK);
    gfx->fillSmoothRoundRect(2, 20, 84, 108, 4, TFT_BLACK, TFT_BLACK);

    /* Title, centred in the panel, with a divider line under it. */
    gfx->setTextDatum(MC_DATUM);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    gfx->drawString(title, 45, 30, 2);
    gfx->drawLine(1, 41, 86, 41, TFT_CYAN);

    /* Selection bar behind the centre row. */
    gfx->fillRoundRect(6, 74, 76, 16, 2, TFT_CYAN);

    /* Five rows, i=-2..+2, 16 px apart around the cursor. The middle one is the
     * highlighted selection (dark text on the cyan bar); the rest are plain. */
    for (int i = -2; i <= 2; i++) {
        const char *item = radio_get_menu_item(idx + i);
        int y = 82 + i * 16;
        if (i == 0) {
            gfx->setTextColor(TFT_BLACK, TFT_CYAN);
        } else {
            gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        }
        gfx->drawString(item, 45, y, 2);
    }

    /* Restore the shared repaint colour default; datum stays MC_DATUM. */
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
}

/*
 * Repaint the receiver status screen from the radio accessors. Sets display_mode
 * back to STATUS. Called at boot and from POST /radio/tune — always the HTTP
 * task, so the snapshot below races with nothing.
 *
 * Non-static: forward-declared in main.cpp so radio.cpp can call it.
 */
void display_show_status() {
    /* Snapshot radio state first — the accessors take radio_mtx briefly and
     * release it, so we never hold radio_mtx and display_mtx together. Take the
     * same fields the tick path does so both render the identical layout. */
    const char *mode = radio_get_mode_str();
    uint16_t freq = radio_get_freq();
    uint8_t volume = radio_get_volume();
    uint8_t target = radio_get_tune_target();
    uint8_t rssi = 0, snr = 0;
    radio_get_signal(&rssi, &snr);

    xSemaphoreTake(display_mtx, portMAX_DELAY);
    display_mode = DISPLAY_STATUS;
    draw_radio_screen(mode, freq, rssi, snr, volume, target);
    display_flush();
    xSemaphoreGive(display_mtx);
}

/*
 * Live handheld readout, repainted from radio_tick() at ~10 Hz. Unlike
 * display_show_status() (a one-shot repaint on HTTP tune) this is the screen you
 * watch while turning the knob: it shows the current frequency, mode, signal and
 * volume, and highlights whichever the encoder is driving (frequency or volume)
 * so you can see what the button last selected.
 *
 * It reads only the radio accessors — the signal read takes radio_mtx briefly
 * inside radio_get_signal(); the TFT itself never touches the receiver, so no
 * lock is held across the (relatively slow) repaint. A custom /display screen is
 * left alone: this returns immediately while display_mode is CUSTOM.
 *
 * Non-static: forward-declared in main.cpp, called from radio.cpp's tick.
 */
void display_tick_render() {
    /* Snapshot the radio state up front (accessors lock radio_mtx internally and
     * release it) before touching display_mtx, so the two locks never nest. */
    const char *mode = radio_get_mode_str();
    uint16_t freq = radio_get_freq();
    uint8_t volume = radio_get_volume();
    uint8_t target = radio_get_tune_target();
    uint8_t ui = radio_get_ui_mode();
    uint8_t menu_level = radio_get_menu_level();
    int menu_idx = radio_get_menu_idx();
    uint8_t rssi = 0, snr = 0;
    radio_get_signal(&rssi, &snr);

    /* Test-and-set display_mode under the same lock that guards the repaint, so a
     * concurrent POST /display can't flip to CUSTOM between our check and draw. */
    xSemaphoreTake(display_mtx, portMAX_DELAY);
    if (display_mode == DISPLAY_CUSTOM) { xSemaphoreGive(display_mtx); return; }
    display_mode = DISPLAY_STATUS;

    /* Change-detection: this fires at ~10 Hz, but the screen only needs a fresh
     * frame when something it shows actually moved. Skip the full redraw+push
     * when every displayed field matches the last one and we pushed recently — a
     * cheap forced refresh every DISPLAY_MAX_IDLE_MS still covers anything missed.
     * These statics are only ever touched here, always under display_mtx. */
    static uint16_t prev_freq = 0xFFFF;
    static uint8_t prev_rssi = 0xFF, prev_snr = 0xFF, prev_volume = 0xFF,
                   prev_target = 0xFF, prev_ui = 0xFF, prev_menu_level = 0xFF;
    static int prev_menu_idx = -1;
    static const char *prev_mode = nullptr;
    static uint32_t last_push_ms = 0;
    const uint32_t DISPLAY_MAX_IDLE_MS = 2000;

    uint32_t now = millis();
    /* The menu fields (ui/level/idx) join the change set so scrolling the menu, or
     * entering/leaving it, forces a fresh frame instead of being read as unchanged. */
    bool unchanged = freq == prev_freq && rssi == prev_rssi && snr == prev_snr &&
                     volume == prev_volume && target == prev_target &&
                     ui == prev_ui && menu_level == prev_menu_level &&
                     menu_idx == prev_menu_idx &&
                     prev_mode != nullptr && strcmp(mode, prev_mode) == 0;
    if (unchanged && (now - last_push_ms) < DISPLAY_MAX_IDLE_MS) {
        xSemaphoreGive(display_mtx);
        return;
    }
    prev_freq = freq;
    prev_rssi = rssi;
    prev_snr = snr;
    prev_volume = volume;
    prev_target = target;
    prev_ui = ui;
    prev_menu_level = menu_level;
    prev_menu_idx = menu_idx;
    prev_mode = mode;
    last_push_ms = now;

    if (ui == DISPLAY_UI_MENU) {
        draw_menu_screen(radio_get_menu_title(), menu_idx);
    } else {
        draw_radio_screen(mode, freq, rssi, snr, volume, target);
    }
    display_flush();
    xSemaphoreGive(display_mtx);
}

/*
 * Provisioning screen: the setup AP's SSID and one-boot password, plus the auth
 * token. Painted at boot instead of the status screen when the node came up on
 * its setup AP (no stored WiFi, or the stored credentials failed). The password
 * exists only in RAM and on this screen — it is never persisted or sent over the
 * wire, so this panel is the only place a human can read it.
 *
 * Non-static: forward-declared in main.cpp, called from skill_display_init().
 */
void display_show_ap(const char *ssid, const char *pass, const char *token) {
    gfx->fillScreen(TFT_BLACK);
    gfx->setTextDatum(MC_DATUM);

    char line[64];

    gfx->setTextColor(TFT_CYAN, TFT_BLACK);
    snprintf(line, sizeof(line), "AP: %s", ssid);
    gfx->drawString(line, DISPLAY_CX, 30, 4);

    gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
    snprintf(line, sizeof(line), "PW: %s", pass);
    gfx->drawString(line, DISPLAY_CX, 70, 4);

    /* Token in a small font: 32 hex chars is too wide for font 4. */
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    gfx->drawString("TOKEN", DISPLAY_CX, 108, 2);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    gfx->drawString(token, DISPLAY_CX, 130, 2);
    display_flush();
}

static void display_register_routes(AsyncWebServer &server) {

    /* POST /display — take over the screen with custom text. */
    server.on("/display", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        const char *text = input["text"] | (const char*)nullptr;
        int line = input["line"] | 0;

        free(body); req->_tempObject = nullptr;

        if (!text) {
            req->send(400, "application/json", "{\"error\":\"text required\"}");
            return;
        }

        /* Clamp `line` to the rows that actually fit the 170 px-tall panel. y is
         * the MC_DATUM vertical centre = 85 + line*30 and a font-4 glyph box is
         * ~26 px tall, so line -2..2 (y 25..145) keeps every row fully on-screen.
         * Out-of-range lines used to draw silently off-screen and still return
         * 200 — clamp so the text stays visible instead of vanishing. */
        if (line < -2) line = -2;
        else if (line > 2) line = 2;

        /* Cap the text length into a bounded buffer: a very long string spilled
         * off both screen edges (and pushed an unbounded pointer through the draw
         * path). 40 chars is well past what fits on one row; truncate rather than
         * crash or overflow. */
        char text_buf[41];
        snprintf(text_buf, sizeof(text_buf), "%s", text);

        /* Take over the screen under display_mtx so the drawing can't interleave
         * with a tick-driven status repaint on loopTask. */
        xSemaphoreTake(display_mtx, portMAX_DELAY);
        display_mode = DISPLAY_CUSTOM;
        gfx->fillScreen(TFT_BLACK);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->setTextDatum(MC_DATUM);
        /* line shifts vertically from centre: 30 px per row. */
        int y = 85 + line * 30;
        gfx->drawString(text_buf, DISPLAY_CX, y, 4);
        display_flush();
        xSemaphoreGive(display_mtx);

        event_add("display: custom text");
        req->send(200, "application/json", "{\"ok\":true}");
    }, NULL, handle_body_collect);
}

static const Skill display_skill = {
    .name = "display",
    .version = "0.1.0",
    .describe = display_describe,
    .endpoints = display_endpoints,
    .register_routes = display_register_routes,
    .tick = nullptr,
};

/*
 * Bring the ST7789 up. Blocking, runs once at boot after skill_radio_init() so
 * the first status paint has real radio state to show. Board power (GPIO15) and
 * Wire are already up from setup() — not touched here.
 */
static void skill_display_init() {
    /* Guard the panel before anything can repaint it from two tasks. Created here,
     * ahead of the first paint below and well before server.begin()/tick(), so no
     * repaint path ever sees a null mutex. The boot paint runs single-tasked. */
    display_mtx = xSemaphoreCreateMutex();

    /* Backlight full-on via ledc (16 kHz, 8-bit). Kept off TFT_eSPI on purpose. */
    ledcAttach(PIN_LCD_BL, 16000, 8);
    ledcWrite(PIN_LCD_BL, 255);

    tft.begin();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);

    /* Allocate the full-screen back-buffer ONCE, here — never in the tick/repaint
     * path (that would re-allocate ~106 KB every frame). 320x170 is the geometry
     * after setRotation(3); the buffer lands in OPI PSRAM. createSprite() returns
     * the pixel buffer or nullptr if the allocation failed: on success switch the
     * draw target to the sprite for flicker-free double buffering, on failure log
     * it and keep drawing straight to the panel so the screen is never lost. */
    if (spr.createSprite(320, 170) != nullptr) {
        spr.setSwapBytes(true);
        spr.setTextDatum(MC_DATUM);
        gfx = &spr;
        spr_ready = true;
    } else {
        Serial.println("[display] sprite alloc failed — direct-to-panel fallback");
    }

    // On the setup AP (no working WiFi) the provisioning screen carries the AP
    // password and token; otherwise show the receiver status readout.
    if (ap_active) {
        display_show_ap(ap_ssid.c_str(), ap_password.c_str(), auth_token.c_str());
    } else {
        display_show_status();
    }

    Serial.println("[display] ST7789 up");
    skill_register(&display_skill);
}
