/* The Linux functions Go writes by hand rather than generates: the path calls
 * that become their at forms with AT_FDCWD, Faccessat with the checks the
 * kernel's faccessat leaves out, the ptrace helpers, ParseDirent, and the
 * calls a few architectures make their own way.
 *
 * Derived from Go's src/syscall/syscall_linux.go, the syscall_linux_ file for
 * each architecture, dirent.go, rlimit.go and flock_linux.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)

#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include "internal.h"

#include <stddef.h>
#include <string.h>

/* Go's own names for these, which the C library spells AT_. */
#define LINUX_AT_FDCWD (-0x64)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EACCESS 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

/* Sets f to v, whatever width f is on this architecture. */
#define LINUX_SET(f, v)                                                                \
    ((f) = _Generic((f),                                                               \
         int32_t: (int32_t)(v),                                                        \
         int64_t: (int64_t)(v),                                                        \
         uint32_t: (uint32_t)(v),                                                      \
         uint64_t: (uint64_t)(v)))

static Error linux_einval(void) {
    return burrow__syscall_errno_err(SYSCALL_EINVAL);
}

/* The Errno in err, or 0 when it is not one. */
static SyscallErrno linux_errno(Error err) {
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    return e != NULL ? *e : 0;
}

/* ----------------------------------------------------------- path calls */

Error syscall_access(Str path, uint32_t mode) {
    return syscall_faccessat(LINUX_AT_FDCWD, path, mode, 0);
}

Error syscall_chmod(Str path, uint32_t mode) {
    return syscall_fchmodat(LINUX_AT_FDCWD, path, mode, 0);
}

Error syscall_chown(Str path, Int uid, Int gid) {
    return syscall_fchownat(LINUX_AT_FDCWD, path, uid, gid, 0);
}

Int syscall_creat(Str path, uint32_t mode, Error *err) {
    return syscall_open(path, SYSCALL_O_CREAT | SYSCALL_O_WRONLY | SYSCALL_O_TRUNC,
                        mode, err);
}

Int syscall_epoll_create(Int size, Error *err) {
    if (size <= 0) {
        BURROW_OUT(err, linux_einval());
        return -1;
    }
    return syscall_epoll_create1(0, err);
}

/* Go's caps, for capget. */
typedef struct LinuxCapHeader {
    uint32_t version;
    int32_t pid;
} LinuxCapHeader;

typedef struct LinuxCapData {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} LinuxCapData;

static bool linux_is_group_member(Int gid) {
    Error err = BURROW_NO_ERROR;
    Slice groups = syscall_getgroups(heap_allocator(), &err);
    if (BURROW_FAILED(err))
        return false;
    bool found = false;
    for (Int i = 0; i < groups.len && !found; i++)
        found = ((const Int *)groups.p)[i] == gid;
    if (groups.p != NULL)
        mem_free(heap_allocator(), groups.p, (size_t)groups.cap * groups.elem->size,
                 groups.elem->align);
    return found;
}

static bool linux_is_cap_dac_override_set(void) {
    const uint32_t cap_dac_override = 1;
    LinuxCapHeader hdr = {0x20080522, 0}; /* _LINUX_CAPABILITY_VERSION_3 */
    LinuxCapData data[2];
    memset(data, 0, sizeof data);
    SyscallErrno e = 0;
    syscall_raw_syscall(SYSCALL_SYS_CAPGET, (Uintptr)&hdr, (Uintptr)&data[0], 0, NULL,
                        &e);
    return e == 0 && (data[0].effective & (1U << (cap_dac_override & 31))) != 0;
}

/* The fstatat the architecture has, which mips64 and loong64 do not, yet. */
static Error linux_fstatat(Int dirfd, Str path, SyscallStat_t *st, Int flags) {
#if defined(BURROW_ARCH_LOONG64) || defined(BURROW_ARCH_MIPS64)
    (void)dirfd, (void)path, (void)st, (void)flags;
    return burrow__syscall_errno_err(SYSCALL_ENOSYS);
#else
    return burrow__syscall_fstatat(dirfd, path, st, flags);
#endif
}

