/*
 * skills/progress.cpp — one progress mechanism, several suppliers
 *
 * Something on this device is busy and the owner wants to know how far along it
 * is. Until now exactly one thing could say so: loop() read ir_progress() and
 * pushed a percentage straight at the LED ring, clearing it unconditionally on
 * every pass where no blast was running. That worked because there was only
 * ever one supplier. There are three now — an IR blast started from the knob, a
 * voice take being uploaded, and whatever an agent pushes over HTTP — and three
 * things writing one arc with no rule between them is a display that shows the
 * wrong job at the wrong moment and never says a word about it.
 *
 * So this is the rule, written once, in one place.
 *
 * Endpoints:
 *   POST /progress — publish or clear one job {label, percent, ttl_s, done}
 *   GET  /progress — every live job, and which one is on the bar
 *
 * Both are registered with AsyncURIMatcher::exact(), and POST registers
 * handle_body_collect. The library's default matcher answers ^{uri}(/.*)?$ and
 * a POST route with no body collector reads NULL from _tempObject forever —
 * this firmware has already paid for both mistakes, and tools/test_routes.sh
 * now gates them.
 *
 * The job model
 * -------------
 * A job is a label, a percentage and a deadline. That is the whole model.
 *
 * The LABEL IS THE KEY, the way `id` is the key on POST /notify: publishing
 * again under a label that is already live updates that job rather than opening
 * a second one, so a task reporting every few seconds leaves one bar rather
 * than fifty. It is also what is drawn, which is why it is bounded at 16
 * characters of printable ASCII and why an over-long one is REFUSED rather than
 * truncated: truncation would silently make "download-backups" and
 * "download-photos" the same key, and the caller would never learn that its two
 * jobs had become one.
 *
 * Arbitration
 * -----------
 * The bar shows exactly one job, chosen like this:
 *
 *   1. An expired job is not a candidate at all.
 *   2. A DEVICE-initiated job beats a NETWORK-pushed one. An IR blast the owner
 *      just started with the knob outranks a download somebody pushed over
 *      HTTP — his finger is on the control, and that is what he is looking at.
 *   3. Between two of the same class, the most recently updated one wins. The
 *      job that just said something is the job with news.
 *   4. Between two updated in the same millisecond, the lower slot. Not a
 *      meaningful rule, but a deterministic one — a display that flickers
 *      between two jobs is worse than one that picks the wrong one.
 *
 * The same order decides who loses a slot when all four are taken: a network
 * push may only displace another network push, never something started on the
 * device. When there is nothing it is allowed to drop, it is refused with a
 * reason rather than quietly dropping the blast the owner is watching.
 *
 * And one rule that outranks all of that, on the LED ring only: an unread
 * CRITICAL notification and a live progress job take turns, with the critical
 * given the longer turn and the whole ring. The ring is the across-the-room
 * channel and a critical message must never be outbid on it. See
 * progress_ring_phase() below for the reasoning and the numbers — it is the
 * part of this file most likely to be "simplified" by somebody who has not
 * thought about what the device is for.
 *
 * That alternation is the ring's alone. The bar on the clock face runs
 * continuously, because the panel is the up-close channel and it already shows
 * an unread critical separately, in the hairline under the header that breathes
 * red and in the unread badge. Two channels, two jobs.
 *
 * TTL is per push, not a constant
 * -------------------------------
 * A publisher says "expect my next update within N seconds" and the bar is
 * patient for exactly that long. This matters because the intended publisher is
 * an agent updating the bar BETWEEN the steps of a long task — gates, review,
 * flash — and a step can honestly take minutes. A fixed short expiry would
 * blink the bar out in the middle of real work.
 *
 * It still expires, because a publisher that dies at 60% must not leave 60% on
 * the screen until the next reboot. Default 30s, maximum 3600s: an hour is far
 * longer than any single step of that workflow and still bounded.
 *
 * Reaching 100 shortens the deadline to PROGRESS_DONE_LINGER_MS rather than
 * removing the job outright, so a finished job is visibly finished for a moment
 * instead of vanishing at 97%.
 *
 * What is deliberately absent
 * ---------------------------
 * No persistence. Progress is transient by definition; a percentage restored
 * from flash after a reboot describes a job that is no longer running.
 *
 * No events. A job updating once a second would push 64 entries through the
 * event ring in a minute and take everything else in it out, and "download is
 * at 43%" is not something anyone reads a log for.
 *
 * Concurrency
 * -----------
 * Two tasks touch the store: the AsyncTCP task through POST /progress, and
 * loop() through the device suppliers and progress_poll(). One spinlock, and
 * every critical section is a walk of at most PROGRESS_MAX entries with a
 * bounded copy in it — no allocation, no file I/O, no time() call.
 *
 * Deadlines are stamped on one task and read on the other, which is the exact
 * shape that has cost this project four defects, so every clock comparison here
 * is signed and progress_poll() samples millis() once and passes it down.
 *
 * The endpoints never draw and never expire anything. They validate into
 * locals, take the lock, write the store, and return; loop() owns the panel,
 * the ring and the reaping.
 *
 * Testing
 * -------
 * The selector, the store operations and the ring's phase are pure computation
 * over an array and a clock reading passed in, and tools/test_progress.sh
 * compiles them on the host straight out of this file.
 *
 * What is NOT covered, said plainly: the drawing, the pixels, and the wiring in
 * ring.cpp that reduces the top unread level to the `crit_unread` bool this
 * file reasons about. Those need a panel and a WS2812 string. The phase
 * function being right is not the same as the ring showing red, and no claim is
 * made that it is.
 */

