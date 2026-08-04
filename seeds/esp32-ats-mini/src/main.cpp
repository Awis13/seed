// ESP32 Seed — ATS Mini v4 (ESP32-S3)
//
// The ATS-Mini port of the ESP32 seed: just enough to boot, connect, and let
// an AI agent grow it via OTA firmware uploads.
//
// This is a bring-up seed only. The SI4732 receiver, the parallel ST7789
// display and SSB are deliberately NOT driven here — a radio/display driver
// is something an agent grows later, in a separate step.
//
// Board specifics vs the generic ESP32 seed:
//   - GPIO15 is a power-hold line: it must go HIGH first thing in setup()
//     or the board powers off when running on battery.
//   - Battery telemetry comes from an ADC on GPIO4 (resistor divider), not a
//     fuel gauge: volts = raw * 1.702 / 1000 (matches the ATS-Mini reference).
//   - An SI4732 radio hangs off the I2C bus (SDA=18, SCL=17) with its RESET on
//     GPIO16; driven by the radio skill (skills/radio.cpp).
//   - A 170x320 ST7789 sits on an 8-bit parallel (i8080) bus; driven by the
//     display skill (skills/display.cpp).
//
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
//   POST /wifi/config       — save WiFi credentials

#include <Arduino.h>
#include <stdlib.h>
#include <time.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>

// ===== Configuration =====
#define SEED_VERSION        "0.2.0"
#define HTTP_PORT           8080
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"
#define TZ_FILE             "/tz.txt"
// No location is baked into the seed: UTC until an agent posts a POSIX TZ.
#define TZ_DEFAULT          "UTC0"
// Anything older than this is the pre-NTP epoch, not a real wall clock.
#define TIME_VALID_EPOCH    1700000000

// ===== ATS Mini v4 pin map =====
#define PIN_PWR_EN      15  // power hold — HIGH before anything else, LOW = off
#define PIN_SI4732_RST  16  // SI4732 receiver RESET (driven by the radio skill)
#define PIN_I2C_SCL     17  // I2C bus (SI4732 + peripherals)
#define PIN_I2C_SDA     18
#define PIN_AUDIO_MUTE   3  // audio mute control (also a strapping pin)
#define PIN_AMP_EN      10  // audio amplifier enable
#define PIN_LCD_BL      38  // display backlight
#define PIN_ENC_A        2  // rotary encoder A
#define PIN_ENC_B        1  // rotary encoder B
#define PIN_ENC_KEY     21  // encoder push button
#define PIN_VBAT_ADC     4  // battery voltage divider (ADC1)
// ST7789 170x320 on an 8-bit parallel (i8080) bus — driven by the display skill
#define PIN_TFT_CS       6
#define PIN_TFT_DC       7
#define PIN_TFT_RST      5
#define PIN_TFT_WR       8
#define PIN_TFT_RD       9
#define PIN_TFT_D0      39
#define PIN_TFT_D1      40
#define PIN_TFT_D2      41
#define PIN_TFT_D3      42
#define PIN_TFT_D4      45
#define PIN_TFT_D5      46
#define PIN_TFT_D6      47
#define PIN_TFT_D7      48

// Battery divider factor for GPIO4: volts = raw * VBAT_FACTOR / 1000
// (from the ATS-Mini reference Battery.cpp; no separate ADC enable pin).
#define VBAT_FACTOR     1.702f

// Pins genuinely free for user I/O on the ATS Mini. Everything else is claimed
// by radio, display, encoder, audio, power, native USB (19/20) or OPI PSRAM
// (33-37). Single source of truth: /capabilities and the gpio skill both derive
// from this list so they can never drift apart. 43/44 are UART0 TX/RX, free
// because the console runs over USB-CDC.
static const int gpio_safe_pins[] = {11, 12, 13, 14, 43, 44};
static const int gpio_safe_pins_count =
    sizeof(gpio_safe_pins) / sizeof(gpio_safe_pins[0]);

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

