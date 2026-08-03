// ESP32 Seed — M5Stack Cardputer ADVANCE (StampS3A / ESP32-S3FN8)
//
// The Cardputer port of the ESP32 seed: just enough to boot, connect, and let
// an AI agent grow it via OTA firmware uploads.
//
// This is the bring-up commit. The ST7789 panel, the TCA8418 keyboard and the
// on-device UI are deliberately NOT driven here — they are the next step, and
// the gpio/serial skills the one after that. What is here is the HTTP API, the
// security baseline (token, provisioning AP, exact route matching) and OTA.
//
// Board specifics vs the other ESP32 seeds in this tree:
//   - ADVANCE, not v1.1. Nothing from the v1.1 pinout transfers: the keyboard
//     moved off the GPIO scan matrix onto a TCA8418 controller on I2C, the I2C
//     bus itself moved to GPIO8/9, and GPIO13/15 — v1.1's keyboard rows — are
//     the EXT 2.54-14P expansion header's UART lines on this board. Firmware
//     written for a v1.1 will drive the expansion header as a key matrix.
//   - 8MB of quad flash and NO PSRAM. The OTA slots are 0x330000 each rather
//     than the 4MB the 16MB boards get, and mem_mb is internal SRAM only.
//   - There is no power-latch pin to hold. The T-Embed seed drives GPIO15 HIGH
//     first thing in setup() to stay alive on battery; on this board GPIO15 is
//     an expansion-header UART line, and doing that would be actively wrong.
//   - No sub-GHz radio and no fuel gauge on the mainboard: the CC1101 and
//     BQ27220 probes the T-Embed seed runs have nothing to find here. Battery
//     voltage is an ADC on GPIO10, left unread until its divider is verified.
//   - The mainboard I2C bus carries exactly three devices — the keyboard
//     controller, the audio codec and the IMU — so it is scanned and reported
//     but never reconfigured. A fourth address answering, 0x43, means a
//     detachable cap is fitted; see the Cap LoRa-1262 block in the pin map.
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
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>

// ===== Configuration =====
#define SEED_VERSION        "0.1.0"
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

// ===== Cardputer ADVANCE pin map =====
//
// From the M5Stack Cardputer-Adv documentation. Everything below is claimed by
// something on the mainboard; nothing here is driven by this commit.

// ST7789V2 240x135 IPS on SPI. GPIO38 is the backlight enable and also gates
// the RGB LED supply, so it has to go HIGH before either can light up.
#define PIN_TFT_RST     33
#define PIN_TFT_DC      34
#define PIN_TFT_MOSI    35
#define PIN_TFT_SCLK    36
#define PIN_TFT_CS      37
#define PIN_TFT_BL      38

// microSD on its own SPI bus. 14/39/40 are also on the 2x7 expansion bus, so a
// cap that uses them shares the wires with the card.
#define PIN_SD_CS       12
#define PIN_SD_MOSI     14
#define PIN_SD_MISO     39
#define PIN_SD_SCLK     40

// The one I2C bus. Per M5Stack's Cardputer-Adv pinmap the mainboard puts
// exactly three devices on it: the TCA8418RTWR keyboard controller, the ES8311
// audio codec and the BMI270 IMU. Explicitly 8/9 — see the variant note in
// platformio.ini for why this must never be left to pins_arduino.h. The same
// two lines are brought out on the EXT 2.54-14P header (pins 8 and 10), which
// is how a cap joins this bus.
#define PIN_I2C_SDA      8
#define PIN_I2C_SCL      9
#define PIN_KB_INT      11  // TCA8418 interrupt, active low

// ES8311 audio codec, I2S side
#define PIN_I2S_SCLK    41
#define PIN_I2S_DSDIN   42
#define PIN_I2S_LRCK    43
#define PIN_I2S_ASDOUT  46

#define PIN_IR_TX       44
#define PIN_VBAT_ADC    10  // battery divider; the ratio is unverified, so unread

// UART broken out on the EXT 2.54-14P expansion header. Nothing on the
// mainboard sits on these two lines; whatever answers here is whatever cap is
// plugged in. Bare, the header is idle.
//
// These are named from the MAINBOARD's point of view — the ESP32 receives on 15
// and transmits on 13 — and that is deliberately the opposite of how M5Stack's
// Cardputer-Adv pinmap labels them. That table calls EXT pin 12 "G13
// (UART_RX)" and pin 14 "G15 (UART_TX)", i.e. from the accessory's side: the
// Cap LoRa-1262 page independently labels the same two lines GPS_RX and GPS_TX
// in that order. Working code settles the direction — both M5Stack's own Cap
// LoRa868/LoRa-1262 Arduino example and the third-party Cardputer-Adv LoRa test
// firmware open the port as
//     Serial1.begin(115200, SERIAL_8N1, /*rx=*/15, /*tx=*/13)
// so the ESP32's receiver is on 15. Reading M5Stack's label as an ESP32-side
// name gives a UART that is wired backwards and simply stays silent.
#define PIN_EXT_UART_RX 15
#define PIN_EXT_UART_TX 13

// I2C addresses of the three devices soldered to the mainboard. Nothing else is
// on this bus unless a cap is fitted.
#define TCA8418_ADDR    0x34  // keyboard controller
#define ES8311_ADDR     0x18  // audio codec
// BMI270 IMU. M5Stack's own two documents disagree: the product I2C address
// index lists the BMI270 at 0x68 AND 0x69 (the part's SDO pin picks one), while
// the Cardputer-Adv page gives no address at all. An I2C scan of the owner's
// unit answered 0x18, 0x34, 0x43 and 0x69 — no 0x68 — so this board straps it
// high. Observation outranks the doc; 0x68 stays in the scan table below only as
// an unconfirmed candidate.
#define BMI270_ADDR     0x69

