/*
 * skills/gpio.cpp — GPIO skill for ESP32 seed (ATS Mini v4)
 *
 * Endpoints:
 *   GET  /gpio/list       — list available GPIO pins with modes/values
 *   GET  /gpio/read?pin=N — read digital value
 *   POST /gpio/write      — set digital output {pin, value}
 *   POST /gpio/mode       — set pin mode {pin, mode}
 *   GET  /gpio/adc?pin=N  — read analog value (ADC-capable pins)
 *
 * ATS Mini pin notes:
 *   GPIO15 — power hold: REFUSED for write/mode, LOW powers the board off
 *   Strapping: 0, 3, 45, 46 — warn but allow (3 = audio mute, 45/46 = TFT data)
 *   Not available: 22-25 (no such GPIO), 26-32 (internal SPI flash)
 *   Reserved: 19/20 (native USB), 33-37 (OPI PSRAM) — refuse in spirit, never safe
 *   I2C: 18(SDA), 17(SCL) — SI4732 + peripherals, warn
 *   Display (8-bit parallel): 5,6,7,8,9,39,40,41,42,45,46,47,48 + BL 38 — warn
 *   Onboard: encoder 1/2/21, amp_en 10, audio_mute 3, si4732_reset 16, vbat 4 — warn
 *   Free: 11, 12, 13, 14, 43, 44 (43/44 = UART0, console runs over USB-CDC)
 *   ADC1: 1-10; ADC2: 11-20 (ADC2 unavailable with WiFi)
 */

// Tracking of configured pins
#define GPIO_MAX_TRACKED 49

static uint8_t gpio_configured[GPIO_MAX_TRACKED]; // 0=unconfigured, 1=input, 2=output, 3=input_pullup, 4=input_pulldown

static bool gpio_pin_exists(int pin) {
    // ESP32-S3 exposes GPIO 0-21 and 33-48. 22-25 do not exist on the chip;
    // 26-32 are the internal SPI flash lines and are never broken out.
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 32) return false;
    return true;
}

// Power hold — writing LOW here powers the board off; never touch it over HTTP
static bool gpio_is_power(int pin) {
    return pin == PIN_PWR_EN;
}

static bool gpio_is_strapping(int pin) {
    return (pin == 0 || pin == 3 || pin == 45 || pin == 46);
}

static bool gpio_is_i2c(int pin) {
    return (pin == PIN_I2C_SDA || pin == PIN_I2C_SCL);
}

// ST7789 on an 8-bit parallel (i8080) bus: control lines + D0..D7 + backlight
static bool gpio_is_display(int pin) {
    return (pin == PIN_TFT_CS || pin == PIN_TFT_DC || pin == PIN_TFT_RST ||
            pin == PIN_TFT_WR || pin == PIN_TFT_RD || pin == PIN_LCD_BL ||
            pin == PIN_TFT_D0 || pin == PIN_TFT_D1 || pin == PIN_TFT_D2 ||
            pin == PIN_TFT_D3 || pin == PIN_TFT_D4 || pin == PIN_TFT_D5 ||
            pin == PIN_TFT_D6 || pin == PIN_TFT_D7);
}

// Other onboard peripherals: encoder, audio amp/mute, SI4732 reset, battery ADC
static bool gpio_is_onboard(int pin) {
    return (pin == PIN_ENC_A || pin == PIN_ENC_B || pin == PIN_ENC_KEY ||
            pin == PIN_AMP_EN || pin == PIN_AUDIO_MUTE ||
            pin == PIN_SI4732_RST || pin == PIN_VBAT_ADC);
}

// Pins that look free but are physically claimed by the SoC itself:
//   19/20 — native USB D-/D+ (USB-Serial/JTAG). The board is flashed and keeps
//           its console over these (USB_CDC_ON_BOOT=1); driving them kills USB.
//   33-37 — octal (OPI) PSRAM data/clock lines; touching them corrupts PSRAM
//           and crashes the chip.
static bool gpio_is_reserved(int pin) {
    return (pin == 19 || pin == 20 || (pin >= 33 && pin <= 37));
}

// Free for user I/O — membership in the single source of truth shared with
// /capabilities (gpio_safe_pins in main.cpp), so the two can never disagree.
static bool gpio_is_safe(int pin) {
    for (int i = 0; i < gpio_safe_pins_count; i++) {
        if (gpio_safe_pins[i] == pin) return true;
    }
    return false;
}

static bool gpio_is_adc1(int pin) {
    return (pin >= 1 && pin <= 10);
}

static bool gpio_is_adc2(int pin) {
    return (pin >= 11 && pin <= 20);
}

