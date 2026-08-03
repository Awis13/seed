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
//   - ST7789 170x320 display shows a clock plus address/battery so the device
//     is self-describing without a serial console. The provisioning AP password
//     and the auth token appear there only while the setup AP is up.
//   - The rotary encoder drives an on-device menu over that clock (src/ui.h):
//     messages, TV-B-Gone (whole-region blasts and single codes by brand), the
//     setup AP and an info page, so nothing needs a network client to be used.
//     It is a front-end over the same state the API drives, not a second
//     implementation of it.
//   - POST /notify makes the device a pager: anything that can run curl puts a
//     message on the screen, and the knob acknowledges it. The queue lives in
//     skills/notify.cpp; the clock face carries the unread count.
//
// Endpoints:
//   GET  /health            — alive check (no auth)
//   GET  /capabilities      — hardware fingerprint
//   GET  /config.md         — node description
//   POST /config.md         — update description
//   GET  /events            — event log (?since=unix_ts)
//   GET  /clock             — local time, timezone, NTP sync state
//   POST /clock/tz          — set the POSIX TZ string
//   GET  /firmware/version  — version, partition, uptime
//   POST /firmware/upload   — upload OTA binary (streaming)
//   POST /firmware/apply    — reboot into new firmware
//   POST /firmware/confirm  — confirm (cancel rollback)
//   POST /firmware/rollback — revert to previous
//   GET  /skill             — AI agent skill file
//   GET  /                  — WiFi config page
//   POST /wifi/config       — save WiFi credentials (token-free only on the AP)

