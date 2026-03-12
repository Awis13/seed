/*
 * skills/net.c — Network reconnaissance skill for Linux seed
 *
 * AI agent lands on a node and wants to know: what else is on this network?
 * This skill provides network scanning, interface listing, and port probing.
 *
 * Endpoints:
 *   GET  /net/interfaces       — list network interfaces with IPs
 *   GET  /net/scan             — ARP scan of local subnet
 *   GET  /net/ping?host=X     — ping a host
 *   GET  /net/probe?host=X&port=N — TCP connect probe
 */

static const skill_endpoint_t net_endpoints[] = {
    { "GET", "/net/interfaces", "List network interfaces with IPs and MACs" },
    { "GET", "/net/scan",       "ARP/ping scan of local subnet" },
    { "GET", "/net/ping",       "Ping host (?host=X&count=3)" },
    { "GET", "/net/probe",      "TCP port probe (?host=X&port=N&timeout=2)" },
    { NULL, NULL, NULL }
};

static const char *net_describe(void) {
    return "## Skill: net\n\n"
           "Network reconnaissance — discover what's on the network.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| GET | /net/interfaces | List interfaces: name, IP, MAC, state |\n"
           "| GET | /net/scan | ARP scan local subnet (uses arping or ping sweep) |\n"
           "| GET | /net/ping?host=1.2.3.4 | Ping: `{host, alive, rtt_ms, ttl}` (&count=3) |\n"
           "| GET | /net/probe?host=X&port=80 | TCP probe: `{open, rtt_ms}` (&timeout=2) |\n\n"
           "### Notes\n\n"
           "- Scan covers /24 of each non-loopback interface\n"
           "- Probe timeout default: 2 seconds (max 10)\n"
           "- No raw sockets needed — uses standard tools and TCP connect\n";
}

/* GET /net/interfaces */
static int net_handle_interfaces(int fd) {
    struct ifaddrs *ifs, *ifa;
    if (getifaddrs(&ifs) < 0) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"getifaddrs failed\"}");
        return 1;
    }

    int cap = 8192;
    char *resp = malloc(cap);
    if (!resp) { freeifaddrs(ifs); json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "[");
    int first = 1;

    /* Track which interfaces we've already output (avoid dups from multiple addresses) */
    char seen[32][IF_NAMESIZE];
    int seen_count = 0;

    for (ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;

        /* Skip if we already output this interface */
        int skip = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen[i], ifa->ifa_name) == 0) { skip = 1; break; }
        }
        if (skip) continue;
        if (seen_count < 32) strncpy(seen[seen_count++], ifa->ifa_name, IF_NAMESIZE);

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

        /* Get MAC address */
        char mac[18] = "unknown";
        char mac_path[128];
        snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", ifa->ifa_name);
        FILE *mf = fopen(mac_path, "r");
        if (mf) {
            if (fgets(mac, sizeof(mac), mf)) {
                int l = strlen(mac);
                while (l > 0 && (mac[l-1] == '\n' || mac[l-1] == '\r')) mac[--l] = '\0';
            }
            fclose(mf);
        }

        /* Check state */
        char state[16] = "unknown";
        char state_path[128];
        snprintf(state_path, sizeof(state_path), "/sys/class/net/%s/operstate", ifa->ifa_name);
        FILE *sf = fopen(state_path, "r");
        if (sf) {
            if (fgets(state, sizeof(state), sf)) {
                int l = strlen(state);
                while (l > 0 && (state[l-1] == '\n' || state[l-1] == '\r')) state[--l] = '\0';
            }
            fclose(sf);
        }

        int is_up = (ifa->ifa_flags & IFF_UP) ? 1 : 0;
        int is_lo = (ifa->ifa_flags & IFF_LOOPBACK) ? 1 : 0;

        if (!first) off += snprintf(resp + off, cap - off, ",");
        first = 0;

        off += snprintf(resp + off, cap - off,
            "{\"name\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"state\":\"%s\","
            "\"up\":%s,\"loopback\":%s}",
            ifa->ifa_name, ip, mac, state,
            is_up ? "true" : "false", is_lo ? "true" : "false");

        if (off >= cap - 256) break;
    }
    freeifaddrs(ifs);

    off += snprintf(resp + off, cap - off, "]");
    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* GET /net/scan — ping sweep of local /24 */
