/* The process group of the platform layer, and mapping a file.
 *
 * The children are this binary again, started with "child" and a mode as its
 * first two arguments, so every platform runs the same program on both sides
 * and nothing depends on which shell or tools the machine has. What is checked
 * is what burrow/pal.h promises and what os/exec and Go's fuzzing workers will
 * build on: arguments arrive byte for byte however they are quoted, the child
 * gets exactly the descriptors and environment it was given, an exit status
 * reads the same on every system, and a shared mapping is shared.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/atomic.h"
#include "burrow/pal.h"
#include "burrow/testing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char *self_path;

/* ------------------------------------------------------------ the children */

static uint32_t interrupted;

static bool on_interrupt(int32_t sig, void *info, void *ctx) {
    (void)info;
    (void)ctx;
    burrow__atomic_store_u32(&interrupted, (uint32_t)sig);
    return true;
}

static int child(int argc, char **argv) {
    const char *mode = argv[2];
    if (strcmp(mode, "exit") == 0)
        return (int)strtol(argv[3], NULL, 10);
    if (strcmp(mode, "args") == 0) {
#if defined(_WIN32)
        /* The C runtime hands main its arguments in the ANSI code page, which
         * loses whatever that page cannot spell, so the child reads the UTF-16
         * command line itself. shell32 is loaded by name to keep it off the
         * link line of every test. */
        (void)argc;
        typedef LPWSTR *(WINAPI * Split)(LPCWSTR, int *);
        HMODULE shell = LoadLibraryW(L"shell32.dll");
        Split split =
            shell != NULL
                ? (Split)(void (*)(void))GetProcAddress(shell, "CommandLineToArgvW")
                : NULL;
        int wargc = 0;
        LPWSTR *wargv = split != NULL ? split(GetCommandLineW(), &wargc) : NULL;
        if (wargv == NULL)
            return 5;
        for (int i = 3; i < wargc; i++) {
            char u[512];
            int n =
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u, sizeof u, NULL, NULL);
            printf("[%s]\n", n > 0 ? u : "?");
        }
        LocalFree(wargv);
        return 0;
#else
        for (int i = 3; i < argc; i++)
            printf("[%s]\n", argv[i]);
        return 0;
#endif
    }
    if (strcmp(mode, "env") == 0) {
        const char *v = getenv(argv[3]);
        printf("%s\n", v != NULL ? v : "(unset)");
        return 0;
    }
    if (strcmp(mode, "cwd") == 0) {
        char buf[1024];
        if (pal_getcwd(buf, sizeof buf, NULL) < 0)
            return 2;
        printf("%s\n", buf);
        return 0;
    }
    if (strcmp(mode, "pid") == 0) {
        printf("%lld\n", (long long)pal_getpid());
        return 0;
    }
    if (strcmp(mode, "cat") == 0) {
        int c;
        while ((c = getchar()) != EOF)
            putchar(c);
        return 0;
    }
    if (strcmp(mode, "fd3") == 0) {
        /* Only on POSIX, where the table is the child's descriptor numbers. */
        const char msg[] = "three\n";
        return pal_write(3, msg, sizeof msg - 1, NULL) == (int64_t)(sizeof msg - 1) ? 0
                                                                                    : 4;
    }
    if (strcmp(mode, "interrupt") == 0) {
        /* Says it is ready once the handler is in, then waits up to ten
         * seconds for the parent's interrupt to reach it. */
        if (!pal_signal_install(PAL_SIGINT, on_interrupt, NULL))
            return 2;
        printf("ready\n");
        fflush(stdout);
        for (int i = 0; i < 1000 && burrow__atomic_load_u32(&interrupted) == 0; i++)
            pal_nanosleep(10000000);
        printf("caught %u\n", burrow__atomic_load_u32(&interrupted));
        return burrow__atomic_load_u32(&interrupted) == PAL_SIGINT ? 0 : 3;
    }
    if (strcmp(mode, "exit-now") == 0) {
        printf("buffered and never flushed");
        pal_exit(7);
    }
    return 99;
}

/* ------------------------------------------------------------- the parent */

typedef struct Run {
    int64_t pid;
    int32_t status;
    char out[4096];
    int64_t nout;
} Run;

/* Starts this binary as a child in the given mode, with its standard output on
 * a pipe that is read to the end, and waits for it. */
