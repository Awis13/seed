/*
 * ui.h — on-device user interface: rotary encoder + the 320x170 panel
 *
 * Everything this firmware can do was reachable only over HTTP, which is no
 * use standing in front of a television. This is the front panel: the same
 * jobs, started from the knob.
 *
 * Screen state machine
 * --------------------
 *
 *      CLOCK  --click-->  MENU  --+-->  MSGLIST  -->  MSGCARD
 *        ^   \                    |
 *        |    \                   +-->  TVMENU  --+-->  BLAST
 *        |     \                  |               |
 *        |      \                 |               +-->  BRAND  -->  BLAST
 *        |       \                +-->  AP
 *        |        \               +-->  INFO
 *        |         \
 *        |          +--a notification arrives while idle-->  MSGCARD
 *        |
 *        +--15s idle on any screen but a running blast
 *
 * Back leaves a screen for the one above it — MSGCARD to MSGLIST, BRAND to
 * TVMENU, TVMENU to MENU, MENU to CLOCK — whether it comes from the Back item
 * or the user key, so the knob alone is enough to get anywhere and out again.
 *
 * MSGCARD is the one screen where the click and the user key differ. A click
 * acknowledges the message and returns; the user key returns without
 * acknowledging, which is the only way to look at something and deliberately
 * leave it unread. That keeps the user key's meaning — one level up, change
 * nothing — intact everywhere in this file.
 *
 * CLOCK is the home screen and is drawn exactly as before by display_tick();
 * this file does not touch it beyond handing control back. Every other screen
 * returns to CLOCK after UI_IDLE_MS without input, so a device left in a menu
 * on the shelf goes back to being a clock. A running blast is the one screen
 * that does not time out — it is live output, and its click means "stop". An
 * idle timeout never acknowledges anything: a pager that clears itself because
 * nobody was standing there is not a pager.
 *
 * The pager face
 * --------------
 * The references are the Motorola Advisor and Scriptor LX, the Sidekick and
 * the ICQ client, and they contribute three things.
 *
 * From the pagers: an inverse header bar — amber ground, black text — and
 * service text in capitals on a grid that does not move. `MSG 03/12` occupies
 * the same pixels whatever the numbers are, the source column starts at the
 * same x on every row, and the age column is right-aligned to the same edge.
 * Content changing must never make the layout twitch.
 *
 * From the Sidekick: the selection in a list is a thick solid inverse bar, not
 * an outline. It is legible across a room, which an outline is not.
 *
 * From the notion of a card: the message itself is tinted glass rather than a
 * panel. TFT_eSPI::alphaBlend() composites against the black ground, so the
 * fill is the level colour at about a tenth and the border at about four
 * tenths — both genuinely blended, not approximated with a darker constant.
 * The 3px accent bar down the left edge is the only saturated element on the
 * screen, which is what makes it read as the subject.
 *
 * Three colours at a time, never more. The card spends its three on warm
 * white, slate and one level colour; the list spends them on warm white, slate
 * and amber. Black is the ground and does not count.
 *
 * Why this is not a skill
 * -----------------------
 * A Skill in this firmware is an HTTP surface: it is registered in g_skills,
 * its endpoints are listed by /capabilities and its markdown is served by
 * /skill so an agent can drive it. The UI has no endpoints and nothing remote
 * can call it, so registering it would advertise routes that do not exist.
 * It also has to drive TFT_eSPI, and main.cpp owns that — skills deliberately
 * only format strings (see ir_status_line). So it is an include, not a skill.
 *
 * Relationship to the API
 * -----------------------
 * The UI is a second front-end over the same state, never a second copy of it.
 * TV-B-Gone goes through ir_start_tvbgone()/ir_start_code()/ir_stop_job(), the
 * same functions POST /ir/tvbgone and /ir/tvbgone/stop call, the by-brand list
 * is ir_codes[] itself rather than a copy of the names, and the progress screen
 * renders ir_progress() whoever started the job. Opening the menu item while a
 * blast started over HTTP is running shows that blast instead of trying to
 * start a second one. Setup AP calls ap_start(), the same path as the gesture.
 *
 * Drawing discipline
 * ------------------
 * fillScreen only on a screen transition. Within a screen every field goes
 * through draw_field(), so a redraw that changes nothing costs no SPI: the
 * live screens are repainted at UI_TICK_MS and a list only when the selection
 * actually moves. Input is polled every loop() pass (~10ms), which is what
 * makes the knob feel attached to the screen.
 *
 * Two things here are animated and both are bounded. The card fades in over
 * three frames, which is three fillRects of one card-sized envelope and then
 * nothing for as long as it stays up. The breathing rule on the clock face is
 * one horizontal line every 80ms and lives in main.cpp. Neither runs unless
 * its screen is in front, and no frame of either calls fillScreen.
 *
 * A solid selection bar defeats draw_field's cache — the text of a row does
 * not change when its ground inverts — so the message list repaints the whole
 * row band and forces its cells whenever the selection or the scroll window
 * moves. That is bounded by the screen and happens only on input; between
 * those moments the cache is back in charge and only the age column, which
 * changes on its own, costs anything.
 */

#include <ESP32Encoder.h>

/* ===== Input ===== */

/*
 * Quadrature counts one physical click of this knob produces.
 *
 * attachFullQuad() counts all four edges of a quadrature cycle, and the
 * obvious reading is that a detent spans all four. On this board it spans two.
 * The BOM names the part only as "ENCODER1" and there is no datasheet, so this
 * is a hardware measurement, not a derivation: the sibling pager firmware for
 * this same T-Embed CC1101 checked it on the device — 27 consecutive selection
 * moves, none skipped, a raw delta of exactly 2 for the majority — and records
 * that four would move the selection two rows per click.
 *
 * Which is exactly the symptom this replaces: at 4, half of every click was
 * left in the accumulator and the highlight moved on every second detent.
 *
 * Changing this is a hardware claim and has to be re-checked on a board. The
 * Info screen shows the raw count for that purpose.
 */
#define UI_COUNTS_PER_DETENT 2

/*
 * Which way the count runs against the direction a hand calls "down the list".
 * The counter runs the other way round on this board, so the delta is negated
 * once, here, rather than by flipping the A/B pins in ui_init() — swapping
 * those would also invert the raw count the Info screen reports.
 */
#define UI_ENCODER_DIRECTION (-1)
#define UI_DEBOUNCE_MS       30
#define UI_IDLE_MS           15000
/* Live screens (blast, AP, info) repaint at this rate; the fields are all
   change-detected, so this bounds the work rather than causing it. */
#define UI_TICK_MS           250
/* How long the finished/stopped result stays up before the menu comes back. */
#define UI_RESULT_MS         1500

static ESP32Encoder ui_encoder;

/*
 * Detents and buttons are tracked by two state machines that share nothing.
 * The sibling pager firmware lost its detents whenever the button was held,
 * because the two were entangled: the accumulator's baseline advanced on a
 * pass whose delta the button branch had already discarded, so movement
 * during a hold evaporated. The invariant that prevents it here is that every
 * count read out of the hardware is folded into ui_enc_accum, and the only
 * other writer is ui_encoder_reset() — which re-baselines onto the current
 * hardware count, so it cannot lose a count either. In particular UiButton
 * writes neither, and no button state can skip the fold.
 */
