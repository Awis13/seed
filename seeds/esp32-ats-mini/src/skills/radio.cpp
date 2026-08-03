/*
 * skills/radio.cpp — SI4732 radio skill for the ATS-Mini seed (C1 + C2)
 *
 * Drives the SI4732 receiver that hangs off the I2C bus (SDA=18, SCL=17) with
 * its RESET on GPIO16. This cut does FM + AM + SSB (LSB/USB) tuning and a
 * signal-quality status readout — no display, no scan/RDS (those grow later).
 *
 * The receiver is brought up once, at boot, from skill_radio_init(). Wire and
 * board power are already up by then (hw_probe() runs first in setup()), so this
 * skill never touches Wire.begin() or the power-hold line.
 *
 * SSB needs a firmware patch (ssb_patch_content, from patch_init.h) uploaded to
 * the SI4732 RAM. That patch is volatile — it is dropped on any power-down and,
 * in practice, whenever we drive the chip back to FM/AM. So we load it lazily on
 * the first SSB tune and clear radio_ssb_loaded whenever we leave SSB, forcing a
 * fresh reload next time. The boot path stays FM and never touches the patch.
 *
 * Endpoints:
 *   POST /radio/tune    — tune: {mode:"FM"|"AM"|"LSB"|"USB", freq:<int>, bfo?:<int>}
 *   GET  /radio/status  — current freq/mode/RSSI/SNR (+bfo in SSB)
 */

#include <SI4735.h>
#include "../patch_init.h"

/*
 * Mode constants follow the ref ats-mini numbering so the SSB mode value doubles
 * as setSSB()'s usblsb argument (1=LSB, 2=USB) — no separate mapping needed.
 * FM stays 0 (SI4735 defaultFunction for setup()); AM moved 1 -> 3. Modes travel
 * over HTTP as strings, so this internal renumbering is not visible to callers.
 */
#define RADIO_MODE_FM  0
#define RADIO_MODE_LSB 1
#define RADIO_MODE_USB 2
#define RADIO_MODE_AM  3

/* FM band limits in 10 kHz units: 6400=64.0 MHz .. 10800=108.0 MHz */
#define RADIO_FM_MIN 6400
#define RADIO_FM_MAX 10800
/* AM band limits in kHz: 520 kHz .. 1710 kHz (MW) */
#define RADIO_AM_MIN 520
#define RADIO_AM_MAX 1710
/* SSB band window in kHz: 1.8 MHz .. 30 MHz (whole HF range) */
#define RADIO_SSB_MIN 1800
#define RADIO_SSB_MAX 30000
/* SSB fine-tuning (BFO) limit in Hz, applied symmetrically (-MAX..+MAX) */
#define RADIO_BFO_MAX 14000

/* --- Receiver + state (file-local) --- */
static SI4735 rx;
static uint16_t radio_freq;              /* current frequency (FM: 10 kHz units, AM/SSB: kHz) */
static uint8_t  radio_mode;              /* RADIO_MODE_FM | _LSB | _USB | _AM */
static uint8_t  radio_volume = 40;       /* 0..63 */
static bool     radio_ok = false;        /* true once the SI4732 answered on I2C */
static bool     radio_ssb_loaded = false;/* true while the SSB patch is live in chip RAM */
static int      radio_bfo = 0;           /* current BFO offset in Hz (SSB only) */

/* --- Endpoints --- */
static const SkillEndpoint radio_endpoints[] = {
    {"POST", "/radio/tune",   "Tune: {mode:\"FM\"|\"AM\"|\"LSB\"|\"USB\", freq:<int>, bfo?:<int>}"},
    {"GET",  "/radio/status", "Current freq/mode/RSSI/SNR (+bfo in SSB)"},
    {NULL, NULL, NULL}
};

static const char *radio_describe() {
    return "## Skill: radio\n\n"
           "SI4732 receiver — FM, AM and SSB (LSB/USB) tuning over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /radio/tune | Tune: `{\"mode\":\"FM|AM|LSB|USB\",\"freq\":<int>,\"bfo\":<int>}` |\n"
           "| GET | /radio/status | Current freq, mode, RSSI, SNR (+bfo in SSB) |\n\n"
           "### Frequency units\n\n"
           "- FM: 10 kHz steps, so `10000` = 100.0 MHz (range 6400..10800)\n"
           "- AM: kHz, so `1000` = 1000 kHz (range 520..1710)\n"
           "- LSB/USB: kHz, so `14074` = 14074 kHz (range 1800..30000)\n\n"
           "### SSB fine-tuning\n\n"
           "- `bfo` (optional, SSB only): BFO offset in Hz, range -14000..14000, default 0\n\n"
           "### Examples\n\n"
           "```\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"FM\",\"freq\":10000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"AM\",\"freq\":1000}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"USB\",\"freq\":14074}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' -X POST \\\n"
           "  -d '{\"mode\":\"LSB\",\"freq\":3750,\"bfo\":500}' http://<host>:8080/radio/tune\n"
           "curl -H 'Authorization: Bearer <token>' http://<host>:8080/radio/status\n"
           "```\n\n"
           "The display and scan/RDS are not driven yet.\n";
}

