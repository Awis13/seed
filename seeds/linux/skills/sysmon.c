/*
 * skills/sysmon.c — System monitoring skill for seed.c
 *
 * Provides system information, process list, disk usage, and log viewing.
 * To enable: #include "skills/sysmon.c" in seed.c, call sysmon_init()
 *
 * Endpoints:
 *   GET /system/info      — hostname, arch, kernel, cpu, memory, disk, uptime, load, temp
 *   GET /system/processes  — top processes by CPU usage (JSON array)
 *   GET /system/disk       — mounted filesystems (JSON array)
 *   GET /system/logs       — last N lines of syslog/journal (plain text, ?n=50)
 */

/* Endpoint list (NULL-terminated) */
static const skill_endpoint_t sysmon_endpoints[] = {
    { "GET", "/system/info",      "System info: hostname, arch, kernel, cpus, memory, disk, uptime, load, temp" },
    { "GET", "/system/processes", "Top processes sorted by CPU usage" },
    { "GET", "/system/disk",      "Mounted filesystems with usage" },
    { "GET", "/system/logs",      "Recent syslog/journal entries (?n=50)" },
    { NULL, NULL, NULL }
};

/* Describe: returns markdown for /skill output */
static const char *sysmon_describe(void) {
    return "## Skill: sysmon\n\n"
           "System monitoring skill. Provides:\n\n"
           "- `GET /system/info` — JSON with hostname, arch, os, kernel, cpus, "
           "mem_mb, mem_free_mb, disk_mb, disk_free_mb, uptime, load_1/5/15, temp_c\n"
           "- `GET /system/processes` — JSON array of top processes (pid, user, cpu, mem, command)\n"
           "- `GET /system/disk` — JSON array of mounted filesystems (device, mountpoint, total/used/free_mb, percent_used)\n"
           "- `GET /system/logs` — Plain text, last N lines of system log (?n=50 to set count)\n";
}

