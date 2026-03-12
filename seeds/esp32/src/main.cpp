// ESP32 Seed — Minimal self-growing firmware for AI agents
//
// The ESP32 equivalent of seed.c: just enough to boot, connect,
// and let an AI agent grow it via OTA firmware uploads.
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

// ===== Configuration =====
#define SEED_VERSION        "0.1.0"
#define HTTP_PORT           8080
#define AP_PASSWORD         "seed1313"
#define TOKEN_FILE          "/auth_token.txt"
#define WIFI_CONFIG_FILE    "/wifi.json"
#define CONFIG_MD_FILE      "/config.md"

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

// ===== Hardware Probe (run once at boot, cached) =====

// Known I2C device addresses
struct I2CDevice {
    uint8_t addr;
    const char *name;
};

static const I2CDevice known_i2c[] = {
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

    // I2C bus 0 (primary)
    I2CFound i2c0[MAX_I2C_FOUND];
    int i2c0_count;

    // I2C bus 1 (external)
    I2CFound i2c1[MAX_I2C_FOUND];
    int i2c1_count;

    // LoRa SX1262
    bool has_lora;
    uint8_t lora_status;

    // Battery ADC
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

static void probe_lora() {
    // SX1262 on Heltec V3: NSS=8, RST=12, BUSY=13, SCK=9, MISO=11, MOSI=10
    pinMode(8, OUTPUT);
    digitalWrite(8, HIGH);
    pinMode(12, OUTPUT);

    // Reset SX1262
    digitalWrite(12, LOW);
    delay(10);
    digitalWrite(12, HIGH);
    delay(10);

    // Wait for BUSY to go low
    pinMode(13, INPUT);
    unsigned long t = millis();
    while (digitalRead(13) == HIGH && millis() - t < 100) delay(1);

    // SPI: ReadRegister(0x0320) — SyncWord register, should return non-zero on SX1262
    SPI.begin(9, 11, 10, 8);
    digitalWrite(8, LOW);
    SPI.transfer(0x1D);          // ReadRegister opcode
    SPI.transfer(0x03);          // addr high
    SPI.transfer(0x20);          // addr low
    SPI.transfer(0x00);          // NOP (status)
    hw.lora_status = SPI.transfer(0x00);  // register value
    digitalWrite(8, HIGH);
    SPI.end();

    // SX1262 default SyncWord[0] at 0x0320 = 0x14 (LoRa) or 0x34 (public)
    hw.has_lora = (hw.lora_status != 0x00 && hw.lora_status != 0xFF);

    // Put LoRa back to sleep
    if (hw.has_lora) {
        SPI.begin(9, 11, 10, 8);
        digitalWrite(8, LOW);
        SPI.transfer(0x84);  // SetSleep
        SPI.transfer(0x00);
        digitalWrite(8, HIGH);
        SPI.end();
    }
}

static void probe_battery() {
    // Heltec V3: battery voltage via ADC on pin 1, with ADC_CTRL=37
    pinMode(37, OUTPUT);
    digitalWrite(37, LOW);  // Enable ADC read
    delay(10);
    uint32_t raw = analogRead(1);
    digitalWrite(37, HIGH);  // Disable

    // Heltec V3 voltage divider: 390k/100k, so multiply by 4.9
    // ADC 12-bit (0-4095) maps to 0-3.3V
    float voltage = (raw / 4095.0f) * 3.3f * 4.9f;

    // Sanity check: LiPo battery is 3.0-4.2V, USB is ~4.5-5.2V after divider
    hw.has_battery = (voltage > 2.5f && voltage < 5.5f);
    hw.battery_v = voltage;
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

    // I2C bus 0: pins 17/18 (Heltec V3 OLED bus)
    Wire.begin(17, 18);
    Wire.setClock(400000);
    i2c_scan(Wire, hw.i2c0, hw.i2c0_count);

    // I2C bus 1: pins 5/6 (external)
    Wire1.begin(5, 6);
    Wire1.setClock(400000);
    i2c_scan(Wire1, hw.i2c1, hw.i2c1_count);

    // LoRa
    probe_lora();

    // Battery
    probe_battery();

    // Board detection heuristic
    bool oled_on_17_18 = false;
    for (int i = 0; i < hw.i2c0_count; i++) {
        if (hw.i2c0[i].addr == 0x3C) oled_on_17_18 = true;
    }
    if (oled_on_17_18 && hw.has_lora && hw.flash_size == 8 * 1024 * 1024) {
        hw.board = "Heltec WiFi LoRa 32 V3";
    } else if (hw.has_lora) {
        hw.board = "ESP32-S3 + SX1262 (unknown board)";
    } else if (oled_on_17_18) {
        hw.board = "ESP32-S3 + OLED (unknown board)";
    } else {
        hw.board = "ESP32-S3 (generic)";
    }

    Serial.printf("[probe] board: %s\n", hw.board);
    Serial.printf("[probe] temp: %.1fC, flash: %uMB, psram: %uKB\n",
        hw.temp_c, (unsigned)(hw.flash_size / 1024 / 1024),
        (unsigned)(hw.psram_size / 1024));
    Serial.printf("[probe] i2c0: %d devices, i2c1: %d devices\n",
        hw.i2c0_count, hw.i2c1_count);
    Serial.printf("[probe] lora: %s (reg=0x%02X), battery: %.2fV\n",
        hw.has_lora ? "yes" : "no", hw.lora_status, hw.battery_v);
}

// ===== LoRa SX1262 Driver (bare SPI, no library) =====

#define LORA_NSS   8
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO1  14
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10

static bool lora_ready = false;

static void lora_wait_busy() {
    unsigned long t = millis();
    while (digitalRead(LORA_BUSY) == HIGH && millis() - t < 100) delay(1);
}

static void lora_cmd(const uint8_t *data, size_t len) {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    for (size_t i = 0; i < len; i++) SPI.transfer(data[i]);
    digitalWrite(LORA_NSS, HIGH);
}

static uint8_t lora_read_reg(uint16_t addr) {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x1D);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr & 0xFF);
    SPI.transfer(0x00);  // NOP
    uint8_t val = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    return val;
}

