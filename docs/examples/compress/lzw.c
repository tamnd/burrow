#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/compress/lzw.h"
#include "burrow/mem/arena.h"

/* The TOBEORNOT line from Wikipedia's LZW article as Go's lzw.NewWriter writes
 * it the way GIF packs codes: least significant bits first, 8 bit literals. */
static Byte tobe[] = {0x00, 0xa9, 0x3c, 0x11, 0x52, 0xe4, 0x89, 0x14, 0x27, 0x4f, 0xa8,
                      0x08, 0x24, 0x68, 0x70, 0x61, 0xc1, 0x83, 0x09, 0x03, 0x02};

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: write
    BytesBuffer out = BYTES_BUFFER(a);
    LzwWriter *zw = lzw_new_writer(a, bytes_buffer_as_io_writer(&out), LZW_LSB, 8);
    if (zw == NULL)
        return 1;
    lzw_writer_write(
        zw, slice_from((char[]){"TOBEORNOTTOBEORTOBEORNOT"}, 24, 24, TYPE_BYTE), &err);
    err = lzw_writer_close(zw);
    lzw_writer_free(zw);
    printf("%d bytes\n", (int)bytes_buffer_len(&out));
    // doc: end
    if (BURROW_FAILED(err) || bytes_buffer_len(&out) != (Int)sizeof tobe ||
        memcmp(bytes_buffer_bytes(&out).p, tobe, sizeof tobe) != 0)
        return 1;

    // doc: read
    BytesReader in;
    bytes_reader_reset(&in, bytes_buffer_bytes(&out));
    LzwReader *zr = lzw_new_reader(a, bytes_reader_as_io_reader(&in), LZW_LSB, 8);
    if (zr == NULL)
        return 1;
    Slice text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s\n", (int)text.len, (const char *)text.p);

    // doc: errors
    bytes_reader_reset(&in, slice_from(tobe, 11, 11, TYPE_BYTE));
    lzw_reader_reset(zr, bytes_reader_as_io_reader(&in), LZW_LSB, 8);
    text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
    if (errors_is(err, io_err_unexpected_eof))
        printf("%.*s after %d bytes\n", (int)error_text(err).len,
               (const char *)error_text(err).p, (int)text.len);

    bytes_reader_reset(&in, bytes_buffer_bytes(&out));
    lzw_reader_reset(zr, bytes_reader_as_io_reader(&in), LZW_LSB, 9);
    text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
    lzw_reader_free(zr);
    // doc: end

    arena_free(&ar);
    return 0;
}
