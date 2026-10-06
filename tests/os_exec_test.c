/* Derived from Go's src/os/exec_unix_test.go, os_test.go, executable_test.go
 * and src/syscall/syscall_linux_test.go.
 * Go source: go1.27.1.
 *
 * StartProcess, Process and ProcessState, FindProcess, Executable, and the
 * syscall Signal and WaitStatus underneath them.
 *
 * The child is this program again. BURROW_OS_EXEC_CHILD in its environment
 * says what it should do, and it does that before any test runs.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#include "check.h"

#include <string.h>

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

#define CHILD_VAR "BURROW_OS_EXEC_CHILD"

#define CHECK_S(got, want)                                                             \
    do {                                                                               \
        Str g_ = (got), w_ = (want);                                                   \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "%s = %q, want %q", #got, g_, w_);                   \
    } while (0)

static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    return e != NULL ? *e : 0;
}

static Slice strs(const Str *v, Int n) {
    Slice s = slice_make(a, TYPE_OF(Str), n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Str, s, i) = v[i];
    return s;
}

static Str self_path(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Error e = BURROW_NO_ERROR;
    Str exe = os_executable(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Executable: %s", error_text(e));
    return exe;
}

/* Starts this program as a child that does what, with files as its
 * descriptors when files.len is not 0. */
static OsProcess *start_child(TestingT *t, Str what, Slice files, Error *err) {
    Str exe = self_path(t);
    Slice env = os_environ(a);
    env = BURROW_APPEND(Str, a, env, fmt_sprintf_v(a, "%s=%s", S(CHILD_VAR), what));
    OsProcAttr attr = {BURROW_STR_EMPTY, env, files, NULL};
    Str argv[1] = {exe};
    return os_start_process(a, exe, strs(argv, 1), &attr, err);
}

static OsProcessState *wait_ok(TestingT *t, OsProcess *p) {
    Error e = BURROW_NO_ERROR;
    OsProcessState *ps = os_process_wait(p, &e);
    if (BURROW_FAILED(e) || ps == NULL)
        testing_t_fatalf_v(t, "Wait: %s", error_text(e));
    return ps;
}

/* ------------------------------------------------------ exit and success */

static void TestStartProcessExit(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:3"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    CHECK(p->pid > 0);
    OsProcessState *ps = wait_ok(t, p);
    CHECK(os_process_state_exited(ps));
    CHECK(!os_process_state_success(ps));
    CHECK_INT_EQ(os_process_state_exit_code(ps), 3);
    CHECK_S(os_process_state_string(ps, a), S("exit status 3"));
#if !defined(BURROW_OS_WINDOWS)
    CHECK_INT_EQ(os_process_state_pid(ps), p->pid);
#endif
    CHECK(os_process_state_user_time(ps) >= 0);
    CHECK(os_process_state_system_time(ps) >= 0);
    os_process_state_free(ps);
    os_process_free(p);
}

static void TestStartProcessSuccess(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:0"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    OsProcessState *ps = wait_ok(t, p);
    CHECK(os_process_state_success(ps));
    CHECK_INT_EQ(os_process_state_exit_code(ps), 0);
    CHECK_S(os_process_state_string(ps, a), S("exit status 0"));
    os_process_state_free(ps);
    os_process_free(p);
}

/* The child's standard output is the write end of a pipe, which checks that
 * Files and Env both arrive. */