static void lora_write_reg(uint16_t addr, uint8_t val) {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x0D);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr & 0xFF);
    SPI.transfer(val);
    digitalWrite(LORA_NSS, HIGH);
}

static void lora_write_buffer(uint8_t offset, const uint8_t *data, size_t len) {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x0E);
    SPI.transfer(offset);
    for (size_t i = 0; i < len; i++) SPI.transfer(data[i]);
    digitalWrite(LORA_NSS, HIGH);
}

static size_t lora_read_buffer(uint8_t offset, uint8_t *data, size_t maxlen) {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x1E);
    SPI.transfer(offset);
    SPI.transfer(0x00);  // NOP
    size_t i;
    for (i = 0; i < maxlen; i++) data[i] = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    return i;
}

static uint16_t lora_get_irq() {
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x12);
    SPI.transfer(0x00);  // NOP
    uint8_t msb = SPI.transfer(0x00);
    uint8_t lsb = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    return (msb << 8) | lsb;
}

static void lora_clear_irq() {
    uint8_t cmd[] = {0x02, 0xFF, 0xFF};
    lora_cmd(cmd, sizeof(cmd));
}

static bool lora_init() {
    if (!hw.has_lora) return false;

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_DIO1, INPUT);

    // HW reset
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(1);
    digitalWrite(LORA_RST, HIGH);
    delay(10);
    lora_wait_busy();

    // TCXO 3.3V, timeout 5ms
    { uint8_t c[] = {0x97, 0x07, 0x00, 0x01, 0x40}; lora_cmd(c, sizeof(c)); }
    // Calibrate all
    { uint8_t c[] = {0x89, 0xFF}; lora_cmd(c, sizeof(c)); }
    delay(5);
    // DIO2 as RF switch
    { uint8_t c[] = {0x9D, 0x01}; lora_cmd(c, sizeof(c)); }
    // Standby RC
    { uint8_t c[] = {0x80, 0x00}; lora_cmd(c, sizeof(c)); }
    // DC-DC regulator
    { uint8_t c[] = {0x96, 0x01}; lora_cmd(c, sizeof(c)); }
    // Buffer base addresses TX=0, RX=128
    { uint8_t c[] = {0x8F, 0x00, 0x80}; lora_cmd(c, sizeof(c)); }
    // Packet type: LoRa
    { uint8_t c[] = {0x8A, 0x01}; lora_cmd(c, sizeof(c)); }
    // Frequency 868.0 MHz
    { uint8_t c[] = {0x86, 0x36, 0x41, 0x40, 0x00}; lora_cmd(c, sizeof(c)); }
    // PA config: 22dBm SX1262
    { uint8_t c[] = {0x95, 0x04, 0x07, 0x00, 0x01}; lora_cmd(c, sizeof(c)); }
    // TX params: 14dBm (0x0E), 200us ramp
    { uint8_t c[] = {0x8E, 0x0E, 0x04}; lora_cmd(c, sizeof(c)); }
    // Modulation: SF7, BW125, CR4/5, LDRO off
    { uint8_t c[] = {0x8B, 0x07, 0x04, 0x01, 0x00}; lora_cmd(c, sizeof(c)); }
    // Packet params: preamble=8, explicit header, max 255, CRC on, normal IQ
    { uint8_t c[] = {0x8C, 0x00, 0x08, 0x00, 0xFF, 0x01, 0x00}; lora_cmd(c, sizeof(c)); }
    // Calibrate image 868MHz band
    { uint8_t c[] = {0x98, 0xD7, 0xDB}; lora_cmd(c, sizeof(c)); }
    delay(5);
    // DIO IRQ: TxDone | RxDone | Timeout | CrcErr on DIO1
    { uint8_t c[] = {0x08, 0x02, 0x63, 0x02, 0x63, 0x00, 0x00, 0x00, 0x00}; lora_cmd(c, sizeof(c)); }

    // Errata: TX clamp fix
    uint8_t clamp = lora_read_reg(0x08D8);
    lora_write_reg(0x08D8, clamp | 0x1E);
    // OCP 160mA
    lora_write_reg(0x08E7, 0x38);
    // Private sync word
    lora_write_reg(0x0740, 0x14);
    lora_write_reg(0x0741, 0x24);
    // BW125 modulation quality fix
    uint8_t mq = lora_read_reg(0x0889);
    lora_write_reg(0x0889, mq | 0x04);

    lora_clear_irq();
    lora_ready = true;
    Serial.println("[LoRa] SX1262 initialized: 868MHz SF7 BW125 14dBm");
    return true;
}

