/* encoding/base32, the RFC 4648 encoding of bytes as text in 32 characters.
 *
 * Five bytes become eight characters, and the end is padded with '=' to a
 * multiple of eight. Base32 is longer than base64 but uses only upper case
 * letters and digits, so it survives case folding and reads out loud. The two
 * encodings RFC 4648 defines are here already:
 *
 *     Str s = base32_encoding_encode_to_string(base32_std_encoding, a, data);
 *     Slice b = base32_encoding_decode_string(base32_std_encoding, a, s, &err);
 *
 * base32_hex_encoding is the "extended hex" alphabet, which sorts the same
 * way as the bytes it encodes. Decoding skips '\r' and '\n' wherever they are.
 *
 * Any other alphabet or padding is a Base32Encoding you make, which is a plain
 * value with no pointers in it, so it can live on the stack, in a struct or in
 * a static, and needs no freeing.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/base32 */

#ifndef BURROW_ENCODING_BASE32_H
#define BURROW_ENCODING_BASE32_H

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
#define BASE32_STD_PADDING ((Rune)'=')
#define BASE32_NO_PADDING ((Rune) - 1)

/* An alphabet and a padding character. The fields are the package's, as Go's
 * are unexported, and a Base32Encoding is only ever made by the functions
 * below. */
typedef struct Base32Encoding {
    Byte encode[32];      /* the character for each 5 bit value */
    Byte decode_map[256]; /* the value for each character, or 0xff */
    Rune pad_char;        /* BASE32_NO_PADDING or a byte */
} Base32Encoding;

/* The standard encoding and the extended hex one, both padded with '='. */
extern const Base32Encoding *const base32_std_encoding;
extern const Base32Encoding *const base32_hex_encoding;

/* A padded encoding with the given alphabet, which has to be 32 bytes with
 * none repeated and neither '\r' nor '\n' among them. Anything else panics
 * with Go's message. The bytes are taken as bytes, with no UTF-8 meaning.
 *
 * Go returns a new *Encoding. This returns the value, which is the same thing
 * without an allocation, and the caller passes its address to the rest. */
Base32Encoding base32_new_encoding(Str encoder);

/* enc with a different padding character, or with none for
 * BASE32_NO_PADDING. The character has to be a byte value, not '\r' or '\n',
 * and not in the alphabet, or this panics. One above 0x7f is written as that
 * byte and not as UTF-8. */
Base32Encoding base32_encoding_with_padding(const Base32Encoding *enc, Rune padding);

/* ------------------------------------------------------------------ errors */

/* The offset of the input byte where decoding went wrong, counted with any
 * newlines left out.
 *
 * Inside an Error it is found with errors_as and
 * TYPE_BASE32_CORRUPT_INPUT_ERROR, which gives a pointer to the offset. The
 * message is Go's, "illegal base32 data at input byte 4". The errors decoding
 * returns live in the calling goroutine's error arena, like strconv's. */
typedef int64_t Base32CorruptInputError;

extern const Type *const TYPE_BASE32_CORRUPT_INPUT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str base32_corrupt_input_error_error(Base32CorruptInputError e,
                                                      Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error base32_corrupt_input_error_as_error(Base32CorruptInputError e,
                                                           Alloc *a);

/* ----------------------------------------------------------------- lengths */

/* The length of the encoding of n bytes. */
Int base32_encoding_encoded_len(const Base32Encoding *enc, Int n);

/* The most bytes that n characters can decode to. With padding that is n / 8
 * * 5, and the real count can be up to four less. */
Int base32_encoding_decoded_len(const Base32Encoding *enc, Int n);

/* ---------------------------------------------------------------- one shot */

/* Writes the encoding of src to the first base32_encoding_encoded_len bytes
 * of dst. The output is padded, so this is for a whole message and not for a
 * piece of a stream: use base32_new_encoder for that. A dst too short for it
 * panics with the index out of range error Go gives. */
void base32_encoding_encode(const Base32Encoding *enc, Slice dst, Slice src);

/* src encoded onto the end of dst, growing it from a if it has to. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice base32_encoding_append_encode(
    const Base32Encoding *enc, Alloc *a, Slice dst, Slice src);

/* The encoding of src, as a new string in a. */
BURROW_OWNS(ret) Str base32_encoding_encode_to_string(const Base32Encoding *enc,
                                                      Alloc *a, Slice src);

/* Decodes src into dst and returns how many bytes it wrote. dst has to hold
 * base32_encoding_decoded_len(src.len) bytes, and one that is too short panics
 * the way Go's does. On bad input the result is the bytes decoded before it
 * and err gets a Base32CorruptInputError. Unlike Go's, this does not allocate
 * a copy of src to take the newlines out. */
Int base32_encoding_decode(const Base32Encoding *enc, Slice dst, Slice src, Error *err);

/* src decoded onto the end of dst, growing it from a if it has to. On bad
 * input the result holds what decoded before it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice base32_encoding_append_decode(
    const Base32Encoding *enc, Alloc *a, Slice dst, Slice src, Error *err);

/* The bytes s decodes to, as a new slice in a. On bad input the result holds
 * what decoded before it. */
BURROW_OWNS(ret) Slice base32_encoding_decode_string(const Base32Encoding *enc,
                                                     Alloc *a, Str s, Error *err);

/* --------------------------------------------------------------- streaming */

/* An IoWriteCloser that encodes what is written to it with enc and writes
 * that to w. It holds back up to four bytes until it has five, so Close has
 * to be called at the end to write them and the padding. Writing after Close
 * is not allowed. enc has to outlive the encoder. The state comes from a, and
 * the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, enc, w) IoWriteCloser
base32_new_encoder(Alloc *a, const Base32Encoding *enc, IoWriter w);

/* An IoReader that decodes what it reads from r with enc, skipping newlines.
 * Input that stops part way through a block is io_err_unexpected_eof, and
 * more input after the padding is a Base32CorruptInputError. enc has to
 * outlive the decoder. The state comes from a, and the result is nil when a is
 * out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, enc, r) IoReader
base32_new_decoder(Alloc *a, const Base32Encoding *enc, IoReader r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_BASE32_H */
