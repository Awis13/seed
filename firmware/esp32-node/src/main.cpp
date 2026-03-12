// Seed Node — ESP32-S3 firmware (grown from seed)
// HTTP API for GPIO, I2C, config, deploy management
// For Heltec WiFi LoRa 32 V3
//
// Compatible HTTP API with Pi Zero W server (pi/server.c)
// WiFi AP+STA, mDNS, SPIFFS, Bearer auth, OLED status
//
// Endpoints:
//   GET  /health            — health check (no auth)
//   GET  /capabilities      — node capabilities description
//   GET  /config.md         — read node config (SPIFFS)
//   POST /config.md         — write node config
//   POST /gpio/{pin}/mode   — set pin mode
//   POST /gpio/{pin}/write  — write pin value
//   GET  /gpio/{pin}/read   — read pin value
//   GET  /gpio/status       — status of all configured pins
//   POST /i2c/scan          — scan I2C buses
//   POST /i2c/{addr}/write  — write bytes to I2C
//   GET  /i2c/{addr}/read/{count} — read bytes from I2C
//   POST /deploy            — upload and run script
//   GET  /deploy            — script status
//   DELETE /deploy          — stop and remove script
//   GET  /deploy/logs       — deployed script logs (ring buffer)
//   GET  /live.md           — runtime state markdown (SPIFFS)
//   POST /live.md           — write runtime state
//   GET  /events            — event log with timestamps (?since=<unix_ts>)
//   GET  /mesh              — mDNS discovery of neighboring Seed nodes
//   POST /auth/token        — change auth token
//   GET  /firmware/version  — firmware version, uptime, partition, heap
//   POST /firmware/upload   — upload OTA firmware (streaming)
//   POST /firmware/apply    — reboot into new firmware
//   POST /firmware/confirm  — confirm firmware (cancel rollback)
//   POST /firmware/rollback — rollback to previous firmware
//   GET  /wg/status         — WireGuard tunnel status
//   POST /wg/config         — configure WG interface (address, private_key)
//   POST /wg/peer           — configure WG peer (public_key, endpoint, port)
//   DELETE /wg/peer         — remove WG peer
//   GET  /wg/peers          — list WG peers (max 1 on ESP32)
//   POST /wg/restart        — restart WG tunnel

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
#include <WireGuard-ESP32.h>
#include <mbedtls/base64.h>

// Curve25519 — for deriving public key from private key
// Using x25519 from WireGuard-ESP32 library (crypto/refc/x25519.h)
extern "C" {
    extern const unsigned char X25519_BASE_POINT[32];
    int x25519(unsigned char out[32], const unsigned char scalar[32],
               const unsigned char base[32], int clamp);
}

// ===== Heltec V3 Board Pins =====
#define LORA_NSS    8
#define LORA_RST    12
#define LORA_BUSY   13
#define LORA_DIO1   14
#define LORA_MOSI   10
#define LORA_MISO   11
#define LORA_SCK    9
#define VEXT_PIN    36
#define LED_PIN     35
#define ADC_CTRL    37
#define OLED_RST    21
#define OLED_SDA    17
#define OLED_SCL    18

// I2C buses
#define I2C_PRIMARY_SDA   17   // Shared with OLED
#define I2C_PRIMARY_SCL   18
#define I2C_EXTERNAL_SDA  5    // For external sensors
#define I2C_EXTERNAL_SCL  6

// OLED SSD1306
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

// ===== Configuration =====
#define FIRMWARE_VERSION    "1.0.0"
#define HTTP_PORT           8080
#define AP_PASSWORD         "seed1313"
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"
#define DEPLOY_SCRIPT_FILE  "/deploy.txt"
#define LIVE_MD_FILE        "/live.md"
#define WG_CONFIG_FILE      "/wg_config.json"
#define MAX_SCRIPT_SIZE     4096

// ===== Deploy Log Ring Buffer =====
#define DEPLOY_LOG_SIZE     4096
static char deploy_log_buf[DEPLOY_LOG_SIZE];
static int deploy_log_pos = 0;  // Current write position
static bool deploy_log_wrapped = false;  // Buffer has wrapped at least once
static SemaphoreHandle_t deploy_log_mutex = NULL;

static void deploy_log(const char *fmt, ...) {
    char line[192];
    int len = snprintf(line, sizeof(line), "[%lu] ", millis());
    va_list ap;
    va_start(ap, fmt);
    len += vsnprintf(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;  // clamp if truncated
    // Append \n if missing
    if (len > 0 && line[len - 1] != '\n' && len < (int)sizeof(line) - 1) {
        line[len++] = '\n';
        line[len] = '\0';
    }

    if (deploy_log_mutex) xSemaphoreTake(deploy_log_mutex, portMAX_DELAY);
    for (int i = 0; i < len && line[i]; i++) {
        deploy_log_buf[deploy_log_pos] = line[i];
        deploy_log_pos = (deploy_log_pos + 1) % DEPLOY_LOG_SIZE;
        if (deploy_log_pos == 0) deploy_log_wrapped = true;
    }
    if (deploy_log_mutex) xSemaphoreGive(deploy_log_mutex);
}

// ===== Events Ring Buffer =====
#define MAX_EVENTS          128
#define EVENT_MSG_LEN       128

struct EventEntry {
    unsigned long timestamp;  // millis() or Unix time if NTP available
    char message[EVENT_MSG_LEN];
};

static EventEntry events_buf[MAX_EVENTS];
static int events_head = 0;
static int events_count = 0;
static SemaphoreHandle_t events_mutex = NULL;

static void event_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (events_mutex) xSemaphoreTake(events_mutex, portMAX_DELAY);
    EventEntry *e = &events_buf[events_head];
    // Use Unix time if WiFi connected (NTP), otherwise millis()/1000
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000) {
        e->timestamp = (unsigned long)tv.tv_sec;
    } else {
        e->timestamp = millis() / 1000;  // seconds since boot
    }
    vsnprintf(e->message, EVENT_MSG_LEN, fmt, ap);
    events_head = (events_head + 1) % MAX_EVENTS;
    if (events_count < MAX_EVENTS) events_count++;
    if (events_mutex) xSemaphoreGive(events_mutex);
    va_end(ap);
}

// Safe GPIO pins for external use (Heltec V3)
static const int SAFE_GPIO_PINS[] = {1, 2, 4, 5, 6, 7, 19, 20, 47, 48};
static const int NUM_SAFE_PINS = sizeof(SAFE_GPIO_PINS) / sizeof(SAFE_GPIO_PINS[0]);

// ===== GPIO State =====
#define MAX_CONFIGURED_PINS 10

struct GpioPin {
    int pin;
    bool configured;
    bool is_output;  // true = output, false = input
};

static GpioPin gpio_configured[MAX_CONFIGURED_PINS];
static int gpio_count = 0;

// ===== Deploy State =====
static TaskHandle_t deploy_task_handle = NULL;
static volatile bool deploy_running = false;
static volatile bool deploy_stop_requested = false;  // Stop request flag
static String deploy_script = "";
// deploy_mutex protects: deploy_script, deploy_task_handle, deploy_running.
// Lock mutex before reading/writing these variables.
// deploy_stop_requested is volatile, atomic access, no mutex needed.
static SemaphoreHandle_t deploy_mutex = NULL;

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
static String auth_token = "";
static String ap_ssid = "";
static String mdns_name = "";
static unsigned long boot_time = 0;

// Mutex for Wire (I2C primary bus), shared between OLED and HTTP handlers
static SemaphoreHandle_t wire_mutex = NULL;

// WiFi STA credentials
static String wifi_ssid = "";
static String wifi_pass = "";

// ===== OTA Firmware Update State =====
static bool firmware_confirmed = false;        // Firmware confirmed (auto-confirm or manual)
static bool firmware_confirm_attempted = false; // One-time auto-confirm attempt
static bool ota_in_progress = false;           // Block concurrent uploads
static bool ota_upload_started = false;        // Update.begin() called successfully
static bool ota_upload_ok = false;             // OTA uploaded and verified
static bool ota_upload_error = false;          // Error during OTA upload
static char ota_upload_error_msg[128] = "";    // OTA error message (fixed buf, not String during OTA)
static size_t ota_bytes_written = 0;           // Bytes written
static volatile bool pending_restart = false;  // Deferred restart flag (for apply/rollback)
static volatile bool pending_rollback = false; // Rollback instead of normal restart

// ===== WireGuard State =====
static WireGuard wg;
static bool wg_running = false;
static bool wg_restart_needed = false;
static bool wg_init_done = false;  // NTP synced, WG start attempted
static char wg_public_key[45] = "";  // Base64-encoded public key (44 chars + \0)

// OLED framebuffer — simple text display (no heavy libraries)
// SSD1306 init and rendering via raw I2C
static bool oled_available = false;

// ===== Forward declarations =====
static bool gpio_is_valid_pin(int pin);
static int gpio_find_configured(int pin);
static void deploy_task_func(void *param);
static void deploy_stop();
static void deploy_stop_and_wait();
static void oled_init();
static void oled_update();
static void oled_clear();
static void oled_text(int row, const char *text);
static void oled_send_cmd(uint8_t cmd);
static void oled_send_data(const uint8_t *data, size_t len);