// ===== M5Stack Cap LoRa-1262 — DETACHABLE, not part of the mainboard =====
//
// A cap that clips onto the EXT 2.54-14P header. None of it is soldered to this
// board, so every field below is reported only when the cap is actually
// detected at boot; see hw_probe().
//
// Why 0x43 is the discriminator: M5Stack's Cardputer-Adv pinmap puts exactly
// three devices on the mainboard I2C bus (TCA8418RTWR, ES8311, BMI270) and no
// IO expander of any kind. The PI4IOE5V6408 lives on the cap and reaches the
// bus through EXT header pins 8/10 (G8/G9), so an answer at 0x43 cannot come
// from the mainboard — it is a cap. M5Stack's own Cap LoRa868/LoRa-1262 example
// detects the cap exactly this way, by probing the expander. Do not "simplify"
// this into an unconditional claim: on a bare ADVANCE it would be false.
//
// The expander's P0 drives the RF antenna switch and must be set high before
// the SX1262 will transmit. This seed does not do that, because it does not
// drive the radio at all.
#define PI4IOE_ADDR     0x43  // PI4IOE5V6408 IO expander, on the cap

// SX1262 on the EXT header's SPI fan-out. SCK/MOSI/MISO are the microSD bus
// pins doing double duty, so the card and the radio share wires.
#define CAP_LORA_SCK    40
#define CAP_LORA_MOSI   14
#define CAP_LORA_MISO   39
#define CAP_LORA_NSS     5
#define CAP_LORA_DIO1    4
#define CAP_LORA_BUSY    6
#define CAP_LORA_RST     3

// ATGM336H-6N GNSS (AT6668 core) on the header's UART, at PIN_EXT_UART_RX/TX.
#define CAP_GNSS_BAUD   115200

// Pins genuinely free for user I/O on the Cardputer ADVANCE. This is a short
// list because the board is dense, and it is deliberately short: a pin wrongly
// called safe lets an agent take down a bus it cannot see. Single source of
// truth — /capabilities and the gpio skill both derive from it.
//
//   1, 2   HY2.0-4P Grove port. The only external connector with nothing of
//          the board's own on it.
//   7      Assigned to nothing on the mainboard. It is also not brought out to
//          any connector, so it is safe to drive and does nothing visible.
//
// Excluded, by group:
//   3,4,5,6,13,14,15,39,40  the EXT 2.54-14P expansion header. Its SPI fan-out
//                           is 3=RESET, 4=INT, 5=CS, 6=BUSY, 14=MOSI, 39=MISO,
//                           40=SCK, and 13/15 are its UART; 14/39/40 are the
//                           microSD bus at the same time. The pins belong to
//                           the header, not to any one accessory — a Cap
//                           LoRa-1262 drives all nine, and a bare board none.
//   8,9,11                  I2C plus the keyboard controller's interrupt.
//   10                      battery sense ADC.
//   12                      microSD chip select.
//   33-38                   ST7789 panel; 38 also gates the RGB LED supply.
//   41,42,43,46             ES8311 audio codec (I2S). 43 is nominally UART0 TX
//                           on an S3 and is free on the other two seeds in this
//                           tree — here it is the codec's word clock.
//   44                      IR transmitter, likewise not the free UART0 RX it
//                           is elsewhere.
//   19,20                   native USB. Driving these ends the console session
//                           and the recovery path with it.
//   0,45,47,48              strapping pins and module-internal lines.
//   26-32                   SPI flash. 22-25 do not exist on the ESP32-S3.
//   16,17,18,21             UNCLASSIFIED — excluded because we do not know, not
//                           because we know. M5Stack's published Cardputer-Adv
//                           pinmap lists no assignment for these four, yet the
//                           board carries a microphone and an NS4150B amplifier
//                           whose pins appear nowhere in that document. The odds
//                           that four undocumented pins and two undocumented
//                           peripherals are unrelated are poor, so guessing is
//                           worse than excluding. This is a gap to close with a
//                           schematic or a meter, not a settled fact: whoever
//                           establishes what they are should move them into a
//                           named group above or onto the safe list.
static const int gpio_safe_pins[] = {1, 2, 7};
static const int gpio_safe_pins_count =
    sizeof(gpio_safe_pins) / sizeof(gpio_safe_pins[0]);

// Used by skills/gpio.cpp, which is not part of this commit yet.
__attribute__((unused))
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
//
// No skills ship in this commit. The scaffold is here so that dropping a
// skills/ directory in later needs no change to anything above it.

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

// Called by each skill's own init(), none of which exist yet.
__attribute__((unused))
static int skill_register(const Skill *skill) {
    if (g_skill_count >= MAX_SKILLS) return -1;
    g_skills[g_skill_count++] = skill;
    return 0;
}

// ===== Hardware Probe (run once at boot, cached) =====

// Known I2C device addresses.
//
// The table has two halves and they are not the same kind of statement, so the
// entries carry a flag saying which they are.
//
//   confirmed == true   The part is documented at this address on hardware this
//                       seed actually runs on, and the source is named. Nothing
//                       goes in here on the strength of an address alone.
//   confirmed == false  A guess. Several unrelated parts share most 7-bit
//                       addresses, and this half is a convenience list of the
//                       usual suspects for whatever an agent hangs off the
//                       Grove port. /capabilities reports these under a
//                       separate key so they cannot be read as identifications.
//
// An address may only move from the second half to the first with a document or
// a probe behind it. Three wrong hardware claims have already been written into
// this file from address lore alone.
struct I2CDevice {
    uint8_t addr;
    const char *name;
    bool confirmed;
};

