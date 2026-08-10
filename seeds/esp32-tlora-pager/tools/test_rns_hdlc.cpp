/*
 * Host tests for the Reticulum TCP interface's HDLC codec.
 *
 * The codec is the one piece of skills/rns.cpp that can be exercised without a
 * radio, a socket or a board: framing bugs there look exactly like a dead link,
 * so they are pinned here instead of being debugged over WiFi.
 *
 * RNS_MIN mirrors RNS::Type::Reticulum::HEADER_MINSIZE (2 + 1 + 128/8).
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/rns/hdlc.h"

#define RNS_MIN 19
#define RNS_MTU 1024

static uint8_t g_frame[RNS_MTU];
static rns_hdlc_rx g_rx;

static void rx_fresh(void) {
    rns_hdlc_rx_init(&g_rx, g_frame, RNS_MTU, RNS_MIN);
}

/* Feed a whole buffer, collecting every frame it completes. Returns how many
 * frames came out; the last one stays in g_rx.buf/g_rx.len. */
static int rx_feed_all(const uint8_t *data, size_t len,
                       uint8_t out[][RNS_MTU], size_t *out_len, int out_max) {
    size_t off = 0;
    int n = 0;
    while (off < len) {
        size_t used = 0;
        int got = rns_hdlc_rx_feed(&g_rx, data + off, len - off, &used);
        off += used;
        assert(used > 0);
        if (!got) continue;
        assert(n < out_max);
        if (out) memcpy(out[n], g_rx.buf, g_rx.len);
        if (out_len) out_len[n] = g_rx.len;
        n++;
    }
    return n;
}

/* Encode payload, decode it back, assert it survived byte for byte. */
static void assert_round_trip(const uint8_t *payload, size_t len) {
    uint8_t wire[RNS_MTU * 2 + 2];
    size_t wire_len = rns_hdlc_encode(payload, len, wire, sizeof(wire));
    assert(wire_len > 0);
    assert(wire[0] == RNS_HDLC_FLAG);
    assert(wire[wire_len - 1] == RNS_HDLC_FLAG);

    uint8_t out[2][RNS_MTU];
    size_t out_len[2];
    rx_fresh();
    int n = rx_feed_all(wire, wire_len, out, out_len, 2);
    assert(n == 1);
    assert(out_len[0] == len);
    assert(memcmp(out[0], payload, len) == 0);
}

/* A payload long enough to be accepted, with a caller-chosen filler. */
static size_t make_payload(uint8_t *buf, size_t len, uint8_t fill) {
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(fill + i);
    return len;
}

