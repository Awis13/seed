#!/usr/bin/env python3
"""Static regressions for the non-blocking GNSS duty cycle."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
gps = (ROOT / "src" / "skills" / "gps.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")

assert "#define GPS_ACQUIRE_INTERVAL_MS 1200000UL" in gps
assert "#define GPS_ACQUIRE_TIMEOUT_MS   120000UL" in gps
assert "GPS_SAVE_DELAY_MS" not in gps
assert "gps_fix = false;" not in gps[gps.index("static bool gps_parse_rmc"):
                                      gps.index("static bool gps_parse_gga")], (
    "a no-fix acquisition must not discard the persisted last-known position"
)
gga = gps[gps.index("static bool gps_parse_gga"):
          gps.index("static void gps_parse_sentence")]
assert "if (q <= 0 || q > 6) return false;" in gga, (
    "a failed acquisition must not overwrite last-valid sats/HDOP metadata"
)

tick = gps[gps.index("static void gps_tick()") : gps.index("/* ---- HTTP ---- */")]
assert "gps_wake_requested" in tick
assert "millis() - gps_last_cycle_ms >= GPS_ACQUIRE_INTERVAL_MS" in tick
assert "millis() - gps_acquire_started_ms >= GPS_ACQUIRE_TIMEOUT_MS" in tick
assert "gps_drain()" in tick
assert "delay(" not in tick, "periodic GPS work must not block loop()"

stop = gps[gps.index("static void gps_stop_acquisition") : gps.index("static void gps_load")]
assert "gps_save()" in stop
assert "Serial1.end()" in stop
assert "gps_power(false)" in stop

age_start = gps.index("long gps_fix_age_s(void) {")
age = gps[age_start : gps.index("float gps_get_hdop(void) {", age_start)]
assert "gps_fix_received_this_boot" in age
assert "millis() - gps_fix_received_ms" in age

route = gps[gps.index('exact("/gps/fix")') : gps.index("static const Skill gps_skill")]
assert "gps_request_fix()" in route, "a stale HTTP request must wake GNSS"

intercept = agents[agents.index("static bool agents_gps_intercept") :
                   agents.index("static void agents_gps_on_fix")]
assert "if (!fresh) gps_request_fix()" in intercept

init = gps[gps.index("static void skill_gps_init") :]
assert "gps_start_acquisition()" in init, "boot must still attempt an initial fix"

print("GPS duty-cycle policy tests: OK")
