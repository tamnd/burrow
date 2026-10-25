# Download a file over HTTP

Get a URL and save the body to a file, treating anything but a 200 as an error and setting a timeout on the whole exchange. The server is an `httptest` one, so the program needs no network. The timeout error answers true to `net_error_timeout`, which is how to tell it apart from the others.

<!-- example: ../examples/cookbook/http_download.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/net/http/httptest.h"

/* Gets url and writes the body to dst, giving back how many bytes it wrote.
 * Anything but a 200 is an error, and so is taking longer than the client's
 * timeout. */
static int64_t download(Alloc *a, HttpClient *c, Str url, Str dst, Error *err) {
    HttpResponse *res = http_client_get(c, url, err);
    if (res == NULL)
        return 0;
    int64_t n = 0;
    if (res->status_code != HTTP_STATUS_OK) {
        *err = fmt_errorf_v("%s: %s", res->request->url->path, res->status);
    } else {
        OsFile *f = os_create(a, dst, err);
        if (OK(*err)) {
            n = io_copy(a, os_file_as_io_writer(f),
                        io_read_closer_as_io_reader(res->body), err);
            Error cerr = os_file_close(f);
            if (OK(*err))
                *err = cerr;
        }
    }
    http_response_free(res);
    return n;
}

static void report(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Error err;
    io_write_string(http_response_writer_as_io_writer(w),
                    strings_repeat(arena_allocator(&r->arena), S("all quiet\n"), 300),
                    &err);
}

static void slow(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
    time_sleep(500 * TIME_MILLISECOND);
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A server to download from. */
    HttpServeMux *mux = http_new_serve_mux(heap_allocator());
    http_serve_mux_handle_func(mux, S("/report.txt"),
                               FN(HttpHandlerFunc, report, NULL));
    http_serve_mux_handle_func(mux, S("/slow"), FN(HttpHandlerFunc, slow, NULL));
    HttptestServer *ts =
        httptest_new_server(heap_allocator(), http_serve_mux_as_handler(mux));

    HttpClient c = *httptest_server_client(ts);
    c.timeout = 100 * TIME_MILLISECOND;

    const char *paths[] = {"/report.txt", "/missing.txt", "/slow"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        Error err = NO_ERROR;
        Str url = fmt_sprintf_v(a, "%s%s", ts->url, str_from_cstr(paths[i]));
        int64_t n = download(a, &c, url, S("download.txt"), &err);
        if (FAILED(err)) {
            bool timeout = net_error_timeout(err);
            fmt_printf_v("%s failed, timeout %t\n", str_from_cstr(paths[i]), timeout);
            if (!timeout)
                fmt_println_v(err);
            continue;
        }
        fmt_printf_v("%s: saved %d bytes\n", str_from_cstr(paths[i]), n);
    }

    httptest_server_free(ts);
    http_serve_mux_free(mux);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

It prints:

```text
/report.txt: saved 3000 bytes
/missing.txt failed, timeout false
/missing.txt: 404 Not Found
/slow failed, timeout true
```

The program is [docs/examples/cookbook/http_download.c](../examples/cookbook/http_download.c). CI builds and runs it and checks that it prints this.
