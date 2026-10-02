/* os.File. See include/burrow/os.h.
 *
 * Derived from Go's src/os/file.go.
 * Go source: go1.27.1.
 *
 * The descriptor handling is internal/poll's fd_unix.go and fd_windows.go, and
 * Open and Close are file_unix.go, file_windows.go and file_posix.go, all from
 * the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/declare.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/sema.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

_Static_assert(OS_O_RDONLY == PAL_O_RDONLY, "OS_O_RDONLY is the PAL's");
_Static_assert(OS_O_WRONLY == PAL_O_WRONLY, "OS_O_WRONLY is the PAL's");
_Static_assert(OS_O_RDWR == PAL_O_RDWR, "OS_O_RDWR is the PAL's");
_Static_assert(OS_O_APPEND == PAL_O_APPEND, "OS_O_APPEND is the PAL's");
_Static_assert(OS_O_CREATE == PAL_O_CREATE, "OS_O_CREATE is the PAL's");
_Static_assert(OS_O_EXCL == PAL_O_EXCL, "OS_O_EXCL is the PAL's");
_Static_assert(OS_O_SYNC == PAL_O_SYNC, "OS_O_SYNC is the PAL's");
_Static_assert(OS_O_TRUNC == PAL_O_TRUNC, "OS_O_TRUNC is the PAL's");

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

/* poll.ErrFileClosing. os turns it into os_err_closed everywhere but the
 * deadline setters, which hand it back as it is, as Go's do. */
BURROW_SENTINEL_ERROR(burrow__os_err_file_closing, "use of closed file");

/* The two methods io_copy asks a File for, in name order. */
#define OS_FILE_METHODS(M, T)                                                          \
    M(T, ReadFrom, os_file_read_from, IO_SIG_READ_FROM)                                \
    M(T, WriteTo, os_file_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(OsFile, OS_FILE_METHODS);

static const Type os_file_desc = {
    {(const Byte *)"File", 4},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsFile),
    (uint16_t)_Alignof(OsFile),
    0,
    (uint16_t)(sizeof burrow__methods_OsFile / sizeof burrow__methods_OsFile[0]),
    NULL,
    burrow__methods_OsFile,
    NULL,
    NULL,
    0,
    0x6f73666cU, /* "osfl" */
    NULL,
};

/* Go's fileWithoutReadFrom and fileWithoutWriteTo: a File with neither
 * method, for the generic copies to go through without coming back. */
static const Type os_file_plain_desc = {
    {(const Byte *)"fileWithoutReadFrom", 19},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsFile),
    (uint16_t)_Alignof(OsFile),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736670U, /* "osfp" */
    NULL,
};

const Type *const TYPE_OS_FILE = &os_file_desc;

/* ------------------------------------------------------------------ paths */

bool burrow__os_cpath(OsCPath *c, Str s, Error *err) {
    if (s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL) {
        BURROW_OUT(err, burrow__os_errno_value(SYSCALL_EINVAL));
        c->p = NULL;
        return false;
    }
    if (s.len < OS_CPATH_SMALL) {
        c->p = c->small;
        c->cap = 0;
    } else {
        c->p = (char *)mem_alloc_nozero(heap_allocator(), (size_t)s.len + 1, 1);
        if (c->p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return false;
        }
        c->cap = s.len + 1;
    }
    if (s.len > 0)
        memcpy(c->p, s.p, (size_t)s.len);
    c->p[s.len] = 0;
    return true;
}

void burrow__os_cpath_free(OsCPath *c) {
    if (c->p != NULL && c->p != c->small)
        mem_free(heap_allocator(), c->p, (size_t)c->cap, 1);
    c->p = NULL;
}

/* ------------------------------------------------------------- the locks */

/* poll.FD.destroy: closes the descriptor once the last user has gone, and
 * lets Close stop waiting. */
static Error os_destroy(OsFile *f) {
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_close(f->fd, &pe))
        e = burrow__os_errno(pe);
    f->fd = PAL_INVALID_HANDLE;
    burrow__os_dirinfo_free(f);
    burrow__sema_release(&f->csema, false);
    return e;
}

bool burrow__os_incref(OsFile *f) {
    return burrow__fdmu_incref(&f->mu);
}

Error burrow__os_decref(OsFile *f) {
    if (burrow__fdmu_decref(&f->mu))
        return os_destroy(f);
    return BURROW_NO_ERROR;
}

