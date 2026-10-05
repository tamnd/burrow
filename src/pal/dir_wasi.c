/* Reading a directory on wasip1, with fd_readdir, the way Go's ReadDir does
 * there.
 *
 * fd_readdir fills the buffer with as many entries as fit from a cookie on, and
 * the last of them may be cut off. Each entry carries the cookie for the one
 * after it, so the next fill starts from the last whole entry read. PalDir has
 * no 64-bit field of its own for that, so the cookie lives in state and pad,
 * low half and high half, and the zeroed PalDir a caller starts with is cookie
 * 0, the top of the directory.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>
#include <wasi/api.h>

#define DIRENT_SIZE ((int32_t)sizeof(__wasi_dirent_t))

static uint64_t dir_cookie(const PalDir *d) {
    return (uint64_t)d->state | (uint64_t)d->pad << 32;
}

static void dir_set_cookie(PalDir *d, uint64_t c) {
    d->state = (uint32_t)c;
    d->pad = (uint32_t)(c >> 32);
}

static uint32_t dir_type(__wasi_filetype_t t) {
    switch (t) {
    case __WASI_FILETYPE_BLOCK_DEVICE:
        return PAL_S_IFBLK;
    case __WASI_FILETYPE_CHARACTER_DEVICE:
        return PAL_S_IFCHR;
    case __WASI_FILETYPE_DIRECTORY:
        return PAL_S_IFDIR;
    case __WASI_FILETYPE_REGULAR_FILE:
        return PAL_S_IFREG;
    case __WASI_FILETYPE_SOCKET_DGRAM:
    case __WASI_FILETYPE_SOCKET_STREAM:
        return PAL_S_IFSOCK;
    case __WASI_FILETYPE_SYMBOLIC_LINK:
        return PAL_S_IFLNK;
    default:
        return 0;
    }
}

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL || d->fd < 0 || d->fd > INT32_MAX) {
        BURROW_OUT(err, d == NULL || out == NULL ? PAL_EINVAL : PAL_EBADF);
        return false;
    }

    for (;;) {
        /* A whole header and a whole name, or it is time to read again. */
        __wasi_dirent_t e;
        bool whole = d->len - d->pos >= DIRENT_SIZE;
        if (whole) {
            memcpy(&e, (const char *)d->buf + d->pos, sizeof e);
            whole = (int64_t)(d->len - d->pos - DIRENT_SIZE) >= (int64_t)e.d_namlen;
        }
        if (!whole) {
            /* A short read last time was the end, and anything left over from
             * it is not a whole entry. */
            if (d->len > 0 && d->len < (int32_t)sizeof d->buf)
                return false;
            if (d->pos == 0 && d->len == (int32_t)sizeof d->buf) {
                /* One entry bigger than the whole buffer. */
                BURROW_OUT(err, PAL_ENAMETOOLONG);
                return false;
            }
            __wasi_size_t n = 0;
            __wasi_errno_t r =
                __wasi_fd_readdir((__wasi_fd_t)d->fd, (uint8_t *)d->buf,
                                  (__wasi_size_t)sizeof d->buf, dir_cookie(d), &n);
            if (r != 0) {
                BURROW_OUT(err, burrow__pal_errno((int)r));
                return false;
            }
            d->pos = 0;
            d->len = (int32_t)n;
            if (n == 0)
                return false;
            continue;
        }

        const char *name = (const char *)d->buf + d->pos + DIRENT_SIZE;
        size_t len = (size_t)e.d_namlen;
        d->pos += DIRENT_SIZE + (int32_t)len;
        dir_set_cookie(d, e.d_next);

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
        out->type = dir_type(e.d_type);
        out->ino = e.d_ino;
        return true;
    }
}

#endif /* BURROW_OS_WASI */