static int64_t ui_enc_raw = 0;   /* last hardware count seen */
static int32_t ui_enc_accum = 0; /* counts not yet worth a detent, sign-carrying */

/* Detents since the last call: positive one way round the knob, negative the
   other. The accumulator carries the remainder in both directions, so half a
   detent forwards followed by half a detent back nets out to nothing instead
   of stepping twice. */
static int ui_encoder_steps() {
    int64_t raw = ui_encoder.getCount();
    ui_enc_accum += (int32_t)(raw - ui_enc_raw);
    ui_enc_raw = raw;

    int steps = 0;
    while (ui_enc_accum >= UI_COUNTS_PER_DETENT) {
        ui_enc_accum -= UI_COUNTS_PER_DETENT;
        steps++;
    }
    while (ui_enc_accum <= -UI_COUNTS_PER_DETENT) {
        ui_enc_accum += UI_COUNTS_PER_DETENT;
        steps--;
    }
    return steps * UI_ENCODER_DIRECTION;
}

/*
 * Re-baseline onto the current hardware count and drop the partial detent.
 *
 * Called on every screen transition. Carrying a remainder is right within a
 * screen — half a click forwards then half back should net out — but across a
 * transition it is a click the user spent somewhere else, and letting it land
 * on the new screen is a selection that moves on its own before the knob is
 * touched. Note this only ever discards less than one detent: a whole one has
 * already been returned to the caller.
 */
static void ui_encoder_reset() {
    ui_enc_raw = ui_encoder.getCount();
    ui_enc_accum = 0;
}

/* Active-low button with a settling filter. A click is a completed
   press-then-release, not a level: holding the key must not repeat, and the
   user key's 3s hold gesture must not also read as a click. */
struct UiButton {
    uint8_t pin;
    bool raw;                  /* last sample, pressed = true */
    bool level;                /* debounced state */
    unsigned long changed_at;  /* when raw last differed */
    unsigned long pressed_at;
    unsigned long held_ms;     /* how long the click just reported was held */
    bool click;                /* one pass only */
};

static UiButton ui_enc_key  = {PIN_ENC_KEY,  false, false, 0, 0, 0, false};
static UiButton ui_user_key = {PIN_USER_KEY, false, false, 0, 0, 0, false};

static void ui_button_poll(UiButton &b) {
    b.click = false;
    bool raw = (digitalRead(b.pin) == LOW);
    if (raw != b.raw) {
        b.raw = raw;
        b.changed_at = millis();
        return;
    }
    if (millis() - b.changed_at < UI_DEBOUNCE_MS) return;
    if (raw == b.level) return;

    b.level = raw;
    if (raw) {
        b.pressed_at = millis();
    } else {
        b.held_ms = millis() - b.pressed_at;
        b.click = true;
    }
}

/* ===== Screens ===== */

enum {
    UI_CLOCK = 0,
    UI_MENU,
    UI_MSGLIST,
    UI_MSGCARD,
    UI_TVMENU,
    UI_BRAND,
    UI_BLAST,
    UI_AP,
    UI_INFO,
    /* Appended at the END, and anything added later must be too: this enum and
       ui_titles[] below are parallel arrays with no static assert to catch a
       value inserted in the middle. */
    UI_REC
};

/* Capitals: this is the service text of the header bar, not prose. One entry
   per screen above, in the same order. */
static const char *ui_titles[] = {
    "", "MENU", "MESSAGES", "MESSAGES", "TV-B-GONE", "BY BRAND", "TV-B-GONE",
    "SETUP AP", "INFO", "RECORDING"
};

/* Top level. The marker on a selected row is also ">", so a submenu is spelled
   out with a trailing one rather than by a different marker. Messages leads
   because it is what this device is for; everything else is a tool it also
   happens to carry. */
enum {
    UI_ITEM_MSG = 0,
    UI_ITEM_TVBGONE,
    UI_ITEM_AP,
    UI_ITEM_INFO,
    UI_ITEM_BACK,
    UI_ITEM_COUNT
};

static const char *ui_items[UI_ITEM_COUNT] = {
    "Messages >",   /* ui_list_label() appends the unread count */
    "TV-B-Gone >",
    "Setup AP",
    "Info",
    "Back"
};

/* TV-B-Gone. The three blasts are the three regions POST /ir/tvbgone takes;
   By brand opens the named codes. */
enum {
    UI_TV_ALL = 0,
    UI_TV_NA,
    UI_TV_EU,
    UI_TV_BRAND,
    UI_TV_BACK,
    UI_TV_COUNT
};

static const char *ui_tv_items[UI_TV_COUNT] = {
    "Blast all",
    "Blast NA",
    "Blast EU",
    "By brand >",
    "Back"
};

static uint8_t ui_screen = UI_CLOCK;
static int ui_sel = 0;
static int ui_sel_drawn = -1;
/* First list entry on screen: the by-brand list is longer than the panel. */
static int ui_first = 0;
static unsigned long ui_last_input = 0;
static unsigned long ui_last_draw = 0;
/* When the blast being watched stopped running, 0 while it still is. */
static unsigned long ui_blast_done_at = 0;
/* The same, for the recording screen: when the take ended, 0 while it runs. */
static unsigned long ui_rec_done_at = 0;
/* False when the menu could not start a blast and none was already running. */
static bool ui_blast_ok = true;
/* The list the blast was started from, so that finishing goes back to it. */
static uint8_t ui_blast_back = UI_TVMENU;
static int ui_blast_back_sel = 0;

/* Which notification the card is showing. An id, not an index: the list can
   shift underneath it when something expires or a new message arrives. */
static uint32_t ui_msg_id = 0;
/* Fade frames already drawn for the card in front, MSG_FADE_STEPS once done. */
static uint8_t ui_card_fade = 0;
/* Scroll window the message list was last drawn with, so that a window that
   moved is recognised as needing the same full repaint a moved selection does. */
static int ui_first_drawn = -1;

#define UI_MENU_Y0   30
#define UI_MENU_DY   32
/* Rows of font 4 at that pitch that fit above the bottom of a 170px panel. */
#define UI_MENU_ROWS 4
#define UI_ROW_COUNT 8

/*
 * Message list geometry. Font 2 rows on a rigid three-column grid, five of
 * them under an 18px header bar: bars at y-4 for 24px, pitch 28, so the last
 * one ends at 156 on a 170px panel.
 *
 * Every column width below is a budget the renderer ellipsises into, and each
 * was checked against TFT_eSPI's own width tables rather than by eye. In font
 * 2 the widest glyph is 10px ('M' and 'W'), so the guaranteed worst case is 7
 * characters of source, 16 of title and 4 of age — and the measured strings
 * that actually appear are well inside that: "! HOME-RIG" is 69px of the 76
 * the source column has, and the longest age "59m" is 24px of 46.
 */
#define MSG_ROWS      5
#define MSG_ROW_Y0    24
#define MSG_ROW_DY    28
#define MSG_BAR_X      6
#define MSG_BAR_W    308
#define MSG_BAR_UP     4   /* bar top, above the row's text origin */
#define MSG_BAR_H     24
#define MSG_SRC_X     10
#define MSG_SRC_W     76
#define MSG_TITLE_X   92
#define MSG_TITLE_W  160
#define MSG_AGE_R    310   /* right-aligned to this edge */
#define MSG_AGE_W     46