// RX ring buffer
#define LORA_RX_SLOTS 8
#define LORA_MAX_PKT  255

struct LoRaPkt {
    uint8_t data[LORA_MAX_PKT];
    uint8_t len;
    int16_t rssi;
    int8_t  snr;
    unsigned long timestamp;
};

static LoRaPkt lora_rx_buf[LORA_RX_SLOTS];
static int lora_rx_head = 0;
static int lora_rx_count = 0;
static bool lora_in_rx = false;
static void lora_start_rx() {
    if (!lora_ready) return;
    // IQ polarity fix for RX (normal IQ)
    uint8_t iq = lora_read_reg(0x0736);
    lora_write_reg(0x0736, iq | 0x04);
    // Continuous RX
    uint8_t cmd[] = {0x82, 0xFF, 0xFF, 0xFF};
    lora_cmd(cmd, sizeof(cmd));
    lora_in_rx = true;
}

static void lora_check_rx() {
    if (!lora_ready || !lora_in_rx) return;

    uint16_t irq = lora_get_irq();
    if (!(irq & 0x0002)) return;  // No RxDone

    lora_clear_irq();

    // Check CRC error
    if (irq & 0x0040) {
        lora_start_rx();  // restart
        return;
    }

    // GetRxBufferStatus
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x13);
    SPI.transfer(0x00);
    uint8_t pkt_len = SPI.transfer(0x00);
    uint8_t pkt_offset = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);

    if (pkt_len == 0) {
        lora_start_rx();
        return;
    }

    // GetPacketStatus (RSSI, SNR)
    lora_wait_busy();
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x14);
    SPI.transfer(0x00);
    uint8_t rssi_raw = SPI.transfer(0x00);
    int8_t snr_raw = (int8_t)SPI.transfer(0x00);
    SPI.transfer(0x00);  // signal rssi (ignore)
    digitalWrite(LORA_NSS, HIGH);

    // Store packet
    LoRaPkt *pkt = &lora_rx_buf[lora_rx_head];
    lora_read_buffer(pkt_offset, pkt->data, pkt_len);
    pkt->len = pkt_len;
    pkt->rssi = -(int16_t)rssi_raw / 2;
    pkt->snr = snr_raw / 4;
    pkt->timestamp = millis();
    lora_rx_head = (lora_rx_head + 1) % LORA_RX_SLOTS;
    if (lora_rx_count < LORA_RX_SLOTS) lora_rx_count++;

    event_add("lora rx: %d bytes, rssi=%d snr=%d", pkt_len, pkt->rssi, pkt->snr);

    // Stay in RX
    lora_start_rx();
}

