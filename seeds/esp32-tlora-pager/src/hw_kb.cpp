// Minimal TCA8418 driver + LilyGo T-Lora Pager keymap + RU layouts.
// Event layout from LilyGoKeyboard.cpp (k--, row=k/10, col=k%10).
//
// Layer model (Pager has_symbol_key=false in LilyGoLib):
//   - Letters by default (layout-dependent: EN / RU PHON / RU JCUKEN).
//   - Orange ALT (0x14): HOLD for symbols/digits (momentary).
//   - CAPS (0x1C): toggle case; ALT+CAPS cycles layout.
//   - Long CAPS (~0.7s): lock/unlock keyboard (pocket).
//   - Key 0x1E is SPACE — not a SYM latch.

#include "hw_kb.h"
#include "board_pins.h"

#include <Wire.h>
#include <ctype.h>
#include <string.h>

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

#define KEY_ALT        0x14
#define KEY_CAPS       0x1C
#define KEY_BACKSPACE  0x1D
#define KEY_SPACE      0x1E

static bool kb_ok = false;
static bool caps = false;
static bool symbol = false;
static bool alt_held = false;
static HwKbLayout layout = HW_KB_LAYOUT_EN;
static bool layout_changed = false;
static bool kb_locked = false;
static bool lock_changed = false;
static uint32_t caps_down_ms = 0;
static bool caps_long_done = false;  // long-press already fired this hold
#define CAPS_LONG_MS 700u

