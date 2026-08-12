#!/usr/bin/env python3
"""Static pins for MeshCore contact persistence — the firmware halves the
record-codec host test cannot reach because they need SPIFFS and the radio.

The table used to be a RAM array rebuilt from adverts on every boot, so a
conversation that survived a restart still could not be answered until its peer
advertised again. These pin the three things that make persistence real, and the
one ordering that makes it useful.
"""

import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
mc = (ROOT / "src" / "mesh" / "mc_client.cpp").read_text(encoding="utf-8")
mc_h = (ROOT / "src" / "mesh" / "mc_client.h").read_text(encoding="utf-8")
agents = (ROOT / "src" / "skills" / "agents.cpp").read_text(encoding="utf-8")


def fn_body(text, sig, terminator):
    """Slice a definition; reject a forward declaration (see the other pins)."""
    start = text.index(sig)
    end = text.index(terminator, start)
    body = text[start : end + len(terminator)]
    head = body[: body.index("{")] if "{" in body else body
    assert ";" not in head, (
        f"fn_body({sig!r}) matched a declaration, not the definition"
    )
    return body


# --- 1. the table is written, and written through the ported codec ----------
save = fn_body(mc, "  void saveContacts() {", "\n  }")
assert "mesh_contact_pack(&r, buf)" in save, (
    "the record must be serialised by the ported codec, not by writing the "
    "struct's raw memory — the on-disk layout is MeshCore's, not ours"
)
assert "MESH_CONTACTS_PATH" in save, "the file is /contacts3, not a name of ours"
assert "mesh_contact_worth_saving(c.type)" in save, (
    "an anonymous/transient contact is not stored, matching upstream's "
    "save_filter (type != ADV_TYPE_NONE)"
)

# --- 2. it is read back, and a short read is end-of-file --------------------
load = fn_body(mc, "  int loadContacts() {", "\n  }")
assert "mesh_contact_unpack(buf, &r)" in load, "read back through the same codec"
assert re.search(r"f\.read\(buf, sizeof\(buf\)\) == \(int\)sizeof\(buf\)", load), (
    "the file has no count and no terminator — a short read IS the end, which "
    "is exactly how upstream reads it"
)
assert "shared_secret_valid = false" in load, (
    "the shared secret is derived, never stored: a restored contact must "
    "recompute it rather than inherit whatever was in memory"
)
assert "addContact(c)" in load, "a loaded contact must enter the live table"

# --- 3. saved on every change, or the table is stale the moment it matters ---
disc = fn_body(mc, "  void onDiscoveredContact(", "\n  }")
assert "saveContacts();" in disc, (
    "a newly heard contact must be persisted when it is heard — that advert "
    "may be the only one for hours"
)
path = fn_body(mc, "  void onContactPathUpdated(", "\n  }")
assert "saveContacts();" in path, (
    "the route to a peer is worth as much as the peer; a stale path cannot "
    "deliver"
)

# --- 4. THE ORDERING THAT MAKES IT USEFUL -----------------------------------
# Everything that sends checks mesh_client_ready() first, and POST /agents/send
# does so from the AsyncTCP task. If ready were set before the table loaded,
# a reply in that window would be refused as "unknown peer" for a peer the
# device knows perfectly well.
begin = fn_body(mc, "bool mesh_client_begin() {", "\n}")
assert "loadContacts();" in begin, "the table must be restored at bring-up"
assert begin.index("loadContacts();") < begin.index("g_ready = true;"), (
    "contacts must load BEFORE the client reports ready — otherwise there is a "
    "window where a send is accepted against an empty table"
)

