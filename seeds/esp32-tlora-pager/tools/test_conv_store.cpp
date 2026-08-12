/*
 * Host tests for the pure conversation store in src/conv_store.h.
 *
 * The firmware half (skills/agents.cpp) owns the FS, the mutex and the SPI bus;
 * everything that decides WHAT is written — the thread key, the bounded on-disk
 * path, and the manifest record (transport + opaque reply address + label +
 * dead flag) — lives in the header and is exercised here with no Arduino, no
 * FreeRTOS and no SD.
 *
 * Covered:
 *   - conversation record round-trip through the manifest line, for every
 *     transport, including the reply address and its length;
 *   - the NEW 5-field manifest line AND the LEGACY 2-field / 3-field lines;
 *   - the on-disk path fits the SPIFFS object-name budget for the seeded agent
 *     rooms and for an 8-hex peer id;
 *   - the OLD "/agent.<id>.<session>" key form is GONE — asserted by exact
 *     equality and by absence of the legacy prefix, not by "the new one is in
 *     there somewhere";
 *   - an over-long key is folded to a path that still fits, and two long keys
 *     sharing a prefix do not collide;
 *   - malformed input is rejected instead of inventing a room.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/conv_store.h"
#include "../src/transport.h"

/* The two seeded agent conversations and the room the firmware mints at boot. */
#define SEED_A     "claude"
#define SEED_B     "hermes"
#define SEED_ROOM  "pager-abcd"
/* A peer conversation id in the shape a later commit will use: 8 hex of the
 * peer address, no rooms. */
#define PEER_ID    "a1b2c3d4"

/* --- helpers ---------------------------------------------------------------- */

static void path_of(const char *conv, const char *session, char *out,
                    size_t out_n) {
    bool ok = conv_log_path(out, out_n, conv, session);
    assert(ok);
}

/* Every path the store may hand out must fit the SPIFFS object-name budget
 * INCLUDING the NUL, and must not be in the retired key namespace. */
static void assert_path_sane(const char *path) {
    assert(strlen(path) + 1 <= CONV_PATH_LEN);
    assert(strncmp(path, CONV_PATH_PREFIX, strlen(CONV_PATH_PREFIX)) == 0);
    /* The retired form. Checking only that the new prefix is present would pass
     * just as happily on "/agent.claude.pager-abcd" gaining a sibling. */
    assert(strstr(path, "/agent.") == NULL);
    assert(strncmp(path, "/agent.", 7) != 0);
}

/* --- 1. path form: the new key, exactly, and the old one absent -------------- */

static void test_path_form(void) {
    char p[CONV_PATH_LEN];

    path_of(SEED_A, SEED_ROOM, p, sizeof(p));
    assert(strcmp(p, "/conv." SEED_A "." SEED_ROOM) == 0);
    assert_path_sane(p);

    path_of(SEED_B, SEED_ROOM, p, sizeof(p));
    assert(strcmp(p, "/conv." SEED_B "." SEED_ROOM) == 0);
    assert_path_sane(p);

    /* A peer conversation has no rooms: the key is the id alone. */
    path_of(PEER_ID, "", p, sizeof(p));
    assert(strcmp(p, "/conv." PEER_ID) == 0);
    assert_path_sane(p);
    path_of(PEER_ID, NULL, p, sizeof(p));
    assert(strcmp(p, "/conv." PEER_ID) == 0);

    /* Distinct rooms of one conversation stay distinct files. */
    char a[CONV_PATH_LEN], b[CONV_PATH_LEN];
    path_of(SEED_A, "work", a, sizeof(a));
    path_of(SEED_A, "home", b, sizeof(b));
    assert(strcmp(a, b) != 0);
    /* ... and so do the same room name under two conversations. */
    path_of(SEED_A, "work", a, sizeof(a));
    path_of(SEED_B, "work", b, sizeof(b));
    assert(strcmp(a, b) != 0);
}

/* --- 2. the budget is enforced, not assumed --------------------------------- */

