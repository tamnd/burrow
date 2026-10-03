/* syscall.Errno. See include/burrow/syscall.h.
 *
 * Derived from Go's src/syscall/syscall_unix.go.
 * Go source: go1.27.1.
 *
 * The Windows rules are from syscall_windows.go in the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "burrow/declare.h"
#include "burrow/io/fs.h"

#include "internal.h"

#include <string.h>

#if defined(BURROW_OS_WINDOWS)
/* Codes Go names in syscall_windows.go without exporting them. */
#define ERRNO_ERROR_NOT_SUPPORTED ((SyscallErrno)50)
#define ERRNO_ERROR_BAD_NETPATH ((SyscallErrno)53)
#define ERRNO_ERROR_CALL_NOT_IMPLEMENTED ((SyscallErrno)120)
#endif

/* The text Go has for e in its table, or NULL. */
static const char *errno_table(SyscallErrno e) {
    Uintptr idx = e - burrow__syscall_errors_base;
    if (idx >= (Uintptr)burrow__syscall_nerrors)
        return NULL;
    return burrow__syscall_errors[idx];
}

/* prefix and then int(e) in decimal, into buf, which has room for both. Go
 * converts with int(e), so a number past the top of Int prints negative. */
static Int errno_numbered(char *buf, const char *prefix, SyscallErrno e) {
    Int n = (Int)strlen(prefix);
    memcpy(buf, prefix, (size_t)n);
    int64_t v = (int64_t)(Int)e;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    char digits[20];
    int nd = 0;
    do {
        digits[nd++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        buf[n++] = '-';
    while (nd > 0)
        buf[n++] = digits[--nd];
    return n;
}

/* Errno.Error's text, into buf, which has ERRNO_TEXT_MAX bytes. Table entries
 * come back as themselves and buf is left alone. */
#define ERRNO_TEXT_MAX 1024

static Str errno_text(SyscallErrno e, char *buf) {
    const char *s = errno_table(e);
    if (s != NULL)
        return str_from_bytes((const Byte *)s, (Int)strlen(s));
#if defined(BURROW_OS_WINDOWS)
    /* FormatMessageW into 300 UTF-16 units, as Go does, is at most 900 bytes
     * of UTF-8. */
    PalErrno err = PAL_OK;
    int64_t n = pal_errno_message((int64_t)e, buf, ERRNO_TEXT_MAX, &err);
    if (n >= 0)
        return str_from_bytes((const Byte *)buf, (Int)n);
    return str_from_bytes((const Byte *)buf, errno_numbered(buf, "winapi error #", e));
#else
    return str_from_bytes((const Byte *)buf, errno_numbered(buf, "errno ", e));
#endif
}

Str syscall_errno_error(SyscallErrno e, Alloc *a) {
    char buf[ERRNO_TEXT_MAX];
    Str t = errno_text(e, buf);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)t.len + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, t.p, (size_t)t.len);
    p[t.len] = 0;
    return str_from_bytes(p, t.len);
}

/* errors_is compares sentinels by identity, and so does Go's switch on
 * target. */
static bool errno_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

