// T-Lora Pager UI — ST7796 + AW9364 + XL9555 + tembed-style clock face.
//
// Pin map + ST7796 init: LilyGoLib / pins_arduino.h.
// Clock face palette + layout language: seeds/esp32-tembed (COL_TIME/ACCENT/…).
// Scaled for 480x222 landscape.

#include "hw_ui.h"
#include "board_pins.h"

#include <SPI.h>
#include <Wire.h>
#include <string.h>
#include <stdio.h>

// ---- palette (from tembed clock face, RGB565) ------------------------------
static const uint16_t COL_BG     = 0x0000;
static const uint16_t COL_TIME   = 0xFFBC;  // warm white
static const uint16_t COL_DIM    = 0x7BD0;  // slate
static const uint16_t COL_ACCENT = 0xFD05;  // amber
static const uint16_t COL_RULE   = 0x2945;  // hairline
static const uint16_t COL_INFO   = 0x2DD5;  // teal
static const uint16_t COL_CRIT   = 0xE1C5;  // red
static const uint16_t COL_WARN   = 0xFD20;  // orange
static const uint16_t COL_FG     = 0xFFFF;

// Layout for 480x222 (tembed was 320x170 — same bones, more air)
static const int HDR_Y    = 4;
static const int RULE_Y   = 22;
static const int CLOCK_Y  = 36;
static const int DATE_Y   = 118;
static const int ROW1_Y   = 160;
static const int ROW2_Y   = 184;
static const int BAR_Y    = 212;
static const int BAR_H    = 4;
static const int MARGIN   = 12;

// ---- XL9555 ----------------------------------------------------------------
static bool xl_ok = false;
static uint8_t xl_out0 = 0xFF, xl_out1 = 0xFF;

static bool xl_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}
static bool xl_read_reg(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)XL9555_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}
static void xl_set_pin(uint8_t pin, bool high) {
    if (pin < 8) {
        if (high) xl_out0 |= (1u << pin); else xl_out0 &= ~(1u << pin);
        xl_write_reg(0x02, xl_out0);
    } else if (pin < 16) {
        uint8_t b = pin - 8;
        if (high) xl_out1 |= (1u << b); else xl_out1 &= ~(1u << b);
        xl_write_reg(0x03, xl_out1);
    }
}
static bool xl_begin() {
    uint8_t id = 0;
    if (!xl_read_reg(0x06, &id)) return false;
    uint8_t cfg0 = (1u << 6);
    uint8_t cfg1 = (1u << (EXP_SD_DET - 8)) | (1u << (EXP_SD_PULLEN - 8)) | 0xE0;
    if (!xl_write_reg(0x06, cfg0) || !xl_write_reg(0x07, cfg1)) return false;
    xl_out0 = xl_out1 = 0xFF;
    xl_write_reg(0x02, xl_out0);
    xl_write_reg(0x03, xl_out1);
    const uint8_t hi[] = {
        EXP_DRV_EN, EXP_AMP_EN, EXP_KB_RST, EXP_LORA_EN,
        EXP_GPS_EN, EXP_NFC_EN, EXP_GPS_RST, EXP_KB_EN,
        EXP_GPIO_EN, EXP_SD_EN,
    };
    for (uint8_t p : hi) { xl_set_pin(p, true); delay(1); }
    return true;
}

// ---- AW9364 ----------------------------------------------------------------
static uint8_t bl_level = 0;
static void bl_begin() {
    pinMode(PIN_DISP_BL, OUTPUT);
    digitalWrite(PIN_DISP_BL, LOW);
    bl_level = 0;
}
static void bl_set(uint8_t value) {
    if (value > 16) value = 16;
    if (value == bl_level) return;
    if (value == 0) { digitalWrite(PIN_DISP_BL, LOW); bl_level = 0; return; }
    if (bl_level == 0) { digitalWrite(PIN_DISP_BL, HIGH); bl_level = 16; }
    int from = 16 - bl_level, to = 16 - value;
    int num = (16 + to - from) % 16;
    for (int i = 0; i < num; i++) {
        digitalWrite(PIN_DISP_BL, LOW);
        digitalWrite(PIN_DISP_BL, HIGH);
    }
    bl_level = value;
}

// ---- ST7796 ----------------------------------------------------------------
static SPIClass *disp_spi = nullptr;
static bool panel_ok = false;
static HwUiScreen screen = HW_UI_CLOCK;
static const uint32_t DISP_SPI_HZ = 40000000;
static const uint16_t PANEL_W = DISP_LANDSCAPE_W;
static const uint16_t PANEL_H = DISP_LANDSCAPE_H;
static const uint16_t PANEL_OFF_X = 0;
static const uint16_t PANEL_OFF_Y = DISP_GAP_Y;

