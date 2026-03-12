/*
 * skills/fs.c — Filesystem skill for Linux seed
 *
 * Browse, read, and write files on the device over HTTP.
 * AI agent can explore the filesystem, read configs, write scripts.
 *
 * Endpoints:
 *   GET  /fs/ls?path=/        — list directory contents
 *   GET  /fs/read?path=/etc/hostname  — read file contents
 *   POST /fs/write            — write file {path, content, mode}
 *   GET  /fs/stat?path=/tmp   — file/directory metadata
 */

static const skill_endpoint_t fs_endpoints[] = {
    { "GET",  "/fs/ls",    "List directory (?path=/)" },
    { "GET",  "/fs/read",  "Read file (?path=/etc/hostname)" },
    { "POST", "/fs/write", "Write file {path, content, mode}" },
    { "GET",  "/fs/stat",  "File metadata (?path=/tmp)" },
    { NULL, NULL, NULL }
};

static const char *fs_describe(void) {
    return "## Skill: fs\n\n"
           "Filesystem access — browse, read, and write files over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /fs/ls?path=/ | List directory (name, size, type, permissions) |\n"
           "| GET | /fs/read?path=/etc/hostname | Read file contents (text) |\n"
           "| POST | /fs/write | Write: `{\"path\":\"/tmp/test.txt\",\"content\":\"hello\"}` |\n"
           "| GET | /fs/stat?path=/tmp | Stat: size, permissions, owner, modified time |\n\n"
           "### Notes\n\n"
           "- Paths must be absolute (start with /)\n"
           "- Read returns raw text, max 1MB\n"
           "- Write creates parent dirs if needed (optional mode, default 0644)\n"
           "- Symlinks are followed\n";
}

/* Validate path: must be absolute, no .. traversal */
static int fs_valid_path(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (strstr(path, "/../") || strstr(path, "/..") == path + strlen(path) - 3)
        return 0;
    return 1;
}

/* URL-decode a path parameter in-place */
static void fs_url_decode(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (*src && i < maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Extract ?path= parameter */
static int fs_get_path_param(const char *url, char *out, int maxlen) {
    char *q = strchr(url, '?');
    if (!q) return 0;
    char *p = strstr(q, "path=");
    if (!p) return 0;
    p += 5;

    char raw[1024];
    int i = 0;
    while (*p && *p != '&' && i < (int)sizeof(raw) - 1) raw[i++] = *p++;
    raw[i] = '\0';

    fs_url_decode(out, raw, maxlen);
    return 1;
}

/* GET /fs/ls?path=/ */
static int fs_handle_ls(int fd, http_req_t *req) {
    char path[1024];
    if (!fs_get_path_param(req->path, path, sizeof(path))) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path parameter required\"}");
        return 1;
    }
    if (!fs_valid_path(path)) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path must be absolute\"}");
        return 1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        char err[256];
        snprintf(err, sizeof(err), "{\"error\":\"cannot open: %s\"}", strerror(errno));
        json_resp(fd, 404, "Not Found", err);
        return 1;
    }

    int cap = 32768;
    char *resp = malloc(cap);
    if (!resp) { closedir(dir); json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "{\"path\":\"%s\",\"entries\":[", path);
    int first = 1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Stat the entry */
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full, &st) < 0) continue;

        char esc_name[512];
        json_escape(ent->d_name, esc_name, sizeof(esc_name));

        const char *type = S_ISDIR(st.st_mode) ? "dir" :
                          S_ISLNK(st.st_mode) ? "link" :
                          S_ISREG(st.st_mode) ? "file" : "other";

        if (!first) off += snprintf(resp + off, cap - off, ",");
        first = 0;

        off += snprintf(resp + off, cap - off,
            "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"mode\":\"%04o\"}",
            esc_name, type, (long)st.st_size, (unsigned)(st.st_mode & 07777));

        if (off >= cap - 512) break;
    }
    closedir(dir);

    off += snprintf(resp + off, cap - off, "]}");
    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* GET /fs/read?path=/etc/hostname */
