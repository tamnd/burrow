/* The command line on macOS, from _NSGetArgv, which is the argv main was
 * given. It is what a shared library has to ask for, since only the
 * executable sees main's arguments.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN)

#include "burrow/pal.h"

#include "internal.h"

#include <crt_externs.h>
#include <string.h>

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();
    int64_t n = 0;
    for (int i = 0; i < argc && argv[i] != NULL; i++) {
        int64_t len = (int64_t)strlen(argv[i]) + 1;
        if (len > cap - n) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        memcpy(buf + n, argv[i], (size_t)len);
        n += len;
    }
    return n;
}

#endif /* BURROW_OS_DARWIN */
