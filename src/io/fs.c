/* io/fs. See include/burrow/io/fs.h.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io/fs.h"

#include "burrow/path.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

#define FS_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* err == target in Go, for the sentinels that are compared and not matched
 * with errors.Is: SkipDir, SkipAll and io.EOF. */
static bool fs_same(Error e, Error target) {
    return e.vt == target.vt && e.data == target.data;
}

static Byte *fs_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* The parts one after another, in one allocation from a. */
static Str fs_cat(Alloc *a, const Str *parts, int n) {
    Int len = 0;
    for (int i = 0; i < n; i++)
        len += parts[i].len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)len + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = p;
    for (int i = 0; i < n; i++)
        q = fs_put(q, parts[i]);
    return str_from_bytes(p, len);
}

/* ------------------------------------------------------------------ errors */

BURROW_SENTINEL_ERROR(fs_err_invalid, "invalid argument");
BURROW_SENTINEL_ERROR(fs_err_permission, "permission denied");
BURROW_SENTINEL_ERROR(fs_err_exist, "file already exists");
BURROW_SENTINEL_ERROR(fs_err_not_exist, "file does not exist");
BURROW_SENTINEL_ERROR(fs_err_closed, "file already closed");
BURROW_SENTINEL_ERROR(fs_skip_dir, "skip this directory");
BURROW_SENTINEL_ERROR(fs_skip_all, "skip everything and stop the walk");

static const Str fs_err_not_implemented__text = {(const Byte *)"not implemented", 15};
static const Error fs_err_not_implemented = {&burrow_sentinel_error_vt,
                                             &fs_err_not_implemented__text};

/* The FsPathError first, so errors_as hands back a pointer to it, and the
 * message after, built once because the message slot cannot allocate. */
typedef struct FsPathErrorBox {
    FsPathError e;
    Str message;
} FsPathErrorBox;

static Str fs_path_error_message(const void *self) {
    return ((const FsPathErrorBox *)self)->message;
}

static Error fs_path_error_unwrap_slot(const void *self) {
    return ((const FsPathError *)self)->err;
}

