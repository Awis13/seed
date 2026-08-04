/*
 * voice.cpp — the pager's first OUTBOUND request.
 *
 * Everything this device has ever done on the network, it did because somebody
 * asked. It listens on port 8080, answers, and goes back to waiting. mic.cpp
 * gave it an ear but kept that shape: a take sits in PSRAM until a client comes
 * and fetches it. This file inverts the arrow. Hold the knob, talk, and the
 * recording goes somewhere by itself.
 *
 * That inversion is the whole feature, and it is why the code below is more
 * careful than "POST some bytes" sounds. An inbound request arrives on a task
 * the framework owns, with a timeout the framework enforces, and the worst a
 * handler can do is answer slowly. An outbound request is ours: our socket, our
 * DNS lookup, our timeout, our deadlock — and it happens while the microphone's
 * only buffer is being read out from under a device that is also drawing a
 * screen, driving an IR state machine and feeding an audio DMA.
 *
 *
 * Why a dedicated task, and why not the two obvious alternatives
 * -------------------------------------------------------------
 * The IDF's `esp_http_client` is a blocking client. There is no way to make a
 * blocking client not block; there is only a choice about WHAT it blocks.
 *
 *   On the loop task, it blocks everything. loop() idles at delay(10), its
 *   worst pass is a ~22ms full-screen repaint, snd_poll()'s entire I2S underrun
 *   margin is 112-128ms, ring_poll() wants its frame at 40Hz and ir_poll()
 *   advances an RMT machine that a blast depends on. A single connect() to an
 *   unreachable host is seconds. That is not a slow upload, it is a frozen
 *   device.
 *
 *   On AsyncTCP as a CLIENT — the dependency is already here, and it is
 *   genuinely non-blocking — it would take a hand-rolled state machine across
 *   six callbacks, a windowed send (tcp_sndbuf is 5744 bytes, so 320KB is about
 *   57 rounds), an HTTP response parser and delete-safety on every path. And
 *   then the thing that settles it: AsyncTCP's DNS is uncancellable.
 *   connect(const char*, port) hands `this` to dns_gethostbyname and there is
 *   no dns_cancel anywhere in the library, so a client deleted during a lookup
 *   is a use-after-free that lands seconds later, on the lwIP task, pointing at
 *   freed memory. A correctness argument we would have to make ourselves, for a
 *   transfer that is allowed to take its time.
 *
 * A dedicated FreeRTOS task blocks only itself, and the SCHEDULER guarantees
 * that rather than our own reasoning about it. That is the entire argument.
 *
 * Parameters, and where they come from. Arduino's loopTask is core 1, priority
 * 1, 8192 bytes. async_tcp is core 1, priority 10, 16384 bytes (the build flags
 * in platformio.ini say so). WiFi and lwIP live on core 0. So this task is
 * pinned to CORE 0 — off the core that draws the screen and runs the web server
 * — at priority 2, which is above idle and below every system task, so it can
 * never starve the stack it depends on.
 *
 * The stack figure is a GUESS. 8192 bytes is loopTask's, and no primary source
 * gives a stack requirement for esp_http_client. So it is not left as a guess:
 * uxTaskGetStackHighWaterMark() is read at the end of every upload and reported
 * as `stack_free` in GET /voice, in bytes (ESP-IDF's FreeRTOS counts this stack
 * in bytes, not words, at both ends). Size it from that number after a few real
 * uploads rather than from this comment.
 *
 *
 * The safety contract — the part that actually matters
 * ---------------------------------------------------
 * mic.cpp's buffer is 320KB of PSRAM allocated once at init and never freed or
 * resized. GET /mic/last is safe because it SNAPSHOTS the take length once and
 * derives every offset from that local. This file inherits the contract exactly,
 * and tightens one part of it:
 *
 *   1. The hold is taken BEFORE the length is read, not after. mic_rec_start()
 *      declines while the hold count is non-zero, so once we hold it no new take
 *      can begin; a take that began in the window before is caught by the second
 *      recording check. Only then is `len` read, into a local, once. Every
 *      offset and the Content-Length come from that local and nothing re-reads
 *      mic_take_len for the rest of the upload.
 *
 *   2. The 44-byte WAV header is built from the same local, on this task's own
 *      stack.
 *
 *   3. mic_buf needs no snapshot because the allocation is fixed for the life of
 *      the boot (mic.cpp's skill_mic_init()). THAT INVARIANT IS LOAD-BEARING. If
 *      the capture buffer is ever made resizable or freed, the analysis here
 *      collapses and this file has to be rewritten, not adjusted.
 *
 *   4. The hold is re-stamped on every slice. mic_poll()'s watchdog releases the
 *      buffer after MIC_SERVE_STALL_MS without progress, and without re-stamping
 *      it would fire in the middle of a perfectly healthy upload.
 *
 *   5. The hold is released on EVERY exit path — success, HTTP error, socket
 *      error, deadline, and every refusal that happens after it was taken.
 *
 * The guarantee this buys, stated precisely: a fixed allocation plus a
 * snapshotted length means a lost race can garble a take but CANNOT read outside
 * the buffer. Compute an offset from a freshly-read mic_take_len, or send
 * MIC_MAX_BYTES instead of `len`, and the guarantee is gone.
 *
 *
 * The deadline, and why there are two of them
 * -------------------------------------------
 * A wedged receiver must not hold the microphone hostage for the life of the
 * boot — the same reasoning that produced the OTA watchdog in loop() and the
 * upload watchdog in sound.cpp. So there is a socket timeout on every operation
 * AND a hard total deadline across the whole upload, checked between slices. The
 * true worst case is therefore the deadline plus one socket timeout, which is
 * bounded, which is the point.
 *
 *
 * Body format
 * -----------
 * `audio/wav`: the canonical 44-byte header mic.cpp already writes, followed by
 * the PCM, streamed straight out of PSRAM in 4KB slices with no copy of the take
 * anywhere. The receiver does not exist yet, so this end chooses — and multipart
 * would need a boundary provably absent from binary PCM, plus a preamble and an
 * epilogue both measured before the connection opens, which is three more things
 * to get off by two for no gain at all.
 *
 *
 * What was NOT done, deliberately: WiFi modem sleep
 * ------------------------------------------------
 * Modem sleep is ON. arduino-esp32 defaults to WIFI_PS_MIN_MODEM and this
 * firmware has never called WiFi.setSleep(), so the radio naps between DTIM
 * beacons and both throughput and ack jitter pay for it.
 *
 * WiFi.setSleep(false) is the lever, and it is not pulled here. It is a GLOBAL
 * radio setting, and pulling it would mean a second task reaching into the radio
 * while an async web server is serving, loop() is re-issuing WiFi.begin() every
 * 30 seconds and mDNS is answering — with a mandatory restore on every exit path
 * including the ones that fail. That is a real blast radius bought with no
 * measurement: nobody has yet seen this upload be too slow.
 *
 * So the measurement ships instead of the optimisation. `last_ms` and
 * `last_bytes` in GET /voice make the throughput a number. If that number is
 * bad, setSleep(false) around the upload is the first thing to try, and there
 * will then be a before and an after to compare it against.
 *
 *
 * The AP can vanish mid-send
 * --------------------------
 * ap_poll() retires the provisioning AP with softAPdisconnect() + WiFi.mode()
 * from the loop task, and loop() re-issues WiFi.begin() every 30 seconds while
 * the STA link is down. Both can happen while this task is inside a write. So
 * the link is checked before starting and refused if it is down, and a
 * mid-transfer socket error is treated as an ordinary failure with a clean
 * teardown. Nothing here assumes the interface it started on still exists.
 *
 *
 * Auto-upload is OFF by default
 * -----------------------------
 * A recording ending can fire an upload by itself, and that is the point of the
 * feature — but not yet. Until there is a receiver that has been seen to work,
 * every upload should be one somebody asked for, so that a failure is attached
 * to an action rather than discovered later in a counter. Turn it on with
 * POST /voice once the round trip is proven.
 */

