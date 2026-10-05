/* ForkExec, StartProcess, Exec, the SysProcAttr fields, Setgroups, and on
 * Linux the set id family and AllThreadsSyscall.
 *
 * TestZeroSysProcAttr, TestSetpgid, TestPgid, TestInvalidExec, TestExec and
 * TestForkExecNilArgv are ported from Go's src/syscall/exec_unix_test.go,
 * TestCloneNEWUSERAndRemap, TestUnshareUidGidMapping and TestPidFD from
 * exec_linux_test.go, and TestAllThreadsSyscall from syscall_linux_test.go.
 * The others are burrow's own.
 * Go source: go1.27.1.
 *
 * The child is this program again. BURROW_SYSCALL_EXEC_CHILD in its
 * environment says what it should do, and it does that before any test runs.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/strings.h"

#include "check.h"

#include <string.h>

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

#define CHILD_VAR "BURROW_SYSCALL_EXEC_CHILD"

#if !defined(BURROW_OS_WINDOWS)

static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    return e != NULL ? *e : 0;
}

/* Cosmopolitan has no way to make a raw system call, so everything that goes
 * through one gives ENOSYS there. These tests need one, in the test or in the
 * child it starts. */
static void skip_without_raw_calls(TestingT *t) {
#if defined(BURROW_OS_COSMO)
    testing_t_skip_v(t, "Cosmopolitan has no raw system calls");
#else
    (void)t;
#endif
}

static Slice strs(const Str *v, Int n) {
    Slice s = slice_make(a, TYPE_OF(Str), n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Str, s, i) = v[i];
    return s;
}

static Slice uintptrs(const Uintptr *v, Int n) {
    Slice s = slice_make(a, TYPE_UINTPTR, n, n);
    for (Int i = 0; i < n; i++)
        BURROW_AT(Uintptr, s, i) = v[i];
    return s;
}

static Str self_path(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str exe = os_executable(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Executable: %s", error_text(e));
    return exe;
}

/* Our environment with the child variable set to what. A child that starts
 * another already has one, and the first of two is the one that counts, so
 * the old one has to go rather than stay in front of the new. */
static Slice child_env(Str what) {
    Slice env = os_environ(a);
    Slice out = slice_make(a, TYPE_STRING, 0, env.len + 1);
    for (Int i = 0; i < env.len; i++) {
        Str kv = BURROW_AT(Str, env, i);
        if (!strings_has_prefix(kv, S(CHILD_VAR "=")))
            out = BURROW_APPEND(Str, a, out, kv);
    }
    return BURROW_APPEND(Str, a, out, fmt_sprintf_v(a, "%s=%s", S(CHILD_VAR), what));
}

/* Starts this program as a child that does what, with sys and files. */
static Int start_child(TestingT *t, Str what, const SyscallSysProcAttr *sys,
                       Slice files, Error *err) {
    Str exe = self_path(t);
    Str argv[1] = {exe};
    SyscallProcAttr attr = {BURROW_STR_EMPTY, child_env(what), files, sys};
    return syscall_fork_exec(exe, strs(argv, 1), &attr, err);
}

/* Waits for pid and gives its exit status, or -1 when a signal ended it. */
static Int wait_code(TestingT *t, Int pid) {
    SyscallWaitStatus ws;
    Error e;
    Int got;
    do {
        e = BURROW_NO_ERROR;
        got = syscall_wait4(pid, &ws, 0, NULL, &e);
    } while (errno_of(e) == SYSCALL_EINTR);
    if (BURROW_FAILED(e) || got != pid) {
        testing_t_errorf_v(t, "Wait4(%d): %s", (int)pid, error_text(e));
        return -2;
    }
    return syscall_wait_status_exit_status(ws);
}

static void make_pipe(TestingT *t, Int fds[2]) {
    /* os makes it rather than syscall_pipe, which is Go's Pipe and leaves both
     * ends open across exec, so a child would hold the write end of its own
     * release pipe and never see it close. The two OsFiles stay in the arena
     * and the descriptors are closed with syscall_close. */
    Error e = BURROW_NO_ERROR;
    OsFile *w = NULL;
    OsFile *r = os_pipe(a, &w, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Pipe: %s", error_text(e));
    fds[0] = (Int)os_file_fd(r);
    fds[1] = (Int)os_file_fd(w);
}

/* Everything on fd until its end, as a Str in a. */
static Str read_all(Int fd) {
    Byte *buf = (Byte *)mem_alloc(a, 4096, 1);
    Int n = 0;
    while (n < 4096) {
        Error e = BURROW_NO_ERROR;
        Int got = syscall_read(fd, (Slice){buf + n, 4096 - n, 4096 - n, TYPE_BYTE}, &e);
        if (errno_of(e) == SYSCALL_EINTR)
            continue;
        if (got <= 0)
            break;
        n += got;
    }
    return str_from_bytes(buf, n);
}

/* A child in the "wait" state, blocked reading its standard input, and the
 * write end that lets it go. */
typedef struct Waiting {
    Int pid;
    Int release;
} Waiting;

static Waiting start_waiting(TestingT *t, const SyscallSysProcAttr *sys) {
    Int p[2];
    make_pipe(t, p);
    Uintptr files[3] = {(Uintptr)p[0], 1, 2};
    Error e = BURROW_NO_ERROR;
    Int pid = start_child(t, S("wait"), sys, uintptrs(files, 3), &e);
    (void)syscall_close(p[0]);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[1]);
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    }
    return (Waiting){pid, p[1]};
}

