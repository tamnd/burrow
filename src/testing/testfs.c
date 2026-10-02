/* Derived from Go's src/testing/fstest/testfs.go. See
 * include/burrow/testing/fstest.h.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/fstest.h"

#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/path.h"
#include "burrow/slices.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/testing/iotest.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

#define TFS_LIT(s) ((Str){(const Byte *)(s), (Int)sizeof(s) - 1})

/* Go's fsTester. Everything it allocates comes from a, an arena that goes
 * away when the test is over, and the errors it collects are copied out of it
 * first. */
typedef struct FsTester {
    Fs fsys;
    Alloc *a;
    Slice errors; /* of Error */
    Slice dirs;   /* of Str */
    Slice files;  /* of Str */
} FsTester;

static void tfs_add(FsTester *t, Error e) {
    t->errors = BURROW_APPEND(Error, t->a, t->errors, e);
}

#define TFS_ERRORF(t, ...) tfs_add((t), fmt_errorf_v(__VA_ARGS__))

/* Go compares with ==, and an Error is two words. */
static bool tfs_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

static Error tfs_close(FsFile f) {
    return f.vt->read_closer.closer.close(f.data);
}

static FsFile tfs_open(FsTester *t, Str name, Error *err) {
    return t->fsys.vt->open(t->fsys.data, t->a, name, err);
}

static Str tfs_cat(FsTester *t, Str x, Str y) {
    return fmt_sprintf_v(t->a, "%s%s", x, y);
}

static Str tfs_cat3(FsTester *t, Str x, Str y, Str z) {
    return fmt_sprintf_v(t->a, "%s%s%s", x, y, z);
}

static Str tfs_slice_str(Str s, Int lo, Int hi) {
    return str_from_bytes(s.p + lo, hi - lo);
}

static Str tfs_bytes_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static Str tfs_entry_name(FsDirEntry e) {
    return e.vt->name(e.data);
}

/* What Go's %T prints for the file Open gave back, which is a pointer to a
 * struct in every FS there is. */
static Str tfs_type_name(FsTester *t, FsFile f) {
    const Type *ty = f.vt->read_closer.reader.self_type;
    if (ty == NULL)
        return TFS_LIT("<nil>");
    Str pkg = ty->pkg_path;
    Int i = strings_last_index(pkg, TFS_LIT("/"));
    if (i >= 0)
        pkg = tfs_slice_str(pkg, i + 1, pkg.len);
    if (pkg.len == 0)
        return fmt_sprintf_v(t->a, "*%s", ty->name);
    return fmt_sprintf_v(t->a, "*%s.%s", pkg, ty->name);
}

/* formatEntry, formatInfoEntry and formatInfo. */
static Str tfs_format_entry(FsTester *t, FsDirEntry e) {
    return fmt_sprintf_v(t->a, "%s IsDir=%v Type=%s", e.vt->name(e.data),
                         e.vt->is_dir(e.data),
                         fs_file_mode_string(e.vt->type(e.data), t->a));
}

static Str tfs_format_info_entry(FsTester *t, FsFileInfo info) {
    return fmt_sprintf_v(
        t->a, "%s IsDir=%v Type=%s", info.vt->name(info.data),
        info.vt->is_dir(info.data),
        fs_file_mode_string(fs_file_mode_type(info.vt->mode(info.data)), t->a));
}

static Str tfs_format_info(FsTester *t, FsFileInfo info) {
    return fmt_sprintf_v(
        t->a, "%s IsDir=%v Mode=%s Size=%d ModTime=%s", info.vt->name(info.data),
        info.vt->is_dir(info.data), fs_file_mode_string(info.vt->mode(info.data), t->a),
        info.vt->size(info.data), time_string(info.vt->mod_time(info.data), t->a));
}

static void tfs_check_dir(FsTester *t, Str dir);
static void tfs_check_stat(FsTester *t, Str path, FsDirEntry entry);
static void tfs_check_open(FsTester *t, Str file);
static void tfs_check_file(FsTester *t, Str file);
static void tfs_check_glob(FsTester *t, Str dir, Slice list);
static void tfs_check_dir_list(FsTester *t, Str dir, Str desc, Slice list1,
                               Slice list2);

