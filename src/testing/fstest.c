/* testing/fstest's MapFS. See include/burrow/testing/fstest.h.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/fstest.h"

#include "burrow/declare.h"
#include "burrow/io.h"
#include "burrow/path.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define FSTEST_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* How many symbolic links a name may go through. Go has no limit and recurses
 * until the stack runs out on a loop. */
enum { FSTEST_MAX_LINKS = 255 };

#define FSTEST_TYPE(var, gonm, T, hash)                                                \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"testing/fstest", 14},                                          \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (hash),                                                                        \
        NULL,                                                                          \
    }

static FstestMapFile *fstest_get(FstestMapFS fsys, Str name) {
    FstestMapFile **f = (FstestMapFile **)map_get(fsys.files, &name);
    return f == NULL ? NULL : *f;
}

static bool fstest_is_symlink(const FstestMapFile *f) {
    return f != NULL && (f->mode & FS_MODE_TYPE) == FS_MODE_SYMLINK;
}

static bool fstest_has_prefix(Str s, Str prefix) {
    return s.len >= prefix.len &&
           (prefix.len == 0 || memcmp(s.p, prefix.p, (size_t)prefix.len) == 0);
}

static Str fstest_cat2(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str fstest_link_target(const FstestMapFile *f) {
    return str_from_bytes((const Byte *)f->data.p, f->data.len);
}

/* A synthesized directory, which is what a name with files under it and no
 * entry of its own is. */
static FstestMapFile *fstest_new_dir(Alloc *a) {
    FstestMapFile *f =
        (FstestMapFile *)mem_alloc(a, sizeof *f, _Alignof(FstestMapFile));
    if (f != NULL)
        f->mode = FS_MODE_DIR | 0555;
    return f;
}

FstestMapFS fstest_map_fs_make(Alloc *a) {
    FstestMapFS fsys = {map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0)};
    return fsys;
}

bool fstest_map_fs_set(FstestMapFS fsys, Str name, FstestMapFile *f) {
    return map_set(fsys.files, &name, &f);
}

/* ------------------------------------------------------------- mapFileInfo */

typedef struct FstestInfo {
    Str name;
    const FstestMapFile *f;
} FstestInfo;

FSTEST_TYPE(fstest_info_desc, "mapFileInfo", FstestInfo, 0x66746d69U);

static Str fstest_info_name(void *self) {
    return path_base(((FstestInfo *)self)->name);
}

static int64_t fstest_info_size(void *self) {
    return (int64_t)((FstestInfo *)self)->f->data.len;
}

static FsFileMode fstest_info_mode(void *self) {
    return ((FstestInfo *)self)->f->mode;
}

static Time fstest_info_mod_time(void *self) {
    return ((FstestInfo *)self)->f->mod_time;
}

static bool fstest_info_is_dir(void *self) {
    return (((FstestInfo *)self)->f->mode & FS_MODE_DIR) != 0;
}

static Any fstest_info_sys(void *self) {
    return ((FstestInfo *)self)->f->sys;
}

static FsFileMode fstest_info_type(void *self) {
    return ((FstestInfo *)self)->f->mode & FS_MODE_TYPE;
}

static const FsFileInfoVT fstest_info_vt = {
    &fstest_info_desc,    fstest_info_name,   fstest_info_size, fstest_info_mode,
    fstest_info_mod_time, fstest_info_is_dir, fstest_info_sys,
};

static FsFileInfo fstest_info_info(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&fstest_info_vt, self};
}

static const FsDirEntryVT fstest_entry_vt = {
    &fstest_info_desc, fstest_info_name, fstest_info_is_dir,
    fstest_info_type,  fstest_info_info,
};

static FstestInfo *fstest_new_info(Alloc *a, Str name, const FstestMapFile *f) {
    FstestInfo *i = (FstestInfo *)mem_alloc_nozero(a, sizeof *i, _Alignof(FstestInfo));
    if (i != NULL) {
        i->name = name;
        i->f = f;
    }
    return i;
}

/* ------------------------------------------------------------- openMapFile */

typedef struct FstestOpenFile {
    FstestInfo info; /* first, so the file's stat can hand back its address */
    Str path;
    int64_t offset;
} FstestOpenFile;

