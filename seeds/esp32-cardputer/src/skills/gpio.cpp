/*
 * skills/gpio.cpp — GPIO skill for ESP32 seed (M5Stack Cardputer ADVANCE)
 *
 * Endpoints:
 *   GET  /gpio/list       — list available GPIO pins with modes/values
 *   GET  /gpio/read?pin=N — read digital value
 *   POST /gpio/write      — set digital output {pin, value}
 *   POST /gpio/mode       — set pin mode {pin, mode}
 *   GET  /gpio/adc?pin=N  — read analog value (ADC-capable pins)
 *
 * This file is #included into main.cpp, so everything above it in that file is
 * in scope: the PIN_* map, gpio_is_safe(), require_auth(), event_add() and
 * handle_body_collect().
 *
 * Two properties of this board shape every decision below.
 *
 * The board is dense. Of the 38 pins the ESP32-S3 brings out, three are free —
 * see gpio_safe_pins[] in main.cpp, which is the single source of truth for
 * that and which this skill reports rather than restates. `safe: true` here is
 * membership in that allowlist and nothing else: it is not "no warning", and a
 * pin nobody has classified reads as unsafe rather than as free.
 *
 * Handlers run on the AsyncTCP task. main.cpp already documents this hazard for
 * the display — the panel is painted only from loop(), because a handler that
 * draws will preempt a half-finished paint and corrupt M5GFX's SPI state. The
 * same reasoning is what makes the refusal set below a hard 403 rather than a
 * warning: every pin in it is owned by a peripheral or a driver that is live
 * *right now*, on another task, with no lock this file could take. Losing that
 * race does not produce a wrong pin level, it produces a wedged bus.
 */

/* Pins the PIN_* map in main.cpp has no name for, because nothing in the
 * firmware drives them. They are still board facts the classifier needs. */
#define GPIO_PIN_USB_DM     19
#define GPIO_PIN_USB_DP     20
#define GPIO_PIN_GROVE_SCL   1
#define GPIO_PIN_GROVE_SDA   2
#define GPIO_PIN_INT_FPC     7

/* Tracking of configured pins */
#define GPIO_MAX_TRACKED 49

/* 0=unconfigured, 1=input, 2=output, 3=input_pullup, 4=input_pulldown */
static uint8_t gpio_configured[GPIO_MAX_TRACKED];

static bool gpio_pin_exists(int pin) {
    /* ESP32-S3 exposes GPIO 0-21 and 33-48. 22-25 do not exist on the die at
     * all; 26-32 are the internal SPI flash lines and are never broken out. */
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 32) return false;
    return true;
}

/* --- Pin groups ---
 *
 * Each predicate answers "does this pin belong to that thing", and the three
 * reporting functions below apply them in one fixed precedence. A pin can be in
 * several groups (38 is panel backlight AND the RGB LED's supply gate; 46 is
 * the codec's data line AND an S3 strapping pin) and callers only get one
 * answer, so the order is worst-consequence-first everywhere. */

/* The one live I2C bus: keyboard controller, audio codec, IMU, and whatever a
 * fitted cap adds. M5Unified owns it as In_I2C on I2C_NUM_1 at 400 kHz. */
static bool gpio_is_i2c(int pin) {
    return (pin == PIN_I2C_SDA || pin == PIN_I2C_SCL);
}

/* The TCA8418's interrupt output.
 *
 * This is an OUTPUT of the keyboard controller, not of the ESP32: the driver
 * calls enableInterrupts() on the chip unconditionally, so the TCA8418 asserts
 * this line on every key event whether or not anything is listening. Driving it
 * from the ESP32 side is contention between two chips on one net. */
static bool gpio_is_kbd(int pin) {
    return (pin == PIN_KB_INT);
}

/* ST7789 panel, SPI3 bus + control lines. M5GFX drives these from loop(). */
static bool gpio_is_panel(int pin) {
    return (pin == PIN_TFT_RST || pin == PIN_TFT_DC || pin == PIN_TFT_MOSI ||
            pin == PIN_TFT_SCLK || pin == PIN_TFT_CS);
}

