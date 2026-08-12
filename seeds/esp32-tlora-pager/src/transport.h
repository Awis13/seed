#pragma once
/*
 * transport.h — the transport BACKEND contract (pure, host-testable).
 *
 * WHY THIS EXISTS
 * ---------------
 * A notification card is this device's main feature, and a card has to be
 * deliverable over every wire it owns — the WiFi/WireGuard HTTP path, a
 * MeshCore DM, an LXMF message. Until now each of those grew its own way in:
 * the HTTP route raised a card inline, the agent chat picked its uplink with a
 * hardwired `strcmp(agent_id, "claude")` ladder, and nothing could answer a
 * thread that arrived over a wire the ladder had never heard of.
 *
 * C1 made the record able to hold a thread from any transport (conv_store.h:
 * `transport` + an opaque `reply_addr`). This header is the other half — the
 * seam those records are ACTED on through:
 *
 *   OUTBOUND   transport_send(conv, text)
 *              one reply path, chosen by conv->transport, not by the
 *              conversation's name.
 *   INBOUND    inbox_deliver_card(...)   raises ONE notification card
 *              inbox_deliver_msg(...)    lands a message in a conversation
 *              every transport comes in through these two doors, so a card
 *              looks and behaves the same whichever wire carried it.
 *
 * WHAT IS PURE HERE. The decisions — which backend a conversation routes to,
 * which return address that backend must use, whether a card is well-formed and
 * what it is labelled as, and which of a conversation's routing fields a
 * manifest on a removable card is allowed to set — are plain functions with no
 * Arduino, no FreeRTOS and no FS, so tools/test_transport_*.cpp drive them
 * directly. The firmware entry points declared at the bottom are implemented in
 * skills/agents.cpp (send/msg) and skills/notify.cpp (card).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "conv_store.h"   /* ConvTransport, CONV_REPLY_MAX */

/* The firmware conversation record (skills/agents.cpp). Only ever passed by
 * pointer across this seam, so a forward declaration is enough and this header
 * stays includable before agents.cpp in the single translation unit. */
struct Conversation;

/* ---- outbound: which backend carries a conversation's reply ---------------- */

enum TransportBackend {
    TRANSPORT_BACKEND_NONE = 0,
    TRANSPORT_BACKEND_AGENT,      /* bridge /v1/chat, then the MeshCore C1 gateway DM */
    TRANSPORT_BACKEND_LXMF,       /* lxmf.delivery to the stored source hash        */
    TRANSPORT_BACKEND_MESH_PEER,  /* MeshCore DM straight to a peer's public key    */
};

#define TRANSPORT_OK          0
#define TRANSPORT_NO_ADDRESS  1   /* right backend, unusable return address */
#define TRANSPORT_UNKNOWN     2   /* no backend for this transport value    */

/* Return-address widths the wire fixes. A conversation whose stored address is
 * the wrong width is not merely unusual — it would be sent to whatever those
 * bytes happen to mean, so it is refused. CONV_AGENT has no fixed width: its
 * "address" is the agent id and the bridge keys off the conversation itself. */
#define TRANSPORT_LXMF_ADDR_LEN 16   /* LXMF destination/source hash */
#define TRANSPORT_MESH_ADDR_LEN 32   /* MeshCore public key          */

/* A conversation reduced to what routing actually needs. Lets the planner (and
 * its test) work without the firmware record. */
struct TransportTarget {
    uint8_t        transport;    /* ConvTransport */
    const uint8_t *reply_addr;
    uint8_t        reply_len;
};

/*
 * Pure dispatch planner: conversation -> backend + the address that backend
 * must send to. No side effects.
 *
 * THE ADDRESS IS AN OUTPUT AND THAT IS THE POINT. A mesh conversation routes to
 * the PEER whose public key is stored on it, never to the one gateway the
 * device happens to be paired with — answering a peer by way of the gateway
 * would deliver a private reply to a third party. Handing the address back
 * here, rather than letting each backend reach for a global, is what makes that
 * checkable.
 *
 * Returns TRANSPORT_OK, or a reason with *out_backend left at NONE.
 */
static inline int transport_plan(const TransportTarget *t, uint8_t *out_backend,
                                 const uint8_t **out_addr, uint8_t *out_len) {
    if (out_backend) *out_backend = (uint8_t)TRANSPORT_BACKEND_NONE;
    if (out_addr) *out_addr = NULL;
    if (out_len) *out_len = 0;
    if (!t) return TRANSPORT_UNKNOWN;

    uint8_t backend;
    uint8_t need;
    switch (t->transport) {
        case CONV_AGENT: backend = TRANSPORT_BACKEND_AGENT;     need = 0; break;
        case CONV_LXMF:  backend = TRANSPORT_BACKEND_LXMF;      need = TRANSPORT_LXMF_ADDR_LEN; break;
        case CONV_MESH:  backend = TRANSPORT_BACKEND_MESH_PEER; need = TRANSPORT_MESH_ADDR_LEN; break;
        default:         return TRANSPORT_UNKNOWN;
    }
    if (need) {
        if (!t->reply_addr || t->reply_len != need) return TRANSPORT_NO_ADDRESS;
    }
    if (out_backend) *out_backend = backend;
    if (out_addr) *out_addr = t->reply_addr;
    if (out_len) *out_len = t->reply_len;
    return TRANSPORT_OK;
}

