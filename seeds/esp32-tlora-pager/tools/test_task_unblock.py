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
  - That pickup now READS THE ENVELOPE this same firmware emits, using the
    parser in rns/outbox.h rather than a second copy of the rules, so the card
    shows the message instead of "1|<32 hex>|<session>|<text>". The parse is
    SOFT: bare text and anything that does not match the shape is shown exactly
    as it arrived and counted, because senders predating the format exist and a
    message is never worth dropping over its shape. The sanitiser pin is
    therefore no longer a literal buffer name — it is the rule that every
    sanitising pass reads from one variable and that variable comes from
    nothing but the raw payload and the parsed text field. A parsed envelope is
    also offered ONCE to a room router: a function pointer, null by default,
    because the skill that owns the conversations has not landed yet and a
    direct call to a symbol nobody has written is a build break dressed up as a
    design. The card is raised first whatever the router does, and a refusal is
    counted with its reason.
  - And rns.cpp now SENDS, which is the first handoff in this firmware that
    genuinely crosses tasks: POST /rns/send answers on the AsyncTCP task and the
    send must run on the loop task. So the ring's publish-last discipline stops
    being defensive and becomes load-bearing — the indices are atomics with one
    writer each and the stores are release/acquire — and the handler is pinned
    as an ordered allowlist that reaches Transport nowhere. The send itself is
    pinned in the order that keeps it from vanishing: with no path,
    Transport::outbound() broadcasts a packet the hub drops and receipt_send()
    still returns a receipt, so has_path() and recall() must both answer before
    anything is sent, recall() is paid for at most once per message, the path is
    requested once, and a ladder that runs out reports a REASON rather than a
    silence.
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
          rns.index('exact("/rns/send")')]
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
# both. This is the seed.pager destination and stays that — the LXMF delivery
# destination is a SEPARATE address with its own app_name/aspects, pinned in
# section 8i below; seed.pager must never be renamed to it.
assert '#define RNS_DEST_APP_NAME "seed"' in rns
assert '#define RNS_DEST_ASPECTS  "pager"' in rns
assert "RNS_DEST_APP_NAME" in dest and "RNS_DEST_ASPECTS" in dest, (
    "the seed.pager destination must be built from its own app_name/aspects "
    "macros, never the lxmf ones"
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

# 8i. THE LXMF DELIVERY DESTINATION. A SECOND destination of ours, and therefore
# a SECOND packet callback on the loop task inside the drain, reached the same
# way as seed.pager's. It mirrors 8a-8h deliberately: same budget, same failure
# modes, same "the callback only copies, the loop task does the work" deferral.
#
# 8i.1 Registered with its own callback, on its own address. app_name "lxmf" and
# aspects "delivery" — a SEPARATE hash from seed.pager, so register_destination()
# cannot collide.
assert '#define RNS_LXMF_APP_NAME "lxmf"' in rns and \
       '#define RNS_LXMF_ASPECTS  "delivery"' in rns, (
    "the LXMF delivery address is its own app_name/aspects pair"
)
lxmf_dest = rns[rns.index("rns_lxmf_destination = RNS::Destination(") :
                rns.index("rns_lxmf_dest_ok = true;")]
assert "rns_identity," in lxmf_dest, (
    "the lxmf destination must be built on the loaded identity too; a NONE "
    "identity mints a throwaway keypair and the address moves every boot"
)
assert "RNS::Type::Destination::IN" in lxmf_dest and \
       "RNS::Type::Destination::SINGLE" in lxmf_dest, (
    "lxmf.delivery is an IN/SINGLE destination, like seed.pager"
)
assert "RNS_LXMF_APP_NAME" in lxmf_dest and "RNS_LXMF_ASPECTS" in lxmf_dest, (
    "it must be built from its own app_name/aspects, not seed.pager's"
)
assert "rns_lxmf_destination.set_packet_callback(rns_lxmf_data_callback);" in rns, (
    "the lxmf destination must register its own packet callback; without one an "
    "inbound LXMF packet is decrypted and dropped without a trace"
)
assert "rns_lxmf_destination.accepts_links(false);" in rns, (
    "accepts_links defaults to TRUE; an inbound link request would run an X25519 "
    "handshake and an Ed25519 sign inside the drain — decline it here too"
)
# The proof strategy stays PROVE_NONE for lxmf too, and against the CODE
# (comment-stripped): PROVE_APP would run Identity::prove() — an Ed25519 sign —
# inside Transport::inbound() on the loop task, in the 8 ms drain. The prose at
# the call site explains this; describing what must not be written may not fail
# the test that forbids writing it. (PROVE_ALL is already forbidden file-wide in
# 8d; set_proof_strategy() is what would opt into either.)
assert "set_proof_strategy(" not in rns_code, (
    "the lxmf destination must NOT set a proof strategy: this library proves "
    "synchronously in the drain, so PROVE_APP/PROVE_ALL would sign an Ed25519 "
    "proof there per recognised packet — a delivery receipt waits for an "
    "off-loop prove a later commit adds"
)

# 8i.2 A bare function pointer, not a lambda, like rns_data_callback.
assert re.search(r"^static void rns_lxmf_data_callback\(const RNS::Bytes ",
                 rns, re.M), (
    "the lxmf callback must be a file-scope function matching "
    "void(*)(const Bytes&, const Packet&)"
)

# 8i.3 The callback body, pinned statement by statement — an ALLOWLIST, exactly
# like 8c. It runs once per inbound LXMF packet inside the drain, so it may only
# copy into its own ring: no parse (that is tens of ms and lives in the loop-task
# poll), no log, no allocation, and the Bytes argument stays a reference.
lcb = rns[rns.index("static void rns_lxmf_data_callback") :]
lcb = lcb[: lcb.index("\n}\n") + 2]
lcb = re.sub(r"/\*.*?\*/", " ", lcb, flags=re.S)
lcb = re.sub(r"//[^\n]*", " ", lcb)
lcb_body = [re.sub(r"\s+", " ", line).strip() for line in lcb.splitlines()]
lcb_body = [line for line in lcb_body if line]
LCB_ALLOWED = [
    "static void rns_lxmf_data_callback(const RNS::Bytes &data, const RNS::Packet &packet) {",
    "(void)packet;",
    "rns_inbox_put(&g_rns_lxmf_inbox, data.data(), data.size());",
    "}",
]
assert lcb_body == LCB_ALLOWED, (
    "the lxmf packet callback body is pinned statement for statement; the parse "
    "must NOT move into it (it is tens of ms in the drain).\n  expected: %r\n"
    "  found:    %r" % (LCB_ALLOWED, lcb_body)
)

# 8i.4 Its ring and buffers are fixed statics at file scope, like the seed.pager
# inbox, and the parsed message is a static too — sizeof(LxmfMsg) is ~1 KB and
# the loop task's stack is shared with the drain.
assert "static rns_inbox g_rns_lxmf_inbox;" in rns, (
    "the lxmf ring is a fixed static, like g_rns_inbox"
)
assert re.search(r"^static uint8_t g_rns_lxmf_payload\[RNS_INBOX_PAYLOAD_MAX\];",
                 rns, re.M), (
    "the lxmf pickup scratch must be a fixed static of the same ceiling"
)
assert "static LxmfMsg g_rns_lxmf_msg;" in rns, (
    "the parsed message must be a file-scope static, not a ~1 KB local on the "
    "loop task's shared stack"
)

# 8i.5 The parse runs OFF the drain, after the stack, and is the tens-of-ms work
# the callback was forbidden. lxmf_parse() is called from exactly one place, the
# loop-task poll, never the callback.
assert '#include "../lxmf_codec.h"' in rns, (
    "rns.cpp must include the C1 codec header that owns the parse"
)
assert rns_code.count("lxmf_parse(") == 1, (
    "there is exactly one parse site, in the loop-task poll — never the callback "
    "and never the drain"
)
lxmf_poll = rns[rns.index("static void rns_lxmf_inbox_poll") :]
lxmf_poll = lxmf_poll[: lxmf_poll.index("\n}\n") + 2]
# TLORA-LXMF C5: the wire is OPPORTUNISTIC (LXMessage.py:633-634) — the sender
# strips the 16-byte destination hash and the RNS packet addressing carries it,
# so the poll first RECONSTRUCTS the full-packed message by prepending our own
# lxmf.delivery hash (the true dest the sender signed against, LXMRouter.py:
# 1930-1934) into a 16-byte-headroom scratch, THEN parses that. The parse target
# is g_rns_lxmf_recon, not the raw g_rns_lxmf_payload.
assert "lxmf_wire_prepend_dest(rns_lxmf_dest_hash, g_rns_lxmf_payload, len," \
       in lxmf_poll, (
    "the poll must prepend our real lxmf.delivery hash to the opportunistic wire "
    "payload before parsing — real clients (Retichat/Sideband) omit the dest hash"
)
assert "lxmf_parse(g_rns_lxmf_recon, rlen, &g_rns_lxmf_msg)" in lxmf_poll, (
    "the poll parses the RECONSTRUCTED full-packed buffer (dest prepended), not "
    "the raw wire payload it took out of its ring"
)
assert "lxmf_parse(" not in lcb, (
    "the parse must never appear in the callback: it is tens of ms in the drain"
)
# The poll drains the ring, counts OK vs bad, and (TLORA-LXMF C3) ROUTES the
# clean ones onto the screen; both counters must still move from it.
assert "g_rns_lxmf_ok++" in lxmf_poll and "g_rns_lxmf_bad++" in lxmf_poll, (
    "the poll must bump data_lxmf_ok on a clean parse and data_lxmf_bad on a "
    "malformed one — the disjoint 'device alive but did not understand' signal"
)
# 8i.5-bis THE RECEIVE MILESTONE: a clean parse is ROUTED, not just counted. The
# card-vs-room decision is the pure lxmf_route_plan() (src/lxmf_route.h), and the
# poll raises a card through notify_ingest() or hands a thread to the room router,
# with the same two off-loop calls the seed.pager pickup uses. Still after the
# stack, still off the drain.
assert "lxmf_route_plan(&g_rns_lxmf_msg" in lxmf_poll, (
    "the clean-parse path must call the pure router lxmf_route_plan() — the "
    "card-vs-room / severity / key decision is factored out and host-tested"
)
assert "notify_ingest(" in lxmf_poll, (
    "the DEFAULT (a plain client, no custom fields) must raise a card — without "
    "this the phone writes and nothing lands on the screen"
)
assert "g_rns_room_router(" in lxmf_poll, (
    "a message carrying a thread must route to a room INSTEAD of a card"
)
assert '#include "../lxmf_route.h"' in rns, (
    "rns.cpp must include the pure routing header that owns the decision"
)
# The text that reaches the screen is sanitised the seed.pager way — control
# bytes stripped, length bounded — never the raw codec buffer straight to notify.
assert "rns_text_sanitize(" in lxmf_poll and \
       "g_rns_card_body" in lxmf_poll and "g_rns_room_text" in lxmf_poll, (
    "title/content must go through rns_text_sanitize() into the shared card/room "
    "buffers, exactly as the seed.pager pickup does"
)
# It runs on the tick, after the stack, right after the seed.pager pickup.
assert "rns_lxmf_inbox_poll();" in tick, "the tick must drive the lxmf poll"
assert tick.index("rns_stack.loop();") < tick.index("rns_lxmf_inbox_poll();"), (
    "the lxmf poll must run AFTER Reticulum::loop(), which runs the drain that "
    "fills its ring — the parse must never sit ahead of the drain"
)

# 8i.6 It is ANNOUNCED, on the same schedule as seed.pager — create is not
# reachable, so without an announce nothing can path to it. Its announce call is
# its own (rns_lxmf_destination.announce()), leaving seed.pager's single site
# from 8g intact.
assert "rns_lxmf_destination.announce();" in ann, (
    "lxmf.delivery must be announced, or no sender can path to it: create is not "
    "reachable"
)
assert ann.index("rns_destination.announce(") < ann.index(
    "rns_lxmf_destination.announce("), (
    "both announces share the seed.pager timed window, so the lxmf one follows "
    "inside the same try/catch"
)

# 8i.7 The address and the disjoint counters reach GET /rns/status.
for key in ('"lxmf_address"', '"data_lxmf_rx"', '"data_lxmf_ok"',
            '"data_lxmf_bad"'):
    assert key in json_builder, (
        "GET /rns/status must publish %s — the lxmf address and its receive "
        "counters are how C2's pipe is shown to work" % key
    )

# 8i.8 REPLYING TO AN LXMF-ORIGIN ROOM AS LXMF (TLORA-LXMF C4). The receive
# poll records who to answer; the reply path signs off the drain and reuses the
# seed.pager send ladder. This section pins the placement and the routing
# decision — the parts a host test cannot see at runtime.

# The build+sign lives in rns_send_lxmf_reply, and its EXPENSIVE crypto (the
# software SHA-256 and Ed25519 sign) must NOT be in the drain-run callback: the
# callback is a bare memcpy (pinned in 8i.3), so the sign and the pack appear
# only in the reply builder, never in the callback body.
assert "bool rns_send_lxmf_reply(" in rns, (
    "the LXMF reply builder must exist — it is the OUT half of the lxmf pipe"
)
reply_fn = rns[rns.index("bool rns_send_lxmf_reply(") :]
reply_fn = reply_fn[: reply_fn.index("\n}\n") + 2]
assert "rns_identity.sign(" in reply_fn and "Identity::full_hash(" in reply_fn, (
    "the reply must sign per the C1 sign-injection contract: full_hash then "
    "Ed25519 sign, in the builder"
)
for banned in ("rns_identity.sign(", "Identity::full_hash(", "lxmf_encode_packed("):
    assert banned not in lcb, (
        "%s must NOT appear in the lxmf packet callback — the callback runs "
        "inside the 8 ms drain and the sign/pack are tens of ms" % banned
    )
# It reuses the payload-agnostic seed.pager ring rather than a second send state
# machine, and the enqueue is under the producer spinlock like every other put.
assert "rns_outbox_put(&g_rns_outbox," in reply_fn, (
    "the reply must reuse the seed.pager outbox ring — one send ladder, one "
    "recall/resolve/encrypt path in rns_send_poll()"
)
assert "portENTER_CRITICAL(&g_rns_tx_mux);" in reply_fn, (
    "the outbox put must be under g_rns_tx_mux, the producer spinlock — the "
    "reply can be enqueued from the AsyncTCP task too"
)

# The origin map is SET when a message routes into a room and CLEARED when a
# seed.pager envelope lands in one, both keyed by the resolved (sanitised) room
# name so the reply lookup finds the same slot, both under the tx spinlock.
assert "lxmf_origin_set(&g_rns_lxmf_origin," in lxmf_poll, (
    "the lxmf room route must record the sender so the room can answer as LXMF"
)
assert "agents_route_sanitize(route.room" in lxmf_poll, (
    "the origin key must be the sanitised room name — the same string the "
    "router stored as the session, or the reply lookup misses"
)
env_poll = rns[rns.index("static void rns_inbox_poll") :]
env_poll = env_poll[: env_poll.index("\n}\n") + 2]
assert "lxmf_origin_clear(&g_rns_lxmf_origin," in env_poll, (
    "a seed.pager message into a room must clear its LXMF origin — its reply "
    "reverts to the seed.pager envelope"
)

# The reply lookup + the disjoint OUT counter reach the room and the status.
assert "bool rns_lxmf_reply_target(" in rns, (
    "the room asks rns_lxmf_reply_target() whether it owes an LXMF reply"
)
assert '"data_lxmf_tx"' in json_builder, (
    "GET /rns/status must publish data_lxmf_tx, the OUT mirror of data_lxmf_rooms"
)

# skills/agents.cpp gates the LXMF reply on `claude` and takes it BEFORE the
# seed.pager uplink; a non-LXMF room falls through to the unchanged old path.
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
send_fn = agents[agents.index("static bool agents_send(") :]
send_fn = send_fn[: send_fn.index("\n/* Inject an agent reply")]
assert "rns_lxmf_reply_target(session, lxmf_dest)" in send_fn, (
    "agents_send must ask whether this room owes an LXMF reply"
)
assert send_fn.index("rns_lxmf_reply_target(") < send_fn.index(
    "agents_rns_uplink("), (
    "the LXMF-origin reply must be decided BEFORE the seed.pager uplink, or an "
    "LXMF room would answer the wrong (configured) peer"
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
assert rns_code.count("rns_inbox_put(") == 2, (
    "rns_inbox_put() belongs in the two packet callbacks and nowhere else — "
    "seed.pager's (g_rns_inbox) and lxmf.delivery's (g_rns_lxmf_inbox)"
)
assert rns_code.count("rns_inbox_take(") == 2, (
    "rns_inbox_take() belongs in the two loop-task pickups and nowhere else — "
    "the seed.pager card pickup and the lxmf.delivery parse poll"
)
# THE SLICE IS COMMENT-STRIPPED, and that is not tidiness. The first version of
# this section tested the raw text, and the prose inside rns_inbox_poll() names
# rns_text_sanitize() — so a reviewer replaced the sanitiser call with a bare
# memcpy into g_rns_card_body and the whole suite still passed. The one guard
# between a stranger's arbitrary bytes and the card body was pinned by a comment
# about it. Every assertion below runs against code.
pickup_raw = rns[rns.index("static void rns_inbox_poll") :
                 rns.index("static void rns_send_retire")]
pickup = re.sub(r"/\*.*?\*/", " ", pickup_raw, flags=re.S)
pickup = re.sub(r"//[^\n]*", " ", pickup)
assert "rns_inbox_take(" in pickup and "notify_ingest(" in pickup, (
    "the pickup is what takes the messages and what raises the cards"
)
assert rns_code.count("notify_ingest(") == 2, (
    "there are exactly two card sites — the seed.pager pickup and the LXMF poll "
    "(TLORA-LXMF C3) — and BOTH are on the loop task outside the drain: a "
    "notify_ingest() reachable from either callback would take the notify_mux "
    "critical section inside Transport::inbound()"
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
# THE SANITISER MUST BE FED THE PAYLOAD THE PICKUP JUST TOOK — and since the
# envelope arrived, "the payload" is one of TWO THINGS: the whole buffer for
# anything that is not an envelope, and the envelope's TEXT FIELD for anything
# that is. Both are bytes off the network, so both have to go through the
# sanitiser, and the earlier form of this pin (the literal
# `rns_text_sanitize(g_rns_inbox_payload,`) can no longer say that: it would
# either forbid the parse or pass while the text field went round it.
#
# So the pin is in two halves. First, every sanitiser call in the pickup reads
# from the SAME variable; second, that variable is assigned from nothing but the
# raw payload and the parsed view. Bypassing the sanitiser on either path — or
# introducing a third source, a decoded buffer, an unparsed remainder — fails
# here.
src_args = [b.split(",")[0].strip()
            for b in re.findall(r"rns_text_sanitize\((.*?)\);", pickup_norm)]
assert src_args and len(set(src_args)) == 1, (
    "every sanitising pass in the pickup must read from one and the same "
    "source variable; found %r" % sorted(set(src_args))
)
SRC = src_args[0]
assert re.search(r"const uint8_t \*%s = g_rns_inbox_payload;" % SRC, pickup), (
    "the sanitiser's source must START as the raw payload the pickup took, so "
    "that a payload which is not an envelope is shown exactly as it arrived"
)
src_sets = re.findall(r"(?<![A-Za-z0-9_])%s\s*=\s*([^;]+);" % SRC, pickup)
assert [re.sub(r"\s+", " ", s).strip() for s in src_sets] == [
    "g_rns_inbox_payload", "(const uint8_t *)ev.text"], (
    "the sanitiser's source may be assigned from exactly two things and in this "
    "order: the raw payload, and the TEXT FIELD of a parsed envelope — which "
    "points into that same payload. Anything else is a route around the "
    "sanitiser or around the parser. Found: %r" % src_sets
)
# ...and the length travels with it. A source swapped to the envelope's text
# while the length stayed the payload's would sanitise 36 bytes of somebody
# else's memory onto the card.
len_args = [b.split(",")[1].strip()
            for b in re.findall(r"rns_text_sanitize\((.*?)\);", pickup_norm)]
assert len(set(len_args)) == 1 and len_args[0] != "len", (
    "the length passed to the sanitiser must travel with the source and must "
    "not be the raw payload's `len`: an envelope contributes only its text. "
    "Found: %r" % sorted(set(len_args))
)
assert re.search(r"size_t %s = len;" % len_args[0], pickup) and \
       re.search(r"%s = ev\.text_len;" % len_args[0], pickup), (
    "the sanitised length must start as the whole payload and become the "
    "envelope's text length when one parsed"
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
assert pickup.count("rns_text_sanitize(") == 3, (
    "the count on the marker must come from a SECOND sanitising pass with the "
    "marker's room already held back — computing it before the marker displaces "
    "text undercounts by exactly the marker's length — and the room router gets "
    "a THIRD, uncut pass of its own"
)
# COUNTING THE CALLS IS NOT ENOUGH: the bug was the BUDGET, not the arity.
# Reverting the first pass to sizeof(g_rns_card_body) restores the original
# defect for every payload between the card's character budget and notify's
# 240-byte storage, with two calls still on the page. So both budgets are pinned:
# the first decides WHETHER anything was cut, the second holds back the marker's
# room so the count is of text that genuinely did not make it.
budgets = re.findall(r"rns_text_sanitize\((.*?)\);", pickup_norm)
assert len(budgets) == 3, "expected three sanitising passes, found %d" % len(budgets)
first, second, third = (re.sub(r"\s+", "", b) for b in budgets)
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
# THE THIRD PASS IS THE ROOM'S, AND IT IS UNCUT. The card's budget is what the
# screen paints; a conversation is the durable copy and gets the whole text, in
# a buffer of its own so the marker prefix and the ~185-codepoint cut cannot
# end up stored as if the sender had written them.
assert third.endswith("g_rns_room_text,sizeof(g_rns_room_text),0"), (
    "the room router's text must be sanitised into its own buffer with NO "
    "character cap (a max_chars of 0): handing it the card body would store a "
    "message cut to the screen and prefixed with a marker meant for the "
    "screen. Found: %r" % third
)
assert re.search(r"^static char g_rns_room_text\[RNS_INBOX_PAYLOAD_MAX \+ 1\];",
                 rns, re.M), (
    "the room text buffer must be a fixed static sized at the payload ceiling "
    "plus a terminator — the loop task's stack is shared with the drain"
)
# ...and NOTHING may write it but the sanitiser, for the same reason nothing
# but the sanitiser may write the card body: it is where a stranger's bytes go.
room_writes = re.findall(r"\b(?:memcpy|memmove|strcpy|strncpy|snprintf|sprintf|"
                         r"memset)\s*\(\s*g_rns_room_text", pickup_norm)
assert not room_writes, (
    "g_rns_room_text may only be written by rns_text_sanitize(): a raw copy "
    "into it hands unfiltered, un-terminated network bytes to another skill"
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

# 9d-ter. THE ENVELOPE IS READ BY THE CODE THAT WRITES IT, and the card shows
# the message rather than the framing. Until this commit the pickup sanitised
# the whole payload onto the screen, so a peer answering in the agreed format
# produced a card reading "1|<32 hex>|<session>|<text>" — the plumbing on
# display, and the text pushed off the bottom by it.
outbox_h = (ROOT / "src" / "rns" / "outbox.h").read_text(encoding="utf-8")
assert rns_code.count("rns_envelope_parse(") == 1, (
    "there is exactly one parse site, in the loop-task pickup"
)
assert "rns_envelope_parse(" in pickup and "rns_envelope_parse" in outbox_h, (
    "the parser must live in rns/outbox.h next to the builder — one owner for "
    "the format, so emit and parse cannot drift — and be called from the pickup"
)
# A SECOND PARSER IS THE FAILURE MODE THIS PINS AGAINST. The format's rules are
# the version digit, the 32-hex address, the session charset and the "first
# three separators" reading; a hand-rolled split in rns.cpp would be a copy of
# them that nothing forces to stay a copy.
for hand_rolled in ("strchr(", "strtok(", "strrchr(", "strsep(", "sscanf("):
    assert hand_rolled not in pickup, (
        "the pickup must not take the payload apart itself (%s): the envelope "
        "has one owner, rns/outbox.h, and a second reading of it drifts"
        % hand_rolled
    )
# THE PARSE IS FED THE WHOLE PAYLOAD AND ITS VERDICT IS WHAT BRANCHES. Pinned
# as statements rather than as a call somewhere in the function: a parse whose
# result nothing tests is a parse that has been switched off, and every other
# assertion in this section still passes with the envelope branch made
# unreachable.
parse_stmt = re.search(
    r"rns_envin_result (\w+) = rns_envelope_parse\("
    r"g_rns_inbox_payload, len, &(\w+)\);", pickup_norm)
assert parse_stmt, (
    "the pickup must parse the payload it just took, whole, into a view it "
    "owns — and keep the verdict"
)
ER, EV = parse_stmt.groups()
assert re.search(r"if \(%s == RNS_ENVIN_OK\) \{" % ER, pickup), (
    "the envelope branch must be guarded on the parse result itself: a verdict "
    "nothing tests is a parser that has been switched off"
)
assert re.search(r"%s\.text_len;" % EV, pickup) and \
       re.search(r"%s\.session, %s\.session_len\);" % (EV, EV), pickup), (
    "the fields must come out of the view, which points into the payload — "
    "nothing is copied out of the network buffer twice"
)
# PARSING IS SOFT. Not every payload is an envelope — tools/rns-send sends bare
# text and so does any peer predating the format — so a payload that does not
# match the shape is SHOWN AS IT ARRIVED and counted, never dropped and never
# blanked. The two counters are what makes the split visible; the else branch is
# what makes it harmless.
assert "g_rns_env_parsed++" in pickup and "g_rns_env_raw++" in pickup, (
    "both outcomes must be counted: a payload read as an envelope and a payload "
    "shown raw are both ordinary, and which one is happening decides what the "
    "screen looks like"
)
assert "g_rns_env_raw_why = rns_envin_reason(" in pickup, (
    "the reason a payload was not an envelope must be kept: the counter cannot "
    "answer 'my peer says it sent one, so why is the framing on my screen?'"
)
# ...AND A SECOND SLOT THAT PLAIN TEXT CANNOT OVERWRITE. Last-reason-wins is
# the wrong policy for the case worth seeing: tools/rns-send is in ordinary use
# and every one of its packets writes NO_FRAME, so a single probe with a bad
# address or a bad session name — the shapes the field exists to surface — is
# gone by the next ordinary message.
assert "if (er != RNS_ENVIN_NO_FRAME) g_rns_env_bad_why = g_rns_env_raw_why;" \
       in pickup, (
    "the last MALFORMED reason must be kept apart from the last reason: bare "
    "text is constant and ordinary, and it would otherwise mask every probe"
)
assert "notify_ingest(" in pickup and "return" not in pickup.split("g_rns_env_raw++")[1].split("}")[0], (
    "a payload that is not an envelope must fall through to the card, not out "
    "of the loop: the one outcome that must never happen is a lost message"
)
# THE CARD IS UNCONDITIONAL FOR EVERY MESSAGE, and it has exactly ONE exception
# — the control frame, which is not a message. That is the assertion below
# rather than something implied: the card site's only condition is `!control`
# plus notify_ingest()'s own return value. Gating it on the parse result
# (`er != RNS_ENVIN_OK && notify_ingest(...)`) leaves EVERY envelope with no
# card at all, which is precisely the outcome this file's header promises can
# never happen, and every other assertion here survives it.
assert re.search(r'if \(notify_ingest\("info", "rns", title, g_rns_card_body, '
                 r'NULL\) != 0\) g_rns_cards\+\+;', pickup_norm), (
    "the card must be raised for every message taken, conditioned on nothing "
    "but whether the notify store accepted it"
)
_body = pickup[pickup.index("{", pickup.index("while (rns_inbox_take(")):
                pickup.index("notify_ingest(")]
assert _body.count("{") - _body.count("}") == 2, (
    "the card site must sit inside exactly ONE branch of the pickup loop — the "
    "control-frame guard — and no other: a message that reaches the pickup "
    "always reaches the screen unless it is not a message"
)
# ...and that one branch is `if (!control)`, tested by its own text. Any other
# condition wrapping the card is a message that can silently fail to appear.
assert re.search(r"if \(!control\) \{", pickup), (
    "the only thing allowed to withhold a card is the control-frame guard, "
    "written as `if (!control)`"
)
assert pickup.index("if (!control) {") < pickup.index("notify_ingest("), (
    "the guard must open before the card is built, not after it is raised"
)
# THE SESSION IDENTIFIES THE CONVERSATION AND GOES IN THE TITLE; the address
# does not go on the screen at all. And the byte count stays, because it is the
# only evidence of truncation the card carries besides the marker.
title = re.findall(r'snprintf\(title, sizeof\(title\), "([^"]*)"', pickup)
assert len(title) == 2 and title[0] == "RNS %s %u B" and title[1] == "RNS %u B", (
    "the title must carry the session name when there is one and read exactly "
    "as it did before when there is not — with the byte count in both. "
    "Found: %r" % title
)
# ...AND THE %s MUST BE THE TERMINATED COPY, NEVER THE VIEW. ev.session points
# into g_rns_inbox_payload and is NOT NUL-terminated: snprintf bounds the WRITE,
# not the read, so "%s" on it walks past session_len through the rest of the
# payload and on into whatever .bss follows. The copy exists for that reason.
title_args = re.search(
    r'snprintf\(title, sizeof\(title\), "RNS %s %u B", ([^,]+),', pickup_norm)
assert title_args and title_args.group(1).strip() == "session", (
    "the title's %%s must be the terminated `session` copy, not the parser's "
    "view: ev.session has no terminator and %%s reads until it finds one. "
    "Found: %r" % (title_args.group(1) if title_args else None)
)
assert "g_rns_card_body" not in pickup.split("char title")[1].split("notify_ingest")[0], (
    "the title must be built from the session and the length, not from the body"
)
for plumbing in ("ev.from", "g_rns_env_from"):
    assert plumbing not in pickup.split("char title")[1].split("notify_ingest")[0], (
        "the sender address is carried for REPLYING, not for reading: 32 hex "
        "characters on a card are 32 characters of message the screen does not "
        "show. Found %s on the way to the card" % plumbing
    )

# 9d-quater. THE ROOM ROUTER: ONE CALL SITE, THROUGH A POINTER, ON THE LOOP
# TASK. The skill that owns the conversations decides where a message lands and
# its helper does not exist in this tree yet, so this side calls through a
# pointer rather than naming a symbol nobody has written — a direct call would
# be a build break dressed up as a design, and it would also make the two sides
# land in a fixed order.
assert "typedef bool (*rns_room_router)(const char *session, const char *text," \
       in outbox_h, (
    "the router's type is part of the contract and belongs in the header both "
    "sides include"
)
assert re.search(r"^void rns_set_room_router\(rns_room_router fn\) \{", rns, re.M), (
    "the setter must be defined in rns.cpp (which owns the pointer) and must "
    "NOT be static: the caller is another skill in this build"
)
assert re.search(r"^static rns_room_router g_rns_room_router = nullptr;", rns, re.M), (
    "the router defaults to null, and null is not a degraded mode: it is the "
    "behaviour that shipped before the hook existed — a card and nothing else"
)
assert rns_code.count("g_rns_room_router(") == 2, (
    "the router is called from exactly TWO sites, each routing a distinct "
    "receive path AT MOST ONCE per message: the seed.pager pickup (an envelope "
    "with a session) and the LXMF poll (a message with a thread, TLORA-LXMF C3). "
    "A third, or the same message through both, would put it into a conversation "
    "twice with nothing downstream to tell"
)
assert "g_rns_room_router(" in pickup, (
    "one call site is the loop-task seed.pager pickup, after rns_stack.loop() — "
    "the router's contract (no synchronous SD, no blocking) is written against "
    "that task and that position in the tick"
)
# ITS TWO ARGUMENTS ARE PINNED BY NAME, because the transformations above can
# all exist while their outputs go unused — and each wrong argument is a
# different defect:
#   ev.session / ev.text  point into g_rns_inbox_payload and are NOT
#       NUL-terminated, while rns/outbox.h promises the router two C strings.
#       A router that calls strlen() on them reads past the payload.
#   g_rns_card_body       is cut to what the screen paints and may carry the
#       "[+N B]" marker, so a conversation would store a truncated message with
#       a marker meant for the panel inside its text.
# The third sanitising pass is pinned above; this is what makes it consumed.
route_args = re.search(r"g_rns_room_router\(([^;]*?)\)", pickup_norm)
assert route_args, "the router call must be readable"
route_args = [a.strip() for a in route_args.group(1).split(",")]
assert route_args == ["session", "g_rns_room_text", "&reason"], (
    "the router takes the TERMINATED session copy and the UNCUT sanitised text "
    "— never the parser's views (no terminator; the contract promises C "
    "strings) and never the card body (cut, and prefixed with a marker meant "
    "for the screen). Found: %r" % route_args
)
# ...and only for a payload that WAS an envelope: a bare-text packet has no
# session to route by and no sender to answer.
route_guard = re.search(r"if \(er == RNS_ENVIN_OK && g_rns_room_router\)", pickup)
assert route_guard, (
    "the router must be called only for a parsed envelope AND only when it is "
    "set, in one guard: a null pointer is the default and a non-envelope has "
    "nothing to route"
)
# THE CARD IS RAISED FIRST, WHATEVER THE ROUTER DOES. A router that throws its
# work away — or a bad pointer somebody set — must not be able to swallow the
# message on the way to the screen.
assert pickup.index("notify_ingest(") < pickup.index("g_rns_room_router("), (
    "the card must be raised BEFORE the router runs: the screen is the outcome "
    "that must happen whatever else does"
)
# A REFUSAL IS NOT ALLOWED TO BE SILENT. The card went up either way, so
# nothing on the screen says the conversation never received the message.
assert "g_rns_env_routed++" in pickup and "g_rns_env_refused++" in pickup and \
       "g_rns_env_refuse_reason = reason;" in pickup, (
    "both outcomes of the router must be counted and the refusal must keep the "
    "router's reason: a message that did not reach a room is invisible "
    "everywhere else"
)
for banned in ("SD.", "SPIFFS", "delay(", "vTaskDelay"):
    assert banned not in pickup, (
        "the pickup — and the router called from it — runs on the loop task, "
        "which is what drains the Reticulum socket; %s in it stops packets "
        "arriving" % banned
    )

# 9d-quinquies. AND THE POINTER IS ACTUALLY SET. Everything above describes a
# call through a pointer that stayed null while the other half of the chat was
# being written elsewhere — a hook nobody installs is a card and nothing else,
# which is exactly what the null default means and exactly what it looked like
# from the outside. This is the one line that closes the chat, so it is pinned:
# the room owner's entry point, registered at bring-up, once.
assert rns_code.count("rns_set_room_router(claude_route_incoming)") == 1, (
    "the router must be installed exactly once, with the room owner's entry "
    "point: no installation leaves the inbound half raising a card and "
    "dropping the message, and two would only be a second chance to get the "
    "same pointer wrong"
)
assert '#include "../agents_chat_route.h"' in rns, (
    "rns.cpp must include the seam's header rather than re-declare "
    "claude_route_incoming: the header is where the loop-safety contract the "
    "router promises is written down"
)
rns_init_body = init[: init.index("skill_register(&rns_skill);")]
assert "rns_set_room_router(claude_route_incoming);" in rns_init_body, (
    "the registration belongs in skill_rns_init, with the rest of the RNS "
    "bring-up, and before the skill is registered"
)
# Unconditionally: the router can only be reached from a packet the stack
# delivered, so gating the installation on how bring-up went would only be able
# to lose messages on a node that came up later through POST /rns/config.
assert re.search(
    r"rns_set_room_router\(claude_route_incoming\);\s*\n\s*Serial\.printf\(\"\[rns\] started=",
    rns,
), (
    "the installation must sit at the end of the bring-up, outside every "
    "`if (rns_started)` scope: a hook that exists only when boot went well is "
    "a hook missing on the node that needed POST /rns/config to come up"
)

# 9d-sexies. THE CONTROL FRAME, AND THE ASYMMETRY THAT MAKES IT SAFE.
#
# The peer now sends `1|<32 hex>|*|main,sonata` — a session field of exactly one
# byte, '*', meaning "these rooms are live" rather than "here is a message".
# Three separate things have to hold, and each one fails differently:
#
#   1. THE RECEIVE SIDE ACCEPTS IT AND THE SEND SIDE DOES NOT. Loosening the one
#      shared validator would pass every receive-side test and would also make
#      this firmware able to EMIT control frames — instructions about what a
#      screen shows, from a code path nobody wrote on purpose. Two functions is
#      what keeps that impossible.
#   2. IT REACHES THE ROUTER VERBATIM. The receiving side branches on the exact
#      byte before its own sanitiser runs, so anything done to the field here —
#      sanitising, substituting, truncating — silently breaks the other half.
#   3. IT DOES NOT BECOME A CHAT MESSAGE. No card, and no card with a null
#      router either; a room list on the screen every time the peer refreshes is
#      more junk than the list was sent to remove.
outbox_h_code = re.sub(r"/\*.*?\*/", " ", outbox_h, flags=re.S)
outbox_h_code = re.sub(r"//[^\n]*", " ", outbox_h_code)

assert "#define RNS_ENVELOPE_CONTROL_SESSION '*'" in outbox_h, (
    "the reserved session is a named constant in the format's one owner, not a "
    "'*' typed into three files that nothing keeps in step"
)
# The reserved byte is only safe BECAUSE it is outside the name charset: the
# send-side charset must stay exactly as it was, or a real room could be named
# '*' and the two meanings would collide.
assert "(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||" in outbox_h_code and \
       "c == '.' || c == '_' || c == '-')" in outbox_h_code, (
    "the session charset must stay [A-Za-z0-9._-]: the control byte is chosen "
    "for being OUTSIDE it, so widening the charset would let a room name "
    "collide with the marker"
)
build_body = outbox_h_code[outbox_h_code.index("rns_env_result rns_envelope_build("):
                           outbox_h_code.index("rns_envin_result rns_envelope_parse(")]
parse_body = outbox_h_code[outbox_h_code.index("rns_envin_result rns_envelope_parse("):]
assert "rns_session_valid(session)" in build_body, (
    "the BUILDER must keep the strict, send-side rule"
)
assert "rns_session_valid_in" not in build_body, (
    "the builder must NOT take the receive-side rule: a device that can build a "
    "control frame can tell its peer which rooms to drop, which is the one "
    "thing this asymmetry exists to prevent"
)
assert "rns_session_valid_in_n(p + sep[1] + 1, slen)" in parse_body, (
    "the PARSER must take the receive-side rule, which is the strict one plus "
    "the single reserved value"
)
assert "rns_session_is_control_n" not in build_body, (
    "the builder must not know about control frames at all"
)
# The receive-side rule is the send-side rule PLUS one value, expressed by
# calling it rather than by copying the charset — a second charset would drift.
valid_in = outbox_h_code[outbox_h_code.index("bool rns_session_valid_in_n("):
                         outbox_h_code.index("size_t rns_envelope_text_budget_n(")]
assert "rns_session_is_control_n(session, n)" in valid_in and \
       "rns_session_valid_n(session, n)" in valid_in, (
    "the receive-side validator must delegate to the send-side one and add the "
    "control value, not restate the charset"
)
# EXACTLY ONE BYTE. "**", "*a" and "a*" are not control frames and are not names
# either; the length test is what keeps them out.
is_control = outbox_h_code[outbox_h_code.index("bool rns_session_is_control_n("):
                           outbox_h_code.index("bool rns_session_valid_in_n(")]
assert "n == 1 && session[0] == RNS_ENVELOPE_CONTROL_SESSION" in is_control, (
    "a control session is EXACTLY one byte: without the length test '*a' and "
    "'**' would become control frames too"
)

# ...and in the pickup: the frame is recognised from the parsed view, counted
# apart from both other outcomes, and kept off the screen.
assert "rns_session_is_control_n(ev.session, ev.session_len)" in pickup, (
    "the pickup must recognise a control frame through the header's helper and "
    "from the PARSED view — never by looking at the raw payload itself"
)
assert re.search(r"if \(control\) g_rns_env_control\+\+;\s*else g_rns_env_parsed\+\+;",
                 pickup_norm), (
    "the three outcomes must be DISJOINT: a control frame counts as a control "
    "frame and not also as an ordinary envelope, or data_envelopes climbs while "
    "nothing appears on the screen"
)
assert "g_rns_env_control" in rns_code.split("g_rns_env_raw++")[0], (
    "the control counter belongs with the envelope counters it partitions"
)
# THE ROUTER STILL GETS IT. The guard is the parse result, NOT `!control`: a
# control frame that reached the router is the entire feature.
assert re.search(r"if \(er == RNS_ENVIN_OK && g_rns_room_router\)", pickup), (
    "the router guard must stay the parse result: excluding control frames "
    "there would deliver the card-less frame to nobody at all"
)
_route_block = pickup[pickup.index("if (er == RNS_ENVIN_OK && g_rns_room_router)"):]
assert "control" not in _route_block.split("g_rns_room_router(")[0], (
    "nothing may re-check `control` on the way to the router: the router is "
    "the consumer a control frame has"
)
# VERBATIM. `session` is a memcpy of the parsed field and nothing between the
# copy and the call touches it — no sanitiser, no substitution, no rewrite.
_sess_copy = pickup[pickup.index("memcpy(session, ev.session, ev.session_len);"):
                    pickup.index("g_rns_room_router(")]
assert "session[ev.session_len] = '\\0';" in _sess_copy, (
    "the session copy is a memcpy plus a terminator; that is the whole "
    "treatment it gets"
)
assert "rns_text_sanitize(session" not in _sess_copy and \
       "session[0] =" not in _sess_copy.split("session[ev.session_len]")[1], (
    "the session must reach the router EXACTLY as it arrived: the receiving "
    "side branches on the '*' before its own sanitiser runs, so a sanitiser "
    "here turns the one value it must recognise into an ordinary name"
)
# NO CARD, AND THAT DOES NOT DEPEND ON A ROUTER BEING INSTALLED. The guard sits
# around the card, not inside the router branch, so with g_rns_room_router null
# a control frame is shown nowhere — which is correct, because it is not a
# message.
assert pickup.index("if (!control) {") < \
       pickup.index("if (er == RNS_ENVIN_OK && g_rns_room_router)"), (
    "the card guard must be independent of the router: a control frame must "
    "not appear as a chat message on a device with no router installed either"
)
# And it is visible in the one place that can show it.
assert '"data_control"' in json_builder, (
    "GET /rns/status must publish data_control: a control frame raises no card, "
    "so the counter is the only evidence the peer is talking to this device"
)
assert "g_rns_snap.env_control = g_rns_env_control;" in publish, (
    "the counter must be sampled on the loop task like every other one"
)
assert "g_rns_snap.env_control" in json_builder, (
    "the handler must read it from the snapshot, never from the live counter"
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
# ...and the envelope's own keys, published through the loop-side snapshot like
# everything else the handler serves. The counters would be single-copy atomic
# to read directly; the address is a char array the loop task rewrites in place
# and the reason is a pointer it reassigns, and those two have no choice.
for key in ('"data_envelopes"', '"data_raw"', '"data_raw_why"',
            '"data_malformed_why"', '"data_from"', '"data_routed"',
            '"data_route_refused"', '"data_route_reason"'):
    assert key in json_builder, (
        "GET /rns/status must publish %s: a message shown raw, or one a room "
        "refused, is invisible on the screen — the card looks the same" % key
    )
for field in ("env_parsed", "env_raw", "env_routed", "env_refused",
              "env_reason", "env_raw_why", "env_bad_why", "env_from"):
    assert "g_rns_snap.%s" % field in publish, (
        "g_rns_snap.%s must be filled on the loop task in rns_status_publish()"
        % field
    )
    assert "g_rns_snap.%s" % field in json_builder, (
        "the handler must read %s from the snapshot, never from the live "
        "counter" % field
    )
assert "memcpy(g_rns_snap.env_from, g_rns_env_from,\n" \
       "           sizeof(g_rns_snap.env_from) - 1);" in publish, (
    "the address copy must stop one byte short of the end, like send_error and "
    "last_error: that last byte is never written after the initialiser, so a "
    "reader crossing the copy can see mixed characters but never an "
    "unterminated array"
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


# --- 10. rns.cpp: the send, which is the first REAL cross-task handoff -------
# Everything above defers from one context of the loop task to another. This one
# does not: POST /rns/send answers on the AsyncTCP task and the send has to run
# on the loop task, because it reaches Identity::recall() and
# Transport::outbound() — which walk and lock structures rns_stack.loop()
# mutates — and because it pays for an X25519 key exchange. So the ring in
# rns/outbox.h is the first queue in this firmware whose memory ordering is
# load-bearing rather than defensive, and this section pins that along with the
# ordering that keeps a send from vanishing.
outbox = (ROOT / "src" / "rns" / "outbox.h").read_text(encoding="utf-8")
assert '#include "rns/outbox.h"' in rns

# rns_describe() returns a page of markdown that NAMES these calls — it is a
# string literal, so stripping comments does not remove it, and counting call
# sites across the whole file would count the documentation as code. Everything
# below counts against the implementation with that page cut out.
rns_impl = (rns_code[: rns_code.index("static const char *rns_describe")] +
            rns_code[rns_code.index("static void rns_register_routes") :])


# 10a. The rules live in a pure header, like inbox.h, annsched.h and
# pktfilter.h, so the host test compiles the real code rather than a copy.
outbox_code = re.sub(r"/\*.*?\*/", " ", outbox, flags=re.S)
outbox_code = re.sub(r"//[^\n]*", " ", outbox_code)
assert not re.search(r"^\s*static\s+(?!inline\b)", outbox_code, re.M), (
    "rns/outbox.h must hold no state of its own; the firmware owns the storage "
    "and passes it in, which is what lets the host test drive the real rules"
)
for banned in ("millis(", "micros(", "esp_timer", "printf", "new ", "malloc",
               "Serial", "time(", "RNS::"):
    assert banned not in outbox_code, (
        "rns/outbox.h must not reach for %s: half of it runs on the AsyncTCP "
        "task, and all of it has to compile on the host" % banned
    )

# 10b. The ceiling is the same 383 the inbox receives into, and it is pinned
# against the library in the one translation unit that can see both. Nothing in
# microReticulum checks the plaintext size before encrypting — Packet::pack()
# throws on the PACKED size, after Destination::encrypt() has already generated
# an ephemeral X25519 keypair — so this arithmetic is the only thing standing
# between a caller and public-key time spent to produce an exception.
assert "#define RNS_OUTBOX_PAYLOAD_MAX 383" in outbox_code
assert "RNS_OUTBOX_PAYLOAD_MAX == (size_t)RNS::Type::Packet::ENCRYPTED_MDU" in rns, (
    "the outbound ceiling must be static_asserted against the library's own "
    "value, exactly like the inbox's; the host test cannot see the library"
)
# The envelope overhead is arithmetic, not a number somebody typed: version,
# separator, 32-character address, separator, separator.
assert "#define RNS_OUTBOX_ENVELOPE_OVERHEAD 36" in outbox_code
assert "#define RNS_OUTBOX_ADDR_HEX 32" in outbox_code
assert "#define RNS_OUTBOX_SESSION_MAX 23" in outbox_code
# ...and the budget must be measured against the ACTUAL session name. A budget
# that used RNS_OUTBOX_TEXT_MAX alone is right for an empty session and one byte
# over the packet for every named one.
budget = outbox_code[outbox_code.index("size_t rns_envelope_text_budget(") :]
budget = budget[: budget.index("\n}\n")]
assert "strlen(session)" in budget, (
    "the text budget is 347 - len(session), so it has to read the session name; "
    "a constant budget overflows the packet for every named session"
)

# 10c. PUBLISH-LAST, WITH A RELEASE STORE. Unlike rns_inbox_put()'s counterpart
# this is not defensive: the consumer is another task on a dual-core chip, so a
# payload store that floats past the index publishing it is a slot transmitted
# while it was still being filled. A single-threaded host test cannot see that,
# which is why the ordering is pinned in the source text.
put_body = outbox_code[outbox_code.index("bool rns_outbox_put(") :]
put_body = put_body[: put_body.index("\n}\n")]
put_stmts = [ln.strip() for ln in put_body.splitlines() if ln.strip()]
take_body = outbox_code[outbox_code.index("bool rns_outbox_take(") :]
take_body = take_body[: take_body.index("\n}\n")]
take_stmts = [ln.strip() for ln in take_body.splitlines() if ln.strip()]
assert put_stmts[-1] == "return true;" and \
       put_stmts[-2] == "bx->wr.store(wr + 1, std::memory_order_release);", (
    "rns_outbox_put() must publish the message LAST, with a release store, "
    "after every byte is in place. Found: %r" % put_stmts[-2:]
)
# The two indices are ATOMIC and MONOTONIC, one writer each. A shared count
# incremented by one task and decremented by the other — which is what inbox.h
# uses, correctly, because there both sides are the loop task — is a
# read-modify-write from two tasks here, and it loses messages.
assert "std::atomic<uint32_t> wr;" in outbox_code and \
       "std::atomic<uint32_t> rd;" in outbox_code, (
    "the ring indices must be atomics with exactly one writer each: the "
    "producer is the AsyncTCP task and the consumer is the loop task"
)
# THE TWO ACQUIRE LOADS, PINNED AS STATEMENTS — not as a substring anywhere in
# the file. `"std::memory_order_acquire" in outbox_code` was the first version
# of this check and it is satisfied by the two loads in rns_outbox_depth()
# alone: relaxing EITHER of the loads that actually guard a payload access
# passed the whole suite. The take-side one is the serious one — relaxed, the
# reads of buf[slot]/len[slot]/to[slot] may be hoisted above the load that says
# the slot is published, which is precisely the torn message this header exists
# to prevent — and no single-threaded host test can fail on either.
assert "wr = bx->wr.load(std::memory_order_acquire);" in take_stmts, (
    "rns_outbox_take() must load wr with ACQUIRE: it pairs with put()'s release "
    "store and it is what stops the payload reads below being hoisted above it"
)
assert "rd = bx->rd.load(std::memory_order_acquire);" in put_stmts, (
    "rns_outbox_put() must load rd with ACQUIRE: it pairs with take()'s release "
    "store and it is what makes 'this slot is free' mean the consumer has "
    "finished reading it"
)
# ...and each is the load of the OTHER task's counter. Its own is relaxed
# because it is its only writer; swapping the two would pin nothing.
assert "wr = bx->wr.load(std::memory_order_relaxed);" in put_stmts
assert "rd = bx->rd.load(std::memory_order_relaxed);" in take_stmts

# THE DEPTH MUST BE A POWER OF TWO, and this is a correctness requirement rather
# than a round number: the slot index is the monotonic counter modulo the depth,
# and that is only continuous across the 2^32 wrap when the depth divides 2^32.
# At five slots, wr = 0xFFFFFFFF and wr = 0 are the same slot — one queued
# message overwritten unread, another delivered twice — while the depth still
# reads 2. Setting RNS_OUTBOX_SLOTS to 5 passed every test before this assert.
assert re.search(r"static_assert\(\(RNS_OUTBOX_SLOTS & \(RNS_OUTBOX_SLOTS - 1\)\)"
                 r" == 0", outbox_code), (
    "RNS_OUTBOX_SLOTS must be static_asserted to a power of two; any other "
    "depth loses and duplicates a message when the counter wraps"
)
slots = int(re.search(r"#define RNS_OUTBOX_SLOTS (\d+)", outbox_code).group(1))
assert slots > 0 and (slots & (slots - 1)) == 0, (
    "RNS_OUTBOX_SLOTS is %d, which is not a power of two" % slots
)
assert take_stmts[-1] == "return true;" and \
       take_stmts[-2] == "bx->rd.store(rd + 1, std::memory_order_release);", (
    "rns_outbox_take() must release the slot LAST: until that store the "
    "producer must not believe it is free to overwrite. Found: %r"
    % take_stmts[-2:]
)

# 10d. THE PRODUCER PATH: POST /rns/send validates, builds and enqueues, and
# does nothing else. Pinned as an ORDERED ALLOWLIST of the calls it makes, for
# the reason sections 6d and 8c give: a denylist of forbidden identifiers could
# not be completed, and the things it would miss are exactly the ones that
# matter — a log call that blocks on the UART, an allocation, or a single
# reach into the Reticulum stack from the wrong task.
sendh = rns[rns.index('exact("/rns/send")') :
            rns.index("/* Announce this node's destination")]
sendh_code = re.sub(r"/\*.*?\*/", " ", sendh, flags=re.S)
sendh_code = re.sub(r"//[^\n]*", " ", sendh_code)
KEYWORDS = {"if", "for", "while", "switch", "return", "catch", "do"}
calls = [c for c in re.findall(r"\b([A-Za-z_][A-Za-z0-9_:]*)\s*\(", sendh_code)
         if c not in KEYWORDS]
SEND_HANDLER_CALLS = [
    "exact",                    # the route
    "require_auth",             # ...behind auth, like every other route here
    "notify_take_body",         # the collected body, malloc'd by the collector
    "free",                     # ...and released before anything else happens
    "notify_send_error",        #   (no body)
    "deserializeJson",
    "free",                     # ...released before the error branch too
    "notify_send_error",        #   (bad json)
    "rns_tx_offer",             # THE HANDOFF: validate, build, enqueue — shared
    "notify_send_error",        #   (every refusal, with the offer's own reason)
    "rns_outbox_depth",
    "notify_send_json",
]
assert calls == SEND_HANDLER_CALLS, (
    "the POST /rns/send handler is pinned call for call: it runs on the "
    "AsyncTCP task, which may not touch Transport and may not block, so "
    "anything new in it has to be argued for here first.\n  expected: %r\n"
    "  found:    %r" % (SEND_HANDLER_CALLS, calls)
)
# The body collector is passed as the fourth argument rather than called, so it
# is not in the list above — but it is what bounds the body this route accepts,
# and a route registered without it takes the framework's unbounded default.
assert "}, NULL, handle_body_collect);" in sendh_code, (
    "the send route must use the shared bounded body collector, like every "
    "other body route in this firmware"
)
# The allowlist above says what it DOES; these say what it must never become.
# Both live here because the list is ordered and a reviewer reading it should
# not have to derive the hazard from the absence of a name.
for banned in ("RNS::", "rns_stack", "rns_destination", "Transport::",
               "Identity::", "receipt_send", "Serial.", "event_add(",
               "String ", "new ", "malloc(", "delay("):
    assert banned not in sendh_code, (
        "POST /rns/send runs on the AsyncTCP task; %s does not belong in it — "
        "the handler enqueues and the loop task sends" % banned
    )
# The values come out of the parsed document as const char*, not as String:
# they point into storage the document already owns, so the only allocation on
# this path is the parse itself.
assert 'const char *to = input["to"] | "";' in sendh_code
assert 'const char *session = input["session"] | "";' in sendh_code
assert 'const char *text = input["text"] | "";' in sendh_code
# It answers "accepted", not an outcome: resolving a path takes up to a minute
# and holding the request open for it would both stall an AsyncTCP slot and lie
# about what was proven.
assert 'resp["accepted"] = true;' in sendh_code, (
    "the handler must answer promptly with acceptance; the outcome is polled "
    "from GET /rns/status, exactly as POST /rns/config already documents"
)

# 10e. Exactly one producer CALL SITE and exactly one consumer, and each is
# where it belongs. Comments are stripped first: the prose names both functions.
#
# The producer used to be POST /rns/send itself. It is not any more — a line
# typed in a chat room is offered too — and the way that stayed safe is that
# BOTH callers reach the ring through one static helper, rns_tx_offer(). So the
# count of one is now a stronger statement than it was: it says there is a
# single place that turns text into a queued envelope, which is what stops a
# second address check and a second builder drifting from these.
# TWO producer call sites now, and they are DELIBERATELY distinct wire formats,
# not two copies of one: rns_tx_offer() enqueues a seed.pager ENVELOPE, and
# rns_send_lxmf_reply() (TLORA-LXMF C4) enqueues a signed LXMF packet. Both put
# under the same g_rns_tx_mux and both feed the one send poll; what must not
# drift is the ENVELOPE builder, pinned to one site just below.
assert rns_impl.count("rns_outbox_put(") == 2, (
    "rns_outbox_put() belongs in exactly two producers: rns_tx_offer() (the "
    "seed.pager envelope) and rns_send_lxmf_reply() (the LXMF reply) — no third "
    "call site, and each is a distinct, deliberate wire format under one spinlock"
)
reply_put = rns[rns.index("bool rns_send_lxmf_reply(") :]
reply_put = reply_put[: reply_put.index("\n}\n") + 2]
assert "rns_outbox_put(&g_rns_outbox," in reply_put, (
    "the LXMF reply is the second — and only other — producer call site"
)
assert rns_impl.count("rns_outbox_take(") == 1, (
    "rns_outbox_take() belongs in the loop-task send poll and nowhere else"
)
offer = rns[rns.index("static rns_tx_result rns_tx_offer(") :
            rns.index("bool rns_send_envelope(")]
offer_code = re.sub(r"/\*.*?\*/", " ", offer, flags=re.S)
offer_code = re.sub(r"//[^\n]*", " ", offer_code)
assert "rns_outbox_put(" in offer_code, "rns_tx_offer() is the producer"
assert "rns_envelope_build(" in offer_code, "...and it is the only builder"
assert rns_impl.count("rns_envelope_build(") == 1, (
    "the envelope must be built in exactly one place; the emitter and the "
    "parser already have one owner each and this keeps the emitter's callers "
    "from growing a second set of rules"
)
assert "rns_tx_offer(" in sendh_code, "the handler goes through the shared offer"

# 10e-bis. THE RING NOW HAS TWO PRODUCER TASKS, AND THE HEADER SAYS THAT IS THE
# CALLER'S PROBLEM. rns_send_envelope() is reached from skills/agents.cpp, whose
# agents_send() runs on the LOOP task when the line was typed on the keyboard
# (main.cpp's ui_reply_submit) and on the AsyncTCP task when it arrived through
# POST /agents/send. Two tasks inside rns_outbox_put() would both read `wr`,
# both fill the same slot and both publish wr+1 — one message overwritten
# unread, one delivered twice. Nothing in a single-threaded host test can fail
# on that, so the serialisation is pinned as source text, exactly like the
# acquire/release pairing above.
assert "static portMUX_TYPE g_rns_tx_mux = portMUX_INITIALIZER_UNLOCKED;" in rns, (
    "the second producer needs a lock; a spinlock because the section is two "
    "bounded memcpys with nothing in it that can block, throw or allocate"
)
assert offer_code.index("portENTER_CRITICAL(&g_rns_tx_mux);") < \
       offer_code.index("rns_envelope_build(") < \
       offer_code.index("rns_outbox_put(") < \
       offer_code.index("portEXIT_CRITICAL(&g_rns_tx_mux);"), (
    "the critical section must cover the build AND the enqueue: at any instant "
    "exactly one task may be inside the ring's producer"
)
# Nothing that can block, yield or allocate may appear inside it — that is what
# makes a spinlock the right primitive rather than a mutex.
crit = offer_code[offer_code.index("portENTER_CRITICAL(&g_rns_tx_mux);") :
                  offer_code.index("portEXIT_CRITICAL(&g_rns_tx_mux);")]
for banned in ("Serial", "event_add", "malloc", "new ", "delay(", "String",
               "xSemaphore", "RNS::", "Transport::", "notify_"):
    assert banned not in crit, (
        "%s cannot appear inside a spinlock: the section runs with interrupts "
        "off on this core" % banned
    )
# The other reader of the peer crosses the same boundary and takes the same lock.
peer_fn = rns[rns.index("bool rns_peer_addr(") :]
peer_fn = peer_fn[: peer_fn.index("\n}\n")]
assert "portENTER_CRITICAL(&g_rns_tx_mux);" in peer_fn, (
    "rns_peer_addr() reads a 33-byte array the loop task rewrites in place, "
    "from whichever task is sending — it takes the same lock"
)

# 10f. THE SEND RUNS ON THE LOOP TASK, AFTER THE STACK. The path response the
# send is waiting for arrives through the drain inside rns_stack.loop(), so a
# step taken ahead of it reads last tick's path table and every rung of the
# ladder costs an extra RNS_TICK_MS.
assert "rns_send_poll();" in tick, "the tick must drive the send"
assert tick.index("rns_stack.loop();") < tick.index("rns_send_poll();"), (
    "the send poll must run AFTER Reticulum::loop(), which is what runs the "
    "drain that carries the path response it is waiting for"
)

sendpoll = rns[rns.index("static void rns_send_poll") :
               rns.index("/* The tick Reticulum finally gets")]
sendpoll_code = re.sub(r"/\*.*?\*/", " ", sendpoll, flags=re.S)
sendpoll_code = re.sub(r"//[^\n]*", " ", sendpoll_code)

# 10g. RESOLVE, THEN SEND — the ordering this whole commit exists for. With no
# path in the table Transport::outbound() falls through to the BROADCAST branch,
# emits a HEADER_1 packet, and the hub drops it without a word; receipt_send()
# still hands back a receipt, because a receipt means "an interface accepted it"
# and one did. So a send to an unresolvable peer does not fail — it vanishes,
# and nothing anywhere says so. These three assertions are what makes that
# unreachable.
assert "RNS::Transport::has_path(" in sendpoll_code, (
    "the send must check Transport::has_path() first; without a path the "
    "packet is broadcast and silently dropped by the hub"
)
assert rns_impl.count("receipt_send(") == 1, (
    "there is exactly one send site, and it is the one behind the resolve"
)
assert sendpoll_code.index("has_path(") < sendpoll_code.index("Identity::recall(") \
       < sendpoll_code.index("receipt_send("), (
    "the order is has_path -> recall -> send, and it is not negotiable: either "
    "resolve step failing means the packet would go nowhere in silence"
)
# ...and the guard must be a REFUSAL, not a warning. `return` out of each
# failing step is what keeps receipt_send() unreachable while it is failing.
resolve = sendpoll_code[sendpoll_code.index("if (!RNS::Transport::has_path(") :
                        sendpoll_code.index("RNS::Bytes payload(")]
assert resolve.count("return;") == 2, (
    "both failing resolve steps must return, not fall through to the send: the "
    "missing path and the unrecoverable identity"
)
# has_path() IS CHECKED ON EVERY ATTEMPT, not only while resolving. Inside the
# `if (!g_rns_out_resolved)` block it made this file's own stated invariant —
# nothing sends unless has_path() said yes — true once per MESSAGE: a rung whose
# send took no receipt re-entered at the packet build and sent again without
# re-asking. Not reachable in this port today (nothing culls the path table and
# the per-entry TTL is a week), which is exactly why it needs a pin rather than
# a comment.
assert sendpoll_code.index("if (!RNS::Transport::has_path(") < \
       sendpoll_code.index("if (!g_rns_out_resolved)"), (
    "has_path() must be checked ahead of the resolve cache, so it runs on every "
    "attempt and not only on the ones that resolve"
)

# 10h. recall() IS BOUNDED BY ATTEMPTS, NOT BY SUCCESSES — and that distinction
# is the defect this pin exists for. It is expensive here for a reason specific
# to this port: the known-destinations store is a BasicFileStore whose init()
# sits behind transport_enabled (false on this node), so recall() only works
# through the library's RNS_IDENTITY_ANNOUNCE_RECALL fallback, whose FAILURE
# path is a full _new_path_table.get() — a msgpack decode and a Packet unpack —
# where has_path() above is a bare exists() that never decodes.
#
# Caching only the success (g_rns_out_resolved) therefore bounds nothing when
# has_path() is true and recall() returns NONE: every rung re-enters and pays
# again, seven times, on the task that owns the socket drain. That is reachable
# without malice, from a path entry whose stored announce will not reconstruct.
assert rns_impl.count("Identity::recall(") == 1, (
    "exactly one recall() site: it is the expensive half of the resolve and it "
    "must not be reachable from anywhere else"
)
assert "static bool g_rns_out_recalled = false;" in rns, (
    "recall() needs a latch of its own — g_rns_out_resolved only stops a "
    "SUCCESSFUL recall from repeating, and it is the failing one that is "
    "expensive"
)
# The latch guards the call, and is set BEFORE it so a throw cannot leave it
# clear. Pinned as the ordered pair, because either half alone is satisfied by
# code that still calls recall() on every rung.
recall_branch = sendpoll_code[sendpoll_code.index("if (g_rns_out_recalled)") :
                              sendpoll_code.index("if (!peer)")]
assert recall_branch.index("g_rns_out_recalled = true;") < \
       recall_branch.index("RNS::Identity::recall(hash)"), (
    "the recall latch must be set before the call it guards"
)
assert "Identity::recall(" not in \
       sendpoll_code[: sendpoll_code.index("if (g_rns_out_recalled)")], (
    "recall() may not be reachable ahead of its own latch"
)
# ...and its result is cached, so a retry after a successful resolve does not
# pay for it again. The cache is the Destination, and the flag that guards it.
assert "g_rns_out_resolved = true;" in sendpoll_code
assert re.search(r"static RNS::Destination g_rns_out_dest\{RNS::Type::NONE\};",
                 rns), (
    "the resolved destination must be cached at file scope across ticks"
)
assert sendpoll_code.index("g_rns_out_dest = RNS::Destination(") < \
       sendpoll_code.index("g_rns_out_resolved = true;"), (
    "the flag must be raised after the destination it promises exists"
)
# Two clear sites, and only two: the pickup that starts a message and the
# retire that ends one. Anything else leaves a stale peer key attached to a new
# address.
clears = re.findall(r"(?<!bool )g_rns_out_resolved = false;", rns_code)
assert len(clears) == 2, (
    "g_rns_out_resolved is cleared when a message is picked up and when one is "
    "retired, and nowhere else; found %d sites" % len(clears)
)

# 10i. The path is requested ONCE per message, and that request is the ONLY
# thing that releases the recall latch. A path request is an outbound packet of
# its own, so repeating it on every rung answers a quiet peer with traffic
# instead of patience; there is no completion to wait for either —
# request_path() returns void, this port has no await_path() and its announce
# handlers explicitly skip path responses — so polling across ticks is the whole
# mechanism and nothing here may block.
assert rns_impl.count("request_path(") == 1, (
    "exactly one request_path() site, behind the once-per-message latch"
)
ask = rns_code[rns_code.index("static void rns_send_ask_path") :
               rns_code.index("static void rns_send_poll")]
assert ask.index("g_rns_out_path_asked = true;") < ask.index("request_path("), (
    "the latch must be set before the request, so a throw out of request_path "
    "cannot leave it asking again on every rung"
)
# THE PAIRING. Releasing the recall latch here and nowhere else is what makes
# the bound two rather than one or seven: the only event that can change what
# recall() answers is a fresh announce for the destination, a path response IS
# such an announce, and this call is the only way this node can ask for one. A
# release anywhere else is a release not caused by anything.
assert "g_rns_out_recalled = false;" in ask, (
    "firing request_path() must release the recall latch: it is the only event "
    "that can change the answer recall() gives"
)
releases = re.findall(r"(?<!bool )g_rns_out_recalled = false;", rns_code)
assert len(releases) == 3, (
    "g_rns_out_recalled is cleared in exactly three places — the pickup that "
    "starts a message, the retire that ends one, and the path request that "
    "makes a second recall worth paying for; found %d" % len(releases)
)

# 10j. The ladder is the tested pure function, not an inline comparison, and it
# is bounded. GIVE_UP is the OUTCOME this commit exists to produce: it is what
# converts a message that went nowhere into a failure with a reason.
assert "rns_outbox_next(" in outbox_code and "rns_outbox_next(" in sendpoll_code, (
    "the retry decision must be the pure function the host test drives"
)
assert "RNS_OUTBOX_GIVE_UP" in sendpoll_code and \
       "RNS_OUTBOX_WAIT" in sendpoll_code, (
    "both non-GO answers must be handled: WAIT returns, GIVE_UP fails the "
    "message with a reason"
)
assert "(uint32_t)(now_ms - last_try_ms) >= RNS_OUTBOX_RETRY_MS" in outbox_code, (
    "the throttle must compare by unsigned subtraction; `now >= last + "
    "interval` goes permanently true for one rollover's worth of time"
)
assert "#define RNS_OUTBOX_ATTEMPTS_MAX" in outbox_code and \
       "#define RNS_OUTBOX_RETRY_MS" in outbox_code, (
    "both bounds of the ladder must be named constants"
)
# The attempt is stamped BEFORE the work, for the reason the announce stamps its
# schedule first: a step that throws must back off to the throttle rather than
# retry on every 20 ms tick.
assert sendpoll_code.index("g_rns_out_tries++;") < \
       sendpoll_code.index("RNS::Bytes hash;"), (
    "the attempt must be counted before the work it pays for, or a throwing "
    "step retries on every tick"
)
assert "catch (...)" in sendpoll_code, (
    "assignHex, recall, the Destination constructor, pack()'s encryption and "
    "Transport::outbound() are all reachable here; a throw must not escape the "
    "skill tick"
)

# 10k. The encryption is MEASURED, the way the announce is. Sending does not
# sign — there is no Ed25519 on this path — but it does encrypt, with a fresh
# ephemeral X25519 keypair per packet and nothing cached between sends.
assert "micros()" in sendpoll_code, (
    "each send must be timed: it generates an ephemeral keypair and runs an "
    "exchange on the loop task, and nobody has measured that on this board"
)
# The clock must start at the PACKET, not at the resolve: including recall()
# would make the number a measurement of the path table instead.
assert sendpoll_code.index("uint32_t t0 = micros();") > \
       sendpoll_code.index("Identity::recall("), (
    "the timed window must start after the resolve, or send_us_max measures "
    "the path table rather than the encryption"
)
assert "Serial." not in sendpoll_code[
    sendpoll_code.index("uint32_t t0 = micros();"):
    sendpoll_code.index("micros() - t0")], (
    "nothing may block on the UART inside the timed window, or send_us_max "
    "becomes a measurement of the console at 115200 baud"
)

# 10l. THE SILENT FAILURE IS NOW VISIBLE, which is the requirement this whole
# section serves. Every one of these answers a question no other key can.
for key in ('"send_queued"', '"send_refused"', '"sent"', '"send_failed"',
            '"send_error"', '"send_us_last"', '"send_us_max"'):
    assert key in json_builder, (
        "GET /rns/status must publish %s: a send to a peer we cannot resolve "
        "vanishes without one, and a message nobody can see lost is a message "
        "nobody can fix" % key
    )
# The failure must carry a REASON, not just a count — "it failed" and "there is
# no path to that address" are different pieces of information and only the
# second one tells anybody what to do.
assert "g_rns_snap.send_error" in json_builder, (
    "the last failure reason must come from the loop-side snapshot"
)
assert "g_rns_send_error" not in json_builder, (
    "the handler must not read the reason at its source: the loop task rewrites "
    "that array in place, and the snapshot is how every other such value is "
    "served"
)
# A BUFFER, NOT A `const char *`. A pointer can only carry a literal chosen from
# a fixed list, so a std::length_error out of pack(), a std::invalid_argument
# and a std::bad_alloc all reach the operator as one indistinguishable sentence
# — and e.what() would then exist only on a console nobody is watching. The
# copy into the snapshot follows last_error's rule: one byte short, so a reader
# crossing it can see mixed characters but never an unterminated array.
assert re.search(r"^static char g_rns_send_error\[\d+\] = \{0\};", rns, re.M), (
    "send_error must be a buffer the loop task formats into, or the text of an "
    "exception cannot reach anybody without a serial cable"
)
assert "memcpy(g_rns_snap.send_error, g_rns_send_error,\n" \
       "           sizeof(g_rns_snap.send_error) - 1);" in publish, (
    "the reason must be copied into the snapshot one byte short of the end, "
    "exactly like last_error"
)
# ...and it must name the DESTINATION. Four messages can be queued and the
# ladder takes them one at a time, so a bare reason does not say which address
# failed, and the message is gone by the time anybody polls.
retire_raw = rns[rns.index("static void rns_send_retire") :
                 rns.index("static void rns_send_ask_path")]
assert 'snprintf(g_rns_send_error, sizeof(g_rns_send_error), "%s: %s",' in \
       retire_raw, (
    "the published reason must carry the destination it belonged to"
)
assert rns_code.count("snprintf(g_rns_send_error") == 1, (
    "the reason is written in exactly one place, on the loop task"
)
# The exception text must reach it, not only Serial. `err` is the copy taken
# inside the catch — e.what()'s storage dies with the handler.
assert "rns_send_retire(false, err);" in sendpoll_code, (
    "a throw must publish what it actually was: a fixed literal makes a "
    "length_error, an invalid_argument and a bad_alloc the same event"
)

# 10l-bis. ONLY A SUCCESSFUL SEND IS TIMED, and this is not tidiness. When
# Transport::outbound() returns false, Packet::receipt_send() emits ERROR("No
# interfaces could process the outbound packet") and Log.cpp ends every emitted
# line with a blocking Serial.flush() — INSIDE the t0..micros() window, from
# within the library, where no source-text pin over this file can see it. Timing
# the failure would publish a 115200-baud console flush as the cost of an X25519
# exchange.
assert "if (sent && dt > 0) {" in sendpoll_code, (
    "send_us_last/max must be updated only when the send succeeded: the failing "
    "branch has already blocked on a UART flush inside the measured window"
)
assert "g_rns_snap.send_queued = rns_outbox_depth(&g_rns_outbox);" in publish, (
    "the queue depth is derived from two atomics the AsyncTCP task also writes, "
    "so it is sampled on the loop task like everything else in the snapshot"
)
assert "rns_outbox_depth(&g_rns_outbox)" not in json_builder, (
    "the depth must come from the snapshot, not be computed in the handler"
)
# Failing must be one operation: count it, record why, and release what the
# message held. A failure that forgot to clear g_rns_out_busy would wedge the
# queue behind a message that is already dead.
retire = rns_code[rns_code.index("static void rns_send_retire") :
                  rns_code.index("static void rns_send_ask_path")]
for field in ("g_rns_out_busy = false;", "g_rns_out_resolved = false;",
              "g_rns_out_path_asked = false;", "g_rns_out_recalled = false;",
              "g_rns_out_dest = RNS::Destination("):
    assert field in retire, (
        "retiring a message must reset %s, or the queue stalls behind a message "
        "that is already finished — or carries its state into the next one"
        % field
    )

# 10m. The host test drives the real header, including the two cases a
# hand-written test skips: the exact byte boundary and a separator inside the
# text.
outbox_test = (ROOT / "tools" / "test_rns_outbox.cpp").read_text(encoding="utf-8")
for call in ("rns_envelope_build(", "rns_outbox_put(", "rns_outbox_take(",
             "rns_outbox_next("):
    assert call in outbox_test, (
        "tools/test_rns_outbox.cpp must drive %s — the point of the pure header "
        "is that the test runs the shipping code" % call
    )
assert "RNS_ENV_TOO_LONG" in outbox_test and "RNS_OUTBOX_PAYLOAD_MAX" in outbox_test, (
    "the boundary must be exercised at the ceiling AND one byte over it: "
    "nothing in the library checks it before spending the key exchange"
)
assert "split3(" in outbox_test, (
    "the envelope must be read back the way the peer reads it — on its FIRST "
    "THREE separators — or the rule that a '|' in the text is an ordinary "
    "character is only a comment"
)
assert ".refused ==" in outbox_test, (
    "the overflow policy is only a policy if the test pins the counter"
)
# AND BOTH DIRECTIONS OF THE CONTROL SESSION, on the same value. A test that
# only parsed '*' would pass with one loosened validator — which would also let
# this device BUILD a control frame, the one thing the asymmetry prevents.
assert 'rns_envelope_build(SELF, "*", "main,sonata"' in outbox_test and \
       "RNS_ENV_BAD_SESSION" in outbox_test, (
    "the host test must pin that the BUILDER refuses the reserved session: "
    "receiving a control frame and being able to send one are different "
    "privileges and only one of them belongs to this device"
)
for pred in ("rns_session_valid_in_n(", "rns_session_is_control_n("):
    assert pred in outbox_test, (
        "the host test must drive %s directly: the receive-side rule and the "
        "'exactly one byte' rule are where '**', '*a' and 'a*' are kept out"
        % pred
    )
assert "0xFFFFFFFEu" in outbox_test and "0xFFFFFF00u" in outbox_test, (
    "both unsigned wraps must be pinned: the ring's monotonic counters at 2^32 "
    "and the retry throttle across the millis() rollover"
)
# put() must refuse a malformed address rather than trust its one caller: it
# memcpys exactly RNS_OUTBOX_ADDR_HEX bytes out of that pointer, so a shorter
# string is a read past its terminator. It is the only function in the header
# that copies a fixed length from a caller's buffer.
assert "if (!rns_addr_valid(to)) return false;" in put_body, (
    "rns_outbox_put() must validate `to` itself — it copies a fixed 32 bytes "
    "out of it, so trusting the caller here is a read overrun and not merely a "
    "bad value"
)
assert 'rns_outbox_put(&bx, "6f796a51", env, len)' in outbox_test, (
    "the short-address refusal must be exercised"
)
assert "(RNS_OUTBOX_ATTEMPTS_MAX - 1) * RNS_OUTBOX_RETRY_MS == 60000UL" in \
       outbox_test, (
    "the ladder's real bound must be pinned as arithmetic: the first attempt is "
    "immediate and the last is not followed by a dwell, so it is 60 s and not "
    "ATTEMPTS_MAX * RETRY_MS"
)

# --- 11. the chat room's OUTGOING half, which is what closes the loop --------
# The inbound half already worked: an envelope arrives, claude_route_incoming()
# lands it in a room by session name. The outgoing half did not — agents_send()
# posted `claude` to the dead /v1/chat bridge and the room printed a bridge
# error. This section pins that it now goes over Reticulum, that it does so
# through the SAME entry point POST /rns/send uses, and that the failures are
# visible rather than silent.
uplink = agents[agents.index("static bool agents_rns_uplink") :]
uplink = uplink[: uplink.index("\n}\n")]
# Sliced forward from the definition, not between two absolute indices: the
# forward declaration of agents_on_inbound sits ABOVE agents_send.
send = agents[agents.index("static bool agents_send") :]
send = send[: send.index("static void agents_on_inbound")]

# 11a. It is the RNS entry point that is called, and it is the one declared in
# the ring's own header — not a private copy, not an HTTP POST to ourselves.
assert '#include "../rns/outbox.h"' in agents, (
    "agents.cpp must include the contract header, the same one skills/rns.cpp "
    "includes: the declaration has to have one owner"
)
assert "rns_send_envelope(peer, session, text, why)" in uplink, (
    "the room's outgoing message must go through rns_send_envelope()"
)
assert "bool rns_send_envelope(" in outbox, (
    "rns_send_envelope() must be declared in rns/outbox.h, next to the room "
    "router hook, because that header is the contract both sides include"
)
# ...and that entry point is a thin wrapper over the SHARED offer, not a second
# copy of the send. This is the assertion that would fail if somebody
# reimplemented validate-build-enqueue for the room.
wrapper = rns[rns.index("bool rns_send_envelope(") :]
wrapper = wrapper[: wrapper.index("\n}\n")]
assert "rns_tx_offer(" in wrapper, (
    "rns_send_envelope() must delegate to the same rns_tx_offer() POST "
    "/rns/send calls; a second implementation is a second set of rules"
)
for banned in ("rns_envelope_build(", "rns_outbox_put(", "rns_addr_valid("):
    assert banned not in wrapper, (
        "rns_send_envelope() must not re-do %s — that is exactly the "
        "duplication rns_tx_offer() exists to prevent" % banned
    )
# Nothing in agents.cpp may reach around it into the ring or the stack. Against
# the CODE, not the prose: the loop-safety note in the chat-route seam names
# rns_stack.loop() to explain what it is deferring off, which is documentation
# and not a reach.
agents_code = re.sub(r"/\*.*?\*/", " ", agents, flags=re.S)
agents_code = re.sub(r"//[^\n]*", " ", agents_code)
for banned in ("rns_outbox_put(", "rns_envelope_build(", "g_rns_outbox",
               "RNS::", "rns_stack"):
    assert banned not in agents_code, (
        "skills/agents.cpp must reach Reticulum only through rns_send_envelope(); "
        "%s is the transport's business" % banned
    )

# 11b. `claude` ONLY. Codex and OpenCode own their mesh inboxes and Hermes has
# its own chat door; a second uplink for them would be a second consumer of the
# same conversation, which is the reason the legacy bridge is already skipped
# for them.
assert 'if (strcmp(agent_id, "claude") != 0) return false;' in uplink, (
    "the RNS uplink is the `claude` room's reply path and nobody else's"
)
# 11c. THE ROOM'S OWN NAME IS THE SESSION FIELD. Without it the peer answers
# into whichever room is newest, and a reply typed in one room lands in another.
assert "agents_rns_uplink(agent_id, session, cleaned, &rns_why)" in send, (
    "the active session name must be the envelope's session field, so the "
    "answer comes back to the room the message was typed in"
)
# 11d. RNS IS FIRST, the bridge is the fallback — not the other way round.
assert send.index("agents_rns_uplink(") < send.index("agents_bridge_post("), (
    "RNS must be tried before the bridge; the bridge is the fallback now"
)
assert send.index("agents_rns_uplink(") < send.index("g_agents_mesh_uplink"), (
    "RNS must be tried before the mesh uplink too"
)
# 11e. NO PEER IS ITS OWN REASON, because it is the one the user can fix.
assert "rns_peer_addr(peer, sizeof(peer))" in uplink, (
    "the uplink must read the configured peer rather than inventing a "
    "destination"
)
assert '*why = "no peer address";' in uplink, (
    "an unconfigured peer must say so by name, not fall into a generic refusal"
)
# ...and a dead link is the room's own precondition, checked HERE and not inside
# the send: the ring's ladder is built to hold a message while a path resolves,
# so POST /rns/send is right to accept one over a link that is momentarily down.
# A room is the caller that has somewhere else to go.
assert "rns_link_up()" in uplink and '*why = "link down";' in uplink, (
    "the room must check the link before choosing RNS over the bridge, and name "
    "it when it falls back"
)
assert "rns_link_up(" not in wrapper and "rns_link_up(" not in offer_code, (
    "the link check must NOT be inside the shared send path: refusing every "
    "offer made over a resolving link would make POST /rns/send flaky"
)
# 11f. EVERY FAILURE IS ONE SHORT LINE IN THE ROOM. This is the whole point of
# the commit: the room used to print a bridge error for a path it was not even
# trying. Silence would be worse.
assert 'snprintf(line, sizeof(line), "(rns: %s%s)", rns_why,' in send, (
    "an RNS refusal must be printed in the room, prefixed so the user can see "
    "WHICH path refused"
)
assert '" - sent via bridge"' in send, (
    "a fallback that succeeded must still be visible: the message went out on a "
    "path the user did not choose"
)
assert send.index("if (rns_why) {") < send.index("} else if (!wifi_ok && !mesh_ok) {"), (
    "the RNS reason must REPLACE the bridge's complaint rather than join it: "
    "two lines per keystroke would push the typed message off the screen"
)
# The `why` values are static literals, so the room line needs no allocation and
# the buffer is a small stack array on the 8 KB loop task.
assert "char line[64];" in send, (
    "the room line must be a small fixed buffer: this runs on the loop task "
    "from the keyboard path, which is where the old oversized buffers overran"
)
# 11g. AND IT MUST NOT PRINT THE OLD LIE. The room said "(bridge: claude not
# wired yet)"-shaped things for a path that was dead; a bare bridge complaint
# for `claude` is now wrong by construction.
assert "not wired yet" not in agents, (
    "the placeholder that told the user claude had no transport must be gone"
)

print("Task unblock policy tests: OK")
