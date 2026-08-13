#ifndef NOTIFY_CHAT_CLASS_H
#define NOTIFY_CHAT_CLASS_H

/*
 * Pure chat-vs-notification classifier for an incoming notification card.
 *
 * An incoming CHAT (an agent talking back — claude / hermes / opencode …) must
 * land in its conversation thread and badge, exactly like a known mesh/LXMF
 * peer's message does (inbox_deliver_msg_mesh / inbox_deliver_msg_lxmf). A
 * genuine notification (HA / a sensor / a service with no conversation) stays a
 * severity card. This header is the ONE decision that tells the two apart, so
 * the five dispatch sites in main.cpp no longer each re-derive it and drift.
 *
 * The signal is "does this card resolve to an existing agent conversation?":
 *   - its source names one (opencode / claude / hermes …), which broadens the
 *     old rule beyond the "-chat" suffix — opencode/opencode-pager cards carry
 *     no "-chat" key (that suffix is a gateway-side choice for some doors), so
 *     the source is what catches them; OR
 *   - it carries a legacy "-chat" door key ("hermes-chat" → "hermes") whose
 *     prefix names one.
 * A source with no conversation and no "-chat" door is a real notification and
 * is NOT chat — that is what keeps HA/sensor/service pages behaving as cards.
 *
 * Kept pure (no device types, conversation lookup injected as a callback) so the
 * host suite pins the decision by value.
 */

#include <stddef.h>
#include <string.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

/* The suffix a legacy bridge chat-door key carries, e.g. "hermes-chat". */
#define NOTIFY_CHAT_DOOR_SUFFIX     "-chat"
#define NOTIFY_CHAT_DOOR_SUFFIX_LEN 5   /* strlen("-chat") */

/*
 * If `key` ends in "-chat", copy the prefix ("hermes-chat" → "hermes") into
 * `out` (NUL-terminated, bounded to out_n) and return its length; otherwise
 * leave `out` empty and return 0. Pure.
 */
static inline size_t notify_chat_door_prefix(const char *key,
                                             char *out, size_t out_n) {
    if (out && out_n) out[0] = '\0';
    if (!key || !out || out_n == 0) return 0;
    size_t n = strlen(key);
    if (n <= NOTIFY_CHAT_DOOR_SUFFIX_LEN) return 0;
    if (strcmp(key + (n - NOTIFY_CHAT_DOOR_SUFFIX_LEN),
               NOTIFY_CHAT_DOOR_SUFFIX) != 0)
        return 0;
    size_t m = n - NOTIFY_CHAT_DOOR_SUFFIX_LEN;
    if (m >= out_n) m = out_n - 1;
    memcpy(out, key, m);
    out[m] = '\0';
    return m;
}

/*
 * Is this card a chat (land in an agent conversation thread + badge) rather than
 * a severity/notification card?
 *
 * `conv_exists(id, ctx)` answers "does a conversation with this literal id
 * exist?". A card is chat when its source names an existing conversation, or it
 * carries a "-chat" door key whose prefix does. Everything else is NOT chat.
 */
static inline bool notify_card_is_chat(const char *source, const char *key,
                                       bool (*conv_exists)(const char *id, void *ctx),
                                       void *ctx) {
    if (!conv_exists) return false;
    /* source names a conversation: opencode / claude / hermes … */
    if (source && source[0] && conv_exists(source, ctx)) return true;
    /* legacy "-chat" door: its prefix names a conversation */
    char prefix[64];
    if (notify_chat_door_prefix(key, prefix, sizeof(prefix)) > 0 &&
        conv_exists(prefix, ctx))
        return true;
    return false;
}

#endif /* NOTIFY_CHAT_CLASS_H */
