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
/* No mDNS header here: mDNS is deliberately off on this board — see the
 * comment on the WiFi up/down transition in loop() for the crash it caused. */
#include <SPIFFS.h>
#include <Wire.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <Preferences.h>          // NVS-backed panic counter that survives poweron
#include <HTTPClient.h>
#include "boot_diag_decide.h"     // pure, host-tested NVS panic-counter decision
#include "secret_store.h"         // format-safe per-device secrets in NVS (C1/C2)
#include "board_pins.h"
#include "hw_ui.h"
#include "hw_input.h"
#include "micron/micron_store.h"
#include "micron/micron_layout.h"  // micron_layout_apply_scroll for in-page scroll
#include "hw_haptic.h"
#include "hw_sound.h"
#include "hw_kb.h"
#include "psram_alloc.h"          // psram_calloc_pref: park big buffers in PSRAM
#include "ui_nav.h"               // pure, host-tested back-navigation policy
#include "notify_chat_class.h"    // pure, host-tested: incoming chat vs notification card
#include "outbox.h"               // durable canonical outbound message store
#include "utf8_text.h"            // code-point-safe display labels
#include "settings_policy.h"      // pure auto-lock timing policy
#include "wifi_profile_store.h"   // format-safe NVS Wi-Fi profile store

// Bind the host-testable UiNavScreen ids to HwUiScreen so the two never drift.
static_assert((int)UINAV_CLOCK          == (int)HW_UI_CLOCK,          "ui_nav enum drift");
static_assert((int)UINAV_NOTIFY         == (int)HW_UI_NOTIFY,         "ui_nav enum drift");
static_assert((int)UINAV_CARD_ACT       == (int)HW_UI_CARD_ACT,      "ui_nav enum drift");
static_assert((int)UINAV_MENU           == (int)HW_UI_MENU,           "ui_nav enum drift");
static_assert((int)UINAV_AGENT_CHAT     == (int)HW_UI_AGENT_CHAT,     "ui_nav enum drift");
static_assert((int)UINAV_AGENT_ACT      == (int)HW_UI_AGENT_ACT,      "ui_nav enum drift");
static_assert((int)UINAV_AGENT_SESSIONS == (int)HW_UI_AGENT_SESSIONS, "ui_nav enum drift");
static_assert((int)UINAV_MSGLIST        == (int)HW_UI_MSGLIST,        "ui_nav enum drift");
static_assert((int)UINAV_INFO           == (int)HW_UI_INFO,           "ui_nav enum drift");
static_assert((int)UINAV_REPLY          == (int)HW_UI_REPLY,          "ui_nav enum drift");
static_assert((int)UINAV_LAYOUT         == (int)HW_UI_LAYOUT,         "ui_nav enum drift");
static_assert((int)UINAV_SETTINGS       == (int)HW_UI_SETTINGS,       "ui_nav enum drift");
static_assert((int)UINAV_MESHCORE       == (int)HW_UI_MESHCORE,       "ui_nav enum drift");
static_assert((int)UINAV_MESH_PING      == (int)HW_UI_MESH_PING,      "ui_nav enum drift");
static_assert((int)UINAV_WIFI           == (int)HW_UI_WIFI,           "ui_nav enum drift");
static_assert((int)UINAV_WIFI_LIST      == (int)HW_UI_WIFI_LIST,      "ui_nav enum drift");
static_assert((int)UINAV_WIFI_PROGRESS  == (int)HW_UI_WIFI_PROGRESS,  "ui_nav enum drift");
static_assert((int)UINAV_PAGE           == (int)HW_UI_PAGE,           "ui_nav enum drift");
static_assert((int)UINAV_CONTACTS       == (int)HW_UI_CONTACTS,       "ui_nav enum drift");
static_assert((int)UINAV_NET            == (int)HW_UI_NET,            "ui_nav enum drift");

// ===== Configuration =====
#define SEED_VERSION        "0.9.116"
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

/* Parked in PSRAM (~8.4 KB): written by event_add on the loop and AsyncTCP
 * tasks, read by the events endpoint — task context only, never an ISR.
 * Allocated on first use rather than in a _begin(): event_add already reports
 * from inside outbox_load(), before setup() reaches any init we could hang it
 * on. A null means both pools are exhausted, and then the event is dropped
 * rather than taking the boot down with it. */
static EventEntry *events_buf = nullptr;
static int events_head = 0;
static int events_count = 0;

static void event_add(const char *fmt, ...) {
    if (!events_buf) {
        events_buf = (EventEntry *)psram_calloc_pref(sizeof(EventEntry) * MAX_EVENTS);
        if (!events_buf) return;
    }
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

// ===== Boot Diagnostics =====
//
// A live stack-canary panic on the ipc1 task (the ESP-IDF cross-core IPC task,
// 1024-byte stack baked into the prebuilt Arduino core) reset the board with
// no trace: no coredump space, second boot clean. These counters make any
// recurrence measurable from the outside — the reset reason and crash counters
// are printed at boot and served in GET /health. RTC_NOINIT memory survives
// every reset except power-on; the magic word rejects power-on garbage.

#define BOOT_DIAG_MAGIC 0x42d1a607
RTC_NOINIT_ATTR static uint32_t boot_diag_magic;
RTC_NOINIT_ATTR static uint32_t boots_since_panic;
RTC_NOINIT_ATTR static uint32_t panic_count;
static esp_reset_reason_t reset_reason = ESP_RST_UNKNOWN;
static bool storage_ok = false;

// NVS-backed companions to the RTC counters above. Flash is NOT wiped on poweron,
// so these survive a manual reset (the button reports ESP_RST_POWERON, which
// wipes RTC_NOINIT). panics_since_flash is cleared only on a genuine reflash
// (build signature change); poweron_count tallies button+power events together.
// These are RAM mirrors of the NVS values, loaded once in boot_diag_init().
static uint32_t panics_since_flash = 0;
static uint32_t poweron_count = 0;

// The build signature stored in NVS. SEED_VERSION plus the compiler's build stamp
// both vary per build, so any reflash changes it and clears panics_since_flash;
// a bare poweron/button leaves it identical and preserves the count.
#define BOOT_DIAG_BUILD_SIG (SEED_VERSION " " __DATE__ " " __TIME__)
#define BOOT_DIAG_NVS_NS    "bootdiag"

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        case ESP_RST_EFUSE:     return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default:                return "unknown";
    }
}

// UNCONDITIONAL panic-class resets: real panics plus the interrupt/task
// watchdogs (both fire the panic handler on a genuine firmware hang), a CPU
// lockup (double exception) and a detected power glitch — unexpected faults
// that must count as instability. USB and JTAG resets are host-driven
// (flashing/debugging) and stay clean, like poweron. An efuse error reset is
// also clean: the eFuse controller re-read its block, a one-shot hardware
// event with no firmware cause, so counting it would misdirect the counters.
//
// ESP_RST_WDT (the generic RTC/other watchdog) is intentionally NOT listed
// here: it is ambiguous and gets a coredump-gated decision in boot_diag_init.
// It is BOTH what esptool's `--after watchdog_reset` fires on every plain
// reflash (board_upload.after_reset in platformio.ini) AND a genuine early
// RTC-watchdog hang in the bootloader / earliest init, before the task and
// interrupt watchdogs exist. A blanket exclude would drop that early-hang
// class; a blanket include would false-alarm panic_count on every reflash.
static bool reset_is_panic(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT ||
           r == ESP_RST_CPU_LOCKUP || r == ESP_RST_PWR_GLITCH;
}

// A CRC-valid ELF coredump image sits in the coredump partition only after a
// crash that reached the panic handler. Coredump-to-flash is enabled in the
// precompiled Arduino-ESP32 esp32s3 libs (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
// _DATA_FORMAT_ELF, _CHECKSUM_CRC32) and the partition table carries a
// data,coredump slot at 0xFF0000, so this returns true iff a real dump landed.
static bool coredump_image_present() {
    return esp_core_dump_image_check() == ESP_OK;
}

static void boot_diag_init() {
    reset_reason = esp_reset_reason();
    if (boot_diag_magic != BOOT_DIAG_MAGIC || reset_reason == ESP_RST_POWERON) {
        boot_diag_magic = BOOT_DIAG_MAGIC;
        boots_since_panic = 0;
        panic_count = 0;
    }
    bool is_panic = reset_is_panic(reset_reason);
    // ESP_RST_WDT is ambiguous (see reset_is_panic): discriminate by coredump
    // presence. The flash tool's `--after watchdog_reset` never leaves a dump,
    // so a plain reflash does NOT bump panic_count; a genuine early RTC-WDT
    // hang that reached the panic handler does leave one, so it still counts.
    // (Residual edge: a stale dump from an earlier real panic that was never
    // pulled/erased makes the next reflash-triggered ESP_RST_WDT count once —
    // accepted, since the dump is deliberately kept in flash for offline decode.)
    if (reset_reason == ESP_RST_WDT) {
        is_panic = coredump_image_present();
    }
    if (is_panic) {
        panic_count++;
        boots_since_panic = 0;
    } else {
        boots_since_panic++;
    }
    // NVS-persistent panic counter (survives poweron, unlike the RTC counter
    // above). Reuses the SAME is_panic decision, not a re-derived one. The pure
    // decision (boot_diag_decide.h) is host-tested; only the flash read/write is
    // here. Writes happen only on a panic, a fresh flash, or a poweron — never on
    // a plain boot — so there is no flash-wear concern. This runs early in setup()
    // on the single-task main core, so the NVS access needs no extra locking.
    {
        Preferences prefs;
        if (prefs.begin(BOOT_DIAG_NVS_NS, false)) {   // read-write namespace
            char stored_sig[64] = {0};
            size_t sig_len = prefs.getString("sig", stored_sig, sizeof(stored_sig));
            uint32_t stored_panics = prefs.getULong("panics", 0);
            boot_diag_nvs_decision nd = boot_diag_nvs_decide(
                sig_len > 0 ? stored_sig : nullptr, BOOT_DIAG_BUILD_SIG,
                stored_panics, is_panic);
            panics_since_flash = nd.panics_since_flash;
            if (nd.sig_changed) {
                prefs.putString("sig", BOOT_DIAG_BUILD_SIG);
            }
            if (nd.write_needed) {
                prefs.putULong("panics", panics_since_flash);
            }
            // poweron_count: button presses and real power-cycles combined — both
            // report ESP_RST_POWERON and cannot be told apart. Written only when a
            // poweron actually happened, so normal boots leave flash untouched.
            poweron_count = prefs.getULong("poweron", 0);
            if (reset_reason == ESP_RST_POWERON) {
                poweron_count++;
                prefs.putULong("poweron", poweron_count);
            }
            prefs.end();
        }
    }
    Serial.printf("[boot] reset: %s, boots since panic: %lu, panics: %lu, "
                  "panics since flash: %lu, powerons: %lu\n",
                  reset_reason_str(reset_reason),
                  (unsigned long)boots_since_panic,
                  (unsigned long)panic_count,
                  (unsigned long)panics_since_flash,
                  (unsigned long)poweron_count);
}

