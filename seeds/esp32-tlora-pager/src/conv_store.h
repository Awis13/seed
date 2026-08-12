#pragma once
/*
 * conv_store.h — the pager's CONVERSATION model (pure, host-testable).
 *
 * WHY THIS EXISTS
 * ---------------
 * The chat store used to be a fixed table of exactly two AGENTS (`claude`,
 * `hermes`) whose only addressable dimension was a session name, and whose only
 * reply path was "hand the agent id back to the bridge". Notification cards are
 * the device's main feature and must be deliverable over EVERY transport, so a
 * thread has to be able to come from an LXMF sender or a mesh peer just as well
 * as from a bridge agent — and a thread that arrived over LXMF must remember
 * WHERE to answer, which an agent id cannot express.
 *
 * So the record is a Conversation, not an agent slot:
 *   - `transport`  says which wire the thread lives on;
 *   - `reply_addr` (+ `reply_len`) is the OPAQUE return address for that wire —
 *     agent-id bytes for CONV_AGENT, a 16-byte LXMF source hash for CONV_LXMF,
 *     a 32-byte public key for CONV_MESH. Nothing here interprets it;
 *   - `label` is the human display name, which no longer has to be the id.
 *
 * ON-DISK KEY  (this is the part with a hard, previously-unenforced limit)
 * -----------------------------------------------------------------------
 * A thread's log lives at  /conv.<shortid>  in the flat store (SD root, or the
 * SPIFFS fallback). SPIFFS object names are capped at CONV_PATH_LEN bytes
 * INCLUDING the NUL; a path over that is silently never created, which on this
 * device shows up as an empty thread and a black chat screen (that is why the
 * old key dropped its ".jsonl" extension). The old key form
 * `/agent.<id>.<session>` could still blow the cap with a long room name and had
 * no guard at all. Here the budget is ENFORCED: conv_log_path() always returns a
 * path that fits, folding an over-long key down to a readable head plus a
 * fingerprint of the WHOLE key, so two long names sharing a prefix cannot land
 * on the same file. The JSONL content contract is untouched.
 *
 * A thread key is  <conv_id>[.<session>]  — the conversation id first, so a
 * conversation with no rooms (a peer: 8 hex of its address) is simply
 * /conv.a1b2c3d4, and an agent room is /conv.claude.pager-abcd. Conversation ids
 * therefore may not contain '.', which is what conv_thread_key() enforces.
 *
 * MANIFEST
 * --------
 * /conversations.txt, one tab-separated line per THREAD:
 *
 *     key <TAB> label <TAB> transport <TAB> reply_hex <TAB> dead
 *
 * The transport/reply/label fields belong to the conversation and simply repeat
 * across its rooms; loading them is idempotent. The legacy manifest
 * (/agents_sessions.txt, "agent<TAB>session" with an optional third dead flag)
 * parses through the same function as transport=CONV_AGENT, so an existing
 * device keeps its rooms.
 *
 * Everything below is pure: no Arduino, no FreeRTOS, no FS. skills/agents.cpp
 * wraps it with the live store; tools/test_conv_store.cpp drives it directly.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One name alphabet for the whole store: [A-Za-z0-9._-], bounded. */
#include "agents_chat_route.h"

/* Which wire a conversation lives on. The value is persisted in the manifest,
 * so these numbers are part of the on-disk format — append, never renumber. */
enum ConvTransport {
    CONV_AGENT = 0,   /* bridge / MeshCore C1 agent chat; reply = agent-id bytes */
    CONV_LXMF  = 1,   /* Reticulum LXMF;  reply = 16-byte source hash           */
    CONV_MESH  = 2,   /* MeshCore peer;   reply = 32-byte public key            */
};
#define CONV_TRANSPORT_LAST  CONV_MESH

#define CONV_ID_LEN        12   /* conversation id, NUL incl (11 usable)        */
#define CONV_LABEL_LEN     16   /* human display name, NUL incl                 */
#define CONV_SESSION_LEN   24   /* room name, NUL incl (23 usable)              */
#define CONV_SESSIONS_MAX   8   /* rooms per conversation (registry cap)        */
#define CONV_REPLY_MAX     32   /* widest return address we carry (mesh pubkey) */
/* Logical thread key "<conv_id>[.<session>]", NUL incl: 11 + 1 + 23 + 1 = 36. */
#define CONV_KEY_LEN       40
#define CONV_PATH_PREFIX   "/conv."
/* SPIFFS object-name budget, bytes INCLUDING the NUL. Paths are held to it.
 * The platform fact, not a guess: SPIFFS_OBJ_NAME_LEN = CONFIG_SPIFFS_OBJ_NAME_LEN
 * = 32 on this build, and spiffs_config.h states "this length include the
 * zero-termination character, meaning maximum string of characters can at most
 * be SPIFFS_OBJ_NAME_LEN - 1" — so 32 bytes with the NUL, 31 characters. */
