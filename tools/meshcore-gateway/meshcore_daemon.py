#!/usr/bin/env python3
"""MeshCore Home Rig Daemon — Heltec V3 Companion USB gateway.

Roles:
  1) Mesh command bot (ping/status/…) → DM reply
  2) Pager transport: P1|level|source|title|body → POST pager /notify
  3) HTTP webhook POST /send → mesh DM to target contact
  4) HTTP POST /pager → force-send a P1 notify over mesh channel (optional)

Payload (our layer on top of MeshCore text):
  P1|<info|warn|crit>|<source>|<title>|<body>
  M1|mid|i|n|level|source|title|chunk   — long notify
  C1|agent|mid|i|n|side|chunk           — chat (side u=uplink user, a=agent)
  R1|key|reply                          — typed pager reply (mesh fallback)
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import logging
import os
import re
import shutil
import signal
import subprocess
import time
import uuid
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

import yaml
from aiohttp import web
from meshcore import EventType, MeshCore

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("meshcore-daemon")

cfg: dict = {}
mc: MeshCore | None = None
target_contact = None
start_time = time.monotonic()
inet_was_up = True

# Uplink C1 reassembly (pager → home). Keyed by mid.
_c1_uplink: dict[str, dict[str, Any]] = {}
# Recently completed C1 IDs suppress a whole-message duplicate when the pager
# retries a frame whose radio ACK was lost after the gateway received it.
_c1_completed: dict[str, float] = {}
# Dedicated agent channels. Each C1 uplink is queued only for the matching
# bridge and each downlink must present the exact source/id pair. This keeps
# OpenCode and Codex from consuming or spoofing one another's conversations.
AGENT_CHANNELS: dict[str, dict[str, str]] = {
    "opencode": {
        "source": "opencode-pager",
        "id": "opencode-chat",
        "title": "OPENCODE",
    },
    "codex": {
        "source": "codex-pager",
        "id": "codex-chat",
        "title": "CODEX",
    },
}
_agent_inboxes: dict[str, list[dict[str, Any]]] = {
    agent: [] for agent in AGENT_CHANNELS
}
_notify_deliveries: dict[str, dict[str, Any]] = {}
_notify_reservations: dict[str, dict[str, Any]] = {}
_agent_dead_letters: dict[str, list[dict[str, Any]]] = {
    agent: [] for agent in AGENT_CHANNELS
}
_AGENT_INBOX_MAX = 200
_MESH_BODY_MAX_BYTES = 1600
# Typed replies from the pager device (WiFi POST /reply or mesh R1 fallback).
_replies: list[dict[str, Any]] = []
_REPLIES_MAX = 200
_REPLY_KEY_MAX = 64


def _agent_channel_for_payload(payload: dict) -> tuple[str, dict[str, str]] | None:
    """Return the exact channel selected by a trusted source/id tuple."""
    source = str(payload.get("source") or "")
    key = str(payload.get("id") or "")
    for agent, channel in AGENT_CHANNELS.items():
        if source == channel["source"] and key == channel["id"]:
            return agent, channel
    return None


def _notify_delivery_key(payload: dict) -> str:
    delivery_id = str(payload.get("deliveryId") or "")
    if not delivery_id:
        return ""
    source = str(payload.get("source") or "")
    item_id = str(payload.get("id") or "")
    return f"{source}\x1f{item_id}\x1f{delivery_id}"


def _agent_inbox_path() -> Path:
    configured = (cfg.get("agent_inbox") or {}).get("path")
    return (
        Path(configured) if configured else Path(__file__).parent / "agent-inboxes.json"
    )


def _persist_agent_inboxes() -> None:
    path = _agent_inbox_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as handle:
        payload = dict(_agent_inboxes)
        payload["_notifyDeliveries"] = _notify_deliveries
        payload["_notifyReservations"] = _notify_reservations
        payload["_agentDeadLetters"] = _agent_dead_letters
        json.dump(payload, handle, ensure_ascii=False, separators=(",", ":"))
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def load_agent_inboxes() -> None:
    global _notify_deliveries, _notify_reservations
    try:
        saved = json.loads(_agent_inbox_path().read_text(encoding="utf-8"))
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError) as exc:
        log.error("cannot load agent inbox state: %s", exc)
        return
    for agent in AGENT_CHANNELS:
        rows = saved.get(agent) if isinstance(saved, dict) else None
        if isinstance(rows, list):
            _agent_inboxes[agent] = [
                row
                for row in rows[-_AGENT_INBOX_MAX:]
                if isinstance(row, dict) and row.get("id") and row.get("text")
            ]
    deliveries = saved.get("_notifyDeliveries") if isinstance(saved, dict) else None
    if isinstance(deliveries, dict):
        _notify_deliveries = {
            str(key): value
            for key, value in list(deliveries.items())[-500:]
            if key and isinstance(value, dict) and value.get("ok")
        }
    reservations = saved.get("_notifyReservations") if isinstance(saved, dict) else None
    if isinstance(reservations, dict):
        _notify_reservations = {
            str(key): {**value, "state": "ambiguous"}
            for key, value in list(reservations.items())[-500:]
            if key and isinstance(value, dict)
        }
    dead_letters = saved.get("_agentDeadLetters") if isinstance(saved, dict) else None
    if isinstance(dead_letters, dict):
        for agent in AGENT_CHANNELS:
            rows = dead_letters.get(agent)
            if isinstance(rows, list):
                _agent_dead_letters[agent] = [
                    row for row in rows[-_AGENT_INBOX_MAX:] if isinstance(row, dict)
                ]


def enqueue_agent_message(agent: str, text: str, ts: int | None = None) -> bool:
    queue = _agent_inboxes.get(agent)
    if queue is None or len(queue) >= _AGENT_INBOX_MAX:
        return False
    queue.append(
        {
            "id": uuid.uuid4().hex,
            "text": text,
            "ts": int(time.time()) if ts is None else ts,
        }
    )
    _persist_agent_inboxes()
    return True


def list_agent_messages(agent: str) -> list[dict[str, Any]]:
    queue = _agent_inboxes.get(agent)
    if queue is None:
        return []
    return list(queue)


def ack_agent_messages(agent: str, message_ids: set[str]) -> int:
    queue = _agent_inboxes.get(agent)
    if queue is None or not message_ids:
        return 0
    before = len(queue)
    queue[:] = [row for row in queue if str(row.get("id")) not in message_ids]
    removed = before - len(queue)
    if removed:
        _persist_agent_inboxes()
    return removed


def reject_agent_messages(agent: str, message_ids: set[str], reason: str) -> int:
    queue = _agent_inboxes.get(agent)
    dead = _agent_dead_letters.get(agent)
    if queue is None or dead is None or not message_ids:
        return 0
    rejected = [row for row in queue if str(row.get("id")) in message_ids]
    if not rejected:
        return 0
    queue[:] = [row for row in queue if str(row.get("id")) not in message_ids]
    now = int(time.time())
    dead.extend({**row, "rejectedAt": now, "reason": reason[:160]} for row in rejected)
    del dead[:-_AGENT_INBOX_MAX]
    _persist_agent_inboxes()
    return len(rejected)


def _replies_path() -> Path:
    configured = (cfg.get("replies") or {}).get("path")
    return Path(configured) if configured else Path(__file__).parent / "replies.json"


def _persist_replies() -> None:
    path = _replies_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(_replies, handle, ensure_ascii=False, separators=(",", ":"))
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def load_replies() -> None:
    try:
        saved = json.loads(_replies_path().read_text(encoding="utf-8"))
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError) as exc:
        log.error("cannot load replies state: %s", exc)
        return
    if isinstance(saved, list):
        _replies[:] = [
            row
            for row in saved[-_REPLIES_MAX:]
            if isinstance(row, dict) and row.get("key") and row.get("reply")
        ]
        # Backfill last_ts for rows persisted before the field existed, so
        # consumers get a uniform entry shape without fallback logic.
        for row in _replies:
            row["last_ts"] = row.get("last_ts") or row.get("ts")


def _clean_reply_fields(key: Any, reply: Any) -> tuple[str, str] | None:
    """Validate and normalize a (key, reply) pair from either transport."""
    if not isinstance(key, str) or not isinstance(reply, str):
        return None
    key = key.strip()
    reply = reply.strip()
    if not key or len(key) > _REPLY_KEY_MAX or not reply:
        return None
    return key, _utf8_ellipsize(reply, _MESH_BODY_MAX_BYTES)


def store_reply(key: str, reply: str, transport: str) -> tuple[bool, dict[str, Any]]:
    """Record one logical typed reply per (key, reply) pair.

    The firmware may deliver the same reply twice — WiFi POST /reply plus a
    mesh R1 fallback when the HTTP path looked failed. The second arrival must
    not create a duplicate entry: it only bumps the duplicate counter.

    Reply keys are stable by design (a recurring card reuses its key), so a
    repeated arrival keeps `ts` as the first-arrival time for transport-race
    forensics while `last_ts` always reflects the most recent arrival.
    Consumers filtering "replies since ..." must use `last_ts`.
    """
    now = int(time.time())
    for row in _replies:
        if row.get("key") == key and row.get("reply") == reply:
            row["duplicates"] = int(row.get("duplicates") or 0) + 1
            row["lastTransport"] = transport
            row["last_ts"] = now
            _persist_replies()
            return False, row
    entry: dict[str, Any] = {
        "id": uuid.uuid4().hex,
        "key": key,
        "reply": reply,
        "transport": transport,
        "ts": now,
        "last_ts": now,
        "duplicates": 0,
    }
    _replies.append(entry)
    del _replies[:-_REPLIES_MAX]
    _persist_replies()
    return True, entry


def gateway_api_tokens() -> dict[str, str]:
    webhook = cfg.get("webhook") or {}
    return {
        "admin": str(
            os.environ.get("MESHCORE_GATEWAY_TOKEN") or webhook.get("token") or ""
        ),
        "opencode": str(
            os.environ.get("MESHCORE_OPENCODE_TOKEN")
            or webhook.get("opencode_token")
            or ""
        ),
        "codex": str(
            os.environ.get("MESHCORE_CODEX_TOKEN") or webhook.get("codex_token") or ""
        ),
        "hermes_notify": str(
            os.environ.get("MESHCORE_HERMES_NOTIFY_TOKEN")
            or webhook.get("hermes_notify_token")
            or ""
        ),
        "device": str(
            os.environ.get("MESHCORE_DEVICE_TOKEN") or webhook.get("device_token") or ""
        ),
    }


def _token_matches(provided: str, expected: str) -> bool:
    return bool(provided and expected and hmac.compare_digest(provided, expected))


async def _request_agent_scope(request: web.Request) -> str | None:
    # The pager device holds its own low-privilege token: it may probe the
    # gateway and submit typed replies, nothing else. Every other route falls
    # through to another scope or to admin, so the device token never
    # authorizes /notify-out, /send, /pager, or any agent inbox.
    if request.path == "/ping" and request.method == "GET":
        return "device"
    if request.path == "/reply" and request.method == "POST":
        return "device"
    if request.path.startswith("/opencode/"):
        return "opencode"
    if request.path.startswith("/codex/"):
        return "codex"
    if request.path == "/notify-out":
        try:
            data = await request.json()
        except Exception:
            return None
        if (
            request.method == "POST"
            and isinstance(data, dict)
            and str(data.get("source") or "") == "hermes"
            and str(data.get("id") or "") == "hermes-note"
        ):
            return "hermes_notify"
        match = _agent_channel_for_payload(data if isinstance(data, dict) else {})
        return match[0] if match else None
    return None


@web.middleware
async def gateway_auth_middleware(request: web.Request, handler):
    if request.path == "/health":
        return await handler(request)
    tokens = gateway_api_tokens()
    scope = await _request_agent_scope(request)
    expected = tokens.get(scope or "admin", "")
    if not expected or not tokens["admin"]:
        return web.json_response(
            {"ok": False, "error": "gateway API tokens are not configured"},
            status=503,
        )
    provided = request.headers.get("Authorization", "")
    if provided.startswith("Bearer "):
        provided = provided[7:].strip()
    else:
        provided = request.headers.get("X-Gateway-Token", "").strip()
    # The admin capability may operate every route; agent bridges receive only
    # their scoped token and cannot read, ack, or spoof the other agent.
    if not (
        _token_matches(provided, expected) or _token_matches(provided, tokens["admin"])
    ):
        return web.json_response({"ok": False, "error": "unauthorized"}, status=401)
    return await handler(request)


def load_config() -> dict:
    config_path = Path(__file__).parent / "config.yaml"
    with open(config_path) as f:
        return yaml.safe_load(f) or {}


def truncate(text: str, limit: int = 200) -> str:
    text = text.strip()
    if len(text) <= limit:
        return text
    return text[: limit - 3] + "..."


def run_shell(cmd: str, timeout: int = 10) -> str:
    try:
        r = subprocess.run(
            cmd, shell=True, capture_output=True, text=True, timeout=timeout
        )
        out = (r.stdout + r.stderr).strip()
        return out if out else "(no output)"
    except subprocess.TimeoutExpired:
        return "(timeout)"
    except Exception as e:
        return f"(error: {e})"


def check_inet(host: str = "8.8.8.8") -> bool:
    try:
        subprocess.run(
            ["ping", "-c", "1", "-W", "3", host],
            capture_output=True,
            timeout=5,
        )
        return True
    except Exception:
        return False


# ── P1 payload ───────────────────────────────────────────────────────────────


def parse_p1(text: str) -> dict | None:
    """Parse P1|level|source|title|body — body may contain |."""
    if not text.startswith("P1|"):
        return None
    parts = text.split("|", 4)
    if len(parts) < 5:
        return None
    _, level, source, title, body = parts
    level = (level or "info").strip().lower()
    if level not in ("info", "warn", "crit"):
        level = "info"
    return {
        "level": level,
        "source": (source or "mesh")[:16],
        "title": (title or "mesh")[:40],
        "body": (body or "")[:256],
    }


def _utf8_fit(text: str, max_bytes: int) -> str:
    raw = (text or "").encode("utf-8")
    if len(raw) <= max_bytes:
        return text or ""
    return raw[:max_bytes].decode("utf-8", errors="ignore")


def _utf8_ellipsize(text: str, max_bytes: int) -> str:
    raw = (text or "").encode("utf-8")
    if len(raw) <= max_bytes:
        return text or ""
    suffix = "…"
    return _utf8_fit(text or "", max_bytes - len(suffix.encode("utf-8"))) + suffix


def _new_mesh_mid(delivery_id: str = "") -> str:
    """Seven hex chars fit the firmware's char mid[8] without collisions by second."""
    if delivery_id:
        return hashlib.sha256(delivery_id.encode("utf-8")).hexdigest()[:7]
    return uuid.uuid4().hex[:7]


