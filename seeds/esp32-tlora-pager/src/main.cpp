// ESP32 Seed - LILYGO T-Lora Pager 868 (ESP32-S3)
//
// The T-Lora Pager port of the ESP32 seed: just enough to boot, join WiFi and
// let an AI agent grow it over the air.
//
// Bring-up seed, grown layer by layer. What is driven today:
//   - I2C on SDA=3/SCL=2 + scan (XL9555, BQ*, TCA8418, …)
//   - XL9555 power rails, ST7796 480x222 + AW9364, notify beeper,
//     encoder UI, haptic, sound, keyboard reply, BQ27220 fuel gauge,
//     GNSS MIA-M10Q
// Not driven yet: SX1262 MeshCore RX stack (keys+P1 path ready), NFC, BQ25896.
// SPIFFS still ships WiFi + token so a dark-panel brick cannot lock us out.
// Endpoints:
//   GET  /health            — alive check (no auth)
//   GET  /capabilities      — hardware fingerprint
//   GET  /config.md         — node description
//   POST /config.md         — update description
//   GET  /events            — event log (?since=unix_ts)
//   GET  /clock             — local time, timezone, NTP sync state
//   POST /clock/tz          — set the POSIX TZ string (raw text/plain body)
//   GET  /firmware/version  — version, partition, uptime
//   POST /firmware/upload   — upload OTA binary (streaming)
//   POST /firmware/apply    — reboot into new firmware
//   POST /firmware/confirm  — confirm (cancel rollback)
//   POST /firmware/rollback — revert to previous
//   GET  /skill             — AI agent skill file
//   GET  /                  — WiFi config page
//   POST /wifi/config       — save WiFi credentials (adds to multi-profile list)
//   GET  /wifi/status       — STA + profiles
//   GET  /wifi/scan         — nearby SSIDs (blocking)
//   POST /wifi/networks     — replace multi-profile list (JSON)
//   WireGuard skill: /wg/*  — tunnel to home gateway

#include <Arduino.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include "board_pins.h"
#include "hw_ui.h"
#include "hw_input.h"
#include "hw_haptic.h"
#include "hw_sound.h"
#include "hw_kb.h"

// ===== Configuration =====
#define SEED_VERSION        "0.9.45"
// Core clock: datasheet puts 240 vs 80 ~11.5mA apart on WAITI. Periph bus holds
// at 80 for every PLL-fed core clock; go lower and RMT/I2S retimes. Same floor
// as tembed idle policy (no light sleep — notify latency is the job).
#define CPU_MHZ             80
#define HTTP_PORT           8080
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"
#define TZ_FILE             "/tz.txt"
// Max POST body collected into _tempObject by handle_body_collect (all routes
// incl. /agents/*). Was 4 KB; fat chat posts (16 KB plan) need headroom for
// JSON + escaping. 20 KB covers a 16 KB message with margin.
#define HTTP_BODY_MAX       20480
// Prague (CET/CEST). Overridable via POST /clock/tz or SPIFFS /tz.txt.
#define TZ_DEFAULT          "CET-1CEST,M3.5.0,M10.5.0/3"
// Anything older than this is the pre-NTP epoch, not a real wall clock.
#define TIME_VALID_EPOCH    1700000000

// Free GPIO for agents — only the external header pin is honestly free.
// Everything else is claimed by a peripheral (see board_pins.h).
static const int gpio_safe_pins_arr[] = { 9 };
static const int *gpio_safe_pins = gpio_safe_pins_arr;
static const int gpio_safe_pins_count = 1;

// ===== Events Ring Buffer =====
#define MAX_EVENTS          64
#define EVENT_MSG_LEN       128

struct EventEntry {
    unsigned long timestamp;
    char message[EVENT_MSG_LEN];
};

static EventEntry events_buf[MAX_EVENTS];
static int events_head = 0;
static int events_count = 0;

static void event_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EventEntry *e = &events_buf[events_head];
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000) {
        e->timestamp = (unsigned long)tv.tv_sec;
    } else {
        e->timestamp = millis() / 1000;
    }
    vsnprintf(e->message, EVENT_MSG_LEN, fmt, ap);
    events_head = (events_head + 1) % MAX_EVENTS;
    if (events_count < MAX_EVENTS) events_count++;
    va_end(ap);
}

// ===== Skill/plugin interface =====

struct SkillEndpoint {
    const char *method;       // "GET", "POST"
    const char *path;         // "/gpio/list"
    const char *description;  // "List available GPIO pins"
};

struct Skill {
    const char *name;         // "gpio"
    const char *version;      // "0.1.0"
    const char *(*describe)();                          // returns markdown
    const SkillEndpoint *endpoints;                     // NULL-terminated array
    void (*register_routes)(AsyncWebServer &server);    // registers routes on the server
    void (*tick)();           // called every loop() iteration, or nullptr if unused
};

#define MAX_SKILLS 16
static const Skill *g_skills[MAX_SKILLS];
static int g_skill_count = 0;

static int skill_register(const Skill *skill) {
    if (g_skill_count >= MAX_SKILLS) return -1;
    g_skills[g_skill_count++] = skill;
    return 0;
}

// ===== Hardware Probe (run once at boot, cached) =====

// Known I2C device addresses
struct I2CDevice {
    uint8_t addr;
    const char *name;
};

// Names for devices this board actually carries (LilyGo hardware table).
// Unknown addresses still show up in the scan with name=null.
static const I2CDevice known_i2c[] = {
    {0x18, "ES8311 audio codec"},
    {0x20, "XL9555 IO expander"},
    {0x28, "BHI260AP sensor"},
    {0x34, "TCA8418 keyboard"},
    {0x51, "PCF85063A RTC"},
    {0x55, "BQ27220 fuel gauge"},
    {0x5A, "DRV2605 haptic"},
    {0x6B, "BQ25896 charger"},
    {0, NULL}
};

static const char *i2c_identify(uint8_t addr) {
    for (int i = 0; known_i2c[i].name; i++) {
        if (known_i2c[i].addr == addr) return known_i2c[i].name;
    }
    return NULL;
}

// Probe results (cached at boot)
#define MAX_I2C_FOUND 16

struct I2CFound {
    uint8_t addr;
    const char *name;
};

struct HWProbe {
    // Chip
    const char *chip_model;
    uint8_t chip_revision;
    uint32_t flash_size;
    uint32_t flash_speed;
    uint32_t psram_size;
    float temp_c;

    // I2C bus (SDA=3, SCL=2)
    I2CFound i2c0[MAX_I2C_FOUND];
    int i2c0_count;

    // Battery via BQ27220 fuel gauge (I2C 0x55)
    bool has_battery;
    float battery_v;
    int battery_soc;   // 0..100, or -1 if Voltage ok but SoC not

    // Board guess
    const char *board;
};

static HWProbe hw;

static void i2c_scan(TwoWire &bus, I2CFound *results, int &count) {
    count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0 && count < MAX_I2C_FOUND) {
            results[count].addr = addr;
            results[count].name = i2c_identify(addr);
            count++;
        }
    }
}

// BQ27220 standard commands: 0x08 = Voltage (mV), 0x2C = StateOfCharge (%).
// Same part and registers as the T-Embed seed; capacity programming is left
// alone so a wrong Design Capacity never bricks the gauge from OTA.
static bool bq27220_read16(uint8_t reg, uint16_t &val) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_ADDR, 2) != 2) return false;
    val = Wire.read() | (Wire.read() << 8);
    return true;
}

static void probe_battery() {
    uint16_t mv = 0, soc = 0;
    if (bq27220_read16(0x08, mv) && mv > 2000 && mv < 6000) {
        hw.has_battery = true;
        hw.battery_v = mv / 1000.0f;
        if (bq27220_read16(0x2C, soc) && soc <= 100) {
            hw.battery_soc = (int)soc;
        } else {
            hw.battery_soc = -1;
        }
    } else {
        hw.has_battery = false;
        hw.battery_soc = -1;
    }
}

// probe_battery() runs once at boot; re-read so the clock BAT % stays honest.
static void battery_refresh() {
    if (!hw.has_battery) return;
    uint16_t mv = 0, soc = 0;
    if (!bq27220_read16(0x08, mv) || mv <= 2000 || mv >= 6000) return;
    hw.battery_v = mv / 1000.0f;
    hw.battery_soc = (bq27220_read16(0x2C, soc) && soc <= 100) ? (int)soc : -1;
}

static void hw_probe() {
    memset(&hw, 0, sizeof(hw));

    // Chip info
    hw.chip_model = ESP.getChipModel();
    hw.chip_revision = ESP.getChipRevision();
    hw.flash_size = ESP.getFlashChipSize();
    hw.flash_speed = ESP.getFlashChipSpeed();
    hw.psram_size = ESP.getPsramSize();
    hw.temp_c = temperatureRead();

    // Wire is already up from hw_ui_begin(); just scan.
    if (PIN_I2C_SDA >= 0 && PIN_I2C_SCL >= 0) {
        i2c_scan(Wire, hw.i2c0, hw.i2c0_count);
    }

    probe_battery();

    // Dedicated board build - this firmware only runs on T-Lora Pager hardware.
    hw.board = "T-Lora-Pager";

    Serial.printf("[probe] board: %s\n", hw.board);
    Serial.printf("[probe] temp: %.1fC, flash: %uMB, psram: %uKB\n",
        hw.temp_c, (unsigned)(hw.flash_size / 1024 / 1024),
        (unsigned)(hw.psram_size / 1024));
    Serial.printf("[probe] i2c: %d devices\n", hw.i2c0_count);
    for (int i = 0; i < hw.i2c0_count; i++) {
        Serial.printf("[probe]   0x%02X %s\n", hw.i2c0[i].addr,
            hw.i2c0[i].name ? hw.i2c0[i].name : "?");
    }
    if (hw.has_battery) {
        Serial.printf("[probe] battery: %.2fV soc=%d%%\n",
            hw.battery_v, hw.battery_soc);
    } else {
        Serial.printf("[probe] battery: not found\n");
    }
}

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
static String auth_token = "";
static String mdns_name = "";
static unsigned long boot_time = 0;

static String wifi_ssid = "";
static String wifi_pass = "";
static bool wifi_user_off = false;
static bool mdns_started = false;
#define WIFI_RETRY_MS 1200000UL
static unsigned long wifi_last_attempt_ms = 0;

/* Multi-profile STA: try each known network in order on boot / reconnect. */
#define WIFI_MAX_NETS 6
struct WifiNet {
    char ssid[33];
    char pass[65];
};
static WifiNet wifi_nets[WIFI_MAX_NETS];
static int wifi_net_count = 0;
static int wifi_net_idx = 0;

// OTA state
static bool firmware_confirmed = false;
static bool firmware_confirm_attempted = false;
static bool ota_in_progress = false;
static bool ota_upload_started = false;
static bool ota_upload_ok = false;
static bool ota_upload_error = false;
static char ota_upload_error_msg[128] = "";
static size_t ota_bytes_written = 0;
static volatile bool pending_restart = false;
static volatile bool pending_rollback = false;

// ===== Utilities =====

static String get_mac_suffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[5];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

static String read_spiffs_file(const char *path) {
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) return "";
    String content = f.readString();
    f.close();
    return content;
}

static bool write_spiffs_file(const char *path, const String &content) {
    // The ATS-Mini bracketed this write with encoder-ISR pauses, because its
    // encoder ISR read flash while SPIFFS had the cache disabled. This board has
    // no encoder ISR yet, so the write stands alone - the hazard returns the
    // day an ISR is added here.
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(content);
    f.close();
    return true;
}

// Write to tmp then rename over path — notify's snapshot must never be empty
// mid-write (power loss during a plain open(FILE_WRITE) would wipe the queue).
static bool write_spiffs_file_atomic(const char *path, const char *tmp_path,
                                     const String &content) {
    if (!write_spiffs_file(tmp_path, content)) return false;
    SPIFFS.remove(path);
    return SPIFFS.rename(tmp_path, path);
}

// Raised by notify endpoints; loop() paints and clears it. The endpoints never
// touch the panel themselves (AsyncTCP task must not hold SPI).
static volatile bool display_force = false;

// ===== Timezone =====
//
// The seed knows no city. It stores a raw POSIX TZ string in SPIFFS and hands
// it to the C library; whoever provisions the node decides what "local" means.

static String tz_string = TZ_DEFAULT;

static void tz_apply() {
    setenv("TZ", tz_string.c_str(), 1);
    tzset();
}

static void tz_load() {
    String stored = read_spiffs_file(TZ_FILE);
    stored.trim();
    if (stored.length() > 0) tz_string = stored;
    tz_apply();
}

// POSIX TZ strings are printable ASCII without spaces, e.g. "UTC0" or
// "CET-1CEST,M3.5.0,M10.5.0/3". Reject anything else rather than feed the
// C library a string that silently degrades to UTC.
static bool tz_valid(const String &tz) {
    if (tz.length() == 0 || tz.length() > 63) return false;
    for (unsigned int i = 0; i < tz.length(); i++) {
        char c = tz.charAt(i);
        if (c < 0x21 || c > 0x7E) return false;
    }
    return true;
}

// Local wall clock, or false before the first NTP sync (time() still near 0).
static bool clock_local_time(struct tm &out) {
    time_t now = time(NULL);
    if (now <= TIME_VALID_EPOCH) return false;
    return localtime_r(&now, &out) != NULL;
}

// ===== Auth =====

static void token_load() {
    auth_token = read_spiffs_file(TOKEN_FILE);
    auth_token.trim();

    if (auth_token.length() == 0) {
        char buf[33];
        for (int i = 0; i < 16; i++) {
            snprintf(buf + i * 2, 3, "%02x", (uint8_t)esp_random());
        }
        buf[32] = '\0';
        auth_token = String(buf);
        write_spiffs_file(TOKEN_FILE, auth_token);
    }
}

static bool check_token_constant_time(const String &provided) {
    if (provided.length() != auth_token.length()) return false;
    volatile uint8_t result = 0;
    for (size_t i = 0; i < auth_token.length(); i++) {
        result |= provided[i] ^ auth_token[i];
    }
    return result == 0;
}

static bool check_auth(AsyncWebServerRequest *request) {
    if (!request->hasHeader("Authorization")) return false;
    String auth = request->header("Authorization");
    if (!auth.startsWith("Bearer ")) return false;
    String token = auth.substring(7);
    token.trim();
    return check_token_constant_time(token);
}

static bool require_auth(AsyncWebServerRequest *request) {
    if (check_auth(request)) return true;
    request->send(401, "application/json",
        "{\"error\":\"Authorization: Bearer <token> required\"}");
    return false;
}

// ===== WiFi =====

static void wifi_nets_clear() {
    wifi_net_count = 0;
    wifi_net_idx = 0;
    memset(wifi_nets, 0, sizeof(wifi_nets));
}

static void wifi_nets_set_active(int idx) {
    if (idx < 0 || idx >= wifi_net_count) return;
    wifi_net_idx = idx;
    wifi_ssid = wifi_nets[idx].ssid;
    wifi_pass = wifi_nets[idx].pass;
}

