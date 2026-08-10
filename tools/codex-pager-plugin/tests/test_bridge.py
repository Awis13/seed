import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "scripts" / "codex_pager_bridge.py"
SPEC = importlib.util.spec_from_file_location("codex_pager_bridge", MODULE_PATH)
BRIDGE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(BRIDGE)

THREAD_ID = "01900000-0000-7000-8000-000000000000"
TASK_TITLE = "Pager proxy integration"
TURN_ID = "01900000-0000-7000-8000-000000000001"


class FakeCompanion:
    def __init__(self, check_error=None, submit_error=None):
        self.check_error = check_error
        self.submit_error = submit_error
        self.checked = 0
        self.submissions = []

    def check(self):
        self.checked += 1
        if self.check_error:
            raise self.check_error

    def submit(self, thread_id, task_title, prompt):
        self.submissions.append((thread_id, task_title, prompt))
        if self.submit_error:
            raise self.submit_error


class TestBridge(BRIDGE.CodexPagerBridge):
    __test__ = False

    def __init__(self, *args, inbox=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.inbox = list(inbox or [])
        self.sent = []
        self.acked = []
        self.rejected = []

    def fetch_inbox(self):
        items, self.inbox = self.inbox, []
        return items

    def send_reply(self, text, delivery_id):
        self.sent.append((text, delivery_id))

    def ack_inbox(self, message_id):
        self.acked.append(message_id)

    def reject_inbox(self, message_id, reason):
        self.rejected.append((message_id, reason))


def row(row_type, payload):
    return json.dumps({"type": row_type, "payload": payload}, ensure_ascii=False) + "\n"


def started_row(turn_id=TURN_ID):
    return row("event_msg", {"type": "task_started", "turn_id": turn_id})


def user_row(text, turn_id=TURN_ID):
    return row(
        "response_item",
        {
            "type": "message",
            "role": "user",
            "content": [{"type": "input_text", "text": text}],
            "internal_chat_message_metadata_passthrough": {"turn_id": turn_id},
        },
    )


def answer_row(text, turn_id=TURN_ID):
    return row(
        "response_item",
        {
            "type": "message",
            "role": "assistant",
            "phase": "final_answer",
            "content": [{"type": "output_text", "text": text}],
            "internal_chat_message_metadata_passthrough": {"turn_id": turn_id},
        },
    )


class BridgeTests(unittest.TestCase):
    def make_bridge(self, directory, inbox=None, companion=None):
        root = Path(directory)
        rollout = root / f"rollout-test-{THREAD_ID}.jsonl"
        rollout.touch(exist_ok=True)
        return TestBridge(
            companion=companion or FakeCompanion(),
            gateway="http://gateway.invalid",
            gateway_token="test-token",
            state_path=root / "state.json",
            thread_id=THREAD_ID,
            task_title=TASK_TITLE,
            sessions_root=root,
            rollout_poll_interval=0.001,
            turn_timeout=0.05,
            submit_grace=0.005,
            inbox=inbox,
        )

    def test_requires_explicit_valid_task_id(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "explicit Codex task id"):
                TestBridge(
                    companion=FakeCompanion(),
                    gateway="http://gateway.invalid",
                    gateway_token="token",
                    state_path=Path(directory) / "state.json",
                    thread_id="newest",
                    task_title=TASK_TITLE,
                    sessions_root=Path(directory),
                )

    def test_initialize_checks_accessibility_and_exact_rollout(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion()
            bridge = self.make_bridge(directory, companion=companion)
            bridge.initialize()
            self.assertEqual(companion.checked, 1)

    def test_unicode_prompt_contains_stable_marker(self):
        prompt = BRIDGE.pager_prompt("msg-42", "こんにちは 🚀")
        self.assertIn("こんにちは 🚀", prompt)
        self.assertTrue(prompt.endswith("[codex-pager-id:msg-42]"))

    def test_ax_submit_does_not_ack_before_rollout_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion()
            bridge = self.make_bridge(
                directory,
                inbox=[{"id": "msg-1", "text": "hello"}],
                companion=companion,
            )

            def answer(item):
                self.assertEqual(len(companion.submissions), 1)
                self.assertEqual(bridge.acked, [])
                return "answer"

            bridge._wait_for_reply = answer
            self.assertTrue(bridge.run_once())
            self.assertEqual(companion.submissions[0][0], THREAD_ID)
            self.assertEqual(companion.submissions[0][1], TASK_TITLE)
            self.assertIn("[codex-pager-id:msg-1]", companion.submissions[0][2])
            self.assertEqual(bridge.sent, [("answer", "codex-reply:msg-1")])

    def test_failed_submission_is_not_acked(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion(check_error=BRIDGE.CompanionError("no access"))
            bridge = self.make_bridge(
                directory,
                inbox=[{"id": "msg-1", "text": "hello"}],
                companion=companion,
            )
            with self.assertRaises(BRIDGE.CompanionError):
                bridge.run_once()
            self.assertEqual(bridge.acked, [])
            saved = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertNotIn("state", saved["pending"][0])

    def test_ui_failure_after_preflight_is_ambiguous_not_retried(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion(submit_error=BRIDGE.CompanionError("no composer"))
            bridge = self.make_bridge(
                directory,
                inbox=[{"id": "msg-1", "text": "hello"}],
                companion=companion,
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.acked, [])
            saved = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["pending"][0]["state"], "ambiguous")
            self.assertEqual(saved["deadMessageIds"], [])
            self.assertEqual(bridge.sent, [])
            self.assertFalse(bridge.run_once())
            self.assertEqual(len(companion.submissions), 1)

    def test_proven_pre_submit_failure_uses_durable_reject(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion(
                check_error=BRIDGE.PreSubmitRejectedError("invalid target")
            )
            bridge = self.make_bridge(
                directory,
                inbox=[{"id": "msg-1", "text": "hello"}],
                companion=companion,
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.rejected, [("msg-1", "invalid target")])
            self.assertEqual(bridge.acked, [])
            self.assertEqual(len(bridge.pending), 0)

    def test_companion_error_reconciles_marker_during_grace(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion()
            bridge = self.make_bridge(
                directory,
                inbox=[{"id": "msg-1", "text": "hello"}],
                companion=companion,
            )
            rollout = bridge._rollout_path()

            def submitted_then_failed(thread_id, task_title, prompt):
                companion.submissions.append((thread_id, task_title, prompt))
                marker = BRIDGE.pager_marker("msg-1")
                rollout.write_text(
                    started_row()
                    + user_row(f"request\n{marker}")
                    + answer_row("answer")
                    + row("event_msg", {"type": "task_complete", "turn_id": TURN_ID}),
                    encoding="utf-8",
                )
                raise BRIDGE.CompanionError("lost helper response")

            companion.submit = submitted_then_failed
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.acked, ["msg-1"])
            self.assertEqual(bridge.sent[0], ("answer", "codex-reply:msg-1"))

    def test_ambiguous_downlink_is_retained_without_automatic_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "reply": "answer",
                    "state": "completed",
                    "gatewayAcked": True,
                }
            )
            calls = []

            def ambiguous(_text, _delivery_id):
                calls.append(True)
                raise BRIDGE.DeliveryAmbiguousError("manual resolution required")

            bridge.send_reply = ambiguous
            self.assertFalse(bridge.run_once())
            self.assertEqual(calls, [True])
            self.assertEqual(bridge.pending[0]["deliveryState"], "ambiguous")
            self.assertFalse(bridge.run_once())
            self.assertEqual(calls, [True])

    def test_ambiguous_restart_refuses_duplicate_submission(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion()
            bridge = self.make_bridge(directory, companion=companion)
            rollout = bridge._rollout_path()
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "state": "submitting",
                    "marker": BRIDGE.pager_marker("msg-1"),
                    "rolloutPath": str(rollout),
                    "scanOffset": 0,
                }
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(companion.submissions, [])
            self.assertEqual(bridge.acked, [])
            self.assertEqual(list(bridge.dead_ids), [])
            self.assertEqual(bridge.pending[0]["state"], "ambiguous")

    def test_restart_recovers_marker_without_resubmission(self):
        with tempfile.TemporaryDirectory() as directory:
            companion = FakeCompanion()
            bridge = self.make_bridge(directory, companion=companion)
            rollout = bridge._rollout_path()
            marker = BRIDGE.pager_marker("msg-1")
            rollout.write_text(
                started_row()
                + user_row(f"request\n{marker}")
                + answer_row("完了 🚀")
                + row("event_msg", {"type": "task_complete", "turn_id": TURN_ID}),
                encoding="utf-8",
            )
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "state": "submitting",
                    "marker": marker,
                    "rolloutPath": str(rollout),
                    "scanOffset": 0,
                }
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(companion.submissions, [])
            self.assertEqual(bridge.acked, ["msg-1"])
            self.assertEqual(bridge.sent, [("完了 🚀", "codex-reply:msg-1")])

    def test_completed_reply_remains_durable_when_delivery_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "reply": "answer",
                    "state": "completed",
                    "gatewayAcked": True,
                }
            )
            bridge.save_state()

            def fail(_text, _delivery_id):
                raise RuntimeError("offline")

            bridge.send_reply = fail
            with self.assertRaises(RuntimeError):
                bridge.run_once()
            saved = json.loads(bridge.state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["pending"][0]["reply"], "answer")

    def test_pending_state_cannot_move_to_another_task(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            bridge.state_path.write_text(
                json.dumps(
                    {
                        "threadId": "019aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                        "pending": [{"id": "msg-1", "text": "hello"}],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "different Codex task"):
                bridge.load_state()

    def test_dead_letter_is_skipped_so_later_gateway_items_can_run(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(
                directory,
                inbox=[
                    {"id": "dead", "text": "old"},
                    {"id": "next", "text": "new"},
                ],
            )
            bridge.dead_ids.append("dead")
            bridge._submit = lambda item: item.update(state="submitted")
            bridge._wait_for_reply = lambda _item: "next answer"
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.sent[0], ("next answer", "codex-reply:next"))

    def test_abort_is_acked_failed_and_released(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            rollout = bridge._rollout_path()
            marker = BRIDGE.pager_marker("msg-1")
            rollout.write_text(
                started_row()
                + user_row(marker)
                + row("event_msg", {"type": "turn_aborted", "turn_id": TURN_ID}),
                encoding="utf-8",
            )
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "state": "submitted",
                    "marker": marker,
                    "rolloutPath": str(rollout),
                    "scanOffset": 0,
                }
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.acked, ["msg-1"])
            self.assertIn("aborted", bridge.sent[0][0])
            self.assertEqual(len(bridge.pending), 0)

    def test_timeout_without_marker_is_dead_lettered_across_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            bridge = self.make_bridge(directory)
            rollout = bridge._rollout_path()
            bridge.pending.append(
                {
                    "id": "msg-1",
                    "text": "hello",
                    "state": "submitted",
                    "marker": BRIDGE.pager_marker("msg-1"),
                    "rolloutPath": str(rollout),
                    "scanOffset": 0,
                }
            )
            self.assertTrue(bridge.run_once())
            self.assertEqual(bridge.acked, [])
            self.assertIn("timed out", bridge.sent[0][0])

            restarted = self.make_bridge(
                directory,
                inbox=[
                    {"id": "msg-1", "text": "old"},
                    {"id": "msg-2", "text": "new"},
                ],
            )
            restarted.load_state()
            restarted._submit = lambda item: item.update(state="submitted")
            restarted._wait_for_reply = lambda _item: "new answer"
            self.assertTrue(restarted.run_once())
            self.assertEqual(restarted.sent[0], ("new answer", "codex-reply:msg-2"))


class RolloutTests(unittest.TestCase):
    def test_ignores_old_answer_and_returns_marker_bound_final(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollout.jsonl"
            marker = BRIDGE.pager_marker("abc")
            path.write_text(
                answer_row("old")
                + row("event_msg", {"type": "task_complete", "turn_id": TURN_ID})
                + started_row()
                + user_row(f"new\n{marker}")
                + answer_row("new answer")
                + row("event_msg", {"type": "task_complete", "turn_id": TURN_ID}),
                encoding="utf-8",
            )
            result = BRIDGE.inspect_rollout(path, 0, marker)
            self.assertEqual(result["status"], "completed")
            self.assertEqual(result["answer"], "new answer")

    def test_partial_json_line_stays_pending_and_is_re_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollout.jsonl"
            marker = BRIDGE.pager_marker("abc")
            complete = started_row() + user_row(f"new\n{marker}")
            path.write_bytes(complete.encode() + b'{"type":"event_msg"')
            first = BRIDGE.inspect_rollout(path, 0, marker)
            self.assertEqual(first["status"], "pending")
            self.assertTrue(first["found"])
            partial_offset = first["offset"]
            with path.open("ab") as handle:
                handle.write(b',"payload":{"type":"task_complete"}}\n')
            second = BRIDGE.inspect_rollout(path, partial_offset, marker)
            self.assertEqual(second["status"], "pending")

    def test_aborted_matching_turn_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollout.jsonl"
            marker = BRIDGE.pager_marker("abc")
            path.write_text(
                started_row()
                + user_row(marker)
                + row("event_msg", {"type": "turn_aborted", "turn_id": TURN_ID}),
                encoding="utf-8",
            )
            result = BRIDGE.inspect_rollout(path, 0, marker)
            self.assertEqual(result["status"], "failed")
            self.assertIn("aborted", result["error"])

    def test_new_turn_before_completion_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollout.jsonl"
            marker = BRIDGE.pager_marker("abc")
            path.write_text(
                started_row()
                + user_row(marker)
                + started_row("01900000-0000-7000-8000-000000000002"),
                encoding="utf-8",
            )
            result = BRIDGE.inspect_rollout(path, 0, marker)
            self.assertEqual(result["status"], "failed")
            self.assertIn("newer Codex turn", result["error"])

    def test_other_turn_final_and_completion_are_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollout.jsonl"
            marker = BRIDGE.pager_marker("abc")
            other = "01900000-0000-7000-8000-000000000099"
            path.write_text(
                started_row()
                + user_row(marker)
                + answer_row("wrong", other)
                + row("event_msg", {"type": "task_complete", "turn_id": other}),
                encoding="utf-8",
            )
            result = BRIDGE.inspect_rollout(path, 0, marker)
            self.assertEqual(result["status"], "pending")
            self.assertEqual(result["turnId"], TURN_ID)


class CompanionProcessTests(unittest.TestCase):
    def test_failure_is_reported_without_triggering_accessibility(self):
        companion = BRIDGE.AccessibilityCompanion(Path("/does/not/exist"), timeout=0.01)
        with self.assertRaisesRegex(BRIDGE.CompanionError, "companion failed"):
            companion.check()


if __name__ == "__main__":
    unittest.main()
