import importlib.util
import json
from pathlib import Path
import queue
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "scripts" / "codex_pager_bridge.py"
SPEC = importlib.util.spec_from_file_location("codex_pager_bridge", MODULE_PATH)
BRIDGE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(BRIDGE)


class FakeRpc:
    def __init__(self):
        self.calls = []
        self.notifications = queue.Queue()

    def start(self):
        self.calls.append(("start", {}))

    def notify(self, method, params=None):
        self.calls.append((method, params or {}))

    def request(self, method, params, timeout=60):
        self.calls.append((method, params))
        if method == "initialize":
            return {}
        if method in ("thread/start", "thread/resume"):
            return {"thread": {"id": params.get("threadId", "pager-thread")}}
        if method == "thread/name/set":
            return {}
        if method == "turn/start":
            turn_id = "turn-1"
            self.notifications.put(
                {
                    "method": "item/completed",
                    "params": {
                        "threadId": params["threadId"],
                        "turnId": turn_id,
                        "item": {
                            "type": "agentMessage",
                            "phase": "final_answer",
                            "text": "Готово 🚀",
                        },
                    },
                }
            )
            self.notifications.put(
                {
                    "method": "turn/completed",
                    "params": {
                        "threadId": params["threadId"],
                        "turn": {"id": turn_id, "status": "completed"},
                    },
                }
            )
            return {"turn": {"id": turn_id}}
        raise AssertionError(method)


class TestBridge(BRIDGE.CodexPagerBridge):
    __test__ = False

    def __init__(self, *args, inbox=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.inbox = list(inbox or [])
        self.sent = []

    def fetch_inbox(self):
        items, self.inbox = self.inbox, []
        return items

    def send_reply(self, text):
        self.sent.append(text)


class BridgeTests(unittest.TestCase):
    def make_bridge(self, directory, inbox=None, thread_id=None):
        return TestBridge(
            rpc=FakeRpc(),
            gateway="http://gateway.invalid",
            state_path=Path(directory) / "state.json",
            workspace=Path(directory),
            thread_id=thread_id,
            inbox=inbox,
        )

    def test_creates_dedicated_named_thread_and_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(
                directory, inbox=[{"text": "тест", "ts": 1}]
            )
            bridge.initialize()
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.sent, ["Готово 🚀"])
            turn = next(params for method, params in bridge.rpc.calls if method == "turn/start")
            self.assertEqual(turn["threadId"], "pager-thread")
            self.assertEqual(
                turn["input"][0]["text"], "[pager tlora thread] CODEX — тест"
            )
            state = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["threadId"], "pager-thread")
            self.assertEqual(state["pending"], [])

    def test_explicit_thread_is_resumed_not_guessed(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory, thread_id="chosen-thread")
            bridge.initialize()
            methods = [method for method, _ in bridge.rpc.calls]
            self.assertIn("thread/resume", methods)
            self.assertNotIn("thread/start", methods)
            resume = next(params for method, params in bridge.rpc.calls if method == "thread/resume")
            self.assertEqual(resume["threadId"], "chosen-thread")

    def test_completed_reply_is_persisted_until_delivery_succeeds(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.thread_id = "pager-thread"
            bridge.pending.append({"text": "hello", "reply": "answer"})
            bridge.save_state()

            def fail(_text):
                raise RuntimeError("offline")

            bridge.send_reply = fail
            with self.assertRaises(RuntimeError):
                bridge.run_once()
            saved = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["pending"][0]["reply"], "answer")


if __name__ == "__main__":
    unittest.main()
