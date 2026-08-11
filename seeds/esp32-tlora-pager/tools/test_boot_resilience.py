#!/usr/bin/env python3
"""Static regressions for boot resilience.

Live incident: the device arrived with foreign firmware that had repartitioned
flash; SPIFFS.begin(true) hit mount error -10025 and silently formatted the
8 MB partition — dark panel, mute serial, indistinguishable from a brick.
A separate boot panicked with a stack canary on the ipc1 task (fixed 1024-byte
stack baked into the prebuilt core) with no coredump space and no trace.

Locked-in shapes:
  - Panel comes up BEFORE storage; mount is split from format; both the format
    start and its outcome are announced on serial and painted on the panel.
  - A failed format degrades (storage_ok=false) instead of hanging or looping.
  - The format never runs under the shared SPI bus lock (SPIFFS is internal
    flash, not the bus); the boot-note paint is a guarded public entry.
  - Reset reason + crash counters (RTC_NOINIT) are printed at boot and served
    in GET /health: reset_reason, boots_since_panic, panic_count, storage_ok.
  - Prime-suspect mitigation stays pinned: the WiFi driver's redundant NVS
    credential store is off (WiFi.persistent(false) before the first mode()),
    and wifi_persist_profiles() skips byte-identical rewrites.
"""

from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
hw_ui = (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")
hw_ui_h = (ROOT / "src" / "hw_ui.h").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    """Slice from a function signature to its closing brace at column 0."""
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


# --- 1. mount is split from format; format-on-fail is gone -------------------
assert "SPIFFS.begin(true)" not in main, (
    "format-on-fail mounts silently for the whole 8 MB format — mount with "
    "begin(false) and announce the format instead"
)
storage = fn_body(main, "static void storage_begin()")
assert "SPIFFS.begin(false)" in storage, "mount must be attempted without format"
assert "SPIFFS.format()" in storage, "explicit format on mount failure"
assert storage.index("SPIFFS.begin(false)") < storage.index("SPIFFS.format()"), (
    "mount attempt must come before any format"
)
# the format is followed by a re-mount, also without format-on-fail
tail = storage[storage.index("SPIFFS.format()") :]
assert "SPIFFS.begin(false)" in tail, "re-mount after format must not format again"

# --- 2. the format is announced before it starts, on serial and panel --------
assert "storage invalid, formatting" in storage, (
    "serial must say the format is starting (this is the silent-brick window)"
)
assert "FORMATTING STORAGE" in storage, "panel must show the format is running"
assert storage.index("storage invalid, formatting") < storage.index("SPIFFS.format()")
assert storage.index("FORMATTING STORAGE") < storage.index("SPIFFS.format()")
# outcome is announced too, both ways
assert "STORAGE FAILED" in storage, "format failure must be announced"
assert storage.index("SPIFFS.format()") < storage.index("STORAGE FAILED")

# --- 3. failure degrades: no hang, no reset loop -----------------------------
for marker in ("while (true)", "while(true)", "for (;;)", "for(;;)",
               "ESP.restart()", "esp_restart(", "abort("):
    assert marker not in storage, (
        f"storage_begin must continue degraded on failure, found {marker!r}"
    )
assert "storage_ok" in storage, "the outcome must be recorded for /health"

# --- 4. no shared-bus lock across the format wait ----------------------------
assert "HwSpiBusGuard" not in storage and "hw_spi_bus_lock" not in storage, (
    "SPIFFS is internal flash, not the shared SPI bus — the multi-second "
    "format must never run with the bus lock held"
)

# --- 5. panel before storage: the format note has somewhere to paint ---------
setup = fn_body(main, "void setup()")
assert setup.index("hw_ui_begin()") < setup.index("storage_begin()"), (
    "hw_ui_begin() must run before storage so the format is visible"
)
# every SPIFFS consumer stays after the mount
for loader in ("backlight_begin()", "kb_layout_load()", "mesh_gw_load()",
               "gw_token_load()", "tz_load()", "token_load()"):
    assert setup.index("storage_begin()") < setup.index(loader), (
        f"{loader} reads SPIFFS and must stay after storage_begin()"
    )

# --- 6. boot note is a guarded public paint entry ----------------------------
assert "void hw_ui_boot_note(const char *line1, const char *line2);" in hw_ui_h
note = fn_body(hw_ui, "void hw_ui_boot_note(")
assert "if (!panel_ok) return;" in note, "must be a no-op before hw_ui_begin"
assert "HwSpiBusGuard" in note, "boot note paints without the bus lock"
assert note.index("HwSpiBusGuard") < note.index("tft_"), (
    "the lock must be taken before the first paint call"
)

# --- 7. reset reason + crash counters surface at boot and in /health ---------
assert "esp_reset_reason()" in main, "reset reason must be read at boot"
assert main.count("RTC_NOINIT_ATTR") >= 3, (
    "crash counters live in RTC noinit RAM (magic + boots_since_panic + "
    "panic_count) so they survive every reset except power-on"
)
diag = fn_body(main, "static void boot_diag_init()")
assert "Serial.printf" in diag, "reset reason must be printed at boot"
health = fn_body(main, "static void handle_health(")
for field in ("reset_reason", "boots_since_panic", "panic_count", "storage_ok"):
    assert f'doc["{field}"]' in health, f"/health must serve {field}"
# History archive observability (ticket C4): the store tier, drop counter and the
# indexed-identity count must surface on /health so the SD archive is verifiable
# live. Read-only — no store selector, no behaviour change.
for field in ("history_sd", "history_drops", "history_records"):
    assert f'doc["{field}"]' in health, f"/health must serve {field}"
# Queue health (hardening): a DEAD write queue (boot alloc failed => nothing
# persists) and the live queue occupancy (early overflow warning) must both be
# visible, so a queue that silently drops everything is not mistaken for healthy.
for field in ("history_ready", "history_queued"):
    assert f'doc["{field}"]' in health, f"/health must serve {field}"
assert "history_ready()" in health, "history_ready must come from the history_ready() getter"
assert "history_queued()" in health, "history_queued must come from the history_queued() getter"
assert "history_on_sd()" in health, "history_sd must come from the history_on_sd() getter"
assert "history_index_live_count()" in health, (
    "history_records must be the RAM index count (history_index_live_count), not an SD scan"
)
assert 'boot_diag_init()' in setup, "counters must be initialized in setup()"

# --- 7b. s10 (C6): full S3 reset-reason coverage and its panic classing ------
# The vendored IDF header (esp_system.h, ESP32-S3) also defines USB, JTAG,
# EFUSE, PWR_GLITCH and CPU_LOCKUP; every one must map to a readable string.
rr = fn_body(main, "static const char *reset_reason_str(")
for enum, name in (("ESP_RST_USB", '"usb"'), ("ESP_RST_JTAG", '"jtag"'),
                   ("ESP_RST_EFUSE", '"efuse"'),
                   ("ESP_RST_PWR_GLITCH", '"pwr_glitch"'),
                   ("ESP_RST_CPU_LOCKUP", '"cpu_lockup"')):
    assert enum in rr and name in rr, f"{enum} must map to {name}"
panic = fn_body(main, "static bool reset_is_panic(")
for fault in ("ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT",
              "ESP_RST_CPU_LOCKUP", "ESP_RST_PWR_GLITCH"):
    assert fault in panic, (
        f"{fault} is an unexpected fault and must count as panic-class"
    )
# The interrupt/task watchdogs are genuine firmware hangs and stay panic-class.
# The generic ESP_RST_WDT is ambiguous: BOTH esptool's --after watchdog_reset on
# every plain reflash (board_upload.after_reset in platformio.ini) AND a genuine
# early RTC-watchdog hang before the task/int watchdogs exist. It is therefore
# NOT in reset_is_panic's unconditional set — it gets a coredump-gated decision
# in boot_diag_init instead (the coredump-aware path shipped, since coredump-to-
# flash is live in the precompiled Arduino-ESP32 esp32s3 libs).
assert "ESP_RST_WDT" not in panic, (
    "ESP_RST_WDT is ambiguous (flash-tool reset vs early RTC hang) and must NOT "
    "be in reset_is_panic's unconditional set — it is coredump-gated in "
    "boot_diag_init"
)
for clean in ("ESP_RST_USB", "ESP_RST_JTAG", "ESP_RST_EFUSE"):
    assert clean not in panic, (
        f"{clean} must stay out of the panic classification"
    )

# --- 7c. ESP_RST_WDT is coredump-gated in boot_diag_init ---------------------
# A flash-tool watchdog_reset never leaves a coredump, so it must NOT bump
# panic_count; a real early RTC-WDT hang that reached the panic handler does,
# so it still counts. The discriminator is a valid coredump image in flash.
diag = fn_body(main, "static void boot_diag_init()")
assert "ESP_RST_WDT" in diag, (
    "boot_diag_init must special-case ESP_RST_WDT rather than exclude it blindly"
)
assert "coredump_image_present()" in diag, (
    "the ESP_RST_WDT decision must be gated on coredump presence"
)
cd = fn_body(main, "static bool coredump_image_present()")
assert "esp_core_dump_image_check()" in cd and "ESP_OK" in cd, (
    "coredump presence must come from esp_core_dump_image_check() == ESP_OK"
)

# --- 8. ipc1 prime-suspect mitigation stays pinned ---------------------------
wifi_setup = fn_body(main, "static void wifi_setup()")
assert "WiFi.persistent(false)" in wifi_setup, (
    "the WiFi driver's NVS credential store is redundant (/wifi.json is the "
    "source of truth) and its core-0 flash writes stall core 1 through the "
    "1024-byte ipc1 task — the prime suspect for the live canary panic"
)
assert wifi_setup.index("WiFi.persistent(false)") < wifi_setup.index("WiFi.mode(WIFI_STA)"), (
    "persistent(false) only reaches wifiLowLevelInit if set before the first "
    "WiFi.mode() call"
)
persist = fn_body(main, "static bool wifi_persist_profiles()")
assert "read_spiffs_file(WIFI_CONFIG_FILE) == json" in persist, (
    "wifi_persist_profiles must skip byte-identical rewrites — every boot "
    "connect otherwise burns a flash write in the busiest window"
)

print("boot resilience tests: OK")
