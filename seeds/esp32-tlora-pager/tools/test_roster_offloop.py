#!/usr/bin/env python3
"""Static pins for the live-room roster RECEIVE path (README "Telling the pager
which rooms are live").

INVARIANTS ENFORCED (by source regex, no board needed)
------------------------------------------------------
  1. The "*" control frame is recognised in claude_route_incoming BEFORE any
     sanitising — agents_session_sanitize strips '*' to an empty name, which
     would deliver the room list into a room as a normal message. So the
     `session[0] == '*'` test must appear before the agents_route_plan() call
     that does the sanitise/resolve.
  2. The "*" branch does NO synchronous SD on the caller (loop) task: it does
     not call the store-touching helpers and never opens the store in append
     mode; it only copies the payload to RAM and hands it to the off-loop drain
     via a 0-tick, non-blocking xQueueSend.
  3. The roster's SD I/O (the reconcile + manifest persist) lives off-loop, in
     agents_roster_apply, dispatched from the drain task on item.kind == 1.
  4. The apply uses the PURE planner (agents_roster_reconcile) and persists via
     the manifest; the queue item carries a kind discriminator.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
header = (ROOT / "src" / "agents_chat_route.h").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


caller = fn_body(
    agents, "bool claude_route_incoming(const char *session, const char *text, int *reason)"
)

# --- 1. the "*" check comes BEFORE the sanitising resolve --------------------
star_test = re.search(r"session\[0\]\s*==\s*'\*'\s*&&\s*session\[1\]\s*==\s*'\\0'", caller)
assert star_test, "claude_route_incoming must detect field 3 == \"*\" (one byte)"
plan_call = caller.index("agents_route_plan(")
assert star_test.start() < plan_call, (
    "the \"*\" comparison MUST precede agents_route_plan() — compare before sanitising"
)

# Isolate the "*" branch body: from the star test to the end of its if-block.
star_branch = caller[star_test.start() : caller.index("\n    }", star_test.start())]

# --- 2. the "*" branch does NO synchronous SD on the caller task ------------
for banned in (
    "agents_store_append",
    "agents_push_line",
    "agents_session_create",
    "agents_session_select",
    "agents_sync_view",
    "agents_manifest_persist",
    "agents_roster_apply",       # the SD reconcile is off-loop, not here
):
    assert banned not in star_branch, (
        f"the \"*\" branch must NOT call {banned} on the caller/loop task"
    )
assert not re.search(r'open\([^)]*"a"', star_branch), (
    "the \"*\" branch must not open the store in append mode on the caller task"
)

# --- 2b. it hands off to the off-loop queue, 0-tick, tagged as a roster ------
assert "item.kind = 1" in star_branch, (
    "the \"*\" branch must tag the queue item as a roster (kind = 1)"
)
assert re.search(r"xQueueSend\(g_route_q,\s*&item,\s*0\)", star_branch), (
    "the roster enqueue must be a 0-tick, non-blocking send"
)

# --- 3. the drain task dispatches kind == 1 to the off-loop apply ------------
task = fn_body(agents, "static void agents_route_task(void *arg)")
assert "item.kind == 1" in task and "agents_roster_apply(" in task, (
    "agents_route_task must route roster items (kind == 1) to agents_roster_apply"
)

# --- 4. the off-loop apply is the ONLY roster SD; it uses the pure planner ---
apply = fn_body(agents, "static void agents_roster_apply(int idx, const char *payload)")
assert "agents_roster_reconcile(" in apply, (
    "agents_roster_apply must use the pure reconcile planner"
)
assert "agents_manifest_persist()" in apply, (
    "agents_roster_apply must persist the liveness flags (roster SD lives here)"
)

# --- 5. the queue item carries a kind discriminator; pure planner is host-side
assert re.search(r"struct AgentRouteItem\s*\{[^}]*uint8_t kind;", agents, re.S), (
    "AgentRouteItem must carry a kind discriminator (0 message, 1 roster)"
)
for sym in (
    "static inline void agents_roster_reconcile(",
    "#define AGENT_ROSTER_ALIVE",
    "#define AGENT_ROSTER_DEAD",
    "#define AGENT_ROSTER_UNCHANGED",
):
    assert sym in header, f"{sym} must live in the host-testable header"

print("roster off-loop pin: OK")
