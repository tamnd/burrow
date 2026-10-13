/* What debug/macho, debug/pe and debug/plan9obj share: the parts of
 * internal/saferio they use, the reader their Open methods hand out, and the
 * "ZLIB" header Mach-O and PE put on a compressed DWARF section.
 *
 * Derived from Go's src/internal/saferio/io.go and the DWARF methods of
 * src/debug/macho/file.go and src/debug/pe/file.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_DEBUG_OBJFILE_INTERNAL_H
#define BURROW_SRC_DEBUG_OBJFILE_INTERNAL_H

/* burrow:package internal/objfile */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

/* saferio.ReadData: n bytes from r, in a, read a chunk at a time when n is
 * large, so that a length a file lies about costs only the bytes that are
 * really there. */
BURROW_OWNS(ret) Slice burrow__saferio_read_data(Alloc *a, IoReader r, uint64_t n,
                                                 Error *err);

/* saferio.ReadDataAt, the same from off in r. */
BURROW_OWNS(ret) Slice burrow__saferio_read_data_at(Alloc *a, IoReaderAt r, uint64_t n,
                                                    int64_t off, Error *err);

/* saferio.SliceCapWithSize: the capacity to start a slice of c elements of
 * size bytes with, or -1 if c is too large to be a length at all. */
Int burrow__saferio_slice_cap(uint64_t size, uint64_t c);

/* io.NewSectionReader(sr, 0, 1<<63-1) in a box from a, which is what
 * Section.Open and Segment.Open return. When a is out of memory it is a reader
 * whose every call fails with burrow_err_out_of_memory. */
BURROW_OWNS(ret) IoReadSeeker burrow__objfile_open(Alloc *a, IoSectionReader *sr);

/* Gives a reader from burrow__objfile_open back. A nil one is fine. */
void burrow__objfile_reader_free(IoReadSeeker r);

/* A DWARF section's contents with the "ZLIB" header and the 8 byte big-endian
 * length after it decompressed, in a, or b itself when it has no such header.
 * The decompressed bytes are read with saferio.ReadData. */
BURROW_OWNS(ret) Slice burrow__objfile_unzlib(Alloc *a, Slice b, Error *err);

/* cstring: the bytes up to the first NUL, or all of them, copied into a. */
BURROW_OWNS(ret) Str burrow__objfile_cstring(Alloc *a, const Byte *p, Int n);

#endif
