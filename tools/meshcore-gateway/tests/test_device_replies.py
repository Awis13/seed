import json
import os
import stat
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

from test_agent_channels import load_daemon

_DEVICE_ENV_KEYS = (
    "MESHCORE_GATEWAY_TOKEN",
    "MESHCORE_OPENCODE_TOKEN",
    "MESHCORE_CODEX_TOKEN",
    "MESHCORE_HERMES_NOTIFY_TOKEN",
    "MESHCORE_DEVICE_TOKEN",
)


def _clear_token_env():
    patcher = mock.patch.dict(os.environ, clear=False)
    patcher.start()
    for name in _DEVICE_ENV_KEYS:
        os.environ.pop(name, None)
    return patcher


class _Request:
    def __init__(self, path, method="POST", body=None, token=None, query=None):
        self.path = path
        self.method = method
        self.headers = {"Authorization": f"Bearer {token}"} if token else {}
        self._body = body
        self.rel_url = types.SimpleNamespace(query=query or {})

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


class DeviceScopeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.daemon.cfg = {}
        self.env_patcher = _clear_token_env()
        self.addCleanup(self.env_patcher.stop)

    def test_device_token_read_from_env(self):
        os.environ["MESHCORE_DEVICE_TOKEN"] = "env-secret"
        self.assertEqual(self.daemon.gateway_api_tokens()["device"], "env-secret")

    def test_device_token_falls_back_to_webhook_config(self):
        self.daemon.cfg = {"webhook": {"device_token": "cfg-secret"}}
        self.assertEqual(self.daemon.gateway_api_tokens()["device"], "cfg-secret")

    def test_missing_device_token_is_empty_and_blocks_startup_gate(self):
        tokens = self.daemon.gateway_api_tokens()
        self.assertEqual(tokens["device"], "")
        self.assertIn("device", [k for k, v in tokens.items() if not v])

    async def test_ping_and_reply_resolve_device_scope(self):
        self.assertEqual(
            await self.daemon._request_agent_scope(_Request("/ping", method="GET")),
            "device",
        )
        self.assertEqual(
            await self.daemon._request_agent_scope(_Request("/reply", method="POST")),
            "device",
        )

    async def test_other_methods_and_paths_do_not_resolve_device_scope(self):
        self.assertIsNone(
            await self.daemon._request_agent_scope(_Request("/ping", method="POST"))
        )
        self.assertIsNone(
            await self.daemon._request_agent_scope(_Request("/reply", method="GET"))
        )
        self.assertIsNone(
            await self.daemon._request_agent_scope(_Request("/replies", method="GET"))
        )


class DeviceTokenAuthTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.daemon.cfg = {
            "webhook": {
                "token": "admin-secret",
                "opencode_token": "open-secret",
                "codex_token": "code-secret",
                "hermes_notify_token": "hermes-secret",
                "device_token": "device-secret",
            }
        }
        self.daemon.web.json_response = lambda body, status=200: (status, body)
        self.env_patcher = _clear_token_env()
        self.addCleanup(self.env_patcher.stop)

    async def _auth(self, request):
        async def handler(_request):
            return "ok"

        return await self.daemon.gateway_auth_middleware(request, handler)

    async def test_device_token_accepted_on_ping_and_reply(self):
        self.assertEqual(
            await self._auth(_Request("/ping", method="GET", token="device-secret")),
            "ok",
        )
        self.assertEqual(
            await self._auth(
                _Request(
                    "/reply",
                    body={"key": "k", "reply": "r"},
                    token="device-secret",
                )
            ),
            "ok",
        )

    async def test_admin_token_still_accepted_on_ping_and_reply(self):
        self.assertEqual(
            await self._auth(_Request("/ping", method="GET", token="admin-secret")),
            "ok",
        )
        self.assertEqual(
            await self._auth(
                _Request(
                    "/reply",
                    body={"key": "k", "reply": "r"},
                    token="admin-secret",
                )
            ),
            "ok",
        )

    async def test_device_token_rejected_everywhere_else(self):
        denied = (401, {"ok": False, "error": "unauthorized"})
        rejected_requests = [
            _Request("/notify-out", body={"source": "watcher", "id": "x"}),
            _Request("/send"),
            _Request("/pager"),
            _Request("/opencode/inbox", method="GET"),
            _Request("/codex/inbox", method="GET"),
            _Request("/codex/inbox/ack", body={"ids": []}),
            _Request("/replies", method="GET"),
        ]
        for request in rejected_requests:
            request.headers = {"Authorization": "Bearer device-secret"}
            with self.subTest(path=request.path, method=request.method):
                self.assertEqual(await self._auth(request), denied)

    async def test_replies_requires_admin_and_admin_is_accepted(self):
        self.assertEqual(
            await self._auth(_Request("/replies", method="GET", token="admin-secret")),
            "ok",
        )
        self.assertEqual(
            await self._auth(_Request("/replies", method="GET", token="device-secret")),
            (401, {"ok": False, "error": "unauthorized"}),
        )


class ReplyIntakeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.replies_path = Path(self.tempdir.name) / "replies.json"
        self.daemon.cfg = {"replies": {"path": str(self.replies_path)}}
        self.daemon.web.json_response = lambda body, status=200: (status, body)

    async def test_http_reply_happy_path_stores_and_persists(self):
        status, body = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "on my way"})
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])
        self.assertTrue(body["stored"])
        self.assertFalse(body["duplicate"])
        rows = self.daemon._replies
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["key"], "card-1")
        self.assertEqual(rows[0]["reply"], "on my way")
        self.assertEqual(rows[0]["transport"], "http")
        self.assertEqual(rows[0]["duplicates"], 0)
        self.assertTrue(self.replies_path.exists())
        mode = stat.S_IMODE(os.stat(self.replies_path).st_mode)
        self.assertEqual(mode, 0o600)

    async def test_http_reply_invalid_bodies_are_rejected(self):
        bad_bodies = [
            None,  # request.json() raises
            ["not", "a", "dict"],
            {},
            {"key": "card-1"},
            {"reply": "text"},
            {"key": "", "reply": "text"},
            {"key": "card-1", "reply": ""},
            {"key": "   ", "reply": "text"},
            {"key": 7, "reply": "text"},
            {"key": "card-1", "reply": 7},
            {"key": "x" * 65, "reply": "text"},
        ]
        for body in bad_bodies:
            with self.subTest(body=body):
                status, payload = await self.daemon.webhook_reply(
                    _Request("/reply", body=body)
                )
                self.assertEqual(status, 400)
                self.assertFalse(payload["ok"])
        self.assertEqual(self.daemon._replies, [])

    async def test_duplicate_key_reply_pair_is_stored_once(self):
        first = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "yes"})
        )
        second = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "yes"})
        )
        different = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "no"})
        )
        self.assertTrue(first[1]["stored"])
        self.assertFalse(second[1]["stored"])
        self.assertTrue(second[1]["duplicate"])
        self.assertEqual(second[1]["id"], first[1]["id"])
        self.assertTrue(different[1]["stored"])
        self.assertEqual(len(self.daemon._replies), 2)
        self.assertEqual(self.daemon._replies[0]["duplicates"], 1)

    async def test_first_store_sets_last_ts_equal_to_ts(self):
        status, _body = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "ok"})
        )
        self.assertEqual(status, 200)
        row = self.daemon._replies[0]
        self.assertEqual(row["last_ts"], row["ts"])

    async def test_duplicate_arrival_refreshes_last_ts_but_not_ts(self):
        self.daemon.time = types.SimpleNamespace(time=lambda: 1000)
        await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "ok"})
        )
        # Same key+text arrives again a day later (recurring card, habitual "ok").
        self.daemon.time = types.SimpleNamespace(time=lambda: 1000 + 86400)
        status, body = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "ok"})
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["duplicate"])
        row = self.daemon._replies[0]
        self.assertEqual(row["ts"], 1000)
        self.assertEqual(row["last_ts"], 1000 + 86400)
        self.assertEqual(row["duplicates"], 1)

    async def test_replies_survive_daemon_restart(self):
        await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "persisted"})
        )
        original = self.daemon._replies[0]
        reloaded = load_daemon()
        reloaded.cfg = self.daemon.cfg
        reloaded.load_replies()
        self.assertEqual(
            [(row["key"], row["reply"]) for row in reloaded._replies],
            [("card-1", "persisted")],
        )
        self.assertEqual(reloaded._replies[0]["last_ts"], original["last_ts"])

    async def test_load_replies_backfills_last_ts_for_legacy_rows(self):
        legacy = [{"key": "card-1", "reply": "old", "transport": "http", "ts": 123}]
        self.replies_path.write_text(json.dumps(legacy), encoding="utf-8")
        self.daemon.load_replies()
        self.assertEqual(self.daemon._replies[0]["last_ts"], 123)

    def test_replies_store_is_capped(self):
        for i in range(self.daemon._REPLIES_MAX + 5):
            self.daemon.store_reply(f"card-{i}", "text", "http")
        self.assertEqual(len(self.daemon._replies), self.daemon._REPLIES_MAX)
        self.assertEqual(self.daemon._replies[0]["key"], "card-5")


