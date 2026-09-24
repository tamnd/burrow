/* Reading a directory under Cosmopolitan.
 *
 * Cosmopolitan has no getdents to call, only opendir and readdir, and a DIR
 * owns memory and a descriptor that PalDir has no close call to give back. So
 * no DIR outlives a call. Each time the buffer runs dry, a DIR is opened on a
 * duplicate of the descriptor, the entries already handed out are skipped, as
 * many of the rest as fit are packed into the caller's buffer, and the DIR is
 * closed again. A directory too big for one buffer is read from the start
 * once for every buffer it fills, which costs time on huge directories and
 * leaks nothing when a caller stops early.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_COSMO)

#include "burrow/pal.h"

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* One packed entry: the name's length, the type, the inode, then the name
 * without its terminator. */
enum { COSMO_DIR_HEAD = 2 + 1 + 8 };

/* PalDir.pad marks the end, so that a call after it stays at the end. */
enum { COSMO_DIR_DONE = 1 };

static bool cosmo_dir_skip(const char *name) {
    return (name[0] == '.' && name[1] == '\0') ||
           (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

/* Refills the buffer with the entries after the first d->state. False with
 * *err set is a failure, and false with PAL_OK is the end. */
static bool cosmo_dir_fill(PalDir *d, PalErrno *err) {
    int fd = dup((int)d->fd);
    if (fd < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    DIR *dir = fdopendir(fd);
    if (dir == NULL) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        close(fd);
        return false;
    }
    /* The duplicate shares its offset with the caller's descriptor, which
     * the last fill left wherever its DIR stopped reading. */
    rewinddir(dir);

    unsigned char *buf = (unsigned char *)d->buf;
    size_t cap = sizeof d->buf;
    size_t len = 0;
    uint32_t index = 0;
    uint32_t packed = 0;
    PalErrno fail = PAL_OK;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (e == NULL) {
            if (errno != 0)
                fail = burrow__pal_errno(errno);
            else if (packed == 0)
                d->pad = COSMO_DIR_DONE;
            break;
        }
        if (cosmo_dir_skip(e->d_name))
            continue;
        if (index++ < d->state)
            continue;
        size_t n = strlen(e->d_name);
        if (n > PAL_NAME_MAX) {
            fail = PAL_ENAMETOOLONG;
            break;
        }
        if (len + COSMO_DIR_HEAD + n > cap) {
            if (packed == 0)
                fail = PAL_ENAMETOOLONG;
            break;
        }
        uint16_t n16 = (uint16_t)n;
        uint8_t type = (uint8_t)e->d_type;
        uint64_t ino = (uint64_t)e->d_ino;
        memcpy(buf + len, &n16, 2);
        buf[len + 2] = type;
        memcpy(buf + len + 3, &ino, 8);
        memcpy(buf + len + COSMO_DIR_HEAD, e->d_name, n);
        len += COSMO_DIR_HEAD + n;
        packed++;
    }
    closedir(dir);

    if (fail != PAL_OK) {
        BURROW_OUT(err, fail);
        return false;
    }
    d->state += packed;
    d->pos = 0;
    d->len = (int32_t)len;
    return packed > 0;
}

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL || d->fd < 0 || d->fd > INT32_MAX) {
        BURROW_OUT(err, d == NULL || out == NULL ? PAL_EINVAL : PAL_EBADF);
        return false;
    }

    if (d->pos >= d->len) {
        if (d->pad == COSMO_DIR_DONE || !cosmo_dir_fill(d, err))
            return false;
    }

    const unsigned char *p = (const unsigned char *)d->buf + d->pos;
    uint16_t n;
    uint64_t ino;
    memcpy(&n, p, 2);
    memcpy(&ino, p + 3, 8);
    memcpy(out->name, p + COSMO_DIR_HEAD, n);
    out->name[n] = '\0';
    out->name_len = (int32_t)n;
    out->type = (uint32_t)p[2] << 12;
    out->ino = ino;
    d->pos += (int32_t)(COSMO_DIR_HEAD + n);
    return true;
}

#endif /* BURROW_OS_COSMO */
