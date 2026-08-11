/*
 * Host tests for the inbound message ring and the payload sanitiser.
 *
 * rns/inbox.h is the seam between a callback that may do almost nothing and a
 * loop task that may do anything, and every rule it holds is a rule about
 * somebody else's bytes: a payload that is not NUL-terminated, whose pointer
 * dangles the moment the callback returns, whose length can be anything up to
 * 383 and whose content can be anything at all. None of that can be exercised
 * on the device without a peer, a socket and a flash, and all of it compiles on
 * the host — so it is pinned here.
 *
 * THE OVERFLOW POLICY IS THE POINT of sections 3 and 4. A payload arriving when
 * all RNS_INBOX_SLOTS slots are occupied is REFUSED and COUNTED — keep the
 * oldest, count the drop — and the reason it is a policy rather than an accident
 * is that both halves are observable: the queued messages must still come out
 * intact and in order, and the refusal must still be visible in a counter. A
 * silently lost message is the one outcome that is not acceptable, and it is
 * exactly what a missing `dropped++` would look like from the outside.
 *
 * SECTIONS 9-12 ARE THE SANITISER, and three of their cases exist because a
 * mutation survived an earlier version of this file: a `>=` relaxed to `>` at
 * the output boundary (a one-byte write past the caller's buffer), a dropped
 * length check on a multi-byte lead (a read past the end of the payload), and an
 * expansion in the byte mapping (which would silently corrupt the "+N bytes"
 * count on the card, because the return value is both the bytes written and the
 * input bytes consumed).
 *
 * The header has no Arduino dependency and no state of its own, so this file
 * needs neither the framework nor the library.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/rns/inbox.h"

/* Fill a buffer with a recognisable, position-dependent pattern: a copy that is
 * short, long, offset or stale fails on content and not only on length. */
static void pattern(uint8_t *buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + (i * 7u) + (i >> 3));
}

/* Take one message and assert it is the one expected, by length and content. */
static void expect_take(rns_inbox *bx, const uint8_t *want, size_t want_len) {
    uint8_t out[RNS_INBOX_PAYLOAD_MAX];
    uint16_t len = 0;
    assert(rns_inbox_take(bx, out, sizeof(out), &len));
    assert(len == want_len);
    assert(memcmp(out, want, want_len) == 0);
}

