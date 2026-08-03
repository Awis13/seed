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
 *      CLOCK  --click-->  MENU  --+-->  TVMENU  --+-->  BLAST
 *        ^                        |               |
 *        |                        |               +-->  BRAND  -->  BLAST
 *        |                        +-->  AP
 *        |                        +-->  INFO
 *        |
 *        +--15s idle on any screen but a running blast
 *
 * Back leaves a screen for the one above it — BRAND to TVMENU, TVMENU to MENU,
 * MENU to CLOCK — whether it comes from the Back item or the user key, so the
 * knob alone is enough to get anywhere and out again.
 *
 * CLOCK is the home screen and is drawn exactly as before by display_tick();
 * this file does not touch it beyond handing control back. Every other screen
 * returns to CLOCK after UI_IDLE_MS without input, so a device left in a menu
 * on the shelf goes back to being a clock. A running blast is the one screen
 * that does not time out — it is live output, and its click means "stop".
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
    UI_TVMENU,
    UI_BRAND,
    UI_BLAST,
    UI_AP,
    UI_INFO
};

static const char *ui_titles[] = {
    "", "MENU", "TV-B-GONE", "BY BRAND", "TV-B-GONE", "SETUP AP", "INFO"
};

/* Top level. The marker on a selected row is also ">", so a submenu is spelled
   out with a trailing one rather than by a different marker. */
enum {
    UI_ITEM_TVBGONE = 0,
    UI_ITEM_AP,
    UI_ITEM_INFO,
    UI_ITEM_BACK,
    UI_ITEM_COUNT
};

static const char *ui_items[UI_ITEM_COUNT] = {
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
/* False when the menu could not start a blast and none was already running. */
static bool ui_blast_ok = true;
/* The list the blast was started from, so that finishing goes back to it. */
static uint8_t ui_blast_back = UI_TVMENU;
static int ui_blast_back_sel = 0;

#define UI_MENU_Y0   30
#define UI_MENU_DY   32
/* Rows of font 4 at that pitch that fit above the bottom of a 170px panel. */
#define UI_MENU_ROWS 4
#define UI_ROW_COUNT 8

/* One cache line per drawn row, shared by every screen in this file: only one
   of them is on screen at a time, and entering any screen wipes the panel and
   raises display_force, which makes draw_field ignore whatever the previous
   screen left in here. */
static char ui_row[UI_ROW_COUNT][40];

static void ui_draw_row(int i, const char *text, int32_t y, uint8_t font,
                        uint16_t color) {
    draw_field(ui_row[i], sizeof(ui_row[i]), text, 12, y, font, color,
               TL_DATUM, 296);
}

/* The three list screens differ only in what they list, so they share one
   renderer and one set of input rules. Both accessors read the live tables —
   the by-brand list is ir_codes[] itself, not a copy of the names. */
static int ui_list_count() {
    switch (ui_screen) {
        case UI_MENU:   return UI_ITEM_COUNT;
        case UI_TVMENU: return UI_TV_COUNT;
        case UI_BRAND:  return ir_code_count + 1;  /* the codes, then Back */
    }
    return 0;
}

static const char *ui_list_label(int i) {
    switch (ui_screen) {
        case UI_MENU:   return ui_items[i];
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

    if (ui_sel < ui_first) ui_first = ui_sel;
    if (ui_sel >= ui_first + UI_MENU_ROWS) ui_first = ui_sel - UI_MENU_ROWS + 1;
    if (ui_first > count - UI_MENU_ROWS) ui_first = count - UI_MENU_ROWS;
    if (ui_first < 0) ui_first = 0;

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
       header where there is room for it. */
    if (count > UI_MENU_ROWS) {
        char pos[16];
        snprintf(pos, sizeof(pos), "%d/%d", ui_sel + 1, count);
        draw_field(ui_row[UI_ROW_COUNT - 1], sizeof(ui_row[0]), pos,
                   tft.width() - 8, HDR_Y, 2, COL_DIM, TR_DATUM, 60);
    }

    ui_sel_drawn = ui_sel;
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

/* Screens that are a list of things to pick, and so only redraw on input. */
static bool ui_is_list(uint8_t screen) {
    return screen == UI_MENU || screen == UI_TVMENU || screen == UI_BRAND;
}

static void ui_draw() {
    switch (ui_screen) {
        case UI_MENU:
        case UI_TVMENU:
        case UI_BRAND: ui_draw_list();  break;
        case UI_BLAST: ui_draw_blast(); break;
        case UI_AP:    ui_draw_ap();    break;
        case UI_INFO:  ui_draw_info();  break;
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
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(ui_titles[screen], 8, HDR_Y, 2);
    tft.drawFastHLine(8, 20, tft.width() - 16, COL_RULE);

    display_force = true;  /* the wipe took every cached field with it */
    ui_sel_drawn = -1;
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

static void ui_activate(int item) {
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
        case UI_BRAND:  ui_enter_list(UI_TVMENU, UI_TV_BRAND);   break;
        case UI_TVMENU: ui_enter_list(UI_MENU, UI_ITEM_TVBGONE); break;
        default:        ui_enter(UI_CLOCK);                      break;
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

    bool click = ui_enc_key.click;
    /* The user key's long hold belongs to the AP gesture in ap_key_poll(), so
       only a short press counts as "back" here. */
    bool back = ui_user_key.click && ui_user_key.held_ms < AP_KEY_HOLD_MS;

    if (steps != 0 || click || back) ui_last_input = millis();

    switch (ui_screen) {
        case UI_CLOCK:
            if (click) ui_enter_list(UI_MENU, 0);
            return;

        case UI_MENU:
        case UI_TVMENU:
        case UI_BRAND: {
            if (back) { ui_back(); return; }
            if (steps != 0) {
                int count = ui_list_count();
                ui_sel += steps;
                /* Wrap rather than clamp: a knob with no end stops, so running
                   off one end should arrive at the other. */
                ui_sel %= count;
                if (ui_sel < 0) ui_sel += count;
                ui_draw_list();
            }
            if (click) { ui_activate(ui_sel); return; }
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
       user; everything else falls back to the clock. */
    bool watching_blast = (ui_screen == UI_BLAST && ir_busy());
    if (!watching_blast && millis() - ui_last_input >= UI_IDLE_MS) {
        ui_enter(UI_CLOCK);
        return;
    }

    if (millis() - ui_last_draw >= UI_TICK_MS) {
        ui_last_draw = millis();
        if (!ui_is_list(ui_screen)) ui_draw();
    }
}
