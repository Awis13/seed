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
load = load[: load.index("}") + 1]
assert "read_spiffs_file(GW_TOKEN_PATH)" in load
assert "gw_token.trim()" in load
assert "gw_token_load();" in main[main.index("void setup()") :], (
    "the token must be loaded once at boot"
)

# Both gateway call sites must send the Bearer header when a token is set —
# and only then: an empty token keeps the legacy header-less request.
header = 'http.addHeader("Authorization", String("Bearer ") + gw_token);'

reply = main[main.index("static bool reply_upstream_http") :]
reply = reply[: reply.index("static bool reply_upstream_mesh")]
assert header in reply, "POST /reply must carry the capability token"
assert "if (gw_token.length() > 0)" in reply
assert "code >= 200 && code < 300" in reply, (
    "reply success semantics must stay 2xx-only so the mesh fallback still fires"
)

ping = main[main.index("static void ui_mesh_ping_gateway()") :]
ping = ping[: ping.index("static void kb_layout_save()")]
assert header in ping, "GET /ping must carry the capability token"
assert "if (gw_token.length() > 0)" in ping

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
assert "SPIFFS.remove(GW_TOKEN_PATH)" in route, "an empty token must clear the file"
assert "gw_token = tok;" in route, "the in-RAM copy must update without a reboot"

# The provisioned token must never be committed.
assert "data/gw_token.txt" in gitignore
assert "data/agent_bridge.txt" in gitignore
assert (ROOT / "data" / "gw_token.txt.example").exists()

print("Gateway auth policy tests: OK")