// ===== Minimal 5x7 font (ASCII 32-126) =====
// Each char is 5 bytes (5 columns, 7 rows, LSB=top)
static const uint8_t font5x7[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00, // 32 space
    0x00,0x00,0x5F,0x00,0x00, // 33 !
    0x00,0x07,0x00,0x07,0x00, // 34 "
    0x14,0x7F,0x14,0x7F,0x14, // 35 #
    0x24,0x2A,0x7F,0x2A,0x12, // 36 $
    0x23,0x13,0x08,0x64,0x62, // 37 %
    0x36,0x49,0x55,0x22,0x50, // 38 &
    0x00,0x05,0x03,0x00,0x00, // 39 '
    0x00,0x1C,0x22,0x41,0x00, // 40 (
    0x00,0x41,0x22,0x1C,0x00, // 41 )
    0x08,0x2A,0x1C,0x2A,0x08, // 42 *
    0x08,0x08,0x3E,0x08,0x08, // 43 +
    0x00,0x50,0x30,0x00,0x00, // 44 ,
    0x08,0x08,0x08,0x08,0x08, // 45 -
    0x00,0x60,0x60,0x00,0x00, // 46 .
    0x20,0x10,0x08,0x04,0x02, // 47 /
    0x3E,0x51,0x49,0x45,0x3E, // 48 0
    0x00,0x42,0x7F,0x40,0x00, // 49 1
    0x42,0x61,0x51,0x49,0x46, // 50 2
    0x21,0x41,0x45,0x4B,0x31, // 51 3
    0x18,0x14,0x12,0x7F,0x10, // 52 4
    0x27,0x45,0x45,0x45,0x39, // 53 5
    0x3C,0x4A,0x49,0x49,0x30, // 54 6
    0x01,0x71,0x09,0x05,0x03, // 55 7
    0x36,0x49,0x49,0x49,0x36, // 56 8
    0x06,0x49,0x49,0x29,0x1E, // 57 9
    0x00,0x36,0x36,0x00,0x00, // 58 :
    0x00,0x56,0x36,0x00,0x00, // 59 ;
    0x00,0x08,0x14,0x22,0x41, // 60 <
    0x14,0x14,0x14,0x14,0x14, // 61 =
    0x41,0x22,0x14,0x08,0x00, // 62 >
    0x02,0x01,0x51,0x09,0x06, // 63 ?
    0x32,0x49,0x79,0x41,0x3E, // 64 @
    0x7E,0x11,0x11,0x11,0x7E, // 65 A
    0x7F,0x49,0x49,0x49,0x36, // 66 B
    0x3E,0x41,0x41,0x41,0x22, // 67 C
    0x7F,0x41,0x41,0x22,0x1C, // 68 D
    0x7F,0x49,0x49,0x49,0x41, // 69 E
    0x7F,0x09,0x09,0x01,0x01, // 70 F
    0x3E,0x41,0x41,0x51,0x32, // 71 G
    0x7F,0x08,0x08,0x08,0x7F, // 72 H
    0x00,0x41,0x7F,0x41,0x00, // 73 I
    0x20,0x40,0x41,0x3F,0x01, // 74 J
    0x7F,0x08,0x14,0x22,0x41, // 75 K
    0x7F,0x40,0x40,0x40,0x40, // 76 L
    0x7F,0x02,0x04,0x02,0x7F, // 77 M
    0x7F,0x04,0x08,0x10,0x7F, // 78 N
    0x3E,0x41,0x41,0x41,0x3E, // 79 O
    0x7F,0x09,0x09,0x09,0x06, // 80 P
    0x3E,0x41,0x51,0x21,0x5E, // 81 Q
    0x7F,0x09,0x19,0x29,0x46, // 82 R
    0x46,0x49,0x49,0x49,0x31, // 83 S
    0x01,0x01,0x7F,0x01,0x01, // 84 T
    0x3F,0x40,0x40,0x40,0x3F, // 85 U
    0x1F,0x20,0x40,0x20,0x1F, // 86 V
    0x7F,0x20,0x18,0x20,0x7F, // 87 W
    0x63,0x14,0x08,0x14,0x63, // 88 X
    0x03,0x04,0x78,0x04,0x03, // 89 Y
    0x61,0x51,0x49,0x45,0x43, // 90 Z
    0x00,0x00,0x7F,0x41,0x41, // 91 [
    0x02,0x04,0x08,0x10,0x20, // 92 backslash
    0x41,0x41,0x7F,0x00,0x00, // 93 ]
    0x04,0x02,0x01,0x02,0x04, // 94 ^
    0x40,0x40,0x40,0x40,0x40, // 95 _
    0x00,0x01,0x02,0x04,0x00, // 96 `
    0x20,0x54,0x54,0x54,0x78, // 97 a
    0x7F,0x48,0x44,0x44,0x38, // 98 b
    0x38,0x44,0x44,0x44,0x20, // 99 c
    0x38,0x44,0x44,0x48,0x7F, // 100 d
    0x38,0x54,0x54,0x54,0x18, // 101 e
    0x08,0x7E,0x09,0x01,0x02, // 102 f
    0x08,0x14,0x54,0x54,0x3C, // 103 g
    0x7F,0x08,0x04,0x04,0x78, // 104 h
    0x00,0x44,0x7D,0x40,0x00, // 105 i
    0x20,0x40,0x44,0x3D,0x00, // 106 j
    0x00,0x7F,0x10,0x28,0x44, // 107 k
    0x00,0x41,0x7F,0x40,0x00, // 108 l
    0x7C,0x04,0x18,0x04,0x78, // 109 m
    0x7C,0x08,0x04,0x04,0x78, // 110 n
    0x38,0x44,0x44,0x44,0x38, // 111 o
    0x7C,0x14,0x14,0x14,0x08, // 112 p
    0x08,0x14,0x14,0x18,0x7C, // 113 q
    0x7C,0x08,0x04,0x04,0x08, // 114 r
    0x48,0x54,0x54,0x54,0x20, // 115 s
    0x04,0x3F,0x44,0x40,0x20, // 116 t
    0x3C,0x40,0x40,0x20,0x7C, // 117 u
    0x1C,0x20,0x40,0x20,0x1C, // 118 v
    0x3C,0x40,0x30,0x40,0x3C, // 119 w
    0x44,0x28,0x10,0x28,0x44, // 120 x
    0x0C,0x50,0x50,0x50,0x3C, // 121 y
    0x44,0x64,0x54,0x4C,0x44, // 122 z
    0x00,0x08,0x36,0x41,0x00, // 123 {
    0x00,0x00,0x7F,0x00,0x00, // 124 |
    0x00,0x41,0x36,0x08,0x00, // 125 }
    0x08,0x08,0x2A,0x1C,0x08, // 126 ~
};

// OLED framebuffer — 8 pages x 128 bytes = 1024 bytes
static uint8_t oled_fb[8][128];

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

// ===== Auth =====

static void token_load() {
    auth_token = read_spiffs_file(TOKEN_FILE);
    auth_token.trim();

    if (auth_token.length() == 0) {
        // Generate random 32-hex token
        char buf[33];
        for (int i = 0; i < 16; i++) {
            snprintf(buf + i * 2, 3, "%02x", (uint8_t)esp_random());
        }
        buf[32] = '\0';
        auth_token = String(buf);
        write_spiffs_file(TOKEN_FILE, auth_token);
        Serial.printf("Token generated: %s\n", auth_token.c_str());
    } else {
        Serial.println("Token loaded from SPIFFS");
    }
}

// Constant-time string comparison to prevent timing attacks
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

// Middleware-like auth check. Returns true if request is authorized.
static bool require_auth(AsyncWebServerRequest *request) {
    if (check_auth(request)) return true;
    request->send(401, "application/json",
        "{\"error\":\"Authorization: Bearer <token> required\"}");
    return false;
}

// ===== GPIO =====

static bool gpio_is_valid_pin(int pin) {
    for (int i = 0; i < NUM_SAFE_PINS; i++) {
        if (SAFE_GPIO_PINS[i] == pin) return true;
    }
    return false;
}

static int gpio_find_configured(int pin) {
    for (int i = 0; i < gpio_count; i++) {
        if (gpio_configured[i].pin == pin && gpio_configured[i].configured)
            return i;
    }
    return -1;
}

static void gpio_track_pin(int pin, bool is_output) {
    int idx = gpio_find_configured(pin);
    if (idx >= 0) {
        gpio_configured[idx].is_output = is_output;
        return;
    }
    if (gpio_count < MAX_CONFIGURED_PINS) {
        gpio_configured[gpio_count].pin = pin;
        gpio_configured[gpio_count].configured = true;
        gpio_configured[gpio_count].is_output = is_output;
        gpio_count++;
    }
}

// ===== I2C =====

static void i2c_init() {
    // Primary bus — shared with OLED (GPIO 17/18)
    Wire.begin(I2C_PRIMARY_SDA, I2C_PRIMARY_SCL);
    Wire.setClock(400000);

    // External bus — for user sensors (GPIO 5/6)
    Wire1.begin(I2C_EXTERNAL_SDA, I2C_EXTERNAL_SCL);
    Wire1.setClock(400000);
}

// Scan one bus, add found addresses to JSON array
static void i2c_scan_bus(TwoWire &wire, JsonArray &devices, const char *bus_name) {
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        wire.beginTransmission(addr);
        uint8_t err = wire.endTransmission();
        if (err == 0) {
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", addr);
            devices.add(String(hex));
            event_add("i2c device detected: 0x%02X on %s", addr, bus_name);
        }
    }
}

// Write bytes. hex_data = "0x00 0x10 0xFF" or "00 10 FF"
static bool i2c_parse_hex(const String &hex_data, uint8_t *bytes, int &nbytes, int max_bytes) {
    nbytes = 0;
    const char *p = hex_data.c_str();
    while (*p && nbytes < max_bytes) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        // Skip "0x" or "0X" prefix
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        unsigned int val;
        if (sscanf(p, "%2x", &val) != 1) break;
        bytes[nbytes++] = (uint8_t)val;
        // Skip processed characters
        while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++;
    }
    return nbytes > 0;
}

// Select bus by address — primary (Wire) or external (Wire1)
// OLED at 0x3C uses primary bus; everything else tries both
static TwoWire& i2c_select_bus(uint8_t addr) {
    // Check primary first
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return Wire;
    // Then external
    Wire1.beginTransmission(addr);
    if (Wire1.endTransmission() == 0) return Wire1;
    // Default to external (user bus)
    return Wire1;
}

// ===== OLED Display (SSD1306 128x64 via I2C) =====

static void oled_send_cmd(uint8_t cmd) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x00);  // Co=0, D/C#=0 (command)
    Wire.write(cmd);
    Wire.endTransmission();
}

static void oled_send_data(const uint8_t *data, size_t len) {
    // Send data in 16-byte chunks (I2C buffer limit)
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > 16) chunk = 16;
        Wire.beginTransmission(OLED_ADDR);
        Wire.write(0x40);  // Co=0, D/C#=1 (data)
        for (size_t i = 0; i < chunk; i++) {
            Wire.write(data[offset + i]);
        }
        Wire.endTransmission();
        offset += chunk;
    }
}

