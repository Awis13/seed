/*
 * skills/serial.cpp — Serial port skill for ESP32 seed (Cardputer ADVANCE)
 *
 * Turns the node into a WiFi-to-serial bridge: open a UART on explicit pins,
 * then read and write it over HTTP.
 *
 * Endpoints:
 *   GET  /serial/ports         — list UARTs and their state
 *   POST /serial/open          — open UART {uart, baud, tx, rx}
 *   POST /serial/write         — send data {uart, data}
 *   GET  /serial/read?uart=N   — read buffered data (&timeout=ms)
 *   POST /serial/close         — close UART {uart}
 *
 * This file is #included into main.cpp and sits after skills/gpio.cpp, so it
 * uses gpio_pin_exists() and gpio_refuse_reason() from there.
 *
 * Three things here are deliberately not what the sibling seeds do.
 *
 * 1. Explicit tx and rx are MANDATORY. HardwareSerial::begin() falls back to a
 *    per-SoC default pin pair whenever both pin arguments are negative, and on
 *    an ESP32-S3 those defaults are: UART0 rx=44/tx=43, UART1 rx=15/tx=16,
 *    UART2 rx=19/tx=20. On this board that is the IR transmitter and the codec
 *    word clock, an undocumented pin, and — for UART2 — the native USB data
 *    lines, i.e. the console and the recovery path. A request that omits pins
 *    is rejected rather than quietly given one of those.
 *
 * 2. tx and rx go through gpio_refuse_reason(), the same gate POST /gpio/write
 *    uses. Both reference seeds validate only that the pin exists, which makes
 *    the serial skill a hole straight through the gpio skill: a UART opened on
 *    a refused pin drives it just as hard as a digitalWrite() would, and the
 *    gpio skill's 403 becomes advisory. Here the two agree.
 *
 * 3. There is no ring buffer in this file, and that is a memory decision as
 *    much as a correctness one. Both references keep a static 4096-byte ring
 *    per port — 8 KB of .bss reserved whether or not a port is ever opened, on
 *    a board with no PSRAM and roughly 200 KB of free heap. It also does not
 *    buy what it looks like it buys: those rings are drained only when an HTTP
 *    request arrives, so between polls the bytes are held by the UART driver's
 *    own buffer and are lost there, not in the ring. Buffering belongs in the
 *    one place that is actually draining the hardware FIFO, so this file sizes
 *    the DRIVER's buffer with setRxBufferSize() instead. It is heap, allocated
 *    by begin() and released by end(), so a node with no port open pays zero.
 *
 * And the thing every caller gets wrong: at 115200 baud a talkative peer
 * outruns any buffer between polls. Losing bytes is normal here; hiding it is
 * not. Overflow is reported in the response instead of being papered over with
 * a 200 and a short string.
 */

/* Driver-side receive buffer, per open port. 4 KB is ~35 ms of continuous
 * 115200-baud traffic — enough to hold several complete NMEA sentences between
 * polls, not enough to pretend an HTTP-polled bridge can keep up with a stream.
 * Allocated by begin(), freed by end(). */
#define SERIAL_RX_BUFFER   4096

/* Most bytes returned by one /serial/read. Heap, not stack: handlers run on the
 * AsyncTCP task and this is not the place to put kilobytes of frame. */
#define SERIAL_READ_CHUNK  2048

/* A blocking wait here blocks the AsyncTCP task, and with it every other
 * request the node might be serving — including /health. The references allow
 * 10 s; 2 s is long enough for any peer that talks at all. */
#define SERIAL_MAX_TIMEOUT_MS 2000

#define SERIAL_BAUD_MIN    300
#define SERIAL_BAUD_MAX    921600

struct SerialPort {
    HardwareSerial *hw;
    int  uart_num;
    int  tx_pin;
    int  rx_pin;
    int  baud;
    bool active;
    /* Set from the UART event task by the onReceiveError callback, cleared when
     * reported. Sticky between reads on purpose: a caller that polls slowly
     * must still be told that something was dropped since it last looked. */
    volatile bool overflow;
};

static SerialPort serial_uarts[2] = {
    { &Serial1, 1, -1, -1, 0, false, false },
    { &Serial2, 2, -1, -1, 0, false, false }
};

static SerialPort *serial_find_uart(int num) {
    if (num == 1) return &serial_uarts[0];
    if (num == 2) return &serial_uarts[1];
    return nullptr;
}

