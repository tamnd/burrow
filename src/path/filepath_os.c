/* Derived from Go's src/path/filepath/path.go (Abs, Walk and WalkDir),
 * match.go (Glob), symlink.go, symlink_unix.go and symlink_windows.go
 * (EvalSymlinks), and path_unix.go and path_windows.go (abs). Go source:
 * go1.27.1.
 *
 * The part of filepath that asks the operating system, kept apart so that
 * filepath.c stays lexical. Unlike there, nothing here takes win: these follow
 * the host's rules, because it is the host they ask.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/path/filepath.h"
#include "filepath_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"
#include "burrow/type.h"

#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#define FPO_WIN true
#define FPO_SEP "\\"
#else
#define FPO_WIN false
#define FPO_SEP "/"
#endif

#define FPO_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* err == target in Go, for SkipDir and SkipAll, which are compared and not
 * matched with errors.Is. */
static bool fpo_same(Error e, Error target) {
    return e.vt == target.vt && e.data == target.data;
}

static Str fpo_tail(Str s, Int i) {
    return (Str){s.p + i, s.len - i};
}

/* x and y in one allocation. */
static Str fpo_cat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return (Str){p, x.len + y.len};
}

/* ---------------------------------------------------------------- WalkDir */

static Error fpo_walk_dir(Alloc *a, Str path, FsDirEntry d, FsWalkDirFunc fn) {
    Error err = fn.f(fn.env, path, d, BURROW_NO_ERROR);
    if (BURROW_FAILED(err) || !d.vt->is_dir(d.data)) {
        if (fpo_same(err, fs_skip_dir) && d.vt->is_dir(d.data))
            err = BURROW_NO_ERROR; /* successfully skipped directory */
        return err;
    }

    Slice dirs = os_read_dir(a, path, &err);
    if (BURROW_FAILED(err)) {
        /* Second call, to report the ReadDir error. */
        err = fn.f(fn.env, path, d, err);
        if (BURROW_FAILED(err)) {
            if (fpo_same(err, fs_skip_dir) && d.vt->is_dir(d.data))
                err = BURROW_NO_ERROR;
            return err;
        }
    }

    for (Int i = 0; i < dirs.len; i++) {
        FsDirEntry d1 = *(FsDirEntry *)slice_at(dirs, i);
        Str path1 = filepath_join_v(a, 2, path, d1.vt->name(d1.data));
        if (path1.len == 0)
            return burrow_err_out_of_memory;
        Error e = fpo_walk_dir(a, path1, d1, fn);
        if (BURROW_FAILED(e)) {
            if (fpo_same(e, fs_skip_dir))
                break;
            return e;
        }
    }
    return BURROW_NO_ERROR;
}

Error filepath_walk_dir(Alloc *a, Str root, FsWalkDirFunc fn) {
    Error err = BURROW_NO_ERROR;
    FsFileInfo info = os_lstat(a, root, &err);
    if (BURROW_FAILED(err)) {
        FsDirEntry none = {NULL, NULL};
        err = fn.f(fn.env, root, none, err);
    } else {
        FsDirEntry d = fs_file_info_to_dir_entry(a, info);
        if (d.vt == NULL)
            return burrow_err_out_of_memory;
        err = fpo_walk_dir(a, root, d, fn);
    }
    if (fpo_same(err, fs_skip_dir) || fpo_same(err, fs_skip_all))
        return BURROW_NO_ERROR;
    return err;
}

/* ------------------------------------------------------------------- Walk */

/* The names in dirname, sorted. */
static Slice fpo_read_dir_names(Alloc *a, Str dirname, Error *err) {
    Slice none = slice_nil(TYPE_STRING);
    OsFile *f = os_open(a, dirname, err);
    if (BURROW_FAILED(*err))
        return none;
    Slice names = os_file_readdirnames(f, a, -1, err);
    os_file_free(f);
    if (BURROW_FAILED(*err))
        return none;
    sort_strings(names);
    return names;
}

