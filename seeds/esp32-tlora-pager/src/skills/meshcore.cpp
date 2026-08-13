/*
 * skills/meshcore.cpp — MeshCore private-link transport into notify
 *
 * Home Heltec gateway (daemon :8325) sends, as a MeshCore private DM (never
 * Public); the AUTHORITATIVE wire spec lives in src/notify_wire.h:
 *   P1|level|source|title|body[|key]           legacy, key OPTIONAL (heuristic)
 *   P2|level|source|title|body|key             explicit trailing card key
 *   M1|mid|i|n|level|source|title|chunk        legacy multipart, KEYLESS
 *   M2|mid|i|n|level|source|title|chunk|key     multipart with stable card key
 * `key` is the card's stable client id: it dedups this card against the SAME card
 * arriving over another transport (M/P/LXMF), and routes a typed reply upstream.
 * On the v1 wires it is recovered by a heuristic (P1) or absent (M1); the v2 tags
 * make it explicit — the trailing '|' always ends the arbitrary body/chunk and
 * begins the key. See notify_wire_parse_p / notify_wire_parse_m.
 *
 * The pager answers on the same link with two device->gateway frames:
 *   R1|key|reply    a user reply typed on the card (main.cpp reply_upstream_mesh)
 *   A1|key          a per-card DELIVERY ACK: "card `key` was admitted here, you
 *                   may dequeue it from the store-and-forward queue" (ticket
 *                   CARD-DELIVERY / C2, staged in mesh_card_ack_stage and emitted
 *                   from mesh_card_ack_poll; full spec in src/card_ack.h). Only a
 *                   card that arrived over an ASYNC lane (mesh/LXMF) is A1'd — the
 *                   WiFi lane's synchronous 200 already confirmed delivery — and
 *                   only a KEYED card, since a keyless one cannot be dequeued by
 *                   key. R1/A1/C1/keepalive all share the ONE MeshCore ACK slot.
 *
 * Pair identity (Ed25519) lives on SPIFFS:
 *   /mesh_identity.id  — pub(32)+prv(64) MeshCore LocalIdentity blob
 *   /mesh_pair.json    — public meta for status UI
 *   /mesh_probe_s.txt  — sparse keepalive interval seconds (default 900)
 *
 * Clock face "M" glyph = private-path health (not a beacon flood):
 *   - outbound: rare private DM "MC|k" to Heltec, MeshCore ACK is enough
 *   - multi-hop: MeshCore stores path after first contact; ACKs ride it
 *   - inbound P1 from gateway also marks the link OK
 *
 * Radio: SX1262 + BaseChatMesh in src/mesh/ (private DM only).
 */

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <string.h>
#include <stdio.h>
#include "mesh/mc_client.h"
#include "mesh/utf8_chunk.h"
#include "../l1_frame.h"    /* pure L1 frame parse + byte reassembler (host-tested) */
#include "../notify_wire.h" /* pure P/M notify-wire parse — the cross-transport key */
#include "../card_ack.h"    /* pure A1|key delivery-ack: decision + wire + dedup ring */

/* THE SHARED LXMF ROUTER lives in skills/rns.cpp, which main.cpp #includes AFTER
 * this file into the same translation unit. Forward-declare it so the MeshCore
 * "L1" receive path can drive the SAME parse/route/counter pipeline the Reticulum
 * poll does — one router, two transports (see lxmf_ingest_wire in rns.cpp). It
 * takes an LXMF opportunistic wire (source+sig+msgpack, no dest hash) and returns
 * true iff it parsed and routed; it owns the rns_lxmf_dest_hash_ok gate. */
static bool lxmf_ingest_wire(const uint8_t *wire, size_t len);

#define MESH_ID_PATH       "/mesh_identity.id"
#define MESH_PAIR_PATH     "/mesh_pair.json"
#define MESH_PROBE_PATH    "/mesh_probe_s.txt"
#define MESH_PUB_LEN       32
#define MESH_PRV_LEN       64
#define MESH_ID_BLOB_LEN   (MESH_PUB_LEN + MESH_PRV_LEN)
/* Sparse keepalive — do not spam the mesh. Override via SPIFFS. */
#define MESH_PROBE_DEFAULT_S   300u   /* 5 min — counter stays honest */
#define MESH_PROBE_MIN_S       120u   /* 2 min floor */
#define MESH_PROBE_MAX_S       7200u  /* 2 h cap */
/* After this many failed probes (or silence), icon → DOWN. */
#define MESH_FAIL_DOWN         2
/* "This DM was handled" where there is no card id to return. The caller only
 * tests the result for zero (to bump dm_rx and mark the link alive), so any
 * non-zero value does; naming it keeps it from reading as a real card id. The
 * L1 branch uses the same sentinel. */
#define MESH_RX_HANDLED        1u
/* Wire keepalive: daemon silent bot; optional app pong MC|a for alive counter. */
/* One pending peer reply; a chat line is bounded by AGENT_TEXT_LEN. */
#define MESH_PEER_TX_TEXT_CAP  512
#define MESH_KEEPALIVE_TEXT    "MC|k"
#define MESH_ALIVE_PONG_TEXT   "MC|a"

struct MeshPairInfo {
    bool     has_identity;
    bool     want_identity;    /* no identity anywhere: mint one on radio bring-up */
    bool     has_meta;
    bool     radio_ready;      /* BaseChatMesh up — probe can TX */
    char     name[16];
    char     public_key_hex[65];
    char     heltec_pk_hex[65];
    float    freq;
    uint8_t  sf;
    float    bw;
    char     radio_state[24];
    uint32_t dm_rx_count;
    uint32_t last_dm_ms;
    /* Sparse private-path health (clock "M") */
    uint32_t probe_interval_s;
    uint32_t last_ok_ms;       /* last ACK or inbound from GW */
    uint32_t last_probe_ms;    /* last outbound attempt */
    uint32_t last_rtt_ms;      /* 0 = unknown */
    uint8_t  fail_streak;
    uint32_t probe_ok_count;
    uint32_t probe_fail_count;
};

static MeshPairInfo g_mesh;