/* --- Endpoints --- */

static const SkillEndpoint serial_endpoints[] = {
    {"GET",  "/serial/ports", "List UARTs with state and pin assignments"},
    {"POST", "/serial/open",  "Open UART: {uart:1|2, baud:115200, tx:13, rx:15} — pins are required"},
    {"POST", "/serial/write", "Send data: {uart:1|2, data:\"Hello\\n\"}"},
    {"GET",  "/serial/read",  "Read buffered data (?uart=1&timeout=1000)"},
    {"POST", "/serial/close", "Close UART: {uart:1|2}"},
    {NULL, NULL, NULL}
};

/* The GNSS paragraph is real on a board with the Cap LoRa-1262 fitted and
 * fiction on a bare one, so it is gated on the same boot-time probe
 * /capabilities uses. The cap is detachable; this text must not outlive it.
 *
 * Built once into a function-local static on first call because describe()
 * returns a plain pointer and the content is not known at compile time. The
 * cost is a few hundred bytes of heap on the first GET /skill, against roughly
 * a kilobyte of flash for a second full copy of the text. */
static const char *serial_describe() {
    static String out;
    if (out.length()) return out.c_str();

    // Sized from a measurement, not an estimate: the literal below is 3323
    // bytes and the GNSS block appended at the end is a further 776, so a node
    // with the cap fitted produces 4099. 2600 did not cover even the base text
    // (2720 before the nonprintable paragraph was rewritten), so the one call
    // this reserve exists to prevent — a reallocating grow partway through
    // building the string, on a heap this firmware shares with the OTA
    // partition buffer and two 4 KB UART FIFOs — happened every time. Rounded
    // up to leave room for small edits without silently going back to
    // reallocating. Re-measure if the text changes materially.
    out.reserve(4224);
    out += "## Skill: serial\n\n"
           "WiFi-to-serial bridge — talk to UART devices over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /serial/ports | UART state: open, pins, baud, bytes waiting |\n"
           "| POST | /serial/open | `{\"uart\":1,\"baud\":115200,\"rx\":15,\"tx\":13}` |\n"
           "| POST | /serial/write | `{\"uart\":1,\"data\":\"Hello\\n\"}` |\n"
           "| GET | /serial/read?uart=1 | Read buffered data (&timeout=1000) |\n"
           "| POST | /serial/close | `{\"uart\":1}` |\n\n"
           "### rx and tx are required, always\n\n"
           "There is no default pin pair, on purpose. `HardwareSerial::begin()`\n"
           "substitutes a per-SoC default whenever both pins are negative, and on\n"
           "the ESP32-S3 those are UART1 rx=15/tx=16 and **UART2 rx=19/tx=20** —\n"
           "19 and 20 being the native USB lines, i.e. the console and the way\n"
           "back in. An open call without both pins is refused, not defaulted.\n\n"
           "Pins are also checked against the gpio skill's refusal list, so a UART\n"
           "cannot be routed onto the I2C bus (8,9), the keyboard controller's\n"
           "interrupt (11), the RGB LED (21), the panel bus (33-37), the backlight\n"
           "(38) or USB (19,20). `GET /gpio/list` shows which pins those are.\n\n"
           "### UARTs\n\n"
           "- UART1 and UART2 are available. Both are unclaimed peripherals here:\n"
           "  the console is USB-CDC, not a UART\n"
           "- UART0 is left reserved. It is NOT the console on this board, but its\n"
           "  default pins (rx=44, tx=43) are the IR transmitter and the codec\n"
           "  word clock, and two ports is enough\n"
           "- Baud 300 to 921600\n\n"
           "### Reading, and what gets lost\n\n"
           "This is an HTTP-polled bridge over a continuous medium, so bytes are\n"
           "dropped whenever a peer talks faster than you poll. The receive buffer\n"
           "is 4 KB per open port — about 35 ms of solid 115200-baud traffic.\n\n"
           "When that happens you are told. `GET /serial/read` returns:\n\n"
           "- `overflow: true` — the driver reported a lost-byte condition since\n"
           "  your last read. The data you are holding has a hole in it, and it is\n"
           "  not marked where. Sticky until reported, then cleared\n"
           "- `truncated: true` with `remaining` — more bytes were waiting than one\n"
           "  response carries (2048). Read again immediately; nothing is lost yet\n"
           "- `nonprintable: N` — count of bytes replaced with `.` because they\n"
           "  cannot be carried in a JSON string. `data` is a JSON string and the\n"
           "  JSON writer escapes only `\" \\ \\b \\f \\n \\r \\t`, so anything\n"
           "  else outside printable ASCII would go out raw: a byte below 0x20 is\n"
           "  an unescaped control character (invalid JSON), and a byte at or\n"
           "  above 0x80 is a lone high byte (invalid UTF-8). Either would make\n"
           "  the entire response unparseable, costing you `bytes`, `truncated`,\n"
           "  `remaining` and `overflow` along with `data`. Tab, CR and LF are\n"
           "  kept as themselves; everything else outside 0x20-0x7E is replaced.\n"
           "  A binary protocol is therefore not readable through this endpoint —\n"
           "  the bytes are not encoded elsewhere, they are gone, and `N` is all\n"
           "  you get. Use it as a signal that you are on the wrong baud or the\n"
           "  wrong protocol, not as a decodable count\n\n"
           "A short read is not evidence of a quiet peer, and a 200 is not\n"
           "evidence of a complete one. Check the flags.\n\n"
           "`timeout` waits up to 2000 ms for the first byte. That wait blocks the\n"
           "node's web server task, so every other request queues behind it.\n\n"
           "### Typical use\n\n"
           "Node TX -> device RX, node RX -> device TX, common ground. Open, then\n"
           "read and write over HTTP; the device sees an ordinary serial port.\n";

    if (hw.cap_lora1262) {
        // 1024 and not 512. At 512 this snprintf needed 777 bytes and silently
        // wrote 511, and because this section is the last thing in GET /skill
        // the whole tail of the document went missing on any node with the cap
        // fitted — which is the only kind of node that reaches this branch at
        // all. The live node was serving a file that ended mid-word. The
        // compiler had been reporting it as -Wformat-truncation the whole time.
        char gnss[1024];
        snprintf(gnss, sizeof(gnss),
                 "\n### There is a GNSS on this node right now\n\n"
                 "A Cap LoRa-1262 was detected at boot, and its ATGM336H sits on\n"
                 "the EXT header UART emitting NMEA continuously without being\n"
                 "asked. No protocol work needed:\n\n"
                 "```\n"
                 "POST /serial/open {\"uart\":1,\"baud\":%d,\"rx\":%d,\"tx\":%d}\n"
                 "GET  /serial/read?uart=1&timeout=1000\n"
                 "```\n\n"
                 "rx=%d/tx=%d are named from the mainboard's side and are the\n"
                 "opposite of M5Stack's pinmap labels, which are written from the\n"
                 "accessory's. Swapping them gives a port that stays silent.\n\n"
                 "It talks faster than you can poll — expect `overflow` and torn\n"
                 "sentences, and discard any NMEA line without a complete checksum\n"
                 "rather than parsing a fragment.\n\n"
                 "The cap is detachable. This section is gated on the boot probe,\n"
                 "so on a bare board it is absent and those pins are idle.\n",
                 CAP_GNSS_BAUD, PIN_EXT_UART_RX, PIN_EXT_UART_TX,
                 PIN_EXT_UART_RX, PIN_EXT_UART_TX);
        out += gnss;
    }

    return out.c_str();
}

