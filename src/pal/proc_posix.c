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
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* wasip1 has no processes but this one, no signals, no mapping and no groups,
 * and the headers for them say so with an #error or are not there. */
#if !defined(BURROW_OS_WASI)
#include <grp.h>
#include <signal.h>
#include <sys/ioctl.h>
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

/* The systems the child can ask to be traced on, with PT_TRACE_ME. */
#if defined(BURROW_OS_DARWIN) || BURROW_BSD
#define PROC_HAS_PT_TRACE_ME 1
#include <sys/ptrace.h>
#include <sys/types.h>
#endif

#if defined(BURROW_OS_FREEBSD)
#include <sys/jail.h>
#include <sys/procctl.h>
#endif

/* chroot left POSIX in 2001, so the headers hide it from a file that asks for
 * POSIX 2008 as this one does, and the BSDs hide setgroups with it. Both are
 * still in every C library. */
#if (defined(BURROW_OS_DARWIN) && defined(_POSIX_C_SOURCE) &&                          \
     _POSIX_C_SOURCE >= 200112L) ||                                                    \
    (BURROW_BSD && defined(__BSD_VISIBLE) && !__BSD_VISIBLE)
int chroot(const char *path);
#endif
#if BURROW_BSD && defined(__BSD_VISIBLE) && !__BSD_VISIBLE
int setgroups(int n, const gid_t *groups);
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

