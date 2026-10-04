/* What every Unix shares in syscall: Read, Write and the rest that wrap a
 * generated call, the string slices exec takes, CloseOnExec and SetNonblock,
 * and Mmap with the table of mappings Munmap checks against.
 *
 * Derived from Go's src/syscall/syscall_unix.go, exec_unix.go,
 * syscall_linux.go and the syscall_linux_ files for 386, arm and s390x.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/syscall.h"

#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include "internal.h"

#include <string.h>

Int syscall_stdin = 0;
Int syscall_stdout = 1;
Int syscall_stderr = 2;

/* --------------------------------------------------------------- reads */

Int syscall_read(Int fd, Slice p, Error *err) {
    return burrow__syscall_read(fd, p, err);
}

Int syscall_write(Int fd, Slice p, Error *err) {
    return burrow__syscall_write(fd, p, err);
}

Int syscall_pread(Int fd, Slice p, int64_t offset, Error *err) {
    return burrow__syscall_pread(fd, p, offset, err);
}

Int syscall_pwrite(Int fd, Slice p, int64_t offset, Error *err) {
    return burrow__syscall_pwrite(fd, p, offset, err);
}

/* ---------------------------------------------------- strings for exec */

/* How many bytes the strings in ss take with a NUL after each, or -1 if one
 * has a NUL in it already. */
static Int unix_strings_size(Slice ss) {
    const Str *s = (const Str *)ss.p;
    Int n = 0;
    for (Int i = 0; i < ss.len; i++) {
        if (s[i].len > 0 && memchr(s[i].p, 0, (size_t)s[i].len) != NULL)
            return -1;
        n += s[i].len + 1;
    }
    return n;
}

/* The pointers and the bytes they point at in one allocation, the bytes after
 * the pointers, so that syscall_slice_ptr_free can give it all back. */
static Slice unix_slice_ptr(Alloc *a, Slice ss, Int n) {
    size_t head = (size_t)(ss.len + 1) * sizeof(uint8_t *);
    uint8_t **bb = (uint8_t **)mem_alloc(a, head + (size_t)n, _Alignof(uint8_t *));
    if (bb == NULL)
        return slice_nil(TYPE_UNSAFE_POINTER);
    const Str *s = (const Str *)ss.p;
    uint8_t *b = (uint8_t *)bb + head;
    for (Int i = 0; i < ss.len; i++) {
        bb[i] = b;
        if (s[i].len > 0)
            memcpy(b, s[i].p, (size_t)s[i].len);
        b += s[i].len + 1;
    }
    bb[ss.len] = NULL;
    return (Slice){bb, ss.len + 1, ss.len + 1, TYPE_UNSAFE_POINTER};
}

Slice syscall_slice_ptr_from_strings(Alloc *a, Slice ss, Error *err) {
    Int n = unix_strings_size(ss);
    if (n < 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return slice_nil(TYPE_UNSAFE_POINTER);
    }
    Slice bb = unix_slice_ptr(a, ss, n);
    BURROW_OUT(err, bb.p == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR);
    return bb;
}

Slice syscall_string_slice_ptr(Alloc *a, Slice ss) {
    Int n = unix_strings_size(ss);
    if (n < 0)
        panic_str(BURROW_S("syscall: string with NUL passed to StringByteSlice"));
    return unix_slice_ptr(a, ss, n);
}

void syscall_slice_ptr_free(Alloc *a, Slice bb) {
    if (bb.p == NULL)
        return;
    uint8_t **p = (uint8_t **)bb.p;
    size_t n = 0;
    for (Int i = 0; i + 1 < bb.len; i++)
        n += strlen((const char *)p[i]) + 1;
    mem_free(a, p, (size_t)bb.len * sizeof(uint8_t *) + n, _Alignof(uint8_t *));
}

/* ---------------------------------------------------------- descriptors */

void syscall_close_on_exec(Int fd) {
    (void)burrow__syscall_fcntl(fd, SYSCALL_F_SETFD, SYSCALL_FD_CLOEXEC, NULL);
}

Error syscall_set_nonblock(Int fd, bool nonblocking) {
    Error err = BURROW_NO_ERROR;
    Int flag = burrow__syscall_fcntl(fd, SYSCALL_F_GETFL, 0, &err);
    if (BURROW_FAILED(err))
        return err;
    if (((flag & SYSCALL_O_NONBLOCK) != 0) == nonblocking)
        return BURROW_NO_ERROR;
    if (nonblocking)
        flag |= SYSCALL_O_NONBLOCK;
    else
        flag &= ~(Int)SYSCALL_O_NONBLOCK;
    (void)burrow__syscall_fcntl(fd, SYSCALL_F_SETFL, flag, &err);
    return err;
}

/* --------------------------------------------------------- struct lengths */

