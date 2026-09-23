/* Reading a directory on Linux.
 *
 * getdents64, straight into the caller's PalDir, and then one entry out of the
 * buffer per call. The system call rather than readdir because readdir wants a
 * DIR, which libc allocates, and because rule 2 in burrow/pal.h says the
 * system call where there is one. It goes through syscall because glibc only
 * grew a wrapper in 2.30 and musl spells its own differently.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The kernel's own layout, written out rather than taken from dirent.h, where
 * glibc and musl disagree about the name and bionic about the header. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL || d->fd < 0 || d->fd > INT32_MAX) {
        BURROW_OUT(err, d == NULL || out == NULL ? PAL_EINVAL : PAL_EBADF);
        return false;
    }

    for (;;) {
        if (d->pos >= d->len) {
            long n;
            do {
                n = syscall(SYS_getdents64, (int)d->fd, (void *)d->buf, sizeof d->buf);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                BURROW_OUT(err, burrow__pal_errno(errno));
                return false;
            }
            if (n == 0)
                return false;
            d->pos = 0;
            d->len = (int32_t)n;
        }

        const struct linux_dirent64 *e =
            (const struct linux_dirent64 *)(const void *)((const char *)d->buf +
                                                          d->pos);
        d->pos += e->d_reclen;

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
        /* DT_ values are the S_IF ones shifted down twelve bits, on every
         * system that has them, and DT_UNKNOWN is zero, which is what
         * PalDirEntry says an unknown type is. */
        out->type = (uint32_t)e->d_type << 12;
        out->ino = e->d_ino;
        return true;
    }
}

#endif /* BURROW_OS_LINUX */