/* Backlight. Not a plain output: M5GFX attaches it to LEDC channel 7 and dims
 * it, so the PWM peripheral drives the pad and digitalWrite() fights it rather
 * than setting a level. It also gates the RGB LED's supply. */
static bool gpio_is_backlight(int pin) {
    return (pin == PIN_TFT_BL);
}

/* Native USB D-/D+. The console, and the recovery path if WiFi goes. */
static bool gpio_is_usb(int pin) {
    return (pin == GPIO_PIN_USB_DM || pin == GPIO_PIN_USB_DP);
}

/* The EXT 2.54-14P header: SPI fan-out, UART, and the microSD bus doing double
 * duty. These belong to the header, not to any one accessory — a Cap LoRa-1262
 * drives all nine and a bare board none. Not refused: nothing on the mainboard
 * is behind them, and the serial skill has to be able to open 13/15. */
static bool gpio_is_ext(int pin) {
    return (pin == CAP_LORA_RST || pin == CAP_LORA_DIO1 || pin == CAP_LORA_NSS ||
            pin == CAP_LORA_BUSY || pin == PIN_SD_MOSI || pin == PIN_SD_MISO ||
            pin == PIN_SD_SCLK ||
            pin == PIN_EXT_UART_RX || pin == PIN_EXT_UART_TX);
}

static bool gpio_is_sd(int pin) {
    return (pin == PIN_SD_CS);
}

/* ES8311 codec, I2S side. Configured by M5Unified, started by nothing here. */
static bool gpio_is_audio(int pin) {
    return (pin == PIN_I2S_SCLK || pin == PIN_I2S_DSDIN ||
            pin == PIN_I2S_LRCK || pin == PIN_I2S_ASDOUT);
}

static bool gpio_is_ir(int pin) {
    return (pin == PIN_IR_TX);
}

/* RGB LED data line.
 *
 * A known assignment, not an unknown one: M5Unified's board table maps
 * board_M5CardputerADV to GPIO21 and _setup_led() runs on every begin(). It
 * only *configures* the LED — it builds an RMT bus object and stores the pin —
 * and the RMT peripheral is not started until M5.Led.begin(), which this
 * firmware never calls, with cfg.led_brightness defaulting to 0. So the pin is
 * idle at runtime. Idle is not free, though: anything that later lights the LED
 * takes it back, so it stays off the safe list and warns. */
static bool gpio_is_led(int pin) {
    return (pin == PIN_RGB_LED);
}

static bool gpio_is_vbat(int pin) {
    return (pin == PIN_VBAT_ADC);
}

/* The HY2.0-4P Grove port and the internal FPC pad — the safe list, named.
 * These get a class so /gpio/list says what they are, and no warning. */
static bool gpio_is_grove(int pin) {
    return (pin == GPIO_PIN_GROVE_SCL || pin == GPIO_PIN_GROVE_SDA);
}

static bool gpio_is_fpc(int pin) {
    return (pin == GPIO_PIN_INT_FPC);
}

/* The ESP32-S3's strapping pins, per datasheet section 2.4: GPIO0 (boot mode),
 * GPIO3 (JTAG source select), GPIO45 (VDD_SPI voltage) and GPIO46 (boot-time
 * ROM messages). 47 and 48 are NOT strapping pins — they were listed here by
 * mistake — and M5Stack's Cardputer-Adv pinmap gives them no assignment at all,
 * so they move to gpio_is_unclassified() on the same "excluded because we do
 * not know" grounds the rest of that group uses. GPIO3 is also on the EXT
 * header, and gpio_is_ext() is checked first so its more actionable class wins;
 * the strapping note is appended to that pin's warning instead. */
static bool gpio_is_strapping(int pin) {
    return (pin == 0 || pin == 3 || pin == 45 || pin == 46);
}

/* Pins with no published assignment on M5Stack's Cardputer-Adv pinmap. 16, 17
 * and 18 sit near a microphone and an NS4150B amplifier that appear nowhere in
 * that document; 47 and 48 appear nowhere at all and are not strapping pins
 * either. Excluded because we do not know, not because we know — see the note
 * on gpio_safe_pins[] in main.cpp. */
