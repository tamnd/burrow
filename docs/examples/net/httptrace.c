#include "burrow/burrow.h"

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    Error err;
    io_write_string(http_response_writer_as_io_writer(w), BURROW_S("hello\n"), &err);
}

// doc: hooks
static void get_conn(void *env, Str host_port) {
    (void)env;
    (void)host_port;
    fmt_printf_v("get conn\n");
}

static void got_conn(void *env, HttptraceGotConnInfo info) {
    (void)env;
    fmt_printf_v("got conn, reused: %t\n", info.reused);
}

static void wrote_headers(void *env) {
    (void)env;
    fmt_printf_v("wrote headers\n");
}

static void first_byte(void *env) {
    (void)env;
    fmt_printf_v("first response byte\n");
}

static void put_idle_conn(void *env, Error err) {
    (void)env;
    fmt_printf_v("put idle conn: %v\n", err);
}
// doc: end

static void run(void *env) {
    Alloc *a = env;
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, hello, NULL);
    HttptestServer *ts =
        httptest_new_server(heap_allocator(), http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    // doc: trace
    HttptraceClientTrace trace = {
        .get_conn = BURROW_FN(HttptraceGetConnFunc, get_conn, NULL),
        .got_conn = BURROW_FN(HttptraceGotConnFunc, got_conn, NULL),
        .wrote_headers = BURROW_FN(Func, wrote_headers, NULL),
        .got_first_response_byte = BURROW_FN(Func, first_byte, NULL),
        .put_idle_conn = BURROW_FN(HttptracePutIdleConnFunc, put_idle_conn, NULL),
    };
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);

    /* The second request gets the connection the first one left idle. */
    for (int i = 0; i < 2; i++) {
        Error err;
        IoReader none = {NULL, NULL};
        HttpRequest *req =
            http_new_request_with_context(a, ctx, BURROW_S("GET"), ts->url, none, &err);
        HttpResponse *res = http_client_do(c, req, &err);
        if (res == NULL) {
            fmt_printf_v("error: %v\n", err);
            break;
        }
        Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        Str status = str_clone(a, res->status);
        http_response_free(res);
        fmt_printf_v("%s: %s", status, str_from_bytes(b.p, b.len));
    }
    context_release(ctx);
    // doc: end
    httptest_server_free(ts);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}

/* Output:
get conn
got conn, reused: false
wrote headers
first response byte
put idle conn: <nil>
200 OK: hello
get conn
got conn, reused: true
wrote headers
first response byte
put idle conn: <nil>
200 OK: hello
*/