/* Return true when the mode is one of the two SSB sidebands. */
static bool radio_is_ssb(uint8_t mode) {
    return mode == RADIO_MODE_LSB || mode == RADIO_MODE_USB;
}

/* Map a mode constant to its wire string (FM|LSB|USB|AM). */
static const char *radio_mode_str(uint8_t mode) {
    switch (mode) {
        case RADIO_MODE_FM:  return "FM";
        case RADIO_MODE_LSB: return "LSB";
        case RADIO_MODE_USB: return "USB";
        default:             return "AM";
    }
}

/* Build a human-readable frequency string for the current mode. */
static void radio_format_freq(uint16_t freq, uint8_t mode, char *out, size_t len) {
    if (mode == RADIO_MODE_FM) {
        snprintf(out, len, "%.2f MHz", freq / 100.0f);
    } else if (radio_is_ssb(mode)) {
        snprintf(out, len, "%u kHz %s", (unsigned)freq, radio_mode_str(mode));
    } else {
        snprintf(out, len, "%u kHz", (unsigned)freq);
    }
}

/* --- State accessors for the display skill (avoids reaching into rx/state) ---
 * All three are only ever called from the HTTP task (tune handler / display
 * routes), so the RSSI snapshot below races with nothing. */
uint16_t radio_get_freq() { return radio_freq; }

const char *radio_get_mode_str() { return radio_mode_str(radio_mode); }

uint8_t radio_get_rssi() {
    if (!radio_ok) return 0;
    rx.getCurrentReceivedSignalQuality();
    return rx.getCurrentRSSI();
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
        int req_bfo = input["bfo"] | 0;

        free(body); req->_tempObject = nullptr;

        if (!mode_str) {
            req->send(400, "application/json", "{\"error\":\"mode required (FM|AM|LSB|USB)\"}");
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
            radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        } else if (strcmp(mode_str, "AM") == 0) {
            mode = RADIO_MODE_AM;
            if (freq < RADIO_AM_MIN || freq > RADIO_AM_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"AM freq out of range (520..1710)\"}");
                return;
            }
            rx.setAM(RADIO_AM_MIN, RADIO_AM_MAX, (uint16_t)freq, 10);
            radio_ssb_loaded = false;  /* SSB patch is dropped when we leave SSB */
        } else if (strcmp(mode_str, "LSB") == 0 || strcmp(mode_str, "USB") == 0) {
            mode = (strcmp(mode_str, "LSB") == 0) ? RADIO_MODE_LSB : RADIO_MODE_USB;
            if (freq < RADIO_SSB_MIN || freq > RADIO_SSB_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"SSB freq out of range (1800..30000)\"}");
                return;
            }
            if (req_bfo < -RADIO_BFO_MAX || req_bfo > RADIO_BFO_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"bfo out of range (-14000..14000)\"}");
                return;
            }
            /* Lazily upload the SSB patch (audiobw=1 -> 2.2 kHz) on first SSB use. */
            if (!radio_ssb_loaded) {
                rx.loadPatch(ssb_patch_content, sizeof(ssb_patch_content), 1);
                radio_ssb_loaded = true;
            }
            /* step=0 in SSB; mode value doubles as usblsb (1=LSB, 2=USB). */
            rx.setSSB(RADIO_SSB_MIN, RADIO_SSB_MAX, (uint16_t)freq, 0, mode);
            rx.setSSBAutomaticVolumeControl(1);
            radio_bfo = req_bfo;
            rx.setSSBBfo(-radio_bfo);  /* sign follows the ref convention */
        } else {
            req->send(400, "application/json", "{\"error\":\"mode must be FM, AM, LSB or USB\"}");
            return;
        }

        radio_mode = mode;
        radio_freq = (uint16_t)freq;

        event_add("radio: tuned %s %d", mode_str, freq);

        /* Reflect the new tune on the status screen (defined in display.cpp,
         * forward-declared in main.cpp before this file is included). */
        display_show_status();

        JsonDocument doc;
        doc["ok"] = true;
        doc["mode"] = mode_str;
        doc["freq"] = radio_freq;
        if (radio_is_ssb(mode)) doc["bfo"] = radio_bfo;
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
        doc["mode"] = radio_mode_str(radio_mode);
        doc["freq"] = radio_freq;
        doc["freq_display"] = freq_display;
        if (radio_is_ssb(radio_mode)) doc["bfo"] = radio_bfo;
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
    .version = "0.2.0",
    .describe = radio_describe,
    .endpoints = radio_endpoints,
    .register_routes = radio_register_routes
};

/*
 * Bring the SI4732 up. Blocking, runs once at boot after hw_probe() (Wire is
 * already begun). The amplifier is muted first to avoid the power-on click,
 * then re-enabled once the receiver is tuned. Boot stays FM — the SSB patch is
 * uploaded lazily on the first SSB tune, not here.
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
