#pragma once
/*
 * contact_record.h — the MeshCore /contacts3 on-disk record (pure).
 *
 * WHY THIS IS A PORT AND NOT A DESIGN. The contact table is what turns a stored
 * public key into something the radio can actually send to, and it lived only
 * in RAM: rebuilt from adverts on every boot, so a peer whose conversation
 * survived a restart still could not be answered until it advertised again.
 * Persisting it is what closes that. The format is NOT ours to choose — a card
 * written by this device should be readable by stock MeshCore and the reverse —
 * so the layout below is transcribed field for field from the companion
 * firmware, examples/companion_radio/DataStore.cpp:
 *
 *     File file = openRead(_getContactsChannelsFS(), "/contacts3");
 *       bool success = (file.read(pub_key, 32) == 32);
 *       success = success && (file.read((uint8_t *)&c.name, 32) == 32);
 *       success = success && (file.read(&c.type, 1) == 1);
 *       success = success && (file.read(&c.flags, 1) == 1);
 *       success = success && (file.read(&unused, 1) == 1);
 *       success = success && (file.read((uint8_t *)&c.sync_since, 4) == 4); // was 'reserved'
 *       success = success && (file.read((uint8_t *)&c.out_path_len, 1) == 1);
 *       success = success && (file.read((uint8_t *)&c.last_advert_timestamp, 4) == 4);
 *       success = success && (file.read(c.out_path, 64) == 64);
 *       success = success && (file.read((uint8_t *)&c.lastmod, 4) == 4);
 *       success = success && (file.read((uint8_t *)&c.gps_lat, 4) == 4);
 *       success = success && (file.read((uint8_t *)&c.gps_lon, 4) == 4);
 *
 * There is no header and no version byte: the file is a flat run of fixed
 * records and the reader stops at the first short read. Note the trap — an
 * OLDER example (examples/simple_secure_chat) writes a 140-byte record to
 * "/contacts" whose 4-byte field is `reserved` and which has no
 * lastmod/gps_lat/gps_lon. Porting that one instead would parse a real card
 * silently wrong, every record after the first shifted by twelve bytes.
 *
 * MULTI-BYTE FIELDS ARE LITTLE-ENDIAN. Upstream writes the raw struct bytes on
 * an ESP32, so the file IS little-endian; this codec says so explicitly rather
 * than depending on the host it was compiled for, which is also what lets the
 * host test read a byte layout instead of its own memory.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 32+32+1+1+1+4+1+4+64+4+4+4 */
#define MESH_CONTACT_REC_LEN 152
#define MESH_CONTACT_PUBKEY_LEN 32
#define MESH_CONTACT_NAME_LEN   32
#define MESH_CONTACT_PATH_LEN   64
#define MESH_CONTACTS_PATH "/contacts3"

/* The persisted subset of ContactInfo. The shared secret is deliberately absent:
 * it is derived from the key pair and upstream does not store it either. */
struct MeshContactRec {
    uint8_t  pub_key[MESH_CONTACT_PUBKEY_LEN];
    char     name[MESH_CONTACT_NAME_LEN];
    uint8_t  type;
    uint8_t  flags;
    uint32_t sync_since;
    uint8_t  out_path_len;
    uint32_t last_advert_timestamp;
    uint8_t  out_path[MESH_CONTACT_PATH_LEN];
    uint32_t lastmod;
    int32_t  gps_lat;
    int32_t  gps_lon;
};

static inline void mesh_contact_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t mesh_contact_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Serialise one record into exactly MESH_CONTACT_REC_LEN bytes. `unused` is
 * written as 0, which is what upstream writes. */
static inline void mesh_contact_pack(const MeshContactRec *c, uint8_t *out) {
    if (!c || !out) return;
    size_t o = 0;
    memcpy(out + o, c->pub_key, MESH_CONTACT_PUBKEY_LEN); o += MESH_CONTACT_PUBKEY_LEN;
    memcpy(out + o, c->name, MESH_CONTACT_NAME_LEN);      o += MESH_CONTACT_NAME_LEN;
    out[o++] = c->type;
    out[o++] = c->flags;
    out[o++] = 0;                                   /* `unused` */
    mesh_contact_put_u32(out + o, c->sync_since);   o += 4;
    out[o++] = c->out_path_len;
    mesh_contact_put_u32(out + o, c->last_advert_timestamp); o += 4;
    memcpy(out + o, c->out_path, MESH_CONTACT_PATH_LEN);     o += MESH_CONTACT_PATH_LEN;
    mesh_contact_put_u32(out + o, c->lastmod);      o += 4;
    mesh_contact_put_u32(out + o, (uint32_t)c->gps_lat); o += 4;
    mesh_contact_put_u32(out + o, (uint32_t)c->gps_lon); o += 4;
}

/* Read one record back. The name is NUL-terminated defensively: it comes off a
 * card and nothing guarantees a terminator inside its 32 bytes, and everything
 * downstream treats it as a C string. */
static inline void mesh_contact_unpack(const uint8_t *in, MeshContactRec *c) {
    if (!in || !c) return;
    size_t o = 0;
    memcpy(c->pub_key, in + o, MESH_CONTACT_PUBKEY_LEN); o += MESH_CONTACT_PUBKEY_LEN;
    memcpy(c->name, in + o, MESH_CONTACT_NAME_LEN);      o += MESH_CONTACT_NAME_LEN;
    c->name[MESH_CONTACT_NAME_LEN - 1] = '\0';
    c->type = in[o++];
    c->flags = in[o++];
    o++;                                            /* `unused` */
    c->sync_since = mesh_contact_get_u32(in + o);   o += 4;
    c->out_path_len = in[o++];
    c->last_advert_timestamp = mesh_contact_get_u32(in + o); o += 4;
    memcpy(c->out_path, in + o, MESH_CONTACT_PATH_LEN);      o += MESH_CONTACT_PATH_LEN;
    c->lastmod = mesh_contact_get_u32(in + o);      o += 4;
    c->gps_lat = (int32_t)mesh_contact_get_u32(in + o); o += 4;
    c->gps_lon = (int32_t)mesh_contact_get_u32(in + o); o += 4;
}

/* Upstream's save_filter: a contact with no advert type is a transient or
 * anonymous entry and is not worth a slot on disk. */
static inline bool mesh_contact_worth_saving(uint8_t type) {
    return type != 0;   /* ADV_TYPE_NONE */
}
