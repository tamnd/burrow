/* What Go writes by hand in syscall for Windows: Open and the other calls os
 * makes on files, the standard handles, the socket calls and the Sockaddr
 * types as Windows has them, the completion port calls, NewCallback and the
 * OIDs.
 *
 * Derived from Go's src/syscall/syscall_windows.go and types_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

/* The file flags Go keeps to itself in types_windows.go, and the error
 * numbers it names there without exporting them. */
#define WIN_FILE_WRITE_EA 0x00000010U
#define WIN_O_DIRECTORY 0x4000
#define WIN_FILE_FLAG_OPEN_NO_RECALL 0x00100000U
#define WIN_FILE_FLAG_SESSION_AWARE 0x00800000U
#define WIN_FILE_FLAG_POSIX_SEMANTICS 0x01000000U
#define WIN_FILE_FLAG_DELETE_ON_CLOSE 0x04000000U
#define WIN_FILE_FLAG_SEQUENTIAL_SCAN 0x08000000U
#define WIN_FILE_FLAG_RANDOM_ACCESS 0x10000000U
#define WIN_FILE_FLAG_NO_BUFFERING 0x20000000U
#define WIN_FILE_FLAG_WRITE_THROUGH 0x80000000U
#define WIN_ERROR_NOT_ENOUGH_MEMORY ((SyscallErrno)8)
#define WIN_ERROR_INVALID_PARAMETER ((SyscallErrno)87)
#define WIN_IO_REPARSE_TAG_MOUNT_POINT 0xA0000003U
#define WIN_SYMLINK_FLAG_RELATIVE 1U

/* Go's fileFlagsMask, the top 12 bits of an Open flag, and
 * validFileFlagsMask, the FILE_FLAG_ bits it lets through to CreateFile. */
#define WIN_FILE_FLAGS_MASK 0xFFF00000U
#define WIN_VALID_FILE_FLAGS_MASK                                                      \
    ((uint32_t)SYSCALL_FILE_FLAG_OPEN_REPARSE_POINT |                                  \
     (uint32_t)SYSCALL_FILE_FLAG_BACKUP_SEMANTICS |                                    \
     (uint32_t)SYSCALL_FILE_FLAG_OVERLAPPED | WIN_FILE_FLAG_OPEN_NO_RECALL |           \
     WIN_FILE_FLAG_SESSION_AWARE | WIN_FILE_FLAG_POSIX_SEMANTICS |                     \
     WIN_FILE_FLAG_DELETE_ON_CLOSE | WIN_FILE_FLAG_SEQUENTIAL_SCAN |                   \
     WIN_FILE_FLAG_RANDOM_ACCESS | WIN_FILE_FLAG_NO_BUFFERING |                        \
     WIN_FILE_FLAG_WRITE_THROUGH)

/* Go's socket_error, what the socket calls return when they fail. */
#define WIN_SOCKET_ERROR ((Uintptr)0xFFFFFFFFU)

static Error win_errno(SyscallErrno e) {
    return burrow__syscall_errno_err(e);
}

/* The Errno in err, or 0 when it has none, for the places Go compares an
 * error with an Errno. */
