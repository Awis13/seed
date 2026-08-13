/*
 * contacts_enum.cpp — the filesystem side of the /contacts3 UI enumerator.
 *
 * A plain record-by-record file read: open /contacts3 on the filesystem it
 * lives on, unpack each fixed record, keep the ones worth showing. The record
 * codec and the save filter are contact_record.h's (proved on the host); this
 * file only turns them onto the file. Records are read one at a time into a
 * single record-sized buffer rather than slurping the whole file, so a large
 * table costs no burst of RAM on the loop task.
 *
 * mesh_contacts_fs() is forward-declared, not pulled in through mc_client.h: the
 * enumerator needs exactly one symbol — WHICH filesystem the table lives on —
 * and nothing else the MeshCore client header carries. Naming just that symbol
 * keeps this a pure file read with no dependency on the radio stack, the same
 * hand-declared seam agents.cpp uses (see the note near agents.cpp:252).
 */

#include <FS.h>

#include "contact_record.h"
#include "contacts_enum.h"

/* THE filesystem /contacts3 lives on (SPIFFS today; see mc_client.cpp). One
 * symbol, hand-declared, so this file never sees the radio stack. */
namespace fs { class FS; }
fs::FS &mesh_contacts_fs();

/* Read one record at a time; apply the visitor to each worth-saving contact.
 * Stops at the first short read, exactly as the on-device reader does. */
static int mesh_contacts_scan(MeshContactRec *out, int max, bool count_only) {
    fs::FS &cfs = mesh_contacts_fs();
    if (!cfs.exists(MESH_CONTACTS_PATH)) return 0;
    File f = cfs.open(MESH_CONTACTS_PATH, "r");
    if (!f) return 0;
    int found = 0;
    uint8_t buf[MESH_CONTACT_REC_LEN];
    while (f.read(buf, sizeof(buf)) == (int)sizeof(buf)) {
        MeshContactRec r;
        mesh_contact_unpack(buf, &r);
        if (!mesh_contact_worth_saving(r.type)) continue;
        if (!count_only) {
            if (found >= max) break;
            out[found] = r;
        }
        found++;
    }
    f.close();
    return found;
}

int mesh_contacts_count(void) {
    return mesh_contacts_scan(nullptr, 0, true);
}

int mesh_contacts_list(MeshContactRec *out, int max) {
    if (!out || max <= 0) return 0;
    return mesh_contacts_scan(out, max, false);
}
