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
 * MENU also carries rows that open nothing. Quiet switches the night window on
 * and off, Backlight steps through the levels, and Auto-dim switches the
 * backlight's idle policy; each acts in place and rewrites its own label. They
 * are settings with no arguments, and a screen holding one line of that would
 * be a screen to get out of again.
 *
 * MSGCARD is the one screen where the click and the user key differ. A click
 * acknowledges the message and returns; the user key returns without
 * acknowledging, which is the only way to look at something and deliberately
 * leave it unread. That keeps the user key's meaning — one level up, change
 * nothing — intact everywhere in this file.
 *
 * It is also the one screen where the knob does not move a selection. A body
 * can be longer than the card, so on MSGCARD the knob reads: it scrolls the
 * text, and past the last line it arrives on the reply chips, which are drawn
 * under the text and are where rolling past the text should land. Walking the
 * stack of messages behind the card moved to a long press of the user key,
 * which is a gesture on this screen only. Alone among the knobs in this file
 * this one has end stops, and ui_card_step() says at length why.
 *
 * CLOCK is the home screen and is drawn exactly as before by display_tick();
 * this file does not touch it beyond handing control back. Every other screen
 * returns to CLOCK after UI_IDLE_MS without input, so a device left in a menu
 * on the shelf goes back to being a clock. A running blast is the one screen
 * that does not time out — it is live output, and its click means "stop". An
 * idle timeout never acknowledges anything: a pager that clears itself because
 * nobody was standing there is not a pager.
 *
 * The same timestamp drives a second, slower thing. ui_backlight_idle() hands
 * the elapsed time to skills/backlight.cpp, which takes the panel down to its
 * dimmest preset after BL_IDLE_DIM_MS and out altogether after BL_IDLE_OFF_MS —
 * the backlight is 85mA of a 150mA device, so this is most of what the battery
 * is spent on. Both thresholds live there, next to the policy that compares
 * against them, and are deliberately not repeated here. Any input puts the
 * panel straight back, and the two screens the timeout above exempts are
 * exempted from the dimming too, along with a third the timeout has no reason
 * to care about — see ui_watching_progress().
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
 * only format strings (see progress_status_line). So it is an include, not a
 * skill.
 *
 * Relationship to the API
 * -----------------------
 * The UI is a second front-end over the same state, never a second copy of it.
 * TV-B-Gone goes through ir_start_tvbgone()/ir_start_code()/ir_stop_job(), the
 * same functions POST /ir/tvbgone and /ir/tvbgone/stop call, the by-brand list
 * is ir_codes[] itself rather than a copy of the names, and the progress screen
 * renders ir_progress() whoever started the job. Opening the menu item while a
 * blast started over HTTP is running shows that blast instead of trying to
 * start a second one. Setup AP calls ap_start(), which is the only way the
 * device raises one after boot.
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
 * Scrolling the card is deliberately not a fourth fillRect. A detent repaints
 * the body rows through draw_field and moves one pixel of scrollbar, and it
 * must never raise display_force: on this screen that is what erases the whole
 * envelope and repaints the stack, the tint, the border and the accent bar,
 * which would be a visible flash on every click of the knob.
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
/* How long the user key must be held on a card to move to the next message
   behind it. MIC_HOLD_MS is the encoder key's own hold in mic.cpp; the two are
   the same number on purpose, so a hold means one length of time on this
   device rather than one per key. */
#define MSG_STACK_HOLD_MS    MIC_HOLD_MS

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
   press-then-release, not a level: holding the key must not repeat, and a hold
   long enough to be a gesture in its own right must not also read as a click.
   `held_ms` is what lets the caller tell the two apart — see ui_poll(). */
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
       ui_titles[] below are parallel arrays, so a value inserted in the middle
       silently shifts every title after it onto the wrong screen. The assert
       below catches a missing entry; nothing can catch a reordering, which is
       why the rule is "append". */
    UI_REC,
    UI_GATE,
    UI_GATE_STATUS
};

/* Capitals: this is the service text of the header bar, not prose. One entry
   per screen above, in the same order. */
static const char *ui_titles[] = {
    "", "MENU", "MESSAGES", "MESSAGES", "TV-B-GONE", "BY BRAND", "TV-B-GONE",
    "SETUP AP", "INFO", "RECORDING", "GATE", "GATE"
};

static_assert(sizeof(ui_titles) / sizeof(ui_titles[0]) == UI_GATE_STATUS + 1,
              "ui_titles[] must have one entry per screen, in enum order");

/* Top level. The marker on a selected row is also ">", so a submenu is spelled
   out with a trailing one rather than by a different marker. Messages leads
   because it is what this device is for; everything else is a tool it also
   happens to carry. */
enum {
    UI_ITEM_MSG = 0,
    UI_ITEM_QUIET,
    UI_ITEM_BACKLIGHT,
    UI_ITEM_DIM,
    UI_ITEM_TVBGONE,
    UI_ITEM_GATE,
    UI_ITEM_AP,
    UI_ITEM_INFO,
    UI_ITEM_BACK,
    UI_ITEM_COUNT
};

/* Quiet sits second, under Messages: it is the other thing this device does to
   messages, and it is the row somebody reaches for at one in the morning
   without wanting to read a menu first. The row's name is here; the hours after
   it come from ring_quiet_hours(), because they are the skill's to say — the
   same division as the Messages row and its unread count.

   Backlight sits third, next to Quiet, because the two are the settings a hand
   reaches for in the dark and both act in place rather than opening a screen.
   Same division again: the name is here, the level's word comes from
   bl_level_label().

   Auto-dim sits directly under Backlight because it decides what happens to
   the row above it: whether that level is what the panel holds, or only what
   it holds while somebody is there. Reading the two in that order is the whole
   explanation of the setting, which is why it is not filed under Info. */
static const char *ui_items[UI_ITEM_COUNT] = {
    "Messages >",   /* ui_list_label() appends the unread count */
    "Quiet",        /* ui_list_label() appends the ring's window, or "off" */
    "Backlight",    /* ui_list_label() appends the level's word, or its step */
    "Auto-dim",     /* ui_list_label() appends "on" or "off" */
    "TV-B-Gone >",
    "Gate >",
    "Setup AP",
    "Info",
    "Back"
};

/* Gate. The spare-remote app: one pairing sequence to enrol the device into
   the receiver (hold the gate's program button while Pair fires), then four
   buttons that each fire their own block sequence. Rows follow the TV-B-Gone
   submenu shape. */
enum {
    UI_GATE_PAIR = 0,
    UI_GATE_B1,
    UI_GATE_B2,
    UI_GATE_B3,
    UI_GATE_B4,
    UI_GATE_BACK,
    UI_GATE_COUNT
};