/* Upsert one profile; promotes it to active ssid/password fields. */
static bool wifi_nets_upsert(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return false;
    if (!pass) pass = "";
    for (int i = 0; i < wifi_net_count; i++) {
        if (strcmp(wifi_nets[i].ssid, ssid) == 0) {
            snprintf(wifi_nets[i].pass, sizeof(wifi_nets[i].pass), "%s", pass);
            wifi_nets_set_active(i);
            return true;
        }
    }
    if (wifi_net_count >= WIFI_MAX_NETS) {
        /* Drop oldest, shift. */
        memmove(&wifi_nets[0], &wifi_nets[1],
                sizeof(WifiNet) * (WIFI_MAX_NETS - 1));
        wifi_net_count = WIFI_MAX_NETS - 1;
    }
    int i = wifi_net_count++;
    snprintf(wifi_nets[i].ssid, sizeof(wifi_nets[i].ssid), "%s", ssid);
    snprintf(wifi_nets[i].pass, sizeof(wifi_nets[i].pass), "%s", pass);
    wifi_nets_set_active(i);
    return true;
}

static bool wifi_persist_profiles() {
    JsonDocument doc;
    if (wifi_net_count > 0) {
        doc["ssid"] = wifi_nets[wifi_net_idx].ssid;
        doc["password"] = wifi_nets[wifi_net_idx].pass;
    } else {
        doc["ssid"] = wifi_ssid;
        doc["password"] = wifi_pass;
    }
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < wifi_net_count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = wifi_nets[i].ssid;
        o["password"] = wifi_nets[i].pass;
    }
    String json;
    serializeJson(doc, json);
    return write_spiffs_file(WIFI_CONFIG_FILE, json);
}

static void wifi_load_config() {
    wifi_nets_clear();
    String json = read_spiffs_file(WIFI_CONFIG_FILE);
    if (json.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    if (doc["networks"].is<JsonArray>()) {
        JsonArray arr = doc["networks"].as<JsonArray>();
        for (JsonObject o : arr) {
            const char *s = o["ssid"] | "";
            const char *p = o["password"] | "";
            if (!s[0]) continue;
            if (wifi_net_count >= WIFI_MAX_NETS) break;
            snprintf(wifi_nets[wifi_net_count].ssid,
                     sizeof(wifi_nets[wifi_net_count].ssid), "%s", s);
            snprintf(wifi_nets[wifi_net_count].pass,
                     sizeof(wifi_nets[wifi_net_count].pass), "%s", p);
            wifi_net_count++;
        }
    }
    /* Legacy single pair always accepted (and merged if not in list). */
    const char *leg_s = doc["ssid"] | "";
    const char *leg_p = doc["password"] | "";
    if (leg_s[0]) {
        bool found = false;
        for (int i = 0; i < wifi_net_count; i++) {
            if (strcmp(wifi_nets[i].ssid, leg_s) == 0) {
                found = true;
                wifi_net_idx = i;
                break;
            }
        }
        if (!found) wifi_nets_upsert(leg_s, leg_p);
        else wifi_nets_set_active(wifi_net_idx);
    } else if (wifi_net_count > 0) {
        wifi_nets_set_active(0);
    }
}

static bool wifi_save_config(const String &ssid, const String &pass) {
    wifi_nets_upsert(ssid.c_str(), pass.c_str());
    return wifi_persist_profiles();
}

/* Every connection attempt goes through one owner so a manual connect cannot
 * be followed by the periodic retry on the next loop pass. */
static void wifi_begin_active_profile() {
    wifi_last_attempt_ms = millis();
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

/* Deferred STA reconnect for HTTP handlers. They run on the AsyncTCP task,
 * which must never sit in delay() or in a blocking WiFi driver call — that
 * stalls every socket on the box (and, since the bus lock landed, can widen
 * SD-vs-panel arbitration windows). A handler only raises the request; the
 * loop task walks mode → disconnect → begin with millis() settle gaps
 * (the same 500/100 ms the old inline sequence used). */
enum {
    WIFI_RECONNECT_IDLE = 0,
    WIFI_RECONNECT_MODE,        /* set STA mode, then let RF settle */
    WIFI_RECONNECT_DISCONNECT,  /* drop the old association, then settle */
    WIFI_RECONNECT_BEGIN        /* start the asynchronous join */
};
#define WIFI_RECONNECT_MODE_SETTLE_MS 500UL
#define WIFI_RECONNECT_DISC_SETTLE_MS 100UL
static volatile int wifi_reconnect_state = WIFI_RECONNECT_IDLE;
static unsigned long wifi_reconnect_step_ms = 0;

static void wifi_reconnect_request() {
    wifi_reconnect_state = WIFI_RECONNECT_MODE;
}

/* Loop task only: one bounded step per pass, nothing ever waits in place. */
static void wifi_reconnect_poll() {
    if (wifi_reconnect_state == WIFI_RECONNECT_IDLE) return;
    if (wifi_user_off) {
        /* User toggled WiFi off after the request landed: their choice wins. */
        wifi_reconnect_state = WIFI_RECONNECT_IDLE;
        return;
    }
    switch (wifi_reconnect_state) {
    case WIFI_RECONNECT_MODE:
        WiFi.mode(WIFI_STA);
        wifi_reconnect_step_ms = millis();
        wifi_reconnect_state = WIFI_RECONNECT_DISCONNECT;
        break;
    case WIFI_RECONNECT_DISCONNECT:
        if (millis() - wifi_reconnect_step_ms < WIFI_RECONNECT_MODE_SETTLE_MS)
            return;
        WiFi.disconnect(false, false);
        wifi_reconnect_step_ms = millis();
        wifi_reconnect_state = WIFI_RECONNECT_BEGIN;
        break;
    case WIFI_RECONNECT_BEGIN:
        if (millis() - wifi_reconnect_step_ms < WIFI_RECONNECT_DISC_SETTLE_MS)
            return;
        wifi_reconnect_state = WIFI_RECONNECT_IDLE;
        wifi_begin_active_profile();
        break;
    }
}

static void wifi_setup() {
    WiFi.mode(WIFI_STA);
    /* Arduino-ESP32 defaults this to true and otherwise retries underneath our
     * scheduler, causing the same UI stalls the 20-minute cadence avoids. */
    WiFi.setAutoReconnect(false);
    String suffix = get_mac_suffix();
    mdns_name = "seed-" + suffix;
    mdns_name.toLowerCase();

    // Start SNTP with the stored TZ before associating: the daemon is
    // non-blocking and keeps retrying on its own, so the clock also syncs after
    // a later reconnect (loop() re-begins WiFi) instead of only on a successful
    // boot-time connect.
    configTzTime(tz_string.c_str(), "pool.ntp.org", "time.nist.gov");

    wifi_load_config();
    if (wifi_net_count > 0 || wifi_ssid.length() > 0) {
        // Boot must never wait for infrastructure. Start one asynchronous STA
        // attempt; loop() rotates profiles later while UI and MeshCore are live.
        Serial.printf("[wifi] boot async try %s\n", wifi_ssid.c_str());
        wifi_begin_active_profile();
    } else {
        Serial.println("[wifi] no credentials — continuing mesh-only");
    }
}

// ===== HTTP Handlers =====

static void handle_health(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["uptime_sec"] = (millis() - boot_time) / 1000;
    doc["type"] = "esp32-seed";
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;
    doc["arch"] = "xtensa-esp32s3";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_capabilities(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    doc["type"] = "esp32-seed";
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;
    doc["board"] = hw.board;
    doc["hostname"] = mdns_name;

    // Chip
    doc["chip"] = hw.chip_model;
    doc["chip_revision"] = hw.chip_revision;
    doc["arch"] = "xtensa-esp32s3";
    doc["os"] = "FreeRTOS";
    doc["cpus"] = ESP.getChipCores();
    doc["cpu_mhz"] = ESP.getCpuFreqMHz();
    doc["free_heap"] = (unsigned long)ESP.getFreeHeap();
    doc["min_free_heap"] = (unsigned long)ESP.getMinFreeHeap();
    doc["flash_mb"] = (unsigned long)(hw.flash_size / 1024 / 1024);
    doc["flash_mhz"] = (unsigned long)(hw.flash_speed / 1000000);
    if (hw.psram_size > 0) {
        doc["psram_kb"] = (unsigned long)(hw.psram_size / 1024);
    }
    doc["temp_c"] = serialized(String(hw.temp_c, 1));

    // Honest to what this build actually drives.
    doc["has_wifi"] = true;
    doc["has_bluetooth"] = true;
    doc["display"] = hw_ui_ready();
    doc["display_w"] = 480;
    doc["display_h"] = 222;
    doc["backlight"] = hw_ui_get_brightness();  // boot path; live = GET /backlight
    doc["xl9555"] = hw_ui_expand_ok();
    doc["peripherals_driven"] = "i2c-scan, xl9555, st7796, aw9364, backlight-idle, notify, progress, haptic, sound, keyboard, bq27220";
    doc["haptic"] = hw_haptic_ok();
    doc["sound"] = hw_sound_ok();
    doc["keyboard"] = hw_kb_ok();

    // Battery: BQ27220 fuel gauge at I2C 0x55.
    if (hw.has_battery) {
        doc["battery_v"] = serialized(String(hw.battery_v, 2));
        if (hw.battery_soc >= 0) doc["battery_soc"] = hw.battery_soc;
    }

    // I2C bus
    if (hw.i2c0_count > 0) {
        JsonArray bus0 = doc["i2c_bus0"].to<JsonArray>();
        for (int i = 0; i < hw.i2c0_count; i++) {
            JsonObject dev = bus0.add<JsonObject>();
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", hw.i2c0[i].addr);
            dev["addr"] = String(hex);
            if (hw.i2c0[i].name) dev["device"] = hw.i2c0[i].name;
        }
    }

    // GPIO: no pin is declared safe until the board map is verified, so this
    // array comes back empty rather than wrong.
    JsonArray pins = doc["gpio_safe"].to<JsonArray>();
    for (int i = 0; i < gpio_safe_pins_count; i++) pins.add(gpio_safe_pins[i]);

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_ip"] = WiFi.localIP().toString();
        doc["wifi_rssi"] = WiFi.RSSI();
    }

    JsonArray ep = doc["endpoints"].to<JsonArray>();
    const char *eps[] = {
        "/health", "/capabilities", "/config.md", "/events",
        "/clock", "/clock/tz",
        "/firmware/version", "/firmware/upload", "/firmware/apply",
        "/firmware/confirm", "/firmware/rollback",
        "/skill", NULL
    };
    for (int i = 0; eps[i]; i++) ep.add(eps[i]);

    // Skill endpoints
    for (int i = 0; i < g_skill_count; i++) {
        const SkillEndpoint *se = g_skills[i]->endpoints;
        for (int j = 0; se[j].path; j++) ep.add(se[j].path);
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_body_collect(AsyncWebServerRequest *request, uint8_t *data,
                                 size_t len, size_t index, size_t total) {
    if (index == 0) {
        if (total > HTTP_BODY_MAX) { request->send(413, "application/json", "{\"error\":\"body too large\"}"); return; }
        char *buf = (char*)malloc(total + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf) memcpy(buf + index, data, len);
    if (index + len == total && buf) buf[total] = '\0';
}

static void handle_config_get(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    String content = read_spiffs_file(CONFIG_MD_FILE);
    /* No config stored yet: return an empty JSON object instead of a bare 200
     * with an empty body, so a caller always gets valid, parseable content.
     * Config is optional (empty = no settings), so this is not a 404. */
    if (content.length() == 0) {
        request->send(200, "application/json", "{}");
        return;
    }
    request->send(200, "text/markdown; charset=utf-8", content);
}

static void handle_config_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    char *body = (char*)request->_tempObject;
    if (!body) { request->send(400, "application/json", "{\"error\":\"no body\"}"); return; }
    String content(body);
    free(body);
    request->_tempObject = nullptr;
    bool ok = write_spiffs_file(CONFIG_MD_FILE, content);
    request->send(ok ? 200 : 500, "application/json",
        ok ? "{\"ok\":true}" : "{\"error\":\"write failed\"}");
}

static void handle_events(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    unsigned long since = 0;
    if (request->hasParam("since"))
        since = strtoul(request->getParam("since")->value().c_str(), NULL, 10);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    int start = (events_count < MAX_EVENTS) ? 0 : events_head;
    for (int i = 0; i < events_count; i++) {
        int idx = (start + i) % MAX_EVENTS;
        if (events_buf[idx].timestamp >= since) {
            JsonObject e = arr.add<JsonObject>();
            e["ts"] = events_buf[idx].timestamp;
            e["msg"] = events_buf[idx].message;
        }
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- Clock ---

static void handle_clock_get(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    struct tm now;
    bool synced = clock_local_time(now);

    JsonDocument doc;
    doc["tz"] = tz_string;
    doc["synced"] = synced;
    doc["epoch"] = (unsigned long)time(NULL);
    if (synced) {
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now);
        doc["local_time"] = buf;
        strftime(buf, sizeof(buf), "%Z", &now);
        doc["tz_abbrev"] = buf;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// Body is a raw POSIX TZ string (text/plain), NOT JSON — collected by
// handle_body_collect into request->_tempObject.
static void handle_clock_tz(AsyncWebServerRequest *request) {
    char *body = (char*)request->_tempObject;
    if (!check_auth(request)) {
        if (body) { free(body); request->_tempObject = nullptr; }
        request->send(401, "application/json",
            "{\"error\":\"Authorization: Bearer <token> required\"}");
        return;
    }
    if (!body) {
        request->send(400, "application/json",
            "{\"error\":\"body must be a POSIX TZ string\"}");
        return;
    }
    String tz(body);
    free(body);
    request->_tempObject = nullptr;

    tz.trim();
    if (!tz_valid(tz)) {
        request->send(400, "application/json",
            "{\"error\":\"invalid TZ (POSIX string, 1..63 printable chars)\"}");
        return;
    }
    if (!write_spiffs_file(TZ_FILE, tz)) {
        request->send(500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }
    tz_string = tz;
    tz_apply();
    event_add("timezone set to %s", tz_string.c_str());

    JsonDocument doc;
    doc["ok"] = true;
    doc["tz"] = tz_string;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- Firmware OTA ---

static void handle_firmware_version(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    JsonDocument doc;
    doc["version"] = SEED_VERSION;
    doc["build_date"] = __DATE__;
    doc["build_time"] = __TIME__;
    doc["uptime_sec"] = (millis() - boot_time) / 1000;
    doc["partition"] = running ? running->label : "unknown";
    doc["free_heap"] = (unsigned long)ESP.getFreeHeap();
    doc["confirmed"] = firmware_confirmed;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_firmware_upload_body(AsyncWebServerRequest *request, uint8_t *data,
                                         size_t len, size_t index, size_t total) {
    if (index == 0) {
        if (!check_auth(request)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg), "auth required");
            return;
        }
        if (ota_in_progress) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg), "already in progress");
            return;
        }
        // OTA app slots are 4MB each (partitions/tlora-pager_16mb_ota.csv)
        if (total == 0 || total > 0x400000) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "invalid size: %u", (unsigned)total);
            return;
        }

        ota_in_progress = true;
        ota_upload_started = false;
        ota_upload_ok = false;
        ota_upload_error = false;
        ota_upload_error_msg[0] = '\0';
        ota_bytes_written = 0;

        event_add("ota upload started, size=%u", (unsigned)total);

        if (!Update.begin(total, U_FLASH)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "Update.begin failed: %s", Update.errorString());
            ota_in_progress = false;
            return;
        }
        ota_upload_started = true;
    }

    if (ota_upload_error) return;

    if (ota_upload_started && Update.isRunning()) {
        if (Update.write(data, len) != len) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "write failed: %s", Update.errorString());
            Update.abort();
            ota_in_progress = false;
            return;
        }
        ota_bytes_written += len;
    }

    if (index + len == total && ota_upload_started) {
        if (!Update.end(true)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "end failed: %s", Update.errorString());
        } else {
            ota_upload_ok = true;
            event_add("ota complete, %u bytes", (unsigned)ota_bytes_written);
        }
        ota_in_progress = false;
    }
}

