/* No registry, and no TIME_ZONE_INFORMATION, which is every system but
 * Windows.
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

bool pal_tz_info(PalTzInfo *out, PalErrno *err) {
    (void)out;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

bool pal_tz_key_names(const char *key, int64_t key_len, char *std, int64_t *std_len,
                      char *dst, int64_t *dst_len, PalErrno *err) {
    (void)key;
    (void)key_len;
    (void)std;
    (void)std_len;
    (void)dst;
    (void)dst_len;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

bool pal_tz_english_name(const char *std, int64_t std_len, const char *dst,
                         int64_t dst_len, char *out, int64_t *out_len, PalErrno *err) {
    (void)std;
    (void)std_len;
    (void)dst;
    (void)dst_len;
    (void)out;
    (void)out_len;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

#endif /* !BURROW_OS_WINDOWS */
