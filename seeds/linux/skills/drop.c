/*
 * skills/drop.c — Append-only message drop for session coordination
 *
 * Parallel agent sessions coordinate through the drop: each session posts
 * messages under its handle and polls its inbox between work steps. The
 * store is append-only JSONL — one message per line, appended, never
 * rewritten. A fixed ring keeps the last 256 messages in memory; per-handle
 * cursors track what each reader has already consumed.
 *
 * Endpoints:
 *   POST /drop            — post message {from, to, reply_to?, link?, text}
 *   GET  /drop            — all ring messages (?since=ID)
 *   GET  /drop/inbox      — inbox for a handle (?handle=X&peek=1)
 *   GET  /drop/board.md   — the ring rendered as markdown
 */

#include <limits.h>

#define DROP_FILE         INSTALL_DIR "/drop.jsonl"
#define DROP_CURSORS_FILE INSTALL_DIR "/drop-cursors.json"

/* host-test:begin drop — sliced out by tools/test_drop.sh */
#define DROP_RING           256
#define DROP_HANDLE_LEN     32
#define DROP_LINK_LEN       128
#define DROP_TEXT_LEN       1024
#define DROP_CURSORS_MAX    32
#define DROP_LINE_MAX       4096  /* one JSONL line, worst case ~2.5KB */
/* Worst-case rendered sizes, derived from the field caps: json_escape can
 * at most double text/link; the +128 covers keys, ids and punctuation. */
#define DROP_MSG_JSON_MAX   (2 * DROP_TEXT_LEN + 2 * DROP_LINK_LEN + 2 * DROP_HANDLE_LEN + 128)
#define DROP_BOARD_LINE_MAX (DROP_TEXT_LEN + DROP_LINK_LEN + 2 * DROP_HANDLE_LEN + 128)
#define DROP_RESP_HEADROOM  128   /* inbox JSON wrapper around the array */

typedef struct {
    int    id;
    time_t ts;
    int    reply_to;              /* 0 = not a reply */
    char   from[DROP_HANDLE_LEN + 1];
    char   to[DROP_HANDLE_LEN + 1];
    char   link[DROP_LINK_LEN + 1];
    char   text[DROP_TEXT_LEN + 1];
} drop_msg_t;

static drop_msg_t g_drop_ring[DROP_RING];
static int g_drop_head;           /* next slot to write */
static int g_drop_count;
static int g_drop_next_id = 1;

typedef struct {
    char handle[DROP_HANDLE_LEN + 1];
    int  cursor;                  /* highest id this reader has consumed */
} drop_cursor_t;

static drop_cursor_t g_drop_cursors[DROP_CURSORS_MAX];
static int g_drop_cursor_count;

/* Bounded copy without the strncpy/snprintf truncation-warning tarpits */
static void drop_str_copy(char *dst, int cap, const char *src) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Bounded accumulate: clamps the offset so snprintf truncation can never
 * wrap the remaining size negative (the repo-wide accumulation trap). */
static void drop_appendf(char *buf, int cap, int *off, const char *fmt, ...) {
    if (*off >= cap) { *off = cap - 1; return; }
    va_list ap;
    va_start(ap, fmt);
    *off += vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (*off >= cap) *off = cap - 1;
}

/* --- JSON extraction (same naive idiom as notes.c; safe here because it
 *     scans one message at a time and stored strings are escaped) --- */

/* Returns -1 if key absent or its value is not a string, else the full
 * (untruncated) value length. The value quote must follow the key's colon
 * directly (whitespace allowed): "from":123 must NOT drift the extraction
 * to the next quoted token and invent a phantom sender. */
static int drop_json_str(const char *json, const char *key, char *out, int maxlen) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0, n = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = *p;   break;  /* covers \" and \\ */
            }
        }
        if (i < maxlen - 1) out[i++] = c;
        n++;
        p++;
    }
    out[i] = '\0';
    return n;
}

static long drop_json_int(const char *json, const char *key, long def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p = strchr(p + strlen(search), ':');
    if (!p) return def;
    return strtol(p + 1, NULL, 10);
}

/* --- Validation / sanitization --- */

/* Handles flow into URLs and to an ASCII-only pager: [A-Za-z0-9_-]{1,32} */
static int drop_handle_ok(const char *s) {
    int i = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return i >= 1 && i <= DROP_HANDLE_LEN;
}

/* Control bytes become spaces (same policy as the tembed notify skill) */
static void drop_scrub(char *s) {
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) s[i] = ' ';
    }
}