static void stop_waiting(TestingT *t, Waiting w) {
    (void)syscall_close(w.release);
    CHECK_INT_EQ(wait_code(t, w.pid), 0);
}

static Int pgid_of(TestingT *t, Int pid) {
    Error e = BURROW_NO_ERROR;
    Int g = syscall_getpgid(pid, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Getpgid: %s", error_text(e));
    return g;
}

/* ------------------------------------------------------------ process groups */

static void TestZeroSysProcAttr(TestingT *t) {
    skip_without_raw_calls(t);
    Int ppid = syscall_getpid(), ppgrp = syscall_getpgrp();
    Waiting w = start_waiting(t, NULL);
    Int cpgrp = pgid_of(t, w.pid);
    if (w.pid == ppid)
        testing_t_errorf_v(t, "Parent and child have the same process ID");
    if (cpgrp != ppgrp)
        testing_t_errorf_v(t, "Child is not in parent's process group");
    stop_waiting(t, w);
}

static void TestSetpgid(TestingT *t) {
    skip_without_raw_calls(t);
    Int ppgrp = syscall_getpgrp();
    SyscallSysProcAttr sys = {.setpgid = true};
    Waiting w = start_waiting(t, &sys);
    Int cpgrp = pgid_of(t, w.pid);
    if (cpgrp == ppgrp)
        testing_t_errorf_v(t, "Parent and child are in the same process group");
    if (cpgrp != w.pid)
        testing_t_errorf_v(t, "Child's process group is not the child's process ID");
    stop_waiting(t, w);
}

static void TestPgid(TestingT *t) {
    skip_without_raw_calls(t);
    Int ppgrp = syscall_getpgrp();
    SyscallSysProcAttr sys1 = {.setpgid = true};
    Waiting w1 = start_waiting(t, &sys1);
    Int cpgrp1 = pgid_of(t, w1.pid);
    if (cpgrp1 == ppgrp)
        testing_t_errorf_v(t, "Parent and child 1 are in the same process group");
    if (cpgrp1 != w1.pid)
        testing_t_errorf_v(t, "Child 1's process group is not its process ID");

    SyscallSysProcAttr sys2 = {.setpgid = true, .pgid = cpgrp1};
    Waiting w2 = start_waiting(t, &sys2);
    Int cpgrp2 = pgid_of(t, w2.pid);
    if (cpgrp2 == ppgrp)
        testing_t_errorf_v(t, "Parent and child 2 are in the same process group");
    if (cpgrp1 != cpgrp2)
        testing_t_errorf_v(t, "Child 1 and 2 are not in the same process group");
    stop_waiting(t, w2);
    stop_waiting(t, w1);
}

/* os goes through the same SysProcAttr code, so joining a group works there
 * too now. */
static void TestOsPgid(TestingT *t) {
    skip_without_raw_calls(t);
    SyscallSysProcAttr sys1 = {.setpgid = true};
    Waiting w1 = start_waiting(t, &sys1);
    SyscallSysProcAttr sys2 = {.setpgid = true, .pgid = w1.pid};
    Str exe = self_path(t);
    Str argv[1] = {exe};
    OsProcAttr attr = {
        BURROW_STR_EMPTY, child_env(S("pgrp")), {NULL, 0, 0, NULL}, &sys2};
    Int p[2];
    make_pipe(t, p);
    OsFile *out = os_new_file(a, (Uintptr)p[1], S("|1"));
    OsFile *files[3] = {NULL, out, os_stderr};
    attr.files = (Slice){files, 3, 3, NULL};
    Error e = BURROW_NO_ERROR;
    OsProcess *proc = os_start_process(a, exe, strs(argv, 1), &attr, &e);
    (void)os_file_close(out);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[0]);
        stop_waiting(t, w1);
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    }
    Str got = read_all(p[0]);
    (void)syscall_close(p[0]);
    OsProcessState *ps = os_process_wait(proc, &e);
    CHECK(ps != NULL && os_process_state_success(ps));
    Str want = fmt_sprintf_v(a, "%d\n", (int)w1.pid);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "child's group = %q, want %q", got, want);
    if (ps != NULL)
        os_process_state_free(ps);
    os_process_free(proc);
    stop_waiting(t, w1);
}

