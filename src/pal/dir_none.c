/* Reading a directory where burrow has no backend for it yet.
 *
 * WASI, illumos, AIX and Emscripten. Each has a way to do it and none of them
 * has a caller that runs there yet, so this says so rather than guessing.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_WINDOWS) &&                        \
    !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_IOS) &&                           \
    !defined(BURROW_OS_FREEBSD) && !defined(BURROW_OS_NETBSD) &&                       \
    !defined(BURROW_OS_OPENBSD) && !defined(BURROW_OS_DRAGONFLY) &&                    \
    !defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    if (d == NULL || out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif
