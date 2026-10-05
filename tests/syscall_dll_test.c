/* syscall's DLLs on Windows: LoadDLL, LazyDLL, SyscallN and the numbered
 * Syscalls, the UTF-16 conversions, and some of the functions generated from
 * Go's zsyscall_windows.go.
 *
 * Go tests these through os and the rest of the library rather than one by
 * one, so these are burrow's own. They check the answers against what the
 * Windows API says when asked directly.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include <windows.h>

/* The Errno err holds, or 0 if it holds none. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

static bool has_prefix(Str s, const char *p) {
    Str ps = str_from_cstr(p);
    return s.len >= ps.len && memcmp(s.p, ps.p, (size_t)ps.len) == 0;
}

static void TestLoadDLL(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    SyscallDLL *d = syscall_load_dll(a, BURROW_S("kernel32.dll"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "LoadDLL(kernel32.dll): %v", err);
    if (!str_eq(d->name, BURROW_S("kernel32.dll")))
        testing_t_errorf_v(t, "Name = %q", d->name);
    SyscallProc *p = syscall_dll_find_proc(d, a, BURROW_S("GetCurrentProcessId"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "FindProc(GetCurrentProcessId): %v", err);
    Uintptr pid = syscall_proc_call(p, (Slice){NULL, 0, 0, TYPE_UINTPTR}, NULL, &err);
    if (pid != (Uintptr)GetCurrentProcessId())
        testing_t_errorf_v(t, "GetCurrentProcessId() = %d, want %d", (Int)pid,
                           (Int)GetCurrentProcessId());
    /* Proc.Call's error is never nil, as in Go. */
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Call's error is nil");

    p = syscall_dll_find_proc(d, a, BURROW_S("NoSuchProcBurrow"), &err);
    const SyscallDLLError *de = errors_as(err, TYPE_SYSCALL_DLL_ERROR);
    if (p != NULL || de == NULL)
        testing_t_fatalf_v(t, "FindProc(NoSuchProcBurrow) = %v, want a DLLError", err);
    if (!has_prefix(de->msg,
                    "Failed to find NoSuchProcBurrow procedure in kernel32.dll: "))
        testing_t_errorf_v(t, "message = %q", de->msg);
    if (!str_eq(de->obj_name, BURROW_S("NoSuchProcBurrow")))
        testing_t_errorf_v(t, "ObjName = %q", de->obj_name);
    if (errno_of(err) != ERROR_PROC_NOT_FOUND)
        testing_t_errorf_v(t, "Errno = %d, want ERROR_PROC_NOT_FOUND",
                           (Int)errno_of(err));

    err = syscall_dll_release(d);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Release: %v", err);

    d = syscall_load_dll(a, BURROW_S("no-such-dll-burrow.dll"), &err);
    de = errors_as(err, TYPE_SYSCALL_DLL_ERROR);
    if (d != NULL || de == NULL)
        testing_t_fatalf_v(t, "LoadDLL(no-such-dll-burrow.dll) = %v, want a DLLError",
                           err);
    if (!has_prefix(error_text(err), "Failed to load no-such-dll-burrow.dll: "))
        testing_t_errorf_v(t, "message = %q", error_text(err));
    if (errno_of(err) != ERROR_MOD_NOT_FOUND)
        testing_t_errorf_v(t, "Errno = %d, want ERROR_MOD_NOT_FOUND",
                           (Int)errno_of(err));

    static const Byte nul[] = {'k', 0};
    d = syscall_load_dll(a, str_from_bytes(nul, 2), &err);
    if (d != NULL || errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "LoadDLL with a NUL = %v, want EINVAL", err);
    arena_free(&ar);
}