static void test_path_budget(void) {
    /* A worst-case room name: the registry accepts CONV_SESSION_LEN-1 chars. */
    char longsess[CONV_SESSION_LEN];
    memset(longsess, 'x', sizeof(longsess) - 1);
    longsess[sizeof(longsess) - 1] = '\0';

    /* The case must still BE the over-long case if the constants ever move:
     * assert the unfolded key would have blown the budget before checking that
     * the folded path fits. Otherwise a later widening silently turns this into
     * a re-test of the short path. */
    char key[CONV_KEY_LEN];
    assert(conv_thread_key(key, sizeof(key), SEED_A, longsess));
    assert(strlen(CONV_PATH_PREFIX) + strlen(key) + 1 > CONV_PATH_LEN);

    char p[CONV_PATH_LEN];
    path_of(SEED_A, longsess, p, sizeof(p));
    assert_path_sane(p);

    /* Two long room names sharing every byte but the last must not collide. */
    char other[CONV_SESSION_LEN];
    memcpy(other, longsess, sizeof(other));
    other[sizeof(other) - 2] = 'y';
    char q[CONV_PATH_LEN];
    path_of(SEED_A, other, q, sizeof(q));
    assert_path_sane(q);
    assert(strcmp(p, q) != 0);

    /* Same key in, same path out — the fold is deterministic across reboots. */
    char again[CONV_PATH_LEN];
    path_of(SEED_A, longsess, again, sizeof(again));
    assert(strcmp(p, again) == 0);
}

/* --- 2b. the fold lives in a namespace no verbatim key can reach ------------- */

static void test_fold_namespace_disjoint(void) {
    /* The alphabet a room name is filtered through can never emit the fold
     * separator. That is the whole guarantee — everything below is the
     * consequence, not an independent hope. */
    char probe[CONV_SESSION_LEN];
    char sep_in[8];
    snprintf(sep_in, sizeof(sep_in), "a%cb", CONV_FOLD_SEP);
    agents_route_sanitize(sep_in, probe, sizeof(probe));
    assert(strchr(probe, CONV_FOLD_SEP) == NULL);

    /* A room name long enough to force the fold. */
    char longsess[CONV_SESSION_LEN];
    memset(longsess, 'x', sizeof(longsess) - 1);
    longsess[sizeof(longsess) - 1] = '\0';

    char key[CONV_KEY_LEN];
    assert(conv_thread_key(key, sizeof(key), SEED_A, longsess));
    char folded[CONV_PATH_LEN];
    path_of(SEED_A, longsess, folded, sizeof(folded));
    assert(strchr(folded, CONV_FOLD_SEP) != NULL);      /* it really folded */

    /* Now name a room whose VERBATIM key reproduces, byte for byte, the shortid
     * the fold would have produced if it were introduced by '-' — a character
     * that IS in the room-name alphabet. Room names arrive from the wire
     * (claude_route_incoming), so a peer can choose this name deliberately;
     * before the separator moved outside the alphabet these two rooms shared a
     * single history file. */
    size_t cap = CONV_SHORTID_LEN - 1;
    size_t head = cap - 5;                    /* head + sep + 4 hex */
    char hex[8];
    snprintf(hex, sizeof(hex), "%04x", (unsigned)conv_fp16(key));
    char collide_room[CONV_SESSION_LEN];
    snprintf(collide_room, sizeof(collide_room), "%.*s-%s",
             (int)(head - strlen(SEED_A) - 1), longsess, hex);

    char verbatim[CONV_PATH_LEN];
    path_of(SEED_A, collide_room, verbatim, sizeof(verbatim));
    assert(strchr(verbatim, CONV_FOLD_SEP) == NULL);    /* it did NOT fold */

    /* Proof the case is still REACHED: this verbatim path is exactly the string
     * the '-' fold used to emit. If a later change stops the two lining up, this
     * fails loudly instead of quietly passing on a case that no longer collides. */
    char old_form[CONV_PATH_LEN + 8];
    snprintf(old_form, sizeof(old_form), "%s%.*s-%s", CONV_PATH_PREFIX,
             (int)head, key, hex);
    assert(strcmp(old_form, verbatim) == 0);

    /* ... and the two rooms must nonetheless land on different files. */
    assert(strcmp(verbatim, folded) != 0);
    assert_path_sane(verbatim);
    assert_path_sane(folded);
}