/* ---- inbound: the one card door ------------------------------------------- */

/* Severity, mirroring skills/notify.cpp's enum. notify.cpp static-asserts the
 * two agree, so a card raised from a transport cannot mean a different level
 * from a card raised over HTTP. */
#define INBOX_SEV_INFO 0
#define INBOX_SEV_WARN 1
#define INBOX_SEV_CRIT 2

/* What a transport is called on a card when the caller does not say. The screen
 * groups by source, so an unlabelled mesh card must not read as an HTTP one. */
#define INBOX_SOURCE_AGENT "agent"
#define INBOX_SOURCE_LXMF  "lxmf"
#define INBOX_SOURCE_MESH  "mesh"

struct InboxCardPlan {
    uint8_t     sev;
    const char *source;   /* caller's string, or the transport's default */
};

/*
 * Pure card admission: is this a card at all, what severity is it really, and
 * what does it say it came from.
 *
 * A card with no title is refused rather than shown blank — the title is the
 * only field the notification list renders, so an empty one is an invisible
 * card that still occupies a slot and still evicts a real one. An out-of-range
 * severity is CLAMPED rather than refused: a transport that miscounts must not
 * be able to drop a critical card, and over-reporting an unknown level as
 * critical fails in the direction that gets looked at.
 */
static inline bool inbox_card_plan(uint8_t via, uint8_t sev, const char *source,
                                   const char *title, InboxCardPlan *out) {
    if (!out) return false;
    out->sev = INBOX_SEV_INFO;
    out->source = "";
    if (!title || !title[0]) return false;
    out->sev = (sev > INBOX_SEV_CRIT) ? (uint8_t)INBOX_SEV_CRIT : sev;
    if (source && source[0]) {
        out->source = source;
    } else {
        switch (via) {
            case CONV_LXMF: out->source = INBOX_SOURCE_LXMF;  break;
            case CONV_MESH: out->source = INBOX_SOURCE_MESH;  break;
            default:        out->source = INBOX_SOURCE_AGENT; break;
        }
    }
    return true;
}

/* ---- what a manifest on a removable card may re-point --------------------- */

/*
 * Resolve a conversation's routing fields while loading the manifest.
 *
 * A SEEDED conversation keeps the transport and return address the firmware
 * compiled in, whatever /conversations.txt says. That file lives on a card a
 * user can pull and edit in any text editor, and `transport` is no longer a
 * label — it now CHOOSES THE WIRE A REPLY LEAVES ON. A line claiming the
 * built-in agent conversation is CONV_MESH with some public key would otherwise
 * redirect everything typed into that room to a stranger's radio, silently and
 * on the next boot. Seeded routing is therefore re-derived, never trusted; only
 * the display label is taken from disk, where the worst case is a wrong name.
 *
 * A non-seeded conversation is the opposite case: the manifest is the ONLY
 * record of where it answers, so its stored address is used as-is — and an
 * empty one leaves the current value alone rather than blanking a live route.
 */
static inline void conv_route_resolve(bool seeded,
                                      uint8_t cur_transport,
                                      const uint8_t *cur_addr, uint8_t cur_len,
                                      uint8_t man_transport,
                                      const uint8_t *man_addr, uint8_t man_len,
                                      uint8_t *out_transport,
                                      uint8_t *out_addr, uint8_t *out_len) {
    uint8_t t = cur_transport;
    const uint8_t *a = cur_addr;
    uint8_t n = cur_len;
    if (!seeded) {
        t = man_transport;
        if (man_addr && man_len) { a = man_addr; n = man_len; }
    }
    if (n > CONV_REPLY_MAX) n = CONV_REPLY_MAX;
    if (out_transport) *out_transport = t;
    if (out_len) *out_len = a ? n : 0;
    /* The caller may pass the conversation's own buffer as both the current
     * address and the destination — the seeded case resolves to itself — so a
     * self-copy is skipped rather than handed to memcpy. */
    if (out_addr && a && n && out_addr != a) memcpy(out_addr, a, n);
}

/* ---- firmware entry points ------------------------------------------------ */

/*
 * Send `text` as the user's reply into `conv`, over whatever wire that
 * conversation lives on (skills/agents.cpp). The active room is taken from the
 * conversation itself, so callers never choose a transport by name. Returns
 * true when the message was accepted by a backend; a backend that cannot take
 * it puts one short line in the room rather than failing silently.
 */
bool transport_send(const struct Conversation *conv, const char *text);

/*
 * Raise ONE notification card, whichever transport it arrived on
 * (skills/notify.cpp). This is THE card entry: the HTTP route delivers through
 * it, and the mesh and LXMF receivers join it as they are wired up. Returns the
 * card id, or 0 when the card was refused or the queue is full.
 */
uint32_t inbox_deliver_card(uint8_t via, const char *key, uint8_t sev,
                            const char *source, const char *title,
                            const char *body);

/*
 * Land a message into the conversation `peer_id` names (skills/agents.cpp).
 * Thin on purpose this commit: it resolves an existing conversation and appends
 * the line. Creating a conversation for an unknown peer is the receive half and
 * belongs with the receivers that will need it. Returns false when no
 * conversation matches.
 */
bool inbox_deliver_msg(uint8_t via, const char *peer_id, const char *label,
                       const char *text);