static const char *ui_gate_items[UI_GATE_COUNT] = {
    "Pair (hold gate btn)",
    "Button 1",
    "Button 2",
    "Button 3",
    "Button 4",
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

/*
 * Somebody is at the device. The one place ui_last_input is written, so that
 * "what counts as input" is a question with a list of callers rather than an
 * assignment scattered across two files.
 *
 * It matters more than it did before the panel could go dark. A screen timeout
 * that fires wrongly costs a trip back into a menu; a blank that fires wrongly
 * costs the owner the screen they are looking at, and every path that puts
 * something on the panel without going through here is a path that draws it
 * into the dark. main.cpp's ap_start() is one such path and calls this; that is
 * the reason it is a named function and not an assignment.
 */
static void ui_note_input() { ui_last_input = millis(); }
/* When the blast being watched stopped running, 0 while it still is. */
static unsigned long ui_blast_done_at = 0;
/* The same, for the recording screen: when the take ended, 0 while it runs. */
static unsigned long ui_rec_done_at = 0;
/* False when the menu could not start a blast and none was already running. */
static bool ui_blast_ok = true;
/* The list the blast was started from, so that finishing goes back to it. */
static uint8_t ui_blast_back = UI_TVMENU;
static int ui_blast_back_sel = 0;
/* When the gate job being watched stopped running, 0 while it still is. */
static unsigned long ui_gate_done_at = 0;
/* False when the menu could not start a gate job and none was already running. */
static bool ui_gate_ok = true;
/* What the menu asked for: the title while the staged job is not yet adopted
   (gate_poll() owns the counters until its next pass) and the row to land on
   when the status screen goes back to the list. */
static uint8_t ui_gate_kind = 0;
static uint8_t ui_gate_button = 0;
static int ui_gate_back_sel = 0;

/* Which notification the card is showing. An id, not an index: the list can
   shift underneath it when something expires or a new message arrives. */
static uint32_t ui_msg_id = 0;
/* Fade frames already drawn for the card in front, MSG_FADE_STEPS once done. */
static uint8_t ui_card_fade = 0;
/* Which reply chip the knob is on, for the card in front. Its own variable and
   deliberately not ui_sel: ui_sel is the row in the message list this card was
   opened from, and the card hands it straight back on the way out — a second
   meaning on it would move the list under the user. Meaningless on a message
   that carries no options, which is every message that never gets here. */
static int ui_chip_sel = 0;
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
 * case. The body gets the same 276px, 27 characters worst case in font 2 and
 * around 34 in practice, over as many lines as the band below the title holds.
 */
/* The numbers below are compiled verbatim on the host by tools/test_card.sh,
   which is what turns the line counts into arithmetic something checks. Keep
   every marker line self-closed and on a line of its own, and keep every
   comment inside the region fully closed: the slicer copies from the marker
   without understanding what it copies. */
/* host-test:begin cardgeom — sliced out by tools/test_card.sh */
#define MSG_CARD_X     8
#define MSG_CARD_Y    28
#define MSG_CARD_W   300
#define MSG_CARD_H   100
#define MSG_PAD_L     14
#define MSG_PAD_R     10
#define MSG_ACCENT_W   3
/* Offset of each card peeking out behind. Named MSG_CARD_PEEK rather than the
   obvious MSG_PEEK because lwIP's <sys/socket.h> already owns that name — it
   arrived in this translation unit the day skills/voice.cpp started including
   esp_http_client.h, and the firmware is one translation unit, so a UI layout
   constant and a socket recv() flag were suddenly the same macro. */
#define MSG_CARD_PEEK  3
#define MSG_HINT_Y   140
/* The band under the card is one padded field the width of the card itself.
   The padding is the erase: TFT_eSPI back-fills it with the background, so a
   string that changes length — and the chip row changes length whenever it is
   answered — leaves nothing of the previous one behind. Narrowing this is what
   would produce ghosting, so it is a budget the chip row is fitted into rather
   than a number to tune. Written as the card's width and not as the number that
   happens to be: this region exists so that these stop being literals. */
#define MSG_HINT_W   MSG_CARD_W

/*
 * The card's own rows, as offsets from its top edge.
 *
 * These were bare literals inside ui_draw_card() — y + 7, y + 28, y + 60,
 * y + 78 — which made the one number this screen turns on, how many lines of
 * body a reader gets, something no test could reach and no reader could check
 * without a ruler. They are named here so the count below is arithmetic
 * instead of a claim in a comment.
 *
 * MSG_LINE_H is chr_hgt_f16 from TFT_eSPI's Fonts/Font16.h, which is what
 * fontHeight(2) returns and what setTextPadding erases: the pitch is two
 * pixels more than that, so there is a 2px band between lines that no field's
 * padding ever covers. That band is why the scroll position is a line index
 * and never a pixel offset — see ui_card_line[].
 */
#define MSG_HDR_DY      7   /* source and age */
#define MSG_TITLE_DY   28   /* the title, font 4, 26px tall */
#define MSG_BODY_DY    60   /* first body line, under a title */
#define MSG_BODY_DY_TOP 25  /* first body line when the title is not drawn */
#define MSG_BODY_PITCH 18
#define MSG_LINE_H     16   /* chr_hgt_f16 */
/* The last row inside the card: the border occupies MSG_CARD_H - 1. */
#define MSG_BODY_BOTTOM (MSG_CARD_H - 2)
#define MSG_BODY_W      (MSG_CARD_W - MSG_PAD_L - MSG_PAD_R)

/* Whole lines of MSG_LINE_H at MSG_BODY_PITCH that fit between `dy` and the
   bottom of the card. */
#define MSG_BODY_FIT(dy) \
    ((MSG_BODY_BOTTOM - (dy) + 1 - MSG_LINE_H) / MSG_BODY_PITCH + 1)
/* Two with the title, four without it. The title is what the fourth and third
   lines cost, and ui_draw_card() spends it only on a body that needs them. */
#define MSG_BODY_ROWS     MSG_BODY_FIT(MSG_BODY_DY)
#define MSG_BODY_ROWS_MAX MSG_BODY_FIT(MSG_BODY_DY_TOP)
static_assert(MSG_BODY_ROWS == 2,
              "the short band is no longer two lines — the title rule below "
              "and the tests that pin it are stated in twos");
static_assert(MSG_BODY_ROWS_MAX == 4,
              "the full band is no longer four lines");
static_assert(MSG_BODY_ROWS_MAX > MSG_BODY_ROWS,
              "hiding the title buys no lines, so there is nothing to spend it on");
static_assert(MSG_BODY_DY_TOP >= MSG_HDR_DY + MSG_LINE_H,
              "the first body line would be drawn over the source and age row");

/* Font 2's extreme glyph widths, from TFT_eSPI's Fonts/Font16.c: 3px is the
   punctuation ('!', an apostrophe, a comma, a colon, a semicolon, a bar) and
   10px is 'M' and 'W'. Both are read back out of that table by
   tools/test_card.sh rather than trusted here, because both are load-bearing:
   the narrow one sizes the line cache and the wide one bounds the line count. */
#define MSG_GLYPH_W_MIN 3
#define MSG_GLYPH_W_MAX 10

/*
 * The most lines a body can wrap into, which is what ui_card_line[] is sized
 * for rather than a number chosen to look safe.
 *
 * A line ends either on a space or, for a word wider than the whole line, on
 * the last character that fits — so a line costs at least two bytes ("a ") and
 * a line that cheap forces the next one to be expensive: the space that made it
 * cheap was the LAST one inside the fitting prefix, so the following
 * MSG_BODY_W / MSG_GLYPH_W_MAX - 2 characters carry no space at all and the
 * next line must swallow them. Two consecutive lines therefore cost at least
 * one fitting prefix between them, and the worst case is bounded by two lines
 * per prefix, plus one for the remainder. tools/test_card.sh measures the real
 * worst case against this bound rather than restating it.
 */
#define MSG_BODY_FIT_MIN   (MSG_BODY_W / MSG_GLYPH_W_MAX)
#define MSG_BODY_MAX_LINES (2 * (NOTIFY_BODY_LEN / MSG_BODY_FIT_MIN + 1) + 1)

/* Where the scrollbar goes: one pixel at the card's inner right edge, running
   the height of the body band. Meshtastic puts its thumb at width - 2 of the
   screen it owns; the card is what this owns, so the same two pixels are
   measured off its own right edge instead. */
#define MSG_SCROLL_DX (MSG_CARD_W - 2)
#define MSG_SCROLL_MIN_H 6

/* One drawn body line, as bytes: a full line of the narrowest glyph, plus the
   terminator. It is the pixel budget that decides how much of a body a line
   carries, so it is the pixel budget that sizes the buffer. */
#define MSG_LINE_LEN (MSG_BODY_W / MSG_GLYPH_W_MIN + 1)
/* host-test:end */

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
   the longest string any of them draws — so it is keyed to the title field and
   not to a number, because draw_field compares only the first cache_size - 1
   bytes: a line narrower than what it is asked to hold makes two different
   titles agreeing on that prefix compare equal, and the card then keeps the
   previous message's title on screen. */
#define UI_ROW_LEN   (NOTIFY_TITLE_LEN + 4)
static char ui_row[UI_ROW_COUNT][UI_ROW_LEN];
/* Says the same thing to a reader who pins this back to a number: the four
   bytes are the ellipsis and its terminator, and a line short of that is a
   cache that cannot hold what the widest column draws through it. */
static_assert(sizeof(ui_row[0]) >= NOTIFY_TITLE_LEN + 4,
              "a row cache line is narrower than an ellipsised title");

/* The message list needs its own cache: three cells per row, and it is the one
   screen whose rows are not a single string. Each cell is wide enough for the
   longest string its column can ever be asked to draw — an ellipsised title —
   so that the cache always holds the whole of what is on the panel and two
   different rows can never compare equal.
   Which is the same width, and the same reason, as a row cache line, so it is
   defined from it rather than spelled out again: two names for one expression
   are two places to change and one of them gets missed. */
#define MSG_CELL_LEN UI_ROW_LEN
static char ui_msg_cell[MSG_ROWS][3][MSG_CELL_LEN];

/* The card's body lines get their own cache, one line per drawn row and each
   wide enough for the widest thing a row can hold: MSG_BODY_W pixels of the
   narrowest glyph font 2 has, plus the terminator. Sized to the pixels rather
   than to the body, because it is the pixels that decide how much of the body
   a line carries — and it must not be sized to ui_row[], which is
   UI_ROW_LEN = 69: draw_field compares only cache_size - 1 bytes, so two
   different lines of narrow glyphs agreeing on the first 68 would compare
   equal and the repaint would be skipped, leaving the previous scroll
   position's text on the panel. */
static char ui_card_body[MSG_BODY_ROWS_MAX][MSG_LINE_LEN];
static_assert(MSG_LINE_LEN > MSG_BODY_W / MSG_GLYPH_W_MIN,
              "a body cache line cannot hold a full line of the narrowest glyph");
static_assert(MSG_LINE_LEN > UI_ROW_LEN,
              "a body line is no wider than a row line — check MSG_BODY_W and "
              "the font table before making them share a cache");

/* The body itself is not cached as text: it is wrapped once into byte offsets
   into the message, and copied out one line at a time as it is drawn. Offsets
   rather than copies because a 256-byte body can wrap into MSG_BODY_MAX_LINES
   lines, and that many line-sized buffers would be several kilobytes on a
   device with 320 of them. See ui_wrap_lines() and ui_card_line[]. */

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
 *
 * The fitting helpers and the chip row further down are compiled verbatim on
 * the host by tools/test_chips.sh. Keep every marker line self-closed and on a
 * line of its own, and keep every comment inside a marked region fully closed:
 * the slicer copies from the marker without understanding what it copies.
 */

/* host-test:begin fit — sliced out by tools/test_chips.sh */
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
/* host-test:end */

/* ===== Wrapping the body over lines =====
 *
 * This replaces a two-line wrap that put an ellipsis on its second line to say
 * "it stops here". It stops nowhere now: the card scrolls, and the scrollbar
 * says where in the body a reader is far better than three dots ever did.
 * ui_ellipsis() is still what cuts the title, the list cells and the chips,
 * which are columns with no second line to continue onto.
 */
/* host-test:begin wrap — sliced out by tools/test_card.sh */
/* One wrapped line, as a place in the body rather than a copy of it. */
struct UiLine {
    uint16_t start;
    uint16_t len;
};

/*
 * Break `src` into lines of at most `max_px`, on a space where there is one,
 * and record where each line begins and how long it is. Returns the number of
 * lines, which is never more than `max_lines`.
 *
 * Called once when a card opens, not once a frame: the body of a stored
 * message does not change while it is being read, and re-measuring 256
 * characters against the font table at UI_TICK_MS would be work nothing asked
 * for. The scroll position then moves a line index over the result.
 *
 * A line never starts on a space — the gap a break was taken at belongs to
 * neither side — so no line is empty and no line is blank, which is what lets
 * the drawing below tell "this row has nothing on it" from "this row has text
 * that happens to be spaces".
 *
 * A word wider than the whole line has no space to break on and is cut, the
 * way the same case has always been cut here. `take` is floored at one
 * character so that a line always consumes something: a glyph wider than
 * `max_px` would otherwise consume nothing and this would not terminate.
 */
static int ui_wrap_lines(const char *src, UiLine *out, int max_lines,
                         uint8_t font, int max_px) {
    if (!src || !out || max_lines <= 0) return 0;

    int n = 0;
    size_t p = 0;
    while (n < max_lines) {
        while (src[p] == ' ') p++;
        if (!src[p]) break;

        int acc = 0, last_space = -1;
        size_t i = p;
        for (; src[i]; i++) {
            /* A gap is noted before it is measured, so the gap the line runs
               out ON is still a place to break. Measuring first loses the last
               word of a line that fills the band exactly: the space after it
               does not fit, and the break falls back to the space before it. */
            if (src[i] == ' ') last_space = (int)(i - p);
            int cw = ui_char_w(src[i], font);
            if (acc + cw > max_px) break;
            acc += cw;
        }

        size_t take;
        if (!src[i])             take = i - p;              /* the rest fits */
        else if (last_space > 0) take = (size_t)last_space;
        else                     take = i - p;              /* one long word */
        if (take == 0) take = 1;

        out[n].start = (uint16_t)p;
        out[n].len   = (uint16_t)take;
        n++;
        p += take;
    }
    return n;
}

/* Copy one line out of the body it was measured in. */
static void ui_line_text(char *dst, size_t n, const char *src, UiLine ln) {
    if (!dst || n == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t take = ln.len;
    if (take > n - 1) take = n - 1;
    memcpy(dst, src + ln.start, take);
    dst[take] = '\0';
}

/*
 * Force the next draw of a cached field, without display_force.
 *
 * A scroll step changes what every body row says, and the obvious way to make
 * draw_field notice is display_force — which on this screen is the second half
 * of `if (fading || display_force)` and would re-erase the whole 306x112
 * envelope, redraw the stack outlines, the tint, the border and the accent bar.
 * That is a visible flash on every detent, for the two or four lines of body
 * text a step actually changes.
 *
 * So the cache is poisoned instead. Emptying it is not enough: draw_field
 * compares the cache against the new text, and a row that has nothing to show
 * would be handed "" and match an emptied cache exactly, leaving the previous
 * line on the panel. The poison is a byte notify_copy_text() replaces with a
 * space in everything it stores, so no line drawn here can ever equal it —
 * including the empty one.
 */
#define UI_CACHE_POISON '\x01'
static_assert(UI_CACHE_POISON < 0x20,
              "the cache poison is a byte a stored message can contain");
static void ui_cache_drop(char *cache) {
    cache[0] = UI_CACHE_POISON;
    cache[1] = '\0';
}
/* host-test:end */

/* The wrapped body and where the reader is in it. Below the region above
   because they are typed by it. */
static UiLine ui_card_line[MSG_BODY_MAX_LINES];
static int ui_card_lines = 0;         /* lines the body wrapped into */
static uint32_t ui_card_wrapped = 0;  /* the id ui_card_line[] describes */
static int ui_card_first = 0;         /* first visible line */
/* Scroll position the body rows were last drawn at, so that a moved one is
   recognised — the same shape as ui_first_drawn on the message list. */
static int ui_card_first_drawn = -1;
/* Whether the title is drawn, decided once with the wrap: a body that fits the
   short band keeps its title, a longer one spends it on two more lines. */
static bool ui_card_titled = true;
/* Thumb the scrollbar currently has on the panel, so a pass that changes
   nothing costs no SPI — the same rule every text field here follows, kept by
   hand because the bar is pixels rather than a string. -1 is "the band is bare
   card", which is also what a repaint of the card leaves behind. */
static int ui_card_thumb_y = -1;
static int ui_card_thumb_h = -1;
/* A press that was still down when the card it began on went away — the message
   expired or was evicted mid-hold. Its release is still to come, and on the list
   the card dropped to it would read as a plain step up: one gesture, two levels
   out. This is what ui_poll() discards it by. Set only while the key is down and
   cleared by the very release it was set for. */
static bool ui_drop_user_click = false;

/* Service text is capitals. Sources arrive as whatever an agent typed. */
/* host-test:begin fit — sliced out by tools/test_chips.sh */
static void ui_caps(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}
/* host-test:end */

/* ===== Reply chips =====
 *
 * A notification may carry up to four short labels, and the knob picks one of
 * them. They are drawn in the band under the card, in place of the hint that
 * band shows for a message with nothing to pick.
 *
 * The whole row is ONE string with marks in it, the way the menu draws its
 * selection: a step then costs one padded draw_field over the MSG_HINT_W band,
 * with no fillRect and no display_force, instead of four fields plus the
 * inverse bars an inverse-bar selection would need. The padding is what erases
 * the previous row — it is at least as wide as the widest row that can be
 * built here, which is the property the ellipsising below guarantees and the
 * one tools/test_chips.sh pins.
 */
/* host-test:begin chips — sliced out by tools/test_chips.sh */
/* One chip's text once it has been fitted: a label and its terminator are
   NOTIFY_OPT_LEN already, and the ellipsis can add three characters on top of
   15 kept whole — 19 bytes at worst. The fourth byte is spare rather than
   counted out to nothing, since the room costs one byte per chip. */
#define UI_CHIP_LEN     (NOTIFY_OPT_LEN + 4)
/* The whole row: every chip carries two wrapper characters, one of them also
   carries the answered mark, and the row carries a terminator. */
#define UI_CHIP_ROW_LEN (NOTIFY_OPT_MAX * (UI_CHIP_LEN + 2) + 2)
/* Two marks rather than one, because they answer two different questions —
   what a click would do, and what a click already did. A message can be
   answered again, which puts both marks on the same chip. */
#define UI_CHIP_OPEN    '['
#define UI_CHIP_CLOSE   ']'
#define UI_CHIP_ANSWER  '*'

/*
 * Build the row: the labels, in the order they arrived, with the selected one
 * bracketed and the answered one starred.
 *
 * Every chip pays for both of its wrapper characters whether it is the selected
 * one or not. That gives the chips their gap without a separator that would
 * have to be counted separately, and it keeps the row's width the same
 * wherever the selection sits — exactly one chip is bracketed at a time — so
 * the band is sized once for the widest row a detent can produce. A bracket is
 * two pixels narrower than a space, so the labels either side of the selection
 * do shift by that much as it moves; holding them still would mean a field per
 * chip, four cached strings and four draw_field calls a step, for two pixels.
 *
 * `row_px` is the pixel budget the whole row has to fit in, and it is shared
 * out: each chip gets what is left after the wrappers and the one answered mark
 * are paid for. A label wider than its share is cut with the same trailing
 * ellipsis every other column on this device uses — 15 bytes of label can be
 * wider than a quarter of the row, and a chip that says "ROLLBA..." is a chip
 * you can still ask about, while one clipped mid-glyph is a smear.
 *
 * `sel` and `chosen` outside 0..count-1 simply mark nothing, which is what an
 * unanswered message (chosen = -1) needs and what a card nobody has turned the
 * knob on yet (sel = -1) needs: a row with no bracket anywhere on it says the
 * click would not answer, which on that card it does not. It is also what an
 * index arriving from the store or from a snapshot outside 0..count-1 gets,
 * rather than a read past the labels there are.
 */
static void ui_chip_row(char *dst, size_t n, const NotifyOptions &opts,
                        uint8_t count, int sel, int chosen, int row_px,
                        uint8_t font) {
    if (!dst || n == 0) return;
    dst[0] = '\0';
    if (count == 0) return;
    if (count > NOTIFY_OPT_MAX) count = NOTIFY_OPT_MAX;

    int wrap_px = 2 * ui_char_w(' ', font);
    int budget = (row_px - (int)count * wrap_px - ui_char_w(UI_CHIP_ANSWER, font))
                 / (int)count;
    if (budget < 1) budget = 1;

    size_t out = 0;
    for (int i = 0; i < (int)count; i++) {
        char caps[UI_CHIP_LEN], text[UI_CHIP_LEN];
        char mark[2] = { (i == chosen) ? UI_CHIP_ANSWER : '\0', '\0' };

        ui_caps(caps, sizeof(caps), opts.label[i]);
        ui_ellipsis(text, sizeof(text), caps, font, budget);

        int wrote = snprintf(dst + out, n - out, "%c%s%s%c",
                             (i == sel) ? UI_CHIP_OPEN : ' ', mark, text,
                             (i == sel) ? UI_CHIP_CLOSE : ' ');
        /* The caller's buffer is sized for the worst case this can build, so a
           short one is a mistake elsewhere: stop with whole chips in it rather
           than leave half a label and an unbalanced bracket on the panel. */
        if (wrote < 0 || (size_t)wrote >= n - out) {
            dst[out] = '\0';
            return;
        }
        out += (size_t)wrote;
    }
}

/*
 * The selection the click is allowed to record, given the options the message
 * actually carries.
 *
 * This used to be a stepper as well, and the knob walked the chips through it.
 * The knob is the body's now — see ui_card_step(), which owns every position on
 * this card, chips included — so what is left here is the check that was always
 * the important half: `sel` at or above `count` is a value that did not come
 * from a detent on this row. A `chosen` read back out of the store or out of a
 * snapshot is the way in, and a snapshot is only as trustworthy as the file it
 * came from, so it is clamped rather than handed to notify_choose_id() as an
 * answer to a question with fewer answers than that.
 *
 * A negative `sel` reports the first chip rather than an index, which is why
 * the click path asks ui_chip_answers() whether there is a selection at all
 * before it asks this what the selection is.
 */
static int ui_chip_clamp(int sel, uint8_t count) {
    if (count == 0) return 0;
    if (sel < 0) return 0;
    if (sel >= (int)count) return (int)count - 1;
    return sel;
}

/*
 * Where the knob sits the moment a card opens: on the answer the message
 * already carries, or on nothing at all.
 *
 * Nothing is the answer for a message nobody has replied to, and it is not a
 * detail of the drawing. A card can arrive in front of somebody who never asked
 * for it — ui_poll() opens one on arrival while the device sits on its clock —
 * and the click that comes next has meant "dismiss" on every firmware this
 * device has run. Starting the knob on a chip would file that reflex as a
 * deliberate answer, and by convention the first chip is the one that says yes.
 *
 * `chosen` is bounds-checked against the count rather than only tested for -1,
 * because it arrives from the store and a snapshot is only as trustworthy as
 * the file it came out of: an answer pointing past the labels that are there is
 * no answer, and starting the knob on it would let the click that leaves record
 * it as one.
 *
 * Its own function, rather than one line inside ui_enter_card(), only so that
 * the host suite can reach it — the caller needs the store, the screen state
 * and the encoder, and cannot be sliced. Same reason notify_take_id() is its
 * own function in notify.cpp.
 */
static int ui_chip_start(int chosen, uint8_t count) {
    if (chosen < 0 || chosen >= (int)count) return -1;
    return chosen;
}

/*
 * Does this click record an answer, or is it the plain acknowledge every other
 * card gets?
 *
 * It answers only when the message asks something AND the knob has been turned
 * to one of the chips. Both halves matter and they fail in opposite ways: with
 * no options there is nothing to record, and with options but no selection
 * there is nobody's decision to record — see ui_chip_start() for why a card
 * can be in front of somebody who never chose to open it.
 *
 * Extracted for the same reason as ui_chip_start(), and next to it because the
 * two are one rule in two halves: the click may only answer what an entry to
 * the card, or a detent since, actually selected.
 */
static bool ui_chip_answers(int sel, uint8_t count) {
    return count > 0 && sel >= 0;
}
/* host-test:end */

/* The chip row's own cache, sized by the region above rather than shared with
   ui_row[]: four 15-character labels and their marks are longer than a row
   line, and draw_field compares only the first cache_size-1 bytes — a cache
   shorter than what is on the panel lets two different rows compare equal and
   silently skips the repaint. */
static char ui_card_hint[UI_CHIP_ROW_LEN];

/* ===== The knob, on a card =====
 *
 * The body no longer fits the card, so the knob reads it. That leaves the
 * chips, which the knob used to walk, with no input of their own — and this is
 * where they get it back: the card is one axis, the body's lines first and the
 * chips under the last of them, exactly as they are drawn. Rolling past the end
 * of the text arrives on the reply row because on the panel that is where the
 * reply row is.
 *
 * Walking the stack of messages behind this one has moved off the knob and onto
 * a long press of the user key. It kept the knob for as long as a card was one
 * screenful; now that a card can be six, browsing and reading cannot share a
 * control.
 */
/* host-test:begin scroll — sliced out by tools/test_card.sh */
/* Where the knob is on a card: which body line is at the top of the band, and
   which chip is picked, or -1 for none. */
struct UiCardPos {
    int first;
    int chip;
};

/*
 * One turn of the knob on a card.
 *
 * It STOPS at both ends, and that is a deliberate exception to this device's
 * own rule that a knob has no end stops — the message list, the menu and the
 * brand list all wrap, and say so where they do it. The reason is that this one
 * is not a list: it is a paragraph. Of four shipped firmwares read for this
 * (Bruce, which ships a board file for this exact hardware, Meshtastic, Flipper
 * and EdgeTX) not one wraps scrolled text. Reading is not browsing, and landing
 * in the middle of the first sentence after the last line loses a reader's
 * place with no way to tell that it happened. Do not "fix" this into a wrap.
 *
 * The chips sit at the top of the axis rather than in a mode of their own, so
 * the invariant ui_chip_start() exists for survives a card that scrolls: a
 * message nobody deliberately read still has no chip selected when the click
 * arrives, because the only way onto the chips is to have turned the knob past
 * every line of the body. On a message short enough not to scroll that is one
 * detent, which is exactly what it was before this screen scrolled at all.
 *
 * The backlog is clamped before it is added rather than applied a step at a
 * time: ui_encoder_steps() returns the net detents since the last pass, and a
 * fast spin is one jump here, not one redraw per detent.
 */
static UiCardPos ui_card_step(UiCardPos pos, int steps, int max_first, int chips) {
    if (max_first < 0) max_first = 0;
    if (chips < 0) chips = 0;
    int span = max_first + chips;

    /* A guard rather than arithmetic any caller needs: it keeps the addition
       below in range for any int, whatever a spun encoder hands in. The clamp
       at the end of the axis gives the same answer without it, so no test
       claims it — tools/test_card.sh says so by name. */
    if (steps > span)  steps = span;
    if (steps < -span) steps = -span;

    /* Where the position given is on that axis. A chip index or a scroll
       position that the message no longer supports is clamped to its own end of
       the axis rather than allowed to spill across the join: a stale scroll
       position is a place in the text, and it must not arrive on a chip and put
       an answer under the next click. */
    int at;
    if (pos.chip >= 0 && chips > 0) {
        /* The clamp here is the same guard as the one on `steps`: the axis
           clamp below already brings a stale index back to the last chip, and
           this only keeps the addition in range on the way. */
        int c = (pos.chip < chips) ? pos.chip : chips - 1;
        at = max_first + 1 + c;
    } else {
        at = pos.first;
        if (at < 0) at = 0;
        if (at > max_first) at = max_first;
        /* A chip on a message that carries none is not a position at all; the
           bottom of the text is the nearest place it can mean. */
        if (pos.chip >= 0) at = max_first;
    }

    at += steps;
    if (at < 0) at = 0;
    if (at > span) at = span;

    UiCardPos out;
    if (at > max_first) {
        out.first = max_first;
        out.chip = at - max_first - 1;
    } else {
        out.first = at;
        out.chip = -1;
    }
    return out;
}

/*
 * The scrollbar's thumb, in pixels down the track.
 *
 * Meshtastic's geometry, kept as it stands there: a thumb and no track, a
 * minimum height so that a long body still leaves something to see, and — the
 * part worth copying rather than reinventing — a position normalised over
 * `track_h - thumb_h` instead of over the whole track. That is what puts the
 * bottom of the thumb flush with the bottom of the track at maximum scroll.
 * Flipper and EdgeTX normalise over the full track, and their thumbs either
 * overrun the end or stop short of it.
 */
static void ui_scroll_thumb(int first, int total, int visible, int track_h,
                            int *thumb_y, int *thumb_h) {
    /* The card asks for two lines or four and never for none, so this stands
       between nothing and the division below. Kept because "nothing calls it
       that way today" is not a reason for the arithmetic to be undefined when
       something does. */
    if (visible < 1) visible = 1;
    if (total < visible) total = visible;

    int h = (int)((long)track_h * visible / total);
    if (h < MSG_SCROLL_MIN_H) h = MSG_SCROLL_MIN_H;
    if (h > track_h) h = track_h;

    int max_scroll = total - visible;
    if (first < 0) first = 0;
    if (first > max_scroll) first = max_scroll;
    if (max_scroll < 1) max_scroll = 1;

    if (thumb_h) *thumb_h = h;
    if (thumb_y) *thumb_y = (int)((long)(track_h - h) * first / max_scroll);
}
/* host-test:end */

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
        case UI_GATE:    return UI_GATE_COUNT;
        case UI_MSGLIST: return notify_count() + 1; /* the messages, then Back */
    }
    return 0;
}

