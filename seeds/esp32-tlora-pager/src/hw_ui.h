// T-Lora Pager front panel.
//
// Home screen is the tembed clock face (Advisor/Scriptor vibe): big warm-white
// time, amber seconds, slate chrome, unread badge, breathing rule on crit.
// Geometry is for the 480x222 ST7796 landscape panel.

#pragma once

#include <Arduino.h>

bool hw_ui_begin();
bool hw_ui_ready();
bool hw_ui_expand_ok();

// Drive one XL9555 pin (e.g. EXP_AMP_EN). No-op if expander missing.
void hw_xl_pin(uint8_t exp_pin, bool high);

void hw_ui_set_brightness(uint8_t level);
uint8_t hw_ui_get_brightness();

// Which face is currently up.
enum HwUiScreen : uint8_t {
    HW_UI_CLOCK = 0,
    HW_UI_NOTIFY,
    HW_UI_MENU,
    HW_UI_MSGLIST,
    HW_UI_INFO,
    HW_UI_REPLY,
};

HwUiScreen hw_ui_screen();

// Full home paint (chrome). Call after WiFi/token settle, then clock_tick.
void hw_ui_show_clock();

// One-second field update on the clock face. No-op if another screen is up.
// batt is "BAT 87%" / "BAT --" / empty (no gauge). Drawn centered in the header.
void hw_ui_clock_tick(const char *version,
                      const char *batt,
                      const char *ip_or_status,
                      const char *row1l,
                      const char *row1r,
                      const char *row2,
                      int unread,
                      bool time_ok,
                      int hour, int minute, int second,
                      const char *date_str,
                      bool crit_unread);

// 80ms breathing hairline under the header when crit is unread. Clock only.
void hw_ui_clock_rule_tick(bool crit_unread);

// Progress band under the note row. pct < 0 clears; 0..100 fills amber.
void hw_ui_clock_bar(int pct);

// Notification card.
void hw_ui_show_notify(const char *level,
                       const char *source,
                       const char *title,
                       const char *body,
                       int unread);

// Menu: MESSAGES / INFO / BACK. selected is 0..2.
void hw_ui_show_menu(int selected);

// Message list: one title per row (already truncated). selected is row index.
#define HW_UI_MSGLIST_MAX 8
void hw_ui_show_msglist(const char *const *titles,
                        const char *const *levels,
                        int count,
                        int selected);

// Device info (version, IP, host, token, free heap).
void hw_ui_show_info(const char *version,
                     const char *host,
                     const char *ip,
                     const char *token,
                     uint32_t free_heap,
                     int unread);

void hw_ui_invalidate_clock();

// Free-text reply composer. buffer is the live draft (ASCII).
// caps/sym badges reflect keyboard modifiers.
void hw_ui_show_reply(const char *title,
                      const char *buffer,
                      bool caps,
                      bool symbol);
