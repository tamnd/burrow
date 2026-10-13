/* Derived from Go's src/debug/buildinfo/buildinfo_test.go and search_test.go,
 * and src/runtime/debug/mod_test.go. Go source: go1.27.1.
 *
 * Go's TestReadFile builds a small program for each platform it knows and
 * reads the build information back, whole and with the record damaged. Here
 * tools/gen-debug-tests.sh buildinfo made those programs ahead of time, for
 * Linux, Windows, macOS, AIX and Plan 9, and kept a dump of what Go's
 * debug/buildinfo makes of each of them, of Go's own test files, and of each
 * file damaged in a few dozen ways. The dump this makes has to be the same.
 * Go's other tests, the search for the magic, the Go 1.17 binary, the C
 * binary, the PE with the magic out of line and the fuzz seeds, are all here
 * as well, and so is runtime/debug's ParseBuildInfo, with what Go made of a
 * few hundred inputs.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/debug/buildinfo.h"
#include "burrow/encoding/binary.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/runtime/debug.h"
#include "burrow/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/debug/buildinfo_internal.h"
#include "debug_dump.h"

typedef struct BuildinfoParseCase {
    const char *in;
    int len;
    const char *want;
} BuildinfoParseCase;

#include "buildinfo_test_gen.h"

#define NFILES (sizeof buildinfo_files / sizeof buildinfo_files[0])

static const char bi_magic_text[] = "\xff Go buildinf:";

/* --------------------------------------------------------------------- dump */

/* The dump of tools/gen-debug-tests.sh: BuildInfo.String, or the error. */
static void dump(Alloc *a, IoWriter w, Slice data) {
    BytesReader *br = bytes_new_reader(a, data);
    Error err = BURROW_NO_ERROR;
    Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
    if (bi == NULL) {
        fmt_fprintf_v(w, "error: %s\n", error_text(err));
        return;
    }
    Str s = debug_build_info_string(bi, a);
    (void)io_write_string(w, s, NULL);
    debug_build_info_free(bi);
}

static void TestDump(TestingT *t) {
    debug_check_dumps(t, buildinfo_files, NFILES, dump);
}

static void TestDamage(TestingT *t) {
    CHECK(debug_check_damage(t, buildinfo_files, NFILES, dump) > 1000);
}

static const DebugFile *find_file(const char *name) {
    for (size_t i = 0; i < NFILES; i++) {
        if (strcmp(buildinfo_files[i].name, name) == 0)
            return &buildinfo_files[i];
    }
    return NULL;
}

/* The binaries the generator built, as against Go's test files. */
static bool is_build(const DebugFile *f) {
    return strncmp(f->name, "testdata/", 9) != 0 && strcmp(f->name, "issue57002") != 0;
}

/* ---------------------------------------------------------- TestReadFile */

static Int magic_index(Slice b) {
    return bytes_index(b, (Slice){(void *)(uintptr_t)bi_magic_text, 14, 14, TYPE_BYTE});
}

/* The error buildinfo_read_file gives for name, or "" when there is none. */
static Str read_file_error(Alloc *a, Str name, Str *got) {
    Error err = BURROW_NO_ERROR;
    Buildinfo *bi = buildinfo_read_file(a, name, &err);
    if (bi == NULL)
        return error_text(err);
    *got = debug_build_info_string(bi, a);
    debug_build_info_free(bi);
    return BURROW_STR_EMPTY;
}

