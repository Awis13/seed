#!/usr/bin/env python3
"""T-Lora Pager ↔ Codex App Server bridge.

The bridge owns one explicit Codex task. It drains only /codex/inbox, submits
each line as a turn, waits for the final assistant message, then sends the
answer through /notify-out as the CODEX C1 thread plus its P1 door card.
"""

from __future__ import annotations

import argparse
from collections import deque
import json
import logging
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any
import urllib.error
import urllib.request


log = logging.getLogger("codex-pager-bridge")


class RpcError(RuntimeError):
    pass


class JsonRpcProcess:
    """Minimal line-delimited JSON-RPC client for `codex app-server`."""

    def __init__(self, command: list[str] | None = None):
        self.command = command or ["codex", "app-server", "--stdio"]
        self.process: subprocess.Popen[str] | None = None
        self._next_id = 1
        self._write_lock = threading.Lock()
        self._pending: dict[int, queue.Queue[dict[str, Any]]] = {}
        self.notifications: queue.Queue[dict[str, Any]] = queue.Queue()

    def start(self) -> None:
        self.process = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self) -> None:
        assert self.process and self.process.stdout
        for raw in self.process.stdout:
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("ignored non-JSON app-server output: %r", raw[:160])
                continue
            request_id = message.get("id")
            if request_id is not None and ("result" in message or "error" in message):
                waiter = self._pending.get(request_id)
                if waiter:
                    waiter.put(message)
                continue
            self.notifications.put(message)

        error = {"error": {"message": "Codex App Server exited"}}
        for waiter in list(self._pending.values()):
            waiter.put(error)

    def _read_stderr(self) -> None:
        assert self.process and self.process.stderr
        for raw in self.process.stderr:
            log.debug("app-server: %s", raw.rstrip())

    def _send(self, message: dict[str, Any]) -> None:
        if not self.process or not self.process.stdin or self.process.poll() is not None:
            raise RpcError("Codex App Server is not running")
        wire = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        with self._write_lock:
            self.process.stdin.write(wire + "\n")
            self.process.stdin.flush()

    def request(self, method: str, params: dict[str, Any], timeout: float = 60) -> Any:
        request_id = self._next_id
        self._next_id += 1
        waiter: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=1)
        self._pending[request_id] = waiter
        try:
            self._send({"id": request_id, "method": method, "params": params})
            try:
                response = waiter.get(timeout=timeout)
            except queue.Empty as exc:
                raise RpcError(f"timeout waiting for {method}") from exc
            if "error" in response:
                raise RpcError(f"{method}: {response['error']}")
            return response.get("result")
        finally:
            self._pending.pop(request_id, None)

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        message: dict[str, Any] = {"method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def close(self) -> None:
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()


class CodexPagerBridge:
    def __init__(
        self,
        rpc: JsonRpcProcess,
        gateway: str,
        state_path: Path,
        workspace: Path,
        thread_id: str | None = None,
        sandbox: str = "read-only",
        approval_policy: str = "never",
        turn_timeout: float = 1800,
    ):
        self.rpc = rpc
        self.gateway = gateway.rstrip("/")
        self.state_path = state_path
        self.workspace = workspace.resolve()
        self.requested_thread_id = thread_id
        self.sandbox = sandbox
        self.approval_policy = approval_policy
        self.turn_timeout = turn_timeout
        self.thread_id: str | None = None
        self.pending: deque[dict[str, Any]] = deque()

    def load_state(self) -> None:
        try:
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"invalid state file {self.state_path}: {exc}") from exc
        if not self.requested_thread_id:
            value = state.get("threadId")
            if isinstance(value, str) and value:
                self.thread_id = value
        entries = state.get("pending") or []
        if isinstance(entries, list):
            self.pending.extend(x for x in entries if isinstance(x, dict) and x.get("text"))

    def save_state(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "threadId": self.thread_id,
            "pending": list(self.pending),
        }
        fd, tmp_name = tempfile.mkstemp(prefix=".codex-pager-", dir=self.state_path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, ensure_ascii=False, indent=2)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp_name, self.state_path)
        finally:
            try:
                os.unlink(tmp_name)
            except FileNotFoundError:
                pass

    def initialize(self) -> None:
        self.rpc.start()
        self.rpc.request(
            "initialize",
            {
                "clientInfo": {
                    "name": "codex-pager-plugin",
                    "title": "Codex T-Lora Pager",
                    "version": "0.1.0",
                },
                "capabilities": {"experimentalApi": False},
            },
        )
        self.rpc.notify("initialized")
        self.load_state()
        self.ensure_thread()

    def ensure_thread(self) -> None:
        candidate = self.requested_thread_id or self.thread_id
        common = {
            "cwd": str(self.workspace),
            "approvalPolicy": self.approval_policy,
            "sandbox": self.sandbox,
        }
        if candidate:
            try:
                result = self.rpc.request(
                    "thread/resume", {"threadId": candidate, **common}
                )
            except RpcError:
                if self.requested_thread_id:
                    raise
                log.warning("persisted pager task is unavailable; creating a replacement")
                candidate = None
        if not candidate:
            result = self.rpc.request(
                "thread/start",
                {
                    **common,
                    "developerInstructions": (
                        "This task is owned by the private T-Lora Pager CODEX room. "
                        "Treat each [pager tlora thread] line as a direct user message. "
                        "Keep final answers concise enough for a small screen."
                    ),
                },
            )
        thread = (result or {}).get("thread") or {}
        resolved = thread.get("id")
        if not isinstance(resolved, str) or not resolved:
            raise RpcError("thread start/resume returned no thread id")
        self.thread_id = resolved
        if not candidate:
            self.rpc.request(
                "thread/name/set",
                {"threadId": resolved, "name": "T-Lora Pager · Codex"},
            )
        self.save_state()
        log.info("bound to Codex task %s", resolved)

    def _http_json(
        self, method: str, path: str, payload: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        data = None
        headers: dict[str, str] = {}
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.gateway + path, data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")[:300]
            raise RuntimeError(f"gateway {path}: HTTP {exc.code}: {detail}") from exc
        return json.loads(body)

    def fetch_inbox(self) -> list[dict[str, Any]]:
        result = self._http_json("GET", "/codex/inbox")
        if not result.get("ok"):
            raise RuntimeError(f"gateway inbox rejected request: {result}")
        items = result.get("items") or []
        return [x for x in items if isinstance(x, dict) and str(x.get("text") or "").strip()]

    def send_reply(self, text: str) -> None:
        result = self._http_json(
            "POST",
            "/notify-out",
            {
                "source": "codex-pager",
                "id": "codex-chat",
                "title": "CODEX",
                "level": "info",
                "body": text,
                "mode": "prefer_wifi",
            },
        )
        if not result.get("ok"):
            raise RuntimeError(f"gateway failed to deliver reply: {result}")

    def run_turn(self, text: str) -> str:
        assert self.thread_id
        result = self.rpc.request(
            "turn/start",
            {
                "threadId": self.thread_id,
                "input": [
                    {
                        "type": "text",
                        "text": f"[pager tlora thread] CODEX — {text}",
                    }
                ],
            },
        )
        turn_id = ((result or {}).get("turn") or {}).get("id")
        if not isinstance(turn_id, str) or not turn_id:
            raise RpcError("turn/start returned no turn id")

        deadline = time.monotonic() + self.turn_timeout
        final_text = ""
        while time.monotonic() < deadline:
            try:
                event = self.rpc.notifications.get(
                    timeout=min(1.0, max(0.01, deadline - time.monotonic()))
                )
            except queue.Empty:
                continue
            method = event.get("method")
            params = event.get("params") or {}
            if params.get("threadId") != self.thread_id:
                continue
            if method == "item/completed" and params.get("turnId") == turn_id:
                item = params.get("item") or {}
                if item.get("type") == "agentMessage" and item.get("phase") in (None, "final_answer"):
                    final_text = str(item.get("text") or "").strip()
            elif method == "turn/completed":
                turn = params.get("turn") or {}
                if turn.get("id") != turn_id:
                    continue
                status = turn.get("status")
                if status != "completed":
                    error = turn.get("error") or status or "unknown error"
                    raise RpcError(f"Codex turn failed: {error}")
                if not final_text:
                    raise RpcError("Codex turn completed without a final answer")
                return final_text
        raise RpcError(f"Codex turn timed out after {self.turn_timeout:.0f}s")

    def run_once(self) -> bool:
        if not self.pending:
            fetched = self.fetch_inbox()
            if fetched:
                self.pending.extend(fetched)
                self.save_state()
        if not self.pending:
            return False

        item = self.pending[0]
        if not item.get("reply"):
            item["reply"] = self.run_turn(str(item["text"]))
            self.save_state()
        self.send_reply(str(item["reply"]))
        self.pending.popleft()
        self.save_state()
        return True

    def run_forever(self, poll_interval: float) -> None:
        self.initialize()
        while True:
            try:
                worked = self.run_once()
            except Exception:
                log.exception("bridge cycle failed; state retained for retry")
                time.sleep(max(2.0, poll_interval))
                continue
            if not worked:
                time.sleep(poll_interval)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true", help="Process at most one queued message")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=os.environ.get("CODEX_PAGER_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s [%(levelname)s] %(message)s",
    )
    workspace = Path(os.environ.get("CODEX_PAGER_WORKSPACE", os.getcwd()))
    state_path = Path(
        os.environ.get(
            "CODEX_PAGER_STATE",
            str(Path.home() / ".codex" / "codex-pager-bridge.json"),
        )
    ).expanduser()
    rpc = JsonRpcProcess(
        [
            os.environ.get("CODEX_PAGER_CODEX_BIN", "codex"),
            "app-server",
            "--stdio",
        ]
    )
    bridge = CodexPagerBridge(
        rpc=rpc,
        gateway=os.environ.get("CODEX_PAGER_GATEWAY", "http://192.168.1.138:8325"),
        state_path=state_path,
        workspace=workspace,
        thread_id=os.environ.get("CODEX_PAGER_THREAD_ID") or None,
        sandbox=os.environ.get("CODEX_PAGER_SANDBOX", "read-only"),
        approval_policy=os.environ.get("CODEX_PAGER_APPROVAL_POLICY", "never"),
        turn_timeout=float(os.environ.get("CODEX_PAGER_TURN_TIMEOUT", "1800")),
    )
    try:
        if args.once:
            bridge.initialize()
            bridge.run_once()
        else:
            bridge.run_forever(float(os.environ.get("CODEX_PAGER_POLL_INTERVAL", "2")))
    finally:
        rpc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
