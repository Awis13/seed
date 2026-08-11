#pragma once

/*
 * rns/inbox.h — the handoff between the Reticulum packet callback and the loop
 * task's pickup, plus the text sanitiser the notification card needs before it
 * will accept a stranger's bytes.
 *
 * WHY THIS EXISTS. Destination::set_packet_callback() fires inside
 * Destination::receive(), which is reached from Transport::inbound(), which is
 * reached from our own drain — so the callback runs on the loop task inside the
 * 8 ms socket budget, with Transport::_jobs_locked set. Raising the card there
 * is not allowed: notify_ingest() calls time(NULL), takes the notify_mux
 * critical section and appends to the event ring, all three on the callback's
 * forbidden list in skills/rns.cpp — and it then write-throughs the card to the
 * off-loop history archive, which is a 0-tick queue hand-off that blocks
 * nothing but spends stack the drain has already spent.
 *
 * So the callback copies the bytes into a slot and returns, and the loop task
 * raises the cards one function call later, after rns_stack.loop() has returned.
 * That is the same deferral cluster the rest of this firmware uses
 * (skills/wg.cpp's request flags, skills/rns.cpp's g_rns_cfg_dirty): the
 * forbidden context raises, the permitted one consumes.
 *
 * BOTH SIDES ARE THE LOOP TASK, and this file will not pretend otherwise. The
 * producer runs inside rns_stack.loop() and the consumer runs after it returns,
 * so there is no cross-task race here and no memory ordering to get right — the
 * hazard being avoided is the CONTEXT (inside the drain, inside inbound()), not
 * a second core. The publish-last discipline in rns_inbox_put() is therefore
 * defensive rather than load-bearing today; it is written that way so that
 * moving the pickup onto another task later is a change of caller and not a
 * change of contract.
 *
 * WHY EIGHT SLOTS AND NOT ONE. The drain admits up to RNS_TCP_DRAIN_FRAMES_MAX
 * frames in a single tick and the pickup runs once per tick, so a one-deep
 * inbox turns eight messages arriving in one TCP read into one card and seven
 * drops. That is not a corner case: rns-send tells a user whose message is over
 * the 383-byte ceiling to send it in pieces, and anything sending those pieces
 * from one process produces exactly that burst.
 *
 * The depth is therefore ONE DRAIN PASS, not a round number — eight, because
 * eight is the most the interface can hand us between two pickups, so a full
 * pass costs no drops at all. skills/rns.cpp static_asserts this against
 * RNS_TCP_DRAIN_FRAMES_MAX itself: if the frame budget ever grows, the ring has
 * to grow with it or the build fails. The pickup drains until the ring is empty
 * rather than taking one per tick, so it is a smoothing buffer and not a queue
 * that can fall behind. Three kilobytes of static RAM is what that costs, and it
 * is the difference between a burst arriving and a burst half arriving.
 *
 * OVERFLOW POLICY FOR A FULL RING: KEEP THE OLDEST, COUNT THE DROP. A payload
 * arriving with every one of the RNS_INBOX_SLOTS slots occupied is refused and
 * counted in `dropped`.
 *
 * THE HONEST REASON, because an earlier version of this comment gave a wrong
 * one. It argued that overwriting would let a stranger suppress an earlier
 * message. It would not: whoever wins the storage suppresses the rest either
 * way, and under keep-oldest a continuous flood wins every slot every tick just
 * as effectively. NOTHING HERE DEFENDS AGAINST A FLOOD — see skills/rns.cpp,
 * which says the same thing about the address itself. The policy is chosen for
 * what a NON-ADVERSARIAL burst does. On a pager the oldest unshown message is
 * the one a human is waiting for, and it is the one that cannot be
 * reconstructed: under overwrite the sender of the destroyed message believes it
 * was delivered and has no way to learn otherwise, whereas the sender of a
 * refused message is the one still holding it. What neither policy may do is
 * lose a message silently, so every refusal is counted and published in GET
 * /rns/status.
 *
 * SIZING. RNS_INBOX_PAYLOAD_MAX is 383, the largest plaintext a single
 * encrypted packet to a SINGLE destination can carry:
 *
 *     floor((MDU - TOKEN_OVERHEAD - KEYSIZE/16) / AES128_BLOCKSIZE)
 *         * AES128_BLOCKSIZE - 1
 *     = floor((464 - 48 - 32) / 16) * 16 - 1 = 383
 *
 * Both implementations compute it identically — RNS/Packet.py ENCRYPTED_MDU and
 * microReticulum's Type.h Type::Packet::ENCRYPTED_MDU — and skills/rns.cpp
 * static_asserts this constant against the library's own value, so a drift is a
 * failed build rather than a truncated message on the device.
 *
 * PURITY. No Arduino, no globals, no allocation: a plain struct the firmware
 * owns and free functions that hold the rules, in the shape of rns/pktfilter.h,
 * rns/hdlc.h and rns/annsched.h. tools/test_rns_inbox.sh compiles this header on
 * the host and drives the real code, including the overflow policy above.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* See SIZING above. Checked against the library's enum in skills/rns.cpp. */