static const char *gpio_mode_str(int pin) {
    if (pin < 0 || pin >= GPIO_MAX_TRACKED) return "unknown";
    switch (gpio_configured[pin]) {
        case 1: return "input";
        case 2: return "output";
        case 3: return "input_pullup";
        case 4: return "input_pulldown";
        default: return "unconfigured";
    }
}

// All usable ESP32-S3 pins
static const int gpio_all_pins[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int gpio_all_pins_count = sizeof(gpio_all_pins) / sizeof(gpio_all_pins[0]);

// Warning for a pin (NULL if none)
static const char *gpio_warning(int pin) {
    if (gpio_is_power(pin)) return "power hold pin — LOW powers the board off";
    if (gpio_is_i2c(pin)) return "I2C pin (SI4732 + peripherals)";
    if (gpio_is_display(pin)) return "display pin (ST7789 8-bit parallel bus)";
    if (gpio_is_onboard(pin)) return "onboard peripheral pin (encoder/amp/mute/radio-reset/vbat)";
    if (gpio_is_reserved(pin)) return "reserved — native USB (19/20) or OPI PSRAM (33-37); writing crashes the board";
    if (gpio_is_strapping(pin)) return "strapping pin — may affect boot";
    return NULL;
}

// --- Endpoints ---

static const SkillEndpoint gpio_endpoints[] = {
    {"GET",  "/gpio/list",  "List GPIO pins with modes and values"},
    {"GET",  "/gpio/read",  "Read digital pin value (?pin=N)"},
    {"POST", "/gpio/write", "Set digital output {pin, value}"},
    {"POST", "/gpio/mode",  "Set pin mode {pin, mode}"},
    {"GET",  "/gpio/adc",   "Read analog value (?pin=N)"},
    {NULL, NULL, NULL}
};

static const char *gpio_describe() {
    return "## Skill: gpio\n\n"
           "Direct GPIO control for the ATS Mini v4 (ESP32-S3).\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /gpio/list | List all GPIO pins with current mode/value |\n"
           "| GET | /gpio/read?pin=N | Read digital value of pin N |\n"
           "| POST | /gpio/write | Set pin output: `{\"pin\":N,\"value\":0\\|1}` |\n"
           "| POST | /gpio/mode | Set mode: `{\"pin\":N,\"mode\":\"input\"\\|\"output\"\\|\"input_pullup\"\\|\"input_pulldown\"}` |\n"
           "| GET | /gpio/adc?pin=N | Read ADC value (pins 1-20) |\n\n"
           "### Pin notes\n\n"
           "- GPIO15 (power hold): write/mode REFUSED — LOW powers the board off\n"
           "- Free pins: 11, 12, 13, 14, 43, 44 (43/44 = UART0, console runs over USB-CDC)\n"
           "- Strapping pins (0,3,45,46): allowed but may affect boot\n"
           "- Reserved (19,20 native USB; 33-37 OPI PSRAM): present but never safe\n"
           "- Pins 22-25: do not exist; 26-32: internal SPI flash\n"
           "- I2C pins (18,17): SI4732 + peripherals, warn if reconfiguring\n"
           "- Display (5,6,7,8,9,39,40,41,42,45,46,47,48) + backlight 38: ST7789 parallel bus\n"
           "- Onboard (1,2,21 encoder, 10 amp, 3 mute, 16 radio-reset, 4 vbat)\n"
           "- ADC1 (pins 1-10): always available\n"
           "- ADC2 (pins 11-20): unavailable when WiFi is active\n";
}

static void gpio_register_routes(AsyncWebServer &server) {

    // GET /gpio/list
    server.on("/gpio/list", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (int i = 0; i < gpio_all_pins_count; i++) {
            int pin = gpio_all_pins[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["pin"] = pin;
            obj["mode"] = gpio_mode_str(pin);
            obj["value"] = digitalRead(pin);

            const char *warn = gpio_warning(pin);
            if (warn) obj["warning"] = warn;

            // Mark ADC capability
            if (gpio_is_adc1(pin)) obj["adc"] = "ADC1";
            else if (gpio_is_adc2(pin)) obj["adc"] = "ADC2";

            // Safe = in the shared free-pin list (single source of truth with
            // /capabilities). Radio, display, encoder, audio, power, native USB
            // and OPI PSRAM pins are all excluded by construction.
            obj["safe"] = gpio_is_safe(pin);
        }

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    // GET /gpio/read?pin=N
    server.on("/gpio/read", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!req->hasParam("pin")) {
            req->send(400, "application/json", "{\"error\":\"pin parameter required\"}");
            return;
        }
        int pin = req->getParam("pin")->value().toInt();

        if (!gpio_pin_exists(pin)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        JsonDocument doc;
        doc["pin"] = pin;
        doc["value"] = digitalRead(pin);
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    // POST /gpio/write — body: {"pin":N,"value":0|1}
    server.on("/gpio/write", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) {
            req->send(400, "application/json", "{\"error\":\"no body\"}");
            return;
        }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        if (!input["pin"].is<int>() || !input["value"].is<int>()) {
            free(body);
            req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"pin and value required\"}");
            return;
        }

        int pin = input["pin"].as<int>();
        int value = input["value"].as<int>();

        free(body);
        req->_tempObject = nullptr;

        if (!gpio_pin_exists(pin)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        if (gpio_is_power(pin)) {
            req->send(403, "application/json",
                "{\"error\":\"GPIO15 is the power hold pin — writing it powers the board off\"}");
            return;
        }

        if (value != 0 && value != 1) {
            req->send(400, "application/json", "{\"error\":\"value must be 0 or 1\"}");
            return;
        }

        // Auto-configure as OUTPUT if not configured yet
        if (gpio_configured[pin] != 2) {
            pinMode(pin, OUTPUT);
            gpio_configured[pin] = 2;
        }

        digitalWrite(pin, value);
        event_add("gpio: pin %d write %d", pin, value);

        JsonDocument doc;
        doc["ok"] = true;
        doc["pin"] = pin;
        doc["value"] = value;

        const char *warn = gpio_warning(pin);
        if (warn) doc["warning"] = warn;

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    // POST /gpio/mode — body: {"pin":N,"mode":"input"|"output"|"input_pullup"|"input_pulldown"}
    server.on("/gpio/mode", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) {
            req->send(400, "application/json", "{\"error\":\"no body\"}");
            return;
        }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        if (!input["pin"].is<int>() || !input["mode"].is<const char*>()) {
            free(body);
            req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"pin and mode required\"}");
            return;
        }

        int pin = input["pin"].as<int>();
        const char *mode = input["mode"].as<const char*>();

        free(body);
        req->_tempObject = nullptr;

        if (!gpio_pin_exists(pin)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        if (gpio_is_power(pin)) {
            req->send(403, "application/json",
                "{\"error\":\"GPIO15 is the power hold pin — reconfiguring it powers the board off\"}");
            return;
        }

        // Map mode string to Arduino mode
        int arduino_mode;
        uint8_t track_mode;
        if (strcmp(mode, "input") == 0) {
            arduino_mode = INPUT;
            track_mode = 1;
        } else if (strcmp(mode, "output") == 0) {
            arduino_mode = OUTPUT;
            track_mode = 2;
        } else if (strcmp(mode, "input_pullup") == 0) {
            arduino_mode = INPUT_PULLUP;
            track_mode = 3;
        } else if (strcmp(mode, "input_pulldown") == 0) {
            arduino_mode = INPUT_PULLDOWN;
            track_mode = 4;
        } else {
            req->send(400, "application/json",
                "{\"error\":\"mode must be: input, output, input_pullup, input_pulldown\"}");
            return;
        }

        pinMode(pin, arduino_mode);
        gpio_configured[pin] = track_mode;
        event_add("gpio: pin %d mode %s", pin, mode);

        JsonDocument doc;
        doc["ok"] = true;
        doc["pin"] = pin;
        doc["mode"] = mode;

        const char *warn = gpio_warning(pin);
        if (warn) doc["warning"] = warn;

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    // GET /gpio/adc?pin=N
    server.on("/gpio/adc", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!req->hasParam("pin")) {
            req->send(400, "application/json", "{\"error\":\"pin parameter required\"}");
            return;
        }
        int pin = req->getParam("pin")->value().toInt();

        if (!gpio_pin_exists(pin)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        if (!gpio_is_adc1(pin) && !gpio_is_adc2(pin)) {
            req->send(400, "application/json", "{\"error\":\"pin is not ADC-capable (use pins 1-20)\"}");
            return;
        }

        // ADC2 is unavailable while WiFi is active
        if (gpio_is_adc2(pin) && WiFi.status() == WL_CONNECTED) {
            req->send(400, "application/json",
                "{\"error\":\"ADC2 pins (11-20) unavailable while WiFi is active\"}");
            return;
        }

        uint32_t raw = analogRead(pin);
        float voltage = (raw / 4095.0f) * 3.3f;

        JsonDocument doc;
        doc["pin"] = pin;
        doc["raw"] = raw;
        doc["voltage"] = serialized(String(voltage, 3));
        if (gpio_is_adc2(pin)) doc["warning"] = "ADC2 — may be inaccurate with WiFi";

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });
}

static const Skill gpio_skill = {
    .name = "gpio",
    .version = "0.1.0",
    .describe = gpio_describe,
    .endpoints = gpio_endpoints,
    .register_routes = gpio_register_routes,
    .tick = nullptr,
};

static void skill_gpio_init() {
    memset(gpio_configured, 0, sizeof(gpio_configured));
    skill_register(&gpio_skill);
}
