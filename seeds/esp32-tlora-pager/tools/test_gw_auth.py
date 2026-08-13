#!/usr/bin/env python3
"""Static regressions for gateway capability-token auth (ping + reply)."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8")

# Token storage: SPIFFS path define, boot load, trimmed into RAM.
assert '#define GW_TOKEN_PATH "/gw_token.txt"' in main
assert "static void gw_token_load()" in main
load = main[main.index("static void gw_token_load()") :]
# Slice to the next top-level definition (the function now has nested braces
# for the NVS-first branch, so a first-"}" cut would truncate the body).
load = load[: load.index("\nstatic ", 1)]
# C2: NVS is the source of truth after the boot migration (survives a SPIFFS
# format); the file remains the pre-migration fallback.
assert 'secret_store_get("gw_tok"' in load, "gw token must load NVS-first"
assert "read_spiffs_file(GW_TOKEN_PATH)" in load, "the SPIFFS file stays the fallback"
assert "stored.trim()" in load
assert 'snprintf(gw_token, sizeof(gw_token), "%s", stored.c_str());' in load
assert "gw_token_load();" in main[main.index("void setup()") :], (
    "the token must be loaded once at boot"
)

# s3 (C1): the token lives in a fixed char buffer, mirroring mesh_gw_url —
# it is written on the AsyncTCP task (POST /gw/token) and read on the loop
# task (/ping, reply); an Arduino String here can reallocate under the
# reader and dangle its c_str().
assert "static char gw_token[GW_TOKEN_MAX + 1]" in main
assert "static String gw_token" not in main
assert "#define GW_TOKEN_MAX  128" in main

# Both gateway call sites must send the Bearer header when a token is set —
# and only then: an empty token keeps the legacy header-less request.
header = 'http.addHeader("Authorization", String("Bearer ") + gw_token);'

reply = main[main.index("static bool reply_upstream_http") :]
reply = reply[: reply.index("static bool reply_upstream_mesh")]
assert header in reply, "POST /reply must carry the capability token"
assert "if (gw_token[0])" in reply
assert "code >= 200 && code < 300" in reply, (
    "reply success semantics must stay 2xx-only so the mesh fallback still fires"
)

ping = main[main.index("static void ui_mesh_ping_gateway()") :]
ping = ping[: ping.index("static void kb_layout_save()")]
assert header in ping, "GET /ping must carry the capability token"
assert "if (gw_token[0])" in ping

# Ping screen semantics: any HTTP status (401 included) proves the WiFi link
# to the gateway is alive; only no-response paths may paint the column dead.
assert "if (code > 0 && code < 400)" not in ping, (
    "a 4xx from the gateway must not read as WiFi-dead on the PING screen"
)
assert "if (code > 0)" in ping
assert "code == 401 || code == 403" in ping
assert '"no auth"' in ping, "unauthorized must be visually distinct"
assert '"no reply"' in ping, "unreachable must be visually distinct"

# Provisioning route: exact matcher, device auth, collected body freed,
# atomic persist, in-RAM copy updated.
route_reg = 'server.on(AsyncURIMatcher::exact("/gw/token"), HTTP_POST'
assert route_reg in main, "POST /gw/token must be registered with an exact matcher"
route = main[main.index(route_reg) :]
route = route[: route.index("}, NULL, handle_body_collect);")]
assert "if (!require_auth(req)) return;" in route
assert "notify_take_body(req)" in route
assert "free(body);" in route, "the collected body must be freed"
assert "write_spiffs_file_atomic(GW_TOKEN_PATH, GW_TOKEN_TMP, tok)" in route
assert "SPIFFS.remove(GW_TOKEN_PATH)" in route, "an empty body must clear the file"
assert 'snprintf(gw_token, sizeof(gw_token), "%s", tok.c_str());' in route, (
    "the in-RAM copy must update without a reboot"
)

# s1 (C1): a JSON body must carry a string token. A valid JSON body without
# one (or with a non-string one, e.g. {"token":5}) used to resolve to "" and
# silently CLEAR the stored token; it must 400 instead. Only a genuinely
# empty body clears.
assert 'if (!input["token"].is<const char *>())' in route, (
    "JSON without a string token must be rejected, not treated as empty"
)
assert "token must be a JSON string" in route
assert 'input["token"] | ""' not in route, (
    "the silent-default extraction (missing/non-string -> \"\") must be gone"
)
assert "send an empty body to clear" in route, (
    "an explicit empty JSON token string must 400 and point at the clear path"
)
assert route.index('is<const char *>') < route.index(
    "SPIFFS.remove(GW_TOKEN_PATH)"
), "the JSON validation must run before anything can clear the file"
assert "raw body or JSON string token; empty body clears" in main, (
    "the /skill row must describe the tightened clear semantics"
)

# s2 (C1): the token VALUE must never be interpolated into the event log or
# echoed in a response body from the route slice — only the set/cleared word.
assert 'event_add("gateway token %s", gw_token[0] ? "set" : "cleared");' in route
for leak in ('event_add("gateway token %s", tok',
             'event_add("gateway token %s", gw_token);',
             'doc["token"]', "+ tok +", "+ gw_token"):
    assert leak not in route, f"token value leak shape found: {leak!r}"
assert route.count("event_add(") == 1, (
    "exactly one event line, and it carries set/cleared, never the token"
)

# The provisioned token must never be committed.
assert "data/gw_token.txt" in gitignore
assert "data/agent_bridge.txt" in gitignore
assert (ROOT / "data" / "gw_token.txt.example").exists()

print("Gateway auth policy tests: OK")