/*
 * Card geometry. 300x100 at (8,28), leaving the header bar above and a hint
 * line below at 140. Text is inset 14px from the left so it clears the 3px
 * accent bar, and 10px from the right, which gives the title 276px — 11
 * characters at the widest glyph font 4 has, and around 18 of ordinary mixed
 * case. The body gets two lines of the same 276px, 27 characters worst case in
 * font 2 and around 34 in practice.
 */
#define MSG_CARD_X     8
#define MSG_CARD_Y    28
#define MSG_CARD_W   300
#define MSG_CARD_H   100
#define MSG_PAD_L     14
#define MSG_PAD_R     10
#define MSG_ACCENT_W   3
#define MSG_PEEK       3   /* offset of each card peeking out behind */
#define MSG_HINT_Y   140

/* Arrival: three frames of rising blend plus a few pixels of upward travel.
   40ms a frame, so the whole thing is over in 120ms — present enough to read
   as an arrival, short enough that it never delays the first look. */
#define MSG_FADE_STEPS 3
#define MSG_FADE_MS   40
#define MSG_TINT_A    26   /* card fill: level colour at ~10% over black */
#define MSG_BORDER_A 102   /* card border: the same colour at ~40% */

/* One cache line per drawn row, shared by every screen in this file: only one
   of them is on screen at a time, and entering any screen wipes the panel and
   raises display_force, which makes draw_field ignore whatever the previous
   screen left in here. Wide enough for an ellipsised font 4 title, which is
   the longest string any of them draws. */
static char ui_row[UI_ROW_COUNT][48];

/* The message list needs its own cache: three cells per row, and it is the one
   screen whose rows are not a single string. Each cell is wide enough for the
   longest string its column can ever be asked to draw — an ellipsised title —
   so that the cache always holds the whole of what is on the panel and two
   different rows can never compare equal. */
#define MSG_CELL_LEN (NOTIFY_TITLE_LEN + 4)
static char ui_msg_cell[MSG_ROWS][3][MSG_CELL_LEN];

/* The card's two body lines get their own, wider cache. A body is 96
   characters and the panel is 276px, which at the narrowest glyphs font 2 has
   (3px, the punctuation) is more characters than fit in a row cache line. The
   pixel budget is the bound that matters; the buffer must not be a second,
   quieter one that cuts a line the display could have shown. */
static char ui_card_body[2][NOTIFY_BODY_LEN + 4];

static void ui_draw_row(int i, const char *text, int32_t y, uint8_t font,
                        uint16_t color) {
    draw_field(ui_row[i], sizeof(ui_row[i]), text, 12, y, font, color,
               TL_DATUM, 296);
}

/* ===== Fitting text to a column =====
 *
 * Columns are pixel budgets, and what goes in them is whatever an agent put in
 * the JSON. Everything below measures with TFT_eSPI's own tables — the same
 * ones textWidth() reads — so a string is cut where it actually stops fitting
 * rather than at a character count guessed from an average glyph.
 */

static int ui_char_w(char c, uint8_t font) {
    char s[2] = {c, '\0'};
    return tft.textWidth(s, font);
}

/* Copy `src` into `dst`, cut to `max_px` with a trailing ellipsis if it does
   not fit. A column too narrow for the ellipsis itself is hard-truncated
   instead — three dots and nothing else says less than two real characters. */
static void ui_ellipsis(char *dst, size_t n, const char *src, uint8_t font,
                        int max_px) {
    if (n == 0) return;
    dst[0] = '\0';
    if (!src) return;
    if (tft.textWidth(src, font) <= max_px) {
        /* Copied a character at a time rather than with snprintf("%s"): the
           destination is a cache line sized for the widest thing this column
           can display, and spelling the bound out here is what keeps it from
           becoming a second, silent limit the compiler has to warn about. */
        size_t i = 0;
        while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
        dst[i] = '\0';
        return;
    }

    int ell = tft.textWidth("...", font);
    bool with_ellipsis = (ell < max_px);
    int budget = with_ellipsis ? max_px - ell : max_px;

    size_t out = 0;
    int acc = 0;
    for (const char *p = src; *p && out + 4 < n; p++) {
        int cw = ui_char_w(*p, font);
        if (acc + cw > budget) break;
        acc += cw;
        dst[out++] = *p;
    }
    /* Hang the ellipsis off a word rather than off a gap. */
    while (out > 0 && dst[out - 1] == ' ') out--;
    dst[out] = '\0';
    if (with_ellipsis) snprintf(dst + out, n - out, "...");
}

/* Break `src` over two lines of `max_px`, on a space where there is one. The
   second line carries the ellipsis, so a body too long for the card ends
   visibly rather than just stopping. */
static void ui_wrap2(const char *src, char *l1, size_t n1, char *l2, size_t n2,
                     uint8_t font, int max_px) {
    l1[0] = '\0';
    l2[0] = '\0';
    if (!src || !src[0]) return;
    if (tft.textWidth(src, font) <= max_px) {
        size_t i = 0;
        while (src[i] && i + 1 < n1) { l1[i] = src[i]; i++; }
        l1[i] = '\0';
        return;
    }

    int acc = 0, last_space = -1, i = 0;
    for (; src[i]; i++) {
        int cw = ui_char_w(src[i], font);
        if (acc + cw > max_px) break;
        if (src[i] == ' ') last_space = i;
        acc += cw;
    }
    /* A single word wider than the line has no space to break on: cut it. */
    int brk = (last_space > 0) ? last_space : i;

    size_t take = (size_t)brk;
    if (take >= n1) take = n1 - 1;
    memcpy(l1, src, take);
    l1[take] = '\0';

    const char *rest = src + brk;
    while (*rest == ' ') rest++;
    ui_ellipsis(l2, n2, rest, font, max_px);
}

/* Service text is capitals. Sources arrive as whatever an agent typed. */
static void ui_caps(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}

static uint16_t ui_level_color(uint8_t level) {
    switch (level) {
        case NOTIFY_CRIT: return COL_CRIT;
        case NOTIFY_WARN: return COL_ACCENT;
        default:          return COL_INFO;
    }
}

/* `MSG 03/12` in the header bar, black on amber, zero-padded so the field is
   the same width whatever it says. Shared by the list and the card, which are
   two views of one position in one queue. */
static void ui_draw_msg_counter(int pos, int total) {
    char buf[16];
    if (pos < 0) pos = 0;
    if (pos > 99) pos = 99;
    if (total < 0) total = 0;
    if (total > 99) total = 99;
    snprintf(buf, sizeof(buf), "MSG %02d/%02d", pos, total);
    draw_field(ui_row[UI_ROW_COUNT - 1], sizeof(ui_row[0]), buf,
               tft.width() - 8, HDR_Y, 2, COL_BG, TR_DATUM, 76, COL_ACCENT);
}

/* Shared scroll window: move it only when the selection would otherwise leave
   it, which keeps a step inside the window down to a two-row repaint. */
static void ui_window(int count, int rows) {
    if (ui_sel < ui_first) ui_first = ui_sel;
    if (ui_sel >= ui_first + rows) ui_first = ui_sel - rows + 1;
    if (ui_first > count - rows) ui_first = count - rows;
    if (ui_first < 0) ui_first = 0;
}

