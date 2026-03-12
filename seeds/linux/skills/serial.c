/*
 * skills/serial.c — Serial port skill for Linux seed
 *
 * Turns any Linux box (Pi, VPS, SBC) into a serial-to-HTTP bridge.
 * AI agent talks HTTP, device talks UART. Plug a $10 Pi Zero into
 * any serial port and that device is now AI-accessible.
 *
 * Endpoints:
 *   GET  /serial/ports         — list available serial ports
 *   POST /serial/open          — open port {port, baud, databits, stopbits, parity}
 *   POST /serial/write         — send data {port, data} (data is string or hex)
 *   GET  /serial/read?port=P   — read buffered data (&timeout=ms, &lines=N)
 *   POST /serial/close         — close port {port}
 *
 * Supports: /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS*, /dev/ttyAMA*
 */

#include <termios.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <poll.h>

/* --- Port tracking --- */
#define SERIAL_MAX_PORTS 8
#define SERIAL_BUF_SIZE  4096
#define SERIAL_PATH_LEN  64

typedef struct {
    char path[SERIAL_PATH_LEN];  /* /dev/ttyUSB0 */
    int  fd;                     /* file descriptor, -1 if closed */
    int  baud;                   /* current baud rate */
    char buf[SERIAL_BUF_SIZE];   /* circular read buffer */
    int  buf_head;               /* write position */
    int  buf_count;              /* bytes in buffer */
} serial_port_t;

static serial_port_t serial_ports[SERIAL_MAX_PORTS];
static int serial_port_count = 0;

/* Find tracked port by path, or NULL */
static serial_port_t *serial_find(const char *path) {
    for (int i = 0; i < serial_port_count; i++) {
        if (strcmp(serial_ports[i].path, path) == 0 && serial_ports[i].fd >= 0)
            return &serial_ports[i];
    }
    return NULL;
}

/* Map baud rate integer to termios constant */
static speed_t serial_baud_const(int baud) {
    switch (baud) {
        case 300:    return B300;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return B0; /* invalid */
    }
}

/* Drain any pending data from an open port into its buffer */
static void serial_drain(serial_port_t *sp) {
    struct pollfd pfd = { .fd = sp->fd, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char tmp[256];
        int n = read(sp->fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            int pos = (sp->buf_head) % SERIAL_BUF_SIZE;
            sp->buf[pos] = tmp[i];
            sp->buf_head = (sp->buf_head + 1) % SERIAL_BUF_SIZE;
            if (sp->buf_count < SERIAL_BUF_SIZE) sp->buf_count++;
        }
    }
}

/* Read buffer contents as a string (consumes the buffer) */
static int serial_buf_read(serial_port_t *sp, char *out, int maxlen) {
    serial_drain(sp);
    if (sp->buf_count == 0) return 0;

    int start = (sp->buf_head - sp->buf_count + SERIAL_BUF_SIZE) % SERIAL_BUF_SIZE;
    int n = sp->buf_count;
    if (n > maxlen - 1) n = maxlen - 1;

    for (int i = 0; i < n; i++) {
        out[i] = sp->buf[(start + i) % SERIAL_BUF_SIZE];
    }
    out[n] = '\0';
    sp->buf_count -= n;
    return n;
}

/* --- Endpoints --- */
static const skill_endpoint_t serial_endpoints[] = {
    { "GET",  "/serial/ports", "List available serial ports" },
    { "POST", "/serial/open",  "Open serial port {port, baud, databits, stopbits, parity}" },
    { "POST", "/serial/write", "Send data {port, data}" },
    { "GET",  "/serial/read",  "Read buffered data (?port=P&timeout=ms)" },
    { "POST", "/serial/close", "Close serial port {port}" },
    { NULL, NULL, NULL }
};