struct Cmd { uint8_t cmd; uint8_t data[16]; uint8_t len; };
static const Cmd ST7796_INIT[] = {
    {0x01, {0x00}, 0x80}, {0x11, {0x00}, 0x80},
    {0xF0, {0xC3}, 0x01}, {0xF0, {0xC3}, 0x01}, {0xF0, {0x96}, 0x01},
    {0x36, {0x48}, 0x01}, {0x3A, {0x55}, 0x01}, {0xB4, {0x01}, 0x01},
    {0xB6, {0x80, 0x02, 0x3B}, 0x03},
    {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 0x08},
    {0xC1, {0x06}, 0x01}, {0xC2, {0xA7}, 0x01}, {0xC5, {0x18}, 0x81},
    {0xE0, {0xF0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 0x0F},
    {0xE1, {0xE0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 0x8F},
    {0xF0, {0x3c}, 0x01}, {0xF0, {0x69}, 0x81},
    {0x21, {0x00}, 0x01}, {0x29, {0x00}, 0x01},
};
static const uint8_t MADCTL_LANDSCAPE = 0xE8;

static void park_spi_cs() {
    const int cs[] = {PIN_LORA_CS, PIN_LORA_RST, PIN_NFC_CS, PIN_SD_CS, PIN_DISP_CS};
    for (int p : cs) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
}
static void tft_cmd(uint8_t c) {
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, LOW);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));
    disp_spi->write(c);
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}
static void tft_data(const uint8_t *d, size_t n) {
    if (!n) return;
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, HIGH);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));
    disp_spi->writeBytes(d, n);
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}
static void tft_data8(uint8_t v) { tft_data(&v, 1); }
static void tft_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += PANEL_OFF_X; x1 += PANEL_OFF_X;
    y0 += PANEL_OFF_Y; y1 += PANEL_OFF_Y;
    uint8_t caset[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t raset[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    tft_cmd(0x2A); tft_data(caset, 4);
    tft_cmd(0x2B); tft_data(raset, 4);
    tft_cmd(0x2C);
}
static void tft_fill(uint16_t color) {
    tft_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, HIGH);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));
    uint8_t pix[2] = {(uint8_t)(color >> 8), (uint8_t)color};
    const size_t chunk = 64;
    uint8_t buf[chunk * 2];
    for (size_t i = 0; i < chunk; i++) { buf[i*2] = pix[0]; buf[i*2+1] = pix[1]; }
    uint32_t total = (uint32_t)PANEL_W * PANEL_H;
    while (total >= chunk) { disp_spi->writeBytes(buf, chunk * 2); total -= chunk; }
    if (total) disp_spi->writeBytes(buf, total * 2);
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}
static void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (!w || !h || x >= PANEL_W || y >= PANEL_H) return;
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    tft_window(x, y, x + w - 1, y + h - 1);
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, HIGH);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));
    uint8_t pix[2] = {(uint8_t)(color >> 8), (uint8_t)color};
    uint32_t total = (uint32_t)w * h;
    // Faster: 32-pixel stripe
    uint8_t stripe[64];
    for (int i = 0; i < 32; i++) { stripe[i*2] = pix[0]; stripe[i*2+1] = pix[1]; }
    while (total >= 32) { disp_spi->writeBytes(stripe, 64); total -= 32; }
    while (total--) disp_spi->writeBytes(pix, 2);
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}
static void tft_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color) {
    tft_fill_rect(x, y, w, 1, color);
}
static void tft_fill_circle(int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) tft_fill_rect(cx + dx, cy + dy, 1, 1, color);
        }
    }
}
static uint16_t blend565(uint8_t a, uint16_t fg, uint16_t bg) {
    // a = 0..255 weight of fg over bg
    uint8_t fr = (fg >> 11) & 0x1F, fg_ = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    uint8_t br = (bg >> 11) & 0x1F, bg_ = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint8_t r = (fr * a + br * (255 - a)) / 255;
    uint8_t g = (fg_ * a + bg_ * (255 - a)) / 255;
    uint8_t b = (fb * a + bb * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// 5x7 GLCD font (ASCII 0x20..0x7E)
static const uint8_t FONT5X7[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5F,0x00,0x00,0x00,0x07,0x00,0x07,0x00,
    0x14,0x7F,0x14,0x7F,0x14,0x24,0x2A,0x7F,0x2A,0x12,0x23,0x13,0x08,0x64,0x62,
    0x36,0x49,0x55,0x22,0x50,0x00,0x05,0x03,0x00,0x00,0x00,0x1C,0x22,0x41,0x00,
    0x00,0x41,0x22,0x1C,0x00,0x08,0x2A,0x1C,0x2A,0x08,0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x60,0x60,0x00,0x00,
    0x20,0x10,0x08,0x04,0x02,0x3E,0x51,0x49,0x45,0x3E,0x00,0x42,0x7F,0x40,0x00,
    0x42,0x61,0x51,0x49,0x46,0x21,0x41,0x45,0x4B,0x31,0x18,0x14,0x12,0x7F,0x10,
    0x27,0x45,0x45,0x45,0x39,0x3C,0x4A,0x49,0x49,0x30,0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36,0x06,0x49,0x49,0x29,0x1E,0x00,0x36,0x36,0x00,0x00,
    0x00,0x56,0x36,0x00,0x00,0x00,0x08,0x14,0x22,0x41,0x14,0x14,0x14,0x14,0x14,
    0x41,0x22,0x14,0x08,0x00,0x02,0x01,0x51,0x09,0x06,0x32,0x49,0x79,0x41,0x3E,
    0x7E,0x11,0x11,0x11,0x7E,0x7F,0x49,0x49,0x49,0x36,0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C,0x7F,0x49,0x49,0x49,0x41,0x7F,0x09,0x09,0x01,0x01,
    0x3E,0x41,0x41,0x51,0x32,0x7F,0x08,0x08,0x08,0x7F,0x00,0x41,0x7F,0x41,0x00,
    0x20,0x40,0x41,0x3F,0x01,0x7F,0x08,0x14,0x22,0x41,0x7F,0x40,0x40,0x40,0x40,
    0x7F,0x02,0x04,0x02,0x7F,0x7F,0x04,0x08,0x10,0x7F,0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06,0x3E,0x41,0x51,0x21,0x5E,0x7F,0x09,0x19,0x29,0x46,
    0x46,0x49,0x49,0x49,0x31,0x01,0x01,0x7F,0x01,0x01,0x3F,0x40,0x40,0x40,0x3F,
    0x1F,0x20,0x40,0x20,0x1F,0x7F,0x20,0x18,0x20,0x7F,0x63,0x14,0x08,0x14,0x63,
    0x03,0x04,0x78,0x04,0x03,0x61,0x51,0x49,0x45,0x43,0x00,0x00,0x7F,0x41,0x41,
    0x02,0x04,0x08,0x10,0x20,0x41,0x41,0x7F,0x00,0x00,0x04,0x02,0x01,0x02,0x04,
    0x40,0x40,0x40,0x40,0x40,0x00,0x01,0x02,0x04,0x00,0x20,0x54,0x54,0x54,0x78,
    0x7F,0x48,0x44,0x44,0x38,0x38,0x44,0x44,0x44,0x20,0x38,0x44,0x44,0x48,0x7F,
    0x38,0x54,0x54,0x54,0x18,0x08,0x7E,0x09,0x01,0x02,0x08,0x14,0x54,0x54,0x3C,
    0x7F,0x08,0x04,0x04,0x78,0x00,0x44,0x7D,0x40,0x00,0x20,0x40,0x44,0x3D,0x00,
    0x00,0x7F,0x10,0x28,0x44,0x00,0x41,0x7F,0x40,0x00,0x7C,0x04,0x18,0x04,0x78,
    0x7C,0x08,0x04,0x04,0x78,0x38,0x44,0x44,0x44,0x38,0x7C,0x14,0x14,0x14,0x08,
    0x08,0x14,0x14,0x18,0x7C,0x7C,0x08,0x04,0x04,0x08,0x48,0x54,0x54,0x54,0x20,
    0x04,0x3F,0x44,0x40,0x20,0x3C,0x40,0x40,0x20,0x7C,0x1C,0x20,0x40,0x20,0x1C,
    0x3C,0x40,0x30,0x40,0x3C,0x44,0x28,0x10,0x28,0x44,0x0C,0x50,0x50,0x50,0x3C,
    0x44,0x64,0x54,0x4C,0x44,0x00,0x08,0x36,0x41,0x00,0x00,0x00,0x7F,0x00,0x00,
    0x00,0x41,0x36,0x08,0x00,0x08,0x04,0x08,0x10,0x08,
};

// ---- Cyrillic 5x7 (hand pixel art, same weight as FONT5X7 Latin) -----------
// Column-major, bit0 = top. Where the letter is a Latin twin, use the exact
// FONT5X7 glyph (А=A, В=B, Е=E, …) so stroke weight matches the panel.
// Unique letters (Д д Л л …) redrawn as classic 5×7 matrix forms.
static const uint8_t FONT_CYR_YO_UP[5] PROGMEM = { 0x7C,0x55,0x54,0x55,0x44 }; // Ё
static const uint8_t FONT_CYR_YO_LO[5] PROGMEM = { 0x38,0x55,0x54,0x55,0x18 }; // ё
static const uint8_t FONT_CYR_UP[32 * 5] PROGMEM = { // А-Я
    0x7E,0x11,0x11,0x11,0x7E, // А (=A)
    0x7F,0x49,0x49,0x49,0x31, // Б
    0x7F,0x49,0x49,0x49,0x36, // В (=B)
    0x7F,0x01,0x01,0x01,0x01, // Г
    0x60,0x3E,0x21,0x3F,0x60, // Д  ..##. / .#.#. ×4 / ##### / #...#
    0x7F,0x49,0x49,0x49,0x41, // Е (=E)
    0x63,0x1C,0x77,0x1C,0x63, // Ж
    0x41,0x49,0x49,0x49,0x36, // З
    0x7F,0x10,0x08,0x04,0x7F, // И
    0x7C,0x21,0x12,0x09,0x7C, // Й
    0x7F,0x08,0x14,0x22,0x41, // К (=K)
    0x40,0x3E,0x01,0x01,0x7F, // Л
    0x7F,0x02,0x0C,0x02,0x7F, // М (=M)
    0x7F,0x08,0x08,0x08,0x7F, // Н (=H)
    0x3E,0x41,0x41,0x41,0x3E, // О (=O)
    0x7F,0x01,0x01,0x01,0x7F, // П
    0x7F,0x09,0x09,0x09,0x06, // Р (=P)
    0x3E,0x41,0x41,0x41,0x22, // С (=C)
    0x01,0x01,0x7F,0x01,0x01, // Т (=T)
    // У — not Latin Y: V arms + stem with left hook (γ), classic matrix form
    0x43,0x24,0x18,0x04,0x03, // У
    0x0C,0x12,0x7F,0x12,0x0C, // Ф
    0x63,0x14,0x08,0x14,0x63, // Х (=X)
    0x3F,0x20,0x20,0x3F,0x60, // Ц
    0x07,0x08,0x08,0x08,0x7F, // Ч
    0x7F,0x40,0x7C,0x40,0x7F, // Ш
    0x3F,0x20,0x3F,0x20,0x7F, // Щ
    0x01,0x7F,0x48,0x48,0x30, // Ъ
    0x7F,0x48,0x48,0x30,0x7F, // Ы
    0x7F,0x48,0x48,0x48,0x30, // Ь
    0x41,0x49,0x49,0x49,0x36, // Э
    0x7F,0x08,0x3E,0x41,0x3E, // Ю
    0x46,0x29,0x19,0x09,0x7F, // Я
};
static const uint8_t FONT_CYR_LO[32 * 5] PROGMEM = { // а-я
    0x20,0x54,0x54,0x54,0x78, // а (=a)
    0x7E,0x49,0x49,0x49,0x30, // б
    0x7C,0x54,0x54,0x54,0x28, // в
    0x7C,0x04,0x04,0x04,0x04, // г
    0x60,0x3C,0x22,0x3E,0x60, // д  same skeleton as Д, x-height
    0x38,0x54,0x54,0x54,0x18, // е (=e)
    0x6C,0x10,0x7C,0x10,0x6C, // ж
    0x44,0x54,0x54,0x54,0x28, // з
    0x7C,0x20,0x10,0x08,0x7C, // и
    0x7C,0x21,0x12,0x09,0x7C, // й
    0x7C,0x10,0x28,0x44,0x00, // к
    0x40,0x38,0x04,0x04,0x7C, // л
    0x7C,0x08,0x10,0x08,0x7C, // м
    0x7C,0x10,0x10,0x10,0x7C, // н
    0x38,0x44,0x44,0x44,0x38, // о (=o)
    0x7C,0x04,0x04,0x04,0x7C, // п
    0x7C,0x14,0x14,0x14,0x08, // р (~p)
    0x38,0x44,0x44,0x44,0x20, // с (~c)
    0x04,0x04,0x7C,0x04,0x04, // т
    // у — same γ-hook as У, not Latin y
    0x0C,0x50,0x20,0x10,0x0C, // у
    0x18,0x24,0x7E,0x24,0x18, // ф
    0x44,0x28,0x10,0x28,0x44, // х (=x)
    0x3C,0x20,0x20,0x3C,0x60, // ц
    0x0C,0x10,0x10,0x10,0x7C, // ч
    0x7C,0x40,0x78,0x40,0x7C, // ш
    0x3C,0x20,0x3C,0x20,0x7C, // щ
    0x04,0x7C,0x50,0x50,0x20, // ъ
    0x7C,0x50,0x50,0x20,0x7C, // ы
    0x7C,0x50,0x50,0x50,0x20, // ь
    0x44,0x54,0x54,0x54,0x28, // э
    0x7C,0x10,0x38,0x44,0x38, // ю
    0x48,0x54,0x34,0x14,0x7C, // я
};

// Decode one UTF-8 codepoint; returns bytes consumed (0 on empty/invalid).
static int utf8_next(const char *s, uint32_t *cp) {
    if (!s || !s[0]) return 0;
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        if (cp) *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x07) << 18) |
                      ((uint32_t)(p[1] & 0x3F) << 12) |
                      ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    // Invalid lead — skip one byte as '?'
    if (cp) *cp = (uint32_t)'?';
    return 1;
}

