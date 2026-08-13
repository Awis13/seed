#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.cpp").read_text(encoding="utf-8")
kb = (root / "src/hw_kb.cpp").read_text(encoding="utf-8")
ui = (root / "src/hw_ui.cpp").read_text(encoding="utf-8")

assert '#define SETTINGS_PATH     "/settings.json"' in main
assert "write_spiffs_file_atomic(SETTINGS_PATH, SETTINGS_TMP, body);" in main
assert 'doc["silent"].is<bool>()' in main
assert "settings_silent = !settings_silent;" in main
assert "if (!settings_silent) hw_sound_notify(lvl);" in main
assert "hw_haptic_notify(lvl);" in main
assert "hw_kb_take_silent_toggle()" in main
assert "pressed && alt_held && row == 1 && col == 1" in kb
assert '"SILENT %s", silent ? "ON" : "OFF"' in ui
assert 'snprintf(row2, sizeof(row2), "SILENT");' in main
assert 'doc["autolock_s"] = settings_autolock_s;' in main
assert "settings_autolock_valid(value)" in main
assert "hw_input_long_press()" in main
assert "hw_kb_set_locked(!hw_kb_locked())" in main
assert "settings_autolock_due(settings_autolock_s" in main
assert '"AUTOLOCK %s", autolock_word' in ui

input_cpp = (root / "src/hw_input.cpp").read_text(encoding="utf-8")
assert "INPUT_LONG_MS          1000" in input_cpp
assert "!long_fired && (t - key_down_at) < INPUT_CLICK_MAX_MS" in input_cpp
assert "pending_click = false;" in input_cpp
assert "kb_toggle_lock" not in kb and "CAPS_LONG_MS" not in kb

print("settings policy tests: OK")