static void TestReadFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("buildinfotest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    Str got = BURROW_STR_EMPTY;
    Str e = read_file_error(a, BURROW_S("doesnotexist.txt"), &got);
    if (!strings_contains(e, BURROW_S("doesnotexist")))
        testing_t_errorf_v(t, "doesnotexist: got error %q", e);
    if (!strings_has_prefix(e, BURROW_S("could not read Go build info: open ")))
        testing_t_errorf_v(
            t, "doesnotexist: got error %q, want the FsPathError wrapped", e);

    Str empty = fmt_sprintf_v(a, "%s/empty", dir);
    err = os_write_file(empty, (Slice){NULL, 0, 0, TYPE_BYTE}, 0644);
    CHECK(BURROW_OK(err));
    e = read_file_error(a, empty, &got);
    Str want = fmt_sprintf_v(
        a, "could not read Go build info from %s: unrecognized file format", empty);
    if (!str_eq(e, want))
        testing_t_errorf_v(t, "empty: got error %q, want %q", e, want);

    int nbuilds = 0;
    for (size_t i = 0; i < NFILES; i++) {
        const DebugFile *df = &buildinfo_files[i];
        if (!is_build(df))
            continue;
        nbuilds++;
        Slice data = file_bytes(a, df);
        Str name = fmt_sprintf_v(a, "%s/%s", dir, str_from_cstr(df->name));

        /* valid_modules and valid_gopath. */
        err = os_write_file(name, data, 0644);
        CHECK(BURROW_OK(err));
        got = BURROW_STR_EMPTY;
        e = read_file_error(a, name, &got);
        if (e.len > 0) {
            testing_t_errorf_v(t, "%s: unexpected error: %s", df->name, e);
            continue;
        }
        if (!strings_has_prefix(got, BURROW_S("go\tgo1.")) ||
            !strings_contains(got, BURROW_S("\npath\texample.com/m\n")) ||
            !strings_contains(got, BURROW_S("\nbuild\t-compiler=gc\n")))
            testing_t_errorf_v(t, "%s: got\n%s", df->name, got);

        Int m = magic_index(data);
        if (m < 0) {
            testing_t_errorf_v(t, "%s: Go buildinf not found", df->name);
            continue;
        }
        Slice b = slice_make(a, TYPE_BYTE, data.len, data.len);
        Byte *p = (Byte *)b.p;

        /* invalid_modules and invalid_gopath. */
        memcpy(p, data.p, (size_t)data.len);
        p[m + 2] = 'N';
        err = os_write_file(name, b, 0644);
        CHECK(BURROW_OK(err));
        e = read_file_error(a, name, &got);
        if (!strings_contains(e, BURROW_S("not a Go executable")))
            testing_t_errorf_v(t, "%s, damaged: got error %q", df->name, e);

        /* invalid_str_len: 16TB ought to be enough for anyone. */
        memcpy(p, data.p, (size_t)data.len);
        (void)binary_put_uvarint(slice_sub(b, m + 32, b.len), UINT64_C(16) << 40);
        err = os_write_file(name, b, 0644);
        CHECK(BURROW_OK(err));
        e = read_file_error(a, name, &got);
        if (!strings_contains(e, BURROW_S("not a Go executable")))
            testing_t_errorf_v(t, "%s, long string: got error %q", df->name, e);
    }
    CHECK(nbuilds >= 14);
    (void)os_remove_all(dir);
    arena_free(&ar);
}

/* ------------------------------------------------- Go's other tests */

static Buildinfo *read_test_file(Alloc *a, const char *name, Error *err) {
    const DebugFile *df = find_file(name);
    if (df == NULL)
        abort();
    BytesReader *br = bytes_new_reader(a, file_bytes(a, df));
    return buildinfo_read(a, bytes_reader_as_io_reader_at(br), err);
}

static void Test117(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Buildinfo *info = read_test_file(a, "testdata/go117", &err);
    if (info == NULL) {
        testing_t_fatalf_v(t, "Read got err %s, want nil", error_text(err));
        arena_free(&ar);
        return;
    }
    if (!str_eq(info->go_version, BURROW_S("go1.17")))
        testing_t_errorf_v(t, "GoVersion got %s want go1.17", info->go_version);
    if (!str_eq(info->path, BURROW_S("example.com/go117")))
        testing_t_errorf_v(t, "Path got %s want example.com/go117", info->path);
    if (!str_eq(info->main.path, BURROW_S("example.com/go117")))
        testing_t_errorf_v(t, "Main.Path got %s want example.com/go117",
                           info->main.path);
    debug_build_info_free(info);
    arena_free(&ar);
}

static void TestNotGo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Buildinfo *info = read_test_file(a, "testdata/notgo", &err);
    if (info != NULL) {
        testing_t_errorf_v(t, "Read got nil err, want non-nil");
        debug_build_info_free(info);
    } else if (!strings_contains(error_text(err), BURROW_S("not a Go executable"))) {
        testing_t_errorf_v(t, "ReadFile got err %s want not a Go executable",
                           error_text(err));
    }
    arena_free(&ar);
}

/* FuzzIssue57002 and FuzzRead, run on their seeds: none of them may crash,
 * and TestDump has already said what each gives. */
static void TestFuzzSeeds(TestingT *t) {
    const char *const seeds[] = {"issue57002", "testdata/go117", "testdata/notgo"};
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Error err = BURROW_NO_ERROR;
        debug_build_info_free(read_test_file(a, seeds[i], &err));
        arena_free(&ar);
    }
    CHECK(find_file("issue57002") != NULL);
}

static void put32(Byte *p, uint32_t v) {
    p[0] = (Byte)v;
    p[1] = (Byte)(v >> 8);
    p[2] = (Byte)(v >> 16);
    p[3] = (Byte)(v >> 24);
}

/* When the first magic is not where it can be, the search used to go round
 * forever. */
