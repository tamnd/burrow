/* The system generator on macOS, the BSDs and Cosmopolitan: getentropy.
 *
 * It needs no loop, which is the difference from Linux's getrandom: getentropy
 * either fills the whole buffer or fails, and it cannot be cut short by a
 * signal. What it does have is a limit of 256 bytes per call, which is part of
 * the contract rather than an implementation detail, so the loop that is here is
 * over chunks and not over short answers.
 *
 * The header it lives in differs. macOS and most of the BSDs put it in
 * sys/random.h, OpenBSD puts it in unistd.h, and both are the same function.
 *
 * Cosmopolitan is here rather than with Linux because one of its binaries runs
 * on all of these, and getentropy is the call its libc answers on every one of
 * them.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_SOLARIS) || defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stddef.h>

#if defined(BURROW_OS_OPENBSD)
#include <unistd.h>
#else
#include <sys/random.h>
#endif

/* What getentropy will take in one call, which every system that has it
 * documents as this number. */
#define GETENTROPY_MAX 256

bool pal_random_bytes(void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (n == 0)
        return true;

    unsigned char *p = (unsigned char *)buf;
    size_t want = (size_t)n;

    while (want > 0) {
        size_t chunk = want < GETENTROPY_MAX ? want : (size_t)GETENTROPY_MAX;

        if (getentropy(p, chunk) != 0) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return false;
        }

        p += chunk;
        want -= chunk;
    }

    return true;
}

#endif /* darwin, the bsds and cosmopolitan */
