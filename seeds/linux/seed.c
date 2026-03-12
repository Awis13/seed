/*
 * seed.c — AI-growable firmware.
 *
 * Compile:  gcc -O2 -o seed seed.c
 * Run:      ./seed [port]          (default: 8080)
 *
 * Seed is a bootloader. An AI agent connects, explores the node,
 * uploads full firmware via /firmware/source+build+apply.
 * The seed grows into a full-featured mesh node.
 *
 * Endpoints:
 *   GET  /health             — always public, watchdog probe
 *   GET  /capabilities       — node capabilities
 *   GET  /config.md          — node config (markdown)
 *   POST /config.md          — write config
 *   GET  /events             — event log (?since=UNIX_TS)
 *   GET  /firmware/version   — version, build date, uptime
 *   GET  /firmware/source    — read source code
 *   POST /firmware/source    — upload new source code
 *   POST /firmware/build     — compile
 *   GET  /firmware/build/logs — compilation log
 *   POST /firmware/apply     — apply with watchdog + rollback
 *   GET  /skill              — markdown skill file for AI agents
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/utsname.h>

/* ===== Config ===== */
#define VERSION         "0.1.0"
#define DEFAULT_PORT    8080
#define HTTP_BUF        4096
#define RESP_BUF        32768
#define MAX_BODY        65536
#define MAX_EVENTS      128
#define EVENT_MSG_LEN   128
#define SOCK_TIMEOUT    5

#define INSTALL_DIR     "/opt/seed"
#define TOKEN_FILE      INSTALL_DIR "/token"
#define SOURCE_FILE     INSTALL_DIR "/seed.c"
#define BINARY_FILE     INSTALL_DIR "/seed"
#define BINARY_NEW      INSTALL_DIR "/seed-new"
#define BINARY_BACKUP   INSTALL_DIR "/seed.bak"
#define BUILD_LOG       INSTALL_DIR "/build.log"
#define CONFIG_FILE     INSTALL_DIR "/config.md"
#define FAIL_COUNT_FILE INSTALL_DIR "/apply_failures"
#define SERVICE_NAME    "seed"

/* ===== Globals ===== */
static time_t g_start_time;
static char   g_token[65];
static int    g_port = DEFAULT_PORT;

/* Apply failure counter — persisted to file (survives fork + restart) */
static int apply_failures_read(void) {
    FILE *fp = fopen(FAIL_COUNT_FILE, "r");
    if (!fp) return 0;
    int n = 0; fscanf(fp, "%d", &n); fclose(fp);
    return n;
}
static void apply_failures_write(int n) {
    FILE *fp = fopen(FAIL_COUNT_FILE, "w");
    if (fp) { fprintf(fp, "%d\n", n); fclose(fp); }
}
static void apply_failures_reset(void) { unlink(FAIL_COUNT_FILE); }

/* ===== Events ring buffer ===== */
typedef struct { time_t ts; char msg[EVENT_MSG_LEN]; } event_t;
static event_t g_events[MAX_EVENTS];
static int     g_ev_head, g_ev_count;

static void event_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    event_t *e = &g_events[g_ev_head];
    e->ts = time(NULL);
    vsnprintf(e->msg, EVENT_MSG_LEN, fmt, ap);
    g_ev_head = (g_ev_head + 1) % MAX_EVENTS;
    if (g_ev_count < MAX_EVENTS) g_ev_count++;
    va_end(ap);
    fprintf(stderr, "[event] %s\n", e->msg);
}

