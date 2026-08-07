// Minimal TCA8418 driver + LilyGo T-Lora Pager keymap.
// Event layout from LilyGoKeyboard.cpp (k--, row=k/10, col=k%10).
//
// Layer model (Pager has_symbol_key=false in LilyGoLib):
//   - Letters by default.
//   - Orange ALT (0x14): HOLD for symbols/digits (momentary). Factory text:
//     "Middle orange button + Key = Symbol Mode".
//   - CAPS (0x1C): toggle A/a.
//   - Key 0x1E is SPACE in the 4x10 map — must NOT latch symbol mode
//     (old seed treated 0x1E as SYM toggle → one space and only digits forever).

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

// After k-- (LilyGo keyboardConfig for T-Lora Pager)
#define KEY_ALT        0x14   // orange — hold = symbols
#define KEY_CHAR_B     0x19
#define KEY_CAPS       0x1C   // toggle caps
#define KEY_BACKSPACE  0x1D
#define KEY_SPACE      0x1E   // space (same code LilyGo lists as symbol_key_value)

static bool kb_ok = false;
static bool caps = false;
static bool symbol = false;   // true while ALT held
static bool alt_held = false;

// LilyGo 4x10 maps (LilyGo_LoRa_Pager.cpp)
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
    caps = symbol = alt_held = false;
    Serial.println("[kb] TCA8418 4x10 ok (ALT-hold=sym, CAPS=toggle)");
    return true;
}

bool hw_kb_ok() { return kb_ok; }
bool hw_kb_caps() { return caps; }
bool hw_kb_symbol() { return symbol; }

void hw_kb_reset_mods() {
    caps = false;
    symbol = false;
    alt_held = false;
}

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

    // Orange ALT: hold = symbol layer (not latch).
    if (k == KEY_ALT) {
        alt_held = pressed;
        symbol = pressed;
        return false;
    }
    // CAPS: toggle A/a
    if (k == KEY_CAPS) {
        if (pressed) caps = !caps;
        return false;
    }
    if (k == KEY_BACKSPACE) {
        if (pressed) {
            // Orange ALT + Backspace = HOME (like Shift+Bksp on a laptop).
            // Plain Backspace still deletes — hold ALT only for the shortcut.
            *out = alt_held ? '\x1b' : '\b';
            return true;
        }
        return false;
    }
    // Space is a real character (was wrongly used as SYM latch).
    if (k == KEY_SPACE) {
        if (pressed) {
            // While ALT held, space is still space (not a digit).
            *out = ' ';
            return true;
        }
        return false;
    }

    if (!pressed) return false;

    uint8_t row = k / 10;
    uint8_t col = k % 10;
    if (row >= 4 || col >= 10) return false;

    // Use live ALT state for layer (symbol follows alt_held).
    bool use_sym = alt_held;
    char c = use_sym ? SYMMAP[row][col] : KEYMAP[row][col];
    if (c == '\0') return false;
    if (!use_sym && caps && c >= 'a' && c <= 'z')
        c = (char)toupper((unsigned char)c);
    *out = c;
    return true;
}