static bool tfs_open_dir(FsTester *t, Str dir, FsFile *out) {
    Error err = BURROW_NO_ERROR;
    FsFile f = tfs_open(t, dir, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: Open: %w", dir, err);
        return false;
    }
    if (f.vt->read_dir == NULL) {
        (void)tfs_close(f);
        TFS_ERRORF(t, "%s: Open returned File type %s, not a fs.ReadDirFile", dir,
                   tfs_type_name(t, f));
        return false;
    }
    *out = f;
    return true;
}

/* checkDir checks the directory dir, which is expected to exist (it is either
 * the root or was found in a directory listing with IsDir true). */
static void tfs_check_dir(FsTester *t, Str dir) {
    /* Read entire directory. */
    t->dirs = BURROW_APPEND(Str, t->a, t->dirs, dir);
    FsFile d;
    if (!tfs_open_dir(t, dir, &d))
        return;
    Error err = BURROW_NO_ERROR;
    Slice list = d.vt->read_dir(d.data, t->a, -1, &err);
    if (BURROW_FAILED(err)) {
        (void)tfs_close(d);
        TFS_ERRORF(t, "%s: ReadDir(-1): %w", dir, err);
        return;
    }

    /* Check all children. */
    Str prefix =
        str_eq(dir, TFS_LIT(".")) ? BURROW_STR_EMPTY : tfs_cat(t, dir, TFS_LIT("/"));
    for (Int i = 0; i < list.len; i++) {
        FsDirEntry info = BURROW_AT(FsDirEntry, list, i);
        Str name = tfs_entry_name(info);
        if (str_eq(name, TFS_LIT(".")) || str_eq(name, TFS_LIT("..")) ||
            name.len == 0) {
            TFS_ERRORF(t, "%s: ReadDir: child has invalid name: %#q", dir, name);
            continue;
        }
        if (strings_index_byte(name, '/') >= 0) {
            TFS_ERRORF(t, "%s: ReadDir: child name contains slash: %#q", dir, name);
            continue;
        }
        if (strings_index_byte(name, '\\') >= 0) {
            TFS_ERRORF(t, "%s: ReadDir: child name contains backslash: %#q", dir, name);
            continue;
        }
        Str path = tfs_cat(t, prefix, name);
        tfs_check_stat(t, path, info);
        tfs_check_open(t, path);
        FsFileMode ty = info.vt->type(info.data);
        if (ty == FS_MODE_DIR)
            tfs_check_dir(t, path);
        else if (ty == FS_MODE_SYMLINK)
            /* No further processing. Avoid following symlinks to avoid
             * potentially unbounded recursion. */
            t->files = BURROW_APPEND(Str, t->a, t->files, path);
        else
            tfs_check_file(t, path);
    }

    /* Check ReadDir(-1) at EOF. */
    Slice list2 = d.vt->read_dir(d.data, t->a, -1, &err);
    if (list2.len > 0 || BURROW_FAILED(err)) {
        (void)tfs_close(d);
        TFS_ERRORF(t, "%s: ReadDir(-1) at EOF = %d entries, %w, wanted 0 entries, nil",
                   dir, list2.len, err);
        return;
    }

    /* Check ReadDir(1) at EOF (different results). */
    list2 = d.vt->read_dir(d.data, t->a, 1, &err);
    if (list2.len > 0 || !tfs_is_eof(err)) {
        (void)tfs_close(d);
        TFS_ERRORF(t, "%s: ReadDir(1) at EOF = %d entries, %w, wanted 0 entries, EOF",
                   dir, list2.len, err);
        return;
    }

    /* Check that close does not report an error. */
    err = tfs_close(d);
    if (BURROW_FAILED(err))
        TFS_ERRORF(t, "%s: Close: %w", dir, err);

    /* Check that closing twice doesn't crash. The return value doesn't
     * matter. */
    (void)tfs_close(d);

    /* Reopen directory, read a second time, make sure contents match. */
    if (!tfs_open_dir(t, dir, &d))
        return;
    list2 = d.vt->read_dir(d.data, t->a, -1, &err);
    if (BURROW_FAILED(err)) {
        (void)tfs_close(d);
        TFS_ERRORF(t, "%s: second Open+ReadDir(-1): %w", dir, err);
        return;
    }
    tfs_check_dir_list(t, dir,
                       TFS_LIT("first Open+ReadDir(-1) vs second Open+ReadDir(-1)"),
                       list, list2);

    /* Reopen directory, read a third time in pieces, make sure contents
     * match. Go defers both closes to the end, and so does this. */
    FsFile d2;
    if (!tfs_open_dir(t, dir, &d2)) {
        (void)tfs_close(d);
        return;
    }
    list2 = slice_nil(TYPE_FS_DIR_ENTRY);
    for (;;) {
        Int n = 1;
        if (list2.len > 0)
            n = 2;
        Slice frag = d2.vt->read_dir(d2.data, t->a, n, &err);
        if (frag.len > n) {
            TFS_ERRORF(t, "%s: third Open: ReadDir(%d) after %d: %d entries (too many)",
                       dir, n, list2.len, frag.len);
            goto out;
        }
        for (Int i = 0; i < frag.len; i++)
            list2 =
                BURROW_APPEND(FsDirEntry, t->a, list2, BURROW_AT(FsDirEntry, frag, i));
        if (tfs_is_eof(err))
            break;
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: third Open: ReadDir(%d) after %d: %w", dir, n, list2.len,
                       err);
            goto out;
        }
        if (n == 0) {
            TFS_ERRORF(t,
                       "%s: third Open: ReadDir(%d) after %d: 0 entries but nil error",
                       dir, n, list2.len);
            goto out;
        }
    }
    tfs_check_dir_list(
        t, dir, TFS_LIT("first Open+ReadDir(-1) vs third Open+ReadDir(1,2) loop"), list,
        list2);

    /* If fsys has ReadDir, check that it matches and is sorted. */
    if (t->fsys.vt->read_dir != NULL) {
        list2 = t->fsys.vt->read_dir(t->fsys.data, t->a, dir, &err);
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: fsys.ReadDir: %w", dir, err);
            goto out;
        }
        tfs_check_dir_list(t, dir, TFS_LIT("first Open+ReadDir(-1) vs fsys.ReadDir"),
                           list, list2);
        for (Int i = 0; i + 1 < list2.len; i++) {
            Str x = tfs_entry_name(BURROW_AT(FsDirEntry, list2, i));
            Str y = tfs_entry_name(BURROW_AT(FsDirEntry, list2, i + 1));
            if (strings_compare(x, y) >= 0)
                TFS_ERRORF(t, "%s: fsys.ReadDir: list not sorted: %s before %s", dir, x,
                           y);
        }
    }

    /* Check fs.ReadDir as well. */
    list2 = fs_read_dir(t->a, t->fsys, dir, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: fs.ReadDir: %w", dir, err);
        goto out;
    }
    tfs_check_dir_list(t, dir, TFS_LIT("first Open+ReadDir(-1) vs fs.ReadDir"), list,
                       list2);
    for (Int i = 0; i + 1 < list2.len; i++) {
        Str x = tfs_entry_name(BURROW_AT(FsDirEntry, list2, i));
        Str y = tfs_entry_name(BURROW_AT(FsDirEntry, list2, i + 1));
        if (strings_compare(x, y) >= 0)
            TFS_ERRORF(t, "%s: fs.ReadDir: list not sorted: %s before %s", dir, x, y);
    }

    tfs_check_glob(t, dir, list2);