/* If a byte-cap cut tore a trailing multi-byte UTF-8 sequence, drop the
 * fragment whole (same policy as the tembed notify skill) — a torn byte
 * passes json_escape raw and breaks strict JSON parsers downstream. */
static void drop_utf8_trim(char *s) {
    int len = (int)strlen(s);
    if (len == 0) return;
    int i = len;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) { s[0] = '\0'; return; }  /* nothing but continuation bytes */
    unsigned char lead = (unsigned char)s[i - 1];
    int need = 1;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else if (lead >= 0x80) { s[i - 1] = '\0'; return; }  /* stray lead byte */
    if (len - (i - 1) < need) s[i - 1] = '\0';  /* torn sequence */
}

/* Ids are int: clamp parsed longs into 0..INT_MAX instead of wrapping */
static int drop_clamp_id(long v) {
    if (v < 0) return 0;
    if (v > INT_MAX) return INT_MAX;
    return (int)v;
}

/* --- In-memory ring --- */

static drop_msg_t *drop_ring_add(void) {
    drop_msg_t *m = &g_drop_ring[g_drop_head];
    memset(m, 0, sizeof(*m));
    g_drop_head = (g_drop_head + 1) % DROP_RING;
    if (g_drop_count < DROP_RING) g_drop_count++;
    return m;
}

/* i-th oldest message, 0 <= i < g_drop_count */
static const drop_msg_t *drop_ring_at(int i) {
    int start = (g_drop_count < DROP_RING) ? 0 : g_drop_head;
    return &g_drop_ring[(start + i) % DROP_RING];
}

/* --- Cursors (in-memory; persistence lives with the handlers) --- */

static int drop_cursor_get(const char *handle) {
    for (int i = 0; i < g_drop_cursor_count; i++)
        if (strcmp(g_drop_cursors[i].handle, handle) == 0)
            return g_drop_cursors[i].cursor;
    return 0;
}

static void drop_cursor_set(const char *handle, int cursor) {
    for (int i = 0; i < g_drop_cursor_count; i++) {
        if (strcmp(g_drop_cursors[i].handle, handle) == 0) {
            g_drop_cursors[i].cursor = cursor;
            return;
        }
    }
    int slot = g_drop_cursor_count;
    if (slot >= DROP_CURSORS_MAX) {
        /* Full: evict the least-advanced reader */
        slot = 0;
        for (int i = 1; i < DROP_CURSORS_MAX; i++)
            if (g_drop_cursors[i].cursor < g_drop_cursors[slot].cursor)
                slot = i;
    } else {
        g_drop_cursor_count++;
    }
    drop_str_copy(g_drop_cursors[slot].handle,
                  sizeof(g_drop_cursors[slot].handle), handle);
    g_drop_cursors[slot].cursor = cursor;
}

/* --- Rendering --- */

/* One message as JSON. Never writes past cap; returns bytes written. */
static int drop_msg_json(const drop_msg_t *m, char *buf, int cap) {
    char esc_link[DROP_LINK_LEN * 2 + 8];
    char esc_text[DROP_TEXT_LEN * 2 + 8];
    json_escape(m->link, esc_link, sizeof(esc_link));
    json_escape(m->text, esc_text, sizeof(esc_text));
    int n = snprintf(buf, cap,
        "{\"id\":%d,\"ts\":%lld,\"from\":\"%s\",\"to\":\"%s\","
        "\"reply_to\":%d,\"link\":\"%s\",\"text\":\"%s\"}",
        m->id, (long long)m->ts, m->from, m->to, m->reply_to,
        esc_link, esc_text);
    if (n >= cap) n = cap - 1;
    return n;
}

/* JSON array of ring messages with id > since, oldest first.
 * to_handle NULL = no filter, else to == handle or to == "all".
 * out_maxid gets the highest id included. Output bounded to cap. */
