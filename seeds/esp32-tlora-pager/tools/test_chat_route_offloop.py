#!/usr/bin/env python3
"""Static pin: the chat-route land (claude_route_incoming) does NOT do a
synchronous SD write on the caller's (loop) task.

INVARIANT ENFORCED
------------------
The transport (skills/rns.cpp) calls claude_route_incoming() from its RNS pickup
on the LOOP task, right after rns_stack.loop(). A synchronous SD append there
would seize the shared FSPI bus that the history writer and the panel paint also
use — the exact stall the memory/bus work fought. So the SD write must be
DEFERRED off the loop task.

The pins prove, by source regex:
  1. claude_route_incoming() itself performs NO synchronous SD write: it does
     not call agents_store_append / agents_push_line / agents_session_create /
     agents_session_select, and never opens the store with append mode.
  2. It hands the reply to a dedicated off-loop FreeRTOS drain task via a
     by-value queue (xQueueSend, 0-tick, non-blocking).
  3. The drain task (agents_route_task) is where the route's SD I/O lives and
     is created at boot with a real FreeRTOS queue + task.
  4. Queue depth / task stack / prio are named constants, not magic numbers.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
header = (ROOT / "src" / "agents_chat_route.h").read_text(encoding="utf-8")


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


# --- 0. the seam exists: header declares it, agents.cpp defines it -----------
assert "bool claude_route_incoming(const char *session, const char *text, int *reason);" in header, (
    "the header must declare the locked claude_route_incoming signature"
)
assert '#include "../agents_chat_route.h"' in agents, (
    "agents.cpp must include the chat-route header"
)

caller = fn_body(
    agents, "bool claude_route_incoming(const char *session, const char *text, int *reason)"
)

# --- 1. the CALLER does NO synchronous SD write ------------------------------
# These are the store-touching helpers; none may appear in the caller body.
for banned in (
    "agents_store_append",     # the JSONL SD append
    "agents_push_line",        # append wrapper (calls store_append)
    "agents_session_create",   # manifest SD write
    "agents_session_select",   # view reload from SD
    "agents_sync_view",        # SD scan
    "agents_manifest_persist", # manifest SD write
):
    assert banned not in caller, (
        f"claude_route_incoming must NOT call {banned} on the caller/loop task"
    )
# No raw append-mode open of the store in the caller either.
assert not re.search(r'open\([^)]*"a"', caller), (
    "claude_route_incoming must not open the store in append mode on the caller task"
)

# --- 2. the caller HANDS OFF to the off-loop queue (non-blocking) ------------
assert "xQueueSend(g_route_q" in caller, (
    "claude_route_incoming must enqueue the reply to the off-loop route queue"
)
assert re.search(r"xQueueSend\(g_route_q,\s*&item,\s*0\)", caller), (
    "the enqueue must be a 0-tick, non-blocking send (never block the loop task)"
)
# reason NULL-safety is guarded.
assert "if (reason)" in caller, "the nullable reason out-param must be guarded"

# --- 3. the drain task owns the route's SD I/O -------------------------------
task = fn_body(agents, "static void agents_route_task(void *arg)")
assert "xQueueReceive(g_route_q" in task, (
    "the drain task must receive from the route queue"
)
assert "agents_push_line(" in task, (
    "the drain task (off-loop) is where the append happens"
)

# --- 4. the queue + task are created at boot with named constants ------------
init = fn_body(agents, "static void skill_agents_init()")
assert "xQueueCreate(AGENT_ROUTE_QUEUE_DEPTH" in init, (
    "skill_agents_init must create the route queue"
)
assert "xTaskCreate(agents_route_task" in init, (
    "skill_agents_init must create the dedicated drain task"
)
for macro in (
    "#define AGENT_ROUTE_QUEUE_DEPTH",
    "#define AGENT_ROUTE_TASK_STACK",
    "#define AGENT_ROUTE_TASK_PRIO",
):
    assert macro in agents, f"{macro} must be a named constant"

# --- 5. reason codes stay the locked triple ---------------------------------
for macro, val in (
    ("#define AGENT_ROUTE_OK", "0"),
    ("#define AGENT_ROUTE_OVERFLOW", "1"),
    ("#define AGENT_ROUTE_BADNAME", "2"),
):
    assert re.search(macro + r"\s+" + val + r"\b", header), (
        f"{macro} must stay {val}"
    )

print("chat-route off-loop pin: OK")
