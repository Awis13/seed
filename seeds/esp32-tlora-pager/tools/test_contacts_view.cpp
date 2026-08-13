/*
 * Host tests for the grouped Contacts model in src/contacts_view.h.
 *
 * Drawing needs the panel and eyes; SECTIONING and ORDER are decisions, and
 * they are exactly what a screenshot cannot confirm once there are three
 * sections and a dozen rows. So these check the row list BY VALUE: which bucket
 * each row belongs to and in what order, that a contact you already chat with
 * sorts above one you do not and the rest sort by name, that an empty section
 * grows no header, that the cap truncates without stranding a lone header, and
 * that each contact row carries the full self-contained handle a later
 * open-or-create needs (id + transport + a copy of the reply address).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/contacts_view.h"

static ContactCandidate mk(const char *label, const char *id, uint8_t transport,
                           uint8_t has_conv, const uint8_t *reply,
                           uint8_t reply_len) {
    ContactCandidate c;
    c.label = label;
    c.id = id;
    c.transport = transport;
    c.has_conversation = has_conv;
    c.reply = reply;
    c.reply_len = reply_len;
    return c;
}

/* --- 1. sections come out AI, LXMF, mesh, each a header then its contacts --- */

static void test_section_order(void) {
    ContactCandidate ai[1]   = { mk("CLAUDE",  "claude",   CONV_AGENT, 1, NULL, 0) };
    ContactCandidate lxmf[1] = { mk("reticu",  "aabbccdd", CONV_LXMF,  0, NULL, 0) };
    ContactCandidate mesh[1] = { mk("walk",    "1122334455", CONV_MESH, 0, NULL, 0) };
    ContactRow out[CONTACT_ROWS_MAX];
    int n = contacts_build_rows(ai, 1, lxmf, 1, mesh, 1, out, CONTACT_ROWS_MAX);
    assert(n == 6);   /* three headers, three contacts */

    assert(out[0].kind == CONTACT_ROW_HEADER && out[0].bucket == CONTACT_BUCKET_AI);
    assert(strcmp(out[0].label, CONTACT_TITLE_AI) == 0);
    assert(out[1].kind == CONTACT_ROW_CONTACT && out[1].bucket == CONTACT_BUCKET_AI);
    assert(strcmp(out[1].label, "CLAUDE") == 0);

    assert(out[2].kind == CONTACT_ROW_HEADER && out[2].bucket == CONTACT_BUCKET_LXMF);
    assert(strcmp(out[2].label, CONTACT_TITLE_LXMF) == 0);
    assert(out[3].kind == CONTACT_ROW_CONTACT && out[3].bucket == CONTACT_BUCKET_LXMF);

    assert(out[4].kind == CONTACT_ROW_HEADER && out[4].bucket == CONTACT_BUCKET_MESH);
    assert(strcmp(out[4].label, CONTACT_TITLE_MESH) == 0);
    assert(out[5].kind == CONTACT_ROW_CONTACT && out[5].bucket == CONTACT_BUCKET_MESH);

    /* The order is the bucket VALUE order, not the argument order — proven by
     * the three titles landing in AI, LXMF, MESH sequence above. */
    assert(CONTACT_BUCKET_AI < CONTACT_BUCKET_LXMF);
    assert(CONTACT_BUCKET_LXMF < CONTACT_BUCKET_MESH);
}

/* --- 2. within a section: has_conversation first, then by name ------------- */