/* --- The core ---
 *
 * Everything from here to the end marker is compiled verbatim on the host by
 * tools/test_progress.sh. Keep the two markers on lines of their own, and keep
 * every comment inside fully closed: the slicer copies from the marker without
 * understanding what it copies. */
/* host-test:begin core — sliced out by tools/test_progress.sh */
#define PROGRESS_MAX            4
#define PROGRESS_LABEL_LEN     17    /* 16 characters plus the terminator */
#define PROGRESS_TTL_DEFAULT_S 30
#define PROGRESS_TTL_MAX_S   3600
/* How long a job that has reached 100 stays on the bar. */
#define PROGRESS_DONE_LINGER_MS 3000

/* Ordered, and the order is the priority: a higher value outranks a lower one.
   Spelling it as a comparison rather than as a chain of if-statements is what
   makes the rule one line in progress_core_select() instead of a shape that has
   to be re-read to be believed. */
enum { PROGRESS_NET = 0, PROGRESS_DEVICE = 1 };

struct ProgressJob {
    char label[PROGRESS_LABEL_LEN];
    unsigned long deadline_ms;
    unsigned long updated_ms;
    uint8_t percent;              /* 0..100 */
    uint8_t source;               /* PROGRESS_NET or PROGRESS_DEVICE */
    bool live;
};

/* Bounded copy, written out rather than left to snprintf, so that nothing
   inside the lock is a libc call with a format string in it — the same
   discipline every other critical section in this firmware keeps. */
static void progress_copy_label(char *dst, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < PROGRESS_LABEL_LEN; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * Is this label usable, and if not, why not.
 *
 * The ASCII rule is not tidiness. The panel's compiled TFT_eSPI fonts cover
 * 32..126 and nothing else: a Cyrillic or accented label would be drawn as
 * garbage, on a device with no console, with no error raised anywhere. This
 * firmware has been bitten by exactly that once already, so the refusal happens
 * at the endpoint where somebody is still holding the curl command.
 */
static bool progress_label_check(const char *label, const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;

    if (!label || !label[0]) { *err = "label is required and must not be empty"; return false; }
    size_t n = strlen(label);
    if (n >= PROGRESS_LABEL_LEN) { *err = "label is too long: 16 characters at most"; return false; }

    bool any = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)label[i];
        if (c < 0x20 || c > 0x7E) {
            *err = "label must be printable ASCII (32..126): the panel has no other glyphs";
            return false;
        }
        if (c != ' ') any = true;
    }
    /* A label of nothing but spaces is an empty label with extra steps: it
       cannot be read on the bar and it is a key nobody can type twice. */
    if (!any) { *err = "label must not be blank"; return false; }
    return true;
}

/* Signed, always. The deadline is stamped by whichever task published and read
   by the loop task, so a stamp a millisecond newer than the reading must come
   out as "not yet" rather than as forty-nine days. */
static bool progress_expired(const ProgressJob &j, unsigned long now) {
    return (long)(now - j.deadline_ms) >= 0;
}