#include <Arduino.h>
#include <stdlib.h>
#include <time.h>
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
#define SEED_VERSION        "0.4.1"
#define HTTP_PORT           8080
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"
#define TZ_FILE             "/tz.txt"
// No location is baked into the seed: UTC until an agent posts a POSIX TZ.
#define TZ_DEFAULT          "UTC0"
// Anything older than this is the pre-NTP epoch, not a real wall clock.
#define TIME_VALID_EPOCH    1700000000
// How long an OTA upload may go without a body chunk before loop() tears it
// down. See the watchdog in loop() for why this number.
#define OTA_STALL_TIMEOUT_MS 30000

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
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > TIME_VALID_EPOCH) {
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

// probe_battery() runs once at boot, so the cached figures would otherwise stay
// frozen at the boot-time snapshot. Re-read the same two registers periodically
// to keep the on-screen charge level honest.
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

// Provisioning AP state, declared here because the display reads it. The
// password is rolled on every raise and exists only in this variable and on the
// screen — never persisted, never sent anywhere.
//
// A gesture-raised session is time-boxed: someone may have leaned on the key,
// and the radio must not then stay up forever. The boot-time session does not
// expire, because with no working credentials the AP is the only way in.
#define AP_SESSION_MS (10UL * 60UL * 1000UL)
static bool ap_active = false;
static String ap_password = "";
static bool ap_temporary = false;
static unsigned long ap_expires_at = 0;

// Whole minutes left on a temporary session, rounded up so it never reads 0
// while the AP is still up. Returns 0 for a session that does not expire.
static unsigned long ap_minutes_left() {
    if (!ap_temporary) return 0;
    long remaining = (long)(ap_expires_at - millis());
    if (remaining <= 0) return 0;
    return ((unsigned long)remaining + 59999UL) / 60000UL;
}

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
// millis() of the last body chunk received, for the abandoned-upload watchdog
// in loop(). Only meaningful while ota_in_progress.
static unsigned long ota_last_chunk_ms = 0;
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

// Same, for a file whose previous contents are worth more than the write.
//
// FILE_WRITE truncates on open, so an ordinary write spends its whole duration
// with the file empty: power lost in there takes the old snapshot with the new
// one. Writing beside it and renaming afterwards narrows that to the remove
// plus the rename, and even in there the complete new snapshot is still on
// flash under the temp name — which is why the reader falls back to it.
//
// SPIFFS.rename() will not clobber an existing name, hence the remove.
static bool write_spiffs_file_atomic(const char *path, const char *tmp_path,
                                     const String &content) {
    if (!write_spiffs_file(tmp_path, content)) return false;
    SPIFFS.remove(path);
    return SPIFFS.rename(tmp_path, path);
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

static bool clock_local_time(struct tm &out) {
    time_t now = time(NULL);
    if (now <= TIME_VALID_EPOCH) return false;
    return localtime_r(&now, &out) != NULL;
}

// ===== Display =====

// Clock-first status screen on the 320x170 panel:
//
//   v0.4.1                   BAT 87%              192.168.1.42     <- font 2
//   ------------------------------------------------------------
//                     12:34   07                                   <- font 8 + 4
//                    Sun 03 Aug 2026                               <- font 4
//   RSSI -55 dBm                              seed-a1b2.local      <- font 2
//                                                                  <- font 2
//
// The last two rows switch with the connectivity mode: on the network they
// carry RSSI and the mDNS name, in setup mode the AP name, its one-shot
// password and the auth token, and offline the gesture that raises the AP.
//
// Three colours only: warm white for the digits, slate for everything
// secondary, one amber accent (seconds, and the AP password in setup mode).
//
// Notifications add exactly two more, and they are states rather than decoration
// — teal for informational, red for critical, with warn reusing the amber accent
// that is already the alert colour on this face. The rule the whole UI keeps is
// that no single screen shows more than three of them at once over the ground:
// the notification card spends its three on warm white, slate and one level
// colour, and the message list spends them on warm white, slate and amber.
#define COL_BG      TFT_BLACK
#define COL_TIME    0xFFBC  // warm white
#define COL_DIM     0x7BD0  // slate grey
#define COL_ACCENT  0xFD05  // amber
#define COL_RULE    0x2945  // hairline under the header
#define COL_INFO    0x2DD5  // teal   — info level
#define COL_CRIT    0xE1C5  // red    — crit level

#define HDR_Y       2
// Height of the inverse header bar the menu screens draw. The clock face keeps
// its plain header and the hairline rule below it.
#define HDR_BAR_H   18
#define CLOCK_Y     24
#define DATE_Y      102
#define ROW1_Y      130
#define ROW2_Y      150

// Unread badge on the clock face: an amber dot and a count, sitting in the gap
// between the version string (which ends at x=50 in font 2) and the battery
// field's erase rectangle (which starts at x=125). Both numbers are measured
// against TFT_eSPI's own width tables, not estimated.
//
// The header carries the version alone rather than "SEED v...", because the
// badge is what the version grows into and the collision would be silent:
// "SEED v0.3.10" measures 88px and would have ended at x=96, through a dot
// sitting at 93-99, so the first two-digit component in any position broke the
// row. "v0.4.1" is 42px, which leaves 43px of growth before the dot instead of
// 5 — room for "v10.10.10" and then some.
#define BADGE_CX    96
#define BADGE_R      3
#define BADGE_TEXT_X 103
#define BADGE_PAD    20

// Defined in skills/ir.cpp, which is included further down: one line of blast
// progress for the bottom row. The skill never touches TFT_eSPI itself.
static bool ir_status_line(char *out, size_t n);

// Defined in skills/notify.cpp, same arrangement: the notification store never
// touches TFT_eSPI, it only answers questions the clock face asks of it.
static int notify_unread_count();
static bool notify_crit_unread();

static bool display_ready = false;
// Set from the web server task, consumed by the clock tick in loop(): TFT_eSPI
// owns the SPI bus and must only be driven from one task.
static volatile bool display_force = false;

// Geometry of the time block, measured once — HH:MM is always five glyphs, so
// the block never shifts and every field can be repainted in place.
static int32_t clock_x = 0, clock_w = 0, sec_x = 0, sec_y = 0, sec_w = 0;

// Last rendered text per field, so a tick that changes nothing costs no SPI.
static char fld_batt[16]  = "";
static char fld_addr[24]  = "";
static char fld_time[8]   = "";
static char fld_sec[4]    = "";
static char fld_date[24]  = "";
static char fld_left[32]  = "";
static char fld_right[32] = "";
static char fld_note[48]  = "";
// Unread count the badge currently shows, or -1 for "nothing drawn yet". 10
// stands for "9+", so that going from 12 unread to 11 costs no SPI.
static int clock_badge_drawn = -1;

// Repaint a field only when its content changed. An opaque text background plus
// a fixed padding width erases the previous value, so per-second updates never
// need fillScreen and never flicker.
//
// `bg` is that opaque background, and defaults to the ground because almost
// every field sits on it. The exceptions are fields drawn on something already
// painted — text inside a tinted notification card, or on an inverse bar —
// where erasing to black would punch a hole through what is underneath. The
// cache keys on the text alone, so a field whose background changes but whose
// text does not needs display_force; every caller that moves a field onto or
// off a coloured ground is a screen transition and already sets it.
static void draw_field(char *cache, size_t cache_size, const char *text,
                       int32_t x, int32_t y, uint8_t font, uint16_t color,
                       uint8_t datum, uint16_t padding, uint16_t bg = COL_BG) {
    if (!display_force && strncmp(cache, text, cache_size - 1) == 0) return;
    snprintf(cache, cache_size, "%s", text);
    tft.setTextDatum(datum);
    tft.setTextColor(color, bg);
    tft.setTextPadding(padding);
    tft.drawString(text, x, y, font);
    tft.setTextPadding(0);
}

static void display_init() {
    tft.init();
    tft.setRotation(3);  // 320x170 landscape, knob on the right
    tft.fillScreen(COL_BG);
    // Backlight only after the panel is initialized — avoids a garbage flash
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);

    clock_w = tft.textWidth("00:00", 8);
    sec_w = tft.textWidth("00", 4);
    clock_x = (tft.width() - (clock_w + 12 + sec_w)) / 2;
    if (clock_x < 4) clock_x = 4;
    sec_x = clock_x + clock_w + 12;
    sec_y = CLOCK_Y + tft.fontHeight(8) - tft.fontHeight(4);  // sit on the baseline
    display_ready = true;
}

// One second of screen work: everything here is change-detected.
static void display_tick() {
    if (!display_ready) return;
    char buf[48];

    bool connected = (WiFi.status() == WL_CONNECTED);

    // Header row, three slots. The padding widths are also the erase
    // rectangles, so they tile the row instead of overlapping.
    if (hw.has_battery && hw.battery_soc >= 0) {
        snprintf(buf, sizeof(buf), "BAT %d%%", hw.battery_soc);
    } else {
        snprintf(buf, sizeof(buf), "BAT --");
    }
    draw_field(fld_batt, sizeof(fld_batt), buf, tft.width() / 2, HDR_Y, 2,
               COL_DIM, TC_DATUM, 70);

    if (connected) {
        snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
    } else if (ap_active) {
        snprintf(buf, sizeof(buf), "AP %s", WiFi.softAPIP().toString().c_str());
    } else {
        snprintf(buf, sizeof(buf), "offline");
    }
    draw_field(fld_addr, sizeof(fld_addr), buf, tft.width() - 8, HDR_Y, 2,
               COL_DIM, TR_DATUM, 117);

    // Unread messages: a dot and a count, and nothing at all when the queue is
    // read. The dot is not a field, so it is drawn from an explicit change test
    // rather than through draw_field; both halves move together so one test
    // covers them. Counts above nine become "9+" — the exact number is on the
    // Messages screen, and the header has 20px for this.
    int unread = notify_unread_count();
    int badge = unread > 9 ? 10 : unread;
    if (display_force || badge != clock_badge_drawn) {
        clock_badge_drawn = badge;
        tft.fillCircle(BADGE_CX, HDR_Y + 8, BADGE_R, badge ? COL_ACCENT : COL_BG);
        if (badge == 0)      buf[0] = '\0';
        else if (badge > 9)  snprintf(buf, sizeof(buf), "9+");
        else                 snprintf(buf, sizeof(buf), "%d", badge);
        // Deliberately not cached: the test above already decided, and the
        // cache would skip the erase on the way back to zero.
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_ACCENT, COL_BG);
        tft.setTextPadding(BADGE_PAD);
        tft.drawString(buf, BADGE_TEXT_X, HDR_Y, 2);
        tft.setTextPadding(0);
    }

    struct tm now;
    char hhmm[8], ss[4], date[24];
    if (clock_local_time(now)) {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", now.tm_hour, now.tm_min);
        snprintf(ss, sizeof(ss), "%02d", now.tm_sec);
        strftime(date, sizeof(date), "%a %d %b %Y", &now);
    } else {
        // Before the first NTP sync a placeholder beats rendering 1970.
        snprintf(hhmm, sizeof(hhmm), "--:--");
        snprintf(ss, sizeof(ss), "--");
        snprintf(date, sizeof(date), "waiting for NTP");
    }
    draw_field(fld_time, sizeof(fld_time), hhmm, clock_x, CLOCK_Y, 8,
               COL_TIME, TL_DATUM, clock_w);
    draw_field(fld_sec, sizeof(fld_sec), ss, sec_x, sec_y, 4,
               COL_ACCENT, TL_DATUM, sec_w);
    draw_field(fld_date, sizeof(fld_date), date, tft.width() / 2, DATE_Y, 4,
               COL_DIM, TC_DATUM, 300);

    // Bottom block: three mutually exclusive modes. The AP password and the
    // auth token are on screen only while the setup AP is actually up — that
    // screen is their only channel, so nothing else has to carry them.
    char row1l[32], row1r[32], row2[48];
    if (ap_active) {
        // A gesture-raised AP is time-boxed, so say how long it has left.
        unsigned long mins = ap_minutes_left();
        if (mins > 0) {
            snprintf(row1l, sizeof(row1l), "AP %s  %lum", ap_ssid.c_str(), mins);
        } else {
            snprintf(row1l, sizeof(row1l), "AP %s", ap_ssid.c_str());
        }
        snprintf(row1r, sizeof(row1r), "PW %s", ap_password.c_str());
        snprintf(row2, sizeof(row2), "TOKEN %s", auth_token.c_str());
    } else if (connected) {
        snprintf(row1l, sizeof(row1l), "RSSI %d dBm", (int)WiFi.RSSI());
        snprintf(row1r, sizeof(row1r), "%s.local", mdns_name.c_str());
        row2[0] = '\0';
    } else {
        snprintf(row1l, sizeof(row1l), "WiFi offline");
        row1r[0] = '\0';
        snprintf(row2, sizeof(row2), "hold KEY 3s for setup AP");
    }
    // An IR blast borrows the note row, but only when it is otherwise empty:
    // the token and the setup gesture have nowhere else to be displayed.
    if (row2[0] == '\0') ir_status_line(row2, sizeof(row2));
    draw_field(fld_left, sizeof(fld_left), row1l, 8, ROW1_Y, 2,
               COL_DIM, TL_DATUM, 150);
    draw_field(fld_right, sizeof(fld_right), row1r, tft.width() - 8, ROW1_Y, 2,
               ap_active ? COL_ACCENT : COL_DIM, TR_DATUM, 150);
    draw_field(fld_note, sizeof(fld_note), row2, tft.width() / 2, ROW2_Y, 2,
               COL_DIM, TC_DATUM, 304);

    display_force = false;
}

