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

/* --- Panel + state (file-local) --- */
static TFT_eSPI tft;

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
           "is one row down from centre, negative moves up.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"text\":\"HELLO\",\"line\":0}' http://<host>:8080/display\n"
           "```\n";
}

/*
 * Repaint the receiver status screen from the radio accessors. Sets display_mode
 * back to STATUS. Called at boot and from POST /radio/tune — always the HTTP
 * task, so the RSSI snapshot inside radio_get_rssi() races with nothing.
 *
 * Non-static: forward-declared in main.cpp so radio.cpp can call it.
 */
void display_show_status() {
    display_mode = DISPLAY_STATUS;

    const char *mode = radio_get_mode_str();
    uint16_t freq = radio_get_freq();
    uint8_t rssi = radio_get_rssi();

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);

    /* Big frequency: MHz for FM (freq is 10 kHz units), kHz otherwise. FONT7 is
     * the 7-segment face — digits and '.' only, which is all a frequency needs. */
    char freq_str[24];
    const char *unit;
    if (strcmp(mode, "FM") == 0) {
        snprintf(freq_str, sizeof(freq_str), "%.2f", freq / 100.0f);
        unit = "MHz";
    } else {
        snprintf(freq_str, sizeof(freq_str), "%u", (unsigned)freq);
        unit = "kHz";
    }
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(freq_str, DISPLAY_CX, 55, 7);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(unit, DISPLAY_CX, 105, 4);

    /* Mode + signal strength line. */
    char status_line[48];
    snprintf(status_line, sizeof(status_line), "%s   %u dBuV", mode, (unsigned)rssi);
    tft.drawString(status_line, DISPLAY_CX, 145, 4);
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
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);

    char line[64];

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    snprintf(line, sizeof(line), "AP: %s", ssid);
    tft.drawString(line, DISPLAY_CX, 30, 4);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    snprintf(line, sizeof(line), "PW: %s", pass);
    tft.drawString(line, DISPLAY_CX, 70, 4);

    /* Token in a small font: 32 hex chars is too wide for font 4. */
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("TOKEN", DISPLAY_CX, 108, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(token, DISPLAY_CX, 130, 2);
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

        display_mode = DISPLAY_CUSTOM;
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        /* line shifts vertically from centre: 30 px per row. */
        int y = 85 + line * 30;
        tft.drawString(text, DISPLAY_CX, y, 4);

        event_add("display: custom text");
        req->send(200, "application/json", "{\"ok\":true}");
    }, NULL, handle_body_collect);
}

static const Skill display_skill = {
    .name = "display",
    .version = "0.1.0",
    .describe = display_describe,
    .endpoints = display_endpoints,
    .register_routes = display_register_routes
};

/*
 * Bring the ST7789 up. Blocking, runs once at boot after skill_radio_init() so
 * the first status paint has real radio state to show. Board power (GPIO15) and
 * Wire are already up from setup() — not touched here.
 */
static void skill_display_init() {
    /* Backlight full-on via ledc (16 kHz, 8-bit). Kept off TFT_eSPI on purpose. */
    ledcAttach(PIN_LCD_BL, 16000, 8);
    ledcWrite(PIN_LCD_BL, 255);

    tft.begin();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);

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