#include <atomic>

#include "esp_http_client.h"

/* --- Configuration ---
 *
 * Same shape as the ring's and the speaker's: RAM is the truth, SPIFFS is a
 * mirror written by the loop task a beat later. */
#define VOICE_CFG_FILE     "/voice.json"
#define VOICE_CFG_TMP      "/voice.tmp"
/* Coalescing window. A client that sets a URL and a flag in two calls costs one
   flash write rather than two, and nothing is lost by the delay: the setting is
   already live in RAM the moment the endpoint returns. */
#define VOICE_CFG_DELAY_MS 1500

/* --- Transfer ---
 *
 * 4KB slices. PSRAM is quad-width at 80MHz — tens of megabytes a second — so it
 * is nowhere near the bottleneck against a 5744-byte TCP send window, and no
 * read-ahead is needed. The slice exists only so the loop reads in blocks rather
 * than byte-wise, and so the hold gets re-stamped and the deadline checked at a
 * sensible granularity: 80 times over a full ten-second take. */
#define VOICE_SLICE_BYTES 4096

/* Per-operation socket timeout: connect, each write, and the header read. Long
   enough that a congested link is not mistaken for a dead one, short enough that
   a dead one is noticed. */
#define VOICE_SOCKET_TIMEOUT_MS 8000

/* Hard ceiling on one whole upload, checked between slices. A full take is
   320KB; even at a miserable 30KB/s that is eleven seconds, so this is roughly
   three times the worst plausible transfer and an order of magnitude above a
   healthy one. What it really bounds is how long a wedged receiver can keep the
   microphone unusable. */
#define VOICE_DEADLINE_MS 30000

/* --- The task --- */
#define VOICE_TASK_CORE   0    /* off the core that draws and serves */
#define VOICE_TASK_PRIO   2    /* above idle, below every system task */
#define VOICE_TASK_STACK  8192 /* a guess; `stack_free` in GET /voice sizes it */
#define VOICE_TASK_NAME   "voice_up"

/* --- URL ---
 *
 * Everything from here to the end marker is compiled verbatim on the host by
 * tools/test_voice_url.sh. Keep the two markers on lines of their own. */
/* host-test:begin url — sliced out by tools/test_voice_url.sh */
#define VOICE_URL_LEN  128
#define VOICE_HOST_LEN  64
#define VOICE_PATH_LEN  96

struct VoiceUrl {
    char host[VOICE_HOST_LEN];
    char path[VOICE_PATH_LEN];   /* always starts with '/', query included */
    uint16_t port;
};

/* Lowercase ASCII prefix test. Schemes are case-insensitive per RFC 3986 and
   somebody will paste "HTTP://"; `prefix` must already be lowercase. Written out
   rather than pulled from <strings.h> so the host test compiles the same code
   the firmware does, with no platform header in between. */