bool syscall_errno_is(SyscallErrno e, Error target) {
    if (errno_same(target, fs_err_permission)) {
#if defined(BURROW_OS_WINDOWS)
        if (e == SYSCALL_ERROR_ACCESS_DENIED)
            return true;
#endif
        return e == SYSCALL_EACCES || e == SYSCALL_EPERM;
    }
    if (errno_same(target, fs_err_exist)) {
#if defined(BURROW_OS_WINDOWS)
        if (e == SYSCALL_ERROR_ALREADY_EXISTS || e == SYSCALL_ERROR_DIR_NOT_EMPTY ||
            e == SYSCALL_ERROR_FILE_EXISTS)
            return true;
#endif
        return e == SYSCALL_EEXIST || e == SYSCALL_ENOTEMPTY;
    }
    if (errno_same(target, fs_err_not_exist)) {
#if defined(BURROW_OS_WINDOWS)
        if (e == SYSCALL_ERROR_FILE_NOT_FOUND || e == ERRNO_ERROR_BAD_NETPATH ||
            e == SYSCALL_ERROR_PATH_NOT_FOUND)
            return true;
#endif
        return e == SYSCALL_ENOENT;
    }
    if (errno_same(target, errors_err_unsupported)) {
#if defined(BURROW_OS_WINDOWS)
        if (e == ERRNO_ERROR_NOT_SUPPORTED || e == ERRNO_ERROR_CALL_NOT_IMPLEMENTED ||
            e == SYSCALL_EWINDOWS)
            return true;
#endif
        /* ENOTSUP and EOPNOTSUPP are the same number on Linux. */
        /* NOLINTNEXTLINE(misc-redundant-expression) */
        return e == SYSCALL_ENOSYS || e == SYSCALL_ENOTSUP || e == SYSCALL_EOPNOTSUPP;
    }
    return false;
}

bool syscall_errno_timeout(SyscallErrno e) {
    return e == SYSCALL_EAGAIN || e == SYSCALL_EWOULDBLOCK || e == SYSCALL_ETIMEDOUT;
}

bool syscall_errno_temporary(SyscallErrno e) {
#if defined(BURROW_OS_WINDOWS)
    return e == SYSCALL_EINTR || e == SYSCALL_EMFILE || syscall_errno_timeout(e);
#else
    return e == SYSCALL_EINTR || e == SYSCALL_EMFILE || e == SYSCALL_ENFILE ||
           syscall_errno_timeout(e);
#endif
}

/* The E constant for each PalErrno that has one, by PalErrno - PAL_EPERM. */
static const SyscallErrno errno_from_pal[] = {
    SYSCALL_EPERM,        SYSCALL_ENOENT,       SYSCALL_ESRCH,
    SYSCALL_EINTR,        SYSCALL_EIO,          SYSCALL_EBADF,
    SYSCALL_ECHILD,       SYSCALL_EDEADLK,      SYSCALL_EAGAIN,
    SYSCALL_ENOMEM,       SYSCALL_EACCES,       SYSCALL_EFAULT,
    SYSCALL_EBUSY,        SYSCALL_EEXIST,       SYSCALL_EXDEV,
    SYSCALL_ENODEV,       SYSCALL_ENOTDIR,      SYSCALL_EISDIR,
    SYSCALL_EINVAL,       SYSCALL_ENFILE,       SYSCALL_EMFILE,
    SYSCALL_ENOTTY,       SYSCALL_EFBIG,        SYSCALL_ENOSPC,
    SYSCALL_ESPIPE,       SYSCALL_EROFS,        SYSCALL_EMLINK,
    SYSCALL_EPIPE,        SYSCALL_ERANGE,       SYSCALL_ENAMETOOLONG,
    SYSCALL_ENOSYS,       SYSCALL_ENOTEMPTY,    SYSCALL_ELOOP,
    SYSCALL_ENOTSUP,      SYSCALL_EOVERFLOW,    SYSCALL_ECANCELED,
    SYSCALL_ETIMEDOUT,    SYSCALL_EADDRINUSE,   SYSCALL_EADDRNOTAVAIL,
    SYSCALL_ENETDOWN,     SYSCALL_ENETUNREACH,  SYSCALL_ECONNABORTED,
    SYSCALL_ECONNRESET,   SYSCALL_ENOBUFS,      SYSCALL_EISCONN,
    SYSCALL_ENOTCONN,     SYSCALL_ECONNREFUSED, SYSCALL_EHOSTUNREACH,
    SYSCALL_EALREADY,     SYSCALL_EINPROGRESS,  SYSCALL_EPROTONOSUPPORT,
    SYSCALL_EAFNOSUPPORT,
};