static void TestStartProcessFiles(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Error e = BURROW_NO_ERROR;
    OsFile *w = NULL;
    OsFile *r = os_pipe(a, &w, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Pipe: %s", error_text(e));
    OsFile *files[3] = {os_stdin, w, os_stderr};
    OsProcess *p = start_child(t, S("echo"), (Slice){files, 3, 3, NULL}, &e);
    CHECK(BURROW_OK(os_file_close(w)));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    Slice got = io_read_all(a, os_file_as_io_reader(r), &e);
    CHECK(BURROW_OK(e));
    CHECK(BURROW_OK(os_file_close(r)));
    CHECK_S(str_from_bytes((const Byte *)got.p, got.len), S("child says echo\n"));
    OsProcessState *ps = wait_ok(t, p);
    CHECK(os_process_state_success(ps));
    os_process_state_free(ps);
    os_process_free(p);
}

/* -------------------------------------------------------- signals and kill */

static void TestKillStartProcess(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("sleep"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    time_sleep(100 * TIME_MILLISECOND);
    Error ke = os_process_kill(p);
    if (BURROW_FAILED(ke))
        testing_t_fatalf_v(t, "Kill: %s", error_text(ke));
    OsProcessState *ps = wait_ok(t, p);
    CHECK(!os_process_state_success(ps));
#if defined(BURROW_OS_WINDOWS)
    CHECK_S(os_process_state_string(ps, a), S("exit status 1"));
#else
    CHECK(!os_process_state_exited(ps));
    CHECK_INT_EQ(os_process_state_exit_code(ps), -1);
    CHECK_S(os_process_state_string(ps, a), S("signal: killed"));
    SyscallWaitStatus ws = os_process_state_sys(ps);
    CHECK(syscall_wait_status_signaled(ws));
    CHECK_INT_EQ(syscall_wait_status_signal(ws), SYSCALL_SIGKILL);
#endif
    os_process_state_free(ps);
    os_process_free(p);
}

/* Go's TestProcessSignalAfterWait and the second Wait. */
static void TestSignalAfterWait(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:0"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    os_process_state_free(wait_ok(t, p));
    Error se = os_process_kill(p);
#if defined(BURROW_OS_WINDOWS)
    CHECK(errno_of(se) == SYSCALL_EINVAL);
#else
    CHECK(errors_is(se, os_err_process_done));
    CHECK_S(error_text(se), S("os: process already finished"));
#endif
    OsProcessState *ps = os_process_wait(p, &e);
    CHECK(ps == NULL);
    CHECK(BURROW_FAILED(e));
    os_process_free(p);
}

static void TestUnsupportedSignal(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:0"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    Error se = os_process_signal(p, (OsSignal){NULL, NULL});
#if defined(BURROW_OS_WINDOWS)
    CHECK(errno_of(se) == SYSCALL_EWINDOWS);
    se = os_process_signal(p, os_interrupt);
    CHECK(errno_of(se) == SYSCALL_EWINDOWS);
#else
    CHECK_S(error_text(se), S("os: unsupported signal type"));
#endif
    os_process_state_free(wait_ok(t, p));
    os_process_free(p);
}

static void TestRelease(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:0"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    /* Wait first, so that nothing is left as a zombie by the release. */
    os_process_state_free(wait_ok(t, p));
    OsProcess *q = os_find_process(a, os_getpid(), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "FindProcess: %s", error_text(e));
    CHECK(BURROW_OK(os_process_release(q)));
#if defined(BURROW_OS_WINDOWS)
    CHECK(errno_of(os_process_release(q)) == SYSCALL_EINVAL);
    CHECK(errno_of(os_process_kill(q)) == SYSCALL_EINVAL);
#else
    CHECK(BURROW_OK(os_process_release(q)));
    CHECK_INT_EQ(q->pid, -1);
    CHECK_S(error_text(os_process_kill(q)), S("os: process already released"));
#endif
    os_process_free(q);
    os_process_free(p);
}

static void TestFindProcess(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = os_find_process(a, os_getpid(), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "FindProcess: %s", error_text(e));
    CHECK_INT_EQ(p->pid, os_getpid());
    CHECK(BURROW_OK(os_process_release(p)));
    os_process_free(p);
}

static void with_handle_cb(void *env, Uintptr h) {
    *(Uintptr *)env = h;
}

static void TestWithHandle(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsProcess *p = start_child(t, S("exit:0"), (Slice){0}, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    Uintptr got = 0;
    Error he = os_process_with_handle(p, (OsHandleFunc){with_handle_cb, &got});
#if defined(BURROW_OS_WINDOWS)
    CHECK(BURROW_OK(he));
    CHECK(got != 0);
#else
    CHECK(errors_is(he, os_err_no_handle));
    CHECK(got == 0);
#endif
    os_process_state_free(wait_ok(t, p));
#if defined(BURROW_OS_WINDOWS)
    he = os_process_with_handle(p, (OsHandleFunc){with_handle_cb, &got});
    CHECK(BURROW_FAILED(he));
#endif
    os_process_free(p);
}

/* ---------------------------------------------------------- start errors */

static void TestStartProcessErrors(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str exe = self_path(t);
    Str argv[1] = {exe};

    /* A directory that is not there is a chdir error. */
    OsProcAttr attr = {S("/no/such/burrow/dir"), {0}, {0}, NULL};
    OsProcess *p = os_start_process(a, exe, strs(argv, 1), &attr, &e);
    CHECK(p == NULL);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL);
    if (pe != NULL)
        CHECK_S(pe->op, S("chdir"));
    CHECK(os_is_not_exist(e));

    /* A NUL in an argument. */
    e = BURROW_NO_ERROR;
    Str bad[2] = {exe, str_from_bytes((const Byte *)"a\0b", 3)};
    p = os_start_process(a, exe, strs(bad, 2), NULL, &e);
    CHECK(p == NULL);
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL);
    if (pe != NULL) {
        CHECK_S(pe->op, S("fork/exec"));
        CHECK(errno_of(pe->err) == SYSCALL_EINVAL);
    }

    /* A program that is not there. */
    e = BURROW_NO_ERROR;
    Str missing = S("/no/such/burrow/program");
    Str margv[1] = {missing};
    p = os_start_process(a, missing, strs(margv, 1), NULL, &e);
    CHECK(p == NULL);
    CHECK(os_is_not_exist(e));
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL);
    if (pe != NULL) {
        CHECK_S(pe->op, S("fork/exec"));
        CHECK_S(pe->path, missing);
    }
}

