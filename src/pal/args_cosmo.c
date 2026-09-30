/* The command line under cosmopolitan, whose C library keeps the argv it gave
 * main in __argv, the same on every system it runs on.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

#include <stdlib.h>
#include <string.h>

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    int64_t n = 0;
    for (int i = 0; i < __argc && __argv[i] != NULL; i++) {
        int64_t len = (int64_t)strlen(__argv[i]) + 1;
        if (len > cap - n) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        memcpy(buf + n, __argv[i], (size_t)len);
        n += len;
    }
    return n;
}

#endif /* BURROW_OS_COSMO */
