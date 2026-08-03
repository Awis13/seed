// ESP32 Seed — LilyGO T-Embed CC1101 (ESP32-S3)
//
// The T-Embed port of the ESP32 seed: just enough to boot, connect,
// and let an AI agent grow it via OTA firmware uploads.
//
// Board specifics vs the generic ESP32 seed:
//   - GPIO15 is a power-hold line: it must go HIGH first thing in setup()
//     or the board powers off when running on battery.
//   - No SX1262 LoRa; a CC1101 sub-GHz transceiver sits on the SPI bus it
//     shares with the ST7789 display. The seed only probes it (PARTNUM/
//     VERSION) — a real radio driver is something an agent grows later.
//   - 8MB of octal PSRAM eats GPIO33-37; the gpio skill refuses to drive them.
//   - Battery telemetry comes from a BQ27220 fuel gauge over I2C, not an ADC.
//   - ST7789 170x320 display shows IP/token/status so the device is
//     self-describing without a serial console.
//
// Endpoints:
//   GET  /health            — alive check (no auth)
//   GET  /capabilities      — hardware fingerprint
//   GET  /config.md         — node description
//   POST /config.md         — update description
//   GET  /events            — event log (?since=unix_ts)
//   GET  /firmware/version  — version, partition, uptime
//   POST /firmware/upload   — upload OTA binary (streaming)
//   POST /firmware/apply    — reboot into new firmware
//   POST /firmware/confirm  — confirm (cancel rollback)
//   POST /firmware/rollback — revert to previous
//   GET  /skill             — AI agent skill file
//   GET  /                  — WiFi config page
//   POST /wifi/config       — save WiFi credentials

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <SPI.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <TFT_eSPI.h>

// ===== Configuration =====
#define SEED_VERSION        "0.2.0"
#define HTTP_PORT           8080
#define AP_PASSWORD         "seed1313"
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"

// ===== T-Embed CC1101 pin map =====
#define PIN_PWR_EN      15  // power hold — HIGH before anything else, LOW = off
#define PIN_TFT_CS      41
#define PIN_TFT_DC      16
#define PIN_TFT_BL      21  // AW9364 backlight driver enable
#define PIN_SPI_SCLK    11  // shared bus: ST7789 + CC1101 + microSD
#define PIN_SPI_MOSI     9
#define PIN_SPI_MISO    10
#define PIN_CC1101_CS   12
#define PIN_CC1101_GDO0  3
#define PIN_CC1101_GDO2 38
#define PIN_SD_CS       13
#define PIN_I2C_SDA      8
#define PIN_I2C_SCL     18
#define PIN_ENC_A        4
#define PIN_ENC_B        5
#define PIN_ENC_KEY      0  // encoder push button (also a strapping pin)
#define PIN_USER_KEY     6
#define PIN_WS2812      14  // 8x WS2812 ring
#define PIN_IR_TX        2
#define PIN_IR_RX        1
#define BQ27220_ADDR    0x55
#define BQ25896_ADDR    0x6B

// The board is qio_opi: GPIO33-37 carry the octal PSRAM bus. They read like
// ordinary GPIOs and are not listed anywhere in the schematic's header pinout,
// but driving one takes the PSRAM down with the running app.
#define PIN_PSRAM_FIRST 33
#define PIN_PSRAM_LAST  37
// USB-Serial/JTAG data lines — the console and the only recovery path.
#define PIN_USB_DM      19
#define PIN_USB_DP      20

// The only pins with nothing wired to them on this board. Everything else is
// claimed by a peripheral, the PSRAM bus, USB, or a strapping function.
// 43/44 are UART0 TX/RX, free because the console runs over USB-CDC.
// Shared with skills/gpio.cpp, which is included further down.
static const int gpio_safe_pins[] = {17, 43, 44};
static const int gpio_safe_pins_count = sizeof(gpio_safe_pins) / sizeof(gpio_safe_pins[0]);

static bool gpio_is_safe(int pin) {
    for (int i = 0; i < gpio_safe_pins_count; i++) {
        if (gpio_safe_pins[i] == pin) return true;
    }
    return false;
}

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
    {BQ27220_ADDR, "BQ27220 fuel gauge"},
    {BQ25896_ADDR, "BQ25896 charger"},
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

    // I2C bus (SDA=8, SCL=18)
    I2CFound i2c0[MAX_I2C_FOUND];
    int i2c0_count;

    // CC1101 sub-GHz transceiver
    bool has_cc1101;
    uint8_t cc1101_partnum;
    uint8_t cc1101_version;

    // Battery via BQ27220 fuel gauge
    bool has_battery;
    float battery_v;
    int battery_soc;

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

