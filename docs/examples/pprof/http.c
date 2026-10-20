#include "burrow/burrow.h"

/* Serves one request with the default mux and prints what came back. */
static void get(Alloc *a, const char *path) {
    HttpRequest *r =
        httptest_new_request(a, BURROW_S("GET"), str_from_cstr(path), (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    if (r == NULL || rec == NULL)
        return;
    http_handler_serve_http(http_serve_mux_as_handler(http_default_serve_mux),
                            httptest_response_recorder_as_response_writer(rec), r);
    HttpHeader h = rec->header_map;
    fmt_printf_v("%s %d %s", path, rec->code,
                 http_header_get(h, BURROW_S("Content-Type")));
    Str disposition = http_header_get(h, BURROW_S("Content-Disposition"));
    if (disposition.len > 0)
        fmt_printf_v(" %s", disposition);
    fmt_printf_v("\n");
    httptest_response_recorder_free(rec);
    http_request_free(r);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: serve
    http_pprof_register();

    get(a, "/debug/pprof/");
    get(a, "/debug/pprof/heap");
    get(a, "/debug/pprof/heap?debug=1");
    get(a, "/debug/pprof/nothing");
    // doc: end

    // doc: index
    HttpRequest *r = httptest_new_request(a, BURROW_S("GET"), BURROW_S("/debug/pprof/"),
                                          (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    if (r == NULL || rec == NULL)
        return 1;
    http_pprof_index(NULL, httptest_response_recorder_as_response_writer(rec), r);
    Slice body = bytes_buffer_bytes(rec->body);
    Str rest = str_from_bytes(body.p, body.len);
    while (rest.len > 0) {
        bool found;
        Str line = strings_cut(rest, BURROW_S("\n"), &rest, &found);
        Str name =
            strings_cut_prefix(line, BURROW_S("<li><div class=profile-name>"), &found);
        if (found)
            fmt_printf_v("%s\n", strings_cut(name, BURROW_S(":"), NULL, &found));
    }
    httptest_response_recorder_free(rec);
    http_request_free(r);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
/debug/pprof/ 200 text/html; charset=utf-8
/debug/pprof/heap 200 application/octet-stream attachment; filename="heap"
/debug/pprof/heap?debug=1 200 text/plain; charset=utf-8
/debug/pprof/nothing 404 text/plain; charset=utf-8
allocs
block
cmdline
goroutine
heap
mutex
profile
symbol
threadcreate
trace
*/
