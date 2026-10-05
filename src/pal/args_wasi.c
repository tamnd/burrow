/* The command line on wasip1, from args_get, which is where the host puts it
 * and where wasi-libc gets main's argv. Asked for directly rather than kept
 * from main, so that a library linked into somebody else's program sees it too.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <wasi/api.h>

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    __wasi_size_t argc = 0;
    __wasi_size_t size = 0;
    __wasi_errno_t e = __wasi_args_sizes_get(&argc, &size);
    if (e != 0) {
        BURROW_OUT(err, burrow__pal_errno((int)e));
        return -1;
    }
    if (argc == 0 || size == 0)
        return 0;
    if ((int64_t)size > cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }

    /* args_get writes the strings, a NUL after each, one after the other, which
     * is the layout wanted here, and a pointer to each, which is not wanted at
     * all but has to go somewhere. */
    uint8_t **argv = calloc(argc, sizeof *argv);
    if (argv == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        return -1;
    }
    e = __wasi_args_get(argv, (uint8_t *)buf);
    free(argv);
    if (e != 0) {
        BURROW_OUT(err, burrow__pal_errno((int)e));
        return -1;
    }
    return (int64_t)size;
}

#endif /* BURROW_OS_WASI */
