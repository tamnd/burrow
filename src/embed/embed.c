/* embed, derived from Go's src/embed/embed.go (go1.27.1). See
 * include/burrow/embed.h.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/embed.h"

#include "burrow/declare.h"
#include "burrow/io.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define EMBED_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

#define EMBED_TYPE(var, gonm, T, hash, nmethods, methods)                              \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"embed", 5},                                                    \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        (nmethods),                                                                    \
        NULL,                                                                          \
        (methods),                                                                     \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (hash),                                                                        \
        NULL,                                                                          \
    }

/* ------------------------------------------------------------------- names */

/* split: the directory a name is in, its last element, and whether it is a
 * directory, which in the list is a name ending in a slash. */
static Str embed_split(Str name, Str *elem, bool *is_dir) {
    *is_dir = name.len > 0 && name.p[name.len - 1] == '/';
    if (*is_dir)
        name.len--;
    Int i = name.len - 1;
    while (i >= 0 && name.p[i] != '/')
        i--;
    if (i < 0) {
        *elem = name;
        return EMBED_LIT(".");
    }
    *elem = (Str){name.p + i + 1, name.len - i - 1};
    return (Str){name.p, i};
}

static Str embed_dir_of(Str name) {
    Str elem;
    bool is_dir;
    return embed_split(name, &elem, &is_dir);
}

/* Go's string comparison: bytes, then length. */
static int embed_cmp(Str x, Str y) {
    Int n = x.len < y.len ? x.len : y.len;
    int c = n > 0 ? memcmp(x.p, y.p, (size_t)n) : 0;
    if (c != 0)
        return c;
    return (x.len > y.len) - (x.len < y.len);
}

static bool embed_eq(Str x, Str y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

/* ------------------------------------------------------------------- file */

EMBED_TYPE(embed_file_desc, "file", EmbedFile, 0x656d6669U, 0, NULL);

static Str embed_file_name(void *self) {
    Str elem;
    bool is_dir;
    embed_split(((const EmbedFile *)self)->name, &elem, &is_dir);
    return elem;
}

static int64_t embed_file_size(void *self) {
    return (int64_t)((const EmbedFile *)self)->data.len;
}

static bool embed_file_is_dir(void *self) {
    Str elem;
    bool is_dir;
    embed_split(((const EmbedFile *)self)->name, &elem, &is_dir);
    return is_dir;
}

static FsFileMode embed_file_mode(void *self) {
    if (embed_file_is_dir(self))
        return FS_MODE_DIR | 0555;
    return 0444;
}

static Time embed_file_mod_time(void *self) {
    (void)self;
    return (Time){0};
}

static Any embed_file_sys(void *self) {
    (void)self;
    return (Any){0};
}

static FsFileMode embed_file_type(void *self) {
    return embed_file_mode(self) & FS_MODE_TYPE;
}

static const FsFileInfoVT embed_info_vt = {
    &embed_file_desc,    embed_file_name,   embed_file_size, embed_file_mode,
    embed_file_mod_time, embed_file_is_dir, embed_file_sys,
};

static FsFileInfo embed_file_info(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&embed_info_vt, self};
}

static const FsDirEntryVT embed_entry_vt = {
    &embed_file_desc, embed_file_name, embed_file_is_dir,
    embed_file_type,  embed_file_info,
};

/* The const the FS's files are declared with, taken off to fit in the void *
 * an interface holds. Nothing writes through it. */
static void *embed_unconst(const void *p) {
    return (void *)(uintptr_t)p;
}

static const EmbedFile embed_dot_file = {{(const Byte *)"./", 2}, {NULL, 0}, {0}};

/* ------------------------------------------------------------------ lookup */

static const EmbedFile *embed_lookup(const EmbedFS *f, Str name) {
    if (!fs_valid_path(name))
        return NULL;
    if (embed_eq(name, EMBED_LIT(".")))
        return &embed_dot_file;
    if (f->files == NULL)
        return NULL;

    Str elem;
    bool is_dir;
    Str dir = embed_split(name, &elem, &is_dir);
    Int i = 0, j = f->n;
    while (i < j) {
        Int h = (Int)((uint64_t)(i + j) >> 1);
        Str ielem;
        Str idir = embed_split(f->files[h].name, &ielem, &is_dir);
        int c = embed_cmp(idir, dir);
        if (!(c > 0 || (c == 0 && embed_cmp(ielem, elem) >= 0)))
            i = h + 1;
        else
            j = h;
    }
    if (i < f->n) {
        Str got = f->files[i].name;
        if (got.len > 0 && got.p[got.len - 1] == '/')
            got.len--;
        if (embed_eq(got, name))
            return &f->files[i];
    }
    return NULL;
}