Error syscall_faccessat(Int dirfd, Str path, uint32_t mode, Int flags) {
    if (flags == 0)
        return burrow__syscall_faccessat(dirfd, path, mode);

    /* Go tries faccessat2 first, everywhere but Android, which does not let
     * it through. */
#if !defined(BURROW_OS_ANDROID)
    Error err = burrow__syscall_faccessat2(dirfd, path, mode, flags);
    SyscallErrno e = linux_errno(err);
    if (!BURROW_FAILED(err) || (e != SYSCALL_ENOSYS && e != SYSCALL_EPERM))
        return err;
#endif

    /* The kernel's faccessat takes no flags, and the C library does the
     * flags itself, so this does what it does. */
    if ((flags & ~(Int)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EACCESS)) != 0)
        return linux_einval();

    SyscallStat_t st;
    memset(&st, 0, sizeof st);
    Error serr = linux_fstatat(dirfd, path, &st, flags & LINUX_AT_SYMLINK_NOFOLLOW);
    if (BURROW_FAILED(serr))
        return serr;

    mode &= 7;
    if (mode == 0)
        return BURROW_NO_ERROR;

    Int uid;
    if ((flags & LINUX_AT_EACCESS) != 0) {
        uid = syscall_geteuid();
        /* With CAP_DAC_OVERRIDE the kernel checks as it does for root. */
        if (uid != 0 && linux_is_cap_dac_override_set())
            uid = 0;
    } else {
        uid = syscall_getuid();
    }

    if (uid == 0) {
        if ((mode & 1) == 0)
            return BURROW_NO_ERROR; /* root can read and write anything */
        if ((st.mode & 0111) != 0)
            return BURROW_NO_ERROR; /* and run what anybody can run */
        return burrow__syscall_errno_err(SYSCALL_EACCES);
    }

    uint32_t fmode;
    if ((uint32_t)uid == st.uid) {
        fmode = (st.mode >> 6) & 7;
    } else {
        Int gid =
            (flags & LINUX_AT_EACCESS) != 0 ? syscall_getegid() : syscall_getgid();
        if ((uint32_t)gid == st.gid || linux_is_group_member((Int)st.gid))
            fmode = (st.mode >> 3) & 7;
        else
            fmode = st.mode & 7;
    }
    if ((fmode & mode) == mode)
        return BURROW_NO_ERROR;
    return burrow__syscall_errno_err(SYSCALL_EACCES);
}