static void test_within_section_order(void) {
    /* Deliberately fed out of order and with a name that would sort first but
     * no conversation, to prove has_conversation is the PRIMARY key. */
    ContactCandidate mesh[4] = {
        mk("zeta",  "z", CONV_MESH, 0, NULL, 0),
        mk("alpha", "a", CONV_MESH, 0, NULL, 0),   /* first by name, but no conv */
        mk("yolo",  "y", CONV_MESH, 1, NULL, 0),   /* has conv -> jumps the queue */
        mk("beta",  "b", CONV_MESH, 1, NULL, 0),   /* has conv, sorts before yolo */
    };
    ContactRow out[CONTACT_ROWS_MAX];
    int n = contacts_build_rows(NULL, 0, NULL, 0, mesh, 4, out, CONTACT_ROWS_MAX);
    assert(n == 5);   /* one header + four contacts */
    assert(out[0].kind == CONTACT_ROW_HEADER);
    /* has_conversation block first, name-ascending inside it: beta, yolo. */
    assert(strcmp(out[1].label, "beta") == 0 && out[1].has_conversation == 1);
    assert(strcmp(out[2].label, "yolo") == 0 && out[2].has_conversation == 1);
    /* then the rest, name-ascending: alpha, zeta. */
    assert(strcmp(out[3].label, "alpha") == 0 && out[3].has_conversation == 0);
    assert(strcmp(out[4].label, "zeta") == 0 && out[4].has_conversation == 0);
}

static void test_ties_keep_input_order(void) {
    /* Same name, same has_conversation: input order must survive, or the list
     * reshuffles between two identical redraws. Distinguish them by id. */
    ContactCandidate ai[3] = {
        mk("dup", "first",  CONV_AGENT, 1, NULL, 0),
        mk("dup", "second", CONV_AGENT, 1, NULL, 0),
        mk("dup", "third",  CONV_AGENT, 1, NULL, 0),
    };
    ContactRow a[CONTACT_ROWS_MAX], b[CONTACT_ROWS_MAX];
    int na = contacts_build_rows(ai, 3, NULL, 0, NULL, 0, a, CONTACT_ROWS_MAX);
    int nb = contacts_build_rows(ai, 3, NULL, 0, NULL, 0, b, CONTACT_ROWS_MAX);
    assert(na == 4 && nb == 4);
    assert(strcmp(a[1].id, "first") == 0);
    assert(strcmp(a[2].id, "second") == 0);
    assert(strcmp(a[3].id, "third") == 0);
    for (int i = 0; i < na; i++) assert(strcmp(a[i].id, b[i].id) == 0);
}

/* --- 3. an empty bucket grows no header ------------------------------------ */

static void test_empty_section_skips_header(void) {
    ContactCandidate ai[1]   = { mk("CLAUDE", "claude", CONV_AGENT, 1, NULL, 0) };
    ContactCandidate mesh[1] = { mk("walk", "1122334455", CONV_MESH, 0, NULL, 0) };
    ContactRow out[CONTACT_ROWS_MAX];
    /* LXMF bucket empty. */
    int n = contacts_build_rows(ai, 1, NULL, 0, mesh, 1, out, CONTACT_ROWS_MAX);
    assert(n == 4);   /* AI header+contact, MESH header+contact — no LXMF */
    for (int i = 0; i < n; i++)
        assert(out[i].bucket != CONTACT_BUCKET_LXMF);
    assert(out[0].bucket == CONTACT_BUCKET_AI);
    assert(out[2].bucket == CONTACT_BUCKET_MESH);

    /* All empty -> nothing at all, not three bare headers. */
    assert(contacts_build_rows(NULL, 0, NULL, 0, NULL, 0, out, CONTACT_ROWS_MAX) == 0);
}

/* --- 4. the row cap: per bucket, per caller-max, and no dangling header ---- */

