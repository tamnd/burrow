/* Files on Windows.
 *
 * Win32 only, never the C runtime's _open family, which is the rule in
 * burrow/pal.h and the one docs/design/20-windows.md spends most of its length
 * on. The choices below are that document's, and each is Go's: how the open
 * flags map onto CreateFileW (section 6), how a mode is made up out of the file
 * attributes (section 7), how a link is read (section 9), and reading a
 * directory with GetFileInformationByHandleEx rather than FindFirstFileW
 * (section 10). Reading a directory lives here rather than in a file of its
 * own because there is one way to do it on Windows, where the POSIX side has
 * three.
 *
 * Paths arrive as UTF-8 and go to UTF-16 in a buffer on the stack, since
 * nothing in the PAL allocates. The conversion is WTF-8 in both directions, so
 * a name holding an unpaired surrogate, which NTFS allows, comes out of
 * pal_readdir as bytes that open the same file when they go back in.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Windows 7, as in the other backends. Everything here is from Vista or
 * earlier. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

#include <winsock2.h>

#include <windows.h>
#include <winioctl.h>

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX 0x80000023UL
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x000900a8UL
#endif

/* A name surrogate is a reparse point that stands for another name, which is
 * what symlinks, junctions and mount points are. IsReparseTagNameSurrogate is a
 * macro in the driver kit and not in the Win32 headers. */
#define TAG_NAME_SURROGATE 0x20000000UL

#define RW_MAX ((int64_t)1 << 30)

/* ---------------------------------------------------------------- UTF-16 */

/* UTF-8 into UTF-16, NUL terminated, into cap units. Bytes that are not UTF-8
 * become U+FFFD one at a time, and a three byte sequence for a surrogate
 * becomes that surrogate, which is WTF-8 and is Go's rule. */
static bool widen(const char *s, wchar_t *out, size_t cap, PalErrno *err) {
    if (s == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p != 0) {
        uint32_t c = 0xfffd;
        size_t len = 1;
        unsigned char b = p[0];
        if (b < 0x80) {
            c = b;
        } else if (b >= 0xc2 && b < 0xe0 && (p[1] & 0xc0) == 0x80) {
            c = ((uint32_t)(b & 0x1f) << 6) | (uint32_t)(p[1] & 0x3f);
            len = 2;
        } else if (b >= 0xe0 && b < 0xf0 && (p[1] & 0xc0) == 0x80 &&
                   (p[2] & 0xc0) == 0x80) {
            uint32_t v = ((uint32_t)(b & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) |
                         (uint32_t)(p[2] & 0x3f);
            if (v >= 0x800) {
                c = v;
                len = 3;
            }
        } else if (b >= 0xf0 && b < 0xf5 && (p[1] & 0xc0) == 0x80 &&
                   (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
            uint32_t v = ((uint32_t)(b & 0x07) << 18) |
                         ((uint32_t)(p[1] & 0x3f) << 12) |
                         ((uint32_t)(p[2] & 0x3f) << 6) | (uint32_t)(p[3] & 0x3f);
            if (v >= 0x10000 && v <= 0x10ffff) {
                c = v;
                len = 4;
            }
        }
        p += len;

        size_t need = c >= 0x10000 ? 2 : 1;
        if (n + need >= cap) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return false;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out[n++] = (wchar_t)(0xd800 + (c >> 10));
            out[n++] = (wchar_t)(0xdc00 + (c & 0x3ff));
        } else {
            out[n++] = (wchar_t)c;
        }
    }
    out[n] = 0;
    return true;
}

/* n units of UTF-16 into UTF-8, not NUL terminated. Returns the length, or -1
 * when it does not fit in cap bytes. An unpaired surrogate is written as its
 * own three bytes rather than replaced. */
static int64_t narrow(const wchar_t *w, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && (uint32_t)w[i + 1] >= 0xdc00 &&
            (uint32_t)w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + ((uint32_t)w[i + 1] - 0xdc00);
            i++;
        }
        size_t len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + len > cap)
            return -1;
        switch (len) {
        case 1:
            out[o++] = (char)c;
            break;
        case 2:
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
            break;
        case 3:
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
            break;
        default:
            out[o++] = (char)(0xf0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
            break;
        }
    }
    return (int64_t)o;
}

