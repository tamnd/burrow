/* The os functions that take a path rather than a File.
 *
 * Derived from Go's src/os/file.go.
 * Go source: go1.27.1.
 *
 * The rest is file_posix.go, file_unix.go, file_windows.go and path.go, from
 * the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/heap.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

bool os_is_path_separator(uint8_t c) {
#if defined(BURROW_OS_WINDOWS)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static Error os_path_error(Str op, Str name, Error e) {
    return fs_path_error_new(error_allocator(), op, name, e);
}

static Error os_link_error(Str op, Str oldname, Str newname, Error e) {
    return os_link_error_new(error_allocator(), op, oldname, newname, e);
}

/* The same mode as file.c's, for mkdir. */
static uint32_t os_path_syscall_mode(OsFileMode m) {
    uint32_t o = m & FS_MODE_PERM;
    if (m & FS_MODE_SETUID)
        o |= PAL_S_ISUID;
    if (m & FS_MODE_SETGID)
        o |= PAL_S_ISGID;
    if (m & FS_MODE_STICKY)
        o |= PAL_S_ISVTX;
    return o;
}

Error os_mkdir(Str name, OsFileMode perm) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("mkdir"), name, e);
    PalErrno pe = PAL_OK;
    if (!pal_mkdir(c.p, os_path_syscall_mode(perm), &pe)) {
        e = burrow__os_errno(pe);
        burrow__os_cpath_free(&c);
        return os_path_error(OS_LIT("mkdir"), name, e);
    }
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_LINUX)
    /* mkdir on the BSDs drops the sticky bit, so it goes on afterwards, and
     * the directory goes again if that fails. */
    if (perm & FS_MODE_STICKY) {
        PalStat st;
        if (!pal_stat(c.p, &st, &pe))
            e = os_path_error(OS_LIT("stat"), name, burrow__os_errno(pe));
        else if (!pal_chmod(c.p, (st.mode & 07777) | PAL_S_ISVTX, &pe))
            e = os_path_error(OS_LIT("chmod"), name, burrow__os_errno(pe));
        if (BURROW_FAILED(e))
            pal_rmdir(c.p, NULL);
    }
#endif
    burrow__os_cpath_free(&c);
    return e;
}

/* os.Remove: unlink, and rmdir when that fails. Which error to report is a
 * guess: ENOTDIR from rmdir means the name was not a directory, so unlink's
 * error is the one that says why. */
Error os_remove(Str name) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("remove"), name, e);
    PalErrno pe = PAL_OK;
    if (pal_unlink(c.p, &pe)) {
        burrow__os_cpath_free(&c);
        return BURROW_NO_ERROR;
    }
    e = burrow__os_errno(pe);
    PalErrno pe1 = PAL_OK;
    if (pal_rmdir(c.p, &pe1)) {
        burrow__os_cpath_free(&c);
        return BURROW_NO_ERROR;
    }
    if (pe1 != PAL_ENOTDIR)
        e = burrow__os_errno(pe1);
    burrow__os_cpath_free(&c);
    return os_path_error(OS_LIT("remove"), name, e);
}