/* The three list screens differ only in what they list, so they share one
   renderer and one set of input rules. Both accessors read the live tables —
   the by-brand list is ir_codes[] itself, not a copy of the names. */
static int ui_list_count() {
    switch (ui_screen) {
        case UI_MENU:    return UI_ITEM_COUNT;
        case UI_TVMENU:  return UI_TV_COUNT;
        case UI_BRAND:   return ir_code_count + 1;  /* the codes, then Back */
        case UI_MSGLIST: return notify_count() + 1; /* the messages, then Back */
    }
    return 0;
}

static const char *ui_list_label(int i) {
    /* The Messages row carries its unread count, so its label is built rather
       than looked up. ui_draw_list() consumes the return value into the row
       string before asking for the next one, so one shared buffer is enough. */
    static char built[24];

    switch (ui_screen) {
        case UI_MENU:
            if (i == UI_ITEM_MSG) {
                int unread = notify_unread_count();
                if (unread > 0) {
                    snprintf(built, sizeof(built), "Messages (%d) >", unread);
                    return built;
                }
            }
            return ui_items[i];
        case UI_TVMENU: return ui_tv_items[i];
        case UI_BRAND:  return (i < ir_code_count) ? ir_codes[i].brand : "Back";
    }
    return "";
}

/* List: the marker is part of the string, so a moved selection changes the
   text of exactly two rows and only those two are repainted. A list longer
   than the panel scrolls a window over it, and the window moves only when the
   selection would otherwise leave it — which keeps the same two-row repaint
   for every step that stays inside it. */
static void ui_draw_list() {
    int count = ui_list_count();

    ui_window(count, UI_MENU_ROWS);

    if (ui_sel == ui_sel_drawn && !display_force) return;

    for (int r = 0; r < UI_MENU_ROWS; r++) {
        int i = ui_first + r;
        char line[40];
        if (i < count) {
            snprintf(line, sizeof(line), "%s %s",
                     i == ui_sel ? ">" : " ", ui_list_label(i));
        } else {
            line[0] = '\0';  /* a short list leaves the rest of the panel blank */
        }
        ui_draw_row(r, line, UI_MENU_Y0 + r * UI_MENU_DY, 4,
                    i == ui_sel ? COL_ACCENT : COL_DIM);
    }

    /* Only a scrolling list says where in itself you are, and it goes in the
       header bar, black on amber like everything else up there. */
    if (count > UI_MENU_ROWS) {
        char pos[16];
        snprintf(pos, sizeof(pos), "%d/%d", ui_sel + 1, count);
        draw_field(ui_row[UI_ROW_COUNT - 1], sizeof(ui_row[0]), pos,
                   tft.width() - 8, HDR_Y, 2, COL_BG, TR_DATUM, 60, COL_ACCENT);
    }

    ui_sel_drawn = ui_sel;
    display_force = false;
}

/*
 * The message list. Three columns on a fixed grid, a solid inverse bar for the
 * selection, and a Back row after the messages.
 *
 * Unlike the other lists this one has to repaint without input, because the
 * age column moves on its own. The split is: the bars and the cells are forced
 * whenever the selection or the window moved (a solid bar inverts the ground
 * under text draw_field would otherwise consider unchanged), and on every
 * other pass the cache decides — which in practice means the ages, once a
 * minute, and nothing else.
 */
static void ui_draw_msglist() {
    int items = notify_count();
    int count = items + 1;  /* messages, then Back */
    ui_window(count, MSG_ROWS);

    bool relayout = display_force || ui_sel != ui_sel_drawn ||
                    ui_first != ui_first_drawn;
    if (relayout) {
        for (int r = 0; r < MSG_ROWS; r++) {
            int32_t y = MSG_ROW_Y0 + r * MSG_ROW_DY;
            tft.fillRect(MSG_BAR_X, y - MSG_BAR_UP, MSG_BAR_W, MSG_BAR_H,
                         (ui_first + r == ui_sel) ? COL_ACCENT : COL_BG);
        }
        display_force = true;  /* every cell is now on a ground it did not have */
    }

    for (int r = 0; r < MSG_ROWS; r++) {
        int i = ui_first + r;
        int32_t y = MSG_ROW_Y0 + r * MSG_ROW_DY;
        bool sel = (i == ui_sel);
        uint16_t bg = sel ? COL_ACCENT : COL_BG;
        /* Sized from the cache they end up in, so the pixel budget stays the
           only thing that decides where a string is cut. */
        char src[MSG_CELL_LEN], title[MSG_CELL_LEN], age[MSG_CELL_LEN];
        bool unread = false;

        src[0] = title[0] = age[0] = '\0';
        if (i == items) {
            snprintf(src, sizeof(src), "BACK");
        } else if (i < items) {
            NotifyView v;
            if (notify_view(i, v)) {
                unread = v.unread;
                /* An unread critical is the one thing that has to be findable
                   without reading the rows, and a leading mark does it without
                   spending a fourth colour on the screen. */
                char caps[NOTIFY_SOURCE_LEN + 4];
                if (v.level == NOTIFY_CRIT && v.unread) {
                    caps[0] = '!';
                    caps[1] = ' ';
                    ui_caps(caps + 2, sizeof(caps) - 2, v.source);
                } else {
                    ui_caps(caps, sizeof(caps), v.source);
                }
                ui_ellipsis(src, sizeof(src), caps, 2, MSG_SRC_W);
                ui_ellipsis(title, sizeof(title), v.title, 2, MSG_TITLE_W);
                notify_age_str(v.age_s, age, sizeof(age));
            }
        }

        /* Black on the bar; off it, unread messages are primary and read ones
           have already had their turn. */
        uint16_t c_pri = sel ? COL_BG : (unread ? COL_TIME : COL_DIM);
        uint16_t c_sec = sel ? COL_BG : COL_DIM;
        draw_field(ui_msg_cell[r][0], sizeof(ui_msg_cell[r][0]), src,
                   MSG_SRC_X, y, 2, c_sec, TL_DATUM, MSG_SRC_W, bg);
        draw_field(ui_msg_cell[r][1], sizeof(ui_msg_cell[r][1]), title,
                   MSG_TITLE_X, y, 2, c_pri, TL_DATUM, MSG_TITLE_W, bg);
        draw_field(ui_msg_cell[r][2], sizeof(ui_msg_cell[r][2]), age,
                   MSG_AGE_R, y, 2, c_sec, TR_DATUM, MSG_AGE_W, bg);
    }

    /* An empty queue says so instead of showing a screen with one Back row on
       it. Drawn after the loop, which has just blanked the row it sits on. */
    draw_field(ui_row[0], sizeof(ui_row[0]), items == 0 ? "NO MESSAGES" : "",
               tft.width() / 2, MSG_ROW_Y0 + MSG_ROW_DY, 2, COL_DIM,
               TC_DATUM, 200);

    ui_draw_msg_counter(ui_sel < items ? ui_sel + 1 : items, items);

    ui_sel_drawn = ui_sel;
    ui_first_drawn = ui_first;
    display_force = false;
}

