// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

typedef struct Serving {
    HttpServer *srv;
    NetListener l;
    Error err;
} Serving;

static void serve(void *env) {
    Serving *s = env;
    s->err = http_server_serve(s->srv, s->l);
}

/* Fetches base + path and prints what came back. */
static void get(Alloc *a, Str base, const char *path) {
    Error err = NO_ERROR;
    Str url = fmt_sprintf_v(a, "%s%s", base, str_from_cstr(path));
    HttpResponse *res = http_client_get(http_default_client, url, &err);
    if (res == NULL) {
        fmt_println_v(err);
        return;
    }
    Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    fmt_printf_v("%s %s (%s): %q\n", path, res->status,
                 http_header_get(res->header, S("Content-Type")),
                 str_from_bytes(body.p, body.len));
    http_response_free(res);
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Something to serve. */
    os_mkdir_all(S("public/css"), 0755);
    os_write_file(S("public/index.html"), BURROW_B("<h1>hello</h1>\n"), 0644);
    os_write_file(S("public/css/site.css"), BURROW_B("h1 { color: teal }\n"), 0644);

    Error err = NO_ERROR;
    NetListener l = net_listen(heap_allocator(), S("tcp"), S("127.0.0.1:0"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    NetAddr addr = l.vt->addr(l.data);
    Str base = fmt_sprintf_v(a, "http://%s", addr.vt->string(addr.data, a));

    HttpDir root = S("public");
    HttpServer srv = {.handler = http_file_server(a, http_dir_as_file_system(&root)),
                      .read_header_timeout = 5 * TIME_SECOND};
    Serving s = {&srv, l, NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, FN(Func, serve, &s));

    get(a, base, "/");
    get(a, base, "/css/site.css");
    get(a, base, "/missing.txt");
    http_client_close_idle_connections(http_default_client);

    /* Stop taking new connections, and give the open ones up to five seconds
     * to finish what they are doing. */
    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout(a, context_background(), 5 * TIME_SECOND, &cancel);
    err = http_server_shutdown(&srv, ctx);
    CALLF0(cancel);
    context_release(ctx);
    sync_wait_group_wait(&wg);
    fmt_println_v("shutdown:", err);
    fmt_println_v("serve:", s.err);

    http_server_free(&srv);
    net_listener_free(l);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
// doc: end

/* Output:
/ 200 OK (text/html; charset=utf-8): "<h1>hello</h1>\n"
/css/site.css 200 OK (text/css; charset=utf-8): "h1 { color: teal }\n"
/missing.txt 404 Not Found (text/plain; charset=utf-8): "404 page not found\n"
shutdown: <nil>
serve: http: Server closed
*/
