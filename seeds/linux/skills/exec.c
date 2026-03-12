/*
 * skills/exec.c — Command execution skill for Linux seed
 *
 * The most powerful skill: run any command on the device.
 * AI agent sends a shell command, gets stdout, stderr, exit code.
 *
 * Endpoints:
 *   POST /exec  — execute command {cmd, timeout}
 *                  returns {ok, exit_code, stdout, stderr}
 */

#include <sys/wait.h>

#define EXEC_MAX_OUTPUT  65536
#define EXEC_DEFAULT_TIMEOUT 30

static const skill_endpoint_t exec_endpoints[] = {
    { "POST", "/exec", "Execute shell command {cmd, timeout}" },
    { NULL, NULL, NULL }
};

static const char *exec_describe(void) {
    return "## Skill: exec\n\n"
           "Execute shell commands on the device.\n\n"
           "### Endpoints\n\n"
           "| Method | Path | Description |\n"
           "|--------|------|-------------|\n"
           "| POST | /exec | Execute: `{\"cmd\":\"ls -la /tmp\",\"timeout\":30}` |\n\n"
           "### Response\n\n"
           "```json\n"
           "{\"ok\":true,\"exit_code\":0,\"stdout\":\"...\",\"stderr\":\"...\"}\n"
           "```\n\n"
           "### Notes\n\n"
           "- Default timeout: 30 seconds (max 300)\n"
           "- stdout and stderr returned separately\n"
           "- Output truncated at 64KB per stream\n"
           "- Commands run as the seed process user\n";
}

static int exec_handle(int fd, http_req_t *req) {
    if (strcmp(req->path, "/exec") != 0 || strcmp(req->method, "POST") != 0)
        return 0;

    if (!req->body) {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"no body\"}");
        return 1;
    }

    /* Parse cmd */
    char cmd[4096] = "";
    int timeout = EXEC_DEFAULT_TIMEOUT;

    char *p = strstr(req->body, "\"cmd\"");
    if (p) {
        p = strchr(p + 5, '"');
        if (p) {
            p++;
            int i = 0;
            while (*p && i < (int)sizeof(cmd) - 1) {
                if (*p == '\\' && *(p+1) == '"') { cmd[i++] = '"'; p += 2; }
                else if (*p == '\\' && *(p+1) == '\\') { cmd[i++] = '\\'; p += 2; }
                else if (*p == '\\' && *(p+1) == 'n') { cmd[i++] = '\n'; p += 2; }
                else if (*p == '"') break;
                else cmd[i++] = *p++;
            }
            cmd[i] = '\0';
        }
    }

    p = strstr(req->body, "\"timeout\"");
    if (p) {
        p = strchr(p + 9, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v > 0 && v <= 300) timeout = v;
        }
    }

    if (cmd[0] == '\0') {
        json_resp(fd, 400, "Bad Request", "{\"error\":\"cmd required\"}");
        return 1;
    }

    event_add("exec: %.*s", 80, cmd);

    /* Create pipes for stdout and stderr */
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"pipe failed\"}");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"fork failed\"}");
        return 1;
    }

    if (pid == 0) {
        /* Child */
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }

    /* Parent */
    close(out_pipe[1]);
    close(err_pipe[1]);

    char *out_buf = malloc(EXEC_MAX_OUTPUT + 1);
    char *err_buf = malloc(EXEC_MAX_OUTPUT + 1);
    int out_len = 0, err_len = 0;

    if (!out_buf || !err_buf) {
        if (out_buf) free(out_buf);
        if (err_buf) free(err_buf);
        close(out_pipe[0]); close(err_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}");
        return 1;
    }

    /* Read stdout and stderr with timeout using poll */
    struct pollfd pfds[2] = {
        { .fd = out_pipe[0], .events = POLLIN },
        { .fd = err_pipe[0], .events = POLLIN }
    };

    time_t deadline = time(NULL) + timeout;
    int open_fds = 2;

    while (open_fds > 0) {
        int remaining = (int)(deadline - time(NULL));
        if (remaining <= 0) {
            kill(pid, SIGKILL);
            break;
        }

        int ret = poll(pfds, 2, remaining * 1000);
        if (ret <= 0) {
            kill(pid, SIGKILL);
            break;
        }

        if (pfds[0].revents & POLLIN) {
            int n = read(out_pipe[0], out_buf + out_len,
                         EXEC_MAX_OUTPUT - out_len);
            if (n > 0) out_len += n;
            else { pfds[0].fd = -1; open_fds--; }
        } else if (pfds[0].revents & (POLLHUP | POLLERR)) {
            pfds[0].fd = -1; open_fds--;
        }

        if (pfds[1].revents & POLLIN) {
            int n = read(err_pipe[0], err_buf + err_len,
                         EXEC_MAX_OUTPUT - err_len);
            if (n > 0) err_len += n;
            else { pfds[1].fd = -1; open_fds--; }
        } else if (pfds[1].revents & (POLLHUP | POLLERR)) {
            pfds[1].fd = -1; open_fds--;
        }
    }

    out_buf[out_len] = '\0';
    err_buf[err_len] = '\0';
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* Build JSON response */
    int esc_out_cap = out_len * 2 + 1;
    int esc_err_cap = err_len * 2 + 1;
    char *esc_out = malloc(esc_out_cap);
    char *esc_err = malloc(esc_err_cap);

    if (!esc_out || !esc_err) {
        free(out_buf); free(err_buf);
        if (esc_out) free(esc_out);
        if (esc_err) free(esc_err);
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}");
        return 1;
    }

    json_escape(out_buf, esc_out, esc_out_cap);
    json_escape(err_buf, esc_err, esc_err_cap);
    free(out_buf);
    free(err_buf);

    int resp_cap = strlen(esc_out) + strlen(esc_err) + 256;
    char *resp = malloc(resp_cap);
    if (!resp) {
        free(esc_out); free(esc_err);
        json_resp(fd, 500, "Internal Server Error", "{\"error\":\"malloc\"}");
        return 1;
    }

    snprintf(resp, resp_cap,
        "{\"ok\":true,\"exit_code\":%d,\"stdout\":\"%s\",\"stderr\":\"%s\"}",
        exit_code, esc_out, esc_err);

    free(esc_out);
    free(esc_err);

    json_resp(fd, 200, "OK", resp);
    free(resp);
    return 1;
}

static const skill_t exec_skill = {
    .name      = "exec",
    .version   = "0.1.0",
    .describe  = exec_describe,
    .endpoints = exec_endpoints,
    .handle    = exec_handle
};

static void exec_init(void) {
    skill_register(&exec_skill);
}
