#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/compress/flate.h"
#include "burrow/mem/arena.h"

/* "hello, hello, hello, hello\n" as Go's flate.NewWriter at level 9 writes it. */
static Byte hello[] = {0xca, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51,
                       0xc0, 0x42, 0x71, 0x01, 0x06, 0x00};

/* "hello, hello\n" compressed with the dictionary "hello, ". */
static Byte with_dict[] = {0x42, 0xa6, 0xb8, 0x00, 0x03, 0x00};

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: read
    BytesReader in;
    bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
    IoReadCloser rc = flate_new_reader(a, bytes_reader_as_io_reader(&in));
    Slice text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)text.len, (const char *)text.p);

    // doc: reset
    Slice dict = slice_from((char[]){"hello, "}, 7, 7, TYPE_BYTE);
    bytes_reader_reset(
        &in, slice_from(with_dict, sizeof with_dict, sizeof with_dict, TYPE_BYTE));
    flate_resetter_reset(flate_reader_as_resetter(rc), bytes_reader_as_io_reader(&in),
                         dict);
    text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)text.len, (const char *)text.p);

    // doc: corrupt
    static Byte bad[] = {0x07};
    bytes_reader_reset(&in, slice_from(bad, 1, 1, TYPE_BYTE));
    flate_resetter_reset(flate_reader_as_resetter(rc), bytes_reader_as_io_reader(&in),
                         (Slice){0});
    io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    const FlateCorruptInputError *off = errors_as(err, TYPE_FLATE_CORRUPT_INPUT_ERROR);
    if (off != NULL)
        printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
    // doc: end

    flate_reader_free(rc);
    arena_free(&ar);
    return off != NULL ? 0 : 1;
}
