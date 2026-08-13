#!/usr/bin/env python3
"""Static regressions for mesh-only boot and the explicit Wi-Fi switch."""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")

assert "WiFi.softAP" not in main, "firmware must never raise a provisioning AP"
assert "WIFI_AP" not in main, "firmware must remain STA-only"
# mDNS stays off. An inbound multicast query reached the component's receive()
# on the tcpip thread, which logged, which allocated the console lock, which
# failed for want of internal DRAM, which aborted -- a boot loop. Removing the
# start is what takes that receive path off the network stack; see the comment
# on the link-up transition in main.cpp. Re-check free internal RAM before
# reinstating any of this.
assert "ESPmDNS.h" not in main, "the mDNS component must not be pulled back in"
assert "MDNS.begin" not in main, "mDNS must never be started on this board"
assert "MDNS.addService" not in main, "no mDNS service registration"
assert not re.search(r"while\s*\(\s*WiFi\.status\(\)", main), (
    "boot/menu code must not block waiting for infrastructure Wi-Fi"
)
# WIFI_RETRY_MS is the steady-state cap of the retry ladder since 0.9.47
# (the ladder itself is pinned by test_wifi_ladder.py).
assert "#define WIFI_RETRY_MS 1200000UL" in main
assert "WiFi.setAutoReconnect(false)" in main, (
    "the Arduino core must not retry underneath our own backoff ladder"
)
# C8: the active credentials are fixed char buffers, not Strings. WiFi.begin()
# reads them on the loop task while HTTP handlers on the AsyncTCP task could
# reassign a String's heap buffer under it — the arrays close that window.
assert "static char wifi_ssid[33]" in main, "wifi_ssid must be a fixed char buffer"
assert "static String wifi_ssid" not in main, "wifi_ssid must not regress to String"
assert "static char wifi_pass[65]" in main, "wifi_pass must be a fixed char buffer"
assert "static String wifi_pass" not in main, "wifi_pass must not regress to String"
assert main.count("WiFi.begin(") == 1, (
    "all connect paths must use wifi_begin_active_profile() and stamp the timer"
)
retry = main[main.index("// WiFi reconnect") :]
retry = retry[: retry.index("if (now_connected != was_connected)")]
assert "millis() - wifi_last_attempt_ms >= wifi_retry_interval_ms()" in retry, (
    "background retry must wait out the current ladder rung"
)
assert "!wifi_user_off" in retry, "manual OFF must suppress background retries"
assert "wifi_reconnect_state == WIFI_RECONNECT_IDLE" in retry, (
    "background retry must not rotate profiles while the deferred reconnect "
    "machine is mid-sequence"
)
assert "wifi_begin_active_profile()" in retry

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

# The bridge-first / mesh-fallback ladder moved into the CONV_AGENT transport
# backend when the chat path started dispatching by the conversation's
# transport. C2 (HERMES-LADDER) then turned the WiFi rung's LINK-UP gate into a
# PROVEN-REACHABILITY gate: the bridge POST is taken only when the pure picker
# selects the WiFi rung (gateway proven REACH_UP), with the mesh uplink below it.
send = agents[agents.index("static bool transport_send_agent(") :]
send = send[: send.index("\n/* CONV_LXMF:")]
# grok/opencode/codex retired: only claude/hermes remain and neither is
# mesh-owned. The WiFi rung is now chosen by reachability, not the raw link
# state, so a captive/no-route WiFi no longer stalls the send before it falls
# back. Matched on the bare ids so the pin still bites after any rename.
assert '"codex"' not in send
assert '"opencode"' not in send
assert "mesh_owned" not in send
assert "agent_pick_transport(" in send, (
    "the WiFi rung must be chosen by the pure reachability picker"
)
assert "agents_bridge_post(conv_id, session, text)" in send, (
    "the WiFi rung is still a bridge POST"
)
assert "g_agents_mesh_uplink(conv_id, text)" in send, (
    "the mesh uplink remains the fallback rung"
)

print("Wi-Fi/mesh boot policy tests: OK")
