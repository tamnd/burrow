/* What every Unix shares in syscall: Read, Write and the rest that wrap a
 * generated call, the string slices exec takes, CloseOnExec and SetNonblock,
 * Mmap with the table of mappings Munmap checks against, and ParseDirent.
 *
 * Derived from Go's src/syscall/syscall_unix.go, exec_unix.go,
 * syscall_linux.go, the syscall_linux_ files for 386, arm and s390x, and
 * dirent.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/mem/heap.h"
#include "burrow/slice.h"

#include "internal.h"

#include <stddef.h>
#include <string.h>

#if BURROW_MSAN
#include <sanitizer/msan_interface.h>
#endif

Int syscall_stdin = 0;
Int syscall_stdout = 1;
Int syscall_stderr = 2;

/* --------------------------------------------------------------- reads */

/* The calls go to the kernel without the C library, so the memory sanitizer
 * never sees the kernel fill the buffer of a read or look at the one of a
 * write. Go tells it, in these four and nowhere else, and so does this. */
static void unix_msan_write(Slice p, Int n) {
#if BURROW_MSAN
    if (n > 0)
        __msan_unpoison(p.p, (size_t)n);
#else
    (void)p;
    (void)n;
#endif
}

static void unix_msan_read(Slice p, Int n) {
#if BURROW_MSAN
    if (n > 0)
        __msan_check_mem_is_initialized(p.p, (size_t)n);
#else
    (void)p;
    (void)n;
#endif
}

Int syscall_read(Int fd, Slice p, Error *err) {
    Int n = burrow__syscall_read(fd, p, err);
    unix_msan_write(p, n);
    return n;
}

Int syscall_write(Int fd, Slice p, Error *err) {
    Int n = burrow__syscall_write(fd, p, err);
    unix_msan_read(p, n);
    return n;
}

Int syscall_pread(Int fd, Slice p, int64_t offset, Error *err) {
    Int n = burrow__syscall_pread(fd, p, offset, err);
    unix_msan_write(p, n);
    return n;
}

Int syscall_pwrite(Int fd, Slice p, int64_t offset, Error *err) {
    Int n = burrow__syscall_pwrite(fd, p, offset, err);
    unix_msan_read(p, n);
    return n;
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

/* --------------------------------------------------------------- dirents */

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI) || \
    defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)

#if defined(BURROW_OS_FREEBSD)
#define UNIX_DIRENT_INO fileno
#else
#define UNIX_DIRENT_INO ino
#endif

/* Go's readInt: the size bytes at off in b, in the machine's order, and
 * false if b is too short. */
static bool unix_read_int(Slice b, size_t off, size_t size, uint64_t *u) {
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
        if (!unix_read_int(buf, offsetof(SyscallDirent, reclen),
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
        if (!unix_read_int(rec, offsetof(SyscallDirent, UNIX_DIRENT_INO),
                           sizeof(((SyscallDirent *)0)->UNIX_DIRENT_INO), &ino))
            break;
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
        /* Linux keeps entries whose inode is 0, where the BSDs skip them. The
         * name runs to the end of the record. */
        (void)ino;
        if (reclen < namoff)
            break;
        uint64_t namlen = reclen - namoff;
#else
        /* An inode of 0 is a file no longer in the directory. */
        if (ino == 0)
            continue;
        uint64_t namlen = 0;
        if (!unix_read_int(rec, offsetof(SyscallDirent, namlen),
                           sizeof(((SyscallDirent *)0)->namlen), &namlen))
            break;
#endif
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

#undef UNIX_DIRENT_INO
#endif

#endif
