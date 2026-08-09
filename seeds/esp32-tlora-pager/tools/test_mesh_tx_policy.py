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
assert "expected_ack_crc = 0" in client
assert 'agents_on_inbound(agent, "(mesh delivery failed - resend)")' in mesh
reply = main[main.index("static bool reply_upstream_mesh"):
             main.index("static void reply_upstream_poll")]
assert "mesh_client_ack_pending()" in reply, "R1 must not overwrite a C1 ACK"

print("Mesh C1 ACK/retry policy tests: OK")