/* ------------------------------------------------------------------ errors */

static void TestInvalidExec(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    SyscallSysProcAttr both = {.setctty = true, .foreground = true, .ctty = 0};
    Int pid = start_child(t, S("exit:0"), &both, (Slice){NULL, 0, 0, NULL}, &e);
    CHECK_INT_EQ(pid, 0);
    if (!BURROW_FAILED(e))
        testing_t_errorf_v(t, "expected error setting both SetCtty and Foreground");
    else
        CHECK(
            str_eq(error_text(e), S("both Setctty and Foreground set in SysProcAttr")));

    e = BURROW_NO_ERROR;
    SyscallSysProcAttr bad = {.setctty = true, .ctty = 3};
    Uintptr files[3] = {0, 1, 2};
    pid = start_child(t, S("exit:0"), &bad, uintptrs(files, 3), &e);
    if (!BURROW_FAILED(e))
        testing_t_errorf_v(t, "expected error with invalid Ctty value");
    else
        CHECK(str_eq(error_text(e), S("Setctty set but Ctty not valid in child")));
}

static void TestForkExecNilArgv(TestingT *t) {
    (void)t;
    /* Nothing that can run, so the exec fails in the child and the error
     * comes back here, with the child already reaped. */
    Error e = BURROW_NO_ERROR;
    Int pid = syscall_fork_exec(S("/dev/null"), (Slice){NULL, 0, 0, NULL}, NULL, &e);
    CHECK_INT_EQ(pid, 0);
    CHECK_INT_EQ(errno_of(e), SYSCALL_EACCES);
}

static void TestForkExecNotFound(TestingT *t) {
    (void)t;
    Str argv[1] = {S("/nonexistent/burrow")};
    Error e = BURROW_NO_ERROR;
    Int pid = syscall_fork_exec(argv[0], strs(argv, 1), NULL, &e);
    CHECK_INT_EQ(pid, 0);
    CHECK_INT_EQ(errno_of(e), SYSCALL_ENOENT);

    Str bad = str_from_bytes((const Byte *)"a\0b", 3);
    e = BURROW_NO_ERROR;
    pid = syscall_fork_exec(bad, strs(argv, 1), NULL, &e);
    CHECK_INT_EQ(errno_of(e), SYSCALL_EINVAL);
}

/* ---------------------------------------------------------------- exec */

