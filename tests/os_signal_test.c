/* Derived from Go's src/os/signal/signal_test.go and signal_windows_test.go.
 * Go source: go1.27.1.
 *
 * The child processes some tests start are this program again, run with
 * -test.run naming one test and an environment variable saying what that
 * test should do, which is how Go's versions use their flags.
 *
 * Three things differ from Go because a C program is not a Go program. Go's
 * runtime catches every signal from the start and drops the ones nobody asked
 * for, so its TestStop can send SIGUSR1 before Notify and after Stop and see
 * nothing happen. Here SIGUSR1 does what it does to any C program when nobody
 * has it, which is end it, so those two sends are left out for SIGUSR1 and
 * kept for SIGWINCH, whose default is to do nothing. burrow contexts have no
 * String, so the checks of NotifyContext's String are not here. And there is
 * no runtime/trace to start and stop, so TestSignalTrace is skipped.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/signal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os/exec.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#include "check.h"

#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#endif

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

/* Go's settleTime, and fatalWaitingTime is with the Unix tests. */
static Duration settle_time = 100 * TIME_MILLISECOND;

/* Set by a child that returns with a context still registered, whose watcher
 * may still be using memory from ar when the tests are over. */
static bool keep_arena;

static OsSignal sig_of(SyscallSignal n) {
    return os_signal_from_syscall(n);
}

static bool sig_eq(OsSignal x, OsSignal y) {
    return x.vt == y.vt && x.data == y.data;
}

static Str sig_name(Alloc *al, OsSignal s) {
    if (s.vt == NULL)
        return S("<nil>");
    return s.vt->string(s.data, al);
}

static Chan *sig_chan(Alloc *al, Int cap) {
    return chan_make(al, TYPE_OS_SIGNAL, cap);
}

static Str env_or_empty(Alloc *al, const char *key) {
    return os_getenv(al, str_from_cstr(key));
}

/* This program again, running the test called name with key=value added to
 * its environment. */