/* ===== JSON helpers ===== */
static int json_escape(const char *src, char *dst, int maxlen) {
    int o = 0;
    for (int i = 0; src[i] && o < maxlen - 7; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
        else if ((unsigned char)c < 0x20) { o += snprintf(dst + o, maxlen - o, "\\u%04x", (unsigned char)c); }
        else dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

/* ===== Token ===== */
static void token_load(void) {
    FILE *fp = fopen(TOKEN_FILE, "r");
    if (fp) {
        if (fgets(g_token, sizeof(g_token), fp)) {
            int l = strlen(g_token);
            while (l > 0 && (g_token[l-1] == '\n' || g_token[l-1] == '\r'))
                g_token[--l] = '\0';
        }
        fclose(fp);
        if (g_token[0]) {
            size_t tl = strlen(g_token);
            if (tl >= 8)
                fprintf(stderr, "[auth] token loaded (%.4s...%s)\n", g_token, g_token + tl - 4);
            else
                fprintf(stderr, "[auth] token loaded (***)\n");
            return;
        }
    }

    /* Generate token from /dev/urandom */
    fp = fopen("/dev/urandom", "r");
    if (!fp) { perror("urandom"); exit(1); }
    unsigned char raw[16];
    if (fread(raw, 1, 16, fp) != 16) { perror("urandom read"); exit(1); }
    fclose(fp);
    for (int i = 0; i < 16; i++)
        sprintf(g_token + i * 2, "%02x", raw[i]);
    g_token[32] = '\0';

    mkdir(INSTALL_DIR, 0755);
    fp = fopen(TOKEN_FILE, "w");
    if (fp) {
        fprintf(fp, "%s\n", g_token);
        fclose(fp);
        chmod(TOKEN_FILE, 0600);
    }
    fprintf(stderr, "[auth] token generated: %s\n", g_token);
    fprintf(stderr, "[auth] saved to %s\n", TOKEN_FILE);
}

static int is_trusted(const char *ip) {
    return strcmp(ip, "127.0.0.1") == 0;
}

/* ===== HTTP parser ===== */
typedef struct {
    char method[8];
    char path[256];
    char auth[128];
    char *body;
    int  body_len;
    int  content_length;
} http_req_t;

/* ===== Skill/plugin interface ===== */
typedef struct {
    const char *method;      /* "GET", "POST", etc. */
    const char *path;        /* "/myendpoint" */
    const char *description; /* Human-readable description */
} skill_endpoint_t;

typedef struct {
    const char *name;                          /* Short name: "sysmon", "gpio" */
    const char *version;                       /* Semver: "0.1.0" */
    const char *(*describe)(void);             /* Returns markdown description */
    const skill_endpoint_t *endpoints;         /* NULL-terminated array */
    int (*handle)(int fd, http_req_t *req);    /* Returns 1 if handled, 0 if not */
} skill_t;

#define MAX_SKILLS 16
static const skill_t *g_skills[MAX_SKILLS];
static int g_skill_count = 0;

__attribute__((unused))
static int skill_register(const skill_t *skill) {
    if (g_skill_count >= MAX_SKILLS) return -1;
    g_skills[g_skill_count++] = skill;
    return 0;
}

static int parse_request(int fd, http_req_t *req) {
    memset(req, 0, sizeof(*req));
    req->body = NULL;

    char hdr[HTTP_BUF];
    int total = 0;
    int hdr_end = -1;

    while (total < HTTP_BUF - 1) {
        int n = read(fd, hdr + total, HTTP_BUF - 1 - total);
        if (n <= 0) return -1;
        total += n;
        hdr[total] = '\0';
        char *end = strstr(hdr, "\r\n\r\n");
        if (end) { hdr_end = (end - hdr) + 4; break; }
    }
    if (hdr_end < 0) return -1;

    /* Method + path */
    if (sscanf(hdr, "%7s %255s", req->method, req->path) != 2) return -1;

    /* Content-Length */
    char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) req->content_length = atoi(cl + 15);

    /* Authorization: Bearer <token> */
    char *auth = strcasestr(hdr, "Authorization: Bearer ");
    if (auth) {
        auth += 22;
        int i = 0;
        while (auth[i] && auth[i] != '\r' && auth[i] != '\n' && i < 127)
            { req->auth[i] = auth[i]; i++; }
        req->auth[i] = '\0';
    }

    /* Body */
    if (req->content_length > 0) {
        if (req->content_length > MAX_BODY) return -2;
        req->body = malloc(req->content_length + 1);
        if (!req->body) return -2;

        /* Copy bytes already read after headers */
        int already = total - hdr_end;
        if (already > req->content_length) already = req->content_length;
        if (already > 0) memcpy(req->body, hdr + hdr_end, already);
        req->body_len = already;

        /* Read remaining */
        while (req->body_len < req->content_length) {
            int n = read(fd, req->body + req->body_len,
                         req->content_length - req->body_len);
            if (n <= 0) break;
            req->body_len += n;
        }
        req->body[req->body_len] = '\0';
    }
    return 0;
}

/* ===== HTTP response ===== */
static void respond(int fd, int status, const char *status_text,
                    const char *ctype, const char *body, int body_len) {
    char hdr[512];
    if (body_len < 0) body_len = body ? (int)strlen(body) : 0;
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, ctype, body_len);
    write(fd, hdr, hl);
    if (body && body_len > 0) write(fd, body, body_len);
}

static void json_resp(int fd, int status, const char *st, const char *json) {
    respond(fd, status, st, "application/json", json, -1);
}

static void text_resp(int fd, int status, const char *st, const char *text) {
    respond(fd, status, st, "text/plain; charset=utf-8", text, -1);
}

/* ===== File helpers ===== */
static char *file_read(const char *path, long *out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    long rd = fread(buf, 1, sz, fp);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    fclose(fp);
    return buf;
}

static int file_write(const char *path, const char *data, int len) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fwrite(data, 1, len, fp);
    fclose(fp);
    return 0;
}

/* ===== Hardware discovery ===== */

/* Helper: run command, trim newline, return static buf */
static const char *cmd_out(const char *cmd, char *buf, int maxlen) {
    buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return buf;
    if (fgets(buf, maxlen, fp)) {
        int l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
    }
    pclose(fp);
    return buf;
}

/* Append JSON array of matching device paths */
static int discover_devices(char *buf, int maxlen, const char *key,
                            const char *cmd) {
    int o = 0;
    o += snprintf(buf + o, maxlen - o, "\"%s\":[", key);
    FILE *fp = popen(cmd, "r");
    int first = 1;
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp) && o < maxlen - 64) {
            int l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
            if (l == 0) continue;
            char esc[256];
            json_escape(line, esc, sizeof(esc));
            o += snprintf(buf + o, maxlen - o, "%s\"%s\"", first ? "" : ",", esc);
            first = 0;
        }
        pclose(fp);
    }
    o += snprintf(buf + o, maxlen - o, "]");
    return o;
}

