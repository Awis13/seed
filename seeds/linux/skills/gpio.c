/*
 * skills/gpio.c — GPIO skill for Linux seed
 *
 * Uses the kernel character device ABI (/dev/gpiochipN) via ioctl.
 * No libgpiod dependency — works on any Linux with CONFIG_GPIO_CDEV.
 *
 * Endpoints:
 *   GET  /gpio/chips       — list available GPIO chips
 *   GET  /gpio/lines       — list all lines on a chip (?chip=0)
 *   GET  /gpio/read        — read line value (?chip=0&line=17)
 *   POST /gpio/write       — set output value {chip, line, value}
 *   POST /gpio/mode        — set line direction {chip, line, mode}
 *
 * Pin numbering: BCM-style via gpiochip + line offset.
 *   On Raspberry Pi: chip=0, line=BCM pin number.
 */

#include <sys/ioctl.h>

/* --- GPIO kernel ABI v2 (linux/gpio.h) --- */

#define GPIO_MAX_NAME_SIZE 32

/* ioctl numbers */
#define GPIO_GET_CHIPINFO_IOCTL     _IOR(0xB4, 0x01, struct gpiochip_info)
#define GPIO_GET_LINEINFO_IOCTL     _IOWR(0xB4, 0x05, struct gpio_v2_line_info)
#define GPIO_V2_GET_LINE_IOCTL      _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_GET_VALUES_IOCTL _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#define GPIO_V2_LINE_SET_VALUES_IOCTL _IOWR(0xB4, 0x0F, struct gpio_v2_line_values)

/* Flags */
#define GPIO_V2_LINE_FLAG_USED          (1UL << 0)
#define GPIO_V2_LINE_FLAG_ACTIVE_LOW    (1UL << 1)
#define GPIO_V2_LINE_FLAG_INPUT         (1UL << 2)
#define GPIO_V2_LINE_FLAG_OUTPUT        (1UL << 3)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP  (1UL << 8)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN (1UL << 9)

struct gpiochip_info {
    char name[GPIO_MAX_NAME_SIZE];
    char label[GPIO_MAX_NAME_SIZE];
    uint32_t lines;
};

struct gpio_v2_line_info {
    char name[GPIO_MAX_NAME_SIZE];
    char consumer[GPIO_MAX_NAME_SIZE];
    uint32_t offset;
    uint32_t num_attrs;
    uint64_t flags;
    uint64_t attrs[10 * 2];  /* simplified: attr entries */
    uint32_t padding[4];
};

struct gpio_v2_line_config {
    uint64_t flags;
    uint32_t num_attrs;
    uint32_t padding[5];
    uint64_t attrs[10 * 3];
};

struct gpio_v2_line_request {
    uint32_t offsets[64];
    char consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    uint32_t num_lines;
    uint32_t event_buffer_size;
    uint32_t padding[5];
    int32_t fd;
};

struct gpio_v2_line_values {
    uint64_t bits;
    uint64_t mask;
};

/* --- JSON helpers (self-contained, no dependency on other skills) --- */

static int gpio_json_str(const char *json, const char *key, char *out, int maxlen) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *p = strstr(json, search);
    if (!p) return 0;
    p = strchr(p + strlen(search), '"');
    if (!p) return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static int gpio_json_int(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *p = strstr(json, search);
    if (!p) return -1;
    p = strchr(p + strlen(search), ':');
    if (!p) return -1;
    return atoi(p + 1);
}

/* --- GPIO tracking --- */

#define GPIO_MAX_CHIPS    4
#define GPIO_MAX_HELD     16

typedef struct {
    int      chip;
    uint32_t line;
    int      fd;        /* line request fd */
    uint64_t flags;     /* GPIO_V2_LINE_FLAG_* */
} gpio_held_t;

static gpio_held_t g_gpio_held[GPIO_MAX_HELD];
static int g_gpio_held_count = 0;

