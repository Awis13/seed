#!/usr/bin/env python3
"""Static pins for the transport seam's FIRMWARE halves — the parts the host
tests cannot reach because they need the radios, the notification queue and the
panel.

WHAT IS PINNED
--------------
1. agents_send() no longer SELECTS a transport by conversation name. It used to
   be a ladder hardwired to one agent (`strcmp(agent_id, "claude")` twice), which
   is why nothing that arrived over another wire could be answered at all. The
   name tests still exist inside the CONV_AGENT backend — they are that wire's
   internal routing and both are load bearing — but they must not be back in the
   chat path, or adding a transport means editing it again.
2. transport_send() dispatches on the record's transport, through the pure
   planner, and hands each backend the address the planner resolved.
3. The CONV_MESH branch refuses rather than falling back to the gateway. The
   mesh layer has no arbitrary-peer send yet (mc_client.h exposes only
   mesh_client_send_to_gateway), and routing a private reply through the gateway
   would hand it to a third party.
4. A card becomes visible in exactly ONE place. The wake/ring/sound/log sequence
   was written out twice before; two copies is the shape where a transport
   quietly gets a card that does not ring.
5. The manifest cannot re-point a seeded conversation's route (S3).
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")
notify = (ROOT / "src" / "skills" / "notify.cpp").read_text(encoding="utf-8")
transport = (ROOT / "src" / "transport.h").read_text(encoding="utf-8")


def fn_body(text, sig, terminator="\n}"):
    start = text.index(sig)
    end = text.index(terminator, start)
    return text[start : end + len(terminator)]


# --- 1. the chat path no longer picks a wire by name -------------------------
send = fn_body(agents, "static bool agents_send(const char *agent_id, const char *text)")
assert '"claude"' not in send, (
    "agents_send must not select a transport by conversation name — that "
    "hardwired ladder is what made a non-agent thread unanswerable"
)
assert "strcmp(agent_id" not in send, (
    "agents_send must not branch on the conversation id at all"
)
assert "rns_lxmf_reply_target" not in send and "agents_rns_uplink" not in send, (
    "the agent wire's own ladder belongs in the CONV_AGENT backend, not in the "
    "chat path"
)
assert "agents_bridge_post" not in send, (
    "the bridge is one backend's business, not the chat path's"
)
# It must still do the two things that are NOT transport: the GPS answer and the
# local thread append. Dropping either would be a silent behaviour change.
assert "agents_gps_intercept(idx, cleaned)" in send, (
    "the local GPS answer must still short-circuit before anything is sent"
)
assert "agents_push_line(idx, true, cleaned)" in send, (
    "the message must still land in the room before it is handed to a transport"
)
assert "transport_send(&g_convs[idx], cleaned)" in send, (
    "agents_send must hand off to the one outbound seam"
)

# --- 2. dispatch goes through the pure planner -------------------------------
tsend = fn_body(agents, "bool transport_send(const struct Conversation *conv, const char *text)")
assert "transport_plan(&tgt" in tsend, (
    "transport_send must dispatch through the pure planner, not a private switch"
)
assert "conv->transport" in tsend, "the wire comes from the record"
for backend in ("TRANSPORT_BACKEND_AGENT", "TRANSPORT_BACKEND_LXMF",
                "TRANSPORT_BACKEND_MESH_PEER"):
    assert backend in tsend, f"transport_send must have a {backend} branch"
# The address the backends are given is the planner's output, not a global.
assert re.search(r"transport_plan\(&tgt,\s*&backend,\s*&addr,\s*&addr_len\)", tsend), (
    "transport_send must take the resolved address from the planner"
)
assert "transport_send_lxmf(addr" in tsend and "transport_send_mesh(addr" in tsend, (
    "each backend must be handed the planner-resolved address"
)

# --- 3. the mesh branch refuses instead of using the gateway -----------------
mesh = fn_body(agents, "static bool transport_send_mesh(")
assert "mesh_client_send_to_gateway" not in mesh, (
    "a peer reply must never be routed through the gateway — that delivers a "
    "private message to a third party"
)
assert "return false;" in mesh, "the unimplemented peer send must refuse"
assert re.search(r'\*why\s*=\s*"[^"]+"', mesh), (
    "the refusal must name a reason, so it surfaces in the room instead of "
    "vanishing"
)
# Nothing anywhere in the mesh backend may reach for the gateway send.
assert "mesh_client_send_to_gateway" not in fn_body(
    agents, "bool transport_send(const struct Conversation *conv, const char *text)"
), "transport_send must not fall back to the gateway"

# --- 4. a card is raised in exactly one place --------------------------------
# The sequence that makes a card visible. notify_push alone does not raise it.
RAISE = "notify_request_sound(e.level, e.source);"
assert notify.count(RAISE) == 1, (
    "the card raise (wake/ring/sound/log) must exist in exactly one place, "
    f"found {notify.count(RAISE)} — two copies is how a transport gets a card "
    "that does not ring"
)
raise_fn = fn_body(notify, "static uint32_t notify_raise(")
assert RAISE in raise_fn, "the raise must live in notify_raise"
# Every step of the raise, not just the flags. Each of these fails silently in
# its own way if it goes missing while the others stay: without the id the
# staged arrival opens whichever card was there before; without the ring level a
# critical card rings as info; without the timestamps a card with any ttl_s
# expires on the first sweep and one with none has no age to show.
for staged in ("notify_arrived_id = id;", "notify_arrived = true;",
               "notify_ring_level = e.level;", "notify_ring_arrived = true;",
               "e.created_epoch =", "e.created_ms =",
               "display_force = true;", "event_add("):
    assert staged in raise_fn, f"the raise must still perform: {staged}"
# Both producers converge on it: the HTTP route and the transport-facing door.
card = fn_body(notify, "uint32_t inbox_deliver_card(")
assert "inbox_card_plan(via, sev, source, title, &plan)" in card, (
    "the card door must run the pure admission plan"
)
assert "notify_raise(e, NULL, NULL)" in card, (
    "the card door must raise through the shared entry"
)
# Running the plan is not the same as USING it. Taking the caller's raw source
# instead of the plan's would leave an unlabelled mesh card looking like an HTTP
# one, and taking the raw severity would undo the clamp — both while the
# inbox_card_plan() call above still sits there looking correct.
assert "notify_copy_text(e.source, sizeof(e.source), plan.source);" in card, (
    "the card must carry the PLAN's source, or a mesh card loses its wire tag"
)
assert "e.level = plan.sev;" in card, (
    "the card must carry the PLAN's severity, or the clamp is decorative"
)
# 0 is a valid option index, so the unanswered marker cannot be left at the
# memset's zero — a fresh card would render as though its first option had
# already been picked.
assert "e.chosen = -1;" in card, (
    "an unanswered card must be marked -1, not left at 0 (a real option index)"
)
assert "notify_raise(e, e.opt_count ? &opts : NULL, &replaced)" in notify, (
    "POST /notify must raise through the shared entry too, not its own copy"
)
# The old inline copy must not creep back into the HTTP route.
http = notify[notify.index('server.on(AsyncURIMatcher::exact("/notify"), HTTP_POST'):]
http = http[: http.index('server.on(AsyncURIMatcher::exact("/notify"), HTTP_GET')]
assert "notify_push(" not in http, (
    "the HTTP route must not push a card itself — it validates, the shared "
    "entry raises"
)

# --- 5. a seeded conversation's route is re-derived, never read from the card -
apply_fn = fn_body(agents, "static void agents_conv_apply(Conversation &a, const ConvManifestLine &ml)")
assert "conv_route_resolve(" in apply_fn, (
    "the manifest loader must resolve the route rather than assign it"
)
assert "a.seeded != 0" in apply_fn, (
    "seededness must be what decides whether the card's route is trusted"
)
assert not re.search(r"a\.transport\s*=\s*ml\.transport", apply_fn), (
    "the manifest must not assign transport directly — /conversations.txt is a "
    "user-editable file on a removable card and transport now chooses the wire "
    "a reply leaves on"
)
assert not re.search(r"memcpy\(a\.reply_addr,\s*ml\.reply", apply_fn), (
    "the manifest must not assign the return address directly either"
)
# The seeded flag is compiled in, not discovered.
assert agents.count(".seeded = 1") == 2, (
    "both seeded conversations must be marked in the static table"
)

# --- 6. the inbound message door checks the wire -----------------------------
msg = fn_body(agents, "bool inbox_deliver_msg(")
assert "a.transport == via" in msg, (
    "a conversation may only be fed by the wire it lives on — otherwise a "
    "sender on one transport can land a line in another's room by guessing an id"
)
assert "if (idx < 0) return false;" in msg, (
    "an unknown peer id must be refused, not minted — creating a conversation "
    "for a peer nobody has met belongs with the receivers that need it"
)
assert "wire_ok && label && label[0] && !a.seeded" in msg, (
    "the label write must be gated on BOTH the wire check and !seeded: without "
    "the first, a sender on the wrong wire still renames the room; without the "
    "second, a remote sender can rename a built-in room on screen and pass "
    "itself off as it"
)

print("transport seam pins: OK")
