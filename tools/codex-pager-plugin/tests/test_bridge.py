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
        self.turns = []

    def start(self):
        self.calls.append(("start", {}))

    def notify(self, method, params=None):
        self.calls.append((method, params or {}))

    def is_alive(self):
        return True

    def request(self, method, params, timeout=60):
        self.calls.append((method, params))
        if method == "initialize":
            return {}
        if method in ("thread/start", "thread/resume"):
            return {"thread": {"id": params.get("threadId", "pager-thread")}}
        if method == "thread/name/set":
            return {}
        if method == "thread/read":
            return {"thread": {"id": params["threadId"], "turns": self.turns}}
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
        self.acked = []

    def fetch_inbox(self):
        items, self.inbox = self.inbox, []
        return items

    def send_reply(self, text):
        self.sent.append(text)

    def ack_inbox(self, message_id):
        self.acked.append(message_id)


class BridgeTests(unittest.TestCase):
    def make_bridge(self, directory, inbox=None, thread_id=None):
        return TestBridge(
            rpc=FakeRpc(),
            gateway="http://gateway.invalid",
            gateway_token="test-token",
            state_path=Path(directory) / "state.json",
            workspace=Path(directory),
            thread_id=thread_id,
            inbox=inbox,
        )

    def test_creates_dedicated_named_thread_and_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(
                directory, inbox=[{"id": "msg-1", "text": "тест", "ts": 1}]
            )
            bridge.initialize()
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.sent, ["Готово 🚀"])
            self.assertEqual(bridge.acked, ["msg-1"])
            start = next(params for method, params in bridge.rpc.calls if method == "thread/start")
            self.assertEqual(start["cwd"], str(Path(directory).resolve()))
            self.assertEqual(start["approvalPolicy"], "never")
            self.assertEqual(start["sandbox"], "read-only")
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
            bridge.pending.append({
                "id": "msg-1", "text": "hello", "reply": "answer",
                "gatewayAcked": True,
            })
            bridge.save_state()

            def fail(_text):
                raise RuntimeError("offline")

            bridge.send_reply = fail
            with self.assertRaises(RuntimeError):
                bridge.run_once()
            saved = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["pending"][0]["reply"], "answer")

    def test_ambiguous_start_is_not_replayed(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.thread_id = "pager-thread"
            bridge.pending.append({
                "id": "msg-1",
                "text": "do once",
                "gatewayAcked": True,
                "state": "starting",
            })
            self.assertTrue(bridge.run_once())
            self.assertEqual(
                [method for method, _ in bridge.rpc.calls if method == "turn/start"],
                [],
            )
            self.assertIn("please resend", bridge.sent[0])

    def test_completed_turn_is_recovered_without_duplicate_start(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.thread_id = "pager-thread"
            bridge.rpc.turns = [{
                "id": "turn-old",
                "status": "completed",
                "items": [{
                    "type": "agentMessage",
                    "phase": "final_answer",
                    "text": "recovered answer",
                }],
            }]
            bridge.pending.append({
                "id": "msg-1",
                "text": "do once",
                "gatewayAcked": True,
                "state": "running",
                "turnId": "turn-old",
            })
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.sent, ["recovered answer"])
            self.assertNotIn("turn/start", [method for method, _ in bridge.rpc.calls])


class JsonRpcTests(unittest.TestCase):
    def test_server_request_gets_immediate_error_response(self):
        class Sink:
            def __init__(self):
                self.rows = []

            def write(self, value):
                self.rows.append(value)

            def flush(self):
                pass

        sink = Sink()

        class Process:
            stdout = iter([
                json.dumps({
                    "id": 77,
                    "method": "tool/requestUserInput",
                    "params": {"questions": []},
                }) + "\n"
            ])
            stderr = iter([])
            stdin = sink

            @staticmethod
            def poll():
                return None

        rpc = BRIDGE.JsonRpcProcess(["fake"])
        rpc.process = Process()
        rpc._read_stdout()
        response = json.loads(sink.rows[0])
        self.assertEqual(response["id"], 77)
        self.assertEqual(response["error"]["code"], -32601)


if __name__ == "__main__":
    unittest.main()