static void mesh_hex_of(const uint8_t *bin, size_t n, char *out, size_t out_n) {
    if (!out || out_n < n * 2 + 1) return;
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[(bin[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[bin[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static void mesh_pair_clear() {
    memset(&g_mesh, 0, sizeof(g_mesh));
    snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "idle");
    g_mesh.freq = 869.618f;
    g_mesh.sf = 8;
    g_mesh.bw = 62.5f;
    g_mesh.probe_interval_s = MESH_PROBE_DEFAULT_S;
}

static void mesh_load_probe_interval() {
    g_mesh.probe_interval_s = MESH_PROBE_DEFAULT_S;
    if (!SPIFFS.exists(MESH_PROBE_PATH)) return;
    File f = SPIFFS.open(MESH_PROBE_PATH, "r");
    if (!f) return;
    long v = f.parseInt();
    f.close();
    if (v < (long)MESH_PROBE_MIN_S) v = MESH_PROBE_MIN_S;
    if (v > (long)MESH_PROBE_MAX_S) v = MESH_PROBE_MAX_S;
    g_mesh.probe_interval_s = (uint32_t)v;
}

/* Per-agent success->fail edge: the synthetic "(mesh delivery failed - resend)"
 * chat line is injected once per outage per agent room. Repeated failures while
 * that room already got the line stay silent (its user has been told); the flag
 * clears on the next chat ACK for that agent, and ANY proof the link is alive
 * (mesh_link_mark_ok: probe/keepalive ACK, inbound DM) re-arms every room, so
 * the next outage after a probe-proven recovery is reported again. Lives
 * outside MeshChatTx, which is zeroed per send. AGENTS_N / agents_find come
 * from agents.cpp (included before this file). */
static bool g_mesh_chat_tx_failed[AGENTS_N] = {};

/* Mark private path alive (MeshCore ACK or inbound DM from gateway). */
static void mesh_link_mark_ok(uint32_t rtt_ms) {
    g_mesh.last_ok_ms = millis();
    g_mesh.last_rtt_ms = rtt_ms;
    g_mesh.fail_streak = 0;
    if (rtt_ms) g_mesh.probe_ok_count++;
    /* Link proven alive: re-arm failure reporting in every agent room. */
    for (int i = 0; i < AGENTS_N; i++) g_mesh_chat_tx_failed[i] = false;
}

/* Seconds since last private-path alive; -1 if never. For clock "M12m". */
static int mesh_alive_age_s() {
    if (!g_mesh.has_identity || g_mesh.last_ok_ms == 0) return -1;
    return (int)((millis() - g_mesh.last_ok_ms) / 1000UL);
}

/* Clock chrome: see HwMeshUi in hw_ui.h */
static int mesh_ui_state() {
    if (!g_mesh.has_identity) return MESH_UI_OFF;
    if (!g_mesh.radio_ready) {
        /* Keys on board, stack not TX/RX yet — dim M, not a false green. */
        if (g_mesh.last_ok_ms == 0) return MESH_UI_WAIT;
    }
    if (g_mesh.last_ok_ms == 0) {
        return g_mesh.fail_streak >= MESH_FAIL_DOWN ? MESH_UI_DOWN : MESH_UI_WAIT;
    }
    unsigned long age = millis() - g_mesh.last_ok_ms;
    unsigned long stale_after = (unsigned long)g_mesh.probe_interval_s * 1000UL * 2UL;
    unsigned long down_after  = (unsigned long)g_mesh.probe_interval_s * 1000UL * 4UL;
    if (g_mesh.fail_streak >= MESH_FAIL_DOWN && age > stale_after)
        return MESH_UI_DOWN;
    if (age > down_after) return MESH_UI_DOWN;
    if (age > stale_after) return MESH_UI_STALE;
    return MESH_UI_OK;
}

/* Pending probe: send returns true; ACK callback sets last_ok. */
static bool mesh_probe_awaiting_ack = false;
static uint32_t mesh_on_private_text(const uint8_t *from_pubkey,
                                    const char *from_name,
                                    const char *text);  /* fwd */

#define MESH_CHAT_TX_MAX_PARTS 32
#define MESH_CHAT_TX_RETRIES   3
struct MeshChatTx {
    bool active;
    bool waiting_ack;
    bool ack_seen;
    uint8_t count;
    uint8_t next;
    uint8_t attempts;
    uint32_t sent_ms;
    uint32_t ack_timeout_ms;
    uint32_t retry_after_ms;
    char agent[13];
    char frames[MESH_CHAT_TX_MAX_PARTS][160];
};
static MeshChatTx g_mesh_chat_tx = {};

/*
 * PEER REPLY HAND-OFF — the MeshCore stack is the LOOP TASK'S, and only its.
 *
 * BaseChatMesh::sendMessage allocates from the packet pool, runs the ECDH,
 * touches the duplicate table and the dispatcher's TX queue, and arms
 * expected_ack_crc — all state mesh_client_loop() drives on the loop task with
 * no lock of its own. Calling it from anywhere else corrupts that state, and
 * the corruption surfaces later as a panic with nothing pointing back at the
 * caller. Every other producer already respects this: the agent uplink only
 * fills g_mesh_chat_tx and returns, and the frames go out from the poll below.
 *
 * A peer reply is submitted the same way. POST /agents/send resolves any
 * conversation and runs on the AsyncTCP task, so without this it would drive
 * the radio underneath the loop task. One slot is enough: a reply is a
 * keystroke, the drain is ~8 ms away, and a second submitted before the first
 * leaves is refused with a reason the room shows rather than queued behind an
 * unbounded backlog.
 *
 * WHAT THE SUBMIT ITSELF TOUCHES, stated plainly so "no stack call off the loop
 * task" is not read as more absolute than it is: mesh_peer_tx_submit() runs
 * mesh_client_ready() and mesh_client_knows_peer() on the CALLER's task. Both
 * are read-only — a bounds-checked scan of a fixed contacts[] array — so they
 * mutate nothing the loop task owns and cannot corrupt it; at worst a contact
 * added in the same instant is missed, and the authoritative lookup runs again
 * in the drain. They are here so the room can say "unknown peer" while the user
 * is still looking at what they typed. The pin in tools/test_transport_pins.py
 * bans the stack symbols BY FILE (skills/agents.cpp), which is a proxy for "not
 * on an arbitrary task" and not a check of the task itself.
 */
struct MeshPeerTx {
    volatile bool pending;
    uint8_t peer[32];
    char text[MESH_PEER_TX_TEXT_CAP];
};
static MeshPeerTx g_mesh_peer_tx = {};
static portMUX_TYPE g_mesh_peer_mux = portMUX_INITIALIZER_UNLOCKED;

/* Submitted from ANY task (keyboard on the loop task, HTTP on AsyncTCP). Does
 * no radio work: it fills the slot and returns. */
static bool mesh_peer_tx_submit(const uint8_t *pubkey, const char *text,
                                const char **why) {
    if (!pubkey || !text || !text[0]) {
        if (why) *why = "nothing to send";
        return false;
    }
    if (!g_mesh.radio_ready || !mesh_client_ready()) {
        if (why) *why = "radio down";
        return false;
    }
    /* Refused here rather than in the drain, so the room can say so while the
     * user is still looking at what they typed. */
    if (!mesh_client_knows_peer(pubkey)) {
        if (why) *why = "unknown peer";
        return false;
    }
    bool taken = false;
    portENTER_CRITICAL(&g_mesh_peer_mux);
    if (!g_mesh_peer_tx.pending) {
        memcpy(g_mesh_peer_tx.peer, pubkey, sizeof(g_mesh_peer_tx.peer));
        snprintf(g_mesh_peer_tx.text, sizeof(g_mesh_peer_tx.text), "%s", text);
        g_mesh_peer_tx.pending = true;
        taken = true;
    }
    portEXIT_CRITICAL(&g_mesh_peer_mux);
    if (!taken && why) *why = "radio busy";
    return taken;
}

/* Drained on the LOOP TASK from the mesh tick — the only place the peer send
 * touches the stack. */
static void mesh_peer_tx_poll() {
    if (!g_mesh_peer_tx.pending) return;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return;
    if (mesh_client_ack_pending()) return;   /* let the outstanding one finish */

    uint8_t peer[32];
    char text[MESH_PEER_TX_TEXT_CAP];
    portENTER_CRITICAL(&g_mesh_peer_mux);
    memcpy(peer, g_mesh_peer_tx.peer, sizeof(peer));
    memcpy(text, g_mesh_peer_tx.text, sizeof(text));
    g_mesh_peer_tx.pending = false;
    portEXIT_CRITICAL(&g_mesh_peer_mux);

    const char *why = nullptr;
    if (!mesh_client_send_to_peer(peer, text, &why))
        event_add("mesh peer send failed: %s", why ? why : "unknown");
}

/*
 * PER-CARD DELIVERY ACK over the mesh lane (ticket CARD-DELIVERY / C2).
 *
 * When a KEYED notification card is admitted off a MeshCore DM (P/M wire), the
 * gateway that pushed it store-and-forward gets no confirmation the way the WiFi
 * lane does. mesh_card_ack_stage() records the card's key; mesh_card_ack_poll()
 * emits "A1|key" as a private DM to the gateway from the LOOP TASK, sharing the
 * SINGLE MeshCore ACK slot with R1/C1/probe (mesh_client_ack_pending gate) so it
 * never stomps a send already in flight.
 *
 * A tiny FIFO, not one slot: a store-and-forward gateway flushing a backlog on
 * reconnect delivers several cards in a burst, and each owes an ack; a single
 * slot would drop all but the last. It is drained one per pass (the ack slot only
 * carries one frame at a time anyway). g_mesh_ack_seen stops an ACK storm — a key
 * acked recently is not re-staged — and is marked on EMIT SUCCESS, so an ack that
 * failed to leave is retried when the gateway re-delivers the card. All bounded,
 * all static (loop-task stack rule). The stage runs on the loop task (the mesh DM
 * callback's task) but is spinlock-guarded so a future off-loop caller is safe. */
#define MESH_ACK_Q_MAX 6
static char       g_mesh_ack_q[MESH_ACK_Q_MAX][NW_KEY_CAP];
static uint8_t    g_mesh_ack_qn = 0;
static CardAckSeen g_mesh_ack_seen = {};
static portMUX_TYPE g_mesh_ack_mux = portMUX_INITIALIZER_UNLOCKED;

static void mesh_card_ack_stage(const char *key) {
    if (card_ack_decide(CONV_MESH, key) != CARD_ACK_MESH) return;
    portENTER_CRITICAL(&g_mesh_ack_mux);
    bool skip = card_ack_seen_has(&g_mesh_ack_seen, key);  /* already acked */
    for (int i = 0; !skip && i < g_mesh_ack_qn; i++)
        if (strcmp(g_mesh_ack_q[i], key) == 0) skip = true;  /* already queued */
    if (!skip && g_mesh_ack_qn < MESH_ACK_Q_MAX) {
        snprintf(g_mesh_ack_q[g_mesh_ack_qn], NW_KEY_CAP, "%s", key);
        g_mesh_ack_qn++;
    }
    portEXIT_CRITICAL(&g_mesh_ack_mux);
}

/* Drained on the LOOP TASK from the mesh tick — the only place the ack touches
 * the stack. One A1 per pass, and only when the shared ACK slot is free. */
static void mesh_card_ack_poll() {
    if (g_mesh_ack_qn == 0) return;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return;
    if (!g_mesh.heltec_pk_hex[0]) return;
    if (mesh_client_ack_pending()) return;   /* R1/C1/probe/A1 share one slot */

    char key[NW_KEY_CAP];
    portENTER_CRITICAL(&g_mesh_ack_mux);
    snprintf(key, sizeof(key), "%s", g_mesh_ack_q[0]);
    for (int i = 1; i < g_mesh_ack_qn; i++)
        memcpy(g_mesh_ack_q[i - 1], g_mesh_ack_q[i], NW_KEY_CAP);
    g_mesh_ack_qn--;
    portEXIT_CRITICAL(&g_mesh_ack_mux);
    if (!key[0]) return;

    char frame[CARD_ACK_FRAME_CAP];
    if (!card_ack_encode(key, frame, sizeof(frame))) return;

    uint32_t ack = 0, est = 0;
    if (mesh_client_send_to_gateway(frame, &ack, &est)) {
        portENTER_CRITICAL(&g_mesh_ack_mux);
        card_ack_seen_add(&g_mesh_ack_seen, key);  /* delivered: suppress dups */
        portEXIT_CRITICAL(&g_mesh_ack_mux);
        event_add("card ack mesh A1: %s", key);
    } else {
        /* Not marked seen: the gateway will re-deliver, which re-stages it. */
        event_add("card ack mesh A1 TX failed: %s", key);
    }
}

static void mesh_chat_tx_fail(const char *reason) {
    char agent[13];
    snprintf(agent, sizeof(agent), "%s", g_mesh_chat_tx.agent);
    event_add("mesh chat FAILED %s part %u/%u", reason,
              (unsigned)(g_mesh_chat_tx.next + 1),
              (unsigned)g_mesh_chat_tx.count);
    memset(&g_mesh_chat_tx, 0, sizeof(g_mesh_chat_tx));
    int aidx = agents_find(agent);
    if (aidx < 0) return;  /* empty/unknown agent: no room to warn, no flag */
    if (g_mesh_chat_tx_failed[aidx]) return;  /* room already told: silent */
    /* Set only together with the injected line, so silence never == success. */
    g_mesh_chat_tx_failed[aidx] = true;
    agents_on_inbound(agent, "(mesh delivery failed - resend)", false);
}

static void mesh_chat_tx_poll() {
    if (!g_mesh_chat_tx.active || !g_mesh.radio_ready || !mesh_client_ready())
        return;

    unsigned long now = millis();
    if (g_mesh_chat_tx.waiting_ack) {
        if (g_mesh_chat_tx.ack_seen) {
            g_mesh_chat_tx.ack_seen = false;
            {   /* delivery to this room works again */
                int aidx = agents_find(g_mesh_chat_tx.agent);
                if (aidx >= 0) g_mesh_chat_tx_failed[aidx] = false;
            }
            g_mesh_chat_tx.waiting_ack = false;
            g_mesh_chat_tx.attempts = 0;
            g_mesh_chat_tx.next++;
            if (g_mesh_chat_tx.next >= g_mesh_chat_tx.count) {
                event_add("mesh chat delivered %u parts",
                          (unsigned)g_mesh_chat_tx.count);
                memset(&g_mesh_chat_tx, 0, sizeof(g_mesh_chat_tx));
                return;
            }
        } else if (now - g_mesh_chat_tx.sent_ms >= g_mesh_chat_tx.ack_timeout_ms) {
            mesh_client_cancel_pending_ack();
            g_mesh_chat_tx.waiting_ack = false;
            if (g_mesh_chat_tx.attempts >= MESH_CHAT_TX_RETRIES) {
                mesh_chat_tx_fail("ACK timeout");
                return;
            }
            g_mesh_chat_tx.retry_after_ms = now + 250UL;
            return;
        } else {
            return;
        }
    }

    if (g_mesh_chat_tx.retry_after_ms &&
        (int32_t)(now - g_mesh_chat_tx.retry_after_ms) < 0)
        return;
    g_mesh_chat_tx.retry_after_ms = 0;

    // Do not overwrite a keepalive's single ACK expectation. A stale probe is
    // cancelled after 12s so an interactive chat cannot wait a full interval.
    if (mesh_client_ack_pending()) {
        if (now - mesh_client_last_send_ms() >= 12000UL) {
            mesh_client_cancel_pending_ack();
            if (mesh_probe_awaiting_ack) {
                mesh_probe_awaiting_ack = false;
                g_mesh.fail_streak++;
                g_mesh.probe_fail_count++;
            }
        } else {
            return;
        }
    }

    uint32_t ack = 0, estimate = 0;
    if (!mesh_client_send_to_gateway(g_mesh_chat_tx.frames[g_mesh_chat_tx.next],
                                     &ack, &estimate)) {
        g_mesh_chat_tx.attempts++;
        if (g_mesh_chat_tx.attempts >= MESH_CHAT_TX_RETRIES) {
            mesh_chat_tx_fail("radio TX");
        } else {
            g_mesh_chat_tx.retry_after_ms = now + 1000UL;
        }
        return;
    }
    g_mesh_chat_tx.attempts++;
    g_mesh_chat_tx.sent_ms = now;
    uint32_t timeout = estimate + 1500UL;
    if (timeout < 3000UL) timeout = 3000UL;
    if (timeout > 12000UL) timeout = 12000UL;
    g_mesh_chat_tx.ack_timeout_ms = timeout;
    g_mesh_chat_tx.waiting_ack = true;
}

/*
 * One sparse private DM to Heltec. MeshCore path (incl. repeaters) is used
 * when the contact has out_path; protocol ACK is enough — no chatty reply.
 * Returns true if stack accepted TX (ACK may arrive later via callback).
 */
static bool mesh_probe_gateway(uint32_t *rtt_ms_out) {
    if (rtt_ms_out) *rtt_ms_out = 0;
    if (!g_mesh.has_identity || !g_mesh.heltec_pk_hex[0]) return false;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return false;
    uint32_t ack = 0, est = 0;
    if (!mesh_client_send_to_gateway(MESH_KEEPALIVE_TEXT, &ack, &est))
        return false;
    mesh_probe_awaiting_ack = true;
    /* RTT filled asynchronously in mesh_client on_ack callback. */
    return true;
}

static void mesh_cb_dm(const uint8_t *from_pubkey, const char *from_name,
                       const char *text) {
    if (!text) return;
    /* App-level alive pong from gateway (or echo) — refresh M counter, no card. */
    if (strcmp(text, "K") == 0 || strcmp(text, MESH_KEEPALIVE_TEXT) == 0 ||
        strcmp(text, MESH_ALIVE_PONG_TEXT) == 0 ||
        strncmp(text, "MC|k", 4) == 0 || strncmp(text, "MC|a", 4) == 0) {
        mesh_link_mark_ok(1);
        event_add("mesh alive pong");
        return;
    }
    mesh_on_private_text(from_pubkey, from_name, text);
}

static void mesh_cb_ack(uint32_t rtt_ms) {
    if (g_mesh_chat_tx.active && g_mesh_chat_tx.waiting_ack)
        g_mesh_chat_tx.ack_seen = true;
    else
        mesh_probe_awaiting_ack = false;
    mesh_link_mark_ok(rtt_ms ? rtt_ms : 1);
    event_add("mesh ACK rtt=%lu", (unsigned long)rtt_ms);
}

static void mesh_cb_log(const char *msg) {
    if (msg && msg[0]) event_add("mesh %s", msg);
}

static bool mesh_load_identity() {
    g_mesh.has_identity = false;
    g_mesh.public_key_hex[0] = '\0';
    /* NVS is the source of truth after the boot migration (it survives a SPIFFS
     * format); the /mesh_identity.id file is only a pre-migration fallback. Both
     * hold the same 96-byte pub(32)+prv(64) blob, so the public-key prefix is
     * read identically from either. This is a STATUS read; the crypto identity
     * is loaded (and minted, when absent) in mc_client.cpp, which owns the RNG. */
    uint8_t blob[MESH_ID_BLOB_LEN];
    size_t n = secret_store_get("mesh_id", blob, sizeof(blob));
    if (n != MESH_ID_BLOB_LEN) {
        if (!SPIFFS.exists(MESH_ID_PATH)) return false;
        File f = SPIFFS.open(MESH_ID_PATH, "r");
        if (!f) return false;
        n = f.read(blob, sizeof(blob));
        f.close();
        if (n != MESH_ID_BLOB_LEN) {
            event_add("mesh identity size %u (want %u)",
                      (unsigned)n, (unsigned)MESH_ID_BLOB_LEN);
            return false;
        }
    }
    mesh_hex_of(blob, MESH_PUB_LEN, g_mesh.public_key_hex, sizeof(g_mesh.public_key_hex));
    snprintf(g_mesh.name, sizeof(g_mesh.name), "%.8s", g_mesh.public_key_hex);
    for (int i = 0; g_mesh.name[i]; i++) {
        if (g_mesh.name[i] >= 'a' && g_mesh.name[i] <= 'f')
            g_mesh.name[i] = (char)(g_mesh.name[i] - 'a' + 'A');
    }
    g_mesh.has_identity = true;
    snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "keys_ok");
    return true;
}

static bool mesh_load_meta() {
    g_mesh.has_meta = false;
    if (!SPIFFS.exists(MESH_PAIR_PATH)) return false;
    File f = SPIFFS.open(MESH_PAIR_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    g_mesh.has_meta = true;
    if (doc["name"].is<const char *>()) {
        snprintf(g_mesh.name, sizeof(g_mesh.name), "%s", doc["name"].as<const char *>());
    }
    if (doc["public_key"].is<const char *>() && !g_mesh.has_identity) {
        snprintf(g_mesh.public_key_hex, sizeof(g_mesh.public_key_hex),
                 "%s", doc["public_key"].as<const char *>());
    }
    if (doc["heltec_public_key"].is<const char *>()) {
        snprintf(g_mesh.heltec_pk_hex, sizeof(g_mesh.heltec_pk_hex),
                 "%s", doc["heltec_public_key"].as<const char *>());
    }
    if (doc["freq"].is<float>()) g_mesh.freq = doc["freq"].as<float>();
    if (doc["sf"].is<int>()) g_mesh.sf = (uint8_t)doc["sf"].as<int>();
    if (doc["bw"].is<float>()) g_mesh.bw = doc["bw"].as<float>();
    return true;
}

/* ---- Multi-part reassembly (M1/M2 notify / C1 chat) -------------------------
 * Wire (gateway meshcore_daemon.encode_mesh_frames; full spec in notify_wire.h):
 *   P1|level|source|title|body[|key]            (single frame, key by heuristic)
 *   P2|level|source|title|body|key              (single frame, explicit key)
 *   M1|mid|i|n|level|source|title|chunk          (multipart, keyless)
 *   M2|mid|i|n|level|source|title|chunk|key      (multipart, explicit card key)
 *   C1|agent|mid|i|n|side|chunk   side=a|u
 * Truncate is per-frame; append is here until i==n, then push to inbox/thread.
 */
#define MESH_REASM_SLOTS   4
#define MESH_REASM_MAX    1800   /* cap assembled body (notify 240 + chat larger) */
#define MESH_REASM_TTL_MS 45000

struct MeshReasm {
    bool     used;
    char     kind;           /* 'M' or 'C' */
    char     mid[8];
    char     agent[16];
    char     level[8];
    char     source[NOTIFY_SOURCE_LEN];
    char     title[NOTIFY_TITLE_LEN];
    char     key[NOTIFY_KEY_LEN];   /* M2 stable card key ("" = keyless, M1) */
    char     side;           /* 'a' or 'u' for C1 */
    uint8_t  n_parts;
    uint8_t  got_mask;       /* bit i-1 set when part i received (max 8 parts in mask;
                                for >8 parts we still append in order if sequential) */
    uint8_t  got_count;
    uint16_t len;
    uint32_t started_ms;
    char     buf[MESH_REASM_MAX];
};

static MeshReasm g_reasm[MESH_REASM_SLOTS];

static void mesh_reasm_gc() {
    uint32_t now = millis();
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (g_reasm[i].used && (now - g_reasm[i].started_ms) > MESH_REASM_TTL_MS)
            g_reasm[i].used = false;
    }
}

static MeshReasm *mesh_reasm_get(char kind, const char *mid,
                                 const char *agent = NULL) {
    mesh_reasm_gc();
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (g_reasm[i].used && g_reasm[i].kind == kind &&
            strcmp(g_reasm[i].mid, mid) == 0 &&
            (kind != 'C' || strcmp(g_reasm[i].agent, agent ? agent : "") == 0))
            return &g_reasm[i];
    }
    for (int i = 0; i < MESH_REASM_SLOTS; i++) {
        if (!g_reasm[i].used) {
            memset(&g_reasm[i], 0, sizeof(g_reasm[i]));
            g_reasm[i].used = true;
            g_reasm[i].kind = kind;
            snprintf(g_reasm[i].mid, sizeof(g_reasm[i].mid), "%s", mid);
            if (kind == 'C' && agent)
                snprintf(g_reasm[i].agent, sizeof(g_reasm[i].agent), "%s", agent);
            g_reasm[i].started_ms = millis();
            return &g_reasm[i];
        }
    }
    /* steal oldest */
    int victim = 0;
    for (int i = 1; i < MESH_REASM_SLOTS; i++)
        if (g_reasm[i].started_ms < g_reasm[victim].started_ms) victim = i;
    memset(&g_reasm[victim], 0, sizeof(g_reasm[victim]));
    g_reasm[victim].used = true;
    g_reasm[victim].kind = kind;
    snprintf(g_reasm[victim].mid, sizeof(g_reasm[victim].mid), "%s", mid);
    if (kind == 'C' && agent)
        snprintf(g_reasm[victim].agent, sizeof(g_reasm[victim].agent), "%s", agent);
    g_reasm[victim].started_ms = millis();
    return &g_reasm[victim];
}

static void mesh_reasm_append(MeshReasm *r, uint8_t part_i, uint8_t n_parts,
                              const char *chunk) {
    if (!r || !chunk) return;
    if (n_parts == 0 || n_parts > 64 || part_i == 0 || part_i > n_parts) return;
    if (r->n_parts == 0) r->n_parts = n_parts;
    /* sequential append: only accept next expected part (got_count+1) to keep
     * order without a per-part buffer — gateway sends in order with gaps. */
    if (part_i != (uint8_t)(r->got_count + 1)) {
        if (part_i <= r->got_count) return;  // dup
        /* gap: still append if we can (best effort) */
    }
    size_t cl = strlen(chunk);
    size_t room = (size_t)(MESH_REASM_MAX - 1 - r->len);
    if (cl > room) cl = room;
    if (cl > 0) {
        memcpy(r->buf + r->len, chunk, cl);
        r->len = (uint16_t)(r->len + cl);
        r->buf[r->len] = '\0';
    }
    if (part_i > r->got_count) r->got_count = part_i;
}

/* ---- L1: fragmented LXMF opportunistic wire over MeshCore ------------------
 * The gateway splits an LXMF opportunistic wire (source+sig+msgpack, raw binary)
 * into base64url chunks and sends them as text frames "L1|mid|i|n|<b64>" (see
 * l1_frame.h for the format the fragmenter must match). The PARSE and the byte
 * accumulation are the pure, host-tested l1_parse()/l1_reasm_feed(); this slot
 * ring is only the TTL/lifecycle wrapper around L1Reasm, mirroring g_reasm above
 * (millis() is why it is here and not in the pure header). On completion the whole
 * LXMF wire goes to the ONE shared router, lxmf_ingest_wire() in rns.cpp — the
 * same door the Reticulum poll uses. Separate slots (not the M1/C1 g_reasm) keep
 * the byte-exact, gap-intolerant L1 policy off the text reassembler's best-effort
 * path and leave the landed M1/C1 code untouched. */
#define L1_SLOTS 2
struct L1Slot {
    bool     used;
    char     mid[L1_MID_CAP];
    uint32_t started_ms;
    L1Reasm  r;
};
static L1Slot g_l1[L1_SLOTS];

/* Disjoint MeshCore-LXMF counters (mesh status), mirroring rns.cpp's data_lxmf_*
 * but never touching them: rx = wires reassembled AND accepted by the router,
 * drop = a frame or reassembly rejected (bad parse/decode, forward gap, overflow,
 * unplaceable wire). */
static uint32_t g_mesh_lxmf_rx = 0;
static uint32_t g_mesh_lxmf_drop = 0;

static void l1_slot_gc() {
    uint32_t now = millis();
    for (int i = 0; i < L1_SLOTS; i++)
        if (g_l1[i].used && (now - g_l1[i].started_ms) > MESH_REASM_TTL_MS)
            g_l1[i].used = false;
}

/* Find or open the slot for `mid`; steals the oldest when full. */
static L1Slot *l1_slot_get(const char *mid) {
    l1_slot_gc();
    for (int i = 0; i < L1_SLOTS; i++)
        if (g_l1[i].used && strcmp(g_l1[i].mid, mid) == 0) return &g_l1[i];
    for (int i = 0; i < L1_SLOTS; i++) {
        if (!g_l1[i].used) {
            memset(&g_l1[i], 0, sizeof(g_l1[i]));
            g_l1[i].used = true;
            snprintf(g_l1[i].mid, sizeof(g_l1[i].mid), "%s", mid);
            g_l1[i].started_ms = millis();
            return &g_l1[i];
        }
    }
    int victim = 0;
    for (int i = 1; i < L1_SLOTS; i++)
        if (g_l1[i].started_ms < g_l1[victim].started_ms) victim = i;
    memset(&g_l1[victim], 0, sizeof(g_l1[victim]));
    g_l1[victim].used = true;
    snprintf(g_l1[victim].mid, sizeof(g_l1[victim].mid), "%s", mid);
    g_l1[victim].started_ms = millis();
    return &g_l1[victim];
}

/* Called when MeshCore stack delivers a private text.
 * Also used by POST /mesh/inject for dry-run of the notify path. */
static uint32_t mesh_on_private_text(const uint8_t *from_pubkey,
                                    const char *from_name,
                                    const char *text) {
    if (!text || !text[0]) return 0;
    uint32_t id = 0;

    if (strncmp(text, "P1|", 3) == 0 || strncmp(text, "P2|", 3) == 0) {
        /* P1 legacy (key by heuristic) / P2 explicit trailing key — one parser. */
        id = notify_ingest_p1(text);
        /* Admitted and keyed: stage a delivery ACK back to the gateway (C2). The
         * pure parse is re-run only to recover the key — cheap, no alloc — so the
         * card admission path in notify.cpp is left untouched. */
        if (id) {
            NotifyPFrame pf;
            if (notify_wire_parse_p(text, &pf) && pf.key[0])
                mesh_card_ack_stage(pf.key);
        }
    } else if (strncmp(text, "M1|", 3) == 0 || strncmp(text, "M2|", 3) == 0) {
        /* M1|mid|i|n|level|source|title|chunk         (legacy, keyless)
         * M2|mid|i|n|level|source|title|chunk|key      (explicit stable card key)
         * The per-frame FIELD parse is the pure, host-tested notify_wire_parse_m();
         * only the TTL/slot reassembly lives here. */
        NotifyMFrame f;
        if (!notify_wire_parse_m(text, &f)) return 0;

        /* Copy the chunk NUL-terminated for the byte reassembler; a single frame's
         * chunk is one radio packet (< NOTIFY_BODY_LEN), reassembly bounds again. */
        char chunk[NOTIFY_BODY_LEN];
        size_t cl = f.chunk_len;
        if (cl > sizeof(chunk) - 1) cl = sizeof(chunk) - 1;
        memcpy(chunk, f.chunk, cl);
        chunk[cl] = '\0';

        MeshReasm *r = mesh_reasm_get('M', f.mid);
        if (r->got_count == 0) {
            snprintf(r->level, sizeof(r->level), "%s", f.level);
            snprintf(r->source, sizeof(r->source), "%s", f.source);
            snprintf(r->title, sizeof(r->title), "%s", f.title);
            /* The gateway repeats the same key on every M2 fragment; keep the one
             * from the first fragment we see. M1 leaves it empty (keyless). */
            snprintf(r->key, sizeof(r->key), "%s", f.key);
        }
        mesh_reasm_append(r, (uint8_t)f.part, (uint8_t)f.n_parts, chunk);
        if (r->got_count >= r->n_parts && r->n_parts > 0) {
            id = notify_ingest(r->level, r->source, r->title, r->buf,
                               r->key[0] ? r->key : NULL);
            /* Admitted and keyed (M2): stage the delivery ACK (C2). M1 is keyless
             * and card_ack_decide returns NONE for it anyway. */
            if (id && r->key[0]) mesh_card_ack_stage(r->key);
            r->used = false;
        }
    } else if (strncmp(text, "C1|", 3) == 0) {
        /* C1|agent|mid|i|n|side|chunk */
        char agent[16] = {0}, mid[8] = {0};
        int part_i = 0, n_parts = 0;
        char side = 'a';
        const char *p = text + 3;
        const char *a = strchr(p, '|');
        if (!a) return 0;
        size_t n = (size_t)(a - p);
        if (n >= sizeof(agent)) n = sizeof(agent) - 1;
        memcpy(agent, p, n);
        p = a + 1;
        a = strchr(p, '|');
        if (!a) return 0;
        n = (size_t)(a - p);
        if (n >= sizeof(mid)) n = sizeof(mid) - 1;
        memcpy(mid, p, n);
        p = a + 1;
        part_i = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        n_parts = atoi(p);
        a = strchr(p, '|');
        if (!a) return 0;
        p = a + 1;
        if (*p) side = *p;
        a = strchr(p, '|');
        if (!a) return 0;
        const char *chunk = a + 1;

        char delivery_key[NW_KEY_CAP];
        if (side == 'u')
            snprintf(delivery_key, sizeof(delivery_key), "%s", mid);
        else
            snprintf(delivery_key, sizeof(delivery_key), "%s:%s", agent, mid);
        if (agents_inbound_key_duplicate(delivery_key)) {
            /* A local user line was already persisted before its uplink, or
             * this completed agent delivery was replayed. It is an ACK/route
             * duplicate, not a second history line. */
            return MESH_RX_HANDLED;
        }

        MeshReasm *r = mesh_reasm_get('C', mid, agent);
        if (r->got_count == 0) {
            snprintf(r->agent, sizeof(r->agent), "%s", agent);
            r->side = side;
        }
        mesh_reasm_append(r, (uint8_t)part_i, (uint8_t)n_parts, chunk);
        if (r->got_count >= r->n_parts && r->n_parts > 0) {
            /* AGENT CONVERSATIONS ONLY. This frame names its target by id and
             * carries a side marker, so `side=u` lands a line attributed to the
             * USER. That was safe while ids were two compiled-in names; peers
             * are minted with ids anyone in radio range can observe, and
             * without this check a stranger could forge words into a peer's
             * thread as if the user had typed them. The agent wire is the only
             * one this frame belongs to. */
            int idx = agents_find(r->agent);
            if (idx >= 0 && agents_transport(idx) != CONV_AGENT) idx = -1;
            if (idx >= 0) {
                bool from_me = (r->side == 'u');
                if (agents_push_line(idx, from_me, r->buf)) {
                    agents_inbound_key_commit(delivery_key);
                    /* Genuine mesh chat arrival: same wake as the WiFi door path.
                     * Set before display_force (loop consumes them together). */
                    g_agents_real_inbound = true;
                    display_force = true;
                    id = 1;  /* non-zero = handled */
                } else {
                    /* Preserve the message as a retryable visible card when the
                     * conversation store cannot durably append it. */
                    id = notify_ingest("info", r->agent, "CHAT", r->buf,
                                       delivery_key);
                    if (id) agents_inbound_key_commit(delivery_key);
                }
            } else {
                /* unknown agent → notify card */
                id = notify_ingest("info", r->agent, "CHAT", r->buf,
                                   delivery_key);
                if (id) agents_inbound_key_commit(delivery_key);
            }
            r->used = false;
        }
    } else if (strncmp(text, "L1|", 3) == 0) {
        /* L1|mid|i|n|<base64url-chunk> — a fragment of an LXMF opportunistic wire.
         * Parse + decode is pure (l1_frame.h); accumulate the bytes in this mid's
         * slot; on the last part hand the whole wire to the shared LXMF router. */
        L1Frame f;
        if (!l1_parse(text, &f)) { g_mesh_lxmf_drop++; return 0; }
        L1Slot *s = l1_slot_get(f.mid);
        int rc = l1_reasm_feed(&s->r, &f);
        if (rc < 0) {                     /* inconsistent n / gap / overflow */
            s->used = false;
            g_mesh_lxmf_drop++;
            return 0;
        }
        if (rc == 1) {                    /* complete: the whole LXMF wire is in s->r.buf */
            bool ok = lxmf_ingest_wire(s->r.buf, s->r.len);
            s->used = false;
            if (ok) { g_mesh_lxmf_rx++; id = 1; }   /* handled: bumps dm_rx below */
            else    { g_mesh_lxmf_drop++; return 0; }
        }
        /* rc == 0: awaiting more parts (or a duplicate) — id stays 0, no dm_rx. */
    } else {
        /* A PLAIN PRIVATE DM IS A MESSAGE FROM A PERSON, so it lands in a
         * conversation rather than as a one-off card. Everything above this is
         * a machine format (P1/M1 cards, C1 agent chat, L1 LXMF) and is
         * untouched.
         *
         * KNOWN CONTACTS ONLY, and this is the whole policy. A contact exists
         * because the mesh cryptographically attributed an advert to that
         * public key, so "known" is a real check and not a name the sender
         * chose. A stranger's DM keeps the OLD behaviour — a card — because the
         * inbox is the user's own list of correspondents and this path runs on
         * the loop task with no rate limit: opening it to anyone in radio range
         * is a separate decision with its own throttling to design. Either way
         * the message is shown: a known peer's lands in a conversation, a
         * stranger's becomes a card. The ONE exception is the off-loop drain
         * finding no free slot (every slot seeded or on screen) — the message is
         * dropped with a log line, because by then this branch has already been
         * taken and there is no card to fall back to. Unreachable as the table
         * stands, with six evictable slots against two seeded ones.
         *
         * The HTTP inject route passes no key (there is no peer behind it), so
         * it takes the card path too — it cannot conjure a conversation. */
        if (from_pubkey && mesh_client_knows_peer(from_pubkey) &&
            inbox_deliver_msg_mesh(from_pubkey, MESH_PUB_LEN, from_name, text)) {
            id = MESH_RX_HANDLED;
        } else {
            /* Through the seam, not around it: this is the card door every
             * transport is supposed to raise a card by. */
            id = inbox_deliver_card(CONV_MESH, NULL, INBOX_SEV_INFO, "mesh",
                                    "MESH", text);
        }
    }

    if (id) {
        g_mesh.dm_rx_count++;
        g_mesh.last_dm_ms = millis();
        mesh_link_mark_ok(0);
    }
    return id;
}

static void mesh_status_json(JsonDocument &doc) {
    doc["ok"] = true;
    doc["paired"] = g_mesh.has_identity;
    doc["has_meta"] = g_mesh.has_meta;
    doc["name"] = g_mesh.name;
    doc["public_key"] = g_mesh.public_key_hex;
    if (g_mesh.has_identity && g_mesh.public_key_hex[0]) {
        char pk8[9];
        memcpy(pk8, g_mesh.public_key_hex, 8);
        pk8[8] = '\0';
        doc["public_key8"] = pk8;
    } else {
        doc["public_key8"] = "";
    }
    if (g_mesh.heltec_pk_hex[0]) {
        char h8[9];
        memcpy(h8, g_mesh.heltec_pk_hex, 8);
        h8[8] = '\0';
        doc["heltec_pk8"] = h8;
    } else {
        doc["heltec_pk8"] = "";
    }
    doc["freq"] = g_mesh.freq;
    doc["sf"] = g_mesh.sf;
    doc["bw"] = g_mesh.bw;
    doc["radio"] = g_mesh.radio_state;
    doc["radio_ready"] = g_mesh.radio_ready;
    doc["dm_rx"] = g_mesh.dm_rx_count;
    /* MeshCore-LXMF receive rung: LXMF wires reassembled off the radio (rx) and
     * frames/reassemblies rejected (drop). Disjoint from rns.cpp's data_lxmf_*. */
    doc["data_lxmf_mesh_rx"] = (unsigned long)g_mesh_lxmf_rx;
    doc["data_lxmf_mesh_drop"] = (unsigned long)g_mesh_lxmf_drop;
    doc["policy"] = "private_dm_only";
    doc["link"] = mesh_ui_state();
    doc["probe_interval_s"] = g_mesh.probe_interval_s;
    doc["probe_ok"] = g_mesh.probe_ok_count;
    doc["probe_fail"] = g_mesh.probe_fail_count;
    doc["last_rtt_ms"] = g_mesh.last_rtt_ms;
    doc["fail_streak"] = g_mesh.fail_streak;
    if (g_mesh.last_ok_ms)
        doc["last_ok_age_s"] = (millis() - g_mesh.last_ok_ms) / 1000UL;
    else
        doc["last_ok_age_s"] = -1;
    doc["keepalive"] = MESH_KEEPALIVE_TEXT;
    doc["note"] = g_mesh.has_identity
                      ? (g_mesh.radio_ready
                             ? "sparse private MC|k probe + P1 notify"
                             : "keys ready; sparse probe waits for radio stack")
                      : "need /mesh_identity.id on SPIFFS";
}

static void skill_meshcore_poll() {
    /* Deferred radio start (~5s after boot): seed UI/WiFi already running.
     * want_identity brings the radio up even with no keys yet, so mc_client.cpp
     * can mint an identity from radio noise on first boot / after a wipe. */
    if ((g_mesh.has_identity || g_mesh.want_identity) && !g_mesh.radio_ready &&
        strcmp(g_mesh.radio_state, "radio_fail") != 0 &&
        millis() > 5000UL) {
        static bool tried = false;
        if (!tried) {
            tried = true;
            if (mesh_client_begin()) {
                g_mesh.radio_ready = true;
                /* A mint may have just happened inside begin(): re-read the now
                 * present NVS identity so status (public key, name) is populated
                 * and has_identity gates the keepalive below. */
                if (!g_mesh.has_identity) mesh_load_identity();
                g_mesh.want_identity = false;
                snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "rx_on");
                event_add("mesh radio RX/TX up");
            } else {
                snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "radio_fail");
                event_add("mesh radio init FAILED (UI stays up)");
            }
        }
    }

    /* MeshCore stack — same loop task as display (shared SPI). */
    if (g_mesh.radio_ready) mesh_client_loop();
    mesh_chat_tx_poll();
    mesh_peer_tx_poll();
    mesh_card_ack_poll();   /* emit any staged A1|key delivery acks (C2) */

    /* Sparse private keepalive — only when radio stack can TX. */
    if (!g_mesh.has_identity || !g_mesh.radio_ready) return;
    if (!g_mesh.heltec_pk_hex[0]) return;
    if (g_mesh_chat_tx.active) return;

    unsigned long now = millis();
    /* A keepalive whose ACK never returns must not wedge the TX path forever.
     * The chat path cancels a stale probe after 12s (see mesh_chat_tx_poll);
     * do the same for the standalone keepalive so it recovers on its own
     * instead of blocking every future probe — and the menu PING's mesh
     * column, which shares this ack-pending gate. */
    if (mesh_client_ack_pending()) {
        if (now - mesh_client_last_send_ms() < 12000UL) return;
        mesh_client_cancel_pending_ack();
        if (mesh_probe_awaiting_ack) {
            mesh_probe_awaiting_ack = false;
            g_mesh.fail_streak++;
            g_mesh.probe_fail_count++;
        }
    }

    unsigned long interval_ms = (unsigned long)g_mesh.probe_interval_s * 1000UL;
    if (g_mesh.last_probe_ms != 0 &&
        (now - g_mesh.last_probe_ms) < interval_ms) {
        return;
    }
    /* First probe ~20s after boot (radio settle + Heltec hear advert). */
    if (g_mesh.last_probe_ms == 0 && now < 20000UL) return;

    g_mesh.last_probe_ms = now;
    uint32_t rtt = 0;
    bool sent = mesh_probe_gateway(&rtt);
    if (!sent) {
        g_mesh.fail_streak++;
        g_mesh.probe_fail_count++;
        if (g_mesh.fail_streak == MESH_FAIL_DOWN)
            event_add("mesh probe DOWN (fail x%u)", (unsigned)g_mesh.fail_streak);
    } else {
        event_add("mesh probe TX MC|k");
        /* Success counted when ACK arrives (mesh_cb_ack). Timeout = fail. */
    }
    /* If previous probe never ACKed, count as fail once next probe fires. */
    static bool had_pending = false;
    if (had_pending && mesh_probe_awaiting_ack) {
        mesh_probe_awaiting_ack = false;
        g_mesh.fail_streak++;
        g_mesh.probe_fail_count++;
    }
    had_pending = sent;
}

static const SkillEndpoint meshcore_endpoints[] = {
    {"GET",  "/mesh/status", "MeshCore pair + sparse link health"},
    {"POST", "/mesh/inject", "Dev: inject P1|… as if private DM arrived"},
    {NULL, NULL, NULL}
};

static const char *meshcore_describe() {
    return "## Skill: meshcore\n\n"
           "Private MeshCore link to home Heltec gateway.\n"
           "Notify: `P1|level|source|title|body[|key]` (P2 makes the key explicit;\n"
           "M1/M2 the multipart forms) → same cards as WiFi `/notify`.\n"
           "Reply: card reply goes up as `R1|key|reply` when WiFi is down.\n"
           "Clock `M`: sparse private keepalive `MC|k` (default every 15 min),\n"
           "MeshCore ACK via stored path/repeaters — never Public flood.\n"
           "SPIFFS: `/mesh_identity.id`, `/mesh_pair.json`, `/mesh_probe_s.txt`.\n";
}

static void meshcore_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/mesh/status"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        mesh_status_json(doc);
        notify_send_json(req, 200, doc);
    });

    server.on(AsyncURIMatcher::exact("/mesh/inject"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body || !body[0]) {
            free(body);
            notify_send_error(req, 400, "body required (P1|… or JSON text)");
            return;
        }
        char wire_buf[320];
        wire_buf[0] = '\0';
        if (body[0] == '{') {
            JsonDocument doc;
            if (deserializeJson(doc, body) != DeserializationError::Ok) {
                free(body);
                notify_send_error(req, 400, "bad json");
                return;
            }
            const char *t = nullptr;
            if (doc["text"].is<const char *>()) t = doc["text"].as<const char *>();
            else if (doc["body"].is<const char *>()) t = doc["body"].as<const char *>();
            free(body);
            if (!t || !t[0]) {
                notify_send_error(req, 400, "need text or body field");
                return;
            }
            snprintf(wire_buf, sizeof(wire_buf), "%s", t);
        } else {
            snprintf(wire_buf, sizeof(wire_buf), "%s", body);
            free(body);
        }
        /* No peer behind an HTTP inject: there is no key to attribute it to,
         * so it cannot open or feed a conversation and takes the card path. */
        uint32_t id = mesh_on_private_text(nullptr, nullptr, wire_buf);
        JsonDocument out;
        out["ok"] = id != 0;
        out["id"] = id;
        out["dm_rx"] = g_mesh.dm_rx_count;
        notify_send_json(req, id ? 200 : 400, out);
    }, NULL, handle_body_collect);
}

