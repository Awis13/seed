/*
 * Host test for the pure chat-route planner in src/agents_chat_route.h.
 *
 * claude_route_incoming() (firmware) wraps the live registry + the off-loop
 * enqueue; the pure resolution logic — sanitise the untrusted name, decide the
 * reason code, resolve the room / newest flag — lives in agents_route_plan()
 * and is exercised here with no firmware / FreeRTOS / SD. Cases:
 *   - a valid existing name resolves to that room
 *   - a valid new name (registry has room) resolves to a new room
 *   - empty / NULL name -> newest-room flag
 *   - a name that sanitises to empty -> reason 2 (bad name)
 *   - a new name with a full registry -> reason 1 (overflow)
 *   - out-param pointers are optional (NULL-safe)
 */
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../src/agents_chat_route.h"

int main() {
    const char *reg2[] = {"pager-abcd", "work"};   /* two rooms, cap 8 */

    /* 1. valid existing name -> that room, OK, not newest */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("work", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(newest == 0);
        assert(strcmp(name, "work") == 0);
    }

    /* 2. valid new name, room available -> OK, resolved to the new name */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("fresh", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(newest == 0);
        assert(strcmp(name, "fresh") == 0);
    }

    /* 3a. empty name -> newest room, OK, name cleared */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 0;
        int r = agents_route_plan("", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(newest == 1);
        assert(name[0] == '\0');
    }

    /* 3b. NULL name -> newest room, OK */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 0;
        int r = agents_route_plan(NULL, reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(newest == 1);
    }

    /* 4. non-empty name that sanitises to empty -> bad name (reason 2) */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("!!!///", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_BADNAME);
        assert(newest == 0);   /* not the newest path */
    }

    /* 5a. new name with a FULL registry -> overflow (reason 1) */
    {
        const char *full[] = {"a","b","c","d","e","f","g","h"};  /* 8 == cap */
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("new", full, 8, 8, name, &newest);
        assert(r == AGENT_ROUTE_OVERFLOW);
    }

    /* 5b. EXISTING name with a full registry -> still OK (no new slot needed) */
    {
        const char *full[] = {"a","b","c","d","e","f","g","h"};
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("c", full, 8, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(strcmp(name, "c") == 0);
    }

    /* 6. mixed valid/invalid chars are filtered, kept chars form the room */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan("a b/c.d", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(strcmp(name, "abc.d") == 0);   /* space and slash stripped */
    }

    /* 7. long name is bounded to the cap (never overruns out_name) */
    {
        char name[AGENT_ROUTE_NAME_CAP];
        int newest = 9;
        int r = agents_route_plan(
            "abcdefghijklmnopqrstuvwxyz0123456789", reg2, 2, 8, name, &newest);
        assert(r == AGENT_ROUTE_OK);
        assert(strlen(name) <= AGENT_ROUTE_NAME_CAP - 1);
    }

    /* 8. out-param pointers are optional (NULL-safe, no crash under ASan) */
    {
        int r = agents_route_plan("work", reg2, 2, 8, NULL, NULL);
        assert(r == AGENT_ROUTE_OK);
        r = agents_route_plan("", reg2, 2, 8, NULL, NULL);
        assert(r == AGENT_ROUTE_OK);
        r = agents_route_plan("!!!", reg2, 2, 8, NULL, NULL);
        assert(r == AGENT_ROUTE_BADNAME);
    }

    /* 9. the sanitiser matches the firmware allow-list [A-Za-z0-9._-] */
    {
        char out[AGENT_ROUTE_NAME_CAP];
        agents_route_sanitize("Aa9._-", out, sizeof(out));
        assert(strcmp(out, "Aa9._-") == 0);
        agents_route_sanitize("x y!z@", out, sizeof(out));
        assert(strcmp(out, "xyz") == 0);
        agents_route_sanitize(NULL, out, sizeof(out));
        assert(out[0] == '\0');
    }

    printf("chat-route planner tests: OK\n");
    return 0;
}
