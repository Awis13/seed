/*
 * backlight.cpp — the panel's backlight as a level rather than a switch.
 *
 * The backlight is the single largest consumer on this board. Measured on the
 * device: 150 mA with it lit, 65 mA with it dark. 85 mA of the total draw is
 * this one part, which is more than everything else together — the radio, the
 * panel's own logic, the ring, the CPU. Anything that wants to make the battery
 * last has to come here first, and until now there was nothing here to come to:
 * the pin was driven HIGH once at boot and never touched again.
 *
 * This file makes the level a number. What USES that number — going dim when
 * nobody is looking at the device — is not here, deliberately: this is the
 * primitive and the two ways a person sets it by hand.
 *
 *
 * The part, and why the pin is not a switch
 * -----------------------------------------
 * GPIO21 does not gate a transistor. It is the EN pin of an AW9364, a 4-channel
 * charge-pump LED driver that takes its brightness from a ONE-WIRE PULSE COUNT
 * on that same enable line. From the datasheet (Awinic AW9364, Feb 2018 V2.3):
 *
 *   - EN above 1.5V enables the device; it has an internal 150k pull-down, so
 *     an undriven pin means dark. Startup time T_ON is 20us, and while the
 *     table calls that a typical the body text and Figure 7 both present it as
 *     a floor: "the ready time is recommended to be greater than 20us".
 *   - A 4-bit DAC gives 16 current steps, and Table 1 is headed "EN RISE Edge
 *     Number": each RISING edge on EN steps the current DOWN one. The first
 *     edge — the one that enables the part — is step 1 and step 1 is MAXIMUM.
 *   - The ladder is LINEAR IN CURRENT and the steps are 1.25mA apart: 20mA at
 *     edge 1 down to 1.25mA at edge 16, on each of four channels. Full scale is
 *     therefore 80mA of LED current, which is where the 85mA measured at the
 *     battery comes from and is the one number here that was checked twice, in
 *     two directions.
 *   - The counter wraps. Figure 7 draws the pulses as 1..16 and then 1 again,
 *     with the current going 16/16 down to 1/16 and back to 16/16. Seventeen
 *     edges is one edge.
 *   - Timing: T_HI at least 0.5us, T_LO at least 0.5us AND AT MOST 500us.
 *   - Holding EN low shuts the part down. The table gives the shutdown delay as
 *     800us minimum and 2500us maximum, which is a spread on a delay and not a
 *     window: below 800us it is guaranteed NOT to shut down, above 2500us it is
 *     guaranteed to. Figure 7 annotates the safe threshold as 2.5ms. The
 *     datasheet's own graph on page 7 is the reason not to lean on the 800us:
 *     shutdown time falls as the supply falls, and at the bottom of the supply
 *     range the curve sits below that 800us. It is a plotted curve with no
 *     printed values, so the direction and the band are what it supports; a
 *     figure read off it would be a figure this comment invented.
 *
 * The consequence that matters for the pulse train is the T_LO maximum. A train
 * interrupted for more than half a millisecond does not merely land on the
 * wrong step — it risks the shutdown, after which the next rising edge brings
 * the part back at FULL. So the worst this file's failure mode can be is a
 * flash of bright, never a dark screen, which is worth knowing before deciding
 * how hard to work at not being interrupted.
 *
 *
 * What the other firmwares for this board do, checked rather than repeated
 * -----------------------------------------------------------------------
 * Three exist and none of them dims this panel correctly. This was checked
 * against current source, because "everyone else does it wrong" is exactly the
 * kind of claim that turns out to be a stale rumour:
 *
 *   - Bruce drives it with PWM. boards/lilygo-t-embed-cc1101/interface.cpp
 *     calls analogWrite(TFT_BL, ...) and nothing else; on this core that lazily
 *     attaches LEDC at 1kHz, 8-bit. Every LEDC rising edge is a dimming
 *     command, so the counter runs in circles sixty times a second and the
 *     brightness is a rotating average rather than a setting. It is worse than
 *     merely ineffective at the bottom of the range: at duty 64 the low time is
 *     about 750us, past the 500us maximum and inside the indeterminate band.
 *   - LilyGO's own examples/factory/factory.cpp is
 *     `digitalWrite(DISPLAY_BL, value == 0 ? LOW : HIGH)` — so
 *     setBacklightBrightness(1) really is byte-identical to (255), and their
 *     own idle-dim path is a visual no-op. Their pin map documents the AW9364;
 *     the code never speaks to it.
 *   - The earlier firmware written for this same device got it right by
 *     refusing: it drives the pin HIGH or LOW only, and says in a comment that
 *     a level needs the pulse protocol implemented deliberately. This file is
 *     that deliberate implementation.
 *
 * Two implementations ARE correct and they agree with each other, which is what
 * makes the protocol above worth trusting: lewisxhe's SensorLib
 * (src/actuator/AW9364LedDriver.hpp) and a Rockchip BSP driver that ships as
 * drivers/video/backlight/aw9364_bl.c in several RK30xx kernel trees. Both
 * write LOW then HIGH, so both step on the rising edge; both keep the current
 * level and compute a RELATIVE pulse count modulo 16 rather than resetting to
 * full; both take the wrap for granted. Neither is in mainline Linux, and it is
 * worth saying so plainly — that file is a vendor driver, not a kernel one.
 *
 * Where this file departs from both: they rely on a GPIO call being slow enough
 * to satisfy T_HI and T_LO on its own. SensorLib contains no delay of any kind.
 * That works on a part whose digitalWrite costs microseconds and does not work
 * here, so the timing below is explicit.
 *
 *
 * How the pulses are sent, and the two routes not taken
 * ----------------------------------------------------
 * A short critical section around direct GPIO register writes.
 *
 * NOT the RMT peripheral, which would otherwise be the nicest answer — it can
 * clock out an exact pulse count in hardware with no CPU involvement at all.
 * There is no channel to do it on. This chip has four RMT TX candidates
 * (SOC_RMT_TX_CANDIDATES_PER_GROUP), the transmitter takes two memory blocks
 * and the ring takes two, and skills/ring.cpp already records the consequence
 * at its head: the TX side is fully committed and a third consumer fails at
 * rmtInit(). That was verified here rather than assumed. The core's RGB_BUILTIN
 * path would take a channel too, but only if something calls rgbLedWrite() or
 * writes that virtual pin, and nothing in this firmware does.
 *
 * NOT LEDC, for the reason Bruce demonstrates above: there is no pulse-count
 * API in it, and a periodic waveform on this pin is not dimming.
 *
 * NOT digitalWrite() either, even inside the critical section. On this core it
 * is a flash-resident function that looks the pin up in the peripheral manager
 * on every call, which is several hundred nanoseconds of overhead per edge and
 * cannot hold a 1us cell with any confidence. REG_WRITE to the set/clear
 * registers is a single store. pinMode() is still called once at init, so the
 * peripheral manager knows the pin is ours and the diagnostic endpoints in
 * skills/gpio.cpp keep telling the truth about it.
 *
 * On the critical section itself: it is cheap and it is not doing what one
 * might assume. A change costs at most fifteen pulses, and a pulse is two
 * register stores and two delayMicroseconds(1) — that call busy-waits on the
 * system timer and so overshoots slightly, which makes a cell a few
 * microseconds rather than exactly two. Some tens of microseconds for the whole
 * train, then: comfortably inside one FreeRTOS tick and three orders of
 * magnitude short of the interrupt watchdog, with the exact figure being the
 * kind of thing a scope would settle and this comment should not pretend to.
 *
 * What the critical section buys is that no interrupt on THIS core stretches a
 * T_LO past 500us; it is the same guard, for the same stated reason, that the
 * Rockchip driver puts a spinlock around ("the wave should not be intterupted",
 * their spelling). What it does NOT do is mask the WiFi MAC, which runs on core
 * 0 while Arduino's loop() runs on core 1 — so this is not the WiFi-latency
 * trade it looks like. delayMicroseconds() is safe to call from inside it: it
 * is IRAM-resident and spins on esp_timer_get_time(), with nothing in it that
 * yields or takes a lock.
 *
 * Everything that pulses runs on the loop task, from backlight_poll(). The
 * endpoint only records a wanted level, exactly as the ring's settings do,
 * because a pulse train started from the web-server task would race the one
 * loop() might be in the middle of.
 *
 *
 * Sixteen steps, of which four are worth offering
 * ----------------------------------------------
 * The ladder is linear in CURRENT and the eye is not. Steps 16 through 9 span
 * 20mA to 10mA, one stop in total, and are close to indistinguishable; nearly
 * all of the visible range is in the bottom third. Presenting sixteen equal
 * choices would be presenting eight that look the same.
 *
 * The palette makes it narrower still. COL_DIM sits at about 20% of white's
 * relative luminance and is the colour of everything on the clock face that is
 * not a headline, so four fifths of the contrast range is already spent before
 * the backlight is touched at all. A backlight at a quarter current puts that
 * text at roughly 5% of full white, which is about as far as it can be taken
 * before the device stops being readable rather than becoming dim.
 *
 * Hence four presets, spaced roughly evenly in RATIO rather than in step
 * number, with a floor set by readability and not by the hardware. They are a
 * starting point to be judged by looking at the screen, which is why they are
 * one table in one place: changing them is editing four numbers, and nothing
 * below reads a level from anywhere else. The full 1..16, and 0 for off, stay
 * reachable over HTTP so the table can be argued with from data.
 *
 * Off is deliberately not one of them. The menu must not be able to blank the
 * screen somebody is reading the menu on; the endpoint may, because the idle
 * policy that will eventually want it is not a person holding the device.
 */