static ExecCmd *self_cmd(TestingT *t, Alloc *al, Str prefix, const char *name, Str key,
                         Str value) {
    Error e = BURROW_NO_ERROR;
    Str exe = os_executable(al, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Executable: %s", error_text(e));

    Str run = fmt_sprintf_v(al, "-test.run=^%s$", name);
    ExecCmd *c;
    if (prefix.len > 0)
        c = exec_command_v(al, prefix, 3, exe, S("-test.v"), run);
    else
        c = exec_command_v(al, exe, 2, S("-test.v"), run);

    Slice env = os_environ(al);
    env = BURROW_APPEND(Str, al, env, fmt_sprintf_v(al, "%s=%s", key, value));
    c->env = env;
    return c;
}

/* ------------------------------------------------------------------- tests */

#if !defined(BURROW_OS_WINDOWS)

static const Duration fatal_waiting_time = 30 * TIME_SECOND;

/* The program's own process gets sig. */
static void kill_self(SyscallSignal sig) {
    (void)syscall_kill(os_getpid(), sig);
}

static void wait_sig1(TestingT *t, Alloc *al, Chan *c, OsSignal sig, bool all) {
    Time start = time_now();
    while (time_since(start) < fatal_waiting_time) {
        OsSignal s;
        bool ok = false;
        if (chan_try_recv(c, &s, &ok) && ok) {
            if (sig_eq(s, sig))
                return;
            /* Go filters SIGURG here, which its runtime uses for preemption.
             * Nothing here sends it, and the filter is kept so the test reads
             * the same. */
            if (!all || os_signal_to_syscall(s) != SYSCALL_SIGURG)
                testing_t_fatalf_v(t, "signal was %s, want %s", sig_name(al, s),
                                   sig_name(al, sig));
            continue;
        }
        time_sleep(settle_time / 10);
    }
    testing_t_fatalf_v(t, "timeout waiting for %s", sig_name(al, sig));
}

static void wait_sig(TestingT *t, Alloc *al, Chan *c, OsSignal sig) {
    wait_sig1(t, al, c, sig, false);
}

static void wait_sig_all(TestingT *t, Alloc *al, Chan *c, OsSignal sig) {
    wait_sig1(t, al, c, sig, true);
}

/* Go's quiesce: long enough that a signal sent before it has been delivered,
 * slept in pieces so the kernel has chances to deliver it. */
static void quiesce(void) {
    Time start = time_now();
    while (time_since(start) < settle_time)
        time_sleep(settle_time / 10);
}

static void expect_nothing(TestingT *t, Chan *c) {
    OsSignal s;
    bool ok = false;
    if (chan_try_recv(c, &s, &ok) && ok)
        testing_t_errorf_v(t, "unexpected signal %s", sig_name(a, s));
}

static void TestSignal(TestingT *t) {
    /* Ask for SIGHUP. */
    Chan *c = sig_chan(a, 1);
    signal_notify_v(c, 1, sig_of(SYSCALL_SIGHUP));

    /* Send this process a SIGHUP. */
    testing_t_logf_v(t, "sighup...");
    kill_self(SYSCALL_SIGHUP);
    wait_sig(t, a, c, sig_of(SYSCALL_SIGHUP));

    /* Ask for everything we can get. */
    Chan *c1 = sig_chan(a, 10);
    signal_notify_v(c1, 0);
    signal_reset_v(1, sig_of(SYSCALL_SIGURG));

    /* Send this process a SIGWINCH. */
    testing_t_logf_v(t, "sigwinch...");
    kill_self(SYSCALL_SIGWINCH);
    wait_sig_all(t, a, c1, sig_of(SYSCALL_SIGWINCH));

    /* Two more SIGHUPs, to make sure they get to c1 and that not reading c
     * does not block everything. */
    testing_t_logf_v(t, "sighup...");
    kill_self(SYSCALL_SIGHUP);
    wait_sig_all(t, a, c1, sig_of(SYSCALL_SIGHUP));
    testing_t_logf_v(t, "sighup...");
    kill_self(SYSCALL_SIGHUP);
    wait_sig_all(t, a, c1, sig_of(SYSCALL_SIGHUP));

    /* The first SIGHUP should be waiting on c. */
    wait_sig(t, a, c, sig_of(SYSCALL_SIGHUP));

    signal_stop(c1);
    signal_stop(c);
}

/* The sending half of TestStress and TestTime. */
typedef struct Hammer {
    Chan *sig;
    Chan *stop;   /* closed to stop, for TestTime */
    Duration dur; /* or a time limit, for TestStress */
    Chan *done;
} Hammer;

static void hammer(void *env) {
    Hammer *h = (Hammer *)env;
    Time start = time_now();
    for (;;) {
        bool stop = false;
        if (h->stop != NULL) {
            bool ok = false;
            stop = chan_try_recv(h->stop, NULL, &ok) && !ok;
        } else {
            stop = time_since(start) >= h->dur;
        }
        if (stop) {
            /* Time for every signal to arrive before stopping. */
            quiesce();
            signal_stop(h->sig);
            /* Stop promises that the channel gets nothing once it returns, so
             * closing it here would panic on a send after close if that
             * promise were broken. */
            chan_close(h->sig);
            return;
        }
        kill_self(SYSCALL_SIGUSR1);
        runtime_gosched();
    }
}

static void drain(void *env) {
    Hammer *h = (Hammer *)env;
    OsSignal s;
    while (chan_recv(h->sig, &s)) {
    }
    chan_close(h->done);
}

static void TestStress(TestingT *t) {
    Duration dur = 3 * TIME_SECOND;
    if (testing_short())
        dur = 100 * TIME_MILLISECOND;

    Hammer h = {sig_chan(a, 1), NULL, dur, NULL};
    signal_notify_v(h.sig, 1, sig_of(SYSCALL_SIGUSR1));

    if (!go(BURROW_FN(Func, hammer, &h)))
        testing_t_fatalf_v(t, "go failed");

    OsSignal s;
    while (chan_recv(h.sig, &s)) {
        /* Until the sender closes it. */
    }
}

static void test_cancel(TestingT *t, bool ignore) {
    /* Notified on c1 for SIGWINCH and on c2 for SIGHUP. */
    Chan *c1 = sig_chan(a, 1);
    signal_notify_v(c1, 1, sig_of(SYSCALL_SIGWINCH));
    Chan *c2 = sig_chan(a, 1);
    signal_notify_v(c2, 1, sig_of(SYSCALL_SIGHUP));

    kill_self(SYSCALL_SIGWINCH);
    wait_sig(t, a, c1, sig_of(SYSCALL_SIGWINCH));
    kill_self(SYSCALL_SIGHUP);
    wait_sig(t, a, c2, sig_of(SYSCALL_SIGHUP));

    /* Ignore or reset both. Either way this undoes both Notifies. */
    if (ignore)
        signal_ignore_v(2, sig_of(SYSCALL_SIGWINCH), sig_of(SYSCALL_SIGHUP));
    else
        signal_reset_v(2, sig_of(SYSCALL_SIGWINCH), sig_of(SYSCALL_SIGHUP));

    /* A SIGWINCH should be ignored now, and so should a SIGHUP if ignoring. */
    kill_self(SYSCALL_SIGWINCH);
    if (ignore)
        kill_self(SYSCALL_SIGHUP);

    quiesce();

    expect_nothing(t, c1);
    expect_nothing(t, c2);

    /* Either signal may have been blocked by whoever started us, so take
     * anything still queued now rather than leave it for the next test. */
    signal_notify_v(c1, 1, sig_of(SYSCALL_SIGWINCH));
    signal_notify_v(c2, 1, sig_of(SYSCALL_SIGHUP));
    quiesce();

    signal_stop(c2);
    signal_stop(c1);
}

static void TestReset(TestingT *t) {
    test_cancel(t, false);
}

static void TestIgnore(TestingT *t) {
    test_cancel(t, true);
}

static void TestIgnored(TestingT *t) {
    Chan *c = sig_chan(a, 1);
    signal_notify_v(c, 1, sig_of(SYSCALL_SIGWINCH));

    /* Being notified means not ignored. */
    if (signal_ignored(sig_of(SYSCALL_SIGWINCH)))
        testing_t_errorf_v(t, "expected SIGWINCH to not be ignored.");
    signal_stop(c);
    signal_ignore_v(1, sig_of(SYSCALL_SIGWINCH));

    if (!signal_ignored(sig_of(SYSCALL_SIGWINCH)))
        testing_t_errorf_v(
            t, "expected SIGWINCH to be ignored when explicitly ignoring it.");

    signal_reset_v(0);
}

/* Whether a file exists, for /usr/bin/nohup. */
static bool exists(Str path) {
    Error e = BURROW_NO_ERROR;
    (void)os_stat(a, path, &e);
    return BURROW_OK(e);
}

static void TestDetectNohup(TestingT *t) {
    if (env_or_empty(a, "BURROW_SIGNAL_CHECK_SIGHUP_IGNORED").len > 0) {
        if (!signal_ignored(sig_of(SYSCALL_SIGHUP)))
            testing_t_fatalf_v(t, "SIGHUP is not ignored.");
        else
            testing_t_logf_v(t, "SIGHUP is ignored.");
        return;
    }

    /* Ask for SIGHUP, so that the child does not have it ignored even if this
     * is running under nohup. Nothing reads c. */
    Chan *c = sig_chan(a, 1);
    signal_notify_v(c, 1, sig_of(SYSCALL_SIGHUP));
    ExecCmd *cmd = self_cmd(t, a, BURROW_STR_EMPTY, "TestDetectNohup",
                            S("BURROW_SIGNAL_CHECK_SIGHUP_IGNORED"), S("1"));
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t,
            "ran test with -check_sighup_ignored and it succeeded: expected "
            "failure.\nOutput:\n%s",
            str_from_bytes(out.p, out.len));
    exec_cmd_free(cmd);
    signal_stop(c);

    /* Again under nohup, if there is one. */
    if (!exists(S("/usr/bin/nohup"))) {
        signal_reset_v(0);
        testing_t_skip_v(t, "cannot find nohup; skipping second half of test");
    }
    signal_ignore_v(1, sig_of(SYSCALL_SIGHUP));
    (void)os_remove(S("nohup.out"));
    cmd = self_cmd(t, a, S("/usr/bin/nohup"), "TestDetectNohup",
                   S("BURROW_SIGNAL_CHECK_SIGHUP_IGNORED"), S("1"));
    err = BURROW_NO_ERROR;
    out = exec_cmd_combined_output(cmd, a, &err);
    Error rerr = BURROW_NO_ERROR;
    Slice data = os_read_file(a, S("nohup.out"), &rerr);
    (void)os_remove(S("nohup.out"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(
            t,
            "ran test with -check_sighup_ignored under nohup and it failed: "
            "expected success.\nError: %s\nOutput:\n%s%s",
            error_text(err), str_from_bytes(out.p, out.len),
            str_from_bytes(data.p, data.len));
    exec_cmd_free(cmd);
    signal_reset_v(0);
}

/* What -send_uncaught_sighup is in Go. */
static int send_uncaught_sighup(void) {
    Str v = env_or_empty(a, "BURROW_SIGNAL_SEND_UNCAUGHT_SIGHUP");
    if (v.len == 1 && (v.p[0] == '1' || v.p[0] == '2'))
        return v.p[0] - '0';
    return 0;
}

static void stop_one(void *env, TestingT *t) {
    SyscallSignal sig = (SyscallSignal)(intptr_t)env;
    int send = send_uncaught_sighup();

    /* Signals from different subtests do not get in each other's way, and
     * there is a lot of waiting, so the three run at once. */
    testing_t_parallel(t);

    Arena local;
    arena_init(&local, NULL, 0);
    Alloc *al = arena_allocator(&local);

    /* Go sends the signal before asking for it, to see that its runtime drops
     * it. A C program has no runtime doing that, and SIGUSR1's default is to
     * end the program, so only signals whose default is harmless are sent
     * here, which is SIGWINCH, and SIGHUP when the flag asks for it. */
    bool harmless = sig != SYSCALL_SIGUSR1;
    bool may_have_blocked = false;
    if (harmless && !signal_ignored(sig_of(sig)) &&
        (sig != SYSCALL_SIGHUP || send == 1)) {
        kill_self(sig);
        quiesce();
        /* It may be blocked by whoever started us, so it may still come. */
        may_have_blocked = true;
    }

    Chan *c = sig_chan(al, 1);
    signal_notify_v(c, 1, sig_of(sig));

    kill_self(sig);
    wait_sig(t, al, c, sig_of(sig));

    if (may_have_blocked) {
        /* The first one may have arrived as well as the second, and waitSig
         * may have seen either. Clear the other. */
        quiesce();
        bool ok = false;
        (void)chan_try_recv(c, NULL, &ok);
    }

    /* Stop, and send it again. */
    signal_stop(c);
    if (harmless && (sig != SYSCALL_SIGHUP || send == 2)) {
        kill_self(sig);
        quiesce();
        expect_nothing(t, c);

        /* Take anything that was blocked rather than leave it. */
        signal_notify_v(c, 1, sig_of(sig));
        quiesce();
        signal_stop(c);
    }

    arena_free(&local);
}

static void TestStop(TestingT *t) {
    SyscallSignal sigs[] = {SYSCALL_SIGWINCH, SYSCALL_SIGHUP, SYSCALL_SIGUSR1};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        Str name = syscall_signal_string(sigs[i], a);
        (void)testing_t_run(
            t, name, BURROW_FN(TestingTFunc, stop_one, (void *)(intptr_t)sigs[i]));
    }
}

static void nohup_uncaught(void *env, TestingT *t) {
    int i = (int)(intptr_t)env;
    testing_t_parallel(t);

    Arena local;
    arena_init(&local, NULL, 0);
    Alloc *al = arena_allocator(&local);

    ExecCmd *cmd =
        self_cmd(t, al, BURROW_STR_EMPTY, "TestStop",
                 S("BURROW_SIGNAL_SEND_UNCAUGHT_SIGHUP"), strconv_itoa(al, i));
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, al, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t,
                           "ran test with -send_uncaught_sighup=%d and it succeeded: "
                           "expected failure.\nOutput:\n%s",
                           (Int)i, str_from_bytes(out.p, out.len));
    else
        testing_t_logf_v(
            t,
            "test with -send_uncaught_sighup=%d failed as expected.\nError: "
            "%s\nOutput:\n%s",
            (Int)i, error_text(err), str_from_bytes(out.p, out.len));
    exec_cmd_free(cmd);
    arena_free(&local);
}