static bool voice_ci_prefix(const char *s, const char *prefix) {
    for (size_t i = 0; prefix[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

/*
 * Split a target URL into the three fields the HTTP client is given.
 *
 * Why parse at all, when esp_http_client_init() takes a whole URL: because a
 * URL that is going to be persisted and used unattended should be REFUSED at
 * the endpoint that sets it, with a reason, rather than accepted and then found
 * to be unusable later by a background task nobody is watching. And because
 * host, port and path are then reported in GET /voice, so a typo is visible as
 * a wrong field instead of as a connect error.
 *
 * The client is configured from host/port/path rather than from the raw string,
 * so there is exactly ONE parse that decides where the audio goes — this one.
 * Two parsers that agree today are two parsers that can disagree tomorrow.
 *
 * What is accepted:
 *   http://host                  -> path "/"      port 80
 *   http://host/                 -> path "/"
 *   http://host:9000/takes       -> port 9000
 *   http://192.168.1.50/takes    -> a bare IP is just a host
 *   http://seed-rx.local/takes   -> resolved by lwIP, which has mDNS queries on
 *   http://host/takes?node=a1b2  -> the query rides along in the path
 *
 * What is refused, each with its own message:
 *   no scheme, https (there is no certificate store in this build), a missing
 *   host, user:password, an IPv6 literal, a fragment, a bad or out-of-range
 *   port, anything over the length caps, and any byte outside printable ASCII.
 *
 * That last one is not tidiness. The path lands in an HTTP request line, so a
 * CR or an LF in it would be a request-splitting bug; refusing every byte below
 * 0x21 and above 0x7E closes that without having to reason about which control
 * character matters.
 *
 * The three length caps are independent and each has its own message, so it is
 * always clear which one was hit. The whole-url cap is the binding one when a
 * long host and a long path are combined — 63 + 95 does not fit in 127 — and
 * that is deliberate: the url is what somebody types, so that is where the
 * limit should be felt.
 *
 * `*err` is set on every false return and untouched on success. `out` is
 * cleared on entry and written only once every check has passed, so a refused
 * url leaves the caller's existing target intact rather than half-overwritten.
 */
static bool voice_url_parse(const char *url, VoiceUrl &out, const char **err) {
    const char *sink = NULL;
    if (!err) err = &sink;

    out.host[0] = '\0';
    out.path[0] = '\0';
    out.port = 0;

    if (!url) { *err = "no url"; return false; }

    size_t n = strlen(url);
    if (n == 0) { *err = "url is empty"; return false; }
    if (n >= VOICE_URL_LEN) { *err = "url is too long"; return false; }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c >= 0x7F) {
            *err = "url has a space or a non-printable character in it";
            return false;
        }
        if (c == '#') { *err = "a '#' fragment has no meaning in a request"; return false; }
    }

    if (!voice_ci_prefix(url, "http://")) {
        /* https gets its own answer rather than the generic one: it is a
           reasonable thing to try, and "must start with http://" would read as
           a typo rather than as a missing feature. */
        if (voice_ci_prefix(url, "https://")) {
            *err = "https is not supported: this build has no certificate store";
            return false;
        }
        *err = "url must start with http://";
        return false;
    }

    const char *p = url + 7;

    /* The authority runs to the first '/' or '?'. */
    const char *hend = p;
    while (*hend && *hend != '/' && *hend != '?') hend++;
    if (hend == p) { *err = "url has no host"; return false; }

    int colons = 0;
    const char *colon = NULL;
    for (const char *q = p; q < hend; q++) {
        if (*q == '@') { *err = "user:password in a url is not supported"; return false; }
        if (*q == '[' || *q == ']') { *err = "ipv6 literals are not supported"; return false; }
        if (*q == ':') { colons++; colon = q; }
    }
    if (colons > 1) { *err = "url has more than one ':' in the host"; return false; }

    const char *host_end = colon ? colon : hend;
    size_t hlen = (size_t)(host_end - p);
    if (hlen == 0) { *err = "url has no host"; return false; }
    if (hlen >= VOICE_HOST_LEN) { *err = "host is too long"; return false; }
    for (size_t i = 0; i < hlen; i++) {
        char c = p[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        if (!ok) {
            *err = "host may only contain letters, digits, '-', '.' and '_'";
            return false;
        }
    }

    uint16_t port = 80;
    if (colon) {
        if (colon + 1 == hend) { *err = "url has a ':' with no port after it"; return false; }
        unsigned long v = 0;
        for (const char *q = colon + 1; q < hend; q++) {
            if (*q < '0' || *q > '9') { *err = "port must be digits"; return false; }
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) { *err = "port must be 1..65535"; return false; }
        }
        if (v == 0) { *err = "port must be 1..65535"; return false; }
        port = (uint16_t)v;
    }

    /* Whatever is left is the request target, and it has to start with a slash.
       "http://host" has nothing left, and "http://host?a=1" has a query with no
       path — both become a path of their own rather than being handed to the
       client as something it would have to guess about. The synthesised slash
       counts against the cap, which is the sort of thing to get wrong once. */
    size_t rest = strlen(hend);
    bool add_slash = (rest == 0 || hend[0] != '/');
    if (rest + (add_slash ? 1u : 0u) >= VOICE_PATH_LEN) {
        *err = "path is too long";
        return false;
    }

    /* Everything has passed, so `out` can be written. Not one field before
       this line: a caller that keeps its previous target on a refusal would
       otherwise end up with a target nobody chose. */
    memcpy(out.host, p, hlen);
    out.host[hlen] = '\0';
    out.port = port;
    char *dst = out.path;
    if (add_slash) *dst++ = '/';
    memcpy(dst, hend, rest + 1);
    return true;
}
/* host-test:end */

/* --- Failure kinds ---
 *
 * Counted apart rather than as one `failures`, for the reason sound.cpp and
 * mic.cpp keep their sets: 0.6.0 shipped silent behind a healthy-looking
 * counter. "It did not upload" is not a diagnosis. "It attempted eleven times
 * and eleven of them were `offline`" is one, and it points somewhere completely
 * different from eleven `http`. */
enum {
    VOICE_OK = 0,
    VOICE_ERR_CONFIG,    /* no target configured */
    VOICE_ERR_OFFLINE,   /* the STA link was down before we started */
    VOICE_ERR_EMPTY,     /* nothing recorded to send */
    VOICE_ERR_BUSY,      /* a recording was running */
    VOICE_ERR_OPEN,      /* DNS, connect, or the request line never went out */
    VOICE_ERR_WRITE,     /* the socket died partway through the body */
    VOICE_ERR_TIMEOUT,   /* the total deadline expired */
    VOICE_ERR_HTTP,      /* it arrived and the far end said no */
    VOICE_ERR_COUNT
};

static const char *voice_err_names[] = {
    "ok", "config", "offline", "empty", "busy", "open", "write", "timeout", "http"
};
static_assert(sizeof(voice_err_names) / sizeof(voice_err_names[0]) == VOICE_ERR_COUNT,
              "voice_err_names[] must have one entry per failure kind, in enum order");

/* --- State ---
 *
 * Ownership, because there are now three tasks in this firmware:
 *
 *   voice_url / voice_target / voice_auto   written by the web server task,
 *                                           read by the uploader. Safe because a
 *                                           config change is refused while an
 *                                           upload is pending, and the uploader
 *                                           takes its own copy regardless.
 *   voice_pending                           the interlock itself, atomic.
 *   every counter below                     written by the uploader task only,
 *                                           read by the web server for JSON. Each
 *                                           is a single aligned scalar, so a
 *                                           reader sees an old value or a new
 *                                           one and never half of either.
 *   voice_cfg_dirty / voice_cfg_save_at     set by the web server, consumed by
 *                                           the loop task, which is the only
 *                                           task allowed to touch flash. */

static bool voice_ready = false;
static TaskHandle_t voice_task_h = NULL;

static char voice_url[VOICE_URL_LEN] = "";
static VoiceUrl voice_target;
static bool voice_auto = false;      /* upload when a take ends — OFF, see header */

static volatile bool voice_cfg_dirty = false;
static volatile unsigned long voice_cfg_save_at = 0;

/* The interlock. Set by whoever asks for an upload, cleared by the task when it
   has finished one. Atomic because the web server task and the loop task can
   both ask, and the compare-exchange is what makes "already in flight" a fact
   rather than a race between a read and a write. */
static std::atomic<bool> voice_pending{false};

/* Falling edge of "a take is running", for the auto-upload trigger. Loop task
   only. */
static bool voice_was_rec = false;

/* Counters. Uploader task only. */
static uint32_t voice_attempts = 0;
static uint32_t voice_uploads = 0;        /* attempts that ended 2xx */
/* Audio bytes, not body bytes: the 44-byte header is not counted, so these
   divide cleanly by 32000 into seconds and can be compared against `take_bytes`
   in GET /mic without an offset to remember. */
static uint64_t voice_bytes_sent = 0;     /* across the whole boot */
static uint32_t voice_fails[VOICE_ERR_COUNT] = {0};
static int voice_last_status = 0;         /* HTTP status of the last attempt */
static int voice_last_kind = VOICE_OK;
static uint32_t voice_last_ms = 0;        /* wall time of the last attempt */
static uint32_t voice_last_bytes = 0;     /* how far the last attempt got */
static uint32_t voice_stack_free = 0;     /* uxTaskGetStackHighWaterMark, bytes */
static char voice_last_err[96] = "";

/* --- Reporting a failure ---
 *
 * One place, so that no path can count a failure without leaving a reason
 * behind or leave a reason behind without counting it. */
static void voice_fail(int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(voice_last_err, sizeof(voice_last_err), fmt, ap);
    va_end(ap);
    if (kind <= VOICE_OK || kind >= VOICE_ERR_COUNT) kind = VOICE_OK;
    else voice_fails[kind]++;
    voice_last_kind = kind;
    /* The receiver's own words can end up in here, so every one of them goes
       through a %s argument and never through the format string itself. */
    event_add("voice: upload failed (%s): %s", voice_err_names[kind], voice_last_err);
}

/* --- One upload ---
 *
 * Runs on the uploader task and nowhere else. Everything it touches on another
 * task's behalf is named in the header comment; read that before changing this.
 *
 * Shape: the refusals return early, because at that point there is nothing to
 * tear down. From the moment the hold is taken there is exactly one exit, and
 * the body of the transfer sets `kind` and breaks out to it. A `do { } while (0)`
 * rather than a goto or a helper per step, so that the cleanup is visibly the
 * only way out and cannot be forgotten by the next person to add a step.
 */
static void voice_upload_run() {
    unsigned long started = millis();

    voice_attempts++;
    voice_last_status = 0;
    voice_last_bytes = 0;
    voice_last_ms = 0;

    /* A copy, taken once. A config change is refused while an upload is
       pending, so this cannot move under us — and it is copied anyway, because
       "cannot" and "does not" are different words. */
    VoiceUrl target = voice_target;

    if (voice_url[0] == '\0') {
        voice_fail(VOICE_ERR_CONFIG, "no target url configured");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        voice_fail(VOICE_ERR_OFFLINE, "wifi is not connected");
        return;
    }
    if (mic_is_recording()) {
        voice_fail(VOICE_ERR_BUSY, "a recording is in progress");
        return;
    }

    /* THE HOLD GOES FIRST, and the length is read after it.
     *
     * mic_rec_start() declines while the hold count is non-zero, so from here
     * no new take can begin and mic_take_len cannot be reset to zero under us.
     * The second recording check catches a take that started in the window
     * between the check above and this line. Only then is the length read —
     * once, into a local — and everything below derives from that local. */
    mic_serve_acquire(millis());

    if (mic_is_recording()) {
        mic_serve_release();
        voice_fail(VOICE_ERR_BUSY, "a recording started as the upload began");
        return;
    }

    const uint32_t len = mic_take_len;
    if (len == 0 || !mic_buf) {
        mic_serve_release();
        voice_fail(VOICE_ERR_EMPTY, "nothing recorded yet, hold the knob");
        return;
    }

    /* Built from the same snapshot, on this task's stack. mic_buf itself needs
       no snapshot: it is allocated once in skill_mic_init() and never freed or
       resized. That invariant is what makes the whole analysis hold — see the
       header comment before changing anything about that allocation. */
    uint8_t hdr[MIC_WAV_HDR_LEN];
    mic_wav_header(hdr, MIC_RATE, len);
    const int total = (int)MIC_WAV_HDR_LEN + (int)len;
    const unsigned long deadline = started + VOICE_DEADLINE_MS;

    esp_http_client_config_t cfg = {};
    cfg.host = target.host;
    cfg.port = (int)target.port;
    cfg.path = target.path;
    cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = VOICE_SOCKET_TIMEOUT_MS;
    cfg.user_agent = "seed-tembed/" SEED_VERSION;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        mic_serve_release();
        voice_fail(VOICE_ERR_OPEN, "http client could not be created");
        return;
    }

    int kind = VOICE_OK;
    bool opened = false;
    bool held = true;
    uint32_t off = 0;
    char aux[24];

    do {
        esp_http_client_set_header(cli, "Content-Type", "audio/wav");
        /* Two headers a receiver can file a take by without parsing anything:
           which node it came from, and how long it is. Both are already known
           here and neither costs a byte of the body. */
        esp_http_client_set_header(cli, "X-Seed-Node", mdns_name.c_str());
        snprintf(aux, sizeof(aux), "%lu", (unsigned long)mic_ms_for(len));
        esp_http_client_set_header(cli, "X-Seed-Take-Ms", aux);

        /* Declares the Content-Length and writes the headers. Everything from
           here is a socket that has to be closed. */
        esp_err_t err = esp_http_client_open(cli, total);
        if (err != ESP_OK) {
            kind = VOICE_ERR_OPEN;
            voice_fail(kind, "could not reach %s:%u (%s)", target.host,
                       (unsigned)target.port, esp_err_to_name(err));
            break;
        }
        opened = true;

        int w = esp_http_client_write(cli, (const char *)hdr, MIC_WAV_HDR_LEN);
        if (w != MIC_WAV_HDR_LEN) {
            kind = VOICE_ERR_WRITE;
            voice_fail(kind, "the wav header did not go out (wrote %d of %d)",
                       w, (int)MIC_WAV_HDR_LEN);
            break;
        }

        while (off < len) {
            uint32_t n = len - off;
            if (n > VOICE_SLICE_BYTES) n = VOICE_SLICE_BYTES;

            /* Straight out of PSRAM. No copy of the take is made anywhere in
               this file, which is the reason the client is driven in its
               open/write/fetch form rather than handed a buffer. */
            w = esp_http_client_write(cli, (const char *)(mic_buf + off), (int)n);
            if (w <= 0) {
                kind = VOICE_ERR_WRITE;
                voice_fail(kind, "socket closed after %lu of %lu bytes",
                           (unsigned long)off, (unsigned long)len);
                break;
            }
            off += (uint32_t)w;   /* a short write is ordinary, not an error */

            /* Or the 3-second watchdog in mic_poll() releases the buffer under
               a healthy upload and the next gesture overwrites what is being
               sent. Every slice, without exception. */
            mic_serve_at = millis();

            /* Signed, as every elapsed-time subtraction in this firmware is.
               Four defects have been spent on this one pattern; the cast costs
               nothing and one idiom everywhere is worth more than each site
               being argued about separately. */
            if ((long)(millis() - deadline) >= 0) {
                kind = VOICE_ERR_TIMEOUT;
                voice_fail(kind, "gave up after %ums with %lu of %lu bytes sent",
                           (unsigned)VOICE_DEADLINE_MS, (unsigned long)off,
                           (unsigned long)len);
                break;
            }

            /* One tick to the scheduler per slice — about 80ms across a full
               ten-second take, under three percent of even a fast upload.
               Insurance rather than a known need: this task sits above core 0's
               idle task, blocking writes normally yield on their own, and this
               makes "they did not, for five seconds" not be a task-watchdog
               reset. */
            vTaskDelay(1);
        }
        if (kind != VOICE_OK) break;

        /* The last byte of audio is out, so mic_buf is not read again and the
           hold goes back HERE rather than at the end of the function.
           Everything below — the response headers, the status line, the
           receiver's own words about why it said no — can each cost a socket
           timeout, and none of it touches the buffer. Holding the microphone
           through it would be holding it for nothing; worse, it would be held
           without being re-stamped, so mic_poll()'s watchdog would release it
           anyway, and a release of ours afterwards could take a newer reader's
           hold with it. */
        held = false;
        mic_serve_release();

        /* The body is out; read what the far end thought of it. */
        int64_t clen = esp_http_client_fetch_headers(cli);
        if (clen < 0) {
            kind = VOICE_ERR_HTTP;
            voice_fail(kind, "no response after the body went out");
            break;
        }
        voice_last_status = esp_http_client_get_status_code(cli);
        if (voice_last_status < 200 || voice_last_status > 299) {
            /* A little of the response body, because a receiver that refuses an
               upload usually says why, and that sentence is worth more than the
               status code on its own. Bounded by the buffer, never printed
               beyond what fits, and never used for anything but this message. */
            char body[64] = "";
            int got = esp_http_client_read(cli, body, (int)sizeof(body) - 1);
            if (got > 0) {
                body[got] = '\0';
                for (int i = 0; i < got; i++)
                    if (body[i] < 0x20 || body[i] > 0x7E) body[i] = ' ';
                voice_fail(VOICE_ERR_HTTP, "http %d: %s", voice_last_status, body);
            } else {
                voice_fail(VOICE_ERR_HTTP, "http %d", voice_last_status);
            }
            kind = VOICE_ERR_HTTP;
            break;
        }
    } while (0);

    /* The one exit. Every failure path above breaks to here, and the hold is
       released here unless the body already finished and gave it back early. */
    if (opened) esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (held) mic_serve_release();

    voice_last_ms = (uint32_t)(millis() - started);
    voice_last_bytes = off;
    voice_bytes_sent += off;
    voice_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    if (kind == VOICE_OK) {
        voice_uploads++;
        voice_last_kind = VOICE_OK;
        voice_last_err[0] = '\0';
        event_add("voice: sent %lums of audio to %s, http %d in %lums",
                  (unsigned long)mic_ms_for(len), target.host, voice_last_status,
                  (unsigned long)voice_last_ms);
    }
}