/* ---------------------------------------------------------------- helpers */

static bool file_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
    return false;
}

static int64_t file_fail_n(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
    return -1;
}

static bool handle_ok(int64_t fd, PalErrno *err) {
    if (fd == PAL_INVALID_HANDLE || fd == 0) {
        BURROW_OUT(err, PAL_EBADF);
        return false;
    }
    return true;
}

static HANDLE as_handle(int64_t fd) {
    return (HANDLE)(intptr_t)fd;
}

static bool count_ok(const void *buf, int64_t n, PalErrno *err) {
    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return true;
}

/* A FILETIME is hundreds of nanoseconds since 1601. */
#define EPOCH_DIFF_100NS 116444736000000000LL

static void time_split(int64_t t100, int64_t *sec, int32_t *nsec) {
    int64_t u = t100 - EPOCH_DIFF_100NS;
    int64_t s = u / 10000000;
    int64_t r = u % 10000000;
    if (r < 0) {
        s--;
        r += 10000000;
    }
    *sec = s;
    *nsec = (int32_t)(r * 100);
}

static FILETIME filetime_from_ns(int64_t ns) {
    int64_t t = ns / 100;
    if (ns % 100 < 0)
        t--;
    t += EPOCH_DIFF_100NS;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)((uint64_t)t & 0xffffffffu);
    ft.dwHighDateTime = (DWORD)((uint64_t)t >> 32);
    return ft;
}

/* Go's mode rules from os/types_windows.go, which section 7 of the Windows
 * design document spells out. A name surrogate that is not a symlink gets no
 * type bits at all, which is how PalStat says irregular. */
static uint32_t mode_from(DWORD attrs, DWORD tag, DWORD ftype) {
    uint32_t perm = (attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    uint32_t type = 0;
    bool reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    bool surrogate = reparse && (tag & TAG_NAME_SURROGATE) != 0;

    if (!surrogate) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            type = PAL_S_IFDIR;
            perm |= 0111;
        } else if (ftype == FILE_TYPE_PIPE) {
            type = PAL_S_IFIFO;
        } else if (ftype == FILE_TYPE_CHAR) {
            type = PAL_S_IFCHR;
        } else {
            type = PAL_S_IFREG;
        }
    }
    if (reparse) {
        if (tag == IO_REPARSE_TAG_SYMLINK)
            type = PAL_S_IFLNK;
        else if (tag == IO_REPARSE_TAG_AF_UNIX)
            type = PAL_S_IFSOCK;
    }
    return type | perm;
}

static int64_t large(LARGE_INTEGER v) {
    return (int64_t)v.QuadPart;
}

