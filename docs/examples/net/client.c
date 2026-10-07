#include "burrow/burrow.h"

static void old_page(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    http_redirect(w, r, BURROW_S("/new"), HTTP_STATUS_FOUND);
}

static void new_page(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    Error err;
    io_write_string(http_response_writer_as_io_writer(w),
                    BURROW_S("you found the new page\n"), &err);
}

static void echo(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Error err;
    Slice b = io_read_all(heap_allocator(), io_read_closer_as_io_reader(r->body), &err);
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "%s %s\n",
                  http_header_get(r->header, BURROW_S("Content-Type")),
                  str_from_bytes(b.p, b.len));
    mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
}

static void slow(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
    time_sleep(500 * TIME_MILLISECOND);
}

static Error stop_here(void *env, HttpRequest *req, Slice via) {
    (void)env;
    (void)req;
    (void)via;
    return http_err_use_last_response;
}

/* Prints the status, the path the response came from and the body, and frees
 * the response. */
static void show(Alloc *a, HttpResponse *res, Error err) {
    if (res == NULL) {
        fmt_printf_v("error: %v\n", err);
        return;
    }
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    fmt_printf_v("%s from %s: %s", res->status, res->request->url->path,
                 str_from_bytes(b.p, b.len));
    http_response_free(res);
}

static void run(void *env) {
    Alloc *a = env;
    HttpServeMux *mux = http_new_serve_mux(heap_allocator());
    http_serve_mux_handle_func(mux, BURROW_S("/old"),
                               BURROW_FN(HttpHandlerFunc, old_page, NULL));
    http_serve_mux_handle_func(mux, BURROW_S("/new"),
                               BURROW_FN(HttpHandlerFunc, new_page, NULL));
    http_serve_mux_handle_func(mux, BURROW_S("POST /echo"),
                               BURROW_FN(HttpHandlerFunc, echo, NULL));
    http_serve_mux_handle_func(mux, BURROW_S("/slow"),
                               BURROW_FN(HttpHandlerFunc, slow, NULL));
    // doc: client
    HttptestServer *ts =
        httptest_new_server(heap_allocator(), http_serve_mux_as_handler(mux));
    HttpClient *c = httptest_server_client(ts);
    Error err;

    /* Get follows the redirect. */
    Str url = fmt_sprintf_v(a, "%s/old", ts->url);
    show(a, http_client_get(c, url, &err), err);

    /* PostForm encodes the values as the body. */
    UrlValues form = url_values_make(a);
    url_values_set(form, BURROW_S("name"), BURROW_S("gopher"));
    url_values_add(form, BURROW_S("name"), BURROW_S("burrow"));
    show(a, http_client_post_form(c, fmt_sprintf_v(a, "%s/echo", ts->url), form, &err),
         err);

    /* check_redirect can stop at the redirect and hand it back. */
    HttpClient stopping = *c;
    stopping.check_redirect = BURROW_FN(HttpCheckRedirectFunc, stop_here, NULL);
    HttpResponse *res = http_client_get(&stopping, url, &err);
    if (res != NULL)
        fmt_printf_v("Location: %s\n",
                     http_header_get(res->header, BURROW_S("Location")));
    show(a, res, err);

    /* timeout covers the whole exchange, body and all. */
    HttpClient hurried = *c;
    hurried.timeout = 100 * TIME_MILLISECOND;
    res = http_client_get(&hurried, fmt_sprintf_v(a, "%s/slow", ts->url), &err);
    fmt_printf_v("deadline exceeded: %t, timeout: %t\n",
                 errors_is(err, context_deadline_exceeded),
                 res == NULL && net_error_timeout(err));
    http_response_free(res);
    httptest_server_free(ts);
    // doc: end
    http_serve_mux_free(mux);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}

/* Output:
200 OK from /new: you found the new page
200 OK from /echo: application/x-www-form-urlencoded name=gopher&name=burrow
Location: /new
302 Found from /old: <a href="/new">Found</a>.

deadline exceeded: true, timeout: true
*/
