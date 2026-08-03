/*
 * skills/radio.cpp — SI4732 radio skill for the ATS-Mini seed (C1)
 *
 * Drives the SI4732 receiver that hangs off the I2C bus (SDA=18, SCL=17) with
 * its RESET on GPIO16. This first cut does FM + AM tuning and a signal-quality
 * status readout — no SSB, no display, no scan/RDS (those grow later).
 *
 * The receiver is brought up once, at boot, from skill_radio_init(). Wire and
 * board power are already up by then (hw_probe() runs first in setup()), so this
 * skill never touches Wire.begin() or the power-hold line.
 *
 * Endpoints:
 *   POST /radio/tune    — tune: {mode:"FM"|"AM", freq:<int>}
 *   GET  /radio/status  — current freq/mode/RSSI/SNR
 */

#include <SI4735.h>

/* --- Mode constants (match SI4735 defaultFunction: 0=FM, 1=AM) --- */
#define RADIO_MODE_FM 0
#define RADIO_MODE_AM 1

/* FM band limits in 10 kHz units: 6400=64.0 MHz .. 10800=108.0 MHz */
#define RADIO_FM_MIN 6400
#define RADIO_FM_MAX 10800
/* AM band limits in kHz: 520 kHz .. 1710 kHz (MW) */
#define RADIO_AM_MIN 520
#define RADIO_AM_MAX 1710

/* --- Receiver + state (file-local) --- */
static SI4735 rx;
static uint16_t radio_freq;          /* current frequency (FM: 10 kHz units, AM: kHz) */
static uint8_t  radio_mode;          /* RADIO_MODE_FM | RADIO_MODE_AM */
static uint8_t  radio_volume = 40;   /* 0..63 */
static bool     radio_ok = false;    /* true once the SI4732 answered on I2C */

/* --- Endpoints --- */
static const SkillEndpoint radio_endpoints[] = {
    {"POST", "/radio/tune",   "Tune: {mode:\"FM\"|\"AM\", freq:<int>}"},
    {"GET",  "/radio/status", "Current freq/mode/RSSI/SNR"},
    {NULL, NULL, NULL}
};

static const char *radio_describe() {
    return "## Skill: radio\n\n"
           "SI4732 receiver — FM and AM tuning over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /radio/tune | Tune: `{\"mode\":\"FM\",\"freq\":10000}` |\n"
           "| GET | /radio/status | Current freq, mode, RSSI, SNR |\n\n"
           "### Frequency units\n\n"
           "- FM: 10 kHz steps, so `10000` = 100.0 MHz (range 6400..10800)\n"
           "- AM: kHz, so `1000` = 1000 kHz (range 520..1710)\n\n"
           "### Examples\n\n"
           "```\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"FM\",\"freq\":10000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"AM\",\"freq\":1000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' http://<host>:8080/radio/status\n"
           "```\n\n"
           "SSB, the display and scan/RDS are not driven yet.\n";
}

/* Build a human-readable frequency string for the current mode. */
static void radio_format_freq(uint16_t freq, uint8_t mode, char *out, size_t len) {
    if (mode == RADIO_MODE_FM) {
        snprintf(out, len, "%.2f MHz", freq / 100.0f);
    } else {
        snprintf(out, len, "%u kHz", (unsigned)freq);
    }
}

static void radio_register_routes(AsyncWebServer &server) {

    /* POST /radio/tune */
    server.on("/radio/tune", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        const char *mode_str = input["mode"] | (const char*)nullptr;
        int freq = input["freq"] | -1;

        free(body); req->_tempObject = nullptr;

        if (!mode_str) {
            req->send(400, "application/json", "{\"error\":\"mode required (FM|AM)\"}");
            return;
        }
        if (freq < 0) {
            req->send(400, "application/json", "{\"error\":\"freq required\"}");
            return;
        }

        uint8_t mode;
        if (strcmp(mode_str, "FM") == 0) {
            mode = RADIO_MODE_FM;
            if (freq < RADIO_FM_MIN || freq > RADIO_FM_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"FM freq out of range (6400..10800)\"}");
                return;
            }
            rx.setFM(RADIO_FM_MIN, RADIO_FM_MAX, (uint16_t)freq, 10);
        } else if (strcmp(mode_str, "AM") == 0) {
            mode = RADIO_MODE_AM;
            if (freq < RADIO_AM_MIN || freq > RADIO_AM_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"AM freq out of range (520..1710)\"}");
                return;
            }
            rx.setAM(RADIO_AM_MIN, RADIO_AM_MAX, (uint16_t)freq, 10);
        } else {
            req->send(400, "application/json", "{\"error\":\"mode must be FM or AM\"}");
            return;
        }

        radio_mode = mode;
        radio_freq = (uint16_t)freq;

        event_add("radio: tuned %s %d", mode_str, freq);

        JsonDocument doc;
        doc["ok"] = true;
        doc["mode"] = mode_str;
        doc["freq"] = radio_freq;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* GET /radio/status */
    server.on("/radio/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!radio_ok) {
            req->send(503, "application/json", "{\"error\":\"SI4732 not detected\"}");
            return;
        }

        rx.getCurrentReceivedSignalQuality();

        char freq_display[24];
        radio_format_freq(radio_freq, radio_mode, freq_display, sizeof(freq_display));

        JsonDocument doc;
        doc["mode"] = (radio_mode == RADIO_MODE_FM) ? "FM" : "AM";
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        doc["rssi"] = rx.getCurrentRSSI();
        doc["snr"] = rx.getCurrentSNR();
        doc["volume"] = radio_volume;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });
}

static const Skill radio_skill = {
    .name = "radio",
    .version = "0.1.0",
    .describe = radio_describe,
    .endpoints = radio_endpoints,
    .register_routes = radio_register_routes
};

/*
 * Bring the SI4732 up. Blocking, runs once at boot after hw_probe() (Wire is
 * already begun). The amplifier is muted first to avoid the power-on click,
 * then re-enabled once the receiver is tuned.
 */
static void skill_radio_init() {
    /* Amplifier off first — silences the power-on click. */
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW);

    if (rx.getDeviceI2CAddress(PIN_SI4732_RST) <= 0) {
        /* Chip absent: register anyway so the routes report 503 honestly. */
        Serial.println("[radio] SI4732 not found on I2C");
        radio_ok = false;
        skill_register(&radio_skill);
        return;
    }

    rx.setup(PIN_SI4732_RST, RADIO_MODE_FM);
    rx.setAudioMuteMcuPin(PIN_AUDIO_MUTE);

    /* FM 64..108 MHz, start 100.0 MHz, 100 kHz step. */
    rx.setFM(RADIO_FM_MIN, RADIO_FM_MAX, 10000, 10);
    radio_freq = 10000;
    radio_mode = RADIO_MODE_FM;

    rx.setVolume(radio_volume);

    /* Let the receiver settle, then enable the amplifier. */
    delay(100);
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH);

    radio_ok = true;
    Serial.println("[radio] SI4732 up: FM 100.0 MHz");
    skill_register(&radio_skill);
}