/* Full hardware fingerprint for /capabilities */
static int hw_discover(char *buf, int maxlen) {
    int o = 0;
    char tmp[128];

    /* Basic system info */
    char hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));
    o += snprintf(buf + o, maxlen - o, "\"hostname\":\"%s\"", hostname);

    cmd_out("uname -m 2>/dev/null", tmp, sizeof(tmp));
    o += snprintf(buf + o, maxlen - o, ",\"arch\":\"%s\"", tmp[0] ? tmp : "unknown");

    cmd_out("uname -s 2>/dev/null", tmp, sizeof(tmp));
    o += snprintf(buf + o, maxlen - o, ",\"os\":\"%s\"", tmp[0] ? tmp : "unknown");

    cmd_out("uname -r 2>/dev/null", tmp, sizeof(tmp));
    o += snprintf(buf + o, maxlen - o, ",\"kernel\":\"%s\"", tmp[0] ? tmp : "unknown");

    /* CPU */
    int cpus = 0;
    char cpu_model[128] = "";
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "processor", 9) == 0) cpus++;
            if (strncmp(line, "model name", 10) == 0 && !cpu_model[0]) {
                char *v = strchr(line, ':');
                if (v) { v++; while (*v == ' ') v++;
                    strncpy(cpu_model, v, sizeof(cpu_model) - 1);
                    int l = strlen(cpu_model);
                    while (l > 0 && cpu_model[l-1] == '\n') cpu_model[--l] = '\0';
                }
            }
            /* ARM: Hardware line */
            if (strncmp(line, "Hardware", 8) == 0 && !cpu_model[0]) {
                char *v = strchr(line, ':');
                if (v) { v++; while (*v == ' ') v++;
                    strncpy(cpu_model, v, sizeof(cpu_model) - 1);
                    int l = strlen(cpu_model);
                    while (l > 0 && cpu_model[l-1] == '\n') cpu_model[--l] = '\0';
                }
            }
        }
        fclose(fp);
    }
    if (!cpus) cpus = 1; /* fallback */
    o += snprintf(buf + o, maxlen - o, ",\"cpus\":%d", cpus);
    if (cpu_model[0]) {
        char esc_cpu[256];
        json_escape(cpu_model, esc_cpu, sizeof(esc_cpu));
        o += snprintf(buf + o, maxlen - o, ",\"cpu_model\":\"%s\"", esc_cpu);
    }

    /* Memory */
    long mem_kb = 0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "MemTotal:", 9) == 0)
                sscanf(line, "MemTotal: %ld", &mem_kb);
        }
        fclose(fp);
    }
    o += snprintf(buf + o, maxlen - o, ",\"mem_mb\":%ld", mem_kb / 1024);

    /* Disk: root filesystem */
    cmd_out("df -BM / 2>/dev/null | awk 'NR==2{print $2+0}'", tmp, sizeof(tmp));
    if (tmp[0]) o += snprintf(buf + o, maxlen - o, ",\"disk_mb\":%s", tmp);
    cmd_out("df -BM / 2>/dev/null | awk 'NR==2{print $4+0}'", tmp, sizeof(tmp));
    if (tmp[0]) o += snprintf(buf + o, maxlen - o, ",\"disk_free_mb\":%s", tmp);

    /* Temperature (thermal zone 0) */
    fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp) {
        long mtemp = 0;
        if (fscanf(fp, "%ld", &mtemp) == 1)
            o += snprintf(buf + o, maxlen - o, ",\"temp_c\":%.1f", mtemp / 1000.0);
        fclose(fp);
    }

    /* Board model (Pi, etc.) */
    fp = fopen("/sys/firmware/devicetree/base/model", "r");
    if (fp) {
        char model[128] = "";
        if (fgets(model, sizeof(model), fp)) {
            int l = strlen(model);
            while (l > 0 && (model[l-1] == '\n' || model[l-1] == '\0')) l--;
            model[l] = '\0';
            if (model[0]) {
                char esc_model[256];
                json_escape(model, esc_model, sizeof(esc_model));
                o += snprintf(buf + o, maxlen - o, ",\"board_model\":\"%s\"", esc_model);
            }
        }
        fclose(fp);
    }

    /* Has GCC? (can self-compile) */
    int has_gcc = (system("which gcc >/dev/null 2>&1") == 0);
    o += snprintf(buf + o, maxlen - o, ",\"has_gcc\":%s", has_gcc ? "true" : "false");

    /* Network interfaces */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "net_interfaces",
        "ls /sys/class/net/ 2>/dev/null || ifconfig -l 2>/dev/null | tr ' ' '\\n'");

    /* Serial ports */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "serial_ports",
        "ls /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM* /dev/ttyS[0-3] 2>/dev/null | sort -u");

    /* I2C buses */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "i2c_buses",
        "ls /dev/i2c-* 2>/dev/null");

    /* SPI devices */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "spi_devices",
        "ls /dev/spidev* 2>/dev/null");

    /* GPIO chip (gpiochip for libgpiod) */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "gpio_chips",
        "ls /dev/gpiochip* 2>/dev/null");

    /* USB devices */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "usb_devices",
        "lsusb 2>/dev/null | sed 's/Bus [0-9]* Device [0-9]*: ID //' | head -20");

    /* Block devices */
    o += snprintf(buf + o, maxlen - o, ",");
    o += discover_devices(buf + o, maxlen - o, "block_devices",
        "lsblk -d -n -o NAME,SIZE,TYPE 2>/dev/null | head -10");

    /* WiFi (has wlan?) */
    int has_wifi = 0;
    fp = popen("ls /sys/class/net/ 2>/dev/null", "r");
    if (fp) {
        char line[64];
        while (fgets(line, sizeof(line), fp))
            if (strncmp(line, "wlan", 4) == 0) has_wifi = 1;
        pclose(fp);
    }
    o += snprintf(buf + o, maxlen - o, ",\"has_wifi\":%s", has_wifi ? "true" : "false");

    /* Bluetooth */
    int has_bt = 0;
    fp = popen("ls /sys/class/bluetooth/ 2>/dev/null", "r");
    if (fp) {
        char line[64];
        if (fgets(line, sizeof(line), fp) && line[0]) has_bt = 1;
        pclose(fp);
    }
    o += snprintf(buf + o, maxlen - o, ",\"has_bluetooth\":%s", has_bt ? "true" : "false");

    /* WireGuard kernel module */
    int has_wg = (system("modinfo wireguard >/dev/null 2>&1") == 0)
              || (system("test -e /sys/module/wireguard 2>/dev/null") == 0);
    o += snprintf(buf + o, maxlen - o, ",\"has_wireguard\":%s", has_wg ? "true" : "false");

    return o;
}