static bool stat_handle(HANDLE h, PalStat *out, PalErrno *err) {
    SetLastError(NO_ERROR);
    DWORD ftype = GetFileType(h);
    if (ftype == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR)
        return file_fail(err);

    *out = (PalStat){0};
    if (ftype == FILE_TYPE_PIPE || ftype == FILE_TYPE_CHAR) {
        out->mode = mode_from(0, 0, ftype);
        out->nlink = 1;
        return true;
    }

    BY_HANDLE_FILE_INFORMATION bi;
    FILE_BASIC_INFO basic;
    if (!GetFileInformationByHandle(h, &bi) ||
        !GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic))
        return file_fail(err);

    DWORD tag = 0;
    if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        FILE_ATTRIBUTE_TAG_INFO ti;
        if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti))
            tag = ti.ReparseTag;
    }

    out->size = ((int64_t)bi.nFileSizeHigh << 32) | (int64_t)bi.nFileSizeLow;
    out->mode = mode_from(bi.dwFileAttributes, tag, ftype);
    out->dev = bi.dwVolumeSerialNumber;
    out->ino = ((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
    out->nlink = bi.nNumberOfLinks;
    time_split(large(basic.LastAccessTime), &out->atime_sec, &out->atime_nsec);
    time_split(large(basic.LastWriteTime), &out->mtime_sec, &out->mtime_nsec);
    time_split(large(basic.ChangeTime), &out->ctime_sec, &out->ctime_nsec);
    time_split(large(basic.CreationTime), &out->btime_sec, &out->btime_nsec);
    return true;
}

/* ------------------------------------------------------------------ open */

int64_t pal_open(const char *path, uint32_t flags, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return PAL_INVALID_HANDLE;

    uint32_t acc = flags & PAL_O_ACCMODE;
    DWORD access = 0;
    switch (acc) {
    case PAL_O_WRONLY:
        access = GENERIC_WRITE;
        break;
    case PAL_O_RDWR:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    default:
        access = GENERIC_READ;
        break;
    }
    if (flags & PAL_O_CREATE)
        access |= GENERIC_WRITE;
    if (flags & PAL_O_APPEND) {
        /* Everything GENERIC_WRITE grants except FILE_WRITE_DATA, which is what
         * makes every write go to the end. O_TRUNC needs FILE_WRITE_DATA to
         * truncate, so it keeps GENERIC_WRITE, as it does in Go. */
        if ((flags & PAL_O_TRUNC) == 0)
            access &= ~(DWORD)GENERIC_WRITE;
        access |= FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
                  STANDARD_RIGHTS_WRITE | SYNCHRONIZE;
    }

    DWORD create = OPEN_EXISTING;
    if ((flags & (PAL_O_CREATE | PAL_O_EXCL)) == (PAL_O_CREATE | PAL_O_EXCL))
        create = CREATE_NEW;
    else if (flags & PAL_O_CREATE)
        create = OPEN_ALWAYS;

    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    if ((flags & PAL_O_CREATE) && (mode & 0200) == 0)
        attrs = FILE_ATTRIBUTE_READONLY;
    /* Backup semantics is what lets CreateFileW open a directory. A write open
     * never asks for it, so writing to a directory fails. */
    if (acc == PAL_O_RDONLY || (flags & PAL_O_DIRECTORY))
        attrs |= FILE_FLAG_BACKUP_SEMANTICS;
    if (create == CREATE_NEW || (flags & PAL_O_NOFOLLOW))
        attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
    if (flags & PAL_O_SYNC)
        attrs |= FILE_FLAG_WRITE_THROUGH;

    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, create,
                           attrs, NULL);
    DWORD last = GetLastError();
    if (h == INVALID_HANDLE_VALUE) {
        if (last == ERROR_ACCESS_DENIED && acc != PAL_O_RDONLY) {
            DWORD fa = GetFileAttributesW(w);
            if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY)) {
                BURROW_OUT(err, PAL_EISDIR);
                return PAL_INVALID_HANDLE;
            }
        }
        BURROW_OUT(err, burrow__pal_errno_win(last));
        return PAL_INVALID_HANDLE;
    }

    PalErrno e = PAL_OK;
    if (flags & (PAL_O_DIRECTORY | PAL_O_NOFOLLOW)) {
        BY_HANDLE_FILE_INFORMATION bi;
        if (!GetFileInformationByHandle(h, &bi)) {
            e = burrow__pal_errno_win(GetLastError());
        } else if ((flags & PAL_O_DIRECTORY) &&
                   !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            e = PAL_ENOTDIR;
        } else if ((flags & PAL_O_NOFOLLOW) &&
                   (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            FILE_ATTRIBUTE_TAG_INFO ti;
            if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) &&
                ti.ReparseTag == IO_REPARSE_TAG_SYMLINK)
                e = PAL_ELOOP;
        }
    }

    /* O_TRUNC is done here rather than with TRUNCATE_EXISTING or
     * CREATE_ALWAYS, because CREATE_ALWAYS on a read only file replaces it
     * with a new read only file, which is Go issue 38225. A file this call
     * just made is empty already. */
    bool existed = create == OPEN_EXISTING ||
                   (create == OPEN_ALWAYS && last == ERROR_ALREADY_EXISTS);
    if (e == PAL_OK && (flags & PAL_O_TRUNC) && existed) {
        FILE_END_OF_FILE_INFO eof;
        eof.EndOfFile.QuadPart = 0;
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof)) {
            DWORD why = GetLastError();
            /* POSIX ignores O_TRUNC on a pipe or a terminal, and so does Go. */
            if (why != ERROR_INVALID_PARAMETER || GetFileType(h) == FILE_TYPE_DISK)
                e = burrow__pal_errno_win(why);
        }
    }

    if (e != PAL_OK) {
        CloseHandle(h);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }
    return (int64_t)(intptr_t)h;
}

