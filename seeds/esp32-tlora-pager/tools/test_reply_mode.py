#!/usr/bin/env python3
"""Regression pins for the explicit REPLY routing mode."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
hw = (ROOT / "src/hw_ui.cpp").read_text(encoding="utf-8")

assert "enum ReplyMode : uint8_t" in main
assert "static ReplyMode reply_mode = REPLY_MODE_NONE;" in main
assert "static bool wifi_compose" not in main
assert "static bool agent_compose" not in main

paint = main[main.index("static void ui_reply_paint()"):
             main.index("// Keyboard shortcut HOME")]
for label in ("NOTIFY REPLY", "AGENT REPLY", "WIFI SSID", "WIFI PASSWORD"):
    assert label in paint
assert "hw_ui_show_reply(mode," in paint
assert 'mode && mode[0] ? mode : "COMPOSE"' in hw

submit_start = main.index("static void ui_reply_submit() {")
submit = main[submit_start:
              main.index("static void ui_agent_chat_refresh() {", submit_start)]
assert "switch (reply_mode)" in submit
assert submit.index("case REPLY_MODE_WIFI_SSID:") < submit.index("case REPLY_MODE_WIFI_PASSWORD:")
assert submit.index("case REPLY_MODE_WIFI_PASSWORD:") < submit.index("wifi_nets_upsert(")
assert submit.index("case REPLY_MODE_AGENT:") < submit.index("agents_send(")
assert submit.index("case REPLY_MODE_NOTIFY:") < submit.index("notify_set_reply(")
assert "default:\n        return;" in submit

print("reply mode tests: OK")
