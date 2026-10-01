/* syscall.Errno.
 *
 * Go has no tests for Errno beyond checking that it is an error, so these are
 * burrow's own. They pin the text, Is, Timeout and Temporary to what Go's
 * syscall_unix.go and syscall_windows.go do, check the constants against the C
 * library's on POSIX, and check that the PAL hands the system's own number
 * through.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS)
#include <errno.h>
#endif

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static bool str_is(Str s, const char *want) {
    return str_eq(s, str_from_bytes((const Byte *)want, (Int)strlen(want)));
}

#if defined(BURROW_OS_WINDOWS)
static bool has_prefix(Str s, const char *prefix) {
    Int n = (Int)strlen(prefix);
    return s.len >= n && memcmp(s.p, prefix, (size_t)n) == 0;
}
#endif

/* The text for the E constants every system has, which Go writes the same on
 * all of them except where noted. */
static void TestErrorText(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        SyscallErrno e;
        const char *want;
    } tests[] = {
        {SYSCALL_ENOENT, "no such file or directory"},
        {SYSCALL_EACCES, "permission denied"},
        {SYSCALL_EEXIST, "file exists"},
        {SYSCALL_EINVAL, "invalid argument"},
        {SYSCALL_EPIPE, "broken pipe"},
        {SYSCALL_ENOTDIR, "not a directory"},
        {SYSCALL_EISDIR, "is a directory"},
        {SYSCALL_EBADF, "bad file descriptor"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
#if defined(BURROW_OS_WINDOWS)
        /* Go makes ENOENT ERROR_FILE_NOT_FOUND and ENOTDIR ERROR_PATH_NOT_FOUND,
         * and those have the system's text. */
        if (tests[i].e < (SyscallErrno)0x20000000)
            continue;
#endif
        Str got = syscall_errno_error(tests[i].e, a);
        if (!str_is(got, tests[i].want))
            testing_t_errorf_v(t, "Errno(%d).Error() = %q, want %q", (Int)tests[i].e,
                               got, tests[i].want);
    }
    ARENA_END;
}

/* A number with no table entry. */
static void TestErrorTextUnknown(TestingT *t) {
    ARENA_BEGIN;
#if defined(BURROW_OS_WINDOWS)
    /* Go asks FormatMessage, which knows ERROR_FILE_NOT_FOUND. The words
     * depend on the system's language, so only the shape is checked: some
     * text, without the line break FormatMessage ends with. */
    Str got = syscall_errno_error(SYSCALL_ERROR_FILE_NOT_FOUND, a);
    if (got.len == 0 || has_prefix(got, "winapi error #") ||
        got.p[got.len - 1] == '\n' || got.p[got.len - 1] == '\r')
        testing_t_errorf_v(t, "Errno(ERROR_FILE_NOT_FOUND).Error() = %q", got);
    /* No message at all, which is what 0x1fffffff gives. */
    got = syscall_errno_error((SyscallErrno)0x1fffffff, a);
    if (!str_is(got, "winapi error #536870911"))
        testing_t_errorf_v(t, "Errno(0x1fffffff).Error() = %q, want %q", got,
                           "winapi error #536870911");
#else
    /* errors[0] is "", which Go passes over. */
    Str got = syscall_errno_error(0, a);
    if (!str_is(got, "errno 0"))
        testing_t_errorf_v(t, "Errno(0).Error() = %q, want %q", got, "errno 0");
    got = syscall_errno_error(9999, a);
    if (!str_is(got, "errno 9999"))
        testing_t_errorf_v(t, "Errno(9999).Error() = %q, want %q", got, "errno 9999");
    /* int(e), so the top half prints negative. */
    got = syscall_errno_error((SyscallErrno)-1, a);
    if (!str_is(got, "errno -1"))
        testing_t_errorf_v(t, "Errno(^uintptr(0)).Error() = %q, want %q", got,
                           "errno -1");
#endif
    ARENA_END;
}

#if defined(BURROW_OS_WINDOWS)
/* The numbers Go made up have Go's text and not the system's. */
static void TestErrorTextInvented(TestingT *t) {
    ARENA_BEGIN;
    Str got = syscall_errno_error(SYSCALL_EWINDOWS, a);
    if (!str_is(got, "not supported by windows"))
        testing_t_errorf_v(t, "EWINDOWS.Error() = %q", got);
    got = syscall_errno_error(SYSCALL_EINVAL, a);
    if (!str_is(got, "invalid argument"))
        testing_t_errorf_v(t, "EINVAL.Error() = %q", got);
    ARENA_END;
}
#endif

