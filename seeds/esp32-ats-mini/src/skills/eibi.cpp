/*
 * skills/eibi.cpp — EIBI shortwave broadcast schedule for the ESP32 seed
 *
 * "Who is broadcasting on this shortwave frequency right now?" The node
 * downloads the EIBI schedule (eibispace.de), parses it into a compact binary
 * blob on SPIFFS, and answers offline lookups keyed on the current frequency
 * and UTC time. No server round-trip after the one-time download.
 *
 * FLASH variant: the whole schedule (~8k slots, ~250 KB) lives in flash as a
 * sorted array of fixed-size records, so lookup is a binary search over the
 * file. The download streams the source line-by-line (never 2 MB in RAM),
 * accumulates parsed slots in an external-RAM (PSRAM) buffer, then commits the
 * buffer to flash in a single write and renames it into place atomically.
 *
 * EIBI is strictly UTC — never the local wall clock. gmtime_r() off time(NULL)
 * gives the hour/minute the schedule is indexed by.
 *
 * Only AM/SSB (frequencies in kHz) make sense here: FM is a different band with
 * a different frequency unit (10 kHz steps) and is not in the EIBI data, so it
 * is rejected up front.
 *
 * The record layout and the fixed-width parse are modelled on the EIBI text
 * format; the code here is original.
 *
 * Endpoints:
 *   GET  /radio/eibi          — who is on the current frequency now (UTC)
 *   GET  /radio/eibi?freq=N   — ... or on an explicit frequency in kHz (test)
 *   GET  /radio/eibi/status   — schedule loaded? how many slots, how many bytes
 *   POST /radio/eibi/load     — download + parse + commit the schedule to flash
 */

#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <ctype.h>

/* Where the compact schedule lives, and where a fresh download is staged before
 * the atomic rename into place. */
#define EIBI_BIN   "/eibi.bin"
#define EIBI_TMP   "/eibi.tmp"
#ifndef EIBI_URL
#define EIBI_URL   "http://eibispace.de/dx/eibi.txt"
#endif

/* Upper bound on parsed slots. The A26 schedule is ~8k lines; 30000 leaves
 * generous headroom for future seasons. 30000 * 30 B = ~900 KB out of 8 MB
 * PSRAM. Anything past the cap is dropped with a logged event, never silently. */
#define EIBI_MAX_SLOTS 30000

/* One schedule entry, packed to exactly 30 bytes so the on-flash array is a flat
 * seekable table. Sorted by freq_khz (ascending), mirroring the source file. */
struct __attribute__((packed)) EibiSlot {
    uint16_t freq_khz;   /* frequency in kHz (integer part; source is sorted) */
    int8_t   sh, sm;     /* start hour/min UTC; sh < 0 == "any time" (24h) */
    int8_t   eh, em;     /* end hour/min UTC */
    char     name[24];   /* station name, NUL-terminated */
};

/* Serialises file access so a lookup on the HTTP task cannot race the final
 * remove+rename of a concurrent download. Not held during the long download
 * itself — only around the swap and around each lookup. */
static SemaphoreHandle_t eibi_mtx = nullptr;

/* True only while a download is in flight, so a second POST /radio/eibi/load
 * gets a clean 409 instead of two streams fighting over the temp file. */
static volatile bool eibi_loading = false;

/* Coarse result state so a poller of /radio/eibi/status can tell "still loading"
 * from "finished ok" from "failed" without relying on the /events ring (that ring
 * is wiped on reboot and races the async task). Set to loading before the worker
 * task is created; the worker sets ok/error (and eibi_err) as it finishes. */
static volatile int eibi_state = 0;   /* 0 idle, 1 loading, 2 ok, 3 error */
static char eibi_err[64] = {0};

/* --- Schedule state --- */

static bool eibi_available() {
    return SPIFFS.exists(EIBI_BIN);
}

/* Slot count = file size / record size. 0 if not loaded. */
static int eibi_slot_count() {
    File f = SPIFFS.open(EIBI_BIN, FILE_READ);
    if (!f) return 0;
    int n = (int)(f.size() / sizeof(EibiSlot));
    f.close();
    return n;
}

