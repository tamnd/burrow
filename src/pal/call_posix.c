/* The raw calls on POSIX: a system call by number and a C function by
 * address, for the syscall package.
 *
 * Linux and Android have syscall(2) in libc. macOS has it too, deprecated, and
 * the BSDs have __syscall, which returns a 64-bit result whole on a 32-bit
 * machine. Those are looked up by name rather than declared, since the headers
 * disagree about whether they are there and what they take. Cosmopolitan runs
 * on systems that number their calls differently, and wasip1 has no calls at
 * all, so both always say ENOSYS.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stdint.h>

#if defined(BURROW_OS_LINUX)
#include <unistd.h>
#elif !defined(BURROW_OS_COSMO) && !defined(BURROW_OS_WASI)
#include <dlfcn.h>
#define HAVE_DLSYM 1
#endif

typedef uintptr_t U;

/* Calling through a function pointer of a type the callee was not compiled
 * with is the point of this file, and clang's -fsanitize=function would stop
 * it at a function burrow built itself, as a test does. */
#if defined(__clang__)
#define NO_SANITIZE_FUNCTION __attribute__((no_sanitize("function")))
#else
#define NO_SANITIZE_FUNCTION
#endif

/* A void * is turned into a function pointer through a union, which is how
 * signal_posix.c says it without a pedantic build arguing. */
#define FN_CALL(params, ...)                                                           \
    do {                                                                               \
        union {                                                                        \
            void *object;                                                              \
            U(*f) params;                                                              \
        } u_;                                                                          \
        u_.object = fn;                                                                \
        r = u_.f(__VA_ARGS__);                                                         \
    } while (0)

_Static_assert(sizeof(void (*)(void)) == sizeof(void *),
               "a function pointer does not fit where this file puts one");

#define A0
#define A1 a[0]
#define A2 A1, a[1]
#define A3 A2, a[2]
#define A4 A3, a[3]
#define A5 A4, a[4]
#define A6 A5, a[5]
#define A7 A6, a[6]
#define A8 A7, a[7]
#define A9 A8, a[8]
#define A10 A9, a[9]
#define A11 A10, a[10]
#define A12 A11, a[11]
#define A13 A12, a[12]
#define A14 A13, a[13]
#define A15 A14, a[14]
#define A16 A15, a[15]
#define A17 A16, a[16]
#define A18 A17, a[17]

#define P1 U
#define P2 P1, U
#define P3 P2, U
#define P4 P3, U
#define P5 P4, U
#define P6 P5, U
#define P7 P6, U
#define P8 P7, U
#define P9 P8, U
#define P10 P9, U
#define P11 P10, U
#define P12 P11, U
#define P13 P12, U
#define P14 P13, U
#define P15 P14, U
#define P16 P15, U
#define P17 P16, U
#define P18 P17, U

/* A variadic function, with nf fixed parameters, gets all nine. The ones after
 * what it reads are never looked at, wherever the convention puts them.
 *
 * Each shape is a function of its own. As three branches of one function, the
 * three calls differ in nothing but their type, and clang at -O2 on arm64 macOS
 * folded them into one call with three fixed parameters. The type is the one
 * thing Apple's arm64 cares about here, since it puts the variadic arguments on
 * the stack, so fcntl and open found their last argument missing. */
