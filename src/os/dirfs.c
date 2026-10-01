/* os.DirFS.
 *
 * Derived from Go's src/os/file.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

/* Go makes this error afresh on each call with errors.New. One sentinel says
 * the same and compares equal to itself, which Go's would not. */
BURROW_SENTINEL_ERROR(burrow__os_err_dir_fs_empty_root, "os: DirFS with empty root");

/* dirFS, a string. The bytes follow the struct. */
typedef struct OsDirFS {
    Str dir;
} OsDirFS;

static const Type os_dir_fs_desc = {
    {(const Byte *)"dirFS", 5},
    {(const Byte *)"os", 2},
    KIND_STRING,
    (uint32_t)sizeof(OsDirFS),
    (uint16_t)_Alignof(OsDirFS),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736466U, /* "osdf" */
    NULL,
};

/* dirFS.join: name, which has to be a valid io/fs name, under dir, in t. */
static Str os_dir_fs_join(Alloc *t, Str dir, Str name, Error *err) {
    if (dir.len == 0) {
        *err = burrow__os_err_dir_fs_empty_root;
        return (Str){NULL, 0};
    }
    Error le = BURROW_NO_ERROR;
    Str local = filepath_localize(t, name, &le);
    if (BURROW_FAILED(le)) {
        *err = fs_err_invalid;
        return (Str){NULL, 0};
    }
    Str full;
    if (os_is_path_separator(dir.p[dir.len - 1])) {
        full = burrow__os_cat3(t, dir, local, (Str){NULL, 0});
    } else {
        Byte sep = (Byte)OS_PATH_SEPARATOR;
        full = burrow__os_cat3(t, dir, str_from_bytes(&sep, 1), local);
    }
    if (full.p == NULL)
        *err = burrow_err_out_of_memory;
    return full;
}

/* Go sets Path on the *PathError it got back to the name it was asked for,
 * so the error talks about the FS's name and not the system's. Errors are
 * not changed in place here, so a new one is made. */
static Error os_dir_fs_rename(Error e, Str name) {
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    if (pe == NULL || pe != e.data)
        return e;
    return fs_path_error_new(error_allocator(), pe->op, name, pe->err);
}

typedef enum OsDirFSOp {
    OS_DFS_OPEN,
    OS_DFS_READ_FILE,
    OS_DFS_READ_DIR,
    OS_DFS_STAT,
    OS_DFS_LSTAT,
    OS_DFS_READ_LINK
} OsDirFSOp;

/* Every method is the same: join, call, and fix the name in the error. */
typedef union OsDirFSResult {
    FsFile file;
    Slice slice;
    FsFileInfo info;
    Str str;
} OsDirFSResult;

static bool os_dir_fs_do(void *self, Alloc *a, Str name, OsDirFSOp op, Str op_name,
                         OsDirFSResult *r, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    const OsDirFS *d = (const OsDirFS *)self;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error je = BURROW_NO_ERROR;
    Str full = os_dir_fs_join(arena_allocator(&ar), d->dir, name, &je);
    if (BURROW_FAILED(je)) {
        arena_free(&ar);
        BURROW_OUT(err, fs_path_error_new(error_allocator(), op_name, name, je));
        return false;
    }
    Error e = BURROW_NO_ERROR;
    switch (op) {
    case OS_DFS_OPEN: {
        OsFile *f = os_open(a, full, &e);
        if (f != NULL)
            r->file = os_file_as_fs_file(f);
        break;
    }
    case OS_DFS_READ_FILE:
        r->slice = os_read_file(a, full, &e);
        break;
    case OS_DFS_READ_DIR:
        r->slice = os_read_dir(a, full, &e);
        break;
    case OS_DFS_STAT:
        r->info = os_stat(a, full, &e);
        break;
    case OS_DFS_LSTAT:
        r->info = os_lstat(a, full, &e);
        break;
    case OS_DFS_READ_LINK:
        r->str = os_readlink(a, full, &e);
        break;
    default:
        break;
    }
    arena_free(&ar);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_dir_fs_rename(e, name));
        return false;
    }
    return true;
}

static FsFile os_dfs_open(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.file = {NULL, NULL}};
    os_dir_fs_do(self, a, name, OS_DFS_OPEN, OS_LIT("open"), &r, err);
    return r.file;
}

static Slice os_dfs_read_file(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.slice = slice_nil(TYPE_BYTE)};
    if (!os_dir_fs_do(self, a, name, OS_DFS_READ_FILE, OS_LIT("readfile"), &r, err))
        return slice_nil(TYPE_BYTE);
    return r.slice;
}

static Slice os_dfs_read_dir(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.slice = slice_nil(TYPE_FS_DIR_ENTRY)};
    if (!os_dir_fs_do(self, a, name, OS_DFS_READ_DIR, OS_LIT("readdir"), &r, err))
        return slice_nil(TYPE_FS_DIR_ENTRY);
    return r.slice;
}

static FsFileInfo os_dfs_stat(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.info = {NULL, NULL}};
    os_dir_fs_do(self, a, name, OS_DFS_STAT, OS_LIT("stat"), &r, err);
    return r.info;
}

static FsFileInfo os_dfs_lstat(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.info = {NULL, NULL}};
    os_dir_fs_do(self, a, name, OS_DFS_LSTAT, OS_LIT("lstat"), &r, err);
    return r.info;
}

static Str os_dfs_read_link(void *self, Alloc *a, Str name, Error *err) {
    OsDirFSResult r = {.str = {NULL, 0}};
    os_dir_fs_do(self, a, name, OS_DFS_READ_LINK, OS_LIT("readlink"), &r, err);
    return r.str;
}

static const FsVT os_dir_fs_vt = {
    &os_dir_fs_desc,
    os_dfs_open,
    os_dfs_read_dir,
    os_dfs_read_file,
    os_dfs_stat,
    NULL,
    NULL,
    os_dfs_read_link,
    os_dfs_lstat,
};

Fs os_dir_fs(Alloc *a, Str dir) {
    OsDirFS *d = (OsDirFS *)mem_alloc_nozero(a, sizeof(OsDirFS) + (size_t)dir.len,
                                             _Alignof(OsDirFS));
    if (d == NULL)
        return (Fs){NULL, NULL};
    Byte *p = (Byte *)(d + 1);
    if (dir.len > 0)
        memcpy(p, dir.p, (size_t)dir.len);
    d->dir = str_from_bytes(p, dir.len);
    return (Fs){&os_dir_fs_vt, d};
}
