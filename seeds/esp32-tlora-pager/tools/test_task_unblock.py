#!/usr/bin/env python3
"""Static regressions: no blocking waits on the loop task or the AsyncTCP task.

Eight sites, one rule each:
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
  - rns.cpp also owns a Destination, which puts a SECOND callback on the loop
    task inside the drain: Destination::set_packet_callback fires once per
    inbound packet addressed to this node. It gets the same allowlist treatment
    as the filter callback, for the same reason and against the same two misses
    (a log call, a by-value RNS::Bytes). The destination itself must decline
    link requests and must not ask for PROVE_ALL — both would run public-key
    work inside that budget — and the announce schedule must stay a pure
    function in its own header rather than an inline millis() comparison.
    The reconnect edge is latched rather than sampled, and the latch is cleared
    by nothing but an announce: the transition lasts one tick, so a floor
    applied to the raw sample discards every reconnect it refuses instead of
    deferring it.
  - That callback now DEFERS instead of counting: an inbound payload becomes a
    notification card, and notify_ingest() calls time(NULL), takes the
    notify_mux critical section and appends to the event ring — three things
    the callback may not do. So it copies into a fixed buffer and the loop task
    raises the card, after rns_stack.loop() rather than before it. The queue's
    rules live in a pure header like every other decision in this file, and the
    pickup's position in the tick is pinned: ahead of the stack it would serve
    last tick's message.
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

# --- 8. rns.cpp: the destination's inbound packet callback -------------------
# The node is addressable now, which puts a second callback of ours on the loop
# task inside the drain: Destination::Callbacks::packet fires once per inbound
# packet addressed to this destination, reached from handle_incoming() ->
# Transport::inbound() -> Destination::receive(). Everything below pins a
# property that keeps that affordable, and it mirrors section 6 deliberately —
# the two hooks have the same budget and the same failure modes.

# 8a. Registered at all. Without a packet callback an inbound DATA packet is
# decrypted and silently discarded, and there would be no evidence anything ever
# arrived — the counter it feeds is the only proof the address works.
assert "rns_destination.set_packet_callback(rns_data_callback);" in rns, (
    "the destination must register a packet callback; without one an inbound "
    "packet is decrypted and dropped without a trace"
)

# 8b. A bare function pointer, not a lambda: Destination::Callbacks::packet is
# `void(*)(const Bytes&, const Packet&)`, so state can only live at file scope.
assert re.search(r"^static void rns_data_callback\(const RNS::Bytes ", rns, re.M), (
    "the callback must be a file-scope function matching "
    "void(*)(const Bytes&, const Packet&)"
)
assert "set_packet_callback([" not in rns, (
    "the hook takes a bare function pointer; a capturing lambda cannot convert"
)

# 8c. The callback body, pinned statement by statement — an ALLOWLIST, exactly
# like section 6d and for exactly the reasons given there. The two things a
# denylist of forbidden identifiers missed on the filter callback apply here
# verbatim and then some: `log_e("rx")` is not spelled Serial. but blocks on the
# UART just the same, and a single dropped `&` on the Bytes argument buys
# refcount traffic, a pinned heap block and — the moment such a local is
# reassigned — a free() inside the drain.
#
# The body grew by exactly one statement in the delivery commit: the payload is
# handed to rns_inbox_put(), which is a memcpy into a fixed buffer plus scalar
# stores and nothing else (rns/inbox.h, host-tested). What it must NOT grow into
# is the card itself — notify_ingest() calls time(NULL), takes the notify_mux
# critical section and appends to the event ring, all forbidden here — so the
# list stays a list and anything new in it has to be argued for by editing it.
dcb = rns[rns.index("static void rns_data_callback") :]
dcb = dcb[: dcb.index("\n}\n") + 2]
dcb = re.sub(r"/\*.*?\*/", " ", dcb, flags=re.S)
dcb = re.sub(r"//[^\n]*", " ", dcb)
dcb_body = [re.sub(r"\s+", " ", line).strip() for line in dcb.splitlines()]
dcb_body = [line for line in dcb_body if line]
DCB_ALLOWED = [
    "static void rns_data_callback(const RNS::Bytes &data, const RNS::Packet &packet) {",
    "(void)packet;",
    "rns_inbox_put(&g_rns_inbox, data.data(), data.size());",
    "}",
]
assert dcb_body == DCB_ALLOWED, (
    "the destination packet callback body is pinned statement for statement; it "
    "runs once per inbound packet inside the drain budget, so anything new in "
    "it has to be argued for here first.\n  expected: %r\n  found:    %r"
    % (DCB_ALLOWED, dcb_body)
)

# 8d. The destination must not sign anything on the drain's behalf. Both of
# these are library DEFAULTS pointing the wrong way for this node:
# _accept_link_requests initialises to TRUE, and an accepted LINKREQUEST runs an
# X25519 exchange plus an Ed25519 signature synchronously inside the 8 ms drain
# for any stranger who can reach the interface. PROVE_ALL would sign a proof per
# inbound DATA packet in the same place. Nothing here terminates a link yet.
assert "rns_destination.accepts_links(false);" in rns, (
    "accepts_links defaults to TRUE; an inbound link request would run an "
    "X25519 handshake and an Ed25519 sign inside the drain"
)
# Against the comment-stripped source: both this rule and the lxmf one below are
# explained in prose at their call sites, and describing what must not be
# written may not fail the test that forbids writing it.
assert "PROVE_ALL" not in rns_code, (
    "the proof strategy stays at the default PROVE_NONE: PROVE_ALL is an "
    "Ed25519 signature per inbound DATA packet, inside the drain, and "
    "reachability is answered by path requests which need no proof strategy"
)

# 8e. The destination is built on the STORED identity. Destination.cpp mints a
# throwaway keypair when an IN/SINGLE destination is handed a NONE identity —
# silently, with no error — and the address would then change on every boot.
dest = rns[rns.index("rns_destination = RNS::Destination(") :]
dest = dest[: dest.index("rns_dest_ok = true;")]
assert "rns_identity," in dest, (
    "the destination must be constructed with the loaded identity; a NONE "
    "identity makes Destination mint a throwaway keypair and the address moves"
)
assert "RNS::Type::Destination::IN" in dest and "RNS::Type::Destination::SINGLE" in dest
# The address is the app_name/aspects pair; changing either moves it, so pin
# both. In particular this must NOT squat lxmf.delivery, which belongs to a
# separate destination that does not exist yet.
assert '#define RNS_DEST_APP_NAME "seed"' in rns
assert '#define RNS_DEST_ASPECTS  "pager"' in rns
assert "lxmf" not in rns_code.lower(), (
    "the LXMF delivery destination is a later commit with its own address; "
    "naming this one lxmf.delivery now would have to be undone"
)

# 8f. The announce schedule is a pure function in its own header, so the host
# test can compile it and so the decision is not an inline millis() comparison
# nobody can exercise. Announce() is synchronous and signs with software
# Ed25519 on the loop task, so "may we announce now?" is worth testing.
annsched = (ROOT / "src" / "rns" / "annsched.h").read_text(encoding="utf-8")
assert '#include "rns/annsched.h"' in rns
assert "rns_announce_due" in annsched and "rns_announce_due" in rns
annsched_code = re.sub(r"/\*.*?\*/", " ", annsched, flags=re.S)
annsched_code = re.sub(r"//[^\n]*", " ", annsched_code)
assert not re.search(r"^\s*static\s+(?!inline\b)", annsched_code, re.M), (
    "rns/annsched.h must hold no static state; the decision is a pure function "
    "of the stamps its caller passes in"
)
for banned in ("millis(", "micros(", "esp_timer", "printf", "new ", "malloc"):
    assert banned not in annsched_code, (
        "the schedule must not reach for %s — every clock value is an argument, "
        "which is what makes it testable" % banned
    )
# Unsigned subtraction, not `now >= last + interval`: the latter breaks for a
# rollover's worth of time every ~49.7 days. tools/test_rns_annsched.sh proves
# the behaviour near 2^32; this pins the form so it cannot be rewritten back.
assert "(uint32_t)(now_ms - online_since_ms) >=" in annsched_code
assert annsched_code.count("(uint32_t)(now_ms - last_announce_ms) >=") == 2, (
    "both the steady-state interval and the reconnect floor must compare by "
    "unsigned subtraction from the last announce"
)
# The reconnect branch must stay FLOORED. `if (!was_online) return true;` is
# the shape this started as, and it is a real defect rather than a style
# preference: RnsTcpInterface::loop() resets g_rns_backoff_ms to
# RNS_TCP_BACKOFF_MIN_MS on every successful connect, so the ladder never climbs
# and a flapping peer offers an offline->online edge every ~3 s forever — one
# software Ed25519 signature each, on the loop task.
assert "RNS_ANNOUNCE_RECONNECT_MIN_MS" in annsched_code, (
    "the offline->online branch needs a floor; without one a flapping link "
    "signs an announce every RNS_TCP_BACKOFF_MIN_MS indefinitely"
)
assert "if (edge_pending) return true;" not in annsched_code, (
    "the reconnect edge must not be unconditional — see above"
)

# 8f-bis. THE FLOOR MUST DEFER, NOT DROP, and that property lives in the seam
# between the header and this file rather than in either one. The edge is a
# ONE-TICK event and rns_announce_poll() stamps g_rns_ann_was_online on every
# tick whether or not an announce fired, so a floor applied to the raw sample
# CONSUMES a refused reconnect: the next tick sees was_online true, falls
# through to the 30 min branch, and the announce is not postponed to
# last + 60 s — it is lost. Any reconnect within a minute of any announce went
# with it, while /rns/status reported ready, announced:true, online:true and a
# climbing up_age_s. The fix is a sticky latch, and its whole discipline is
# "set on the transition, cleared by nothing but an announce". Pin that here;
# tools/test_rns_annsched.sh section 8 drives the timeline that proves it.
assert "rns_announce_edge_latch" in annsched_code, (
    "the transition must be latched, not sampled: it is visible for one tick "
    "and a floor that refuses it would otherwise discard it"
)
assert rns_code.count("rns_announce_edge_latch(") == 1, (
    "exactly one latch site, in rns_announce_poll()"
)
# The announce poll, comments stripped — reused by 8g below. Prose at the call
# sites explains these hazards by naming them, and describing what must not be
# written may not fail the test that forbids writing it.
ann_code = rns_code[rns_code.index("static void rns_announce_poll") :
                    rns_code.index("static void skill_rns_poll")]
assert "g_rns_ann_edge_pending" in ann_code
assert ann_code.index("rns_announce_edge_latch(") < ann_code.index("rns_announce_due("), (
    "latch first, decide second — the decision reads the flag the latch sets"
)
# The decision must be handed the LATCH, never the raw sample. Whitespace is
# normalised so a reflow cannot break this without changing the call.
ann_norm = re.sub(r"\s+", " ", ann_code)
assert ("rns_announce_due(now, g_rns_ann_last_ms, g_rns_announced, online, "
        "g_rns_ann_edge_pending, g_rns_up_ms)") in ann_norm, (
    "rns_announce_due() must receive g_rns_ann_edge_pending as its edge "
    "argument; passing g_rns_ann_was_online is the defect this replaced"
)
# Cleared in exactly one place, and that place is the branch that announced.
# The declaration initialises it to false too, so match assignments only.
clears = re.findall(r"(?<!bool )g_rns_ann_edge_pending = false;", rns_code)
assert len(clears) == 1, (
    "the latch may be cleared in exactly one place: after an announce fires. "
    "Clearing it on a refusal, on the link going down, or on a timer turns the "
    "floor back into a filter"
)
assert ann_code.index("rns_announce_due(") < ann_code.index(
    "g_rns_ann_edge_pending = false;"), (
    "the clear belongs inside the branch that announced, after the decision"
)
# ...and that pair is NOT enough on its own, which is why the next assertion
# exists. "Exactly one clear, textually after the decision" is satisfied by any
# position later in the function, INCLUDING outside the if. Two edits that keep
# one clear site and put it after rns_announce_due() — moving it to the end of
# the function, and guarding it with `if (online)` — both reintroduce the exact
# 29.8-minute silent outage this section was written to prevent, and both passed
# the full suite green. tools/test_rns_annsched.sh cannot see them either: its
# section 8 drives its own step() replica, not this function.
#
# So pin the three stamps as ONE UNIT. They are the branch body's opening, they
# only ever run together, and an edit as ordinary as tidying them out of a long
# if body has to fail here first.
assert ("g_rns_ann_last_ms = now; g_rns_announced = true; "
        "g_rns_ann_edge_pending = false;") in ann_norm, (
    "the three stamps are one unit inside the branch that announced; a clear "
    "anywhere else turns the floor back into a filter"
)
# The host test must keep driving BOTH functions in the caller's order — a
# per-call test of a correct header cannot see a seam defect, which is exactly
# how this one shipped.
annsched_test = (ROOT / "tools" / "test_rns_annsched.cpp").read_text(encoding="utf-8")
assert "rns_announce_edge_latch(" in annsched_test and \
       "rns_announce_due(" in annsched_test, (
    "the schedule test must drive the latch and the decision together, not the "
    "decision alone"
)
# The floor is only meaningful relative to the reconnect ladder it defends
# against, so pin that the ladder is still the thing it was sized for.
assert "#define RNS_TCP_BACKOFF_MIN_MS 3000UL" in rns
assert "g_rns_backoff_ms = RNS_TCP_BACKOFF_MIN_MS;" in loop_body, (
    "the backoff reset on a successful connect is what makes the flap period "
    "constant; if this moves, resize RNS_ANNOUNCE_RECONNECT_MIN_MS"
)

# 8g. The announce itself: gated on the schedule, timed, and never a bare call.
ann = rns[rns.index("static void rns_announce_poll") :
          rns.index("static void skill_rns_poll")]
assert "rns_announce_due(" in ann, (
    "the announce must be gated on the tested schedule, not on an inline "
    "comparison written here"
)
assert rns.count("rns_destination.announce(") == 1, (
    "there is exactly one announce call site, and it is the scheduled one"
)
assert "micros()" in ann, (
    "each announce must be timed: it signs with software Ed25519 on the loop "
    "task and that cost is the point of the measurement"
)
assert "g_rns_ann_last_ms = now;" in ann and "g_rns_announced = true;" in ann, (
    "the schedule must be stamped before the call, so an announce that throws "
    "backs off to the interval instead of retrying every tick"
)
assert ann.index("g_rns_announced = true;") < ann.index("rns_destination.announce("), (
    "the stamp must precede the call for that backoff to hold"
)
assert "catch (...)" in ann, (
    "announce() reaches Transport::outbound, Identity::sign and our own "
    "send_outgoing; a throw must not escape the skill tick"
)
# Nothing that blocks on the UART may sit inside the timed window: a
# Serial.printf() is milliseconds at 115200 baud and would land inside a number
# documented as the Ed25519 sign cost. ann_code (comment-stripped) is defined
# in 8f-bis above.
assert "Serial." not in ann_code[: ann_code.index("micros() - t0")], (
    "the failure report must come after the clock stops, or announce_us_max "
    "becomes a measurement of the console"
)
assert "g_rns_ann_was_online = online;" in ann, (
    "the online edge must be stamped every tick, or the offline->online "
    "re-announce becomes a permanent state instead of a one-tick event"
)
# The announce runs after the stack has, so it reads this tick's link state.
tick = rns[rns.index("static void skill_rns_poll") :]
assert "rns_announce_poll();" in tick
assert tick.index("rns_stack.loop();") < tick.index("rns_announce_poll();"), (
    "the announce must run after Reticulum::loop(), which is what updates the "
    "interface's online flag"
)

# 8h. The address and the measurement reach GET /rns/status. `hash` is the
# IDENTITY hash and stays that; `address` is the DESTINATION hash and is a
# different value. Nothing may quietly repoint the older key.
assert 'doc["hash"] = rns_hexhash;' in json_builder, (
    "`hash` must keep meaning the identity hash; the destination hash is a "
    "separate key"
)
for key in ('"address"', '"announced"', '"announces_sent"', '"announce_us_last"',
            '"announce_us_max"', '"data_rx"'):
    assert key in json_builder, (
        "GET /rns/status must publish %s — the address and its announce cost "
        "are the point of the destination" % key
    )

# --- 9. rns.cpp: the deferral that turns a packet into a card ----------------
# The callback may not raise the card: notify_ingest() calls time(NULL), takes
# the notify_mux critical section, appends to the event ring and write-throughs
# the card to the off-loop history archive, and it would do all of it on the
# loop task inside the 8 ms drain with Transport::_jobs_locked set. So the
# callback hands off and the loop task consumes — the same deferral cluster as
# g_rns_cfg_dirty above and as skills/wg.cpp's request flags. This section pins
# the handoff, its rules and its position in the tick.
inbox = (ROOT / "src" / "rns" / "inbox.h").read_text(encoding="utf-8")
assert '#include "rns/inbox.h"' in rns

# 9a. The rules live in a pure header, like pktfilter.h and annsched.h, so the
# host test compiles the real code rather than a copy of it.
inbox_code = re.sub(r"/\*.*?\*/", " ", inbox, flags=re.S)
inbox_code = re.sub(r"//[^\n]*", " ", inbox_code)
assert not re.search(r"^\s*static\s+(?!inline\b)", inbox_code, re.M), (
    "rns/inbox.h must hold no state of its own; the firmware owns the storage "
    "and passes it in, which is what lets the host test drive the real rules"
)
for banned in ("millis(", "micros(", "esp_timer", "printf", "new ", "malloc",
               "Serial", "time("):
    assert banned not in inbox_code, (
        "rns/inbox.h must not reach for %s: half of it runs inside the drain, "
        "and all of it has to compile on the host" % banned
    )
assert "#define RNS_INBOX_PAYLOAD_MAX 383" in inbox_code, (
    "the buffer is sized once at the 383-byte ceiling — "
    "floor((464-48-32)/16)*16-1, the largest plaintext one encrypted packet to "
    "a SINGLE destination carries"
)
# The ring is deep enough for ONE WHOLE DRAIN PASS. The drain admits
# RNS_TCP_DRAIN_FRAMES_MAX frames in a tick and the pickup runs once per tick, so
# a shallower ring answers an ordinary burst — the pieces of a message too long
# for one packet, say — with drops, by construction. The relationship is pinned
# in the firmware itself so it cannot drift; this checks the pin exists.
slots = re.search(r"#define RNS_INBOX_SLOTS (\d+)", inbox_code)
frames = re.search(r"#define RNS_TCP_DRAIN_FRAMES_MAX (\d+)", rns_code)
assert slots and frames, "both the ring depth and the frame budget must be named"
assert int(slots.group(1)) >= int(frames.group(1)), (
    "the inbox ring (%s) is shallower than one drain pass (%s): a burst that "
    "the interface accepts in a single tick would be dropped by the inbox"
    % (slots.group(1), frames.group(1))
)
assert "static_assert(RNS_INBOX_SLOTS >= RNS_TCP_DRAIN_FRAMES_MAX" in rns_code, (
    "the ring depth must be static_asserted against the frame budget, so that "
    "growing the drain without growing the ring fails the build"
)
# PUBLISH-LAST is a contract a single-threaded host test cannot check, so it is
# pinned in the source text: the count that makes a message visible must be the
# final statement of put(). Today both sides are the loop task and the ordering
# is defensive; the day the pickup moves to another task it stops being.
put_body = inbox_code[inbox_code.index("bool rns_inbox_put(") :]
put_body = put_body[: put_body.index("\n}\n")]
put_stmts = [ln.strip() for ln in put_body.splitlines() if ln.strip()]
assert put_stmts[-1] == "return true;" and put_stmts[-2] == "bx->count++;", (
    "rns_inbox_put() must publish the message LAST — `bx->count++;` immediately "
    "before the return, after every byte is in place. Found: %r" % put_stmts[-2:]
)
assert "RNS_INBOX_PAYLOAD_MAX == (size_t)RNS::Type::Packet::ENCRYPTED_MDU" in rns, (
    "the buffer size must be static_asserted against the library's own value; "
    "the host test cannot see the library, so this is the only place a drift "
    "can be caught, and it must break the build rather than the device"
)

# 9b. Fixed storage, sized once, at file scope. The receive path allocates
# nothing of its own — see the file header for what the library allocates
# underneath it, which is a different and unavoidable thing.
assert "static rns_inbox g_rns_inbox;" in rns, (
    "the firmware owns exactly one inbox, at file scope"
)
assert re.search(r"^static uint8_t g_rns_inbox_payload\[RNS_INBOX_PAYLOAD_MAX\];",
                 rns, re.M), (
    "the pickup scratch must be a fixed static too: the loop task's stack is "
    "shared with the drain and with Transport::inbound()"
)
assert re.search(r"^static char g_rns_card_body\[NOTIFY_BODY_LEN\];", rns, re.M), (
    "the card body buffer must be sized from notify's own limit"
)

# 9c. Exactly one producer and exactly one consumer, and each is where it
# belongs. Comments are stripped first: the prose names both functions.
assert rns_code.count("rns_inbox_put(") == 1, (
    "rns_inbox_put() belongs in the packet callback and nowhere else"
)
assert rns_code.count("rns_inbox_take(") == 1, (
    "rns_inbox_take() belongs in the loop-task pickup and nowhere else"
)
# THE SLICE IS COMMENT-STRIPPED, and that is not tidiness. The first version of
# this section tested the raw text, and the prose inside rns_inbox_poll() names
# rns_text_sanitize() — so a reviewer replaced the sanitiser call with a bare
# memcpy into g_rns_card_body and the whole suite still passed. The one guard
# between a stranger's arbitrary bytes and the card body was pinned by a comment
# about it. Every assertion below runs against code.
pickup_raw = rns[rns.index("static void rns_inbox_poll") :
                 rns.index("static void skill_rns_poll")]
pickup = re.sub(r"/\*.*?\*/", " ", pickup_raw, flags=re.S)
pickup = re.sub(r"//[^\n]*", " ", pickup)
assert "rns_inbox_take(" in pickup and "notify_ingest(" in pickup, (
    "the pickup is what takes the messages and what raises the cards"
)
assert rns_code.count("notify_ingest(") == 1, (
    "there is exactly one card site, and it is on the loop task outside the "
    "drain — a notify_ingest() reachable from the callback would take the "
    "notify_mux critical section inside Transport::inbound()"
)
for banned in ("new ", "malloc", "String "):
    assert banned not in pickup, (
        "the pickup runs every tick on the loop task; %s does not belong in it"
        % banned
    )
# It DRAINS the ring rather than taking one message per tick, or the ring would
# only postpone the drops it exists to prevent.
assert re.search(r"while \(rns_inbox_take\(", pickup), (
    "the pickup must drain until the ring is empty: one drain pass can admit "
    "RNS_TCP_DRAIN_FRAMES_MAX frames and the pickup runs once per tick"
)

# 9d. The payload is sanitised before it reaches the screen. notify_ingest()
# filters nothing, the body is stored as a C string, and a peer can send any 383
# bytes it likes — a NUL in the middle would end the card silently and a stray
# 0xFF is not UTF-8 at all. The card body may be written by nothing else.
assert "rns_text_sanitize(" in pickup, (
    "an arbitrary payload must be sanitised before it becomes a card body"
)
assert "rns_text_sanitize" in inbox_code, (
    "the sanitiser lives in the pure header so the host test can drive it"
)
# NOTHING may write the card body except the sanitiser and the marker. A raw
# copy of the payload into it is the mutation this section was rewritten for:
# it puts a stranger's unfiltered bytes on the screen, and every other assertion
# here would still pass.
pickup_norm = re.sub(r"\s+", " ", pickup)
writes = re.findall(r"\b(?:memcpy|memmove|strcpy|strncpy|snprintf|memset)\s*"
                    r"\(\s*g_rns_card_body[^,]*,\s*([A-Za-z_][A-Za-z0-9_]*)",
                    pickup_norm)
assert set(writes) <= {"mark", "g_rns_card_body"}, (
    "the card body may only be written by rns_text_sanitize(), by the marker "
    "prefix (`mark`) and by the memmove that closes the gap in front of it. "
    "Found a copy from %r — a raw copy of the payload puts unfiltered bytes on "
    "the screen" % sorted(set(writes) - {"mark", "g_rns_card_body"})
)
assert "rns_text_sanitize(g_rns_inbox_payload," in pickup_norm, (
    "the sanitiser must be fed the payload the pickup just took, not something "
    "assembled around it"
)
# ...AND ITS OUTPUT MUST BE THE THING THAT GOES ON THE CARD. Feeding
# g_rns_inbox_payload straight to notify_ingest() calls the sanitiser and throws
# the result away: unsanitised control bytes reach the renderer, and the payload
# is NOT NUL-terminated, so notify_copy_text()'s snprintf("%s") reads past the
# end of a 383-byte buffer. Every other assertion in this section passes with
# that edit in place, which is why the argument itself is pinned.
ingest = re.search(r"notify_ingest\(([^;]*)\)", pickup_norm)
assert ingest, "the pickup must raise the card through notify_ingest()"
ingest_args = [a.strip() for a in ingest.group(1).split(",")]
assert len(ingest_args) == 5 and ingest_args[3] == "g_rns_card_body", (
    "the card body argument must be g_rns_card_body — the sanitiser's output — "
    "and never g_rns_inbox_payload, which is raw, arbitrary and not "
    "NUL-terminated. Found: %r" % ingest_args
)
assert "g_rns_inbox_payload" not in ingest.group(1), (
    "the raw payload may not reach notify_ingest() in any argument"
)

# 9d-bis. THE CUT IS SIZED AGAINST WHAT THE SCREEN PAINTS, not against notify's
# storage. hw_ui_show_notify() draws at most max_rows rows of max_cols columns
# and then stops — there is no scroll on that card — so a body cut at notify's
# 240-byte limit puts its "+N bytes" marker at an offset the renderer never
# reaches, which is exactly the bug this pins. The budget is in CODEPOINTS
# because the renderer counts codepoints, not bytes.
hw_ui = (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")
assert "RNS_CARD_VISIBLE_CHARS" in pickup, (
    "the cut must be sized against the renderer's character budget"
)
assert re.search(r"#define RNS_CARD_ROWS 5\b", rns) and \
       re.search(r"#define RNS_CARD_COLS 37\b", rns), (
    "the card geometry must be stated as constants that can be checked against "
    "the renderer"
)
# ...and those two numbers are only true while the renderer's own constants are.
# If any of these moves, RNS_CARD_ROWS/RNS_CARD_COLS has to be recomputed.
notify_render = hw_ui[hw_ui.index("void hw_ui_show_notify(") :]
notify_render = notify_render[: notify_render.index("\nvoid hw_ui_show_agent_invite")]
assert "const int max_rows = 5;" in notify_render, (
    "hw_ui_show_notify() no longer paints 5 body rows; RNS_CARD_ROWS in rns.cpp "
    "was sized against it"
)
assert "const uint8_t body_scale = 2;" in notify_render, (
    "the card body scale decides the column count RNS_CARD_COLS was sized from"
)
assert "const int max_cols = (int)((PANEL_W - MARGIN - 4 - MARGIN) / body_adv);" \
    in notify_render, (
    "the card's column arithmetic changed; recompute RNS_CARD_COLS in rns.cpp"
)
assert "static const int MARGIN   = 12;" in hw_ui, (
    "MARGIN feeds the column count RNS_CARD_COLS was sized from"
)
# There is no scroll on the notify card, which is why text past the budget is
# lost rather than merely off-screen. If that ever changes, the cut can relax.
assert "scroll" not in notify_render, (
    "the notify card gained a scroll: text past RNS_CARD_VISIBLE_CHARS is no "
    "longer unreachable, so the cut can be sized against notify's storage again"
)

# A truncated card must say so, and the marker must be somewhere the renderer
# actually paints. Word wrap decides how much of the LAST row is used, so the
# marker goes in FRONT, where row one is always drawn.
assert "g_rns_card_cut++" in pickup, (
    "a card that could not hold the whole message must be counted"
)
mark = re.search(r'snprintf\(mark, sizeof\(mark\), "([^"]*)"', pickup)
assert mark and mark.group(1).startswith("[+"), (
    "the truncation marker must be a PREFIX: a suffix can be pushed off the "
    "bottom of the card by word wrap alone, which is how the previous version "
    "shipped a marker nobody could see. Found: %r"
    % (mark.group(1) if mark else None)
)
assert pickup.count("rns_text_sanitize(") == 2, (
    "the count on the marker must come from a SECOND sanitising pass with the "
    "marker's room already held back — computing it before the marker displaces "
    "text undercounts by exactly the marker's length"
)
# COUNTING THE CALLS IS NOT ENOUGH: the bug was the BUDGET, not the arity.
# Reverting the first pass to sizeof(g_rns_card_body) restores the original
# defect for every payload between the card's character budget and notify's
# 240-byte storage, with two calls still on the page. So both budgets are pinned:
# the first decides WHETHER anything was cut, the second holds back the marker's
# room so the count is of text that genuinely did not make it.
budgets = re.findall(r"rns_text_sanitize\((.*?)\);", pickup_norm)
assert len(budgets) == 2, "expected two sanitising passes, found %d" % len(budgets)
first, second = (re.sub(r"\s+", "", b) for b in budgets)
assert first.endswith("sizeof(g_rns_card_body),RNS_CARD_VISIBLE_CHARS"), (
    "the first pass must measure against the CARD's character budget, not "
    "notify's storage: sizing the cut at 240 bytes is the bug that put the "
    "marker where the renderer never reaches. Found: %r" % first
)
assert second.endswith(
    "sizeof(g_rns_card_body)-RNS_CARD_CUT_RESERVE,"
    "RNS_CARD_VISIBLE_CHARS-RNS_CARD_CUT_RESERVE"), (
    "the second pass must hold the marker's room back in BOTH budgets, or the "
    "count is measured against text the marker then overwrites. Found: %r"
    % second
)
# The guard is bounded by the RESERVATION, not by sizeof(mark). They differ, and
# the larger one is unsafe: the memmove below moves the text to offset mn, so any
# mn past the reservation moves it UP and past the end of the body. Bounding by
# sizeof(mark) leaves that three characters from a constant whose comment invites
# tuning — ASAN reaches it by lowering the reservation alone.
assert "mn <= RNS_CARD_CUT_RESERVE" in pickup, (
    "the marker length must be bounded by RNS_CARD_CUT_RESERVE (the room the "
    "memmove assumes), not by sizeof(mark)"
)
assert "sizeof(mark)" not in pickup.split("if (mn")[1].split("}")[0], (
    "sizeof(mark) is not the safe bound for the marker guard"
)

# 9e. THE PICKUP RUNS AFTER THE STACK. The drain that fills the inbox is inside
# rns_stack.loop(), so a pickup ahead of it serves last tick's message and turns
# every delivery into an extra RNS_TICK_MS of latency — and makes the overflow
# policy fire on message pairs a prompt pickup would have kept apart.
assert "rns_inbox_poll();" in tick, "the tick must drive the pickup"
assert tick.index("rns_stack.loop();") < tick.index("rns_inbox_poll();"), (
    "the inbox pickup must run AFTER Reticulum::loop(), which is what runs the "
    "drain that fills it"
)

# 9f. The receive path is diagnosable from GET /rns/status: how many arrived,
# how many were dropped by the overflow policy, how big the last one was.
# A silently lost message is the one outcome that is not acceptable.
for key in ('"data_dropped"', '"data_last_len"', '"data_oversize"',
            '"data_cards"', '"data_card_cut"'):
    assert key in json_builder, (
        "GET /rns/status must publish %s: the receive path drops messages by "
        "policy, and a drop nobody can see is a drop nobody can fix" % key
    )
assert 'doc["data_rx"] = (unsigned long)g_rns_inbox.received;' in json_builder, (
    "data_rx keeps its meaning — payloads the callback was handed — and now "
    "comes from the inbox's own counter"
)

# 9g. The host test drives the real header, including the overflow policy: the
# first message survives, the refusal is counted, and neither is a comment.
inbox_test = (ROOT / "tools" / "test_rns_inbox.cpp").read_text(encoding="utf-8")
for call in ("rns_inbox_put(", "rns_inbox_take(", "rns_text_sanitize("):
    assert call in inbox_test, (
        "tools/test_rns_inbox.cpp must drive %s — the point of the pure header "
        "is that the test runs the shipping code" % call
    )
assert ".dropped ==" in inbox_test, (
    "the overflow policy is only a policy if the test pins the counter: keep "
    "the oldest, COUNT the drop"
)
assert "RNS_INBOX_PAYLOAD_MAX + 1" in inbox_test, (
    "one byte over the ceiling must be exercised"
)
# The two guards that a well-behaved test misses unless it is written for them:
# the output boundary (a one-byte write past the caller's buffer) and the
# multi-byte length check (a read past the end of the payload). Both were live
# mutations that survived the first version of that file.
assert "canary" in inbox_test, (
    "the out_cap boundary needs a canary after the destination: `>=` relaxed to "
    "`>` writes the terminator one byte past it and nothing else notices"
)
assert re.search(r"rns_text_sanitize\(\s*truncated,\s*1,", inbox_test), (
    "the read-past-end guard needs a payload whose LENGTH is shorter than the "
    "sequence its lead byte announces, with real continuation bytes behind it — "
    "a lead at the end of an array is decided by whatever follows the array"
)
# And the invariant the "+N bytes" count rests on: bytes written == input bytes
# consumed, on every branch.
assert "== 1);" in inbox_test and "for (int b = 0; b < 256; b++)" in inbox_test, (
    "the length-preserving invariant must be pinned over a corpus that reaches "
    "every branch of the mapping, or an expanding escape would silently corrupt "
    "the count on the card"
)

print("Task unblock policy tests: OK")