static const char *serial_describe(void) {
    return "## Skill: serial\n\n"
           "Serial port bridge — talk to UART devices over HTTP.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /serial/ports | List available serial ports with metadata |\n"
           "| POST | /serial/open | Open port: `{\"port\":\"/dev/ttyUSB0\",\"baud\":9600}` |\n"
           "| POST | /serial/write | Send data: `{\"port\":\"/dev/ttyUSB0\",\"data\":\"Hello\\n\"}` |\n"
           "| GET | /serial/read?port=/dev/ttyUSB0 | Read buffered data (&timeout=1000 for blocking) |\n"
           "| POST | /serial/close | Close port: `{\"port\":\"/dev/ttyUSB0\"}` |\n\n"
           "### Baud rates\n\n"
           "300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600\n\n"
           "### Notes\n\n"
           "- Scans /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS*, /dev/ttyAMA*\n"
           "- Default: 9600 8N1 (8 data bits, no parity, 1 stop bit)\n"
           "- Read buffer is 4KB per port, circular — old data is overwritten\n"
           "- Up to 8 ports can be open simultaneously\n";
}

/* GET /serial/ports */
static int serial_handle_ports(int fd) {
    DIR *dir = opendir("/dev");
    if (!dir) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"cannot open /dev\"}");
        return 1;
    }

    int cap = 8192;
    char *resp = malloc(cap);
    if (!resp) { closedir(dir); json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "[");
    int first = 1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        /* Match ttyUSB*, ttyACM*, ttyS*, ttyAMA* */
        if (strncmp(ent->d_name, "ttyUSB", 6) != 0 &&
            strncmp(ent->d_name, "ttyACM", 6) != 0 &&
            strncmp(ent->d_name, "ttyS", 4) != 0 &&
            strncmp(ent->d_name, "ttyAMA", 6) != 0)
            continue;

        char path[SERIAL_PATH_LEN];
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);

        /* Check if accessible */
        int accessible = (access(path, R_OK | W_OK) == 0) ? 1 : 0;

        /* Check if already open by us */
        serial_port_t *sp = serial_find(path);
        const char *status = sp ? "open" : "available";

        char esc_path[128];
        json_escape(path, esc_path, sizeof(esc_path));

        if (!first) off += snprintf(resp + off, cap - off, ",");
        first = 0;

        off += snprintf(resp + off, cap - off,
            "{\"port\":\"%s\",\"status\":\"%s\",\"accessible\":%s",
            esc_path, status, accessible ? "true" : "false");

        if (sp) {
            off += snprintf(resp + off, cap - off, ",\"baud\":%d,\"buffered\":%d",
                sp->baud, sp->buf_count);
        }

        /* Try to identify device type */
        if (strncmp(ent->d_name, "ttyUSB", 6) == 0)
            off += snprintf(resp + off, cap - off, ",\"type\":\"usb-serial\"");
        else if (strncmp(ent->d_name, "ttyACM", 6) == 0)
            off += snprintf(resp + off, cap - off, ",\"type\":\"usb-acm\"");
        else if (strncmp(ent->d_name, "ttyAMA", 6) == 0)
            off += snprintf(resp + off, cap - off, ",\"type\":\"hardware-uart\"");
        else if (strncmp(ent->d_name, "ttyS", 4) == 0)
            off += snprintf(resp + off, cap - off, ",\"type\":\"hardware-uart\"");

        off += snprintf(resp + off, cap - off, "}");

        if (off >= cap - 256) break;
    }
    closedir(dir);

    off += snprintf(resp + off, cap - off, "]");
    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* POST /serial/open — {port, baud, databits, stopbits, parity} */