// ===== Storage bring-up =====
//
// Mount WITHOUT format-on-fail first. The device once arrived with foreign
// firmware that had repartitioned flash; the old format-on-fail mount then
// formatted the 8 MB partition silently — dark panel, mute serial,
// indistinguishable from a brick for the whole format. Now the format is
// announced on serial and on the panel (which is initialized before storage
// exactly for this), and a format failure degrades instead of hanging: every
// consumer already treats a failed open/read as "file missing" and falls back
// to defaults.
static void storage_begin() {
    if (SPIFFS.begin(false)) {
        storage_ok = true;
        return;
    }
    Serial.println("[boot] storage invalid, formatting (up to ~60 s)...");
    hw_ui_boot_note("FORMATTING STORAGE", "takes up to a minute");
    // The paint above released the SPI bus lock before returning; the format
    // below runs on internal flash and must never hold the shared bus.
    bool ok = SPIFFS.format() && SPIFFS.begin(false);
    storage_ok = ok;
    if (ok) {
        Serial.println("[boot] storage formatted, starting with defaults");
        hw_ui_boot_note("STORAGE READY", "settings reset to defaults");
    } else {
        Serial.println("[boot] STORAGE FAILED, continuing without saved settings");
        hw_ui_boot_note("STORAGE FAILED", "running without saved settings");
    }
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
/* Node identifier ("seed-<mac suffix>"). A label only — it is shown on the
 * INFO screen, on the clock face and in /capabilities so a node can be told
 * apart in a fleet. It is NOT resolvable: mDNS is off (see loop()), and
 * nothing registers it with a DNS server, so reach the node by IP. */
static String node_name = "";
static unsigned long boot_time = 0;

/* Active credentials. Fixed buffers, not Arduino Strings: they are written on
 * the AsyncTCP task (wifi_nets_set_active via the /wifi handlers) and read on
 * the loop task (WiFi.begin), and a String reallocating under the reader
 * dangles its c_str(). A torn text is the worst a fixed buffer can suffer —
 * one failed join, which the retry ladder repairs. Sizes match WifiNet. */
static char wifi_ssid[33] = "";
static char wifi_pass[65] = "";
static bool wifi_user_off = false;
/* Background retry backoff. After an association loss (or a failed attempt)
 * the next try comes quickly — a router reboot or a walk back into range is
 * usually over in seconds — then the interval stretches until it reaches the
 * 20-minute steady state, so a genuinely absent AP does not keep waking the
 * radio. WIFI_RETRY_MS stays the steady-state cap (and the last rung). */
#define WIFI_RETRY_MS 1200000UL
static const unsigned long WIFI_RETRY_LADDER_MS[] = {
    5000UL,         /* 5 s — walk back into the AP, try now */
    15000UL,        /* 15 s */
    30000UL,        /* 30 s */
    120000UL,       /* 2 min */
    300000UL,       /* 5 min */
    WIFI_RETRY_MS,  /* 20 min steady state */
};
#define WIFI_RETRY_LADDER_STEPS \
    ((int)(sizeof(WIFI_RETRY_LADDER_MS) / sizeof(WIFI_RETRY_LADDER_MS[0])))
static int wifi_retry_step = 0;
static unsigned long wifi_last_attempt_ms = 0;

static unsigned long wifi_retry_interval_ms() {
    return WIFI_RETRY_LADDER_MS[wifi_retry_step];
}

/* Multi-profile STA: try each known network in order on boot / reconnect. */
#define WIFI_MAX_NETS 6
static_assert(WIFI_MAX_NETS == WIFI_PROFILE_MAX, "Wi-Fi profile cap drift");
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
    /* fopen aborts under low internal DRAM (lock_init_generic) — never open. */
    if (!fs_internal_heap_ok()) return "";
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
    if (!fs_internal_heap_ok()) return false;
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

#define OUTBOX_FILE "/outbox.bin"
#define OUTBOX_TMP  "/outbox.tmp"
static OutboxStore g_outbox;
static uint8_t g_outbox_snapshot[OUTBOX_SNAPSHOT_MAX];

static bool outbox_persist() {
    size_t len = outbox_encode(&g_outbox, g_outbox_snapshot,
                               sizeof(g_outbox_snapshot));
    if (!len) return false;
    String body((const char *)g_outbox_snapshot, (unsigned int)len);
    return write_spiffs_file_atomic(OUTBOX_FILE, OUTBOX_TMP, body);
}

static void outbox_load() {
    outbox_init(&g_outbox);
    String body = read_spiffs_file(OUTBOX_FILE);
    if (!body.length()) return;
    if (!outbox_decode(&g_outbox, (const uint8_t *)body.c_str(), body.length())) {
        outbox_init(&g_outbox);
        event_add("outbox snapshot invalid");
    }
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
    // NVS-first: the auth (OTA) token must be STABLE across a SPIFFS format.
    // The old path re-read only the file and esp_random()-minted a new token
    // whenever the file was gone — every format rotated it and locked out WiFi
    // flashing. NVS is the source of truth after migration; the file is only a
    // pre-migration fallback.
    uint8_t buf[SECRET_VALUE_MAX];
    size_t n = secret_store_get("auth_tok", buf, sizeof(buf));
    if (n > 0) {
        auth_token = String();
        auth_token.reserve(n);
        for (size_t i = 0; i < n; i++) auth_token += (char)buf[i];
    } else {
        auth_token = read_spiffs_file(TOKEN_FILE);
    }
    auth_token.trim();

    if (auth_token.length() == 0) {
        char hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(hex + i * 2, 3, "%02x", (uint8_t)esp_random());
        }
        hex[32] = '\0';
        auth_token = String(hex);
        // Persist to NVS (survives a format) AND the file (back-compat).
        secret_store_put("auth_tok", (const uint8_t *)auth_token.c_str(),
                         auth_token.length());
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
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", wifi_nets[idx].ssid);
    snprintf(wifi_pass, sizeof(wifi_pass), "%s", wifi_nets[idx].pass);
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
    WifiProfileSet stored = {};
    stored.count = (uint8_t)wifi_net_count;
    stored.active = wifi_net_count ? (uint8_t)wifi_net_idx : 0;
    for (int i = 0; i < wifi_net_count; i++) {
        snprintf(stored.entries[i].ssid, sizeof(stored.entries[i].ssid),
                 "%s", wifi_nets[i].ssid);
        snprintf(stored.entries[i].pass, sizeof(stored.entries[i].pass),
                 "%s", wifi_nets[i].pass);
    }
    bool nvs_ok = wifi_profile_nvs_save(&stored);

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
    // Coalesce: loop() persists on every connect transition, which on a normal
    // boot rewrites byte-identical content a few seconds in — exactly when
    // WiFi and the mesh radio bring-up keep both cores busy. A flash write
    // stalls the other core via a tiny fixed-stack IPC task, so skip the
    // write entirely when nothing changed.
    bool file_ok = read_spiffs_file(WIFI_CONFIG_FILE) == json ||
                   write_spiffs_file(WIFI_CONFIG_FILE, json);
    return nvs_ok || file_ok;  // legacy file is a failure-safe fallback only
}

static bool wifi_load_legacy_config() {
    wifi_nets_clear();
    String json = read_spiffs_file(WIFI_CONFIG_FILE);
    if (json.length() == 0) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

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
    return wifi_net_count > 0;
}

static void wifi_load_config() {
    wifi_nets_clear();
    WifiProfileSet stored = {};
    if (wifi_profile_nvs_load(&stored)) {
        for (uint8_t i = 0; i < stored.count; i++) {
            snprintf(wifi_nets[i].ssid, sizeof(wifi_nets[i].ssid),
                     "%s", stored.entries[i].ssid);
            snprintf(wifi_nets[i].pass, sizeof(wifi_nets[i].pass),
                     "%s", stored.entries[i].pass);
        }
        wifi_net_count = stored.count;
        if (wifi_net_count) wifi_nets_set_active(stored.active);
        return;
    }
    /* First NVS boot, or both CRC-protected slots damaged: keep the legacy
       profiles and retry migration. A SPIFFS format cannot affect a valid NVS
       slot, so this fallback is needed only before migration or after damage. */
    if (wifi_load_legacy_config()) wifi_persist_profiles();
}

static bool wifi_save_config(const String &ssid, const String &pass) {
    wifi_nets_upsert(ssid.c_str(), pass.c_str());
    return wifi_persist_profiles();
}

/* Every connection attempt goes through one owner so a manual connect cannot
 * be followed by the periodic retry on the next loop pass. */
static void wifi_begin_active_profile() {
    wifi_last_attempt_ms = millis();
    WiFi.begin(wifi_ssid, wifi_pass);
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

/* Manual TOGGLE WIFI used to call disconnect(wifioff)+mode(OFF) on the same
 * click as the net screen, while WireGuard and AsyncTCP still owned the STA
 * netif. That tears lwIP down under live sockets and reboots the S3. Same
 * settle machine as reconnect, in reverse: stop WG, drop the association,
 * then power the RF off. */
enum {
    WIFI_OFF_IDLE = 0,
    WIFI_OFF_STOP_WG,
    WIFI_OFF_DISC,
    WIFI_OFF_MODE
};
#define WIFI_OFF_WG_SETTLE_MS   100UL
#define WIFI_OFF_DISC_SETTLE_MS 200UL
static volatile int wifi_off_state = WIFI_OFF_IDLE;
static unsigned long wifi_off_step_ms = 0;

static void wifi_off_request() {
    wifi_reconnect_state = WIFI_RECONNECT_IDLE;
    if (wifi_off_state == WIFI_OFF_IDLE)
        wifi_off_state = WIFI_OFF_STOP_WG;
}

static void wifi_off_poll();

static void wifi_reconnect_request() {
    /* User intent (config apply / manual connect) starts a fresh ladder:
     * without this, a saturated step would leave the next background retry
     * 20 minutes away if this attempt fails on the wrong profile. */
    wifi_retry_step = 0;
    wifi_off_state = WIFI_OFF_IDLE;
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
    /* Keep the WiFi driver's own credential store in RAM. Credentials live in
     * SPIFFS /wifi.json and every connect passes them explicitly, so the NVS
     * copy the driver writes on each connect is pure redundancy — and those
     * writes run on the WiFi task (pinned to core 0), where every flash
     * program/erase stalls core 1 through the ipc1 task and its fixed
     * 1024-byte stack (prebuilt core, not tunable). ipc1 is exactly the task
     * that blew its stack canary once, right in the connect window; removing
     * the recurring core-0 flash writes removes the prime suspect. Must be
     * set before the first WiFi.mode() call to reach wifiLowLevelInit(). */
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    /* Disable modem-sleep power save. The S3 default (WIFI_PS_MIN_MODEM) parks
     * the radio between DTIM beacons; under the RNS/mesh/display load on core 1
     * the association is not serviced in time and the AP drops us on a ~20 s
     * cadence. WIFI_PS_NONE keeps the receiver awake — the standard fix for
     * exactly this "WiFi keeps falling off" symptom on ESP32. */
    WiFi.setSleep(false);
    /* Arduino-ESP32 defaults this to true and otherwise retries underneath our
     * scheduler, causing the same UI stalls the 20-minute cadence avoids. */
    WiFi.setAutoReconnect(false);
    String suffix = get_mac_suffix();
    node_name = "seed-" + suffix;
    node_name.toLowerCase();

    // Start SNTP with the stored TZ before associating: the daemon is
    // non-blocking and keeps retrying on its own, so the clock also syncs after
    // a later reconnect (loop() re-begins WiFi) instead of only on a successful
    // boot-time connect.
    configTzTime(tz_string.c_str(), "pool.ntp.org", "time.nist.gov");

    wifi_load_config();
    if (wifi_net_count > 0 || wifi_ssid[0]) {
        // Boot must never wait for infrastructure. Start one asynchronous STA
        // attempt; loop() rotates profiles later while UI and MeshCore are live.
        Serial.printf("[wifi] boot async try %s\n", wifi_ssid);
        wifi_begin_active_profile();
    } else {
        Serial.println("[wifi] no credentials — continuing mesh-only");
    }
}

// ===== HTTP Handlers =====

// History archive health getters. Defined in skills/history.cpp, which is
// #included far below this handler, so they are forward-declared here (same TU,
// static). All three are cheap and AsyncTCP-safe: history_on_sd() is a lock-free
// read of a boot-latched flag; history_drops() reads a plain writer-side counter;
// history_index_live_count() takes ONLY g_hist_mux (no SPI bus) for a brief RAM
// index read — no bus lock is taken on the /health handler (no history_bytes),
// so no mux->bus order and no new deadlock hazard on this task.
static bool     history_on_sd(void);
static uint32_t history_drops(void);
static uint32_t history_index_live_count(void);
// history_ready() is a lock-free read of the boot-latched write-queue handle
// (false => the queue alloc failed and NOTHING persists); history_queued() is a
// lock-free uxQueueMessagesWaiting read of the queue occupancy (early overflow
// warning). Both are AsyncTCP-safe: no g_hist_mux, no SPI bus.
static bool     history_ready(void);
static uint32_t history_queued(void);

static void handle_health(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["uptime_sec"] = (millis() - boot_time) / 1000;
    doc["type"] = "esp32-seed";
    doc["version"] = SEED_VERSION;
    doc["seed"] = true;
    doc["arch"] = "xtensa-esp32s3";
    // Panic visibility: why the last reset happened and how long the board
    // has stayed clean since. Counters live in RTC_NOINIT (see boot_diag_init).
    doc["reset_reason"] = reset_reason_str(reset_reason);
    doc["boots_since_panic"] = (unsigned long)boots_since_panic;
    doc["panic_count"] = (unsigned long)panic_count;
    // panics_since_flash lives in NVS (flash), so it survives a manual reset —
    // unlike panic_count/boots_since_panic (RTC), which the reset button wipes
    // (it reports ESP_RST_POWERON). It is cleared only by a reflash. poweron_count
    // is the combined button+power tally.
    doc["panics_since_flash"] = (unsigned long)panics_since_flash;
    doc["poweron_count"] = (unsigned long)poweron_count;
    doc["storage_ok"] = storage_ok;
    // History archive observability (ticket C4, read-only): is the archive on the
    // SD card or the SPIFFS fallback, how many records have been dropped on a full
    // write queue, and how many identities the RAM index knows. history_records is
    // INDEXED IDENTITIES (the bounded MRU nav window), not the whole-archive total
    // — an honest count with no SD scan on this handler. No store selector, no
    // behaviour change: this only surfaces state C1-C3 already track.
    doc["history_sd"] = history_on_sd();
    doc["history_drops"] = (unsigned long)history_drops();
    doc["history_records"] = (unsigned long)history_index_live_count();
    // Queue health: history_ready=false means the write queue failed to allocate
    // at boot and every enqueue drops (a dead queue is otherwise invisible while
    // history_drops climbs); history_queued is live occupancy, an early warning
    // that fills before drops start.
    doc["history_ready"] = history_ready();
    doc["history_queued"] = (unsigned long)history_queued();
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
    /* Identifier, not an address: mDNS is off on this board, so this name
     * resolves nowhere. Per the capabilities spec the field is a "node
     * hostname or identifier"; talk to the node on wifi_ip below. */
    doc["hostname"] = node_name;

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

// Collects a request body into a single NUL-terminated heap buffer for every
// handler registered with it (POST /config.md, /clock/tz, /wifi/networks,
// /gw/token, and the skills' JSON POSTs: /notify, /notify/ack, /wg/*,
// /agents/*, /backlight, /progress, /mesh/inject, /gps/fix). Runs on the
// AsyncTCP task, once per body chunk, BEFORE the
// route handler — and therefore before any auth check on those routes.
//
// total is the declared body length. It is authoritative here and MUST be, for
// two reasons handle_firmware_upload_body() below already learned:
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
        if (total > HTTP_BODY_MAX) { request->send(413, "application/json", "{\"error\":\"body too large\"}"); return; }
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
    s += "Name: " + node_name + " (identifier; mDNS is off, reach it by IP)\n";
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
    s += "| GET | /health | Alive (no auth); reset_reason, boots_since_panic, panic_count, panics_since_flash, poweron_count, storage_ok |\n";
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
    s += "| POST | /gw/token | Set gateway capability token (raw body or JSON string token; empty body clears) |\n";
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
    if (WiFi.status() == WL_CONNECTED) doc["ssid"] = WiFi.SSID();
    else doc["ssid"] = wifi_ssid;
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
    if (wifi_user_off) wifi_off_request();
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
    event_add("wifi profiles=%d primary=%s", wifi_net_count, wifi_ssid);

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
#include "inbox_view.h"   // the inbox list model: order, transport glyph, unread
#include "feed_view.h"    // the unified Messages feed: cards + chats merged by time
#include "contacts_view.h"      // the grouped Contacts model: AI / LXMF / mesh rows
#include "net_view.h"           // the sectioned Network-status model (pure)
static_assert(HW_UI_NET_MAX == NET_ROWS_MAX, "net status row cap drift");
#include "mesh/contacts_enum.h" // /contacts3 enumeration for the mesh bucket
#include "skills/notify.cpp"
// After notify: reuses notify_send_json / notify_send_error / notify_ingest_p1.
#include "skills/progress.cpp"
#include "skills/agents.cpp"
#include "skills/history.cpp"  // append-only SD history archive + off-loop write queue
#include "skills/meshcore.cpp"
#include "skills/backlight.cpp"
#include "skills/wg.cpp"
#include "skills/gps.cpp"    // after notify: reuses notify_send_json; uses hw_ui_expand_ok
#include "skills/rns.cpp"    // Reticulum bring-up; uses write_spiffs_file_atomic

// ===== Connection coordinator =====
// Pure policy in src/conn_mgr.h; this is the thin device half that reads the
// real transport accessors, runs one coordinator step, and dispatches the
// returned actions to the EXISTING recover entrypoints (never re-implementing
// them). Placed here, after every skill translation unit is included, so the
// per-transport statics (wg_is_up, g_rns_*, g_mesh, ...) are in scope.
//
// Called once per loop() pass, throttled to CONN_MGR_TICK_MS. On the happy path
// (every wanted layer up) conn_mgr_step() returns all-false and this writes
// nothing — no behaviour change when the network is healthy.
#include "conn_mgr.h"
static ConnMgrState g_conn_mgr;

// Fast "I'm back, flush my queued cards" beacon on the WiFi attach edge
// (ticket CARD-DELIVERY / C3). Pure decision + wire in src/attach_beacon.h; the
// attach edge only STAGES it here, the blocking POST is deferred to
// flush_beacon_poll() on the loop task (mirrors reply_upstream_poll / card_ack).
#include "attach_beacon.h"
static AttachBeaconState g_attach_beacon;

// Cached route-home reachability (ticket HERMES-LADDER / C1, foundation only).
// Pure cadence + staleness policy in src/reachability.h; the bounded GET that
// feeds it lives in reachability_service(), defined below once mesh_gw_url and
// gw_token are in scope. Driven here on the loop task at the conn-mgr cadence so
// the verdict is always warm when the send ladder (C2) asks for it — the send
// path never blocks on a probe.
#include "reachability.h"
static ReachState g_reach;
static void reachability_service(uint32_t now);
// Defined below (after mesh_gw_url/gw_token); the send ladder in the
// #include'd skills/agents.cpp — which expands ABOVE this point — is registered
// with it in skills_init() via agents_set_reachability().
static ReachStatus gateway_reachable();

static void conn_mgr_service() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (last != 0 && (now - last) < CONN_MGR_TICK_MS) return;
    last = now ? now : 1;

    ConnMgrInputs in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.wifi_wanted = !wifi_user_off && (wifi_net_count > 0 || wifi_ssid[0]);
    in.wifi_connected = (WiFi.status() == WL_CONNECTED);
    in.time_valid = (time(NULL) > TIME_VALID_EPOCH);
    in.wg_wanted = g_wg_want && g_wg_cfg_ok;
    in.wg_up = wg_is_up();
    in.rns_wanted = g_rns_cfg_enabled && g_rns_cfg_ok;
    in.rns_up = (g_rns_cs.load() == RNS_CS_CONNECTED);
    in.mesh_wanted = g_mesh.has_identity && g_mesh.radio_ready &&
                     g_mesh.heltec_pk_hex[0] != '\0';
    in.mesh_up = (g_mesh.fail_streak < MESH_FAIL_DOWN);

    ConnMgrActions a = conn_mgr_step(&g_conn_mgr, &in);

    if (a.on_attach) {
        outbox_nudge_pending(&g_outbox);
        event_add("conn: network attached");
    }
    if (a.on_lost) event_add("conn: network lost");

    // Stage a flush beacon on a genuine attach (rate-limited against flaps). The
    // POST itself is deferred to flush_beacon_poll() — nothing blocking here.
    if (attach_beacon_decide(&g_attach_beacon, a.on_attach, now))
        event_add("conn: flush beacon staged");

    // WireGuard starts itself from skill_wg_poll, like it did before the
    // coordinator existed. Mapping start/restart onto g_wg_restart_req
    // STOPs the tunnel and begin()s again on this task — that froze the menu.
    (void)a.start_wg;
    (void)a.restart_wg;

    // RNS: rearm the connect retry at its minimum backoff so the interface's
    // own loop() picks it up on the next tick. No socket work here.
    if (a.nudge_rns) {
        g_rns_backoff_ms = RNS_TCP_BACKOFF_MIN_MS;
        g_rns_next_try_ms = millis();
        event_add("conn: nudge rns reconnect");
    }

    // mesh: clear the last-probe stamp so skill_meshcore_poll() fires a probe.
    if (a.nudge_mesh) {
        g_mesh.last_probe_ms = 0;
        event_add("conn: nudge mesh probe");
    }

    // WiFi wedge escape: restart the STA ladder from its fastest rung. The
    // threshold is past the ladder's own 20 min top rung, so this only fires if
    // the ladder itself is stuck.
    if (a.nudge_wifi) {
        wifi_reconnect_request();
        event_add("conn: nudge wifi ladder");
    }

    // Keep the cached route-home verdict warm for the send ladder (C2). Shares
    // this loop-task cadence; the probe itself is bounded and self-throttled by
    // reach_should_probe(), so most ticks are a no-op.
    reachability_service(now);
}

/* Defined with the other UI helpers below; the store needs it as soon as it
 * starts counting unread, which is from the first message it stores. */
static bool ui_conv_on_screen(int idx);
static bool notify_event_distinct_cb(const char *source, const char *key);

static void skills_init() {
    skill_notify_init();
    skill_progress_init();
    skill_agents_init();
    notify_set_event_distinct_fn(notify_event_distinct_cb);
    /* C2: the send ladder picks the WiFi rung only when the route home is PROVEN.
     * Hand it the cached C1 verdict; the probe stays off the send path. */
    agents_set_reachability(gateway_reachable);
    /* Only the UI knows which conversation is being read; the store must not
     * infer it from window ownership, which outlives the screen. */
    agents_set_on_screen_hook(ui_conv_on_screen);
    skill_meshcore_init();
    skill_backlight_init();
    skill_wg_init();
    skill_gps_init();
    skill_rns_init();
}

// Build one second of the home clock face (tembed look, 480x222).
// Lives after notify.cpp so it can read unread/crit without a second copy.
static bool settings_silent = false;
static uint16_t settings_autolock_s = 0;

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
        /* Plain node name, no ".local" — mDNS is off, nothing resolves it. */
        snprintf(row1r, sizeof(row1r), "%s", node_name.c_str());
    } else {
        snprintf(addr, sizeof(addr), "offline");
        snprintf(row1l, sizeof(row1l), "WiFi offline");
        snprintf(row2, sizeof(row2), "mesh ready - STA retry async");
    }
    if (hw_kb_locked()) {
        /* LOCK takes the note row so pocket mode is obvious. */
        snprintf(row2, sizeof(row2), "LOCKED  hold USER unlock");
    }
    if (settings_silent && row2[0] == '\0')
        snprintf(row2, sizeof(row2), "SILENT");
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
                     mesh_alive_age_s());
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
    /* MESSAGES is now the single unified feed: notification cards and chat
     * conversations merged by time. The old separate AGENTS entry is retired —
     * chats are reachable straight from here (src/feed_view.h). */
    MENU_MESSAGES = 0,
    MENU_MESHCORE,
    MENU_WIFI,
    MENU_SETTINGS,
    MENU_INFO,
    MENU_CONTACTS,
    MENU_BACK,
    MENU_COUNT
};
// Card action sheet (click / Enter on a notification). DELETE removes the card
// from the feed for good (RAM ring + an archive tombstone); it survives reboot.
enum { CARD_ACT_ACK = 0, CARD_ACT_REPLY, CARD_ACT_DELETE, CARD_ACT_BACK, CARD_ACT_COUNT };
// In-chat menu (click while in agent chat room). DELETE removes the whole
// conversation (history + manifest + slot); CLEAR only empties the active room.
// DELETE is offered only for non-seeded conversations (see agent_act_del_ok).
enum { AGENT_ACT_CLEAR = 0, AGENT_ACT_DELETE, AGENT_ACT_BACK, AGENT_ACT_COUNT };
// Layout picker: 0=ABC 1=PHON 2=RU 3=BACK
enum { LAYOUT_BACK = 3, LAYOUT_LIST_COUNT = 4 };
// SETTINGS: LAYOUT / BACKLIGHT / AUTO-DIM / SILENT / AUTOLOCK / BACK
enum {
    SETTINGS_LAYOUT = 0,
    SETTINGS_BACKLIGHT = 1,
    SETTINGS_AUTODIM = 2,
    SETTINGS_SILENT = 3,
    SETTINGS_AUTOLOCK = 4,
    SETTINGS_BACK = 5,
    SETTINGS_LIST_COUNT = 6
};
// MeshCore menu: 0=STATUS 1=PING 2=BACK
enum { MESH_ACT_STATUS = 0, MESH_ACT_PING = 1, MESH_ACT_BACK = 2, MESH_LIST_COUNT = 3 };
// WiFi menu: STATUS / SCAN / PROFILES / HIDDEN SSID / TOGGLE / BACK
enum {
    WIFI_ACT_STATUS = 0,
    WIFI_ACT_SCAN = 1,
    WIFI_ACT_PROFILES = 2,
    WIFI_ACT_HIDDEN = 3,
    WIFI_ACT_TOGGLE = 4,
    WIFI_ACT_BACK = 5,
    WIFI_LIST_COUNT = 6
};
// WiFi list mode (scan results vs saved profiles)
enum { WIFI_LIST_SCAN = 0, WIFI_LIST_PROFILES = 1 };
static int menu_sel = 0;
static int card_act_sel = 0;
static int agent_act_sel = 0;
// The in-chat sheet drops the DELETE row for a seeded conversation (claude /
// hermes): those doors are re-created from firmware every boot, so a delete
// could never stick. Set when the sheet opens; drives its row count + mapping.
static bool agent_act_del_ok = false;
static int layout_sel = 0;
static int settings_sel = 0;
static int mesh_sel = 0;
static int wifi_sel = 0;
// User turned WiFi off from the menu: suppress the loop() auto-reconnect until
// they toggle it back on (garden mesh-only testing).
static int wifi_list_sel = 0;
static int wifi_list_mode = WIFI_LIST_SCAN;
static int wifi_list_count = 0;
static char wifi_list_titles[HW_UI_WIFI_LIST_MAX][48];
static uint8_t net_origin = UINAV_WIFI;
static char wifi_list_ssids[HW_UI_WIFI_LIST_MAX][33];
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
// Fixed buffer, mirroring mesh_gw_url: written on the AsyncTCP task
// (POST /gw/token), read on the loop task (/ping, reply). An Arduino String
// here can reallocate under the reader and dangle its c_str(); a torn char
// buffer costs at worst one 401, which the caller can simply retry.
static char gw_token[GW_TOKEN_MAX + 1] = "";