static void TestExec(TestingT *t) {
    skip_without_raw_calls(t);
    Error e = BURROW_NO_ERROR;
    Int pid =
        start_child(t, S("exec"), NULL, uintptrs((const Uintptr[]){0, 1, 2}, 3), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    /* The child execs itself as "exit:5", so 5 is the exec working. */
    CHECK_INT_EQ(wait_code(t, pid), 5);
}

static void TestExecFails(TestingT *t) {
    skip_without_raw_calls(t);
    (void)t;
    Str argv[1] = {S("/nonexistent/burrow")};
    Error e = syscall_exec(argv[0], strs(argv, 1), (Slice){NULL, 0, 0, NULL});
    CHECK_INT_EQ(errno_of(e), SYSCALL_ENOENT);
}

/* -------------------------------------------------- environment and files */

/* A nil Env is no environment at all, which is Go's rule for ForkExec. */
static void TestForkExecNilEnv(TestingT *t) {
    Int p[2];
    make_pipe(t, p);
    Str argv[1] = {S("/usr/bin/env")};
    Uintptr files[3] = {0, (Uintptr)p[1], 2};
    SyscallProcAttr attr = {
        BURROW_STR_EMPTY, {NULL, 0, 0, NULL}, uintptrs(files, 3), NULL};
    Error e = BURROW_NO_ERROR;
    Int pid = syscall_fork_exec(argv[0], strs(argv, 1), &attr, &e);
    (void)syscall_close(p[1]);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[0]);
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    }
    Str got = read_all(p[0]);
    (void)syscall_close(p[0]);
    CHECK_INT_EQ(wait_code(t, pid), 0);
    if (got.len != 0)
        testing_t_errorf_v(t, "env printed %q, want nothing", got);
}

static void TestForkExecDir(TestingT *t) {
    skip_without_raw_calls(t);
    Int p[2];
    make_pipe(t, p);
    Str argv[1] = {S("/bin/pwd")};
    Uintptr files[3] = {0, (Uintptr)p[1], 2};
    SyscallProcAttr attr = {S("/"), {NULL, 0, 0, NULL}, uintptrs(files, 3), NULL};
    Error e = BURROW_NO_ERROR;
    Int pid = syscall_fork_exec(argv[0], strs(argv, 1), &attr, &e);
    (void)syscall_close(p[1]);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[0]);
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    }
    Str got = read_all(p[0]);
    (void)syscall_close(p[0]);
    CHECK_INT_EQ(wait_code(t, pid), 0);
    if (!str_eq(got, S("/\n")))
        testing_t_errorf_v(t, "pwd = %q, want %q", got, S("/\n"));
}

/* A descriptor that is not close on exec stays open in a ForkExec child, as
 * it does in Go, and os_start_process closes it. The shell is asked to read
 * from it, which fails with a bad descriptor if it is not there. */
static void TestForkExecKeepsFds(TestingT *t) {
    skip_without_raw_calls(t);
    Int p[2];
    make_pipe(t, p);
    /* Linux on arm64, riscv64 and loong64 has no dup2, and Go has no Dup2
     * there either. */
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO)
    Error e = syscall_dup3(p[0], 7, 0);
#else
    Error e = syscall_dup2(p[0], 7);
#endif
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Dup2: %s", error_text(e));
    (void)syscall_close(p[1]);
    Str argv[3] = {S("/bin/sh"), S("-c"), S("exec true <&7")};
    Uintptr files[3] = {0, 1, 2};
    SyscallProcAttr attr = {
        BURROW_STR_EMPTY, {NULL, 0, 0, NULL}, uintptrs(files, 3), NULL};
    e = BURROW_NO_ERROR;
    Int pid = syscall_fork_exec(argv[0], strs(argv, 3), &attr, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "ForkExec: %s", error_text(e));
    else
        CHECK_INT_EQ(wait_code(t, pid), 0);

    OsFile *files2[3] = {os_stdin, os_stdout, os_stderr};
    OsProcAttr oattr = {
        BURROW_STR_EMPTY, {NULL, 0, 0, NULL}, {files2, 3, 3, NULL}, NULL};
    e = BURROW_NO_ERROR;
    OsProcess *proc = os_start_process(a, argv[0], strs(argv, 3), &oattr, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "StartProcess: %s", error_text(e));
    } else {
        OsProcessState *ps = os_process_wait(proc, &e);
        if (ps == NULL || os_process_state_success(ps))
            testing_t_errorf_v(t, "os child could read descriptor 7, want it closed");
        if (ps != NULL)
            os_process_state_free(ps);
        os_process_free(proc);
    }
    (void)syscall_close(7);
    (void)syscall_close(p[0]);
}

