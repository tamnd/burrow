#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

static void header(Alloc *a) {
    // doc: header
    HttpHeader h = http_header_make(a);
    http_header_add(h, BURROW_S("accept-encoding"), BURROW_S("gzip"));
    http_header_add(h, BURROW_S("Accept-Encoding"), BURROW_S("br"));
    http_header_set(h, BURROW_S("content-type"), BURROW_S("text/html"));
    http_header_set(h, BURROW_S("X-Note"), BURROW_S("one\r\nInjected: two"));
    printf("%.*s\n", P(http_header_get(h, BURROW_S("CONTENT-TYPE"))));

    BytesBuffer out = BYTES_BUFFER(a);
    Error err = http_header_write(h, bytes_buffer_as_io_writer(&out));
    if (BURROW_OK(err))
        fmt_printf_v("%q\n", bytes_buffer_string(&out, a));
    // doc: end
}

static void sniff(void) {
    // doc: sniff
    Byte png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    char html[] = "  <!DOCTYPE html><title>hi</title>";
    Str ct =
        http_detect_content_type(slice_from(png, sizeof png, sizeof png, TYPE_BYTE));
    printf("%.*s\n", P(ct));
    Int n = (Int)strlen(html);
    ct = http_detect_content_type(slice_from(html, n, n, TYPE_BYTE));
    printf("%.*s\n", P(ct));
    printf("%d %.*s\n", HTTP_STATUS_TEAPOT, P(http_status_text(HTTP_STATUS_TEAPOT)));
    // doc: end
}

static void times(Alloc *a) {
    // doc: time
    Error err;
    Time t = http_parse_time(a, BURROW_S("Sunday, 06-Nov-94 08:49:37 GMT"), &err);
    if (BURROW_OK(err))
        printf("%.*s\n", P(time_format(t, a, HTTP_TIME_FORMAT)));
    http_parse_time(a, BURROW_S("yesterday"), &err);
    printf("%s\n", BURROW_FAILED(err) ? "not a date" : "a date");
    // doc: end
}

static void cookies(Alloc *a) {
    // doc: cookie
    Error err;
    Str line =
        BURROW_S("session=38afes7a8; Path=/; Max-Age=3600; HttpOnly; Flavour=mint");
    HttpCookie c = http_parse_set_cookie(a, line, &err);
    if (BURROW_OK(err)) {
        printf("%.*s=%.*s, path %.*s, max-age %d\n", P(c.name), P(c.value), P(c.path),
               (int)c.max_age);
        printf("unparsed: %.*s\n", P(BURROW_AT(Str, c.unparsed, 0)));
    }

    Slice sent = http_parse_cookie(a, BURROW_S("lang=en; theme=\"dark\""), &err);
    for (Int i = 0; i < sent.len; i++) {
        HttpCookie k = BURROW_AT(HttpCookie, sent, i);
        printf("%.*s is %.*s%s\n", P(k.name), P(k.value), k.quoted ? ", quoted" : "");
    }

    HttpCookie out = {
        .name = BURROW_S_INIT("cart"),
        .value = BURROW_S_INIT("3 items"),
        .path = BURROW_S_INIT("/shop"),
        .expires = time_date(2030, TIME_MARCH, 1, 12, 0, 0, 0, time_utc_loc),
        .secure = true,
        .same_site = HTTP_SAME_SITE_STRICT_MODE,
    };
    printf("%.*s\n", P(http_cookie_string(a, &out)));
    // doc: end
}

static void wire(Alloc *a) {
    // doc: wire
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("POST /upload?name=notes HTTP/1.1\r\n"
                                       "Host: example.com\r\n"
                                       "Authorization: Basic YWxpY2U6czNjcmV0\r\n"
                                       "Content-Length: 11\r\n"
                                       "\r\n"
                                       "hello world"));
    BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
    Error err;
    HttpRequest *req = http_read_request(a, br, &err);
    if (BURROW_OK(err)) {
        printf("%.*s %.*s for %.*s, %lld bytes\n", P(req->method), P(req->url->path),
               P(req->host), (long long)req->content_length);
        Str user;
        Str pass;
        if (http_request_basic_auth(req, a, &user, &pass))
            printf("from %.*s\n", P(user));
        Slice body = io_read_all(a, io_read_closer_as_io_reader(req->body), &err);
        printf("body: %.*s\n", (int)body.len, (const char *)body.p);
        http_request_free(req);
    }
    bufio_reader_free(br);

    strings_reader_reset(&sr, BURROW_S("HTTP/1.1 404 Not Found\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Transfer-Encoding: chunked\r\n"
                                       "\r\n"
                                       "4\r\nnone\r\n5\r\n here\r\n0\r\n\r\n"));
    br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
    HttpResponse *resp = http_read_response(a, br, NULL, &err);
    if (BURROW_OK(err)) {
        printf("%d, %.*s\n", (int)resp->status_code, P(resp->status));
        Slice body = io_read_all(a, io_read_closer_as_io_reader(resp->body), &err);
        printf("body: %.*s\n", (int)body.len, (const char *)body.p);
        http_response_free(resp);
    }
    bufio_reader_free(br);

    strings_reader_reset(&sr, BURROW_S("HTTP/1.1 OK\r\n\r\n"));
    br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
    http_read_response(a, br, NULL, &err);
    printf("%.*s\n", P(error_text(err)));
    bufio_reader_free(br);
    // doc: end
}