/* The slot holding this label, live or expired-but-not-yet-reaped. Republishing
   under a label whose deadline has just passed revives that job in place rather
   than opening a second one beside it. */
static int progress_find(const ProgressJob *jobs, const char *label) {
    for (int i = 0; i < PROGRESS_MAX; i++) {
        if (jobs[i].live && strcmp(jobs[i].label, label) == 0) return i;
    }
    return -1;
}

/*
 * Which slot a NEW label gets: a free one, then an expired one, and only then
 * one taken from a live job.
 *
 * That last step follows the same priority the bar does. A network push may
 * take a slot from another network push and from nothing else; when every slot
 * holds a device job it is refused, which is the honest answer — the
 * alternative is a curl command silently cancelling the blast the owner is
 * standing in front of.
 */
static int progress_admit(const ProgressJob *jobs, uint8_t source, unsigned long now) {
    for (int i = 0; i < PROGRESS_MAX; i++) if (!jobs[i].live) return i;
    for (int i = 0; i < PROGRESS_MAX; i++) if (progress_expired(jobs[i], now)) return i;

    int victim = -1;
    for (int i = 0; i < PROGRESS_MAX; i++) {
        if (source != PROGRESS_DEVICE && jobs[i].source == PROGRESS_DEVICE) continue;
        if (victim < 0) { victim = i; continue; }
        if (jobs[i].source != jobs[victim].source) {
            if (jobs[i].source < jobs[victim].source) victim = i;
            continue;
        }
        if ((long)(jobs[i].updated_ms - jobs[victim].updated_ms) < 0) victim = i;
    }
    return victim;   /* -1: a network push that found only device jobs */
}

/*
 * Publish or update one job. `now` is the caller's single clock reading.
 *
 * `percent` is clamped rather than rejected here, because the device suppliers
 * compute it as sent*100/total and a rounding surprise must not become a bar
 * drawn past its own end. POST /progress is stricter than this on the way in: a
 * client that sends 142 has a bug, and a silent clamp is how that bug survives.
 */
static bool progress_core_publish(ProgressJob *jobs, const char *label, int percent,
                                  unsigned long ttl_s, uint8_t source,
                                  unsigned long now, const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;

    if (!progress_label_check(label, err)) return false;
    if (ttl_s == 0) ttl_s = PROGRESS_TTL_DEFAULT_S;
    if (ttl_s > PROGRESS_TTL_MAX_S) { *err = "ttl_s must be 1..3600"; return false; }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int slot = progress_find(jobs, label);
    if (slot < 0) slot = progress_admit(jobs, source, now);
    if (slot < 0) { *err = "no free slot, and none of the running jobs is mine to drop"; return false; }

    ProgressJob &j = jobs[slot];
    progress_copy_label(j.label, label);
    j.percent = (uint8_t)percent;
    j.source = source;
    j.updated_ms = now;
    j.deadline_ms = now + ttl_s * 1000UL;
    /* A finished job is news for a moment and noise after that. Shortening
       rather than overwriting, so a caller that asked for two seconds does not
       have them stretched to three by reaching the end. */
    if (percent == 100) {
        unsigned long linger = now + PROGRESS_DONE_LINGER_MS;
        if ((long)(linger - j.deadline_ms) < 0) j.deadline_ms = linger;
    }
    j.live = true;
    return true;
}

/* Returns whether there was anything to clear. Clearing a label that is not
   there is not an error: a cleanup step that runs twice should not fail the
   second time. */
static bool progress_core_clear(ProgressJob *jobs, const char *label) {
    if (!label || !label[0]) return false;
    int slot = progress_find(jobs, label);
    if (slot < 0) return false;
    jobs[slot].live = false;
    return true;
}

/* Drop everything past its deadline. Separate from the selector so that an
   expired job's slot is genuinely free for the next publisher rather than
   merely invisible on the bar. */
static int progress_core_reap(ProgressJob *jobs, unsigned long now) {
    int n = 0;
    for (int i = 0; i < PROGRESS_MAX; i++) {
        if (jobs[i].live && progress_expired(jobs[i], now)) { jobs[i].live = false; n++; }
    }
    return n;
}

/*
 * The arbiter: which job the bar shows, or -1 for none. See the header for the
 * rule; this is that rule and nothing else, which is why it is the piece with a
 * host test.
 */