static int drop_list_json(char *buf, int cap, int since, const char *to_handle,
                          int *out_count, int *out_maxid) {
    int o = 0, first = 1, count = 0, maxid = 0;
    drop_appendf(buf, cap, &o, "[");
    for (int i = 0; i < g_drop_count; i++) {
        const drop_msg_t *m = drop_ring_at(i);
        if (m->id <= since) continue;
        if (to_handle) {
            /* Direct mail, or a broadcast — but not the reader's own:
             * a sender knows what they posted to "all". */
            int direct = strcmp(m->to, to_handle) == 0;
            int bcast  = strcmp(m->to, "all") == 0 &&
                         strcmp(m->from, to_handle) != 0;
            if (!direct && !bcast) continue;
        }
        if (cap - o < DROP_MSG_JSON_MAX + 4) break;
        if (!first) buf[o++] = ',';
        o += drop_msg_json(m, buf + o, cap - o);
        first = 0;
        count++;
        if (m->id > maxid) maxid = m->id;
    }
    drop_appendf(buf, cap, &o, "]");
    if (out_count) *out_count = count;
    if (out_maxid) *out_maxid = maxid;
    return o;
}

/* The cursor-commit half of an inbox fetch, separated so the ordering is
 * testable: delivered=0 (the response never reached the client) must leave
 * the cursor alone — the mail redelivers on the next fetch. A duplicate
 * is acceptable, loss is not. Returns the resulting cursor. */
static int drop_inbox_commit(const char *handle, int peek, int maxid,
                             int delivered) {
    int cur = drop_cursor_get(handle);
    if (delivered && !peek && maxid > cur) {
        drop_cursor_set(handle, maxid);
        cur = maxid;
    }
    return cur;
}

/* The ring as markdown, oldest first, bounded to cap with a visible cut */
static int drop_board_render(char *buf, int cap) {
    int o = 0;
    drop_appendf(buf, cap, &o, "# Drop board\n\n%d message(s), oldest first.\n\n",
                 g_drop_count);
    for (int i = 0; i < g_drop_count; i++) {
        const drop_msg_t *m = drop_ring_at(i);
        if (cap - o < DROP_BOARD_LINE_MAX) {
            drop_appendf(buf, cap, &o, "\n(truncated)\n");
            break;
        }
        char reply[24] = "";
        if (m->reply_to > 0)
            snprintf(reply, sizeof(reply), " re #%d", m->reply_to);
        time_t ts = m->ts;
        struct tm tm;
        gmtime_r(&ts, &tm);
        drop_appendf(buf, cap, &o,
            "- **%s→%s** (#%d%s, %02d:%02d UTC) — %s%s%s\n",
            m->from, m->to, m->id, reply, tm.tm_hour, tm.tm_min,
            m->text, m->link[0] ? " — " : "", m->link);
    }
    return o;
}

/* --- JSONL store --- */

static int drop_msg_format_jsonl(const drop_msg_t *m, char *buf, int cap) {
    int n = drop_msg_json(m, buf, cap);
    if (n < cap - 1) { buf[n++] = '\n'; buf[n] = '\0'; }
    return n;
}

static int drop_parse_line(const char *line, drop_msg_t *m) {
    memset(m, 0, sizeof(*m));
    /* id must leave room for next_id = id + 1 without overflow */
    long idv = drop_json_int(line, "id", 0);
    if (idv <= 0 || idv >= (long)INT_MAX - 1) return 0;
    m->id = (int)idv;
    m->ts = (time_t)drop_json_int(line, "ts", 0);
    m->reply_to = drop_clamp_id(drop_json_int(line, "reply_to", 0));
    drop_json_str(line, "from", m->from, sizeof(m->from));
    drop_json_str(line, "to", m->to, sizeof(m->to));
    drop_json_str(line, "link", m->link, sizeof(m->link));
    drop_json_str(line, "text", m->text, sizeof(m->text));
    /* Torn lines (mid-write crash) and foreign edits are refused, not
     * loaded as ghosts. Valid handles cannot carry quotes, so this also
     * keeps drop_msg_json — which prints from/to unescaped — injection-free. */
    if (!drop_handle_ok(m->from) || !drop_handle_ok(m->to) || !m->text[0])
        return 0;
    drop_scrub(m->text);
    drop_scrub(m->link);
    drop_utf8_trim(m->text);
    drop_utf8_trim(m->link);
    return 1;
}

/* If the store does not end in a newline (torn append: crash mid-write),
 * seal it with one '\n' so the fragment becomes a junk line the parser
 * refuses, instead of silently merging with the next append. */
static void drop_seal_file(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz > 0) {
        char last = 0;
        if (lseek(fd, sz - 1, SEEK_SET) == sz - 1 &&
            read(fd, &last, 1) == 1 && last != '\n') {
            char nl = '\n';
            if (write(fd, &nl, 1) != 1) { /* nothing else to do */ }
        }
    }
    close(fd);
}

/* Stale cursors (file from a longer history than the store) would swallow
 * all new mail up to their old position; clamp them to the loaded range. */