// Connectivity the clock face was last drawn for. A change here is what earns
// a full repaint, and it lives at file scope so that returning from a menu —
// which repaints everything anyway — can adopt the current status instead of
// wiping the screen a second time one tick later.
static wl_status_t clock_last_status = WL_NO_SHIELD;

// Full repaint — only worth doing when the static chrome has to change.
static void display_status() {
    tft.fillScreen(COL_BG);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("v" SEED_VERSION, 8, HDR_Y, 2);
    tft.drawFastHLine(8, 20, tft.width() - 16, COL_RULE);
    display_force = true;  // the wipe took every field with it
    clock_badge_drawn = -1;
    display_tick();
}

// An unacknowledged critical message makes the hairline under the header
// breathe: its colour ramps from the ordinary rule to a dim red and back, one
// cycle every two seconds.
//
// Breathing rather than blinking is the whole point. A blink is a hard edge
// that the eye keeps re-detecting, which is right for an alarm demanding an
// action in the next second and wrong for a device sitting on a shelf saying
// "there is something here for you". The ramp reads as present rather than
// urgent, and it never goes dark, so the rule never looks broken.
//
// Cost is one drawFastHLine of 304 pixels per step — about 0.25ms of SPI at
// 40MHz, twelve times a second, and nothing at all when no critical is
// outstanding. It draws over the clock face only; every other screen owns its
// own header.
#define RULE_BREATHE_MS   80    // one step
#define RULE_BREATHE_CYCLE 2000 // full ramp up and back down
#define RULE_BREATHE_MAX  160   // peak blend of COL_CRIT over COL_RULE

