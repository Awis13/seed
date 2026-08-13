#pragma once
/*
 * contacts_view.h — the grouped Contacts model (pure, host-testable).
 *
 * The pager is a MESSENGER, and a messenger needs a "who can I write to" screen
 * distinct from the Messages feed (which shows threads that already exist). The
 * Contacts screen answers "start a chat with…": it lists the correspondents the
 * device knows, grouped by the wire they live on — the seeded AI doors, the
 * LXMF peers, and the mesh peers this device has actually met — and picking one
 * opens its thread or creates it. THAT full screen is several commits; this
 * header is only its FOUNDATION: the pure grouped view model and nothing else.
 * There is no menu entry, no panel drawing and no open-or-create wiring here.
 *
 * WHAT IS PURE HERE, AND WHY IT MATTERS. Sectioning and the order within a
 * section are decisions; drawing is not. Which bucket a contact lands in, which
 * empty buckets disappear, whether a contact you already chat with sorts above
 * one you do not, and where the row cap truncates — none of that can be
 * confirmed by a screenshot once there are three sections and a dozen rows. So
 * it is done in a function with no Arduino and no panel, and proved by VALUE in
 * tools/test_contacts_view.cpp. This mirrors feed_view.h / inbox_view.h.
 *
 * THE THREE BUCKETS ARE THREE INPUT ARRAYS, exactly as feed_build_rows() takes
 * cards and conversations separately: the real caller draws the AI doors from
 * the conversation store, the mesh peers from /contacts3 (see
 * mesh/contacts_enum.h), and the LXMF peers from their own roster — three
 * different sources, so one array per source keeps the caller from having to
 * tag-and-merge them just to have this header tag-and-split them again.
 *
 * THE ROW CARRIES A SELF-CONTAINED HANDLE. A chosen contact must be openable
 * after the input arrays are gone, so each contact row copies the id, the
 * transport, and the return address (mesh public key / LXMF source hash / agent
 * id bytes) into itself. Those are exactly what a later open-or-create hands to
 * the conversation store — the same id/transport/reply_addr triple conv_mint()
 * already keys on — so the handle dimensions are taken from conv_store.h rather
 * than re-declared, and cannot drift from it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "conv_store.h"   /* CONV_AGENT/LXMF/MESH, CONV_ID_LEN, CONV_REPLY_MAX */

/* The sections, in the order they are drawn. AI first (the device's own doors,
 * always reachable), then LXMF, then mesh. The values ARE the draw order, so a
 * later section can never sort above an earlier one. Deliberately a separate
 * enum from ConvTransport: a bucket is a display grouping, a transport is the
 * wire a reply leaves on, and although they map one-to-one today the screen's
 * order is its own decision and must not be hostage to the on-disk numbering. */
enum ContactBucket {
    CONTACT_BUCKET_AI   = 0,   /* seeded doors,     transport CONV_AGENT */
    CONTACT_BUCKET_LXMF = 1,   /* Reticulum peers,  transport CONV_LXMF  */
    CONTACT_BUCKET_MESH = 2,   /* MeshCore peers,   transport CONV_MESH  */
};
#define CONTACT_BUCKET_N 3

/* One row is either a section header or a contact. A header carries only its
 * bucket and its title (in `label`); a contact carries the full handle. */
enum ContactRowKind {
    CONTACT_ROW_HEADER  = 0,
    CONTACT_ROW_CONTACT = 1,
};

/* Handle dimensions, borrowed from the store the handle is destined for. The id
 * is the conversation id (a peer's hex prefix, or an agent id like "claude");
 * the reply address is the widest of the three return-address shapes. */
#define CONTACT_ID_LEN     CONV_ID_LEN      /* 12 */
#define CONTACT_REPLY_MAX  CONV_REPLY_MAX   /* 32 */
/* Display name width. Wider than CONV_LABEL_LEN (16) on purpose: a mesh contact
 * name off a card is up to 31 bytes and the contacts list shows it in full; the
 * open-or-create step later truncates to the conversation label. */
#define CONTACT_LABEL_LEN  32