/* --- /system/info --- */
static int sysmon_handle_info(int fd) {
    char buf[512];
    char esc[512];
    int off = 0;
    int cap = 8192;
    char *resp = malloc(cap);
    if (!resp) { json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    off += snprintf(resp + off, cap - off, "{");

    /* hostname */
    cmd_out("hostname", buf, sizeof(buf));
    json_escape(buf, esc, sizeof(esc));
    off += snprintf(resp + off, cap - off, "\"hostname\":\"%s\"", esc);

    /* arch */
    cmd_out("uname -m", buf, sizeof(buf));
    json_escape(buf, esc, sizeof(esc));
    off += snprintf(resp + off, cap - off, ",\"arch\":\"%s\"", esc);

    /* os */
    cmd_out("cat /etc/os-release 2>/dev/null | grep '^PRETTY_NAME=' | cut -d'\"' -f2", buf, sizeof(buf));
    if (buf[0] == '\0') cmd_out("uname -o", buf, sizeof(buf));
    json_escape(buf, esc, sizeof(esc));
    off += snprintf(resp + off, cap - off, ",\"os\":\"%s\"", esc);

    /* kernel */
    cmd_out("uname -r", buf, sizeof(buf));
    json_escape(buf, esc, sizeof(esc));
    off += snprintf(resp + off, cap - off, ",\"kernel\":\"%s\"", esc);

    /* cpus */
    cmd_out("nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 0", buf, sizeof(buf));
    off += snprintf(resp + off, cap - off, ",\"cpus\":%d", atoi(buf));

    /* memory total/free (in MB) */
    cmd_out("grep MemTotal /proc/meminfo 2>/dev/null | awk '{print int($2/1024)}'", buf, sizeof(buf));
    int mem_mb = atoi(buf);
    cmd_out("grep MemAvailable /proc/meminfo 2>/dev/null | awk '{print int($2/1024)}'", buf, sizeof(buf));
    int mem_free_mb = atoi(buf);
    off += snprintf(resp + off, cap - off, ",\"mem_mb\":%d,\"mem_free_mb\":%d", mem_mb, mem_free_mb);

    /* disk total/free for root filesystem (in MB) */
    cmd_out("df -BM / 2>/dev/null | tail -1 | awk '{gsub(/M/,\"\",$2); print $2}'", buf, sizeof(buf));
    int disk_mb = atoi(buf);
    cmd_out("df -BM / 2>/dev/null | tail -1 | awk '{gsub(/M/,\"\",$4); print $4}'", buf, sizeof(buf));
    int disk_free_mb = atoi(buf);
    off += snprintf(resp + off, cap - off, ",\"disk_mb\":%d,\"disk_free_mb\":%d", disk_mb, disk_free_mb);

    /* uptime in seconds */
    cmd_out("cat /proc/uptime 2>/dev/null | awk '{printf \"%.0f\", $1}'", buf, sizeof(buf));
    off += snprintf(resp + off, cap - off, ",\"uptime\":%s", buf[0] ? buf : "0");

    /* load averages */
    cmd_out("cat /proc/loadavg 2>/dev/null | awk '{print $1}'", buf, sizeof(buf));
    double load1 = buf[0] ? atof(buf) : 0.0;
    cmd_out("cat /proc/loadavg 2>/dev/null | awk '{print $2}'", buf, sizeof(buf));
    double load5 = buf[0] ? atof(buf) : 0.0;
    cmd_out("cat /proc/loadavg 2>/dev/null | awk '{print $3}'", buf, sizeof(buf));
    double load15 = buf[0] ? atof(buf) : 0.0;
    off += snprintf(resp + off, cap - off, ",\"load_1\":%.2f,\"load_5\":%.2f,\"load_15\":%.2f",
                    load1, load5, load15);

    /* temperature (millidegrees C -> degrees C, null if unavailable) */
    cmd_out("cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null", buf, sizeof(buf));
    if (buf[0] && atoi(buf) > 0) {
        double temp = atoi(buf) / 1000.0;
        off += snprintf(resp + off, cap - off, ",\"temp_c\":%.1f", temp);
    } else {
        off += snprintf(resp + off, cap - off, ",\"temp_c\":null");
    }

    off += snprintf(resp + off, cap - off, "}");

    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* --- /system/processes --- */
static int sysmon_handle_processes(int fd) {
    int cap = 32768;
    char *resp = malloc(cap);
    if (!resp) { json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "[");

    FILE *fp = popen("ps aux --sort=-pcpu 2>/dev/null | head -21", "r");
    if (!fp) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"ps failed\"}");
        free(resp);
        return 1;
    }

    char line[1024];
    int first = 1;
    int header_skipped = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        int l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

        /* Skip header line */
        if (!header_skipped) { header_skipped = 1; continue; }

        /* Parse: USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND */
        char user[64] = "", cmd_buf[512] = "";
        int pid = 0;
        float cpu = 0.0f, mem = 0.0f;

        /* sscanf the fixed fields, then grab command as remainder */
        char tty[32], stat[16], start[16], ttime[32];
        unsigned long vsz, rss;
        int n = sscanf(line, "%63s %d %f %f %lu %lu %31s %15s %15s %31s",
                        user, &pid, &cpu, &mem, &vsz, &rss, tty, stat, start, ttime);
        if (n < 10) continue;

        /* Command is everything after the 10th field */
        char *p = line;
        int fields = 0;
        while (fields < 10 && *p) {
            while (*p == ' ') p++;
            while (*p && *p != ' ') p++;
            fields++;
        }
        while (*p == ' ') p++;
        if (*p) {
            strncpy(cmd_buf, p, sizeof(cmd_buf) - 1);
            cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        }

        char esc_user[128], esc_cmd[1024];
        json_escape(user, esc_user, sizeof(esc_user));
        json_escape(cmd_buf, esc_cmd, sizeof(esc_cmd));

        /* Ensure buffer space */
        int need = strlen(esc_user) + strlen(esc_cmd) + 128;
        if (off + need >= cap) break;

        if (!first) off += snprintf(resp + off, cap - off, ",");
        first = 0;

        off += snprintf(resp + off, cap - off,
            "{\"pid\":%d,\"user\":\"%s\",\"cpu\":%.1f,\"mem\":%.1f,\"command\":\"%s\"}",
            pid, esc_user, (double)cpu, (double)mem, esc_cmd);
    }
    pclose(fp);

    off += snprintf(resp + off, cap - off, "]");

    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* --- /system/disk --- */