static int net_handle_scan(int fd) {
    /* Find first non-loopback interface IP to determine subnet */
    struct ifaddrs *ifs, *ifa;
    if (getifaddrs(&ifs) < 0) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"getifaddrs failed\"}");
        return 1;
    }

    char subnet[32] = "";
    for (ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        /* Extract x.x.x. prefix */
        char *last_dot = strrchr(ip, '.');
        if (last_dot) {
            int prefix_len = last_dot - ip;
            strncpy(subnet, ip, prefix_len);
            subnet[prefix_len] = '\0';
        }
        break;
    }
    freeifaddrs(ifs);

    if (subnet[0] == '\0') {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"no non-loopback interface found\"}");
        return 1;
    }

    /* Use arp -an for instant results (no active scanning needed) */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "arp -an 2>/dev/null | grep '%s\\.'", subnet);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"arp failed\"}");
        return 1;
    }

    int cap = 16384;
    char *resp = malloc(cap);
    if (!resp) { pclose(fp); json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}"); return 1; }

    int off = 0;
    off += snprintf(resp + off, cap - off, "{\"subnet\":\"%s.0/24\",\"hosts\":[", subnet);
    int first = 1;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Parse arp output: ? (192.168.1.1) at aa:bb:cc:dd:ee:ff [ether] on wlan0 */
        char ip[64] = "", mac[32] = "";
        char *ip_start = strchr(line, '(');
        char *ip_end = strchr(line, ')');
        if (ip_start && ip_end && ip_end > ip_start) {
            int len = ip_end - ip_start - 1;
            if (len < (int)sizeof(ip)) { strncpy(ip, ip_start + 1, len); ip[len] = '\0'; }
        }

        char *at = strstr(line, " at ");
        if (at) {
            at += 4;
            int i = 0;
            while (*at && *at != ' ' && i < (int)sizeof(mac) - 1) mac[i++] = *at++;
            mac[i] = '\0';
        }

        if (ip[0] && mac[0] && strcmp(mac, "<incomplete>") != 0) {
            if (!first) off += snprintf(resp + off, cap - off, ",");
            first = 0;
            off += snprintf(resp + off, cap - off, "{\"ip\":\"%s\",\"mac\":\"%s\"}", ip, mac);
        }
        if (off >= cap - 256) break;
    }
    pclose(fp);

    off += snprintf(resp + off, cap - off, "]}");
    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

/* Extract query parameter value */
static int net_get_param(const char *url, const char *name, char *out, int maxlen) {
    char *q = strchr(url, '?');
    if (!q) return 0;
    char key[64];
    snprintf(key, sizeof(key), "%s=", name);
    char *p = strstr(q, key);
    if (!p) return 0;
    p += strlen(key);
    int i = 0;
    while (*p && *p != '&' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* GET /net/ping?host=X&count=N */
static int net_handle_ping(int fd, http_req_t *req) {
    char host[256] = "";
    char count_str[16] = "3";

    net_get_param(req->path, "host", host, sizeof(host));
    net_get_param(req->path, "count", count_str, sizeof(count_str));

    if (host[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"host parameter required\"}");
        return 1;
    }

    /* Validate host: only allow alnum, dots, dashes, colons (IPv6) */
    for (int i = 0; host[i]; i++) {
        char c = host[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':')) {
            json_resp(fd, 400, "Bad Request", "{\"error\":\"invalid host\"}");
            return 1;
        }
    }

    int count = atoi(count_str);
    if (count < 1) count = 1;
    if (count > 10) count = 10;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ping -c %d -W 2 '%s' 2>&1", count, host);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"ping failed\"}");
        return 1;
    }

    char output[4096] = "";
    int olen = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        int l = strlen(line);
        if (olen + l < (int)sizeof(output) - 1) {
            memcpy(output + olen, line, l);
            olen += l;
        }
    }
    output[olen] = '\0';
    int exit_code = pclose(fp);
    int alive = (WIFEXITED(exit_code) && WEXITSTATUS(exit_code) == 0) ? 1 : 0;

    /* Parse avg rtt from "rtt min/avg/max/mdev = X/Y/Z/W ms" */
    float rtt_avg = 0;
    char *rtt_line = strstr(output, "rtt min/avg/max");
    if (!rtt_line) rtt_line = strstr(output, "round-trip min/avg/max");
    if (rtt_line) {
        char *eq = strchr(rtt_line, '=');
        if (eq) {
            float min_v, avg_v;
            if (sscanf(eq + 2, "%f/%f", &min_v, &avg_v) >= 2)
                rtt_avg = avg_v;
        }
    }

    char esc_output[8192];
    json_escape(output, esc_output, sizeof(esc_output));

    char resp[8192 + 256];
    snprintf(resp, sizeof(resp),
        "{\"host\":\"%s\",\"alive\":%s,\"rtt_avg_ms\":%.1f,\"output\":\"%s\"}",
        host, alive ? "true" : "false", (double)rtt_avg, esc_output);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