out:
    (void)tfs_close(d2);
    (void)tfs_close(d);
}

static bool tfs_str_slices_equal(Slice x, Slice y) {
    if (x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++)
        if (!str_eq(BURROW_AT(Str, x, i), BURROW_AT(Str, y, i)))
            return false;
    return true;
}

/* checkGlob checks that various glob patterns work if the file system
 * implements GlobFS. */
static void tfs_check_glob(FsTester *t, Str dir, Slice list) {
    if (t->fsys.vt->glob == NULL)
        return;

    /* Make a complex glob pattern prefix that only matches dir. */
    Str glob = BURROW_STR_EMPTY;
    if (!str_eq(dir, TFS_LIT("."))) {
        Slice elem = strings_split(t->a, dir, TFS_LIT("/"));
        for (Int i = 0; i < elem.len; i++) {
            Str e = BURROW_AT(Str, elem, i);
            StringsBuilder pattern = STRINGS_BUILDER(t->a);
            for (Int j = 0; j < e.len;) {
                Int size = 0;
                Rune r = utf8_decode_rune_in_string(tfs_slice_str(e, j, e.len), &size);
                if (r == '*' || r == '?' || r == '\\' || r == '[' || r == '-') {
                    (void)strings_builder_write_byte(&pattern, '\\');
                    strings_builder_write_rune(&pattern, r, NULL);
                    j += size;
                    continue;
                }
                switch ((i + j) % 5) {
                case 0:
                    strings_builder_write_rune(&pattern, r, NULL);
                    break;
                case 1:
                    (void)strings_builder_write_byte(&pattern, '[');
                    strings_builder_write_rune(&pattern, r, NULL);
                    (void)strings_builder_write_byte(&pattern, ']');
                    break;
                case 2:
                    (void)strings_builder_write_byte(&pattern, '[');
                    strings_builder_write_rune(&pattern, r, NULL);
                    (void)strings_builder_write_byte(&pattern, '-');
                    strings_builder_write_rune(&pattern, r, NULL);
                    (void)strings_builder_write_byte(&pattern, ']');
                    break;
                case 3:
                    strings_builder_write_string(&pattern, TFS_LIT("[\\"), NULL);
                    strings_builder_write_rune(&pattern, r, NULL);
                    (void)strings_builder_write_byte(&pattern, ']');
                    break;
                default:
                    strings_builder_write_string(&pattern, TFS_LIT("[\\"), NULL);
                    strings_builder_write_rune(&pattern, r, NULL);
                    strings_builder_write_string(&pattern, TFS_LIT("-\\"), NULL);
                    strings_builder_write_rune(&pattern, r, NULL);
                    (void)strings_builder_write_byte(&pattern, ']');
                    break;
                }
                j += size;
            }
            BURROW_AT(Str, elem, i) = strings_builder_string(&pattern);
        }
        glob = tfs_cat(t, strings_join(t->a, elem, TFS_LIT("/")), TFS_LIT("/"));
    }

    /* Test that malformed patterns are detected. The error is likely
     * path_err_bad_pattern but need not be. */
    Error err = BURROW_NO_ERROR;
    Str bad = tfs_cat(t, glob, TFS_LIT("nonexist/[]"));
    (void)t->fsys.vt->glob(t->fsys.data, t->a, bad, &err);
    if (BURROW_OK(err))
        TFS_ERRORF(t, "%s: Glob(%#q): bad pattern not detected", dir, bad);

    /* Try to find a letter that appears in only some of the final names. */
    Rune c = 'a';
    for (; c <= 'z'; c++) {
        bool have = false, have_not = false;
        for (Int i = 0; i < list.len; i++) {
            if (strings_contains_rune(tfs_entry_name(BURROW_AT(FsDirEntry, list, i)),
                                      c))
                have = true;
            else
                have_not = true;
        }
        if (have && have_not)
            break;
    }
    if (c > 'z')
        c = 'a';
    glob = fmt_sprintf_v(t->a, "%s*%c*", glob, c);

    Slice want = slice_nil(TYPE_STRING);
    for (Int i = 0; i < list.len; i++) {
        Str name = tfs_entry_name(BURROW_AT(FsDirEntry, list, i));
        if (strings_contains_rune(name, c))
            want = BURROW_APPEND(Str, t->a, want, path_join_v(t->a, 2, dir, name));
    }

    Slice names = t->fsys.vt->glob(t->fsys.data, t->a, glob, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: Glob(%#q): %w", dir, glob, err);
        return;
    }
    if (tfs_str_slices_equal(want, names))
        return;

    if (!sort_strings_are_sorted(names)) {
        TFS_ERRORF(t, "%s: Glob(%#q): unsorted output:\n%s", dir, glob,
                   strings_join(t->a, names, TFS_LIT("\n")));
        sort_strings(names);
    }

    Slice problems = slice_nil(TYPE_STRING);
    Int wi = 0, ni = 0;
    while (wi < want.len || ni < names.len) {
        Str w = wi < want.len ? BURROW_AT(Str, want, wi) : BURROW_STR_EMPTY;
        Str n = ni < names.len ? BURROW_AT(Str, names, ni) : BURROW_STR_EMPTY;
        if (wi < want.len && ni < names.len && str_eq(w, n)) {
            wi++;
            ni++;
        } else if (wi < want.len && (ni == names.len || strings_compare(w, n) < 0)) {
            problems =
                BURROW_APPEND(Str, t->a, problems, tfs_cat(t, TFS_LIT("missing: "), w));
            wi++;
        } else {
            problems =
                BURROW_APPEND(Str, t->a, problems, tfs_cat(t, TFS_LIT("extra: "), n));
            ni++;
        }
    }
    TFS_ERRORF(t, "%s: Glob(%#q): wrong output:\n%s", dir, glob,
               strings_join(t->a, problems, TFS_LIT("\n")));
}

/* checkStat checks that a direct stat of path matches entry, which was found
 * in the parent's directory listing. */
static void tfs_check_stat(FsTester *t, Str path, FsDirEntry entry) {
    Error err = BURROW_NO_ERROR;
    FsFile file = tfs_open(t, path, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: Open: %w", path, err);
        return;
    }
    FsFileInfo info = file.vt->stat(file.data, t->a, &err);
    (void)tfs_close(file);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: Stat: %w", path, err);
        return;
    }
    bool symlink = (entry.vt->type(entry.data) & FS_MODE_SYMLINK) != 0;
    Str fentry = tfs_format_entry(t, entry);
    Str fientry = tfs_format_info_entry(t, info);
    /* Note: mismatch here is OK for symlink, because Open dereferences
     * symlink. */
    if (!str_eq(fentry, fientry) && !symlink)
        TFS_ERRORF(t, "%s: mismatch:\n\tentry = %s\n\tfile.Stat() = %s", path, fentry,
                   fientry);

    FsFileInfo einfo = entry.vt->info(entry.data, t->a, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: entry.Info: %w", path, err);
        return;
    }
    Str finfo = tfs_format_info(t, info);
    if (symlink) {
        /* For symlink, just check that entry.Info matches entry on common
         * fields. Open deferences symlink, so info itself may differ. */
        Str feentry = tfs_format_info_entry(t, einfo);
        if (!str_eq(fentry, feentry))
            TFS_ERRORF(t, "%s: mismatch\n\tentry = %s\n\tentry.Info() = %s\n", path,
                       fentry, feentry);
    } else {
        Str feinfo = tfs_format_info(t, einfo);
        if (!str_eq(feinfo, finfo))
            TFS_ERRORF(t, "%s: mismatch:\n\tentry.Info() = %s\n\tfile.Stat() = %s\n",
                       path, feinfo, finfo);
    }

    /* Stat should be the same as Open+Stat, even for symlinks. */
    FsFileInfo info2 = fs_stat(t->a, t->fsys, path, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: fs.Stat: %w", path, err);
        return;
    }
    Str finfo2 = tfs_format_info(t, info2);
    if (!str_eq(finfo2, finfo))
        TFS_ERRORF(t, "%s: fs.Stat(...) = %s\n\twant %s", path, finfo2, finfo);

    if (t->fsys.vt->stat != NULL) {
        info2 = t->fsys.vt->stat(t->fsys.data, t->a, path, &err);
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: fsys.Stat: %w", path, err);
            return;
        }
        finfo2 = tfs_format_info(t, info2);
        if (!str_eq(finfo2, finfo))
            TFS_ERRORF(t, "%s: fsys.Stat(...) = %s\n\twant %s", path, finfo2, finfo);
    }

    if (t->fsys.vt->lstat != NULL) {
        info2 = t->fsys.vt->lstat(t->fsys.data, t->a, path, &err);
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: fsys.Lstat: %v", path, err);
            return;
        }
        Str fientry2 = tfs_format_info_entry(t, info2);
        if (!str_eq(fentry, fientry2))
            TFS_ERRORF(t, "%s: mismatch:\n\tentry = %s\n\tfsys.Lstat(...) = %s", path,
                       fentry, fientry2);
        Str feinfo = tfs_format_info(t, einfo);
        finfo2 = tfs_format_info(t, info2);
        if (!str_eq(feinfo, finfo2))
            TFS_ERRORF(t,
                       "%s: mismatch:\n\tentry.Info() = %s\n\tfsys.Lstat(...) = %s\n",
                       path, feinfo, finfo2);
    }
}