static const Skill meshcore_skill = {
    .name = "meshcore",
    .version = "0.1.0",
    .describe = meshcore_describe,
    .endpoints = meshcore_endpoints,
    .register_routes = meshcore_register_routes,
    .tick = skill_meshcore_poll
};

/* Encode user chat as C1 frames and TX private DM to Heltec (same wire as downlink). */
static bool mesh_chat_uplink(const char *agent_id, const char *text,
                             const char *delivery_key) {
    if (!agent_id || !text || !text[0]) return false;
    if (!g_mesh.radio_ready || !mesh_client_ready()) return false;
    if (g_mesh_chat_tx.active) return false;

    char agent[13];
    snprintf(agent, sizeof(agent), "%.12s", agent_id);
    char mid[8];
    if (!agents_delivery_key_valid(delivery_key) || strlen(delivery_key) >= sizeof(mid))
        return false;
    snprintf(mid, sizeof(mid), "%s", delivery_key);

    const uint8_t *raw = (const uint8_t *)text;
    size_t raw_len = strlen(text);
    /* Per-frame budget ~150 bytes total (MeshCore MTU). */
    const int limit = 150;
    char hdr_sample[48];
    snprintf(hdr_sample, sizeof(hdr_sample), "C1|%s|%s|99|99|u|", agent, mid);
    int room = limit - (int)strlen(hdr_sample);
    if (room < 16) room = 16;

    /* Count parts */
    int n_parts = 0;
    size_t i = 0;
    while (i < raw_len) {
        size_t take = mesh_utf8_chunk_len(raw, raw_len, i, (size_t)room);
        if (take == 0) return false;
        n_parts++;
        i += take;
        if (n_parts > MESH_CHAT_TX_MAX_PARTS) return false;
    }
    if (n_parts == 0) n_parts = 1;

    i = 0;
    int part = 0;
    while (i < raw_len && part < n_parts) {
        size_t take = mesh_utf8_chunk_len(raw, raw_len, i, (size_t)room);
        if (take == 0) return false;
        part++;
        char *frame = g_mesh_chat_tx.frames[part - 1];
        int n = snprintf(frame, sizeof(g_mesh_chat_tx.frames[0]),
                         "C1|%s|%s|%d|%d|u|", agent, mid, part, n_parts);
        if (n < 0 || n >= (int)sizeof(g_mesh_chat_tx.frames[0])) return false;
        size_t room2 = sizeof(g_mesh_chat_tx.frames[0]) - 1U - (size_t)n;
        if (take > room2) return false;
        memcpy(frame + n, text + i, take);
        frame[n + take] = '\0';
        i += take;
    }
    if (part <= 0 || part != n_parts || i != raw_len) {
        memset(&g_mesh_chat_tx, 0, sizeof(g_mesh_chat_tx));
        return false;
    }
    g_mesh_chat_tx.count = (uint8_t)n_parts;
    snprintf(g_mesh_chat_tx.agent, sizeof(g_mesh_chat_tx.agent), "%s", agent);
    g_mesh_chat_tx.active = true;
    event_add("mesh chat queued %u parts", (unsigned)g_mesh_chat_tx.count);
    return true;
}