static void nohup_nohup(void *env, TestingT *t) {
    int i = (int)(intptr_t)env;
    testing_t_parallel(t);

    Arena local;
    arena_init(&local, NULL, 0);
    Alloc *al = arena_allocator(&local);

    ExecCmd *cmd =
        self_cmd(t, al, S("nohup"), "TestStop", S("BURROW_SIGNAL_SEND_UNCAUGHT_SIGHUP"),
                 strconv_itoa(al, i));
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, al, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "ran test with -send_uncaught_sighup=%d under nohup and it "
                           "failed: expected success.\nError: %s\nOutput:\n%s",
                           (Int)i, error_text(err), str_from_bytes(out.p, out.len));
    else
        testing_t_logf_v(
            t, "ran test with -send_uncaught_sighup=%d under nohup.\nOutput:\n%s",
            (Int)i, str_from_bytes(out.p, out.len));
    exec_cmd_free(cmd);
    arena_free(&local);
}

static void nohup_group(void *env, TestingT *t) {
    bool under_nohup = env != NULL;

    if (under_nohup) {
        Error e = BURROW_NO_ERROR;
        (void)exec_look_path(a, S("nohup"), &e);
        if (BURROW_FAILED(e))
            testing_t_skip_v(t, "cannot find nohup; skipping second half of test");
    }

    for (int i = 1; i <= 2; i++) {
        Str name = strconv_itoa(a, i);
        TestingTFunc f =
            under_nohup ? BURROW_FN(TestingTFunc, nohup_nohup, (void *)(intptr_t)i)
                        : BURROW_FN(TestingTFunc, nohup_uncaught, (void *)(intptr_t)i);
        (void)testing_t_run(t, name, f);
    }
}

