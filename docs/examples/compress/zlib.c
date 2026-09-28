#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/compress/zlib.h"
#include "burrow/mem/arena.h"

/* "hello, world\n" as Go's zlib.NewWriter writes it, from Go's ExampleNewWriter. */
static Byte hello[] = {120, 156, 0,   13,  0,   242, 255, 104, 101, 108, 108, 111, 44,
                       32,  119, 111, 114, 108, 100, 10,  3,   0,   33,  231, 4,   147};

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: write
    BytesBuffer out = BYTES_BUFFER(a);
    ZlibWriter *zw = zlib_new_writer(a, bytes_buffer_as_io_writer(&out));
    if (zw == NULL)
        return 1;
    zlib_writer_write(zw, slice_from((char[]){"hello, world\n"}, 13, 13, TYPE_BYTE),
                      &err);
    err = zlib_writer_close(zw);
    zlib_writer_free(zw);
    printf("%d bytes\n", (int)bytes_buffer_len(&out));
    // doc: end
    if (BURROW_FAILED(err) || bytes_buffer_len(&out) != (Int)sizeof hello ||
        memcmp(bytes_buffer_bytes(&out).p, hello, sizeof hello) != 0)
        return 1;

    // doc: read
    BytesReader in;
    bytes_reader_reset(&in, bytes_buffer_bytes(&out));
    IoReadCloser rc = zlib_new_reader(a, bytes_reader_as_io_reader(&in), &err);
    if (BURROW_FAILED(err))
        return 1;
    Slice text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    zlib_reader_free(rc);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)text.len, (const char *)text.p);

    // doc: dict
    Slice dict = slice_from((char[]){"hello, "}, 7, 7, TYPE_BYTE);
    bytes_buffer_reset(&out);
    zw = zlib_new_writer_level_dict(a, bytes_buffer_as_io_writer(&out),
                                    ZLIB_BEST_COMPRESSION, dict, &err);
    if (zw == NULL)
        return 1;
    zlib_writer_write(zw, slice_from((char[]){"hello, hello\n"}, 13, 13, TYPE_BYTE),
                      &err);
    err = zlib_writer_close(zw);
    zlib_writer_free(zw);

    bytes_reader_reset(&in, bytes_buffer_bytes(&out));
    rc = zlib_new_reader(a, bytes_reader_as_io_reader(&in), &err);
    if (errors_is(err, zlib_err_dictionary))
        printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);

    bytes_reader_reset(&in, bytes_buffer_bytes(&out));
    rc = zlib_new_reader_dict(a, bytes_reader_as_io_reader(&in), dict, &err);
    if (BURROW_FAILED(err))
        return 1;
    text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    zlib_reader_free(rc);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)text.len, (const char *)text.p);

    arena_free(&ar);
    return 0;
}
