#!/usr/bin/env python3
"""Static pins for the conversation store's FIRMWARE halves in
skills/agents.cpp — the parts tools/test_conv_store.cpp cannot reach because
they need the FS, the mutex and the SPI bus.

WHY THESE EXIST
---------------
The pure header supports a ROOM-LESS conversation: one keyed on its address
alone, with no session component, which is the shape a mesh/LXMF peer gets. The
host test exercises that shape and passes. But a green header test says nothing
about whether the firmware can actually persist and reload such a record, and
for a while it could not: the loader dropped any line with an empty session and
the writer emitted nothing for a conversation with no rooms. The store could
therefore describe a peer on disk and silently forget it on the next boot, with
the host suite fully green — a false green on the exact seam peer registration
is built on.

So the two halves are pinned here by source shape:
  1. the writer emits a conversation-level line when a conversation has no rooms;
  2. the loader applies a room-less line instead of discarding it, and applies
     the conversation's own fields BEFORE it branches on the room — that
     ordering IS the fix, so it is asserted as an ordering, not as presence;
  3. the boot-time key migration counts what it could not move, so a card whose
     history ends up split across both key namespaces says so.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


# --- 1. the writer emits a line for a conversation with no rooms -------------
persist = fn_body(agents, "static void agents_manifest_persist()")
assert "a.n_sessions == 0" in persist, (
    "agents_manifest_persist must have a branch for a conversation with no "
    "rooms — without it a peer conversation is never written and vanishes on "
    "the next boot"
)
# That branch must write a real record with an EMPTY session field, not skip.
roomless = persist[persist.index("a.n_sessions == 0") :]
roomless = roomless[: roomless.index("for (int j")]
assert "conv_manifest_format(" in roomless, (
    "the room-less branch must emit a manifest record, not merely continue"
)
assert re.search(r'conv_manifest_format\([^;]*?a\.id,\s*""', roomless, re.S), (
    "the room-less record must carry an empty session field"
)

# --- 2. the loader keeps a room-less line ------------------------------------
apply_fn = fn_body(agents, "static void agents_manifest_apply(const ConvManifestLine &ml)")
assert "if (idx < 0) return;" in apply_fn, (
    "an unknown conversation is still skipped (C1 mints no conversations)"
)
# The conversation's own fields must be applied BEFORE the room branch: that
# ordering is the whole fix. A room-less line carries label/transport/reply and
# must not be thrown away for lacking a room.
assert "agents_conv_apply(a, ml);" in apply_fn, (
    "the conversation-level fields must be applied via agents_conv_apply"
)
assert "if (!ml.session[0]) return;" in apply_fn, (
    "a room-less line must return AFTER the conversation fields, not before"
)
assert apply_fn.index("agents_conv_apply(a, ml);") < apply_fn.index(
    "if (!ml.session[0]) return;"
), (
    "agents_manifest_apply must apply the conversation's label/transport/reply "
    "BEFORE branching on the room — returning first is exactly the bug that made "
    "a room-less conversation unloadable"
)
# The old shape must not come back: a combined guard that drops the line whole.
assert "if (idx < 0 || !ml.session[0]) return;" not in apply_fn, (
    "the combined guard discards room-less conversations — that is the "
    "regression this pin exists for"
)

# --- 3. the key migration reports what it could not move ---------------------
migrate = fn_body(agents, "static void agents_migrate_logs()")
for counter in ("moved", "skipped", "failed"):
    assert counter in migrate, f"the migration must count {counter}"
# ... and each counter must actually be INCREMENTED somewhere. Asserting only
# that the name appears is a pin that cannot bite: deleting `skipped++` leaves
# the declaration and the report line intact, so the both-files-present split
# would go silent again with this file still green — the very anti-pattern the
# breadcrumb was added to prevent.
assert "skipped++" in migrate, (
    "a skip (both key forms present) must increment the counter, not just "
    "declare it — otherwise the split is reported as 0 and vanishes"
)
assert "failed++" in migrate, (
    "a failed rename must increment the counter, not just declare it"
)
assert "moved++" in migrate, "a successful rename must increment the counter"
assert "event_add(" in migrate, (
    "the migration must leave a breadcrumb in the event log"
)
assert re.search(r"if \(moved \|\| skipped \|\| failed\)", migrate), (
    "a skip (both key forms present) or a failed rename must be reported too, "
    "not just the happy path — that is the case where history looks short"
)
# Non-destructive: the migration must never remove a log file.
assert "remove(" not in migrate, (
    "the migration must never delete a log — a failed rename leaves the legacy "
    "file exactly where it was"
)

print("conversation store firmware pins: OK")