static const I2CDevice known_i2c[] = {
    // Mainboard — M5Stack's Cardputer-Adv pinmap lists these three and no others.
    {TCA8418_ADDR, "TCA8418RTWR keyboard controller (mainboard)", true},
    {ES8311_ADDR,  "ES8311 audio codec (mainboard)", true},
    {BMI270_ADDR,  "BMI270 IMU (mainboard)", true},
    // Cap LoRa-1262 — detachable. Presence of this address is what tells the
    // firmware a cap is fitted; see the cap block in the pin map.
    {PI4IOE_ADDR,  "PI4IOE5V6408 IO expander (Cap LoRa-1262, detachable)", true},

    // --- address-only guesses from here down ---
    // 0x68 is where M5Stack's product I2C index says a BMI270 may sit, but this
    // board answers on 0x69 and 0x68 is shared with several common parts, so it
    // stays a guess rather than a second IMU entry.
    {0x68, "MPU6050 / DS3231 / BMI270 at its alternate address", false},
    {0x3C, "SSD1306 OLED 128x64", false},
    {0x3D, "SSD1306 OLED 128x64 (alt)", false},
    {0x27, "PCF8574 LCD/IO", false},
    {0x20, "PCF8574 IO expander", false},
    {0x48, "ADS1115 ADC / TMP102", false},
    {0x49, "ADS1115 ADC (alt)", false},
    {0x50, "AT24C32 EEPROM", false},
    {0x57, "MAX30102 pulse oximeter", false},
    {0x76, "BME280 / BMP280 / MS5611", false},
    {0x77, "BME280 / BMP085 (alt)", false},
    {0x29, "VL53L0X ToF / TSL2591 lux", false},
    {0x39, "TSL2561 lux", false},
    {0x40, "INA219 power / HDC1080 / SHT30", false},
    {0x44, "SHT30 / SHT31 / PI4IOE5V6408-2 IO expander", false},
    {0x5A, "MLX90614 IR temp / CCS811", false},
    {0x5B, "CCS811 air quality (alt)", false},
    {0x60, "SI1145 UV / ATECC608", false},
    {0x62, "SCD30 CO2", false},
    {0x70, "TCA9548A I2C mux", false},
    {0x75, "BME688", false},
    {0x23, "BH1750 lux", false},
    {0x53, "ADXL345 accel", false},
    {0x1E, "HMC5883L compass", false},
    {0x0D, "QMC5883L compass", false},
    {0, NULL, false}
};

static const I2CDevice *i2c_identify(uint8_t addr) {
    for (int i = 0; known_i2c[i].name; i++) {
        if (known_i2c[i].addr == addr) return &known_i2c[i];
    }
    return NULL;
}

// Probe results (cached at boot)
#define MAX_I2C_FOUND 16

struct I2CFound {
    uint8_t addr;
    const char *name;
    bool confirmed;
};

struct HWProbe {
    // Chip
    const char *chip_model;
    uint8_t chip_revision;
    uint32_t flash_size;
    uint32_t flash_speed;
    uint32_t psram_size;
    float temp_c;

    // I2C bus (SDA=8, SCL=9)
    I2CFound i2c0[MAX_I2C_FOUND];
    int i2c0_count;

    // Board guess
    const char *board;

    // True when the boot scan saw the Cap LoRa-1262's IO expander. Detected,
    // never assumed: the cap unplugs.
    bool cap_lora1262;
};

static HWProbe hw;

static void i2c_scan(TwoWire &bus, I2CFound *results, int &count) {
    count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0 && count < MAX_I2C_FOUND) {
            const I2CDevice *known = i2c_identify(addr);
            results[count].addr = addr;
            results[count].name = known ? known->name : NULL;
            results[count].confirmed = known ? known->confirmed : false;
            count++;
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
    hw.psram_size = ESP.getPsramSize();  // zero on this board, reported anyway
    hw.temp_c = temperatureRead();

    // Single I2C bus, pins given explicitly: the generic esp32s3 variant has no
    // opinion about SDA/SCL, and the m5stack_cardputer variant has the wrong one.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    i2c_scan(Wire, hw.i2c0, hw.i2c0_count);

    // Two independent questions, answered from the same scan.
    //
    // 1. Which board is this? The keyboard controller alone. It is what makes
    //    an ADVANCE an ADVANCE — a v1.1 scans its matrix on bare GPIOs and has
    //    nothing at 0x34 — and it is the only mainboard signal here that
    //    discriminates. The expander at 0x43 must NOT gate this: it is not a
    //    mainboard device at all, so requiring it would call every ADVANCE
    //    without a cap on it "generic".
    //
    // 2. Is a cap fitted? The PI4IOE5V6408 at 0x43. M5Stack's Cardputer-Adv
    //    pinmap puts three devices on the mainboard I2C bus — TCA8418RTWR,
    //    ES8311, BMI270 — and no IO expander of any kind; the PI4IOE5V6408 is
    //    on the Cap LoRa-1262, reaching the bus through EXT header pins 8/10.
    //    So an answer at 0x43 is a cap, and its absence is a bare header.
    //    M5Stack's own Cap LoRa868/LoRa-1262 example probes the same expander
    //    for the same purpose.
    //
    // This is a positive detection in both directions and it decides what
    // /capabilities is allowed to say. It does not gate access to anything, and
    // it is not a claim that this firmware drives the cap — it drives nothing on
    // that header. Whatever the scan found is reported verbatim in i2c_devices
    // regardless.
    bool keyboard = false;
    bool cap_expander = false;
    for (int i = 0; i < hw.i2c0_count; i++) {
        if (hw.i2c0[i].addr == TCA8418_ADDR) keyboard = true;
        if (hw.i2c0[i].addr == PI4IOE_ADDR) cap_expander = true;
    }
    hw.board = keyboard ? "M5Stack Cardputer ADVANCE" : "ESP32-S3 (generic)";
    hw.cap_lora1262 = cap_expander;

    Serial.printf("[probe] board: %s\n", hw.board);
    Serial.printf("[probe] cap: %s\n", hw.cap_lora1262
        ? "M5Stack Cap LoRa-1262 (PI4IOE5V6408 answered)"
        : "none detected");
    Serial.printf("[probe] temp: %.1fC, flash: %uMB, psram: %uKB\n",
        hw.temp_c, (unsigned)(hw.flash_size / 1024 / 1024),
        (unsigned)(hw.psram_size / 1024));
    Serial.printf("[probe] i2c: %d devices\n", hw.i2c0_count);
}

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
static String auth_token = "";
static String ap_ssid = "";
static String mdns_name = "";
static unsigned long boot_time = 0;