static Error fpo_walk(Alloc *a, Str path, FsFileInfo info, FilepathWalkFunc fn) {
    if (!info.vt->is_dir(info.data))
        return fn.f(fn.env, path, info, BURROW_NO_ERROR);

    Error err = BURROW_NO_ERROR;
    Slice names = fpo_read_dir_names(a, path, &err);
    Error err1 = fn.f(fn.env, path, info, err);
    /* A read that failed means the walk cannot go into the directory, and
     * err1 means fn wants it skipped or stopped. fn decides which by what it
     * returns, and may have ignored err. */
    if (BURROW_FAILED(err) || BURROW_FAILED(err1))
        return err1;

    for (Int i = 0; i < names.len; i++) {
        Str filename = filepath_join_v(a, 2, path, *(Str *)slice_at(names, i));
        if (filename.len == 0)
            return burrow_err_out_of_memory;
        Error lerr = BURROW_NO_ERROR;
        FsFileInfo fi = os_lstat(a, filename, &lerr);
        if (BURROW_FAILED(lerr)) {
            FsFileInfo none = {NULL, NULL};
            Error e = fn.f(fn.env, filename, none, lerr);
            if (BURROW_FAILED(e) && !fpo_same(e, fs_skip_dir))
                return e;
        } else {
            Error e = fpo_walk(a, filename, fi, fn);
            if (BURROW_FAILED(e) &&
                (!fi.vt->is_dir(fi.data) || !fpo_same(e, fs_skip_dir)))
                return e;
        }
    }
    return BURROW_NO_ERROR;
}

Error filepath_walk(Alloc *a, Str root, FilepathWalkFunc fn) {
    Error err = BURROW_NO_ERROR;
    FsFileInfo info = os_lstat(a, root, &err);
    if (BURROW_FAILED(err)) {
        FsFileInfo none = {NULL, NULL};
        err = fn.f(fn.env, root, none, err);
    } else {
        err = fpo_walk(a, root, info, fn);
    }
    if (fpo_same(err, fs_skip_dir) || fpo_same(err, fs_skip_all))
        return BURROW_NO_ERROR;
    return err;
}

/* ------------------------------------------------------------------- Glob */

static bool fpo_has_meta(Str path) {
    return strings_contains_any(path, FPO_WIN ? FPO_LIT("*?[") : FPO_LIT("*?[\\"));
}

/* cleanGlobPath, or cleanGlobPathWindows there: dir as Split left it, made
 * into the directory to read. Returns the length of the volume name in it,
 * or -1 when an allocation fails. */
static Int fpo_clean_glob_path(Alloc *a, Str path, Str *cleaned) {
    if (path.len == 0) {
        *cleaned = FPO_LIT(".");
        return 0;
    }
    if (!FPO_WIN) {
        /* A lone separator stays, and anything else loses the one it ends
         * with. */
        *cleaned = path.len == 1 && path.p[0] == FILEPATH_SEPARATOR
                       ? path
                       : (Str){path.p, path.len - 1};
        return 0;
    }
    Int vollen = burrow__filepath_volume_name_len(path, true);
    if (vollen + 1 == path.len && filepath_is_path_separator(path.p[path.len - 1])) {
        /* /, \, C:\ and C:/ */
        *cleaned = path;
        return vollen + 1;
    }
    if (vollen == path.len && path.len == 2) {
        /* C: into C:. */
        *cleaned = fpo_cat(a, path, FPO_LIT("."));
        return cleaned->len == 0 ? -1 : vollen;
    }
    if (vollen >= path.len)
        vollen = path.len - 1;
    *cleaned = (Str){path.p, path.len - 1};
    return vollen;
}

/* glob: the names in dir that match pattern, joined onto dir and appended to
 * matches in lexical order. A directory that cannot be read adds nothing. */
static Slice fpo_glob_dir(Alloc *a, Str dir, Str pattern, Slice matches, Error *err) {
    *err = BURROW_NO_ERROR;
    Error ioerr = BURROW_NO_ERROR;
    FsFileInfo fi = os_stat(a, dir, &ioerr);
    if (BURROW_FAILED(ioerr) || !fi.vt->is_dir(fi.data))
        return matches;
    OsFile *d = os_open(a, dir, &ioerr);
    if (BURROW_FAILED(ioerr))
        return matches;
    Slice names = os_file_readdirnames(d, a, -1, &ioerr);
    os_file_free(d);
    sort_strings(names);

    for (Int i = 0; i < names.len; i++) {
        Str n = *(Str *)slice_at(names, i);
        bool matched = filepath_match(pattern, n, err);
        if (BURROW_FAILED(*err))
            return matches;
        if (matched) {
            Str full = filepath_join_v(a, 2, dir, n);
            if (full.len == 0) {
                *err = burrow_err_out_of_memory;
                return matches;
            }
            matches = slice_append(a, matches, &full, 1);
        }
    }
    return matches;
}

/* Go's globWithLimit recurses on the directory part. This goes down the same
 * way with a loop, keeping the file part of each level, and then comes back
 * up through them. */
