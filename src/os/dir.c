/* Reading directories, MkdirAll and RemoveAll.
 *
 * Derived from Go's src/os/dir.go.
 * Go source: go1.27.1.
 *
 * The reading is dir_unix.go and the entries are file_unix.go's unixDirent,
 * from the same release, on every system, with the PAL hiding how each one
 * hands its entries over. MkdirAll is path.go. RemoveAll is
 * removeall_noat.go, since the PAL has no unlinkat or openat for the version
 * Go uses on Unix, so its errors name remove and open where Go's name
 * unlinkat and openat.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/atomic.h"
#include "burrow/func.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/path/filepath.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

/* --------------------------------------------------------------- dirInfo */

/* os.dirInfo: the PAL's buffer and the lock that keeps two readers of the
 * same directory from sharing it at once. It comes from the heap, since
 * stdio files have no allocator, and goes when the descriptor does. */
typedef struct OsDirInfo {
    SyncMutex mu;
    PalDir d;
} OsDirInfo;

static OsDirInfo *os_dirinfo(OsFile *f) {
    void *d = burrow__atomic_load_acquire_ptr(&f->dirinfo);
    if (d != NULL)
        return (OsDirInfo *)d;
    OsDirInfo *n = (OsDirInfo *)mem_alloc(heap_allocator(), sizeof(OsDirInfo),
                                          _Alignof(OsDirInfo));
    if (n == NULL)
        return NULL;
    void *expected = NULL;
    if (burrow__atomic_cas_ptr(&f->dirinfo, &expected, n))
        return n;
    mem_free(heap_allocator(), n, sizeof(OsDirInfo), _Alignof(OsDirInfo));
    return (OsDirInfo *)expected;
}

/* Go drops the dirInfo on a seek and makes a new one on the next read. A
 * reader may be holding this one, so it is zeroed under its lock instead,
 * which reads the same afterwards. */
void burrow__os_dirinfo_reset(OsFile *f) {
    OsDirInfo *d = (OsDirInfo *)burrow__atomic_load_acquire_ptr(&f->dirinfo);
    if (d == NULL)
        return;
    sync_mutex_lock(&d->mu);
    memset(&d->d, 0, sizeof d->d);
    sync_mutex_unlock(&d->mu);
}

/* From destroy, once nobody holds a reference, so nobody is reading. */
void burrow__os_dirinfo_free(OsFile *f) {
    void *d = burrow__atomic_swap_ptr(&f->dirinfo, NULL);
    if (d != NULL)
        mem_free(heap_allocator(), d, sizeof(OsDirInfo), _Alignof(OsDirInfo));
}

/* ------------------------------------------------------------- unixDirent */

/* What Info stats is parent, a separator and name. parent is shared by the
 * entries of one read and name's bytes follow the struct. */
typedef struct OsDirent {
    Str parent;
    Str name;
    FsFileMode typ;
    FsFileInfo info; /* the lstat done to find the type, when one was */
} OsDirent;

static const Type os_dirent_desc = {
    {(const Byte *)"unixDirent", 10},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsDirent),
    (uint16_t)_Alignof(OsDirent),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736465U, /* "osde" */
    NULL,
};

/* parent, a separator and name as a path, on the stack when it fits. */
static bool os_join_cpath(OsCPath *c, Str parent, Str name, Error *err) {
    Int len = parent.len + 1 + name.len;
    if (len < OS_CPATH_SMALL) {
        c->p = c->small;
        c->cap = 0;
    } else {
        c->p = (char *)mem_alloc_nozero(heap_allocator(), (size_t)len + 1, 1);
        if (c->p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return false;
        }
        c->cap = len + 1;
    }
    if (parent.len > 0)
        memcpy(c->p, parent.p, (size_t)parent.len);
    c->p[parent.len] = (char)OS_PATH_SEPARATOR;
    if (name.len > 0)
        memcpy(c->p + parent.len + 1, name.p, (size_t)name.len);
    c->p[len] = 0;
    return true;
}

/* lstat(parent + "/" + name). */
static FsFileInfo os_lstat_in(Alloc *a, Str parent, Str name, Error *err) {
    OsCPath c;
    if (!os_join_cpath(&c, parent, name, err))
        return (FsFileInfo){NULL, NULL};
    FsFileInfo fi =
        os_lstat(a, str_from_bytes((const Byte *)c.p, parent.len + 1 + name.len), err);
    burrow__os_cpath_free(&c);
    return fi;
}