_Static_assert(sizeof errno_from_pal / sizeof errno_from_pal[0] ==
                   PAL_EHOSTNOTFOUND - PAL_EPERM,
               "one E constant for each PalErrno up to PAL_EHOSTNOTFOUND");

SyscallErrno syscall_errno_from_pal(PalErrno e) {
    if (e < PAL_EPERM || e > PAL_EOTHER)
        return 0;
    /* The system's own code first. For PAL_EOTHER, a Windows error with no
     * PalErrno of its own, it is the only answer there is. */
    int64_t native = pal_errno_native(e);
    if (native != 0)
        return (SyscallErrno)native;
    if (e >= PAL_EHOSTNOTFOUND)
        return 0;
    return errno_from_pal[e - PAL_EPERM];
}

/* --------------------------------------------------------------- the type */

static Str errno_m_error(SyscallErrno *self) {
    return syscall_errno_error(*self, error_allocator());
}

static bool errno_m_is(SyscallErrno *self, Error target) {
    return syscall_errno_is(*self, target);
}

static bool errno_m_temporary(SyscallErrno *self) {
    return syscall_errno_temporary(*self);
}

static bool errno_m_timeout(SyscallErrno *self) {
    return syscall_errno_timeout(*self);
}

#define ERRNO_SIG_STRING(IN, OUT) OUT(Str)
#define ERRNO_SIG_IS(IN, OUT) IN(0, Error) OUT(bool)
#define ERRNO_SIG_BOOL(IN, OUT) OUT(bool)

#define ERRNO_METHODS(M, T)                                                            \
    M(T, Error, errno_m_error, ERRNO_SIG_STRING)                                       \
    M(T, Is, errno_m_is, ERRNO_SIG_IS)                                                 \
    M(T, Temporary, errno_m_temporary, ERRNO_SIG_BOOL)                                 \
    M(T, Timeout, errno_m_timeout, ERRNO_SIG_BOOL)

BURROW_METHODS_DEFINE(SyscallErrno, ERRNO_METHODS);

static const Type errno_desc = {
    {(const Byte *)"Errno", 5},
    {(const Byte *)"syscall", 7},
    KIND_UINTPTR,
    (uint32_t)sizeof(SyscallErrno),
    (uint16_t)_Alignof(SyscallErrno),
    0,
    (uint16_t)(sizeof burrow__methods_SyscallErrno /
               sizeof burrow__methods_SyscallErrno[0]),
    NULL,
    burrow__methods_SyscallErrno,
    NULL,
    NULL,
    0,
    0x73797365U, /* "syse" */
    NULL,
};

const Type *const TYPE_SYSCALL_ERRNO = &errno_desc;

/* ------------------------------------------------------------- the error */

/* The Errno first, so errors_as hands back a pointer to it, and the text
 * after, built once because the message slot cannot allocate. */
typedef struct ErrnoBox {
    SyscallErrno e;
    Str message;
} ErrnoBox;

static Str errno_message(const void *self) {
    return ((const ErrnoBox *)self)->message;
}

static bool errno_is_slot(const void *self, Error target) {
    return syscall_errno_is(((const ErrnoBox *)self)->e, target);
}

static Error errno_clone(const void *self, Alloc *a) {
    return syscall_errno_as_error(((const ErrnoBox *)self)->e, a);
}

static const ErrorVT errno_vt = {
    &errno_desc, errno_message, NULL, NULL, errno_is_slot, NULL, errno_clone,
};

Error syscall_errno_as_error(SyscallErrno e, Alloc *a) {
    char buf[ERRNO_TEXT_MAX];
    Str t = errno_text(e, buf);
    ErrnoBox *b = (ErrnoBox *)mem_alloc_nozero(a, sizeof(ErrnoBox) + (size_t)t.len,
                                               _Alignof(ErrnoBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (t.len > 0)
        memcpy(p, t.p, (size_t)t.len);
    b->e = e;
    b->message = str_from_bytes(p, t.len);
    return (Error){&errno_vt, b};
}
