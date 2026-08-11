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
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


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
restore = fn_body(main, "static void notify_restore_from_archive()")
assert "history_restore_at(MICRON_NS_NOTIFY" in restore, (
    "boot-restore must walk the archive index for MICRON_NS_NOTIFY, newest-first"
)
assert "notify_rec_decode(" in restore and "notify_restore_one(" in restore, (
    "boot-restore must decode each archived card body and insert it into the ring"
)

print("notify off-loop persistence pin: OK")
