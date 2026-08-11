#!/usr/bin/env python3
"""Static regressions for the /rns/status loop-task health diagnostics.

Three numbers were added to GET /rns/status, all produced on the loop task and
all read (never mutated live) from the AsyncTCP handler:

  - loop_stack_free_bytes: uxTaskGetStackHighWaterMark(NULL) sampled in the RNS
    tick. On this Arduino-ESP32 3.3.9 / ESP-IDF 5 build the return is in BYTES,
    not words (task.h: "the minimum free stack space there has been in bytes (as
    opposed to words in the standard FreeRTOS documentation)"), so the unit is
    baked into the field name.
  - drain_us_max: rolling max microseconds a single RnsTcpInterface::drain()
    pass has spent, via esp_timer_get_time(); GET /rns/status?reset=1 zeroes it
    (and only it) after the read.
  - path_hashes / path_hashes_total: destination hashes in Transport's path
    table. The table is an std::map/TypedStore that Transport::inbound() inserts
    into on the LOOP task; iterating it from the AsyncTCP handler would race a
    concurrent insert. The loop task snapshots up to eight hashes into a fixed
    static buffer and the handler serves ONLY that buffer.

These asserts pin the shape so a later edit cannot quietly relabel the stack
unit, drop the reset, or start iterating the live map from the HTTP handler.
"""

from pathlib import Path

ROOT = Path(__file__).parents[1]
rns = (ROOT / "src" / "skills" / "rns.cpp").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def slice_between(text, start, end, what):
    i = text.index(start)
    j = text.index(end, i + len(start))
    assert i < j, f"could not slice {what}"
    return text[i:j]


publish = slice_between(
    rns, "static void rns_status_publish", "static void rns_status_json",
    "rns_status_publish",
)
json_builder = slice_between(
    rns, "static void rns_status_json", "static const SkillEndpoint",
    "rns_status_json",
)
drain = slice_between(
    rns, "void RnsTcpInterface::drain()", "bool RnsTcpInterface::send_outgoing",
    "drain",
)
route = slice_between(
    rns,
    'server.on(AsyncURIMatcher::exact("/rns/status")',
    'server.on(AsyncURIMatcher::exact("/rns/config")',
    "/rns/status route",
)

# --- version bump so /health distinguishes this build ------------------------
assert '#define SEED_VERSION        "0.9.71"' in main, (
    "SEED_VERSION must bump to 0.9.71 for this build"
)

# --- 1. stack field: sampled on the loop task, unit baked into the name ------
assert "uxTaskGetStackHighWaterMark(NULL)" in publish, (
    "the stack high-water mark must be sampled in rns_status_publish (loop task)"
)
assert "g_loop_stack_free_bytes" in publish, (
    "the sample must be stored in the _bytes-named static"
)
assert '"loop_stack_free_bytes"' in json_builder, (
    "the stack field must be emitted, unit labelled in the name"
)
# The 4x trap: this ESP-IDF build returns BYTES, so the field must never be
# named _words, anywhere in the file.
assert "loop_stack_free_words" not in rns, (
    "ESP-IDF returns bytes, not words — the field must not be named _words"
)

# --- 2. drain_us_max: esp_timer_get_time rolling max + ?reset=1 --------------
assert "esp_timer_get_time" in drain, (
    "drain() must be timed with esp_timer_get_time (microseconds)"
)
assert "if (dt > g_drain_us_max) g_drain_us_max = dt;" in drain, (
    "drain() must keep a rolling MAX of the pass duration"
)
assert '"drain_us_max"' in json_builder, "drain_us_max must be emitted"
# Reset is gated on ?reset=1 and zeroes ONLY the drain max.
assert 'req->hasParam("reset")' in route and 'value() == "1"' in route, (
    "the status route must honour ?reset=1"
)
assert "g_drain_us_max = 0;" in route, (
    "?reset=1 must zero the drain rolling max in the handler"
)
# Reset must not live in the publish/json paths (only the handler zeroes it),
# and must touch nothing but g_drain_us_max.
assert "g_drain_us_max = 0" not in publish and "g_drain_us_max = 0" not in json_builder, (
    "only the /rns/status handler may reset the drain max"
)
assert route.count("= 0;") == 1, (
    "?reset=1 must zero the drain max and only that"
)

# --- 3. path_hashes: loop snapshot, NOT a live map walk in the handler -------
assert '"path_hashes"' in json_builder, "path_hashes array must be emitted"
assert '"path_hashes_total"' in json_builder, (
    "path_hashes_total must be emitted so truncation is visible"
)
# The handler serves ONLY the loop-side snapshot buffer...
assert "g_path_hash[" in json_builder, (
    "the handler must read the loop-side snapshot buffer g_path_hash"
)
# ...and must NOT iterate the live path table from the AsyncTCP task.
assert "new_path_table" not in json_builder, (
    "the AsyncTCP status handler must not touch the live path table"
)
assert "new_path_table" not in route, (
    "the AsyncTCP status route must not touch the live path table"
)
# The snapshot itself is filled on the loop task, in the publisher.
assert "new_path_table()" in publish, (
    "the path-hash snapshot must be taken on the loop task in rns_status_publish"
)
assert "g_path_hash" in publish and "path.key.toHex()" in publish, (
    "the publisher must copy destination hashes into the snapshot buffer"
)
# Bounded to eight fixed-width hex slots.
assert "#define RNS_PATH_HASH_MAX 8" in rns, "the snapshot must be bounded to 8"
assert "static char g_path_hash[RNS_PATH_HASH_MAX][33];" in rns, (
    "the snapshot buffer must be a fixed 8x33 static (16-byte hash hex + NUL)"
)
# path_hashes_total reuses the already-published table size.
assert "doc[\"path_hashes_total\"] = (unsigned long)g_rns_snap.paths;" in json_builder, (
    "path_hashes_total must reuse the published paths count"
)