/* The first index whose directory is at least dir, or past it when after is
 * set. */
static Int embed_search_dir(const EmbedFS *f, Str dir, bool after) {
    Int i = 0, j = f->n;
    while (i < j) {
        Int h = (Int)((uint64_t)(i + j) >> 1);
        int c = embed_cmp(embed_dir_of(f->files[h].name), dir);
        if (!(after ? c > 0 : c >= 0))
            i = h + 1;
        else
            j = h;
    }
    return i;
}

static const EmbedFile *embed_read_dir_files(const EmbedFS *f, Str dir, Int *n) {
    *n = 0;
    if (f->files == NULL)
        return NULL;
    Int i = embed_search_dir(f, dir, false);
    Int j = embed_search_dir(f, dir, true);
    *n = j - i;
    return f->files + i;
}

/* ---------------------------------------------------------------- openFile */

typedef struct EmbedOpenFile {
    const EmbedFile *f;
    int64_t offset;
} EmbedOpenFile;

static int64_t embed_open_seek(EmbedOpenFile *f, int64_t offset, Int whence,
                               Error *err);
static Int embed_open_read_at(EmbedOpenFile *f, Slice b, int64_t offset, Error *err);

/* Seek and ReadAt are only reached through the method set, the way Go's
 * io.Seeker and io.ReaderAt assertions find them. */
#define EMBED_OPEN_METHODS(M, T)                                                       \
    M(T, ReadAt, embed_open_read_at, IO_SIG_READ_AT)                                   \
    M(T, Seek, embed_open_seek, IO_SIG_SEEK)
BURROW_METHODS_DEFINE(EmbedOpenFile, EMBED_OPEN_METHODS);

EMBED_TYPE(embed_open_desc, "openFile", EmbedOpenFile, 0x656d6f66U,
           (uint16_t)(sizeof burrow__methods_EmbedOpenFile /
                      sizeof burrow__methods_EmbedOpenFile[0]),
           burrow__methods_EmbedOpenFile);

static Int embed_open_read(void *self, Slice b, Error *err) {
    EmbedOpenFile *f = (EmbedOpenFile *)self;
    Str data = f->f->data;
    if (f->offset >= (int64_t)data.len) {
        *err = io_eof;
        return 0;
    }
    if (f->offset < 0) {
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("read"), f->f->name,
                                 fs_err_invalid);
        return 0;
    }
    Int n = data.len - (Int)f->offset;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, data.p + f->offset, (size_t)n);
    f->offset += n;
    *err = BURROW_NO_ERROR;
    return n;
}

static int64_t embed_open_seek(EmbedOpenFile *f, int64_t offset, Int whence,
                               Error *err) {
    int64_t size = (int64_t)f->f->data.len;
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
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("seek"), f->f->name,
                                 fs_err_invalid);
        return 0;
    }
    f->offset = offset;
    *err = BURROW_NO_ERROR;
    return offset;
}

static Int embed_open_read_at(EmbedOpenFile *f, Slice b, int64_t offset, Error *err) {
    Str data = f->f->data;
    if (offset < 0 || offset > (int64_t)data.len) {
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("read"), f->f->name,
                                 fs_err_invalid);
        return 0;
    }
    Int n = data.len - (Int)offset;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, data.p + offset, (size_t)n);
    *err = n < b.len ? io_eof : BURROW_NO_ERROR;
    return n;
}

static Error embed_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static FsFileInfo embed_open_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&embed_info_vt, embed_unconst(((EmbedOpenFile *)self)->f)};
}

static const FsFileVT embed_open_vt = {
    {{&embed_open_desc, embed_open_read}, {&embed_open_desc, embed_close}},
    embed_open_stat,
    NULL,
};

/* ----------------------------------------------------------------- openDir */

typedef struct EmbedOpenDir {
    const EmbedFile *f;
    const EmbedFile *files;
    Int n;
    Int offset;
} EmbedOpenDir;

EMBED_TYPE(embed_dir_desc, "openDir", EmbedOpenDir, 0x656d6f64U, 0, NULL);

static Error embed_is_a_directory(Str name) {
    Alloc *a = error_allocator();
    return fs_path_error_new(a, EMBED_LIT("read"), name,
                             errors_new(a, EMBED_LIT("is a directory")));
}

static Int embed_dir_read(void *self, Slice b, Error *err) {
    (void)b;
    *err = embed_is_a_directory(((EmbedOpenDir *)self)->f->name);
    return 0;
}

static FsFileInfo embed_dir_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&embed_info_vt, embed_unconst(((EmbedOpenDir *)self)->f)};
}