static bool lora_send(const uint8_t *data, size_t len) {
    if (!lora_ready || len == 0 || len > LORA_MAX_PKT) return false;

    // Go to standby first
    { uint8_t c[] = {0x80, 0x00}; lora_cmd(c, sizeof(c)); }
    lora_in_rx = false;

    // Write payload to buffer at offset 0
    lora_write_buffer(0x00, data, len);

    // Update packet params with actual length
    { uint8_t c[] = {0x8C, 0x00, 0x08, 0x00, (uint8_t)len, 0x01, 0x00}; lora_cmd(c, sizeof(c)); }

    // IQ polarity fix for TX
    uint8_t iq = lora_read_reg(0x0736);
    lora_write_reg(0x0736, iq | 0x04);

    // SetTx, timeout 3s
    { uint8_t c[] = {0x83, 0x02, 0xEE, 0x00}; lora_cmd(c, sizeof(c)); }

    // Wait for TxDone or timeout (max 3s)
    unsigned long t = millis();
    while (millis() - t < 3000) {
        uint16_t irq = lora_get_irq();
        if (irq & 0x0001) {  // TxDone
            lora_clear_irq();
            event_add("lora tx: %d bytes", (int)len);
            // Resume RX
            lora_start_rx();
            return true;
        }
        if (irq & 0x0200) {  // Timeout
            lora_clear_irq();
            lora_start_rx();
            return false;
        }
        delay(1);
    }

    lora_clear_irq();
    lora_start_rx();
    return false;
}

