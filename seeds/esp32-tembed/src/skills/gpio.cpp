/*
 * skills/gpio.cpp — GPIO skill for ESP32 seed (LilyGO T-Embed CC1101)
 *
 * Endpoints:
 *   GET  /gpio/list       — list available GPIO pins with modes/values
 *   GET  /gpio/read?pin=N — read digital value
 *   POST /gpio/write      — set digital output {pin, value}
 *   POST /gpio/mode       — set pin mode {pin, mode}
 *   GET  /gpio/adc?pin=N  — read analog value (ADC-capable pins)
 *
 * T-Embed CC1101 pin notes:
 *   GPIO15 — power hold: REFUSED for write/mode, LOW powers the board off
 *   GPIO33-37 — octal PSRAM bus: REFUSED, driving one crashes the board
 *   USB: 19(D-), 20(D+) — the USB-CDC console, warn
 *   Strapping: 0, 3, 45, 46 — warn but allow
 *   Not available: 26-32 (no GPIO on ESP32-S3)
 *   I2C: 8(SDA), 18(SCL) — fuel gauge + charger, warn
 *   Shared SPI bus + chip selects (display/CC1101/SD): 9,10,11,12,13,41 — warn
 *   Display control: 16(TFT DC), 21(backlight) — warn
 *   CC1101 GDO lines: 3(GDO0), 38(GDO2) — warn
 *   Onboard: encoder 4/5/0/6, WS2812 14, IR 2/1, mic 42/39 — warn
 *   ADC1: 1-10; ADC2: 11-20 (ADC2 unavailable with WiFi)
 *
 * "safe" is membership in gpio_safe_pins[] from main.cpp — an allowlist, not
 * the complement of the warnings above. A pin nobody remembered to classify
 * must read as unsafe, not as free.
 */

// Tracking of configured pins
#define GPIO_MAX_TRACKED 49

static uint8_t gpio_configured[GPIO_MAX_TRACKED]; // 0=unconfigured, 1=input, 2=output, 3=input_pullup, 4=input_pulldown

static bool gpio_pin_exists(int pin) {
    // ESP32-S3: GPIO 0-48, but 26-32 do not exist
    if (pin < 0 || pin > 48) return false;
    if (pin >= 26 && pin <= 32) return false;
    return true;
}

// Power hold — writing LOW here powers the board off; never touch it over HTTP
static bool gpio_is_power(int pin) {
    return pin == PIN_PWR_EN;
}

// Octal PSRAM bus (board is qio_opi). Driving one of these kills the PSRAM
// under the running app — unrecoverable without a physical power cycle.
static bool gpio_is_psram(int pin) {
    return (pin >= PIN_PSRAM_FIRST && pin <= PIN_PSRAM_LAST);
}

// USB-Serial/JTAG data lines — the console, and the only way back in if WiFi dies
static bool gpio_is_usb(int pin) {
    return (pin == PIN_USB_DM || pin == PIN_USB_DP);
}

static bool gpio_is_strapping(int pin) {
    return (pin == 0 || pin == 3 || pin == 45 || pin == 46);
}

static bool gpio_is_i2c(int pin) {
    return (pin == PIN_I2C_SDA || pin == PIN_I2C_SCL);
}

// Shared SPI bus + chip selects: display, CC1101, microSD
static bool gpio_is_spi(int pin) {
    return (pin == PIN_SPI_MOSI || pin == PIN_SPI_MISO || pin == PIN_SPI_SCLK ||
            pin == PIN_CC1101_CS || pin == PIN_SD_CS || pin == PIN_TFT_CS);
}

// Display control lines — not on the SPI bus itself
static bool gpio_is_display(int pin) {
    return (pin == PIN_TFT_DC || pin == PIN_TFT_BL);
}

// CC1101 interrupt/status lines
static bool gpio_is_radio(int pin) {
    return (pin == PIN_CC1101_GDO0 || pin == PIN_CC1101_GDO2);
}

