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
3. The CONV_MESH branch reaches the PEER and can never reach the gateway. It
   submits to the loop task rather than driving the radio from whatever task
   called it, and no MeshCore stack symbol may appear in skills/agents.cpp at
   all — routing a private reply through the gateway would hand it to a third
   party, and driving the stack off the loop task would corrupt it.
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
    """Slice a function DEFINITION.

    The guard is not decoration: the firmware forward-declares many of these
    near the top of their file, and `text.index(sig)` happily lands on the
    declaration. The slice then runs from there to the end of some unrelated
    later function, so an assertion against it — especially an ABSENCE
    assertion — can pass on code that has nothing to do with the function under
    test. That is a pin that looks green and checks nothing. A slice whose head
    reaches a ';' before its first '{' is therefore rejected outright.
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

# --- 7. the mesh wire: peer in, peer out ------------------------------------
meshcore = (ROOT / "src" / "skills" / "meshcore.cpp").read_text(encoding="utf-8")
mc_h = (ROOT / "src" / "mesh" / "mc_client.h").read_text(encoding="utf-8")
mc_c = (ROOT / "src" / "mesh" / "mc_client.cpp").read_text(encoding="utf-8")

# 7a. the peer send exists, addresses the PEER, and cannot become the gateway.
assert "bool mesh_client_send_to_peer(const uint8_t *pubkey" in mc_h, (
    "the peer send must be its own primitive, not a gateway call with a "
    "different argument"
)
peer_send = fn_body(mc_c, "  bool sendToPeer(const uint8_t *pk, const char *text, const char **why) {",
                    "\n  }")
assert "lookupContactByPubKey((uint8_t *)pk, 32)" in peer_send, (
    "the peer send must resolve the PEER's contact from its own key"
)
# Absence assertions run on the CODE: the function's own comment explains why it
# is not the gateway send, and a check that reads comments would fire on that.
peer_code = re.sub(r"/\*.*?\*/", " ", peer_send, flags=re.S)
peer_code = re.sub(r"//[^\n]*", " ", peer_code)
assert "heltec_pk_hex" not in peer_code and "ateway" not in peer_code, (
    "a peer reply must never resolve or fall back to the gateway — that "
    "delivers a private message to a third party"
)
assert "addContact" not in peer_code, (
    "there must be no create-on-miss: inventing a contact from an unverified "
    "key is exactly how a stranger would get one"
)
assert "expected_ack_crc != 0" in peer_send, (
    "only one private send may await an ACK; a peer send must wait rather than "
    "stomp the gateway's multi-part chat mid-delivery"
)

# 7b. transport_send_mesh is wired to it and to nothing else.
tmesh = fn_body(agents, "static bool transport_send_mesh(")
assert "g_agents_mesh_peer_send(addr, text, why)" in tmesh, (
    "the mesh backend must SUBMIT the planner-resolved peer address to the "
    "loop task, not drive the radio from whatever task it was called on"
)
assert "mesh_client_send_to_gateway" not in tmesh, (
    "the mesh backend must never fall back to the gateway"
)
# NO MESHCORE STACK CALL MAY LIVE IN THIS FILE AT ALL. sendMessage allocates
# from the packet pool, runs the ECDH and arms the ACK, all state the loop task
# drives without a lock — and a reply can originate on AsyncTCP, since
# POST /agents/send resolves any conversation. The whole file is checked, not
# just the backend, because the next inline call would be somewhere else.
agents_code = re.sub(r"/\*.*?\*/", " ", agents, flags=re.S)
agents_code = re.sub(r"//[^\n]*", " ", agents_code)
for stack_call in ("mesh_client_send_to_peer", "mesh_client_send_to_gateway",
                   "mesh_client_loop", "mesh_client_knows_peer"):
    assert stack_call not in agents_code, (
        f"{stack_call} is a MeshCore stack call and must not be reachable from "
        "this file — the radio belongs to the loop task, and this file runs on "
        "the keyboard task and the AsyncTCP task too"
    )
assert "#include \"../mesh/mc_client.h\"" not in agents, (
    "the store must not even see the mesh client header — the peer send is "
    "handed in as a submit callback the mesh skill registers"
)
assert "no peer send yet" not in agents, (
    "the placeholder refusal must be gone now that the peer send exists"
)

# 7c. the plain-DM default is a CONVERSATION, and the old card default is gone.
priv = fn_body(
    meshcore,
    "static uint32_t mesh_on_private_text(const uint8_t *from_pubkey,\n"
    "                                    const char *from_name,\n"
    "                                    const char *text) {",
)
# The plain-DM branch is the one containing the conversation call — anchored on
# that rather than on "the last else", which is the fallback nested inside it.
assert "inbox_deliver_msg_mesh" in priv, (
    "the plain-DM path must reach a conversation at all — without it there "
    "is no branch to check and every assertion below is meaningless"
)
_mark = priv.index("inbox_deliver_msg_mesh")
tail = priv[priv.rindex("} else {", 0, _mark) :]
assert "inbox_deliver_msg_mesh(from_pubkey, MESH_PUB_LEN, from_name, text)" in tail, (
    "a plain private DM must land in a conversation"
)
# THE OLD DEFAULT IS GONE AS A DEFAULT. The card call still exists — it is the
# fallback for an unknown peer — so its mere presence proves nothing. What must
# be true is that it is no longer reached unconditionally: the peer test and the
# conversation attempt both come FIRST. (If either were deleted outright the
# .index below raises and this file fails, which is the same signal.)
assert 'inbox_deliver_card(CONV_MESH,' in tail, (
    "the unknown-peer card fallback must still exist — without it a "
    "stranger's DM is silently dropped — and it must go through the card door "
    "like every other transport rather than around it"
)
_card = tail.index('inbox_deliver_card(CONV_MESH,')
assert tail.index("mesh_client_knows_peer") < _card, (
    "the card must no longer be the unconditional plain-DM default — the peer "
    "test comes first"
)
assert tail.index("inbox_deliver_msg_mesh") < _card, (
    "a known peer's DM must be tried as a conversation before falling back"
)
# ... and the fallback must still be there, or a stranger's DM vanishes.
assert 'inbox_deliver_card(CONV_MESH,' in tail, (
    "an unknown peer's DM must still become a card — nothing may be dropped"
)

# 7d. the policy gate: known contacts only.
# Same conditional-delivery rule on the mesh side, and for the same reason:
# inbox_deliver_msg_mesh returns false on a full/dead queue or when no slot is
# free, and the card is the floor under it. Pinned as the delivery gating the
# branch, so calling it and ignoring the result cannot pass.
assert "mesh_client_knows_peer(from_pubkey) &&\n            inbox_deliver_msg_mesh(" in tail, (
    "the mesh chat branch must be taken only when delivery SUCCEEDED — calling "
    "it and ignoring the result skips the card fallback and drops the message"
)
assert "mesh_client_knows_peer(from_pubkey)" in tail, (
    "only an advert-attributed contact may open or feed a conversation; the "
    "inbox is not open to anyone in radio range"
)
assert "from_pubkey &&" in tail, (
    "a message with no key at all (the HTTP inject) cannot reach a conversation"
)
inject = meshcore[meshcore.index("uint32_t id = mesh_on_private_text(nullptr") :][:120]
assert "nullptr, nullptr, wire_buf" in inject, (
    "the HTTP inject has no peer behind it and must stay on the card path"
)

# 7e. the mesh inbound is LOOP-SAFE: the radio callback does no SD work.
producer = fn_body(agents,
                   "bool inbox_deliver_msg_mesh(const uint8_t *pubkey, uint8_t pubkey_len,\n"
                   "                            const char *name, const char *text) {")
for banned in ("agents_store_append", "agents_push_line", "conv_mint",
               "agents_manifest_persist", "agents_sync_view"):
    assert banned not in producer, (
        f"inbox_deliver_msg_mesh must not call {banned} — it runs on the loop "
        "task from the radio callback, where an SD append seizes the shared bus"
    )
assert re.search(r"xQueueSend\(g_route_q,\s*&item,\s*0\)", producer), (
    "the mesh inbound must hand off to the off-loop drain with a 0-tick send"
)
drain_mesh = fn_body(agents, "static bool agents_route_peer(const AgentRouteItem &item)")
assert "conv_mint(id, item.session, item.via, item.peer, item.peer_len,\n                        item.via == CONV_MESH)" in drain_mesh, (
    "the mint happens off-loop, in the drain, on the wire the message came "
    "in on — and only an advert-attributed mesh peer may evict"
)
# The drain delivers through the door rather than appending directly, so the
# door's wire check runs on the mint path too. The append still happens
# off-loop — inside inbox_deliver_msg, which the drain calls.
assert "inbox_deliver_msg(item.via, id, item.session, item.text)" in drain_mesh, (
    "the drain must deliver through the message door, so its transport check "
    "runs on the path that mints — appending directly skips it"
)
door = fn_body(agents, "bool inbox_deliver_msg(uint8_t via, const char *peer_id, const char *label,\n"
                       "                       const char *text) {")
assert "a.transport == via" in door, (
    "the door is only worth routing through if it still checks the wire"
)
assert "agents_on_inbound(a.id, text, true);" in door, (
    "the door is where the append happens, off-loop, via the drain"
)
assert "conv_peer_id(item.peer, item.peer_len, id, sizeof(id))" in drain_mesh, (
    "the conversation is keyed on the peer's public key, not on its name"
)

# 7f. A PEER IS LIVE-ONLY: history persists, routing never does.
# The append writes /conv.<id> (checked above). The manifest must NOT be
# written: nothing reads a peer line back, so it is state no consumer wants —
# and it would leave /conversations.txt carrying CONV_MESH records with
# peer-chosen labels that this build authored and no later reader has decided
# to trust, which is how the "the loader creates nothing from the card" rule
# gets quietly re-armed with real data underneath it.
drain_code = re.sub(r"/\*.*?\*/", " ", drain_mesh, flags=re.S)
drain_code = re.sub(r"//[^\n]*", " ", drain_code)
assert "agents_manifest_persist" not in drain_code, (
    "minting a peer must not write /conversations.txt — a peer's route is "
    "live-only, and persisting it is a decision of its own rather than a "
    "side effect of meeting the peer"
)
# The length contract is a parameter, not an assumption.
assert "pubkey_len != TRANSPORT_MESH_ADDR_LEN" in producer, (
    "the public key length must be checked, so a caller with a shorter buffer "
    "is refused rather than having 32 bytes read out of it"
)
assert "memcpy(item.peer, pubkey, pubkey_len);" in producer, (
    "the copy must use the checked length, not a hardcoded width"
)

# --- 8. the LXMF wire: a person's message is a chat, a stranger cannot churn --
rns = (ROOT / "src" / "skills" / "rns.cpp").read_text(encoding="utf-8")

ingest = fn_body(rns, "static bool lxmf_ingest_wire(const uint8_t *wire, size_t len)")
assert "if (route.kind == LXMF_ROUTE_CHAT) {" in ingest, (
    "a person's LXMF must route to their conversation"
)
assert "inbox_deliver_msg_lxmf(g_rns_lxmf_msg.source_hash, LXMF_HASH_LEN," in ingest, (
    "the conversation is keyed on the sender's SOURCE HASH, not on a thread "
    "name — that is what stops two senders sharing one return address"
)
# NEVER A SILENT DROP. The chat branch must fall through to the card, not
# return: the queue can be full, and a stranger can find no free slot.
_chat = ingest.index("if (route.kind == LXMF_ROUTE_CHAT) {")
assert "inbox_deliver_card(CONV_LXMF," in ingest, (
    "the LXMF card fallback must exist and go through the card door"
)
_card = ingest.index("inbox_deliver_card(CONV_LXMF,")
assert _chat < _card, "the chat attempt must precede the card fallback"
chat_branch = ingest[_chat:_card]
# THE LABEL IS A VALUE BUG A STRUCTURAL PIN CANNOT SEE. route.source is
# provably the "lxmf" literal on this branch (a chat is reached only when our
# meta is absent, which is exactly when the planner falls back to it), so
# passing it would name every sender alike and the hex id would never show.
chat_code = re.sub(r"/\*.*?\*/", " ", chat_branch, flags=re.S)
assert "route.source" not in chat_code, (
    "the chat path must not pass route.source as the conversation label — it "
    "identifies nobody there, and it masks the distinguishing hex id"
)
assert "nullptr, g_rns_room_text)" in chat_code, (
    "the chat path passes no name: LXMF carries none for a third-party sender"
)
# Chats and rooms are different things and must not share a counter.
assert "g_rns_lxmf_chats++" in chat_code, (
    "a chat must count as a chat, not as a room — /rns/status would otherwise "
    "report room routing that never happened"
)
assert "g_rns_lxmf_rooms++" not in chat_code, (
    "the room counter belongs to the thread-named route, not to chats"
)
assert chat_branch.count("return true;") == 1, (
    "only the SUCCESS path may return — a failed chat has to fall through to "
    "the card below, or an LXMF message vanishes"
)
assert "return false;" not in chat_branch, (
    "a failed chat must fall through to the card, not abandon the message"
)
# THE RETURN MUST BE CONDITIONAL ON DELIVERY, and the two shape checks above do
# not establish that: calling the deliver, ignoring its bool and returning true
# unconditionally satisfies both (one return true, no return false) while the
# card fallback never runs and the message is silently dropped. That failure is
# not hypothetical — inbox_deliver_msg_lxmf returns false exactly when the queue
# is full or a stranger finds no free slot, which is the designed outcome of the
# no-evict rule. So the call itself must BE the condition.
assert "if (inbox_deliver_msg_lxmf(" in chat_branch, (
    "the chat's return must be conditional on delivery — an unconditional "
    "return true skips the card fallback and drops the message"
)

# The producer is loop-safe: the LXMF receive runs on the loop task, both feeds.
lproducer = fn_body(agents,
                    "bool inbox_deliver_msg_lxmf(const uint8_t *source_hash, uint8_t hash_len,\n"
                    "                            const char *name, const char *text) {")
for banned in ("agents_store_append", "agents_push_line", "conv_mint",
               "agents_manifest_persist", "agents_sync_view"):
    assert banned not in lproducer, (
        f"inbox_deliver_msg_lxmf must not call {banned} — it runs on the loop "
        "task, where an SD append seizes the shared bus"
    )
assert re.search(r"xQueueSend\(g_route_q,\s*&item,\s*0\)", lproducer), (
    "the LXMF inbound must hand off to the off-loop drain with a 0-tick send"
)
assert "hash_len != TRANSPORT_LXMF_ADDR_LEN" in lproducer, (
    "the source-hash length must be checked, not assumed"
)
assert "item.via = CONV_LXMF;" in lproducer, (
    "the item must record which wire it came in on — the drain decides whether "
    "it may evict from that"
)

# AN ADDRESS-BASED SENDER MAY NOT EVICT. Anyone holding our announced address
# can write to us, so minting by displacement would let a stranger flood out
# the conversations the user actually has.
assert "item.via == CONV_MESH)" in drain_mesh, (
    "only the advert-attributed mesh wire may evict; an LXMF sender takes a "
    "free slot or the message becomes a card"
)
conv_store_h = (ROOT / "src" / "conv_store.h").read_text(encoding="utf-8")
assert "if (!may_evict) return -1;" in conv_store_h, (
    "the planner must refuse outright rather than displace when eviction is "
    "not permitted"
)

# --- 9. the agent chat frame may only land in an AGENT conversation ----------
# The frame names its target by id and carries a side marker, so `side=u` lands
# a line attributed to the USER. That was safe while ids were two compiled-in
# names; peer ids are observable by anyone in radio range, so without this gate
# a stranger could forge words into a peer's thread as if the user typed them.
assert "if (idx >= 0 && agents_transport(idx) != CONV_AGENT) idx = -1;" in meshcore, (
    "the C1-style agent frame must be refused for a non-agent conversation — "
    "otherwise it can forge a user-attributed line into a peer's thread"
)

print("transport seam pins: OK")