static void handle_firmware_upload(AsyncWebServerRequest *request) {
    // The body handler authenticates the upload itself, but a POST with no body
    // never reaches it and would otherwise report the md5 and size of whatever
    // was last flashed. Checking here gates every path through, and makes the
    // body handler's own "auth required" unreachable from this side.
    if (!require_auth(request)) return;
    if (ota_upload_error) {
        JsonDocument doc;
        doc["error"] = ota_upload_error_msg;
        String response;
        serializeJson(doc, response);
        request->send(500, "application/json", response);
        return;
    }
    if (!ota_upload_ok) {
        request->send(400, "application/json", "{\"error\":\"no firmware uploaded\"}");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["bytes_written"] = ota_bytes_written;
    doc["md5"] = Update.md5String();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_firmware_apply(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    if (!ota_upload_ok) {
        request->send(400, "application/json",
            "{\"error\":\"upload first via POST /firmware/upload\"}");
        return;
    }
    event_add("ota apply: restarting");
    pending_restart = true;
    request->send(200, "application/json",
        "{\"ok\":true,\"warning\":\"restarting in ~1s\"}");
}

static void handle_firmware_confirm(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    firmware_confirmed = (err == ESP_OK);
    if (firmware_confirmed) event_add("firmware confirmed");
    JsonDocument doc;
    doc["ok"] = firmware_confirmed;
    doc["confirmed"] = firmware_confirmed;
    String response;
    serializeJson(doc, response);
    request->send(firmware_confirmed ? 200 : 500, "application/json", response);
}

static void handle_firmware_rollback(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    event_add("ota rollback");
    pending_restart = true;
    pending_rollback = true;
    request->send(200, "application/json",
        "{\"ok\":true,\"warning\":\"rolling back in ~1s\"}");
}

// --- GET /skill ---

static void handle_skill(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String ip = WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString() : String("offline");

    String s = "# ESP32 Seed - T-Lora Pager\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    s += "WiFi mode: STA only; no provisioning AP\n\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n\n";
    s += "## Grow cycle\n\n";
    s += "ESP32 has no compiler. Build on host, upload binary:\n\n";
    s += "1. GET /capabilities\n";
    s += "2. Write firmware (PlatformIO/Arduino/ESP-IDF)\n";
    s += "3. Compile: `pio run -e tlora-pager`\n";
    s += "4. POST /firmware/upload — send .bin (`-H 'Content-Type: application/octet-stream'`)\n";
    s += "5. POST /firmware/apply — reboot\n";
    s += "6. GET /health — verify\n";
    s += "7. POST /firmware/confirm (or auto after 60s)\n\n";
    s += "## Board gotchas\n\n";
    s += "- Driven: I2C scan, XL9555, ST7796 + AW9364, notify, progress, haptic, sound, keyboard, BQ27220\n";
    s += "- MeshCore: pair keys + P1→notify path (RX radio stack next); not yet: GNSS, NFC, BQ25896\n";
    s += "- Battery: BQ27220 at 0x55 — Voltage 0x08, SoC 0x2C; BAT % on clock header; /capabilities battery_v/soc\n";
    s += "- Beeper model: any LAN service POSTs /notify; MeshCore/Telegram are later transports into the same inbox\n";
    s += "- Four devices share SPI (display, SX1262, SD, NFC): park every CS HIGH\n";
    s += "- SX1262 needs DIO3 TCXO 3.0V + DIO2 RF switch or it hears nothing\n";
    s += "- Backlight is AW9364 pulse-count (0..16), not plain PWM\n";
    s += "- Native USB-JTAG flash needs --after watchdog_reset\n\n";
    s += "## API\n\n";
    s += "| Method | Path | Description |\n";
    s += "|--------|------|-------------|\n";
    s += "| GET | /health | Alive (no auth) |\n";
    s += "| GET | /capabilities | Hardware info |\n";
    s += "| GET | /config.md | Node config |\n";
    s += "| POST | /config.md | Update config |\n";
    s += "| GET | /events | Event log |\n";
    s += "| GET | /clock | Local time, timezone, NTP sync state |\n";
    s += "| POST | /clock/tz | Set POSIX TZ, raw body e.g. `CET-1CEST,M3.5.0,M10.5.0/3` |\n";
    s += "| GET | /firmware/version | Version |\n";
    s += "| POST | /firmware/upload | Upload .bin |\n";
    s += "| POST | /firmware/apply | Apply + reboot |\n";
    s += "| POST | /firmware/confirm | Confirm |\n";
    s += "| POST | /firmware/rollback | Rollback |\n";
    s += "| POST | /gw/token | Set gateway capability token (raw body or JSON token field; empty clears) |\n";
    s += "| GET | /skill | This file |\n";

    // Skill endpoints
    for (int i = 0; i < g_skill_count; i++) {
        const SkillEndpoint *se = g_skills[i]->endpoints;
        for (int j = 0; se[j].path; j++) {
            s += "| " + String(se[j].method) + " | " + String(se[j].path) +
                 " | " + String(se[j].description) + " |\n";
        }
    }

    // Skill descriptions
    for (int i = 0; i < g_skill_count; i++) {
        s += "\n";
        s += g_skills[i]->describe();
    }

    request->send(200, "text/markdown; charset=utf-8", s);
}

// --- WiFi config page ---

static void handle_wifi_page(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Seed WiFi</title>"
        "<style>body{font-family:monospace;max-width:400px;margin:40px auto;padding:0 20px}"
        "input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
        "button{padding:10px 20px;background:#333;color:#fff;border:none;cursor:pointer}"
        "</style></head><body>"
        "<h2>ESP32 Seed — WiFi</h2>"
        "<form method='POST' action='/wifi/config'>"
        "<label>SSID:</label><input type='text' name='ssid' required>"
        "<label>Password:</label><input type='password' name='pass'>"
        "<button type='submit'>Connect</button>"
        "</form>"
        "<p><a href='/ui' style='color:#58a6ff'>Radio UI</a></p>";
    if (WiFi.status() == WL_CONNECTED)
        html += "<p>Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
}

static void handle_wifi_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String ssid = "", pass = "";
    if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
    if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();
    if (ssid.length() == 0) {
        request->send(400, "text/html", "<h2>SSID required</h2><a href='/'>Back</a>");
        return;
    }
    wifi_save_config(ssid, pass);
    request->send(200, "text/html",
        "<h2>Saved. Connecting...</h2><p>" + ssid + "</p><a href='/'>Back</a>");
    /* AsyncTCP task: never wait here, never call into the WiFi driver. The
     * loop task applies mode → disconnect → begin with millis() settle gaps. */
    wifi_user_off = false;
    wifi_reconnect_request();
}

static void handle_wifi_status(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    JsonDocument doc;
    doc["connected"] = (WiFi.status() == WL_CONNECTED);
    doc["user_off"] = wifi_user_off;
    doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifi_ssid;
    doc["ip"] = WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString() : "";
    if (WiFi.status() == WL_CONNECTED) doc["rssi"] = WiFi.RSSI();
    doc["active_idx"] = wifi_net_idx;
    doc["profile_count"] = wifi_net_count;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < wifi_net_count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = wifi_nets[i].ssid;
        o["has_pass"] = wifi_nets[i].pass[0] != '\0';
        o["active"] = (i == wifi_net_idx);
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_wifi_scan(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    /* Blocking scan — user-triggered only; firmware remains STA-only. */
    if (wifi_user_off) {
        WiFi.mode(WIFI_STA);
    }
    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
    JsonDocument doc;
    doc["count"] = n < 0 ? 0 : n;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 24; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    if (wifi_user_off) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_wifi_networks_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    char *body = (char *)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"body required\"}");
        return;
    }
    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) {
        free(body);
        request->_tempObject = nullptr;
        request->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    free(body);
    request->_tempObject = nullptr;

    if (!input["networks"].is<JsonArray>()) {
        request->send(400, "application/json",
                      "{\"error\":\"need networks:[{ssid,password}]\"}");
        return;
    }
    wifi_nets_clear();
    for (JsonObject o : input["networks"].as<JsonArray>()) {
        const char *s = o["ssid"] | "";
        const char *p = o["password"] | "";
        if (!s[0]) continue;
        wifi_nets_upsert(s, p);
    }
    if (wifi_net_count == 0) {
        request->send(400, "application/json", "{\"error\":\"empty list\"}");
        return;
    }
    wifi_persist_profiles();
    event_add("wifi profiles=%d primary=%s", wifi_net_count, wifi_ssid.c_str());

    JsonDocument out;
    out["ok"] = true;
    out["count"] = wifi_net_count;
    out["ssid"] = wifi_ssid;
    String response;
    serializeJson(out, response);
    request->send(200, "application/json", response);

    /* Kick reconnect with the new list — deferred to the loop task; WiFi
     * driver calls can block and this handler runs on the AsyncTCP task. */
    wifi_user_off = false;
    wifi_reconnect_request();
}

// ===== Skills =====
// Included into this TU so they share auth, SPIFFS, event_add, display_force.
// build_src_filter excludes skills/ from separate compilation (same as tembed).
#include "skills/notify.cpp"
// After notify: reuses notify_send_json / notify_send_error / notify_ingest_p1.
#include "skills/progress.cpp"
#include "skills/agents.cpp"
#include "skills/meshcore.cpp"
#include "skills/backlight.cpp"
#include "skills/wg.cpp"
#include "skills/gps.cpp"    // after notify: reuses notify_send_json; uses hw_ui_expand_ok

static void skills_init() {
    skill_notify_init();
    skill_progress_init();
    skill_agents_init();
    skill_meshcore_init();
    skill_backlight_init();
    skill_wg_init();
    skill_gps_init();
}

// Build one second of the home clock face (tembed look, 480x222).
// Lives after notify.cpp so it can read unread/crit without a second copy.
static void ui_clock_paint(const char *note) {
    if (!hw_ui_ready()) return;

    char ver[12];
    snprintf(ver, sizeof(ver), "v%s", SEED_VERSION);

    char batt[16];
    if (hw.has_battery && hw.battery_soc >= 0) {
        snprintf(batt, sizeof(batt), "BAT %d%%", hw.battery_soc);
    } else if (hw.has_battery) {
        snprintf(batt, sizeof(batt), "BAT --");
    } else {
        batt[0] = '\0';
    }

    char addr[24];
    char row1l[36], row1r[36], row2[56];
    row1l[0] = row1r[0] = row2[0] = '\0';

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(addr, sizeof(addr), "%s", WiFi.localIP().toString().c_str());
        snprintf(row1l, sizeof(row1l), "RSSI %d dBm", (int)WiFi.RSSI());
        snprintf(row1r, sizeof(row1r), "%s.local", mdns_name.c_str());
    } else {
        snprintf(addr, sizeof(addr), "offline");
        snprintf(row1l, sizeof(row1l), "WiFi offline");
        snprintf(row2, sizeof(row2), "mesh ready - STA retry async");
    }
    if (hw_kb_locked()) {
        /* LOCK takes the note row so pocket mode is obvious. */
        snprintf(row2, sizeof(row2), "LOCKED  long CAPS unlock");
    }
    if (note && note[0]) snprintf(row2, sizeof(row2), "%s", note);

    // Progress borrows the note row when it is free (same rule as tembed).
    int bar_pct = -1;
    if (row2[0] == '\0') progress_status_line(row2, sizeof(row2), &bar_pct);

    char date[28];
    int hh = 0, mm = 0, ss = 0;
    bool ok = false;
    struct tm now;
    if (clock_local_time(now)) {
        ok = true;
        hh = now.tm_hour; mm = now.tm_min; ss = now.tm_sec;
        strftime(date, sizeof(date), "%a %d %b %Y", &now);
    } else {
        snprintf(date, sizeof(date), "waiting for NTP");
    }

    hw_ui_clock_tick(ver, batt, addr, row1l, row1r, row2,
                     notify_unread_count(),
                     ok, hh, mm, ss, date,
                     notify_crit_unread(),
                     mesh_ui_state(),
                     mesh_alive_age_s(),
                     wg_ui_state());
    hw_ui_clock_bar(bar_pct);
}

static void ui_go_clock(const char *note) {
    hw_ui_show_clock();
    ui_clock_paint(note);
}

// Full redraw right after tft_wake(), called by backlight_poll BEFORE the
// backlight rises (loop task only). Idle blanking always lands on the clock
// (UI_IDLE_MS returns every face to it long before BL_IDLE_OFF_S); any other
// face can only be dark via a manual level-0 set, and the ST7796 keeps its
// frame memory in sleep-in, so those faces relight with their last frame.
static void ui_blank_wake_repaint() {
    if (hw_ui_screen() != HW_UI_CLOCK) return;
    hw_ui_invalidate_clock();
    ui_clock_paint(NULL);
}

// --- Front-panel state (encoder) --------------------------------------------
// MENU items
enum {
    MENU_MESSAGES = 0,
    MENU_AGENTS,
    MENU_MESHCORE,
    MENU_WIFI,
    MENU_SETTINGS,
    MENU_INFO,
    MENU_BACK,
    MENU_COUNT
};
// Card action sheet (click / Enter on a notification)
enum { CARD_ACT_ACK = 0, CARD_ACT_REPLY, CARD_ACT_BACK, CARD_ACT_COUNT };
// Agents list: 0..4 = agents, 5 = BACK
enum { AGENTS_BACK = 5, AGENTS_LIST_COUNT = 6 };
// In-chat menu (click while in agent chat room)
enum { AGENT_ACT_CLEAR = 0, AGENT_ACT_BACK, AGENT_ACT_COUNT };
// Layout picker: 0=ABC 1=PHON 2=RU 3=BACK
enum { LAYOUT_BACK = 3, LAYOUT_LIST_COUNT = 4 };
// SETTINGS: 0=LAYOUT 1=BACKLIGHT 2=AUTO-DIM 3=BACK
enum {
    SETTINGS_LAYOUT = 0,
    SETTINGS_BACKLIGHT = 1,
    SETTINGS_AUTODIM = 2,
    SETTINGS_BACK = 3,
    SETTINGS_LIST_COUNT = 4
};
// MeshCore menu: 0=STATUS 1=PING 2=BACK
enum { MESH_ACT_STATUS = 0, MESH_ACT_PING = 1, MESH_ACT_BACK = 2, MESH_LIST_COUNT = 3 };
// WiFi menu: 0=STATUS 1=SCAN 2=PROFILES 3=TOGGLE 4=BACK
enum {
    WIFI_ACT_STATUS = 0,
    WIFI_ACT_SCAN = 1,
    WIFI_ACT_PROFILES = 2,
    WIFI_ACT_TOGGLE = 3,
    WIFI_ACT_BACK = 4,
    WIFI_LIST_COUNT = 5
};
// WiFi list mode (scan results vs saved profiles)
enum { WIFI_LIST_SCAN = 0, WIFI_LIST_PROFILES = 1 };
static int menu_sel = 0;
static int card_act_sel = 0;
static int agents_sel = 0;
static int agent_act_sel = 0;
static int layout_sel = 0;
static int settings_sel = 0;
static int mesh_sel = 0;
static int wifi_sel = 0;
// User turned WiFi off from the menu: suppress the loop() auto-reconnect until
// they toggle it back on (garden mesh-only testing).
static int wifi_list_sel = 0;
static int wifi_list_mode = WIFI_LIST_SCAN;
static int wifi_list_count = 0;
static char wifi_list_titles[HW_UI_WIFI_LIST_MAX][36];
static char wifi_list_ssids[HW_UI_WIFI_LIST_MAX][33];
static bool wifi_compose = false;           // REPLY screen = enter WiFi password
static char wifi_pending_ssid[33] = "";

