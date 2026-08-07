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

// Draw one glyph by pushing a pre-scaled bitmap window (fast path).
static void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (c < 0x20 || c > 0x7E) c = '?';
    if (scale < 1) scale = 1;
    const uint8_t *g = &FONT5X7[(c - 0x20) * 5];
    const uint16_t gw = 6 * scale;   // 5 cols + 1 gap
    const uint16_t gh = 7 * scale;
    if (x >= PANEL_W || y >= PANEL_H) return;

    // Build line buffer for one scaled column-row scan in row-major order
    // Max scale we use is 8 → 48x56 = 2688 pixels → ~5.4KB. Stack-allocate
    // per row of scaled height instead.
    uint8_t rowbuf[6 * 8 * 2];  // max gw at scale 8
    if (gw * 2 > sizeof(rowbuf)) return;

    tft_window(x, y, x + gw - 1, y + gh - 1);
    digitalWrite(PIN_DISP_CS, LOW); digitalWrite(PIN_DISP_DC, HIGH);
    disp_spi->beginTransaction(SPISettings(DISP_SPI_HZ, MSBFIRST, SPI_MODE0));

    for (uint8_t row = 0; row < 7; row++) {
        // Build one logical font row, scaled horizontally into rowbuf
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&g[col]);
            uint16_t color = (bits & (1u << row)) ? fg : bg;
            uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
            for (uint8_t sx = 0; sx < scale; sx++) {
                size_t i = (col * scale + sx) * 2;
                rowbuf[i] = hi; rowbuf[i + 1] = lo;
            }
        }
        // gap column = bg
        uint8_t bhi = (uint8_t)(bg >> 8), blo = (uint8_t)bg;
        for (uint8_t sx = 0; sx < scale; sx++) {
            size_t i = (5 * scale + sx) * 2;
            rowbuf[i] = bhi; rowbuf[i + 1] = blo;
        }
        // repeat vertical scale
        for (uint8_t sy = 0; sy < scale; sy++) {
            disp_spi->writeBytes(rowbuf, gw * 2);
        }
    }
    disp_spi->endTransaction();
    digitalWrite(PIN_DISP_CS, HIGH);
}

static void tft_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!s) return;
    uint16_t adv = 6 * scale;
    while (*s) {
        if (x + adv > PANEL_W) break;
        tft_draw_char(x, y, *s++, fg, bg, scale);
        x += adv;
    }
}

// Text width helper
static uint16_t text_width(const char *s, uint8_t scale) {
    if (!s) return 0;
    return (uint16_t)(strlen(s) * 6 * scale);
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
static bool clock_dirty   = true;

// Selection caches for partial repaint (menu / msglist). -1 = need full paint.
static int menu_sel_drawn = -1;
static int msglist_sel_drawn = -1;
static int msglist_top_drawn = -1;
static int msglist_count_drawn = -1;
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
    clock_bar_drawn = -1;
}

