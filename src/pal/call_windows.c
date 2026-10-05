/* The raw calls on Windows, for the syscall package: a C function by address,
 * which is what Go's SyscallN is, LoadLibraryExW and GetProcAddress, which are
 * how syscall's DLL finds one, and no system call by number, which Windows
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

#define FN_CALL(params, ...)                                                           \
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
#define A19 A18, a[18]
#define A20 A19, a[19]
#define A21 A20, a[20]
#define A22 A21, a[21]
#define A23 A22, a[22]
#define A24 A23, a[23]
#define A25 A24, a[24]
#define A26 A25, a[25]
#define A27 A26, a[26]
#define A28 A27, a[27]
#define A29 A28, a[28]
#define A30 A29, a[29]
#define A31 A30, a[30]
#define A32 A31, a[31]
#define A33 A32, a[32]
#define A34 A33, a[33]
#define A35 A34, a[34]
#define A36 A35, a[35]
#define A37 A36, a[36]
#define A38 A37, a[37]
#define A39 A38, a[38]
#define A40 A39, a[39]
#define A41 A40, a[40]
#define A42 A41, a[41]

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
#define P19 P18, U
#define P20 P19, U
#define P21 P20, U
#define P22 P21, U
#define P23 P22, U
#define P24 P23, U
#define P25 P24, U
#define P26 P25, U
#define P27 P26, U
#define P28 P27, U
#define P29 P28, U
#define P30 P29, U
#define P31 P30, U
#define P32 P31, U
#define P33 P32, U
#define P34 P33, U
#define P35 P34, U
#define P36 P35, U
#define P37 P36, U
#define P38 P37, U
#define P39 P38, U
#define P40 P39, U
#define P41 P40, U
#define P42 P41, U

uintptr_t pal_call(void *fn, const uintptr_t *args, int32_t n, int32_t nfixed,
                   uintptr_t *errnum) {
    (void)nfixed; /* Windows passes a variadic argument like any other. */
    U a[42] = {0};
    if (n < 0 || n > 42 || fn == NULL) {
        BURROW_OUT(errnum, (uintptr_t)ERROR_INVALID_PARAMETER);
        return ~(uintptr_t)0;
    }
    for (int32_t i = 0; i < n; i++)
        a[i] = args[i];
    U r = 0;
    SetLastError(0);
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
    case 18:
        FN_CALL((P18), A18);
        break;
    case 19:
        FN_CALL((P19), A19);
        break;
    case 20:
        FN_CALL((P20), A20);
        break;
    case 21:
        FN_CALL((P21), A21);
        break;
    case 22:
        FN_CALL((P22), A22);
        break;
    case 23:
        FN_CALL((P23), A23);
        break;
    case 24:
        FN_CALL((P24), A24);
        break;
    case 25:
        FN_CALL((P25), A25);
        break;
    case 26:
        FN_CALL((P26), A26);
        break;
    case 27:
        FN_CALL((P27), A27);
        break;
    case 28:
        FN_CALL((P28), A28);
        break;
    case 29:
        FN_CALL((P29), A29);
        break;
    case 30:
        FN_CALL((P30), A30);
        break;
    case 31:
        FN_CALL((P31), A31);
        break;
    case 32:
        FN_CALL((P32), A32);
        break;
    case 33:
        FN_CALL((P33), A33);
        break;
    case 34:
        FN_CALL((P34), A34);
        break;
    case 35:
        FN_CALL((P35), A35);
        break;
    case 36:
        FN_CALL((P36), A36);
        break;
    case 37:
        FN_CALL((P37), A37);
        break;
    case 38:
        FN_CALL((P38), A38);
        break;
    case 39:
        FN_CALL((P39), A39);
        break;
    case 40:
        FN_CALL((P40), A40);
        break;
    case 41:
        FN_CALL((P41), A41);
        break;
    default:
        FN_CALL((P42), A42);
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

uintptr_t pal_load_library(const uint16_t *name, bool system, uintptr_t *errnum) {
    HMODULE h =
        LoadLibraryExW((LPCWSTR)name, NULL, system ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
    BURROW_OUT(errnum, h == NULL ? (uintptr_t)GetLastError() : 0);
    return (uintptr_t)h;
}

void *pal_proc_address(uintptr_t module, const char *name, uintptr_t *errnum) {
    union {
        FARPROC f;
        void *object;
    } u;
    u.f = GetProcAddress((HMODULE)module, name);
    BURROW_OUT(errnum, u.f == NULL ? (uintptr_t)GetLastError() : 0);
    return u.object;
}

#endif /* BURROW_OS_WINDOWS */