#define os_incref burrow__os_incref
#define os_decref burrow__os_decref

static bool os_lock(OsFile *f, bool read) {
    return burrow__fdmu_rwlock(&f->mu, read, true);
}

static void os_unlock(OsFile *f, bool read) {
    if (burrow__fdmu_rwunlock(&f->mu, read))
        os_destroy(f);
}

/* Go takes both locks for a read or a write on Windows, because a disk file
 * there has one offset that a read and a write would both move. */
#if defined(BURROW_OS_WINDOWS)
static bool os_read_lock(OsFile *f) {
    if (!os_lock(f, true))
        return false;
    if (!os_lock(f, false)) {
        os_unlock(f, true);
        return false;
    }
    return true;
}

static void os_read_unlock(OsFile *f) {
    os_unlock(f, true);
    os_unlock(f, false);
}

#define os_write_lock os_read_lock
#define os_write_unlock os_read_unlock
#else
static bool os_read_lock(OsFile *f) {
    return os_lock(f, true);
}

static void os_read_unlock(OsFile *f) {
    os_unlock(f, true);
}

static bool os_write_lock(OsFile *f) {
    return os_lock(f, false);
}

static void os_write_unlock(OsFile *f) {
    os_unlock(f, false);
}
#endif

/* File.wrapErr: no error and io_eof pass through, the closing error becomes
 * os_err_closed, and everything else goes in an OsPathError with f's name. */
static Error os_wrap(const OsFile *f, Str op, Error e) {
    if (BURROW_OK(e) || (e.vt == io_eof.vt && e.data == io_eof.data))
        return e;
    if (e.vt == burrow__os_err_file_closing.vt &&
        e.data == burrow__os_err_file_closing.data)
        e = fs_err_closed;
    return fs_path_error_new(error_allocator(), op, f->name, e);
}

/* ---------------------------------------------------------------- opening */

static OsFile *os_new(Alloc *a, int64_t fd, Str name) {
    OsFile *f =
        (OsFile *)mem_alloc(a, sizeof(OsFile) + (size_t)name.len, _Alignof(OsFile));
    if (f == NULL)
        return NULL;
    Byte *p = (Byte *)(f + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    f->name = str_from_bytes(p, name.len);
    f->fd = fd;
    f->a = a;
    return f;
}

OsFile *os_new_file(Alloc *a, Uintptr fd, Str name) {
    int64_t h = (int64_t)(intptr_t)fd;
#if defined(BURROW_OS_WINDOWS)
    if (h == PAL_INVALID_HANDLE)
        return NULL;
#else
    if (h < 0)
        return NULL;
#endif
    return os_new(a, h, name);
}

/* syscallMode: the permission bits and the three special ones, in the
 * system's numbering. */
static uint32_t os_syscall_mode(OsFileMode m) {
    uint32_t o = m & FS_MODE_PERM;
    if (m & FS_MODE_SETUID)
        o |= PAL_S_ISUID;
    if (m & FS_MODE_SETGID)
        o |= PAL_S_ISGID;
    if (m & FS_MODE_STICKY)
        o |= PAL_S_ISVTX;
    return o;
}

OsFile *os_open_file(Alloc *a, Str name, Int flag, OsFileMode perm, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    if (name.len == 0) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), OS_LIT("open"), name,
                                          burrow__os_errno_value(SYSCALL_ENOENT)));
        return NULL;
    }
#endif
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e)) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), OS_LIT("open"), name, e));
        return NULL;
    }

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_LINUX)
    /* The BSDs, macOS among them, drop the sticky bit when a file is created
     * with it, so Go sets it afterwards on a file this open created. */
    bool set_sticky = false;
    if ((flag & OS_O_CREATE) != 0 && (perm & FS_MODE_STICKY) != 0) {
        PalStat st;
        PalErrno se = PAL_OK;
        if (!pal_stat(c.p, &st, &se) && se == PAL_ENOENT)
            set_sticky = true;
    }
#endif

    PalErrno pe = PAL_OK;
    int64_t fd = pal_open(c.p, (uint32_t)flag, os_syscall_mode(perm), &pe);
    if (fd == PAL_INVALID_HANDLE) {
        e = burrow__os_errno(pe);
        burrow__os_cpath_free(&c);
        BURROW_OUT(err, fs_path_error_new(error_allocator(), OS_LIT("open"), name, e));
        return NULL;
    }

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_LINUX)
    if (set_sticky) {
        PalStat st;
        if (pal_stat(c.p, &st, &pe))
            pal_chmod(c.p, (st.mode & 07777) | PAL_S_ISVTX, &pe);
    }