static void TestIs(TestingT *t) {
    static const struct {
        SyscallErrno e;
        int target; /* 0 permission, 1 exist, 2 not exist, 3 unsupported */
        bool want;
    } tests[] = {
        {SYSCALL_EACCES, 0, true},
        {SYSCALL_EPERM, 0, true},
        {SYSCALL_ENOENT, 0, false},
        {SYSCALL_EEXIST, 1, true},
        {SYSCALL_ENOTEMPTY, 1, true},
        {SYSCALL_EACCES, 1, false},
        {SYSCALL_ENOENT, 2, true},
        {SYSCALL_EEXIST, 2, false},
        {SYSCALL_ENOSYS, 3, true},
        {SYSCALL_ENOTSUP, 3, true},
        {SYSCALL_EOPNOTSUPP, 3, true},
        {SYSCALL_EINVAL, 3, false},
#if defined(BURROW_OS_WINDOWS)
        {SYSCALL_ERROR_ACCESS_DENIED, 0, true},
        {SYSCALL_ERROR_ALREADY_EXISTS, 1, true},
        {SYSCALL_ERROR_DIR_NOT_EMPTY, 1, true},
        {SYSCALL_ERROR_FILE_EXISTS, 1, true},
        {SYSCALL_ERROR_FILE_NOT_FOUND, 2, true},
        {SYSCALL_ERROR_PATH_NOT_FOUND, 2, true},
        {(SyscallErrno)53, 2, true},
        {(SyscallErrno)50, 3, true},
        {(SyscallErrno)120, 3, true},
        {SYSCALL_EWINDOWS, 3, true},
        {SYSCALL_ERROR_FILE_NOT_FOUND, 0, false},
#endif
    };
    const Error *targets[] = {&fs_err_permission, &fs_err_exist, &fs_err_not_exist,
                              &errors_err_unsupported};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = syscall_errno_is(tests[i].e, *targets[tests[i].target]);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "Errno(%d).Is(target %d) = %v, want %v",
                               (Int)tests[i].e, tests[i].target, got, tests[i].want);
    }
    /* Anything else, including an error with the same text, is no match. */
    CHECK(!syscall_errno_is(SYSCALL_ENOENT, fs_err_invalid));
    CHECK(
        !syscall_errno_is(SYSCALL_ENOENT, errors_new(error_allocator(),
                                                     BURROW_S("file does not exist"))));
}

static void TestTimeoutTemporary(TestingT *t) {
    CHECK(syscall_errno_timeout(SYSCALL_EAGAIN));
    CHECK(syscall_errno_timeout(SYSCALL_EWOULDBLOCK));
    CHECK(syscall_errno_timeout(SYSCALL_ETIMEDOUT));
    CHECK(!syscall_errno_timeout(SYSCALL_EINTR));
    CHECK(syscall_errno_temporary(SYSCALL_EINTR));
    CHECK(syscall_errno_temporary(SYSCALL_EMFILE));
    CHECK(syscall_errno_temporary(SYSCALL_EAGAIN));
    CHECK(!syscall_errno_temporary(SYSCALL_ENOENT));
#if defined(BURROW_OS_WINDOWS)
    /* syscall_windows.go leaves ENFILE out. */
    CHECK(!syscall_errno_temporary(SYSCALL_ENFILE));
#else
    CHECK(syscall_errno_temporary(SYSCALL_ENFILE));
#endif
}

/* The Error: its text, errors_is through the Is slot, errors_as back to the
 * number, and the same from inside a PathError. */
static void TestAsError(TestingT *t) {
    ARENA_BEGIN;
    Error err = syscall_errno_as_error(SYSCALL_EEXIST, a);
    CHECK(str_is(error_text(err), "file exists"));
    CHECK(errors_is(err, fs_err_exist));
    CHECK(!errors_is(err, fs_err_not_exist));
    const SyscallErrno *p = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    CHECK(p != NULL && *p == SYSCALL_EEXIST);

    Error pe = fs_path_error_new(a, BURROW_S("open"), BURROW_S("/nowhere"), err);
    CHECK(str_is(error_text(pe), "open /nowhere: file exists"));
    CHECK(errors_is(pe, fs_err_exist));
    p = (const SyscallErrno *)errors_as(pe, TYPE_SYSCALL_ERRNO);
    CHECK(p != NULL && *p == SYSCALL_EEXIST);

    /* fs_path_error_timeout finds Timeout through the type's methods. */
    const FsPathError *fp = (const FsPathError *)errors_as(pe, TYPE_FS_PATH_ERROR);
    CHECK(fp != NULL && !fs_path_error_timeout(fp));
    Error again = fs_path_error_new(a, BURROW_S("read"), BURROW_S("x"),
                                    syscall_errno_as_error(SYSCALL_EAGAIN, a));
    fp = (const FsPathError *)errors_as(again, TYPE_FS_PATH_ERROR);
    CHECK(fp != NULL && fs_path_error_timeout(fp));

    /* error_retain clones it, and the clone is still an Errno. */
    Arena ar2;
    arena_init(&ar2, NULL, 0);
    Error kept = error_retain(arena_allocator(&ar2), err);
    ARENA_END;
    p = (const SyscallErrno *)errors_as(kept, TYPE_SYSCALL_ERRNO);
    CHECK(p != NULL && *p == SYSCALL_EEXIST);
    CHECK(str_is(error_text(kept), "file exists"));
    arena_free(&ar2);
}

/* Go lists a type's methods in name order, and the four are there with the
 * signatures Go gives them. */
