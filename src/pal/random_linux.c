/* The system generator on Linux: getrandom.
 *
 * It needs a 3.17 kernel, which is 2014, and glibc 2.25 or musl 1.1.20 for the
 * wrapper, which are 2017 and 2018. All three are below the oldest system
 * docs/design/10-packages-os.md commits to.
 *
 * The wrapper and not the syscall, which is the one place in this file that
 * bends PAL rule 2. A system with the kernel and not the wrapper exists, and it
 * is old enough that reaching it would mean an inline syscall per architecture
 * for a call that happens once per thread at startup. If a supported
 * distribution ever turns up without the wrapper, the answer is to add the
 * syscall here and not to lower the floor.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stddef.h>
#include <sys/random.h>
#include <sys/types.h>

bool pal_random_bytes(void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (n == 0)
        return true;

    /* getrandom can come back with less than was asked for and can be cut short
     * by a signal, so it gets a loop. Zero flags, so no GRND_RANDOM and no
     * GRND_NONBLOCK: the only case where this blocks is a machine that has not
     * gathered any entropy at all yet, where waiting is the right answer and
     * taking whatever is lying around is not.
     *
     * There is no attempt limit. A kernel that returns EINTR forever is a
     * kernel nothing wins against, and giving up here would hand a caller fewer
     * bytes than it asked for with no way to tell, which for a key is worse
     * than not returning. A caller that needs a deadline puts one above this. */
    unsigned char *p = (unsigned char *)buf;
    size_t want = (size_t)n;

    while (want > 0) {
        ssize_t got = getrandom(p, want, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            BURROW_OUT(err, burrow__pal_errno(errno));
            return false;
        }
        p += got;
        want -= (size_t)got;
    }

    return true;
}

#endif /* BURROW_OS_LINUX */
