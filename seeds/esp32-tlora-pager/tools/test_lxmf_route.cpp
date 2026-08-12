/*
 * Host test for the pure LXMF routing planner in src/lxmf_route.h.
 *
 * No firmware / FreeRTOS / SD / RNS. lxmf_route_plan() decides card-vs-room, the
 * severity string, the card source label and the replace-in-place key from a
 * parsed LxmfMsg; this drives it with hand-built messages and asserts the VALUES,
 * not just "no crash". Cases:
 *   - a plain message (only title+content) -> info card, source "lxmf", derived key
 *   - meta.sev 0/1/2/7 -> info/warn/crit/crit
 *   - meta.key present -> that key verbatim
 *   - meta.src present -> that source label
 *   - a thread -> room, whose name is the thread bytes
 *   - a zero-length thread is treated as absent -> card
 *   - key derivation: identical message -> identical key; different content or
 *     different source -> different key
 *   - a control byte in a source label is stripped to '.'
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../src/lxmf_route.h"

/* Build a minimal plain message: title + content only, source hash filled. */
static void mk_plain(LxmfMsg *m, const char *title, const char *content,
                     uint8_t src_seed) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < LXMF_HASH_LEN; i++) m->source_hash[i] = (uint8_t)(src_seed + i);
    m->title_len = strlen(title);
    memcpy(m->title, title, m->title_len + 1);
    m->content_len = strlen(content);
    memcpy(m->content, content, m->content_len + 1);
}

/* A bare message — no thread, none of our notification meta — is a person
 * writing to us, so it opens their conversation rather than a card. */
static void test_bare_message_is_chat() {
    LxmfMsg m; mk_plain(&m, "Hi", "Hello there", 0x10);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_CHAT);
    /* The card fields are still computed: a chat that cannot be landed (queue
     * full, or no free slot for a stranger) falls back to a card, and it must
     * have one to fall back to. */
    assert(strcmp(r.level, "info") == 0);
    assert(strcmp(r.source, "lxmf") == 0);
    assert(strncmp(r.key, "lxmf-", 5) == 0);
    assert(strlen(r.key) < LXMF_ROUTE_KEY_CAP);
}

static void test_severity() {
    struct { uint32_t sev; const char *want; } cases[] = {
        {0, "info"}, {1, "warn"}, {2, "crit"}, {7, "crit"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        LxmfMsg m; mk_plain(&m, "T", "B", 0x20);
        m.has_meta = 1; m.meta_has_sev = 1; m.meta_sev = cases[i].sev;
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD);
        assert(strcmp(r.level, cases[i].want) == 0);
    }
}

static void test_meta_key_and_src() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x30);
    m.has_meta = 1;
    const char *k = "k1c.print";
    m.meta_key_len = strlen(k); memcpy(m.meta_key, k, m.meta_key_len + 1);
    const char *s = "home-rig";
    m.meta_src_len = strlen(s); memcpy(m.meta_src, s, m.meta_src_len + 1);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_CARD);
    assert(strcmp(r.key, "k1c.print") == 0);
    assert(strcmp(r.source, "home-rig") == 0);
}

static void test_thread_room() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x40);
    m.has_thread = 1;
    const char *th = "work";
    m.thread_len = strlen(th); memcpy(m.thread, th, m.thread_len);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_ROOM);
    assert(strcmp(r.room, "work") == 0);
}

static void test_empty_thread_is_chat() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x50);
    m.has_thread = 1; m.thread_len = 0;   /* flagged but empty */
    LxmfRoute r; lxmf_route_plan(&m, &r);
    /* An empty thread names no room, so it falls through like any bare
     * message — to the sender's conversation. */
    assert(r.kind == LXMF_ROUTE_CHAT);
}

/* THE ORDER OF THE TWO TESTS IS ITSELF THE CONTRACT. A message carrying BOTH a
 * thread and our notification meta is a room, and has been since before chats
 * existed. Testing the meta first would quietly turn every one of those into a
 * card — a live behaviour change disguised as a refactor — so the case is
 * pinned explicitly rather than left to the reading order of the source. */
static void test_meta_with_thread_stays_a_room() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x55);
    m.has_thread = 1;
    const char *th = "work";
    m.thread_len = strlen(th); memcpy(m.thread, th, m.thread_len);
    m.has_meta = 1; m.meta_has_sev = 1; m.meta_sev = 2;
    const char *k = "some.key";
    m.meta_key_len = strlen(k); memcpy(m.meta_key, k, m.meta_key_len + 1);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_ROOM);
    assert(strcmp(r.room, "work") == 0);
}