Error syscall_fchmodat(Int dirfd, Str path, uint32_t mode, Int flags) {
    if (flags == 0)
        return burrow__syscall_fchmodat(dirfd, path, mode);
    /* Only fchmodat2 takes flags. Without it, flags that are known to be
     * good are EOPNOTSUPP rather than ENOSYS. */
    Error err = burrow__syscall_fchmodat2(dirfd, path, mode, flags);
    if (linux_errno(err) == SYSCALL_ENOSYS) {
        if ((flags & ~(Int)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
            return linux_einval();
        if ((flags & (LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
            return burrow__syscall_errno_err(SYSCALL_EOPNOTSUPP);
    }
    return err;
}

Error syscall_link(Str oldpath, Str newpath) {
    return burrow__syscall_linkat(LINUX_AT_FDCWD, oldpath, LINUX_AT_FDCWD, newpath, 0);
}

Error syscall_mkdir(Str path, uint32_t mode) {
    return syscall_mkdirat(LINUX_AT_FDCWD, path, mode);
}

Error syscall_mknod(Str path, uint32_t mode, Int dev) {
    return syscall_mknodat(LINUX_AT_FDCWD, path, mode, dev);
}

Error syscall_mkfifo(Str path, uint32_t mode) {
    return syscall_mknod(path, mode | SYSCALL_S_IFIFO, 0);
}

Int syscall_open(Str path, Int mode, uint32_t perm, Error *err) {
    return burrow__syscall_openat(LINUX_AT_FDCWD, path, mode | SYSCALL_O_LARGEFILE,
                                  perm, err);
}

Int syscall_openat(Int dirfd, Str path, Int flags, uint32_t mode, Error *err) {
    return burrow__syscall_openat(dirfd, path, flags | SYSCALL_O_LARGEFILE, mode, err);
}

Error syscall_pipe(Slice p) {
    return syscall_pipe2(p, 0);
}

Error syscall_pipe2(Slice p, Int flags) {
    if (p.len != 2)
        return linux_einval();
    int32_t pp[2] = {0, 0};
    Error err = burrow__syscall_pipe2(pp, flags);
    if (!BURROW_FAILED(err)) {
        ((Int *)p.p)[0] = pp[0];
        ((Int *)p.p)[1] = pp[1];
    }
    return err;
}

Int syscall_readlink(Str path, Slice buf, Error *err) {
    return burrow__syscall_readlinkat(LINUX_AT_FDCWD, path, buf, err);
}

#if defined(BURROW_ARCH_RISCV64)
/* riscv64 has no renameat, only renameat2. */
Error syscall_renameat(Int olddirfd, Str oldpath, Int newdirfd, Str newpath) {
    return burrow__syscall_renameat2(olddirfd, oldpath, newdirfd, newpath, 0);
}
#endif

Error syscall_rename(Str oldpath, Str newpath) {
    return syscall_renameat(LINUX_AT_FDCWD, oldpath, LINUX_AT_FDCWD, newpath);
}

Error syscall_rmdir(Str path) {
    return burrow__syscall_unlinkat(LINUX_AT_FDCWD, path, LINUX_AT_REMOVEDIR);
}

Error syscall_symlink(Str oldpath, Str newpath) {
    return burrow__syscall_symlinkat(oldpath, LINUX_AT_FDCWD, newpath);
}

Error syscall_unlink(Str path) {
    return burrow__syscall_unlinkat(LINUX_AT_FDCWD, path, 0);
}

Error syscall_unlinkat(Int dirfd, Str path) {
    return burrow__syscall_unlinkat(dirfd, path, 0);
}

/* ---------------------------------------------------------------- times */

#if !defined(BURROW_ARCH_386) && !defined(BURROW_ARCH_AMD64) &&                        \
    !defined(BURROW_ARCH_ARM) && !defined(BURROW_ARCH_MIPS64) &&                       \
    !defined(BURROW_ARCH_PPC64) && !defined(BURROW_ARCH_S390X)
/* The architectures with no utimes or futimesat, which Go does with
 * utimensat. */
static Error linux_utimensat_tv(Int dirfd, Str path, SyscallTimeval *tv) {
    if (tv == NULL)
        return burrow__syscall_utimensat(dirfd, path, NULL, 0);
    SyscallTimespec ts[2];
    ts[0] = syscall_nsec_to_timespec(syscall_timeval_to_nsec(tv[0]));
    ts[1] = syscall_nsec_to_timespec(syscall_timeval_to_nsec(tv[1]));
    return burrow__syscall_utimensat(dirfd, path, ts, 0);
}

static Error linux_utimes(Str path, SyscallTimeval *tv) {
    return linux_utimensat_tv(LINUX_AT_FDCWD, path, tv);
}

static Error linux_futimesat(Int dirfd, Str path, SyscallTimeval *tv) {
    return linux_utimensat_tv(dirfd, path, tv);
}

Error syscall_utime(Str path, SyscallUtimbuf *buf) {
    SyscallTimeval tv[2];
    memset(tv, 0, sizeof tv);
    LINUX_SET(tv[0].sec, buf->actime);
    LINUX_SET(tv[1].sec, buf->modtime);
    return syscall_utimes(path, (Slice){tv, 2, 2, NULL});
}
#else
static Error linux_utimes(Str path, SyscallTimeval *tv) {
    return burrow__syscall_utimes(path, tv);
}

static Error linux_futimesat(Int dirfd, Str path, SyscallTimeval *tv) {
    return burrow__syscall_futimesat(dirfd, path, tv);
}
#endif

Error syscall_utimes(Str path, Slice tv) {
    if (tv.len != 2)
        return linux_einval();
    return linux_utimes(path, (SyscallTimeval *)tv.p);
}

Error syscall_utimes_nano(Str path, Slice ts) {
    if (ts.len != 2)
        return linux_einval();
    return burrow__syscall_utimensat(LINUX_AT_FDCWD, path, (SyscallTimespec *)ts.p, 0);
}

Error syscall_futimesat(Int dirfd, Str path, Slice tv) {
    if (tv.len != 2)
        return linux_einval();
    return linux_futimesat(dirfd, path, (SyscallTimeval *)tv.p);
}

Error syscall_futimes(Int fd, Slice tv) {
    /* The best Linux can do, and what the C library does too. */
    char buf[40];
    const char *prefix = "/proc/self/fd/";
    size_t n = strlen(prefix);
    memcpy(buf, prefix, n);
    char digits[24];
    size_t d = 0;
    Uint u = fd < 0 ? (Uint)0 - (Uint)fd : (Uint)fd;
    do {
        digits[d++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (fd < 0)
        buf[n++] = '-';
    while (d > 0)
        buf[n++] = digits[--d];
    return syscall_utimes(str_from_bytes((const Byte *)buf, (Int)n), tv);
}

#if defined(BURROW_ARCH_AMD64)
/* Go reaches gettimeofday through the vDSO on amd64. The system call gives
 * the same answer. */
Error syscall_gettimeofday(SyscallTimeval *tv) {
    SyscallErrno e = 0;
    syscall_raw_syscall(SYSCALL_SYS_GETTIMEOFDAY, (Uintptr)tv, 0, 0, NULL, &e);
    return e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR;
}
#endif

#if !defined(BURROW_ARCH_386) && !defined(BURROW_ARCH_ARM) &&                          \
    !defined(BURROW_ARCH_PPC64)
SyscallTime_t syscall_time(SyscallTime_t *t, Error *err) {
    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    Error e = syscall_gettimeofday(&tv);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    if (t != NULL)
        *t = (SyscallTime_t)tv.sec;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (SyscallTime_t)tv.sec;
}
#endif

/* ------------------------------------------------------- stat and chown */

#if !defined(BURROW_ARCH_PPC64) && !defined(BURROW_ARCH_S390X) &&                      \
    !defined(BURROW_ARCH_LOONG64) && !defined(BURROW_ARCH_MIPS64)
/* Go uses fstatat for these, since Android's seccomp policy blocks stat. */
Error syscall_stat(Str path, SyscallStat_t *stat) {
    return burrow__syscall_fstatat(LINUX_AT_FDCWD, path, stat, 0);
}

Error syscall_lstat(Str path, SyscallStat_t *stat) {
    return burrow__syscall_fstatat(LINUX_AT_FDCWD, path, stat,
                                   LINUX_AT_SYMLINK_NOFOLLOW);
}
#endif

#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64)
Error syscall_fstatat(Int fd, Str path, SyscallStat_t *stat, Int flags) {
    return burrow__syscall_fstatat(fd, path, stat, flags);
}
#endif

#if !defined(BURROW_ARCH_MIPS64) && !defined(BURROW_ARCH_PPC64) &&                     \
    !defined(BURROW_ARCH_S390X)
Error syscall_lchown(Str path, Int uid, Int gid) {
    return syscall_fchownat(LINUX_AT_FDCWD, path, uid, gid, LINUX_AT_SYMLINK_NOFOLLOW);
}
#endif

/* ------------------------------------------------- what 386 and arm add */

#if defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM)
/* Go's seek, which is _llseek, with the offset in two halves and the result
 * written through a pointer. */
int64_t syscall_seek(Int fd, int64_t offset, Int whence, Error *err) {
    int64_t newoffset = 0;
    SyscallErrno e = 0;
    syscall_syscall6(SYSCALL_SYS__LLSEEK, (Uintptr)fd,
                     (Uintptr)((uint64_t)offset >> 32),
                     (Uintptr)((uint64_t)offset & 0xffffffffU), (Uintptr)&newoffset,
                     (Uintptr)whence, 0, NULL, &e);
    if (e != 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(e));
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return newoffset;
}

Error syscall_fstatfs(Int fd, SyscallStatfs_t *buf) {
    SyscallErrno e = 0;
    syscall_syscall(SYSCALL_SYS_FSTATFS64, (Uintptr)fd, sizeof *buf, (Uintptr)buf, NULL,
                    &e);
    return e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR;
}

Error syscall_statfs(Str path, SyscallStatfs_t *buf) {
    Error err = BURROW_NO_ERROR;
    uint8_t *p = syscall_byte_ptr_from_string(heap_allocator(), path, &err);
    if (BURROW_FAILED(err))
        return err;
    SyscallErrno e = 0;
    syscall_syscall(SYSCALL_SYS_STATFS64, (Uintptr)p, sizeof *buf, (Uintptr)buf, NULL,
                    &e);
    mem_free(heap_allocator(), p, (size_t)path.len + 1, 1);
    return e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR;
}
#endif

/* ------------------------------- what the newer architectures do by hand */

#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64) ||                      \
    defined(BURROW_ARCH_LOONG64) || defined(BURROW_ARCH_MIPS64)
/* select is pselect6 here, with the timeout as a Timespec. */
Int syscall_select(Int nfd, SyscallFdSet *r, SyscallFdSet *w, SyscallFdSet *e,
                   SyscallTimeval *timeout, Error *err) {
    SyscallTimespec ts;
    SyscallTimespec *tsp = NULL;
    if (timeout != NULL) {
        memset(&ts, 0, sizeof ts);
        LINUX_SET(ts.sec, timeout->sec);
        LINUX_SET(ts.nsec, timeout->usec * 1000);
        tsp = &ts;
    }
    SyscallErrno en = 0;
    Uintptr n = syscall_syscall6(SYSCALL_SYS_PSELECT6, (Uintptr)nfd, (Uintptr)r,
                                 (Uintptr)w, (Uintptr)e, (Uintptr)tsp, 0, NULL, &en);
    BURROW_OUT(err, en != 0 ? burrow__syscall_errno_err(en) : BURROW_NO_ERROR);
    return (Int)n;
}
#endif

#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64) ||                      \
    defined(BURROW_ARCH_LOONG64)
Int syscall_inotify_init(Error *err) {
    return syscall_inotify_init1(0, err);
}

/* pause is ppoll with nothing to wait for. */
Error syscall_pause(void) {
    SyscallErrno e = 0;
    syscall_syscall6(SYSCALL_SYS_PPOLL, 0, 0, 0, 0, 0, 0, NULL, &e);
    return e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR;
}
#endif

#if defined(BURROW_ARCH_PPC64)
/* ppc64 has the flags before the offsets. */
Error syscall_sync_file_range(Int fd, int64_t off, int64_t n, Int flags) {
    return burrow__syscall_sync_file_range2(fd, flags, off, n);
}
#endif

/* ------------------------------------------------------------ the rest */

Str syscall_getwd(Alloc *a, Error *err) {
    Byte buf[SYSCALL_PATH_MAX];
    Error e = BURROW_NO_ERROR;
    Int n =
        syscall_getcwd((Slice){buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE}, &e);
    Str nil = {NULL, 0};
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    /* Getcwd counts the NUL. */
    if (n < 1 || n > (Int)sizeof buf || buf[n - 1] != 0) {
        BURROW_OUT(err, linux_einval());
        return nil;
    }
    /* Linux can give a path that starts "(unreachable)", which could be a
     * real relative path, so anything that is not absolute is ENOENT. */
    if (buf[0] != '/') {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_ENOENT));
        return nil;
    }
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(n - 1), 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    memcpy(p, buf, (size_t)(n - 1));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str_from_bytes(p, n - 1);
}

Slice syscall_getgroups(Alloc *a, Error *err) {
    Slice nil = slice_nil(TYPE_INT);
    Error e = BURROW_NO_ERROR;
    Int n = burrow__syscall_getgroups(0, NULL, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (n == 0)
        return nil;
    /* The most Linux allows is 1<<16. */
    if (n < 0 || n > 1 << 20) {
        BURROW_OUT(err, linux_einval());
        return nil;
    }
    uint32_t *list = (uint32_t *)mem_alloc_array(heap_allocator(), (size_t)n,
                                                 sizeof *list, _Alignof(uint32_t));
    if (list == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Int got = burrow__syscall_getgroups(n, list, &e);
    Slice gids = nil;
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
    } else {
        gids = slice_make(a, TYPE_INT, got, got);
        if (gids.p == NULL && got > 0)
            BURROW_OUT(err, burrow_err_out_of_memory);
        for (Int i = 0; i < got && gids.p != NULL; i++)
            ((Int *)gids.p)[i] = (Int)list[i];
    }
    mem_free(heap_allocator(), list, (size_t)n * sizeof *list, _Alignof(uint32_t));
    return gids;
}

Error syscall_mount(Str source, Str target, Str fstype, Uintptr flags, Str data) {
    if (data.len == 0)
        return burrow__syscall_mount(source, target, fstype, flags, NULL);
    Error err = BURROW_NO_ERROR;
    uint8_t *p = syscall_byte_ptr_from_string(heap_allocator(), data, &err);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__syscall_mount(source, target, fstype, flags, p);
    mem_free(heap_allocator(), p, (size_t)data.len + 1, 1);
    return err;
}

Error syscall_reboot(Int cmd) {
    return burrow__syscall_reboot(SYSCALL_LINUX_REBOOT_MAGIC1,
                                  SYSCALL_LINUX_REBOOT_MAGIC2, cmd, (Str){NULL, 0});
}

Int syscall_getpgrp(void) {
    return syscall_getpgid(0, NULL);
}

Error syscall_getrlimit(Int resource, SyscallRlimit *rlim) {
    return burrow__syscall_prlimit1(0, resource, NULL, rlim);
}

/* Go forgets the RLIMIT_NOFILE it raised at start when this changes it.
 * burrow does not raise it, so there is nothing to forget. */
Error syscall_setrlimit(Int resource, SyscallRlimit *rlim) {
    return burrow__syscall_prlimit1(0, resource, rlim, NULL);
}

Int syscall_sendfile(Int outfd, Int infd, int64_t *offset, Int count, Error *err) {
    return burrow__syscall_sendfile(outfd, infd, offset, count, err);
}

Error syscall_fcntl_flock(Uintptr fd, Int cmd, SyscallFlock_t *lk) {
#if defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM)
    Uintptr trap = SYSCALL_SYS_FCNTL64;
#else
    Uintptr trap = SYSCALL_SYS_FCNTL;
#endif
    SyscallErrno e = 0;
    syscall_syscall(trap, fd, (Uintptr)cmd, (Uintptr)lk, NULL, &e);
    return e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- dirents */

Int syscall_read_dirent(Int fd, Slice buf, Error *err) {
    return syscall_getdents(fd, buf, err);
}

/* Go's readInt: the size bytes at off in b, in the machine's order, and
 * false if b is too short. */
static bool linux_read_int(Slice b, size_t off, size_t size, uint64_t *u) {
    if ((size_t)b.len < off + size)
        return false;
    const uint8_t *p = (const uint8_t *)b.p + off;
    uint64_t v = 0;
    for (size_t i = 0; i < size; i++) {
#if BURROW_BIG_ENDIAN
        v = v << 8 | p[i];
#else
        v |= (uint64_t)p[i] << (8 * i);
#endif
    }
    *u = v;
    return true;
}

Int syscall_parse_dirent(Alloc *a, Slice buf, Int max, Slice names, Int *count,
                         Slice *newnames) {
    const size_t namoff = offsetof(SyscallDirent, name);
    Int origlen = buf.len;
    Int c = 0;
    while (max != 0 && buf.len > 0) {
        uint64_t reclen = 0;
        if (!linux_read_int(buf, offsetof(SyscallDirent, reclen),
                            sizeof(((SyscallDirent *)0)->reclen), &reclen) ||
            reclen > (uint64_t)buf.len) {
            BURROW_OUT(count, c);
            BURROW_OUT(newnames, names);
            return origlen;
        }
        Slice rec = {buf.p, (Int)reclen, (Int)reclen, buf.elem};
        buf.p = (uint8_t *)buf.p + reclen;
        buf.len -= (Int)reclen;
        buf.cap -= (Int)reclen;
        uint64_t ino = 0;
        if (!linux_read_int(rec, offsetof(SyscallDirent, ino),
                            sizeof(((SyscallDirent *)0)->ino), &ino))
            break;
        /* Linux keeps entries whose inode is 0, where the BSDs skip them. */
        if (reclen < namoff)
            break;
        uint64_t namlen = reclen - namoff;
        if (namoff + namlen > (uint64_t)rec.len)
            break;
        const Byte *name = (const Byte *)rec.p + namoff;
        const Byte *nul = (const Byte *)memchr(name, 0, (size_t)namlen);
        Int len = nul != NULL ? (Int)(nul - name) : (Int)namlen;
        if ((len == 1 && name[0] == '.') ||
            (len == 2 && name[0] == '.' && name[1] == '.'))
            continue;
        Byte *copy = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
        if (copy == NULL && len > 0)
            break;
        if (len > 0)
            memcpy(copy, name, (size_t)len);
        Str s = str_from_bytes(copy, len);
        max--;
        c++;
        names = slice_append(a, names, &s, 1);
    }
    BURROW_OUT(count, c);
    BURROW_OUT(newnames, names);
    return origlen - buf.len;
}

/* ---------------------------------------------------------------- ptrace */

static Int linux_ptrace_peek(Int req, Int pid, Uintptr addr, Slice out, Error *err) {
    enum { W = sizeof(Uintptr) };
    uint8_t buf[W];
    Int n = 0;
    uint8_t *o = (uint8_t *)out.p;
    Int left = out.len;
    if (addr % W != 0) {
        Error e = burrow__syscall_ptrace_ptr(req, pid, addr - addr % W, buf);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        Int k = (Int)(W - addr % W);
        if (k > left)
            k = left;
        memcpy(o, buf + addr % W, (size_t)k);
        n += k;
        o += k;
        left -= k;
    }
    while (left > 0) {
        Error e = burrow__syscall_ptrace_ptr(req, pid, addr + (Uintptr)n, buf);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        Int k = left < (Int)W ? left : (Int)W;
        memcpy(o, buf, (size_t)k);
        n += k;
        o += k;
        left -= k;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int syscall_ptrace_peek_text(Int pid, Uintptr addr, Slice out, Error *err) {
    return linux_ptrace_peek(SYSCALL_PTRACE_PEEKTEXT, pid, addr, out, err);
}

Int syscall_ptrace_peek_data(Int pid, Uintptr addr, Slice out, Error *err) {
    return linux_ptrace_peek(SYSCALL_PTRACE_PEEKDATA, pid, addr, out, err);
}

static Int linux_ptrace_poke(Int poke, Int peek, Int pid, Uintptr addr, Slice data,
                             Error *err) {
    enum { W = sizeof(Uintptr) };
    Int n = 0;
    const uint8_t *d = (const uint8_t *)data.p;
    Int left = data.len;
    Error e;
    if (addr % W != 0) {
        uint8_t buf[W];
        e = burrow__syscall_ptrace_ptr(peek, pid, addr - addr % W, buf);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        Int k = (Int)(W - addr % W);
        if (k > left)
            k = left;
        memcpy(buf + addr % W, d, (size_t)k);
        n += k;
        Uintptr word;
        memcpy(&word, buf, W);
        e = burrow__syscall_ptrace(poke, pid, addr - addr % W, word);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        d += k;
        left -= k;
    }
    while (left > (Int)W) {
        Uintptr word;
        memcpy(&word, d, W);
        e = burrow__syscall_ptrace(poke, pid, addr + (Uintptr)n, word);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        n += W;
        d += W;
        left -= W;
    }
    if (left > 0) {
        uint8_t buf[W];
        e = burrow__syscall_ptrace_ptr(peek, pid, addr + (Uintptr)n, buf);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        memcpy(buf, d, (size_t)left);
        Uintptr word;
        memcpy(&word, buf, W);
        e = burrow__syscall_ptrace(poke, pid, addr + (Uintptr)n, word);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        n += left;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int syscall_ptrace_poke_text(Int pid, Uintptr addr, Slice data, Error *err) {
    return linux_ptrace_poke(SYSCALL_PTRACE_POKETEXT, SYSCALL_PTRACE_PEEKTEXT, pid,
                             addr, data, err);
}

Int syscall_ptrace_poke_data(Int pid, Uintptr addr, Slice data, Error *err) {
    return linux_ptrace_poke(SYSCALL_PTRACE_POKEDATA, SYSCALL_PTRACE_PEEKDATA, pid,
                             addr, data, err);
}

#define LINUX_NT_PRSTATUS 1

Error syscall_ptrace_get_regs(Int pid, SyscallPtraceRegs *regsout) {
    SyscallIovec iov;
    memset(&iov, 0, sizeof iov);
    iov.base = (uint8_t *)regsout;
    syscall_iovec_set_len(&iov, (Int)sizeof *regsout);
    return burrow__syscall_ptrace_ptr(SYSCALL_PTRACE_GETREGSET, pid, LINUX_NT_PRSTATUS,
                                      &iov);
}

Error syscall_ptrace_set_regs(Int pid, SyscallPtraceRegs *regs) {
    SyscallIovec iov;
    memset(&iov, 0, sizeof iov);
    iov.base = (uint8_t *)regs;
    syscall_iovec_set_len(&iov, (Int)sizeof *regs);
    return burrow__syscall_ptrace_ptr(SYSCALL_PTRACE_SETREGSET, pid, LINUX_NT_PRSTATUS,
                                      &iov);
}

Error syscall_ptrace_set_options(Int pid, Int options) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_SETOPTIONS, pid, 0, (Uintptr)options);
}

Uint syscall_ptrace_get_event_msg(Int pid, Error *err) {
    long data = 0;
    Error e = burrow__syscall_ptrace_ptr(SYSCALL_PTRACE_GETEVENTMSG, pid, 0, &data);
    BURROW_OUT(err, e);
    return (Uint)data;
}

Error syscall_ptrace_cont(Int pid, Int signal) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_CONT, pid, 0, (Uintptr)signal);
}

Error syscall_ptrace_syscall(Int pid, Int signal) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_SYSCALL, pid, 0, (Uintptr)signal);
}

Error syscall_ptrace_single_step(Int pid) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_SINGLESTEP, pid, 0, 0);
}

Error syscall_ptrace_attach(Int pid) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_ATTACH, pid, 0, 0);
}

Error syscall_ptrace_detach(Int pid) {
    return burrow__syscall_ptrace(SYSCALL_PTRACE_DETACH, pid, 0, 0);
}

#if defined(BURROW_ARCH_LOONG64)
uint64_t syscall_ptrace_regs_get_era(const SyscallPtraceRegs *r) {
    return r->era;
}

void syscall_ptrace_regs_set_era(SyscallPtraceRegs *r, uint64_t era) {
    r->era = era;
}
#else
uint64_t syscall_ptrace_regs_pc(const SyscallPtraceRegs *r) {
#if defined(BURROW_ARCH_AMD64)
    return r->rip;
#elif defined(BURROW_ARCH_386)
    return (uint64_t)(uint32_t)r->eip;
#elif defined(BURROW_ARCH_ARM)
    return (uint64_t)r->uregs[15];
#elif defined(BURROW_ARCH_MIPS64)
    return r->regs[64];
#elif defined(BURROW_ARCH_PPC64)
    return r->nip;
#elif defined(BURROW_ARCH_S390X)
    return r->psw.addr;
#else
    return r->pc;
#endif
}

void syscall_ptrace_regs_set_pc(SyscallPtraceRegs *r, uint64_t pc) {
#if defined(BURROW_ARCH_AMD64)
    r->rip = pc;
#elif defined(BURROW_ARCH_386)
    r->eip = (int32_t)pc;
#elif defined(BURROW_ARCH_ARM)
    r->uregs[15] = (uint32_t)pc;
#elif defined(BURROW_ARCH_MIPS64)
    r->regs[64] = pc;
#elif defined(BURROW_ARCH_PPC64)
    r->nip = pc;
#elif defined(BURROW_ARCH_S390X)
    r->psw.addr = pc;
#else
    r->pc = pc;
#endif
}
#endif

#endif
