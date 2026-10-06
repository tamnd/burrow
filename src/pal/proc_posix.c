/* Processes on everything but Windows: starting one, waiting for it, and the
 * few things a process asks about itself. Mapping a file is here too, because
 * it is the other half of what two processes share.
 *
 * Spawning is fork and exec, written the way Go's forkAndExecInChild is. Every
 * signal is blocked across the fork so that no handler of ours runs in the
 * child before exec, and the child puts every handled signal back to its
 * default before it unblocks them. Between the fork and the exec the child only
 * makes calls that POSIX lists as async signal safe, because another thread in
 * the parent may have held any lock at the moment of the fork and the child has
 * a copy of it held by nobody. A failure in the child goes back to the parent
 * over a pipe that exec closes, so an empty read is a successful exec.
 *
 * FreeBSD and DragonFly get no _XOPEN_SOURCE, because asking for X/Open there
 * hides NSIG, wait4 and wait6.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#if !defined(__FreeBSD__) && !defined(__DragonFly__)
#define _XOPEN_SOURCE 700
#endif
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* wasip1 has no processes but this one, no signals and no mapping, and the
 * headers for them say so with an #error. */
#if !defined(BURROW_OS_WASI)
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#if defined(BURROW_OS_LINUX)
#include <sys/syscall.h>
#endif

#if defined(BURROW_OS_DARWIN)
#include <crt_externs.h>
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#include <mach-o/dyld.h>
#endif

#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_NETBSD)
#include <sys/sysctl.h>
#endif

/* unistd.h declares it on Linux, where _GNU_SOURCE is set, and gcc calls a
 * second declaration redundant. */
#if !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_LINUX)
extern char **environ;
#endif

/* Go's syscall.ForkLock. A descriptor that is open without close on exec, for
 * as long as that lasts, is held under the read side, and a fork takes the
 * write side, so that no child is ever handed a descriptor that was about to
 * be marked. Only pal_pipe on a platform without pipe2 needs it now. */
static pthread_rwlock_t fork_lock = PTHREAD_RWLOCK_INITIALIZER;

void burrow__pal_fork_rlock(void) {
    pthread_rwlock_rdlock(&fork_lock);
}

void burrow__pal_fork_runlock(void) {
    pthread_rwlock_unlock(&fork_lock);
}

static bool proc_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return false;
}

#if !defined(BURROW_OS_WASI)
static int64_t proc_fail_n(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return -1;
}
#endif

/* ------------------------------------------------------------------ spawn */

#if defined(BURROW_OS_WASI)

/* Nothing to start and nothing to wait for. Go's StartProcess and Wait4 on
 * wasip1 both give ENOSYS, and so does everything here. */
static int64_t proc_nosys(PalErrno *err) {
    BURROW_OUT(err, PAL_ENOSYS);
    return -1;
}

int64_t pal_spawn(const PalSpawn *req, PalErrno *err) {
    (void)req;
    return proc_nosys(err);
}

int64_t pal_wait(int64_t pid, int32_t *status, uint32_t flags, PalErrno *err) {
    (void)pid;
    (void)status;
    (void)flags;
    return proc_nosys(err);
}

int64_t pal_wait4(int64_t pid, uint32_t *status, int32_t options, PalRusage *ru,
                  PalErrno *err) {
    (void)pid;
    (void)status;
    (void)options;
    (void)ru;
    return proc_nosys(err);
}