static const I2CDevice known_i2c[] = {
    {0x11, "SI4732 radio"},
    {0x63, "SI4732 radio (alt)"},
    {0x55, "BQ27220 fuel gauge"},
    {0x6B, "BQ25896 charger"},
    {0x3C, "SSD1306 OLED 128x64"},
    {0x3D, "SSD1306 OLED 128x64 (alt)"},
    {0x27, "PCF8574 LCD/IO"},
    {0x20, "PCF8574 IO expander"},
    {0x48, "ADS1115 ADC / TMP102"},
    {0x49, "ADS1115 ADC (alt)"},
    {0x50, "AT24C32 EEPROM"},
    {0x57, "MAX30102 pulse oximeter"},
    {0x68, "MPU6050 IMU / DS3231 RTC"},
    {0x69, "MPU6050 IMU (alt)"},
    {0x76, "BME280 / BMP280 / MS5611"},
    {0x77, "BME280 / BMP085 (alt)"},
    {0x29, "VL53L0X ToF / TSL2591 lux"},
    {0x39, "TSL2561 lux"},
    {0x40, "INA219 power / HDC1080 / SHT30"},
    {0x44, "SHT30 / SHT31"},
    {0x5A, "MLX90614 IR temp / CCS811"},
    {0x5B, "CCS811 air quality (alt)"},
    {0x60, "SI1145 UV / ATECC608"},
    {0x62, "SCD30 CO2"},
    {0x70, "TCA9548A I2C mux"},
    {0x75, "BME688"},
    {0x23, "BH1750 lux"},
    {0x53, "ADXL345 accel"},
    {0x1E, "HMC5883L compass"},
    {0x0D, "QMC5883L compass"},
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

    // I2C bus (SDA=18, SCL=17)
    I2CFound i2c0[MAX_I2C_FOUND];
    int i2c0_count;

    // Battery via ADC on GPIO4 (resistor divider)
    bool has_battery;
    float battery_v;

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

// Battery voltage from the ADC on GPIO4. The ATS-Mini divides VBAT into ADC1
// range; volts = raw * 1.702 / 1000. There is no fuel gauge and no separate
// ADC-enable pin, so this is a plain analogRead.
static void probe_battery() {
    uint32_t raw = analogRead(PIN_VBAT_ADC);
    float v = raw * VBAT_FACTOR / 1000.0f;
    if (v > 2.5f && v < 5.5f) {
        hw.has_battery = true;
        hw.battery_v = v;
    }
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

    // I2C: single bus, SDA=18, SCL=17 (SI4732 receiver lives here)
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    i2c_scan(Wire, hw.i2c0, hw.i2c0_count);

    probe_battery();

    // Dedicated board build — this firmware only runs on ATS-Mini hardware.
    hw.board = "ATS-Mini";

    Serial.printf("[probe] board: %s\n", hw.board);
    Serial.printf("[probe] temp: %.1fC, flash: %uMB, psram: %uKB\n",
        hw.temp_c, (unsigned)(hw.flash_size / 1024 / 1024),
        (unsigned)(hw.psram_size / 1024));
    Serial.printf("[probe] i2c: %d devices\n", hw.i2c0_count);
    Serial.printf("[probe] battery: %.2fV (adc gpio%d)\n", hw.battery_v, PIN_VBAT_ADC);
}

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
static String auth_token = "";
static String ap_ssid = "";
static String mdns_name = "";
static unsigned long boot_time = 0;

// Provisioning AP state. The password is rolled on every raise and lives only
// in this variable and on the device screen — never persisted, never sent over
// the wire. ap_active is true only while the softAP is genuinely up; from_setup_ap()
// keys off it so a gashed AP (softAPIP()==0.0.0.0) can never match a LAN client.
static bool ap_active = false;
static String ap_password = "";

static String wifi_ssid = "";
static String wifi_pass = "";

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

// Encoder-interrupt gate (defined in skills/radio.cpp, included far below). The
// encoder ISR reads flash via encoder.process(); flash writes here — the SPIFFS
// helper and the OTA stream — disable the cache, so they bracket themselves with
// these to keep an ISR from faulting mid-write. Declared this high because
// write_spiffs_file() below already calls them, well before radio.cpp is pulled in.
void radio_encoder_pause();
void radio_encoder_resume();

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
    // Pause the encoder ISR around the flash write: SPIFFS disables the cache and
    // encoder.process() reads flash, so an edge mid-write would crash the chip.
    radio_encoder_pause();
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) { radio_encoder_resume(); return false; }
    f.print(content);
    f.close();
    radio_encoder_resume();
    return true;
}

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

