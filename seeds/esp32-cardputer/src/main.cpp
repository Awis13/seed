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
// Two skills ship: gpio and serial, both at the bottom of this file. Everything
// a skill needs — the PIN_* map, gpio_is_safe(), require_auth(), event_add(),
// handle_body_collect() — is declared above the #includes that pull them in.

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
// read by ui_field() on the AsyncTCP task (GET /ui) and a String reassignment
// frees the buffer a concurrent reader is walking. Written once in wifi_setup()
// before the web server starts and never again, so today no writer races it —
// but the panel's cross-task reader makes that a property to keep by
// construction, not one to rely on staying true. "Seed-" + four hex + NUL fits
// in ten; sized with headroom.
static char ap_ssid[16] = "";
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
// ap_stop clears it) and read on the AsyncTCP task by ui_field() — the exact
// cross-task String reassignment that was a use-after-free for the WiFi
// credentials. GET /ui never reaches the read (it passes redact=true and
// returns a placeholder first), but the panel does, and defence here costs a
// dozen bytes. The generated password is twelve chars; sized with headroom.
static char ap_password[16] = "";
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
// before the UI section below. Written from loop() context only.
static bool ui_force = false;

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
    // %.4s, not %s. get_mac_suffix() returns exactly four hex characters, so
    // "Seed-" + suffix is nine and fits with room to spare — but the compiler
    // cannot see inside the String and warned that up to 14 bytes could go into
    // an 11-byte tail (-Wformat-truncation). The precision makes the invariant
    // the source already relies on visible to the compiler instead of implied,
    // and clamps the write if get_mac_suffix() ever returns something longer.
    // Not silenced on the grounds that it looked like a false positive: the
    // GNSS block in serial.cpp shipped truncated on a live node while the same
    // warning was being reported and read as noise.
    snprintf(ap_ssid, sizeof(ap_ssid), "Seed-%.4s", suffix.c_str());
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
//    node. GET /ui and the panel derive every value from the same ui_field()
//    calls, so the endpoint reports the screen's real content rather than a
//    second, parallel description of it that can drift.

// Layout, in pixels, for the 240x135 panel in landscape.
//
//   0..18    header: screen title left, network summary right
//   20       rule
//   26..114  five rows, 18px apart
//   116      rule
//   121..129 footer: the key legend for the current screen
#define UI_ROWS         5
#define UI_HDR_Y        2
#define UI_RULE1_Y     20
#define UI_ROW0_Y      26
#define UI_ROW_PITCH   18
#define UI_ROW_H       18
#define UI_RULE2_Y    116
#define UI_FOOT_Y     121
// Two columns on a data row: a dim label, then the value.
#define UI_LABEL_X      4
#define UI_LABEL_W     50
#define UI_VALUE_X     56
#define UI_VALUE_W    182
#define UI_MENU_X       8
#define UI_MENU_R_X   232

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

// Text anchors. Spelled out here because M5GFX keeps them in a namespace that
// no using-directive drags into global scope, and the short TL_DATUM spellings
// are only reachable from inside the library's own namespaces.
static const uint8_t UI_TL = lgfx::textdatum::top_left;
static const uint8_t UI_TR = lgfx::textdatum::top_right;

enum ui_screen_t {
    UI_STATUS = 0,
    UI_MENU,
    UI_NETWORK,
    UI_SYSTEM,
    UI_HARDWARE,
    UI_SCREEN_COUNT
};

static const char *ui_screen_name[UI_SCREEN_COUNT] = {
    "status", "menu", "network", "system", "hardware"
};
static const char *ui_screen_title[UI_SCREEN_COUNT] = {
    "STATUS", "MENU", "NETWORK", "SYSTEM", "HARDWARE"
};

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