/* Section titles. One short word each — the header row has a whole line. */
#define CONTACT_TITLE_AI    "AI"
#define CONTACT_TITLE_LXMF  "LXMF"
#define CONTACT_TITLE_MESH  "MESH"

/* Candidates considered per bucket. Bounds the internal sort index buffer and
 * caps a single bucket (a big /contacts3) from crowding the others out. */
#define CONTACT_BUCKET_CAP  16
/* The defined ceiling on total rows (headers + contacts). The caller's own
 * `max` may be smaller; the model never writes more than either. */
#define CONTACT_ROWS_MAX    (CONTACT_BUCKET_N + CONTACT_BUCKET_N * CONTACT_BUCKET_CAP)

/* A correspondent the screen could start a chat with. `reply`/`reply_len` are
 * the return address bytes (may be NULL/0 for a door whose id already IS its
 * address); everything else is what the row needs to show and to open. */
struct ContactCandidate {
    const char   *label;            /* display name; NULL -> empty            */
    const char   *id;               /* conversation id / join key; NULL -> "" */
    uint8_t       transport;        /* CONV_AGENT / CONV_LXMF / CONV_MESH     */
    uint8_t       has_conversation; /* already has a thread (0/1)             */
    const uint8_t *reply;           /* return address bytes; may be NULL      */
    uint8_t       reply_len;        /* bytes in reply (0 = unknown)           */
};

/* One drawn row. For a header only `kind`, `bucket` and `label` (the title) are
 * meaningful; for a contact the whole handle is filled and self-contained. */
struct ContactRow {
    uint8_t kind;                       /* ContactRowKind                     */
    uint8_t bucket;                     /* ContactBucket this row belongs to  */
    uint8_t transport;                  /* contact: CONV_ transport to open   */
    uint8_t has_conversation;           /* contact: already has a thread      */
    uint8_t reply_len;                  /* contact: bytes in reply[]          */
    uint8_t reply[CONTACT_REPLY_MAX];   /* contact: copy of the return address */
    char    id[CONTACT_ID_LEN];         /* contact: conversation id / join key */
    char    label[CONTACT_LABEL_LEN];   /* contact: name; header: section title */
};

/* The section title for a bucket, or "" for an out-of-range value. */
static inline const char *contacts_section_title(uint8_t bucket) {
    switch (bucket) {
        case CONTACT_BUCKET_AI:   return CONTACT_TITLE_AI;
        case CONTACT_BUCKET_LXMF: return CONTACT_TITLE_LXMF;
        case CONTACT_BUCKET_MESH: return CONTACT_TITLE_MESH;
        default:                  return "";
    }
}

/* Copy a NUL-terminated field into a fixed row buffer, always terminated. */
static inline void contacts_copy_str(char *dst, size_t dst_n, const char *src) {
    size_t j = 0;
    if (src)
        for (; src[j] && j + 1 < dst_n; j++) dst[j] = src[j];
    dst[j] = '\0';
}

/* Does candidate `a` sort BEFORE candidate `b` within a section?
 *
 * Primary key: a contact you already have a conversation with comes first — the
 * screen is "who can I write to", and the people you are already talking to are
 * the likely target. Secondary key: name, ascending by byte (strcmp), so the
 * rest is browsable rather than in arrival order. Equal on both keys is NOT
 * "before": the caller's input order is then preserved, which keeps two builds
 * of the same list identical and stops the section flickering on redraw. */
static inline bool contacts_cand_before(const ContactCandidate &a,
                                        const ContactCandidate &b) {
    if (a.has_conversation != b.has_conversation)
        return a.has_conversation > b.has_conversation;
    int c = strcmp(a.label ? a.label : "", b.label ? b.label : "");
    return c < 0;
}

/* Emit one contact row from a candidate: copy the label, the id and the return
 * address so the row survives the input array being freed. */
