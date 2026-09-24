#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: oneshot
    Str s = hex_encode_to_string(a, BURROW_B("Hello"));

    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, BURROW_S("48656C6C6F"), &err);
    // doc: end
    printf("s: " BURROW_STR_FMT "\n", BURROW_STR_ARG(s));
    printf("b: %.*s\n", (int)b.len, (const char *)b.p);

    // doc: bad
    Slice part = hex_decode_string(a, BURROW_S("4865zz"), &err);
    const HexInvalidByteError *bad = errors_as(err, TYPE_HEX_INVALID_BYTE_ERROR);
    // doc: end
    printf("part: %.*s\n", (int)part.len, (const char *)part.p);
    printf("bad: %c\n", *bad);
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: dump
    Str d = hex_dump(a, BURROW_B("Go is an open source programming language."));
    // doc: end
    printf(BURROW_STR_FMT, BURROW_STR_ARG(d));

    // doc: stream
    StringsBuilder out = STRINGS_BUILDER(a);
    IoWriter enc = hex_new_encoder(a, strings_builder_as_io_writer(&out));
    fmt_fprintf_v(enc, "%d apples", 12);
    // doc: end
    printf("stream: " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(strings_builder_string(&out)));

    arena_free(&ar);
    return 0;
}

/* Output:
s: 48656c6c6f
b: Hello
part: He
bad: z
err: encoding/hex: invalid byte: U+007A 'z'
00000000  47 6f 20 69 73 20 61 6e  20 6f 70 65 6e 20 73 6f  |Go is an open so|
00000010  75 72 63 65 20 70 72 6f  67 72 61 6d 6d 69 6e 67  |urce programming|
00000020  20 6c 61 6e 67 75 61 67  65 2e                    | language.|
stream: 3132206170706c6573
*/