def _utf8_chunks(raw: bytes, room: int) -> list[str]:
    """Split UTF-8 bytes without dropping a complete final multibyte char."""
    chunks: list[str] = []
    i = 0
    while i < len(raw):
        end = min(i + room, len(raw))
        while end > i:
            try:
                piece = raw[i:end].decode("utf-8")
                break
            except UnicodeDecodeError:
                end -= 1
        if end == i:
            # room is always >=16 for mesh frames, so this is defensive only.
            end = min(i + 1, len(raw))
            piece = raw[i:end].decode("utf-8", errors="ignore")
        chunks.append(piece)
        i = end
    return chunks


def encode_p1(payload: dict) -> str:
    """Single-frame notify. Prefer encode_mesh_frames() for long bodies."""
    frames = encode_mesh_frames(payload)
    return frames[0] if frames else "P1|info|home|notify|"


def encode_mesh_frames(payload: dict) -> list[str]:
    """Build private-DM frame(s). MeshCore text MTU ~160; keep limit ≤150.

    Short body → one P1|level|source|title|body
    Long body  → M1|mid|i|n|level|source|title|chunk  (1-based i, n parts)
    Chat door (source/id ends with -chat or source in grok|claude|hermes):
                 C1|agent|mid|i|n|a|chunk  (side a=agent, u=user)
    """
    limit = int(cfg.get("mesh_msg_limit", 150))
    level = (payload.get("level") or "info").strip().lower()
    if level not in ("info", "warn", "crit"):
        level = "info"
    source = (payload.get("source") or "home")[:16]
    title = (payload.get("title") or "notify")[:60]
    body = payload.get("body") or ""
    key = payload.get("id") or ""
    force_p1 = bool(payload.get("_force_p1"))
    if not force_p1 and isinstance(key, str) and key.endswith("-chat"):
        channel_match = _agent_channel_for_payload(payload)
        if not channel_match:
            return []
        agent, _channel = channel_match
        return _encode_c1(
            agent,
            body,
            side="a",
            limit=limit,
            delivery_id=str(payload.get("deliveryId") or ""),
        )
    if not force_p1 and (
        source in ("grok", "claude", "hermes") or source.endswith("-chat")
    ):
        agent = source.replace("-chat", "")[:12]
        return _encode_c1(
            agent,
            body,
            side="a",
            limit=limit,
            delivery_id=str(payload.get("deliveryId") or ""),
        )

    prefix = f"P1|{level}|{source}|{title}|"
    # A P1 may carry the client id as its final field.  The pager uses ids that
    # end in "-chat" as doors: Enter opens that agent room instead of the
    # ordinary Ack/Reply sheet.
    suffix = f"|{key}" if force_p1 and isinstance(key, str) and key else ""
    room = max(0, limit - len(prefix.encode("utf-8")) - len(suffix.encode("utf-8")))
    raw = body.encode("utf-8")
    if len(raw) <= room:
        return [prefix + body + suffix]

    # Multi-part notify reassembly on pager
    mid = _new_mesh_mid(str(payload.get("deliveryId") or ""))
    # header worst-case: M1|ffff|99|99|crit|src16|title60|
    # provisional part count — iterate until stable
    n_guess = max(1, (len(raw) + 80) // 80)
    while True:
        hdr_sample = f"M1|{mid}|{n_guess}|{n_guess}|{level}|{source}|{title}|"
        room = max(24, limit - len(hdr_sample.encode("utf-8")))
        chunks = _utf8_chunks(raw, room)
        if len(chunks) == n_guess or n_guess > 64:
            n_guess = len(chunks) or 1
            break
        n_guess = len(chunks) or 1
    frames = []
    n = max(1, len(chunks))
    if n > 64:
        return []
    for i, ch in enumerate(chunks, 1):
        frames.append(f"M1|{mid}|{i}|{n}|{level}|{source}|{title}|{ch}")
    return frames or [prefix + _utf8_fit(body, room)]


def _encode_c1(
    agent: str,
    body: str,
    side: str = "a",
    limit: int = 150,
    delivery_id: str = "",
) -> list[str]:
    """Chat multi-part: C1|agent|mid|i|n|side|chunk"""
    agent = (agent or "chat")[:12]
    side = "u" if side == "u" else "a"
    mid = _new_mesh_mid(delivery_id)
    body = _utf8_ellipsize(body or "", _MESH_BODY_MAX_BYTES)
    raw = body.encode("utf-8")
    if not raw:
        return [f"C1|{agent}|{mid}|1|1|{side}|"]
    n_guess = max(1, (len(raw) + 70) // 70)
    while True:
        hdr = f"C1|{agent}|{mid}|{n_guess}|{n_guess}|{side}|"
        room = max(16, limit - len(hdr.encode("utf-8")))
        chunks = _utf8_chunks(raw, room)
        if len(chunks) == n_guess or n_guess > 64:
            break
        n_guess = len(chunks) or 1
    n = max(1, len(chunks))
    if n > 64:
        return []
    return [f"C1|{agent}|{mid}|{i}|{n}|{side}|{ch}" for i, ch in enumerate(chunks, 1)]


def normalize_payload(data: dict) -> dict:
    level = (data.get("level") or "info").strip().lower()
    if level not in ("info", "warn", "crit"):
        level = "info"
    return {
        "level": level,
        "source": (data.get("source") or "home")[:16],
        "title": (data.get("title") or "notify")[:60],
        "body": _utf8_ellipsize(str(data.get("body") or ""), _MESH_BODY_MAX_BYTES),
        "id": data.get("id"),  # optional client key (*-chat door)
        "deliveryId": str(data.get("deliveryId") or "")[:200],
    }


def pager_post_notify(payload: dict) -> tuple[int, str]:
    """Direct LAN POST to T-Lora seed /notify."""
    pager = cfg.get("pager") or {}
    url = (pager.get("url") or "").rstrip("/") + "/notify"
    token = pager.get("token") or os.environ.get("PAGER_TOKEN", "")
    if not url.startswith("http"):
        return 0, "pager url not set"
    if not token:
        return 0, "pager token not set"

    body = {
        "level": payload["level"],
        "source": payload["source"],
        "title": payload["title"],
        "body": payload["body"],
    }
    key = payload.get("id") or ""
    if isinstance(key, str) and key.endswith("-chat"):
        body["id"] = key[:24]
    elif (payload.get("source") or "").endswith("-chat"):
        body["id"] = (payload["source"])[:24]

    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {token}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=4) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")[:300]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")[:300]
    except Exception as e:
        return 0, str(e)