Error os_rename(Str oldpath, Str newpath) {
    Error e = BURROW_NO_ERROR;
    OsCPath co, cn;
    if (!burrow__os_cpath(&co, oldpath, &e))
        return os_link_error(OS_LIT("rename"), oldpath, newpath, e);
    if (!burrow__os_cpath(&cn, newpath, &e)) {
        burrow__os_cpath_free(&co);
        return os_link_error(OS_LIT("rename"), oldpath, newpath, e);
    }
    PalErrno pe = PAL_OK;
#if !defined(BURROW_OS_WINDOWS)
    /* rename(2) would replace an empty directory with a directory, which Go
     * does not allow. A case only rename on a filesystem that ignores case
     * finds the same file under both names, and that one is fine. */
    PalStat nst, ost;
    if (pal_lstat(cn.p, &nst, &pe) && (nst.mode & PAL_S_IFMT) == PAL_S_IFDIR) {
        if (!pal_lstat(co.p, &ost, &pe))
            e = burrow__os_errno(pe);
        else if (str_eq(oldpath, newpath) || nst.dev != ost.dev || nst.ino != ost.ino)
            e = burrow__os_errno_value(SYSCALL_EEXIST);
    }
#endif
    if (BURROW_OK(e) && !pal_rename(co.p, cn.p, &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&co);
    burrow__os_cpath_free(&cn);
    if (BURROW_FAILED(e))
        return os_link_error(OS_LIT("rename"), oldpath, newpath, e);
    return BURROW_NO_ERROR;
}

static Error os_two_paths(Str op, Str oldname, Str newname,
                          bool (*call)(const char *, const char *, PalErrno *)) {
    Error e = BURROW_NO_ERROR;
    OsCPath co, cn;
    if (!burrow__os_cpath(&co, oldname, &e))
        return os_link_error(op, oldname, newname, e);
    if (!burrow__os_cpath(&cn, newname, &e)) {
        burrow__os_cpath_free(&co);
        return os_link_error(op, oldname, newname, e);
    }
    PalErrno pe = PAL_OK;
    if (!call(co.p, cn.p, &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&co);
    burrow__os_cpath_free(&cn);
    if (BURROW_FAILED(e))
        return os_link_error(op, oldname, newname, e);
    return BURROW_NO_ERROR;
}

Error os_link(Str oldname, Str newname) {
    return os_two_paths(OS_LIT("link"), oldname, newname, pal_link);
}

Error os_symlink(Str oldname, Str newname) {
    return os_two_paths(OS_LIT("symlink"), oldname, newname, pal_symlink);
}

Str os_readlink(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str none = {NULL, 0};
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e)) {
        BURROW_OUT(err, os_path_error(OS_LIT("readlink"), name, e));
        return none;
    }
    for (Int len = 128;; len *= 2) {
        Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)len, 1);
        if (b == NULL) {
            burrow__os_cpath_free(&c);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        PalErrno pe = PAL_OK;
        int64_t n = pal_readlink(c.p, (char *)b, len, &pe);
        if (n < 0 && pe == PAL_ERANGE) {
            /* Go grows the buffer when the target fills it. The PAL says
             * ERANGE instead of filling it, which comes to the same. */
            mem_free(a, b, (size_t)len, 1);
            continue;
        }
        if (n < 0) {
            e = burrow__os_errno(pe);
            mem_free(a, b, (size_t)len, 1);
            burrow__os_cpath_free(&c);
            BURROW_OUT(err, os_path_error(OS_LIT("readlink"), name, e));
            return none;
        }
        if (n < len) {
            burrow__os_cpath_free(&c);
            Byte *s = (Byte *)mem_realloc(a, b, (size_t)len, (size_t)n, 1);
            if (s == NULL)
                s = b;
            return str_from_bytes(s, (Int)n);
        }
        mem_free(a, b, (size_t)len, 1);
    }
}

Error os_chmod(Str name, OsFileMode mode) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("chmod"), name, e);
    PalErrno pe = PAL_OK;
    if (!pal_chmod(c.p, os_path_syscall_mode(mode), &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e))
        return os_path_error(OS_LIT("chmod"), name, e);
    return BURROW_NO_ERROR;
}

Error os_chown(Str name, Int uid, Int gid) {
#if defined(BURROW_OS_WINDOWS)
    (void)uid;
    (void)gid;
    return os_path_error(OS_LIT("chown"), name,
                         burrow__os_errno_value(SYSCALL_EWINDOWS));
#else
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("chown"), name, e);
    PalErrno pe = PAL_OK;
    if (!pal_chown(c.p, (int64_t)uid, (int64_t)gid, &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e))
        return os_path_error(OS_LIT("chown"), name, e);
    return BURROW_NO_ERROR;
#endif
}

/* A zero Time leaves that time as it is. */
Error os_chtimes(Str name, Time atime, Time mtime) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("chtimes"), name, e);
    int64_t at = time_is_zero(atime) ? PAL_UTIME_OMIT : time_unix_nano(atime);
    int64_t mt = time_is_zero(mtime) ? PAL_UTIME_OMIT : time_unix_nano(mtime);
    PalErrno pe = PAL_OK;
    if (!pal_utimes(c.p, at, mt, &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e))
        return os_path_error(OS_LIT("chtimes"), name, e);
    return BURROW_NO_ERROR;
}

