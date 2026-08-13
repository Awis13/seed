#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/outbox.h"

int main() {
    OutboxStore store;
    outbox_init(&store);
    uint32_t first = outbox_enqueue(&store, OUTBOX_KIND_REPLY, "card-1", "",
                                    "wire-1", "hello");
    assert(first == 1);
    assert(outbox_enqueue(&store, OUTBOX_KIND_AGENT, "claude", "room-a",
                          "wire-2", "world") == 2);
    assert(outbox_oldest_pending(&store)->id == first);
    assert(outbox_set_state(&store, first, OUTBOX_STATE_SENT));
    assert(outbox_latest_target(&store, OUTBOX_KIND_REPLY, "card-1")->id == first);
    assert(outbox_oldest_pending(&store)->id == 2);

    for (int i = 0; i < OUTBOX_SLOTS - 2; ++i) {
        char target[16];
        snprintf(target, sizeof(target), "agent-%d", i);
        assert(outbox_enqueue(&store, OUTBOX_KIND_AGENT, target, "room", "k",
                              "message"));
    }
    uint32_t replacement = outbox_enqueue(&store, OUTBOX_KIND_REPLY, "card-2",
                                           "", "wire-3", "replacement");
    assert(replacement != 0);
    assert(outbox_find_id(&store, first) < 0);
    assert(outbox_find_id(&store, replacement) >= 0);
    assert(outbox_enqueue(&store, OUTBOX_KIND_REPLY, "full", "", "k", "x") == 0);
    assert(outbox_retry_delay_ms(1) == 5000);
    assert(outbox_retry_delay_ms(9) == 900000);

    uint8_t snapshot[OUTBOX_SNAPSHOT_MAX];
    size_t encoded = outbox_encode(&store, snapshot, sizeof(snapshot));
    assert(encoded > 0);
    OutboxStore restored;
    assert(outbox_decode(&restored, snapshot, encoded));
    assert(restored.next_id == store.next_id);
    int slot = outbox_find_id(&restored, replacement);
    assert(slot >= 0);
    assert(strcmp(restored.items[slot].text, "replacement") == 0);

    snapshot[8] ^= 0x01;
    OutboxStore untouched = restored;
    assert(!outbox_decode(&restored, snapshot, encoded));
    assert(memcmp(&restored, &untouched, sizeof(restored)) == 0);
    puts("outbox tests: OK");
}