#endif
    burrow__os_cpath_free(&c);

    OsFile *f = os_new(a, fd, name);
    if (f == NULL) {
        pal_close(fd, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    f->append_mode = (flag & OS_O_APPEND) != 0;
    return f;
}

OsFile *os_open(Alloc *a, Str name, Error *err) {
    return os_open_file(a, name, OS_O_RDONLY, 0, err);
}

OsFile *os_create(Alloc *a, Str name, Error *err) {
    return os_open_file(a, name, OS_O_RDWR | OS_O_CREATE | OS_O_TRUNC, 0666, err);
}

Str os_file_name(const OsFile *f) {
    return f->name;
}

Uintptr os_file_fd(OsFile *f) {
    if (f == NULL)
        return ~(Uintptr)0;
    return (Uintptr)(intptr_t)f->fd;
}

/* ---------------------------------------------------------- reading */

Int os_file_read(OsFile *f, Slice p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    if (!os_read_lock(f)) {
        BURROW_OUT(err, os_wrap(f, OS_LIT("read"), burrow__os_err_file_closing));
        return 0;
    }
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (p.len > 0) {
        PalErrno pe = PAL_OK;
        int64_t r = pal_read(f->fd, p.p, p.len, &pe);
        if (r < 0)
            e = burrow__os_errno(pe);
        else if (r == 0)
            e = io_eof;
        else
            n = (Int)r;
    }
    os_read_unlock(f);
    BURROW_OUT(err, os_wrap(f, OS_LIT("read"), e));
    return n;
}

/* poll.FD.Pread: one pread, with a reference rather than a lock on Unix, since
 * it does not move the offset. 0 bytes and no error is the end. */
static Int os_pread(OsFile *f, Byte *p, Int len, int64_t off, Error *e) {
#if defined(BURROW_OS_WINDOWS)
    if (!os_read_lock(f)) {
#else
    if (!os_incref(f)) {
#endif
        *e = burrow__os_err_file_closing;
        return 0;
    }
    PalErrno pe = PAL_OK;
    int64_t r = pal_pread(f->fd, p, len, off, &pe);
    Int n = 0;
    *e = BURROW_NO_ERROR;
    if (r < 0)
        *e = burrow__os_errno(pe);
    else if (r == 0 && len > 0)
        *e = io_eof;
    else
        n = (Int)r;
#if defined(BURROW_OS_WINDOWS)
    os_read_unlock(f);
#else
    os_decref(f);
#endif
    return n;
}

Int os_file_read_at(OsFile *f, Slice p, int64_t off, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    if (off < 0) {
        BURROW_OUT(err, fs_path_error_new(
                            error_allocator(), OS_LIT("readat"), f->name,
                            errors_new(error_allocator(), OS_LIT("negative offset"))));
        return 0;
    }
    Byte *b = (Byte *)p.p;
    Int left = p.len;
    Int n = 0;
    while (left > 0) {
        Error e = BURROW_NO_ERROR;
        Int m = os_pread(f, b, left, off, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, os_wrap(f, OS_LIT("read"), e));
            break;
        }
        n += m;
        b += m;
        left -= m;
        off += m;
    }
    return n;
}

/* ---------------------------------------------------------- writing */

/* poll.FD.Write: loops until all of p is written, since a short write with no
 * error just means try again. A write of nothing at all with no error would
 * loop for ever, so that is io_err_unexpected_eof. */
static Int os_write_all(OsFile *f, const Byte *p, Int len, Error *e) {
    Int nn = 0;
    *e = BURROW_NO_ERROR;
    for (;;) {
        PalErrno pe = PAL_OK;
        int64_t n = pal_write(f->fd, p + nn, len - nn, &pe);
        Error we = BURROW_NO_ERROR;
        if (n < 0)
            we = burrow__os_errno(pe);
        if (n > 0)
            nn += (Int)n;
        if (nn == len) {
            *e = we;
            return nn;
        }
        if (BURROW_FAILED(we)) {
            *e = we;
            return nn;
        }
        if (n == 0) {
            *e = io_err_unexpected_eof;
            return nn;
        }
    }
}

Int os_file_write(OsFile *f, Slice p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (!os_write_lock(f)) {
        e = burrow__os_err_file_closing;
    } else {
        n = os_write_all(f, (const Byte *)p.p, p.len, &e);
        os_write_unlock(f);
    }
    /* Go raises SIGPIPE here when stdout or stderr gets EPIPE. burrow leaves
     * SIGPIPE as the host program set it, so with the default the process
     * already died in the write. */
    Error out = BURROW_NO_ERROR;
    if (n != p.len)
        out = io_err_short_write;
    if (BURROW_FAILED(e))
        out = os_wrap(f, OS_LIT("write"), e);
    BURROW_OUT(err, out);
    return n;
}

Int os_file_write_string(OsFile *f, Str s, Error *err) {
    return os_file_write(f, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE),
                         err);
}