bool pal_wait_ready(int64_t pid, PalErrno *err) {
    (void)pid;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#else

/* What the child does after the fork. Nothing in here may allocate, take a
 * lock, or touch anything but its arguments and the system calls. Answers
 * with the errno of whatever failed, having not returned at all if the exec
 * worked. */
static int proc_spawn_child(const PalSpawn *req, char *const *envp, int errfd,
                            int maxfd) {
    /* Handlers first, while everything is still blocked. exec would reset
     * them itself, but that is too late for a signal that arrives before it. */
    struct sigaction dfl;
    dfl.sa_handler = SIG_DFL;
    dfl.sa_flags = 0;
    sigemptyset(&dfl.sa_mask);
    for (int s = 1; s < NSIG; s++) {
        struct sigaction cur;
        if (s == SIGKILL || s == SIGSTOP || sigaction(s, NULL, &cur) != 0)
            continue;
        if (cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN)
            sigaction(s, &dfl, NULL);
    }
    sigset_t none;
    sigemptyset(&none);
    if (pthread_sigmask(SIG_SETMASK, &none, NULL) != 0)
        return EINVAL;

    if ((req->flags & PAL_SPAWN_SETSID) != 0 && setsid() < 0)
        return errno;
    if ((req->flags & PAL_SPAWN_SETPGID) != 0 && setpgid(0, 0) != 0)
        return errno;

    /* The error pipe goes above every slot being filled, so that the dup2s
     * below cannot land on it. */
    int nfds = (int)req->nfds;
    if (errfd < nfds) {
        int moved = fcntl(errfd, F_DUPFD_CLOEXEC, nfds);
        if (moved < 0)
            return errno;
        errfd = moved;
    }

    /* Pass one: any source that sits in a slot about to be overwritten moves
     * up out of the way first. Pass two puts each one where it belongs. This
     * is Go's order, and the only one that handles fds = {1, 0}. */
    int src[256];
    for (int i = 0; i < nfds; i++) {
        src[i] = (int)req->fds[i];
        if (src[i] >= 0 && src[i] < nfds && src[i] != i) {
            int moved = fcntl(src[i], F_DUPFD_CLOEXEC, nfds);
            if (moved < 0)
                return errno;
            src[i] = moved;
        }
    }
    for (int i = 0; i < nfds; i++) {
        if (src[i] < 0) {
            close(i);
        } else if (src[i] == i) {
            if (fcntl(i, F_SETFD, 0) != 0)
                return errno;
        } else if (dup2(src[i], i) < 0) {
            return errno;
        }
    }

    if (req->dir != NULL && chdir(req->dir) != 0)
        return errno;

    /* Everything above the table closes, bar the error pipe, which exec
     * closes itself. */
#if defined(BURROW_OS_LINUX) && defined(SYS_close_range)
    bool ranged = true;
    if (errfd > nfds &&
        syscall(SYS_close_range, (unsigned)nfds, (unsigned)errfd - 1, 0) != 0)
        ranged = false;
    if (ranged && syscall(SYS_close_range, (unsigned)errfd + 1, ~0U, 0) != 0)
        ranged = false;
    if (!ranged)
#endif
    {
        for (int fd = nfds; fd < maxfd; fd++) {
            if (fd != errfd)
                close(fd);
        }
    }

    execve(req->path, (char *const *)(uintptr_t)req->argv, envp);
    return errno;
}

/* The highest descriptor worth closing in the child, asked for here because
 * getrlimit is not on the list of calls a child may make. */
static int proc_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit((int)RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY ||
        rl.rlim_cur > 65536)
        return 65536;
    return (int)rl.rlim_cur;
}