static int progress_core_select(const ProgressJob *jobs, unsigned long now) {
    int best = -1;
    for (int i = 0; i < PROGRESS_MAX; i++) {
        const ProgressJob &j = jobs[i];
        if (!j.live || progress_expired(j, now)) continue;
        if (best < 0) { best = i; continue; }

        const ProgressJob &b = jobs[best];
        if (j.source != b.source) {
            if (j.source > b.source) best = i;   /* device outranks network */
            continue;
        }
        /* Same class: whoever spoke last. Signed, because the two stamps can
           come from two different tasks. Strictly greater, so an exact tie
           keeps the lower slot and the choice never oscillates. */
        if ((long)(j.updated_ms - b.updated_ms) > 0) best = i;
    }
    return best;
}

/* --- Sharing the LED ring with an unread critical ---
 *
 * The ring is the across-the-room channel, and that is not a decoration — it is
 * the reason this device earns a place next to a phone that is already full of
 * notifications. From the other side of a room you cannot read the panel; what
 * you can see is that the ring is green, amber or red. A critical message
 * lighting that ring red is the single most important thing this firmware does.
 *
 * Progress was about to take it away. In the ring's priority stack a progress
 * arc sat ABOVE the unread breathe, so starting a download while a critical was
 * unacknowledged would have replaced the red with a percentage — silently,
 * indefinitely, since a crit does not expire until it is acknowledged.
 *
 * The answer is not to suppress progress outright: then the bar becomes
 * invisible for as long as anything is unacked, which can be hours. The two
 * ALTERNATE. But NOT on equal terms, and that inequality is the whole design:
 *
 *   - the critical phase is the whole ring pulsing red, and it is the LONGER;
 *   - the progress phase is the thin arc, and it is the SHORTER.
 *
 * If they were symmetric the ring would read as merely busy and the red would
 * stop standing out, which is the property being protected in the first place.
 *
 * The test the numbers below are chosen against: a glance from across the room
 * during ANY five-second window must land on red, while somebody deliberately
 * watching the ring still gets their percentage. With a 3s arc phase the
 * longest stretch without red is 3s, so every 5s window contains at least 2s of
 * it. Making the two phases equal at 5s and 5s would fail that outright — a
 * five-second glance could land entirely inside the arc.
 *
 * DO NOT TIDY THESE INTO A MATCHING PAIR. They are deliberately unequal, and
 * 6000 is deliberately a whole multiple of the ring's 2000ms critical breathe
 * cycle: a phase that ended mid-breath would hand over at whatever brightness
 * the red happened to be at, which is the hard visual edge the breathe exists
 * to avoid. tools/test_progress.sh pins the five-second property, so a
 * symmetric pair fails a test rather than merely looking wrong.
 */
#define PROGRESS_CRIT_PHASE_MS 6000   /* whole ring, red, three full breaths */
#define PROGRESS_ARC_PHASE_MS  3000   /* thin arc — deliberately the shorter */

enum { PROGRESS_RING_NONE = 0, PROGRESS_RING_ARC, PROGRESS_RING_CRIT };

/*
 * Who owns the ring at time `now`. A pure function of its three arguments and
 * of nothing else — no static, no millis() call — which is what makes the
 * alternation testable at all.
 *
 * `crit_unread` is specifically a CRITICAL that nobody has acknowledged. Unread
 * info and warn do not qualify and simply lose to a live job: they are already
 * visible on the clock face as the amber dot and the unread count, so the ring
 * is not their only channel. It is a crit's only channel that matters.
 *
 * Phased on the caller's clock rather than on when either party started, the
 * same way the breathe is, so that a job appearing mid-cycle joins the rhythm
 * instead of restarting it. The one artefact of that is a millis() rollover
 * landing inside a cycle, which restarts it — and since a cycle begins with the
 * critical phase, the only thing a rollover can do is give the red MORE time.
 * That is the direction to be wrong in.
 */
static uint8_t progress_ring_phase(bool crit_unread, bool progress_live,
                                   unsigned long now) {
    if (!crit_unread) return progress_live ? PROGRESS_RING_ARC : PROGRESS_RING_NONE;
    if (!progress_live) return PROGRESS_RING_CRIT;

    unsigned long cycle = PROGRESS_CRIT_PHASE_MS + PROGRESS_ARC_PHASE_MS;
    return (now % cycle) < PROGRESS_CRIT_PHASE_MS ? PROGRESS_RING_CRIT
                                                  : PROGRESS_RING_ARC;
}
/* host-test:end */