static void tfs_check_mode(FsTester *t, Str dir, FsDirEntry entry) {
    bool is_dir = entry.vt->is_dir(entry.data);
    if (is_dir != ((entry.vt->type(entry.data) & FS_MODE_DIR) != 0)) {
        if (is_dir)
            TFS_ERRORF(
                t, "%s: ReadDir returned %s with IsDir() = true, Type() & ModeDir = 0",
                dir, tfs_entry_name(entry));
        else
            TFS_ERRORF(t,
                       "%s: ReadDir returned %s with IsDir() = false, Type() & ModeDir "
                       "= ModeDir",
                       dir, tfs_entry_name(entry));
    }
}

/* The order Go sorts the diff lines in: by name, and then "+" before "-". */
static int tfs_diff_cmp(void *env, const void *pa, const void *pb) {
    FsTester *t = (FsTester *)env;
    Slice fa = strings_fields(t->a, *(const Str *)pa);
    Slice fb = strings_fields(t->a, *(const Str *)pb);
    Str x = tfs_cat3(t, BURROW_AT(Str, fa, 1), TFS_LIT(" "), BURROW_AT(Str, fb, 0));
    Str y = tfs_cat3(t, BURROW_AT(Str, fb, 1), TFS_LIT(" "), BURROW_AT(Str, fa, 0));
    return (int)strings_compare(x, y);
}

