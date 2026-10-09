#include "burrow/burrow.h"

/* The handler. REMOTE_USER is nowhere in the request itself, so it comes from
 * fcgi_process_env. The Date is fixed so that the output is the same every
 * run; without one, the response gets the time it was written. */
static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Str user = BURROW_S("nobody"), key = BURROW_S("REMOTE_USER");
    Map *vars = fcgi_process_env(r);
    const Str *v = vars != NULL ? map_get(vars, &key) : NULL;
    if (v != NULL)
        user = *v;
    http_header_set(http_response_writer_header(w), BURROW_S("Date"),
                    BURROW_S("Thu, 08 Oct 2026 10:00:00 GMT"));
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello %s, you asked for %s\n",
                  user, r->url->path);
}

static void serve(void *env) {
    NetListener *l = env;
    static HttpHandlerFunc f;
    f = BURROW_FN(HttpHandlerFunc, hello, NULL);
    (void)fcgi_serve(*l, http_handler_func_as_handler(&f));
}

/* One record, with no padding, which a FastCGI reader takes. */
static void record(BytesBuffer *b, int type, Slice content) {
    Byte h[8] = {1, (Byte)type, 0, 1, (Byte)(content.len >> 8), (Byte)content.len,
                 0, 0};
    bytes_buffer_write(b, (Slice){h, 8, 8, TYPE_BYTE}, NULL);
    bytes_buffer_write(b, content, NULL);
}

/* A name-value pair, for names and values shorter than 128 bytes. */
static void param(BytesBuffer *b, const char *name, const char *value) {
    Str n = str_from_cstr(name), v = str_from_cstr(value);
    (void)bytes_buffer_write_byte(b, (Byte)n.len);
    (void)bytes_buffer_write_byte(b, (Byte)v.len);
    bytes_buffer_write_string(b, n, NULL);
    bytes_buffer_write_string(b, v, NULL);
}

static void run(void *env) {
    (void)env;
    Alloc *heap = heap_allocator();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    // doc: serve
    /* The program's side: serve FastCGI on a TCP port. */
    NetListener l = net_listen(heap, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (l.vt == NULL)
        return;
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, &l));

    /* The web server's side, by hand: begin a request, send its variables,
     * then an empty body. */
    NetAddr la = l.vt->addr(l.data);
    NetConn c = net_dial(heap, BURROW_S("tcp"), la.vt->string(la.data, a), &err);
    if (c.vt != NULL) {
        BytesBuffer *params = bytes_new_buffer(a, (Slice){0});
        param(params, "REQUEST_METHOD", "GET");
        param(params, "SERVER_PROTOCOL", "HTTP/1.1");
        param(params, "REQUEST_URI", "/hello");
        param(params, "REMOTE_USER", "jane");
        BytesBuffer *out = bytes_new_buffer(a, (Slice){0});
        Byte begin[8] = {0, 1}; /* the responder role */
        record(out, 1, (Slice){begin, 8, 8, TYPE_BYTE});
        record(out, 4, bytes_buffer_bytes(params));
        record(out, 4, (Slice){0});
        record(out, 5, (Slice){0});
        bytes_buffer_write_to(out, net_conn_as_io_writer(c), &err);

        /* The response comes back as stdout records, up to an end-request
         * record. */
        BytesBuffer *stdout_ = bytes_new_buffer(a, (Slice){0});
        Byte h[8];
        Byte *body = mem_alloc(a, 65535 + 255, 1);
        IoReader r = net_conn_as_io_reader(c);
        while (io_read_full(r, (Slice){h, 8, 8, TYPE_BYTE}, &err) == 8 && h[1] != 3) {
            Int n = (Int)h[4] << 8 | h[5];
            io_read_full(r, (Slice){body, n + h[6], n + h[6], TYPE_BYTE}, &err);
            if (h[1] == 6)
                bytes_buffer_write(stdout_, (Slice){body, n, n, TYPE_BYTE}, NULL);
        }
        Str got = bytes_buffer_string(stdout_, a);
        fmt_printf_v("%s",
                     strings_replace_all(a, got, BURROW_S("\r\n"), BURROW_S("\n")));
        net_conn_free(c);
    }
    (void)l.vt->closer.close(l.data);
    sync_wait_group_wait(&wg);
    net_listener_free(l);
    // doc: end
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
PENDING
*/