#include <soc/gpio_reg.h>

/* Datasheet timings, in microseconds. The two cell halves are asked for as 1us
   against a 0.5us minimum: the margin is free and the whole train still costs
   about 30us. T_LO's 500us MAXIMUM is the one that matters and is nowhere near.
   BL_ON_US is the Rockchip driver's 30us rather than the datasheet's 20us
   floor, for the same reason it chose it — a floor wants headroom above it.
   BL_OFF_MS is Figure 7's 2.5ms shutdown threshold, rounded up.

   Outside the sliced region below on purpose: none of them exists on a host,
   and a test that asserted about them would be asserting about nothing. */
#define BL_HI_US        1
#define BL_LO_US        1
#define BL_ON_US        30
#define BL_OFF_MS       3

/* Settings arrive on the web-server task and the flash write is deferred to
   loop(), the one task allowed to spend milliseconds. Same arrangement, and the
   same reason, as the ring's. */
#define BL_CFG_DELAY_MS 1500
#define BL_CFG_FILE     "/backlight.json"
#define BL_CFG_TMP      "/backlight.tmp"

/* --- The part, and the ladder it counts in --- */

/* host-test:begin bl_ladder — sliced out by tools/test_backlight.sh */
#define BL_STEPS        16
#define BL_CHANNELS     4      /* LED1..LED4, all four wired on this board */

