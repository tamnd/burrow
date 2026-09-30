/* The command line on Linux, from /proc/self/cmdline, which is already the
 * arguments with a NUL after each. It is what the kernel set up on the stack
 * at exec, so it is main's argv whichever C library is underneath, including
 * musl, which passes nothing to constructors that could have caught it.
 *
 * A program that writes over its own argv changes what is read here, as it
 * changes what ps shows.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* No /proc, as in some containers. An empty command line is what
         * a system with nowhere to ask gets too. */
        return 0;
    }
    int64_t n = 0;
    for (;;) {
        char probe;
        char *p = n < cap ? buf + n : &probe;
        size_t want = n < cap ? (size_t)(cap - n) : 1;
        ssize_t got = read(fd, p, want);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            PalErrno e = burrow__pal_errno(errno);
            close(fd);
            BURROW_OUT(err, e);
            return -1;
        }
        if (got == 0)
            break;
        if (n >= cap) {
            close(fd);
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        n += got;
    }
    close(fd);
    /* The kernel ends every argument with a NUL, but a program that wrote
     * over its argv can leave the last one without. */
    if (n > 0 && buf[n - 1] != 0) {
        if (n >= cap) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        buf[n++] = 0;
    }
    return n;
}

#endif /* BURROW_OS_LINUX */
