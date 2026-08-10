#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/mesh/utf8_chunk.h"

static void assert_chunks_reassemble(const char *text, size_t limit) {
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(text);
    const size_t raw_len = strlen(text);
    size_t offset = 0;
    while (offset < raw_len) {
        const size_t take = mesh_utf8_chunk_len(raw, raw_len, offset, limit);
        assert(take > 0);
        assert(take <= limit);
        if (offset + take < raw_len)
            assert((raw[offset + take] & 0xC0) != 0x80);
        offset += take;
    }
    assert(offset == raw_len);
}

int main() {
    const char final_cyrillic[] = "klev\xD0\xBE";
    const size_t cyr_len = strlen(final_cyrillic);
    const uint8_t *cyr = reinterpret_cast<const uint8_t *>(final_cyrillic);
    assert(mesh_utf8_chunk_len(cyr, cyr_len, 0, 64) == cyr_len);
    assert(mesh_utf8_chunk_len(cyr, cyr_len, 0, cyr_len - 1) == cyr_len - 2);

    const char emoji[] = "abcd\xF0\x9F\x9A\x80tail";
    assert_chunks_reassemble(emoji, 5);

    const char multipart[] =
        "0123456789\xD0\xB0\xD0\xB1\xD0\xB2\xD0\xB3"
        "ABCDEFGHIJ\xF0\x9F\x9A\x80XYZ";
    assert_chunks_reassemble(multipart, 11);

    const uint8_t ascii[] = {'a', 'b', 'c'};
    assert(mesh_utf8_chunk_len(ascii, sizeof(ascii), 0, 2) == 2);
    assert(mesh_utf8_chunk_len(ascii, sizeof(ascii), 3, 2) == 0);
    assert(mesh_utf8_chunk_len(nullptr, 3, 0, 2) == 0);
    return 0;
}