// Resolve glyph pointer for a Unicode codepoint (5 bytes each).
static const uint8_t *font_glyph(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E)
        return &FONT5X7[(cp - 0x20) * 5];
    if (cp == 0x0401) return FONT_CYR_YO_UP;
    if (cp == 0x0451) return FONT_CYR_YO_LO;
    if (cp >= 0x0410 && cp <= 0x042F)
        return &FONT_CYR_UP[(cp - 0x0410) * 5];
    if (cp >= 0x0430 && cp <= 0x044F)
        return &FONT_CYR_LO[(cp - 0x0430) * 5];
    return &FONT5X7[('?' - 0x20) * 5];
}

// Draw one glyph by pushing a pre-scaled bitmap window (fast path).
static void tft_draw_glyph(uint16_t x, uint16_t y, uint32_t cp,
                           uint16_t fg, uint16_t bg, uint8_t scale) {
    if (scale < 1) scale = 1;
    const uint8_t *g = font_glyph(cp);
    const uint16_t gw = 6 * scale;   // 5 cols + 1 gap
    const uint16_t gh = 7 * scale;
    if (x >= PANEL_W || y >= PANEL_H) return;

    uint8_t rowbuf[6 * 8 * 2];
    if (gw * 2 > sizeof(rowbuf)) return;

    tft_window(x, y, x + gw - 1, y + gh - 1);
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, HIGH);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));

    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&g[col]);
            uint16_t color = (bits & (1u << row)) ? fg : bg;
            uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
            for (uint8_t sx = 0; sx < scale; sx++) {
                size_t i = (col * scale + sx) * 2;
                rowbuf[i] = hi; rowbuf[i + 1] = lo;
            }
        }
        uint8_t bhi = (uint8_t)(bg >> 8), blo = (uint8_t)bg;
        for (uint8_t sx = 0; sx < scale; sx++) {
            size_t i = (5 * scale + sx) * 2;
            rowbuf[i] = bhi; rowbuf[i + 1] = blo;
        }
        for (uint8_t sy = 0; sy < scale; sy++) {
            disp_spi->writeBytes(rowbuf, gw * 2);
        }
    }
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}

static void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    tft_draw_glyph(x, y, (unsigned char)c, fg, bg, scale);
}

// UTF-8 text. Each codepoint advances one cell (6*scale px).
static void tft_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!s) return;
    uint16_t adv = 6 * scale;
    while (*s) {
        if (x + adv > PANEL_W) break;
        uint32_t cp = 0;
        int n = utf8_next(s, &cp);
        if (n <= 0) break;
        tft_draw_glyph(x, y, cp, fg, bg, scale);
        x += adv;
        s += n;
    }
}


// Text width helper — one cell per UTF-8 codepoint (not per byte).
static uint16_t text_width(const char *s, uint8_t scale) {
    if (!s) return 0;
    int n = 0;
    while (*s) {
        uint32_t cp;
        int k = utf8_next(s, &cp);
        if (k <= 0) break;
        s += k;
        n++;
    }
    return (uint16_t)(n * 6 * scale);
}

// Centered text
static void tft_draw_text_c(uint16_t cx, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    uint16_t w = text_width(s, scale);
    uint16_t x = (cx > w / 2) ? (cx - w / 2) : 0;
    tft_draw_text(x, y, s, fg, bg, scale);
}

// Right-aligned
static void tft_draw_text_r(uint16_t right, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    uint16_t w = text_width(s, scale);
    uint16_t x = (right > w) ? (right - w) : 0;
    tft_draw_text(x, y, s, fg, bg, scale);
}

static bool panel_begin() {
    park_spi_cs();
    pinMode(PIN_DISP_DC, OUTPUT); digitalWrite(PIN_DISP_DC, HIGH);
    pinMode(PIN_DISP_CS, OUTPUT); digitalWrite(PIN_DISP_CS, HIGH);
    static SPIClass spi(FSPI);
    disp_spi = &spi;
    disp_spi->begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    for (const Cmd &c : ST7796_INIT) {
        tft_cmd(c.cmd);
        uint8_t n = c.len & 0x1F;
        if (n) tft_data(c.data, n);
        if (c.len & 0x80) delay(120);
    }
    tft_cmd(0x36); tft_data8(MADCTL_LANDSCAPE);
    tft_fill(COL_BG);
    panel_ok = true;
    return true;
}

// ---- clock field cache -----------------------------------------------------
static char fld_ver[12]   = "";
static char fld_batt[16]  = "";
static char fld_addr[24]  = "";
static char fld_time[8]   = "";
static char fld_sec[4]    = "";
static char fld_date[28]  = "";
static char fld_left[36]  = "";
static char fld_right[36] = "";
static char fld_note[56]  = "";
static int  clock_badge   = -1;
static int  clock_mesh_ui = -1;
static int  clock_mesh_age_bucket = -2;  // -2 dirty; -1 never; else minutes (or hours*1000)
static bool clock_dirty   = true;

// Selection caches for partial repaint (menu / msglist / card_act / agents).
static int menu_sel_drawn = -1;
static int card_act_sel_drawn = -1;
static int agents_sel_drawn = -1;
static int agent_act_sel_drawn = -1;
static int layout_sel_drawn = -1;
static int layout_cur_drawn = -1;
static int mesh_sel_drawn = -1;
static int msglist_sel_drawn = -1;
static int msglist_top_drawn = -1;
static int msglist_count_drawn = -1;
static uint8_t msglist_unread_mask = 0;  // bit i = unread for partial-redraw guard
static int clock_bar_drawn = -1;  // progress band; -1 = bare ground

// Geometry of big time block (measured once at first paint)
static int32_t clock_x = 0, sec_x = 0, sec_y = 0;
static const uint8_t TIME_SCALE = 7;   // HH:MM → ~42px tall
static const uint8_t SEC_SCALE  = 3;   // ss   → ~21px tall

static void clock_measure() {
    // "00:00" = 5 glyphs at TIME_SCALE, "00" = 2 at SEC_SCALE, 12px gap
    int tw = 5 * 6 * TIME_SCALE;
    int sw = 2 * 6 * SEC_SCALE;
    int total = tw + 16 + sw;
    clock_x = (PANEL_W - total) / 2;
    if (clock_x < MARGIN) clock_x = MARGIN;
    sec_x = clock_x + tw + 16;
    sec_y = CLOCK_Y + (7 * TIME_SCALE) - (7 * SEC_SCALE);  // baseline align
}

// Draw field only when text changes (opaque bg erases previous).
static void draw_field(char *cache, size_t n, const char *text,
                       int32_t x, int32_t y, uint8_t scale, uint16_t color,
                       char align /* 'L','C','R' */, uint16_t pad_px) {
    if (!clock_dirty && strncmp(cache, text, n - 1) == 0) return;
    snprintf(cache, n, "%s", text);

    // Erase pad region then draw
    uint16_t tw = text_width(text, scale);
    uint16_t th = 7 * scale;
    int32_t ex = x;
    if (align == 'C') ex = x - (int32_t)pad_px / 2;
    else if (align == 'R') ex = x - (int32_t)pad_px;
    if (ex < 0) ex = 0;
    tft_fill_rect((uint16_t)ex, (uint16_t)y, pad_px, th + 2, COL_BG);

    if (align == 'C') tft_draw_text_c((uint16_t)x, (uint16_t)y, text, color, COL_BG, scale);
    else if (align == 'R') tft_draw_text_r((uint16_t)x, (uint16_t)y, text, color, COL_BG, scale);
    else tft_draw_text((uint16_t)x, (uint16_t)y, text, color, COL_BG, scale);
    (void)tw;
}

// ---- public ----------------------------------------------------------------
SPIClass *hw_ui_spi() { return disp_spi; }

bool hw_ui_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    xl_ok = xl_begin();
    Serial.printf("[hw] XL9555 %s\n", xl_ok ? "ok" : "MISSING");
    bl_begin();
    if (!panel_begin()) return false;
    bl_set(12);
    clock_measure();
    screen = HW_UI_CLOCK;
    tft_fill(COL_BG);
    tft_draw_text(MARGIN, 40, "T-Lora Pager", COL_ACCENT, COL_BG, 2);
    tft_draw_text(MARGIN, 80, "booting...", COL_DIM, COL_BG, 2);
    Serial.printf("[hw] panel %ux%u, clock face ready\n", PANEL_W, PANEL_H);
    return true;
}

bool hw_ui_ready() { return panel_ok; }
bool hw_ui_expand_ok() { return xl_ok; }
void hw_xl_pin(uint8_t exp_pin, bool high) {
    if (!xl_ok) return;
    xl_set_pin(exp_pin, high);
}
HwUiScreen hw_ui_screen() { return screen; }
void hw_ui_set_brightness(uint8_t level) { bl_set(level); }
uint8_t hw_ui_get_brightness() { return bl_level; }

void hw_ui_invalidate_clock() {
    clock_dirty = true;
    fld_batt[0] = '\0';
    fld_ver[0] = fld_addr[0] = fld_time[0] = fld_sec[0] = '\0';
    fld_date[0] = fld_left[0] = fld_right[0] = fld_note[0] = '\0';
    clock_badge = -1;
    clock_mesh_ui = -1;
    clock_mesh_age_bucket = -2;
    clock_bar_drawn = -1;
}

