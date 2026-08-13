/*
 * Host tests for the MeshCore /contacts3 record codec (src/mesh/contact_record.h).
 *
 * This is a PORT of an external on-disk format, not a design of ours: a card
 * written here should be readable by stock MeshCore and the reverse. So the
 * tests check the BYTES — offsets and widths against the transcribed field
 * order — rather than only that a struct survives a round trip, because a
 * self-consistent codec with the wrong layout would pass a round-trip test and
 * mis-parse every real card.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/mesh/contact_record.h"
#include "../src/mesh/contacts_enum.h"   /* pure mesh_contacts_filter selection */

static MeshContactRec sample(void) {
    MeshContactRec c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < MESH_CONTACT_PUBKEY_LEN; i++) c.pub_key[i] = (uint8_t)(0x40 + i);
    snprintf(c.name, sizeof(c.name), "night walk");
    c.type = 1;
    c.flags = 0x05;
    c.sync_since = 0x11223344;
    c.out_path_len = 3;
    c.last_advert_timestamp = 0xAABBCCDD;
    for (int i = 0; i < MESH_CONTACT_PATH_LEN; i++) c.out_path[i] = (uint8_t)(i * 3 + 1);
    c.lastmod = 0x01020304;
    c.gps_lat = -501234567;
    c.gps_lon = 141234567;
    return c;
}

/* --- 1. the record is exactly the size the format fixes -------------------- */

static void test_record_size(void) {
    /* 32+32+1+1+1+4+1+4+64+4+4+4. The older /contacts layout is 140 and lacks
     * lastmod/gps — porting that one would shift every record after the first. */
    assert(MESH_CONTACT_REC_LEN == 152);
    assert(MESH_CONTACT_REC_LEN != 140);
}

/* --- 2. every field lands at the offset the format says -------------------- */

static void test_field_offsets(void) {
    MeshContactRec c = sample();
    uint8_t b[MESH_CONTACT_REC_LEN];
    memset(b, 0xEE, sizeof(b));
    mesh_contact_pack(&c, b);

    assert(memcmp(b + 0, c.pub_key, 32) == 0);
    assert(memcmp(b + 32, "night walk", 10) == 0);
    assert(b[64] == 1);           /* type   */
    assert(b[65] == 0x05);        /* flags  */
    assert(b[66] == 0);           /* unused, written as 0 like upstream */
    /* sync_since, little-endian — the field that used to be `reserved` */
    assert(b[67] == 0x44 && b[68] == 0x33 && b[69] == 0x22 && b[70] == 0x11);
    assert(b[71] == 3);           /* out_path_len */
    assert(b[72] == 0xDD && b[73] == 0xCC && b[74] == 0xBB && b[75] == 0xAA);
    assert(memcmp(b + 76, c.out_path, 64) == 0);
    assert(b[140] == 0x04 && b[141] == 0x03 && b[142] == 0x02 && b[143] == 0x01);
    /* gps_lat/gps_lon are the two fields the 140-byte layout does not have */
    assert(mesh_contact_get_u32(b + 144) == (uint32_t)c.gps_lat);
    assert(mesh_contact_get_u32(b + 148) == (uint32_t)c.gps_lon);
}

/* --- 3. a contact survives being written and read back --------------------- */

static void test_roundtrip(void) {
    MeshContactRec c = sample();
    uint8_t b[MESH_CONTACT_REC_LEN];
    mesh_contact_pack(&c, b);

    MeshContactRec got;
    memset(&got, 0xAA, sizeof(got));
    mesh_contact_unpack(b, &got);

    /* The pubkey is the identity: without it the contact addresses nobody. */
    assert(memcmp(got.pub_key, c.pub_key, MESH_CONTACT_PUBKEY_LEN) == 0);
    /* The path is what lets the radio actually reach them. */
    assert(got.out_path_len == c.out_path_len);
    assert(memcmp(got.out_path, c.out_path, MESH_CONTACT_PATH_LEN) == 0);
    assert(got.flags == c.flags);
    assert(got.type == c.type);
    assert(strcmp(got.name, "night walk") == 0);
    assert(got.sync_since == c.sync_since);
    assert(got.last_advert_timestamp == c.last_advert_timestamp);
    assert(got.lastmod == c.lastmod);
    /* Signed coordinates must come back signed, not as huge positives. */
    assert(got.gps_lat == c.gps_lat && got.gps_lat < 0);
    assert(got.gps_lon == c.gps_lon && got.gps_lon > 0);
}

/* --- 4. several records pack end to end with no gaps ----------------------- */

