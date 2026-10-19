/* pal_text_mappings where there is no system call for it. pprof reads
 * /proc/self/maps on these systems, as Go does, and fakes a mapping when that
 * fails too.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

bool pal_text_mappings(PalMappingFn fn, void *env) {
    (void)fn;
    (void)env;
    return false;
}

#endif /* !BURROW_OS_DARWIN && !BURROW_OS_WINDOWS */