static inline void contacts_emit_contact(const ContactCandidate &c,
                                         uint8_t bucket, ContactRow &r) {
    r.kind = CONTACT_ROW_CONTACT;
    r.bucket = bucket;
    r.transport = c.transport;
    r.has_conversation = c.has_conversation ? 1 : 0;
    r.reply_len = 0;
    memset(r.reply, 0, sizeof(r.reply));
    if (c.reply && c.reply_len) {
        r.reply_len = (c.reply_len > CONTACT_REPLY_MAX) ? CONTACT_REPLY_MAX
                                                        : c.reply_len;
        memcpy(r.reply, c.reply, r.reply_len);
    }
    contacts_copy_str(r.id, sizeof(r.id), c.id);
    contacts_copy_str(r.label, sizeof(r.label), c.label);
}

/* Emit a section header row: only the bucket and its title are meaningful. */
static inline void contacts_emit_header(uint8_t bucket, ContactRow &r) {
    r.kind = CONTACT_ROW_HEADER;
    r.bucket = bucket;
    r.transport = 0;
    r.has_conversation = 0;
    r.reply_len = 0;
    memset(r.reply, 0, sizeof(r.reply));
    r.id[0] = '\0';
    contacts_copy_str(r.label, sizeof(r.label), contacts_section_title(bucket));
}

/* Order one bucket's candidates into `order` (indices into `in`), stable, by
 * contacts_cand_before. At most CONTACT_BUCKET_CAP are taken — in input order,
 * before sorting — so an oversized bucket is bounded, not overrun. Returns the
 * number of indices written. */
static inline int contacts_order_bucket(const ContactCandidate *in, int n,
                                        int *order) {
    if (n < 0) n = 0;
    if (n > CONTACT_BUCKET_CAP) n = CONTACT_BUCKET_CAP;
    int count = 0;
    for (int i = 0; i < n; i++) order[count++] = i;
    /* Stable insertion sort: cur moves ahead only of elements it strictly
     * precedes, so ties keep their input order. */
    for (int i = 1; i < count; i++) {
        int cur = order[i];
        int j = i - 1;
        while (j >= 0 && contacts_cand_before(in[cur], in[order[j]])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }
    return count;
}

/*
 * Build the grouped, sectioned row list.
 *
 * Sections are emitted in bucket order (AI, LXMF, mesh). A bucket with no
 * candidates is skipped ENTIRELY — no header for an empty section, because a
 * lone "MESH" over nothing tells the user the device met a mesh peer when it
 * did not. A header is written only when at least one of its contacts will also
 * fit under it, so the cap never leaves a dangling header: if there is room for
 * a header but not a single contact, the section (and everything after it) is
 * dropped instead.
 *
 * Within a section, contacts that already have a conversation come first, then
 * by name — see contacts_cand_before. The whole result is bounded by the
 * caller's `max` and by CONTACT_ROWS_MAX, and every contact row is
 * self-contained (id, transport and reply address copied in), so the caller may
 * drop the input arrays the moment this returns.
 *
 * Returns the number of rows written.
 */
static inline int contacts_build_rows(const ContactCandidate *ai, int n_ai,
                                      const ContactCandidate *lxmf, int n_lxmf,
                                      const ContactCandidate *mesh, int n_mesh,
                                      ContactRow *out, int max) {
    if (!out || max <= 0) return 0;
    if (max > CONTACT_ROWS_MAX) max = CONTACT_ROWS_MAX;

    const ContactCandidate *arr[CONTACT_BUCKET_N] = { ai, lxmf, mesh };
    int n[CONTACT_BUCKET_N] = { n_ai, n_lxmf, n_mesh };
    for (int b = 0; b < CONTACT_BUCKET_N; b++)
        if (!arr[b]) n[b] = 0;

    int written = 0;
    for (int b = 0; b < CONTACT_BUCKET_N; b++) {
        int order[CONTACT_BUCKET_CAP];
        int nb = contacts_order_bucket(arr[b], n[b], order);
        if (nb == 0) continue;                 /* empty section: no header    */
        /* Room for the header AND at least one contact, or skip the section
         * (and the rest) rather than strand a header with nothing under it. */
        if (written + 1 >= max) break;
        contacts_emit_header((uint8_t)b, out[written++]);
        for (int i = 0; i < nb && written < max; i++)
            contacts_emit_contact(arr[b][order[i]], (uint8_t)b, out[written++]);
    }
    return written;
}