// ---- Route-home reachability, device half (HERMES-LADDER / C1) --------------
// Runs on the loop task from conn_mgr_service(). Keeps g_reach current so the
// send ladder (C2) can read gateway_reachable() without ever blocking on a
// network call. The probe is a single bounded GET {mesh_gw_url}/ping, gated by
// the pure two-speed cadence in reachability.h; a black-holed gateway costs
// ~1 s (connect cap) at most and only at the probe cadence, never per send.
static void reachability_service(uint32_t now) {
    /* NO HTTP probe. Live serial 2026-08-14: REACH_DOWN_INTERVAL_MS=3s meant
     * NetworkClient.connect every 3 s → socket:105 forever → HTTP server dead.
     * Association is NOT a route home: treating WL_CONNECTED as REACH_UP made
     * the send ladder POST the bridge on a zombie STA and stall the UI (~1.7s)
     * instead of falling to mesh. Record DOWN when the link is gone; while
     * associated, leave the cache alone so it stays UNKNOWN/DOWN and the
     * ladder skips WiFi. */
    if (WiFi.status() != WL_CONNECTED)
        reach_record(&g_reach, now, false);
}

// Accessor for the send ladder (C2): the cached route-home verdict, no I/O.
// UNKNOWN until the first probe (or once a past result goes stale), UP when the
// gateway answered recently, DOWN on a recent failure or a down WiFi link.
// Registered with the agent-send ladder in skills_init() (agents_set_reachability);
// transport_send_agent() reads it to gate the WiFi rung on a PROVEN route home.
static ReachStatus gateway_reachable() {
    return reach_status(&g_reach, millis(), REACH_STALE_MS);
}

static int agent_focus = 0;   // which agent chat is open (0..AGENTS_N-1)
enum ReplyMode : uint8_t {
    REPLY_MODE_NONE = 0,
    REPLY_MODE_NOTIFY,
    REPLY_MODE_AGENT,
    REPLY_MODE_WIFI_SSID,
    REPLY_MODE_WIFI_PASSWORD,
};
static ReplyMode reply_mode = REPLY_MODE_NONE;
static int agent_scroll = -1;       // visual row; -1 = pin to latest
static int agent_scroll_total = 0;  // last known wrapped row count
static int agent_sess_sel = 0;      // selected row on the agent SESSIONS list
// Where the agent flow was entered from, so the session picker backs out to the
// list that opened it (the unified feed or the contacts list) rather than always
// the feed. Set at the forward entry points and left untouched by the internal
// chat<->picker navigation, so it survives a round trip through a room.
static uint8_t agent_origin = UINAV_MSGLIST;
// Chat window copy: only the current viewport (AGENT_THREAD_MAX lines) — the
// full history lives on SD, so no whole-thread RAM snapshot is held.
/* Parked in PSRAM (~12 KB, the largest single buffer left in DRAM): filled by
 * ui_agent_chat_refresh under agents_lock and read by the painter, both on the
 * UI/loop task — never an ISR. Allocated on first refresh; a null leaves the
 * chat window empty instead of crashing. */
static char (*ag_chat_lines)[AGENT_TEXT_LEN] = nullptr;
static bool ag_chat_from_me[AGENT_THREAD_MAX];
static uint8_t ag_chat_deliv[AGENT_THREAD_MAX];
static uint32_t ag_chat_ts[AGENT_THREAD_MAX];
static const char *ag_chat_ptrs[AGENT_THREAD_MAX];
/* The store's "is the user reading this?" test. The chat screen being up on
 * this conversation is the only thing that counts as reading it. */
static bool ui_conv_on_screen(int idx) {
    return hw_ui_screen() == HW_UI_AGENT_CHAT && agent_focus == idx;
}

// Session-list screen rows: N sessions + "NEW SESSION" + "BACK".
// ag_sess_msgs[i] < 0 ⇒ no message-count badge on that row.
static char ag_sess_titles[AGENT_SESSIONS_MAX + 2][AGENT_SESSION_LEN + 2];
static int  ag_sess_msgs[AGENT_SESSIONS_MAX + 2];
static bool ag_sess_active[AGENT_SESSIONS_MAX + 2];
static const char *ag_sess_ptrs[AGENT_SESSIONS_MAX + 2];
// Live-room roster: dead sessions are hidden from the picker, so a display row
// is not the raw session index. ag_sess_row2idx maps the row shown to the real
// session index; ag_sess_vis_n is the count of visible (non-dead) rows drawn.
static int ag_sess_row2idx[AGENT_SESSIONS_MAX];
static int ag_sess_vis_n = 0;
static int msglist_sel = 0;
// Tagged handle for one unified-feed row: the Messages feed mixes notification
// cards and chat conversations, so a row opens by one of two handles. A card
// row carries its notify id; a conversation row carries its table slot and the
// id it displayed (revalidated before opening, so a recycled slot cannot drop
// the user into a stranger's thread — see the HW_UI_MSGLIST click handler).
struct MsgHandle {
    bool     is_conv;                 // false = notify card, true = conversation
    uint32_t card_id;                 // card handle (is_conv == false)
    uint8_t  slot;                    // conversation slot (is_conv == true)
    bool     rooms;                   // conv opens a room picker vs a chat
    char     conv_id[CONV_ID_LEN];    // what the conv row displayed
};
static MsgHandle msglist_h[HW_UI_MSGLIST_MAX];
static int msglist_count = 0;
static_assert(HW_UI_MSGLIST_MAX == FEED_ORDER_MAX,
              "the unified feed must expose every merged row");
// Notify card currently shown (for ack-on-click / reply).
static uint32_t notify_card_id = 0;
static char reply_title[NOTIFY_TITLE_LEN];

// --- Micron system-layer page store (ticket C4) -----------------------------
// Eight-slot store of micron pages the wheel flips through from the home screen.
// The store is PURE data; the clock is read here (loop task) and injected as
// now_ms — the store never reads a clock itself. The namespace is set by the
// PRODUCER (our own trusted boot code for a system page; an untrusted browser
// transport would pass MICRON_NS_FOREIGN), never parsed from a page body.
// Parked in PSRAM (~4.5 KB, 8 x micron_slot): allocated once by
// ui_page_store_begin(), touched only from the loop/AsyncTCP task context.
static micron_store *g_page_store;
// Wheel-paging view state. Two distinct input modes, never conflated:
//   page_open == false → PAGING between pages: a wheel detent changes which
//                        system page is shown (ordinal); a click OPENS it.
//   page_open == true  → SCROLLING within the open page: a wheel detent drives
//                        C3's in-page scroll; a click closes back to paging.
static int    page_ordinal = -1;    // which system page (by ns order), -1 = none
static int    page_scroll = 0;      // top-visible visual row inside the open page
static size_t page_total_rows = 0;  // last render's bounded page height (scroll clamp)
static bool   page_open = false;

// Store a page in the RAM hot-set and, when the insert DISPLACES a live slot,
// FLUSH the displaced page to the SD archive so it is not lost — read-back
// (wheel-paging past the hot window) pulls it back later. The flush is a single
// off-loop history_enqueue(): the queue + write task own the SD append, so this
// NEVER writes SD on the loop task. The displaced page's full body travels in the
// put result (evicted_src/_len), captured before the slot was overwritten. The
// namespace travels with it (evicted_ns), so a FOREIGN page archives under
// FOREIGN and can never surface through a SYSTEM wheel walk. This is the single
// producer seam every page insert goes through (C3 adds notify-card producers).
static micron_put_result ui_page_put(uint8_t ns, const char *key,
                                     const char *src, size_t len, uint32_t ttl_ms) {
    if (!g_page_store) { micron_put_result z; memset(&z, 0, sizeof(z)); return z; }
    micron_put_result r = micron_store_put(g_page_store, ns, key, src, len,
                                           ttl_ms, millis());
    if (r.evicted) {
        history_enqueue(r.evicted_ns, r.evicted_key,
                        (const uint8_t *)r.evicted_src, r.evicted_len);
    }
    return r;
}

// Bring up the system-layer page store and seed our own data UI. The namespace
// (MICRON_NS_SYSTEM) is chosen HERE, by trusted firmware — it is the delivery
// account of a page WE produce, never a field any page body could set. The
// monotonic clock is read at this hw call site and injected as now_ms; the
// store itself never reads a clock. A later ticket wires the untrusted browser
// transport, which will store under MICRON_NS_FOREIGN from its own transport
// identity — and can never reach this namespace.
static void ui_page_store_begin() {
    // ~4.5 KB in PSRAM (internal-DRAM fallback). A null here means both pools are
    // exhausted; ui_page_put guards on it and the page UI degrades to empty.
    g_page_store = (micron_store *)psram_calloc_pref(sizeof(*g_page_store));
    if (!g_page_store) {
        Serial.println("[ui] page store alloc FAILED — page UI disabled");
        return;
    }
    micron_store_init(g_page_store);
    static const char kHelpPage[] =
        ">System pages\n"
        "\n"
        "Turn the wheel from the clock to flip through stored pages. "
        "Click to open one, then the wheel scrolls it.\n";
    ui_page_put(MICRON_NS_SYSTEM, "help", kHelpPage, sizeof(kHelpPage) - 1, 0);
}

// Scratch record for an archive page body pulled from SD. File-scope (564 B) to
// keep it off the loop task's stack; only ui_page_render touches it.
static history_record g_page_fetch;

// Render the `ordinal`-th SYSTEM page at page_scroll and remember its height for
// the in-page scroll clamp. The ordinal spans the combined nav axis: RAM hot-set
// first, then older archived pages (history_nav_page_at). A RAM page renders
// straight from its slot; an archive page's body is fetched from SD with ONE
// bounded record read (history_read_at) here on open/render — never a full scan.
// Namespace is fixed to MICRON_NS_SYSTEM, so paging can only ever surface system
// pages: a foreign page is structurally unreachable from this call site, through
// the RAM hot-set AND the archive. Returns false when there is no such page
// (empty store / out of range / archive body unreadable — graceful, no SD).
static bool ui_page_render(int ordinal) {
    if (!g_page_store) return false;
    history_nav_result nav = history_nav_page_at(g_page_store,
                                                 MICRON_NS_SYSTEM, ordinal);
    const char *src = NULL;
    size_t len = 0;
    if (nav.kind == HISTORY_NAV_RAM) {
        src = nav.slot->src;
        len = nav.slot->len;
    } else if (nav.kind == HISTORY_NAV_ARCHIVE) {
        if (!history_read_at(nav.offset, &g_page_fetch)) return false;
        src = (const char *)g_page_fetch.payload;
        len = g_page_fetch.len;
    } else {
        return false;   // out of range: paging stops at the last real page
    }
    page_ordinal = ordinal;
    page_total_rows = hw_ui_render_page(src, len, page_scroll);
    return true;
}

// Rebuild the notify RAM ring from the persistent history archive (ticket C3).
// Runs in setup() AFTER history_begin() (archive mounted + index seeded) and
// after skill_notify_init() memset the ring — single-threaded, before the web
// server and mesh start, so it fills the ring without the notify spinlock (same
// discipline as the old /notify.json restore). It walks the archive index for
// MICRON_NS_NOTIFY entries NEWEST-FIRST (history_restore_at), decodes each card
// body (notify_rec_decode), and inserts it (notify_restore_one, which dedups by
// id and clamps like a POST body). Namespace isolation is structural: only
// MICRON_NS_NOTIFY records are walked, so a SYSTEM/FOREIGN page can never enter
// the notify ring. Graceful no-SD: an empty/absent archive yields nothing on the
// first rank and the ring simply stays empty — notify still works in RAM.
static void notify_reconcile_restored_chats();
static bool notify_chat_retry_pending = false;
static unsigned long notify_chat_retry_at = 0;
struct NotifyChatInflight {
    bool active;
    char conversation[CONV_ID_LEN];
    NotifyEventId event;
};
static NotifyChatInflight notify_chat_inflight[CONV_MAX] = {};

static int notify_chat_inflight_find(const char *conversation) {
    if (!conversation || !conversation[0]) return -1;
    for (int i = 0; i < CONV_MAX; i++)
        if (notify_chat_inflight[i].active &&
            strcmp(notify_chat_inflight[i].conversation, conversation) == 0)
            return i;
    return -1;
}

static bool notify_chat_inflight_add(const char *conversation,
                                     const NotifyEventId *event) {
    if (!conversation || !conversation[0] || !notify_event_id_valid(event) ||
        notify_chat_inflight_find(conversation) >= 0) return false;
    size_t conversation_n = strnlen(conversation, CONV_ID_LEN);
    if (conversation_n >= CONV_ID_LEN) return false;
    for (int i = 0; i < CONV_MAX; i++) {
        if (notify_chat_inflight[i].active) continue;
        notify_chat_inflight[i].active = true;
        memcpy(notify_chat_inflight[i].conversation,
               conversation, conversation_n + 1);
        notify_chat_inflight[i].event = *event;
        return true;
    }
    return false;
}

