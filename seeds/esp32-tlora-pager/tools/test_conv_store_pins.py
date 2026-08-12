#!/usr/bin/env python3
"""Static pins for the conversation store's FIRMWARE halves in
skills/agents.cpp — the parts tools/test_conv_store.cpp cannot reach because
they need the FS, the mutex and the SPI bus.

WHY THESE EXIST
---------------
The pure header supports a ROOM-LESS conversation: one keyed on its address
alone, with no session component, which is the shape a mesh/LXMF peer gets. The
host test exercises that shape and passes. But a green header test says nothing
about whether the firmware can actually persist and reload such a record, and
for a while it could not: the loader dropped any line with an empty session and
the writer emitted nothing for a conversation with no rooms. The store could
therefore describe a peer on disk and silently forget it on the next boot, with
the host suite fully green — a false green on the exact seam peer registration
is built on.

So the two halves are pinned here by source shape:
  1. the writer emits a conversation-level line when a conversation has no rooms;
  2. the loader applies a room-less line instead of discarding it, and applies
     the conversation's own fields BEFORE it branches on the room — that
     ordering IS the fix, so it is asserted as an ordering, not as presence;
  3. the boot-time key migration counts what it could not move, so a card whose
     history ends up split across both key namespaces says so.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    """Slice a function DEFINITION.

    The guard is not decoration: agents.cpp forward-declares several of these
    near the top, and `text.index(sig)` happily lands on the declaration. The
    slice then runs from there to the end of some unrelated later function, and
    an assertion against it can pass on code that has nothing to do with the
    function under test — a pin that looks green and checks nothing. So a slice
    whose head reaches a ';' before its first '{' is rejected outright.
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


# --- 1. the writer emits a line for a conversation with no rooms -------------
persist = fn_body(agents, "static void agents_manifest_persist()")
assert "a.n_sessions == 0" in persist, (
    "agents_manifest_persist must have a branch for a conversation with no "
    "rooms — without it a peer conversation is never written and vanishes on "
    "the next boot"
)
# That branch must write a real record with an EMPTY session field, not skip.
roomless = persist[persist.index("a.n_sessions == 0") :]
roomless = roomless[: roomless.index("for (int j")]
assert "conv_manifest_format(" in roomless, (
    "the room-less branch must emit a manifest record, not merely continue"
)
assert re.search(r'conv_manifest_format\([^;]*?a\.id,\s*""', roomless, re.S), (
    "the room-less record must carry an empty session field"
)

# --- 2. the loader keeps a room-less line ------------------------------------
apply_fn = fn_body(agents, "static void agents_manifest_apply(const ConvManifestLine &ml)")
assert "if (idx < 0) return;" in apply_fn, (
    "an unknown conversation is still skipped — the loader creates nothing"
)
# THE LOADER MUST NOT MINT. /conversations.txt is on a card a user can edit and
# a minted conversation is not seeded, so conv_route_resolve would take its
# transport and reply address from that file verbatim — a hand-written line
# could conjure a conversation whose replies go wherever it says, which is the
# seeded-route re-pointing rule defeated through a different door. The legacy
# manifest makes it concrete: it can still name retired agent ids.
# Checked against the CODE, comments stripped: an absence assertion that reads
# comments too would fire on a comment merely naming the function (and, worse,
# a presence assertion would be satisfied by a commented-out call).
apply_code = re.sub(r"/\*.*?\*/", " ", apply_fn, flags=re.S)
apply_code = re.sub(r"//[^\n]*", " ", apply_code)
assert "conv_mint(" not in apply_code, (
    "the manifest loader must not mint conversations — a line on a removable "
    "card would then choose where replies are sent"
)
assert "if (ml.transport != CONV_AGENT) return;" in apply_fn, (
    "a non-AGENT manifest line must create nothing: no peer conversation is "
    "created anywhere in this build, so such a line can only be hand-written"
)
assert "agents_find(ml.conv)" in apply_fn, (
    "the loader resolves against the conversations the build already has"
)
# The conversation's own fields must be applied BEFORE the room branch: that
# ordering is the whole fix. A room-less line carries label/transport/reply and
# must not be thrown away for lacking a room.
assert "agents_conv_apply(a, ml);" in apply_fn, (
    "the conversation-level fields must be applied via agents_conv_apply"
)
assert "if (!ml.session[0]) return;" in apply_fn, (
    "a room-less line must return AFTER the conversation fields, not before"
)
assert apply_fn.index("agents_conv_apply(a, ml);") < apply_fn.index(
    "if (!ml.session[0]) return;"
), (
    "agents_manifest_apply must apply the conversation's label/transport/reply "
    "BEFORE branching on the room — returning first is exactly the bug that made "
    "a room-less conversation unloadable"
)
# The old shape must not come back: a combined guard that drops the line whole.
assert "if (idx < 0 || !ml.session[0]) return;" not in apply_fn, (
    "the combined guard discards room-less conversations — that is the "
    "regression this pin exists for"
)

