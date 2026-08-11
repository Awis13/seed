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

static void test_plain_card() {
    LxmfMsg m; mk_plain(&m, "Hi", "Hello there", 0x10);
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_CARD);
    assert(strcmp(r.level, "info") == 0);
    assert(strcmp(r.source, "lxmf") == 0);
    /* derived key: "lxmf-" prefix, inside the cap, non-empty */
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

static void test_empty_thread_is_card() {
    LxmfMsg m; mk_plain(&m, "T", "B", 0x50);
    m.has_thread = 1; m.thread_len = 0;   /* flagged but empty */
    LxmfRoute r; lxmf_route_plan(&m, &r);
    assert(r.kind == LXMF_ROUTE_CARD);
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
    test_plain_card();
    test_severity();
    test_meta_key_and_src();
    test_thread_room();
    test_empty_thread_is_card();
    test_key_stability();
    test_source_control_stripped();
    printf("lxmf route tests: OK\n");
    return 0;
}