void hw_ui_show_clock() {
    if (!panel_ok) return;
    screen = HW_UI_CLOCK;
    // Leaving interactive faces: next visit must full-paint.
    menu_sel_drawn = -1;
    card_act_sel_drawn = -1;
    agents_sel_drawn = -1;
    agent_act_sel_drawn = -1;
    layout_sel_drawn = -1;
    layout_cur_drawn = -1;
    mesh_sel_drawn = -1;
    msglist_sel_drawn = -1;
    msglist_top_drawn = -1;
    msglist_count_drawn = -1;
    msglist_unread_mask = 0;
    tft_fill(COL_BG);
    tft_hline(MARGIN, RULE_Y, PANEL_W - 2 * MARGIN, COL_RULE);
    hw_ui_invalidate_clock();
    // First paint is done by the next clock_tick from main.
}

void hw_ui_clock_tick(const char *version,
                      const char *batt,
                      const char *ip_or_status,
                      const char *row1l,
                      const char *row1r,
                      const char *row2,
                      int unread,
                      bool time_ok,
                      int hour, int minute, int second,
                      const char *date_str,
                      bool crit_unread,
                      int mesh_ui,
                      int mesh_age_s) {
    if (!panel_ok || screen != HW_UI_CLOCK) return;
    (void)crit_unread;

    // Header: version | badge | M+age | BAT | IP
    draw_field(fld_ver, sizeof(fld_ver), version ? version : "",
               MARGIN, HDR_Y, 2, COL_DIM, 'L', 80);
    draw_field(fld_batt, sizeof(fld_batt), batt ? batt : "",
               PANEL_W / 2, HDR_Y, 2, COL_DIM, 'C', 96);
    draw_field(fld_addr, sizeof(fld_addr), ip_or_status ? ip_or_status : "offline",
               PANEL_W - MARGIN, HDR_Y, 2, COL_DIM, 'R', 160);

    // Unread badge — amber dot + count between version and BAT
    int badge = unread > 9 ? 10 : unread;
    if (clock_dirty || badge != clock_badge) {
        clock_badge = badge;
        const int bx = 100, by = HDR_Y + 6;
        tft_fill_rect(bx - 6, HDR_Y, 40, 16, COL_BG);
        if (badge > 0) {
            tft_fill_circle(bx, by, 3, COL_ACCENT);
            char b[8];
            if (badge > 9) snprintf(b, sizeof(b), "9+");
            else snprintf(b, sizeof(b), "%d", badge);
            tft_draw_text(bx + 8, HDR_Y, b, COL_ACCENT, COL_BG, 2);
        }
    }

    // MeshCore "M" + alive age since last private-path success (ACK / inbound).
    // Age bucket: minutes 0..59, then hours as 1000+h so we repaint once per
    // minute (or hour) without redrawing every second.
    int age_bucket = -1;
    if (mesh_age_s >= 0) {
        if (mesh_age_s < 3600) age_bucket = mesh_age_s / 60;           // 0..59 min
        else if (mesh_age_s < 86400) age_bucket = 1000 + mesh_age_s / 3600;  // hours
        else age_bucket = 2000 + mesh_age_s / 86400;                  // days
    }
    if (clock_dirty || mesh_ui != clock_mesh_ui ||
        age_bucket != clock_mesh_age_bucket) {
        clock_mesh_ui = mesh_ui;
        clock_mesh_age_bucket = age_bucket;
        // Left of BAT; wider slot for "M12m" / "M2h"
        const int mx = PANEL_W / 2 - 108;
        tft_fill_rect(mx - 2, HDR_Y, 56, 16, COL_BG);
        if (mesh_ui > MESH_UI_OFF) {
            uint16_t col = COL_DIM;
            if (mesh_ui == MESH_UI_OK) col = COL_INFO;
            else if (mesh_ui == MESH_UI_STALE) col = COL_WARN;
            else if (mesh_ui == MESH_UI_DOWN) col = COL_CRIT;
            char label[10];
            if (mesh_age_s < 0) {
                snprintf(label, sizeof(label), "M--");
            } else if (mesh_age_s < 60) {
                snprintf(label, sizeof(label), "M0m");
            } else if (mesh_age_s < 3600) {
                snprintf(label, sizeof(label), "M%um",
                         (unsigned)(mesh_age_s / 60));
            } else if (mesh_age_s < 86400) {
                snprintf(label, sizeof(label), "M%uh",
                         (unsigned)(mesh_age_s / 3600));
            } else {
                snprintf(label, sizeof(label), "M%ud",
                         (unsigned)(mesh_age_s / 86400));
            }
            tft_draw_text((uint16_t)mx, HDR_Y, label, col, COL_BG, 2);
        }
    }

    // Big time
    char hhmm[8], ss[4];
    if (time_ok) {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", hour, minute);
        snprintf(ss, sizeof(ss), "%02d", second);
    } else {
        snprintf(hhmm, sizeof(hhmm), "--:--");
        snprintf(ss, sizeof(ss), "--");
    }
    draw_field(fld_time, sizeof(fld_time), hhmm,
               clock_x, CLOCK_Y, TIME_SCALE, COL_TIME, 'L',
               (uint16_t)(5 * 6 * TIME_SCALE + 4));
    draw_field(fld_sec, sizeof(fld_sec), ss,
               sec_x, sec_y, SEC_SCALE, COL_ACCENT, 'L',
               (uint16_t)(2 * 6 * SEC_SCALE + 4));

    // Date
    draw_field(fld_date, sizeof(fld_date),
               date_str && date_str[0] ? date_str : "waiting for NTP",
               PANEL_W / 2, DATE_Y, 2, COL_DIM, 'C', 320);

    // Bottom rows
    draw_field(fld_left, sizeof(fld_left), row1l ? row1l : "",
               MARGIN, ROW1_Y, 2, COL_DIM, 'L', 220);
    draw_field(fld_right, sizeof(fld_right), row1r ? row1r : "",
               PANEL_W - MARGIN, ROW1_Y, 2, COL_DIM, 'R', 220);
    draw_field(fld_note, sizeof(fld_note), row2 ? row2 : "",
               PANEL_W / 2, ROW2_Y, 2, COL_DIM, 'C', 400);

    clock_dirty = false;
}

void hw_ui_clock_rule_tick(bool crit_unread) {
    if (!panel_ok || screen != HW_UI_CLOCK) return;
    static unsigned long last_step = 0;
    static bool was_breathing = false;
    if (millis() - last_step < 80) return;
    last_step = millis();

    if (!crit_unread) {
        if (was_breathing) {
            was_breathing = false;
            tft_hline(MARGIN, RULE_Y, PANEL_W - 2 * MARGIN, COL_RULE);
        }
        return;
    }
    was_breathing = true;
    unsigned long pos = millis() % 2000;
    unsigned long half = 1000;
    unsigned long up = (pos < half) ? pos : (2000 - pos);
    uint8_t alpha = (uint8_t)(up * 160 / half);
    tft_hline(MARGIN, RULE_Y, PANEL_W - 2 * MARGIN,
              blend565(alpha, COL_CRIT, COL_RULE));
}

// Progress bar: only paint the delta, same idea as tembed clock_bar_draw.
void hw_ui_clock_bar(int pct) {
    if (!panel_ok || screen != HW_UI_CLOCK) return;
    const uint16_t bx = MARGIN;
    const uint16_t by = BAR_Y;
    const uint16_t bw = PANEL_W - 2 * MARGIN;
    const uint16_t bh = BAR_H;

    if (pct < 0) {
        if (clock_bar_drawn < 0) return;
        tft_fill_rect(bx, by, bw, bh, COL_BG);
        clock_bar_drawn = -1;
        return;
    }
    if (pct > 100) pct = 100;
    int w = (int)((uint32_t)bw * (uint32_t)pct / 100u);

    if (clock_bar_drawn < 0) {
        tft_fill_rect(bx, by, bw, bh, COL_RULE);
        clock_bar_drawn = 0;
    }
    if (w == clock_bar_drawn) return;
    if (w > clock_bar_drawn) {
        tft_fill_rect((uint16_t)(bx + clock_bar_drawn), by,
                      (uint16_t)(w - clock_bar_drawn), bh, COL_ACCENT);
    } else {
        tft_fill_rect((uint16_t)(bx + w), by,
                      (uint16_t)(clock_bar_drawn - w), bh, COL_RULE);
    }
    clock_bar_drawn = w;
}