def mesh_pager_dst() -> str | None:
    """Full public key (64 hex) or prefix of the paired pager node. Never Public."""
    m = cfg.get("mesh") or {}
    pk = (m.get("pager_pubkey") or m.get("pager_pk") or "").strip().lower()
    if not pk or pk in ("", "change_me", "none"):
        return None
    # allow 0x prefix
    if pk.startswith("0x"):
        pk = pk[2:]
    return pk


def is_paired_pager_sender(sender: str) -> bool:
    """Accept C1 commands only from the configured pager identity."""
    expected = mesh_pager_dst()
    actual = str(sender or "").strip().lower()
    if not expected or actual == "unknown" or len(expected) < 12 or len(actual) < 12:
        return False
    return expected[:12] == actual[:12]


async def mesh_send_p1(payload: dict) -> tuple[bool, str]:
    """TX private DM frame(s) to the paired pager only. NEVER Public channel.

    Long bodies / chat become multi-part M1/C1 frames; pager reassemblies.
    Small gap between parts so the radio stack can ACK without flooding.
    """
    if not mc or not mc.is_connected:
        return False, "mesh not connected"
    dst = mesh_pager_dst()
    if not dst:
        return False, "pager not paired (set mesh.pager_pubkey)"
    frames = encode_mesh_frames(payload)
    if not frames:
        return False, "empty frames"
    try:
        sent = 0
        for i, wire in enumerate(frames):
            # send_msg() only means that the radio accepted the packet. It does
            # not mean the pager received it. Wait for the MeshCore ACK, retry
            # twice, and let the SDK reset a stale direct path to flood on the
            # final attempt.
            result = await mc.commands.send_msg_with_retry(
                dst,
                wire,
                max_attempts=3,
                max_flood_attempts=2,
                flood_after=2,
                timeout=2.5,
                min_timeout=1.5,
            )
            if result is None:
                return (
                    False,
                    f"part {i + 1}/{len(frames)}: no delivery ACK after retries",
                )
            if result.type == EventType.ERROR:
                return False, f"part {i + 1}/{len(frames)}: {result.payload}"
            sent += 1
            if i + 1 < len(frames):
                await asyncio.sleep(0.35)  # ~one SF8 airtime between parts
        head = frames[0][:50]
        return True, f"dm:{dst[:12]}… acked_parts={sent} {head}"
    except Exception as e:
        return False, str(e)


