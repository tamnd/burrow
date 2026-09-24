/* encoding/ascii85, the btoa and Adobe encoding of bytes as text.
 *
 * Four bytes become five characters from '!' to 'u', and four zero bytes
 * become the single character 'z'. PostScript and PDF use it, and it is
 * denser than base64. There is one alphabet and no padding, so the calls are
 * plain functions:
 *
 *     Slice dst = slice_make(a, TYPE_BYTE, ascii85_max_encoded_len(src.len), 0);
 *     dst.len = ascii85_encode(dst, src);
 *
 * Decoding skips spaces and control characters. Neither side adds or looks
 * for the <~ and ~> that mark ascii85 in a PostScript file, as in Go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/ascii85 */

#ifndef BURROW_ENCODING_ASCII85_H
#define BURROW_ENCODING_ASCII85_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ errors */

/* The offset of the input byte where decoding went wrong.
 *
 * Inside an Error it is found with errors_as and
 * TYPE_ASCII85_CORRUPT_INPUT_ERROR, which gives a pointer to the offset. The
 * message is Go's, "illegal ascii85 data at input byte 4". The errors decoding
 * returns live in the calling goroutine's error arena, like strconv's. */
typedef int64_t Ascii85CorruptInputError;

extern const Type *const TYPE_ASCII85_CORRUPT_INPUT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str ascii85_corrupt_input_error_error(Ascii85CorruptInputError e,
                                                       Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error ascii85_corrupt_input_error_as_error(Ascii85CorruptInputError e,
                                                            Alloc *a);

/* ---------------------------------------------------------------- encoding */

/* The most characters n bytes can encode to, which is n rounded up to four,
 * times five over four. Runs of zeros come out shorter. */
Int ascii85_max_encoded_len(Int n);

/* Encodes src into dst and returns how many characters it wrote. dst needs
 * ascii85_max_encoded_len(src.len) bytes: each group is worked out in five
 * bytes of dst, even the last one and a 'z', and one that is too short panics
 * with the index out of range error Go gives. The bytes after the result can
 * be overwritten.
 *
 * The output is a whole message and not a piece of a stream, since the last
 * group is cut short. Use ascii85_new_encoder for a stream. */
Int ascii85_encode(Slice dst, Slice src);

/* An IoWriteCloser that encodes what is written to it and writes that to w.
 * It holds back up to three bytes until it has four, so Close has to be
 * called at the end to write them. Writing after Close is not allowed. The
 * state comes from a, and the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, w) IoWriteCloser ascii85_new_encoder(Alloc *a,
                                                                          IoWriter w);

/* ---------------------------------------------------------------- decoding */

/* Decodes src into dst and returns how many bytes it wrote, with *nsrc set to
 * how many bytes of src it used up. It stops when fewer than four bytes of
 * dst are left, so a caller with more input can empty dst and go on from
 * *nsrc. nsrc may be NULL.
 *
 * With flush false a group of five cut short at the end of src is left for
 * the next call. With flush true src is the end of the input, and the short
 * group is decoded as the end of the message.
 *
 * A byte that is not '!' to 'u', 'z' or a space or control character, or a
 * 'z' inside a group, gives an Ascii85CorruptInputError. So does a single
 * character left over at the end with flush true. Both results are 0 then, as
 * in Go, though dst may have been written. */
Int ascii85_decode(Slice dst, Slice src, bool flush, Int *nsrc, Error *err);

/* An IoReader that decodes what it reads from r. The state comes from a, and
 * the result is nil when a is out of memory. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, r) IoReader ascii85_new_decoder(Alloc *a,
                                                                     IoReader r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_ASCII85_H */
