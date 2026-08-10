/*
 * skills/notify.cpp — notification queue: the seed as a pager
 *
 * Anything that can speak HTTP can poke this device. A one-line curl leaves a
 * message; the knob reads it and acknowledges it.
 *
 * Endpoints:
 *   POST /notify      — queue one {level, title, body, source, ttl_s, id, options}
 *   GET  /notify      — list, newest first, plus the unread count (?unread=1)
 *   GET  /notify/one  — one entry by id (?id=N), for a caller waiting on a reply
 *   POST /notify/ack  — mark one read {"id":N} or all of them {"all":true}
 *
 * All four are registered with AsyncURIMatcher::exact(). The library's default
 * matches ^{uri}(/.*)?$, under which /notify would also answer /notify/ack and,
 * being registered first, would silently swallow every acknowledgement. That
 * exact failure already cost this firmware a working POST /ir/tvbgone/stop.
 *
 * That rule is also why the single-entry read is /notify/one?id=N rather than
 * /notify/12: an id in the path needs a prefix or a regex matcher. The library
 * offers both, and this firmware's own route gate — tools/test_routes.sh, part
 * 3 — allows neither, because a matcher looser than exact() is what once made
 * POST /ir/tvbgone swallow /ir/tvbgone/stop. The id is a query parameter and
 * the path is a constant, which is the shape exact() can defend.
 *
 * Reply options
 * -------------
 * A notification may carry up to NOTIFY_OPT_MAX short labels. The knob picks
 * one and the index is STORED HERE, in `chosen`; nothing is sent anywhere.
 * A caller that wants the answer comes back and reads it — GET /notify/one is
 * that read, and it exists because polling the whole list for one entry
 * serialises twenty of them on every pass.
 *
 * Two consequences, both deliberate:
 *
 *   - Re-posting under the same client key RESETS the choice. notify_push()
 *     drops the old entry and appends a new one, so it happens by itself; it
 *     is also what should happen. A re-post means the question changed, and
 *     carrying yesterday's answer onto today's question is worse than having
 *     no answer at all.
 *   - Choosing an option also marks the entry read. Answering is a stronger
 *     act than reading, and a queue that still counts an answered message as
 *     unread is lying to the ring and to the badge.
 *
 * The labels themselves live in a table parallel to the slots rather than in
 * Notification, which keeps the per-entry struct at the 208 bytes it already
 * costs and puts the whole price of the feature — NOTIFY_MAX * NOTIFY_OPT_MAX
 * * NOTIFY_OPT_LEN, about 1.3 KB — in one place that can be read off.
 *
 * Store shape
 * -----------
 * A fixed array of NOTIFY_MAX slots with bounded strings — no allocation per
 * notification, so a device left running for a month costs exactly what it
 * costs at boot. Order is a separate array of slot indices held newest-first,
 * which is what both the list endpoint and the screen want, and which makes
 * removing an entry from the middle (expiry, replacement) a 20-byte shuffle
 * instead of a rewrite of the entries themselves.
 *
 * A full queue evicts in this order: the oldest already-read entry, else the
 * oldest non-critical one, else the oldest outright. A pager whose queue is
 * full of unread criticals must still be able to take the next one — refusing
 * it would make the device useless exactly when it matters — but nothing
 * unread is dropped while anything read is still occupying a slot.
 *
 * Concurrency
 * -----------
 * Two tasks touch this: the AsyncTCP task through the endpoints, and loop()
 * through notify_poll() and the screen. The store is guarded by one spinlock
 * and every critical section is a bounded memcpy or a walk of at most
 * NOTIFY_MAX entries — no allocation, no file I/O, no time() call, nothing
 * that can block or nest. Anything that does block happens outside it: the
 * wall clock is read before the lock is taken and passed in, event_add() is
 * called after it is released, and the SPIFFS write runs on the loop() task
 * from notify_poll() only.
 *
 * That is why an endpoint may write the store directly rather than staging a
 * request the way the IR skill does. There is nothing here to stage: an IR
 * blast is tens of seconds of peripheral time that must not happen on the web
 * server task, whereas queueing a notification is a struct copy. What the
 * endpoints still do not do is draw or change screens — they raise a flag and
 * loop() decides.
 *
 * Persistence
 * -----------
 * The newest NOTIFY_PERSIST entries are mirrored to SPIFFS so that a reboot —
 * an OTA apply, a flat battery, a watchdog — does not silently lose a critical
 * message. Not the whole queue: flash has a finite number of erase cycles and
 * the tail of the queue is by definition the part nobody is going to miss.
 *
 * Writes are coalesced. A burst of notifications produces one write, and
 * NOTIFY_COALESCE_MS after the last of them; a critical one is written on the
 * next loop() pass instead, because the reboot it is warning about may be
 * seconds away. The file is a snapshot taken entry by entry rather than under
 * one long lock, so a notification arriving mid-snapshot can appear twice —
 * notify_load() drops the duplicate by id rather than paying for a longer
 * critical section on every save.
 *
 * The write goes to a temp name and is renamed over the real one. Opening the
 * real file for writing would empty it first, which spends the whole write
 * with no snapshot on flash at all — and the message worth keeping across a
 * power loss is exactly the kind that arrives just before one.
 *
 * Ages survive the reboot through the stored epoch: millis() restarts at zero,
 * so an entry whose creation time was known is aged against the wall clock and
 * only falls back to millis() when one of the two is unset. An entry restored
 * before the first NTP sync therefore reads as new until the clock lands, at
 * which point every age corrects itself.
 */

/* --- Limits ---
 *
 * The string lengths are what the API accepts, and they are deliberately a
 * little wider than the panel shows: the screen ellipsises, the JSON does not.
 * NOTIFY_MAX * sizeof(Notification), plus the option table below it, is the
 * whole cost of this skill.
 *
 * Everything from here to the end marker is compiled verbatim on the host by
 * tools/test_notify_options.sh. Keep the two markers on lines of their own, and
 * keep every comment inside fully closed: the slicer copies from the marker
 * without understanding what it copies. */
/* host-test:begin types — sliced out by tools/test_notify_options.sh */
/* Queue depth / string widths — bumped 0.9.22 for mesh multi-part + long chat
 * spill. Screen still ellipsises; SPIFFS/API keep the full field.
 * RAM: ~40 * sizeof(Notification) + options table; PSRAM not required. */
#define NOTIFY_MAX          40
#define NOTIFY_PERSIST      12
#define NOTIFY_SOURCE_LEN   17   /* 16 chars, e.g. "home-rig", "k1c" */
#define NOTIFY_TITLE_LEN    61   /* 60 chars */
#define NOTIFY_BODY_LEN    241   /* 240 chars — multi-line / mesh reassembly */
#define NOTIFY_KEY_LEN      25   /* 24 chars of client-supplied dedup key */
/* The range an id may take is 1..NOTIFY_ID_MAX, and both ends are excluded for
   the same reason. 0 marks a free slot. 0xFFFFFFFF is the id whose successor is
   0, and the id counter is always left one past the highest id in play — by
   notify_push() as it hands one out, and by notify_restore_entries() as it reads
   a snapshot back. An id of 0xFFFFFFFF therefore parks the counter on 0, and the
   next notification is queued carrying the value that marks its slot free: the
   caller is told "queue full" for a message that WAS queued, the next one takes
   the same slot and overwrites it, and the list shows that one twice. */
#define NOTIFY_ID_MAX       0xFFFFFFFEu
/* Reply options. Three labels sit comfortably across the 320px panel and four
   are tight — about seven capitals each — which is the width the card's chip
   row draws into and the reason the API says so in describe() rather than
   letting a caller find out by looking at the device.
   NOTIFY_OPT_LEN is 16 for the same reason every other field here is wider
   than the panel: the JSON carries what was sent, the screen ellipsises. The
   table costs NOTIFY_MAX * NOTIFY_OPT_MAX * NOTIFY_OPT_LEN = 1280 bytes of
   static RAM, about 0.4% of it, which is worth paying once at boot rather than
   allocating per notification. */
#define NOTIFY_OPT_MAX       4
#define NOTIFY_OPT_LEN      16   /* 15 chars plus the terminator */
/* The refusal that quotes the maximum, in one place and next to the maximum
   itself: the endpoint and the builder both say it, and two copies of a number
   are how a device ends up stating a limit it no longer has. */
static const char *NOTIFY_OPT_ERR = "options must be an array of at most 4 strings";
#define NOTIFY_FILE         "/notify.json"
/* The snapshot is written here and renamed into place; see notify_save(). */
#define NOTIFY_FILE_TMP     "/notify.tmp"
/* A month. Longer than this is indistinguishable from "no expiry", which is
   what ttl_s 0 already means. */
#define NOTIFY_TTL_MAX      (30UL * 24UL * 3600UL)
#define NOTIFY_COALESCE_MS  3000

enum { NOTIFY_INFO = 0, NOTIFY_WARN, NOTIFY_CRIT };