static int fs_handle_read(int fd, http_req_t *req) {
    char path[1024];
    if (!fs_get_path_param(req->path, path, sizeof(path))) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path parameter required\"}");
        return 1;
    }
    if (!fs_valid_path(path)) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path must be absolute\"}");
        return 1;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"file not found\"}");
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"not a regular file\"}");
        return 1;
    }
    if (st.st_size > 1048576) {
        json_resp(fd, 413, "Payload Too Large", "{\"error\":\"file too large (max 1MB)\"}");
        return 1;
    }

    long len = 0;
    char *content = file_read(path, &len);
    if (!content) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"cannot read file\"}");
        return 1;
    }

    text_resp(fd, 200, "OK", content);
    free(content);
    return 1;
}

/* POST /fs/write — {path, content, mode} */
static int fs_handle_write(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    /* Extract path */
    char path[1024] = "";
    char *p = strstr(req->body, "\"path\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) { p++; int i = 0; while (*p && *p != '"' && i < (int)sizeof(path) - 1) path[i++] = *p++; path[i] = '\0'; }
    }

    if (!fs_valid_path(path)) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"valid absolute path required\"}");
        return 1;
    }

    /* Extract content */
    p = strstr(req->body, "\"content\"");
    if (!p) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"content required\"}");
        return 1;
    }
    p = strchr(p + 9, '"');
    if (!p) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"content must be a string\"}");
        return 1;
    }
    p++;

    /* Decode JSON string */
    char *content = malloc(MAX_BODY);
    if (!content) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}");
        return 1;
    }
    int ci = 0;
    while (*p && *p != '"' && ci < MAX_BODY - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': content[ci++] = '\n'; break;
                case 'r': content[ci++] = '\r'; break;
                case 't': content[ci++] = '\t'; break;
                case '\\': content[ci++] = '\\'; break;
                case '"': content[ci++] = '"'; break;
                default: content[ci++] = '\\'; content[ci++] = *p; break;
            }
        } else {
            content[ci++] = *p;
        }
        p++;
    }
    content[ci] = '\0';

    /* Extract optional mode (default 0644) */
    int mode = 0644;
    p = strstr(req->body, "\"mode\"");
    if (p) {
        p = strchr(p + 6, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            /* Always parse as octal — file modes are octal by convention */
            int v = (int)strtol(p, NULL, 8);
            if (v > 0 && v <= 07777) mode = v;
        }
    }

    /* Create parent directories (no shell) */
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        for (char *p2 = dir + 1; *p2; p2++) {
            if (*p2 == '/') { *p2 = '\0'; mkdir(dir, 0755); *p2 = '/'; }
        }
        mkdir(dir, 0755);
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        char err[256];
        snprintf(err, sizeof(err), "{\"error\":\"cannot write: %s\"}", strerror(errno));
        free(content);
        json_resp(fd, 500, "Internal Server Error", err);
        return 1;
    }
    fwrite(content, 1, ci, fp);
    fclose(fp);
    chmod(path, mode);

    event_add("fs: wrote %d bytes to %s", ci, path);

    free(content);

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\",\"bytes\":%d}", path, ci);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* GET /fs/stat?path=/tmp */
static int fs_handle_stat(int fd, http_req_t *req) {
    char path[1024];
    if (!fs_get_path_param(req->path, path, sizeof(path))) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path parameter required\"}");
        return 1;
    }
    if (!fs_valid_path(path)) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"path must be absolute\"}");
        return 1;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"not found\"}");
        return 1;
    }

    const char *type = S_ISDIR(st.st_mode) ? "dir" :
                      S_ISLNK(st.st_mode) ? "link" :
                      S_ISREG(st.st_mode) ? "file" : "other";

    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"path\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"mode\":\"%04o\","
        "\"uid\":%d,\"gid\":%d,\"modified\":%ld}",
        path, type, (long)st.st_size, (unsigned)(st.st_mode & 07777),
        (int)st.st_uid, (int)st.st_gid, (long)st.st_mtime);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

static int fs_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (strncmp(req->path, "/fs/ls", 6) == 0)
            return fs_handle_ls(fd, req);
        if (strncmp(req->path, "/fs/read", 8) == 0)
            return fs_handle_read(fd, req);
        if (strncmp(req->path, "/fs/stat", 8) == 0)
            return fs_handle_stat(fd, req);
    }
    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(req->path, "/fs/write") == 0)
            return fs_handle_write(fd, req);
    }
    return 0;
}

static const skill_t fs_skill = {
    .name      = "fs",
    .version   = "0.1.0",
    .describe  = fs_describe,
    .endpoints = fs_endpoints,
    .handle    = fs_handle
};

static void fs_init(void) {
    skill_register(&fs_skill);
}