// CC1101 status registers are read with the burst bit set: 0x30|0xC0.
// This runs BEFORE tft.init() — the display library owns the shared SPI bus
// afterwards, and the seed never talks to the CC1101 again.
static void probe_cc1101() {
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);

    SPIClass spi(HSPI);
    spi.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
    spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

    // SRES strobe, then give the crystal time to settle
    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    spi.transfer(0x30);
    digitalWrite(PIN_CC1101_CS, HIGH);
    delay(1);

    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    spi.transfer(0x30 | 0xC0);  // PARTNUM
    hw.cc1101_partnum = spi.transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);

    digitalWrite(PIN_CC1101_CS, LOW);
    delayMicroseconds(50);
    spi.transfer(0x31 | 0xC0);  // VERSION
    hw.cc1101_version = spi.transfer(0x00);
    digitalWrite(PIN_CC1101_CS, HIGH);

    spi.endTransaction();
    spi.end();

    // VERSION reads 0x14 (or 0x04 on older silicon); 0x00/0xFF = nothing there
    hw.has_cc1101 = (hw.cc1101_version != 0x00 && hw.cc1101_version != 0xFF);
}

static bool bq27220_read16(uint8_t reg, uint16_t &val) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_ADDR, 2) != 2) return false;
    val = Wire.read() | (Wire.read() << 8);
    return true;
}

static void probe_battery() {
    // BQ27220 standard commands: 0x08 = Voltage (mV), 0x2C = StateOfCharge (%)
    uint16_t mv = 0, soc = 0;
    if (bq27220_read16(0x08, mv) && mv > 2000 && mv < 6000) {
        hw.has_battery = true;
        hw.battery_v = mv / 1000.0f;
        if (bq27220_read16(0x2C, soc) && soc <= 100) {
            hw.battery_soc = soc;
        } else {
            hw.battery_soc = -1;
        }
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

    // I2C: single bus, SDA=8, SCL=18 (fuel gauge + charger live here)
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    i2c_scan(Wire, hw.i2c0, hw.i2c0_count);

    probe_cc1101();
    probe_battery();

    // Board detection heuristic
    bool fuel_gauge = false;
    for (int i = 0; i < hw.i2c0_count; i++) {
        if (hw.i2c0[i].addr == BQ27220_ADDR) fuel_gauge = true;
    }
    if (hw.has_cc1101 && fuel_gauge) {
        hw.board = "LilyGO T-Embed CC1101";
    } else if (hw.has_cc1101) {
        hw.board = "ESP32-S3 + CC1101 (unknown board)";
    } else {
        hw.board = "ESP32-S3 (generic)";
    }

    Serial.printf("[probe] board: %s\n", hw.board);
    Serial.printf("[probe] temp: %.1fC, flash: %uMB, psram: %uKB\n",
        hw.temp_c, (unsigned)(hw.flash_size / 1024 / 1024),
        (unsigned)(hw.psram_size / 1024));
    Serial.printf("[probe] i2c: %d devices\n", hw.i2c0_count);
    Serial.printf("[probe] cc1101: %s (partnum=0x%02X version=0x%02X)\n",
        hw.has_cc1101 ? "yes" : "no", hw.cc1101_partnum, hw.cc1101_version);
    Serial.printf("[probe] battery: %.2fV soc=%d%%\n", hw.battery_v, hw.battery_soc);
}

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
static TFT_eSPI tft;
static String auth_token = "";
static String ap_ssid = "";
static String mdns_name = "";
static unsigned long boot_time = 0;

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
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(content);
    f.close();
    return true;
}

// ===== Display =====

static void display_init() {
    tft.init();
    tft.setRotation(3);  // 320x170 landscape, knob on the right
    tft.fillScreen(TFT_BLACK);
    // Backlight only after the panel is initialized — avoids a garbage flash
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
}