async def notify_out(payload: dict, mode: str | None = None) -> dict:
    """
    Seamless home→pager egress. Same notify payload; two transports.

    User-facing cards on the pager are identical (POST /notify shape).
    WiFi when the pager is on LAN; private MeshCore DM when paired / offline.

    mode:
      both        — try WiFi AND private mesh DM (recommended)
      prefer_wifi — mesh DM only if WiFi fails
      wifi_only
      mesh_only
    """
    payload = normalize_payload(payload)
    mode = (mode or (cfg.get("notify") or {}).get("mode") or "both").lower()

    out: dict = {
        "payload": payload,
        "mode": mode,
        "wifi": None,
        "mesh": None,
        "mesh_policy": "private_dm_only",
    }

    channel_match = _agent_channel_for_payload(payload)
    key = str(payload.get("id") or "")
    if key.endswith("-chat") and not channel_match:
        out["ok"] = False
        out["error"] = "invalid agent source/id pair"
        return out

    do_wifi = mode in ("both", "prefer_wifi", "wifi_only")
    do_mesh = mode in ("both", "mesh_only")
    # Agent history is C1-backed. Even with pager Wi-Fi available, a direct
    # /notify supplies only the door card, never the full room history.
    if channel_match:
        do_mesh = True
    wifi_ok = False

    if do_wifi:
        wifi_payload = payload
        if channel_match:
            _agent, channel = channel_match
            wifi_payload = dict(payload)
            wifi_payload["title"] = channel["title"]
            wifi_payload["body"] = _utf8_fit(str(payload.get("body") or ""), 80)
        code, resp = await asyncio.to_thread(pager_post_notify, wifi_payload)
        wifi_ok = 200 <= code < 300
        out["wifi"] = {"http": code, "ok": wifi_ok, "resp": resp[:200]}
        log.info("notify-out wifi → %s %s", code, resp[:80])

    if mode == "prefer_wifi" and not channel_match:
        do_mesh = not wifi_ok

    if do_mesh:
        ok, detail = await mesh_send_p1(payload)
        mesh_result = {"ok": ok, "detail": detail[:200]}

        # C1 is the persistent agent-thread transport. Mirror trusted agent
        # replies as a short P1 chat door so Enter opens the matching room.
        if channel_match:
            agent, channel = channel_match
            mesh_result["thread_ok"] = ok
            # A successful direct /notify already created the door. Otherwise
            # send a short P1 door over mesh after the complete C1 history.
            if wifi_ok:
                visible_ok, visible_detail = True, "wifi door delivered"
            else:
                visible = dict(payload)
                visible["_force_p1"] = True
                visible["id"] = channel["id"]
                visible["title"] = channel["title"]
                visible["body"] = _utf8_fit(str(payload.get("body") or ""), 80)
                visible_ok, visible_detail = await mesh_send_p1(visible)
            mesh_result["visible_ok"] = visible_ok
            mesh_result["visible_detail"] = visible_detail[:200]
            mesh_result["ok"] = bool(ok and visible_ok)
            log.info(
                "notify-out %s visible → %s %s", agent, visible_ok, visible_detail[:80]
            )

        out["mesh"] = mesh_result
        log.info("notify-out mesh-dm → %s %s", mesh_result["ok"], detail[:80])

    if channel_match:
        out["ok"] = bool((out.get("mesh") or {}).get("ok"))
    else:
        out["ok"] = bool(
            (out.get("wifi") or {}).get("ok") or (out.get("mesh") or {}).get("ok")
        )
    out["paired"] = bool(mesh_pager_dst())
    return out


# ── Commands ─────────────────────────────────────────────────────────────────


