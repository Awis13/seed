#!/usr/bin/env python3
"""Static pin: notify persistence runs OFF the loop task (ticket TLORA-HISTORY / C3).

INVARIANT ENFORCED
------------------
Before C3, notify cards were mirrored to /notify.json by notify_save(), a ~3 KB
SPIFFS write executed on the LOOP task from notify_poll() (the notify skill's
.tick). That loop-task filesystem write is the LOOPHEALTH loop-blocker class this
ticket removes. C3 migrates notify persistence onto the unified history archive:
every persist goes through history_enqueue() (a 0-tick queue hand-off whose SD
append runs on the archive's own write task), and the loop-task /notify.json write
is deleted outright.

The pins prove, by source regex:
  1. the old loop-task SPIFFS path is GONE: no notify_save/notify_load, no
     write_spiffs_file_atomic / read_spiffs_file, no /notify.json in notify.cpp.
  2. the .tick (notify_poll) does NO persistence write — it only expires.
  3. persistence is a write-through to the off-loop archive: notify_archive_put
     calls history_enqueue under the distinct MICRON_NS_NOTIFY namespace, and it
     is NOT derived from the client-supplied `source` field.
  4. the enqueue is off the spinlock: notify_archive_put holds no critical
     section, and notify_archive_by_id enqueues only after portEXIT_CRITICAL.
  5. the create/update and ack seams actually call the archive write-through.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
notify = (ROOT / "src" / "skills" / "notify.cpp").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    """Slice a function DEFINITION.

    The guard is not decoration: the firmware forward-declares many of these
    near the top of their file, and `text.index(sig)` happily lands on the
    declaration. The slice then runs from there to the end of some unrelated
    later function, so an assertion against it — especially an ABSENCE
    assertion — can pass on code that has nothing to do with the function under
    test. That is a pin that looks green and checks nothing. A slice whose head
    reaches a ';' before its first '{' is therefore rejected outright.
    """
    start = text.index(sig)
    end = text.index(terminator, start)
    body = text[start : end + len(terminator)]
    head = body[: body.index("{")] if "{" in body else body
    assert ";" not in head, (
        f"fn_body({sig!r}) matched a forward declaration, not the definition — "
        "the assertions below would be checking unrelated code"
    )
    return body


# --- 1. the old loop-task SPIFFS snapshot is entirely gone --------------------
# The write primitives, the file paths, the dirty-flag machinery and the snapshot
# codec definitions must all be gone (prose may still reference the old names to
# explain the migration; code must not).
for gone in ("write_spiffs_file_atomic", "read_spiffs_file",
             '"/notify.json"', '"/notify.tmp"',   # the path LITERALS, not prose
             "notify_mark_dirty", "notify_dirty", "notify_save_at",
             "static void notify_save(", "static void notify_load(",
             "notify_snapshot_store(", "notify_snapshot_restore(",
             "notify_restore_entries("):
    assert gone not in notify, (
        f"'{gone}' must be gone from notify.cpp — notify no longer writes SPIFFS "
        "on the loop task (persistence is the off-loop history archive)"
    )

# --- 2. the tick does no persistence write -----------------------------------
poll = fn_body(notify, "static void notify_poll()")
for bad in ("history_enqueue", "write_spiffs", "SPIFFS.open", "notify_save",
            "->open("):
    assert bad not in poll, (
        f"notify_poll (the .tick, on the loop task) must not '{bad}' — expiry is "
        "RAM-only; persistence happens off-loop at create/ack"
    )
assert "notify_expire()" in poll, "notify_poll must still expire cards each second"

# --- 3. persistence is an off-loop archive write-through ----------------------
put = fn_body(notify, "static void notify_archive_put(")
assert "history_enqueue(MICRON_NS_NOTIFY" in put, (
    "notify_archive_put must write through history_enqueue under the DISTINCT "
    "MICRON_NS_NOTIFY namespace"
)
assert "notify_rec_encode(" in put, "notify_archive_put must serialize via the pure codec"
# The namespace passed to the archive is the fixed trusted MICRON_NS_NOTIFY
# literal, never a value computed from the client-supplied `source` field.
enq = put[put.index("history_enqueue(") : put.index(")", put.index("history_enqueue("))]
assert enq.startswith("history_enqueue(MICRON_NS_NOTIFY,"), (
    "the archive namespace must be the fixed MICRON_NS_NOTIFY literal, never "
    f"derived from the card body — found: {enq!r}"
)

# --- 4. the enqueue is off the notify spinlock -------------------------------
assert "portENTER_CRITICAL" not in put, (
    "notify_archive_put must not run inside the notify spinlock — history_enqueue "
    "hands off to a queue and must be called outside any critical section"
)
by_id = fn_body(notify, "static void notify_archive_by_id(")
assert by_id.index("portEXIT_CRITICAL") < by_id.index("notify_archive_put"), (
    "notify_archive_by_id must copy the card under the lock, then archive AFTER "
    "releasing it — never enqueue while holding the spinlock"
)

# --- 5. the seams call the write-through -------------------------------------
push = fn_body(notify, "static uint32_t notify_push(")
assert "notify_archive_put(e)" in push, (
    "notify_push (the create/update seam for both producers) must write the card "
    "through to the archive"
)
ack = fn_body(notify, "static bool notify_ack_id(")
assert "notify_archive_by_id(id)" in ack, (
    "notify_ack_id must re-archive the card when its unread badge changes, so the "
    "read state survives a reboot"
)
ackall = fn_body(notify, "static int notify_ack_all()")
assert "notify_archive_by_id(" in ackall, (
    "notify_ack_all must re-archive each newly-read card"
)

# --- 6. boot-restore is wired and reads the archive (not /notify.json) --------
assert "notify_restore_from_archive();" in main, (
    "setup() must rebuild the notify ring from the archive"
)
setup = main[main.index("void setup()") : main.index("void loop()")]
assert setup.index("history_begin();") < setup.index("notify_restore_from_archive();"), (
    "notify_restore_from_archive must run AFTER history_begin (archive mounted + "
    "index seeded before the ring is rebuilt)"
)
assert setup.index("skills_init();") < setup.index("history_begin();"), (
    "conversation init must finish before notify archive restore/reconciliation"
)
skills = fn_body(main, "static void skills_init()")
assert skills.index("skill_agents_init();") < skills.index("skill_meshcore_init();")
restore = fn_body(main, "static void notify_restore_from_archive()")
assert "history_restore_at(MICRON_NS_NOTIFY" in restore, (
    "boot-restore must walk the archive index for MICRON_NS_NOTIFY, newest-first"
)
assert "notify_rec_decode(" in restore and "notify_restore_one(" in restore, (
    "boot-restore must decode each archived card body and insert it into the ring"
)
assert "notify_restore_finish(restored);" in restore
assert "notify_reconcile_restored_chats();" in restore
assert restore.index("notify_restore_finish(restored);") < restore.index(
    "notify_reconcile_restored_chats();"
), "legacy chat doors must reconcile only after the complete archive restore"
reconcile = fn_body(main, "static void notify_reconcile_restored_chats() {")
assert "notify_chat_reconcile_plan(" in reconcile
assert "agents_restore_inbound(" in reconcile
assert "notify_chat_restore_before(" in reconcile, (
    "restored chat doors must be replayed oldest-first"
)
assert "agents_has_origin(" in reconcile, (
    "boot dedup must use durable card origin, never text equality"
)
assert "NOTIFY_CHAT_DRAIN_ACK_ROUTED" in reconcile
assert "blocked[ax] = true" in reconcile
assert "notify_chat_retry_pending = true" in reconcile
assert reconcile.index("agents_restore_inbound(") < reconcile.index(
    "notify_ack_id(view.id);"
), "a restored card may be ACKed only after the thread accepts it"
assert "ui_note_input" not in reconcile and "ui_note_wake" not in reconcile
assert "SPIFFS" not in reconcile and "history_enqueue" not in reconcile, (
    "boot reconciliation must persist only through notify_ack_id"
)
runtime = fn_body(main, "static bool notify_reconcile_pending_chats(")
assert "agents_chat_door_enqueue(" in runtime
assert "agents_has_origin(" not in runtime and "agents_on_inbound(" not in runtime
assert "notify_ack" not in runtime, (
    "loop-side pending scan may only enqueue; origin scan, durable append and ACK wait for completion"
)
completion = fn_body(main, "static void notify_take_chat_completions(")
assert "notify_ack_identity(" in completion
assert "notify_chat_inflight_remove(done.conversation, &done.event)" in completion
assert "done.slot_hint" not in completion
pending = fn_body(main, "static bool notify_reconcile_pending_chats(")
assert "notify_chat_inflight_find(resolution.id)" in pending
assert "notify_chat_inflight_add(resolution.id, &view.event_id)" in pending
assert "notify_set_event_distinct_fn(notify_event_distinct_cb);" in main
classifier = fn_body(main, "static bool notify_event_distinct_cb(const char *source, const char *key) {")
assert "agents_notify_chat_resolve_snapshot(" in classifier

agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
worker = fn_body(agents, "static void agents_route_task(")
assert "agents_has_origin(" in worker and "agents_on_inbound(" in worker
assert worker.index("agents_has_origin(") < worker.index("agents_on_inbound(")
assert "xQueueSend(g_door_done_q, &done, portMAX_DELAY)" in worker
assert "item.door_source, item.door_key" in worker
assert "notify_chat_stable_slot(" in worker
enqueue = fn_body(agents, "static bool agents_chat_door_enqueue(")
assert "strcmp(g_convs[idx].id, conversation) != 0" in enqueue
assert "item.via =" not in enqueue, "notification jobs must not retain recyclable slots"
snapshot = fn_body(agents, "static NotifyChatResolution agents_notify_chat_resolve_snapshot(")
assert "portENTER_CRITICAL(&agents_registry_spin)" in snapshot
assert "agents_lock" not in snapshot and "xSemaphoreTake" not in snapshot
assert "agents_registry_publish_locked();" in agents
append = fn_body(agents, "static bool agents_store_append(int idx, bool from_me, const char *text,")
persist = append.index("if (!persisted) return false;")
assert persist < append.index("a.last.from_me = from_me;")
assert persist < append.index("agents_view_append(g_win")
assert persist < append.index("a.unread++")
assert "if (!persisted) return false;" in append, (
    "verified persistence failure must stop before RAM/unread state changes"
)
record = fn_body(agents, "static size_t agents_record_build(")
assert all(field in record for field in
           ('\\"origin_id\\"', '\\"origin_key\\"', '\\"origin_event\\"')), (
    "notification-backed lines must persist durable origin correlation"
)
origin = fn_body(agents, "static bool agents_has_origin(")
assert "for (int i = 0; i < a.n_sessions && !found; i++)" in origin, (
    "origin search must cover non-active rooms too"
)
assert "agents_jsonl_origin_matches(record, origin_id," in origin
assert "notify_event_id_valid(origin_event)" in origin
matcher = fn_body(agents, "static bool agents_jsonl_origin_matches(")
assert "notify_event_origin_matches(" in matcher, (
    "numeric ids and reused client keys must never deduplicate distinct events"
)

notify = (ROOT / "src" / "skills" / "notify.cpp").read_text(encoding="utf-8")
take_event = fn_body(notify, "static bool notify_event_take(NotifyEventId &out) {")
assert "portENTER_CRITICAL" in take_event
assert "notify_event_ram_take(" in take_event
for forbidden in ("Preferences", "notify_event_reserve_locked", "portMAX_DELAY",
                  "xSemaphoreTake"):
    assert forbidden not in take_event, f"live event allocation must not use {forbidden}"
raise_fn = fn_body(notify, "static uint32_t notify_raise(")
assert "Preferences" not in raise_fn and "notify_event_reserve_locked" not in raise_fn
init_fn = fn_body(notify, "static void skill_notify_init() {")
assert "notify_event_reserve_locked();" in init_fn
reserve = fn_body(notify, "static bool notify_event_reserve_locked() {")
assert 'putULong64("counter_hi", new_limit)' in reserve
assert 'getULong64("counter_hi", 0) == new_limit' in reserve
assert 'NOTIFY_EVENT_STATE_INITIALIZING' in reserve
assert 'NOTIFY_EVENT_STATE_READY' in reserve
assert 'getType("counter_hi") == PT_U64' in reserve
assert reserve.index('putULong64("counter_hi", new_limit)') < reserve.index(
    "notify_event_next.counter = reservation.first;"
), "a counter block must persist and read back before any identity is issued"
verified = fn_body(agents, "static bool agents_store_append_verified(")
for required in ("ftruncate", "fsync", "agents_fd_write_all",
                 "agents_fd_read_all", "memcmp", "agents_fd_rollback"):
    assert required in verified, f"strict door append must use {required}"
write_path = verified[verified.index("agents_fd_write_all") :]
assert write_path.index("fsync(fd)") < write_path.index("close(fd)")
assert "O_RDONLY" in verified, "verification must reopen the file after close"

print("notify off-loop persistence pin: OK")
