/* No system generator, for a platform that has nowhere to ask.
 *
 * This is wasm, it is a freestanding target, and it is any system somebody adds
 * to platform.h before adding it to one of the three files next to this one.
 *
 * It fails rather than mixing an address and a clock together and calling the
 * result random. A caller that can carry on with a weaker source is entitled to
 * decide that for itself, and src/runtime/rand.c does exactly that for map
 * iteration order, which needs to differ between runs and does not need to
 * resist anybody. A caller that cannot carry on, which is everything under
 * crypto, gets an error and stops, and that is the only safe answer a layer
 * this low can give.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_WINDOWS) &&                        \
    !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_IOS) &&                           \
    !defined(BURROW_OS_FREEBSD) && !defined(BURROW_OS_NETBSD) &&                       \
    !defined(BURROW_OS_OPENBSD) && !defined(BURROW_OS_DRAGONFLY) &&                    \
    !defined(BURROW_OS_SOLARIS) && !defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

bool pal_random_bytes(void *buf, int64_t n, PalErrno *err) {
    (void)buf;
    (void)n;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#endif /* no system generator */