static int64_t fstest_open_seek(FstestOpenFile *f, int64_t offset, Int whence,
                                Error *err);
static Int fstest_open_read_at(FstestOpenFile *f, Slice b, int64_t offset, Error *err);

/* Seek and ReadAt are only reached through the method set, the way Go's
 * io.Seeker and io.ReaderAt assertions find them, and testing/iotest is what
 * asks. */
#define FSTEST_OPEN_METHODS(M, T)                                                      \
    M(T, ReadAt, fstest_open_read_at, IO_SIG_READ_AT)                                  \
    M(T, Seek, fstest_open_seek, IO_SIG_SEEK)
BURROW_METHODS_DEFINE(FstestOpenFile, FSTEST_OPEN_METHODS);

static const Type fstest_open_desc = {
    {(const Byte *)"openMapFile", 11},
    {(const Byte *)"testing/fstest", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(FstestOpenFile),
    (uint16_t)_Alignof(FstestOpenFile),
    0,
    (uint16_t)(sizeof burrow__methods_FstestOpenFile /
               sizeof burrow__methods_FstestOpenFile[0]),
    NULL,
    burrow__methods_FstestOpenFile,
    NULL,
    NULL,
    0,
    0x66746f66U,
    NULL,
};

static Int fstest_open_read(void *self, Slice b, Error *err) {
    FstestOpenFile *f = (FstestOpenFile *)self;
    Slice data = f->info.f->data;
    if (f->offset >= (int64_t)data.len) {
        *err = io_eof;
        return 0;
    }
    Int n = data.len - (Int)f->offset;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, (const Byte *)data.p + f->offset, (size_t)n);
    f->offset += n;
    *err = BURROW_NO_ERROR;
    return n;
}

static int64_t fstest_open_seek(FstestOpenFile *f, int64_t offset, Int whence,
                                Error *err) {
    int64_t size = (int64_t)f->info.f->data.len;
    switch (whence) {
    case 0:
        break;
    case 1:
        offset += f->offset;
        break;
    case 2:
        offset += size;
        break;
    default:
        break;
    }
    if (offset < 0 || offset > size) {
        *err = fs_path_error_new(error_allocator(), FSTEST_LIT("seek"), f->path,
                                 fs_err_invalid);
        return 0;
    }
    f->offset = offset;
    *err = BURROW_NO_ERROR;
    return offset;
}

static Int fstest_open_read_at(FstestOpenFile *f, Slice b, int64_t offset, Error *err) {
    Slice data = f->info.f->data;
    if (offset < 0 || offset > (int64_t)data.len) {
        *err = fs_path_error_new(error_allocator(), FSTEST_LIT("read"), f->path,
                                 fs_err_invalid);
        return 0;
    }
    Int n = data.len - (Int)offset;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, (const Byte *)data.p + offset, (size_t)n);
    *err = n < b.len ? io_eof : BURROW_NO_ERROR;
    return n;
}

static Error fstest_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static FsFileInfo fstest_open_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&fstest_info_vt, &((FstestOpenFile *)self)->info};
}

static const FsFileVT fstest_open_vt = {
    {{&fstest_open_desc, fstest_open_read}, {&fstest_open_desc, fstest_close}},
    fstest_open_stat,
    NULL,
};

/* ------------------------------------------------------------------ mapDir */

typedef struct FstestDir {
    FstestInfo info;
    Str path;
    FstestInfo *entry;
    Int n;
    Int offset;
    Error read_err; /* what a read gives, made at open, where there is an Alloc */
} FstestDir;

FSTEST_TYPE(fstest_dir_desc, "mapDir", FstestDir, 0x66746464U);

static Int fstest_dir_read(void *self, Slice b, Error *err) {
    (void)b;
    *err = ((FstestDir *)self)->read_err;
    return 0;
}

static FsFileInfo fstest_dir_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&fstest_info_vt, &((FstestDir *)self)->info};
}