/* --- 3. keys that must be refused ------------------------------------------- */

static void test_key_rejects(void) {
    char key[CONV_KEY_LEN];
    char p[CONV_PATH_LEN];
    /* '.' splits the key back apart on load, so it cannot live in an id. */
    assert(!conv_thread_key(key, sizeof(key), "cla.ude", "room"));
    assert(!conv_log_path(p, sizeof(p), "cla.ude", "room"));
    /* An id that sanitises to nothing is not a conversation. */
    assert(!conv_thread_key(key, sizeof(key), "***", "room"));
    assert(!conv_thread_key(key, sizeof(key), "", "room"));
    assert(!conv_thread_key(key, sizeof(key), NULL, "room"));
    /* A room name that sanitises to nothing degrades to the bare id, not to a
     * dangling separator. */
    assert(conv_thread_key(key, sizeof(key), SEED_A, "***"));
    assert(strcmp(key, SEED_A) == 0);
}

/* --- 4. record round-trip: transport + opaque reply address ------------------ */

static void roundtrip(const char *conv, const char *session, const char *label,
                      uint8_t transport, const uint8_t *reply, uint8_t reply_len,
                      uint8_t dead) {
    char line[CONV_MANIFEST_LINE_LEN];
    assert(conv_manifest_format(line, sizeof(line), conv, session, label,
                                transport, reply, reply_len, dead));
    assert(strchr(line, '\n') == NULL);   /* the caller joins, not the codec */

    ConvManifestLine got;
    assert(conv_manifest_parse(line, &got));
    assert(strcmp(got.conv, conv) == 0);
    assert(strcmp(got.session, session ? session : "") == 0);
    assert(strcmp(got.label, label) == 0);
    assert(got.transport == transport);
    assert(got.reply_len == reply_len);
    if (reply_len) assert(memcmp(got.reply, reply, reply_len) == 0);
    assert(got.dead == dead);

    /* The parsed key is the one the path is built from. */
    char key[CONV_KEY_LEN];
    assert(conv_thread_key(key, sizeof(key), conv, session));
    assert(strcmp(got.key, key) == 0);
}

static void test_record_roundtrip(void) {
    /* CONV_AGENT: the return address is the agent id's own bytes. */
    const uint8_t agent_addr[] = {'c', 'l', 'a', 'u', 'd', 'e'};
    roundtrip(SEED_A, SEED_ROOM, "CLAUDE", (uint8_t)CONV_AGENT, agent_addr,
              (uint8_t)sizeof(agent_addr), 0);

    /* CONV_LXMF: a 16-byte source hash, on a room marked dead. */
    uint8_t lxmf[16];
    for (size_t i = 0; i < sizeof(lxmf); i++) lxmf[i] = (uint8_t)(0xF0 - i);
    roundtrip(SEED_A, "work", "lxmf sender", (uint8_t)CONV_LXMF, lxmf,
              (uint8_t)sizeof(lxmf), 1);

    /* CONV_MESH: the widest address we carry, on a room-less peer thread. */
    uint8_t pub[CONV_REPLY_MAX];
    for (size_t i = 0; i < sizeof(pub); i++) pub[i] = (uint8_t)(i * 7 + 1);
    roundtrip(PEER_ID, "", "peer a1b2c3d4", (uint8_t)CONV_MESH, pub,
              (uint8_t)sizeof(pub), 0);

    /* An empty return address is legal (nothing to answer yet). */
    roundtrip(SEED_B, SEED_ROOM, "HERMES", (uint8_t)CONV_AGENT, NULL, 0, 0);
}

