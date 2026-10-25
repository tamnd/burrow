// doc: http
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello, %s\n",
                  http_request_path_value(r, S("name")));
}

static void run(void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    http_handle_func(S("GET /hello/{name}"), FN(HttpHandlerFunc, hello, NULL));
    HttptestServer *ts = httptest_new_server(heap_allocator(), (HttpHandler){0});
    Error err = NO_ERROR;
    Str url = fmt_sprintf_v(a, "%s/hello/gopher", ts->url);
    HttpResponse *res = http_client_get(httptest_server_client(ts), url, &err);
    if (res != NULL) {
        Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("%s: %s", res->status, str_from_bytes(body.p, body.len));
        http_response_free(res);
    }
    httptest_server_free(ts);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
200 OK: hello, gopher
*/