# --- 3. the key migration reports what it could not move ---------------------
migrate = fn_body(agents, "static void agents_migrate_logs()")
for counter in ("moved", "skipped", "failed"):
    assert counter in migrate, f"the migration must count {counter}"
# ... and each counter must actually be INCREMENTED somewhere. Asserting only
# that the name appears is a pin that cannot bite: deleting `skipped++` leaves
# the declaration and the report line intact, so the both-files-present split
# would go silent again with this file still green — the very anti-pattern the
# breadcrumb was added to prevent.
assert "skipped++" in migrate, (
    "a skip (both key forms present) must increment the counter, not just "
    "declare it — otherwise the split is reported as 0 and vanishes"
)
assert "failed++" in migrate, (
    "a failed rename must increment the counter, not just declare it"
)
assert "moved++" in migrate, "a successful rename must increment the counter"
assert "event_add(" in migrate, (
    "the migration must leave a breadcrumb in the event log"
)
assert re.search(r"if \(moved \|\| skipped \|\| failed\)", migrate), (
    "a skip (both key forms present) or a failed rename must be reported too, "
    "not just the happy path — that is the case where history looks short"
)
# Non-destructive: the migration must never remove a log file.
assert "remove(" not in migrate, (
    "the migration must never delete a log — a failed rename leaves the legacy "
    "file exactly where it was"
)

# --- 4. the scrollback window is SHARED, not per conversation ----------------
# This is the whole reason the table can hold peers at all: a per-conversation
# 24-message window cost 12,480 of the record's 12,796 bytes, so every new
# conversation spent 12.5 KB of static RAM on scrollback for a chat nobody was
# looking at. A leftover per-conversation view would silently re-inflate that
# while every other pin here stayed green, so its ABSENCE is asserted, not just
# the shared window's presence.
conv_struct = fn_body(agents, "struct Conversation {", "\n};")
assert "AgentLine view[" not in conv_struct, (
    "a conversation must NOT carry its own scrollback window — that is the "
    "12.5 KB per slot that made the table impossible to grow"
)
for gone in ("file_sync", "win_start", "uint8_t vn"):
    assert gone not in conv_struct, (
        f"{gone} is window state and belongs to ConvWindow, not to a conversation"
    )
assert "AgentLine last;" in conv_struct, (
    "a conversation keeps only its newest line, for the picker"
)

win_struct = fn_body(agents, "struct ConvWindow {", "\n};")
assert "AgentLine view[AGENT_VIEW_MAX];" in win_struct, (
    "the shared window holds the scrollback"
)
assert "int      owner;" in win_struct, "the window must know whose it is"
# Exactly one instance of it, or "shared" is a fiction.
assert len(re.findall(r"^static ConvWindow\s+\w+", agents, re.M)) == 1, (
    "there must be exactly one shared window instance"
)
assert not re.search(r"ConvWindow\s+\w+\[", agents), (
    "the window must not become an array — that is the per-conversation cost "
    "coming back under a new name"
)

# --- 5. an arriving message must not steal the window ------------------------
# The chat that is OPEN must keep its scrollback when a line lands for some
# other conversation; otherwise the screen blanks on every inbound message.
append = fn_body(agents, "static void agents_store_append(int idx, bool from_me, const char *text)")
assert "if (g_win.owner == idx) agents_view_append(g_win" in append, (
    "the scrollback may only be appended to when this conversation owns the "
    "window — an unfocused arrival must not touch the open chat"
)
assert "a.last.from_me = from_me;" in append, (
    "every conversation's cached last line must be updated, focused or not"
)
select = fn_body(agents, "bool agents_session_select(int idx, const char *name)")
assert "if (g_win.owner == idx)" in select, (
    "selecting a room on an unfocused conversation must not reload the window "
    "out from under the chat the user is reading — the off-loop route drain "
    "does exactly that"
)

