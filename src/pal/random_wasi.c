/* The system generator on wasip1: random_get, which is the host's.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <wasi/api.h>

bool pal_random_bytes(void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* random_get fills the whole buffer or fails. It takes a 32-bit size, so a
     * larger request goes in pieces, though no wasm32 program has the memory
     * to make one. */
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        __wasi_size_t chunk = n > (int64_t)UINT32_MAX ? UINT32_MAX : (__wasi_size_t)n;
        __wasi_errno_t e = __wasi_random_get(p, chunk);
        if (e != 0) {
            BURROW_OUT(err, burrow__pal_errno((int)e));
            return false;
        }
        p += chunk;
        n -= (int64_t)chunk;
    }
    return true;
}

#endif /* BURROW_OS_WASI */