static bool gpio_is_unclassified(int pin) {
    return (pin == 16 || pin == 17 || pin == 18 || pin == 47 || pin == 48);
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

/* Every pin the ESP32-S3 brings out: 0-21 and 33-48. */
static const int gpio_all_pins[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int gpio_all_pins_count = sizeof(gpio_all_pins) / sizeof(gpio_all_pins[0]);

/* What owns this pin, in words. NULL if nothing does. */
static const char *gpio_warning(int pin) {
    if (gpio_is_usb(pin))       return "native USB D-/D+ — kills the console and the recovery path";
    if (gpio_is_i2c(pin))       return "live I2C bus (keyboard controller, audio codec, IMU)";
    if (gpio_is_kbd(pin))       return "TCA8418 interrupt output — the controller drives this line";
    if (gpio_is_panel(pin))     return "ST7789 panel SPI bus, driven from loop()";
    if (gpio_is_backlight(pin)) return "backlight on LEDC channel 7, and the RGB LED supply gate";
    if (gpio_is_ext(pin))       return (pin == 3)
        ? "EXT 2.54-14P header (SPI/UART fan-out, shared with microSD); also an S3 strapping pin (JTAG source select) — may affect boot"
        : "EXT 2.54-14P header (SPI/UART fan-out, shared with microSD)";
    if (gpio_is_sd(pin))        return "microSD chip select";
    if (gpio_is_audio(pin))     return "ES8311 audio codec (I2S) — configured, not started";
    if (gpio_is_ir(pin))        return "IR transmitter";
    if (gpio_is_led(pin))       return "RGB LED data line — configured by M5Unified, RMT not started";
    if (gpio_is_vbat(pin))      return "battery sense divider";
    if (gpio_is_strapping(pin)) return "strapping pin or module-internal line — may affect boot";
    if (gpio_is_unclassified(pin)) return "unclassified — no published assignment, treat as taken";
    return NULL;
}

/* Machine-readable counterpart of gpio_warning(), same precedence. */
static const char *gpio_class(int pin) {
    if (gpio_is_usb(pin))       return "usb";
    if (gpio_is_i2c(pin))       return "i2c";
    if (gpio_is_kbd(pin))       return "keyboard";
    if (gpio_is_panel(pin))     return "panel";
    if (gpio_is_backlight(pin)) return "backlight";
    if (gpio_is_ext(pin))       return "ext_header";
    if (gpio_is_sd(pin))        return "sdcard";
    if (gpio_is_audio(pin))     return "audio";
    if (gpio_is_ir(pin))        return "ir";
    if (gpio_is_led(pin))       return "led";
    if (gpio_is_vbat(pin))      return "vbat";
    if (gpio_is_grove(pin))     return "grove";
    if (gpio_is_fpc(pin))       return "fpc";
    if (gpio_is_strapping(pin)) return "strapping";
    if (gpio_is_unclassified(pin)) return "unclassified";
    return "free";
}

/* Pins the seed refuses to drive over HTTP at all. Returns the refusal message,
 * or NULL if the pin may be driven.
 *
 * The test for membership is not "is this pin important" — most of the board is
 * important — but "does driving it break something that cannot be put back over
 * the same HTTP connection that broke it". Five groups pass that test:
 *
 *   19,20  native USB. Ends the console session, and with it the way back in if
 *          WiFi ever drops. Nothing over HTTP can undo it.
 *   8,9    the I2C bus, live at 400 kHz. Driving either line mid-transaction
 *          wedges the bus for the keyboard controller, the codec, the IMU and
 *          every future probe at once. /capabilities stops being able to see
 *          the board it is describing.
 *   11     the TCA8418's own interrupt output, which the controller asserts on
 *          every key event. Driving it from this side is two chips pushing one
 *          net against each other.
 *   33-37  the ST7789's SPI bus. M5GFX is mid-transaction on the loop() task
 *          whenever the panel repaints, and handlers run on the AsyncTCP task,
 *          so there is no moment at which this is merely rude.
 *   38     the backlight, on LEDC channel 7. digitalWrite() here does not set a
 *          level, it fights the PWM peripheral for the pad — and the same pin
 *          gates the RGB LED supply, so the failure is a dark screen.
 *
 * Everything else warns and proceeds. In particular the EXT header (3,4,5,6,13,
 * 14,15,39,40) is NOT refused: the mainboard has nothing behind those pins, and
 * refusing them would also refuse the serial skill the GNSS UART on 13/15.
 *
 * This applies to /gpio/adc as well as write/mode, which is the one place this
 * skill departs from both reference seeds. analogRead() is not a passive read:
 * it switches the pad to the ADC function, detaching it from whatever
 * peripheral had it. Five of the refused pins (8, 9, 11, 19, 20) fall inside
 * the ADC2 range, so an adc call is a complete bypass of the gate above unless
 * it is checked too. */
static const char *gpio_refuse_reason(int pin) {
    if (gpio_is_usb(pin))
        return "GPIO19/20 are the native USB data lines — driving them ends the console and the recovery path";
    if (gpio_is_i2c(pin))
        return "GPIO8/9 are the live I2C bus (keyboard, codec, IMU) — driving them wedges it for every device";
    if (gpio_is_kbd(pin))
        return "GPIO11 is the TCA8418's interrupt output — the keyboard controller drives this line, not the ESP32";
    if (gpio_is_panel(pin))
        return "GPIO33-37 are the ST7789 SPI bus — the panel driver is mid-transaction on another task";
    if (gpio_is_backlight(pin))
        return "GPIO38 is the backlight on LEDC channel 7 and the RGB LED supply gate — a pin level cannot win against the PWM peripheral";
    return NULL;
}

/* Sends the 403 for a refused pin. Returns true if the request was answered. */
static bool gpio_reject_if_refused(AsyncWebServerRequest *req, int pin) {
    const char *refused = gpio_refuse_reason(pin);
    if (!refused) return false;
    JsonDocument err;
    err["error"] = refused;
    err["pin"] = pin;
    err["class"] = gpio_class(pin);
    String out;
    serializeJson(err, out);
    req->send(403, "application/json", out);
    return true;
}

/* --- Endpoints --- */

static const SkillEndpoint gpio_endpoints[] = {
    {"GET",  "/gpio/list",  "List GPIO pins with class, mode and value"},
    {"GET",  "/gpio/read",  "Read digital pin value (?pin=N)"},
    {"POST", "/gpio/write", "Set digital output {pin, value}"},
    {"POST", "/gpio/mode",  "Set pin mode {pin, mode}"},
    {"GET",  "/gpio/adc",   "Read analog value (?pin=N)"},
    {NULL, NULL, NULL}
};

static const char *gpio_describe() {
    return "## Skill: gpio\n\n"
           "Direct GPIO control for the M5Stack Cardputer ADVANCE (ESP32-S3).\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /gpio/list | Every pin with `class`, `safe`, `refused`, mode and value |\n"
           "| GET | /gpio/read?pin=N | Read digital value of pin N |\n"
           "| POST | /gpio/write | Set pin output: `{\"pin\":N,\"value\":0\\|1}` |\n"
           "| POST | /gpio/mode | Set mode: `{\"pin\":N,\"mode\":\"input\"\\|\"output\"\\|\"input_pullup\"\\|\"input_pulldown\"}` |\n"
           "| GET | /gpio/adc?pin=N | Read ADC value (pins 1-20) |\n\n"
           "### Which pins you may actually use\n\n"
           "Three, and this is the whole list: **1, 2** (the HY2.0-4P Grove port)\n"
           "and **7** (an unpopulated internal FPC pad). That is what `safe: true`\n"
           "means — membership in an allowlist, not the absence of a warning. The\n"
           "board is dense and almost every other pin is already spoken for.\n\n"
           "GPIO1/2 are idle only because M5Unified registers them as `Ex_I2C` and\n"
           "never starts that bus. They stop being free the moment anything calls\n"
           "`Ex_I2C.begin()`. GPIO7 is wired to an internal FPC connector that is\n"
           "unpopulated on a stock unit; it is free while nothing is plugged into\n"
           "it, and not free afterwards.\n\n"
           "### Refused with 403, on write, mode AND adc\n\n"
           "- `usb` (19,20): native USB — ends the console and the recovery path\n"
           "- `i2c` (8,9): the live 400 kHz bus — keyboard, codec, IMU\n"
           "- `keyboard` (11): the TCA8418's interrupt output, driven by the controller\n"
           "- `panel` (33-37): ST7789 SPI, in use from loop() while you call this\n"
           "- `backlight` (38): LEDC channel 7, and the RGB LED supply gate\n\n"
           "The adc half of that is deliberate: `analogRead()` switches the pad to\n"
           "the ADC function and detaches whatever peripheral held it, so leaving\n"
           "it ungated would be a way straight through the gate.\n\n"
           "### Allowed, but warned — `class` in /gpio/list says which\n\n"
           "- `ext_header` (3,4,5,6,13,14,15,39,40): the EXT 2.54-14P header's SPI\n"
           "  and UART fan-out, shared with the microSD bus. Nothing of the\n"
           "  mainboard's own is behind these; what answers is whatever cap is\n"
           "  fitted, and on a bare board nothing does\n"
           "- `sdcard` (12): microSD chip select\n"
           "- `audio` (41,42,43,46): ES8311 codec, configured but never started\n"
           "- `ir` (44): IR transmitter\n"
           "- `led` (21): RGB LED data. M5Unified configures it on every begin();\n"
           "  the RMT peripheral is only started by `M5.Led.begin()`, which this\n"
           "  firmware does not call\n"
           "- `vbat` (10): battery sense divider\n"
           "- `strapping` (0,45): may affect boot. GPIO3 and GPIO46 are strapping\n"
           "  pins too, but they carry a more actionable class here and report as\n"
           "  `ext_header` and `audio`; GPIO3's warning carries the strapping note\n"
           "- `unclassified` (16,17,18,47,48): no published assignment. The board\n"
           "  has a microphone and an amplifier whose pins appear in no M5Stack\n"
           "  document; guessing is worse than excluding\n\n"
           "Note that 43 and 44 are NOT the free UART0 pins they are on the other\n"
           "two ESP32 seeds in this tree — here they are the codec's word clock and\n"
           "the IR transmitter. The console is USB-CDC.\n\n"
           "### Other notes\n\n"
           "- Pins 22-32 do not exist as usable GPIO: 22-25 are absent from the\n"
           "  ESP32-S3 entirely, 26-32 are the internal SPI flash lines\n"
           "- ADC1 (pins 1-10): always available\n"
           "- ADC2 (pins 11-20): refused whenever the WiFi peripheral is\n"
           "  initialised at all — STA, AP or both — because the radio owns the\n"
           "  block. That includes a first-boot node serving only the\n"
           "  provisioning AP, which has no STA association. In practice the\n"
           "  radio is always up here, so treat ADC2 as unavailable\n";
}

/* Exact matching, like every route in the seed — see setup_routes() in main.cpp
   for what the library's default does to a path with children. */
static void gpio_register_routes(AsyncWebServer &server) {

    /* GET /gpio/list */
    server.on(AsyncURIMatcher::exact("/gpio/list"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (int i = 0; i < gpio_all_pins_count; i++) {
            int pin = gpio_all_pins[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["pin"] = pin;
            obj["mode"] = gpio_mode_str(pin);
            /* digitalRead() reads the input register and reconfigures nothing,
               so it is safe even on a refused pin. */
            obj["value"] = digitalRead(pin);
            obj["class"] = gpio_class(pin);

            const char *warn = gpio_warning(pin);
            if (warn) obj["warning"] = warn;

            if (gpio_is_adc1(pin)) obj["adc"] = "ADC1";
            else if (gpio_is_adc2(pin)) obj["adc"] = "ADC2";

            /* safe = on the allowlist in main.cpp, nothing else */
            obj["safe"] = gpio_is_safe(pin);
            if (gpio_refuse_reason(pin)) obj["refused"] = true;
        }

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* GET /gpio/read?pin=N */
    server.on(AsyncURIMatcher::exact("/gpio/read"), HTTP_GET, [](AsyncWebServerRequest *req) {
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

        /* Not gated: a digital read changes no pin configuration. */
        JsonDocument doc;
        doc["pin"] = pin;
        doc["value"] = digitalRead(pin);
        doc["class"] = gpio_class(pin);
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* POST /gpio/write — body: {"pin":N,"value":0|1} */
    server.on(AsyncURIMatcher::exact("/gpio/write"), HTTP_POST, [](AsyncWebServerRequest *req) {
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
        if (gpio_reject_if_refused(req, pin)) return;

        if (value != 0 && value != 1) {
            req->send(400, "application/json", "{\"error\":\"value must be 0 or 1\"}");
            return;
        }

        /* Auto-configure as OUTPUT if not configured yet */
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
        doc["safe"] = gpio_is_safe(pin);

        const char *warn = gpio_warning(pin);
        if (warn) doc["warning"] = warn;

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /gpio/mode — body: {"pin":N,"mode":"input"|"output"|...} */
    server.on(AsyncURIMatcher::exact("/gpio/mode"), HTTP_POST, [](AsyncWebServerRequest *req) {
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
        char mode_copy[24];
        snprintf(mode_copy, sizeof(mode_copy), "%s", mode ? mode : "");

        free(body);
        req->_tempObject = nullptr;

        if (!gpio_pin_exists(pin)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }
        if (gpio_reject_if_refused(req, pin)) return;

        int arduino_mode;
        uint8_t track_mode;
        if (strcmp(mode_copy, "input") == 0) {
            arduino_mode = INPUT;
            track_mode = 1;
        } else if (strcmp(mode_copy, "output") == 0) {
            arduino_mode = OUTPUT;
            track_mode = 2;
        } else if (strcmp(mode_copy, "input_pullup") == 0) {
            arduino_mode = INPUT_PULLUP;
            track_mode = 3;
        } else if (strcmp(mode_copy, "input_pulldown") == 0) {
            arduino_mode = INPUT_PULLDOWN;
            track_mode = 4;
        } else {
            req->send(400, "application/json",
                "{\"error\":\"mode must be: input, output, input_pullup, input_pulldown\"}");
            return;
        }

        pinMode(pin, arduino_mode);
        gpio_configured[pin] = track_mode;
        event_add("gpio: pin %d mode %s", pin, mode_copy);

        JsonDocument doc;
        doc["ok"] = true;
        doc["pin"] = pin;
        doc["mode"] = mode_copy;
        doc["safe"] = gpio_is_safe(pin);

        const char *warn = gpio_warning(pin);
        if (warn) doc["warning"] = warn;

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* GET /gpio/adc?pin=N */
    server.on(AsyncURIMatcher::exact("/gpio/adc"), HTTP_GET, [](AsyncWebServerRequest *req) {
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

        /* analogRead() re-muxes the pad to the ADC, so the refusal gate applies
           here exactly as it does to write and mode. */
        if (gpio_reject_if_refused(req, pin)) return;

        if (!gpio_is_adc1(pin) && !gpio_is_adc2(pin)) {
            req->send(400, "application/json",
                "{\"error\":\"pin is not ADC-capable (use pins 1-20)\"}");
            return;
        }

        /* ADC2 is unavailable whenever the radio owns it, which is whenever the
           radio is up at all — not merely when it has an association.
           WL_CONNECTED was the wrong test: it describes the STA link's state,
           while what contends for ADC2 is the WiFi peripheral being
           initialised. On a node running the provisioning AP (WIFI_AP, and
           WIFI_AP_STA while the STA half is still searching) the radio has the
           block and the status is not WL_CONNECTED, so the gate opened and the
           endpoint returned numbers off a contended peripheral as though they
           were measurements. A first-boot node with no credentials is exactly
           that node, and gpio_describe() has always stated ADC2 flatly as
           unavailable — so the description was right and the code was wrong.

           WIFI_MODE_NULL is the only mode in which the peripheral is down; the
           test covers STA, AP and AP_STA in one. */
        if (gpio_is_adc2(pin) && WiFi.getMode() != WIFI_MODE_NULL) {
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
        doc["class"] = gpio_class(pin);
        /* Reaching here on an ADC2 pin means the gate above found the radio in
           WIFI_MODE_NULL, so nothing is contending for the block and the
           reading is good. Said plainly, because the old text ("may be
           inaccurate with WiFi") was attached to the one case in which WiFi is
           provably not running. */
        if (gpio_is_adc2(pin))
            doc["note"] = "ADC2 — readable because the radio is currently down; "
                          "this pin is refused whenever WiFi is up";

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