def cmd_help() -> str:
    return (
        "Commands:\n"
        "ping — pong\n"
        "status — system info\n"
        "inet — check internet\n"
        "mesh — radio self info\n"
        "reboot — sudo reboot\n"
        "wol <mac> — wake-on-lan\n"
        "sh <cmd> — run shell\n"
        "help — this message\n"
        "\n"
        "Pager: send P1|level|source|title|body"
    )


def cmd_ping() -> str:
    return "pong"


def cmd_status() -> str:
    uptime_s = int(time.monotonic() - start_time)
    h, m = divmod(uptime_s // 60, 60)
    daemon_up = f"{h}h{m}m"
    load = os.getloadavg()
    load_str = f"{load[0]:.1f} {load[1]:.1f} {load[2]:.1f}"
    try:
        import psutil

        mem = psutil.virtual_memory()
        ram = f"{mem.used // (1024**2)}M/{mem.total // (1024**2)}M ({mem.percent}%)"
        disk = psutil.disk_usage("/")
        disk_str = (
            f"{disk.used // (1024**3)}G/{disk.total // (1024**3)}G ({disk.percent}%)"
        )
    except ImportError:
        ram = run_shell("free -m | awk '/Mem:/{printf \"%sM/%sM\", $3, $2}'")
        disk_str = run_shell("df -h / | awk 'NR==2{printf \"%s/%s\", $3, $2}'")
    inet = (
        "up"
        if check_inet(cfg.get("monitor", {}).get("ping_host", "8.8.8.8"))
        else "DOWN"
    )
    return truncate(
        f"Daemon: {daemon_up}\nLoad: {load_str}\nRAM: {ram}\nDisk: {disk_str}\nInet: {inet}"
    )


def cmd_inet() -> str:
    host = cfg.get("monitor", {}).get("ping_host", "8.8.8.8")
    ping_ok = check_inet(host)
    result = f"Ping {host}: {'OK' if ping_ok else 'FAIL'}"
    if ping_ok and shutil.which("speedtest-cli"):
        st = run_shell("speedtest-cli --simple", timeout=30)
        result += f"\n{st}"
    return truncate(result)


def cmd_mesh() -> str:
    if not mc or not mc.self_info:
        return "mesh not connected"
    s = mc.self_info
    name = s.get("name") or "?"
    pk = (s.get("public_key") or "")[:12]
    freq = s.get("radio_freq")
    return truncate(f"node {name}\npk {pk}…\nfreq {freq} SF{s.get('radio_sf')}")


def cmd_reboot() -> str:
    asyncio.get_event_loop().call_later(2, lambda: os.system("sudo reboot"))
    return "Rebooting in 2s..."


def cmd_wol(mac: str) -> str:
    if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}", mac or ""):
        return "WOL error: invalid MAC"
    if shutil.which("wakeonlan"):
        result = subprocess.run(
            ["wakeonlan", mac],
            capture_output=True,
            text=True,
            timeout=cfg.get("shell_timeout", 10),
            check=False,
        )
        return truncate((result.stdout or result.stderr).strip())
    if shutil.which("etherwake"):
        result = subprocess.run(
            ["sudo", "etherwake", mac],
            capture_output=True,
            text=True,
            timeout=cfg.get("shell_timeout", 10),
            check=False,
        )
        return truncate((result.stdout or result.stderr).strip())
    try:
        import socket

        mac_bytes = bytes.fromhex(mac.replace(":", "").replace("-", ""))
        packet = b"\xff" * 6 + mac_bytes * 16
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.sendto(packet, ("255.255.255.255", 9))
        return f"WOL sent to {mac}"
    except Exception as e:
        return f"WOL error: {e}"


def cmd_sh(cmd: str) -> str:
    timeout = cfg.get("shell_timeout", 10)
    return truncate(run_shell(cmd, timeout=timeout))


COMMANDS = {
    "help": lambda _: cmd_help(),
    "ping": lambda _: cmd_ping(),
    "status": lambda _: cmd_status(),
    "inet": lambda _: cmd_inet(),
    "mesh": lambda _: cmd_mesh(),
    "reboot": lambda _: cmd_reboot(),
    "wol": lambda args: cmd_wol(args) if args else "Usage: wol <mac>",
    "sh": lambda args: cmd_sh(args) if args else "Usage: sh <command>",
}


def handle_command(text: str) -> str:
    text = text.strip()
    parts = text.split(None, 1)
    cmd = parts[0].lower() if parts else ""
    args = parts[1] if len(parts) > 1 else ""
    if cmd in ("sh", "reboot") and not bool(cfg.get("allow_shell_commands", False)):
        return f"{cmd}: disabled"
    handler = COMMANDS.get(cmd)
    if handler:
        try:
            return handler(args)
        except Exception as e:
            return f"Error: {e}"
    return f"Unknown: {cmd}\nhelp — list"


# ── Mesh handlers ────────────────────────────────────────────────────────────


async def on_mesh_message(event):
    global target_contact
    data = event.payload if isinstance(event.payload, dict) else {}
    text = (data.get("text") or "").strip()
    sender = data.get("pubkey_prefix") or data.get("public_key") or "unknown"
    if isinstance(sender, str) and len(sender) > 12:
        sender = sender[:12]
    log.info("Mesh msg from %s: %r", sender, text[:120])

    # Every private command path, not only C1, belongs to the paired pager.
    # Channel traffic and unpaired DMs are never allowed to reach the bot.
    if not is_paired_pager_sender(sender):
        log.warning("ignored mesh message from unpaired sender %s", sender)
        return

    # Sparse pager keepalive: "MC|k" (or bare "K").
    # No shell bot, but send tiny app-level pong "MC|a" so the pager can refresh
    # its alive counter even if the MeshCore protocol ACK is missed (shared SPI).
    if text in ("K", "MC|k") or text.startswith("MC|k"):
        log.info("mesh keepalive from %s — pong MC|a", sender)
        if (
            cfg.get("target_node", "CHANGE_ME") == "CHANGE_ME"
            and target_contact is None
        ):
            await resolve_contact(str(sender))
        try:
            # Prefer full pager pubkey if configured
            dst = mesh_pager_dst() or sender
            await mc.commands.send_msg(dst, "MC|a")
        except Exception as e:
            log.warning("keepalive pong failed: %s", e)
        return

    # Chat / multi-part notify frames — never shell-bot
    if text.startswith("C1|"):
        await handle_c1_uplink(sender, text)
        return
    # Typed reply fallback frame — never shell-bot, malformed frames drop here.
    if text.startswith("R1|"):
        handle_r1_reply(sender, text)
        return
    if text.startswith("M1|") or text.startswith("P1|"):
        # Inbound P1/M1 from pager is unusual; ignore bot, optional LAN mirror
        if text.startswith("P1|"):
            p1 = parse_p1(text)
            if p1:
                code, resp = await asyncio.to_thread(pager_post_notify, p1)
                log.info("pager /notify (mesh P1) → %s %s", code, resp[:80])
        return

    # Auto-learn target
    if cfg.get("target_node", "CHANGE_ME") == "CHANGE_ME" and target_contact is None:
        await resolve_contact(str(sender))

    response = handle_command(text)
    log.info("Response: %s", response[:120])
    await reply_to(sender, response)