// Home MeshCore gateway (Heltec daemon). Overridable via SPIFFS /mesh_gw.txt
#define MESH_GW_PATH "/mesh_gw.txt"
#define MESH_GW_DEFAULT "http://192.168.1.138:8325"
static char mesh_gw_url[96] = MESH_GW_DEFAULT;
// Gateway capability token: the gateway authenticates every route except
// /health, so gateway HTTP calls (/ping, /reply) carry it as
// `Authorization: Bearer <token>` when it is set. Optional — an empty or
// missing file means the requests go out header-less, exactly as before the
// gateway grew auth. Provisioned via POST /gw/token or SPIFFS /gw_token.txt.
#define GW_TOKEN_PATH "/gw_token.txt"
#define GW_TOKEN_TMP  "/gw_token.tmp"
#define GW_TOKEN_MAX  128
static String gw_token = "";
static int agent_focus = 0;   // which agent chat is open (0..AGENTS_N-1)
static bool agent_compose = false;  // REPLY screen is for agent, not notify
static int agent_scroll = -1;       // visual row; -1 = pin to latest
static int agent_scroll_total = 0;  // last known wrapped row count
static int agent_sess_sel = 0;      // selected row on the agent SESSIONS list
// Chat window copy: only the current viewport (AGENT_THREAD_MAX lines) — the
// full history lives on SD, so no whole-thread RAM snapshot is held.
static char ag_chat_lines[AGENT_THREAD_MAX][AGENT_TEXT_LEN];
static bool ag_chat_from_me[AGENT_THREAD_MAX];
static uint32_t ag_chat_ts[AGENT_THREAD_MAX];
static const char *ag_chat_ptrs[AGENT_THREAD_MAX];
// Session-list screen rows: N sessions + "NEW SESSION" + "BACK".
// ag_sess_msgs[i] < 0 ⇒ no message-count badge on that row.
static char ag_sess_titles[AGENT_SESSIONS_MAX + 2][AGENT_SESSION_LEN + 2];
static int  ag_sess_msgs[AGENT_SESSIONS_MAX + 2];
static bool ag_sess_active[AGENT_SESSIONS_MAX + 2];
static const char *ag_sess_ptrs[AGENT_SESSIONS_MAX + 2];
static int msglist_sel = 0;
// Cached ids for the visible msglist rows (map row → notify id).
static uint32_t msglist_ids[HW_UI_MSGLIST_MAX];
static int msglist_count = 0;
// Notify card currently shown (for ack-on-click / reply).
static uint32_t notify_card_id = 0;
static char reply_title[NOTIFY_TITLE_LEN];
// UTF-8 draft: keep room for ~40 Cyrillic codepoints (2–3 bytes each).
static char reply_buf[192];
// Idle return to clock (Advisor: shelf is a clock). Longer while typing.
// Same timestamp drives backlight auto-dim.
static unsigned long ui_last_input_ms = 0;
#define UI_IDLE_MS        15000
#define UI_IDLE_REPLY_MS  60000
#define KB_LAYOUT_PATH    "/kb_layout.txt"

static_assert(BL_IDLE_DIM_MS > UI_IDLE_MS,
              "backlight must not dim while a message card can still be on screen");

static void ui_note_input() { ui_last_input_ms = millis(); }

// SYSTEM events only (a message arriving): wake the panel and restart the
// idle/dim countdown once per event; repaints and synthetic errors must not call this.
static void ui_note_wake() { ui_last_input_ms = millis(); }

// SETTINGS face: rebuild labels from backlight skill and paint.
static void ui_open_settings() {
    char bl[BL_LABEL_MAX];
    bl_level_label(backlight_wanted(), bl, sizeof(bl));
    hw_ui_show_settings(settings_sel, bl, bl_idle_word());
    ui_note_input();
}

// Hand the idle policy the elapsed time + what is drawn. Loop only.
static void ui_backlight_idle() {
    BacklightPanel panel;
    panel.on_bar = progress_shown.present;
    panel.crit_unread = notify_crit_unread();
    backlight_idle(millis() - ui_last_input_ms, panel);
}

// ---- keyboard layout persist ----------------------------------------------
static void kb_layout_load() {
    if (!SPIFFS.exists(KB_LAYOUT_PATH)) return;
    File f = SPIFFS.open(KB_LAYOUT_PATH, "r");
    if (!f) return;
    char id[16] = {0};
    size_t n = f.readBytes(id, sizeof(id) - 1);
    f.close();
    while (n > 0 && (id[n - 1] == '\n' || id[n - 1] == '\r' || id[n - 1] == ' '))
        id[--n] = '\0';
    HwKbLayout lay;
    if (hw_kb_layout_from_id(id, &lay)) {
        hw_kb_set_layout(lay);
        hw_kb_take_layout_changed();  // don't flash badge on boot
    }
}

static void mesh_gw_load() {
    snprintf(mesh_gw_url, sizeof(mesh_gw_url), "%s", MESH_GW_DEFAULT);
    if (!SPIFFS.exists(MESH_GW_PATH)) return;
    File f = SPIFFS.open(MESH_GW_PATH, "r");
    if (!f) return;
    String s = f.readString();
    f.close();
    s.trim();
    if (s.startsWith("http://") && s.length() < (int)sizeof(mesh_gw_url))
        snprintf(mesh_gw_url, sizeof(mesh_gw_url), "%s", s.c_str());
}

static void gw_token_load() {
    gw_token = read_spiffs_file(GW_TOKEN_PATH);
    gw_token.trim();
}

// ---- Reply upstream ---------------------------------------------------------
//
// A reply typed on a card is stored on the device and read back by whoever is
// still waiting on the line (GET /notify/one). That only serves a caller that
// stayed. Pushing the same reply to the home gateway serves the one that did
// not: the gateway holds the routing table and hands it to whichever service
// asked the question.
//
// Two paths, same order the rest of this firmware uses. WiFi is
// POST {gw}/reply, carrying the gateway capability token as a Bearer header
// when one is provisioned (the gateway rejects tokenless requests with 401
// since it grew auth middleware). When that does not land — no STA, no
// route, anything but a 2xx — one private mesh DM carries R1|key|reply instead,
// which is the C1 uplink mechanism with a single frame and no reassembly.
//
// The client key is the card's `id`. Without one there is nobody to route to,
// so nothing is sent and the reply behaves exactly as it did before: stored,
// and available to a poller.
//
// Neither path is allowed to fail loudly. The reply is already in the store by
// the time this runs, so a gateway that is down costs a routed delivery and
// nothing else.
#define REPLY_UP_FRAME_MAX  150   // one MeshCore DM, same budget as C1 uplink

// Staged by Enter, drained by loop(). ui_reply_submit() runs on the loop task
// already — it is reached from the keyboard drain — so this is not about task
// safety but about the clock: an HTTP POST with a 5 s timeout inside the key
// handler freezes the panel between the keystroke and the "sent" face. Raising
// a flag lets the card paint and return to the clock first, and the blocking
// work happens further down the same loop pass with nothing left on screen
// waiting for it. One slot, because it is drained on the pass that fills it and
// a second Enter cannot arrive inside one iteration.
static volatile bool reply_up_pending = false;
static char reply_up_key[NOTIFY_KEY_LEN];
static char reply_up_text[NOTIFY_REPLY_LEN];

static void reply_upstream_queue(const char *key, const char *text) {
    if (!key || !key[0] || !text || !text[0]) return;
    snprintf(reply_up_key, sizeof(reply_up_key), "%s", key);
    snprintf(reply_up_text, sizeof(reply_up_text), "%s", text);
    reply_up_pending = true;
}

// POST {gw}/reply {"key":…,"reply":…}. True only on 2xx — anything else is a
// gateway that did not take it, and the mesh path is what that is for.
static bool reply_upstream_http(const char *key, const char *text) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!mesh_gw_url[0]) return false;

    size_t bl = strlen(mesh_gw_url);
    while (bl > 0 && mesh_gw_url[bl - 1] == '/') bl--;
    char url[sizeof(mesh_gw_url) + 8];
    snprintf(url, sizeof(url), "%.*s/reply", (int)bl, mesh_gw_url);

    JsonDocument doc;
    doc["key"] = key;
    doc["reply"] = text;
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    // Runs on the loop task: a black-holed gateway must cost ~1 s to connect,
    // not the whole 5 s read window, or the panel freezes for the difference.
    http.setConnectTimeout(1000);
    http.setTimeout(5000);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (gw_token.length() > 0) {
        http.addHeader("Authorization", String("Bearer ") + gw_token);
    }
    int code = http.POST(body);
    http.end();
    return code >= 200 && code < 300;
}

// One private DM: R1|key|reply, cut to the frame budget on a codepoint
// boundary. A reply long enough to need splitting is a reply the gateway can
// act on truncated — the whole text stays on the device for GET /notify/one —
// so this stays one frame rather than growing a second reassembly protocol.
static bool reply_upstream_mesh(const char *key, const char *text) {
    if (!g_mesh.has_identity || !g_mesh.radio_ready) return false;
    if (!g_mesh.heltec_pk_hex[0]) return false;
    if (!mesh_client_ready()) return false;
    if (mesh_client_ack_pending()) return false;

    char frame[REPLY_UP_FRAME_MAX + 1];
    int n = snprintf(frame, sizeof(frame), "R1|%s|", key);
    if (n < 0 || n >= (int)sizeof(frame)) return false;

    size_t room = sizeof(frame) - 1 - (size_t)n;
    size_t take = strlen(text);
    if (take > room) take = room;
    // text[take] is the first byte left behind: a continuation byte there means
    // the cut landed inside a character, so give the whole character back.
    while (take > 0 && ((unsigned char)text[take] & 0xC0) == 0x80) take--;
    if (take == 0) return false;
    memcpy(frame + n, text, take);
    frame[n + take] = '\0';

    uint32_t ack = 0, est = 0;
    return mesh_client_send_to_gateway(frame, &ack, &est);
}

static void reply_upstream_poll() {
    if (!reply_up_pending) return;

    char key[NOTIFY_KEY_LEN];
    char text[NOTIFY_REPLY_LEN];
    snprintf(key, sizeof(key), "%s", reply_up_key);
    snprintf(text, sizeof(text), "%s", reply_up_text);
    if (!key[0] || !text[0]) return;

    if (reply_upstream_http(key, text)) {
        reply_up_pending = false;
        event_add("reply upstream wifi ok: %s", key);
        return;
    }
    // C1/probe/R1 share one MeshCore ACK slot. Keep this reply pending until
    // the current private frame completes instead of overwriting its CRC.
    if (mesh_client_ack_pending()) return;
    if (reply_upstream_mesh(key, text)) {
        reply_up_pending = false;
        event_add("reply upstream mesh R1: %s", key);
        return;
    }
    reply_up_pending = false;
    event_add("reply upstream unreachable: %s (kept on device)", key);
}