# --- 5. a restored mesh conversation must name a peer we have met -----------
# The radio comes up ~5 s after boot while conversations are restored during
# setup, so the loader reads the contact FILE rather than asking the mesh.
assert "agents_known_peers_load();" in agents, (
    "the conversation loader must know which peers have been met"
)
known = fn_body(agents, "static void agents_known_peers_load() {", "\n}")
assert "mesh_contact_unpack" in known, "it must read through the same codec"
# WHICH FILESYSTEM, not just which path. The conversation store keeps history on
# the SD card when one is mounted while the contact table is on SPIFFS, so the
# same path on two filesystems is two files: the gate would find nothing, refuse
# every mesh conversation, and the manifest rewrite immediately after would
# delete those records — turning the feature that restores chats into the thing
# that erases them. Asserting the path constant alone cannot see that.
# It reaches the accessor by DECLARATION, not by including mc_client.h — that
# header is barred from this file by the loop-safety pin in
# test_transport_pins.py, because the MeshCore stack belongs to the loop task.
# The accessor names a filesystem and touches no stack state, so it crosses by
# hand and the ban stays intact.
assert "fs::FS &mesh_contacts_fs();" in agents, (
    "the accessor must be forward-declared in agents.cpp, not pulled in with "
    "the whole mesh client header"
)
assert '#include "../mesh/mc_client.h"' not in agents, (
    "including the mesh client header would re-open the door the loop-safety "
    "pin closed"
)
assert "mesh_contacts_fs()" in known, (
    "the gate must open /contacts3 on the filesystem the mesh WRITES it to, "
    "via the shared accessor — g_store is the SD card on a card-equipped "
    "device and would find nothing"
)
# Absence assertions run on CODE: this function's comment explains why it does
# NOT use g_store, and a check that reads comments would fire on that.
known_code = re.sub(r"/\*.*?\*/", " ", known, flags=re.S)
known_code = re.sub(r"//[^\n]*", " ", known_code)
assert "g_store" not in known_code, (
    "the gate must not use the conversation store's filesystem for the "
    "contact file"
)
# Both sides of the file go through the one accessor, so they cannot drift.
for body, who in ((save, "the writer"), (load, "the reader")):
    assert "mesh_contacts_fs()" in body, (
        f"{who} must use the shared filesystem accessor"
    )
    body_code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body_code = re.sub(r"//[^\n]*", " ", body_code)
    assert "SPIFFS.open" not in body_code, (
        f"{who} must not name a filesystem directly — that is how the two "
        "sides drifted apart"
    )

# WHICH filesystem the accessor actually RETURNS. Every other check here says
# "both sides use the accessor", which stays true if the accessor itself is
# changed: pointing it at SD leaves the whole suite green while, on a card-less
# device, every open fails, nothing persists, the gate finds no contacts, every
# mesh conversation is refused at load, and the manifest rewrite deletes them —
# the same damage the SD/SPIFFS split caused, by a different route and just as
# silently.
assert re.search(r"fs::FS &mesh_contacts_fs\(\)\s*\{\s*return SPIFFS;", mc), (
    "the contact table lives on SPIFFS — the internal flash that is always "
    "there; naming SD would silently stop persisting on a card-less device"
)

# --- 6. a repeat advert must not rewrite the file ---------------------------
# onDiscoveredContact fires for EVERY advert, repeats included. Saving
# unconditionally rewrites the whole file every few minutes for as long as
# anything is in range: flash wear on the internal SPIFFS, and a multi-ms
# blocking write on the loop task, for data that did not change.
assert "if (is_new) saveContacts();" in disc, (
    "only a NEW contact may trigger a write — a repeat advert from a known "
    "peer changes nothing and must not rewrite the table"
)
apply_fn = fn_body(agents, "static void agents_manifest_apply(const ConvManifestLine &ml)", "\n}")
assert "ml.transport == CONV_MESH && !agents_peer_is_known(ml.conv)" in apply_fn, (
    "a mesh conversation may only be restored for a peer this device has heard "
    "advertise — otherwise a line typed into /conversations.txt by hand can "
    "conjure a correspondent that never existed"
)
assert "g_conv_load_unknown++" in apply_fn, (
    "a refused line must be counted, not silently dropped — boot rewrites the "
    "manifest from the table, so the line is erased the same boot"
)

print("contact persistence pins: OK")