static void clock_rule_tick() {
    static unsigned long last_step = 0;
    static bool was_breathing = false;

    // The step gate comes first so that the notification store is asked once
    // every 80ms rather than on every one of loop()'s hundred passes a second.
    if (millis() - last_step < RULE_BREATHE_MS) return;
    last_step = millis();

    if (!notify_crit_unread()) {
        // Put the plain rule back exactly once, on the edge.
        if (was_breathing) {
            was_breathing = false;
            tft.drawFastHLine(8, 20, tft.width() - 16, COL_RULE);
        }
        return;
    }
    was_breathing = true;

    unsigned long pos = millis() % RULE_BREATHE_CYCLE;
    unsigned long half = RULE_BREATHE_CYCLE / 2;
    unsigned long up = (pos < half) ? pos : (RULE_BREATHE_CYCLE - pos);
    uint8_t alpha = (uint8_t)(up * RULE_BREATHE_MAX / half);
    tft.drawFastHLine(8, 20, tft.width() - 16,
                      tft.alphaBlend(alpha, COL_CRIT, COL_RULE));
}

// ===== Auth =====

// Call only after wifi_setup(): the token generated on a device's first boot
// gates /firmware/upload, which is arbitrary code execution, and it is then
// persisted for the life of the node. esp_random() is only a hardware RNG once
// the RF subsystem is running — before that it is a PRNG from a predictable
// seed. The AP password in ap_generate_password() is generated after RF for
// the same reason. An existing token in SPIFFS is always kept as-is.
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
// its password is random per raise.
//
// It used to run permanently in WIFI_AP_STA with the password hardcoded as a
// #define in a public repo. Combined with handle_wifi_page(), which hands the
// auth token to any client on the AP subnet, that gave anyone within radio
// range of a seed sitting inside the owner's LAN a token — and the token is
// POST /firmware/upload, i.e. arbitrary code on a box behind the firewall.
// The other seeds still ship that pattern and need the same treatment.

