/* No command line, for a platform that has nowhere to ask for one.
 *
 * This is wasm, illumos, AIX, a freestanding target, and any system somebody
 * adds to platform.h before adding it to one of the files next to this one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_WINDOWS) &&                        \
    !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_FREEBSD) &&                       \
    !defined(BURROW_OS_NETBSD) && !defined(BURROW_OS_OPENBSD) &&                       \
    !defined(BURROW_OS_DRAGONFLY) && !defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    (void)buf;
    (void)cap;
    (void)err;
    return 0;
}

#endif /* no command line */