/* The labels of one entry. A struct rather than a bare two-dimensional array so
   that copying a set in or out is one assignment, which is what keeps the
   critical sections here the fixed-size memcpys they have always been. */
struct NotifyOptions {
    char label[NOTIFY_OPT_MAX][NOTIFY_OPT_LEN];
};

struct Notification {
    uint32_t id;              /* 0 marks a free slot */
    uint32_t ttl_s;           /* 0 = never expires */
    unsigned long created_ms;
    time_t   created_epoch;   /* 0 when the clock was unset at arrival */
    uint8_t  level;
    bool     unread;
    /* Which option the knob picked, -1 while the question is unanswered, and
       how many there are to pick from. Both land in what was the struct's tail
       padding, so the queue costs exactly what it cost before. The labels are
       in notify_opt[], indexed by the same slot. */
    int8_t   chosen;
    uint8_t  opt_count;
    char source[NOTIFY_SOURCE_LEN];
    char title[NOTIFY_TITLE_LEN];
    char body[NOTIFY_BODY_LEN];
    char key[NOTIFY_KEY_LEN];
};

/* What the screen gets. A copy, not a pointer into the store: the store is
   under a lock and the panel takes milliseconds to draw. */
/* Free-text reply from the device keyboard. Parallel to the slot, not inside
   Notification: keeps the host-tested sizeof(Notification) pin intact. */
#define NOTIFY_REPLY_LEN    129  /* UTF-8 reply (~40 Cyrillic or 128 ASCII) + NUL */

struct NotifyView {
    uint32_t id;
    uint8_t  level;
    bool     unread;
    int8_t   chosen;
    uint8_t  opt_count;
    unsigned long age_s;
    char source[NOTIFY_SOURCE_LEN];
    char title[NOTIFY_TITLE_LEN];
    char body[NOTIFY_BODY_LEN];
    char key[NOTIFY_KEY_LEN];   /* client id, e.g. "hermes-chat" door vs real pages */
    char reply[NOTIFY_REPLY_LEN];
    NotifyOptions options;
};
/* host-test:end */

/* The queue is twenty of these and nothing else, so its size is worth pinning
   rather than trusting: `chosen` and `opt_count` were added into two bytes of
   tail padding the struct was already paying for, and this is what says so.
   Outside the sliced region deliberately — the host's `unsigned long` and
   `time_t` are wider than the device's and the number would not hold there. */
/* 0.9.22: title 61 + body 241 (was 41+97) → larger fixed slot; pin so host
 * tests and future field adds stay honest. */
/* 0.9.22: title 61 + body 241 (was 41+97). */
static_assert(sizeof(Notification) == 376,
              "Notification changed size: the queue costs NOTIFY_MAX times this");

/* The store itself is sliced onto the host too, because the restore loop under
   it is where a refused entry can leak a slot and only a real slot array shows
   that. The markers work the same way they do in the types region. */
/* host-test:begin store — sliced out by tools/test_notify_options.sh */
static Notification notify_slot[NOTIFY_MAX];
/* Parallel to notify_slot and indexed the same way. Only the first opt_count
   labels of a slot mean anything; the rest are whatever the previous occupant
   left, which nothing reads. */
static NotifyOptions notify_opt[NOTIFY_MAX];
static char notify_reply[NOTIFY_MAX][NOTIFY_REPLY_LEN];
static uint8_t notify_order[NOTIFY_MAX];  /* slot indices, newest first */
static uint8_t notify_len = 0;
static uint32_t notify_next_id = 1;
/* host-test:end */

static portMUX_TYPE notify_mux = portMUX_INITIALIZER_UNLOCKED;

/* Raised by the endpoint, consumed by loop(). The endpoints never draw. */
static volatile bool notify_arrived = false;
static volatile uint32_t notify_arrived_id = 0;
static volatile bool notify_dirty = false;
static unsigned long notify_save_at = 0;

/* A second, independent set of one-shots for the LED ring. The screen and the
   ring both want to hear about the same arrival, and a single flag would let
   whichever read it first swallow it — the screen consumes arrivals only on a
   pass with no user input, so sharing would silently cost the ring a comet
   whenever a hand was on the knob. Acknowledgement gets a flag rather than a
   count comparison so that a ttl expiry, which also lowers the unread count,
   cannot be mistaken for a human clearing the queue. */
static volatile bool notify_ring_arrived = false;
static volatile uint8_t notify_ring_level = NOTIFY_INFO;
static volatile bool notify_ring_acked = false;

/* And a third, for the speaker, for the same reason there is a second: three
   consumers of one arrival, and whichever read a shared flag first would
   swallow it for the others.
 *
 * Unlike the screen's and the ring's, this one carries a string. The source is
 * what decides which cue plays, so it is copied under the store lock rather
 * than left to be read a byte at a time while a second notification overwrites
 * it — a 17-byte memcpy, which is the same shape as every other critical
 * section in this file. */
static volatile bool notify_snd_arrived = false;
static uint8_t notify_snd_level = NOTIFY_INFO;
static char notify_snd_source[NOTIFY_SOURCE_LEN];

/*
 * Ask for the flash mirror to be rewritten, no sooner than `delay_ms` from now.
 *
 * Every path that changes the store goes through here, because the deadline
 * and the flag are one decision. Setting the flag without the deadline leaves
 * whatever the last POST wrote — which is in the past, and after 24.8 days of
 * uptime with no POST since boot is far enough in the past for the millis()
 * subtraction to read as negative, deferring an acknowledged critical until
 * the counter wraps.
 *
 * The earliest asked-for deadline wins: a crit POST pulls the write onto the
 * next loop() pass, and an ack or an expiry arriving after it must not push
 * that back out into a coalescing window it never asked for.
 *
 * Both fields are touched from the web server task and from loop() without the
 * store lock, as they were before this became a function. Losing that race
 * costs a coalescing window either way, never a write: the flag stays raised
 * until a save actually runs.
 */
static void notify_mark_dirty(unsigned long delay_ms) {
    unsigned long at = millis() + delay_ms;
    if (!notify_dirty || (long)(at - notify_save_at) < 0) notify_save_at = at;
    notify_dirty = true;
}

/* --- Levels --- */

static const char *notify_level_name(uint8_t level) {
    switch (level) {
        case NOTIFY_CRIT: return "crit";
        case NOTIFY_WARN: return "warn";
        default:          return "info";
    }
}

static bool notify_level_parse(const char *s, uint8_t &out) {
    if (strcmp(s, "info") == 0) { out = NOTIFY_INFO; return true; }
    if (strcmp(s, "warn") == 0) { out = NOTIFY_WARN; return true; }
    if (strcmp(s, "crit") == 0) { out = NOTIFY_CRIT; return true; }
    return false;
}

/* --- Text ---
 *
 * Copy into a bounded field, cutting on a character rather than on a byte.
 *
 * snprintf() alone cuts at whatever byte the limit lands on, and a byte in the
 * middle of a UTF-8 sequence is half a character: the field then ends in a
 * fragment that GET /notify hands to its caller verbatim and that a strict
 * JSON reader is entitled to reject. The screen is indifferent — it measures
 * in pixels and ellipsises — so this is about the wire and the snapshot file.
 *
 * Input that was not valid UTF-8 to begin with is not repaired, only left no
 * worse than it arrived.
 *
 * This and the two option helpers under it are compiled on the host by
 * tools/test_notify_options.sh; the marker rules from the types region apply.
 */
/* host-test:begin text — sliced out by tools/test_notify_options.sh */
static void notify_copy_text(char *dst, size_t size, const char *src) {
    snprintf(dst, size, "%s", src);
    size_t len = strlen(dst);
    if (len == 0 || strlen(src) <= len) return;  /* nothing was cut */

    /* Back off over the continuation bytes (10xxxxxx) to the byte that opened
       the last sequence, and drop it when what it announces did not fit. */
    size_t start = len;
    while (start > 0 && ((unsigned char)dst[start - 1] & 0xC0) == 0x80) start--;
    if (start == 0) return;
    start--;
    unsigned char lead = (unsigned char)dst[start];
    size_t need = (lead & 0x80) == 0x00 ? 1
                : (lead & 0xE0) == 0xC0 ? 2
                : (lead & 0xF0) == 0xE0 ? 3
                : (lead & 0xF8) == 0xF0 ? 4
                : 1;
    if (start + need > len) dst[start] = '\0';
}

/*
 * Is this option label usable, and if not, why not.
 *
 * The ASCII rule is not tidiness, and it is the same rule progress.cpp keeps
 * for the same reason: the panel's compiled TFT_eSPI fonts cover 32..126 and
 * nothing else, so a Cyrillic or accented label is drawn as garbage on a device
 * with no console and with no error raised anywhere. It is refused here, at the
 * endpoint, while somebody is still holding the curl command.
 *
 * Length is not checked: an over-long label is truncated by notify_copy_text()
 * like every other string this skill accepts. A label is a chip on a screen —
 * cutting it is a cosmetic loss, and refusing the whole notification over it
 * would lose the message.
 *
 * The truncation is why notify_options_build() runs this twice, on what was
 * sent and again on what was kept: the blank rule is about what the panel draws
 * and what GET /notify serves, and a cut can turn something that read as a
 * label into fifteen spaces.
 */