/* A ResponseWriter that prints the status a handler sends, with Location and
 * Allow when it sets them, and the body of a 200, so the example needs no
 * server and no client. */
typedef struct Printer {
    HttpHeader header;
    Int code;
} Printer;

static void printer_write_header(void *self, Int code) {
    Printer *p = self;
    if (p->code != 0)
        return;
    p->code = code;
    printf("%d", (int)code);
    Str loc = http_header_get(p->header, BURROW_S("Location"));
    Str allow = http_header_get(p->header, BURROW_S("Allow"));
    if (loc.len > 0)
        printf(" to %.*s", P(loc));
    if (allow.len > 0)
        printf(", allow %.*s", P(allow));
    printf("\n");
}

static Int printer_write(void *self, Slice b, Error *err) {
    Printer *p = self;
    printer_write_header(self, 200);
    if (p->code == 200)
        printf("    %.*s", (int)b.len, (const char *)b.p);
    *err = BURROW_NO_ERROR;
    return b.len;
}

static HttpHeader printer_header(void *self) {
    return ((Printer *)self)->header;
}

static const HttpResponseWriterVT printer_vt = {
    {NULL, printer_write}, printer_header, printer_write_header};

static void show_note(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "note %s\n",
                  http_request_path_value(r, BURROW_S("id")));
}

static void show_file(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "file %s\n", r->url->path);
}

static void route(Alloc *a) {
    // doc: mux
    HttpServeMux *mux = http_new_serve_mux(a);
    http_serve_mux_handle_func(mux, BURROW_S("GET /notes/{id}"),
                               BURROW_FN(HttpHandlerFunc, show_note, NULL));
    http_serve_mux_handle_func(mux, BURROW_S("/files/"),
                               BURROW_FN(HttpHandlerFunc, show_file, NULL));

    const char *reqs[][2] = {
        {"GET", "/notes/42"}, {"DELETE", "/notes/42"},      {"GET", "/files/a.txt"},
        {"GET", "/files"},    {"GET", "/files/x/../b.txt"}, {"GET", "/other"},
    };
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        Error err;
        HttpRequest *r =
            http_new_request(a, str_from_cstr(reqs[i][0]), str_from_cstr(reqs[i][1]),
                             (IoReader){0}, &err);
        if (r == NULL)
            continue;
        printf("%s %s: ", reqs[i][0], reqs[i][1]);
        Printer out = {http_header_make(a), 0};
        http_serve_mux_serve_http(mux, (HttpResponseWriter){&printer_vt, &out}, r);
        http_request_free(r);
    }
    http_serve_mux_free(mux);
    // doc: end
}

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    http_header_set(http_response_writer_header(w), BURROW_S("Cache-Control"),
                    BURROW_S("no-store"));
    if (!str_eq(r->method, BURROW_S("GET"))) {
        http_error(w, BURROW_S("only GET here"), HTTP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "<p>hello from %s</p>",
                  r->url->path);
}

static void recorder(Alloc *a) {
    // doc: httptest
    HttpHandlerFunc h = BURROW_FN(HttpHandlerFunc, hello, NULL);
    const char *methods[] = {"GET", "POST"};
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        HttpRequest *r = httptest_new_request(a, str_from_cstr(methods[i]),
                                              BURROW_S("/greet"), (IoReader){0});
        HttptestResponseRecorder *rec = httptest_new_recorder(a);
        http_handler_func_serve_http(
            h, httptest_response_recorder_as_response_writer(rec), r);

        HttpResponse *res = httptest_response_recorder_result(rec);
        Error err;
        Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("%s %s\n", r->method, res->status);
        fmt_printf_v("  Content-Type: %s\n",
                     http_header_get(res->header, BURROW_S("Content-Type")));
        fmt_printf_v("  Cache-Control: %s\n",
                     http_header_get(res->header, BURROW_S("Cache-Control")));
        Str text = {(const Byte *)body.p, body.len};
        fmt_printf_v("  body: %q\n", text);
        httptest_response_recorder_free(rec);
        http_request_free(r);
    }
    // doc: end
}