static Slice fstest_dir_read_dir(void *self, Alloc *a, Int count, Error *err) {
    FstestDir *d = (FstestDir *)self;
    Int n = d->n - d->offset;
    *err = BURROW_NO_ERROR;
    if (n == 0 && count > 0) {
        *err = io_eof;
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    if (count > 0 && n > count)
        n = count;
    Slice list = slice_make(a, TYPE_FS_DIR_ENTRY, n, n);
    if (n > 0 && list.p == NULL) {
        *err = burrow_err_out_of_memory;
        return list;
    }
    FsDirEntry *e = (FsDirEntry *)list.p;
    for (Int i = 0; i < n; i++)
        e[i] = (FsDirEntry){&fstest_entry_vt, &d->entry[d->offset + i]};
    d->offset += n;
    return list;
}

static const FsFileVT fstest_dir_vt = {
    {{&fstest_dir_desc, fstest_dir_read}, {&fstest_dir_desc, fstest_close}},
    fstest_dir_stat,
    fstest_dir_read_dir,
};

/* ------------------------------------------------------------------- Open */

/* The name with every symbolic link in it followed, and whether that name is
 * valid. */
static bool fstest_resolve(FstestMapFS fsys, Alloc *a, Str name, int depth, Str *out) {
    if (depth > FSTEST_MAX_LINKS)
        return false;
    FstestMapFile *file = fstest_get(fsys, name);
    if (fstest_is_symlink(file)) {
        Str target = fstest_link_target(file);
        if (path_is_abs(target))
            return false;
        return fstest_resolve(fsys, a, path_join_v(a, 2, path_dir(a, name), target),
                              depth + 1, out);
    }

    /* Check if a leading directory is a link. */
    for (Int i = 0; i < name.len;) {
        Int j =
            strings_index(str_from_bytes(name.p + i, name.len - i), FSTEST_LIT("/"));
        Str dir;
        if (j < 0) {
            dir = name;
            i = name.len;
        } else {
            dir = str_from_bytes(name.p, i + j);
            i += j;
        }
        FstestMapFile *f = fstest_get(fsys, dir);
        if (fstest_is_symlink(f)) {
            Str target = fstest_link_target(f);
            if (path_is_abs(target))
                return false;
            Str joined = path_join_v(a, 2, path_dir(a, dir), target);
            return fstest_resolve(
                fsys, a,
                fstest_cat2(a, joined, str_from_bytes(name.p + i, name.len - i)),
                depth + 1, out);
        }
        i += 1;
    }
    *out = name;
    return fs_valid_path(name);
}

static int fstest_info_cmp(void *env, const void *x, const void *y) {
    (void)env;
    return (int)strings_compare(((const FstestInfo *)x)->name,
                                ((const FstestInfo *)y)->name);
}

static FsFile fstest_not_exist(Alloc *a, Str op, Str name, Error *err) {
    *err = fs_path_error_new(a, op, name, fs_err_not_exist);
    return (FsFile){NULL, NULL};
}

FsFile fstest_map_fs_open(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    FsFile none = {NULL, NULL};
    Str open = FSTEST_LIT("open");
    Str real_name;
    if (!fs_valid_path(name) || !fstest_resolve(fsys, a, name, 0, &real_name))
        return fstest_not_exist(a, open, name, err);

    FstestMapFile *file = fstest_get(fsys, real_name);
    if (file != NULL && (file->mode & FS_MODE_DIR) == 0) {
        FstestOpenFile *f =
            (FstestOpenFile *)mem_alloc(a, sizeof *f, _Alignof(FstestOpenFile));
        if (f == NULL) {
            *err = burrow_err_out_of_memory;
            return none;
        }
        f->path = name;
        f->info.name = path_base(name);
        f->info.f = file;
        *err = BURROW_NO_ERROR;
        return (FsFile){&fstest_open_vt, f};
    }

    /* Directory, possibly synthesized. The list holds the entries that are in
     * the map, and need the directories under this one that only exist
     * because something inside them does. */
    Slice list = slice_nil(TYPE_UNSAFE_POINTER);
    Map *need = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    if (need == NULL) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    bool dot = real_name.len == 1 && real_name.p[0] == '.';
    Str prefix = dot ? BURROW_STR_EMPTY : fstest_cat2(a, real_name, FSTEST_LIT("/"));
    const void *k;
    void *v;
    for (MapIter it = map_iter(fsys.files); map_next(&it, &k, &v);) {
        Str fname = *(const Str *)k;
        FstestMapFile *f = *(FstestMapFile **)v;
        if (!dot && !fstest_has_prefix(fname, prefix))
            continue;
        Str felem = str_from_bytes(fname.p + prefix.len, fname.len - prefix.len);
        Int i = strings_index(felem, FSTEST_LIT("/"));
        if (i < 0) {
            if (dot && felem.len == 1 && felem.p[0] == '.')
                continue;
            FstestInfo *info = fstest_new_info(a, felem, f);
            if (info == NULL) {
                *err = burrow_err_out_of_memory;
                return none;
            }
            list = slice_append(a, list, &info, 1);
        } else {
            Str sub = str_from_bytes(felem.p, i);
            if (!map_set(need, &sub, NULL)) {
                *err = burrow_err_out_of_memory;
                return none;
            }
        }
    }
    /* Go checks this for every name but "." only, since "." always exists. */
    if (!dot && file == NULL && list.len == 0 && map_len(need) == 0)
        return fstest_not_exist(a, open, name, err);

    Int n = list.len;
    FstestInfo **infos = (FstestInfo **)list.p;
    for (Int i = 0; i < n; i++)
        map_del(need, &infos[i]->name);
    Int total = n + map_len(need);
    FstestDir *d = (FstestDir *)mem_alloc(a, sizeof *d, _Alignof(FstestDir));
    FstestInfo *entry = (FstestInfo *)mem_alloc_array(
        a, (size_t)total + 1, sizeof *entry, _Alignof(FstestInfo));
    if (d == NULL || entry == NULL) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    for (Int i = 0; i < n; i++)
        entry[i] = *infos[i];
    Int m = n;
    for (MapIter it = map_iter(need); map_next(&it, &k, NULL);) {
        FstestMapFile *f = fstest_new_dir(a);
        if (f == NULL) {
            *err = burrow_err_out_of_memory;
            return none;
        }
        entry[m].name = *(const Str *)k;
        entry[m].f = f;
        m++;
    }
    slices_sort_func(slice_from(entry, total, total, &fstest_info_desc),
                     BURROW_FN(SlicesCmpFunc, fstest_info_cmp, NULL));

    if (file == NULL) {
        file = fstest_new_dir(a);
        if (file == NULL) {
            *err = burrow_err_out_of_memory;
            return none;
        }
    }
    Str elem;
    if (name.len == 1 && name.p[0] == '.') {
        elem = FSTEST_LIT(".");
    } else {
        Int slash = strings_last_index(name, FSTEST_LIT("/"));
        elem = str_from_bytes(name.p + slash + 1, name.len - slash - 1);
    }
    d->path = name;
    d->info.name = elem;
    d->info.f = file;
    d->entry = entry;
    d->n = total;
    d->offset = 0;
    d->read_err = fs_path_error_new(a, FSTEST_LIT("read"), name, fs_err_invalid);
    *err = BURROW_NO_ERROR;
    return (FsFile){&fstest_dir_vt, d};
}

/* ------------------------------------------------------------ the rest */

/* MapFS.lstat: the file itself when name is a link, with the links in the
 * directories above it followed. */
static FstestInfo *fstest_lstat(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    *err = fs_err_not_exist;
    if (!fs_valid_path(name))
        return NULL;
    Str real_dir;
    if (!fstest_resolve(fsys, a, path_dir(a, name), 0, &real_dir))
        return NULL;
    Str elem = path_base(name);
    Str real_name = path_join_v(a, 2, real_dir, elem);

    FstestMapFile *file = fstest_get(fsys, real_name);
    if (file == NULL) {
        bool found = real_name.len == 1 && real_name.p[0] == '.';
        Str prefix = fstest_cat2(a, real_name, FSTEST_LIT("/"));
        const void *k;
        for (MapIter it = map_iter(fsys.files); !found && map_next(&it, &k, NULL);)
            found = fstest_has_prefix(*(const Str *)k, prefix);
        if (!found)
            return NULL;
        file = fstest_new_dir(a);
        if (file == NULL) {
            *err = burrow_err_out_of_memory;
            return NULL;
        }
    }
    FstestInfo *info = fstest_new_info(a, elem, file);
    *err = info == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR;
    return info;
}

Str fstest_map_fs_read_link(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    FstestInfo *info = fstest_lstat(fsys, a, name, err);
    if (info == NULL) {
        *err = fs_path_error_new(a, FSTEST_LIT("readlink"), name, *err);
        return BURROW_STR_EMPTY;
    }
    if (!fstest_is_symlink(info->f)) {
        *err = fs_path_error_new(a, FSTEST_LIT("readlink"), name, fs_err_invalid);
        return BURROW_STR_EMPTY;
    }
    return fstest_link_target(info->f);
}

FsFileInfo fstest_map_fs_lstat(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    FstestInfo *info = fstest_lstat(fsys, a, name, err);
    if (info == NULL) {
        *err = fs_path_error_new(a, FSTEST_LIT("lstat"), name, *err);
        return (FsFileInfo){NULL, NULL};
    }
    return (FsFileInfo){&fstest_info_vt, info};
}

/* The slots take the map itself as their data, which is all a MapFS is. */
static FstestMapFS fstest_self(void *self) {
    FstestMapFS fsys = {(Map *)self};
    return fsys;
}

static FsFile fstest_open_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_open(fstest_self(self), a, name, err);
}