static bool notify_option_check(const char *label, const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;

    if (!label || !label[0]) { *err = "an option label must not be empty"; return false; }

    bool any = false;
    for (size_t i = 0; label[i]; i++) {
        unsigned char c = (unsigned char)label[i];
        if (c < 0x20 || c > 0x7E) {
            *err = "an option label must be printable ASCII (32..126): the panel has no other glyphs";
            return false;
        }
        if (c != ' ') any = true;
    }
    /* All spaces is an empty label with extra steps: an unreadable chip that
       still occupies a position the knob can land on. */
    if (!any) { *err = "an option label must not be blank"; return false; }
    return true;
}

/*
 * Fill an option set from `n` labels, or refuse the whole set with a reason.
 *
 * Shared by POST /notify and by the restore path, because a snapshot is not a
 * more trustworthy source than a request body — the filesystem can be replaced
 * wholesale — and two validators that agree today are two that can disagree
 * tomorrow.
 *
 * `n` above NOTIFY_OPT_MAX is refused BEFORE `labels` is read at all, so a
 * caller holding more labels than fit may pass the count it saw while filling
 * only the first NOTIFY_OPT_MAX pointers.
 *
 * All or nothing: one unusable label refuses the set rather than dropping it
 * and renumbering the rest. `chosen` is an index into this set, and a set that
 * quietly lost its second entry answers a different question than the one that
 * was asked.
 */
static bool notify_options_build(NotifyOptions &out, uint8_t &count,
                                 const char *const *labels, size_t n,
                                 const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;

    memset(&out, 0, sizeof(out));
    count = 0;

    if (n > NOTIFY_OPT_MAX) {
        *err = NOTIFY_OPT_ERR;
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!notify_option_check(labels[i], err)) return false;
        notify_copy_text(out.label[i], NOTIFY_OPT_LEN, labels[i]);
        /* Again, on the bytes that were kept. What was sent passing the check
           is not the same claim as what is stored passing it: "<15 spaces>XYZ"
           has a printable character in it, loses it to the cut, and is left as
           a chip nobody can read and a blank label served back over the API —
           the exact thing the blank rule exists to refuse. Checking the stored
           bytes also keeps the restore path honest, since a snapshot arrives
           already truncated and would otherwise be held to a different rule
           than the request that produced it. */
        if (!notify_option_check(out.label[i], err)) return false;
    }
    count = (uint8_t)n;
    return true;
}
/* host-test:end */

/* --- Age ---
 *
 * `now` and `now_ms` are read by the caller before it takes the lock: time()
 * reaches into the ESP-IDF time subsystem and has no business inside a
 * spinlock. Both are passed down rather than sampled per entry, which also
 * makes every age on one screen consistent with every other.
 */
static unsigned long notify_age_of(const Notification &e, time_t now,
                                   unsigned long now_ms) {
    if (e.created_epoch > TIME_VALID_EPOCH && now > TIME_VALID_EPOCH) {
        return (now >= e.created_epoch) ? (unsigned long)(now - e.created_epoch) : 0;
    }
    /* Unsigned subtraction, so this stays correct across a millis() rollover
       and across the negative created_ms notify_load() deliberately produces. */
    return (now_ms - e.created_ms) / 1000;
}

/* Age in the four characters the panel's age column is cut for. Presentation
   with no TFT in it, so it lives here next to the clock arithmetic. */
static void notify_age_str(unsigned long age_s, char *out, size_t n) {
    if (age_s < 60)          snprintf(out, n, "now");
    else if (age_s < 3600)   snprintf(out, n, "%lum", age_s / 60);
    else if (age_s < 86400)  snprintf(out, n, "%luh", age_s / 3600);
    else {
        unsigned long days = age_s / 86400;
        if (days > 99) days = 99;
        snprintf(out, n, "%lud", days);
    }
}

/* --- Store primitives (lock held by the caller) --- */

static void notify_drop_at(int pos) {
    uint8_t slot = notify_order[pos];
    notify_slot[slot].id = 0;
    notify_reply[slot][0] = '\0';
    for (int i = pos; i + 1 < notify_len; i++) notify_order[i] = notify_order[i + 1];
    notify_len--;
}

/* host-test:begin store — sliced out by tools/test_notify_options.sh */
static int notify_free_slot() {
    for (int i = 0; i < NOTIFY_MAX; i++) if (notify_slot[i].id == 0) return i;
    return -1;
}

/* Hand out the next id, keeping the counter inside the range ids live in. It
   can be found above the ceiling two ways: a restored snapshot leaves it one
   past the highest id the file carried, and a device that has issued four
   billion notifications gets there on its own. Rolling back to 1 can collide
   with a live entry, which the list survives; walking off the end into 0 cannot
   be survived, because the slot that id is written into reads as free.
   Its own function, rather than two lines inside notify_push(), only so that
   the host suite can reach it: notify_push() takes a spinlock, evicts and
   copies structs, and cannot be sliced onto the host. The restore clamp alone
   does not cover this — a counter parked at 0xFFFFFFFF still issues one valid
   id, and it is the notification AFTER that one that lands on 0. */
static uint32_t notify_take_id() {
    if (notify_next_id > NOTIFY_ID_MAX) notify_next_id = 1;
    return notify_next_id++;
}
/* host-test:end */

/* Which entry loses its slot when the queue is full. See the header: read
   entries go first, then anything not critical, and only then the oldest
   critical — a queue full of unread criticals still has to accept the next
   one. */
static int notify_victim() {
    for (int i = notify_len - 1; i >= 0; i--)
        if (!notify_slot[notify_order[i]].unread) return i;
    for (int i = notify_len - 1; i >= 0; i--)
        if (notify_slot[notify_order[i]].level != NOTIFY_CRIT) return i;
    return notify_len - 1;
}

/* --- Store operations (each takes the lock itself) --- */

/*
 * Queue one notification, replacing any live entry carrying the same client
 * key. Replacement drops the old entry and appends a new one rather than
 * editing in place: an update is news, and news belongs at the front of the
 * list. The previous numeric id is reported back so a client that was tracking
 * it knows which one it just superseded.
 *
 * Replacement is also what resets a reply: the new entry starts unanswered,
 * because the question it asks is a new one. See the header.
 *
 * `e` arrives fully built — including its timestamps — so that the critical
 * section is one struct copy and two array shuffles. `opts` is the label set
 * for e.opt_count labels, or NULL when there are none; it is a second copy of
 * the same shape rather than a field of `e` so that Notification stays the size
 * it was.
 */
static uint32_t notify_push(Notification &e, const NotifyOptions *opts,
                            uint32_t *replaced_out) {
    uint32_t replaced = 0;

    portENTER_CRITICAL(&notify_mux);

    if (e.key[0]) {
        for (int i = notify_len - 1; i >= 0; i--) {
            if (strcmp(notify_slot[notify_order[i]].key, e.key) == 0) {
                replaced = notify_slot[notify_order[i]].id;
                notify_drop_at(i);
                break;
            }
        }
    }

    if (notify_len >= NOTIFY_MAX) notify_drop_at(notify_victim());

    int slot = notify_free_slot();
    if (slot < 0) {  /* unreachable: the eviction above guarantees a free slot */
        portEXIT_CRITICAL(&notify_mux);
        if (replaced_out) *replaced_out = replaced;
        return 0;
    }

    e.id = notify_take_id();
    notify_slot[slot] = e;
    if (opts) notify_opt[slot] = *opts;
    else      memset(&notify_opt[slot], 0, sizeof(notify_opt[slot]));
    notify_reply[slot][0] = '\0';  /* new question starts unanswered in text too */
    for (int i = notify_len; i > 0; i--) notify_order[i] = notify_order[i - 1];
    notify_order[0] = (uint8_t)slot;
    notify_len++;

    uint32_t id = e.id;
    portEXIT_CRITICAL(&notify_mux);

    if (replaced_out) *replaced_out = replaced;
    return id;
}

static int notify_unread_count() {
    int n = 0;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++)
        if (notify_slot[notify_order[i]].unread) n++;
    portEXIT_CRITICAL(&notify_mux);
    return n;
}

/* Whether anything critical is still unacknowledged — the one piece of state
   the idle clock face reacts to. */
static bool notify_crit_unread() {
    bool found = false;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len && !found; i++) {
        const Notification &e = notify_slot[notify_order[i]];
        found = e.unread && e.level == NOTIFY_CRIT;
    }
    portEXIT_CRITICAL(&notify_mux);
    return found;
}

/* The most severe level still unacknowledged, or false if nothing is. The LED
   ring breathes in this colour, so it is asked for once per ring frame — hence
   one pass under the lock rather than three calls to notify_crit_unread() and
   friends. */
