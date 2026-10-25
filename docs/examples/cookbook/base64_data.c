// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The standard alphabet uses + and /, which mean something in a URL. The
     * URL alphabet uses - and _ instead. */
    Byte raw[] = {0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
    Slice data = slice_from(raw, 6, 6, TYPE_BYTE);
    Str std = base64_encoding_encode_to_string(base64_std_encoding, a, data);
    Str url = base64_encoding_encode_to_string(base64_url_encoding, a, data);
    fmt_printf_v("standard: %s\nurl:      %s\n", std, url);

    /* Padding fills the last group out to four characters. The raw encodings
     * leave it off, which is what JWTs and most URLs want. */
    Str padded =
        base64_encoding_encode_to_string(base64_std_encoding, a, BURROW_B("hello"));
    Str bare =
        base64_encoding_encode_to_string(base64_raw_std_encoding, a, BURROW_B("hello"));
    fmt_printf_v("padded:   %s\nraw:      %s\n", padded, bare);

    /* Decoding gives the bytes back, and says where the input went wrong. */
    Error err = NO_ERROR;
    Slice back = base64_encoding_decode_string(base64_url_encoding, a, url, &err);
    Str h = hex_encode_to_string(a, back);
    fmt_printf_v("decoded:  %s\n", h);
    base64_encoding_decode_string(base64_std_encoding, a, S("aGVsbG8*"), &err);
    fmt_println_v("error:", err);
    arena_free(&ar);
}
// doc: end

/* Output:
standard: +vv8/f7/
url:      -vv8_f7_
padded:   aGVsbG8=
raw:      aGVsbG8
decoded:  fafbfcfdfeff
error: illegal base64 data at input byte 7
*/
