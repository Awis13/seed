#!/usr/bin/env python3
"""Static regressions: no blocking waits on the loop task or the AsyncTCP task.

Four sites, one rule each:
  - /wifi/config and /wifi/networks handlers only raise a flag; the loop task
    walks mode -> disconnect -> begin on millis() settle gaps.
  - The PING screen is a state machine advanced from loop(); the old
    skill_meshcore_poll()+delay(25) spin is gone and leaving the face cancels.
  - gps.cpp keeps no busy-wait at all (gps_take_fix had no callers and is
    removed; request+poll via gps_request_fix()/gps_tick() is the only path).
  - Every loop-task HTTPClient site bounds the connect phase to ~1 s, and the
    /wg/restart, /wg/start and /wg/stop handlers never touch the tunnel on the
    AsyncTCP task — they raise flags consumed by skill_wg_poll.
"""

from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
gps = (ROOT / "src" / "skills" / "gps.cpp").read_text(encoding="utf-8")
wg = (ROOT / "src" / "skills" / "wg.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")

# --- 1. WiFi handlers (AsyncTCP task) defer to the loop task -----------------
post = main[main.index("static void handle_wifi_post") :
            main.index("static void handle_wifi_status")]
assert "delay(" not in post, "/wifi/config must not block the AsyncTCP task"
assert "WiFi.mode(" not in post and "WiFi.disconnect(" not in post, (
    "the handler must not call into the WiFi driver at all"
)
assert "wifi_reconnect_request()" in post, "the handler must only raise the flag"

nets = main[main.index("static void handle_wifi_networks_post") :
            main.index("// ===== Skills =====")]
assert "delay(" not in nets
assert "WiFi.mode(" not in nets and "WiFi.disconnect(" not in nets
assert "wifi_reconnect_request()" in nets

machine = main[main.index("static void wifi_reconnect_poll()") :
               main.index("static void wifi_setup()")]
assert "delay(" not in machine, "the reconnect sequence must never wait in place"
assert "WIFI_RECONNECT_MODE_SETTLE_MS" in machine
assert "WIFI_RECONNECT_DISC_SETTLE_MS" in machine
assert "millis() - wifi_reconnect_step_ms" in machine
assert "wifi_begin_active_profile()" in machine, (
    "the deferred sequence must still stamp the retry timer via the one owner"
)
assert "wifi_user_off" in machine, (
    "a user WiFi-off toggle must cancel a pending deferred reconnect"
)
assert "#define WIFI_RECONNECT_MODE_SETTLE_MS 500UL" in main
assert "#define WIFI_RECONNECT_DISC_SETTLE_MS 100UL" in main

loop = main[main.index("void loop()") :]
assert "wifi_reconnect_poll();" in loop, "loop() must drive the deferred sequence"

# --- 2. PING screen: state machine, no spin, cancel on exit ------------------
ping = main[main.index("static void ui_mesh_ping_gateway()") :
            main.index("static void kb_layout_save()")]
assert "delay(" not in ping, "the PING sequence must not block the loop task"
assert "skill_meshcore_poll()" not in ping, (
    "the pong wait must lean on the meshcore skill tick, not a manual spin"
)
assert "MESH_PING_MESH_WAIT" in main and "MESH_PING_HTTP" in main
assert "#define MESH_PING_WAIT_MS 2800UL" in main, (
    "the pong window must stay the C1-era 2.8 s"
)
assert "millis() - mesh_ping_deadline_ms" in ping
poll = main[main.index("static void ui_mesh_ping_poll()") :
            main.index("static void kb_layout_save()")]
assert "hw_ui_screen() != HW_UI_MESH_PING" in poll, (
    "leaving the PING face mid-sequence must cancel the run"
)
assert "mesh_ping_state = MESH_PING_IDLE;" in poll
assert "ui_mesh_ping_poll();" in loop, "loop() must advance the PING sequence"

# s8 (C5): the HTTP step must hand off to the mesh TX state — pin the exact
# transition line inside the HTTP step slice so a refactor cannot silently
# park the machine after the WiFi column.
http_step = main[main.index("static void ui_mesh_ping_step_http()") :
                 main.index("static void ui_mesh_ping_step_mesh_tx()")]
assert "mesh_ping_state = MESH_PING_MESH_TX;" in http_step, (
    "the HTTP step must always advance to MESH_PING_MESH_TX"
)

# --- 3. gps.cpp: request+poll only, no busy-wait anywhere --------------------
assert "gps_take_fix" not in gps, (
    "the blocking compatibility helper must stay deleted (it had no callers)"
)
# s9 (C6): no ghost references to the deleted helper anywhere else either —
# agents.cpp used to name it in a comment.
assert "gps_take_fix" not in agents, (
    "comments must not reference the deleted gps_take_fix()"
)
assert "delay(" not in gps, "no GPS code may wait in place on any task"
assert "void gps_request_fix(void)" in gps
assert "gps_wake_requested" in gps[gps.index("static void gps_tick()") :]

# --- 4. Connect timeouts on every loop-task HTTPClient site ------------------
reply = main[main.index("static bool reply_upstream_http") :
             main.index("static bool reply_upstream_mesh")]
assert "http.setConnectTimeout(1000)" in reply, (
    "a black-holed gateway must cost ~1 s on the reply path"
)
assert "http.setConnectTimeout(1000)" in ping, (
    "a black-holed gateway must cost ~1 s on the PING path"
)
probe = wg[wg.index("static void skill_wg_poll()") :]
assert "http.setConnectTimeout(1000)" in probe, (
    "a dead tunnel must cost ~1 s on the liveness probe"
)

# --- 4b. /wg/restart: deferred, no delay on the AsyncTCP task ----------------
assert "delay(200)" not in wg, "the restart settle gap must live on millis()"
restart = wg[wg.index('exact("/wg/restart")') : wg.index("static void skill_wg_poll()")]
assert "delay(" not in restart
assert "wg_start_now()" not in restart and "wg_stop_now()" not in restart, (
    "the handler must only raise the request; the poll owns the tunnel"
)
assert "g_wg_restart_req = true;" in restart
assert "#define WG_RESTART_GAP_MS 200UL" in wg
assert "millis() - g_wg_restart_stop_ms < WG_RESTART_GAP_MS" in probe

# --- 4c. s7 (C5): /wg/start and /wg/stop defer exactly like /wg/restart ------
for ep, flag in (("/wg/start", "g_wg_start_req"),
                 ("/wg/stop", "g_wg_stop_req")):
    handler = wg[wg.index(f'exact("{ep}")') :]
    handler = handler[: handler.index("});")]
    assert "wg_start_now()" not in handler and "wg_stop_now()" not in handler, (
        f"{ep} must not run the tunnel inline on the AsyncTCP task"
    )
    assert "delay(" not in handler, f"{ep} must not block the AsyncTCP task"
    assert f"{flag} = true;" in handler, f"{ep} must only raise its flag"
    assert '"up"' in handler, (
        f"{ep} must report the current (pre-deferral) tunnel state"
    )
assert '"starting"' in wg and '"stopping"' in wg, (
    "the immediate responses must document that the action is deferred"
)
assert "if (g_wg_stop_req)" in probe and "if (g_wg_start_req)" in probe, (
    "skill_wg_poll must consume both deferral flags"
)
assert probe.index("if (g_wg_stop_req)") < probe.index("if (g_wg_start_req)"), (
    "stop must drain before start so a racing stop+start behaves like restart"
)
assert probe.index("if (g_wg_start_req)") < probe.index("WG_TICK_MS"), (
    "the deferrals must run ahead of the tick throttle, like the restart"
)

print("Task unblock policy tests: OK")