static void TestLazyDLL(TestingT *t) {
    static SyscallLazyDLL kernel32 = {.name = {(const Byte *)"kernel32.dll", 12}};
    static SyscallLazyProc tid = {.name = {(const Byte *)"GetCurrentThreadId", 18},
                                  .l = &kernel32};
    Error err = syscall_lazy_proc_find(&tid);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Find: %v", err);
    Uintptr r =
        syscall_lazy_proc_call(&tid, (Slice){NULL, 0, 0, TYPE_UINTPTR}, NULL, &err);
    if (r != (Uintptr)GetCurrentThreadId())
        testing_t_errorf_v(t, "GetCurrentThreadId() = %d, want %d", (Int)r,
                           (Int)GetCurrentThreadId());
    if (syscall_lazy_dll_handle(&kernel32) !=
        (Uintptr)GetModuleHandleW(L"kernel32.dll"))
        testing_t_errorf_v(t, "Handle() is not kernel32's");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SyscallLazyDLL *d = syscall_new_lazy_dll(a, BURROW_S("no-such-dll-burrow.dll"));
    err = syscall_lazy_dll_load(d);
    if (errors_as(err, TYPE_SYSCALL_DLL_ERROR) == NULL)
        testing_t_errorf_v(t, "Load(no-such-dll-burrow.dll) = %v, want a DLLError",
                           err);
    SyscallLazyProc *p = syscall_lazy_dll_new_proc(d, a, BURROW_S("Nothing"));
    volatile bool panicked = false;
    BURROW_TRY {
        syscall_lazy_proc_addr(p);
    }
    BURROW_CATCH(e) {
        (void)e;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "Addr of a procedure in a missing DLL did not panic");
    arena_free(&ar);
}

/* A function with fifteen arguments, to call by address. */
static uintptr_t WINAPI sum15(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                              uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                              uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
                              uintptr_t a13, uintptr_t a14, uintptr_t a15) {
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 +
           10 * a10 + 11 * a11 + 12 * a12 + 13 * a13 + 14 * a14 + 15 * a15;
}

static Uintptr addr_of(uintptr_t(WINAPI *f)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                            uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                            uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                            uintptr_t, uintptr_t, uintptr_t)) {
    union {
        uintptr_t(WINAPI *f)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
        void *object;
    } u;
    u.f = f;
    return (Uintptr)u.object;
}

static void TestSyscallN(TestingT *t) {
    Uintptr fn = addr_of(sum15);
    Uintptr args[43];
    Uintptr want = 0;
    for (int i = 0; i < 43; i++)
        args[i] = (Uintptr)(i + 1);
    for (int i = 0; i < 15; i++)
        want += (Uintptr)((i + 1) * (i + 1));
    Uintptr r = syscall_syscall_n(fn, (Slice){args, 15, 15, TYPE_UINTPTR}, NULL, NULL);
    if (r != want)
        testing_t_errorf_v(t, "SyscallN(sum15) = %d, want %d", (Int)r, (Int)want);
    r = syscall_syscall15(fn, 15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                          NULL, NULL);
    if (r != want)
        testing_t_errorf_v(t, "Syscall15(sum15) = %d, want %d", (Int)r, (Int)want);

    volatile int panics = 0;
    BURROW_TRY {
        syscall_syscall_n(fn, (Slice){args, 43, 43, TYPE_UINTPTR}, NULL, NULL);
    }
    BURROW_CATCH(e) {
        (void)e;
        panics++;
    }
    BURROW_TRY_END;
    BURROW_TRY {
        syscall_syscall(fn, 4, 1, 2, 3, NULL, NULL);
    }
    BURROW_CATCH(e) {
        (void)e;
        panics++;
    }
    BURROW_TRY_END;
    if (panics != 2)
        testing_t_errorf_v(t, "%d of the 2 calls with too many arguments panicked",
                           (Int)panics);
}

