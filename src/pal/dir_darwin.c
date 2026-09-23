/* Reading a directory on macOS and iOS.
 *
 * getattrlistbulk, which is the documented way to read a directory in batches
 * on Apple's systems and the one that needs no DIR. readdir would need
 * fdopendir, which allocates and takes the descriptor away from the caller,
 * and __getdirentries64, which is what libc uses underneath, is private.
 *
 * Each record is a length, the set of attributes the filesystem actually
 * returned, and then the attributes asked for in the order of their bits. The
 * name is an attrreference_t, an offset from itself and a length that counts
 * the NUL. getattrlistbulk never returns "." or "..", so there is nothing to
 * filter.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/vnode.h>
#include <unistd.h>

static uint32_t type_from_vtype(uint32_t t) {
    switch (t) {
    case VREG:
        return PAL_S_IFREG;
    case VDIR:
        return PAL_S_IFDIR;
    case VLNK:
        return PAL_S_IFLNK;
    case VBLK:
        return PAL_S_IFBLK;
    case VCHR:
        return PAL_S_IFCHR;
    case VFIFO:
        return PAL_S_IFIFO;
    case VSOCK:
        return PAL_S_IFSOCK;
    default:
        return 0;
    }
}

static const unsigned char *dir_take(const unsigned char **p, size_t n) {
    const unsigned char *at = *p;
    *p += n;
    return at;
}

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL || d->fd < 0 || d->fd > INT32_MAX) {
        BURROW_OUT(err, d == NULL || out == NULL ? PAL_EINVAL : PAL_EBADF);
        return false;
    }

    /* pos and len count records here rather than bytes, because the records
     * are variable length and the next one is found by walking. state is the
     * byte offset of record pos. */
    if (d->pos >= d->len) {
        struct attrlist al = {0};
        al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE |
                        ATTR_CMN_FILEID;

        int n;
        do {
            n = getattrlistbulk((int)d->fd, &al, d->buf, sizeof d->buf, 0);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return false;
        }
        if (n == 0)
            return false;
        d->pos = 0;
        d->len = n;
        d->state = 0;
    }

    const unsigned char *rec = (const unsigned char *)d->buf + d->state;
    uint32_t reclen;
    memcpy(&reclen, rec, sizeof reclen);
    d->pos++;
    d->state += reclen;

    const unsigned char *p = rec + sizeof reclen;
    attribute_set_t got;
    memcpy(&got, dir_take(&p, sizeof got), sizeof got);

    *out = (PalDirEntry){0};
    if (got.commonattr & ATTR_CMN_NAME) {
        const unsigned char *ref_at = dir_take(&p, sizeof(attrreference_t));
        attrreference_t ref;
        memcpy(&ref, ref_at, sizeof ref);
        size_t len = ref.attr_length > 0 ? (size_t)ref.attr_length - 1 : 0;
        if (len > PAL_NAME_MAX) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return false;
        }
        memcpy(out->name, ref_at + ref.attr_dataoffset, len);
        out->name[len] = '\0';
        out->name_len = (int32_t)len;
    }
    if (got.commonattr & ATTR_CMN_OBJTYPE) {
        fsobj_type_t t;
        memcpy(&t, dir_take(&p, sizeof t), sizeof t);
        out->type = type_from_vtype((uint32_t)t);
    }
    if (got.commonattr & ATTR_CMN_FILEID) {
        uint64_t ino;
        memcpy(&ino, dir_take(&p, sizeof ino), sizeof ino);
        out->ino = ino;
    }
    return true;
}

#endif /* BURROW_OS_DARWIN || BURROW_OS_IOS */