void hw_ui_show_notify(const char *level,
                       const char *source,
                       const char *title,
                       const char *body,
                       int unread) {
    if (!panel_ok) return;
    screen = HW_UI_NOTIFY;

    uint16_t accent = COL_INFO;
    if (level && strcmp(level, "crit") == 0) accent = COL_CRIT;
    else if (level && strcmp(level, "warn") == 0) accent = COL_WARN;

    tft_fill(COL_BG);
    // Inverse header bar — Advisor vibe
    tft_fill_rect(0, 0, PANEL_W, 22, accent);
    char head[48];
    snprintf(head, sizeof(head), "%s", level && level[0] ? level : "info");
    for (char *p = head; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    tft_draw_text(MARGIN, 4, head, COL_BG, accent, 2);
    if (unread > 0) {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d UNREAD", unread);
        tft_draw_text_r(PANEL_W - MARGIN, 4, badge, COL_BG, accent, 2);
    }

    if (source && source[0]) {
        char src[40];
        snprintf(src, sizeof(src), "FROM %s", source);
        for (char *p = src; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        tft_draw_text(MARGIN, 36, src, COL_DIM, COL_BG, 2);
    }

    // 3px level accent bar on the left of the card body
    tft_fill_rect(0, 60, 3, 140, accent);

    tft_draw_text(MARGIN + 4, 64, title && title[0] ? title : "(no title)",
                  COL_TIME, COL_BG, 2);

    if (body && body[0]) {
        const int max_cols = 72;
        char line[80];
        const char *p = body;
        uint16_t y = 100;
        int rows = 0;
        while (*p && rows < 5 && y + 14 < PANEL_H - 20) {
            int n = 0;
            while (p[n] && p[n] != '\n' && n < max_cols) n++;
            if (p[n] && p[n] != '\n') {
                int cut = n;
                while (cut > 24 && p[cut] != ' ') cut--;
                if (p[cut] == ' ') n = cut;
            }
            memcpy(line, p, n); line[n] = '\0';
            tft_draw_text(MARGIN + 4, y, line, COL_FG, COL_BG, 1);
            p += n;
            if (*p == ' ' || *p == '\n') p++;
            y += 16; rows++;
        }
    }
}

void hw_ui_show_agent_invite(const char *agent_name,
                             const char *teaser,
                             int unread) {
    if (!panel_ok) return;
    screen = HW_UI_NOTIFY;  // same input paths as notify card

    const uint16_t accent = COL_ACCENT;
    tft_fill(COL_BG);

    // Full-width amber header
    tft_fill_rect(0, 0, PANEL_W, 28, accent);
    tft_draw_text(MARGIN, 6, "CHAT", COL_BG, accent, 2);
    if (unread > 0) {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d", unread > 9 ? 9 : unread);
        tft_draw_text_r(PANEL_W - MARGIN, 8, badge, COL_BG, accent, 2);
    }

    // Big agent name
    char name[20];
    snprintf(name, sizeof(name), "%s",
             agent_name && agent_name[0] ? agent_name : "AGENT");
    for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    tft_draw_text(MARGIN, 48, name, COL_ACCENT, COL_BG, 3);

    tft_draw_text(MARGIN, 84, "NEW REPLY", COL_DIM, COL_BG, 2);

    // Teaser box
    tft_fill_rect(MARGIN, 118, PANEL_W - 2 * MARGIN, 52, COL_RULE);
    tft_fill_rect(MARGIN + 2, 120, PANEL_W - 2 * MARGIN - 4, 48, COL_BG);
    if (teaser && teaser[0]) {
        // scale 2, ~36 cols
        const int max_cols = 34;
        char line[40];
        int n = 0;
        while (teaser[n] && n < max_cols) n++;
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        memcpy(line, teaser, n);
        line[n] = '\0';
        tft_draw_text(MARGIN + 10, 132, line, COL_TIME, COL_BG, 2);
    }

    tft_draw_text(MARGIN, 188, "click / Enter  =  open chat", COL_DIM, COL_BG, 1);
    tft_draw_text(MARGIN, 204, "scroll list from MENU > MESSAGES", COL_DIM, COL_BG, 1);
}

// Menu row geometry — fixed so selection can repaint one bar without a wipe.
static const int MENU_N = 6;
static const int MENU_ROW0_Y = 32;
static const int MENU_ROW_H = 28;
static const int MENU_BAR_H = 24;
static const char *const MENU_ITEMS[MENU_N] = {
    "MESSAGES", "AGENTS", "MESHCORE", "SETTINGS", "INFO", "BACK"
};

// MeshCore submenu
static const int MESH_N = 3;
static const int MESH_ROW0_Y = 48;
static const int MESH_ROW_H = 32;
static const int MESH_BAR_H = 28;
static const char *const MESH_ITEMS[MESH_N] = {
    "STATUS", "PING GATEWAY", "BACK"
};

// Layout picker (SETTINGS → LAYOUT)
static const int LAYOUT_N = 4;  // 3 layouts + BACK
static const int LAYOUT_ROW0_Y = 48;
static const int LAYOUT_ROW_H = 34;
static const int LAYOUT_BAR_H = 28;
static const char *const LAYOUT_ITEMS[LAYOUT_N] = {
    "ABC", "RU PHON", "RU", "BACK"
};

// Agents list (same geometry language as menu).
static const int AGENTS_LIST_N = 4;
static const int AGENTS_ROW0_Y = 48;
static const int AGENTS_ROW_H = 34;
static const int AGENTS_BAR_H = 28;
static const char *const AGENTS_LIST_ITEMS[AGENTS_LIST_N] = {
    "GROK", "CLAUDE", "HERMES", "BACK"
};

// Card action sheet (after click/Enter on a notification).
static const int CARD_ACT_N = 3;
static const int CARD_ACT_ROW0_Y = 72;
static const int CARD_ACT_ROW_H = 36;
static const int CARD_ACT_BAR_H = 28;
static const char *const CARD_ACT_ITEMS[CARD_ACT_N] = {
    "ACKNOWLEDGE", "REPLY", "BACK"
};

static void menu_draw_row(int i, bool on) {
    uint16_t y = (uint16_t)(MENU_ROW0_Y + i * MENU_ROW_H);
    uint16_t bar_y = y - 4;
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    if (on) {
        tft_fill_rect(MARGIN, bar_y, bar_w, MENU_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 12, y, MENU_ITEMS[i], COL_BG, COL_ACCENT, 2);
    } else {
        tft_fill_rect(MARGIN, bar_y, bar_w, MENU_BAR_H, COL_BG);
        tft_draw_text(MARGIN + 12, y, MENU_ITEMS[i], COL_TIME, COL_BG, 2);
    }
}

static void card_act_draw_row(int i, bool on) {
    uint16_t y = (uint16_t)(CARD_ACT_ROW0_Y + i * CARD_ACT_ROW_H);
    uint16_t bar_y = y - 4;
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    if (on) {
        tft_fill_rect(MARGIN, bar_y, bar_w, CARD_ACT_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 12, y, CARD_ACT_ITEMS[i], COL_BG, COL_ACCENT, 2);
    } else {
        tft_fill_rect(MARGIN, bar_y, bar_w, CARD_ACT_BAR_H, COL_BG);
        tft_draw_text(MARGIN + 12, y, CARD_ACT_ITEMS[i], COL_TIME, COL_BG, 2);
    }
}

void hw_ui_show_card_act(int selected, const char *title) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= CARD_ACT_N) selected = CARD_ACT_N - 1;

    if (screen == HW_UI_CARD_ACT && card_act_sel_drawn >= 0 &&
        card_act_sel_drawn != selected) {
        card_act_draw_row(card_act_sel_drawn, false);
        card_act_draw_row(selected, true);
        card_act_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_CARD_ACT && card_act_sel_drawn == selected) return;

    screen = HW_UI_CARD_ACT;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "MESSAGE", COL_BG, COL_ACCENT, 2);
    // Dim title of the card this action applies to
    tft_draw_text(MARGIN, 36,
                  title && title[0] ? title : "(msg)",
                  COL_DIM, COL_BG, 1);
    for (int i = 0; i < CARD_ACT_N; i++) card_act_draw_row(i, i == selected);
    card_act_sel_drawn = selected;
}

void hw_ui_show_menu(int selected) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= MENU_N) selected = MENU_N - 1;

    // Same face, only the bar moved → repaint two rows, no fillScreen.
    if (screen == HW_UI_MENU && menu_sel_drawn >= 0 && menu_sel_drawn != selected) {
        menu_draw_row(menu_sel_drawn, false);
        menu_draw_row(selected, true);
        menu_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_MENU && menu_sel_drawn == selected) return;

    screen = HW_UI_MENU;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "MENU", COL_BG, COL_ACCENT, 2);
    for (int i = 0; i < MENU_N; i++) menu_draw_row(i, i == selected);
    menu_sel_drawn = selected;
}

static void layout_draw_row(int i, bool on, int current_layout) {
    uint16_t y = (uint16_t)(LAYOUT_ROW0_Y + i * LAYOUT_ROW_H);
    uint16_t bar_y = y - 4;
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    char label[24];
    if (i < 3) {
        // Active layout marked like Nokia "* Item"
        snprintf(label, sizeof(label), "%s %s",
                 (i == current_layout) ? "*" : " ",
                 LAYOUT_ITEMS[i]);
    } else {
        snprintf(label, sizeof(label), "  %s", LAYOUT_ITEMS[i]);
    }
    if (on) {
        tft_fill_rect(MARGIN, bar_y, bar_w, LAYOUT_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 8, y, label, COL_BG, COL_ACCENT, 2);
    } else {
        tft_fill_rect(MARGIN, bar_y, bar_w, LAYOUT_BAR_H, COL_BG);
        uint16_t fg = (i < 3 && i == current_layout) ? COL_ACCENT : COL_TIME;
        tft_draw_text(MARGIN + 8, y, label, fg, COL_BG, 2);
    }
}

void hw_ui_show_layout(int selected, int current_layout) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= LAYOUT_N) selected = LAYOUT_N - 1;
    if (current_layout < 0) current_layout = 0;
    if (current_layout > 2) current_layout = 2;

    if (screen == HW_UI_LAYOUT && layout_sel_drawn >= 0 &&
        layout_sel_drawn != selected &&
        layout_cur_drawn == current_layout) {
        layout_draw_row(layout_sel_drawn, false, current_layout);
        layout_draw_row(selected, true, current_layout);
        layout_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_LAYOUT && layout_sel_drawn == selected &&
        layout_cur_drawn == current_layout) {
        return;
    }

    screen = HW_UI_LAYOUT;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "LAYOUT", COL_BG, COL_ACCENT, 2);
    tft_draw_text_r(PANEL_W - MARGIN, 6, "ALT+CAPS", COL_BG, COL_ACCENT, 1);
    for (int i = 0; i < LAYOUT_N; i++)
        layout_draw_row(i, i == selected, current_layout);
    layout_sel_drawn = selected;
    layout_cur_drawn = current_layout;
}

