/*
 * Host tests for the pure transport dispatch in src/transport.h.
 *
 * transport_send() (firmware) owns the radios, the bridge and the room; the
 * decisions it makes first — which backend a conversation routes to, which
 * return address that backend must use, and which of a conversation's routing
 * fields a manifest on a removable card is allowed to set — live in
 * transport_plan() and conv_route_resolve() and are exercised here with no
 * Arduino, no FreeRTOS and no radio.
 *
 * The load-bearing case is CONV_MESH: a peer conversation must resolve to the
 * PEER's public key, never to the one gateway the device is paired with.
 * Answering a peer by way of the gateway would deliver a private reply to a
 * third party, so the planner hands the address back as an output and the test
 * checks the bytes rather than trusting the backend to reach for the right
 * global.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/transport.h"

/* Two distinguishable 32-byte keys: the peer we mean to answer, and the gateway
 * we must not answer through. */
static uint8_t PEER_KEY[TRANSPORT_MESH_ADDR_LEN];
static uint8_t GATEWAY_KEY[TRANSPORT_MESH_ADDR_LEN];
static uint8_t LXMF_HASH[TRANSPORT_LXMF_ADDR_LEN];

static void fixtures(void) {
    for (size_t i = 0; i < sizeof(PEER_KEY); i++)    PEER_KEY[i]    = (uint8_t)(0x10 + i);
    for (size_t i = 0; i < sizeof(GATEWAY_KEY); i++) GATEWAY_KEY[i] = (uint8_t)(0xA0 + i);
    for (size_t i = 0; i < sizeof(LXMF_HASH); i++)   LXMF_HASH[i]   = (uint8_t)(0xF0 - i);
    /* The case only means something if the two keys differ. */
    assert(memcmp(PEER_KEY, GATEWAY_KEY, sizeof(PEER_KEY)) != 0);
}

static int plan_of(uint8_t transport, const uint8_t *addr, uint8_t len,
                   uint8_t *backend, const uint8_t **out_addr, uint8_t *out_len) {
    TransportTarget t;
    t.transport = transport;
    t.reply_addr = addr;
    t.reply_len = len;
    return transport_plan(&t, backend, out_addr, out_len);
}

/* --- 1. each transport reaches its own backend ------------------------------ */

static void test_backend_selection(void) {
    uint8_t backend = 0xFF, len = 0xFF;
    const uint8_t *addr = NULL;

    /* CONV_AGENT: the bridge keys off the conversation itself, so the agent-id
     * bytes are carried but no width is imposed. */
    const uint8_t agent_id[] = {'c', 'l', 'a', 'u', 'd', 'e'};
    assert(plan_of(CONV_AGENT, agent_id, (uint8_t)sizeof(agent_id),
                   &backend, &addr, &len) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_AGENT);
    assert(len == sizeof(agent_id));
    assert(addr && memcmp(addr, agent_id, sizeof(agent_id)) == 0);

    /* CONV_LXMF: the stored 16-byte source hash of whoever opened the thread. */
    assert(plan_of(CONV_LXMF, LXMF_HASH, (uint8_t)sizeof(LXMF_HASH),
                   &backend, &addr, &len) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_LXMF);
    assert(len == TRANSPORT_LXMF_ADDR_LEN);
    assert(addr && memcmp(addr, LXMF_HASH, sizeof(LXMF_HASH)) == 0);
}

/* --- 2. a mesh conversation answers the PEER, not the gateway --------------- */

static void test_mesh_targets_the_peer(void) {
    uint8_t backend = 0xFF, len = 0xFF;
    const uint8_t *addr = NULL;

    assert(plan_of(CONV_MESH, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                   &backend, &addr, &len) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_MESH_PEER);
    assert(len == TRANSPORT_MESH_ADDR_LEN);
    assert(addr != NULL);
    /* The bytes are the peer's. */
    assert(memcmp(addr, PEER_KEY, sizeof(PEER_KEY)) == 0);
    /* ... and emphatically not the gateway's — the whole point of the branch. */
    assert(memcmp(addr, GATEWAY_KEY, sizeof(GATEWAY_KEY)) != 0);
}

/* --- 3. an address of the wrong width is refused, not sent ------------------ */

static void test_address_width_is_enforced(void) {
    uint8_t backend = 0xFF, len = 0xFF;
    const uint8_t *addr = NULL;

    /* A mesh key that is really an LXMF hash, and the reverse: both would be
     * "sent" to whatever those bytes happen to mean on the other wire. */
    assert(plan_of(CONV_MESH, LXMF_HASH, TRANSPORT_LXMF_ADDR_LEN,
                   &backend, &addr, &len) == TRANSPORT_NO_ADDRESS);
    assert(backend == TRANSPORT_BACKEND_NONE);
    assert(addr == NULL && len == 0);

    assert(plan_of(CONV_LXMF, PEER_KEY, TRANSPORT_MESH_ADDR_LEN,
                   &backend, &addr, &len) == TRANSPORT_NO_ADDRESS);
    assert(backend == TRANSPORT_BACKEND_NONE);

    /* No address at all. */
    assert(plan_of(CONV_LXMF, NULL, 0, &backend, &addr, &len) == TRANSPORT_NO_ADDRESS);
    assert(plan_of(CONV_MESH, NULL, 0, &backend, &addr, &len) == TRANSPORT_NO_ADDRESS);
    /* Off-by-one on the width is still the wrong width. */
    assert(plan_of(CONV_LXMF, LXMF_HASH, TRANSPORT_LXMF_ADDR_LEN - 1,
                   &backend, &addr, &len) == TRANSPORT_NO_ADDRESS);

    /* An agent conversation needs no address and must still route. */
    assert(plan_of(CONV_AGENT, NULL, 0, &backend, &addr, &len) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_AGENT);
}

