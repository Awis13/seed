// TCA8418 4x10 keyboard (I2C 0x34). LilyGo keymap + layout layers.
// Power: EXP_KB_EN + EXP_KB_RST raised with the other XL9555 rails at boot.
//
// Layouts (Settings + ALT+CAPS cycle):
//   EN      — Latin ABC (default)
//   RU PHON — Apple Russian-Phonetic (Mac: "Russian – QWERTY")
//   RU      — standard ЙЦУКЕН (Apple "Russian" / Russian – PC)

#pragma once

#include <Arduino.h>

// Layout ids — stable for SPIFFS /kb_layout.txt
enum HwKbLayout : uint8_t {
    HW_KB_LAYOUT_EN = 0,
    HW_KB_LAYOUT_RU_PHON = 1,
    HW_KB_LAYOUT_RU_JCUK = 2,
    HW_KB_LAYOUT_COUNT = 3,
};

bool hw_kb_begin();
bool hw_kb_ok();

// Poll FIFO. On a character event writes a UTF-8 sequence into out
// (1..4 bytes + NUL; out_sz must be >= 5). Specials are single-byte:
//   '\b' backspace, '\n' enter, '\x1b' HOME (ALT+Backspace).
// Modifier-only and layout-cycle events return false.
// Only reports presses, not releases.
bool hw_kb_read(char *out, size_t out_sz);

// Modifier / layout state for UI badges.
bool hw_kb_caps();
bool hw_kb_symbol();
HwKbLayout hw_kb_layout();
void hw_kb_set_layout(HwKbLayout layout);
void hw_kb_cycle_layout();                 // EN → PHON → RU → EN
const char *hw_kb_layout_name();           // "ABC" / "PHON" / "RU"
const char *hw_kb_layout_id();             // "en" / "ru-phon" / "ru-jcuk"
bool hw_kb_layout_from_id(const char *id, HwKbLayout *out);

// Clear CAPS/SYM before opening the reply composer (layout stays).
void hw_kb_reset_mods();

// True once after a layout change (cycle or set); UI clears via this read.
bool hw_kb_take_layout_changed();

// True once after a dedicated silent-toggle event. ALT+S is '/' on the
// symbol layer and must type that character — silent lives in Settings.
bool hw_kb_take_silent_toggle();

// Keyboard lock (pocket / bag). Held encoder click toggles it; CAPS unlocks.
// While locked all other keys are ignored.
bool hw_kb_locked();
void hw_kb_set_locked(bool on);
// True once after lock state flips (for haptic / note).
bool hw_kb_take_lock_changed();

// Keyboard backlight on GPIO46. That pad is an ESP32-S3 strap pin — never
// PWM it. park() must run first in setup() so a dirty reset cannot boot-loop.
void hw_kb_park_backlight();
void hw_kb_set_backlight(uint8_t level);
uint8_t hw_kb_get_backlight();
void hw_kb_toggle_backlight();