static const UiMenuItem ui_menu[] = {
    {"Network",   UI_ACT_SCREEN,    UI_NETWORK},
    {"System",    UI_ACT_SCREEN,    UI_SYSTEM},
    {"Hardware",  UI_ACT_SCREEN,    UI_HARDWARE},
    {"Setup AP",  UI_ACT_AP,        0},
    {"Backlight", UI_ACT_BACKLIGHT, 0}
};
#define UI_MENU_COUNT ((int)(sizeof(ui_menu) / sizeof(ui_menu[0])))
// The menu does not scroll, so it must fit the rows it has. Growing it past
// five entries is a real change — a window offset and a scroll indicator — not
// a one-line edit, and this is what stops that from being discovered on the
// device.
static_assert(UI_MENU_COUNT <= UI_ROWS, "menu does not scroll: keep it within UI_ROWS");

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
// ui_field() also reads ap_ssid and ap_password, which were the last two
// Strings on the /ui path; both are now fixed buffers too. Their write patterns
// differ and the difference is why they were not the same risk. ap_password is
// rolled by ap_start() and cleared by ap_stop(), both on the loop task, so a
// raise or drop concurrent with a report on AsyncTCP was a genuine reassignment
// race — narrowed only by /ui redacting it before the read. ap_ssid, despite
// what this ledger once said, is NOT written in ap_start()/ap_stop() at all: it
// is set once in wifi_setup() (before the web server exists) and never touched
// again, so it never actually raced. Both are fixed buffers now regardless, so
// the whole /ui path is String-free by construction rather than by argument.
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
// Sized to hold a whole value rather than a prefix of one: the change test is a
// strncmp against the cache, so a cache shorter than the strings it stores
// would silently stop repainting fields that differ only past its end.
static char ui_cache_label[UI_ROWS][16];
static char ui_cache_value[UI_ROWS][48];
static char ui_cache_foot[48];
static int ui_cache_sel = -1;

// ---- Field values ----

static void ui_uptime(char *out, size_t n) {
    unsigned long s = (millis() - boot_time) / 1000;
    unsigned long d = s / 86400;
    s %= 86400;
    if (d > 0) {
        snprintf(out, n, "%lud %02lu:%02lu:%02lu", d, s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(out, n, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    }
}

// One line summarising where the node can be reached, for the header.
//
// The octets are formatted by hand rather than through IPAddress::toString().
// That method builds and returns a String — a heap allocation, a copy and a
// free every time — and this function runs on every UI tick, twice per tick on
// the STATUS screen, which is the header plus the "net" row. At 5Hz that was
// roughly ten allocate/free pairs a second for the life of the boot, on a board
// with no PSRAM and a single 300-odd KB heap that also has to find a contiguous
// 6KB block whenever GET /skill is called. Nothing here needs the heap at all.
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
        default:
            break;
    }
}

// The single source of truth for what a screen shows. The panel calls this to
// draw a row; GET /ui calls it to report one. Both get their own buffers, so
// there is no shared string for the two tasks to race over, and the two can
// never describe the screen differently.
//
// `redact` is the whole reason this takes an argument at all. The provisioning
// AP's password is on the STATUS screen while the AP is up, because the screen
// is its only channel — setup() no longer prints it and it is never persisted.
// It must not leave the device. /ui passes redact=true and gets a placeholder;
// the panel passes false. Anything secret added here must go the same way.
//
// Returns false when the screen has no row at that index (the menu has no
// fields at all — it has entries, which GET /ui reports separately).
static bool ui_field(ui_screen_t screen, int idx, char *label, size_t label_n,
                     char *value, size_t value_n, bool redact) {
    label[0] = '\0';
    value[0] = '\0';
    if (idx < 0 || idx >= UI_ROWS) return false;

    switch (screen) {
    case UI_STATUS: {
        // While the AP is up the bottom two rows become the credentials
        // somebody standing in front of the node needs to type into a phone.
        // That is a deliberate swap, not an extra screen: those two rows are
        // worth less than the only copy of a password that exists.
        switch (idx) {
        case 0: {
            snprintf(label, label_n, "time");
            struct tm now;
            if (clock_local_time(now)) {
                strftime(value, value_n, "%Y-%m-%d %H:%M:%S", &now);
            } else {
                snprintf(value, value_n, "waiting for NTP");
            }
            return true;
        }
        case 1:
            snprintf(label, label_n, "net");
            ui_net_summary(value, value_n);
            return true;
        case 2:
            snprintf(label, label_n, "up");
            ui_uptime(value, value_n);
            return true;
        case 3:
            if (ap_active) {
                snprintf(label, label_n, "ap");
                // Which kind of session this is, next to the SSID, because the
                // two behave differently and the difference matters to somebody
                // standing here: a keyboard-raised AP closes on a timer, the
                // boot AP stays up until the node is actually provisioned.
                if (ap_temporary) {
                    snprintf(value, value_n, "%s  %lum left", ap_ssid,
                             ap_minutes_left());
                } else {
                    snprintf(value, value_n, "%s  stays up", ap_ssid);
                }
            } else {
                snprintf(label, label_n, "heap");
                snprintf(value, value_n, "%lu KB free",
                         (unsigned long)(ESP.getFreeHeap() / 1024));
            }
            return true;
        case 4:
            if (ap_active) {
                snprintf(label, label_n, "pw");
                if (redact) {
                    snprintf(value, value_n, "(on the panel only)");
                } else {
                    // Nothing but the password on this row. The session's
                    // remaining time moved up to the "ap" row above, where it
                    // does not compete for width with the one string on this
                    // screen that has to be transcribed without a typo.
                    snprintf(value, value_n, "%s", ap_password);
                }
            } else {
                snprintf(label, label_n, "seed");
                snprintf(value, value_n, "v%s", SEED_VERSION);
            }
            return true;
        default:
            return false;
        }
    }

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

    case UI_MENU:
    default:
        return false;
    }
}