// Other onboard peripherals: encoder, WS2812 ring, IR, microphone
static bool gpio_is_onboard(int pin) {
    return (pin == PIN_ENC_A || pin == PIN_ENC_B || pin == PIN_ENC_KEY ||
            pin == PIN_USER_KEY || pin == PIN_WS2812 ||
            pin == PIN_IR_TX || pin == PIN_IR_RX ||
            pin == 42 || pin == 39 /* mic */);
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

// What owns this pin (NULL if nothing does). Ordered worst-first: a pin can
// belong to several groups and the caller only shows one.
static const char *gpio_warning(int pin) {
    if (gpio_is_power(pin)) return "power hold pin — LOW powers the board off";
    if (gpio_is_psram(pin)) return "octal PSRAM bus — writing crashes the board";
    if (gpio_is_usb(pin)) return "USB D-/D+ — kills the USB console";
    if (gpio_is_i2c(pin)) return "I2C pin (fuel gauge/charger)";
    if (gpio_is_spi(pin)) return "shared SPI bus/chip select (display/CC1101/SD)";
    if (gpio_is_display(pin)) return "display control pin (ST7789 DC/backlight)";
    if (gpio_is_radio(pin)) return "CC1101 GDO line (radio status/interrupt)";
    if (gpio_is_onboard(pin)) return "onboard peripheral pin (encoder/LED/IR/mic)";
    if (gpio_is_strapping(pin)) return "strapping pin — may affect boot";
    return NULL;
}

// Machine-readable counterpart of gpio_warning, same precedence
static const char *gpio_class(int pin) {
    if (gpio_is_power(pin)) return "power";
    if (gpio_is_psram(pin)) return "psram";
    if (gpio_is_usb(pin)) return "usb";
    if (gpio_is_i2c(pin)) return "i2c";
    if (gpio_is_spi(pin)) return "spi";
    if (gpio_is_display(pin)) return "display";
    if (gpio_is_radio(pin)) return "radio";
    if (gpio_is_onboard(pin)) return "onboard";
    if (gpio_is_strapping(pin)) return "strapping";
    return "free";
}

// Pins the seed refuses to drive over HTTP: both take the node down in a way
// nobody can undo remotely. Returns the refusal message, or NULL if allowed.
static const char *gpio_refuse_reason(int pin) {
    if (gpio_is_power(pin))
        return "GPIO15 is the power hold pin — driving it powers the board off";
    if (gpio_is_psram(pin))
        return "GPIO33-37 carry the octal PSRAM bus — driving them crashes the board";
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
           "Direct GPIO control for the T-Embed CC1101 (ESP32-S3).\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /gpio/list | List all GPIO pins with current mode/value |\n"
           "| GET | /gpio/read?pin=N | Read digital value of pin N |\n"
           "| POST | /gpio/write | Set pin output: `{\"pin\":N,\"value\":0\\|1}` |\n"
           "| POST | /gpio/mode | Set mode: `{\"pin\":N,\"mode\":\"input\"\\|\"output\"\\|\"input_pullup\"\\|\"input_pulldown\"}` |\n"
           "| GET | /gpio/adc?pin=N | Read ADC value (pins 1-20) |\n\n"
           "### Pin notes\n\n"
           "`safe: true` is an allowlist, not \"no warning\" — only 17, 43 and 44\n"
           "have nothing wired to them (43/44 = UART0, console runs over USB-CDC).\n\n"
           "Refused with 403 on write/mode:\n\n"
           "- GPIO15 (power hold): LOW powers the board off\n"
           "- GPIO33-37 (octal PSRAM bus): driving one crashes the running app\n\n"
           "Allowed but warned, with `class` in /gpio/list:\n\n"
           "- `usb` (19,20): USB D-/D+ — the console and the only recovery path\n"
           "- `i2c` (8,18): fuel gauge + charger\n"
           "- `spi` (9,10,11,12,13,41): shared bus + chip selects, display is active\n"
           "- `display` (16,21): ST7789 DC and backlight enable\n"
           "- `radio` (3,38): CC1101 GDO0/GDO2\n"
           "- `onboard` (0,1,2,4,5,6,14,39,42): encoder, WS2812, IR, mic\n"
           "- `strapping` (0,3,45,46): may affect boot\n\n"
           "Other notes:\n\n"
           "- Pins 26-32: not available on ESP32-S3\n"
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

            obj["class"] = gpio_class(pin);

            const char *warn = gpio_warning(pin);
            if (warn) obj["warning"] = warn;

            // Mark ADC capability
            if (gpio_is_adc1(pin)) obj["adc"] = "ADC1";
            else if (gpio_is_adc2(pin)) obj["adc"] = "ADC2";

            // Safe = on the allowlist in main.cpp, nothing else
            obj["safe"] = gpio_is_safe(pin);
            if (gpio_refuse_reason(pin)) obj["refused"] = true;
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

        const char *refused = gpio_refuse_reason(pin);
        if (refused) {
            JsonDocument err;
            err["error"] = refused;
            String body_out;
            serializeJson(err, body_out);
            req->send(403, "application/json", body_out);
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

        const char *refused = gpio_refuse_reason(pin);
        if (refused) {
            JsonDocument err;
            err["error"] = refused;
            String body_out;
            serializeJson(err, body_out);
            req->send(403, "application/json", body_out);
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
    .register_routes = gpio_register_routes
};

static void skill_gpio_init() {
    memset(gpio_configured, 0, sizeof(gpio_configured));
    skill_register(&gpio_skill);
}