static void TestNohup(TestingT *t) {
    /* Without nohup an uncaught SIGHUP ends the child, and under nohup it does
     * not. Either way TestStop in the child catches it while it wants it.
     * send_uncaught_sighup=1 sends before Notify and 2 after Stop. */

    /* Ask for SIGHUP, so the children do not start with it ignored even if
     * this is running under nohup. Nothing reads c. */
    Chan *c = sig_chan(a, 1);
    signal_notify_v(c, 1, sig_of(SYSCALL_SIGHUP));

    (void)testing_t_run(t, S("uncaught"), BURROW_FN(TestingTFunc, nohup_group, NULL));
    signal_stop(c);

    (void)testing_t_run(t, S("nohup"), BURROW_FN(TestingTFunc, nohup_group, (void *)1));
}

static void TestSIGCONT(TestingT *t) {
    Chan *c = sig_chan(a, 1);
    signal_notify_v(c, 1, sig_of(SYSCALL_SIGCONT));
    kill_self(SYSCALL_SIGCONT);
    wait_sig(t, a, c, sig_of(SYSCALL_SIGCONT));
    signal_stop(c);
}

/* atomicStopTestProgram's goroutine. */
typedef struct AtomicStop {
    Chan *cs;
    SyncWaitGroup wg;
} AtomicStop;

static void atomic_stop_go(void *env) {
    AtomicStop *s = (AtomicStop *)env;
    signal_stop(s->cs);
    sync_wait_group_done(&s->wg);
}

/* Run in a child by TestAtomicStop. Each try either gets the signal or dies
 * from it. Neither is a dropped signal. */
static void atomic_stop_test_program(TestingT *t) {
    (void)t;
    if (signal_ignored(sig_of(SYSCALL_SIGINT))) {
        fmt_println_v(S("SIGINT is ignored"));
        os_exit(1);
    }

    enum { TRIES = 10 };
    const Duration timeout = 2 * TIME_SECOND;
    bool printed = false;

    for (int i = 0; i < TRIES; i++) {
        AtomicStop *s = BURROW_NEW(a, AtomicStop);
        s->cs = sig_chan(a, 1);
        signal_notify_v(s->cs, 1, sig_of(SYSCALL_SIGINT));

        sync_wait_group_add(&s->wg, 1);
        if (!go(BURROW_FN(Func, atomic_stop_go, s)))
            os_exit(3);

        kill_self(SYSCALL_SIGINT);

        /* Now the program either dies from SIGINT or gets it on cs. */
        Chan *after = time_after_chan(a, timeout);
        SelectCase cases[2];
        cases[0] = BURROW_RECV(s->cs, NULL);
        cases[1] = BURROW_RECV(after, NULL);
        if (chan_select(cases, 2) == 1) {
            if (!printed) {
                fmt_print_v(S("lost signal on tries:"));
                printed = true;
            }
            fmt_printf_v(" %d", (Int)i);
        }

        sync_wait_group_wait(&s->wg);
    }
    if (printed)
        fmt_print_v(S("\n"));

    os_exit(0);
}

