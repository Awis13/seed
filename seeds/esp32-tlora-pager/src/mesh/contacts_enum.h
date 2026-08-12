#pragma once
/*
 * contacts_enum.h — enumerate the persisted MeshCore /contacts3 table for the UI.
 *
 * The Contacts screen's mesh bucket is "peers this device has actually met". The
 * authority on that is /contacts3 — the same file mc_client reads to answer the
 * radio — NOT the radio's in-RAM table, which does not exist until the deferred
 * bring-up runs ~5 s after boot. So this reads the FILE, and needs no radio and
 * no loop: it is a plain filesystem read, safe to call from the draw path.
 *
 * WHY A SEPARATE ACCESSOR AND NOT mc_client's own reader. mc_client_load()
 * parses the same file, but INTO the radio stack's contact table, on the loop
 * task, as part of bring-up — using it from the UI would drag the whole
 * MeshCore stack into the draw path and mutate live radio state to answer a
 * display question. agents.cpp already reads /contacts3 by hand for its
 * "known peers" barrier (agents_known_peers_load), for exactly this reason; this
 * is that same file read, given a clean UI-facing shape.
 *
 * The record layout and the type!=0 save filter are contact_record.h's, already
 * proved in tools/test_contact_record.cpp; the only thing NEW here is walking a
 * flat run of records and keeping the ones worth showing, which is what the pure
 * mesh_contacts_filter() below does and tools/test_contact_record.cpp covers.
 */

#include <stddef.h>
#include <stdint.h>

#include "contact_record.h"   /* MeshContactRec, MESH_CONTACT_REC_LEN, filter */

/*
 * PURE: select the showable contacts out of a flat /contacts3 byte run.
 *
 * `buf`/`n` is a run of fixed MESH_CONTACT_REC_LEN records (the file has no
 * header and no separator). Each is unpacked and kept only if it is worth
 * saving (type != 0 — an anonymous/transient advert is not a contact), filling
 * out[] up to `max`. A trailing partial record (n not a whole multiple of the
 * record length) is ignored, exactly as the on-device reader stops at the first
 * short read. Returns the number of records written to out[].
 *
 * This is the whole selection rule the file reader applies, factored out so it
 * can be tested against a byte layout on the host.
 */
static inline int mesh_contacts_filter(const uint8_t *buf, size_t n,
                                       MeshContactRec *out, int max) {
    if (!buf || !out || max <= 0) return 0;
    int written = 0;
    size_t off = 0;
    while (off + MESH_CONTACT_REC_LEN <= n && written < max) {
        MeshContactRec r;
        mesh_contact_unpack(buf + off, &r);
        if (mesh_contact_worth_saving(r.type)) out[written++] = r;
        off += MESH_CONTACT_REC_LEN;
    }
    return written;
}

/*
 * FS-backed enumeration (firmware only; defined in contacts_enum.cpp).
 *
 * mesh_contacts_list() reads /contacts3 ONCE, fills out[] with up to `max`
 * showable contacts (type != 0), and returns how many. One read is what makes
 * it loop-safe: there is no count-then-fetch window an advert rewrite could
 * change between. mesh_contacts_count() is the same read without the copy, for
 * a caller that only needs the size.
 */
int mesh_contacts_count(void);
int mesh_contacts_list(MeshContactRec *out, int max);
