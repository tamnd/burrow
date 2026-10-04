/* What the generated functions in zsyscall.c stand on: syscall.Syscall and the
 * other raw calls, BytePtrFromString, and on macOS the calls into libSystem.
 *
 * Linux and FreeBSD make a system call by number, through pal_syscall. macOS
 * makes it through libSystem, as Go does, since Apple does not promise its
 * numbers: each function zsyscall.c reaches is a number in the table it has,
 * looked up by name the first time and kept in burrow__syscall_libc_cache.
 *
 * Derived from Go's src/syscall/syscall_linux.go, syscall_darwin.go,
 * syscall_unix.go and syscall.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "burrow/atomic.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include "internal.h"

#include <string.h>

/* What &_zero is in Go: somewhere to point for a slice with nothing in it. */
Uintptr burrow__syscall_zero;

/* ------------------------------------------------------------- C strings */

/* Go's ByteSliceFromString: EINVAL when s has a NUL in it. */
static bool has_nul(Str s) {
    return s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL;
}

uint8_t *syscall_byte_ptr_from_string(Alloc *a, Str s, Error *err) {
    if (has_nul(s)) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return NULL;
    }
    uint8_t *p = mem_alloc(a, (size_t)s.len + 1, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

Slice syscall_byte_slice_from_string(Alloc *a, Str s, Error *err) {
    if (has_nul(s)) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return slice_make(a, TYPE_BYTE, 0, 0);
    }
    Slice b = slice_make(a, TYPE_BYTE, s.len + 1, s.len + 1);
    if (b.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return b;
    }
    if (s.len > 0)
        memcpy(b.p, s.p, (size_t)s.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

Slice syscall_string_byte_slice(Alloc *a, Str s) {
    Error err = BURROW_NO_ERROR;
    Slice b = syscall_byte_slice_from_string(a, s, &err);
    if (BURROW_FAILED(err) && !errors_is(err, burrow_err_out_of_memory))
        panic_str(BURROW_S("syscall: string with NUL passed to StringByteSlice"));
    return b;
}

uint8_t *syscall_string_byte_ptr(Alloc *a, Str s) {
    return (uint8_t *)syscall_string_byte_slice(a, s).p;
}

/* ------------------------------------------------------------ the process */

Int syscall_getpagesize(void) {
    return (Int)pal_page_size();
}

void syscall_exit(Int code) {
    pal_exit((int32_t)code);
}

uint8_t *burrow__syscall_cstring(burrow__SyscallCString *h, Str s, Error *err) {
    h->heap = NULL;
    if (has_nul(s)) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return NULL;
    }
    uint8_t *p = h->buf;
    if ((size_t)s.len >= sizeof h->buf) {
        h->size = (size_t)s.len + 1;
        h->heap = mem_alloc(heap_allocator(), h->size, 1);
        if (h->heap == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        p = h->heap;
    }
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

void burrow__syscall_cstring_free(burrow__SyscallCString *h) {
    if (h->heap != NULL)
        mem_free(heap_allocator(), h->heap, h->size, 1);
    h->heap = NULL;
}

#if !defined(BURROW_OS_WINDOWS)

/* Go's errnoErr on Unix, which is nil for 0. */
Error burrow__syscall_errno_err(SyscallErrno e) {
    if (e == 0)
        return BURROW_NO_ERROR;
    return syscall_errno_as_error(e, error_allocator());
}

/* ------------------------------------------------------------- by number */

/* A system call by number. r2 is always 0: syscall(2) has nowhere to put the
 * second register, which only Linux's tee and pipe on a few 32-bit machines
 * ever set. */
static Uintptr by_number(Uintptr trap, const uintptr_t *args, int32_t n, Uintptr *r2,
                         SyscallErrno *err) {
    uintptr_t e = 0;
    Uintptr r = (Uintptr)pal_syscall((uintptr_t)trap, args, n, &e);
    BURROW_OUT(r2, 0);
    BURROW_OUT(err, (SyscallErrno)e);
    return r;
}

Uintptr syscall_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr *r2,
                        SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return by_number(trap, args, 3, r2, err);
}

Uintptr syscall_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr a4,
                         Uintptr a5, Uintptr a6, Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6};
    return by_number(trap, args, 6, r2, err);
}

