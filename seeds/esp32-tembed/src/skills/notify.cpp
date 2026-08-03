/*
 * skills/notify.cpp — notification queue: the seed as a pager
 *
 * Anything that can speak HTTP can poke this device. A one-line curl leaves a
 * message; the knob reads it and acknowledges it.
 *
 * Endpoints:
 *   POST /notify      — queue one {level, title, body, source, ttl_s, id}
 *   GET  /notify      — list, newest first, plus the unread count (?unread=1)
 *   POST /notify/ack  — mark one read {"id":N} or all of them {"all":true}
 *
 * All three are registered with AsyncURIMatcher::exact(). The library's default
 * matches ^{uri}(/.*)?$, under which /notify would also answer /notify/ack and,
 * being registered first, would silently swallow every acknowledgement. That
 * exact failure already cost this firmware a working POST /ir/tvbgone/stop.
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
 * NOTIFY_MAX * sizeof(Notification) is the whole cost of this skill.
 */
#define NOTIFY_MAX          20
#define NOTIFY_PERSIST       6
#define NOTIFY_SOURCE_LEN   17   /* 16 chars, e.g. "home-rig", "k1c" */
#define NOTIFY_TITLE_LEN    41   /* 40 chars */
#define NOTIFY_BODY_LEN     97   /* 96 chars, two ellipsised lines on screen */
#define NOTIFY_KEY_LEN      25   /* 24 chars of client-supplied dedup key */
#define NOTIFY_FILE         "/notify.json"
/* The snapshot is written here and renamed into place; see notify_save(). */
#define NOTIFY_FILE_TMP     "/notify.tmp"
/* A month. Longer than this is indistinguishable from "no expiry", which is
   what ttl_s 0 already means. */
#define NOTIFY_TTL_MAX      (30UL * 24UL * 3600UL)
#define NOTIFY_COALESCE_MS  3000

enum { NOTIFY_INFO = 0, NOTIFY_WARN, NOTIFY_CRIT };

struct Notification {
    uint32_t id;              /* 0 marks a free slot */
    uint32_t ttl_s;           /* 0 = never expires */
    unsigned long created_ms;
    time_t   created_epoch;   /* 0 when the clock was unset at arrival */
    uint8_t  level;
    bool     unread;
    char source[NOTIFY_SOURCE_LEN];
    char title[NOTIFY_TITLE_LEN];
    char body[NOTIFY_BODY_LEN];
    char key[NOTIFY_KEY_LEN];
};

/* What the screen gets. A copy, not a pointer into the store: the store is
   under a lock and the panel takes milliseconds to draw. */
struct NotifyView {
    uint32_t id;
    uint8_t  level;
    bool     unread;
    unsigned long age_s;
    char source[NOTIFY_SOURCE_LEN];
    char title[NOTIFY_TITLE_LEN];
    char body[NOTIFY_BODY_LEN];
};

static Notification notify_slot[NOTIFY_MAX];
static uint8_t notify_order[NOTIFY_MAX];  /* slot indices, newest first */
static uint8_t notify_len = 0;
static uint32_t notify_next_id = 1;

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
 */
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
    notify_slot[notify_order[pos]].id = 0;
    for (int i = pos; i + 1 < notify_len; i++) notify_order[i] = notify_order[i + 1];
    notify_len--;
}

static int notify_free_slot() {
    for (int i = 0; i < NOTIFY_MAX; i++) if (notify_slot[i].id == 0) return i;
    return -1;
}

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
 * `e` arrives fully built — including its timestamps — so that the critical
 * section is one struct copy and two array shuffles.
 */
