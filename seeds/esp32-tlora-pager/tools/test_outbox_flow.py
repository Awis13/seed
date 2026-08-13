#!/usr/bin/env python3
"""Firmware wiring pins for durable notification replies."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
hw = (ROOT / "src/hw_ui.cpp").read_text(encoding="utf-8")

assert "reply_up_pending" not in main
assert "outbox_enqueue(&g_outbox, OUTBOX_KIND_REPLY" in main
assert "if (outbox_persist()) return true;" in main
assert "case REPLY_HTTP_AUTH:" not in main  # enum result is handled by equality
assert "http == REPLY_HTTP_AUTH" in main
assert "OUTBOX_STATE_AUTH" in main
assert "OUTBOX_ATTEMPTS_MAX" in main
assert "outbox_nudge_pending(&g_outbox);" in main
assert 'ui_go_clock(queued ? "queued" : "saved")' in main
assert "outbox_reply_status(v.key)" in main
assert "outbox_reply_mark(v.key)" in main
assert 'delivery && delivery[0] ?' not in hw  # renderer uses explicit branch
assert "if (delivery && delivery[0])" in hw

print("outbox flow tests: OK")