/* --- Parsing --- */
//
// The EIBI text is fixed-width. Each data line begins at column 0 with the
// frequency; the fields we need sit at known offsets:
//   [0..13]  frequency in kHz, may be fractional ("6010.5")
//   [14..22] time window "HHMM-HHMM" in UTC
//   [23..33] days + ITU country code (ignored here)
//   [34..57] station name (24 chars, space-padded)
// `line` must already be left-trimmed and start with a digit.

static bool eibi_parse_line(const char *line, EibiSlot &slot) {
    size_t len = strlen(line);
    /* Need at least the frequency and the full time window. */
    if (len < 23) return false;

    /* Frequency: leading number, truncated to integer kHz. Must fit uint16. */
    double f = atof(line);
    if (f < 1.0 || f > 65535.0) return false;
    slot.freq_khz = (uint16_t)f;

    /* Time window "HHMM-HHMM" at offset 14. */
    int sh, sm, eh, em;
    if (sscanf(line + 14, "%2d%2d-%2d%2d", &sh, &sm, &eh, &em) != 4) return false;

    /* "0000-2400" means around the clock — collapse to the "any time" sentinel
     * so the time match short-circuits instead of comparing against 24:00. */
    if (sh == 0 && sm == 0 && eh == 24 && em == 0) {
        slot.sh = -1; slot.sm = 0;
        slot.eh = -1; slot.em = 0;
    } else {
        slot.sh = (int8_t)sh; slot.sm = (int8_t)sm;
        slot.eh = (int8_t)eh; slot.em = (int8_t)em;
    }

    /* Station name: up to 24 chars at offset 34, then trim padding. */
    char name[25];
    name[0] = '\0';
    if (len > 34) {
        strncpy(name, line + 34, 24);
        name[24] = '\0';
    }
    for (int i = (int)strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\t'); i--)
        name[i] = '\0';
    char *n = name;
    while (*n == ' ' || *n == '\t') n++;
    if (*n == '\0') return false;             /* no station name */
    if (strstr(n, "Jammer")) return false;    /* skip jammers */

    strncpy(slot.name, n, sizeof(slot.name) - 1);
    slot.name[sizeof(slot.name) - 1] = '\0';
    return true;
}

/* --- Time match --- */
//
// now_min is minutes since UTC midnight. A slot with sh < 0 airs around the
// clock. A normal window [start,end) is inclusive of start, exclusive of end;
// a window whose start > end wraps past midnight.

static bool eibi_slot_now(const EibiSlot *s, int now_min) {
    if (s->sh < 0 || s->eh < 0) return true;
    int start = s->sh * 60 + s->sm;
    int end   = s->eh * 60 + s->em;
    if (start <= end) return now_min >= start && now_min < end;
    return now_min >= start || now_min < end;   /* wraps midnight */
}

/* --- Lookup --- */
//
// Binary search the sorted file for the first slot at `freq`, then scan forward
// over every slot sharing that frequency and return the first one on air at the
// given UTC time. Serialised by eibi_mtx. Returns false if the schedule is
// missing, the frequency is absent, or nothing is on air right now.

static bool eibi_lookup(uint16_t freq, int utc_h, int utc_m, EibiSlot &out) {
    if (eibi_mtx) xSemaphoreTake(eibi_mtx, portMAX_DELAY);

    File f = SPIFFS.open(EIBI_BIN, FILE_READ);
    if (!f) {
        if (eibi_mtx) xSemaphoreGive(eibi_mtx);
        return false;
    }

    long total = (long)(f.size() / sizeof(EibiSlot));
    if (total <= 0) {
        f.close();
        if (eibi_mtx) xSemaphoreGive(eibi_mtx);
        return false;
    }

    /* Find the first (leftmost) record with this frequency. */
    long lo = 0, hi = total - 1, hit = -1;
    EibiSlot s;
    bool io_ok = true;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (!f.seek(mid * sizeof(EibiSlot)) ||
            f.read((uint8_t*)&s, sizeof(s)) != sizeof(s)) { io_ok = false; break; }
        if (s.freq_khz < freq)      lo = mid + 1;
        else if (s.freq_khz > freq) hi = mid - 1;
        else { hit = mid; hi = mid - 1; }
    }

    bool found = false;
    if (io_ok && hit >= 0 && f.seek(hit * sizeof(EibiSlot))) {
        int now = utc_h * 60 + utc_m;
        while (f.read((uint8_t*)&s, sizeof(s)) == sizeof(s)) {
            if (s.freq_khz != freq) break;
            if (eibi_slot_now(&s, now)) { out = s; found = true; break; }
        }
    }

    f.close();
    if (eibi_mtx) xSemaphoreGive(eibi_mtx);
    return found;
}