/* Microamps per step, per channel. The ladder is 20mA at the top in sixteen
   even 1.25mA decrements, so the current at a level is the level times this. */
#define BL_STEP_UA      1250

/* Brightest, and what a device with no stored setting comes up at. Being wrong
   in this direction is being wrong harmlessly: a panel that is too bright is
   noticed and turned down, while one that comes up too dim to read reads as a
   dead device. */
#define BL_DEFAULT      BL_STEPS

/*
 * A level, as this firmware counts them, is 1..16 with 16 BRIGHTEST — and 0 for
 * off. That is upside down from the datasheet, which numbers the edges 1..16
 * with edge 1 at 20mA, and the inversion is on purpose: every other setting on
 * this device grows with the thing it describes, and a menu where a bigger
 * number is darker is a menu somebody will get wrong at one in the morning.
 * bl_edge_of() is the only place the two numberings meet.
 */
static uint8_t bl_edge_of(uint8_t level) {
    return (uint8_t)(BL_STEPS + 1 - level);
}

/* What one channel draws at a level, in microamps; 0 is off and draws none.
   Straight out of Table 1 once the numbering is turned around: level 16 is edge
   1 is 20mA, level 1 is edge 16 is 1.25mA. */
static uint32_t bl_current_ua(uint8_t level) {
    if (level == 0 || level > BL_STEPS) return 0;
    return (uint32_t)level * BL_STEP_UA;
}

