/*
 * skills/gps.cpp — u-blox MIA-M10Q GNSS fix skill (T-Lora Pager)
 *
 * Feed: module UART at 38400 8N1 (factory default). ESP32-S3 UART1:
 *   RX = GPIO4 (module TX), TX = GPIO12 (module RX), PPS = GPIO13.
 * Power/reset ride the XL9555 expander: EXP_GPS_EN=4, EXP_GPS_RST=7.
 *
 * Sentences are parsed by hand — no TinyGPS++/Adafruit dependency:
 *   $GPRMC — time, status A/V, lat ddmm.mm, lon dddmm.mm, date
 *   $GPGGA — fix quality 0..6, satellite count, HDOP
 * The talker prefix is ignored (MIA-M10Q is multi-constellation, so the
 * same sentences arrive as $GNRMC/$GNGGA, $GL*, $GA*, $GB*).
 *
 * The last valid fix lives in RAM and is persisted to /gps.json so GET /gps
 * still answers after a reboot. Saves are debounced like /backlight.json.
 * Endpoints:
 *   GET  /gps     — {ready, lat, lon, fix, fix_quality, sats, hdop, ts, age_s}
 *   POST /gps/fix — {agent?} fresh fix or {fix:false} (+ arm reply-on-fix)
 *
 * Also exposes a couple of public accessors + a weak on-fix hook so agents.cpp
 * can answer a "where are you?" without polluting the agents contract.
 */

#include <math.h>

#define GPS_FILE        "/gps.json"
#define GPS_FILE_TMP    "/gps.tmp"
#define GPS_BAUD        38400
#define GPS_ACQUIRE_INTERVAL_MS 1200000UL  /* periodic fix every 20 minutes */
#define GPS_ACQUIRE_TIMEOUT_MS   120000UL  /* cold TTFF is not a hard bound */
#define GPS_LINE_MAX    96       /* NMEA sentences cap at ~82 chars */
#define GPS_FIELDS_MAX  20
/* Event log flood guard: log a live position heartbeat at most once a minute,
 * and always when the fix moves past ~20 m (~0.0002 deg). */
#define GPS_EVENT_MOVE_DEG   0.0002
#define GPS_EVENT_INTERVAL_S 60
/* A fix is "fresh" for the location door-card / /gps/fix answer. */
#define GPS_FRESH_S          60

/* ---- public accessors (prototypes here; agents.cpp reads them through these) - */
bool gps_get_position(double *lat, double *lon, uint32_t *ts, bool *fix,
                      int *sats);
long gps_fix_age_s(void);
float gps_get_hdop(void);
void gps_set_on_fix(void (*cb)(void));
void gps_request_fix(void);
/* ---------------------------------------------------------------------------- */

/* ---- state ---- */
static bool gps_ready = false;        /* XL9555 rail up + UART configured */
static bool gps_powered = false;
static bool gps_fix = false;          /* last RMC carried status 'A' */
static double gps_lat = 0.0;
static double gps_lon = 0.0;
static unsigned long gps_fix_ts = 0;  /* unix epoch of the last valid RMC */
static int gps_quality = 0;           /* GGA fix quality 0..6 */
static int gps_sats = 0;
static float gps_hdop = 0.0f;
static bool gps_hdop_valid = false;
static uint32_t gps_last_event_s = 0; /* throttle stamp for live/heartbeat logs */

static bool gps_dirty = false;
static bool gps_acquiring = false;
static bool gps_fix_this_acquisition = false;
static bool gps_fix_received_this_boot = false;
static volatile bool gps_wake_requested = false;
static unsigned long gps_acquire_started_ms = 0;
static unsigned long gps_last_cycle_ms = 0;
static unsigned long gps_fix_received_ms = 0;
static char gps_line[GPS_LINE_MAX];
static size_t gps_line_len = 0;

/* Weak hook: fired on every fresh valid RMC 'A'. agents.cpp registers it to
 * answer pending "where are you?" queries the moment a fix lands. */