static void oled_init() {
    // Reset OLED
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(10);
    digitalWrite(OLED_RST, HIGH);
    delay(10);

    // Check OLED presence
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[OLED] Not found at 0x3C");
        oled_available = false;
        return;
    }

    // SSD1306 init sequence
    oled_send_cmd(0xAE);  // Display OFF
    oled_send_cmd(0xD5);  // Set display clock
    oled_send_cmd(0x80);
    oled_send_cmd(0xA8);  // Set multiplex ratio
    oled_send_cmd(0x3F);  // 64
    oled_send_cmd(0xD3);  // Set display offset
    oled_send_cmd(0x00);
    oled_send_cmd(0x40);  // Set start line = 0
    oled_send_cmd(0x8D);  // Charge pump
    oled_send_cmd(0x14);  // Enable
    oled_send_cmd(0x20);  // Memory mode
    oled_send_cmd(0x00);  // Horizontal addressing
    oled_send_cmd(0xA1);  // Segment remap (flip horizontal)
    oled_send_cmd(0xC8);  // COM scan direction (flip vertical)
    oled_send_cmd(0xDA);  // COM pins
    oled_send_cmd(0x12);
    oled_send_cmd(0x81);  // Contrast
    oled_send_cmd(0xCF);
    oled_send_cmd(0xD9);  // Pre-charge period
    oled_send_cmd(0xF1);
    oled_send_cmd(0xDB);  // VCOMH deselect
    oled_send_cmd(0x40);
    oled_send_cmd(0xA4);  // Entire display ON (resume)
    oled_send_cmd(0xA6);  // Normal display (not inverted)
    oled_send_cmd(0xAF);  // Display ON

    oled_available = true;
    oled_clear();
    Serial.println("[OLED] Initialized");
}

static void oled_clear() {
    memset(oled_fb, 0, sizeof(oled_fb));
}

// Draw one character into framebuffer
static void oled_draw_char(int x, int page, char c) {
    if (c < 32 || c > 126) c = '?';
    int idx = (c - 32) * 5;
    for (int col = 0; col < 5; col++) {
        if (x + col < OLED_WIDTH) {
            oled_fb[page][x + col] = pgm_read_byte(&font5x7[idx + col]);
        }
    }
}

// Write text to row (page 0-7)
static void oled_text(int row, const char *text) {
    // Clear row
    memset(oled_fb[row], 0, OLED_WIDTH);

    int x = 0;
    for (int i = 0; text[i] && x < OLED_WIDTH - 5; i++) {
        oled_draw_char(x, row, text[i]);
        x += 6;  // 5px char + 1px gap
    }
}

// Flush framebuffer to display
static void oled_flush() {
    if (!oled_available) return;

    // Set addressing to full area
    oled_send_cmd(0x21);  // Column address
    oled_send_cmd(0);     // Start
    oled_send_cmd(127);   // End
    oled_send_cmd(0x22);  // Page address
    oled_send_cmd(0);     // Start
    oled_send_cmd(7);     // End

    for (int page = 0; page < 8; page++) {
        oled_send_data(oled_fb[page], OLED_WIDTH);
    }
}

static void oled_update() {
    if (!oled_available) return;
    // Take Wire mutex since OLED is on primary I2C bus
    if (xSemaphoreTake(wire_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    char line[22];  // 128px / 6px per char = ~21 chars

    oled_clear();

    // Row 0: Seed + name
    snprintf(line, sizeof(line), "Seed %s", ap_ssid.c_str() + 5);  // Skip "Seed-"
    oled_text(0, line);

    // Row 1: empty (separator)

    // Row 2: WiFi status
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(line, sizeof(line), "WiFi: %s", WiFi.SSID().c_str());
    } else {
        snprintf(line, sizeof(line), "WiFi: AP only");
    }
    oled_text(2, line);

    // Row 3: IP
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(line, sizeof(line), "IP: %s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(line, sizeof(line), "IP: 192.168.4.1");
    }
    oled_text(3, line);

    // Row 4: port
    snprintf(line, sizeof(line), "Port: %d", HTTP_PORT);
    oled_text(4, line);

    // Row 5: GPIO configured
    snprintf(line, sizeof(line), "GPIO: %d pins", gpio_count);
    oled_text(5, line);

    // Row 6: Deploy status
    if (deploy_running) {
        oled_text(6, "Deploy: RUNNING");
    } else {
        oled_text(6, "Deploy: idle");
    }

    // Row 7: Uptime + firmware version
    unsigned long uptime = (millis() - boot_time) / 1000;
    snprintf(line, sizeof(line), "v%s Up:%lus", FIRMWARE_VERSION, uptime);
    oled_text(7, line);

    oled_flush();
    xSemaphoreGive(wire_mutex);
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

    // AP+STA mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid.c_str(), AP_PASSWORD);
    delay(100);

    Serial.printf("[WiFi] AP started: %s (password: %s)\n", ap_ssid.c_str(), AP_PASSWORD);
    Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Connect to saved network
    wifi_load_config();
    if (wifi_ssid.length() > 0) {
        Serial.printf("[WiFi] Connecting to: %s\n", wifi_ssid.c_str());
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

        // Wait for connection (max 10 sec)
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            // NTP for correct Unix timestamps in events
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("[NTP] Time sync started");
        } else {
            Serial.println("\n[WiFi] Connection failed, AP-only mode");
        }
    }

    // mDNS
    if (MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("seed", "tcp", HTTP_PORT);
        Serial.printf("[mDNS] %s.local\n", mdns_name.c_str());
    }
}

// ===== WireGuard =====

// Validate WG key: exactly 44 chars, only [A-Za-z0-9+/=]
static bool wg_validate_key(const char *key) {
    if (!key) return false;
    int len = strlen(key);
    if (len != 44) return false;
    for (int i = 0; i < len; i++) {
        char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    }
    return true;
}

// Validate IP address: only [0-9.]
static bool wg_validate_ip(const char *ip) {
    if (!ip) return false;
    for (int i = 0; ip[i]; i++) {
        char c = ip[i];
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    }
    return true;
}

// Validate allowed_ips: only [0-9./,]
static bool wg_validate_allowed_ips(const char *ips) {
    if (!ips) return false;
    for (int i = 0; ips[i]; i++) {
        char c = ips[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == '/' || c == ',')) return false;
    }
    return true;
}

// Derive public key from private key via Curve25519
// Private key is base64, result in wg_public_key (global)
static bool wg_derive_public_key(const char *private_key_b64) {
    uint8_t privkey[32];
    size_t privkey_len = 0;

    // Decode base64 private key
    int ret = mbedtls_base64_decode(privkey, sizeof(privkey), &privkey_len,
                                     (const unsigned char *)private_key_b64,
                                     strlen(private_key_b64));
    if (ret != 0 || privkey_len != 32) {
        wg_public_key[0] = '\0';
        return false;
    }

    // Curve25519 scalar multiplication with base point (clamp=1 for WG compatibility)
    uint8_t pubkey[32];
    x25519(pubkey, privkey, X25519_BASE_POINT, 1);

    // Wipe private key from stack
    memset(privkey, 0, sizeof(privkey));

    // Encode to base64
    size_t pubkey_b64_len = 0;
    ret = mbedtls_base64_encode((unsigned char *)wg_public_key, sizeof(wg_public_key),
                                 &pubkey_b64_len, pubkey, 32);
    memset(pubkey, 0, sizeof(pubkey));
    if (ret != 0) {
        wg_public_key[0] = '\0';
        return false;
    }
    wg_public_key[pubkey_b64_len] = '\0';
    return true;
}

// Load WG config from SPIFFS
static bool wg_load_config(JsonDocument &doc) {
    String json = read_spiffs_file(WG_CONFIG_FILE);
    if (json.length() == 0) return false;
    return deserializeJson(doc, json) == DeserializationError::Ok;
}

// Save WG config to SPIFFS
static bool wg_save_config(const JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    return write_spiffs_file(WG_CONFIG_FILE, json);
}

// Start WG tunnel (call after WiFi + NTP)
static void wg_start() {
    JsonDocument doc;
    if (!wg_load_config(doc)) {
        Serial.println("[WG] No config found");
        return;
    }

    const char *address = doc["address"];
    const char *private_key = doc["private_key"];
    if (!address || !private_key) {
        Serial.println("[WG] Config missing address or private_key");
        return;
    }

    // Check peer exists
    if (!doc["peer"].is<JsonObject>() || !doc["peer"]["public_key"] || !doc["peer"]["endpoint"]) {
        Serial.println("[WG] No peer configured");
        return;
    }

    const char *peer_pubkey = doc["peer"]["public_key"];
    const char *peer_endpoint = doc["peer"]["endpoint"];
    uint16_t peer_port = doc["peer"]["port"] | 51820;

    // Derive public key
    wg_derive_public_key(private_key);

    IPAddress local_ip;
    if (!local_ip.fromString(address)) {
        Serial.printf("[WG] Invalid address: %s\n", address);
        return;
    }

    Serial.printf("[WG] Starting tunnel: %s -> %s:%d\n", address, peer_endpoint, peer_port);

    bool ok = wg.begin(local_ip, private_key, peer_endpoint, peer_pubkey, peer_port);
    if (ok) {
        wg_running = true;
        wg_restart_needed = false;
        event_add("wg tunnel up: %s -> %s:%d", address, peer_endpoint, peer_port);
        Serial.println("[WG] Tunnel started");
    } else {
        Serial.println("[WG] Failed to start tunnel");
        event_add("wg tunnel failed to start");
    }
}

// Stop WG tunnel
static void wg_stop() {
    if (wg_running) {
        wg.end();
        wg_running = false;
        event_add("wg tunnel stopped");
        Serial.println("[WG] Tunnel stopped");
    }
}

// ===== Deploy Engine =====
// Simple command interpreter running in a FreeRTOS task