static void notify_chat_inflight_remove(const char *conversation,
                                        const NotifyEventId *event) {
    int i = notify_chat_inflight_find(conversation);
    if (i >= 0 && notify_event_id_equal(&notify_chat_inflight[i].event, event))
        memset(&notify_chat_inflight[i], 0, sizeof(notify_chat_inflight[i]));
}
static void notify_restore_from_archive() {
    time_t now = time(NULL);
    unsigned long now_ms = millis();
    int restored = 0;
    static char deleted_keys[HISTORY_INDEX_MAX][NR_KEY_CAP];
    memset(deleted_keys, 0, sizeof(deleted_keys));
    int deleted_key_count = 0;
    /* Skipped tombstones, corrupt records, duplicate ids and legacy same-key
       event identities do not consume ring capacity. Scan the finite index
       window until it is exhausted or the ring is actually full. */
    for (int rank = 0;
         rank < HISTORY_INDEX_MAX && restored < NOTIFY_MAX;
         rank++) {
        history_record rec;
        if (!history_restore_at(MICRON_NS_NOTIFY, rank, &rec)) break;  // past the last
        // Old tombstones skip their exact archive identity. New keyed tombstones
        // also shadow older legacy identities carrying the same logical key.
        if (notify_rec_is_tombstone(rec.payload, rec.len)) {
            char key[NR_KEY_CAP] = {};
            if (deleted_key_count < HISTORY_INDEX_MAX &&
                notify_rec_tombstone_key(rec.payload, rec.len,
                                         key, sizeof(key))) {
                memcpy(deleted_keys[deleted_key_count++], key, sizeof(key));
            }
            continue;
        }
        notify_rec nr;
        if (!notify_rec_decode(rec.payload, rec.len, &nr)) continue;   // skip a bad body
        bool deleted = false;
        for (int i = 0; i < deleted_key_count; i++) {
            if (strcmp(nr.key, deleted_keys[i]) != 0) continue;
            deleted = true;
            break;
        }
        if (deleted) continue;
        if (notify_restore_one(&nr, now, now_ms)) restored++;
    }
    notify_restore_finish(restored);   // drop ttls that ran out while powered off
    notify_reconcile_restored_chats();
}

// UTF-8 draft: keep room for ~40 Cyrillic codepoints (2–3 bytes each).
static char reply_buf[192];
// Idle return to clock (Advisor: shelf is a clock). Longer while typing.
// Same timestamp drives backlight auto-dim.
static unsigned long ui_last_input_ms = 0;
#define UI_IDLE_MS        15000
#define UI_IDLE_REPLY_MS  60000
#define KB_LAYOUT_PATH    "/kb_layout.txt"
#define KB_LAYOUT_TMP     "/kb_layout.tmp"
#define SETTINGS_PATH     "/settings.json"
#define SETTINGS_TMP      "/settings.tmp"

static_assert(BL_IDLE_DIM_MS > UI_IDLE_MS,
              "backlight must not dim while a message card can still be on screen");

static void ui_note_input() { ui_last_input_ms = millis(); }

static void settings_save() {
    JsonDocument doc;
    doc["silent"] = settings_silent;
    doc["autolock_s"] = settings_autolock_s;
    String body;
    serializeJson(doc, body);
    write_spiffs_file_atomic(SETTINGS_PATH, SETTINGS_TMP, body);
}

static void settings_load() {
    String body = read_spiffs_file(SETTINGS_PATH);
    if (!body.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    if (doc["silent"].is<bool>()) settings_silent = doc["silent"].as<bool>();
    if (doc["autolock_s"].is<uint16_t>()) {
        uint16_t value = doc["autolock_s"].as<uint16_t>();
        if (settings_autolock_valid(value)) settings_autolock_s = value;
    }
}

static void settings_toggle_silent() {
    settings_silent = !settings_silent;
    settings_save();
    hw_haptic_notify(1);
}

static const char *settings_autolock_word() {
    if (settings_autolock_s == 30) return "30S";
    if (settings_autolock_s == 60) return "1M";
    if (settings_autolock_s == 300) return "5M";
    return "OFF";
}

// SYSTEM events only (a message arriving): wake the panel and restart the
// idle/dim countdown once per event; repaints and synthetic errors must not call this.
static void ui_note_wake() { ui_last_input_ms = millis(); }

// SETTINGS face: rebuild labels from backlight skill and paint.
static void ui_open_settings() {
    char bl[BL_LABEL_MAX];
    bl_level_label(backlight_wanted(), bl, sizeof(bl));
    hw_ui_show_settings(settings_sel, bl, bl_idle_word(), settings_silent,
                        settings_autolock_word());
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
    // NVS-first (source of truth after migration); the SPIFFS file is only a
    // pre-migration fallback. An absent token is legitimate (none provisioned),
    // and leaves gw_token empty exactly as before.
    String stored;
    uint8_t buf[SECRET_VALUE_MAX];
    size_t n = secret_store_get("gw_tok", buf, sizeof(buf));
    if (n > 0) {
        stored.reserve(n);
        for (size_t i = 0; i < n; i++) stored += (char)buf[i];
    } else {
        stored = read_spiffs_file(GW_TOKEN_PATH);
    }
    stored.trim();
    snprintf(gw_token, sizeof(gw_token), "%s", stored.c_str());
}

// Staged by POST /gw/token on the AsyncTCP task, drained here on the loop
// task. NVS (Preferences) and SPIFFS writes stay off AsyncTCP — the same
// deferral handle_wifi_post uses for the WiFi driver; the boot-time loaders
// are no precedent, they run before the server starts. One slot, newest wins:
// a second POST before the drain replaces the staged value, and the drain
// clears the flag BEFORE snapshotting, so a rotation landing mid-drain
// re-raises the flag and its full value is persisted on the next pass (at
// worst one torn intermediate persist, then convergence on the newest).
static volatile bool gw_token_persist_pending = false;
static char gw_token_stage[GW_TOKEN_MAX + 1] = "";

static void gw_token_persist_poll() {
    if (!gw_token_persist_pending) return;
    // Clear the flag BEFORE the snapshot. A writer landing mid-snapshot
    // re-raises it: this pass may persist a torn value once, the next pass
    // persists the full newest value — newest always wins. The reverse order
    // (snapshot, then clear) would clobber a raise that arrived between the
    // two and durably persist the OLDER token — the stale-wins bug class
    // this route exists to kill.
    gw_token_persist_pending = false;
    static char tok[GW_TOKEN_MAX + 1];   // static: stays off the loop stack
    snprintf(tok, sizeof(tok), "%s", gw_token_stage);
    if (tok[0]) {
        // Dual write, NVS first: gw_token_load() reads NVS as the source of
        // truth once the boot migration has sealed (token_load() precedent),
        // so a rotation that only rewrote the SPIFFS file was shadowed by the
        // stale NVS value on the next boot — a permanent 401 against the
        // gateway. The file write stays for pre-migration back-compat.
        bool nvs_ok = secret_store_put("gw_tok", (const uint8_t *)tok,
                                       strlen(tok));
        bool fs_ok = write_spiffs_file_atomic(GW_TOKEN_PATH, GW_TOKEN_TMP,
                                              String(tok));
        if (!nvs_ok || !fs_ok)
            event_add("gateway token persist failed (nvs=%d fs=%d)",
                      (int)nvs_ok, (int)fs_ok);
    } else {
        // Clear must erase BOTH stores: removing only the SPIFFS file leaves
        // the NVS copy to resurrect the cleared token on the next boot — the
        // same failure the set path logs, so this half logs it too.
        bool nvs_ok = secret_store_del("gw_tok");
        // SPIFFS.remove() returns false for an already-absent file; absent is
        // the state a clear asks for, not a failure.
        bool fs_ok = !SPIFFS.exists(GW_TOKEN_PATH) ||
                     SPIFFS.remove(GW_TOKEN_PATH);
        if (!nvs_ok || !fs_ok)
            event_add("gateway token clear failed (nvs=%d fs=%d)",
                      (int)nvs_ok, (int)fs_ok);
    }
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

static bool reply_upstream_queue(const char *key, const char *text) {
    uint32_t id = outbox_enqueue(&g_outbox, OUTBOX_KIND_REPLY, key, "", key, text);
    if (!id) return false;
    if (outbox_persist()) return true;
    outbox_remove(&g_outbox, id);
    return false;
}

enum ReplyHttpResult : uint8_t { REPLY_HTTP_FAIL, REPLY_HTTP_OK, REPLY_HTTP_AUTH };

static ReplyHttpResult reply_upstream_http(const char *key, const char *text) {
    if (WiFi.status() != WL_CONNECTED) return REPLY_HTTP_FAIL;
    if (!mesh_gw_url[0]) return REPLY_HTTP_FAIL;

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
    // Runs on the loop task: must stay under the task WDT (~5 s).
    http.setConnectTimeout(500);
    http.setTimeout(1200);
    if (!http.begin(url)) return REPLY_HTTP_FAIL;
    http.addHeader("Content-Type", "application/json");
    if (gw_token[0]) {
        http.addHeader("Authorization", String("Bearer ") + gw_token);
    }
    int code = http.POST(body);
    http.end();
    if (code == 401 || code == 403) return REPLY_HTTP_AUTH;
    return (code >= 200 && code < 300) ? REPLY_HTTP_OK : REPLY_HTTP_FAIL;
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

static void outbox_reply_poll(OutboxItem *item) {
    if (!item || item->kind != OUTBOX_KIND_REPLY) return;
    uint32_t now = millis();
    if (item->next_try_ms && (int32_t)(now - item->next_try_ms) < 0) return;

    item->state = OUTBOX_STATE_SENDING;
    ReplyHttpResult http = reply_upstream_http(item->target, item->text);
    if (http == REPLY_HTTP_OK) {
        item->state = OUTBOX_STATE_SENT;
        outbox_persist();
        event_add("reply upstream wifi ok: %s", item->target);
        return;
    }
    if (http == REPLY_HTTP_AUTH) {
        item->state = OUTBOX_STATE_AUTH;
        outbox_persist();
        event_add("reply upstream auth: %s", item->target);
        return;
    }
    // C1/probe/R1 share one MeshCore ACK slot. Keep this reply pending until
    // the current private frame completes instead of overwriting its CRC.
    if (mesh_client_ack_pending()) {
        item->state = OUTBOX_STATE_QUEUED;
        return;
    }
    if (reply_upstream_mesh(item->target, item->text)) {
        item->state = OUTBOX_STATE_SENT;
        outbox_persist();
        event_add("reply upstream mesh R1: %s", item->target);
        return;
    }
    item->attempts++;
    if (item->attempts >= OUTBOX_ATTEMPTS_MAX) {
        item->state = OUTBOX_STATE_FAIL;
        item->next_try_ms = 0;
    } else {
        item->state = OUTBOX_STATE_QUEUED;
        item->next_try_ms = now + outbox_retry_delay_ms(item->attempts);
    }
    outbox_persist();
    event_add("reply upstream retry %u: %s", (unsigned)item->attempts,
              item->target);
}

static void outbox_agent_poll(OutboxItem *item) {
    if (!item || item->kind != OUTBOX_KIND_AGENT) return;
    uint32_t now = millis();
    if (item->next_try_ms && (int32_t)(now - item->next_try_ms) < 0) return;
    int idx = agents_find(item->target);
    if (idx < 0 || g_convs[idx].transport != CONV_AGENT) {
        item->state = OUTBOX_STATE_FAIL;
        outbox_persist();
        return;
    }
    /* Loop task: never HTTP-post here (was 4 s freeze + chat spam). Mesh
     * uplink is queue-or-fail only. WiFi retries when the user resends. */
    item->state = OUTBOX_STATE_SENDING;
    bool ok = false;
    if (g_agents_mesh_uplink)
        ok = g_agents_mesh_uplink(item->target, item->text, item->delivery_key);
    if (ok) {
        item->state = OUTBOX_STATE_SENT;
        outbox_persist();
        agents_mark_last_pending(idx, AGENT_DELIV_OK);
        display_force = true;
        return;
    }
    item->attempts++;
    if (item->attempts >= OUTBOX_ATTEMPTS_MAX) {
        item->state = OUTBOX_STATE_FAIL;
        item->next_try_ms = 0;
        agents_mark_last_pending(idx, AGENT_DELIV_FAIL);
        display_force = true;
    } else {
        item->state = OUTBOX_STATE_QUEUED;
        item->next_try_ms = now + outbox_retry_delay_ms(item->attempts);
    }
    outbox_persist();
}

static void outbox_poll() {
    OutboxItem *item = outbox_oldest_pending(&g_outbox);
    if (!item) return;
    if (item->kind == OUTBOX_KIND_REPLY) outbox_reply_poll(item);
    else if (item->kind == OUTBOX_KIND_AGENT) outbox_agent_poll(item);
}

static const char *outbox_reply_status(const char *key) {
    const OutboxItem *item = outbox_latest_target(&g_outbox, OUTBOX_KIND_REPLY, key);
    if (!item) return nullptr;
    switch (item->state) {
    case OUTBOX_STATE_QUEUED:
    case OUTBOX_STATE_SENDING: return "QUEUED";
    case OUTBOX_STATE_SENT:    return "SENT";
    case OUTBOX_STATE_AUTH:    return "AUTH";
    case OUTBOX_STATE_FAIL:    return "FAIL";
    default:                   return nullptr;
    }
}

static char outbox_reply_mark(const char *key) {
    const OutboxItem *item = outbox_latest_target(&g_outbox, OUTBOX_KIND_REPLY, key);
    if (!item) return '\0';
    if (item->state == OUTBOX_STATE_QUEUED || item->state == OUTBOX_STATE_SENDING)
        return '~';
    if (item->state == OUTBOX_STATE_SENT) return '+';
    return '!';
}

// POST {gw}/flush {"id":…} — the attach flush beacon (CARD-DELIVERY / C3). Runs
// on the loop task, off AsyncTCP, same blocking-HTTP discipline as
// reply_upstream_http (1 s connect cap so a black-holed gateway does not freeze
// the panel). True only on 2xx: the gateway saw us and will flush the outbox.
static bool flush_beacon_http() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!mesh_gw_url[0]) return false;

    char url[sizeof(mesh_gw_url) + 8];
    if (!attach_beacon_endpoint(mesh_gw_url, url, sizeof(url))) return false;
    char body[ATTACH_BEACON_BODY_CAP];
    if (!attach_beacon_body(node_name.c_str(), body, sizeof(body))) return false;

    HTTPClient http;
    http.setConnectTimeout(500);
    http.setTimeout(1200);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (gw_token[0]) {
        http.addHeader("Authorization", String("Bearer ") + gw_token);
    }
    int code = http.POST(body);
    http.end();
    return code >= 200 && code < 300;
}

// Emit any staged flush beacon. One-shot: WiFi is the attach edge's uplink, so
// if the POST does not land now we DROP it rather than hold it pending and
// hammer a black-holed gateway once per loop pass — the mesh probe and RNS
// announce remain the slow presence backstop, and the next genuine attach
// re-stages (subject to the rate-limit floor).
static void flush_beacon_poll() {
    if (!g_attach_beacon.pending) return;
    if (flush_beacon_http()) {
        g_attach_beacon.pending = false;
        event_add("flush beacon wifi ok: %s", node_name.c_str());
        return;
    }
    g_attach_beacon.pending = false;
    event_add("flush beacon unreachable (dropped)");
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
static void ui_open_net(uint8_t origin = UINAV_WIFI);

static void ui_open_wifi() {
    wifi_sel = 0;
    reply_mode = REPLY_MODE_NONE;
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

static void wifi_off_poll() {
    if (wifi_off_state == WIFI_OFF_IDLE) return;
    if (!wifi_user_off) {
        wifi_off_state = WIFI_OFF_IDLE;
        return;
    }
    switch (wifi_off_state) {
    case WIFI_OFF_STOP_WG:
        g_wg_stop_req = true;
        wifi_off_step_ms = millis();
        wifi_off_state = WIFI_OFF_DISC;
        break;
    case WIFI_OFF_DISC:
        if (millis() - wifi_off_step_ms < WIFI_OFF_WG_SETTLE_MS) return;
        WiFi.disconnect(false, false);
        wifi_off_step_ms = millis();
        wifi_off_state = WIFI_OFF_MODE;
        break;
    case WIFI_OFF_MODE:
        if (millis() - wifi_off_step_ms < WIFI_OFF_DISC_SETTLE_MS) return;
        WiFi.mode(WIFI_OFF);
        wifi_off_state = WIFI_OFF_IDLE;
        Serial.println("[wifi] RF off (mesh only)");
        break;
    }
}

// WiFi on/off switch for mesh-only testing in the garden: STA ON (reconnect via
// saved profiles) or STA OFF (disconnect, leave mesh/WG path alone).
static void ui_wifi_toggle() {
    if (!wifi_user_off) {
        wifi_user_off = true;
        wifi_off_request();
        event_add("wifi off (mesh only)");
        Serial.println("[wifi] user toggled OFF");
    } else {
        wifi_user_off = false;
        wifi_off_state = WIFI_OFF_IDLE;
        WiFi.mode(WIFI_STA);
        if (wifi_net_count > 0 || wifi_ssid[0]) {
            wifi_retry_step = 0;  /* user intent = fresh ladder */
            wifi_begin_active_profile();
            event_add("wifi on (async reconnect)");
            Serial.println("[wifi] user toggled ON — reconnecting");
        } else {
            event_add("wifi on requested, no saved profile");
            Serial.println("[wifi] user toggled ON — no saved profile");
        }
    }
    ui_open_net(UINAV_WIFI);
    ui_note_input();
}

static void ui_wifi_do_scan() {
    /* Blocking scan — paint "SCAN…" first so the panel is not frozen blank. */
    static const char *wait_lines[] = { "scanning…", "hold on" };
    hw_ui_show_wifi_progress(wait_lines, 2);
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
        char raw_title[48];
        snprintf(raw_title, sizeof(raw_title), "%s%c %ddBm",
                 wifi_list_ssids[wifi_list_count], known ? '*' : ' ',
                 (int)WiFi.RSSI(i));
        utf8_text_copy(wifi_list_titles[wifi_list_count],
                       sizeof(wifi_list_titles[0]), raw_title, SIZE_MAX, false);
        wifi_list_count++;
    }
    WiFi.scanDelete();
    if (wifi_user_off) wifi_off_request();
    ui_wifi_paint_list();
}

static void ui_wifi_show_profiles() {
    wifi_list_mode = WIFI_LIST_PROFILES;
    wifi_list_count = 0;
    wifi_list_sel = 0;
    for (int i = 0; i < wifi_net_count && wifi_list_count < HW_UI_WIFI_LIST_MAX; i++) {
        snprintf(wifi_list_ssids[wifi_list_count],
                 sizeof(wifi_list_ssids[0]), "%s", wifi_nets[i].ssid);
        char raw_title[48];
        snprintf(raw_title, sizeof(raw_title), "%c %s",
                 (i == wifi_net_idx) ? '*' : ' ', wifi_nets[i].ssid);
        utf8_text_copy(wifi_list_titles[wifi_list_count],
                       sizeof(wifi_list_titles[0]), raw_title, SIZE_MAX, false);
        wifi_list_count++;
    }
    ui_wifi_paint_list();
}

static void ui_wifi_open_password(const char *ssid) {
    if (!ssid || !ssid[0]) return;
    snprintf(wifi_pending_ssid, sizeof(wifi_pending_ssid), "%s", ssid);
    reply_mode = REPLY_MODE_WIFI_PASSWORD;
    notify_card_id = 0;
    snprintf(reply_title, sizeof(reply_title), "PASS %s", ssid);
    reply_buf[0] = '\0';
    hw_kb_reset_mods();
    ui_reply_paint();
    ui_note_input();
}

static void ui_wifi_open_hidden_ssid() {
    wifi_pending_ssid[0] = '\0';
    reply_mode = REPLY_MODE_WIFI_SSID;
    notify_card_id = 0;
    snprintf(reply_title, sizeof(reply_title), "NETWORK NAME (32 BYTES)");
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
            hw_ui_show_wifi_progress(ptrs, 2);
            delay(30);
            wifi_user_off = false;
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(false, false);
            delay(80);
            wifi_retry_step = 0;  /* user intent = fresh ladder */
            wifi_begin_active_profile();
            snprintf(lines[0], sizeof(lines[0]), "CONNECTING");
            snprintf(lines[1], sizeof(lines[1]), "%s", ssid);
            snprintf(lines[2], sizeof(lines[2]), "async - UI stays live");
            ptrs[2] = lines[2];
            hw_ui_show_wifi_progress(ptrs, 3);
            event_add("wifi menu async connect %s", ssid);
            ui_note_input();
            return;
        }
    }
    /* Unknown network from scan → type password. */
    ui_wifi_open_password(ssid);
}

