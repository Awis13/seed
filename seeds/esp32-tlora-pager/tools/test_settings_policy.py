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

print("settings policy tests: OK")