// ===== Globals =====
static AsyncWebServer server(HTTP_PORT);
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

    // Peripherals
    doc["has_wifi"] = true;
    doc["has_bluetooth"] = true;
    doc["has_lora"] = hw.has_lora;
    if (hw.has_lora) {
        doc["lora_chip"] = "SX1262";
        doc["lora_pins"] = "NSS=8,RST=12,BUSY=13,DIO1=14,SCK=9,MISO=11,MOSI=10";
    }

    // Battery
    if (hw.has_battery) {
        doc["battery_v"] = serialized(String(hw.battery_v, 2));
    }

    // I2C bus 0
    if (hw.i2c0_count > 0) {
        JsonArray bus0 = doc["i2c_bus0"].to<JsonArray>();
        for (int i = 0; i < hw.i2c0_count; i++) {
            JsonObject dev = bus0.add<JsonObject>();
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", hw.i2c0[i].addr);
            dev["addr"] = String(hex);
            if (hw.i2c0[i].name) dev["device"] = hw.i2c0[i].name;
        }
        doc["i2c_bus0_pins"] = "SDA=17,SCL=18";
    }

    // I2C bus 1
    if (hw.i2c1_count > 0) {
        JsonArray bus1 = doc["i2c_bus1"].to<JsonArray>();
        for (int i = 0; i < hw.i2c1_count; i++) {
            JsonObject dev = bus1.add<JsonObject>();
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", hw.i2c1[i].addr);
            dev["addr"] = String(hex);
            if (hw.i2c1[i].name) dev["device"] = hw.i2c1[i].name;
        }
        doc["i2c_bus1_pins"] = "SDA=5,SCL=6";
    }

    // GPIO: safe pins (not used by LoRa/OLED/flash)
    JsonArray pins = doc["gpio_safe"].to<JsonArray>();
    static const int safe[] = {1, 2, 4, 5, 6, 7, 19, 20, 47, 48};
    for (int i = 0; i < 10; i++) pins.add(safe[i]);

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_ip"] = WiFi.localIP().toString();
        doc["wifi_rssi"] = WiFi.RSSI();
    }
    doc["ap_ssid"] = ap_ssid;
    doc["ap_ip"] = WiFi.softAPIP().toString();

    // Endpoints
    if (lora_ready) {
        doc["lora_freq_mhz"] = 868.0;
        doc["lora_sf"] = 7;
        doc["lora_bw_khz"] = 125;
        doc["lora_tx_dbm"] = 14;
    }

    JsonArray ep = doc["endpoints"].to<JsonArray>();
    const char *eps[] = {
        "/health", "/capabilities", "/config.md", "/events",
        "/firmware/version", "/firmware/upload", "/firmware/apply",
        "/firmware/confirm", "/firmware/rollback",
        "/lora/status", "/lora/send", "/lora/recv",
        "/skill", NULL
    };
    for (int i = 0; eps[i]; i++) ep.add(eps[i]);

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
    request->send(write_spiffs_file(CONFIG_MD_FILE, content) ? 200 : 500,
        "application/json", write_spiffs_file(CONFIG_MD_FILE, content)
        ? "{\"ok\":true}" : "{\"error\":\"write failed\"}");
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
        if (total == 0 || total > 0x330000) {
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

// --- LoRa HTTP handlers ---

static void handle_lora_status(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    JsonDocument doc;
    doc["available"] = hw.has_lora;
    doc["initialized"] = lora_ready;
    doc["receiving"] = lora_in_rx;
    doc["frequency_mhz"] = 868.0;
    doc["sf"] = 7;
    doc["bw_khz"] = 125;
    doc["cr"] = "4/5";
    doc["tx_power_dbm"] = 14;
    doc["rx_buffer_count"] = lora_rx_count;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

static void handle_lora_send(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;
    char *body = (char*)request->_tempObject;
    if (!body) { request->send(400, "application/json", "{\"error\":\"no body\"}"); return; }

    if (!lora_ready) {
        free(body);
        request->_tempObject = nullptr;
        request->send(503, "application/json", "{\"error\":\"lora not initialized\"}");
        return;
    }

    size_t len = 0;
    if (request->hasHeader("Content-Length")) {
        len = strtoul(request->header("Content-Length").c_str(), NULL, 10);
    }
    if (len == 0) len = strlen(body);
    if (len > LORA_MAX_PKT) len = LORA_MAX_PKT;

    bool ok = lora_send((const uint8_t*)body, len);

    free(body);
    request->_tempObject = nullptr;

    JsonDocument doc;
    doc["ok"] = ok;
    doc["bytes"] = (int)len;
    String response;
    serializeJson(doc, response);
    request->send(ok ? 200 : 500, "application/json", response);
}

static void handle_lora_recv(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    int start = (lora_rx_count < LORA_RX_SLOTS) ? 0 : lora_rx_head;
    int count = min(lora_rx_count, LORA_RX_SLOTS);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LORA_RX_SLOTS;
        LoRaPkt *pkt = &lora_rx_buf[idx];
        JsonObject obj = arr.add<JsonObject>();
        // Encode payload as hex
        char hex[LORA_MAX_PKT * 2 + 1];
        for (int j = 0; j < pkt->len && j < LORA_MAX_PKT; j++) {
            snprintf(hex + j * 2, 3, "%02x", pkt->data[j]);
        }
        hex[pkt->len * 2] = '\0';
        obj["hex"] = String(hex);
        // Also try as UTF-8 text
        char text[LORA_MAX_PKT + 1];
        memcpy(text, pkt->data, pkt->len);
        text[pkt->len] = '\0';
        obj["text"] = String(text);
        obj["len"] = pkt->len;
        obj["rssi"] = pkt->rssi;
        obj["snr"] = pkt->snr;
        obj["ms_ago"] = (long)(millis() - pkt->timestamp);
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// --- GET /skill ---

static void handle_skill(AsyncWebServerRequest *request) {
    if (!require_auth(request)) return;

    String ip = WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

    String s = "# ESP32 Seed\n\n";
    s += "Host: " + ip + ":" + String(HTTP_PORT) + "\n";
    s += "mDNS: " + mdns_name + ".local\n";
    s += "AP: " + ap_ssid + "\n\n";
    s += "Auth: `Authorization: Bearer <token>` (except /health)\n\n";
    s += "## Grow cycle\n\n";
    s += "ESP32 has no compiler. Build on host, upload binary:\n\n";
    s += "1. GET /capabilities\n";
    s += "2. Write firmware (PlatformIO/Arduino/ESP-IDF)\n";
    s += "3. Compile: `pio run -e heltec_v3`\n";
    s += "4. POST /firmware/upload — send .bin (`-H 'Content-Type: application/octet-stream'`)\n";
    s += "5. POST /firmware/apply — reboot\n";
    s += "6. GET /health — verify\n";
    s += "7. POST /firmware/confirm (or auto after 60s)\n\n";
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
    s += "| GET | /lora/status | LoRa radio status |\n";
    s += "| POST | /lora/send | Send LoRa packet (body = payload) |\n";
    s += "| GET | /lora/recv | Last received packets |\n";
    s += "| GET | /skill | This file |\n";

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
    server.on("/lora/status", HTTP_GET, handle_lora_status);
    server.on("/lora/send", HTTP_POST, handle_lora_send, NULL, handle_body_collect);
    server.on("/lora/recv", HTTP_GET, handle_lora_recv);
    server.on("/skill", HTTP_GET, handle_skill);
    server.on("/", HTTP_GET, handle_wifi_page);
    server.on("/wifi/config", HTTP_POST, handle_wifi_post);
}

// ===== Main =====

void setup() {
    Serial.begin(115200);
    delay(500);
    boot_time = millis();

    if (!SPIFFS.begin(true)) {
        Serial.println("[!] SPIFFS failed");
    }

    hw_probe();
    token_load();
    wifi_setup();
    setup_routes();
    server.begin();

    // Init LoRa if detected
    if (hw.has_lora) {
        lora_init();
        lora_start_rx();
    }

    Serial.printf("\nESP32 Seed v%s\n", SEED_VERSION);
    Serial.printf("Token: %s\n", auth_token.c_str());
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("http://%s:%d/health\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    Serial.printf("http://%s:%d/health  (AP: %s)\n",
        WiFi.softAPIP().toString().c_str(), HTTP_PORT, ap_ssid.c_str());

    event_add("seed started v%s", SEED_VERSION);
}

void loop() {
    // Poll LoRa RX
    lora_check_rx();

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

    delay(10);
}