/* --- 5. legacy manifest lines still load ------------------------------------ */

static void test_legacy_lines(void) {
    ConvManifestLine got;

    /* Two fields: the oldest manifest — agent, room, nothing else. */
    assert(conv_manifest_parse("claude\tpager-abcd", &got));
    assert(strcmp(got.conv, SEED_A) == 0);
    assert(strcmp(got.session, SEED_ROOM) == 0);
    assert(got.transport == (uint8_t)CONV_AGENT);
    assert(got.dead == 0);
    assert(strcmp(got.key, "claude.pager-abcd") == 0);
    /* Promoted losslessly: the agent id IS the reply address for that wire. */
    assert(got.reply_len == strlen(SEED_A));
    assert(memcmp(got.reply, SEED_A, got.reply_len) == 0);

    /* Three fields: the roster dead flag was appended later. */
    assert(conv_manifest_parse("hermes\twork\t1", &got));
    assert(strcmp(got.conv, SEED_B) == 0);
    assert(strcmp(got.session, "work") == 0);
    assert(got.transport == (uint8_t)CONV_AGENT);
    assert(got.dead == 1);

    assert(conv_manifest_parse("hermes\twork\t0", &got));
    assert(got.dead == 0);

    /* A trailing newline from the line scanner must not leak into a field. */
    assert(conv_manifest_parse("claude\twork\r\n", &got));
    assert(strcmp(got.session, "work") == 0);

    /* A legacy line whose path would have blown the old key budget still loads,
     * and the path built from it fits. */
    assert(conv_manifest_parse("claude\txxxxxxxxxxxxxxxxxxxxxxx", &got));
    char p[CONV_PATH_LEN];
    assert(conv_log_path(p, sizeof(p), got.conv, got.session));
    assert_path_sane(p);
}

/* --- 6. new-form lines parsed straight off disk ------------------------------ */

static void test_new_line_parse(void) {
    ConvManifestLine got;

    assert(conv_manifest_parse("a1b2c3d4\tpeer a1b2c3d4\t2\t0102030405\t0", &got));
    assert(strcmp(got.conv, PEER_ID) == 0);
    assert(got.session[0] == '\0');
    assert(strcmp(got.label, "peer a1b2c3d4") == 0);
    assert(got.transport == (uint8_t)CONV_MESH);
    assert(got.reply_len == 5);
    assert(got.reply[0] == 0x01 && got.reply[4] == 0x05);
    assert(got.dead == 0);

    /* The key splits at the FIRST '.', so a room name may contain dots. */
    assert(conv_manifest_parse("claude.pager.room\tCLAUDE\t0\t\t1", &got));
    assert(strcmp(got.conv, SEED_A) == 0);
    assert(strcmp(got.session, "pager.room") == 0);
    assert(got.reply_len == 0);
    assert(got.dead == 1);

    /* An unknown future transport degrades to the agent path; the room and its
     * history survive rather than being dropped by an older build. */
    assert(conv_manifest_parse("claude\tCLAUDE\t99\t\t0", &got));
    assert(got.transport == (uint8_t)CONV_AGENT);
    assert(strcmp(got.conv, SEED_A) == 0);
}

/* --- 7. malformed input is refused ------------------------------------------ */