def _agent_bridge_url() -> str:
    ab = cfg.get("agent_bridge") or {}
    url = ab.get("url") or os.environ.get("AGENT_BRIDGE_URL") or "http://127.0.0.1:8091"
    return str(url).rstrip("/")


def _http_json_post(url: str, body: dict, timeout: float = 30.0) -> tuple[int, str]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")[:400]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")[:400]
    except Exception as e:
        return 0, str(e)


async def handle_c1_uplink(sender: str, text: str) -> None:
    """Reassemble C1|agent|mid|i|n|side|chunk from pager; side=u → agent bridge."""
    # C1|agent|mid|i|n|side|chunk
    parts = text.split("|", 6)
    if len(parts) < 7:
        log.warning("bad C1 from %s: %r", sender, text[:80])
        return
    _, agent, mid, si, sn, side, chunk = parts
    try:
        i, n = int(si), int(sn)
    except ValueError:
        return
    if n < 1 or n > 64 or i < 1 or i > n:
        return
    agent = (agent or "hermes")[:12].lower()
    side = (side or "u")[:1]
    key = f"{agent}:{mid}"
    completed_key = f"{sender}:{agent}:{mid}:{side}"
    now = time.time()
    # GC stale
    dead = [k for k, v in _c1_uplink.items() if now - v.get("t0", 0) > 60]
    for k in dead:
        _c1_uplink.pop(k, None)
    completed_dead = [k for k, ts in _c1_completed.items() if now - ts > 300]
    for k in completed_dead:
        _c1_completed.pop(k, None)
    if completed_key in _c1_completed:
        log.info("C1 duplicate suppressed %s", completed_key)
        return

    slot = _c1_uplink.get(key)
    if not slot:
        slot = {"t0": now, "n": n, "side": side, "agent": agent, "chunks": {}}
        _c1_uplink[key] = slot
    slot["chunks"][i] = chunk or ""
    slot["t0"] = now
    if len(slot["chunks"]) < n:
        log.info("C1 uplink %s part %s/%s", key, i, n)
        return

    full = "".join(slot["chunks"].get(j, "") for j in range(1, n + 1))
    _c1_uplink.pop(key, None)
    log.info("C1 uplink complete %s side=%s len=%d", key, side, len(full))

    if side != "u":
        # Agent→pager frames should not hit the gateway as RX (they are TX from us)
        _c1_completed[completed_key] = now
        return

    # Dedicated OpenCode/Codex channels go straight to their own LAN inbox.
    # The pager's WiFi may be off; only the always-on gateway needs LAN access.
    if agent in AGENT_CHANNELS:
        if enqueue_agent_message(agent, full):
            _c1_completed[completed_key] = now
            log.info("%s inbox +1 (%d pending)", agent, len(_agent_inboxes[agent]))
        else:
            log.error("%s inbox full; refusing to drop an unacked item", agent)
        return

    # Forward to pager-agent-bridge (same contract as WiFi AGENTS skill)

    bridge = _agent_bridge_url()
    code, resp = await asyncio.to_thread(
        _http_json_post,
        f"{bridge}/v1/chat",
        {
            "agent": agent,
            "session": "pager-mesh",
            "text": full,
            "from": "tlora-mesh",
        },
        30.0,
    )
    log.info("agent bridge ← mesh chat %s → %s %s", agent, code, resp[:120])
    if 200 <= code < 300:
        _c1_completed[completed_key] = now
    # Bridge replies async via pager_push → notify-out mesh C1 downlink.
    # No mesh bot-reply here.


def handle_r1_reply(sender: str, text: str) -> None:
    """R1|key|reply — single-frame mesh fallback for a typed pager reply.

    The reply text may itself contain "|". Shares the store and dedup path
    with POST /reply, so a WiFi/mesh double delivery stays one logical reply.
    """
    parts = text.split("|", 2)
    if len(parts) < 3:
        log.warning("bad R1 from %s: %r", sender, text[:80])
        return
    cleaned = _clean_reply_fields(parts[1], parts[2])
    if cleaned is None:
        log.warning("invalid R1 fields from %s: %r", sender, text[:80])
        return
    key, reply = cleaned
    created, _entry = store_reply(key, reply, "mesh")
    log.info("reply (mesh R1) key=%s stored=%s", key[:32], created)


async def reply_to(sender_prefix: str, text: str) -> None:
    if not mc:
        return
    contact = await find_contact_by_prefix(sender_prefix)
    if not contact and target_contact:
        contact = target_contact
    if not contact:
        log.warning("no contact for reply to %s", sender_prefix)
        return
    try:
        await mc.commands.send_msg(
            contact, truncate(text, cfg.get("mesh_msg_limit", 200))
        )
    except Exception as e:
        log.error("send_msg failed: %s", e)


async def find_contact_by_prefix(prefix: str):
    if not mc or not prefix:
        return None
    # library helper if present
    fn = getattr(mc, "get_contact_by_key_prefix", None)
    if callable(fn):
        try:
            c = fn(prefix)
            if c:
                return c
        except Exception:
            pass
    try:
        result = await mc.commands.get_contacts()
    except Exception as e:
        log.error("get_contacts: %s", e)
        return None
    if not result or result.type == EventType.ERROR:
        return None
    payload = result.payload
    # dict keyed by pubkey, or list
    items: list[Any]
    if isinstance(payload, dict):
        items = list(payload.values()) if payload else []
        # also try keys
        for k, v in payload.items():
            if str(k).startswith(prefix):
                return v if v is not None else k
    elif isinstance(payload, list):
        items = payload
    else:
        return None
    for c in items:
        pk = c if isinstance(c, str) else getattr(c, "public_key", "") or ""
        if str(pk).startswith(prefix):
            return c
    return None


async def resolve_contact(prefix: str | None = None):
    global target_contact
    target_prefix = prefix or cfg.get("target_node", "CHANGE_ME")
    if not target_prefix or target_prefix == "CHANGE_ME":
        log.warning("target_node not set — will use first incoming sender")
        return
    c = await find_contact_by_prefix(target_prefix)
    if c:
        target_contact = c
        log.info("Target contact resolved: %s", target_prefix)
    else:
        log.warning("Target contact %s not found", target_prefix)


# ── HTTP ─────────────────────────────────────────────────────────────────────


async def webhook_send(request: web.Request) -> web.Response:
    if target_contact is None:
        return web.Response(text="No target contact\n", status=503)
    body = await request.text()
    if not body.strip():
        return web.Response(text="Empty body\n", status=400)
    msg = truncate(body, cfg.get("mesh_msg_limit", 200))
    log.info("Webhook send: %s", msg)
    try:
        result = await mc.commands.send_msg(target_contact, msg)
        if result and result.type == EventType.ERROR:
            return web.Response(text=f"Send error: {result.payload}\n", status=500)
    except Exception as e:
        return web.Response(text=f"Send error: {e}\n", status=500)
    return web.Response(text="OK\n")


