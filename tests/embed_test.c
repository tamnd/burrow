/* Derived from Go's src/embed/internal/embedtest/embed_test.go and
 * embedx_test.go.
 * Go source: go1.27.1.
 *
 * The variables are defined in tests/embedtest/embed_test_embed.c, which
 *
 *     tools/burrow-gen embed tests/embed_test.c --dir tests/embedtest \
 *         -o tests/embedtest/embed_test_embed.c
 *
 * writes and tools/check-gen.sh keeps up to date. The files are Go's, copied
 * from embedtest. The tests from embedx_test.go are the ones starting TestX,
 * with x in front of the names of their variables, which in Go are in a
 * package of their own. The tests after TestXGlobal are not in Go.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/embed.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/testing/fstest.h"

#include <string.h>

BURROW_EMBED_FS(global, "testdata/h*.txt", "c*.txt testdata/g*.txt");
BURROW_EMBED_FILE(concurrency, "c*txt");
BURROW_EMBED_BYTES(glass, "testdata/g*.txt");

BURROW_EMBED_FS(test_dir_all, "testdata");
BURROW_EMBED_FS(test_hidden_dir, "testdata");
BURROW_EMBED_FS(test_hidden_star, "testdata/*");

BURROW_EMBED_BYTES(hello_bytes, "\"testdata/hello.txt\"");
BURROW_EMBED_FILE(hello_string, "\"testdata/hello.txt\"");

BURROW_EMBED_FS(xglobal, "testdata/*.txt");
BURROW_EMBED_FILE(xconcurrency, "c*txt");
BURROW_EMBED_BYTES(xglass, "testdata/g*.txt");
BURROW_EMBED_FILE(sbig, "testdata/ascii.txt");
BURROW_EMBED_BYTES(bbig, "testdata/ascii.txt");

/* The arena everything in a test is made in, freed when the test ends. */
typedef struct EmbedTest {
    Arena ar;
    Alloc *a;
} EmbedTest;

static void embed_test_free(void *env) {
    arena_free(&((EmbedTest *)env)->ar);
    mem_free(heap_allocator(), env, sizeof(EmbedTest), _Alignof(EmbedTest));
}

static Alloc *test_alloc(TestingT *t) {
    EmbedTest *e =
        (EmbedTest *)mem_alloc(heap_allocator(), sizeof *e, _Alignof(EmbedTest));
    arena_init(&e->ar, NULL, 0);
    e->a = arena_allocator(&e->ar);
    testing_t_cleanup(t, BURROW_FN(Func, embed_test_free, e));
    return e->a;
}

static void test_files(TestingT *t, Alloc *a, const EmbedFS *f, Str name, Str data) {
    testing_t_helper(t);
    Error err;
    Slice d = embed_fs_read_file(f, a, name, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }
    if (!str_eq(str_from_bytes(d.p, d.len), data))
        testing_t_errorf_v(t, "read %v = %q, want %q", name, d, data);
}

static void test_string(TestingT *t, Str s, Str name, Str data) {
    testing_t_helper(t);
    if (!str_eq(s, data))
        testing_t_errorf_v(t, "%v = %q, want %q", name, s, data);
}

/* testDir, with the names it wants as one string, separated by spaces. */
static void test_dir(TestingT *t, Alloc *a, const EmbedFS *f, Str name, Str expect) {
    testing_t_helper(t);
    Error err;
    Slice dirs = embed_fs_read_dir(f, a, name, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }
    Slice names = slice_nil(TYPE_STRING);
    FsDirEntry *e = (FsDirEntry *)dirs.p;
    for (Int i = 0; i < dirs.len; i++) {
        Str n = BURROW_CALL0(e[i], name);
        if (BURROW_CALL0(e[i], is_dir))
            n = fmt_sprintf_v(a, "%s/", n);
        names = BURROW_APPEND(Str, a, names, n);
    }
    Str got = strings_join(a, names, BURROW_S(" "));
    if (!str_eq(got, expect))
        testing_t_errorf_v(t, "readdir %v = [%v], want [%v]", name, got, expect);
}

static void TestGlobal(TestingT *t) {
    Alloc *a = test_alloc(t);
    test_files(t, a, &global, BURROW_S("concurrency.txt"),
               BURROW_S("Concurrency is not parallelism.\n"));
    test_files(t, a, &global, BURROW_S("testdata/hello.txt"),
               BURROW_S("hello, world\n"));
    test_files(t, a, &global, BURROW_S("testdata/glass.txt"),
               BURROW_S("I can eat glass and it doesn't hurt me.\n"));

    Error err = fstest_test_fs_v(embed_fs_as_fs(&global), BURROW_S("concurrency.txt"),
                                 BURROW_S("testdata/hello.txt"));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    test_string(t, concurrency, BURROW_S("concurrency"),
                BURROW_S("Concurrency is not parallelism.\n"));
    test_string(t, str_from_bytes(glass.p, glass.len), BURROW_S("glass"),
                BURROW_S("I can eat glass and it doesn't hurt me.\n"));
}

