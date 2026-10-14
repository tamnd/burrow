#include "burrow/burrow.h"

static void run(void *env) {
    Alloc *a = env;
    // doc: dump
    /* A request as the transport would send it, with the fields it adds. */
    StringsReader body;
    strings_reader_reset(&body, BURROW_S("name=gopher"));
    Error err;
    HttpRequest *req = http_new_request(a, BURROW_S("POST"),
                                        BURROW_S("http://example.com/signup?ref=docs"),
                                        strings_reader_as_io_reader(&body), &err);
    if (req == NULL)
        return;
    http_header_set(req->header, BURROW_S("Content-Type"),
                    BURROW_S("application/x-www-form-urlencoded"));
    Slice out = httputil_dump_request_out(a, req, true, &err);
    if (BURROW_OK(err))
        fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
    http_request_free(req);

    /* A response, with its body read into the dump and put back after. */
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: 5\r\n"
                                       "\r\n"
                                       "hello"));
    BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
    HttpResponse *res = http_read_response(a, br, NULL, &err);
    if (res != NULL) {
        out = httputil_dump_response(a, res, false, &err);
        fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
        out = httputil_dump_response(a, res, true, &err);
        fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
        Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("body after the dump: %s\n", str_from_bytes(b.p, b.len));
        http_response_free(res);
    }
    bufio_reader_free(br);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}
