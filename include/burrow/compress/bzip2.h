/* compress/bzip2, bzip2 decompression.
 *
 * Only the reading half, as in Go: there is no writer.
 *
 *     IoReader zr = bzip2_new_reader(a, compressed);
 *     Slice text = io_read_all(a, zr, &err);
 *     bzip2_reader_free(zr);
 *
 * Concatenated bzip2 files, which is what pbzip2 and `cat a.bz2 b.bz2` make,
 * read back as one stream. The reader pulls bytes one at a time through
 * ReadByte when r has one, and then stops right after the end of the last
 * file. Any other reader gets a bufio.Reader of its own in front of it.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package compress/bzip2 */

#ifndef BURROW_COMPRESS_BZIP2_H
#define BURROW_COMPRESS_BZIP2_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bzip2.StructuralError: the input is not valid bzip2. The message is
 * "bzip2 data invalid: " and then the text, as in "bzip2 data invalid: block
 * checksum mismatch". errors_is matches two of them with the same text, and
 * errors_as with TYPE_BZIP2_STRUCTURAL_ERROR gives a pointer to the text. */
typedef Str Bzip2StructuralError;

extern const Type *const TYPE_BZIP2_STRUCTURAL_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str bzip2_structural_error_error(Bzip2StructuralError e, Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error bzip2_structural_error_as_error(Bzip2StructuralError e,
                                                       Alloc *a);

/* bzip2.NewReader. A reader of the decompressed form of r, from a. It gives
 * io_eof after the last file, io_err_unexpected_eof when the input stops
 * early, and a Bzip2StructuralError for anything else wrong with it. Each
 * block's checksum is checked once the block has been read out, so the bytes
 * of a damaged block come out before the error does. A nil IoReader when a
 * refuses.
 *
 * The block buffer is allocated at the first read, 400 KB for each step of the
 * level in the header, so up to 3.6 MB. Where Go would crash when that fails,
 * this gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) IoReader bzip2_new_reader(Alloc *a, IoReader r);

/* Gives a reader from bzip2_new_reader and everything it holds back to its
 * allocator. A nil IoReader is fine, and anything else not from
 * bzip2_new_reader is left alone. */
void bzip2_reader_free(IoReader r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_COMPRESS_BZIP2_H */
