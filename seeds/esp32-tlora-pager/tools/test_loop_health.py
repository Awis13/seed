#!/usr/bin/env python3
"""Static regressions for the per-skill / whole-pass loop timing in /health.

The loop task may carry blocking calls (a wg HTTP probe, reply_upstream_poll on
Enter, SPIFFS writes, SPI) heavier than any single "worst pass" number reveals,
because that number is the SUM of every skill in a pass and gets misattributed
to whoever measured last. This commit instruments each tick/poll entry with a
rolling-max microsecond timer (esp_timer_get_time) and surfaces the breakdown in
GET /health, so a specific blocker becomes provable before it can be bounded.
It is MEASUREMENT ONLY: the timers add no delay and change no control flow.

Cross-task safety mirrors rns.cpp's drain_us_max: every max is WRITTEN only on
the loop task (single writer, in loop()) and READ on the AsyncTCP task by
handle_health() with no lock — a torn read of one uint32 is at worst one stale
sample. GET /health?reset=1 zeroes all of them (and only them) after the
response is serialized.

These asserts pin the shape so a later edit cannot drop the per-skill breakdown,
stop timing a poll, move the reset into the wrong task, or relabel the pass max.
"""

from pathlib import Path

ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def slice_between(text, start, end, what):
    i = text.index(start)
    j = text.index(end, i + len(start))
    assert i < j, f"could not slice {what}"
    return text[i:j]


loop = main[main.index("void loop()"):]
health = slice_between(
    main, "static void handle_health", "static void handle_capabilities",
    "handle_health",
)

# --- version bump so /health distinguishes this build ------------------------
assert '#define SEED_VERSION        "0.9.52"' in main, (
    "SEED_VERSION must bump to 0.9.52 for this build"
)

# --- 1. instrumentation primitives are present, single-writer helper ---------
assert "#include <esp_timer.h>" in main, (
    "esp_timer_get_time() needs its header included"
)
assert "static inline void loop_time_note(uint32_t *slot, uint64_t t0)" in main, (
    "the rolling-max helper must exist and take a slot + a start timestamp"
)
assert "if (dt > *slot) *slot = dt;" in main, (
    "loop_time_note must keep a rolling MAX, not overwrite"
)
assert "static uint32_t g_skill_tick_us_max[MAX_SKILLS] = {0};" in main, (
    "per-skill maxes must be a fixed-size static indexed by skill index"
)
assert "static uint32_t g_tick_poll_us_max[LP_COUNT] = {0};" in main, (
    "hand-placed poll maxes must be a fixed-size static keyed by label"
)
assert "static uint32_t g_loop_pass_us_max = 0;" in main, (
    "the whole-pass rolling max must exist"
)

# --- 2. whole-pass region is wrapped start -> end ----------------------------
assert "uint64_t _loop_pass_t0 = esp_timer_get_time();" in loop, (
    "the instrumented region must open with a pass-start timestamp"
)
assert "loop_time_note(&g_loop_pass_us_max, _loop_pass_t0);" in loop, (
    "the instrumented region must close by noting the whole-pass max"
)
# The pass timer opens before the first poll and closes after the last one.
assert loop.index("_loop_pass_t0 = esp_timer_get_time();") < loop.index(
    "wifi_reconnect_poll();"), (
    "the pass timer must start before the first instrumented poll"
)
assert loop.index("loop_time_note(&g_loop_pass_us_max, _loop_pass_t0);") > loop.index(
    "ui_mesh_ping_poll();"), (
    "the pass timer must close after the last instrumented poll"
)