Error os_truncate(Str name, int64_t size) {
#if defined(BURROW_OS_WINDOWS)
    /* Go opens the file and truncates that, so the errors are Open's and
     * File.Truncate's. */
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open_file(heap_allocator(), name, OS_O_WRONLY, 0666, &e);
    if (f == NULL)
        return e;
    e = os_file_truncate(f, size);
    Error ce = os_file_close(f);
    os_file_free(f);
    if (BURROW_OK(e))
        e = ce;
    return e;
#else
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e))
        return os_path_error(OS_LIT("truncate"), name, e);
    PalErrno pe = PAL_OK;
    if (!pal_truncate(c.p, size, &pe))
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e))
        return os_path_error(OS_LIT("truncate"), name, e);
    return BURROW_NO_ERROR;
#endif
}

/* ---------------------------------------------------------- whole files */

/* The smallest buffer os_read_file reads into. A Linux file under /proc says
 * its size is 0, so this is also how much room a read gets there. */
#define OS_MIN_READ_BUFFER 512

Slice os_read_file(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice data = {NULL, 0, 0, TYPE_BYTE};
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(heap_allocator(), name, &e);
    if (f == NULL) {
        BURROW_OUT(err, e);
        return data;
    }

    /* statOrZero, without allocating a FileInfo. Nothing else has f. */
    PalStat st;
    int64_t stat_size = 0;
    if (pal_fstat(f->fd, &st, NULL))
        stat_size = st.size;
    bool zero_size = stat_size == 0;
    Int size = 0;
    if (stat_size > 0 && stat_size < BURROW_INT_MAX)
        size = (Int)stat_size;
    size++; /* one byte for the read that finds the end */
    if (size < OS_MIN_READ_BUFFER)
        size = OS_MIN_READ_BUFFER;

    data.p = mem_alloc_nozero(a, (size_t)size, 1);
    if (data.p == NULL) {
        os_file_close(f);
        os_file_free(f);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return data;
    }
    data.cap = size;
    for (;;) {
        Slice room = {(Byte *)data.p + data.len, 0, data.cap - data.len, TYPE_BYTE};
        room.len = room.cap;
        Int n = os_file_read(f, room, &e);
        data.len += n;
        if (BURROW_FAILED(e)) {
            if (e.vt == io_eof.vt && e.data == io_eof.data)
                e = BURROW_NO_ERROR;
            break;
        }
        if (data.len >= data.cap ||
            (zero_size && data.cap - data.len < OS_MIN_READ_BUFFER)) {
            Int ncap = data.cap * 2;
            void *p = mem_realloc(a, data.p, (size_t)data.cap, (size_t)ncap, 1);
            if (p == NULL) {
                e = burrow_err_out_of_memory;
                break;
            }
            data.p = p;
            data.cap = ncap;
        }
    }
    os_file_close(f);
    os_file_free(f);
    BURROW_OUT(err, e);
    return data;
}

Error os_write_file(Str name, Slice data, OsFileMode perm) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open_file(heap_allocator(), name,
                             OS_O_WRONLY | OS_O_CREATE | OS_O_TRUNC, perm, &e);
    if (f == NULL)
        return e;
    os_file_write(f, data, &e);
    Error ce = os_file_close(f);
    os_file_free(f);
    if (BURROW_FAILED(ce) && BURROW_OK(e))
        e = ce;
    return e;
}

Str burrow__os_cat3(Alloc *a, Str x, Str y, Str z) {
    Int n = x.len + y.len + z.len;
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    Byte *p = b;
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    p += x.len;
    if (y.len > 0)
        memcpy(p, y.p, (size_t)y.len);
    p += y.len;
    if (z.len > 0)
        memcpy(p, z.p, (size_t)z.len);
    b[n] = 0;
    return str_from_bytes(b, n);
}