static void test_stream(void) {
    /* The file has no header, no count and no separator: a flat run of fixed
     * records, and the reader stops at the first short read. If a record were
     * a byte off, contact N+1 would be garbage while contact N looked fine. */
    uint8_t file[MESH_CONTACT_REC_LEN * 3];
    MeshContactRec in[3];
    for (int k = 0; k < 3; k++) {
        in[k] = sample();
        in[k].pub_key[0] = (uint8_t)(0xF0 + k);
        in[k].lastmod = (uint32_t)(1000 + k);
        snprintf(in[k].name, sizeof(in[k].name), "peer-%d", k);
        mesh_contact_pack(&in[k], file + k * MESH_CONTACT_REC_LEN);
    }
    for (int k = 0; k < 3; k++) {
        MeshContactRec got;
        mesh_contact_unpack(file + k * MESH_CONTACT_REC_LEN, &got);
        assert(got.pub_key[0] == (uint8_t)(0xF0 + k));
        assert(got.lastmod == (uint32_t)(1000 + k));
        char want[16];
        snprintf(want, sizeof(want), "peer-%d", k);
        assert(strcmp(got.name, want) == 0);
    }
}

/* --- 5. hostile / edge input ----------------------------------------------- */

static void test_edges(void) {
    /* A name filling all 32 bytes with no terminator must not run off the end
     * when something downstream treats it as a C string. */
    MeshContactRec c = sample();
    memset(c.name, 'q', sizeof(c.name));
    uint8_t b[MESH_CONTACT_REC_LEN];
    mesh_contact_pack(&c, b);
    MeshContactRec got;
    mesh_contact_unpack(b, &got);
    assert(strlen(got.name) == MESH_CONTACT_NAME_LEN - 1);

    /* Upstream's save filter: an anonymous/transient contact is not stored. */
    assert(!mesh_contact_worth_saving(0));   /* ADV_TYPE_NONE */
    assert(mesh_contact_worth_saving(1));
    assert(mesh_contact_worth_saving(2));

    /* NULL-safe both ways. */
    mesh_contact_pack(NULL, b);
    mesh_contact_pack(&c, NULL);
    mesh_contact_unpack(NULL, &got);
    mesh_contact_unpack(b, NULL);
}

/* --- 6. the UI enumerator's pure selection over a flat run ----------------- */

static void test_enum_filter(void) {
    /* Five records on disk, two of them anonymous (type 0 — not contacts). The
     * enumerator keeps only the worth-saving ones, in file order, so the mesh
     * bucket shows peers this device actually met, not adverts it discarded. */
    uint8_t file[MESH_CONTACT_REC_LEN * 5];
    uint8_t types[5] = { 1, 0, 2, 0, 1 };
    for (int k = 0; k < 5; k++) {
        MeshContactRec c = sample();
        c.type = types[k];
        c.pub_key[0] = (uint8_t)(0x50 + k);
        snprintf(c.name, sizeof(c.name), "rec-%d", k);
        mesh_contact_pack(&c, file + k * MESH_CONTACT_REC_LEN);
    }
    MeshContactRec got[5];
    int n = mesh_contacts_filter(file, sizeof(file), got, 5);
    assert(n == 3);                                  /* records 0, 2, 4 */
    assert(strcmp(got[0].name, "rec-0") == 0 && got[0].type == 1);
    assert(strcmp(got[1].name, "rec-2") == 0 && got[1].type == 2);
    assert(strcmp(got[2].name, "rec-4") == 0 && got[2].type == 1);

    /* max bounds the copy: only the first two worth-saving records. */
    assert(mesh_contacts_filter(file, sizeof(file), got, 2) == 2);
    assert(strcmp(got[1].name, "rec-2") == 0);

    /* A trailing partial record is ignored, exactly as the reader stops at the
     * first short read: three whole records (types 1,0,2) plus 7 stray bytes
     * yields the two worth-saving ones, and never touches the partial. */
    assert(mesh_contacts_filter(file, MESH_CONTACT_REC_LEN * 3 + 7, got, 5) == 2);
    assert(strcmp(got[1].name, "rec-2") == 0);

    /* Fewer bytes than one record: nothing. */
    assert(mesh_contacts_filter(file, MESH_CONTACT_REC_LEN - 1, got, 5) == 0);

    /* NULL / zero-max are safe and select nothing. */
    assert(mesh_contacts_filter(NULL, sizeof(file), got, 5) == 0);
    assert(mesh_contacts_filter(file, sizeof(file), NULL, 5) == 0);
    assert(mesh_contacts_filter(file, sizeof(file), got, 0) == 0);
}

int main(void) {
    test_record_size();
    test_field_offsets();
    test_roundtrip();
    test_stream();
    test_edges();
    test_enum_filter();
    printf("contact record tests: OK\n");
    return 0;
}