/* Find or open a line request */
static int gpio_find_held(int chip, uint32_t line) {
    for (int i = 0; i < g_gpio_held_count; i++) {
        if (g_gpio_held[i].chip == chip && g_gpio_held[i].line == line)
            return i;
    }
    return -1;
}

static int gpio_open_chip(int chip_num) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/gpiochip%d", chip_num);
    return open(path, O_RDWR | O_CLOEXEC);
}

/* Request a line with given flags */
static int gpio_request_line(int chip_num, uint32_t line, uint64_t flags) {
    /* Release if already held */
    int idx = gpio_find_held(chip_num, line);
    if (idx >= 0) {
        close(g_gpio_held[idx].fd);
        g_gpio_held[idx] = g_gpio_held[--g_gpio_held_count];
    }

    if (g_gpio_held_count >= GPIO_MAX_HELD) return -1;

    int chip_fd = gpio_open_chip(chip_num);
    if (chip_fd < 0) return -1;

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = line;
    req.num_lines = 1;
    req.config.flags = flags;
    snprintf(req.consumer, GPIO_MAX_NAME_SIZE, "seed");

    int ret = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip_fd);
    if (ret < 0) return -1;

    gpio_held_t *h = &g_gpio_held[g_gpio_held_count++];
    h->chip = chip_num;
    h->line = line;
    h->fd = req.fd;
    h->flags = flags;
    return g_gpio_held_count - 1;
}

/* --- Endpoint list --- */

static const skill_endpoint_t gpio_endpoints[] = {
    { "GET",  "/gpio/chips", "List GPIO chips" },
    { "GET",  "/gpio/lines", "List lines on chip (?chip=0)" },
    { "GET",  "/gpio/read",  "Read line (?chip=0&line=17)" },
    { "POST", "/gpio/write", "Write line {chip, line, value}" },
    { "POST", "/gpio/mode",  "Set direction {chip, line, mode}" },
    { NULL, NULL, NULL }
};

static const char *gpio_describe(void) {
    return "## Skill: gpio\n\n"
           "GPIO control via Linux character device ABI (`/dev/gpiochipN`).\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /gpio/chips | List available GPIO chips |\n"
           "| GET | /gpio/lines | List all lines on a chip: `?chip=0` |\n"
           "| GET | /gpio/read | Read line value: `?chip=0&line=17` |\n"
           "| POST | /gpio/write | Set output: `{\"chip\":0,\"line\":17,\"value\":1}` |\n"
           "| POST | /gpio/mode | Set direction: `{\"chip\":0,\"line\":17,\"mode\":\"output\"}` |\n\n"
           "### Modes\n\n"
           "input, output, input_pullup, input_pulldown\n\n"
           "### Pin numbering\n\n"
           "Uses gpiochip + line offset. On Raspberry Pi: chip=0, line=BCM pin.\n"
           "Example: BCM17 = `chip=0, line=17`.\n\n"
           "### Workflow\n\n"
           "1. `GET /gpio/chips` — find available chips\n"
           "2. `GET /gpio/lines?chip=0` — see all pins and their state\n"
           "3. `POST /gpio/mode` — configure as input or output\n"
           "4. `GET /gpio/read` or `POST /gpio/write` — interact\n";
}

/* --- Query helpers --- */

static int gpio_query_int(const char *path, const char *key, int defval) {
    char *q = strchr(path, '?');
    if (!q) return defval;
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    char *p = strstr(q, search);
    if (!p) return defval;
    return atoi(p + strlen(search));
}

/* --- Handlers --- */

/* GET /gpio/chips — list GPIO chips */
static int gpio_handle_chips(int fd) {
    char buf[2048];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "[");
    int first = 1;

    for (int i = 0; i < GPIO_MAX_CHIPS; i++) {
        int chip_fd = gpio_open_chip(i);
        if (chip_fd < 0) continue;

        struct gpiochip_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
            close(chip_fd);
            continue;
        }
        close(chip_fd);

        if (!first) off += snprintf(buf + off, sizeof(buf) - off, ",");
        first = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
            "{\"chip\":%d,\"name\":\"%s\",\"label\":\"%s\",\"lines\":%u}",
            i, info.name, info.label, info.lines);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]");

    if (first) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"no GPIO chips found\"}");
    } else {
        json_resp(fd, 200, "OK", buf);
    }
    return 1;
}

