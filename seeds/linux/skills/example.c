/*
 * skills/example.c — Example skill for seed.c
 *
 * This file demonstrates the skill pattern. It is NOT included by default.
 * To enable: uncomment `#include "skills/example.c"` in seed.c
 *
 * Provides: GET /example -> {"ok":true,"skill":"example"}
 */

/* Endpoint list (NULL-terminated) */
static const skill_endpoint_t example_endpoints[] = {
    { "GET", "/example", "Example skill endpoint" },
    { NULL, NULL, NULL }
};

/* Describe: returns markdown for /skill output */
static const char *example_describe(void) {
    return "## Skill: example\n\n"
           "Demonstration skill. Returns `{\"ok\":true,\"skill\":\"example\"}` "
           "on `GET /example`.\n";
}

/* Handler: return 1 if handled, 0 to pass */
static int example_handle(int fd, http_req_t *req) {
    if (strcmp(req->path, "/example") == 0 && strcmp(req->method, "GET") == 0) {
        json_resp(fd, 200, "OK", "{\"ok\":true,\"skill\":\"example\"}");
        return 1;
    }
    return 0;
}

/* Skill definition */
static const skill_t example_skill = {
    .name      = "example",
    .version   = "0.1.0",
    .describe  = example_describe,
    .endpoints = example_endpoints,
    .handle    = example_handle
};

/* Init: call from main or from a skills_init() function */
static void example_init(void) {
    skill_register(&example_skill);
}