// MESHCORE → PING GATEWAY: WiFi reachability probe + Mesh private DM.
// The final face focuses on MeshCore; WiFi detail lives in Network. The
// existing HTTP probe remains unchanged.
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
        // Loop task / PING face: stay under task WDT.
        http.setConnectTimeout(500);
        http.setTimeout(1200);
        if (http.begin(url)) {
            if (gw_token[0]) {
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

    hw_ui_show_mesh_ping_result(mesh_ok, mesh_s1, mesh_s2);
    if (mesh_ok) hw_haptic_notify(0);
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
    // Atomic like every other small persisted setting: a power cut mid-write
    // must never leave an empty layout file (empty reads as "no preference").
    write_spiffs_file_atomic(KB_LAYOUT_PATH, KB_LAYOUT_TMP,
                             String(hw_kb_layout_id()));
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
    const char *mode = "COMPOSE";
    switch (reply_mode) {
    case REPLY_MODE_NOTIFY: mode = "NOTIFY REPLY";  break;
    case REPLY_MODE_AGENT:  mode = "AGENT REPLY";   break;
    case REPLY_MODE_WIFI_SSID: mode = "WIFI SSID"; break;
    case REPLY_MODE_WIFI_PASSWORD: mode = "WIFI PASSWORD"; break;
    default: break;
    }
    hw_ui_show_reply(mode, reply_title, reply_buf, hw_kb_caps(), hw_kb_symbol(),
                     hw_kb_layout_name());
}

// Keyboard shortcut HOME: leave any face, drop drafts, show clock.
static void ui_go_home() {
    notify_card_id = 0;
    reply_buf[0] = '\0';
    reply_mode = REPLY_MODE_NONE;
    hw_kb_reset_mods();
    ui_go_clock(NULL);
    ui_note_input();
}

static void ui_open_msglist();  // defined below
static void ui_open_notify_id(uint32_t id);
static void ui_open_card_act();
static void ui_card_act_confirm();
static void ui_open_agent_sessions(int idx);
static void ui_agent_sessions_refresh();
static void ui_open_agent_chat(int idx);
static void ui_agent_chat_refresh();
static void ui_open_agent_act();
static bool notify_is_chat(const NotifyView &v);
static void ui_enter_agent_from_notify(uint32_t id, const NotifyView &v);
static void ui_open_menu();
static void ui_open_info();
static void ui_open_contacts();

// Navigate to `target` (a UiNavScreen id) by rebuilding its content — the same
// open helpers the per-screen BACK rows call. Used only for the small set of
// screens ui_nav_back_target() can hand back.
static void ui_go_to_screen(uint8_t target) {
    switch (target) {
    case UINAV_CLOCK:          ui_go_clock(NULL);                 break;
    case UINAV_MENU:           ui_open_menu();                    break;
    case UINAV_MSGLIST:        ui_open_msglist();                 break;
    case UINAV_NOTIFY:         ui_open_notify_id(notify_card_id); break;
    case UINAV_AGENT_CHAT:     ui_open_agent_chat(agent_focus);   break;
    case UINAV_AGENT_SESSIONS: ui_open_agent_sessions(agent_focus); break;
    case UINAV_SETTINGS:       ui_open_settings();                break;
    case UINAV_MESHCORE:       ui_open_meshcore();                break;
    case UINAV_WIFI:           ui_open_wifi();                    break;
    case UINAV_CONTACTS:       ui_open_contacts();               break;
    default:                   ui_go_clock(NULL);                 break;
    }
}

// One "go back" step from the current face — the BACKSPACE key's navigation
// action and the shared spec for the BACK rows. No-op on CLOCK (root) and on the
// text-entry editor (REPLY handles BACKSPACE as delete-a-char before this runs).
static void ui_go_back() {
    HwUiScreen scr = hw_ui_screen();
    if (!ui_nav_backspace_goes_back((uint8_t)scr)) return;
    bool has_rooms = (scr == HW_UI_AGENT_CHAT) && agents_has_rooms(agent_focus);
    ui_go_to_screen(ui_nav_back_target((uint8_t)scr, has_rooms, agent_origin,
                                       net_origin));
    hw_haptic_notify(0);
}

// first_utf8: optional first character (UTF-8); NULL/empty = empty draft.
static void ui_open_reply(uint32_t id, const char *title, const char *first_utf8) {
    reply_mode = REPLY_MODE_NOTIFY;
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
    reply_mode = REPLY_MODE_AGENT;
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
    if (!reply_buf[0] && reply_mode != REPLY_MODE_WIFI_PASSWORD) return;
    switch (reply_mode) {
    case REPLY_MODE_WIFI_SSID: {
        bool content = false;
        size_t bytes = strlen(reply_buf);
        if (bytes > 32 || !utf8_text_is_printable(reply_buf, &content) || !content) {
            hw_haptic_notify(1);
            return;
        }
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", reply_buf);
        ui_wifi_open_password(ssid);
        return;
    }
    case REPLY_MODE_WIFI_PASSWORD: {
        /* Save password for pending SSID and connect. */
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", wifi_pending_ssid);
        wifi_nets_upsert(ssid, reply_buf);
        wifi_persist_profiles();
        reply_buf[0] = '\0';
        reply_mode = REPLY_MODE_NONE;
        wifi_pending_ssid[0] = '\0';
        hw_haptic_notify(0);
        ui_wifi_connect_ssid(ssid);
        return;
    }
    case REPLY_MODE_AGENT: {
        const char *aid = agents_id(agent_focus);
        if (agents_send(aid, reply_buf)) {
            hw_haptic_notify(0);
            if (!settings_silent) hw_sound_notify(0);
        }
        reply_buf[0] = '\0';
        reply_mode = REPLY_MODE_NONE;
        ui_open_agent_chat(agent_focus);
        return;
    }
    case REPLY_MODE_NOTIFY:
        break;
    default:
        return;
    }
    if (!notify_card_id) return;
    bool queued = false;
    if (notify_set_reply(notify_card_id, reply_buf)) {
        hw_haptic_notify(0);
        if (!settings_silent) hw_sound_notify(0);
        event_add("reply id=%lu: %s", (unsigned long)notify_card_id, reply_buf);
        // Read the card back rather than sending reply_buf: the store cleaned
        // and truncated it, and what goes upstream must be what GET /notify/one
        // serves. A card with no client id has nowhere to go — that is the whole
        // condition, and it leaves the reply stored exactly as before.
        NotifyView v;
        if (notify_view_by_id(notify_card_id, v, NULL, NULL) && v.key[0]) {
            queued = reply_upstream_queue(v.key, v.reply);
            event_add("reply outbox %s: %s", queued ? "queued" : "full", v.key);
        }
    }
    notify_card_id = 0;
    reply_buf[0] = '\0';
    reply_mode = REPLY_MODE_NONE;
    ui_go_clock(queued ? "queued" : "saved");
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

    // Backspace = go back one screen on every navigation face. On the text-entry
    // editor (REPLY) it still deletes a character — handled by that screen's
    // block below, so it is excluded here.
    if (c0 == '\b' && !ui_nav_is_text_entry((uint8_t)scr)) {
        ui_go_back();
        return;
    }

    // From a card: typing jumps straight into reply.
    // Enter: agent door → open chat; else Ack·Reply sheet.
    if (scr == HW_UI_NOTIFY && notify_card_id) {
        if (c0 == '\n') {
            NotifyView v;
            if (notify_view_by_id(notify_card_id, v, NULL, NULL) &&
                notify_is_chat(v)) {
                ui_enter_agent_from_notify(notify_card_id, v);
            } else {
                ui_open_card_act();
            }
            return;
        }
        NotifyView v;
        if (notify_view_by_id(notify_card_id, v, NULL, NULL)) {
            if (notify_is_chat(v)) {
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

static bool notify_event_distinct_cb(const char *source, const char *key) {
    if (notify_rec_is_chat_door_key(key)) return false;
    NotifyChatResolution resolution = agents_notify_chat_resolve_snapshot(source, key);
    /* Distinct means "one chat event, never a key update", so it must name the
     * chat events and nothing else. Reading it as "everything that is not a
     * conversation" made every ordinary keyed card distinct, which switched OFF
     * replace-by-key for exactly the cards that live on it: the pills reminder
     * re-paged hourly and stacked ten unread CRITs instead of updating one, the
     * store filled to 40/40, and crit_unread then floored the backlight at dim
     * forever, so the panel stopped blanking. A known conversation is carried by
     * its thread; the feed filter (notify_is_chat) is what keeps those rows out
     * of the inbox, not this flag. */
    return resolution.conversation >= 0;
}

static char notify_chat_normalized[NOTIFY_BODY_LEN];

/* Resolve and normalize together. Every classifier, feed filter and router uses
 * this one plan, so a card cannot be hidden under one conversation rule and
 * then routed under another. */
static NotifyChatResolution notify_chat_candidate(const NotifyView &v) {
    agents_normalize_inbound(v.body, notify_chat_normalized,
                             sizeof(notify_chat_normalized));
    return agents_notify_chat_resolve_snapshot(v.source, v.key);
}

/* A resolved, valid card becomes a chat row only after its backing door is
 * read. Fresh and ambiguous restored doors remain visible until routing has
 * succeeded and notify_ack_id has archived that fact. */
static bool notify_is_chat(const NotifyView &v) {
    NotifyChatResolution resolution = notify_chat_candidate(v);
    return !v.unread && resolution.conversation >= 0 &&
           notify_chat_normalized[0] != '\0';
}

struct NotifyChatRestoreItem {
    uint32_t id;
    NotifyChatRestoreOrder order;
};

/* Restored cards never pass through loop()'s arrival branch. Snapshot their
 * order first, then replay oldest-first so one empty thread preserves the
 * original chronology. Exact durable event identity makes a crash-window
 * replay an ACK-only operation; absent origin is appended and ACKed only after
 * the full JSONL record persists. Legacy cards without an identity stay visible
 * because text, client key and the recyclable ring id are not safe evidence. */
static void notify_reconcile_restored_chats() {
    static NotifyView view;
    static NotifyChatRestoreItem items[NOTIFY_MAX];
    bool blocked[CONV_MAX] = {};
    bool retry_needed = false;
    int count = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notify_view(i, view)) break;
        if (!view.unread || count >= NOTIFY_MAX) continue;
        NotifyChatResolution resolution = notify_chat_candidate(view);
        if (resolution.conversation < 0 || !notify_chat_normalized[0]) continue;
        /* Boot: doorbells stay out of the JSONL (inbound already persisted). */
        if (notify_rec_is_chat_door_key(view.key)) {
            notify_ack_id(view.id);
            continue;
        }
        if (!notify_event_id_valid(&view.event_id)) continue;
        items[count].id = view.id;
        items[count].order.card_epoch = view.created_epoch;
        items[count].order.restore_rank = (uint8_t)i;
        count++;
    }
    for (int i = 1; i < count; i++) {
        NotifyChatRestoreItem item = items[i];
        int j = i;
        while (j > 0 && notify_chat_restore_before(item.order,
                                                    items[j - 1].order)) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = item;
    }
    for (int i = 0; i < count; i++) {
        if (!notify_view_by_id(items[i].id, view, nullptr, nullptr) || !view.unread)
            continue;
        NotifyChatResolution resolution = notify_chat_candidate(view);
        int ax = resolution.conversation;
        if (ax < 0 || ax >= CONV_MAX || !notify_chat_normalized[0]) continue;
        if (notify_rec_is_chat_door_key(view.key)) {
            notify_ack_id(view.id);
            continue;
        }
        if (agents_last_text_is(ax, notify_chat_normalized)) {
            notify_ack_id(view.id);
            continue;
        }
        NotifyChatThreadState thread = {
            agents_has_origin(ax, view.id, view.key,
                              &view.event_id)
        };
        NotifyChatAction action = notify_chat_reconcile_plan(
            &resolution, notify_chat_normalized, view.created_epoch, &thread);
        bool accepted = (action == NOTIFY_CHAT_ACK_ALREADY_ROUTED);
        if (action == NOTIFY_CHAT_ROUTE_THEN_ACK && !blocked[ax])
            accepted = agents_restore_inbound(ax,
                                               notify_chat_normalized,
                                               view.id, view.key, &view.event_id);
        NotifyChatDrainResult result = notify_chat_drain_result(blocked[ax], accepted);
        if (result == NOTIFY_CHAT_DRAIN_KEEP_FAILED) blocked[ax] = true;
        if (result != NOTIFY_CHAT_DRAIN_ACK_ROUTED) {
            retry_needed = true;
            continue;
        }
        notify_ack_id(view.id);
    }
    if (retry_needed) {
        notify_chat_retry_pending = true;
        notify_chat_retry_at = millis() + 1000;
    }
    /* Boot reconciliation is store repair, not a user-visible arrival/input.
     * notify_ack_id is still used for its archive write-through, but its normal
     * runtime repaint/ring hints must not leak into the first loop pass. */
    notify_ring_acked = false;
    display_force = false;
}

/* A single arrival latch is only a repaint signal: on each signal, snapshot and
 * drain every fresh unread chat door oldest-first. This makes a burst lossless
 * even when producers overwrite notify_arrived_id before loop() runs. A failed
 * append blocks newer messages for that conversation during this pass, while
 * other conversations may continue. Exact event origins make retries safe. */
static bool notify_reconcile_pending_chats(HwUiScreen scr) {
    (void)scr;
    static const int DRAIN_BATCH = 4;
    static NotifyView view;
    static NotifyChatRestoreItem items[NOTIFY_MAX];
    bool queued[CONV_MAX] = {};
    bool retry_needed = false;
    int attempted = 0;
    int count = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notify_view(i, view)) break;
        if (!view.unread || count >= NOTIFY_MAX) continue;
        NotifyChatResolution resolution = notify_chat_candidate(view);
        if (resolution.conversation < 0 || !notify_chat_normalized[0]) continue;
        /* *-chat: wait 2 s for /agents/inbound. If inbound landed, skip.
         * If it missed (HTTP died), copy the card so the room is not empty. */
        if (notify_rec_is_chat_door_key(view.key)) {
            if (agents_thread_has_prefix(resolution.conversation,
                                         notify_chat_normalized))
                continue;
            if (view.age_s < 2) {
                retry_needed = true;
                continue;
            }
        }
        if (!notify_event_id_valid(&view.event_id)) continue;
        items[count].id = view.id;
        items[count].order.card_epoch = view.created_epoch;
        items[count].order.restore_rank = (uint8_t)i;
        count++;
    }
    for (int i = 1; i < count; i++) {
        NotifyChatRestoreItem item = items[i];
        int j = i;
        while (j > 0 && notify_chat_restore_before(item.order,
                                                    items[j - 1].order)) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = item;
    }
    for (int i = 0; i < count; i++) {
        if (!notify_view_by_id(items[i].id, view, nullptr, nullptr) || !view.unread)
            continue;
        NotifyChatResolution resolution = notify_chat_candidate(view);
        int ax = resolution.conversation;
        if (ax < 0 || ax >= CONV_MAX || !notify_chat_normalized[0]) continue;
        if (notify_rec_is_chat_door_key(view.key) &&
            agents_thread_has_prefix(ax, notify_chat_normalized))
            continue;
        if (agents_last_text_is(ax, notify_chat_normalized)) {
            notify_ack_id(view.id);
            continue;
        }
        if (attempted >= DRAIN_BATCH) { retry_needed = true; continue; }
        if (queued[ax] || notify_chat_inflight_find(resolution.id) >= 0) {
            retry_needed = true;
            continue;
        }
        attempted++;
        if (notify_chat_inflight_add(resolution.id, &view.event_id) &&
            agents_chat_door_enqueue(ax, resolution.id, view.source,
                                     notify_chat_normalized, view.id,
                                     view.key, &view.event_id)) {
            queued[ax] = true;
        } else {
            notify_chat_inflight_remove(resolution.id, &view.event_id);
            retry_needed = true;
        }
    }
    return retry_needed;
}

static void notify_take_chat_completions(HwUiScreen scr) {
    AgentDoorCompletion done;
    while (agents_chat_door_take_completion(done)) {
        notify_chat_inflight_remove(done.conversation, &done.event);
        NotifyView view;
        bool same_card = notify_view_by_id(done.id, view, nullptr, nullptr) &&
                         notify_event_id_equal(&view.event_id, &done.event);
        if (done.accepted && same_card &&
            notify_ack_identity(done.id, &done.event)) {
            int current_idx = agents_notify_chat_resolve_snapshot(
                done.conversation, "").conversation;
            if (scr == HW_UI_AGENT_CHAT && agent_focus == current_idx) {
                ui_agent_chat_refresh();
                ui_note_wake();
            }
            notify_chat_retry_at = millis();
        } else {
            notify_chat_retry_at = millis() + 1000;
        }
        notify_chat_retry_pending = true;
    }
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
    if (!ag_chat_lines)
        ag_chat_lines = (char (*)[AGENT_TEXT_LEN])psram_calloc_pref(
            (size_t)AGENT_THREAD_MAX * AGENT_TEXT_LEN);
    if (!ag_chat_lines) {
        /* Both pools exhausted. Leave the face as it stands rather than paint a
         * half-built one: the refresh is idempotent and the next one recovers. */
        Serial.println("[ui] chat line buffer alloc FAILED - chat not refreshed");
        return;
    }
    agents_lock();
    n = agents_thread_view(agent_focus, ag_chat_lines, ag_chat_from_me,
                           ag_chat_ts, AGENT_THREAD_MAX, ag_chat_deliv);
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
    int shown = hw_ui_show_agent_chat(head, ag_chat_ptrs, ag_chat_from_me,
                                      n, agent_scroll, &total, foot,
                                      ag_chat_deliv);
    agent_scroll_total = total;
    // Keep -1 as the follow-tail sentinel. The renderer converts it to
    // max_scroll for paint; writing that back would lose "pinned to latest"
    // and a new reply would land off-screen.
    if (agent_scroll >= 0) agent_scroll = shown;
}

static void notify_ack_open_chat(int conv_idx) {
    static NotifyView view;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notify_view(i, view)) break;
        if (!view.unread) continue;
        if (notify_chat_candidate(view).conversation == conv_idx)
            notify_ack_id(view.id);
    }
}