static void wifi_load_config() {
    String json = read_spiffs_file(WIFI_CONFIG_FILE);
    if (json.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    wifi_ssid = doc["ssid"].as<String>();
    wifi_pass = doc["password"].as<String>();
}

static bool wifi_save_config(const String &ssid, const String &pass) {
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["password"] = pass;
    String json;
    serializeJson(doc, json);
    return write_spiffs_file(WIFI_CONFIG_FILE, json);
}

// ===== Provisioning AP =====
//
// The softAP is up only while somebody is actually provisioning the node, and
// its password is random per raise. It used to run permanently in WIFI_AP_STA
// with the password hardcoded as a #define in a public repo. Combined with
// handle_wifi_page(), which handed the auth token to any client on the AP
// subnet, that gave anyone within radio range of a seed sitting inside the
// owner's LAN a token — and the token is POST /firmware/upload, i.e. arbitrary
// code on a box behind the firewall.

// The AP subnet is pinned rather than left on the ESP-IDF default of
// 192.168.4.1/24. from_setup_ap() decides "this client came in over the setup
// AP" by matching the first three octets, so an AP subnet that a STA network
// might also use turns that test into a false positive reachable from the LAN.
// 172.31.157.0/24 sits clear of the common consumer-router defaults and of the
// 192.168.1.0/24 this node's owner runs at home.
#define AP_IP_A 172
#define AP_IP_B  31
#define AP_IP_C 157
#define AP_IP_D   1

// One session's password. No lookalike glyphs — this gets typed off the device
// screen; 12 chars out of a 32-symbol alphabet is 60 bits. Runs after RF is up
// (wifi_setup() has switched WiFi on), which is what makes esp_random() a
// hardware RNG rather than a seeded PRNG.
static void ap_generate_password() {
    static const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";
    const uint32_t n = sizeof(charset) - 1;
    ap_password = "";
    for (int i = 0; i < 12; i++) {
        ap_password += charset[esp_random() % n];
    }
}

// Raise the provisioning softAP. `manual` is reserved for a future press-to-raise
// gesture; today it is always false (called only from wifi_setup() when STA fails).
static void ap_start(bool manual) {
    if (ap_active) return;  // idempotent: a re-raise must not re-roll the password
    (void)manual;
    ap_generate_password();  // esp_random after RF is up
    // Pin the subnet before the AP comes up, clear of the owner's LAN, so a LAN
    // client can never land in it and pass from_setup_ap()'s /24 test. If the pin
    // fails the AP would fall back to the default 192.168.4.1/24, where a LAN
    // client could share that subnet and slip past from_setup_ap() — so refuse to
    // raise the AP at all rather than expose the token-skipping path over the LAN.
    if (!WiFi.softAPConfig(IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(255, 255, 255, 0))) {
        event_add("setup AP: subnet pin failed, not raising AP");
        return;
    }
    WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
    ap_active = true;
    event_add("setup AP up: %s", ap_ssid.c_str());  // SSID only, never the password
}

// True only for a request that arrived over the provisioning AP while that AP is
// actually up. Both halves matter: once the AP is down softAPIP() is 0.0.0.0 and
// the subnet test is meaningless, and belonging to some subnet proves nothing on
// its own. Reaching the node this way costs physical presence plus the per-boot
// password shown on the screen, which is why this is the one path allowed to
// skip the token.
static bool from_setup_ap(AsyncWebServerRequest *request) {
    if (!ap_active) return false;
    IPAddress client_ip;
    client_ip.fromString(request->client()->remoteIP().toString());
    IPAddress ap_ip = WiFi.softAPIP();
    return client_ip[0] == ap_ip[0] && client_ip[1] == ap_ip[1] &&
           client_ip[2] == ap_ip[2];
}

static void wifi_setup() {
    String suffix = get_mac_suffix();
    ap_ssid = "Seed-" + suffix;
    mdns_name = "seed-" + suffix;
    mdns_name.toLowerCase();

    // STA-only: the AP is not started up-front. It comes up only if the stored
    // credentials fail to get us on the network (see below), so a provisioned
    // node running on WiFi never offers a way in.
    WiFi.mode(WIFI_STA);

    // Start SNTP with the stored TZ before associating: the daemon is
    // non-blocking and keeps retrying on its own, so the clock also syncs after
    // a later reconnect (loop() re-begins WiFi) instead of only on a successful
    // boot-time connect.
    configTzTime(tz_string.c_str(), "pool.ntp.org", "time.nist.gov");

    wifi_load_config();
    if (wifi_ssid.length() > 0) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            attempts++;
        }
    }

    // Nothing to provision if the stored credentials already got us online: in
    // that case the AP is never started at all.
    if (WiFi.status() != WL_CONNECTED) ap_start(false);

    if (MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("seed", "tcp", HTTP_PORT);
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

    // Peripherals — honest to the ATS-Mini; the SI4732 radio and ST7789 display are driven by skills.
    doc["has_wifi"] = true;
    doc["has_bluetooth"] = true;
    doc["si4732_reset_pin"] = PIN_SI4732_RST;
    doc["audio_mute_pin"] = PIN_AUDIO_MUTE;
    doc["amp_en_pin"] = PIN_AMP_EN;
    doc["lcd_bl_pin"] = PIN_LCD_BL;
    doc["display"] = "ST7789 170x320 (8-bit parallel i8080)";
    doc["encoder_pins"] = "A=2,B=1,PUSH=21";
    doc["power_hold_pin"] = PIN_PWR_EN;
    doc["vbat_adc_pin"] = PIN_VBAT_ADC;

    // Battery (ADC on GPIO4)
    if (hw.has_battery) {
        doc["battery_v"] = serialized(String(hw.battery_v, 2));
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
    doc["i2c_bus0_pins"] = "SDA=18,SCL=17";

    // GPIO: most pins are taken by radio, display, encoder, audio and power.
    // 43/44 are UART0 TX/RX, free because the console runs over USB-CDC.
    JsonArray pins = doc["gpio_safe"].to<JsonArray>();
    for (int i = 0; i < gpio_safe_pins_count; i++) pins.add(gpio_safe_pins[i]);

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_ip"] = WiFi.localIP().toString();
        doc["wifi_rssi"] = WiFi.RSSI();
    }
    doc["ap_ssid"] = ap_ssid;
    doc["ap_ip"] = WiFi.softAPIP().toString();

    JsonArray ep = doc["endpoints"].to<JsonArray>();
    const char *eps[] = {
        "/health", "/capabilities", "/config.md", "/events",
        "/clock", "/clock/tz",
        "/firmware/version", "/firmware/upload", "/firmware/apply",
        "/firmware/confirm", "/firmware/rollback",
        "/skill", "/ui", NULL
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
        if (total > 4096) { request->send(413, "application/json", "{\"error\":\"too large\"}"); return; }
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
        // OTA app slots are 4MB each (partitions/ats-mini_16mb_ota.csv)
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

        // Detach the encoder ISR for the whole OTA write: Update.write disables
        // the flash cache and the ISR reads flash (encoder.process()), so an edge
        // mid-write would crash. Re-attached in handle_firmware_upload(), which
        // always runs once the stream finishes — every success and error path.
        radio_encoder_pause();

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
    // The stream is done by the time this always-invoked completion handler runs,
    // so re-attach the encoder ISR here — on every path, success or error —
    // rather than in each body-handler branch. If no upload started (no body),
    // radio_encoder_pause() never ran and this attach is a harmless idempotent.
    radio_encoder_resume();

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
        ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

    String s = "# ESP32 Seed — ATS-Mini\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    s += "AP: " + ap_ssid + "\n\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n\n";
    s += "## Grow cycle\n\n";
    s += "ESP32 has no compiler. Build on host, upload binary:\n\n";
    s += "1. GET /capabilities\n";
    s += "2. Write firmware (PlatformIO/Arduino/ESP-IDF)\n";
    s += "3. Compile: `pio run -e ats-mini`\n";
    s += "4. POST /firmware/upload — send .bin (`-H 'Content-Type: application/octet-stream'`)\n";
    s += "5. POST /firmware/apply — reboot\n";
    s += "6. GET /health — verify\n";
    s += "7. POST /firmware/confirm (or auto after 60s)\n\n";
    s += "## Board gotchas\n\n";
    s += "- GPIO15 is power hold: drive it HIGH first in setup(), LOW powers off\n";
    s += "- SI4732 receiver: RESET on GPIO16, I2C on SDA=18/SCL=17 — driven by the `radio` skill (FM/AM/SSB tune, band scan, RSSI/SNR); see POST /radio/tune, /radio/scan, GET /radio/status\n";
    s += "- Battery telemetry: ADC on GPIO4, volts = raw * 1.702 / 1000 (no fuel gauge)\n";
    s += "- ST7789 170x320 display (8-bit parallel i8080): driven by the `display` skill — live radio readout via a TFT_eSprite back-buffer, plus POST /display for custom text\n";
    s += "- Rotary encoder (A=2/B=1, push=21): handheld control — turn to tune, press to toggle frequency/volume (driven in the radio skill's tick())\n";
    s += "- Native USB-Serial/JTAG: flashing needs `--after watchdog_reset`\n";
    s += "- To extend the radio (menu, band presets, RDS, memory, ...): edit skills/radio.cpp + skills/display.cpp; the SI4732 is accessed under a mutex, the UI is a shared sprite\n\n";
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
    // The token is handed out only over the provisioning AP (initial setup) —
    // never to a client on the LAN, even one that happens to share our subnet.
    if (from_setup_ap(request)) {
        html += "<p>Token: " + auth_token + "</p>";
    }
    html += "</body></html>";
    request->send(200, "text/html", html);
}

static void handle_wifi_post(AsyncWebServerRequest *request) {
    // Rewriting the credentials moves the node to a different network, so off the
    // provisioning AP this needs the token like any other mutating call —
    // otherwise anyone on the LAN could repoint the device at their own AP.
    if (!from_setup_ap(request) && !require_auth(request)) return;

    String ssid = "", pass = "";
    if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
    if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();
    if (ssid.length() == 0) {
        request->send(400, "text/html", "<h2>SSID required</h2><a href='/'>Back</a>");
        return;
    }
    wifi_save_config(ssid, pass);
    wifi_ssid = ssid;
    wifi_pass = pass;
    request->send(200, "text/html",
        "<h2>Saved. Connecting...</h2><p>" + ssid + "</p><a href='/'>Back</a>");
    delay(1000);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

// --- Radio dashboard page (/ui) ---
//
// A public, self-contained control panel. Unlike the WiFi page above it never
// carries the token: the page ships zero secrets and the browser supplies the
// Bearer token from localStorage on every fetch. So it is safe to serve to any
// LAN client — an unauthenticated GET /ui only returns static HTML/JS, and the
// data/control endpoints it calls still enforce require_auth on their own.
// Served straight from flash with send_P so it never lands on the heap.
static const char UI_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ATS Mini Radio</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--fg:#e6edf3;--muted:#8b949e;--accent:#3fb950;--err:#f85149;--border:#30363d}
*{box-sizing:border-box}
body{margin:0 auto;max-width:520px;padding:16px;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
h1{font-size:1.1rem;margin:0 0 12px;font-weight:600}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:16px;margin-bottom:14px}
.freq{font-size:2.6rem;font-weight:700;text-align:center;letter-spacing:1px}
.sub{text-align:center;color:var(--muted);margin-top:4px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}
.cell{background:#0d1117;border:1px solid var(--border);border-radius:8px;padding:8px 10px}
.cell .k{color:var(--muted);font-size:.72rem;text-transform:uppercase}
.cell .v{font-size:1.05rem;margin-top:2px}
label{display:block;color:var(--muted);font-size:.8rem;margin:12px 0 4px}
input,select,button{font-size:1rem;padding:11px;border-radius:8px;border:1px solid var(--border);background:#0d1117;color:var(--fg)}
input,select{width:100%}
input[type=range]{padding:6px 0}
button{background:#238636;border-color:#238636;color:#fff;cursor:pointer;width:100%;font-weight:600}
button.sec{background:#21262d;border-color:var(--border)}
.row{display:flex;gap:8px}.row>*{flex:1}
.tag{display:inline-block;padding:2px 8px;border-radius:12px;font-size:.72rem;margin-left:6px;background:var(--err);color:#fff}
#banner{display:none;padding:10px;border-radius:8px;margin-bottom:12px;font-size:.9rem;background:#3a1416;border:1px solid var(--err);color:#ffb3b0}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--muted);margin-right:6px}
.dot.ok{background:var(--accent)}.dot.off{background:var(--err)}
</style></head><body>
<h1><span class="dot" id="live"></span>ATS Mini Radio</h1>
<div id="banner"></div>
<div id="login" class="panel" style="display:none">
<label>Enter Bearer token (shown on the device screen at setup)</label>
<input id="tok" type="password" autocomplete="off" placeholder="token">
<div style="height:8px"></div><button onclick="saveToken()">Save</button>
</div>
<div id="app" style="display:none">
<div class="panel">
<div class="freq mono" id="freq">--</div>
<div class="sub"><span id="mode">--</span><span class="tag" id="muted" style="display:none">MUTE</span><span class="tag" id="sql" style="display:none">SQL</span></div>
<div class="sub mono" id="rds" style="display:none"></div>
<div class="grid">
<div class="cell"><div class="k">RSSI</div><div class="v mono" id="rssi">--</div></div>
<div class="cell"><div class="k">SNR</div><div class="v mono" id="snr">--</div></div>
<div class="cell"><div class="k">Volume</div><div class="v mono" id="vol">--</div></div>
<div class="cell"><div class="k">Clock</div><div class="v mono" id="clock">--</div></div>
<div class="cell"><div class="k">Uptime</div><div class="v mono" id="uptime">--</div></div>
<div class="cell"><div class="k">Version</div><div class="v mono" id="ver">--</div></div>
</div></div>
<div class="panel">
<label>Tune</label>
<div class="row"><input id="tfreq" type="number" placeholder="freq"><select id="tmode"><option>FM</option><option>AM</option><option>LSB</option><option>USB</option></select></div>
<div style="height:8px"></div><button onclick="tune()">Tune</button>
<label>Volume <span id="vollab" class="mono"></span></label>
<input id="volsl" type="range" min="0" max="63" oninput="document.getElementById('vollab').textContent=this.value" onchange="setVol(this.value)">
<label>Squelch <span id="sqlab" class="mono"></span></label>
<input id="sqsl" type="range" min="0" max="127" oninput="document.getElementById('sqlab').textContent=this.value" onchange="setSql(this.value)">
<div style="height:12px"></div>
<div class="row"><button class="sec" onclick="toggleMute()">Toggle Mute</button><button class="sec" onclick="clearToken()">Log out</button></div>
</div>
<div class="panel">
<label>Band</label>
<select id="bandsel" onchange="setBand(this.value)"></select>
</div>
<div class="panel">
<label>Theme</label>
<select id="themesel" onchange="setTheme(this.value)"></select>
</div>
<div class="panel">
<label>Memory</label>
<div id="memlist" class="mono" style="font-size:.85rem">--</div>
<label>Save / clear slot</label>
<div class="row"><input id="memslot" type="number" min="1" max="99" placeholder="1-99"><button class="sec" onclick="memAct('save')">Save</button><button class="sec" onclick="memAct('clear')">Clear</button></div>
</div>
<div class="panel">
<label>DSP / Tuning</label>
<div class="grid">
<div class="cell"><div class="k">Step</div><div class="v mono" id="stepdesc">--</div></div>
<div class="cell"><div class="k">Bandwidth</div><div class="v mono" id="bwdesc">--</div></div>
</div>
<div class="row"><div><label>Step idx</label><input id="stepidx" type="number" min="0"></div><div><label>BW idx</label><input id="bwidx" type="number" min="0"></div></div>
<div style="height:6px"></div>
<div class="row"><button class="sec" onclick="setCfg('step_idx','stepidx')">Set step</button><button class="sec" onclick="setCfg('bw_idx','bwidx')">Set BW</button></div>
<label>AGC</label>
<div class="row"><input id="agc" type="number"><button class="sec" onclick="setCfg('agc','agc')">Set</button></div>
<label>Squelch metric</label>
<div class="row"><button class="sec" id="mrssi" onclick="setMetric(false)">RSSI</button><button class="sec" id="msnr" onclick="setMetric(true)">SNR</button></div>
<div id="dspssb">
<label>AVC (AM/SSB)</label>
<div class="row"><input id="avc" type="number"><button class="sec" onclick="setCfg('avc','avc')">Set</button></div>
<label>SoftMute (AM/SSB)</label>
<div class="row"><input id="softmute" type="number"><button class="sec" onclick="setCfg('softmute','softmute')">Set</button></div>
<label>Cal (SSB, Hz)</label>
<div class="row"><input id="cal" type="number"><button class="sec" onclick="setCfg('cal','cal')">Set</button></div>
</div>
</div>
<div class="panel">
<label>Clock</label>
<div class="grid">
<div class="cell"><div class="k">Local time</div><div class="v mono" id="clk2">--</div></div>
<div class="cell"><div class="k">Timezone</div><div class="v mono" id="tzcur">--</div></div>
</div>
<label>POSIX TZ string</label>
<div class="row"><input id="tzin" placeholder="CET-1CEST,M3.5.0,M10.5.0/3"><button class="sec" onclick="setTz()">Set</button></div>
</div>
<div class="panel">
<label>Device</label>
<div class="grid">
<div class="cell"><div class="k">Battery</div><div class="v mono" id="batt">--</div></div>
<div class="cell"><div class="k">Temp</div><div class="v mono" id="temp">--</div></div>
<div class="cell"><div class="k">WiFi RSSI</div><div class="v mono" id="wrssi">--</div></div>
<div class="cell"><div class="k">Free heap</div><div class="v mono" id="heap">--</div></div>
<div class="cell"><div class="k">IP</div><div class="v mono" id="ip">--</div></div>
</div>
</div>
</div>
<script>
var token=localStorage.getItem('ats_token')||'',lastMute=false;
function $(i){return document.getElementById(i)}
function showLogin(){$('login').style.display='block';$('app').style.display='none'}
function showApp(){$('login').style.display='none';$('app').style.display='block'}
function saveToken(){var v=$('tok').value.trim();if(!v)return;token=v;localStorage.setItem('ats_token',token);$('tok').value='';showApp();poll();boot()}
function clearToken(){token='';localStorage.removeItem('ats_token');showLogin()}
function banner(m){var b=$('banner');if(!m){b.style.display='none';return}b.textContent=m;b.style.display='block'}
function live(s){$('live').className='dot '+s}
function api(p,o){o=o||{};o.headers=o.headers||{};o.headers['Authorization']='Bearer '+token;
return fetch(p,o).then(function(r){if(r.status===401){clearToken();throw{handled:true}}
return r.json().then(function(j){return{status:r.status,body:j}})})}
function post(p,obj){return api(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)})
.then(function(r){if(r.status===503){banner('Radio not detected');throw{handled:true}}
if(r.status>=400){banner((r.body&&r.body.error)||('Error '+r.status));throw{handled:true}}
banner('');return r.body})}
function fmtUp(s){s=s|0;return((s/3600)|0)+'h '+(((s%3600)/60)|0)+'m'}
function poll(){if(!token){showLogin();return}
Promise.all([api('/radio/status'),api('/health')]).then(function(res){live('ok');
var s=res[0].body,h=res[1].body;
if(res[0].status===503){banner('Radio not detected')}else{banner('')}
if(res[0].status===200){
$('freq').textContent=s.freq_display||s.freq||'--';
$('mode').textContent=s.mode||'--';
$('rssi').textContent=(s.rssi!=null?s.rssi+' dBuV':'--');
$('snr').textContent=(s.snr!=null?s.snr+' dB':'--');
$('vol').textContent=(s.volume!=null?s.volume:'--');
$('muted').style.display=s.mute?'inline-block':'none';
$('sql').style.display=s.squelched?'inline-block':'none';
lastMute=!!s.mute;
if(s.rds_ps&&s.rds_ps.trim()){$('rds').style.display='block';$('rds').textContent=s.rds_ps}else{$('rds').style.display='none'}
if(document.activeElement!==$('volsl')&&s.volume!=null){$('volsl').value=s.volume;$('vollab').textContent=s.volume}
if(document.activeElement!==$('sqsl')&&s.squelch!=null){$('sqsl').value=s.squelch;$('sqlab').textContent=s.squelch}
if(document.activeElement!==$('themesel')&&s.theme!=null)$('themesel').value=s.theme;
renderDsp(s);}
if(h){$('uptime').textContent=fmtUp(h.uptime_sec);$('ver').textContent=h.version||'--'}})
.catch(function(e){if(e&&e.handled)return;live('off');banner('Device offline')});
api('/clock').then(function(r){if(r.status===200){var c=r.body;var t=(c.synced&&c.local_time)?c.local_time:(c.synced?'synced':'no sync');$('clock').textContent=t;$('clk2').textContent=t;$('tzcur').textContent=c.tz||'--'}}).catch(function(){})}
function tune(){var f=parseInt($('tfreq').value,10);if(!f){banner('Enter a frequency');return}post('/radio/tune',{mode:$('tmode').value,freq:f}).then(poll).catch(function(){})}
function setVol(v){post('/radio/volume',{volume:parseInt(v,10)}).catch(function(){})}
function setSql(v){post('/radio/config',{squelch:parseInt(v,10)}).catch(function(){})}
function toggleMute(){post('/radio/config',{mute:!lastMute}).then(poll).catch(function(){})}
function renderDsp(s){$('stepdesc').textContent=s.step||'--';$('bwdesc').textContent=s.bw||'--';
if(document.activeElement!==$('agc')&&s.agc!=null)$('agc').value=s.agc;
var snr=(s.squelch_metric==='snr');$('msnr').style.opacity=snr?'1':'.5';$('mrssi').style.opacity=snr?'.5':'1';
var fm=(s.mode==='FM');$('dspssb').style.display=fm?'none':'block';
if(!fm){if(document.activeElement!==$('avc')&&s.avc!=null)$('avc').value=s.avc;
if(document.activeElement!==$('softmute')&&s.softmute!=null)$('softmute').value=s.softmute;
if(document.activeElement!==$('cal')&&s.cal!=null)$('cal').value=s.cal}}
function setBand(v){post('/radio/band',{idx:parseInt(v,10)}).then(poll).catch(function(){})}
function loadBands(){api('/radio/bands').then(function(r){if(r.status!==200)return;var b=r.body,sel=$('bandsel');sel.innerHTML='';(b.bands||[]).forEach(function(x){var o=document.createElement('option');o.value=x.idx;o.textContent=x.name;if(x.idx===b.current)o.selected=true;sel.appendChild(o)})}).catch(function(){})}
function setTheme(v){post('/radio/config',{theme:parseInt(v,10)}).then(poll).catch(function(){})}
function loadThemes(){api('/radio/themes').then(function(r){if(r.status!==200)return;var b=r.body,sel=$('themesel');sel.innerHTML='';(b.themes||[]).forEach(function(x){var o=document.createElement('option');o.value=x.idx;o.textContent=x.name;if(x.idx===b.current)o.selected=true;sel.appendChild(o)})}).catch(function(){})}
function loadMemory(){api('/radio/memory').then(function(r){if(r.status!==200)return;var h='';(r.body.slots||[]).forEach(function(x){h+='<div class="row" style="align-items:center;margin-bottom:4px"><span style="flex:2">#'+x.slot+' '+x.band+' '+x.freq_display+' '+x.mode+'</span><button class="sec" style="flex:1" onclick="memRecall('+x.slot+')">Recall</button></div>'});$('memlist').innerHTML=h||'<span style="color:var(--muted)">no saved slots</span>'}).catch(function(){})}
function memRecall(n){post('/radio/memory',{slot:n,action:'recall'}).then(function(){poll();loadMemory()}).catch(function(){})}
function memAct(a){var n=parseInt($('memslot').value,10);if(!n){banner('Enter a slot 1-99');return}post('/radio/memory',{slot:n,action:a}).then(function(){loadMemory();if(a!=='clear')poll()}).catch(function(){})}
function setCfg(k,id){var v=$(id).value;if(v===''){banner('Enter a value');return}var o={};o[k]=parseInt(v,10);post('/radio/config',o).then(poll).catch(function(){})}
function setMetric(b){post('/radio/config',{squelch_snr:b}).then(poll).catch(function(){})}
function setTz(){var v=$('tzin').value.trim();if(!v){banner('Enter a TZ string');return}api('/clock/tz',{method:'POST',headers:{'Content-Type':'text/plain'},body:v}).then(function(r){if(r.status>=400){banner((r.body&&r.body.error)||('Error '+r.status));return}banner('');$('tzin').value=''}).catch(function(e){if(e&&e.handled)return})}
function loadCaps(){if(!token)return;api('/capabilities').then(function(r){if(r.status!==200)return;var c=r.body;$('batt').textContent=(c.battery_v!=null?c.battery_v+' V':'n/a');$('temp').textContent=(c.temp_c!=null?c.temp_c+' C':'--');$('wrssi').textContent=(c.wifi_rssi!=null?c.wifi_rssi+' dBm':'--');$('heap').textContent=(c.free_heap!=null?((c.free_heap/1024)|0)+' KB':'--');$('ip').textContent=c.wifi_ip||'--'}).catch(function(){})}
function boot(){if(!token){showLogin();return}loadBands();loadThemes();loadMemory();loadCaps()}
if(token)showApp();else showLogin();
poll();boot();setInterval(poll,1000);setInterval(loadCaps,5000);
</script></body></html>)HTML";

static void handle_ui(AsyncWebServerRequest *request) {
    // Public: the page holds no secret, so no auth here. The endpoints it calls
    // do their own require_auth; the browser attaches the Bearer token itself.
    request->send_P(200, "text/html", UI_HTML);
}

// ===== Skills =====
// Forward-declared here so radio.cpp can repaint the status screen on tune;
// the definition lives in display.cpp, included after radio.cpp (same TU).
void display_show_status();
// Live radio readout, repainted from the radio skill's tick(): reads the radio
// accessors and redraws frequency/mode/signal/volume. Defined in display.cpp.
void display_tick_render();
// Setup screen: AP SSID + one-boot password + token. The password lives only
// here and on the screen, so this is the only channel that carries it.
void display_show_ap(const char *ssid, const char *pass, const char *token);

#include "skills/gpio.cpp"
#include "skills/serial.cpp"
#include "skills/radio.cpp"
#include "skills/display.cpp"  // after radio.cpp — reads radio_get_* accessors

static void skills_init() {
    skill_gpio_init();
    skill_serial_init();
    skill_radio_init();
    skill_display_init();
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
    server.on("/ui", HTTP_GET, handle_ui);
    server.on("/", HTTP_GET, handle_wifi_page);
    server.on("/wifi/config", HTTP_POST, handle_wifi_post);

    // Register skill routes
    for (int i = 0; i < g_skill_count; i++) {
        g_skills[i]->register_routes(server);
    }
}

// ===== Main =====

void setup() {
    // Power hold FIRST — on battery the board dies without it.
    pinMode(PIN_PWR_EN, OUTPUT);
    digitalWrite(PIN_PWR_EN, HIGH);

    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    hw_probe();       // I2C scan + battery ADC
    tz_load();        // before wifi_setup(): configTzTime() needs the TZ string
    wifi_setup();     // RF up first; also raises the setup AP if STA fails
    token_load();     // after wifi_setup(): needs RF up for a real hardware RNG
    skills_init();    // display can now show the token / AP password on screen
    setup_routes();
    server.begin();

    Serial.printf("\nESP32 Seed v%s (ATS-Mini)\n", SEED_VERSION);
    // The token is deliberately never printed to Serial: it lives only in flash
    // and on the device screen. A serial log would leak it to anyone with a cable.
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("http://%s:%d/health\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    Serial.printf("http://%s:%d/health  (AP: %s)\n",
        WiFi.softAPIP().toString().c_str(), HTTP_PORT, ap_ssid.c_str());

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

    // Auto-confirm after 60s
    if (!firmware_confirmed && !firmware_confirm_attempted &&
        (millis() - boot_time) > 60000 && WiFi.status() == WL_CONNECTED) {
        firmware_confirm_attempted = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        firmware_confirmed = true;
        if (err == ESP_OK) event_add("firmware auto-confirmed");
    }

    // WiFi reconnect
    static unsigned long last_wifi = 0;
    if (wifi_ssid.length() > 0 && WiFi.status() != WL_CONNECTED &&
        millis() - last_wifi > 30000) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        last_wifi = millis();
    }

    // Give each skill its per-loop slice: encoder polling, screen repaint, etc.
    for (int i = 0; i < g_skill_count; i++) {
        if (g_skills[i]->tick) g_skills[i]->tick();
    }

    delay(10);
}