// Latin letter map (QWERTY positions).
static const char KEYMAP_EN[4][10] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
    {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
    {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};

// SYM layer (ALT held) — digits + punctuation; RU extras on a few slots.
static const char SYMMAP[4][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
    {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
    {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
};

// UTF-8 Cyrillic code units as C strings (2 bytes + NUL).
// Apple Russian-Phonetic (com.apple.keylayout.Russian-Phonetic):
//   q→я w→ш e→е r→р t→т y→ы u→у i→и o→о p→п
//   a→а s→с d→д f→ф g→г h→ч j→й k→к l→л
//   z→з x→х c→ц v→в b→б n→н m→м
//   SYM: -→ь =→ъ '→э ;→ж $→ю _→щ !→ё   (pager has no [ ] \ `)
static const char *const PHON_LO[26] = {
    /*a*/ "\xD0\xB0", /*b*/ "\xD0\xB1", /*c*/ "\xD1\x86", /*d*/ "\xD0\xB4",
    /*e*/ "\xD0\xB5", /*f*/ "\xD1\x84", /*g*/ "\xD0\xB3", /*h*/ "\xD1\x87",
    /*i*/ "\xD0\xB8", /*j*/ "\xD0\xB9", /*k*/ "\xD0\xBA", /*l*/ "\xD0\xBB",
    /*m*/ "\xD0\xBC", /*n*/ "\xD0\xBD", /*o*/ "\xD0\xBE", /*p*/ "\xD0\xBF",
    /*q*/ "\xD1\x8F", /*r*/ "\xD1\x80", /*s*/ "\xD1\x81", /*t*/ "\xD1\x82",
    /*u*/ "\xD1\x83", /*v*/ "\xD0\xB2", /*w*/ "\xD1\x88", /*x*/ "\xD1\x85",
    /*y*/ "\xD1\x8B", /*z*/ "\xD0\xB7",
};
static const char *const PHON_UP[26] = {
    /*A*/ "\xD0\x90", /*B*/ "\xD0\x91", /*C*/ "\xD0\xA6", /*D*/ "\xD0\x94",
    /*E*/ "\xD0\x95", /*F*/ "\xD0\xA4", /*G*/ "\xD0\x93", /*H*/ "\xD0\xA7",
    /*I*/ "\xD0\x98", /*J*/ "\xD0\x99", /*K*/ "\xD0\x9A", /*L*/ "\xD0\x9B",
    /*M*/ "\xD0\x9C", /*N*/ "\xD0\x9D", /*O*/ "\xD0\x9E", /*P*/ "\xD0\x9F",
    /*Q*/ "\xD0\xAF", /*R*/ "\xD0\xA0", /*S*/ "\xD0\xA1", /*T*/ "\xD0\xA2",
    /*U*/ "\xD0\xA3", /*V*/ "\xD0\x92", /*W*/ "\xD0\xA8", /*X*/ "\xD0\xA5",
    /*Y*/ "\xD0\xAB", /*Z*/ "\xD0\x97",
};

// Apple Russian / Russian–PC ЙЦУКЕН on QWERTY positions:
//   q→й w→ц e→у r→к t→е y→н u→г i→ш o→щ p→з
//   a→ф s→ы d→в f→а g→п h→р j→о k→л l→д
//   z→я x→ч c→с v→м b→и n→т m→ь
//   SYM: ;→ж '→э ,→б .→ю $→х _→ъ !→ё
static const char *const JCUK_LO[26] = {
    /*a*/ "\xD1\x84", /*b*/ "\xD0\xB8", /*c*/ "\xD1\x81", /*d*/ "\xD0\xB2",
    /*e*/ "\xD1\x83", /*f*/ "\xD0\xB0", /*g*/ "\xD0\xBF", /*h*/ "\xD1\x80",
    /*i*/ "\xD1\x88", /*j*/ "\xD0\xBE", /*k*/ "\xD0\xBB", /*l*/ "\xD0\xB4",
    /*m*/ "\xD1\x8C", /*n*/ "\xD1\x82", /*o*/ "\xD1\x89", /*p*/ "\xD0\xB7",
    /*q*/ "\xD0\xB9", /*r*/ "\xD0\xBA", /*s*/ "\xD1\x8B", /*t*/ "\xD0\xB5",
    /*u*/ "\xD0\xB3", /*v*/ "\xD0\xBC", /*w*/ "\xD1\x86", /*x*/ "\xD1\x87",
    /*y*/ "\xD0\xBD", /*z*/ "\xD1\x8F",
};
static const char *const JCUK_UP[26] = {
    /*A*/ "\xD0\xA4", /*B*/ "\xD0\x98", /*C*/ "\xD0\xA1", /*D*/ "\xD0\x92",
    /*E*/ "\xD0\xA3", /*F*/ "\xD0\x90", /*G*/ "\xD0\x9F", /*H*/ "\xD0\xA0",
    /*I*/ "\xD0\xA8", /*J*/ "\xD0\x9E", /*K*/ "\xD0\x9B", /*L*/ "\xD0\x94",
    /*M*/ "\xD0\xAC", /*N*/ "\xD0\xA2", /*O*/ "\xD0\xA9", /*P*/ "\xD0\x97",
    /*Q*/ "\xD0\x99", /*R*/ "\xD0\x9A", /*S*/ "\xD0\xAB", /*T*/ "\xD0\x95",
    /*U*/ "\xD0\x93", /*V*/ "\xD0\x9C", /*W*/ "\xD0\xA6", /*X*/ "\xD0\xA7",
    /*Y*/ "\xD0\x9D", /*Z*/ "\xD0\xAF",
};

// RU-only SYM extras (when ALT held and layout is Russian).
// Returns UTF-8 string or NULL to fall through to Latin SYM char.
static const char *ru_sym_utf8(char sym_ch, bool upper) {
    // Shared helpers
    #define YO_LO "\xD1\x91"
    #define YO_UP "\xD0\x81"
    if (layout == HW_KB_LAYOUT_RU_PHON) {
        switch (sym_ch) {
        case '-': return upper ? "\xD0\xAC" : "\xD1\x8C"; // ь
        case '=': return upper ? "\xD0\xAA" : "\xD1\x8A"; // ъ
        case '\'': return upper ? "\xD0\xAD" : "\xD1\x8D"; // э
        case ';': return upper ? "\xD0\x96" : "\xD0\xB6"; // ж
        case '$': return upper ? "\xD0\xAE" : "\xD1\x8E"; // ю
        case '_': return upper ? "\xD0\xA9" : "\xD1\x89"; // щ
        case '!': return upper ? YO_UP : YO_LO;           // ё
        default: return NULL;
        }
    }
    if (layout == HW_KB_LAYOUT_RU_JCUK) {
        switch (sym_ch) {
        case ';': return upper ? "\xD0\x96" : "\xD0\xB6"; // ж
        case '\'': return upper ? "\xD0\xAD" : "\xD1\x8D"; // э
        case ',': return upper ? "\xD0\x91" : "\xD0\xB1"; // б
        case '.': return upper ? "\xD0\xAE" : "\xD1\x8E"; // ю
        case '$': return upper ? "\xD0\xA5" : "\xD1\x85"; // х
        case '_': return upper ? "\xD0\xAA" : "\xD1\x8A"; // ъ
        case '!': return upper ? YO_UP : YO_LO;           // ё
        case '-': return "-";
        case '=': return "=";
        default: return NULL;
        }
    }
    return NULL;
    #undef YO_LO
    #undef YO_UP
}

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

static bool emit_utf8(char *out, size_t out_sz, const char *u8) {
    if (!out || out_sz < 2 || !u8 || !u8[0]) return false;
    size_t n = strlen(u8);
    if (n + 1 > out_sz) return false;
    memcpy(out, u8, n + 1);
    return true;
}

static bool emit_byte(char *out, size_t out_sz, char c) {
    if (!out || out_sz < 2) return false;
    out[0] = c;
    out[1] = '\0';
    return true;
}

bool hw_kb_begin() {
    uint8_t v = 0;
    if (!tca_read(TCA_REG_CFG, &v)) {
        Serial.println("[kb] TCA8418 missing");
        kb_ok = false;
        return false;
    }

    tca_write(TCA_REG_KP_GPIO1, 0x0F);
    tca_write(TCA_REG_KP_GPIO2, 0xFF);
    tca_write(TCA_REG_KP_GPIO3, 0x03);
    tca_write(TCA_REG_DEBOUNCE_DIS1, 0x00);
    tca_write(TCA_REG_DEBOUNCE_DIS2, 0x00);
    tca_write(TCA_REG_DEBOUNCE_DIS3, 0x00);
    tca_write(TCA_REG_CFG, 0x01);
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
    pinMode(PIN_KB_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_KB_BACKLIGHT, LOW);

    kb_ok = true;
    caps = symbol = alt_held = false;
    layout_changed = false;
    Serial.printf("[kb] TCA8418 ok layout=%s (ALT=sym, CAPS=case, ALT+CAPS=layout)\n",
                  hw_kb_layout_name());
    return true;
}

bool hw_kb_ok() { return kb_ok; }
bool hw_kb_caps() { return caps; }
bool hw_kb_symbol() { return symbol; }
HwKbLayout hw_kb_layout() { return layout; }

void hw_kb_set_layout(HwKbLayout lay) {
    if (lay >= HW_KB_LAYOUT_COUNT) lay = HW_KB_LAYOUT_EN;
    if (layout == lay) return;
    layout = lay;
    layout_changed = true;
    Serial.printf("[kb] layout → %s\n", hw_kb_layout_name());
}

void hw_kb_cycle_layout() {
    hw_kb_set_layout((HwKbLayout)((layout + 1) % HW_KB_LAYOUT_COUNT));
}

const char *hw_kb_layout_name() {
    switch (layout) {
    case HW_KB_LAYOUT_RU_PHON: return "PHON";
    case HW_KB_LAYOUT_RU_JCUK: return "RU";
    default: return "ABC";
    }
}

const char *hw_kb_layout_id() {
    switch (layout) {
    case HW_KB_LAYOUT_RU_PHON: return "ru-phon";
    case HW_KB_LAYOUT_RU_JCUK: return "ru-jcuk";
    default: return "en";
    }
}

bool hw_kb_layout_from_id(const char *id, HwKbLayout *out) {
    if (!id || !out) return false;
    if (strcmp(id, "en") == 0 || strcmp(id, "abc") == 0) {
        *out = HW_KB_LAYOUT_EN; return true;
    }
    if (strcmp(id, "ru-phon") == 0 || strcmp(id, "phon") == 0 ||
        strcmp(id, "ru_phon") == 0) {
        *out = HW_KB_LAYOUT_RU_PHON; return true;
    }
    if (strcmp(id, "ru-jcuk") == 0 || strcmp(id, "ru") == 0 ||
        strcmp(id, "jcuk") == 0 || strcmp(id, "ru_jcuk") == 0) {
        *out = HW_KB_LAYOUT_RU_JCUK; return true;
    }
    return false;
}

bool hw_kb_take_layout_changed() {
    bool c = layout_changed;
    layout_changed = false;
    return c;
}

void hw_kb_reset_mods() {
    caps = false;
    symbol = false;
    alt_held = false;
}

bool hw_kb_locked() { return kb_locked; }

void hw_kb_set_locked(bool on) {
    if (kb_locked == on) return;
    kb_locked = on;
    lock_changed = true;
    if (on) {
        caps = false;
        symbol = false;
        alt_held = false;
    }
    Serial.printf("[kb] %s\n", on ? "LOCKED (long CAPS to unlock)" : "unlocked");
}

bool hw_kb_take_lock_changed() {
    bool c = lock_changed;
    lock_changed = false;
    return c;
}

static void kb_toggle_lock() {
    hw_kb_set_locked(!kb_locked);
}

bool hw_kb_read(char *out, size_t out_sz) {
    if (!kb_ok || !out || out_sz < 2) return false;
    out[0] = 0;

    uint8_t n = 0;
    if (!tca_read(TCA_REG_KEY_LCK_EC, &n)) return false;
    n &= 0x0F;
    if (n == 0) {
        tca_write(TCA_REG_INT_STAT, 0x01);
        /* While CAPS held, poll duration so long-press fires without release. */
        if (caps_down_ms != 0 && !caps_long_done &&
            (millis() - caps_down_ms) >= CAPS_LONG_MS) {
            caps_long_done = true;
            kb_toggle_lock();
        }
        return false;
    }

    uint8_t ev = 0;
    if (!tca_read(TCA_REG_KEY_EVENT_A, &ev) || ev == 0) return false;
    tca_write(TCA_REG_INT_STAT, 0x01);

    bool pressed = (ev & 0x80) != 0;
    uint8_t k = ev & 0x7F;
    if (k == 0 || k > 96) return false;
    k--;

    if (k == KEY_ALT) {
        if (kb_locked) return false;  // no mods while locked
        alt_held = pressed;
        symbol = pressed;
        return false;
    }
    if (k == KEY_CAPS) {
        if (pressed) {
            caps_down_ms = millis();
            caps_long_done = false;
        } else {
            /* Release: short tap = caps/layout; long already handled. */
            uint32_t held = (caps_down_ms != 0) ? (millis() - caps_down_ms) : 0;
            caps_down_ms = 0;
            if (caps_long_done) {
                caps_long_done = false;
                return false;  // long-press already toggled lock
            }
            if (kb_locked) return false;  // short CAPS ignored while locked
            if (alt_held) {
                hw_kb_cycle_layout();
            } else if (held < CAPS_LONG_MS) {
                caps = !caps;
            }
        }
        return false;
    }

    /* Locked: swallow everything else (still drain FIFO above). */
    if (kb_locked) return false;

    if (k == KEY_BACKSPACE) {
        if (pressed) return emit_byte(out, out_sz, alt_held ? '\x1b' : '\b');
        return false;
    }
    if (k == KEY_SPACE) {
        if (pressed) return emit_byte(out, out_sz, ' ');
        return false;
    }

    if (!pressed) return false;

    uint8_t row = k / 10;
    uint8_t col = k % 10;
    if (row >= 4 || col >= 10) return false;

    bool use_sym = alt_held;
    if (use_sym) {
        char sc = SYMMAP[row][col];
        if (sc == '\0') return false;
        if (sc == '\n') return emit_byte(out, out_sz, '\n');
        // RU extras on SYM layer
        if (layout != HW_KB_LAYOUT_EN) {
            const char *ru = ru_sym_utf8(sc, caps);
            if (ru) {
                if (ru[0] && (unsigned char)ru[0] < 0x80 && ru[1] == '\0')
                    return emit_byte(out, out_sz, ru[0]);
                return emit_utf8(out, out_sz, ru);
            }
        }
        return emit_byte(out, out_sz, sc);
    }

    char base = KEYMAP_EN[row][col];
    if (base == '\0') return false;
    if (base == '\n') return emit_byte(out, out_sz, '\n');
    if (base == ' ') return emit_byte(out, out_sz, ' ');

    if (base >= 'a' && base <= 'z') {
        int idx = base - 'a';
        if (layout == HW_KB_LAYOUT_RU_PHON) {
            return emit_utf8(out, out_sz, caps ? PHON_UP[idx] : PHON_LO[idx]);
        }
        if (layout == HW_KB_LAYOUT_RU_JCUK) {
            return emit_utf8(out, out_sz, caps ? JCUK_UP[idx] : JCUK_LO[idx]);
        }
        // EN
        char c = caps ? (char)toupper((unsigned char)base) : base;
        return emit_byte(out, out_sz, c);
    }

    return emit_byte(out, out_sz, base);
}