/* All four channels together — the figure the battery actually sees. */
static uint32_t bl_total_ua(uint8_t level) {
    return bl_current_ua(level) * BL_CHANNELS;
}

/*
 * How many rising edges take the part from the level it is at to the level it
 * should be at.
 *
 * The counter is modulo 16 and only ever counts DOWN in brightness, so getting
 * brighter means going round: every step of the way down to the dimmest, then
 * the wrap to full, then back down to what was asked for. That is why this is
 * one expression rather than two cases — the wrap is not an edge case here, it
 * is the mechanism, and a version that special-cased "brighter" would be
 * re-deriving the same number in a second way.
 *
 * Both arguments are 1..16. The answer is 0..15, and 0 exactly when nothing has
 * to move: asking for the level already set sends no pulses at all, which is
 * what keeps a poll that runs every few milliseconds from clocking the part.
 */
static uint8_t bl_pulses(uint8_t from, uint8_t to) {
    return (uint8_t)((BL_STEPS + (int)from - (int)to) % BL_STEPS);
}

/* Is this a level the endpoints may set? 0 is off and is a level; anything
   above the ladder is not. */
static bool bl_level_valid(int level) {
    return level >= 0 && level <= BL_STEPS;
}
/* host-test:end */

/* host-test:begin bl_presets — sliced out by tools/test_backlight.sh */
/*
 * The levels worth offering from the front panel, brightest first.
 *
 * THIS TABLE IS THE WHOLE SETTING. It is four numbers and four words, nothing
 * below reads a level from anywhere else, and it is meant to be edited after
 * looking at the screen rather than defended — see the head of this file for
 * why sixteen equal choices would be a worse offer than four unequal ones.
 *
 * Spaced by ratio rather than by step number, because the ladder is linear in
 * current: 20mA, 13.75mA, 8.75mA and 5mA per channel come to 100%, 69%, 44% and
 * 25%, which is about half a stop apart each and two stops end to end. The
 * floor is 4 and not 1 because of the palette rather than the part — a quarter
 * of full current already puts COL_DIM near the bottom of what can be read.
 *
 * The names are about the light in the room. They have nothing to do with the
 * ring's night window, which is about the hour, and neither one reads the
 * other.
 *
 * Descending order is not decoration: bl_preset_next() is written as "the first
 * one dimmer than this", which is what makes it behave sensibly for a level the
 * endpoint set that is not in the table at all.
 */
struct BacklightPreset {
    uint8_t level;
    const char *name;
};

static const BacklightPreset bl_preset_table[] = {
    {16, "full"},
    {11, "day"},
    { 7, "room"},
    { 4, "night"},
};

#define BL_PRESET_COUNT ((int)(sizeof(bl_preset_table) / sizeof(bl_preset_table[0])))

/* The name of a level that is exactly a preset, or NULL for one that is not.
   NULL rather than a nearest match: a level set over HTTP for an experiment
   should say what it is, not be rounded into a word that misdescribes it. */
static const char *bl_preset_name(uint8_t level) {
    for (int i = 0; i < BL_PRESET_COUNT; i++)
        if (bl_preset_table[i].level == level) return bl_preset_table[i].name;
    return NULL;
}

/*
 * What a click on the menu row moves to: the brightest preset that is DIMMER
 * than where we are, or back to the top when there is none.
 *
 * Written as a comparison rather than as an index step so that it answers for
 * every level and not only for the four in the table. A device sitting at 13
 * because somebody was experimenting goes to 11 on the first click and joins
 * the cycle; one sitting at 0 because the idle policy blanked it goes to full,
 * which is the only sensible thing a click on a dark screen can mean.
 */
static uint8_t bl_preset_next(uint8_t level) {
    for (int i = 0; i < BL_PRESET_COUNT; i++)
        if (bl_preset_table[i].level < level) return bl_preset_table[i].level;
    return bl_preset_table[0].level;
}