/* checkDirList checks that two directory lists contain the same files and file
 * info. The order of the lists need not match.
 *
 * Go keeps the first list in a map by name. Here it is the list itself with a
 * flag for each entry, and an entry with the same name as a later one is
 * dropped the way the map would overwrite it. */
static void tfs_check_dir_list(FsTester *t, Str dir, Str desc, Slice list1,
                               Slice list2) {
    bool *live = BURROW_NEW_N(t->a, bool, (size_t)(list1.len > 0 ? list1.len : 1));
    if (live == NULL)
        return;
    for (Int i = 0; i < list1.len; i++) {
        FsDirEntry e1 = BURROW_AT(FsDirEntry, list1, i);
        Str name = tfs_entry_name(e1);
        for (Int j = 0; j < i; j++)
            if (live[j] &&
                str_eq(tfs_entry_name(BURROW_AT(FsDirEntry, list1, j)), name))
                live[j] = false;
        live[i] = true;
        tfs_check_mode(t, dir, e1);
    }

    Slice diffs = slice_nil(TYPE_STRING);
    for (Int k = 0; k < list2.len; k++) {
        FsDirEntry e2 = BURROW_AT(FsDirEntry, list2, k);
        Str name = tfs_entry_name(e2);
        Int found = -1;
        for (Int i = 0; i < list1.len; i++)
            if (live[i] &&
                str_eq(tfs_entry_name(BURROW_AT(FsDirEntry, list1, i)), name)) {
                found = i;
                break;
            }
        if (found < 0) {
            tfs_check_mode(t, dir, e2);
            diffs = BURROW_APPEND(Str, t->a, diffs,
                                  tfs_cat(t, TFS_LIT("+ "), tfs_format_entry(t, e2)));
            continue;
        }
        FsDirEntry e1 = BURROW_AT(FsDirEntry, list1, found);
        Str f1 = tfs_format_entry(t, e1);
        Str f2 = tfs_format_entry(t, e2);
        if (!str_eq(f1, f2)) {
            diffs = BURROW_APPEND(Str, t->a, diffs, tfs_cat(t, TFS_LIT("- "), f1));
            diffs = BURROW_APPEND(Str, t->a, diffs, tfs_cat(t, TFS_LIT("+ "), f2));
        }
        live[found] = false;
    }
    for (Int i = 0; i < list1.len; i++)
        if (live[i])
            diffs = BURROW_APPEND(
                Str, t->a, diffs,
                tfs_cat(t, TFS_LIT("- "),
                        tfs_format_entry(t, BURROW_AT(FsDirEntry, list1, i))));

    if (diffs.len == 0)
        return;

    slices_sort_func(diffs, BURROW_FN(SlicesCmpFunc, tfs_diff_cmp, t));
    TFS_ERRORF(t, "%s: diff %s:\n\t%s", dir, desc,
               strings_join(t->a, diffs, TFS_LIT("\n\t")));
}

