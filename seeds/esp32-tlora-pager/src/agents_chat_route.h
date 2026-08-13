#pragma once
/*
 * agents_chat_route.h — chat-route seam for the pager <-> Claude session chat.
 *
 * claude_route_incoming() lands an incoming chat reply (already parsed out of
 * the transport envelope "1|<hex>|<session>|<text>" by the transport session,
 * with the sender hex stripped) into the correct on-screen "room" (agent
 * session) of the `claude` agent, selected by name.
 *
 * LOOP-SAFETY CONTRACT (non-negotiable)
 * -------------------------------------
 * The transport session calls this from its RNS pickup on the LOOP task, AFTER
 * rns_stack.loop(). It must therefore do NO synchronous SD write on the caller's
 * task: an SD append would seize the shared FSPI bus that the history write and
 * the panel paint also live on — the exact contention the memory/bus work
 * fought. The implementation (skills/agents.cpp) therefore:
 *   1. resolves the room in RAM (registry op + reason codes, under agents_mux);
 *   2. hands the cleaned message to a dedicated off-loop FreeRTOS drain task
 *      (agents_route_task) that performs the RAM view append AND the JSONL SD
 *      persist (and, for a brand-new room, the session-manifest SD write);
 *   3. returns immediately. No SD I/O ever runs on the caller's stack.
 * The screen updates as soon as the drain task folds the line into the RAM view
 * window and raises display_force (sub-frame; the loop repaints on its next
 * pass) — never gated on the SD write.
 *
 * REASON CODES (via the nullable `reason` out-param, and the pure planner)
 * -----------------------------------------------------------------------
 *   AGENT_ROUTE_OK        0  routed (queued off-loop)
 *   AGENT_ROUTE_OVERFLOW  1  name is new but the session registry is full
 *   AGENT_ROUTE_BADNAME   2  name was non-empty but sanitised to empty
 *
 * "NEWEST ROOM" (empty `session`)
 * ------------------------------
 * An empty/NULL name routes into the claude agent's most-recently-active room,
 * i.e. sessions[active_idx] (updated on every select/create). If no room exists
 * yet a default is created. active_idx is the room the user last opened/created
 * and so the natural target for an unaddressed reply.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* reason codes — also the return value of the pure planner below. */
#define AGENT_ROUTE_OK        0
#define AGENT_ROUTE_OVERFLOW  1
#define AGENT_ROUTE_BADNAME   2

/* Per-session reconcile verdict from the pure roster planner below. */
#define AGENT_ROSTER_UNCHANGED 0   /* incomplete list: leave liveness as it is */
#define AGENT_ROSTER_ALIVE     1   /* listed live -> show the room             */
#define AGENT_ROSTER_DEAD      2   /* complete list, unlisted -> hide the room */

/* Roster payload ceiling: 346 wire bytes (README) + slack + NUL. */
#define AGENT_ROSTER_PAYLOAD_CAP 384

/* Session-name cap mirrors AGENT_SESSION_LEN in skills/agents.cpp (23 usable). */
#define AGENT_ROUTE_NAME_CAP  24

/*
 * Pure name filter: keep [A-Za-z0-9._-], bounded to out_n-1 bytes. Byte-for-byte
 * identical to agents_session_sanitize() in skills/agents.cpp; kept here so the
 * pure planner and its host test need no firmware / FreeRTOS / SD.
 */
static inline void agents_route_sanitize(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    if (!out || out_n == 0) return;
    if (in) {
        for (const char *p = in; *p && j + 1 < out_n; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
                out[j++] = c;
        }
    }
    out[j] = '\0';
}

/*
 * Pure routing planner: raw session name + a RAM snapshot of the claude agent's
 * session registry -> reason code, resolved room name and the empty->newest
 * flag. No side effects, no I/O. The firmware claude_route_incoming() wraps this
 * with the live registry (snapshotted under agents_mux) and the off-loop enqueue;
 * the host test drives it directly.
 *
 *   session      raw (untrusted) name; NULL/empty => newest room
 *   existing     the agent's current session names (n_existing of them)
 *   sessions_max registry cap (AGENT_SESSIONS_MAX)
 *   out_name     resolved sanitised room name ("" when out_newest==1)
 *   out_newest   set to 1 for the empty->newest case, else 0
 *
 * Returns AGENT_ROUTE_OK / _OVERFLOW / _BADNAME.
 */
