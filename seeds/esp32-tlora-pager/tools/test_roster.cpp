/*
 * Host test for the pure live-room roster reconcile planner in
 * src/agents_chat_route.h (agents_roster_reconcile / agents_roster_is_countmarker).
 *
 * The firmware wraps this with the live registry (under agents_mux) and the
 * off-loop persist; the pure decision — parse field 4 of a "1|<addr>|*|<payload>"
 * control frame and decide, per session, alive / dead / leave-unchanged — lives
 * here and is exercised with no firmware / FreeRTOS / SD. Cases:
 *   - a normal "main,sonata" list marks listed rooms alive, unlisted dead
 *   - an empty payload => every room dead (no live sessions at all)
 *   - a trailing "+N" makes it incomplete: listed alive, unlisted UNCHANGED
 *   - "+3" alone => incomplete with zero listed live
 *   - a room previously dead that reappears in the list => alive (un-mark)
 *   - names with '.', '-', '_' survive the alphabet
 *   - whitespace / garbage is tolerated (sanitised, never crashes)
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../src/agents_chat_route.h"

int main() {
    /* The claude agent's four current rooms. */
    const char *reg[] = {"main", "sonata", "pager-2010", "claude-pager-channel"};
    const int N = 4;

    /* 1. normal snapshot: two listed live, the other two unlisted => dead. */
    {
        uint8_t act[8];
        int incomplete = 9;
        agents_roster_reconcile("main,sonata", reg, N, act, &incomplete);
        assert(incomplete == 0);
        assert(act[0] == AGENT_ROSTER_ALIVE);   /* main   */
        assert(act[1] == AGENT_ROSTER_ALIVE);   /* sonata */
        assert(act[2] == AGENT_ROSTER_DEAD);    /* pager-2010 */
        assert(act[3] == AGENT_ROSTER_DEAD);    /* claude-pager-channel */
    }

    /* 2. empty payload => no live sessions at all: everything dead, complete. */
    {
        uint8_t act[8];
        int incomplete = 9;
        agents_roster_reconcile("", reg, N, act, &incomplete);
        assert(incomplete == 0);
        for (int i = 0; i < N; i++) assert(act[i] == AGENT_ROSTER_DEAD);
    }
    /* 2b. NULL payload is the same legal "nobody live" frame. */
    {
        uint8_t act[8];
        int incomplete = 9;
        agents_roster_reconcile(NULL, reg, N, act, &incomplete);
        assert(incomplete == 0);
        for (int i = 0; i < N; i++) assert(act[i] == AGENT_ROSTER_DEAD);
    }

    /* 3. trailing +N: the list was cut for space. Listed rooms alive; the rest
     *    must NOT be inferred dead — they stay UNCHANGED. */
    {
        uint8_t act[8];
        int incomplete = 0;
        agents_roster_reconcile("main,sonata,+3", reg, N, act, &incomplete);
        assert(incomplete == 1);
        assert(act[0] == AGENT_ROSTER_ALIVE);       /* main   */
        assert(act[1] == AGENT_ROSTER_ALIVE);       /* sonata */
        assert(act[2] == AGENT_ROSTER_UNCHANGED);   /* not inferred dead */
        assert(act[3] == AGENT_ROSTER_UNCHANGED);
    }

    /* 4. "+3" alone: incomplete, zero listed live, everything UNCHANGED. */
    {
        uint8_t act[8];
        int incomplete = 0;
        agents_roster_reconcile("+3", reg, N, act, &incomplete);
        assert(incomplete == 1);
        for (int i = 0; i < N; i++) assert(act[i] == AGENT_ROSTER_UNCHANGED);
    }

    /* 5. a room reappearing in a complete list => ALIVE (the apply un-marks it).
     *    pager-2010 was the classic dead room; here the node says it is live. */
    {
        uint8_t act[8];
        int incomplete = 9;
        agents_roster_reconcile("pager-2010", reg, N, act, &incomplete);
        assert(incomplete == 0);
        assert(act[0] == AGENT_ROSTER_DEAD);    /* main   unlisted */
        assert(act[1] == AGENT_ROSTER_DEAD);    /* sonata unlisted */
        assert(act[2] == AGENT_ROSTER_ALIVE);   /* pager-2010 back */
        assert(act[3] == AGENT_ROSTER_DEAD);
    }

    /* 6. names carrying '.', '-', '_' pass the alphabet and match verbatim. */
    {
        const char *dots[] = {"a.b", "c-d", "e_f", "plain"};
        uint8_t act[8];
        int incomplete = 9;
        agents_roster_reconcile("a.b,c-d,e_f", dots, 4, act, &incomplete);
        assert(incomplete == 0);
        assert(act[0] == AGENT_ROSTER_ALIVE);
        assert(act[1] == AGENT_ROSTER_ALIVE);
        assert(act[2] == AGENT_ROSTER_ALIVE);
        assert(act[3] == AGENT_ROSTER_DEAD);    /* "plain" unlisted */
    }

    /* 7. whitespace / garbage robustness: a label with spaces sanitises to the
     *    bare name; junk tokens and empty tokens are skipped, never crash. A
     *    stray "+" (no digits) is NOT a count marker. */
    {
        const char *r[] = {"main", "sonata"};
        uint8_t act[8];
        int incomplete = 9;
        /* "  main " -> "main"; "!!!" -> ""(skip); ",," empty tokens; "+" not a
         * marker; sonata absent => dead, list is complete. */
        agents_roster_reconcile("  main ,!!!,,+,x@y", r, 2, act, &incomplete);
        assert(incomplete == 0);
        assert(act[0] == AGENT_ROSTER_ALIVE);   /* main matched after sanitise */
        assert(act[1] == AGENT_ROSTER_DEAD);    /* sonata never listed */
    }

    /* 8. out_incomplete NULL-safe; a zero-session agent never touches act[]. */
    {
        agents_roster_reconcile("main", NULL, 0, NULL, NULL);   /* no crash */
        uint8_t act[1] = {0xAA};
        agents_roster_reconcile("main", NULL, 0, act, NULL);
        assert(act[0] == 0xAA);                 /* untouched: n_existing == 0 */
    }

    /* 9. the count-marker predicate is exactly ^\+[0-9]+$. */
    {
        assert(agents_roster_is_countmarker("+3") == 1);
        assert(agents_roster_is_countmarker("+42") == 1);
        assert(agents_roster_is_countmarker("+") == 0);   /* no digits */
        assert(agents_roster_is_countmarker("+3a") == 0); /* trailing junk */
        assert(agents_roster_is_countmarker("3") == 0);   /* no plus */
        assert(agents_roster_is_countmarker("") == 0);
        assert(agents_roster_is_countmarker(NULL) == 0);
    }

    printf("roster reconcile planner tests: OK\n");
    return 0;
}
