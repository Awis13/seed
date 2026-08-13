/*
 * Host test for the cross-transport card KEY (ticket CARD-DELIVERY / C1).
 *
 * The device dedups a notification card by its archive identity
 * (NS_NOTIFY, notify_rec_archive_key(key,id)). For the SAME logical card arriving
 * over two transports to dedup to ONE entry, every ingress must extract the SAME
 * gateway-stamped key. This drives the PURE pieces of each ingress — the P/M wire
 * parse (src/notify_wire.h), the LXMF route planner (src/lxmf_route.h), and the
 * archive-key derivation (src/micron/notify_record.h) — with hand-built wires and
 * asserts the VALUES, so a card sent as P2, M2 and LXMF with key K lands on ONE
 * archive identity, while a keyless M1 lands on a DISJOINT synthetic one.
 *
 * These are the same pure functions the firmware compiles (notify_ingest_p1 and
 * mesh_on_private_text call notify_wire_parse_*; lxmf_route_plan feeds the LXMF
 * card path), so this test exercises the real key logic, not a copy.
 *
 * MUTATION CHECKS (run by hand, must go RED, then restore):
 *   - notify_wire_parse_m: on the M2 branch set out->key[0]='\0' (drop the key)
 *     -> test_cross_transport_same_identity / test_m2_dedups_against_p2 FAIL.
 *   - notify_rec_archive_key: make it ignore `key` and always return the id-key
 *     -> the same two tests FAIL (keyed cards would collide with keyless ones).
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/notify_wire.h"
#include "../src/lxmf_route.h"
#include "../src/micron/notify_record.h"

/* The archive identity a card would upsert under, given its dedup key and id. */
static void archive_id(const char *key, uint32_t id, char *out, size_t cap) {
    char tmp[NOTIFY_ARCHIVE_IDKEY_CAP];
    const char *k = notify_rec_archive_key(key, id, tmp, sizeof(tmp));
    snprintf(out, cap, "%s", k);
}

/* --- 1. P-series parse: explicit key (P2) and legacy heuristic (P1) --------- */

static void test_p2_explicit_key(void) {
    NotifyPFrame f;
    /* body deliberately CONTAINS '|': the v2 tag guarantees the LAST '|' begins
     * the key, so the body is kept whole and the key is exact. */
    assert(notify_wire_parse_p("P2|warn|home-rig|Backup|a|b|c done|job-backup", &f));
    assert(strcmp(f.level, "warn") == 0);
    assert(strcmp(f.source, "home-rig") == 0);
    assert(strcmp(f.title, "Backup") == 0);
    assert(strcmp(f.key, "job-backup") == 0);
    assert(f.body_len == strlen("a|b|c done"));
    assert(memcmp(f.body, "a|b|c done", f.body_len) == 0);

    /* P2 with an EMPTY key ("...|body|") is keyless. */
    NotifyPFrame g;
    assert(notify_wire_parse_p("P2|info|src|T|the body|", &g));
    assert(g.key[0] == '\0');
    assert(g.body_len == strlen("the body"));
}

static void test_p1_legacy_heuristic(void) {
    NotifyPFrame f;
    /* A key-shaped trailing token is recovered (legacy behaviour, unchanged). */
    assert(notify_wire_parse_p("P1|info|k1c|Print|layer 5/100|k1c.print", &f));
    assert(strcmp(f.key, "k1c.print") == 0);
    assert(f.body_len == strlen("layer 5/100"));

    /* A trailing token that cannot be a key (has a space) stays in the body. */
    NotifyPFrame g;
    assert(notify_wire_parse_p("P1|info|k1c|Print|done|all good", &g));
    assert(g.key[0] == '\0');
    assert(g.body_len == strlen("done|all good"));

    /* No trailing key at all: whole remainder is the body. */
    NotifyPFrame h;
    assert(notify_wire_parse_p("P1|info|src|T|just a body", &h));
    assert(h.key[0] == '\0');
    assert(h.body_len == strlen("just a body"));
}

