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
