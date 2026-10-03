/* os.Stat, os.Lstat, os.SameFile and the FileInfo behind them.
 *
 * Derived from Go's src/os/stat.go.
 * Go source: go1.27.1.
 *
 * The rest is stat_unix.go, stat_windows.go, types.go, types_unix.go and
 * path_unix.go's basename, from the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "internal.h"

#include <string.h>

/* os.fileStat. The name's bytes follow the struct in the same allocation. */
typedef struct OsFileStat {
    Str name;
    int64_t size;
    FsFileMode mode;
    Time mod_time;
    uint64_t dev;
    uint64_t ino;
} OsFileStat;

static const Type os_file_stat_desc = {
    {(const Byte *)"fileStat", 8},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsFileStat),
    (uint16_t)_Alignof(OsFileStat),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736673U, /* "osfs" */
    NULL,
};

static Str os_fs_name(void *self) {
    return ((OsFileStat *)self)->name;
}

static int64_t os_fs_size(void *self) {
    return ((OsFileStat *)self)->size;
}

static FsFileMode os_fs_mode(void *self) {
    return ((OsFileStat *)self)->mode;
}

static Time os_fs_mod_time(void *self) {
    return ((OsFileStat *)self)->mod_time;
}

static bool os_fs_is_dir(void *self) {
    return (((OsFileStat *)self)->mode & FS_MODE_DIR) != 0;
}

/* Go hands back a *syscall.Stat_t, which burrow has no type for yet. */
static Any os_fs_sys(void *self) {
    (void)self;
    return (Any){0};
}

static const FsFileInfoVT os_file_stat_vt = {
    &os_file_stat_desc, os_fs_name,   os_fs_size, os_fs_mode,
    os_fs_mod_time,     os_fs_is_dir, os_fs_sys,
};

/* basename: the last element of name, with trailing separators dropped, and
 * on Windows a drive letter too. */
static Str os_basename(Str name) {
#if defined(BURROW_OS_WINDOWS)
    if (name.len == 2 && name.p[1] == ':')
        name = str_from_bytes((const Byte *)".", 1);
    else if (name.len > 2 && name.p[1] == ':')
        name = str_from_bytes(name.p + 2, name.len - 2);
#endif
    Int i = name.len - 1;
    for (; i > 0 && os_is_path_separator(name.p[i]); i--)
        name.len = i;
    for (i--; i >= 0; i--) {
        if (os_is_path_separator(name.p[i])) {
            name = str_from_bytes(name.p + i + 1, name.len - i - 1);
            break;
        }
    }
    return name;
}

FsFileMode burrow__os_mode_of(uint32_t m) {
    FsFileMode mode = (FsFileMode)(m & 0777);
    switch (m & PAL_S_IFMT) {
    case PAL_S_IFBLK:
        mode |= FS_MODE_DEVICE;
        break;
    case PAL_S_IFCHR:
        mode |= FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE;
        break;
    case PAL_S_IFDIR:
        mode |= FS_MODE_DIR;
        break;
    case PAL_S_IFIFO:
        mode |= FS_MODE_NAMED_PIPE;
        break;
    case PAL_S_IFLNK:
        mode |= FS_MODE_SYMLINK;
        break;
    case PAL_S_IFSOCK:
        mode |= FS_MODE_SOCKET;
        break;
    case PAL_S_IFREG:
        break;
    case 0:
        /* What the Windows PAL says for a reparse point that is neither a
         * link nor a mount point, which Go calls irregular. */
        mode |= FS_MODE_IRREGULAR;
        break;
    default:
        break;
    }
    if (m & PAL_S_ISGID)
        mode |= FS_MODE_SETGID;
    if (m & PAL_S_ISUID)
        mode |= FS_MODE_SETUID;
    if (m & PAL_S_ISVTX)
        mode |= FS_MODE_STICKY;
    return mode;
}

OsFileInfo burrow__os_file_info(Alloc *a, Str name, const PalStat *st, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str base = os_basename(name);
    OsFileStat *fs = (OsFileStat *)mem_alloc_nozero(
        a, sizeof(OsFileStat) + (size_t)base.len, _Alignof(OsFileStat));
    if (fs == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (OsFileInfo){NULL, NULL};
    }
    Byte *p = (Byte *)(fs + 1);
    if (base.len > 0)
        memcpy(p, base.p, (size_t)base.len);
    fs->name = str_from_bytes(p, base.len);
    fs->size = st->size;
    fs->mode = burrow__os_mode_of(st->mode);
    fs->mod_time = time_from_unix(st->mtime_sec, st->mtime_nsec);
    fs->dev = st->dev;
    fs->ino = st->ino;
    return (OsFileInfo){&os_file_stat_vt, fs};
}

static OsFileInfo os_stat_op(Alloc *a, Str name, bool follow, Error *err) {
    Str op = follow ? str_from_bytes((const Byte *)"stat", 4)
                    : str_from_bytes((const Byte *)"lstat", 5);
    OsFileInfo none = {NULL, NULL};
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e)) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), op, name, e));
        return none;
    }
#if defined(BURROW_OS_WINDOWS)
    /* Lstat of a name ending in a separator follows a link in its last part,
     * the way POSIX resolves one, as Go's does on Windows. */
    if (!follow && name.len > 0 && os_is_path_separator(name.p[name.len - 1]))
        follow = true;
#endif
    PalStat st;
    PalErrno pe = PAL_OK;
    bool ok = follow ? pal_stat(c.p, &st, &pe) : pal_lstat(c.p, &st, &pe);
    if (!ok)
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (!ok) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), op, name, e));
        return none;
    }
    return burrow__os_file_info(a, name, &st, err);
}

OsFileInfo os_stat(Alloc *a, Str name, Error *err) {
    return os_stat_op(a, name, true, err);
}

OsFileInfo os_lstat(Alloc *a, Str name, Error *err) {
    return os_stat_op(a, name, false, err);
}

bool os_same_file(OsFileInfo fi1, OsFileInfo fi2) {
    if (fi1.vt != &os_file_stat_vt || fi2.vt != &os_file_stat_vt)
        return false;
    const OsFileStat *a = (const OsFileStat *)fi1.data;
    const OsFileStat *b = (const OsFileStat *)fi2.data;
    return a->dev == b->dev && a->ino == b->ino;
}
