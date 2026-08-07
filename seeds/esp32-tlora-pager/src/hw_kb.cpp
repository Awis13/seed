// Minimal TCA8418 driver + LilyGo T-Lora Pager keymap.
// Event layout from LilyGoKeyboard.cpp (k--, row=k/10, col=k%10).

#include "hw_kb.h"
#include "board_pins.h"

#include <Wire.h>
#include <ctype.h>

#define TCA_ADDR            0x34
#define TCA_REG_CFG         0x01
#define TCA_REG_INT_STAT    0x02
#define TCA_REG_KEY_LCK_EC  0x03
#define TCA_REG_KEY_EVENT_A 0x04
#define TCA_REG_KP_GPIO1    0x1D
#define TCA_REG_KP_GPIO2    0x1E
#define TCA_REG_KP_GPIO3    0x1F
#define TCA_REG_DEBOUNCE_DIS1 0x29
#define TCA_REG_DEBOUNCE_DIS2 0x2A
#define TCA_REG_DEBOUNCE_DIS3 0x2B

// After k-- (LilyGo specials)
#define KEY_ALT        0x14
#define KEY_CHAR_B     0x19
#define KEY_CAPS       0x1C
#define KEY_BACKSPACE  0x1D
#define KEY_SYMBOL     0x1E

static bool kb_ok = false;
static bool caps = false;
static bool symbol = false;
static bool alt = false;

// LilyGo 4x10 maps
static const char KEYMAP[4][10] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
    {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
    {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};
static const char SYMMAP[4][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
    {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
    {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};

static bool tca_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool tca_read(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TCA_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

bool hw_kb_begin() {
    // Soft presence
    uint8_t v = 0;
    if (!tca_read(TCA_REG_CFG, &v)) {
        Serial.println("[kb] TCA8418 missing");
        kb_ok = false;
        return false;
    }

    // 4 rows (KP_GPIO1 low nibble), 10 cols (KP_GPIO2 all + KP_GPIO3 low 2)
    tca_write(TCA_REG_KP_GPIO1, 0x0F);  // rows 0-3
    tca_write(TCA_REG_KP_GPIO2, 0xFF);  // cols 0-7
    tca_write(TCA_REG_KP_GPIO3, 0x03);  // cols 8-9

    // Debounce on for matrix pins
    tca_write(TCA_REG_DEBOUNCE_DIS1, 0x00);
    tca_write(TCA_REG_DEBOUNCE_DIS2, 0x00);
    tca_write(TCA_REG_DEBOUNCE_DIS3, 0x00);

    // CFG: AI (auto-increment) + KE_IEN (key event interrupt enable)
    // INT active low open-drain typical: bit settings 0x01 KE_IEN
    tca_write(TCA_REG_CFG, 0x01);

    // Clear any stale IRQ / events
    tca_write(TCA_REG_INT_STAT, 0x0F);
    while (true) {
        uint8_t n = 0;
        if (!tca_read(TCA_REG_KEY_LCK_EC, &n)) break;
        n &= 0x0F;
        if (!n) break;
        uint8_t ev = 0;
        tca_read(TCA_REG_KEY_EVENT_A, &ev);
    }

    pinMode(PIN_KB_INT, INPUT_PULLUP);
    // Keyboard backlight off for now (power); user can type without it.
    pinMode(PIN_KB_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_KB_BACKLIGHT, LOW);

    kb_ok = true;
    caps = symbol = alt = false;
    Serial.println("[kb] TCA8418 4x10 ok");
    return true;
}

bool hw_kb_ok() { return kb_ok; }
bool hw_kb_caps() { return caps; }
bool hw_kb_symbol() { return symbol; }

bool hw_kb_read(char *out) {
    if (!kb_ok || !out) return false;
    *out = 0;

    uint8_t n = 0;
    if (!tca_read(TCA_REG_KEY_LCK_EC, &n)) return false;
    n &= 0x0F;
    if (n == 0) {
        // Clear INT_STAT key bit if stuck
        tca_write(TCA_REG_INT_STAT, 0x01);
        return false;
    }

    uint8_t ev = 0;
    if (!tca_read(TCA_REG_KEY_EVENT_A, &ev) || ev == 0) return false;

    // Clear key interrupt flag
    tca_write(TCA_REG_INT_STAT, 0x01);

    bool pressed = (ev & 0x80) != 0;
    uint8_t k = ev & 0x7F;
    if (k == 0 || k > 96) return false;
    k--;  // 0-based, LilyGo

    // Modifiers / specials (index after k--)
    if (k == KEY_SYMBOL) {
        if (pressed) symbol = !symbol;
        return false;
    }
    if (k == KEY_CAPS) {
        if (pressed) caps = !caps;
        return false;
    }
    if (k == KEY_ALT) {
        if (pressed) alt = !alt;
        return false;
    }
    if (k == KEY_BACKSPACE) {
        if (pressed) { *out = '\b'; return true; }
        return false;
    }

    if (!pressed) return false;

    uint8_t row = k / 10;
    uint8_t col = k % 10;
    if (row >= 4 || col >= 10) return false;

    char c = symbol ? SYMMAP[row][col] : KEYMAP[row][col];
    if (c == '\0') return false;
    if (!symbol && caps && c >= 'a' && c <= 'z') c = (char)toupper((unsigned char)c);
    *out = c;
    return true;
}