/* GET /gpio/lines?chip=0 — list all lines */
static int gpio_handle_lines(int fd, http_req_t *req) {
    int chip_num = gpio_query_int(req->path, "chip", 0);
    int chip_fd = gpio_open_chip(chip_num);
    if (chip_fd < 0) {
        json_resp(fd, 404, "Not Found",
            "{\"error\":\"chip not found\"}");
        return 1;
    }

    struct gpiochip_info chip_info;
    memset(&chip_info, 0, sizeof(chip_info));
    if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
        close(chip_fd);
        json_resp(fd, 500, "Error", "{\"error\":\"ioctl failed\"}");
        return 1;
    }

    int cap = chip_info.lines * 256 + 128;
    char *buf = malloc(cap);
    if (!buf) { close(chip_fd); json_resp(fd, 500, "Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(buf + off, cap - off,
        "{\"chip\":%d,\"name\":\"%s\",\"lines\":[",
        chip_num, chip_info.name);
    int first = 1;

    for (uint32_t i = 0; i < chip_info.lines && off < cap - 512; i++) {
        struct gpio_v2_line_info li;
        memset(&li, 0, sizeof(li));
        li.offset = i;
        if (ioctl(chip_fd, GPIO_GET_LINEINFO_IOCTL, &li) < 0) continue;

        const char *dir = "unknown";
        if (li.flags & GPIO_V2_LINE_FLAG_INPUT) dir = "input";
        else if (li.flags & GPIO_V2_LINE_FLAG_OUTPUT) dir = "output";
        else if (!(li.flags & GPIO_V2_LINE_FLAG_USED)) dir = "free";

        int held = gpio_find_held(chip_num, i);

        if (!first) off += snprintf(buf + off, cap - off, ",");
        first = 0;
        off += snprintf(buf + off, cap - off,
            "{\"line\":%u,\"name\":\"%s\",\"consumer\":\"%s\","
            "\"direction\":\"%s\",\"used\":%s,\"held_by_seed\":%s}",
            i, li.name[0] ? li.name : "",
            li.consumer[0] ? li.consumer : "",
            dir,
            (li.flags & GPIO_V2_LINE_FLAG_USED) ? "true" : "false",
            held >= 0 ? "true" : "false");
    }

    off += snprintf(buf + off, cap - off, "]}");
    close(chip_fd);
    json_resp(fd, 200, "OK", buf);
    free(buf);
    return 1;
}

