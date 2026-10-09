/* The functions Go writes by hand on macOS and FreeBSD: Getwd, Getgroups,
 * Pipe, the times, Getdirentries and ReadDirent, Kevent, Sysctl, Sendfile,
 * Getfsstat and the rest that wrap a generated call. macOS has no
 * getdirentries a program may call, so Go builds it from fdopendir and
 * readdir_r, and burrow does the same.
 *
 * Derived from Go's src/syscall/syscall_bsd.go, syscall_darwin.go,
 * syscall_freebsd.go, the syscall_darwin_ and syscall_freebsd_ file for each
 * architecture, flock_bsd.go and rlimit.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)

#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include "internal.h"

#include <string.h>

/* Go's pathMax, and _AT_FDCWD and _AT_SYMLINK_NOFOLLOW, which it does not
 * export. */
#define BSD_PATH_MAX 1024
#if defined(BURROW_OS_FREEBSD)
#define BSD_AT_FDCWD (-100)
#define BSD_AT_SYMLINK_NOFOLLOW 0x200
#else
#define BSD_AT_FDCWD (-2)
#endif

static Error bsd_errno(SyscallErrno e) {
    return burrow__syscall_errno_err(e);
}

/* -------------------------------------------------- the working directory */

Str syscall_getwd(Alloc *a, Error *err) {
    Byte buf[BSD_PATH_MAX];
    Str nil = {NULL, 0};
    Error e = BURROW_NO_ERROR;
    burrow__syscall_getcwd((Slice){buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE},
                           &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    const Byte *nul = (const Byte *)memchr(buf, 0, sizeof buf);
    Int n = nul != NULL ? (Int)(nul - buf) : (Int)sizeof buf;
    if (n < 1) {
        BURROW_OUT(err, bsd_errno(SYSCALL_EINVAL));
        return nil;
    }
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    memcpy(p, buf, (size_t)n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str_from_bytes(p, n);
}

/* ---------------------------------------------------------------- groups */

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
    /* The most the BSDs allow is 16, so this is only a sanity check. */
    if (n < 0 || n > 1000) {
        BURROW_OUT(err, bsd_errno(SYSCALL_EINVAL));
        return nil;
    }
    uint32_t list[1000];
    Int got = burrow__syscall_getgroups(n, list, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    Slice gids = slice_make(a, TYPE_INT, got, got);
    if (gids.p == NULL && got > 0) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    for (Int i = 0; i < got; i++)
        ((Int *)gids.p)[i] = (Int)list[i];
    return gids;
}

/* ----------------------------------------------------------------- pipes */

#if defined(BURROW_OS_FREEBSD)
Error syscall_pipe(Slice p) {
    return syscall_pipe2(p, 0);
}

Error syscall_pipe2(Slice p, Int flags) {
    if (p.len != 2)
        return bsd_errno(SYSCALL_EINVAL);
    int32_t pp[2] = {0, 0};
    Error err = burrow__syscall_pipe2(pp, flags);
    if (!BURROW_FAILED(err)) {
        ((Int *)p.p)[0] = pp[0];
        ((Int *)p.p)[1] = pp[1];
    }
    return err;
}
#else
Error syscall_pipe(Slice p) {
    if (p.len != 2)
        return bsd_errno(SYSCALL_EINVAL);
    int32_t q[2] = {0, 0};
    Error err = burrow__syscall_pipe(q);
    if (!BURROW_FAILED(err)) {
        ((Int *)p.p)[0] = q[0];
        ((Int *)p.p)[1] = q[1];
    }
    return err;
}
#endif

/* ----------------------------------------------------------------- times */

Error syscall_utimes(Str path, Slice tv) {
    if (tv.len != 2)
        return bsd_errno(SYSCALL_EINVAL);
    return burrow__syscall_utimes(path, (SyscallTimeval *)tv.p);
}

Error syscall_utimes_nano(Str path, Slice ts) {
    if (ts.len != 2)
        return bsd_errno(SYSCALL_EINVAL);
    const SyscallTimespec *t = (const SyscallTimespec *)ts.p;
    SyscallTimespec both[2] = {t[0], t[1]};
    Error err = burrow__syscall_utimensat(BSD_AT_FDCWD, path, both, 0);
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    if (e == NULL || *e != SYSCALL_ENOSYS)
        return err;
    /* A system without utimensat: the same times to the microsecond. */
    SyscallTimeval tv[2] = {
        syscall_nsec_to_timeval(syscall_timespec_to_nsec(t[0])),
        syscall_nsec_to_timeval(syscall_timespec_to_nsec(t[1])),
    };
    return burrow__syscall_utimes(path, tv);
}

Error syscall_futimes(Int fd, Slice tv) {
    if (tv.len != 2)
        return bsd_errno(SYSCALL_EINVAL);
    return burrow__syscall_futimes(fd, (SyscallTimeval *)tv.p);
}

/* --------------------------------------------------------------- dirents */

Int syscall_read_dirent(Int fd, Slice buf, Error *err) {
    /* Getdirentries does not take a NULL base, and 64 bits is enough on any
     * of them. */
    uint64_t base = 0;
    return syscall_getdirentries(fd, buf, (Uintptr *)(void *)&base, err);
}

#if defined(BURROW_OS_FREEBSD)
Int syscall_getdirentries(Int fd, Slice buf, Uintptr *basep, Error *err) {
    if (basep == NULL || sizeof *basep == 8)
        return burrow__syscall_getdirentries(fd, buf, (uint64_t *)(void *)basep, err);
    /* The call wants a 64-bit base, which a 32-bit Uintptr cannot hold. */
    uint64_t base = (uint64_t)*basep;
    Error e = BURROW_NO_ERROR;
    Int n = burrow__syscall_getdirentries(fd, buf, &base, &e);
    *basep = (Uintptr)base;
    /* A base that does not fit would make the next call wrong, and EIO is an
     * error getdirentries is allowed to give. */
    if (base >> 32 != 0)
        e = bsd_errno(SYSCALL_EIO);
    BURROW_OUT(err, e);
    return n;
}
#else
enum { BSD_LIBC_FDOPENDIR, BSD_LIBC_GETFSSTAT, BSD_LIBC_SENDFILE, BSD_NLIBC };

/* The libSystem functions Go writes its own trampolines for, looked up the
 * first time each is called. */
static void *bsd_libc_slots[BSD_NLIBC];

static Uintptr bsd_libc(int fn, const char *name, const uintptr_t *args, int32_t n,
                        burrow__SyscallFail fail, SyscallErrno *e) {
    return burrow__syscall_libc_call(&bsd_libc_slots[fn], name, -1, args, n, fail, NULL,
                                     e);
}

/* macOS has no getdirentries for programs to call, so this does what Go does:
 * it reads the directory with readdir_r on a new descriptor, and keeps how
 * many entries it has given back as fd's offset, so the next call skips them.
 * That is enough for calling it again and again, but not for a directory that
 * changes in between. basep is not used. */
Int syscall_getdirentries(Int fd, Slice buf, Uintptr *basep, Error *err) {
    (void)basep;
    Error e = BURROW_NO_ERROR;
    int64_t skip = syscall_seek(fd, 0, 1, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    /* fdopendir takes the descriptor over, and a dup would share the offset,
     * so openat gives a new one on the same directory. */
    Int fd2 = burrow__syscall_openat(fd, BURROW_S("."), SYSCALL_O_RDONLY, 0, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    SyscallErrno en = 0;
    const uintptr_t args[] = {(uintptr_t)fd2};
    Uintptr d = bsd_libc(BSD_LIBC_FDOPENDIR, "fdopendir", args, 1,
                         BURROW__SYSCALL_FAIL_PTR, &en);
    if (en != 0) {
        Error ignored = syscall_close(fd2);
        (void)ignored;
        BURROW_OUT(err, bsd_errno(en));
        return 0;
    }
    Int n = 0;
    int64_t cnt = 0;
    uint8_t *out = (uint8_t *)buf.p;
    Int left = buf.len;
    for (;;) {
        SyscallDirent entry;
        SyscallDirent *entryp = NULL;
        SyscallErrno re = burrow__syscall_readdir_r(d, &entry, &entryp);
        if (re != 0) {
            Error ignored = burrow__syscall_closedir(d);
            (void)ignored;
            BURROW_OUT(err, bsd_errno(re));
            return n;
        }
        if (entryp == NULL)
            break;
        if (skip > 0) {
            skip--;
            cnt++;
            continue;
        }
        Int reclen = (Int)entry.reclen;
        /* No room for this one. The count says where the next call starts,
         * which makes reading a whole directory quadratic, as it is in Go. */
        if (reclen > left)
            break;
        memcpy(out, &entry, (size_t)reclen);
        out += reclen;
        left -= reclen;
        n += reclen;
        cnt++;
    }
    syscall_seek(fd, cnt, 0, &e);
    Error ignored = burrow__syscall_closedir(d);
    (void)ignored;
    BURROW_OUT(err, e);
    return n;
}
#endif

/* ---------------------------------------------------------------- kevent */

Int syscall_kevent(Int kq, Slice changes, Slice events, SyscallTimespec *timeout,
                   Error *err) {
    void *change = changes.len > 0 ? changes.p : NULL;
    void *event = events.len > 0 ? events.p : NULL;
    return burrow__syscall_kevent(kq, change, changes.len, event, events.len, timeout,
                                  err);
}

void syscall_set_kevent(SyscallKevent_t *k, Int fd, Int mode, Int flags) {
#if defined(BURROW_OS_FREEBSD) && (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM))
    k->ident = (uint32_t)fd;
#else
    k->ident = (uint64_t)fd;
#endif
    k->filter = (int16_t)mode;
    k->flags = (uint16_t)flags;
}

/* ---------------------------------------------------------------- sysctl */

/* Go's nametomib: "kern.hostname" as its numbers, which sysctl gives back when
 * the name is written to the magic {0, 3}. The buffer has two more than
 * CTL_MAXNAME, as the kernel's own does, in case it writes past what it is
 * told. */
static Int bsd_nametomib(Str name, int32_t *mib, Error *err) {
    int32_t buf[SYSCALL_CTL_MAXNAME + 2];
    Uintptr n = (Uintptr)SYSCALL_CTL_MAXNAME * sizeof buf[0];
    burrow__SyscallCString h;
    uint8_t *bytes = burrow__syscall_cstring(&h, name, err);
    if (bytes == NULL)
        return 0;
    int32_t magic[2] = {0, 3};
    Error e = burrow__syscall_sysctl((Slice){magic, 2, 2, TYPE_INT32}, (uint8_t *)buf,
                                     &n, bytes, (Uintptr)name.len);
    burrow__syscall_cstring_free(&h);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    Int count = (Int)(n / sizeof buf[0]);
    memcpy(mib, buf, (size_t)count * sizeof buf[0]);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return count;
}

Str syscall_sysctl(Alloc *a, Str name, Error *err) {
    Str nil = {NULL, 0};
    int32_t mib[SYSCALL_CTL_MAXNAME + 2];
    Error e = BURROW_NO_ERROR;
    Int m = bsd_nametomib(name, mib, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    Slice ms = {mib, m, m, TYPE_INT32};
    Uintptr n = 0;
    e = burrow__syscall_sysctl(ms, NULL, &n, NULL, 0);
    if (BURROW_FAILED(e) || n == 0) {
        BURROW_OUT(err, e);
        return nil;
    }
    Uintptr size = n;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
    if (buf == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    e = burrow__syscall_sysctl(ms, buf, &n, NULL, 0);
    if (BURROW_FAILED(e)) {
        mem_free(a, buf, (size_t)size, 1);
        BURROW_OUT(err, e);
        return nil;
    }
    /* The string comes with its NUL. */
    if (n > 0 && buf[n - 1] == 0)
        n--;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str_from_bytes(buf, (Int)n);
}

uint32_t syscall_sysctl_uint32(Str name, Error *err) {
    int32_t mib[SYSCALL_CTL_MAXNAME + 2];
    Error e = BURROW_NO_ERROR;
    Int m = bsd_nametomib(name, mib, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    uint32_t v = 0;
    Uintptr n = sizeof v;
    e = burrow__syscall_sysctl((Slice){mib, m, m, TYPE_INT32}, (uint8_t *)&v, &n, NULL,
                               0);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    if (n != sizeof v) {
        BURROW_OUT(err, bsd_errno(SYSCALL_EIO));
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return v;
}

/* ------------------------------------------------------------- the rest */

/* Go forgets the RLIMIT_NOFILE it raised at start when this changes it.
 * burrow does not raise it, so there is nothing to forget. */
Error syscall_setrlimit(Int resource, SyscallRlimit *rlim) {
    return burrow__syscall_setrlimit(resource, rlim);
}

Error syscall_fcntl_flock(Uintptr fd, Int cmd, SyscallFlock_t *lk) {
    Error err = BURROW_NO_ERROR;
    burrow__syscall_fcntl_ptr((Int)fd, cmd, lk, &err);
    return err;
}

Int syscall_sendfile(Int outfd, Int infd, int64_t *offset, Int count, Error *err) {
#if defined(BURROW_OS_FREEBSD)
    uint64_t written = 0;
    SyscallErrno e = 0;
    if (sizeof(Uintptr) == 4) {
        /* The 64-bit offset goes in two words. */
        syscall_syscall9(SYSCALL_SYS_SENDFILE, (Uintptr)infd, (Uintptr)outfd,
                         (Uintptr)*offset, (Uintptr)((uint64_t)*offset >> 32),
                         (Uintptr)count, 0, (Uintptr)&written, 0, 0, NULL, &e);
    } else {
        syscall_syscall9(SYSCALL_SYS_SENDFILE, (Uintptr)infd, (Uintptr)outfd,
                         (Uintptr)*offset, (Uintptr)count, 0, (Uintptr)&written, 0, 0,
                         0, NULL, &e);
    }
#else
    /* macOS gives back how much it sent through the length. */
    uint64_t written = (uint64_t)count;
    SyscallErrno e = 0;
    const uintptr_t args[] = {(uintptr_t)infd,
                              (uintptr_t)outfd,
                              (uintptr_t)*offset,
                              (uintptr_t)&written,
                              0,
                              0};
    bsd_libc(BSD_LIBC_SENDFILE, "sendfile", args, 6, BURROW__SYSCALL_FAIL_INT, &e);
#endif
    BURROW_OUT(err, bsd_errno(e));
    return (Int)written;
}

Int syscall_getfsstat(Slice buf, Int flags, Error *err) {
    Uintptr p = 0;
    Uintptr size = 0;
    if (buf.len > 0) {
        p = (Uintptr)buf.p;
        size = (Uintptr)sizeof(SyscallStatfs_t) * (Uintptr)buf.len;
    }
    SyscallErrno e = 0;
#if defined(BURROW_OS_FREEBSD)
    Uintptr r =
        syscall_syscall(SYSCALL_SYS_GETFSSTAT, p, size, (Uintptr)flags, NULL, &e);
#else
    const uintptr_t args[] = {p, size, (uintptr_t)flags};
    Uintptr r = bsd_libc(BSD_LIBC_GETFSSTAT, "getfsstat", args, 3,
                         BURROW__SYSCALL_FAIL_INT, &e);
#endif
    BURROW_OUT(err, bsd_errno(e));
    return (Int)r;
}

#if defined(BURROW_OS_FREEBSD)
Error syscall_stat(Str path, SyscallStat_t *st) {
    return syscall_fstatat(BSD_AT_FDCWD, path, st, 0);
}

Error syscall_lstat(Str path, SyscallStat_t *st) {
    return syscall_fstatat(BSD_AT_FDCWD, path, st, BSD_AT_SYMLINK_NOFOLLOW);
}

Error syscall_mknod(Str path, uint32_t mode, uint64_t dev) {
    return burrow__syscall_mknodat(BSD_AT_FDCWD, path, mode, dev);
}
#else
Error syscall_ptrace_attach(Int pid) {
    return burrow__syscall_ptrace(SYSCALL_PT_ATTACH, pid, 0, 0);
}

Error syscall_ptrace_detach(Int pid) {
    return burrow__syscall_ptrace(SYSCALL_PT_DETACH, pid, 0, 0);
}
#endif

#endif