/* ===== Raw socket health check (no curl dependency) ===== */
static int health_check(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    const char *req = "GET /health HTTP/1.0\r\n\r\n";
    write(s, req, strlen(req));
    char buf[512];
    int n = read(s, buf, sizeof(buf) - 1);
    close(s);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return strstr(buf, "\"ok\":true") ? 0 : -1;
}

/* ===== Skills ===== */
/* Skills are loaded by #include. Each skill file defines a static skill_t
 * and calls skill_register() in an init function.
 * Example: #include "skills/sysmon.c"
 */
/* #include "skills/example.c" */

static void skills_init(void) {
    /* Call each skill's init function here when included above.
     * Example: example_init();
     */
}

/* ===== Request handler ===== */
static void handle(int fd, const char *ip) {
    http_req_t req;
    int rc = parse_request(fd, &req);
    if (rc == -1) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"bad request\"}");
        return;
    }
    if (rc == -2) {
        json_resp(fd, 413, "Payload Too Large",
            "{\"error\":\"body too large, max 64KB\"}");
        return;
    }

    char resp[RESP_BUF];

    /* === /health — ALWAYS PUBLIC === */
    if (strcmp(req.path, "/health") == 0 && strcmp(req.method, "GET") == 0) {
        long up = (long)(time(NULL) - g_start_time);
        struct utsname uts;
        const char *arch = "unknown";
        if (uname(&uts) == 0) arch = uts.machine;
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"uptime_sec\":%ld,\"type\":\"seed\",\"version\":\"%s\",\"seed\":true,\"arch\":\"%s\"}",
            up, VERSION, arch);
        json_resp(fd, 200, "OK", resp);
        goto done;
    }

    /* === Auth check === */
    if (!is_trusted(ip) && g_token[0] && strcmp(req.auth, g_token) != 0) {
        json_resp(fd, 401, "Unauthorized",
            "{\"error\":\"Authorization: Bearer <token> required\"}");
        event_add("auth fail from %s", ip);
        goto done;
    }

    /* === GET /capabilities === */
    if (strcmp(req.path, "/capabilities") == 0 && strcmp(req.method, "GET") == 0) {
        int o = 0;
        o += snprintf(resp + o, sizeof(resp) - o,
            "{\"type\":\"seed\",\"version\":\"%s\",\"seed\":true,", VERSION);
        o += hw_discover(resp + o, sizeof(resp) - o);
        o += snprintf(resp + o, sizeof(resp) - o,
            ",\"endpoints\":["
            "\"/health\",\"/capabilities\",\"/config.md\",\"/events\","
            "\"/firmware/version\",\"/firmware/source\",\"/firmware/build\","
            "\"/firmware/build/logs\",\"/firmware/apply\","
            "\"/firmware/apply/reset\",\"/skill\"");
        /* Append skill endpoints */
        for (int si = 0; si < g_skill_count; si++) {
            const skill_endpoint_t *ep = g_skills[si]->endpoints;
            if (!ep) continue;
            for (; ep->path; ep++)
                o += snprintf(resp + o, sizeof(resp) - o, ",\"%s\"", ep->path);
        }
        o += snprintf(resp + o, sizeof(resp) - o, "]}");
        json_resp(fd, 200, "OK", resp);
        goto done;
    }

    /* === GET /events === */
    if (strcmp(req.path, "/events") == 0 || strncmp(req.path, "/events?", 8) == 0) {
        if (strcmp(req.method, "GET") != 0) {
            json_resp(fd, 405, "Method Not Allowed", "{\"error\":\"GET only\"}");
            goto done;
        }
        time_t since = 0;
        char *q = strchr(req.path, '?');
        if (q) { char *s = strstr(q, "since="); if (s) since = (time_t)strtol(s+6, NULL, 10); }

        int start = (g_ev_count < MAX_EVENTS) ? 0 : g_ev_head;
        int o = 0;
        o += snprintf(resp + o, sizeof(resp) - o, "{\"events\":[");
        int first = 1;
        for (int i = 0; i < g_ev_count && o < (int)sizeof(resp) - 256; i++) {
            event_t *e = &g_events[(start + i) % MAX_EVENTS];
            if (e->ts <= since) continue;
            char esc[256];
            json_escape(e->msg, esc, sizeof(esc));
            o += snprintf(resp + o, sizeof(resp) - o, "%s{\"t\":%ld,\"event\":\"%s\"}",
                first ? "" : ",", (long)e->ts, esc);
            first = 0;
        }
        o += snprintf(resp + o, sizeof(resp) - o, "],\"count\":%d}", g_ev_count);
        json_resp(fd, 200, "OK", resp);
        goto done;
    }

    /* === GET /config.md === */
    if (strcmp(req.path, "/config.md") == 0 && strcmp(req.method, "GET") == 0) {
        char *cfg = file_read(CONFIG_FILE, NULL);
        text_resp(fd, 200, "OK", cfg ? cfg : "# Seed Node\n\nNo config yet.\n");
        free(cfg);
        goto done;
    }

    /* === POST /config.md === */
    if (strcmp(req.path, "/config.md") == 0 && strcmp(req.method, "POST") == 0) {
        if (!req.body || req.body_len == 0) {
            json_resp(fd, 400, "Bad Request", "{\"error\":\"empty body\"}");
            goto done;
        }
        mkdir(INSTALL_DIR, 0755);
        if (file_write(CONFIG_FILE, req.body, req.body_len) < 0) {
            json_resp(fd, 500, "Error", "{\"error\":\"write failed\"}");
            goto done;
        }
        event_add("config.md updated (%d bytes)", req.body_len);
        json_resp(fd, 200, "OK", "{\"ok\":true}");
        goto done;
    }

    /* === GET /firmware/version === */
    if (strcmp(req.path, "/firmware/version") == 0 && strcmp(req.method, "GET") == 0) {
        long up = (long)(time(NULL) - g_start_time);
        snprintf(resp, sizeof(resp),
            "{\"version\":\"%s\",\"build_date\":\"%s %s\",\"uptime_sec\":%ld,\"seed\":true}",
            VERSION, __DATE__, __TIME__, up);
        json_resp(fd, 200, "OK", resp);
        goto done;
    }

    /* === GET /firmware/source === */
    if (strcmp(req.path, "/firmware/source") == 0 && strcmp(req.method, "GET") == 0) {
        long sz = 0;
        char *src = file_read(SOURCE_FILE, &sz);
        if (!src) {
            /* Fallback: try to read own binary's source from __FILE__ */
            src = file_read(__FILE__, &sz);
        }
        if (src) {
            text_resp(fd, 200, "OK", src);
            free(src);
        } else {
            json_resp(fd, 404, "Not Found", "{\"error\":\"source not found\"}");
        }
        goto done;
    }

    /* === POST /firmware/source === */
    if (strcmp(req.path, "/firmware/source") == 0 && strcmp(req.method, "POST") == 0) {
        if (!req.body || req.body_len == 0) {
            json_resp(fd, 400, "Bad Request", "{\"error\":\"empty body\"}");
            goto done;
        }
        mkdir(INSTALL_DIR, 0755);
        if (file_write(SOURCE_FILE, req.body, req.body_len) < 0) {
            json_resp(fd, 500, "Error", "{\"error\":\"write failed\"}");
            goto done;
        }
        event_add("firmware source updated (%d bytes)", req.body_len);
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"bytes\":%d,\"path\":\"%s\"}", req.body_len, SOURCE_FILE);
        json_resp(fd, 200, "OK", resp);
        goto done;
    }

    /* === POST /firmware/build === */
    if (strcmp(req.path, "/firmware/build") == 0 && strcmp(req.method, "POST") == 0) {
        struct stat st;
        if (stat(SOURCE_FILE, &st) != 0) {
            json_resp(fd, 400, "Bad Request",
                "{\"ok\":false,\"error\":\"source not found, POST /firmware/source first\"}");
            goto done;
        }

        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "gcc -O2 -o %s %s > %s 2>&1",
            BINARY_NEW, SOURCE_FILE, BUILD_LOG);

        signal(SIGCHLD, SIG_DFL);
        int rc2 = system(cmd);
        signal(SIGCHLD, SIG_IGN);
        int exit_code = WIFEXITED(rc2) ? WEXITSTATUS(rc2) : -1;

        if (exit_code == 0) {
            struct stat ns;
            stat(BINARY_NEW, &ns);
            event_add("build OK (%ld bytes)", (long)ns.st_size);
            snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"exit_code\":0,\"binary\":\"%s\",\"size_bytes\":%ld}",
                BINARY_NEW, (long)ns.st_size);
            json_resp(fd, 200, "OK", resp);
        } else {
            long logsz = 0;
            char *logs = file_read(BUILD_LOG, &logsz);
            char esc[8192];
            json_escape(logs ? logs : "no output", esc, sizeof(esc));
            free(logs);
            event_add("build FAILED (exit %d)", exit_code);
            snprintf(resp, sizeof(resp),
                "{\"ok\":false,\"exit_code\":%d,\"errors\":\"%s\"}", exit_code, esc);
            json_resp(fd, 200, "OK", resp);
        }
        goto done;
    }

    /* === GET /firmware/build/logs === */
    if ((strcmp(req.path, "/firmware/build/logs") == 0 ||
         strcmp(req.path, "/firmware/build/log") == 0) &&
        strcmp(req.method, "GET") == 0) {
        char *logs = file_read(BUILD_LOG, NULL);
        text_resp(fd, 200, "OK", logs ? logs : "(no build log)");
        free(logs);
        goto done;
    }

    /* === POST /firmware/apply === */
    if (strcmp(req.path, "/firmware/apply") == 0 && strcmp(req.method, "POST") == 0) {
        /* Safe mode: 3 consecutive failures block apply */
        if (apply_failures_read() >= 3) {
            json_resp(fd, 423, "Locked",
                "{\"ok\":false,\"error\":\"apply locked after 3 failures, "
                "POST /firmware/apply/reset with recovery token to unlock\"}");
            goto done;
        }

        struct stat st;
        if (stat(BINARY_NEW, &st) != 0) {
            json_resp(fd, 400, "Bad Request",
                "{\"ok\":false,\"error\":\"no built binary, POST /firmware/build first\"}");
            goto done;
        }

        /* Backup current binary */
        FILE *src = fopen(BINARY_FILE, "rb");
        if (src) {
            FILE *dst = fopen(BINARY_BACKUP, "wb");
            if (dst) {
                char cpbuf[4096];
                size_t n;
                while ((n = fread(cpbuf, 1, sizeof(cpbuf), src)) > 0)
                    fwrite(cpbuf, 1, n, dst);
                fclose(dst);
                chmod(BINARY_BACKUP, 0755);
            }
            fclose(src);
        }

        /* Atomic swap */
        if (rename(BINARY_NEW, BINARY_FILE) != 0) {
            rename(BINARY_BACKUP, BINARY_FILE);
            json_resp(fd, 500, "Error", "{\"ok\":false,\"error\":\"rename failed\"}");
            goto done;
        }
        chmod(BINARY_FILE, 0755);
        event_add("firmware apply: new binary installed, restarting");

        json_resp(fd, 200, "OK",
            "{\"ok\":true,\"warning\":\"restarting with watchdog, 10s grace period\"}");

        /* Fork watchdog */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: watchdog process */
            for (int i = 3; i < 1024; i++) close(i);
            sleep(1);

            /* Try systemctl first, fall back to fork+exec */
            int has_systemd = (system("systemctl is-active " SERVICE_NAME " >/dev/null 2>&1") == 0);
            if (has_systemd) {
                system("systemctl restart " SERVICE_NAME);
            } else {
                /* No systemd: kill parent, fork+exec new binary ourselves */
                pid_t ppid = getppid();
                kill(ppid, SIGTERM);
                sleep(1);
                /* Start new binary */
                pid_t child = fork();
                if (child == 0) {
                    setsid();
                    char port_str[8];
                    snprintf(port_str, sizeof(port_str), "%d", g_port);
                    execl(BINARY_FILE, BINARY_FILE, port_str, (char *)NULL);
                    _exit(1);
                }
            }

            sleep(10);

            /* Health check (raw socket, no curl dependency) */
            int check = health_check(g_port);
            if (check != 0) {
                /* ROLLBACK */
                int fails = apply_failures_read() + 1;
                apply_failures_write(fails);
                fprintf(stderr, "[watchdog] health check FAILED (%d/3), rolling back\n", fails);
                char rb[256];
                snprintf(rb, sizeof(rb), "cp %s %s", BINARY_BACKUP, BINARY_FILE);
                system(rb);
                if (has_systemd) {
                    system("systemctl restart " SERVICE_NAME);
                } else {
                    /* Kill failed firmware, restart from backup */
                    system("pkill -f " BINARY_FILE);
                    sleep(1);
                    pid_t child = fork();
                    if (child == 0) {
                        setsid();
                        char port_str[8];
                        snprintf(port_str, sizeof(port_str), "%d", g_port);
                        execl(BINARY_FILE, BINARY_FILE, port_str, (char *)NULL);
                        _exit(1);
                    }
                }
            } else {
                fprintf(stderr, "[watchdog] health check OK, new firmware confirmed\n");
                apply_failures_reset();
                unlink(BINARY_BACKUP);
            }
            _exit(0);
        }
        if (pid < 0) {
            /* fork failed — rollback */
            char rb[256];
            snprintf(rb, sizeof(rb), "cp %s %s", BINARY_BACKUP, BINARY_FILE);
            system(rb);
            event_add("apply: fork failed, rolled back");
        }
        goto done;
    }

    /* === POST /firmware/apply/reset — unlock after 3 failures === */
    if (strcmp(req.path, "/firmware/apply/reset") == 0
        && strcmp(req.method, "POST") == 0) {
        int fails = apply_failures_read();
        if (fails == 0) {
            json_resp(fd, 200, "OK", "{\"ok\":true,\"message\":\"not locked\"}");
        } else {
            apply_failures_reset();
            event_add("apply lock reset (was %d failures)", fails);
            json_resp(fd, 200, "OK",
                "{\"ok\":true,\"message\":\"apply unlocked\"}");
        }
        goto done;
    }

    /* === GET /skill — generate AI agent skill file === */
    if (strcmp(req.path, "/skill") == 0 && strcmp(req.method, "GET") == 0) {
        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname));

        /* Find first real network IP (skip loopback, docker, veth) */
        char my_ip[INET_ADDRSTRLEN] = "localhost";
        struct ifaddrs *ifa_list, *ifa;
        if (getifaddrs(&ifa_list) == 0) {
            for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING)) continue;
                if (strcmp(ifa->ifa_name, "lo") == 0) continue;
                if (strncmp(ifa->ifa_name, "docker", 6) == 0) continue;
                if (strncmp(ifa->ifa_name, "veth", 4) == 0) continue;
                if (strncmp(ifa->ifa_name, "br-", 3) == 0) continue;
                inet_ntop(AF_INET,
                    &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                    my_ip, sizeof(my_ip));
                break;
            }
            freeifaddrs(ifa_list);
        }

        int skill_sz = 16384;
        char *skill = malloc(skill_sz);
        if (!skill) { json_resp(fd, 500, "Error", "{\"error\":\"alloc\"}"); goto done; }
        int sk = 0;
        sk += snprintf(skill + sk, skill_sz - sk,
            "# Seed Node: %s\n\n"
            "Hardware node accessible via HTTP. "
            "This is a **seed** — a minimal firmware that you can grow "
            "by uploading new C source code.\n\n"
            "## Connection\n\n"
            "```\n"
            "Host: %s:%d\n"
            "Token: %s\n"
            "```\n\n"
            "All requests (except /health) require:\n"
            "`Authorization: Bearer %s`\n\n"
            "## Endpoints\n\n"
            "| Method | Path | Description |\n"
            "|--------|------|-------------|\n"
            "| GET | /health | Alive check (no auth) |\n"
            "| GET | /capabilities | Hardware: CPU, RAM, disk, GPIO, I2C, USB |\n"
            "| GET | /config.md | Node configuration (markdown) |\n"
            "| POST | /config.md | Update configuration |\n"
            "| GET | /events | Event log (?since=UNIX_TS) |\n"
            "| GET | /firmware/version | Version, build date, uptime |\n"
            "| GET | /firmware/source | Read current source code |\n"
            "| POST | /firmware/source | Upload new source (C code in body) |\n"
            "| POST | /firmware/build | Compile (gcc -O2) |\n"
            "| GET | /firmware/build/logs | Compilation output |\n"
            "| POST | /firmware/apply | Apply + restart (10s watchdog, auto-rollback) |\n"
            "| POST | /firmware/apply/reset | Unlock apply after 3 consecutive failures |\n"
            "| GET | /skill | This file |\n",
            hostname, my_ip, g_port, g_token, g_token);
        /* Append skill endpoints to table */
        for (int si = 0; si < g_skill_count; si++) {
            const skill_endpoint_t *ep = g_skills[si]->endpoints;
            if (!ep) continue;
            for (; ep->path; ep++)
                sk += snprintf(skill + sk, skill_sz - sk,
                    "| %s | %s | %s |\n", ep->method, ep->path, ep->description);
        }
        sk += snprintf(skill + sk, skill_sz - sk, "\n");
        /* Append skill descriptions */
        for (int si = 0; si < g_skill_count; si++) {
            if (!g_skills[si]->describe) continue;
            const char *desc = g_skills[si]->describe();
            if (desc)
                sk += snprintf(skill + sk, skill_sz - sk, "%s\n", desc);
        }
        sk += snprintf(skill + sk, skill_sz - sk,
            "## Growing the node\n\n"
            "This seed knows only the basics. To add capabilities:\n\n"
            "1. `GET /firmware/source` — read current code\n"
            "2. Write new firmware in C (add endpoints, features)\n"
            "3. `POST /firmware/source` — upload your code\n"
            "4. `POST /firmware/build` — compile on the node\n"
            "5. `GET /firmware/build/logs` — check for errors\n"
            "6. `POST /firmware/apply` — hot-swap (auto-rollback on failure)\n\n"
            "The node compiles itself. No cross-compilation needed.\n\n");
        sk += snprintf(skill + sk, skill_sz - sk,
            "## Constraints\n\n"
            "- **C only, libc only.** No external libraries. "
            "`gcc -O2 -o seed seed.c` must work with no flags.\n"
            "- **Single-threaded.** One request at a time. No pthreads.\n"
            "- **Max request body: 64KB.** For larger uploads, chunk them.\n"
            "- **Token persists in %s.** Survives restarts. "
            "Don't regenerate unless the user asks.\n"
            "- **Firmware path: %s.** Build output: %s. "
            "Apply does atomic rename + 10s watchdog.\n"
            "- **If apply fails 3 times**, /firmware/apply locks. "
            "Unlock with POST /firmware/apply/reset.\n\n",
            TOKEN_FILE, BINARY_FILE, BINARY_NEW);
        sk += snprintf(skill + sk, skill_sz - sk,
            "## Endpoint handler pattern\n\n"
            "When writing new firmware, follow this pattern for each endpoint:\n\n"
            "```c\n"
            "/* In handle() function — route matching */\n"
            "if (strcmp(req.path, \"/myendpoint\") == 0\n"
            "    && strcmp(req.method, \"GET\") == 0) {\n"
            "    /* Build JSON response */\n"
            "    char resp[4096];\n"
            "    snprintf(resp, sizeof(resp),\n"
            "        \"{\\\"key\\\":\\\"value\\\"}\");\n"
            "    json_resp(fd, 200, \"OK\", resp);\n"
            "    goto done;\n"
            "}\n"
            "```\n\n"
            "Key functions available: `json_resp(fd, code, status, json)`, "
            "`text_resp(fd, code, status, text)`, "
            "`respond(fd, code, status, content_type, body, len)`, "
            "`file_read(path, &len)`, `file_write(path, data, len)`, "
            "`cmd_out(shell_cmd, buf, bufsize)`, "
            "`event_add(fmt, ...)`.\n\n"
            "Request struct: `req.method`, `req.path`, `req.body` (malloc'd, may be NULL), "
            "`req.body_len`, `req.content_length`. Path includes query string. "
            "Auth is already checked before your handler runs "
            "(except /health which is public).\n\n");
        sk += snprintf(skill + sk, skill_sz - sk,
            "## Capabilities response example\n\n"
            "`GET /capabilities` returns hardware fingerprint:\n\n"
            "```json\n"
            "{\n"
            "  \"type\": \"seed\", \"version\": \"%s\", \"seed\": true,\n"
            "  \"hostname\": \"seed-01\", \"arch\": \"armv6l\",\n"
            "  \"os\": \"Linux\", \"kernel\": \"6.1.0\",\n"
            "  \"cpus\": 1, \"mem_mb\": 512, \"disk_mb\": 14000,\n"
            "  \"temp_c\": 42.3, \"board_model\": \"Raspberry Pi Zero W\",\n"
            "  \"has_gcc\": true,\n"
            "  \"net_interfaces\": [\"eth0\", \"wlan0\", \"usb0\"],\n"
            "  \"serial_ports\": [\"/dev/ttyAMA0\"],\n"
            "  \"i2c_buses\": [\"/dev/i2c-1\"],\n"
            "  \"gpio_chips\": [\"/dev/gpiochip0\"],\n"
            "  \"usb_devices\": [\"1d6b:0002 Linux Foundation 2.0 root hub\"],\n"
            "  \"has_wifi\": true, \"has_bluetooth\": true,\n"
            "  \"has_wireguard\": false,\n"
            "  \"endpoints\": [\"/health\", \"/capabilities\", ...]\n"
            "}\n"
            "```\n\n"
            "Use this to decide what the node can do and what firmware to write.\n\n",
            VERSION);
        sk += snprintf(skill + sk, skill_sz - sk,
            "## Quick test\n\n"
            "```bash\n"
            "curl http://%s:%d/health\n"
            "curl -H 'Authorization: Bearer %s' http://%s:%d/capabilities\n"
            "curl -H 'Authorization: Bearer %s' http://%s:%d/skill\n"
            "```\n",
            my_ip, g_port,
            g_token, my_ip, g_port,
            g_token, my_ip, g_port);
        respond(fd, 200, "OK", "text/markdown; charset=utf-8", skill, sk);
        free(skill);
        goto done;
    }

    /* === Skill handlers === */
    for (int si = 0; si < g_skill_count; si++) {
        if (g_skills[si]->handle && g_skills[si]->handle(fd, &req))
            goto done;
    }

    /* === 404 === */
    snprintf(resp, sizeof(resp),
        "{\"error\":\"not found\",\"path\":\"%s\",\"hint\":\"GET /capabilities for API list\"}",
        req.path);
    json_resp(fd, 404, "Not Found", resp);