/* GET /gpio/read?chip=0&line=17 */
static int gpio_handle_read(int fd, http_req_t *req) {
    int chip_num = gpio_query_int(req->path, "chip", 0);
    int line = gpio_query_int(req->path, "line", -1);
    if (line < 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"line parameter required\"}");
        return 1;
    }

    /* If not held, request as input */
    int idx = gpio_find_held(chip_num, (uint32_t)line);
    if (idx < 0) {
        idx = gpio_request_line(chip_num, (uint32_t)line, GPIO_V2_LINE_FLAG_INPUT);
        if (idx < 0) {
            json_resp(fd, 500, "Error",
                "{\"error\":\"cannot request line — may be in use by another process\"}");
            return 1;
        }
    }

    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.mask = 1;
    if (ioctl(g_gpio_held[idx].fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
        json_resp(fd, 500, "Error", "{\"error\":\"read failed\"}");
        return 1;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"chip\":%d,\"line\":%d,\"value\":%d}",
        chip_num, line, (int)(vals.bits & 1));
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* POST /gpio/write {chip, line, value} */
static int gpio_handle_write(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    /* Use the gpio_json_int helper pattern */
    int chip_num = 0;
    if (gpio_json_int(req->body, "chip") >= 0)
        chip_num = gpio_json_int(req->body, "chip");
    int line = gpio_json_int(req->body, "line");
    int value = gpio_json_int(req->body, "value");

    if (line < 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"line required\"}");
        return 1;
    }
    if (value < 0) value = 0;

    /* Ensure line is held as output */
    int idx = gpio_find_held(chip_num, (uint32_t)line);
    if (idx < 0 || !(g_gpio_held[idx].flags & GPIO_V2_LINE_FLAG_OUTPUT)) {
        idx = gpio_request_line(chip_num, (uint32_t)line, GPIO_V2_LINE_FLAG_OUTPUT);
        if (idx < 0) {
            json_resp(fd, 500, "Error",
                "{\"error\":\"cannot request line as output\"}");
            return 1;
        }
    }

    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.mask = 1;
    vals.bits = value ? 1 : 0;
    if (ioctl(g_gpio_held[idx].fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
        json_resp(fd, 500, "Error", "{\"error\":\"write failed\"}");
        return 1;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"chip\":%d,\"line\":%d,\"value\":%d}",
        chip_num, line, value ? 1 : 0);
    json_resp(fd, 200, "OK", resp);
    event_add("gpio: set chip%d/line%d = %d", chip_num, line, value ? 1 : 0);
    return 1;
}

/* POST /gpio/mode {chip, line, mode} */
static int gpio_handle_mode(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    int chip_num = 0;
    if (gpio_json_int(req->body, "chip") >= 0)
        chip_num = gpio_json_int(req->body, "chip");
    int line = gpio_json_int(req->body, "line");
    if (line < 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"line required\"}");
        return 1;
    }

    char mode[32] = "";
    gpio_json_str(req->body, "mode", mode, sizeof(mode));
    if (!mode[0]) {
        json_resp(fd, 400, "Bad Request",
            "{\"error\":\"mode required (input, output, input_pullup, input_pulldown)\"}");
        return 1;
    }

    uint64_t flags = 0;
    if (strcmp(mode, "input") == 0)
        flags = GPIO_V2_LINE_FLAG_INPUT;
    else if (strcmp(mode, "output") == 0)
        flags = GPIO_V2_LINE_FLAG_OUTPUT;
    else if (strcmp(mode, "input_pullup") == 0)
        flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    else if (strcmp(mode, "input_pulldown") == 0)
        flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    else {
        json_resp(fd, 400, "Bad Request",
            "{\"error\":\"invalid mode — use input, output, input_pullup, input_pulldown\"}");
        return 1;
    }

    int idx = gpio_request_line(chip_num, (uint32_t)line, flags);
    if (idx < 0) {
        json_resp(fd, 500, "Error",
            "{\"error\":\"cannot request line — may be in use\"}");
        return 1;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"chip\":%d,\"line\":%d,\"mode\":\"%s\"}",
        chip_num, line, mode);
    json_resp(fd, 200, "OK", resp);
    event_add("gpio: chip%d/line%d → %s", chip_num, line, mode);
    return 1;
}

/* --- Router --- */

static int gpio_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->path, "/gpio/chips") == 0)
            return gpio_handle_chips(fd);
        if (strncmp(req->path, "/gpio/lines", 11) == 0 &&
            (req->path[11] == '\0' || req->path[11] == '?'))
            return gpio_handle_lines(fd, req);
        if (strncmp(req->path, "/gpio/read", 10) == 0 &&
            (req->path[10] == '\0' || req->path[10] == '?'))
            return gpio_handle_read(fd, req);
    }
    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(req->path, "/gpio/write") == 0)
            return gpio_handle_write(fd, req);
        if (strcmp(req->path, "/gpio/mode") == 0)
            return gpio_handle_mode(fd, req);
    }
    return 0;
}

static const skill_t gpio_skill = {
    .name      = "gpio",
    .version   = "0.1.0",
    .describe  = gpio_describe,
    .endpoints = gpio_endpoints,
    .handle    = gpio_handle
};

static void gpio_init(void) {
    skill_register(&gpio_skill);
}