/* --- The store --- */

static ProgressJob progress_jobs[PROGRESS_MAX];
static portMUX_TYPE progress_mux = portMUX_INITIALIZER_UNLOCKED;

/* What the panel and the ring read. A copy taken once per loop() pass, owned by
   the loop task from then on: the store is under a lock, and the panel takes
   milliseconds to draw. */
struct ProgressView {
    char label[PROGRESS_LABEL_LEN];
    uint8_t percent;
    uint8_t source;
    bool present;
};

static ProgressView progress_shown;

/* --- Publishing (each takes the lock itself) --- */

/*
 * The device suppliers' entry point, called from loop(): an IR blast, a voice
 * upload. It is a separate function from the endpoint's path only so that the
 * class is decided by which door the job came through rather than by a field a
 * caller could set — nothing over HTTP can claim to be device-initiated.
 */
static bool progress_publish_device(const char *label, int percent, unsigned long ttl_s) {
    unsigned long now = millis();
    portENTER_CRITICAL(&progress_mux);
    bool ok = progress_core_publish(progress_jobs, label, percent, ttl_s,
                                    PROGRESS_DEVICE, now, NULL);
    portEXIT_CRITICAL(&progress_mux);
    return ok;
}

static bool progress_clear(const char *label) {
    portENTER_CRITICAL(&progress_mux);
    bool cleared = progress_core_clear(progress_jobs, label);
    portEXIT_CRITICAL(&progress_mux);
    return cleared;
}

/* --- Poll --- */

/*
 * Called from loop(). One millis() reading for the whole pass, which is what
 * makes the reaping and the selection agree with each other: sampling twice
 * lets a job expire between the two and be selected out of a store it has just
 * been dropped from.
 *
 * The snapshot is written on the loop task and read on the loop task, so the
 * readers below need no lock of their own.
 */
static void progress_poll() {
    unsigned long now = millis();

    portENTER_CRITICAL(&progress_mux);
    progress_core_reap(progress_jobs, now);
    int win = progress_core_select(progress_jobs, now);
    if (win >= 0) {
        progress_shown.present = true;
        progress_shown.percent = progress_jobs[win].percent;
        progress_shown.source = progress_jobs[win].source;
        memcpy(progress_shown.label, progress_jobs[win].label, PROGRESS_LABEL_LEN);
    } else {
        progress_shown.present = false;
        progress_shown.percent = 0;
        progress_shown.source = PROGRESS_NET;
        progress_shown.label[0] = '\0';
    }
    portEXIT_CRITICAL(&progress_mux);
}

/*
 * The winning job as one line for the clock face's note row, plus the
 * percentage the bar under it is drawn from. Formatting only — this skill never
 * touches TFT_eSPI, the same arrangement ir_status_line() had before the arc
 * grew an arbiter.
 *
 * Reads the loop-owned snapshot, so it must be called from the loop task, which
 * display_tick() is.
 */
static bool progress_status_line(char *out, size_t n, int *pct) {
    if (!progress_shown.present) return false;
    snprintf(out, n, "%s  %u%%", progress_shown.label, (unsigned)progress_shown.percent);
    if (pct) *pct = (int)progress_shown.percent;
    return true;
}

/* The same snapshot for the LED ring's arc. */
static bool progress_current(int *pct) {
    if (!progress_shown.present) return false;
    if (pct) *pct = (int)progress_shown.percent;
    return true;
}

/* --- Endpoints --- */

static const SkillEndpoint progress_endpoints[] = {
    {"POST", "/progress", "Publish or clear one job {label, percent, ttl_s, done}"},
    {"GET",  "/progress", "Every live job and which one is on the bar"},
    {NULL, NULL, NULL}
};

