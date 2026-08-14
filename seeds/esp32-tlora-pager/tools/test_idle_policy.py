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
# Covers the chat door pop, the severity-card branch, the compose branch and
# the on_clock tail: two wakes in arrival (door invite + severity) plus one
# in the drain (open-room refresh). No user-input stamps.
arrival = main[main.index("uint32_t arrived_id = 0;") :]
arrival = arrival[: arrival.index("hw_sound_poll();")]
drain = main[main.index("static bool notify_reconcile_pending_chats(") :]
drain = drain[: drain.index("static void agents_head_time(")]
assert arrival.count("ui_note_wake();") == 2 and drain.count("ui_note_wake();") == 1, (
    "arrival handling must wake for the door card, a severity card, and an open chat"
)
assert "ui_note_input" not in arrival, (
    "an arriving card is a system event, not user input"
)
assert "ui_note_wake();" in drain, (
    "a chat message landing in the open room must wake the panel"
)
sev = arrival[arrival.index("// Real message from any service") :]
sev = sev[: sev.index("} else {")]
assert "ui_note_wake();" in sev, "an arriving severity card must light the screen"

# --- an arriving chat lands in its thread AND as a door card ---
# The body is queued into the conversation; the notification is the CHAT
# invite (not a severity card). Must not stamp user input.
assert "agents_chat_door_enqueue(" in drain, (
    "an arriving chat must enqueue off-loop delivery into its conversation"
)
assert "hw_ui_show_agent_invite" in arrival, (
    "an arriving chat must pop the door card, not disappear into the thread"
)
assert arrival.count("hw_ui_show_notify") == 1, (
    "severity cards still pop via show_notify; chat must not reuse that path"
)
assert "ui_note_input" not in drain, (
    "an arriving chat is a system event, not user input"
)

# Mesh C1 side=a (Hermes reply over radio) must raise a door card. Thread-only
# append leaves the clock silent: radio ACK ≠ user saw it.
mesh = (ROOT / "src" / "skills" / "meshcore.cpp").read_text(encoding="utf-8")
c1 = mesh[mesh.index("} else if (strncmp(text, \"C1|\", 3) == 0) {") :]
c1 = c1[: c1.index("} else if (strncmp(text, \"L1|\", 3) == 0) {")]
assert 'snprintf(door, sizeof(door), "%s-chat", r->agent);' in c1
assert "notify_ingest" in c1
assert "if (from_me)" in c1
assert "agents_push_line(idx, false, body)" in c1, (
    "agent-side C1 writes the thread once, then a short door teaser"
)
assert "if (!agents_is_on_screen(idx))" in c1, (
    "an open chat must not raise another inbox doorbell"
)
assert "display_force = true" in c1, (
    "C1 side=a into an open room must force a chat repaint; radio ACK is not a draw"
)
assert "scr == HW_UI_WIFI" in arrival and "scr == HW_UI_NET" in arrival, (
    "a mesh reply must still pop the invite after TOGGLE WIFI / Network"
)

# --- CLEAR CHAT on an agent room also sends /new (one click, not two) --------
act = main[main.index("static void ui_agent_act_confirm()") :]
act = act[: act.index("static void ui_open_info()")]
assert 'agents_send(aid, "/new")' in act, (
    "CLEAR CHAT must send /new so the user does not type it after every wipe"
)
assert "CONV_AGENT" in act, "peer chats must not get a /new after CLEAR"