// Pull one JSON string field "key":"value" into out (best-effort, no nested).
static bool json_str_field(const char *js, const char *key, char *out, size_t out_n) {
    if (!js || !key || !out || out_n == 0) return false;
    out[0] = '\0';
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_n) {
        if (*p == '\\' && p[1]) { p++; }  // skip escape
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool json_num_field(const char *js, const char *key, long *out) {
    if (!js || !key || !out) return false;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static void ui_open_meshcore() {
    mesh_sel = 0;
    hw_ui_show_meshcore(mesh_sel);
    ui_note_input();
}

static void ui_reply_paint();  // defined later (WiFi password reuses reply face)

static void ui_open_wifi() {
    wifi_sel = 0;
    wifi_compose = false;
    hw_ui_show_wifi(wifi_sel);
    ui_note_input();
}

static void ui_wifi_paint_list() {
    const char *ptrs[HW_UI_WIFI_LIST_MAX];
    for (int i = 0; i < wifi_list_count; i++)
        ptrs[i] = wifi_list_titles[i];
    const char *hdr = (wifi_list_mode == WIFI_LIST_PROFILES) ? "PROFILES" : "SCAN";
    hw_ui_show_wifi_list(hdr, ptrs, wifi_list_count, wifi_list_sel);
    ui_note_input();
}

static void ui_wifi_show_status() {
    static char lines[12][42];
    static const char *ptrs[12];
    int n = 0;
    auto add = [&](const char *fmt, ...) {
        if (n >= 12) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(lines[n], sizeof(lines[0]), fmt, ap);
        va_end(ap);
        ptrs[n] = lines[n];
        n++;
    };
    if (WiFi.status() == WL_CONNECTED) {
        add("SSID %s", WiFi.SSID().c_str());
        add("IP   %s", WiFi.localIP().toString().c_str());
        add("RSSI %d dBm", (int)WiFi.RSSI());
    } else {
        add("WiFi %s", wifi_user_off ? "OFF (mesh only)" : "STA offline");
    }
    add("profiles %d  idx %d", wifi_net_count, wifi_net_idx);
    for (int i = 0; i < wifi_net_count && n < 10; i++) {
        add("%c %s", (i == wifi_net_idx) ? '*' : ' ', wifi_nets[i].ssid);
    }
    switch (wg_ui_state()) {
    case WG_UI_OK:    add("WG  UP (tunnel)"); break;
    case WG_UI_WAIT:  add("WG  wait"); break;
    case WG_UI_STALE: add("WG  stale"); break;
    case WG_UI_DOWN:  add("WG  DOWN"); break;
    default:          add("WG  off / no config"); break;
    }
    hw_ui_show_wifi_info(ptrs, n);
    ui_note_input();
}

// WiFi on/off switch for mesh-only testing in the garden: STA ON (reconnect via
// saved profiles) or STA OFF (disconnect, leave mesh/WG path alone).
static void ui_wifi_toggle() {
    if (!wifi_user_off) {
        wifi_user_off = true;
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        event_add("wifi off (mesh only)");
        Serial.println("[wifi] user toggled OFF");
    } else {
        wifi_user_off = false;
        WiFi.mode(WIFI_STA);
        if (wifi_net_count > 0 || wifi_ssid.length() > 0) {
            wifi_begin_active_profile();
            event_add("wifi on (async reconnect)");
            Serial.println("[wifi] user toggled ON — reconnecting");
        } else {
            event_add("wifi on requested, no saved profile");
            Serial.println("[wifi] user toggled ON — no saved profile");
        }
    }
    ui_wifi_show_status();
    ui_note_input();
}

static void ui_wifi_do_scan() {
    /* Blocking scan — paint "SCAN…" first so the panel is not frozen blank. */
    static const char *wait_lines[] = { "scanning…", "hold on" };
    hw_ui_show_wifi_info(wait_lines, 2);
    delay(50);

    if (wifi_user_off) {
        WiFi.mode(WIFI_STA);
    }

    int found = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    wifi_list_mode = WIFI_LIST_SCAN;
    wifi_list_count = 0;
    wifi_list_sel = 0;
    if (found < 0) found = 0;
    for (int i = 0; i < found && wifi_list_count < HW_UI_WIFI_LIST_MAX; i++) {
        String ss = WiFi.SSID(i);
        if (ss.length() == 0) continue;
        snprintf(wifi_list_ssids[wifi_list_count],
                 sizeof(wifi_list_ssids[0]), "%s", ss.c_str());
        bool known = false;
        for (int j = 0; j < wifi_net_count; j++) {
            if (strcmp(wifi_nets[j].ssid, wifi_list_ssids[wifi_list_count]) == 0) {
                known = true;
                break;
            }
        }
        snprintf(wifi_list_titles[wifi_list_count],
                 sizeof(wifi_list_titles[0]),
                 "%s%c %ddBm",
                 wifi_list_ssids[wifi_list_count],
                 known ? '*' : ' ',
                 (int)WiFi.RSSI(i));
        wifi_list_count++;
    }
    WiFi.scanDelete();
    if (wifi_user_off) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    ui_wifi_paint_list();
}

static void ui_wifi_show_profiles() {
    wifi_list_mode = WIFI_LIST_PROFILES;
    wifi_list_count = 0;
    wifi_list_sel = 0;
    for (int i = 0; i < wifi_net_count && wifi_list_count < HW_UI_WIFI_LIST_MAX; i++) {
        snprintf(wifi_list_ssids[wifi_list_count],
                 sizeof(wifi_list_ssids[0]), "%s", wifi_nets[i].ssid);
        snprintf(wifi_list_titles[wifi_list_count],
                 sizeof(wifi_list_titles[0]),
                 "%c %s",
                 (i == wifi_net_idx) ? '*' : ' ',
                 wifi_nets[i].ssid);
        wifi_list_count++;
    }
    ui_wifi_paint_list();
}

static void ui_wifi_open_password(const char *ssid) {
    if (!ssid || !ssid[0]) return;
    snprintf(wifi_pending_ssid, sizeof(wifi_pending_ssid), "%s", ssid);
    wifi_compose = true;
    agent_compose = false;
    notify_card_id = 0;
    snprintf(reply_title, sizeof(reply_title), "PASS %s", ssid);
    reply_buf[0] = '\0';
    hw_kb_reset_mods();
    ui_reply_paint();
    ui_note_input();
}

/* Connect to a saved profile by ssid, or open password entry if unknown. */
static void ui_wifi_connect_ssid(const char *ssid) {
    if (!ssid || !ssid[0]) return;
    for (int i = 0; i < wifi_net_count; i++) {
        if (strcmp(wifi_nets[i].ssid, ssid) == 0) {
            wifi_nets_set_active(i);
            wifi_persist_profiles();
            static char lines[4][40];
            static const char *ptrs[4];
            snprintf(lines[0], sizeof(lines[0]), "connecting…");
            snprintf(lines[1], sizeof(lines[1]), "%s", ssid);
            ptrs[0] = lines[0];
            ptrs[1] = lines[1];
            hw_ui_show_wifi_info(ptrs, 2);
            delay(30);
            wifi_user_off = false;
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(false, false);
            delay(80);
            wifi_begin_active_profile();
            snprintf(lines[0], sizeof(lines[0]), "CONNECTING");
            snprintf(lines[1], sizeof(lines[1]), "%s", ssid);
            snprintf(lines[2], sizeof(lines[2]), "async - UI stays live");
            ptrs[2] = lines[2];
            hw_ui_show_wifi_info(ptrs, 3);
            event_add("wifi menu async connect %s", ssid);
            ui_note_input();
            return;
        }
    }
    /* Unknown network from scan → type password. */
    ui_wifi_open_password(ssid);
}

// MESHCORE → STATUS: pair keys + RF policy (from SPIFFS /mesh_*).
static void ui_mesh_show_status() {
    static char lines[HW_UI_MESH_PING_LINES][40];
    static const char *ptrs[HW_UI_MESH_PING_LINES];
    char raw[HW_UI_MESH_PING_LINES][40];
    int n = mesh_status_lines(raw, HW_UI_MESH_PING_LINES);
    for (int i = 0; i < n; i++) {
        snprintf(lines[i], sizeof(lines[i]), "%s", raw[i]);
        ptrs[i] = lines[i];
    }
    const char *phase = g_mesh.has_identity ? "PAIR" : "NOKEY";
    hw_ui_show_mesh_ping(phase, ptrs, n);
    ui_note_input();
}

// MESHCORE → PING GATEWAY: dual-path glance (WiFi HTTP + Mesh private DM).
// Final face = two big icons (who is up) + strength under each.
//
// Non-blocking: opening the PING face arms a small state machine that loop()
// advances one bounded step per pass via ui_mesh_ping_poll(). The WiFi HTTP
// GET stays synchronous but is bounded (1 s connect + 4 s read); the mesh
// pong wait is a millis() deadline checked once per pass while the meshcore
// skill tick keeps the radio polled — the old skill_meshcore_poll()+delay(25)
// spin froze the UI for up to 2.8 s. Leaving the PING face mid-sequence
// (click, HOME, idle-to-clock) cancels the run: the poll bails the moment the
// screen is no longer HW_UI_MESH_PING, so no stale result can paint into
// another face.
enum {
    MESH_PING_IDLE = 0,
    MESH_PING_HTTP,       // WiFi column: one bounded GET {gw}/ping
    MESH_PING_MESH_TX,    // mesh column: fire one sparse private probe
    MESH_PING_MESH_WAIT   // wait for the pong until the deadline
};
/* ACK often ~0.5–2s on local path; multi-hop a bit more. */
#define MESH_PING_WAIT_MS 2800UL
static int mesh_ping_state = MESH_PING_IDLE;
static unsigned long mesh_ping_deadline_ms = 0;
static uint32_t mesh_ping_ok_before = 0;
static uint32_t mesh_ping_rtt_before = 0;
static bool mesh_ping_tx = false;
static bool mesh_ping_wifi_ok = false;
static char mesh_ping_wifi_s1[24] = "";
static char mesh_ping_wifi_s2[24] = "";
static char mesh_ping_lines[HW_UI_MESH_PING_LINES][48];
static const char *mesh_ping_ptrs[HW_UI_MESH_PING_LINES];
static int mesh_ping_nlines = 0;

static void mesh_ping_push(const char *fmt, ...) {
    if (mesh_ping_nlines >= HW_UI_MESH_PING_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mesh_ping_lines[mesh_ping_nlines], sizeof(mesh_ping_lines[0]),
              fmt, ap);
    va_end(ap);
    mesh_ping_ptrs[mesh_ping_nlines] = mesh_ping_lines[mesh_ping_nlines];
    mesh_ping_nlines++;
}

// Entry (encoder click on PING): paint the start face and arm the sequence.
static void ui_mesh_ping_gateway() {
    mesh_ping_wifi_ok = false;
    mesh_ping_wifi_s1[0] = mesh_ping_wifi_s2[0] = '\0';
    mesh_ping_tx = false;

    // --- phase: start ---
    mesh_ping_nlines = 0;
    mesh_ping_push("probing WiFi + Mesh…");
    mesh_ping_push("gw: %s", mesh_gw_url);
    mesh_ping_push("wifi: %s",
                   WiFi.status() == WL_CONNECTED
                       ? WiFi.localIP().toString().c_str()
                       : "OFFLINE");
    hw_ui_show_mesh_ping("PING…", mesh_ping_ptrs, mesh_ping_nlines);
    ui_note_input();
    mesh_ping_state = MESH_PING_HTTP;
}

// ---- WiFi path: GET gateway /ping (one bounded synchronous GET) ----
static void ui_mesh_ping_step_http() {
    unsigned long wifi_rtt = 0;
    int wifi_rssi = 0;
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1), "offline");
        snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2), "no STA");
    } else {
        wifi_rssi = WiFi.RSSI();
        char url[128];
        unsigned long t0 = millis();
        snprintf(url, sizeof(url), "%s/ping?t=%lu", mesh_gw_url, t0);

        mesh_ping_nlines = 0;
        mesh_ping_push("WiFi GET /ping …");
        mesh_ping_push("%s", mesh_gw_url);
        mesh_ping_push("RSSI %d dBm", wifi_rssi);
        hw_ui_show_mesh_ping("WIFI", mesh_ping_ptrs, mesh_ping_nlines);

        HTTPClient http;
        // A black-holed gateway must cost ~1 s here, not the full read window.
        http.setConnectTimeout(1000);
        http.setTimeout(4000);
        if (http.begin(url)) {
            if (gw_token.length() > 0) {
                http.addHeader("Authorization", String("Bearer ") + gw_token);
            }
            int code = http.GET();
            wifi_rtt = millis() - t0;
            http.end();
            // Any HTTP status proves the link to the gateway is up — a 401/403
            // is the gateway refusing the token, not a dead WiFi path, so it
            // must not paint the WiFi column red.
            if (code > 0) {
                mesh_ping_wifi_ok = true;
                if (code < 400) {
                    snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1),
                             "%d dBm", wifi_rssi);
                    snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2),
                             "%lu ms", wifi_rtt);
                } else if (code == 401 || code == 403) {
                    snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1),
                             "HTTP %d", code);
                    snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2),
                             "no auth");
                } else {
                    snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1),
                             "HTTP %d", code);
                    snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2),
                             "%lu ms", wifi_rtt);
                }
            } else {
                snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1),
                         "no reply");
                snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2),
                         "err %d", code);
            }
        } else {
            snprintf(mesh_ping_wifi_s1, sizeof(mesh_ping_wifi_s1), "begin fail");
            snprintf(mesh_ping_wifi_s2, sizeof(mesh_ping_wifi_s2), "bad URL");
        }
    }
    mesh_ping_state = MESH_PING_MESH_TX;
}

// ---- Mesh path: sparse private probe, wait ACK ----
static void ui_mesh_ping_step_mesh_tx() {
    mesh_ping_nlines = 0;
    mesh_ping_push("Mesh private DM …");
    mesh_ping_push("MC|k → Heltec");
    if (!g_mesh.has_identity) mesh_ping_push("no identity");
    else if (!g_mesh.radio_ready) mesh_ping_push("radio not ready");
    else if (!g_mesh.heltec_pk_hex[0]) mesh_ping_push("no GW pubkey");
    else if (g_mesh_chat_tx.active || mesh_client_ack_pending())
        mesh_ping_push("radio busy - retry");
    else mesh_ping_push("waiting ACK…");
    hw_ui_show_mesh_ping("MESH", mesh_ping_ptrs, mesh_ping_nlines);

    mesh_ping_ok_before = g_mesh.last_ok_ms;
    mesh_ping_rtt_before = g_mesh.last_rtt_ms;
    mesh_ping_tx = false;
    if (g_mesh.has_identity && g_mesh.radio_ready && g_mesh.heltec_pk_hex[0] &&
        !g_mesh_chat_tx.active && !mesh_client_ack_pending()) {
        mesh_ping_tx = mesh_probe_gateway(nullptr);
        // The meshcore skill tick polls the radio every loop pass; this state
        // machine only watches for the pong until the deadline.
        mesh_ping_deadline_ms = millis() + MESH_PING_WAIT_MS;
    } else {
        mesh_ping_deadline_ms = millis();   // nothing in flight: no wait
    }
    mesh_ping_state = MESH_PING_MESH_WAIT;
}

// ---- Mesh wait + verdict: identical classification to the blocking version.
static void ui_mesh_ping_step_wait() {
    bool pong = (g_mesh.last_ok_ms != mesh_ping_ok_before &&
                 g_mesh.last_ok_ms != 0);
    if (!pong && (long)(millis() - mesh_ping_deadline_ms) < 0)
        return;   // keep waiting — one check per loop pass, nothing blocks

    char mesh_s1[24] = "", mesh_s2[24] = "";
    bool mesh_ok = false;

    if (pong) {
        mesh_ok = true;
        uint32_t rtt = g_mesh.last_rtt_ms
                           ? g_mesh.last_rtt_ms
                           : (mesh_ping_rtt_before ? mesh_ping_rtt_before : 0);
        if (rtt)
            snprintf(mesh_s1, sizeof(mesh_s1), "%lu ms", (unsigned long)rtt);
        else
            snprintf(mesh_s1, sizeof(mesh_s1), "ACK");
        snprintf(mesh_s2, sizeof(mesh_s2), "live");
    } else if (mesh_ping_tx) {
        /* TX accepted but no ACK in window — still show last-known if fresh. */
        int age = mesh_alive_age_s();
        if (age >= 0 && age < 120) {
            mesh_ok = true;
            if (g_mesh.last_rtt_ms)
                snprintf(mesh_s1, sizeof(mesh_s1), "%lu ms",
                         (unsigned long)g_mesh.last_rtt_ms);
            else
                snprintf(mesh_s1, sizeof(mesh_s1), "recent");
            snprintf(mesh_s2, sizeof(mesh_s2), "age %ds", age);
        } else {
            snprintf(mesh_s1, sizeof(mesh_s1), "no ACK");
            snprintf(mesh_s2, sizeof(mesh_s2), age >= 0 ? "stale" : "timeout");
        }
    } else if (!g_mesh.has_identity) {
        snprintf(mesh_s1, sizeof(mesh_s1), "no keys");
        snprintf(mesh_s2, sizeof(mesh_s2), "pair first");
    } else if (!g_mesh.radio_ready) {
        snprintf(mesh_s1, sizeof(mesh_s1), "radio");
        snprintf(mesh_s2, sizeof(mesh_s2), "%s", g_mesh.radio_state);
    } else if (!g_mesh.heltec_pk_hex[0]) {
        snprintf(mesh_s1, sizeof(mesh_s1), "no GW");
        snprintf(mesh_s2, sizeof(mesh_s2), "pair meta");
    } else {
        int age = mesh_alive_age_s();
        snprintf(mesh_s1, sizeof(mesh_s1), "TX fail");
        if (age >= 0)
            snprintf(mesh_s2, sizeof(mesh_s2), "age %ds", age);
        else
            snprintf(mesh_s2, sizeof(mesh_s2), "never");
    }

    hw_ui_show_mesh_ping_result(mesh_ping_wifi_ok, mesh_ping_wifi_s1,
                                mesh_ping_wifi_s2,
                                mesh_ok, mesh_s1, mesh_s2);
    if (mesh_ping_wifi_ok || mesh_ok) hw_haptic_notify(0);
    else hw_haptic_notify(1);
    ui_note_input();
    mesh_ping_state = MESH_PING_IDLE;
}

