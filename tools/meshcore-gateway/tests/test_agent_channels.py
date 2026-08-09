import importlib.util
import sys
import types
import unittest
from pathlib import Path


def load_daemon():
    yaml = types.ModuleType("yaml")
    yaml.safe_load = lambda _stream: {}
    sys.modules.setdefault("yaml", yaml)

    aiohttp = types.ModuleType("aiohttp")
    aiohttp.web = types.SimpleNamespace(
        Request=object, Response=object, Application=object
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

    def test_inboxes_are_isolated_and_destructive(self):
        self.assertTrue(self.daemon.enqueue_agent_message("opencode", "open", 1))
        self.assertTrue(self.daemon.enqueue_agent_message("codex", "code", 2))
        self.assertFalse(self.daemon.enqueue_agent_message("unknown", "drop", 3))

        self.assertEqual(
            self.daemon.drain_agent_messages("codex"),
            [{"text": "code", "ts": 2}],
        )
        self.assertEqual(self.daemon.drain_agent_messages("codex"), [])
        self.assertEqual(
            self.daemon.drain_agent_messages("opencode"),
            [{"text": "open", "ts": 1}],
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


if __name__ == "__main__":
    unittest.main()
