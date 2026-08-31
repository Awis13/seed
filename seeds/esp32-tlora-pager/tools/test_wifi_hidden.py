#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.cpp").read_text(encoding="utf-8")
ui = (root / "src/hw_ui.cpp").read_text(encoding="utf-8")

assert '"HIDDEN SSID"' in ui and "WIFI_MENU_N = 6" in ui
assert "WIFI_ACT_HIDDEN = 3" in main
assert "ui_wifi_open_hidden_ssid();" in main
assert "REPLY_MODE_WIFI_SSID" in main
assert "REPLY_MODE_WIFI_PASSWORD" in main

submit = main[main.index("static void ui_reply_submit() {"):
              main.index("static void ui_agent_chat_refresh() {")]
assert "bytes > 32" in submit
assert "utf8_text_is_printable(reply_buf, &content)" in submit
assert "ui_wifi_open_password(ssid);" in submit
assert submit.index("wifi_nets_upsert(ssid, reply_buf);") < submit.index("wifi_persist_profiles();")

cancel = main[main.index("case HW_UI_REPLY:"):
              main.index("    }\n}", main.index("case HW_UI_REPLY:"))]
assert "REPLY_MODE_WIFI_SSID" in cancel and "REPLY_MODE_WIFI_PASSWORD" in cancel
assert "wifi_pending_ssid[0] = '\\0';" in cancel

print("hidden Wi-Fi tests: OK")