async def webhook_pager(request: web.Request) -> web.Response:
    """Legacy alias: LAN-only push to pager /notify (no mesh TX)."""
    ctype = request.headers.get("Content-Type", "")
    if "json" in ctype:
        try:
            data = await request.json()
        except Exception:
            return web.Response(text="bad json\n", status=400)
        payload = normalize_payload(data)
    else:
        text = (await request.text()).strip()
        p1 = parse_p1(text)
        payload = p1 or {
            "level": "info",
            "source": "mesh",
            "title": "MESH",
            "body": text[:256],
        }
    code, resp = await asyncio.to_thread(pager_post_notify, payload)
    return web.Response(
        text=json.dumps({"http": code, "resp": resp}) + "\n",
        content_type="application/json",
        status=200 if 200 <= code < 300 else 502,
    )


async def webhook_notify_out(request: web.Request) -> web.Response:
    """
    Main egress for home notifications via Heltec gateway.

    POST JSON:
      {level, source, title, body, id?, mode?}
    mode: both | prefer_wifi | wifi_only | mesh_only  (default from config)

    - wifi_only / both / prefer_wifi → LAN POST pager /notify
    - both / mesh_only / prefer_wifi(on wifi fail) → private MeshCore DM (pager_pubkey) P1|…
    """
    try:
        data = await request.json()
    except Exception:
        # plain P1 or text
        text = (await request.text()).strip()
        p1 = parse_p1(text)
        if not p1:
            data = {
                "level": "info",
                "source": "home",
                "title": "notify",
                "body": text[:256],
            }
        else:
            data = p1
    mode = data.pop("mode", None) if isinstance(data, dict) else None
    if not isinstance(data, dict):
        return web.json_response(
            {"ok": False, "error": "JSON object required"}, status=400
        )
    delivery_key = _notify_delivery_key(data)
    if delivery_key and delivery_key in _notify_deliveries:
        duplicate = dict(_notify_deliveries[delivery_key])
        duplicate["duplicate"] = True
        return web.json_response(duplicate)
    if delivery_key and delivery_key in _notify_reservations:
        return web.json_response(
            {
                "ok": False,
                "ambiguous": True,
                "error": "delivery outcome requires manual resolution",
            },
            status=409,
        )
    if delivery_key:
        _notify_reservations[delivery_key] = {
            "state": "inflight",
            "reservedAt": int(time.time()),
        }
        _persist_agent_inboxes()
    result = await notify_out(data, mode=mode)
    if delivery_key and result.get("ok"):
        _notify_deliveries[delivery_key] = result
        _notify_reservations.pop(delivery_key, None)
        while len(_notify_deliveries) > 500:
            _notify_deliveries.pop(next(iter(_notify_deliveries)))
        _persist_agent_inboxes()
    elif delivery_key:
        _notify_reservations[delivery_key]["state"] = "ambiguous"
        _persist_agent_inboxes()
        return web.json_response(
            {
                "ok": False,
                "ambiguous": True,
                "error": "delivery failed after durable reservation",
                "result": result,
            },
            status=409,
        )
    status = 200 if result.get("ok") else 502
    return web.json_response(result, status=status)


async def webhook_health(request: web.Request) -> web.Response:
    info = {}
    if mc and mc.self_info:
        info = {
            "name": mc.self_info.get("name"),
            "pk": (mc.self_info.get("public_key") or "")[:16],
            "freq": mc.self_info.get("radio_freq"),
            "connected": bool(mc.is_connected),
        }
    return web.json_response(
        {
            "ok": True,
            "service": "meshcore-daemon",
            "radio": info,
            "notify_mode": (cfg.get("notify") or {}).get("mode", "both"),
            "endpoints": [
                "/health",
                "/ping",
                "/send",
                "/pager",
                "/notify-out",
                "/reply",
                "/replies",
                "/opencode/inbox",
                "/opencode/inbox/ack",
                "/codex/inbox",
                "/codex/inbox/ack",
                "/codex/inbox/reject",
            ],
        }
    )


async def webhook_ping(request: web.Request) -> web.Response:
    """Rich gateway probe for the pager MESHCORE → PING screen.

    Query: ?t=<client_ms> echoed back so the pager can compute RTT.
    """
    qs = request.rel_url.query
    client_t = qs.get("t")
    try:
        client_t_i = int(client_t) if client_t is not None else None
    except ValueError:
        client_t_i = None

    radio: dict = {}
    if mc and mc.self_info:
        s = mc.self_info
        radio = {
            "name": s.get("name"),
            "public_key": s.get("public_key"),
            "pk8": (s.get("public_key") or "")[:8],
            "pk16": (s.get("public_key") or "")[:16],
            "freq": s.get("radio_freq"),
            "bw": s.get("radio_bw"),
            "sf": s.get("radio_sf"),
            "cr": s.get("radio_cr"),
            "tx_power": s.get("tx_power"),
            "max_tx_power": s.get("max_tx_power"),
            "connected": bool(mc.is_connected),
        }

    # channel 0 snapshot (best-effort)
    ch0: dict = {}
    if mc and mc.is_connected:
        try:
            r = await mc.commands.get_channel(0)
            if r and r.type != EventType.ERROR and isinstance(r.payload, dict):
                ch0 = {
                    "idx": r.payload.get("channel_idx", 0),
                    "name": r.payload.get("channel_name"),
                    "hash": r.payload.get("channel_hash"),
                }
        except Exception as e:
            ch0 = {"error": str(e)[:80]}

    uptime_s = int(time.monotonic() - start_time)
    body = {
        "ok": True,
        "service": "meshcore-daemon",
        "role": "home-gateway",
        "server_ms": int(time.time() * 1000),
        "client_t": client_t_i,
        "daemon_uptime_s": uptime_s,
        "notify_mode": (cfg.get("notify") or {}).get("mode", "both"),
        "mesh_channel": int((cfg.get("mesh") or {}).get("channel", 0)),
        "radio": radio,
        "channel0": ch0,
        "transport": {
            "wifi_path": "POST /notify on pager LAN",
            "mesh_path": "private DM to mesh.pager_pubkey only (never Public)",
            "pager_paired": bool(mesh_pager_dst()),
            "pager_pk8": (mesh_pager_dst() or "")[:8] or None,
            "note": "same notify UI either path; pager needs MeshCore identity for DM RX",
        },
    }
    return web.json_response(body)