static int serial_handle_open(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    /* Minimal JSON parsing — find "port", "baud" */
    char port[SERIAL_PATH_LEN] = "";
    int baud = 9600;
    int databits = 8;
    int stopbits = 1;
    char parity = 'N';

    /* Extract port */
    char *p = strstr(req->body, "\"port\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) {
            p++;
            int i = 0;
            while (*p && *p != '"' && i < SERIAL_PATH_LEN - 1) port[i++] = *p++;
            port[i] = '\0';
        }
    }

    /* Extract baud */
    p = strstr(req->body, "\"baud\"");
    if (p) {
        p = strchr(p + 6, ':');
        if (p) baud = atoi(p + 1);
    }

    /* Extract databits */
    p = strstr(req->body, "\"databits\"");
    if (p) {
        p = strchr(p + 10, ':');
        if (p) databits = atoi(p + 1);
    }

    /* Extract stopbits */
    p = strstr(req->body, "\"stopbits\"");
    if (p) {
        p = strchr(p + 10, ':');
        if (p) stopbits = atoi(p + 1);
    }

    /* Extract parity */
    p = strstr(req->body, "\"parity\"");
    if (p) {
        p = strchr(p + 8, '"');
        if (p && *(p + 1)) parity = *(p + 1);
    }

    if (port[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port required\"}");
        return 1;
    }

    /* Validate port path (prevent path traversal) */
    if (strncmp(port, "/dev/tty", 8) != 0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port must be /dev/tty*\"}");
        return 1;
    }
    if (strstr(port, "..")) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"invalid port path\"}");
        return 1;
    }

    /* Check if already open */
    if (serial_find(port)) {
        json_resp(fd, 409, "Conflict", "{\"error\":\"port already open\"}");
        return 1;
    }

    /* Check slot availability */
    int slot = -1;
    for (int i = 0; i < SERIAL_MAX_PORTS; i++) {
        if (serial_ports[i].fd < 0) { slot = i; break; }
    }
    if (slot < 0 && serial_port_count < SERIAL_MAX_PORTS) {
        slot = serial_port_count++;
    }
    if (slot < 0) {
        json_resp(fd, 507, "Insufficient Storage", "{\"error\":\"max ports open (8)\"}");
        return 1;
    }

    /* Validate baud */
    speed_t baud_c = serial_baud_const(baud);
    if (baud_c == B0) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"unsupported baud rate\"}");
        return 1;
    }

    /* Open the port */
    int sfd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (sfd < 0) {
        char err[256];
        snprintf(err, sizeof(err), "{\"error\":\"cannot open %s: %s\"}", port, strerror(errno));
        json_resp(fd, 500, "Internal Server Error", err);
        return 1;
    }

    /* Configure termios */
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tcgetattr(sfd, &tio);

    /* Baud rate */
    cfsetispeed(&tio, baud_c);
    cfsetospeed(&tio, baud_c);

    /* Data bits */
    tio.c_cflag &= ~CSIZE;
    switch (databits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }

    /* Stop bits */
    if (stopbits == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    /* Parity */
    switch (parity) {
        case 'E': case 'e':
            tio.c_cflag |= PARENB;
            tio.c_cflag &= ~PARODD;
            break;
        case 'O': case 'o':
            tio.c_cflag |= PARENB;
            tio.c_cflag |= PARODD;
            break;
        default: /* None */
            tio.c_cflag &= ~PARENB;
            break;
    }

    /* Raw mode — no echo, no signals, no processing */
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tio.c_oflag &= ~OPOST;

    /* Non-blocking read */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    tcflush(sfd, TCIOFLUSH);
    tcsetattr(sfd, TCSANOW, &tio);

    /* Track */
    serial_port_t *sp = &serial_ports[slot];
    strncpy(sp->path, port, SERIAL_PATH_LEN - 1);
    sp->path[SERIAL_PATH_LEN - 1] = '\0';
    sp->fd = sfd;
    sp->baud = baud;
    sp->buf_head = 0;
    sp->buf_count = 0;

    event_add("serial: opened %s at %d baud", port, baud);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"port\":\"%s\",\"baud\":%d,\"databits\":%d,\"stopbits\":%d,\"parity\":\"%c\"}",
        port, baud, databits, stopbits, parity);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* POST /serial/write — {port, data} */