BURROW_SENTINEL_ERROR(burrow__os_err_write_at_in_append_mode,
                      "os: invalid use of WriteAt on file opened with O_APPEND");

/* poll.FD.Pwrite, the loop of os_write_all at an offset. */
static Int os_pwrite(OsFile *f, const Byte *p, Int len, int64_t off, Error *e) {
#if defined(BURROW_OS_WINDOWS)
    if (!os_write_lock(f)) {
#else
    if (!os_incref(f)) {
#endif
        *e = burrow__os_err_file_closing;
        return 0;
    }
    Int nn = 0;
    *e = BURROW_NO_ERROR;
    for (;;) {
        PalErrno pe = PAL_OK;
        int64_t n = pal_pwrite(f->fd, p + nn, len - nn, off + nn, &pe);
        Error we = BURROW_NO_ERROR;
        if (n < 0)
            we = burrow__os_errno(pe);
        if (n > 0)
            nn += (Int)n;
        if (nn == len) {
            *e = we;
            break;
        }
        if (BURROW_FAILED(we)) {
            *e = we;
            break;
        }
        if (n == 0) {
            *e = io_err_unexpected_eof;
            break;
        }
    }
#if defined(BURROW_OS_WINDOWS)
    os_write_unlock(f);
#else
    os_decref(f);
#endif
    return nn;
}

Int os_file_write_at(OsFile *f, Slice p, int64_t off, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    if (f->append_mode) {
        BURROW_OUT(err, burrow__os_err_write_at_in_append_mode);
        return 0;
    }
    if (off < 0) {
        BURROW_OUT(err, fs_path_error_new(
                            error_allocator(), OS_LIT("writeat"), f->name,
                            errors_new(error_allocator(), OS_LIT("negative offset"))));
        return 0;
    }
    const Byte *b = (const Byte *)p.p;
    Int left = p.len;
    Int n = 0;
    while (left > 0) {
        Error e = BURROW_NO_ERROR;
        Int m = os_pwrite(f, b, left, off, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, os_wrap(f, OS_LIT("write"), e));
            n += m;
            break;
        }
        n += m;
        b += m;
        left -= m;
        off += m;
    }
    return n;
}

/* ------------------------------------------------------- everything else */

int64_t os_file_seek(OsFile *f, int64_t offset, Int whence, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
#if defined(BURROW_OS_WINDOWS)
    bool held = os_read_lock(f);
#else
    bool held = os_incref(f);
#endif
    if (!held) {
        BURROW_OUT(err, os_wrap(f, OS_LIT("seek"), burrow__os_err_file_closing));
        return 0;
    }
    /* Reading the directory again after a seek starts from the top. */
    burrow__os_dirinfo_reset(f);
    PalErrno pe = PAL_OK;
    int64_t r = pal_seek(f->fd, offset, (int32_t)whence, &pe);
    Error e = BURROW_NO_ERROR;
    if (r < 0)
        e = burrow__os_errno(pe);
#if defined(BURROW_OS_WINDOWS)
    os_read_unlock(f);
#else
    os_decref(f);
#endif
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_wrap(f, OS_LIT("seek"), e));
        return 0;
    }
    return r;
}

OsFileInfo os_file_stat(OsFile *f, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsFileInfo none = {NULL, NULL};
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return none;
    }
    if (!os_incref(f)) {
        BURROW_OUT(err, os_wrap(f, OS_LIT("stat"), burrow__os_err_file_closing));
        return none;
    }
    PalStat st;
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_fstat(f->fd, &st, &pe))
        e = burrow__os_errno(pe);
    os_decref(f);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_wrap(f, OS_LIT("stat"), e));
        return none;
    }
    return burrow__os_file_info(a, f->name, &st, err);
}