static void deploy_execute_line(const String &line) {
    String trimmed = line;
    trimmed.trim();

    // Skip empty lines and comments
    if (trimmed.length() == 0 || trimmed.startsWith("#")) return;

    // gpio <pin> mode <input|output>
    if (trimmed.startsWith("gpio ")) {
        int pin = 0;
        char action[16] = "";
        char arg[16] = "";
        if (sscanf(trimmed.c_str(), "gpio %d %15s %15s", &pin, action, arg) < 2) return;

        if (!gpio_is_valid_pin(pin)) {
            Serial.printf("[Deploy] Invalid pin %d\n", pin);
            return;
        }

        if (strcmp(action, "mode") == 0) {
            bool is_out = (strcmp(arg, "output") == 0);
            pinMode(pin, is_out ? OUTPUT : INPUT);
            gpio_track_pin(pin, is_out);
            Serial.printf("[Deploy] GPIO %d mode %s\n", pin, arg);
            deploy_log("gpio %d mode %s -> ok", pin, arg);
            event_add("gpio %d mode set to %s", pin, arg);
        } else if (strcmp(action, "write") == 0) {
            int val = atoi(arg);
            if (gpio_find_configured(pin) < 0) {
                Serial.printf("[Deploy] GPIO %d not configured\n", pin);
                deploy_log("gpio %d write %s -> err: not configured", pin, arg);
                return;
            }
            digitalWrite(pin, val ? HIGH : LOW);
            Serial.printf("[Deploy] GPIO %d = %d\n", pin, val);
            deploy_log("gpio %d write %d -> ok", pin, val);
            event_add("gpio %d set to %d", pin, val);
        } else if (strcmp(action, "read") == 0) {
            if (gpio_find_configured(pin) < 0) {
                Serial.printf("[Deploy] GPIO %d not configured\n", pin);
                deploy_log("gpio %d read -> err: not configured", pin);
                return;
            }
            int val = digitalRead(pin);
            Serial.printf("[Deploy] GPIO %d reads %d\n", pin, val);
            deploy_log("gpio %d read -> %d", pin, val);
        }
        return;
    }

    // i2c <addr> write <bytes>
    if (trimmed.startsWith("i2c ")) {
        int addr = 0;
        char action[16] = "";
        char rest[256] = "";
        if (sscanf(trimmed.c_str(), "i2c %i %15s %255[^\n]", &addr, action, rest) < 2) return;

        if (addr < 0x03 || addr > 0x77) return;

        if (strcmp(action, "write") == 0) {
            uint8_t bytes[128];
            int nbytes = 0;
            if (i2c_parse_hex(String(rest), bytes, nbytes, 128) && nbytes > 0) {
                TwoWire &bus = i2c_select_bus(addr);
                bus.beginTransmission(addr);
                bus.write(bytes, nbytes);
                bus.endTransmission();
                Serial.printf("[Deploy] I2C 0x%02X write %d bytes\n", addr, nbytes);
                deploy_log("i2c 0x%02X write %d bytes -> ok", addr, nbytes);
            }
        } else if (strcmp(action, "read") == 0) {
            int count = atoi(rest);
            if (count <= 0 || count > 255) count = 1;
            TwoWire &bus = i2c_select_bus(addr);
            bus.requestFrom((uint8_t)addr, (uint8_t)count);
            int nread = 0;
            Serial.printf("[Deploy] I2C 0x%02X read:", addr);
            while (bus.available()) {
                Serial.printf(" %02X", bus.read());
                nread++;
            }
            Serial.println();
            deploy_log("i2c 0x%02X read %d bytes -> ok", addr, nread);
        }
        return;
    }

    // sleep <ms>
    if (trimmed.startsWith("sleep ")) {
        int ms = atoi(trimmed.c_str() + 6);
        if (ms > 0 && ms <= 60000) {
            deploy_log("sleep %d -> ok", ms);
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
        return;
    }

    // loop — restart handled externally
    if (trimmed == "loop") return;  // Handled in deploy_task_func

    Serial.printf("[Deploy] Unknown command: %s\n", trimmed.c_str());
    deploy_log("unknown: %s -> err", trimmed.c_str());
}

static void deploy_task_func(void *param) {
    (void)param;
    deploy_running = true;
    deploy_stop_requested = false;

    Serial.println("[Deploy] Script started");
    event_add("deploy started");

    while (!deploy_stop_requested) {
        // Copy script under mutex
        xSemaphoreTake(deploy_mutex, portMAX_DELAY);
        String script = deploy_script;
        xSemaphoreGive(deploy_mutex);

        bool has_loop = false;

        // Parse lines
        int start = 0;
        int len = script.length();
        while (start < len && !deploy_stop_requested) {
            int nl = script.indexOf('\n', start);
            if (nl < 0) nl = len;

            String line = script.substring(start, nl);
            line.trim();

            if (line == "loop") {
                has_loop = true;
                break;  // Restart from beginning
            }

            deploy_execute_line(line);
            start = nl + 1;
        }

        if (!has_loop) break;  // No loop — single execution

        // Small delay before repeat to avoid CPU hogging
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("[Deploy] Script finished");
    event_add("deploy stopped");

    // Set flags and clear handle BEFORE vTaskDelete,
    // so other code doesn't access freed TCB
    xSemaphoreTake(deploy_mutex, portMAX_DELAY);
    deploy_running = false;
    deploy_task_handle = NULL;
    xSemaphoreGive(deploy_mutex);

    vTaskDelete(NULL);
}

static void deploy_start(const String &script) {
    deploy_stop_and_wait();  // Stop previous and wait for completion

    // Bug #6: clear previous deploy log before new run
    if (deploy_log_mutex) xSemaphoreTake(deploy_log_mutex, portMAX_DELAY);
    deploy_log_pos = 0;
    deploy_log_wrapped = false;
    memset(deploy_log_buf, 0, DEPLOY_LOG_SIZE);
    if (deploy_log_mutex) xSemaphoreGive(deploy_log_mutex);

    xSemaphoreTake(deploy_mutex, portMAX_DELAY);
    deploy_script = script;
    xSemaphoreGive(deploy_mutex);

    write_spiffs_file(DEPLOY_SCRIPT_FILE, script);

    xTaskCreatePinnedToCore(
        deploy_task_func,
        "deploy",
        4096,
        NULL,
        1,          // Low priority
        &deploy_task_handle,
        1           // Core 1 (Core 0 = WiFi)
    );
}

// Non-blocking stop — sets flag, task will finish on its own
static void deploy_stop() {
    deploy_stop_requested = true;
}

// Blocking stop — waits for actual completion (for deploy_start)
static void deploy_stop_and_wait() {
    deploy_stop_requested = true;
    // Wait for task to finish and clear handle (max 5 sec)
    int attempts = 0;
    while (attempts < 50) {
        xSemaphoreTake(deploy_mutex, portMAX_DELAY);
        bool still_running = (deploy_task_handle != NULL);
        xSemaphoreGive(deploy_mutex);
        if (!still_running) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        attempts++;
    }
    // If task is stuck — force kill
    xSemaphoreTake(deploy_mutex, portMAX_DELAY);
    if (deploy_task_handle != NULL) {
        vTaskDelete(deploy_task_handle);
        deploy_task_handle = NULL;
        deploy_running = false;
    }
    xSemaphoreGive(deploy_mutex);
}

// ===== POST body =====
// Per-request body buffers via request->_tempObject (malloc/free)
// Thread-safe: each request has its own buffer

// ===== HTTP Handlers =====

// Helper — collect body from async POST and call handler
// ESPAsyncWebServer calls body callback in chunks, then onRequest

// --- GET /health (no auth) ---
static void handle_health(AsyncWebServerRequest *request) {
    unsigned long uptime_sec = (millis() - boot_time) / 1000;

    JsonDocument doc;
    doc["ok"] = true;
    doc["uptime_sec"] = uptime_sec;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /capabilities ---
static void handle_capabilities(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    doc["node"] = mdns_name;
    doc["type"] = "esp32-s3";

    JsonArray modules = doc["modules"].to<JsonArray>();
    modules.add("gpio");
    modules.add("i2c");
    modules.add("logs");
    modules.add("live");
    modules.add("events");
    modules.add("mesh");
    modules.add("firmware");
    modules.add("wireguard");

    JsonArray pins = doc["gpio_pins"].to<JsonArray>();
    for (int i = 0; i < NUM_SAFE_PINS; i++) {
        pins.add(SAFE_GPIO_PINS[i]);
    }

    JsonArray configured = doc["gpio_configured"].to<JsonArray>();
    for (int i = 0; i < gpio_count; i++) {
        if (!gpio_configured[i].configured) continue;
        JsonObject p = configured.add<JsonObject>();
        p["pin"] = gpio_configured[i].pin;
        p["direction"] = gpio_configured[i].is_output ? "out" : "in";
        p["value"] = digitalRead(gpio_configured[i].pin);
    }

    doc["i2c_bus"] = "Wire(17/18)+Wire1(5/6)";
    doc["has_keyboard"] = false;
    doc["has_kvm"] = false;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /config.md ---
static void handle_config_get(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String content = read_spiffs_file(CONFIG_MD_FILE);
    request->send(200, "text/markdown; charset=utf-8", content);
}

// --- POST /config.md (body handler) ---

static void handle_config_post_body(AsyncWebServerRequest *request, uint8_t *data,
                                     size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Body size limit — OOM protection
        if (total > 4096) { request->send(413, "application/json", "{\"error\":\"Payload Too Large\"}"); return; }
        char *buf = (char*)malloc(total + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf) memcpy(buf + index, data, len);
    if (index + len == total && buf) buf[total] = '\0';
}

static void handle_config_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    String content(body);
    free(body);
    request->_tempObject = nullptr;

    if (write_spiffs_file(CONFIG_MD_FILE, content)) {
        JsonDocument doc;
        doc["ok"] = true;
        doc["bytes"] = (int)content.length();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    } else {
        request->send(500, "application/json",
            "{\"error\":\"cannot write config.md\"}");
    }
}

// --- POST /auth/token ---

static void handle_auth_body(AsyncWebServerRequest *request, uint8_t *data,
                              size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Body size limit — OOM protection
        if (total > 4096) { request->send(413, "application/json", "{\"error\":\"Payload Too Large\"}"); return; }
        char *buf = (char*)malloc(total + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf) memcpy(buf + index, data, len);
    if (index + len == total && buf) buf[total] = '\0';
}

static void handle_auth_token(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"empty token\"}");
        return;
    }

    String new_token(body);
    free(body);
    request->_tempObject = nullptr;

    new_token.trim();
    if (new_token.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"empty token\"}");
        return;
    }

    auth_token = new_token;
    write_spiffs_file(TOKEN_FILE, auth_token);

    JsonDocument doc;
    doc["ok"] = true;
    doc["token_length"] = (int)auth_token.length();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GPIO handlers ---
// POST /gpio/{pin}/mode and POST /gpio/{pin}/write use path params

static void handle_gpio_mode(AsyncWebServerRequest *request, int pin) {
    if (!require_auth(request)) return;

    if (!gpio_is_valid_pin(pin)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"invalid GPIO pin %d\"}", pin);
        request->send(400, "application/json", buf);
        return;
    }

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json",
            "{\"error\":\"body must be 'input' or 'output'\"}");
        return;
    }
    String gpio_body(body);
    free(body);
    request->_tempObject = nullptr;

    gpio_body.trim();
    bool is_output;
    if (gpio_body == "input" || gpio_body == "in") {
        is_output = false;
    } else if (gpio_body == "output" || gpio_body == "out") {
        is_output = true;
    } else {
        request->send(400, "application/json",
            "{\"error\":\"body must be 'input' or 'output'\"}");
        return;
    }

    pinMode(pin, is_output ? OUTPUT : INPUT);
    gpio_track_pin(pin, is_output);
    event_add("gpio %d mode set to %s", pin, is_output ? "output" : "input");

    JsonDocument doc;
    doc["ok"] = true;
    doc["pin"] = pin;
    doc["direction"] = is_output ? "out" : "in";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_gpio_write(AsyncWebServerRequest *request, int pin) {
    if (!require_auth(request)) return;

    if (!gpio_is_valid_pin(pin)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"invalid GPIO pin %d\"}", pin);
        request->send(400, "application/json", buf);
        return;
    }

    int cfg_idx = gpio_find_configured(pin);
    if (cfg_idx < 0) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"error\":\"GPIO %d not configured. POST /gpio/%d/mode first\"}", pin, pin);
        request->send(400, "application/json", buf);
        return;
    }

    // Bug #1: check pin is configured as output before writing
    if (!gpio_configured[cfg_idx].is_output) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"error\":\"pin %d is configured as input, set mode to output first\"}", pin);
        request->send(400, "application/json", buf);
        return;
    }

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json",
            "{\"error\":\"body must be 'high', 'low', '1', or '0'\"}");
        return;
    }
    String gpio_body(body);
    free(body);
    request->_tempObject = nullptr;

    gpio_body.trim();
    int val = -1;
    if (gpio_body == "high" || gpio_body == "1") val = 1;
    else if (gpio_body == "low" || gpio_body == "0") val = 0;
    else {
        request->send(400, "application/json",
            "{\"error\":\"body must be 'high', 'low', '1', or '0'\"}");
        return;
    }

    digitalWrite(pin, val ? HIGH : LOW);
    event_add("gpio %d set to %d", pin, val);

    JsonDocument doc;
    doc["ok"] = true;
    doc["pin"] = pin;
    doc["value"] = val;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_gpio_read(AsyncWebServerRequest *request, int pin) {
    if (!require_auth(request)) return;

    if (!gpio_is_valid_pin(pin)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"invalid GPIO pin %d\"}", pin);
        request->send(400, "application/json", buf);
        return;
    }

    if (gpio_find_configured(pin) < 0) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"error\":\"GPIO %d not configured. POST /gpio/%d/mode first\"}", pin, pin);
        request->send(400, "application/json", buf);
        return;
    }

    int val = digitalRead(pin);
    int idx = gpio_find_configured(pin);
    const char *dir = (idx >= 0 && gpio_configured[idx].is_output) ? "out" : "in";

    JsonDocument doc;
    doc["pin"] = pin;
    doc["value"] = val;
    doc["direction"] = dir;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_gpio_status(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < gpio_count; i++) {
        if (!gpio_configured[i].configured) continue;
        int p = gpio_configured[i].pin;
        JsonObject obj = arr.add<JsonObject>();
        obj["pin"] = p;
        obj["value"] = digitalRead(p);
        obj["direction"] = gpio_configured[i].is_output ? "out" : "in";
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- I2C handlers ---

static void handle_i2c_scan(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    JsonArray devices = doc["devices"].to<JsonArray>();

    // Scan both buses (under mutex since Wire is shared with OLED)
    xSemaphoreTake(wire_mutex, portMAX_DELAY);
    i2c_scan_bus(Wire, devices, "primary");
    i2c_scan_bus(Wire1, devices, "external");
    xSemaphoreGive(wire_mutex);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_i2c_write(AsyncWebServerRequest *request, int addr) {
    if (!require_auth(request)) return;

    if (addr < 0x03 || addr > 0x77) {
        request->send(400, "application/json",
            "{\"error\":\"I2C address must be 0x03-0x77\"}");
        return;
    }

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"no valid hex bytes\"}");
        return;
    }
    String i2c_write_body(body);
    free(body);
    request->_tempObject = nullptr;

    i2c_write_body.trim();
    uint8_t bytes[128];
    int nbytes = 0;

    if (!i2c_parse_hex(i2c_write_body, bytes, nbytes, 128) || nbytes == 0) {
        request->send(400, "application/json", "{\"error\":\"no valid hex bytes\"}");
        return;
    }

    xSemaphoreTake(wire_mutex, portMAX_DELAY);
    TwoWire &bus = i2c_select_bus(addr);
    bus.beginTransmission(addr);
    bus.write(bytes, nbytes);
    uint8_t err = bus.endTransmission();
    xSemaphoreGive(wire_mutex);

    if (err != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"I2C write failed (err=%d)\"}", err);
        request->send(500, "application/json", buf);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    char hex_addr[7];
    snprintf(hex_addr, sizeof(hex_addr), "0x%02X", addr);
    doc["addr"] = hex_addr;
    doc["bytes_written"] = nbytes;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_i2c_read(AsyncWebServerRequest *request, int addr, int count) {
    if (!require_auth(request)) return;

    if (addr < 0x03 || addr > 0x77) {
        request->send(400, "application/json",
            "{\"error\":\"I2C address must be 0x03-0x77\"}");
        return;
    }

    if (count <= 0 || count > 255) {
        request->send(400, "application/json",
            "{\"error\":\"count must be 1-255 (I2C hardware limit)\"}");
        return;
    }

    xSemaphoreTake(wire_mutex, portMAX_DELAY);
    TwoWire &bus = i2c_select_bus(addr);
    bus.requestFrom((uint8_t)addr, (uint8_t)count);

    // Build hex string as in Pi server: "0A FF 01"
    String hex_data = "";
    int nread = 0;
    while (bus.available() && nread < count) {
        uint8_t b = bus.read();
        char hex[4];
        snprintf(hex, sizeof(hex), "%s%02X", nread > 0 ? " " : "", b);
        hex_data += hex;
        nread++;
    }
    xSemaphoreGive(wire_mutex);

    // Device didn't respond (no ACK) — return error instead of empty data
    if (nread == 0) {
        char err[80];
        snprintf(err, sizeof(err), "{\"error\":\"no ACK from 0x%02X -- device not found or not responding\"}", addr);
        request->send(404, "application/json", err);
        return;
    }

    JsonDocument doc;
    char hex_addr[7];
    snprintf(hex_addr, sizeof(hex_addr), "0x%02X", addr);
    doc["addr"] = hex_addr;
    doc["count"] = nread;
    doc["data"] = hex_data;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- Deploy handlers ---

static void handle_deploy_body(AsyncWebServerRequest *request, uint8_t *data,
                                size_t len, size_t index, size_t total) {
    if (index == 0 && total > MAX_SCRIPT_SIZE) {
        // Script size limit — return error instead of silent truncation
        request->send(413, "application/json", "{\"error\":\"deploy script too large (max 4KB)\"}");
        return;
    }
    size_t alloc_size = total < MAX_SCRIPT_SIZE ? total : MAX_SCRIPT_SIZE;
    if (index == 0) {
        char *buf = (char*)malloc(alloc_size + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf && index + len <= alloc_size) {
        memcpy(buf + index, data, len);
    }
    if (index + len == total && buf) {
        size_t end = total < alloc_size ? total : alloc_size;
        buf[end] = '\0';
    }
}

static void handle_deploy_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"empty script body\"}");
        return;
    }

    String deploy_body(body);
    free(body);
    request->_tempObject = nullptr;

    deploy_body.trim();
    if (deploy_body.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"empty script body\"}");
        return;
    }

    deploy_start(deploy_body);

    JsonDocument doc;
    doc["ok"] = true;
    doc["script_bytes"] = (int)deploy_body.length();
    doc["status"] = "started";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_deploy_get(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String script = read_spiffs_file(DEPLOY_SCRIPT_FILE);

    JsonDocument doc;
    if (script.length() == 0) {
        doc["status"] = "none";
        doc["script"] = (const char *)NULL;
        doc["pid"] = (const char *)NULL;
    } else {
        doc["status"] = deploy_running ? "running" : "stopped";
        doc["script"] = script;
        doc["pid"] = 0;  // FreeRTOS has no PID
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_deploy_delete(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    deploy_stop();  // Non-blocking — sets flag, task finishes on its own
    SPIFFS.remove(DEPLOY_SCRIPT_FILE);

    request->send(200, "application/json", "{\"ok\":true,\"stopping\":true}");
}

// --- GET /deploy/logs ---
static void handle_deploy_logs(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    JsonArray lines = doc["lines"].to<JsonArray>();

    if (deploy_log_mutex) xSemaphoreTake(deploy_log_mutex, portMAX_DELAY);

    // Collect lines from ring buffer
    int start = deploy_log_wrapped ? deploy_log_pos : 0;
    int total = deploy_log_wrapped ? DEPLOY_LOG_SIZE : deploy_log_pos;

    // Extract lines
    String current_line = "";
    int line_count = 0;
    for (int i = 0; i < total; i++) {
        int idx = (start + i) % DEPLOY_LOG_SIZE;
        char c = deploy_log_buf[idx];
        if (c == '\n') {
            if (current_line.length() > 0) {
                lines.add(current_line);
                line_count++;
            }
            current_line = "";
        } else {
            current_line += c;
        }
    }
    if (current_line.length() > 0) {
        lines.add(current_line);
        line_count++;
    }

    if (deploy_log_mutex) xSemaphoreGive(deploy_log_mutex);

    doc["count"] = line_count;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /live.md ---
static void handle_live_get(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String content = read_spiffs_file(LIVE_MD_FILE);
    request->send(200, "text/markdown; charset=utf-8", content);
}

// --- POST /live.md (body handler) ---
static void handle_live_post_body(AsyncWebServerRequest *request, uint8_t *data,
                                   size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Body size limit — OOM protection
        if (total > 4096) { request->send(413, "application/json", "{\"error\":\"Payload Too Large\"}"); return; }
        char *buf = (char*)malloc(total + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf) memcpy(buf + index, data, len);
    if (index + len == total && buf) buf[total] = '\0';
}

static void handle_live_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    String content(body);
    free(body);
    request->_tempObject = nullptr;

    if (write_spiffs_file(LIVE_MD_FILE, content)) {
        JsonDocument doc;
        doc["ok"] = true;
        doc["bytes"] = (int)content.length();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    } else {
        request->send(500, "application/json",
            "{\"error\":\"cannot write live.md\"}");
    }
}

// --- GET /events ---
static void handle_events(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    unsigned long since = 0;
    if (request->hasParam("since")) {
        const char *start = request->getParam("since")->value().c_str();
        char *endptr = NULL;
        since = strtoul(start, &endptr, 10);
        // Verify string contains at least one digit
        if (endptr == start) {
            request->send(400, "application/json", "{\"error\":\"since must be a unix timestamp\"}");
            return;
        }
    }

    if (events_mutex) xSemaphoreTake(events_mutex, portMAX_DELAY);

    int start_idx = (events_count < MAX_EVENTS) ? 0 : events_head;
    int total = events_count;

    JsonDocument doc;
    JsonArray events = doc["events"].to<JsonArray>();

    int emitted = 0;
    for (int i = 0; i < total; i++) {
        int idx = (start_idx + i) % MAX_EVENTS;
        EventEntry *e = &events_buf[idx];
        if (since > 0 && e->timestamp <= since) continue;
        // Without since — only last 100
        if (since == 0 && total - i > 100) continue;

        JsonObject obj = events.add<JsonObject>();
        obj["t"] = e->timestamp;
        obj["event"] = e->message;
        emitted++;
    }

    if (events_mutex) xSemaphoreGive(events_mutex);

    doc["count"] = emitted;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /mesh — mDNS discovery of neighboring Seed nodes ---
static void handle_mesh(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;

    // Self info — use same IP for display and filtering.
    // If STA connected — localIP(), otherwise softAPIP().
    JsonObject self = doc["self"].to<JsonObject>();
    self["name"] = mdns_name;
    String self_ip = (WiFi.status() == WL_CONNECTED)
        ? WiFi.localIP().toString()
        : WiFi.softAPIP().toString();
    self["ip"] = self_ip;
    self["port"] = HTTP_PORT;

    // Discover other _seed._tcp nodes
    JsonArray nodes = doc["nodes"].to<JsonArray>();
    int count = 0;

    // NB: queryService blocks ~2s. Acceptable since /mesh is called rarely.
    // TODO: could switch to async discovery + result caching.
    int n = MDNS.queryService("seed", "tcp");
    if (n > 0) {
        // Current Unix timestamp
        struct timeval tv;
        unsigned long now_ts = 0;
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000) {
            now_ts = (unsigned long)tv.tv_sec;
        } else {
            now_ts = millis() / 1000;
        }

        for (int i = 0; i < n; i++) {
            String node_ip = MDNS.IP(i).toString();
            // Filter self
            if (node_ip == self_ip && MDNS.port(i) == HTTP_PORT) continue;

            JsonObject node = nodes.add<JsonObject>();
            node["name"] = MDNS.hostname(i);
            node["ip"] = node_ip;
            node["port"] = MDNS.port(i);
            node["last_seen"] = now_ts;
            count++;

            event_add("mesh: discovered node %s at %s", MDNS.hostname(i).c_str(), node_ip.c_str());
        }
    }

    doc["count"] = count;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /firmware/version ---
static void handle_firmware_version(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    const esp_partition_t *running = esp_ota_get_running_partition();

    JsonDocument doc;
    doc["version"] = FIRMWARE_VERSION;
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

// --- POST /firmware/upload (body handler) ---
// Stream firmware binary directly to OTA partition, no RAM buffering
static void handle_firmware_upload_body(AsyncWebServerRequest *request, uint8_t *data,
                                         size_t len, size_t index, size_t total) {
    // Check auth on first chunk
    if (index == 0) {
        if (!check_auth(request)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "Authorization: Bearer <token> required");
            return;
        }

        // Block concurrent uploads
        if (ota_in_progress) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "OTA upload already in progress");
            return;
        }

        // Minimal firmware size validation
        if (total == 0 || total > 0x330000) {  // Max = OTA partition size (3.1875 MB)
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "invalid firmware size: %u (max 3342336)", (unsigned)total);
            return;
        }

        // Reset state
        ota_in_progress = true;
        ota_upload_started = false;
        ota_upload_ok = false;
        ota_upload_error = false;
        ota_upload_error_msg[0] = '\0';
        ota_bytes_written = 0;

        Serial.printf("[OTA] Firmware upload started, size: %u bytes\n", (unsigned)total);
        event_add("ota upload started, size=%u", (unsigned)total);

        if (!Update.begin(total, U_FLASH)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "Update.begin() failed: %s", Update.errorString());
            Serial.printf("[OTA] Update.begin() failed: %s\n", Update.errorString());
            ota_in_progress = false;
            return;
        }
        ota_upload_started = true;
    }

    // If error already occurred — skip remaining chunks
    if (ota_upload_error) return;

    // Write chunk
    if (ota_upload_started && Update.isRunning()) {
        if (Update.write(data, len) != len) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "Update.write() failed: %s", Update.errorString());
            Serial.printf("[OTA] Update.write() failed: %s\n", Update.errorString());
            Update.abort();
            ota_in_progress = false;
            return;
        }
        ota_bytes_written += len;
    }

    // Last chunk — finalize OTA (only if Update.begin() was called)
    if (index + len == total && ota_upload_started) {
        if (!Update.end(true)) {
            ota_upload_error = true;
            snprintf(ota_upload_error_msg, sizeof(ota_upload_error_msg),
                     "Update.end() failed: %s", Update.errorString());
            Serial.printf("[OTA] Update.end() failed: %s\n", Update.errorString());
        } else {
            ota_upload_ok = true;
            Serial.printf("[OTA] Firmware uploaded: %u bytes, MD5: %s\n",
                          (unsigned)ota_bytes_written, Update.md5String().c_str());
            event_add("ota upload complete, %u bytes, md5=%s",
                      (unsigned)ota_bytes_written, Update.md5String().c_str());
        }
        ota_in_progress = false;
    }
}

