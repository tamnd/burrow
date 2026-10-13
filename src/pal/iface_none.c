/* The network interfaces where burrow has no backend for them.
 *
 * wasip1 and Emscripten have no interfaces a program can see, and Go's
 * interface_stub.go answers an empty list and no error for them, which is
 * what these do there. illumos, AIX and Cosmopolitan do have them, each
 * behind a call of its own, and none of them has a caller yet, so they say
 * PAL_ENOSYS rather than pretend there are none.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI) || defined(BURROW_OS_JS) || defined(BURROW_OS_SOLARIS) ||  \
    defined(BURROW_OS_AIX) || defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

#include <stddef.h>

static int64_t pifn_none(PalIfError *err) {
#if defined(BURROW_OS_WASI) || defined(BURROW_OS_JS)
    (void)err;
    return 0;
#else
    if (err != NULL) {
        err->call = NULL;
        err->err = PAL_ENOSYS;
    }
    return -1;
#endif
}

int64_t pal_if_enumerate(int32_t index, PalInterface *out, int64_t cap,
                         PalIfError *err) {
    (void)index;
    (void)out;
    (void)cap;
    return pifn_none(err);
}

int64_t pal_if_addrs(int32_t index, PalIfAddr *out, int64_t cap, PalIfError *err) {
    (void)index;
    (void)out;
    (void)cap;
    return pifn_none(err);
}

int64_t pal_if_multicast_addrs(int32_t index, PalIfAddr *out, int64_t cap,
                               PalIfError *err) {
    (void)index;
    (void)out;
    (void)cap;
    return pifn_none(err);
}

#endif /* no backend */