Uintptr syscall_raw_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                            Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return by_number(trap, args, 3, r2, err);
}

Uintptr syscall_raw_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                             Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                             SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6};
    return by_number(trap, args, 6, r2, err);
}

#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_COSMO) && !defined(BURROW_OS_WASI)
Uintptr syscall_syscall9(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr a4,
                         Uintptr a5, Uintptr a6, Uintptr a7, Uintptr a8, Uintptr a9,
                         Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    return by_number(trap, args, 9, r2, err);
}
#else
/* Go's rawSyscallNoError, for the calls that cannot fail, such as getpid. */
Uintptr burrow__syscall_raw_syscall_no_error(Uintptr trap, Uintptr a1, Uintptr a2,
                                             Uintptr a3, Uintptr *r2) {
    const uintptr_t args[] = {a1, a2, a3};
    return by_number(trap, args, 3, r2, NULL);
}
#endif

/* ------------------------------------------------------------ libSystem */

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

void *burrow__syscall_libc_cache[BURROW__SYSCALL_NLIBC];

Uintptr burrow__syscall_libc_call(void **slot, const char *name, int32_t nfixed,
                                  const uintptr_t *args, int32_t n,
                                  burrow__SyscallFail fail, Uintptr *r2,
                                  SyscallErrno *err) {
    BURROW_OUT(r2, 0);
    void *f = burrow__atomic_load_acquire_ptr(slot);
    if (f == NULL) {
        f = pal_libc_symbol(name);
        if (f == NULL) {
            BURROW_OUT(err, SYSCALL_ENOSYS);
            return ~(Uintptr)0;
        }
        burrow__atomic_store_release_ptr(slot, f);
    }
    uintptr_t e = 0;
    Uintptr r = (Uintptr)pal_call(f, args, n, nfixed, &e);
    bool failed = fail == BURROW__SYSCALL_FAIL_INT    ? (int32_t)r == -1
                  : fail == BURROW__SYSCALL_FAIL_LONG ? r == ~(Uintptr)0
                                                      : r == 0;
    BURROW_OUT(err, failed ? (SyscallErrno)e : 0);
    return r;
}

typedef burrow__SyscallFail Fail;
#define FAIL_INT BURROW__SYSCALL_FAIL_INT
#define FAIL_LONG BURROW__SYSCALL_FAIL_LONG
#define FAIL_PTR BURROW__SYSCALL_FAIL_PTR

static Uintptr libc(Uintptr fn, const uintptr_t *args, int32_t n, Fail fail,
                    Uintptr *r2, SyscallErrno *err) {
    if (fn >= BURROW__SYSCALL_NLIBC) {
        BURROW_OUT(r2, 0);
        BURROW_OUT(err, SYSCALL_ENOSYS);
        return ~(Uintptr)0;
    }
    return burrow__syscall_libc_call(
        &burrow__syscall_libc_cache[fn], burrow__syscall_libc[fn].name,
        burrow__syscall_libc[fn].nfixed, args, n, fail, r2, err);
}

Uintptr burrow__syscall_syscall(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return libc(fn, args, 3, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_syscall_x(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                  Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return libc(fn, args, 3, FAIL_LONG, r2, err);
}

Uintptr burrow__syscall_syscall6(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                 Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                 SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6};
    return libc(fn, args, 6, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_syscall6_x(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                   Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                   SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6};
    return libc(fn, args, 6, FAIL_LONG, r2, err);
}

Uintptr burrow__syscall_syscall9(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                 Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                                 Uintptr a8, Uintptr a9, Uintptr *r2,
                                 SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    return libc(fn, args, 9, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_raw_syscall(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return libc(fn, args, 3, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_raw_syscall6(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                     SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6};
    return libc(fn, args, 6, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_raw_syscall9(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                                     Uintptr a8, Uintptr a9, Uintptr *r2,
                                     SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    return libc(fn, args, 9, FAIL_INT, r2, err);
}

Uintptr burrow__syscall_syscall_ptr(Uintptr fn, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err) {
    const uintptr_t args[] = {a1, a2, a3};
    return libc(fn, args, 3, FAIL_PTR, r2, err);
}

#endif /* BURROW_OS_DARWIN || BURROW_OS_IOS */

#endif /* !BURROW_OS_WINDOWS */