static Str os_de_name(void *self) {
    return ((OsDirent *)self)->name;
}

static bool os_de_is_dir(void *self) {
    return (((OsDirent *)self)->typ & FS_MODE_DIR) != 0;
}

static FsFileMode os_de_type(void *self) {
    return ((OsDirent *)self)->typ;
}

static FsFileInfo os_de_info(void *self, Alloc *a, Error *err) {
    OsDirent *d = (OsDirent *)self;
    if (d->info.vt != NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return d->info;
    }
    return os_lstat_in(a, d->parent, d->name, err);
}

static const FsDirEntryVT os_dirent_vt = {
    &os_dirent_desc, os_de_name, os_de_is_dir, os_de_type, os_de_info,
};

/* ---------------------------------------------------------------- readdir */

typedef enum OsReaddirMode {
    OS_READDIR_NAME,
    OS_READDIR_DIR_ENTRY,
    OS_READDIR_FILE_INFO
} OsReaddirMode;

static Str os_copy_str(Alloc *a, const char *p, Int n) {
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    if (n > 0)
        memcpy(b, p, (size_t)n);
    b[n] = 0;
    return str_from_bytes(b, n);
}

static Slice os_readdir(OsFile *f, Alloc *a, Int n, OsReaddirMode mode, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    const Type *elem = mode == OS_READDIR_NAME        ? TYPE_STRING
                       : mode == OS_READDIR_DIR_ENTRY ? TYPE_FS_DIR_ENTRY
                                                      : TYPE_FS_FILE_INFO;
    Slice out = slice_nil(elem);
    if (f == NULL) {
        BURROW_OUT(err, fs_err_invalid);
        return out;
    }
#if defined(BURROW_OS_WINDOWS)
    Str op = OS_LIT("readdir");
#else
    Str op = OS_LIT("readdirent");
#endif
    /* Go's ReadDirent takes a reference and fails with the closing error,
     * which nothing turns into os_err_closed on this path. */
    if (!burrow__os_incref(f)) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), op, f->name,
                                          burrow__os_err_file_closing));
        return out;
    }
    OsDirInfo *d = os_dirinfo(f);
    if (d == NULL) {
        burrow__os_decref(f);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    sync_mutex_lock(&d->mu);

    /* n <= 0 is everything, which below is any negative number, so that a
     * positive n can count down to zero and stop. */
    if (n == 0)
        n = -1;

    Str parent = {NULL, 0};
    Error e = BURROW_NO_ERROR;
    PalDirEntry ent;
    while (n != 0) {
        PalErrno pe = PAL_OK;
        d->d.fd = f->fd;
        if (!pal_readdir(&d->d, &ent, &pe)) {
            if (pe != PAL_OK)
                e = fs_path_error_new(error_allocator(), op, f->name,
                                      burrow__os_errno(pe));
            break;
        }
        Str name = str_from_bytes((const Byte *)ent.name, ent.name_len);
        if (mode == OS_READDIR_NAME) {
            Str s = os_copy_str(a, ent.name, ent.name_len);
            if (s.p == NULL) {
                e = burrow_err_out_of_memory;
                break;
            }
            out = slice_append(a, out, &s, 1);
            if (n > 0)
                n--;
            continue;
        }
        if (parent.p == NULL && f->name.len > 0) {
            parent = os_copy_str(a, (const char *)f->name.p, f->name.len);
            if (parent.p == NULL) {
                e = burrow_err_out_of_memory;
                break;
            }
        }
        if (mode == OS_READDIR_FILE_INFO) {
            Error le = BURROW_NO_ERROR;
            FsFileInfo info = os_lstat_in(a, parent, name, &le);
            /* The file went between the read and the stat. Go acts as if it
             * had never been there. */
            if (os_is_not_exist(le))
                continue;
            if (BURROW_FAILED(le)) {
                e = le;
                break;
            }
            out = slice_append(a, out, &info, 1);
            if (n > 0)
                n--;
            continue;
        }
        OsDirent *de = (OsDirent *)mem_alloc_nozero(
            a, sizeof(OsDirent) + (size_t)name.len, _Alignof(OsDirent));
        if (de == NULL) {
            e = burrow_err_out_of_memory;
            break;
        }
        Byte *p = (Byte *)(de + 1);
        if (name.len > 0)
            memcpy(p, name.p, (size_t)name.len);
        de->parent = parent;
        de->name = str_from_bytes(p, name.len);
        de->info = (FsFileInfo){NULL, NULL};
#if defined(BURROW_OS_WINDOWS)
        de->typ = burrow__os_mode_of(ent.type) & FS_MODE_TYPE;
#else
        if (ent.type != 0) {
            de->typ = burrow__os_mode_of(ent.type) & FS_MODE_TYPE;
        } else {
            /* The filesystem does not say, so Go asks with an lstat and keeps
             * what it found for Info. */
            Error le = BURROW_NO_ERROR;
            FsFileInfo info = os_lstat_in(a, parent, name, &le);
            if (os_is_not_exist(le))
                continue;
            if (BURROW_FAILED(le)) {
                e = le;
                break;
            }
            de->typ = info.vt->mode(info.data) & FS_MODE_TYPE;
            de->info = info;
        }
#endif
        FsDirEntry entry = {&os_dirent_vt, de};
        out = slice_append(a, out, &entry, 1);
        if (n > 0)
            n--;
    }

    sync_mutex_unlock(&d->mu);
    burrow__os_decref(f);

    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return out;
    }
    if (n > 0 && out.len == 0)
        BURROW_OUT(err, io_eof);
    return out;
}