static const char *progress_describe() {
    return "## Skill: progress\n\n"
           "One progress bar, several suppliers. The device publishes into it\n"
           "(an IR blast started from the knob), and so can anything that runs\n"
           "curl. The bar is on the clock face, under the bottom row, and the\n"
           "LED ring shows the same percentage as a lit arc.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /progress | `{\"label\":\"download\",\"percent\":42,\"ttl_s\":30}`, or `{\"label\":\"download\",\"done\":true}` to clear |\n"
           "| GET | /progress | `{\"count\":N,\"showing\":\"label\",\"jobs\":[...]}` |\n\n"
           "### The job model\n\n"
           "A job is a label, a percentage and a deadline. **The label is the\n"
           "key**: posting again with the same label updates that job rather\n"
           "than opening a second one, exactly the way `id` works on\n"
           "POST /notify. Four jobs are held at once.\n\n"
           "`label` is 1-16 characters of printable ASCII (32-126) and is\n"
           "REFUSED, not truncated, when it is longer — truncation would make\n"
           "two different labels the same key without telling anyone. Non-ASCII\n"
           "is refused because the panel's fonts have no glyphs for it and it\n"
           "would be drawn as garbage with no error anywhere.\n\n"
           "`percent` is 0-100 and anything else is a 400.\n\n"
           "`ttl_s` is 1-3600, default 30, and it is set per push rather than\n"
           "being a constant: it means \"expect my next update within this many\n"
           "seconds\". Set it to cover the step you are about to run, so a step\n"
           "that honestly takes four minutes does not blink the bar out halfway\n"
           "through. A job nobody updates again still expires, so a publisher\n"
           "that dies at 60% does not leave 60% on the screen forever.\n\n"
           "Clearing: `{\"done\":true}` removes the job at once. Reaching\n"
           "`\"percent\":100` leaves it up for three more seconds and then\n"
           "removes it, so a finished job is visibly finished.\n\n"
           "### Which job is shown\n\n"
           "One bar, so one job wins:\n\n"
           "1. Expired jobs are not candidates.\n"
           "2. **Device-initiated beats network-pushed.** A blast the owner just\n"
           "   started with the knob outranks a download pushed over HTTP: his\n"
           "   finger is on the control and that is what he is watching.\n"
           "3. Between two of the same kind, the most recently updated one.\n\n"
           "The same order decides who gives up a slot when all four are taken:\n"
           "a pushed job can displace another pushed job and nothing else, and\n"
           "is refused with a reason when there is nothing it may drop.\n\n"
           "### The ring, and unacknowledged criticals\n\n"
           "One rule outranks all of the above, and only on the LED ring: while\n"
           "a `crit` notification is unacknowledged, the ring **alternates**\n"
           "between it and the progress arc — six seconds of the whole ring\n"
           "pulsing red, then three seconds of the thin arc, repeating.\n\n"
           "The phases are deliberately unequal and are not to be evened up.\n"
           "The ring is the channel you read from across a room, where the\n"
           "panel is unreadable and the only information is the colour; a\n"
           "critical message must never be outbid on it. Equal phases would\n"
           "make the ring merely busy and the red would stop standing out. As\n"
           "it stands, no five-second glance can miss the red, and anyone\n"
           "actually watching the ring still gets the percentage.\n\n"
           "Unread `info` and `warn` do not alternate: an active job simply\n"
           "wins, because those are already visible on the clock face as the\n"
           "amber dot and the unread count. The bar on the panel does not\n"
           "alternate either — it runs continuously, since the panel shows an\n"
           "unread critical separately in the breathing rule under its header.\n\n"
           "Progress is never written to flash: a percentage restored after a\n"
           "reboot describes a job that is no longer running.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"label\":\"gates\",\"percent\":40,\"ttl_s\":600}' \\\n"
           "  http://seed.local:8080/progress\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"label\":\"gates\",\"done\":true}' \\\n"
           "  http://seed.local:8080/progress\n"
           "```\n";
}

static const char *progress_source_name(uint8_t source) {
    return source == PROGRESS_DEVICE ? "device" : "network";
}