// How long the user key must be held to raise the AP.
#define AP_KEY_HOLD_MS 3000

// The AP subnet is pinned rather than left on the ESP-IDF default of
// 192.168.4.1/24. from_setup_ap() decides "this client came in over the setup
// AP" by matching the first three octets, so an AP subnet that a STA network
// might also use turns that test into a false positive reachable from the LAN.
// 172.31.157.0/24 sits clear of every common consumer-router default
// (192.168.0/1/2/4/8/10/100.0/24, 10.0.0-1.0/24, 172.16.0.0/24) and of the /20s
// a default AWS VPC carves out of 172.31.0.0/16.
#define AP_IP_A 172
#define AP_IP_B  31
#define AP_IP_C 157
#define AP_IP_D   1

// Whether the STA link has been down since the AP came up. The teardown is
// edge-triggered on that transition, so raising the AP while already online
// (to move the node to a different network) does not immediately undo itself.
static bool ap_seen_sta_down = false;

// One session's password. No lookalike glyphs — this gets typed off a 170px
// screen; 12 chars out of a 32-symbol alphabet is 60 bits.
static String ap_generate_password() {
    static const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";
    const uint32_t n = sizeof(charset) - 1;
    char buf[13];
    for (size_t i = 0; i < sizeof(buf) - 1; i++) {
        buf[i] = charset[esp_random() % n];
    }
    buf[sizeof(buf) - 1] = '\0';
    return String(buf);
}

// The responder binds to the interfaces that exist when it starts, so it is
// restarted whenever the AP interface appears or goes away.
static void mdns_restart() {
    MDNS.end();
    if (MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("seed", "tcp", HTTP_PORT);
    }
}

static void ap_start(bool temporary) {
    if (ap_active) return;
    // RF is up by now (wifi_setup() has run), which is what makes esp_random()
    // a hardware RNG rather than a seeded PRNG — same reason token_load() is
    // deferred until after wifi_setup().
    ap_password = ap_generate_password();
    WiFi.mode(WIFI_AP_STA);
    // Pin the subnet before the AP comes up. If this ever fails the AP falls
    // back to the default 192.168.4.1/24; from_setup_ap() reads the live
    // softAPIP() so it stays self-consistent, just on a collision-prone subnet.
    if (!WiFi.softAPConfig(IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(255, 255, 255, 0))) {
        event_add("setup AP: subnet pin failed, using default");
    }
    if (!WiFi.softAP(ap_ssid.c_str(), ap_password.c_str())) {
        ap_password = "";
        WiFi.mode(WIFI_STA);
        event_add("setup AP failed to start");
        return;
    }
    ap_active = true;
    ap_temporary = temporary;
    ap_expires_at = millis() + AP_SESSION_MS;
    ap_seen_sta_down = (WiFi.status() != WL_CONNECTED);
    event_add("setup AP up: %s", ap_ssid.c_str());  // SSID only, never the password
    mdns_restart();
    display_force = true;  // the password is only readable off the screen
}

static void ap_stop() {
    if (!ap_active) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap_active = false;
    ap_password = "";
    ap_temporary = false;
    ap_seen_sta_down = false;
    event_add("setup AP down");
    mdns_restart();
    display_force = true;
}

// Provisioning is finished the moment the STA link comes up, so the AP goes
// away on its own. Losing WiFi later does NOT bring it back: a node that drops
// off the network must not silently start offering a way in.
static void ap_poll() {
    if (!ap_active) return;
    if (WiFi.status() != WL_CONNECTED) {
        ap_seen_sta_down = true;
    } else if (ap_seen_sta_down) {
        ap_stop();
        return;
    }
    // Nobody reprovisioned in time: close the window the gesture opened. The
    // subtraction is rollover-safe as long as it stays in signed arithmetic.
    if (ap_temporary && (long)(millis() - ap_expires_at) >= 0) {
        event_add("setup AP expired");
        ap_stop();
    }
}

