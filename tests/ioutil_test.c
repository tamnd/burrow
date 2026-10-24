/* Derived from Go's src/io/ioutil/ioutil_test.go, tempfile_test.go and
 * example_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests read their own source and the directory above it, which are the
 * package directory they run in. These run from the top of the tree, so they
 * read tests/ioutil_test.c and tests, and ExampleReadFile reads
 * tests/ioutiltest/testdata/hello.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io/ioutil.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/log.h"
#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"
#include "burrow/regexp.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include "check.h"

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#endif

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* ----------------------------------------------------------- ioutil_test.go */

static void check_size(TestingT *t, Str path, int64_t size) {
    Error err = BURROW_NO_ERROR;
    OsFileInfo dir = os_stat(a, path, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Stat %q (looking for size %d): %v", path, size, err);
    if (dir.vt->size(dir.data) != size)
        testing_t_errorf_v(t, "Stat %q: size %d want %d", path, dir.vt->size(dir.data),
                           size);
}

static void TestReadFile(TestingT *t) {
    Str filename = S("rumpelstilzchen");
    Error err = BURROW_NO_ERROR;
    Slice contents = ioutil_read_file(a, filename, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "ReadFile %s: error expected, none found", filename);

    filename = S("tests/ioutil_test.c");
    contents = ioutil_read_file(a, filename, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadFile %s: %v", filename, err);

    check_size(t, filename, (int64_t)contents.len);
}

static void TestWriteFile(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    OsFile *f = ioutil_temp_file(a, S(""), S("ioutil-test"), &err);
    if (f == NULL)
        testing_t_fatal_v(t, err);
    Str filename = os_file_name(f);
    Str data =
        S("Programming today is a race between software engineers striving to "
          "build bigger and better idiot-proof programs, and the Universe trying "
          "to produce bigger and better idiots. So far, the Universe is winning.");

    err = ioutil_write_file(filename, bytes_of(data), 0644);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WriteFile %s: %v", filename, err);

    Slice contents = ioutil_read_file(a, filename, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadFile %s: %v", filename, err);

    if (!str_eq(str_of(contents), data))
        testing_t_fatalf_v(t, "contents = %q\nexpected = %q", str_of(contents), data);

    /* cleanup */
    os_file_free(f);
    (void)os_remove(filename); /* ignore error */
}

static void TestReadOnlyWriteFile(TestingT *t) {
    if (os_getuid() == 0)
        testing_t_skipf_v(t, "Root can write to read-only files anyway, so skip the "
                             "read-only test.");
#if defined(BURROW_OS_WASI)
    testing_t_skip_v(t, "file permissions are not supported by wasip1");
#endif
#if defined(BURROW_OS_WINDOWS)
    /* Not in Go: under wine, run as root on the Linux host the suite runs on,
     * the second write succeeds. Wine keeps the read-only attribute in the
     * file's mode bits, and root can write past those, which is the case Go
     * skips above. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL)
        testing_t_skip_v(t, "wine may be running as root, which can write to "
                            "read-only files");
#endif

    /* We don't want to use TempFile directly, since that opens a file for us as
     * 0600. */
    Error err = BURROW_NO_ERROR;
    Str temp_dir = ioutil_temp_dir(a, S(""), testing_t_name(t), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "TempDir %s: %v", testing_t_name(t), err);
    Str filename = filepath_join_v(a, 2, temp_dir, S("blurp.txt"));

    Slice shmorp = bytes_of(S("shmorp"));
    Slice florp = bytes_of(S("florp"));
    err = ioutil_write_file(filename, shmorp, 0444);
    if (BURROW_FAILED(err)) {
        (void)os_remove_all(temp_dir);
        testing_t_fatalf_v(t, "WriteFile %s: %v", filename, err);
    }
    err = ioutil_write_file(filename, florp, 0444);
    if (BURROW_OK(err)) {
        (void)os_remove_all(temp_dir);
        testing_t_fatalf_v(t, "Expected an error when writing to read-only file %s",
                           filename);
    }
    Slice got = ioutil_read_file(a, filename, &err);
    if (BURROW_FAILED(err)) {
        (void)os_remove_all(temp_dir);
        testing_t_fatalf_v(t, "ReadFile %s: %v", filename, err);
    }
    if (!bytes_equal(got, shmorp)) {
        (void)os_remove_all(temp_dir);
        testing_t_fatalf_v(t, "want %s, got %s", str_of(shmorp), str_of(got));
    }
    (void)os_remove_all(temp_dir);
}

static void TestReadDir(TestingT *t) {
    Str dirname = S("rumpelstilzchen");
    Error err = BURROW_NO_ERROR;
    (void)ioutil_read_dir(a, dirname, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "ReadDir %s: error expected, none found", dirname);

    dirname = S("tests");
    Slice list = ioutil_read_dir(a, dirname, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadDir %s: %v", dirname, err);

    bool found_file = false;
    bool found_sub_dir = false;
    for (Int i = 0; i < list.len; i++) {
        const FsFileInfo *dir = (const FsFileInfo *)slice_at(list, i);
        Str name = dir->vt->name(dir->data);
        if (!dir->vt->is_dir(dir->data) && str_eq(name, S("io_test.c")))
            found_file = true;
        else if (dir->vt->is_dir(dir->data) && str_eq(name, S("ioutiltest")))
            found_sub_dir = true;
        /* Not in Go: the list is sorted by name. */
        if (i > 0) {
            const FsFileInfo *prev = (const FsFileInfo *)slice_at(list, i - 1);
            if (strings_compare(prev->vt->name(prev->data), name) >= 0)
                testing_t_errorf_v(t, "ReadDir %s: %s before %s", dirname,
                                   prev->vt->name(prev->data), name);
        }
    }
    if (!found_file)
        testing_t_fatalf_v(t, "ReadDir %s: io_test.c file not found", dirname);
    if (!found_sub_dir)
        testing_t_fatalf_v(t, "ReadDir %s: ioutiltest directory not found", dirname);
}

/* --------------------------------------------------------- tempfile_test.go */

static void TestTempFile(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str dir = ioutil_temp_dir(a, S(""), S("TestTempFile_BadDir"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    Str nonexistent_dir = filepath_join_v(a, 2, dir, S("_not_exists_"));
    OsFile *f = ioutil_temp_file(a, nonexistent_dir, S("foo"), &err);
    if (f != NULL || BURROW_OK(err)) {
        Str got = f != NULL ? os_file_name(f) : S("<nil>");
        testing_t_errorf_v(t, "TempFile(%q, `foo`) = %v, %v", nonexistent_dir, got,
                           err);
        os_file_free(f);
    }
    (void)os_remove_all(dir);
}

static void TestTempFile_pattern(TestingT *t) {
    static const struct {
        const char *pattern, *prefix, *suffix;
    } tests[] = {
        {"ioutil_test", "ioutil_test", ""},
        {"ioutil_test*", "ioutil_test", ""},
        {"ioutil_test*xyz", "ioutil_test", "xyz"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str pattern = str_from_cstr(tests[i].pattern);
        Str prefix = str_from_cstr(tests[i].prefix);
        Str suffix = str_from_cstr(tests[i].suffix);
        Error err = BURROW_NO_ERROR;
        OsFile *f = ioutil_temp_file(a, S(""), pattern, &err);
        if (f == NULL) {
            testing_t_errorf_v(t, "TempFile(..., %q) error: %v", pattern, err);
            continue;
        }
        Str name = os_file_name(f);
        Str base = filepath_base(name);
        os_file_free(f);
        if (!(strings_has_prefix(base, prefix) && strings_has_suffix(base, suffix)))
            testing_t_errorf_v(
                t,
                "TempFile pattern %q created bad name %q; want prefix %q "
                "& suffix %q",
                pattern, base, prefix, suffix);
        (void)os_remove(name);
    }
}

/* This string is from os.errPatternHasSeparator. */
#define PATTERN_HAS_SEPARATOR "pattern contains path separator"

typedef struct {
    Str tmp_dir;
    Str pattern;
    bool want_err;
    bool dir; /* TempDir rather than TempFile */
} BadPatternEnv;

static void bad_pattern(void *env, TestingT *t) {
    const BadPatternEnv *tt = env;
    Error err = BURROW_NO_ERROR;
    if (tt->dir) {
        (void)ioutil_temp_dir(a, tt->tmp_dir, tt->pattern, &err);
    } else {
        OsFile *tmpfile = ioutil_temp_file(a, tt->tmp_dir, tt->pattern, &err);
        os_file_free(tmpfile);
    }
    Str want = S(PATTERN_HAS_SEPARATOR);
    if (tt->want_err) {
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Expected an error for pattern %q", tt->pattern);
        else if (!strings_contains(error_text(err), want))
            testing_t_errorf_v(t, "Error mismatch: got %v, want %q for pattern %q", err,
                               want, tt->pattern);
    } else if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Unexpected error %v for pattern %q", err, tt->pattern);
    }
}

/* The table both BadPattern tests share, with sep for "/". */
static void run_bad_patterns(TestingT *t, bool dir) {
    Error err = BURROW_NO_ERROR;
    Str tmp_dir = ioutil_temp_dir(a, S(""), testing_t_name(t), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    static const struct {
        const char *pattern;
        bool want_err;
    } tests[] = {
        {"ioutil*test", false},     {"ioutil_test*foo", false},
        {"ioutil_test/foo", true},  {"ioutil_test*/foo", true},
        {"ioutil_test/*foo", true}, {"/ioutil_test/*foo", true},
        {"ioutil_test*foo/", true},
    };
    Str sep = str_from_bytes(&(Byte){(Byte)OS_PATH_SEPARATOR}, 1);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BadPatternEnv env = {
            tmp_dir,
            strings_replace_all(a, str_from_cstr(tests[i].pattern), S("/"), sep),
            tests[i].want_err, dir};
        (void)testing_t_run(t, env.pattern, BURROW_FN(TestingTFunc, bad_pattern, &env));
    }
    (void)os_remove_all(tmp_dir);
}

static void TestTempFile_BadPattern(TestingT *t) {
    run_bad_patterns(t, false);
}

typedef struct {
    Str dir;
    Str pattern;
    Str want_re_pat;
} TempDirEnv;

static void run_test_temp_dir(void *env, TestingT *t) {
    const TempDirEnv *e = env;
    Error err = BURROW_NO_ERROR;
    Str name = ioutil_temp_dir(a, e->dir, e->pattern, &err);
    if (name.len == 0 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "TempDir(dir, `ioutil_test`) = %v, %v", name, err);

    Regexp *re = regexp_must_compile(a, e->want_re_pat);
    if (!regexp_match_string(re, name))
        testing_t_errorf_v(t,
                           "TempDir(%q, %q) created bad name\n\t%q\ndid not match "
                           "pattern\n\t%q",
                           e->dir, e->pattern, name, e->want_re_pat);
    (void)os_remove(name);
}

static void TestTempDir(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str name = ioutil_temp_dir(a, S("/_not_exists_"), S("foo"), &err);
    if (name.len != 0 || BURROW_OK(err))
        testing_t_errorf_v(t, "TempDir(`/_not_exists_`, `foo`) = %v, %v", name, err);

    static const struct {
        const char *pattern, *want_prefix, *want_suffix;
    } tests[] = {
        {"ioutil_test", "ioutil_test", ""},
        {"ioutil_test*", "ioutil_test", ""},
        {"ioutil_test*xyz", "ioutil_test", "xyz"},
    };

    Str dir = os_temp_dir(a);

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str prefix = filepath_join_v(a, 2, dir, str_from_cstr(tests[i].want_prefix));
        Str want_re_pat =
            fmt_sprintf_v(a, "^%s[0-9]+%s$", regexp_quote_meta(a, prefix),
                          regexp_quote_meta(a, str_from_cstr(tests[i].want_suffix)));
        TempDirEnv env = {dir, str_from_cstr(tests[i].pattern), want_re_pat};
        (void)testing_t_run(t, env.pattern,
                            BURROW_FN(TestingTFunc, run_test_temp_dir, &env));
    }

    /* Separately testing "*xyz" (which has no prefix). That is when
     * constructing the pattern to assert on, as in the previous loop, using
     * filepath.Join for an empty prefix filepath.Join(dir, ""), produces the
     * pattern:
     *     ^<DIR>[0-9]+xyz$
     * yet we just want to match
     *     "^<DIR>/[0-9]+xyz" */
    Str sep = str_from_bytes(&(Byte){(Byte)OS_PATH_SEPARATOR}, 1);
    Str want_re_pat = fmt_sprintf_v(a, "^%s%s[0-9]+xyz$",
                                    regexp_quote_meta(a, filepath_join_v(a, 1, dir)),
                                    regexp_quote_meta(a, sep));
    TempDirEnv env = {dir, S("*xyz"), want_re_pat};
    (void)testing_t_run(t, S("*xyz"), BURROW_FN(TestingTFunc, run_test_temp_dir, &env));
}

/* test that we return a nice error message if the dir argument to TempDir
 * doesn't exist (or that it's empty and os.TempDir doesn't exist) */
static void TestTempDir_BadDir(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str dir = ioutil_temp_dir(a, S(""), S("TestTempDir_BadDir"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    Str bad_dir = filepath_join_v(a, 2, dir, S("not-exist"));
    (void)ioutil_temp_dir(a, bad_dir, S("foo"), &err);
    const FsPathError *pe = (const FsPathError *)errors_as(err, TYPE_FS_PATH_ERROR);
    if (pe == NULL || !os_is_not_exist(err) || !str_eq(pe->path, bad_dir))
        testing_t_errorf_v(t,
                           "TempDir error = %v; want PathError for path %q satisfying "
                           "os.IsNotExist",
                           err, bad_dir);
    (void)os_remove_all(dir);
}

static void TestTempDir_BadPattern(TestingT *t) {
    run_bad_patterns(t, true);
}

/* ---------------------------------------------------------- example_test.go */

static void ExampleReadAll(void) {
    StringsReader *r =
        strings_new_reader(a, S("Go is a general-purpose language designed "
                                "with systems programming in mind."));

    Error err = BURROW_NO_ERROR;
    Slice b = ioutil_read_all(a, strings_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    fmt_printf_v("%s", str_of(b));
}

static void ExampleReadDir(void) {
    Error err = BURROW_NO_ERROR;
    Slice files = ioutil_read_dir(a, S("."), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    for (Int i = 0; i < files.len; i++) {
        const FsFileInfo *file = (const FsFileInfo *)slice_at(files, i);
        fmt_println_v(file->vt->name(file->data));
    }
}

static void ExampleTempDir(void) {
    Slice content = bytes_of(S("temporary file's content"));
    Error err = BURROW_NO_ERROR;
    Str dir = ioutil_temp_dir(a, S(""), S("example"), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    Str tmpfn = filepath_join_v(a, 2, dir, S("tmpfile"));
    err = ioutil_write_file(tmpfn, content, 0666);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    (void)os_remove_all(dir); /* clean up */
}

static void ExampleTempDir_suffix(void) {
    Str parent_dir = os_temp_dir(a);
    Error err = BURROW_NO_ERROR;
    Str logs_dir = ioutil_temp_dir(a, parent_dir, S("*-logs"), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    /* Logs can be cleaned out earlier if needed by searching for all
     * directories whose suffix ends in *-logs. */
    Str glob_pattern = filepath_join_v(a, 2, parent_dir, S("*-logs"));
    Slice matches = filepath_glob(a, glob_pattern, &err);
    if (BURROW_FAILED(err))
        log_fatalf_v("Failed to match %q: %v", glob_pattern, err);

    for (Int i = 0; i < matches.len; i++) {
        Str match = BURROW_AT(Str, matches, i);
        err = os_remove_all(match);
        if (BURROW_FAILED(err))
            log_printf_v("Failed to remove %q: %v", match, err);
    }
    (void)os_remove_all(logs_dir); /* clean up */
}

static void ExampleTempFile(void) {
    Slice content = bytes_of(S("temporary file's content"));
    Error err = BURROW_NO_ERROR;
    OsFile *tmpfile = ioutil_temp_file(a, S(""), S("example"), &err);
    if (tmpfile == NULL)
        log_fatal_v(err);

    Str name = os_file_name(tmpfile);
    (void)os_file_write(tmpfile, content, &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);
    err = os_file_close(tmpfile);
    if (BURROW_FAILED(err))
        log_fatal_v(err);
    os_file_free(tmpfile);
    (void)os_remove(name); /* clean up */
}

static void ExampleTempFile_suffix(void) {
    Slice content = bytes_of(S("temporary file's content"));
    Error err = BURROW_NO_ERROR;
    OsFile *tmpfile = ioutil_temp_file(a, S(""), S("example.*.txt"), &err);
    if (tmpfile == NULL)
        log_fatal_v(err);

    Str name = os_file_name(tmpfile);
    (void)os_file_write(tmpfile, content, &err);
    if (BURROW_FAILED(err)) {
        (void)os_file_close(tmpfile);
        log_fatal_v(err);
    }
    err = os_file_close(tmpfile);
    if (BURROW_FAILED(err))
        log_fatal_v(err);
    os_file_free(tmpfile);
    (void)os_remove(name); /* clean up */
}

static void ExampleReadFile(void) {
    Error err = BURROW_NO_ERROR;
    Slice content = ioutil_read_file(a, S("tests/ioutiltest/testdata/hello"), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);

    fmt_printf_v("File contents: %s", str_of(content));
}

static void ExampleWriteFile(void) {
    Slice message = bytes_of(S("Hello, Gophers!"));
    Error err = ioutil_write_file(S("hello"), message, 0644);
    if (BURROW_FAILED(err))
        log_fatal_v(err);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReadFile)                                                                    \
    X(TestWriteFile)                                                                   \
    X(TestReadOnlyWriteFile)                                                           \
    X(TestReadDir)                                                                     \
    X(TestTempFile)                                                                    \
    X(TestTempFile_pattern)                                                            \
    X(TestTempFile_BadPattern)                                                         \
    X(TestTempDir)                                                                     \
    X(TestTempDir_BadDir)                                                              \
    X(TestTempDir_BadPattern)                                                          \
    X(ExampleReadAll, "Go is a general-purpose language designed with systems "        \
                      "programming in mind.\n")                                        \
    X(ExampleReadDir)                                                                  \
    X(ExampleTempDir)                                                                  \
    X(ExampleTempDir_suffix)                                                           \
    X(ExampleTempFile)                                                                 \
    X(ExampleTempFile_suffix)                                                          \
    X(ExampleReadFile, "File contents: Hello, Gophers!\n")                             \
    X(ExampleWriteFile)

static int ioutil_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(ioutil_main, TESTS)