// Provisioning AP state. The password is rolled on every raise and exists only
// in this variable — never persisted, never sent anywhere over the network.
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
// The request that owns the in-flight transfer. See the ownership guard at the
// top of handle_firmware_upload_body() for why the flags above are not enough
// on their own.
static AsyncWebServerRequest *ota_owner = nullptr;
// millis() of the last body chunk received, for the abandoned-upload watchdog
// in loop(). Only meaningful while ota_in_progress.
static unsigned long ota_last_chunk_ms = 0;
static volatile bool pending_restart = false;
static volatile bool pending_rollback = false;

// ===== Utilities =====

// The last two bytes of the factory MAC, read out of eFuse.
//
// Deliberately not WiFi.macAddress(). On arduino-esp32 3.x that resolves to
// NetworkInterface::macAddress(), which forwards to esp_netif_get_mac() and
// returns NULL — leaving the caller's buffer untouched — while the STA netif
// is not yet ready. WiFi.mode(WIFI_STA) returning does not close that window:
// the netif is started off the event loop, so the call can still fail right
// after it. The bytes then came back as whatever was on the stack, which is
// why the node named itself Seed-A5A5 on one build and Seed-0000 on the next.
// Upstream tracks the symptom as arduino-esp32 issue 9509.
//
// esp_read_mac() reads the same eFuse the WiFi stack derives its own STA MAC
// from and needs none of that stack initialised, so the answer is stable from
// the first line of setup() and the identity no longer depends on call order.
// The buffer is zeroed and the status checked so that a failure is a loud,
// repeatable Seed-0000 rather than a different garbage name every boot.
static String get_mac_suffix() {
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK)
        Serial.printf("esp_read_mac failed: %s\n", esp_err_to_name(err));
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

// One session's password. No lookalike glyphs — this gets typed off a serial
// console or, once the panel is driven, off a 240px screen; 12 chars out of a
// 32-symbol alphabet is 60 bits.
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

// Raise the provisioning softAP. `manual` is reserved for the press-to-raise
// gesture that arrives with the keyboard: this commit drives no input device,
// so there is no way to ask for the AP and this is called only from
// wifi_setup() when the stored credentials fail. That also means an AP raised
// here is not time-boxed — with no working credentials it is the only way in.
static void ap_start(bool manual) {
    if (ap_active) return;  // idempotent: a re-raise must not re-roll the password
    (void)manual;
    // RF is up by now (wifi_setup() has run), which is what makes esp_random()
    // a hardware RNG rather than a seeded PRNG — same reason token_load() is
    // deferred until after wifi_setup().
    ap_password = ap_generate_password();
    WiFi.mode(WIFI_AP_STA);
    // Pin the subnet before the AP comes up, clear of the owner's LAN, so a LAN
    // client can never land in it and pass from_setup_ap()'s /24 test. If the pin
    // fails the AP would fall back to the default 192.168.4.1/24, where a LAN
    // client could share that subnet and slip past from_setup_ap() — so refuse to
    // raise the AP at all rather than expose the token-skipping path over the LAN.
    if (!WiFi.softAPConfig(IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(255, 255, 255, 0))) {
        ap_password = "";
        WiFi.mode(WIFI_STA);
        event_add("setup AP: subnet pin failed, not raising AP");
        return;
    }
    if (!WiFi.softAP(ap_ssid.c_str(), ap_password.c_str())) {
        ap_password = "";
        WiFi.mode(WIFI_STA);
        event_add("setup AP failed to start");
        return;
    }
    ap_active = true;
    ap_seen_sta_down = (WiFi.status() != WL_CONNECTED);
    event_add("setup AP up: %s", ap_ssid.c_str());  // SSID only, never the password
    mdns_restart();
}

static void ap_stop() {
    if (!ap_active) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap_active = false;
    ap_password = "";
    ap_seen_sta_down = false;
    event_add("setup AP down");
    mdns_restart();
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
    }
}