static void test_malformed(void) {
    ConvManifestLine got;
    assert(!conv_manifest_parse("", &got));
    assert(!conv_manifest_parse("\n", &got));
    assert(!conv_manifest_parse("claude", &got));            /* one field  */
    assert(!conv_manifest_parse("claude\tx\tL\t00", &got));  /* four fields */
    assert(!conv_manifest_parse("\tpager", &got));           /* empty id   */
    assert(!conv_manifest_parse("***\tpager", &got));        /* id vanishes */
    assert(!conv_manifest_parse(NULL, &got));

    /* Odd-length / non-hex reply fields decode to nothing rather than to a
     * half-formed return address that would misdeliver a reply. */
    assert(conv_manifest_parse("claude\tL\t1\t0102030\t0", &got));
    assert(got.reply_len == 0);
    assert(conv_manifest_parse("claude\tL\t1\tzz\t0", &got));
    assert(got.reply_len == 0);

    /* A reply address wider than we carry is refused at format time. */
    char line[CONV_MANIFEST_LINE_LEN];
    uint8_t wide[CONV_REPLY_MAX + 1] = {0};
    assert(!conv_manifest_format(line, sizeof(line), SEED_A, "", "L",
                                 (uint8_t)CONV_MESH, wide,
                                 (uint8_t)sizeof(wide), 0));
}

/* --- 7b. a label cannot break the record it lives in ------------------------ */