#define CONV_PATH_LEN      32
/* Filename component after the prefix: CONV_PATH_LEN - strlen("/conv.").       */
#define CONV_SHORTID_LEN   (CONV_PATH_LEN - 6)
/* Separator between a folded key's head and its fingerprint. It MUST be outside
 * the room-name alphabet [A-Za-z0-9._-], and that is the whole point: room names
 * arrive from the wire (claude_route_incoming), so if the fingerprint were
 * introduced by a byte a room name can contain — '-' was the first attempt — a
 * remote peer could name a room whose VERBATIM key is byte-identical to some
 * other room's FOLDED key, and the two would share one history file:
 * interleaved messages, wrong per-room counts, and a clear of one wiping the
 * other. '~' cannot be produced by agents_route_sanitize, so the verbatim and
 * folded namespaces are disjoint by construction rather than by luck. It is
 * legal on SPIFFS (any byte) and on FAT long names (FatFs bars only
 * " * : < > ? | and DEL). */
#define CONV_FOLD_SEP      '~'
#define CONV_MANIFEST         "/conversations.txt"
#define CONV_MANIFEST_LEGACY  "/agents_sessions.txt"
/* key + label + "255" + 64 hex + flag + 4 tabs + NUL, with slack. */
#define CONV_MANIFEST_LINE_LEN 160

/*
 * Pure slot planner: which slot should a NEW conversation take?
 *
 * A free slot if the table has one. Otherwise the least-recently-used
 * conversation that is NOT seeded — the seeded ones are the device's own doors
 * and the user must always be able to reach them, so they are never candidates
 * and a table of nothing but seeded slots refuses (-1) rather than throwing one
 * out. Ties go to the lowest index, so the choice is deterministic.
 *
 * Evicting a slot is not deleting a conversation: the slot is a RAM cache of a
 * thread whose history lives in its own /conv.<id> file, which nothing here
 * touches. A peer that comes back is re-minted onto the same id and reopens the
 * same log. That is what makes reusing the slot safe rather than lossy.
 *
 * THE CONVERSATION BEING READ IS ALSO PROTECTED. The screen holds an index into
 * this table, so recycling the slot it points at does not close that chat — it
 * silently REPLACES it, leaving the reader looking at a stranger's messages
 * under a stranger's name, mid-scroll, with no event to notice. Losing an
 * incoming conversation is recoverable; being quietly moved into someone else's
 * is not. protect_idx is that slot (-1 for none).
 *
 *   n_live      conversations currently in the table
 *   cap         table capacity
 *   seeded      per-slot flag, non-zero = never evict
 *   last_use    per-slot monotonic use stamp (higher = more recent)
 *   protect_idx slot currently on screen, or -1
 *   may_evict   false = take a free slot or refuse; never displace anyone
 *
 * SOME CALLERS MAY NOT EVICT AT ALL. An address-based transport accepts
 * messages from anyone who knows our address, so letting those mint by
 * displacement would hand an unauthenticated sender the power to churn the
 * table — flooding out conversations the user actually has. Such a caller
 * passes may_evict=false: it may occupy a free slot, and when there is none the
 * message becomes a card instead. Transports that only accept cryptographically
 * attributed peers may evict normally.
 *
 * Returns the slot index to use, or -1 when nothing may be evicted.
 */
static inline int conv_slot_plan(int n_live, int cap, const uint8_t *seeded,
                                 const uint32_t *last_use, int protect_idx,
                                 bool may_evict) {
    if (cap <= 0) return -1;
    if (n_live < 0) n_live = 0;
    if (n_live > cap) n_live = cap;
    if (n_live < cap) return n_live;          /* a free slot at the end */
    if (!may_evict) return -1;                /* full: refuse, never displace */
    if (!seeded || !last_use) return -1;
    int victim = -1;
    uint32_t oldest = 0;
    for (int i = 0; i < n_live; i++) {
        if (seeded[i] || i == protect_idx) continue;
        if (victim < 0 || last_use[i] < oldest) {
            victim = i;
            oldest = last_use[i];
        }
    }
    return victim;
}

