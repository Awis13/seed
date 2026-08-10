#!/usr/bin/env python3
"""Bridge the T-Lora CODEX room to one explicit Codex Desktop task."""

from __future__ import annotations

import argparse
from collections import deque
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any
import urllib.error
import urllib.request
import uuid


log = logging.getLogger("codex-pager-bridge")


class CompanionError(RuntimeError):
    pass


class TerminalTurnError(RuntimeError):
    pass


class AmbiguousTurnError(RuntimeError):
    pass


class PreSubmitRejectedError(RuntimeError):
    pass


class DeliveryAmbiguousError(RuntimeError):
    pass


class AccessibilityCompanion:
    """Invoke the narrowly scoped macOS Accessibility helper."""

    def __init__(self, executable: Path, timeout: float = 20):
        self.executable = executable
        self.timeout = timeout

    def check(self) -> None:
        self._run(["check"])

    def submit(self, thread_id: str, task_title: str, prompt: str) -> None:
        self._run(
            [
                "submit",
                "--thread",
                thread_id,
                "--task-title",
                task_title,
                "--prompt",
                prompt,
            ]
        )

    def _run(self, arguments: list[str]) -> str:
        try:
            result = subprocess.run(
                [str(self.executable), *arguments],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=self.timeout,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise CompanionError(f"companion failed: {exc}") from exc
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()[:300]
            if result.returncode == 64:
                raise PreSubmitRejectedError(detail or "companion rejected input")
            raise CompanionError(detail or f"companion exited {result.returncode}")
        return result.stdout.strip()


def pager_marker(message_id: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]", "_", message_id)[:160]
    if not safe:
        raise ValueError("gateway message id is empty")
    return f"[codex-pager-id:{safe}]"


def pager_prompt(message_id: str, text: str) -> str:
    return f"[pager tlora thread] CODEX — {text}\n\n{pager_marker(message_id)}"


def message_text(payload: dict[str, Any]) -> str:
    content = payload.get("content") or []
    if not isinstance(content, list):
        return ""
    return "".join(
        str(part.get("text") or "")
        for part in content
        if isinstance(part, dict) and part.get("type") in ("input_text", "output_text")
    )


def payload_turn_id(payload: dict[str, Any]) -> str:
    direct = payload.get("turn_id")
    if isinstance(direct, str):
        return direct
    metadata = payload.get("internal_chat_message_metadata_passthrough") or {}
    return str(metadata.get("turn_id") or "") if isinstance(metadata, dict) else ""


def inspect_rollout(path: Path, offset: int, marker: str) -> dict[str, Any]:
    """Read appended complete JSONL records and track one marker-bound turn."""
    found = False
    active_turn = ""
    matched_turn = ""
    final = ""
    next_offset = offset
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            while True:
                line_start = handle.tell()
                raw = handle.readline()
                if not raw:
                    break
                if not raw.endswith(b"\n"):
                    next_offset = line_start
                    break
                next_offset = handle.tell()
                try:
                    row = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                payload = row.get("payload") or {}
                row_type = row.get("type")
                payload_type = payload.get("type")
                row_turn = payload_turn_id(payload)
                if not found:
                    if row_type == "event_msg" and payload_type == "task_started":
                        active_turn = row_turn
                    if (
                        row_type == "response_item"
                        and payload_type == "message"
                        and payload.get("role") == "user"
                        and marker in message_text(payload)
                        and row_turn
                        and row_turn == active_turn
                    ):
                        found = True
                        matched_turn = row_turn
                    continue
                if (
                    row_type == "event_msg"
                    and payload_type == "task_started"
                    and row_turn != matched_turn
                ):
                    return {
                        "status": "failed",
                        "found": True,
                        "turnId": matched_turn,
                        "error": "a newer Codex turn started",
                        "offset": next_offset,
                    }
                if row_type == "response_item" and payload_type == "message":
                    if (
                        row_turn == matched_turn
                        and payload.get("role") == "assistant"
                        and payload.get("phase") == "final_answer"
                    ):
                        final = message_text(payload).strip()
                elif (
                    row_type == "event_msg"
                    and payload_type == "turn_aborted"
                    and row_turn == matched_turn
                ):
                    return {
                        "status": "failed",
                        "found": True,
                        "turnId": matched_turn,
                        "error": "Codex turn was aborted",
                        "offset": next_offset,
                    }
                elif (
                    row_type == "event_msg"
                    and payload_type == "task_complete"
                    and row_turn == matched_turn
                ):
                    if final:
                        return {
                            "status": "completed",
                            "found": True,
                            "turnId": matched_turn,
                            "answer": final,
                            "offset": next_offset,
                        }
                    return {
                        "status": "failed",
                        "found": True,
                        "turnId": matched_turn,
                        "error": "Codex completed without a final answer",
                        "offset": next_offset,
                    }
    except FileNotFoundError:
        return {"status": "pending", "found": False, "offset": offset}
    return {
        "status": "pending",
        "found": found,
        "turnId": matched_turn,
        "offset": next_offset,
    }


class CodexPagerBridge:
    def __init__(
        self,
        companion: AccessibilityCompanion,
        gateway: str,
        gateway_token: str,
        state_path: Path,
        thread_id: str,
        task_title: str,
        sessions_root: Path,
        turn_timeout: float = 1800,
        rollout_poll_interval: float = 0.5,
        submit_grace: float = 5.0,
    ):
        try:
            uuid.UUID(thread_id)
        except (ValueError, AttributeError):
            raise ValueError("CODEX_PAGER_THREAD_ID must be an explicit Codex task id")
        if not task_title.strip():
            raise ValueError("CODEX_PAGER_TASK_TITLE must prove the visible Codex task")
        self.companion = companion
        self.gateway = gateway.rstrip("/")
        self.gateway_token = gateway_token
        self.state_path = state_path
        self.thread_id = thread_id
        self.task_title = task_title
        self.sessions_root = sessions_root.expanduser()
        self.turn_timeout = turn_timeout
        self.rollout_poll_interval = rollout_poll_interval
        self.submit_grace = submit_grace
        self.pending: deque[dict[str, Any]] = deque()
        self.dead_ids: deque[str] = deque(maxlen=500)

    def load_state(self) -> None:
        try:
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"invalid state file {self.state_path}: {exc}") from exc
        saved_thread = state.get("threadId")
        saved_title = state.get("taskTitle")
        entries = state.get("pending") or []
        dead_ids = state.get("deadMessageIds") or []
        if entries and (
            (saved_thread and saved_thread != self.thread_id)
            or (saved_title and saved_title != self.task_title)
        ):
            raise RuntimeError("state contains pending work for a different Codex task")
        if isinstance(entries, list):
            self.pending.extend(
                row
                for row in entries
                if isinstance(row, dict) and row.get("id") and row.get("text")
            )
        if isinstance(dead_ids, list):
            self.dead_ids.extend(str(value) for value in dead_ids if value)

    def save_state(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "threadId": self.thread_id,
            "taskTitle": self.task_title,
            "pending": list(self.pending),
            "deadMessageIds": list(self.dead_ids),
        }
        fd, tmp_name = tempfile.mkstemp(
            prefix=".codex-pager-", dir=self.state_path.parent
        )
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
        self.load_state()
        self.companion.check()
        self._rollout_path()
        log.info("bound to visible Codex task %s", self.thread_id)

    def _rollout_path(self) -> Path:
        matches = list(self.sessions_root.glob(f"**/rollout-*-{self.thread_id}.jsonl"))
        if len(matches) != 1:
            raise RuntimeError(
                f"expected one rollout for task {self.thread_id}, found {len(matches)}"
            )
        return matches[0]

    def _http_json(
        self, method: str, path: str, payload: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        data = None
        headers: dict[str, str] = {}
        if self.gateway_token:
            headers["Authorization"] = f"Bearer {self.gateway_token}"
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.gateway + path, data=data, headers=headers, method=method
        )
        try:
            timeout = 180 if path == "/notify-out" else 15
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            raw_detail = exc.read().decode("utf-8", errors="replace")[:4096]
            detail = raw_detail[:300]
            try:
                error_body = json.loads(raw_detail)
            except json.JSONDecodeError:
                error_body = {}
            if error_body.get("ambiguous"):
                raise DeliveryAmbiguousError(
                    str(error_body.get("error") or detail)
                ) from exc
            raise RuntimeError(f"gateway {path}: HTTP {exc.code}: {detail}") from exc
        return json.loads(body)

    def fetch_inbox(self) -> list[dict[str, Any]]:
        result = self._http_json("GET", "/codex/inbox")
        if not result.get("ok"):
            raise RuntimeError(f"gateway inbox rejected request: {result}")
        items = result.get("items") or []
        return [
            row
            for row in items
            if isinstance(row, dict)
            and row.get("id")
            and str(row.get("text") or "").strip()
        ]

    def ack_inbox(self, message_id: str) -> None:
        result = self._http_json("POST", "/codex/inbox/ack", {"ids": [message_id]})
        if not result.get("ok"):
            raise RuntimeError(f"gateway inbox ack failed: {result}")

    def reject_inbox(self, message_id: str, reason: str) -> None:
        result = self._http_json(
            "POST", "/codex/inbox/reject", {"ids": [message_id], "reason": reason[:160]}
        )
        if not result.get("ok"):
            raise RuntimeError(f"gateway inbox reject failed: {result}")

    def send_reply(self, text: str, delivery_id: str) -> None:
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
                "deliveryId": delivery_id,
            },
        )
        if not result.get("ok"):
            raise RuntimeError(f"gateway failed to deliver reply: {result}")

    def _submission_status(self, item: dict[str, Any]) -> dict[str, Any]:
        return inspect_rollout(
            Path(str(item["rolloutPath"])),
            int(item.get("scanOffset") or 0),
            str(item["marker"]),
        )

    def _submit(self, item: dict[str, Any]) -> None:
        # Permission failures are known to happen before any UI action, so keep
        # the item retryable instead of entering the ambiguous submit state.
        self.companion.check()
        rollout = self._rollout_path()
        marker = pager_marker(str(item["id"]))
        item.update(
            {
                "state": "submitting",
                "marker": marker,
                "rolloutPath": str(rollout),
                "scanOffset": rollout.stat().st_size,
            }
        )
        self.save_state()
        try:
            self.companion.submit(
                self.thread_id,
                self.task_title,
                pager_prompt(str(item["id"]), str(item["text"])),
            )
        except CompanionError as exc:
            deadline = time.monotonic() + self.submit_grace
            while time.monotonic() < deadline:
                status = self._submission_status(item)
                if status.get("found"):
                    item["state"] = "submitted"
                    item["turnId"] = status["turnId"]
                    self.save_state()
                    return
                time.sleep(self.rollout_poll_interval)
            item["state"] = "ambiguous"
            item["ambiguity"] = str(exc)[:160]
            self.save_state()
            raise AmbiguousTurnError(str(exc)) from exc
        item["state"] = "submitted"
        self.save_state()

    def _wait_for_reply(self, item: dict[str, Any]) -> str:
        deadline = time.monotonic() + self.turn_timeout
        while time.monotonic() < deadline:
            status = self._submission_status(item)
            if status.get("found") and not item.get("gatewayAcked"):
                item["turnId"] = status["turnId"]
                self.save_state()
                self.ack_inbox(str(item["id"]))
                item["gatewayAcked"] = True
                self.save_state()
            if status["status"] == "completed":
                return str(status["answer"])
            if status["status"] == "failed":
                raise TerminalTurnError(str(status["error"]))
            if status.get("found") and item.get("state") == "submitting":
                item["state"] = "submitted"
                self.save_state()
            time.sleep(self.rollout_poll_interval)
        raise TerminalTurnError(f"Codex turn timed out after {self.turn_timeout:.0f}s")

    def run_turn(self, item: dict[str, Any]) -> str:
        state = item.get("state")
        if state == "ambiguous":
            status = self._submission_status(item)
            if not status.get("found"):
                raise AmbiguousTurnError(
                    str(item.get("ambiguity") or "submission ambiguous")
                )
            item["state"] = "submitted"
            item["turnId"] = status["turnId"]
            self.save_state()
            state = "submitted"
        if state == "submitting":
            status = self._submission_status(item)
            if status["status"] == "pending" and not status.get("found"):
                item["state"] = "ambiguous"
                item["ambiguity"] = "bridge restarted during UI submission"
                self.save_state()
                raise AmbiguousTurnError(item["ambiguity"])
            item["state"] = "submitted"
            self.save_state()
        elif state != "submitted":
            self._submit(item)
        return self._wait_for_reply(item)

    def run_once(self) -> bool:
        fetched = self.fetch_inbox()
        known = {str(row["id"]) for row in self.pending}
        for row in fetched:
            message_id = str(row["id"])
            if message_id not in self.dead_ids and message_id not in known:
                self.pending.append(dict(row))
                known.add(message_id)
        if fetched:
            self.save_state()
        actionable = next(
            (
                row
                for row in self.pending
                if row.get("state") != "ambiguous"
                and row.get("deliveryState") != "ambiguous"
            ),
            None,
        )
        if actionable is None:
            for row in self.pending:
                if row.get("state") == "ambiguous":
                    status = self._submission_status(row)
                    if status.get("found"):
                        actionable = row
                        break
        if actionable is None:
            return False
        self.pending.remove(actionable)
        self.pending.appendleft(actionable)
        item = actionable
        if not item.get("reply"):
            try:
                item["reply"] = self.run_turn(item)
                item["state"] = "completed"
            except AmbiguousTurnError:
                self.pending.rotate(-1)
                self.save_state()
                return True
            except PreSubmitRejectedError as exc:
                self.reject_inbox(str(item["id"]), str(exc))
                self.dead_ids.append(str(item["id"]))
                self.pending.popleft()
                self.save_state()
                return True
            except TerminalTurnError as exc:
                item["reply"] = (
                    f"Codex could not complete this pager turn: {str(exc)[:160]}"
                )
                item["state"] = "failed"
            self.save_state()
        delivery_id = f"codex-reply:{item['id']}"
        try:
            self.send_reply(str(item["reply"]), delivery_id)
        except DeliveryAmbiguousError:
            item["deliveryState"] = "ambiguous"
            self.save_state()
            return False
        if not item.get("gatewayAcked"):
            self.dead_ids.append(str(item["id"]))
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
    parser.add_argument(
        "--once", action="store_true", help="Process at most one queued message"
    )
    return parser


