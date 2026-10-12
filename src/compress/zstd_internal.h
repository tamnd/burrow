/* internal/zstd, a zstd decompressor for the packages that need one.
 *
 * Go keeps this out of the public library, and so does this: debug/elf reads
 * compressed sections with it and nothing else is meant to. It decompresses a
 * stream of zstd frames, skips skippable frames, checks the content checksum
 * when a frame has one, and does not support dictionaries.
 *
 * Derived from Go's src/internal/zstd. Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_COMPRESS_ZSTD_INTERNAL_H
#define BURROW_COMPRESS_ZSTD_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"

typedef struct ZstdReader ZstdReader;

/* zstd.NewReader: a reader of the decompressed contents of input, from a.
 * Nothing is read until the first Read. NULL when a cannot spare the reader. */
BURROW_OWNS(ret) ZstdReader *burrow__zstd_new_reader(Alloc *a, IoReader input);

/* Reader.Reset: starts again on a new input, keeping the buffers. */
void burrow__zstd_reader_reset(ZstdReader *r, IoReader input);

/* Reader.Read and Reader.ReadByte. */
Int burrow__zstd_reader_read(ZstdReader *r, Slice p, Error *err);
Byte burrow__zstd_reader_read_byte(ZstdReader *r, Error *err);

/* r as an io.Reader, borrowing it. */
IoReader burrow__zstd_reader_as_io_reader(ZstdReader *r);

/* Gives r and its buffers back to the allocator it came from. NULL is fine. */
void burrow__zstd_reader_free(ZstdReader *r);

#endif