Slice os_file_read_dir(OsFile *f, Alloc *a, Int n, Error *err) {
    return os_readdir(f, a, n, OS_READDIR_DIR_ENTRY, err);
}

Slice os_file_readdir(OsFile *f, Alloc *a, Int n, Error *err) {
    return os_readdir(f, a, n, OS_READDIR_FILE_INFO, err);
}

Slice os_file_readdirnames(OsFile *f, Alloc *a, Int n, Error *err) {
    return os_readdir(f, a, n, OS_READDIR_NAME, err);
}

static int os_entry_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsDirEntry *d1 = (const FsDirEntry *)x;
    const FsDirEntry *d2 = (const FsDirEntry *)y;
    return (int)strings_compare(d1->vt->name(d1->data), d2->vt->name(d2->data));
}

Slice os_read_dir(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsFile *f = os_open_file(heap_allocator(), name, (Int)PAL_O_DIRECTORY, 0, err);
    if (f == NULL)
        return slice_nil(TYPE_FS_DIR_ENTRY);
    Slice dirs = os_file_read_dir(f, a, -1, err);
    os_file_free(f);
    slices_sort_func(dirs, BURROW_FN(SlicesCmpFunc, os_entry_cmp, NULL));
    return dirs;
}

/* --------------------------------------------------------------- MkdirAll */

/* Whether name is there and a directory, 1, there and not one, 0, or not
 * there or not stat-able, -1. */
static int os_dir_kind(Str name, bool follow) {
    OsCPath c;
    if (!burrow__os_cpath(&c, name, NULL))
        return -1;
    PalStat st;
    bool ok = follow ? pal_stat(c.p, &st, NULL) : pal_lstat(c.p, &st, NULL);
    burrow__os_cpath_free(&c);
    if (!ok)
        return -1;
    return (st.mode & PAL_S_IFMT) == PAL_S_IFDIR ? 1 : 0;
}

/* The length of filepathlite.VolumeName(path), which is zero off Windows. */
static Int os_volume_len(Str path) {
#if defined(BURROW_OS_WINDOWS)
    Arena ar;
    arena_init(&ar, NULL, 0);
    Int n = filepath_volume_name(arena_allocator(&ar), path).len;
    arena_free(&ar);
    return n;
#else
    (void)path;
    return 0;
#endif
}