static void tfs_check_file_read(FsTester *t, Str file, Str desc, Slice data1,
                                Slice data2) {
    if (!str_eq(tfs_bytes_str(data1), tfs_bytes_str(data2)))
        TFS_ERRORF(t, "%s: %s: different data returned\n\t%q\n\t%q", file, desc,
                   tfs_bytes_str(data1), tfs_bytes_str(data2));
}

/* The two ways checkBadPath opens a name: Open, closing what it gets, and
 * ReadFile. */
typedef enum { TFS_OPEN, TFS_READ_FILE } TfsOpenKind;

static Error tfs_try(FsTester *t, TfsOpenKind kind, Str name) {
    Error err = BURROW_NO_ERROR;
    if (kind == TFS_READ_FILE) {
        (void)t->fsys.vt->read_file(t->fsys.data, t->a, name, &err);
        return err;
    }
    FsFile f = tfs_open(t, name, &err);
    if (BURROW_OK(err))
        (void)tfs_close(f);
    return err;
}

/* checkBadPath checks that various invalid forms of file's name cannot be
 * opened. */
static void tfs_check_bad_path(FsTester *t, Str file, Str desc, TfsOpenKind kind) {
    Str bad[12];
    Int n = 0;
    bad[n++] = tfs_cat(t, TFS_LIT("/"), file);
    bad[n++] = tfs_cat(t, file, TFS_LIT("/."));
    if (str_eq(file, TFS_LIT(".")))
        bad[n++] = TFS_LIT("/");
    Int i = strings_index_byte(file, '/');
    if (i >= 0) {
        Str before = tfs_slice_str(file, 0, i),
            after = tfs_slice_str(file, i + 1, file.len);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("//"), after);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("/./"), after);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("\\"), after);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("/../"), file);
    }
    i = strings_last_index(file, TFS_LIT("/"));
    if (i >= 0) {
        Str before = tfs_slice_str(file, 0, i),
            after = tfs_slice_str(file, i + 1, file.len);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("//"), after);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("/./"), after);
        bad[n++] = tfs_cat3(t, before, TFS_LIT("\\"), after);
        bad[n++] = tfs_cat3(t, file, TFS_LIT("/../"), after);
    }

    for (Int k = 0; k < n; k++)
        if (BURROW_OK(tfs_try(t, kind, bad[k])))
            TFS_ERRORF(t, "%s: %s(%s) succeeded, want error", file, desc, bad[k]);
}

