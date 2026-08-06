#!/usr/bin/env bash
# Host regression tests for the shipping home-page navigation in src/ui.h.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ui="$here/../src/ui.h"
panel="$here/../src/skills/panel.cpp"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

slice_panel_ui() {
    awk '
        /host-test:begin panelui/ { grab = 1; next }
        grab && /host-test:end/ { grab = 0; next }
        grab { print }
    ' "$ui"
}

slice_panel_render() {
    awk '
        /host-test:begin panelrender/ { grab = 1; next }
        grab && /host-test:end/ { grab = 0; next }
        grab { print }
    ' "$ui"
}

slice_wrap() {
    awk '
        /host-test:begin wrap/ { grab = 1; next }
        grab && /host-test:end/ { grab = 0; next }
        grab { print }
    ' "$ui"
}

extract_char_width() {
    awk '
        /^static int ui_char_w\(/ { grab = 1 }
        grab { print }
        grab && /^}/ { exit }
    ' "$ui"
}

extract_panel_select() {
    awk '
        /^static int ui_panel_select\(/ { grab = 1 }
        grab {
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            depth += opens - closes
        }
        grab && depth == 0 { exit }
    ' "$ui"
}

{
    printf '%s\n' '#include <climits>' '#include <cstdint>' '#include <cstdio>' \
        '#include <cstring>' '#include <unistd.h>' \
        'struct FakeTft {' \
        '    int textWidth(const char *text, uint8_t font) {' \
        '        return static_cast<int>(std::strlen(text)) * (font == 1 ? 6 : 10);' \
        '    }' \
        '} tft;'
    extract_char_width
    slice_wrap
    slice_panel_ui
    printf '%s\n' \
        '#define PANEL_BODY_LEN 257' \
        '#define MSG_GLYPH_W_MIN 3' \
        '#define MSG_GLYPH_W_MAX 10'
    slice_panel_render
    printf '%s\n' \
        '#define PANEL_MAX 8' \
        '#define PANEL_KEY_LEN 17' \
        'struct Panel { char key[PANEL_KEY_LEN]; };' \
        'static uint8_t ui_panel_sel = UI_PANEL_NONE;' \
        'static char ui_panel_key[PANEL_KEY_LEN] = "";'
    extract_panel_select
    printf '%s\n' '
static int failures = 0;

static void check(bool condition, const char *what) {
    if (condition) std::printf("  ok:   %s\n", what);
    else { std::printf("  FAIL: %s\n", what); failures++; }
}

static void key(Panel &panel, const char *value) {
    std::snprintf(panel.key, sizeof(panel.key), "%s", value);
}

int main() {
    alarm(30);
    bool gaps[5] = {true, false, true, false, true};
    check(ui_panel_next(gaps, 5, 0, 1) == 2, "forward traversal skips gaps");
    check(ui_panel_next(gaps, 5, 0, -1) == 4, "backward traversal wraps over gaps");
    check(ui_panel_next(gaps, 5, 4, 1) == 0, "forward traversal wraps");
    check(ui_panel_next(gaps, 5, 0, -4) == 4, "accumulated backward steps wrap");
    check(ui_panel_next(gaps, 5, 0, 7) == 2, "accumulated forward steps wrap");
    check(ui_panel_next(gaps, 5, -1, 1) == 0, "no selection enters at the front");
    check(ui_panel_next(gaps, 5, -1, -1) == 4, "reverse entry starts at the back");

    bool one[5] = {false, false, true, false, false};
    bool none[5] = {};
    check(ui_panel_next(one, 5, 2, INT_MAX) == 2, "one live page absorbs extreme forward steps");
    check(ui_panel_next(one, 5, 2, INT_MIN) == 2, "one live page absorbs extreme backward steps");
    check(ui_panel_next(none, 5, 0, 1) == -1, "all gaps return signed minus one");
    check(static_cast<uint8_t>(ui_panel_next(none, 5, 0, 1)) == UI_PANEL_NONE,
          "signed minus one maps to the uint8 sentinel");

    bool maximum[UINT8_MAX] = {};
    maximum[0] = true;
    maximum[UINT8_MAX - 1] = true;
    check(ui_panel_next(maximum, UINT8_MAX, 0, INT_MAX) == UINT8_MAX - 1,
          "maximum count handles INT_MAX without overflow");
    check(ui_panel_next(maximum, UINT8_MAX, 0, INT_MIN) == 0,
          "maximum count handles INT_MIN without overflow");

    char maximum_body[PANEL_BODY_LEN];
    std::memset(maximum_body, 87, PANEL_BODY_LEN - 1);
    maximum_body[PANEL_BODY_LEN - 1] = 0;
    UiLine wrapped[PANEL_WRAP_MAX_LINES];
    int font2_lines = ui_wrap_lines(maximum_body, wrapped, PANEL_WRAP_MAX_LINES,
                                    PANEL_FONT2, PANEL_BODY_W);
    UiPanelLayout maximum_layout = ui_panel_layout(font2_lines);
    check(font2_lines > PANEL_FONT2_ROWS && maximum_layout.font == PANEL_FONT1,
          "a maximum worst-width body selects the guaranteed compact layout");
    int compact_lines = ui_wrap_lines(maximum_body, wrapped, PANEL_WRAP_MAX_LINES,
                                      maximum_layout.font, PANEL_BODY_W);
    int accounted = 0;
    for (int i = 0; i < compact_lines; i++) accounted += wrapped[i].len;
    check(compact_lines <= maximum_layout.rows && accounted == PANEL_BODY_LEN - 1,
          "all 256 worst-width bytes are exposed in the available rows");
    char extracted[PANEL_BODY_LINE_LEN];
    ui_line_text(extracted, sizeof(extracted), maximum_body, wrapped[0]);
    check(std::strlen(extracted) == wrapped[0].len,
          "the shipping line copier preserves a complete compact row");

    UiLine short_wrapped[PANEL_WRAP_MAX_LINES];
    int short_lines = ui_wrap_lines("short body", short_wrapped,
                                    PANEL_WRAP_MAX_LINES, PANEL_FONT2, PANEL_BODY_W);
    UiPanelLayout short_layout = ui_panel_layout(short_lines);
    check(short_layout.font == PANEL_FONT2 && short_layout.rows == PANEL_FONT2_ROWS,
          "a short body keeps the preferred font 2 layout");

    uint8_t initial_clear = ui_panel_clear_plan(false, false, false, true, 0, 0);
    uint8_t live_to_empty = ui_panel_clear_plan(true, true, false, false,
                                                PANEL_FONT2, 0);
    uint8_t empty_to_live = ui_panel_clear_plan(true, false, true, false,
                                                0, PANEL_FONT2);
    uint8_t page_to_page = ui_panel_clear_plan(true, true, true, false,
                                               PANEL_FONT2, PANEL_FONT2);
    uint8_t font_change = ui_panel_clear_plan(true, true, true, true,
                                              PANEL_FONT2, PANEL_FONT1);
    uint8_t every_field = UI_PANEL_CLEAR_TITLE_AGE | UI_PANEL_CLEAR_BODY |
                          UI_PANEL_CLEAR_COUNTER | UI_PANEL_CLEAR_CACHES;
    check(initial_clear == every_field && live_to_empty == every_field,
          "initial and live-to-empty frames clear title, age, counter, body, and caches");
    check(empty_to_live == every_field && page_to_page == every_field &&
          font_change == every_field,
          "empty-to-live, page-to-page, and font changes clear the complete old frame");
    check(ui_panel_clear_plan(true, true, true, true, PANEL_FONT2, PANEL_FONT2) == 0,
          "a same-page same-layout update remains field-differential");
    check(PANEL_EMPTY_TITLE_Y + PANEL_EMPTY_TITLE_H <= PANEL_EMPTY_HINT_Y &&
          PANEL_EMPTY_HINT_Y + PANEL_EMPTY_HINT_H <= PANEL_BODY_BOTTOM + 1,
          "the two empty-state rows do not overlap and stay on screen");

    char old_line[PANEL_BODY_LINE_LEN] = {};
    char new_line[PANEL_BODY_LINE_LEN] = {};
    std::memset(old_line, 97, 90);
    std::memset(new_line, 97, 90);
    old_line[90] = 0; new_line[90] = 0;
    new_line[80] = 98;
    check(std::strncmp(old_line, new_line, 68) == 0 &&
          ui_panel_line_changed(old_line, new_line),
          "the shipping cache decision repaints a suffix after a shared 68-byte prefix");
    ui_cache_drop(old_line);
    check(ui_panel_line_changed(old_line, ""),
          "a cleared frame poisons even an empty row cache for repaint");

    Panel pages[3] = {};
    key(pages[0], "alpha"); key(pages[1], "beta"); key(pages[2], "gamma");
    ui_panel_sel = UI_PANEL_NONE; ui_panel_key[0] = 0;
    check(ui_panel_select(pages, 3, 1) == 0 && std::strcmp(ui_panel_key, "alpha") == 0,
          "forward clock entry selects the first dynamic page");
    ui_panel_sel = UI_PANEL_NONE; ui_panel_key[0] = 0;
    check(ui_panel_select(pages, 3, -1) == 2 && std::strcmp(ui_panel_key, "gamma") == 0,
          "backward clock entry selects the last dynamic page");

    ui_panel_sel = 1; std::snprintf(ui_panel_key, sizeof(ui_panel_key), "beta");
    Panel reordered[3] = {};
    key(reordered[0], "gamma"); key(reordered[1], "alpha"); key(reordered[2], "beta");
    check(ui_panel_select(reordered, 3, 0) == 2 && std::strcmp(ui_panel_key, "beta") == 0,
          "a reorder preserves selection by key");
    Panel deleted[2] = {};
    key(deleted[0], "gamma"); key(deleted[1], "alpha");
    check(ui_panel_select(deleted, 2, 0) == 1 && std::strcmp(ui_panel_key, "alpha") == 0,
          "deleting the selected key recovers at the nearest row");
    check(ui_panel_select(nullptr, 0, 0) == -1 && ui_panel_sel == UI_PANEL_NONE &&
          ui_panel_key[0] == 0, "an empty snapshot has a stable explicit state");

    if (failures != 0) return 1;
    std::printf("all panel UI navigation checks passed\n");
    return 0;
}'
} > "$work/test_panel_ui.cpp"

c++ -std=c++17 -Wall -Wextra -Werror "$work/test_panel_ui.cpp" -o "$work/test_panel_ui"
"$work/test_panel_ui"

echo "UI source gates"
enum_span="$(awk '/^enum \{/{n++} n == 1{print} /^};/ && n == 1{exit}' "$ui")"
printf '%s\n' "$enum_span" | grep -Eq 'UI_REC,$'
printf '%s\n' "$enum_span" | grep -Eq 'UI_PANEL$'
grep -Fq '"SETUP AP", "INFO", "RECORDING", "PANEL"' "$ui"
grep -Fq '== UI_PANEL + 1' "$ui"
echo "  ok:   UI_PANEL is append-only with a parallel title assertion"

clock_span="$(awk '
    /^static void ui_poll\(/ { poll = 1 }
    poll && /case UI_CLOCK:/ { grab = 1 }
    grab { print }
    grab && /return;/ { exit }
' "$ui")"
printf '%s\n' "$clock_span" | grep -Fq 'if (click)'
printf '%s\n' "$clock_span" | grep -Fq 'ui_enter_list(UI_MENU, 0)'
printf '%s\n' "$clock_span" | grep -Fq 'else if (steps != 0)'
printf '%s\n' "$clock_span" | grep -Fq 'ui_panel_move(steps)'
printf '%s\n' "$clock_span" | grep -Fq 'ui_enter(UI_PANEL)'
panel_case="$(awk '
    /^static void ui_poll\(/ { poll = 1 }
    poll && /case UI_PANEL:/ { grab = 1 }
    grab { print }
    grab && /^            break;/ { exit }
' "$ui")"
printf '%s\n' "$panel_case" | grep -Fq 'if (click || back)'
printf '%s\n' "$panel_case" | grep -Fq 'ui_enter(UI_CLOCK)'
printf '%s\n' "$panel_case" | grep -Fq 'ui_panel_move(steps)'
echo "  ok:   clock and panel detents/clicks/buttons have explicit transitions"

grep -Fq 'ui_screen != UI_PANEL &&' "$ui"
input_span="$(awk '/^static bool ui_input_driven\(/{grab=1} grab{print} grab && /^}/{exit}' "$ui")"
if printf '%s\n' "$input_span" | grep -q 'UI_PANEL'; then
    echo "  FAIL: UI_PANEL is input-driven and cannot repaint from POST or TTL"
    exit 1
fi
echo "  ok:   panel survives menu idle timeout but remains remotely repaintable"

draw_span="$(awk '/^static void ui_draw_panel\(/{grab=1} grab{print} grab && /^}/{exit}' "$ui")"
for required in '"NO PANELS"' 'panel.title' 'panel.key' 'panel.body' \
    'panel_age(panel, sampled_ms)' 'notify_age_str' 'panel_live_snapshot' \
    'ui_panel_clear_plan' 'ui_panel_clear(clear)'; do
    printf '%s\n' "$draw_span" | grep -Fq "$required"
done
if printf '%s\n' "$draw_span" | grep -Eqi 'tesla|weather|home.?assistant'; then
    echo "  FAIL: panel renderer contains a hardcoded publisher or page"
    exit 1
fi
echo "  ok:   empty state, title, body, and age all come from the live snapshot"

grep -Fq '#define PANEL_BODY_LINE_LEN    (PANEL_BODY_W / MSG_GLYPH_W_MIN + 1)' "$ui"
grep -Fq 'sizeof(ui_panel_body[0]) == PANEL_BODY_LINE_LEN' "$ui"
grep -Fq 'PANEL_FONT1_MAX_LINES <= PANEL_FONT1_ROWS' "$ui"
grep -Fq 'ui_panel_line_changed(ui_panel_body[r], line)' "$ui"
echo "  ok:   renderer uses the executed adaptive layout, transition, and cache core"

if grep -Eqi 'panel_cfg|panel config|toggle panel|SPIFFS|tesla|weather|home.?assistant' \
        "$ui" "$panel"; then
    echo "  FAIL: C2 additions contain C3/config or hardcoded publisher content"
    exit 1
fi
echo "  ok:   C2 has no config, C3 toggle, HA publisher, or fixed page"

post_span="$(awk '/^static void panel_send_post\(/{grab=1} grab{print} grab && /^}/{exit}' "$panel")"
for forbidden in ui_note_input ui_last_input wake backlight; do
    if printf '%s\n' "$post_span" | grep -qi "$forbidden"; then
        echo "  FAIL: POST /panel touches forbidden '$forbidden'"
        exit 1
    fi
done
grep -Fq 'display_force = true;' <<< "$post_span"
echo "  ok:   POST invalidates the display without input, wake, or backlight effects"