Error os_mkdir_all(Str path, OsFileMode perm) {
    /* If path is there, it is done or it is in the way. */
    int kind = os_dir_kind(path, true);
    if (kind == 1)
        return BURROW_NO_ERROR;
    if (kind == 0)
        return fs_path_error_new(error_allocator(), OS_LIT("mkdir"), path,
                                 burrow__os_errno_value(SYSCALL_ENOTDIR));

    /* The parent is path without any trailing separators and then without
     * its last element. */
    Int i = path.len - 1;
    while (i >= 0 && os_is_path_separator(path.p[i]))
        i--;
    while (i >= 0 && !os_is_path_separator(path.p[i]))
        i--;
    if (i < 0)
        i = 0;

    /* Make the parent first, unless it is only the volume name. */
    Str parent = str_from_bytes(path.p, i);
    if (parent.len > os_volume_len(path)) {
        Error e = os_mkdir_all(parent, perm);
        if (BURROW_FAILED(e))
            return e;
    }

    Error e = os_mkdir(path, perm);
    if (BURROW_FAILED(e)) {
        /* "foo/." fails to mkdir and is there all the same. */
        if (os_dir_kind(path, false) == 1)
            return BURROW_NO_ERROR;
        return e;
    }
    return BURROW_NO_ERROR;
}

/* -------------------------------------------------------------- RemoveAll */

static bool os_ends_with_dot(Str path) {
    if (path.len == 1 && path.p[0] == '.')
        return true;
    return path.len >= 2 && path.p[path.len - 1] == '.' &&
           os_is_path_separator(path.p[path.len - 2]);
}

static bool os_is_errno(Error e, SyscallErrno n) {
    const SyscallErrno *p = (const SyscallErrno *)errors_as(e, TYPE_SYSCALL_ERRNO);
    return p != NULL && *p == n;
}

Error os_remove_all(Str path) {
    /* An empty path has always been quietly fine. */
    if (path.len == 0)
        return BURROW_NO_ERROR;

    /* A trailing separator goes, so that "not_a_directory/" works. */
    while (path.len > 1 && os_is_path_separator(path.p[path.len - 1]))
        path.len--;

    if (os_ends_with_dot(path))
        return fs_path_error_new(error_allocator(), OS_LIT("RemoveAll"), path,
                                 burrow__os_errno_value(SYSCALL_EINVAL));

    /* The simple case is a file or an empty directory. */
    Error err = os_remove(path);
    if (BURROW_OK(err) || os_is_not_exist(err))
        return BURROW_NO_ERROR;

    /* Otherwise it has to be a directory to go into. */
    OsCPath c;
    Error ce = BURROW_NO_ERROR;
    if (!burrow__os_cpath(&c, path, &ce))
        return fs_path_error_new(error_allocator(), OS_LIT("lstat"), path, ce);
    PalStat st;
    PalErrno pe = PAL_OK;
    bool ok = pal_lstat(c.p, &st, &pe);
    burrow__os_cpath_free(&c);
    if (!ok) {
        Error serr = burrow__os_errno(pe);
        if (os_is_not_exist(serr) || os_is_errno(serr, SYSCALL_ENOTDIR))
            return BURROW_NO_ERROR;
        return fs_path_error_new(error_allocator(), OS_LIT("lstat"), path, serr);
    }
    if ((st.mode & PAL_S_IFMT) != PAL_S_IFDIR)
        return err;

    /* Remove what is inside and keep the first error. */
    enum { REQ_SIZE = 1024 };
    err = BURROW_NO_ERROR;
    for (;;) {
        Error oe = BURROW_NO_ERROR;
        OsFile *fd = os_open(heap_allocator(), path, &oe);
        if (fd == NULL) {
            /* Somebody else removed it first. */
            if (os_is_not_exist(oe))
                return BURROW_NO_ERROR;
            return oe;
        }

        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Slice names;
        Error read_err;
        for (;;) {
            Int num_err = 0;
            read_err = BURROW_NO_ERROR;
            names = os_file_readdirnames(fd, a, REQ_SIZE, &read_err);
            for (Int i = 0; i < names.len; i++) {
                Str name = *(const Str *)slice_at(names, i);
                Byte sep = (Byte)OS_PATH_SEPARATOR;
                Str child = burrow__os_cat3(a, path, str_from_bytes(&sep, 1), name);
                Error err1 =
                    child.p == NULL ? burrow_err_out_of_memory : os_remove_all(child);
                if (BURROW_OK(err))
                    err = err1;
                if (BURROW_FAILED(err1))
                    num_err++;
            }
            /* If anything here could go, start over. Otherwise these all
             * failed, so move on to the next batch. */
            if (num_err != REQ_SIZE)
                break;
        }
        Int got = names.len;
        arena_free(&ar);

        /* Removing entries may have shuffled the directory, and reading on
         * could skip some. Reopening is the only sure way, issue 20841. */
        os_file_free(fd);

        if (errors_is(read_err, io_eof))
            break;
        if (BURROW_OK(err))
            err = read_err;
        if (got == 0)
            break;

        /* Fewer than asked for means there are probably no more, so try the
         * directory itself before reading it again. */
        if (got < REQ_SIZE) {
            Error err1 = os_remove(path);
            if (BURROW_OK(err1) || os_is_not_exist(err1))
                return BURROW_NO_ERROR;
            if (BURROW_FAILED(err))
                return err;
        }
    }

    Error err1 = os_remove(path);
    if (BURROW_OK(err1) || os_is_not_exist(err1))
        return BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    /* A read only directory on Windows has to be made writable first. */
    if (os_is_permission(err1)) {
        OsCPath c2;
        PalStat st2;
        if (burrow__os_cpath(&c2, path, NULL)) {
            bool got_st = pal_stat(c2.p, &st2, NULL);
            burrow__os_cpath_free(&c2);
            if (got_st &&
                BURROW_OK(os_chmod(path, 0200 | burrow__os_mode_of(st2.mode))))
                err1 = os_remove(path);
        }
    }
#endif
    if (BURROW_OK(err))
        err = err1;
    return err;
}

