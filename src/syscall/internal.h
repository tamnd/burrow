/* What the syscall sources share and the public header does not show.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_SYSCALL_INTERNAL_H
#define BURROW_SRC_SYSCALL_INTERNAL_H

#include "burrow/syscall.h"

#include "burrow/core.h"

/* Go's errors table for this system, from src/syscall/zerrors.c. Entry i is
 * the text for the Errno burrow__syscall_errors_base + i, and NULL where Go
 * has none. The base is 0 except on Windows, where the table only covers the
 * numbers Go made up, from APPLICATION_ERROR on. */
extern const Uintptr burrow__syscall_errors_base;
extern const char *const burrow__syscall_errors[];
extern const Int burrow__syscall_nerrors;

/* Go's signals table, entry i the name of signal i, and NULL or "" where Go
 * has none. */
extern const char *const burrow__syscall_signals[];
extern const Int burrow__syscall_nsignals;

/* Go's &_zero, which a slice with nothing in it points at. */
extern Uintptr burrow__syscall_zero;

/* A string as a C string, for the length of one call: in buf when it fits and
 * on the heap when it does not. burrow__syscall_cstring fails with EINVAL if s
 * has a NUL in it, as BytePtrFromString does, and
 * burrow__syscall_cstring_free gives back what it took, which is nothing if it
 * failed or heap is still NULL. */
typedef struct burrow__SyscallCString {
    uint8_t *heap;
    size_t size;
    uint8_t buf[256];
} burrow__SyscallCString;

uint8_t *burrow__syscall_cstring(burrow__SyscallCString *h, Str s, Error *err);
void burrow__syscall_cstring_free(burrow__SyscallCString *h);

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
/* Go's rawSyscallNoError, for a call that cannot fail. */
Uintptr burrow__syscall_raw_syscall_no_error(Uintptr trap, Uintptr a1, Uintptr a2,
                                             Uintptr a3, Uintptr *r2);
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
/* Go's syscall, syscall6 and the rest on macOS, which call the libSystem
 * function fn, a BURROW__SYSCALL_LIBC_ number from zsyscall.h. They fail when
 * it returns -1 as an int, or for the X ones as a long, or for syscallPtr a
 * NULL pointer, the way Go's do. */
Uintptr burrow__syscall_syscall(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                Uintptr *r2, SyscallErrno *err);
Uintptr burrow__syscall_syscall_x(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                  Uintptr *r2, SyscallErrno *err);
Uintptr burrow__syscall_syscall6(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                 Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                 SyscallErrno *err);
Uintptr burrow__syscall_syscall6_x(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                   Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                   SyscallErrno *err);
Uintptr burrow__syscall_syscall9(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                 Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                                 Uintptr a8, Uintptr a9, Uintptr *r2,
                                 SyscallErrno *err);
Uintptr burrow__syscall_raw_syscall(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err);
Uintptr burrow__syscall_raw_syscall6(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                     SyscallErrno *err);
Uintptr burrow__syscall_raw_syscall9(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                                     Uintptr a8, Uintptr a9, Uintptr *r2,
                                     SyscallErrno *err);
Uintptr burrow__syscall_syscall_ptr(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err);
#endif

/* runtime.GOOS == "ios", which Go's ptrace checks. */
#if defined(BURROW_OS_IOS)
#define BURROW__SYSCALL_IOS 1
#else
#define BURROW__SYSCALL_IOS 0
#endif

/* The unexported functions of Go's zsyscall files. */
#include "zsyscall.h"

#endif /* BURROW_SRC_SYSCALL_INTERNAL_H */