static void (*gps_on_fix_cb)(void) = nullptr;

/* ---- power (XL9555) ---- */
static void gps_power(bool on) {
    if (!gps_ready) return;
    gps_powered = on;
    if (on) {
        hw_xl_pin(EXP_GPS_EN, true);
        hw_xl_pin(EXP_GPS_RST, true);
    } else {
        hw_xl_pin(EXP_GPS_RST, false);
        hw_xl_pin(EXP_GPS_EN, false);
    }
}

/* ---- persist ---- */
static void gps_mark_dirty() {
    gps_dirty = true;
}

static void gps_save() {
    JsonDocument doc;
    doc["lat"] = gps_lat;
    doc["lon"] = gps_lon;
    doc["fix"] = gps_fix;
    doc["ts"] = gps_fix_ts;
    doc["quality"] = gps_quality;
    doc["sats"] = gps_sats;
    if (gps_hdop_valid) doc["hdop"] = gps_hdop;
    String out;
    serializeJson(doc, out);
    if (!write_spiffs_file_atomic(GPS_FILE, GPS_FILE_TMP, out))
        event_add("gps: save failed");
}

static void gps_start_acquisition() {
    if (!gps_ready || gps_acquiring) return;
    gps_wake_requested = false;
    gps_fix_this_acquisition = false;
    gps_line_len = 0;
    gps_power(true);
    Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    gps_acquire_started_ms = millis();
    gps_acquiring = true;
    event_add("gps: acquiring");
}

static void gps_stop_acquisition(bool fixed) {
    if (!gps_acquiring) return;
    if (fixed && gps_dirty) {
        gps_dirty = false;
        gps_save();             /* exactly one blocking write per successful cycle */
    }
    Serial1.end();
    gps_power(false);
    gps_acquiring = false;
    gps_last_cycle_ms = millis();
    event_add("gps: %s, power off", fixed ? "fixed" : "timeout");
}