/*
 * What the menu row carries after its name: the preset's word, or the bare step
 * for a level that has none, or "off".
 *
 * The row's NAME is ui.h's and goes in front of this, the same way the Quiet
 * row's hours and the Messages row's count are built. It takes a level rather
 * than reading the live word so that it can be sliced onto the host: the width
 * argument at the call site counts these exact characters, and the longest this
 * can produce is the seven of "step 16", which as the row "Backlight step 16"
 * measures 210px of font 4 with the marker, against the 296px ui_draw_row()
 * has. The length is pinned by the host test rather than left as a sentence
 * here, the same way the Quiet row's hours are.
 *
 * BL_LABEL_MAX is that pin expressed as a buffer, so that the callers' arrays
 * and the test's expectation cannot drift apart: a fifth preset with a longer
 * name than "step 16" fails the test rather than being quietly cut short on the
 * panel. It is also what lets ui.h size its row buffer tightly enough for the
 * compiler to see that the row cannot overflow.
 *
 * Nine and not the eight the pinned seven characters need, because the argument
 * is a uint8_t and the compiler is right to count what the TYPE can print
 * rather than what the ladder can produce: "step 255" is what an out-of-range
 * level would come to, and it should come out whole rather than cut in half.
 * The endpoint refuses those, so this is the second line and not the first.
 */
#define BL_LABEL_MAX 9

static void bl_level_label(uint8_t level, char *out, size_t n) {
    if (level == 0) {
        snprintf(out, n, "off");
        return;
    }
    const char *name = bl_preset_name(level);
    if (name) snprintf(out, n, "%s", name);
    else      snprintf(out, n, "step %u", (unsigned)level);
}
/* host-test:end */

/* --- State ---
 *
 * bl_wanted is written by the endpoint on the web-server task and by the menu
 * on the loop task, so it is volatile for the reason every cross-task word in
 * skills/ring.cpp is: it is a single aligned scalar, the worst a collision can
 * cost is one poll acting on the previous value, and the next poll fixes it.
 *
 * bl_applied is NOT volatile and must not become so. It is this file's model of
 * what the counter inside the part is standing on, it is only ever read and
 * written from backlight_poll() on the loop task, and it is the one piece of
 * state here that would actually be corrupted by a second writer — a wrong
 * value in it makes every later pulse count wrong, and stays wrong. Keeping it
 * single-task is what makes the relative arithmetic safe.
 */
static volatile uint8_t bl_wanted = BL_DEFAULT;
static uint8_t bl_applied = 0;          /* 0 = the part is shut down */
static bool bl_ready = false;

/* When the line was last taken low, so that a level asked for immediately after
   an off waits for the part to have actually shut down. */
static unsigned long bl_off_at = 0;

static volatile bool bl_cfg_dirty = false;
static volatile unsigned long bl_cfg_save_at = 0;

/* --- Driving the pin ---
 *
 * One store each. See the head of this file for why these are not
 * digitalWrite() and why the pin is still declared through pinMode() once. */

/* W1TS/W1TC are the low bank and cover GPIO0-31; 32 and above are a second
   pair of registers entirely. Pin 21 is well inside the first, but this board
   brings out GPIOs up to 48, and moving the backlight to one of those would
   otherwise set the right bit of the wrong register and pulse nothing — a
   failure with no symptom to follow back here. Made a compile error instead. */
static_assert(PIN_TFT_BL < 32,
              "PIN_TFT_BL must be in the GPIO_OUT_W1TS/W1TC bank (GPIO0-31)");

#define BL_PIN_MASK (1UL << PIN_TFT_BL)

static inline void bl_pin_high() { REG_WRITE(GPIO_OUT_W1TS_REG, BL_PIN_MASK); }
static inline void bl_pin_low()  { REG_WRITE(GPIO_OUT_W1TC_REG, BL_PIN_MASK); }

/* The critical section's lock. One pulse train at a time, and only ever from
   the loop task — this exists to keep interrupts off the wave, not to arbitrate
   between callers. */
static portMUX_TYPE bl_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Walk the part from bl_applied to `want`. Loop task only.
 *
 * Returns false when nothing could be done yet, which happens in exactly one
 * situation: the part was switched off less than BL_OFF_MS ago and has not
 * necessarily noticed. Enabling it before it has shut down would not restart
 * the counter, so the "first edge lands on full" assumption below would be
 * wrong and every level after it would be off by however many steps the old
 * counter held. The poll simply asks again a few milliseconds later rather than
 * busy-waiting, because the only thing that ever hits this is turning the
 * screen back on within one frame of blanking it.
 */
