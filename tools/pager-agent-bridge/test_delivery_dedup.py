#!/usr/bin/env python3
"""Regression tests for pager-agent bridge delivery identity."""

import importlib.util
from pathlib import Path
from unittest import mock


MODULE = Path(__file__).with_name("pager_agent_bridge.py")
spec = importlib.util.spec_from_file_location("pager_agent_bridge", MODULE)
bridge = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(bridge)


def test_keyed_reservation_and_keyless_semantics() -> None:
    bridge._deliveries.clear()
    assert bridge._delivery_reserve("tx-1234")
    assert not bridge._delivery_reserve("tx-1234")
    assert bridge._delivery_reserve("")
    assert bridge._delivery_reserve("")


def test_reply_uses_one_backing_card() -> None:
    calls = []

    def fake_http(method, url, body=None, headers=None, timeout=0):
        calls.append((method, url, body, timeout))
        return 200, {}

    with mock.patch.object(bridge, "PAGER_TOKEN", "configured"), \
         mock.patch.object(bridge, "_http_json", side_effect=fake_http):
        bridge.pager_push("hermes", "answer", "tx-1234")

    assert len(calls) == 1
    assert calls[0][1].endswith("/agents/inbound")
    assert calls[0][2] == {
        "agent": "hermes", "text": "answer", "id": "tx-1234.r"
    }


if __name__ == "__main__":
    test_keyed_reservation_and_keyless_semantics()
    test_reply_uses_one_backing_card()
    print("pager agent bridge delivery dedup tests: OK")