static void test_caps(void) {
    ContactRow out[CONTACT_ROWS_MAX];

    /* Per-bucket cap: more candidates than CONTACT_BUCKET_CAP -> only the cap
     * many contacts (plus the one header). */
    ContactCandidate many[CONTACT_BUCKET_CAP + 5];
    for (int i = 0; i < CONTACT_BUCKET_CAP + 5; i++) {
        /* Distinct names so ordering is total; none has a conversation. */
        static char names[CONTACT_BUCKET_CAP + 5][8];
        snprintf(names[i], sizeof(names[i]), "n%02d", i);
        many[i] = mk(names[i], names[i], CONV_MESH, 0, NULL, 0);
    }
    int n = contacts_build_rows(NULL, 0, NULL, 0, many, CONTACT_BUCKET_CAP + 5,
                                out, CONTACT_ROWS_MAX);
    assert(n == 1 + CONTACT_BUCKET_CAP);   /* header + capped contacts */

    /* Caller max smaller than needed: bounded to max, header still has a
     * contact under it. */
    ContactCandidate ai[3] = {
        mk("a", "a", CONV_AGENT, 0, NULL, 0),
        mk("b", "b", CONV_AGENT, 0, NULL, 0),
        mk("c", "c", CONV_AGENT, 0, NULL, 0),
    };
    assert(contacts_build_rows(ai, 3, NULL, 0, NULL, 0, out, 2) == 2);
    assert(out[0].kind == CONTACT_ROW_HEADER);
    assert(out[1].kind == CONTACT_ROW_CONTACT);

    /* max == 1: a lone header would be all that fits, so nothing is written —
     * the model never strands a header with no contact under it. */
    assert(contacts_build_rows(ai, 3, NULL, 0, NULL, 0, out, 1) == 0);

    /* Fill every bucket to the cap and ask for more than the ceiling: the
     * result is clamped to CONTACT_ROWS_MAX exactly. */
    static ContactCandidate full[CONTACT_BUCKET_N][CONTACT_BUCKET_CAP];
    static char fnames[CONTACT_BUCKET_N][CONTACT_BUCKET_CAP][8];
    uint8_t tr[CONTACT_BUCKET_N] = { CONV_AGENT, CONV_LXMF, CONV_MESH };
    for (int b = 0; b < CONTACT_BUCKET_N; b++)
        for (int i = 0; i < CONTACT_BUCKET_CAP; i++) {
            snprintf(fnames[b][i], 8, "%d_%02d", b, i);
            full[b][i] = mk(fnames[b][i], fnames[b][i], tr[b], 0, NULL, 0);
        }
    int big = contacts_build_rows(full[0], CONTACT_BUCKET_CAP,
                                  full[1], CONTACT_BUCKET_CAP,
                                  full[2], CONTACT_BUCKET_CAP, out, 100000);
    assert(big == CONTACT_ROWS_MAX);
}

/* --- 5. each contact row carries the full self-contained handle ------------ */

static void test_handle_copied(void) {
    uint8_t pub[32];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x10 + i);
    static const uint8_t agent_id[6] = { 'c','l','a','u','d','e' };

    ContactCandidate ai[1]   = { mk("CLAUDE", "claude", CONV_AGENT, 1, agent_id, 6) };
    ContactCandidate mesh[1] = { mk("walk", "1122334455", CONV_MESH, 1, pub, 32) };
    ContactRow out[CONTACT_ROWS_MAX];
    int n = contacts_build_rows(ai, 1, NULL, 0, mesh, 1, out, CONTACT_ROWS_MAX);
    assert(n == 4);

    /* AI door row: id, transport, has_conversation, agent-id reply. */
    const ContactRow &door = out[1];
    assert(door.transport == CONV_AGENT);
    assert(door.has_conversation == 1);
    assert(strcmp(door.id, "claude") == 0);
    assert(door.reply_len == 6 && memcmp(door.reply, agent_id, 6) == 0);

    /* mesh peer row: the 32-byte public key survives verbatim. */
    const ContactRow &peer = out[3];
    assert(peer.transport == CONV_MESH);
    assert(peer.has_conversation == 1);
    assert(strcmp(peer.id, "1122334455") == 0);
    assert(peer.reply_len == 32 && memcmp(peer.reply, pub, 32) == 0);

    /* A header row carries no handle. */
    assert(out[0].kind == CONTACT_ROW_HEADER);
    assert(out[0].reply_len == 0 && out[0].id[0] == '\0');
}