# EVERY window-ownership guard, one assert each. These were found the hard way:
# the route drain below was a fourth steal site that no pin covered, so the
# rule is now that each guard is named individually rather than sampled.
route = fn_body(agents, "static void agents_route_task(void *arg)")
assert "else if (g_win.owner == idx) agents_thread_goto_tail(idx);" in route, (
    "an arriving reply must re-pin the tail ONLY when it already owns the "
    "window: goto_tail is the focus point, so calling it unconditionally hands "
    "the window to whichever message just landed and takes the user's scroll "
    "position away mid-read"
)
sync = fn_body(agents, "static void agents_sync_view(int idx)")
assert "if (g_win.owner != idx) agents_window_take(idx);" in sync, (
    "syncing a conversation the window is not loaded for would fold its lines "
    "into someone else's scrollback"
)
view = fn_body(agents, "int agents_thread_view(")
assert "if (g_win.owner != idx) agents_thread_goto_tail(idx);" in view, (
    "the renderer's lazy focus is not optional: without it a draw for a "
    "conversation the window is not loaded for paints ANOTHER conversation's "
    "messages under this one's header — corrupt, not merely blank"
)
refresh = fn_body(agents, "bool agents_session_refresh_counts(int idx)")
assert "if (g_win.owner == idx) agents_sync_view(idx);" in refresh, (
    "recounting an unfocused conversation must not take the window"
)
clear = fn_body(agents, "static bool agents_clear(const char *agent_id)")
assert clear.count("if (g_win.owner == i) agents_window_take(i);") == 2, (
    "both clear paths (all rooms, active room) must release the window only "
    "when they own it"
)
start_fn = fn_body(agents, "uint32_t agents_thread_start(int idx) {")
assert "g_win.owner == idx" in start_fn, (
    "the scroll position must be reported only for the conversation the "
    "window belongs to, never another's"
)
ts_fn = fn_body(agents, "uint32_t agents_thread_line_ts(int idx, int i) {")
assert "g_win.owner != idx" in ts_fn, (
    "a line timestamp must not be read out of another conversation's window"
)
# The picker reads the cache, never a window that may be parked on an old page.
get_agents = agents[agents.index('server.on(AsyncURIMatcher::exact("/agents"), HTTP_GET'):]
get_agents = get_agents[: get_agents.index("notify_send_json")]
assert 'o["last"] = a.last.text;' in get_agents, (
    "the conversation list must read the per-conversation cached line"
)
assert ".view[" not in get_agents, (
    "the list must not read the shared scrollback — it belongs to whichever "
    "single conversation is on screen"
)

# --- 6. minting and eviction --------------------------------------------------
mint = fn_body(agents, "static int conv_mint(")
assert "int idx = agents_find(id);" in mint and "return idx;" in mint, (
    "mint must FIND first — a peer that speaks twice gets one conversation, "
    "not two"
)
assert "conv_slot_plan(g_conv_n, CONV_MAX, seeded_of, use_of, g_win.owner,\n                         may_evict)" in mint, (
    "slot choice must go through the pure planner AND protect the slot on "
    "screen — g_win.owner is the conversation being read, and recycling it "
    "would swap the reader into a stranger's chat rather than close it"
)
assert "g_win.owner == idx) agents_window_take(-1)" in mint, (
    "evicting the conversation that holds the window must release it"
)
assert "remove(" not in mint and "->remove" not in mint, (
    "eviction must never delete the evicted conversation's history — the slot "
    "is a RAM cache, the log is the conversation"
)
assert re.search(r"#define CONV_MAX\s+8", agents), (
    "the table must actually be able to hold peers"
)

# --- 7. boot does one scan per conversation, and greets only its own doors ----
init = fn_body(agents, "static void skill_agents_init()")
boot_loop = init[init.index("for (int i = 0; i < g_conv_n; i++) {\n        /* One scan each") :]
assert "agents_thread_goto_tail" not in boot_loop, (
    "the boot loop must not rebuild the window after syncing it: a long "
    "history looks non-tail to goto_tail, which rescanned the whole file a "
    "second time, once per conversation"
)
assert "if (g_convs[i].seeded && g_convs[i].lines == 0)" in boot_loop, (
    "the greeting is for the compiled-in doors only — it must never be written "
    "into a peer conversation's history"
)

# --- 8. unread: counted only for a conversation nobody is reading ------------
# A value rule a shape check cannot see: bumping unconditionally would mark the
# chat the user is looking at, and counting our own lines would mark a
# conversation the user just wrote in.
append_fn = fn_body(agents, "static void agents_store_append(int idx, bool from_me, const char *text)")
assert "if (!from_me && g_win.owner != idx && a.unread < 0xFFFF) a.unread++;" in append_fn, (
    "unread must count only THEIR lines, and only when the conversation is not "
    "the one on screen — reading it is the acknowledgement"
)
tail_fn = fn_body(agents, "void agents_thread_goto_tail(int idx)")
assert "a.unread = 0;" in tail_fn, (
    "opening a conversation must clear its unread count — goto_tail is the "
    "focus point everything else in this file hangs off"
)

print("conversation store firmware pins: OK")