static void wifi_setup() {
    // The whole identity of the node — its setup-AP SSID and its mDNS hostname
    // — hangs off these four characters. They come from eFuse rather than from
    // the WiFi stack (see get_mac_suffix), so this is free to sit ahead of the
    // radio and is already settled by the time anything advertises it.
    String suffix = get_mac_suffix();
    ap_ssid = "Seed-" + suffix;
    mdns_name = "seed-" + suffix;
    mdns_name.toLowerCase();

    // STA-only: the AP is not started up-front. It comes up only if the stored
    // credentials fail to get us on the network (see below), so a provisioned
    // node running on WiFi never offers a way in.
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

// mem_mb per docs/capabilities-spec.md: total RAM in megabytes. This board has
// no PSRAM, so it is internal SRAM only — about 390KB of heap, which rounds to
// zero. The spec makes mem_mb a MUST and a node reporting 0MB of RAM reads as
// broken rather than small, so the value is floored at 1.
static uint32_t total_mem_mb() {
    uint64_t bytes = (uint64_t)ESP.getHeapSize() + (uint64_t)ESP.getPsramSize();
    uint32_t mb = (uint32_t)((bytes + 512 * 1024) / (1024 * 1024));
    return mb > 0 ? mb : 1;
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

    // Required by docs/capabilities-spec.md
    doc["arch"] = "esp32-s3";
    doc["mem_mb"] = total_mem_mb();
    doc["hostname"] = mdns_name;
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;

    // Optional, spec-named
    doc["board_model"] = hw.board;
    doc["cpu_model"] = hw.chip_model;
    doc["os"] = "FreeRTOS";
    doc["cpus"] = ESP.getChipCores();
    doc["temp_c"] = serialized(String(hw.temp_c, 1));
    doc["has_wifi"] = true;
    doc["has_bluetooth"] = true;

    // Board-specific extras
    doc["chip_revision"] = hw.chip_revision;
    doc["cpu_mhz"] = ESP.getCpuFreqMHz();
    doc["free_heap"] = (unsigned long)ESP.getFreeHeap();
    doc["min_free_heap"] = (unsigned long)ESP.getMinFreeHeap();
    doc["flash_mb"] = (unsigned long)(hw.flash_size / 1024 / 1024);
    doc["flash_mhz"] = (unsigned long)(hw.flash_speed / 1000000);
    doc["has_psram"] = hw.psram_size > 0;

    // Peripherals present but not driven by this firmware. Reported so an agent
    // growing the node knows what is there and on which pins.
    //
    // Every pin number below is formatted from the #defines at the top of this
    // file rather than typed into the string. The pin map is meant to have one
    // source of truth; a hand-written "MOSI=35" in here is a second one that
    // goes stale in silence the day the define is corrected, and it goes stale
    // in the one endpoint an agent trusts to describe hardware it cannot see.
    char peri[256];
    snprintf(peri, sizeof(peri),
             "ST7789V2 240x135 IPS (not driven: RST=%d,DC=%d,MOSI=%d,SCLK=%d,CS=%d,BL=%d)",
             PIN_TFT_RST, PIN_TFT_DC, PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_CS,
             PIN_TFT_BL);
    doc["display"] = peri;
    snprintf(peri, sizeof(peri),
             "TCA8418 matrix controller on I2C 0x%02X (INT=%d, not driven)",
             TCA8418_ADDR, PIN_KB_INT);
    doc["keyboard"] = peri;
    snprintf(peri, sizeof(peri), "CS=%d,MOSI=%d,MISO=%d,SCLK=%d",
             PIN_SD_CS, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_SCLK);
    doc["sd_pins"] = peri;
    snprintf(peri, sizeof(peri),
             "ES8311 codec on I2C 0x%02X (I2S SCLK=%d,DSDIN=%d,LRCK=%d,ASDOUT=%d)",
             ES8311_ADDR, PIN_I2S_SCLK, PIN_I2S_DSDIN, PIN_I2S_LRCK,
             PIN_I2S_ASDOUT);
    doc["audio"] = peri;
    doc["ir_tx_pin"] = PIN_IR_TX;
    doc["battery_adc_pin"] = PIN_VBAT_ADC;
    // The EXT 2.54-14P header is on every ADVANCE; what is plugged into it is
    // not. Both halves of that are hardware facts an agent needs, and getting
    // either one wrong costs the same. Claiming a GNSS unconditionally puts
    // absent hardware in the fingerprint of a bare board; saying nothing hides
    // present hardware on a board that has it. So the header's own pins are
    // stated always, and the cap's contents only when the cap answered on I2C
    // at boot — see hw_probe() for why 0x43 is the discriminator.
    //
    // Nothing below is driven by this seed. It drives no pin on this header.
    snprintf(peri, sizeof(peri),
             "EXT 2.54-14P header UART: mainboard RX=%d,TX=%d. M5Stack's pinmap "
             "names these from the accessory's side, so its UART_RX/UART_TX read "
             "swapped against these",
             PIN_EXT_UART_RX, PIN_EXT_UART_TX);
    doc["ext_uart"] = peri;

    if (hw.cap_lora1262) {
        snprintf(peri, sizeof(peri),
                 "M5Stack Cap LoRa-1262, detected at boot: PI4IOE5V6408 IO "
                 "expander answered on I2C 0x%02X, which no ADVANCE mainboard "
                 "device uses",
                 PI4IOE_ADDR);
        doc["cap"] = peri;
        snprintf(peri, sizeof(peri),
                 "SX1262 on the detected Cap LoRa-1262, EXT header SPI "
                 "SCK=%d,MOSI=%d,MISO=%d,NSS=%d,DIO1=%d,BUSY=%d,RESET=%d "
                 "(not driven; MOSI/MISO/SCK are the microSD bus, and P0 of the "
                 "expander is the RF antenna switch)",
                 CAP_LORA_SCK, CAP_LORA_MOSI, CAP_LORA_MISO, CAP_LORA_NSS,
                 CAP_LORA_DIO1, CAP_LORA_BUSY, CAP_LORA_RST);
        doc["lora"] = peri;
        snprintf(peri, sizeof(peri),
                 "ATGM336H-6N (AT6668) on the detected Cap LoRa-1262, UART "
                 "mainboard RX=%d,TX=%d @%d 8N1 (not driven)",
                 PIN_EXT_UART_RX, PIN_EXT_UART_TX, CAP_GNSS_BAUD);
        doc["gnss"] = peri;
    } else {
        snprintf(peri, sizeof(peri),
                 "none detected: nothing answered on I2C 0x%02X, so no radio and "
                 "no GNSS are claimed. The EXT 2.54-14P header is present and "
                 "idle; a cap without an IO expander would not be seen",
                 PI4IOE_ADDR);
        doc["cap"] = peri;
    }

    // I2C, spec-named: descriptions, one per bus.
    JsonArray buses = doc["i2c_buses"].to<JsonArray>();
    char busdesc[48];
    snprintf(busdesc, sizeof(busdesc), "i2c0 SDA=%d SCL=%d 400kHz",
             PIN_I2C_SDA, PIN_I2C_SCL);
    buses.add(busdesc);

    // What the boot-time scan actually answered on that bus.
    //
    // An address that answered is a fact; the part behind it usually is not.
    // Only entries backed by M5Stack documentation for this board, or by a probe
    // of it, go out as "device". Everything else is a lookup in a table of parts
    // that commonly sit at that address, and it goes out as "device_guess" so an
    // agent is not handed a guess wearing the same key as a fact. An address
    // with neither gets no name at all rather than an invented one.
    if (hw.i2c0_count > 0) {
        JsonArray devs = doc["i2c_devices"].to<JsonArray>();
        for (int i = 0; i < hw.i2c0_count; i++) {
            JsonObject dev = devs.add<JsonObject>();
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", hw.i2c0[i].addr);
            dev["addr"] = String(hex);
            if (hw.i2c0[i].name) {
                dev[hw.i2c0[i].confirmed ? "device" : "device_guess"] =
                    hw.i2c0[i].name;
            }
        }
    }

    // GPIO, spec-named: the pins an agent may drive. Everything else is taken
    // by a bus, a connector or the module itself. Same allowlist the gpio skill
    // will report as safe.
    JsonArray pins = doc["gpio_pins"].to<JsonArray>();
    for (int i = 0; i < gpio_safe_pins_count; i++) pins.add(gpio_safe_pins[i]);

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_ip"] = WiFi.localIP().toString();
        doc["wifi_rssi"] = WiFi.RSSI();
    }
    // Only reported when the AP is genuinely running. The AP password is
    // deliberately absent: it never travels over the network.
    doc["ap_active"] = ap_active;
    if (ap_active) {
        doc["ap_ssid"] = ap_ssid;
        doc["ap_ip"] = WiFi.softAPIP().toString();
    }

    // docs/capabilities-spec.md: "endpoints lists all HTTP paths the node
    // handles". All of them — so "/" and "/wifi/config" are in here even though
    // the other two ESP32 seeds omit their equivalents. The spec is the only
    // written contract in the repo; where it and a sibling seed's habit
    // disagree, the spec wins. Keep this in step with setup_routes().
    JsonArray ep = doc["endpoints"].to<JsonArray>();
    const char *eps[] = {
        "/", "/health", "/capabilities", "/config.md", "/events",
        "/clock", "/clock/tz",
        "/firmware/version", "/firmware/upload", "/firmware/apply",
        "/firmware/confirm", "/firmware/rollback",
        "/skill", "/wifi/config", NULL
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

// Largest image the flash layout can take: the app slot size from
// partitions/cardputer_8mb_ota.csv. Both OTA slots are that size, so one
// constant covers whichever is inactive.
#define OTA_MAX_IMAGE_BYTES 0x330000

// Body callback for POST /firmware/upload.
//
// CROSS-CONNECTION OWNERSHIP GUARD. This one function is the body callback for
// EVERY connection to this route, and the ota_* flags below it are file-scope
// globals shared by all of them. That combination, without an owner, is a
// denial of service that any unauthenticated host on the LAN can hold open
// indefinitely:
//
//   while the token holder streams a real image, an attacker with no token at
//   all loops `curl -X POST -d x http://node:8080/firmware/upload`. Each of
//   those requests enters here at index == 0, fails the auth check — and, in
//   the obvious ordering, sets the shared ota_upload_error on its way out. The
//   legitimate transfer's very next chunk hits `if (ota_upload_error) return;`
//   and is dropped, possibly including its final chunk, so Update.end() never
//   runs and the image never completes. Keep the loop up and no upload can
//   ever succeed. It fails closed — the attacker cannot get a byte of their own
//   into flash — but it defeats the whole point of the token gate, because the
//   party WITH the token is the one who gets blocked.
//
// The fix is to bind the transfer to the request object that owns it, and to
// let no other request touch shared state:
//
//   1. an already-running transfer is checked FIRST, before auth, so a
//      stranger's request cannot even report an error into the owner's slot;
//   2. a failed auth check returns having written nothing at all. This callback
//      runs first — the library streams the body through here and only calls
//      the request handler once the request is complete — but the 401 does not
//      have to be produced here to be produced: handle_firmware_upload() runs
//      require_auth() on that same tokenless request a moment later and sends
//      it. Staying silent costs the caller nothing and costs the owner nothing;
//   3. only a request that gets past both claims ota_owner, and every later
//      chunk is matched against it before anything is read or written.
//
// A bare `if (ota_in_progress)` is not a substitute: it says a transfer exists,
// never whose it is, so any request could still write into another's state on
// the paths that run before it. The owner pointer is compared, never
// dereferenced — it is an identity token, valid only while the transfer it
// names is live, which is why every exit path below clears it.
//
// This guard is new here. The two sibling ESP32 seeds in this tree share the
// same globals-only structure and the same hole; whoever ports this back needs
// the mechanism above, not just the diff.
static void handle_firmware_upload_body(AsyncWebServerRequest *request, uint8_t *data,
                                         size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Someone else's transfer owns the slot. Touch none of its state — not
        // even to record an error, which is exactly the write that would break
        // it. This request still gets an answer from handle_firmware_upload()
        // once its body has drained; whatever that answer is, it is not allowed
        // to have changed what the owner sees.
        if (ota_in_progress) return;
        // No token: write nothing at all. handle_firmware_upload() runs
        // require_auth() on this same request when the body is done and sends
        // the 401 from there.
        if (!check_auth(request)) return;
        // Rejected before anything is claimed, so ota_owner stays null and the
        // remaining chunks of this request fall out at the guard below. The
        // error is still recorded: no transfer is in flight to disturb (that
        // was ruled out above), and it is how the client learns why.
        if (total == 0 || total > OTA_MAX_IMAGE_BYTES) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "invalid size: %u", (unsigned)total);
            return;
        }

        ota_owner = request;
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
            ota_owner = nullptr;
            return;
        }
        ota_upload_started = true;
    }

    // Chunks of a request that never claimed the slot. Nothing of theirs is
    // being tracked, so there is nothing to do with their bytes.
    if (request != ota_owner) return;

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
            ota_owner = nullptr;
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
        ota_owner = nullptr;
    }
}

