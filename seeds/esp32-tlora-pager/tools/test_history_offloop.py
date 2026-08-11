#!/usr/bin/env python3
"""Static pin: the history archive WRITE runs OFF the loop task (ticket
TLORA-HISTORY / C1).

INVARIANT ENFORCED
------------------
The firmware's history persistence must never do a synchronous SD append on the
loop task (or on the AsyncTCP handler task). The only way a producer records a
record is history_enqueue(), which hands the record to a dedicated FreeRTOS
write-queue task and returns without touching the store; the actual
g_hist_store->open(..., "a") append exists in exactly ONE place —
history_write_task — which drains the queue. This is the same stall class as
RETICULUM's per-announce SPIFFS write inside the loop drain; an SD append on the
8 ms loop repeats it, so it is pinned out statically here.

The pins prove, by source regex:
  1. history.cpp has a write-queue task created with a real FreeRTOS queue, and
     the append (open "a" + encode/write) lives ONLY inside that task.
  2. history_enqueue() is a pure hand-off: xQueueSend, and NOT the store append.
  3. The producer seam is history_enqueue — there is no synchronous append API,
     and nothing in main.cpp's loop()/setup wiring calls the store append.
  4. The bus/lock discipline (HwSpiBusGuard, g_hist_mux-before-bus, chunked
     scan) is present so the off-loop task still cooperates on the shared bus.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
hist = (ROOT / "src" / "skills" / "history.cpp").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


# --- 0. the file is wired into the build and started at boot -----------------
assert '#include "skills/history.cpp"' in main, (
    "history.cpp must be compiled (included into main.cpp like the other skills)"
)
setup = main[main.index("void setup()") : main.index("void loop()")]
assert "history_begin();" in setup, "history_begin() must run at boot (mount + task)"

# --- 1. the off-loop task: a real FreeRTOS queue + a dedicated task -----------
begin = fn_body(hist, "static void history_begin()")
assert "xQueueCreate(HISTORY_WRITE_QUEUE_DEPTH" in begin, (
    "history_begin must create the write queue"
)
assert "xTaskCreate(history_write_task" in begin, (
    "history_begin must create the dedicated write task"
)
# The queue depth and task stack are named constants, not magic numbers.
for macro in ("#define HISTORY_WRITE_QUEUE_DEPTH",
              "#define HISTORY_WRITE_TASK_STACK",
              "#define HISTORY_WRITE_TASK_PRIO"):
    assert macro in hist, f"{macro} must be a named constant"

# --- 2. the append lives ONLY inside the write task --------------------------
# The single SD-append signature in this file.
APPEND = 'g_hist_store->open(HISTORY_PATH, "a")'
assert hist.count(APPEND) == 1, (
    "the archive append must exist in exactly one place (the write task), "
    f"found {hist.count(APPEND)}"
)
task = fn_body(hist, "static void history_write_task(")
assert APPEND in task, "the append must be inside history_write_task"
assert "xQueueReceive(g_hist_q" in task, (
    "the write task must drain the queue (xQueueReceive), that is its whole job"
)
assert "history_encode(" in task, "the write task streams the record via history_encode"

# --- 3. enqueue is a pure hand-off, never a synchronous write ----------------
enqueue = fn_body(hist, "static bool history_enqueue(")
assert "xQueueSend(g_hist_q" in enqueue, (
    "history_enqueue must hand the record to the queue"
)
assert "&item, 0)" in enqueue and "!= pdTRUE" in enqueue, (
    "history_enqueue must use a 0-tick send: it returns immediately, dropping on "
    "a full queue rather than blocking a producer on SD"
)
assert "g_hist_store->open" not in enqueue and "history_encode(" not in enqueue, (
    "history_enqueue must NOT touch the store or encode inline — that would put "
    "the SD append back on the producer's (loop/AsyncTCP) task"
)
# validation happens at the boundary, before anything is queued
assert "micron_store_key_valid(key)" in enqueue, (
    "history_enqueue must validate the key at the boundary"
)

# --- 3b. a full-queue drop is COUNTED, not silent (I2 data-loss visibility) ---
# A drop must bump a monotonic counter surfaced by a pure getter, so a future
# C4 /health can see the loss instead of it vanishing on the 0-tick send.
assert re.search(r"static\s+uint32_t\s+g_hist_drops", hist), (
    "a monotonic g_hist_drops counter must exist for full-queue drops"
)
assert "g_hist_drops++" in enqueue, (
    "history_enqueue must increment g_hist_drops when the 0-tick send fails "
    "(errQUEUE_FULL) — the dropped record must not vanish silently"
)
assert re.search(r"uint32_t\s+history_drops\(void\)", hist), (
    "a pure history_drops() getter must expose the drop count (for C4 /health)"
)
# The drop path still returns without touching the store (no SD write on drop).
assert "g_hist_store->open" not in enqueue, (
    "the drop path must not fall back to a synchronous SD write"
)

# --- 4. no synchronous append leaks onto any non-writer path -----------------
# Every function in history.cpp that performs the append must BE the write task.
# (Regex-derived so a future helper that opens the archive for append fails here
# instead of silently reintroducing the loop stall.)
for m in re.finditer(r'g_hist_store->open\(HISTORY_PATH,\s*"a"\)', hist):
    # find the enclosing function name by scanning backwards to the last
    # "static <type> name(" before this match
    before = hist[: m.start()]
    fn = re.findall(r"static [\w:<>\*&\s]+?(\w+)\s*\([^;{]*\)\s*\{", before)
    assert fn and fn[-1] == "history_write_task", (
        f"an archive append appears in {fn[-1] if fn else '??'}(), not the "
        "write task — that is a synchronous SD write off the queue"
    )

# --- 5. shared-bus discipline survives the move off-loop ---------------------
# NEW CONTRACT (LOOPHEALTH fix): the write task must NOT hold g_hist_mux across
# the SD append. The append burst takes ONLY the bus (an EOF append is correct
# against readers without the mux); g_hist_mux is taken ONLY afterwards, around
# the in-RAM index publish. This keeps the loop task's pure-RAM nav lookups
# (history_nav_page_*, mux-only) from waiting on the writer's SD I/O.
assert "LOCK ORDER" in hist, "history.cpp must still document the lock order"
assert "HwSpiBusGuard" in task, "the append burst must hold the SPI bus lock"
# The append burst (bus) comes FIRST, the mux take comes AFTER it — the inverse
# of the old "mux across the whole append" shape.
assert task.index("HwSpiBusGuard") < task.index("xSemaphoreTake(g_hist_mux"), (
    "the SD append (HwSpiBusGuard burst) must run BEFORE g_hist_mux is taken — "
    "the mux must NOT be held across the file burst"
)
# The append (open "a") must NOT sit inside a g_hist_mux critical section: there
# is no mux take between the start of the task's work and the append.
assert APPEND not in task[task.index("xSemaphoreTake(g_hist_mux") :], (
    "the archive append must not run under g_hist_mux — it belongs to the "
    "bus-only burst, before the index-publish critical section"
)
# The mux is taken ONLY to publish the index, and that publish happens AFTER the
# record is closed — so a concurrent reader can only observe complete records.
assert task.index("f.close()") < task.index("xSemaphoreTake(g_hist_mux"), (
    "g_hist_mux must be taken only AFTER close() — the offset is published post-"
    "close so readers never see a torn/partial record"
)
assert (
    task.index("xSemaphoreTake(g_hist_mux")
    < task.index("history_index_observe(")
    < task.index("xSemaphoreGive(g_hist_mux")
), "history_index_observe must run INSIDE the g_hist_mux critical section"

# The whole-file scan bounds the bus hold (chunked) and yields between chunks —
# the agents.cpp discipline, so the off-loop reader still cooperates on the bus.
scan = fn_body(hist, "static bool history_scan_chunked(")
assert "HISTORY_SCAN_BUS_CHUNK" in scan, "the scan must bound its per-read bus hold"
assert "taskYIELD" in scan, "the scan must yield the bus between chunks"
assert scan.index("HwSpiBusGuard") < scan.index("taskYIELD"), (
    "the bus must be released (guard scope closed) before the yield"
)
m = re.search(r"#define\s+HISTORY_SCAN_BUS_CHUNK\s+(\d+)u?", hist)
assert m and 0 < int(m.group(1)) <= 16384, (
    "HISTORY_SCAN_BUS_CHUNK must be a bounded (<= 16 KB) byte budget"
)

print("history off-loop write pin: OK")
