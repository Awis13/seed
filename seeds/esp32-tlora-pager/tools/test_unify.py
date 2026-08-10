#!/usr/bin/env python3
"""Static regressions for the C8 pattern unification.

Poll dispatch: polls whose order provably does not matter run through the
Skill.tick dispatcher (notify: expiry/snapshot only raise flags consumed next
pass; progress: reaping updates a snapshot read at 1 Hz / 30 s scales). Polls
whose position in loop() carries an ordering constraint stay hand-placed and
say so in an ORDER comment (backlight before the arrival block — C3 wake
order; reply upstream after the keyboard drain; the sound DMA refill before
the 1 Hz clock repaint). reply_upstream and hw_sound are not Skills, so the
dispatcher was never an option for them.

Persistence: every small settings file that must never be half-written goes
through write_spiffs_file_atomic, and every tmp name is unique across the
firmware (a shared tmp name would let two writers rename each other's data).
"""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
notify = (ROOT / "src" / "skills" / "notify.cpp").read_text(encoding="utf-8")
progress = (ROOT / "src" / "skills" / "progress.cpp").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
backlight = (ROOT / "src" / "skills" / "backlight.cpp").read_text(encoding="utf-8")
meshcore = (ROOT / "src" / "skills" / "meshcore.cpp").read_text(encoding="utf-8")
wg = (ROOT / "src" / "skills" / "wg.cpp").read_text(encoding="utf-8")
gps = (ROOT / "src" / "skills" / "gps.cpp").read_text(encoding="utf-8")

SKILLS = {
    "notify": notify,
    "progress": progress,
    "agents": agents,
    "backlight": backlight,
    "meshcore": meshcore,
    "wg": wg,
    "gps": gps,
}

# --- 1. order-free polls run through the tick dispatcher ---------------------
assert ".tick = notify_poll" in notify, (
    "notify expiry + coalesced snapshot are order-free: dispatcher, not loop()"
)
assert ".tick = progress_poll" in progress, (
    "progress reaping is order-free: dispatcher, not loop()"
)
loop = main[main.index("void loop()") :]
assert "notify_poll();" not in loop, "notify_poll must not be hand-called too"
assert "progress_poll();" not in loop, (
    "progress_poll must not be hand-called too"
)

# --- 2. order-bound polls stay hand-placed, constraint written down ----------
assert "backlight_poll();" in loop, (
    "backlight_poll is order-bound (C3): it must precede the arrival block"
)
assert loop.index("backlight_poll();") < loop.index("uint32_t arrived_id = 0;"), (
    "C3 wake order: the panel must wake before an arriving card paints"
)
assert loop.index("ui_backlight_idle();") < loop.index("backlight_poll();"), (
    "the idle decision must precede the drive that applies it"
)
assert "reply_upstream_poll();" in loop, (
    "the reply uplink is not a Skill; it stays pinned after the keyboard drain"
)
assert loop.index("hw_kb_read") < loop.index("reply_upstream_poll();"), (
    "an Enter must be carried on the pass that produced it"
)
assert loop.count("hw_sound_poll();") == 2, (
    "both sound pumps are deliberate: cue prime + refill before the clock paint"
)
assert loop.count("ORDER: hand-placed") == 3, (
    "each hand-placed poll (backlight, reply upstream, sound refill) must "
    "state its ordering constraint"
)
# The refill pump stays ahead of the 1 Hz clock repaint.
assert loop.rindex("hw_sound_poll();") < loop.index("ui_clock_paint(NULL);",
                                                    loop.index("last_clock")), (
    "a long clock repaint must not starve an in-flight cue"
)
# The PING state machine stays after the tick dispatcher (C5).
assert loop.index("g_skills[i]->tick") < loop.index("ui_mesh_ping_poll();"), (
    "meshcore's tick must get the pong before the PING machine reads it"
)

# --- 3. every Skill initializer spells out .tick -----------------------------
for name, text in SKILLS.items():
    # "static const Skill " (with the space) so the SkillEndpoint arrays
    # earlier in each file cannot shadow the initializer.
    blk = text[text.index("static const Skill ") :]
    blk = blk[: blk.index("};")]
    assert ".tick" in blk, f"{name}: Skill initializer must spell out .tick"

# --- 4. atomic small-file writers --------------------------------------------
assert 'write_spiffs_file_atomic(KB_LAYOUT_PATH, KB_LAYOUT_TMP' in main, (
    "the keyboard layout must persist via tmp+rename"
)
assert '#define KB_LAYOUT_TMP     "/kb_layout.tmp"' in main
assert 'SPIFFS.open(KB_LAYOUT_PATH, "w")' not in main, (
    "the raw layout writer must be gone"
)
assert 'write_spiffs_file_atomic(AGENT_BRIDGE_FILE, AGENT_BRIDGE_TMP' in agents, (
    "the agents bridge URL must persist via tmp+rename"
)
assert '#define AGENT_BRIDGE_TMP    "/agent_bridge.tmp"' in agents
assert 'SPIFFS.open(AGENT_BRIDGE_FILE, "w")' not in agents, (
    "the raw bridge writer must be gone"
)

# --- 5. tmp names are unique across the firmware -----------------------------
sources = [main, notify, progress, agents, backlight, meshcore, wg, gps,
           (ROOT / "src" / "hw_ui.cpp").read_text(encoding="utf-8")]
tmps = []
for text in sources:
    tmps += re.findall(r'"(/[A-Za-z0-9_.]+\.tmp)"', text)
assert len(tmps) == len(set(tmps)), (
    f"duplicate tmp file names: {sorted(t for t in tmps if tmps.count(t) > 1)}"
)
expected = {"/gw_token.tmp", "/notify.tmp", "/gps.tmp", "/backlight.tmp",
            "/kb_layout.tmp", "/agent_bridge.tmp"}
assert set(tmps) == expected, f"unexpected tmp set: {sorted(set(tmps))}"

print("unify pattern tests: OK")