// The only way to raise the AP after boot: hold the user key for three seconds.
// Physical presence, not network reachability, is what authorises provisioning,
// so nothing remote can ask for the AP back. Contact bounce reads as a release
// and restarts the timer, which is debounce enough for a hold this long.
static void ap_key_poll() {
    static unsigned long held_since = 0;
    static bool fired = false;

    if (digitalRead(PIN_USER_KEY) != LOW) {  // active low
        held_since = 0;
        fired = false;
        return;
    }
    if (held_since == 0) held_since = millis();
    if (fired || millis() - held_since < AP_KEY_HOLD_MS) return;
    fired = true;
    if (!ap_active) ap_start(true);  // time-boxed: see AP_SESSION_MS
}

static void wifi_setup() {
    String suffix = get_mac_suffix();
    ap_ssid = "Seed-" + suffix;
    mdns_name = "seed-" + suffix;
    mdns_name.toLowerCase();

    WiFi.mode(WIFI_STA);

    // Start SNTP with the stored TZ before associating: the daemon keeps
    // retrying on its own, so the clock also syncs after a later reconnect
    // instead of only on a successful boot-time connect.
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

    // Nothing to provision if the stored credentials already got us on the
    // network: in that case the AP is never started at all.
    if (WiFi.status() != WL_CONNECTED) ap_start(false);

    mdns_restart();
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
    // Only reported when the AP is genuinely running. The AP password is
    // deliberately absent: the device screen is its only channel.
    doc["ap_active"] = ap_active;
    if (ap_active) {
        doc["ap_ssid"] = ap_ssid;
        doc["ap_ip"] = WiFi.softAPIP().toString();
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
            "{\"error\":\"invalid TZ: 1-63 printable ASCII chars, no spaces\"}");
        return;
    }
    if (!write_spiffs_file(TZ_FILE, tz)) {
        request->send(500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }
    tz_string = tz;
    tz_apply();
    display_force = true;  // loop() repaints on the next tick
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
        ota_last_chunk_ms = millis();

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

    // Every chunk, including ones that then fail to write: this stamp is what
    // tells the watchdog in loop() that the transfer is still alive.
    ota_last_chunk_ms = millis();

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
        ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

    String s = "# ESP32 Seed — LilyGO T-Embed CC1101\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    if (ap_active) s += "AP: " + ap_ssid + " (setup mode; password is on the device screen)\n";
    s += "\n";
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
    s += "- Flashing over USB-Serial/JTAG needs `--after watchdog_reset`\n";
    s += "- The clock runs on UTC until POST /clock/tz stores a POSIX TZ string in SPIFFS\n";
    s += "- The setup AP is only up during provisioning: hold the user key (GPIO6) 3s to\n";
    s += "  raise it, with a fresh random password shown on the device screen. A\n";
    s += "  gesture-raised AP closes itself after 10 minutes if nothing reprovisions\n";
    s += "- POST /wifi/config needs the token unless the request comes over that AP\n";
    s += "- Paths match exactly: `/health/` is not `/health` and returns 404. Nothing\n";
    s += "  here generates a trailing slash, but a client that appends one will break.\n";
    s += "  This replaced prefix matching, under which `/ir/tvbgone` also answered\n";
    s += "  `/ir/tvbgone/stop` and started a blast instead of aborting one\n";
    s += "- An OTA upload whose connection dies is torn down after 30s without data,\n";
    s += "  so a dropped transfer no longer blocks every later one until reboot\n";
    s += "- The encoder button opens an on-device menu (messages, TV-B-Gone, setup AP,\n";
    s += "  info). TV-B-Gone holds the three region blasts and a by-brand list of the\n";
    s += "  nine named codes. It drives the same code paths as the API and has no\n";
    s += "  endpoints of its own, so a job started here shows up in\n";
    s += "  GET /ir/tvbgone/status like any other\n";
    s += "- POST /notify makes this a pager: the message appears on the screen at once\n";
    s += "  if the device is idle, and the clock face carries an unread count until\n";
    s += "  somebody acknowledges it with the knob or POST /notify/ack\n\n";
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

// True only for a request that arrived over the provisioning AP while that AP
// is actually up. Both halves matter: once the AP is down softAPIP() is
// 0.0.0.0 and the subnet test is meaningless, and belonging to some subnet
// proves nothing on its own. Reaching the node this way costs an attacker
// physical presence plus the per-boot password shown on the screen, which is
// why this is the one path allowed to skip the token.
//
// The /24 match is only as good as the AP subnet being one no STA network
// uses: ours is pinned to 172.31.157.0/24 in ap_start() precisely so a LAN
// client cannot land in it and pass this test while the AP happens to be up.
static bool from_setup_ap(AsyncWebServerRequest *request) {
    if (!ap_active) return false;
    IPAddress client_ip;
    client_ip.fromString(request->client()->remoteIP().toString());
    IPAddress ap_ip = WiFi.softAPIP();
    return client_ip[0] == ap_ip[0] && client_ip[1] == ap_ip[1] &&
           client_ip[2] == ap_ip[2];
}

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
    if (from_setup_ap(request)) html += "<p>Token: " + auth_token + "</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
}

