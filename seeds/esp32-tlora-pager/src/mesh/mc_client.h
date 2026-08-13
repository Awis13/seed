// Seed MeshCore client — private DM RX/TX for notify + sparse keepalive.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdbool.h>
#include <stdint.h>

// Returns true if radio + BaseChatMesh started.
bool mesh_client_begin();

// Call from main loop (same core as display — shared SPI).
void mesh_client_loop();

// True after successful begin().
bool mesh_client_ready();

// Send private text to home Heltec (by pubkey hex). Returns false if not ready.
// expected_ack filled for processAck RTT; last_send_ms for RTT calc.
bool mesh_client_send_to_gateway(const char *text, uint32_t *expected_ack,
                                 uint32_t *est_timeout_ms);

// Send private text to an arbitrary PEER by its 32-byte public key — the reply
// path for a conversation that a peer opened. Distinct from the gateway send
// above and deliberately not built on it: a peer's reply must go to that peer,
// and routing it through the gateway would hand a private message to a third
// party.
//
// KNOWN CONTACTS ONLY. The key must already be in the MeshCore contact table
// (put there by an advert, i.e. cryptographically attributed by the mesh);
// there is no lookup-or-create here, because creating a contact from an
// unverified key is exactly how a stranger would get one. An unknown key
// returns false with *why = "unknown peer".
//
// Returns false without sending when the radio is down, the peer is unknown, or
// a private send is already awaiting its ACK (only one may be outstanding, and
// the gateway's multi-part chat owns that slot while it runs). *why is a short
// static literal naming which; it may be NULL.
bool mesh_client_send_to_peer(const uint8_t *pubkey, const char *text,
                              const char **why);

// Is this 32-byte public key a contact we already know (advert-attributed)?
bool mesh_client_knows_peer(const uint8_t *pubkey);

// Contact-table persistence, in the stock MeshCore /contacts3 layout (see
// mesh/contact_record.h). The table used to be rebuilt from adverts on every
// boot, so a stored conversation could not be answered until its peer spoke
// again. load() runs inside mesh_client_begin(), before the client reports
// ready; save() runs on every advert and path update.
void mesh_contacts_save();
int  mesh_contacts_load();

// THE filesystem /contacts3 lives on. Exposed rather than assumed because the
// conversation store reads this same file to decide which peers have been met,
// and it keeps its own history on a DIFFERENT filesystem (SD when a card is
// mounted, SPIFFS otherwise). Two components naming the same path on two
// filesystems is two files: the reader finds nothing, every mesh conversation
// is refused, and the manifest rewrite that follows deletes them. One accessor
// means that cannot drift.
fs::FS &mesh_contacts_fs();

// Exactly one private send may await a MeshCore ACK at a time. Multi-part
// callers use these to serialize frames and cancel a timed-out expectation
// before retrying the same frame.
bool mesh_client_ack_pending();
void mesh_client_cancel_pending_ack();

// Last send millis (for ACK RTT).
uint32_t mesh_client_last_send_ms();

// Injected by skill: callbacks implemented in meshcore.cpp.
// on_dm carries the sender's 32-byte public key as well as its display name:
// the key is the peer's identity (the mesh attributes it), the name is only
// what the peer calls itself and may change or collide.
void mesh_client_set_callbacks(
    void (*on_dm)(const uint8_t *from_pubkey, const char *from_name,
                  const char *text),
    void (*on_ack)(uint32_t rtt_ms),
    void (*on_log)(const char *msg));