static void drop_cursors_clamp(void) {
    for (int i = 0; i < g_drop_cursor_count; i++)
        if (g_drop_cursors[i].cursor > g_drop_next_id - 1)
            g_drop_cursors[i].cursor = g_drop_next_id - 1;
}

/* Rebuild ring + next id from the JSONL store. Lines that do not parse
 * (foreign edits, truncation) are skipped, not fatal. */
static int drop_load_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[DROP_LINE_MAX];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        drop_msg_t tmp;
        if (!drop_parse_line(line, &tmp)) continue;
        *drop_ring_add() = tmp;
        if (tmp.id >= g_drop_next_id) g_drop_next_id = tmp.id + 1;
        loaded++;
    }
    fclose(fp);
    return loaded;
}
/* host-test:end */

/* --- Persistence: cursors (tiny map, whole-file rewrite is fine here) --- */

static void drop_cursors_save(void) {
    char buf[DROP_CURSORS_MAX * (DROP_HANDLE_LEN + 20) + 8];
    int o = 0;
    drop_appendf(buf, (int)sizeof(buf), &o, "{");
    for (int i = 0; i < g_drop_cursor_count; i++)
        drop_appendf(buf, (int)sizeof(buf), &o, "%s\"%s\":%d",
                     i ? "," : "", g_drop_cursors[i].handle,
                     g_drop_cursors[i].cursor);
    drop_appendf(buf, (int)sizeof(buf), &o, "}\n");
    /* tmp + rename: a crash mid-save must not tear the cursors file */
    if (file_write(DROP_CURSORS_FILE ".tmp", buf, o) == 0)
        rename(DROP_CURSORS_FILE ".tmp", DROP_CURSORS_FILE);
}

static void drop_cursors_load(void) {
    char *data = file_read(DROP_CURSORS_FILE, NULL);
    if (!data) return;
    const char *p = data;
    while ((p = strchr(p, '"'))) {
        p++;
        char handle[DROP_HANDLE_LEN + 1];
        int i = 0;
        while (*p && *p != '"' && i < DROP_HANDLE_LEN) handle[i++] = *p++;
        handle[i] = '\0';
        if (*p != '"') break;
        p++;
        if (*p != ':') continue;
        int cur = (int)strtol(p + 1, NULL, 10);
        if (drop_handle_ok(handle) && cur > 0) drop_cursor_set(handle, cur);
    }
    free(data);
}

/* Append one message to the JSONL store — O_APPEND, never rewritten */
static int drop_append(const drop_msg_t *m) {
    char line[DROP_LINE_MAX];
    int n = drop_msg_format_jsonl(m, line, sizeof(line));
    mkdir(INSTALL_DIR, 0755);
    int fd = open(DROP_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    int rc = (write(fd, line, n) == n) ? 0 : -1;
    if (rc != 0) {
        /* Partial write: seal the fragment so it cannot merge with the
         * next append; the parser will refuse the junk line. */
        char nl = '\n';
        if (write(fd, &nl, 1) != 1) { /* nothing else to do */ }
    }
    close(fd);
    return rc;
}

/* Full-write with confirmation. The stock respond() ignores write results,
 * which is fine everywhere except the inbox: mail must not be consumed
 * when the client never received it. */
static int drop_write_all(int fd, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += (int)n;
    }
    return 0;
}

/* write() success only means the kernel buffered the bytes — a client that
 * dies mid-transfer still "succeeds". So after the full write we half-close
 * and wait for the client's orderly EOF: a client that consumed the stream
 * closes cleanly (FIN, read returns 0), while a dead one resets — the
 * kernel sends RST when a socket with unread data is torn down — and RST
 * or timeout (SO_RCVTIMEO is set by the accept loop) means unconfirmed. */
static int drop_send_json_checked(int fd, const char *body) {
    char hdr[256];
    int blen = (int)strlen(body);
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", blen);
    if (drop_write_all(fd, hdr, hl) != 0) return -1;
    if (drop_write_all(fd, body, blen) != 0) return -1;
    if (shutdown(fd, SHUT_WR) != 0) return -1;
    for (;;) {
        char tail[64];
        ssize_t n = read(fd, tail, sizeof(tail));
        if (n == 0) return 0;   /* orderly EOF: delivery confirmed */
        if (n < 0) return -1;   /* RST or timeout: unconfirmed */
        /* stray bytes from the client are ignored */
    }
}

