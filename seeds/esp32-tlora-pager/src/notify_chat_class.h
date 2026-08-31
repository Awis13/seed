#ifndef NOTIFY_CHAT_CLASS_H
#define NOTIFY_CHAT_CLASS_H

/*
 * Pure chat-card resolver and reconciliation planner.
 *
 * A card may be hidden as a chat only when it resolves to an existing
 * conversation. Resolution and routing deliberately share this one result:
 * callers cannot classify a card with one rule and later derive a different
 * conversation with another.
 *
 * Restored cards need a second decision because boot does not replay their
 * arrival event. Conversation JSONL records therefore carry the durable card
 * id and key as optional origin fields. Boot can acknowledge only an exact
 * origin match; text equality is never identity, so consecutive "OK" cards are
 * retained as two messages.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define NOTIFY_CHAT_DOOR_SUFFIX     "-chat"
#define NOTIFY_CHAT_DOOR_SUFFIX_LEN 5
#define NOTIFY_CHAT_ID_CAP          64
#define NOTIFY_CHAT_VALID_EPOCH     1700000000u

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

typedef int (*NotifyChatFindFn)(const char *id, void *ctx);

typedef struct NotifyChatResolution {
    int conversation;
    char id[NOTIFY_CHAT_ID_CAP];
} NotifyChatResolution;

static inline NotifyChatResolution notify_chat_resolve(
        const char *source, const char *key, NotifyChatFindFn find, void *ctx) {
    NotifyChatResolution out;
    out.conversation = -1;
    out.id[0] = '\0';
    if (!find) return out;

    int source_conversation = -1;
    if (source && source[0]) source_conversation = find(source, ctx);

    char prefix[NOTIFY_CHAT_ID_CAP];
    int door_conversation = -1;
    if (notify_chat_door_prefix(key, prefix, sizeof(prefix)) > 0) {
        door_conversation = find(prefix, ctx);
    }
    /* Two independently valid coordinates that name different rooms are
     * ambiguous. Keeping the card visible is safer than routing it wrongly. */
    if (source_conversation >= 0 && door_conversation >= 0 &&
        source_conversation != door_conversation) return out;
    if (source_conversation >= 0) {
        out.conversation = source_conversation;
        snprintf(out.id, sizeof(out.id), "%s", source);
    } else if (door_conversation >= 0) {
        out.conversation = door_conversation;
        snprintf(out.id, sizeof(out.id), "%s", prefix);
    }
    return out;
}

typedef enum NotifyChatAction {
    NOTIFY_CHAT_KEEP_CARD = 0,
    NOTIFY_CHAT_ACK_ALREADY_ROUTED,
    NOTIFY_CHAT_ROUTE_THEN_ACK,
} NotifyChatAction;

typedef struct NotifyChatThreadState {
    bool origin_present;
} NotifyChatThreadState;

static inline NotifyChatAction notify_chat_reconcile_plan(
        const NotifyChatResolution *resolution, const char *normalized_body,
        uint32_t card_epoch, const NotifyChatThreadState *thread) {
    if (!resolution || resolution->conversation < 0 ||
        !normalized_body || !normalized_body[0])
        return NOTIFY_CHAT_KEEP_CARD;

    (void)card_epoch;
    if (thread && thread->origin_present)
        return NOTIFY_CHAT_ACK_ALREADY_ROUTED;
    return NOTIFY_CHAT_ROUTE_THEN_ACK;
}

static inline NotifyChatAction notify_chat_arrival_plan(
        const NotifyChatResolution *resolution, const char *normalized_body,
        const NotifyChatThreadState *thread) {
    (void)thread;
    if (!resolution || resolution->conversation < 0 ||
        !normalized_body || !normalized_body[0])
        return NOTIFY_CHAT_KEEP_CARD;
    return NOTIFY_CHAT_ROUTE_THEN_ACK;
}

typedef struct NotifyChatRestoreOrder {
    uint32_t card_epoch;
    uint8_t restore_rank;
} NotifyChatRestoreOrder;

typedef enum NotifyChatDrainResult {
    NOTIFY_CHAT_DRAIN_BLOCKED = 0,
    NOTIFY_CHAT_DRAIN_KEEP_FAILED,
    NOTIFY_CHAT_DRAIN_ACK_ROUTED,
} NotifyChatDrainResult;

static inline NotifyChatDrainResult notify_chat_drain_result(
        bool earlier_failed, bool accepted) {
    if (earlier_failed) return NOTIFY_CHAT_DRAIN_BLOCKED;
    return accepted ? NOTIFY_CHAT_DRAIN_ACK_ROUTED
                    : NOTIFY_CHAT_DRAIN_KEEP_FAILED;
}

static inline int notify_chat_stable_slot(const char *expected_id,
                                          const char *resolved_id,
                                          int resolved_slot) {
    if (!expected_id || !expected_id[0] || !resolved_id ||
        strcmp(expected_id, resolved_id) != 0 || resolved_slot < 0)
        return -1;
    return resolved_slot;
}

/* Oldest first by archive restore rank. Rank is available for every card and
 * reflects append chronology even across an unset clock or a wall-clock
 * rollback. Using it as the primary key also makes this a strict total order;
 * pairwise switching between timestamp and rank is non-transitive. Epoch is a
 * deterministic fallback only for synthetic equal-rank inputs. */
static inline bool notify_chat_restore_before(NotifyChatRestoreOrder a,
                                              NotifyChatRestoreOrder b) {
    if (a.restore_rank != b.restore_rank)
        return a.restore_rank > b.restore_rank;
    if (a.card_epoch != b.card_epoch)
        return a.card_epoch < b.card_epoch;
    return false;
}

static inline bool notify_chat_tail_needs_separator(uint32_t size,
                                                    bool last_byte_read,
                                                    uint8_t last_byte) {
    return size > 0 && (!last_byte_read || last_byte != (uint8_t)'\n');
}

static inline bool notify_chat_readback_verified(uint32_t append_start,
                                                 uint32_t record_n,
                                                 uint32_t file_size,
                                                 uint32_t read_n,
                                                 bool bytes_equal) {
    return record_n > 0 && file_size == append_start + record_n &&
           read_n == record_n && bytes_equal;
}

#endif /* NOTIFY_CHAT_CLASS_H */
