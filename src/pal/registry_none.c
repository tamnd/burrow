/* No registry, which is every system but Windows.
 *
 * mime asks for the registry only on Windows and reads the mime.types files
 * everywhere else, so nothing calls this today. It is here so the PAL has the
 * same functions on every system.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

bool pal_registry_content_types(PalContentTypeFn fn, void *env, PalErrno *err) {
    (void)fn;
    (void)env;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* !BURROW_OS_WINDOWS */