static void ui_open_agent_chat(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= agents_count()) idx = agents_count() - 1;
    agent_focus = idx;
    reply_mode = REPLY_MODE_NONE;
    agent_scroll = -1;  // pin to latest when entering the room
    agents_thread_goto_tail(idx);  // load the session's history from the store
    notify_ack_open_chat(idx);     // doorbells are read once the room is open
    ui_agent_chat_refresh();
}

/* Sessions screen for the focused agent: existing sessions (with honest
 * message counts) + "NEW SESSION" + "BACK". Counts are refreshed on open.
 * When the session registry is full the NEW row turns into a visible
 * "MAX n SESSIONS" note and is not selectable — no silent refusal. */
static void ui_agent_sessions_refresh() {
    int all = agents_session_count(agent_focus);
    // Registry capacity is over ALL sessions (dead rooms still hold a slot), so
    // the "NEW" row disables on the full registry regardless of what is hidden.
    bool full = (all >= AGENT_SESSIONS_MAX);
    int n = 0;  // visible (non-dead) rows
    for (int i = 0; i < all; i++) {
        if (agents_session_is_dead(agent_focus, i)) continue;  // roster: hidden
        snprintf(ag_sess_titles[n], sizeof(ag_sess_titles[0]), "%s",
                 agents_session_name(agent_focus, i));
        ag_sess_msgs[n] = agents_session_msg_count(agent_focus, i);
        ag_sess_active[n] = agents_session_is_active(agent_focus, i);
        ag_sess_ptrs[n] = ag_sess_titles[n];
        ag_sess_row2idx[n] = i;
        n++;
    }
    ag_sess_vis_n = n;
    int total = n + 2;
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
    // Select the active session's VISIBLE row (dead rooms are hidden, so the
    // row is not the raw index). If the active room is itself dead it is not in
    // the list; fall back to the first visible row (0) — refresh clamps into
    // the NEW/BACK tail when nothing is visible.
    agent_sess_sel = 0;
    int vis_row = 0;
    for (int i = 0; i < agents_session_count(idx); i++) {
        if (agents_session_is_dead(idx, i)) continue;
        if (agents_session_is_active(idx, i)) { agent_sess_sel = vis_row; break; }
        vis_row++;
    }
    ui_agent_sessions_refresh();
}

// The in-chat sheet has a variable row count: CLEAR / DELETE / BACK for a peer
// conversation, CLEAR / BACK for a seeded door (DELETE dropped). These two map a
// visible row index onto the stable AGENT_ACT_* action for the current layout.
static int agent_act_row_count() { return agent_act_del_ok ? 3 : 2; }
static int agent_act_row_action(int sel) {
    if (agent_act_del_ok) return sel;                 // 0=CLEAR 1=DELETE 2=BACK
    return (sel == 0) ? AGENT_ACT_CLEAR : AGENT_ACT_BACK;  // 0=CLEAR 1=BACK
}

static void ui_open_agent_act() {
    agent_act_sel = 0;
    agent_act_del_ok = !agents_is_seeded(agent_focus);
    hw_ui_show_agent_act(agent_act_sel, agents_name(agent_focus), agent_act_del_ok);
    ui_note_input();
}

static void ui_agent_act_confirm() {
    int act = agent_act_row_action(agent_act_sel);
    if (act == AGENT_ACT_CLEAR) {
        const char *aid = agents_id(agent_focus);
        agents_clear(aid);
        /* Hermes /new is the session reset the user always types after clear.
         * One wheel click: wipe local history and start a fresh agent turn. */
        if (agents_transport(agent_focus) == CONV_AGENT)
            agents_send(aid, "/new");
        hw_haptic_notify(0);
        ui_open_agent_chat(agent_focus);
    } else if (act == AGENT_ACT_DELETE) {
        // Whole conversation gone — history, manifest line and RAM slot — matched
        // by id (recycle-safe), so it does not come back on the next boot. Then
        // the unified feed, which recomputes without it.
        agents_delete(agents_id(agent_focus));
        hw_haptic_notify(0);
        ui_open_msglist();
    } else {
        ui_open_msglist();   // BACK from the in-chat sheet → the unified feed
    }
}

static void ui_open_info() {
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", SEED_VERSION);
    hw_ui_show_info(ver, node_name.c_str(), auth_token.c_str(),
                    ESP.getFreeHeap());
    ui_note_input();
}

// One notify level → one severity letter, the glyph a card row shows in the
// unified feed (conversation rows show a transport glyph instead).
static char msglist_sev_letter(uint8_t level) {
    const char *n = notify_level_name(level);
    if (n[0] == 'c' || n[0] == 'C') return 'C';
    if (n[0] == 'w' || n[0] == 'W') return 'W';
    return 'I';
}

// Build the unified Messages feed: notification cards AND chat conversations,
// merged into one list ordered by unix time, newest first (src/feed_view.h).
// Cards sort on created_epoch, conversations on last.ts; the row's tagged
// handle (card id vs conv slot+id) is cached so a click opens the right thing.
static void ui_open_msglist() {
    static char titles[HW_UI_MSGLIST_MAX][FEED_LABEL_LEN];
    static char times[HW_UI_MSGLIST_MAX][6];
    static char glyphs[HW_UI_MSGLIST_MAX];
    static bool is_conv[HW_UI_MSGLIST_MAX];
    static bool unread[HW_UI_MSGLIST_MAX];
    static const char *title_ptrs[HW_UI_MSGLIST_MAX];
    static const char *time_ptrs[HW_UI_MSGLIST_MAX];

    // Cards out of the notify queue (newest first). Titles are copied into a
    // scratch that outlives the merge, so the view can point at them.
    static FeedCardView cards[FEED_MAX_CARDS];
    static char card_titles[FEED_MAX_CARDS][NOTIFY_TITLE_LEN];
    static NotifyView v;
    int nc = 0;
    for (int i = 0; i < NOTIFY_MAX && nc < FEED_MAX_CARDS; i++) {
        if (!notify_view(i, v)) break;
        // Chat lives on the conversation row. A doorbell is a wake, not
        // another inbox line — otherwise Hermes fills the whole list.
        if (notify_rec_is_chat_door_key(v.key)) continue;
        if (notify_chat_candidate(v).conversation >= 0) continue;
        snprintf(card_titles[nc], sizeof(card_titles[0]), "%s", v.title);
        cards[nc].id = v.id;
        cards[nc].epoch = v.created_epoch;
        cards[nc].title = card_titles[nc];
        char delivery = outbox_reply_mark(v.key);
        cards[nc].sev = delivery ? delivery : msglist_sev_letter(v.level);
        cards[nc].unread = v.unread ? 1 : 0;
        nc++;
    }

    // Conversations out of the live table, then merge — both under the lock,
    // because the view points into the table until feed_build_rows copies each
    // label and id out. After it returns the rows are self-contained.
    static FeedConvView convs[CONV_MAX];
    static FeedRow rows[HW_UI_MSGLIST_MAX];
    int rn = 0;
    agents_lock();
    int total = agents_count();
    int nv = 0;
    for (int i = 0; i < total && nv < CONV_MAX; i++) {
        convs[nv].slot = (uint8_t)i;
        convs[nv].id = agents_id(i);
        convs[nv].label = agents_name(i);
        convs[nv].epoch = agents_last_ts(i);
        convs[nv].transport = agents_transport(i);
        convs[nv].unread = agents_unread(i);
        convs[nv].has_rooms = agents_has_rooms(i) ? 1 : 0;
        nv++;
    }
    rn = feed_build_rows(cards, nc, convs, nv, rows, HW_UI_MSGLIST_MAX);
    agents_unlock();

    msglist_count = 0;
    for (int i = 0; i < rn && msglist_count < HW_UI_MSGLIST_MAX; i++) {
        const FeedRow &r = rows[i];
        snprintf(titles[msglist_count], sizeof(titles[0]), "%s", r.label);
        title_ptrs[msglist_count] = titles[msglist_count];
        agents_head_time(r.epoch, times[msglist_count], sizeof(times[0]));
        time_ptrs[msglist_count] = times[msglist_count];
        glyphs[msglist_count] = r.glyph;
        is_conv[msglist_count] = (r.origin == FEED_CONV);
        unread[msglist_count] = (r.mark != ' ');
        MsgHandle &h = msglist_h[msglist_count];
        if (r.origin == FEED_CONV) {
            h.is_conv = true;
            h.slot = r.slot;
            h.rooms = r.has_rooms != 0;
            h.card_id = 0;
            snprintf(h.conv_id, sizeof(h.conv_id), "%s", r.id);
        } else {
            h.is_conv = false;
            h.card_id = r.card_id;
            h.slot = 0;
            h.rooms = false;
            h.conv_id[0] = '\0';
        }
        msglist_count++;
    }
    if (msglist_sel >= msglist_count) msglist_sel = msglist_count > 0 ? msglist_count - 1 : 0;
    if (msglist_sel < 0) msglist_sel = 0;
    hw_ui_show_msglist(title_ptrs, time_ptrs, glyphs, is_conv, unread,
                       msglist_count, msglist_sel);
    ui_note_input();
}

// ===== Contacts screen =====
// The grouped "who can I write to" list. The rows contacts_build_rows produces
// ARE the tagged handle array: a clicked contact row carries its own
// {transport, id, reply} so open-or-create can act on it after the input arrays
// are gone (src/contacts_view.h). Selection only ever lands on a contact row;
// header rows are non-selectable dividers.
/* Parked in PSRAM (~4 KB): rebuilt under agents_lock on the UI/loop task and
 * read by the renderer on the same task — never an ISR. Allocated on the first
 * rebuild; every reader is already gated by contacts_count, which stays 0 while
 * the pointer is null, so a failed alloc shows an empty list. */
static ContactRow *contacts_rows = nullptr;
static int contacts_count = 0;
static int contacts_sel = 0;
// Flattened view arrays for the renderer (labels point into contacts_rows, which
// is static, so the pointers stay valid until the next rebuild).
static const char *contacts_labels[CONTACT_ROWS_MAX];
static bool contacts_is_header[CONTACT_ROWS_MAX];
static bool contacts_has_conv[CONTACT_ROWS_MAX];

// First contact (non-header) row at or after `from`, stepping by dir (+1/-1);
// -1 if there is none in that direction. This is how selection skips headers.
static int contacts_next_contact(int from, int dir) {
    for (int i = from; i >= 0 && i < contacts_count; i += dir)
        if (contacts_rows[i].kind == CONTACT_ROW_CONTACT) return i;
    return -1;
}

// Flatten contacts_rows into the primitive view arrays and paint. `note` is an
// optional short status line (e.g. the table-full refusal), else NULL.
static void ui_contacts_render(const char *note) {
    for (int i = 0; i < contacts_count; i++) {
        contacts_labels[i] = contacts_rows[i].label;
        contacts_is_header[i] = (contacts_rows[i].kind == CONTACT_ROW_HEADER);
        contacts_has_conv[i] = (contacts_rows[i].has_conversation != 0);
    }
    hw_ui_show_contacts(contacts_labels, contacts_is_header, contacts_has_conv,
                        contacts_count, contacts_sel, note);
    ui_note_input();
}

// Build the three bucket candidate arrays and run contacts_build_rows.
//   AI   = seeded doors (always have a slot).
//   mesh = /contacts3 peers (id + has_conversation derived from the public key).
//   LXMF = EXISTING LXMF conversations only — no manual address-add this commit
//          (that is the future address-book ticket).
static void ui_open_contacts() {
    // Candidate backing: contacts_build_rows COPIES label/id/reply into each
    // row, so this only needs to outlive the build call below. ALL of these are
    // file-/function-scope static (not loop-task stack): mrec alone is
    // sizeof(MeshContactRec)*16 ~= 2.5 KB and, with the three candidate arrays,
    // overflowed the loop task's stack and tripped the canary. Same idiom as
    // ui_open_msglist's static titles/handles above — rebuilt in full each call
    // (the n_ai/n_mesh/n_lxmf loops overwrite every slot before build reads it),
    // reached only from the loop task, so reuse across renders is safe. These are
    // this screen's OWN statics; they are not shared with the msglist buffers.
    static ContactCandidate ai[CONTACT_BUCKET_CAP];
    static ContactCandidate mesh[CONTACT_BUCKET_CAP];
    static ContactCandidate lxmf[CONTACT_BUCKET_CAP];
    static MeshContactRec   mrec[CONTACT_BUCKET_CAP];
    static char mesh_ids[CONTACT_BUCKET_CAP][CONV_ID_LEN];
    static char lx_ids[CONTACT_BUCKET_CAP][CONV_ID_LEN];
    static char lx_labels[CONTACT_BUCKET_CAP][CONV_LABEL_LEN];
    static uint8_t lx_reply[CONTACT_BUCKET_CAP][CONV_REPLY_MAX];
    static uint8_t lx_reply_len[CONTACT_BUCKET_CAP];

    // AI: the seeded doors. They always have a slot (has_conversation = true) and
    // their return address is the agent-id bytes the bridge answers to. The
    // pointers agents_seeded_at hands back stay valid after its lock drops — a
    // seeded slot never moves or is evicted (see agents.cpp).
    int n_ai = 0;
    int seeded = agents_seeded_count();
    for (int i = 0; i < seeded && n_ai < CONTACT_BUCKET_CAP; i++) {
        const char *id = NULL, *label = NULL;
        const uint8_t *reply = NULL;
        uint8_t rlen = 0;
        if (!agents_seeded_at(i, &id, &label, &reply, &rlen)) break;
        ai[n_ai].label = label;
        ai[n_ai].id = id;
        ai[n_ai].transport = CONV_AGENT;
        ai[n_ai].has_conversation = 1;
        ai[n_ai].reply = reply;
        ai[n_ai].reply_len = rlen;
        n_ai++;
    }

    // Mesh: peers this device has met (/contacts3, loop-safe file read). The id
    // and the has_conversation flag both derive from the 32-byte public key the
    // same way conv_mint keys a peer, so a row that maps to an existing thread is
    // marked and opens it rather than making a duplicate.
    int nmesh = mesh_contacts_list(mrec, CONTACT_BUCKET_CAP);
    int n_mesh = 0;
    for (int i = 0; i < nmesh && n_mesh < CONTACT_BUCKET_CAP; i++) {
        conv_peer_id(mrec[i].pub_key, MESH_CONTACT_PUBKEY_LEN,
                     mesh_ids[n_mesh], sizeof(mesh_ids[0]));
        if (!mesh_ids[n_mesh][0]) continue;   // key too short to key on: skip
        mesh[n_mesh].label = mrec[i].name;
        mesh[n_mesh].id = mesh_ids[n_mesh];
        mesh[n_mesh].transport = CONV_MESH;
        mesh[n_mesh].has_conversation =
            agents_peer_has_conversation(mrec[i].pub_key,
                                         MESH_CONTACT_PUBKEY_LEN) ? 1 : 0;
        mesh[n_mesh].reply = mrec[i].pub_key;
        mesh[n_mesh].reply_len = MESH_CONTACT_PUBKEY_LEN;
        n_mesh++;
    }

    // LXMF: existing LXMF conversations only. Snapshot the table under the lock,
    // then build the rows — contacts_build_rows copies each id/label/reply out,
    // so the rows are self-contained once it returns and the lock can drop.
    int n_lxmf = 0;
    agents_lock();
    int total = agents_count();
    for (int i = 0; i < total && n_lxmf < CONTACT_BUCKET_CAP; i++) {
        if (agents_transport(i) != CONV_LXMF) continue;
        snprintf(lx_ids[n_lxmf], sizeof(lx_ids[0]), "%s", agents_id(i));
        snprintf(lx_labels[n_lxmf], sizeof(lx_labels[0]), "%s", agents_name(i));
        lx_reply_len[n_lxmf] = agents_reply_addr(i, lx_reply[n_lxmf],
                                                 CONV_REPLY_MAX);
        lxmf[n_lxmf].label = lx_labels[n_lxmf];
        lxmf[n_lxmf].id = lx_ids[n_lxmf];
        lxmf[n_lxmf].transport = CONV_LXMF;
        lxmf[n_lxmf].has_conversation = 1;   // it exists: that IS the thread
        lxmf[n_lxmf].reply = lx_reply[n_lxmf];
        lxmf[n_lxmf].reply_len = lx_reply_len[n_lxmf];
        n_lxmf++;
    }
    if (!contacts_rows)
        contacts_rows = (ContactRow *)psram_calloc_pref(sizeof(ContactRow) *
                                                        CONTACT_ROWS_MAX);
    contacts_count = contacts_rows
        ? contacts_build_rows(ai, n_ai, lxmf, n_lxmf, mesh, n_mesh,
                              contacts_rows, CONTACT_ROWS_MAX)
        : 0;
    agents_unlock();

    // Land the selection on the first contact row (headers are not selectable).
    contacts_sel = contacts_next_contact(0, +1);
    if (contacts_sel < 0) contacts_sel = 0;
    ui_contacts_render(NULL);
}