// Loop task: advance the PING sequence. Any exit from the PING face cancels
// the run before it can paint a stale result into another screen.
static void ui_mesh_ping_poll() {
    if (mesh_ping_state == MESH_PING_IDLE) return;
    if (hw_ui_screen() != HW_UI_MESH_PING) {
        mesh_ping_state = MESH_PING_IDLE;
        return;
    }
    switch (mesh_ping_state) {
    case MESH_PING_HTTP:      ui_mesh_ping_step_http(); break;
    case MESH_PING_MESH_TX:   ui_mesh_ping_step_mesh_tx(); break;
    case MESH_PING_MESH_WAIT: ui_mesh_ping_step_wait(); break;
    }
}

static void kb_layout_save() {
    File f = SPIFFS.open(KB_LAYOUT_PATH, "w");
    if (!f) return;
    f.print(hw_kb_layout_id());
    f.close();
}

// UTF-8 helpers for the reply draft
static int utf8_cp_len(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static void reply_buf_backspace() {
    size_t n = strlen(reply_buf);
    if (n == 0) return;
    // Walk codepoints; drop the last one.
    size_t i = 0, prev = 0;
    while (i < n) {
        prev = i;
        int k = utf8_cp_len(reply_buf + i);
        if (k <= 0) break;
        i += (size_t)k;
    }
    reply_buf[prev] = '\0';
}

static bool reply_buf_append(const char *utf8) {
    if (!utf8 || !utf8[0]) return false;
    size_t n = strlen(reply_buf);
    size_t add = strlen(utf8);
    if (n + add + 1 > sizeof(reply_buf)) return false;
    memcpy(reply_buf + n, utf8, add + 1);
    return true;
}

static void ui_reply_paint() {
    hw_ui_show_reply(reply_title, reply_buf, hw_kb_caps(), hw_kb_symbol(),
                     hw_kb_layout_name());
}

// Keyboard shortcut HOME: leave any face, drop drafts, show clock.
static void ui_go_home() {
    notify_card_id = 0;
    reply_buf[0] = '\0';
    agent_compose = false;
    hw_kb_reset_mods();
    ui_go_clock(NULL);
    ui_note_input();
}

static void ui_open_msglist();  // defined below
static void ui_open_notify_id(uint32_t id);
static void ui_open_card_act();
static void ui_card_act_confirm();
static void ui_open_agents();
static void ui_open_agent_sessions(int idx);
static void ui_agent_sessions_refresh();
static void ui_open_agent_chat(int idx);
static void ui_agent_chat_refresh();
static void ui_open_agent_act();
static int agents_index_from_source(const char *src);
static bool notify_is_chat_door(const NotifyView &v);
static int agents_index_from_door(const NotifyView &v);
static void ui_show_agent_invite_from_view(const NotifyView &v, uint32_t id);
static void ui_enter_agent_from_notify(uint32_t id, const NotifyView &v);

// first_utf8: optional first character (UTF-8); NULL/empty = empty draft.
static void ui_open_reply(uint32_t id, const char *title, const char *first_utf8) {
    agent_compose = false;
    wifi_compose = false;
    notify_card_id = id;
    snprintf(reply_title, sizeof(reply_title), "%s", title ? title : "");
    reply_buf[0] = '\0';
    // Clear CAPS/SYM latch; layout stays (user's Settings choice).
    hw_kb_reset_mods();
    if (first_utf8 && first_utf8[0] &&
        first_utf8[0] != '\b' && first_utf8[0] != '\n' && first_utf8[0] != '\x1b') {
        reply_buf_append(first_utf8);
    }
    ui_reply_paint();
    ui_note_input();
}

static void ui_open_agent_compose(const char *first_utf8) {
    agent_compose = true;
    wifi_compose = false;
    notify_card_id = 0;
    snprintf(reply_title, sizeof(reply_title), "-> %s", agents_name(agent_focus));
    reply_buf[0] = '\0';
    hw_kb_reset_mods();
    if (first_utf8 && first_utf8[0] &&
        first_utf8[0] != '\b' && first_utf8[0] != '\n' && first_utf8[0] != '\x1b') {
        reply_buf_append(first_utf8);
    }
    ui_reply_paint();
    ui_note_input();
}

static void ui_reply_redraw() {
    ui_reply_paint();
    ui_note_input();
}

static bool utf8_is_printable(const char *u) {
    if (!u || !u[0]) return false;
    unsigned char c = (unsigned char)u[0];
    if (c == '\b' || c == '\n' || c == '\x1b') return false;
    if (c >= 0x20 && c < 0x7F) return true;   // ASCII printable
    if (c >= 0xC0) return true;                 // UTF-8 lead (Cyrillic etc.)
    return false;
}

static void ui_reply_submit() {
    if (!reply_buf[0]) return;
    if (wifi_compose) {
        /* Save password for pending SSID and connect. */
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", wifi_pending_ssid);
        wifi_nets_upsert(ssid, reply_buf);
        wifi_persist_profiles();
        reply_buf[0] = '\0';
        wifi_compose = false;
        wifi_pending_ssid[0] = '\0';
        hw_haptic_notify(0);
        ui_wifi_connect_ssid(ssid);
        return;
    }
    if (agent_compose) {
        const char *aid = agents_id(agent_focus);
        if (agents_send(aid, reply_buf)) {
            hw_haptic_notify(0);
            hw_sound_notify(0);
        }
        reply_buf[0] = '\0';
        agent_compose = false;
        ui_open_agent_chat(agent_focus);
        return;
    }
    if (!notify_card_id) return;
    if (notify_set_reply(notify_card_id, reply_buf)) {
        hw_haptic_notify(0);
        hw_sound_notify(0);
        event_add("reply id=%lu: %s", (unsigned long)notify_card_id, reply_buf);
        // Read the card back rather than sending reply_buf: the store cleaned
        // and truncated it, and what goes upstream must be what GET /notify/one
        // serves. A card with no client id has nowhere to go — that is the whole
        // condition, and it leaves the reply stored exactly as before.
        NotifyView v;
        if (notify_view_by_id(notify_card_id, v, NULL, NULL) && v.key[0])
            reply_upstream_queue(v.key, v.reply);
    }
    notify_card_id = 0;
    reply_buf[0] = '\0';
    ui_go_clock("sent");
}

static void ui_on_key(const char *u) {
    if (!u || !u[0]) return;
    ui_note_input();
    HwUiScreen scr = hw_ui_screen();
    char c0 = u[0];

    // ALT (orange) + Backspace → home clock from any screen.
    if (c0 == '\x1b') {
        ui_go_home();
        hw_haptic_notify(0);
        return;
    }

    // From a card: typing jumps straight into reply.
    // Enter: agent door → open chat; else Ack·Reply sheet.
    if (scr == HW_UI_NOTIFY && notify_card_id) {
        if (c0 == '\b') return;
        if (c0 == '\n') {
            NotifyView v;
            if (notify_view_by_id(notify_card_id, v, NULL, NULL) &&
                notify_is_chat_door(v)) {
                ui_enter_agent_from_notify(notify_card_id, v);
            } else {
                ui_open_card_act();
            }
            return;
        }
        NotifyView v;
        if (notify_view_by_id(notify_card_id, v, NULL, NULL)) {
            if (notify_is_chat_door(v)) {
                ui_enter_agent_from_notify(notify_card_id, v);
                if (utf8_is_printable(u)) ui_open_agent_compose(u);
                return;
            }
        }
        if (utf8_is_printable(u)) {
            ui_open_reply(notify_card_id, reply_title[0] ? reply_title : NULL, u);
            if (!reply_title[0]) {
                NotifyView vv;
                if (notify_view_by_id(notify_card_id, vv, NULL, NULL))
                    snprintf(reply_title, sizeof(reply_title), "%s", vv.title);
                ui_reply_redraw();
            }
            return;
        }
    }

    // On Ack/Reply sheet: Enter confirms; typing still starts a reply.
    if (scr == HW_UI_CARD_ACT && notify_card_id) {
        if (c0 == '\n') {
            ui_card_act_confirm();
            return;
        }
        if (utf8_is_printable(u)) {
            ui_open_reply(notify_card_id, reply_title[0] ? reply_title : NULL, u);
            return;
        }
    }

    if (scr == HW_UI_REPLY) {
        if (c0 == '\n') {
            ui_reply_submit();
            return;
        }
        if (c0 == '\b') {
            reply_buf_backspace();
            ui_reply_redraw();
            return;
        }
        if (utf8_is_printable(u)) {
            if (reply_buf_append(u)) ui_reply_redraw();
            return;
        }
    }

    // Agent chat: type = compose to that agent; Enter = empty compose
    if (scr == HW_UI_AGENT_CHAT) {
        if (c0 == '\n') {
            ui_open_agent_compose(NULL);
            return;
        }
        if (utf8_is_printable(u)) {
            ui_open_agent_compose(u);
            return;
        }
    }

    // Clock / menu: 'm' opens messages, bare keys ignored
    if (scr == HW_UI_CLOCK && (c0 == 'm' || c0 == 'M')) {
        msglist_sel = 0;
        ui_open_msglist();
    }
}

static void ui_open_menu() {
    menu_sel = 0;
    hw_ui_show_menu(menu_sel);
    ui_note_input();
}

static void ui_open_agents() {
    agents_sel = 0;
    hw_ui_show_agents(agents_sel, agents_bridge_ok());
    ui_note_input();
}

static int agents_index_from_source(const char *src) {
    if (!src || !src[0]) return -1;
    // notify source is short: "hermes", "grok", "claude", "opencode", "codex"
    return agents_find(src);
}

/* Chat-door vs real message:
 *   door  = client id ends with "-chat" (bridge posts "hermes-chat",
 *           "opencode-chat", "codex-chat" etc.)
 *   page  = everything else — full severity card (info/warn/crit colours),
 *           from any service/agent; reply works as before.
 * Chat rooms (AGENTS menu) are separate from the message queue. */
static bool notify_is_chat_door(const NotifyView &v) {
    if (!v.key[0]) return false;
    size_t n = strlen(v.key);
    return n >= 5 && strcmp(v.key + (n - 5), "-chat") == 0;
}

static int agents_index_from_door(const NotifyView &v) {
    if (!notify_is_chat_door(v)) return -1;
    // Prefer source; fall back to key prefix "hermes-chat" → "hermes",
    // "opencode-chat" → "opencode", "codex-chat" → "codex"
    int ax = agents_index_from_source(v.source);
    if (ax >= 0) return ax;
    char id[AGENT_ID_LEN];
    size_t n = strlen(v.key);
    if (n <= 5) return -1;
    size_t m = n - 5;
    if (m >= sizeof(id)) m = sizeof(id) - 1;
    memcpy(id, v.key, m);
    id[m] = '\0';
    return agents_find(id);
}

static void agents_head_time(uint32_t ts, char *out, size_t out_n) {
    out[0] = '\0';
    if (!out || out_n < 4 || ts <= 1700000000u) return;  // no wall clock yet
    time_t t = (time_t)ts;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return;
    strftime(out, out_n, "%H:%M", &tmv);
}

static void ui_agent_chat_refresh() {
    int n = 0;
    agents_lock();
    n = agents_thread_view(agent_focus, ag_chat_lines, ag_chat_from_me,
                           ag_chat_ts, AGENT_THREAD_MAX);
    agents_unlock();
    for (int i = 0; i < n; i++) ag_chat_ptrs[i] = ag_chat_lines[i];

    // Header: agent · active session (recency/identity at a glance).
    char head[32];
    const char *sess = agents_active_session(agent_focus);
    if (sess && sess[0])
        snprintf(head, sizeof(head), "%s · %s", agents_name(agent_focus), sess);
    else
        snprintf(head, sizeof(head), "%s", agents_name(agent_focus));

    // Footer: local time of the bottommost message + scroll hint.
    char foot[56];
    char tbuf[8];
    tbuf[0] = '\0';
    if (n > 0) agents_head_time(ag_chat_ts[n - 1], tbuf, sizeof(tbuf));
    if (tbuf[0])
        snprintf(foot, sizeof(foot), "%s  wheel=scroll  type=msg", tbuf);
    else
        snprintf(foot, sizeof(foot), "wheel=scroll  type=msg");

    int total = 0;
    agent_scroll = hw_ui_show_agent_chat(head, ag_chat_ptrs, ag_chat_from_me,
                                         n, agent_scroll, &total, foot);
    agent_scroll_total = total;
}

static void ui_open_agent_chat(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= agents_count()) idx = agents_count() - 1;
    agent_focus = idx;
    agent_compose = false;
    agent_scroll = -1;  // pin to latest when entering the room
    agents_thread_goto_tail(idx);  // load the session's history from the store
    ui_agent_chat_refresh();
}

/* Sessions screen for the focused agent: existing sessions (with honest
 * message counts) + "NEW SESSION" + "BACK". Counts are refreshed on open.
 * When the session registry is full the NEW row turns into a visible
 * "MAX n SESSIONS" note and is not selectable — no silent refusal. */
static void ui_agent_sessions_refresh() {
    int n = agents_session_count(agent_focus);
    bool full = (n >= AGENT_SESSIONS_MAX);
    int total = n + 2;
    for (int i = 0; i < n; i++) {
        snprintf(ag_sess_titles[i], sizeof(ag_sess_titles[0]), "%s",
                 agents_session_name(agent_focus, i));
        ag_sess_msgs[i] = agents_session_msg_count(agent_focus, i);
        ag_sess_active[i] = agents_session_is_active(agent_focus, i);
        ag_sess_ptrs[i] = ag_sess_titles[i];
    }
    if (full)
        snprintf(ag_sess_titles[n], sizeof(ag_sess_titles[0]),
                 "MAX %d SESSIONS", AGENT_SESSIONS_MAX);
    else
        snprintf(ag_sess_titles[n], sizeof(ag_sess_titles[0]), "NEW SESSION");
    ag_sess_msgs[n] = -1; ag_sess_active[n] = false;
    ag_sess_ptrs[n] = ag_sess_titles[n];
    snprintf(ag_sess_titles[n + 1], sizeof(ag_sess_titles[0]), "BACK");
    ag_sess_msgs[n + 1] = -1; ag_sess_active[n + 1] = false;
    ag_sess_ptrs[n + 1] = ag_sess_titles[n + 1];

    if (agent_sess_sel >= total) agent_sess_sel = total - 1;
    if (agent_sess_sel < 0) agent_sess_sel = 0;
    if (full && agent_sess_sel == n)
        agent_sess_sel = n + 1;   // never land on the disabled NEW row
    hw_ui_show_agent_sessions(agents_name(agent_focus), ag_sess_ptrs,
                              ag_sess_msgs, ag_sess_active, total,
                              agent_sess_sel);
    ui_note_input();
}

static void ui_open_agent_sessions(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= agents_count()) idx = agents_count() - 1;
    agent_focus = idx;
    agents_session_refresh_counts(idx);
    agent_sess_sel = 0;
    for (int i = 0; i < agents_session_count(idx); i++)
        if (agents_session_is_active(idx, i)) { agent_sess_sel = i; break; }
    ui_agent_sessions_refresh();
}

static void ui_open_agent_act() {
    agent_act_sel = AGENT_ACT_CLEAR;
    hw_ui_show_agent_act(agent_act_sel, agents_name(agent_focus));
    ui_note_input();
}