static bool bl_drive(uint8_t want) {
    if (want == 0) {
        bl_pin_low();
        bl_off_at = millis();
        bl_applied = 0;
        return true;
    }

    if (bl_applied == 0) {
        if (millis() - bl_off_at < BL_OFF_MS) return false;
        /* The enabling edge IS the first counted edge, and the datasheet's step
           1 is maximum — so the part comes up at full and the walk below starts
           from there. Both correct drivers make the same assumption. */
        bl_pin_high();
        delayMicroseconds(BL_ON_US);
        bl_applied = BL_STEPS;
    }

    uint8_t n = bl_pulses(bl_applied, want);
    if (n > 0) {
        /* Low then high: the RISING edge is the one the part counts. */
        portENTER_CRITICAL(&bl_mux);
        for (uint8_t i = 0; i < n; i++) {
            bl_pin_low();
            delayMicroseconds(BL_LO_US);
            bl_pin_high();
            delayMicroseconds(BL_HI_US);
        }
        portEXIT_CRITICAL(&bl_mux);
    }
    bl_applied = want;
    return true;
}

/* --- Persistence --- */

static void bl_cfg_save() {
    JsonDocument doc;
    doc["level"] = bl_wanted;
    String out;
    serializeJson(doc, out);
    write_spiffs_file_atomic(BL_CFG_FILE, BL_CFG_TMP, out);
}

static void bl_cfg_mark_dirty() {
    unsigned long at = millis() + BL_CFG_DELAY_MS;
    if (!bl_cfg_dirty || (long)(at - bl_cfg_save_at) < 0) bl_cfg_save_at = at;
    bl_cfg_dirty = true;
}

/* A file that is missing, truncated or nonsense leaves the compiled default
   standing, and the range is checked rather than trusted for the reason the
   ring's loader gives about its own fields: a filesystem is not a more
   trustworthy source than a request body, since anyone who can write an image
   can write this.

   A stored 0 is honoured. It means the device was blanked and then rebooted,
   and coming up dark is what was asked for — the menu and the endpoint both
   still reach it, and the boot event line says so out loud. */