void hw_ui_show_clock() {
    if (!panel_ok) return;
    screen = HW_UI_CLOCK;
    // Leaving interactive faces: next visit must full-paint.
    menu_sel_drawn = -1;
    msglist_sel_drawn = -1;
    msglist_top_drawn = -1;
    msglist_count_drawn = -1;
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
                      bool crit_unread) {
    if (!panel_ok || screen != HW_UI_CLOCK) return;
    (void)crit_unread;

    // Header: version | badge | BAT | IP  (same three-slot idea as tembed)
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

// Menu row geometry — fixed so selection can repaint one bar without a wipe.
static const int MENU_N = 3;
static const int MENU_ROW0_Y = 48;
static const int MENU_ROW_H = 36;
static const int MENU_BAR_H = 28;
static const char *const MENU_ITEMS[MENU_N] = {"MESSAGES", "INFO", "BACK"};

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

// Msglist: only re-stripe rows when selection moves inside the same window.
static const int MSGLIST_VIS = 5;
static const int MSGLIST_ROW0_Y = 36;
static const int MSGLIST_ROW_H = 32;
static const int MSGLIST_BAR_H = 28;

static int msglist_top_for(int selected, int count) {
    int top = selected - MSGLIST_VIS / 2;
    if (top < 0) top = 0;
    if (top > count - MSGLIST_VIS && count > MSGLIST_VIS) top = count - MSGLIST_VIS;
    if (top < 0) top = 0;
    return top;
}

static void msglist_draw_row(const char *const *titles,
                             const char *const *levels,
                             int i, uint16_t y, bool on) {
    uint16_t bar_w = PANEL_W - 2 * MARGIN;
    uint16_t bg = on ? COL_ACCENT : COL_BG;
    uint16_t fg = on ? COL_BG : COL_TIME;
    tft_fill_rect(MARGIN, y - 2, bar_w, MSGLIST_BAR_H, bg);

    char lvl[8];
    snprintf(lvl, sizeof(lvl), "%s",
             (levels && levels[i] && levels[i][0]) ? levels[i] : "info");
    for (char *p = lvl; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    tft_draw_text(MARGIN + 8, y + 4, lvl, on ? COL_BG : COL_DIM, bg, 1);

    const char *t = (titles && titles[i]) ? titles[i] : "?";
    tft_draw_text(MARGIN + 56, y + 2, t, fg, bg, 2);
}

void hw_ui_show_msglist(const char *const *titles,
                        const char *const *levels,
                        int count,
                        int selected) {
    if (!panel_ok) return;

    if (count < 0) count = 0;
    if (count > HW_UI_MSGLIST_MAX) count = HW_UI_MSGLIST_MAX;
    if (selected < 0) selected = 0;
    if (count > 0 && selected >= count) selected = count - 1;

    int top = (count > 0) ? msglist_top_for(selected, count) : 0;

    // Same list content + same scroll window → only move the bar between rows.
    if (screen == HW_UI_MSGLIST &&
        msglist_count_drawn == count &&
        msglist_top_drawn == top &&
        msglist_sel_drawn >= 0 &&
        msglist_sel_drawn != selected &&
        count > 0) {
        // Unselect old row if still visible
        int old_row = msglist_sel_drawn - top;
        int new_row = selected - top;
        if (old_row >= 0 && old_row < MSGLIST_VIS)
            msglist_draw_row(titles, levels, msglist_sel_drawn,
                             (uint16_t)(MSGLIST_ROW0_Y + old_row * MSGLIST_ROW_H), false);
        if (new_row >= 0 && new_row < MSGLIST_VIS)
            msglist_draw_row(titles, levels, selected,
                             (uint16_t)(MSGLIST_ROW0_Y + new_row * MSGLIST_ROW_H), true);
        msglist_sel_drawn = selected;
        return;
    }
    if (screen == HW_UI_MSGLIST &&
        msglist_count_drawn == count &&
        msglist_top_drawn == top &&
        msglist_sel_drawn == selected) {
        return;  // nothing changed
    }

    screen = HW_UI_MSGLIST;
    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    char head[24];
    snprintf(head, sizeof(head), "MSG  %02d", count);
    tft_draw_text(MARGIN, 4, head, COL_BG, COL_ACCENT, 2);
    tft_draw_text_r(PANEL_W - MARGIN, 4, "CLICK OPEN", COL_BG, COL_ACCENT, 1);

    if (count == 0) {
        tft_draw_text(MARGIN, 80, "INBOX EMPTY", COL_DIM, COL_BG, 2);
        tft_draw_text(MARGIN, 120, "click = back", COL_DIM, COL_BG, 1);
        msglist_sel_drawn = -1;
        msglist_top_drawn = -1;
        msglist_count_drawn = 0;
        return;
    }

    for (int row = 0; row < MSGLIST_VIS; row++) {
        int i = top + row;
        if (i >= count) break;
        msglist_draw_row(titles, levels, i,
                         (uint16_t)(MSGLIST_ROW0_Y + row * MSGLIST_ROW_H),
                         i == selected);
    }
    msglist_sel_drawn = selected;
    msglist_top_drawn = top;
    msglist_count_drawn = count;
}

void hw_ui_show_reply(const char *title,
                      const char *buffer,
                      bool caps,
                      bool symbol) {
    if (!panel_ok) return;
    screen = HW_UI_REPLY;

    tft_fill(COL_BG);
    tft_fill_rect(0, 0, PANEL_W, 22, COL_ACCENT);
    tft_draw_text(MARGIN, 4, "REPLY", COL_BG, COL_ACCENT, 2);
    char mods[16];
    snprintf(mods, sizeof(mods), "%s%s",
             caps ? "CAPS " : "",
             symbol ? "SYM" : "");
    if (mods[0]) tft_draw_text_r(PANEL_W - MARGIN, 4, mods, COL_BG, COL_ACCENT, 1);

    // Original title, dim
    tft_draw_text(MARGIN, 32, title && title[0] ? title : "(msg)", COL_DIM, COL_BG, 1);

    // Draft box
    tft_fill_rect(MARGIN, 56, PANEL_W - 2 * MARGIN, 100, COL_RULE);
    tft_fill_rect(MARGIN + 2, 58, PANEL_W - 2 * MARGIN - 4, 96, COL_BG);

    const char *buf = buffer ? buffer : "";
    // Wrap draft at ~70 cols scale 2 would be wide — use scale 1
    const int max_cols = 70;
    char line[80];
    const char *p = buf;
    uint16_t y = 64;
    int rows = 0;
    while ((*p || rows == 0) && rows < 5 && y + 14 < 150) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_cols) n++;
        if (n == 0 && !*p) {
            // empty draft: show cursor only
            tft_draw_text(MARGIN + 8, y, "_", COL_ACCENT, COL_BG, 2);
            break;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        // append cursor on last fragment
        bool last = (p[n] == '\0');
        if (last && n < (int)sizeof(line) - 2) {
            line[n] = '_';
            line[n + 1] = '\0';
        }
        tft_draw_text(MARGIN + 8, y, line, COL_TIME, COL_BG, 2);
        p += n;
        if (*p == '\n') p++;
        y += 18;
        rows++;
        if (last) break;
    }

    tft_draw_text(MARGIN, 170, "Enter=send  Bksp=del  Esc=cancel", COL_DIM, COL_BG, 1);
    tft_draw_text(MARGIN, 192, "Type on the keyboard", COL_DIM, COL_BG, 1);
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