static bool notify_top_unread_level(uint8_t &out) {
    bool found = false;
    uint8_t top = NOTIFY_INFO;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        const Notification &e = notify_slot[notify_order[i]];
        if (!e.unread) continue;
        if (!found || e.level > top) top = e.level;
        found = true;
    }
    portEXIT_CRITICAL(&notify_mux);
    if (found) out = top;
    return found;
}

static int notify_count() {
    portENTER_CRITICAL(&notify_mux);
    int n = notify_len;
    portEXIT_CRITICAL(&notify_mux);
    return n;
}

/* The copy itself, with the lock already held: a handful of memcpys and the age
   arithmetic, both clocks having been read before the caller took it. */
static void notify_fill_view(const Notification &e, const NotifyOptions &op,
                             const char *reply, NotifyView &out,
                             time_t now, unsigned long now_ms) {
    out.id = e.id;
    out.level = e.level;
    out.unread = e.unread;
    out.chosen = e.chosen;
    out.opt_count = e.opt_count;
    out.age_s = notify_age_of(e, now, now_ms);
    memcpy(out.source, e.source, sizeof(out.source));
    memcpy(out.title, e.title, sizeof(out.title));
    memcpy(out.body, e.body, sizeof(out.body));
    memcpy(out.key, e.key, sizeof(out.key));
    if (reply) memcpy(out.reply, reply, NOTIFY_REPLY_LEN);
    else out.reply[0] = '\0';
    /* The labels come out with the entry rather than being fetched afterwards:
       a second acquisition is a second chance for the slot to have been reused
       under the reader, and then the chips would belong to another message. */
    out.options = op;
}

/* Copy entry `index` out, 0 being the newest. False when the index is past the
   end — which is also how a caller finds out that the entry it was looking at
   expired or was evicted while it was on screen. */
static bool notify_view(int index, NotifyView &out) {
    time_t now = time(NULL);
    unsigned long now_ms = millis();

    portENTER_CRITICAL(&notify_mux);
    if (index < 0 || index >= notify_len) {
        portEXIT_CRITICAL(&notify_mux);
        return false;
    }
    {
        uint8_t s = notify_order[index];
        notify_fill_view(notify_slot[s], notify_opt[s], notify_reply[s],
                         out, now, now_ms);
    }
    portEXIT_CRITICAL(&notify_mux);
    return true;
}

/*
 * The entry carrying this id, together with where it currently sits and how
 * many entries there are, all out of one acquisition.
 *
 * The card screen knows an id, not an index, precisely because the list moves
 * under it. Resolving that id to an index and then copying the entry at that
 * index is two acquisitions with a gap in between, and a notification arriving
 * in the gap shifts the list by one — so the card would draw its neighbour,
 * under the neighbour's counter, for a frame. Doing the whole lookup once is
 * what makes "an arrival cannot change which message you are reading" true of
 * the drawing and not only of the navigation.
 */
static bool notify_view_by_id(uint32_t id, NotifyView &out, int *index_out,
                              int *count_out) {
    time_t now = time(NULL);
    unsigned long now_ms = millis();
    bool found = false;

    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        uint8_t s = notify_order[i];
        const Notification &e = notify_slot[s];
        if (e.id != id) continue;
        notify_fill_view(e, notify_opt[s], notify_reply[s], out, now, now_ms);
        if (index_out) *index_out = i;
        found = true;
        break;
    }
    if (count_out) *count_out = notify_len;
    portEXIT_CRITICAL(&notify_mux);
    return found;
}

/* Position of an id in the list, or -1. The screen holds an id rather than an
   index precisely because the list can shift under it. */
static int notify_index_of(uint32_t id) {
    int found = -1;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        if (notify_slot[notify_order[i]].id == id) { found = i; break; }
    }
    portEXIT_CRITICAL(&notify_mux);
    return found;
}

/* Returns false when there is no such entry, so the endpoint can answer 404
   rather than pretend. Acking something already read is a success. */
static bool notify_ack_id(uint32_t id) {
    bool found = false, changed = false;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        Notification &e = notify_slot[notify_order[i]];
        if (e.id != id) continue;
        found = true;
        changed = e.unread;
        e.unread = false;
        break;
    }
    portEXIT_CRITICAL(&notify_mux);
    if (changed) {
        notify_mark_dirty(NOTIFY_COALESCE_MS);
        display_force = true;
        notify_ring_acked = true;
    }
    return found;
}

/*
 * Record which option the knob picked. Called from the loop task, which is
 * where every input this device has is handled.
 *
 * False when there is no such entry, when it carries no options, or when the
 * index is past the ones it does carry. The index is the screen's, and the
 * options are the store's: this is where the two are checked against each
 * other, so that an index which came from anywhere but a detent on this row —
 * a `chosen` read back out of a snapshot, a count that does not match the one
 * the caller drew — records nothing rather than an answer nobody gave.
 *
 * Acknowledgement is not reimplemented here. Answering implies reading, so this
 * calls notify_ack_id() and inherits whatever an ack does today: the deferred
 * save, the redraw, and the ring's ack one-shot. The ring flag in particular is
 * right to leave to it — an already-read entry being answered is not an event
 * the ring should replay.
 *
 * What the ack does NOT cover is the choice itself, which changes the store
 * whether or not the entry was still unread, so the save and the redraw are
 * asked for again here. Both are idempotent: the earliest save deadline wins
 * and display_force is a flag, so asking twice costs nothing.
 *
 * The caller is the card screen in ui.h, on a click while reply chips are up.
 */
static bool notify_choose_id(uint32_t id, uint8_t index) {
    bool ok = false, changed = false;

    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        Notification &e = notify_slot[notify_order[i]];
        if (e.id != id) continue;
        if (index < e.opt_count) {
            changed = (e.chosen != (int8_t)index);
            e.chosen = (int8_t)index;
            ok = true;
        }
        break;
    }
    portEXIT_CRITICAL(&notify_mux);
    if (!ok) return false;

    notify_ack_id(id);
    if (changed) {
        notify_mark_dirty(NOTIFY_COALESCE_MS);
        display_force = true;
    }
    return true;
}

/*
 * Free-text reply typed on the device keyboard. Same ack semantics as
 * notify_choose_id: answering marks the entry read. Accepts printable ASCII
 * and UTF-8 multi-byte (Cyrillic). Control bytes are dropped. Empty refused.
 */
static bool notify_set_reply(uint32_t id, const char *text) {
    if (!text || !text[0]) return false;

    char cleaned[NOTIFY_REPLY_LEN];
    size_t j = 0;
    for (size_t i = 0; text[i] && j + 1 < sizeof(cleaned); ) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            if (j == 0 || cleaned[j - 1] != ' ') cleaned[j++] = ' ';
            i++;
            continue;
        }
        if (c < 0x20) { i++; continue; }           // other controls
        if (c < 0x80) {                             // ASCII printable
            cleaned[j++] = (char)c;
            i++;
            continue;
        }
        // UTF-8 multi-byte sequence — copy whole character if it fits
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else { i++; continue; }  // invalid lead
        bool ok = true;
        for (int k = 1; k < need; k++) {
            unsigned char cc = (unsigned char)text[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { i++; continue; }
        if (j + (size_t)need >= sizeof(cleaned)) break;
        for (int k = 0; k < need; k++) cleaned[j++] = text[i + k];
        i += (size_t)need;
    }
    cleaned[j] = '\0';
    if (j == 0) return false;

    bool found = false;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        uint8_t s = notify_order[i];
        if (notify_slot[s].id != id) continue;
        memcpy(notify_reply[s], cleaned, sizeof(cleaned));
        found = true;
        break;
    }
    portEXIT_CRITICAL(&notify_mux);
    if (!found) return false;

    notify_ack_id(id);
    notify_mark_dirty(NOTIFY_COALESCE_MS);
    display_force = true;
    return true;
}

static int notify_ack_all() {
    int n = 0;
    portENTER_CRITICAL(&notify_mux);
    for (int i = 0; i < notify_len; i++) {
        Notification &e = notify_slot[notify_order[i]];
        if (e.unread) { e.unread = false; n++; }
    }
    portEXIT_CRITICAL(&notify_mux);
    if (n > 0) {
        notify_mark_dirty(NOTIFY_COALESCE_MS);
        display_force = true;
        notify_ring_acked = true;
    }
    return n;
}

/* An entry past its ttl leaves the list without saying anything — that is what
   a ttl is for. The exception is an unread critical: the whole point of the
   level is that it waits for a human, so it stays until acknowledged and only
   then becomes eligible again. */
static int notify_expire() {
    time_t now = time(NULL);
    unsigned long now_ms = millis();
    int dropped = 0;

    portENTER_CRITICAL(&notify_mux);
    for (int i = notify_len - 1; i >= 0; i--) {
        const Notification &e = notify_slot[notify_order[i]];
        if (e.ttl_s == 0) continue;
        if (e.unread && e.level == NOTIFY_CRIT) continue;
        if (notify_age_of(e, now, now_ms) < e.ttl_s) continue;
        notify_drop_at(i);
        dropped++;
    }
    portEXIT_CRITICAL(&notify_mux);
    return dropped;
}