static void progress_register_routes(AsyncWebServer &server) {

    /* POST /progress */
    server.on(AsyncURIMatcher::exact("/progress"), HTTP_POST, [](AsyncWebServerRequest *req) {
        /* The collected body belongs to this handler from here on, including on
           the unauthenticated path — hence check_auth() rather than
           require_auth()'s early return. */
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON with at least a label");
            return;
        }

        JsonDocument input;
        DeserializationError jerr = deserializeJson(input, body);
        free(body);
        if (jerr != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        if (!input["label"].is<const char*>()) {
            notify_send_error(req, 400, "label is required and must be a string");
            return;
        }
        const char *label = input["label"].as<const char*>();
        const char *err = NULL;
        if (!progress_label_check(label, &err)) {
            notify_send_error(req, 400, err);
            return;
        }

        bool done = input["done"].is<bool>() && input["done"].as<bool>();

        /* A field of the wrong type is a caller bug, and answering it with a
           silent default is how that bug survives to production. */
        int percent = 0;
        if (!done) {
            if (!input["percent"].is<int>()) {
                notify_send_error(req, 400, "percent is required and must be a number 0..100");
                return;
            }
            percent = input["percent"].as<int>();
            if (percent < 0 || percent > 100) {
                notify_send_error(req, 400, "percent must be 0..100");
                return;
            }
        }

        unsigned long ttl_s = 0;   /* 0 asks the core for the default */
        if (!input["ttl_s"].isNull()) {
            /* is<unsigned long>() is false for a negative number and for a
               string, which is exactly the set that must not be turned into a
               default nobody asked for. */
            if (!input["ttl_s"].is<unsigned long>() ||
                input["ttl_s"].as<unsigned long>() == 0 ||
                input["ttl_s"].as<unsigned long>() > PROGRESS_TTL_MAX_S) {
                notify_send_error(req, 400, "ttl_s must be 1..3600");
                return;
            }
            ttl_s = input["ttl_s"].as<unsigned long>();
        }

        /* millis() before the lock, one reading for the whole operation. */
        unsigned long now = millis();
        bool ok = true, cleared = false;
        const char *why = NULL;

        portENTER_CRITICAL(&progress_mux);
        if (done) {
            cleared = progress_core_clear(progress_jobs, label);
        } else {
            ok = progress_core_publish(progress_jobs, label, percent, ttl_s,
                                       PROGRESS_NET, now, &why);
        }
        portEXIT_CRITICAL(&progress_mux);

        if (!ok) {
            /* 503: the store is full of jobs this publisher may not touch. That
               is a state of the device, not a defect in the request, and it
               goes away on its own when the device's own job finishes. */
            notify_send_error(req, 503, why ? why : "could not publish");
            return;
        }

        /* Nothing here draws. loop() picks the winner up on its next pass and
           the clock face redraws on its own tick. */
        JsonDocument doc;
        doc["ok"] = true;
        doc["label"] = label;
        if (done) doc["cleared"] = cleared;
        else      doc["percent"] = percent;
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    /* GET /progress */
    server.on(AsyncURIMatcher::exact("/progress"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        /* One copy of the store, taken under the lock, reported outside it: the
           JSON build allocates and must not happen in a critical section. */
        ProgressJob snap[PROGRESS_MAX];
        int win;
        unsigned long now = millis();
        portENTER_CRITICAL(&progress_mux);
        memcpy(snap, progress_jobs, sizeof(snap));
        win = progress_core_select(snap, now);
        portEXIT_CRITICAL(&progress_mux);

        JsonDocument doc;
        doc["capacity"] = PROGRESS_MAX;
        if (win >= 0) doc["showing"] = snap[win].label;
        JsonArray arr = doc["jobs"].to<JsonArray>();
        int count = 0;
        for (int i = 0; i < PROGRESS_MAX; i++) {
            if (!snap[i].live || progress_expired(snap[i], now)) continue;
            count++;
            JsonObject o = arr.add<JsonObject>();
            o["label"] = snap[i].label;
            o["percent"] = snap[i].percent;
            o["source"] = progress_source_name(snap[i].source);
            long left = (long)(snap[i].deadline_ms - now);
            o["expires_in_s"] = (unsigned long)(left > 0 ? left : 0) / 1000;
            o["showing"] = (i == win);
        }
        doc["count"] = count;
        notify_send_json(req, 200, doc);
    });
}

static const Skill progress_skill = {
    .name = "progress",
    .version = "0.1.0",
    .describe = progress_describe,
    .endpoints = progress_endpoints,
    .register_routes = progress_register_routes,
    // Order-free (C8): reaping only updates the loop-owned snapshot; its
    // consumers (1 Hz clock note row, 30/120 s idle policy) cannot tell a
    // one-pass-old snapshot from a fresh one. At the dispatcher position a
    // TTL-reaped bar can linger up to ~1 s (one clock repaint) on the note row.
    .tick = progress_poll
};

static void skill_progress_init() {
    memset(progress_jobs, 0, sizeof(progress_jobs));
    memset(&progress_shown, 0, sizeof(progress_shown));
    skill_register(&progress_skill);
}