Error os_file_sync(OsFile *f) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return os_wrap(f, OS_LIT("sync"), burrow__os_err_file_closing);
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_fsync(f->fd, &pe))
        e = burrow__os_errno(pe);
    os_decref(f);
    return os_wrap(f, OS_LIT("sync"), e);
}

Error os_file_truncate(OsFile *f, int64_t size) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return os_wrap(f, OS_LIT("truncate"), burrow__os_err_file_closing);
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_ftruncate(f->fd, size, &pe))
        e = burrow__os_errno(pe);
    os_decref(f);
    return os_wrap(f, OS_LIT("truncate"), e);
}

Error os_file_chmod(OsFile *f, OsFileMode mode) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return os_wrap(f, OS_LIT("chmod"), burrow__os_err_file_closing);
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_fchmod(f->fd, os_syscall_mode(mode), &pe))
        e = burrow__os_errno(pe);
    os_decref(f);
    return os_wrap(f, OS_LIT("chmod"), e);
}

Error os_file_chown(OsFile *f, Int uid, Int gid) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return os_wrap(f, OS_LIT("chown"), burrow__os_err_file_closing);
    Error e = BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    (void)uid;
    (void)gid;
    e = burrow__os_errno_value(SYSCALL_EWINDOWS);
#else
    PalErrno pe = PAL_OK;
    if (!pal_fchown(f->fd, (int64_t)uid, (int64_t)gid, &pe))
        e = burrow__os_errno(pe);
#endif
    os_decref(f);
    return os_wrap(f, OS_LIT("chown"), e);
}

Error os_file_chdir(OsFile *f) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return os_wrap(f, OS_LIT("chdir"), burrow__os_err_file_closing);
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_fchdir(f->fd, &pe))
        e = burrow__os_errno(pe);
    os_decref(f);
    return os_wrap(f, OS_LIT("chdir"), e);
}

