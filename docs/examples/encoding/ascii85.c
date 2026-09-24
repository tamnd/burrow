#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/ascii85.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: encode
    Slice src = BURROW_B("Hello, world");
    Int max = ascii85_max_encoded_len(src.len);
    Slice dst = slice_make(a, TYPE_BYTE, max, max);
    Int n = ascii85_encode(dst, src);
    // doc: end
    printf("max: %lld\n", (long long)max);
    printf("enc: %.*s\n", (int)n, (const char *)dst.p);

    // doc: decode
    Slice in = BURROW_B("87cURD]i,\"Ebo80");
    Slice out = slice_make(a, TYPE_BYTE, 4 * in.len, 4 * in.len);
    Int nsrc;
    Error err = BURROW_NO_ERROR;
    Int ndst = ascii85_decode(out, in, true, &nsrc, &err);
    // doc: end
    printf("dec: %.*s\n", (int)ndst, (const char *)out.p);
    printf("ndst: %lld, nsrc: %lld\n", (long long)ndst, (long long)nsrc);

    // doc: bad
    ascii85_decode(out, BURROW_B("87cUR~>"), true, NULL, &err);
    const Ascii85CorruptInputError *off =
        errors_as(err, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
    // doc: end
    printf("off: %lld\n", (long long)*off);
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: stream
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("87cURD]i,\n\"Ebo80"));
    IoReader dec = ascii85_new_decoder(a, strings_reader_as_io_reader(&sr));
    BytesBuffer got = BYTES_BUFFER(a);
    io_copy(a, bytes_buffer_as_io_writer(&got), dec, &err);
    // doc: end
    Slice g = bytes_buffer_bytes(&got);
    printf("stream: %.*s\n", (int)g.len, (const char *)g.p);

    arena_free(&ar);
    return 0;
}

/* Output:
max: 15
enc: 87cURD_*#TDfTZ)
dec: Hello World!
ndst: 12, nsrc: 15
off: 5
err: illegal ascii85 data at input byte 5
stream: Hello World!
*/