static void test_label_cannot_forge_a_record(void) {
    /* A label carrying a TAB and a NEWLINE, followed by a complete forged
     * record for ANOTHER conversation. Labels are compile-time today, but the
     * field exists so a later commit can fill it from an LXMF/mesh sender name,
     * i.e. from the wire. */
    const char *hostile = "ev\til\nhermes.work\tX\t2\taabb\t1";
    uint8_t addr[2] = {0xAB, 0xCD};
    char line[CONV_MANIFEST_LINE_LEN];
    assert(conv_manifest_format(line, sizeof(line), SEED_A, "work", hostile,
                                (uint8_t)CONV_LXMF, addr, 2, 0));

    /* One record, five fields: the newline cannot start a second line and the
     * tab cannot shift the columns. */
    assert(strchr(line, '\n') == NULL);
    int tabs = 0;
    for (const char *p = line; *p; p++) if (*p == '\t') tabs++;
    assert(tabs == 4);

    /* Everything after the label still reads from the right column. */
    ConvManifestLine got;
    assert(conv_manifest_parse(line, &got));
    assert(strcmp(got.conv, SEED_A) == 0);
    assert(strcmp(got.session, "work") == 0);
    assert(got.transport == (uint8_t)CONV_LXMF);
    assert(got.reply_len == 2 && got.reply[0] == 0xAB && got.reply[1] == 0xCD);
    assert(got.dead == 0);
    assert(strchr(got.label, '\t') == NULL);
    assert(strchr(got.label, '\n') == NULL);

    /* Same barrier on the way IN — the manifest sits on a card a user can edit,
     * so a control byte can arrive without ever passing through _format. */
    assert(conv_manifest_parse("claude.work\tbad\x01label\t1\t\t0", &got));
    for (const char *p = got.label; *p; p++)
        assert((unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7f);

    /* S4: an extra tab stays INSIDE the last field instead of truncating it,
     * which is what the parse comment has always claimed. */
    assert(conv_manifest_parse("claude.work\tL\t1\t\t1\textra", &got));
    assert(got.dead == 1);
}

/* --- 7c. slot planning: who gets evicted when the table is full ------------- */

static void test_slot_plan(void) {
    /* Two seeded conversations, six free slots: a new one appends. */
    uint8_t seeded[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    uint32_t use[8]   = {50, 51, 0, 0, 0, 0, 0, 0};
    assert(conv_slot_plan(2, 8, seeded, use, -1, true) == 2);
    assert(conv_slot_plan(7, 8, seeded, use, -1, true) == 7);

    /* Full table: the least-recently-used NON-seeded slot goes. Slot 1 is the
     * globally oldest but seeded, so it must survive; slot 5 is the oldest peer. */
    uint32_t full_use[8] = {90, 1, 40, 70, 60, 20, 30, 80};
    uint8_t full_seed[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    int victim = conv_slot_plan(8, 8, full_seed, full_use, -1, true);
    assert(victim == 5);
    assert(full_seed[victim] == 0);          /* never a seeded slot */

    /* Make that peer the most recent and the choice must move on — otherwise
     * the "least recently used" claim is just the lowest index in disguise. */
    full_use[5] = 99;
    assert(conv_slot_plan(8, 8, full_seed, full_use, -1, true) == 6);

    /* Ties resolve to the lowest index, deterministically. */
    uint32_t tie_use[8] = {9, 9, 5, 5, 5, 5, 5, 5};
    assert(conv_slot_plan(8, 8, full_seed, tie_use, -1, true) == 2);
    assert(conv_slot_plan(8, 8, full_seed, tie_use, -1, true) == 2);

    /* A table of nothing but seeded conversations refuses rather than throwing
     * one of the device's own doors away. */
    uint8_t all_seeded[4] = {1, 1, 1, 1};
    uint32_t any_use[4] = {1, 2, 3, 4};
    assert(conv_slot_plan(4, 4, all_seeded, any_use, -1, true) == -1);

    /* Degenerate inputs are refused, not guessed at. */
    assert(conv_slot_plan(0, 0, seeded, use, -1, true) == -1);
    assert(conv_slot_plan(8, 8, NULL, NULL, -1, true) == -1);

    /* THE CONVERSATION ON SCREEN IS NOT A VICTIM. Recycling its slot does not
     * close that chat — it replaces it, leaving the reader in a stranger's
     * conversation under a stranger's name, mid-scroll, with nothing to notice.
     * Slot 5 is the LRU peer, so protecting it must move the choice on.
     * (The LRU case above made slot 5 the most recent; put it back, and assert
     * the unprotected answer first so the protected one is a real change.) */
    full_use[5] = 20;
    assert(conv_slot_plan(8, 8, full_seed, full_use, -1, true) == 5);
    assert(conv_slot_plan(8, 8, full_seed, full_use, 5, true) == 6);
    /* Protecting a slot that was never the victim changes nothing. */
    assert(conv_slot_plan(8, 8, full_seed, full_use, 7, true) == 5);
    /* Protecting a seeded slot is harmless (it was already excluded). */
    assert(conv_slot_plan(8, 8, full_seed, full_use, 0, true) == 5);
    /* Every non-seeded slot unavailable => refuse rather than evict the reader. */
    uint8_t one_peer[3] = {1, 1, 0};
    uint32_t one_use[3] = {5, 6, 7};
    assert(conv_slot_plan(3, 3, one_peer, one_use, -1, true) == 2);
    assert(conv_slot_plan(3, 3, one_peer, one_use, 2, true) == -1);
    /* An out-of-range protect index must not silently protect a real slot. */
    assert(conv_slot_plan(8, 8, full_seed, full_use, 99, true) == 5);
}

/* --- 7d. a minted peer survives a reboot with its return address ------------ */

static void test_peer_roundtrip(void) {
    /* The shape a mesh peer gets: id from its address, no rooms, a 32-byte key.
     * This is the whole point of the room-less manifest line — without it the
     * store could describe a peer on disk and forget it on the next boot. */
    uint8_t key[CONV_REPLY_MAX];
    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i * 3 + 5);

    char line[CONV_MANIFEST_LINE_LEN];
    assert(conv_manifest_format(line, sizeof(line), PEER_ID, "", "night walk",
                                (uint8_t)CONV_MESH, key, (uint8_t)sizeof(key), 0));

    ConvManifestLine got;
    assert(conv_manifest_parse(line, &got));
    assert(strcmp(got.conv, PEER_ID) == 0);
    assert(got.session[0] == '\0');            /* room-less, as minted */
    assert(strcmp(got.label, "night walk") == 0);
    assert(got.transport == (uint8_t)CONV_MESH);
    assert(got.reply_len == CONV_REPLY_MAX);
    assert(memcmp(got.reply, key, sizeof(key)) == 0);

    /* The log it reopens after the reboot is the same file it wrote before —
     * which is what makes evicting its RAM slot lossless. */
    char p1[CONV_PATH_LEN], p2[CONV_PATH_LEN];
    path_of(PEER_ID, "", p1, sizeof(p1));
    path_of(got.conv, got.session, p2, sizeof(p2));
    assert(strcmp(p1, p2) == 0);
    assert_path_sane(p2);
}

/* --- 7e. a peer survives a reboot with its route intact --------------------- */

/* The whole point of persisting a peer: the device is rebooted constantly, and
 * a conversation that cannot survive a battery change is not a conversation.
 * This walks the real round trip — write the record, lose everything, read it
 * back — and checks the bytes that decide where a reply goes. */
static void test_peer_survives_reboot(void) {
    struct Peer { const char *id; const char *label; uint8_t transport; uint8_t len; };
    const Peer peers[2] = {
        { "a1b2c3d4d5", "night walk", (uint8_t)CONV_MESH, 32 },
        { "f0e1d2c3b4", "",           (uint8_t)CONV_LXMF, 16 },
    };
    for (int k = 0; k < 2; k++) {
        uint8_t addr[CONV_REPLY_MAX];
        for (size_t i = 0; i < peers[k].len; i++) addr[i] = (uint8_t)(k * 91 + i * 7 + 3);

        /* --- before the reboot: the record as the writer emits it --- */
        char line[CONV_MANIFEST_LINE_LEN];
        assert(conv_manifest_format(line, sizeof(line), peers[k].id, "",
                                    peers[k].label, peers[k].transport,
                                    addr, peers[k].len, 0));
        char before[CONV_PATH_LEN];
        path_of(peers[k].id, "", before, sizeof(before));

        /* --- the reboot: nothing survives but that line --- */
        ConvManifestLine got;
        assert(conv_manifest_parse(line, &got));

        /* --- after: the conversation is the same one, and answerable --- */
        assert(strcmp(got.conv, peers[k].id) == 0);
        assert(got.session[0] == '\0');            /* still room-less */
        assert(got.transport == peers[k].transport); /* same wire */
        assert(got.reply_len == peers[k].len);       /* same address width */
        assert(memcmp(got.reply, addr, peers[k].len) == 0);  /* SAME ADDRESS */
        assert(strcmp(got.label, peers[k].label) == 0);
        /* ... and it reopens the history it already had, rather than a new file */
        char after[CONV_PATH_LEN];
        path_of(got.conv, got.session, after, sizeof(after));
        assert(strcmp(before, after) == 0);
        assert_path_sane(after);

        /* The restored record must route to the backend it came in on — a
         * reply that went out on the wrong wire would reach nobody. */
        uint8_t backend = 0xFF;
        const uint8_t *out = NULL;
        uint8_t out_len = 0;
        TransportTarget tgt;
        tgt.transport = got.transport;
        tgt.reply_addr = got.reply;
        tgt.reply_len = got.reply_len;
        assert(transport_plan(&tgt, &backend, &out, &out_len) == TRANSPORT_OK);
        assert(backend == (peers[k].transport == CONV_MESH
                               ? TRANSPORT_BACKEND_MESH_PEER
                               : TRANSPORT_BACKEND_LXMF));
        assert(out_len == peers[k].len);
        assert(memcmp(out, addr, peers[k].len) == 0);
    }
}

/* --- 8. the manifest file names moved, and the old one is only a reader ------ */

static void test_manifest_names(void) {
    assert(strcmp(CONV_MANIFEST, "/conversations.txt") == 0);
    assert(strcmp(CONV_MANIFEST_LEGACY, "/agents_sessions.txt") == 0);
    assert(strcmp(CONV_MANIFEST, CONV_MANIFEST_LEGACY) != 0);
}

int main(void) {
    test_path_form();
    test_path_budget();
    test_fold_namespace_disjoint();
    test_key_rejects();
    test_record_roundtrip();
    test_legacy_lines();
    test_new_line_parse();
    test_malformed();
    test_label_cannot_forge_a_record();
    test_slot_plan();
    test_peer_roundtrip();
    test_peer_survives_reboot();
    test_manifest_names();
    printf("conversation store tests: OK\n");
    return 0;
}
