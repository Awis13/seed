// ESP32 Seed — M5Stack Cardputer ADVANCE (StampS3A / ESP32-S3FN8)
//
// The Cardputer port of the ESP32 seed: just enough to boot, connect, and let
// an AI agent grow it via OTA firmware uploads.
//
// The HTTP API, the security baseline (token, provisioning AP, exact route
// matching), OTA, and the on-device UI: the ST7789 panel and the TCA8418
// keyboard are driven through M5Cardputer/M5Unified. The gpio and serial skills
// ship here too, #included into this translation unit from src/skills/ — see
// the Skills section near the bottom for why they are #included rather than
// compiled separately.
//
// The node has no camera and nobody can see its screen remotely, so GET /ui
// exists to make the panel's state answerable over the network: which screen is
// up, which fields it is showing and what they read, the backlight, and the last
// key pressed. It is the only way this firmware's display can be verified at all
// from off the device.
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
//   - No sub-GHz radio. The CC1101 probe the T-Embed seed runs has nothing to
//     find on this mainboard.
//   - No fuel gauge. The T-Embed's BQ27220 computes a charge percentage and
//     hands it over I2C, which is what its status screen displays; there is no
//     such chip here, so there is no percentage to display.
//   - There IS a battery. It is sensed as a plain voltage divider on the ADC at
//     GPIO10, and it is left unread because the divider ratio is documented
//     nowhere: a guessed ratio would put a fabricated voltage in /capabilities,
//     which states the pin number and stops there. Settling the ratio takes a
//     meter across the cell, not a code change.
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
//   GET  /ui                — what the panel is showing right now
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
//
// gpio skill (src/skills/gpio.cpp):
//   GET  /gpio/list         — every pin with class, safe/refused, mode, value
//   GET  /gpio/read         — read a digital pin (?pin=N)
//   POST /gpio/write        — set a digital output {pin, value}
//   POST /gpio/mode         — set a pin mode {pin, mode}
//   GET  /gpio/adc          — read an ADC-capable pin (?pin=N)
//
// serial skill (src/skills/serial.cpp):
//   GET  /serial/ports      — UART state: open, pins, baud, bytes waiting
//   POST /serial/open       — open a UART {uart, baud, rx, tx}
//   POST /serial/write      — write bytes {uart, data}
//   GET  /serial/read       — read buffered data (?uart=N&timeout=ms)
//   POST /serial/close      — close a UART {uart}

#include <Arduino.h>
#include <stdlib.h>
#include <time.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
// Pulls in M5Unified and M5GFX. Note what is NOT here: <Wire.h>. This seed has
// exactly one I2C owner and it is M5Unified's m5::In_I2C — see the ownership
// note above hw_probe() before reaching for Wire again.
#include <M5Cardputer.h>
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
// something on the mainboard. The panel, the I2C bus and the keyboard
// controller ARE driven now, but not from these numbers — M5GFX and M5Unified
// carry their own copies of the pinmap and configure those pins themselves. The
// defines stay as documentation and as the single source /capabilities formats
// its pin lists from; the rest of the map is still untouched by this firmware.

// ST7789V2 240x135 IPS on SPI. GPIO38 is the backlight enable and also gates
// the RGB LED supply, so it has to go HIGH before either can light up.
//
// These six are documentation only now: M5GFX owns the panel and configures
// every one of them itself from its own autodetect table, and nothing in this
// file drives them. They stay because /capabilities reports them and because a
// skill added later needs to know they are taken. GPIO38 in particular is not a
// plain output any more — M5GFX attaches it to an LEDC channel and dims it, so
// the backlight is a brightness value rather than a pin level.
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
//
// These two are also documentation only. M5.begin() brings this bus up itself
// as m5::In_I2C on I2C_NUM_1 with exactly these pins, and that is the seed's
// only I2C owner; see the ownership note above hw_probe().
#define PIN_I2C_SDA      8
#define PIN_I2C_SCL      9
// TCA8418 interrupt, active low. It is an OUTPUT of the keyboard controller and
// an input as far as this board is concerned: the driver calls the chip's
// enableInterrupts() unconditionally, so the TCA8418 asserts this line on every
// key event. That makes the pin externally driven, which is why the gpio skill
// refuses it — two chips pushing one net is not a thing to expose over HTTP.
//
// The ESP32 side DOES listen, and an earlier revision of this file claimed
// twice that it does not. The claim came from reading only the declaration:
// TCA8418KeyboardReader's interrupt_pin argument defaults to -1 in the header,
// but the constructor BODY replaces any negative value with the library's
// DEFAULT_TCA8418_INT_PIN, which is 11 — this pin. The reader's begin() then
// takes the >= 0 branch and does pinMode(INPUT) plus attachInterruptArg() on
// CHANGE. Reading a line that another chip drives is not contention, and it is
// how key events get here at all; the gpio skill's refusal above is about
// WRITING it, which is still the right call.
#define PIN_KB_INT      11

// ES8311 audio codec, I2S side. Nothing here drives I2S: M5Unified only
// *configures* its Speaker and Mic at begin() and touches no pin until one of
// them is actually started, which this firmware never does.
#define PIN_I2S_SCLK    41
#define PIN_I2S_DSDIN   42
#define PIN_I2S_LRCK    43
// The codec's data line out, i.e. the ESP32's input — M5Unified configures it
// as the microphone's pin_data_in for this board.
//
// One surprise to record rather than rediscover: M5.begin() drives GPIO46 HIGH
// as an output before it has worked out what board it is on. It is the power
// hold for the Capsule/Dial/DinMeter and the code is guarded on the target
// being an ESP32-S3, not on the board, so every S3 board gets it — including
// this one, where 46 is not a power latch. It is harmless here only because
// this firmware never starts the microphone; a later commit that does must
// expect the pin to have been an output first.
#define PIN_I2S_ASDOUT  46

#define PIN_IR_TX       44
#define PIN_VBAT_ADC    10  // battery divider; the ratio is unverified, so unread

// RGB LED data line. M5Unified's board table maps board_M5CardputerADV to this
// pin, and _setup_led() runs on every begin(), so the assignment is documented
// rather than guessed. It only CONFIGURES the LED — it builds an RMT bus object
// holding this pin number and drives nothing. The RMT peripheral is started by
// M5.Led.begin(), which this firmware never calls, and cfg.led_brightness
// defaults to 0, so the pin is idle at runtime. Idle, not free: the supply
// behind it is gated by GPIO38 and anything that lights the LED takes the pin
// back, which is why it is not on the safe list below.
//
// The #undef is not tidying: the generic esp32s3 variant's pins_arduino.h, which
// Arduino.h drags in above, already defines this name as 48 for a devkit's
// on-board LED. Redefining it over the top is what we want — 48 is meaningless
// here — but doing it silently is not, so the collision is acknowledged rather
// than left as a compiler warning that trains everyone to ignore warnings.
#undef PIN_RGB_LED
#define PIN_RGB_LED     21

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
//          the board's own on it. M5Unified knows these two as Ex_I2C (SCL=1,
//          SDA=2) and assigns them I2C_NUM_0, but it only calls setPort() —
//          which stores the numbers and starts nothing — so the pins stay idle
//          and stay safe. They stop being safe the moment anything calls
//          Ex_I2C.begin().
//   7      Wired to an internal FPC connector and to nothing else. M5GFX's own
//          board table names it "Internal FPC" for the ADVANCE, in the same
//          column where it writes "NC" for a pin that goes nowhere on a
//          sibling board — so the distinction is the library's, not an
//          inference. M5Stack's product notes describe an unpopulated FPC
//          footprint near the 3.5mm jack. On a stock unit nothing is attached
//          to it, which is what makes it safe to drive; it stops being safe the
//          moment something is plugged into that connector.
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
//   21                      RGB LED data line — see PIN_RGB_LED above. A known
//                           assignment from M5Unified's board table, not an
//                           unknown one, and excluded because the LED owns the
//                           pin even though nothing here starts the RMT.
//   16,17,18                UNCLASSIFIED — excluded because we do not know, not
//                           because we know. M5Stack's published Cardputer-Adv
//                           pinmap lists no assignment for these three, yet the
//                           board carries a microphone and an NS4150B amplifier
//                           whose pins appear nowhere in that document. The odds
//                           that three undocumented pins and two undocumented
//                           peripherals are unrelated are poor, so guessing is
//                           worse than excluding. This is a gap to close with a
//                           schematic or a meter, not a settled fact: whoever
//                           establishes what they are should move them into a
//                           named group above or onto the safe list.
static const int gpio_safe_pins[] = {1, 2, 7};
static const int gpio_safe_pins_count =
    sizeof(gpio_safe_pins) / sizeof(gpio_safe_pins[0]);

// Used by /capabilities and by skills/gpio.cpp, which reports it as `safe`.
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
// Three skills ship: notify, #included above the UI section because the screens
// read its store, and gpio and serial at the bottom of this file. Everything a
// skill needs — the PIN_* map, gpio_is_safe(), require_auth(), event_add(),
// handle_body_collect() — is declared above the #includes that pull them in.
//
// handle_body_collect() is the one that needs a declaration rather than a
// definition: it is defined with the other HTTP handlers, below the UI section
// and so below the notify include, and a skill registering a POST route hands
// it to server.on() as a body callback.
static void handle_body_collect(AsyncWebServerRequest *request, uint8_t *data,
                                size_t len, size_t index, size_t total);

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

// Called by each skill's own init(), from skills_init() below.
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

// Highest 7-bit address the scan covers, exclusive. Also the size the result
// buffer must have: m5::I2C_Class::scanID() indexes it by address.
#define I2C_SCAN_LIMIT 0x78

// Scan the one I2C bus through M5Unified.
//
// scanID() fills result[0x08 .. 0x77] and leaves 0x00..0x07 alone — the reserved
// low addresses are skipped deliberately, because probing them wedges an
// ESP32-S3 — so the buffer is zeroed first rather than trusted.
static void i2c_scan(I2CFound *results, int &count) {
    bool present[I2C_SCAN_LIMIT];
    memset(present, 0, sizeof(present));
    m5::In_I2C.scanID(present, 400000);

    count = 0;
    for (uint8_t addr = 0x08; addr < I2C_SCAN_LIMIT; addr++) {
        if (!present[addr] || count >= MAX_I2C_FOUND) continue;
        const I2CDevice *known = i2c_identify(addr);
        results[count].addr = addr;
        results[count].name = known ? known->name : NULL;
        results[count].confirmed = known ? known->confirmed : false;
        count++;
    }
}

// I2C OWNERSHIP — read this before adding a bus user.
//
// There is one I2C bus on this board and exactly one driver allowed to own it:
// m5::In_I2C, brought up by M5.begin() on I2C_NUM_1 with SDA=8/SCL=9. Arduino's
// Wire is deliberately not used anywhere in this seed, and restoring it would
// break the bus in a way that produces no error anywhere.
//
// Why: M5Unified::begin() calls _setup_i2c() unconditionally — there is no
// config flag to turn it off — and that ends in In_I2C.begin(I2C_NUM_1, 8, 9).
// Bringing up a second controller on the same two pads re-points their output
// selector, and a pad has only one. Whichever controller was pointed at them
// first is left electrically disconnected while its driver still reports itself
// initialised: Wire.begin(8, 9) returns true, beginTransmission()/
// endTransmission() keep returning success codes, and every device answers
// nothing. An I2C scan through the disconnected bus finds zero devices, which
// this firmware would read as "not an ADVANCE" — a silent, total misdetection
// with no failed call anywhere to point at.
//
// Ordering around the problem does not fix it. Scanning through Wire before
// M5.begin() would work exactly once, at boot, and leave a Wire object behind
// that looks usable and is not — a trap for the gpio and serial skills that
// come next. One bus, one owner, and the owner is M5.
//
// Calling this needs M5.begin() to have run. ui_begin() does that, and setup()
// calls it first for this reason.
static void hw_probe() {
    memset(&hw, 0, sizeof(hw));

    // Chip info
    hw.chip_model = ESP.getChipModel();
    hw.chip_revision = ESP.getChipRevision();
    hw.flash_size = ESP.getFlashChipSize();
    hw.flash_speed = ESP.getFlashChipSpeed();
    hw.psram_size = ESP.getPsramSize();  // zero on this board, reported anyway
    hw.temp_c = temperatureRead();

    // Single I2C bus, already up and already on 8/9 because M5.begin() put it
    // there. Nothing here configures pins: the pin numbers come from
    // M5Unified's own table for board_M5CardputerADV, which is the same 8/9 the
    // #defines above record.
    i2c_scan(hw.i2c0, hw.i2c0_count);

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
// FIXED BUFFER, not String, for the same reason wifi_ssid below is one: it is
// read on the AsyncTCP task (GET /ui, through ui_status_row() and ui_field())
// and a String reassignment frees the buffer a concurrent reader is walking.
// Written once in wifi_setup() before the web server starts and never again,
// so today no writer races it — but the panel's cross-task reader makes that a
// property to keep by construction, not one to rely on staying true. It is read
// by both formatters: the clock face's AP line and the NETWORK screen's.
// "Seed-" + four hex + NUL fits in ten; sized with headroom.
static char ap_ssid[16] = "";
// HOW LONG THAT IS, AS A NUMBER, because the clock face puts the SSID, the
// session state and the password on ONE row and has to bound the total. The
// buffer is no use for that — it is headroom, and three buffers' headroom on
// one row overflows it.
//
// THE TWO PIECES AND NOT THE ANSWER, so that the length and the format that
// builds it cannot drift apart. wifi_setup() formats with these same two
// constants rather than with a matching pair of literals; widen the suffix
// there and this number follows, instead of staying nine while the row prints
// the first nine characters of a longer SSID — silently, and identically on the
// panel and in GET /ui, which is the failure that has no witness.
#define AP_SSID_PREFIX "Seed-"
#define AP_SSID_HEX     4     // get_mac_suffix() returns exactly this many
#define AP_SSID_CHARS  ((int)(sizeof(AP_SSID_PREFIX) - 1) + AP_SSID_HEX)
static String mdns_name = "";
static unsigned long boot_time = 0;

// Provisioning AP state. The password is rolled on every raise and exists only
// in this variable and on the panel — never persisted, never sent anywhere over
// the network, and specifically never through /ui.
//
// A session raised by hand from the keyboard is time-boxed: somebody may have
// leaned on the key, or walked away mid-provisioning, and an open AP with a
// known-to-someone password must not then stay up until the next reboot. The
// boot-time session is NOT time-boxed, because when the stored credentials do
// not work it is the only way into the node at all.
#define AP_SESSION_MS (10UL * 60UL * 1000UL)
static bool ap_active = false;
// FIXED BUFFER, not String. Written on the loop task (ap_start rolls it,
// ap_stop clears it) and read on the AsyncTCP task by ui_status_row() — the
// exact cross-task String reassignment that was a use-after-free for the WiFi
// credentials. GET /ui never reaches the read (it passes redact=true and
// returns a placeholder first), but the panel does, and defence here costs a
// dozen bytes. The generated password is AP_PASSWORD_CHARS long; sized with
// headroom.
static char ap_password[16] = "";
// The generated length, shared with ap_generate_password() rather than written
// twice, for the reason AP_SSID_CHARS above exists: the clock face's AP row
// bounds three variable parts against one buffer, and a password that grew by a
// character somewhere else in the file would push that row over without a word.
#define AP_PASSWORD_CHARS  12
static bool ap_temporary = false;
static unsigned long ap_expires_at = 0;
// Set when a keyboard-raised AP failed to come up, so the menu entry can say so
// instead of reading "off" as though nothing had been asked. Cleared by the
// next successful raise.
static bool ap_start_failed = false;

// Whole minutes left on a time-boxed session, rounded up so it never reads 0
// while the AP is still up. Zero means a session that does not expire.
static unsigned long ap_minutes_left() {
    if (!ap_temporary) return 0;
    long remaining = (long)(ap_expires_at - millis());
    if (remaining <= 0) return 0;
    return ((unsigned long)remaining + 59999UL) / 60000UL;
}

// Ask the next UI tick to repaint everything rather than trusting its per-field
// caches. Declared up here because state changes that the screen must show
// immediately — the AP coming up with a fresh password on it — happen well
// before the UI section below.
//
// `volatile` is not a fix. Every writer of THIS flag runs in setup() or loop()
// context, so nothing here is broken without it and nothing here is made
// correct by it — `volatile` is not a synchronisation primitive and this is a
// single-byte flag, not shared state that needs one. What it buys is the one
// thing it is actually for: a store cannot be optimised away or hoisted out of
// reach of the loop task that polls it in ui_tick(). The firmware this is
// ported from already marks its equivalent volatile, so this is parity with it
// and not an invention.
//
// See ui_tick() for the other half of that — where the flag is cleared, and
// why it is not where the source firmware clears it.
static volatile bool ui_force = false;

// The same request, raised from the web server task instead, and it is a
// SEPARATE FLAG PRECISELY SO THAT IT CANNOT REACH THE TICK GATE.
//
// A repaint costs a fillScreen() of the whole 240x135 panel plus every field
// redrawn — tens of thousands of pixels of SPI, on the loop task, in one
// uninterruptible run. ui_force short-circuits the 200ms gate in ui_tick(),
// which is right for a keypress: a user who pressed a key and waited a fifth
// of a second for the screen to answer would call the device slow. It is
// wrong for anything the network raises, because the network sets the rate.
// POST /notify at 20/s through ui_force is 20 full-frame repaints a second,
// each one of them time the loop task is not spending in ui_key_poll() — which
// retires at most one keyboard event per pass. That is the same starvation
// NOTIFY_CRIT_MIN_GAP_MS exists to keep the flash writer out of, arriving by
// the other door, and /skill tells agents the queue is rate-limited to protect
// the keyboard.
//
// So network raises wait for the gate. They are folded into `force` inside
// ui_tick() and appear nowhere in the expression that decides whether this
// tick runs at all, which bounds the panel at one repaint per UI_TICK_MS no
// matter what arrives. The cost is up to 200ms of latency on a message card,
// on a node with no buzzer and no LED where the panel is not the thing that
// makes an arrival urgent.
//
// Raised only from skills/notify.cpp, and there only on the endpoint paths.
// notify_poll() runs on the loop task and correctly uses ui_force.
static volatile bool ui_force_net = false;

// Stored WiFi credentials.
//
// FIXED BUFFERS, NOT String, AND THAT IS THE WHOLE POINT. These are written on
// the AsyncTCP task by handle_wifi_post() and read on the loop task by
// ui_field() and the reconnect timer, and ui_field() is itself also called from
// AsyncTCP by handle_ui() — three readers, one of them concurrent with the
// writer by construction, since provisioning is exactly when the panel is
// showing the network screen. As Strings, `wifi_ssid = ssid` frees the old
// heap buffer and installs a new one, so a reader holding the pointer that
// c_str() handed it a moment earlier reads freed memory. Sized to the protocol
// maxima: SSID 32 octets, WPA2 passphrase 63, plus a terminator each.
//
// A fixed array cannot move and is never freed, so a reader can at worst catch
// a half-written value and print a garbled line for one 200ms tick. Every
// writer below goes through snprintf() into the full array, which keeps a
// terminator inside the bounds at all times, so even that case stays a string.
// Copying out per call would not have been better: the copy would race the same
// way, and it would put 96 bytes on the stack of a task that has to be frugal.
static char wifi_ssid[33] = "";
static char wifi_pass[65] = "";

// OTA state
static bool firmware_confirmed = false;
static bool firmware_confirm_attempted = false;
// Written on the AsyncTCP task, in handle_firmware_upload_body(), and read on
// the loop task, by the stall watchdog at the bottom of loop(). Hence volatile:
// without it the compiler is entitled to hoist the read out of loop() and cache
// it in a register, since nothing in loop()'s own control flow can change it,
// and the watchdog would then be testing a snapshot taken at an arbitrary
// earlier time. The same applies to ota_last_chunk_ms below.
static volatile bool ota_in_progress = false;
static bool ota_upload_started = false;
static bool ota_upload_ok = false;
static bool ota_upload_error = false;
static char ota_upload_error_msg[128] = "";
static size_t ota_bytes_written = 0;
// Identity of the in-flight transfer. See the ownership guard at the top of
// handle_firmware_upload_body() for why the flags above are not enough on their
// own, and why this is a session id rather than the request pointer it used to
// be: the request object is destroyed on client disconnect, so a later request
// whose AsyncWebServerRequest lands at the same freed address would pass a
// pointer-equality check and splice its chunks into the abandoned transfer.
// A monotonically increasing session id cannot be aliased that way. Zero means
// no transfer is in flight. ota_claimed_total is what the owner declared, kept
// so a chunk whose total disagrees is rejected — a continuity check aliasing
// could not satisfy even if it forged the id.
static volatile uint32_t ota_session = 0;
static uint32_t ota_next_session = 1;
static size_t ota_claimed_total = 0;
// millis() of the last body chunk received, for the abandoned-upload watchdog
// in loop(). Only meaningful while ota_in_progress. Volatile for the reason
// given above, and additionally because the watchdog's comparison must read it
// exactly once: see the signed subtraction there.
static volatile unsigned long ota_last_chunk_ms = 0;
static volatile bool pending_restart = false;
static volatile bool pending_rollback = false;
// Set by POST /wifi/config on the AsyncTCP task, acted on by loop(). The
// reconnect belongs to loop() for the same reason the OTA restart above does:
// the handler runs on the web server's task and must not do slow work or touch
// the radio there.
//
// It used to call delay(1000) and then WiFi.begin() inline. That was wrong
// three times over. The delay blocks the single AsyncTCP task, so for a whole
// second every other request queues behind it — including /health, the one an
// agent uses to decide whether the node is alive, which makes a successful
// provisioning look like a node that just went unresponsive. WiFi.begin() from
// that task then races the identical call in wifi_poll() on loop(), two tasks
// driving one radio through an API that is not reentrant. And it reads
// wifi_ssid/wifi_pass, the fixed buffers this very handler has just rewritten
// from the other task, so the connect could be issued with half of the old
// credentials and half of the new.
//
// Deferring fixes all three: the response goes out immediately, and loop() does
// the reconnect from the task that already owns the radio, after the buffers
// are whole.
static volatile bool pending_wifi_connect = false;

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

// A SHORT WRITE IS A FAILED WRITE AND HAS TO BE REPORTED AS ONE. File::print()
// reaches fwrite() through VFSFileImpl::write(), and fwrite() answers a full-
// filesystem partition with a short count, not an error: 400 bytes of a 900
// byte snapshot land on flash and the call comes back. The partition here is
// 1.5 MB and the notification store is exactly the kind of file that grows into
// it, so this is a case that will be reached rather than a theoretical one.
// Comparing the count against the string is what turns it into a false return,
// and every safety the callers have rests on that false.
static bool write_spiffs_file(const char *path, const String &content) {
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.print(content);
    f.close();
    return written == content.length();
}

// Same, for a file whose previous contents are worth more than the write.
//
// FILE_WRITE truncates on open, so an ordinary write spends its whole duration
// with the file empty: power lost in there takes the old snapshot with the new
// one. Writing beside it and renaming afterwards narrows that to the remove
// plus the rename, and even in there the complete new snapshot is still on
// flash under the temp name.
//
// THAT LAST PART IS ONLY HALF A MECHANISM AND THIS FUNCTION IS THE OTHER HALF
// OF NOTHING WITHOUT IT. The crash that this narrows the window on is exactly
// the one that leaves the real name gone or empty and the whole snapshot
// sitting under the temp name. A caller that reads the real name, gets an
// empty string and gives up has bought nothing at all: every caller must fall
// back to reading tmp_path when path comes back empty, or this is decoration.
//
// THAT FALLBACK COVERS THE CRASH WINDOW AND NOTHING ELSE. It is keyed on the
// real name coming back empty, so it can only answer a power loss between the
// remove and the rename. It is no answer at all to a write that ran out of
// room, because the real name would not be empty then — it would hold a
// truncated snapshot that parses as nothing, under a call that returned true.
// A failed write is therefore not allowed to reach the remove: it has to stop
// at the guard below, which is why write_spiffs_file() checks its byte count
// and why this returns early instead of taking the old file down first.
//
// SPIFFS.rename() will not clobber an existing name, hence the remove.
//
// The notification store is the caller, and notify_load() holds up the other
// half: it reads NOTIFY_FILE, and falls back to NOTIFY_FILE_TMP when that comes
// back empty. Any future caller owes the same fallback.
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
    // as<const char*>() answers null for a missing or non-string key, which
    // snprintf would happily render as "(null)" into the credential itself.
    const char *ssid = doc["ssid"].as<const char *>();
    const char *pass = doc["password"].as<const char *>();
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid ? ssid : "");
    snprintf(wifi_pass, sizeof(wifi_pass), "%s", pass ? pass : "");
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
// console or, once the panel is driven, off a 240px screen; AP_PASSWORD_CHARS
// out of a 32-symbol alphabet is 60 bits at twelve.
static String ap_generate_password() {
    static const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";
    const uint32_t n = sizeof(charset) - 1;
    char buf[AP_PASSWORD_CHARS + 1];
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

// Raise the provisioning softAP.
//
// `manual` means the keyboard asked for it — the Setup AP entry in the on-device
// menu. Two things follow from that and only from that. The session is
// time-boxed (see AP_SESSION_MS), because unlike the boot-time raise it is not
// the last way into the node: the node is on WiFi, so closing the window costs
// nothing and leaving it open costs a standing invitation. And it means the
// password has to reach a human with no cable attached, which is what the panel
// is for; setup() no longer prints it to the console.
static void ap_start(bool manual) {
    if (ap_active) return;  // idempotent: a re-raise must not re-roll the password
    // RF is up by now (wifi_setup() has run), which is what makes esp_random()
    // a hardware RNG rather than a seeded PRNG — same reason token_load() is
    // deferred until after wifi_setup().
    snprintf(ap_password, sizeof(ap_password), "%s", ap_generate_password().c_str());
    WiFi.mode(WIFI_AP_STA);
    // Pin the subnet before the AP comes up, clear of the owner's LAN, so a LAN
    // client can never land in it and pass from_setup_ap()'s /24 test. If the pin
    // fails the AP would fall back to the default 192.168.4.1/24, where a LAN
    // client could share that subnet and slip past from_setup_ap() — so refuse to
    // raise the AP at all rather than expose the token-skipping path over the LAN.
    if (!WiFi.softAPConfig(IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D),
                           IPAddress(255, 255, 255, 0))) {
        ap_password[0] = '\0';
        WiFi.mode(WIFI_STA);
        event_add("setup AP: subnet pin failed, not raising AP");
        return;
    }
    if (!WiFi.softAP(ap_ssid, ap_password)) {
        ap_password[0] = '\0';
        WiFi.mode(WIFI_STA);
        event_add("setup AP failed to start");
        return;
    }
    ap_active = true;
    ap_start_failed = false;
    ap_temporary = manual;
    ap_expires_at = millis() + AP_SESSION_MS;
    ap_seen_sta_down = (WiFi.status() != WL_CONNECTED);
    event_add("setup AP up: %s%s", ap_ssid,
              manual ? " (time-boxed)" : "");  // SSID only, never the password
    mdns_restart();
    ui_force = true;  // the password exists nowhere but on the screen
}