static void display_status() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(4);
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.setCursor(8, 6);
    tft.printf("SEED v%s", SEED_VERSION);

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 40);
    tft.print(hw.board);

    tft.setCursor(8, 60);
    if (WiFi.status() == WL_CONNECTED) {
        tft.printf("WiFi  %s  %s", WiFi.SSID().c_str(),
                   WiFi.localIP().toString().c_str());
    } else {
        tft.print("WiFi  not connected");
    }

    tft.setCursor(8, 78);
    tft.printf("AP    %s  %s", ap_ssid.c_str(),
               WiFi.softAPIP().toString().c_str());

    tft.setCursor(8, 96);
    tft.printf("mDNS  %s.local:%d", mdns_name.c_str(), HTTP_PORT);

    tft.setCursor(8, 114);
    if (hw.has_battery) {
        tft.printf("Batt  %.2fV", hw.battery_v);
        if (hw.battery_soc >= 0) tft.printf("  %d%%", hw.battery_soc);
    } else {
        tft.print("Batt  n/a");
    }
    tft.printf("   CC1101 %s", hw.has_cc1101 ? "ok" : "-");

    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    tft.setCursor(8, 140);
    tft.print("Token");
    tft.setCursor(8, 154);
    tft.print(auth_token);
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

static void wifi_setup() {
    String suffix = get_mac_suffix();
    ap_ssid = "Seed-" + suffix;
    mdns_name = "seed-" + suffix;
    mdns_name.toLowerCase();

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid.c_str(), AP_PASSWORD);
    delay(100);

    wifi_load_config();
    if (wifi_ssid.length() > 0) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        }
    }

    if (MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("seed", "tcp", HTTP_PORT);
    }
}

// ===== HTTP Handlers =====

// mem_mb per docs/capabilities-spec.md: total RAM in megabytes. Internal SRAM
// heap plus the 8MB OPI PSRAM, rounded to the nearest MB.
static uint32_t total_mem_mb() {
    uint64_t bytes = (uint64_t)ESP.getHeapSize() + (uint64_t)ESP.getPsramSize();
    return (uint32_t)((bytes + 512 * 1024) / (1024 * 1024));
}

