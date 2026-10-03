/* exec.LookPath, and the extension search Windows needs at Start.
 *
 * Derived from Go's src/os/exec/lp_unix.go, lp_windows.go and lookpath.go.
 * Go source: go1.27.1.
 *
 * Go's execerrdot GODEBUG setting is not read, so a relative result is
 * always an ErrDot.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os/exec.h"

#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include "exec_internal.h"
#include "internal.h"

#include <string.h>

#define LP_LIT(s) ((Str){(const Byte *)(s), (Int)(sizeof(s) - 1)})

#if defined(BURROW_OS_WINDOWS)
BURROW_SENTINEL_ERROR(exec_err_not_found, "executable file not found in %PATH%");
#else
BURROW_SENTINEL_ERROR(exec_err_not_found, "executable file not found in $PATH");
#endif

/* validateLookPath: the names that cannot be a program, go.dev/issue/74466. */
static Error lp_validate(Str s) {
    if (s.len == 0 || str_eq(s, LP_LIT(".")) || str_eq(s, LP_LIT("..")))
        return exec_err_not_found;
    return BURROW_NO_ERROR;
}

/* A copy of s in a, so the answer does not point into the scratch arena. */
static Str lp_keep(Alloc *a, Str s, Error *err) {
    Str out = str_clone(a, s);
    if (out.len != s.len)
        *err = burrow_err_out_of_memory;
    return out;
}

#if !defined(BURROW_OS_WINDOWS)

/* findExecutable: a file that exists, is not a directory, and that we may
 * run, asking the system with eaccess and falling back to the mode bits when
 * it cannot say. */
static Error lp_find_executable(Alloc *a, Str file) {
    Error err = BURROW_NO_ERROR;
    FsFileInfo d = os_stat(a, file, &err);
    if (BURROW_FAILED(err))
        return err;
    FsFileMode m = d.vt->mode(d.data);
    if (fs_file_mode_is_dir(m))
        return burrow__os_errno_value(SYSCALL_EISDIR);
    OsCPath c;
    if (!burrow__os_cpath(&c, file, &err))
        return err;
    PalErrno e = PAL_OK;
    bool ok = pal_eaccess(c.p, PAL_X_OK, &e);
    burrow__os_cpath_free(&c);
    if (ok)
        return BURROW_NO_ERROR;
    /* ENOSYS means there is no eaccess here, and EPERM is what a Linux
     * container's seccomp filter gives. Either way, look at the bits. */
    if (e != PAL_ENOSYS && e != PAL_EPERM)
        return burrow__os_errno(e);
    if ((m & 0111) != 0)
        return BURROW_NO_ERROR;
    return fs_err_permission;
}

static Str lp_look_path(Alloc *a, Arena *scratch, Str file, Error *err) {
    Alloc *s = arena_allocator(scratch);
    Error e = lp_validate(file);
    if (BURROW_FAILED(e)) {
        *err = burrow__exec_error(file, e);
        return BURROW_STR_EMPTY;
    }
    if (strings_index_byte(file, '/') >= 0) {
        e = lp_find_executable(s, file);
        if (BURROW_OK(e))
            return file;
        *err = burrow__exec_error(file, e);
        return BURROW_STR_EMPTY;
    }
    Slice dirs = filepath_split_list(s, os_getenv(s, LP_LIT("PATH")));
    for (Int i = 0; i < dirs.len; i++) {
        Str dir = BURROW_AT(Str, dirs, i);
        /* Unix shell semantics: an empty entry means ".". */
        if (dir.len == 0)
            dir = LP_LIT(".");
        Str path = filepath_join_v(s, 2, dir, file);
        if (BURROW_OK(lp_find_executable(s, path))) {
            Str out = lp_keep(a, path, err);
            if (BURROW_OK(*err) && !filepath_is_abs(path))
                *err = burrow__exec_error(file, exec_err_dot);
            return out;
        }
    }
    *err = burrow__exec_error(file, exec_err_not_found);
    return BURROW_STR_EMPTY;
}