static void TestIssue54968(TestingT *t) {
    enum { PADDING = 200, ALIGN = 16, MAGIC = 14, START = 0x40 + 4 + 20 + 40 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A PE file with one section and no optional header. */
    Byte d[START + PADDING + MAGIC];
    memset(d, 0, sizeof d);
    d[0] = 'M';
    d[1] = 'Z';
    put32(d + 0x3c, 0x40);
    memcpy(d + 0x40, "PE\0\0", 4);
    d[0x44 + 2] = 1; /* NumberOfSections */
    Byte *sh = d + 0x44 + 20;
    sh[0] = 't';
    put32(sh + 16, PADDING + MAGIC); /* SizeOfRawData */
    put32(sh + 20, START);           /* PointerToRawData */

    Error err = BURROW_NO_ERROR;
    BytesReader *br = bytes_new_reader(a, (Slice){d, sizeof d, sizeof d, TYPE_BYTE});
    PeFile *pf = pe_new_file(a, bytes_reader_as_io_reader_at(br), &err);
    if (pf == NULL) {
        testing_t_fatalf_v(t,
                           "need a valid PE header for the misaligned buildInfoMagic "
                           "test: %s",
                           error_text(err));
        arena_free(&ar);
        return;
    }
    pe_file_free(pf);

    for (int i = 1; i < PADDING - MAGIC; i++) {
        if (i % ALIGN == 0)
            continue;
        memset(d + START, 0, PADDING + MAGIC);
        memcpy(d + START + i, bi_magic_text, MAGIC);
        br = bytes_new_reader(a, (Slice){d, sizeof d, sizeof d, TYPE_BYTE});
        err = BURROW_NO_ERROR;
        Buildinfo *bi = buildinfo_read(a, bytes_reader_as_io_reader_at(br), &err);
        if (bi != NULL) {
            testing_t_errorf_v(t, "start_at_%d: got error nil", i);
            debug_build_info_free(bi);
        } else if (!strings_contains(error_text(err),
                                     BURROW_S("not a Go executable"))) {
            testing_t_errorf_v(t, "start_at_%d: got error %q", i, error_text(err));
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------- TestSearchMagic */

typedef struct ByteExe {
    Slice b;
    IoReaderAt r;
} ByteExe;

static bool byte_exe_data_reader(void *ctx, uint64_t addr, IoSectionReader *sr,
                                 Error *err) {
    const ByteExe *x = (const ByteExe *)ctx;
    if (addr >= (uint64_t)x->b.len) {
        BURROW_OUT(err, fmt_errorf_v("ReadData(%d) out of bounds of %d-byte slice",
                                     addr, x->b.len));
        return false;
    }
    *sr = io_new_section_reader(x->r, (int64_t)addr, x->b.len - (int64_t)addr);
    return true;
}

static void TestSearchMagic(TestingT *t) {
    enum { HEADER = 32, ALIGN = 16, CHUNK = BURROW__BUILDINFO_SEARCH_CHUNK };
    static const struct {
        const char *name;
        Int size;
        Int at; /* where the magic goes, or -1 */
        Int magic_len;
        Int want; /* -1 for errNotGoExe */
    } tests[] = {
        {"beginning", HEADER, 0, 14, 0},
        {"offset", 512, 4 * ALIGN, 14, 4 * ALIGN},
        {"second_chunk", 4 * CHUNK, CHUNK + 4 * ALIGN, 14, CHUNK + 4 * ALIGN},
        /* The magic is 64 bytes into the second chunk, which is only just long
         * enough to hold the header. */
        {"second_chunk_short", CHUNK + 4 * ALIGN + HEADER, CHUNK + 4 * ALIGN, 14,
         CHUNK + 4 * ALIGN},
        {"missing", HEADER, -1, 0, -1},
        /* There has to be room for the whole header, not just the magic. */
        {"too_short", 14, 0, 14, -1},
        {"misaligned", 512, 7, 14, -1},
        /* The magic is across the edge of a chunk, so it cannot be aligned. */
        {"misaligned_across_chunk", 2 * CHUNK, CHUNK - 8, 14, -1},
        /* The magic is aligned in the first chunk, but the rest of the header
         * is in the next. */
        {"header_across_chunk", 2 * CHUNK, CHUNK - ALIGN, 14, CHUNK - ALIGN},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Slice b = slice_make(a, TYPE_BYTE, tests[i].size, tests[i].size);
        if (tests[i].at >= 0)
            memcpy((Byte *)b.p + tests[i].at, bi_magic_text,
                   (size_t)tests[i].magic_len);
        BytesReader *br = bytes_new_reader(a, b);
        ByteExe bx = {b, bytes_reader_as_io_reader_at(br)};
        BuildinfoExe x = {&bx, byte_exe_data_reader};
        Error err = BURROW_NO_ERROR;
        uint64_t addr = burrow__buildinfo_search_magic(a, &x, 0, (uint64_t)b.len, &err);
        if (tests[i].want >= 0) {
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s: searchMagic got err %s, want nil",
                                   tests[i].name, error_text(err));
            else if (addr != (uint64_t)tests[i].want)
                testing_t_errorf_v(t, "%s: searchMagic got addr %d, want %d",
                                   tests[i].name, addr, tests[i].want);
        } else if (!str_eq(error_text(err), BURROW_S("not a Go executable"))) {
            testing_t_errorf_v(t,
                               "%s: searchMagic got err %q, want not a Go executable",
                               tests[i].name, error_text(err));
        }
        arena_free(&ar);
    }
}

/* ------------------------------------------------------- runtime/debug */

/* What Go's ParseBuildInfo made of each input, and FuzzParseBuildInfoRoundTrip
 * on all of them: what String gives has to parse to the same again. */
static void TestParseBuildInfo(TestingT *t) {
    size_t n = sizeof buildinfo_parse_cases / sizeof buildinfo_parse_cases[0];
    for (size_t i = 0; i < n; i++) {
        const BuildinfoParseCase *c = &buildinfo_parse_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str in = str_from_bytes((const Byte *)c->in, c->len);
        Error err = BURROW_NO_ERROR;
        DebugBuildInfo *bi = debug_parse_build_info(a, in, &err);
        Str got = BURROW_STR_EMPTY;
        if (bi == NULL) {
            got = fmt_sprintf_v(a, "error: %s\n", error_text(err));
        } else {
            Str s = debug_build_info_string(bi, a);
            got = fmt_sprintf_v(a, "ok\n%s", s);
            DebugBuildInfo *bi2 = debug_parse_build_info(a, s, &err);
            if (bi2 == NULL) {
                testing_t_errorf_v(t, "%q: %s:\n%s", in, error_text(err), s);
            } else {
                Str s2 = debug_build_info_string(bi2, a);
                if (!str_eq(s, s2) || bi2->deps.len != bi->deps.len ||
                    bi2->settings.len != bi->settings.len)
                    testing_t_errorf_v(t,
                                       "Parsed representation differs.\ninput:\n%s\n"
                                       "output:\n%s",
                                       in, s);
                debug_build_info_free(bi2);
            }
            debug_build_info_free(bi);
        }
        Str want = str_from_cstr(c->want);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "ParseBuildInfo(%q):\n got %q\nwant %q", in, got,
                               want);
        arena_free(&ar);
    }
    CHECK(n > 200);
}

static void TestReadBuildInfo(TestingT *t) {
    bool ok = true;
    CHECK(debug_read_build_info(NULL, &ok) == NULL);
    CHECK(!ok);
}

/* A DebugBuildInfo made by hand, with a replaced main module and settings
 * that need quoting, comes out as Go's String writes it, down to the empty
 * line after the replacement. */
static void TestBuildInfoString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DebugModule rep = {BURROW_S("../m"), BURROW_STR_EMPTY, BURROW_STR_EMPTY, NULL};
    DebugModule dep = {BURROW_S("golang.org/x/text"), BURROW_S("v0.3.0"),
                       BURROW_S("h1:abc="), NULL};
    DebugModule *deps[] = {&dep};
    DebugBuildSetting settings[] = {
        {BURROW_S("-compiler"), BURROW_S("gc")},
        {BURROW_S("-ldflags"), BURROW_S("-s -w")},
        {BURROW_S("a=b"), BURROW_STR_EMPTY},
    };
    DebugBuildInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.go_version = BURROW_S("go1.27.1");
    bi.path = BURROW_S("example.com/m/cmd");
    bi.main = (DebugModule){BURROW_S("example.com/m"), BURROW_S("(devel)"),
                            BURROW_STR_EMPTY, &rep};
    bi.deps = (Slice){deps, 1, 1, TYPE_UNSAFE_POINTER};
    bi.settings = (Slice){settings, 3, 3, TYPE_DEBUG_BUILD_SETTING};
    Str got = debug_build_info_string(&bi, a);
    Str want = BURROW_S("go\tgo1.27.1\n"
                        "path\texample.com/m/cmd\n"
                        "mod\texample.com/m\t(devel)\n"
                        "=>\t../m\t\t\n"
                        "\n"
                        "dep\tgolang.org/x/text\tv0.3.0\th1:abc=\n"
                        "build\t-compiler=gc\n"
                        "build\t-ldflags=\"-s -w\"\n"
                        "build\t\"a=b\"=\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q\nwant %q", got, want);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDump)                                                                        \
    X(TestDamage)                                                                      \
    X(TestReadFile)                                                                    \
    X(Test117)                                                                         \
    X(TestNotGo)                                                                       \
    X(TestFuzzSeeds)                                                                   \
    X(TestIssue54968)                                                                  \
    X(TestSearchMagic)                                                                 \
    X(TestParseBuildInfo)                                                              \
    X(TestReadBuildInfo)                                                               \
    X(TestBuildInfoString)

TESTING_MAIN(TESTS)
