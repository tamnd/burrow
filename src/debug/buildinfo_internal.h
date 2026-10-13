/* What tests/buildinfo_test.c needs of debug/buildinfo beyond its API: the
 * search for the start of the build information, which Go tests on its own.
 *
 * Derived from Go's src/debug/buildinfo/buildinfo.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/buildinfo */

#ifndef BURROW_SRC_DEBUG_BUILDINFO_INTERNAL_H
#define BURROW_SRC_DEBUG_BUILDINFO_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"

#include <stdbool.h>
#include <stdint.h>

/* The exe interface: the one method the search and the reads need, which
 * sets *sr to a reader of the bytes from addr to the end of the segment or
 * section that holds addr, or gives false and an error when none does. */
typedef struct BuildinfoExe {
    void *ctx;
    bool (*data_reader)(void *ctx, uint64_t addr, IoSectionReader *sr, Error *err);
} BuildinfoExe;

/* The size of the pieces searchMagic reads at a time. */
#define BURROW__BUILDINFO_SEARCH_CHUNK (1 << 20)

/* searchMagic: the address of the first instance of the build information's
 * magic in [start, start+size) that is aligned as it has to be, or the error,
 * which is "not a Go executable" when there is none. The memory it reads with
 * comes from a and goes back before it returns. */
uint64_t burrow__buildinfo_search_magic(Alloc *a, const BuildinfoExe *x, uint64_t start,
                                        uint64_t size, Error *err);

#endif