static void ap_stop() {
    if (!ap_active) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap_active = false;
    ap_password[0] = '\0';
    ap_temporary = false;
    ap_seen_sta_down = false;
    event_add("setup AP down");
    mdns_restart();
    ui_force = true;
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
    // Nobody used the window the keyboard opened: close it. The subtraction
    // stays in signed arithmetic so it survives the millis() rollover.
    if (ap_temporary && (long)(millis() - ap_expires_at) >= 0) {
        event_add("setup AP expired");
        ap_stop();
    }
}

static void wifi_setup() {
    // The whole identity of the node — its setup-AP SSID and its mDNS hostname
    // — hangs off these four characters. They come from eFuse rather than from
    // the WiFi stack (see get_mac_suffix), so this is free to sit ahead of the
    // radio and is already settled by the time anything advertises it.
    String suffix = get_mac_suffix();
    // A PRECISION, not a bare %s. get_mac_suffix() returns exactly
    // AP_SSID_HEX hex characters, so the SSID is AP_SSID_CHARS long and fits
    // with room to spare — but the compiler cannot see inside the String and
    // warned that up to 14 bytes could go into an 11-byte tail
    // (-Wformat-truncation). The precision makes the invariant the source
    // already relies on visible to the compiler instead of implied, and clamps
    // the write if get_mac_suffix() ever returns something longer. Not silenced
    // on the grounds that it looked like a false positive: the GNSS block in
    // serial.cpp shipped truncated on a live node while the same warning was
    // being reported and read as noise.
    //
    // BUILT FROM THE SAME TWO CONSTANTS AP_SSID_CHARS IS, and not from a
    // matching pair of literals. The clock face puts this SSID on a shared row
    // and clamps it there to AP_SSID_CHARS; a format widened here to a longer
    // suffix would not truncate — ap_ssid has headroom — it would print short
    // on the panel AND in GET /ui, identically, so nothing would look wrong and
    // the one person standing at a stranded node could not find the network.
    snprintf(ap_ssid, sizeof(ap_ssid), AP_SSID_PREFIX "%.*s", AP_SSID_HEX,
             suffix.c_str());
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
    if (wifi_ssid[0]) {
        WiFi.begin(wifi_ssid, wifi_pass);
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

// ===== The notification store =====
//
// #included HERE rather than at the bottom with gpio and serial, and the
// position is load-bearing: the two screens below read this store directly.
// They need the NotifyView type complete — each renderer declares one on its
// own stack — so a forward declaration cannot stand in for the definition, and
// the include has to precede the UI section rather than follow it.
//
// The one thing it needs from further down the file is handle_body_collect(),
// which is declared just above and defined with the other handlers. Everything
// else it uses — event_add(), require_auth(), read_spiffs_file(),
// write_spiffs_file_atomic(), skill_register(), SkillEndpoint, ui_force,
// ui_force_net — is already defined above this line.
#include "skills/notify.cpp"

// ===== On-device UI =====
//
// A 240x135 panel and a 56-key QWERTY, driven through M5Cardputer. Three rules
// hold this together and none of them is optional.
//
// 1. THE DISPLAY IS TOUCHED FROM loop() AND NOWHERE ELSE. Every HTTP handler in
//    this file runs on the AsyncTCP task — core 1, priority 10, higher than the
//    Arduino loop — and will preempt a half-finished draw. M5GFX keeps mutable
//    state (the active font, the text datum, padding, the SPI transaction) that
//    a second writer corrupts silently: the symptom is garbled glyphs and a
//    hung bus, not a crash with a stack trace. The sibling ats-mini port added a
//    mutex to make two writers safe; here there is only ever one writer, which
//    is cheaper and cannot be got wrong by a later handler that forgets to take
//    the lock. GET /ui therefore READS ui state and never draws.
//
// 2. NO CANVAS, NO SPRITE. A full-screen 240x135 16bpp sprite is 64 800 bytes.
//    This board has no PSRAM at all, so that comes out of roughly 320KB of DRAM
//    — alongside AsyncTCP's 16KB stack, the TLS-free but still hungry web
//    server, and the OTA path, which has to survive a 3.3MB image streaming
//    through it. Instead every field remembers what it last rendered and a tick
//    that changes nothing costs no SPI at all. See ui_draw_field().
//
// 3. WHAT IS ON THE SCREEN IS ANSWERABLE OVER HTTP. There is no camera on this
//    node. GET /ui and the panel derive every value from the same formatters —
//    ui_field() for the label/value screens, ui_status_row() for the clock
//    face's two rows — or, where a value only exists once it has been laid out,
//    from the draw caches themselves. Either way the endpoint reports the
//    screen's real content rather than a second, parallel description of it
//    that can drift. A screen that grows a shape of its own grows a `content`
//    kind and an object to match; it does not get described twice.

// Layout, in pixels, for the 240x135 panel in landscape.
//
//   0..18    header: screen title left, unread badge, network summary right
//   20       rule
//   26..114  five rows, 18px apart
//   116      rule
//   121..129 footer: the key legend for the current screen
//
// STATUS does not use the five rows, the lower rule or the footer; it is a
// clock face and owns everything below the header. See the clock geometry
// further down.
#define UI_ROWS         5
#define UI_HDR_Y        2
#define UI_RULE1_Y     20
#define UI_ROW0_Y      26
#define UI_ROW_PITCH   18
#define UI_ROW_H       18
#define UI_RULE2_Y    116
#define UI_FOOT_Y     121
// The header row, left to right, on the 240px-wide panel. Four spans that never
// overlap, so no one of them can erase another's pixels:
//
//   4..70    screen title, left at x=4, padding 67
//   72..78   badge dot, r=3 about cx=75
//   81..100  badge count, left at x=81, padding 20
//   102..235 network summary, right at x=236, padding 134
//
// Padding is what erases the previous value here, and M5GFX only fills the
// REMAINDER of the band — and only when `padding > string width`, STRICTLY
// greater. A string as wide as its padding erases nothing at all and the
// previous value's tail stays on the glass, with no error and no clipping to
// show for it. So every width below clears the widest string its field can
// ever hold: 66px for "HARDWARE" and "MESSAGES", and 14px for "9+".
//
// The network field is sized to the widest string its FORMAT can hold, which
// is not the same thing. ui_net_summary() prints at most "AP " and a dotted
// quad, so the structural bound is "AP 255.255.255.255" at 133px. What this
// build can actually reach is narrower — ap_start() pins the AP to AP_IP_A..D
// and refuses to raise it at all if the pin fails, so the AP branch tops out
// at "AP 172.31.157.1" (109px) and the STA branch at 111px. The 124 this field
// used before was therefore already sufficient, and no erase was ever missed.
// 134 buys the field the freedom to keep working if AP_IP_A..D is ever moved
// to wider octets, which is a one-line edit four hundred lines from here with
// nothing to connect it to this number.
//
// UI_PANEL_W exists so the seams below can be asserted at compile time; the
// draw calls themselves still take the width from M5.Display at runtime. It is
// the landscape geometry setRotation(1) produces on this board, which is what
// the layout note above already assumes throughout.
#define UI_PANEL_W     240
#define UI_PANEL_H     135
#define UI_HDR_TITLE_W  67
#define UI_BADGE_CX     75
#define UI_BADGE_R       3
#define UI_BADGE_X      81
#define UI_BADGE_W      20
#define UI_HDR_NET_W   134
// Two columns on a data row: a dim label, then the value.
#define UI_LABEL_X      4
#define UI_LABEL_W     50
#define UI_VALUE_X     56
#define UI_VALUE_W    182
#define UI_MENU_X       8
#define UI_MENU_R_X   232

// The header's spans are asserted and not merely written down, on the precedent
// of the footer legend's counters further down this file. Every seam up there
// is one or two pixels wide and one #define away from a silent overlap: M5GFX
// reports neither a collision nor a clip, so the first evidence of one would be
// a smear on the glass that no build and no endpoint can show. ui_begin()'s
// splash held the title's old width for exactly this reason — the invariant had
// already drifted at one call site before it was ever written down.
static_assert(UI_LABEL_X + UI_HDR_TITLE_W <= UI_BADGE_CX - UI_BADGE_R,
              "the header title's erase band reaches into the badge dot");
static_assert(UI_BADGE_CX + UI_BADGE_R < UI_BADGE_X,
              "the badge dot reaches into the badge count's erase band");
static_assert(UI_BADGE_X + UI_BADGE_W <= UI_PANEL_W - UI_LABEL_X - UI_HDR_NET_W,
              "the badge count's erase band reaches into the network summary");
static_assert(UI_PANEL_W - UI_LABEL_X - UI_HDR_NET_W >= 0,
              "the network summary's erase band starts off the left of the panel");
// The dot is the one header element drawn as geometry rather than as a font
// cell, so it is the one that can leave the row without a glyph metric stopping
// it. UI_HDR_Y + 8 is the centre line of Font2's 16px cell.
static_assert(UI_HDR_Y + 8 + UI_BADGE_R < UI_RULE1_Y,
              "the badge dot crosses the rule under the header");

// ---- The clock face ----
//
// STATUS is the screen this node sits on, so it is a clock and not a table: a
// large HH:MM with the seconds beside it, the date under that, and two rows at
// the bottom for how the node is reached. That stack needs 106 rows and only 95
// are free between the two rules, so this one screen drops the lower rule and
// the footer legend and takes 21..134 instead. ui_draw_frame() and ui_tick()
// are where the two are suppressed.
//
// EVERY WIDTH BELOW IS A SUM OVER THE M5GFX FONT TABLES, and deliberately not a
// runtime measurement. textWidth() and textLength() run the display's shared
// UTF-8 decoder against the one M5.Display object and may only be called from
// the loop task; GET /ui runs on AsyncTCP and measures nothing, and keeping the
// figures constant is what lets it stay that way. They come from
// lgfx/Fonts/Font7srle.h (Font7), Font32rle.h (Font4) and Font16.h (Font2).
// RLEfont and BMPfont set x_advance to exactly the width-table entry and
// text_width() sums those advances, so a string's width is the sum of its
// glyphs' entries and there is no letter spacing to add.
//
// FONT0 IS THE EXCEPTION AND IT COMES FROM SOMEWHERE ELSE: lgfx/Fonts/
// glcdfont.h for the bitmaps, and the GLCDfont constructed in lgfx_fonts.cpp
// for the metrics. It is the one font here whose x_advance does NOT come from a
// table through updateFontMetric() — that override takes its metrics pointer
// unnamed and only range-checks the code point, so the advance stays whatever
// getDefaultMetric() put there, which is the font's own `width`. That is not a
// footnote: it is the entire basis of the fixed-pitch bound the bottom rows'
// erase band is derived from.
//
// THESE ARE A SNAPSHOT OF ONE M5GFX RELEASE, hand-transcribed, and every
// assertion below is expressed in terms of the copies rather than the tables.
// fontHeight() is not constexpr and the width tables are not reachable from a
// constant expression, so nothing here can check itself: a library bump that
// retabulated a font would relay this screen silently, with a clean build and
// every assert still passing.
//
// AND M5GFX IS NOT PINNED. platformio.ini asks for `^0.2.26`, which is a caret
// RANGE and not a pin: any 0.2.x above that resolves, so the library can move
// under these numbers on a machine that fetches dependencies afresh, with no
// edit to this repository to notice. This comment used to say the version was
// pinned and that whoever moved the pin would re-check the metrics — there is
// no such moment. Whoever narrows that range, or finds it has drifted,
// re-checks the metrics below against lgfx/Fonts/ in the same commit.
//
// FONT7 IS THE SEVEN-SEGMENT FACE, and its table is why the clock row carries
// no padding at all: a digit and '-' both advance 32 and ':' advances 12, so
// "00:00" and "--:--" are the same 140px. Every string that row can ever hold
// covers exactly the same pixels, and the per-glyph opaque background erases
// all of them — there is nothing left for a padding number to fill, and any
// number written here would be a figure that never does anything.
//
// Font7 also has REAL GLYPHS ONLY FOR THE DIGITS, ':', '.' AND '-'. Its own
// header says the rest print as a space, and they do it while still advancing
// 12px, so a word placed on that row renders as a run of invisible cells. That
// is why the pre-sync notice lives in the date row and never in the clock's.
#define UI_F7_H         48   // chr_hgt_f7s
#define UI_F4_H         26   // chr_hgt_f32
#define UI_F2_H         16   // chr_hgt_f16
#define UI_F0_H          8   // the 6x8 GLCD font the footer legend uses
// FONT0 HAS NO WIDTH TABLE AT ALL, which is the one metric here that is a
// property of the font's TYPE rather than a transcribed number. It is a
// GLCDfont, whose updateFontMetric() only range-checks the code point and never
// touches the metrics, so every glyph keeps the default x_advance — the font's
// own `width` field, 6. A Font0 string is therefore exactly 6px a character
// whatever characters it holds. Font2 and Font4 are BMP/RLE fonts and do carry
// per-glyph tables, which is why their widths below are sums and this one is a
// multiplication.
#define UI_F0_W          6   // GLCDfont width field; fixed pitch, no table
#define UI_F7_DIGIT_W   32   // widtbl_f7s, and the same entry for '-'
#define UI_F7_COLON_W   12   // widtbl_f7s
#define UI_F4_DIGIT_W   14   // widtbl_f32; '-' there is 8, which is what the pad covers
#define UI_CLOCK_Y      22
#define UI_CLOCK_W     (4 * UI_F7_DIGIT_W + UI_F7_COLON_W)
#define UI_SEC_W       (2 * UI_F4_DIGIT_W)
#define UI_CLOCK_GAP    12
// Neither x is written down, because the clock and the seconds are centred as
// one block: changing the gap or either width has to move both, and a literal
// would move only one of them.
#define UI_CLOCK_X     ((UI_PANEL_W - (UI_CLOCK_W + UI_CLOCK_GAP + UI_SEC_W)) / 2)
#define UI_SEC_X       (UI_CLOCK_X + UI_CLOCK_W + UI_CLOCK_GAP)
// BOTTOMS FLUSH, NOT BASELINES. fontHeight() returns the em box and this uses
// it as one, so the bottom of the seconds' 26px cell lands on the bottom of the
// clock's 48px cell. The SECONDS' digits then sit about six pixels above the
// clock's, because Font4 leaves 7 rows of descender under its baseline (19 of
// 26) where Font7 leaves 1 (47 of 48). Aligning the two baselines instead would
// be UI_CLOCK_Y + 47 - 19 = 50, and a 26px cell starting there ends at 75 —
// five rows inside the date band below. The mismatch is the deliberate choice;
// the collision is what it avoids.
#define UI_SEC_Y       (UI_CLOCK_Y + UI_F7_H - UI_F4_H)
#define UI_DATE_Y       71
// THE FORMAT AND ITS WIDEST RENDERING ARE ONE FACT AND LIVE TOGETHER. The pad
// below has to cover the widest string this row can ever hold, and that number
// is a property of the format string, not of the field — change one and the
// other is wrong. "%a %d %b %Y" tops out at "Wed 28 May 2026": Wed is the
// widest weekday at 52px in Font4, May the widest month at 48, the day and year
// are 14px digits. The pre-sync notice is 170. Widening the format — "%A %d %B
// %Y" would put "Wednesday 05 August 2026" on this row — means re-deriving
// UI_DATE_MAX_W from the tables above, and the assertion below is what stops
// the field silently overrunning its erase band in the meantime.
#define UI_DATE_FMT    "%a %d %b %Y"
#define UI_DATE_MAX_W  199
#define UI_DATE_W      236
#define UI_CROW0_Y      99
#define UI_CROW1_Y     117
// THE TWO BOTTOM ROWS ARE THREE MUTUALLY EXCLUSIVE MODES, AND THE MODE PICKS
// THE FONT FOR BOTH ROWS AT ONCE. ui_status_row_mode() is the only place that
// decides which mode is up, and each of its two callers asks it once per pass
// and passes the answer down — ui_draw_clock() to both the font and the
// formatter, GET /ui to both rows.
//
//   AP up      row 0   SSID, session state and password      Font0
//              row 1   the auth token                        Font0
//   connected  row 0   signal strength                       Font2
//              row 1   the mDNS name and port                Font2
//   offline    row 0   says so                               Font2
//              row 1   the keys this screen answers to       Font2
//
// AP FIRST, BEFORE THE ASSOCIATED TEST, and the order is the whole point. A
// setup AP raised from the menu on a node that is already on WiFi puts the
// radio in WIFI_AP_STA and leaves both up until the session expires, so the two
// states are simultaneously true — and the panel is the only place the AP
// password exists at all. Testing the link first would show signal strength to
// somebody standing in front of a node whose password they have no other way to
// read. (ui_net_summary(), which feeds the HEADER, tests the link first and so
// can show the STA address above rows showing AP credentials. That is a
// pre-existing disagreement between the two and not one this introduces.)
//
// FONT0 ON THE AP ROWS IS A CONSTRAINT, NOT A PREFERENCE. The token is 32 hex
// characters; Font2 gives a hex digit 8px, 'a'..'e' 7px and 'f' 6px, so the bare
// token is 242px for the one this node happens to carry and 256px if it were
// all digits. The panel is 240 wide and these rows start at x=4, so Font2
// cannot put the token on the glass at all, with or without a word in front of
// it. Font0 has no width table (see UI_F0_W), so "token " plus 32 characters is
// a CONSTANT 228px whatever the token says — that is what makes the row safe
// rather than lucky, and it leaves six pixels against this band. The AP's other
// row carries an SSID, a session state and a twelve-character password on one
// line, which is 260px in Font2 and 216 in Font0. The connected and offline
// rows are short — 155px at their widest — and stay in the 16px face.
#define UI_CROW_BYTES   40
// The keys this screen answers to. The commit that made this screen a clock
// face took the footer legend off it for the room, and said the hint would come
// back into a bottom row when there was nothing else to say; the offline mode
// is that case, and this is the hint. ENT and ` both open the menu
// and `;`/`.` do nothing here, so naming ENT alone is the honest short form.
// Deliberately NOT the sibling firmware's "hold KEY 3s for setup AP": there is
// no such gesture in this firmware, where the AP is raised from the menu.
#define UI_CROW_KEYS   "ENT menu  , / dim/bright"
// token_load() makes a token of sixteen random bytes as hex. The precision at
// the format site is what makes this 32 a property of the ROW rather than a
// note about today: a token restored from SPIFFS is whatever that file holds.
#define UI_CROW_TOKEN_LEN  32
#define UI_CROW_TOKEN_PFX  "token "
// What GET /ui puts where the panel puts the AP password. Deliberately no
// longer than a real password, so the endpoint's copy of the row and the
// panel's are the same shape and neither truncates where the other does not.
#define UI_CROW_PW_HIDDEN  "(panel only)"
// AP row 0 is "<ssid>  <session state>  pw <password>". It is the one row in
// this file where three variable parts share a line, so its length is a
// computed budget asserted against UI_CROW_BYTES rather than an eyeballed one,
// and the two variable lengths are clamped by precisions at the format site.
//
// THE FIXED PARTS ARE MACROS THE FORMAT STRINGS ARE ASSEMBLED FROM, not
// separators counted by eye against literals seven hundred lines away. Every
// term below is either sizeof() over the exact text that gets printed or a
// named length that its own producer also uses.
#define UI_STRLEN(s)        ((int)(sizeof(s) - 1))
#define UI_CROW_AP_SEP      "  "
#define UI_CROW_AP_PWPFX    "pw "
#define UI_CROW_AP_LEFT     "m left"    // after the minute count
#define UI_CROW_AP_PERM     "stays up"
#define UI_CROW_AP0_TEMP_FMT \
    "%.*s" UI_CROW_AP_SEP "%lu" UI_CROW_AP_LEFT UI_CROW_AP_SEP \
    UI_CROW_AP_PWPFX "%.*s"
#define UI_CROW_AP0_PERM_FMT \
    "%.*s" UI_CROW_AP_SEP UI_CROW_AP_PERM UI_CROW_AP_SEP \
    UI_CROW_AP_PWPFX "%.*s"
// Two digits for the minute count: AP_SESSION_MS is asserted below to keep
// ap_minutes_left() inside them. The permanent wording is asserted to be no
// longer than the countdown it shares the budget with.
#define UI_CROW_AP_STATE_CHARS  (2 + UI_STRLEN(UI_CROW_AP_LEFT))
#define UI_CROW_AP0_CHARS  (AP_SSID_CHARS + 2 * UI_STRLEN(UI_CROW_AP_SEP) + \
                            UI_CROW_AP_STATE_CHARS + \
                            UI_STRLEN(UI_CROW_AP_PWPFX) + AP_PASSWORD_CHARS)
// THE ERASE BAND IS SIZED FROM THE BUFFER AND NOT FROM TODAY'S STRINGS, which
// is what makes the Font0 rows provable instead of re-derived by hand every
// time somebody edits a format string forty lines away. Font0 is fixed pitch,
// so the widest thing a UI_CROW_BYTES buffer can EVER put on the glass in it is
// (UI_CROW_BYTES - 1) glyphs, and that bound holds for any string anyone writes
// into it later. The Font2 rows get no such gift — Font2's widest advance is
// 10px, so 39 glyphs there could be 390px — and are still summed by hand below.
#define UI_CROW_W      234
#define UI_CROW_MAX_W_F0  (UI_F0_W * (UI_CROW_BYTES - 1))
// The widest string the Font2 modes can hold, summed over widtbl_f16:
//
//   "signal -128 dBm"       100px  — WiFi.RSSI() is an int8_t, so four
//                                    characters is the whole numeric range
//   "seed-0000.local:8080"  135px  — mdns_name is "seed-" plus four LOWERCASE
//                                    hex, and the widest hex glyphs there are
//                                    the digits at 8px, not the letters
//   UI_CROW_KEYS            155px  <- the widest, and the one below is asserted
//   "offline"                42px
#define UI_CROW_MAX_W_F2  155
// There is deliberately no single UI_CROW_MAX_W any more. It named one number
// when one font drew both rows; with two fonts a single figure would hide which
// of them it came from, and the two are established in completely different
// ways — one from the buffer, one by hand from a width table.
//
// The band a mode change has to clear by hand: both rows, at the height of the
// TALLER of the two fonts. See ui_draw_clock() for why a font change cannot be
// left to ui_draw_field()'s padding.
#define UI_CROW_BAND_H (UI_CROW1_Y + UI_F2_H - UI_CROW0_Y)

// Which of the three the bottom rows are in. The value is remembered across
// draws — see ui_clock_rows_drawn — so the enum is here with the geometry it
// decides rather than beside the formatter that reads it.
enum ui_crow_mode_t {
    UI_CROW_AP = 0,   // the provisioning AP is up, with or without a link
    UI_CROW_LINK,     // associated, no AP
    UI_CROW_OFF       // neither
};
// What the face reads before the first NTP sync. Named rather than written out
// at each site because GET /ui decides whether the clock is synced by comparing
// what the panel actually drew against this string, instead of asking the C
// library a second time from a different task and possibly getting a different
// answer than the glass has on it.
#define UI_CLOCK_UNSYNCED  "--:--"
#define UI_SEC_UNSYNCED    "--"
#define UI_NTP_NOTICE      "waiting for NTP"

static_assert(UI_CLOCK_X >= 0,
              "the clock block is wider than the panel");
static_assert(UI_CLOCK_Y > UI_RULE1_Y,
              "the clock's cell crosses the rule under the header");
static_assert(UI_SEC_X + UI_SEC_W <= UI_PANEL_W,
              "the seconds' erase band leaves the right edge of the panel");
static_assert(UI_CLOCK_Y + UI_F7_H <= UI_DATE_Y,
              "the clock's cell reaches into the date row");
static_assert(UI_SEC_Y >= UI_CLOCK_Y,
              "the seconds sit above the top of the clock's cell");
static_assert(UI_DATE_Y + UI_F4_H <= UI_CROW0_Y,
              "the date's cell reaches into the row below it");
// THE ROW CELLS ARE SPELT AT THE TALLER FONT ON PURPOSE. The bottom rows are
// Font2 in two of their three modes and Font0 in the third, so UI_F2_H here is
// the binding case and the Font0 mode has eight rows of slack under it. These
// two would still pass at UI_F0_H and would then be asserting nothing about the
// mode that actually fills the band.
static_assert(UI_CROW0_Y + UI_F2_H <= UI_CROW1_Y,
              "the two bottom rows overlap in the taller of their two fonts");
static_assert(UI_CROW1_Y + UI_F2_H <= UI_PANEL_H,
              "the lower row leaves the bottom of the panel in the taller of its two fonts");
static_assert(UI_F2_H >= UI_F0_H,
              "Font0 is now the taller row font, so every cell asserted at UI_F2_H "
              "and the mode-change clear derived from it are all understated");
static_assert(UI_PANEL_W / 2 - UI_DATE_W / 2 >= 0 &&
              UI_PANEL_W / 2 + UI_DATE_W / 2 <= UI_PANEL_W,
              "the date's centred erase band leaves the panel");
static_assert(UI_LABEL_X + UI_CROW_W <= UI_PANEL_W,
              "a bottom row's erase band leaves the right edge of the panel");
// THE ONES THAT A CONTENT CHANGE BREAKS, rather than more restatements of the
// geometry. An erase band narrower than the widest string its field can hold
// leaves the previous value's tail on the glass, and M5GFX reports neither a
// clip nor an overrun for it — the same silent failure the header's spans are
// asserted against. These are the only numbers on this screen that a format
// string or a message text can invalidate from somewhere else in the file.
static_assert(UI_DATE_W >= UI_DATE_MAX_W,
              "the date's erase band is narrower than the widest date its format can render");
// Font0: derived from the buffer, so nothing anybody writes into a bottom row
// can outrun the band. Binding at equality today by construction, and the drift
// it catches is a buffer grown without the band.
static_assert(UI_CROW_W >= UI_CROW_MAX_W_F0,
              "a Font0 bottom row can outrun its erase band: the row buffer holds "
              "more fixed-pitch glyphs than UI_CROW_W has room for");
// Font2: NOT the same quality of guard, and worth saying so plainly rather than
// letting the pair below look like the one above. The band carries 79px of
// slack over the sum, so this assert will not fire for any realistic edit; the
// length tie under it is what actually stands between the legend and a silent
// overrun, and it catches only a change of LENGTH. A 24-character rewrite could
// reach 240px — Font2's widest advance is 10 — and nothing here would say so.
// Two of the four strings the sum is taken over are runtime-composed and tied
// to nothing at all; they are bounded by their formats' numeric ranges, which
// is an argument and not a check. Making this as strong as the Font0 bound
// needs the width table transcribed into a constexpr array here, which is a
// second copy of the thing this whole block already warns about drifting.
static_assert(UI_CROW_W >= UI_CROW_MAX_W_F2,
              "a Font2 bottom row's erase band is narrower than the widest string it can hold");
static_assert(UI_STRLEN(UI_CROW_KEYS) == 24,
              "the key legend changed length; re-sum UI_CROW_MAX_W_F2 over "
              "widtbl_f16 before changing this number to match");
// The token row is the one string on this screen that a single byte of prefix
// pushes past its buffer, and it would be cut identically on the panel and in
// GET /ui — so nothing would look wrong and the token would simply be short.
static_assert((int)sizeof(UI_CROW_TOKEN_PFX) + UI_CROW_TOKEN_LEN <= UI_CROW_BYTES,
              "the token row does not fit its buffer and would be truncated silently");
// GET /ui's copy of the AP row has to be the same shape as the panel's, and the
// redaction goes through the SAME %.*s the password does — so a placeholder
// longer than a password would not widen the row, it would be cut down to
// twelve characters and read as something else entirely.
static_assert((int)sizeof(UI_CROW_PW_HIDDEN) - 1 <= AP_PASSWORD_CHARS,
              "the redaction is longer than the password it stands in for and would "
              "be truncated by the row's own precision");
static_assert(AP_SSID_CHARS < (int)sizeof(ap_ssid) &&
              AP_PASSWORD_CHARS < (int)sizeof(ap_password),
              "an AP row length budget is longer than the buffer it describes");
static_assert(UI_CROW_AP0_CHARS < UI_CROW_BYTES,
              "the AP's SSID, session state and password no longer fit one row buffer");
// The session state's budget is the countdown's length, so the permanent
// wording has to fit inside it, and the minute count has to stay at two digits.
static_assert(UI_STRLEN(UI_CROW_AP_PERM) <= UI_CROW_AP_STATE_CHARS,
              "the permanent AP's wording is longer than the countdown whose budget "
              "UI_CROW_AP0_CHARS gives the session state");
static_assert(AP_SESSION_MS <= 99UL * 60UL * 1000UL,
              "a session over 99 minutes gives ap_minutes_left() a third digit and "
              "overruns the two UI_CROW_AP_STATE_CHARS budgets for it");
// WHY THE LOWER CHROME GOES, asserted rather than only asserted in prose, so
// that a stack moved back up cannot leave two suppressions in place with
// nothing left to justify them. The rule would fall in the two-pixel gap
// between the bottom rows and read as a divider that separates nothing; the
// footer legend's 8px band overlaps the lower row outright.
//
// THE SECOND ONE IS SPELT AT UI_F0_H AND NOT AT UI_F2_H, which is the opposite
// choice from the cell asserts above and for the opposite reason. Font2 is on
// the RIGHT of that comparison, where a taller font makes the claim EASIER: at
// UI_F2_H it reads 121 < 133 and would go on passing while the Font0 mode's
// lower row ended at 125 and left the footer band clear. At UI_F0_H it reads
// 121 < 125 — four pixels of margin instead of twelve, and true in both fonts.
static_assert(UI_CROW0_Y + UI_F2_H <= UI_RULE2_Y && UI_RULE2_Y < UI_CROW1_Y,
              "the lower rule no longer falls between the clock face's bottom rows");
static_assert(UI_CROW1_Y < UI_FOOT_Y + UI_F0_H && UI_FOOT_Y < UI_CROW1_Y + UI_F0_H,
              "the footer legend no longer overlaps the clock face's lower row in its shorter font");

// ---- Notification geometry ----
//
// THE SOURCE FIRMWARE'S NUMBERS DO NOT SURVIVE THE TRIP AND M5GFX WILL NOT SAY
// SO. It clips a rect or a string that leaves the panel silently — no error, no
// return value, no assert — so a straight port of a 320x170 layout onto this
// 240x135 one produces a wrong screen and a clean build. Every constant below
// was re-fitted rather than copied. For the record, what the originals do here:
// the selection bar runs 74px off the right edge, list row 4 lands at y=136 on
// a panel whose last row is 134, the selection band for row 3 paints over the
// footer rule, the age column's erase band (264..310) is entirely off-panel,
// the card is 68px too wide, its fade envelope leaves the panel on both axes,
// and its 276px text column is wider than this whole display.
//
// WHAT DOES CARRY OVER IS THE PIXEL BUDGET PER CHARACTER. M5GFX ships the same
// font tables as TFT_eSPI — verified in lgfx/Fonts/: Font2 is 16px tall with a
// widest glyph of 10px ('M' and 'W'), Font4 is 26px tall with a widest of 25px
// ('@'; 'M' is 21). So the columns are re-fitted and the glyph widths are not
// re-derived.

// The list reuses the seed's own rows — ui_row_y() and UI_ROW_H, the same
// geometry the menu draws on — rather than porting a second row pitch that
// would not fit. Only the columns within a row are new.
//
// Three of them, and 240px does not stretch to a worst case in all three. The
// age column does get one: 32px against a measured 24px for the widest string
// it can hold ("59m"; "99d" is 23px). The other two are fitted to what actually
// appears and ellipsise beyond it — 74px of source takes "! home-rig" at 62px
// and around ten ordinary characters, 118px of title takes about sixteen. A
// 16-character source of nothing but 'M' would need 169px and is cut; the JSON
// still carries it whole, which is the division of labour this seed already
// has between the panel and GET /notify.
#define MSG_SRC_X      4
#define MSG_SRC_W     74
#define MSG_TITLE_X   82
#define MSG_TITLE_W  118
#define MSG_AGE_R    236   /* right-aligned to this edge */
#define MSG_AGE_W     32
// Longest string a list cell holds: a 40-character title plus an ellipsis.
#define MSG_CELL_LEN  45

// Card geometry: 224x92 at (8,21), between the header rule and the footer.
//
// THE WIDTH SHRANK AND THE HEIGHT VERY NEARLY DID NOT, and that asymmetry is
// forced by the fonts. The card's contents are one Font2 line, one Font4 line
// and two more Font2 lines — 16+26+16+16 = 74px of glyphs that do not scale
// with the panel. Scaling the source's 100px card down in proportion with the
// panel gives 89px, which after borders and gaps cannot hold its own text. So
// the card keeps its stack and gives up width, which is the axis where a
// smaller panel really does mean less room for characters.
//
// The vertical budget is exact and there is nothing spare in it. The header
// rule is at UI_RULE1_Y=20 and the footer text at UI_FOOT_Y=121, so the card
// owns rows 21..120 — 100 rows. The fade envelope is MSG_CARD_H + 4*MSG_PEEK
// tall (see ui_draw_card for where the 4 comes from), which is exactly 100 at
// these values. Raising either constant paints over the footer.
#define MSG_CARD_X     8
#define MSG_CARD_Y    21
#define MSG_CARD_W   224
#define MSG_CARD_H    92
#define MSG_PAD_L     10   /* clears the 3px accent bar with 7px to spare */
#define MSG_PAD_R      8
#define MSG_ACCENT_W   3
#define MSG_PEEK       2   /* offset of each card peeking out behind */
// The stack inside the card, as offsets from its top edge. Font heights are
// fixed; these gaps are the only slack there is, and they are what pays for the
// 8px the card lost against the source's 100.
#define MSG_SRC_Y      4   /* Font2, so rows +4..+19  */
#define MSG_TITLE_Y   24   /* Font4, so rows +24..+49 */
#define MSG_BODY1_Y   54   /* Font2, so rows +54..+69 */
#define MSG_BODY2_Y   72   /* Font2, so rows +72..+87, leaving a 4px margin */

// Arrival: three frames of rising blend plus a few pixels of upward travel.
// 40ms a frame, so the whole thing is over in 120ms — present enough to read as
// an arrival, short enough that it never delays the first look.
#define MSG_FADE_STEPS 3
#define MSG_FADE_MS   40
#define MSG_TINT_A    26   /* card fill: level colour at ~10% over black */
#define MSG_BORDER_A 102   /* card border: the same colour at ~40% */

// How often the screen is allowed to reconsider itself. Everything on it
// changes at 1Hz at most (the clock), and the tick is cheap only because of the
// per-field caches, so five times a second is responsive without being busy.
#define UI_TICK_MS    200

// Backlight. 0 is a legitimate value and means the panel is dark but still
// being drawn to — the state survives into /ui so a dark screen is
// distinguishable from a dead one.
#define UI_BRIGHT_DEFAULT 96
#define UI_BRIGHT_STEP    32

// 16-bit 565 colour, computed here rather than pulled from a library constant
// so the palette is legible in one place.
static constexpr uint16_t ui_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static const uint16_t COL_BG     = ui_rgb(0, 0, 0);
static const uint16_t COL_TEXT   = ui_rgb(220, 220, 220);
static const uint16_t COL_DIM    = ui_rgb(120, 120, 120);
static const uint16_t COL_RULE   = ui_rgb(60, 60, 60);
static const uint16_t COL_ACCENT = ui_rgb(60, 200, 110);

// One colour per notification level. The palette above is monochrome apart from
// the single accent, so these three are the only saturated things this firmware
// puts on the glass and they only ever appear on a notification — which is what
// makes a red card mean something from across a room.
static const uint16_t COL_INFO = ui_rgb(80, 150, 225);
static const uint16_t COL_WARN = ui_rgb(230, 175, 55);
static const uint16_t COL_CRIT = ui_rgb(235, 75, 60);

static uint16_t ui_level_color(uint8_t level) {
    switch (level) {
    case NOTIFY_CRIT: return COL_CRIT;
    case NOTIFY_WARN: return COL_WARN;
    default:          return COL_INFO;
    }
}

// Composite `fg` over `bg` at `alpha`/255 and hand back the RGB565 result.
//
// M5GFX has no equivalent — there is no alphaBlend anywhere in the library
// tree — so a seed that wants a tint or a fade has to bring its own. The
// argument order is the one TFT_eSPI uses, alpha first, so a call reads the
// same here as in the firmware this is ported from.
//
// It is NOT bit-identical to TFT_eSPI's, on purpose. That one works on the
// packed value and scales red and blue by `alpha >> 2` over 64, which makes
// alpha 255 return 30/62/30 out of a possible 31/63/31 — the endpoint of a
// fade is not quite the colour the fade was aimed at, and any test asserting
// "fully opaque means fg" fails against it. Unpacking the three channels and
// blending each as a weighted sum costs nothing at the sizes involved, cannot
// underflow, rounds to nearest, and lands exactly on bg at alpha 0 and exactly
// on fg at alpha 255. Everything in between differs from TFT_eSPI by at most
// one step per channel, which is below what the panel resolves.
//
// ui_rgb() above already packs 5/6/5 in the order M5GFX expects, so the two
// halves of the palette agree and nothing converts anything.
//
// Called by ui_draw_card(), for the tint, the border, the accent bar and both
// text colours — five blends a fade frame.
static uint16_t ui_blend(uint8_t alpha, uint16_t fg, uint16_t bg) {
    uint32_t a = alpha;
    uint32_t ia = 255u - a;
    uint32_t r = ((((uint32_t)fg >> 11) & 0x1F) * a +
                  (((uint32_t)bg >> 11) & 0x1F) * ia + 127u) / 255u;
    uint32_t g = ((((uint32_t)fg >>  5) & 0x3F) * a +
                  (((uint32_t)bg >>  5) & 0x3F) * ia + 127u) / 255u;
    uint32_t b = (( (uint32_t)fg        & 0x1F) * a +
                  ( (uint32_t)bg        & 0x1F) * ia + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Text anchors. Spelled out here because M5GFX keeps them in a namespace that
// no using-directive drags into global scope, and the short TL_DATUM spellings
// are only reachable from inside the library's own namespaces.
static const uint8_t UI_TL = lgfx::textdatum::top_left;
static const uint8_t UI_TR = lgfx::textdatum::top_right;
static const uint8_t UI_TC = lgfx::textdatum::top_center;

enum ui_screen_t {
    UI_STATUS = 0,
    UI_MENU,
    UI_NETWORK,
    UI_SYSTEM,
    UI_HARDWARE,
    UI_MESSAGES,   // the queue as a scrolling list
    UI_MESSAGE,    // one message as a card
    UI_SCREEN_COUNT
};

// DECLARED WITHOUT A BOUND SO THAT A MISSING ENTRY IS A COMPILE ERROR. Written
// `[UI_SCREEN_COUNT]`, as these were, a short initialiser is perfectly legal
// C++ — the remaining elements are value-initialised to null pointers — so
// adding a screen and forgetting its name here builds clean under -Wall -Wextra
// and then hands a null to snprintf's %s on the first tick that draws the
// header. Letting the initialiser set the size and asserting the size is what
// makes the two tables and the enum impossible to get out of step.
static const char *ui_screen_name[] = {
    "status", "menu", "network", "system", "hardware", "messages", "message"
};
static const char *ui_screen_title[] = {
    "STATUS", "MENU", "NETWORK", "SYSTEM", "HARDWARE", "MESSAGES", "MESSAGE"
};
static_assert(sizeof(ui_screen_name) / sizeof(ui_screen_name[0]) == UI_SCREEN_COUNT,
              "ui_screen_name is missing an entry for a screen in ui_screen_t");
static_assert(sizeof(ui_screen_title) / sizeof(ui_screen_title[0]) == UI_SCREEN_COUNT,
              "ui_screen_title is missing an entry for a screen in ui_screen_t");

// What the menu can do. Everything here is reachable from the keyboard alone,
// which is the point: the AP entry in particular has to work on a node whose
// network nobody can reach.
enum ui_action_t {
    UI_ACT_SCREEN = 0,   // arg is the ui_screen_t to open
    UI_ACT_AP,           // raise the provisioning AP (raise-only; see ui_menu_activate)
    UI_ACT_BACKLIGHT     // panel dark / panel lit
};

struct UiMenuItem {
    const char *title;
    ui_action_t action;
    int arg;
};

// MESSAGES WENT IN AT THE TOP, NOT ON THE END, and that is a change to a menu
// that already existed rather than an addition beside it: every entry below
// moved down one, and the cursor a fresh boot starts on is now Messages where
// it used to be Network. Deliberate, and still right now that the header
// carries a badge — but for a different reason than the one it went in for.
// The badge is what gets a message noticed; this row is where the reader is
// sent once it has, and it is the only entry here whose state changes without
// the user having touched anything. Everything below it is configuration, which
// a user goes looking for and can find by looking. Landing a fresh boot's
// cursor on the one row that may have something new on it costs the rest
// nothing.
//
// Nothing keys off the old numbering. The indices are used in exactly three
// places and all three take them from this table: ui_activate() by lookup,
// ui_window() by position, and GET /ui's menu.selected, which reports whatever
// is here. The one literal index in the file — ui_handle_key()'s Back row,
// which puts the cursor on Messages — is written against this order and
// commented with the name it means.
static const UiMenuItem ui_menu[] = {
    {"Messages",  UI_ACT_SCREEN,    UI_MESSAGES},
    {"Network",   UI_ACT_SCREEN,    UI_NETWORK},
    {"System",    UI_ACT_SCREEN,    UI_SYSTEM},
    {"Hardware",  UI_ACT_SCREEN,    UI_HARDWARE},
    {"Setup AP",  UI_ACT_AP,        0},
    {"Backlight", UI_ACT_BACKLIGHT, 0}
};
#define UI_MENU_COUNT ((int)(sizeof(ui_menu) / sizeof(ui_menu[0])))
// THE MENU SCROLLS NOW. It used to carry a static_assert that it must fit
// UI_ROWS, whose whole job was to make growing it past five entries fail at
// compile time rather than by quietly dropping the sixth off the bottom of a
// five-row screen. Messages is that sixth entry, so the assert has been paid
// off rather than deleted: ui_menu_first below is the window offset it demanded
// and the footer carries the position counter. There is no longer an upper
// bound on this table.

// ---- UI state ----
//
// All of it single-word scalars, written from loop() and read from loop() plus
// the AsyncTCP task in handle_ui(). Aligned word loads and stores do not tear,
// so the endpoint's worst case is a value one tick (200ms) out of date, which
// for a status report is a non-problem.
//
// This block used to claim that nothing anywhere on the /ui path was a String a
// reader could catch mid-reassignment. That was wrong, and it was wrong in the
// dangerous direction: ui_field() reads the stored WiFi credentials, which the
// provisioning POST rewrites from the AsyncTCP task. Those are now fixed buffers
// — see wifi_ssid above — precisely so the claim can be true.
//
// ui_status_row() also reads ap_ssid and ap_password, which were the last two
// Strings on the /ui path THAT A WRITER REASSIGNS. Their write patterns differ
// and the difference is why they were not the same risk. ap_password is rolled
// by ap_start() and cleared by ap_stop(), both on the loop task, so a raise or
// drop concurrent with a report on AsyncTCP was a genuine reassignment race —
// narrowed only by /ui redacting it before the read. ap_ssid, despite what this
// ledger once said, is NOT written in ap_start()/ap_stop() at all: it is set
// once in wifi_setup() (before the web server exists) and never touched again,
// so it never actually raced. Both are fixed buffers now regardless.
//
// THIS LEDGER USED TO END "SO THE WHOLE /ui PATH IS STRING-FREE BY
// CONSTRUCTION". It is not, and it was not when that was written. Three Strings
// are read on this path today and all three are safe for the SAME reason, which
// is not "no String" but "no reassignment after server.begin()":
//
//   mdns_name    set once in wifi_setup(); read by UI_NETWORK row 3 and now by
//                the clock face's connected row.
//   auth_token   set once in token_load(), which setup() runs before
//                server.begin(); read by the clock face's AP row.
//   WiFi.SSID()  returns a String BY VALUE — a fresh object per call, so there
//                is nothing for another task to free under it. It is a heap
//                allocation on every tick of UI_NETWORK, which is why our own
//                copy is preferred, not a race.
//
// So the property to keep is narrower than the old sentence and worth stating
// exactly: nothing read from AsyncTCP may be a String that a writer reassigns
// after the web server starts. A String written once before that, or returned
// by value, is fine; a fixed buffer is required only where a writer exists.
static bool ui_ready = false;          // panel initialised and drawable
// What M5GFX's panel autodetection actually answered, as opposed to what
// M5.getBoard() reports after cfg.fallback_board has papered over a failure.
// See ui_begin() for why only one of those two can report a failure at all.
static bool ui_board_detected = false;
// Whether a keyboard reader was installed. Deliberately a separate flag from
// the one above: the reader is chosen by M5.getBoard(), so it is installed
// whenever the fallback applies, INCLUDING when detection failed. Folding the
// two together would mean an honest "not detected" silently disabled a keyboard
// that works.
static bool ui_keyboard_enabled = false;
static ui_screen_t ui_screen = UI_STATUS;
static int ui_menu_index = 0;
// Where the menu's visible window starts, and where it started when the panel
// was last painted.
//
// THE SECOND ONE IS NOT REDUNDANT AND THE SCREEN IS WRONG WITHOUT IT. The menu
// repaints only the two rows whose highlight changed, and that optimisation
// compares against the selected ROW rather than the selected entry. A window
// that scrolls moves five entries under five rows while the selected row stays
// exactly where it was — the sixth entry arriving under a cursor already on the
// bottom row is the ordinary case — so the row test alone sees nothing moving,
// skips the repaint, and leaves the previous window's text on the glass under a
// correctly-placed bar. Comparing the window against what was drawn is what
// catches it.
static int ui_menu_first = 0;
static int ui_menu_first_drawn = -1;
// The message list's selection and window. The list is the messages followed by
// a Back row, so the selection runs 0..notify_count() inclusive.
static int ui_msg_sel = 0;
static int ui_msg_sel_drawn = -1;
static int ui_msg_first = 0;
static int ui_msg_first_drawn = -1;
// How many messages were in the list when it was last painted. A message
// arriving or expiring shifts every row's content without moving the selection
// or the window, so this is the third thing that has to force a relayout.
static int ui_msg_items_drawn = -1;
// THE CARD HOLDS AN ID, NOT AN INDEX. Messages arrive and expire while it is on
// screen and every one of those shifts the list under it; an index would
// silently start pointing at a different message. Zero means no subject.
static uint32_t ui_msg_id = 0;
// How far into the arrival fade the card is. MSG_FADE_STEPS means settled.
static uint8_t ui_card_fade = MSG_FADE_STEPS;
static uint8_t ui_brightness = UI_BRIGHT_DEFAULT;
// What "on" means for the backlight toggle, so turning it off and on again
// returns to the brightness the user had chosen rather than to the default.
static uint8_t ui_brightness_on = UI_BRIGHT_DEFAULT;
// Written on the loop task by ui_handle_key(), read on AsyncTCP by handle_ui().
static volatile char ui_last_key = 0;
static volatile unsigned long ui_last_key_ms = 0;

// Per-field render caches: what is currently on the glass, so a tick that
// changes nothing costs no SPI.
static char ui_cache_hdr[16];
static char ui_cache_net[24];
// The unread badge is a dot and a count that always move together, so one
// remembered value covers both. It is not a string cache: the dot has no string
// to key on. Quantised the same way the drawn value is, so everything above
// nine compares equal. -1 is a state nothing can quantise to.
static int ui_badge_drawn = -1;
// Sized to hold a whole value rather than a prefix of one: the change test is a
// strncmp against the cache, so a cache shorter than the strings it stores
// would silently stop repainting fields that differ only past its end.
static char ui_cache_label[UI_ROWS][16];
static char ui_cache_value[UI_ROWS][48];
static char ui_cache_foot[48];
// Which ROW currently carries the menu's selection bar, or -1. A row and not an
// entry index: with a window the two stopped being the same number, and the
// only question this answers is which row has to be un-highlighted.
static int ui_cache_sel = -1;
// The message list's three columns per row, and the card's two wrapped body
// lines. Separate from the caches above rather than sharing them: a list row
// has three fields where a data row has two, and a 40-character title plus an
// ellipsis does not fit ui_cache_label's 16 bytes.
//
// Nothing needs clearing when a screen changes. Every transition goes through
// ui_goto(), which raises ui_force, which repaints the frame and forces every
// field on the new screen — so whatever the previous screen left in here is
// overwritten before it can be compared against.
static char ui_msg_cell[UI_ROWS][3][MSG_CELL_LEN];
static char ui_card_row[3][48];                    // source, age, title
// The empty-queue notice, which needs its own cache because it is centred
// across columns the row fields also write to. See ui_draw_msglist().
static char ui_cache_note[16];
// THE WRAPPED BODY LINES ARE ALSO WHAT GET /ui REPORTS. The raw body is 96
// characters and every buffer on the /ui path is 48, so reporting the body
// through ui_field() would cut it at 47 while the panel showed all of it over
// two lines — a report that disagrees with the glass. These two are what the
// glass actually has on it. See handle_ui() for why the endpoint reads this
// cache instead of wrapping the body itself.
static char ui_card_body[2][NOTIFY_BODY_LEN + 4];
// The clock face's five fields. Each is sized from the string it stores and not
// from the one next to it: ui_draw_field() compares with strncmp over cache_n-1,
// so a cache shorter than its own content silently compares a prefix and stops
// repainting on a change past the cut. The date is 15 characters either way
// ("Wed 28 May 2026" and the pre-sync notice both); 24 is headroom.
//
// THE BOTTOM ROWS' SIZE IS NO LONGER HEADROOM AND IS NOT SPELT HERE. It is
// UI_CROW_BYTES, up with the geometry, because the erase band is derived from
// it: Font0 is fixed pitch, so the buffer's length is what bounds the widest
// thing the AP rows can ever put on the glass. The tightest content is the
// token row at 39 of those 40 bytes, which is one byte of slack — a longer
// prefix than "token " truncates the token, and truncates it identically on the
// panel and in GET /ui, so nothing would look wrong. That is asserted.
//
// SEPARATE FROM ui_cache_value[] AND NOT SHARING IT. That array is written by
// ui_begin()'s splash and by three other screens, and the fields here are a
// different shape in a different font at different coordinates.
static char ui_clock_time[8];
static char ui_clock_sec[4];
static char ui_clock_date[24];
static char ui_clock_row[2][UI_CROW_BYTES];
// WHICH MODE THOSE TWO ROWS WERE LAST DRAWN IN, remembered the way the header's
// badge remembers its drawn state and for the same class of reason: the thing
// that changes is not the string, so the per-field cache cannot see it. Here it
// is the FONT. -1 is a value no mode has, so the first draw always clears.
static int ui_clock_rows_drawn = -1;
// WHETHER A DRAW HAS EVER FILLED THEM, on the same principle as ui_card_body_id
// and for a sharper reason. Only ui_draw_clock() writes those caches, and
// ui_tick() returns before reaching it whenever !ui_ready — so on a node whose
// panel autodetect failed they stay empty for the whole boot, and on every
// normal boot they are empty from the moment the web server starts until the
// first tick. An ungated read reports "" for all three, and, worse, computes
// synced=true from an empty string that is not the pre-sync placeholder: /ui
// would assert the clock had synced while /clock said it had not, on a node
// whose screen shows nothing at all. Absence is what handle_ui() reports
// instead, because an absent key reads as "not known yet" and an empty one
// reads as "the panel is showing a blank clock".
//
// One-way: set once by the first draw and never cleared. A screen change does
// not invalidate it — the caches still hold what that screen last had on it,
// which is what they are for.
static bool ui_clock_drawn = false;
// WHICH MESSAGE THOSE TWO LINES BELONG TO, and the report is worthless without
// it. The cache is only ever refilled by a draw, and a draw happens on the tick
// after the card opens — so between ui_enter_card() and the first
// ui_draw_card(), and again on every `;`/`.` step from one card to the next,
// ui_card_body still holds the PREVIOUS message's wrapped lines. A GET /ui
// landing in that window would pair this card's id and title with another
// card's body, which is worse than reporting no body at all: a reader has no
// way to tell the two apart. ui_draw_card() stamps this with the id it just
// wrapped, and handle_ui() emits the lines only while the stamp still matches.
// Zero is not a valid notification id, so an unstamped cache matches nothing.
static uint32_t ui_card_body_id = 0;

// ---- Field values ----

// THE PANEL NO LONGER SHOWS AN UPTIME ANYWHERE. ui_uptime() lived here and had
// exactly one caller, the clock face's lower row in its non-AP case; the three
// modes below give that row to the mDNS name and the key legend instead, which
// leaves the function with no caller at all and -Wunused-function would say so.
// It is removed rather than kept for a future caller. The number itself has not
// gone anywhere — GET /health and GET /firmware/version both report
// uptime_sec, and they compute it from millis() and boot_time directly rather
// than through this formatter, so nothing on the network path changes.

// One line summarising where the node can be reached, for the header.
//
// The octets are formatted by hand rather than through IPAddress::toString().
// That method builds and returns a String — a heap allocation, a copy and a
// free every time — and it runs on every UI tick on every screen, because the
// header is on every screen. It used to run TWICE per tick on STATUS, once for
// the header and once for the clock face's first row; the three row modes below
// took the second call away. That does not make it dead code, and the saving is
// one call in five per second and not a reason for anything: two callers remain
// and both matter — the header, and UI_NETWORK's `ip` row while that screen is
// up. At 5Hz this was roughly ten allocate/free pairs a second for the life of
// the boot, on a board with no PSRAM and a single 300-odd KB heap that also has
// to find a contiguous 6KB block whenever GET /skill is called. Nothing here
// needs the heap at all.
static void ui_net_summary(char *out, size_t n) {
    if (WiFi.status() == WL_CONNECTED) {
        uint32_t ip = (uint32_t)WiFi.localIP();
        snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip & 0xFF),
                 (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                 (unsigned)((ip >> 24) & 0xFF));
    } else if (ap_active) {
        uint32_t ip = (uint32_t)WiFi.softAPIP();
        snprintf(out, n, "AP %u.%u.%u.%u", (unsigned)(ip & 0xFF),
                 (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                 (unsigned)((ip >> 24) & 0xFF));
    } else {
        snprintf(out, n, "offline");
    }
}

// The right-hand half of a menu row: what the entry's state is right now, so
// "Setup AP" says whether the AP is up without having to be selected.
static void ui_menu_state(int i, char *out, size_t n) {
    out[0] = '\0';
    if (i < 0 || i >= UI_MENU_COUNT) return;
    switch (ui_menu[i].action) {
        case UI_ACT_AP:
            if (!ap_active) {
                // The two are different things and the user needs to tell them
                // apart: never asked for, versus asked for and refused.
                snprintf(out, n, "%s", ap_start_failed ? "failed" : "off");
            } else if (ap_temporary) {
                snprintf(out, n, "%lum left", ap_minutes_left());
            } else {
                snprintf(out, n, "up, open");
            }
            break;
        case UI_ACT_BACKLIGHT:
            snprintf(out, n, "%u", (unsigned)ui_brightness);
            break;
        case UI_ACT_SCREEN:
            // The Messages row carries the queue's state in full: how many are
            // unread AND how many are held. The header's badge is the thing
            // that gets a message noticed — it is on every screen and this row
            // is not — but it is quantised at "9+" and says nothing about the
            // read ones, so this is still where the queue is actually read.
            //
            // Both calls take the store's spinlock and are safe from any task,
            // which matters because GET /ui calls this from AsyncTCP.
            if (ui_menu[i].arg == UI_MESSAGES) {
                int total = notify_count();
                int unread = notify_unread_count();
                if (total == 0)      snprintf(out, n, "empty");
                else if (unread > 0) snprintf(out, n, "%d new / %d", unread, total);
                else                 snprintf(out, n, "%d read", total);
            }
            break;
        default:
            break;
    }
}

// Which of the three shapes the clock face's bottom rows are in. THE ONE PLACE
// THAT DECIDES, AND ASKED ONCE PER PASS — the answer is then passed to the
// formatter rather than recomputed by it. That is why ui_status_row() takes a
// mode instead of calling this itself: this function reads WiFi.status(), a
// live radio state, so three calls in one draw are three chances to get three
// answers, and a row drawn in one mode's font holding another mode's text is a
// failure with no error, no clip and no report behind it.
//
// Today's arrangement would survive the sloppier version for a narrow reason —
// ap_active is written only from the loop task, so the AP edge cannot land
// mid-draw, and the one pair that CAN flip under us shares a font. Neither of
// those is a property of this design, only of its current three modes: a fourth
// mode with a font of its own would break it silently. One read per pass costs
// nothing and does not depend on either.
//
// AP IS TESTED FIRST AND THE LINK SECOND. Both can be true: a setup AP raised
// from the menu on an associated node puts the radio in WIFI_AP_STA and leaves
// both up until the session expires. Signal strength is available from a dozen
// places; the AP password is available from this screen and nowhere else in the
// universe, so it wins the tie. See the geometry block for the note on the
// header disagreeing with these rows while that is true.
static ui_crow_mode_t ui_status_row_mode() {
    if (ap_active) return UI_CROW_AP;
    if (WiFi.status() == WL_CONNECTED) return UI_CROW_LINK;
    return UI_CROW_OFF;
}

// The clock face's two bottom rows, as one string each: how this node is
// reached right now. The panel draws them and GET /ui reports them, from here
// and from nowhere else, for the same reason ui_field() exists — one formatter
// means the endpoint cannot describe the screen differently from the screen.
//
// THIS IS WHERE THE PROVISIONING AP'S PASSWORD LIVES, and it is the only place
// it exists at all: rolled on every raise, never persisted, never sent
// anywhere. `redact` is what keeps it that way — the panel passes false and
// gets the password, /ui passes true and gets a placeholder. Anything secret
// that lands on these rows later must go through the same argument.
//
// THE TOKEN IS NOT REDACTED AND THAT IS NOT AN OVERSIGHT. GET /ui will not
// answer without it, so a caller reading it back out of this row already had
// it; there is nothing to leak. The password is the opposite case, which is
// why the two are treated differently on rows that sit one above the other.
// If a later row ever carries both at once, the redaction has to survive the
// merge — it is the row, not the field, that /ui serialises.
//
// While the AP is up BOTH rows are given to it, which is the same deliberate
// swap the five-row screen this replaced already made: everything else these
// rows could say is worth less than the only copy of a password that exists,
// and the token beside it is what turns a joined phone into a working client
// without a second trip to a terminal. Which kind of session it is goes beside
// the SSID, because the two behave differently and the difference matters to
// somebody standing here — a keyboard-raised AP closes on a timer, the boot AP
// stays up until the node is provisioned.
//
// Safe from either task. Everything read here is a scalar, one of the two fixed
// char buffers that exist precisely so that a String reassignment on the loop
// task cannot free an array an AsyncTCP reader is walking, or one of the two
// write-once Strings the /ui ledger up in the UI state block accounts for.
//
// `mode` IS A PARAMETER AND NOT A CALL, for the reason ui_draw_field()'s
// `force` is one: a pass has to see one answer throughout. Both callers take it
// once — the panel so the font it installs matches the text it draws, the
// endpoint so its two rows describe the same instant.
static void ui_status_row(ui_crow_mode_t mode, int row, char *out, size_t n,
                          bool redact) {
    out[0] = '\0';
    switch (mode) {
    case UI_CROW_AP:
        // ap_minutes_left() RETURNS 0 FOR A SESSION THAT DOES NOT EXPIRE, and
        // the boot AP — raised because the stored credentials did not work, and
        // the only way into the node when that happens — is exactly that
        // session. Formatting it through the countdown branch would read
        // "Seed-f1f8  0m left" to the one user who has no other way in. The
        // guard is ap_temporary and not a zero test, because zero is also what
        // the last few seconds of a real session round to.
        //
        // THE TWO PRECISIONS ARE THE BOUND, not decoration. ap_ssid and
        // ap_password are both sixteen-byte buffers with headroom, and this is
        // the one row in the file where three variable parts share a line — the
        // compiler said so, with -Wformat-truncation, before they were added.
        // They clamp to the lengths UI_CROW_AP0_CHARS was derived from, which
        // is the same trick wifi_setup() plays with %.4s and for the same
        // reason: make the invariant visible instead of implied.
        if (row == 0) {
            if (ap_temporary) {
                snprintf(out, n, UI_CROW_AP0_TEMP_FMT,
                         AP_SSID_CHARS, ap_ssid, ap_minutes_left(),
                         AP_PASSWORD_CHARS,
                         redact ? UI_CROW_PW_HIDDEN : ap_password);
            } else {
                snprintf(out, n, UI_CROW_AP0_PERM_FMT,
                         AP_SSID_CHARS, ap_ssid, AP_PASSWORD_CHARS,
                         redact ? UI_CROW_PW_HIDDEN : ap_password);
            }
        } else if (row == 1) {
            // THE PRECISION IS THE INVARIANT. UI_CROW_TOKEN_LEN is what the
            // erase band and the buffer assert are derived from, and
            // token_load() reads its token back out of SPIFFS — so clamp the
            // row to the length that was asserted rather than assume the file.
            snprintf(out, n, UI_CROW_TOKEN_PFX "%.*s", UI_CROW_TOKEN_LEN,
                     auth_token.c_str());
        }
        break;
    case UI_CROW_LINK:
        // WiFi.RSSI() RETURNS 0 WHEN NOT ASSOCIATED — not a sentinel, not a
        // very negative number, just zero, which reads as an unusually strong
        // signal. This branch is only reached with the link up, which is what
        // makes the reading mean anything; the mode test above is the gate.
        if (row == 0)      snprintf(out, n, "signal %d dBm", (int)WiFi.RSSI());
        else if (row == 1) snprintf(out, n, "%s.local:%d", mdns_name.c_str(),
                                    HTTP_PORT);
        break;
    case UI_CROW_OFF:
        // Nothing to say about how the node is reached, so the row that would
        // have said it carries the keys instead. See UI_CROW_KEYS.
        if (row == 0)      snprintf(out, n, "offline");
        else if (row == 1) snprintf(out, n, "%s", UI_CROW_KEYS);
        break;
    }
}

// The single source of truth for what a label/value screen shows. The panel
// calls this to draw a row; GET /ui calls it to report one. Both get their own
// buffers, so there is no shared string for the two tasks to race over, and the
// two can never describe the screen differently.
//
// STATUS IS NO LONGER ONE OF THEM. It is a clock face with two full-width rows
// under it, so it reports through its own object exactly as the menu, the list
// and the card do, and ui_status_row() above is its formatter — including the
// redaction that used to live here.
//
// Returns false when the screen has no row at that index (the menu has no
// fields at all — it has entries, which GET /ui reports separately).
static bool ui_field(ui_screen_t screen, int idx, char *label, size_t label_n,
                     char *value, size_t value_n) {
    label[0] = '\0';
    value[0] = '\0';
    if (idx < 0 || idx >= UI_ROWS) return false;

    switch (screen) {
    case UI_NETWORK:
        switch (idx) {
        case 0:
            snprintf(label, label_n, "ssid");
            if (WiFi.status() == WL_CONNECTED) {
                // Our own copy in preference to WiFi.SSID(), which returns a
                // String and so allocates on every tick this screen is up. The
                // two cannot disagree: the only thing that ever associates this
                // node is WiFi.begin(wifi_ssid, ...). The call is kept as a
                // fallback purely for the case of a link established by
                // something other than this firmware.
                snprintf(value, value_n, "%s",
                         wifi_ssid[0] ? wifi_ssid : WiFi.SSID().c_str());
            } else if (wifi_ssid[0]) {
                snprintf(value, value_n, "%s (down)", wifi_ssid);
            } else {
                snprintf(value, value_n, "not configured");
            }
            return true;
        case 1:
            snprintf(label, label_n, "ip");
            ui_net_summary(value, value_n);
            return true;
        case 2:
            snprintf(label, label_n, "rssi");
            if (WiFi.status() == WL_CONNECTED) {
                snprintf(value, value_n, "%d dBm", (int)WiFi.RSSI());
            } else {
                snprintf(value, value_n, "-");
            }
            return true;
        case 3:
            snprintf(label, label_n, "host");
            snprintf(value, value_n, "%s.local:%d", mdns_name.c_str(), HTTP_PORT);
            return true;
        case 4:
            snprintf(label, label_n, "ap");
            if (!ap_active) {
                snprintf(value, value_n, "%s", ap_start_failed ? "failed to start" : "off");
            } else if (ap_temporary) {
                snprintf(value, value_n, "%s  %lum left", ap_ssid,
                         ap_minutes_left());
            } else {
                snprintf(value, value_n, "%s  stays up", ap_ssid);
            }
            return true;
        default:
            return false;
        }

    case UI_SYSTEM:
        switch (idx) {
        case 0:
            snprintf(label, label_n, "seed");
            snprintf(value, value_n, "v%s", SEED_VERSION);
            return true;
        case 1: {
            snprintf(label, label_n, "slot");
            const esp_partition_t *running = esp_ota_get_running_partition();
            snprintf(value, value_n, "%s %s", running ? running->label : "unknown",
                     firmware_confirmed ? "confirmed" : "pending");
            return true;
        }
        case 2:
            snprintf(label, label_n, "heap");
            snprintf(value, value_n, "%lu KB / %lu min",
                     (unsigned long)(ESP.getFreeHeap() / 1024),
                     (unsigned long)(ESP.getMinFreeHeap() / 1024));
            return true;
        case 3:
            snprintf(label, label_n, "flash");
            snprintf(value, value_n, "%lu MB, no psram",
                     (unsigned long)(hw.flash_size / 1024 / 1024));
            return true;
        case 4:
            // The boot reading, the same number /capabilities reports. Nothing
            // re-reads the sensor here: temperatureRead() is not documented as
            // safe to call from two tasks, and /ui runs on the server's.
            snprintf(label, label_n, "temp");
            snprintf(value, value_n, "%.1f C at boot", (double)hw.temp_c);
            return true;
        default:
            return false;
        }

    case UI_HARDWARE:
        switch (idx) {
        case 0:
            snprintf(label, label_n, "board");
            snprintf(value, value_n, "%s", hw.board);
            return true;
        case 1:
            // M5's own verdict, kept separate from ours on purpose, and stated
            // as the reading it comes from rather than as a bare verdict.
            //
            // Row 0 above is this firmware's answer: an I2C probe that saw, or
            // did not see, the keyboard controller reply at 0x34. This row is
            // M5GFX's panel autodetection. They are genuinely independent — one
            // is the I2C bus, the other is the SPI panel — so when they
            // disagree, the disagreement is the bug report, and the pair is
            // worth more than either alone.
            //
            // Specifically NOT M5.getBoard(): cfg.fallback_board overwrites an
            // unknown result with the right answer before that is readable, so
            // it agrees with us even when nothing was detected at all. See
            // ui_begin().
            snprintf(label, label_n, "m5");
            snprintf(value, value_n, "%s%s",
                     ui_board_detected ? "panel: CardputerADV"
                                       : "panel: NOT DETECTED",
                     ui_keyboard_enabled ? "" : ", no keys");
            return true;
        case 2:
            snprintf(label, label_n, "cap");
            snprintf(value, value_n, "%s",
                     hw.cap_lora1262 ? "LoRa-1262" : "none");
            return true;
        case 3: {
            snprintf(label, label_n, "i2c");
            // Addresses only, no names: at 8px a glyph the row holds about
            // twenty characters and /capabilities carries the identifications.
            size_t used = 0;
            for (int i = 0; i < hw.i2c0_count && used + 5 < value_n; i++) {
                used += snprintf(value + used, value_n - used, "%s%02X",
                                 used ? " " : "", hw.i2c0[i].addr);
            }
            if (used == 0) snprintf(value, value_n, "none");
            return true;
        }
        case 4: {
            snprintf(label, label_n, "free");
            size_t used = 0;
            for (int i = 0; i < gpio_safe_pins_count && used + 4 < value_n; i++) {
                used += snprintf(value + used, value_n - used, "%sG%d",
                                 used ? " " : "", gpio_safe_pins[i]);
            }
            // Same fallback as the i2c row above. An empty value renders as a
            // blank cell, which reads as "this row failed to load" rather than
            // as the real answer, which is that there are no free pins.
            if (used == 0) snprintf(value, value_n, "none");
            return true;
        }
        default:
            return false;
        }

    // Four screens have no label/value rows at all. Status has a clock face,
    // the menu has entries, the list has messages and the card has one message,
    // and GET /ui reports each of them through its own object rather than
    // through fields[]. An empty fields[] on those four is therefore not a
    // screen failing to describe itself; doc["content"] names where its content
    // actually is.
    case UI_STATUS:
    case UI_MENU:
    case UI_MESSAGES:
    case UI_MESSAGE:
    default:
        return false;
    }
}

// The key legend, which is the only documentation a user standing in front of
// the node gets. Kept in sync with ui_handle_key() by being right next to it.
//
// THE SCROLL COUNTER LIVES HERE AND NOT IN THE HEADER. The source firmware puts
// its `n/m` in the header bar, which on this panel has nowhere to put it: the
// four spans up there — title, badge dot, badge count, network summary — are
// asserted not to touch, and what they leave between them is one pixel in two
// places and two in a third. The footer has the room, but not much of it —
// Font0 is fixed at 6px a glyph, so the 236px field holds 39 characters, and
// the longest legend below is the card's at exactly 39 with a two-digit
// counter on it:
// "; . next  ENT ack  ` keep unread  20/20" measures 234px into 236. Two
// pixels of slack, so a legend that grows by one character does not fit.
//
// A THREE-DIGIT COUNTER IS THE WAY THAT HAPPENS, and it is asserted rather
// than left to the panel. The counters come from NOTIFY_MAX and UI_MENU_COUNT;
// raising the queue to three digits pushes that legend to 246px, which the
// field does not clip and the 240px panel silently cuts off at the right edge
// — the user loses the end of the sentence that tells them backtick keeps a
// message unread. Fail the build instead and let whoever raises the cap shorten
// the legend in the same commit.
static_assert(NOTIFY_MAX <= 99,
              "the card's footer legend is 39 of the 39 characters Font0 fits "
              "across 236px with a two-digit counter; a three-digit queue "
              "overflows it off the panel — shorten the legend first");
static_assert(UI_MENU_COUNT <= 99,
              "the menu's footer legend needs a two-digit counter to fit");
//
// The card's legend is where the difference between the two ways of leaving it
// is documented, because it is the only place it can be. Enter acknowledges and
// backtick does not, and a user who cannot be told that will assume looking at
// a message is what marks it read.
//
// AN EMPTY LEGEND MEANS NO FOOTER AT ALL, not an empty footer. ui_tick() skips
// the field entirely when this comes back empty, and it has to: the field's
// padding is what erases the band, so drawing an empty string there would wipe
// whatever the screen has put in those rows. The clock face is exactly that
// case — it owns the panel down to the last row and has no legend.
static void ui_footer(ui_screen_t screen, char *out, size_t n) {
    switch (screen) {
    case UI_STATUS:
        // No legend on the clock face. Its lower row occupies the footer band —
        // and now carries the legend itself, as UI_CROW_KEYS, but only in the
        // offline mode, where that row has nothing better to say. On a node
        // that is on the network or offering a setup AP the keys are still
        // undocumented on the glass, because the row is spent on the address
        // and the credentials.
        out[0] = '\0';
        break;
    case UI_MENU:
        snprintf(out, n, "; . move  ENT select  ` back  %d/%d",
                 ui_menu_index + 1, UI_MENU_COUNT);
        break;
    case UI_MESSAGES: {
        int items = notify_count();
        // The Back row is not a message and must not be counted as one. Sitting
        // on it reports the last message's position rather than a position one
        // past the end, which is what the source does and reads correctly as
        // "you are at the bottom of this many".
        snprintf(out, n, "; . move  ENT open  ` back  %d/%d",
                 items == 0 ? 0 : (ui_msg_sel < items ? ui_msg_sel + 1 : items),
                 items);
        break;
    }
    case UI_MESSAGE: {
        int idx = notify_index_of(ui_msg_id);
        snprintf(out, n, "; . next  ENT ack  ` keep unread  %d/%d",
                 idx < 0 ? 0 : idx + 1, notify_count());
        break;
    }
    default:
        snprintf(out, n, "` back  ENT menu  , / dim/bright");
        break;
    }
}

// ---- Scrolling a list longer than the screen ----

// Where the visible window starts, given where the selection is. Move it only
// when the selection would otherwise leave it, which keeps a step inside the
// window down to a two-row repaint instead of a full one.
//
// The source firmware spells this against a pair of file-scope globals. Here it
// takes the position and returns the new one, because the seed has no such
// globals and inventing them would land dead state in this commit for the sake
// of a four-line function; whichever screen grows a scrolling list can then own
// its own pair rather than sharing one with every other screen.
//
// The four clamps run in the source's order and the order is load-bearing. The
// two selection clamps go first so the window follows the cursor; the
// end-of-list clamp then pulls a window that ran off the bottom back on; and
// the floor at zero runs last because it is the one that has to win — when
// `count` is smaller than `rows` the previous line computes a negative start,
// and this is what turns that into a list pinned at the top rather than one
// indexed from before its own beginning.
//
// Two callers now, each with its own pair of offsets exactly as this comment
// anticipated: the menu (ui_menu_first) and the message list (ui_msg_first).
static int ui_window(int sel, int first, int count, int rows) {
    if (sel < first) first = sel;
    if (sel >= first + rows) first = sel - rows + 1;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    return first;
}

// ---- Fitting text to a column ----
//
// Columns are pixel budgets, and what goes in them is whatever an agent put in
// the JSON. Everything below measures with M5GFX's own font tables — the same
// ones drawString() renders from — so a string is cut where it actually stops
// fitting rather than at a character count guessed from an average glyph.
//
// TWO M5GFX CALLS DO THE WHOLE JOB AND NEITHER IS OBVIOUS FROM ITS NAME:
//
//   textWidth(str, font)  — measures against `font` without disturbing the
//                           live one. Takes the font as an argument and works
//                           on a copy of the metrics, so it leaves no font
//                           state behind. It still runs the display's shared
//                           UTF-8 decoder, so "no font state" is not the same
//                           as "safe from any task" — see the warning below.
//
//   textLength(str, width) — returns THE BYTE OFFSET OF THE FIRST CHARACTER
//                           THAT DOES NOT FIT in `width`. That offset is the
//                           cut point, already UTF-8 decoded with real per
//                           glyph advances, and already on a code-point
//                           boundary because the library captures the start of
//                           each character before it decodes it. There is no
//                           reason to walk a string a character at a time and
//                           add up widths; this is that loop, written by people
//                           who own the font tables.
//
// textLength() has two edges worth writing down. It takes NO font argument — it
// measures with the live `_font` and rewrites the display's own metrics in
// place — so the font has to be installed around the call and put back after.
// And it advances the display's shared UTF-8 decoder, which is fine on a
// complete well-formed string and is why nothing below ever hands it a
// fragment.
//
// EVERYTHING IN THIS SECTION IS FOR THE loop() TASK AND NOTHING ELSE MAY CALL
// IT. There is one M5.Display and these borrow its state: ui_fit_bytes()
// installs a font on it, measures, and puts the caller's font back, and both
// measuring calls advance its shared UTF-8 decoder. ui_ellipsis() and
// ui_wrap2() inherit all of that. None of it is guarded by anything.
//
// The task that will want to break these rules is the AsyncTCP one, because
// that is where a notification arrives and where formatting its card text is
// the obvious thing to do. Do not. Run it there while loop() is inside
// ui_draw_field() and the draw picks up whichever font was installed last,
// which is not the one it asked for. THE PANEL DOES NOT RECOVER FROM THAT ON
// ITS OWN: ui_draw_field() writes the string it was given into the field cache
// whether or not it came out right, so the next tick compares equal, skips the
// repaint, and the mis-rendered field stays mis-rendered until something
// changes its text. A garbled row that never redraws is the symptom; a fit
// call from the wrong task is the cause.
//
// The rule for the notification store, then: the web task owns the data and
// nothing else, and every one of these calls happens inside ui_tick().

// Copy at most `len` bytes of `src` into `dst`, bounded by the buffer, and
// never split a well-formed UTF-8 sequence at the end: back off over any
// continuation bytes at the cut. Returns how many bytes were written.
//
// The buffer bound is the one that needs this. Cut points from textLength() are
// already on a boundary; a destination that simply ran out of room is not.
//
// WELL-FORMED IS A PRECONDITION, NOT SOMETHING THIS CHECKS. The back-off looks
// at the byte AT the cut and stops when that byte is not a continuation byte,
// which says nothing about whether the sequence before the cut was ever
// complete. ui_copy_utf8(dst, n, "ab\xC3", 3) copies all three bytes and hands
// back a string ending in a lone lead byte, because src[3] is the terminator
// rather than a continuation byte. Malformed input passes through.
//
// It is left that way deliberately, because validating here would not buy the
// thing it looks like it buys. The damage a bad sequence does is to the
// display's shared UTF-8 decoder, which setFont() pointedly does not reset
// (LGFXBase.cpp:2539) — but ui_ellipsis() and ui_wrap2() both measure the RAW
// `src` with textWidth()/textLength() before a byte is ever copied, and those
// run the same decodeUTF8() over the same bad bytes. The decoder is already
// mid-sequence by the time control reaches here; a clean copy afterwards
// cannot undo it. The reach of that is also short: decodeUTF8() only enters
// its state machine for bytes with the high bit set, and any ASCII byte drops
// it straight back to state0, so a stranded decoder is corrected by the very
// next plain character and only ever mangles a following string that itself
// begins non-ASCII. Rejecting malformed input belongs where it enters, at the
// JSON, not in a copy helper three layers down.
static size_t ui_copy_utf8(char *dst, size_t n, const char *src, size_t len) {
    if (n == 0) return 0;
    if (len > n - 1) len = n - 1;
    while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

// Measure where `src` stops fitting in `width`, with `font` installed for the
// duration and the caller's font restored afterwards.
static size_t ui_fit_bytes(const char *src, const lgfx::IFont *font,
                           int32_t width) {
    const lgfx::IFont *prev = M5.Display.getFont();
    M5.Display.setFont(font);
    int32_t cut = M5.Display.textLength(src, width);
    M5.Display.setFont(prev);
    return cut < 0 ? 0 : (size_t)cut;
}

// Copy `src` into `dst`, cut to `max_px` with a trailing ellipsis if it does
// not fit. A column too narrow for the ellipsis itself is hard-truncated
// instead — three dots and nothing else says less than two real characters.
static void ui_ellipsis(char *dst, size_t n, const char *src,
                        const lgfx::IFont *font, int32_t max_px) {
    if (n == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    if (M5.Display.textWidth(src, font) <= max_px) {
        ui_copy_utf8(dst, n, src, strlen(src));
        return;
    }

    int32_t ell_w = M5.Display.textWidth("...", font);
    bool with_ellipsis = (ell_w < max_px);
    size_t cut = ui_fit_bytes(src, font,
                              with_ellipsis ? max_px - ell_w : max_px);

    // The ellipsis needs room in the destination as well as on the panel.
    size_t cap = with_ellipsis ? (n >= 5 ? n - 4 : 0) : n - 1;
    if (cut > cap) cut = cap;

    size_t out = ui_copy_utf8(dst, n, src, cut);
    // Hang the ellipsis off a word rather than off a gap.
    while (out > 0 && dst[out - 1] == ' ') out--;
    dst[out] = '\0';
    if (with_ellipsis) snprintf(dst + out, n - out, "...");
}

// Break `src` over two lines of `max_px`, on a space where there is one. The
// second line carries the ellipsis, so a body too long for the card ends
// visibly rather than just stopping.
//
// LINE TWO RESUMES FROM WHAT LINE ONE ACTUALLY TOOK, NOT FROM WHERE THE PIXELS
// RAN OUT. Those are two different offsets and confusing them drops text on the
// floor. `brk` is a panel measurement; ui_copy_utf8() answers to the buffer as
// well, and clamps to n1 - 1 when the buffer is the tighter of the two. Resume
// at src + brk after that and the bytes between what l1 could hold and what the
// column could show appear on neither line, with no ellipsis anywhere to admit
// it — a narrow l1 against a wide column silently eats a run of characters out
// of the middle of the body. Using the return value costs nothing and is
// already on a code-point boundary. ui_ellipsis() handles the same collision
// with its `cap` clamp; this is that idea applied to the other helper.
//
// Called by ui_draw_card() for the message body, and by nothing else. The two
// lines it produces are cached in ui_card_body[] and are what GET /ui reports,
// because they are what is on the glass.
static void ui_wrap2(const char *src, char *l1, size_t n1, char *l2, size_t n2,
                     const lgfx::IFont *font, int32_t max_px) {
    if (n1) l1[0] = '\0';
    if (n2) l2[0] = '\0';
    if (!src || !src[0] || n1 == 0) return;

    // Fits the column, which says nothing about fitting l1: a short buffer
    // still overflows onto line two, and it is line two's ellipsis that keeps
    // the overflow honest.
    if (M5.Display.textWidth(src, font) <= max_px) {
        size_t took = ui_copy_utf8(l1, n1, src, strlen(src));
        const char *tail = src + took;
        while (*tail == ' ') tail++;
        if (*tail) ui_ellipsis(l2, n2, tail, font, max_px);
        return;
    }

    size_t fit = ui_fit_bytes(src, font, max_px);
    // Break on the last space at or before the cut. The scan stops short of
    // index zero on purpose: breaking on a leading space would put nothing at
    // all on the first line. A single word wider than the line has no space to
    // break on either way, and is cut where it stopped fitting.
    //
    // THE SCAN INCLUDES src[fit] AND MUST. `fit` is the offset of the first
    // character that does NOT fit, so a word ending exactly at the column edge
    // puts a space right there — and that space is the ideal break, because the
    // line then ends exactly where the word does. Starting the scan at fit - 1
    // steps over it and falls back to the previous space, dropping a whole word
    // onto the next line for no reason. This was written that way and only
    // became visible when the card gave ui_wrap2() its first caller: a 206px
    // body line was rendering "412 GB transferred to the" and pushing "array"
    // down, having measured 205px of the 206 it was allowed.
    size_t brk = fit;
    for (size_t i = fit + 1; i > 1; i--) {
        if (src[i - 1] == ' ') { brk = i - 1; break; }
    }

    size_t took = ui_copy_utf8(l1, n1, src, brk);
    const char *rest = src + took;
    while (*rest == ' ') rest++;
    ui_ellipsis(l2, n2, rest, font, max_px);
}

// ---- Drawing ----

// Repaint one text field, and only if its content changed.
//
// The opaque background plus a fixed padding width is what erases the previous
// value, so nothing here ever needs fillScreen and nothing ever flickers.
//
// THE ERASE IS `padding` WIDE AND ONE CELL OF THE FONT BEING DRAWN TALL, and
// the second half of that is a real bound, not a detail. M5GFX fills the
// remainder of the pad at the height it gets from the font's metrics, so a
// field redrawn in a SHORTER font than it last used repaints only the top of
// its old band and leaves the rest of the previous glyphs on the glass. The
// cache cannot catch it, because the font is not part of the key. (It also
// fills only when `padding` is STRICTLY greater than the string's width: a
// string exactly as wide as its pad erases nothing at all.)
//
// The cache keys on the text alone, so a field whose colours change, or whose
// FONT changes, but whose string does not, will not repaint on its own —
// MOVING A FIELD ONTO OR OFF A COLOURED GROUND, OR INTO A DIFFERENT FACE, IS
// THE CALLER'S PROBLEM TO SOLVE, and there are three answers to it in this
// file. A screen transition sets ui_force and every field repaints. The menu
// selection bar does not: it slides one row on every arrow key with no
// transition and no ui_force, and pays for that with the explicit `moved` test
// below, which fills the new ground and blanks the two affected rows' caches by
// hand so the fields draw themselves back over it. The clock face's bottom rows
// change FONT with their mode, and pay for it the same way: ui_draw_clock()
// remembers the mode it drew, clears the band and blanks those caches when it
// moves. All three do the caller's half by hand; none of them is something this
// function can do for them.
//
// `force` is a PARAMETER AND NOT A READ OF ui_force, and that is the whole
// reason ui_tick() can clear the flag before it draws instead of after. One
// pass has to see one value throughout: the pass repaints the frame when it is
// forced, which blanks the panel, and every field it then draws has to be told
// to repaint over that blank ground. A field that consulted the global would
// find it already cleared, find its cache still matching what it drew before
// the fillScreen, and return without drawing — a black panel with two rules on
// it and no text anywhere.
static void ui_draw_field(bool force, char *cache, size_t cache_n,
                          const char *text,
                          int32_t x, int32_t y, const lgfx::IFont *font,
                          uint16_t color, uint8_t datum, uint16_t padding,
                          uint16_t bg) {
    if (!force && strncmp(cache, text, cache_n - 1) == 0) return;
    snprintf(cache, cache_n, "%s", text);
    M5.Display.setFont(font);
    M5.Display.setTextDatum(datum);
    M5.Display.setTextColor(color, bg);
    M5.Display.setTextPadding(padding);
    M5.Display.drawString(text, x, y);
    M5.Display.setTextPadding(0);
}

static int32_t ui_row_y(int row) {
    return UI_ROW0_Y + row * UI_ROW_PITCH;
}

// Everything the frame owns rather than a field: the two rules. Drawn on a full
// repaint only, because nothing ever erases them.
//
// Two screens are exceptions and take the lower rule with them.
//
// The card: that rule sits at y=116, inside the rows the card's fade envelope
// repaints (21..120), so on the card screen it would survive only as an 8px
// stub either side of the card. A rule with a hole in it reads as a drawing
// bug; the footer below is perfectly legible without one.
//
// The clock face: the rule lands in the two-pixel gap between its bottom two
// rows, where it separates two halves of one thing. See the clock geometry and
// the assertions over it.
static void ui_draw_frame(ui_screen_t screen) {
    M5.Display.fillScreen(COL_BG);
    M5.Display.drawFastHLine(0, UI_RULE1_Y, M5.Display.width(), COL_RULE);
    if (screen != UI_MESSAGE && screen != UI_STATUS) {
        M5.Display.drawFastHLine(0, UI_RULE2_Y, M5.Display.width(), COL_RULE);
    }
}

// ---- The message list ----
//
// The queue newest first, then a Back row, over the seed's ordinary five rows.
//
// Unlike every other screen this one repaints without any input, because the
// age column moves on its own. The split is: the bars and every cell are forced
// whenever the selection, the window or the item count moved — a solid bar
// inverts the ground under text ui_draw_field() would otherwise consider
// unchanged — and on every other pass the per-field caches decide, which in
// practice means the ages, once a minute, and nothing else.
static void ui_draw_msglist(bool force) {
    int items = notify_count();
    int count = items + 1;  // the messages, then Back

    // The selection has to be clamped here and not only where the keys move it,
    // because the list shrinks on its own. Messages expire and get evicted with
    // nobody touching the keyboard, and ui_window() clamps the WINDOW without
    // ever looking at the selection it was given — so a cursor left past the
    // end produces a screen with no bar on it at all and a footer counting past
    // the total. `items` is the Back row, which is always a valid position.
    if (ui_msg_sel > items) ui_msg_sel = items;
    if (ui_msg_sel < 0) ui_msg_sel = 0;

    ui_msg_first = ui_window(ui_msg_sel, ui_msg_first, count, UI_ROWS);

    // The item count is in this test because a message arriving or expiring
    // shifts every row's content by one without touching the selection or the
    // window. It is also what keeps the empty-queue notice below honest.
    bool relayout = force || ui_msg_sel != ui_msg_sel_drawn ||
                    ui_msg_first != ui_msg_first_drawn ||
                    items != ui_msg_items_drawn;
    if (relayout) {
        for (int r = 0; r < UI_ROWS; r++) {
            int32_t y = ui_row_y(r);
            M5.Display.fillRect(0, y - 1, M5.Display.width(), UI_ROW_H,
                                (ui_msg_first + r == ui_msg_sel) ? COL_ACCENT
                                                                 : COL_BG);
        }
        // THE LOCAL, NEVER ui_force. The source firmware assigns its global
        // display_force here, which works there because its fields read that
        // global. Ours cannot: ui_tick() takes and clears ui_force before any
        // drawing and every field reads the local copy, so assigning the global
        // would do nothing on this pass and buy a redundant full repaint on the
        // next one. Every cell below is now on a ground it did not have, and
        // this is what tells them so.
        force = true;
    }

    for (int r = 0; r < UI_ROWS; r++) {
        int i = ui_msg_first + r;
        int32_t y = ui_row_y(r);
        bool sel = (i == ui_msg_sel);
        uint16_t bg = sel ? COL_ACCENT : COL_BG;
        // Sized from the cache they end up in, so the pixel budget stays the
        // only thing that decides where a string is cut.
        char src[MSG_CELL_LEN], title[MSG_CELL_LEN], age[MSG_CELL_LEN];
        bool unread = false;

        src[0] = title[0] = age[0] = '\0';
        if (i == items) {
            snprintf(src, sizeof(src), "Back");
        } else if (i < items) {
            NotifyView v;
            if (notify_view(i, v)) {
                unread = v.unread;
                // An unread critical is the one thing that has to be findable
                // without reading the rows, and a leading mark does it without
                // spending a fourth colour on a list that has no colour.
                //
                // The source uppercases the whole column here. Not ported: this
                // firmware's screens are mixed case throughout, and the sender
                // chose the string.
                char mark[NOTIFY_SOURCE_LEN + 2];
                if (v.level == NOTIFY_CRIT && v.unread) {
                    snprintf(mark, sizeof(mark), "! %s", v.source);
                } else {
                    snprintf(mark, sizeof(mark), "%s", v.source);
                }
                ui_ellipsis(src, sizeof(src), mark, &fonts::Font2, MSG_SRC_W);
                ui_ellipsis(title, sizeof(title), v.title, &fonts::Font2,
                            MSG_TITLE_W);
                notify_age_str(v.age_s, age, sizeof(age));
            }
        }

        // Black on the bar; off it, unread messages are primary text and read
        // ones have already had their turn.
        uint16_t c_pri = sel ? COL_BG : (unread ? COL_TEXT : COL_DIM);
        uint16_t c_sec = sel ? COL_BG : COL_DIM;
        ui_draw_field(force, ui_msg_cell[r][0], sizeof(ui_msg_cell[r][0]), src,
                      MSG_SRC_X, y, &fonts::Font2, c_sec, UI_TL, MSG_SRC_W, bg);
        ui_draw_field(force, ui_msg_cell[r][1], sizeof(ui_msg_cell[r][1]), title,
                      MSG_TITLE_X, y, &fonts::Font2, c_pri, UI_TL, MSG_TITLE_W, bg);
        ui_draw_field(force, ui_msg_cell[r][2], sizeof(ui_msg_cell[r][2]), age,
                      MSG_AGE_R, y, &fonts::Font2, c_sec, UI_TR, MSG_AGE_W, bg);
    }

    // An empty queue says so instead of showing a screen with one Back row on
    // it. Drawn on row 1, which the loop above has just blanked.
    //
    // The else branch clears the cache rather than drawing an empty string over
    // it, and the difference matters. This field is centred with a 200px pad,
    // so drawing "" through it would erase 20..220 of row 1 — straight through
    // the "Back" that lands there the moment the queue is no longer empty.
    // Every transition out of empty changes `items`, which forces the relayout
    // above, which has already refilled that row: the pixels are gone, and all
    // that is left to do is stop the cache from claiming otherwise.
    if (items == 0) {
        ui_draw_field(force, ui_cache_note, sizeof(ui_cache_note), "no messages",
                      M5.Display.width() / 2, ui_row_y(1), &fonts::Font2,
                      COL_DIM, UI_TC, 200, COL_BG);
    } else {
        ui_cache_note[0] = '\0';
    }

    ui_msg_sel_drawn = ui_msg_sel;
    ui_msg_first_drawn = ui_msg_first;
    ui_msg_items_drawn = items;
}

// ---- One message, as tinted glass ----
//
// The fill is the level colour blended onto black at about a tenth and the
// border at about four tenths — real compositing through ui_blend(), so the
// card sits over the ground rather than replacing it. The 3px bar down the left
// edge is the level colour at full strength and is the only saturated thing on
// the screen.
//
// During the fade the whole block is repainted, because the card is also
// moving; once settled nothing repaints but the age, and that goes through
// ui_draw_field() against the tint rather than against black.
//
// NO SPRITE, and there must not be one. The whole thing is fillRect, drawRect
// and drawString straight to the panel — about 50 400 pixels a frame at this
// size, which is ~20ms of a 40ms frame on this board's 40MHz panel bus, and not
// one byte of it allocates. See rule 2 at the top of this section for why a
// canvas is not available to reach for.
static void ui_draw_card(bool force) {
    NotifyView v;
    int idx = 0, total = 0;
    // ENTRY, POSITION AND DEPTH OUT OF ONE ACQUISITION, and it must stay that
    // way. Resolving the id to an index and then reading at that index is two
    // acquisitions with a gap, and a message arriving in the gap shifts the
    // list — so the card would draw one message's text under another message's
    // counter. The false return is "gone", expired or evicted while it was on
    // screen; ui_tick() has already left for the list on this same pass, so
    // there is nothing to draw.
    if (!notify_view_by_id(ui_msg_id, v, &idx, &total)) return;

    uint16_t level = ui_level_color(v.level);

    bool fading = (ui_card_fade < MSG_FADE_STEPS);
    uint8_t step = fading ? (uint8_t)(ui_card_fade + 1) : (uint8_t)MSG_FADE_STEPS;
    int32_t x = MSG_CARD_X;
    int32_t y = MSG_CARD_Y + (MSG_FADE_STEPS - step) * MSG_PEEK;

    // Everything the card draws ramps together, so it arrives as one object
    // rather than as a border that appears before its contents.
    uint16_t tint   = ui_blend((uint8_t)(MSG_TINT_A * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t edge   = ui_blend((uint8_t)(MSG_BORDER_A * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t accent = ui_blend((uint8_t)(255 * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t c_pri  = ui_blend((uint8_t)(255 * step / MSG_FADE_STEPS), COL_TEXT, tint);
    uint16_t c_sec  = ui_blend((uint8_t)(255 * step / MSG_FADE_STEPS), COL_DIM, tint);

    if (fading || force) {
        // Erase the travel envelope: the card's footprint plus the two peeks
        // behind it and the travel it still has to rise. The height is
        // MSG_CARD_H + 4*MSG_PEEK because the deepest pixel over the whole fade
        // is the p=2 card on the first frame — that frame starts 2*MSG_PEEK
        // low and the peek is another 2*MSG_PEEK down from its own top edge.
        // 228x100 once per fade frame and then never again while this card is
        // up, which is exactly the 21..120 the geometry note budgets for it.
        M5.Display.fillRect(MSG_CARD_X, MSG_CARD_Y, MSG_CARD_W + 2 * MSG_PEEK,
                            MSG_CARD_H + 4 * MSG_PEEK, COL_BG);

        // Two more cards behind, when there are two more to be behind: the
        // stack `;` and `.` rotate through. Outlines only, dimmer with depth.
        if (total > 1) {
            for (int p = 2; p >= 1; p--) {
                uint8_t a = (uint8_t)(MSG_BORDER_A / (p + 1) * step / MSG_FADE_STEPS);
                M5.Display.drawRect(x + p * MSG_PEEK, y + p * MSG_PEEK,
                                    MSG_CARD_W, MSG_CARD_H,
                                    ui_blend(a, level, COL_BG));
            }
        }

        M5.Display.fillRect(x, y, MSG_CARD_W, MSG_CARD_H, tint);
        M5.Display.drawRect(x, y, MSG_CARD_W, MSG_CARD_H, edge);
        M5.Display.fillRect(x, y, MSG_ACCENT_W, MSG_CARD_H, accent);
        // The local again, for the same reason as in the list above: the fill
        // took the text with it and every field below has to be told.
        force = true;
    }

    int32_t tx = x + MSG_PAD_L;
    int32_t rx = x + MSG_CARD_W - MSG_PAD_R;
    int32_t text_w = MSG_CARD_W - MSG_PAD_L - MSG_PAD_R;
    // What is left of the top line once the age has taken its right-hand end,
    // with a 6px gap between them. 168px, which is wider than a 16-character
    // source at the widest glyph Font2 has, so this column never ellipsises.
    int32_t src_w = text_w - MSG_AGE_W - 6;

    char src[NOTIFY_SOURCE_LEN + 4], age[16];
    ui_ellipsis(src, sizeof(src), v.source[0] ? v.source : "device",
                &fonts::Font2, src_w);
    notify_age_str(v.age_s, age, sizeof(age));
    ui_draw_field(force, ui_card_row[0], sizeof(ui_card_row[0]), src,
                  tx, y + MSG_SRC_Y, &fonts::Font2, c_sec, UI_TL,
                  (uint16_t)src_w, tint);
    ui_draw_field(force, ui_card_row[1], sizeof(ui_card_row[1]), age,
                  rx, y + MSG_SRC_Y, &fonts::Font2, c_sec, UI_TR,
                  MSG_AGE_W, tint);

    char title[48];
    ui_ellipsis(title, sizeof(title), v.title, &fonts::Font4, text_w);
    ui_draw_field(force, ui_card_row[2], sizeof(ui_card_row[2]), title,
                  tx, y + MSG_TITLE_Y, &fonts::Font4, c_pri, UI_TL,
                  (uint16_t)text_w, tint);

    // The two lines the panel actually shows, and the two lines GET /ui
    // reports. ui_wrap2() borrows the one M5.Display to measure and may only be
    // called from this task; the endpoint reads the cache these fill instead of
    // wrapping anything itself.
    char b1[NOTIFY_BODY_LEN + 4], b2[NOTIFY_BODY_LEN + 4];
    ui_wrap2(v.body, b1, sizeof(b1), b2, sizeof(b2), &fonts::Font2, text_w);
    ui_draw_field(force, ui_card_body[0], sizeof(ui_card_body[0]), b1,
                  tx, y + MSG_BODY1_Y, &fonts::Font2, c_sec, UI_TL,
                  (uint16_t)text_w, tint);
    ui_draw_field(force, ui_card_body[1], sizeof(ui_card_body[1]), b2,
                  tx, y + MSG_BODY2_Y, &fonts::Font2, c_sec, UI_TL,
                  (uint16_t)text_w, tint);
    // AFTER both, never before. The stamp is the promise that the cache holds
    // this message's lines, and it must not be made until they are in it —
    // GET /ui runs on the AsyncTCP task and preempts this one, so a stamp
    // written first would be true for a reader arriving between the two.
    ui_card_body_id = v.id;

    if (fading) ui_card_fade++;
}

// ---- The clock face ----
//
// The whole of STATUS: HH:MM, the seconds beside it, the date under both, and
// the two rows ui_status_row() formats. Geometry and every width are up with
// the UI_CLOCK_* constants; nothing is measured here.
//
// THE BOTTOM TWO ROWS HAVE THREE MODES AND THE MODE PICKS THEIR FONT. This
// function asks ui_status_row_mode() ONCE and hands the answer to both the font
// it installs and the formatter that fills the rows, so the text and the face
// it is drawn in cannot come from two different reads of a live radio. What
// that costs and why it needs remembering is at the fillRect below.
//
// NOTHING ON THIS SCREEN CAN PRINT 1970. clock_local_time() returns false for
// anything at or below TIME_VALID_EPOCH, which is the whole pre-NTP epoch, and
// the placeholders below are what a caller sees until the first sync lands. The
// notice goes in the date row because Font7 draws letters as blank cells.
//
// The clock field is drawn with NO PADDING, which is correct and not an
// omission: "00:00" and "--:--" are both 140px in Font7, so the per-glyph
// opaque background of whichever one is being drawn covers every pixel the
// other one left. There is nothing outside the glyphs for a pad to erase.
static void ui_draw_clock(bool force) {
    char hhmm[8], ss[4], date[24], row[UI_CROW_BYTES];
    struct tm now;
    if (clock_local_time(now)) {
        strftime(hhmm, sizeof(hhmm), "%H:%M", &now);
        strftime(ss, sizeof(ss), "%S", &now);
        strftime(date, sizeof(date), UI_DATE_FMT, &now);
    } else {
        snprintf(hhmm, sizeof(hhmm), "%s", UI_CLOCK_UNSYNCED);
        snprintf(ss, sizeof(ss), "%s", UI_SEC_UNSYNCED);
        snprintf(date, sizeof(date), "%s", UI_NTP_NOTICE);
    }

    ui_draw_field(force, ui_clock_time, sizeof(ui_clock_time), hhmm,
                  UI_CLOCK_X, UI_CLOCK_Y, &fonts::Font7, COL_TEXT, UI_TL,
                  0, COL_BG);
    // The one field here that does need its pad: "00" is 28px and "--" only 16,
    // so the wide-to-narrow step leaves 12px of the previous value behind
    // without a side-fill to take them.
    ui_draw_field(force, ui_clock_sec, sizeof(ui_clock_sec), ss,
                  UI_SEC_X, UI_SEC_Y, &fonts::Font4, COL_DIM, UI_TL,
                  UI_SEC_W, COL_BG);
    ui_draw_field(force, ui_clock_date, sizeof(ui_clock_date), date,
                  M5.Display.width() / 2, UI_DATE_Y, &fonts::Font4, COL_DIM,
                  UI_TC, UI_DATE_W, COL_BG);

    // ONE FONT FOR BOTH ROWS, CHOSEN ONCE FROM THE MODE. Not one choice per
    // row: the two rows are one block and a mode that splits them across two
    // faces would put an 8px row above a 16px one with the gap between them
    // still sized for two 16px cells.
    //
    // The casts are not decoration: fonts::Font0 is a GLCDfont and fonts::Font2
    // a BMPfont (an RLEfont is Font4, Font6, Font7 and Font8 — not this one),
    // two unrelated concrete types, so the conditional has no common pointer
    // type without them. ui_draw_field() takes the base IFont anyway.
    ui_crow_mode_t mode = ui_status_row_mode();
    const lgfx::IFont *row_font =
        (mode == UI_CROW_AP) ? (const lgfx::IFont *)&fonts::Font0
                             : (const lgfx::IFont *)&fonts::Font2;

    // THE FONT CHANGES WITH THE MODE AND ui_draw_field() CANNOT SEE THAT.
    // It caches on the TEXT alone and its padding erases only the CURRENT
    // font's cell height — eight rows under Font0 where Font2 painted sixteen —
    // so the AP mode drawing over a Font2 row repaints the top half and leaves
    // the bottom half of the old glyphs on the glass. No clip, no error, no
    // difference in what GET /ui reports; the only witness is the panel, and
    // this project has no camera.
    //
    // ap_start() and ap_stop() both raise ui_force, so today the two edges into
    // and out of the AP mode happen to repaint in full — but that is an
    // incidental property of two call sites, and the connected/offline edge
    // raises nothing whatsoever. Remembering what was drawn is what makes the
    // screen correct wherever ui_force is or is not raised.
    //
    // The band is both rows at UI_F2_H, which covers the Font0 rows too, and it
    // is exactly the span the fields themselves can paint: UI_CROW_W is asserted
    // to be at least as wide as anything either font can put here.
    if (mode != ui_clock_rows_drawn) {
        M5.Display.fillRect(UI_LABEL_X, UI_CROW0_Y, UI_CROW_W, UI_CROW_BAND_H,
                            COL_BG);
        // Blanked, not left alone: the fillRect took the text off the glass and
        // a cache still holding it would compare equal and decline to draw it
        // back. No mode's rows are ever empty, so "" can never match one.
        ui_clock_row[0][0] = '\0';
        ui_clock_row[1][0] = '\0';
        ui_clock_rows_drawn = mode;
    }

    for (int r = 0; r < 2; r++) {
        // false: the panel is the password's only channel, so this is the one
        // caller that asks for it unredacted.
        ui_status_row(mode, r, row, sizeof(row), false);
        ui_draw_field(force, ui_clock_row[r], sizeof(ui_clock_row[r]), row,
                      UI_LABEL_X, r == 0 ? UI_CROW0_Y : UI_CROW1_Y,
                      row_font, COL_TEXT, UI_TL, UI_CROW_W, COL_BG);
    }
    // AFTER every field, never before, exactly as ui_card_body_id is stamped.
    // The flag is the promise that the caches hold a drawn screen, and GET /ui
    // runs on the AsyncTCP task and preempts this one — a flag raised first
    // would be true for a reader arriving mid-function.
    ui_clock_drawn = true;
}

// Leave the current screen for another one. The frame is repainted from
// scratch: rows carry different labels, the menu paints a coloured selection
// bar, and painting a new screen over the old one field by field would leave
// whichever rows the new screen does not use showing the old screen's text.
// Defined further down with the rest of the keyboard handling; declared here
// because every screen transition has to flush input and the transitions come
// first in this file.
static void ui_input_flush();

static void ui_goto(ui_screen_t screen) {
    if (screen == ui_screen) return;
    ui_screen = screen;
    ui_force = true;
    // Arrive with no input owing. Anything the controller buffered while the
    // action that brought us here was running was aimed at the previous screen,
    // and must not move a selection on this one.
    ui_input_flush();
}

static void ui_set_brightness(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    ui_brightness = (uint8_t)value;
    if (ui_brightness > 0) ui_brightness_on = ui_brightness;
    if (ui_ready) M5.Display.setBrightness(ui_brightness);
}

// Open one message. The card is given the id rather than the row, because the
// list moves under it; see ui_msg_id.
//
// ui_goto() is not enough on its own here and the extra raise is not
// belt-and-braces. Stepping from one card to the next leaves ui_screen already
// UI_MESSAGE, so ui_goto() returns early without forcing anything — and a fade
// that is not forced repaints nothing on its first frame. Raising the flag
// afterwards covers both cases. The one thing ui_goto() does that must NOT
// happen on a step within the stack is the input flush, and it correctly does
// not: there was no screen transition to flush for.
static void ui_enter_card(uint32_t id) {
    ui_msg_id = id;
    ui_card_fade = 0;
    ui_goto(UI_MESSAGE);
    ui_force = true;
}

// Leave the card for the list, putting the cursor on the message that was being
// read rather than at the top.
static void ui_leave_card(int idx) {
    ui_msg_sel = idx < 0 ? 0 : idx;
    ui_goto(UI_MESSAGES);
}

static void ui_activate(int index) {
    if (index < 0 || index >= UI_MENU_COUNT) return;
    const UiMenuItem &item = ui_menu[index];
    switch (item.action) {
    case UI_ACT_SCREEN:
        ui_goto((ui_screen_t)item.arg);
        break;
    case UI_ACT_AP:
        // RAISE ONLY. There is deliberately no user-reachable path to
        // ap_stop() anywhere in this firmware, and this entry must not become
        // one again. It used to be a toggle, which on a first boot with no
        // working credentials was a way to strand the node: the boot AP is then
        // the only way in and is deliberately never time-boxed (see the note on
        // AP_SESSION_MS), so one keystroke closed the only door, and re-raising
        // it from here marks the new session temporary — ten minutes later the
        // node has neither STA nor AP and only a power cycle recovers it.
        //
        // The AP goes down two ways, both automatic and both in ap_poll(): the
        // STA link coming up after having been down, which means provisioning
        // succeeded, and a time-boxed session expiring. Pressing this while the
        // AP is already up is therefore a no-op that just shows the credentials
        // again, which is what somebody who pressed it wanted anyway.
        //
        // Physical presence at the keyboard authorises provisioning; nothing
        // over the network can ask for the AP back.
        if (!ap_active) ap_start(true);
        if (ap_active) {
            // The password is now on the STATUS screen and nowhere else, so go
            // show it rather than leaving the user in the menu.
            ui_goto(UI_STATUS);
        } else {
            // ap_start() has two early returns that leave the AP down: the
            // subnet pin failing and softAP() itself refusing. Navigating
            // regardless used to land the user on a screen reading "ap: off"
            // with nothing to say the key had been seen at all. Stay in the
            // menu, where the entry's own state column now reads "failed".
            ap_start_failed = true;
            event_add("setup AP requested from keyboard, but it did not come up");
        }
        break;
    case UI_ACT_BACKLIGHT:
        ui_set_brightness(ui_brightness > 0 ? 0 : ui_brightness_on);
        break;
    }
    ui_force = true;
}

// One step of `;` or `.`, on whichever screen has something to step through.
//
// All three wrap rather than clamp, which is the behaviour the menu already
// had. On the card that wrap is through the message stack itself: the card
// holds an id, so a step has to resolve that id to its current position first,
// move from there, and adopt the id at the new position.
//
// THE CARD BRANCH TAKES THREE SEPARATE ACQUISITIONS — the depth, the position
// and the entry at the new one — and a message arriving on the web server task
// between any two of them makes them disagree. That is tolerated here and
// nowhere else in this file, because the worst outcome is a step that lands on
// a neighbour of the intended message or does not move at all: notify_view()
// fails and the branch simply breaks, leaving the card where it was for the
// user to press the key again. What is NOT tolerated is drawing from a split
// read, and nothing here draws — ui_draw_card() gets entry, position and depth
// out of one fused acquisition of its own, so whatever id this function
// settles on is rendered self-consistently or not at all. If the message has
// gone by then, ui_tick() leaves for the list on the next pass.
static void ui_move(int step) {
    switch (ui_screen) {
    case UI_MENU: {
        ui_menu_index = (ui_menu_index + UI_MENU_COUNT + step) % UI_MENU_COUNT;
        break;
    }
    case UI_MESSAGES: {
        int count = notify_count() + 1;  // the messages, then Back
        ui_msg_sel = (ui_msg_sel + count + step) % count;
        break;
    }
    case UI_MESSAGE: {
        int count = notify_count();
        int idx = notify_index_of(ui_msg_id);
        if (count <= 0 || idx < 0) break;
        int n = (idx + count + step) % count;
        NotifyView v;
        // Re-fade on the way in, so moving through the stack reads as one card
        // replacing another rather than as text changing inside a frame that
        // never moved.
        if (notify_view(n, v)) {
            ui_msg_sel = n;
            ui_enter_card(v.id);
        }
        break;
    }
    default:
        break;
    }
}

// Navigation is on the BARE `;` `.` `,` `/` keys and Enter, with no Fn chord
// anywhere.
//
// Those four keys are the inverted-T printed on the keycaps, and the device is
// held in two hands with both thumbs on the keyboard: an Fn chord to move one
// row down is a two-handed operation on a thing designed to be used with two
// thumbs. It also means this seed needs nothing from the library's Fn layer.
//
//   ;  /  .     up / down      move the selection in the menu
//   ,  /  /     left / right   dim / brighten the panel, on every screen
//   Enter                      open the menu, or activate the selected entry
//   `                          back: a screen returns to the menu, the menu to
//                              status. Backtick and not `q`, because it is the
//                              top-left key where Escape lives on a full
//                              keyboard, and because every letter has to stay
//                              free for a screen that takes text later.
static void ui_handle_key(char key) {
    // Only printable ASCII and Enter are recorded or acted on. GET /ui puts
    // this byte straight into a JSON string, and a mis-decoded FIFO read — a
    // dropped I2C bit turns one row/column pair into another, and the key map
    // has entries above 0x7F for the Fn layer — would otherwise emit a lone
    // high byte, which is not valid UTF-8 and makes the whole document
    // unparseable for every client, not just that field. Nothing in the
    // navigation set below is outside this range.
    if (key != '\n' && (key < 0x20 || key > 0x7E)) return;

    ui_last_key = key;
    ui_last_key_ms = millis();

    switch (key) {
    case ';':
        ui_move(-1);
        break;
    case '.':
        ui_move(1);
        break;
    case ',':
        ui_set_brightness((int)ui_brightness - UI_BRIGHT_STEP);
        break;
    case '/':
        ui_set_brightness((int)ui_brightness + UI_BRIGHT_STEP);
        break;
    case '\n':
        if (ui_screen == UI_MENU) {
            ui_activate(ui_menu_index);
        } else if (ui_screen == UI_MESSAGES) {
            int items = notify_count();
            if (ui_msg_sel < items) {
                NotifyView v;
                if (notify_view(ui_msg_sel, v)) ui_enter_card(v.id);
            } else {
                // The Back row. Put the menu cursor on the entry this screen
                // hangs off rather than wherever it happened to be left — a
                // card raised by an arrival can reach this list without the
                // menu ever having been open.
                ui_menu_index = 0;  // Messages
                ui_goto(UI_MENU);
            }
        } else if (ui_screen == UI_MESSAGE) {
            // ENTER IS THE ACKNOWLEDGEMENT AND BACKTICK IS NOT, which is the
            // whole reason this screen reads two keys instead of one. Having
            // looked at a message is not the same as having dealt with it, and
            // an unread critical outlives its ttl until it is acknowledged — so
            // a user who wants to look at one now and act on it later needs a
            // way out that leaves it unread. The footer says which is which.
            int idx = notify_index_of(ui_msg_id);
            notify_ack_id(ui_msg_id);
            ui_leave_card(idx);
        } else {
            ui_goto(UI_MENU);
        }
        break;
    case '`':
        if (ui_screen == UI_MESSAGE) {
            ui_leave_card(notify_index_of(ui_msg_id));
        } else if (ui_screen == UI_MESSAGES) {
            ui_goto(UI_MENU);
        } else {
            ui_goto(ui_screen == UI_MENU ? UI_STATUS : UI_MENU);
        }
        break;
    default:
        break;
    }
}

// ---- Keyboard edge detection ----
//
// THE LIBRARY'S "SOMETHING CHANGED" FLAG IS NOT USABLE AND IS NOT USED HERE.
// Keyboard_Class::isChange() compares the COUNT of currently-pressed keys
// against the count it saw last time and nothing else. Two consequences, both
// reachable by ordinary typing:
//
//   1. Press A, then press B while A is still down, then release B. The count
//      goes 1 -> 2 -> 1, so the release is reported as a change; isPressed() is
//      still true because A is down; and the last character in the state buffer
//      is now A. A fires a second time, on a key-up, having never been touched.
//   2. Any release event the controller drops leaves the count permanently one
//      too high. From then on every press-release pair looks like the case
//      above and every keystroke double-fires, for the life of the boot.
//
// So this seed keeps its own record of which key codes are currently down and
// derives the edges itself. A key acts exactly once, when it first appears in
// the down set, and cannot act again until it has genuinely been released. That
// also makes a dropped release cost one stuck key rather than corrupting every
// keystroke that follows.
#define UI_KEYS_MAX 8
static char ui_keys_down[UI_KEYS_MAX];
static uint8_t ui_keys_down_count = 0;

static bool ui_key_was_down(char c) {
    for (uint8_t i = 0; i < ui_keys_down_count; i++) {
        if (ui_keys_down[i] == c) return true;
    }
    return false;
}

// Collect the key codes the library currently reports as held, in the same
// character space ui_handle_key() switches on. Enter is a flag rather than a
// member of the word buffer, so it is folded in as the newline it is elsewhere.
static uint8_t ui_keys_snapshot(char *out) {
    Keyboard_Class::KeysState &state = M5Cardputer.Keyboard.keysState();
    uint8_t n = 0;
    if (state.enter && n < UI_KEYS_MAX) out[n++] = '\n';
    for (auto c : state.word) {
        if (n >= UI_KEYS_MAX) break;
        bool dup = false;
        for (uint8_t i = 0; i < n; i++) {
            if (out[i] == c) dup = true;
        }
        if (!dup) out[n++] = c;
    }
    return n;
}

// Discard whatever the keyboard has queued and re-baseline the down set to what
// is held right now, so that nothing typed before this moment can act after it.
//
// This exists because ui_activate() can block for a long time — raising the
// softAP is hundreds of milliseconds — and the controller buffers key events in
// a hardware FIFO throughout. Without this, keys pressed while the node was busy
// arrive afterwards and drive whichever screen the action navigated to. The
// drain is bounded: the TCA8418's FIFO holds ten events, update() retires at
// most one per call, and a call with nothing pending is a flag test.
static void ui_input_flush() {
    if (!ui_keyboard_enabled) return;
    for (int i = 0; i < 12; i++) M5Cardputer.update();
    ui_keys_down_count = ui_keys_snapshot(ui_keys_down);
}

// Read the keyboard. Cheap to call every loop: the reader only touches I2C when
// its ISR flag is set, so an idle keyboard costs a flag test.
//
// On the interrupt, which an earlier revision of this file got wrong twice:
// the reader DOES listen. TCA8418KeyboardReader's constructor takes the pin as
// a defaulted argument, but the default is not the -1 the header shows — the
// constructor body normalises any negative value to DEFAULT_TCA8418_INT_PIN,
// which the library defines as 11 and which is exactly PIN_KB_INT on this
// board. So begin() does run pinMode()/attachInterruptArg() on GPIO11, the ISR
// does set the flag, and installing the reader by hand to pass the same pin
// would change nothing. Reading an externally driven line is not contention;
// nothing here drives it.
static void ui_key_poll() {
    // No reader was installed at all if M5 did not identify the board — see
    // ui_begin() for why that is on purpose.
    if (!ui_keyboard_enabled) return;
    M5Cardputer.update();

    char now[UI_KEYS_MAX];
    uint8_t n = ui_keys_snapshot(now);

    // Down edges only: a code present now that was absent last pass. Key-up
    // edges fall out of the same diff and are simply not acted on yet.
    //
    // Collected before anything is dispatched, and the new baseline committed
    // before that too, because a handler can call ui_input_flush() by way of
    // ui_goto() — and a flush that re-baselines the down set must be the last
    // word on it. Writing the baseline after the dispatch loop instead would
    // silently undo the flush with this pass's now-stale snapshot.
    char fired[UI_KEYS_MAX];
    uint8_t f = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!ui_key_was_down(now[i])) fired[f++] = now[i];
    }

    ui_keys_down_count = n;
    for (uint8_t i = 0; i < n; i++) ui_keys_down[i] = now[i];

    for (uint8_t i = 0; i < f; i++) ui_handle_key(fired[i]);
}

// One pass of screen work. Everything below is change-detected, so a tick where
// nothing moved issues no SPI at all.
static void ui_tick() {
    if (!ui_ready) return;

    // THE TICK IS NO LONGER A SINGLE FIXED GATE. The arrival fade is three
    // frames that have to land 40ms apart to read as motion, and a 200ms gate
    // would stretch them over 600ms and turn the animation into three separate
    // redraws. The card asks for the faster interval only while it is actually
    // fading and drops back to the ordinary tick the moment it has settled, so
    // nothing else on this device pays for it.
    static unsigned long last_tick = 0;
    unsigned long interval =
        (ui_screen == UI_MESSAGE && ui_card_fade < MSG_FADE_STEPS)
            ? MSG_FADE_MS : UI_TICK_MS;
    // ui_force AND NOT ui_force_net. The whole difference between the two
    // flags is this line: a keypress skips the gate because the user is
    // standing there waiting, and a POST does not because the poster sets the
    // rate. See the declaration of ui_force_net.
    if (!ui_force && millis() - last_tick < interval) return;
    last_tick = millis();

    // Both of these change the screen, so both run BEFORE the force flag is
    // taken below — a ui_goto() after the take raises a flag this pass has
    // already cleared, and the repaint it asks for would not happen until the
    // next tick.

    // An arrival, handed over by the endpoint on the AsyncTCP task and consumed
    // here because loop() owns every screen change.
    //
    // ONLY FROM THE STATUS SCREEN. A card that shoves itself in front of
    // whatever was being read is a screen that cannot be trusted to stay still
    // while it is being used, and the queue is not so urgent that it is worth
    // that: an unacknowledged message keeps, and the Messages row in the menu
    // carries its count. Status is the idle screen, so raising a card from it
    // interrupts nobody. The flag is consumed either way, so a message that
    // arrives while somebody is navigating cannot surface as a card several
    // screens later when they happen to return to status.
    uint32_t arrived = 0;
    if (notify_take_arrival(&arrived) && arrived != 0 &&
        ui_screen == UI_STATUS) {
        ui_msg_sel = 0;
        ui_msg_first = 0;
        ui_enter_card(arrived);
    }

    // The card's subject, expired or evicted out from under it. Checked here
    // rather than left to ui_draw_card()'s own false return, because that
    // return draws nothing at all and would leave the last card frozen on the
    // glass for as long as the screen stayed up. This is a bare existence test
    // and deliberately not the beginning of an index-then-read: everything the
    // card actually draws still comes out of the single fused acquisition in
    // ui_draw_card(), so no two fields can ever disagree about which message
    // they belong to.
    if (ui_screen == UI_MESSAGE && notify_index_of(ui_msg_id) < 0) {
        ui_leave_card(0);
    }

    // Take the flag and clear it BEFORE any drawing, then work from the copy.
    //
    // This is deliberately not what the source firmware does. That one clears
    // its flag at the END of a pass, in eight separate places, and an
    // end-of-pass clear is a one-way door: a writer that raises the flag from
    // another task while the pass is in flight has its request overwritten by
    // the clear that follows, and the repaint it asked for never happens. Not a
    // hypothetical there either — its flag is already written from its web
    // server task. Clearing first inverts which way that race falls. A flag
    // raised after the take survives into the next tick and costs one redundant
    // repaint, instead of being swallowed and costing a screen that does not
    // show what the device knows. The window is not closed, only cut down to
    // the two instructions between the take and the clear — a raise landing in
    // there is still lost, where before the whole SPI-bound pass was exposed.
    // This is an improvement over the firmware this was ported from, not parity
    // with it.
    //
    // Everything below therefore reads `force` and never ui_force.
    //
    // The network flag is folded in HERE and nowhere else. Having survived the
    // gate above it is worth exactly as much as a local raise from this point
    // on — a repaint is a repaint — so the two merge into one local and the
    // rest of this function never learns which side asked.
    bool force = ui_force || ui_force_net;
    ui_force = false;
    ui_force_net = false;

    if (force) {
        ui_draw_frame(ui_screen);
        ui_cache_sel = -1;
    }

    // Header.
    ui_draw_field(force, ui_cache_hdr, sizeof(ui_cache_hdr),
                  ui_screen_title[ui_screen],
                  UI_LABEL_X, UI_HDR_Y, &fonts::Font2, COL_ACCENT,
                  UI_TL, UI_HDR_TITLE_W, COL_BG);
    char buf[48];
    ui_net_summary(buf, sizeof(buf));
    ui_draw_field(force, ui_cache_net, sizeof(ui_cache_net), buf,
                  M5.Display.width() - UI_LABEL_X, UI_HDR_Y, &fonts::Font2,
                  COL_DIM, UI_TR, UI_HDR_NET_W, COL_BG);

    // The unread badge, in the gap the two header fields leave between them.
    // The header is on every screen, so this is the one place that says a
    // message arrived without having to navigate to Messages for it.
    //
    // Not a ui_draw_field, and it cannot be one: that caches on the text, and
    // the dot has no text. A single explicit test drives both halves, which is
    // correct because they never disagree — the dot is lit exactly when the
    // count is non-empty.
    //
    // The quantise is what makes this cheap. Everything above nine draws the
    // same "9+", so collapsing it to one value before the comparison means a
    // queue churning between ten and twenty unread costs no SPI at all.
    //
    // notify_unread_count() takes the store's spinlock, which disables
    // interrupts on this core for the length of the walk, so the dot and the
    // count are driven from ONE call and not one each. That is this call site
    // only — a MENU tick still reaches the store again through ui_menu_state().
    int unread = notify_unread_count();
    int badge = unread > 9 ? 10 : unread;
    // `force` and not ui_force: a forced pass has already run fillScreen, so
    // the dot is gone from the glass whatever ui_badge_drawn still says.
    if (force || badge != ui_badge_drawn) {
        ui_badge_drawn = badge;
        // Cleared by painting the same circle in the background colour, so the
        // dot gives back exactly the pixels it took and no more. The +8 is half
        // of Font2's 16px cell, which puts the dot on the centre line of the
        // two strings either side of it.
        M5.Display.fillCircle(UI_BADGE_CX, UI_HDR_Y + 8, UI_BADGE_R,
                              badge ? COL_ACCENT : COL_BG);
        if (badge == 0)      buf[0] = '\0';
        else if (badge > 9)  snprintf(buf, sizeof(buf), "9+");
        else                 snprintf(buf, sizeof(buf), "%d", badge);
        // Drawn straight rather than through the field cache: the test above
        // has already decided, and a second gate keyed on the text alone would
        // be a second answer to a question the dot's half cannot ask.
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextDatum(UI_TL);
        M5.Display.setTextColor(COL_ACCENT, COL_BG);
        M5.Display.setTextPadding(UI_BADGE_W);
        M5.Display.drawString(buf, UI_BADGE_X, UI_HDR_Y);
        M5.Display.setTextPadding(0);
    }

    if (ui_screen == UI_MENU) {
        ui_menu_first = ui_window(ui_menu_index, ui_menu_first, UI_MENU_COUNT,
                                  UI_ROWS);
        // A row, not an entry. Since the menu started scrolling these are two
        // different numbers, and every test below is about which ROW carries
        // the bar.
        int sel_row = ui_menu_index - ui_menu_first;
        // A scrolled window changes what every row says while the bar can stay
        // exactly where it is, so the two-row optimisation below has to be
        // switched off wholesale when it moves. Without this the entries slide
        // under a stationary bar and the rows that are not repainted keep the
        // previous window's text.
        bool scrolled = force || ui_menu_first != ui_menu_first_drawn;
        for (int row = 0; row < UI_ROWS; row++) {
            int32_t y = ui_row_y(row);
            int item = ui_menu_first + row;
            bool selected = (row == sel_row);
            // The selection bar is not a field, so it is painted from an
            // explicit test: only the row that gained the highlight and the one
            // that lost it are touched.
            bool moved = scrolled || (ui_cache_sel != sel_row &&
                                      (selected || row == ui_cache_sel));
            uint16_t bg = selected ? COL_ACCENT : COL_BG;
            uint16_t fg = selected ? COL_BG : COL_TEXT;
            if (moved) {
                M5.Display.fillRect(0, y - 1, M5.Display.width(), UI_ROW_H, bg);
                ui_cache_label[row][0] = '\0';
                ui_cache_value[row][0] = '\0';
            }
            const char *title = item < UI_MENU_COUNT ? ui_menu[item].title : "";
            ui_draw_field(force, ui_cache_label[row],
                          sizeof(ui_cache_label[row]), title,
                          UI_MENU_X, y, &fonts::Font2, fg, UI_TL,
                          140, bg);
            ui_menu_state(item, buf, sizeof(buf));
            ui_draw_field(force, ui_cache_value[row],
                          sizeof(ui_cache_value[row]), buf,
                          UI_MENU_R_X, y, &fonts::Font2,
                          selected ? COL_BG : COL_DIM, UI_TR, 80, bg);
        }
        ui_cache_sel = sel_row;
        ui_menu_first_drawn = ui_menu_first;
    } else if (ui_screen == UI_MESSAGES) {
        ui_draw_msglist(force);
    } else if (ui_screen == UI_MESSAGE) {
        ui_draw_card(force);
    } else if (ui_screen == UI_STATUS) {
        ui_draw_clock(force);
    } else {
        char label[16], value[48];
        for (int row = 0; row < UI_ROWS; row++) {
            int32_t y = ui_row_y(row);
            ui_field(ui_screen, row, label, sizeof(label), value, sizeof(value));
            ui_draw_field(force, ui_cache_label[row],
                          sizeof(ui_cache_label[row]), label,
                          UI_LABEL_X, y, &fonts::Font2, COL_DIM,
                          UI_TL, UI_LABEL_W, COL_BG);
            ui_draw_field(force, ui_cache_value[row],
                          sizeof(ui_cache_value[row]), value,
                          UI_VALUE_X, y, &fonts::Font2, COL_TEXT,
                          UI_TL, UI_VALUE_W, COL_BG);
        }
    }

    // An empty legend is drawn as nothing at all rather than as an empty field,
    // because the field's padding is the erase and the clock face has its lower
    // row in that band. Leaving ui_cache_foot holding the previous screen's
    // legend is safe: every screen change goes through ui_goto(), which raises
    // ui_force, and a forced pass fillScreen()s the panel and repaints this
    // field with force=true — so a stale cache can never survive a transition
    // into a screen that does draw a legend.
    ui_footer(ui_screen, buf, sizeof(buf));
    if (buf[0] != '\0') {
        ui_draw_field(force, ui_cache_foot, sizeof(ui_cache_foot), buf,
                      UI_LABEL_X, UI_FOOT_Y, &fonts::Font0, COL_DIM, UI_TL,
                      236, COL_BG);
    }
}

// Bring up M5Unified, the panel and the keyboard. Must run before hw_probe(),
// which scans the I2C bus this call is what opens.
static void ui_begin() {
    m5::M5Unified::config_t cfg = M5.config();

    // Autodetection failing has to fail into the RIGHT board, and on this chip
    // the default is not merely unhelpful, it is a different device: M5Unified
    // falls back to board_M5AtomS3Lite on every ESP32-S3, silently, with a
    // pinmap that shares nothing with this one. Two things ride on getting it
    // right.
    //
    // The keyboard: Keyboard_Class::begin() picks its reader from
    // M5.getBoard() at runtime. board_M5CardputerADV gets the TCA8418 reader
    // that talks I2C; board_M5Cardputer — the v1.1 — gets a reader that strobes
    // GPIO 8, 9 and 11 as a scan matrix, which on this board is the I2C bus and
    // the keyboard controller's own interrupt line. Anything else gets a
    // do-nothing reader and a printf.
    //
    // The screen: begin() reads the current brightness, sets it to 0 so the
    // panel does not flash garbage while it initialises, and restores it only
    // if the display's own autodetect came back with a board. GPIO38 gates the
    // backlight AND the RGB LED supply, so a detection failure is a dark screen
    // and a dark LED at once, with nothing to show that anything went wrong.
    //
    // This does not make detection succeed — it decides what happens when it
    // does not. It also destroys the obvious way of asking which happened, and
    // an earlier revision of this file walked straight into that: M5Unified
    // substitutes the fallback for an unknown autodetect result BEFORE it
    // assigns _board, so M5.getBoard() answers board_M5CardputerADV in both
    // cases and a flag built on it can never report the failure it is named
    // after. The reading that survives is M5.Display.getBoard(), which is the
    // raw autodetect answer M5Unified itself is substituting for, and which
    // stays board_unknown when the panel never replied. That is what
    // ui_board_detected is taken from below.
    cfg.fallback_board = m5::board_t::board_M5CardputerADV;

    // Left at its default of 0 deliberately, which is the value that makes
    // M5Unified not call Serial.begin() at all. setup() has already opened the
    // console at 115200 and prints the node's token through it; letting M5
    // reopen the port would cut the banner off mid-boot.
    cfg.serial_baudrate = 0;

    // internal_imu / internal_mic / internal_spk are left on. M5Unified only
    // CONFIGURES those at begin() — it stores pin numbers and drives nothing —
    // and none of them is started anywhere in this firmware, so the I2S pins in
    // the map above stay idle.

    // Split deliberately into two calls rather than the one M5Cardputer.begin()
    // that does both, so that what the board turned out to be is known BEFORE
    // anything installs a keyboard reader.
    //
    // Keyboard_Class::begin() is not defensive: board_M5Cardputer — the v1.1 —
    // gets a reader that immediately strobes GPIO 8, 9 and 11 as a scan matrix,
    // and on this board those are the live I2C bus and the keyboard
    // controller's interrupt. Asking for the reader only once the board has
    // answered means a misidentification costs a keyboard that does not work,
    // which is visible and harmless, instead of a bus that is being driven by
    // two things at once.
    //
    // The second call is safe because M5Unified::begin() refuses to run twice —
    // it returns immediately once _board is set — so it does nothing here but
    // pass the keyboard flag through.
    M5.begin(cfg);

    // Two different questions, and they must not share an answer.
    //
    // Did autodetection actually work? Only the display can say — see the
    // fallback note above. This is the flag that is reported, and the one
    // allowed to say no.
    ui_board_detected =
        (M5.Display.getBoard() == m5::board_t::board_M5CardputerADV);
    // Which reader will Keyboard_Class::begin() install? It asks M5.getBoard(),
    // so the answer is the TCA8418 reader whenever the fallback applies, which
    // is always on this build. Gating the keyboard on ui_board_detected instead
    // would mean a failed panel probe also cost the keys, for no reason: the
    // keyboard is on I2C and has nothing to do with the panel.
    ui_keyboard_enabled = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
    M5Cardputer.begin(cfg, ui_keyboard_enabled);

    // width() reads a member and is safe even when autodetection left no panel
    // behind, which is exactly the case that must not reach setRotation().
    ui_ready = (M5.Display.width() > 0 && M5.Display.height() > 0);
    // Both readings by name, not one verdict. They differ exactly when
    // autodetection failed and the fallback covered for it, which is the case
    // worth being able to see from a console.
    Serial.printf("[ui] m5 display autodetect %d (%s), effective board %d, "
                  "keyboard %s, panel %s\n",
                  (int)M5.Display.getBoard(),
                  ui_board_detected ? "CardputerADV" : "NOT DETECTED, using fallback",
                  (int)M5.getBoard(),
                  ui_keyboard_enabled ? "reader installed" : "no reader",
                  ui_ready ? "up" : "NOT initialised");
    if (!ui_ready) return;

    M5.Display.setRotation(1);  // 240x135 landscape, keyboard toward the user
    M5.Display.setBrightness(ui_brightness);
    // The splash draws itself with a literal `true` rather than by leaning on
    // the flag: these two fields have to land on the frame that was just
    // blanked above them, and ui_draw_field() no longer reads the global. The
    // flag is still raised, and still for its own reason — the first ui_tick()
    // after setup() has to repaint the real screen over this splash rather than
    // diff against the two strings it left in the caches.
    ui_force = true;
    ui_draw_frame(ui_screen);
    ui_draw_field(true, ui_cache_hdr, sizeof(ui_cache_hdr), "SEED", UI_LABEL_X,
                  UI_HDR_Y, &fonts::Font2, COL_ACCENT, UI_TL,
                  UI_HDR_TITLE_W, COL_BG);
    ui_draw_field(true, ui_cache_value[0], sizeof(ui_cache_value[0]),
                  "starting...",
                  UI_VALUE_X, ui_row_y(0), &fonts::Font2, COL_TEXT,
                  UI_TL, UI_VALUE_W, COL_BG);
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
             "ST7789V2 240x135 IPS, driven via M5GFX (RST=%d,DC=%d,MOSI=%d,"
             "SCLK=%d,CS=%d,BL=%d PWM). %s; see GET /ui for what it is showing",
             PIN_TFT_RST, PIN_TFT_DC, PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_CS,
             PIN_TFT_BL,
             ui_ready ? "panel up" : "panel NOT initialised, autodetect failed");
    doc["display"] = peri;
    snprintf(peri, sizeof(peri),
             "TCA8418 matrix controller on I2C 0x%02X (INT=%d), driven via "
             "M5Cardputer; %s",
             TCA8418_ADDR, PIN_KB_INT,
             // ui_keyboard_enabled, not ui_board_detected: the reader is what
             // actually gates key reading, and it is installed whenever the
             // library's fallback applies, INCLUDING when panel autodetect
             // failed. Reporting the panel-detect flag here would tell an agent
             // the keys are dead on a board whose keys work.
             ui_keyboard_enabled
                 ? "reader installed, keys are being read over I2C"
                 : "reader NOT installed, keys are not being read");
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
    char busdesc[96];
    snprintf(busdesc, sizeof(busdesc),
             "i2c0 SDA=%d SCL=%d 400kHz (M5Unified In_I2C on I2C_NUM_1; Wire is "
             "not used by this firmware)",
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
        "/", "/health", "/capabilities", "/config.md", "/events", "/ui",
        "/clock", "/clock/tz",
        "/firmware/version", "/firmware/upload", "/firmware/apply",
        "/firmware/confirm", "/firmware/rollback",
        "/skill", "/wifi/config", NULL
    };
    for (int i = 0; eps[i]; i++) ep.add(eps[i]);

    // Skill endpoints, each path at most once.
    //
    // A skill's endpoint table has one row per METHOD, because that is what
    // /skill renders as its API table and GET /notify and POST /notify are two
    // different things to document. This array is a list of PATHS — the spec
    // above says "all HTTP paths the node handles" — and a path repeated here
    // is not extra detail, it is a false statement about the surface: a client
    // counting entries to size a probe list finds one endpoint that does not
    // exist. So the dedup belongs here, at the point where rows become paths,
    // and NOT in the skill tables, which are right as they stand.
    //
    // Linear scan over a list this length is cheaper than anything cleverer,
    // and /capabilities is not a hot path.
    for (int i = 0; i < g_skill_count; i++) {
        const SkillEndpoint *se = g_skills[i]->endpoints;
        for (int j = 0; se[j].path; j++) {
            bool seen = false;
            for (JsonVariant v : ep) {
                if (strcmp(v.as<const char*>(), se[j].path) == 0) { seen = true; break; }
            }
            if (!seen) ep.add(se[j].path);
        }
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// Collects a request body into a single NUL-terminated heap buffer for the
// handlers below that parse one (POST /config.md, /clock/tz, and the gpio and
// serial skills' JSON POSTs). Runs on the AsyncTCP task, once per body chunk.
//
// total is the declared body length. It is authoritative here and MUST be, for
// two reasons that the firmware/upload body callback learned the hard way:
//
//   - total == 0 means the client sent Transfer-Encoding: chunked with no
//     Content-Length. The library never fills in a running total for a chunked
//     body — _contentLength stays 0 — and it delivers chunks through this
//     callback with a *growing* index and total fixed at 0. There is no length
//     to size an allocation from, and the non-chunked path's clamp
//     (len = min(len, _contentLength - _parsedLength)) does not run for a
//     chunked body, so index and len are attacker-controlled and unbounded. A
//     naive malloc(total+1) then returns a 1-byte block that every later chunk
//     memcpys past. Unknown length cannot be served safely, so it is refused
//     outright with 411. Do NOT "relax" this to treat 0 as "allocate on
//     demand": the endpoints here have no streaming parser and the growing
//     index is exactly the primitive that overflows the heap. This is the same
//     total == 0 guard handle_firmware_upload_body() already carries.
//
//   - even with a known total, every chunk is bounds-checked against it before
//     the memcpy, and the buffer is NUL-terminated on every chunk rather than
//     only when index + len == total. A body that never delivers its final
//     byte (a truncated chunked stream, or bytes past the declared length)
//     would otherwise leave the buffer unterminated, and the handlers all read
//     it as a C string.
static void handle_body_collect(AsyncWebServerRequest *request, uint8_t *data,
                                 size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Unknown length (chunked): cannot size the allocation, refuse. See above.
        if (total == 0) { request->send(411, "application/json", "{\"error\":\"length required\"}"); return; }
        if (total > 4096) { request->send(413, "application/json", "{\"error\":\"too large\"}"); return; }
        // calloc so a body that never completes still leaves a defined,
        // NUL-filled buffer rather than uninitialised heap.
        char *buf = (char*)calloc(total + 1, 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (!buf) return;
    // Drop anything that would land outside the allocation. index + len can
    // overflow past total on a chunked stream the library did not clamp, and a
    // single byte past the end is the whole bug.
    if (index > total || len > total - index) return;
    memcpy(buf + index, data, len);
    // Terminate on every chunk: the final chunk is not guaranteed to arrive.
    buf[index + len] = '\0';
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

// --- GET /ui ---
//
// What the panel is showing, for a caller who cannot see it. This node has no
// camera and no second channel to the screen, so without this endpoint the
// display is unverifiable from anywhere but in front of it.
//
// What it proves: which screen is active, which fields that screen is composed
// of and what each one currently reads, whether the panel came up at all,
// whether M5 identified the board (and therefore whether the keyboard is being
// read over I2C rather than not at all), the backlight level, and the last key
// the firmware saw. That is enough to confirm that keys arrive, that navigation
// moves between screens, and that the values are the right values.
//
// What it does NOT prove: anything about pixels. Layout, legibility, whether a
// value overflows its column, whether the selection bar lands on the right row
// — none of that is observable here, and this endpoint agreeing with
// expectations is not a substitute for somebody looking at the glass.
//
// This handler runs on the AsyncTCP task and therefore draws NOTHING, and
// measures nothing either; see the note at the top of the UI section. It reads
// word-sized scalars and calls the screens' formatters into its own buffers,
// which is why there is no lock and nothing to tear. It passes redact=true to
// ui_status_row(), so the provisioning AP's password — which lives only in RAM
// and on the panel — does not travel the network.
//
// The clock face's other AP row is the auth token, and that one goes out AS IT
// IS. require_auth() above is why: this handler answers nothing without the
// token, so a caller reading it back off a reported row is being shown what
// they already presented. Redacting it would hide the row's shape from the only
// window anyone has onto this screen and protect nothing.
static void handle_ui(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    doc["screen"] = ui_screen_name[ui_screen];
    doc["panel_ready"] = ui_ready;
    // The panel's own autodetect answer, which is the only one that can come
    // back negative — M5.getBoard() is covered by cfg.fallback_board. Reported
    // alongside the independent I2C probe's verdict in board_probed, so that a
    // caller can see the two agree rather than take one on trust.
    doc["board_detected"] = ui_board_detected;
    doc["board_probed"] = hw.board;
    doc["keyboard"] = ui_keyboard_enabled;
    doc["width"] = (int)M5.Display.width();
    doc["height"] = (int)M5.Display.height();
    doc["brightness"] = ui_brightness;
    doc["backlight"] = ui_brightness > 0;

    // WHICH KEY BELOW CARRIES THIS SCREEN'S CONTENT. Three screens fill
    // fields[]; status, the menu, the message list and the card have no
    // label/value rows at all and report through "clock", "menu", "messages"
    // and "card" instead. An empty fields[] is a true statement about those
    // four, but only if something says where to look instead — otherwise a
    // caller reasonably reads it as a screen that failed to describe itself.
    switch (ui_screen) {
    case UI_STATUS:   doc["content"] = "clock";    break;
    case UI_MENU:     doc["content"] = "menu";     break;
    case UI_MESSAGES: doc["content"] = "messages"; break;
    case UI_MESSAGE:  doc["content"] = "card";     break;
    default:          doc["content"] = "fields";   break;
    }

    JsonArray fields = doc["fields"].to<JsonArray>();
    char label[16], value[48];
    for (int row = 0; row < UI_ROWS; row++) {
        if (!ui_field(ui_screen, row, label, sizeof(label), value,
                      sizeof(value))) {
            break;
        }
        JsonObject f = fields.add<JsonObject>();
        f["label"] = label;
        f["value"] = value;
    }

    // The clock face, only while it is up, for the same reason the card object
    // is: the caches below are what one screen last drew, and reporting them
    // from another screen would describe a panel nobody is looking at.
    //
    // THE TOP THREE COME FROM THE DRAW CACHES, exactly as the card's body lines
    // do. Re-deriving them here would call localtime_r() and strftime() from a
    // second task and land on a different second than the panel is showing —
    // "what is on the glass" is the whole contract of this endpoint, and a
    // clock is the one field where a plausible-looking second answer is
    // indistinguishable from the right one.
    //
    // ONLY ONCE A DRAW HAS FILLED THEM. See ui_clock_drawn: the caches are
    // empty until the first tick paints this screen, and never filled at all on
    // a node whose panel did not come up. Omitted rather than emptied, on the
    // card's principle — an absent key reads as "not known yet" where an empty
    // string reads as a blank clock — and because `synced` computed from an
    // unwritten cache does not merely read wrong, it reads TRUE: "" is not the
    // pre-sync placeholder, so an ungated report would claim a synced clock on
    // a node with a dead panel and an unsynced clock. GET /clock is the
    // endpoint that answers the time itself; this one answers the screen.
    //
    // THE TWO ROWS DO NOT COME FROM THE CACHE, AND CANNOT. It holds the AP
    // password verbatim, because the panel is that password's only channel;
    // emitting it would put the password on the network. They go back through
    // the formatter with redact=true instead — the same route the five-row
    // screen used, and the same one the panel takes with redact=false.
    //
    // That is a deliberate exception to the cache policy above and it has a
    // price: these two are recomputed here, so on a minute boundary a
    // time-boxed AP's countdown can come back one minute ahead of the glass,
    // and they are reported even before the first draw, when the glass has
    // nothing on it. Both are the cost of the password never leaving RAM, and
    // both are visible to a caller through the absence of the fields above.
    if (ui_screen == UI_STATUS) {
        JsonObject clock = doc["clock"].to<JsonObject>();
        if (ui_clock_drawn) {
            clock["time"] = ui_clock_time;
            clock["seconds"] = ui_clock_sec;
            clock["date"] = ui_clock_date;
            // Derived from what was drawn rather than from a second call into
            // the C library, so it cannot disagree with the three strings
            // beside it.
            clock["synced"] = strcmp(ui_clock_time, UI_CLOCK_UNSYNCED) != 0;
        }
        JsonArray rows = clock["rows"].to<JsonArray>();
        // Sized from the draw cache and not from `value` above, so that a row
        // long enough to be cut is cut at the same place here as it is on the
        // glass. That is no longer a hypothetical margin: the AP mode's lower
        // row is "token " and thirty-two hex characters, 38 of the buffer's 39
        // usable bytes, and its upper row is 36 with room for 39. A `value`-
        // sized buffer here would cut both at a different byte from the panel
        // and the report would quietly stop describing the screen.
        char row[sizeof(ui_clock_row[0])];
        // ONE READ OF THE MODE FOR BOTH ROWS, for the reason the card object
        // below takes one read of ui_msg_id: the mode is derived from live
        // radio state, and two reads could put row 0 from one shape beside
        // row 1 from another — a pair that never existed on the glass.
        ui_crow_mode_t mode = ui_status_row_mode();
        for (int r = 0; r < 2; r++) {
            ui_status_row(mode, r, row, sizeof(row), true);
            rows.add(row);
        }
    }

    // The menu goes out on every screen, not just while it is open: it is the
    // list of what the keyboard can reach, and an agent asking what this node
    // can be told to do should not have to navigate there first.
    //
    // It scrolls now, so where the window sits is part of what is on the glass:
    // "selected" alone no longer says which entries are visible.
    JsonObject menu = doc["menu"].to<JsonObject>();
    menu["selected"] = ui_menu_index;
    menu["first"] = ui_menu_first;
    menu["rows"] = UI_ROWS;
    JsonArray items = menu["items"].to<JsonArray>();
    for (int i = 0; i < UI_MENU_COUNT; i++) {
        JsonObject it = items.add<JsonObject>();
        it["title"] = ui_menu[i].title;
        ui_menu_state(i, value, sizeof(value));
        it["state"] = value;
    }

    // The message list's position, on every screen for the same reason the menu
    // is: it is retained state, and it is where the list will be when it is
    // next opened.
    JsonObject msgs = doc["messages"].to<JsonObject>();
    msgs["count"] = notify_count();
    msgs["unread"] = notify_unread_count();
    msgs["selected"] = ui_msg_sel;
    msgs["first"] = ui_msg_first;
    msgs["rows"] = UI_ROWS;

    // The card, only while it is up, because only then is there a subject.
    //
    // THE BODY GOES OUT AS THE TWO LINES THE PANEL WRAPPED, NOT AS THE RAW
    // STRING, and that is the difference between a report and a description of
    // one. NOTIFY_BODY_LEN is 97 and every buffer on this path is 48, so a body
    // pushed through ui_field() would be cut at 47 characters while the panel
    // showed all 96 over two lines.
    //
    // Neither line can truncate, and the reason is the BUFFER and not the
    // column. ui_card_body[] is sized from NOTIFY_BODY_LEN, so each line has
    // room for a whole 96-byte body on its own and the wrap is free to put the
    // break wherever the pixels fall. It is emphatically NOT that a 206px
    // column holds 48 characters: Font2's narrowest advance is 3px — `.` `,`
    // `:` `;` `!` `|` — so 206px takes up to 68 of them, and a buffer sized to
    // the column rather than to the body would cut a punctuation-heavy line in
    // the middle. Nothing here shrinks with the column.
    //
    // They are READ FROM THE DRAW CACHE AND NOT WRAPPED HERE. Wrapping calls
    // ui_wrap2(), which installs a font on the one shared M5.Display and
    // advances its UTF-8 decoder; this handler runs on the AsyncTCP task, which
    // preempts loop(), and doing it here would corrupt whatever draw was in
    // flight — with the field cache then recording the mis-rendered string as
    // correct, so the row would stay wrong. The cache is what loop() already
    // computed and already drew.
    if (ui_screen == UI_MESSAGE) {
        JsonObject card = doc["card"].to<JsonObject>();
        // ONE READ OF ui_msg_id FOR THE WHOLE OBJECT. The card can step to the
        // next message on the loop task while this handler runs, and an id
        // reported from one read beside a lookup made from another would
        // describe two different messages under one heading.
        uint32_t id = ui_msg_id;
        card["id"] = id;
        NotifyView v;
        int idx = 0, total = 0;
        if (notify_view_by_id(id, v, &idx, &total)) {
            card["present"] = true;
            card["source"] = v.source;
            card["title"] = v.title;
            card["level"] = notify_level_name(v.level);
            card["unread"] = v.unread;
            card["age_s"] = v.age_s;
            card["index"] = idx;
            card["total"] = total;
        } else {
            // The next tick leaves for the list; until then the screen is still
            // the card and saying so is more honest than omitting the object.
            card["present"] = false;
        }
        // ONLY WHILE THE CACHE BELONGS TO THIS CARD. See ui_card_body_id: the
        // draw that fills those two lines happens a tick after the card opens
        // and a tick after every step through the stack, so an ungated read
        // publishes the previous message's body under this message's id. The
        // fields are omitted rather than emptied, because an absent key reads
        // as "not known yet" while an empty string reads as "this message has
        // no body", and those are different facts.
        if (ui_card_body_id == id) {
            card["body1"] = ui_card_body[0];
            card["body2"] = ui_card_body[1];
        }
    }

    JsonObject key = doc["last_key"].to<JsonObject>();
    if (ui_last_key) {
        char k[2] = {ui_last_key, '\0'};
        key["key"] = ui_last_key == '\n' ? "enter" : k;
        // Same cross-task shape as the OTA watchdog, and signed for the same
        // reason: ui_last_key_ms is stamped on the loop task and read here on
        // AsyncTCP, so a key pressed between the two reads would otherwise
        // report an age of about 4.29 billion milliseconds instead of zero.
        // Only the report is at stake here, not a decision, but a field that
        // occasionally prints 49 days for a key just pressed is still wrong.
        long age = (long)(millis() - ui_last_key_ms);
        key["ms_ago"] = (unsigned long)(age < 0 ? 0 : age);
    }

    // The bare keys navigation is on, repeated here so an agent reading /ui can
    // work out what to tell somebody standing at the device.
    //
    // IT DESCRIBED THE MENU AND ONLY THE MENU. Two screens have since given
    // the same keys different jobs — `;` and `.` step through the message
    // stack on a card, and Enter there acknowledges while backtick deliberately
    // does not — and telling an agent "Enter select" when the person in front
    // of the device is looking at a card is telling them the wrong thing about
    // the one key whose two exits mean different things. /skill was brought up
    // to date when the card shipped and this string was not, so the two
    // documents an agent has disagreed about the device.
    doc["keys"] = "; up, . down (menu, message list, or previous/next card), "
                  ", dim, / bright, Enter select — on a card it ACKNOWLEDGES "
                  "the message, ` back — on a card it leaves it UNREAD";

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
// the paths that run before it. Identity is a session id, not the request
// pointer it used to be: a pointer is only unique while the object it names is
// alive, and this one is destroyed the instant the client disconnects — see
// WebRequest.cpp's destructor — while ota_in_progress stays true for up to the
// stall timeout. A later request whose AsyncWebServerRequest is allocated at
// that same freed address would then pass a pointer-equality check and have its
// chunks accepted into the abandoned transfer. The session id is stamped into
// this request's own _tempObject (a small heap tag the library frees when the
// request is destroyed) and cannot be inherited by whatever reuses the address:
// a fresh request's _tempObject is NULL until it claims a session of its own.
// Two continuity checks back it up — the declared total and the running byte
// offset both have to match what the owner established — so even a forged id
// cannot splice a differently-shaped stream in. The abandoned-connection path
// is precisely the exit that does NOT clear ota_in_progress here; the watchdog
// in loop() is what clears the session on that path.
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

        // Stamp this request with a fresh session id and record it as the tag
        // the library frees on destroy. A request that reuses this address later
        // starts with _tempObject NULL and so cannot inherit the claim.
        uint32_t *tag = (uint32_t*)malloc(sizeof(uint32_t));
        if (!tag) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "OOM claiming upload");
            return;
        }
        ota_session = ota_next_session++;
        if (ota_next_session == 0) ota_next_session = 1;  // never hand out 0
        *tag = ota_session;
        request->_tempObject = tag;

        ota_claimed_total = total;
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
            ota_session = 0;
            return;
        }
        ota_upload_started = true;
    }

    // Chunks of a request that never claimed the slot, or whose session has been
    // retired (the abandoned-upload watchdog zeroes ota_session). The tag is
    // this request's own copy of the id it claimed; a request that reused a
    // freed address has no tag at all. Nothing of theirs is being tracked, so
    // there is nothing to do with their bytes.
    uint32_t *tag = (uint32_t*)request->_tempObject;
    if (!tag || *tag != ota_session) return;
    // Continuity: a chunk that disagrees about the transfer's shape is not part
    // of this transfer. index is the running byte offset the library hands us;
    // it must track what has actually been written, and total must not change
    // mid-stream. Either mismatch means the streams have been spliced.
    if (total != ota_claimed_total || index != ota_bytes_written) return;

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
            ota_session = 0;
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
        ota_session = 0;
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

    String s;
    // Sized from what this actually produces, not from a guess: the live node
    // answers GET /skill with 14308 bytes today, and the two skills' describe()
    // blocks below are most of it. Without this the document is assembled by
    // about 125 successive += on an empty String, and Arduino's String::concat
    // reserves the exact new length every time, so each one is a realloc that
    // copies everything written so far and leaves the old block behind. On a
    // board with no PSRAM and one heap this is the most fragmentation-prone
    // path in the firmware — it is also the one that then needs a contiguous
    // 14KB block to hand to the response. One allocation up front instead.
    // Raise this if the answer outgrows it; overshooting costs a transient
    // 16KB against a free heap that measures around 195KB.
    s.reserve(16384);
    s = "# ESP32 Seed — M5Stack Cardputer ADVANCE\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    if (ap_active) s += String("AP: ") + ap_ssid + " (setup mode; the password is on the node's screen)\n";
    s += "\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n";
    s += "The token is a 32-char hex string, generated on the node's first boot and\n";
    s += "kept for its life. Two places give it to you: the serial console prints it\n";
    s += "at every boot (115200 8N1, `Token: ...`), and the setup AP's page at `/`\n";
    s += "shows it while the node is being provisioned. The AP is down whenever the\n";
    s += "node is on WiFi, so on a provisioned node the serial console is the only\n";
    s += "source — the AP can be raised again from the keyboard (Enter, Setup AP),\n";
    s += "but that needs somebody at the device\n\n";
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
    s += "- GPIO38 is the display backlight and also gates the RGB LED supply. It is\n";
    s += "  not a plain output here: M5GFX attaches it to an LEDC channel and dims it,\n";
    s += "  so the backlight is the brightness value in GET /ui rather than a pin\n";
    s += "  level. A brightness of 0 also cuts the RGB LED's supply\n";
    s += "- The mainboard I2C bus (SDA=8/SCL=9) carries exactly three devices: TCA8418\n";
    s += "  keyboard controller 0x34, ES8311 codec 0x18, BMI270 IMU 0x69. Anything else\n";
    s += "  in /capabilities' i2c_devices came from a cap or the Grove port. Entries\n";
    s += "  there are keyed `device` when the part is documented or probed for this\n";
    s += "  board and `device_guess` when it is only a common part at that address —\n";
    s += "  do not treat a guess as an identification\n";
    s += "- That bus has exactly ONE owner and it is M5Unified's `In_I2C` (I2C_NUM_1),\n";
    s += "  brought up by M5.begin(). Do not add an Arduino `Wire` on 8/9: a second\n";
    s += "  controller re-points the pads' output selector, and whichever driver loses\n";
    s += "  keeps reporting success while talking to nothing. It fails silently, with\n";
    s += "  no error anywhere. Use `M5.In_I2C` (or `m5::In_I2C`) for anything on this\n";
    s += "  bus. The Grove port on G1/G2 is a separate bus, `Ex_I2C`, and is NOT\n";
    s += "  started — it is yours if you begin() it\n";
    s += "- Flashing: `pio run -e cardputer -t upload` is one call and needs nothing\n";
    s += "  extra — platformio.ini already sets board_upload.after_reset=watchdog_reset\n";
    s += "- Only if you drive esptool directly: with its default `--after hard_reset`\n";
    s += "  this chip is observed to stay in the bootloader after a successful write\n";
    s += "  instead of running the image, which then looks like a flash that did\n";
    s += "  nothing. `--after watchdog_reset` boots it. The remedy is reliable; the\n";
    s += "  mechanism is not documented well enough to assert one here\n";
    s += "- The clock runs on UTC until POST /clock/tz stores a POSIX TZ string in SPIFFS\n";
    s += "- The setup AP comes up on its own only while the node has no working WiFi\n";
    s += "  credentials, with a fresh random password per raise. It tears itself down\n";
    s += "  the moment the STA link comes up and does not come back if WiFi is lost\n";
    s += "  later. It can also be raised by hand from the on-device menu, and a session\n";
    s += "  raised that way expires after 10 minutes. The password is shown on the\n";
    s += "  node's screen and NOWHERE else: it is never persisted, never printed to the\n";
    s += "  console, and GET /ui redacts it\n";
    s += "- POST /wifi/config needs the token unless the request comes over that AP\n";
    s += "- Paths match exactly: `/health/` is not `/health` and returns 404. Nothing\n";
    s += "  here generates a trailing slash, but a client that appends one will break\n";
    s += "- An OTA upload whose connection dies is torn down after 30s without data,\n";
    s += "  so a dropped transfer no longer blocks every later one until reboot\n\n";
    s += "## On-device UI\n\n";
    s += "The 240x135 panel and the QWERTY are driven. Seven screens: status, menu,\n";
    s += "network, system, hardware, messages (the notification queue as a\n";
    s += "scrolling list) and message (one notification as a card). Navigation is\n";
    s += "on the BARE keys, no Fn chord:\n\n";
    s += "| Key | Does |\n";
    s += "|-----|------|\n";
    // One table row per line: a markdown row that wraps is not a row.
    s += "| `;` / `.` | move the selection in the menu or the message list; on a card, step to the previous / next message |\n";
    s += "| `,` / `/` | dim / brighten the panel, on any screen |\n";
    s += "| Enter | open the menu, activate the selected entry, or — on a card — acknowledge the message and return to the list |\n";
    s += "| `` ` `` | back — a card returns to the list LEAVING IT UNREAD, a screen returns to the menu, the menu to status |\n\n";
    s += "The menu scrolls: it has six entries and five rows, and the footer\n";
    s += "carries the position. Entries, in order: Messages FIRST, carrying the\n";
    s += "queue's state beside it — \"empty\", \"3 new / 8\" or \"8 read\" — then\n";
    s += "Network, System, Hardware, Setup AP (raise the provisioning AP; it comes\n";
    s += "down on its own, there is no keyboard path to drop it), Backlight (panel\n";
    s += "dark / lit).\n\n";
    s += "A notification arriving while the node sits on its status screen raises\n";
    s += "its own card, fading in over three 40ms frames. It does NOT interrupt any\n";
    s += "other screen — the unread count in the menu is how it gets noticed then.\n\n";
    s += "You cannot see this screen. GET /ui answers what is on it: the active\n";
    s += "screen, the backlight level, whether the panel came up, whether M5\n";
    s += "identified the board, and the last key the firmware saw. Its `content`\n";
    s += "field names where that screen's content is reported, because not every\n";
    s += "screen has rows: three fill `fields[]`, status fills `clock`, the menu\n";
    s += "fills `menu` (entries, selection and window offset, since it scrolls),\n";
    s += "the message list fills `messages` (count, unread, selection and window),\n";
    s += "and the card fills `card`. An empty `fields[]` on those four is correct,\n";
    s += "not a failure.\n\n";
    s += "Status is a clock face, not a table: a large HH:MM, the seconds beside\n";
    s += "it, the date under both, and two rows for how the node is reached. It\n";
    s += "has no key legend along the bottom — the stack needs the room — but ENT\n";
    s += "still opens the menu and `,` / `/` still change the brightness — and\n";
    s += "the lower row says exactly that while the node is offline and has\n";
    s += "nothing better to report. `clock`\n";
    s += "reports `time`, `seconds` and `date` as the panel drew them, plus\n";
    s += "`synced` — false until the first NTP sync, when the face reads `--:--`\n";
    s += "and the date row says so. Those four are ABSENT until a draw has\n";
    s += "actually filled them: for the first tick after boot, and for the whole\n";
    s += "boot on a node whose panel did not come up. Absent means the screen is\n";
    s += "not known, not that it is blank; GET /clock answers the time itself.\n";
    s += "`rows` is always present and is recomputed per request rather than\n";
    s += "read back from the panel, because one of its three shapes carries the\n";
    s += "setup AP's password and that has to be redacted here. The shapes are\n";
    s += "mutually exclusive. AP up: the SSID, how long the session has left and\n";
    s += "the password — REDACTED, it exists on the panel and nowhere else —\n";
    s += "then the auth token, which is NOT redacted, because this endpoint\n";
    s += "already required it and there is nothing left to leak. On the network:\n";
    s += "signal strength, then the mDNS name and port. Offline: says so, then\n";
    s += "the key hint. The AP shape is tested FIRST and both it and the link\n";
    s += "can be true at once, so a node with a live AP reports credentials and\n";
    s += "not signal strength.\n\n";
    s += "The card reports its subject by id, plus `body1` and `body2` — THE TWO\n";
    s += "LINES THE PANEL ACTUALLY WRAPPED, not the raw body. A body is 96\n";
    s += "characters and the report buffers are 48, so the raw string would be cut\n";
    s += "in half while the panel showed all of it; these two are what is on the\n";
    s += "glass. `present: false` means the message expired out from under the card\n";
    s += "and the next tick returns to the list.\n\n";
    s += "It reports content, not pixels — it says nothing about layout or\n";
    s += "legibility, and the AP password is redacted there even though it is on\n";
    s += "the screen.\n\n";
    s += "The display is written only from loop(). If you add a handler, it may READ\n";
    s += "UI state and must never draw: handlers run on the AsyncTCP task and will\n";
    s += "preempt a half-finished paint, corrupting M5GFX's font/datum/SPI state.\n\n";
    s += "## API\n\n";
    s += "| Method | Path | Description |\n";
    s += "|--------|------|-------------|\n";
    s += "| GET | /health | Alive (no auth) |\n";
    s += "| GET | /capabilities | Hardware info |\n";
    s += "| GET | /config.md | Node config |\n";
    s += "| POST | /config.md | Update config |\n";
    s += "| GET | /events | Event log |\n";
    s += "| GET | /ui | What the panel is showing right now |\n";
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
    // Into the fixed buffers rather than at them: see the declaration for why
    // assigning a String here was a use-after-free for the panel's reader.
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid.c_str());
    snprintf(wifi_pass, sizeof(wifi_pass), "%s", pass.c_str());
    request->send(200, "text/html",
        "<h2>Saved. Connecting...</h2><p>" + html_escape(ssid) +
        "</p><a href='/'>Back</a>");
    // Hand the reconnect to loop() rather than doing it here — see the flag's
    // declaration. The page above is already on its way out, so the browser
    // gets its answer at once instead of a second later.
    pending_wifi_connect = true;
}

// ===== Skills =====
//
// Skills are #included, not compiled separately: build_src_filter in
// platformio.ini excludes src/skills/ from the build so that each one lands in
// THIS translation unit. That is what lets a skill use the file-scope helpers
// above without a header to keep in step with them.
//
// Order is load-bearing between these two. serial.cpp calls gpio_pin_exists()
// and gpio_refuse_reason() from gpio.cpp, so that it validates UART pins
// against exactly the list POST /gpio/write refuses.
#include "skills/gpio.cpp"
#include "skills/serial.cpp"

static void skills_init() {
    skill_gpio_init();
    skill_serial_init();
    // Reads the stored queue off SPIFFS, so it has to run after SPIFFS.begin()
    // in setup() — which it does, skills_init() being called well below it.
    skill_notify_init();
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
    server.on(AsyncURIMatcher::exact("/ui"), HTTP_GET, handle_ui);
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

    // First, and before hw_probe(): M5.begin() is what brings the I2C bus up,
    // and it is the seed's only I2C owner. See the ownership note above
    // hw_probe() for what happens to a second one.
    ui_begin();
    hw_probe();       // I2C scan, through M5's bus
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
    // The setup AP is torn down the moment the STA link comes up, and the only
    // way to raise it again is the gesture on the node's own keyboard. So after
    // a successful provisioning the page that hands out the token behind
    // from_setup_ap() is unreachable to anyone not standing at the device;
    // /capabilities sits behind require_auth() like every route but /health, so
    // it cannot hand it out either. Without this line the node's whole grow
    // cycle — /capabilities, /firmware/upload, /firmware/apply,
    // /firmware/confirm — is locked away from the node's own owner.
    //
    // The panel now exists and could carry it, and that is precisely the
    // argument for leaving this here rather than moving it: the screen is the
    // WEAKER channel of the two. It is readable across a room, over a shoulder
    // and by any camera pointed at the desk, where Serial costs a USB cable
    // physically attached. And the token's consumer is not a person reading a
    // panel — it is an agent pasting 32 hex characters into a header, off a
    // console it is already connected to.
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
        // The AP password is NOT printed. It used to be, as a bring-up crutch
        // with an explicit note to remove it once the panel could carry it
        // instead; the panel carries it now, on the STATUS screen, for as long
        // as the AP is up. That is the channel the password needs, because the
        // AP can now be raised from the keyboard with no cable anywhere near
        // the board — and a secret whose only copy is behind a USB port is
        // useless to the person standing in front of the device.
        //
        // The one exception, and it is not a hedge: if the panel never came up,
        // the reason for taking the print away has not happened. A node with a
        // dead screen and no stored credentials that also refuses to say its
        // AP password is a node nobody can provision at all.
        Serial.printf("http://%s:%d/health  (setup AP: %s, password is on the "
                      "node's screen)\n",
            WiFi.softAPIP().toString().c_str(), HTTP_PORT, ap_ssid);
        if (!ui_ready) {
            Serial.printf("[!] panel not initialised; AP password: %s\n",
                          ap_password);
        }
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

    // Auto-confirm after 60s.
    //
    // firmware_confirmed records what the ROM actually did, not that it was
    // asked — the same rule handle_firmware_confirm() follows, and the two
    // paths set the one flag so they have to agree. This one used to set it
    // unconditionally and merely suppress the event on failure, which left the
    // node reporting the single state an operator most needs told truthfully as
    // its exact opposite: /firmware/version and the SYSTEM screen both read
    // "confirmed" while the rollback was still armed, so the next reboot threw
    // the image away with nothing anywhere having said it might.
    //
    // The failure is logged rather than passed over in silence: it is the only
    // notice that the window is closing, since firmware_confirm_attempted stops
    // this from ever running again and POST /firmware/confirm is then the only
    // way to commit the image.
    if (!firmware_confirmed && !firmware_confirm_attempted &&
        (millis() - boot_time) > 60000 && WiFi.status() == WL_CONNECTED) {
        firmware_confirm_attempted = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        firmware_confirmed = (err == ESP_OK);
        // Two format strings, not one with an argument: the success message has
        // nothing to substitute, and event_add() is printf-style.
        if (firmware_confirmed) {
            event_add("firmware auto-confirmed");
        } else {
            event_add("firmware auto-confirm FAILED (%s), rollback still armed",
                      esp_err_to_name(err));
        }
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
    // The cast is load-bearing and must not be tidied away. ota_last_chunk_ms
    // is stamped on the AsyncTCP task, which preempts this one: a chunk landing
    // between the millis() call and the subtraction leaves the stamp NEWER than
    // the reading it is subtracted from. In unsigned arithmetic that difference
    // does not go negative, it wraps to about 4.29 billion, sails past the
    // timeout and aborts a perfectly healthy upload. Evaluated as signed, the
    // same case is a small negative number and correctly reads as "not stalled".
    // The signed form is also what survives the millis() rollover at 49.7 days.
    if (ota_in_progress &&
        (long)(millis() - ota_last_chunk_ms) > (long)OTA_STALL_TIMEOUT_MS) {
        Update.abort();
        ota_in_progress = false;
        // Retire the session for the next uploader. The dead request is gone;
        // its id must not still match an incoming chunk. Any chunk that arrived
        // late from the abandoned connection now fails the tag/session check.
        ota_session = 0;
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
    // Freshly provisioned credentials, handed over by POST /wifi/config. Acting
    // on them here rather than in the handler is what keeps this WiFi.begin()
    // the only one in the firmware: two tasks calling it on one radio was the
    // race, and the retry below is the other half of it. Connect at once rather
    // than making a just-provisioned node wait out the retry interval, and stamp
    // last_wifi so the retry does not immediately fire on top of this attempt.
    //
    // The flag is cleared before the call, not after: a second POST landing
    // while WiFi.begin() runs then sets it again and is reissued on the next
    // pass, instead of being cleared away unserved.
    if (pending_wifi_connect) {
        pending_wifi_connect = false;
        event_add("wifi credentials updated, reconnecting");
        WiFi.begin(wifi_ssid, wifi_pass);
        last_wifi = millis();
    }
    if (wifi_ssid[0] && WiFi.status() != WL_CONNECTED &&
        millis() - last_wifi > 30000) {
        WiFi.begin(wifi_ssid, wifi_pass);
        last_wifi = millis();
    }

    // Retire the provisioning AP once it has done its job, or once the window
    // a keyboard-raised session opened has run out. Non-blocking.
    ap_poll();

    // Expiry and the coalesced SPIFFS write. Above ui_tick() on purpose: an
    // expiry handled on this pass raises ui_force, and the ui_tick() below then
    // acts on it in the same pass instead of leaving the screen stale for the
    // delay(10). Arrivals raise the flag too, but from the AsyncTCP task and at
    // any point in the pass — notify_poll() handles none of them — so their
    // latency owes nothing to this placement.
    notify_poll();

    // The keyboard and the screen, in that order and only from here. Every
    // HTTP handler runs on the AsyncTCP task and must never draw; see the note
    // at the top of the UI section for what a second writer does to M5GFX.
    ui_key_poll();
    ui_tick();

    delay(10);
}