static const Type fs_path_error_desc = {
    {(const Byte *)"PathError", 9},
    {(const Byte *)"io/fs", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(FsPathError),
    (uint16_t)_Alignof(FsPathError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66737065U, /* "fspe" */
    NULL,
};

const Type *const TYPE_FS_PATH_ERROR = &fs_path_error_desc;

static Error fs_path_error_clone(const void *self, Alloc *a);

static const ErrorVT fs_path_error_vt = {
    &fs_path_error_desc,
    fs_path_error_message,
    fs_path_error_unwrap_slot,
    NULL,
    NULL,
    NULL,
    fs_path_error_clone,
};

/* One allocation: the box, then op, path and the message, which is
 * op + " " + path + ": " + the text of err. */
Error fs_path_error_new(Alloc *a, Str op, Str path, Error err) {
    Str text = error_text(err);
    Int mlen = op.len + 1 + path.len + 2 + text.len;
    size_t size =
        sizeof(FsPathErrorBox) + (size_t)op.len + (size_t)path.len + (size_t)mlen;
    FsPathErrorBox *b =
        (FsPathErrorBox *)mem_alloc_nozero(a, size, _Alignof(FsPathErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.op = str_from_bytes(p, op.len);
    p = fs_put(p, op);
    b->e.path = str_from_bytes(p, path.len);
    p = fs_put(p, path);
    b->e.err = err;
    b->message = str_from_bytes(p, mlen);
    p = fs_put(p, op);
    *p++ = ' ';
    p = fs_put(p, path);
    *p++ = ':';
    *p++ = ' ';
    fs_put(p, text);
    return (Error){&fs_path_error_vt, b};
}

Error fs_path_error_as_error(const FsPathError *e, Alloc *a) {
    return fs_path_error_new(a, e->op, e->path, e->err);
}

static Error fs_path_error_clone(const void *self, Alloc *a) {
    const FsPathError *e = (const FsPathError *)self;
    return fs_path_error_new(a, e->op, e->path, error_retain(a, e->err));
}

Str fs_path_error_error(const FsPathError *e, Alloc *a) {
    Str parts[5] = {e->op, FS_LIT(" "), e->path, FS_LIT(": "), error_text(e->err)};
    return fs_cat(a, parts, 5);
}

Error fs_path_error_unwrap(const FsPathError *e) {
    return e->err;
}

/* Go asks e.Err for a Timeout() bool method. Here the methods are on the
 * descriptor of the error's type, when it has one. */
bool fs_path_error_timeout(const FsPathError *e) {
    if (e == NULL || e->err.vt == NULL || e->err.vt->self_type == NULL)
        return false;
    const Method *m = type_method_by_name(e->err.vt->self_type, FS_LIT("Timeout"));
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    if (type_num_in(m->ftype) != 0 || type_num_out(m->ftype) != 1 ||
        type_out(m->ftype, 0) != TYPE_BOOL)
        return false;
    bool out = false;
    void *rets[1] = {&out};
    method_call(m, (void *)(uintptr_t)e->err.data, NULL, rets);
    return out;
}

/* ------------------------------------------------------------- descriptors */

#define FS_IFACE_TYPE(cname, gonm, hash)                                               \
    static const Type cname##_desc = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"io/fs", 5},                                                    \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
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

FS_IFACE_TYPE(FsFileInfo, "FileInfo", 0x66736669U);
FS_IFACE_TYPE(FsDirEntry, "DirEntry", 0x66736465U);
FS_IFACE_TYPE(FsFile, "File", 0x6673666cU);
FS_IFACE_TYPE(Fs, "FS", 0x66736673U);

const Type *const TYPE_FS_FILE_INFO = &FsFileInfo_desc;
const Type *const TYPE_FS_DIR_ENTRY = &FsDirEntry_desc;
const Type *const TYPE_FS_FILE = &FsFile_desc;
const Type *const TYPE_FS = &Fs_desc;

/* ---------------------------------------------------------------- FileMode */

Str fs_file_mode_string(FsFileMode m, Alloc *a) {
    static const char letters[] = "dalTLDpSugct?";
    static const char rwx[] = "rwxrwxrwx";
    Byte buf[32];
    int w = 0;
    for (int i = 0; letters[i] != 0; i++)
        if (m & ((FsFileMode)1U << (31 - i)))
            buf[w++] = (Byte)letters[i];
    if (w == 0)
        buf[w++] = '-';
    for (int i = 0; i < 9; i++)
        buf[w++] = (m & ((FsFileMode)1U << (8 - i))) ? (Byte)rwx[i] : (Byte)'-';
    Str s = str_from_bytes(buf, w);
    return fs_cat(a, &s, 1);
}

bool fs_file_mode_is_dir(FsFileMode m) {
    return (m & FS_MODE_DIR) != 0;
}

bool fs_file_mode_is_regular(FsFileMode m) {
    return (m & FS_MODE_TYPE) == 0;
}

FsFileMode fs_file_mode_perm(FsFileMode m) {
    return m & FS_MODE_PERM;
}

FsFileMode fs_file_mode_type(FsFileMode m) {
    return m & FS_MODE_TYPE;
}

/* ------------------------------------------------------------------ format */

Str fs_format_file_info(Alloc *a, FsFileInfo info) {
    Str name = info.vt->name(info.data);
    Str mode = fs_file_mode_string(info.vt->mode(info.data), a);

    /* The size in decimal, with a '-' in front of a negative one. */
    Byte num[21];
    int i = (int)sizeof(num);
    int64_t size = info.vt->size(info.data);
    uint64_t usize = size >= 0 ? (uint64_t)size : 0 - (uint64_t)size;
    do {
        num[--i] = (Byte)('0' + usize % 10);
        usize /= 10;
    } while (usize > 0);
    if (size < 0)
        num[--i] = '-';

    Str when = time_format(info.vt->mod_time(info.data), a, TIME_DATE_TIME);
    bool dir = info.vt->is_dir(info.data);
    Str parts[8] = {
        mode,
        FS_LIT(" "),
        str_from_bytes(num + i, (Int)sizeof(num) - i),
        FS_LIT(" "),
        when,
        FS_LIT(" "),
        name,
        dir ? FS_LIT("/") : BURROW_STR_EMPTY,
    };
    return fs_cat(a, parts, 8);
}

Str fs_format_dir_entry(Alloc *a, FsDirEntry dir) {
    Str mode = fs_file_mode_string(dir.vt->type(dir.data), a);
    if (mode.len >= 9)
        mode.len -= 9;
    Str parts[4] = {mode, FS_LIT(" "), dir.vt->name(dir.data),
                    dir.vt->is_dir(dir.data) ? FS_LIT("/") : BURROW_STR_EMPTY};
    return fs_cat(a, parts, 4);
}

/* ----------------------------------------------------------------- dirInfo */

/* fs.dirInfo, a FileInfo seen as a DirEntry. */
typedef struct FsDirInfo {
    FsFileInfo info;
} FsDirInfo;

static const Type fs_dir_info_desc = {
    {(const Byte *)"dirInfo", 7},
    {(const Byte *)"io/fs", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(FsDirInfo),
    (uint16_t)_Alignof(FsDirInfo),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66736469U, /* "fsdi" */
    NULL,
};

static Str fs_dir_info_name(void *self) {
    FsFileInfo fi = ((FsDirInfo *)self)->info;
    return fi.vt->name(fi.data);
}

static bool fs_dir_info_is_dir(void *self) {
    FsFileInfo fi = ((FsDirInfo *)self)->info;
    return fi.vt->is_dir(fi.data);
}

static FsFileMode fs_dir_info_type(void *self) {
    FsFileInfo fi = ((FsDirInfo *)self)->info;
    return fi.vt->mode(fi.data) & FS_MODE_TYPE;
}

static FsFileInfo fs_dir_info_info(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return ((FsDirInfo *)self)->info;
}

static const FsDirEntryVT fs_dir_info_vt = {
    &fs_dir_info_desc, fs_dir_info_name, fs_dir_info_is_dir,
    fs_dir_info_type,  fs_dir_info_info,
};

FsDirEntry fs_file_info_to_dir_entry(Alloc *a, FsFileInfo info) {
    FsDirEntry out = {NULL, NULL};
    if (info.vt == NULL)
        return out;
    FsDirInfo *d = (FsDirInfo *)mem_alloc_nozero(a, sizeof *d, _Alignof(FsDirInfo));
    if (d == NULL)
        return out;
    d->info = info;
    out.vt = &fs_dir_info_vt;
    out.data = d;
    return out;
}

/* -------------------------------------------------------------------- File */

IoReader fs_file_as_io_reader(FsFile f) {
    IoReader r = {NULL, NULL};
    if (f.vt == NULL)
        return r;
    r.vt = &f.vt->read_closer.reader;
    r.data = f.data;
    return r;
}

static void fs_close(FsFile f) {
    (void)f.vt->read_closer.closer.close(f.data);
}

/* --------------------------------------------------------------- ValidPath */

bool fs_valid_path(Str name) {
    if (!utf8_valid_string(name))
        return false;
    if (name.len == 1 && name.p[0] == '.')
        return true;
    Int start = 0;
    for (;;) {
        Int i = start;
        while (i < name.len && name.p[i] != '/')
            i++;
        Int n = i - start;
        const Byte *e = n > 0 ? name.p + start : name.p;
        if (n == 0 || (n == 1 && e[0] == '.') || (n == 2 && e[0] == '.' && e[1] == '.'))
            return false;
        if (i == name.len)
            return true;
        start = i + 1;
    }
}

/* -------------------------------------------------------------------- Stat */

FsFileInfo fs_stat(Alloc *a, Fs fsys, Str name, Error *err) {
    if (fsys.vt->stat != NULL)
        return fsys.vt->stat(fsys.data, a, name, err);
    FsFileInfo none = {NULL, NULL};
    FsFile file = fsys.vt->open(fsys.data, a, name, err);
    if (BURROW_FAILED(*err))
        return none;
    FsFileInfo info = file.vt->stat(file.data, a, err);
    fs_close(file);
    return info;
}

FsFileInfo fs_lstat(Alloc *a, Fs fsys, Str name, Error *err) {
    if (fsys.vt->lstat == NULL)
        return fs_stat(a, fsys, name, err);
    return fsys.vt->lstat(fsys.data, a, name, err);
}

Str fs_read_link(Alloc *a, Fs fsys, Str name, Error *err) {
    if (fsys.vt->read_link == NULL) {
        *err = fs_path_error_new(a, FS_LIT("readlink"), name, fs_err_invalid);
        return BURROW_STR_EMPTY;
    }
    return fsys.vt->read_link(fsys.data, a, name, err);
}

/* ---------------------------------------------------------------- ReadFile */

Slice fs_read_file(Alloc *a, Fs fsys, Str name, Error *err) {
    if (fsys.vt->read_file != NULL)
        return fsys.vt->read_file(fsys.data, a, name, err);

    Slice data = slice_nil(TYPE_BYTE);
    FsFile file = fsys.vt->open(fsys.data, a, name, err);
    if (BURROW_FAILED(*err))
        return data;

    /* The size the file says it has, and one byte more, so that the read that
     * finds the end has somewhere to go without growing the buffer. */
    Int size = 0;
    Error serr = BURROW_NO_ERROR;
    FsFileInfo info = file.vt->stat(file.data, a, &serr);
    if (BURROW_OK(serr) && info.vt != NULL) {
        int64_t size64 = info.vt->size(info.data);
        if (size64 >= 0 && (int64_t)(Int)size64 == size64 &&
            (Int)size64 < BURROW_INT_MAX)
            size = (Int)size64;
    }

    Int cap = size + 1;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (buf == NULL) {
        fs_close(file);
        *err = burrow_err_out_of_memory;
        return data;
    }
    Int len = 0;
    for (;;) {
        if (len >= cap) {
            Int ncap = cap < 256 ? 512 : cap + cap / 2;
            Byte *nbuf = (Byte *)mem_realloc(a, buf, (size_t)cap, (size_t)ncap, 1);
            if (nbuf == NULL) {
                fs_close(file);
                *err = burrow_err_out_of_memory;
                return slice_from(buf, len, cap, TYPE_BYTE);
            }
            buf = nbuf;
            cap = ncap;
        }
        Error rerr = BURROW_NO_ERROR;
        Slice p = slice_from(buf + len, cap - len, cap - len, TYPE_BYTE);
        Int n = file.vt->read_closer.reader.read(file.data, p, &rerr);
        if (n > 0)
            len += n;
        if (BURROW_FAILED(rerr)) {
            fs_close(file);
            *err = fs_same(rerr, io_eof) ? BURROW_NO_ERROR : rerr;
            return slice_from(buf, len, cap, TYPE_BYTE);
        }
    }
}

/* ----------------------------------------------------------------- ReadDir */

static int fs_entry_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsDirEntry *d1 = (const FsDirEntry *)x;
    const FsDirEntry *d2 = (const FsDirEntry *)y;
    return (int)strings_compare(d1->vt->name(d1->data), d2->vt->name(d2->data));
}

Slice fs_read_dir(Alloc *a, Fs fsys, Str name, Error *err) {
    if (fsys.vt->read_dir != NULL)
        return fsys.vt->read_dir(fsys.data, a, name, err);

    Slice none = slice_nil(TYPE_FS_DIR_ENTRY);
    FsFile file = fsys.vt->open(fsys.data, a, name, err);
    if (BURROW_FAILED(*err))
        return none;
    if (file.vt->read_dir == NULL) {
        fs_close(file);
        *err = fs_path_error_new(a, FS_LIT("readdir"), name, fs_err_not_implemented);
        return none;
    }
    Slice list = file.vt->read_dir(file.data, a, -1, err);
    slices_sort_func(list, BURROW_FN(SlicesCmpFunc, fs_entry_cmp, NULL));
    fs_close(file);
    return list;
}

/* -------------------------------------------------------------------- Glob */

static bool fs_has_meta(Str path) {
    for (Int i = 0; i < path.len; i++) {
        switch (path.p[i]) {
        case '*':
        case '?':
        case '[':
        case '\\':
            return true;
        default:
            break;
        }
    }
    return false;
}

/* fs.glob: the entries of dir matching pattern, appended to matches. An error
 * reading dir is ignored. */
static Slice fs_glob_dir(Alloc *a, Fs fsys, Str dir, Str pattern, Slice matches,
                         Error *err) {
    *err = BURROW_NO_ERROR;
    Error rerr = BURROW_NO_ERROR;
    Slice infos = fs_read_dir(a, fsys, dir, &rerr);
    if (BURROW_FAILED(rerr))
        return matches;
    for (Int i = 0; i < infos.len; i++) {
        FsDirEntry *d = (FsDirEntry *)slice_at(infos, i);
        Str n = d->vt->name(d->data);
        Error merr = BURROW_NO_ERROR;
        bool matched = path_match(pattern, n, &merr);
        if (BURROW_FAILED(merr)) {
            *err = merr;
            return matches;
        }
        if (matched) {
            Str full = path_join_v(a, 2, dir, n);
            matches = slice_append(a, matches, &full, 1);
        }
    }
    return matches;
}

/* globWithLimit, as a loop. Go recurses once for each directory in the
 * pattern that has a meta character in it, which a goroutine's stack does not
 * mind and a fixed one does, so this walks up the pattern to the first
 * directory without one, keeping the file parts it passes, and then globs its
 * way back down. The checks happen in the order Go's make them. */
Slice fs_glob(Alloc *a, Fs fsys, Str pattern, Error *err) {
    Slice none = slice_nil(TYPE_STRING);
    *err = BURROW_NO_ERROR;
    if (fsys.vt->glob != NULL)
        return fsys.vt->glob(fsys.data, a, pattern, err);

    Slice files = slice_nil(TYPE_STRING); /* the file parts, deepest first */
    Slice matches = none;
    Str p = pattern;
    for (int depth = 0;; depth++) {
        if (depth > 10000) {
            *err = path_err_bad_pattern;
            return none;
        }
        (void)path_match(p, BURROW_STR_EMPTY, err);
        if (BURROW_FAILED(*err))
            return none;
        if (!fs_has_meta(p)) {
            Error serr = BURROW_NO_ERROR;
            (void)fs_stat(a, fsys, p, &serr);
            if (BURROW_OK(serr))
                matches = slice_append(a, none, &p, 1);
            break;
        }

        Str file;
        Str dir = path_split(p, &file);
        if (dir.len == 0)
            dir = FS_LIT(".");
        else
            dir.len--; /* the slash at the end */

        if (!fs_has_meta(dir)) {
            matches = fs_glob_dir(a, fsys, dir, file, none, err);
            if (BURROW_FAILED(*err))
                return files.len == 0 ? matches : none;
            break;
        }

        /* Prevent infinite recursion. */
        if (str_eq(dir, p)) {
            *err = path_err_bad_pattern;
            return none;
        }
        files = slice_append(a, files, &file, 1);
        if (files.len == 0) {
            *err = burrow_err_out_of_memory;
            return none;
        }
        p = dir;
    }

    for (Int i = files.len - 1; i >= 0; i--) {
        Str file = *(Str *)slice_at(files, i);
        Slice m = matches;
        matches = none;
        for (Int j = 0; j < m.len; j++) {
            matches = fs_glob_dir(a, fsys, *(Str *)slice_at(m, j), file, matches, err);
            if (BURROW_FAILED(*err))
                return i == 0 ? matches : none;
        }
    }
    return matches;
}

/* --------------------------------------------------------------------- Sub */

/* fs.subFS: fsys seen from dir. */
typedef struct FsSubData {
    Fs fsys;
    Str dir;
} FsSubData;

static const Type fs_sub_desc = {
    {(const Byte *)"subFS", 5},
    {(const Byte *)"io/fs", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(FsSubData),
    (uint16_t)_Alignof(FsSubData),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66737362U, /* "fssb" */
    NULL,
};

/* dir joined with name, or an FsPathError for op when name is not valid. */
static Str fs_sub_full_name(FsSubData *f, Alloc *a, Str op, Str name, Error *err) {
    if (!fs_valid_path(name)) {
        *err = fs_path_error_new(a, op, name, fs_err_invalid);
        return BURROW_STR_EMPTY;
    }
    *err = BURROW_NO_ERROR;
    return path_join_v(a, 2, f->dir, name);
}

/* name made relative to dir again, when it is under dir. */
static bool fs_sub_shorten(const FsSubData *f, Str name, Str *rel) {
    Int d = f->dir.len;
    if (str_eq(name, f->dir)) {
        *rel = FS_LIT(".");
        return true;
    }
    if (name.len >= d + 2 && name.p[d] == '/' &&
        memcmp(name.p, f->dir.p, (size_t)d) == 0) {
        *rel = str_from_bytes(name.p + d + 1, name.len - d - 1);
        return true;
    }
    *rel = BURROW_STR_EMPTY;
    return false;
}

/* An FsPathError whose path is under dir, with the path made relative. Go
 * changes the error it was given, and this makes a new one, which is the same
 * to anyone who only has the result. */
static Error fs_sub_fix_err(const FsSubData *f, Alloc *a, Error err) {
    if (err.vt != &fs_path_error_vt)
        return err;
    const FsPathError *e = (const FsPathError *)err.data;
    Str short_name;
    if (e == NULL || !fs_sub_shorten(f, e->path, &short_name))
        return err;
    return fs_path_error_new(a, e->op, short_name, e->err);
}

static FsFile fs_sub_open(void *self, Alloc *a, Str name, Error *err) {
    FsSubData *f = (FsSubData *)self;
    FsFile none = {NULL, NULL};
    Str full = fs_sub_full_name(f, a, FS_LIT("open"), name, err);
    if (BURROW_FAILED(*err))
        return none;
    FsFile file = f->fsys.vt->open(f->fsys.data, a, full, err);
    *err = fs_sub_fix_err(f, a, *err);
    return file;
}

static Slice fs_sub_read_dir(void *self, Alloc *a, Str name, Error *err) {
    FsSubData *f = (FsSubData *)self;
    Str full = fs_sub_full_name(f, a, FS_LIT("read"), name, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_FS_DIR_ENTRY);
    Slice dir = fs_read_dir(a, f->fsys, full, err);
    *err = fs_sub_fix_err(f, a, *err);
    return dir;
}

static Slice fs_sub_read_file(void *self, Alloc *a, Str name, Error *err) {
    FsSubData *f = (FsSubData *)self;
    Str full = fs_sub_full_name(f, a, FS_LIT("read"), name, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    Slice data = fs_read_file(a, f->fsys, full, err);
    *err = fs_sub_fix_err(f, a, *err);
    return data;
}

static Str fs_sub_read_link(void *self, Alloc *a, Str name, Error *err) {
    FsSubData *f = (FsSubData *)self;
    Str full = fs_sub_full_name(f, a, FS_LIT("readlink"), name, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;
    Str target = fs_read_link(a, f->fsys, full, err);
    if (BURROW_FAILED(*err)) {
        *err = fs_sub_fix_err(f, a, *err);
        return BURROW_STR_EMPTY;
    }
    return target;
}

static FsFileInfo fs_sub_lstat(void *self, Alloc *a, Str name, Error *err) {
    FsSubData *f = (FsSubData *)self;
    FsFileInfo none = {NULL, NULL};
    Str full = fs_sub_full_name(f, a, FS_LIT("lstat"), name, err);
    if (BURROW_FAILED(*err))
        return none;
    FsFileInfo info = fs_lstat(a, f->fsys, full, err);
    if (BURROW_FAILED(*err)) {
        *err = fs_sub_fix_err(f, a, *err);
        return none;
    }
    return info;
}

static Slice fs_sub_glob(void *self, Alloc *a, Str pattern, Error *err) {
    FsSubData *f = (FsSubData *)self;
    Slice none = slice_nil(TYPE_STRING);
    *err = BURROW_NO_ERROR;
    (void)path_match(pattern, BURROW_STR_EMPTY, err);
    if (BURROW_FAILED(*err))
        return none;
    if (pattern.len == 1 && pattern.p[0] == '.') {
        Str dot = FS_LIT(".");
        return slice_append(a, none, &dot, 1);
    }

    Str parts[3] = {f->dir, FS_LIT("/"), pattern};
    Str full = fs_cat(a, parts, 3);
    Slice list = fs_glob(a, f->fsys, full, err);
    for (Int i = 0; i < list.len; i++) {
        Str *name = (Str *)slice_at(list, i);
        Str rel;
        if (!fs_sub_shorten(f, *name, &rel)) {
            /* Go's message has the name the failed shorten gave back, which
             * is empty, and not the name that was not under dir. */
            Str msg[4] = {FS_LIT("invalid result from inner fsys Glob: "), rel,
                          FS_LIT(" not in "), f->dir};
            *err = errors_new(a, fs_cat(a, msg, 4));
            return none;
        }
        *name = rel;
    }
    *err = fs_sub_fix_err(f, a, *err);
    return list;
}

static Fs fs_sub_sub(void *self, Alloc *a, Str dir, Error *err);

static const FsVT fs_sub_vt = {
    &fs_sub_desc, fs_sub_open, fs_sub_read_dir,  fs_sub_read_file, NULL,
    fs_sub_sub,   fs_sub_glob, fs_sub_read_link, fs_sub_lstat,
};

static Fs fs_sub_make(Alloc *a, Fs fsys, Str dir, Error *err) {
    Fs out = {NULL, NULL};
    FsSubData *f = (FsSubData *)mem_alloc_nozero(a, sizeof *f, _Alignof(FsSubData));
    if (f == NULL) {
        *err = burrow_err_out_of_memory;
        return out;
    }
    f->fsys = fsys;
    f->dir = dir;
    *err = BURROW_NO_ERROR;
    out.vt = &fs_sub_vt;
    out.data = f;
    return out;
}

static Fs fs_sub_sub(void *self, Alloc *a, Str dir, Error *err) {
    FsSubData *f = (FsSubData *)self;
    if (dir.len == 1 && dir.p[0] == '.') {
        *err = BURROW_NO_ERROR;
        return (Fs){&fs_sub_vt, f};
    }
    Fs none = {NULL, NULL};
    Str full = fs_sub_full_name(f, a, FS_LIT("sub"), dir, err);
    if (BURROW_FAILED(*err))
        return none;
    return fs_sub_make(a, f->fsys, full, err);
}

Fs fs_sub(Alloc *a, Fs fsys, Str dir, Error *err) {
    Fs none = {NULL, NULL};
    if (!fs_valid_path(dir)) {
        *err = fs_path_error_new(a, FS_LIT("sub"), dir, fs_err_invalid);
        return none;
    }
    if (dir.len == 1 && dir.p[0] == '.') {
        *err = BURROW_NO_ERROR;
        return fsys;
    }
    if (fsys.vt->sub != NULL)
        return fsys.vt->sub(fsys.data, a, dir, err);
    return fs_sub_make(a, fsys, dir, err);
}

/* -------------------------------------------------------------------- Walk */

static Error fs_walk(Alloc *a, Fs fsys, Str name, FsDirEntry d, FsWalkDirFunc fn) {
    Error err = fn.f(fn.env, name, d, BURROW_NO_ERROR);
    if (BURROW_FAILED(err) || !d.vt->is_dir(d.data)) {
        if (fs_same(err, fs_skip_dir) && d.vt->is_dir(d.data))
            err = BURROW_NO_ERROR;
        return err;
    }

    Slice dirs = fs_read_dir(a, fsys, name, &err);
    if (BURROW_FAILED(err)) {
        /* Second call, to report the ReadDir error. */
        err = fn.f(fn.env, name, d, err);
        if (BURROW_FAILED(err)) {
            if (fs_same(err, fs_skip_dir) && d.vt->is_dir(d.data))
                err = BURROW_NO_ERROR;
            return err;
        }
    }

    for (Int i = 0; i < dirs.len; i++) {
        FsDirEntry d1 = *(FsDirEntry *)slice_at(dirs, i);
        Str name1 = path_join_v(a, 2, name, d1.vt->name(d1.data));
        Error e = fs_walk(a, fsys, name1, d1, fn);
        if (BURROW_FAILED(e)) {
            if (fs_same(e, fs_skip_dir))
                break;
            return e;
        }
    }
    return BURROW_NO_ERROR;
}

Error fs_walk_dir(Alloc *a, Fs fsys, Str root, FsWalkDirFunc fn) {
    Error err = BURROW_NO_ERROR;
    FsFileInfo info = fs_stat(a, fsys, root, &err);
    if (BURROW_FAILED(err)) {
        FsDirEntry none = {NULL, NULL};
        err = fn.f(fn.env, root, none, err);
    } else {
        FsDirEntry d = fs_file_info_to_dir_entry(a, info);
        if (d.vt == NULL)
            return burrow_err_out_of_memory;
        err = fs_walk(a, fsys, root, d, fn);
    }
    if (fs_same(err, fs_skip_dir) || fs_same(err, fs_skip_all))
        return BURROW_NO_ERROR;
    return err;
}