static void TestAtomicStop(TestingT *t) {
    if (env_or_empty(a, "BURROW_SIGNAL_ATOMIC_STOP").len > 0) {
        atomic_stop_test_program(t);
        testing_t_fatalf_v(t, "atomicStopTestProgram returned");
    }

    /* qemu's user mode emulation can crash a guest that changes a signal's
     * disposition on one thread while the signal is being delivered on
     * another, which is the race this test is made of. A twenty line C
     * program doing only that died with SIGSEGV once in 120 runs under
     * qemu-s390x 8.2.2, and never in 400 runs on the host. CI sets this for
     * the jobs that run under emulation. */
    if (env_or_empty(a, "BURROW_TEST_EMULATED").len > 0)
        testing_t_skip_v(t, "qemu user mode loses this race on its own");

    /* Notify for SIGINT before starting the children, so that SIGINT is not
     * ignored in them, which would be a third outcome the child does not
     * expect. */
    Chan *cs = sig_chan(a, 1);
    signal_notify_v(cs, 1, sig_of(SYSCALL_SIGINT));

    enum { EXECS = 10 };
    for (Int i = 0; i < EXECS; i++) {
        ExecCmd *cmd = self_cmd(t, a, BURROW_STR_EMPTY, "TestAtomicStop",
                                S("BURROW_SIGNAL_ATOMIC_STOP"), S("1"));
        Error err = BURROW_NO_ERROR;
        Slice out = exec_cmd_combined_output(cmd, a, &err);
        Str outs = str_from_bytes(out.p, out.len);
        if (BURROW_OK(err)) {
            if (out.len > 0)
                testing_t_logf_v(t, "iteration %d: output %s", i, outs);
        } else {
            testing_t_logf_v(t, "iteration %d: exit status %q: output: %s", i,
                             error_text(err), outs);
        }

        bool lost = strings_contains(outs, S("lost signal"));
        if (lost)
            testing_t_errorf_v(t, "iteration %d: lost signal", i);

        /* Either it died from SIGINT, or it exited 0 without saying it lost
         * one. The test's own -test.v lines are expected output here, unlike
         * Go's, which runs the child without -test.v. */
        if (BURROW_FAILED(err)) {
            const ExecExitError *ee =
                (const ExecExitError *)errors_as(err, TYPE_EXEC_EXIT_ERROR);
            if (ee == NULL) {
                testing_t_errorf_v(t, "iteration %d: error (%s) is not an ExitError", i,
                                   error_text(err));
            } else {
                SyscallWaitStatus ws = os_process_state_sys(ee->process_state);
                if (!syscall_wait_status_signaled(ws) ||
                    syscall_wait_status_signal(ws) != SYSCALL_SIGINT)
                    testing_t_errorf_v(
                        t, "iteration %d: got exit status %s; expected SIGINT", i,
                        error_text(err));
            }
        }
        exec_cmd_free(cmd);
    }

    signal_stop(cs);
}

static void TestTime(TestingT *t) {
    /* That signals work while the program is reading the clock, which some
     * systems do through the vDSO. Go's issue 34391. */
    Duration dur = 3 * TIME_SECOND;
    if (testing_short())
        dur = 100 * TIME_MILLISECOND;

    Hammer h = {sig_chan(a, 1), chan_make(a, TYPE_UINT8, 0), 0,
                chan_make(a, TYPE_UINT8, 0)};
    signal_notify_v(h.sig, 1, sig_of(SYSCALL_SIGUSR1));

    if (!go(BURROW_FN(Func, hammer, &h)) || !go(BURROW_FN(Func, drain, &h)))
        testing_t_fatalf_v(t, "go failed");

    Time t0 = time_now();
    for (Time t1 = t0; time_sub(t1, t0) < dur; t1 = time_now()) {
        /* Hammering on getting the time. */
    }

    chan_close(h.stop);
    (void)chan_recv(h.done, NULL);
}

/* The goroutines TestNotifyContextNotifications's child starts. */
typedef struct Kills {
    SyncWaitGroup wg;
} Kills;

static void kill_int_go(void *env) {
    Kills *k = (Kills *)env;
    kill_self(SYSCALL_SIGINT);
    sync_wait_group_done(&k->wg);
}

typedef struct NotifyCase {
    const char *name;
    Int n;
} NotifyCase;