static int serial_handle_write(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    char port[SERIAL_PATH_LEN] = "";
    char *p = strstr(req->body, "\"port\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) { p++; int i = 0; while (*p && *p != '"' && i < SERIAL_PATH_LEN - 1) port[i++] = *p++; port[i] = '\0'; }
    }

    if (port[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port required\"}");
        return 1;
    }

    serial_port_t *sp = serial_find(port);
    if (!sp) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"port not open\"}");
        return 1;
    }

    /* Extract data string — handle escape sequences */
    p = strstr(req->body, "\"data\"");
    if (!p) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"data required\"}");
        return 1;
    }
    p = strchr(p + 6, '"');
    if (!p) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"data must be a string\"}");
        return 1;
    }
    p++;

    /* Decode JSON string (handle \n, \r, \t, \\) */
    char data[4096];
    int di = 0;
    while (*p && *p != '"' && di < (int)sizeof(data) - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': data[di++] = '\n'; break;
                case 'r': data[di++] = '\r'; break;
                case 't': data[di++] = '\t'; break;
                case '\\': data[di++] = '\\'; break;
                case '"': data[di++] = '"'; break;
                default: data[di++] = '\\'; data[di++] = *p; break;
            }
        } else {
            data[di++] = *p;
        }
        p++;
    }
    data[di] = '\0';

    int written = write(sp->fd, data, di);
    if (written < 0) {
        char err[256];
        snprintf(err, sizeof(err), "{\"error\":\"write failed: %s\"}", strerror(errno));
        json_resp(fd, 500, "Internal Server Error", err);
        return 1;
    }

    event_add("serial: wrote %d bytes to %s", written, port);

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"port\":\"%s\",\"bytes_written\":%d}", port, written);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* GET /serial/read?port=P&timeout=ms */
static int serial_handle_read(int fd, http_req_t *req) {
    /* Parse query params */
    char port[SERIAL_PATH_LEN] = "";
    int timeout_ms = 0;

    char *q = strchr(req->path, '?');
    if (q) {
        char *pp = strstr(q, "port=");
        if (pp) {
            pp += 5;
            int i = 0;
            /* URL-decode /dev/ paths: %2F -> / */
            while (*pp && *pp != '&' && i < SERIAL_PATH_LEN - 1) {
                if (*pp == '%' && *(pp+1) == '2' && (*(pp+2) == 'F' || *(pp+2) == 'f')) {
                    port[i++] = '/';
                    pp += 3;
                } else {
                    port[i++] = *pp++;
                }
            }
            port[i] = '\0';
        }
        char *tp = strstr(q, "timeout=");
        if (tp) {
            int v = atoi(tp + 8);
            if (v > 0 && v <= 10000) timeout_ms = v;
        }
    }

    if (port[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port parameter required\"}");
        return 1;
    }

    serial_port_t *sp = serial_find(port);
    if (!sp) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"port not open\"}");
        return 1;
    }

    /* If timeout specified, wait for data */
    if (timeout_ms > 0 && sp->buf_count == 0) {
        struct pollfd pfd = { .fd = sp->fd, .events = POLLIN };
        poll(&pfd, 1, timeout_ms);
    }

    char data[SERIAL_BUF_SIZE + 1];
    int n = serial_buf_read(sp, data, sizeof(data));

    /* JSON-escape the data */
    char esc_data[SERIAL_BUF_SIZE * 2];
    json_escape(data, esc_data, sizeof(esc_data));

    int cap = sizeof(esc_data) + 256;
    char *resp = malloc(cap);
    if (!resp) { json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    snprintf(resp, cap,
        "{\"port\":\"%s\",\"bytes\":%d,\"data\":\"%s\"}",
        port, n, esc_data);
    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* POST /serial/close — {port} */
static int serial_handle_close(int fd, http_req_t *req) {
    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    char port[SERIAL_PATH_LEN] = "";
    char *p = strstr(req->body, "\"port\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) { p++; int i = 0; while (*p && *p != '"' && i < SERIAL_PATH_LEN - 1) port[i++] = *p++; port[i] = '\0'; }
    }

    if (port[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port required\"}");
        return 1;
    }

    serial_port_t *sp = serial_find(port);
    if (!sp) {
        json_resp(fd, 404, "Not Found", "{\"error\":\"port not open\"}");
        return 1;
    }

    close(sp->fd);
    sp->fd = -1;
    sp->buf_count = 0;
    sp->buf_head = 0;

    event_add("serial: closed %s", port);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"port\":\"%s\"}", port);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* Handler dispatch */
static int serial_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->path, "/serial/ports") == 0)
            return serial_handle_ports(fd);
        if (strncmp(req->path, "/serial/read", 12) == 0)
            return serial_handle_read(fd, req);
    }
    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(req->path, "/serial/open") == 0)
            return serial_handle_open(fd, req);
        if (strcmp(req->path, "/serial/write") == 0)
            return serial_handle_write(fd, req);
        if (strcmp(req->path, "/serial/close") == 0)
            return serial_handle_close(fd, req);
    }
    return 0;
}

/* Skill definition */
static const skill_t serial_skill = {
    .name      = "serial",
    .version   = "0.1.0",
    .describe  = serial_describe,
    .endpoints = serial_endpoints,
    .handle    = serial_handle
};

/* Init */
static void serial_init(void) {
    for (int i = 0; i < SERIAL_MAX_PORTS; i++)
        serial_ports[i].fd = -1;
    skill_register(&serial_skill);
}