// A picked contact opens its chat or creates one, then lands there. On a full
// table that refuses (only a brand-new mesh peer can hit this), surface it and
// stay on the list rather than crash.
static void ui_contacts_open_selected() {
    if (contacts_count == 0) { ui_open_menu(); return; }
    if (contacts_sel < 0 || contacts_sel >= contacts_count) return;
    const ContactRow &r = contacts_rows[contacts_sel];
    if (r.kind != CONTACT_ROW_CONTACT) return;   // never open a header
    int slot = agents_open_or_create(r.transport, r.id, r.label,
                                     r.reply_len ? r.reply : NULL, r.reply_len);
    if (slot < 0) {
        event_add("contacts open refused (table full): %s", r.id);
        hw_haptic_notify(0);
        ui_contacts_render("CONTACTS FULL - CLEAR A CHAT");
        return;
    }
    hw_haptic_notify(0);
    // A multi-session AI door opens its room picker; a single-session one (a
    // freshly created conversation always has exactly one) opens the chat
    // directly, so a first contact tap does not land on an empty picker. The
    // picker backs out to this contacts list.
    agent_origin = UINAV_CONTACTS;
    if (ui_nav_conv_open_target(agents_session_count(slot)) == UINAV_AGENT_SESSIONS)
        ui_open_agent_sessions(slot);
    else
        ui_open_agent_chat(slot);
}

// ===== Network status screen =====
// The sectioned "how am I connected" screen (WiFi / Reticulum / mesh / tunnel).
// Read-only: it snapshots the live per-transport accessors into a NetStatus and
// runs the pure net_build_rows model (src/net_view.h); it changes NO
// connectivity behaviour. Reached from the WiFi menu's STATUS entry — folded in
// there rather than as a new top-level menu item because the main menu is
// already seven rows (MESSAGES..BACK) and an eighth would run off the 222 px
// panel, whereas the WiFi menu's "STATUS" is the natural home for a network
// status view and needs no new row anywhere.
static NetRow      net_rows[NET_ROWS_MAX];
static int         net_count = 0;
static int         net_top = 0;
// Flattened view arrays for the renderer (labels/values point into net_rows,
// which is static, so the pointers stay valid until the next rebuild).
static const char *net_labels[NET_ROWS_MAX];
static const char *net_values[NET_ROWS_MAX];
static bool        net_is_header[NET_ROWS_MAX];
static uint8_t     net_levels[NET_ROWS_MAX];

static void ui_net_render() {
    for (int i = 0; i < net_count; i++) {
        net_labels[i] = net_rows[i].label;
        net_values[i] = net_rows[i].value;
        net_is_header[i] = (net_rows[i].kind == NET_ROW_HEADER);
        net_levels[i] = net_rows[i].level;
    }
    hw_ui_show_net(net_labels, net_values, net_is_header, net_levels,
                   net_count, net_top);
    ui_note_input();
}

// Snapshot every transport's live status and build the sectioned rows. All
// scratch is file-/function-scope static (loop task, tight stack — same rule as
// ui_open_contacts): net_build_rows COPIES each label/value into net_rows, so
// these value buffers only need to outlive the build call. g_mesh / g_rns_* /
// WiFi are read directly on the loop task exactly as conn_mgr_service() and
// ui_clock_paint() do — those globals are loop-task owned, so no extra lock.
static void ui_open_net(uint8_t origin) {
    static char wifi_ssid_s[NET_VALUE_LEN];
    static char wifi_ip_s[NET_VALUE_LEN];
    static char rns_addr_s[NET_VALUE_LEN];
    static char mesh_key_s[NET_VALUE_LEN];
    static char mesh_rf_s[NET_VALUE_LEN];

    net_origin = origin;
    NetStatus s;
    memset(&s, 0, sizeof(s));

    // WiFi.
    bool wc = (WiFi.status() == WL_CONNECTED);
    s.wifi.wanted = !wifi_user_off && (wifi_net_count > 0 || wifi_ssid[0]);
    s.wifi.connected = wc;
    s.wifi.profiles = wifi_net_count;
    if (wc) {
        snprintf(wifi_ssid_s, sizeof(wifi_ssid_s), "%s", WiFi.SSID().c_str());
        snprintf(wifi_ip_s, sizeof(wifi_ip_s), "%s",
                 WiFi.localIP().toString().c_str());
        s.wifi.ssid = wifi_ssid_s;
        s.wifi.ip = wifi_ip_s;
        s.wifi.rssi_dbm = (int)WiFi.RSSI();
    }

    // Reticulum.
    s.rns.enabled = g_rns_cfg_enabled && g_rns_cfg_ok;
    s.rns.has_identity = rns_identity_ok;
    s.rns.link_up = (g_rns_cs.load() == RNS_CS_CONNECTED);
    if (rns_identity_ok && rns_hexhash[0]) {
        snprintf(rns_addr_s, sizeof(rns_addr_s), "%s", rns_hexhash);
        s.rns.addr = rns_addr_s;
    }

    // MeshCore radio.
    s.mesh.has_identity = g_mesh.has_identity;
    s.mesh.ui_state = mesh_ui_state();
    s.mesh.seen_age_s = mesh_alive_age_s();
    if (g_mesh.public_key_hex[0]) {
        snprintf(mesh_key_s, sizeof(mesh_key_s), "%.8s", g_mesh.public_key_hex);
        s.mesh.key8 = mesh_key_s;
    }
    if (g_mesh.sf) {
        snprintf(mesh_rf_s, sizeof(mesh_rf_s), "%.1f SF%u",
                 g_mesh.freq, (unsigned)g_mesh.sf);
        s.mesh.rf = mesh_rf_s;
    }

    // WireGuard tunnel.
    s.tun.wanted = g_wg_want && g_wg_cfg_ok;
    s.tun.ui_state = wg_ui_state();

    net_count = net_build_rows(&s, net_rows, NET_ROWS_MAX);
    net_top = 0;
    if (origin == UINAV_MESHCORE) {
        for (int i = 0; i < net_count; i++) {
            if (net_rows[i].kind == NET_ROW_HEADER &&
                net_rows[i].section == NET_SEC_MESH) {
                net_top = i;
                break;
            }
        }
    }
    ui_net_render();
}

// Enter the chat room from a chat-door notify (ack + open).
static void ui_enter_agent_from_notify(uint32_t id, const NotifyView &v) {
    NotifyChatResolution resolution = notify_chat_candidate(v);
    int ax = resolution.conversation;
    if (ax < 0) return;
    notify_ack_id(id);
    notify_card_id = 0;
    // A chat-door notify lives in the feed, so a picker reached by backing out of
    // this chat backs on to the feed.
    agent_origin = UINAV_MSGLIST;
    ui_open_agent_chat(ax);
}