async def webhook_reply(request: web.Request) -> web.Response:
    """Typed reply intake from the pager device (device scope).

    POST JSON: {"key": "<card key>", "reply": "<typed text>"}
    """
    try:
        data = await request.json()
    except ValueError:
        return web.json_response({"ok": False, "error": "JSON required"}, status=400)
    if not isinstance(data, dict):
        return web.json_response(
            {"ok": False, "error": "JSON object required"}, status=400
        )
    cleaned = _clean_reply_fields(data.get("key"), data.get("reply"))
    if cleaned is None:
        return web.json_response(
            {"ok": False, "error": "key and reply must be non-empty strings"},
            status=400,
        )
    key, reply = cleaned
    created, entry = store_reply(key, reply, "http")
    log.info("reply (http) key=%s stored=%s", key[:32], created)
    return web.json_response(
        {"ok": True, "stored": created, "duplicate": not created, "id": entry["id"]}
    )


async def webhook_replies(request: web.Request) -> web.Response:
    """Admin-only list of stored device replies, newest first.

    Query: ?key=<card key> narrows to one card's replies.
    """
    wanted = request.rel_url.query.get("key")
    rows = [row for row in _replies if not wanted or row.get("key") == wanted]
    rows.reverse()
    return web.json_response({"ok": True, "count": len(rows), "items": rows})


async def webhook_agent_inbox(request: web.Request) -> web.Response:
    """List one agent's durable queue; removal requires explicit ack."""
    agent = request.match_info["agent"]
    if agent not in AGENT_CHANNELS:
        return web.json_response({"ok": False, "error": "unknown agent"}, status=404)
    items = list_agent_messages(agent)
    return web.json_response({"ok": True, "count": len(items), "items": items})


async def webhook_agent_inbox_ack(request: web.Request) -> web.Response:
    agent = request.match_info["agent"]
    if agent not in AGENT_CHANNELS:
        return web.json_response({"ok": False, "error": "unknown agent"}, status=404)
    try:
        data = await request.json()
    except Exception:
        return web.json_response({"ok": False, "error": "JSON required"}, status=400)
    raw_ids = data.get("ids") if isinstance(data, dict) else None
    if not isinstance(raw_ids, list) or not all(isinstance(x, str) for x in raw_ids):
        return web.json_response({"ok": False, "error": "ids[] required"}, status=400)
    removed = ack_agent_messages(agent, set(raw_ids))
    return web.json_response({"ok": True, "acked": removed})


async def webhook_agent_inbox_reject(request: web.Request) -> web.Response:
    agent = request.match_info["agent"]
    if agent not in AGENT_CHANNELS:
        return web.json_response({"ok": False, "error": "unknown agent"}, status=404)
    try:
        data = await request.json()
    except Exception:
        return web.json_response({"ok": False, "error": "JSON required"}, status=400)
    raw_ids = data.get("ids") if isinstance(data, dict) else None
    reason = str(data.get("reason") or "rejected") if isinstance(data, dict) else ""
    if not isinstance(raw_ids, list) or not all(isinstance(x, str) for x in raw_ids):
        return web.json_response({"ok": False, "error": "ids[] required"}, status=400)
    rejected = reject_agent_messages(agent, set(raw_ids), reason)
    return web.json_response({"ok": True, "rejected": rejected})


def create_webhook_app() -> web.Application:
    app = web.Application(middlewares=[gateway_auth_middleware])
    app.router.add_post("/send", webhook_send)
    app.router.add_post("/pager", webhook_pager)
    app.router.add_post("/notify-out", webhook_notify_out)
    app.router.add_post("/reply", webhook_reply)
    app.router.add_get("/replies", webhook_replies)
    app.router.add_get("/{agent:opencode|codex}/inbox", webhook_agent_inbox)
    app.router.add_post("/{agent:opencode|codex}/inbox/ack", webhook_agent_inbox_ack)
    app.router.add_post(
        "/{agent:opencode|codex}/inbox/reject", webhook_agent_inbox_reject
    )
    app.router.add_get("/health", webhook_health)
    app.router.add_get("/ping", webhook_ping)
    return app


async def internet_monitor():
    global inet_was_up
    monitor_cfg = cfg.get("monitor", {})
    if not monitor_cfg.get("enabled", True):
        log.info("Internet monitor disabled")
        return
    interval = monitor_cfg.get("interval_seconds", 300)
    host = monitor_cfg.get("ping_host", "8.8.8.8")
    log.info("Internet monitor every %ss ping %s", interval, host)
    while True:
        await asyncio.sleep(interval)
        is_up = await asyncio.to_thread(check_inet, host)
        if inet_was_up and not is_up:
            log.warning("Internet DOWN")
            if target_contact and mc:
                await mc.commands.send_msg(target_contact, "ALERT: Internet is DOWN")
        elif not inet_was_up and is_up:
            log.info("Internet restored")
            if target_contact and mc:
                await mc.commands.send_msg(target_contact, "Internet restored")
        inet_was_up = is_up


async def main():
    global cfg, mc

    cfg = load_config()
    # env overrides for secrets
    if os.environ.get("PAGER_TOKEN"):
        cfg.setdefault("pager", {})["token"] = os.environ["PAGER_TOKEN"]
    if os.environ.get("PAGER_URL"):
        cfg.setdefault("pager", {})["url"] = os.environ["PAGER_URL"]
    missing_tokens = [name for name, token in gateway_api_tokens().items() if not token]
    if missing_tokens:
        raise SystemExit("missing gateway API tokens: " + ", ".join(missing_tokens))
    load_agent_inboxes()
    load_replies()

    serial_cfg = cfg.get("serial", {})
    port = serial_cfg.get("port", "/dev/ttyUSB0")
    baud = serial_cfg.get("baudrate", 115200)

    log.info("Connecting MeshCore companion on %s…", port)
    mc = await MeshCore.create_serial(port, baudrate=baud)
    if mc is None or not mc.is_connected:
        raise SystemExit(f"Failed to open companion on {port}")
    log.info(
        "Connected name=%s pk=%s… freq=%s",
        (mc.self_info or {}).get("name"),
        ((mc.self_info or {}).get("public_key") or "")[:12],
        (mc.self_info or {}).get("radio_freq"),
    )

    mc.subscribe(EventType.CONTACT_MSG_RECV, on_mesh_message)
    # channel flood messages (if library emits them)
    if hasattr(EventType, "CHANNEL_MSG_RECV"):
        mc.subscribe(EventType.CHANNEL_MSG_RECV, on_mesh_message)

    await mc.start_auto_message_fetching()
    await resolve_contact()

    asyncio.create_task(internet_monitor())

    webhook_cfg = cfg.get("webhook", {})
    app = create_webhook_app()
    runner = web.AppRunner(app)
    await runner.setup()
    host = webhook_cfg.get("host", "0.0.0.0")
    port_w = int(webhook_cfg.get("port", 8325))
    await web.TCPSite(runner, host, port_w).start()
    log.info(
        "Webhook http://%s:%s  (/health /send /pager /notify-out) mode=%s",
        host,
        port_w,
        (cfg.get("notify") or {}).get("mode", "both"),
    )

    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass
    await stop.wait()
    log.info("Shutting down…")
    await runner.cleanup()
    await mc.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
