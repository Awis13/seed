/*
 * skills/serial.cpp — Serial port skill for ESP32 seed
 *
 * Turns a $3 ESP32 into a WiFi-to-serial bridge. Plug into any device's
 * UART port and it's instantly accessible to AI agents over HTTP.
 *
 * ESP32 has 3 hardware UARTs:
 *   UART0 — used by USB/console (Serial), do NOT use
 *   UART1 — free, remappable to any GPIO pins
 *   UART2 — free, remappable to any GPIO pins
 *
 * Endpoints:
 *   GET  /serial/ports         — list available UARTs and their state
 *   POST /serial/open          — open UART {uart, baud, tx, rx}
 *   POST /serial/write         — send data {uart, data}
 *   GET  /serial/read?uart=N   — read buffered data (&timeout=ms)
 *   POST /serial/close         — close UART {uart}
 */

/* --- UART tracking --- */
#define SERIAL_BUF_SIZE 4096

struct SerialPort {
    HardwareSerial *hw;
    int  uart_num;      /* 1 or 2 */
    int  tx_pin;
    int  rx_pin;
    int  baud;
    bool active;
    char buf[SERIAL_BUF_SIZE];
    int  buf_head;
    int  buf_count;
};

static SerialPort serial_uarts[2] = {
    { &Serial1, 1, -1, -1, 0, false, {}, 0, 0 },
    { &Serial2, 2, -1, -1, 0, false, {}, 0, 0 }
};

static SerialPort *serial_find_uart(int num) {
    if (num == 1) return &serial_uarts[0];
    if (num == 2) return &serial_uarts[1];
    return nullptr;
}

/* Drain available bytes from HardwareSerial into our ring buffer */
static void serial_drain(SerialPort *sp) {
    while (sp->hw->available()) {
        char c = sp->hw->read();
        int pos = sp->buf_head % SERIAL_BUF_SIZE;
        sp->buf[pos] = c;
        sp->buf_head = (sp->buf_head + 1) % SERIAL_BUF_SIZE;
        if (sp->buf_count < SERIAL_BUF_SIZE) sp->buf_count++;
    }
}

/* Read and consume buffer contents */
static int serial_buf_read(SerialPort *sp, char *out, int maxlen) {
    serial_drain(sp);
    if (sp->buf_count == 0) return 0;

    int start = (sp->buf_head - sp->buf_count + SERIAL_BUF_SIZE) % SERIAL_BUF_SIZE;
    int n = sp->buf_count;
    if (n > maxlen - 1) n = maxlen - 1;

    for (int i = 0; i < n; i++) {
        out[i] = sp->buf[(start + i) % SERIAL_BUF_SIZE];
    }
    out[n] = '\0';
    sp->buf_count -= n;
    return n;
}

/* --- Endpoints --- */
static const SkillEndpoint serial_endpoints[] = {
    {"GET",  "/serial/ports", "List available UARTs with state and pin assignments"},
    {"POST", "/serial/open",  "Open UART: {uart:1|2, baud:9600, tx:17, rx:18}"},
    {"POST", "/serial/write", "Send data: {uart:1|2, data:\"Hello\\n\"}"},
    {"GET",  "/serial/read",  "Read buffered data (?uart=1&timeout=1000)"},
    {"POST", "/serial/close", "Close UART: {uart:1|2}"},
    {NULL, NULL, NULL}
};

static const char *serial_describe() {
    return "## Skill: serial\n\n"
           "WiFi-to-serial bridge — talk to UART devices over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /serial/ports | List UARTs: which are open, pins, baud rate |\n"
           "| POST | /serial/open | Open UART: `{\"uart\":1,\"baud\":9600,\"tx\":17,\"rx\":18}` |\n"
           "| POST | /serial/write | Send data: `{\"uart\":1,\"data\":\"Hello\\n\"}` |\n"
           "| GET | /serial/read?uart=1 | Read buffered data (&timeout=1000 for blocking) |\n"
           "| POST | /serial/close | Close UART: `{\"uart\":1}` |\n\n"
           "### UARTs\n\n"
           "- UART0 is reserved (USB console) — only UART1 and UART2 are available\n"
           "- TX/RX pins are freely remappable to any available GPIO\n"
           "- Default baud: 9600. Supported: 300 to 921600\n"
           "- Read buffer: 4KB per UART, circular\n\n"
           "### Typical use\n\n"
           "Plug ESP32 TX→device RX, ESP32 RX→device TX, share GND.\n"
           "Open the UART, then read/write over HTTP. The device sees normal serial.\n";
}

