/* The user and group databases on Windows. There is no passwd or group file
 * and no getpwnam_r, so these say so and os/user asks the security APIs
 * instead.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

PalErrno pal_getpwnam(const char *name, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)name, (void)pw, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getpwuid(uint32_t uid, PalPasswd *pw, char *buf, int64_t cap,
                      bool *found) {
    (void)uid, (void)pw, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getgrnam(const char *name, PalGroup *gr, char *buf, int64_t cap,
                      bool *found) {
    (void)name, (void)gr, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

PalErrno pal_getgrgid(uint32_t gid, PalGroup *gr, char *buf, int64_t cap, bool *found) {
    (void)gid, (void)gr, (void)buf, (void)cap;
    *found = false;
    return PAL_ENOTSUP;
}

int64_t pal_user_buf_size(bool group) {
    (void)group;
    return -1;
}

int pal_getgrouplist(const char *name, uint32_t gid, uint32_t *gids, int *n) {
    (void)name, (void)gid, (void)gids;
    *n = 0;
    return -1;
}

#endif /* BURROW_OS_WINDOWS */