#define RNS_INBOX_PAYLOAD_MAX 383
/* See WHY EIGHT SLOTS above: one whole drain pass, checked against
 * RNS_TCP_DRAIN_FRAMES_MAX in skills/rns.cpp. */
#define RNS_INBOX_SLOTS 8

/*
 * The whole storage. The firmware owns one of these at file scope — fixed size,
 * sized once, never allocated — and passes it to every function below.
 *
 * head is the slot the next take() will read and count is how many are
 * occupied, so the producer writes at (head + count) % RNS_INBOX_SLOTS. count is
 * the only field that publishes a message, which is why it is stored last.
 *
 * last_len is the length of the most recent OFFER, accepted or refused, so a
 * dropped message still leaves evidence of how big it was. received counts every
 * offer, which is what makes received - dropped the number of messages that
 * actually reached the screen path.
 */
typedef struct {
    uint8_t buf[RNS_INBOX_SLOTS][RNS_INBOX_PAYLOAD_MAX];
    uint16_t len[RNS_INBOX_SLOTS];
    uint8_t head;       /* slot the next take() reads */
    uint8_t count;      /* slots occupied; published LAST by put() */
    uint32_t received;  /* payloads offered by the callback, full stop */
    uint32_t dropped;   /* offers refused: the ring was full (see policy) */
    uint32_t oversize;  /* offers longer than a slot; stored short and counted */
    uint16_t last_len;  /* length of the most recent offer, accepted or not */
} rns_inbox;

static inline void rns_inbox_init(rns_inbox *bx) {
    if (!bx) return;
    memset(bx, 0, sizeof(*bx));
}

/*
 * Producer side, called from the packet callback. Everything it does is a
 * memcpy and a handful of scalar stores: no allocation, no clock, no logging,
 * nothing that can throw.
 *
 * `data` points into the decrypted plaintext owned by Destination::receive() and
 * DANGLES the moment the callback returns, so the bytes are copied here and the
 * length is carried with them — the payload is not NUL-terminated and never
 * becomes so.
 *
 * An offer of zero bytes is refused without counting: it cannot happen from
 * Destination::receive(), which tests `if (plaintext)` and skips the callback
 * when the plaintext is empty, so counting it would invent traffic that the
 * device never saw.
 *
 * Returns true when the message was stored.
 */
static inline bool rns_inbox_put(rns_inbox *bx, const uint8_t *data, size_t len) {
    if (!bx || !data || len == 0) return false;

    bx->received++;
    bx->last_len = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;

    /* KEEP THE OLDEST, COUNT THE DROP — see the policy note in the header. */
    if (bx->count >= RNS_INBOX_SLOTS) {
        bx->dropped++;
        return false;
    }

    uint8_t slot = (uint8_t)((bx->head + bx->count) % RNS_INBOX_SLOTS);
    size_t n = len;
    if (n > RNS_INBOX_PAYLOAD_MAX) {
        n = RNS_INBOX_PAYLOAD_MAX;
        bx->oversize++;
    }
    memcpy(bx->buf[slot], data, n);
    bx->len[slot] = (uint16_t)n;
    /* Published LAST: the message is visible only once the bytes are in. This
     * must remain the final statement of this function — a source-text pin in
     * tools/test_task_unblock.py enforces it, because a single-threaded host
     * test cannot. */
    bx->count++;
    return true;
}

/*
 * Consumer side, called from the loop task in a loop of its own until it returns
 * false. Copies the oldest pending message out and frees its slot; returns false
 * when nothing is waiting, in which case *len_out is zeroed and dst is not
 * touched.
 *
 * A `cap` smaller than the stored length is a caller bug rather than a runtime
 * condition — the firmware passes a buffer of RNS_INBOX_PAYLOAD_MAX — so the
 * copy is clamped to cap and the reported length is the number of bytes actually
 * written, never a promise about bytes that were not.
 */
static inline bool rns_inbox_take(rns_inbox *bx, uint8_t *dst, size_t cap,
                                  uint16_t *len_out) {
    if (len_out) *len_out = 0;
    if (!bx || bx->count == 0) return false;

    uint8_t slot = bx->head;
    size_t n = bx->len[slot];
    if (n > cap) n = cap;
    if (n > 0 && dst) memcpy(dst, bx->buf[slot], n);
    if (len_out) *len_out = (uint16_t)n;

    bx->len[slot] = 0;
    bx->head = (uint8_t)((slot + 1) % RNS_INBOX_SLOTS);
    /* Released LAST, for the symmetric reason put() publishes last. */
    bx->count--;
    return true;
}

