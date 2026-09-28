#include "burrow/mime/quotedprintable.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

static void show(const char *label, Slice b) {
    Str s = {(const Byte *)b.p, b.len};
    fmt_printf_v("%s: %q\n", label, s);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: encode
    BytesBuffer out = BYTES_BUFFER(a);
    QuotedprintableWriter *w =
        quotedprintable_new_writer(a, bytes_buffer_as_io_writer(&out));
    io_write_string(
        quotedprintable_writer_as_io_writer(w),
        BURROW_S("Caf\xc3\xa9 au lait, 2 = 1 + 1 \n"
                 "This line is long enough that the encoder has to break it in "
                 "two, since a line may be 76 characters at most."),
        &err);
    err = quotedprintable_writer_close(w);
    show("encoded", bytes_buffer_bytes(&out));
    // doc: end

    // doc: decode
    StringsReader src;
    strings_reader_reset(&src,
                         BURROW_S("Caf=C3=A9 au lait, soft=\r\n break, tab=09.\r\n"));
    QuotedprintableReader *r =
        quotedprintable_new_reader(a, strings_reader_as_io_reader(&src));
    Slice text = io_read_all(a, quotedprintable_reader_as_io_reader(r), &err);
    show("decoded", text);
    quotedprintable_reader_free(r);
    // doc: end

    // doc: errors
    strings_reader_reset(&src, BURROW_S("fine=\r\nbad\x01"));
    r = quotedprintable_new_reader(a, strings_reader_as_io_reader(&src));
    text = io_read_all(a, quotedprintable_reader_as_io_reader(r), &err);
    show("before the error", text);
    fmt_printf_v("error: %v\n", err);
    quotedprintable_reader_free(r);
    // doc: end

    quotedprintable_writer_free(w);
    arena_free(&ar);
    return 0;
}

/* Output:
encoded: "Caf=C3=A9 au lait, 2 =3D 1 + 1=20\r\nThis line is long enough that the encoder has to break it in two, since a l=\r\nine may be 76 characters at most."
decoded: "Café au lait, soft break, tab\t.\r\n"
before the error: "finebad"
error: quotedprintable: invalid unescaped byte 0x01 in body
*/