static const char *ui_list_label(int i) {
    /* The Messages row carries its unread count and the Quiet row carries the
       window's hours, so those labels are built rather than looked up.
       ui_draw_list() consumes the return value into the row string before
       asking for the next one, so one shared buffer is enough. */
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
            if (i == UI_ITEM_QUIET) {
                /* "Quiet 00:00-08:00" or "Quiet off" — the row's name from the
                   table above, the window from the skill that owns it. The
                   longer of the two is 17 characters, which with the row's
                   marker measures 218px of font 4 against the 296px
                   ui_draw_row() has — measured against TFT_eSPI's own width
                   table, like every other column here, and pinned by
                   tools/test_quiet.sh at the hours' end. */
                char win[16];
                ring_quiet_hours(ring_night_from, ring_night_to, win, sizeof(win));
                snprintf(built, sizeof(built), "%s %s", ui_items[i], win);
                return built;
            }
            if (i == UI_ITEM_BACKLIGHT) {
                /* "Backlight day" for one of the four the menu offers, or
                   "Backlight step 13" for a level only the endpoint can set —
                   the row never rounds one into the other. The longest is the
                   step form, 210px of font 4 with the row's marker against the
                   296px ui_draw_row() has, measured against TFT_eSPI's own
                   width table like every other column here and pinned by
                   tools/test_backlight.sh at the label's end. */
                char level[BL_LABEL_MAX];
                bl_level_label(bl_wanted, level, sizeof(level));
                snprintf(built, sizeof(built), "%s %s", ui_items[i], level);
                return built;
            }
            if (i == UI_ITEM_DIM) {
                /* "Auto-dim on" or "Auto-dim off" — the row's name from the
                   table above, the state from the skill that owns it, exactly
                   as the two rows before it are built. "Auto-dim off" is the
                   longer of the two states at twelve characters against the
                   Backlight row's seventeen, so it needs no measurement of its
                   own: the row it sits under already cleared the width with
                   five characters to spare. */
                snprintf(built, sizeof(built), "%s %s", ui_items[i], bl_idle_word());
                return built;
            }
            return ui_items[i];
        case UI_TVMENU: return ui_tv_items[i];
        case UI_BRAND:  return (i < ir_code_count) ? ir_codes[i].brand : "Back";
        case UI_GATE:
            /* The Pair row says so when there is no radio to pair with: the
               screen is reachable either way, and a row that silently did
               nothing would read as a broken button. */
            if (i == UI_GATE_PAIR && !gate_ready) {
                snprintf(built, sizeof(built), "%s (no radio)", ui_gate_items[i]);
                return built;
            }
            return ui_gate_items[i];
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
 * moving; once settled nothing repaints but the age, the body rows the knob
 * moved and the scrollbar's thumb — all through draw_field or a one-pixel
 * fillRect against the tint rather than against black.
 */

/* The body, wrapped once for this card and not again. ui_msg_id is the key,
   and ui_enter_card() is what clears it. */
static void ui_card_wrap(const NotifyView &v) {
    if (ui_card_wrapped == ui_msg_id) return;
    ui_card_lines = ui_wrap_lines(v.body, ui_card_line, MSG_BODY_MAX_LINES, 2,
                                  MSG_BODY_W);
    /* The title is the price of the third and fourth lines, and it is only
       worth paying when there is something to put on them. A message short
       enough to be read in the short band keeps its title; a longer one gives
       it up, because a title a reader can see anyway in the list is worth less
       than half the words of the message. */
    ui_card_titled = (ui_card_lines <= MSG_BODY_ROWS);
    ui_card_wrapped = ui_msg_id;
}

/* Body lines this card shows at once, and the furthest down it can be scrolled.
   Both are read by the drawing and by the input, which is why they are here
   rather than inside either. */
static int ui_card_rows() {
    return ui_card_titled ? MSG_BODY_ROWS : MSG_BODY_ROWS_MAX;
}

static int ui_card_max_first() {
    int m = ui_card_lines - ui_card_rows();
    return m > 0 ? m : 0;
}

static void ui_draw_card() {
    NotifyView v;
    int idx = 0, total = 0;
    /* Entry, position and depth of the stack in one acquisition: an arrival
       between two of them would draw one card's text under another card's
       counter. Gone — expired or evicted while it was on screen — is the false
       return; ui_poll() leaves for the list on this same pass, so there is
       nothing to draw. */
    if (!notify_view_by_id(ui_msg_id, v, &idx, &total)) return;

    /* Before anything is measured against it: both the title rule and the
       number of rows below come out of the wrap, and it is done once per card
       rather than once per frame. */
    ui_card_wrap(v);

    uint16_t level = ui_level_color(v.level);

    bool fading = (ui_card_fade < MSG_FADE_STEPS);
    uint8_t step = fading ? (uint8_t)(ui_card_fade + 1) : (uint8_t)MSG_FADE_STEPS;
    int32_t x = MSG_CARD_X;
    int32_t y = MSG_CARD_Y + (MSG_FADE_STEPS - step) * MSG_CARD_PEEK;

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
        tft.fillRect(MSG_CARD_X, MSG_CARD_Y, MSG_CARD_W + 2 * MSG_CARD_PEEK,
                     MSG_CARD_H + 4 * MSG_CARD_PEEK, COL_BG);

        /* Two more cards behind, when there are two more to be behind: the
           stack a long press of the user key walks. Outlines only, dimmer with
           depth. */
        if (total > 1) {
            for (int p = 2; p >= 1; p--) {
                uint8_t a = (uint8_t)(MSG_BORDER_A / (p + 1) * step / MSG_FADE_STEPS);
                tft.drawRect(x + p * MSG_CARD_PEEK, y + p * MSG_CARD_PEEK,
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

    /* Every column below is MSG_BODY_W, written out by that name each time
       rather than through a local: it is the same budget ui_card_wrap()
       measured the body against, and a local would be a second name for it —
       which is exactly what one of them being changed and the other not looks
       like. */
    char caps[NOTIFY_SOURCE_LEN], src[NOTIFY_SOURCE_LEN], age[16];
    ui_caps(caps, sizeof(caps), v.source[0] ? v.source : "device");
    ui_ellipsis(src, sizeof(src), caps, 2, MSG_BODY_W - MSG_AGE_W - 8);
    notify_age_str(v.age_s, age, sizeof(age));
    draw_field(ui_row[0], sizeof(ui_row[0]), src, tx, y + MSG_HDR_DY, 2,
               c_sec, TL_DATUM, (uint16_t)(MSG_BODY_W - MSG_AGE_W - 8), tint);
    draw_field(ui_row[1], sizeof(ui_row[1]), age, rx, y + MSG_HDR_DY, 2,
               c_sec, TR_DATUM, MSG_AGE_W, tint);

    /* Keyed to the field, like the row cache it is drawn through: at the
       narrowest glyphs font 4 has, about fifty-five characters cross this
       column, so a fixed buffer sized for the old title would cut the card's
       title before the pixels did. The card geometry note above counts the
       same 276px the other way — 11 characters at the widest glyph and around
       18 of ordinary mixed case — because that is what a reader gets; a buffer
       has to survive the narrowest.
       Two asserts, because the two directions fail differently and both fail
       quietly. Wider than the cache is the aliasing one: draw_field compares
       only the first cache_size - 1 bytes, so a string wider than the cache it
       is drawn through makes two different titles compare equal and leaves the
       previous message's title on the card. Narrower than the field is the
       hardcoded 48 this buffer stopped being — a second, quieter cut ahead of
       the pixels — so it is pinned from below as well.
       Both are about the buffer declared below and not about UI_ROW_LEN, which
       the assert beside ui_row[] already pins. They read as a restatement of it
       only for as long as this buffer is declared from the same macro: write
       `char title[64]` here and the lower of the two is the one that fails, and
       nothing else in the file would. */
    if (ui_card_titled) {
        char title[UI_ROW_LEN];
        static_assert(sizeof(title) <= sizeof(ui_row[0]),
                      "the card title is wider than the cache it is compared in");
        static_assert(sizeof(title) >= NOTIFY_TITLE_LEN + 4,
                      "the card title is narrower than an ellipsised title");
        ui_ellipsis(title, sizeof(title), v.title, 4, MSG_BODY_W);
        draw_field(ui_row[2], sizeof(ui_row[2]), title, tx, y + MSG_TITLE_DY, 4,
                   c_pri, TL_DATUM, (uint16_t)MSG_BODY_W, tint);
    }

    /*
     * The body: as many lines as the band holds, starting at the one the knob
     * has scrolled to.
     *
     * A moved scroll position changes what every row says, and the way to make
     * draw_field notice is emphatically NOT display_force — on this screen that
     * is the second half of the branch above, and raising it would re-erase the
     * whole envelope and repaint the stack, the tint, the border and the accent
     * bar on every single detent. The caches are dropped instead; see
     * ui_cache_drop() for why they are poisoned rather than emptied.
     */
    int32_t body_y = y + (ui_card_titled ? MSG_BODY_DY : MSG_BODY_DY_TOP);
    int rows = ui_card_rows();

    if (ui_card_first != ui_card_first_drawn) {
        for (int r = 0; r < MSG_BODY_ROWS_MAX; r++) ui_cache_drop(ui_card_body[r]);
        ui_card_first_drawn = ui_card_first;
    }

    for (int r = 0; r < rows; r++) {
        char line[MSG_LINE_LEN];
        static_assert(sizeof(line) == sizeof(ui_card_body[0]),
                      "a body line is not the size of the cache it is compared "
                      "in — draw_field only compares cache_size - 1 bytes, so "
                      "the wider of the two aliases silently");
        int i = ui_card_first + r;
        line[0] = '\0';
        if (i < ui_card_lines) ui_line_text(line, sizeof(line), v.body, ui_card_line[i]);
        draw_field(ui_card_body[r], sizeof(ui_card_body[r]), line, tx,
                   body_y + r * MSG_BODY_PITCH, 2, c_sec, TL_DATUM,
                   (uint16_t)MSG_BODY_W, tint);
    }

    /* One pixel at the card's right edge, and only when there is something
       below the fold. It is drawn by hand rather than through draw_field
       because it is pixels and not a string, so it carries its own change test
       — like the progress bar on the clock face, and for the same reason. */
    if (ui_card_lines > rows) {
        int32_t track_y = body_y;
        /* Always the tall band, with no arm for the short one: a body with more
           lines than the band holds is a body that gave its title up for them,
           because ui_card_wrap() keeps the title only for a body that fits
           MSG_BODY_ROWS — and this branch is precisely the case where it does
           not. A `titled ? :` here would be a second layout that cannot occur. */
        int track_h = MSG_BODY_BOTTOM - MSG_BODY_DY_TOP + 1;
        int ty = 0, th = 0;
        ui_scroll_thumb(ui_card_first, ui_card_lines, rows, track_h, &ty, &th);
        if (display_force || ty != ui_card_thumb_y || th != ui_card_thumb_h) {
            tft.fillRect(x + MSG_SCROLL_DX, track_y, 1, track_h, tint);
            tft.fillRect(x + MSG_SCROLL_DX, track_y + ty, 1, th, c_sec);
            ui_card_thumb_y = ty;
            ui_card_thumb_h = th;
        }
    }

    /* Below the card, on the ground: what the knob does next. A message that
       carries reply options answers that with the options themselves, drawn in
       the accent so they read as the live control rather than as a caption —
       everything else on this screen is text about the message. Both go into
       the same field, and both are erased by the same padding. */
    if (v.opt_count > 0) {
        char chips[UI_CHIP_ROW_LEN];
        ui_chip_row(chips, sizeof(chips), v.options, v.opt_count,
                    ui_chip_sel, v.chosen, MSG_HINT_W, 2);
        draw_field(ui_card_hint, sizeof(ui_card_hint), chips,
                   tft.width() / 2, MSG_HINT_Y, 2, COL_ACCENT, TC_DATUM,
                   MSG_HINT_W);
    } else {
        draw_field(ui_card_hint, sizeof(ui_card_hint),
                   v.unread ? "CLICK TO ACK" : "CLICK TO GO BACK",
                   tft.width() / 2, MSG_HINT_Y, 2, COL_DIM, TC_DATUM,
                   MSG_HINT_W);
    }

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

/* One line of what the gate status screen watches: the paired kind names its
   button, a raw job staged over HTTP has no button to name. Stack buffer, no
   allocation — this runs on every tick while the screen is up. */
static void ui_gate_title(char *out, size_t n, uint8_t kind, uint8_t button) {
    if (kind == GATE_JOB_PAIR) snprintf(out, n, "Pair");
    else if (kind == GATE_JOB_BUTTON) snprintf(out, n, "Button %u", (unsigned)button);
    else snprintf(out, n, "Raw");
}

/* The gate TX status: what was asked for, which block is on the air, how many
   frames left the chip, how the last job ended. The blast-screen shape: the
   job is staged the moment the menu item is clicked, but gate_poll() only
   adopts it — and resets the counters — on its next pass, so a staged job
   still shows the previous counters and says "starting" instead. */
static void ui_draw_gate_status() {
    GateProgress p = gate_progress();
    char buf[40];

    /* The job is staged but gate_poll() has not adopted it yet. */
    bool starting = gate_busy() && !p.running;

    if (!ui_gate_ok) {
        ui_draw_row(0, gate_ready ? "Busy" : "No radio", 40, 4, COL_TIME);
        ui_draw_row(1, "", 78, 4, COL_DIM);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    } else if (starting) {
        ui_gate_title(buf, sizeof(buf), ui_gate_kind, ui_gate_button);
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        ui_draw_row(1, "starting", 78, 4, COL_ACCENT);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    } else if (p.running) {
        ui_gate_title(buf, sizeof(buf), p.kind, p.button);
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        /* p.sent, not a step count: frames the loop has actually keyed. */
        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)p.sent, (unsigned)p.total);
        ui_draw_row(1, buf, 78, 4, COL_ACCENT);
        /* All frames are out but the poll has not wound the job down yet. */
        if (p.sent >= p.total) snprintf(buf, sizeof(buf), "finishing");
        else snprintf(buf, sizeof(buf), "block %u of %u",
                      (unsigned)p.block, (unsigned)p.blocks);
        ui_draw_row(2, buf, 112, 2, COL_DIM);
    } else {
        snprintf(buf, sizeof(buf), "%s", p.result[0] ? p.result : "idle");
        ui_draw_row(0, buf, 40, 4, COL_TIME);
        snprintf(buf, sizeof(buf), "%u / %u sent", (unsigned)p.sent, (unsigned)p.total);
        ui_draw_row(1, buf, 78, 4, COL_DIM);
        ui_draw_row(2, "", 112, 2, COL_DIM);
    }
    /* A click or a detent leaves for the gate list; the job keeps running. */
    ui_draw_row(3, "click to go back", 148, 2, COL_DIM);
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
    return screen == UI_MENU || screen == UI_TVMENU || screen == UI_BRAND ||
           screen == UI_GATE;
}

static void ui_draw() {
    switch (ui_screen) {
        case UI_MENU:
        case UI_TVMENU:
        case UI_BRAND:
        case UI_GATE:    ui_draw_list();    break;
        case UI_MSGLIST: ui_draw_msglist(); break;
        case UI_MSGCARD: ui_draw_card();    break;
        case UI_BLAST:   ui_draw_blast();   break;
        case UI_GATE_STATUS: ui_draw_gate_status(); break;
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
    /* A screen change is input, including the ones nobody pressed for: a card
       arriving and a recording taking the panel both land here, and both put
       something on a screen that has to be lit to be worth putting it on. */
    ui_note_input();
    ui_last_draw = millis();
    ui_encoder_reset();

    /* Tell the ring what is on the panel, the same way loop() tells it which
       job is on the bar: the UI hands over a fact, and skills/ring.cpp decides
       on its own what to do with it.
       This is the only place a screen changes, which is exactly why the telling
       is here. Every way out of a card passes through it — the click that
       acknowledges the message, the user key, the idle timeout, the message
       expiring underneath the card — so the ring cannot be left naming a
       message nobody is looking at by a new exit path somebody forgets to
       update. The lookup can fail on a card whose message went away between the
       decision to show it and this line, and that is a card about to be closed
       on the next pass; the ring says nothing for it in the meantime.
       No host test covers these six lines and none can: they are a call into a
       driver from a function that draws. What IS covered is everything they
       feed — progress_ring_phase() by tools/test_progress.sh and the exclusion
       of the card's own message by tools/test_notify_options.sh — so the part
       left to inspection is that this sits in ui_enter() and nowhere else. */
    if (screen == UI_MSGCARD) {
        NotifyView v;
        if (notify_view_by_id(ui_msg_id, v, NULL, NULL)) ring_card_set(v.id, v.level);
        else ring_card_clear();
    } else {
        ring_card_clear();
    }

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

/* Watch a gate job the menu just asked for. `job` is 0 when nothing started,
   which is either a job already in flight — watched rather than restarted —
   or a transmitter that is not there, which gets its own row instead of
   silence. The kind and button are remembered for the "starting" window in
   which the staged job still shows the previous counters. */
static void ui_enter_gate(uint16_t job, uint8_t kind, uint8_t button, int back_sel) {
    ui_gate_ok = (job != 0) || gate_busy();
    ui_gate_kind = kind;
    ui_gate_button = button;
    ui_gate_back_sel = back_sel;
    ui_gate_done_at = 0;
    if (job != 0) {
        event_add("gate: job %u started from the menu", (unsigned)job);
        /* The one-shot channel: half a second of "something is happening right
           now", the same overlay an arrival fires. The steady state is this
           screen; the ring has no steady TX colour of its own, because the
           steady channels are taken — the card names a message and the arc is
           arbitrated by progress.cpp out of main.cpp, which would clear a
           foreign value on the next pass. */
        ring_fire(RING_FX_COMET, NOTIFY_WARN, millis());
    } else {
        event_add("gate: menu start refused (%s)", gate_ready ? "busy" : "no radio");
    }
    ui_enter(UI_GATE_STATUS);
}

/* Open one message. The card holds the id rather than the row, so that a
   message arriving or expiring while it is up cannot silently swap it for a
   different one. */
static void ui_enter_card(uint32_t id) {
    NotifyView v;

    ui_msg_id = id;
    ui_card_fade = 0;
    /* Where the knob starts is ui_chip_start()'s decision and only its, so that
       the host suite can hold it to account: an already-answered message opens
       on its own answer, so the click that leaves the card cannot quietly
       change it, and an unanswered one opens on nothing, so that same click
       stays the dismissal it has always been. notify_view_by_id() is the lookup
       ui_draw_card() draws with and stops at the same first id match, so the
       chip the knob starts on is the chip drawn starred. No entry to read is
       nothing to answer, which is the same nothing. */
    ui_chip_sel = notify_view_by_id(id, v, NULL, NULL)
                      ? ui_chip_start(v.chosen, v.opt_count)
                      : -1;
    /* A new card is a new body: drop the wrap, the scroll position and the
       thumb together, in the one place the card's identity changes. The fade
       that follows repaints the whole card, which is what erases the previous
       thumb from the panel — hence the drawn state going back to "nothing
       drawn" rather than to a position. */
    ui_card_wrapped = 0;
    ui_card_first = 0;
    ui_card_first_drawn = -1;
    ui_card_thumb_y = -1;
    ui_card_thumb_h = -1;
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

    if (ui_screen == UI_GATE) {
        /* Pair fires the enrolment sequence for button 1 (the /gate/pair
           default); a button fires its own block. Both go through the same
           staging path POST /gate/* uses — the job runs on the loop task —
           and land on the status screen instead of answering with silence. */
        if (item == UI_GATE_PAIR) {
            ui_enter_gate(gate_start_code(GATE_JOB_PAIR, 1), GATE_JOB_PAIR, 1, item);
        } else if (item >= UI_GATE_B1 && item <= UI_GATE_B4) {
            uint8_t b = (uint8_t)(item - UI_GATE_B1 + 1);
            ui_enter_gate(gate_start_code(GATE_JOB_BUTTON, b), GATE_JOB_BUTTON, b, item);
        } else ui_enter_list(UI_MENU, UI_ITEM_GATE);
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
        case UI_ITEM_QUIET:
            /* The one row that acts without leaving the menu: the answer is the
               row's own label, which now says the other thing. A screen of its
               own would be a screen with one line on it.

               display_force, because ui_draw_list() returns early when the
               selection has not moved — and it has not: what changed is a
               string the cache still holds the old copy of. */
            ring_quiet_toggle();
            display_force = true;
            ui_draw();
            break;
        case UI_ITEM_BACKLIGHT:
            /* Acts without leaving the menu, exactly as Quiet does, and for the
               same reason: the answer is the row's own label, and a screen with
               one line on it would be a worse way to say it. The level is only
               recorded here — the pulse train that carries it out belongs to
               backlight_poll() on the next pass, so the redraw below happens
               while the panel is still at the old brightness and the new one
               arrives a few milliseconds later. That is the right order:
               brightness changing after the row that announced it reads as the
               row causing it. */
            backlight_menu_click();
            display_force = true;
            ui_draw();
            break;
        case UI_ITEM_DIM:
            /* The third row that acts in place, for the third time the same
               reason: two states, no arguments, and the answer is the row's
               own label — which is why display_force is needed here as it is
               for Quiet, since ui_draw_list() returns early on a selection
               that has not moved. The brightness itself does not change on
               this pass: the click has just stamped ui_last_input, so the
               policy is handing the level back either way, and the switch
               only decides what happens once the dim threshold passes. */
            backlight_idle_toggle();
            display_force = true;
            ui_draw();
            break;
        case UI_ITEM_TVBGONE:
            ui_enter_list(UI_TVMENU, UI_TV_ALL);
            break;
        case UI_ITEM_GATE:
            ui_enter_list(UI_GATE, UI_GATE_PAIR);
            break;
        case UI_ITEM_AP:
            /* The only route in on the device, and time-boxed: a session
               nobody reprovisions closes itself. It used to share the job with
               a three-second hold of the user key, which is now what walks the
               message stack — but this row was always the reliable one, and
               ap_start() has one more caller in wifi_setup(), which raises the
               AP at boot when the stored credentials do not connect. */
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
        case UI_GATE:    ui_enter_list(UI_MENU, UI_ITEM_GATE);    break;
        case UI_GATE_STATUS: ui_enter_list(UI_GATE, ui_gate_back_sel); break;
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
    /* A boot is input: the device came up because somebody pressed something or
       plugged it in, so the idle clock starts here rather than at zero. */
    ui_note_input();
}

/*
 * The two screens that must not be taken away from whoever is watching them.
 *
 * They are predicates rather than two locals inside ui_poll() because there is
 * a second consumer: the backlight's idle policy exempts exactly these, and it
 * has to exempt the SAME ones. A blast dimmed to nothing halfway through, or a
 * level meter that fades while a finger is still on the key, would be the same
 * defect the screen timeout was already written to avoid — and two copies of
 * this pair, thirty lines apart, is how they would come to disagree.
 *
 * A blast counts only while it is on the panel: one started over HTTP while a
 * menu is up is not live output the person in front of the device is watching,
 * and there is nothing on screen for the exemption to protect.
 */
static bool ui_watching_blast() { return ui_screen == UI_BLAST && ir_busy(); }

/* The recording screen is exempt outright rather than only while it records: a
   hold can outlast fifteen seconds of "no input" — holding a key is not input
   to the click state machine — and its own exit is deterministic, so neither
   the timeout nor the dimming has anything to add and both could only fire
   mid-sentence. */
static bool ui_watching_rec() { return ui_screen == UI_REC; }

/*
 * The progress bar, which is live output too — but only to the backlight, and
 * that asymmetry is the whole reason it is a third predicate rather than a
 * third case in the pair above.
 *
 * The screen timeout may fire under a bar and cost nothing: what it does is
 * return to the clock face, and the clock face is where the bar is drawn. Going
 * dark is not the same act. A job pushed with POST /progress — a backup script,
 * a long build, anything holding a percentage up on somebody's desk — has
 * nothing to do with the knob, so nothing stamps ui_last_input for it and the
 * panel it is drawn on would otherwise go out two minutes in.
 *
 * On the clock face only, for the same reason a blast counts only while it is
 * on the panel: a bar nobody can see is not output being protected. And bounded
 * without any help from here, because a job carries a deadline of its own — the
 * bar clears itself and the panel goes back to blanking on schedule.
 */
static bool ui_watching_progress() {
    return ui_screen == UI_CLOCK && progress_current(NULL);
}

/*
 * Hand the backlight policy the idle clock. Called from loop(), once a pass,
 * immediately above backlight_poll().
 *
 * NOT from inside ui_poll(), and the reason is worth stating because the
 * opposite looks obviously right: ui_poll() returns early from most paths, and
 * from the clock face it returns UNCONDITIONALLY — `case UI_CLOCK` has nothing
 * to time out to, so it answers the click and leaves. The clock face is where
 * this device spends its life, so a brightness decision made at the bottom of
 * ui_poll() would be a brightness decision that almost never runs. What IS
 * shared with the screen timeout is everything that matters: the one timestamp,
 * and the two exemptions it also has.
 *
 * That unconditional return is also why the thresholds mean exactly what they
 * say FROM THE CLOCK FACE. Elsewhere they are later, and by a knowable amount:
 * ui_enter() stamps the timestamp, so the automatic return to the clock at
 * UI_IDLE_MS restarts the dim clock once. Input on a menu therefore dims at
 * UI_IDLE_MS + BL_IDLE_DIM_MS and blanks at UI_IDLE_MS + BL_IDLE_OFF_MS — one
 * restart and not a repeating one, because from the clock face there is no
 * further timeout to stamp anything. Worth knowing before timing the device
 * with a stopwatch and concluding the thresholds are wrong.
 *
 * The subtraction is here, in the one place that owns ui_last_input, and it is
 * the unsigned form that survives the 49-day wrap of millis(). bl_idle_level()
 * is handed the elapsed time and no clocks at all.
 */
static void ui_backlight_idle() {
    /* The message card needs no exemption of its own, and this is why. A card
       is returned to the clock face after UI_IDLE_MS whether it was read or
       not, so with the dim threshold strictly longer than that, a card is
       never on the panel when the first dimming happens — there is no moment
       at which a notification could fade out from under somebody reading it.
       That is arithmetic between two constants in two files, so it is checked
       here rather than trusted: shortening either one below the other stops
       the build instead of quietly costing somebody a message. */
    static_assert(BL_IDLE_DIM_MS > UI_IDLE_MS,
                  "the backlight must not dim while a message card can still "
                  "be on the panel: BL_IDLE_DIM_MS must exceed UI_IDLE_MS");
    /* The fourth fact is not about a screen at all: an unread critical never
       expires, and blanking the panel would take away the breathing hairline
       that is the only sign of one left once the ring's night window has
       silenced the ring. skills/backlight.cpp turns that into a floor, not a
       wake-up. */
    BacklightPanel panel;
    panel.blasting = ui_watching_blast();
    panel.recording = ui_watching_rec();
    panel.on_bar = ui_watching_progress();
    panel.crit_unread = notify_crit_unread();
    backlight_idle(millis() - ui_last_input, panel);
}

/* A press that began on a panel the idle policy had blanked. Its whole job was
   to bring the light back, and the release that ends it must not also be read
   as a click — but by then the panel is lit, and lit by this very press, so
   there is nothing left at the release to tell the two apart. Hence a mark, set
   where the press starts and spent by the release it was set for. See the wake
   in ui_poll(), which is the only reader and the only writer. */
static bool ui_wake_press = false;

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
    /*
     * The user key's long hold walks the stack of messages behind the card,
     * which is the job the knob used to do before the knob became the way to
     * read a body longer than the card. It is a gesture on one screen only:
     * everywhere else a press of this key is "one level up" however long it
     * lasted, which is what it has always been, and confining the new meaning
     * to the card is what keeps a slow press on a menu from doing nothing at
     * all.
     *
     * The threshold is the one mic.cpp already uses for the encoder key's own
     * hold, so the device has one idea of how long a hold is rather than two.
     * It is not the three seconds the retired AP gesture used: that was long on
     * purpose, because it authorised provisioning, and walking a stack does not.
     *
     * `back` discards the release that ends the hold, the way it discarded the
     * one that ended the AP gesture — a hold produces a click on release like
     * any other press, and without this a long hold would both walk the stack
     * and leave the card.
     *
     * What `back` no longer does is throw away a slow press. It used to be
     * `click && held_ms < AP_KEY_HOLD_MS`, which meant a press of three seconds
     * or more did nothing at all on ANY screen: the filter was there to stop the
     * AP gesture's release from stepping up a level as well, and with the
     * gesture gone it protected nothing. So the device-wide consequence of that
     * one line is that a slow press is now one level up everywhere, which is
     * what this key means and what somebody leaning on it expects. The card is
     * the only screen that reads the length of a press at all, and it reads it
     * through `card_hold`.
     */
    bool card_hold = ui_user_key.click && ui_screen == UI_MSGCARD &&
                     ui_user_key.held_ms >= MSG_STACK_HOLD_MS;
    bool back = ui_user_key.click && !card_hold;

    /*
     * The release of a press whose card went away underneath it. It is not a
     * step up and it is not a walk of the stack: it is the end of a gesture
     * whose subject no longer exists. See ui_drop_user_click.
     *
     * First of the three rules that follow, and the order between them is the
     * whole of why they agree. This is the only one that decides what a press
     * MEANT, so it runs before anything reads that meaning. It also has to run
     * on every pass that carries the release, whatever else is true of that
     * pass: the flag is cleared by the very release it was set for, and a pass
     * that skipped it would leave it standing over somebody else's click.
     */
    if (ui_user_key.click && ui_drop_user_click) {
        ui_drop_user_click = false;
        card_hold = false;
        back = false;
    }

    /*
     * A key that is DOWN is input, and nothing above can see it: ui_button_poll()
     * reports a click on RELEASE only, so a press that has not been let go of
     * yet produces no steps, no click and no back at all. Three decisions turn
     * on that — whether this pass belongs to somebody already touching the
     * device, whether the device has been idle, and whether a panel that is lit
     * was lit by the press still being held — and the first two were wrong for
     * the whole length of every hold without it.
     */
    bool key_down = ui_user_key.level || ui_enc_key.level;

    /*
     * Any click of either key, whatever it was read as — including the one
     * discarded just above, which is still a hand on the device — and a key
     * still down, which is a hand on it too.
     *
     * Second, and deliberately above the wake rather than below it: a press
     * that does nothing but bring the light back is still somebody standing at
     * the device, and a wake that did not restart the idle clock would dim
     * again under the hand it had just answered. ui_note_input() is the one
     * writer of the timestamp, and this condition is the whole of what
     * ui_poll() calls input.
     */
    if (steps != 0 || click || ui_user_key.click || key_down) ui_note_input();

    /*
     * Last: a press on a panel the policy has blanked wakes it and does nothing
     * else.
     *
     * Before there was a dark state every press was deliberate, because there
     * was always something on the screen to press AT. Now the first press is
     * usually somebody wanting to see the time — and on the clock face a click
     * opens the menu, so without this they get the menu instead. That is how
     * every phone and every watch behaves and the opposite is a surprise every
     * single time.
     *
     * Only while it was BLANK. A press on a merely dimmed panel is a press on a
     * screen its owner can read, and swallowing that would be taking away a
     * click they meant. backlight_blanked() answers the narrow question — dark
     * because of the policy, which a press undoes — and not "dark", which on a
     * device somebody set to level 0 by hand a press does not.
     *
     * The recording gesture is untouched and must stay so: a hold is not a
     * glance. mic_key_poll() reads it off the pin level from loop() and never
     * through the click state machine, and the MIC_HOLD_MS guard above already
     * keeps its release out of `click` — so a hold on the encoder still starts
     * a take from a dark panel, and nothing here decides anything about it.
     *
     * ui_wake_press is what the key-down rule above costs, and it is not
     * optional. A key that is DOWN now stamps input, so the panel is already
     * lit again by the time the release that carries the click arrives:
     * backlight_blanked() is false on the one pass that has something to
     * swallow, and every press would reach the screen behind the dark exactly
     * as if none of this were here. So the press is marked where it starts, on
     * the pass the panel was still blank, and the release is read against the
     * mark rather than against a panel it lit itself. A detent needs no mark:
     * the knob reports a turn on the pass it happens, while the panel is still
     * dark. Nothing else can arrive with the panel blank AND a key down —
     * a held key stamps every pass, so a panel cannot blank underneath one.
     *
     * card_hold is discarded with the other two and cannot currently arise: the
     * card has no exemption from the screen timeout, which fires at UI_IDLE_MS
     * against a blank at BL_IDLE_OFF_MS, so a blank panel is a panel that went
     * back to the clock face long ago. It is written down because it is one of
     * the three things a press is read as here, and a rule that covers two of
     * them is a rule waiting on somebody giving the card an exemption.
     */
    bool blanked = backlight_blanked();
    if (blanked && key_down) ui_wake_press = true;

    if (blanked && steps != 0) steps = 0;
    if (ui_wake_press) {
        click = false;
        card_hold = false;
        back = false;
    }
    /* Spent: either the release it was set for has just been discarded, or the
       press ended without ever producing one. */
    if (!key_down) ui_wake_press = false;

    /* The knob's own feedback on the ring, which outranks whatever the ring was
       saying by itself: a hand is on the control. The screen and the ring get
       the same detents, and neither is told what the other did with them.
       Below the wake, deliberately: the detent that brings the panel back is
       spent on bringing it back, and lighting an arc for a turn that moved
       nothing would be the same surprise in a different colour. */
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
     * timer happens to land. And it yields to a hand already on the device: a
     * pass carrying input is that user's pass.
     *
     * A key merely still DOWN counts as that input, which is why `key_down` is
     * here and not only the clicks. A press begun on the clock — where a press
     * of the user key has always meant nothing — carries no click until it is
     * released, so without this the card would open UNDER a press that started
     * before it existed, and the release would then be answering a card the
     * presser never saw: at MSG_STACK_HOLD_MS it walks straight past the
     * arrival, leaving the message unread.
     *
     * That does defer the flag rather than consume it, for as long as the key
     * is held. It is the one way the paragraph above is not quite exact, and
     * the bound is the length of a press and not the idle timer: the pass after
     * the release picks the arrival up, on whatever screen that release left.
     *
     * `card_hold` is deliberately not named here. It can only be true on the
     * card and this branch only fires on the clock, so listing it would read as
     * a case that can arise; `key_down` is what covers the press it is made of.
     */
    if (steps == 0 && !click && !back && !key_down) {
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
        case UI_GATE:
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
            if (idx < 0) {
                /* A hold that is still down has just lost what it was a hold
                   ON, and its release is still to come. Mark it so that the
                   list this drops to does not read that release as a second
                   step up on top of the one being taken here. */
                if (ui_user_key.level) ui_drop_user_click = true;
                ui_enter_list(UI_MSGLIST, 0);
                return;
            }

            /* What the message carries is what decides what the knob does, and
               it is read on the pass that carries the input rather than kept
               across passes: a count cached at entry is a count that can no
               longer be checked against the entry being answered.
               notify_view_by_id() is the lookup ui_draw_card() draws with and
               stops at the same first id match, so the row on screen and the
               row being answered cannot be different rows. */
            NotifyView v;
            uint8_t opts = 0;
            bool have = (click || card_hold || steps != 0) &&
                        notify_view_by_id(ui_msg_id, v, NULL, NULL);
            if (have) opts = v.opt_count;

            /* A long press moves to the next card in the stack. It wraps, and
               the text below does not: this one is browsing, which is what a
               ring of cards is for, and the body is reading, which has a first
               line and a last one.
               It is answered BEFORE the click, because the two keys can be let
               go on the same pass and only one of them can be obeyed: this is
               the deliberate one, seven hundred milliseconds of it, and the
               click is what a hand resting on the device produces by accident. */
            if (card_hold) {
                int count = notify_count();
                if (count > 1) {
                    int n = (idx + 1) % count;
                    /* Its own name: `v` above is the card on screen, this is
                       the neighbour being stepped to, and they are two
                       different messages. */
                    NotifyView next;
                    /* Re-fade on the way in, so moving through the stack reads
                       as one card replacing another rather than as text
                       changing inside a frame that never moved. */
                    if (notify_view(n, next)) { ui_sel = n; ui_enter_card(next.id); return; }
                }
                /* Nowhere to go on a stack of one, and going nowhere is better
                   than re-opening the same card: that would drop the reader
                   back at the first line of what they were reading. Falling
                   through rather than breaking, because a detent that arrived on
                   the same pass is still a detent, and this hold did nothing
                   with it. */
            }

            /* A click is "I have seen this" — or, when ui_chip_answers() says
               this one is an answer, the answer, which is stronger:
               notify_choose_id() marks it read as part of recording the choice.
               Everything else falls through to the plain acknowledgement,
               including a click on a card that asks something nobody has
               scrolled far enough to reach. ui_chip_clamp() is what keeps a
               selection restored from a `chosen` the store no longer has
               options for from being handed over as an answer. A short press
               of the user key is the same plain step up it is on every other
               screen and leaves the message unread, and its question
               unanswered, on purpose. */
            if (click) {
                if (ui_chip_answers(ui_chip_sel, opts)) {
                    notify_choose_id(ui_msg_id,
                                     (uint8_t)ui_chip_clamp(ui_chip_sel, opts));
                } else {
                    notify_ack_id(ui_msg_id);
                }
                ui_enter_list(UI_MSGLIST, idx);
                return;
            }
            if (back) { ui_enter_list(UI_MSGLIST, idx); return; }

            if (steps != 0 && have) {
                /* The wrap is normally already done — entering the card drew
                   it — but the position the detent moves is read from it, so
                   the input asks for it by name rather than assuming the draw
                   ran first. */
                ui_card_wrap(v);
                UiCardPos pos = { ui_card_first, ui_chip_sel };
                pos = ui_card_step(pos, steps, ui_card_max_first(), (int)opts);
                if (pos.first != ui_card_first || pos.chip != ui_chip_sel) {
                    ui_card_first = pos.first;
                    ui_chip_sel = pos.chip;
                    ui_draw();
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

        case UI_GATE_STATUS: {
            /* A click or a detent leaves for the gate list at once; the job
               keeps running underneath. Otherwise the screen stays while the
               job is on the air, then holds how it ended for a moment — the
               blast-screen shape, minus the stop: nothing here aborts a TX. */
            if (click || back || steps != 0) {
                ui_enter_list(UI_GATE, ui_gate_back_sel);
                return;
            }
            if (gate_busy()) {
                ui_gate_done_at = 0;
            } else if (ui_gate_done_at == 0) {
                ui_gate_done_at = millis();
            } else if (millis() - ui_gate_done_at >= UI_RESULT_MS) {
                ui_enter_list(UI_GATE, ui_gate_back_sel);
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

    /* Live output must not be timed out from under the user; everything else
       falls back to the clock. Both exemptions are stated once, above, and the
       backlight's idle policy reads the same two.

       The card is the other screen where holding a key is normal, and it gets
       no exemption on purpose — it is where an unread message sits, and a card
       that never times out is a card left on the panel of a device nobody is
       standing at. What it needed instead was for a hold to count as input,
       which is what `key_down` above does. Before that, a hold outlasting
       UI_IDLE_MS let this fire mid-gesture: the card went back to the clock,
       and the release then arrived there as a plain `back` on a screen that
       ignores it, so the whole gesture was swallowed. */
    if (!ui_watching_blast() && !ui_watching_rec() &&
        millis() - ui_last_input >= UI_IDLE_MS) {
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
