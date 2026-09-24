/* What the files of the io package share and nobody else sees.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_IO_INTERNAL_H
#define BURROW_SRC_IO_INTERNAL_H

#include "burrow/io.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"

#include <stdint.h>

/* err == EOF, which is identity and not errors.Is. io.c says why. */
bool burrow__io_is_eof(Error e);

/* s as a Slice of Byte without a copy, to hand to a Writer, which only reads
 * it. */
static inline Slice burrow__io_str_bytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* What io_multi_reader and io_multi_writer hand back the data of. The tests
 * look inside to check that nested lists get flattened, which Go checks by
 * counting stack frames. */
typedef struct MultiReader {
    Alloc *a;
    IoReader *readers; /* what is left */
    Int n;
    Int total; /* how many the allocation holds, for freeing it */
} MultiReader;

typedef struct MultiWriter {
    IoWriter *writers;
    Int n;
} MultiWriter;

#endif /* BURROW_SRC_IO_INTERNAL_H */