/* THE PRESENCE OF OUR FIELD IS THE TEST, not the presence of any key inside
 * it. Listing the keys individually is how `ttl` and an empty map got missed
 * and routed to chats — so every shape of the field, including one with
 * nothing in it, is pinned here as a card. */
static void test_any_meta_field_is_a_card() {
    {   LxmfMsg m; mk_plain(&m, "T", "B", 0x56);
        m.has_meta = 1; m.meta_has_sev = 1; m.meta_sev = 0;
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD); }
    {   LxmfMsg m; mk_plain(&m, "T", "B", 0x57);
        m.has_meta = 1;
        const char *k = "kk";
        m.meta_key_len = strlen(k); memcpy(m.meta_key, k, m.meta_key_len + 1);
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD); }
    {   LxmfMsg m; mk_plain(&m, "T", "B", 0x58);
        m.has_meta = 1;
        const char *s = "ss";
        m.meta_src_len = strlen(s); memcpy(m.meta_src, s, m.meta_src_len + 1);
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD); }
    /* ttl alone — the fourth meta key, and the one an enumeration forgot. */
    {   LxmfMsg m; mk_plain(&m, "T", "B", 0x5A);
        m.has_meta = 1; m.meta_has_ttl = 1; m.meta_ttl = 3600;
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD); }
    /* The field present but EMPTY is still our gateway speaking: no
     * third-party client sets FIELD_CUSTOM_META at all, so its presence is
     * what carries the meaning. */
    {   LxmfMsg m; mk_plain(&m, "T", "B", 0x59);
        m.has_meta = 1;
        LxmfRoute r; lxmf_route_plan(&m, &r);
        assert(r.kind == LXMF_ROUTE_CARD); }
}

/* A chat's route carries NO sender identity: on that branch source is always
 * the "lxmf" literal, because a chat is reached only when our meta is absent,
 * which is exactly when the planner falls back to it. Passing it as a
 * conversation label would name every sender alike, so the firmware passes
 * nullptr and the distinguishing hex id shows instead. Pinned here because it
 * is a property of the ROUTE, and the value is what makes it a bug. */
static void test_chat_route_carries_no_sender_name() {
    LxmfMsg a; mk_plain(&a, "T", "B", 0x10);
    LxmfMsg b; mk_plain(&b, "T", "B", 0x40);
    LxmfRoute ra, rb;
    lxmf_route_plan(&a, &ra);
    lxmf_route_plan(&b, &rb);
    assert(ra.kind == LXMF_ROUTE_CHAT && rb.kind == LXMF_ROUTE_CHAT);
    /* Same constant for two different senders — it identifies nobody. */
    assert(strcmp(ra.source, "lxmf") == 0);
    assert(strcmp(rb.source, ra.source) == 0);
    /* The source hashes, by contrast, differ — which is why the id is the
     * label that tells two senders apart. */
    assert(memcmp(a.source_hash, b.source_hash, LXMF_HASH_LEN) != 0);
}

static void test_key_stability() {
    /* identical -> identical key */
    LxmfMsg a; mk_plain(&a, "Same", "Body", 0x60);
    LxmfMsg b; mk_plain(&b, "Same", "Body", 0x60);
    LxmfRoute ra, rb; lxmf_route_plan(&a, &ra); lxmf_route_plan(&b, &rb);
    assert(strcmp(ra.key, rb.key) == 0);

    /* different content -> different key */
    LxmfMsg c; mk_plain(&c, "Same", "Other", 0x60);
    LxmfRoute rc; lxmf_route_plan(&c, &rc);
    assert(strcmp(ra.key, rc.key) != 0);

    /* different source -> different key */
    LxmfMsg d; mk_plain(&d, "Same", "Body", 0x99);
    LxmfRoute rd; lxmf_route_plan(&d, &rd);
    assert(strcmp(ra.key, rd.key) != 0);
}

static void test_source_control_stripped() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x70);
    m.has_meta = 1;
    char s[4] = { 'a', 0x01, 'b', 0 };   /* control byte in the middle */
    m.meta_src_len = 3; memcpy(m.meta_src, s, 4);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(strcmp(r.source, "a.b") == 0);
}

int main() {
    test_bare_message_is_chat();
    test_severity();
    test_meta_key_and_src();
    test_thread_room();
    test_empty_thread_is_chat();
    test_meta_with_thread_stays_a_room();
    test_any_meta_field_is_a_card();
    test_chat_route_carries_no_sender_name();
    test_key_stability();
    test_source_control_stripped();
    printf("lxmf route tests: OK\n");
    return 0;
}
