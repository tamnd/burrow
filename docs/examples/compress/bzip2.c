#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/compress/bzip2.h"
#include "burrow/mem/arena.h"

/* "hello world\n" as bzip2 -9 writes it. */
static Byte hello[] = {0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x4e,
                       0xec, 0xe8, 0x36, 0x00, 0x00, 0x02, 0x51, 0x80, 0x00, 0x10, 0x40,
                       0x00, 0x06, 0x44, 0x90, 0x80, 0x20, 0x00, 0x31, 0x06, 0x4c, 0x41,
                       0x01, 0xa7, 0xa9, 0xa5, 0x80, 0xbb, 0x94, 0x31, 0xf8, 0xbb, 0x92,
                       0x29, 0xc2, 0x84, 0x82, 0x77, 0x67, 0x41, 0xb0};

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: read
    BytesReader in;
    bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
    IoReader zr = bzip2_new_reader(a, bytes_reader_as_io_reader(&in));
    if (zr.vt == NULL)
        return 1;
    Slice text = io_read_all(a, zr, &err);
    bzip2_reader_free(zr);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)text.len, (const char *)text.p);

    // doc: errors
    hello[sizeof hello - 3] ^= 0x01;
    bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
    zr = bzip2_new_reader(a, bytes_reader_as_io_reader(&in));
    if (zr.vt == NULL)
        return 1;
    text = io_read_all(a, zr, &err);
    bzip2_reader_free(zr);
    const Bzip2StructuralError *se = errors_as(err, TYPE_BZIP2_STRUCTURAL_ERROR);
    if (se != NULL)
        printf("%d bytes, then %.*s\n", (int)text.len, (int)se->len,
               (const char *)se->p);
    // doc: end

    arena_free(&ar);
    return 0;
}
