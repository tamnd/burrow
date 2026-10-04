/* The raw calls on Windows, for the syscall package: a C function by address,
 * which is what Go's SyscallN is, and no system call by number, which Windows
 * does not have.
 *
 * The Windows API is __stdcall on 386 and the one calling convention there is
 * on amd64 and arm64, so a pointer typed with uintptr parameters calls any of
 * it, the way Go's does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

typedef uintptr_t U;

#define CALL(params, ...)                                                              \
    do {                                                                               \
        union {                                                                        \
            void *object;                                                              \
            U(WINAPI *f) params;                                                       \
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

uintptr_t pal_call(void *fn, const uintptr_t *args, int32_t n, int32_t nfixed,
                   uintptr_t *errnum) {
    (void)nfixed; /* Windows passes a variadic argument like any other. */
    U a[18] = {0};
    if (n < 0 || n > 18 || fn == NULL) {
        BURROW_OUT(errnum, (uintptr_t)ERROR_INVALID_PARAMETER);
        return ~(uintptr_t)0;
    }
    for (int32_t i = 0; i < n; i++)
        a[i] = args[i];
    U r = 0;
    SetLastError(0);
    switch (n) {
    case 0:
        CALL((void), A0);
        break;
    case 1:
        CALL((P1), A1);
        break;
    case 2:
        CALL((P2), A2);
        break;
    case 3:
        CALL((P3), A3);
        break;
    case 4:
        CALL((P4), A4);
        break;
    case 5:
        CALL((P5), A5);
        break;
    case 6:
        CALL((P6), A6);
        break;
    case 7:
        CALL((P7), A7);
        break;
    case 8:
        CALL((P8), A8);
        break;
    case 9:
        CALL((P9), A9);
        break;
    case 10:
        CALL((P10), A10);
        break;
    case 11:
        CALL((P11), A11);
        break;
    case 12:
        CALL((P12), A12);
        break;
    case 13:
        CALL((P13), A13);
        break;
    case 14:
        CALL((P14), A14);
        break;
    case 15:
        CALL((P15), A15);
        break;
    case 16:
        CALL((P16), A16);
        break;
    case 17:
        CALL((P17), A17);
        break;
    default:
        CALL((P18), A18);
        break;
    }
    BURROW_OUT(errnum, (uintptr_t)GetLastError());
    return r;
}

uintptr_t pal_syscall(uintptr_t trap, const uintptr_t *args, int32_t n,
                      uintptr_t *errnum) {
    (void)trap;
    (void)args;
    (void)n;
    BURROW_OUT(errnum, (uintptr_t)ERROR_NOT_SUPPORTED);
    return ~(uintptr_t)0;
}

void *pal_libc_symbol(const char *name) {
    (void)name;
    return NULL;
}

#endif /* BURROW_OS_WINDOWS */
