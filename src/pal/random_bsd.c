/* The system generator on macOS, the BSDs and Cosmopolitan: arc4random_buf on
 * macOS, iOS and OpenBSD, and getentropy everywhere else.
 *
 * arc4random_buf is what Go's crypto/rand reads on macOS and OpenBSD. It never
 * fails, it takes any length, and it only goes to the kernel now and then to
 * reseed, as the macOS manual page puts it. getentropy goes to the kernel on
 * every call.
 *
 * It needs no loop, which is the difference from Linux's getrandom: getentropy
 * either fills the whole buffer or fails, and it cannot be cut short by a
 * signal. What it does have is a limit of 256 bytes per call, which is part of
 * the contract rather than an implementation detail, so the loop that is here is
 * over chunks and not over short answers.
 *
 * getentropy is in sys/random.h on every system left that uses it here.
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

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_OPENBSD)
#define BURROW__PAL_ARC4RANDOM 1
#include <stdlib.h>
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

#if defined(BURROW__PAL_ARC4RANDOM)
    arc4random_buf(buf, (size_t)n);
    return true;
#else
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
#endif
}

#endif /* darwin, the bsds and cosmopolitan */
