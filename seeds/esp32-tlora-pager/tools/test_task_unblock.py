#!/usr/bin/env python3
"""Static regressions: no blocking waits on the loop task or the AsyncTCP task.

Five sites, one rule each:
  - /wifi/config and /wifi/networks handlers only raise a flag; the loop task
    walks mode -> disconnect -> begin on millis() settle gaps.
  - The PING screen is a state machine advanced from loop(); the old
    skill_meshcore_poll()+delay(25) spin is gone and leaving the face cancels.
  - gps.cpp keeps no busy-wait at all (gps_take_fix had no callers and is
    removed; request+poll via gps_request_fix()/gps_tick() is the only path).
  - Every loop-task HTTPClient site bounds the connect phase to ~1 s, and the
    /wg/restart, /wg/start and /wg/stop handlers never touch the tunnel on the
    AsyncTCP task — they raise flags consumed by skill_wg_poll.
    /wg/restart handler no longer sits in delay(200) on the AsyncTCP task.
  - rns.cpp drives a TCP socket from the loop task, which is where every
    NetworkClient blocking call lives: connect() (3 s + DNS), write() (10 x a
    1 s select) and readBytes() (delay(2) until getTimeout()). None of them may
    appear on the loop task, and the socket drain must carry explicit caps.
  - rns.cpp also hands Transport a packet filter callback that runs once per
    inbound packet, on the loop task, inside that same drain budget. It must be
    registered before the stack starts, must be a bare function pointer with no
    state beyond file scope, and must publish its counters through the loop-task
    snapshot like everything else GET /rns/status serves. Its body is pinned as
    an allowlist rather than screened against a list of forbidden identifiers:
    that list could not be completed, and the two things it missed (a log_e()
    call, a by-value RNS::Bytes local) are exactly the allocating and blocking
    work it existed to keep out.
  - rns.cpp sets no microStore size caps of its own, and the known-destinations
    store stays at the library default. The firmware that capped those stores
    before Transport::start() was rolled back off the device; the caps went with
    the flag that needed them, and neither may come back unnoticed.
"""