/* One-shot handoff of an arrival to loop(), which owns every screen change. */
static bool notify_take_arrival(uint32_t *id) {
    if (!notify_arrived) return false;
    notify_arrived = false;
    if (id) *id = notify_arrived_id;
    return true;
}

/* The same handoff for the LED ring, which loop() polls on its own cadence. */
static bool notify_take_ring_arrival(uint8_t *level) {
    if (!notify_ring_arrived) return false;
    notify_ring_arrived = false;
    if (level) *level = notify_ring_level;
    return true;
}

static bool notify_take_ring_ack() {
    if (!notify_ring_acked) return false;
    notify_ring_acked = false;
    return true;
}

/* Stage a cue. Raised by POST /notify with a real arrival's level and source,
   and by POST /sound/test with an invented one — the endpoints never touch the
   I2S channel any more than they touch the panel. The flag is set last, so a
   consumer that sees it raised sees a complete source string behind it. */
static void notify_request_sound(uint8_t level, const char *source) {
    /* Flag + payload under the same critical section so the loop-core cannot
       observe a raised flag with stale level/source (dual-core ESP32-S3). */
    char staged[NOTIFY_SOURCE_LEN];
    snprintf(staged, sizeof(staged), "%s", source ? source : "");

    portENTER_CRITICAL(&notify_mux);
    notify_snd_level = level;
    memcpy(notify_snd_source, staged, sizeof(notify_snd_source));
    notify_snd_arrived = true;
    portEXIT_CRITICAL(&notify_mux);
}

static bool notify_take_sound_arrival(uint8_t *level, char *source, size_t n) {
    char staged[NOTIFY_SOURCE_LEN];
    uint8_t lv = 0;
    bool got = false;

    portENTER_CRITICAL(&notify_mux);
    if (notify_snd_arrived) {
        notify_snd_arrived = false;
        lv = notify_snd_level;
        memcpy(staged, notify_snd_source, sizeof(staged));
        got = true;
    }
    portEXIT_CRITICAL(&notify_mux);

    if (!got) return false;
    if (level) *level = lv;
    if (source && n) snprintf(source, n, "%s", staged);
    return true;
}

/* --- Persistence ---
 *
 * The two halves of the file format, one entry at a time, with no filesystem
 * and no store in either of them. tools/test_notify_options.sh compiles them on
 * the host against the real ArduinoJson and round-trips one through the other,
 * which is what pins the two-letter keys to each other: a writer emitting "op"
 * against a reader looking for "opt" loses every reply across a reboot and
 * nothing else in this firmware would notice.
 */
/* host-test:begin snapshot — sliced out by tools/test_notify_options.sh */

/*
 * One entry into one JSON object.
 *
 * `e` and `op` are NON-CONST, and that is load-bearing rather than an
 * oversight. ArduinoJson stores a `const char *` by pointer and duplicates a
 * `char *`; the entries here are stack copies taken under the lock, and they
 * are gone by the time the document is serialised. Take these by const
 * reference and the snapshot silently fills with whatever the stack holds
 * later — it compiles, it runs, and the file is garbage.
 */
static void notify_snapshot_store(JsonObject o, Notification &e, NotifyOptions &op,
                                  time_t now) {
    o["id"] = e.id;
    o["lv"] = e.level;
    o["ur"] = e.unread;
    o["tt"] = e.ttl_s;
    /* An entry that arrived before the first NTP sync has no epoch of its own.
       Stamping it with the current one on the way out is the best available
       answer and beats writing a zero that reads as "just now" forever. */
    o["ts"] = (uint32_t)(e.created_epoch > TIME_VALID_EPOCH ? e.created_epoch
                         : (now > TIME_VALID_EPOCH ? now : 0));
    if (e.source[0]) o["sr"] = e.source;
    o["ti"] = e.title;
    if (e.body[0]) o["bd"] = e.body;
    if (e.key[0])  o["ky"] = e.key;
    /* Options and the reply travel together or not at all: `ch` is an index
       into `op` and means nothing beside a different set of labels. */
    if (e.opt_count > 0) {
        JsonArray a = o["op"].to<JsonArray>();
        for (uint8_t i = 0; i < e.opt_count && i < NOTIFY_OPT_MAX; i++)
            a.add(op.label[i]);
        o["ch"] = e.chosen;
    }
}

/*
 * One JSON object back into one entry, clamped.
 *
 * False when there is nothing usable here — no id, or no title — which is how
 * the caller skips an object instead of restoring a blank. `e` and `op` are
 * fully overwritten either way, and a refusal leaves `e.id` at 0: the caller
 * restores straight into a slot, and a non-zero id in a slot the caller then
 * skips is a slot no free-slot search will ever hand out again. Every path
 * that returns false must therefore clear the id it had already read.
 *
 * Every field that has a range is clamped into it. A stored file is not a more
 * trustworthy source than a request body: this filesystem can be replaced
 * wholesale by anyone who can write an image, so the numbers coming out of it
 * get the same treatment the endpoint gives the ones coming in.
 */
static bool notify_snapshot_restore(JsonObjectConst o, Notification &e, NotifyOptions &op,
                                    time_t now, unsigned long now_ms) {
    memset(&e, 0, sizeof(e));
    memset(&op, 0, sizeof(op));
    e.chosen = -1;

    e.id = o["id"] | 0u;
    /* The id has a range like every other field here — see NOTIFY_ID_MAX — and a
       stored file is the one place a value outside it can come from, because
       nothing on the device ever issues one. 0xFFFFFFFF is the only value that
       ever reaches the clamp: anything wider, negative, or not a number at all
       reads back from the JSON as 0 and is refused a line below. The clamp is
       not free. The first entry to hit it keeps its message under a name that
       is not its own, and every later entry that clamps onto the same id — as
       does a legitimate holder of NOTIFY_ID_MAX — is dropped whole by the load
       loop as the duplicate it has been made into. */
    if (e.id > NOTIFY_ID_MAX) e.id = NOTIFY_ID_MAX;
    if (e.id == 0 || !o["ti"].is<const char *>()) { e.id = 0; return false; }

    e.level = (uint8_t)(o["lv"] | 0);
    if (e.level > NOTIFY_CRIT) e.level = NOTIFY_INFO;
    e.unread = o["ur"] | false;
    /* The same ceiling the API enforces, and a ttl past the cap says "never
       expires", which is what ttl_s 0 already spells. */
    e.ttl_s = o["tt"] | 0u;
    if (e.ttl_s > NOTIFY_TTL_MAX) e.ttl_s = NOTIFY_TTL_MAX;
    e.created_epoch = (time_t)(uint32_t)(o["ts"] | 0u);
    /* Wind millis() back by however long the entry has really been alive. The
       subtraction is meant to go negative and wrap: notify_age_of() reads it
       back with the same unsigned arithmetic. */
    unsigned long elapsed = 0;
    if (e.created_epoch > TIME_VALID_EPOCH && now > e.created_epoch)
        elapsed = (unsigned long)(now - e.created_epoch) * 1000UL;
    e.created_ms = now_ms - elapsed;
    /* Through the same copy the API path uses, for the same reason: these
       strings go back out over GET /notify, and a stored file is not a more
       trustworthy source of UTF-8 than a POST body. */
    notify_copy_text(e.source, sizeof(e.source), o["sr"] | "");
    notify_copy_text(e.title, sizeof(e.title), o["ti"] | "");
    notify_copy_text(e.body, sizeof(e.body), o["bd"] | "");
    notify_copy_text(e.key, sizeof(e.key), o["ky"] | "");

    /* The options go through the endpoint's own validator, so a set that could
       not have been posted cannot be restored either. A set that fails takes
       the reply with it and leaves a plain notification, which is the readable
       half of the entry and the half worth keeping. */
    JsonArrayConst a = o["op"];
    if (!a.isNull()) {
        const char *labels[NOTIFY_OPT_MAX];
        size_t n = a.size();
        for (size_t i = 0; i < n && i < NOTIFY_OPT_MAX; i++) labels[i] = a[i] | "";
        if (!notify_options_build(op, e.opt_count, labels, n, NULL)) {
            memset(&op, 0, sizeof(op));
            e.opt_count = 0;
        }
    }
    if (e.opt_count > 0) {
        int chosen = o["ch"] | -1;
        /* -1 is "unanswered" and 0..opt_count-1 is an answer; anything else is
           a reply to a question this entry is not asking. */
        if (chosen >= 0 && chosen < (int)e.opt_count) e.chosen = (int8_t)chosen;
    }
    return true;
}