/* checkOpen validates file opening behavior by attempting to open and then
 * close the given file path. */
static void tfs_check_open(FsTester *t, Str file) {
    tfs_check_bad_path(t, file, TFS_LIT("Open"), TFS_OPEN);
}

/* checkFile checks that basic file reading works correctly. */
static void tfs_check_file(FsTester *t, Str file) {
    t->files = BURROW_APPEND(Str, t->a, t->files, file);

    /* Read entire file. */
    Error err = BURROW_NO_ERROR;
    FsFile f = tfs_open(t, file, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: Open: %w", file, err);
        return;
    }

    Slice data = io_read_all(t->a, fs_file_as_io_reader(f), &err);
    if (BURROW_FAILED(err)) {
        (void)tfs_close(f);
        TFS_ERRORF(t, "%s: Open+ReadAll: %w", file, err);
        return;
    }

    err = tfs_close(f);
    if (BURROW_FAILED(err))
        TFS_ERRORF(t, "%s: Close: %w", file, err);

    /* Check that closing twice doesn't crash. The return value doesn't
     * matter. */
    (void)tfs_close(f);

    /* Check that ReadFile works if present. */
    if (t->fsys.vt->read_file != NULL) {
        Slice data2 = t->fsys.vt->read_file(t->fsys.data, t->a, file, &err);
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: fsys.ReadFile: %w", file, err);
            return;
        }
        tfs_check_file_read(t, file, TFS_LIT("ReadAll vs fsys.ReadFile"), data, data2);

        /* Modify the data and check it again. Modifying the returned byte
         * slice should not affect the next call. */
        for (Int i = 0; i < data2.len; i++)
            ((Byte *)data2.p)[i]++;
        data2 = t->fsys.vt->read_file(t->fsys.data, t->a, file, &err);
        if (BURROW_FAILED(err)) {
            TFS_ERRORF(t, "%s: second call to fsys.ReadFile: %w", file, err);
            return;
        }
        tfs_check_file_read(t, file, TFS_LIT("Readall vs second fsys.ReadFile"), data,
                            data2);

        tfs_check_bad_path(t, file, TFS_LIT("ReadFile"), TFS_READ_FILE);
    }

    /* Check that fs.ReadFile works with t.fsys. */
    Slice data2 = fs_read_file(t->a, t->fsys, file, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: fs.ReadFile: %w", file, err);
        return;
    }
    tfs_check_file_read(t, file, TFS_LIT("ReadAll vs fs.ReadFile"), data, data2);

    /* Use iotest_test_reader to check small reads, Seek, ReadAt. */
    f = tfs_open(t, file, &err);
    if (BURROW_FAILED(err)) {
        TFS_ERRORF(t, "%s: second Open: %w", file, err);
        return;
    }
    err = iotest_test_reader(fs_file_as_io_reader(f), data);
    if (BURROW_FAILED(err))
        TFS_ERRORF(
            t, "%s: failed TestReader:\n\t%s", file,
            strings_replace_all(t->a, error_text(err), TFS_LIT("\n"), TFS_LIT("\n\t")));
    (void)tfs_close(f);
}