/* One parsed manifest line. `key` is the logical thread key; `conv`/`session`
 * are its two components (session empty for a conversation with no rooms). */
struct ConvManifestLine {
    char    key[CONV_KEY_LEN];
    char    conv[CONV_ID_LEN];
    char    session[CONV_SESSION_LEN];
    char    label[CONV_LABEL_LEN];
    uint8_t transport;
    uint8_t reply[CONV_REPLY_MAX];
    uint8_t reply_len;
    uint8_t dead;
};

/* FNV-1a folded to 16 bits. Only ever used to keep two over-long thread keys
 * off the same file, never for security. */
static inline uint16_t conv_fp16(const char *s) {
    uint32_t h = 0x811c9dc5u;
    if (!s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return (uint16_t)((h >> 16) ^ (h & 0xffffu));
}

/* Logical thread key: "<conv_id>" or "<conv_id>.<session>". Both components are
 * run through the store's name alphabet first. A conversation id containing '.'
 * is REJECTED — '.' is the separator that splits the key back apart on load. */
static inline bool conv_thread_key(char *out, size_t out_n, const char *conv_id,
                                   const char *session) {
    if (!out || out_n == 0) return false;
    out[0] = '\0';
    char cid[CONV_ID_LEN];
    agents_route_sanitize(conv_id, cid, sizeof(cid));
    if (!cid[0] || strchr(cid, '.')) return false;
    char sess[CONV_SESSION_LEN];
    agents_route_sanitize(session, sess, sizeof(sess));
    int n = sess[0] ? snprintf(out, out_n, "%s.%s", cid, sess)
                    : snprintf(out, out_n, "%s", cid);
    if (n <= 0 || (size_t)n >= out_n) { out[0] = '\0'; return false; }
    return true;
}

/* Filename component for a thread key, held inside CONV_SHORTID_LEN. A key that
 * fits is used verbatim (so /conv.claude.pager-abcd stays readable on the card);
 * a longer one keeps a head and gains a "~<4 hex>" fingerprint of the FULL key,
 * which is deterministic across reboots and distinguishes shared prefixes. The
 * '~' keeps the folded names in a namespace no verbatim key can reach — see
 * CONV_FOLD_SEP for why that separation is load-bearing and not cosmetic. */
static inline bool conv_shortid(char *out, size_t out_n, const char *key) {
    if (!out || out_n == 0) return false;
    out[0] = '\0';
    if (!key || !key[0]) return false;
    size_t cap = out_n - 1;
    if (cap > CONV_SHORTID_LEN - 1) cap = CONV_SHORTID_LEN - 1;
    size_t klen = strlen(key);
    if (klen <= cap) {
        memcpy(out, key, klen);
        out[klen] = '\0';
        return true;
    }
    if (cap < 6) return false;              /* no room for head + "~xxxx" */
    size_t head = cap - 5;
    memcpy(out, key, head);
    snprintf(out + head, out_n - head, "%c%04x", CONV_FOLD_SEP,
             (unsigned)conv_fp16(key));
    return true;
}

/* Full on-disk path for a thread. Guaranteed to fit CONV_PATH_LEN (NUL incl) or
 * to fail outright — it never hands back a path the store would silently drop. */
static inline bool conv_log_path(char *out, size_t out_n, const char *conv_id,
                                 const char *session) {
    if (!out || out_n == 0) return false;
    out[0] = '\0';
    char key[CONV_KEY_LEN];
    char sid[CONV_SHORTID_LEN];
    if (!conv_thread_key(key, sizeof(key), conv_id, session)) return false;
    if (!conv_shortid(sid, sizeof(sid), key)) return false;
    int n = snprintf(out, out_n, "%s%s", CONV_PATH_PREFIX, sid);
    if (n <= 0 || (size_t)n >= out_n || (size_t)n + 1 > CONV_PATH_LEN) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static inline size_t conv_hex_encode(const uint8_t *in, size_t n, char *out,
                                     size_t out_n) {
    static const char *hx = "0123456789abcdef";
    size_t j = 0;
    if (!out || out_n == 0) return 0;
    for (size_t i = 0; in && i < n && j + 2 < out_n; i++) {
        out[j++] = hx[(in[i] >> 4) & 0xf];
        out[j++] = hx[in[i] & 0xf];
    }
    out[j] = '\0';
    return j;
}

/* Hex -> bytes. Returns the byte count; a malformed or over-long string yields
 * 0 (an unusable return address is better than a half-decoded one). */
static inline size_t conv_hex_decode(const char *in, uint8_t *out, size_t out_n) {
    if (!in || !out) return 0;
    size_t len = strlen(in);
    if (len == 0 || (len & 1) || len / 2 > out_n) return 0;
    for (size_t i = 0; i < len; i += 2) {
        uint8_t v = 0;
        for (size_t k = 0; k < 2; k++) {
            char c = in[i + k];
            uint8_t d;
            if (c >= '0' && c <= '9')      d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else return 0;
            v = (uint8_t)((v << 4) | d);
        }
        out[i / 2] = v;
    }
    return len / 2;
}

/* CONV_PEER_ID_BYTES of a peer's address, hex, as the conversation id it is
 * keyed under. Five, not four: the id is what find-before-mint hits on, and four
 * bytes is a 2^32 prefix grind an attacker can run offline against a known
 * correspondent. Five costs two characters — /conv.<10hex> is 16 bytes against a
 * 32-byte budget — and the full address is compared anyway (see conv_mint in
 * skills/agents.cpp), so this is depth, not the only barrier. A shorter-than-id
 * address yields "" (an unkeyable peer), never a partial id. Lives here rather
 * than in agents.cpp so both the live store and the Contacts screen derive a
 * peer id the one way, and the derivation is proved by value on the host. */
#define CONV_PEER_ID_BYTES 5
static inline void conv_peer_id(const uint8_t *pubkey, uint8_t len, char *out,
                                size_t out_n) {
    if (!out || out_n == 0) return;
    out[0] = '\0';
    if (!pubkey || len < CONV_PEER_ID_BYTES) return;
    conv_hex_encode(pubkey, CONV_PEER_ID_BYTES, out, out_n);
}

/* Copy a display label into a field-safe form, bounded to CONV_LABEL_LEN.
 *
 * THE LABEL IS THE ONE FREE-TEXT FIELD IN A TAB-SEPARATED, NEWLINE-JOINED
 * RECORD, so it is also the one place a byte can break the record apart. A TAB
 * ends the label early and shifts every field after it — the transport, the
 * return address and the dead flag are read from the wrong columns on the next
 * boot. A NEWLINE is worse: it ends the record and starts a second
 * well-formed-looking one, letting a label forge a line for a DIFFERENT
 * conversation and flip its dead flag, transport or reply address. Today every
 * label is a compile-time literal, so neither is reachable — but the field
 * exists precisely so it can be set from an LXMF or mesh sender's name, i.e.
 * from the wire, and a barrier added after that is a barrier added too late.
 * Control bytes become spaces rather than truncating the label: a
 * hostile name still shows, it just cannot restructure the file. */
static inline void conv_label_sanitize(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    if (!out || out_n == 0) return;
    if (out_n > CONV_LABEL_LEN) out_n = CONV_LABEL_LEN;
    if (in) {
        for (const char *p = in; *p && j + 1 < out_n; p++) {
            unsigned char c = (unsigned char)*p;
            out[j++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        }
    }
    out[j] = '\0';
}

/* Serialise one thread as a manifest line (no trailing newline — the caller
 * joins). Field order is the on-disk contract:
 *   key <TAB> label <TAB> transport <TAB> reply_hex <TAB> dead */
static inline bool conv_manifest_format(char *out, size_t out_n,
                                        const char *conv_id, const char *session,
                                        const char *label, uint8_t transport,
                                        const uint8_t *reply, uint8_t reply_len,
                                        uint8_t dead) {
    if (!out || out_n == 0) return false;
    out[0] = '\0';
    char key[CONV_KEY_LEN];
    if (!conv_thread_key(key, sizeof(key), conv_id, session)) return false;
    if (reply_len > CONV_REPLY_MAX) return false;
    char hex[CONV_REPLY_MAX * 2 + 1];
    conv_hex_encode(reply, reply_len, hex, sizeof(hex));
    char lbl[CONV_LABEL_LEN];
    conv_label_sanitize(label, lbl, sizeof(lbl));   /* no TAB/newline escapes */
    int n = snprintf(out, out_n, "%s\t%s\t%u\t%s\t%u", key, lbl,
                     (unsigned)transport, hex, dead ? 1u : 0u);
    if (n <= 0 || (size_t)n >= out_n) { out[0] = '\0'; return false; }
    return true;
}

/* Parse one manifest line, NEW 5-field or LEGACY 2/3-field, into *out.
 *
 * MIGRATION SHAPE: read BOTH manifests, write only the new one. The loader in
 * skills/agents.cpp reads /agents_sessions.txt first (if present) and then
 * /conversations.txt on top, so a device that has both ends up with the newer
 * flags winning, and the next persist writes the new format only. That is
 * simpler than a rewrite-in-place migration and has no half-migrated state: the
 * legacy file is never edited or removed, it just stops being authoritative.
 *
 * A legacy line has no transport and no return address, so it loads as
 * CONV_AGENT with the agent id itself as the reply address — exactly what the
 * bridge and gateway-DM path has always used to answer such a room.
 *
 * Returns false on a malformed line (too few fields, unparseable key); the
 * caller skips it rather than inventing a room. */
static inline bool conv_manifest_parse(const char *line, ConvManifestLine *out) {
    if (!line || !out) return false;
    memset(out, 0, sizeof(*out));

    char buf[CONV_MANIFEST_LINE_LEN];
    size_t n = 0;
    for (; line[n] && n + 1 < sizeof(buf); n++) buf[n] = line[n];
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (!buf[0]) return false;

    char *f[5] = {buf, NULL, NULL, NULL, NULL};
    int nf = 1;
    for (char *p = buf; *p; p++) {
        if (*p != '\t') continue;
        if (nf >= 5) break;      /* extra tabs stay inside the last field */
        *p = '\0';               /* ... so stop BEFORE cutting, not after */
        f[nf++] = p + 1;
    }

    const char *key_src = NULL;
    if (nf >= 5) {
        key_src = f[0];
        /* Same barrier on the way IN: the manifest lives on a card a user can
         * edit, so a control byte can reach us without ever passing _format. */
        conv_label_sanitize(f[1], out->label, sizeof(out->label));
        unsigned t = (unsigned)strtoul(f[2], NULL, 10);
        /* An unknown (future) transport degrades to the agent path rather than
         * dropping the room: the history is still readable, only the reply
         * route is unknown, and the next writer re-states it. */
        out->transport = (t <= (unsigned)CONV_TRANSPORT_LAST) ? (uint8_t)t
                                                             : (uint8_t)CONV_AGENT;
        out->reply_len = (uint8_t)conv_hex_decode(f[3], out->reply,
                                                  sizeof(out->reply));
        out->dead = (f[4][0] == '1') ? 1 : 0;
    } else if (nf == 2 || nf == 3) {
        out->transport = (uint8_t)CONV_AGENT;
        out->dead = (nf == 3 && f[2][0] == '1') ? 1 : 0;
    } else {
        return false;            /* 1 or 4 fields: not a shape we ever wrote */
    }

    char cid[CONV_ID_LEN];
    char sess[CONV_SESSION_LEN];
    if (key_src) {               /* new form: split the key at the FIRST '.' */
        const char *dot = strchr(key_src, '.');
        size_t clen = dot ? (size_t)(dot - key_src) : strlen(key_src);
        char raw[CONV_ID_LEN];
        size_t c = 0;
        for (; c < clen && c + 1 < sizeof(raw); c++) raw[c] = key_src[c];
        raw[c] = '\0';
        agents_route_sanitize(raw, cid, sizeof(cid));
        agents_route_sanitize(dot ? dot + 1 : "", sess, sizeof(sess));
    } else {                     /* legacy form: the two fields ARE the parts */
        agents_route_sanitize(f[0], cid, sizeof(cid));
        agents_route_sanitize(f[1], sess, sizeof(sess));
        out->reply_len = (uint8_t)strlen(cid);
        memcpy(out->reply, cid, out->reply_len);
    }
    if (!conv_thread_key(out->key, sizeof(out->key), cid, sess)) return false;
    snprintf(out->conv, sizeof(out->conv), "%s", cid);
    snprintf(out->session, sizeof(out->session), "%s", sess);
    return true;
}
