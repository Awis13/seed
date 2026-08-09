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

print("OpenCode pager policy tests: OK")