bool pal_close(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return false;
    return CloseHandle(as_handle(fd)) || file_fail(err);
}

/* ------------------------------------------------------------ read, write */

static int64_t read_at(int64_t fd, void *buf, int64_t n, OVERLAPPED *o, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (n > RW_MAX)
        n = RW_MAX;

    DWORD got = 0;
    if (!ReadFile(as_handle(fd), buf, (DWORD)n, &got, o)) {
        DWORD why = GetLastError();
        /* The end of a file read at an offset, and the far end of a pipe going
         * away, are both the end of the file to a POSIX reader, and Go's os
         * package turns both into io.EOF. */
        if (why == ERROR_HANDLE_EOF || why == ERROR_BROKEN_PIPE)
            return 0;
        BURROW_OUT(err, burrow__pal_errno_win(why));
        return -1;
    }
    return (int64_t)got;
}

static int64_t write_at(int64_t fd, const void *buf, int64_t n, OVERLAPPED *o,
                        PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (n > RW_MAX)
        n = RW_MAX;

    DWORD put = 0;
    if (!WriteFile(as_handle(fd), buf, (DWORD)n, &put, o))
        return file_fail_n(err);
    return (int64_t)put;
}

int64_t pal_read(int64_t fd, void *buf, int64_t n, PalErrno *err) {
    return read_at(fd, buf, n, NULL, err);
}

int64_t pal_write(int64_t fd, const void *buf, int64_t n, PalErrno *err) {
    return write_at(fd, buf, n, NULL, err);
}

/* A synchronous handle moves its offset on a read at an offset too, which a
 * POSIX pread never does. So the offset is read first and put back after, the
 * way Go's pread on Windows does it. */
static bool offset_get(int64_t fd, LARGE_INTEGER *at) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return SetFilePointerEx(as_handle(fd), zero, at, FILE_CURRENT) != 0;
}

static OVERLAPPED overlapped_at(int64_t off) {
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)((uint64_t)off & 0xffffffffu);
    o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    return o;
}