static void test_handle_edges(void) {
    ContactRow out[CONTACT_ROWS_MAX];

    /* A reply wider than we carry is clamped, not overrun. */
    uint8_t wide[64];
    memset(wide, 0xAB, sizeof(wide));
    ContactCandidate over[1] = { mk("x", "x", CONV_MESH, 0, wide, 64) };
    assert(contacts_build_rows(NULL, 0, NULL, 0, over, 1, out, CONTACT_ROWS_MAX) == 2);
    assert(out[1].reply_len == CONTACT_REPLY_MAX);

    /* NULL reply -> empty return address, not a read of garbage. */
    ContactCandidate none[1] = { mk("y", "y", CONV_LXMF, 0, NULL, 5) };
    assert(contacts_build_rows(NULL, 0, none, 1, NULL, 0, out, CONTACT_ROWS_MAX) == 2);
    assert(out[1].reply_len == 0);

    /* A label / id longer than the row field is cut and stays NUL-terminated. */
    char longlabel[CONTACT_LABEL_LEN * 3];
    memset(longlabel, 'q', sizeof(longlabel) - 1);
    longlabel[sizeof(longlabel) - 1] = '\0';
    char longid[CONTACT_ID_LEN * 3];
    memset(longid, 'z', sizeof(longid) - 1);
    longid[sizeof(longid) - 1] = '\0';
    ContactCandidate big[1] = { mk(longlabel, longid, CONV_AGENT, 0, NULL, 0) };
    assert(contacts_build_rows(big, 1, NULL, 0, NULL, 0, out, CONTACT_ROWS_MAX) == 2);
    assert(strlen(out[1].label) == CONTACT_LABEL_LEN - 1);
    assert(strlen(out[1].id) == CONTACT_ID_LEN - 1);

    /* A NULL label yields an empty one rather than a guess. */
    ContactCandidate noname[1] = { mk(NULL, "id", CONV_MESH, 0, NULL, 0) };
    assert(contacts_build_rows(NULL, 0, NULL, 0, noname, 1, out, CONTACT_ROWS_MAX) == 2);
    assert(out[1].label[0] == '\0');
}

/* --- 6. degenerate input --------------------------------------------------- */

static void test_bounds(void) {
    ContactRow out[CONTACT_ROWS_MAX];
    ContactCandidate one[1] = { mk("x", "x", CONV_AGENT, 0, NULL, 0) };

    assert(contacts_build_rows(one, 1, NULL, 0, NULL, 0, NULL, CONTACT_ROWS_MAX) == 0);
    assert(contacts_build_rows(one, 1, NULL, 0, NULL, 0, out, 0) == 0);
    assert(contacts_build_rows(one, 1, NULL, 0, NULL, 0, out, -3) == 0);

    /* A NULL array with a positive count is treated as empty, not dereferenced. */
    assert(contacts_build_rows(NULL, 5, NULL, 0, NULL, 0, out, CONTACT_ROWS_MAX) == 0);
    /* A negative count is clamped to zero. */
    assert(contacts_build_rows(one, -5, NULL, 0, NULL, 0, out, CONTACT_ROWS_MAX) == 0);
}

/* --- 7. the section-title helper ------------------------------------------- */

static void test_section_title(void) {
    assert(strcmp(contacts_section_title(CONTACT_BUCKET_AI), CONTACT_TITLE_AI) == 0);
    assert(strcmp(contacts_section_title(CONTACT_BUCKET_LXMF), CONTACT_TITLE_LXMF) == 0);
    assert(strcmp(contacts_section_title(CONTACT_BUCKET_MESH), CONTACT_TITLE_MESH) == 0);
    assert(contacts_section_title(99)[0] == '\0');
    /* The three titles must be distinct, or the sections say nothing. */
    assert(strcmp(CONTACT_TITLE_AI, CONTACT_TITLE_LXMF) != 0);
    assert(strcmp(CONTACT_TITLE_LXMF, CONTACT_TITLE_MESH) != 0);
    assert(strcmp(CONTACT_TITLE_AI, CONTACT_TITLE_MESH) != 0);
}

int main(void) {
    test_section_order();
    test_within_section_order();
    test_ties_keep_input_order();
    test_empty_section_skips_header();
    test_caps();
    test_handle_copied();
    test_handle_edges();
    test_bounds();
    test_section_title();
    printf("contacts view tests: OK\n");
    return 0;
}