/* --- 4. unknown transports and NULL-safety ---------------------------------- */

static void test_unknown_and_null(void) {
    uint8_t backend = 0xFF, len = 0xFF;
    const uint8_t *addr = NULL;

    /* A value a future build wrote and this one does not know. It must not fall
     * through to whichever backend happens to be first. */
    assert(plan_of(99, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                   &backend, &addr, &len) == TRANSPORT_UNKNOWN);
    assert(backend == TRANSPORT_BACKEND_NONE);
    assert(addr == NULL);

    assert(transport_plan(NULL, &backend, &addr, &len) == TRANSPORT_UNKNOWN);
    /* Every out-param is optional. */
    TransportTarget t;
    t.transport = CONV_MESH;
    t.reply_addr = PEER_KEY;
    t.reply_len = (uint8_t)sizeof(PEER_KEY);
    assert(transport_plan(&t, NULL, NULL, NULL) == TRANSPORT_OK);
}

/* --- 5. a manifest on a removable card cannot re-point a seeded route ------- */

static void test_seeded_transport_immutable(void) {
    const uint8_t agent_id[] = {'c', 'l', 'a', 'u', 'd', 'e'};
    uint8_t out_t = 0xFF;
    uint8_t out_addr[CONV_REPLY_MAX];
    uint8_t out_len = 0xFF;

    /* THE ATTACK: /conversations.txt is a text file on a card anyone can pull.
     * A line claiming the built-in agent conversation is a mesh conversation
     * with a stranger's public key would redirect everything typed in that room
     * to that stranger's radio on the next boot. A seeded conversation must
     * come back CONV_AGENT with its own address, whatever the line says. */
    memset(out_addr, 0, sizeof(out_addr));
    conv_route_resolve(/*seeded=*/true,
                       CONV_AGENT, agent_id, (uint8_t)sizeof(agent_id),
                       CONV_MESH, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                       &out_t, out_addr, &out_len);
    assert(out_t == CONV_AGENT);
    assert(out_len == sizeof(agent_id));
    assert(memcmp(out_addr, agent_id, sizeof(agent_id)) == 0);
    assert(memcmp(out_addr, PEER_KEY, sizeof(agent_id)) != 0);

    /* And the routing that results really is the agent backend, not the mesh
     * one — the property the attack was after, checked end to end. */
    uint8_t backend = 0xFF;
    assert(plan_of(out_t, out_addr, out_len, &backend, NULL, NULL) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_AGENT);
    assert(backend != TRANSPORT_BACKEND_MESH_PEER);

    /* A NON-seeded conversation is the opposite case: the manifest is the only
     * record of where it answers, so it is taken as written. */
    memset(out_addr, 0, sizeof(out_addr));
    conv_route_resolve(/*seeded=*/false,
                       CONV_AGENT, agent_id, (uint8_t)sizeof(agent_id),
                       CONV_MESH, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                       &out_t, out_addr, &out_len);
    assert(out_t == CONV_MESH);
    assert(out_len == TRANSPORT_MESH_ADDR_LEN);
    assert(memcmp(out_addr, PEER_KEY, sizeof(PEER_KEY)) == 0);
    assert(plan_of(out_t, out_addr, out_len, &backend, NULL, NULL) == TRANSPORT_OK);
    assert(backend == TRANSPORT_BACKEND_MESH_PEER);

    /* A manifest line with no address must not blank a live route. */
    memcpy(out_addr, PEER_KEY, sizeof(PEER_KEY));
    conv_route_resolve(false, CONV_MESH, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                       CONV_MESH, NULL, 0, &out_t, out_addr, &out_len);
    assert(out_len == TRANSPORT_MESH_ADDR_LEN);
    assert(memcmp(out_addr, PEER_KEY, sizeof(PEER_KEY)) == 0);

    /* The firmware passes the conversation's own buffer as both source and
     * destination for the seeded case; that must not be a self-copy fault. */
    uint8_t live[CONV_REPLY_MAX];
    memcpy(live, agent_id, sizeof(agent_id));
    uint8_t live_len = (uint8_t)sizeof(agent_id);
    uint8_t live_t = CONV_AGENT;
    conv_route_resolve(true, live_t, live, live_len,
                       CONV_MESH, PEER_KEY, (uint8_t)sizeof(PEER_KEY),
                       &live_t, live, &live_len);
    assert(live_t == CONV_AGENT);
    assert(live_len == sizeof(agent_id));
    assert(memcmp(live, agent_id, sizeof(agent_id)) == 0);

    /* An over-wide manifest address is clamped to what the record can hold
     * rather than overrunning it. */
    uint8_t wide[CONV_REPLY_MAX];
    memset(wide, 0x5A, sizeof(wide));
    conv_route_resolve(false, CONV_AGENT, agent_id, (uint8_t)sizeof(agent_id),
                       CONV_MESH, wide, (uint8_t)(CONV_REPLY_MAX + 8),
                       &out_t, out_addr, &out_len);
    assert(out_len == CONV_REPLY_MAX);
}

int main(void) {
    fixtures();
    test_backend_selection();
    test_mesh_targets_the_peer();
    test_address_width_is_enforced();
    test_unknown_and_null();
    test_seeded_transport_immutable();
    printf("transport dispatch tests: OK\n");
    return 0;
}