static inline int agents_route_plan(const char *session,
                                    const char *const *existing, int n_existing,
                                    int sessions_max,
                                    char out_name[AGENT_ROUTE_NAME_CAP],
                                    int *out_newest) {
    if (out_newest) *out_newest = 0;
    if (out_name) out_name[0] = '\0';

    if (!session || !session[0]) {              /* empty input -> newest room */
        if (out_newest) *out_newest = 1;
        return AGENT_ROUTE_OK;
    }

    char name[AGENT_ROUTE_NAME_CAP];
    agents_route_sanitize(session, name, sizeof(name));
    if (!name[0]) return AGENT_ROUTE_BADNAME;   /* every character stripped */

    bool exists = false;
    for (int i = 0; i < n_existing; i++) {
        if (existing && existing[i] && strcmp(existing[i], name) == 0) {
            exists = true;
            break;
        }
    }
    if (!exists && n_existing >= sessions_max)  /* new name, no registry slot */
        return AGENT_ROUTE_OVERFLOW;

    if (out_name) {                             /* hand back the resolved name */
        size_t j = 0;
        for (; name[j] && j + 1 < AGENT_ROUTE_NAME_CAP; j++) out_name[j] = name[j];
        out_name[j] = '\0';
    }
    return AGENT_ROUTE_OK;
}

/* Is `tok` the roster's incomplete marker `+<digits>` (^\+[0-9]+$)? Tested on
 * the RAW token, before any sanitising — '+' is outside the name alphabet, so
 * agents_route_sanitize would erase it and the marker would vanish. */
static inline int agents_roster_is_countmarker(const char *tok) {
    if (!tok || tok[0] != '+' || !tok[1]) return 0;
    for (const char *p = tok + 1; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/*
 * Pure roster reconcile planner: the receive-end of the "live-room roster"
 * control frame (README "Telling the pager which rooms are live"). Mirrors the
 * agents_route_plan pattern so it is host-testable with no FreeRTOS / SD.
 *
 * Field 4 of a "1|<addr>|*|<payload>" frame is a FULL SNAPSHOT (never a delta)
 * of the sessions that can be written to right now: comma-separated labels,
 * each [A-Za-z0-9._-]{1,23}, with an OPTIONAL trailing `+<digits>` element
 * meaning "the list was cut for space".
 *
 *   payload       raw field-4 text; NULL/empty => no live sessions at all
 *   existing      the claude agent's current session names (n_existing of them)
 *   out_action    per-session verdict, one of AGENT_ROSTER_ALIVE / _DEAD /
 *                 _UNCHANGED (array of at least n_existing bytes)
 *   out_incomplete set to 1 iff a `+<digits>` element was present, else 0
 *
 * Rules (all from the README):
 *   - split on ',';
 *   - a final element matching ^\+[0-9]+$ sets incomplete and is NOT a label
 *     (`+3` alone => incomplete with zero listed live);
 *   - each other element is sanitised with the name alphabet and compared to
 *     the session names;
 *   - a session whose name is in the live set  -> ALIVE;
 *   - else, if incomplete                       -> UNCHANGED (never infer dead);
 *   - else (complete list, name unlisted)       -> DEAD;
 *   - empty/NULL payload => live set empty, incomplete=0 (=> everything DEAD).
 */
static inline void agents_roster_reconcile(const char *payload,
                                           const char *const *existing,
                                           int n_existing,
                                           uint8_t *out_action,
                                           int *out_incomplete) {
    int incomplete = 0;
    if (out_action)
        for (int i = 0; i < n_existing; i++) out_action[i] = AGENT_ROSTER_UNCHANGED;

    char buf[AGENT_ROSTER_PAYLOAD_CAP];
    size_t bn = 0;
    if (payload)
        for (; payload[bn] && bn + 1 < sizeof(buf); bn++) buf[bn] = payload[bn];
    buf[bn] = '\0';

    /* Pass 1: tokenise; mark listed-live sessions ALIVE, detect the +N marker. */
    char *s = buf;
    for (;;) {
        char *comma = strchr(s, ',');
        if (comma) *comma = '\0';
        if (agents_roster_is_countmarker(s)) {
            incomplete = 1;               /* not a label */
        } else if (s[0]) {
            char stok[AGENT_ROUTE_NAME_CAP];
            agents_route_sanitize(s, stok, sizeof(stok));
            if (stok[0] && out_action) {
                for (int i = 0; i < n_existing; i++)
                    if (existing && existing[i] && strcmp(existing[i], stok) == 0)
                        out_action[i] = AGENT_ROSTER_ALIVE;
            }
        }
        if (!comma) break;
        s = comma + 1;
    }

    /* Pass 2: sessions still not ALIVE are DEAD on a complete list, else kept. */
    if (out_action)
        for (int i = 0; i < n_existing; i++)
            if (out_action[i] != AGENT_ROSTER_ALIVE)
                out_action[i] = incomplete ? AGENT_ROSTER_UNCHANGED
                                           : AGENT_ROSTER_DEAD;
    if (out_incomplete) *out_incomplete = incomplete;
}

/*
 * Firmware entry point (defined in skills/agents.cpp). Land `text` into the
 * `claude` agent's room named `session` (empty => newest-active room). Loop-safe
 * per the contract above. `reason` may be NULL. Returns true when the reply was
 * queued for the off-loop persist, false on a bad name / registry overflow / a
 * dead-or-full route queue (never a synchronous SD fallback).
 */
bool claude_route_incoming(const char *session, const char *text, int *reason);