static void skill_meshcore_init() {
    mesh_pair_clear();
    mesh_load_identity();
    mesh_load_meta();
    mesh_load_probe_interval();
    g_mesh.radio_ready = false;

    mesh_client_set_callbacks(mesh_cb_dm, mesh_cb_ack, mesh_cb_log);
    agents_set_mesh_uplink(mesh_chat_uplink);
    agents_set_mesh_peer_send(mesh_peer_tx_submit);

    if (g_mesh.has_identity) {
        /* Defer radio bring-up to poll() so WiFi/HTTP always come up first.
         * A hard radio fail must not brick the seed UI after OTA. */
        snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "radio_deferred");
        event_add("mesh pair %s probe %lus (radio deferred)",
                  g_mesh.name, (unsigned long)g_mesh.probe_interval_s);
    } else {
        /* No identity anywhere (fresh board, or one whose SPIFFS was wiped before
         * the NVS store existed). RNS already self-generates; mesh now does too:
         * bring the radio up so mc_client.cpp can mint a keypair from radio noise
         * and persist it to NVS. */
        g_mesh.want_identity = true;
        snprintf(g_mesh.radio_state, sizeof(g_mesh.radio_state), "radio_deferred");
        event_add("mesh: no identity, will mint on radio bring-up");
    }
    skill_register(&meshcore_skill);
    Serial.printf("[meshcore] paired=%d name=%s probe=%lus radio=%s\n",
                  (int)g_mesh.has_identity, g_mesh.name,
                  (unsigned long)g_mesh.probe_interval_s, g_mesh.radio_state);
}