static void files(Alloc *a) {
    // doc: files
    FstestMapFS site = fstest_map_fs_make(a);
    FstestMapFile page = {
        .data = slice_from_str(a, BURROW_S("<h1>burrow</h1>\n")),
        .mod_time = time_date(2026, TIME_JANUARY, 2, 15, 4, 5, 0, time_utc_loc),
    };
    FstestMapFile notes = {.data = slice_from_str(a, BURROW_S("0123456789\n"))};
    fstest_map_fs_set(site, BURROW_S("index.html"), &page);
    fstest_map_fs_set(site, BURROW_S("notes.txt"), &notes);
    HttpHandler h = http_file_server_fs(a, fstest_map_fs_as_fs(site));

    const struct {
        const char *target, *key, *value;
    } reqs[] = {
        {"/", NULL, NULL},
        {"/", "If-Modified-Since", "Fri, 02 Jan 2026 15:04:05 GMT"},
        {"/notes.txt", "Range", "bytes=2-5"},
        {"/index.html", NULL, NULL},
        {"/missing.txt", NULL, NULL},
    };
    const char *show[] = {"Content-Type", "Content-Range", "Last-Modified", "Location"};
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        HttpRequest *r = httptest_new_request(
            a, BURROW_S("GET"), str_from_cstr(reqs[i].target), (IoReader){0});
        fmt_printf_v("GET %s", r->url->path);
        if (reqs[i].key != NULL) {
            Str key = str_from_cstr(reqs[i].key), value = str_from_cstr(reqs[i].value);
            http_header_set(r->header, key, value);
            fmt_printf_v(" with %s: %s", key, value);
        }
        HttptestResponseRecorder *rec = httptest_new_recorder(a);
        http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec),
                                r);

        HttpResponse *res = httptest_response_recorder_result(rec);
        fmt_printf_v("\n  %s\n", res->status);
        for (size_t j = 0; j < sizeof show / sizeof show[0]; j++) {
            Str v = http_header_get(res->header, str_from_cstr(show[j]));
            if (v.len > 0)
                fmt_printf_v("  %s: %s\n", str_from_cstr(show[j]), v);
        }
        Slice body = bytes_buffer_bytes(rec->body);
        if (body.len > 0)
            fmt_printf_v("  body: %q\n", str_from_bytes(body.p, body.len));
        httptest_response_recorder_free(rec);
        http_request_free(r);
    }
    // doc: end
}

static void show_cookies(CookiejarJar *jar, Alloc *a, const char *target) {
    Error err;
    Url *u = url_parse(a, str_from_cstr(target), &err);
    Slice got = cookiejar_jar_cookies(jar, a, u);
    printf("%s:", target);
    for (Int i = 0; i < got.len; i++) {
        HttpCookie c = BURROW_AT(HttpCookie, got, i);
        printf(" %.*s=%.*s", P(c.name), P(c.value));
    }
    printf("\n");
}

static void jar(Alloc *a) {
    // doc: cookiejar
    Error err;
    CookiejarJar *jar = cookiejar_new(a, NULL, &err);
    Url *from = url_parse(a, BURROW_S("http://www.example.com/shop/"), &err);
    const char *lines[] = {
        "session=1; Path=/",       "lang=en; Domain=example.com",
        "cart=3; Max-Age=3600",    "track=x; Domain=other.com",
        "admin=1; Path=/; Secure",
    };
    Slice set = slice_make(a, TYPE_HTTP_COOKIE, 0, 5);
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        HttpCookie c = http_parse_set_cookie(a, str_from_cstr(lines[i]), &err);
        set = slice_append(a, set, &c, 1);
    }
    cookiejar_jar_set_cookies(jar, from, set);

    show_cookies(jar, a, "http://www.example.com/shop/basket");
    show_cookies(jar, a, "https://www.example.com/");
    show_cookies(jar, a, "https://api.example.com/");
    show_cookies(jar, a, "http://other.com/");

    HttpCookie gone =
        http_parse_set_cookie(a, BURROW_S("session=; Path=/; Max-Age=0"), &err);
    cookiejar_jar_set_cookies(jar, from, slice_from(&gone, 1, 1, TYPE_HTTP_COOKIE));
    show_cookies(jar, a, "https://www.example.com/shop/basket");
    cookiejar_jar_free(jar);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    header(a);
    sniff();
    times(a);
    cookies(a);
    wire(a);
    route(a);
    recorder(a);
    files(a);
    jar(a);
    arena_free(&ar);
    return 0;
}