static bool run(TestingT *t, const char *const *args, const char *const *envp,
                const char *dir, Run *r) {
    const char *argv[16];
    int n = 0;
    argv[n++] = self_path;
    argv[n++] = "child";
    for (int i = 0; args[i] != NULL && n < 15; i++)
        argv[n++] = args[i];
    argv[n] = NULL;

    PalErrno err = PAL_OK;
    int64_t p[2];
    if (!pal_pipe(p, 0, &err)) {
        testing_t_errorf_v(t, "pipe: %s", pal_errno_string(err));
        return false;
    }
    int64_t fds[3] = {PAL_INVALID_HANDLE, p[1], p[1]};
    PalSpawn req = {self_path, argv, envp, dir, fds, 3, 0};
    r->pid = pal_spawn(&req, &err);
    pal_close(p[1], NULL);
    if (r->pid < 0) {
        pal_close(p[0], NULL);
        testing_t_errorf_v(t, "spawn %s: %s", argv[2], pal_errno_string(err));
        return false;
    }
    r->nout = 0;
    for (;;) {
        int64_t got = pal_read(p[0], r->out + r->nout,
                               (int64_t)sizeof r->out - 1 - r->nout, &err);
        if (got <= 0)
            break;
        r->nout += got;
    }
    r->out[r->nout] = 0;
    pal_close(p[0], NULL);
    /* Windows writes a newline as \r\n through stdio, and that is not what any
     * of these checks are about. */
    int64_t w = 0;
    for (int64_t i = 0; i < r->nout; i++) {
        if (r->out[i] != '\r')
            r->out[w++] = r->out[i];
    }
    r->nout = w;
    r->out[w] = 0;
    if (pal_wait(r->pid, &r->status, 0, &err) != r->pid) {
        testing_t_errorf_v(t, "wait: %s", pal_errno_string(err));
        return false;
    }
    return true;
}

static void TestExitStatus(TestingT *t) {
    static const char *const codes[] = {"0", "3", "200"};
    static const int32_t want[] = {0, 3, 200};
    for (size_t i = 0; i < 3; i++) {
        const char *args[] = {"exit", codes[i], NULL};
        Run r;
        if (run(t, args, NULL, NULL, &r) && r.status != want[i])
            testing_t_errorf_v(t, "exit %s gave status %d", codes[i], r.status);
    }
}

static void TestExitSkipsBufferedOutput(TestingT *t) {
    const char *args[] = {"exit-now", NULL};
    Run r;
    if (!run(t, args, NULL, NULL, &r))
        return;
    if (r.status != 7)
        testing_t_errorf_v(t, "status %d, want 7", r.status);
    if (r.nout != 0)
        testing_t_errorf_v(t, "pal_exit flushed stdio: %q", r.out);
}

/* The quoting cases are Go's, from TestEscapeArg and the ones that have had
 * CVEs, because on Windows the child splits one string back into these. */
static void TestArgumentsArriveIntact(TestingT *t) {
    const char *args[] = {"args",
                          "plain",
                          "",
                          "two words",
                          "tab\there",
                          "quote\"inside",
                          "trailing\\",
                          "back\\slash",
                          "\\\\server\\share\\",
                          "both \\\"x\\\"",
                          "ends in space ",
                          "héllo",
                          NULL};
    Run r;
    if (!run(t, args, NULL, NULL, &r))
        return;
    char want[1024];
    size_t o = 0;
    for (int i = 1; args[i] != NULL; i++)
        o += (size_t)snprintf(want + o, sizeof want - o, "[%s]\n", args[i]);
    if (r.status != 0 || strcmp(r.out, want) != 0)
        testing_t_errorf_v(t, "status %d, output\n%s\nwant\n%s", r.status, r.out, want);
}

