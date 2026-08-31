#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/utf8_text.h"

int main(void) {
    char out[96];

    assert(!utf8_text_copy(out, sizeof(out), "Привет, мир", 32, true));
    assert(strcmp(out, "Привет, мир") == 0);

    assert(utf8_text_copy(out, sizeof(out), "abcПриветxyz", 9, true));
    assert(strcmp(out, "abcПри...") == 0);
    assert(utf8_text_cells(out) == 9);

    assert(!utf8_text_copy(out, sizeof(out), "123456789", 9, true));
    assert(strcmp(out, "123456789") == 0);

    const char invalid[] = {'A', (char)0xD0, 'B', '\0'};
    assert(!utf8_text_copy(out, sizeof(out), invalid, 8, false));
    assert(strcmp(out, "A?B") == 0);

    char tight[6];
    assert(utf8_text_copy(tight, sizeof(tight), "ЖЖЖ", 8, false));
    assert(strcmp(tight, "ЖЖ") == 0);

    bool content = false;
    assert(utf8_text_is_printable("Готово", &content) && content);
    assert(utf8_text_is_printable(" café ", &content) && content);
    assert(utf8_text_is_printable("   ", &content) && !content);
    assert(!utf8_text_is_printable("line\nbreak", &content));
    assert(!utf8_text_is_printable(invalid, &content));

    puts("utf8 text tests: OK");
    return 0;
}
