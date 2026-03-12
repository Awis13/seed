/*
 * skills/notes.c — Agent dead-drop notes for Linux seed
 *
 * Agents leave notes for each other on the node.
 * First thing an agent should do: GET /notes to see pending tasks.
 * Before leaving: POST /notes with findings for the next agent.
 *
 * Endpoints:
 *   GET    /notes          — list all notes (filter: ?status=open)
 *   POST   /notes          — add note {title, body, priority, tags}
 *   POST   /notes/update   — update note {id, status, body}
 *   POST   /notes/delete   — delete note {id}
 */

#define NOTES_FILE    INSTALL_DIR "/notes.json"
#define NOTES_MAX     64
#define NOTE_TITLE    128
#define NOTE_BODY     4096
#define NOTE_AGENT    64
#define NOTE_TAGS     256

typedef struct {
    int    id;
    int    active;
    char   title[NOTE_TITLE];
    char   body[NOTE_BODY];
    char   agent[NOTE_AGENT];
    char   priority[16];     /* critical, high, medium, low, info */
    char   status[16];       /* open, in_progress, done, wont_fix */
    char   tags[NOTE_TAGS];
    time_t created;
    time_t updated;
} note_t;

static note_t g_notes[NOTES_MAX];
static int g_notes_count = 0;
static int g_notes_next_id = 1;

static const skill_endpoint_t notes_endpoints[] = {
    { "GET",  "/notes",        "List notes (?status=open)" },
    { "POST", "/notes",        "Add note {title, body, priority, tags}" },
    { "POST", "/notes/update", "Update note {id, status, body}" },
    { "POST", "/notes/delete", "Delete note {id}" },
    { NULL, NULL, NULL }
};

static const char *notes_describe(void) {
    return "## Skill: notes\n\n"
           "Agent dead-drop — leave notes for the next agent.\n\n"
           "**IMPORTANT: Read GET /notes before doing any work.** "
           "Previous agents may have left tasks, findings, or warnings.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /notes | List all notes (?status=open for pending only) |\n"
           "| POST | /notes | Add: `{\"title\":\"...\",\"body\":\"...\",\"priority\":\"high\",\"tags\":\"maintenance,security\"}` |\n"
           "| POST | /notes/update | Update: `{\"id\":1,\"status\":\"done\",\"body\":\"fixed by ...\"}` |\n"
           "| POST | /notes/delete | Delete: `{\"id\":1}` |\n\n"
           "### Priority levels\n\n"
           "critical, high, medium, low, info\n\n"
           "### Status values\n\n"
           "open, in_progress, done, wont_fix\n\n"
           "### Workflow\n\n"
           "1. Arrive on node → `GET /notes` (check pending work)\n"
           "2. Do your work (maintenance, recon, fixes)\n"
           "3. Before leaving → `POST /notes` with findings\n"
           "4. If you fixed something → `POST /notes/update` to mark done\n";
}

/* --- JSON extraction helpers (reuse pattern from other skills) --- */

static int notes_json_str(const char *json, const char *key, char *out, int maxlen) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *p = strstr(json, search);
    if (!p) return 0;
    p = strchr(p + strlen(search), '"');
    if (!p) return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

static int notes_json_int(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *p = strstr(json, search);
    if (!p) return -1;
    p = strchr(p + strlen(search), ':');
    if (!p) return -1;
    return atoi(p + 1);
}

/* --- Persistence: load/save notes.json --- */

static void notes_save(void) {
    int cap = NOTES_MAX * (NOTE_TITLE + NOTE_BODY + NOTE_AGENT + NOTE_TAGS + 256);
    char *buf = malloc(cap);
    if (!buf) return;

    int off = 0;
    off += snprintf(buf + off, cap - off, "[");
    int first = 1;

    for (int i = 0; i < g_notes_count; i++) {
        if (!g_notes[i].active) continue;
        note_t *n = &g_notes[i];

        char esc_title[NOTE_TITLE * 2];
        char esc_body[NOTE_BODY * 2];
        char esc_agent[NOTE_AGENT * 2];
        char esc_tags[NOTE_TAGS * 2];
        json_escape(n->title, esc_title, sizeof(esc_title));
        json_escape(n->body, esc_body, sizeof(esc_body));
        json_escape(n->agent, esc_agent, sizeof(esc_agent));
        json_escape(n->tags, esc_tags, sizeof(esc_tags));

        if (!first) off += snprintf(buf + off, cap - off, ",");
        first = 0;

        off += snprintf(buf + off, cap - off,
            "\n{\"id\":%d,\"title\":\"%s\",\"body\":\"%s\","
            "\"agent\":\"%s\",\"priority\":\"%s\",\"status\":\"%s\","
            "\"tags\":\"%s\",\"created\":%ld,\"updated\":%ld}",
            n->id, esc_title, esc_body, esc_agent,
            n->priority, n->status, esc_tags,
            (long)n->created, (long)n->updated);
    }

    off += snprintf(buf + off, cap - off, "\n]");
    file_write(NOTES_FILE, buf, off);
    free(buf);
}