static void ui_agent_act_confirm() {
    if (agent_act_sel == AGENT_ACT_CLEAR) {
        agents_clear(agents_id(agent_focus));
        hw_haptic_notify(0);
        ui_open_agent_chat(agent_focus);
    } else {
        ui_open_agents();
    }
}

static void ui_open_info() {
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", SEED_VERSION);
    String ip = (WiFi.status() == WL_CONNECTED)
        ? WiFi.localIP().toString() : String("");
    hw_ui_show_info(ver, mdns_name.c_str(), ip.c_str(),
                    auth_token.c_str(), ESP.getFreeHeap(),
                    notify_unread_count());
    ui_note_input();
}

// Build msglist from newest-first notify views (up to HW_UI_MSGLIST_MAX).
// Unread flags drive the Nokia-style * NEW markers on the list face.
static void ui_open_msglist() {
    static char titles[HW_UI_MSGLIST_MAX][41];
    static char levels[HW_UI_MSGLIST_MAX][8];
    static bool unread[HW_UI_MSGLIST_MAX];
    static const char *title_ptrs[HW_UI_MSGLIST_MAX];
    static const char *level_ptrs[HW_UI_MSGLIST_MAX];

    msglist_count = 0;
    for (int i = 0; i < NOTIFY_MAX && msglist_count < HW_UI_MSGLIST_MAX; i++) {
        NotifyView v;
        if (!notify_view(i, v)) break;
        msglist_ids[msglist_count] = v.id;
        snprintf(titles[msglist_count], sizeof(titles[0]), "%s", v.title);
        snprintf(levels[msglist_count], sizeof(levels[0]), "%s",
                 notify_level_name(v.level));
        unread[msglist_count] = v.unread;
        title_ptrs[msglist_count] = titles[msglist_count];
        level_ptrs[msglist_count] = levels[msglist_count];
        msglist_count++;
    }
    if (msglist_sel >= msglist_count) msglist_sel = msglist_count > 0 ? msglist_count - 1 : 0;
    if (msglist_sel < 0) msglist_sel = 0;
    hw_ui_show_msglist(title_ptrs, level_ptrs, unread, msglist_count, msglist_sel);
    ui_note_input();
}

// Show the agent "door" card (pretty invite). Does not enter the room yet.
static void ui_show_agent_invite_from_view(const NotifyView &v, uint32_t id) {
    notify_card_id = id;
    snprintf(reply_title, sizeof(reply_title), "%s", v.title);
    int ax = agents_index_from_door(v);
    const char *aname = (ax >= 0) ? agents_name(ax) : v.source;
    hw_ui_show_agent_invite(aname, v.body, notify_unread_count());
    // Reachable from the loop() arrival path: a system wake, not user input.
    ui_note_wake();
}

// Enter the chat room from a chat-door notify (ack + open).
static void ui_enter_agent_from_notify(uint32_t id, const NotifyView &v) {
    int ax = agents_index_from_door(v);
    if (ax < 0) return;
    notify_ack_id(id);
    notify_card_id = 0;
    ui_open_agent_chat(ax);
}

static void ui_open_notify_id(uint32_t id) {
    NotifyView v;
    if (!notify_view_by_id(id, v, NULL, NULL)) {
        ui_open_msglist();
        return;
    }
    // Only *-chat doors open the invite; real pages keep severity colours.
    if (notify_is_chat_door(v)) {
        ui_show_agent_invite_from_view(v, id);
        return;
    }
    notify_card_id = id;
    snprintf(reply_title, sizeof(reply_title), "%s", v.title);
    hw_ui_show_notify(notify_level_name(v.level), v.source, v.title,
                      v.body, notify_unread_count());
    ui_note_input();
}

static void ui_open_card_act() {
    if (!notify_card_id) return;
    card_act_sel = CARD_ACT_ACK;  // default: mark read
    hw_ui_show_card_act(card_act_sel,
                        reply_title[0] ? reply_title : NULL);
    ui_note_input();
}

static void ui_card_act_confirm() {
    if (!notify_card_id) {
        ui_go_clock(NULL);
        return;
    }
    if (card_act_sel == CARD_ACT_ACK) {
        notify_ack_id(notify_card_id);
        notify_card_id = 0;
        if (notify_unread_count() > 0) ui_open_msglist();
        else ui_go_clock("acked");
    } else if (card_act_sel == CARD_ACT_REPLY) {
        ui_open_reply(notify_card_id,
                      reply_title[0] ? reply_title : NULL, NULL);
    } else {
        // BACK → re-show the card
        ui_open_notify_id(notify_card_id);
    }
}

// Handle one click depending on the current face.
static void ui_on_click() {
    // Blanked panel: first click only wakes (does not open menu).
    if (backlight_blanked()) {
        ui_note_input();
        return;
    }
    ui_note_input();
    switch (hw_ui_screen()) {
    case HW_UI_CLOCK:
        ui_open_menu();
        break;
    case HW_UI_MENU:
        if (menu_sel == MENU_MESSAGES) {
            msglist_sel = 0;
            ui_open_msglist();
        } else if (menu_sel == MENU_AGENTS) {
            ui_open_agents();
        } else if (menu_sel == MENU_MESHCORE) {
            ui_open_meshcore();
        } else if (menu_sel == MENU_WIFI) {
            ui_open_wifi();
        } else if (menu_sel == MENU_SETTINGS) {
            settings_sel = 0;
            ui_open_settings();
        } else if (menu_sel == MENU_INFO) {
            ui_open_info();
        } else {
            ui_go_clock(NULL);
        }
        break;
    case HW_UI_WIFI:
        if (wifi_sel == WIFI_ACT_BACK) {
            ui_open_menu();
        } else if (wifi_sel == WIFI_ACT_STATUS) {
            ui_wifi_show_status();
        } else if (wifi_sel == WIFI_ACT_SCAN) {
            ui_wifi_do_scan();
        } else if (wifi_sel == WIFI_ACT_PROFILES) {
            ui_wifi_show_profiles();
        } else if (wifi_sel == WIFI_ACT_TOGGLE) {
            ui_wifi_toggle();
        }
        break;
    case HW_UI_WIFI_LIST:
        if (wifi_list_count == 0) {
            ui_open_wifi();
        } else {
            ui_wifi_connect_ssid(wifi_list_ssids[wifi_list_sel]);
        }
        break;
    case HW_UI_WIFI_INFO:
        ui_open_wifi();
        break;
    case HW_UI_SETTINGS:
        if (settings_sel == SETTINGS_BACK) {
            ui_open_menu();
        } else if (settings_sel == SETTINGS_LAYOUT) {
            layout_sel = (int)hw_kb_layout();
            hw_ui_show_layout(layout_sel, (int)hw_kb_layout());
            ui_note_input();
        } else if (settings_sel == SETTINGS_BACKLIGHT) {
            backlight_menu_click();
            ui_open_settings();
        } else if (settings_sel == SETTINGS_AUTODIM) {
            backlight_idle_toggle();
            ui_open_settings();
        }
        break;
    case HW_UI_MESHCORE:
        if (mesh_sel == MESH_ACT_BACK) {
            ui_open_menu();
        } else if (mesh_sel == MESH_ACT_STATUS) {
            ui_mesh_show_status();
        } else {
            ui_mesh_ping_gateway();
        }
        break;
    case HW_UI_MESH_PING:
        // any click leaves the result
        ui_open_meshcore();
        break;
    case HW_UI_LAYOUT:
        if (layout_sel == LAYOUT_BACK) {
            ui_open_settings();
        } else {
            hw_kb_set_layout((HwKbLayout)layout_sel);
            kb_layout_save();
            hw_kb_take_layout_changed();
            hw_haptic_notify(0);
            // Re-draw so the * marker jumps to the new default.
            hw_ui_show_layout(layout_sel, (int)hw_kb_layout());
        }
        break;
    case HW_UI_AGENTS:
        if (agents_sel == AGENTS_BACK) {
            ui_open_menu();
        } else {
            ui_open_agent_sessions(agents_sel);
        }
        break;
    case HW_UI_AGENT_SESSIONS: {
        int n = agents_session_count(agent_focus);
        if (agent_sess_sel < n) {  // existing session → open its chat
            const char *nm = agents_session_name(agent_focus, agent_sess_sel);
            if (nm && nm[0] && agents_session_select(agent_focus, nm)) {
                hw_haptic_notify(0);
                ui_open_agent_chat(agent_focus);
            }
        } else if (agent_sess_sel == n) {  // NEW SESSION
            if (n >= AGENT_SESSIONS_MAX) {
                break;   // row is disabled (shown as "MAX n SESSIONS")
            }
            bool created = false;
            int r = agents_session_create(agent_focus, nullptr, created);
            if (r >= 0) {
                agents_push_line(agent_focus, false, "new session - type to talk");
                hw_haptic_notify(0);
                ui_open_agent_chat(agent_focus);
            } else {
                event_add("agent new session refused (full)");
            }
        } else {  // BACK
            ui_open_agents();
        }
        break;
    }
    case HW_UI_AGENT_CHAT:
        // Click → CLEAR CHAT / BACK sheet (chat room, not notify card)
        ui_open_agent_act();
        break;
    case HW_UI_AGENT_ACT:
        ui_agent_act_confirm();
        break;
    case HW_UI_MSGLIST:
        if (msglist_count == 0) {
            ui_open_menu();
        } else {
            ui_open_notify_id(msglist_ids[msglist_sel]);
        }
        break;
    case HW_UI_NOTIFY: {
        // Chat door → open room. Severity page → Ack / Reply / Back.
        NotifyView v;
        if (notify_card_id && notify_view_by_id(notify_card_id, v, NULL, NULL) &&
            notify_is_chat_door(v)) {
            ui_enter_agent_from_notify(notify_card_id, v);
        } else {
            ui_open_card_act();
        }
        break;
    }
    case HW_UI_CARD_ACT:
        ui_card_act_confirm();
        break;
    case HW_UI_INFO:
        ui_open_menu();
        break;
    case HW_UI_REPLY:
        // Encoder click = send if draft non-empty, else cancel
        if (reply_buf[0]) {
            ui_reply_submit();
        } else if (wifi_compose) {
            wifi_compose = false;
            wifi_pending_ssid[0] = '\0';
            reply_buf[0] = '\0';
            ui_open_wifi();
        } else if (agent_compose) {
            agent_compose = false;
            reply_buf[0] = '\0';
            ui_open_agent_chat(agent_focus);
        } else {
            notify_card_id = 0;
            reply_buf[0] = '\0';
            ui_go_clock(NULL);
        }
        break;
    }
}

// Handle encoder detents.
static void ui_on_steps(int steps) {
    if (steps == 0) return;
    // Blanked: detents only wake (no menu navigation under the dark).
    if (backlight_blanked()) {
        ui_note_input();
        return;
    }
    ui_note_input();
    switch (hw_ui_screen()) {
    case HW_UI_MENU: {
        menu_sel += steps;
        while (menu_sel < 0) menu_sel += MENU_COUNT;
        while (menu_sel >= MENU_COUNT) menu_sel -= MENU_COUNT;
        hw_ui_show_menu(menu_sel);
        break;
    }
    case HW_UI_SETTINGS: {
        settings_sel += steps;
        while (settings_sel < 0) settings_sel += SETTINGS_LIST_COUNT;
        while (settings_sel >= SETTINGS_LIST_COUNT) settings_sel -= SETTINGS_LIST_COUNT;
        ui_open_settings();
        break;
    }
    case HW_UI_CARD_ACT: {
        card_act_sel += steps;
        while (card_act_sel < 0) card_act_sel += CARD_ACT_COUNT;
        while (card_act_sel >= CARD_ACT_COUNT) card_act_sel -= CARD_ACT_COUNT;
        hw_ui_show_card_act(card_act_sel,
                            reply_title[0] ? reply_title : NULL);
        break;
    }
    case HW_UI_AGENTS: {
        agents_sel += steps;
        while (agents_sel < 0) agents_sel += AGENTS_LIST_COUNT;
        while (agents_sel >= AGENTS_LIST_COUNT) agents_sel -= AGENTS_LIST_COUNT;
        hw_ui_show_agents(agents_sel, agents_bridge_ok());
        break;
    }
    case HW_UI_AGENT_SESSIONS: {
        int n = agents_session_count(agent_focus);
        int cnt = n + 2;
        if (cnt <= 0) break;
        agent_sess_sel += steps;
        while (agent_sess_sel < 0) agent_sess_sel += cnt;
        while (agent_sess_sel >= cnt) agent_sess_sel -= cnt;
        if (n >= AGENT_SESSIONS_MAX && agent_sess_sel == n) {
            // full registry — NEW row disabled: skip over it
            agent_sess_sel += (steps > 0) ? 1 : -1;
            if (agent_sess_sel < 0) agent_sess_sel = cnt - 1;
            if (agent_sess_sel >= cnt) agent_sess_sel = 0;
        }
        ui_agent_sessions_refresh();
        break;
    }
    case HW_UI_AGENT_ACT: {
        agent_act_sel += steps;
        while (agent_act_sel < 0) agent_act_sel += AGENT_ACT_COUNT;
        while (agent_act_sel >= AGENT_ACT_COUNT) agent_act_sel -= AGENT_ACT_COUNT;
        hw_ui_show_agent_act(agent_act_sel, agents_name(agent_focus));
        break;
    }
    case HW_UI_LAYOUT: {
        layout_sel += steps;
        while (layout_sel < 0) layout_sel += LAYOUT_LIST_COUNT;
        while (layout_sel >= LAYOUT_LIST_COUNT) layout_sel -= LAYOUT_LIST_COUNT;
        hw_ui_show_layout(layout_sel, (int)hw_kb_layout());
        break;
    }
    case HW_UI_WIFI: {
        wifi_sel += steps;
        while (wifi_sel < 0) wifi_sel += WIFI_LIST_COUNT;
        while (wifi_sel >= WIFI_LIST_COUNT) wifi_sel -= WIFI_LIST_COUNT;
        hw_ui_show_wifi(wifi_sel);
        break;
    }
    case HW_UI_WIFI_LIST: {
        if (wifi_list_count <= 0) break;
        wifi_list_sel += steps;
        while (wifi_list_sel < 0) wifi_list_sel += wifi_list_count;
        while (wifi_list_sel >= wifi_list_count) wifi_list_sel -= wifi_list_count;
        ui_wifi_paint_list();
        break;
    }
    case HW_UI_MESHCORE: {
        mesh_sel += steps;
        while (mesh_sel < 0) mesh_sel += MESH_LIST_COUNT;
        while (mesh_sel >= MESH_LIST_COUNT) mesh_sel -= MESH_LIST_COUNT;
        hw_ui_show_meshcore(mesh_sel);
        break;
    }
    case HW_UI_MSGLIST:
        if (msglist_count <= 0) break;
        msglist_sel += steps;
        if (msglist_sel < 0) msglist_sel = 0;
        if (msglist_sel >= msglist_count) msglist_sel = msglist_count - 1;
        ui_open_msglist();
        break;
    case HW_UI_NOTIFY: {
        // Scroll the queue: next/prev message by position in newest-first list.
        if (!notify_card_id) break;
        int idx = notify_index_of(notify_card_id);
        if (idx < 0) break;
        int n = notify_count();
        int next = idx + steps;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
        NotifyView v;
        if (notify_view(next, v)) ui_open_notify_id(v.id);
        break;
    }
    case HW_UI_AGENT_CHAT: {
        // Wheel scrolls the wrapped transcript. steps>0 = newer (down the
        // page, scroll decreases); steps<0 = older (scroll increases).
        // The RAM window holds the newest messages; scrolling past its top
        // loads the previous page from the store (long-history browsing).
        if (agent_scroll < 0) {
            // pin-bottom (-1) first materializes to the latest viewport
            agent_scroll = agent_scroll_total;
        }
        if (steps > 0) {  // toward newer
            int ns = agent_scroll - steps;
            if (ns < 0) {
                if (!agents_thread_is_tail(agent_focus)) {
                    agents_thread_goto_tail(agent_focus);  // back to live chat
                    agent_scroll = -1;
                    ui_agent_chat_refresh();
                    break;
                }
                ns = 0;
            }
            agent_scroll = ns;
        } else {          // toward older
            int ns = agent_scroll + (-steps);
            if (ns > agent_scroll_total) {
                uint32_t start = agents_thread_start(agent_focus);
                if (start > 0) {
                    agents_thread_goto_page(agent_focus, start);  // older page
                    agent_scroll = -1;  // pin to bottom of the loaded page
                    ui_agent_chat_refresh();
                    break;
                }
                ns = agent_scroll_total;
            }
            agent_scroll = ns;
        }
        ui_agent_chat_refresh();
        break;
    }
    case HW_UI_CLOCK:
    case HW_UI_INFO:
    case HW_UI_REPLY:
        break;
    }
}