static void ui_open_notify_id(uint32_t id) {
    NotifyView v;
    if (!notify_view_by_id(id, v, NULL, NULL)) {
        ui_open_msglist();
        return;
    }
    // A chat card opens straight into its room (the picker handles multi-session);
    // real pages keep severity colours.
    if (notify_is_chat(v)) {
        ui_enter_agent_from_notify(id, v);
        return;
    }
    notify_card_id = id;
    snprintf(reply_title, sizeof(reply_title), "%s", v.title);
    hw_ui_show_notify(notify_level_name(v.level), v.source, v.title,
                      v.body, notify_unread_count(), outbox_reply_status(v.key));
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
    } else if (card_act_sel == CARD_ACT_DELETE) {
        // Gone for good: out of the RAM ring and tombstoned in the archive, so
        // it does not come back on the next boot. Rebuild the feed around it.
        notify_delete_id(notify_card_id);
        notify_card_id = 0;
        ui_open_msglist();
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
        } else if (menu_sel == MENU_MESHCORE) {
            ui_open_meshcore();
        } else if (menu_sel == MENU_WIFI) {
            ui_open_wifi();
        } else if (menu_sel == MENU_SETTINGS) {
            settings_sel = 0;
            ui_open_settings();
        } else if (menu_sel == MENU_INFO) {
            ui_open_info();
        } else if (menu_sel == MENU_CONTACTS) {
            ui_open_contacts();
        } else {
            ui_go_clock(NULL);
        }
        break;
    case HW_UI_WIFI:
        if (wifi_sel == WIFI_ACT_BACK) {
            ui_open_menu();
        } else if (wifi_sel == WIFI_ACT_STATUS) {
            ui_open_net(UINAV_WIFI);
        } else if (wifi_sel == WIFI_ACT_SCAN) {
            ui_wifi_do_scan();
        } else if (wifi_sel == WIFI_ACT_PROFILES) {
            ui_wifi_show_profiles();
        } else if (wifi_sel == WIFI_ACT_HIDDEN) {
            ui_wifi_open_hidden_ssid();
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
    case HW_UI_WIFI_PROGRESS:
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
        } else if (settings_sel == SETTINGS_SILENT) {
            settings_toggle_silent();
            ui_open_settings();
        } else if (settings_sel == SETTINGS_AUTOLOCK) {
            settings_autolock_s = settings_autolock_next(settings_autolock_s);
            settings_save();
            hw_haptic_notify(0);
            ui_open_settings();
        }
        break;
    case HW_UI_MESHCORE:
        if (mesh_sel == MESH_ACT_BACK) {
            ui_open_menu();
        } else if (mesh_sel == MESH_ACT_STATUS) {
            ui_open_net(UINAV_MESHCORE);
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
    case HW_UI_AGENT_SESSIONS: {
        int n = ag_sess_vis_n;   // visible (non-dead) rows drawn by the refresh
        if (agent_sess_sel < n) {  // existing visible session → open its chat
            int si = ag_sess_row2idx[agent_sess_sel];  // row → real session index
            const char *nm = agents_session_name(agent_focus, si);
            if (nm && nm[0] && agents_session_select(agent_focus, nm)) {
                hw_haptic_notify(0);
                ui_open_agent_chat(agent_focus);
            }
        } else if (agent_sess_sel == n) {  // NEW SESSION
            if (agents_session_count(agent_focus) >= AGENT_SESSIONS_MAX) {
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
            // Back to whatever list opened the picker (the unified feed or the
            // contacts list), not the retired agents list.
            ui_go_to_screen(agent_origin);
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
            const MsgHandle &h = msglist_h[msglist_sel];
            if (!h.is_conv) {
                ui_open_notify_id(h.card_id);
            } else {
                /* A conversation row. THE SLOT MAY HAVE CHANGED HANDS since the
                 * feed was drawn: the off-loop drain mints while the list sits
                 * open, and minting on a full table recycles a slot. Opening
                 * without checking would put the user in a stranger's thread
                 * under the name they picked — one string compare closes it,
                 * the same guard the old Agents screen used. */
                int slot = h.slot;
                InboxRow probe;
                memset(&probe, 0, sizeof(probe));
                snprintf(probe.id, sizeof(probe.id), "%s", h.conv_id);
                agents_lock();
                bool same = inbox_row_matches(&probe, agents_id(slot));
                agents_unlock();
                if (!same) {
                    /* Show what is actually there now rather than opening it. */
                    ui_open_msglist();
                    break;
                }
                // A multi-session conversation opens its room picker so the user
                // chooses rather than landing silently in the last-used room; a
                // single-session one opens the chat directly (one-tap). Either
                // way the picker backs out to this feed.
                agent_origin = UINAV_MSGLIST;
                if (ui_nav_conv_open_target(agents_session_count(slot))
                        == UINAV_AGENT_SESSIONS)
                    ui_open_agent_sessions(slot);
                else
                    ui_open_agent_chat(slot);
            }
        }
        break;
    case HW_UI_NOTIFY: {
        // Chat card → open room. Severity page → Ack / Reply / Back.
        NotifyView v;
        if (notify_card_id && notify_view_by_id(notify_card_id, v, NULL, NULL) &&
            notify_is_chat(v)) {
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
    case HW_UI_CONTACTS:
        ui_contacts_open_selected();
        break;
    case HW_UI_NET:
        ui_go_to_screen(net_origin);
        break;
    case HW_UI_PAGE:
        // Click toggles the two input modes: OPEN the page for in-page scrolling,
        // or (when already open) CLOSE back to paging between pages.
        if (page_open) {
            page_open = false;
            page_scroll = 0;
            if (!ui_page_render(page_ordinal)) ui_go_clock(NULL);
        } else {
            page_open = true;
            page_scroll = 0;
            if (!ui_page_render(page_ordinal)) ui_go_clock(NULL);
        }
        break;
    case HW_UI_REPLY:
        // Encoder click = send if draft non-empty, else cancel
        if (reply_buf[0]) {
            ui_reply_submit();
        } else if (reply_mode == REPLY_MODE_WIFI_SSID ||
                   reply_mode == REPLY_MODE_WIFI_PASSWORD) {
            reply_mode = REPLY_MODE_NONE;
            wifi_pending_ssid[0] = '\0';
            reply_buf[0] = '\0';
            ui_open_wifi();
        } else if (reply_mode == REPLY_MODE_AGENT) {
            reply_mode = REPLY_MODE_NONE;
            reply_buf[0] = '\0';
            ui_open_agent_chat(agent_focus);
        } else {
            reply_mode = REPLY_MODE_NONE;
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
    // Blanked: the wheel must not relight the panel. A sitting encoder
    // (pocket, bag, desk edge) emits stray detents; each one used to restart
    // the 120 s off-timer and drain the cell. Click or a key still wakes.
    if (backlight_blanked()) return;
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
    case HW_UI_AGENT_SESSIONS: {
        int n = ag_sess_vis_n;   // visible rows; NEW at n, BACK at n+1
        int cnt = n + 2;
        if (cnt <= 0) break;
        agent_sess_sel += steps;
        while (agent_sess_sel < 0) agent_sess_sel += cnt;
        while (agent_sess_sel >= cnt) agent_sess_sel -= cnt;
        if (agents_session_count(agent_focus) >= AGENT_SESSIONS_MAX &&
            agent_sess_sel == n) {
            // full registry — NEW row disabled: skip over it
            agent_sess_sel += (steps > 0) ? 1 : -1;
            if (agent_sess_sel < 0) agent_sess_sel = cnt - 1;
            if (agent_sess_sel >= cnt) agent_sess_sel = 0;
        }
        ui_agent_sessions_refresh();
        break;
    }
    case HW_UI_AGENT_ACT: {
        int n = agent_act_row_count();
        agent_act_sel += steps;
        while (agent_act_sel < 0) agent_act_sel += n;
        while (agent_act_sel >= n) agent_act_sel -= n;
        hw_ui_show_agent_act(agent_act_sel, agents_name(agent_focus), agent_act_del_ok);
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
    case HW_UI_CONTACTS: {
        // Move the selection by |steps| CONTACT rows, skipping section headers.
        // Stops at the first/last contact rather than wrapping (headers make a
        // wrap ambiguous, and clamping matches the other lists here).
        if (contacts_count <= 0) break;
        int dir = (steps > 0) ? 1 : -1;
        int nsteps = (steps > 0) ? steps : -steps;
        int cur = contacts_sel;
        for (int s = 0; s < nsteps; s++) {
            int nx = contacts_next_contact(cur + dir, dir);
            if (nx < 0) break;   // no further contact this way: stay put
            cur = nx;
        }
        contacts_sel = cur;
        ui_contacts_render(NULL);
        break;
    }
    case HW_UI_NET: {
        // Read-only: the wheel scrolls the window. No selection to move; the
        // renderer clamps net_top, so clamp to [0, count-1] here and let it frame.
        if (net_count <= 0) break;
        net_top += steps;
        if (net_top < 0) net_top = 0;
        if (net_top > net_count - 1) net_top = net_count - 1;
        ui_net_render();
        break;
    }
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
    case HW_UI_CLOCK: {
        // Wheel from home enters page paging when there is anything to show.
        // First detent lands on page 0 (CW) or the last page (CCW). The count
        // spans the RAM hot-set AND older archived pages, so a CCW step can land
        // straight on a page pulled from the SD archive.
        // NOTE: the ordinal axis is intentionally NOT snapshot-consistent across
        // the separate nav_count->nav_at mux acquisitions here. The writer only
        // GROWS the archive range and never touches g_page_store (micron_store),
        // so a concurrent append can at worst land a fixed ordinal on an adjacent
        // archive page — never OOB, never a foreign page — and it self-corrects on
        // the next detent. No snapshot lock is needed.
        int n = g_page_store ? history_nav_page_count(g_page_store, MICRON_NS_SYSTEM) : 0;
        if (n <= 0) break;
        page_open = false;
        page_scroll = 0;
        ui_page_render(steps > 0 ? 0 : n - 1);
        break;
    }
    case HW_UI_PAGE: {
        // As in HW_UI_CLOCK: the nav_count->nav_at pair is deliberately not
        // snapshot-consistent across its two mux takes. A concurrent append only
        // grows the archive range (never mutates g_page_store), so at worst a fixed
        // ordinal shifts onto an adjacent archive page — never OOB/foreign — and
        // the next detent corrects it.
        int n = g_page_store ? history_nav_page_count(g_page_store, MICRON_NS_SYSTEM) : 0;
        if (n <= 0) { ui_go_clock(NULL); break; }
        if (page_open) {
            // Scrolling WITHIN the open page (C3 in-page scroll). One detent
            // advances MICRON_PAGE_SCROLL_STEP visual rows via the layout's own
            // multiplier — the input constant stays 1.
            page_scroll = micron_layout_apply_scroll(page_scroll, steps,
                                                     page_total_rows);
            ui_page_render(page_ordinal);
        } else {
            // Paging BETWEEN pages. Stepping before the first page returns home.
            int next = page_ordinal + steps;
            if (next < 0) { ui_go_clock(NULL); break; }
            if (next >= n) next = n - 1;
            page_scroll = 0;
            ui_page_render(next);
        }
        break;
    }
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
    // {"token":"…"}; only a genuinely EMPTY body clears the file and returns
    // the gateway calls to their legacy header-less form. A JSON body whose
    // token field is missing, non-string, or empty is a 400 — a malformed
    // provisioning call must never silently drop the stored token.
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
            if (!input["token"].is<const char *>()) {
                notify_send_error(req, 400, "token must be a JSON string");
                return;
            }
            tok = input["token"].as<const char *>();
            tok.trim();
            if (tok.length() == 0) {
                notify_send_error(req, 400,
                    "empty token string - send an empty body to clear");
                return;
            }
        }
        if (tok.length() > GW_TOKEN_MAX) {
            notify_send_error(req, 400, "token too long"); return;
        }
        // AsyncTCP task: no NVS (Preferences) and no SPIFFS writes here.
        // Update the in-RAM copy (live for /ping and reply at once), stage
        // the same value, and respond now — gw_token_persist_poll() on the
        // loop task makes it durable a tick later (NVS + SPIFFS dual write
        // on set, dual erase on clear).
        snprintf(gw_token, sizeof(gw_token), "%s", tok.c_str());
        snprintf(gw_token_stage, sizeof(gw_token_stage), "%s", tok.c_str());
        gw_token_persist_pending = true;
        event_add("gateway token %s", gw_token[0] ? "set" : "cleared");
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
    // GPIO46 is a boot-mode strap. Park it low before anything else so a
    // previous LEDC/HIGH cannot sample as the invalid 0+1 combination.
    hw_kb_park_backlight();
    // No power-hold line is driven here. The board runs from USB during bring-up.
    Serial.begin(115200);
    delay(500);
    boot_time = millis();
    setCpuFrequencyMhz(CPU_MHZ);
    boot_diag_init();  // reset reason + crash counters, first serial line out

    // Panel + I2C before storage: hw_ui_begin() touches no SPIFFS, and a
    // storage format (foreign/blank partition) must be visible on the panel
    // instead of a dead screen. The face can also say "connecting..." while
    // STA associates.
    hw_ui_begin();
    storage_begin();  // SPIFFS mount → announced format → degraded continue
    outbox_load();    // durable outgoing replies survive an offline reboot
    // Capture this device's real secrets into NVS, byte-for-byte, the moment
    // storage is up and BEFORE any consumer (auth/gw/rns/mesh) loads one. NVS
    // survives a SPIFFS format, so this is what keeps the live identity, RNS
    // address and OTA token from changing after a data-partition wipe. Idempotent
    // (a sentinel makes later boots a no-op); only run when the mount succeeded,
    // so a failed mount cannot set the sentinel over files it never read.
    if (storage_ok) secret_store_migrate_from_spiffs(SPIFFS);
    // Reclaim AW9364 from the boot pulse in hw_ui; load /backlight.json.
    backlight_begin();
    hw_input_begin();
    hw_haptic_begin();  // after Wire is up (hw_ui_begin)
    hw_sound_begin();
    hw_kb_begin();
    if (backlight_wanted() == 0) hw_kb_set_backlight(0);
    kb_layout_load(); // SPIFFS /kb_layout.txt → EN / RU PHON / RU
    settings_load();  // SPIFFS /settings.json; missing keys keep defaults
    mesh_gw_load();   // SPIFFS /mesh_gw.txt → home Heltec daemon URL
    gw_token_load();  // SPIFFS /gw_token.txt → gateway capability token
    hw_probe();
    tz_load();        // before wifi_setup(): configTzTime() needs the TZ string
    wifi_setup();     // non-blocking STA attempt; never raises an AP
    token_load();     // after wifi_setup(): needs RF up for a real hardware RNG
    skills_init();    // notify store load + route registration data
    ui_page_store_begin();  // micron system-layer page store (wheel-paged from home)
    history_begin();        // append-only SD archive + off-loop write-queue task
    notify_restore_from_archive();  // rebuild the notify ring from the archive (after mount+seed)
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
    wifi_off_poll();

    // Connection coordinator: observes the WiFi attach/lost edge, sequences
    // bring-up (clock -> WG -> RNS), debounces WG flaps, and backs up each
    // transport's own recovery if it wedges. No-op while everything is healthy.
    conn_mgr_service();

    // WiFi reconnect — one non-blocking attempt per ladder rung while offline
    // (the driver's own auto-reconnect stays off: its retries underneath the
    // scheduler caused UI stalls). Every begin — boot included — stamps
    // wifi_last_attempt_ms, so the first rung always gives the in-flight
    // association its full 30 s before any retry can interrupt it.
    // Skipped while the user turned WiFi off from the menu (mesh-only test):
    // they toggled it off, loop must not undo their choice. Also skipped
    // while the deferred reconnect machine above is mid-sequence, so a
    // background attempt cannot rotate the active profile out from under it.
    static bool was_connected = false;
    bool now_connected = WiFi.status() == WL_CONNECTED;
    if (!wifi_user_off && (wifi_net_count > 0 || wifi_ssid[0]) &&
        wifi_reconnect_state == WIFI_RECONNECT_IDLE && !now_connected &&
        millis() - wifi_last_attempt_ms >= wifi_retry_interval_ms()) {
        /* One profile attempt (~5s) so loop stays responsive. */
        if (wifi_net_count > 1) {
            wifi_net_idx = (wifi_net_idx + 1) % wifi_net_count;
            wifi_nets_set_active(wifi_net_idx);
        }
        if (wifi_retry_step < WIFI_RETRY_LADDER_STEPS - 1) wifi_retry_step++;
        Serial.printf("[wifi] reconnect try %s\n", wifi_ssid);
        event_add("wifi retry %s, next in %lus (step %d)", wifi_ssid,
                  wifi_retry_interval_ms() / 1000UL, wifi_retry_step);
        WiFi.disconnect(false, false);
        wifi_begin_active_profile();
    }
    if (now_connected != was_connected) {
        was_connected = now_connected;
        if (now_connected) {
            wifi_retry_step = 0;  /* next loss starts the ladder over */
            wifi_persist_profiles();
            /* mDNS used to be started here (begin + http/seed services).
             * It is gone, and it must stay gone until internal DRAM is free
             * again. The board boot-looped on an inbound multicast mDNS query;
             * the coredump, decoded against the matching .elf, read:
             *
             *   tcpip_thread -> ethernet_input -> ip4_input -> udp_input
             *     -> mdns_networking_lwip.c:176 receive() -> esp_log
             *     -> vprintf -> uart_write -> _lock_acquire_recursive
             *     -> lock_init_generic -> abort()
             *
             * That is not an mDNS bug. The component logged, the console lock
             * allocated its mutex lazily on first use, the allocation failed
             * for want of internal DRAM, and _lock_acquire_recursive aborts on
             * failure — it has no way to report one. Static internal RAM had
             * just gone from 58.1% to 65.0%, leaving ~114 KB for every task
             * stack, lwIP, WiFi, the SD buffers and the heap. Any unsolicited
             * packet that reached a logging path could have done it; mDNS just
             * listens on a multicast group the network keeps talking to.
             *
             * Silencing the log would not have fixed it — it would have moved
             * the abort to the next allocation. Removing the start is what
             * takes the receive path off the tcpip thread for good.
             *
             * Since then the history index, the write-queue storage and the
             * page/render buffers moved to PSRAM (src/psram_alloc.h), so the
             * 65.0% above is no longer this build's figure: with the whole RNS
             * stack linked in, static internal RAM is 61.1% (200112 B), leaving
             * ~127 KB. The pressure is lower, not gone — and a link-time figure
             * is not the number that aborted the board anyway; free internal
             * heap at the moment of the packet is.
             *
             * Before reinstating this: measure free internal DRAM at runtime
             * (esp_get_free_internal_heap_size / the RAM figure in `pio run`),
             * and only then decide. The node is reachable by IP, and the
             * fleet finds it that way. Convenience is not worth the boot loop.
             */
        }
        if (hw_ui_screen() == HW_UI_CLOCK) {
            hw_ui_invalidate_clock();
            ui_clock_paint(now_connected ? "wifi up" : "wifi lost");
        }
    }

    // Front panel: encoder + click (must run before screens consume edges).
    // Pocket lock: swallow wheel + click — keys are already silent in
    // hw_kb. Still poll+drain so detents don't queue and fire after unlock.
    hw_input_poll();
    int steps = hw_input_steps();
    bool click = hw_input_click();
    if (hw_input_long_press()) hw_kb_set_locked(!hw_kb_locked());
    if (!hw_kb_locked()) {
        if (steps) ui_on_steps(steps);
        if (click) ui_on_click();
    }

    // Keyboard → UTF-8 into reply / open compose from card.
    // ALT+CAPS cycles layout; held USER toggles full lock. Refresh badges.
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
            if (hw_kb_locked() && !settings_silent) hw_sound_notify(0);
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
        if (hw_kb_take_silent_toggle()) {
            settings_toggle_silent();
            if (hw_ui_screen() == HW_UI_SETTINGS) ui_open_settings();
            else if (hw_ui_screen() == HW_UI_CLOCK) {
                hw_ui_invalidate_clock();
                ui_clock_paint(settings_silent ? "SILENT ON" : "SILENT OFF");
            }
            ui_note_input();
        }
    }

    if (settings_autolock_due(settings_autolock_s,
                              millis() - ui_last_input_ms,
                              hw_kb_locked())) {
        hw_kb_set_locked(true);
    }

    // Idle policy owns brightness; drive pulses after the decision.
    // ORDER: hand-placed — must follow ui_backlight_idle() (this pass's
    // decision) and precede the notify-arrival block below, so a blanked
    // panel is awake and repainted before an arriving card paints (C3).
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

    // Notify expiry + coalesced SPIFFS snapshot run in the notify skill tick
    // (order-free: the arrival/sound flags consumed below are produced by the
    // HTTP handlers and mesh ingest, not by that poll). The consumption
    // blocks below stay hand-placed — see the C3 ordering note above.

    // Typed reply on its way to the gateway. ORDER: hand-placed — after the
    // keyboard drain above, so an Enter is carried on the pass that produced
    // it, and after the card has already painted its way back to the clock.
    outbox_poll();

    // Flush beacon: staged by conn_mgr_service() on the WiFi attach edge, sent
    // here on the loop task (off AsyncTCP), same deferral as the reply above.
    flush_beacon_poll();

    // Rotated gateway token: staged by POST /gw/token (AsyncTCP task),
    // persisted here on the loop task (NVS + SPIFFS), same deferral as above.
    gw_token_persist_poll();

    // Speaker + haptic FIRST — before any full-screen SPI paint can delay the
    // cue. (Boot tone worked; notify was silent when paint ran first.)
    {
        uint8_t lvl = 0;
        char src[NOTIFY_SOURCE_LEN];
        if (notify_take_sound_arrival(&lvl, src, sizeof(src))) {
            if (!settings_silent) hw_sound_notify(lvl);
            hw_haptic_notify(lvl);
            hw_sound_poll();        // prime DMA immediately
        }
    }

    // Agent thread inbound (display_force) — refresh open chat room live.
    if (display_force && hw_ui_screen() == HW_UI_AGENT_CHAT) {
        display_force = false;
        bool real_in = g_agents_real_inbound;
        g_agents_real_inbound = false;
        // A reply into the open room always jumps to the latest line.
        if (real_in) agent_scroll = -1;
        ui_agent_chat_refresh();
        // A genuine arrival (mesh C1 or WiFi) in the open room wakes the
        // panel once; a repaint alone (e.g. synthetic error line) does not.
        if (real_in) ui_note_wake();
    }

    uint32_t arrived_id = 0;
    bool got_notify_arrival = notify_take_arrival(&arrived_id);
    bool notify_chat_retry_due = notify_chat_retry_pending &&
        (long)(millis() - notify_chat_retry_at) >= 0;
    if (got_notify_arrival || display_force || notify_chat_retry_due) {
        display_force = false;
        // Room not on screen: the arrival flag must not survive to be claimed
        // by a later, unrelated repaint of the room.
        g_agents_real_inbound = false;
        NotifyView v;
        HwUiScreen scr = hw_ui_screen();
        bool on_clock = (scr == HW_UI_CLOCK);
        notify_take_chat_completions(scr);
        if (got_notify_arrival || notify_chat_retry_due) {
            bool retry_after_drain = notify_reconcile_pending_chats(scr);
            notify_chat_retry_pending = retry_after_drain;
            if (notify_chat_retry_pending) notify_chat_retry_at = millis() + 1000;
        }
        if (arrived_id && notify_view_by_id(arrived_id, v, NULL, NULL)) {
            NotifyChatResolution resolution = notify_chat_candidate(v);
            bool is_pending_chat = resolution.conversation >= 0 &&
                                   notify_chat_normalized[0] &&
                                   notify_event_id_valid(&v.event_id);
            if (is_pending_chat) {
                /* Body is already queued into the thread. The door card is the
                 * notification: pop the CHAT invite, do not swallow it. */
                bool in_this_chat = (scr == HW_UI_AGENT_CHAT &&
                                     agent_focus == resolution.conversation);
                bool can_pop = on_clock || scr == HW_UI_MENU ||
                               scr == HW_UI_MSGLIST || scr == HW_UI_INFO ||
                               scr == HW_UI_WIFI || scr == HW_UI_WIFI_LIST ||
                               scr == HW_UI_NET || scr == HW_UI_SETTINGS;
                if (!in_this_chat && can_pop) {
                    notify_card_id = arrived_id;
                    const char *nm = (resolution.conversation >= 0)
                        ? agents_name(resolution.conversation) : v.source;
                    const char *teaser = notify_chat_normalized[0]
                        ? notify_chat_normalized : v.body;
                    hw_ui_show_agent_invite(nm, teaser, notify_unread_count());
                    ui_note_wake();
                }
            } else if (on_clock || scr == HW_UI_MENU || scr == HW_UI_MSGLIST ||
                       scr == HW_UI_INFO) {
                // Real message from any service: severity colours (info/warn/crit).
                notify_card_id = arrived_id;
                snprintf(reply_title, sizeof(reply_title), "%s", v.title);
                hw_ui_show_notify(notify_level_name(v.level), v.source, v.title,
                                  v.body, notify_unread_count(),
                                  outbox_reply_status(v.key));
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

    // ORDER: hand-placed — refill the sound DMA after the arrival paints and
    // before the 1 Hz clock repaint below, so a long paint cannot starve an
    // in-flight cue (hw_sound is a hardware module, not a Skill).
    hw_sound_poll();

    // Progress job reaping (TTL) runs in the progress skill tick.

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

    // Skill ticks: notify (expiry + snapshot), progress (reaping), meshcore
    // (radio), wireguard, gps — registration order.
    for (int i = 0; i < g_skill_count; i++) {
        if (g_skills[i]->tick) g_skills[i]->tick();
    }

    // PING screen sequence — after the skill ticks so the meshcore poll above
    // has already had its chance to receive the pong this pass.
    ui_mesh_ping_poll();

    delay(10);
}