/*
 * One message as tinted glass.
 *
 * The fill is the level colour blended onto black at about a tenth and the
 * border at about four tenths — real compositing through alphaBlend(), so the
 * card sits over the ground rather than replacing it. The 3px bar down the
 * left edge is the level colour at full strength and is the only saturated
 * thing on the screen.
 *
 * During the fade the whole block is repainted, because the card is also
 * moving; once settled nothing repaints but the age, and that goes through
 * draw_field against the tint rather than against black.
 */
static void ui_draw_card() {
    NotifyView v;
    int idx = 0, total = 0;
    /* Entry, position and depth of the stack in one acquisition: an arrival
       between two of them would draw one card's text under another card's
       counter. Gone — expired or evicted while it was on screen — is the false
       return; ui_poll() leaves for the list on this same pass, so there is
       nothing to draw. */
    if (!notify_view_by_id(ui_msg_id, v, &idx, &total)) return;

    uint16_t level = ui_level_color(v.level);

    bool fading = (ui_card_fade < MSG_FADE_STEPS);
    uint8_t step = fading ? (uint8_t)(ui_card_fade + 1) : (uint8_t)MSG_FADE_STEPS;
    int32_t x = MSG_CARD_X;
    int32_t y = MSG_CARD_Y + (MSG_FADE_STEPS - step) * MSG_PEEK;

    /* Everything the card draws ramps together, so the whole thing arrives as
       one object instead of a border appearing before its contents. */
    uint16_t tint   = tft.alphaBlend((uint8_t)(MSG_TINT_A * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t edge   = tft.alphaBlend((uint8_t)(MSG_BORDER_A * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t accent = tft.alphaBlend((uint8_t)(255 * step / MSG_FADE_STEPS), level, COL_BG);
    uint16_t c_pri  = tft.alphaBlend((uint8_t)(255 * step / MSG_FADE_STEPS), COL_TIME, tint);
    uint16_t c_sec  = tft.alphaBlend((uint8_t)(255 * step / MSG_FADE_STEPS), COL_DIM, tint);

    if (fading || display_force) {
        /* Erase the travel envelope — the card's own footprint plus the two
           peeks and the three pixels it still has to rise. 306x112 once per
           fade frame and then never again while this card is up. */
        tft.fillRect(MSG_CARD_X, MSG_CARD_Y, MSG_CARD_W + 2 * MSG_PEEK,
                     MSG_CARD_H + 4 * MSG_PEEK, COL_BG);

        /* Two more cards behind, when there are two more to be behind: the
           stack the knob can rotate through. Outlines only, dimmer with depth. */
        if (total > 1) {
            for (int p = 2; p >= 1; p--) {
                uint8_t a = (uint8_t)(MSG_BORDER_A / (p + 1) * step / MSG_FADE_STEPS);
                tft.drawRect(x + p * MSG_PEEK, y + p * MSG_PEEK,
                             MSG_CARD_W, MSG_CARD_H,
                             tft.alphaBlend(a, level, COL_BG));
            }
        }

        tft.fillRect(x, y, MSG_CARD_W, MSG_CARD_H, tint);
        tft.drawRect(x, y, MSG_CARD_W, MSG_CARD_H, edge);
        tft.fillRect(x, y, MSG_ACCENT_W, MSG_CARD_H, accent);
        display_force = true;  /* the fill took the text with it */
    }

    int32_t tx = x + MSG_PAD_L;
    int32_t rx = x + MSG_CARD_W - MSG_PAD_R;
    int text_w = MSG_CARD_W - MSG_PAD_L - MSG_PAD_R;

    char caps[NOTIFY_SOURCE_LEN], src[NOTIFY_SOURCE_LEN], age[16];
    ui_caps(caps, sizeof(caps), v.source[0] ? v.source : "device");
    ui_ellipsis(src, sizeof(src), caps, 2, text_w - MSG_AGE_W - 8);
    notify_age_str(v.age_s, age, sizeof(age));
    draw_field(ui_row[0], sizeof(ui_row[0]), src, tx, y + 7, 2,
               c_sec, TL_DATUM, (uint16_t)(text_w - MSG_AGE_W - 8), tint);
    draw_field(ui_row[1], sizeof(ui_row[1]), age, rx, y + 7, 2,
               c_sec, TR_DATUM, MSG_AGE_W, tint);

    char title[48];
    ui_ellipsis(title, sizeof(title), v.title, 4, text_w);
    draw_field(ui_row[2], sizeof(ui_row[2]), title, tx, y + 28, 4,
               c_pri, TL_DATUM, (uint16_t)text_w, tint);

    char b1[NOTIFY_BODY_LEN + 4], b2[NOTIFY_BODY_LEN + 4];
    ui_wrap2(v.body, b1, sizeof(b1), b2, sizeof(b2), 2, text_w);
    draw_field(ui_card_body[0], sizeof(ui_card_body[0]), b1, tx, y + 60, 2,
               c_sec, TL_DATUM, (uint16_t)text_w, tint);
    draw_field(ui_card_body[1], sizeof(ui_card_body[1]), b2, tx, y + 78, 2,
               c_sec, TL_DATUM, (uint16_t)text_w, tint);

    /* Below the card, on the ground: what the knob does next. */
    draw_field(ui_row[5], sizeof(ui_row[5]),
               v.unread ? "CLICK TO ACK" : "CLICK TO GO BACK",
               tft.width() / 2, MSG_HINT_Y, 2, COL_DIM, TC_DATUM, 300);

    ui_draw_msg_counter(idx + 1, total);

    if (fading) ui_card_fade++;
    display_force = false;
}

static void ui_draw_blast() {
    IrProgress p = ir_progress();
    char buf[40];

    /* The job is staged the moment the menu item is clicked, but ir_poll()
       only adopts it — and resets the counters — on its next pass. Until then
       ir_state still describes the previous job, so say "starting" rather than
       paint a stale 27/27 over the first frame of this one. */
    bool starting = ir_busy() && !p.running;

    if (!ui_blast_ok) {
        ui_draw_row(0, "IR unavailable", 40, 4, COL_TIME);
        ui_draw_row(1, "", 78, 4, COL_DIM);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    } else if (starting) {
        ui_draw_row(0, "starting", 40, 4, COL_TIME);
        ui_draw_row(1, "", 78, 4, COL_ACCENT);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    } else if (p.running) {
        ui_draw_row(0, p.label[0] ? p.label : "starting", 40, 4, COL_TIME);
        /* p.sent, not p.step: the count on screen is frames the peripheral
           has actually finished clocking out. */
        snprintf(buf, sizeof(buf), "%d / %d", p.sent, p.total);
        ui_draw_row(1, buf, 78, 4, COL_ACCENT);
        snprintf(buf, sizeof(buf), "%lus elapsed", p.elapsed_ms / 1000);
        ui_draw_row(2, buf, 112, 2, COL_DIM);
    } else {
        snprintf(buf, sizeof(buf), "%s", p.result[0] ? p.result : "idle");
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        snprintf(buf, sizeof(buf), "%d / %d sent", p.sent, p.total);
        ui_draw_row(1, buf, 78, 4, COL_DIM);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    }
    ui_draw_row(3, (ui_blast_ok && ir_busy()) ? "click to stop" : "", 148, 2, COL_DIM);
    display_force = false;
}

static void ui_draw_ap() {
    char buf[40];

    if (!ap_active) {
        ui_draw_row(0, "AP failed to start", 40, 4, COL_TIME);
        ui_draw_row(1, "", 78, 4, COL_ACCENT);
        ui_draw_row(2, "", 118, 2, COL_DIM);
    } else {
        snprintf(buf, sizeof(buf), "SSID %s", ap_ssid.c_str());
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        snprintf(buf, sizeof(buf), "PW   %s", ap_password.c_str());
        ui_draw_row(1, buf, 78, 4, COL_ACCENT);
        unsigned long mins = ap_minutes_left();
        if (mins > 0) snprintf(buf, sizeof(buf), "closes in %lu min", mins);
        else          snprintf(buf, sizeof(buf), "stays up until provisioned");
        ui_draw_row(2, buf, 118, 2, COL_DIM);
    }
    ui_draw_row(3, "click to go back", 148, 2, COL_DIM);
    display_force = false;
}

static void ui_draw_info() {
    char buf[40];
    int row = 0;
    const int32_t y0 = 26, dy = 18;

    /* The raw quadrature count rides along with the version because it is the
       only way to measure UI_COUNTS_PER_DETENT on a real board: turn the knob
       one click and read the difference. */
    snprintf(buf, sizeof(buf), "VER    %s   ENC %ld", SEED_VERSION,
             (long)ui_enc_raw);
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "IP     %s", WiFi.localIP().toString().c_str());
    } else if (ap_active) {
        snprintf(buf, sizeof(buf), "IP     %s (AP)", WiFi.softAPIP().toString().c_str());
    } else {
        snprintf(buf, sizeof(buf), "IP     offline");
    }
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    snprintf(buf, sizeof(buf), "MDNS   %s.local", mdns_name.c_str());
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    if (hw.has_battery && hw.battery_soc >= 0) {
        snprintf(buf, sizeof(buf), "BAT    %.2fV  %d%%", hw.battery_v, hw.battery_soc);
    } else if (hw.has_battery) {
        snprintf(buf, sizeof(buf), "BAT    %.2fV", hw.battery_v);
    } else {
        snprintf(buf, sizeof(buf), "BAT    no fuel gauge");
    }
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "RSSI   %d dBm", (int)WiFi.RSSI());
    } else {
        snprintf(buf, sizeof(buf), "RSSI   --");
    }
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    snprintf(buf, sizeof(buf), "HEAP   %lu KB",
             (unsigned long)(ESP.getFreeHeap() / 1024));
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    if (hw.has_cc1101) {
        snprintf(buf, sizeof(buf), "CC1101 yes (ver 0x%02X)", hw.cc1101_version);
    } else {
        snprintf(buf, sizeof(buf), "CC1101 not detected");
    }
    ui_draw_row(row, buf, y0 + dy * row, 2, COL_DIM); row++;

    ui_draw_row(row, "click to go back", y0 + dy * row, 2, COL_DIM);
    display_force = false;
}