static Slice fstest_read_dir_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_read_dir(fstest_self(self), a, name, err);
}

static Slice fstest_read_file_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_read_file(fstest_self(self), a, name, err);
}

static FsFileInfo fstest_stat_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_stat(fstest_self(self), a, name, err);
}

static Fs fstest_sub_slot(void *self, Alloc *a, Str dir, Error *err) {
    return fstest_map_fs_sub(fstest_self(self), a, dir, err);
}

static Slice fstest_glob_slot(void *self, Alloc *a, Str pattern, Error *err) {
    return fstest_map_fs_glob(fstest_self(self), a, pattern, err);
}

static Str fstest_read_link_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_read_link(fstest_self(self), a, name, err);
}

static FsFileInfo fstest_lstat_slot(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_lstat(fstest_self(self), a, name, err);
}

FSTEST_TYPE(fstest_map_fs_desc, "MapFS", FstestMapFS, 0x66746d66U);
FSTEST_TYPE(fstest_fs_only_desc, "fsOnly", FstestMapFS, 0x66746f6eU);
FSTEST_TYPE(fstest_no_sub_desc, "noSub", FstestMapFS, 0x66746e73U);

static const FsVT fstest_map_fs_vt = {
    &fstest_map_fs_desc,   fstest_open_slot,      fstest_read_dir_slot,
    fstest_read_file_slot, fstest_stat_slot,      fstest_sub_slot,
    fstest_glob_slot,      fstest_read_link_slot, fstest_lstat_slot,
};