static void TestEnvironment(TestingT *t) {
    const char *const *env = pal_environ();
    bool path = false;
    for (int i = 0; env[i] != NULL; i++)
        path = path || strncmp(env[i], "PATH=", 5) == 0 ||
               strncmp(env[i], "Path=", 5) == 0;
    if (!path)
        testing_t_errorf_v(t, "pal_environ has no PATH");

    /* Everything we have plus one, since a Windows child without SystemRoot
     * cannot load the libraries it needs. */
    const char *envp[512];
    int n = 0;
    for (int i = 0; env[i] != NULL && n < 510; i++) {
        if (strncmp(env[i], "BURROW_PROC_TEST=", 17) != 0)
            envp[n++] = env[i];
    }
    envp[n++] = "BURROW_PROC_TEST=hello there";
    envp[n] = NULL;
    const char *args[] = {"env", "BURROW_PROC_TEST", NULL};
    Run r;
    if (run(t, args, envp, NULL, &r) && strcmp(r.out, "hello there\n") != 0)
        testing_t_errorf_v(t, "child saw %q", r.out);

    /* NULL is ours, which does not have it. */
    if (run(t, args, NULL, NULL, &r) && strcmp(r.out, "(unset)\n") != 0)
        testing_t_errorf_v(t, "child of an inherited environment saw %q", r.out);
}

static const char *temp_root(void) {
    const char *names[] = {"TMPDIR", "TEMP", "TMP"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (v != NULL && v[0] != '\0')
            return v;
    }
    return "/tmp";
}

/* The last element of a path, without trailing separators, which is enough
 * to tell two directories apart when one side may have resolved a symlink. */
static void base_of(const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\' || path[n - 1] == '\n'))
        n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/' && path[s - 1] != '\\')
        s--;
    snprintf(out, cap, "%.*s", (int)(n - s), path + s);
}

static void TestWorkingDirectory(TestingT *t) {
    const char *args[] = {"cwd", NULL};
    Run r;
    if (!run(t, args, NULL, temp_root(), &r))
        return;
    char got[256], want[256];
    base_of(r.out, got, sizeof got);
    base_of(temp_root(), want, sizeof want);
    if (strcmp(got, want) != 0)
        testing_t_errorf_v(t, "child ran in %q, want a directory named %q", r.out,
                           want);
}

static void TestMissingProgram(TestingT *t) {
    const char *argv[] = {"nope", NULL};
    PalSpawn req = {"/no/such/program", argv, NULL, NULL, NULL, 0, 0};
    PalErrno err = PAL_OK;
    if (pal_spawn(&req, &err) != -1 || err != PAL_ENOENT)
        testing_t_errorf_v(t, "spawn of a missing program: %s, want ENOENT",
                           pal_errno_string(err));
    const char *full[] = {self_path, "child", "cwd", NULL};
    PalSpawn bad_dir = {self_path, full, NULL, "/no/such/dir", NULL, 0, 0};
    err = PAL_OK;
    if (pal_spawn(&bad_dir, &err) != -1 || err != PAL_ENOENT)
        testing_t_errorf_v(t, "spawn into a missing directory: %s, want ENOENT",
                           pal_errno_string(err));
}

static void TestWaitNoHangAndKill(TestingT *t) {
    PalErrno err = PAL_OK;
    int64_t in[2];
    if (!pal_pipe(in, 0, &err))
        testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
    const char *argv[] = {self_path, "child", "cat", NULL};
    int64_t fds[1] = {in[0]};
    PalSpawn req = {self_path, argv, NULL, NULL, fds, 1, 0};
    int64_t pid = pal_spawn(&req, &err);
    pal_close(in[0], NULL);
    if (pid < 0) {
        pal_close(in[1], NULL);
        testing_t_fatalf_v(t, "spawn: %s", pal_errno_string(err));
    }
    int32_t status = -1;
    if (pal_wait(pid, &status, PAL_WAIT_NOHANG, &err) != 0)
        testing_t_errorf_v(t, "a child blocked on its input was already done");
    if (!pal_kill(pid, 0, &err))
        testing_t_errorf_v(t, "signal 0 to a live child: %s", pal_errno_string(err));
    if (!pal_kill(pid, PAL_SIGKILL, &err))
        testing_t_errorf_v(t, "kill: %s", pal_errno_string(err));
    if (pal_wait(pid, &status, 0, &err) != pid)
        testing_t_errorf_v(t, "wait: %s", pal_errno_string(err));
#if defined(_WIN32)
    int32_t want = 1; /* Go's exit code for a killed process on Windows */
#else
    int32_t want = 128 + PAL_SIGKILL;
#endif
    if (status != want)
        testing_t_errorf_v(t, "a killed child has status %d, want %d", status, want);
    pal_close(in[1], NULL);
}

/* PAL_WAIT_SIGNAL: a child a signal ended says which, rather than 128 plus
 * it, so that a caller can tell that from a child that exited with such a
 * code. */