static void TestMethods(TestingT *t) {
    const Type *ty = TYPE_SYSCALL_ERRNO;
    CHECK(ty->kind == KIND_UINTPTR);
    CHECK(type_methods_sorted(ty));
    CHECK_INT_EQ(ty->nmethod, 4);
    const char *names[] = {"Error", "Is", "Temporary", "Timeout"};
    for (int i = 0; i < 4; i++)
        CHECK(type_method_by_name(ty, str_from_bytes((const Byte *)names[i],
                                                     (Int)strlen(names[i]))) != NULL);

    const Method *m = type_method_by_name(ty, BURROW_S("Is"));
    SyscallErrno e = SYSCALL_EPERM;
    Error target = fs_err_permission;
    bool out = false;
    void *args[1] = {&target};
    void *rets[1] = {&out};
    CHECK(m != NULL && method_call(m, &e, args, rets));
    CHECK(out);

    m = type_method_by_name(ty, BURROW_S("Error"));
    Str text = BURROW_STR_EMPTY;
    rets[0] = &text;
    CHECK(m != NULL && method_call(m, &e, NULL, rets));
    CHECK(str_is(text, "operation not permitted"));
}

/* Straight after a failed PAL call the Errno is the system's own number, and
 * for any other PalErrno it is the E constant. */
static void TestFromPal(TestingT *t) {
    PalErrno perr = PAL_OK;
    int64_t fd =
        pal_open("burrow-syscall-test-does-not-exist/x", PAL_O_RDONLY, 0, &perr);
    CHECK(fd == PAL_INVALID_HANDLE);
    if (perr != PAL_ENOENT) {
        testing_t_errorf_v(t, "pal_open gave %s, want PAL_ENOENT",
                           pal_errno_string(perr));
        return;
    }
    SyscallErrno e = syscall_errno_from_pal(perr);
#if defined(BURROW_OS_WINDOWS)
    if (e != SYSCALL_ERROR_PATH_NOT_FOUND && e != SYSCALL_ERROR_FILE_NOT_FOUND)
        testing_t_errorf_v(
            t, "syscall_errno_from_pal(PAL_ENOENT) = %d, want an ERROR_ code", (Int)e);
#else
    CHECK_INT_EQ(e, SYSCALL_ENOENT);
#endif
    CHECK(syscall_errno_is(e, fs_err_not_exist));
    /* Not the last failure, so the table. */
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_EACCES), SYSCALL_EACCES);
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_ETIMEDOUT), SYSCALL_ETIMEDOUT);
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_EAFNOSUPPORT), SYSCALL_EAFNOSUPPORT);
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_OK), 0);
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_EHOSTNOTFOUND), 0);
    CHECK_INT_EQ(syscall_errno_from_pal(PAL_EOTHER), 0);
}

#if !defined(BURROW_OS_WINDOWS)
/* The generated numbers are the C library's, for every E constant both have. */
static void TestMatchesErrnoH(TestingT *t) {
#define E(name) CHECK_INT_EQ(SYSCALL_##name, name)
    E(EPERM);
    E(ENOENT);
    E(ESRCH);
    E(EINTR);
    E(EIO);
    E(EBADF);
    E(ECHILD);
    E(EDEADLK);
    E(EAGAIN);
    E(ENOMEM);
    E(EACCES);
    E(EFAULT);
    E(EBUSY);
    E(EEXIST);
    E(EXDEV);
    E(ENODEV);
    E(ENOTDIR);
    E(EISDIR);
    E(EINVAL);
    E(ENFILE);
    E(EMFILE);
    E(ENOTTY);
    E(EFBIG);
    E(ENOSPC);
    E(ESPIPE);
    E(EROFS);
    E(EMLINK);
    E(EPIPE);
    E(ERANGE);
    E(ENAMETOOLONG);
    E(ENOSYS);
    E(ENOTEMPTY);
    E(ELOOP);
    E(ENOTSUP);
    E(EOVERFLOW);
    E(ECANCELED);
    E(ETIMEDOUT);
    E(EADDRINUSE);
    E(EADDRNOTAVAIL);
    E(ENETDOWN);
    E(ENETUNREACH);
    E(ECONNABORTED);
    E(ECONNRESET);
    E(ENOBUFS);
    E(EISCONN);
    E(ENOTCONN);
    E(ECONNREFUSED);
    E(EHOSTUNREACH);
    E(EALREADY);
    E(EINPROGRESS);
    E(EPROTONOSUPPORT);
    E(EAFNOSUPPORT);
    E(EOPNOTSUPP);
    E(EWOULDBLOCK);
#undef E
}
#endif

#if defined(BURROW_OS_WINDOWS)
#define PLATFORM_TESTS(X) X(TestErrorTextInvented)
#else
#define PLATFORM_TESTS(X) X(TestMatchesErrnoH)
#endif

#define TESTS(X)                                                                       \
    X(TestErrorText)                                                                   \
    X(TestErrorTextUnknown)                                                            \
    X(TestIs)                                                                          \
    X(TestTimeoutTemporary)                                                            \
    X(TestAsError)                                                                     \
    X(TestMethods)                                                                     \
    X(TestFromPal)                                                                     \
    PLATFORM_TESTS(X)

TESTING_MAIN(TESTS)