static void handle_health(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["uptime_sec"] = (millis() - boot_time) / 1000;
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;
    doc["arch"] = "esp32-s3";
    doc["mem_mb"] = total_mem_mb();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_capabilities(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;
    doc["board_model"] = hw.board;
    doc["hostname"] = mdns_name;

    // Chip
    doc["cpu_model"] = hw.chip_model;
    doc["chip_revision"] = hw.chip_revision;
    doc["arch"] = "esp32-s3";
    doc["mem_mb"] = total_mem_mb();
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

    // Peripherals
    doc["has_wifi"] = true;
    doc["has_bluetooth"] = true;
    doc["has_cc1101"] = hw.has_cc1101;
    if (hw.has_cc1101) {
        char v[8];
        snprintf(v, sizeof(v), "0x%02X", hw.cc1101_version);
        doc["cc1101_version"] = String(v);
        doc["cc1101_pins"] = "CS=12,GDO0=3,GDO2=38,SCK=11,MISO=10,MOSI=9 (bus shared with display)";
    }
    doc["display"] = "ST7789 170x320 IPS (TFT_eSPI, rotation 3)";
    doc["display_pins"] = "CS=41,DC=16,BL=21,SCK=11,MISO=10,MOSI=9";
    doc["encoder_pins"] = "A=4,B=5,KEY=0,USER_KEY=6";
    doc["ws2812"] = "8 LEDs on pin 14";
    doc["ir_pins"] = "TX=2,RX=1";
    doc["mic_pins"] = "DATA=42,CLK=39";
    doc["power_hold_pin"] = 15;

    // Battery
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
        doc["i2c_bus0_pins"] = "SDA=8,SCL=18";
    }

    // GPIO: nearly everything is taken by onboard peripherals, the PSRAM bus
    // or USB. Same allowlist the gpio skill reports as safe.
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
    request->send(200, "text/markdown; charset=utf-8", read_spiffs_file(CONFIG_MD_FILE));
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
        // OTA app slots are 4MB each (partitions/tembed_16mb_ota.csv)
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
    if (ota_upload_error && strstr(ota_upload_error_msg, "auth") != NULL) {
        request->send(401, "application/json", "{\"error\":\"auth required\"}");
        return;
    }
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

    String s = "# ESP32 Seed — LilyGO T-Embed CC1101\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    s += "AP: " + ap_ssid + "\n\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n\n";
    s += "## Grow cycle\n\n";
    s += "ESP32 has no compiler. Build on host, upload binary:\n\n";
    s += "1. GET /capabilities\n";
    s += "2. Write firmware (PlatformIO/Arduino/ESP-IDF)\n";
    s += "3. Compile: `pio run -e tembed`\n";
    s += "4. POST /firmware/upload — send .bin (`-H 'Content-Type: application/octet-stream'`)\n";
    s += "5. POST /firmware/apply — reboot\n";
    s += "6. GET /health — verify\n";
    s += "7. POST /firmware/confirm (or auto after 60s)\n\n";
    s += "## Board gotchas\n\n";
    s += "- GPIO15 is power hold: drive it HIGH first in setup(), LOW powers off\n";
    s += "- ST7789 display, CC1101 radio and microSD share one SPI bus (SCK=11, MISO=10, MOSI=9)\n";
    s += "- Battery telemetry: BQ27220 fuel gauge at I2C 0x55 (SDA=8, SCL=18), not an ADC\n";
    s += "- Flashing over USB-Serial/JTAG needs `--after watchdog_reset`\n\n";
    s += "## API\n\n";
    s += "| Method | Path | Description |\n";
    s += "|--------|------|-------------|\n";
    s += "| GET | /health | Alive (no auth) |\n";
    s += "| GET | /capabilities | Hardware info |\n";
    s += "| GET | /config.md | Node config |\n";
    s += "| POST | /config.md | Update config |\n";
    s += "| GET | /events | Event log |\n";
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
        "</form>";
    if (WiFi.status() == WL_CONNECTED)
        html += "<p>Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")</p>";
    // Only show token to clients on the AP subnet (initial setup)
    IPAddress client_ip;
    client_ip.fromString(request->client()->remoteIP().toString());
    IPAddress ap_ip = WiFi.softAPIP();
    if (client_ip[0] == ap_ip[0] && client_ip[1] == ap_ip[1] && client_ip[2] == ap_ip[2]) {
        html += "<p>Token: " + auth_token + "</p>";
    }
    html += "</body></html>";
    request->send(200, "text/html", html);
}

static void handle_wifi_post(AsyncWebServerRequest *request) {
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

// ===== Skills =====
#include "skills/gpio.cpp"
#include "skills/serial.cpp"

static void skills_init() {
    skill_gpio_init();
    skill_serial_init();
}

// ===== Routes =====

static void setup_routes() {
    server.on("/health", HTTP_GET, handle_health);
    server.on("/capabilities", HTTP_GET, handle_capabilities);
    server.on("/config.md", HTTP_GET, handle_config_get);
    server.on("/config.md", HTTP_POST, handle_config_post, NULL, handle_body_collect);
    server.on("/events", HTTP_GET, handle_events);
    server.on("/firmware/version", HTTP_GET, handle_firmware_version);
    server.on("/firmware/upload", HTTP_POST, handle_firmware_upload, NULL, handle_firmware_upload_body);
    server.on("/firmware/apply", HTTP_POST, handle_firmware_apply);
    server.on("/firmware/confirm", HTTP_POST, handle_firmware_confirm);
    server.on("/firmware/rollback", HTTP_POST, handle_firmware_rollback);
    server.on("/skill", HTTP_GET, handle_skill);
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

    // Park all chip-selects on the shared SPI bus before anyone talks on it
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    hw_probe();       // I2C + CC1101 probe (before the display claims the SPI bus)
    display_init();
    token_load();
    wifi_setup();
    skills_init();
    setup_routes();
    server.begin();
    display_status();

    Serial.printf("\nESP32 Seed v%s (T-Embed CC1101)\n", SEED_VERSION);
    Serial.printf("Token: %s\n", auth_token.c_str());
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

    // Redraw the status screen when connectivity changes
    static wl_status_t last_status = WL_NO_SHIELD;
    static unsigned long last_draw_check = 0;
    if (millis() - last_draw_check > 1000) {
        last_draw_check = millis();
        wl_status_t st = WiFi.status();
        if (st != last_status) {
            last_status = st;
            display_status();
        }
    }

    delay(10);
}
