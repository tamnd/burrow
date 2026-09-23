/* Reading a directory on FreeBSD, NetBSD, OpenBSD and DragonFly.
 *
 * getdents, which all four have with the same shape, into the caller's PalDir
 * and then one entry out per call. struct dirent is the kernel's record on
 * these systems, so the one from dirent.h is the right one to walk with.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#if defined(__NetBSD__)
#define _NETBSD_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY)

#include "burrow/pal.h"

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL || d->fd < 0 || d->fd > INT32_MAX) {
        BURROW_OUT(err, d == NULL || out == NULL ? PAL_EINVAL : PAL_EBADF);
        return false;
    }

    for (;;) {
        if (d->pos >= d->len) {
            int n;
            do {
                n = (int)getdents((int)d->fd, (void *)d->buf, sizeof d->buf);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                BURROW_OUT(err, burrow__pal_errno(errno));
                return false;
            }
            if (n == 0)
                return false;
            d->pos = 0;
            d->len = n;
        }

        const struct dirent *e =
            (const struct dirent *)(const void *)((const char *)d->buf + d->pos);
        d->pos += e->d_reclen;
        if (e->d_fileno == 0)
            continue; /* a deleted entry the kernel left in place */

        const char *name = e->d_name;
        size_t len = strlen(name);
        if ((len == 1 && name[0] == '.') ||
            (len == 2 && name[0] == '.' && name[1] == '.'))
            continue;
        if (len > PAL_NAME_MAX) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return false;
        }

        memcpy(out->name, name, len);
        out->name[len] = '\0';
        out->name_len = (int32_t)len;
        out->type = (uint32_t)e->d_type << 12;
        out->ino = (uint64_t)e->d_fileno;
        return true;
    }
}

#endif /* the BSDs */
