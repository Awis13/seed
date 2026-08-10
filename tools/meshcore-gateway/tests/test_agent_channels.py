import asyncio
import importlib.util
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


def load_daemon():
    yaml = types.ModuleType("yaml")
    yaml.safe_load = lambda _stream: {}
    sys.modules.setdefault("yaml", yaml)

    aiohttp = types.ModuleType("aiohttp")
    aiohttp.web = types.SimpleNamespace(
        Request=object,
        Response=object,
        Application=object,
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
        self.assertLessEqual(
            len(body.encode("utf-8")), self.daemon._MESH_BODY_MAX_BYTES
        )

    def test_delivery_id_makes_c1_message_id_stable(self):
        payload = {
            "source": "codex-pager",
            "id": "codex-chat",
            "body": "answer",
            "deliveryId": "codex-reply:msg-1",
        }
        first = self.daemon.encode_mesh_frames(payload)
        second = self.daemon.encode_mesh_frames(payload)
        self.assertEqual(first, second)
        changed = self.daemon.encode_mesh_frames(
            {**payload, "deliveryId": "codex-reply:msg-2"}
        )
        self.assertNotEqual(first[0].split("|")[2], changed[0].split("|")[2])


class DeliveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_reject_moves_item_to_durable_dead_letter(self):
        daemon = load_daemon()
        with tempfile.TemporaryDirectory() as directory:
            daemon.cfg = {"agent_inbox": {"path": str(Path(directory) / "state.json")}}
            self.assertTrue(daemon.enqueue_agent_message("codex", "bad", 1))
            message_id = daemon.list_agent_messages("codex")[0]["id"]
            self.assertEqual(
                daemon.reject_agent_messages("codex", {message_id}, "pre-submit"), 1
            )
            self.assertEqual(daemon.list_agent_messages("codex"), [])
            self.assertTrue(daemon.enqueue_agent_message("codex", "next", 2))

            reloaded = load_daemon()
            reloaded.cfg = daemon.cfg
            reloaded.load_agent_inboxes()
            self.assertEqual(reloaded._agent_dead_letters["codex"][0]["id"], message_id)
            self.assertEqual(
                [row["text"] for row in reloaded.list_agent_messages("codex")],
                ["next"],
            )

    async def test_concurrent_identical_delivery_is_reserved_once(self):
        daemon = load_daemon()
        with tempfile.TemporaryDirectory() as directory:
            daemon.cfg = {"agent_inbox": {"path": str(Path(directory) / "state.json")}}
            daemon.web.json_response = lambda body, status=200: (status, body)
            started = asyncio.Event()
            release = asyncio.Event()
            calls = []

            async def deliver(payload, mode=None):
                calls.append((payload, mode))
                started.set()
                await release.wait()
                return {"ok": True, "payload": payload}

            daemon.notify_out = deliver

            class Request:
                async def json(self):
                    return {
                        "source": "codex-pager",
                        "id": "codex-chat",
                        "body": "answer",
                        "deliveryId": "codex-reply:concurrent",
                    }

            first_task = asyncio.create_task(daemon.webhook_notify_out(Request()))
            await started.wait()
            second = await daemon.webhook_notify_out(Request())
            self.assertEqual(second[0], 409)
            self.assertTrue(second[1]["ambiguous"])
            self.assertEqual(len(calls), 1)
            release.set()
            first = await first_task
            self.assertEqual(first[0], 200)

    async def test_restart_does_not_resend_ambiguous_reservation(self):
        daemon = load_daemon()
        with tempfile.TemporaryDirectory() as directory:
            daemon.cfg = {"agent_inbox": {"path": str(Path(directory) / "state.json")}}
            payload = {
                "source": "codex-pager",
                "id": "codex-chat",
                "deliveryId": "codex-reply:crash",
            }
            key = daemon._notify_delivery_key(payload)
            daemon._notify_reservations[key] = {"state": "inflight", "reservedAt": 1}
            daemon._persist_agent_inboxes()

            reloaded = load_daemon()
            reloaded.cfg = daemon.cfg
            reloaded.web.json_response = lambda body, status=200: (status, body)
            reloaded.load_agent_inboxes()
            calls = []

            async def forbidden(*_args, **_kwargs):
                calls.append(True)
                return {"ok": True}

            reloaded.notify_out = forbidden

            class Request:
                async def json(self):
                    return {**payload, "body": "answer"}

            result = await reloaded.webhook_notify_out(Request())
            self.assertEqual(result[0], 409)
            self.assertTrue(result[1]["ambiguous"])
            self.assertEqual(calls, [])

    async def test_notify_delivery_dedupe_survives_daemon_restart(self):
        daemon = load_daemon()
        with tempfile.TemporaryDirectory() as directory:
            daemon.cfg = {"agent_inbox": {"path": str(Path(directory) / "state.json")}}
            daemon.web.json_response = lambda body, status=200: (status, body)
            calls = []

            async def deliver(payload, mode=None):
                calls.append((payload, mode))
                return {"ok": True, "payload": payload}

            daemon.notify_out = deliver

            class Request:
                async def json(self):
                    return {
                        "source": "codex-pager",
                        "id": "codex-chat",
                        "body": "answer",
                        "deliveryId": "codex-reply:msg-1",
                    }

            first = await daemon.webhook_notify_out(Request())
            self.assertTrue(first[1]["ok"])
            self.assertEqual(len(calls), 1)

            reloaded = load_daemon()
            reloaded.cfg = daemon.cfg
            reloaded.web.json_response = lambda body, status=200: (status, body)
            reloaded.load_agent_inboxes()
            reloaded.notify_out = lambda *_args, **_kwargs: self.fail(
                "duplicate delivery reached transport"
            )
            duplicate = await reloaded.webhook_notify_out(Request())
            self.assertTrue(duplicate[1]["duplicate"])

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

    async def test_retried_completed_c1_is_enqueued_once(self):
        daemon = load_daemon()
        with tempfile.TemporaryDirectory() as directory:
            daemon.cfg = {
                "mesh": {"pager_pubkey": "abcdef1234567890"},
                "agent_inbox": {"path": str(Path(directory) / "inboxes.json")},
            }
            event = types.SimpleNamespace(
                payload={
                    "pubkey_prefix": "abcdef123456",
                    "text": "C1|codex|abc1234|1|1|u|once",
                }
            )
            await daemon.on_mesh_message(event)
            await daemon.on_mesh_message(event)
            self.assertEqual(
                [row["text"] for row in daemon.list_agent_messages("codex")],
                ["once"],
            )

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

    async def test_agent_tokens_cannot_cross_inboxes_or_spoof_downlink(self):
        daemon = load_daemon()
        daemon.cfg = {
            "webhook": {
                "token": "admin-secret",
                "opencode_token": "open-secret",
                "codex_token": "code-secret",
            }
        }
        daemon.web.json_response = lambda body, status=200: (status, body)

        async def handler(_request):
            return "ok"

        class Request:
            def __init__(self, path, token, body=None):
                self.path = path
                self.method = "POST"
                self.headers = {"Authorization": f"Bearer {token}"}
                self._body = body or {}

            async def json(self):
                return self._body

        self.assertEqual(
            await daemon.gateway_auth_middleware(
                Request("/codex/inbox", "open-secret"), handler
            ),
            (401, {"ok": False, "error": "unauthorized"}),
        )
        self.assertEqual(
            await daemon.gateway_auth_middleware(
                Request("/opencode/inbox", "code-secret"), handler
            ),
            (401, {"ok": False, "error": "unauthorized"}),
        )
        self.assertEqual(
            await daemon.gateway_auth_middleware(
                Request(
                    "/notify-out",
                    "open-secret",
                    {"source": "codex-pager", "id": "codex-chat"},
                ),
                handler,
            ),
            (401, {"ok": False, "error": "unauthorized"}),
        )
        self.assertEqual(
            await daemon.gateway_auth_middleware(
                Request(
                    "/notify-out",
                    "open-secret",
                    {"source": "opencode-pager", "id": "opencode-chat"},
                ),
                handler,
            ),
            "ok",
        )

    def test_full_inbox_refuses_new_message_without_dropping_oldest(self):
        daemon = load_daemon()
        daemon.cfg = {"agent_inbox": {"path": "/dev/null"}}
        original = [
            {"id": str(i), "text": str(i), "ts": i}
            for i in range(daemon._AGENT_INBOX_MAX)
        ]
        daemon._agent_inboxes["codex"] = list(original)
        self.assertFalse(daemon.enqueue_agent_message("codex", "overflow"))
        self.assertEqual(daemon._agent_inboxes["codex"], original)

    def test_shell_commands_are_disabled_by_default(self):
        daemon = load_daemon()
        daemon.cfg = {}
        self.assertEqual(daemon.handle_command("sh id"), "sh: disabled")
        self.assertEqual(daemon.handle_command("reboot"), "reboot: disabled")
        self.assertEqual(
            daemon.cmd_wol("aa:bb:cc:dd:ee:ff; id"), "WOL error: invalid MAC"
        )

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


class _ScopeRequest:
    def __init__(self, path, method="POST", body=None, token=None):
        self.path = path
        self.method = method
        self.headers = {"Authorization": f"Bearer {token}"} if token else {}
        self._body = body

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


class HermesNotifyScopeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.daemon = load_daemon()
        self.daemon.cfg = {}

    def test_hermes_notify_token_read_from_env(self):
        with mock.patch.dict(
            os.environ, {"MESHCORE_HERMES_NOTIFY_TOKEN": "env-secret"}
        ):
            self.assertEqual(
                self.daemon.gateway_api_tokens()["hermes_notify"], "env-secret"
            )

    def test_hermes_notify_token_falls_back_to_webhook_config(self):
        self.daemon.cfg = {"webhook": {"hermes_notify_token": "cfg-secret"}}
        with mock.patch.dict(os.environ, clear=False):
            os.environ.pop("MESHCORE_HERMES_NOTIFY_TOKEN", None)
            tokens = self.daemon.gateway_api_tokens()
        self.assertEqual(tokens["hermes_notify"], "cfg-secret")

    def test_missing_hermes_notify_token_is_empty(self):
        with mock.patch.dict(os.environ, clear=False):
            os.environ.pop("MESHCORE_HERMES_NOTIFY_TOKEN", None)
            tokens = self.daemon.gateway_api_tokens()
        self.assertEqual(tokens["hermes_notify"], "")

    async def test_hermes_note_payload_resolves_hermes_notify_scope(self):
        scope = await self.daemon._request_agent_scope(
            _ScopeRequest(
                "/notify-out", body={"source": "hermes", "id": "hermes-note"}
            )
        )
        self.assertEqual(scope, "hermes_notify")

    async def test_path_prefix_scopes_still_resolve(self):
        self.assertEqual(
            await self.daemon._request_agent_scope(_ScopeRequest("/opencode/inbox")),
            "opencode",
        )
        self.assertEqual(
            await self.daemon._request_agent_scope(_ScopeRequest("/codex/inbox")),
            "codex",
        )

    async def test_near_miss_hermes_payloads_do_not_resolve_the_scope(self):
        self.assertIsNone(
            await self.daemon._request_agent_scope(
                _ScopeRequest("/notify-out", body={"source": "hermes", "id": "other"})
            )
        )
        self.assertIsNone(
            await self.daemon._request_agent_scope(
                _ScopeRequest(
                    "/notify-out", body={"source": "other", "id": "hermes-note"}
                )
            )
        )

    async def test_non_post_hermes_payload_does_not_resolve_the_scope(self):
        self.assertIsNone(
            await self.daemon._request_agent_scope(
                _ScopeRequest(
                    "/notify-out",
                    method="GET",
                    body={"source": "hermes", "id": "hermes-note"},
                )
            )
        )

    async def test_generic_notify_out_and_ping_fall_back_to_admin(self):
        self.assertIsNone(
            await self.daemon._request_agent_scope(
                _ScopeRequest("/notify-out", body={"source": "watcher", "id": "x"})
            )
        )
        self.assertIsNone(
            await self.daemon._request_agent_scope(
                _ScopeRequest("/ping", method="GET")
            )
        )

    async def test_hermes_token_scoped_to_hermes_note_only(self):
        self.daemon.cfg = {
            "webhook": {"token": "admin-secret", "hermes_notify_token": "hermes-secret"}
        }
        self.daemon.web.json_response = lambda body, status=200: (status, body)

        async def handler(_request):
            return "ok"

        with mock.patch.dict(os.environ, clear=False):
            os.environ.pop("MESHCORE_HERMES_NOTIFY_TOKEN", None)
            os.environ.pop("MESHCORE_GATEWAY_TOKEN", None)
            allowed = await self.daemon.gateway_auth_middleware(
                _ScopeRequest(
                    "/notify-out",
                    body={"source": "hermes", "id": "hermes-note"},
                    token="hermes-secret",
                ),
                handler,
            )
            denied = await self.daemon.gateway_auth_middleware(
                _ScopeRequest(
                    "/notify-out",
                    body={"source": "watcher", "id": "generic"},
                    token="hermes-secret",
                ),
                handler,
            )
            admin_ok = await self.daemon.gateway_auth_middleware(
                _ScopeRequest(
                    "/notify-out",
                    body={"source": "hermes", "id": "hermes-note"},
                    token="admin-secret",
                ),
                handler,
            )
        self.assertEqual(allowed, "ok")
        self.assertEqual(denied, (401, {"ok": False, "error": "unauthorized"}))
        self.assertEqual(admin_ok, "ok")


async def _async_next(iterator):
    return next(iterator)


if __name__ == "__main__":
    unittest.main()
