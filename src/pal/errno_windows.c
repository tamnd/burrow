/* Win32 and Winsock errors into PalErrno.
 *
 * Two mappings, because Windows has two numbering schemes and they overlap.
 * GetLastError answers one, WSAGetLastError answers the other, and a socket
 * call that returns a Win32 code or a file call that returns a Winsock one does
 * not happen, so the caller always knows which to ask.
 *
 * The list is shorter than the POSIX one, not because Windows has fewer ways to
 * fail but because most of them are variations that Go's os package flattens
 * anyway. What matters is that the three questions Go can ask of an error, which
 * are does it not exist, does it already exist, and was I not allowed, come out
 * right, and that a socket error a caller branches on is not lost.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

/* winsock2.h first, and it is the one include order in this project that is not
 * alphabetical. windows.h pulls in winsock.h, which is the 1.1 header, and the
 * two define the same names differently, so whichever arrives second loses.
 * Winsock's own documentation says to include winsock2.h first and the mingw
 * header enforces it with a #warning, which under -Werror is an error and is
 * what this file was failing to build with. */
#include <winsock2.h>

#include <windows.h>

PalErrno burrow__pal_errno_win(unsigned long native) {
    switch (native) {
    case ERROR_SUCCESS:
        return PAL_EOTHER; /* nothing failed, so nobody should be asking */
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_NO_MORE_FILES:
    case ERROR_BAD_NET_NAME:
        return PAL_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_CANNOT_MAKE:
    case ERROR_SEEK_ON_DEVICE:
        return PAL_EACCES;
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_TARGET_HANDLE:
        return PAL_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NOT_ENOUGH_QUOTA:
        return PAL_ENOMEM;
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_FUNCTION:
        return PAL_EINVAL;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return PAL_EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return PAL_ENOTEMPTY;
    case ERROR_DIRECTORY:
        return PAL_ENOTDIR;
    case ERROR_WRITE_PROTECT:
        return PAL_EROFS;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return PAL_ENOSPC;
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_FILENAME_EXCED_RANGE:
        return PAL_ENAMETOOLONG;
    case ERROR_INSUFFICIENT_BUFFER:
    case ERROR_MORE_DATA:
        return PAL_ERANGE;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
        return PAL_EPIPE;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
        return PAL_EBUSY;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_INVALID_LEVEL:
        return PAL_ENOTSUP;
    case ERROR_OPERATION_ABORTED:
        return PAL_ECANCELED;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        return PAL_ETIMEDOUT;
    case ERROR_TOO_MANY_OPEN_FILES:
        return PAL_EMFILE;
    case ERROR_NOT_SAME_DEVICE:
        return PAL_EXDEV;
    case ERROR_IO_PENDING:
        return PAL_EINPROGRESS;
    case ERROR_NOT_FOUND:
        return PAL_ESRCH;
    case ERROR_ARITHMETIC_OVERFLOW:
        return PAL_EOVERFLOW;
    case ERROR_CANT_RESOLVE_FILENAME:
        return PAL_ELOOP;
    case ERROR_PRIVILEGE_NOT_HELD:
        return PAL_EPERM;
    case ERROR_NOT_A_REPARSE_POINT:
        return PAL_EINVAL;
    default:
        return PAL_EOTHER;
    }
}

PalErrno burrow__pal_errno_wsa(int native) {
    switch (native) {
    case WSAEINTR:
        return PAL_EINTR;
    case WSAEBADF:
        return PAL_EBADF;
    case WSAEACCES:
        return PAL_EACCES;
    case WSAEFAULT:
        return PAL_EFAULT;
    case WSAEINVAL:
        return PAL_EINVAL;
    case WSAEMFILE:
        return PAL_EMFILE;
    case WSAEWOULDBLOCK:
        return PAL_EAGAIN;
    case WSAEINPROGRESS:
        return PAL_EINPROGRESS;
    case WSAEALREADY:
        return PAL_EALREADY;
    case WSAEADDRINUSE:
        return PAL_EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return PAL_EADDRNOTAVAIL;
    case WSAENETDOWN:
        return PAL_ENETDOWN;
    case WSAENETUNREACH:
        return PAL_ENETUNREACH;
    case WSAENETRESET:
    case WSAECONNRESET:
        return PAL_ECONNRESET;
    case WSAECONNABORTED:
        return PAL_ECONNABORTED;
    case WSAENOBUFS:
        return PAL_ENOBUFS;
    case WSAEISCONN:
        return PAL_EISCONN;
    case WSAENOTCONN:
    case WSAESHUTDOWN:
        return PAL_ENOTCONN;
    case WSAETIMEDOUT:
        return PAL_ETIMEDOUT;
    case WSAECONNREFUSED:
        return PAL_ECONNREFUSED;
    case WSAEHOSTUNREACH:
        return PAL_EHOSTUNREACH;
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
        return PAL_EPROTONOSUPPORT;
    case WSAEAFNOSUPPORT:
        return PAL_EAFNOSUPPORT;
    case WSAEOPNOTSUPP:
        return PAL_ENOTSUP;
    case WSAELOOP:
        return PAL_ELOOP;
    case WSAENAMETOOLONG:
        return PAL_ENAMETOOLONG;
    case WSAHOST_NOT_FOUND:
    case WSANO_DATA:
        return PAL_EHOSTNOTFOUND;
    case WSATRY_AGAIN:
        return PAL_ETRYAGAIN;
    case WSAENOTSOCK:
        return PAL_EBADF;
    default:
        return PAL_EOTHER;
    }
}

#endif /* BURROW_OS_WINDOWS */
