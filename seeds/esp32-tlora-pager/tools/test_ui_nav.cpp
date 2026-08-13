/*
 * Host tests for the pure back-navigation policy in src/ui_nav.h.
 *
 * BACKSPACE and the per-screen BACK rows share one decision: is BACKSPACE
 * editing text or navigating, and where does "go back" land. Drawing needs the
 * panel; this table does not. It is checked here BY VALUE so a wrong back target
 * or a lost delete-a-char cannot slip in unseen on the device.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/ui_nav.h"

int main(void) {
    // Only the reply/compose editor is a text-entry field: BACKSPACE deletes a
    // char there, and navigates everywhere else.
    assert(ui_nav_is_text_entry(UINAV_REPLY));
    for (int s = 0; s <= UINAV_PAGE; s++) {
        if (s == UINAV_REPLY) continue;
        assert(!ui_nav_is_text_entry((uint8_t)s));
    }

    // BACKSPACE goes back on every navigation face; never on the root clock or
    // in the editor.
    assert(!ui_nav_backspace_goes_back(UINAV_CLOCK));
    assert(!ui_nav_backspace_goes_back(UINAV_REPLY));
    assert(ui_nav_backspace_goes_back(UINAV_MENU));
    assert(ui_nav_backspace_goes_back(UINAV_MSGLIST));
    assert(ui_nav_backspace_goes_back(UINAV_AGENT_CHAT));
    assert(ui_nav_backspace_goes_back(UINAV_NOTIFY));
    assert(ui_nav_backspace_goes_back(UINAV_CARD_ACT));
    assert(ui_nav_backspace_goes_back(UINAV_WIFI_INFO));
    assert(ui_nav_backspace_goes_back(UINAV_PAGE));

    // Back targets mirror the BACK rows in ui_on_click(), by value.
    assert(ui_nav_back_target(UINAV_NOTIFY, false)        == UINAV_MSGLIST);
    assert(ui_nav_back_target(UINAV_CARD_ACT, false)      == UINAV_NOTIFY);
    assert(ui_nav_back_target(UINAV_MENU, false)          == UINAV_CLOCK);
    assert(ui_nav_back_target(UINAV_AGENTS, false)        == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_AGENT_ACT, false)     == UINAV_AGENT_CHAT);
    assert(ui_nav_back_target(UINAV_AGENT_SESSIONS, false) == UINAV_MSGLIST);
    assert(ui_nav_back_target(UINAV_MSGLIST, false)       == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_INFO, false)          == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_LAYOUT, false)        == UINAV_SETTINGS);
    assert(ui_nav_back_target(UINAV_SETTINGS, false)      == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_MESHCORE, false)      == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_MESH_PING, false)     == UINAV_MESHCORE);
    assert(ui_nav_back_target(UINAV_WIFI, false)          == UINAV_MENU);
    assert(ui_nav_back_target(UINAV_WIFI_LIST, false)     == UINAV_WIFI);
    assert(ui_nav_back_target(UINAV_WIFI_INFO, false)     == UINAV_WIFI);
    assert(ui_nav_back_target(UINAV_PAGE, false)          == UINAV_CLOCK);
    // Contacts backs out to the menu it was opened from, and BACKSPACE navigates
    // (it is a list, not a text-entry field).
    assert(ui_nav_back_target(UINAV_CONTACTS, false)      == UINAV_MENU);
    assert(ui_nav_backspace_goes_back(UINAV_CONTACTS));
    assert(!ui_nav_is_text_entry(UINAV_CONTACTS));

    // Network status backs out to the WiFi menu it was opened from, and BACKSPACE
    // navigates (it is a read-only status list, not a text-entry field).
    assert(ui_nav_back_target(UINAV_NET, false)           == UINAV_WIFI);
    assert(ui_nav_backspace_goes_back(UINAV_NET));
    assert(!ui_nav_is_text_entry(UINAV_NET));

    // Root and editor: no navigation (return self).
    assert(ui_nav_back_target(UINAV_CLOCK, false) == UINAV_CLOCK);
    assert(ui_nav_back_target(UINAV_REPLY, false) == UINAV_REPLY);

    // AGENT_CHAT: a multi-room conversation backs to its room list so the list
    // stays reachable after we drop the intermediate stop; a single-room chat
    // backs straight to the unified feed.
    assert(ui_nav_back_target(UINAV_AGENT_CHAT, true)  == UINAV_AGENT_SESSIONS);
    assert(ui_nav_back_target(UINAV_AGENT_CHAT, false) == UINAV_MSGLIST);

    printf("ui nav tests: OK\n");
    return 0;
}