int64_t pal_spawn(const PalSpawn *req, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (req == NULL || req->path == NULL || req->argv == NULL || req->nfds < 0 ||
        req->nfds > 256 || (req->nfds > 0 && req->fds == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    char *const *envp = req->envp != NULL ? (char *const *)(uintptr_t)req->envp
                                          : (char *const *)(uintptr_t)pal_environ();
    int maxfd = proc_fd_limit();

    int64_t p[2];
    if (!pal_pipe(p, 0, err))
        return -1;

    sigset_t all, old;
    sigfillset(&all);
    pthread_rwlock_wrlock(&fork_lock);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    pid_t pid = fork();
    if (pid == 0) {
        int e = proc_spawn_child(req, envp, (int)p[1], maxfd);
        ssize_t w;
        do {
            w = write((int)p[1], &e, sizeof e);
        } while (w < 0 && errno == EINTR);
        _exit(127);
    }
    int forked = errno;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_rwlock_unlock(&fork_lock);
    pal_close(p[1], NULL);
    if (pid < 0) {
        pal_close(p[0], NULL);
        BURROW_OUT(err, burrow__pal_errno(forked));
        return -1;
    }

    int e = 0;
    ssize_t n;
    do {
        n = read((int)p[0], &e, sizeof e);
    } while (n < 0 && errno == EINTR);
    pal_close(p[0], NULL);
    if (n == 0)
        return (int64_t)pid;

    /* The exec failed, and the child has exited or is about to. Reap it here,
     * since the caller was never told there was anything to wait for. */
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    BURROW_OUT(err, n == (ssize_t)sizeof e ? burrow__pal_errno(e) : PAL_EOTHER);
    return -1;
}

/* WCOREDUMP is not POSIX, though every system this runs on has it. */
static bool proc_core_dumped(int st) {
#ifdef WCOREDUMP
    return WCOREDUMP(st) != 0;
#else
    (void)st;
    return false;
#endif
}

int64_t pal_wait(int64_t pid, int32_t *status, uint32_t flags, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (pid <= 0 || (flags & ~(uint32_t)(PAL_WAIT_NOHANG | PAL_WAIT_SIGNAL)) != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    int st = 0;
    pid_t got;
    do {
        got = waitpid((pid_t)pid, &st, (flags & PAL_WAIT_NOHANG) != 0 ? WNOHANG : 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return proc_fail_n(err);
    if (got == 0)
        return 0;
    int32_t code = 0;
    if (WIFEXITED(st))
        code = (int32_t)WEXITSTATUS(st);
    else if (WIFSIGNALED(st) && (flags & PAL_WAIT_SIGNAL) != 0)
        code = -burrow__pal_signal_from_native(WTERMSIG(st)) -
               (proc_core_dumped(st) ? 256 : 0);
    else if (WIFSIGNALED(st))
        code = 128 + burrow__pal_signal_from_native(WTERMSIG(st));
    BURROW_OUT(status, code);
    return (int64_t)got;
}

int64_t pal_wait4(int64_t pid, uint32_t *status, int32_t options, PalRusage *ru,
                  PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    int st = 0;
    struct rusage r;
    memset(&r, 0, sizeof r);
    pid_t got = wait4((pid_t)pid, &st, (int)options, &r);
    if (got < 0)
        return proc_fail_n(err);
    BURROW_OUT(status, (uint32_t)st);
    if (ru != NULL) {
        memset(ru, 0, sizeof *ru);
        ru->utime_sec = (int64_t)r.ru_utime.tv_sec;
        ru->utime_usec = (int64_t)r.ru_utime.tv_usec;
        ru->stime_sec = (int64_t)r.ru_stime.tv_sec;
        ru->stime_usec = (int64_t)r.ru_stime.tv_usec;
        ru->maxrss = (int64_t)r.ru_maxrss;
        ru->ixrss = (int64_t)r.ru_ixrss;
        ru->idrss = (int64_t)r.ru_idrss;
        ru->isrss = (int64_t)r.ru_isrss;
        ru->minflt = (int64_t)r.ru_minflt;
        ru->majflt = (int64_t)r.ru_majflt;
        ru->nswap = (int64_t)r.ru_nswap;
        ru->inblock = (int64_t)r.ru_inblock;
        ru->oublock = (int64_t)r.ru_oublock;
        ru->msgsnd = (int64_t)r.ru_msgsnd;
        ru->msgrcv = (int64_t)r.ru_msgrcv;
        ru->nsignals = (int64_t)r.ru_nsignals;
        ru->nvcsw = (int64_t)r.ru_nvcsw;
        ru->nivcsw = (int64_t)r.ru_nivcsw;
    }
    return (int64_t)got;
}

bool pal_wait_ready(int64_t pid, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
#if defined(BURROW_OS_LINUX) && !defined(BURROW_OS_COSMO)
    siginfo_t si;
    memset(&si, 0, sizeof si);
    int r;
    do {
        r = waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT);
    } while (r != 0 && errno == EINTR);
    return r == 0 || proc_fail(err);
#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                       \
    defined(BURROW_OS_DRAGONFLY)
    int st = 0;
    pid_t r;
    do {
        r = wait6(P_PID, (id_t)pid, &st, WEXITED | WNOWAIT, NULL, NULL);
    } while (r < 0 && errno == EINTR);
    return r >= 0 || proc_fail(err);
#else
    (void)pid;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
#endif
}

#endif /* !BURROW_OS_WASI */

int64_t pal_process_id(int64_t pid) {
    return pid;
}

int64_t pal_process_open(int64_t id, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    return id;
}

void pal_process_close(int64_t pid) {
    (void)pid;
}

#if defined(BURROW_OS_WASI)

/* wasip1 has no ids to ask for. These are the numbers Go's syscall makes up
 * for it, so that a program sees the same ones under either. */
int64_t pal_getpid(void) {
    return 3;
}

int64_t pal_getppid(void) {
    return 2;
}

void pal_ids(PalIds *out) {
    out->uid = 1;
    out->euid = 1;
    out->gid = 1;
    out->egid = 1;
}

int64_t pal_getgroups(uint32_t *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (cap < 0 || (cap > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    /* getgroups(0, NULL) asks how many there are, and the answer is Go's one
     * group, 1. */
    if (cap > 0)
        buf[0] = 1;
    return 1;
}

#else

int64_t pal_getpid(void) {
    return (int64_t)getpid();
}

int64_t pal_getppid(void) {
    return (int64_t)getppid();
}

void pal_ids(PalIds *out) {
    out->uid = (int64_t)getuid();
    out->euid = (int64_t)geteuid();
    out->gid = (int64_t)getgid();
    out->egid = (int64_t)getegid();
}

_Static_assert(sizeof(gid_t) == sizeof(uint32_t), "gid_t is 32 bits");

int64_t pal_getgroups(uint32_t *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (cap < 0 || (cap > 0 && buf == NULL) || cap > INT32_MAX) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    int n = getgroups((int)cap, (gid_t *)(void *)buf);
    if (n < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return -1;
    }
    return (int64_t)n;
}

#endif /* !BURROW_OS_WASI */

int64_t pal_std_handle(int i) {
    return i >= 0 && i <= 2 ? (int64_t)i : PAL_INVALID_HANDLE;
}

void pal_exit(int32_t code) {
#if BURROW_TSAN && defined(BURROW_OS_LINUX)
    /* tsan's _exit runs its finalizer first, and that flushes stdio, which
     * _exit must not do. The system call underneath skips all of it. */
    syscall(SYS_exit_group, (int)code);
#endif
    _exit((int)code);
}

const char *const *pal_environ(void) {
#if defined(BURROW_OS_DARWIN)
    /* environ is only there for executables on macOS, and a shared library
     * has to ask for it. */
    return (const char *const *)(uintptr_t)*_NSGetEnviron();
#else
    return (const char *const *)(uintptr_t)environ;
#endif
}

int64_t pal_getenv(const char *key, char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (key == NULL || cap < 0 || (cap > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    const char *v = getenv(key);
    if (v == NULL) {
        BURROW_OUT(err, PAL_ENOENT);
        return -1;
    }
    size_t n = strlen(v);
    if (n > (size_t)cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    if (n > 0)
        memcpy(buf, v, n);
    return (int64_t)n;
}

bool pal_setenv(const char *key, const char *value, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (key == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int r = value == NULL ? unsetenv(key) : setenv(key, value, 1);
    if (r != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    return true;
}

int64_t pal_environ_read(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (cap < 0 || (cap > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    int64_t used = 0;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        size_t n = strlen(*env) + 1;
        if ((int64_t)n > cap - used) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        memcpy(buf + used, *env, n);
        used += (int64_t)n;
    }
    return used;
}

/* ------------------------------------------------------------ exec lookup */

/* Go's findExecutable: there, not a directory, and executable by someone. */
static bool proc_executable(const char *path, PalErrno *err) {
    struct stat st;
    if (stat(path, &st) != 0)
        return proc_fail(err);
    if (S_ISDIR(st.st_mode) || (st.st_mode & 0111) == 0) {
        BURROW_OUT(err, PAL_EACCES);
        return false;
    }
    return true;
}

static int64_t proc_put_path(char *buf, int64_t cap, const char *dir, size_t dlen,
                             const char *name, PalErrno *err) {
    size_t nlen = 0;
    while (name[nlen] != 0)
        nlen++;
    size_t need = dlen + (dlen > 0 ? 1 : 0) + nlen;
    if ((int64_t)need >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < dlen; i++)
        buf[o++] = dir[i];
    if (dlen > 0)
        buf[o++] = '/';
    for (size_t i = 0; i < nlen; i++)
        buf[o++] = name[i];
    buf[o] = 0;
    return (int64_t)o;
}

int64_t pal_exec_lookup(const char *name, char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (name == NULL || name[0] == 0 || buf == NULL || cap <= 0) {
        BURROW_OUT(err, name != NULL && name[0] == 0 ? PAL_ENOENT : PAL_EINVAL);
        return -1;
    }
    bool slash = false;
    for (const char *c = name; *c != 0; c++)
        slash = slash || *c == '/';
    if (slash) {
        if (!proc_executable(name, err))
            return -1;
        return proc_put_path(buf, cap, NULL, 0, name, err);
    }

    /* Each element of PATH in turn, with an empty one meaning the current
     * directory, as Go's filepath.SplitList and LookPath have it. Go refuses a
     * relative answer with ErrDot, and that is for os/exec to decide, since
     * the answer here is the path either way. */
    const char *path = getenv("PATH");
    if (path == NULL)
        path = "";
    /* Each candidate is built in a buffer of our own, so that a caller whose
     * buffer is too small hears ERANGE about the program that is there rather
     * than ENOENT because it was never looked at. */
    char cand[PATH_MAX];
    const char *p = path;
    for (;;) {
        const char *end = p;
        while (*end != 0 && *end != ':')
            end++;
        const char *dir = p;
        size_t dlen = (size_t)(end - p);
        if (dlen == 0) {
            dir = ".";
            dlen = 1;
        }
        PalErrno e = PAL_OK;
        if (proc_put_path(cand, (int64_t)sizeof cand, dir, dlen, name, &e) >= 0 &&
            proc_executable(cand, &e))
            return proc_put_path(buf, cap, NULL, 0, cand, err);
        if (*end == 0)
            break;
        p = end + 1;
    }
    BURROW_OUT(err, PAL_ENOENT);
    return -1;
}

/* -------------------------------------------------------------- executable */

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_SOLARIS) ||                         \
    defined(BURROW_OS_COSMO)
#if defined(BURROW_OS_COSMO)
/* cosmo's libc declares this only under _COSMO_SOURCE, which this file leaves
 * off. */
char *GetProgramExecutableName(void);
#endif

/* src into buf, NUL terminated, or PAL_ERANGE. */
static int64_t proc_put_cstr(char *buf, int64_t cap, const char *src, size_t n,
                             PalErrno *err) {
    if (buf == NULL || cap < 0 || (int64_t)n >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, src, n);
    buf[n] = 0;
    return (int64_t)n;
}
#endif

int64_t pal_executable(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    char small[PATH_MAX];
    uint32_t n = (uint32_t)sizeof small;
    if (_NSGetExecutablePath(small, &n) != 0) {
        /* n is now the size it wants, and the path does not fit ours. */
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    return proc_put_cstr(buf, cap, small, strlen(small), err);
#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY) ||                    \
    defined(BURROW_OS_NETBSD)
#if defined(BURROW_OS_NETBSD)
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME};
#else
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
#endif
    char small[PATH_MAX];
    size_t n = sizeof small;
    if (sysctl(mib, 4, small, &n, NULL, 0) != 0)
        return proc_fail_n(err);
    if (n == 0)
        return proc_put_cstr(buf, cap, "", 0, err);
    return proc_put_cstr(buf, cap, small, n - 1, err);
#elif defined(BURROW_OS_SOLARIS)
    const char *e = getexecname();
    if (e == NULL) {
        BURROW_OUT(err, PAL_ENOENT);
        return -1;
    }
    return proc_put_cstr(buf, cap, e, strlen(e), err);
#elif defined(BURROW_OS_COSMO)
    /* The same binary runs on several systems, and cosmo works out where it was
     * started from on each of them. */
    const char *e = GetProgramExecutableName();
    if (e == NULL || *e == 0) {
        BURROW_OUT(err, PAL_ENOENT);
        return -1;
    }
    return proc_put_cstr(buf, cap, e, strlen(e), err);
#else
    (void)buf;
    (void)cap;
    BURROW_OUT(err, PAL_ENOTSUP);
    return -1;
#endif
}

/* -------------------------------------------------------------------- mmap */

#if defined(BURROW_OS_WASI)

/* Go's syscall has no Mmap on wasip1, and wasi-libc's emulation copies the
 * file into memory, which is not a mapping two processes could share. */
void *pal_mmap(int64_t fd, int64_t off, int64_t len, uint32_t prot, PalErrno *err) {
    (void)fd;
    (void)off;
    (void)len;
    (void)prot;
    BURROW_OUT(err, PAL_ENOSYS);
    return NULL;
}

bool pal_munmap(void *addr, int64_t len, PalErrno *err) {
    (void)addr;
    (void)len;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#else

void *pal_mmap(int64_t fd, int64_t off, int64_t len, uint32_t prot, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (fd < 0 || off < 0 || len <= 0 ||
        (prot & ~(uint32_t)(PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC |
                            PAL_PROT_PRIVATE)) != 0 ||
        (prot & (PAL_PROT_WRITE | PAL_PROT_EXEC)) == (PAL_PROT_WRITE | PAL_PROT_EXEC)) {
        BURROW_OUT(err, PAL_EINVAL);
        return NULL;
    }
    int p = 0;
    if ((prot & PAL_PROT_READ) != 0)
        p |= PROT_READ;
    /* Never both, which was refused above. */
    if ((prot & PAL_PROT_EXEC) != 0)
        p |= PROT_EXEC;
    else if ((prot & PAL_PROT_WRITE) != 0)
        p |= PROT_WRITE;
    int flags = (prot & PAL_PROT_PRIVATE) != 0 ? MAP_PRIVATE : MAP_SHARED;
    void *addr = mmap(NULL, (size_t)len, p, flags, (int)fd, (off_t)off);
    if (addr == MAP_FAILED) {
        proc_fail(err);
        return NULL;
    }
    return addr;
}

bool pal_munmap(void *addr, int64_t len, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (addr == NULL || len <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return munmap(addr, (size_t)len) == 0 || proc_fail(err);
}

#endif /* !BURROW_OS_WASI */

#endif /* !BURROW_OS_WINDOWS */