static uint32_t notify_push(Notification &e, uint32_t *replaced_out) {
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

    e.id = notify_next_id++;
    notify_slot[slot] = e;
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

/* The copy itself, with the lock already held: three memcpys and the age
   arithmetic, both clocks having been read before the caller took it. */
static void notify_fill_view(const Notification &e, NotifyView &out, time_t now,
                             unsigned long now_ms) {
    out.id = e.id;
    out.level = e.level;
    out.unread = e.unread;
    out.age_s = notify_age_of(e, now, now_ms);
    memcpy(out.source, e.source, sizeof(out.source));
    memcpy(out.title, e.title, sizeof(out.title));
    memcpy(out.body, e.body, sizeof(out.body));
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
    notify_fill_view(notify_slot[notify_order[index]], out, now, now_ms);
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
        const Notification &e = notify_slot[notify_order[i]];
        if (e.id != id) continue;
        notify_fill_view(e, out, now, now_ms);
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

/* --- Persistence --- */

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
        portENTER_CRITICAL(&notify_mux);
        bool ok = (i < notify_len);
        if (ok) e = notify_slot[notify_order[i]];
        portEXIT_CRITICAL(&notify_mux);
        if (!ok) break;

        JsonObject o = arr.add<JsonObject>();
        o["id"] = e.id;
        o["lv"] = e.level;
        o["ur"] = e.unread;
        o["tt"] = e.ttl_s;
        /* An entry that arrived before the first NTP sync has no epoch of its
           own. Stamping it with the current one on the way out is the best
           available answer and beats writing a zero that reads as "just now"
           forever. */
        o["ts"] = (uint32_t)(e.created_epoch > TIME_VALID_EPOCH ? e.created_epoch
                             : (now > TIME_VALID_EPOCH ? now : 0));
        if (e.source[0]) o["sr"] = e.source;
        o["ti"] = e.title;
        if (e.body[0]) o["bd"] = e.body;
        if (e.key[0])  o["ky"] = e.key;
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

    time_t now = time(NULL);
    unsigned long now_ms = millis();
    int restored = 0;

    /* The file is newest-first, so appending to the tail rebuilds the order as
       it was. Nothing else runs yet — this is called from setup() — so the
       slots are filled directly instead of going through notify_push(), which
       would issue new ids and reverse the list. */
    for (JsonObject o : doc["n"].as<JsonArray>()) {
        if (notify_len >= NOTIFY_MAX) break;
        uint32_t id = o["id"] | 0u;
        if (id == 0 || !o["ti"].is<const char*>()) continue;

        bool dup = false;
        for (int i = 0; i < notify_len && !dup; i++)
            dup = (notify_slot[notify_order[i]].id == id);
        if (dup) continue;

        int slot = notify_free_slot();
        if (slot < 0) break;
        Notification &e = notify_slot[slot];
        memset(&e, 0, sizeof(e));
        e.id = id;
        e.level = (uint8_t)(o["lv"] | 0);
        if (e.level > NOTIFY_CRIT) e.level = NOTIFY_INFO;
        e.unread = o["ur"] | false;
        /* The same ceiling the API enforces. A restored entry is not more
           trusted than a posted one — this file can be replaced wholesale with
           a filesystem image — and a ttl past the cap says "never expires",
           which is what ttl_s 0 already spells. */
        e.ttl_s = o["tt"] | 0u;
        if (e.ttl_s > NOTIFY_TTL_MAX) e.ttl_s = NOTIFY_TTL_MAX;
        e.created_epoch = (time_t)(uint32_t)(o["ts"] | 0u);
        /* Wind millis() back by however long the entry has really been alive.
           The subtraction is meant to go negative and wrap: notify_age_of()
           reads it back with the same unsigned arithmetic. */
        unsigned long elapsed = 0;
        if (e.created_epoch > TIME_VALID_EPOCH && now > e.created_epoch)
            elapsed = (unsigned long)(now - e.created_epoch) * 1000UL;
        e.created_ms = now_ms - elapsed;
        /* Through the same copy the API path uses, for the same reason: these
           strings go back out over GET /notify, and a stored file is not a
           more trustworthy source of UTF-8 than a POST body. */
        notify_copy_text(e.source, sizeof(e.source), o["sr"] | "");
        notify_copy_text(e.title, sizeof(e.title), o["ti"] | "");
        notify_copy_text(e.body, sizeof(e.body), o["bd"] | "");
        notify_copy_text(e.key, sizeof(e.key), o["ky"] | "");

        notify_order[notify_len++] = (uint8_t)slot;
        if (id >= notify_next_id) notify_next_id = id + 1;
        restored++;
    }

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

/* --- Endpoints --- */

static const SkillEndpoint notify_endpoints[] = {
    {"POST", "/notify",     "Queue a notification {level, title, body, source, ttl_s, id}"},
    {"GET",  "/notify",     "List notifications newest first (?unread=1) plus the unread count"},
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
           "| POST | /notify | `{\"level\":\"info\"\\|\"warn\"\\|\"crit\",\"title\":\"...\",\"body\":\"...\",\"source\":\"home-rig\",\"ttl_s\":3600,\"id\":\"backup\"}` |\n"
           "| GET | /notify | `{\"unread\":N,\"count\":M,\"notifications\":[...]}`, newest first; `?unread=1` lists only unread |\n"
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
           "title 40, body 96, source 16, id 24 — bytes, which is characters\n"
           "for ASCII and fewer for anything else. The cut lands on a character\n"
           "boundary, so a multi-byte one is dropped whole rather than left\n"
           "half-written in the JSON.\n\n"
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

static void notify_send_error(AsyncWebServerRequest *req, int code, const char *msg) {
    JsonDocument doc;
    doc["error"] = msg;
    notify_send_json(req, code, doc);
}

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
        memset(&e, 0, sizeof(e));
        e.level = NOTIFY_INFO;
        e.unread = true;

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

        /* Both clocks are read before the lock: time() must not be called
           inside a critical section, and one timestamp for the whole entry is
           what makes its age self-consistent. */
        time_t now = time(NULL);
        e.created_epoch = (now > TIME_VALID_EPOCH) ? now : 0;
        e.created_ms = millis();

        uint32_t replaced = 0;
        uint32_t id = notify_push(e, &replaced);
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
            JsonObject o = arr.add<JsonObject>();
            o["id"] = v.id;
            o["level"] = notify_level_name(v.level);
            o["unread"] = v.unread;
            o["age_s"] = v.age_s;
            if (v.source[0]) o["source"] = v.source;
            o["title"] = v.title;
            if (v.body[0]) o["body"] = v.body;
        }

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
    .version = "0.1.0",
    .describe = notify_describe,
    .endpoints = notify_endpoints,
    .register_routes = notify_register_routes
};

static void skill_notify_init() {
    memset(notify_slot, 0, sizeof(notify_slot));
    notify_load();
    skill_register(&notify_skill);
}
