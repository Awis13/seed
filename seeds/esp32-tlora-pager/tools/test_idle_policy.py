#!/usr/bin/env python3
"""Static regressions for the screen idle policy: only user input and one-shot
system wakes may restart the idle/dim countdown. Repaints and synthetic error
lines must never keep the panel alive (the screen-never-sleeps bug)."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
mesh = (ROOT / "src" / "skills" / "meshcore.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")

# --- ui_note_wake: separate, documented entry point for SYSTEM events --------
assert "static void ui_note_wake()" in main, (
    "system events need their own auditable wake entry point"
)
wake_decl = main[: main.index("static void ui_note_wake()")]
wake_decl = wake_decl[wake_decl.rindex("ui_note_input() { ui_last_input_ms") :]
assert "repaints and synthetic errors must not call this" in wake_decl, (
    "ui_note_wake must carry its contract comment (wake once per event)"
)

# --- the WHOLE notify_take_arrival consumption block -------------------------
# Covers the chat branch, the severity-card branch, the compose branch and the
# on_clock tail: exactly two wakes (chat open-room + severity card), no
# user-input stamps, and nothing stamped after the branches either.
arrival = main[main.index("uint32_t arrived_id = 0;") :]
arrival = arrival[: arrival.index("hw_sound_poll();")]
assert arrival.count("ui_note_wake();") == 2, (
    "the arrival block must wake exactly twice: chat open-room + severity card"
)
assert "ui_note_input" not in arrival, (
    "an arriving card is a system event, not user input"
)
door = arrival[arrival.index("notify_is_chat(v)") :]
door = door[: door.index("// Real message from any service")]
assert "ui_note_wake();" in door, (
    "a chat message landing in the open room must wake the panel"
)
sev = arrival[arrival.index("// Real message from any service") :]
sev = sev[: sev.index("} else {")]
assert "ui_note_wake();" in sev, "an arriving severity card must light the screen"

# --- an arriving chat lands in its thread, never as an invite/severity card ---
# The loop() arrival chat branch mirrors the mesh/LXMF model: it delivers the
# text with agents_on_inbound() and badges; it must NOT pop the agent invite or
# a severity card, and must not stamp user input.
chat_arr = arrival[arrival.index("notify_is_chat(v)") :]
chat_arr = chat_arr[: chat_arr.index("// Real message from any service")]
assert "agents_on_inbound(agents_id(ax), v.body, true)" in chat_arr, (
    "an arriving chat must land in its conversation thread like a mesh/LXMF peer"
)
assert (
    "hw_ui_show_agent_invite" not in chat_arr
    and "hw_ui_show_notify" not in chat_arr
), "an arriving chat must not pop an invite or a severity card"
assert "ui_note_input" not in chat_arr, (
    "an arriving chat is a system event, not user input"
)

# --- a chat repaint is not activity ------------------------------------------
refresh = main[main.index("static void ui_agent_chat_refresh() {") :]
refresh = refresh[: refresh.index("static void ui_open_agent_chat")]
assert "ui_note_input" not in refresh, (
    "repainting the agent chat must not reset the idle clock"
)
assert "ui_note_wake" not in refresh, (
    "repainting the agent chat must not wake the panel either"
)

# --- the open-room repaint block: wake ONLY on a genuine arrival -------------
repaint = main[main.index("// Agent thread inbound (display_force)") :]
repaint = repaint[: repaint.index("uint32_t arrived_id = 0;")]
assert "bool real_in = g_agents_real_inbound;" in repaint
assert "g_agents_real_inbound = false;" in repaint, (
    "the repaint block must consume (clear) the real-inbound flag"
)
assert "if (real_in) ui_note_wake();" in repaint, (
    "mesh/WiFi arrival parity: a genuine inbound line in the open room wakes"
)
assert repaint.count("ui_note_wake") == 1, (
    "the repaint block may wake only behind the real-inbound guard"
)
assert "ui_note_input" not in repaint, "a repaint is never user input"

# --- the real-inbound flag never outlives its display_force ------------------
consume = arrival[: arrival.index("NotifyView v;")]
assert "g_agents_real_inbound = false;" in consume, (
    "when the room is not on screen the arrival flag must be dropped, not kept"
)

# --- real_inbound classification of every agents_on_inbound producer ---------
assert "static volatile bool g_agents_real_inbound = false;" in agents
assert 'agents_on_inbound(agent, text, true);' in agents, (
    "HTTP /agents/inbound is a genuine arrival: real_inbound = true"
)
assert "agents_on_inbound(g_convs[idx].id, line, true);" in agents, (
    "a GPS answer landing (possibly minutes later) is an arrival for the user"
)
assert 'agents_on_inbound(agent, "(mesh delivery failed - resend)", false);' in mesh, (
    "the synthetic failure line must never claim to be a genuine arrival"
)
onin = agents[agents.index("static void agents_on_inbound(const char *agent_id,"
                           " const char *text,\n                              "
                           "bool real_inbound) {") :]
onin = onin[: onin.index("static bool agents_clear")]
assert "if (real_inbound) g_agents_real_inbound = true;" in onin
assert onin.index("if (real_inbound) g_agents_real_inbound = true;") < onin.index(
    "display_force = true;"
), "the flag must be set before display_force so loop() sees them together"

# --- mesh C1 RX into a known room counts as a genuine arrival ----------------
c1 = mesh[mesh.index('strncmp(text, "C1|", 3)') :]
c1 = c1[: c1.index("mesh_status_json")]
assert "g_agents_real_inbound = true;" in c1, (
    "a chat line arriving over LoRa must wake the open room like WiFi does"
)
assert c1.index("g_agents_real_inbound = true;") < c1.index("display_force = true;")

# --- mesh TX failure line: per-agent, only on that room's success->fail edge --
assert "static bool g_mesh_chat_tx_failed[AGENTS_N]" in mesh, (
    "the failure edge is per agent room, not one global bit"
)
fail = mesh[mesh.index("static void mesh_chat_tx_fail") :]
fail = fail[: fail.index("static void mesh_chat_tx_poll")]
assert "int aidx = agents_find(agent);" in fail
assert "if (aidx < 0) return;" in fail, (
    "unknown/empty agent: no room to warn, and no flag may be set"
)
assert "if (g_mesh_chat_tx_failed[aidx]) return;" in fail, (
    "repeated failures while that room is already failed must stay silent"
)
assert "g_mesh_chat_tx_failed[aidx] = true;" in fail
assert fail.index("if (aidx < 0) return;") < fail.index(
    "g_mesh_chat_tx_failed[aidx] = true;"
) < fail.index("agents_on_inbound"), (
    "the flag may be set only when the synthetic line is actually injected"
)

# --- a chat ACK re-arms that room; ANY link proof re-arms every room ---------
ack = mesh[mesh.index("if (g_mesh_chat_tx.ack_seen)") :]
ack = ack[: ack.index("g_mesh_chat_tx.next++")]
assert "g_mesh_chat_tx_failed[aidx] = false;" in ack, (
    "a successful chat ACK must re-arm the failure notification for its room"
)
mark = mesh[mesh.index("static void mesh_link_mark_ok") :]
mark = mark[: mark.index("static int mesh_alive_age_s")]
assert (
    "for (int i = 0; i < AGENTS_N; i++) g_mesh_chat_tx_failed[i] = false;" in mark
), (
    "probe/keepalive ACKs prove recovery: mesh_link_mark_ok must re-arm all "
    "rooms or the next outage after a probe-proven recovery is silent"
)

# --- idle constants and the dim/card invariant are untouched -----------------
assert "#define UI_IDLE_MS        15000" in main
assert "#define UI_IDLE_REPLY_MS  60000" in main
assert "static_assert(BL_IDLE_DIM_MS > UI_IDLE_MS" in main, (
    "backlight must still be forbidden from dimming under a live card"
)

print("idle policy tests: OK")