int main() {
    uint8_t buf[RNS_MTU * 2 + 2];
    uint8_t wire[RNS_MTU * 4 + 8];

    /* --- encoder shape ------------------------------------------------- */
    /* Empty payload is two bare FLAGs on the wire... */
    size_t n = rns_hdlc_encode(NULL, 0, wire, sizeof(wire));
    assert(n == 2);
    assert(wire[0] == RNS_HDLC_FLAG && wire[1] == RNS_HDLC_FLAG);
    /* ...and the decoder reports nothing for it: a run of FLAGs with no bytes
     * between them is an idle line, not a zero-length frame. */
    rx_fresh();
    assert(rx_feed_all(wire, n, NULL, NULL, 1) == 0);
    assert(g_rx.rejected_short == 0 && g_rx.rejected_long == 0);

    /* Escaping. */
    const uint8_t esc_cases[] = {RNS_HDLC_FLAG, RNS_HDLC_ESC};
    n = rns_hdlc_encode(esc_cases, 2, wire, sizeof(wire));
    assert(n == 6);
    assert(wire[1] == RNS_HDLC_ESC && wire[2] == (RNS_HDLC_FLAG ^ RNS_HDLC_ESC_MASK));
    assert(wire[3] == RNS_HDLC_ESC && wire[4] == (RNS_HDLC_ESC ^ RNS_HDLC_ESC_MASK));

    /* Refuses to write into a buffer that cannot hold the worst case. */
    assert(rns_hdlc_encode(esc_cases, 2, wire, 5) == 0);

    /* --- round trips ---------------------------------------------------- */
    size_t len = make_payload(buf, 32, 0x40);
    assert_round_trip(buf, len);

    /* A payload carrying the frame delimiter. */
    len = make_payload(buf, 32, 0x40);
    buf[0] = RNS_HDLC_FLAG;
    buf[17] = RNS_HDLC_FLAG;
    buf[31] = RNS_HDLC_FLAG;
    assert_round_trip(buf, len);

    /* A payload carrying the escape byte. */
    len = make_payload(buf, 32, 0x40);
    buf[3] = RNS_HDLC_ESC;
    buf[31] = RNS_HDLC_ESC;
    assert_round_trip(buf, len);

    /* Both, adjacent, in either order and at both ends. */
    len = make_payload(buf, 32, 0x40);
    buf[0] = RNS_HDLC_ESC;
    buf[1] = RNS_HDLC_FLAG;
    buf[15] = RNS_HDLC_FLAG;
    buf[16] = RNS_HDLC_ESC;
    buf[30] = RNS_HDLC_ESC;
    buf[31] = RNS_HDLC_FLAG;
    assert_round_trip(buf, len);

    /* Every byte value, so no escape case is missed. */
    for (size_t i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    assert_round_trip(buf, 256);

    /* Exactly at the MTU, and exactly one byte over the minimum. */
    make_payload(buf, RNS_MTU, 0x10);
    assert_round_trip(buf, RNS_MTU);
    make_payload(buf, RNS_MIN + 1, 0x10);
    assert_round_trip(buf, RNS_MIN + 1);

    /* --- back-to-back frames sharing a flag ------------------------------ */
    {
        uint8_t a[24], b[40];
        make_payload(a, sizeof(a), 0x01);
        make_payload(b, sizeof(b), 0x80);
        b[5] = RNS_HDLC_ESC;
        b[6] = RNS_HDLC_FLAG;

        size_t la = rns_hdlc_encode(a, sizeof(a), wire, sizeof(wire));
        /* Drop the second frame's opening FLAG: one FLAG closes the first and
         * opens the second. */
        size_t lb = rns_hdlc_encode(b, sizeof(b), wire + la, sizeof(wire) - la);
        memmove(wire + la, wire + la + 1, lb - 1);
        size_t total = la + lb - 1;

        uint8_t out[3][RNS_MTU];
        size_t out_len[3];
        rx_fresh();
        int got = rx_feed_all(wire, total, out, out_len, 3);
        assert(got == 2);
        assert(out_len[0] == sizeof(a) && memcmp(out[0], a, sizeof(a)) == 0);
        assert(out_len[1] == sizeof(b) && memcmp(out[1], b, sizeof(b)) == 0);

        /* The doubled-FLAG form a Python sender actually emits must decode to
         * the same two frames. */
        la = rns_hdlc_encode(a, sizeof(a), wire, sizeof(wire));
        lb = rns_hdlc_encode(b, sizeof(b), wire + la, sizeof(wire) - la);
        rx_fresh();
        got = rx_feed_all(wire, la + lb, out, out_len, 3);
        assert(got == 2);
        assert(out_len[0] == sizeof(a) && memcmp(out[0], a, sizeof(a)) == 0);
        assert(out_len[1] == sizeof(b) && memcmp(out[1], b, sizeof(b)) == 0);
    }

    /* --- a frame split across two reads ---------------------------------- */
    {
        uint8_t payload[64];
        make_payload(payload, sizeof(payload), 0x20);
        /* Put an ESC at the very byte the split lands on, so the escape state
         * itself has to survive the boundary. */
        payload[29] = RNS_HDLC_ESC;
        size_t wl = rns_hdlc_encode(payload, sizeof(payload), wire, sizeof(wire));

        for (size_t split = 1; split < wl; split++) {
            uint8_t out[2][RNS_MTU];
            size_t out_len[2];
            rx_fresh();
            int got = rx_feed_all(wire, split, out, out_len, 2);
            got += rx_feed_all(wire + split, wl - split, out + got, out_len + got,
                               2 - got);
            assert(got == 1);
            assert(out_len[0] == sizeof(payload));
            assert(memcmp(out[0], payload, sizeof(payload)) == 0);
        }
    }

    /* --- rejections ------------------------------------------------------ */
    /* Over the MTU: dropped, counted, and the decoder recovers on the frame
     * that follows rather than staying wedged. */
    {
        uint8_t big[RNS_MTU + 1];
        make_payload(big, sizeof(big), 0x55);
        size_t wl = rns_hdlc_encode(big, sizeof(big), wire, sizeof(wire));
        rx_fresh();
        assert(rx_feed_all(wire, wl, NULL, NULL, 1) == 0);
        assert(g_rx.rejected_long == 1);
        assert(g_rx.rejected_short == 0);

        uint8_t good[32];
        make_payload(good, sizeof(good), 0x11);
        wl = rns_hdlc_encode(good, sizeof(good), wire, sizeof(wire));
        uint8_t out[1][RNS_MTU];
        size_t out_len[1];
        assert(rx_feed_all(wire, wl, out, out_len, 1) == 1);
        assert(out_len[0] == sizeof(good));
    }

    /* At or below the minimum header: dropped and counted. */
    {
        rx_fresh();
        for (size_t l = 1; l <= RNS_MIN; l++) {
            make_payload(buf, l, 0x60);
            size_t wl = rns_hdlc_encode(buf, l, wire, sizeof(wire));
            assert(rx_feed_all(wire, wl, NULL, NULL, 1) == 0);
        }
        assert(g_rx.rejected_short == RNS_MIN);
        assert(g_rx.rejected_long == 0);
    }

    /* An ESC with the closing FLAG right behind it — a frame truncated on the
     * wire mid-escape. The dangling escape must not survive into the frame that
     * follows: dropping `rx->escape = 0` from the FLAG branch would XOR 0x20
     * into the next frame's first byte and nothing else would notice. */
    {
        uint8_t good[32];
        make_payload(good, sizeof(good), 0x5A);
        good[0] = 0xA5;  /* neither FLAG nor ESC, and visibly wrong under ^0x20 */

        size_t wl = 0;
        wire[wl++] = RNS_HDLC_FLAG;
        for (size_t i = 0; i < 24; i++) wire[wl++] = (uint8_t)(0x30 + i);
        wire[wl++] = RNS_HDLC_ESC;   /* escape prefix with nothing after it */
        size_t gl = rns_hdlc_encode(good, sizeof(good), wire + wl,
                                    sizeof(wire) - wl);
        assert(gl > 0);
        wl += gl;

        uint8_t out[3][RNS_MTU];
        size_t out_len[3];
        rx_fresh();
        int got = rx_feed_all(wire, wl, out, out_len, 3);
        assert(got == 2);
        /* The truncated frame is long enough to be accepted, and arrives
         * without the trailing ESC — that byte was a prefix, not data. */
        assert(out_len[0] == 24);
        assert(out[0][0] == 0x30 && out[0][23] == 0x47);
        /* The frame behind it is untouched. */
        assert(out_len[1] == sizeof(good));
        assert(out[1][0] == 0xA5);
        assert(memcmp(out[1], good, sizeof(good)) == 0);
        assert(g_rx.rejected_short == 0 && g_rx.rejected_long == 0);
    }

    /* Garbage ahead of the first FLAG is skipped, not framed. */
    {
        uint8_t good[32];
        make_payload(good, sizeof(good), 0x77);
        uint8_t junk[] = {0x01, 0x02, 0x03};
        memcpy(wire, junk, sizeof(junk));
        size_t wl = rns_hdlc_encode(good, sizeof(good), wire + sizeof(junk),
                                    sizeof(wire) - sizeof(junk));
        uint8_t out[1][RNS_MTU];
        size_t out_len[1];
        rx_fresh();
        assert(rx_feed_all(wire, sizeof(junk) + wl, out, out_len, 1) == 1);
        assert(out_len[0] == sizeof(good));
        assert(g_rx.rejected_short == 0 && g_rx.rejected_long == 0);
    }

    /* A reset mid-frame throws the partial away and keeps the counters. */
    {
        uint8_t good[32];
        make_payload(good, sizeof(good), 0x33);
        size_t wl = rns_hdlc_encode(good, sizeof(good), wire, sizeof(wire));
        rx_fresh();
        size_t used = 0;
        assert(rns_hdlc_rx_feed(&g_rx, wire, wl - 4, &used) == 0);
        assert(g_rx.len > 0);
        rns_hdlc_rx_reset(&g_rx);
        assert(g_rx.len == 0 && g_rx.in_frame == 0);
        /* The tail of the old frame is now leading garbage; the next whole
         * frame still decodes. */
        uint8_t out[1][RNS_MTU];
        size_t out_len[1];
        assert(rx_feed_all(wire, wl, out, out_len, 1) == 1);
        assert(out_len[0] == sizeof(good));
    }

    /* Defensive arguments. */
    {
        size_t used = 123;
        assert(rns_hdlc_rx_feed(NULL, wire, 4, &used) == 0);
        assert(used == 0);
        rx_fresh();
        assert(rns_hdlc_rx_feed(&g_rx, NULL, 4, &used) == 0);
        assert(used == 0);
        assert(rns_hdlc_encode(NULL, 8, wire, sizeof(wire)) == 0);
    }

    return 0;
}