static void TestStartProcessHandle(TestingT *t) {
    Str exe = self_path(t);
    Str argv[1] = {exe};
    SyscallProcAttr attr = {BURROW_STR_EMPTY, child_env(S("exit:3")),
                            uintptrs((const Uintptr[]){0, 1, 2}, 3), NULL};
    Uintptr handle = 99;
    Error e = BURROW_NO_ERROR;
    Int pid = syscall_start_process(exe, strs(argv, 1), &attr, &handle, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "StartProcess: %s", error_text(e));
    CHECK(pid > 0);
    CHECK_INT_EQ(handle, 0);
    CHECK_INT_EQ(wait_code(t, pid), 3);
}

static void TestForkLock(TestingT *t) {
    (void)t;
    /* A reader holds off every fork, and a fork waits for it. */
    sync_rw_mutex_r_lock(&syscall_fork_lock);
    CHECK(!sync_rw_mutex_try_lock(&syscall_fork_lock));
    sync_rw_mutex_r_unlock(&syscall_fork_lock);
    CHECK(sync_rw_mutex_try_lock(&syscall_fork_lock));
    sync_rw_mutex_unlock(&syscall_fork_lock);
}

/* ------------------------------------------------------------ credentials */

static void TestSetgroups(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Slice groups = os_getgroups(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Getgroups: %s", error_text(e));
    e = syscall_setgroups(groups);
    if (syscall_getuid() != 0) {
        CHECK_INT_EQ(errno_of(e), SYSCALL_EPERM);
        return;
    }
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Setgroups: %s", error_text(e));
    Slice again = os_getgroups(a, &e);
    CHECK_INT_EQ(again.len, groups.len);
}

/* As root, a child started as nobody says it is nobody. */
static void TestCredential(TestingT *t) {
    if (syscall_getuid() != 0)
        testing_t_skipf_v(t, "not root");
    Int p[2];
    make_pipe(t, p);
    SyscallCredential cred = {65534, 65534, {NULL, 0, 0, NULL}, false};
    SyscallSysProcAttr sys = {.credential = &cred};
    Uintptr files[3] = {0, (Uintptr)p[1], 2};
    Error e = BURROW_NO_ERROR;
    Int pid = start_child(t, S("ids"), &sys, uintptrs(files, 3), &e);
    (void)syscall_close(p[1]);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[0]);
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    }
    Str got = read_all(p[0]);
    (void)syscall_close(p[0]);
    Int code = wait_code(t, pid);
    /* The test binary may live somewhere nobody cannot reach. */
    if (code == 9 || got.len == 0)
        testing_t_skipf_v(t, "child could not run as nobody");
    if (!str_eq(got, S("65534 65534 0\n")))
        testing_t_errorf_v(t, "child ids = %q, want %q", got, S("65534 65534 0\n"));
}

#if defined(BURROW_OS_LINUX)

static void TestAllThreadsSyscall(TestingT *t) {
    (void)t;
    Uintptr r2 = 0;
    SyscallErrno e = 0;
    Uintptr r1 = syscall_all_threads_syscall(SYSCALL_SYS_PRCTL, 0, 0, 0, &r2, &e);
    CHECK_INT_EQ(e, SYSCALL_ENOTSUP);
    CHECK(r1 == ~(Uintptr)0 && r2 == ~(Uintptr)0);
    r1 = syscall_all_threads_syscall6(SYSCALL_SYS_PRCTL, 0, 0, 0, 0, 0, 0, &r2, &e);
    CHECK_INT_EQ(e, SYSCALL_ENOTSUP);
}

