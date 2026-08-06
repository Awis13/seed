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

extract_ellipsis() {
    awk '
        /^static void ui_ellipsis\(/ { grab = 1 }
        grab {
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            depth += opens - closes
            if (opens) started = 1
        }
        grab && started && depth == 0 { exit }
    ' "$ui"
}

extract_structured_renderers() {
    awk '
        /^static void ui_panel_direct_text\(/ { grab = 1 }
        grab && /^static void ui_draw_panel\(/ { exit }
        grab { print }
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
    printf '%s\n' '#include <climits>' '#include <cmath>' '#include <cstdint>' '#include <cstdio>' \
        '#include <cstring>' '#include <unistd.h>' \
        'enum PanelKind : uint8_t { PANEL_KIND_TEXT, PANEL_KIND_KV, PANEL_KIND_BARS, PANEL_KIND_SPARKLINE, PANEL_KIND_STATUS };' \
        'struct DrawOp { char type; int x1; int y1; int x2; int y2; int w; int h; uint8_t font; uint8_t datum; char text[80]; };' \
        'struct FakeTft {' \
        '    DrawOp ops[256] = {}; int count = 0; uint8_t datum = 0;' \
        '    int textWidth(const char *text, uint8_t font) {' \
        '        return static_cast<int>(std::strlen(text)) * (font == 1 ? 6 : 10);' \
        '    }' \
        '    void setTextDatum(uint8_t value) { datum = value; }' \
        '    void setTextColor(uint16_t, uint16_t) {}' \
        '    void drawString(const char *text, int x, int y, uint8_t font) {' \
        '        DrawOp &op = ops[count++]; op.type = 84; op.x1 = x; op.y1 = y; op.font = font; op.datum = datum;' \
        '        std::snprintf(op.text, sizeof(op.text), "%s", text);' \
        '    }' \
        '    void fillRect(int x, int y, int w, int h, uint16_t) {' \
        '        DrawOp &op = ops[count++]; op.type = 82; op.x1 = x; op.y1 = y; op.w = w; op.h = h;' \
        '    }' \
        '    void drawLine(int x1, int y1, int x2, int y2, uint16_t) {' \
        '        DrawOp &op = ops[count++]; op.type = 76; op.x1 = x1; op.y1 = y1; op.x2 = x2; op.y2 = y2;' \
        '    }' \
        '    void reset() { count = 0; }' \
        '} tft;'
    extract_char_width
    extract_ellipsis
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
        '#define PANEL_KV_MAX 5' '#define PANEL_BAR_MAX 4' '#define PANEL_SPARK_MAX 48' \
        '#define PANEL_LABEL_LEN 17' '#define PANEL_VALUE_LEN 33' '#define PANEL_STATUS_LEN 25' \
        '#define PANEL_UNIT_LEN 9' '#define PANEL_DETAIL_LEN 65' \
        '#define COL_BG 0' '#define COL_TIME 1' '#define COL_DIM 2' '#define COL_ACCENT 3' '#define COL_RULE 4' '#define COL_CRIT 5' \
        '#define TL_DATUM 0' '#define TR_DATUM 1' '#define TC_DATUM 2' '#define MC_DATUM 3' \
        'struct PanelKvItem { char label[PANEL_LABEL_LEN]; char value[PANEL_VALUE_LEN]; };' \
        'struct PanelBarItem { char label[PANEL_LABEL_LEN]; float value; float max; char unit[PANEL_UNIT_LEN]; };' \
        'struct PanelKvPayload { uint8_t count; PanelKvItem items[PANEL_KV_MAX]; };' \
        'struct PanelBarsPayload { uint8_t count; PanelBarItem items[PANEL_BAR_MAX]; };' \
        'struct PanelSparkPayload { uint8_t count; char unit[PANEL_UNIT_LEN]; float values[PANEL_SPARK_MAX]; };' \
        'struct PanelStatusPayload { char value[PANEL_STATUS_LEN]; char unit[PANEL_UNIT_LEN]; char detail[PANEL_DETAIL_LEN]; };' \
        'union PanelPayload { char body[PANEL_BODY_LEN]; PanelKvPayload kv; PanelBarsPayload bars; PanelSparkPayload sparkline; PanelStatusPayload status; };' \
        'struct Panel { char key[PANEL_KEY_LEN]; PanelKind kind; PanelPayload payload; };' \
        'static uint8_t ui_panel_sel = UI_PANEL_NONE;' \
        'static char ui_panel_key[PANEL_KEY_LEN] = "";'
    extract_structured_renderers
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

static bool recorded_primitives_in_bounds() {
    for (int i = 0; i < tft.count; i++) {
        const DrawOp &op = tft.ops[i];
        if (op.type == 82 && (op.x1 < 0 || op.y1 < 0 || op.w < 0 || op.h < 0 ||
            op.x1 + op.w > 320 || op.y1 + op.h > 170)) return false;
        if (op.type == 76 && (op.x1 < 0 || op.x1 >= 320 || op.x2 < 0 || op.x2 >= 320 ||
            op.y1 < 0 || op.y1 >= 170 || op.y2 < 0 || op.y2 >= 170)) return false;
        if (op.type == 84) {
            int width = tft.textWidth(op.text, op.font);
            int height = op.font == 4 ? 26 : (op.font == 2 ? 16 : 8);
            int left = op.x1, top = op.y1;
            if (op.datum == TR_DATUM) left -= width - 1;
            else if (op.datum == TC_DATUM) left -= width / 2;
            else if (op.datum == MC_DATUM) {
                left -= width / 2;
                top -= height / 2;
            }
            if (left < 0 || top < 0 || left + width > 320 || top + height > 170)
                return false;
        }
    }
    return true;
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

    uint8_t initial_clear = ui_panel_clear_plan(false, false, false, true, 0, 0,
                                                PANEL_KIND_TEXT, PANEL_KIND_TEXT);
    uint8_t live_to_empty = ui_panel_clear_plan(true, true, false, false,
                                                PANEL_FONT2, 0, PANEL_KIND_TEXT,
                                                PANEL_KIND_TEXT);
    uint8_t empty_to_live = ui_panel_clear_plan(true, false, true, false,
                                                0, PANEL_FONT2, PANEL_KIND_TEXT,
                                                PANEL_KIND_TEXT);
    uint8_t page_to_page = ui_panel_clear_plan(true, true, true, false,
                                               PANEL_FONT2, PANEL_FONT2,
                                               PANEL_KIND_TEXT, PANEL_KIND_TEXT);
    uint8_t font_change = ui_panel_clear_plan(true, true, true, true,
                                              PANEL_FONT2, PANEL_FONT1,
                                              PANEL_KIND_TEXT, PANEL_KIND_TEXT);
    uint8_t every_field = UI_PANEL_CLEAR_TITLE_AGE | UI_PANEL_CLEAR_BODY |
                          UI_PANEL_CLEAR_COUNTER | UI_PANEL_CLEAR_CACHES;
    check(initial_clear == every_field && live_to_empty == every_field,
          "initial and live-to-empty frames clear title, age, counter, body, and caches");
    check(empty_to_live == every_field && page_to_page == every_field &&
          font_change == every_field,
          "empty-to-live, page-to-page, and font changes clear the complete old frame");
    check(ui_panel_clear_plan(true, true, true, true, PANEL_FONT2, PANEL_FONT2,
                              PANEL_KIND_TEXT, PANEL_KIND_TEXT) == 0,
          "a same-page same-layout update remains field-differential");
    check(ui_panel_clear_plan(true, true, true, true, 0, 0, PANEL_KIND_KV,
                              PANEL_KIND_BARS) == every_field,
          "a same-key kind transition clears the complete old frame");
    check(PANEL_EMPTY_TITLE_Y + PANEL_EMPTY_TITLE_H <= PANEL_EMPTY_HINT_Y &&
          PANEL_EMPTY_HINT_Y + PANEL_EMPTY_HINT_H <= PANEL_BODY_BOTTOM + 1,
          "the two empty-state rows do not overlap and stay on screen");

    check(ui_panel_bar_width(0.0f, 10.0f, PANEL_BODY_W) == 0 &&
          ui_panel_bar_width(5.0f, 10.0f, PANEL_BODY_W) == PANEL_BODY_W / 2 &&
          ui_panel_bar_width(10.0f, 10.0f, PANEL_BODY_W) == PANEL_BODY_W,
          "bar geometry covers empty, partial, and full fills");
    check(ui_panel_graph_x(0, 48) == PANEL_GRAPH_X &&
          ui_panel_graph_x(47, 48) == PANEL_GRAPH_X + PANEL_GRAPH_W - 1,
          "a maximum sparkline includes the first and last graph pixels");
    check(ui_panel_graph_y(0.0f, 0.0f, 10.0f) == PANEL_GRAPH_Y + PANEL_GRAPH_H - 1 &&
          ui_panel_graph_y(10.0f, 0.0f, 10.0f) == PANEL_GRAPH_Y &&
          ui_panel_graph_y(4.0f, 4.0f, 4.0f) == PANEL_GRAPH_Y + PANEL_GRAPH_H / 2,
          "sparkline maps rising, falling, and constant-series points safely");
    check(!ui_panel_structured_repaint(false, 0, true, PANEL_KIND_KV, PANEL_KIND_KV) &&
          ui_panel_structured_repaint(true, 0, true, PANEL_KIND_KV, PANEL_KIND_KV) &&
          ui_panel_structured_repaint(false, UI_PANEL_CLEAR_ALL, true,
                                      PANEL_KIND_KV, PANEL_KIND_KV) &&
          ui_panel_structured_repaint(false, 0, true, PANEL_KIND_KV, PANEL_KIND_BARS),
          "structured panels stay idle until content or kind invalidates the frame");
    check(PANEL_GRAPH_X >= PANEL_TEXT_X &&
          PANEL_GRAPH_X + PANEL_GRAPH_W - 1 <= PANEL_TEXT_X + PANEL_BODY_W - 1 &&
          PANEL_GRAPH_Y >= PANEL_BODY_Y &&
          PANEL_GRAPH_Y + PANEL_GRAPH_H - 1 <= PANEL_BODY_BOTTOM,
          "the complete graph envelope stays in the 320 by 170 content band");

    Panel drawn = {};
    drawn.kind = PANEL_KIND_KV;
    drawn.payload.kv.count = PANEL_KV_MAX;
    for (int i = 0; i < PANEL_KV_MAX; i++) {
        std::snprintf(drawn.payload.kv.items[i].label, PANEL_LABEL_LEN,
                      "abcdefghijklmnop");
        std::snprintf(drawn.payload.kv.items[i].value, PANEL_VALUE_LEN,
                      "12345678901234567890123456789012");
    }
    tft.reset(); ui_panel_draw_structured(drawn);
    check(tft.count == 1 + PANEL_KV_MAX * 2 && recorded_primitives_in_bounds(),
          "the maximum kv renderer keeps every recorded primitive in bounds");
    bool kv_fitted = true;
    for (int i = 1; i < tft.count; i++) {
        int budget = (i % 2) ? 126 : 154;
        if (tft.textWidth(tft.ops[i].text, 2) > budget) kv_fitted = false;
    }
    check(kv_fitted, "kv label and value columns apply pixel-safe ellipsis");

    drawn = {}; drawn.kind = PANEL_KIND_BARS; drawn.payload.bars.count = PANEL_BAR_MAX;
    for (int i = 0; i < PANEL_BAR_MAX; i++) {
        std::snprintf(drawn.payload.bars.items[i].label, PANEL_LABEL_LEN, "bar%d", i);
        drawn.payload.bars.items[i].value = i == 0 ? 0.0f : (i == 1 ? 5.0f : 10.0f);
        drawn.payload.bars.items[i].max = 10.0f;
    }
    tft.reset(); ui_panel_draw_structured(drawn);
    check(recorded_primitives_in_bounds(),
          "the maximum bars renderer keeps text, troughs, and fills in bounds");
    int troughs = 0, fills = 0;
    for (int i = 0; i < tft.count; i++) if (tft.ops[i].type == 82) {
        if (tft.ops[i].w == PANEL_BODY_W && tft.ops[i].h == PANEL_BAR_H) troughs++;
        if (tft.ops[i].h == PANEL_BAR_H && tft.ops[i].w < PANEL_BODY_W) fills++;
    }
    check(troughs >= PANEL_BAR_MAX && fills >= 1,
          "bar draw records complete background erases plus bounded partial fill");

    drawn = {}; drawn.kind = PANEL_KIND_SPARKLINE;
    drawn.payload.sparkline.count = PANEL_SPARK_MAX;
    for (int i = 0; i < PANEL_SPARK_MAX; i++) drawn.payload.sparkline.values[i] = (float)i;
    tft.reset(); ui_panel_draw_structured(drawn);
    check(tft.count == 1 + 1 + PANEL_SPARK_MAX - 1 && recorded_primitives_in_bounds() &&
          tft.ops[2].x1 == PANEL_GRAPH_X &&
          tft.ops[tft.count - 1].x2 == PANEL_GRAPH_X + PANEL_GRAPH_W - 1,
          "the maximum rising sparkline draws all segments and both endpoints in bounds");
    for (int i = 0; i < PANEL_SPARK_MAX; i++) drawn.payload.sparkline.values[i] = (float)-i;
    tft.reset(); ui_panel_draw_structured(drawn);
    check(recorded_primitives_in_bounds(), "a falling sparkline stays in bounds");
    for (int i = 0; i < PANEL_SPARK_MAX; i++) drawn.payload.sparkline.values[i] = 7.0f;
    tft.reset(); ui_panel_draw_structured(drawn);
    bool centered = true;
    for (int i = 2; i < tft.count; i++)
        if (tft.ops[i].y1 != PANEL_GRAPH_Y + PANEL_GRAPH_H / 2 ||
            tft.ops[i].y2 != PANEL_GRAPH_Y + PANEL_GRAPH_H / 2) centered = false;
    check(centered && recorded_primitives_in_bounds(),
          "a constant sparkline is centered without division by zero");

    drawn = {}; drawn.kind = PANEL_KIND_STATUS;
    std::snprintf(drawn.payload.status.value, PANEL_STATUS_LEN, "123456789012345678901234");
    std::snprintf(drawn.payload.status.unit, PANEL_UNIT_LEN, "abcdefgh");
    const char *worst_detail =
        "W WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
    std::snprintf(drawn.payload.status.detail, PANEL_DETAIL_LEN, "%s", worst_detail);
    UiLine status_lines[PANEL_STATUS_WRAP_MAX_LINES];
    int status_font2_lines = ui_wrap_lines(worst_detail, status_lines,
                                           PANEL_STATUS_WRAP_MAX_LINES,
                                           PANEL_FONT2, PANEL_BODY_W);
    UiPanelLayout status_layout = ui_panel_status_layout(status_font2_lines);
    int status_lines_used = ui_wrap_lines(worst_detail, status_lines,
                                          PANEL_STATUS_WRAP_MAX_LINES,
                                          status_layout.font, PANEL_BODY_W);
    check(std::strlen(worst_detail) == 64 &&
          status_font2_lines > PANEL_STATUS_FONT2_ROWS &&
          status_layout.font == PANEL_FONT1 &&
          status_lines_used <= status_layout.rows &&
          status_lines[status_lines_used - 1].start +
              status_lines[status_lines_used - 1].len == 64,
          "a worst-case 64-byte status detail is completely accounted in its adaptive rows");
    tft.reset(); ui_panel_draw_structured(drawn);
    check(recorded_primitives_in_bounds() &&
          tft.count == 2 + status_lines_used,
          "status headline and every wrapped glyph extent remain inside 320 by 170");
    drawn.kind = static_cast<PanelKind>(99);
    tft.reset(); ui_panel_draw_structured(drawn);
    check(recorded_primitives_in_bounds() && tft.count == 2 &&
          std::strcmp(tft.ops[1].text, "INVALID PANEL") == 0,
          "a corrupt enum renders the defensive invalid-panel state");

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
for required in '"NO PANELS"' 'panel.title' 'panel.key' 'panel.payload.body' \
    'panel_age(panel, sampled_ms)' 'notify_age_str' 'panel_live_snapshot' \
    'ui_panel_clear_plan' 'ui_panel_clear(clear)' 'ui_panel_draw_structured(panel)'; do
    printf '%s\n' "$draw_span" | grep -Fq "$required"
done
if printf '%s\n' "$draw_span" | grep -Eqi 'tesla|weather|home.?assistant'; then
    echo "  FAIL: panel renderer contains a hardcoded publisher or page"
    exit 1
fi
echo "  ok:   empty state, typed content, title, and age come from the live snapshot"

grep -Fq '#define PANEL_BODY_LINE_LEN    (PANEL_BODY_W / MSG_GLYPH_W_MIN + 1)' "$ui"
grep -Fq 'sizeof(ui_panel_body[0]) == PANEL_BODY_LINE_LEN' "$ui"
grep -Fq 'PANEL_FONT1_MAX_LINES <= PANEL_FONT1_ROWS' "$ui"
grep -Fq 'ui_panel_line_changed(ui_panel_body[r], line)' "$ui"
grep -Fq 'tft.drawLine(prior_x, prior_y, x, y, COL_ACCENT)' "$ui"
grep -Fq 'tft.fillRect(PANEL_TEXT_X, bar_y, PANEL_BODY_W, PANEL_BAR_H, COL_RULE)' "$ui"
grep -Fq '"INVALID PANEL"' "$ui"
echo "  ok:   renderer uses the executed layouts, geometry, transition, and cache core"

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
