#pragma once
/*
 * base64url.h — pure, header-only base64url (RFC 4648 §5) decoder.
 *
 * Host-testable, Arduino/RTOS-free (only <string.h>/<stddef.h>/<stdint.h>), the
 * same shape as lxmf_codec.h / agents_chat_route.h: `static inline`, no dynamic
 * allocation, no global state, every write bounded against the supplied cap.
 *
 * WHY IT EXISTS. The MeshCore private-DM transport is TEXT-ONLY (a `const char *`
 * all the way down, src/mesh/mc_client.cpp). The LXMF opportunistic wire, on the
 * other hand, is RAW BINARY — source(16)+sig(64)+msgpack — and carries NUL and
 * arbitrary bytes and the pipe byte '|' that the mesh frame grammar uses as its
 * field separator. So each LXMF chunk is carried base64url-encoded: the alphabet
 * A-Za-z0-9-_ contains no '|', no NUL, and survives a C string unchanged. This
 * file is the RECEIVE decode; the gateway fragmenter does the matching encode.
 *
 * ALPHABET (RFC 4648 §5, "URL and filename safe"): A-Z=0..25, a-z=26..51,
 * 0-9=52..61, '-'=62, '_'=63. NO padding ('=' is rejected, not consumed): the
 * frame carries exactly the significant characters. This decoder is STRICT — a
 * canonical decoder for a wire WE define on both ends:
 *   - any byte outside the alphabet (incl. '=', whitespace, '|') → reject;
 *   - a length ≡ 1 (mod 4) is impossible for real base64 → reject;
 *   - the trailing partial group's pad bits MUST be zero (a canonical encoder
 *     emits zero there) → nonzero pad bits reject.
 * Rejection is a clean `false` with no partial-output promise; never an OOB write.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* Decode `in`/`in_len` base64url text into `out` (cap `out_cap`); on success
 * *out_len holds the byte count. Returns false on any invalid character, an
 * impossible length, nonzero trailing pad bits, or if the output would overflow
 * out_cap. On false the contents of `out` are unspecified but bounded. An empty
 * input decodes to zero bytes and returns true. */
static inline bool b64url_decode(const char *in, size_t in_len,
                                 uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!in || !out) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        int v;
        if      (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-')             v = 62;
        else if (c == '_')             v = 63;
        else return false;                     /* invalid alphabet (incl '=', ' ', '|') */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return false;    /* output overflow */
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    /* Leftover bits are 0, 2, 4 or 6. Six means a length ≡ 1 (mod 4): impossible
     * for real base64 (a single 6-bit char cannot encode any byte). Two/four are
     * the tail of a 2- or 3-char final group and MUST be zero pad bits. */
    if (bits >= 6) return false;
    if (bits > 0 && (acc & (uint32_t)((1u << bits) - 1u)) != 0) return false;
    if (out_len) *out_len = o;
    return true;
}