static void notes_load(void) {
    long len = 0;
    char *data = file_read(NOTES_FILE, &len);
    if (!data) return;

    /* Simple parser: find each {"id": block */
    char *p = data;
    g_notes_count = 0;
    g_notes_next_id = 1;

    while ((p = strstr(p, "{\"id\"")) && g_notes_count < NOTES_MAX) {
        /* Find the end of this object */
        char *end = strchr(p, '}');
        if (!end) break;

        /* Temporarily null-terminate this object */
        char saved = *(end + 1);
        *(end + 1) = '\0';

        note_t *n = &g_notes[g_notes_count];
        memset(n, 0, sizeof(*n));
        n->active = 1;

        n->id = notes_json_int(p, "id");
        notes_json_str(p, "title", n->title, NOTE_TITLE);
        notes_json_str(p, "body", n->body, NOTE_BODY);
        notes_json_str(p, "agent", n->agent, NOTE_AGENT);
        notes_json_str(p, "priority", n->priority, sizeof(n->priority));
        notes_json_str(p, "status", n->status, sizeof(n->status));
        notes_json_str(p, "tags", n->tags, NOTE_TAGS);
        n->created = (time_t)notes_json_int(p, "created");
        n->updated = (time_t)notes_json_int(p, "updated");

        if (n->id >= g_notes_next_id) g_notes_next_id = n->id + 1;
        g_notes_count++;

        *(end + 1) = saved;
        p = end + 1;
    }

    free(data);
}

/* --- Handlers --- */

/* GET /notes  (?status=open) */
static int notes_handle_list(int fd, http_req_t *req) {
    /* Parse optional ?status= filter */
    char filter[16] = "";
    char *q = strchr(req->path, '?');
    if (q) {
        char *s = strstr(q, "status=");
        if (s) {
            s += 7;
            int i = 0;
            while (*s && *s != '&' && i < (int)sizeof(filter) - 1)
                filter[i++] = *s++;
            filter[i] = '\0';
        }
    }

    int cap = NOTES_MAX * (NOTE_TITLE + NOTE_BODY + 512);
    char *buf = malloc(cap);
    if (!buf) { json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(buf + off, cap - off, "[");
    int first = 1;
    int count = 0;

    for (int i = 0; i < g_notes_count; i++) {
        if (!g_notes[i].active) continue;
        note_t *n = &g_notes[i];

        /* Apply status filter */
        if (filter[0] && strcmp(n->status, filter) != 0) continue;

        char esc_title[NOTE_TITLE * 2];
        char esc_body[NOTE_BODY * 2];
        char esc_agent[NOTE_AGENT * 2];
        char esc_tags[NOTE_TAGS * 2];
        json_escape(n->title, esc_title, sizeof(esc_title));
        json_escape(n->body, esc_body, sizeof(esc_body));
        json_escape(n->agent, esc_agent, sizeof(esc_agent));
        json_escape(n->tags, esc_tags, sizeof(esc_tags));

        if (!first) off += snprintf(buf + off, cap - off, ",");
        first = 0;

        off += snprintf(buf + off, cap - off,
            "{\"id\":%d,\"title\":\"%s\",\"body\":\"%s\","
            "\"agent\":\"%s\",\"priority\":\"%s\",\"status\":\"%s\","
            "\"tags\":\"%s\",\"created\":%ld,\"updated\":%ld}",
            n->id, esc_title, esc_body, esc_agent,
            n->priority, n->status, esc_tags,
            (long)n->created, (long)n->updated);

        count++;
        if (off >= cap - 4096) break;
    }

    off += snprintf(buf + off, cap - off, "]");
    json_resp(fd, 200, "OK", buf);
    free(buf);
    return 1;
}

/* POST /notes — add new note */
static int notes_handle_add(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }
    if (g_notes_count >= NOTES_MAX) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"max notes reached (64)\"}");
        return 1;
    }

    note_t *n = &g_notes[g_notes_count];
    memset(n, 0, sizeof(*n));

    notes_json_str(req->body, "title", n->title, NOTE_TITLE);
    notes_json_str(req->body, "body", n->body, NOTE_BODY);
    notes_json_str(req->body, "agent", n->agent, NOTE_AGENT);
    notes_json_str(req->body, "priority", n->priority, sizeof(n->priority));
    notes_json_str(req->body, "tags", n->tags, NOTE_TAGS);

    if (!n->title[0]) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"title required\"}");
        return 1;
    }

    if (!n->priority[0]) strcpy(n->priority, "medium");
    if (!n->agent[0]) strcpy(n->agent, "unknown");
    strcpy(n->status, "open");
    n->id = g_notes_next_id++;
    n->active = 1;
    n->created = time(NULL);
    n->updated = n->created;
    g_notes_count++;

    notes_save();
    event_add("notes: added #%d \"%s\" (%s)", n->id, n->title, n->priority);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"id\":%d,\"title\":\"%s\"}", n->id, n->title);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* POST /notes/update — update note status/body */