// --- POST /firmware/upload (request handler) ---
static void handle_firmware_upload(AsyncWebServerRequest *request) {
    // Auth checked in body handler; if auth error — return 401
    if (ota_upload_error && strstr(ota_upload_error_msg, "Authorization") != NULL) {
        request->send(401, "application/json",
            "{\"error\":\"Authorization: Bearer <token> required\"}");
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
        request->send(400, "application/json",
            "{\"error\":\"no firmware uploaded\"}");
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

// --- POST /firmware/apply ---
// Restart is deferred to loop() so HTTP response has time to send
static void handle_firmware_apply(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    // Check firmware was uploaded
    if (!ota_upload_ok) {
        request->send(400, "application/json",
            "{\"error\":\"no firmware uploaded, use POST /firmware/upload first\"}");
        return;
    }

    event_add("ota apply: restarting to new firmware");
    Serial.println("[OTA] Rebooting into new firmware...");

    pending_restart = true;
    request->send(200, "application/json",
        "{\"ok\":true,\"warning\":\"restarting in ~1 second\"}");
}

// --- POST /firmware/confirm ---
static void handle_firmware_confirm(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    firmware_confirmed = (err == ESP_OK);
    if (firmware_confirmed) {
        event_add("ota firmware confirmed manually");
        Serial.println("[OTA] Firmware confirmed manually");
    }

    JsonDocument doc;
    doc["ok"] = firmware_confirmed;
    doc["confirmed"] = firmware_confirmed;
    if (!firmware_confirmed) {
        doc["error"] = "esp_ota_mark_app_valid failed";
    }

    String response;
    serializeJson(doc, response);
    request->send(firmware_confirmed ? 200 : 500, "application/json", response);
}

// --- POST /firmware/rollback ---
// Rollback is deferred to loop() so HTTP response has time to send
static void handle_firmware_rollback(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    event_add("ota rollback: reverting to previous firmware");
    Serial.println("[OTA] Rolling back to previous firmware...");

    pending_restart = true;
    pending_rollback = true;
    request->send(200, "application/json",
        "{\"ok\":true,\"warning\":\"rolling back and rebooting in ~1 second\"}");
}

// --- WiFi config page (on AP) ---

static void handle_wifi_config_page(AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Seed WiFi Config</title>"
        "<style>body{font-family:monospace;max-width:400px;margin:40px auto;padding:0 20px}"
        "input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
        "button{padding:10px 20px;background:#333;color:#fff;border:none;cursor:pointer}"
        "</style></head><body>"
        "<h2>Seed WiFi Config</h2>"
        "<form method='POST' action='/wifi/config'>"
        "<label>SSID:</label><input type='text' name='ssid' required>"
        "<label>Password:</label><input type='password' name='pass'>"
        "<button type='submit'>Save & Connect</button>"
        "</form>";

    if (WiFi.status() == WL_CONNECTED) {
        html += "<p>Connected to: " + WiFi.SSID() + "<br>IP: " +
                WiFi.localIP().toString() + "</p>";
    }
    html += "<p>AP: " + ap_ssid + "</p></body></html>";
    request->send(200, "text/html", html);
}

static void handle_wifi_config_post(AsyncWebServerRequest *request) {
    // Parse form data
    String ssid = "";
    String pass = "";

    if (request->hasParam("ssid", true)) {
        ssid = request->getParam("ssid", true)->value();
    }
    if (request->hasParam("pass", true)) {
        pass = request->getParam("pass", true)->value();
    }

    if (ssid.length() == 0) {
        request->send(400, "text/html", "<h2>SSID required</h2><a href='/'>Back</a>");
        return;
    }

    wifi_save_config(ssid, pass);
    wifi_ssid = ssid;
    wifi_pass = pass;

    request->send(200, "text/html",
        "<h2>Saved! Connecting...</h2>"
        "<p>SSID: " + ssid + "</p>"
        "<p>Device will connect. If successful, access via mDNS: " +
        mdns_name + ".local:" + String(HTTP_PORT) + "</p>"
        "<p><a href='/'>Back</a></p>");

    // Connect to WiFi in next loop cycle
    // (don't block HTTP response)
    delay(1000);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

// ===== WireGuard HTTP Handlers =====

// Body handler for WG POST endpoints (shared pattern)
static void handle_wg_body(AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
    if (index == 0) {
        if (total > 2048) { request->send(413, "application/json", "{\"error\":\"Payload Too Large\"}"); return; }
        char *buf = (char*)malloc(total + 1);
        if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
        request->_tempObject = buf;
    }
    char *buf = (char*)request->_tempObject;
    if (buf) memcpy(buf + index, data, len);
    if (index + len == total && buf) buf[total] = '\0';
}

// --- GET /wg/status ---
static void handle_wg_status(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    doc["interface"] = "wg0";
    doc["up"] = wg_running;
    doc["public_key"] = wg_public_key;

    // Load config for extra fields
    JsonDocument cfg;
    bool has_config = wg_load_config(cfg);

    doc["address"] = has_config && cfg["address"] ? cfg["address"].as<String>() : "";
    doc["listen_port"] = has_config && cfg["listen_port"] ? cfg["listen_port"].as<int>() : 0;
    doc["peer_configured"] = has_config && cfg["peer"].is<JsonObject>() && cfg["peer"]["public_key"];

    // Check peer status via library (if WG running)
    bool peer_connected = false;
    if (wg_running) {
        // wireguardif_peer_is_up available via extern — but WireGuard class
        // doesn't export netif/peer_index directly. Treat "connected" = is_initialized.
        peer_connected = wg.is_initialized();
    }
    doc["peer_connected"] = peer_connected;
    doc["restart_needed"] = wg_restart_needed;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- POST /wg/config ---
static void handle_wg_config_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) {
        free(body);
        request->_tempObject = nullptr;
        request->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    free(body);
    request->_tempObject = nullptr;

    // Validation
    const char *address = input["address"];
    if (address && !wg_validate_ip(address)) {
        request->send(400, "application/json", "{\"error\":\"invalid address format\"}");
        return;
    }

    const char *private_key = input["private_key"];
    if (private_key && !wg_validate_key(private_key)) {
        request->send(400, "application/json", "{\"error\":\"invalid private_key (must be 44 base64 chars)\"}");
        return;
    }

    int listen_port = input["listen_port"] | 0;
    if (input["listen_port"] && (listen_port < 1 || listen_port > 65535)) {
        request->send(400, "application/json", "{\"error\":\"listen_port must be 1-65535\"}");
        return;
    }

    // Load existing config (if any) and merge
    JsonDocument cfg;
    wg_load_config(cfg);

    if (address) cfg["address"] = address;
    if (private_key) {
        cfg["private_key"] = private_key;
        // Derive and store public key
        wg_derive_public_key(private_key);
    }
    if (listen_port > 0) cfg["listen_port"] = listen_port;

    if (!wg_save_config(cfg)) {
        request->send(500, "application/json", "{\"error\":\"failed to save config\"}");
        return;
    }

    bool needs_restart = wg_running;
    if (needs_restart) wg_restart_needed = true;

    event_add("wg config updated: address=%s", cfg["address"].as<const char*>());

    JsonDocument resp;
    resp["ok"] = true;
    resp["address"] = cfg["address"].as<String>();
    resp["listen_port"] = cfg["listen_port"] | 0;
    resp["restart_needed"] = needs_restart;

    String response;
    serializeJson(resp, response);
    request->send(200, "application/json", response);
}

// --- POST /wg/peer ---
static void handle_wg_peer_post(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    if (!body) {
        request->send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) {
        free(body);
        request->_tempObject = nullptr;
        request->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    free(body);
    request->_tempObject = nullptr;

    // Validation
    const char *public_key = input["public_key"];
    if (!public_key || !wg_validate_key(public_key)) {
        request->send(400, "application/json", "{\"error\":\"invalid or missing public_key (must be 44 base64 chars)\"}");
        return;
    }

    const char *endpoint = input["endpoint"];
    if (!endpoint || strlen(endpoint) == 0) {
        request->send(400, "application/json", "{\"error\":\"endpoint required\"}");
        return;
    }
    // Validate endpoint — IP or hostname, allow [a-zA-Z0-9.-]
    for (int i = 0; endpoint[i]; i++) {
        char c = endpoint[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
        {
            request->send(400, "application/json", "{\"error\":\"invalid endpoint format\"}");
            return;
        }
    }

    int port = input["port"] | 51820;
    if (port < 1 || port > 65535) {
        request->send(400, "application/json", "{\"error\":\"port must be 1-65535\"}");
        return;
    }

    const char *allowed_ips = input["allowed_ips"];
    if (allowed_ips && !wg_validate_allowed_ips(allowed_ips)) {
        request->send(400, "application/json", "{\"error\":\"invalid allowed_ips format\"}");
        return;
    }

    int keepalive = input["persistent_keepalive"] | 0;

    // Load existing config and update peer
    JsonDocument cfg;
    wg_load_config(cfg);

    JsonObject peer = cfg["peer"].to<JsonObject>();
    peer["public_key"] = public_key;
    peer["endpoint"] = endpoint;
    peer["port"] = port;
    if (allowed_ips) peer["allowed_ips"] = allowed_ips;
    if (keepalive > 0) peer["persistent_keepalive"] = keepalive;

    if (!wg_save_config(cfg)) {
        request->send(500, "application/json", "{\"error\":\"failed to save config\"}");
        return;
    }

    bool needs_restart = wg_running;
    if (needs_restart) wg_restart_needed = true;

    event_add("wg peer configured: %s -> %s:%d", public_key, endpoint, port);

    JsonDocument resp;
    resp["ok"] = true;
    resp["public_key"] = public_key;
    resp["restart_needed"] = needs_restart;

    String response;
    serializeJson(resp, response);
    request->send(200, "application/json", response);
}

// --- DELETE /wg/peer ---
static void handle_wg_peer_delete(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    char *body = (char*)request->_tempObject;
    String pubkey_to_remove;
    if (body) {
        pubkey_to_remove = String(body);
        pubkey_to_remove.trim();
        free(body);
        request->_tempObject = nullptr;
    }

    JsonDocument cfg;
    if (!wg_load_config(cfg)) {
        request->send(404, "application/json", "{\"error\":\"no WG config\"}");
        return;
    }

    if (!cfg["peer"].is<JsonObject>()) {
        request->send(404, "application/json", "{\"error\":\"no peer configured\"}");
        return;
    }

    // If key specified — verify it matches
    const char *existing_key = cfg["peer"]["public_key"];
    if (pubkey_to_remove.length() > 0 && existing_key &&
        pubkey_to_remove != String(existing_key)) {
        request->send(404, "application/json", "{\"error\":\"peer public_key mismatch\"}");
        return;
    }

    String removed_key = existing_key ? String(existing_key) : "";
    cfg.remove("peer");

    if (!wg_save_config(cfg)) {
        request->send(500, "application/json", "{\"error\":\"failed to save config\"}");
        return;
    }

    // If WG running — stop it
    if (wg_running) {
        wg_stop();
    }

    event_add("wg peer removed: %s", removed_key.c_str());

    JsonDocument resp;
    resp["ok"] = true;
    resp["removed"] = removed_key;

    String response;
    serializeJson(resp, response);
    request->send(200, "application/json", response);
}

// --- GET /wg/peers ---
static void handle_wg_peers(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    JsonArray peers = doc["peers"].to<JsonArray>();

    JsonDocument cfg;
    bool has_config = wg_load_config(cfg);

    int count = 0;
    if (has_config && cfg["peer"].is<JsonObject>() && cfg["peer"]["public_key"]) {
        JsonObject p = peers.add<JsonObject>();
        p["public_key"] = cfg["peer"]["public_key"].as<String>();

        String ep = cfg["peer"]["endpoint"].as<String>();
        int port = cfg["peer"]["port"] | 51820;
        p["endpoint"] = ep + ":" + String(port);

        p["allowed_ips"] = cfg["peer"]["allowed_ips"] | "0.0.0.0/0";

        // Connection status
        bool connected = false;
        if (wg_running) {
            connected = wg.is_initialized();
        }
        p["connected"] = connected;
        count = 1;
    }

    doc["count"] = count;
    doc["note"] = "ESP32 supports single peer only";

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- POST /wg/restart — schedule WG tunnel restart ---
// wg.begin() can block up to 10 seconds (DNS resolve) — cannot call
// from async HTTP handler. Set flag, restart happens in loop().
static void handle_wg_restart(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    wg_restart_needed = true;

    request->send(200, "application/json",
        "{\"ok\":true,\"message\":\"restart scheduled, will apply in loop()\"}");
}

// ===== URL Router =====
// ESPAsyncWebServer doesn't support path params like /gpio/<pin>/read
// natively, so we use catch-all + URL parsing

static void setup_routes() {
    // --- GET /health (no auth) ---
    server.on("/health", HTTP_GET, handle_health);

    // --- GET /capabilities ---
    server.on("/capabilities", HTTP_GET, handle_capabilities);

    // --- GET /config.md ---
    server.on("/config.md", HTTP_GET, handle_config_get);

    // --- POST /config.md ---
    server.on("/config.md", HTTP_POST, handle_config_post, NULL, handle_config_post_body);

    // --- POST /auth/token ---
    server.on("/auth/token", HTTP_POST, handle_auth_token, NULL, handle_auth_body);

    // --- GET /gpio/status ---
    server.on("/gpio/status", HTTP_GET, handle_gpio_status);

    // --- POST /i2c/scan ---
    server.on("/i2c/scan", HTTP_POST, handle_i2c_scan);

    // --- Observability (deploy/logs must be before /deploy to avoid prefix match) ---
    server.on("/deploy/logs", HTTP_GET, handle_deploy_logs);
    server.on("/live.md", HTTP_GET, handle_live_get);
    server.on("/live.md", HTTP_POST, handle_live_post, NULL, handle_live_post_body);
    server.on("/events", HTTP_GET, handle_events);
    server.on("/mesh", HTTP_GET, handle_mesh);

    // --- Firmware OTA ---
    server.on("/firmware/version", HTTP_GET, handle_firmware_version);
    server.on("/firmware/upload", HTTP_POST, handle_firmware_upload, NULL, handle_firmware_upload_body);
    server.on("/firmware/apply", HTTP_POST, handle_firmware_apply);
    server.on("/firmware/confirm", HTTP_POST, handle_firmware_confirm);
    server.on("/firmware/rollback", HTTP_POST, handle_firmware_rollback);

    // --- Deploy ---
    server.on("/deploy", HTTP_POST, handle_deploy_post, NULL, handle_deploy_body);
    server.on("/deploy", HTTP_GET, handle_deploy_get);
    server.on("/deploy", HTTP_DELETE, handle_deploy_delete);

    // --- WireGuard ---
    server.on("/wg/status", HTTP_GET, handle_wg_status);
    server.on("/wg/config", HTTP_POST, handle_wg_config_post, NULL, handle_wg_body);
    server.on("/wg/peer", HTTP_POST, handle_wg_peer_post, NULL, handle_wg_body);
    server.on("/wg/peer", HTTP_DELETE, handle_wg_peer_delete, NULL, handle_wg_body);
    server.on("/wg/peers", HTTP_GET, handle_wg_peers);
    server.on("/wg/restart", HTTP_POST, handle_wg_restart);

    // --- WiFi config page ---
    server.on("/", HTTP_GET, handle_wifi_config_page);
    server.on("/wifi/config", HTTP_POST, handle_wifi_config_post);

    // --- GPIO dynamic routes: /gpio/{pin}/mode, /gpio/{pin}/write, /gpio/{pin}/read ---
    // Use onRequestBody + onRequest for POST routes with dynamic path

    // POST /gpio/*/mode, POST /gpio/*/write, POST /i2c/*/write
    // Per-request body buffers via _tempObject
    server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
        String url = request->url();
        if (url.startsWith("/gpio/") || url.startsWith("/i2c/")) {
            if (index == 0) {
                // Body size limit — OOM protection
                if (total > 4096) { request->send(413, "application/json", "{\"error\":\"Payload Too Large\"}"); return; }
                char *buf = (char*)malloc(total + 1);
                if (!buf) { request->send(500, "application/json", "{\"error\":\"OOM\"}"); return; }
                request->_tempObject = buf;
            }
            char *buf = (char*)request->_tempObject;
            if (buf) memcpy(buf + index, data, len);
            if (index + len == total && buf) buf[total] = '\0';
        }
    });

    // Catch-all handler for dynamic routes
    server.onNotFound([](AsyncWebServerRequest *request) {
        String url = request->url();
        String method = request->methodToString();

        // --- GPIO routes ---
        if (url.startsWith("/gpio/") && url != "/gpio/status") {
            // Parse /gpio/{pin}/{action}
            int pin = 0;
            char action[16] = "";
            if (sscanf(url.c_str(), "/gpio/%d/%15s", &pin, action) < 1) {
                request->send(400, "application/json",
                    "{\"error\":\"use /gpio/{pin}/mode or /gpio/{pin}/write or /gpio/{pin}/read\"}");
                return;
            }

            if (method == "POST") {
                if (strcmp(action, "mode") == 0) {
                    handle_gpio_mode(request, pin);
                } else if (strcmp(action, "write") == 0) {
                    handle_gpio_write(request, pin);
                } else {
                    request->send(400, "application/json",
                        "{\"error\":\"use /gpio/{pin}/mode or /gpio/{pin}/write\"}");
                }
            } else if (method == "GET") {
                if (action[0] == '\0' || strcmp(action, "read") == 0) {
                    handle_gpio_read(request, pin);
                } else {
                    request->send(400, "application/json",
                        "{\"error\":\"use GET /gpio/{pin}/read or GET /gpio/status\"}");
                }
            } else {
                request->send(405, "application/json", "{\"error\":\"method not allowed\"}");
            }
            return;
        }

        // --- I2C routes ---
        if (url.startsWith("/i2c/") && url != "/i2c/scan") {
            int addr = 0;
            char action[16] = "";
            int count = 0;

            // POST /i2c/{addr}/write
            if (method == "POST") {
                if (sscanf(url.c_str(), "/i2c/%i/%15s", &addr, action) >= 2 &&
                    strcmp(action, "write") == 0) {
                    handle_i2c_write(request, addr);
                } else {
                    request->send(400, "application/json",
                        "{\"error\":\"use POST /i2c/{addr}/write\"}");
                }
                return;
            }

            // GET /i2c/{addr}/read/{count}
            if (method == "GET") {
                if (sscanf(url.c_str(), "/i2c/%i/%15[^/]/%d", &addr, action, &count) >= 3 &&
                    strcmp(action, "read") == 0) {
                    handle_i2c_read(request, addr, count);
                } else {
                    request->send(400, "application/json",
                        "{\"error\":\"use GET /i2c/{addr}/read/{count}\"}");
                }
                return;
            }

            request->send(405, "application/json", "{\"error\":\"method not allowed\"}");
            return;
        }

        // 404 fallback
        request->send(404, "application/json", "{\"error\":\"not found\"}");
    });
}

// ===== Heltec V3 Board Init =====

static void board_init() {
    // Vext power control
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, LOW);  // Enable Vext (LOW = on for Heltec V3)

    // LoRa SX1262 — put to sleep to save power
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(10);
    digitalWrite(LORA_RST, HIGH);
    delay(10);
    pinMode(LORA_BUSY, INPUT);
    pinMode(LORA_DIO1, INPUT);

    // SPI sleep command for SX1262
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x84);  // SetSleep
    SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    SPI.end();

    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
}

// ===== Main =====

void setup() {
    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    Serial.println("\n=== Seed Node Firmware ===");

    // Board init — LoRa sleep, LED flash
    board_init();

    // SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Mount failed!");
    } else {
        Serial.println("[SPIFFS] Mounted");
    }

    // Mutexes (before I2C/OLED since oled_update uses wire_mutex)
    deploy_mutex = xSemaphoreCreateMutex();
    wire_mutex = xSemaphoreCreateMutex();
    deploy_log_mutex = xSemaphoreCreateMutex();
    events_mutex = xSemaphoreCreateMutex();

    // I2C init (before OLED)
    i2c_init();

    // OLED init
    oled_init();

    // Auth token
    token_load();
    Serial.printf("[Auth] Token: %s\n", auth_token.c_str());

    // WiFi
    wifi_setup();

    // WireGuard — derive public key from config (before NTP so /wg/status shows it)
    {
        JsonDocument wg_doc;
        if (wg_load_config(wg_doc)) {
            const char *pk = wg_doc["private_key"];
            if (pk) wg_derive_public_key(pk);
        }
    }

    // HTTP routes
    setup_routes();
    server.begin();
    Serial.printf("[HTTP] Server started on port %d\n", HTTP_PORT);

    // Initial OLED update
    oled_update();

    Serial.println("=== Ready ===");
    Serial.printf("AP: %s (pass: %s)\n", ap_ssid.c_str(), AP_PASSWORD);
    Serial.printf("mDNS: %s.local\n", mdns_name.c_str());
    Serial.printf("Token: %s\n", auth_token.c_str());
}

void loop() {
    // OLED update every 2 seconds
    static unsigned long last_oled_update = 0;
    if (millis() - last_oled_update > 2000) {
        oled_update();
        last_oled_update = millis();
    }

    // WireGuard auto-start after NTP sync
    // NTP needed for handshake (timestamp-based anti-replay)
    if (!wg_init_done && WiFi.status() == WL_CONNECTED) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000) {
            wg_start();
            wg_init_done = true;
        }
    }

    // WireGuard deferred restart (from /wg/restart, /wg/config, /wg/peer)
    // wg.begin() blocks up to 10s — cannot call from async HTTP handler
    if (wg_restart_needed) {
        wg_restart_needed = false;
        wg_stop();
        wg_start();
    }

    // Deferred restart (from apply/rollback) — wait for HTTP response to send
    static unsigned long restart_requested_at = 0;
    if (pending_restart && restart_requested_at == 0) {
        restart_requested_at = millis();
    }
    if (pending_restart && restart_requested_at > 0 &&
        millis() - restart_requested_at > 1000) {
        if (pending_rollback) {
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        ESP.restart();
    }

    // Auto-confirm firmware after 60 seconds of stable operation (one attempt)
    if (!firmware_confirmed && !firmware_confirm_attempted &&
        (millis() - boot_time) > 60000 && WiFi.status() == WL_CONNECTED) {
        firmware_confirm_attempted = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            firmware_confirmed = true;
            event_add("ota firmware auto-confirmed after 60s uptime");
            Serial.println("[OTA] Firmware auto-confirmed (60s uptime + WiFi)");
        } else {
            Serial.printf("[OTA] Auto-confirm failed: %d (not an OTA boot?)\n", err);
            firmware_confirmed = true;  // Not an OTA boot — consider confirmed
        }
    }

    // WiFi reconnect if disconnected
    static unsigned long last_wifi_check = 0;
    static bool was_connected = false;
    bool is_connected = (WiFi.status() == WL_CONNECTED);
    if (is_connected != was_connected) {
        if (is_connected) {
            event_add("wifi connected: %s", WiFi.SSID().c_str());
        } else {
            event_add("wifi disconnected");
        }
        was_connected = is_connected;
    }
    if (wifi_ssid.length() > 0 && !is_connected &&
        millis() - last_wifi_check > 30000) {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        last_wifi_check = millis();
    }

    delay(10);
}