class MeshR1Tests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.daemon.cfg = {
            "mesh": {"pager_pubkey": "abcdef1234567890"},
            "replies": {"path": str(Path(self.tempdir.name) / "replies.json")},
        }
        self.daemon.web.json_response = lambda body, status=200: (status, body)
        self.daemon.handle_command = lambda _text: self.fail(
            "R1 frame reached the command bot"
        )

    def _event(self, text):
        return types.SimpleNamespace(
            payload={"pubkey_prefix": "abcdef123456", "text": text}
        )

    async def test_valid_r1_is_stored_with_mesh_transport(self):
        await self.daemon.on_mesh_message(self._event("R1|card-1|running late"))
        rows = self.daemon._replies
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["key"], "card-1")
        self.assertEqual(rows[0]["reply"], "running late")
        self.assertEqual(rows[0]["transport"], "mesh")

    async def test_r1_reply_may_contain_pipes(self):
        await self.daemon.on_mesh_message(self._event("R1|card-1|a|b|c"))
        self.assertEqual(self.daemon._replies[0]["reply"], "a|b|c")

    async def test_r1_dedups_against_prior_http_reply(self):
        status, body = await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "ok"})
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["stored"])
        await self.daemon.on_mesh_message(self._event("R1|card-1|ok"))
        rows = self.daemon._replies
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["transport"], "http")
        self.assertEqual(rows[0]["lastTransport"], "mesh")
        self.assertEqual(rows[0]["duplicates"], 1)

    async def test_r1_duplicate_after_http_refreshes_last_ts(self):
        self.daemon.time = types.SimpleNamespace(time=lambda: 2000)
        await self.daemon.webhook_reply(
            _Request("/reply", body={"key": "card-1", "reply": "ok"})
        )
        self.daemon.time = types.SimpleNamespace(time=lambda: 2077)
        await self.daemon.on_mesh_message(self._event("R1|card-1|ok"))
        row = self.daemon._replies[0]
        self.assertEqual(row["ts"], 2000)
        self.assertEqual(row["last_ts"], 2077)
        self.assertEqual(row["lastTransport"], "mesh")

    async def test_malformed_r1_is_dropped_without_reaching_command_bot(self):
        for text in ("R1|", "R1|card-1", "R1||reply", "R1|card-1|", "R1|   |reply"):
            with self.subTest(text=text):
                await self.daemon.on_mesh_message(self._event(text))
        self.assertEqual(self.daemon._replies, [])

    async def test_utf8_r1_reply_survives(self):
        sample = "žluťoučký kůň " + chr(0x1F680)
        await self.daemon.on_mesh_message(self._event("R1|card-1|" + sample))
        self.assertEqual(self.daemon._replies[0]["reply"], sample)

    async def test_unpaired_sender_r1_is_ignored(self):
        event = types.SimpleNamespace(
            payload={"pubkey_prefix": "deadbeef0000", "text": "R1|card-1|spoof"}
        )
        await self.daemon.on_mesh_message(event)
        self.assertEqual(self.daemon._replies, [])


class RepliesEndpointTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.daemon.cfg = {
            "replies": {"path": str(Path(self.tempdir.name) / "replies.json")}
        }
        self.daemon.web.json_response = lambda body, status=200: (status, body)

    async def test_replies_are_listed_newest_first(self):
        self.daemon.store_reply("card-1", "first", "http")
        self.daemon.store_reply("card-2", "second", "mesh")
        status, body = await self.daemon.webhook_replies(
            _Request("/replies", method="GET")
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])
        self.assertEqual(body["count"], 2)
        self.assertEqual([row["reply"] for row in body["items"]], ["second", "first"])

    async def test_key_filter_narrows_results(self):
        self.daemon.store_reply("card-1", "one", "http")
        self.daemon.store_reply("card-2", "two", "http")
        self.daemon.store_reply("card-1", "three", "mesh")
        status, body = await self.daemon.webhook_replies(
            _Request("/replies", method="GET", query={"key": "card-1"})
        )
        self.assertEqual(status, 200)
        self.assertEqual(body["count"], 2)
        self.assertEqual([row["reply"] for row in body["items"]], ["three", "one"])
        self.assertTrue(all(row["key"] == "card-1" for row in body["items"]))


if __name__ == "__main__":
    unittest.main()