static void TestUTF16(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    /* h, e with an acute accent, a CJK character, an emoji that takes a
     * surrogate pair, and a lone surrogate written in WTF-8. */
    static const Byte in[] = "h\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xED\xA0\x80";
    Str s = str_from_bytes(in, (Int)sizeof in - 1);
    Slice u = syscall_utf16_from_string(a, s, &err);
    static const uint16_t want[] = {'h', 0xE9, 0x4E2D, 0xD83D, 0xDE00, 0xD800, 0};
    if (BURROW_FAILED(err) || u.len != 7 || memcmp(u.p, want, sizeof want) != 0)
        testing_t_errorf_v(t, "UTF16FromString = %d units, %v", u.len, err);
    Str back = syscall_utf16_to_string(a, u);
    if (!str_eq(back, s))
        testing_t_errorf_v(t, "UTF16ToString = %q, want %q", back, s);

    static const uint16_t stop[] = {'a', 'b', 0, 'c'};
    back =
        syscall_utf16_to_string(a, (Slice){(void *)(uintptr_t)stop, 4, 4, TYPE_UINT16});
    if (!str_eq(back, BURROW_S("ab")))
        testing_t_errorf_v(t, "UTF16ToString stops at 0: %q", back);

    static const Byte nul[] = {'a', 0, 'b'};
    uint16_t *p = syscall_utf16_ptr_from_string(a, str_from_bytes(nul, 3), &err);
    if (p != NULL || errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "UTF16PtrFromString with a NUL = %v, want EINVAL", err);
    volatile bool panicked = false;
    BURROW_TRY {
        syscall_string_to_utf16(a, str_from_bytes(nul, 3));
    }
    BURROW_CATCH(e) {
        (void)e;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "StringToUTF16 with a NUL did not panic");
    arena_free(&ar);
}

/* Some of the functions generated from zsyscall_windows.go. */
static void TestGenerated(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    SyscallHandle h = syscall_get_current_process(&err);
    if (BURROW_FAILED(err) || h != (SyscallHandle)(uintptr_t)GetCurrentProcess())
        testing_t_errorf_v(t, "GetCurrentProcess = %d, %v", (Int)h, err);

    h = syscall_load_library(BURROW_S("kernel32.dll"), &err);
    if (BURROW_FAILED(err) || h == 0)
        testing_t_fatalf_v(t, "LoadLibrary(kernel32.dll): %v", err);
    Uintptr f = syscall_get_proc_address(h, BURROW_S("GetCurrentProcessId"), &err);
    if (BURROW_FAILED(err) ||
        f != (Uintptr)(uintptr_t)GetProcAddress((HMODULE)h, "GetCurrentProcessId"))
        testing_t_errorf_v(t, "GetProcAddress: %v", err);
    f = syscall_get_proc_address(h, BURROW_S("NoSuchProcBurrow"), &err);
    if (f != 0 || errno_of(err) != ERROR_PROC_NOT_FOUND)
        testing_t_errorf_v(t, "GetProcAddress(missing) = %v, want ERROR_PROC_NOT_FOUND",
                           err);
    err = syscall_free_library(h);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "FreeLibrary: %v", err);

    uint16_t *name =
        syscall_utf16_ptr_from_string(a, BURROW_S("C:\\no-such-burrow-file"), &err);
    uint32_t attrs = syscall_get_file_attributes(name, &err);
    if (errno_of(err) != ERROR_FILE_NOT_FOUND)
        testing_t_errorf_v(
            t, "GetFileAttributes(missing) = %v, want ERROR_FILE_NOT_FOUND", err);
    if (attrs != INVALID_FILE_ATTRIBUTES)
        testing_t_errorf_v(t, "GetFileAttributes(missing) = %d", (Int)attrs);

    err = syscall_close_handle((SyscallHandle)0x123450);
    if (errno_of(err) != ERROR_INVALID_HANDLE)
        testing_t_errorf_v(t, "CloseHandle(bad) = %v, want ERROR_INVALID_HANDLE", err);

    /* An environment variable, through GetEnvironmentVariableW. */
    SetEnvironmentVariableW(L"BURROW_DLL_TEST", L"yes");
    uint16_t buf[16];
    uint16_t *key = syscall_utf16_ptr_from_string(a, BURROW_S("BURROW_DLL_TEST"), &err);
    uint32_t n = syscall_get_environment_variable(key, buf, 16, &err);
    Str v = syscall_utf16_to_string(a, (Slice){buf, n, 16, TYPE_UINT16});
    if (BURROW_FAILED(err) || !str_eq(v, BURROW_S("yes")))
        testing_t_errorf_v(t, "GetEnvironmentVariable = %q, %v", v, err);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLoadDLL)                                                                     \
    X(TestLazyDLL)                                                                     \
    X(TestSyscallN)                                                                    \
    X(TestUTF16)                                                                       \
    X(TestGenerated)

#else

static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "the DLL functions are Windows only");
}

#define TESTS(X) X(TestNothing)

#endif

TESTING_MAIN(TESTS)