static Error tfs_test_fs(Fs fsys, const Str *expected, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    FsTester tt = {fsys, arena_allocator(&ar), slice_nil(TYPE_ERROR),
                   slice_nil(TYPE_STRING), slice_nil(TYPE_STRING)};
    FsTester *t = &tt;
    tfs_check_dir(t, TFS_LIT("."));
    tfs_check_open(t, TFS_LIT("."));

    /* Go's found map, as a sorted list without "." or repeats. */
    Slice all = slice_nil(TYPE_STRING);
    for (Int i = 0; i < t->dirs.len; i++)
        all = BURROW_APPEND(Str, t->a, all, BURROW_AT(Str, t->dirs, i));
    for (Int i = 0; i < t->files.len; i++)
        all = BURROW_APPEND(Str, t->a, all, BURROW_AT(Str, t->files, i));
    sort_strings(all);
    Slice found = slice_nil(TYPE_STRING);
    for (Int i = 0; i < all.len; i++) {
        Str s = BURROW_AT(Str, all, i);
        if (str_eq(s, TFS_LIT(".")))
            continue;
        if (found.len > 0 && str_eq(BURROW_AT(Str, found, found.len - 1), s))
            continue;
        found = BURROW_APPEND(Str, t->a, found, s);
    }

    if (n == 0 && found.len > 0) {
        Slice list = found;
        if (list.len > 15) {
            list = slice_sub(list, 0, 10);
            list =
                BURROW_APPEND(Str, t->a, slice_sub3(list, 0, 10, 10), TFS_LIT("..."));
        }
        TFS_ERRORF(t, "expected empty file system but found files:\n%s",
                   strings_join(t->a, list, TFS_LIT("\n")));
    }
    for (Int i = 0; i < n; i++) {
        Int j = sort_search_strings(found, expected[i]);
        if (j >= found.len || !str_eq(BURROW_AT(Str, found, j), expected[i]))
            TFS_ERRORF(t, "expected but not found: %s", expected[i]);
    }

    Error ret = BURROW_NO_ERROR;
    if (t->errors.len > 0) {
        ret = fmt_errorf_v("TestFS found errors:\n%w",
                           errors_join(error_allocator(), t->errors));
        /* What the errors wrap came from the arena, so the copy has to be
         * made before it goes. */
        ret = error_retain(error_allocator(), ret);
    }
    arena_free(&ar);
    return ret;
}

Error fstest_test_fs(Fs fsys, Slice expected) {
    const Str *exp = (const Str *)expected.p;
    Int n = expected.len;
    Error err = tfs_test_fs(fsys, exp, n);
    if (BURROW_FAILED(err))
        return err;
    for (Int k = 0; k < n; k++) {
        Str name = exp[k];
        Int i = strings_index_byte(name, '/');
        if (i < 0)
            continue;
        Str dir = tfs_slice_str(name, 0, i);
        Str dir_slash = tfs_slice_str(name, 0, i + 1);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Slice sub_expected = slice_nil(TYPE_STRING);
        for (Int j = 0; j < n; j++)
            if (strings_has_prefix(exp[j], dir_slash))
                sub_expected =
                    BURROW_APPEND(Str, a, sub_expected,
                                  tfs_slice_str(exp[j], dir_slash.len, exp[j].len));
        Fs sub = fs_sub(a, fsys, dir, &err);
        if (BURROW_OK(err)) {
            err = tfs_test_fs(sub, (const Str *)sub_expected.p, sub_expected.len);
            if (BURROW_FAILED(err))
                err = fmt_errorf_v("testing fs.Sub(fsys, %s): %w", dir, err);
        }
        err = error_retain(error_allocator(), err);
        arena_free(&ar);
        return err; /* one sub-test is enough */
    }
    return BURROW_NO_ERROR;
}