/* ===== Recording =====
 *
 * The one screen that comes up without anybody asking for it: mic.cpp decides
 * a recording has started, ui_poll() notices and puts this in front. It is
 * live output, like a running blast, so the idle timeout does not apply to it
 * — a screen that vanished fifteen seconds into a hold would be exactly wrong.
 *
 * Two things are on it. The elapsed time, which comes from bytes captured
 * rather than from the clock, so it stops moving if the samples stop arriving
 * instead of counting up over audio that does not exist. And a level meter,
 * which is the only feedback that says the microphone is hearing anything at
 * all — without it a dead microphone and a quiet room look identical while you
 * are standing there talking into it.
 */
#define REC_BAR_X    12
#define REC_BAR_Y    86
#define REC_BAR_W   296
#define REC_BAR_H    24
/* The fill is quantised to this many pixels before it is compared against what
   is already on the panel. Room noise moves a peak meter constantly, and
   without a step every tick would repaint the bar for a pixel nobody can see. */
#define REC_BAR_STEP  8

/* How long the finished take stays on screen before the clock comes back.
   Longer than a blast's UI_RESULT_MS, because this result is a number to read
   — the peak — rather than a word to glance at. */
#define UI_REC_RESULT_MS 2500

static int ui_rec_bar_drawn = -1;
/* The colour that width was drawn in. Compared as well as the width, because
   the bar changes colour when the take ends — and if the last live level
   happened to quantise to the same width as the take's peak, comparing width
   alone would leave the finished take showing the recording's red. */
static uint16_t ui_rec_bar_color = 0;

static void ui_draw_rec_bar(int pct, uint16_t color) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = (REC_BAR_W * pct) / 100;
    w = (w / REC_BAR_STEP) * REC_BAR_STEP;
    if (color != ui_rec_bar_color) ui_rec_bar_drawn = -1;
    ui_rec_bar_color = color;
    if (w == ui_rec_bar_drawn) return;

    /* Only the pixels that changed: the fill grows or the trough takes back
       what it lost. Repainting the whole bar every step would be 296x24 of SPI
       traffic on a pass that is already sharing the loop with the capture. */
    if (w > ui_rec_bar_drawn) {
        int from = (ui_rec_bar_drawn < 0) ? 0 : ui_rec_bar_drawn;
        tft.fillRect(REC_BAR_X + from, REC_BAR_Y, w - from, REC_BAR_H, color);
        if (ui_rec_bar_drawn < 0)
            tft.fillRect(REC_BAR_X + w, REC_BAR_Y, REC_BAR_W - w, REC_BAR_H, COL_RULE);
    } else {
        tft.fillRect(REC_BAR_X + w, REC_BAR_Y, ui_rec_bar_drawn - w, REC_BAR_H, COL_RULE);
    }
    ui_rec_bar_drawn = w;
}

/* Seconds to one decimal, which is the resolution a hand on a button can
   actually aim at. The layout does not move as the number grows: the string is
   always the same shape. */
static void ui_rec_secs(char *out, size_t n, uint32_t ms) {
    snprintf(out, n, "%lu.%lus", (unsigned long)(ms / 1000),
             (unsigned long)((ms % 1000) / 100));
}

static void ui_draw_rec() {
    char buf[40];
    char secs[16];
    bool rec = mic_is_recording();

    /* The wipe on the way in took the bar with it. */
    if (display_force) ui_rec_bar_drawn = -1;

    if (rec) {
        ui_rec_secs(secs, sizeof(secs), mic_elapsed_ms());
        snprintf(buf, sizeof(buf), "REC  %s", secs);
        ui_draw_row(0, buf, 40, 4, COL_CRIT);
        /* Reset-on-read: this is the level meter's own peak and nothing else
           may sample it, or the meter would show whatever was left over from
           the last reader. */
        ui_draw_rec_bar(mic_level_pct(), COL_ACCENT);
        ui_draw_row(2, "", 120, 2, COL_DIM);
        ui_draw_row(3, "release the knob to stop", 148, 2, COL_DIM);
    } else {
        uint32_t ms = mic_last_take_ms();
        int32_t peak = mic_last_take_peak();
        ui_rec_secs(secs, sizeof(secs), ms);
        snprintf(buf, sizeof(buf), "%s recorded", secs);
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        /* The take's own peak, held still. A bar that drops to nothing the
           moment the button comes up would say "silent" about a recording that
           was not. */
        ui_draw_rec_bar((int)((peak * 100) / 32767), COL_DIM);
        if (ms == 0) {
            snprintf(buf, sizeof(buf), "nothing captured");
        } else if (peak == 0) {
            /* ASCII only, here and on every other string this file draws: the
               TFT_eSPI fonts compiled in cover 32-126, and a multi-byte dash
               would render as two pieces of garbage rather than as a dash. */
            snprintf(buf, sizeof(buf), "silent, peak 0");
        } else {
            snprintf(buf, sizeof(buf), "peak %ld / 32767", (long)peak);
        }
        ui_draw_row(2, buf, 120, 2, COL_DIM);
        ui_draw_row(3, "GET /mic/last to hear it", 148, 2, COL_DIM);
    }
    display_force = false;
}