/*
 * Turn an arbitrary payload into text a notification card can hold.
 *
 * THE PAYLOAD IS NOT TEXT until this function says it is. A peer can send any
 * 383 bytes it likes: notify_ingest() filters nothing, the card renderer draws
 * what it is given, and the body is stored as a C string — so a single 0x00 in
 * the middle would silently end the message, and a stray 0xFF is not UTF-8 at
 * all. What this does, exactly:
 *
 *   - '\n' is kept; the card body is already multi-line.
 *   - every other byte below 0x20, and 0x7F, becomes '.', one dot per byte.
 *     That includes NUL. A dot rather than a deletion because the length the
 *     user sees should match the length the sender sent.
 *   - a well-formed UTF-8 sequence is copied whole. Well-formed means the strict
 *     definition: no overlong encodings, no surrogates, nothing above U+10FFFF.
 *   - every byte that is not part of one becomes '?'.
 *
 * TWO BUDGETS, BOTH OF WHICH STOP IT ON A CHARACTER BOUNDARY. out_cap is the
 * destination size in BYTES including the terminator (notify's storage limit).
 * max_chars is a budget in CODEPOINTS, which is what the card renderer actually
 * counts — hw_ui.cpp walks the body by codepoint and paints a fixed number of
 * columns and rows, so a byte budget alone would cut a Cyrillic message at half
 * the text the screen can show. Zero means no character limit.
 *
 * THE RETURN VALUE IS BOTH NUMBERS AT ONCE, and that is a property of the
 * mapping rather than a convenience: every branch above writes exactly one
 * output byte per input byte, or copies a sequence of N bytes for N bytes of
 * input, so bytes-written and input-bytes-consumed are the same number on every
 * path. The caller depends on that to say how much of the message did not fit —
 * `in_len - rns_text_sanitize(...)` is the honest count of what was lost.
 * DO NOT ADD A MAPPING THAT EXPANDS (an escape like "\\x07" for a control byte,
 * say) without splitting these two numbers apart again: it would silently
 * corrupt that count. tools/test_rns_inbox.cpp asserts the invariant over a
 * corpus that exercises every branch.
 *
 * out is always NUL-terminated (out_cap must be at least 1).
 */
static inline size_t rns_text_sanitize(const uint8_t *in, size_t in_len,
                                       char *out, size_t out_cap,
                                       size_t max_chars) {
    size_t o = 0;
    size_t i = 0;
    size_t chars = 0;

    if (!out || out_cap == 0) return 0;
    if (!in) in_len = 0;

    while (i < in_len) {
        uint8_t c = in[i];
        size_t need = 1;   /* input bytes this step consumes when copied whole */
        char repl = 0;     /* non-zero: emit this one byte for one input byte */

        if (max_chars != 0 && chars >= max_chars) break;

        if (c == '\n') {
            repl = '\n';
        } else if (c < 0x20 || c == 0x7F) {
            repl = '.';
        } else if (c < 0x80) {
            repl = (char)c;
        } else {
            size_t seq = 0;
            bool ok;
            if (c >= 0xC2 && c <= 0xDF) seq = 2;
            else if (c >= 0xE0 && c <= 0xEF) seq = 3;
            else if (c >= 0xF0 && c <= 0xF4) seq = 4;
            /* The length check is the read-past-end guard, not a formality: a
             * lead byte at the very end of the payload announces continuation
             * bytes that are not there, and the payload's buffer ends where it
             * ends. */
            ok = (seq != 0) && (in_len - i >= seq);
            if (ok) {
                for (size_t k = 1; k < seq; k++) {
                    if ((in[i + k] & 0xC0) != 0x80) { ok = false; break; }
                }
            }
            /* The ranges a continuation-byte check alone lets through: overlong
             * forms of an already-shorter code point, the UTF-16 surrogate
             * block, and anything past U+10FFFF. */
            if (ok && seq == 3) {
                if (c == 0xE0 && in[i + 1] < 0xA0) ok = false;
                if (c == 0xED && in[i + 1] >= 0xA0) ok = false;
            }
            if (ok && seq == 4) {
                if (c == 0xF0 && in[i + 1] < 0x90) ok = false;
                if (c == 0xF4 && in[i + 1] >= 0x90) ok = false;
            }
            if (ok) need = seq;
            else repl = '?';
        }

        if (repl != 0) {
            if (o + 1 >= out_cap) break;    /* +1: the terminator has to fit */
            out[o++] = repl;
            i += 1;
        } else {
            /* >=, not >: at equality the last sequence byte would land on the
             * terminator's own slot and the NUL would be written one past the
             * end of out. */
            if (o + need >= out_cap) break; /* stop on the boundary, never mid-sequence */
            for (size_t k = 0; k < need; k++) out[o++] = (char)in[i + k];
            i += need;
        }
        chars++;
    }

    out[o] = '\0';
    return o;   /* == i, always: see THE RETURN VALUE IS BOTH NUMBERS AT ONCE */
}