Str burrow__exec_look_extensions(Alloc *a, Str path, Str dir, Error *err) {
    (void)a;
    (void)dir;
    *err = BURROW_NO_ERROR;
    return path;
}

#else

/* chkStat: anything that exists and is not a directory. */
static Error lp_chk_stat(Alloc *a, Str file) {
    Error err = BURROW_NO_ERROR;
    FsFileInfo d = os_stat(a, file, &err);
    if (BURROW_FAILED(err))
        return err;
    if (d.vt->is_dir(d.data))
        return fs_err_permission;
    return BURROW_NO_ERROR;
}

static bool lp_has_ext(Str file) {
    Int i = strings_last_index(file, LP_LIT("."));
    if (i < 0)
        return false;
    return strings_last_index_any(file, LP_LIT(":\\/")) < i;
}

static Str lp_cat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len) + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

/* findExecutable: file itself when it has an extension already, and then
 * file with each of exts on the end. */
static Str lp_find_executable(Alloc *a, Str file, Slice exts, Error *err) {
    *err = BURROW_NO_ERROR;
    if (exts.len == 0) {
        *err = lp_chk_stat(a, file);
        return file;
    }
    if (lp_has_ext(file) && BURROW_OK(lp_chk_stat(a, file)))
        return file;
    for (Int i = 0; i < exts.len; i++) {
        Str f = lp_cat(a, file, BURROW_AT(Str, exts, i));
        if (f.len > 0 && BURROW_OK(lp_chk_stat(a, f)))
            return f;
    }
    *err = lp_has_ext(file) ? fs_err_not_exist : exec_err_not_found;
    return BURROW_STR_EMPTY;
}

/* pathExt: %PATHEXT% lowered and split on ';', each with a dot in front, or
 * Go's four when it is not set. */
static Slice lp_path_ext(Alloc *a) {
    Str x = os_getenv(a, LP_LIT("PATHEXT"));
    Slice exts = slice_make(a, TYPE_STRING, 0, 4);
    if (x.len == 0) {
        static const char *const defaults[] = {".com", ".exe", ".bat", ".cmd"};
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
            exts = BURROW_APPEND(Str, a, exts, str_from_cstr(defaults[i]));
        return exts;
    }
    Slice parts = strings_split(a, strings_to_lower(a, x), LP_LIT(";"));
    for (Int i = 0; i < parts.len; i++) {
        Str e = BURROW_AT(Str, parts, i);
        if (e.len == 0)
            continue;
        if (e.p[0] != '.')
            e = lp_cat(a, LP_LIT("."), e);
        exts = BURROW_APPEND(Str, a, exts, e);
    }
    return exts;
}

static bool lp_same_file(Alloc *a, Str x, Str y) {
    Error e1 = BURROW_NO_ERROR, e2 = BURROW_NO_ERROR;
    FsFileInfo fx = os_lstat(a, x, &e1);
    FsFileInfo fy = os_lstat(a, y, &e2);
    return BURROW_OK(e1) && BURROW_OK(e2) && os_same_file(fx, fy);
}