static void mesh_draw_row(int i, bool on) {
    uint16_t y = (uint16_t)(MESH_ROW0_Y + i * MESH_ROW_H);
    uint16_t bar_y = y - 4;
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    if (on) {
        tft_fill_rect(MARGIN, bar_y, bar_w, MESH_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 12, y, MESH_ITEMS[i], COL_BG, COL_ACCENT, 2);
    } else {
        tft_fill_rect(MARGIN, bar_y, bar_w, MESH_BAR_H, COL_BG);
        tft_draw_text(MARGIN + 12, y, MESH_ITEMS[i], COL_TIME, COL_BG, 2);
    }
}

void hw_ui_show_meshcore(int selected) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= MESH_N) selected = MESH_N - 1;

    if (screen == HW_UI_MESHCORE && mesh_sel_drawn >= 0 &&
        mesh_sel_drawn != selected) {
        mesh_draw_row(mesh_sel_drawn, false);
        mesh_draw_row(selected, true);
        mesh_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_MESHCORE && mesh_sel_drawn == selected) return;

    screen = HW_UI_MESHCORE;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "MESHCORE", COL_BG, COL_ACCENT, 2);
    tft_draw_text_r(PANEL_W - MARGIN, 6, "GW", COL_BG, COL_ACCENT, 1);
    tft_draw_text(MARGIN, 32, "home radio gateway", COL_DIM, COL_BG, 1);
    for (int i = 0; i < MESH_N; i++) mesh_draw_row(i, i == selected);
    mesh_sel_drawn = selected;
}

void hw_ui_show_mesh_ping(const char *phase,
                          const char *const *lines,
                          int n_lines) {
    if (!panel_ok) return;
    screen = HW_UI_MESH_PING;
    if (n_lines < 0) n_lines = 0;
    if (n_lines > HW_UI_MESH_PING_LINES) n_lines = HW_UI_MESH_PING_LINES;

    tft_fill(COL_BG);
    // Phase colour: OK=info teal, FAIL=crit, else amber
    uint16_t accent = COL_ACCENT;
    if (phase && (strcmp(phase, "OK") == 0 || strcmp(phase, "DONE") == 0))
        accent = COL_INFO;
    else if (phase && (strcmp(phase, "FAIL") == 0 || strcmp(phase, "ERR") == 0))
        accent = COL_CRIT;

    tft_fill_rect(0, 0, PANEL_W, 22, accent);
    tft_draw_text(MARGIN, 4, "MESH PING", COL_BG, accent, 2);
    tft_draw_text_r(PANEL_W - MARGIN, 4,
                    phase && phase[0] ? phase : "…",
                    COL_BG, accent, 2);

    uint16_t y = 32;
    for (int i = 0; i < n_lines; i++) {
        const char *ln = (lines && lines[i]) ? lines[i] : "";
        tft_draw_text(MARGIN, y, ln, COL_TIME, COL_BG, 1);
        y = (uint16_t)(y + 16);
        if (y > PANEL_H - 24) break;
    }
    tft_draw_text(MARGIN, PANEL_H - 16, "click = back", COL_DIM, COL_BG, 1);
}

static void agents_list_draw_row(int i, bool on) {
    uint16_t y = (uint16_t)(AGENTS_ROW0_Y + i * AGENTS_ROW_H);
    uint16_t bar_y = y - 4;
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    if (on) {
        tft_fill_rect(MARGIN, bar_y, bar_w, AGENTS_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 12, y, AGENTS_LIST_ITEMS[i], COL_BG, COL_ACCENT, 2);
    } else {
        tft_fill_rect(MARGIN, bar_y, bar_w, AGENTS_BAR_H, COL_BG);
        tft_draw_text(MARGIN + 12, y, AGENTS_LIST_ITEMS[i], COL_TIME, COL_BG, 2);
    }
}

void hw_ui_show_agents(int selected, bool bridge_ok) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= AGENTS_LIST_N) selected = AGENTS_LIST_N - 1;

    if (screen == HW_UI_AGENTS && agents_sel_drawn >= 0 &&
        agents_sel_drawn != selected) {
        agents_list_draw_row(agents_sel_drawn, false);
        agents_list_draw_row(selected, true);
        agents_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_AGENTS && agents_sel_drawn == selected) return;

    screen = HW_UI_AGENTS;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "AGENTS", COL_BG, COL_ACCENT, 2);
    tft_draw_text_r(PANEL_W - MARGIN, 6,
                    bridge_ok ? "bridge" : "local",
                    COL_BG, COL_ACCENT, 1);
    for (int i = 0; i < AGENTS_LIST_N; i++)
        agents_list_draw_row(i, i == selected);
    agents_sel_drawn = selected;
}

// Row line buffer: max_cols glyphs × 4 UTF-8 bytes + "> " + NUL.
// Old 48-byte stack buffer silently truncated Cyrillic mid-line and then
// advanced past the unpainted tail ("Готов" → "Гот" / drop "ов ").
#define AGENT_CHAT_LINE_MAX  168

// Advance one wrap step of *p; return *bytes* consumed (always == painted body).
// first=true → prepend "> " or "< ".
// max_cols is in *codepoints* (glyphs). line_n must hold max_cols*4 + 3.
static int agent_chat_next_row(const char **pp, bool mine, bool first,
                               int max_cols, char *line, size_t line_n) {
    if (!pp || !*pp || !line || line_n < 8) return 0;
    const char *p = *pp;

    const int off = first ? 2 : 0;
    int max_body = (int)line_n - 1 - off;  // bytes available for message text
    if (max_body < 4) max_body = 4;

    int budget = max_cols - (first ? 2 : 0);
    if (budget < 4) budget = 4;

    if (!*p) {
        if (first) {
            snprintf(line, line_n, "%s", mine ? "> " : "< ");
            return 0;
        }
        line[0] = '\0';
        return 0;
    }

    int chars = 0;
    int bytes = 0;
    int last_sp_bytes = -1;
    // Stop on glyph budget OR when the next UTF-8 char would not fit in line[].
    while (p[bytes] && chars < budget) {
        uint32_t cp = 0;
        int k = utf8_next(p + bytes, &cp);
        if (k <= 0) break;
        if (bytes + k > max_body) break;
        if (cp == (uint32_t)' ') last_sp_bytes = bytes;
        bytes += k;
        chars++;
    }
    // Prefer break at last space when more text remains (word wrap).
    if (p[bytes] && last_sp_bytes > 0) {
        bytes = last_sp_bytes + 1;  // consume the space
    }
    if (bytes <= 0) {
        // Force one codepoint so we never stall (fits: max_body >= 4).
        uint32_t cp = 0;
        int k = utf8_next(p, &cp);
        if (k <= 0) k = 1;
        if (k > max_body) k = max_body;
        bytes = k;
    }

    // Paint length: drop trailing space (still consumed in advance below).
    int take = bytes;
    while (take > 0 && p[take - 1] == ' ') take--;

    if (first) {
        line[0] = mine ? '>' : '<';
        line[1] = ' ';
    }
    memcpy(line + off, p, (size_t)take);
    line[off + take] = '\0';

    // Advance exactly what this row committed — never past unpainted body.
    *pp = p + bytes;
    while (**pp == ' ') (*pp)++;
    return bytes;
}

static int agent_chat_wrap_rows(const char *raw, bool mine, int max_cols) {
    if (!raw) raw = "";
    const char *p = raw;
    bool first = true;
    int rows = 0;
    char discard[AGENT_CHAT_LINE_MAX];
    if (!*raw) return 1;
    while (*p) {
        agent_chat_next_row(&p, mine, first, max_cols, discard, sizeof(discard));
        rows++;
        first = false;
        if (rows > 400) break;
    }
    return rows > 0 ? rows : 1;
}

