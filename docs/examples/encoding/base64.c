#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/base64.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: oneshot
    Str s = base64_encoding_encode_to_string(base64_std_encoding, a,
                                             BURROW_B("any carnal pleas"));

    Error err = BURROW_NO_ERROR;
    Slice b = base64_encoding_decode_string(base64_url_encoding, a,
                                            BURROW_S("PDw_Pz8-Pg=="), &err);
    // doc: end
    printf("s: " BURROW_STR_FMT "\n", BURROW_STR_ARG(s));
    printf("b: %.*s\n", (int)b.len, (const char *)b.p);

    // doc: raw
    Base64Encoding raw =
        base64_encoding_with_padding(base64_url_encoding, BASE64_NO_PADDING);
    Slice c = base64_encoding_decode_string(&raw, a, BURROW_S("PDw_Pz8-Pg"), &err);
    // doc: end
    printf("c: %.*s\n", (int)c.len, (const char *)c.p);

    // doc: bad
    Slice part = base64_encoding_decode_string(base64_std_encoding, a,
                                               BURROW_S("aGVsbG8*"), &err);
    const Base64CorruptInputError *off =
        errors_as(err, TYPE_BASE64_CORRUPT_INPUT_ERROR);
    // doc: end
    printf("part: %.*s\n", (int)part.len, (const char *)part.p);
    printf("off: %lld\n", (long long)*off);
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: stream
    StringsBuilder out = STRINGS_BUILDER(a);
    IoWriteCloser enc =
        base64_new_encoder(a, base64_std_encoding, strings_builder_as_io_writer(&out));
    fmt_fprintf_v(io_write_closer_as_io_writer(enc), "%d apples", 12);
    enc.vt->closer.close(enc.data);
    // doc: end
    printf("stream: " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(strings_builder_string(&out)));

    arena_free(&ar);
    return 0;
}

/* Output:
s: YW55IGNhcm5hbCBwbGVhcw==
b: <<???>>
c: <<???>>
part: hel
off: 7
err: illegal base64 data at input byte 7
stream: MTIgYXBwbGVz
*/