/*
 * Fill the store from a stored array, and say how many entries went in.
 *
 * A function of its own rather than a loop inside notify_load() so that the
 * host suite can run it against a real slot array: everything that can go
 * wrong here is a slot left in a state nothing looks at again, which no test
 * of notify_snapshot_restore() alone can see.
 *
 * setup()-time only. It walks and rewrites notify_slot[], notify_order[] and
 * notify_len WITHOUT taking notify_mux, and that is safe for exactly one
 * reason: notify_load() calls it from setup(), before the web server and its
 * AsyncTCP task exist, so there is no second party to race. Do not call it from
 * anywhere else — from a request handler it would rewrite the arrays a
 * concurrent notify_push() is reading.
 *
 * The file is newest-first, so appending to the tail rebuilds the order as it
 * was. Nothing else runs yet — notify_load() is called from setup() — so the
 * slots are filled directly instead of going through notify_push(), which would
 * issue new ids and reverse the list.
 *
 * Every path out of the loop that does not publish the slot puts its id back to
 * 0 first. Restoring writes into the slot before it knows whether the entry is
 * usable, so an id left behind on a slot that never reaches notify_order[] is
 * a slot that is neither free nor in the list: notify_free_slot() skips it
 * forever, and a file whose entries all fail this way leaves POST /notify
 * answering "queue full" on an empty queue.
 */
static int notify_restore_entries(JsonArrayConst arr, time_t now, unsigned long now_ms) {
    int restored = 0;

    for (JsonObjectConst o : arr) {
        if (notify_len >= NOTIFY_MAX) break;

        int slot = notify_free_slot();
        if (slot < 0) break;

        if (!notify_snapshot_restore(o, notify_slot[slot], notify_opt[slot], now, now_ms)) {
            /* Redundant today: notify_snapshot_restore() has one refusal path and
               it clears the id itself. Kept because the state it prevents — a slot
               neither free nor listed — answers "queue full" on an empty queue and
               comes back only with a reflash, and that is one guard away. The line
               DOES run, on every refused entry; what no test can observe is the
               assignment doing anything, because the id is already 0 whenever the
               guard upstream holds. Read it as belt-and-braces, not as something
               the suite has proven. */
            notify_slot[slot].id = 0;
            continue;
        }

        uint32_t id = notify_slot[slot].id;
        bool dup = false;
        for (int i = 0; i < notify_len && !dup; i++)
            dup = (notify_slot[notify_order[i]].id == id);
        if (dup) { notify_slot[slot].id = 0; continue; }

        notify_order[notify_len++] = (uint8_t)slot;
        if (id >= notify_next_id) notify_next_id = id + 1;
        restored++;
    }
    return restored;
}
/* host-test:end */

static void notify_save() {
    JsonDocument doc;
    JsonArray arr = doc["n"].to<JsonArray>();
    time_t now = time(NULL);

    /* One short critical section per entry rather than one long one. A
       notification arriving between two of them can be copied twice; the id
       makes that harmless on the way back in. */
    int count = notify_count();
    if (count > NOTIFY_PERSIST) count = NOTIFY_PERSIST;
    for (int i = 0; i < count; i++) {
        Notification e;
        NotifyOptions op;
        portENTER_CRITICAL(&notify_mux);
        bool ok = (i < notify_len);
        if (ok) {
            e = notify_slot[notify_order[i]];
            op = notify_opt[notify_order[i]];
        }
        portEXIT_CRITICAL(&notify_mux);
        if (!ok) break;

        notify_snapshot_store(arr.add<JsonObject>(), e, op, now);
    }

    String out;
    serializeJson(doc, out);
    /* Not a plain write: that truncates the file first, and the entries this
       skill exists to protect are the ones a power loss during the write would
       take. The new snapshot is complete on flash before the old one goes. */
    if (!write_spiffs_file_atomic(NOTIFY_FILE, NOTIFY_FILE_TMP, out))
        event_add("notify: save failed");
}

static void notify_load() {
    String json = read_spiffs_file(NOTIFY_FILE);
    /* Nothing under the real name means the last save was interrupted between
       its remove and its rename. What is under the temp name is then the whole
       snapshot, so take it; a save interrupted earlier than that leaves a
       partial file there instead, which fails to parse and is discarded like
       any other unreadable one. */
    if (json.length() == 0) json = read_spiffs_file(NOTIFY_FILE_TMP);
    if (json.length() == 0) return;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        event_add("notify: stored queue unreadable, discarded");
        return;
    }

    int restored = notify_restore_entries(doc["n"].as<JsonArrayConst>(),
                                          time(NULL), millis());

    /* A ttl that ran out while the device was off has still run out. */
    int dropped = notify_expire();
    if (restored > 0) {
        event_add("notify: restored %d of %d stored", restored - dropped, restored);
    }
}

/* Called every loop() pass. Expiry is worth checking at most once a second —
   ttls are in seconds — and the flash write waits for the coalescing window so
   that a burst of notifications costs one erase cycle rather than ten. */
static void notify_poll() {
    static unsigned long last_expire = 0;
    unsigned long now_ms = millis();

    if (now_ms - last_expire >= 1000) {
        last_expire = now_ms;
        if (notify_expire() > 0) {
            notify_mark_dirty(NOTIFY_COALESCE_MS);
            display_force = true;
        }
    }

    if (notify_dirty && (long)(now_ms - notify_save_at) >= 0) {
        notify_dirty = false;
        notify_save();
    }
}

/* Ingest without HTTP — same card path as POST /notify.
 * Used by MeshCore private DM (P1|…) and any future transport. */
static uint32_t notify_ingest(const char *level_s,
                              const char *source,
                              const char *title,
                              const char *body,
                              const char *key) {
    Notification e;
    memset(&e, 0, sizeof(e));
    e.level = NOTIFY_INFO;
    if (level_s) {
        if (!strcmp(level_s, "warn") || !strcmp(level_s, "warning")) e.level = NOTIFY_WARN;
        else if (!strcmp(level_s, "crit") || !strcmp(level_s, "critical")) e.level = NOTIFY_CRIT;
    }
    e.unread = true;
    e.chosen = -1;
    e.opt_count = 0;
    e.ttl_s = 0;
    notify_copy_text(e.source, sizeof(e.source), source ? source : "mesh");
    notify_copy_text(e.title, sizeof(e.title), title ? title : "notify");
    notify_copy_text(e.body, sizeof(e.body), body ? body : "");
    if (key && key[0]) notify_copy_text(e.key, sizeof(e.key), key);

    time_t now = time(NULL);
    e.created_epoch = (now > TIME_VALID_EPOCH) ? now : 0;
    e.created_ms = millis();

    uint32_t replaced = 0;
    uint32_t id = notify_push(e, NULL, &replaced);
    if (id == 0) return 0;

    notify_arrived_id = id;
    notify_arrived = true;
    notify_ring_level = e.level;
    notify_ring_arrived = true;
    notify_request_sound(e.level, e.source);
    notify_mark_dirty(e.level == NOTIFY_CRIT ? 0 : NOTIFY_COALESCE_MS);
    display_force = true;
    event_add("notify %s: %s%s%s", notify_level_name(e.level),
              e.source[0] ? e.source : "", e.source[0] ? ": " : "", e.title);
    return id;
}

/*
 * Could this be a client key rather than the tail of a body?
 *
 * The alphabet is the one every key this pager is sent already uses —
 * "pingani-a1b2", "hermes-chat", "k1c.print" — and it deliberately excludes the
 * space, which is what keeps ordinary prose from qualifying. See
 * notify_ingest_p1() for why a guess is needed here at all.
 */
static bool notify_key_plausible(const char *s) {
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n >= NOTIFY_KEY_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':')
            continue;
        return false;
    }
    return true;
}

/* Wire format from home-rig MeshCore gateway:
 *   P1|<info|warn|crit>|<source>|<title>|<body>
 *   P1|<info|warn|crit>|<source>|<title>|<body>|<key>
 * body may contain '|'. Returns notify id or 0.
 *
 * The trailing key is the client id, and it is what a reply typed on the card
 * needs in order to be routed back to whoever asked — a card without one has
 * nobody upstream, on this transport as much as on the WiFi one.
 *
 * Recovering it means guessing, because the body in front of it is unescaped
 * and may hold separators of its own. The guess is the last '|' of the
 * remainder, taken only when what follows it could be a key at all
 * (notify_key_plausible). A five-field frame whose body happens to end in
 * "|something-like-this" is therefore read as six and loses that tail off the
 * body. That is the whole cost of the ambiguity, and it is paid in a few
 * characters of one message rather than in the routing of every reply.
 */