/* --- Handlers --- */

/* Anchored query lookup: "key" must follow '?' or '&' and be followed by
 * '='. Returns a pointer past the '=' or NULL. Bare strstr would let
 * ?handle=x&xpeek=1 match peek. */
static const char *drop_query_find(const char *path, const char *key) {
    const char *p = strchr(path, '?');
    if (!p) return NULL;
    size_t klen = strlen(key);
    for (; *p; p++) {
        if ((*p == '?' || *p == '&') &&
            strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '=')
            return p + klen + 2;
    }
    return NULL;
}

/* POST /drop — validate, assign id/ts, append, ring */
static int drop_handle_post(int fd, http_req_t *req) {
    if (!req->body || req->body_len == 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    drop_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    int nf = drop_json_str(req->body, "from", msg.from, sizeof(msg.from));
    int nt = drop_json_str(req->body, "to", msg.to, sizeof(msg.to));
    if (nf > DROP_HANDLE_LEN || nt > DROP_HANDLE_LEN ||
        !drop_handle_ok(msg.from) || !drop_handle_ok(msg.to)) {
        json_resp(fd, 400, "Bad Request",
            "{\"error\":\"from/to must match [A-Za-z0-9_-]{1,32}\"}");
        return 1;
    }

    drop_json_str(req->body, "text", msg.text, sizeof(msg.text));
    if (!msg.text[0]) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"text required\"}");
        return 1;
    }
    drop_scrub(msg.text);
    drop_utf8_trim(msg.text);
    drop_json_str(req->body, "link", msg.link, sizeof(msg.link));
    drop_scrub(msg.link);
    drop_utf8_trim(msg.link);
    msg.reply_to = drop_clamp_id(drop_json_int(req->body, "reply_to", 0));

    msg.id = g_drop_next_id;
    msg.ts = time(NULL);
    if (drop_append(&msg) != 0) {
        json_resp(fd, 500, "Error", "{\"error\":\"append failed\"}");
        return 1;
    }
    g_drop_next_id++;
    *drop_ring_add() = msg;
    event_add("drop #%d %s->%s", msg.id, msg.from, msg.to);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":%d}", msg.id);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* GET /drop?since=N */
static int drop_handle_all(int fd, http_req_t *req) {
    int since = 0;
    const char *sv = drop_query_find(req->path, "since");
    if (sv) since = drop_clamp_id(strtol(sv, NULL, 10));
    int cap = DROP_RING * DROP_MSG_JSON_MAX + 64;
    char *buf = malloc(cap);
    if (!buf) {
        json_resp(fd, 500, "Error", "{\"error\":\"malloc\"}");
        return 1;
    }
    drop_list_json(buf, cap, since, NULL, NULL, NULL);
    json_resp(fd, 200, "OK", buf);
    free(buf);
    return 1;
}

/* GET /drop/inbox?handle=X[&peek=1] */
static int drop_handle_inbox(int fd, http_req_t *req) {
    char handle[DROP_HANDLE_LEN + 1] = "";
    int peek = 0, overlong = 0;
    const char *s = drop_query_find(req->path, "handle");
    if (s) {
        int i = 0;
        while (*s && *s != '&') {
            if (i < DROP_HANDLE_LEN) handle[i++] = *s;
            else overlong = 1;
            s++;
        }
        handle[i] = '\0';
    }
    const char *pv = drop_query_find(req->path, "peek");
    if (pv && pv[0] == '1' && (pv[1] == '\0' || pv[1] == '&')) peek = 1;
    if (overlong || !drop_handle_ok(handle)) {
        json_resp(fd, 400, "Bad Request",
            "{\"error\":\"handle required, [A-Za-z0-9_-]{1,32}\"}");
        return 1;
    }

    int cap = DROP_RING * DROP_MSG_JSON_MAX + 64;
    char *arr = malloc(cap);
    if (!arr) {
        json_resp(fd, 500, "Error", "{\"error\":\"malloc\"}");
        return 1;
    }
    int cur = drop_cursor_get(handle);
    int count = 0, maxid = 0;
    drop_list_json(arr, cap, cur, handle, &count, &maxid);
    int prospective = (!peek && maxid > cur) ? maxid : cur;

    char *out = malloc(cap + DROP_RESP_HEADROOM);
    if (!out) {
        free(arr);
        json_resp(fd, 500, "Error", "{\"error\":\"malloc\"}");
        return 1;
    }
    snprintf(out, cap + DROP_RESP_HEADROOM,
        "{\"handle\":\"%s\",\"cursor\":%d,\"count\":%d,\"messages\":%s}",
        handle, prospective, count, arr);
    /* Consume-after-delivery: the cursor advances only when the full
     * response reached the client. A broken write leaves it, and the
     * mail redelivers on the next fetch. */
    int sent = drop_send_json_checked(fd, out);
    int after = drop_inbox_commit(handle, peek, maxid, sent == 0);
    if (after != cur) drop_cursors_save();
    free(arr);
    free(out);
    return 1;
}