int hw_ui_show_agent_chat(const char *agent_name,
                          const char *const *lines,
                          const bool *from_me,
                          int line_count,
                          int scroll_row,
                          int *total_rows_out,
                          const char *footer) {
    if (!panel_ok) return 0;

    const uint8_t sc = 2;
    const int max_cols = (int)((PANEL_W - 2 * MARGIN) / (6 * sc));
    const int row_h = 7 * sc + 4;
    const uint16_t y0 = 30;
    const uint16_t y_max = 178;
    const int vis = (int)((y_max - y0) / row_h);  // ~7 rows at scale 2

    if (line_count < 0) line_count = 0;

    int total = 0;
    for (int i = 0; i < line_count; i++) {
        bool mine = from_me && from_me[i];
        const char *raw = (lines && lines[i]) ? lines[i] : "";
        total += agent_chat_wrap_rows(raw, mine, max_cols);
    }
    if (total < 1) total = 1;
    if (total_rows_out) *total_rows_out = total;

    int max_scroll = total - vis;
    if (max_scroll < 0) max_scroll = 0;
    int scroll = scroll_row;
    if (scroll < 0 || scroll > max_scroll) scroll = max_scroll;  // pin bottom

    char head[24];
    snprintf(head, sizeof(head), "%s",
             agent_name && agent_name[0] ? agent_name : "AGENT");

    // Scroll-only path: do NOT wipe the whole panel (that was the flicker).
    // Keep chrome (header/footer) and only repaint the body rectangle.
    static char chat_head[24] = "";
    bool same_room = (screen == HW_UI_AGENT_CHAT &&
                      strncmp(chat_head, head, sizeof(chat_head)) == 0);
    screen = HW_UI_AGENT_CHAT;
    agents_sel_drawn = -1;

    if (!same_room) {
        tft_fill(COL_BG);
        tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
        tft_draw_text(MARGIN, 4, head, COL_BG, COL_ACCENT, 2);
        tft_draw_text(MARGIN, 186,
                      footer && footer[0] ? footer : "wheel=scroll  type=msg",
                      COL_DIM, COL_BG, 1);
        tft_draw_text(MARGIN, 204, "ALT+Bksp=HOME  click=CLEAR",
                      COL_DIM, COL_BG, 1);
        snprintf(chat_head, sizeof(chat_head), "%s", head);
    } else {
        // Body only — solid fill then redraw text (no full-screen flash).
        tft_fill_rect(0, y0, PANEL_W, (uint16_t)(y_max - y0), COL_BG);
    }

    // Scroll badge on amber header (always refresh — small region).
    {
        char sb[16];
        if (max_scroll <= 0) snprintf(sb, sizeof(sb), "CHAT");
        else {
            int pct = (int)((scroll * 100L) / (max_scroll ? max_scroll : 1));
            snprintf(sb, sizeof(sb), "%d%%", pct);
        }
        // Erase badge strip then draw (opaque so old digits do not ghost).
        tft_fill_rect(PANEL_W - 72, 0, 72, 22, COL_ACCENT);
        tft_draw_text_r(PANEL_W - MARGIN, 6, sb, COL_BG, COL_ACCENT, 1);
    }

    int row_i = 0;
    uint16_t y = y0;
    for (int i = 0; i < line_count; i++) {
        bool mine = from_me && from_me[i];
        const char *raw = (lines && lines[i]) ? lines[i] : "";
        const char *p = raw;
        bool first = true;
        if (!*raw) {
            if (row_i >= scroll && y + row_h <= y_max) {
                char line[8];
                snprintf(line, sizeof(line), "%s", mine ? "> " : "< ");
                tft_draw_text(MARGIN, y, line,
                              mine ? COL_ACCENT : COL_TIME, COL_BG, sc);
                y = (uint16_t)(y + row_h);
            }
            row_i++;
            continue;
        }
        while (*p) {
            char line[AGENT_CHAT_LINE_MAX];
            agent_chat_next_row(&p, mine, first, max_cols, line, sizeof(line));
            if (row_i >= scroll && y + row_h <= y_max) {
                tft_draw_text(MARGIN, y, line,
                              mine ? COL_ACCENT : COL_TIME, COL_BG, sc);
                y = (uint16_t)(y + row_h);
            }
            row_i++;
            first = false;
            if (y > y_max && row_i > scroll + vis + 2) break;
        }
    }

    // Chevrons in body corners (already wiped with body fill).
    if (scroll > 0)
        tft_draw_text(PANEL_W - MARGIN - 12, 28, "^", COL_ACCENT, COL_BG, 1);
    if (scroll < max_scroll)
        tft_draw_text(PANEL_W - MARGIN - 12, 170, "v", COL_ACCENT, COL_BG, 1);

    return scroll;
}

static const int AGENT_ACT_N = 2;
static const char *const AGENT_ACT_ITEMS[AGENT_ACT_N] = {
    "CLEAR CHAT", "BACK"
};

void hw_ui_show_agent_act(int selected, const char *agent_name) {
    if (!panel_ok) return;
    if (selected < 0) selected = 0;
    if (selected >= AGENT_ACT_N) selected = AGENT_ACT_N - 1;

    if (screen == HW_UI_AGENT_ACT && agent_act_sel_drawn >= 0 &&
        agent_act_sel_drawn != selected) {
        // reuse card_act row geometry
        uint16_t y0 = (uint16_t)(CARD_ACT_ROW0_Y + agent_act_sel_drawn * CARD_ACT_ROW_H);
        uint16_t y1 = (uint16_t)(CARD_ACT_ROW0_Y + selected * CARD_ACT_ROW_H);
        uint16_t bar_w = PANEL_W - 2 * MARGIN;
        tft_fill_rect(MARGIN, y0 - 4, bar_w, CARD_ACT_BAR_H, COL_BG);
        tft_draw_text(MARGIN + 12, y0, AGENT_ACT_ITEMS[agent_act_sel_drawn],
                      COL_TIME, COL_BG, 2);
        tft_fill_rect(MARGIN, y1 - 4, bar_w, CARD_ACT_BAR_H, COL_ACCENT);
        tft_draw_text(MARGIN + 12, y1, AGENT_ACT_ITEMS[selected],
                      COL_BG, COL_ACCENT, 2);
        agent_act_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_AGENT_ACT && agent_act_sel_drawn == selected) return;

    screen = HW_UI_AGENT_ACT;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "CHAT", COL_BG, COL_ACCENT, 2);
    tft_draw_text(MARGIN, 36,
                  agent_name && agent_name[0] ? agent_name : "AGENT",
                  COL_DIM, COL_BG, 1);
    for (int i = 0; i < AGENT_ACT_N; i++) {
        uint16_t y = (uint16_t)(CARD_ACT_ROW0_Y + i * CARD_ACT_ROW_H);
        bool on = (i == selected);
        uint16_t bar_w = PANEL_W - 2 * MARGIN;
        if (on) {
            tft_fill_rect(MARGIN, y - 4, bar_w, CARD_ACT_BAR_H, COL_ACCENT);
            tft_draw_text(MARGIN + 12, y, AGENT_ACT_ITEMS[i], COL_BG, COL_ACCENT, 2);
        } else {
            tft_fill_rect(MARGIN, y - 4, bar_w, CARD_ACT_BAR_H, COL_BG);
            tft_draw_text(MARGIN + 12, y, AGENT_ACT_ITEMS[i], COL_TIME, COL_BG, 2);
        }
    }
    agent_act_sel_drawn = selected;
}

// Msglist — Nokia 3310 / classic pager inbox language:
//   * = NEW (unread), dim title = already read
//   single-letter severity chip I/W/C with colour
//   header carries NEW count like "2 New messages"
static const int MSGLIST_VIS = 5;
static const int MSGLIST_ROW0_Y = 34;
static const int MSGLIST_ROW_H = 32;
static const int MSGLIST_BAR_H = 30;

static int msglist_top_for(int selected, int count) {
    int top = selected - MSGLIST_VIS / 2;
    if (top < 0) top = 0;
    if (top > count - MSGLIST_VIS && count > MSGLIST_VIS) top = count - MSGLIST_VIS;
    if (top < 0) top = 0;
    return top;
}

static char msglist_level_letter(const char *level) {
    if (!level || !level[0]) return 'I';
    if (level[0] == 'c' || level[0] == 'C') return 'C';
    if (level[0] == 'w' || level[0] == 'W') return 'W';
    return 'I';
}

static uint16_t msglist_level_color(const char *level) {
    if (!level || !level[0]) return COL_INFO;
    if (level[0] == 'c' || level[0] == 'C') return COL_CRIT;
    if (level[0] == 'w' || level[0] == 'W') return COL_WARN;
    return COL_INFO;
}

static uint8_t msglist_mask_of(const bool *unread, int count) {
    uint8_t m = 0;
    if (!unread) return 0;
    for (int i = 0; i < count && i < 8; i++)
        if (unread[i]) m |= (uint8_t)(1u << i);
    return m;
}

// Row:  [*| ][W]  Title…     — * only when NEW; title bright vs dim.
static void msglist_draw_row(const char *const *titles,
                             const char *const *levels,
                             const bool *unread,
                             int i, uint16_t y, bool on) {
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    bool is_new = unread && unread[i];
    const char *lvl = (levels && levels[i]) ? levels[i] : "info";
    uint16_t sev = msglist_level_color(lvl);
    char letter[2] = { msglist_level_letter(lvl), '\0' };

    uint16_t bg = on ? COL_ACCENT : COL_BG;
    // Unread title = warm white; read = slate (Nokia bold vs plain).
    uint16_t title_fg = on ? COL_BG : (is_new ? COL_TIME : COL_DIM);
    uint16_t mark_fg  = on ? COL_BG : (is_new ? COL_ACCENT : COL_RULE);
    uint16_t chip_fg  = on ? COL_BG : sev;

    tft_fill_rect(MARGIN, y - 2, bar_w, MSGLIST_BAR_H, bg);

    // Left severity tick — full height when NEW, hairline when read.
    if (!on) {
        tft_fill_rect(MARGIN, y - 2, is_new ? 4 : 2, MSGLIST_BAR_H, sev);
    }

    // Status glyph: * NEW  /  · read  (classic pager envelope substitute)
    tft_draw_text(MARGIN + 10, y + 4, is_new ? "*" : ".", mark_fg, bg, 2);

    // Severity letter chip
    tft_draw_text(MARGIN + 30, y + 4, letter, chip_fg, bg, 2);

    // Title — truncate so it never crashes the right edge (scale-2 ~12px/glyph)
    const char *raw = (titles && titles[i] && titles[i][0]) ? titles[i] : "(no title)";
    char t[36];
    // ~28 glyphs fit after mark+chip (x≈54 … 468)
    snprintf(t, sizeof(t), "%s", raw);
    if (strlen(t) > 28) {
        t[27] = '.';
        t[26] = '.';
        t[25] = '.';
        t[28] = '\0';
    }
    tft_draw_text(MARGIN + 52, y + 4, t, title_fg, bg, 2);
}