static void handle_firmware_upload(AsyncWebServerRequest *request) {
    // The body handler re-checks the token itself, but a POST with no body
    // never reaches it and would otherwise report the md5 and size of whatever
    // was last flashed. Checking here gates every path through, and it is what
    // lets the body handler's own auth check fail silently: the 401 is sent
    // from here, so that check has nothing left to do but refuse to touch the
    // shared OTA state.
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

    String s = "# ESP32 Seed — M5Stack Cardputer ADVANCE\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    if (ap_active) s += "AP: " + ap_ssid + " (setup mode; password is on the serial console)\n";
    s += "\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n";
    s += "The token is a 32-char hex string, generated on the node's first boot and\n";
    s += "kept for its life. Two places give it to you: the serial console prints it\n";
    s += "at every boot (115200 8N1, `Token: ...`), and the setup AP's page at `/`\n";
    s += "shows it while the node is being provisioned. Once the node is on WiFi the\n";
    s += "AP is gone, so the serial console is the only remaining source\n\n";
    s += "## Grow cycle\n\n";
    s += "ESP32 has no compiler. Build on host, upload binary:\n\n";
    s += "1. GET /capabilities\n";
    s += "2. Write firmware (PlatformIO/Arduino/ESP-IDF)\n";
    s += "3. Compile: `pio run -e cardputer`\n";
    s += "4. POST /firmware/upload — send .bin (`-H 'Content-Type: application/octet-stream'`)\n";
    s += "5. POST /firmware/apply — reboot\n";
    s += "6. GET /health — verify\n";
    s += "7. POST /firmware/confirm (or auto after 60s)\n\n";
    s += "## Board gotchas\n\n";
    s += "- This is the ADVANCE, not the Cardputer v1.1. No v1.1 pin transfers: the\n";
    s += "  keyboard is a TCA8418 controller on I2C 0x34 instead of a GPIO scan matrix,\n";
    s += "  I2C is SDA=8/SCL=9 instead of 13/15, and 13/15 are the EXT 2.54-14P header's\n";
    s += "  UART here. Code or a board variant written for a v1.1 will drive the\n";
    s += "  expansion header as a key matrix\n";
    char otamax[96];
    snprintf(otamax, sizeof(otamax),
             "- 8MB flash, no PSRAM. OTA images must fit 0x%X bytes (%.1fMB); anything\n",
             (unsigned)OTA_MAX_IMAGE_BYTES,
             (double)OTA_MAX_IMAGE_BYTES / (1024.0 * 1024.0));
    s += otamax;
    s += "  larger is rejected before a single byte is written\n";
    s += "- There is no power-hold pin to drive. GPIO15 is an expansion-header UART\n";
    s += "  line, not a power latch\n";
    s += "- Only GPIO1, 2 (Grove port) and 7 are free for user I/O. 3,4,5,6,13,14,15,\n";
    s += "  39,40 are the EXT 2.54-14P expansion header (3=RESET, 4=INT, 5=CS, 6=BUSY,\n";
    s += "  14=MOSI, 39=MISO, 40=SCK, 13/15 UART, with 14/39/40 doubling as the microSD\n";
    s += "  bus). Those pins belong to the header, not to any one accessory\n";
    s += "- Whether a cap is plugged into that header is DETECTED at boot, not assumed:\n";
    s += "  a PI4IOE5V6408 IO expander answering on I2C 0x43 means an M5Stack Cap\n";
    s += "  LoRa-1262, because the ADVANCE mainboard has no IO expander of its own. When\n";
    s += "  it is seen, /capabilities gains `lora` (SX1262) and `gnss` (ATGM336H-6N) with\n";
    s += "  their pins; when it is not, `cap` says so and neither is claimed. This\n";
    s += "  firmware drives neither device either way\n";
    s += "- The header's UART is mainboard RX=15, TX=13. M5Stack's Cardputer-Adv pinmap\n";
    s += "  labels G13 UART_RX and G15 UART_TX from the accessory's side, so reading it\n";
    s += "  as an ESP32-side name gives a UART wired backwards. M5Stack's own Cap\n";
    s += "  LoRa-1262 example opens it as `Serial1.begin(115200, SERIAL_8N1, 15, 13)`\n";
    s += "- GPIO16,17,18,21 are not on the safe list either, and not because they are\n";
    s += "  known to be taken: M5Stack's pinmap simply does not mention them, while the\n";
    s += "  board has a microphone and an NS4150B amplifier documented on no pins at all\n";
    s += "- GPIO38 is the display backlight and also gates the RGB LED supply\n";
    s += "- The mainboard I2C bus (SDA=8/SCL=9) carries exactly three devices: TCA8418\n";
    s += "  keyboard controller 0x34, ES8311 codec 0x18, BMI270 IMU 0x69. Anything else\n";
    s += "  in /capabilities' i2c_devices came from a cap or the Grove port. Entries\n";
    s += "  there are keyed `device` when the part is documented or probed for this\n";
    s += "  board and `device_guess` when it is only a common part at that address —\n";
    s += "  do not treat a guess as an identification\n";
    s += "- Flashing: `pio run -e cardputer -t upload` is one call and needs nothing\n";
    s += "  extra — platformio.ini already sets board_upload.after_reset=watchdog_reset\n";
    s += "- Only if you drive esptool directly: with its default `--after hard_reset`\n";
    s += "  this chip is observed to stay in the bootloader after a successful write\n";
    s += "  instead of running the image, which then looks like a flash that did\n";
    s += "  nothing. `--after watchdog_reset` boots it. The remedy is reliable; the\n";
    s += "  mechanism is not documented well enough to assert one here\n";
    s += "- The clock runs on UTC until POST /clock/tz stores a POSIX TZ string in SPIFFS\n";
    s += "- The setup AP is only up while the node has no working WiFi credentials, with\n";
    s += "  a fresh random password per raise. It tears itself down the moment the STA\n";
    s += "  link comes up and does not come back if WiFi is lost later\n";
    s += "- POST /wifi/config needs the token unless the request comes over that AP\n";
    s += "- Paths match exactly: `/health/` is not `/health` and returns 404. Nothing\n";
    s += "  here generates a trailing slash, but a client that appends one will break\n";
    s += "- An OTA upload whose connection dies is torn down after 30s without data,\n";
    s += "  so a dropped transfer no longer blocks every later one until reboot\n\n";
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

// Escapes the five characters that can break out of HTML text or an attribute.
//
// Both call sites below interpolate strings the node does not control — an SSID
// broadcast by a neighbouring access point, and the raw `ssid` field of a POST
// — straight into a text/html response. Today the damage is bounded: the only
// request that reaches the token-bearing version of this page is one already
// coming from the provisioning AP, which means whoever can inject can also just
// read the token off the same page. That is an argument about the page as it is
// today, not about the code, and it stops holding the moment this page grows a
// second reader, a status view, or a link. Escaping on the way out costs
// nothing and removes the primitive rather than the current exploit for it.
static String html_escape(const String &in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

// True only for a request that arrived over the provisioning AP while that AP
// is actually up. Both halves matter: once the AP is down softAPIP() is
// 0.0.0.0 and the subnet test is meaningless, and belonging to some subnet
// proves nothing on its own. Reaching the node this way costs an attacker
// physical presence plus the per-raise password, which is why this is the one
// path allowed to skip the token.
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
        html += "<p>Connected: " + html_escape(WiFi.SSID()) +
                " (" + WiFi.localIP().toString() + ")</p>";
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
        "<h2>Saved. Connecting...</h2><p>" + html_escape(ssid) +
        "</p><a href='/'>Back</a>");
    delay(1000);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

// ===== Skills =====
//
// Nothing to include yet. Skills arrive as `#include "skills/<name>.cpp"` here
// and one init() call below; build_src_filter already excludes src/skills/ from
// the build so they compile into this translation unit rather than separately.

static void skills_init() {
}

// ===== Routes =====

// Every route is registered with AsyncURIMatcher::exact(), and any route added
// later must be too.
//
// The library's default is AsyncURIMatcher BackwardCompatible, which matches
// the regex ^{uri}(/.*)?$ — so a handler for /clock also answers /clock/tz, and
// whichever of the two is registered first wins. That cost the T-Embed seed a
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
    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    hw_probe();       // I2C scan
    tz_load();        // before wifi_setup(): configTzTime() needs the TZ string
    wifi_setup();     // RF up first; also raises the setup AP if STA fails
    token_load();     // after wifi_setup(): needs RF up for a real hardware RNG
    skills_init();
    setup_routes();
    server.begin();

    Serial.printf("\nESP32 Seed v%s (Cardputer ADVANCE)\n", SEED_VERSION);
    // The token IS printed, and unlike the AP password below this line is not a
    // bring-up crutch to be removed once the panel is driven.
    //
    // The setup AP is torn down for good the moment the STA link comes up, and
    // this commit drives no input device, so there is no gesture to raise it
    // again. After a successful provisioning the page that hands out the token
    // behind from_setup_ap() is therefore unreachable, permanently; /capabilities
    // sits behind require_auth() like every route but /health, so it cannot hand
    // it out either. Without this line the node's whole grow cycle —
    // /capabilities, /firmware/upload, /firmware/apply, /firmware/confirm — is
    // locked away from the node's own owner.
    //
    // Reading Serial costs a USB cable physically attached to the board, and
    // whoever has that can already reflash the chip wholesale through the ROM
    // bootloader. The disclosure hands an attacker nothing they did not have.
    // The sibling seed in seeds/esp32 prints the token for the same reason and
    // keeps doing so with a screen fitted; seeds/esp32-ats-mini is the one that
    // withholds it, and only because its panel is driven and carries the token
    // instead — which is the weaker of the two channels, being readable across a
    // room with no cable at all.
    Serial.printf("Token: %s\n", auth_token.c_str());
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("http://%s:%d/health\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    if (ap_active) {
        // The AP password IS printed, and only until this commit grows a screen.
        // It is raised at boot when no stored credentials work, so with no
        // display and no keyboard there would otherwise be no way to read it and
        // no way to join the AP the node just brought up. Serial needs a USB
        // cable physically attached, which is the same physical-presence
        // argument the screen makes. REMOVE THIS once the panel is driven and
        // can carry the password instead. The token print above stays, and the
        // difference is not that one secret is graver than the other: the
        // password is re-rolled per raise, and a later commit can raise the AP
        // by keyboard gesture with no cable anywhere near the board, so it needs
        // a channel that does not assume one. The token is minted once, on a
        // first boot that by definition happens on a cable.
        Serial.printf("http://%s:%d/health  (setup AP: %s, password: %s)\n",
            WiFi.softAPIP().toString().c_str(), HTTP_PORT,
            ap_ssid.c_str(), ap_password.c_str());
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
    // "already in progress" — recoverable only by rebooting the node, which
    // on a half-flashed slot means a rollback.
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
        // Releases the slot for the next uploader. The dead request is gone; a
        // pointer to it must not stay behind as an owner.
        ota_owner = nullptr;
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

    // Retire the provisioning AP once it has done its job. Non-blocking.
    ap_poll();

    delay(10);
}
