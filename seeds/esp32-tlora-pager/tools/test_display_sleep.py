#!/usr/bin/env python3
"""Static regressions for ST7796 panel sleep: the controller must enter
sleep-in (SLPIN) when the backlight blanks and leave it (SLPOUT + 120 ms
settle) before anything repaints or the backlight rises. Mirrors the vendor
LilyGoLib sleep()/wakeup() pair and MeshCore's _isOn idempotence guard."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
hw_ui = (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")
hw_ui_h = (ROOT / "src" / "hw_ui.h").read_text(encoding="utf-8")
backlight = (ROOT / "src" / "skills" / "backlight.cpp").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

# --- mechanics: SLPIN/SLPOUT through the file's own command idiom -------------
assert "void tft_sleep()" in hw_ui, "panel sleep entry point missing"
assert "void tft_wake()" in hw_ui, "panel wake entry point missing"

sleep_fn = hw_ui[hw_ui.index("void tft_sleep()") :]
sleep_fn = sleep_fn[: sleep_fn.index("void tft_wake()")]
assert "tft_cmd(0x10)" in sleep_fn, (
    "SLPIN must go through tft_cmd (the CS + transaction command path), "
    "not a hand-rolled SPI sequence"
)
assert "delay(5)" in sleep_fn, "ST7796 needs 5 ms after SLPIN before commands"

wake_fn = hw_ui[hw_ui.index("void tft_wake()") :]
wake_fn = wake_fn[: wake_fn.index("static void tft_window")]
assert "tft_cmd(0x11)" in wake_fn, (
    "SLPOUT must go through tft_cmd like every other panel command"
)
# s4 (C3): the ~120 ms settle is sliced, pumping the sound queue between
# slices, so a notify cue that woke a blanked panel does not gap for the
# whole SLPOUT wait. The total stays 120 ms and the wait still runs with the
# bus lock released (test_spi_lock.py pins the lock scope).
assert "delay(120)" not in wake_fn, (
    "the monolithic 120 ms settle starves the sound DMA — it must be sliced"
)
assert "waited < 120" in wake_fn and "waited += 10" in wake_fn, (
    "the sliced settle must still total the ST7796's 120 ms"
)
assert "delay(10);" in wake_fn and "hw_sound_poll();" in wake_fn, (
    "each slice must pump the sound queue"
)
assert wake_fn.index("tft_cmd(0x11)") < wake_fn.index("delay(10);") < wake_fn.index(
    "hw_sound_poll();"
) < wake_fn.index("panel_awake = true;"), (
    "wake order: SLPOUT, then the sliced settle with sound pumps, then the "
    "state flip"
)
assert '#include "hw_sound.h"' in hw_ui, (
    "the settle pump needs the sound module's public surface"
)

# --- the command path itself is the manual-CS transaction discipline ----------
cmd_fn = hw_ui[hw_ui.index("static void tft_cmd(uint8_t c)") :]
cmd_fn = cmd_fn[: cmd_fn.index("static void tft_data(")]
assert "digitalWrite(PIN_DISP_CS, LOW)" in cmd_fn
assert "beginTransaction" in cmd_fn and "endTransaction" in cmd_fn
assert "digitalWrite(PIN_DISP_CS, HIGH)" in cmd_fn, (
    "CS must be released after every command — the SX1262 shares this bus"
)

# --- idempotence guard (MeshCore's _isOn) -------------------------------------
assert "static bool panel_awake" in hw_ui
assert "if (!panel_ok || !panel_awake) return;" in sleep_fn, (
    "tft_sleep must be a no-op when already asleep (or panel missing)"
)
assert "if (!panel_ok || panel_awake) return;" in wake_fn, (
    "tft_wake must be a no-op when already awake (or panel missing)"
)
assert "panel_awake = false;" in sleep_fn
assert "panel_awake = true;" in wake_fn
init_fn = hw_ui[hw_ui.index("static bool panel_begin()") :]
init_fn = init_fn[: init_fn.index("// ---- clock field cache")]
assert "panel_awake = true;" in init_fn, (
    "panel_begin ends with SLPOUT + display on: the awake flag must agree"
)

# --- public surface -----------------------------------------------------------
assert "void tft_sleep();" in hw_ui_h and "void tft_wake();" in hw_ui_h, (
    "hw_ui.h must expose the sleep pair to the backlight policy"
)

# --- policy hook: sleep on the 0-transition, wake on the rise -----------------
poll = backlight[backlight.index("static void backlight_poll()") :]
poll = poll[: poll.index("static void backlight_menu_click()")]
assert "tft_sleep();" in poll, "blanking must sleep the panel controller"
assert "tft_wake();" in poll, "leaving blank must wake the panel controller"

blank = poll[poll.index("if (shown == 0)") : poll.index("} else if (bl_applied == 0)")]
assert "kb_bl_follow_blank();" in blank, "keyboard light must die with the panel"
assert "bl_drive(0);" in blank and "tft_sleep();" in blank
assert blank.index("kb_bl_follow_blank();") < blank.index("bl_drive(0);") < blank.index("tft_sleep();"), (
    "blank order: keys off, panel backlight to 0, then the controller sleeps"
)

wake = poll[poll.index("} else if (bl_applied == 0)") :]
wake = wake[: wake.index("} else {")]
assert (
    wake.index("tft_wake();")
    < wake.index("ui_blank_wake_repaint();")
    < wake.index("bl_drive(shown);")
    < wake.index("kb_bl_follow_wake();")
), (
    "wake order (vendor order): SLPOUT + settle, repaint, raise the panel "
    "backlight, then the keys — the lit panel must never show a stale frame"
)

# --- s5 (C3): boot with a saved level 0 sleeps the panel controller ----------
begin = backlight[backlight.index("static void backlight_begin()") :]
begin = begin[: begin.index("static void skill_backlight_init()")]
assert "if (bl_wanted == 0) tft_sleep();" in begin, (
    "a saved backlight level 0 must not leave the ST7796 awake under a dark "
    "screen at boot"
)
assert begin.index("bl_drive(bl_wanted)") < begin.index(
    "if (bl_wanted == 0) tft_sleep();"
), "sleep only after the backlight drive settled at 0"

# --- the pure host-tested region stays hardware-free --------------------------
pure = backlight[: backlight.index("/* --- state --- */")]
assert "tft_sleep" not in pure and "tft_wake" not in pure, (
    "bl_idle_level and friends are host-tested pure arithmetic: no panel calls"
)

# --- the wake repaint helper exists and does a full clock redraw --------------
repaint = main[main.index("static void ui_blank_wake_repaint() {") :]
repaint = repaint[: repaint.index("// --- Front-panel state")]
assert "hw_ui_invalidate_clock();" in repaint and "ui_clock_paint(NULL);" in repaint, (
    "the post-wake repaint must be a full clock redraw, not a field tick"
)

print("display sleep tests: OK")