# --- 3. every hand-placed poll is wrapped by a timing capture ----------------
# Each poll keeps its exact original call statement, framed by a t0 taken just
# before and a rolling-max note just after into its labelled slot.
POLLS = [
    ("wifi_reconnect_poll();", "_t_wifi",  "LP_WIFI_RECONNECT"),
    ("hw_input_poll();",       "_t_input", "LP_HW_INPUT"),
    ("backlight_poll();",      "_t_bl",    "LP_BACKLIGHT"),
    ("reply_upstream_poll();", "_t_reply", "LP_REPLY_UPSTREAM"),
    ("ui_mesh_ping_poll();",   "_t_ping",  "LP_UI_MESH_PING"),
]
for call, t0, label in POLLS:
    assert f"uint64_t {t0} = esp_timer_get_time();" in loop, (
        f"{call} must take a start timestamp in {t0}"
    )
    assert f"loop_time_note(&g_tick_poll_us_max[{label}], {t0});" in loop, (
        f"{call} must record its rolling max into {label}"
    )
    # start -> call -> note, in that order and adjacent.
    ci = loop.index(call)
    assert loop.index(f"uint64_t {t0} =") < ci < loop.index(
        f"loop_time_note(&g_tick_poll_us_max[{label}], {t0});"), (
        f"{call} must sit between its t0 and its note"
    )

# hw_sound has TWO call sites (cue prime + refill); both feed the same slot.
assert loop.count("loop_time_note(&g_tick_poll_us_max[LP_HW_SOUND],") == 2, (
    "both hw_sound_poll sites must be timed into the shared LP_HW_SOUND slot"
)
assert "uint64_t _t_snd = esp_timer_get_time();" in loop, "first hw_sound site timed"
assert "uint64_t _t_snd2 = esp_timer_get_time();" in loop, "second hw_sound site timed"

# --- 4. the dispatcher loop times per skill, keyed by index ------------------
dispatch = slice_between(
    loop, "for (int i = 0; i < g_skill_count; i++) {", "ui_mesh_ping_poll();",
    "skill tick dispatcher",
)
assert "if (g_skills[i]->tick) {" in dispatch, (
    "the dispatcher form must stay: guard then call (C8 unification)"
)
assert "uint64_t _t_skill = esp_timer_get_time();" in dispatch, (
    "each skill tick must be timed"
)
assert "g_skills[i]->tick();" in dispatch, "the skill tick call must remain"
assert "loop_time_note(&g_skill_tick_us_max[i], _t_skill);" in dispatch, (
    "each skill tick must record its rolling max into its per-index slot"
)

# --- 5. /health emits the pass max and a per-entry tick_us object ------------
assert 'doc["loop_pass_us_max"] = (unsigned long)g_loop_pass_us_max;' in health, (
    "/health must emit the whole-pass max"
)
assert 'JsonObject tick = doc["tick_us"].to<JsonObject>();' in health, (
    "/health must emit a tick_us object mapping label -> max µs"
)
# Skills by name, then the hand-placed polls by label.
assert "tick[g_skills[i]->name] = (unsigned long)g_skill_tick_us_max[i];" in health, (
    "tick_us must key each skill by its name"
)
assert "tick[g_loop_poll_names[i]] = (unsigned long)g_tick_poll_us_max[i];" in health, (
    "tick_us must include the hand-placed polls by label"
)
# The label table lists every hand-placed poll.
for name in ("wifi_reconnect", "hw_input", "backlight", "reply_upstream",
             "hw_sound", "ui_mesh_ping"):
    assert f'"{name}"' in main, f"the poll label {name!r} must be defined"

# --- 6. ?reset=1 zeroes ALL maxes AFTER serialize, and only in the handler ---
assert health.index("serializeJson(doc, response);") < health.index(
    'request->hasParam("reset")'), (
    "the reset must run after the response is serialized (value captured first)"
)
reset = health[health.index('request->hasParam("reset")'):]
assert "g_loop_pass_us_max = 0;" in reset, "reset must zero the whole-pass max"
assert "for (int i = 0; i < LP_COUNT; i++) g_tick_poll_us_max[i] = 0;" in reset, (
    "reset must zero every hand-placed poll max"
)
assert "for (int i = 0; i < MAX_SKILLS; i++) g_skill_tick_us_max[i] = 0;" in reset, (
    "reset must zero every per-skill max"
)
# The reset lives ONLY in the handler — the loop task never zeroes these.
assert "g_loop_pass_us_max = 0;" not in loop, (
    "only the /health handler may reset the maxes; the loop task never zeroes"
)

print("loop health tests: OK")
