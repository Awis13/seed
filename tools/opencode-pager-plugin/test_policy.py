#!/usr/bin/env python3
"""Static ownership and secret regressions for the OpenCode plugin."""

from pathlib import Path
import re


source = (Path(__file__).parent / "pager-bridge.js").read_text(encoding="utf-8")

assert not re.search(
    r"PAGER_(?:TEMBED|TLORA)_TOKEN\s*\|\|\s*[\"'][^\"']+[\"']",
    source,
), "pager bearer token literal found in plugin source"
assert 'tool.schema.enum(["tlora"]).default("tlora")' in source
assert 'default("tembed")' not in source
assert 'gwAck("opencode", [item.id])' in source
assert 'if (existing.device === "tlora") subs.delete(existingKey)' in source
assert 'return gwRequest("/notify-out", "POST", body, 180000)' in source
assert 'stableMessageID(s.sessionID, `gateway:${item.id}`)' in source
assert 'messageID ? { messageID }' in source
assert 'client.session.message' in source, "retry must recognize an accepted prompt"
assert 'api(d, "/agents")' not in source, "OpenCode room must have one C1 owner"
ask = source[source.index("pager_ask: tool") : source.index("pager_tell: tool")]
assert "const g = await gwSend" in ask, "pager_ask must work with pager Wi-Fi OFF"
subscription_poll = source.index("for (const [key, s] of subs)")
poll = source[source.index("for (const n of r.json.notifications)", subscription_poll):
              source.index("// Карточки, что исчезли", subscription_poll)]
assert poll.index("const delivered = await inject") < poll.index("s.lastSeen.set"), (
    "a failed direct inject must remain retryable"
)

print("OpenCode pager policy tests: OK")
