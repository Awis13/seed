/*
 * Host tests for card admission in src/transport.h — the pure half of
 * inbox_deliver_card(), the one door every transport raises a notification card
 * through.
 *
 * A card is this device's main feature, so what makes one well-formed cannot
 * differ by wire: the same title rule, the same severity range, and a source
 * label that says which transport carried it. The firmware half (filling the
 * Notification and raising it — wake, ring, sound, event) needs the queue and
 * the panel and is pinned in tools/test_transport_pins.py instead.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/transport.h"

/* --- 1. a card must have a title -------------------------------------------- */

static void test_title_required(void) {
    InboxCardPlan p;
    /* The notification list renders the title. An empty one is an invisible
     * card that still takes a slot and still evicts a real one, so it is
     * refused at the door rather than shown blank. */
    assert(!inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "peer", "", &p));
    assert(!inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "peer", NULL, &p));
    assert(!inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, NULL, NULL, &p));
    /* NULL out-param is refused, not dereferenced. */
    assert(!inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "peer", "hi", NULL));

    /* A body is optional — plenty of cards are a title alone. */
    assert(inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "peer", "hi", &p));
}

/* --- 2. severity is clamped, never dropped ---------------------------------- */

static void test_severity(void) {
    InboxCardPlan p;

    assert(inbox_card_plan(CONV_AGENT, INBOX_SEV_INFO, "s", "t", &p));
    assert(p.sev == INBOX_SEV_INFO);
    assert(inbox_card_plan(CONV_AGENT, INBOX_SEV_WARN, "s", "t", &p));
    assert(p.sev == INBOX_SEV_WARN);
    assert(inbox_card_plan(CONV_AGENT, INBOX_SEV_CRIT, "s", "t", &p));
    assert(p.sev == INBOX_SEV_CRIT);

    /* Out of range CLAMPS UP. A transport that miscounts must not be able to
     * make a critical card vanish, and over-reporting an unknown level as
     * critical fails in the direction that gets looked at. */
    assert(inbox_card_plan(CONV_AGENT, INBOX_SEV_CRIT + 1, "s", "t", &p));
    assert(p.sev == INBOX_SEV_CRIT);
    assert(inbox_card_plan(CONV_AGENT, 255, "s", "t", &p));
    assert(p.sev == INBOX_SEV_CRIT);
}

/* --- 3. an unlabelled card still says which wire carried it ----------------- */

static void test_source_defaulting(void) {
    InboxCardPlan p;

    /* The screen groups by source, so a mesh card with no source of its own
     * must not read as one that came in over HTTP. */
    assert(inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, NULL, "t", &p));
    assert(strcmp(p.source, INBOX_SOURCE_MESH) == 0);
    assert(inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "", "t", &p));
    assert(strcmp(p.source, INBOX_SOURCE_MESH) == 0);

    assert(inbox_card_plan(CONV_LXMF, INBOX_SEV_INFO, "", "t", &p));
    assert(strcmp(p.source, INBOX_SOURCE_LXMF) == 0);

    assert(inbox_card_plan(CONV_AGENT, INBOX_SEV_INFO, "", "t", &p));
    assert(strcmp(p.source, INBOX_SOURCE_AGENT) == 0);

    /* The three defaults must be distinguishable, or grouping by them is a lie. */
    assert(strcmp(INBOX_SOURCE_MESH, INBOX_SOURCE_LXMF) != 0);
    assert(strcmp(INBOX_SOURCE_MESH, INBOX_SOURCE_AGENT) != 0);
    assert(strcmp(INBOX_SOURCE_LXMF, INBOX_SOURCE_AGENT) != 0);

    /* A caller that names a source keeps it — the default is a fallback, not an
     * override, or a real sender's name would be replaced by its wire. */
    assert(inbox_card_plan(CONV_MESH, INBOX_SEV_INFO, "home-rig", "t", &p));
    assert(strcmp(p.source, "home-rig") == 0);
}

int main(void) {
    test_title_required();
    test_severity();
    test_source_defaulting();
    printf("transport card tests: OK\n");
    return 0;
}