static void bl_cfg_load() {
    String raw = read_spiffs_file(BL_CFG_FILE);
    if (raw.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
    if (doc["level"].is<int>()) {
        int level = doc["level"].as<int>();
        if (bl_level_valid(level)) bl_wanted = (uint8_t)level;
    }
}

/* --- Poll --- */

/* Called every loop() pass. Two comparisons when nothing has changed, which is
   almost always: the pulse train only runs on an actual change of level. */
static void backlight_poll() {
    if (!bl_ready) return;

    if (bl_wanted != bl_applied) bl_drive(bl_wanted);

    if (bl_cfg_dirty && (long)(millis() - bl_cfg_save_at) >= 0) {
        bl_cfg_dirty = false;
        bl_cfg_save();
    }
}

/* --- The row on the panel --- */

/* Set the level from the device itself, which is the only route that exists
   with no network in reach. The decision is bl_preset_next()'s, which is what
   the host test drives; this is the half that reads the live word, writes it
   back and says so in the log. */
static void backlight_menu_click() {
    uint8_t next = bl_preset_next(bl_wanted);
    /* Defensive, and unreachable as the table stands: bl_preset_next() can only
       hand back the level it was given if there is nothing in the table dimmer
       than it AND the top entry is it, which takes a one-entry table — and the
       host test pins that there is more than one. Kept because it costs a
       comparison and the alternative is a click that logs a change it did not
       make. */
    if (next == bl_wanted) return;
    bl_wanted = next;
    bl_cfg_mark_dirty();
    char label[BL_LABEL_MAX];
    bl_level_label(next, label, sizeof(label));
    event_add("backlight: %s (%u/%u, %lu mA)", label, (unsigned)next,
              (unsigned)BL_STEPS, (unsigned long)(bl_total_ua(next) / 1000));
}

/* --- Endpoints --- */

static const SkillEndpoint backlight_endpoints[] = {
    {"GET",  "/backlight", "Backlight level, what it draws, and the presets the menu offers"},
    {"POST", "/backlight", "Set {level} — 0 for off, 1 (dimmest) to 16 (brightest)"},
    {NULL, NULL, NULL}
};

static void backlight_state_json(JsonDocument &doc) {
    uint8_t level = bl_wanted;
    doc["ready"] = bl_ready;
    doc["pin"] = PIN_TFT_BL;
    doc["level"] = level;
    doc["steps"] = BL_STEPS;
    doc["on"] = level > 0;
    /* The step the part itself counts in, so a reading taken here can be
       compared against the datasheet's own table without doing the subtraction
       by hand. Absent when the part is off, because it is not counting. */
    if (level > 0) doc["chip_step"] = bl_edge_of(level);
    const char *name = bl_preset_name(level);
    if (name) doc["preset"] = name;
    /* Exact: every current on this ladder is a multiple of 1.25mA, which is a
       quarter and therefore has no rounding in it. */
    doc["led_ma"] = bl_current_ua(level) / 1000.0;
    doc["total_ma"] = bl_total_ua(level) / 1000.0;
    JsonArray presets = doc["presets"].to<JsonArray>();
    for (int i = 0; i < BL_PRESET_COUNT; i++) {
        JsonObject p = presets.add<JsonObject>();
        p["name"] = bl_preset_table[i].name;
        p["level"] = bl_preset_table[i].level;
    }
}

/* notify_take_body() and notify_send_error() are notify.cpp's, borrowed the
   same way ring.cpp and voice.cpp borrow them rather than growing a fourth copy
   of the same four-line helpers. Both routes are registered with
   AsyncURIMatcher::exact(), as every route in this firmware is. */
static void backlight_register_routes(AsyncWebServer &server) {

    server.on(AsyncURIMatcher::exact("/backlight"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        backlight_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/backlight"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON with a level");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        if (!input["level"].is<int>()) {
            notify_send_error(req, 400, "level must be a number, 0 (off) to 16 (brightest)");
            return;
        }
        int level = input["level"].as<int>();
        if (!bl_level_valid(level)) {
            notify_send_error(req, 400, "level must be 0 (off) to 16 (brightest)");
            return;
        }

        if ((uint8_t)level != bl_wanted) {
            bl_wanted = (uint8_t)level;
            bl_cfg_mark_dirty();
            char label[BL_LABEL_MAX];
            bl_level_label((uint8_t)level, label, sizeof(label));
            event_add("backlight: %s (%u/%u, %lu mA)", label, (unsigned)level,
                      (unsigned)BL_STEPS,
                      (unsigned long)(bl_total_ua((uint8_t)level) / 1000));
        }

        JsonDocument doc;
        doc["ok"] = true;
        backlight_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    }, NULL, handle_body_collect);
}