done:
    free(req.body);
}

/* ===== Main ===== */
int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    g_port = port;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    g_start_time = time(NULL);

    mkdir(INSTALL_DIR, 0755);
    token_load();
    skills_init();

    /* Socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv, 8) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "\n");
    fprintf(stderr, "  Seed v%s\n", VERSION);
    fprintf(stderr, "  Token: %s\n\n", g_token);

    /* Show all network addresses for easy connection */
    {
        struct ifaddrs *ifa_list, *ifa;
        if (getifaddrs(&ifa_list) == 0) {
            fprintf(stderr, "  Connect:\n");
            for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                if (strcmp(ifa->ifa_name, "lo") == 0) continue;
                char addr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET,
                    &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                    addr, sizeof(addr));
                fprintf(stderr, "    http://%s:%d/health\n", addr, port);
            }
            freeifaddrs(ifa_list);
        }
        fprintf(stderr, "    http://localhost:%d/health\n", port);
    }

    fprintf(stderr, "\n  Skill file: GET /skill\n");
    fprintf(stderr, "  API list:   GET /capabilities\n\n");

    event_add("seed started on port %d", port);

    /* Accept loop */
    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client = accept(srv, (struct sockaddr *)&cli, &cli_len);
        if (client < 0) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        struct timeval tv = { .tv_sec = SOCK_TIMEOUT };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle(client, ip);
        close(client);
    }
}