static void notify_ctx_case(void *env, TestingT *t) {
    const NotifyCase *tc = (const NotifyCase *)env;
    testing_t_parallel(t);

    Arena local;
    arena_init(&local, NULL, 0);
    Alloc *al = arena_allocator(&local);

    ExecCmd *cmd =
        self_cmd(t, al, BURROW_STR_EMPTY, "TestNotifyContextNotifications",
                 S("BURROW_SIGNAL_CHECK_NOTIFY_CTX"), strconv_itoa(al, tc->n));
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, al, &err);
    Str outs = str_from_bytes(out.p, out.len);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(
            t,
            "ran test with -check_notify_ctx_notification and it failed with "
            "%s.\nOutput:\n%s",
            error_text(err), outs);
    if (!strings_contains(outs, S("received SIGINT\n")))
        testing_t_errorf_v(t, "got %q, wanted %q", outs, S("received SIGINT\n"));
    exec_cmd_free(cmd);
    arena_free(&local);
}

static void TestNotifyContextNotifications(TestingT *t) {
    Str times = env_or_empty(a, "BURROW_SIGNAL_CHECK_NOTIFY_CTX");
    if (times.len > 0) {
        Error e = BURROW_NO_ERROR;
        Int n = strconv_atoi(times, &e);
        Context ctx = signal_notify_context_v(a, context_background(), NULL, 1,
                                              sig_of(SYSCALL_SIGINT));

        /* Several signals, so that a stop called from inside the package when
         * the first arrives would show up as the program dying from a later
         * one. */
        Kills k;
        memset(&k, 0, sizeof k);
        sync_wait_group_add(&k.wg, n);
        for (Int i = 0; i < n; i++)
            if (!go(BURROW_FN(Func, kill_int_go, &k)))
                testing_t_fatalf_v(t, "go failed");
        sync_wait_group_wait(&k.wg);

        (void)chan_recv(context_done(ctx), NULL);
        Str got = error_text(context_cause(ctx));
        if (!str_eq(got, S("interrupt signal received")))
            testing_t_errorf_v(t, "context.Cause(ctx) = %q, want %q", got,
                               S("interrupt signal received"));
        fmt_println_v(S("received SIGINT"));

        /* Time for the other signals to get here. stop is not called, so
         * they have to be taken and dropped, not end the program. */
        time_sleep(settle_time);
        keep_arena = true;
        return;
    }

    testing_t_parallel(t);
    static const NotifyCase cases[] = {{"once", 1}, {"multiple", 10}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        (void)testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, notify_ctx_case, (void *)(uintptr_t)&cases[i]));
}

static void TestNotifyContextCause(TestingT *t) {
    ContextCancelFunc stop;
    Context ctx = signal_notify_context_v(a, testing_t_context(t), &stop, 1,
                                          sig_of(SYSCALL_SIGINT));
    kill_self(SYSCALL_SIGINT);
    (void)chan_recv(context_done(ctx), NULL);

    Error err = context_err(ctx);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ctx.Err returned nil");
    else if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "error %s is not context.Canceled", error_text(err));

    err = context_cause(ctx);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "context.Cause returned nil");
    else if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "cause %s is not context.Canceled", error_text(err));

    BURROW_CALLF0(stop);
    context_release(ctx);
}

static void TestSignalTrace(TestingT *t) {
    testing_t_skip_v(t, "burrow has no runtime/trace to start and stop");
}

#endif /* !BURROW_OS_WINDOWS */

/* The NotifyContext tests that send no signal. Go runs them on Unix only, as
 * the whole file is, and nothing in them needs Unix, so they run on Windows
 * too. */

static void TestNotifyContextStop(TestingT *t) {
    signal_ignore_v(1, sig_of(SYSCALL_SIGHUP));
    if (!signal_ignored(sig_of(SYSCALL_SIGHUP)))
        testing_t_errorf_v(
            t, "expected SIGHUP to be ignored when explicitly ignoring it.");

    ContextCancelFunc cancel_parent;
    Context parent = context_with_cancel(a, context_background(), &cancel_parent);
    ContextCancelFunc stop;
    Context c = signal_notify_context_v(a, parent, &stop, 1, sig_of(SYSCALL_SIGHUP));

    /* Being notified means not ignored. */
    if (signal_ignored(sig_of(SYSCALL_SIGHUP)))
        testing_t_errorf_v(t, "expected SIGHUP to not be ignored.");

    BURROW_CALLF0(stop);
    (void)chan_recv(context_done(c), NULL);
    Error got = context_err(c);
    if (got.vt != context_canceled.vt || got.data != context_canceled.data)
        testing_t_errorf_v(t, "c.Err() = %q, want %q", error_text(got),
                           error_text(context_canceled));
    got = context_cause(c);
    if (got.vt != context_canceled.vt || got.data != context_canceled.data)
        testing_t_errorf_v(t, "context.Cause(c.Err()) = %q, want %q", error_text(got),
                           error_text(context_canceled));

    /* Go's deferred stop, then the frees Go leaves to its collector. */
    BURROW_CALLF0(stop);
    context_release(c);
    BURROW_CALLF0(cancel_parent);
    context_release(parent);
}