/* Sets f to v, whatever width the system gives f. */
#define UNIX_SET(f, v)                                                                 \
    ((f) = _Generic((f),                                                               \
         int32_t: (int32_t)(v),                                                        \
         int64_t: (int64_t)(v),                                                        \
         uint32_t: (uint32_t)(v),                                                      \
         uint64_t: (uint64_t)(v)))

void syscall_iovec_set_len(SyscallIovec *iov, Int length) {
    UNIX_SET(iov->len, length);
}

void syscall_msghdr_set_controllen(SyscallMsghdr *msghdr, Int length) {
    UNIX_SET(msghdr->controllen, length);
}

void syscall_cmsghdr_set_len(SyscallCmsghdr *cmsg, Int length) {
    UNIX_SET(cmsg->len, length);
}

/* ------------------------------------------------------------------ mmap */

/* Go's mmap for the system: mmap2, which counts in pages, on 32-bit x86 and
 * Arm, and the old call that takes its arguments in memory on s390x. */
static Uintptr unix_mmap(Uintptr length, Int prot, Int flags, Int fd, int64_t offset,
                         Error *err) {
#if defined(BURROW_OS_LINUX) && (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM))
    Uintptr page = (Uintptr)(offset / 4096);
    if (offset != (int64_t)page * 4096) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return 0;
    }
    return burrow__syscall_mmap2(0, length, prot, flags, fd, page, err);
#elif defined(BURROW_OS_LINUX) && defined(BURROW_ARCH_S390X)
    Uintptr args[6] = {0,           length,         (Uintptr)prot, (Uintptr)flags,
                       (Uintptr)fd, (Uintptr)offset};
    SyscallErrno e = 0;
    Uintptr r = syscall_syscall(SYSCALL_SYS_MMAP, (Uintptr)args, 0, 0, NULL, &e);
    BURROW_OUT(err, e != 0 ? burrow__syscall_errno_err(e) : BURROW_NO_ERROR);
    return r;
#else
    return burrow__syscall_mmap(0, length, prot, flags, fd, offset, err);
#endif
}

/* Go's mapper: each mapping Mmap made and Munmap has not undone, so Munmap can
 * refuse a slice it did not hand out. Go keys its map by the last byte, and so
 * does this, since a slice of a mapping that starts at its first byte but is
 * shorter does not end there. */
typedef struct UnixMapping {
    uint8_t *base;
    Int len;
} UnixMapping;

static SyncMutex unix_mmap_mu;
static UnixMapping *unix_mmap_active;
static Int unix_mmap_len;
static Int unix_mmap_cap;

Slice syscall_mmap(Int fd, int64_t offset, Int length, Int prot, Int flags,
                   Error *err) {
    Slice nil = slice_nil(TYPE_BYTE);
    if (length <= 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return nil;
    }
    Error e = BURROW_NO_ERROR;
    Uintptr addr = unix_mmap((Uintptr)length, prot, flags, fd, offset, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return nil;
    }
    sync_mutex_lock(&unix_mmap_mu);
    if (unix_mmap_len == unix_mmap_cap) {
        Int cap = unix_mmap_cap == 0 ? 8 : unix_mmap_cap * 2;
        UnixMapping *m = (UnixMapping *)mem_realloc(
            heap_allocator(), unix_mmap_active, (size_t)unix_mmap_cap * sizeof *m,
            (size_t)cap * sizeof *m, _Alignof(UnixMapping));
        if (m == NULL) {
            sync_mutex_unlock(&unix_mmap_mu);
            (void)burrow__syscall_munmap(addr, (Uintptr)length);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return nil;
        }
        unix_mmap_active = m;
        unix_mmap_cap = cap;
    }
    unix_mmap_active[unix_mmap_len++] = (UnixMapping){(uint8_t *)addr, length};
    sync_mutex_unlock(&unix_mmap_mu);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){(void *)addr, length, length, TYPE_BYTE};
}

Error syscall_munmap(Slice b) {
    if (b.len == 0 || b.len != b.cap)
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    const uint8_t *last = (const uint8_t *)b.p + b.cap - 1;
    sync_mutex_lock(&unix_mmap_mu);
    Int i = 0;
    while (i < unix_mmap_len &&
           unix_mmap_active[i].base + unix_mmap_active[i].len - 1 != last)
        i++;
    if (i == unix_mmap_len || unix_mmap_active[i].base != (uint8_t *)b.p) {
        sync_mutex_unlock(&unix_mmap_mu);
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    }
    UnixMapping m = unix_mmap_active[i];
    Error err = burrow__syscall_munmap((Uintptr)m.base, (Uintptr)m.len);
    if (!BURROW_FAILED(err))
        unix_mmap_active[i] = unix_mmap_active[--unix_mmap_len];
    sync_mutex_unlock(&unix_mmap_mu);
    return err;
}

#endif
