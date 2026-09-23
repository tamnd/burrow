/* PalErrno: the names, and the translation from what the platform said.
 *
 * The table and the enum in burrow/pal.h have to stay in step, and the
 * _Static_assert below is what makes that a build failure rather than an
 * off by one in a log line six months later. The entries are in the enum's
 * order, not alphabetical, so a new code goes at the end of both.
 *
 * The mappings are deliberately not clever. Each one is a switch that names
 * every case it handles and falls through to PAL_EOTHER for the rest, which
 * means a platform code nobody thought about arrives as "something outside the
 * set" rather than as whichever of ours happens to share its number.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/pal.h"

#include "burrow/platform.h"

#include "internal.h"

/* In the enum's order. */
static const char *const errno_names[] = {
    "not the owner",
    "no such file or directory",
    "no such process",
    "interrupted",
    "input/output error",
    "bad file descriptor",
    "no child processes",
    "would deadlock",
    "resource temporarily unavailable",
    "out of memory",
    "permission denied",
    "bad address",
    "resource busy",
    "file exists",
    "cross-device link",
    "no such device",
    "not a directory",
    "is a directory",
    "invalid argument",
    "too many open files in system",
    "too many open files",
    "not a terminal",
    "file too large",
    "no space left on device",
    "illegal seek",
    "read-only file system",
    "too many links",
    "broken pipe",
    "result out of range",
    "file name too long",
    "function not implemented",
    "directory not empty",
    "too many levels of symbolic links",
    "operation not supported",
    "value too large to be stored in its type",
    "operation canceled",
    "operation timed out",
    "address already in use",
    "cannot assign requested address",
    "network is down",
    "network is unreachable",
    "software caused connection abort",
    "connection reset by peer",
    "no buffer space available",
    "socket is already connected",
    "socket is not connected",
    "connection refused",
    "no route to host",
    "operation already in progress",
    "operation now in progress",
    "protocol not supported",
    "address family not supported by protocol",
    "no such host",
    "temporary failure in name resolution",
    "unknown error",
};

_Static_assert((int)(sizeof errno_names / sizeof errno_names[0]) ==
                   PAL_EOTHER - PAL_EPERM + 1,
               "errno_names and the PalErrno enum have drifted apart");

const char *pal_errno_string(PalErrno e) {
    if (e == PAL_OK)
        return "no error";
    if (e < PAL_EPERM || e > PAL_EOTHER)
        return "unknown error";
    return errno_names[e - PAL_EPERM];
}

#if !defined(BURROW_OS_WINDOWS)

#include <errno.h>

/* Several of these are the same number on some platforms and not on others.
 * EAGAIN and EWOULDBLOCK are equal everywhere burrow builds, and ENOTSUP and
 * EOPNOTSUPP are equal on Linux and different on macOS, so both pairs are
 * written as one case with a guard rather than as two cases that fail to
 * compile on whichever platform happens to join them. */
PalErrno burrow__pal_errno(int native) {
    switch (native) {
    case EPERM:
        return PAL_EPERM;
    case ENOENT:
        return PAL_ENOENT;
    case ESRCH:
        return PAL_ESRCH;
    case EINTR:
        return PAL_EINTR;
    case EIO:
        return PAL_EIO;
    case EBADF:
        return PAL_EBADF;
    case ECHILD:
        return PAL_ECHILD;
    case EDEADLK:
        return PAL_EDEADLK;
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return PAL_EAGAIN;
    case ENOMEM:
        return PAL_ENOMEM;
    case EACCES:
        return PAL_EACCES;
    case EFAULT:
        return PAL_EFAULT;
    case EBUSY:
        return PAL_EBUSY;
    case EEXIST:
        return PAL_EEXIST;
    case EXDEV:
        return PAL_EXDEV;
    case ENODEV:
        return PAL_ENODEV;
    case ENOTDIR:
        return PAL_ENOTDIR;
    case EISDIR:
        return PAL_EISDIR;
    case EINVAL:
        return PAL_EINVAL;
    case ENFILE:
        return PAL_ENFILE;
    case EMFILE:
        return PAL_EMFILE;
    case ENOTTY:
        return PAL_ENOTTY;
    case EFBIG:
        return PAL_EFBIG;
    case ENOSPC:
        return PAL_ENOSPC;
    case ESPIPE:
        return PAL_ESPIPE;
    case EROFS:
        return PAL_EROFS;
    case EMLINK:
        return PAL_EMLINK;
    case EPIPE:
        return PAL_EPIPE;
    case ERANGE:
        return PAL_ERANGE;
    case ENAMETOOLONG:
        return PAL_ENAMETOOLONG;
    case ENOSYS:
        return PAL_ENOSYS;
    case ENOTEMPTY:
        return PAL_ENOTEMPTY;
    case ELOOP:
        return PAL_ELOOP;
    case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
        return PAL_ENOTSUP;
    case EOVERFLOW:
        return PAL_EOVERFLOW;
    case ECANCELED:
        return PAL_ECANCELED;
    case ETIMEDOUT:
        return PAL_ETIMEDOUT;
    case EADDRINUSE:
        return PAL_EADDRINUSE;
    case EADDRNOTAVAIL:
        return PAL_EADDRNOTAVAIL;
    case ENETDOWN:
        return PAL_ENETDOWN;
    case ENETUNREACH:
        return PAL_ENETUNREACH;
    case ECONNABORTED:
        return PAL_ECONNABORTED;
    case ECONNRESET:
        return PAL_ECONNRESET;
    case ENOBUFS:
        return PAL_ENOBUFS;
    case EISCONN:
        return PAL_EISCONN;
    case ENOTCONN:
        return PAL_ENOTCONN;
    case ECONNREFUSED:
        return PAL_ECONNREFUSED;
    case EHOSTUNREACH:
        return PAL_EHOSTUNREACH;
    case EALREADY:
        return PAL_EALREADY;
    case EINPROGRESS:
        return PAL_EINPROGRESS;
    case EPROTONOSUPPORT:
        return PAL_EPROTONOSUPPORT;
    case EAFNOSUPPORT:
        return PAL_EAFNOSUPPORT;
    default:
        return PAL_EOTHER;
    }
}

#endif /* !BURROW_OS_WINDOWS */