/* Current UTC hour/minute, or false before the first NTP sync (time() still in
 * the pre-epoch range). EIBI is UTC — deliberately gmtime_r, never localtime. */
static bool eibi_utc_now(int &h, int &m) {
    time_t now = time(NULL);
    if (now <= TIME_VALID_EPOCH) return false;
    struct tm g;
    gmtime_r(&now, &g);
    h = g.tm_hour;
    m = g.tm_min;
    return true;
}

/* Format a slot's end time as "HH:MM" for the "on air until" readout. An
 * around-the-clock slot has no meaningful end, so report "24:00". */
static void eibi_format_until(const EibiSlot *s, char *out, size_t len) {
    if (s->sh < 0 || s->eh < 0) { snprintf(out, len, "24:00"); return; }
    snprintf(out, len, "%02d:%02d", s->eh, s->em);
}

/* --- Download + parse + commit --- */
//
// Runs on a dedicated worker task (eibi_load_task), NOT on the async_tcp handler:
// the download blocks for 15-30s, and the async_tcp task is subscribed to the
// FreeRTOS task watchdog (~5s), so blocking it there trips the WDT and reboots
// the node. A freshly created worker task is not WDT-subscribed, so the blocking
// HTTPClient GET is safe on it. Must still never run in a tick or under radio_mtx.
// Streams the source line by line into a PSRAM buffer, then writes the whole
// buffer to flash in one shot under the encoder-ISR pause and renames it into
// place. On any failure the old schedule (if any) is left untouched.