/* fsOnly: the MapFS with only Open, which is how its ReadFile, Stat, ReadDir
 * and Glob get fs's versions of themselves without calling themselves. */
static const FsVT fstest_fs_only_vt = {
    &fstest_fs_only_desc, fstest_open_slot, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

/* noSub: the MapFS with everything but Sub, so that fs_sub wraps it. */
static const FsVT fstest_no_sub_vt = {
    &fstest_no_sub_desc,   fstest_open_slot,      fstest_read_dir_slot,
    fstest_read_file_slot, fstest_stat_slot,      NULL,
    fstest_glob_slot,      fstest_read_link_slot, fstest_lstat_slot,
};

Fs fstest_map_fs_as_fs(FstestMapFS fsys) {
    return (Fs){&fstest_map_fs_vt, fsys.files};
}

static Fs fstest_fs_only(FstestMapFS fsys) {
    return (Fs){&fstest_fs_only_vt, fsys.files};
}

Slice fstest_map_fs_read_file(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    return fs_read_file(a, fstest_fs_only(fsys), name, err);
}

FsFileInfo fstest_map_fs_stat(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    return fs_stat(a, fstest_fs_only(fsys), name, err);
}

Slice fstest_map_fs_read_dir(FstestMapFS fsys, Alloc *a, Str name, Error *err) {
    return fs_read_dir(a, fstest_fs_only(fsys), name, err);
}

Slice fstest_map_fs_glob(FstestMapFS fsys, Alloc *a, Str pattern, Error *err) {
    return fs_glob(a, fstest_fs_only(fsys), pattern, err);
}

Fs fstest_map_fs_sub(FstestMapFS fsys, Alloc *a, Str dir, Error *err) {
    return fs_sub(a, (Fs){&fstest_no_sub_vt, fsys.files}, dir, err);
}