static void serial_register_routes(AsyncWebServer &server) {

    /* GET /serial/ports */
    server.on("/serial/ports", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (int i = 0; i < 2; i++) {
            SerialPort *sp = &serial_uarts[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["uart"] = sp->uart_num;
            obj["active"] = sp->active;

            if (sp->active) {
                obj["baud"] = sp->baud;
                obj["tx_pin"] = sp->tx_pin;
                obj["rx_pin"] = sp->rx_pin;

                /* Drain and report buffer state */
                serial_drain(sp);
                obj["buffered"] = sp->buf_count;
            }
        }

        /* Also note UART0 as reserved */
        JsonObject u0 = arr.add<JsonObject>();
        u0["uart"] = 0;
        u0["active"] = true;
        u0["reserved"] = true;
        u0["note"] = "USB console — do not use";

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* POST /serial/open */
    server.on("/serial/open", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        int uart_num = input["uart"] | 0;
        int baud = input["baud"] | 9600;
        int tx = input["tx"] | -1;
        int rx = input["rx"] | -1;

        free(body); req->_tempObject = nullptr;

        if (uart_num != 1 && uart_num != 2) {
            req->send(400, "application/json", "{\"error\":\"uart must be 1 or 2 (0 is reserved)\"}");
            return;
        }

        if (tx < 0 || rx < 0) {
            req->send(400, "application/json", "{\"error\":\"tx and rx pin numbers required\"}");
            return;
        }

        /* Validate pins exist */
        if (!gpio_pin_exists(tx) || !gpio_pin_exists(rx)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp) {
            req->send(400, "application/json", "{\"error\":\"invalid uart\"}");
            return;
        }

        if (sp->active) {
            req->send(409, "application/json", "{\"error\":\"uart already open\"}");
            return;
        }

        /* Open with pin remap */
        sp->hw->begin(baud, SERIAL_8N1, rx, tx);
        sp->baud = baud;
        sp->tx_pin = tx;
        sp->rx_pin = rx;
        sp->active = true;
        sp->buf_head = 0;
        sp->buf_count = 0;

        event_add("serial: opened UART%d tx=%d rx=%d baud=%d", uart_num, tx, rx, baud);

        JsonDocument doc;
        doc["ok"] = true;
        doc["uart"] = uart_num;
        doc["baud"] = baud;
        doc["tx"] = tx;
        doc["rx"] = rx;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /serial/write */
    server.on("/serial/write", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        int uart_num = input["uart"] | 0;
        const char *data = input["data"] | (const char*)nullptr;

        free(body); req->_tempObject = nullptr;

        if (uart_num != 1 && uart_num != 2) {
            req->send(400, "application/json", "{\"error\":\"uart must be 1 or 2\"}");
            return;
        }

        if (!data) {
            req->send(400, "application/json", "{\"error\":\"data required\"}");
            return;
        }

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp || !sp->active) {
            req->send(404, "application/json", "{\"error\":\"uart not open\"}");
            return;
        }

        size_t written = sp->hw->print(data);
        event_add("serial: wrote %d bytes to UART%d", (int)written, uart_num);

        JsonDocument doc;
        doc["ok"] = true;
        doc["uart"] = uart_num;
        doc["bytes_written"] = (int)written;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* GET /serial/read?uart=N&timeout=ms */
    server.on("/serial/read", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!req->hasParam("uart")) {
            req->send(400, "application/json", "{\"error\":\"uart parameter required\"}");
            return;
        }

        int uart_num = req->getParam("uart")->value().toInt();
        int timeout_ms = 0;
        if (req->hasParam("timeout")) {
            timeout_ms = req->getParam("timeout")->value().toInt();
            if (timeout_ms > 10000) timeout_ms = 10000;
        }

        if (uart_num != 1 && uart_num != 2) {
            req->send(400, "application/json", "{\"error\":\"uart must be 1 or 2\"}");
            return;
        }

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp || !sp->active) {
            req->send(404, "application/json", "{\"error\":\"uart not open\"}");
            return;
        }

        /* If timeout and no data, wait */
        if (timeout_ms > 0) {
            unsigned long start = millis();
            while (sp->buf_count == 0 && !sp->hw->available() &&
                   (millis() - start) < (unsigned long)timeout_ms) {
                delay(10);
                serial_drain(sp);
            }
        }

        char data[SERIAL_BUF_SIZE + 1];
        int n = serial_buf_read(sp, data, sizeof(data));

        JsonDocument doc;
        doc["uart"] = uart_num;
        doc["bytes"] = n;
        doc["data"] = data;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* POST /serial/close */
    server.on("/serial/close", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        char *body = (char*)req->_tempObject;
        if (!body) { req->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

        JsonDocument input;
        if (deserializeJson(input, body) != DeserializationError::Ok) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
            return;
        }

        int uart_num = input["uart"] | 0;
        free(body); req->_tempObject = nullptr;

        if (uart_num != 1 && uart_num != 2) {
            req->send(400, "application/json", "{\"error\":\"uart must be 1 or 2\"}");
            return;
        }

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp || !sp->active) {
            req->send(404, "application/json", "{\"error\":\"uart not open\"}");
            return;
        }

        sp->hw->end();
        sp->active = false;
        sp->buf_count = 0;
        sp->buf_head = 0;

        event_add("serial: closed UART%d", uart_num);

        JsonDocument doc;
        doc["ok"] = true;
        doc["uart"] = uart_num;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);
}

static const Skill serial_skill = {
    .name = "serial",
    .version = "0.1.0",
    .describe = serial_describe,
    .endpoints = serial_endpoints,
    .register_routes = serial_register_routes
};

static void skill_serial_init() {
    skill_register(&serial_skill);
}