int64_t pal_pread(int64_t fd, void *buf, int64_t n, int64_t off, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (off < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    if (!handle_ok(fd, err))
        return -1;
    LARGE_INTEGER cur;
    if (!offset_get(fd, &cur))
        return file_fail_n(err);
    OVERLAPPED o = overlapped_at(off);
    int64_t got = read_at(fd, buf, n, &o, err);
    SetFilePointerEx(as_handle(fd), cur, NULL, FILE_BEGIN);
    return got;
}

int64_t pal_pwrite(int64_t fd, const void *buf, int64_t n, int64_t off, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (off < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    if (!handle_ok(fd, err))
        return -1;
    LARGE_INTEGER cur;
    if (!offset_get(fd, &cur))
        return file_fail_n(err);
    OVERLAPPED o = overlapped_at(off);
    int64_t put = write_at(fd, buf, n, &o, err);
    SetFilePointerEx(as_handle(fd), cur, NULL, FILE_BEGIN);
    return put;
}

int64_t pal_seek(int64_t fd, int64_t off, int32_t whence, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return -1;

    DWORD method;
    switch (whence) {
    case PAL_SEEK_SET:
        method = FILE_BEGIN;
        break;
    case PAL_SEEK_CUR:
        method = FILE_CURRENT;
        break;
    case PAL_SEEK_END:
        method = FILE_END;
        break;
    default:
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    LARGE_INTEGER to, at;
    to.QuadPart = off;
    if (!SetFilePointerEx(as_handle(fd), to, &at, method))
        return file_fail_n(err);
    return (int64_t)at.QuadPart;
}

bool pal_fsync(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return false;
    return FlushFileBuffers(as_handle(fd)) || file_fail(err);
}

bool pal_ftruncate(int64_t fd, int64_t size, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return false;
    if (size < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = size;
    return SetFileInformationByHandle(as_handle(fd), FileEndOfFileInfo, &eof,
                                      sizeof eof) ||
           file_fail(err);
}

/* ------------------------------------------------------------------ stat */

/* What is left when a file cannot be opened even to read its attributes, which
 * is c:\pagefile.sys and anything else the system holds open with no sharing.
 * The directory listing still knows about it. */
static bool stat_find(const wchar_t *w, PalStat *out, PalErrno *err) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return file_fail(err);
    FindClose(h);

    DWORD tag =
        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
    *out = (PalStat){0};
    out->size = ((int64_t)fd.nFileSizeHigh << 32) | (int64_t)fd.nFileSizeLow;
    out->mode = mode_from(fd.dwFileAttributes, tag, FILE_TYPE_DISK);
    out->nlink = 1;
    LARGE_INTEGER t;
    t.LowPart = fd.ftLastAccessTime.dwLowDateTime;
    t.HighPart = (LONG)fd.ftLastAccessTime.dwHighDateTime;
    time_split(t.QuadPart, &out->atime_sec, &out->atime_nsec);
    t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    t.HighPart = (LONG)fd.ftLastWriteTime.dwHighDateTime;
    time_split(t.QuadPart, &out->mtime_sec, &out->mtime_nsec);
    out->ctime_sec = out->mtime_sec;
    out->ctime_nsec = out->mtime_nsec;
    t.LowPart = fd.ftCreationTime.dwLowDateTime;
    t.HighPart = (LONG)fd.ftCreationTime.dwHighDateTime;
    time_split(t.QuadPart, &out->btime_sec, &out->btime_nsec);
    return true;
}

static bool stat_path(const char *path, PalStat *out, bool follow, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;

    DWORD flags =
        FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION)
            return stat_find(w, out, err);
        return file_fail(err);
    }
    bool ok = stat_handle(h, out, err);
    CloseHandle(h);
    return ok;
}

bool pal_stat(const char *path, PalStat *out, PalErrno *err) {
    return stat_path(path, out, true, err);
}

bool pal_lstat(const char *path, PalStat *out, PalErrno *err) {
    return stat_path(path, out, false, err);
}

bool pal_fstat(int64_t fd, PalStat *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return false;
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return stat_handle(as_handle(fd), out, err);
}

/* ------------------------------------------------------------ names */

bool pal_unlink(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    if (DeleteFileW(w))
        return true;

    DWORD why = GetLastError();
    DWORD fa = GetFileAttributesW(w);
    if (fa != INVALID_FILE_ATTRIBUTES) {
        /* A symlink to a directory is a directory to Windows and goes with
         * RemoveDirectoryW, where unlink on POSIX takes any link. */
        if ((fa & FILE_ATTRIBUTE_DIRECTORY) && (fa & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (RemoveDirectoryW(w))
                return true;
            return file_fail(err);
        }
        if (fa & FILE_ATTRIBUTE_DIRECTORY) {
            BURROW_OUT(err, PAL_EISDIR);
            return false;
        }
        /* POSIX removes a file whatever its mode says, because removing is a
         * change to the directory and not to the file. Windows refuses a read
         * only file, so the attribute comes off first, and goes back on if the
         * delete still fails. */
        if (why == ERROR_ACCESS_DENIED && (fa & FILE_ATTRIBUTE_READONLY)) {
            if (SetFileAttributesW(w, fa & ~(DWORD)FILE_ATTRIBUTE_READONLY)) {
                if (DeleteFileW(w))
                    return true;
                why = GetLastError();
                SetFileAttributesW(w, fa);
            }
        }
    }
    BURROW_OUT(err, burrow__pal_errno_win(why));
    return false;
}

bool pal_rename(const char *from, const char *to, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t wf[PAL_WPATH_MAX], wt[PAL_WPATH_MAX];
    if (!widen(from, wf, PAL_WPATH_MAX, err) || !widen(to, wt, PAL_WPATH_MAX, err))
        return false;
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) || file_fail(err);
}

bool pal_mkdir(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    (void)mode; /* no permission bits to set, and Go ignores it here too */
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    return CreateDirectoryW(w, NULL) || file_fail(err);
}

bool pal_rmdir(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    return RemoveDirectoryW(w) || file_fail(err);
}

bool pal_chdir(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    return SetCurrentDirectoryW(w) || file_fail(err);
}

int64_t pal_getcwd(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    DWORD n = GetCurrentDirectoryW(PAL_WPATH_MAX, w);
    if (n == 0) {
        file_fail(err);
        return -1;
    }
    if (n >= PAL_WPATH_MAX) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    int64_t len = buf == NULL || cap <= 0 ? -1 : narrow(w, n, buf, (size_t)cap - 1);
    if (len < 0) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    buf[len] = 0;
    return len;
}

bool pal_link(const char *from, const char *to, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t wf[PAL_WPATH_MAX], wt[PAL_WPATH_MAX];
    if (!widen(from, wf, PAL_WPATH_MAX, err) || !widen(to, wt, PAL_WPATH_MAX, err))
        return false;
    return CreateHardLinkW(wt, wf, NULL) || file_fail(err);
}

static bool is_sep(wchar_t c) {
    return c == L'\\' || c == L'/';
}

bool pal_symlink(const char *target, const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t wt[PAL_WPATH_MAX], wp[PAL_WPATH_MAX];
    if (!widen(target, wt, PAL_WPATH_MAX, err) || !widen(path, wp, PAL_WPATH_MAX, err))
        return false;
    /* The kernel reads the stored target and does not know about '/'. */
    for (size_t i = 0; wt[i] != 0; i++)
        if (wt[i] == L'/')
            wt[i] = L'\\';

    /* A directory link and a file link are different things on Windows, so the
     * target is looked at. A relative target is relative to the directory the
     * link is in, not to ours. */
    bool absolute = is_sep(wt[0]) || (wt[0] != 0 && wt[1] == L':');
    DWORD fa;
    if (absolute) {
        fa = GetFileAttributesW(wt);
    } else {
        wchar_t full[PAL_WPATH_MAX];
        size_t dir = 0;
        for (size_t i = 0; wp[i] != 0; i++)
            if (is_sep(wp[i]) || wp[i] == L':')
                dir = i + 1;
        size_t tlen = wcslen(wt);
        if (dir + tlen + 1 > PAL_WPATH_MAX) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return false;
        }
        memcpy(full, wp, dir * sizeof(wchar_t));
        memcpy(full + dir, wt, (tlen + 1) * sizeof(wchar_t));
        fa = GetFileAttributesW(full);
    }
    DWORD flags = 0;
    if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY))
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

    /* Without administrator rights this works only in Developer Mode, and the
     * flag that asks for that is refused by Windows before 10 1703. */
    if (CreateSymbolicLinkW(wp, wt,
                            flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return true;
    if (GetLastError() == ERROR_INVALID_PARAMETER && CreateSymbolicLinkW(wp, wt, flags))
        return true;
    return file_fail(err);
}

/* REPARSE_DATA_BUFFER is in the driver kit's headers and not in Win32's, so
 * the two parts of it that are read here are written out. */
typedef struct ReparseHeader {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT sub_offset;
    USHORT sub_length;
    USHORT print_offset;
    USHORT print_length;
} ReparseHeader;

#define SYMLINK_FLAG_RELATIVE 1

int64_t pal_readlink(const char *path, char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!count_ok(buf, cap, err))
        return -1;
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return -1;

    HANDLE h = CreateFileW(
        w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return file_fail_n(err);

    /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE, as ULONGs for the alignment. */
    ULONG data[16 * 1024 / sizeof(ULONG)];
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, data, sizeof data,
                              &got, NULL);
    DWORD why = GetLastError();
    CloseHandle(h);
    if (!ok) {
        /* Not a reparse point at all is EINVAL on POSIX, which is what
         * readlink says of a file that is not a link. */
        BURROW_OUT(err, why == ERROR_NOT_A_REPARSE_POINT ? PAL_EINVAL
                                                         : burrow__pal_errno_win(why));
        return -1;
    }

    ReparseHeader hdr;
    memcpy(&hdr, data, sizeof hdr);
    const unsigned char *names;
    bool relative = false;
    if (hdr.tag == IO_REPARSE_TAG_SYMLINK) {
        ULONG lflags;
        memcpy(&lflags, (const unsigned char *)data + sizeof hdr, sizeof lflags);
        relative = (lflags & SYMLINK_FLAG_RELATIVE) != 0;
        names = (const unsigned char *)data + sizeof hdr + sizeof lflags;
    } else if (hdr.tag == IO_REPARSE_TAG_MOUNT_POINT) {
        names = (const unsigned char *)data + sizeof hdr;
    } else {
        BURROW_OUT(err, PAL_ENOENT); /* Go's answer for any other kind */
        return -1;
    }
    if ((size_t)hdr.sub_offset + hdr.sub_length >
        sizeof data - (size_t)(names - (const unsigned char *)data)) {
        BURROW_OUT(err, PAL_EIO);
        return -1;
    }
    const wchar_t *s = (const wchar_t *)(const void *)(names + hdr.sub_offset);
    size_t n = hdr.sub_length / sizeof(wchar_t);

    /* An absolute target is stored as an NT path. Go's normaliseLinkPath turns
     * \??\C:\x into C:\x, \??\UNC\h\s into \\h\s, and anything else under \??\
     * into \\?\ and the rest. */
    char *o = buf;
    int64_t room = cap;
    if (!relative && n >= 4 && s[0] == L'\\' && s[1] == L'?' && s[2] == L'?' &&
        s[3] == L'\\') {
        s += 4;
        n -= 4;
        const char *prefix = "\\\\?\\";
        if (n >= 2 && s[1] == L':') {
            prefix = "";
        } else if (n >= 4 && s[0] == L'U' && s[1] == L'N' && s[2] == L'C' &&
                   s[3] == L'\\') {
            prefix = "\\\\";
            s += 4;
            n -= 4;
        }
        int64_t plen = (int64_t)strlen(prefix);
        if (plen >= room) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        memcpy(o, prefix, (size_t)plen);
        o += plen;
        room -= plen;
    }
    int64_t len = narrow(s, n, o, (size_t)room);
    /* Full is as bad as over, for the reason the POSIX backend gives. */
    if (len < 0 || len >= room) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    return (int64_t)(o - buf) + len;
}

