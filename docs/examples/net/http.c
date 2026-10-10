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

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    header(a);
    sniff();
    times(a);
    cookies(a);
    arena_free(&ar);
    return 0;
}