static void TestDir(TestingT *t) {
    Alloc *a = test_alloc(t);
    const EmbedFS *all = &test_dir_all;
    test_files(t, a, all, BURROW_S("testdata/hello.txt"), BURROW_S("hello, world\n"));
    test_files(t, a, all, BURROW_S("testdata/i/i18n.txt"),
               BURROW_S("internationalization\n"));
    test_files(t, a, all, BURROW_S("testdata/i/j/k/k8s.txt"), BURROW_S("kubernetes\n"));
    test_files(t, a, all, BURROW_S("testdata/ken.txt"),
               BURROW_S("If a program is too slow, it must have a loop.\n"));

    test_dir(t, a, all, BURROW_S("."), BURROW_S("testdata/"));
    test_dir(t, a, all, BURROW_S("testdata/i"), BURROW_S("i18n.txt j/"));
    test_dir(t, a, all, BURROW_S("testdata/i/j"), BURROW_S("k/"));
    test_dir(t, a, all, BURROW_S("testdata/i/j/k"), BURROW_S("k8s.txt"));
}

static void TestHidden(TestingT *t) {
    Alloc *a = test_alloc(t);
    const EmbedFS *dir = &test_hidden_dir;
    const EmbedFS *star = &test_hidden_star;

    testing_t_logf_v(t, "//go:embed testdata");

    test_dir(t, a, dir, BURROW_S("testdata"),
             BURROW_S("-not-hidden/ ascii.txt glass.txt hello.txt i/ ken.txt"));

    testing_t_logf_v(t, "//go:embed testdata/*");

    test_dir(t, a, star, BURROW_S("testdata"),
             BURROW_S("-not-hidden/ .hidden/ _hidden/ ascii.txt glass.txt hello.txt i/ "
                      "ken.txt"));

    /* but not .more or _more */
    test_dir(t, a, star, BURROW_S("testdata/.hidden"), BURROW_S("fortune.txt more/"));
}

