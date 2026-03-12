/*
 * skills/gpio.cpp — GPIO skill for ESP32 seed
 *
 * Endpoints:
 *   GET  /gpio/list       — list available GPIO pins with modes/values
 *   GET  /gpio/read?pin=N — read digital value
 *   POST /gpio/write      — set digital output {pin, value}
 *   POST /gpio/mode       — set pin mode {pin, mode}
 *   GET  /gpio/adc?pin=N  — read analog value (ADC-capable pins)
 *
 * ESP32-S3 pin notes:
 *   Strapping: 0, 3, 45, 46 — warn but allow
 *   Not available: 26-32 (no GPIO on ESP32-S3)
 *   I2C bus0: 17(SDA), 18(SCL); bus1: 5(SDA), 6(SCL) — warn
 *   SPI/LoRa: 8,9,10,11,12,13,14 — warn if LoRa active
 *   ADC1: 1-10; ADC2: 11-20 (ADC2 unavailable with WiFi)
 */

// Трекинг сконфигурированных пинов
#define GPIO_MAX_TRACKED 49

static uint8_t gpio_configured[GPIO_MAX_TRACKED]; // 0=unconfigured, 1=input, 2=output, 3=input_pullup, 4=input_pulldown

static bool gpio_pin_exists(int pin) {
    // ESP32-S3: GPIO 0-48, но 26-32 не существуют
    if (pin < 0 || pin > 48) return false;
    if (pin >= 26 && pin <= 32) return false;
    return true;
}

static bool gpio_is_strapping(int pin) {
    return (pin == 0 || pin == 3 || pin == 45 || pin == 46);
}

static bool gpio_is_i2c(int pin) {
    return (pin == 17 || pin == 18 || pin == 5 || pin == 6);
}

static bool gpio_is_lora(int pin) {
    return (pin == 8 || pin == 9 || pin == 10 || pin == 11 ||
            pin == 12 || pin == 13 || pin == 14);
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

// Список всех доступных пинов ESP32-S3
static const int gpio_all_pins[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int gpio_all_pins_count = sizeof(gpio_all_pins) / sizeof(gpio_all_pins[0]);

// Предупреждения для пина (возвращает NULL если нет)
static const char *gpio_warning(int pin) {
    if (gpio_is_strapping(pin)) return "strapping pin — may affect boot";
    if (gpio_is_i2c(pin)) {
        if (pin == 17 || pin == 18) return "I2C bus0 (OLED) pin";
        return "I2C bus1 (external) pin";
    }
    if (gpio_is_lora(pin) && lora_ready) return "SPI/LoRa pin — LoRa is active";
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
           "Direct GPIO control for ESP32-S3.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /gpio/list | List all GPIO pins with current mode/value |\n"
           "| GET | /gpio/read?pin=N | Read digital value of pin N |\n"
           "| POST | /gpio/write | Set pin output: `{\"pin\":N,\"value\":0\\|1}` |\n"
           "| POST | /gpio/mode | Set mode: `{\"pin\":N,\"mode\":\"input\"\\|\"output\"\\|\"input_pullup\"\\|\"input_pulldown\"}` |\n"
           "| GET | /gpio/adc?pin=N | Read ADC value (pins 1-20) |\n\n"
           "### Pin notes\n\n"
           "- Strapping pins (0,3,45,46): allowed but may affect boot\n"
           "- Pins 26-32: not available on ESP32-S3\n"
           "- I2C pins (5,6,17,18): warn if reconfiguring\n"
           "- SPI/LoRa pins (8-14): warn if LoRa is active\n"
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

            // Отмечаем ADC-способность
            if (gpio_is_adc1(pin)) obj["adc"] = "ADC1";
            else if (gpio_is_adc2(pin)) obj["adc"] = "ADC2";

            // Безопасный ли пин (не занят системой)
            bool safe = !gpio_is_strapping(pin) && !gpio_is_i2c(pin) &&
                        !(gpio_is_lora(pin) && lora_ready);
            obj["safe"] = safe;
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

        if (value != 0 && value != 1) {
            req->send(400, "application/json", "{\"error\":\"value must be 0 or 1\"}");
            return;
        }

        // Автоконфигурация как OUTPUT если ещё не сконфигурирован
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

        // Маппинг строки в Arduino mode
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

        // ADC2 недоступен при активном WiFi
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
