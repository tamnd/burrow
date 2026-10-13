#include <stdio.h>

#include "burrow/burrow.h"

// doc: func
/* How many jobs are waiting, worked out again each time the variables are
 * read. */
static Int queued = 3;

static Any queue_depth(void *env, Alloc *a) {
    (void)env;
    Int *n = BURROW_NEW(a, Int);
    if (n == NULL)
        return (Any){NULL, NULL};
    *n = queued;
    return (Any){TYPE_INT, n};
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: publish
    ExpvarInt *requests = expvar_new_int(BURROW_S("requests"));
    ExpvarMap *by_path = expvar_new_map(BURROW_S("by_path"));
    ExpvarString *version = expvar_new_string(BURROW_S("version"));
    if (requests == NULL || by_path == NULL || version == NULL ||
        !expvar_string_set(version, BURROW_S("1.4.2")))
        return 1;

    const char *paths[] = {"/", "/about", "/", "/"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        expvar_int_add(requests, 1);
        expvar_map_add(by_path, str_from_cstr(paths[i]), 1);
    }
    fmt_printf_v("requests %s\n", expvar_int_string(requests, a));
    fmt_printf_v("by_path %s\n", expvar_map_string(by_path, a));
    // doc: end

    // doc: publish-func
    ExpvarFunc queue = BURROW_FN(ExpvarFunc, queue_depth, NULL);
    expvar_publish(BURROW_S("queue"), expvar_func_as_var(&queue));
    queued = 5;
    fmt_printf_v("queue %s\n", expvar_var_string(expvar_get(BURROW_S("queue")), a));
    // doc: end

    // doc: serve
    expvar_register();

    HttpRequest *r = httptest_new_request(a, BURROW_S("GET"), BURROW_S("/debug/vars"),
                                          (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    http_handler_serve_http(http_serve_mux_as_handler(http_default_serve_mux),
                            httptest_response_recorder_as_response_writer(rec), r);
    fmt_printf_v("%d %s\n", rec->code,
                 http_header_get(rec->header_map, BURROW_S("Content-Type")));

    /* cmdline and memstats change from run to run, so only their names. */
    Slice body = bytes_buffer_bytes(rec->body);
    Str rest = str_from_bytes(body.p, body.len);
    while (rest.len > 0) {
        bool found;
        Str line = strings_cut(rest, BURROW_S("\n"), &rest, &found);
        if (strings_has_prefix(line, BURROW_S("\"cmdline\":")) ||
            strings_has_prefix(line, BURROW_S("\"memstats\":")))
            line = strings_cut(line, BURROW_S(" "), NULL, &found);
        fmt_printf_v("%s\n", line);
    }
    httptest_response_recorder_free(rec);
    http_request_free(r);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
*/
