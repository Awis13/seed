/*
 * Host test for the pure chat-vs-notification classifier (TLORA-UI-FIX / C2),
 * src/notify_chat_class.h.
 *
 * Drives the shared resolver and boot/live planners by value. Device wiring is
 * pinned separately by test_feed_view.sh and test_notify_offloop.py.
 *
 * MUTATION CHECKS (apply by hand to src/notify_chat_class.h, must go RED, then
 * restore):
 * Mutation checks: removing source resolution breaks test_source_with_conv;
 * returning a conversation for every card breaks test_genuine_notification;
 * relaxing timestamp/direction checks breaks test_reconcile_planner.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../src/notify_chat_class.h"

/* --- an injected conversation registry -------------------------------------
 * The device answers this from agents_find(); here it is a fixed small table so
 * the decision is pinned by value. "opencode" is present (a minted agent room),
 * "ha" is not (a genuine notification source). */
static const char *g_convs[] = { "claude", "hermes", "opencode" };

static int conv_find(const char *id, void * /*ctx*/) {
    if (!id || !id[0]) return -1;
    for (size_t i = 0; i < sizeof(g_convs) / sizeof(g_convs[0]); i++)
        if (strcmp(id, g_convs[i]) == 0) return (int)i;
    return -1;
}

/* --- 1. the "-chat" door prefix helper ------------------------------------- */

static void test_door_prefix(void) {
    char out[64];
    assert(notify_chat_door_prefix("hermes-chat", out, sizeof(out)) == 6);
    assert(strcmp(out, "hermes") == 0);

    /* no suffix -> 0, out emptied */
    assert(notify_chat_door_prefix("hermes", out, sizeof(out)) == 0);
    assert(out[0] == '\0');

    /* "-chat" alone (no prefix) -> 0 */
    assert(notify_chat_door_prefix("-chat", out, sizeof(out)) == 0);

    /* NULL / empty are safe */
    assert(notify_chat_door_prefix(NULL, out, sizeof(out)) == 0);
    assert(notify_chat_door_prefix("", out, sizeof(out)) == 0);
}

/* --- 2. a legacy "-chat" door key is chat ---------------------------------- */

static void test_chat_door_key(void) {
    NotifyChatResolution r = notify_chat_resolve("", "hermes-chat", conv_find, NULL);
    assert(r.conversation == 1);
    assert(strcmp(r.id, "hermes") == 0);
    r = notify_chat_resolve("", "claude-chat", conv_find, NULL);
    assert(r.conversation == 0);
}

/* --- 3. an agent source with a conversation is chat (the broadening) -------- */

static void test_source_with_conv(void) {
    NotifyChatResolution r = notify_chat_resolve(
        "opencode", "opencode-pager", conv_find, NULL);
    assert(r.conversation == 2 && strcmp(r.id, "opencode") == 0);
    r = notify_chat_resolve("claude", "", conv_find, NULL);
    assert(r.conversation == 0 && strcmp(r.id, "claude") == 0);
    /* Conflicting valid coordinates are ambiguous and stay as a card. */
    r = notify_chat_resolve("claude", "hermes-chat", conv_find, NULL);
    assert(r.conversation < 0 && r.id[0] == '\0');
    /* Matching source and legacy door still resolve normally. */
    r = notify_chat_resolve("hermes", "hermes-chat", conv_find, NULL);
    assert(r.conversation == 1 && strcmp(r.id, "hermes") == 0);
}

/* --- 4. a genuine notification (no conversation, no door) is NOT chat ------- */

static void test_genuine_notification(void) {
    assert(notify_chat_resolve("ha", "ha", conv_find, NULL).conversation < 0);
    assert(notify_chat_resolve("sensor", "", conv_find, NULL).conversation < 0);
    assert(notify_chat_resolve("k1c", "k1c.print", conv_find, NULL).conversation < 0);
}

/* --- 5. empty / unknown is NOT chat ---------------------------------------- */

static void test_empty(void) {
    assert(notify_chat_resolve("", "", conv_find, NULL).conversation < 0);
    assert(notify_chat_resolve(NULL, NULL, conv_find, NULL).conversation < 0);
    assert(notify_chat_resolve("", "ghost-chat", conv_find, NULL).conversation < 0);
    assert(notify_chat_resolve("claude", "claude-chat", NULL, NULL).conversation < 0);
}

static NotifyChatThreadState thread(bool origin_present) {
    NotifyChatThreadState s = {origin_present};
    return s;
}