static uint32_t notify_ingest_p1(const char *wire) {
    if (!wire || strncmp(wire, "P1|", 3) != 0) return 0;
    const char *p = wire + 3;
    char level[8] = {0}, source[NOTIFY_SOURCE_LEN] = {0};
    char title[NOTIFY_TITLE_LEN] = {0};
    const char *body = "";

    const char *a = strchr(p, '|');
    if (!a) return 0;
    size_t n = (size_t)(a - p);
    if (n >= sizeof(level)) n = sizeof(level) - 1;
    memcpy(level, p, n);
    p = a + 1;

    a = strchr(p, '|');
    if (!a) return 0;
    n = (size_t)(a - p);
    if (n >= sizeof(source)) n = sizeof(source) - 1;
    memcpy(source, p, n);
    p = a + 1;

    a = strchr(p, '|');
    if (!a) return 0;
    n = (size_t)(a - p);
    if (n >= sizeof(title)) n = sizeof(title) - 1;
    memcpy(title, p, n);
    body = a + 1;

    /* Split the optional trailing key off the body. The body copy only happens
       on that path; a five-field frame keeps pointing straight into `wire`. */
    char body_buf[NOTIFY_BODY_LEN] = {0};
    char key[NOTIFY_KEY_LEN] = {0};
    const char *last = strrchr(body, '|');
    if (last && notify_key_plausible(last + 1)) {
        size_t bn = (size_t)(last - body);
        if (bn >= sizeof(body_buf)) bn = sizeof(body_buf) - 1;
        /* Back off a cut that landed inside a UTF-8 sequence: notify_copy_text()
           can only keep the boundary honest on text it is handed whole. */
        while (bn > 0 && ((unsigned char)body[bn] & 0xC0) == 0x80) bn--;
        memcpy(body_buf, body, bn);
        snprintf(key, sizeof(key), "%s", last + 1);
        body = body_buf;
    }

    return notify_ingest(level, source, title, body, key[0] ? key : NULL);
}

/* --- Endpoints --- */

static const SkillEndpoint notify_endpoints[] = {
    {"POST", "/notify",     "Queue a notification {level, title, body, source, ttl_s, id, options}"},
    {"GET",  "/notify",     "List notifications newest first (?unread=1) plus the unread count"},
    {"GET",  "/notify/one", "One notification by id (?id=N), with its options and the reply"},
    {"POST", "/notify/ack", "Mark one read {\"id\":N} or all of them {\"all\":true}"},
    {NULL, NULL, NULL}
};

static const char *notify_describe() {
    return "## Skill: notify\n\n"
           "A pager. Anything that can run curl can put a message on the\n"
           "device screen; the knob reads it and acknowledges it.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /notify | `{\"level\":\"info\"\\|\"warn\"\\|\"crit\",\"title\":\"...\",\"body\":\"...\",\"source\":\"home-rig\",\"ttl_s\":3600,\"id\":\"backup\",\"options\":[\"Yes\",\"No\"]}` |\n"
           "| GET | /notify | `{\"unread\":N,\"count\":M,\"notifications\":[...]}`, newest first; `?unread=1` lists only unread |\n"
           "| GET | /notify/one | one entry by numeric id: `/notify/one?id=12`, 404 if it is gone |\n"
           "| POST | /notify/ack | `{\"id\":12}` or `{\"all\":true}` |\n\n"
           "### Fields\n\n"
           "`title` is the only required one. `level` defaults to `info` and\n"
           "decides the colour on screen. `source` is a short label for who\n"
           "sent it. `ttl_s` drops the entry silently once it is that many\n"
           "seconds old, 0 (the default) never expires, and an unread `crit`\n"
           "ignores its ttl until it has been acknowledged. `id` is a client\n"
           "key, not the numeric id: posting again with the same key replaces\n"
           "the earlier message instead of queueing a second one, so a job that\n"
           "reports progress leaves one entry rather than fifty.\n\n"
           "Lengths are capped and longer values are truncated, not rejected:\n"
           "title 40, body 96, source 16, id 24, and each option label 15 —\n"
           "bytes, which is characters for ASCII and fewer for anything else.\n"
           "The cut lands on a character boundary, so a multi-byte one is\n"
           "dropped whole rather than left half-written in the JSON.\n\n"
           "### Asking a question\n\n"
           "`options` is up to four short labels. The knob picks one and the\n"
           "index is stored on the device — nothing is sent anywhere, so ask,\n"
           "then come back and read `chosen` with `GET /notify/one?id=N`. It is\n"
           "-1 until somebody answers, and both `options` and `chosen` are\n"
           "absent from an entry that carries no question at all.\n\n"
           "Three labels fit the screen comfortably; four are tight, about\n"
           "seven capitals each, so `Yes`/`No`/`Later` reads well and\n"
           "`Restart now` does not — and a label over 15 bytes is cut like any\n"
           "other field, so `Deploy to production` is stored as `Deploy to\n"
           "produ`. The panel then puts it in capitals and cuts it again to\n"
           "whatever share of the row it gets, which depends on how many\n"
           "options there are: what is stored is what the API serves back, not\n"
           "what appears on screen. Labels are printable ASCII only (32..126):\n"
           "anything else is refused with a reason, because the panel has no\n"
           "glyphs for it and would draw a message nobody can read.\n\n"
           "Posting again under the same `id` key clears any answer along with\n"
           "the message it belonged to. A re-post is a new question, and\n"
           "carrying the old reply onto it would be worse than losing it.\n"
           "Answering also marks the entry read.\n\n"
           "### Behaviour\n\n"
           "Twenty entries are held in RAM and the newest six survive a reboot.\n"
           "A full queue drops the oldest read entry first, and only ever drops\n"
           "an unread critical when every slot holds one.\n\n"
           "Nothing here blocks: a POST is a struct copy, and the flash write\n"
           "that mirrors the queue happens on the main loop a few seconds\n"
           "later — immediately for a `crit`, which may be warning about the\n"
           "very reboot that would lose it.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"level\":\"crit\",\"source\":\"home-rig\",\"title\":\"RAID degraded\"}' \\\n"
           "  http://seed.local:8080/notify\n"
           "\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -H 'Content-Type: application/json' \\\n"
           "  -d '{\"source\":\"claude\",\"title\":\"Deploy to prod?\",\"options\":[\"Yes\",\"No\",\"Later\"]}' \\\n"
           "  http://seed.local:8080/notify\n"
           "curl -H \"Authorization: Bearer $TOKEN\" http://seed.local:8080/notify/one?id=12\n"
           "```\n";
}

/* Bodies are collected by handle_body_collect() into _tempObject and belong to
   the handler from then on, including on the unauthenticated path — hence
   check_auth() rather than require_auth()'s early return. */
static char *notify_take_body(AsyncWebServerRequest *req) {
    char *body = (char*)req->_tempObject;
    req->_tempObject = nullptr;
    return body;
}

static void notify_send_json(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

/*
 * One entry as the API renders it, shared by the list and the single-entry
 * read so the two cannot drift into describing the same message differently.
 *
 * `v` is NON-CONST for the reason spelled out at notify_snapshot_store():
 * ArduinoJson keeps a `const char *` by pointer and copies a `char *`, and this
 * view is a stack local that is reused on the next pass of the caller's loop.
 *
 * `options` and `chosen` appear only on an entry that has options, so their
 * absence means "this message asks nothing" rather than "no reply yet" — which
 * is what -1 means, and only ever appears where there is something to choose.
 */
static void notify_entry_json(JsonObject o, NotifyView &v) {
    o["id"] = v.id;
    o["level"] = notify_level_name(v.level);
    o["unread"] = v.unread;
    o["age_s"] = v.age_s;
    if (v.source[0]) o["source"] = v.source;
    o["title"] = v.title;
    if (v.body[0]) o["body"] = v.body;
    if (v.opt_count > 0) {
        JsonArray a = o["options"].to<JsonArray>();
        for (uint8_t i = 0; i < v.opt_count && i < NOTIFY_OPT_MAX; i++)
            a.add(v.options.label[i]);
        o["chosen"] = v.chosen;
    }
    if (v.reply[0]) o["reply"] = v.reply;
}

static void notify_send_error(AsyncWebServerRequest *req, int code, const char *msg) {
    JsonDocument doc;
    doc["error"] = msg;
    notify_send_json(req, code, doc);
}

/*
 * The numeric id out of ?id=, or false with nothing written.
 *
 * Digits and nothing else, counted by hand, because every library parser here
 * is more generous than the query string is allowed to be. strtoul() skips
 * leading whitespace, accepts a leading '+', and turns a leading '-' into the
 * unsigned negation — so "-1" arrives as 4294967295 and asks the store for a
 * notification that cannot exist, which answers 404 to what is plainly a
 * malformed request. It also reports overflow through errno rather than in its
 * return value, and a 33-digit id truncated into uint32_t would not be refused
 * at all: it would fetch some other entry and look like a successful read.
 *
 * The ceiling is NOTIFY_ID_MAX, which is the largest id this store ever hands
 * out, and it is checked on every digit so the accumulator cannot wrap on the
 * way to being compared against it. Refusing at the store's own ceiling rather
 * than at the width of the type is what makes `?id=4294967295` a 400 — a value
 * no notification can carry is a malformed request, not a lookup that missed.
 * 0 is refused with it, for the same reason: it is what marks a slot free.
 *
 * Sliced onto the host by tools/test_notify_options.sh; the marker rules from
 * the types region apply.
 */
/* host-test:begin id — sliced out by tools/test_notify_options.sh */
static bool notify_parse_id(const char *s, uint32_t &out) {
    if (!s || !s[0]) return false;

    unsigned long long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long long)(*p - '0');
        if (v > (unsigned long long)NOTIFY_ID_MAX) return false;
    }
    if (v == 0) return false;

    out = (uint32_t)v;
    return true;
}
/* host-test:end */