static void test_p_rejects_malformed(void) {
    NotifyPFrame f;
    assert(!notify_wire_parse_p(NULL, &f));
    assert(!notify_wire_parse_p("X1|a|b|c|d", &f));   /* wrong family      */
    assert(!notify_wire_parse_p("P3|a|b|c|d", &f));   /* unknown version   */
    assert(!notify_wire_parse_p("P1|only", &f));      /* missing fields    */
    assert(!notify_wire_parse_p("P1|a|b", &f));       /* no title separator */

    /* Single-char / empty frames: the prefix guard must NOT read wire[2] past the
     * NUL. Without the wire[1]!='\0' check these abort under ASAN (OOB read of
     * size 1); with it they are cleanly rejected. */
    assert(!notify_wire_parse_p("P", &f));            /* wire[2] is 1 past NUL */
    assert(!notify_wire_parse_p("", &f));             /* empty frame        */
    assert(!notify_wire_parse_p("P1", &f));           /* two chars, no '|'  */
}

/* --- 2. M-series parse: explicit key (M2) and keyless legacy (M1) ----------- */

static void test_m2_explicit_key(void) {
    NotifyMFrame f;
    /* chunk contains '|': last '|' begins the key, chunk kept whole. */
    assert(notify_wire_parse_m("M2|ab|1|2|crit|home-rig|Alert|part|one|job-x", &f));
    assert(strcmp(f.mid, "ab") == 0);
    assert(f.part == 1 && f.n_parts == 2);
    assert(strcmp(f.level, "crit") == 0);
    assert(strcmp(f.source, "home-rig") == 0);
    assert(strcmp(f.title, "Alert") == 0);
    assert(strcmp(f.key, "job-x") == 0);
    assert(f.chunk_len == strlen("part|one"));
    assert(memcmp(f.chunk, "part|one", f.chunk_len) == 0);
}

static void test_m1_legacy_keyless(void) {
    NotifyMFrame f;
    assert(notify_wire_parse_m("M1|ab|1|2|info|home-rig|Note|hello world", &f));
    assert(f.key[0] == '\0');                       /* M1 never carries a key */
    assert(f.chunk_len == strlen("hello world"));
    assert(memcmp(f.chunk, "hello world", f.chunk_len) == 0);
}

static void test_m_rejects_malformed(void) {
    NotifyMFrame f;
    assert(!notify_wire_parse_m(NULL, &f));
    assert(!notify_wire_parse_m("M1|ab", &f));       /* truncated */
    assert(!notify_wire_parse_m("Z1|ab|1|2|i|s|t|c", &f));

    /* Single-char / empty frames: the prefix guard must NOT read frame[2] past the
     * NUL. Without the frame[1]!='\0' check these abort under ASAN (OOB read of
     * size 1); with it they are cleanly rejected. */
    assert(!notify_wire_parse_m("M", &f));           /* frame[2] is 1 past NUL */
    assert(!notify_wire_parse_m("", &f));            /* empty frame        */
    assert(!notify_wire_parse_m("M1", &f));          /* two chars, no '|'  */
}

/* --- 3. THE FIX: one gateway key -> one archive identity over any wire ------ */

/* Reassemble a two-fragment M2 exactly as mesh_on_private_text does: capture the
 * key from the FIRST fragment; the gateway repeats the same key on every one. */
static void reassemble_m2_key(char *key_out, size_t cap) {
    NotifyMFrame a, b;
    assert(notify_wire_parse_m("M2|zz|1|2|info|home-rig|Ping|hel|pingani-x1", &a));
    assert(notify_wire_parse_m("M2|zz|2|2|info|home-rig|Ping|lo|pingani-x1", &b));
    snprintf(key_out, cap, "%s", a.key);   /* first-fragment key wins */
    (void)b;
}

