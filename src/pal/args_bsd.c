/* The command line on the BSDs, from sysctl, which asks the kernel for the
 * argv it set up at exec.
 *
 * FreeBSD and DragonFly answer KERN_PROC_ARGS with the arguments, a NUL after
 * each, which is the form pal_args hands back. NetBSD and OpenBSD take a
 * fourth name, KERN_PROC_ARGV. NetBSD answers in the same form, and OpenBSD
 * with an argv array of its own, the pointers first and the strings after
 * them, which are moved down over the pointers here.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, -1};
#else
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, (int)getpid(), KERN_PROC_ARGV};
#endif
    /* A NULL buf asks sysctl for the size and gets no strings, and there is
     * always at least the program name. */
    if (cap <= 0) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    size_t len = (size_t)cap;
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        BURROW_OUT(err, errno == ENOMEM ? PAL_ERANGE : burrow__pal_errno(errno));
        return -1;
    }
#if defined(BURROW_OS_OPENBSD)
    /* The kernel copies the strings out one after another, so they are one
     * block starting at argv[0], and moving that block down to the start of
     * buf is the whole job. Moving them one at a time would write over
     * pointers not yet read. */
    char **argv = (char **)(void *)buf;
    if (argv[0] == NULL)
        return 0;
    size_t total = 0;
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (argv[i] != argv[0] + total) {
            BURROW_OUT(err, PAL_ENOSYS);
            return -1;
        }
        total += strlen(argv[i]) + 1;
    }
    memmove(buf, argv[0], total);
    return (int64_t)total;
#else
    return (int64_t)len;
#endif
}

#endif /* the BSDs */