// ===== Routes =====

static void setup_routes() {
    server.on("/health", HTTP_GET, handle_health);
    server.on("/capabilities", HTTP_GET, handle_capabilities);
    server.on("/config.md", HTTP_GET, handle_config_get);
    server.on("/config.md", HTTP_POST, handle_config_post, NULL, handle_body_collect);
    server.on("/events", HTTP_GET, handle_events);
    server.on("/clock", HTTP_GET, handle_clock_get);
    server.on("/clock/tz", HTTP_POST, handle_clock_tz, NULL, handle_body_collect);
    server.on("/firmware/version", HTTP_GET, handle_firmware_version);
    server.on("/firmware/upload", HTTP_POST, handle_firmware_upload, NULL, handle_firmware_upload_body);
    server.on("/firmware/apply", HTTP_POST, handle_firmware_apply);
    server.on("/firmware/confirm", HTTP_POST, handle_firmware_confirm);
    server.on("/firmware/rollback", HTTP_POST, handle_firmware_rollback);
    server.on("/skill", HTTP_GET, handle_skill);
    server.on("/", HTTP_GET, handle_wifi_page);
    server.on("/wifi/config", HTTP_POST, handle_wifi_post);
    server.on("/wifi/status", HTTP_GET, handle_wifi_status);
    server.on("/wifi/scan", HTTP_GET, handle_wifi_scan);
    server.on("/wifi/networks", HTTP_POST, handle_wifi_networks_post, NULL,
              handle_body_collect);

    // Provision the gateway capability token remotely. Raw body or JSON
    // {"token":"…"}; an empty token clears the file and returns the gateway
    // calls to their legacy header-less form.
    server.on(AsyncURIMatcher::exact("/gw/token"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        String tok = body ? String(body) : String("");
        free(body);
        tok.trim();
        if (tok.startsWith("{")) {
            JsonDocument input;
            if (deserializeJson(input, tok) != DeserializationError::Ok) {
                notify_send_error(req, 400, "invalid JSON"); return;
            }
            tok = input["token"] | "";
            tok.trim();
        }
        if (tok.length() > GW_TOKEN_MAX) {
            notify_send_error(req, 400, "token too long"); return;
        }
        if (tok.length() == 0) {
            SPIFFS.remove(GW_TOKEN_PATH);
        } else if (!write_spiffs_file_atomic(GW_TOKEN_PATH, GW_TOKEN_TMP, tok)) {
            notify_send_error(req, 500, "failed to save token"); return;
        }
        gw_token = tok;
        event_add("gateway token %s", gw_token.length() ? "set" : "cleared");
        JsonDocument doc;
        doc["ok"] = true;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    // Register skill routes (notify uses AsyncURIMatcher::exact)
    for (int i = 0; i < g_skill_count; i++) {
        g_skills[i]->register_routes(server);
    }
}

// ===== Main =====

void setup() {
    // No power-hold line is driven here. The board runs from USB during bring-up.
    Serial.begin(115200);
    delay(500);
    boot_time = millis();
    setCpuFrequencyMhz(CPU_MHZ);

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    // Panel + I2C first so the face can say "connecting..." while STA associates.
    hw_ui_begin();
    // Reclaim AW9364 from the boot pulse in hw_ui; load /backlight.json.
    backlight_begin();
    hw_input_begin();
    hw_haptic_begin();  // after Wire is up (hw_ui_begin)
    hw_sound_begin();
    hw_kb_begin();
    kb_layout_load(); // SPIFFS /kb_layout.txt → EN / RU PHON / RU
    mesh_gw_load();   // SPIFFS /mesh_gw.txt → home Heltec daemon URL
    gw_token_load();  // SPIFFS /gw_token.txt → gateway capability token
    hw_probe();
    tz_load();        // before wifi_setup(): configTzTime() needs the TZ string
    wifi_setup();     // non-blocking STA attempt; never raises an AP
    token_load();     // after wifi_setup(): needs RF up for a real hardware RNG
    skills_init();    // notify store load + route registration data
    ui_go_clock(WiFi.status() == WL_CONNECTED ? "ready" : "click = menu");
    ui_note_input();
    setup_routes();
    server.begin();

    Serial.printf("\nESP32 Seed v%s (T-Lora Pager) cpu=%u MHz\n",
                  SEED_VERSION, (unsigned)ESP.getCpuFreqMHz());
    // Token stays off Serial; it is on the panel now.
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("http://%s:%d/health\n", WiFi.localIP().toString().c_str(), HTTP_PORT);

    event_add("seed started v%s", SEED_VERSION);
}

void loop() {
    // Deferred restart
    static unsigned long restart_at = 0;
    if (pending_restart && restart_at == 0) restart_at = millis();
    if (pending_restart && restart_at > 0 && millis() - restart_at > 1000) {
        if (pending_rollback) esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP.restart();
    }

    // Auto-confirm after 60s of healthy runtime. Confirmation is local flash
    // metadata and must not depend on infrastructure Wi-Fi; otherwise a valid
    // mesh-only boot can roll back merely because the router is absent.
    if (!firmware_confirmed && !firmware_confirm_attempted &&
        (millis() - boot_time) > 60000) {
        firmware_confirm_attempted = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        firmware_confirmed = true;
        if (err == ESP_OK) event_add("firmware auto-confirmed");
    }

    // Deferred STA sequence raised by HTTP handlers (mode → disconnect →
    // begin, millis() settle gaps) — the AsyncTCP task never touches WiFi.
    wifi_reconnect_poll();

    // WiFi reconnect — one non-blocking attempt every 20 minutes while offline.
    // Skipped while the user turned WiFi off from the menu (mesh-only test):
    // they toggled it off, loop must not undo their choice.
    static bool was_connected = false;
    bool now_connected = WiFi.status() == WL_CONNECTED;
    if (!wifi_user_off && (wifi_net_count > 0 || wifi_ssid.length() > 0) &&
        !now_connected && millis() - wifi_last_attempt_ms >= WIFI_RETRY_MS) {
        /* One profile attempt (~5s) so loop stays responsive. */
        if (wifi_net_count > 1) {
            wifi_net_idx = (wifi_net_idx + 1) % wifi_net_count;
            wifi_nets_set_active(wifi_net_idx);
        }
        Serial.printf("[wifi] reconnect try %s\n", wifi_ssid.c_str());
        WiFi.disconnect(false, false);
        delay(50);
        wifi_begin_active_profile();
    }
    if (now_connected != was_connected) {
        was_connected = now_connected;
        if (now_connected) {
            wifi_persist_profiles();
            if (!mdns_started && MDNS.begin(mdns_name.c_str())) {
                MDNS.addService("http", "tcp", HTTP_PORT);
                MDNS.addService("seed", "tcp", HTTP_PORT);
                mdns_started = true;
            }
        } else if (mdns_started) {
            MDNS.end();
            mdns_started = false;
        }
        if (hw_ui_screen() == HW_UI_CLOCK) {
            hw_ui_invalidate_clock();
            ui_clock_paint(now_connected ? "wifi up" : "wifi lost");
        }
    }

    // Front panel: encoder + click (must run before screens consume edges).
    // Pocket lock (long CAPS): swallow wheel + click — keys already silent in
    // hw_kb. Still poll+drain so detents don't queue and fire after unlock.
    hw_input_poll();
    int steps = hw_input_steps();
    bool click = hw_input_click();
    if (!hw_kb_locked()) {
        if (steps) ui_on_steps(steps);
        if (click) ui_on_click();
    }

    // Keyboard → UTF-8 into reply / open compose from card.
    // ALT+CAPS cycles layout; long CAPS = full lock (kb + wheel). Refresh badges.
    // Any key also wakes a blanked panel (stamps idle clock).
    {
        char u8[5];
        while (hw_kb_read(u8, sizeof(u8))) {
            if (backlight_blanked()) {
                ui_note_input();
                continue;  // first key on dark: wake only
            }
            ui_on_key(u8);
        }
        if (hw_kb_take_lock_changed()) {
            hw_haptic_notify(hw_kb_locked() ? 1 : 0);
            if (hw_kb_locked()) hw_sound_notify(0);
            if (hw_ui_screen() == HW_UI_CLOCK) {
                hw_ui_invalidate_clock();
                ui_clock_paint(hw_kb_locked() ? "LOCKED" : "unlocked");
            }
            ui_note_input();
        }
        if (hw_kb_take_layout_changed()) {
            kb_layout_save();
            hw_haptic_notify(0);
            if (hw_ui_screen() == HW_UI_REPLY) ui_reply_paint();
            else if (hw_ui_screen() == HW_UI_LAYOUT)
                hw_ui_show_layout(layout_sel, (int)hw_kb_layout());
        }
    }

    // Idle policy owns brightness; drive pulses after the decision.
    ui_backlight_idle();
    backlight_poll();

    // Idle → clock (longer while typing a reply).
    {
        unsigned long idle = (hw_ui_screen() == HW_UI_REPLY) ? UI_IDLE_REPLY_MS : UI_IDLE_MS;
        if (hw_ui_screen() != HW_UI_CLOCK &&
            (millis() - ui_last_input_ms) > idle) {
            notify_card_id = 0;
            reply_buf[0] = '\0';
            ui_go_clock(NULL);
        }
    }

    // Notify skill: expiry, coalesced SPIFFS snapshot, auto-card on arrival
    // only when the user is on the clock (do not yank them out of a menu).
    notify_poll();

    // Typed reply on its way to the gateway. After the keyboard drain above, so
    // an Enter is carried on the pass that produced it, and after the card has
    // already painted its way back to the clock.
    reply_upstream_poll();

    // Speaker + haptic FIRST — before any full-screen SPI paint can delay the
    // cue. (Boot tone worked; notify was silent when paint ran first.)
    {
        uint8_t lvl = 0;
        char src[NOTIFY_SOURCE_LEN];
        if (notify_take_sound_arrival(&lvl, src, sizeof(src))) {
            hw_sound_notify(lvl);   // amp + queue before haptic I2C
            hw_haptic_notify(lvl);
            hw_sound_poll();        // prime DMA immediately
        }
    }

    // Agent thread inbound (display_force) — refresh open chat room live.
    if (display_force && hw_ui_screen() == HW_UI_AGENT_CHAT) {
        display_force = false;
        bool real_in = g_agents_real_inbound;
        g_agents_real_inbound = false;
        // Stay pinned to latest if user was following the bottom.
        if (agent_scroll < 0 ||
            agent_scroll >= agent_scroll_total - 2) {
            agent_scroll = -1;
        }
        ui_agent_chat_refresh();
        // A genuine arrival (mesh C1 or WiFi) in the open room wakes the
        // panel once; a repaint alone (e.g. synthetic error line) does not.
        if (real_in) ui_note_wake();
    }

    uint32_t arrived_id = 0;
    if (notify_take_arrival(&arrived_id) || display_force) {
        display_force = false;
        // Room not on screen: the arrival flag must not survive to be claimed
        // by a later, unrelated repaint of the room.
        g_agents_real_inbound = false;
        NotifyView v;
        HwUiScreen scr = hw_ui_screen();
        bool on_clock = (scr == HW_UI_CLOCK);
        if (arrived_id && notify_view_by_id(arrived_id, v, NULL, NULL)) {
            if (notify_is_chat_door(v)) {
                // Chat reply door (id "hermes-chat" etc.) — not a severity page.
                int ax = agents_index_from_door(v);
                if (ax >= 0 && scr == HW_UI_AGENT_CHAT && agent_focus == ax) {
                    notify_ack_id(arrived_id);
                    ui_agent_chat_refresh();
                    ui_note_wake();  // real inbound message, not a repaint
                } else if (ax >= 0 && scr == HW_UI_REPLY && agent_compose &&
                           agent_focus == ax) {
                    // Typing in that room — leave compose; thread updates later.
                } else {
                    // Pretty door: open chat from it. Full text is in the room.
                    ui_show_agent_invite_from_view(v, arrived_id);
                }
            } else if (on_clock || scr == HW_UI_MENU || scr == HW_UI_AGENTS ||
                       scr == HW_UI_MSGLIST || scr == HW_UI_INFO) {
                // Real message from any service: severity colours (info/warn/crit).
                notify_card_id = arrived_id;
                snprintf(reply_title, sizeof(reply_title), "%s", v.title);
                hw_ui_show_notify(notify_level_name(v.level), v.source, v.title,
                                  v.body, notify_unread_count());
                ui_note_wake();
            } else {
                // In reply compose / sheets — badge only, don't yank the user.
                hw_ui_invalidate_clock();
            }
        } else if (on_clock) {
            hw_ui_invalidate_clock();
            ui_clock_paint(NULL);
        }
    }

    hw_sound_poll();

    // Progress job reaping (TTL).
    progress_poll();

    // Keep the fuel gauge current — probe_battery() only ran at boot.
    static unsigned long last_battery = 0;
    if (millis() - last_battery > 60000) {
        last_battery = millis();
        battery_refresh();
    }

    // Home clock: 1 Hz field update + crit breathing rule.
    static unsigned long last_clock = 0;
    if (hw_ui_screen() == HW_UI_CLOCK) {
        if (millis() - last_clock >= 1000) {
            last_clock = millis();
            ui_clock_paint(NULL);
        }
        hw_ui_clock_rule_tick(notify_crit_unread());
    }

    for (int i = 0; i < g_skill_count; i++) {
        if (g_skills[i]->tick) g_skills[i]->tick();
    }

    // PING screen sequence — after the skill ticks so the meshcore poll above
    // has already had its chance to receive the pong this pass.
    ui_mesh_ping_poll();

    delay(10);
}