bool pal_chmod(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    DWORD fa = GetFileAttributesW(w);
    if (fa == INVALID_FILE_ATTRIBUTES)
        return file_fail(err);
    /* The owner's write bit is the only one Windows has anywhere to put. */
    DWORD want = (mode & 0200) ? (fa & ~(DWORD)FILE_ATTRIBUTE_READONLY)
                               : (fa | FILE_ATTRIBUTE_READONLY);
    if (want == fa)
        return true;
    return SetFileAttributesW(w, want) || file_fail(err);
}

bool pal_chown(const char *path, int64_t uid, int64_t gid, PalErrno *err) {
    (void)path;
    (void)uid;
    (void)gid;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

bool pal_utimes(const char *path, int64_t atime_ns, int64_t mtime_ns, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    wchar_t w[PAL_WPATH_MAX];
    if (!widen(path, w, PAL_WPATH_MAX, err))
        return false;
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return file_fail(err);
    FILETIME a = filetime_from_ns(atime_ns);
    FILETIME m = filetime_from_ns(mtime_ns);
    BOOL ok = SetFileTime(h, NULL, &a, &m);
    DWORD why = GetLastError();
    CloseHandle(h);
    if (!ok) {
        BURROW_OUT(err, burrow__pal_errno_win(why));
        return false;
    }
    return true;
}

int64_t pal_dup(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!handle_ok(fd, err))
        return -1;
    HANDLE self = GetCurrentProcess();
    HANDLE out = NULL;
    if (!DuplicateHandle(self, as_handle(fd), self, &out, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return file_fail_n(err);
    return (int64_t)(intptr_t)out;
}

bool pal_pipe(int64_t out[2], uint32_t flags, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (out == NULL || (flags & ~(uint32_t)PAL_O_NONBLOCK) != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    /* An anonymous pipe cannot be put in any mode that does not block. */
    if (flags & PAL_O_NONBLOCK) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    HANDLE r, w;
    if (!CreatePipe(&r, &w, NULL, 0))
        return file_fail(err);
    out[0] = (int64_t)(intptr_t)r;
    out[1] = (int64_t)(intptr_t)w;
    return true;
}

/* ---------------------------------------------------------- directories */

enum {
    DIR_FULL_INFO = 1u << 0, /* the filesystem refused FileIdBothDirectoryInfo */
    DIR_HAVE = 1u << 1       /* buf holds records from pos on */
};

bool pal_readdir(PalDir *d, PalDirEntry *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (d == NULL || out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (!handle_ok(d->fd, err))
        return false;

    for (;;) {
        if ((d->state & DIR_HAVE) == 0) {
            FILE_INFO_BY_HANDLE_CLASS cls = (d->state & DIR_FULL_INFO)
                                                ? FileFullDirectoryInfo
                                                : FileIdBothDirectoryInfo;
            if (!GetFileInformationByHandleEx(as_handle(d->fd), cls, d->buf,
                                              sizeof d->buf)) {
                DWORD why = GetLastError();
                if (why == ERROR_NO_MORE_FILES)
                    return false;
                if ((d->state & DIR_FULL_INFO) == 0 &&
                    (why == ERROR_INVALID_PARAMETER || why == ERROR_NOT_SUPPORTED ||
                     why == ERROR_INVALID_LEVEL)) {
                    d->state |= DIR_FULL_INFO;
                    continue;
                }
                BURROW_OUT(err, burrow__pal_errno_win(why));
                return false;
            }
            d->pos = 0;
            d->state |= DIR_HAVE;
        }

        const unsigned char *rec = (const unsigned char *)d->buf + d->pos;
        DWORD next, attrs, name_bytes, ea;
        const wchar_t *name;
        uint64_t ino;
        if (d->state & DIR_FULL_INFO) {
            const FILE_FULL_DIR_INFO *fi =
                (const FILE_FULL_DIR_INFO *)(const void *)rec;
            next = fi->NextEntryOffset;
            attrs = fi->FileAttributes;
            name_bytes = fi->FileNameLength;
            ea = fi->EaSize;
            name = fi->FileName;
            ino = fi->FileIndex;
        } else {
            const FILE_ID_BOTH_DIR_INFO *fi =
                (const FILE_ID_BOTH_DIR_INFO *)(const void *)rec;
            next = fi->NextEntryOffset;
            attrs = fi->FileAttributes;
            name_bytes = fi->FileNameLength;
            ea = fi->EaSize;
            name = fi->FileName;
            ino = (uint64_t)fi->FileId.QuadPart;
        }
        if (next == 0)
            d->state &= ~(uint32_t)DIR_HAVE;
        else
            d->pos += (int32_t)next;

        size_t n = name_bytes / sizeof(wchar_t);
        if ((n == 1 && name[0] == L'.') ||
            (n == 2 && name[0] == L'.' && name[1] == L'.'))
            continue;

        int64_t len = narrow(name, n, out->name, PAL_NAME_MAX);
        if (len < 0) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return false;
        }
        out->name[len] = '\0';
        out->name_len = (int32_t)len;
        /* EaSize holds the reparse tag when the entry is a reparse point, which
         * is how a listing knows a symlink without opening it. */
        DWORD tag = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? ea : 0;
        out->type = mode_from(attrs, tag, FILE_TYPE_DISK) & PAL_S_IFMT;
        out->ino = ino;
        return true;
    }
}

#endif /* BURROW_OS_WINDOWS */
