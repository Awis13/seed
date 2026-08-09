#pragma once

#include <stddef.h>
#include <stdint.h>

/* Return at most limit bytes without ending inside a UTF-8 code point.
 * offset must already point at a code-point boundary in a valid UTF-8 string. */
static inline size_t mesh_utf8_chunk_len(const uint8_t *raw, size_t raw_len,
                                         size_t offset, size_t limit) {
    if (!raw || offset >= raw_len || limit == 0) return 0;

    size_t remaining = raw_len - offset;
    size_t take = remaining < limit ? remaining : limit;
    if (take == remaining) return take;

    /* raw[offset + take] is the first byte left for the next chunk. Back up
       while that byte is a continuation byte, leaving the whole code point
       for the next frame. */
    while (take > 0 && (raw[offset + take] & 0xC0) == 0x80) take--;
    return take;
}