/* The task itself: block until somebody asks, upload, drop the interlock,
   block again. Idle cost is zero — a task blocked on a notification is not
   scheduled at all, which is why this is a notification rather than a poll. */
static void voice_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        voice_upload_run();
        voice_pending.store(false);
    }
}

/* Ask for an upload. Returns false if one is already in flight — the
   compare-exchange is what makes that answer true rather than a guess, and it
   is why two clients hitting POST /voice/send at once produce one upload and
   one honest 409 instead of two uploads racing over the same buffer. */
static bool voice_request() {
    if (!voice_ready) return false;
    bool expected = false;
    if (!voice_pending.compare_exchange_strong(expected, true)) return false;
    xTaskNotifyGive(voice_task_h);
    return true;
}

/* --- Persistence --- */

static void voice_cfg_save() {
    JsonDocument doc;
    doc["url"] = voice_url;
    doc["auto"] = voice_auto;
    String out;
    serializeJson(doc, out);
    write_spiffs_file_atomic(VOICE_CFG_FILE, VOICE_CFG_TMP, out);
}

static void voice_cfg_mark_dirty() {
    unsigned long at = millis() + VOICE_CFG_DELAY_MS;
    if (!voice_cfg_dirty || (long)(at - voice_cfg_save_at) < 0) voice_cfg_save_at = at;
    voice_cfg_dirty = true;
}