static void test_reconcile_planner(void) {
    NotifyChatResolution mapped = notify_chat_resolve(
        "hermes", "", conv_find, NULL);
    NotifyChatResolution orphan = notify_chat_resolve(
        "ghost", "ghost-chat", conv_find, NULL);

    NotifyChatThreadState exact = thread(true);
    assert(notify_chat_reconcile_plan(&mapped, "hello", 1800000000u, &exact)
           == NOTIFY_CHAT_ACK_ALREADY_ROUTED);
    NotifyChatThreadState absent = thread(false);
    /* Text and timestamp equality are not identity: a different card routes. */
    assert(notify_chat_reconcile_plan(&mapped, "hello", 1800000000u, &absent)
           == NOTIFY_CHAT_ROUTE_THEN_ACK);
    NotifyChatThreadState empty = thread(false);
    assert(notify_chat_reconcile_plan(&mapped, "first", 0, &empty)
           == NOTIFY_CHAT_ROUTE_THEN_ACK);

    assert(notify_chat_reconcile_plan(&orphan, "hello", 1800000001u, &absent)
           == NOTIFY_CHAT_KEEP_CARD);
    assert(notify_chat_reconcile_plan(&mapped, "", 1800000001u, &absent)
           == NOTIFY_CHAT_KEEP_CARD);
}

static void test_arrival_planner(void) {
    NotifyChatResolution mapped = notify_chat_resolve(
        "hermes", "", conv_find, NULL);
    NotifyChatThreadState exact = thread(true);
    NotifyChatThreadState different = thread(false);
    assert(notify_chat_arrival_plan(&mapped, "hello", &exact)
           == NOTIFY_CHAT_ROUTE_THEN_ACK);
    assert(notify_chat_arrival_plan(&mapped, "hello", &different)
           == NOTIFY_CHAT_ROUTE_THEN_ACK);
    assert(notify_chat_arrival_plan(&mapped, "", &different)
           == NOTIFY_CHAT_KEEP_CARD);
}

static void test_restore_order(void) {
    NotifyChatRestoreOrder newest = {1800000002u, 0};
    NotifyChatRestoreOrder oldest = {1800000001u, 1};
    assert(notify_chat_restore_before(oldest, newest));
    assert(!notify_chat_restore_before(newest, oldest));

    /* Equal and clockless cards preserve inverse archive rank (oldest first). */
    NotifyChatRestoreOrder equal_old = {1800000000u, 4};
    NotifyChatRestoreOrder equal_new = {1800000000u, 2};
    assert(notify_chat_restore_before(equal_old, equal_new));
    NotifyChatRestoreOrder clockless_old = {0, 5};
    NotifyChatRestoreOrder clockless_new = {0, 1};
    assert(notify_chat_restore_before(clockless_old, clockless_new));

    /* Rank remains authoritative for mixed clocks and rollback, producing no
     * comparator cycle: c(oldest) < b < a(newest), regardless of epochs. */
    NotifyChatRestoreOrder a = {1800000300u, 0};
    NotifyChatRestoreOrder b = {0, 1};
    NotifyChatRestoreOrder c = {1799990000u, 2};
    assert(notify_chat_restore_before(c, b));
    assert(notify_chat_restore_before(b, a));
    assert(notify_chat_restore_before(c, a));
    assert(!notify_chat_restore_before(a, c));

    NotifyChatRestoreOrder rollback_old = {1800000200u, 5};
    NotifyChatRestoreOrder rollback_new = {1800000100u, 4};
    assert(notify_chat_restore_before(rollback_old, rollback_new));
}

static void test_append_persistence(void) {
    assert(notify_chat_readback_verified(50, 100, 150, 100, true));
    assert(!notify_chat_readback_verified(50, 100, 149, 100, true));
    assert(!notify_chat_readback_verified(50, 100, 150, 99, true));
    assert(!notify_chat_readback_verified(50, 100, 150, 100, false));
    assert(!notify_chat_readback_verified(50, 0, 50, 0, true));

    assert(!notify_chat_tail_needs_separator(0, false, 0));
    assert(!notify_chat_tail_needs_separator(20, true, '\n'));
    assert(notify_chat_tail_needs_separator(20, true, '}'));
    assert(notify_chat_tail_needs_separator(20, false, 0));
}

static void test_two_card_restore_batch(void) {
    struct Card { uint32_t id; const char *text; NotifyChatRestoreOrder order; };
    Card cards[2] = {
        {102, "OK", {1800000002u, 0}},
        {101, "OK", {1800000001u, 1}},
    };
    if (notify_chat_restore_before(cards[1].order, cards[0].order)) {
        Card tmp = cards[0]; cards[0] = cards[1]; cards[1] = tmp;
    }
    assert(cards[0].id == 101 && cards[1].id == 102);

    NotifyChatResolution mapped = notify_chat_resolve(
        "hermes", "", conv_find, NULL);
    NotifyChatThreadState origin_absent = thread(false);
    int appended = 0;
    uint32_t persisted_ids[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        NotifyChatAction action = notify_chat_reconcile_plan(
            &mapped, cards[i].text, cards[i].order.card_epoch, &origin_absent);
        assert(action == NOTIFY_CHAT_ROUTE_THEN_ACK);
        persisted_ids[appended++] = cards[i].id;
    }
    /* Same text is retained twice, in original chronology, with distinct ids. */
    assert(appended == 2);
    assert(persisted_ids[0] == 101 && persisted_ids[1] == 102);
}