/* Exact matching, like every route in the seed — see setup_routes() in main.cpp
   for what the library's default does to a path with children. */
static void serial_register_routes(AsyncWebServer &server) {

    /* GET /serial/ports */
    server.on(AsyncURIMatcher::exact("/serial/ports"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        /* UART0 first, so the list reads in hardware order. */
        JsonObject u0 = arr.add<JsonObject>();
        u0["uart"] = 0;
        u0["active"] = false;
        u0["reserved"] = true;
        u0["note"] = "not the console on this board (the console is USB-CDC), but "
                     "left reserved: its default pins rx=44/tx=43 are the IR "
                     "transmitter and the ES8311 word clock";

        for (int i = 0; i < 2; i++) {
            SerialPort *sp = &serial_uarts[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["uart"] = sp->uart_num;
            obj["active"] = sp->active;

            if (sp->active) {
                obj["baud"] = sp->baud;
                obj["tx_pin"] = sp->tx_pin;
                obj["rx_pin"] = sp->rx_pin;
                obj["buffered"] = sp->hw->available();
                obj["buffer_size"] = SERIAL_RX_BUFFER;
                if (sp->overflow) obj["overflow"] = true;
            }
        }

        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    });

    /* POST /serial/open — body: {"uart":N,"baud":B,"tx":T,"rx":R} */
    server.on(AsyncURIMatcher::exact("/serial/open"), HTTP_POST, [](AsyncWebServerRequest *req) {
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
        int baud = input["baud"] | 0;
        /* No default. -1 means "absent", and absent is an error, not a hint to
           use the SoC defaults — see the header of this file. */
        int tx = input["tx"] | -1;
        int rx = input["rx"] | -1;

        free(body); req->_tempObject = nullptr;

        if (uart_num != 1 && uart_num != 2) {
            req->send(400, "application/json",
                "{\"error\":\"uart must be 1 or 2 (0 is reserved)\"}");
            return;
        }

        if (tx < 0 || rx < 0) {
            req->send(400, "application/json",
                "{\"error\":\"tx and rx are required — this endpoint never falls back to "
                "the ESP32-S3 default UART pins, which include the native USB lines\"}");
            return;
        }

        if (baud == 0) baud = 115200;
        if (baud < SERIAL_BAUD_MIN || baud > SERIAL_BAUD_MAX) {
            req->send(400, "application/json",
                "{\"error\":\"baud must be between 300 and 921600\"}");
            return;
        }

        if (!gpio_pin_exists(tx) || !gpio_pin_exists(rx)) {
            req->send(400, "application/json", "{\"error\":\"invalid pin number\"}");
            return;
        }

        if (tx == rx) {
            req->send(400, "application/json", "{\"error\":\"tx and rx must differ\"}");
            return;
        }

        /* The same gate POST /gpio/write applies. A UART drives its tx pin as
           hard as a digitalWrite() does, so anything refused there is refused
           here — otherwise this endpoint is a way around it. */
        if (gpio_reject_if_undrivable(req, tx)) return;
        if (gpio_reject_if_undrivable(req, rx)) return;

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp) {
            req->send(400, "application/json", "{\"error\":\"invalid uart\"}");
            return;
        }

        if (sp->active) {
            req->send(409, "application/json", "{\"error\":\"uart already open\"}");
            return;
        }

        /* Order matters: the driver allocates its receive buffer inside
           begin(), so the size has to be set before it, not after. */
        sp->hw->setRxBufferSize(SERIAL_RX_BUFFER);
        sp->overflow = false;
        sp->hw->onReceiveError([sp](hardwareSerial_error_t err) {
            /* Runs on the UART event task. A dropped byte is either the driver
               ring filling up or the hardware FIFO overrunning; the other error
               kinds are line problems, not loss, and are left alone. */
            if (err == UART_BUFFER_FULL_ERROR || err == UART_FIFO_OVF_ERROR) {
                sp->overflow = true;
            }
        });
        sp->hw->begin(baud, SERIAL_8N1, rx, tx);

        sp->baud = baud;
        sp->tx_pin = tx;
        sp->rx_pin = rx;
        sp->active = true;

        event_add("serial: opened UART%d rx=%d tx=%d baud=%d", uart_num, rx, tx, baud);

        JsonDocument doc;
        doc["ok"] = true;
        doc["uart"] = uart_num;
        doc["baud"] = baud;
        doc["tx"] = tx;
        doc["rx"] = rx;
        doc["buffer_size"] = SERIAL_RX_BUFFER;
        String response;
        serializeJson(doc, response);
        req->send(200, "application/json", response);
    }, NULL, handle_body_collect);

    /* POST /serial/write — body: {"uart":N,"data":"..."} */
    server.on(AsyncURIMatcher::exact("/serial/write"), HTTP_POST, [](AsyncWebServerRequest *req) {
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

        if (uart_num != 1 && uart_num != 2) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"uart must be 1 or 2\"}");
            return;
        }
        if (!data) {
            free(body); req->_tempObject = nullptr;
            req->send(400, "application/json", "{\"error\":\"data required\"}");
            return;
        }

        SerialPort *sp = serial_find_uart(uart_num);
        if (!sp || !sp->active) {
            free(body); req->_tempObject = nullptr;
            req->send(404, "application/json", "{\"error\":\"uart not open\"}");
            return;
        }

        /* Written before the body is released: `data` points into it. */
        size_t written = sp->hw->write((const uint8_t*)data, strlen(data));

        free(body); req->_tempObject = nullptr;

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
    server.on(AsyncURIMatcher::exact("/serial/read"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!req->hasParam("uart")) {
            req->send(400, "application/json", "{\"error\":\"uart parameter required\"}");
            return;
        }

        int uart_num = req->getParam("uart")->value().toInt();
        int timeout_ms = 0;
        if (req->hasParam("timeout")) {
            timeout_ms = req->getParam("timeout")->value().toInt();
            if (timeout_ms < 0) timeout_ms = 0;
            if (timeout_ms > SERIAL_MAX_TIMEOUT_MS) timeout_ms = SERIAL_MAX_TIMEOUT_MS;
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

        if (timeout_ms > 0) {
            unsigned long start = millis();
            while (sp->hw->available() == 0 &&
                   (millis() - start) < (unsigned long)timeout_ms) {
                delay(10);
            }
        }

        char *data = (char*)malloc(SERIAL_READ_CHUNK + 1);
        if (!data) {
            req->send(500, "application/json", "{\"error\":\"OOM\"}");
            return;
        }

        int n = 0;
        int nonprintable = 0;
        while (n < SERIAL_READ_CHUNK && sp->hw->available() > 0) {
            int c = sp->hw->read();
            if (c < 0) break;
            /* `data` leaves here inside a JSON string, so every byte that
               cannot be carried in one is substituted and counted. This used to
               substitute NUL alone, which covered the one byte that truncates a
               C string and none of the bytes that destroy the document.

               ArduinoJson escapes exactly " \ \b \f \n \r \t (see the table in
               EscapeSequence.hpp) and emits everything else verbatim. So a
               control byte below 0x20 goes out unescaped, which RFC 8259 s7
               forbids outright, and any byte >= 0x80 goes out as a lone high
               byte, which is not valid UTF-8. Either one is fatal to the WHOLE
               response, not just this field: a caller that cannot parse the
               document loses `bytes`, `truncated`, `remaining` and `overflow`
               too — precisely the flags that would have told it the read was
               incomplete. One stray byte turns a partial answer into no answer.

               This is reachable without any binary protocol. Open the GNSS at
               9600 while it is talking 115200 and the framing garbage arrives
               with the high bit set on most bytes.

               main.cpp's ui_handle_key() refuses the identical range for the
               identical reason before stamping ui_last_key, which GET /ui puts
               in a JSON string the same way. Tab, CR and LF are kept: those
               three are on the escape table, they come out correct, and
               line-oriented peers are the ordinary case.

               skills/notify.cpp's notify_copy_text() is the third site, and
               the one that does NOT match these two. It replaces the same
               control bytes with a space instead of '.', keeps no count, and
               flattens tab, CR and LF rather than keeping them — but it lets
               every well-formed multi-byte UTF-8 sequence through, where both
               of the sites above throw the whole upper half away. It has to:
               a serial dump and a keypress are ASCII, and a notification body
               is text the API promised to return as it was given. See the
               comment over that function for the rest of the reasoning. */
            if (c == '\t' || c == '\r' || c == '\n') {
                data[n++] = (char)c;
            } else if (c < 0x20 || c > 0x7E) {
                data[n++] = '.';
                nonprintable++;
            } else {
                data[n++] = (char)c;
            }
        }
        data[n] = '\0';

        int remaining = sp->hw->available();
        bool overflowed = sp->overflow;
        sp->overflow = false;

        JsonDocument doc;
        doc["uart"] = uart_num;
        doc["bytes"] = n;
        doc["data"] = data;
        if (remaining > 0) {
            doc["truncated"] = true;
            doc["remaining"] = remaining;
        }
        if (nonprintable > 0) doc["nonprintable"] = nonprintable;
        if (overflowed) {
            doc["overflow"] = true;
            doc["overflow_note"] =
                "the UART driver reported dropped bytes since the last read — "
                "this data has a gap in it and the gap is not marked";
        }

        String response;
        serializeJson(doc, response);
        free(data);
        req->send(200, "application/json", response);
    });

    /* POST /serial/close — body: {"uart":N} */
    server.on(AsyncURIMatcher::exact("/serial/close"), HTTP_POST, [](AsyncWebServerRequest *req) {
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

        /* Releases the driver's receive buffer as well as the peripheral. */
        sp->hw->end();
        sp->active = false;
        sp->overflow = false;
        sp->tx_pin = -1;
        sp->rx_pin = -1;
        sp->baud = 0;

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