/* A file that is missing, truncated or nonsense leaves the compiled defaults
   standing — no target and no auto-upload — exactly as the ring's and the
   speaker's do. The stored URL is re-parsed rather than trusted: this file can
   be replaced wholesale, and a target that arrived by any route other than the
   endpoint would otherwise skip every check the endpoint makes. */
static void voice_cfg_load() {
    String raw = read_spiffs_file(VOICE_CFG_FILE);
    if (raw.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

    if (doc["auto"].is<bool>()) voice_auto = doc["auto"].as<bool>();

    const char *u = doc["url"].as<const char *>();
    if (u && u[0]) {
        VoiceUrl parsed;
        if (voice_url_parse(u, parsed, NULL)) {
            snprintf(voice_url, sizeof(voice_url), "%s", u);
            voice_target = parsed;
        } else {
            event_add("voice: stored target url is not usable, ignored");
        }
    }
}

/* --- Poll ---
 *
 * Loop task. Two jobs, and neither of them is the upload: the flash write the
 * endpoints are not allowed to do, and the falling edge that fires an
 * auto-upload. Idle cost is a comparison and a digitalRead-free bool. */
static void voice_poll() {
    if (!voice_ready) return;

    unsigned long now = millis();

    if (voice_cfg_dirty && (long)(now - voice_cfg_save_at) >= 0) {
        voice_cfg_dirty = false;
        voice_cfg_save();
    }

    /* A take has just ended when mic_is_recording() falls. Both this and
       mic_poll() run on every loop() pass, and mic_poll() runs first, so the
       length is already published by the time the edge is seen here. A take
       cannot begin and end inside one pass either: starting one takes a 700ms
       hold and ending it takes the key coming back up. */
    bool rec = mic_is_recording();
    bool ended = voice_was_rec && !rec;
    voice_was_rec = rec;

    if (ended && voice_auto && mic_take_len > 0) {
        if (!voice_request())
            event_add("voice: auto-upload skipped, one is already in flight");
    }
}

/* --- Endpoints --- */

static const SkillEndpoint voice_endpoints[] = {
    {"GET",  "/voice",      "Upload target, counters and the last upload's result"},
    {"POST", "/voice",      "Set the target url and whether takes upload themselves"},
    {"POST", "/voice/send", "Send the last recording to the configured target now"},
    {NULL, NULL, NULL}
};

static const char *voice_describe() {
    return "## Skill: voice\n\n"
           "The pager's first outbound request. `skills/mic.cpp` captures a take\n"
           "and hands it back when asked; this sends it somewhere by itself.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /voice | `{\"url\":\"http://...\",\"auto\":false,\"attempts\":3,\"uploads\":2,...}` |\n"
           "| POST | /voice | `{\"url\":\"http://host:9000/takes\",\"auto\":true}` |\n"
           "| POST | /voice/send | Upload the last take now; 409 if one is already in flight |\n\n"
           "### What is sent\n\n"
           "A single `POST` with `Content-Type: audio/wav` and a body that is the\n"
           "canonical 44-byte header followed by 16kHz mono 16-bit PCM — byte for\n"
           "byte what `GET /mic/last` serves. `Content-Length` is set before the\n"
           "connection opens, so a receiver never has to guess. Two extra headers\n"
           "come along: `X-Seed-Node` (the mDNS name) and `X-Seed-Take-Ms`.\n\n"
           "Any HTTP server that accepts a POST body will do. There is no TLS:\n"
           "this build carries no certificate store, so `https://` is refused at\n"
           "the endpoint rather than failing later.\n\n"
           "### The target url\n\n"
           "`http://host[:port][/path]`. A bare IP works and skips name\n"
           "resolution entirely; a `.local` name resolves through lwIP, which has\n"
           "mDNS queries enabled in this core. Port defaults to 80, path to `/`.\n"
           "A url that cannot be used is refused with a reason when you set it,\n"
           "not later in a counter, and `host`, `port` and `path` are echoed in\n"
           "`GET /voice` so a typo shows up as a wrong field.\n\n"
           "### Auto-upload is off by default\n\n"
           "`auto: true` uploads every take the moment the knob is released. It\n"
           "ships off on purpose: until a receiver has been seen to work, every\n"
           "upload should be one somebody asked for, so a failure is attached to\n"
           "an action instead of found later in a counter.\n\n"
           "### Telling a failure from a silence\n\n"
           "The device is not going to tell you it failed — that is the whole\n"
           "difference between this skill and every other one, because nothing is\n"
           "waiting on the answer. So the counters are the diagnosis, and they are\n"
           "split by KIND rather than summed, because \"it did not upload\" points\n"
           "nowhere and eleven `offline` points somewhere very specific.\n\n"
           "Read them in this order.\n\n"
           "`attempts` against `uploads`. If attempts is not climbing at all, the\n"
           "upload is not being asked for: check `auto`, and check that a take\n"
           "exists at all with `GET /mic` — a take of zero bytes is refused here\n"
           "as `empty` and is a microphone question, not a network one.\n\n"
           "`fails` names where it stopped, and each name is a different half of\n"
           "the system:\n\n"
           "- `config` — no url set. Nothing was ever attempted.\n"
           "- `offline` — the STA link was down before the socket was opened. The\n"
           "  device's problem, not the receiver's.\n"
           "- `empty` / `busy` — nothing to send, or the microphone was in use.\n"
           "- `open` — DNS or connect. The name did not resolve or nothing was\n"
           "  listening. Try the same url with a bare IP: if that works, the name\n"
           "  is the fault.\n"
           "- `write` — the connection died partway through the body.\n"
           "  `last_bytes` says how far it got.\n"
           "- `timeout` — the whole upload hit its 30s ceiling. A receiver that\n"
           "  accepts a connection and then stops reading looks exactly like this,\n"
           "  and the deadline exists so it cannot hold the microphone hostage.\n"
           "- `http` — it ARRIVED. The bytes went out and the far end said no;\n"
           "  `last_status` and `last_error` carry its own words. Nothing on this\n"
           "  device is broken.\n\n"
           "`last_ms` and `last_bytes` are the throughput, and they are here\n"
           "because WiFi modem sleep is on in this firmware and nobody has yet\n"
           "measured what it costs. If a full 320KB take is taking many seconds,\n"
           "`WiFi.setSleep(false)` around the upload is the lever to try — with\n"
           "these two numbers as the before.\n\n"
           "`stack_free` is the uploader task's stack high-water mark in bytes,\n"
           "out of 8192. That figure was a guess, and this is how it stops being\n"
           "one: a number that stays comfortably above zero after several real\n"
           "uploads means the stack is right.\n\n"
           "### Example\n\n"
           "```\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -X POST \\\n"
           "  -d '{\"url\":\"http://192.168.1.10:9000/takes\"}' \\\n"
           "  http://seed.local:8080/voice\n"
           "curl -H \"Authorization: Bearer $TOKEN\" -X POST \\\n"
           "  http://seed.local:8080/voice/send\n"
           "curl -H \"Authorization: Bearer $TOKEN\" http://seed.local:8080/voice\n"
           "```\n";
}

static void voice_state_json(JsonDocument &doc) {
    doc["ready"] = voice_ready;
    doc["url"] = voice_url;
    doc["auto"] = voice_auto;
    doc["pending"] = voice_pending.load();

    /* Echoed so a typo is a visibly wrong field rather than a connect error. */
    if (voice_url[0]) {
        doc["host"] = voice_target.host;
        doc["port"] = voice_target.port;
        doc["path"] = voice_target.path;
    }

    /* What there is to send, so "nothing happened" can be told apart from
       "nothing was recorded" without a second request. */
    doc["take_bytes"] = mic_take_len;

    doc["attempts"] = voice_attempts;
    doc["uploads"] = voice_uploads;
    doc["bytes_sent"] = voice_bytes_sent;
    doc["last_status"] = voice_last_status;
    doc["last_kind"] = voice_err_names[voice_last_kind];
    doc["last_ms"] = voice_last_ms;
    doc["last_bytes"] = voice_last_bytes;
    doc["last_error"] = voice_last_err;

    JsonObject f = doc["fails"].to<JsonObject>();
    for (int i = VOICE_ERR_CONFIG; i < VOICE_ERR_COUNT; i++)
        f[voice_err_names[i]] = voice_fails[i];

    /* The task's own facts, so its parameters are checkable from the network
       rather than only from this file. `stack_free` is the high-water mark in
       bytes, and it is the whole reason the stack size below is allowed to have
       been a guess. */
    JsonObject t = doc["task"].to<JsonObject>();
    t["core"] = VOICE_TASK_CORE;
    t["prio"] = VOICE_TASK_PRIO;
    t["stack"] = VOICE_TASK_STACK;
    t["stack_free"] = voice_stack_free;

    doc["deadline_ms"] = VOICE_DEADLINE_MS;
    doc["timeout_ms"] = VOICE_SOCKET_TIMEOUT_MS;
}

static void voice_register_routes(AsyncWebServer &server) {

    server.on(AsyncURIMatcher::exact("/voice"), HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        voice_state_json(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on(AsyncURIMatcher::exact("/voice"), HTTP_POST, [](AsyncWebServerRequest *req) {
        char *body = notify_take_body(req);
        if (!check_auth(req)) {
            free(body);
            notify_send_error(req, 401, "Authorization: Bearer <token> required");
            return;
        }
        if (!body) {
            notify_send_error(req, 400, "body must be JSON");
            return;
        }
        JsonDocument input;
        DeserializationError jerr = deserializeJson(input, body);
        free(body);
        if (jerr != DeserializationError::Ok) {
            notify_send_error(req, 400, "invalid JSON");
            return;
        }

        /* The uploader task reads the target while it works, so it must not
           change underneath it. Refusing is the honest answer — the caller
           knows within a second whether it took. */
        if (voice_pending.load()) {
            notify_send_error(req, 409, "an upload is in flight, try again in a moment");
            return;
        }

        /* Into locals first: a body that names two settings and gets one of them
           wrong applies neither, rather than half — the rule POST /sound and
           POST /ring already follow. */
        char url[VOICE_URL_LEN];
        snprintf(url, sizeof(url), "%s", voice_url);
        VoiceUrl target = voice_target;
        bool on_auto = voice_auto;

        if (input["auto"].is<bool>()) on_auto = input["auto"].as<bool>();
        else if (!input["auto"].isNull()) {
            notify_send_error(req, 400, "auto must be true or false");
            return;
        }

        if (input["url"].is<const char *>()) {
            const char *u = input["url"].as<const char *>();
            if (u[0] == '\0') {
                /* An empty string clears the target rather than being a bad
                   url: there has to be a way to turn this off that is not
                   "set it to something wrong". The parse goes with it, so
                   nothing stale is left for GET /voice to report. */
                url[0] = '\0';
                memset(&target, 0, sizeof(target));
            } else {
                const char *perr = NULL;
                if (!voice_url_parse(u, target, &perr)) {
                    notify_send_error(req, 400, perr);
                    return;
                }
                snprintf(url, sizeof(url), "%s", u);
            }
        } else if (!input["url"].isNull()) {
            notify_send_error(req, 400, "url must be a string");
            return;
        }

        if (on_auto && url[0] == '\0') {
            notify_send_error(req, 400, "auto needs a url to upload to");
            return;
        }

        snprintf(voice_url, sizeof(voice_url), "%s", url);
        voice_target = target;
        voice_auto = on_auto;
        /* RAM is live now; the loop task writes the flash a beat later. Nothing
           in a request handler touches SPIFFS. */
        voice_cfg_mark_dirty();
        event_add("voice: target %s, auto %s",
                  voice_url[0] ? voice_url : "(none)", voice_auto ? "on" : "off");

        JsonDocument doc;
        voice_state_json(doc);
        notify_send_json(req, 200, doc);
    });

    server.on(AsyncURIMatcher::exact("/voice/send"), HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        if (!voice_ready) {
            notify_send_error(req, 503, "the uploader task did not start");
            return;
        }
        if (voice_url[0] == '\0') {
            notify_send_error(req, 400, "no target url configured, POST /voice first");
            return;
        }
        /* Answered from what is knowable here. Everything else — the link, the
           socket, the far end — is the task's to find out, and this returns
           long before it does; GET /voice is where the result lands. */
        if (mic_is_recording()) {
            notify_send_error(req, 409, "a recording is in progress");
            return;
        }
        if (mic_take_len == 0) {
            notify_send_error(req, 404, "nothing recorded yet, hold the knob");
            return;
        }
        if (!voice_request()) {
            notify_send_error(req, 409, "an upload is already in flight");
            return;
        }

        JsonDocument doc;
        doc["ok"] = true;
        doc["queued"] = true;
        doc["take_bytes"] = mic_take_len;
        notify_send_json(req, 202, doc);
    });
}

static const Skill voice_skill = {
    .name = "voice",
    .version = "0.1.0",
    .describe = voice_describe,
    .endpoints = voice_endpoints,
    .register_routes = voice_register_routes
};

/*
 * The firmware's first task. Everything before this ran on the loop task, the
 * web server's task or an interrupt; nothing here had ever created one.
 *
 * xTaskCreatePinnedToCore() takes the stack depth in BYTES on ESP-IDF's
 * FreeRTOS, not in words as the vanilla kernel does. Getting that backwards is
 * a factor of four in either direction and the failure is a stack overflow much
 * later, on a path nobody was looking at.
 *
 * A failure to create it leaves voice_ready false and registers the skill
 * anyway, for the same reason mic.cpp does: GET /voice should still answer and
 * say so, rather than a missing endpoint making a broken uploader look like an
 * old firmware.
 */
static void skill_voice_init() {
    voice_cfg_load();

    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, VOICE_TASK_NAME,
                                            VOICE_TASK_STACK, NULL,
                                            VOICE_TASK_PRIO, &voice_task_h,
                                            VOICE_TASK_CORE);
    if (ok != pdPASS || !voice_task_h) {
        voice_task_h = NULL;
        event_add("voice: uploader task could not be created");
        skill_register(&voice_skill);
        return;
    }

    voice_ready = true;
    skill_register(&voice_skill);
}