Slice filepath_glob(Alloc *a, Str pattern, Error *err) {
    /* The limit Go has against running out of stack. See CVE-2022-30632. */
    enum { PATH_SEPARATORS_LIMIT = 10000 };
    Slice none = slice_nil(TYPE_STRING);
    *err = BURROW_NO_ERROR;

    Slice files = slice_nil(TYPE_STRING); /* the file parts, deepest first */
    Slice matches = none;
    Str p = pattern;
    for (int depth = 0;; depth++) {
        if (depth == PATH_SEPARATORS_LIMIT) {
            *err = filepath_err_bad_pattern;
            return none;
        }
        /* Check the pattern is well formed. */
        (void)filepath_match(p, BURROW_STR_EMPTY, err);
        if (BURROW_FAILED(*err))
            return none;
        if (!fpo_has_meta(p)) {
            Error lerr = BURROW_NO_ERROR;
            (void)os_lstat(a, p, &lerr);
            if (BURROW_OK(lerr))
                matches = slice_append(a, none, &p, 1);
            break;
        }

        Str file;
        Str dir = filepath_split(p, &file);
        Int vol = fpo_clean_glob_path(a, dir, &dir);
        if (vol < 0) {
            *err = burrow_err_out_of_memory;
            return none;
        }

        if (!fpo_has_meta(fpo_tail(dir, vol))) {
            matches = fpo_glob_dir(a, dir, file, none, err);
            if (BURROW_FAILED(*err))
                return files.len == 0 ? matches : none;
            break;
        }

        /* Prevent infinite recursion. See Go issue 15879. */
        if (str_eq(dir, p)) {
            *err = filepath_err_bad_pattern;
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
            matches = fpo_glob_dir(a, *(Str *)slice_at(m, j), file, matches, err);
            if (BURROW_FAILED(*err))
                return i == 0 ? matches : none;
        }
    }
    return matches;
}

/* -------------------------------------------------------------------- Abs */

#if defined(BURROW_OS_WINDOWS)

/* What the PAL calls below need: path as a C string, or EINVAL for one with
 * a NUL in it, which is what Go's UTF16PtrFromString says. */