import re
from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
gps = (ROOT / "src" / "skills" / "gps.cpp").read_text(encoding="utf-8")
wg = (ROOT / "src" / "skills" / "wg.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
rns = (ROOT / "src" / "skills" / "rns.cpp").read_text(encoding="utf-8")

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

# --- 5. rns.cpp: a TCP socket driven from the loop task ----------------------
# Not a substring test for "delay(": the obvious next thing to reach for here is
# vTaskDelay(), which that would sail straight past on the capital D. Match the
# whole family (delay, vTaskDelay, delayMicroseconds, ets_delay_us, ...) plus
# sleep()/usleep(), which is the spelling the library's own blocking waits use.
#
# It runs against code only: the file header names the library's own blocking
# calls (OS::sleep(0.2) inside Transport::write_path_table, among others) and
# describing them must not fail the test that forbids writing them.
rns_code = re.sub(r"/\*.*?\*/", " ", rns, flags=re.S)
rns_code = re.sub(r"//[^\n]*", " ", rns_code)
#
# setNoDelay() is the one member of the family that is not a wait — it is the
# Nagle switch, and turning it ON is what would cost latency.
WAIT_ALLOWED = ("setNoDelay(",)
waits = [w for w in re.findall(r"\b\w*[Dd]elay\w*\s*\(", rns_code)
         if w not in WAIT_ALLOWED]
waits += re.findall(r"\bu?sleep\s*\(", rns_code)
assert not waits, (
    "no Reticulum code may wait in place on any task; found %r" % sorted(set(waits))
)
assert "while (WiFi.status()" not in rns, (
    "the upstream UDPInterface::start() spin must never be copied in"
)
assert "WiFi.begin(" not in rns, "main.cpp owns the single WiFi.begin() call"

# The three NetworkClient calls that can sit for seconds. connect() is allowed
# only inside the one-shot task; write()/readBytes() are not used at all.
assert ".readBytes(" not in rns, (
    "Stream::readBytes loops on delay(2) until getTimeout() — read(buf, n) is "
    "the MSG_DONTWAIT call to use"
)
assert "g_rns_client.write(" not in rns and "client.write(" not in rns, (
    "NetworkClient::write() retries 10x around a 1 s select(); the interface "
    "must call lwip_send(MSG_DONTWAIT) itself"
)
assert "MSG_DONTWAIT" in rns and "lwip_send(" in rns

# connect() lives on its own task, and the connection state is the baton that
# keeps the loop task off the client while that task owns it.
assert "xTaskCreate(rns_connect_task" in rns, (
    "the blocking connect must run on a one-shot task, not the loop task"
)
task = rns[rns.index("static void rns_connect_task") :
          rns.index("class RnsTcpInterface")]
assert "g_rns_client.connect(" in task, (
    "the only connect() call belongs inside the connect task"
)
assert rns.count("g_rns_client.connect(") == 1
assert "handle_incoming" not in task and "rns_stack" not in task, (
    "the connect task must not touch the library: Transport has no locks"
)
loop_body = rns[rns.index("void RnsTcpInterface::loop()") :
                rns.index("static const char *rns_link_state_name")]
# Not `"RNS_CS_CONNECTING" in loop_body and "return;" in loop_body`: "return;"
# occurs in every branch of this function, so that pair would still pass with
# the early return deleted. Pin the shape of the guard instead — an if on the
# CONNECTING state whose block returns — and then pin that nothing which touches
# the client can run ahead of it.
guard = re.search(r"if \(cs == RNS_CS_CONNECTING\) \{[^{}]*\breturn;[^{}]*\}",
                  loop_body)
assert guard, (
    "loop() must open with `if (cs == RNS_CS_CONNECTING) { ... return; }` — "
    "while that task owns the client, nothing here may read or write it"
)
before_guard = loop_body[: guard.start()]
for call in ("g_rns_client.", "drain()", "tx_flush()", "begin_connect()",
             "link_down(", "wg_is_up()", "WiFi.status()"):
    assert call not in before_guard, (
        "%s runs before the RNS_CS_CONNECTING guard, so it can race the connect "
        "task for the socket" % call
    )

# The drain is bounded three ways, and every cap is a named constant.
for macro, why in (
    ("#define RNS_TCP_READ_CHUNK", "bytes per socket read"),
    ("#define RNS_TCP_DRAIN_BYTES_MAX", "bytes per tick"),
    ("#define RNS_TCP_DRAIN_FRAMES_MAX", "frames per tick"),
    ("#define RNS_TCP_DRAIN_BUDGET_MS", "milliseconds per tick"),
):
    assert macro in rns, "the drain needs an explicit cap on %s" % why

drain = rns[rns.index("void RnsTcpInterface::drain()") :
            rns.index("bool RnsTcpInterface::send_outgoing")]
assert "RNS_TCP_DRAIN_BYTES_MAX" in drain
assert "RNS_TCP_DRAIN_FRAMES_MAX" in drain
assert "RNS_TCP_DRAIN_BUDGET_MS" in drain
assert "millis() - started" in drain, "the drain must watch a millisecond budget"
assert "g_rns_client.available()" in drain, (
    "available() (FIONREAD ioctl) is the only safe way to size a read"
)

# The unsent tail is bounded too: MSG_DONTWAIT can short-write, and a tail that
# never drains kills the socket rather than wedging the interface.
flush = rns[rns.index("void RnsTcpInterface::tx_flush()") :
            rns.index("void RnsTcpInterface::drain()")]
assert "RNS_TCP_TX_FLUSH_TRIES" in flush, "the flush retry count must be capped"
assert "#define RNS_TCP_TX_STALL_MS" in rns
assert "RNS_TCP_TX_STALL_MS" in loop_body

# POST /rns/config only writes the file; the loop task applies it, so the
# endpoint the connect task reads never changes underneath it.
cfg = rns[rns.index('exact("/rns/config")') :
          rns.index("static void skill_rns_poll")]
assert "g_rns_cfg_dirty = true;" in cfg
assert "begin_connect" not in cfg and "link_down" not in cfg, (
    "the handler must only raise the flag; the poll owns the socket"
)

# The tick is on, and it is throttled rather than free-running.
assert ".tick = skill_rns_poll" in rns, "Reticulum::loop() needs a caller"
assert "#define RNS_TICK_MS" in rns

# --- 6. rns.cpp: the inbound announce prefilter ------------------------------
# The filter callback is the single hottest piece of code this firmware hands to
# a third-party stack: Transport::inbound() calls it once per inbound packet,
# synchronously, on the loop task, inside the drain budget. Everything below
# pins a property that makes that safe.
pktfilter = (ROOT / "src" / "rns" / "pktfilter.h").read_text(encoding="utf-8")

# 6a. The decision is a pure function in its own header, so the host test can
# compile it directly and so the callback has nothing to decide.
assert "rns_filter_keep_packet" in pktfilter and "rns_filter_keep_packet" in rns, (
    "the keep/drop decision must live in rns/pktfilter.h and be called from here"
)
assert '#include "rns/pktfilter.h"' in rns
# Pure means pure: no state of any kind in that header, or the same packet
# arriving twice could be judged differently — and a dropped packet never enters
# Transport's hashlist, so retransmissions DO reach the filter again.
pktfilter_code = re.sub(r"/\*.*?\*/", " ", pktfilter, flags=re.S)
pktfilter_code = re.sub(r"//[^\n]*", " ", pktfilter_code)
assert not re.search(r"^\s*static\s+(?!inline\b)", pktfilter_code, re.M), (
    "rns/pktfilter.h must hold no static state; the decision is a pure function"
)
for banned in ("millis(", "esp_timer", "printf", "new ", "malloc"):
    assert banned not in pktfilter_code, (
        "the decision function must not reach for %s" % banned
    )

# 6b. Registered BEFORE the stack starts. Transport reads the callback pointer
# on every packet with no other guard, so a late registration is just a window
# in which announces are verified at full price.
init = rns[rns.index("static void skill_rns_init") :]
assert "RNS::Transport::set_filter_packet_callback(rns_packet_filter);" in init, (
    "the prefilter must be registered on Transport"
)
assert init.index("set_filter_packet_callback") < init.index("rns_stack.start()"), (
    "the filter must be registered BEFORE reticulum start(), not after"
)

# 6c. A bare function pointer, not a lambda: Transport::Callbacks::filter_packet
# is `bool(*)(const Packet&)`, so state can only live at file scope.
assert re.search(r"^static bool rns_packet_filter\(const RNS::Packet ",
                 rns, re.M), (
    "the callback must be a file-scope function matching bool(*)(const Packet&)"
)
assert "set_filter_packet_callback([" not in rns, (
    "the hook takes a bare function pointer; a capturing lambda cannot convert"
)

# 6d. The callback body, pinned statement by statement — an ALLOWLIST, not a
# denylist. This section used to ban a list of identifiers (new, malloc, String,
# Serial., delay, ...) and that list could not be finished: `log_e("filter")`
# passed it, because arduino-esp32's logging macros are not spelled Serial. and
# write to the UART just as blockingly; `RNS::Bytes h = packet.packet_hash();`
# passed it too, because a by-value Bytes is a heap allocation with none of the
# banned words in it. Guessing the next forbidden identifier is a losing game,
# so the rule is inverted: comments are stripped, whitespace is normalised, and
# what is left has to equal exactly what the callback is allowed to be, in
# order. Anything added, removed, reworded or reordered fails here and has to be
# argued for by editing this list.
#
# Why the body is allowed so little: Transport::inbound() calls it once per
# inbound packet, synchronously, on the loop task, inside the drain budget, so
# it must be O(1), must not allocate, must not block and must not be able to
# throw. Fail-open covers only part of that last one — Transport.cpp wraps the
# call in `catch (const std::exception&)`, and anything else escapes inbound()
# with _jobs_locked left true, after which Transport::jobs() never runs again.
# The list below also subsumes the positive assertions this section used to
# carry (the two field reads, the delegation, the two counters): they are lines
# in it.
cb = rns[rns.index("static bool rns_packet_filter") :]
cb = cb[: cb.index("\n}\n") + 2]
cb = re.sub(r"/\*.*?\*/", " ", cb, flags=re.S)
cb = re.sub(r"//[^\n]*", " ", cb)
cb_body = [re.sub(r"\s+", " ", line).strip() for line in cb.splitlines()]
cb_body = [line for line in cb_body if line]
CB_ALLOWED = [
    "static bool rns_packet_filter(const RNS::Packet &packet) {",
    "uint8_t type = (uint8_t)packet.packet_type();",
    "uint8_t context = (uint8_t)packet.context();",
    "bool keep = rns_filter_keep_packet(type, context);",
    "if (type == RNS_PKT_TYPE_ANNOUNCE) {",
    "if (keep) {",
    "g_rns_ann_kept++;",
    "} else {",
    "g_rns_ann_dropped++;",
    "}",
    "}",
    "return keep;",
    "}",
]
assert cb_body == CB_ALLOWED, (
    "the filter callback body is pinned statement for statement; it runs once "
    "per inbound packet inside the drain budget, so anything new in it has to "
    "be argued for here first.\n  expected: %r\n  found:    %r"
    % (CB_ALLOWED, cb_body)
)

# 6e. The constants the decision uses are checked against the library's enums at
# compile time, so a drifting value fails the build and not the device.
assert "static_assert(RNS_PKT_TYPE_ANNOUNCE == (uint8_t)RNS::Type::Packet::ANNOUNCE" in rns
assert "RNS::Type::Packet::PATH_RESPONSE" in rns, (
    "PATH_RESPONSE must be pinned to the library enum too"
)

# 6f. The counters reach GET /rns/status through the loop-task snapshot, like
# every other number that handler serves — never read live from the AsyncTCP
# task.
publish = rns[rns.index("static void rns_status_publish") :
              rns.index("static void rns_status_json")]
json_builder = rns[rns.index("static void rns_status_json") :
                   rns.index("static const SkillEndpoint")]
assert "g_rns_snap.ann_dropped = g_rns_ann_dropped;" in publish
assert "g_rns_snap.ann_kept = g_rns_ann_kept;" in publish
assert '"announces_dropped"' in json_builder and '"announces_kept"' in json_builder, (
    "the filter must be provable from /rns/status"
)
assert "g_rns_snap.ann_dropped" in json_builder and "g_rns_snap.ann_kept" in json_builder
assert "g_rns_ann_dropped" not in json_builder and "g_rns_ann_kept" not in json_builder, (
    "the status handler must serve the snapshot, not the live counters"
)
# The drain timing this filter exists to fix stays published alongside them.
assert '"drain_us_max"' in json_builder, (
    "drain_us_max must survive: it is the number the filter is judged by"
)

# --- 7. rns.cpp: nothing else runs between the filter and start() ------------
# A previous revision capped two microStore heap stores here, by calling
# Identity::known_destinations_maxsize() and Transport::path_table_maxsize()
# before Transport::start() — i.e. on stores whose init() the library only runs
# inside `if (Reticulum::transport_enabled())`, which is false on this node.
# That firmware panic-looped on the device and was rolled back. The caps are gone
# and so is the -DRNS_PERSIST_KNOWN_DESTINATIONS=0 flag that made them look
# necessary; this pins that neither comes back by accident. It does NOT pin a
# cause: set_max_recs() is a scalar assignment in both store flavours and cannot
# fault, so what these lines are guilty of is being unnecessary, not being the
# crash. Comments are stripped first, because the note at the call site names
# both setters.
init_code = re.sub(r"/\*.*?\*/", " ", init, flags=re.S)
init_code = re.sub(r"//[^\n]*", " ", init_code)
for setter in ("known_destinations_maxsize(", "path_table_maxsize("):
    assert setter not in init_code, (
        "%s...) was removed with the flag that needed it; it capped a store the "
        "library never initialises on this node" % setter
    )

ini = (ROOT / "platformio.ini").read_text()
assert "-DRNS_PERSIST_KNOWN_DESTINATIONS=0" not in ini, (
    "the known-destinations store stays at the library default: at 0 it becomes "
    "a live heap store that allocates on the receive path and has to be capped "
    "from here, and the firmware that did that was rolled back off the device"
)
# The two that stay, and the reason they need nothing from us: the path store
# has been live across every build that booted here, and the packet hashlist is
# capped by the library at the top of Transport::start(), outside the
# transport_enabled() block.
assert "-DRNS_PERSIST_PATHS=0" in ini
assert "-DRNS_PERSIST_HASHLIST=0" in ini

print("Task unblock policy tests: OK")
