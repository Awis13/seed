/*
 * Host test for the pure chat-vs-notification classifier (TLORA-UI-FIX / C2),
 * src/notify_chat_class.h.
 *
 * Drives the PURE decision the device calls (main.cpp notify_is_chat() wraps
 * notify_card_is_chat() with a live agents_find() lookup; agents_index_for_card()
 * mirrors the same resolution order) with hand-built cards and asserts the
 * VALUES. The arrival wiring in main.cpp — landing the chat via agents_on_inbound
 * and suppressing the invite/severity card — is device-only (FreeRTOS tasks, the
 * panel, the SD store) and has no host harness; it is exercised on the device.
 *
 * MUTATION CHECKS (apply by hand to src/notify_chat_class.h, must go RED, then
 * restore):
 *   - drop the agent-source branch: neuter the source guard in notify_card_is_chat
 *     to `if (source && source[0] && false)` -> test_source_with_conv
 *     ("opencode","" -> chat) FAILS. "an opencode card is no longer recognised as
 *     chat." (Deleting the line outright also goes RED, via -Werror: `source`
 *     becomes unused.)
 *   - treat all cards as chat: make notify_card_is_chat `return true;` at the top
 *     -> test_genuine_notification (ha -> NOT chat) and test_empty (-> NOT chat)
 *     FAIL. "a genuine HA notification pops as a chat."
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

static bool conv_exists(const char *id, void * /*ctx*/) {
    if (!id || !id[0]) return false;
    for (size_t i = 0; i < sizeof(g_convs) / sizeof(g_convs[0]); i++)
        if (strcmp(id, g_convs[i]) == 0) return true;
    return false;
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
    /* no source, but the door prefix names a conversation */
    assert(notify_card_is_chat("", "hermes-chat", conv_exists, NULL) == true);
    /* claude-chat likewise */
    assert(notify_card_is_chat("", "claude-chat", conv_exists, NULL) == true);
}

/* --- 3. an agent source with a conversation is chat (the broadening) -------- */

static void test_source_with_conv(void) {
    /* opencode/opencode-pager carry NO "-chat" key — the source is the signal */
    assert(notify_card_is_chat("opencode", "opencode-pager", conv_exists, NULL) == true);
    assert(notify_card_is_chat("opencode", "", conv_exists, NULL) == true);
    /* claude by source alone, no key */
    assert(notify_card_is_chat("claude", "", conv_exists, NULL) == true);
}

/* --- 4. a genuine notification (no conversation, no door) is NOT chat ------- */

static void test_genuine_notification(void) {
    /* HA / sensor / service source with no conversation and no "-chat" door */
    assert(notify_card_is_chat("ha", "ha", conv_exists, NULL) == false);
    assert(notify_card_is_chat("sensor", "", conv_exists, NULL) == false);
    /* a source that is NOT a known conversation, key without "-chat" */
    assert(notify_card_is_chat("k1c", "k1c.print", conv_exists, NULL) == false);
}

/* --- 5. empty / unknown is NOT chat ---------------------------------------- */

static void test_empty(void) {
    assert(notify_card_is_chat("", "", conv_exists, NULL) == false);
    assert(notify_card_is_chat(NULL, NULL, conv_exists, NULL) == false);
    /* a "-chat" door whose prefix names NO conversation -> not chat */
    assert(notify_card_is_chat("", "ghost-chat", conv_exists, NULL) == false);
    /* a missing lookup callback is a hard no */
    assert(notify_card_is_chat("claude", "claude-chat", NULL, NULL) == false);
}

int main(void) {
    test_door_prefix();
    test_chat_door_key();
    test_source_with_conv();
    test_genuine_notification();
    test_empty();
    printf("notify chat-class tests: OK\n");
    return 0;
}
