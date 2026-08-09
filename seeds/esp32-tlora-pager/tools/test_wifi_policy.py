#!/usr/bin/env python3
"""Static regressions for mesh-only boot and the explicit Wi-Fi switch."""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")

assert "WiFi.softAP" not in main, "firmware must never raise a provisioning AP"
assert "WIFI_AP" not in main, "firmware must remain STA-only"
assert not re.search(r"while\s*\(\s*WiFi\.status\(\)", main), (
    "boot/menu code must not block waiting for infrastructure Wi-Fi"
)

toggle = main[main.index("static void ui_wifi_toggle()") :]
toggle = toggle[: toggle.index("static void ui_wifi_do_scan()")]
assert "if (!wifi_user_off)" in toggle
assert "WiFi.mode(WIFI_OFF)" in toggle
assert "WiFi.disconnect(true, false)" in toggle

for marker, end_marker in (
    ("static void handle_wifi_scan", "static void handle_wifi_networks_post"),
    ("static void ui_wifi_do_scan", "static void ui_wifi_show_profiles"),
):
    scan = main[main.index(marker):main.index(end_marker)]
    assert "wifi_user_off = false" not in scan, "scan must preserve manual Wi-Fi OFF"
    assert "WiFi.mode(WIFI_OFF)" in scan, "scan must restore RF-off state"

confirm = main[main.index("// Auto-confirm after 60s") :]
confirm = confirm[: confirm.index("// WiFi reconnect")]
assert "WiFi.status()" not in confirm, "OTA confirmation must work mesh-only"

send = agents[agents.index("static bool agents_send") :]
send = send[: send.index("static void agents_on_inbound")]
assert 'strcmp(agent_id, "codex") == 0' in send
assert 'strcmp(agent_id, "opencode") == 0' in send
assert "mesh_owned || !wifi_ok" in send

print("Wi-Fi/mesh boot policy tests: OK")
