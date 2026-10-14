#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static void greet(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello, %s\n",
                  http_request_path_value(r, BURROW_S("name")));
}

typedef struct Serving {
    HttpServer *srv;
    NetListener l;
    Error err;
} Serving;

static void serve(void *env) {
    Serving *s = env;
    s->err = http_server_serve(s->srv, s->l);
}

/* A client in a few lines: sends req on a new connection to port and prints
 * the response, all but the Date line, which changes. */
static void ask(Alloc *a, Int port, const char *req) {
    Byte loopback[4] = {127, 0, 0, 1};
    NetTCPAddr raddr = {slice_from(loopback, 4, 4, TYPE_BYTE), port, BURROW_STR_EMPTY};
    Error err;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &raddr, &err);
    if (c == NULL)
        return;
    NetConn conn = net_tcp_conn_as_conn(c);
    io_write_string(net_conn_as_io_writer(conn), str_from_cstr(req), &err);
    Slice b = io_read_all(a, net_conn_as_io_reader(conn), &err);
    net_tcp_conn_free(c);

    const char *p = b.p;
    const char *end = p + b.len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl == NULL ? (size_t)(end - p) : (size_t)(nl - p);
        size_t shown = n > 0 && p[n - 1] == '\r' ? n - 1 : n;
        if (shown < 6 || memcmp(p, "Date: ", 6) != 0)
            printf("%.*s\n", (int)shown, p);
        p += nl == NULL ? n : n + 1;
    }
}

static void run(void *env) {
    Alloc *a = env;
    // doc: server
    Byte loopback[4] = {127, 0, 0, 1};
    NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
    if (l == NULL)
        return;
    Int port = ((const NetTCPAddr *)net_tcp_listener_addr(l).data)->port;

    HttpServeMux *mux = http_new_serve_mux(heap_allocator());
    http_serve_mux_handle_func(mux, BURROW_S("GET /hello/{name}"),
                               BURROW_FN(HttpHandlerFunc, greet, NULL));
    HttpServer srv = {.handler = http_serve_mux_as_handler(mux),
                      .read_header_timeout = 5 * TIME_SECOND};
    Serving s = {&srv, net_tcp_listener_as_listener(l), BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, &s));

    ask(a, port,
        "GET /hello/gopher HTTP/1.1\r\nHost: example\r\nConnection: close\r\n\r\n");
    ask(a, port,
        "DELETE /hello/gopher HTTP/1.1\r\nHost: example\r\nConnection: close\r\n\r\n");

    /* Shutdown stops taking connections and waits for the open ones to go
     * idle, for five seconds at most. */
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       5 * TIME_SECOND, &cancel);
    err = http_server_shutdown(&srv, ctx);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    sync_wait_group_wait(&wg);
    fmt_printf_v("shutdown: %v\nserve: %v\n", err, s.err);
    http_server_free(&srv);
    http_serve_mux_free(mux);
    net_tcp_listener_free(l);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}
