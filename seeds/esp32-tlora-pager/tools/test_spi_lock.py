#!/usr/bin/env python3
"""Static regressions for shared SPI bus arbitration (vendor LilyGoLib
lockSPI/unlockSPI shape). One FSPI bus serves the ST7796 (loop task), the SD
history store (AsyncTCP task via the agents HTTP routes) and the SX1262
(RadioLib hal). Every complete bus operation must hold the bus mutex, chip
selects may only be asserted inside a locked transaction, and the lock order
against agents_mux must never invert."""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
hw_ui = (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")
hw_ui_h = (ROOT / "src" / "hw_ui.h").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
mc_target = (ROOT / "src" / "mesh" / "mc_target.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    """Slice from a function signature to its closing brace at column 0."""
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


# --- the lock exists, is non-recursive, and documents the invariant ----------
assert "static SemaphoreHandle_t spi_bus_mux" in hw_ui, "bus mutex missing"
assert "xSemaphoreCreateMutex()" in hw_ui, (
    "bus mutex must be a plain (non-recursive) mutex — the invariant below "
    "makes recursion unnecessary"
)
assert "xSemaphoreCreateRecursiveMutex" not in hw_ui, (
    "bus lock must stay non-recursive; nesting means the public/helper "
    "invariant broke"
)
assert "INVARIANT" in hw_ui, "the public-entry/helper invariant must be written down at the lock"
assert "LOCK ORDER" in hw_ui, "the agents_mux-first/bus-second order must be written down"
assert "void hw_spi_bus_lock()" in hw_ui and "void hw_spi_bus_unlock()" in hw_ui

# public surface + RAII guard
assert "void hw_spi_bus_lock();" in hw_ui_h and "void hw_spi_bus_unlock();" in hw_ui_h
assert "struct HwSpiBusGuard" in hw_ui_h, "RAII guard missing from hw_ui.h"

# created before the first bus byte
panel_begin = fn_body(hw_ui, "static bool panel_begin()")
assert "xSemaphoreCreateMutex()" in panel_begin, (
    "the mutex must exist before panel_begin touches the bus"
)
assert panel_begin.index("xSemaphoreCreateMutex()") < panel_begin.index("park_spi_cs()")
assert "HwSpiBusGuard" in panel_begin

# --- every public paint entry takes the lock before painting -----------------
PAINT_ENTRIES = [
    "void hw_ui_show_clock()",
    "void hw_ui_clock_tick(",
    "void hw_ui_clock_rule_tick(",
    "void hw_ui_clock_bar(",
    "void hw_ui_show_notify(",
    "void hw_ui_show_agent_invite(",
    "void hw_ui_show_card_act(",
    "void hw_ui_show_menu(",
    "void hw_ui_show_layout(",
    "void hw_ui_show_settings(",
    "void hw_ui_show_meshcore(",
    "void hw_ui_show_mesh_ping(",
    "void hw_ui_show_mesh_ping_result(",
    "void hw_ui_show_wifi(",
    "void hw_ui_show_wifi_list(",
    "void hw_ui_show_wifi_info(",
    "void hw_ui_show_agents(",
    "void hw_ui_show_agent_sessions(",
    "int hw_ui_show_agent_chat(",
    "void hw_ui_show_agent_act(",
    "void hw_ui_show_msglist(",
    "void hw_ui_show_reply(",
    "void hw_ui_show_info(",
    # Boot-progress note (storage format feedback) is a public paint entry
    # like any other: guarded for the whole paint, releases before returning.
    "void hw_ui_boot_note(",
]
for sig in PAINT_ENTRIES:
    body = fn_body(hw_ui, sig)
    assert "HwSpiBusGuard" in body, f"{sig} paints without the bus lock"
    first_paint = min(
        (body.index(m) for m in ("tft_", "draw_field", "menu_draw_row",
                                 "ping_draw_", "msglist_draw_row")
         if m in body),
        default=len(body),
    )
    assert body.index("HwSpiBusGuard") < first_paint, (
        f"{sig} must take the bus lock BEFORE its first paint call"
    )

# Every hw_ui_show_* definition in the file MUST be listed as a paint entry
# above, so it is subject to the bus-lock assertion. Regex-derived so a future
# hw_ui_show_foo() added without landing in PAINT_ENTRIES fails HERE instead of
# painting under no lock and passing silently (the C4-review gap).
_paint_name = lambda s: s.split(None, 1)[1].split("(")[0]
_paint_names = {_paint_name(s) for s in PAINT_ENTRIES}
_show_defs = re.findall(r"^(?:void|int|bool) (hw_ui_show_\w+)\(", hw_ui, re.M)
assert len(_show_defs) >= 15, (
    f"regex found only {len(_show_defs)} hw_ui_show_* defs — pattern drifted, "
    "the derived paint-entry guard would be toothless"
)
for _name in _show_defs:
    assert _name in _paint_names, (
        f"{_name}() is a hw_ui_show_* paint entry but is missing from "
        "PAINT_ENTRIES — add it so it gets the bus-lock-before-paint assertion"
    )

# sleep pair: the SLPIN/SLPOUT command goes out under the lock, the panel
# settle delays run with the bus released (they are not bus traffic)
sleep_fn = fn_body(hw_ui, "void tft_sleep()")
wake_fn = fn_body(hw_ui, "void tft_wake()")
for name, body, cmd in (("tft_sleep", sleep_fn, "tft_cmd(0x10)"),
                        ("tft_wake", wake_fn, "tft_cmd(0x11)")):
    assert "HwSpiBusGuard" in body, f"{name} must hold the bus lock for its command"
    assert body.index("HwSpiBusGuard") < body.index(cmd)
    assert body.index(cmd) < body.index("delay("), (
        f"{name}: settle delay must sit outside the locked command scope"
    )

# --- CS discipline: asserted only inside beginTransaction/endTransaction -----
CS_PRIMITIVES = [
    "static void tft_cmd(uint8_t c)",
    "static void tft_data(const uint8_t *d, size_t n)",
    "static void tft_fill(uint16_t color)",
    "static void tft_fill_rect(",
    "static void tft_draw_glyph(",
]
for sig in CS_PRIMITIVES:
    body = fn_body(hw_ui, sig)
    assert body.index("beginTransaction") < body.index(
        "digitalWrite(PIN_DISP_CS, LOW)"
    ), f"{sig}: CS may go LOW only after beginTransaction (bus already granted)"
    assert body.index("digitalWrite(PIN_DISP_CS, HIGH)") < body.index(
        "endTransaction"
    ), f"{sig}: CS must be HIGH again before the transaction ends"
# no primitive may drop CS before its transaction anywhere in the file
assert not re.search(
    r"digitalWrite\(PIN_DISP_CS, LOW\);[^\n]*\n\s*disp_spi->beginTransaction",
    hw_ui,
), "found a CS assert OUTSIDE the SPI transaction — the C4 bug shape"

# helpers stay lock-free (they run under the caller's lock)
for sig in CS_PRIMITIVES + ["static void tft_window(", "static void tft_fill_circle("]:
    body = fn_body(hw_ui, sig)
    assert "HwSpiBusGuard" not in body and "hw_spi_bus_lock" not in body, (
        f"{sig} is an internal helper: it must ASSUME the lock, not take it "
        "(non-recursive mutex would deadlock)"
    )

# ...and so does EVERY paint helper: any *_draw_row / draw_field / ping_draw_* /
# tft_draw_text* runs inside a public entry's already-held lock. Regex-derived
# (C4-review widen): a new draw_row helper that grabs the guard itself would
# re-enter the non-recursive bus mutex and deadlock — catch it statically.
DRAW_HELPERS = []
for _m in re.finditer(r"^(static (?:void|int|bool) (\w+)\()", hw_ui, re.M):
    _sig, _name = _m.group(1), _m.group(2)
    if (_name.endswith("_draw_row") or _name == "draw_field"
            or _name.startswith("ping_draw_") or _name.startswith("tft_draw_text")):
        DRAW_HELPERS.append(_sig)
assert len(DRAW_HELPERS) >= 12, (
    f"draw-helper regex found only {len(DRAW_HELPERS)} helpers — pattern "
    "drifted, the lock-free assertion below would be toothless"
)
for sig in DRAW_HELPERS:
    body = fn_body(hw_ui, sig)
    assert "HwSpiBusGuard" not in body and "hw_spi_bus_lock" not in body, (
        f"{sig} is a paint helper: it must ASSUME the caller's lock, not take "
        "it (re-entering the non-recursive bus mutex deadlocks)"
    )

# --- tft_fill_circle: spans per scanline, not a pixel matrix -----------------
circle = fn_body(hw_ui, "static void tft_fill_circle(")
assert circle.count("for (") == 1, (
    "fill_circle must be a single scanline loop, not the old dy*dx pixel matrix"
)
assert "tft_fill_rect(cx + dx, cy + dy, 1, 1" not in circle, (
    "per-pixel tft_fill_rect loop is back — (2r+1)^2 transactions"
)
assert ", 1, color)" in circle, "each scanline must be one horizontal span fill"

# --- CS parking at boot (vendor initShareSPIPins) ----------------------------
park = fn_body(hw_ui, "static void park_spi_cs()")
for pin in ("PIN_LORA_CS", "PIN_LORA_RST", "PIN_NFC_CS", "PIN_SD_CS", "PIN_DISP_CS"):
    assert pin in park, f"{pin} must be parked HIGH before first bus use"
assert "digitalWrite(p, HIGH)" in park
assert panel_begin.index("park_spi_cs()") < panel_begin.index("disp_spi->begin"), (
    "CS parking must happen before the bus is brought up"
)

# --- SD side: bus lock inside agents_mux, never the other way round ----------
assert "LOCK ORDER" in agents, "agents.cpp must state the agents_mux-first convention"
append = fn_body(agents, "static void agents_store_append(")
assert "HwSpiBusGuard" in append, "SD history append runs on the AsyncTCP task unlocked"
assert append.index("HwSpiBusGuard") < append.index("g_store->open")

for sig in (
    "static void agents_sync_view(",
    "static void agents_manifest_load()",
    "static void agents_manifest_persist()",
    "static void agents_load_page(",
    "static bool agents_file_last(",
    "static void agents_store_init()",
):
    body = fn_body(agents, sig)
    assert "HwSpiBusGuard" in body, f"{sig} touches the store without the bus lock"

clear = fn_body(agents, "static bool agents_clear(")
assert "HwSpiBusGuard" in clear, "agents_clear removes files without the bus lock"
# lock-order inversion guard: nothing takes agents_mux inside a bus-locked
# region — textually, no agents_lock() call after a guard within one scope in
# the guarded store helpers (they contain no agents_lock at all)
for sig in ("static void agents_store_append(", "static void agents_sync_view(",
            "static void agents_load_page(", "static bool agents_file_last("):
    body = fn_body(agents, sig)
    assert "agents_lock()" not in body, (
        f"{sig}: taking agents_mux under the bus lock inverts the lock order"
    )

# --- whole-file session scans bound the bus hold (chunked, Option A) ----------
# Unbounded session JSONL (no rotation) scanned under one bus lock would freeze
# the SX1262 servicing for the whole read (~300 ms for ~100 KB) and drop an
# inbound LoRa packet sitting in the FIFO. Each scan must read a bounded number
# of bytes under the bus lock, release it, yield, then re-take and continue.
m = re.search(r"#define\s+AGENT_SCAN_BUS_CHUNK\s+(\d+)u?", agents)
assert m, "AGENT_SCAN_BUS_CHUNK bus-hold budget constant is missing"
_chunk = int(m.group(1))
assert 0 < _chunk <= 16384, (
    f"AGENT_SCAN_BUS_CHUNK={_chunk} is out of range; at ~350 KB/s a chunk must "
    "stay well under the ~40 ms that would still hitch the radio (<= 16 KB)"
)

# The scanner tracks bytes read so the budget can be enforced.
assert "size_t total_read;" in agents, (
    "AgentJScanner must count bytes read (total_read) to budget the bus hold"
)

# The chunked-scan helper: releases the bus per bounded chunk and yields, and
# NEVER releases agents_mux (that is what keeps the file immutable between
# chunks — every writer also holds agents_mux, so no append/remove/rewrite can
# land under the open File handle; the size-shrink case cannot occur mid-scan).
scan = fn_body(agents, "static void agents_scan_chunked(")
assert "HwSpiBusGuard" in scan, "agents_scan_chunked must take the bus per chunk"
assert "AGENT_SCAN_BUS_CHUNK" in scan, "agents_scan_chunked must enforce the byte budget"
assert "total_read" in scan, "agents_scan_chunked must budget on bytes read"
assert "taskYIELD" in scan, "agents_scan_chunked must yield after releasing the bus"
assert scan.index("HwSpiBusGuard") < scan.index("taskYIELD"), (
    "the bus must be RELEASED (guard scope closed) before the yield"
)
assert "if (!more) break;" in scan, "agents_scan_chunked must stop at EOF"
assert "agents_lock()" not in scan and "agents_unlock()" not in scan, (
    "agents_scan_chunked must NOT touch agents_mux — the caller holds it across "
    "the whole scan; releasing it mid-scan would let agents_clear remove the "
    "file under the open handle"
)

# The three JSONL scanners route through the chunked helper (not one big burst).
for sig in ("static void agents_sync_view(",
            "static void agents_load_page(",
            "static bool agents_file_last("):
    body = fn_body(agents, sig)
    assert "agents_scan_chunked" in body, (
        f"{sig} must scan via agents_scan_chunked so the bus hold is bounded"
    )

# agents_sync_view keeps the size-shrink → full-rebuild guard (external
# truncation across calls; a concurrent writer is excluded by agents_mux).
sync = fn_body(agents, "static void agents_sync_view(")
assert "sz < a.file_sync" in sync, (
    "agents_sync_view must keep the shrink-detect full-rebuild path"
)

# agents_session_refresh_counts counts newlines in bounded bus chunks + yields.
refresh = fn_body(agents, "bool agents_session_refresh_counts(")
assert "AGENT_SCAN_BUS_CHUNK" in refresh, (
    "agents_session_refresh_counts must bound its per-file read bus hold"
)
assert "taskYIELD" in refresh, (
    "agents_session_refresh_counts must yield the bus between chunks"
)

# --- radio side: RadioLib hal transaction hooks carry the lock ---------------
assert "class LockedArduinoHal : public ArduinoHal" in mc_target, (
    "radio SPI must be covered via the hal hooks (vendor _lock_callback shape)"
)
hal = fn_body(mc_target, "class LockedArduinoHal", "\n};")
begin_hook = hal[hal.index("spiBeginTransaction") :]
begin_hook = begin_hook[: begin_hook.index("}")]
assert "hw_spi_bus_lock()" in begin_hook
assert begin_hook.index("hw_spi_bus_lock()") < begin_hook.index(
    "ArduinoHal::spiBeginTransaction()"
), "lock must be taken before the SPI transaction opens"
end_hook = hal[hal.index("spiEndTransaction") :]
end_hook = end_hook[: end_hook.index("}")]
assert "hw_spi_bus_unlock()" in end_hook
assert end_hook.index("ArduinoHal::spiEndTransaction()") < end_hook.index(
    "hw_spi_bus_unlock()"
), "unlock must follow the transaction close"
assert "new Module(s_hal," in mc_target, (
    "the Module must be built on the locking hal, or the hooks never run"
)

print("spi lock tests: OK")
