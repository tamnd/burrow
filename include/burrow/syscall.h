/* syscall's Errno, the error number a system call failed with.
 *
 * Go's syscall package is mostly the raw system calls, and burrow keeps those
 * behind the PAL. What the rest of the library needs from it is Errno, because
 * os wraps one in every PathError and code tests for it by value:
 *
 *     const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
 *     if (e != NULL && *e == SYSCALL_ENOENT)
 *
 * The SYSCALL_E constants are the system's own numbers, generated from Go's
 * tables, so SYSCALL_EAGAIN is 11 on Linux and 35 on macOS, and the text of
 * an Errno is the text Go gives on that system. On Windows the constants also
 * include the ERROR_ and WSA codes Go names, and an Errno's text comes from
 * FormatMessage, as it does in Go. The E constants Windows has no code for are
 * numbers Go made up, from APPLICATION_ERROR on, with the Unix text.
 *
 * An Errno is a number. syscall_errno_as_error boxes it into an Error, whose
 * errors_is matches the io/fs errors the way Go's Errno.Is does, and whose
 * type has the Timeout and Temporary methods, so a PathError around it answers
 * fs_path_error_timeout too.
 *
 * Derived from Go's src/syscall/syscall_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package syscall */

#ifndef BURROW_SYSCALL_H
#define BURROW_SYSCALL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* syscall.Errno. 0 means no error. */
typedef Uintptr SyscallErrno;

#include "burrow/syscall/zerrors.h"

/* The descriptor of Errno, for errors_as. What errors_as hands back points at
 * a SyscallErrno. */
extern const Type *const TYPE_SYSCALL_ERRNO;

/* Errno.Error: the text for e, built in a. On Unix it is Go's table entry, or
 * "errno " and the number for one the table does not have. On Windows it is
 * the table entry for the numbers Go made up, then what FormatMessage says,
 * and "winapi error #" and the number when that fails. A failed allocation
 * gives the empty string. */
BURROW_OWNS(ret) Str syscall_errno_error(SyscallErrno e, Alloc *a);

/* e as an Error, in one allocation from a. Its text is syscall_errno_error's.
 * A failed allocation gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error syscall_errno_as_error(SyscallErrno e, Alloc *a);

/* Errno.Is: whether e counts as target, which is one of fs_err_permission,
 * fs_err_exist, fs_err_not_exist or errors_err_unsupported. EACCES and EPERM
 * are permission errors, EEXIST and ENOTEMPTY mean it exists, ENOENT means
 * it does not, and ENOSYS, ENOTSUP and EOPNOTSUPP are unsupported. Windows
 * adds its own ERROR_ codes to each. */
bool syscall_errno_is(SyscallErrno e, Error target);

/* Errno.Timeout: EAGAIN, EWOULDBLOCK and ETIMEDOUT. */
bool syscall_errno_timeout(SyscallErrno e);

/* Errno.Temporary: EINTR, EMFILE, ENFILE everywhere but Windows, and the
 * timeouts. */
bool syscall_errno_temporary(SyscallErrno e);

/* The Errno for a PAL failure. Straight after the PAL call that failed with e
 * it is the system's own code, which on Windows keeps the ERROR_ code Go would
 * show. Later, or for a PalErrno a backend chose itself, it is the E constant
 * for e. PAL_OK, and the PalErrno values with no errno behind them, which are
 * PAL_EHOSTNOTFOUND, PAL_ETRYAGAIN and PAL_EOTHER, give 0. */
SyscallErrno syscall_errno_from_pal(PalErrno e);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYSCALL_H */
