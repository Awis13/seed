// TCA8418 4x10 keyboard (I2C 0x34). LilyGo keymap + special keys.
// Power: EXP_KB_EN + EXP_KB_RST raised with the other XL9555 rails at boot.

#pragma once

#include <Arduino.h>

bool hw_kb_begin();
bool hw_kb_ok();

// Poll FIFO. On press of a character key: *out = char, return true.
// Specials: '\b' backspace, '\n' enter. Modifier toggles are silent (false).
// Only reports presses, not releases.
bool hw_kb_read(char *out);

// Current modifier state (for UI badge).
bool hw_kb_caps();
bool hw_kb_symbol();
