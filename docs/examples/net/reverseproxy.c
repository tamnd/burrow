#include "burrow/burrow.h"

/* The server behind the proxy says what it got. */
static void backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "path %s, X-Forwarded-Proto %s\n",
                  r->url->path, http_header_get(r->header, BURROW_S("X-Forwarded-Proto")));
}

// doc: hooks
static void rewrite(void *env, HttputilProxyRequest *r) {
    const Url *target = env;
    httputil_proxy_request_set_url(r, target);
    httputil_proxy_request_set_x_forwarded(r);
    http_header_set(r->out->header, BURROW_S("X-Api-Key"), BURROW_S("secret"));
}

static Error modify_response(void *env, HttpResponse *res) {
    (void)env;
    http_header_set(res->header, BURROW_S("X-Proxied"), BURROW_S("yes"));
    return BURROW_NO_ERROR;
}
// doc: end

static void run(void *env) {
    Alloc *a = env;
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, backend, NULL);
    HttptestServer *back =
        httptest_new_server(heap_allocator(), http_handler_func_as_handler(&hf));
    Error err;
    // doc: proxy
    Url *target = url_parse(a, fmt_sprintf_v(a, "%s/api", back->url), &err);
    HttputilReverseProxy proxy = {
        .rewrite = BURROW_FN(HttputilRewriteFunc, rewrite, target),
        .modify_response = BURROW_FN(HttputilModifyResponseFunc, modify_response, NULL),
    };
    HttptestServer *front =
        httptest_new_server(heap_allocator(), httputil_reverse_proxy_as_handler(&proxy));

    HttpResponse *res = http_client_get(httptest_server_client(front),
                                        fmt_sprintf_v(a, "%s/users", front->url), &err);
    if (res != NULL) {
        Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("%s, X-Proxied %s\n", res->status,
                     http_header_get(res->header, BURROW_S("X-Proxied")));
        fmt_printf_v("%s", str_from_bytes(b.p, b.len));
        http_response_free(res);
    }
    // doc: end
    httptest_server_free(front);
    httptest_server_free(back);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}

/* Output:
200 OK, X-Proxied yes
path /api/users, X-Forwarded-Proto http
*/