/* GET /net/probe?host=X&port=N&timeout=S */
static int net_handle_probe(int fd, http_req_t *req) {
    char host[256] = "", port_str[16] = "", timeout_str[16] = "2";

    net_get_param(req->path, "host", host, sizeof(host));
    net_get_param(req->path, "port", port_str, sizeof(port_str));
    net_get_param(req->path, "timeout", timeout_str, sizeof(timeout_str));

    if (host[0] == '\0' || port_str[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"host and port parameters required\"}");
        return 1;
    }

    /* Validate host */
    for (int i = 0; host[i]; i++) {
        char c = host[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':')) {
            json_resp(fd, 400, "Bad Request", "{\"error\":\"invalid host\"}");
            return 1;
        }
    }

    int port = atoi(port_str);
    int timeout_sec = atoi(timeout_str);
    if (port < 1 || port > 65535) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"port must be 1-65535\"}");
        return 1;
    }
    if (timeout_sec < 1) timeout_sec = 1;
    if (timeout_sec > 10) timeout_sec = 10;

    /* TCP connect probe */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Try DNS resolution */
        char cmd[256], resolved[64] = "";
        snprintf(cmd, sizeof(cmd), "getent hosts '%s' 2>/dev/null | awk '{print $1}'", host);
        cmd_out(cmd, resolved, sizeof(resolved));
        if (resolved[0] == '\0' || inet_pton(AF_INET, resolved, &addr.sin_addr) <= 0) {
            json_resp(fd, 400, "Bad Request", "{\"error\":\"cannot resolve host\"}");
            return 1;
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"socket failed\"}");
        return 1;
    }

    /* Non-blocking connect with timeout */
    fcntl(sock, F_SETFL, O_NONBLOCK);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    int open_port = 0;
    float rtt_ms = 0;

    if (ret == 0) {
        open_port = 1;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        ret = poll(&pfd, 1, timeout_sec * 1000);
        if (ret > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            open_port = (err == 0) ? 1 : 0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    rtt_ms = (end.tv_sec - start.tv_sec) * 1000.0f +
             (end.tv_nsec - start.tv_nsec) / 1000000.0f;

    close(sock);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"host\":\"%s\",\"port\":%d,\"open\":%s,\"rtt_ms\":%.1f}",
        host, port, open_port ? "true" : "false", (double)rtt_ms);
    json_resp(fd, 200, "OK", resp);
    return 1;
}

static int net_handle(int fd, http_req_t *req) {
    if (strcmp(req->method, "GET") != 0) return 0;

    if (strcmp(req->path, "/net/interfaces") == 0)
        return net_handle_interfaces(fd);

    if (strcmp(req->path, "/net/scan") == 0)
        return net_handle_scan(fd);

    if (strncmp(req->path, "/net/ping", 9) == 0)
        return net_handle_ping(fd, req);

    if (strncmp(req->path, "/net/probe", 10) == 0)
        return net_handle_probe(fd, req);

    return 0;
}

static const skill_t net_skill = {
    .name      = "net",
    .version   = "0.1.0",
    .describe  = net_describe,
    .endpoints = net_endpoints,
    .handle    = net_handle
};

static void net_init(void) {
    skill_register(&net_skill);
}
