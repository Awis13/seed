#pragma once
/*
 * agent_transport.h — pure "which rung does this agent-send take?" decision
 * (host-testable). The consumer half of HERMES-LADDER C1.
 *
 * WHY THIS EXISTS. transport_send_agent() (skills/agents.cpp) has three ways to
 * put an outbound card on the wire — the WiFi/gateway bridge, a Reticulum/LXMF
 * envelope, and a MeshCore DM. Until C2 it chose the WiFi rung on the driver
 * LINK state (WiFi.status()==WL_CONNECTED). But a link that is UP is not a route
 * that WORKS: a captive-portal or no-internet WiFi answers the association
 * instantly yet swallows the POST, so a send that trusts link-up stalls the full
 * connect window before it ever falls back. C1 (reachability.h) added the cached
 * verdict that says whether the HOME GATEWAY is actually reachable; this header
 * is its first consumer — it turns that verdict plus "which rungs are usable for
 * THIS send" into the single rung to take.
 *
 * WHAT IS PURE HERE. The SELECTION is a decision, not I/O: given the reachability
 * tri-state and three availability booleans, exactly one rung (or none) is
 * correct. That is easy to get subtly wrong (treating UNKNOWN as good enough for
 * WiFi is exactly the captive-stall bug), so it lives in one pure function proven
 * by value in tools/test_agent_transport.cpp. The device half owns only the
 * sockets: it computes the three availability booleans and the cached verdict,
 * asks agent_pick_transport() which rung to take, and drives that rung's NATIVE
 * send (no transport is tunnelled over another).
 *
 * THE LADDER, HIGHEST RUNG FIRST:
 *   1. WiFi/gateway — taken ONLY when the route home is PROVEN (REACH_UP) AND a
 *      bridge is configured. A merely-up link (UNKNOWN/DOWN) is skipped so the
 *      send never blocks on a captive/no-route POST.
 *   2. Reticulum   — when the WiFi rung is not taken and an RNS uplink is usable
 *      for this agent (a configured peer over a live link).
 *   3. MeshCore    — last resort, when neither of the above is usable.
 * With nothing usable the result is AGENT_RUNG_NONE and the caller reports a
 * no-transport line rather than blocking on a rung that cannot deliver.
 */

#include "reachability.h"

// The rungs of the agent-send ladder, highest (most preferred when usable)
// first. AGENT_RUNG_NONE means no configured transport can carry this send.
enum AgentRung {
    AGENT_RUNG_NONE = 0,   // no transport usable
    AGENT_RUNG_WIFI,       // WiFi/gateway bridge POST
    AGENT_RUNG_RNS,        // Reticulum / LXMF envelope uplink
    AGENT_RUNG_MESHCORE    // MeshCore private DM (last resort)
};

// Pure ladder selection. Choose the single rung to take given the cached
// route-home verdict and which rungs are configured/usable for THIS send. WiFi
// is taken ONLY when the route home is PROVEN (REACH_UP); REACH_UNKNOWN and
// REACH_DOWN both skip it so the send never blocks on a captive/no-route WiFi
// POST. Below WiFi the order is Reticulum then MeshCore. No I/O, no globals:
// same function host and target.
static inline AgentRung agent_pick_transport(ReachStatus reach,
                                             bool wifi_avail,
                                             bool rns_avail,
                                             bool mesh_avail) {
    if (reach == REACH_UP && wifi_avail) return AGENT_RUNG_WIFI;
    if (rns_avail)  return AGENT_RUNG_RNS;
    if (mesh_avail) return AGENT_RUNG_MESHCORE;
    return AGENT_RUNG_NONE;
}