/* os.Pipe. The ends are called "|0" and "|1", as in Go. */
OsFile *os_pipe(Alloc *a, OsFile **w, Error *err) {
    BURROW_OUT(w, NULL);
    BURROW_OUT(err, BURROW_NO_ERROR);
    int64_t p[2];
    PalErrno pe = PAL_OK;
    if (!pal_pipe(p, 0, &pe)) {
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY)
        Str op = OS_LIT("pipe2");
#else
        Str op = OS_LIT("pipe");
#endif
        BURROW_OUT(err,
                   os_new_syscall_error(error_allocator(), op, burrow__os_errno(pe)));
        return NULL;
    }
    OsFile *rf = os_new(a, p[0], OS_LIT("|0"));
    OsFile *wf = rf == NULL ? NULL : os_new(a, p[1], OS_LIT("|1"));
    if (wf == NULL) {
        if (rf != NULL)
            os_file_free(rf);
        else
            pal_close(p[0], NULL);
        pal_close(p[1], NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(w, wf);
    return rf;
}

/* poll.FD.SetDeadline on something that is not on the poller. The closing
 * error comes back as it is, without the wrapping, as it does in Go. */
static Error os_no_deadline(OsFile *f) {
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return burrow__os_err_file_closing;
    os_decref(f);
    return os_err_no_deadline;
}

Error os_file_set_deadline(OsFile *f, Time t) {
    (void)t;
    return os_no_deadline(f);
}

Error os_file_set_read_deadline(OsFile *f, Time t) {
    (void)t;
    return os_no_deadline(f);
}

Error os_file_set_write_deadline(OsFile *f, Time t) {
    (void)t;
    return os_no_deadline(f);
}

/* ----------------------------------------------------------------- close */

Error os_file_close(OsFile *f) {
    if (f == NULL)
        return fs_err_invalid;
    if (!burrow__fdmu_incref_and_close(&f->mu))
        return fs_path_error_new(error_allocator(), OS_LIT("close"), f->name,
                                 fs_err_closed);
    Error e = os_decref(f);
    /* Wait until the descriptor is closed. If this was the only reference it
     * is closed already and the semaphore has its wakeup. */
    burrow__sema_acquire(&f->csema, false, false);
    if (BURROW_FAILED(e))
        return fs_path_error_new(error_allocator(), OS_LIT("close"), f->name, e);
    return BURROW_NO_ERROR;
}

void os_file_free(OsFile *f) {
    if (f == NULL || f->stdio)
        return;
    if (!burrow__fdmu_closing(&f->mu))
        os_file_close(f);
    mem_free(f->a, f, sizeof(OsFile) + (size_t)f->name.len, _Alignof(OsFile));
}

/* ----------------------------------------------------------------- stdio */

static SyncOnce os_std_once;
static OsFile os_std[3];
static OsFile *os_std_files[3];

static void os_std_init(void *env) {
    (void)env;
    static const Str names[3] = {
        {(const Byte *)"/dev/stdin", 10},
        {(const Byte *)"/dev/stdout", 11},
        {(const Byte *)"/dev/stderr", 11},
    };
    for (int i = 0; i < 3; i++) {
        int64_t h = pal_std_handle(i);
        if (h == PAL_INVALID_HANDLE)
            continue;
        os_std[i].fd = h;
        os_std[i].name = names[i];
        os_std[i].stdio = true;
        os_std_files[i] = &os_std[i];
    }
}

static OsFile *os_std_get(int i) {
    sync_once_do(&os_std_once, BURROW_FN(Func, os_std_init, NULL));
    return os_std_files[i];
}

OsFile *os_stdin_file(void) {
    return os_std_get(0);
}

OsFile *os_stdout_file(void) {
    return os_std_get(1);
}

OsFile *os_stderr_file(void) {
    return os_std_get(2);
}

/* -------------------------------------------------------------- adapters */

static Int os_vt_read(void *self, Slice p, Error *err) {
    return os_file_read((OsFile *)self, p, err);
}

static Int os_vt_write(void *self, Slice p, Error *err) {
    return os_file_write((OsFile *)self, p, err);
}

static Error os_vt_close(void *self) {
    return os_file_close((OsFile *)self);
}

static int64_t os_vt_seek(void *self, int64_t offset, int whence, Error *err) {
    return os_file_seek((OsFile *)self, offset, whence, err);
}

static Int os_vt_read_at(void *self, Slice p, int64_t off, Error *err) {
    return os_file_read_at((OsFile *)self, p, off, err);
}

static Int os_vt_write_at(void *self, Slice p, int64_t off, Error *err) {
    return os_file_write_at((OsFile *)self, p, off, err);
}

static Int os_vt_write_string(void *self, Str s, Error *err) {
    return os_file_write_string((OsFile *)self, s, err);
}

static FsFileInfo os_vt_stat(void *self, Alloc *a, Error *err) {
    return os_file_stat((OsFile *)self, a, err);
}

static const IoReaderVT os_reader_vt = {&os_file_desc, os_vt_read};
static const IoWriterVT os_writer_vt = {&os_file_desc, os_vt_write};
static const IoCloserVT os_closer_vt = {&os_file_desc, os_vt_close};
static const IoSeekerVT os_seeker_vt = {&os_file_desc, os_vt_seek};
static const IoReaderAtVT os_reader_at_vt = {&os_file_desc, os_vt_read_at};
static const IoWriterAtVT os_writer_at_vt = {&os_file_desc, os_vt_write_at};
static const IoStringWriterVT os_string_writer_vt = {&os_file_desc, os_vt_write_string};
static const IoReadWriteSeekerVT os_read_write_seeker_vt = {
    {&os_file_desc, os_vt_read},
    {&os_file_desc, os_vt_write},
    {&os_file_desc, os_vt_seek},
};
static Slice os_vt_read_dir(void *self, Alloc *a, Int n, Error *err) {
    return os_file_read_dir((OsFile *)self, a, n, err);
}

static const FsFileVT os_fs_file_vt = {
    {{&os_file_desc, os_vt_read}, {&os_file_desc, os_vt_close}},
    os_vt_stat,
    os_vt_read_dir,
};

IoReader os_file_as_io_reader(OsFile *f) {
    return (IoReader){&os_reader_vt, f};
}

IoWriter os_file_as_io_writer(OsFile *f) {
    return (IoWriter){&os_writer_vt, f};
}

IoCloser os_file_as_io_closer(OsFile *f) {
    return (IoCloser){&os_closer_vt, f};
}

IoSeeker os_file_as_io_seeker(OsFile *f) {
    return (IoSeeker){&os_seeker_vt, f};
}

IoReaderAt os_file_as_io_reader_at(OsFile *f) {
    return (IoReaderAt){&os_reader_at_vt, f};
}

IoWriterAt os_file_as_io_writer_at(OsFile *f) {
    return (IoWriterAt){&os_writer_at_vt, f};
}

IoStringWriter os_file_as_io_string_writer(OsFile *f) {
    return (IoStringWriter){&os_string_writer_vt, f};
}

IoReadWriteSeeker os_file_as_io_read_write_seeker(OsFile *f) {
    return (IoReadWriteSeeker){&os_read_write_seeker_vt, f};
}

FsFile os_file_as_fs_file(OsFile *f) {
    return (FsFile){&os_fs_file_vt, f};
}

/* ------------------------------------------------- ReadFrom and WriteTo */

static const IoReaderVT os_plain_reader_vt = {&os_file_plain_desc, os_vt_read};
static const IoWriterVT os_plain_writer_vt = {&os_file_plain_desc, os_vt_write};

int64_t os_file_read_from(OsFile *f, IoReader r, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    /* genericReadFrom, whose errors are not wrapped. */
    return io_copy(heap_allocator(), (IoWriter){&os_plain_writer_vt, f}, r, err);
}

int64_t os_file_write_to(OsFile *f, IoWriter w, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return 0;
    }
    return io_copy(heap_allocator(), w, (IoReader){&os_plain_reader_vt, f}, err);
}