static void TestWaitSignal(TestingT *t) {
#if defined(_WIN32)
    testing_t_skip_v(t, "Windows has no signals to report");
#else
    PalErrno err = PAL_OK;
    int64_t in[2];
    if (!pal_pipe(in, 0, &err))
        testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
    const char *argv[] = {self_path, "child", "cat", NULL};
    int64_t fds[1] = {in[0]};
    PalSpawn req = {self_path, argv, NULL, NULL, fds, 1, 0};
    int64_t pid = pal_spawn(&req, &err);
    pal_close(in[0], NULL);
    if (pid < 0) {
        pal_close(in[1], NULL);
        testing_t_fatalf_v(t, "spawn: %s", pal_errno_string(err));
    }
    if (!pal_kill(pid, PAL_SIGTERM, &err))
        testing_t_errorf_v(t, "kill: %s", pal_errno_string(err));
    int32_t status = 0;
    int64_t got;
    /* NOHANG until it is gone, which is how the fuzz coordinator reaps. */
    while ((got = pal_wait(pid, &status, PAL_WAIT_NOHANG | PAL_WAIT_SIGNAL, &err)) == 0)
        pal_nanosleep(1000000);
    if (got != pid)
        testing_t_errorf_v(t, "wait: %s", pal_errno_string(err));
    if (status != -PAL_SIGTERM)
        testing_t_errorf_v(t, "a terminated child has status %d, want %d", status,
                           -PAL_SIGTERM);
    pal_close(in[1], NULL);
#endif
}

/* A handler for PAL_SIGINT sees the interrupt and the process carries on,
 * which is what -test.fuzz needs to stop cleanly on Ctrl-C. On Windows the
 * interrupt is a Ctrl-Break sent to a child in a process group of its own,
 * because Ctrl-C to the whole console would reach the test runner too. */
static void TestInterruptReachesTheHandler(TestingT *t) {
#if defined(_WIN32)
    /* Wine ends a process on Ctrl-Break even when a handler has claimed it. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL)
        testing_t_skip_v(t,
                         "Wine ends a process on Ctrl-Break whatever its handler says");
#endif
    PalErrno err = PAL_OK;
    int64_t p[2];
    if (!pal_pipe(p, 0, &err))
        testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
    const char *argv[] = {self_path, "child", "interrupt", NULL};
    int64_t fds[3] = {PAL_INVALID_HANDLE, p[1], p[1]};
    PalSpawn req = {self_path, argv, NULL, NULL, fds, 3, PAL_SPAWN_SETPGID};
    int64_t pid = pal_spawn(&req, &err);
    pal_close(p[1], NULL);
    if (pid < 0) {
        pal_close(p[0], NULL);
        testing_t_fatalf_v(t, "spawn: %s", pal_errno_string(err));
    }

    char out[256];
    int64_t n = 0;
    while (n < (int64_t)sizeof out - 1 && (n == 0 || out[n - 1] != '\n')) {
        int64_t got = pal_read(p[0], out + n, 1, &err);
        if (got <= 0)
            break;
        n += got;
    }
    out[n] = 0;

    bool sent;
#if defined(_WIN32)
    /* No console to send it through, as under a service, is not what this is
     * about. */
    sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                    GetProcessId((HANDLE)(intptr_t)pid)) != 0;
    if (!sent) {
        pal_kill(pid, PAL_SIGKILL, NULL);
        int32_t ignored;
        pal_wait(pid, &ignored, 0, NULL);
        pal_close(p[0], NULL);
        testing_t_skip_v(t, "no console to send Ctrl-Break through");
    }
#else
    sent = pal_kill(pid, PAL_SIGINT, &err);
    if (!sent)
        testing_t_errorf_v(t, "kill: %s", pal_errno_string(err));
#endif

    while (n < (int64_t)sizeof out - 1) {
        int64_t got = pal_read(p[0], out + n, (int64_t)sizeof out - 1 - n, &err);
        if (got <= 0)
            break;
        n += got;
    }
    out[n] = 0;
    pal_close(p[0], NULL);
    int32_t status = -1;
    if (pal_wait(pid, &status, 0, &err) != pid)
        testing_t_errorf_v(t, "wait: %s", pal_errno_string(err));
    if (status != 0 || strstr(out, "ready") == NULL || strstr(out, "caught 2") == NULL)
        testing_t_errorf_v(t, "child exited %d, output\n%s", status, out);
}