bool pal_set_ids(PalSetID which, uint32_t a, uint32_t b, uint32_t c, PalErrno *err) {
    (void)which;
    (void)a;
    (void)b;
    (void)c;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

bool pal_setgroups(const uint32_t *gids, int64_t n, PalErrno *err) {
    (void)gids;
    (void)n;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#else

#if defined(BURROW_OS_LINUX)
/* The numbers Go spells out as well, for prctl, mount, clone and capset,
 * rather than take them from the kernel's headers. */
#define PROC_PR_SET_PDEATHSIG 1
#define PROC_PR_SET_KEEPCAPS 8
#define PROC_PR_CAP_AMBIENT 0x2f
#define PROC_PR_CAP_AMBIENT_RAISE 2
#define PROC_PTRACE_TRACEME 0
#define PROC_MS_REC 0x4000UL
#define PROC_MS_PRIVATE 0x40000UL
#define PROC_CLONE_NEWTIME 0x80ULL
#define PROC_CLONE_PIDFD 0x1000ULL
#define PROC_CLONE_VM 0x100ULL
#define PROC_CLONE_VFORK 0x4000ULL
#define PROC_CLONE_NEWNS 0x20000ULL
#define PROC_CLONE_NEWUSER 0x10000000ULL
#define PROC_CLONE_INTO_CGROUP 0x200000000ULL
#define PROC_CAP_VERSION_3 0x20080522U
#if !defined(SYS_clone3)
#define SYS_clone3 435
#endif

/* The 32 bit systems have a second set of id calls that take 32 bit ids, and
 * those are the ones Go uses. */
#if defined(SYS_setgroups32)
#define PROC_SYS_SETGROUPS SYS_setgroups32
#define PROC_SYS_SETGID SYS_setgid32
#define PROC_SYS_SETUID SYS_setuid32
#else
#define PROC_SYS_SETGROUPS SYS_setgroups
#define PROC_SYS_SETGID SYS_setgid
#define PROC_SYS_SETUID SYS_setuid
#endif
#endif

/* What the child is handed, all of it worked out before the fork. */
typedef struct ProcChild {
    const PalSpawn *req;
    const PalSpawnSys *sys;
    char *const *envp;
    int errfd;
    /* Room for the child's copy of the descriptor table while it shuffles it,
     * made before the fork since the child cannot allocate. */
    int *scratch;
    /* The read end of the pipe the parent says the id maps are written on, or
     * -1 when there are none. Linux only. */
    int mapfd;
    int maxfd;
    pid_t ppid;
    size_t uid_map_len;
    size_t gid_map_len;
} ProcChild;

static const PalSpawnSys proc_no_sys;

/* Every handled signal back to its default, then nothing blocked. exec would
 * reset the handlers itself, but that is too late for a signal that arrives
 * before it. This is Go's runtime_AfterForkInChild. */
static int proc_child_signals(void) {
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
    return 0;
}

#if defined(BURROW_OS_LINUX)
/* Writes n bytes of data to the file at path, for the id maps. Errno or 0. */
static int proc_child_write(const char *path, const char *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    int e = write(fd, data, n) < 0 ? errno : 0;
    if (close(fd) != 0 && e == 0)
        e = errno;
    return e;
}

/* unshare, and what Go does after it: the maps of a new user namespace, which
 * only the child can write, and / made private in a new mount namespace. */
static int proc_child_unshare(const ProcChild *c) {
    const PalSpawnSys *sys = c->sys;
    if (syscall(SYS_unshare, (unsigned long)sys->unshareflags) != 0)
        return errno;
    int e;
    if ((sys->unshareflags & PROC_CLONE_NEWUSER) != 0 && sys->gid_map != NULL) {
        static const char allow[] = "allow", deny[] = "deny";
        if (sys->gid_map_setgroups)
            e = proc_child_write("/proc/self/setgroups", allow, sizeof allow - 1);
        else
            e = proc_child_write("/proc/self/setgroups", deny, sizeof deny - 1);
        if (e != 0)
            return e;
        e = proc_child_write("/proc/self/gid_map", sys->gid_map, c->gid_map_len);
        if (e != 0)
            return e;
    }
    if ((sys->unshareflags & PROC_CLONE_NEWUSER) != 0 && sys->uid_map != NULL) {
        e = proc_child_write("/proc/self/uid_map", sys->uid_map, c->uid_map_len);
        if (e != 0)
            return e;
    }
    /* unshare leaves alone a mount that was mounted shared, and systemd mounts
     * / that way, so the new namespace would still see the old one's mounts.
     * Go makes it private, as the unshare command does. */
    if ((sys->unshareflags & PROC_CLONE_NEWNS) != 0 &&
        syscall(SYS_mount, "none", "/", NULL, PROC_MS_REC | PROC_MS_PRIVATE, NULL) != 0)
        return errno;
    return 0;
}

/* Raises the ambient capabilities, which have to be permitted and inheritable
 * first. */
static int proc_child_caps(const PalSpawnSys *sys) {
    struct {
        uint32_t version;
        int32_t pid;
    } hdr = {PROC_CAP_VERSION_3, 0};
    struct {
        uint32_t effective;
        uint32_t permitted;
        uint32_t inheritable;
    } data[2];
    memset(data, 0, sizeof data);
    if (syscall(SYS_capget, &hdr, data) != 0)
        return errno;
    for (int64_t i = 0; i < sys->nambient_caps; i++) {
        uint64_t cap = sys->ambient_caps[i];
        if ((cap >> 5) >= 2)
            return EINVAL;
        data[cap >> 5].permitted |= 1U << (cap & 31);
        data[cap >> 5].inheritable |= 1U << (cap & 31);
    }
    if (syscall(SYS_capset, &hdr, data) != 0)
        return errno;
    for (int64_t i = 0; i < sys->nambient_caps; i++) {
        if (syscall(SYS_prctl, PROC_PR_CAP_AMBIENT, PROC_PR_CAP_AMBIENT_RAISE,
                    (unsigned long)sys->ambient_caps[i], 0UL, 0UL) != 0)
            return errno;
    }
    return 0;
}
#endif

/* The user, group and supplementary groups. On Linux these are the system
 * calls themselves, since the C library's would try to change every thread
 * the parent had, and in the child they are gone. */
static int proc_child_credential(const PalSpawnSys *sys) {
#if defined(BURROW_OS_LINUX)
    if (!(sys->gid_map != NULL && !sys->gid_map_setgroups && sys->ngroups == 0) &&
        !sys->no_set_groups &&
        syscall(PROC_SYS_SETGROUPS, (unsigned long)sys->ngroups, sys->groups) != 0)
        return errno;
    if (syscall(PROC_SYS_SETGID, (unsigned long)sys->gid) != 0)
        return errno;
    if (syscall(PROC_SYS_SETUID, (unsigned long)sys->uid) != 0)
        return errno;
#else
#if defined(BURROW_OS_COSMO)
    /* Cosmopolitan has the Linux signature, with a size_t count. */
    size_t ngroups = (size_t)sys->ngroups;
#else
    int ngroups = (int)sys->ngroups;
#endif
    if (!sys->no_set_groups &&
        setgroups(ngroups, (const gid_t *)(const void *)sys->groups) != 0)
        return errno;
    if (setgid((gid_t)sys->gid) != 0)
        return errno;
    if (setuid((uid_t)sys->uid) != 0)
        return errno;
#endif
    return 0;
}

/* The parent death signal, and the signal itself straight away when the
 * parent died before it was set. */
static int proc_child_pdeathsig(const ProcChild *c) {
    int sig = c->sys->pdeathsig;
#if defined(BURROW_OS_LINUX)
    if (syscall(SYS_prctl, PROC_PR_SET_PDEATHSIG, (unsigned long)sig, 0UL, 0UL, 0UL) !=
        0)
        return errno;
#elif defined(BURROW_OS_FREEBSD)
    if (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig) != 0)
        return errno;
#endif
    if (getppid() != c->ppid && kill(getpid(), sig) != 0)
        return errno;
    return 0;
}

/* What the child does after the fork, in the order Go's forkAndExecInChild
 * does it on the system. Nothing in here may allocate, take a lock, or touch
 * anything but its arguments and the system calls. Answers with the errno of
 * whatever failed, having not returned at all if the exec worked. */
static int proc_spawn_child(const ProcChild *c) {
    const PalSpawn *req = c->req;
    const PalSpawnSys *sys = c->sys;
    bool setpgid_ = (req->flags & PAL_SPAWN_SETPGID) != 0 || sys->foreground;
    int e;

#if defined(BURROW_OS_FREEBSD)
    if (sys->jail > 0 && jail_attach((int)sys->jail) != 0)
        return errno;
#endif
#if defined(PROC_HAS_PT_TRACE_ME)
    if (sys->ptrace && ptrace(PT_TRACE_ME, 0, 0, 0) != 0)
        return errno;
#endif
#if defined(BURROW_OS_LINUX)
    if (sys->nambient_caps > 0 &&
        syscall(SYS_prctl, PROC_PR_SET_KEEPCAPS, 1UL, 0UL, 0UL, 0UL) != 0)
        return errno;
    /* The parent writes the maps of a user namespace clone made, and nothing
     * here can go ahead until it has. */
    if (c->mapfd >= 0) {
        int32_t e2 = 0;
        ssize_t n;
        do {
            n = read(c->mapfd, &e2, sizeof e2);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return errno;
        if (n != (ssize_t)sizeof e2)
            return EINVAL;
        if (e2 != 0)
            return e2;
    }
#endif

    if ((req->flags & PAL_SPAWN_SETSID) != 0 && setsid() < 0)
        return errno;
    if (setpgid_ && setpgid(0, (pid_t)sys->pgid) != 0)
        return errno;
    if (sys->foreground) {
        pid_t pgrp = sys->pgid != 0 ? (pid_t)sys->pgid : getpid();
        if (ioctl((int)sys->ctty, TIOCSPGRP, &pgrp) != 0)
            return errno;
    }

    /* After TIOCSPGRP, which would otherwise stop the child with SIGTTOU. */
    e = proc_child_signals();
    if (e != 0)
        return e;

#if defined(BURROW_OS_LINUX)
    if (sys->unshareflags != 0) {
        e = proc_child_unshare(c);
        if (e != 0)
            return e;
    }
#endif
    if (sys->chroot != NULL && chroot(sys->chroot) != 0)
        return errno;
    if (sys->credential) {
        e = proc_child_credential(sys);
        if (e != 0)
            return e;
    }
#if defined(BURROW_OS_LINUX)
    if (sys->nambient_caps > 0) {
        e = proc_child_caps(sys);
        if (e != 0)
            return e;
    }
#endif
    if (req->dir != NULL && chdir(req->dir) != 0)
        return errno;
    if (sys->pdeathsig != 0) {
        e = proc_child_pdeathsig(c);
        if (e != 0)
            return e;
    }

    /* The error pipe goes above every slot being filled, so that the dup2s
     * below cannot land on it. */
    int errfd = c->errfd;
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
    int *src = c->scratch;
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

    if (sys->keep_fds) {
        /* Go's convention: 0, 1 and 2 are not left to the child unless they
         * were asked for. */
        for (int fd = nfds; fd < 3; fd++)
            close(fd);
    } else {
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
            for (int fd = nfds; fd < c->maxfd; fd++) {
                if (fd != errfd)
                    close(fd);
            }
        }
    }

#if defined(TIOCSCTTY) && defined(TIOCNOTTY)
    if (sys->noctty && ioctl(0, TIOCNOTTY, 0) != 0)
        return errno;
#endif
#if defined(BURROW_OS_LINUX)
    if (sys->setctty && ioctl((int)sys->ctty, TIOCSCTTY, 1) != 0)
        return errno;
    /* Last, so that what the child does before the exec is not traced. */
    if (sys->ptrace && syscall(SYS_ptrace, PROC_PTRACE_TRACEME, 0L, 0L, 0L) != 0)
        return errno;
#elif defined(TIOCSCTTY) && defined(TIOCNOTTY)
    if (sys->setctty && ioctl((int)sys->ctty, TIOCSCTTY, 0) != 0)
        return errno;
#endif

    execve(req->path, (char *const *)(uintptr_t)req->argv, c->envp);
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

/* Whether this system can do everything sys asks for. */
static bool proc_sys_supported(const PalSpawnSys *sys) {
#if !defined(BURROW_OS_LINUX)
    if (sys->cloneflags != 0 || sys->unshareflags != 0 || sys->uid_map != NULL ||
        sys->gid_map != NULL || sys->nambient_caps > 0 || sys->use_cgroup_fd ||
        sys->pidfd != NULL)
        return false;
#endif
#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_FREEBSD)
    if (sys->pdeathsig != 0)
        return false;
#endif
#if !defined(BURROW_OS_FREEBSD)
    if (sys->jail != 0)
        return false;
#endif
#if !defined(BURROW_OS_LINUX) && !defined(PROC_HAS_PT_TRACE_ME)
    if (sys->ptrace)
        return false;
#endif
#if !defined(TIOCSCTTY) || !defined(TIOCNOTTY)
    if (sys->setctty || sys->noctty)
        return false;
#endif
    return true;
}

/* fork, or on Linux, when sys asks for something only clone does, clone
 * itself. It is never CLONE_VM, which is how Go saves copying the parent: the
 * child here is a copy, as fork makes it, so CLONE_VM and CLONE_VFORK in
 * cloneflags are left out. */
static pid_t proc_fork(const PalSpawnSys *sys, int32_t *pidfd) {
#if defined(BURROW_OS_LINUX)
    if (sys->cloneflags != 0 || sys->pidfd != NULL || sys->use_cgroup_fd) {
        uint64_t flags = sys->cloneflags & ~(PROC_CLONE_VM | PROC_CLONE_VFORK);
        if (sys->pidfd != NULL)
            flags |= PROC_CLONE_PIDFD;
        if (sys->use_cgroup_fd || (flags & PROC_CLONE_NEWTIME) != 0) {
            struct {
                uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack,
                    stack_size, tls, set_tid, set_tid_size, cgroup;
            } args;
            memset(&args, 0, sizeof args);
            args.flags = flags;
            args.exit_signal = SIGCHLD;
            if (sys->use_cgroup_fd) {
                args.flags |= PROC_CLONE_INTO_CGROUP;
                args.cgroup = (uint64_t)sys->cgroup_fd;
            }
            if (sys->pidfd != NULL)
                args.pidfd = (uint64_t)(uintptr_t)pidfd;
            return (pid_t)syscall(SYS_clone3, &args, sizeof args);
        }
        flags |= SIGCHLD;
#if defined(__s390x__) || defined(__s390__)
        /* s390 has the first two the other way round. */
        return (pid_t)syscall(SYS_clone, 0UL, (unsigned long)flags, pidfd, NULL, 0UL);
#else
        return (pid_t)syscall(SYS_clone, (unsigned long)flags, 0UL, pidfd, NULL, 0UL);
#endif
    }
#else
    (void)sys;
    (void)pidfd;
#endif
    return fork();
}

#if defined(BURROW_OS_LINUX)
/* Writes data to the file at path for the parent, the way Go's
 * writeIDMappings does. Errno or 0. */
static int proc_write_file(const char *path, const char *data, size_t n) {
    int fd;
    do {
        fd = open(path, O_RDWR | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno;
    int e = 0;
    while (n > 0) {
        ssize_t w = write(fd, data, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            e = errno;
            break;
        }
        data += w;
        n -= (size_t)w;
    }
    if (close(fd) != 0 && e == 0)
        e = errno;
    return e;
}

/* The maps of the user namespace the child was cloned into, which have to be
 * written from outside it. Go's writeUidGidMappings. */
static int proc_write_maps(pid_t pid, const PalSpawnSys *sys) {
    char path[64];
    int e;
    if (sys->uid_map != NULL) {
        snprintf(path, sizeof path, "/proc/%ld/uid_map", (long)pid);
        e = proc_write_file(path, sys->uid_map, strlen(sys->uid_map));
        if (e != 0)
            return e;
    }
    if (sys->gid_map != NULL) {
        /* A kernel from before 3.19 has no setgroups file, and needs none. */
        snprintf(path, sizeof path, "/proc/%ld/setgroups", (long)pid);
        const char *sg = sys->gid_map_setgroups ? "allow" : "deny";
        e = proc_write_file(path, sg, strlen(sg));
        if (e != 0 && e != ENOENT)
            return e;
        snprintf(path, sizeof path, "/proc/%ld/gid_map", (long)pid);
        e = proc_write_file(path, sys->gid_map, strlen(sys->gid_map));
        if (e != 0)
            return e;
    }
    return 0;
}
#endif

static int64_t proc_spawn(const ProcChild *c, PalErrno *err);

int64_t pal_spawn(const PalSpawn *req, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (req == NULL || req->path == NULL || req->argv == NULL || req->nfds < 0 ||
        (req->nfds > 0 && req->fds == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    const PalSpawnSys *sys = req->sys != NULL ? req->sys : &proc_no_sys;
    if (!proc_sys_supported(sys)) {
        BURROW_OUT(err, PAL_ENOSYS);
        return -1;
    }

    ProcChild c;
    memset(&c, 0, sizeof c);
    c.req = req;
    c.sys = sys;
    c.envp = req->envp != NULL ? (char *const *)(uintptr_t)req->envp
                               : (char *const *)(uintptr_t)pal_environ();
    c.maxfd = proc_fd_limit();
    c.mapfd = -1;
    c.ppid = sys->pdeathsig != 0 ? getpid() : 0;
    c.uid_map_len = sys->uid_map != NULL ? strlen(sys->uid_map) : 0;
    c.gid_map_len = sys->gid_map != NULL ? strlen(sys->gid_map) : 0;

    /* Most spawns pass three, and the rest get pages of their own. */
    int small[64];
    size_t scratch_size = 0;
    c.scratch = small;
    if (req->nfds > (int32_t)(sizeof small / sizeof small[0])) {
        scratch_size = (size_t)req->nfds * sizeof(int);
        void *m = mmap(NULL, scratch_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) {
            (void)proc_fail(err);
            return -1;
        }
        c.scratch = (int *)m;
    }
    int64_t pid = proc_spawn(&c, err);
    if (scratch_size > 0)
        munmap(c.scratch, scratch_size);
    return pid;
}

/* The rest of pal_spawn, once c is ready. */
static int64_t proc_spawn(const ProcChild *cc, PalErrno *err) {
    ProcChild c = *cc;
    const PalSpawnSys *sys = c.sys;
    int64_t p[2];
    if (!pal_pipe(p, 0, err))
        return -1;
    c.errfd = (int)p[1];

    /* The pipe the parent tells the child on that the maps are written. */
    int64_t mp[2] = {-1, -1};
    if (sys->uid_map != NULL || sys->gid_map != NULL) {
        if (!pal_pipe(mp, 0, err)) {
            pal_close(p[0], NULL);
            pal_close(p[1], NULL);
            return -1;
        }
        c.mapfd = (int)mp[0];
    }

    int32_t pidfd = -1;
    sigset_t all, old;
    sigfillset(&all);
    pthread_rwlock_wrlock(&fork_lock);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    pid_t pid = proc_fork(sys, &pidfd);
    if (pid == 0) {
        if (mp[1] >= 0)
            close((int)mp[1]);
        int e = proc_spawn_child(&c);
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
    if (mp[0] >= 0)
        pal_close(mp[0], NULL);
    if (pid < 0) {
        pal_close(p[0], NULL);
        if (mp[1] >= 0)
            pal_close(mp[1], NULL);
        BURROW_OUT(err, burrow__pal_errno(forked));
        return -1;
    }

#if defined(BURROW_OS_LINUX)
    /* A user namespace unshare made, the child maps itself. One clone made
     * is mapped from out here. */
    if (mp[1] >= 0) {
        int32_t e2 = 0;
        if ((sys->unshareflags & PROC_CLONE_NEWUSER) == 0)
            e2 = proc_write_maps(pid, sys);
        ssize_t w;
        do {
            w = write((int)mp[1], &e2, sizeof e2);
        } while (w < 0 && errno == EINTR);
        pal_close(mp[1], NULL);
    }
#endif

    int e = 0;
    ssize_t n;
    do {
        n = read((int)p[0], &e, sizeof e);
    } while (n < 0 && errno == EINTR);
    pal_close(p[0], NULL);
    if (n == 0) {
        if (sys->pidfd != NULL)
            *sys->pidfd = pidfd;
        return (int64_t)pid;
    }

    /* The exec failed, and the child has exited or is about to. Reap it here,
     * since the caller was never told there was anything to wait for. */
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (pidfd >= 0)
        close(pidfd);
    if (sys->pidfd != NULL)
        *sys->pidfd = -1;
    BURROW_OUT(err, burrow__pal_errno(n == (ssize_t)sizeof e ? e : EPIPE));
    return -1;
}

bool pal_set_ids(PalSetID which, uint32_t a, uint32_t b, uint32_t c, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    int r;
    switch (which) {
    case PAL_SETUID:
        r = setuid((uid_t)a);
        break;
    case PAL_SETGID:
        r = setgid((gid_t)a);
        break;
    case PAL_SETEUID:
        r = seteuid((uid_t)a);
        break;
    case PAL_SETEGID:
        r = setegid((gid_t)a);
        break;
    case PAL_SETREUID:
        r = setreuid((uid_t)a, (uid_t)b);
        break;
    case PAL_SETREGID:
        r = setregid((gid_t)a, (gid_t)b);
        break;
#if defined(BURROW_OS_LINUX)
    case PAL_SETRESUID:
        r = setresuid((uid_t)a, (uid_t)b, (uid_t)c);
        break;
    case PAL_SETRESGID:
        r = setresgid((gid_t)a, (gid_t)b, (gid_t)c);
        break;
#else
    case PAL_SETRESUID:
    case PAL_SETRESGID:
        BURROW_OUT(err, PAL_ENOSYS);
        return false;
#endif
    default:
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    (void)b;
    (void)c;
    if (r != 0)
        return proc_fail(err);
    return true;
}

bool pal_setgroups(const uint32_t *gids, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (n < 0 || n > INT_MAX) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    /* gid_t is 32 bits unsigned on every system this runs on. */
    _Static_assert(sizeof(gid_t) == sizeof(uint32_t), "gid_t is not 32 bits");
    const gid_t *list = (const gid_t *)(const void *)gids;
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO)
    int r = setgroups((size_t)n, list);
#else
    int r = setgroups((int)n, list);
#endif
    if (r != 0)
        return proc_fail(err);
    return true;
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