# --- 4. the peer address: /rns.json round-trip and its validation ------------
# There is no `rns_peer` setting anywhere else — the chat room's outgoing
# message is addressed to this and to nothing else — so the three ends of it
# have to meet: POST /rns/config writes it, rns_cfg_load() reads it back, and
# GET /rns/status publishes it. A field written but never read, or read but
# never published, is a peer the user has no way to confirm.
cfg_route = slice_between(
    rns,
    'server.on(AsyncURIMatcher::exact("/rns/config")',
    'server.on(AsyncURIMatcher::exact("/rns/send")',
    "/rns/config route",
)
cfg_load = slice_between(
    rns, "static bool rns_cfg_load()", "/* ---- connect task ----", "rns_cfg_load",
)

# WRITE. The upsert merges onto what is stored, so an explicit empty string is
# the only way to clear a field; without the remove() branch a peer set once
# could never be unset.
assert 'if (!input["peer"].isNull())' in cfg_route, (
    "POST /rns/config must accept a `peer` field"
)
assert 'cfg["peer"] = peer;' in cfg_route, "the peer must be merged into /rns.json"
assert 'cfg.remove("peer");' in cfg_route, (
    "an explicit empty string must CLEAR the peer: this is a merging upsert, so "
    "there is no other way to unset it"
)
# VALIDATED WITH THE SAME FUNCTION `to` GOES THROUGH. Bytes::assignHex()
# validates nothing — a typo decodes to a DIFFERENT hash and every message is
# encrypted into the void with no error at either end — so the check has to
# happen at the door or nowhere.
assert "rns_addr_valid(peer.c_str())" in cfg_route, (
    "the peer must be validated with rns_addr_valid(), the same 32-hex check "
    "POST /rns/send puts a caller's `to` through"
)
assert 'notify_send_error(req, 400, "peer must be 32 hex characters")' in cfg_route, (
    "a bad peer must be refused synchronously, with a reason the caller can act on"
)
# The handler still only writes SPIFFS and raises the dirty flag: applying is
# the loop task's job, exactly as it already is for the endpoint.
assert "g_rns_peer" not in cfg_route, (
    "the AsyncTCP handler must not apply the peer; rns_cfg_load() on the loop "
    "task re-reads the file, like it does for the endpoint"
)

# READ BACK. Same validation on the way in, and OUTSIDE the host branch: a
# /rns.json with a bad host still has a peer worth addressing.
assert 'doc["peer"]' in cfg_load, "rns_cfg_load() must read the peer back"
assert "rns_addr_valid(p)" in cfg_load, (
    "the stored peer must be validated on load too — the file is editable"
)
assert cfg_load.index('doc["peer"]') > cfg_load.index('doc["enabled"]'), (
    "the peer must be read outside the valid-host branch: it is a destination "
    "address, not part of the endpoint"
)
# ...and published in ONE store, not cleared-then-filled: a reader crossing the
# middle of that would see no peer on a device that has one.
assert "portENTER_CRITICAL(&g_rns_tx_mux);" in cfg_load and \
       "memcpy(g_rns_peer, peer, sizeof(g_rns_peer));" in cfg_load, (
    "the peer must be published in one locked store; its reader may be the "
    "AsyncTCP task"
)
# A NEW PEER MUST NOT DROP THE LINK. rns_cfg_load()'s return value means
# "redial", and the peer decides where a message is addressed, not which socket
# is held open.
ret = cfg_load[cfg_load.rindex("return ("):]
assert "peer" not in ret, (
    "the peer must not be in the endpoint-change comparison: changing it would "
    "drop a live link for no reason"
)

# PUBLISHED. Through the snapshot, like every other loop-written string here.
assert "memcpy(g_rns_snap.peer, g_rns_peer, sizeof(g_rns_snap.peer) - 1);" in publish, (
    "the peer must be snapshotted on the loop task, one byte short, like host"
)
assert 'doc["peer"] = g_rns_snap.peer[0] ? g_rns_snap.peer : (const char *)nullptr;' \
       in json_builder, (
    "GET /rns/status must publish the peer from the snapshot, null when unset"
)

# --- 5. send_refused is no longer safe to read at its source -----------------
# It was, while POST /rns/send was the only producer: the same AsyncTCP task
# incremented it and served it. A line typed on the keyboard is now offered from
# the LOOP task, so the read has to come through the snapshot like the rest.
assert "g_rns_snap.send_refused = g_rns_outbox.refused;" in publish, (
    "send_refused must be published from the loop task now that the loop task "
    "can increment it"
)
assert 'doc["send_refused"] = (unsigned long)g_rns_snap.send_refused;' in json_builder, (
    "the handler must read send_refused from the snapshot, not from the ring"
)
assert "g_rns_outbox.refused" not in json_builder, (
    "the AsyncTCP handler must not read the ring's counter directly any more: "
    "it is no longer the only task that writes it"
)

print("rns diag tests: OK")