static Slice embed_entries(Alloc *a, const EmbedFile *files, Int n, Error *err) {
    Slice list = slice_make(a, TYPE_FS_DIR_ENTRY, n, n);
    if (n > 0 && list.p == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    FsDirEntry *e = (FsDirEntry *)list.p;
    for (Int i = 0; i < n; i++)
        e[i] = (FsDirEntry){&embed_entry_vt, embed_unconst(&files[i])};
    return list;
}

static Slice embed_dir_read_dir(void *self, Alloc *a, Int count, Error *err) {
    EmbedOpenDir *d = (EmbedOpenDir *)self;
    Int n = d->n - d->offset;
    *err = BURROW_NO_ERROR;
    if (n == 0) {
        if (count > 0)
            *err = io_eof;
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    if (count > 0 && n > count)
        n = count;
    Slice list = embed_entries(a, d->files + d->offset, n, err);
    if (BURROW_FAILED(*err))
        return list;
    d->offset += n;
    return list;
}

static const FsFileVT embed_dir_vt = {
    {{&embed_dir_desc, embed_dir_read}, {&embed_dir_desc, embed_close}},
    embed_dir_stat,
    embed_dir_read_dir,
};

/* ---------------------------------------------------------------------- FS */

FsFile embed_fs_open(const EmbedFS *fsys, Alloc *a, Str name, Error *err) {
    const EmbedFile *file = embed_lookup(fsys, name);
    if (file == NULL) {
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("open"), name,
                                 fs_err_not_exist);
        return (FsFile){0};
    }
    if (embed_file_is_dir(embed_unconst(file))) {
        EmbedOpenDir *d =
            (EmbedOpenDir *)mem_alloc(a, sizeof *d, _Alignof(EmbedOpenDir));
        if (d == NULL) {
            *err = burrow_err_out_of_memory;
            return (FsFile){0};
        }
        d->f = file;
        d->files = embed_read_dir_files(fsys, name, &d->n);
        *err = BURROW_NO_ERROR;
        return (FsFile){&embed_dir_vt, d};
    }
    EmbedOpenFile *f =
        (EmbedOpenFile *)mem_alloc(a, sizeof *f, _Alignof(EmbedOpenFile));
    if (f == NULL) {
        *err = burrow_err_out_of_memory;
        return (FsFile){0};
    }
    f->f = file;
    *err = BURROW_NO_ERROR;
    return (FsFile){&embed_open_vt, f};
}

Slice embed_fs_read_dir(const EmbedFS *fsys, Alloc *a, Str name, Error *err) {
    const EmbedFile *file = embed_lookup(fsys, name);
    if (file == NULL) {
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("open"), name,
                                 fs_err_not_exist);
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    if (!embed_file_is_dir(embed_unconst(file))) {
        Alloc *ea = error_allocator();
        *err = fs_path_error_new(ea, EMBED_LIT("read"), name,
                                 errors_new(ea, EMBED_LIT("not a directory")));
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    Int n;
    const EmbedFile *files = embed_read_dir_files(fsys, name, &n);
    *err = BURROW_NO_ERROR;
    return embed_entries(a, files, n, err);
}

Slice embed_fs_read_file(const EmbedFS *fsys, Alloc *a, Str name, Error *err) {
    const EmbedFile *file = embed_lookup(fsys, name);
    if (file == NULL) {
        *err = fs_path_error_new(error_allocator(), EMBED_LIT("open"), name,
                                 fs_err_not_exist);
        return slice_nil(TYPE_BYTE);
    }
    if (embed_file_is_dir(embed_unconst(file))) {
        *err = embed_is_a_directory(name);
        return slice_nil(TYPE_BYTE);
    }
    Int n = file->data.len;
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    if (b.p == NULL && n > 0) {
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_BYTE);
    }
    if (n > 0)
        memcpy(b.p, file->data.p, (size_t)n);
    *err = BURROW_NO_ERROR;
    return b;
}

static FsFile embed_open_slot(void *self, Alloc *a, Str name, Error *err) {
    return embed_fs_open((const EmbedFS *)self, a, name, err);
}

static Slice embed_read_dir_slot(void *self, Alloc *a, Str name, Error *err) {
    return embed_fs_read_dir((const EmbedFS *)self, a, name, err);
}

static Slice embed_read_file_slot(void *self, Alloc *a, Str name, Error *err) {
    return embed_fs_read_file((const EmbedFS *)self, a, name, err);
}

EMBED_TYPE(embed_fs_desc, "FS", EmbedFS, 0x656d6673U, 0, NULL);

static const FsVT embed_fs_vt = {
    &embed_fs_desc,
    embed_open_slot,
    embed_read_dir_slot,
    embed_read_file_slot,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

Fs embed_fs_as_fs(const EmbedFS *fsys) {
    return (Fs){&embed_fs_vt, embed_unconst(fsys)};
}
