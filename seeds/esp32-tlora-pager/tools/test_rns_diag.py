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
assert '#define SEED_VERSION        "0.9.64"' in main, (
    "SEED_VERSION must bump to 0.9.64 for this build"
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

print("rns diag tests: OK")
