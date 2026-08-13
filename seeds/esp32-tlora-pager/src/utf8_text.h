#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Strictly decode one UTF-8 scalar from a NUL-terminated string. */
static inline size_t utf8_text_decode(const char *text, uint32_t *codepoint) {
    if (!text || !text[0]) return 0;
    const uint8_t *p = (const uint8_t *)text;
    uint32_t cp = 0;
    size_t n = 0;

    if (p[0] < 0x80) {
        cp = p[0];
        n = 1;
    } else if (p[0] >= 0xC2 && p[0] <= 0xDF &&
               p[1] && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        n = 2;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF && p[1] && p[2] &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               !(p[0] == 0xE0 && p[1] < 0xA0) &&
               !(p[0] == 0xED && p[1] >= 0xA0)) {
        cp = ((uint32_t)(p[0] & 0x0F) << 12) |
             ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
        n = 3;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4 && p[1] && p[2] && p[3] &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               (p[3] & 0xC0) == 0x80 &&
               !(p[0] == 0xF0 && p[1] < 0x90) &&
               !(p[0] == 0xF4 && p[1] >= 0x90)) {
        cp = ((uint32_t)(p[0] & 0x07) << 18) |
             ((uint32_t)(p[1] & 0x3F) << 12) |
             ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        n = 4;
    }
    if (n && codepoint) *codepoint = cp;
    return n;
}

static inline size_t utf8_text_cells(const char *text) {
    if (!text) return 0;
    size_t cells = 0;
    for (size_t pos = 0; text[pos]; cells++) {
        size_t n = utf8_text_decode(text + pos, NULL);
        pos += n ? n : 1;  /* malformed input becomes one replacement cell */
    }
    return cells;
}

/* Copy whole code points, replacing malformed bytes with '?'. When ellipsis
 * is requested, max_cells includes the three cells occupied by "...". */
static inline bool utf8_text_copy(char *out, size_t out_n, const char *text,
                                  size_t max_cells, bool ellipsis) {
    if (!out || out_n == 0) return false;
    if (!text) text = "";

    size_t total = utf8_text_cells(text);
    bool clipped = total > max_cells;
    size_t content_cells = max_cells;
    if (clipped && ellipsis)
        content_cells = max_cells >= 3 ? max_cells - 3 : 0;

    size_t src = 0, used = 0, cells = 0;
    while (text[src] && cells < content_cells) {
        size_t n = utf8_text_decode(text + src, NULL);
        const char *piece = text + src;
        if (!n) {
            piece = "?";
            n = 1;
        }
        if (used + n >= out_n) {
            clipped = true;
            break;
        }
        memcpy(out + used, piece, n);
        used += n;
        src += utf8_text_decode(text + src, NULL) ? n : 1;
        cells++;
    }
    if (text[src]) clipped = true;
    if (clipped && ellipsis && max_cells >= 3 && used + 3 < out_n) {
        memcpy(out + used, "...", 3);
        used += 3;
    }
    out[used] = '\0';
    return clipped;
}