# --- a chat repaint is not activity ------------------------------------------
refresh = main[main.index("static void ui_agent_chat_refresh() {") :]
refresh = refresh[: refresh.index("static void ui_open_agent_chat")]
assert "ui_note_input" not in refresh, (
    "repainting the agent chat must not reset the idle clock"
)
assert "ui_note_wake" not in refresh, (
    "repainting the agent chat must not wake the panel either"
)
assert "if (agent_scroll >= 0) agent_scroll = shown;" in refresh, (
    "the follow-tail sentinel (-1) must survive a repaint"
)
assert "agent_scroll = hw_ui_show_agent_chat" not in refresh, (
    "writing the rendered max_scroll back into agent_scroll loses follow-tail"
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
assert "if (real_in) agent_scroll = -1;" in repaint, (
    "a reply into the open room must jump to the latest line"
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
inbound_http = agents[agents.index('server.on(AsyncURIMatcher::exact("/agents/inbound")'):]
inbound_http = inbound_http[:inbound_http.index('server.on(AsyncURIMatcher::exact("/agents/bridge")')]
assert "agents_inbound_enqueue(" in inbound_http, (
    "HTTP agent replies must enqueue the full body, not a 241-byte notify card"
)
assert "inbox_deliver_card(" not in inbound_http, (
    "a notify card cannot hold the chat line"
)
assert "agents_inbound_recent(" in agents, (
    "WiFi + C1 of the same long body must dedup by full-text hash, not last chunk"
)
push = agents[agents.index("static bool agents_push_line_with_origin("):]
push = push[: push.index("static bool agents_push_line(")]
assert "agents_inbound_recent(idx, text)" in push
assert "strcmp(g_convs[idx].last.text, text)" not in push, (
    "last.text is one 511-byte chunk and misses a long dual-path reply"
)
assert "agents_on_inbound(" not in inbound_http, (
    "the HTTP task must not persist to SD"
)
assert 'const char *key   = input["id"]' in inbound_http
assert "agents_inbound_key_duplicate(key)" in inbound_http
assert "agents_inbound_key_commit(key)" in inbound_http
door_worker = agents[agents.index("if (item.kind == 3)"):]
door_worker = door_worker[:door_worker.index("if (idx < 0) continue;")]
assert "agents_on_inbound(" in door_worker and "item.door_event" in door_worker, (
    "the off-loop card worker must be the canonical thread receiver"
)
assert "agents_on_inbound(g_convs[idx].id, line, true);" in agents, (
    "a GPS answer landing (possibly minutes later) is an arrival for the user"
)
assert "agents_mark_last_pending(aidx, AGENT_DELIV_FAIL)" in mesh, (
    "mesh TX give-up marks delivery fail without injecting a chat spam line"
)
assert "agents_on_inbound(agent," not in mesh[mesh.index("static void mesh_chat_tx_fail"):
                                               mesh.index("static void mesh_chat_tx_poll")], (
    "mesh TX fail must not inject a system line into the chat"
)
onin = agents[agents.index("static bool agents_on_inbound(const char *agent_id,"
                           " const char *text,\n                              "
                           "bool real_inbound, uint32_t origin_id,") :]
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
assert "notify_ingest" in c1, (
    "agent-side C1 must raise a door card, not only append the thread"
)
assert "agents_inbound_key_duplicate(delivery_key)" in c1, (
    "a looped-back user line or replayed agent delivery must not append twice"
)
assert "agents_inbound_key_commit(delivery_key)" in c1

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
assert "agents_mark_last_pending(aidx, AGENT_DELIV_FAIL)" in fail
assert fail.index("if (aidx < 0) return;") < fail.index(
    "g_mesh_chat_tx_failed[aidx] = true;"
) < fail.index("agents_mark_last_pending"), (
    "the flag may be set only together with the delivery-fail mark"
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

# --- one chat, one inbox row: opening the room ACKs leftover doorbells ------
open_chat = main[main.index("static void ui_open_agent_chat(") :]
open_chat = open_chat[: open_chat.index("static void ui_agent_sessions_refresh")]
assert "notify_ack_open_chat(idx);" in open_chat, (
    "opening a room must clear the unread doorbells that used to stick at 4"
)

# --- outgoing delivery ticks (mesh ACK / WiFi 2xx) ---------------------------
assert "agents_mark_last_pending(int idx, uint8_t status)" in agents
assert "AGENT_DELIV_PEND" in agents and "AGENT_DELIV_OK" in agents
assert "agents_mark_last_pending(idx, AGENT_DELIV_OK)" in agents, (
    "a WiFi/RNS/LXMF 2xx must flip the last pending mine line to delivered"
)
assert "agents_mark_last_pending(done, AGENT_DELIV_OK)" in mesh, (
    "a completed mesh chat ACK must flip the last pending mine line"
)
assert "agents_mark_last_pending(aidx, AGENT_DELIV_FAIL)" in fail, (
    "a mesh TX give-up must mark the last pending mine line failed"
)
ui = (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")
assert "agent_chat_mark" in ui
assert "if (delivery == 2) return '*';" in ui
assert "if (delivery == 1) return '~';" in ui

# --- keyboard backlight: GPIO46 is a strap pin, never PWM/LEDC -------------
# No ALT+B steal: that pad is SYM '!'. park() LOW before anything else in
# setup() and on shutdown; after boot plain GPIO HIGH is the backlight.
kb = (ROOT / "src" / "hw_kb.cpp").read_text(encoding="utf-8")
assert "ledcAttach" not in kb
assert "hw_kb_park_backlight" in kb
assert "if (k == KEY_B && alt_held)" not in kb
begin = kb[kb.index("bool hw_kb_begin()"):]
begin = begin[: begin.index("void hw_kb_park_backlight")]
assert "digitalWrite(PIN_KB_BACKLIGHT, HIGH)" in begin, (
    "keyboard backlight must come on after boot"
)
assert "hw_kb_park_backlight();" in (ROOT / "src" / "main.cpp").read_text(
    encoding="utf-8"
), "setup must park GPIO46 LOW before other init"

print("idle policy tests: OK")
