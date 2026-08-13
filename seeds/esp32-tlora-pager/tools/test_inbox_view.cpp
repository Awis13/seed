/* Host tests for the conversation-row helpers retained by the unified feed. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/inbox_view.h"

static void test_glyphs(void) {
    assert(inbox_transport_glyph(CONV_AGENT) == INBOX_GLYPH_AGENT);
    assert(inbox_transport_glyph(CONV_MESH) == INBOX_GLYPH_MESH);
    assert(inbox_transport_glyph(CONV_LXMF) == INBOX_GLYPH_LXMF);
    assert(inbox_transport_glyph(99) == INBOX_GLYPH_NONE);
    assert(INBOX_GLYPH_AGENT != INBOX_GLYPH_MESH);
    assert(INBOX_GLYPH_MESH != INBOX_GLYPH_LXMF);
    assert(INBOX_GLYPH_AGENT != INBOX_GLYPH_LXMF);
    assert(INBOX_UNREAD_MARK != ' ');
}

static void test_row_revalidation(void) {
    InboxRow row;
    memset(&row, 0, sizeof(row));
    snprintf(row.id, sizeof(row.id), "%s", "a1b2c3d4");

    assert(inbox_row_matches(&row, "a1b2c3d4"));
    assert(!inbox_row_matches(&row, "ff00ff00"));
    assert(!inbox_row_matches(NULL, "a1b2c3d4"));
    assert(!inbox_row_matches(&row, NULL));
    assert(!inbox_row_matches(&row, ""));

    row.id[0] = '\0';
    assert(!inbox_row_matches(&row, "anything"));
}

int main(void) {
    test_glyphs();
    test_row_revalidation();
    printf("inbox view tests: OK\n");
    return 0;
}
