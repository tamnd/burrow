#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

static void header(Alloc *a) {
    // doc: header
    StringsReader *sr = strings_new_reader(
        a, BURROW_S("content-type: text/plain\r\nX-Tag: a\r\nx-tag: b\r\n\r\nbody"));
    TextprotoReader *r =
        textproto_new_reader(a, bufio_new_reader(a, strings_reader_as_io_reader(sr)));
    Error err;
    TextprotoMIMEHeader h = textproto_reader_read_mime_header(r, a, &err);
    printf("%.*s\n", P(textproto_mime_header_get(h, BURROW_S("Content-Type"))));
    Slice tags = textproto_mime_header_values(h, BURROW_S("x-tag"));
    printf("%d tags, first %.*s\n", (int)tags.len, P(((const Str *)tags.p)[0]));
    printf("%.*s\n",
           P(textproto_canonical_mime_header_key(a, BURROW_S("x-forwarded-for"))));
    // doc: end
}

static void codes(Alloc *a) {
    // doc: codes
    StringsReader *sr = strings_new_reader(
        a, BURROW_S("250-mail.example.com\r\n250-SIZE 35882577\r\n250 HELP\r\n"
                    "550 no such user\r\n"));
    TextprotoReader *r =
        textproto_new_reader(a, bufio_new_reader(a, strings_reader_as_io_reader(sr)));
    Error err;
    Str msg;
    Int code = textproto_reader_read_response(r, a, 250, &msg, &err);
    printf("%d %.*s\n", (int)code, P(msg));
    textproto_reader_read_code_line(r, a, 250, &msg, &err);
    fmt_printf_v("%v\n", err);
    // doc: end
}

static void dot(Alloc *a) {
    // doc: dot
    BytesBuffer out = BYTES_BUFFER(a);
    TextprotoWriter *w =
        textproto_new_writer(a, bufio_new_writer(a, bytes_buffer_as_io_writer(&out)));
    textproto_writer_printf_line_v(w, "DATA");
    IoWriteCloser d = textproto_writer_dot_writer(w);
    io_write_string(io_write_closer_as_io_writer(d), BURROW_S("line one\n.hidden\n"),
                    NULL);
    d.vt->closer.close(d.data);
    fmt_printf_v("%q\n", bytes_buffer_string(&out, a));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    header(a);
    codes(a);
    dot(a);
    arena_free(&ar);
    return 0;
}

/* Output:
text/plain
2 tags, first a
X-Forwarded-For
250 mail.example.com
SIZE 35882577
HELP
550 "no such user"
"DATA\r\nline one\r\n..hidden\r\n.\r\n"
*/