int main() {
    /* ---- 1. put then take: the bytes come back exactly ---------------------
     * The payload is copied because the caller's pointer dangles after the
     * callback returns, so "exactly" means content as well as length. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[64];
        pattern(msg, sizeof(msg), 0x11);
        assert(rns_inbox_put(&bx, msg, sizeof(msg)));
        assert(bx.count == 1);
        assert(bx.received == 1);
        assert(bx.dropped == 0);
        assert(bx.oversize == 0);
        assert(bx.last_len == sizeof(msg));

        /* The source is overwritten before the take, standing in for the
         * dangling pointer: anything the ring hands back now came from its own
         * buffer or the test is wrong about what put() does. */
        uint8_t expect[64];
        memcpy(expect, msg, sizeof(msg));
        memset(msg, 0, sizeof(msg));

        expect_take(&bx, expect, sizeof(expect));
        assert(bx.count == 0);
    }

    /* ---- 2. a take on an empty ring yields nothing -------------------------
     * Before anything has arrived, and again after the queue has drained. The
     * destination must be left alone in both cases: the loop task calls this
     * every tick and almost every call is this one. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t out[RNS_INBOX_PAYLOAD_MAX];
        memset(out, 0xAB, sizeof(out));
        uint16_t len = 0xFFFF;
        assert(!rns_inbox_take(&bx, out, sizeof(out), &len));
        assert(len == 0);
        for (size_t i = 0; i < sizeof(out); i++) assert(out[i] == 0xAB);

        uint8_t msg[8];
        pattern(msg, sizeof(msg), 0x33);
        assert(rns_inbox_put(&bx, msg, sizeof(msg)));
        assert(rns_inbox_take(&bx, out, sizeof(out), &len));
        assert(len == sizeof(msg));

        len = 0xFFFF;
        assert(!rns_inbox_take(&bx, out, sizeof(out), &len));
        assert(len == 0);

        /* An empty offer is refused and is NOT counted: Destination::receive()
         * skips the callback on empty plaintext, so counting it here would
         * invent traffic the device never saw. */
        assert(!rns_inbox_put(&bx, msg, 0));
        assert(!rns_inbox_put(&bx, NULL, 4));
        assert(bx.received == 1);
        assert(bx.dropped == 0);
    }

    /* ---- 3. the ring holds a burst, in order -------------------------------
     * RNS_TCP_DRAIN_FRAMES_MAX admits 8 frames in one tick and the pickup runs
     * once per tick, so the ring exists precisely so that a burst is not a
     * sequence of drops. Order matters: the pager shows the oldest first. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[RNS_INBOX_SLOTS][40];
        for (int i = 0; i < RNS_INBOX_SLOTS; i++) {
            pattern(msg[i], sizeof(msg[i]), (uint8_t)(0x10 * (i + 1)));
            assert(rns_inbox_put(&bx, msg[i], sizeof(msg[i])));
        }
        assert(bx.count == RNS_INBOX_SLOTS);
        assert(bx.received == RNS_INBOX_SLOTS);
        assert(bx.dropped == 0);

        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            expect_take(&bx, msg[i], sizeof(msg[i]));
        assert(bx.count == 0);
    }

    /* ---- 4. THE OVERFLOW POLICY: keep the oldest, count the drop -----------
     * One more than the ring holds, with no take in between. Everything already
     * queued survives whole and in order, the extra is refused, the refusal is
     * counted, and last_len still reports the refused message's size so the drop
     * can be sized from /rns/status. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[RNS_INBOX_SLOTS][40];
        for (int i = 0; i < RNS_INBOX_SLOTS; i++) {
            pattern(msg[i], sizeof(msg[i]), (uint8_t)(0x21 * (i + 1)));
            assert(rns_inbox_put(&bx, msg[i], sizeof(msg[i])));
        }

        uint8_t late[200];
        pattern(late, sizeof(late), 0x99);
        assert(!rns_inbox_put(&bx, late, sizeof(late)));
        assert(bx.received == RNS_INBOX_SLOTS + 1);
        assert(bx.dropped == 1);
        assert(bx.last_len == sizeof(late));

        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            expect_take(&bx, msg[i], sizeof(msg[i]));
        /* And nothing of the refused message is left behind pretending to be a
         * further delivery. */
        uint8_t out[RNS_INBOX_PAYLOAD_MAX];
        uint16_t len = 0;
        assert(!rns_inbox_take(&bx, out, sizeof(out), &len));

        /* A burst is a burst: nine refusals, nine counted, and the oldest four
         * still the ones that come out. */
        rns_inbox_init(&bx);
        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            assert(rns_inbox_put(&bx, msg[i], sizeof(msg[i])));
        for (int i = 0; i < 9; i++)
            assert(!rns_inbox_put(&bx, late, sizeof(late)));
        assert(bx.received == RNS_INBOX_SLOTS + 9);
        assert(bx.dropped == 9);
        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            expect_take(&bx, msg[i], sizeof(msg[i]));
    }

    /* ---- 5. slots reopen, and the ring wraps -------------------------------
     * The drop policy costs the messages in the burst and never the ones after
     * it. Driven past the modulo boundary several times so an index that only
     * works before the first wrap cannot pass. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[16];
        for (int round = 0; round < RNS_INBOX_SLOTS * 3; round++) {
            pattern(msg, sizeof(msg), (uint8_t)(round + 1));
            assert(rns_inbox_put(&bx, msg, sizeof(msg)));
            expect_take(&bx, msg, sizeof(msg));
            assert(bx.count == 0);
        }
        assert(bx.dropped == 0);

        /* Half-full, offset head, then filled to the brim and drained: this is
         * the shape a real burst arriving mid-drain takes. */
        uint8_t a[24], b[24], c[24], d[24];
        pattern(a, sizeof(a), 0xA0);
        pattern(b, sizeof(b), 0xB0);
        pattern(c, sizeof(c), 0xC0);
        pattern(d, sizeof(d), 0xD0);
        assert(rns_inbox_put(&bx, a, sizeof(a)));
        assert(rns_inbox_put(&bx, b, sizeof(b)));
        expect_take(&bx, a, sizeof(a));
        assert(rns_inbox_put(&bx, c, sizeof(c)));
        assert(rns_inbox_put(&bx, d, sizeof(d)));
        expect_take(&bx, b, sizeof(b));
        expect_take(&bx, c, sizeof(c));
        expect_take(&bx, d, sizeof(d));
        assert(bx.count == 0);
    }

    /* ---- 6. exactly 383 bytes, the ceiling, fits whole ---------------------
     * 383 is floor((464 - 48 - 32) / 16) * 16 - 1, the largest plaintext a
     * single encrypted packet to a SINGLE destination can carry. Off by one
     * here is a message that loses its last character on the device and nowhere
     * else. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        assert(RNS_INBOX_PAYLOAD_MAX == 383);

        uint8_t msg[RNS_INBOX_PAYLOAD_MAX];
        pattern(msg, sizeof(msg), 0x77);
        assert(rns_inbox_put(&bx, msg, sizeof(msg)));
        assert(bx.oversize == 0);
        assert(bx.last_len == RNS_INBOX_PAYLOAD_MAX);
        expect_take(&bx, msg, sizeof(msg));

        /* Every slot at the ceiling at once — the ring is sized for that. */
        rns_inbox_init(&bx);
        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            assert(rns_inbox_put(&bx, msg, sizeof(msg)));
        assert(bx.dropped == 0);
        for (int i = 0; i < RNS_INBOX_SLOTS; i++)
            expect_take(&bx, msg, sizeof(msg));
    }

    /* ---- 7. one byte over: stored short, counted, never overrun ------------
     * Structurally impossible through a single packet, which is precisely why
     * it is tested: if it ever happens the buffer must not be the thing that
     * discovers it. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[RNS_INBOX_PAYLOAD_MAX + 1];
        pattern(msg, sizeof(msg), 0x21);
        assert(rns_inbox_put(&bx, msg, sizeof(msg)));
        assert(bx.oversize == 1);
        /* last_len reports what was OFFERED, not what was kept — otherwise the
         * evidence that anything was lost disappears with the bytes. */
        assert(bx.last_len == RNS_INBOX_PAYLOAD_MAX + 1);
        expect_take(&bx, msg, RNS_INBOX_PAYLOAD_MAX);

        /* Far over the ceiling behaves the same way. */
        rns_inbox_init(&bx);
        uint8_t huge[RNS_INBOX_PAYLOAD_MAX * 2];
        pattern(huge, sizeof(huge), 0x31);
        assert(rns_inbox_put(&bx, huge, sizeof(huge)));
        assert(bx.oversize == 1);
        expect_take(&bx, huge, RNS_INBOX_PAYLOAD_MAX);
    }

    /* ---- 8. a take into a short buffer clamps and says so ------------------
     * The firmware always passes a full-size destination, so this is a caller
     * bug rather than a runtime condition — but the reported length has to be
     * what was written, never a promise about bytes that were not. */
    {
        rns_inbox bx;
        rns_inbox_init(&bx);

        uint8_t msg[64];
        pattern(msg, sizeof(msg), 0x44);
        assert(rns_inbox_put(&bx, msg, sizeof(msg)));

        uint8_t out[16];
        uint16_t len = 0;
        assert(rns_inbox_take(&bx, out, sizeof(out), &len));
        assert(len == sizeof(out));
        assert(memcmp(out, msg, sizeof(out)) == 0);
        assert(bx.count == 0);
    }

    /* ---- 9. the sanitiser: control bytes, NUL, and plain ASCII -------------
     * notify_ingest() filters nothing and the card body is a C string, so a NUL
     * in the middle of a payload would end the message with no trace. One dot
     * per control byte, so what the user sees is as long as what was sent. */
    {
        const uint8_t in[] = {'h', 'i', 0x00, 'y', 'o', 'u', 0x07, '\n', 'x',
                              0x7F, '\t', '!'};
        char out[64];
        size_t n = rns_text_sanitize(in, sizeof(in), out, sizeof(out), 0);
        assert(n == sizeof(in));
        assert(strcmp(out, "hi.you.\nx..!") == 0);
    }

    /* ---- 10. the sanitiser: UTF-8 kept, malformed UTF-8 replaced -----------
     * Well-formed sequences are copied whole — the pager renders Cyrillic and
     * mangling it would be a regression. Everything that is not part of one
     * becomes '?': lone continuation bytes, truncated sequences, overlong
     * forms, surrogates, and anything above U+10FFFF. */
    {
        char out[64];

        /* U+00E9, U+20AC, U+1F600 — two, three and four byte sequences. */
        const uint8_t ok[] = {0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
        size_t n = rns_text_sanitize(ok, sizeof(ok), out, sizeof(out), 0);
        assert(n == sizeof(ok));
        assert(memcmp(out, ok, sizeof(ok)) == 0);

        /* Lone continuation byte, and a lead byte with nothing behind it. */
        const uint8_t lone[] = {'a', 0x80, 'b', 0xC3};
        n = rns_text_sanitize(lone, sizeof(lone), out, sizeof(out), 0);
        assert(n == 4);
        assert(strcmp(out, "a?b?") == 0);

        /* Overlong '/' (C0 AF), overlong NUL (E0 80 80), a surrogate
         * (ED A0 80) and U+110000 (F4 90 80 80): each byte one '?'. */
        const uint8_t bad[] = {0xC0, 0xAF};
        rns_text_sanitize(bad, sizeof(bad), out, sizeof(out), 0);
        assert(strcmp(out, "??") == 0);

        const uint8_t overlong3[] = {0xE0, 0x80, 0x80};
        rns_text_sanitize(overlong3, sizeof(overlong3), out, sizeof(out), 0);
        assert(strcmp(out, "???") == 0);

        const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
        rns_text_sanitize(surrogate, sizeof(surrogate), out, sizeof(out), 0);
        assert(strcmp(out, "???") == 0);

        const uint8_t too_high[] = {0xF4, 0x90, 0x80, 0x80};
        rns_text_sanitize(too_high, sizeof(too_high), out, sizeof(out), 0);
        assert(strcmp(out, "????") == 0);

        /* The four-byte OVERLONG arm, which the others do not reach: F0 followed
         * by a second byte below 0x90 encodes a code point that fits in three
         * bytes or fewer. F0 80 80 AF is the classic overlong '/', the shape
         * that has walked past path checks in other software for decades. Its
         * guard is a separate clause from the F4 one above and deleting it
         * survives every other case in this file. */
        const uint8_t overlong4[] = {0xF0, 0x80, 0x80, 0xAF};
        rns_text_sanitize(overlong4, sizeof(overlong4), out, sizeof(out), 0);
        assert(strcmp(out, "????") == 0);

        /* The boundary either side of it: F0 8F .. is still overlong, F0 90 ..
         * is the first legitimate four-byte sequence (U+10000). */
        const uint8_t overlong4_edge[] = {0xF0, 0x8F, 0xBF, 0xBF};
        rns_text_sanitize(overlong4_edge, sizeof(overlong4_edge), out, sizeof(out), 0);
        assert(strcmp(out, "????") == 0);

        const uint8_t lowest4[] = {0xF0, 0x90, 0x80, 0x80};
        size_t k = rns_text_sanitize(lowest4, sizeof(lowest4), out, sizeof(out), 0);
        assert(k == 4);
        assert(memcmp(out, lowest4, 4) == 0);
    }

    /* ---- 10b. THE READ-PAST-END GUARD ON A MULTI-BYTE LEAD -----------------
     * A lead byte at the very end of the payload announces continuation bytes
     * that are not there, and the payload's buffer ends where it ends —
     * `in_len - i >= seq` is what stops the walk reading past it. That guard
     * cannot be caught by feeding a lead byte at the end of an array, because
     * whatever follows the array in memory decides the answer. So the bytes ARE
     * there and the LENGTH says they are not: a correct implementation writes
     * one '?', an implementation without the length check reads in[1], finds a
     * valid continuation byte and consumes two bytes of a one-byte payload. */
    {
        const uint8_t truncated[] = {0xC3, 0xA9};
        char out[16];
        size_t n = rns_text_sanitize(truncated, 1, out, sizeof(out), 0);
        assert(n == 1);
        assert(strcmp(out, "?") == 0);

        /* Same shape for the three and four byte leads. */
        const uint8_t three[] = {0xE2, 0x82, 0xAC};
        n = rns_text_sanitize(three, 2, out, sizeof(out), 0);
        assert(n == 2);
        assert(strcmp(out, "??") == 0);

        const uint8_t four[] = {0xF0, 0x9F, 0x98, 0x80};
        n = rns_text_sanitize(four, 3, out, sizeof(out), 0);
        assert(n == 3);
        assert(strcmp(out, "???") == 0);
    }

    /* ---- 11. the sanitiser stops on a character boundary, inside out_cap ---
     * The card body is finite and a payload is up to 383, so the cut is the
     * normal case for a long message rather than an edge. Half a UTF-8 sequence
     * on the screen is a mojibake bug — and the boundary test is `o + need >=
     * out_cap`, not `>`, because at equality the sequence would fill the buffer
     * exactly and the terminator would be written one byte past its end. The
     * canary is what proves that: it sits immediately after the destination. */
    {
        struct { char out[4]; char canary; } g;

        /* Two ASCII bytes then a two-byte sequence, into four bytes of room:
         * the sequence would end exactly at out_cap, so it must not be written
         * at all. */
        const uint8_t in[] = {'a', 'a', 0xC3, 0xA9};
        g.canary = 0x5A;
        size_t n = rns_text_sanitize(in, sizeof(in), g.out, sizeof(g.out), 0);
        assert(n == 2);
        assert(strcmp(g.out, "aa") == 0);
        assert(g.canary == 0x5A);

        /* The single-byte arm has the same boundary and the same canary: four
         * ASCII bytes into four bytes of room must keep three and terminate,
         * because the fourth would leave the NUL nowhere to go. */
        const uint8_t plain[] = {'a', 'b', 'c', 'd'};
        g.canary = 0x5A;
        n = rns_text_sanitize(plain, sizeof(plain), g.out, sizeof(g.out), 0);
        assert(n == 3);
        assert(strcmp(g.out, "abc") == 0);
        assert(g.canary == 0x5A);

        /* And so does the replacement arm, which takes the same path. */
        const uint8_t ctrl[] = {0x01, 0x02, 0x03, 0x04};
        g.canary = 0x5A;
        n = rns_text_sanitize(ctrl, sizeof(ctrl), g.out, sizeof(g.out), 0);
        assert(n == 3);
        assert(strcmp(g.out, "...") == 0);
        assert(g.canary == 0x5A);

        /* The same at the four-byte width. */
        struct { char out[6]; char canary; } h;
        const uint8_t in4[] = {'a', 'a', 0xF0, 0x9F, 0x98, 0x80};
        h.canary = 0x5A;
        n = rns_text_sanitize(in4, sizeof(in4), h.out, sizeof(h.out), 0);
        assert(n == 2);
        assert(strcmp(h.out, "aa") == 0);
        assert(h.canary == 0x5A);

        /* Three two-byte sequences into room for two plus the terminator: the
         * third must not be split. */
        const uint8_t seqs[] = {0xC3, 0xA9, 0xC3, 0xA9, 0xC3, 0xA9};
        char out5[5];
        n = rns_text_sanitize(seqs, sizeof(seqs), out5, sizeof(out5), 0);
        assert(n == 4);
        assert(out5[4] == '\0');
        assert(memcmp(out5, seqs, 4) == 0);

        /* An output buffer of one byte holds nothing but the terminator. */
        char tiny[1];
        n = rns_text_sanitize(seqs, sizeof(seqs), tiny, sizeof(tiny), 0);
        assert(n == 0);
        assert(tiny[0] == '\0');
    }

    /* ---- 12. the character budget is codepoints, not bytes -----------------
     * The card renderer walks the body by codepoint and paints a fixed number of
     * columns and rows, so this is the budget that decides what the screen can
     * actually show. A byte budget would cut a Cyrillic message at half the text
     * the screen has room for. */
    {
        char out[64];

        /* Four two-byte codepoints, budget of two characters: four bytes out. */
        const uint8_t cyr[] = {0xD0, 0x9F, 0xD1, 0x80, 0xD0, 0xB8, 0xD0, 0xB2};
        size_t n = rns_text_sanitize(cyr, sizeof(cyr), out, sizeof(out), 2);
        assert(n == 4);
        assert(memcmp(out, cyr, 4) == 0);
        assert(out[4] == '\0');

        /* The same budget over ASCII is the same number of bytes. */
        const uint8_t ascii[] = "abcdef";
        n = rns_text_sanitize(ascii, 6, out, sizeof(out), 2);
        assert(n == 2);
        assert(strcmp(out, "ab") == 0);

        /* A budget at or above the character count changes nothing, and zero
         * means no budget at all. */
        n = rns_text_sanitize(cyr, sizeof(cyr), out, sizeof(out), 4);
        assert(n == sizeof(cyr));
        n = rns_text_sanitize(cyr, sizeof(cyr), out, sizeof(out), 99);
        assert(n == sizeof(cyr));
        n = rns_text_sanitize(cyr, sizeof(cyr), out, sizeof(out), 0);
        assert(n == sizeof(cyr));

        /* A replaced byte costs one character of the budget, like any other. */
        const uint8_t mixed[] = {0xFF, 0xFF, 'a', 'b'};
        n = rns_text_sanitize(mixed, sizeof(mixed), out, sizeof(out), 3);
        assert(n == 3);
        assert(strcmp(out, "??a") == 0);
    }

    /* ---- 13. THE INVARIANT: bytes written == input bytes consumed ----------
     * The caller computes "how much of this message did not fit" as
     * in_len - return, which is only true because every branch of the mapping is
     * length-preserving: one output byte per replaced input byte, N bytes copied
     * for an N-byte sequence. An escape expansion ("\\x07" for a control byte,
     * say) would break that quietly and put a wrong number on the card, so the
     * property is pinned over a corpus that reaches every branch: with room to
     * spare and no character budget, the answer must be exactly in_len. */
    {
        char out[2048];

        /* Every single byte value, alone. */
        for (int b = 0; b < 256; b++) {
            uint8_t one = (uint8_t)b;
            assert(rns_text_sanitize(&one, 1, out, sizeof(out), 0) == 1);
        }

        /* Every byte value as a lead with three continuation bytes behind it —
         * this reaches the valid, overlong, surrogate and out-of-range arms. */
        for (int b = 0; b < 256; b++) {
            uint8_t seq[4] = {(uint8_t)b, 0x9F, 0x98, 0x80};
            assert(rns_text_sanitize(seq, sizeof(seq), out, sizeof(out), 0) == 4);
        }

        /* And a long mixed payload at the real ceiling. */
        uint8_t big[RNS_INBOX_PAYLOAD_MAX];
        for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 5u + i / 3u);
        assert(rns_text_sanitize(big, sizeof(big), out, sizeof(out), 0) ==
               sizeof(big));
    }

    return 0;
}