static int sysmon_handle_disk(int fd) {
    int cap = 16384;
    char *resp = malloc(cap);
    if (!resp) { json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "[");

    FILE *fp = popen("df -BM 2>/dev/null", "r");
    if (!fp) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"df failed\"}");
        free(resp);
        return 1;
    }

    char line[1024];
    int first = 1;
    int header_skipped = 0;

    while (fgets(line, sizeof(line), fp)) {
        int l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

        if (!header_skipped) { header_skipped = 1; continue; }

        /* Parse: Filesystem 1M-blocks Used Available Use% Mounted_on */
        char device[256], mountpoint[256], use_pct_str[16];
        unsigned long total = 0, used = 0, avail = 0;

        int n = sscanf(line, "%255s %luM %luM %luM %15s %255s",
                        device, &total, &used, &avail, use_pct_str, mountpoint);
        if (n < 6) continue;

        /* Skip pseudo-filesystems */
        if (strncmp(device, "/dev/", 5) != 0) continue;

        int pct = atoi(use_pct_str); /* "42%" -> 42 */

        char esc_dev[512], esc_mp[512];
        json_escape(device, esc_dev, sizeof(esc_dev));
        json_escape(mountpoint, esc_mp, sizeof(esc_mp));

        int need = strlen(esc_dev) + strlen(esc_mp) + 128;
        if (off + need >= cap) break;

        if (!first) off += snprintf(resp + off, cap - off, ",");
        first = 0;

        off += snprintf(resp + off, cap - off,
            "{\"device\":\"%s\",\"mountpoint\":\"%s\",\"total_mb\":%lu,\"used_mb\":%lu,\"free_mb\":%lu,\"percent_used\":%d}",
            esc_dev, esc_mp, total, used, avail, pct);
    }
    pclose(fp);

    off += snprintf(resp + off, cap - off, "]");

    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* --- /system/logs --- */
static int sysmon_handle_logs(int fd, http_req_t *req) {
    /* Parse ?n=NN from query string, default 50 */
    int count = 50;
    char *q = strchr(req->path, '?');
    if (q) {
        char *np = strstr(q, "n=");
        if (np) {
            int v = atoi(np + 2);
            if (v > 0 && v <= 1000) count = v;
        }
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "journalctl -n %d --no-pager 2>/dev/null || "
        "tail -%d /var/log/syslog 2>/dev/null || "
        "tail -%d /var/log/messages 2>/dev/null || "
        "echo 'No system logs available'",
        count, count, count);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        text_resp(fd, 500, "Internal Server Error", "Failed to read logs\n");
        return 1;
    }

    /* Read all output into a buffer */
    int cap = 65536;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); text_resp(fd, 500, "Internal Server Error", "malloc failed\n"); return 1; }

    int off = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        int l = strlen(line);
        if (off + l >= cap - 1) break;
        memcpy(buf + off, line, l);
        off += l;
    }
    buf[off] = '\0';
    pclose(fp);

    text_resp(fd, 200, "OK", buf);
    free(buf);
    return 1;
}

/* Handler: return 1 if handled, 0 to pass */
static int sysmon_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") != 0) return 0;

    if (strcmp(req->path, "/system/info") == 0)
        return sysmon_handle_info(fd);

    if (strcmp(req->path, "/system/processes") == 0)
        return sysmon_handle_processes(fd);

    if (strcmp(req->path, "/system/disk") == 0)
        return sysmon_handle_disk(fd);

    if (strcmp(req->path, "/system/logs") == 0 || strncmp(req->path, "/system/logs?", 13) == 0)
        return sysmon_handle_logs(fd, req);

    return 0;
}

/* Skill definition */
static const skill_t sysmon_skill = {
    .name      = "sysmon",
    .version   = "0.1.0",
    .describe  = sysmon_describe,
    .endpoints = sysmon_endpoints,
    .handle    = sysmon_handle
};

/* Init: call from skills_init() */
static void sysmon_init(void) {
    skill_register(&sysmon_skill);
}