static void TestUninitialized(TestingT *t) {
    Alloc *a = test_alloc(t);
    EmbedFS uninitialized = {0};
    test_dir(t, a, &uninitialized, BURROW_S("."), BURROW_STR_EMPTY);
    Error err;
    FsFile f = embed_fs_open(&uninitialized, a, BURROW_S("."), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    FsFileInfo fi = BURROW_CALL(f, stat, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (!BURROW_CALL0(fi, is_dir))
        testing_t_errorf_v(t, "in uninitialized embed.FS, . is not a directory");
    BURROW_CALL0(f, read_closer.closer.close);
}

/* golang.org/issue/47735. Go's test is about the names a []byte can go by,
 * which C does not have, so here it checks that a Str and a Slice of the same
 * file agree with the FS. */
static void TestAliases(TestingT *t) {
    Alloc *a = test_alloc(t);
    Error e;
    Slice want =
        embed_fs_read_file(&test_dir_all, a, BURROW_S("testdata/hello.txt"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ReadFile: %v", e);
    if (!bytes_equal(hello_bytes, want))
        testing_t_fatalf_v(t, "got %v want %v", hello_bytes, want);
    if (!str_eq(hello_string, str_from_bytes(want.p, want.len)))
        testing_t_fatalf_v(t, "got %q want %q", hello_string, want);
}

static void TestOffset(TestingT *t) {
    Alloc *a = test_alloc(t);
    Error err;
    FsFile file = embed_fs_open(&test_dir_all, a, BURROW_S("testdata/hello.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);

    Str want = BURROW_S("hello, world\n");

    /* Read the entire file. */
    Slice got = slice_make(a, TYPE_BYTE, want.len, want.len);
    Int n = BURROW_CALL(file, read_closer.reader.read, got, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read: %v", err);
    if (n != want.len)
        testing_t_fatalf_v(t, "Read: %d", n);
    if (!str_eq(str_from_bytes(got.p, got.len), want))
        testing_t_fatalf_v(t, "Read: %q", got);

    /* Try to read one byte; confirm we're at the EOF. */
    Byte buf[1];
    n = BURROW_CALL(file, read_closer.reader.read, slice_from(buf, 1, 1, TYPE_BYTE),
                    &err);
    if (!errors_is(err, io_eof))
        testing_t_fatalf_v(t, "Read: %v", err);
    if (n != 0)
        testing_t_fatalf_v(t, "Read: %d", n);

    /* Use seek to get the offset at the EOF. */
    const Type *ft = file.vt->read_closer.reader.self_type;
    const Method *seeker = burrow__io_seek_method(ft);
    if (seeker == NULL)
        testing_t_fatalf_v(t, "openFile has no Seek");
    int64_t off = burrow__io_seek(seeker, file.data, 0, BURROW_IO_SEEK_CURRENT, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Seek: %v", err);
    if (off != (int64_t)want.len)
        testing_t_fatalf_v(t, "Seek: %d", off);

    /* Use ReadAt to read the entire file, ignoring the offset. */
    const Method *at = burrow__io_read_at_method(ft);
    if (at == NULL)
        testing_t_fatalf_v(t, "openFile has no ReadAt");
    got = slice_make(a, TYPE_BYTE, want.len, want.len);
    n = burrow__io_read_at(at, file.data, got, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAt: %v", err);
    if (n != want.len)
        testing_t_fatalf_v(t, "ReadAt: got %d bytes, want %d bytes", n, want.len);
    if (!str_eq(str_from_bytes(got.p, got.len), want))
        testing_t_fatalf_v(t, "ReadAt: got %q, want %q", got, want);

    /* Use ReadAt with non-zero offset. */
    off = 7;
    want = (Str){want.p + off, want.len - (Int)off};
    got = slice_make(a, TYPE_BYTE, want.len, want.len);
    n = burrow__io_read_at(at, file.data, got, off, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAt: %v", err);
    if (n != want.len)
        testing_t_fatalf_v(t, "ReadAt: got %d bytes, want %d bytes", n, want.len);
    if (!str_eq(str_from_bytes(got.p, got.len), want))
        testing_t_fatalf_v(t, "ReadAt: got %q, want %q", got, want);
}

/* ------------------------------------------------------------ embedx_test */

static void TestXGlobal(TestingT *t) {
    Alloc *a = test_alloc(t);
    /* Go's global2 and the rest are copies made when the package starts,
     * which in C is a copy of the variable. */
    Str xconcurrency2 = xconcurrency;
    Slice xglass2 = xglass;
    Str sbig2 = sbig;
    Slice bbig2 = bbig;

    test_files(t, a, &xglobal, BURROW_S("testdata/hello.txt"),
               BURROW_S("hello, world\n"));
    test_string(t, xconcurrency, BURROW_S("concurrency"),
                BURROW_S("Concurrency is not parallelism.\n"));
    test_string(t, str_from_bytes(xglass.p, xglass.len), BURROW_S("glass"),
                BURROW_S("I can eat glass and it doesn't hurt me.\n"));
    test_string(t, xconcurrency2, BURROW_S("concurrency2"),
                BURROW_S("Concurrency is not parallelism.\n"));
    test_string(t, str_from_bytes(xglass2.p, xglass2.len), BURROW_S("glass2"),
                BURROW_S("I can eat glass and it doesn't hurt me.\n"));

    Error err;
    Slice big = os_read_file(a, BURROW_S("tests/embedtest/testdata/ascii.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str bigs = str_from_bytes(big.p, big.len);
    test_string(t, sbig, BURROW_S("sbig"), bigs);
    test_string(t, sbig2, BURROW_S("sbig2"), bigs);
    test_string(t, str_from_bytes(bbig.p, bbig.len), BURROW_S("bbig"), bigs);
    test_string(t, str_from_bytes(bbig2.p, bbig2.len), BURROW_S("bbig"), bigs);

    if (testing_t_failed(t))
        return;

    /* Could check &glass[0] == &glass2[0] but also want to make sure write does
     * not fault (data must not be in read-only memory). */
    Byte *g = (Byte *)xglass.p, *g2 = (Byte *)xglass2.p;
    Byte old = g[0];
    g[0]++;
    if (g2[0] != g[0])
        testing_t_fatalf_v(t, "glass and glass2 do not share storage");
    g[0] = old;

    Byte *b = (Byte *)bbig.p, *b2 = (Byte *)bbig2.p;
    old = b[0];
    b[0]++;
    if (b2[0] != b[0])
        testing_t_fatalf_v(t, "bbig and bbig2 do not share storage");
    b[0] = old;
}

/* ------------------------------------------------------------- not in Go */

/* The errors, which Go's tests do not look at. */
static void TestErrors(TestingT *t) {
    Alloc *a = test_alloc(t);
    const EmbedFS *all = &test_dir_all;
    Error err;

    embed_fs_read_file(all, a, BURROW_S("testdata"), &err);
    test_string(t, error_text(err), BURROW_S("ReadFile(testdata)"),
                BURROW_S("read testdata: is a directory"));

    embed_fs_read_dir(all, a, BURROW_S("testdata/hello.txt"), &err);
    test_string(t, error_text(err), BURROW_S("ReadDir(testdata/hello.txt)"),
                BURROW_S("read testdata/hello.txt: not a directory"));

    const char *missing[] = {"nope",      "testdata/nope", "/testdata",
                             "testdata/", "./testdata",    "testdata/../testdata",
                             ""};
    for (size_t i = 0; i < sizeof missing / sizeof missing[0]; i++) {
        Str name = str_from_cstr(missing[i]);
        embed_fs_open(all, a, name, &err);
        if (!errors_is(err, fs_err_not_exist))
            testing_t_errorf_v(t, "Open(%q) = %v, want not exist", name, err);
    }

    /* A directory open for reading says so with the name it has in the list,
     * slash and all, as Go's openDir does. */
    FsFile d = embed_fs_open(all, a, BURROW_S("testdata"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Byte buf[1];
    BURROW_CALL(d, read_closer.reader.read, slice_from(buf, 1, 1, TYPE_BYTE), &err);
    test_string(t, error_text(err), BURROW_S("Read(testdata)"),
                BURROW_S("read testdata/: is a directory"));

    FsFile f = embed_fs_open(all, a, BURROW_S("testdata/hello.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    const Method *seek = burrow__io_seek_method(f.vt->read_closer.reader.self_type);
    burrow__io_seek(seek, f.data, 14, BURROW_IO_SEEK_START, &err);
    test_string(t, error_text(err), BURROW_S("Seek(14, 0)"),
                BURROW_S("seek testdata/hello.txt: invalid argument"));
}

/* The FS through fs's functions, the stat that is not a slot of its own, and
 * the modes, sizes and times Go's file gives. */
static void TestFileInfo(TestingT *t) {
    Alloc *a = test_alloc(t);
    Fs fsys = embed_fs_as_fs(&test_dir_all);
    Error err;

    FsFileInfo fi = fs_stat(a, fsys, BURROW_S("testdata/ken.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    test_string(t, fs_format_file_info(a, fi), BURROW_S("Stat(testdata/ken.txt)"),
                BURROW_S("-r--r--r-- 47 0001-01-01 00:00:00 ken.txt"));

    fi = fs_stat(a, fsys, BURROW_S("testdata/i"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    test_string(t, fs_format_file_info(a, fi), BURROW_S("Stat(testdata/i)"),
                BURROW_S("dr-xr-xr-x 0 0001-01-01 00:00:00 i/"));

    fi = fs_stat(a, fsys, BURROW_S("."), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    test_string(t, fs_format_file_info(a, fi), BURROW_S("Stat(.)"),
                BURROW_S("dr-xr-xr-x 0 0001-01-01 00:00:00 ./"));

    Slice m = fs_glob(a, fsys, BURROW_S("testdata/*.txt"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    test_string(t, strings_join(a, m, BURROW_S(" ")), BURROW_S("Glob(testdata/*.txt)"),
                BURROW_S("testdata/ascii.txt testdata/glass.txt testdata/hello.txt "
                         "testdata/ken.txt"));

    err = fstest_test_fs_v(fsys, BURROW_S("testdata/i/j/k/k8s.txt"));
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    err = fstest_test_fs_v(embed_fs_as_fs(&test_hidden_star),
                           BURROW_S("testdata/.hidden/more/tip.txt"));
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
}

/* The hash the generator writes is the first half of the SHA-256. */
static void TestHash(TestingT *t) {
    for (Int i = 0; i < test_dir_all.n; i++) {
        const EmbedFile *f = &test_dir_all.files[i];
        Byte want[16] = {0};
        if (f->name.p[f->name.len - 1] != '/') {
            Sha256Sum256Ret s = sha256_sum256(slice_from(
                (void *)(uintptr_t)f->data.p, f->data.len, f->data.len, TYPE_BYTE));
            memcpy(want, s.a, sizeof want);
        }
        if (memcmp(f->hash, want, sizeof want) != 0)
            testing_t_errorf_v(t, "hash of %v is wrong", f->name);
    }
}

#define TESTS(X)                                                                       \
    X(TestGlobal)                                                                      \
    X(TestDir)                                                                         \
    X(TestHidden)                                                                      \
    X(TestUninitialized)                                                               \
    X(TestAliases)                                                                     \
    X(TestOffset)                                                                      \
    X(TestXGlobal)                                                                     \
    X(TestErrors)                                                                      \
    X(TestFileInfo)                                                                    \
    X(TestHash)
TESTING_MAIN(TESTS)