/* Setting each id to what it is already works for anyone. */
static void TestSetuidEtc(TestingT *t) {
    Int uid = syscall_getuid(), gid = syscall_getgid();
    Int euid = syscall_geteuid(), egid = syscall_getegid();
    Error e;
#define NOERR(call)                                                                    \
    do {                                                                               \
        e = (call);                                                                    \
        if (BURROW_FAILED(e))                                                          \
            testing_t_errorf_v(t, "%s: %s", #call, error_text(e));                     \
    } while (0)
    NOERR(syscall_setegid(egid));
    NOERR(syscall_seteuid(euid));
    NOERR(syscall_setregid(-1, -1));
    NOERR(syscall_setreuid(-1, -1));
    NOERR(syscall_setresgid(-1, -1, -1));
    NOERR(syscall_setresuid(-1, -1, -1));
    if (uid == euid && gid == egid) {
        NOERR(syscall_setgid(gid));
        NOERR(syscall_setuid(uid));
    }
#undef NOERR
    CHECK_INT_EQ(syscall_getuid(), uid);
    CHECK_INT_EQ(syscall_geteuid(), euid);
    if (uid != 0)
        CHECK_INT_EQ(errno_of(syscall_setuid(uid == 0 ? 1 : 0)), SYSCALL_EPERM);
}

/* Whether e is how a system says it will not make user namespaces, which Go's
 * tests skip on too. */
static bool userns_refused(Error e) {
    SyscallErrno n = errno_of(e);
    return n == SYSCALL_EPERM || n == SYSCALL_EACCES || n == SYSCALL_EINVAL ||
           n == SYSCALL_ENOSPC || n == SYSCALL_ENOSYS || n == SYSCALL_EUSERS;
}

/* Runs the "ids" child in a new user namespace, with clone or with unshare,
 * mapping it to root there, and gives what it printed. */
static Str ids_in_userns(TestingT *t, bool unshare, bool *skipped) {
    *skipped = false;
    Int p[2];
    make_pipe(t, p);
    SyscallSysProcIDMap uid_map = {0, syscall_getuid(), 1};
    SyscallSysProcIDMap gid_map = {0, syscall_getgid(), 1};
    SyscallSysProcAttr sys = {
        .uid_mappings = {&uid_map, 1, 1, NULL},
        .gid_mappings = {&gid_map, 1, 1, NULL},
        .gid_mappings_enable_setgroups = false,
    };
    if (unshare)
        sys.unshareflags = SYSCALL_CLONE_NEWUSER;
    else
        sys.cloneflags = SYSCALL_CLONE_NEWUSER;
    Uintptr files[3] = {0, (Uintptr)p[1], 2};
    Error e = BURROW_NO_ERROR;
    Int pid = start_child(t, S("ids"), &sys, uintptrs(files, 3), &e);
    (void)syscall_close(p[1]);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(p[0]);
        if (userns_refused(e)) {
            *skipped = true;
            return BURROW_STR_EMPTY;
        }
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    }
    Str got = read_all(p[0]);
    (void)syscall_close(p[0]);
    CHECK_INT_EQ(wait_code(t, pid), 0);
    return got;
}

static void TestCloneNEWUSERAndRemap(TestingT *t) {
    bool skipped;
    Str got = ids_in_userns(t, false, &skipped);
    if (skipped)
        testing_t_skipf_v(t, "user namespaces are not available here");
    /* Root in there, and setgroups denied, so no groups but the one. */
    if (!strings_has_prefix(got, S("0 0 ")))
        testing_t_errorf_v(t, "ids in the namespace = %q, want root", got);
}

static void TestUnshareUidGidMapping(TestingT *t) {
    bool skipped;
    Str got = ids_in_userns(t, true, &skipped);
    if (skipped)
        testing_t_skipf_v(t, "user namespaces are not available here");
    if (!strings_has_prefix(got, S("0 0 ")))
        testing_t_errorf_v(t, "ids in the namespace = %q, want root", got);
}