static SyscallErrno win_errno_of(Error err) {
    if (BURROW_OK(err))
        return 0;
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

/* Go's makeInheritSa. */
static SyscallSecurityAttributes win_inherit_sa(void) {
    SyscallSecurityAttributes sa;
    memset(&sa, 0, sizeof sa);
    sa.length = (uint32_t)sizeof sa;
    sa.inherit_handle = 1;
    return sa;
}

/* ----------------------------------------------------------- std handles */

static SyncOnce win_std_once;
static SyscallHandle win_std[3];

static void win_std_init(void *env) {
    (void)env;
    static const Int which[3] = {SYSCALL_STD_INPUT_HANDLE, SYSCALL_STD_OUTPUT_HANDLE,
                                 SYSCALL_STD_ERROR_HANDLE};
    for (int i = 0; i < 3; i++)
        win_std[i] = syscall_get_std_handle(which[i], NULL);
}

SyscallHandle *burrow__syscall_std_handle(Int i) {
    sync_once_do(&win_std_once, BURROW_FN(Func, win_std_init, NULL));
    return &win_std[i];
}

/* ------------------------------------------------------------------ files */

SyscallHandle syscall_open(Str path, Int mode, uint32_t perm, Error *err) {
    if (path.len == 0) {
        BURROW_OUT(err, win_errno(SYSCALL_ERROR_FILE_NOT_FOUND));
        return SYSCALL_INVALID_HANDLE;
    }
    burrow__SyscallWString w;
    uint16_t *namep = burrow__syscall_wstring(&w, path, err);
    if (namep == NULL)
        return SYSCALL_INVALID_HANDLE;
    Int access_flags = mode & (SYSCALL_O_RDONLY | SYSCALL_O_WRONLY | SYSCALL_O_RDWR);
    uint32_t access = 0;
    switch (access_flags) {
    case SYSCALL_O_RDONLY:
        access = (uint32_t)SYSCALL_GENERIC_READ;
        break;
    case SYSCALL_O_WRONLY:
        access = (uint32_t)SYSCALL_GENERIC_WRITE;
        break;
    case SYSCALL_O_RDWR:
        access = (uint32_t)SYSCALL_GENERIC_READ | (uint32_t)SYSCALL_GENERIC_WRITE;
        break;
    default:
        break;
    }
    if ((mode & SYSCALL_O_CREAT) != 0)
        access |= (uint32_t)SYSCALL_GENERIC_WRITE;
    if ((mode & SYSCALL_O_APPEND) != 0) {
        /* GENERIC_WRITE goes unless O_TRUNC needs it, and everything it gives
         * but FILE_WRITE_DATA comes back, because without FILE_WRITE_DATA a
         * write appends, and GENERIC_WRITE without it writes from the start. */
        if ((mode & SYSCALL_O_TRUNC) == 0)
            access &= ~(uint32_t)SYSCALL_GENERIC_WRITE;
        access |= (uint32_t)SYSCALL_FILE_APPEND_DATA |
                  (uint32_t)SYSCALL_FILE_WRITE_ATTRIBUTES | WIN_FILE_WRITE_EA |
                  (uint32_t)SYSCALL_STANDARD_RIGHTS_WRITE |
                  (uint32_t)SYSCALL_SYNCHRONIZE;
    }
    uint32_t sharemode =
        (uint32_t)SYSCALL_FILE_SHARE_READ | (uint32_t)SYSCALL_FILE_SHARE_WRITE;
    SyscallSecurityAttributes inherit = win_inherit_sa();
    SyscallSecurityAttributes *sa = NULL;
    if ((mode & SYSCALL_O_CLOEXEC) == 0)
        sa = &inherit;
    uint32_t attrs = (uint32_t)SYSCALL_FILE_ATTRIBUTE_NORMAL;
    if ((perm & SYSCALL_S_IWRITE) == 0)
        attrs = (uint32_t)SYSCALL_FILE_ATTRIBUTE_READONLY;
    uint32_t file_flags = (uint32_t)mode & WIN_FILE_FLAGS_MASK;
    if ((file_flags & ~WIN_VALID_FILE_FLAGS_MASK) != 0) {
        burrow__syscall_wstring_free(&w);
        BURROW_OUT(err, fs_err_invalid);
        return SYSCALL_INVALID_HANDLE;
    }
    attrs |= file_flags;
    /* Unix will not open a directory for writing, so a write open leaves
     * BACKUP_SEMANTICS out and CreateFile says ERROR_ACCESS_DENIED for a
     * directory, which becomes EISDIR below. A read open may be of a
     * directory, which CreateFile only opens with it. */
    if (access_flags != SYSCALL_O_WRONLY && access_flags != SYSCALL_O_RDWR)
        attrs |= (uint32_t)SYSCALL_FILE_FLAG_BACKUP_SEMANTICS;
    if ((mode & SYSCALL_O_SYNC) != 0)
        attrs |= WIN_FILE_FLAG_WRITE_THROUGH;
    /* Not CREATE_ALWAYS, which would replace a file with a new read only one
     * when perm says read only, go.dev/issue/38225. O_TRUNC truncates after the
     * open instead. */
    uint32_t createmode;
    if ((mode & (SYSCALL_O_CREAT | SYSCALL_O_EXCL)) ==
        (SYSCALL_O_CREAT | SYSCALL_O_EXCL)) {
        createmode = (uint32_t)SYSCALL_CREATE_NEW;
        attrs |= (uint32_t)SYSCALL_FILE_FLAG_OPEN_REPARSE_POINT;
    } else if ((mode & SYSCALL_O_CREAT) == SYSCALL_O_CREAT) {
        createmode = (uint32_t)SYSCALL_OPEN_ALWAYS;
    } else {
        createmode = (uint32_t)SYSCALL_OPEN_EXISTING;
    }
    Error e = BURROW_NO_ERROR;
    SyscallHandle h = burrow__syscall_create_file(namep, access, sharemode, sa,
                                                  createmode, attrs, 0, &e);
    if (h == SYSCALL_INVALID_HANDLE) {
        if (win_errno_of(e) == SYSCALL_ERROR_ACCESS_DENIED &&
            (attrs & (uint32_t)SYSCALL_FILE_FLAG_BACKUP_SEMANTICS) == 0) {
            Error e1 = BURROW_NO_ERROR;
            uint32_t fa = syscall_get_file_attributes(namep, &e1);
            if (BURROW_OK(e1) && (fa & (uint32_t)SYSCALL_FILE_ATTRIBUTE_DIRECTORY) != 0)
                e = win_errno(SYSCALL_EISDIR);
        }
        burrow__syscall_wstring_free(&w);
        BURROW_OUT(err, e);
        return h;
    }
    burrow__syscall_wstring_free(&w);
    if ((mode & WIN_O_DIRECTORY) != 0) {
        SyscallByHandleFileInformation fi;
        memset(&fi, 0, sizeof fi);
        Error e2 = syscall_get_file_information_by_handle(h, &fi);
        if (BURROW_FAILED(e2)) {
            (void)syscall_close_handle(h);
            BURROW_OUT(err, e2);
            return SYSCALL_INVALID_HANDLE;
        }
        if ((fi.file_attributes & (uint32_t)SYSCALL_FILE_ATTRIBUTE_DIRECTORY) == 0) {
            (void)syscall_close_handle(h);
            BURROW_OUT(err, win_errno(SYSCALL_ENOTDIR));
            return SYSCALL_INVALID_HANDLE;
        }
    }
    /* A file that was just made has nothing to truncate. */
    if ((mode & SYSCALL_O_TRUNC) == SYSCALL_O_TRUNC &&
        (createmode == (uint32_t)SYSCALL_OPEN_EXISTING ||
         (createmode == (uint32_t)SYSCALL_OPEN_ALWAYS &&
          win_errno_of(e) == SYSCALL_ERROR_ALREADY_EXISTS))) {
        Error e3 = syscall_ftruncate(h, 0);
        if (win_errno_of(e3) == WIN_ERROR_INVALID_PARAMETER) {
            /* The handle cannot be truncated. Unix ignores O_TRUNC on a pipe
             * or a terminal, and so does this. */
            Error e4 = BURROW_NO_ERROR;
            uint32_t t = syscall_get_file_type(h, &e4);
            if (BURROW_OK(e4) && (t == (uint32_t)SYSCALL_FILE_TYPE_PIPE ||
                                  t == (uint32_t)SYSCALL_FILE_TYPE_CHAR))
                e3 = BURROW_NO_ERROR;
        }
        if (BURROW_FAILED(e3)) {
            (void)syscall_close_handle(h);
            BURROW_OUT(err, e3);
            return SYSCALL_INVALID_HANDLE;
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return h;
}

Error syscall_read_file(SyscallHandle fd, Slice p, uint32_t *done,
                        SyscallOverlapped *overlapped) {
    return burrow__syscall_read_file(fd, p, done, overlapped);
}

Error syscall_write_file(SyscallHandle fd, Slice p, uint32_t *done,
                         SyscallOverlapped *overlapped) {
    return burrow__syscall_write_file(fd, p, done, overlapped);
}

Int syscall_read(SyscallHandle fd, Slice p, Error *err) {
    uint32_t done = 0;
    Error e = syscall_read_file(fd, p, &done, NULL);
    if (BURROW_FAILED(e)) {
        /* Windows says ERROR_BROKEN_PIPE at the end of a pipe, stdin
         * included. */
        if (win_errno_of(e) == SYSCALL_ERROR_BROKEN_PIPE)
            e = BURROW_NO_ERROR;
        BURROW_OUT(err, e);
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Int)done;
}

Int syscall_write(SyscallHandle fd, Slice p, Error *err) {
    uint32_t done = 0;
    Error e = syscall_write_file(fd, p, &done, NULL);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? 0 : (Int)done;
}

/* Go's procSetFilePointerEx, which the generated wrappers do not have. */
static SyscallLazyProc win_set_file_pointer_ex = {
    .name = {(const Byte *)"SetFilePointerEx", 16},
    .l = &burrow__syscall_mods[BURROW__SYSCALL_MOD_KERNEL32],
};

int64_t syscall_seek(SyscallHandle fd, int64_t offset, Int whence, Error *err) {
    uint32_t w = 0;
    switch (whence) {
    case 0:
        w = (uint32_t)SYSCALL_FILE_BEGIN;
        break;
    case 1:
        w = (uint32_t)SYSCALL_FILE_CURRENT;
        break;
    case 2:
        w = (uint32_t)SYSCALL_FILE_END;
        break;
    default:
        break;
    }
    int64_t newoffset = 0;
    SyscallErrno e1 = 0;
    Uintptr fn = syscall_lazy_proc_addr(&win_set_file_pointer_ex);
#if UINTPTR_MAX > 0xFFFFFFFFU
    const Uintptr args[] = {(Uintptr)fd, (Uintptr)offset, (Uintptr)(void *)&newoffset,
                            (Uintptr)w};
#else
    /* The distance is a LARGE_INTEGER, which takes two words here. */
    const Uintptr args[] = {(Uintptr)fd, (Uintptr)(uint32_t)offset,
                            (Uintptr)(uint32_t)((uint64_t)offset >> 32),
                            (Uintptr)(void *)&newoffset, (Uintptr)w};
#endif
    (void)burrow__syscall_n(fn, args, (Int)(sizeof args / sizeof args[0]), NULL, &e1);
    if (e1 != 0) {
        BURROW_OUT(err, win_errno(e1));
        return newoffset;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return newoffset;
}

Error syscall_close(SyscallHandle fd) {
    return syscall_close_handle(fd);
}

Error syscall_fsync(SyscallHandle fd) {
    return syscall_flush_file_buffers(fd);
}

Error syscall_ftruncate(SyscallHandle fd, int64_t length) {
    /* FILE_END_OF_FILE_INFO, which FileEndOfFileInfo, 6, sets. */
    int64_t info = length;
    return burrow__syscall_set_file_information_by_handle(fd, 6, &info,
                                                          (uint32_t)sizeof info);
}

Str syscall_getwd(Alloc *a, Error *err) {
    /* The directory may not fit in 300 units with long paths turned on, and
     * may change between calls, so this asks until it fits. */
    uint32_t n = 300;
    for (;;) {
        uint16_t *b = BURROW_NEW_N(heap_allocator(), uint16_t, n);
        if (b == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return BURROW_STR_EMPTY;
        }
        Error e = BURROW_NO_ERROR;
        uint32_t got = syscall_get_current_directory(n, b, &e);
        if (BURROW_FAILED(e)) {
            mem_free(heap_allocator(), b, (size_t)n * sizeof(uint16_t),
                     _Alignof(uint16_t));
            BURROW_OUT(err, e);
            return BURROW_STR_EMPTY;
        }
        if (got <= n) {
            Str s =
                syscall_utf16_to_string(a, (Slice){b, (Int)got, (Int)n, TYPE_UINT16});
            mem_free(heap_allocator(), b, (size_t)n * sizeof(uint16_t),
                     _Alignof(uint16_t));
            BURROW_OUT(err, BURROW_NO_ERROR);
            return s;
        }
        mem_free(heap_allocator(), b, (size_t)n * sizeof(uint16_t), _Alignof(uint16_t));
        n = got;
    }
}

/* Calls f on path as UTF-16. */
static Error win_path_call(Str path, Error (*f)(uint16_t *)) {
    burrow__SyscallWString w;
    Error e = BURROW_NO_ERROR;
    uint16_t *p = burrow__syscall_wstring(&w, path, &e);
    if (p == NULL)
        return e;
    e = f(p);
    burrow__syscall_wstring_free(&w);
    return e;
}

static Error win_create_directory(uint16_t *p) {
    return syscall_create_directory(p, NULL);
}

Error syscall_chdir(Str path) {
    return win_path_call(path, syscall_set_current_directory);
}

Error syscall_mkdir(Str path, uint32_t mode) {
    (void)mode;
    return win_path_call(path, win_create_directory);
}

Error syscall_rmdir(Str path) {
    return win_path_call(path, syscall_remove_directory);
}

Error syscall_unlink(Str path) {
    return win_path_call(path, syscall_delete_file);
}

Error syscall_rename(Str oldpath, Str newpath) {
    burrow__SyscallWString wf, wt;
    Error e = BURROW_NO_ERROR;
    uint16_t *from = burrow__syscall_wstring(&wf, oldpath, &e);
    if (from == NULL)
        return e;
    uint16_t *to = burrow__syscall_wstring(&wt, newpath, &e);
    if (to == NULL) {
        burrow__syscall_wstring_free(&wf);
        return e;
    }
    e = syscall_move_file(from, to);
    burrow__syscall_wstring_free(&wt);
    burrow__syscall_wstring_free(&wf);
    return e;
}

Error syscall_chmod(Str path, uint32_t mode) {
    burrow__SyscallWString w;
    Error e = BURROW_NO_ERROR;
    uint16_t *p = burrow__syscall_wstring(&w, path, &e);
    if (p == NULL)
        return e;
    uint32_t attrs = syscall_get_file_attributes(p, &e);
    if (BURROW_OK(e)) {
        if ((mode & SYSCALL_S_IWRITE) != 0)
            attrs &= ~(uint32_t)SYSCALL_FILE_ATTRIBUTE_READONLY;
        else
            attrs |= (uint32_t)SYSCALL_FILE_ATTRIBUTE_READONLY;
        e = syscall_set_file_attributes(p, attrs);
    }
    burrow__syscall_wstring_free(&w);
    return e;
}

Str syscall_computer_name(Alloc *a, Error *err) {
    uint16_t b[SYSCALL_MAX_COMPUTERNAME_LENGTH + 1];
    uint32_t n = (uint32_t)(sizeof b / sizeof b[0]);
    Error e = syscall_get_computer_name(b, &n);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return syscall_utf16_to_string(a, (Slice){b, (Int)n, (Int)n, TYPE_UINT16});
}

Error syscall_gettimeofday(SyscallTimeval *tv) {
    SyscallFiletime ft;
    memset(&ft, 0, sizeof ft);
    syscall_get_system_time_as_file_time(&ft);
    *tv = syscall_nsec_to_timeval(syscall_filetime_nanoseconds(&ft));
    return BURROW_NO_ERROR;
}

Error syscall_pipe(Slice p) {
    if (p.len != 2)
        return win_errno(SYSCALL_EINVAL);
    SyscallHandle r = 0, w = 0;
    SyscallSecurityAttributes sa = win_inherit_sa();
    Error e = syscall_create_pipe(&r, &w, &sa, 0);
    if (BURROW_FAILED(e))
        return e;
    ((SyscallHandle *)p.p)[0] = r;
    ((SyscallHandle *)p.p)[1] = w;
    return BURROW_NO_ERROR;
}

/* Sets path's access and modification times, the way Utimes and UtimesNano
 * both do once they have them as Filetimes. */
static Error win_set_times(Str path, SyscallFiletime *at, SyscallFiletime *wt) {
    burrow__SyscallWString w;
    Error e = BURROW_NO_ERROR;
    uint16_t *p = burrow__syscall_wstring(&w, path, &e);
    if (p == NULL)
        return e;
    SyscallHandle h = syscall_create_file(
        p, (uint32_t)SYSCALL_FILE_WRITE_ATTRIBUTES, (uint32_t)SYSCALL_FILE_SHARE_WRITE,
        NULL, (uint32_t)SYSCALL_OPEN_EXISTING,
        (uint32_t)SYSCALL_FILE_FLAG_BACKUP_SEMANTICS, 0, &e);
    burrow__syscall_wstring_free(&w);
    if (BURROW_FAILED(e))
        return e;
    e = syscall_set_file_time(h, NULL, at, wt);
    (void)syscall_close(h);
    return e;
}

Error syscall_utimes(Str path, Slice tv) {
    if (tv.len != 2)
        return win_errno(SYSCALL_EINVAL);
    const SyscallTimeval *t = (const SyscallTimeval *)tv.p;
    SyscallFiletime at, wt;
    memset(&at, 0, sizeof at);
    memset(&wt, 0, sizeof wt);
    if (syscall_timeval_nanoseconds(&t[0]) != 0)
        at = syscall_nsec_to_filetime(syscall_timeval_nanoseconds(&t[0]));
    if (syscall_timeval_nanoseconds(&t[1]) != 0)
        wt = syscall_nsec_to_filetime(syscall_timeval_nanoseconds(&t[1]));
    return win_set_times(path, &at, &wt);
}

Error syscall_utimes_nano(Str path, Slice ts) {
    if (ts.len != 2)
        return win_errno(SYSCALL_EINVAL);
    const SyscallTimespec *t = (const SyscallTimespec *)ts.p;
    SyscallFiletime at, wt;
    memset(&at, 0, sizeof at);
    memset(&wt, 0, sizeof wt);
    /* _UTIME_OMIT, which os/file_windows.go uses too. */
    if (t[0].nsec != -1)
        at = syscall_nsec_to_filetime(syscall_timespec_to_nsec(t[0]));
    if (t[1].nsec != -1)
        wt = syscall_nsec_to_filetime(syscall_timespec_to_nsec(t[1]));
    return win_set_times(path, &at, &wt);
}

/* Go's fdpath: the path fd is open on, which Windows starts with \\?\. */
static uint16_t *win_fdpath(SyscallHandle fd, uint16_t *buf, uint32_t size,
                            uint32_t *len, uint32_t *heap_size, Error *err) {
    *heap_size = 0;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        /* FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, both 0. */
        uint32_t n =
            burrow__syscall_get_final_path_name_by_handle(fd, buf, size, 0, &e);
        if (BURROW_OK(e)) {
            *len = n;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return buf;
        }
        if (*heap_size != 0)
            mem_free(heap_allocator(), buf, (size_t)*heap_size * sizeof(uint16_t),
                     _Alignof(uint16_t));
        *heap_size = 0;
        if (win_errno_of(e) != WIN_ERROR_NOT_ENOUGH_MEMORY) {
            BURROW_OUT(err, e);
            return NULL;
        }
        buf = BURROW_NEW_N(heap_allocator(), uint16_t, n);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        size = n;
        *heap_size = n;
    }
}

Error syscall_fchdir(SyscallHandle fd) {
    uint16_t buf[SYSCALL_MAX_PATH + 1];
    uint32_t len = 0, heap_size = 0;
    Error e = BURROW_NO_ERROR;
    uint16_t *path = win_fdpath(fd, buf, (uint32_t)(sizeof buf / sizeof buf[0]), &len,
                                &heap_size, &e);
    if (path == NULL)
        return e;
    /* SetCurrentDirectory takes the \\?\, but Getwd would give it back and
     * other calls do not take it, so it goes. */
    uint16_t *p = path;
    if (len >= 4 && p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    e = syscall_set_current_directory(p);
    if (heap_size != 0)
        mem_free(heap_allocator(), path, (size_t)heap_size * sizeof(uint16_t),
                 _Alignof(uint16_t));
    return e;
}

Error syscall_link(Str oldpath, Str newpath) {
    (void)oldpath;
    (void)newpath;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_symlink(Str path, Str link) {
    (void)path;
    (void)link;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_fchmod(SyscallHandle fd, uint32_t mode) {
    (void)fd;
    (void)mode;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_chown(Str path, Int uid, Int gid) {
    (void)path;
    (void)uid;
    (void)gid;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_lchown(Str path, Int uid, Int gid) {
    (void)path;
    (void)uid;
    (void)gid;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_fchown(SyscallHandle fd, Int uid, Int gid) {
    (void)fd;
    (void)uid;
    (void)gid;
    return win_errno(SYSCALL_EWINDOWS);
}

Int syscall_getuid(void) {
    return -1;
}

Int syscall_geteuid(void) {
    return -1;
}

Int syscall_getgid(void) {
    return -1;
}

Int syscall_getegid(void) {
    return -1;
}

Slice syscall_getgroups(Alloc *a, Error *err) {
    (void)a;
    BURROW_OUT(err, win_errno(SYSCALL_EWINDOWS));
    return (Slice){NULL, 0, 0, NULL};
}

Int syscall_getpid(void) {
    return (Int)burrow__syscall_get_current_process_id();
}

Int syscall_getppid(void) {
    Error e = BURROW_NO_ERROR;
    SyscallHandle snapshot =
        syscall_create_toolhelp32_snapshot((uint32_t)SYSCALL_TH32CS_SNAPPROCESS, 0, &e);
    if (BURROW_FAILED(e))
        return -1;
    uint32_t pid = (uint32_t)syscall_getpid();
    SyscallProcessEntry32 pe;
    memset(&pe, 0, sizeof pe);
    pe.size = (uint32_t)sizeof pe;
    Int ppid = -1;
    for (e = syscall_process32_first(snapshot, &pe); BURROW_OK(e);
         e = syscall_process32_next(snapshot, &pe)) {
        if (pe.process_id == pid) {
            ppid = (Int)pe.parent_process_id;
            break;
        }
    }
    (void)syscall_close_handle(snapshot);
    return ppid;
}

/* --------------------------------------------------------------- Readlink */

/* Go's reparseDataBuffer and the two kinds of reparse point it reads, from
 * types_windows.go: the tag, the length and a reserved word, then the
 * buffer, which for both kinds starts with the four name offsets and
 * lengths, and for a symbolic link has the flags after them. */
enum { WIN_REPARSE_HEADER = 8 };

static uint16_t win_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint32_t win_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Whether the n units at u start with the units of the ASCII prefix. */
static bool win_has_prefix(const uint16_t *u, Int n, const char *prefix) {
    Int k = (Int)strlen(prefix);
    if (n < k)
        return false;
    for (Int i = 0; i < k; i++) {
        if (u[i] != (uint16_t)(uint8_t)prefix[i])
            return false;
    }
    return true;
}

Int syscall_readlink(Str path, Slice buf, Error *err) {
    burrow__SyscallWString w;
    Error e = BURROW_NO_ERROR;
    uint16_t *pathp = burrow__syscall_wstring(&w, path, &e);
    if (pathp == NULL) {
        BURROW_OUT(err, e);
        return -1;
    }
    SyscallHandle fd = syscall_create_file(
        pathp, (uint32_t)SYSCALL_GENERIC_READ, 0, NULL, (uint32_t)SYSCALL_OPEN_EXISTING,
        (uint32_t)SYSCALL_FILE_FLAG_OPEN_REPARSE_POINT |
            (uint32_t)SYSCALL_FILE_FLAG_BACKUP_SEMANTICS,
        0, &e);
    burrow__syscall_wstring_free(&w);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return -1;
    }
    size_t size = SYSCALL_MAXIMUM_REPARSE_DATA_BUFFER_SIZE;
    uint8_t *rdb = mem_alloc(heap_allocator(), size, 8);
    if (rdb == NULL) {
        (void)syscall_close_handle(fd);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return -1;
    }
    uint32_t returned = 0;
    e = syscall_device_io_control(fd, (uint32_t)SYSCALL_FSCTL_GET_REPARSE_POINT, NULL,
                                  0, rdb, (uint32_t)size, &returned, NULL);
    (void)syscall_close_handle(fd);
    if (BURROW_FAILED(e)) {
        mem_free(heap_allocator(), rdb, size, 8);
        BURROW_OUT(err, e);
        return -1;
    }
    uint32_t tag = win_u32(rdb);
    const uint8_t *rb = rdb + WIN_REPARSE_HEADER;
    size_t names;
    if (tag == (uint32_t)SYSCALL_IO_REPARSE_TAG_SYMLINK)
        names = 12; /* four uint16_t and the flags */
    else if (tag == WIN_IO_REPARSE_TAG_MOUNT_POINT)
        names = 8;
    else {
        /* Some other kind of reparse point, not a link or a junction. */
        mem_free(heap_allocator(), rdb, size, 8);
        BURROW_OUT(err, win_errno(SYSCALL_ENOENT));
        return -1;
    }
    size_t off = win_u16(rb);
    size_t len = win_u16(rb + 2);
    const uint8_t *pb = rb + names;
    /* Go would panic on a name past the end of the buffer. */
    if (WIN_REPARSE_HEADER + names + off + len > size) {
        mem_free(heap_allocator(), rdb, size, 8);
        BURROW_OUT(err, win_errno(SYSCALL_EINVAL));
        return -1;
    }
    /* The name as aligned units, then as UTF-8 after them, which takes 3
     * bytes a unit at most, in one allocation. */
    Int units = (Int)((off + len) / 2 - off / 2);
    size_t tmp_size = (size_t)units * 2 + (size_t)units * 3 + 2;
    uint16_t *u = mem_alloc_nozero(heap_allocator(), tmp_size, _Alignof(uint16_t));
    if (u == NULL) {
        mem_free(heap_allocator(), rdb, size, 8);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return -1;
    }
    if (units > 0)
        memcpy(u, pb + off / 2 * 2, (size_t)units * 2);
    mem_free(heap_allocator(), rdb, size, 8);
    /* UTF16ToString stops at a NUL. */
    Int n = units;
    for (Int i = 0; i < units; i++) {
        if (u[i] == 0) {
            n = i;
            break;
        }
    }
    Int start = 0;
    Byte *s = (Byte *)(u + units);
    Int slen = 0;
    if (tag == (uint32_t)SYSCALL_IO_REPARSE_TAG_SYMLINK) {
        uint32_t flags = win_u32(rb + 8);
        if ((flags & WIN_SYMLINK_FLAG_RELATIVE) == 0 &&
            win_has_prefix(u, n, "\\??\\")) {
            start = 4;
            if (n - start >= 2 && u[start + 1] == ':') {
                /* \??\C:\foo\bar */
            } else if (win_has_prefix(u + start, n - start, "UNC\\")) {
                /* \??\UNC\foo\bar */
                start += 4;
                s[slen++] = '\\';
                s[slen++] = '\\';
            }
        }
    } else if (win_has_prefix(u, n, "\\??\\")) {
        /* \??\C:\foo\bar */
        start = 4;
    }
    slen += burrow__syscall_decode_wtf16(u + start, n - start, s + slen,
                                         (Int)tmp_size - units * 2 - slen);
    Int copied = slen < buf.len ? slen : buf.len;
    if (copied > 0)
        memcpy(buf.p, s, (size_t)copied);
    mem_free(heap_allocator(), u, tmp_size, _Alignof(uint16_t));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return copied;
}

/* ---------------------------------------------------- FindFirstFile and co */

/* Go's win32finddata1, the struct FindFirstFileW writes, with names one unit
 * longer than Win32finddata's. */
typedef struct WinFindData1 {
    uint32_t file_attributes;
    SyscallFiletime creation_time;
    SyscallFiletime last_access_time;
    SyscallFiletime last_write_time;
    uint32_t file_size_high;
    uint32_t file_size_low;
    uint32_t reserved0;
    uint32_t reserved1;
    uint16_t file_name[SYSCALL_MAX_PATH];
    uint16_t alternate_file_name[14];
} WinFindData1;

/* Go's copyFindData. */
static void win_copy_find_data(SyscallWin32finddata *dst, const WinFindData1 *src) {
    dst->file_attributes = src->file_attributes;
    dst->creation_time = src->creation_time;
    dst->last_access_time = src->last_access_time;
    dst->last_write_time = src->last_write_time;
    dst->file_size_high = src->file_size_high;
    dst->file_size_low = src->file_size_low;
    dst->reserved0 = src->reserved0;
    dst->reserved1 = src->reserved1;
    /* The last unit is a NUL, which is the one that does not fit. */
    memcpy(dst->file_name, src->file_name, sizeof dst->file_name);
    memcpy(dst->alternate_file_name, src->alternate_file_name,
           sizeof dst->alternate_file_name);
}

SyscallHandle syscall_find_first_file(uint16_t *name, SyscallWin32finddata *data,
                                      Error *err) {
    WinFindData1 d1;
    memset(&d1, 0, sizeof d1);
    SyscallErrno e1 = 0;
    Uintptr r0 = burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs[BURROW__SYSCALL_PROC_FIND_FIRST_FILE_W]),
        (const Uintptr[]){(Uintptr)(void *)name, (Uintptr)(void *)&d1}, 2, NULL, &e1);
    SyscallHandle h = (SyscallHandle)r0;
    if (h == SYSCALL_INVALID_HANDLE) {
        BURROW_OUT(err, win_errno(e1));
        return h;
    }
    win_copy_find_data(data, &d1);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return h;
}

Error syscall_find_next_file(SyscallHandle handle, SyscallWin32finddata *data) {
    WinFindData1 d1;
    memset(&d1, 0, sizeof d1);
    SyscallErrno e1 = 0;
    Uintptr r1 = burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs[BURROW__SYSCALL_PROC_FIND_NEXT_FILE_W]),
        (const Uintptr[]){(Uintptr)handle, (Uintptr)(void *)&d1}, 2, NULL, &e1);
    if (r1 == 0)
        return win_errno(e1);
    win_copy_find_data(data, &d1);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------- the rest of the files */

SyscallHandle syscall_create_file(uint16_t *name, uint32_t access, uint32_t mode,
                                  SyscallSecurityAttributes *sa, uint32_t createmode,
                                  uint32_t attrs, int32_t templatefile, Error *err) {
    Error e = BURROW_NO_ERROR;
    SyscallHandle h = burrow__syscall_create_file(name, access, mode, sa, createmode,
                                                  attrs, templatefile, &e);
    /* CreateFileW can say ERROR_ALREADY_EXISTS with a good handle. */
    if (h != SYSCALL_INVALID_HANDLE)
        e = BURROW_NO_ERROR;
    BURROW_OUT(err, e);
    return h;
}

Error syscall_get_startup_info(SyscallStartupInfo *si) {
    burrow__syscall_get_startup_info(si);
    return BURROW_NO_ERROR;
}

Error syscall_reg_enum_key_ex(SyscallHandle key, uint32_t index, uint16_t *name,
                              uint32_t *name_len, uint32_t *reserved, uint16_t *class_,
                              uint32_t *class_len, SyscallFiletime *last_write_time) {
    return burrow__syscall_reg_enum_key_ex(key, index, name, name_len, reserved, class_,
                                           class_len, last_write_time);
}

uint32_t syscall_format_message(uint32_t flags, uint32_t msgsrc, uint32_t msgid,
                                uint32_t langid, Slice buf, uint8_t *args, Error *err) {
    return burrow__syscall_format_message(flags, (Uintptr)msgsrc, msgid, langid, buf,
                                          args, err);
}

SyscallHandle syscall_create_io_completion_port(SyscallHandle filehandle,
                                                SyscallHandle cphandle, uint32_t key,
                                                uint32_t threadcnt, Error *err) {
    return burrow__syscall_create_io_completion_port(filehandle, cphandle, (Uintptr)key,
                                                     threadcnt, err);
}

Error syscall_get_queued_completion_status(SyscallHandle cphandle, uint32_t *qty,
                                           uint32_t *key,
                                           SyscallOverlapped **overlapped,
                                           uint32_t timeout) {
    Uintptr ukey = 0;
    Uintptr *pukey = NULL;
    if (key != NULL) {
        ukey = (Uintptr)*key;
        pukey = &ukey;
    }
    Error e = burrow__syscall_get_queued_completion_status(cphandle, qty, pukey,
                                                           overlapped, timeout);
    if (key != NULL) {
        *key = (uint32_t)ukey;
        if ((Uintptr)*key != ukey && BURROW_OK(e))
            e = errors_new(error_allocator(),
                           BURROW_S("GetQueuedCompletionStatus returned key overflow"));
    }
    return e;
}

Error syscall_post_queued_completion_status(SyscallHandle cphandle, uint32_t qty,
                                            uint32_t key,
                                            SyscallOverlapped *overlapped) {
    return burrow__syscall_post_queued_completion_status(cphandle, qty, (Uintptr)key,
                                                         overlapped);
}

Error syscall_load_cancel_io_ex(void) {
    return syscall_lazy_proc_find(
        &burrow__syscall_procs[BURROW__SYSCALL_PROC_CANCEL_IO_EX]);
}

Error syscall_load_set_file_completion_notification_modes(void) {
    return syscall_lazy_proc_find(
        &burrow__syscall_procs
            [BURROW__SYSCALL_PROC_SET_FILE_COMPLETION_NOTIFICATION_MODES]);
}

Error syscall_load_get_addr_info(void) {
    return syscall_lazy_proc_find(
        &burrow__syscall_procs[BURROW__SYSCALL_PROC_GET_ADDR_INFO_W]);
}

Error syscall_load_create_symbolic_link(void) {
    return syscall_lazy_proc_find(
        &burrow__syscall_procs[BURROW__SYSCALL_PROC_CREATE_SYMBOLIC_LINK_W]);
}

Uintptr syscall_new_callback(void (*fn)(void)) {
    if (fn == NULL)
        panic_str(BURROW_S("compileCallback: expected function with one uintptr-sized "
                           "result"));
    return (Uintptr)fn;
}

Uintptr syscall_new_callback_c_decl(void (*fn)(void)) {
    return syscall_new_callback(fn);
}

/* ---------------------------------------------------------------- the OIDs */

static uint8_t win_oid_server_auth[] = "1.3.6.1.5.5.7.3.1";
static uint8_t win_oid_server_gated[] = "1.3.6.1.4.1.311.10.3.3";
static uint8_t win_oid_sgc_netscape[] = "2.16.840.1.113730.4.1";

/* Each with the NUL after it, which Go writes as \x00. */
Slice syscall_oid_pkix_kp_server_auth = {win_oid_server_auth,
                                         sizeof win_oid_server_auth,
                                         sizeof win_oid_server_auth, TYPE_UINT8};
Slice syscall_oid_server_gated_crypto = {win_oid_server_gated,
                                         sizeof win_oid_server_gated,
                                         sizeof win_oid_server_gated, TYPE_UINT8};
Slice syscall_oid_sgc_netscape = {win_oid_sgc_netscape, sizeof win_oid_sgc_netscape,
                                  sizeof win_oid_sgc_netscape, TYPE_UINT8};

/* ------------------------------------------------------------- sockaddrs */

bool syscall_socket_disable_ipv6 = false;

static const Type win_inet4_desc = {
    {(const Byte *)"SockaddrInet4", 13},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrInet4),
    (uint16_t)_Alignof(SyscallSockaddrInet4),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616934U, /* "sai4" */
    NULL,
};

static const Type win_inet6_desc = {
    {(const Byte *)"SockaddrInet6", 13},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrInet6),
    (uint16_t)_Alignof(SyscallSockaddrInet6),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616936U, /* "sai6" */
    NULL,
};

static const Type win_unix_desc = {
    {(const Byte *)"SockaddrUnix", 12},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrUnix),
    (uint16_t)_Alignof(SyscallSockaddrUnix),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7361756eU, /* "saun" */
    NULL,
};

const Type *const TYPE_SYSCALL_SOCKADDR_INET4 = &win_inet4_desc;
const Type *const TYPE_SYSCALL_SOCKADDR_INET6 = &win_inet6_desc;
const Type *const TYPE_SYSCALL_SOCKADDR_UNIX = &win_unix_desc;

/* The port, which Windows keeps in network order, as two bytes. */
static void win_put_port(void *raw_port, Int port) {
    uint8_t *p = (uint8_t *)raw_port;
    p[0] = (uint8_t)(port >> 8);
    p[1] = (uint8_t)port;
}

static Int win_get_port(const void *raw_port) {
    const uint8_t *p = (const uint8_t *)raw_port;
    return ((Int)p[0] << 8) + (Int)p[1];
}

static void *win_inet4_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrInet4 *sa = (SyscallSockaddrInet4 *)self;
    if (sa->port < 0 || sa->port > 0xFFFF) {
        BURROW_OUT(err, win_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.family = SYSCALL_AF_INET;
    win_put_port(&sa->raw.port, sa->port);
    memcpy(sa->raw.addr, sa->addr, sizeof sa->raw.addr);
    *len = (uint32_t)sizeof sa->raw;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static void *win_inet6_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrInet6 *sa = (SyscallSockaddrInet6 *)self;
    if (sa->port < 0 || sa->port > 0xFFFF) {
        BURROW_OUT(err, win_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.family = SYSCALL_AF_INET6;
    win_put_port(&sa->raw.port, sa->port);
    sa->raw.scope_id = sa->zone_id;
    memcpy(sa->raw.addr, sa->addr, sizeof sa->raw.addr);
    *len = (uint32_t)sizeof sa->raw;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

/* Windows takes the names Linux does: one that starts with @ or a NUL is
 * abstract, goes with a NUL first and none after, and may fill the whole
 * path. Any other needs room for the NUL after it. */
static void *win_unix_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrUnix *sa = (SyscallSockaddrUnix *)self;
    Int n = sa->name.len;
    Int max = (Int)sizeof sa->raw.path;
    bool abstract = n > 0 && (sa->name.p[0] == '@' || sa->name.p[0] == 0);
    if (n > max || (n == max && !abstract)) {
        BURROW_OUT(err, win_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.family = SYSCALL_AF_UNIX;
    if (n > 0)
        memcpy(sa->raw.path, sa->name.p, (size_t)n);
    uint32_t sl = (uint32_t)(2 + n);
    if (abstract)
        sa->raw.path[0] = 0;
    else if (n > 0)
        sl++;
    *len = sl;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static const SyscallSockaddrVT win_inet4_vt = {&win_inet4_desc, win_inet4_sockaddr};
static const SyscallSockaddrVT win_inet6_vt = {&win_inet6_desc, win_inet6_sockaddr};
static const SyscallSockaddrVT win_unix_vt = {&win_unix_desc, win_unix_sockaddr};

SyscallSockaddr syscall_sockaddr_inet4_as_sockaddr(SyscallSockaddrInet4 *sa) {
    return (SyscallSockaddr){&win_inet4_vt, sa};
}

SyscallSockaddr syscall_sockaddr_inet6_as_sockaddr(SyscallSockaddrInet6 *sa) {
    return (SyscallSockaddr){&win_inet6_vt, sa};
}

SyscallSockaddr syscall_sockaddr_unix_as_sockaddr(SyscallSockaddrUnix *sa) {
    return (SyscallSockaddr){&win_unix_vt, sa};
}

/* The size of the one allocation behind a Sockaddr this file made. */
static size_t win_alloc_size(SyscallSockaddr sa) {
    size_t n = sa.vt->self_type->size;
    if (sa.vt == &win_unix_vt)
        n += (size_t)((SyscallSockaddrUnix *)sa.data)->name.len;
    return n;
}

void syscall_sockaddr_free(Alloc *a, SyscallSockaddr sa) {
    if (sa.vt == NULL || sa.data == NULL)
        return;
    mem_free(a, sa.data, win_alloc_size(sa), sa.vt->self_type->align);
}

SyscallSockaddr syscall_raw_sockaddr_any_sockaddr(SyscallRawSockaddrAny *rsa, Alloc *a,
                                                  Error *err) {
    switch (rsa->addr.family) {
    case SYSCALL_AF_UNIX: {
        /* An abstract name comes with a NUL first, which Go shows as @, in
         * rsa itself, and ends at the first NUL after. */
        SyscallRawSockaddrUnix *pp = (SyscallRawSockaddrUnix *)(void *)rsa;
        if (pp->path[0] == 0)
            pp->path[0] = '@';
        Int n = 0;
        while (n < (Int)sizeof pp->path && pp->path[n] != 0)
            n++;
        SyscallSockaddrUnix *sa = (SyscallSockaddrUnix *)mem_alloc(
            a, sizeof(SyscallSockaddrUnix) + (size_t)n, _Alignof(SyscallSockaddrUnix));
        if (sa == NULL)
            break;
        Byte *name = (Byte *)(sa + 1);
        if (n > 0)
            memcpy(name, pp->path, (size_t)n);
        sa->name = (Str){name, n};
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (SyscallSockaddr){&win_unix_vt, sa};
    }
    case SYSCALL_AF_INET: {
        const SyscallRawSockaddrInet4 *pp =
            (const SyscallRawSockaddrInet4 *)(void *)rsa;
        SyscallSockaddrInet4 *sa = (SyscallSockaddrInet4 *)mem_alloc(
            a, sizeof(SyscallSockaddrInet4), _Alignof(SyscallSockaddrInet4));
        if (sa == NULL)
            break;
        sa->port = win_get_port(&pp->port);
        memcpy(sa->addr, pp->addr, sizeof sa->addr);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (SyscallSockaddr){&win_inet4_vt, sa};
    }
    case SYSCALL_AF_INET6: {
        const SyscallRawSockaddrInet6 *pp =
            (const SyscallRawSockaddrInet6 *)(void *)rsa;
        SyscallSockaddrInet6 *sa = (SyscallSockaddrInet6 *)mem_alloc(
            a, sizeof(SyscallSockaddrInet6), _Alignof(SyscallSockaddrInet6));
        if (sa == NULL)
            break;
        sa->port = win_get_port(&pp->port);
        sa->zone_id = pp->scope_id;
        memcpy(sa->addr, pp->addr, sizeof sa->addr);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return (SyscallSockaddr){&win_inet6_vt, sa};
    }
    default:
        BURROW_OUT(err, win_errno(SYSCALL_EAFNOSUPPORT));
        return (SyscallSockaddr){NULL, NULL};
    }
    BURROW_OUT(err, burrow_err_out_of_memory);
    return (SyscallSockaddr){NULL, NULL};
}

/* sa's system form, or NULL and 0 for the zero Sockaddr. */
static void *win_raw(SyscallSockaddr sa, uint32_t *len, Error *err) {
    *len = 0;
    if (sa.vt == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return NULL;
    }
    return sa.vt->sockaddr(sa.data, len, err);
}

/* ---------------------------------------------------------------- sockets */

SyscallHandle syscall_socket(Int domain, Int typ, Int proto, Error *err) {
    if (domain == SYSCALL_AF_INET6 && syscall_socket_disable_ipv6) {
        BURROW_OUT(err, win_errno(SYSCALL_EAFNOSUPPORT));
        return SYSCALL_INVALID_HANDLE;
    }
    return burrow__syscall_socket((int32_t)domain, (int32_t)typ, (int32_t)proto, err);
}

Error syscall_bind(SyscallHandle fd, SyscallSockaddr sa) {
    uint32_t n = 0;
    Error e = BURROW_NO_ERROR;
    void *ptr = win_raw(sa, &n, &e);
    if (BURROW_FAILED(e))
        return e;
    return burrow__syscall_bind(fd, ptr, (int32_t)n);
}

Error syscall_connect(SyscallHandle fd, SyscallSockaddr sa) {
    uint32_t n = 0;
    Error e = BURROW_NO_ERROR;
    void *ptr = win_raw(sa, &n, &e);
    if (BURROW_FAILED(e))
        return e;
    return burrow__syscall_connect(fd, ptr, (int32_t)n);
}

SyscallSockaddr syscall_getsockname(Alloc *a, SyscallHandle fd, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    int32_t l = (int32_t)sizeof rsa;
    Error e = burrow__syscall_getsockname(fd, &rsa, &l);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (SyscallSockaddr){NULL, NULL};
    }
    return syscall_raw_sockaddr_any_sockaddr(&rsa, a, err);
}

SyscallSockaddr syscall_getpeername(Alloc *a, SyscallHandle fd, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    int32_t l = (int32_t)sizeof rsa;
    Error e = burrow__syscall_getpeername(fd, &rsa, &l);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (SyscallSockaddr){NULL, NULL};
    }
    return syscall_raw_sockaddr_any_sockaddr(&rsa, a, err);
}

Error syscall_listen(SyscallHandle s, Int n) {
    return burrow__syscall_listen(s, (int32_t)n);
}

Error syscall_shutdown(SyscallHandle fd, Int how) {
    return burrow__syscall_shutdown(fd, (int32_t)how);
}

SyscallHandle syscall_accept(Alloc *a, SyscallHandle fd, SyscallSockaddr *sa,
                             Error *err) {
    (void)a;
    (void)fd;
    if (sa != NULL)
        *sa = (SyscallSockaddr){NULL, NULL};
    BURROW_OUT(err, win_errno(SYSCALL_EWINDOWS));
    return 0;
}

Int syscall_recvfrom(Alloc *a, SyscallHandle fd, Slice p, Int flags,
                     SyscallSockaddr *from, Error *err) {
    (void)a;
    (void)fd;
    (void)p;
    (void)flags;
    if (from != NULL)
        *from = (SyscallSockaddr){NULL, NULL};
    BURROW_OUT(err, win_errno(SYSCALL_EWINDOWS));
    return 0;
}

Error syscall_sendto(SyscallHandle fd, Slice p, Int flags, SyscallSockaddr to) {
    (void)fd;
    (void)p;
    (void)flags;
    (void)to;
    return win_errno(SYSCALL_EWINDOWS);
}

Error syscall_setsockopt_timeval(SyscallHandle fd, Int level, Int opt,
                                 SyscallTimeval *tv) {
    (void)fd;
    (void)level;
    (void)opt;
    (void)tv;
    return win_errno(SYSCALL_EWINDOWS);
}

Int syscall_getsockopt_int(SyscallHandle fd, Int level, Int opt, Error *err) {
    int32_t optval = 0;
    int32_t optlen = (int32_t)sizeof optval;
    Error e = syscall_getsockopt(fd, (int32_t)level, (int32_t)opt, (uint8_t *)&optval,
                                 &optlen);
    BURROW_OUT(err, e);
    return (Int)optval;
}

Error syscall_setsockopt_int(SyscallHandle fd, Int level, Int opt, Int value) {
    int32_t v = (int32_t)value;
    return syscall_setsockopt(fd, (int32_t)level, (int32_t)opt, (uint8_t *)&v,
                              (int32_t)sizeof v);
}

Error syscall_setsockopt_linger(SyscallHandle fd, Int level, Int opt,
                                SyscallLinger *l) {
    /* Go's sysLinger, the struct linger Windows has. */
    struct {
        uint16_t onoff;
        uint16_t linger;
    } sys = {(uint16_t)l->onoff, (uint16_t)l->linger};
    return syscall_setsockopt(fd, (int32_t)level, (int32_t)opt, (uint8_t *)&sys,
                              (int32_t)sizeof sys);
}

Error syscall_setsockopt_inet4_addr(SyscallHandle fd, Int level, Int opt,
                                    const uint8_t value[4]) {
    uint8_t v[4];
    memcpy(v, value, sizeof v);
    return syscall_setsockopt(fd, (int32_t)level, (int32_t)opt, v, 4);
}

Error syscall_setsockopt_ip_mreq(SyscallHandle fd, Int level, Int opt,
                                 SyscallIPMreq *mreq) {
    return syscall_setsockopt(fd, (int32_t)level, (int32_t)opt, (uint8_t *)mreq,
                              (int32_t)sizeof *mreq);
}

Error syscall_setsockopt_ipv6_mreq(SyscallHandle fd, Int level, Int opt,
                                   SyscallIPv6Mreq *mreq) {
    return syscall_setsockopt(fd, (int32_t)level, (int32_t)opt, (uint8_t *)mreq,
                              (int32_t)sizeof *mreq);
}

Error syscall_wsa_sendto(SyscallHandle s, SyscallWSABuf *bufs, uint32_t bufcnt,
                         uint32_t *sent, uint32_t flags, SyscallSockaddr to,
                         SyscallOverlapped *overlapped, uint8_t *croutine) {
    uint32_t len = 0;
    Error e = BURROW_NO_ERROR;
    void *rsa = win_raw(to, &len, &e);
    if (BURROW_FAILED(e))
        return e;
    SyscallErrno e1 = 0;
    Uintptr r1 = burrow__syscall_n(
        syscall_lazy_proc_addr(
            &burrow__syscall_procs[BURROW__SYSCALL_PROC_WSA_SEND_TO]),
        (const Uintptr[]){(Uintptr)s, (Uintptr)(void *)bufs, (Uintptr)bufcnt,
                          (Uintptr)(void *)sent, (Uintptr)flags, (Uintptr)rsa,
                          (Uintptr)(int32_t)len, (Uintptr)(void *)overlapped,
                          (Uintptr)(void *)croutine},
        9, NULL, &e1);
    if (r1 == WIN_SOCKET_ERROR)
        return win_errno(e1);
    return BURROW_NO_ERROR;
}

SyscallGUID syscall_wsaid_connectex = {
    0x25a207b9, 0xddf3, 0x4660, {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}};

/* Go's connectExFunc: ConnectEx's address and why it could not be found,
 * set once under win_connectex_once. */
static SyncOnce win_connectex_once;
static Uintptr win_connectex_addr;
static Error win_connectex_err;

static void win_connectex_init(void *env) {
    (void)env;
    Error e = BURROW_NO_ERROR;
    SyscallHandle s =
        syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, SYSCALL_IPPROTO_TCP, &e);
    if (BURROW_OK(e)) {
        uint32_t n = 0;
        e = syscall_wsa_ioctl(s, (uint32_t)SYSCALL_SIO_GET_EXTENSION_FUNCTION_POINTER,
                              (uint8_t *)&syscall_wsaid_connectex,
                              (uint32_t)sizeof syscall_wsaid_connectex,
                              (uint8_t *)&win_connectex_addr,
                              (uint32_t)sizeof win_connectex_addr, &n, NULL, 0);
        (void)syscall_close_handle(s);
    }
    /* It is kept for every call after, so it cannot live in this
     * goroutine's errors. */
    if (BURROW_FAILED(e))
        win_connectex_err = error_retain(heap_allocator(), e);
}

Error syscall_load_connect_ex(void) {
    sync_once_do(&win_connectex_once, BURROW_FN(Func, win_connectex_init, NULL));
    return win_connectex_err;
}

Error syscall_connect_ex(SyscallHandle fd, SyscallSockaddr sa, uint8_t *send_buf,
                         uint32_t send_data_len, uint32_t *bytes_sent,
                         SyscallOverlapped *overlapped) {
    Error e = syscall_load_connect_ex();
    if (BURROW_FAILED(e)) {
        Str msg = error_text(e);
        Str prefix = BURROW_S("failed to find ConnectEx: ");
        Byte *b =
            mem_alloc_nozero(error_allocator(), (size_t)(prefix.len + msg.len), 1);
        if (b == NULL)
            return burrow_err_out_of_memory;
        memcpy(b, prefix.p, (size_t)prefix.len);
        if (msg.len > 0)
            memcpy(b + prefix.len, msg.p, (size_t)msg.len);
        return errors_new(error_allocator(), (Str){b, prefix.len + msg.len});
    }
    uint32_t n = 0;
    void *ptr = win_raw(sa, &n, &e);
    if (BURROW_FAILED(e))
        return e;
    SyscallErrno e1 = 0;
    Uintptr r1 = burrow__syscall_n(
        win_connectex_addr,
        (const Uintptr[]){(Uintptr)fd, (Uintptr)ptr, (Uintptr)(int32_t)n,
                          (Uintptr)(void *)send_buf, (Uintptr)send_data_len,
                          (Uintptr)(void *)bytes_sent, (Uintptr)(void *)overlapped},
        7, NULL, &e1);
    if (r1 == 0)
        return win_errno(e1);
    return BURROW_NO_ERROR;
}

#endif /* BURROW_OS_WINDOWS */