static int notes_handle_update(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    int id = notes_json_int(req->body, "id");
    if (id < 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"id required\"}");
        return 1;
    }

    /* Find note */
    note_t *n = NULL;
    for (int i = 0; i < g_notes_count; i++) {
        if (g_notes[i].active && g_notes[i].id == id) {
            n = &g_notes[i];
            break;
        }
    }
    if (!n) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"note not found\"}");
        return 1;
    }

    /* Update fields if provided */
    char tmp[NOTE_BODY];
    if (notes_json_str(req->body, "status", tmp, sizeof(tmp)))
        strncpy(n->status, tmp, sizeof(n->status) - 1);
    if (notes_json_str(req->body, "body", tmp, sizeof(tmp)))
        strncpy(n->body, tmp, NOTE_BODY - 1);
    if (notes_json_str(req->body, "priority", tmp, sizeof(tmp)))
        strncpy(n->priority, tmp, sizeof(n->priority) - 1);
    if (notes_json_str(req->body, "agent", tmp, sizeof(tmp)))
        strncpy(n->agent, tmp, NOTE_AGENT - 1);

    n->updated = time(NULL);
    notes_save();
    event_add("notes: updated #%d → %s", n->id, n->status);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"id\":%d,\"status\":\"%s\"}", n->id, n->status);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* POST /notes/delete — remove note */
static int notes_handle_delete(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    int id = notes_json_int(req->body, "id");
    if (id < 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"id required\"}");
        return 1;
    }

    for (int i = 0; i < g_notes_count; i++) {
        if (g_notes[i].active && g_notes[i].id == id) {
            g_notes[i].active = 0;
            notes_save();
            event_add("notes: deleted #%d", id);
            json_resp(fd, 200, "OK", "{\"ok\":true}");
            return 1;
        }
    }

    json_resp(fd, 404, "Not Found", "{\"error\":\"note not found\"}");
    return 1;
}

static int notes_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (strncmp(req->path, "/notes", 6) == 0 &&
            (req->path[6] == '\0' || req->path[6] == '?'))
            return notes_handle_list(fd, req);
    }
    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(req->path, "/notes/update") == 0)
            return notes_handle_update(fd, req);
        if (strcmp(req->path, "/notes/delete") == 0)
            return notes_handle_delete(fd, req);
        if (strcmp(req->path, "/notes") == 0)
            return notes_handle_add(fd, req);
    }
    return 0;
}

static const skill_t notes_skill = {
    .name      = "notes",
    .version   = "0.1.0",
    .describe  = notes_describe,
    .endpoints = notes_endpoints,
    .handle    = notes_handle
};

static void notes_init(void) {
    notes_load();
    skill_register(&notes_skill);
}