static void handle_wifi_post(AsyncWebServerRequest *request) {
    // Rewriting the credentials moves the node to a different network, so off
    // the provisioning AP this needs the token like any other mutating call —
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

// ===== Skills =====
#include "skills/gpio.cpp"
#include "skills/serial.cpp"
#include "skills/ir.cpp"
#include "skills/notify.cpp"

static void skills_init() {
    skill_gpio_init();
    skill_serial_init();
    skill_ir_init();
    skill_notify_init();
}

// ===== On-device UI =====
//
// Included after the skills because it drives them: the menu starts an IR
// blast through the skill's own entry point. It is not a skill itself — it
// registers no routes and it owns screens rather than endpoints.
#include "ui.h"

// ===== Routes =====

// Every route is registered with AsyncURIMatcher::exact(), and any route added
// later must be too.
//
// The library's default is AsyncURIMatcher BackwardCompatible, which matches
// the regex ^{uri}(/.*)?$ — so a handler for /clock also answers /clock/tz, and
// whichever of the two is registered first wins. That cost this firmware a
// working POST /ir/tvbgone/stop: the stop request landed on /ir/tvbgone and
// started a blast instead of aborting one. Nothing here serves a subtree, so
// exact matching is what every route in the seed actually wants; registration
// order then stops being load-bearing.
static void setup_routes() {
    server.on(AsyncURIMatcher::exact("/health"), HTTP_GET, handle_health);
    server.on(AsyncURIMatcher::exact("/capabilities"), HTTP_GET, handle_capabilities);
    server.on(AsyncURIMatcher::exact("/config.md"), HTTP_GET, handle_config_get);
    server.on(AsyncURIMatcher::exact("/config.md"), HTTP_POST, handle_config_post, NULL, handle_body_collect);
    server.on(AsyncURIMatcher::exact("/events"), HTTP_GET, handle_events);
    server.on(AsyncURIMatcher::exact("/clock"), HTTP_GET, handle_clock_get);
    server.on(AsyncURIMatcher::exact("/clock/tz"), HTTP_POST, handle_clock_tz, NULL, handle_body_collect);
    server.on(AsyncURIMatcher::exact("/firmware/version"), HTTP_GET, handle_firmware_version);
    server.on(AsyncURIMatcher::exact("/firmware/upload"), HTTP_POST, handle_firmware_upload, NULL, handle_firmware_upload_body);
    server.on(AsyncURIMatcher::exact("/firmware/apply"), HTTP_POST, handle_firmware_apply);
    server.on(AsyncURIMatcher::exact("/firmware/confirm"), HTTP_POST, handle_firmware_confirm);
    server.on(AsyncURIMatcher::exact("/firmware/rollback"), HTTP_POST, handle_firmware_rollback);
    server.on(AsyncURIMatcher::exact("/skill"), HTTP_GET, handle_skill);
    server.on(AsyncURIMatcher::exact("/"), HTTP_GET, handle_wifi_page);
    server.on(AsyncURIMatcher::exact("/wifi/config"), HTTP_POST, handle_wifi_post);

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

    // Read-only: the hold-to-raise-AP gesture is the only user of this pin.
    pinMode(PIN_USER_KEY, INPUT_PULLUP);

    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    hw_probe();       // I2C + CC1101 probe (before the display claims the SPI bus)
    display_init();
    tz_load();        // before wifi_setup(): configTzTime() needs the TZ string
    wifi_setup();
    token_load();     // after wifi_setup(): needs RF up for a real RNG
    skills_init();
    ui_init();        // after skills_init(): the menu drives the IR skill
    setup_routes();
    server.begin();
    display_status();

    Serial.printf("\nESP32 Seed v%s (T-Embed CC1101)\n", SEED_VERSION);
    Serial.printf("Token: %s\n", auth_token.c_str());
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("http://%s:%d/health\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    if (ap_active) {
        // Password intentionally not printed: it lives on the screen only.
        Serial.printf("http://%s:%d/health  (setup AP: %s)\n",
            WiFi.softAPIP().toString().c_str(), HTTP_PORT, ap_ssid.c_str());
    }

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

    // Abandoned OTA upload.
    //
    // handle_firmware_upload_body() clears ota_in_progress on exactly three
    // paths: a failed Update.begin(), a failed write, and the final chunk. A
    // connection that dies mid-body reaches none of them, so the flag stays
    // set for the life of the boot and every later upload is refused with
    // "already in progress" — recoverable until now only by rebooting the
    // node, which on a half-flashed slot means a rollback.
    //
    // Clearing the flag alone would not be enough. UpdateClass::begin()
    // refuses to start while _size is non-zero and answers "already running",
    // so the abandoned Update object has to be torn down too. abort() calls
    // _reset(), which zeroes _size and releases the partition handle, and the
    // next upload then starts clean.
    //
    // 30s: a struggling-but-alive TCP transfer retries with exponential
    // backoff — roughly 1+2+4+8s before a stack gives up on a segment — so
    // this is about double the worst gap a slow link can produce, while a
    // genuinely dead upload frees the flash long before anyone retries by
    // hand. A healthy upload puts chunks milliseconds apart and never comes
    // near it.
    if (ota_in_progress && millis() - ota_last_chunk_ms > OTA_STALL_TIMEOUT_MS) {
        Update.abort();
        ota_in_progress = false;
        ota_upload_started = false;
        ota_upload_error = true;
        snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                 "upload abandoned after %lus without data",
                 (unsigned long)(OTA_STALL_TIMEOUT_MS / 1000));
        event_add("ota upload abandoned after %u bytes, partition released",
                  (unsigned)ota_bytes_written);
    }

    // WiFi reconnect
    static unsigned long last_wifi = 0;
    if (wifi_ssid.length() > 0 && WiFi.status() != WL_CONNECTED &&
        millis() - last_wifi > 30000) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        last_wifi = millis();
    }

    // Retire the provisioning AP once it has done its job, and watch the user
    // key for the gesture that brings it back. Both polls are non-blocking.
    ap_poll();
    ap_key_poll();

    // Starts frames and collects completions; the transmission itself runs in
    // the RMT peripheral. Advances as far as it can each pass and never blocks.
    ir_poll();

    // Expires notifications whose ttl has run out and mirrors the newest few to
    // SPIFFS. The endpoints only ever touch RAM; the flash write is here, on the
    // one task that is allowed to spend milliseconds.
    notify_poll();

    // Encoder and buttons, every pass: input latency is what makes the knob
    // feel attached to the screen. This owns the panel whenever the UI is on a
    // screen other than the clock, and draws only on an actual change.
    ui_poll();

    // Clock tick: at most once a second, and each field only repaints when its
    // text actually changed. The screen is only wiped when connectivity flips.
    // Skipped entirely while a menu is up — ui_enter(UI_CLOCK) repaints the
    // whole clock on the way back, so nothing here has to catch up.
    static unsigned long last_tick = 0;
    if (ui_screen == UI_CLOCK && (display_force || millis() - last_tick >= 1000)) {
        last_tick = millis();
        wl_status_t st = WiFi.status();
        if (st != clock_last_status) {
            clock_last_status = st;
            display_status();
        } else {
            display_tick();
        }
    }

    // The one thing on the clock face that moves faster than once a second, and
    // it only moves while a critical message is unacknowledged. One horizontal
    // line, so it does not need the tick above and must not wait for it.
    if (ui_screen == UI_CLOCK) clock_rule_tick();

    // Keep the fuel gauge reading current — probe_battery() only ran at boot.
    static unsigned long last_battery = 0;
    if (millis() - last_battery > 60000) {
        last_battery = millis();
        battery_refresh();
    }

    // 10ms is the idle cadence, and it is the whole pass's granularity. A blast
    // cannot queue its next frame until it has seen the previous one finish, so
    // at 10ms that rounding is dead air on every one of several hundred frames —
    // seconds of it. 1ms while a job runs costs nothing (delay() yields either
    // way) and takes the scheduler out of the timing budget.
    delay(ir_busy() ? 1 : 10);
}
