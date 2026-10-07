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

/* Go's errnoErr: e as an Error from error_allocator. 0 is nil on Unix, and
 * EINVAL on Windows, where a call can fail without saying why. */
Error burrow__syscall_errno_err(SyscallErrno e);

#if defined(BURROW_OS_WINDOWS)
/* A string as UTF-16 with a 0 after it, for the length of one call, the way
 * burrow__SyscallCString holds a C string: in buf when it fits and on the
 * heap when it does not, EINVAL if s has a NUL in it. */
typedef struct burrow__SyscallWString {
    uint16_t *heap;
    size_t size;
    uint16_t buf[256];
} burrow__SyscallWString;

uint16_t *burrow__syscall_wstring(burrow__SyscallWString *h, Str s, Error *err);
void burrow__syscall_wstring_free(burrow__SyscallWString *h);

/* SyscallN with the arguments as an array, n of them, which args may be NULL
 * for when n is 0. */
Uintptr burrow__syscall_n(Uintptr fn, const Uintptr *args, Int n, Uintptr *r2,
                          SyscallErrno *err);
#endif

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
/* Go's rawSyscallNoError, for a call that cannot fail. */
Uintptr burrow__syscall_raw_syscall_no_error(Uintptr trap, Uintptr a1, Uintptr a2,
                                             Uintptr a3, Uintptr *r2);
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
/* How a libSystem function says it failed: -1 as a C int, -1 as a long, or a
 * NULL pointer. */
typedef enum burrow__SyscallFail {
    BURROW__SYSCALL_FAIL_INT,
    BURROW__SYSCALL_FAIL_LONG,
    BURROW__SYSCALL_FAIL_PTR
} burrow__SyscallFail;

/* Calls the libSystem function name with n arguments, the first nfixed of them
 * before the ..., or all of them when nfixed is -1, looking it up the first
 * time and keeping it in *slot. The functions Go writes its own trampolines
 * for, fdopendir and the rest, go through this. */
Uintptr burrow__syscall_libc_call(void **slot, const char *name, int32_t nfixed,
                                  const uintptr_t *args, int32_t n,
                                  burrow__SyscallFail fail, Uintptr *r2,
                                  SyscallErrno *err);

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

/* The socket calls Go writes by hand on Linux for 32-bit x86 and s390x, through
 * socketcall(2), in socketcall.c. The other architectures have them generated
 * in zsyscall.h, with the same prototypes. */
#if (defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) ||                           \
     defined(BURROW_OS_WASI)) &&                                                       \
    (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_S390X))
Int burrow__syscall_accept4(Int s, SyscallRawSockaddrAny *rsa, uint32_t *addrlen,
                            Int flags, Error *err);
BURROW_OWNS(ret) Error burrow__syscall_getsockname(Int fd, SyscallRawSockaddrAny *rsa,
                                                   uint32_t *addrlen);
BURROW_OWNS(ret) Error burrow__syscall_getpeername(Int fd, SyscallRawSockaddrAny *rsa,
                                                   uint32_t *addrlen);
BURROW_OWNS(ret) Error burrow__syscall_socketpair(Int domain, Int typ, Int proto,
                                                  int32_t *fd);
BURROW_OWNS(ret) Error burrow__syscall_bind(Int s, void *addr, uint32_t addrlen);
BURROW_OWNS(ret) Error burrow__syscall_connect(Int s, void *addr, uint32_t addrlen);
Int burrow__syscall_socket(Int domain, Int typ, Int proto, Error *err);
BURROW_OWNS(ret) Error burrow__syscall_getsockopt(Int s, Int level, Int name, void *val,
                                                  uint32_t *vallen);
BURROW_OWNS(ret) Error burrow__syscall_setsockopt(Int s, Int level, Int name, void *val,
                                                  Uintptr vallen);
Int burrow__syscall_recvfrom(Int fd, Slice p, Int flags, SyscallRawSockaddrAny *from,
                             uint32_t *fromlen, Error *err);
BURROW_OWNS(ret) Error burrow__syscall_sendto(Int s, Slice buf, Int flags, void *to,
                                              uint32_t addrlen);
Int burrow__syscall_recvmsg(Int s, SyscallMsghdr *msg, Int flags, Error *err);
Int burrow__syscall_sendmsg(Int s, SyscallMsghdr *msg, Int flags, Error *err);
#endif

/* The unexported functions of Go's zsyscall files. */
#include "zsyscall.h"

#endif /* BURROW_SRC_SYSCALL_INTERNAL_H */