static bool eibi_download(int &out_slots, size_t &out_bytes, char *err, size_t errlen) {
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    if (!http.begin(EIBI_URL)) {
        snprintf(err, errlen, "http begin failed");
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(err, errlen, "HTTP %d", code);
        http.end();
        return false;
    }

    /* Parse buffer in external RAM (PSRAM). ~900 KB at the cap. */
    EibiSlot *buf = (EibiSlot*)heap_caps_malloc(
        (size_t)EIBI_MAX_SLOTS * sizeof(EibiSlot), MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(err, errlen, "PSRAM alloc failed");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int total_len = http.getSize();
    int n_slots = 0, dropped = 0;
    long byte_cnt = 0;
    char line[256];
    int li = 0;

    while (http.connected() && (total_len < 0 || byte_cnt < total_len)) {
        if (!stream->available()) { delay(1); continue; }
        char c = (char)stream->read();
        byte_cnt++;

        if (c == '\r') continue;
        if (c != '\n' && li < (int)sizeof(line) - 1) { line[li++] = c; continue; }

        /* End of line (or line buffer full) — process what we have. */
        line[li] = '\0';
        li = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (isdigit((unsigned char)*p)) {
            EibiSlot slot;
            if (eibi_parse_line(p, slot)) {
                if (n_slots < EIBI_MAX_SLOTS) buf[n_slots++] = slot;
                else dropped++;
            }
        }
    }
    http.end();

    if (dropped) event_add("eibi: capped at %d slots, dropped %d", EIBI_MAX_SLOTS, dropped);

    if (n_slots == 0) {
        heap_caps_free(buf);
        snprintf(err, errlen, "no slots parsed");
        return false;
    }

    /* Commit the buffer to flash in a single write. SPIFFS detaches the flash
     * cache during a write and the encoder ISR reads flash, so bracket the whole
     * write with the encoder pause (same contract as the OTA/config writers). */
    SPIFFS.remove(EIBI_TMP);
    radio_encoder_pause();
    File f = SPIFFS.open(EIBI_TMP, FILE_WRITE);
    bool wok = false;
    if (f) {
        size_t want = (size_t)n_slots * sizeof(EibiSlot);
        size_t wrote = f.write((const uint8_t*)buf, want);
        f.close();
        wok = (wrote == want);
    }
    radio_encoder_resume();
    heap_caps_free(buf);

    if (!wok) {
        SPIFFS.remove(EIBI_TMP);
        snprintf(err, errlen, "flash write failed");
        return false;
    }

    /* Atomic swap into place, serialised against lookups. */
    if (eibi_mtx) xSemaphoreTake(eibi_mtx, portMAX_DELAY);
    SPIFFS.remove(EIBI_BIN);
    bool rok = SPIFFS.rename(EIBI_TMP, EIBI_BIN);
    if (eibi_mtx) xSemaphoreGive(eibi_mtx);

    if (!rok) {
        SPIFFS.remove(EIBI_TMP);
        snprintf(err, errlen, "rename failed");
        return false;
    }

    out_slots = n_slots;
    out_bytes = (size_t)n_slots * sizeof(EibiSlot);
    return true;
}

/* --- Endpoints --- */

static const SkillEndpoint eibi_endpoints[] = {
    {"GET",  "/radio/eibi",        "Who is broadcasting on the current freq now (UTC); ?freq=<kHz> to test"},
    {"GET",  "/radio/eibi/status", "Schedule loaded? slot count and byte size"},
    {"POST", "/radio/eibi/load",   "Download the EIBI schedule to flash (needs WiFi, ~10-30s)"},
    {NULL, NULL, NULL}
};

static const char *eibi_describe() {
    return "## Skill: eibi\n\n"
           "Offline shortwave broadcast lookup — \"who is on this kHz right now?\"\n"
           "The EIBI schedule is downloaded once to flash, then queried offline\n"
           "against the current frequency and UTC time. Only AM/SSB (kHz) — FM is\n"
           "a different band and is rejected.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /radio/eibi/load | Download + parse the schedule to flash (needs WiFi, ~10-30s) |\n"
           "| GET | /radio/eibi | Match on the current freq + UTC; `?freq=<kHz>` overrides the freq |\n"
           "| GET | /radio/eibi/status | `{loaded, slots, bytes}` |\n\n"
           "### Notes\n\n"
           "- Load first via POST /radio/eibi/load, then GET /radio/eibi.\n"
           "- Time is strictly UTC and needs an NTP sync; before that GET returns `reason:\"no NTP\"`.\n"
           "- FM tuning returns `match:false, reason:\"FM\"` — EIBI is shortwave only.\n"
           "- Match response: `{match:true, station, until:\"HH:MM\", freq_khz}`.\n";
}

/* GET /radio/eibi — current frequency (or ?freq=) vs the loaded schedule. */
static void eibi_handle_get(AsyncWebServerRequest *req) {
    if (!require_auth(req)) return;

    JsonDocument doc;

    if (!eibi_available()) {
        doc["loaded"] = false;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
        return;
    }

    /* Frequency: explicit ?freq= (kHz, for testing) overrides the live tuning
     * and bypasses the FM gate; otherwise read the receiver and reject FM. */
    uint16_t freq = 0;
    if (req->hasParam("freq")) {
        long v = strtol(req->getParam("freq")->value().c_str(), NULL, 10);
        if (v < 1 || v > 65535) {
            req->send(400, "application/json",
                "{\"error\":\"freq must be 1..65535 kHz\"}");
            return;
        }
        freq = (uint16_t)v;
    } else {
        const char *mode = radio_get_mode_str();
        if (strcmp(mode, "FM") == 0) {
            doc["match"] = false;
            doc["reason"] = "FM";
            String out; serializeJson(doc, out);
            req->send(200, "application/json", out);
            return;
        }
        freq = radio_get_freq();
    }

    int uh, um;
    if (!eibi_utc_now(uh, um)) {
        doc["match"] = false;
        doc["reason"] = "no NTP";
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
        return;
    }

    EibiSlot slot;
    if (eibi_lookup(freq, uh, um, slot)) {
        char until[12];
        eibi_format_until(&slot, until, sizeof(until));
        doc["match"] = true;
        doc["station"] = slot.name;
        doc["until"] = until;
        doc["freq_khz"] = freq;
    } else {
        doc["match"] = false;
        doc["freq_khz"] = freq;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

/* GET /radio/eibi/status — is the schedule loaded, and how big. */
static void eibi_handle_status(AsyncWebServerRequest *req) {
    if (!require_auth(req)) return;

    JsonDocument doc;
    bool loaded = eibi_available();
    doc["loaded"] = loaded;
    int slots = loaded ? eibi_slot_count() : 0;
    doc["slots"] = slots;
    doc["bytes"] = (unsigned long)((size_t)slots * sizeof(EibiSlot));

    const char *st = "idle";
    switch (eibi_state) {
        case 1: st = "loading"; break;
        case 2: st = "ok";      break;
        case 3: st = "error";   break;
        default: st = "idle";   break;
    }
    doc["state"] = st;
    if (eibi_state == 3 && eibi_err[0]) doc["error"] = eibi_err;

    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

/* One-shot worker task carrying the blocking download off the async_tcp handler.
 * A freshly created task is not subscribed to the task watchdog, so the 15-30s
 * GET cannot starve it (do NOT esp_task_wdt_add this task). It logs the same
 * events, records the coarse result state, clears eibi_loading, then deletes
 * itself. */
static void eibi_load_task(void *arg) {
    (void)arg;

    int slots = 0;
    size_t bytes = 0;
    char err[64] = {0};
    bool ok = eibi_download(slots, bytes, err, sizeof(err));

    if (ok) {
        event_add("eibi: loaded %d slots, %u bytes", slots, (unsigned)bytes);
        eibi_err[0] = '\0';
        eibi_state = 2;
    } else {
        event_add("eibi: load failed (%s)", err);
        strncpy(eibi_err, err, sizeof(eibi_err) - 1);
        eibi_err[sizeof(eibi_err) - 1] = '\0';
        eibi_state = 3;
    }

    eibi_loading = false;
    vTaskDelete(NULL);
}

/* POST /radio/eibi/load — kick off the download on the worker task and return
 * immediately. The download must NOT run here: this handler executes on the
 * async_tcp task, which is watchdog-subscribed, and a multi-second blocking GET
 * trips the WDT and reboots the node. Poll /radio/eibi/status for the result. */
static void eibi_handle_load(AsyncWebServerRequest *req) {
    if (!require_auth(req)) return;

    if (WiFi.status() != WL_CONNECTED) {
        req->send(503, "application/json", "{\"error\":\"no WiFi\"}");
        return;
    }
    if (eibi_loading) {
        req->send(409, "application/json", "{\"error\":\"already loading\"}");
        return;
    }

    eibi_loading = true;
    eibi_state = 1;
    eibi_err[0] = '\0';
    event_add("eibi: schedule download started");

    /* Generous stack: HTTPClient + TLS-capable buffers need well past the default.
     * Low priority so it never starves the radio/UI tasks. */
    BaseType_t rc = xTaskCreate(eibi_load_task, "eibi_load", 16384, nullptr, 1, nullptr);
    if (rc != pdPASS) {
        eibi_loading = false;
        eibi_state = 3;
        strncpy(eibi_err, "task create failed", sizeof(eibi_err) - 1);
        eibi_err[sizeof(eibi_err) - 1] = '\0';
        req->send(500, "application/json", "{\"error\":\"task create failed\"}");
        return;
    }

    req->send(200, "application/json",
        "{\"ok\":true,\"loading\":true,\"poll\":\"/radio/eibi/status\"}");
}

static void eibi_register_routes(AsyncWebServer &server) {
    /* Order matters: this server's handler matches sub-paths (first-match wins),
     * so the greedy "/radio/eibi" must be registered LAST or it would swallow
     * "/radio/eibi/status" and "/radio/eibi/load". */
    server.on("/radio/eibi/status", HTTP_GET, eibi_handle_status);
    server.on("/radio/eibi/load", HTTP_POST, eibi_handle_load);
    server.on("/radio/eibi", HTTP_GET, eibi_handle_get);
}

static const Skill eibi_skill = {
    .name = "eibi",
    .version = "0.1.0",
    .describe = eibi_describe,
    .endpoints = eibi_endpoints,
    .register_routes = eibi_register_routes,
    .tick = nullptr,
};

static void skill_eibi_init() {
    if (!eibi_mtx) eibi_mtx = xSemaphoreCreateMutex();
    skill_register(&eibi_skill);
}