#define VCALL(nf)                                                                      \
    NO_SANITIZE_FUNCTION BURROW_NOINLINE static U vcall##nf(void *fn, const U *a) {    \
        U r;                                                                           \
        FN_CALL((P##nf, ...), A9);                                                     \
        return r;                                                                      \
    }

VCALL(1)
VCALL(2)
VCALL(3)

NO_SANITIZE_FUNCTION
uintptr_t pal_call(void *fn, const uintptr_t *args, int32_t n, int32_t nfixed,
                   uintptr_t *errnum) {
    U a[18] = {0};
    if (n < 0 || n > 18 || fn == NULL || (nfixed >= 0 && n > 9)) {
        BURROW_OUT(errnum, (uintptr_t)EINVAL);
        return ~(uintptr_t)0;
    }
    for (int32_t i = 0; i < n; i++)
        a[i] = args[i];
    U r = 0;
    errno = 0;
    if (nfixed >= 1 && nfixed <= 3 && n >= nfixed) {
        if (nfixed == 1)
            r = vcall1(fn, a);
        else if (nfixed == 2)
            r = vcall2(fn, a);
        else
            r = vcall3(fn, a);
    } else {
        switch (n) {
        case 0:
            FN_CALL((void), A0);
            break;
        case 1:
            FN_CALL((P1), A1);
            break;
        case 2:
            FN_CALL((P2), A2);
            break;
        case 3:
            FN_CALL((P3), A3);
            break;
        case 4:
            FN_CALL((P4), A4);
            break;
        case 5:
            FN_CALL((P5), A5);
            break;
        case 6:
            FN_CALL((P6), A6);
            break;
        case 7:
            FN_CALL((P7), A7);
            break;
        case 8:
            FN_CALL((P8), A8);
            break;
        case 9:
            FN_CALL((P9), A9);
            break;
        case 10:
            FN_CALL((P10), A10);
            break;
        case 11:
            FN_CALL((P11), A11);
            break;
        case 12:
            FN_CALL((P12), A12);
            break;
        case 13:
            FN_CALL((P13), A13);
            break;
        case 14:
            FN_CALL((P14), A14);
            break;
        case 15:
            FN_CALL((P15), A15);
            break;
        case 16:
            FN_CALL((P16), A16);
            break;
        case 17:
            FN_CALL((P17), A17);
            break;
        default:
            FN_CALL((P18), A18);
            break;
        }
    }
    BURROW_OUT(errnum, (uintptr_t)errno);
    return r;
}

void *pal_libc_symbol(const char *name) {
#if defined(HAVE_DLSYM) && defined(RTLD_DEFAULT)
    return name == NULL ? NULL : dlsym(RTLD_DEFAULT, name);
#else
    (void)name;
    return NULL;
#endif
}

/* Windows's, which syscall only declares there. */
uintptr_t pal_load_library(const uint16_t *name, bool system, uintptr_t *errnum) {
    (void)name;
    (void)system;
    BURROW_OUT(errnum, (uintptr_t)ENOSYS);
    return 0;
}

void *pal_proc_address(uintptr_t module, const char *name, uintptr_t *errnum) {
    (void)module;
    (void)name;
    BURROW_OUT(errnum, (uintptr_t)ENOSYS);
    return NULL;
}

NO_SANITIZE_FUNCTION
uintptr_t pal_syscall(uintptr_t trap, const uintptr_t *args, int32_t n,
                      uintptr_t *errnum) {
    U a[9] = {0};
    if (n < 0 || n > 9) {
        BURROW_OUT(errnum, (uintptr_t)EINVAL);
        return ~(uintptr_t)0;
    }
    for (int32_t i = 0; i < n; i++)
        a[i] = args[i];
#if defined(BURROW_OS_LINUX)
    /* The arguments go as longs, which is what the kernel takes them as,
     * and the ones the call does not use are never read. */
    errno = 0;
    long r = syscall((long)trap, (long)a[0], (long)a[1], (long)a[2], (long)a[3],
                     (long)a[4], (long)a[5], (long)a[6], (long)a[7], (long)a[8]);
    if (r == -1) {
        BURROW_OUT(errnum, (uintptr_t)errno);
        return ~(uintptr_t)0;
    }
    BURROW_OUT(errnum, 0);
    return (uintptr_t)r;
#elif defined(HAVE_DLSYM)
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY)
    void *fn = pal_libc_symbol("__syscall");
    if (fn != NULL) {
        int64_t r = 0;
        errno = 0;
        {
            union {
                void *object;
                int64_t (*f)(int64_t, ...);
            } u;
            u.object = fn;
            r = u.f((int64_t)trap, A9);
        }
        if (r == -1) {
            BURROW_OUT(errnum, (uintptr_t)errno);
            return ~(uintptr_t)0;
        }
        BURROW_OUT(errnum, 0);
        return (uintptr_t)r;
    }
#endif
    void *sys = pal_libc_symbol("syscall");
    if (sys == NULL) {
        BURROW_OUT(errnum, (uintptr_t)ENOSYS);
        return ~(uintptr_t)0;
    }
    int r = 0;
    errno = 0;
    {
        union {
            void *object;
            int (*f)(int, ...);
        } u;
        u.object = sys;
        r = u.f((int)trap, A9);
    }
    if (r == -1) {
        BURROW_OUT(errnum, (uintptr_t)errno);
        return ~(uintptr_t)0;
    }
    BURROW_OUT(errnum, 0);
    return (uintptr_t)(intptr_t)r;
#else
    (void)trap;
    (void)a;
    BURROW_OUT(errnum, (uintptr_t)ENOSYS);
    return ~(uintptr_t)0;
#endif
}

#endif /* !BURROW_OS_WINDOWS */