static void notify_register_routes(AsyncWebServer &server) {

    /* POST /notify — exact matching, or this route also answers /notify/ack
       and every acknowledgement queues a message instead. */
    server.on(AsyncURIMatcher::exact("/notify"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON with at least a title");
            return;
        }

        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        Notification e;
        NotifyOptions opts;
        memset(&e, 0, sizeof(e));
        memset(&opts, 0, sizeof(opts));
        e.level = NOTIFY_INFO;
        e.unread = true;
        e.chosen = -1;   /* memset made it 0, which would read as "picked the first" */

        /* A field of the wrong type is a caller bug, and answering it with a
           silent default is how that bug survives to production. Absent is
           fine; present and wrong is a 400. */
        if (!input["level"].isNull() &&
            (!input["level"].is<const char*>() ||
             !notify_level_parse(input["level"].as<const char*>(), e.level))) {
            notify_send_error(req, 400, "level must be info, warn or crit");
            return;
        }
        if (!input["title"].is<const char*>()) {
            notify_send_error(req, 400, "title is required and must be a string");
            return;
        }
        notify_copy_text(e.title, sizeof(e.title), input["title"].as<const char*>());
        if (e.title[0] == '\0') {
            notify_send_error(req, 400, "title must not be empty");
            return;
        }
        if (input["body"].is<const char*>())
            notify_copy_text(e.body, sizeof(e.body), input["body"].as<const char*>());
        if (input["source"].is<const char*>())
            notify_copy_text(e.source, sizeof(e.source), input["source"].as<const char*>());
        if (input["id"].is<const char*>())
            notify_copy_text(e.key, sizeof(e.key), input["id"].as<const char*>());
        if (!input["ttl_s"].isNull()) {
            /* is<unsigned long>() is false for a negative number as well as
               for a string, which is exactly the set that should be rejected
               rather than quietly turned into "never expires". */
            if (!input["ttl_s"].is<unsigned long>() ||
                input["ttl_s"].as<unsigned long>() > NOTIFY_TTL_MAX) {
                notify_send_error(req, 400, "ttl_s must be 0 (never) to 2592000");
                return;
            }
            e.ttl_s = (uint32_t)input["ttl_s"].as<unsigned long>();
        }
        if (!input["options"].isNull()) {
            /* Same rule as every other field here: absent is fine, present and
               wrong is a 400. A caller that asked a question and got a plain
               notification back would never find out. */
            if (!input["options"].is<JsonArrayConst>()) {
                notify_send_error(req, 400, NOTIFY_OPT_ERR);
                return;
            }
            JsonArrayConst a = input["options"];
            size_t n = a.size();
            const char *labels[NOTIFY_OPT_MAX];
            for (size_t i = 0; i < n && i < NOTIFY_OPT_MAX; i++) {
                if (!a[i].is<const char *>()) {
                    notify_send_error(req, 400, "each option must be a string");
                    return;
                }
                labels[i] = a[i].as<const char *>();
            }
            /* A count over the maximum is refused inside, before the labels are
               read — which is why only the ones that fit were collected. */
            const char *why = NULL;
            if (!notify_options_build(opts, e.opt_count, labels, n, &why)) {
                notify_send_error(req, 400, why);
                return;
            }
        }

        /* Both clocks are read before the lock: time() must not be called
           inside a critical section, and one timestamp for the whole entry is
           what makes its age self-consistent. */
        time_t now = time(NULL);
        e.created_epoch = (now > TIME_VALID_EPOCH) ? now : 0;
        e.created_ms = millis();

        uint32_t replaced = 0;
        uint32_t id = notify_push(e, e.opt_count ? &opts : NULL, &replaced);
        if (id == 0) {
            notify_send_error(req, 500, "queue full");
            return;
        }

        /* Staged, not acted on: loop() owns every screen change, and the flash
           write happens there too. A critical message is written on the next
           pass rather than after the coalescing window, because the reboot it
           is reporting may not wait. */
        notify_arrived_id = id;
        notify_arrived = true;
        notify_ring_level = e.level;
        notify_ring_arrived = true;
        notify_request_sound(e.level, e.source);
        notify_mark_dirty(e.level == NOTIFY_CRIT ? 0 : NOTIFY_COALESCE_MS);
        display_force = true;

        event_add("notify %s: %s%s%s", notify_level_name(e.level),
                  e.source[0] ? e.source : "", e.source[0] ? ": " : "", e.title);

        JsonDocument doc;
        doc["ok"] = true;
        doc["id"] = id;
        doc["level"] = notify_level_name(e.level);
        if (replaced) doc["replaced"] = replaced;
        doc["unread"] = notify_unread_count();
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);

    /* GET /notify?unread=1 */
    server.on(AsyncURIMatcher::exact("/notify"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        bool only_unread = false;
        if (req->hasParam("unread")) {
            String v = req->getParam("unread")->value();
            only_unread = (v != "0" && v != "false");
        }

        JsonDocument doc;
        doc["unread"] = notify_unread_count();
        doc["count"] = notify_count();
        doc["capacity"] = NOTIFY_MAX;
        JsonArray arr = doc["notifications"].to<JsonArray>();

        /* One entry at a time through the same accessor the screen uses, so
           the web server task never holds the lock for longer than a copy. */
        for (int i = 0; i < NOTIFY_MAX; i++) {
            NotifyView v;
            if (!notify_view(i, v)) break;
            if (only_unread && !v.unread) continue;
            notify_entry_json(arr.add<JsonObject>(), v);
        }

        notify_send_json(req, 200, doc);
    });

    /*
     * GET /notify/one?id=N — one entry, and nothing else.
     *
     * The list endpoint builds and serialises the whole queue, which is several
     * kilobytes of JSON for a caller that wants one field of one message. That
     * caller is the point of the reply options: something that posted a
     * question and is now waiting on the line for the knob to answer it, asking
     * again every few seconds. This is what it should be asking.
     *
     * Registered under /notify with exact(), so it is a sibling of the list
     * rather than something the list can swallow — the failure that once made
     * POST /ir/tvbgone answer /ir/tvbgone/stop.
     */
    server.on(AsyncURIMatcher::exact("/notify/one"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;

        if (!req->hasParam("id")) {
            notify_send_error(req, 400, "id is required: /notify/one?id=N");
            return;
        }
        /* Parsed rather than handed to toInt(): a typo must come back as a 400
           with a reason, not as a silent lookup of notification 0 answering
           404 — which is what toInt() returns for "twelve", for "" and for
           "12x" alike. See notify_parse_id() for what it refuses and why. */
        uint32_t id = 0;
        if (!notify_parse_id(req->getParam("id")->value().c_str(), id)) {
            notify_send_error(req, 400, "id must be a positive number");
            return;
        }

        NotifyView v;
        if (!notify_view_by_id(id, v, NULL, NULL)) {
            notify_send_error(req, 404, "no notification with that id");
            return;
        }

        JsonDocument doc;
        notify_entry_json(doc.to<JsonObject>(), v);
        notify_send_json(req, 200, doc);
    });

    /* POST /notify/ack — {"id":N} or {"all":true} */
    server.on(AsyncURIMatcher::exact("/notify/ack"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be {\"id\":N} or {\"all\":true}");
            return;
        }

        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        JsonDocument doc;
        if (input["all"].is<bool>() && input["all"].as<bool>()) {
            doc["acked"] = notify_ack_all();
        } else if (input["id"].is<unsigned long>()) {
            uint32_t id = (uint32_t)input["id"].as<unsigned long>();
            if (!notify_ack_id(id)) {
                notify_send_error(req, 404, "no notification with that id");
                return;
            }
            doc["acked"] = 1;
            doc["id"] = id;
        } else {
            notify_send_error(req, 400, "body must be {\"id\":N} or {\"all\":true}");
            return;
        }

        doc["ok"] = true;
        doc["unread"] = notify_unread_count();
        notify_send_json(req, 200, doc);
    }, NULL, handle_body_collect);
}

static const Skill notify_skill = {
    .name = "notify",
    .version = "0.2.0",
    .describe = notify_describe,
    .endpoints = notify_endpoints,
    .register_routes = notify_register_routes,
    // Order-free (C8): expiry raises display_force (and the coalesced save
    // flag), which the loop consumes on its next pass — a one-pass repaint
    // deferral, benign. Arrival/sound flags are produced by the HTTP handlers
    // and mesh ingest, not by this poll.
    .tick = notify_poll
};

static void skill_notify_init() {
    memset(notify_slot, 0, sizeof(notify_slot));
    memset(notify_opt, 0, sizeof(notify_opt));
    memset(notify_reply, 0, sizeof(notify_reply));
    notify_load();
    skill_register(&notify_skill);
}