static void test_live_burst_drain(void) {
    NotifyChatResolution mapped = notify_chat_resolve(
        "hermes", "hermes-chat", conv_find, NULL);
    /* Two same-door arrivals collected before one loop drain both ACK in
     * chronological order when persistence succeeds. */
    bool blocked = false;
    uint32_t oldest_first[2] = {201, 202};
    uint32_t acked[2] = {0, 0};
    int ack_n = 0;
    for (int i = 0; i < 2; i++) {
        NotifyChatDrainResult result = notify_chat_drain_result(blocked, true);
        if (result == NOTIFY_CHAT_DRAIN_ACK_ROUTED) acked[ack_n++] = oldest_first[i];
    }
    assert(ack_n == 2 && acked[0] == 201 && acked[1] == 202);

    /* A persistence failure keeps the first door unread and prevents a newer
     * same-conversation message from overtaking it during this drain. */
    NotifyChatDrainResult first = notify_chat_drain_result(false, false);
    assert(first == NOTIFY_CHAT_DRAIN_KEEP_FAILED);
    blocked = true;
    assert(notify_chat_drain_result(blocked, true) == NOTIFY_CHAT_DRAIN_BLOCKED);

    /* Boot uses the same barrier. A fails, so B is neither attempted nor ACKed;
     * the retry starts again at A and then preserves A,B chronology exactly. */
    bool accepted_by_round[2][2] = {{false, true}, {true, true}};
    uint32_t ids[2] = {301, 302};
    uint32_t boot_acked[2] = {0, 0};
    int boot_ack_n = 0;
    int attempts[2] = {0, 0};
    for (int round = 0; round < 2; round++) {
        bool room_blocked = false;
        for (int i = 0; i < 2; i++) {
            NotifyChatDrainResult result = notify_chat_drain_result(
                room_blocked, accepted_by_round[round][i]);
            if (result == NOTIFY_CHAT_DRAIN_KEEP_FAILED) room_blocked = true;
            if (result == NOTIFY_CHAT_DRAIN_BLOCKED) continue;
            attempts[i]++;
            if (result == NOTIFY_CHAT_DRAIN_ACK_ROUTED)
                boot_acked[boot_ack_n++] = ids[i];
        }
    }
    assert(attempts[0] == 2 && attempts[1] == 1);
    assert(boot_ack_n == 2 && boot_acked[0] == 301 && boot_acked[1] == 302);

    /* Crash-before-ACK: B is already durable. Boot blocks it behind failed A;
     * retry appends A once, then plans B as ACK-only without a duplicate append. */
    bool origins[2] = {false, true};
    bool retry_blocked = false;
    int append_calls[2] = {0, 0};
    int ack_calls[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        NotifyChatThreadState s = thread(origins[i]);
        NotifyChatAction action = notify_chat_reconcile_plan(
            &mapped, "line", 1800000000u + (uint32_t)i, &s);
        bool accepted = action == NOTIFY_CHAT_ACK_ALREADY_ROUTED;
        if (action == NOTIFY_CHAT_ROUTE_THEN_ACK) {
            append_calls[i]++;
            accepted = true;
        }
        NotifyChatDrainResult result = notify_chat_drain_result(retry_blocked,
                                                                 accepted);
        assert(result == NOTIFY_CHAT_DRAIN_ACK_ROUTED);
        ack_calls[i]++;
    }
    assert(append_calls[0] == 1 && append_calls[1] == 0);
    assert(ack_calls[0] == 1 && ack_calls[1] == 1);

    /* A recycled hint must never authorize its new occupant. Missing stable id
     * fails; if the same stable id relocated, the fresh lookup is authoritative. */
    assert(notify_chat_stable_slot("peer-old", "peer-new", -1) == -1);
    assert(notify_chat_stable_slot("peer-old", "peer-new", 6) == -1);
    assert(notify_chat_stable_slot("peer-old", "peer-old", 2) == 2);
}

int main(void) {
    test_door_prefix();
    test_chat_door_key();
    test_source_with_conv();
    test_genuine_notification();
    test_empty();
    test_reconcile_planner();
    test_arrival_planner();
    test_restore_order();
    test_append_persistence();
    test_two_card_restore_batch();
    test_live_burst_drain();
    printf("notify chat-class tests: OK\n");
    return 0;
}
