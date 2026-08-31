#!/usr/bin/env python3
"""Static regressions for serialized C1 ACK/retry delivery."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
mesh = (ROOT / "src" / "skills" / "meshcore.cpp").read_text(encoding="utf-8")
client = (ROOT / "src" / "mesh" / "mc_client.cpp").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

poll = mesh[mesh.index("static void mesh_chat_tx_poll"):
            mesh.index("static bool mesh_probe_gateway")]
uplink = mesh[mesh.index("static bool mesh_chat_uplink"):
              mesh.index("static void skill_meshcore_init")]

assert "MESH_CHAT_TX_RETRIES" in poll
assert "mesh_client_ack_pending()" in poll
assert "mesh_client_cancel_pending_ack()" in poll
assert "ack_seen" in poll and "waiting_ack" in poll
assert "mesh_client_send_to_gateway" in poll
assert "mesh_client_send_to_gateway" not in uplink, (
    "uplink must enqueue frames; the poller serializes ACKed radio sends"
)
assert "delay(350)" not in uplink, "multi-part chat must not freeze the UI"
assert "g_mesh_chat_tx.active = true" in uplink
assert "agents_delivery_key_valid(delivery_key)" in uplink
assert 'snprintf(mid, sizeof(mid), "%s", delivery_key)' in uplink, (
    "WiFi timeout fallback must reuse the same idempotency key as C1 mid"
)
assert "expected_ack_crc = 0" in client
assert 'agents_on_inbound(agent, "(mesh delivery failed - resend)", false)' in mesh
manual_ping = main[main.index("// ---- Mesh path: sparse private probe"):
                   main.index("static void kb_layout_save", main.index("// ---- Mesh path: sparse private probe"))]
assert "!g_mesh_chat_tx.active && !mesh_client_ack_pending()" in manual_ping
reply = main[main.index("static bool reply_upstream_mesh"):
             main.index("static void outbox_reply_poll")]
assert "mesh_client_ack_pending()" in reply, "R1 must not overwrite a C1 ACK"

print("Mesh C1 ACK/retry policy tests: OK")
