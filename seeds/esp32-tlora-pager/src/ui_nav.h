// Pure back-navigation policy for the T-Lora pager front panel.
//
// The BACKSPACE key and the per-screen BACK rows share one question: from the
// screen that is up, where does "go back" land, and is BACKSPACE editing text
// instead of navigating? That decision is plain data — no panel, no bus — so it
// lives here and is proved by value in tools/test_ui_nav. hw_ui.cpp binds these
// ids to HwUiScreen with static_asserts so the two never drift.

#pragma once

#include <stdint.h>

// Mirror of HwUiScreen (hw_ui.h). Kept as an independent enum so this header
// stays free of the Arduino include and remains host-testable.
enum UiNavScreen : uint8_t {
    UINAV_CLOCK = 0,
    UINAV_NOTIFY,
    UINAV_CARD_ACT,
    UINAV_MENU,
    UINAV_AGENTS,
    UINAV_AGENT_CHAT,
    UINAV_AGENT_ACT,
    UINAV_AGENT_SESSIONS,
    UINAV_MSGLIST,
    UINAV_INFO,
    UINAV_REPLY,
    UINAV_LAYOUT,
    UINAV_SETTINGS,
    UINAV_MESHCORE,
    UINAV_MESH_PING,
    UINAV_WIFI,
    UINAV_WIFI_LIST,
    UINAV_WIFI_INFO,
    UINAV_PAGE,
    UINAV_CONTACTS,
    UINAV_NET,
};

// BACKSPACE on a text-entry screen deletes a character; everywhere else it
// navigates back one screen. Only the reply/compose editor (message, WiFi
// password, agent compose all share HW_UI_REPLY) is a text-entry field.
static inline bool ui_nav_is_text_entry(uint8_t screen) {
    return screen == UINAV_REPLY;
}

// The screen that a BACK row / BACKSPACE navigates to from `screen`. Mirrors the
// per-screen BACK handling in ui_on_click(). CLOCK is the root and REPLY is text
// entry, so both return themselves (no navigation).
//
// has_rooms only matters for AGENT_CHAT: a conversation with multiple rooms
// backs out to its room list (so the list stays reachable after we drop the
// intermediate stop and open the active room directly); a single-room chat backs
// straight to the unified feed.
//
// sessions_origin only matters for AGENT_SESSIONS: the picker can be entered
// forward from the unified feed (MSGLIST) or from the contacts list (CONTACTS),
// so it backs out to whichever list opened it. Defaults to MSGLIST — the
// historical target and the value every non-forward path passes.
static inline uint8_t ui_nav_back_target(uint8_t screen, bool has_rooms,
                                         uint8_t sessions_origin = UINAV_MSGLIST) {
    switch (screen) {
    case UINAV_NOTIFY:         return UINAV_MSGLIST;
    case UINAV_CARD_ACT:       return UINAV_NOTIFY;
    case UINAV_MENU:           return UINAV_CLOCK;
    case UINAV_AGENTS:         return UINAV_MENU;
    case UINAV_AGENT_CHAT:     return has_rooms ? UINAV_AGENT_SESSIONS : UINAV_MSGLIST;
    case UINAV_AGENT_ACT:      return UINAV_AGENT_CHAT;
    case UINAV_AGENT_SESSIONS: return sessions_origin;
    case UINAV_MSGLIST:        return UINAV_MENU;
    case UINAV_INFO:           return UINAV_MENU;
    case UINAV_LAYOUT:         return UINAV_SETTINGS;
    case UINAV_SETTINGS:       return UINAV_MENU;
    case UINAV_MESHCORE:       return UINAV_MENU;
    case UINAV_MESH_PING:      return UINAV_MESHCORE;
    case UINAV_WIFI:           return UINAV_MENU;
    case UINAV_WIFI_LIST:      return UINAV_WIFI;
    case UINAV_WIFI_INFO:      return UINAV_WIFI;
    case UINAV_PAGE:           return UINAV_CLOCK;
    case UINAV_CONTACTS:       return UINAV_MENU;
    case UINAV_NET:            return UINAV_WIFI;  // reached from the WiFi menu
    default:                   return screen;  // CLOCK, REPLY: no back
    }
}

// True when BACKSPACE should navigate back from this screen (i.e. it is not text
// entry and it is not the root clock).
static inline bool ui_nav_backspace_goes_back(uint8_t screen) {
    return !ui_nav_is_text_entry(screen) && screen != UINAV_CLOCK;
}

// Forward-open policy for an AI conversation row (from the feed or the contacts
// list): a conversation with more than one session opens the session picker so
// the user chooses a room instead of being dropped into the last-active one; a
// single-session (or freshly created) conversation opens the chat directly, so
// the common case keeps its one-tap open. session_count is agents_session_count
// for the slot — the same count that gates the picker's capacity and rows.
static inline uint8_t ui_nav_conv_open_target(int session_count) {
    return session_count > 1 ? UINAV_AGENT_SESSIONS : UINAV_AGENT_CHAT;
}