/* ---------------------------------------------------------- ProcessState */

static void TestProcessStateNil(TestingT *t) {
    CHECK_S(os_process_state_string(NULL, a), S("<nil>"));
    CHECK_INT_EQ(os_process_state_exit_code(NULL), -1);
}

/* Go's TestExecutable checks the path against the one the test was run as. */
static void TestExecutable(TestingT *t) {
    Str exe = self_path(t);
    CHECK(exe.len > 0);
    Error e = BURROW_NO_ERROR;
    OsFileInfo got = os_stat(a, exe, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Stat(%s): %s", exe, error_text(e));
    Slice args = os_args();
    Str arg0 = BURROW_AT(Str, args, 0);
    OsFileInfo want = os_stat(a, arg0, &e);
    if (BURROW_FAILED(e)) {
        testing_t_logf_v(t, "Stat(Args[0] %s): %s", arg0, error_text(e));
        return;
    }
    if (!os_same_file(got, want))
        testing_t_errorf_v(t, "Executable() = %s, which is not %s", exe, arg0);
}

/* -------------------------------------------------------------- syscall */

static void TestSignalString(TestingT *t) {
    CHECK_S(syscall_signal_string(1000, a), S("signal 1000"));
    CHECK_S(syscall_signal_string(-1, a), S("signal -1"));
    CHECK_S(syscall_signal_string(SYSCALL_SIGKILL, a), S("killed"));
    CHECK_S(syscall_signal_string(SYSCALL_SIGINT, a), S("interrupt"));
    CHECK(os_signal_to_syscall(os_kill) == SYSCALL_SIGKILL);
    OsSignal s = os_signal_from_syscall(SYSCALL_SIGTERM);
    CHECK_S(s.vt->string(s.data, a), syscall_signal_string(SYSCALL_SIGTERM, a));
    CHECK(s.vt->self_type == TYPE_SYSCALL_SIGNAL);
    CHECK(os_signal_from_syscall(256).vt == NULL);
    CHECK(os_signal_to_syscall((OsSignal){NULL, NULL}) == -1);
}

#if defined(BURROW_OS_LINUX)
static void TestWaitStatus(TestingT *t) {
    SyscallWaitStatus w = 0x0300;
    CHECK(syscall_wait_status_exited(w));
    CHECK_INT_EQ(syscall_wait_status_exit_status(w), 3);
    w = 0x0089;
    CHECK(syscall_wait_status_signaled(w));
    CHECK(syscall_wait_status_core_dump(w));
    CHECK_INT_EQ(syscall_wait_status_signal(w), SYSCALL_SIGKILL);
    CHECK_INT_EQ(syscall_wait_status_exit_status(w), -1);
    w = 0x137f;
    CHECK(syscall_wait_status_stopped(w));
    CHECK_INT_EQ(syscall_wait_status_stop_signal(w), 0x13);
    w = 0x0005057f;
    CHECK_INT_EQ(syscall_wait_status_stop_signal(w), SYSCALL_SIGTRAP);
    CHECK_INT_EQ(syscall_wait_status_trap_cause(w), 5);
    CHECK(syscall_wait_status_continued(0xffff));
    CHECK(!syscall_wait_status_continued(0x137f));
}
#endif

#if !defined(BURROW_OS_WINDOWS)
static void TestTimeval(TestingT *t) {
    SyscallTimeval tv = syscall_nsec_to_timeval(1500000001);
    CHECK_INT_EQ(tv.sec, 1);
    CHECK_INT_EQ(tv.usec, 500001);
    CHECK_INT_EQ(syscall_timeval_nano(&tv), 1500001000);
    tv = syscall_nsec_to_timeval(-1500000000);
    CHECK_INT_EQ(tv.sec, -2);
    CHECK_INT_EQ(tv.usec, 500001); /* the 999 Go adds to round up comes first */
}
#endif

/* ----------------------------------------------------------- the child */

static void child(Str what) {
    if (strings_has_prefix(what, S("exit:"))) {
        os_exit((Int)(what.p[5] - '0'));
    } else if (str_eq(what, S("echo"))) {
        Error e = BURROW_NO_ERROR;
        os_file_write_string(os_stdout, fmt_sprintf_v(a, "child says %s\n", what), &e);
        os_exit(BURROW_FAILED(e) ? 2 : 0);
    } else if (str_eq(what, S("sleep"))) {
        time_sleep(60 * TIME_SECOND);
        os_exit(0);
    }
    os_exit(9);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#if defined(BURROW_OS_LINUX)
#define LINUX_TESTS(X) X(TestWaitStatus)
#else
#define LINUX_TESTS(X)
#endif

#if !defined(BURROW_OS_WINDOWS)
#define UNIX_TESTS(X) X(TestTimeval)
#else
#define UNIX_TESTS(X)
#endif

#define TESTS(X)                                                                       \
    X(TestStartProcessExit)                                                            \
    X(TestStartProcessSuccess)                                                         \
    X(TestStartProcessFiles)                                                           \
    X(TestKillStartProcess)                                                            \
    X(TestSignalAfterWait)                                                             \
    X(TestUnsupportedSignal)                                                           \
    X(TestRelease)                                                                     \
    X(TestFindProcess)                                                                 \
    X(TestWithHandle)                                                                  \
    X(TestStartProcessErrors)                                                          \
    X(TestProcessStateNil)                                                             \
    X(TestExecutable)                                                                  \
    X(TestSignalString)                                                                \
    LINUX_TESTS(X)                                                                     \
    UNIX_TESTS(X)

static int os_exec_main(TestingM *m) {
    setup();
    Str what = os_getenv(a, S(CHILD_VAR));
    if (what.len > 0)
        child(what);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_exec_main, TESTS)