/* Screens whose content can only change when the knob moves, and which
   therefore have nothing to gain from the periodic repaint. The message list
   is deliberately not one of them: its age column advances on its own. */
static bool ui_input_driven(uint8_t screen) {
    return screen == UI_MENU || screen == UI_TVMENU || screen == UI_BRAND;
}

static void ui_draw() {
    switch (ui_screen) {
        case UI_MENU:
        case UI_TVMENU:
        case UI_BRAND:   ui_draw_list();    break;
        case UI_MSGLIST: ui_draw_msglist(); break;
        case UI_MSGCARD: ui_draw_card();    break;
        case UI_BLAST:   ui_draw_blast();   break;
        case UI_AP:      ui_draw_ap();      break;
        case UI_INFO:    ui_draw_info();    break;
        case UI_REC:     ui_draw_rec();     break;
        default: break;
    }
}

/* The only place a screen changes, and the only place fillScreen is called
   outside display_status(). */
static void ui_enter(uint8_t screen) {
    ui_screen = screen;
    ui_last_input = millis();
    ui_last_draw = millis();
    ui_encoder_reset();

    if (screen == UI_CLOCK) {
        display_status();  /* repaints the clock chrome and every clock field */
        /* The link may have come or gone while the menu was up. This repaint
           already reflects it, so record it rather than let loop() spend a
           second fillScreen on the same news. */
        clock_last_status = WiFi.status();
        return;
    }

    tft.fillScreen(COL_BG);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    /* Inverse header bar: amber ground, black text, the way the Advisor's LCD
       did it. It replaces the hairline rule — the bar is its own separator —
       and it is what every screen under the knob has in common. The clock face
       keeps the plain header and the rule, because that face is the one thing
       here nobody asked to be louder. */
    tft.fillRect(0, 0, tft.width(), HDR_BAR_H, COL_ACCENT);
    tft.setTextColor(COL_BG, COL_ACCENT);
    tft.drawString(ui_titles[screen], 8, HDR_Y, 2);

    display_force = true;  /* the wipe took every cached field with it */
    ui_sel_drawn = -1;
    ui_first_drawn = -1;
    ui_draw();
}

/* Entering a list places the selection as well as the screen: ui_sel is shared
   by all three, so a brand index left over from one would be out of range in
   another. Callers use it to land on the item that leads back where they came
   from. */
static void ui_enter_list(uint8_t screen, int sel) {
    ui_sel = sel;
    ui_first = 0;
    ui_enter(screen);
}

/* Watch a job the list just asked for. `job` is 0 when nothing started, which
   is either a blast already in flight — watched rather than restarted — or a
   transmitter that is not there. */
static void ui_enter_blast(uint16_t job, int back_sel) {
    ui_blast_ok = (job != 0) || ir_busy();
    ui_blast_done_at = 0;
    ui_blast_back = ui_screen;
    ui_blast_back_sel = back_sel;
    if (job != 0) event_add("ir: job %u started from the menu", (unsigned)job);
    ui_enter(UI_BLAST);
}

/* Open one message. The card holds the id rather than the row, so that a
   message arriving or expiring while it is up cannot silently swap it for a
   different one. */
static void ui_enter_card(uint32_t id) {
    ui_msg_id = id;
    ui_card_fade = 0;
    ui_enter(UI_MSGCARD);
}

static void ui_activate(int item) {
    if (ui_screen == UI_MSGLIST) {
        int items = notify_count();
        if (item < items) {
            NotifyView v;
            if (notify_view(item, v)) ui_enter_card(v.id);
        } else {
            ui_enter_list(UI_MENU, UI_ITEM_MSG);
        }
        return;
    }

    if (ui_screen == UI_BRAND) {
        /* One named code, repeat 1 — which still means three frames for a Sony
           entry, because that is what SIRC needs. This is the point of the
           screen: the codes are power toggles, so blasting a set that holds
           several a television answers to turns it off, on and off again. */
        if (item < ir_code_count) ui_enter_blast(ir_start_code(item, 1), item);
        else ui_enter_list(UI_TVMENU, UI_TV_BRAND);
        return;
    }

    if (ui_screen == UI_TVMENU) {
        switch (item) {
            /* Repeat 1, the same default POST /ir/tvbgone applies to a
               body-less request. */
            case UI_TV_ALL: ui_enter_blast(ir_start_tvbgone(IR_REGION_ALL, 1), item); break;
            case UI_TV_NA:  ui_enter_blast(ir_start_tvbgone(IR_REGION_NA, 1), item);  break;
            case UI_TV_EU:  ui_enter_blast(ir_start_tvbgone(IR_REGION_EU, 1), item);  break;
            case UI_TV_BRAND: ui_enter_list(UI_BRAND, 0); break;
            default: ui_enter_list(UI_MENU, UI_ITEM_TVBGONE); break;
        }
        return;
    }

    switch (item) {
        case UI_ITEM_MSG:
            ui_enter_list(UI_MSGLIST, 0);
            break;
        case UI_ITEM_TVBGONE:
            ui_enter_list(UI_TVMENU, UI_TV_ALL);
            break;
        case UI_ITEM_AP:
            /* Same entry point as the hold gesture, and time-boxed the same
               way. Raising it from the menu is the reliable route in. */
            if (!ap_active) ap_start(true);
            ui_enter(UI_AP);
            break;
        case UI_ITEM_INFO:
            ui_enter(UI_INFO);
            break;
        default:
            ui_enter(UI_CLOCK);
            break;
    }
}

/* One level up, landing on the item that leads back down. */
static void ui_back() {
    switch (ui_screen) {
        case UI_BRAND:   ui_enter_list(UI_TVMENU, UI_TV_BRAND);   break;
        case UI_TVMENU:  ui_enter_list(UI_MENU, UI_ITEM_TVBGONE); break;
        case UI_MSGLIST: ui_enter_list(UI_MENU, UI_ITEM_MSG);     break;
        default:         ui_enter(UI_CLOCK);                      break;
    }
}

static void ui_init() {
    /* GPIO0 is a strapping pin: it is only ever read, never driven. */
    pinMode(PIN_ENC_KEY, INPUT_PULLUP);
    /* Pull-ups before attaching: the library defaults to pull-DOWN, which
       leaves both channels floating against the encoder's common ground and
       makes the count wander with no error of any kind. */
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    /* Direction belongs to UI_ENCODER_DIRECTION, not to the pin order here. */
    ui_encoder.attachFullQuad(PIN_ENC_A, PIN_ENC_B);
    /* Hardware glitch filter, in APB (80MHz) cycles, 10 bits wide. attach()
       leaves it at 250 (~3.1us); 1023 is the register maximum and about
       12.8us, far below the shortest edge a hand can produce and well above
       contact bounce. The quadrature channels get this instead of a software
       time filter, which would drop real detents during a fast spin. */
    ui_encoder.setFilter(1023);
    ui_encoder.clearCount();
    ui_encoder_reset();
    ui_last_input = millis();
}

