#!/usr/bin/env python3
"""
pager-agent-bridge — thin LAN bridge: T-Lora AGENTS skill ↔ Hermes API server.

Pager contract (seed skills/agents.cpp):
  POST /v1/chat  JSON { "agent", "session", "text", "from" }
  → 2xx means "accepted"; real reply is pushed later.

Hermes (docs: api-server.md):
  POST http://127.0.0.1:8642/v1/chat/completions
  Authorization: Bearer $API_SERVER_KEY
  OpenAI-compatible; full agent tool loop runs server-side.

Reply path:
  1) POST pager /agents/inbound  {agent, text}  — thread on device
  2) POST pager /notify          {source, title, body, level} — buzz + card

Env (or flags):
  BRIDGE_ADDR=0.0.0.0:8091
  HERMES_URL=http://127.0.0.1:8642
  HERMES_KEY=...
  PAGER_URL=http://192.168.1.116:8080
  PAGER_TOKEN=...
"""

from __future__ import annotations

import json
import os
import sys
import threading
import traceback
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

BRIDGE_ADDR = os.environ.get("BRIDGE_ADDR", "0.0.0.0:8091")
HERMES_URL = os.environ.get("HERMES_URL", "http://127.0.0.1:8642").rstrip("/")
HERMES_KEY = os.environ.get("HERMES_KEY", "")
PAGER_URL = os.environ.get("PAGER_URL", "http://192.168.1.116:8080").rstrip("/")
PAGER_TOKEN = os.environ.get("PAGER_TOKEN", "")

# Long replies are fine — the pager scrolls. Still ASCII-only for the panel font.
SYSTEM = (
    "You are Hermes talking to Nikolaj on a pocket pager (480x222 screen, ASCII only). "
    "Reply in English. You may write several short paragraphs; he can scroll. "
    "Prefer clear prose under ~1500 characters when possible. "
    "No markdown fences, no emoji, no Cyrillic (panel cannot render it)."
)


def _http_json(
    method: str,
    url: str,
    body: dict | None = None,
    headers: dict | None = None,
    timeout: float = 120.0,
) -> tuple[int, Any]:
    data = None
    hdrs = {"Accept": "application/json"}
    if headers:
        hdrs.update(headers)
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            code = resp.getcode()
    except urllib.error.HTTPError as e:
        raw = e.read()
        code = e.code
    except Exception as e:
        return 0, {"error": str(e)}
    if not raw:
        return code, {}
    try:
        return code, json.loads(raw.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return code, {"raw": raw.decode("utf-8", errors="replace")[:500]}


def hermes_chat(session: str, text: str) -> str:
    if not HERMES_KEY:
        return "(bridge: HERMES_KEY not set)"
    # Session-ish continuity via system + single user turn for now.
    # Full multi-turn can load history later from the pager thread.
    payload = {
        "model": "hermes-agent",
        "messages": [
            {"role": "system", "content": SYSTEM + f" Session={session}."},
            {"role": "user", "content": text},
        ],
        "stream": False,
    }
    code, data = _http_json(
        "POST",
        f"{HERMES_URL}/v1/chat/completions",
        body=payload,
        headers={"Authorization": f"Bearer {HERMES_KEY}"},
        timeout=180.0,
    )
    if code < 200 or code >= 300:
        return f"(hermes error {code}: {data!s})"[:200]
    try:
        content = data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        return f"(hermes bad response: {data!s})"[:200]
    if not isinstance(content, str):
        content = str(content)
    # ASCII-safe for panel; keep length for scrolling chat (body collect max 4KB)
    cleaned = "".join(
        (ch if 32 <= ord(ch) <= 126 else " ") for ch in content
    )
    cleaned = " ".join(cleaned.split())
    if len(cleaned) > 3500:
        cleaned = cleaned[:3497] + "..."
    return cleaned or "(empty reply)"


def pager_push(agent: str, text: str) -> None:
    if not PAGER_TOKEN:
        print("PAGER_TOKEN missing; skip push", file=sys.stderr)
        return
    auth = {"Authorization": f"Bearer {PAGER_TOKEN}"}
    # 1) Full reply → chat room (may be long; pager splits + scrolls).
    _http_json(
        "POST",
        f"{PAGER_URL}/agents/inbound",
        body={"agent": agent, "text": text[:3500]},
        headers=auth,
        timeout=12.0,
    )
    # 2) Notify is only a HEADER / door into the chat room.
    name = agent.upper()[:16]
    teaser = text[:48] + ("..." if len(text) > 48 else "")
    _http_json(
        "POST",
        f"{PAGER_URL}/notify",
        body={
            "level": "info",
            "source": agent[:16],
            "title": name,
            "body": teaser or "new reply - open chat",
            "id": f"{agent}-chat"[:24],
        },
        headers=auth,
        timeout=8.0,
    )


def handle_chat(payload: dict) -> tuple[int, dict]:
    agent = (payload.get("agent") or "").strip().lower()
    text = (payload.get("text") or "").strip()
    session = (payload.get("session") or "pager").strip() or "pager"
    if agent not in ("hermes", "grok", "claude"):
        return 400, {"error": "agent must be hermes, grok or claude"}
    if not text:
        return 400, {"error": "text required"}
    if agent != "hermes":
        # Phase 2: only Hermes wired. Others get an honest stub.
        def _stub() -> None:
            pager_push(agent, f"(bridge: {agent} not wired yet)")

        threading.Thread(target=_stub, daemon=True).start()
        return 202, {"ok": True, "queued": True, "agent": agent, "stub": True}

    def _work() -> None:
        try:
            reply = hermes_chat(session, text)
            print(f"[bridge] hermes << {text!r} >> {reply!r}", flush=True)
            pager_push("hermes", reply)
        except Exception:
            traceback.print_exc()
            try:
                pager_push("hermes", "(bridge exception — see home-rig log)")
            except Exception:
                pass

    threading.Thread(target=_work, daemon=True).start()
    return 202, {"ok": True, "queued": True, "agent": "hermes"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, obj: dict) -> None:
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        if self.path in ("/", "/health"):
            self._send(
                200,
                {
                    "ok": True,
                    "service": "pager-agent-bridge",
                    "hermes_url": HERMES_URL,
                    "pager_url": PAGER_URL,
                    "hermes_key_set": bool(HERMES_KEY),
                    "pager_token_set": bool(PAGER_TOKEN),
                },
            )
            return
        self._send(404, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path.rstrip("/") != "/v1/chat":
            self._send(404, {"error": "not found"})
            return
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            self._send(400, {"error": "invalid JSON"})
            return
        if not isinstance(payload, dict):
            self._send(400, {"error": "JSON object required"})
            return
        code, obj = handle_chat(payload)
        self._send(code, obj)


def main() -> int:
    host, _, port_s = BRIDGE_ADDR.partition(":")
    port = int(port_s or "8091")
    print(
        f"pager-agent-bridge on {host}:{port} → hermes={HERMES_URL} pager={PAGER_URL}",
        flush=True,
    )
    httpd = ThreadingHTTPServer((host, port), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("bye", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
