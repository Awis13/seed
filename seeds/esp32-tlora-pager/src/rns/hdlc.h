#pragma once

/*
 * rns/hdlc.h — HDLC framing for the Reticulum TCP interface.
 *
 * Reticulum's Python TCPServerInterface spawns one client interface per
 * accepted socket and that spawned config carries no `kiss_framing` key, so the
 * server always speaks HDLC. There is no banner, no handshake and no
 * application keepalive: the first byte on the socket is already a frame FLAG.
 *
 *   FLAG 0x7E  frame delimiter
 *   ESC  0x7D  escape prefix
 *   0x7D -> 7D 5D, 0x7E -> 7D 5E   (payload byte XOR 0x20)
 *
 * Free functions and a plain struct, no Arduino and no C++ runtime, so the same
 * code compiles on the host for tools/test_rns_hdlc.cpp. Nothing here
 * allocates: the decoder writes into a caller-owned buffer whose capacity is
 * also the largest frame it will accept.
 *
 * Framing follows the reference HDLC reader (TCPInterface.py read_loop, the
 * `else` branch — the in_frame/escape state machine above it is the KISS path
 * and is not what a spawned TCPClientInterface speaks). That reader restarts
 * its search from the closing FLAG rather than consuming it, so one FLAG closes
 * a frame and opens the next; `7E A 7E B 7E` is two frames there and two frames
 * here. The doubled `7E A 7E 7E B 7E` a Python sender actually emits decodes
 * the same way in both, because a FLAG pair with nothing between it is an idle
 * line and not a zero-length frame.
 *
 * Length policy is where the two genuinely differ, and this is the stricter
 * side. The lower bound matches: a frame is accepted only when it is strictly
 * longer than the RNS minimum header. The upper bound has no counterpart at all
 * in the reference HDLC reader — that one appends to frame_buffer without any
 * limit and delivers whatever it finds between two FLAGs, so an over-long frame
 * is delivered in full and a peer that never sends a closing FLAG grows its
 * memory without bound. (Its KISS reader does carry an HW_MTU check, but that
 * one stops appending and still delivers the truncated buffer.) Here a frame
 * longer than the interface's HW MTU is rejected outright and counted, and the
 * bytes past the MTU are discarded rather than buffered, so neither the frame
 * nor a missing closing FLAG can cost us memory.
 */

#include <stddef.h>
#include <stdint.h>

#define RNS_HDLC_FLAG     0x7E
#define RNS_HDLC_ESC      0x7D
#define RNS_HDLC_ESC_MASK 0x20

/* Worst case on the wire: every payload byte escaped, plus both FLAGs. */
static inline size_t rns_hdlc_encoded_max(size_t payload_len) {
    return payload_len * 2 + 2;
}

/* Encode one frame. Returns bytes written, or 0 when out is too small. */
static inline size_t rns_hdlc_encode(const uint8_t *in, size_t in_len,
                                     uint8_t *out, size_t out_cap) {
    if (!out || out_cap < rns_hdlc_encoded_max(in_len)) return 0;
    if (in_len > 0 && !in) return 0;

    size_t n = 0;
    out[n++] = RNS_HDLC_FLAG;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t b = in[i];
        if (b == RNS_HDLC_FLAG || b == RNS_HDLC_ESC) {
            out[n++] = RNS_HDLC_ESC;
            out[n++] = (uint8_t)(b ^ RNS_HDLC_ESC_MASK);
        } else {
            out[n++] = b;
        }
    }
    out[n++] = RNS_HDLC_FLAG;
    return n;
}

/* Reassembly state. Lives across reads: a frame split over several sockets
 * reads (or an ESC landing on a read boundary) resumes exactly where it left
 * off. buf/cap are caller-owned; cap is the largest frame that will be
 * accepted. */
typedef struct {
    uint8_t *buf;
    size_t cap;      /* == HW MTU; a frame longer than this is rejected */
    size_t min_len;  /* a frame of this length or shorter is rejected */
    size_t len;      /* bytes accumulated, or the frame length after a hit */
    uint8_t in_frame;
    uint8_t escape;
    uint8_t overflow;      /* current frame already exceeded cap */
    uint8_t pending_clear; /* a frame was reported; clear it on the next feed */
    uint32_t rejected_short;
    uint32_t rejected_long;
} rns_hdlc_rx;

static inline void rns_hdlc_rx_init(rns_hdlc_rx *rx, uint8_t *buf, size_t cap,
                                    size_t min_len) {
    if (!rx) return;
    rx->buf = buf;
    rx->cap = cap;
    rx->min_len = min_len;
    rx->len = 0;
    rx->in_frame = 0;
    rx->escape = 0;
    rx->overflow = 0;
    rx->pending_clear = 0;
    rx->rejected_short = 0;
    rx->rejected_long = 0;
}

/* Drop any half-assembled frame. Called when the socket dies: the bytes that
 * would have completed it are never coming. Counters survive. */
static inline void rns_hdlc_rx_reset(rns_hdlc_rx *rx) {
    if (!rx) return;
    rx->len = 0;
    rx->in_frame = 0;
    rx->escape = 0;
    rx->overflow = 0;
    rx->pending_clear = 0;
}

/*
 * Feed bytes until one complete frame is assembled.
 *
 * Returns 1 when rx->buf[0 .. rx->len) holds an accepted frame, and sets
 * *consumed to how much of data was eaten getting there — the caller resumes
 * from that offset. Returns 0 when the whole input was consumed without
 * completing a frame. Rejected frames are counted, never reported.
 */
static inline int rns_hdlc_rx_feed(rns_hdlc_rx *rx, const uint8_t *data,
                                   size_t len, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!rx || !rx->buf || !data) return 0;

    if (rx->pending_clear) {
        rx->pending_clear = 0;
        rx->len = 0;
    }

    size_t i = 0;
    int hit = 0;
    while (i < len) {
        uint8_t b = data[i++];

        if (b == RNS_HDLC_FLAG) {
            int accept = 0;
            if (rx->in_frame && rx->len > 0) {
                if (rx->overflow) {
                    rx->rejected_long++;
                } else if (rx->len <= rx->min_len) {
                    rx->rejected_short++;
                } else {
                    accept = 1;
                }
            }
            /* This FLAG closes the frame above and opens the next one. */
            rx->in_frame = 1;
            rx->escape = 0;
            rx->overflow = 0;
            if (accept) {
                rx->pending_clear = 1;
                hit = 1;
                break;
            }
            rx->len = 0;
            continue;
        }

        if (!rx->in_frame) continue;  /* garbage ahead of the first FLAG */

        if (b == RNS_HDLC_ESC) {
            rx->escape = 1;
            continue;
        }
        if (rx->escape) {
            b = (uint8_t)(b ^ RNS_HDLC_ESC_MASK);
            rx->escape = 0;
        }
        if (rx->len >= rx->cap) {
            /* Over MTU: keep counting to the closing FLAG, buffer no more. */
            rx->overflow = 1;
            continue;
        }
        rx->buf[rx->len++] = b;
    }

    if (consumed) *consumed = i;
    return hit;
}
