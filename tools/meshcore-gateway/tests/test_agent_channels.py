import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path


def load_daemon():
    yaml = types.ModuleType("yaml")
    yaml.safe_load = lambda _stream: {}
    sys.modules.setdefault("yaml", yaml)

    aiohttp = types.ModuleType("aiohttp")
    aiohttp.web = types.SimpleNamespace(
        Request=object, Response=object, Application=object,
        middleware=lambda function: function,
    )
    sys.modules.setdefault("aiohttp", aiohttp)

    meshcore = types.ModuleType("meshcore")
    meshcore.EventType = types.SimpleNamespace(ERROR="error")
    meshcore.MeshCore = object
    sys.modules.setdefault("meshcore", meshcore)

    path = Path(__file__).parents[1] / "meshcore_daemon.py"
    spec = importlib.util.spec_from_file_location("meshcore_daemon_test", path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class AgentChannelTests(unittest.TestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.daemon.cfg = {
            "agent_inbox": {"path": str(Path(self.tempdir.name) / "inboxes.json")}
        }

    def test_inboxes_are_isolated_durable_and_explicitly_acked(self):
        self.assertTrue(self.daemon.enqueue_agent_message("opencode", "open", 1))
        self.assertTrue(self.daemon.enqueue_agent_message("codex", "code", 2))
        self.assertFalse(self.daemon.enqueue_agent_message("unknown", "drop", 3))

        codex = self.daemon.list_agent_messages("codex")
        opencode = self.daemon.list_agent_messages("opencode")
        self.assertEqual([(x["text"], x["ts"]) for x in codex], [("code", 2)])
        self.assertEqual([(x["text"], x["ts"]) for x in opencode], [("open", 1)])
        self.assertEqual(self.daemon.list_agent_messages("codex"), codex)
        self.assertEqual(self.daemon.ack_agent_messages("codex", {codex[0]["id"]}), 1)
        self.assertEqual(self.daemon.list_agent_messages("codex"), [])

        reloaded = load_daemon()
        reloaded.cfg = self.daemon.cfg
        reloaded.load_agent_inboxes()
        self.assertEqual(
            [x["text"] for x in reloaded.list_agent_messages("opencode")], ["open"]
        )

    def test_only_exact_source_and_id_open_a_chat_door(self):
        exact = {"source": "codex-pager", "id": "codex-chat"}
        wrong_source = {"source": "opencode-pager", "id": "codex-chat"}
        wrong_id = {"source": "codex-pager", "id": "opencode-chat"}

        agent, channel = self.daemon._agent_channel_for_payload(exact)
        self.assertEqual(agent, "codex")
        self.assertEqual(channel["title"], "CODEX")
        self.assertIsNone(self.daemon._agent_channel_for_payload(wrong_source))
        self.assertIsNone(self.daemon._agent_channel_for_payload(wrong_id))
        self.assertEqual(self.daemon.encode_mesh_frames(wrong_source), [])

    def test_only_paired_pager_can_submit_agent_chat(self):
        self.daemon.cfg = {"mesh": {"pager_pubkey": "abcdef1234567890"}}
        self.assertTrue(self.daemon.is_paired_pager_sender("abcdef123456"))
        self.assertFalse(self.daemon.is_paired_pager_sender("deadbeef0000"))
        self.assertFalse(self.daemon.is_paired_pager_sender("abcdef"))
        self.assertFalse(self.daemon.is_paired_pager_sender("unknown"))

    def test_codex_reply_is_c1_and_utf8_safe(self):
        self.daemon.cfg = {"mesh_msg_limit": 42}
        body = "0123456789 " + (("тест " + chr(0x1F680) + " ") * 12)
        frames = self.daemon.encode_mesh_frames(
            {
                "source": "codex-pager",
                "id": "codex-chat",
                "title": "CODEX",
                "body": body,
            }
        )
        self.assertGreater(len(frames), 1)
        chunks = []
        for frame in frames:
            parts = frame.split("|", 6)
            self.assertEqual(parts[0], "C1")
            self.assertEqual(parts[1], "codex")
            self.assertEqual(parts[5], "a")
            frame.encode("utf-8")
            chunks.append(parts[6])
        self.assertEqual("".join(chunks), body)

    def test_long_unicode_reply_fits_firmware_contract(self):
        self.daemon.cfg["mesh_msg_limit"] = 42
        frames = self.daemon.encode_mesh_frames(
            {
                "source": "codex-pager",
                "id": "codex-chat",
                "body": chr(0x1F680) * 2000,
            }
        )
        self.assertLessEqual(len(frames), 64)
        body = "".join(frame.split("|", 6)[6] for frame in frames)
        self.assertLessEqual(len(body.encode("utf-8")), self.daemon._MESH_BODY_MAX_BYTES)


class DeliveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_unpaired_mesh_sender_never_reaches_command_bot(self):
        daemon = load_daemon()
        daemon.cfg = {
            "mesh": {"pager_pubkey": "abcdef1234567890"},
            "allow_shell_commands": True,
        }
        daemon.handle_command = lambda _text: self.fail("command bot was reached")
        event = types.SimpleNamespace(
            payload={"pubkey_prefix": "deadbeef0000", "text": "sh id"}
        )
        await daemon.on_mesh_message(event)

    async def test_sensitive_http_requires_bearer_token(self):
        daemon = load_daemon()
        daemon.cfg = {"webhook": {"token": "secret"}}
        daemon.web.json_response = lambda body, status=200: (status, body)
        reached = []

        async def handler(_request):
            reached.append(True)
            return "ok"

        denied = await daemon.gateway_auth_middleware(
            types.SimpleNamespace(path="/notify-out", headers={}), handler
        )
        self.assertEqual(denied[0], 401)
        self.assertEqual(reached, [])
        allowed = await daemon.gateway_auth_middleware(
            types.SimpleNamespace(
                path="/notify-out", headers={"Authorization": "Bearer secret"}
            ),
            handler,
        )
        self.assertEqual(allowed, "ok")
        self.assertEqual(reached, [True])

    def test_shell_commands_are_disabled_by_default(self):
        daemon = load_daemon()
        daemon.cfg = {}
        self.assertEqual(daemon.handle_command("sh id"), "sh: disabled")
        self.assertEqual(daemon.handle_command("reboot"), "reboot: disabled")

    async def test_wifi_chat_still_sends_full_c1_history(self):
        daemon = load_daemon()
        daemon.cfg = {"notify": {"mode": "prefer_wifi"}}
        daemon.pager_post_notify = lambda _payload: (200, "ok")
        calls = []

        async def mesh_send(payload):
            calls.append(payload)
            return True, "acked"

        daemon.mesh_send_p1 = mesh_send
        result = await daemon.notify_out(
            {"source": "codex-pager", "id": "codex-chat", "body": "answer"}
        )
        self.assertTrue(result["ok"])
        self.assertEqual(len(calls), 1)
        self.assertFalse(calls[0].get("_force_p1", False))
        self.assertTrue(result["mesh"]["thread_ok"])

    async def test_door_success_never_masks_failed_thread(self):
        daemon = load_daemon()
        daemon.cfg = {"notify": {"mode": "prefer_wifi"}}
        daemon.pager_post_notify = lambda _payload: (0, "offline")
        outcomes = iter([(False, "thread failed"), (True, "door acked")])
        daemon.mesh_send_p1 = lambda _payload: _async_next(outcomes)
        result = await daemon.notify_out(
            {"source": "codex-pager", "id": "codex-chat", "body": "answer"}
        )
        self.assertFalse(result["ok"])
        self.assertFalse(result["mesh"]["thread_ok"])
        self.assertTrue(result["mesh"]["visible_ok"])


async def _async_next(iterator):
    return next(iterator)


if __name__ == "__main__":
    unittest.main()
