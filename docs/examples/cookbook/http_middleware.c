// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* The handler that does the real work. */
static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello, %s\n",
                  http_request_path_value(r, S("name")));
}

/* Middleware is a handler that wraps another one, the next handler, which it
 * gets through env. This one turns a request away unless it has the key. */
static void require_key(void *env, HttpResponseWriter w, HttpRequest *r) {
    HttpHandler *next = env;
    if (!str_eq(http_header_get(r->header, S("X-Api-Key")), S("s3cret"))) {
        http_error(w, S("missing or wrong API key"), HTTP_STATUS_UNAUTHORIZED);
        return; /* next never sees the request */
    }
    http_handler_serve_http(*next, w, r);
}

/* And this one prints each request on its way in. */
static void log_requests(void *env, HttpResponseWriter w, HttpRequest *r) {
    HttpHandler *next = env;
    fmt_printf_v("%s %s\n", r->method, r->url->path);
    http_handler_serve_http(*next, w, r);
}

static void try_request(Alloc *a, HttpHandler h, const char *path, const char *key) {
    HttpRequest *r =
        httptest_new_request(a, S("GET"), str_from_cstr(path), (IoReader){0});
    if (key != NULL)
        http_header_set(r->header, S("X-Api-Key"), str_from_cstr(key));
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec), r);
    HttpResponse *res = httptest_response_recorder_result(rec);
    Slice body = bytes_buffer_bytes(rec->body);
    fmt_printf_v("  %s: %q\n", res->status, str_from_bytes(body.p, body.len));
    httptest_response_recorder_free(rec);
    http_request_free(r);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    HttpServeMux *mux = http_new_serve_mux(a);
    http_serve_mux_handle_func(mux, S("GET /hello/{name}"),
                               FN(HttpHandlerFunc, hello, NULL));

    /* Each layer wraps the one before it, so a request goes through them from
     * the outside in: logging, then the key check, then the mux. */
    HttpHandler routes = http_serve_mux_as_handler(mux);
    HttpHandlerFunc auth_f = FN(HttpHandlerFunc, require_key, &routes);
    HttpHandler auth = http_handler_func_as_handler(&auth_f);
    HttpHandlerFunc log_f = FN(HttpHandlerFunc, log_requests, &auth);
    HttpHandler app = http_handler_func_as_handler(&log_f);

    try_request(a, app, "/hello/ada", "s3cret");
    try_request(a, app, "/hello/ada", NULL);
    try_request(a, app, "/goodbye", "s3cret");
    http_serve_mux_free(mux);
    arena_free(&ar);
}
// doc: end

/* Output:
GET /hello/ada
  200 OK: "hello, ada\n"
GET /hello/ada
  401 Unauthorized: "missing or wrong API key\n"
GET /goodbye
  404 Not Found: "404 page not found\n"
*/