static void TestNotifyContextCancelParent(TestingT *t) {
    ContextCancelCauseFunc cancel_parent;
    Context parent = context_with_cancel_cause(a, context_background(), &cancel_parent);
    Error parent_cause = errors_new(a, S("parent canceled"));
    ContextCancelFunc stop;
    Context c = signal_notify_context_v(a, parent, &stop, 1, sig_of(SYSCALL_SIGINT));

    BURROW_CALLF(cancel_parent, parent_cause);
    (void)chan_recv(context_done(c), NULL);
    Error got = context_err(c);
    if (got.vt != context_canceled.vt || got.data != context_canceled.data)
        testing_t_errorf_v(t, "c.Err() = %q, want %q", error_text(got),
                           error_text(context_canceled));
    got = context_cause(c);
    if (got.vt != parent_cause.vt || got.data != parent_cause.data)
        testing_t_errorf_v(t, "context.Cause(c) = %q, want %q", error_text(got),
                           error_text(parent_cause));

    BURROW_CALLF0(stop);
    context_release(c);
    context_release(parent);
}

static void TestNotifyContextPrematureCancelParent(TestingT *t) {
    ContextCancelCauseFunc cancel_parent;
    Context parent = context_with_cancel_cause(a, context_background(), &cancel_parent);
    Error parent_cause = errors_new(a, S("parent canceled"));

    /* Cancelled before NotifyContext is called. */
    BURROW_CALLF(cancel_parent, parent_cause);
    ContextCancelFunc stop;
    Context c = signal_notify_context_v(a, parent, &stop, 1, sig_of(SYSCALL_SIGINT));

    (void)chan_recv(context_done(c), NULL);
    Error got = context_err(c);
    if (got.vt != context_canceled.vt || got.data != context_canceled.data)
        testing_t_errorf_v(t, "c.Err() = %q, want %q", error_text(got),
                           error_text(context_canceled));
    got = context_cause(c);
    if (got.vt != parent_cause.vt || got.data != parent_cause.data)
        testing_t_errorf_v(t, "context.Cause(c) = %q, want %q", error_text(got),
                           error_text(parent_cause));

    BURROW_CALLF0(stop);
    context_release(c);
    context_release(parent);
}

typedef struct Stops {
    ContextCancelFunc stop;
    SyncWaitGroup wg;
} Stops;

static void stop_go(void *env) {
    Stops *s = (Stops *)env;
    BURROW_CALLF0(s->stop);
    sync_wait_group_done(&s->wg);
}

static void TestNotifyContextSimultaneousStop(TestingT *t) {
    Stops s;
    memset(&s, 0, sizeof s);
    Context c = signal_notify_context_v(a, context_background(), &s.stop, 1,
                                        sig_of(SYSCALL_SIGINT));

    enum { N = 10 };
    sync_wait_group_add(&s.wg, N);
    for (int i = 0; i < N; i++)
        if (!go(BURROW_FN(Func, stop_go, &s)))
            testing_t_fatalf_v(t, "go failed");
    sync_wait_group_wait(&s.wg);
    (void)chan_recv(context_done(c), NULL);
    Error got = context_err(c);
    if (got.vt != context_canceled.vt || got.data != context_canceled.data)
        testing_t_errorf_v(t, "c.Err() = %q, want %q", error_text(got),
                           error_text(context_canceled));

    BURROW_CALLF0(s.stop);
    context_release(c);
}

static void TestNotifyContextStringer(TestingT *t) {
    testing_t_skip_v(t, "burrow contexts have no String");
}

/* Not Go's: freeing the context without calling stop takes the registration
 * away with it, and the signal goes back to what it did before. */
static void TestNotifyContextRelease(TestingT *t) {
    Context c = signal_notify_context_v(a, context_background(), NULL, 1,
                                        sig_of(SYSCALL_SIGHUP));
    if (signal_ignored(sig_of(SYSCALL_SIGHUP)))
        testing_t_errorf_v(t, "expected SIGHUP to not be ignored.");
    context_release(c);

    /* The same channel can be asked for SIGHUP again, and gets it. */
    Chan *ch = sig_chan(a, 1);
    signal_notify_v(ch, 1, sig_of(SYSCALL_SIGHUP));
#if !defined(BURROW_OS_WINDOWS)
    kill_self(SYSCALL_SIGHUP);
    wait_sig(t, a, ch, sig_of(SYSCALL_SIGHUP));
#endif
    signal_stop(ch);
}

/* Not Go's either: the checks Notify makes before anything else. */
static void TestNotifyBadChannel(TestingT *t) {
    Chan *wrong = chan_make(a, TYPE_INT, 1);
    volatile bool panicked = false;
    BURROW_TRY {
        signal_notify_v(wrong, 1, os_interrupt);
    }
    BURROW_CATCH(p) {
        (void)p;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "Notify with a chan int did not panic");

    panicked = false;
    BURROW_TRY {
        signal_notify_v(NULL, 1, os_interrupt);
    }
    BURROW_CATCH(p2) {
        (void)p2;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "Notify with a nil channel did not panic");
}

#if defined(BURROW_OS_WINDOWS)

/* The child TestCtrlBreak starts: notified for everything, it says it is
 * ready and waits up to three seconds for Ctrl-Break. */