static void test_cross_transport_same_identity(void) {
    const char *K = "pingani-x1";

    /* P2 wire */
    NotifyPFrame p;
    assert(notify_wire_parse_p("P2|info|home-rig|Ping|hello|pingani-x1", &p));

    /* M2 wire (reassembled) */
    char mkey[NW_KEY_CAP];
    reassemble_m2_key(mkey, sizeof(mkey));

    /* LXMF wire: gateway meta.key */
    LxmfMsg m;
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < LXMF_HASH_LEN; i++) m.source_hash[i] = (uint8_t)(0x40 + i);
    m.title_len = 4; memcpy(m.title, "Ping", 5);
    m.content_len = 5; memcpy(m.content, "hello", 6);
    m.has_meta = 1;
    m.meta_key_len = strlen(K); memcpy(m.meta_key, K, m.meta_key_len + 1);
    LxmfRoute lr; lxmf_route_plan(&m, &lr);
    assert(lr.kind == LXMF_ROUTE_CARD);

    /* Every ingress recovered the SAME key verbatim. */
    assert(strcmp(p.key, K) == 0);
    assert(strcmp(mkey, K) == 0);
    assert(strcmp(lr.key, K) == 0);

    /* And so all three map to ONE archive identity, even with DIFFERENT numeric
     * ids (the id never enters the identity when a valid client key is present). */
    char ip[64], im[64], il[64];
    archive_id(p.key,  101, ip, sizeof(ip));
    archive_id(mkey,   202, im, sizeof(im));
    archive_id(lr.key, 303, il, sizeof(il));
    assert(strcmp(ip, K)  == 0);
    assert(strcmp(ip, im) == 0);
    assert(strcmp(ip, il) == 0);
}

/* THE regression this ticket closes: a keyed M2 card now dedups against the SAME
 * key delivered as P2, despite arriving with a different numeric id. Before C1 the
 * multipart path passed a NULL key, so its identity was "#<id>" — never equal. */
static void test_m2_dedups_against_p2(void) {
    char mkey[NW_KEY_CAP];
    reassemble_m2_key(mkey, sizeof(mkey));
    NotifyPFrame p;
    assert(notify_wire_parse_p("P2|info|home-rig|Ping|hello|pingani-x1", &p));

    char im[64], ip[64];
    archive_id(mkey, 1, im, sizeof(im));
    archive_id(p.key, 999, ip, sizeof(ip));
    assert(strcmp(im, ip) == 0);   /* one identity -> newest-wins upsert, one card */
}

/* A keyless M1 gets a SYNTHETIC id-key in a disjoint subspace, so it can never
 * dedup against a keyed card — two separate cards, as intended. */
static void test_keyless_m1_is_separate(void) {
    NotifyMFrame f;
    assert(notify_wire_parse_m("M1|ab|1|1|info|home-rig|Note|body", &f));
    assert(f.key[0] == '\0');

    char idk[64], keyed[64];
    archive_id(f.key[0] ? f.key : "", 55, idk, sizeof(idk));   /* keyless -> "#55" */
    archive_id("pingani-x1", 55, keyed, sizeof(keyed));        /* keyed   -> key    */
    assert(idk[0] == '#');
    assert(strcmp(idk, "#55") == 0);
    assert(strcmp(idk, keyed) != 0);   /* disjoint: never collide, even at same id */
}

/* A DIFFERENT key is a DIFFERENT identity — distinct cards stay distinct. */
static void test_different_key_is_separate(void) {
    char a[64], b[64];
    archive_id("pingani-x1", 1, a, sizeof(a));
    archive_id("k1c.print",  1, b, sizeof(b));
    assert(strcmp(a, b) != 0);
}

int main(void) {
    test_p2_explicit_key();
    test_p1_legacy_heuristic();
    test_p_rejects_malformed();
    test_m2_explicit_key();
    test_m1_legacy_keyless();
    test_m_rejects_malformed();
    test_cross_transport_same_identity();
    test_m2_dedups_against_p2();
    test_keyless_m1_is_separate();
    test_different_key_is_separate();
    printf("notify wire cross-transport key tests: OK\n");
    return 0;
}
