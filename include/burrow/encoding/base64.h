/* encoding/base64, the RFC 4648 encoding of bytes as text.
 *
 * Three bytes become four characters from a 64 character alphabet, and the
 * end is padded with '=' to a multiple of four. The four encodings everyone
 * uses are here already:
 *
 *     Str s = base64_encoding_encode_to_string(base64_std_encoding, a, data);
 *     Slice b = base64_encoding_decode_string(base64_std_encoding, a, s, &err);
 *
 * base64_url_encoding swaps + and / for - and _ so the text can sit in a URL
 * or a file name, and the raw forms leave the padding off. Decoding skips
 * '\r' and '\n' wherever they are, as MIME wraps its lines.
 *
 * Any other alphabet or padding is a Base64Encoding you make, which is a plain
 * value with no pointers in it, so it can live on the stack, in a struct or in
 * a static, and needs no freeing.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/base64 */

#ifndef BURROW_ENCODING_BASE64_H
#define BURROW_ENCODING_BASE64_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------- encodings */

/* The padding character of the standard encodings, and the value that means
 * no padding at all. */
#define BASE64_STD_PADDING ((Rune)'=')
#define BASE64_NO_PADDING ((Rune) - 1)

/* An alphabet, a padding character and whether decoding is strict. The fields
 * are the package's, as Go's are unexported, and a Base64Encoding is only
 * ever made by the functions below. */
typedef struct Base64Encoding {
    Byte encode[64];      /* the character for each 6 bit value */
    Byte decode_map[256]; /* the value for each character, or 0xff */
    Rune pad_char;        /* BASE64_NO_PADDING or a byte */
    bool strict;          /* the unused bits at the end must be zero */
} Base64Encoding;

/* The standard encoding with '=' padding, the URL and file name safe one with
 * the same padding, and both again without it. */
extern const Base64Encoding *const base64_std_encoding;
extern const Base64Encoding *const base64_url_encoding;
extern const Base64Encoding *const base64_raw_std_encoding;
extern const Base64Encoding *const base64_raw_url_encoding;

/* A padded encoding with the given alphabet, which has to be 64 bytes with
 * none repeated and neither '\r' nor '\n' among them. Anything else panics
 * with Go's message. The bytes are taken as bytes, with no UTF-8 meaning.
 *
 * Go returns a new *Encoding. This returns the value, which is the same thing
 * without an allocation, and the caller passes its address to the rest. */
Base64Encoding base64_new_encoding(Str encoder);

/* enc with a different padding character, or with none for
 * BASE64_NO_PADDING. The character has to be a byte value, not '\r' or '\n',
 * and not in the alphabet, or this panics. One above 0x7f is written as that
 * byte and not as UTF-8. */
Base64Encoding base64_encoding_with_padding(const Base64Encoding *enc, Rune padding);

/* enc with strict decoding, which rejects input whose unused bits at the end
 * are not zero, as RFC 4648 section 3.5 describes. Newlines are still
 * skipped, so the input is still not the only possible spelling. */
Base64Encoding base64_encoding_strict(const Base64Encoding *enc);

/* ------------------------------------------------------------------ errors */

/* The offset of the input byte where decoding went wrong.
 *
 * Inside an Error it is found with errors_as and
 * TYPE_BASE64_CORRUPT_INPUT_ERROR, which gives a pointer to the offset. The
 * message is Go's, "illegal base64 data at input byte 4". The errors decoding
 * returns live in the calling goroutine's error arena, like strconv's. */
typedef int64_t Base64CorruptInputError;

extern const Type *const TYPE_BASE64_CORRUPT_INPUT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str base64_corrupt_input_error_error(Base64CorruptInputError e,
                                                      Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error base64_corrupt_input_error_as_error(Base64CorruptInputError e,
                                                           Alloc *a);

/* ----------------------------------------------------------------- lengths */

/* The length of the encoding of n bytes. */
Int base64_encoding_encoded_len(const Base64Encoding *enc, Int n);

/* The most bytes that n characters can decode to. With padding that is n / 4
 * * 3, and the real count can be up to two less. */
Int base64_encoding_decoded_len(const Base64Encoding *enc, Int n);

/* ---------------------------------------------------------------- one shot */

/* Writes the encoding of src to the first base64_encoding_encoded_len bytes
 * of dst. The output is padded, so this is for a whole message and not for a
 * piece of a stream: use base64_new_encoder for that. A dst too short for it
 * panics with the index out of range error Go gives. */
void base64_encoding_encode(const Base64Encoding *enc, Slice dst, Slice src);

/* src encoded onto the end of dst, growing it from a if it has to. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice base64_encoding_append_encode(
    const Base64Encoding *enc, Alloc *a, Slice dst, Slice src);

/* The encoding of src, as a new string in a. */
BURROW_OWNS(ret) Str base64_encoding_encode_to_string(const Base64Encoding *enc,
                                                      Alloc *a, Slice src);

/* Decodes src into dst and returns how many bytes it wrote. dst has to hold
 * base64_encoding_decoded_len(src.len) bytes. On bad input the result is the
 * bytes decoded before it and err gets a Base64CorruptInputError. */
Int base64_encoding_decode(const Base64Encoding *enc, Slice dst, Slice src, Error *err);

/* src decoded onto the end of dst, growing it from a if it has to. On bad
 * input the result holds what decoded before it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice base64_encoding_append_decode(
    const Base64Encoding *enc, Alloc *a, Slice dst, Slice src, Error *err);

/* The bytes s decodes to, as a new slice in a. On bad input the result holds
 * what decoded before it. */
BURROW_OWNS(ret) Slice base64_encoding_decode_string(const Base64Encoding *enc,
                                                     Alloc *a, Str s, Error *err);

/* --------------------------------------------------------------- streaming */

/* An IoWriteCloser that encodes what is written to it with enc and writes
 * that to w. It holds back up to two bytes until it has three, so Close has
 * to be called at the end to write them and the padding. Writing after Close
 * is not allowed. enc has to outlive the encoder. The state comes from a, and
 * the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, enc, w) IoWriteCloser
base64_new_encoder(Alloc *a, const Base64Encoding *enc, IoWriter w);

/* An IoReader that decodes what it reads from r with enc, skipping newlines.
 * enc has to outlive the decoder. The state comes from a, and the result is
 * nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, enc, r) IoReader
base64_new_decoder(Alloc *a, const Base64Encoding *enc, IoReader r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_BASE64_H */