static void ctrl_break_child(TestingT *t) {
    (void)t;
    Chan *c = sig_chan(a, 10);
    signal_notify_v(c, 0);
    /* Straight to the file, since stdio holds on to what it is given when
     * stdout is a pipe. */
    fmt_fprintln_v(os_file_as_io_writer(os_stdout), S("ready"));

    Chan *after = time_after_chan(a, 3 * TIME_SECOND);
    OsSignal s;
    SelectCase cases[2];
    cases[0] = BURROW_RECV(c, &s);
    cases[1] = BURROW_RECV(after, NULL);
    if (chan_select(cases, 2) == 1) {
        fmt_fprintln_v(os_file_as_io_writer(os_stderr),
                       S("Timeout waiting for Ctrl+Break"));
        os_exit(1);
    }
    if (!sig_eq(s, os_interrupt)) {
        fmt_fprintf_v(os_file_as_io_writer(os_stderr),
                      "Wrong signal received: got %q, want %q\n", sig_name(a, s),
                      sig_name(a, os_interrupt));
        os_exit(1);
    }
    os_exit(0);
}

static void TestCtrlBreak(TestingT *t) {
    if (env_or_empty(a, "BURROW_SIGNAL_CTRLBREAK").len > 0)
        ctrl_break_child(t);

    /* Wine says GenerateConsoleCtrlEvent worked and the child never hears of
     * it, and plain Win32 code does the same there, so the test is for real
     * Windows. Without a console there is nothing to send the event through. */
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL)
        testing_t_skip_v(t,
                         "wine does not deliver Ctrl-Break to another process group");
    if (GetConsoleWindow() == NULL)
        testing_t_skip_v(t, "no console to send Ctrl-Break through");

    ExecCmd *cmd = self_cmd(t, a, BURROW_STR_EMPTY, "TestCtrlBreak",
                            S("BURROW_SIGNAL_CTRLBREAK"), S("1"));
    static const SyscallSysProcAttr attr = {.creation_flags = CREATE_NEW_PROCESS_GROUP};
    cmd->sys_proc_attr = &attr;

    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    cmd->stderr_ = bytes_buffer_as_io_writer(&buf);
    Error err = BURROW_NO_ERROR;
    IoReadCloser stdout_ = exec_cmd_stdout_pipe(cmd, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "StdoutPipe failed: %s", error_text(err));
    err = exec_cmd_start(cmd);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Start failed: %s", error_text(err));

    /* Read the child's output up to "ready", skipping the lines -test.v
     * prints before it. */
    Slice line = slice_make(a, TYPE_BYTE, 0, 64);
    for (;;) {
        Byte ch;
        Error e = BURROW_NO_ERROR;
        Int n =
            stdout_.vt->reader.read(stdout_.data, slice_from(&ch, 1, 1, TYPE_BYTE), &e);
        if (n == 1) {
            if (ch != '\n') {
                line = BURROW_APPEND(Byte, a, line, ch);
                continue;
            }
            Str l = strings_trim_space(str_from_bytes(line.p, line.len));
            if (str_eq(l, S("ready")))
                break;
            line.len = 0;
            continue;
        }
        if (BURROW_FAILED(e)) {
            Slice b = bytes_buffer_bytes(&buf);
            testing_t_fatalf_v(t, "could not read stdout: %s\n%s", error_text(e),
                               str_from_bytes(b.p, b.len));
        }
    }

    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)cmd->process->pid))
        testing_t_fatalf_v(t, "GenerateConsoleCtrlEvent: %d", (Int)GetLastError());

    err = exec_cmd_wait(cmd);
    if (BURROW_FAILED(err)) {
        Slice b = bytes_buffer_bytes(&buf);
        testing_t_fatalf_v(t, "Program exited with error: %s\n%s", error_text(err),
                           str_from_bytes(b.p, b.len));
    }
    bytes_buffer_free(&buf);
    exec_cmd_free(cmd);
}

#endif /* BURROW_OS_WINDOWS */

#if !defined(BURROW_OS_WINDOWS)
#define PLATFORM_TESTS(X)                                                              \
    X(TestSignal)                                                                      \
    X(TestStress)                                                                      \
    X(TestReset)                                                                       \
    X(TestIgnore)                                                                      \
    X(TestIgnored)                                                                     \
    X(TestDetectNohup)                                                                 \
    X(TestStop)                                                                        \
    X(TestNohup)                                                                       \
    X(TestSIGCONT)                                                                     \
    X(TestAtomicStop)                                                                  \
    X(TestTime)                                                                        \
    X(TestNotifyContextNotifications)                                                  \
    X(TestSignalTrace)                                                                 \
    X(TestNotifyContextCause)
#else
#define PLATFORM_TESTS(X) X(TestCtrlBreak)
#endif

#define TESTS(X)                                                                       \
    PLATFORM_TESTS(X)                                                                  \
    X(TestNotifyContextStop)                                                           \
    X(TestNotifyContextCancelParent)                                                   \
    X(TestNotifyContextPrematureCancelParent)                                          \
    X(TestNotifyContextSimultaneousStop)                                               \
    X(TestNotifyContextStringer)                                                       \
    X(TestNotifyContextRelease)                                                        \
    X(TestNotifyBadChannel)

static int os_signal_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);

    /* Go's GO_TEST_TIMEOUT_SCALE, for slow machines. */
    Str scale = env_or_empty(a, "GO_TEST_TIMEOUT_SCALE");
    if (scale.len > 0) {
        Error e = BURROW_NO_ERROR;
        Int n = strconv_atoi(scale, &e);
        if (BURROW_OK(e) && n > 0)
            settle_time *= n;
    }

    int r = testing_m_run(m);
    if (!keep_arena)
        arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_signal_main, TESTS)
