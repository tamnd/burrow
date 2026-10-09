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

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    header(a);
    sniff();
    times(a);
    arena_free(&ar);
    return 0;
}
