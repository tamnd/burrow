/* No CryptoAPI, which is every system but Windows, and no SecTrust, which is
 * every system but macOS and iOS.
 *
 * crypto/x509 asks for each only where it is, so nothing calls these. They are
 * here so the PAL has the same functions on every system.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#include "burrow/pal.h"

#include "internal.h"

#if !defined(BURROW_OS_WINDOWS)

bool pal_cert_verify(const PalCertVerifyOpts *opts, PalCertChainFn fn, void *env,
                     PalErrno *err) {
    (void)opts;
    (void)fn;
    (void)env;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* !BURROW_OS_WINDOWS */

#if !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_IOS)

bool pal_trust_evaluate(const PalTrustOpts *opts, PalTrustFn fn, PalTrustCertFn cert_fn,
                        void *env, PalErrno *err) {
    (void)opts;
    (void)fn;
    (void)cert_fn;
    (void)env;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* !BURROW_OS_DARWIN && !BURROW_OS_IOS */
