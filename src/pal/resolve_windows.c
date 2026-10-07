/* The system's resolver on Windows, which is not here yet.
 *
 * Go's Windows lookups are lookup_windows.go, which asks GetAddrInfoW and
 * DnsQuery_W and has a shape of its own rather than the cgo resolver's. They
 * arrive together with it. Until then net uses its own resolver on Windows,
 * which is what it did before this file existed, and these say so.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <string.h>

static void pwgai_nosys(PalLookupError *err) {
    if (err == NULL)
        return;
    memset(err, 0, sizeof *err);
    err->code = PAL_EAI_SYSTEM;
    err->err = PAL_ENOSYS;
}

int64_t pal_getaddrinfo(const char *host, const char *service, int32_t family,
                        int32_t socktype, int32_t protocol, int32_t flags,
                        PalAddrInfo *out, int64_t cap, PalLookupError *err) {
    (void)host;
    (void)service;
    (void)family;
    (void)socktype;
    (void)protocol;
    (void)flags;
    (void)out;
    (void)cap;
    pwgai_nosys(err);
    return -1;
}

bool pal_getnameinfo(const PalSockAddr *addr, char *host, int64_t cap,
                     PalLookupError *err) {
    (void)addr;
    (void)host;
    (void)cap;
    pwgai_nosys(err);
    return false;
}

int64_t pal_res_search(const char *name, int32_t rclass, int32_t rtype, uint8_t *ans,
                       int64_t cap, PalErrno *err) {
    (void)name;
    (void)rclass;
    (void)rtype;
    (void)ans;
    (void)cap;
    BURROW_OUT(err, PAL_ENOSYS);
    return -1;
}

#endif /* BURROW_OS_WINDOWS */