static const char *backlight_describe() {
    return "## Skill: backlight\n\n"
           "The panel's backlight, as a level rather than a switch. GPIO21 is\n"
           "the enable pin of an AW9364 LED driver, which takes its brightness\n"
           "from a pulse count on that same line: each rising edge steps the\n"
           "current down one of sixteen, and the seventeenth wraps back to\n"
           "full.\n\n"
           "This is the largest single consumer on the board. Measured on the\n"
           "device, the backlight is 85mA of a 150mA total; at full the four\n"
           "LED channels draw 80mA between them, and every step down the ladder\n"
           "takes 5mA off that.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /backlight | `{\"ready\":true,\"pin\":21,\"level\":16,\"steps\":16,\"on\":true,\"chip_step\":1,\"preset\":\"full\",\"led_ma\":20,\"total_ma\":80,\"presets\":[...]}` |\n"
           "| POST | /backlight | `{\"level\":11}` — 0 for off, 1 (dimmest) to 16 (brightest) |\n\n"
           "`level` counts the way a person expects: 16 is brightest and 1 is\n"
           "dimmest. `chip_step` is the same setting in the part's own\n"
           "numbering, where 1 is 20mA and 16 is 1.25mA, so a reading can be\n"
           "checked against the datasheet without arithmetic. The value\n"
           "survives a reboot, including a stored 0.\n\n"
           "### When the level and the part disagree\n\n"
           "The AW9364 cannot be read back. Nothing can ask it which of the\n"
           "sixteen steps it is standing on, so the level above is a MODEL that\n"
           "this skill keeps in step by counting the edges it sends. Anything\n"
           "that puts an edge on GPIO21 from outside moves the part without\n"
           "moving the model, and every level set afterwards is off by the same\n"
           "amount, with nothing in this response able to show it. That is why\n"
           "/gpio/write and /gpio/mode refuse the pin.\n\n"
           "If the two ever do come apart, `{\"level\":0}` followed by any level\n"
           "puts them back together. Off holds the line low past the part's\n"
           "shutdown delay, and the edge that enables it again brings it up at\n"
           "full — a step both sides know — so the count starts from a fact\n"
           "rather than from a memory. It costs a blink of the panel.\n\n"
           "### Levels, and why not all sixteen are offered\n\n"
           "The ladder is linear in CURRENT and the eye is not, so the top half\n"
           "of it — 20mA down to 10mA — is about one stop in total and reads as\n"
           "very nearly one brightness. The palette narrows it further: the\n"
           "colour most of the screen is drawn in sits at a fifth of white's\n"
           "luminance before the backlight is touched at all.\n\n"
           "So the menu offers four: **full** (16), **day** (11), **room** (7)\n"
           "and **night** (4), roughly half a stop apart each. The names are\n"
           "about the light in the room and have nothing to do with the ring's\n"
           "night window, which is about the hour. The whole 1..16 stays\n"
           "reachable here, which is the point of the endpoint accepting a\n"
           "level the menu will not offer — the four are a starting point to be\n"
           "judged against a real screen, not a measurement.\n\n"
           "Off is a level for this endpoint and not for the menu: a click on\n"
           "the device must not be able to blank the screen the menu is being\n"
           "read on.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"level\":7}' http://seed.local:8080/backlight\n"
           "```\n";
}

static const Skill backlight_skill = {
    .name = "backlight",
    .version = "0.1.0",
    .describe = backlight_describe,
    .endpoints = backlight_endpoints,
    .register_routes = backlight_register_routes
};

/*
 * Bring the backlight up, from display_init().
 *
 * Split from skill_backlight_init() the same way, and for the same reason, as
 * the fuel gauge's probe: skills_init() runs after the network is up, and a
 * panel that stayed dark until then would be dark across the whole of WiFi
 * association. The light has to come on the moment there is something on the
 * screen to light, so the bring-up is called from where the two lines it
 * replaces used to be and only the registration waits for the skill list.
 *
 * bl_applied starts at 0 and that is the truth rather than an assumption: EN
 * has a 150k internal pull-down, and a reset leaves the pin an input, so the
 * part is shut down through every boot including a warm one.
 */
static void backlight_begin() {
    /* Once, and only for the peripheral manager's benefit — the level changes
       below go straight to the registers. Without this the pin is not
       registered as a GPIO and skills/gpio.cpp would describe it wrongly. */
    pinMode(PIN_TFT_BL, OUTPUT);

    /* Low first, and stamped, so that the shutdown the first enable depends on
       is something this function ENFORCES rather than something it inherits.
       On a cold boot the pull-down has already seen to it; on a warm one the
       pin was driven high microseconds ago by the run that just restarted, and
       an enable before the part has shut down would land on the old counter
       instead of on full. */
    bl_pin_low();
    bl_off_at = millis();
    bl_ready = true;

    bl_cfg_load();

    /* Waited out here rather than left to the next poll, which is what every
       other deferral in this file does. loop() does not run until setup() has
       finished associating with the network, so a level handed to the poll
       would leave the panel dark for the whole of that — the one thing this
       function exists to prevent. Bounded by BL_OFF_MS, and the config read
       above has usually spent it already. */
    while (!bl_drive(bl_wanted)) delay(1);

    char label[BL_LABEL_MAX];
    bl_level_label(bl_wanted, label, sizeof(label));
    event_add("backlight: %s (%u/%u, %lu mA)", label, (unsigned)bl_wanted,
              (unsigned)BL_STEPS, (unsigned long)(bl_total_ua(bl_wanted) / 1000));
}

/* Nothing to bring up: the pin was configured and the stored level applied at
   display_init(), before the network existed. This is the registration and
   nothing else. */
static void skill_backlight_init() {
    skill_register(&backlight_skill);
}