static void TestStdHandle(TestingT *t) {
    for (int i = 0; i < 3; i++)
        if (pal_std_handle(i) == PAL_INVALID_HANDLE)
            testing_t_errorf_v(t, "no handle for standard file %d", i);
#if !defined(_WIN32)
    for (int i = 0; i < 3; i++)
        if (pal_std_handle(i) != i)
            testing_t_errorf_v(t, "standard file %d is handle %d", i,
                               pal_std_handle(i));
#endif
    if (pal_std_handle(3) != PAL_INVALID_HANDLE ||
        pal_std_handle(-1) != PAL_INVALID_HANDLE)
        testing_t_error_v(t, "a handle for a standard file that is not one");
}

static void TestDescriptorTable(TestingT *t) {
#if defined(_WIN32)
    testing_t_skip_v(t, "Windows has no descriptor numbers to hand a child");
#else
    PalErrno err = PAL_OK;
    int64_t p[2];
    if (!pal_pipe(p, 0, &err))
        testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
    /* The write end goes to 3, and 1 is left closed, so nothing arrives on
     * the pipe unless the child found it in the slot it was put in. */
    const char *argv[] = {self_path, "child", "fd3", NULL};
    int64_t fds[4] = {PAL_INVALID_HANDLE, PAL_INVALID_HANDLE, 2, p[1]};
    PalSpawn req = {self_path, argv, NULL, NULL, fds, 4, 0};
    int64_t pid = pal_spawn(&req, &err);
    pal_close(p[1], NULL);
    if (pid < 0)
        testing_t_fatalf_v(t, "spawn: %s", pal_errno_string(err));
    char buf[64];
    int64_t n = 0, got;
    while ((got = pal_read(p[0], buf + n, (int64_t)sizeof buf - 1 - n, NULL)) > 0)
        n += got;
    buf[n] = 0;
    pal_close(p[0], NULL);
    int32_t status = -1;
    pal_wait(pid, &status, 0, NULL);
    if (status != 0 || strcmp(buf, "three\n") != 0)
        testing_t_errorf_v(t, "status %d, read %q from descriptor 3", status, buf);
#endif
}

static void TestGetpid(TestingT *t) {
    int64_t me = pal_getpid();
    if (me <= 0)
        testing_t_fatalf_v(t, "getpid %d", me);
    const char *args[] = {"pid", NULL};
    Run r;
    if (!run(t, args, NULL, NULL, &r))
        return;
    long long child_pid = strtoll(r.out, NULL, 10);
    if (child_pid <= 0 || child_pid == me)
        testing_t_errorf_v(t, "child reports pid %q, parent is %d", r.out, me);
#if !defined(_WIN32)
    if (child_pid != r.pid)
        testing_t_errorf_v(t, "spawn said %d, child says %q", r.pid, r.out);
#endif
}

static void TestExecLookup(TestingT *t) {
    char buf[1024];
    PalErrno err = PAL_OK;
#if defined(_WIN32)
    const char *name = "cmd";
    const char *suffix = "cmd.exe";
#else
    const char *name = "sh";
    const char *suffix = "/sh";
#endif
    int64_t n = pal_exec_lookup(name, buf, sizeof buf, &err);
    size_t sl = strlen(suffix);
    if (n < (int64_t)sl || (size_t)n != strlen(buf))
        testing_t_fatalf_v(t, "lookup %s: %d, %s", name, n, pal_errno_string(err));
    const char *tail = buf + n - (int64_t)sl;
    bool match = true;
    for (size_t i = 0; i < sl; i++) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        match = match && c == suffix[i];
    }
    if (!match)
        testing_t_errorf_v(t, "lookup %s gave %q", name, buf);

    err = PAL_OK;
    if (pal_exec_lookup("burrow-no-such-program", buf, sizeof buf, &err) != -1 ||
        err != PAL_ENOENT)
        testing_t_errorf_v(t, "a missing program: %s, want ENOENT",
                           pal_errno_string(err));
    err = PAL_OK;
    if (pal_exec_lookup(name, buf, 2, &err) != -1 || err != PAL_ERANGE)
        testing_t_errorf_v(t, "into two bytes: %s, want ERANGE", pal_errno_string(err));

    /* A name with a separator is looked at where it is and nowhere else. */
    err = PAL_OK;
    n = pal_exec_lookup(self_path, buf, sizeof buf, &err);
    if (n < 0 || strncmp(buf, self_path, strlen(self_path)) != 0)
        testing_t_errorf_v(t, "lookup of this binary by path: %s, %s", buf,
                           pal_errno_string(err));
}

