#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/base32.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: oneshot
    Str s = base32_encoding_encode_to_string(base32_std_encoding, a,
                                             BURROW_B("Hello, Gophers"));

    Error err = BURROW_NO_ERROR;
    Slice b = base32_encoding_decode_string(base32_hex_encoding, a,
                                            BURROW_S("91IMOR3F41BMUSJCCG======"), &err);
    // doc: end
    printf("s: " BURROW_STR_FMT "\n", BURROW_STR_ARG(s));
    printf("b: %.*s\n", (int)b.len, (const char *)b.p);

    // doc: raw
    Base32Encoding raw =
        base32_encoding_with_padding(base32_std_encoding, BASE32_NO_PADDING);
    Str r = base32_encoding_encode_to_string(&raw, a, BURROW_B("key"));
    Slice c = base32_encoding_decode_string(&raw, a, BURROW_S("NNSXS"), &err);
    // doc: end
    printf("raw: " BURROW_STR_FMT "\n", BURROW_STR_ARG(r));
    printf("c: %.*s\n", (int)c.len, (const char *)c.p);

    // doc: bad
    Slice part = base32_encoding_decode_string(base32_std_encoding, a,
                                               BURROW_S("NBSWY3DPEB3W64TMMQ1="), &err);
    const Base32CorruptInputError *off =
        errors_as(err, TYPE_BASE32_CORRUPT_INPUT_ERROR);
    // doc: end
    printf("part: %.*s\n", (int)part.len, (const char *)part.p);
    printf("off: %lld\n", (long long)*off);
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: stream
    StringsBuilder out = STRINGS_BUILDER(a);
    IoWriteCloser enc =
        base32_new_encoder(a, base32_std_encoding, strings_builder_as_io_writer(&out));
    fmt_fprintf_v(io_write_closer_as_io_writer(enc), "%d apples", 12);
    enc.vt->closer.close(enc.data);
    // doc: end
    printf("stream: " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(strings_builder_string(&out)));

    arena_free(&ar);
    return 0;
}

/* Output:
s: JBSWY3DPFQQEO33QNBSXE4Y=
b: Hello World
raw: NNSXS
c: key
part: hello worl
off: 18
err: illegal base32 data at input byte 18
stream: GEZCAYLQOBWGK4Y=
*/