// The key legend, which is the only documentation a user standing in front of
// the node gets. Kept in sync with ui_handle_key() by being right next to it.
static void ui_footer(ui_screen_t screen, char *out, size_t n) {
    switch (screen) {
    case UI_STATUS:
        snprintf(out, n, "ENT menu   , / dim/bright");
        break;
    case UI_MENU:
        snprintf(out, n, "; . move  ENT select  ` back");
        break;
    default:
        snprintf(out, n, "` back  ENT menu  , / dim/bright");
        break;
    }
}

// ---- Drawing ----

// Repaint one text field, and only if its content changed.
//
// The opaque background plus a fixed padding width is what erases the previous
// value, so nothing here ever needs fillScreen and nothing ever flickers. The
// cache keys on the text alone: a field whose colours change but whose string
// does not needs ui_force, and every caller that moves a field onto or off a
// coloured ground is a screen transition that already sets it.
static void ui_draw_field(char *cache, size_t cache_n, const char *text,
                          int32_t x, int32_t y, const lgfx::IFont *font,
                          uint16_t color, uint8_t datum, uint16_t padding,
                          uint16_t bg) {
    if (!ui_force && strncmp(cache, text, cache_n - 1) == 0) return;
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
static void ui_draw_frame() {
    M5.Display.fillScreen(COL_BG);
    M5.Display.drawFastHLine(0, UI_RULE1_Y, M5.Display.width(), COL_RULE);
    M5.Display.drawFastHLine(0, UI_RULE2_Y, M5.Display.width(), COL_RULE);
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
        if (ui_screen == UI_MENU) {
            ui_menu_index = (ui_menu_index + UI_MENU_COUNT - 1) % UI_MENU_COUNT;
        }
        break;
    case '.':
        if (ui_screen == UI_MENU) {
            ui_menu_index = (ui_menu_index + 1) % UI_MENU_COUNT;
        }
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
        } else {
            ui_goto(UI_MENU);
        }
        break;
    case '`':
        ui_goto(ui_screen == UI_MENU ? UI_STATUS : UI_MENU);
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

    static unsigned long last_tick = 0;
    if (!ui_force && millis() - last_tick < UI_TICK_MS) return;
    last_tick = millis();

    if (ui_force) {
        ui_draw_frame();
        ui_cache_sel = -1;
    }

    // Header.
    ui_draw_field(ui_cache_hdr, sizeof(ui_cache_hdr), ui_screen_title[ui_screen],
                  UI_LABEL_X, UI_HDR_Y, &fonts::Font2, COL_ACCENT,
                  UI_TL, 110, COL_BG);
    char buf[48];
    ui_net_summary(buf, sizeof(buf));
    ui_draw_field(ui_cache_net, sizeof(ui_cache_net), buf,
                  M5.Display.width() - UI_LABEL_X, UI_HDR_Y, &fonts::Font2,
                  COL_DIM, UI_TR, 124, COL_BG);

    if (ui_screen == UI_MENU) {
        for (int row = 0; row < UI_ROWS; row++) {
            int32_t y = ui_row_y(row);
            bool selected = (row == ui_menu_index);
            // The selection bar is not a field, so it is painted from an
            // explicit test: only the row that gained the highlight and the one
            // that lost it are touched.
            bool moved = ui_force || (ui_cache_sel != ui_menu_index &&
                                      (selected || row == ui_cache_sel));
            uint16_t bg = selected ? COL_ACCENT : COL_BG;
            uint16_t fg = selected ? COL_BG : COL_TEXT;
            if (moved) {
                M5.Display.fillRect(0, y - 1, M5.Display.width(), UI_ROW_H, bg);
                ui_cache_label[row][0] = '\0';
                ui_cache_value[row][0] = '\0';
            }
            const char *title = row < UI_MENU_COUNT ? ui_menu[row].title : "";
            ui_draw_field(ui_cache_label[row], sizeof(ui_cache_label[row]), title,
                          UI_MENU_X, y, &fonts::Font2, fg, UI_TL,
                          140, bg);
            ui_menu_state(row, buf, sizeof(buf));
            ui_draw_field(ui_cache_value[row], sizeof(ui_cache_value[row]), buf,
                          UI_MENU_R_X, y, &fonts::Font2,
                          selected ? COL_BG : COL_DIM, UI_TR, 80, bg);
        }
        ui_cache_sel = ui_menu_index;
    } else {
        char label[16], value[48];
        for (int row = 0; row < UI_ROWS; row++) {
            int32_t y = ui_row_y(row);
            ui_field(ui_screen, row, label, sizeof(label), value, sizeof(value),
                     false);
            ui_draw_field(ui_cache_label[row], sizeof(ui_cache_label[row]), label,
                          UI_LABEL_X, y, &fonts::Font2, COL_DIM,
                          UI_TL, UI_LABEL_W, COL_BG);
            ui_draw_field(ui_cache_value[row], sizeof(ui_cache_value[row]), value,
                          UI_VALUE_X, y, &fonts::Font2, COL_TEXT,
                          UI_TL, UI_VALUE_W, COL_BG);
        }
    }

    ui_footer(ui_screen, buf, sizeof(buf));
    ui_draw_field(ui_cache_foot, sizeof(ui_cache_foot), buf, UI_LABEL_X,
                  UI_FOOT_Y, &fonts::Font0, COL_DIM, UI_TL,
                  236, COL_BG);

    ui_force = false;
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
    ui_force = true;
    ui_draw_frame();
    ui_draw_field(ui_cache_hdr, sizeof(ui_cache_hdr), "SEED", UI_LABEL_X,
                  UI_HDR_Y, &fonts::Font2, COL_ACCENT, UI_TL,
                  110, COL_BG);
    ui_draw_field(ui_cache_value[0], sizeof(ui_cache_value[0]), "starting...",
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

    // Skill endpoints
    for (int i = 0; i < g_skill_count; i++) {
        const SkillEndpoint *se = g_skills[i]->endpoints;
        for (int j = 0; se[j].path; j++) ep.add(se[j].path);
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
// This handler runs on the AsyncTCP task and therefore draws NOTHING; see the
// note at the top of the UI section. It reads word-sized scalars and calls
// ui_field() into its own buffers, which is why there is no lock and nothing to
// tear. It passes redact=true, so the provisioning AP's password — which lives
// only in RAM and on the panel — does not travel the network.
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

    JsonArray fields = doc["fields"].to<JsonArray>();
    char label[16], value[48];
    for (int row = 0; row < UI_ROWS; row++) {
        if (!ui_field(ui_screen, row, label, sizeof(label), value, sizeof(value),
                      true)) {
            break;
        }
        JsonObject f = fields.add<JsonObject>();
        f["label"] = label;
        f["value"] = value;
    }

    // The menu goes out on every screen, not just while it is open: it is the
    // list of what the keyboard can reach, and an agent asking what this node
    // can be told to do should not have to navigate there first.
    JsonObject menu = doc["menu"].to<JsonObject>();
    menu["selected"] = ui_menu_index;
    JsonArray items = menu["items"].to<JsonArray>();
    for (int i = 0; i < UI_MENU_COUNT; i++) {
        JsonObject it = items.add<JsonObject>();
        it["title"] = ui_menu[i].title;
        ui_menu_state(i, value, sizeof(value));
        it["state"] = value;
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
    doc["keys"] = "; up, . down, , dim, / bright, Enter select, ` back";

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
    s += "The 240x135 panel and the QWERTY are driven. Five screens: status, menu,\n";
    s += "network, system, hardware. Navigation is on the BARE keys, no Fn chord:\n\n";
    s += "| Key | Does |\n";
    s += "|-----|------|\n";
    s += "| `;` / `.` | move the selection up / down in the menu |\n";
    s += "| `,` / `/` | dim / brighten the panel, on any screen |\n";
    s += "| Enter | open the menu, or activate the selected entry |\n";
    s += "| `` ` `` | back — a screen returns to the menu, the menu to status |\n\n";
    s += "Menu entries: Network, System, Hardware, Setup AP (raise the\n";
    s += "provisioning AP; it comes down on its own, there is no keyboard path to\n";
    s += "drop it), Backlight (panel dark / lit).\n\n";
    s += "You cannot see this screen. GET /ui answers what is on it: the active\n";
    s += "screen, every field it is showing with its current value, the menu and\n";
    s += "which entry is selected, the backlight level, whether the panel came up,\n";
    s += "whether M5 identified the board, and the last key the firmware saw. It\n";
    s += "reports content, not pixels — it says nothing about layout or legibility,\n";
    s += "and the AP password is redacted there even though it is on the screen.\n\n";
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

    // The keyboard and the screen, in that order and only from here. Every
    // HTTP handler runs on the AsyncTCP task and must never draw; see the note
    // at the top of the UI section for what a second writer does to M5GFX.
    ui_key_poll();
    ui_tick();

    delay(10);
}