/* ------------------------------------------------------------------ CopyFS */

typedef struct OsCopyFSEnv {
    Str dir;
    Fs fsys;
} OsCopyFSEnv;

/* A regular file from fsys copied to new_path, which must not exist yet. */
static Error os_copy_fs_file(Fs fsys, Str path, Str new_path) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    FsFile r = fsys.vt->open(fsys.data, a, path, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&ar);
        return e;
    }
    FsFileInfo info = r.vt->stat(r.data, a, &e);
    OsFile *w = NULL;
    if (BURROW_OK(e)) {
        OsFileMode perm = 0666 | (info.vt->mode(info.data) & 0777);
        w = os_open_file(a, new_path, OS_O_CREATE | OS_O_EXCL | OS_O_WRONLY, perm, &e);
    }
    if (BURROW_OK(e)) {
        Error ce = BURROW_NO_ERROR;
        io_copy(a, os_file_as_io_writer(w), fs_file_as_io_reader(r), &ce);
        if (BURROW_FAILED(ce)) {
            (void)os_file_close(w);
            e = fs_path_error_new(error_allocator(), BURROW_S("Copy"), new_path, ce);
        } else {
            e = os_file_close(w);
        }
        os_file_free(w);
    }
    (void)r.vt->read_closer.closer.close(r.data);
    arena_free(&ar);
    return e;
}

static Error os_copy_fs_visit(void *env, Str path, FsDirEntry d, Error err) {
    const OsCopyFSEnv *c = (const OsCopyFSEnv *)env;
    if (BURROW_FAILED(err))
        return err;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    Str fpath = filepath_localize(a, path, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&ar);
        return e;
    }
    /* joinPath, which leaves out the separator when dir ends in one. */
    Str new_path;
    if (c->dir.len > 0 && os_is_path_separator(c->dir.p[c->dir.len - 1])) {
        new_path = burrow__os_cat3(a, c->dir, fpath, (Str){NULL, 0});
    } else {
        Byte sep = (Byte)OS_PATH_SEPARATOR;
        new_path = burrow__os_cat3(a, c->dir, str_from_bytes(&sep, 1), fpath);
    }
    if (new_path.p == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    switch (d.vt->type(d.data)) {
    case FS_MODE_DIR:
        e = os_mkdir_all(new_path, 0777);
        break;
    case FS_MODE_SYMLINK: {
        Str target = fs_read_link(a, c->fsys, path, &e);
        if (BURROW_OK(e))
            e = os_symlink(target, new_path);
        break;
    }
    case 0:
        e = os_copy_fs_file(c->fsys, path, new_path);
        break;
    default:
        e = fs_path_error_new(error_allocator(), BURROW_S("CopyFS"), path,
                              fs_err_invalid);
        break;
    }
    arena_free(&ar);
    return e;
}

Error os_copy_fs(Str dir, Fs fsys) {
    OsCopyFSEnv env = {dir, fsys};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error e = fs_walk_dir(arena_allocator(&ar), fsys, BURROW_S("."),
                          (FsWalkDirFunc){os_copy_fs_visit, &env});
    arena_free(&ar);
    return e;
}
