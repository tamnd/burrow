/* No CryptoAPI, which is every system but Windows.
 *
 * crypto/x509 asks for it only on Windows, so nothing calls this today. It is
 * here so the PAL has the same functions on every system.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

bool pal_cert_verify(const PalCertVerifyOpts *opts, PalCertChainFn fn, void *env,
                     PalErrno *err) {
    (void)opts;
    (void)fn;
    (void)env;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* !BURROW_OS_WINDOWS */
