#!/usr/bin/env python3
"""Static regressions for the Wi-Fi retry backoff ladder (0.9.47).

The driver's own auto-reconnect stays disabled (its retries underneath the
scheduler stalled the UI); background reconnect is the firmware's own loop()
attempt. Since 0.9.47 that attempt follows a backoff ladder instead of a flat
20 minutes, so a rebooted router gets the pager back on the LAN in ~30 s while
a genuinely absent AP still converges to the old 20-minute cadence.
"""

from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

# 1. Ladder constants: quick first rungs, WIFI_RETRY_MS stays the last rung so
#    the documented 20-minute steady state keeps its meaning.
ladder = main[main.index("WIFI_RETRY_LADDER_MS[]") :]
ladder = ladder[: ladder.index("};")]
for rung in ("30000UL", "60000UL", "120000UL", "300000UL", "600000UL"):
    assert rung in ladder, f"ladder must keep the {rung} rung"
assert "WIFI_RETRY_MS" in ladder, "the last rung must be the WIFI_RETRY_MS cap"
assert "#define WIFI_RETRY_MS 1200000UL" in main, "20-minute steady-state cap"
assert "static int wifi_retry_step = 0;" in main, "ladder must start at rung 0"
assert "static unsigned long wifi_retry_interval_ms()" in main, (
    "the retry condition must read the interval through one accessor"
)

# 2. The background retry waits out the current rung (not the flat cap),
#    saturates at the last rung, and logs each attempt to the event ring.
retry = main[main.index("// WiFi reconnect") :]
retry = retry[: retry.index("if (now_connected != was_connected)")]
assert "millis() - wifi_last_attempt_ms >= wifi_retry_interval_ms()" in retry
assert "if (wifi_retry_step < WIFI_RETRY_LADDER_STEPS - 1) wifi_retry_step++;" in retry, (
    "the step must advance per attempt and saturate at the 20-minute rung"
)
assert 'event_add("wifi retry' in retry, (
    "each background attempt must be visible in the event log"
)
assert "wifi_begin_active_profile()" in retry, (
    "attempts must go through the single WiFi.begin owner"
)

# 3. Suppressors: manual OFF wins, and the deferred reconnect machine (raised
#    by HTTP handlers) must never race a background profile rotation.
assert "!wifi_user_off" in retry, "manual OFF must suppress background retries"
assert "wifi_reconnect_state == WIFI_RECONNECT_IDLE" in retry, (
    "no background retry while the reconnect machine is mid-sequence"
)

# 4. A successful association resets the ladder, so the next loss starts at
#    the quick 30 s rung again. Anchor to the connected branch specifically:
#    a reset that drifted to the offline branch would defeat the backoff.
conn = main[main.index("if (now_connected != was_connected)") :]
conn = conn[: conn.index("// Front panel")]
up = conn[conn.index("if (now_connected) {") :]
up = up[: up.index("} else")]
assert "wifi_retry_step = 0" in up, (
    "ladder must reset in the link-up branch of the transition"
)

# 4b. User-initiated attempts also start a fresh ladder: a saturated step must
#     not leave the user 20 minutes from the next rotation after their own
#     attempt fails on the wrong profile.
req = main[main.index("static void wifi_reconnect_request()") :]
req = req[: req.index("}")]
assert "wifi_retry_step = 0" in req, (
    "HTTP config apply / manual connect must reset the ladder"
)
toggle = main[main.index("static void ui_wifi_toggle()") :]
toggle = toggle[: toggle.index("static void ui_wifi_do_scan()")]
assert "wifi_retry_step = 0" in toggle, (
    "menu toggle ON must reset the ladder"
)
menu_conn = main[main.index("static void ui_wifi_connect_ssid(") :]
menu_conn = menu_conn[: menu_conn.index("/* Unknown network from scan")]
assert "wifi_retry_step = 0" in menu_conn, (
    "menu connect-to-SSID must reset the ladder"
)

# 5. Boot: wifi_setup()'s async begin goes through the owner and stamps
#    wifi_last_attempt_ms, so the first rung gives the boot association its
#    full 30 s before any retry can interrupt it — no parallel attempt.
setup = main[main.index("static void wifi_setup()") :]
setup = setup[: setup.index("// ===== HTTP Handlers")]
assert "wifi_begin_active_profile()" in setup, (
    "the boot attempt must stamp the retry timer like every other attempt"
)
owner = main[main.index("static void wifi_begin_active_profile()") :]
owner = owner[: owner.index("}")]
assert "wifi_last_attempt_ms = millis();" in owner

print("Wi-Fi retry ladder tests: OK")