void hw_ui_show_msglist(const char *const *titles,
                        const char *const *levels,
                        const bool *unread,
                        int count,
                        int selected) {
    if (!panel_ok) return;

    if (count < 0) count = 0;
    if (count > HW_UI_MSGLIST_MAX) count = HW_UI_MSGLIST_MAX;
    if (selected < 0) selected = 0;
    if (count > 0 && selected >= count) selected = count - 1;

    int top = (count > 0) ? msglist_top_for(selected, count) : 0;
    uint8_t mask = msglist_mask_of(unread, count);

    // Same list + unread set + scroll window → only move the selection bar.
    if (screen == HW_UI_MSGLIST &&
        msglist_count_drawn == count &&
        msglist_top_drawn == top &&
        msglist_unread_mask == mask &&
        msglist_sel_drawn >= 0 &&
        msglist_sel_drawn != selected &&
        count > 0) {
        int old_row = msglist_sel_drawn - top;
        int new_row = selected - top;
        if (old_row >= 0 && old_row < MSGLIST_VIS)
            msglist_draw_row(titles, levels, unread, msglist_sel_drawn,
                             (uint16_t)(MSGLIST_ROW0_Y + old_row * MSGLIST_ROW_H), false);
        if (new_row >= 0 && new_row < MSGLIST_VIS)
            msglist_draw_row(titles, levels, unread, selected,
                             (uint16_t)(MSGLIST_ROW0_Y + new_row * MSGLIST_ROW_H), true);
        msglist_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_MSGLIST &&
        msglist_count_drawn == count &&
        msglist_top_drawn == top &&
        msglist_unread_mask == mask &&
        msglist_sel_drawn == selected) {
        return;  // nothing changed
    }

    screen = HW_UI_MSGLIST;
    tft_fill(COL_BG);

    // Header — amber bar, Nokia-style NEW tally on the right.
    int n_new = 0;
    if (unread) {
        for (int i = 0; i < count; i++) if (unread[i]) n_new++;
    }
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    char head[24];
    snprintf(head, sizeof(head), "INBOX  %02d", count);
    tft_draw_text(MARGIN, 4, head, COL_BG, COL_ACCENT, 2);
    if (n_new > 0) {
        char right[20];
        snprintf(right, sizeof(right), "*%02d NEW", n_new > 99 ? 99 : n_new);
        tft_draw_text_r(PANEL_W - MARGIN, 4, right, COL_BG, COL_ACCENT, 2);
    } else if (count > 0) {
        tft_draw_text_r(PANEL_W - MARGIN, 4, "ALL READ", COL_BG, COL_ACCENT, 1);
    }

    // Hairline under header (pager chrome)
    tft_hline(0, 22, PANEL_W, COL_RULE);

    if (count == 0) {
        tft_draw_text(MARGIN, 80, "NO MESSAGES", COL_DIM, COL_BG, 2);
        tft_draw_text(MARGIN, 120, "click = back", COL_DIM, COL_BG, 1);
        msglist_sel_drawn = -1;
        msglist_top_drawn = -1;
        msglist_count_drawn = 0;
        msglist_unread_mask = 0;
        return;
    }

    for (int row = 0; row < MSGLIST_VIS; row++) {
        int i = top + row;
        if (i >= count) break;
        msglist_draw_row(titles, levels, unread, i,
                         (uint16_t)(MSGLIST_ROW0_Y + row * MSGLIST_ROW_H),
                         i == selected);
        // Subtle separator between rows (Symbian-era lists had rules)
        if (row + 1 < MSGLIST_VIS && (i + 1) < count) {
            uint16_t ry = (uint16_t)(MSGLIST_ROW0_Y + row * MSGLIST_ROW_H + MSGLIST_BAR_H - 1);
            tft_hline(MARGIN + 8, ry, PANEL_W - 2 * MARGIN - 8, COL_RULE);
        }
    }

    // Footer legend — teaches the * glyph once, classic phone chrome.
    tft_hline(0, PANEL_H - 18, PANEL_W, COL_RULE);
    tft_draw_text(MARGIN, PANEL_H - 14, "* NEW   . read   I/W/C level",
                  COL_DIM, COL_BG, 1);
    tft_draw_text_r(PANEL_W - MARGIN, PANEL_H - 14, "click open",
                    COL_DIM, COL_BG, 1);

    msglist_sel_drawn = selected;
    msglist_top_drawn = top;
    msglist_count_drawn = count;
    msglist_unread_mask = mask;
}

void hw_ui_show_reply(const char *title,
                      const char *buffer,
                      bool caps,
                      bool symbol,
                      const char *layout_name) {
    if (!panel_ok) return;
    screen = HW_UI_REPLY;

    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "REPLY", COL_BG, COL_ACCENT, 2);
    // Badge: layout (ABC/PHON/RU) + CAPS + 123 while ALT held.
    char mods[28];
    const char *lay = (layout_name && layout_name[0]) ? layout_name : "ABC";
    if (symbol)
        snprintf(mods, sizeof(mods), "%s%s 123", caps ? "CAPS " : "", lay);
    else
        snprintf(mods, sizeof(mods), "%s%s", caps ? "CAPS " : "", lay);
    tft_draw_text_r(PANEL_W - MARGIN, 4, mods, COL_BG, COL_ACCENT, 1);

    // Original title, dim
    tft_draw_text(MARGIN, 32, title && title[0] ? title : "(msg)", COL_DIM, COL_BG, 1);

    // Draft box
    tft_fill_rect(MARGIN, 56, PANEL_W - 2 * MARGIN, 100, COL_RULE);
    tft_fill_rect(MARGIN + 2, 58, PANEL_W - 2 * MARGIN - 4, 96, COL_BG);

    const char *buf = buffer ? buffer : "";
    const uint8_t draft_scale = 2;
    const uint16_t draft_x = MARGIN + 8;
    const uint16_t box_inner_r = PANEL_W - MARGIN - 4;
    const uint16_t draft_w = (box_inner_r > draft_x + 4)
                                 ? (uint16_t)(box_inner_r - draft_x - 4) : 40;
    int max_cols = (int)(draft_w / (6 * draft_scale));
    if (max_cols < 8) max_cols = 8;
    if (max_cols > 60) max_cols = 60;

    char line[96];
    const char *p = buf;
    uint16_t y = 64;
    int rows = 0;
    const int max_rows = 5;
    while ((*p || rows == 0) && rows < max_rows && y + 14 < 150) {
        // Wrap by codepoints, not bytes (UTF-8 Cyrillic is 2 bytes/glyph).
        int chars = 0;
        int last_sp_chars = -1;
        int last_sp_bytes = -1;
        int bytes = 0;
        while (p[bytes] && p[bytes] != '\n' && chars < max_cols) {
            uint32_t cp = 0;
            int k = utf8_next(p + bytes, &cp);
            if (k <= 0) break;
            if (cp == (uint32_t)' ') {
                last_sp_chars = chars;
                last_sp_bytes = bytes;
            }
            bytes += k;
            chars++;
        }
        if (p[bytes] && p[bytes] != '\n' && last_sp_chars > 0) {
            // Break after last space
            bytes = last_sp_bytes + 1;  // include space in this line's advance
            // trim painted space below
        }
        if (bytes == 0 && !*p) {
            tft_draw_text(draft_x, y, "_", COL_ACCENT, COL_BG, draft_scale);
            break;
        }
        if (bytes == 0 && *p) {
            uint32_t cp;
            int k = utf8_next(p, &cp);
            bytes = k > 0 ? k : 1;
        }
        bool last = (p[bytes] == '\0');
        int take = bytes;
        // Soft-wrap mid-text: hide the break-space at EOL.
        // Last line (with the caret): KEEP trailing spaces so "_" advances.
        if (!last) {
            while (take > 0 && p[take - 1] == ' ') take--;
        }
        if (take >= (int)sizeof(line)) take = (int)sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = '\0';
        if (last && take + 1 < (int)sizeof(line)) {
            line[take] = '_';
            line[take + 1] = '\0';
        }
        tft_draw_text(draft_x, y, line, COL_TIME, COL_BG, draft_scale);
        p += bytes;
        if (*p == '\n') p++;
        y += (uint16_t)(7 * draft_scale + 4);
        rows++;
        if (last) break;
    }

    tft_draw_text(MARGIN, 170, "Enter=send  Bksp=del", COL_DIM, COL_BG, 1);
    tft_draw_text(MARGIN, 192, "ALT+CAPS=layout  ALT+Bksp=HOME", COL_DIM, COL_BG, 1);
}

void hw_ui_show_info(const char *version,
                     const char *host,
                     const char *ip,
                     const char *token,
                     uint32_t free_heap,
                     int unread) {
    if (!panel_ok) return;
    screen = HW_UI_INFO;

    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "INFO", COL_BG, COL_ACCENT, 2);

    char line[64];
    uint16_t y = 40;
    snprintf(line, sizeof(line), "FW    %s", version ? version : "?");
    tft_draw_text(MARGIN, y, line, COL_TIME, COL_BG, 2); y += 28;
    snprintf(line, sizeof(line), "HOST  %s", host ? host : "?");
    tft_draw_text(MARGIN, y, line, COL_TIME, COL_BG, 2); y += 28;
    snprintf(line, sizeof(line), "IP    %s", ip && ip[0] ? ip : "offline");
    tft_draw_text(MARGIN, y, line, COL_TIME, COL_BG, 2); y += 28;
    snprintf(line, sizeof(line), "HEAP  %u", (unsigned)free_heap);
    tft_draw_text(MARGIN, y, line, COL_DIM, COL_BG, 2); y += 28;
    snprintf(line, sizeof(line), "INBOX %d unread", unread);
    tft_draw_text(MARGIN, y, line, COL_DIM, COL_BG, 2); y += 28;
    tft_draw_text(MARGIN, y, "TOKEN", COL_DIM, COL_BG, 1); y += 16;
    tft_draw_text(MARGIN, y, token && token[0] ? token : "(none)", COL_TIME, COL_BG, 1);
}