/* GET /drop/board.md */
static int drop_handle_board(int fd, http_req_t *req) {
    (void)req;
    /* Sized for a full ring of worst-case lines: a board that cut off the
     * newest messages would hide exactly what readers open it for. The
     * render-side truncation guard stays as a last resort. */
    int cap = DROP_RING * DROP_BOARD_LINE_MAX + 256;
    char *buf = malloc(cap);
    if (!buf) {
        json_resp(fd, 500, "Error", "{\"error\":\"malloc\"}");
        return 1;
    }
    int o = drop_board_render(buf, cap);
    respond(fd, 200, "OK", "text/markdown; charset=utf-8", buf, o);
    free(buf);
    return 1;
}

/* --- Skill plumbing --- */

static const skill_endpoint_t drop_endpoints[] = {
    { "POST", "/drop",          "Post message {from,to,reply_to?,link?,text}" },
    { "GET",  "/drop",          "All messages (?since=ID)" },
    { "GET",  "/drop/inbox",    "Inbox for a handle (?handle=X&peek=1)" },
    { "GET",  "/drop/board.md", "Message board rendered as markdown" },
    { NULL, NULL, NULL }
};

static const char *drop_describe(void) {
    return "## Skill: drop\n\n"
           "Append-only message drop for coordinating parallel agent sessions. "
           "Messages are never edited or deleted; the node keeps the last 256 "
           "in memory and everything on disk.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /drop | Post: `{\"from\":\"sess-a\",\"to\":\"all\",\"reply_to\":0,\"link\":\"TICKET/12\",\"text\":\"...\"}` |\n"
           "| GET | /drop | All messages, `?since=ID` for newer than ID |\n"
           "| GET | /drop/inbox | `?handle=X` — undelivered for X (to == X or `all`) |\n"
           "| GET | /drop/board.md | The whole ring as markdown |\n\n"
           "### Fields\n\n"
           "`from`/`to`: handles `[A-Za-z0-9_-]{1,32}`, `to` may be `all`. "
           "`reply_to`: numeric id, 0 = none. `link`: optional ticket/PR ref "
           "(128 bytes). `text`: UTF-8, 1024 bytes, control bytes become "
           "spaces. Over-long text/link is truncated silently.\n\n"
           "### Cursor semantics\n\n"
           "Each handle has a cursor. Fetching the inbox returns messages past "
           "the cursor and advances it — fetch = read. Add `&peek=1` to look "
           "without consuming. Cursors persist across restarts. Your own "
           "`to:all` broadcasts are not delivered back to you.\n\n"
           "### Notes\n\n"
           "Loopback is unauthenticated by server design (trusted box). "
           "Post under your session handle; poll your inbox between work "
           "steps.\n\n"
           "Limits: fetches serve from the in-memory ring (last 256) — "
           "older disk-only mail is skipped. Cursors track up to 32 "
           "handles; the least-advanced is evicted when full.\n";
}

static int drop_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->path, "/drop/board.md") == 0)
            return drop_handle_board(fd, req);
        if (strncmp(req->path, "/drop/inbox", 11) == 0 &&
            (req->path[11] == '\0' || req->path[11] == '?'))
            return drop_handle_inbox(fd, req);
        if (strncmp(req->path, "/drop", 5) == 0 &&
            (req->path[5] == '\0' || req->path[5] == '?'))
            return drop_handle_all(fd, req);
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/drop") == 0)
        return drop_handle_post(fd, req);
    return 0;
}

static const skill_t drop_skill = {
    .name      = "drop",
    .version   = "0.1.0",
    .describe  = drop_describe,
    .endpoints = drop_endpoints,
    .handle    = drop_handle
};

static void drop_init(void) {
    drop_seal_file(DROP_FILE);
    drop_load_file(DROP_FILE);
    drop_cursors_load();
    drop_cursors_clamp();
    skill_register(&drop_skill);
}
