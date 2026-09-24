/* encoding/hex, bytes as hexadecimal text and back.
 *
 * Each byte becomes two lowercase hex digits, so the text is always twice as
 * long as the data. Decoding takes either case. The calls most people want:
 *
 *     Str s = hex_encode_to_string(a, data);          // "48656c6c6f"
 *     Slice b = hex_decode_string(a, s, &err);
 *
 * hex_encode and hex_decode work into a slice the caller already has, with
 * hex_encoded_len and hex_decoded_len to size it. The streaming forms wrap an
 * IoWriter or an IoReader, and hex_dump gives the same layout as hexdump -C.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/hex */

#ifndef BURROW_ENCODING_HEX_H
#define BURROW_ENCODING_HEX_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- errors */

/* What hex_decode and hex_decode_string report for input of odd length. The
 * decoder from hex_new_decoder reports io_err_unexpected_eof instead, as Go's
 * does, since for a stream the input simply stopped early. */
extern const Error hex_err_length;

/* The byte that was not a hex digit.
 *
 * Inside an Error it is found with errors_as and TYPE_HEX_INVALID_BYTE_ERROR,
 * which gives a pointer to the byte. The errors are static, one per byte
 * value, so two of them for the same byte are errors_is each other the way
 * InvalidByteError('z') == InvalidByteError('z') in Go, and none of them ever
 * needs error_retain. */
typedef Byte HexInvalidByteError;

extern const Type *const TYPE_HEX_INVALID_BYTE_ERROR;

/* Go's Error method, such as "encoding/hex: invalid byte: U+0067 'g'". The
 * text is static. */
BURROW_STATIC(ret) Str hex_invalid_byte_error_error(HexInvalidByteError e);

/* The Error for e, which is what Go gets by converting InvalidByteError(e) to
 * an error. */
BURROW_STATIC(ret) Error hex_invalid_byte_error_as_error(HexInvalidByteError e);

/* ------------------------------------------------------------------ lengths */

/* The length of the encoding of n bytes, which is n * 2. */
Int hex_encoded_len(Int n);

/* The length of the decoding of x hex digits, which is x / 2. */
Int hex_decoded_len(Int x);

/* ----------------------------------------------------------------- one shot */

/* Writes the encoding of src into the first hex_encoded_len(src.len) bytes of
 * dst and returns that length. A dst too short for it panics with the index
 * out of range error Go gives. */
Int hex_encode(Slice dst, Slice src);

/* src encoded onto the end of dst, growing it from a if it has to. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice hex_append_encode(Alloc *a, Slice dst,
                                                                  Slice src);

/* Decodes src into dst and returns how many bytes it wrote. src has to be hex
 * digits in either case and of even length. When it is not, the result is the
 * bytes decoded before the problem, and err gets a HexInvalidByteError for the
 * first bad byte or hex_err_length for a lone digit at the end. A bad byte is
 * reported ahead of the odd length, since it comes first. */
Int hex_decode(Slice dst, Slice src, Error *err);

/* src decoded onto the end of dst, growing it from a if it has to. On bad
 * input the result holds what decoded before the problem. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice hex_append_decode(Alloc *a, Slice dst,
                                                                  Slice src,
                                                                  Error *err);

/* The hex encoding of src, as a new string in a. */
BURROW_OWNS(ret) Str hex_encode_to_string(Alloc *a, Slice src);

/* The bytes s encodes, as a new slice in a. Bad input is reported the way
 * hex_decode reports it, and the result holds what decoded before it. */
BURROW_OWNS(ret) Slice hex_decode_string(Alloc *a, Str s, Error *err);

/* A dump of data in the layout of hexdump -C: an offset, sixteen bytes in hex
 * with a gap after the eighth, then the same sixteen as text with anything
 * unprintable shown as a dot.
 *
 *     00000000  47 6f 70 68 65 72 73 21                           |Gophers!|
 *
 * Empty data gives the empty string. */
BURROW_OWNS(ret) Str hex_dump(Alloc *a, Slice data);

/* ---------------------------------------------------------------- streaming */

/* An IoWriter that writes the hex encoding of what it is given to w, in
 * lowercase. The count it returns is of the bytes whose encoding w took. The
 * state comes from a, and the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, w) IoWriter hex_new_encoder(Alloc *a, IoWriter w);

/* An IoReader that decodes the hex digits it reads from r. r has to hold an
 * even number of them. A lone digit at the end is io_err_unexpected_eof and a
 * byte that is not a digit is a HexInvalidByteError, each reported once the
 * bytes before it have been read. The state comes from a, and the result is
 * nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, r) IoReader hex_new_decoder(Alloc *a, IoReader r);

/* An IoWriteCloser that writes a hex_dump of everything written to it to w.
 * Close finishes the last line, and writing after Close is an error. The
 * state comes from a, and the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, w) IoWriteCloser hex_dumper(Alloc *a, IoWriter w);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_HEX_H */