static void gps_load() {
    String raw = read_spiffs_file(GPS_FILE);
    if (raw.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
    gps_lat = doc["lat"] | 0.0;
    gps_lon = doc["lon"] | 0.0;
    gps_fix = doc["fix"] | false;
    gps_fix_ts = doc["ts"] | 0UL;
    gps_quality = doc["quality"] | 0;
    gps_sats = doc["sats"] | 0;
    if (doc["hdop"].is<float>() || doc["hdop"].is<int>()) {
        gps_hdop = doc["hdop"].as<float>();
        gps_hdop_valid = true;
    }
}

/* ---- NMEA parsing ---- */
static bool gps_checksum_ok(const char *line) {
    const char *star = strchr(line, '*');
    if (!star) return false;
    uint8_t sum = 0;
    for (const char *p = line + 1; p < star; p++) sum ^= (uint8_t)*p;
    int hi = 0, lo = 0;
    if (sscanf(star + 1, "%1x%1x", &hi, &lo) != 2) return false;
    return ((uint8_t)((hi << 4) | lo)) == sum;
}

/* Split a comma sentence in place; returns field count (fields not NUL-ended
 * past the stream are left as-is). *CS is already stripped by the caller. */
static int gps_split(char *line, char **f, int n) {
    int cnt = 0;
    char *p = line;
    while (cnt < n) {
        f[cnt++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return cnt;
}

/* "ddmm.mm" (deg_digits=2) / "dddmm.mm" (deg_digits=3) to decimal degrees. */
static double gps_nmea_to_deg(const char *field, int deg_digits) {
    if (!field || !field[0]) return NAN;
    char *end = nullptr;
    double v = strtod(field, &end);
    if (end == field || v < 0) return NAN;
    double scale = (deg_digits == 2) ? 100.0 : 1000.0;
    double deg = floor(v / scale);
    double mins = v - deg * scale;
    return deg + mins / 60.0;
}

static bool gps_parse_rmc(char **f, int n) {
    if (n < 7) return false;
    if (f[2][0] != 'A') {               /* status 'V': keep last-known fix */
        return false;
    }
    double lat = gps_nmea_to_deg(f[3], 2);
    double lon = gps_nmea_to_deg(f[5], 3);
    if (isnan(lat) || isnan(lon)) return false;
    if (lat < 0 || lat > 90 || lon < 0 || lon > 180) return false;
    if (!(f[4][0] == 'N' || f[4][0] == 'S')) return false;
    if (!(f[6][0] == 'E' || f[6][0] == 'W')) return false;
    if (f[4][0] == 'S') lat = -lat;
    if (f[6][0] == 'W') lon = -lon;

    time_t now = time(NULL);
    gps_fix_ts = (now > (time_t)TIME_VALID_EPOCH) ? (unsigned long)now : 0;
    gps_fix_received_ms = millis();
    gps_fix_received_this_boot = true;
    gps_fix_this_acquisition = gps_acquiring;
    bool fresh = !gps_fix;
    bool moved = fabs(lat - gps_lat) > GPS_EVENT_MOVE_DEG ||
                 fabs(lon - gps_lon) > GPS_EVENT_MOVE_DEG;

    gps_lat = lat;
    gps_lon = lon;
    uint32_t now_s = millis() / 1000UL;
    gps_fix = true;

    /* Not every fix hits the event ring (~1/s would drown boot/wifi/notify):
     * log the first fix, a movement past ~20 m, and otherwise a throttled
     * heartbeat no more than once a minute. */
    if (fresh) {
        event_add("gps fix: %.5f, %.5f", lat, lon);
        gps_last_event_s = now_s;
    } else if (moved || now_s >= gps_last_event_s + GPS_EVENT_INTERVAL_S) {
        event_add("gps %s: %.5f, %.5f", moved ? "moved" : "live", lat, lon);
        gps_last_event_s = now_s;
    }
    gps_mark_dirty();
    if (gps_on_fix_cb) gps_on_fix_cb();   /* serve pending "where are you?" */
    return true;
}

static bool gps_parse_gga(char **f, int n) {
    if (n < 9) return false;
    int q = atoi(f[6]);
    if (q >= 0 && q <= 6) gps_quality = q;
    int sv = atoi(f[7]);
    if (sv >= 0) gps_sats = sv;
    double h = atof(f[8]);
    gps_hdop_valid = !isnan(h) && h > 0;
    if (gps_hdop_valid) gps_hdop = (float)h;
    return true;
}

static void gps_parse_sentence(char *line) {
    char *star = strchr(line, '*');
    if (star) *star = '\0';
    char *f[GPS_FIELDS_MAX];
    int n = gps_split(line, f, GPS_FIELDS_MAX);
    if (n < 2) return;
    const char *type = f[0] + 1;        /* skip '$' */
    size_t tlen = strlen(type);
    if (tlen >= 3 && strcmp(type + tlen - 3, "RMC") == 0)
        gps_parse_rmc(f, n);
    else if (tlen >= 3 && strcmp(type + tlen - 3, "GGA") == 0)
        gps_parse_gga(f, n);
}

static void gps_feed_byte(uint8_t b) {
    if (b == '\n') {
        gps_line[gps_line_len] = '\0';
        while (gps_line_len > 0 &&
               (gps_line[gps_line_len - 1] == '\r' ||
                gps_line[gps_line_len - 1] == '\n'))
            gps_line[--gps_line_len] = '\0';
        if (gps_line[0] == '$' && gps_line_len >= 6) {
            /* Lenient: validate when a checksum is present, else trust. */
            if (strchr(gps_line, '*') == NULL || gps_checksum_ok(gps_line))
                gps_parse_sentence(gps_line);
        }
        gps_line_len = 0;
        return;
    }
    if (gps_line_len < GPS_LINE_MAX - 1) gps_line[gps_line_len++] = (char)b;
}

static void gps_drain() {
    if (!gps_ready || !gps_acquiring) return;
    while (Serial1.available() > 0) gps_feed_byte((uint8_t)Serial1.read());
}

/* Compatibility helper for callers that truly need to wait. A restored fix
 * cannot satisfy it: only an RMC received after this acquisition starts can. */
bool gps_take_fix(unsigned long max_wait_ms, double *lat_out, double *lon_out) {
    if (!gps_ready) return false;
    unsigned long start = millis();
    gps_start_acquisition();
    while (millis() - start < max_wait_ms) {
        gps_drain();
        if (gps_fix_this_acquisition) {
            if (lat_out) *lat_out = gps_lat;
            if (lon_out) *lon_out = gps_lon;
            gps_stop_acquisition(true);
            return true;
        }
        delay(20);
    }
    return false;
}

/* ---- public accessors (declared at the top; non-static on purpose) ------- */

/* Copy the last known fix into the out-params (all optional). Returns false
 * only when the whole GPS skill is down (XL9555 rail missing). agents.cpp and
 * the /gps/fix route read position through this — never gps_take_fix(). */
bool gps_get_position(double *lat, double *lon, uint32_t *ts, bool *fix,
                      int *sats) {
    if (lat)  *lat  = gps_lat;
    if (lon)  *lon  = gps_lon;
    if (ts)   *ts   = gps_fix_ts;
    if (fix)  *fix  = gps_fix;
    if (sats) *sats = gps_sats;
    return gps_ready;
}

/* Age of the last fix. Monotonic receive time keeps fresh fixes usable during
 * mesh-only/offline boots where SNTP has not supplied an epoch. */
long gps_fix_age_s(void) {
    if (!gps_fix) return -1;
    if (gps_fix_received_this_boot)
        return (long)((millis() - gps_fix_received_ms) / 1000UL);
    if (gps_fix_ts == 0) return -1;
    time_t now = time(NULL);
    if (now <= (time_t)TIME_VALID_EPOCH) return -1;
    return (long)(now - (time_t)gps_fix_ts);
}

/* Last HDOP, or -1 when not yet measured. */
float gps_get_hdop(void) {
    return gps_hdop_valid ? gps_hdop : -1.0f;
}

/* Install the "a fresh RMC 'A' arrived" callback (agents.cpp registers it). */
void gps_set_on_fix(void (*cb)(void)) {
    gps_on_fix_cb = cb;
}

/* Safe from HTTP callbacks: hardware transitions happen later in loop(). */
void gps_request_fix(void) {
    if (gps_ready && !gps_acquiring) gps_wake_requested = true;
}

static void gps_tick() {
    if (!gps_ready) return;
    if (!gps_acquiring) {
        if (gps_wake_requested ||
            millis() - gps_last_cycle_ms >= GPS_ACQUIRE_INTERVAL_MS)
            gps_start_acquisition();
        return;
    }
    gps_drain();
    if (gps_fix_this_acquisition) gps_stop_acquisition(true);
    else if (millis() - gps_acquire_started_ms >= GPS_ACQUIRE_TIMEOUT_MS)
        gps_stop_acquisition(false);
}

/* ---- HTTP ---- */
static void gps_status_json(JsonDocument &doc) {
    doc["ready"] = gps_ready;
    if (!gps_ready) return;
    doc["powered"] = gps_powered;
    doc["acquiring"] = gps_acquiring;
    doc["fix"] = gps_fix;
    if (gps_fix || gps_fix_ts > 0) {
        doc["lat"] = gps_lat;
        doc["lon"] = gps_lon;
        doc["quality"] = gps_quality;
        doc["sats"] = gps_sats;
        if (gps_hdop_valid) doc["hdop"] = gps_hdop;
        doc["ts"] = gps_fix_ts;
        time_t now = time(NULL);
        long age = -1;
        if ((unsigned long)now >= gps_fix_ts && gps_fix_ts > 0)
            age = (long)(now - (time_t)gps_fix_ts);
        doc["age_s"] = age;
    }
}

static const SkillEndpoint gps_endpoints[] = {
    {"GET",  "/gps",     "Last GNSS fix (lat, lon, sats, hdop)"},
    {"POST", "/gps/fix", "Freshest fix now, else {fix:false} (+ agent reply on first fix)"},
    {NULL, NULL, NULL}
};

static const char *gps_describe() {
    return "## Skill: gps\n\n"
           "u-blox MIA-M10Q GNSS on the pager UART (38400 8N1). Tracks the\n"
           "last valid RMC sentence and reports it via `GET /gps`.\n"
           "`fix` is false before the first sky fix; once any fix has been\n"
           "recorded, `lat`/`lon` keep the last known position and are\n"
           "persisted across reboots in `/gps.json`. Also surfaces GGA\n"
           "`quality`, `sats` and `hdop`. Without the XL9555 rail the skill\n"
           "degrades to `ready:false` — the endpoint stays up.\n\n"
           "| GET | /gps |\n"
           "| POST | /gps/fix |\n";
}

static void gps_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/gps"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        gps_status_json(doc);
        notify_send_json(req, 200, doc);
    });

    /* Freshest fix for an agent over the network. No blocking wait — if the
     * saved fix is fresh (< GPS_FRESH_S) it answers coordinates, otherwise
     * {fix:false} and optionally arms the pending agent (agents.cpp) so the
     * reply lands in the thread the moment the first RMC 'A' arrives. */
    server.on(AsyncURIMatcher::exact("/gps/fix"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        JsonDocument input;
        if (body && deserializeJson(input, body) != DeserializationError::Ok) {
            free(body);
            notify_send_error(req, 400, "invalid JSON"); return;
        }
        free(body);
        const char *agent = input["agent"] | "";
        long age = gps_fix_age_s();
        JsonDocument doc;
        if (gps_fix && age >= 0 && age < GPS_FRESH_S) {
            doc["fix"] = true;
            doc["lat"] = gps_lat;
            doc["lon"] = gps_lon;
            doc["sats"] = gps_sats;
            if (gps_hdop_valid) doc["hdop"] = gps_hdop;
            doc["ts"] = gps_fix_ts;
            doc["age_s"] = age;
        } else {
            doc["fix"] = false;
            if (agent[0]) agents_gps_pending(agent);   /* reply in thread on fix */
            gps_request_fix();
        }
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);
}

static const Skill gps_skill = {
    .name = "gps",
    .version = "0.3.0",
    .describe = gps_describe,
    .endpoints = gps_endpoints,
    .register_routes = gps_register_routes,
    .tick = gps_tick
};

static void skill_gps_init() {
    gps_line_len = 0;
    gps_powered = false;
    gps_acquiring = false;
    gps_fix_this_acquisition = false;
    gps_fix_received_this_boot = false;
    gps_wake_requested = false;
    gps_last_event_s = 0;
    gps_load();                 /* last position across reboots — RAM only */

    if (!hw_ui_expand_ok()) {
        gps_ready = false;
        Serial.println("[gps] XL9555 missing — registered, ready=false");
    } else {
        gps_ready = true;
        gps_start_acquisition(); /* immediate boot fix, then 20-minute cadence */
        Serial.printf("[gps] duty cycle %lus/%lus UART1 %u rx=%d tx=%d\n",
                      GPS_ACQUIRE_TIMEOUT_MS / 1000UL,
                      GPS_ACQUIRE_INTERVAL_MS / 1000UL,
                      GPS_BAUD, PIN_GPS_RX, PIN_GPS_TX);
        if (gps_fix_ts > 0)
            Serial.printf("[gps] restored last fix ts=%lu\n",
                          (unsigned long)gps_fix_ts);
    }
    skill_register(&gps_skill);
    event_add("gps skill ready=%d", (int)gps_ready);
}