static void TestPidFD(TestingT *t) {
    Int pidfd = -7;
    SyscallSysProcAttr sys = {.pid_fd = &pidfd};
    Error e = BURROW_NO_ERROR;
    Int pid =
        start_child(t, S("exit:4"), &sys, uintptrs((const Uintptr[]){0, 1, 2}, 3), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    /* A kernel before 5.2 has no pidfds and leaves -1. */
    CHECK(pidfd == -1 || pidfd >= 0);
    CHECK_INT_EQ(wait_code(t, pid), 4);
    if (pidfd >= 0)
        CHECK(!BURROW_FAILED(syscall_close(pidfd)));

    /* A failed exec leaves no pidfd behind. */
    pidfd = -7;
    Str argv[1] = {S("/nonexistent/burrow")};
    SyscallProcAttr attr = {
        BURROW_STR_EMPTY, {NULL, 0, 0, NULL}, {NULL, 0, 0, NULL}, &sys};
    pid = syscall_fork_exec(argv[0], strs(argv, 1), &attr, &e);
    CHECK_INT_EQ(errno_of(e), SYSCALL_ENOENT);
    CHECK_INT_EQ(pidfd, -1);
}

/* pdeathsig is set and the child still runs, which is all that can be checked
 * without the parent dying. */
static void TestPdeathsig(TestingT *t) {
    SyscallSysProcAttr sys = {.pdeathsig = SYSCALL_SIGKILL};
    Error e = BURROW_NO_ERROR;
    Int pid =
        start_child(t, S("exit:6"), &sys, uintptrs((const Uintptr[]){0, 1, 2}, 3), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ForkExec: %s", error_text(e));
    CHECK_INT_EQ(wait_code(t, pid), 6);
}

#define LINUX_TESTS(X)                                                                 \
    X(TestAllThreadsSyscall)                                                           \
    X(TestSetuidEtc)                                                                   \
    X(TestCloneNEWUSERAndRemap)                                                        \
    X(TestUnshareUidGidMapping)                                                        \
    X(TestPidFD)                                                                       \
    X(TestPdeathsig)
#else
#define LINUX_TESTS(X)
#endif

/* ------------------------------------------------------------------ child */

static void child(Str what) {
    Error e = BURROW_NO_ERROR;
    if (strings_has_prefix(what, S("exit:"))) {
        os_exit((Int)(what.p[5] - '0'));
    } else if (str_eq(what, S("wait"))) {
        Byte b[64];
        while (syscall_read(0, (Slice){b, 64, 64, TYPE_BYTE}, &e) > 0 ||
               errno_of(e) == SYSCALL_EINTR)
            e = BURROW_NO_ERROR;
        os_exit(0);
    } else if (str_eq(what, S("pgrp"))) {
        os_file_write_string(os_stdout,
                             fmt_sprintf_v(a, "%d\n", (int)syscall_getpgrp()), &e);
        os_exit(BURROW_FAILED(e) ? 2 : 0);
    } else if (str_eq(what, S("ids"))) {
        Slice groups = os_getgroups(a, &e);
        os_file_write_string(os_stdout,
                             fmt_sprintf_v(a, "%d %d %d\n", (int)syscall_getuid(),
                                           (int)syscall_getgid(), (int)groups.len),
                             &e);
        os_exit(BURROW_FAILED(e) ? 2 : 0);
    } else if (str_eq(what, S("exec"))) {
        Str exe = os_executable(a, &e);
        Str argv[1] = {exe};
        e = syscall_exec(exe, strs(argv, 1), child_env(S("exit:5")));
        os_exit(9);
    }
    os_exit(9);
}

#define UNIX_TESTS(X)                                                                  \
    X(TestZeroSysProcAttr)                                                             \
    X(TestSetpgid)                                                                     \
    X(TestPgid)                                                                        \
    X(TestOsPgid)                                                                      \
    X(TestInvalidExec)                                                                 \
    X(TestForkExecNilArgv)                                                             \
    X(TestForkExecNotFound)                                                            \
    X(TestExec)                                                                        \
    X(TestExecFails)                                                                   \
    X(TestForkExecNilEnv)                                                              \
    X(TestForkExecDir)                                                                 \
    X(TestForkExecKeepsFds)                                                            \
    X(TestStartProcessHandle)                                                          \
    X(TestForkLock)                                                                    \
    X(TestSetgroups)                                                                   \
    X(TestCredential)                                                                  \
    LINUX_TESTS(X)

#else

/* Windows has its own SysProcAttr, with CreationFlags, which os_signal_test
 * uses. */
static void TestSysProcAttrWindows(TestingT *t) {
    (void)t;
    SyscallSysProcAttr sys = {0x200};
    CHECK_INT_EQ(sys.creation_flags, 0x200);
}

static void child(Str what) {
    (void)what;
    os_exit(9);
}

#define UNIX_TESTS(X) X(TestSysProcAttrWindows)

#endif

#define TESTS(X) UNIX_TESTS(X)

static int syscall_exec_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    Str what = os_getenv(a, S(CHILD_VAR));
    if (what.len > 0)
        child(what);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(syscall_exec_main, TESTS)