/* ----------------------------------------------------------- SyscallConn */

static Error os_raw_control(void *self, SyscallFdFunc fn) {
    OsFile *f = (OsFile *)self;
    if (f == NULL)
        return fs_err_invalid;
    if (!os_incref(f))
        return burrow__os_err_file_closing;
    fn.f(fn.env, (Uintptr)f->fd);
    return os_decref(f);
}

#if defined(BURROW_OS_WINDOWS)
static Error os_raw_read(void *self, SyscallFdDoneFunc fn) {
    OsFile *f = (OsFile *)self;
    if (f == NULL)
        return fs_err_invalid;
    if (!os_read_lock(f))
        return burrow__os_err_file_closing;
    Error e = BURROW_NO_ERROR;
    /* Go waits with a zero byte WSARecv on the handle, which a file is not. */
    if (!fn.f(fn.env, (Uintptr)f->fd))
        e = burrow__os_errno_value((SyscallErrno)10038); /* WSAENOTSOCK */
    os_read_unlock(f);
    return e;
}

static Error os_raw_write(void *self, SyscallFdDoneFunc fn) {
    OsFile *f = (OsFile *)self;
    if (f == NULL)
        return fs_err_invalid;
    if (!os_write_lock(f))
        return burrow__os_err_file_closing;
    Error e = BURROW_NO_ERROR;
    if (!fn.f(fn.env, (Uintptr)f->fd))
        e = burrow__os_errno_value(SYSCALL_EWINDOWS);
    os_write_unlock(f);
    return e;
}
#else
BURROW_SENTINEL_ERROR(burrow__os_err_unsupported_wait,
                      "waiting for unsupported file type");

static Error os_raw_read(void *self, SyscallFdDoneFunc fn) {
    OsFile *f = (OsFile *)self;
    if (f == NULL)
        return fs_err_invalid;
    if (!os_read_lock(f))
        return burrow__os_err_file_closing;
    Error e = BURROW_NO_ERROR;
    if (!fn.f(fn.env, (Uintptr)f->fd))
        e = burrow__os_err_unsupported_wait;
    os_read_unlock(f);
    return e;
}

static Error os_raw_write(void *self, SyscallFdDoneFunc fn) {
    OsFile *f = (OsFile *)self;
    if (f == NULL)
        return fs_err_invalid;
    if (!os_write_lock(f))
        return burrow__os_err_file_closing;
    Error e = BURROW_NO_ERROR;
    if (!fn.f(fn.env, (Uintptr)f->fd))
        e = burrow__os_err_unsupported_wait;
    os_write_unlock(f);
    return e;
}
#endif

static const Type os_raw_conn_desc = {
    {(const Byte *)"rawConn", 7},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsFile),
    (uint16_t)_Alignof(OsFile),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f737263U, /* "osrc" */
    NULL,
};

static const SyscallRawConnVT os_raw_conn_vt = {
    &os_raw_conn_desc,
    os_raw_control,
    os_raw_read,
    os_raw_write,
};

SyscallRawConn os_file_syscall_conn(OsFile *f, Error *err) {
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (SyscallRawConn){&os_raw_conn_vt, f};
}