/* lookPathExts. The answer is in s, the scratch allocator. */
static Str lp_look_path_exts(Alloc *s, Str file, Slice exts, Error *err) {
    *err = BURROW_NO_ERROR;
    Error e = BURROW_NO_ERROR;
    if (strings_contains_any(file, LP_LIT(":\\/"))) {
        Str f = lp_find_executable(s, file, exts, &e);
        if (BURROW_OK(e))
            return f;
        *err = burrow__exec_error(file, e);
        return BURROW_STR_EMPTY;
    }

    /* cmd.exe looks in the current directory before %PATH%, and so does
     * Go unless NoDefaultCurrentDirectoryInExePath is set. What it finds
     * there is only used when the same file is first in %PATH% too. */
    Str dotf = BURROW_STR_EMPTY;
    Error dot_err = BURROW_NO_ERROR;
    bool found = false;
    (void)os_lookup_env(s, LP_LIT("NoDefaultCurrentDirectoryInExePath"), &found);
    if (!found) {
        Str f =
            lp_find_executable(s, filepath_join_v(s, 2, LP_LIT("."), file), exts, &e);
        if (BURROW_OK(e)) {
            dotf = f;
            dot_err = burrow__exec_error(file, exec_err_dot);
        }
    }

    Slice dirs = filepath_split_list(s, os_getenv(s, LP_LIT("path")));
    for (Int i = 0; i < dirs.len; i++) {
        Str dir = BURROW_AT(Str, dirs, i);
        if (dir.len == 0)
            continue;
        Str f = lp_find_executable(s, filepath_join_v(s, 2, dir, file), exts, &e);
        if (BURROW_FAILED(e))
            continue;
        if (BURROW_FAILED(dot_err) && !lp_same_file(s, dotf, f)) {
            /* The one in the current directory would have been run before
             * Go 1.19, so that is the one to report. */
            *err = dot_err;
            return dotf;
        }
        if (!filepath_is_abs(f)) {
            if (BURROW_OK(dot_err)) {
                dotf = f;
                dot_err = burrow__exec_error(file, exec_err_dot);
            }
            continue;
        }
        return f;
    }
    if (BURROW_FAILED(dot_err)) {
        *err = dot_err;
        return dotf;
    }
    *err = burrow__exec_error(file, exec_err_not_found);
    return BURROW_STR_EMPTY;
}

static Str lp_look_path(Alloc *a, Arena *scratch, Str file, Error *err) {
    Alloc *s = arena_allocator(scratch);
    Error e = lp_validate(file);
    if (BURROW_FAILED(e)) {
        *err = burrow__exec_error(file, e);
        return BURROW_STR_EMPTY;
    }
    Str f = lp_look_path_exts(s, file, lp_path_ext(s), err);
    if (f.len == 0)
        return f;
    Error ke = BURROW_NO_ERROR;
    Str out = lp_keep(a, f, &ke);
    if (BURROW_FAILED(ke))
        *err = ke;
    return out;
}

/* lookExtensions: path with the extension Start would run it with, looking
 * in dir when path is relative. */
Str burrow__exec_look_extensions(Alloc *a, Str path, Str dir, Error *err) {
    *err = BURROW_NO_ERROR;
    Error e = lp_validate(path);
    if (BURROW_FAILED(e)) {
        *err = burrow__exec_error(path, e);
        return BURROW_STR_EMPTY;
    }
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *s = arena_allocator(&scratch);
    if (str_eq(filepath_base(path), path))
        path = filepath_join_v(s, 2, LP_LIT("."), path);
    Slice exts = lp_path_ext(s);
    Str ext = filepath_ext(path);
    Str out = BURROW_STR_EMPTY;
    bool done = false;
    if (ext.len > 0) {
        for (Int i = 0; i < exts.len && !done; i++)
            done = strings_equal_fold(ext, BURROW_AT(Str, exts, i));
        if (done)
            out = path;
    }
    if (!done) {
        /* Go's filepath.Join(".", p) cleans the "./" off again, so it puts
         * the dot back with a plain concatenation. */
        if (str_eq(filepath_base(path), path))
            path = lp_cat(s, LP_LIT(".\\"), path);
        if (dir.len == 0 || filepath_volume_name(s, path).len > 0 ||
            (path.len > 1 && os_is_path_separator(path.p[0]))) {
            out = lp_look_path_exts(s, path, exts, err);
        } else {
            Str dirandpath = filepath_join_v(s, 2, dir, path);
            Str lp = lp_look_path_exts(s, dirandpath, exts, err);
            if (BURROW_OK(*err))
                out = lp_cat(s, path, strings_trim_prefix(lp, dirandpath));
        }
    }
    if (BURROW_OK(*err))
        out = lp_keep(a, out, err);
    else
        out = BURROW_STR_EMPTY;
    *err = error_retain(error_allocator(), *err);
    arena_free(&scratch);
    return out;
}

#endif

Str exec_look_path(Alloc *a, Str file, Error *err) {
    *err = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Str out = lp_look_path(a, &scratch, file, err);
    /* The errors were made from the error arena, but the strings in them may
     * point into the scratch arena. */
    *err = error_retain(error_allocator(), *err);
    arena_free(&scratch);
    return out;
}