/* Called every loop() pass. Nothing here blocks and nothing here draws unless
   something actually changed. */
static void ui_poll() {
    int steps = ui_encoder_steps();
    ui_button_poll(ui_enc_key);
    ui_button_poll(ui_user_key);

    /* The encoder key's long hold belongs to the recording gesture in
       mic_key_poll(), so only a short press counts as a click here. Without
       this the release that ends a recording would ALSO open the menu from the
       clock, on top of stopping the take — the same trap the user key was
       already guarded against below, and for the same reason. */
    bool click = ui_enc_key.click && ui_enc_key.held_ms < MIC_HOLD_MS;
    /* The user key's long hold belongs to the AP gesture in ap_key_poll(), so
       only a short press counts as "back" here. */
    bool back = ui_user_key.click && ui_user_key.held_ms < AP_KEY_HOLD_MS;

    if (steps != 0 || click || back) ui_last_input = millis();

    /* The knob's own feedback on the ring, which outranks whatever the ring was
       saying by itself: a hand is on the control. The screen and the ring get
       the same detents, and neither is told what the other did with them. */
    ring_knob(steps);

    /*
     * A recording takes the panel, from any screen and without being asked.
     *
     * It outranks everything else here for the same reason a running blast
     * does: it is live output, and there is no point in a level meter you
     * cannot see. It is also the only feedback that says the hold registered
     * at all — mic.cpp decides that on its own, off the pin level, and this is
     * how the UI finds out. Nothing below can take the screen back while a
     * take runs: the arrival branch only fires on the clock face, and the case
     * for UI_REC ignores input entirely.
     */
    if (mic_is_recording() && ui_screen != UI_REC) {
        ui_rec_done_at = 0;
        ui_enter(UI_REC);
        return;
    }

    /*
     * A message that arrives while the device is sitting on its clock puts
     * itself in front — that is the whole job. It does not take a screen away
     * from somebody using one: the unread badge and the Messages list are how
     * it gets noticed then. Either way the flag is consumed on the pass it
     * arrives, so a card can never appear fifteen seconds later when the idle
     * timer happens to land. And it yields to a hand already on the knob: a
     * pass carrying input is that user's pass.
     */
    if (steps == 0 && !click && !back) {
        uint32_t arrived = 0;
        if (notify_take_arrival(&arrived) && arrived != 0 &&
            ui_screen == UI_CLOCK) {
            ui_sel = 0;
            ui_first = 0;
            ui_enter_card(arrived);
            return;
        }
    }

    switch (ui_screen) {
        case UI_CLOCK:
            if (click) ui_enter_list(UI_MENU, 0);
            return;

        case UI_MENU:
        case UI_TVMENU:
        case UI_BRAND:
        case UI_MSGLIST: {
            if (back) { ui_back(); return; }
            if (steps != 0) {
                int count = ui_list_count();
                ui_sel += steps;
                /* Wrap rather than clamp: a knob with no end stops, so running
                   off one end should arrive at the other. */
                ui_sel %= count;
                if (ui_sel < 0) ui_sel += count;
                ui_draw();
            }
            if (click) { ui_activate(ui_sel); return; }
            break;
        }

        case UI_MSGCARD: {
            int idx = notify_index_of(ui_msg_id);
            /* Expired or evicted out from under the card. */
            if (idx < 0) { ui_enter_list(UI_MSGLIST, 0); return; }
            /* A click is "I have seen this"; the user key is the same plain
               step up it is on every other screen and leaves the message
               unread on purpose. */
            if (click) {
                notify_ack_id(ui_msg_id);
                ui_enter_list(UI_MSGLIST, idx);
                return;
            }
            if (back) { ui_enter_list(UI_MSGLIST, idx); return; }
            if (steps != 0) {
                int count = notify_count();
                if (count > 0) {
                    int n = (idx + steps) % count;
                    if (n < 0) n += count;
                    NotifyView v;
                    /* Re-fade on the way in, so moving through the stack reads
                       as one card replacing another rather than as text
                       changing inside a frame that never moved. */
                    if (notify_view(n, v)) { ui_sel = n; ui_enter_card(v.id); return; }
                }
            }
            break;
        }

        case UI_BLAST: {
            /* A click stops the job; the screen stays until it has actually
               wound down, then shows how it ended for a moment. Either way it
               goes back to the list the job was started from. */
            if (click) ir_stop_job();
            if (back) {
                ir_stop_job();
                ui_enter_list(ui_blast_back, ui_blast_back_sel);
                return;
            }
            if (ir_busy()) {
                ui_blast_done_at = 0;
            } else if (ui_blast_done_at == 0) {
                ui_blast_done_at = millis();
            } else if (millis() - ui_blast_done_at >= UI_RESULT_MS) {
                ui_enter_list(ui_blast_back, ui_blast_back_sel);
                return;
            }
            break;
        }

        case UI_REC:
            /* Input is not how this screen ends. The release of the very key
               that started the take is what stops it, and mic_key_poll() has
               already acted on that release by the time ui_poll() runs — so
               there is nothing here to react to, and reacting would mean
               reacting twice. The click that release produced was suppressed
               above by the MIC_HOLD_MS guard in any case. */
            if (mic_is_recording()) {
                ui_rec_done_at = 0;
                break;
            }
            /* The take is over: hold the result up long enough to read the
               peak, then go home. Signed, and against a fresh reading, as
               everything in this firmware that subtracts clocks now is. */
            if (ui_rec_done_at == 0) {
                ui_rec_done_at = millis();
            } else if ((long)(millis() - ui_rec_done_at) >= (long)UI_REC_RESULT_MS) {
                ui_enter(UI_CLOCK);
                return;
            }
            break;

        case UI_AP:
        case UI_INFO:
            if (click || back) {
                ui_enter_list(UI_MENU,
                              ui_screen == UI_AP ? UI_ITEM_AP : UI_ITEM_INFO);
                return;
            }
            break;
    }

    /* A running blast is live output and must not be timed out from under the
       user; everything else falls back to the clock. The recording screen is
       exempt outright rather than only while it records: a hold can outlast
       fifteen seconds of "no input" — holding a key is not input to the click
       state machine — and its own exit above is deterministic, so the idle
       timer has nothing to add and could only fire mid-sentence. */
    bool watching_blast = (ui_screen == UI_BLAST && ir_busy());
    bool watching_rec = (ui_screen == UI_REC);
    if (!watching_blast && !watching_rec && millis() - ui_last_input >= UI_IDLE_MS) {
        ui_enter(UI_CLOCK);
        return;
    }

    /* The card runs at the fade rate only while it is actually fading — three
       frames — and drops back to the ordinary tick the moment it has settled. */
    unsigned long interval =
        (ui_screen == UI_MSGCARD && ui_card_fade < MSG_FADE_STEPS)
            ? MSG_FADE_MS : UI_TICK_MS;

    if (millis() - ui_last_draw >= interval) {
        ui_last_draw = millis();
        if (!ui_input_driven(ui_screen)) ui_draw();
    }
}