static void TestMapShared(TestingT *t) {
    char path[1100];
    uint32_t rnd = 0;
    pal_random_bytes(&rnd, sizeof rnd, NULL);
    snprintf(path, sizeof path, "%s/burrow-mmap-%08x", temp_root(), (unsigned)rnd);
    PalErrno err = PAL_OK;
    int64_t fd = pal_open(path, PAL_O_RDWR | PAL_O_CREATE | PAL_O_TRUNC, 0600, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open %s: %s", path, pal_errno_string(err));
    enum { SIZE = 65536 };
    if (!pal_ftruncate(fd, SIZE, &err))
        testing_t_errorf_v(t, "truncate: %s", pal_errno_string(err));

    char *m = pal_mmap(fd, 0, SIZE, PAL_PROT_READ | PAL_PROT_WRITE, &err);
    if (m == NULL) {
        testing_t_errorf_v(t, "mmap: %s", pal_errno_string(err));
    } else {
        memcpy(m + 100, "shared", 6);
        char back[7] = {0};
        if (pal_pread(fd, back, 6, 100, &err) != 6 || strcmp(back, "shared") != 0)
            testing_t_errorf_v(t, "a write through the map read back as %q", back);
        if (pal_pwrite(fd, "FILE", 4, 200, &err) != 4 ||
            memcmp(m + 200, "FILE", 4) != 0)
            testing_t_errorf_v(t, "a write to the file is not in the map");

        char *p = pal_mmap(fd, 0, SIZE,
                           PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_PRIVATE, &err);
        if (p == NULL) {
            testing_t_errorf_v(t, "private mmap: %s", pal_errno_string(err));
        } else {
            memcpy(p + 100, "PRIVAT", 6);
            if (memcmp(m + 100, "shared", 6) != 0)
                testing_t_errorf_v(t, "a private write reached the shared map");
            if (!pal_munmap(p, SIZE, &err))
                testing_t_errorf_v(t, "munmap private: %s", pal_errno_string(err));
        }
        if (!pal_munmap(m, SIZE, &err))
            testing_t_errorf_v(t, "munmap: %s", pal_errno_string(err));
    }
    if (pal_mmap(fd, 0, 0, PAL_PROT_READ, &err) != NULL || err != PAL_EINVAL)
        testing_t_errorf_v(t, "a zero length map: %s, want EINVAL",
                           pal_errno_string(err));
    pal_close(fd, NULL);
    pal_unlink(path, NULL);
}

#define TESTS(X)                                                                       \
    X(TestExitStatus)                                                                  \
    X(TestExitSkipsBufferedOutput)                                                     \
    X(TestArgumentsArriveIntact)                                                       \
    X(TestEnvironment)                                                                 \
    X(TestWorkingDirectory)                                                            \
    X(TestMissingProgram)                                                              \
    X(TestWaitNoHangAndKill)                                                           \
    X(TestWaitSignal)                                                                  \
    X(TestInterruptReachesTheHandler)                                                  \
    X(TestStdHandle)                                                                   \
    X(TestDescriptorTable)                                                             \
    X(TestGetpid)                                                                      \
    X(TestExecLookup)                                                                  \
    X(TestMapShared)

/* A relative program path is taken relative to the directory the child starts
 * in, as it is in Go, so the children are started by an absolute one. */
static char self_buf[2048];

static const char *absolute(const char *p) {
    bool abs = p[0] == '/' || p[0] == '\\' || (p[0] != 0 && p[1] == ':');
    char cwd[1024];
    if (abs || pal_getcwd(cwd, sizeof cwd, NULL) < 0)
        return p;
    snprintf(self_buf, sizeof self_buf, "%s/%s", cwd, p);
    return self_buf;
}

int main(int argc, char **argv) {
    self_path = absolute(argv[0]);
    if (argc >= 3 && strcmp(argv[1], "child") == 0)
        return child(argc, argv);
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(argc, argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                testing_m_run);
}