def load_bridge_secrets() -> dict[str, Any]:
    path = Path(
        os.environ.get(
            "CODEX_PAGER_SECRETS",
            str(Path.home() / ".config" / "codex-pager" / "secrets.json"),
        )
    ).expanduser()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return {}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=os.environ.get("CODEX_PAGER_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s [%(levelname)s] %(message)s",
    )
    secrets = load_bridge_secrets()
    gateway_token = os.environ.get("CODEX_PAGER_GATEWAY_TOKEN") or str(
        secrets.get("gatewayToken") or ""
    )
    if not gateway_token:
        raise SystemExit("CODEX pager gateway token is not configured")
    thread_id = os.environ.get("CODEX_PAGER_THREAD_ID") or ""
    if not thread_id:
        raise SystemExit("CODEX_PAGER_THREAD_ID must name the visible Codex task")
    task_title = os.environ.get("CODEX_PAGER_TASK_TITLE") or ""
    if not task_title:
        raise SystemExit("CODEX_PAGER_TASK_TITLE must prove the visible Codex task")
    state_path = Path(
        os.environ.get(
            "CODEX_PAGER_STATE",
            str(Path.home() / ".codex" / "codex-pager-bridge.json"),
        )
    ).expanduser()
    default_companion = Path(__file__).parents[1] / "bin" / "codex-pager-companion"
    companion = AccessibilityCompanion(
        Path(
            os.environ.get("CODEX_PAGER_COMPANION", str(default_companion))
        ).expanduser()
    )
    bridge = CodexPagerBridge(
        companion=companion,
        gateway=os.environ.get("CODEX_PAGER_GATEWAY", "http://192.168.1.138:8325"),
        gateway_token=gateway_token,
        state_path=state_path,
        thread_id=thread_id,
        task_title=task_title,
        sessions_root=Path(
            os.environ.get(
                "CODEX_PAGER_SESSIONS", str(Path.home() / ".codex" / "sessions")
            )
        ),
        turn_timeout=float(os.environ.get("CODEX_PAGER_TURN_TIMEOUT", "1800")),
        rollout_poll_interval=float(
            os.environ.get("CODEX_PAGER_ROLLOUT_POLL_INTERVAL", "0.5")
        ),
        submit_grace=float(os.environ.get("CODEX_PAGER_SUBMIT_GRACE", "5")),
    )
    if args.once:
        bridge.initialize()
        bridge.run_once()
    else:
        bridge.run_forever(float(os.environ.get("CODEX_PAGER_POLL_INTERVAL", "2")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