static char *fpo_cstr(Alloc *a, Str path, Error *err) {
    if (path.len > 0 && memchr(path.p, 0, (size_t)path.len) != NULL) {
        *err = syscall_errno_as_error(SYSCALL_EINVAL, error_allocator());
        return NULL;
    }
    char *c = (char *)mem_alloc_nozero(a, (size_t)path.len + 1, 1);
    if (c == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    if (path.len > 0)
        memcpy(c, path.p, (size_t)path.len);
    c[path.len] = 0;
    return c;
}

/* A PAL call that writes a path into a buffer of the size it promises fits,
 * with the result copied into a. */
typedef int64_t (*FpoPathCall)(const char *, char *, int64_t, PalErrno *);

static Str fpo_pal_path(Alloc *a, FpoPathCall call, Str path, Error *err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *s = arena_allocator(&ar);
    Str out = BURROW_STR_EMPTY;
    char *c = fpo_cstr(s, path, err);
    size_t cap = (size_t)PAL_WPATH_MAX * 3 + 1;
    char *buf = c == NULL ? NULL : (char *)mem_alloc_nozero(s, cap, 1);
    if (c != NULL && buf == NULL)
        *err = burrow_err_out_of_memory;
    if (buf != NULL) {
        PalErrno pe = PAL_OK;
        int64_t n = call(c, buf, (int64_t)cap, &pe);
        if (n < 0) {
            *err =
                syscall_errno_as_error(syscall_errno_from_pal(pe), error_allocator());
        } else {
            out = str_clone(a, (Str){(const Byte *)buf, (Int)n});
            if (out.len != n)
                *err = burrow_err_out_of_memory;
        }
    }
    arena_free(&ar);
    return out;
}

Str filepath_abs(Alloc *a, Str path, Error *err) {
    *err = BURROW_NO_ERROR;
    /* GetFullPathNameW refuses the empty path, and Go's Abs gives the
     * working directory for it. See Go issue 24441. */
    if (path.len == 0)
        path = FPO_LIT(".");
    Str full = fpo_pal_path(a, pal_full_path, path, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;
    return filepath_clean(a, full);
}

#else

Str filepath_abs(Alloc *a, Str path, Error *err) {
    *err = BURROW_NO_ERROR;
    if (filepath_is_abs(path))
        return filepath_clean(a, path);
    Str wd = os_getwd(a, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;
    return filepath_join_v(a, 2, wd, path);
}

#endif

/* ----------------------------------------------------------- EvalSymlinks */

/* A string that grows at the end and is cut back from it, which is all Go's
 * dest += x and dest = dest[:r] need. It lives in a scratch arena. */
typedef struct FpoBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
} FpoBuf;

static bool fpo_buf_append(FpoBuf *b, Str s) {
    if (b->len + s.len > b->cap) {
        Int ncap = b->cap * 2 > b->len + s.len ? b->cap * 2 : b->len + s.len + 32;
        Byte *np = (Byte *)mem_alloc_nozero(b->a, (size_t)ncap, 1);
        if (np == NULL)
            return false;
        if (b->len > 0)
            memcpy(np, b->p, (size_t)b->len);
        b->p = np;
        b->cap = ncap;
    }
    if (s.len > 0)
        memcpy(b->p + b->len, s.p, (size_t)s.len);
    b->len += s.len;
    return true;
}

static Str fpo_buf_str(const FpoBuf *b) {
    return (Str){b->p, b->len};
}

/* Go's walkSymlinks. Everything it builds is in s, the result included. */
static Str fpo_walk_symlinks(Alloc *s, Str path, Error *err) {
    const Str sep = FPO_LIT(FPO_SEP);
    Int vol_len = burrow__filepath_volume_name_len(path, FPO_WIN);
    if (vol_len < path.len && filepath_is_path_separator(path.p[vol_len]))
        vol_len++;
    Str vol = {path.p, vol_len};
    FpoBuf dest = {s, NULL, 0, 0};
    if (!fpo_buf_append(&dest, vol))
        goto oom;
    int links_walked = 0;
    for (Int start = vol_len, end = vol_len; start < path.len; start = end) {
        while (start < path.len && filepath_is_path_separator(path.p[start]))
            start++;
        end = start;
        while (end < path.len && !filepath_is_path_separator(path.p[end]))
            end++;

        /* On Windows "." can be a symlink. It is looked up, and what it
         * points to used if that is absolute, and if not the answer is ".". */
        Str after_vol = fpo_tail(path, burrow__filepath_volume_name_len(path, FPO_WIN));
        bool is_windows_dot = FPO_WIN && str_eq(after_vol, FPO_LIT("."));

        Str elem = {path.p + start, end - start};
        if (end == start)
            break; /* no more path elements */
        if (str_eq(elem, FPO_LIT(".")) && !is_windows_dot)
            continue;
        if (str_eq(elem, FPO_LIT(".."))) {
            /* Back up to the element before, if there is one. vol_len counts
             * any leading separator. r is the last separator in dest after
             * the volume. */
            Int r;
            for (r = dest.len - 1; r >= vol_len; r--)
                if (filepath_is_path_separator(dest.p[r]))
                    break;
            if (r < vol_len ||
                str_eq(fpo_tail(fpo_buf_str(&dest), r + 1), FPO_LIT(".."))) {
                /* Either dest has no separators, being empty or just "C:",
                 * or it ends in a ".." that had to stay. Either way this
                 * ".." stays too. */
                if (dest.len > vol_len && !fpo_buf_append(&dest, sep))
                    goto oom;
                if (!fpo_buf_append(&dest, FPO_LIT("..")))
                    goto oom;
            } else {
                dest.len = r; /* drop everything from the last separator */
            }
            continue;
        }

        /* An ordinary element, which goes on the end of dest. */
        Int dvol = burrow__filepath_volume_name_len(fpo_buf_str(&dest), FPO_WIN);
        if (dest.len > dvol && !filepath_is_path_separator(dest.p[dest.len - 1]) &&
            !fpo_buf_append(&dest, sep))
            goto oom;
        if (!fpo_buf_append(&dest, elem))
            goto oom;

        /* Resolve it if it is a symlink. */
        FsFileInfo fi = os_lstat(s, fpo_buf_str(&dest), err);
        if (BURROW_FAILED(*err))
            return BURROW_STR_EMPTY;
        FsFileMode mode = fi.vt->mode(fi.data);
        if ((mode & FS_MODE_SYMLINK) == 0) {
            if (!fs_file_mode_is_dir(mode) && end < path.len) {
                *err = syscall_errno_as_error(SYSCALL_ENOTDIR, error_allocator());
                return BURROW_STR_EMPTY;
            }
            continue;
        }

        /* A symlink. */
        if (++links_walked > 255) {
            *err =
                errors_new(error_allocator(), FPO_LIT("EvalSymlinks: too many links"));
            return BURROW_STR_EMPTY;
        }
        Str link = os_readlink(s, fpo_buf_str(&dest), err);
        if (BURROW_FAILED(*err))
            return BURROW_STR_EMPTY;
        if (is_windows_dot && !filepath_is_abs(link))
            break; /* a relative "." on Windows is just "." */

        Str npath = fpo_cat(s, link, fpo_tail(path, end));
        if (npath.len == 0 && link.len + path.len - end > 0)
            goto oom;
        path = npath;

        Int v = burrow__filepath_volume_name_len(link, FPO_WIN);
        if (v > 0) {
            /* A link to a drive name is absolute. */
            if (v < link.len && filepath_is_path_separator(link.p[v]))
                v++;
            vol = (Str){path.p, v};
            dest.len = 0;
            if (!fpo_buf_append(&dest, vol))
                goto oom;
            end = v;
            /* Go leaves volLen alone here. */
        } else if (link.len > 0 && filepath_is_path_separator(link.p[0])) {
            /* A link to an absolute path. */
            dest.len = 0;
            if (!fpo_buf_append(&dest, (Str){path.p, 1}))
                goto oom;
            end = 1;
            vol = (Str){path.p, 1};
            vol_len = 1;
        } else {
            /* A link to a relative path, which replaces the last element of
             * dest. */
            Int r;
            for (r = dest.len - 1; r >= vol_len; r--)
                if (filepath_is_path_separator(dest.p[r]))
                    break;
            if (r < vol_len) {
                dest.len = 0;
                if (!fpo_buf_append(&dest, vol))
                    goto oom;
            } else {
                dest.len = r;
            }
            end = 0;
        }
    }
    return filepath_clean(s, fpo_buf_str(&dest));

oom:
    *err = burrow_err_out_of_memory;
    return BURROW_STR_EMPTY;
}

#if defined(BURROW_OS_WINDOWS)

/* Go's toNorm with normBase: path with each element in the case Windows
 * stores it in, and the drive letter in upper case, so that the result is the
 * same for every spelling of one file. path is clean, so its separators are
 * backslashes. */
static Str fpo_to_norm(Alloc *s, Str path, Error *err) {
    if (path.len == 0)
        return path;

    /* normVolumeName: VolumeName with the drive letter upper cased. */
    Int vlen = burrow__filepath_volume_name_len(path, true);
    Str volume = burrow__filepath_volume_name(s, path, true);
    if (volume.len <= 2)
        volume = strings_to_upper(s, volume);
    if (volume.len != vlen)
        goto oom;
    path = fpo_tail(path, vlen);

    if (path.len == 0 || str_eq(path, FPO_LIT(".")) || str_eq(path, FPO_LIT("\\")))
        return fpo_cat(s, volume, path);

    /* The elements come out last first, so they are kept in a list and put
     * together the other way round. lead is a leading backslash. */
    Slice names = slice_nil(TYPE_STRING);
    bool lead = false;
    for (;;) {
        Int i = strings_last_index_byte(path, '\\');
        if (str_eq(fpo_tail(path, i + 1), FPO_LIT(".."))) {
            /* baseIsDotDot: what is left goes in as it is. */
            names = slice_append(s, names, &path, 1);
            if (names.len == 0)
                goto oom;
            break;
        }
        Str name = fpo_pal_path(s, pal_find_name, fpo_cat(s, volume, path), err);
        if (BURROW_FAILED(*err))
            return BURROW_STR_EMPTY;
        names = slice_append(s, names, &name, 1);
        if (names.len == 0)
            goto oom;
        if (i == -1)
            break;
        if (i == 0) { /* \Go or C:\Go */
            lead = true;
            break;
        }
        path = (Str){path.p, i};
    }

    FpoBuf b = {s, NULL, 0, 0};
    if (!fpo_buf_append(&b, volume) || (lead && !fpo_buf_append(&b, FPO_LIT("\\"))))
        goto oom;
    for (Int i = names.len - 1; i >= 0; i--) {
        if (!fpo_buf_append(&b, *(Str *)slice_at(names, i)) ||
            (i > 0 && !fpo_buf_append(&b, FPO_LIT("\\"))))
            goto oom;
    }
    return fpo_buf_str(&b);

oom:
    *err = burrow_err_out_of_memory;
    return BURROW_STR_EMPTY;
}

#endif

Str filepath_eval_symlinks(Alloc *a, Str path, Error *err) {
    *err = BURROW_NO_ERROR;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *s = arena_allocator(&ar);
    Str out = fpo_walk_symlinks(s, path, err);
#if defined(BURROW_OS_WINDOWS)
    if (BURROW_OK(*err))
        out = fpo_to_norm(s, out, err);
#endif
    Str r = BURROW_STR_EMPTY;
    if (BURROW_OK(*err)) {
        r = str_clone(a, out);
        if (r.len != out.len)
            *err = burrow_err_out_of_memory;
    }
    arena_free(&ar);
    return r;
}
