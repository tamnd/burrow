/* Derived from Go's src/go/token/position_test.go, serialize_test.go,
 * token_test.go and tree_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/gob.h"
#include "burrow/go/token.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

bool burrow__token_file_info(TokenFile *f, Int i, Int *offset, Str *filename, Int *line,
                             Int *column);

static Str cstr(const char *s) {
    return str_from_cstr(s);
}

static Slice ints(const Int *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_INT);
}

static Bytes bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static void check_pos(TestingT *t, Str msg, TokenPosition got, TokenPosition want) {
    if (!str_eq(got.filename, want.filename))
        testing_t_errorf_v(t, "%s: got filename = %q; want %q", msg, got.filename,
                           want.filename);
    if (got.offset != want.offset)
        testing_t_errorf_v(t, "%s: got offset = %d; want %d", msg, got.offset,
                           want.offset);
    if (got.line != want.line)
        testing_t_errorf_v(t, "%s: got line = %d; want %d", msg, got.line, want.line);
    if (got.column != want.column)
        testing_t_errorf_v(t, "%s: got column = %d; want %d", msg, got.column,
                           want.column);
}

static void TestNoPos(TestingT *t) {
    if (token_pos_is_valid(TOKEN_NO_POS))
        testing_t_errorf_v(t, "NoPos should not be valid");
    TokenPosition zero = {BURROW_STR_EMPTY, 0, 0, 0};
    check_pos(t, BURROW_S("nil NoPos"), token_file_set_position(NULL, TOKEN_NO_POS),
              zero);
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    check_pos(t, BURROW_S("fset NoPos"), token_file_set_position(fset, TOKEN_NO_POS),
              zero);
    token_file_set_free(fset);
}

typedef struct PosTest {
    const char *filename;
    const char *source; /* may be NULL */
    Int size;
    Int lines[12];
    Int nlines;
} PosTest;

static const PosTest pos_tests[] = {
    {"a", "", 0, {0}, 0},
    {"b", "01234", 5, {0}, 1},
    {"c", "\n\n\n\n\n\n\n\n\n", 9, {0, 1, 2, 3, 4, 5, 6, 7, 8}, 9},
    {"d", NULL, 100, {0, 5, 10, 20, 30, 70, 71, 72, 80, 85, 90, 99}, 12},
    {"e", NULL, 777, {0, 80, 100, 120, 130, 180, 267, 455, 500, 567, 620}, 11},
    {"f", "package p\n\nimport \"fmt\"", 23, {0, 10, 11}, 3},
    {"g", "package p\n\nimport \"fmt\"\n", 24, {0, 10, 11}, 3},
    {"h", "package p\n\nimport \"fmt\"\n ", 25, {0, 10, 11, 24}, 4},
};

enum { NPOS_TESTS = (int)(sizeof pos_tests / sizeof pos_tests[0]) };

static void linecol(const Int *lines, Int n, Int offs, Int *line, Int *col) {
    Int prev_line_offs = 0;
    for (Int l = 0; l < n; l++) {
        if (offs < lines[l]) {
            *line = l;
            *col = offs - prev_line_offs + 1;
            return;
        }
        prev_line_offs = lines[l];
    }
    *line = n;
    *col = offs - prev_line_offs + 1;
}

static void verify_positions(TestingT *t, Alloc *a, TokenFileSet *fset, TokenFile *f,
                             const Int *lines, Int n) {
    for (Int offs = 0; offs < token_file_size(f); offs++) {
        TokenPos p = token_file_pos(f, offs);
        Int offs2 = token_file_offset(f, p);
        if (offs2 != offs)
            testing_t_errorf_v(t, "%s, Offset: got offset %d; want %d",
                               token_file_name(f), offs2, offs);
        Int line = 0;
        Int col = 0;
        linecol(lines, n, offs, &line, &col);
        Str msg =
            fmt_sprintf_v(a, "%s (offs = %d, p = %d)", token_file_name(f), offs, p);
        TokenPosition want = {token_file_name(f), offs, line, col};
        check_pos(t, msg, token_file_position(f, token_file_pos(f, offs)), want);
        check_pos(t, msg, token_file_set_position(fset, p), want);
    }
}

static Bytes make_test_source(Alloc *a, Int size, const Int *lines, Int n) {
    Byte *src = mem_alloc(a, (size_t)size + 1, 1);
    for (Int i = 0; i < n; i++) {
        if (lines[i] > 0)
            src[lines[i] - 1] = '\n';
    }
    return slice_from(src, size, size, TYPE_BYTE);
}

static void TestPositions(TestingT *t) {
    enum { delta = 7 }; /* a non-zero base offset increment */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (int k = 0; k < NPOS_TESTS; k++) {
        const PosTest *test = &pos_tests[k];
        Str filename = cstr(test->filename);
        /* verify consistency of test case */
        if (test->source != NULL && (Int)strlen(test->source) != test->size)
            testing_t_errorf_v(t,
                               "%s: inconsistent test case: got file size %d; want %d",
                               filename, (Int)strlen(test->source), test->size);

        /* add file and verify name and size */
        TokenFile *f = token_file_set_add_file(
            fset, filename, token_file_set_base(fset) + delta, test->size);
        if (!str_eq(token_file_name(f), filename))
            testing_t_errorf_v(t, "got filename %q; want %q", token_file_name(f),
                               filename);
        if (token_file_size(f) != test->size)
            testing_t_errorf_v(t, "%s: got file size %d; want %d", token_file_name(f),
                               token_file_size(f), test->size);
        if (token_file_set_file(fset, token_file_pos(f, 0)) != f)
            testing_t_errorf_v(t, "%s: f.Pos(0) was not found in f",
                               token_file_name(f));

        /* add lines individually and verify all positions */
        for (Int i = 0; i < test->nlines; i++) {
            Int offset = test->lines[i];
            token_file_add_line(f, offset);
            if (token_file_line_count(f) != i + 1)
                testing_t_errorf_v(t, "%s, AddLine: got line count %d; want %d",
                                   token_file_name(f), token_file_line_count(f), i + 1);
            /* adding the same offset again should be ignored */
            token_file_add_line(f, offset);
            if (token_file_line_count(f) != i + 1)
                testing_t_errorf_v(t,
                                   "%s, AddLine: got unchanged line count %d; want %d",
                                   token_file_name(f), token_file_line_count(f), i + 1);
            verify_positions(t, a, fset, f, test->lines, i + 1);
        }

        /* add lines with SetLines and verify all positions */
        if (!token_file_set_lines(f, ints(test->lines, test->nlines)))
            testing_t_errorf_v(t, "%s: SetLines failed", token_file_name(f));
        if (token_file_line_count(f) != test->nlines)
            testing_t_errorf_v(t, "%s, SetLines: got line count %d; want %d",
                               token_file_name(f), token_file_line_count(f),
                               test->nlines);
        Slice got = token_file_lines(f);
        bool same = got.len == test->nlines;
        for (Int i = 0; same && i < got.len; i++)
            same = ((const Int *)got.p)[i] == test->lines[i];
        if (!same)
            testing_t_errorf_v(t, "%s, Lines after SetLines(v): got %v; want %v",
                               token_file_name(f), got,
                               ints(test->lines, test->nlines));
        verify_positions(t, a, fset, f, test->lines, test->nlines);

        /* add lines with SetLinesForContent and verify all positions */
        Bytes src;
        if (test->source != NULL) {
            src = bytes_of(cstr(test->source));
        } else {
            /* no test source available - create one from scratch */
            src = make_test_source(a, test->size, test->lines, test->nlines);
        }
        token_file_set_lines_for_content(f, src);
        if (token_file_line_count(f) != test->nlines)
            testing_t_errorf_v(t, "%s, SetLinesForContent: got line count %d; want %d",
                               token_file_name(f), token_file_line_count(f),
                               test->nlines);
        verify_positions(t, a, fset, f, test->lines, test->nlines);
    }
    token_file_set_free(fset);
    arena_free(&ar);
}

static void TestLineInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f =
        token_file_set_add_file(fset, BURROW_S("foo"), token_file_set_base(fset), 500);
    static const Int lines[] = {0, 42, 77, 100, 210, 220, 277, 300, 333, 401};
    Int n = (Int)(sizeof lines / sizeof lines[0]);
    /* add lines individually and provide alternative line information */
    for (Int i = 0; i < n; i++) {
        token_file_add_line(f, lines[i]);
        token_file_add_line_info(f, lines[i], BURROW_S("bar"), 42);
    }
    /* verify positions for all offsets */
    for (Int offs = 0; offs <= token_file_size(f); offs++) {
        TokenPos p = token_file_pos(f, offs);
        Int line = 0;
        Int col = 0;
        linecol(lines, n, offs, &line, &col);
        Str msg =
            fmt_sprintf_v(a, "%s (offs = %d, p = %d)", token_file_name(f), offs, p);
        TokenPosition want = {BURROW_S("bar"), offs, 42, col};
        check_pos(t, msg, token_file_position(f, token_file_pos(f, offs)), want);
        check_pos(t, msg, token_file_set_position(fset, p), want);
    }
    token_file_set_free(fset);
    arena_free(&ar);
}

typedef struct FilesWalk {
    TestingT *t;
    Int j;
} FilesWalk;

static bool files_yield(void *env, TokenFile *f) {
    FilesWalk *w = env;
    Str want = cstr(pos_tests[w->j].filename);
    if (!str_eq(token_file_name(f), want))
        testing_t_errorf_v(w->t, "got filename = %s; want %s", token_file_name(f),
                           want);
    w->j++;
    return true;
}

static void TestFiles(TestingT *t) {
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    for (Int i = 0; i < NPOS_TESTS; i++) {
        Int base = token_file_set_base(fset);
        if (i % 2 == 1) {
            /* Setting a negative base is equivalent to fset.Base(), so test some
             * of each. */
            base = -1;
        }
        token_file_set_add_file(fset, cstr(pos_tests[i].filename), base,
                                pos_tests[i].size);
        FilesWalk w = {t, 0};
        token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, files_yield, &w));
        if (w.j != i + 1)
            testing_t_errorf_v(t, "got %d files; want %d", w.j, i + 1);
    }
    token_file_set_free(fset);
}

/* FileSet.File should return nil if Pos is past the end of the FileSet. */
static void TestFileSetPastEnd(TestingT *t) {
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    for (int k = 0; k < NPOS_TESTS; k++)
        token_file_set_add_file(fset, cstr(pos_tests[k].filename),
                                token_file_set_base(fset), pos_tests[k].size);
    TokenFile *f = token_file_set_file(fset, token_file_set_base(fset));
    if (f != NULL)
        testing_t_errorf_v(t, "got %s, want nil", token_file_name(f));
    token_file_set_free(fset);
}

static void TestFileSetCacheUnlikely(TestingT *t) {
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    Int offsets[NPOS_TESTS];
    for (int k = 0; k < NPOS_TESTS; k++) {
        offsets[k] = token_file_set_base(fset);
        token_file_set_add_file(fset, cstr(pos_tests[k].filename),
                                token_file_set_base(fset), pos_tests[k].size);
    }
    /* Go goes through a map here, so in no set order. Backwards is as unlikely
     * an order for the cache as any. */
    for (int k = NPOS_TESTS - 1; k >= 0; k--) {
        TokenFile *f = token_file_set_file(fset, offsets[k]);
        Str file = cstr(pos_tests[k].filename);
        if (!str_eq(token_file_name(f), file))
            testing_t_errorf_v(t, "got %q at position %d, want %q", token_file_name(f),
                               offsets[k], file);
    }
    token_file_set_free(fset);
}

typedef struct RaceEnv {
    TokenFileSet *fset;
    TokenFile *file;
    MathRandRand *r;
    int32_t max;
    Chan *ch;
} RaceEnv;

static void race_positions(void *env) {
    RaceEnv *e = env;
    for (int i = 0; i < 1000; i++)
        token_file_set_position(e->fset, math_rand_rand_int31n(e->r, e->max));
}

/* issue 4345. Test that concurrent use of FileSet.Pos does not trigger a race in
 * the FileSet position cache. */
static void TestFileSetRace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    for (int i = 0; i < 100; i++)
        token_file_set_add_file(fset, fmt_sprintf_v(a, "file-%d", i),
                                token_file_set_base(fset), 1031);
    int32_t max = (int32_t)token_file_set_base(fset);
    SyncWaitGroup stop = {0};
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 7));
    RaceEnv envs[2];
    for (int i = 0; i < 2; i++) {
        envs[i] = (RaceEnv){fset, NULL, NULL, max, NULL};
        envs[i].r = math_rand_new(a, math_rand_new_source(a, math_rand_rand_int63(r)));
        sync_wait_group_go(&stop, BURROW_FN(Func, race_positions, &envs[i]));
    }
    sync_wait_group_wait(&stop);
    token_file_set_free(fset);
    arena_free(&ar);
    (void)t;
}

enum { RACE2_N = 1000 };

static void race2_add_lines(void *env) {
    RaceEnv *e = env;
    for (Int i = 0; i < RACE2_N; i++)
        token_file_add_line(e->file, i);
    Int one = 1;
    chan_send(e->ch, &one);
}

static void race2_positions(void *env) {
    RaceEnv *e = env;
    TokenPos pos = token_file_pos(e->file, 0);
    for (Int i = 0; i < RACE2_N; i++)
        token_file_set_position_for(e->fset, pos, false);
    Int one = 1;
    chan_send(e->ch, &one);
}

/* issue 16548. Test that concurrent use of File.AddLine and FileSet.PositionFor
 * does not trigger a race in the FileSet position cache. */
static void TestFileSetRace2(TestingT *t) {
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    TokenFile *file = token_file_set_add_file(fset, BURROW_STR_EMPTY, -1, RACE2_N);
    Chan *ch = chan_make(heap_allocator(), TYPE_INT, 2);
    RaceEnv e = {fset, file, NULL, 0, ch};

    if (!go(BURROW_FN(Func, race2_add_lines, &e)) ||
        !go(BURROW_FN(Func, race2_positions, &e))) {
        testing_t_fatalf_v(t, "go failed");
        return;
    }

    Int v = 0;
    chan_recv(ch, &v);
    chan_recv(ch, &v);
    chan_free(ch);
    token_file_set_free(fset);
}

static void TestPositionFor(TestingT *t) {
    Str src = BURROW_S("\n"
                       "foo\n"
                       "b\n"
                       "ar\n"
                       "//line :100\n"
                       "foobar\n"
                       "//line bar:3\n"
                       "done\n");

    Str filename = BURROW_S("foo");
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    TokenFile *f =
        token_file_set_add_file(fset, filename, token_file_set_base(fset), src.len);
    token_file_set_lines_for_content(f, bytes_of(src));
    Slice lines = token_file_lines(f);
    const Int *l = lines.p;

    /* verify position info */
    for (Int i = 0; i < lines.len; i++) {
        Int offs = l[i];
        TokenPosition got1 = token_file_position_for(f, token_file_pos(f, offs), false);
        TokenPosition got2 = token_file_position_for(f, token_file_pos(f, offs), true);
        TokenPosition got3 = token_file_position(f, token_file_pos(f, offs));
        TokenPosition want = {filename, offs, i + 1, 1};
        check_pos(t, BURROW_S("1. PositionFor unadjusted"), got1, want);
        check_pos(t, BURROW_S("1. PositionFor adjusted"), got2, want);
        check_pos(t, BURROW_S("1. Position"), got3, want);
    }

    /* manually add //line info on lines l1, l2 */
    enum { l1 = 5, l2 = 7 };
    token_file_add_line_info(f, l[l1 - 1], BURROW_STR_EMPTY, 100);
    token_file_add_line_info(f, l[l2 - 1], BURROW_S("bar"), 3);

    /* unadjusted position info must remain unchanged */
    for (Int i = 0; i < lines.len; i++) {
        Int offs = l[i];
        TokenPosition got1 = token_file_position_for(f, token_file_pos(f, offs), false);
        TokenPosition want = {filename, offs, i + 1, 1};
        check_pos(t, BURROW_S("2. PositionFor unadjusted"), got1, want);
    }

    /* adjusted position info should have changed */
    for (Int i = 0; i < lines.len; i++) {
        Int offs = l[i];
        TokenPosition got2 = token_file_position_for(f, token_file_pos(f, offs), true);
        TokenPosition got3 = token_file_position(f, token_file_pos(f, offs));
        TokenPosition want = {filename, offs, i + 1, 1};
        /* manually compute wanted filename and line */
        Int line = want.line;
        if (i + 1 >= l1) {
            want.filename = BURROW_STR_EMPTY;
            want.line = line - l1 + 100;
        }
        if (i + 1 >= l2) {
            want.filename = BURROW_S("bar");
            want.line = line - l2 + 3;
        }
        check_pos(t, BURROW_S("3. PositionFor adjusted"), got2, want);
        check_pos(t, BURROW_S("3. Position"), got3, want);
    }
    token_file_set_free(fset);
}

static void TestLineStart(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = BURROW_S("one\ntwo\nthree\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("input"), -1, src.len);
    token_file_set_lines_for_content(f, bytes_of(src));

    for (Int line = 1; line <= 3; line++) {
        TokenPos pos = token_file_line_start(f, line);
        TokenPosition position = token_file_set_position(fset, pos);
        if (position.line != line || position.column != 1)
            testing_t_errorf_v(t, "LineStart(%d) returned wrong pos %d: %s", line, pos,
                               token_position_string(position, a));
    }
    token_file_set_free(fset);
    arena_free(&ar);
}

static bool count_yield(void *env, TokenFile *f) {
    (void)f;
    (*(Int *)env)++;
    return true;
}

static Int num_files(TokenFileSet *fset) {
    Int got = 0;
    token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, count_yield, &got));
    return got;
}

static void remove_check_pos(TestingT *t, Alloc *a, TokenFileSet *fset, TokenPos pos,
                             const char *want) {
    Str got = token_position_string(token_file_set_position(fset, pos), a);
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "Position(%d) = %s, want %s", pos, got, cstr(want));
}

static void remove_check_num_files(TestingT *t, TokenFileSet *fset, Int want) {
    Int got = num_files(fset);
    if (got != want)
        testing_t_errorf_v(t, "Iterate called %d times, want %d", got, want);
}

static void TestRemoveFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *as = arena_allocator(&ar);
    Str content_a = BURROW_S("this\nis\nfileA");
    Str content_b = BURROW_S("this\nis\nfileB");
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    TokenFile *a = token_file_set_add_file(fset, BURROW_S("fileA"), -1, content_a.len);
    token_file_set_lines_for_content(a, bytes_of(content_a));
    TokenFile *b = token_file_set_add_file(fset, BURROW_S("fileB"), -1, content_b.len);
    token_file_set_lines_for_content(b, bytes_of(content_b));

    TokenPos apos3 = token_file_pos(a, 3);
    TokenPos bpos3 = token_file_pos(b, 3);
    remove_check_pos(t, as, fset, apos3, "fileA:1:4");
    remove_check_pos(t, as, fset, bpos3, "fileB:1:4");
    remove_check_num_files(t, fset, 2);

    /* After removal, queries on fileA fail. */
    token_file_set_remove_file(fset, a);
    remove_check_pos(t, as, fset, apos3, "-");
    remove_check_pos(t, as, fset, bpos3, "fileB:1:4");
    remove_check_num_files(t, fset, 1);

    /* idempotent / no effect */
    token_file_set_remove_file(fset, a);
    remove_check_pos(t, as, fset, apos3, "-");
    remove_check_pos(t, as, fset, bpos3, "fileB:1:4");
    remove_check_num_files(t, fset, 1);

    token_file_set_free(fset);
    arena_free(&ar);
}

typedef struct LineInfo {
    Int offset;
    const char *filename;
    Int line;
    Int column;
} LineInfo;

typedef struct LineColumnTest {
    const char *name;
    LineInfo infos[3];
    Int ninfos;
    LineInfo want[3];
    Int nwant;
} LineColumnTest;

enum { filesize = 100 };
#define FILENAME "test.go"

static const LineColumnTest line_column_tests[] = {
    {"normal",
     {{10, FILENAME, 2, 1}, {50, FILENAME, 3, 1}, {80, FILENAME, 4, 2}},
     3,
     {{10, FILENAME, 2, 1}, {50, FILENAME, 3, 1}, {80, FILENAME, 4, 2}},
     3},
    {"offset1 == file size", {{filesize, FILENAME, 2, 1}}, 1, {{0}}, 0},
    {"offset1 > file size", {{filesize + 1, FILENAME, 2, 1}}, 1, {{0}}, 0},
    {"offset2 == file size",
     {{10, FILENAME, 2, 1}, {filesize, FILENAME, 3, 1}},
     2,
     {{10, FILENAME, 2, 1}},
     1},
    {"offset2 > file size",
     {{10, FILENAME, 2, 1}, {filesize + 1, FILENAME, 3, 1}},
     2,
     {{10, FILENAME, 2, 1}},
     1},
    {"offset2 == offset1",
     {{10, FILENAME, 2, 1}, {10, FILENAME, 3, 1}},
     2,
     {{10, FILENAME, 2, 1}},
     1},
    {"offset2 < offset1",
     {{10, FILENAME, 2, 1}, {9, FILENAME, 3, 1}},
     2,
     {{10, FILENAME, 2, 1}},
     1},
};

static void line_column_case(void *env, TestingT *t) {
    const LineColumnTest *test = env;
    TokenFileSet *fs = token_new_file_set(heap_allocator());
    TokenFile *f = token_file_set_add_file(fs, BURROW_S(FILENAME), -1, filesize);
    for (Int i = 0; i < test->ninfos; i++) {
        const LineInfo *info = &test->infos[i];
        token_file_add_line_column_info(f, info->offset, cstr(info->filename),
                                        info->line, info->column);
    }
    Int n = 0;
    Int offset = 0;
    Int line = 0;
    Int column = 0;
    Str filename = BURROW_STR_EMPTY;
    bool same = true;
    for (; burrow__token_file_info(f, n, &offset, &filename, &line, &column); n++) {
        if (n >= test->nwant)
            continue;
        const LineInfo *w = &test->want[n];
        if (offset != w->offset || !str_eq(filename, cstr(w->filename)) ||
            line != w->line || column != w->column)
            same = false;
    }
    if (!same || n != test->nwant) {
        testing_t_errorf_v(t, "got %d infos, want %d, or they differ", n, test->nwant);
        for (Int i = 0;
             burrow__token_file_info(f, i, &offset, &filename, &line, &column); i++)
            testing_t_logf_v(t, "got {Offset:%d Filename:%s Line:%d Column:%d}", offset,
                             filename, line, column);
    }
    token_file_set_free(fs);
}

static void TestFileAddLineColumnInfo(TestingT *t) {
    Int n = (Int)(sizeof line_column_tests / sizeof line_column_tests[0]);
    for (Int i = 0; i < n; i++) {
        const LineColumnTest *test = &line_column_tests[i];
        testing_t_run(
            t, cstr(test->name),
            BURROW_FN(TestingTFunc, line_column_case, (void *)(uintptr_t)test));
    }
}

static void TestIssue57490(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    enum { fsize = 5 };
    TokenFileSet *fset = token_new_file_set(a);
    Int base = token_file_set_base(fset);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("f"), base, fsize);

    /* out-of-bounds positions must not lead to a panic when calling f.Offset */
    Int got = token_file_offset(f, TOKEN_NO_POS);
    if (got != 0)
        testing_t_errorf_v(t, "offset = %d, want %d", got, (Int)0);
    got = token_file_offset(f, -1);
    if (got != 0)
        testing_t_errorf_v(t, "offset = %d, want %d", got, (Int)0);
    got = token_file_offset(f, base + fsize + 1);
    if (got != fsize)
        testing_t_errorf_v(t, "offset = %d, want %d", got, (Int)fsize);

    /* out-of-bounds offsets must not lead to a panic when calling f.Pos */
    TokenPos gotp = token_file_pos(f, -1);
    if (gotp != base)
        testing_t_errorf_v(t, "pos = %d, want %d", gotp, base);
    gotp = token_file_pos(f, fsize + 1);
    if (gotp != base + fsize)
        testing_t_errorf_v(t, "pos = %d, want %d", gotp, base + fsize);

    /* out-of-bounds Pos values must not lead to a panic when calling
     * f.Position */
    Str want = fmt_sprintf_v(a, "%s:1:1", token_file_name(f));
    Str gots = token_position_string(token_file_position(f, -1), a);
    if (!str_eq(gots, want))
        testing_t_errorf_v(t, "position = %s, want %s", gots, want);
    want = fmt_sprintf_v(a, "%s:1:%d", token_file_name(f), (Int)fsize + 1);
    gots = token_position_string(token_file_position(f, fsize + 1), a);
    if (!str_eq(gots, want))
        testing_t_errorf_v(t, "position = %s, want %s", gots, want);

    /* check invariants */
    enum { xsize = fsize + 5 };
    for (Int offset = -xsize; offset < xsize; offset++) {
        Int want1 = token_file_offset(f, token_file_base(f) + offset);
        got = token_file_offset(f, token_file_pos(f, offset));
        if (got != want1)
            testing_t_errorf_v(t, "offset = %d, want %d", got, want1);

        TokenPos want2 = token_file_pos(f, offset);
        gotp = token_file_pos(f, token_file_offset(f, want2));
        if (gotp != want2)
            testing_t_errorf_v(t, "pos = %d, want %d", gotp, want2);
    }
    token_file_set_free(fset);
    arena_free(&ar);
}

typedef struct FsetString {
    BytesBuffer *buf;
    const char *sep;
} FsetString;

static bool fset_string_yield(void *env, TokenFile *f) {
    FsetString *s = env;
    fmt_fprintf_v(bytes_buffer_as_io_writer(s->buf), "%s%s:%d-%d", cstr(s->sep),
                  token_file_name(f), token_file_base(f), token_file_end(f));
    s->sep = " ";
    return true;
}

static Str fset_string(Alloc *a, TokenFileSet *fset) {
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, BURROW_S("{"), NULL);
    FsetString s = {&buf, ""};
    token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, fset_string_yield, &s));
    bytes_buffer_write_string(&buf, BURROW_S("}"), NULL);
    Str out = bytes_buffer_string(&buf, a);
    bytes_buffer_free(&buf);
    return out;
}

static void add_existing_check(TestingT *t, Alloc *a, TokenFileSet *fset,
                               const char *descr, const char *want) {
    Str got = fset_string(a, fset);
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "%s: got %s, want %s", cstr(descr), got, cstr(want));
}

static void TestFileSet_AddExistingFiles(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);

    TokenFile *file_a = token_file_set_add_file(fset, BURROW_S("A"), -1, 3);
    token_file_set_add_file(fset, BURROW_S("B"), -1, 5);
    add_existing_check(t, a, fset, "after AddFile [AB]", "{A:1-4 B:5-10}");

    token_file_set_add_existing_files(fset, NULL, 0); /* noop */
    add_existing_check(t, a, fset, "after AddExistingFiles []", "{A:1-4 B:5-10}");

    TokenFileSet *set_c = token_new_file_set(a);
    TokenFileSet *set_d = token_new_file_set(a);
    TokenFile *file_c = token_file_set_add_file(set_c, BURROW_S("C"), 100, 5);
    TokenFile *file_d = token_file_set_add_file(set_d, BURROW_S("D"), 200, 5);
    TokenFile *const files[] = {file_c, file_a, file_d, file_c};
    token_file_set_add_existing_files(fset, files, 4);
    add_existing_check(t, a, fset, "after AddExistingFiles [CADC]",
                       "{A:1-4 B:5-10 C:100-105 D:200-205}");

    token_file_set_add_file(fset, BURROW_S("E"), -1, 3);
    add_existing_check(t, a, fset, "after AddFile [E]",
                       "{A:1-4 B:5-10 C:100-105 D:200-205 E:206-209}");

    token_file_set_free(fset);
    token_file_set_free(set_c);
    token_file_set_free(set_d);
    arena_free(&ar);
}

enum { RACE_FILES = 20000 };

typedef struct RemoveRace {
    TokenFileSet *fset;
    TokenFile **files;
    Chan *race1;
    Chan *race2;
    Chan *start;
    SyncWaitGroup done;
} RemoveRace;

/* governor goroutine */
static void remove_race_governor(void *env) {
    RemoveRace *r = env;
    Int v = 0;
    for (Int i = 0; i < RACE_FILES; i++) {
        chan_recv(r->start, &v);
        chan_send(r->race1, &i);
        chan_send(r->race2, &i);
    }
    chan_recv(r->start, &v); /* unlock main test goroutine */
    chan_close(r->race1);
    chan_close(r->race2);
}

static void remove_race_lookup(void *env) {
    RemoveRace *r = env;
    Int i = 0;
    while (chan_recv(r->race1, &i)) {
        TokenFile *f = r->files[i];
        token_file_set_file(r->fset,
                            token_file_base(f) + 5); /* populates s.last with f */
    }
}

/* Test that File() does not return the already removed file, while used
 * concurrently. */
static void TestRemoveFileRace(TestingT *t) {
    Alloc *a = heap_allocator();
    RemoveRace r = {0};
    r.fset = token_new_file_set(a);

    /* Create bunch of files. */
    r.files = mem_alloc_array(a, RACE_FILES, sizeof *r.files, _Alignof(TokenFile *));
    for (Int i = 0; i < RACE_FILES; i++)
        r.files[i] = token_file_set_add_file(r.fset, BURROW_S("f"), -1, (i + 1) * 10);

    r.race1 = chan_make(a, TYPE_INT, 0);
    r.race2 = chan_make(a, TYPE_INT, 0);
    r.start = chan_make(a, TYPE_INT, 0);
    sync_wait_group_go(&r.done, BURROW_FN(Func, remove_race_governor, &r));
    sync_wait_group_go(&r.done, BURROW_FN(Func, remove_race_lookup, &r));

    Int v = 0;
    chan_send(r.start, &v);
    Int i = 0;
    bool failed = false;
    while (chan_recv(r.race2, &i)) {
        TokenFile *f = r.files[i];
        if (!failed) {
            token_file_set_remove_file(r.fset, f);
            TokenFile *got = token_file_set_file(r.fset, token_file_base(f) + 5);
            if (got != NULL) {
                testing_t_errorf_v(t, "file was not removed correctly");
                failed = true;
            }
        }
        chan_send(r.start, &v);
    }
    sync_wait_group_wait(&r.done);

    chan_free(r.race1);
    chan_free(r.race2);
    chan_free(r.start);
    mem_free(a, r.files, RACE_FILES * sizeof *r.files, _Alignof(TokenFile *));
    token_file_set_free(r.fset);
}

static void swap_files(void *env, Int i, Int j) {
    TokenFile **files = env;
    TokenFile *f = files[i];
    files[i] = files[j];
    files[j] = f;
}

static void TestRemovedFileFileReturnsNil(TestingT *t) {
    Alloc *a = heap_allocator();
    TokenFileSet *fset = token_new_file_set(a);

    /* Create bunch of files. */
    enum { n = 1000 };
    TokenFile **files = mem_alloc_array(a, n, sizeof *files, _Alignof(TokenFile *));
    for (Int i = 0; i < n; i++)
        files[i] = token_file_set_add_file(fset, BURROW_S("f"), -1, (i + 1) * 100);

    math_rand_shuffle(n, BURROW_FN(SwapFunc, swap_files, files));

    for (Int i = 0; i < n; i++) {
        TokenFile *f = files[i];
        token_file_set_remove_file(fset, f);
        TokenFile *got = token_file_set_file(fset, token_file_base(f) + 10);
        if (got != NULL) {
            testing_t_fatalf_v(t,
                               "file was not removed correctly; got file with base: %v",
                               token_file_base(got));
            break;
        }
    }
    mem_free(a, files, n * sizeof *files, _Alignof(TokenFile *));
    token_file_set_free(fset);
}

static void TestFile_End(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("a.go"), 100, 42);
    Str got = fmt_sprintf_v(a, "%d, %d", token_file_base(f), token_file_end(f));
    Str want = BURROW_S("100, 142");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Base, End = %s, want %s", got, want);
    token_file_set_free(fset);
    arena_free(&ar);
}

static void TestFile_String(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("a.go"), 100, 42);
    Str got = token_file_string(f, a);
    Str want = BURROW_S("a.go(100-142)");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "String = %q, want %q", got, want);
    token_file_set_free(fset);
    arena_free(&ar);
}

/* ------------------------------------------------------------ serialization */

typedef struct Collect {
    TokenFile *files[16];
    Int n;
} Collect;

static bool collect_yield(void *env, TokenFile *f) {
    Collect *c = env;
    if (c->n < 16)
        c->files[c->n] = f;
    c->n++;
    return true;
}

static Str equal(Alloc *a, TokenFileSet *p, TokenFileSet *q) {
    if (p == q) {
        /* avoid deadlock if p == q */
        return BURROW_STR_EMPTY;
    }

    if (token_file_set_base(p) != token_file_set_base(q))
        return fmt_sprintf_v(a, "different bases: %d != %d", token_file_set_base(p),
                             token_file_set_base(q));

    Collect pfiles = {{0}, 0};
    Collect qfiles = {{0}, 0};
    token_file_set_iterate(p, BURROW_FN(TokenFileFunc, collect_yield, &pfiles));
    token_file_set_iterate(q, BURROW_FN(TokenFileFunc, collect_yield, &qfiles));
    if (pfiles.n != qfiles.n)
        return fmt_sprintf_v(a, "different number of files: %d != %d", pfiles.n,
                             qfiles.n);

    for (Int i = 0; i < pfiles.n && i < 16; i++) {
        TokenFile *f = pfiles.files[i];
        TokenFile *g = qfiles.files[i];
        if (!str_eq(token_file_name(f), token_file_name(g)))
            return fmt_sprintf_v(a, "different filenames: %q != %q", token_file_name(f),
                                 token_file_name(g));
        if (token_file_base(f) != token_file_base(g))
            return fmt_sprintf_v(a, "different base for %q: %d != %d",
                                 token_file_name(f), token_file_base(f),
                                 token_file_base(g));
        if (token_file_size(f) != token_file_size(g))
            return fmt_sprintf_v(a, "different size for %q: %d != %d",
                                 token_file_name(f), token_file_size(f),
                                 token_file_size(g));
        Slice fl = token_file_lines(f);
        Slice gl = token_file_lines(g);
        for (Int j = 0; j < fl.len; j++) {
            if (j >= gl.len || ((const Int *)fl.p)[j] != ((const Int *)gl.p)[j])
                return fmt_sprintf_v(a, "different offsets for %q", token_file_name(f));
        }
        Int lo = 0, ll = 0, lc = 0, mo = 0, ml = 0, mc = 0;
        Str lf = BURROW_STR_EMPTY;
        Str mf = BURROW_STR_EMPTY;
        for (Int j = 0; burrow__token_file_info(f, j, &lo, &lf, &ll, &lc); j++) {
            if (!burrow__token_file_info(g, j, &mo, &mf, &ml, &mc) || lo != mo ||
                !str_eq(lf, mf) || ll != ml)
                return fmt_sprintf_v(a, "different infos for %q", token_file_name(f));
        }
    }

    /* we don't care about .last - it's just a cache */
    return BURROW_STR_EMPTY;
}

typedef struct Codec {
    Alloc *a;
    BytesBuffer *buf;
} Codec;

static Error gob_encode(void *env, Any x) {
    Codec *c = env;
    GobEncoder *enc = gob_new_encoder(c->a, bytes_buffer_as_io_writer(c->buf));
    Error err = gob_encoder_encode(enc, x);
    gob_encoder_free(enc);
    return err;
}

static Error gob_decode(void *env, Any x) {
    Codec *c = env;
    GobDecoder *dec = gob_new_decoder(c->a, bytes_buffer_as_io_reader(c->buf));
    Error err = gob_decoder_decode(dec, x);
    gob_decoder_free(dec);
    return err;
}

static void check_serialize(TestingT *t, TokenFileSet *p) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Codec c = {a, &buf};
    Error err = token_file_set_write(p, BURROW_FN(TokenCodecFunc, gob_encode, &c));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "writing fileset failed: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    TokenFileSet *q = token_new_file_set(heap_allocator());
    err = token_file_set_read(q, BURROW_FN(TokenCodecFunc, gob_decode, &c));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "reading fileset failed: %s", error_text(err));
    } else {
        Str msg = equal(a, p, q);
        if (msg.len > 0)
            testing_t_errorf_v(t, "filesets not identical: %s", msg);
    }
    token_file_set_free(q);
    arena_free(&ar);
}

static void TestSerialization(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *p = token_new_file_set(heap_allocator());
    check_serialize(t, p);
    /* add some files */
    for (Int i = 0; i < 10; i++) {
        TokenFile *f = token_file_set_add_file(p, fmt_sprintf_v(a, "file%d", i),
                                               token_file_set_base(p) + i, i * 100);
        check_serialize(t, p);
        /* add some lines and alternative file infos */
        Int line = 1000;
        for (Int offs = 0; offs < token_file_size(f); offs += 40 + i) {
            token_file_add_line(f, offs);
            if (offs % 7 == 0) {
                token_file_add_line_info(f, offs, fmt_sprintf_v(a, "file%d", offs),
                                         line);
                line += 33;
            }
        }
        check_serialize(t, p);
    }
    token_file_set_free(p);
    arena_free(&ar);
}

/* -------------------------------------------------------------------- tree */

typedef struct TreeWalk {
    TokenFile **want;
    Int n;
    Int i;
    bool ok;
} TreeWalk;

static bool tree_walk_yield(void *env, TokenFile *f) {
    TreeWalk *w = env;
    if (w->i >= w->n || w->want[w->i] != f)
        w->ok = false;
    w->i++;
    return true;
}

/* Go's TestTree works on the tree inside a FileSet. That tree is not to be had
 * from C, so this works on it from outside: the files go in through
 * token_file_set_add_existing_files, which is tree.add, come out through
 * token_file_set_remove_file, which is tree.delete, and are looked up through
 * token_file_set_file, which is tree.locate. */
static void TestTree(TestingT *t) {
    Alloc *a = heap_allocator();
    /* Use a reproducible PRNG. */
    int64_t seed = math_rand_int63();
    testing_t_logf_v(t, "random seed: %d", seed);
    Arena ar;
    arena_init(&ar, NULL, 0);
    MathRandRand *rng = math_rand_new(arena_allocator(&ar),
                                      math_rand_new_source(arena_allocator(&ar), seed));

    /* Create a number of Files of arbitrary size. */
    enum { nfiles = 500 };
    TokenFileSet *owner = token_new_file_set(a);
    TokenFile **files =
        mem_alloc_array(a, nfiles, sizeof *files, _Alignof(TokenFile *));
    TokenFile **files2 =
        mem_alloc_array(a, nfiles, sizeof *files, _Alignof(TokenFile *));
    Int base = 0;
    for (Int i = 0; i < nfiles; i++) {
        base++;
        Int size = 1000;
        files[i] = token_file_set_add_file(owner, BURROW_S("f"), base, size);
        files2[i] = files[i];
        base += size;
    }

    /* Add them all to the tree in random order. */
    TokenFileSet *tr = token_new_file_set(a);
    math_rand_rand_shuffle(rng, nfiles, BURROW_FN(SwapFunc, swap_files, files2));
    for (Int i = 0; i < nfiles; i++)
        token_file_set_add_existing_files(tr, &files2[i], 1);

    /* Randomly delete a subset of them. */
    for (int k = 0; k < 100; k++) {
        Int i = math_rand_rand_intn(rng, nfiles);
        TokenFile *file = files[i];
        if (file == NULL)
            continue; /* already deleted */
        files[i] = NULL;

        if (token_file_set_file(tr, token_file_base(file)) != file) {
            testing_t_fatalf_v(t, "locate returned wrong file");
            goto out;
        }
        token_file_set_remove_file(tr, file);
    }

    /* Check some position lookups within each file. */
    for (Int i = 0; i < nfiles; i++) {
        TokenFile *file = files[i];
        if (file == NULL)
            continue; /* deleted */
        Int fb = token_file_base(file);
        Int fs = token_file_size(file);
        const Int poss[] = {fb, fb + fs / 2, fb + fs};
        for (int j = 0; j < 3; j++) {
            TokenFile *got = token_file_set_file(tr, poss[j]);
            if (got != file) {
                testing_t_fatalf_v(t, "lookup %s@%d returned wrong file %v",
                                   token_file_name(file), poss[j],
                                   got != NULL ? token_file_base(got) : (Int)-1);
                goto out;
            }
        }
    }

    /* Check that the sequence is the same. */
    Int n = 0;
    for (Int i = 0; i < nfiles; i++) {
        if (files[i] != NULL)
            files[n++] = files[i];
    }
    TreeWalk w = {files, n, 0, true};
    token_file_set_iterate(tr, BURROW_FN(TokenFileFunc, tree_walk_yield, &w));
    if (!w.ok || w.i != n)
        testing_t_fatalf_v(t, "incorrect tree.all sequence");

out:
    token_file_set_free(tr);
    token_file_set_free(owner);
    mem_free(a, files, nfiles * sizeof *files, _Alignof(TokenFile *));
    mem_free(a, files2, nfiles * sizeof *files, _Alignof(TokenFile *));
    arena_free(&ar);
}

/* ------------------------------------------------------------------ tokens */

typedef struct IdentTest {
    const char *name;
    const char *in;
    bool want;
} IdentTest;

static const IdentTest ident_tests[] = {
    {"Empty", "", false},
    {"Space", " ", false},
    {"SpaceSuffix", "foo ", false},
    {"Number", "123", false},
    {"Keyword", "func", false},

    {"LettersASCII", "foo", true},
    {"MixedASCII", "_bar123", true},
    {"UppercaseKeyword", "Func", true},
    {"LettersUnicode", "f\xc3\xb3\xc3\xb6", true},
};

static void is_identifier_case(void *env, TestingT *t) {
    const IdentTest *test = env;
    bool got = token_is_identifier(cstr(test->in));
    if (got != test->want)
        testing_t_fatalf_v(t, "IsIdentifier(%q) = %t, want %v", cstr(test->in), got,
                           test->want);
}

static void TestIsIdentifier(TestingT *t) {
    Int n = (Int)(sizeof ident_tests / sizeof ident_tests[0]);
    for (Int i = 0; i < n; i++) {
        const IdentTest *test = &ident_tests[i];
        testing_t_run(
            t, cstr(test->name),
            BURROW_FN(TestingTFunc, is_identifier_case, (void *)(uintptr_t)test));
    }
}

/* Not in Go: every token's string looks itself back up, keywords as
 * themselves and everything else as an identifier, and the predicates agree
 * with the ranges the token is in. */
static void TestLookupRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Token tok = TOKEN_ILLEGAL; tok <= TOKEN_TILDE; tok++) {
        Str s = token_string(tok, a);
        if (s.len == 0) {
            testing_t_errorf_v(t, "token %d has an empty string", tok);
            continue;
        }
        Token got = token_lookup(s);
        Token want = token_is_keyword(tok) ? tok : TOKEN_IDENT;
        if (got != want)
            testing_t_errorf_v(t, "Lookup(%q) = %d, want %d", s, got, want);
        if (token_is_keyword(tok) != token_is_keyword_str(s))
            testing_t_errorf_v(t, "IsKeyword(%q) = %t, want %t", s,
                               token_is_keyword_str(s), token_is_keyword(tok));
        int kinds =
            token_is_literal(tok) + token_is_operator(tok) + token_is_keyword(tok);
        if (kinds > 1)
            testing_t_errorf_v(t, "token %q is in %d classes", s, (Int)kinds);
    }
    Str got = token_string(200, a);
    if (!str_eq(got, BURROW_S("token(200)")))
        testing_t_errorf_v(t, "Token(200).String() = %q, want %q", got,
                           BURROW_S("token(200)"));
    arena_free(&ar);
}

/* Not in Go: the panics AddFile and LineStart have, with Go's messages, and the
 * set still works after them. */
static TokenFileSet *panic_set;
static Int panic_base;
static Int panic_size;

static void add_file_panics(void) {
    token_file_set_add_file(panic_set, BURROW_S("p"), panic_base, panic_size);
}

static Byte panic_buf[200];

static Str recovered(void (*f)(void)) {
    volatile Int n = -1;
    BURROW_TRY {
        f();
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < (Int)sizeof panic_buf ? m.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0)
        return (Str){NULL, 0};
    return (Str){panic_buf, n};
}

static void want_panic(TestingT *t, Str got, Str want) {
    if (got.p == NULL)
        testing_t_errorf_v(t, "failed to panic, want %q", want);
    else if (!str_eq(got, want))
        testing_t_errorf_v(t, "wrong panic message: got %q, want %q", got, want);
}

static TokenFile *panic_file;
static Int panic_line;

static void line_start_panics(void) {
    token_file_line_start(panic_file, panic_line);
}

static void TestAddFilePanics(TestingT *t) {
    panic_set = token_new_file_set(heap_allocator());
    token_file_set_add_file(panic_set, BURROW_S("a"), -1, 10);

    panic_base = 5;
    panic_size = 1;
    want_panic(t, recovered(add_file_panics),
               BURROW_S("invalid base 5 (should be >= 12)"));
    panic_base = -1;
    panic_size = -1;
    want_panic(t, recovered(add_file_panics),
               BURROW_S("invalid size -1 (should be >= 0)"));
    panic_size = INT64_MAX - 5;
    want_panic(t, recovered(add_file_panics),
               BURROW_S("token.Pos offset overflow (> 2G of source code in file set)"));

    /* The set is unlocked and unchanged after each of those. */
    panic_file = token_file_set_add_file(panic_set, BURROW_S("b"), -1, 3);
    if (token_file_base(panic_file) != 12)
        testing_t_errorf_v(t, "base after panics = %d, want 12",
                           token_file_base(panic_file));

    panic_line = 0;
    want_panic(t, recovered(line_start_panics),
               BURROW_S("invalid line number 0 (should be >= 1)"));
    panic_line = 2;
    want_panic(t, recovered(line_start_panics),
               BURROW_S("invalid line number 2 (should be < 1)"));

    token_file_set_free(panic_set);
}

/* Not in Go: a file removed while Iterate is in yield is passed over, and the
 * walk carries on from where it was. */
typedef struct RemoveWalk {
    TokenFileSet *fset;
    TokenFile **files;
    Int seen[8];
    Int n;
} RemoveWalk;

static bool remove_walk_yield(void *env, TokenFile *f) {
    RemoveWalk *w = env;
    w->seen[w->n++] = token_file_base(f);
    if (f == w->files[1]) {
        /* the file being visited and the one after it */
        token_file_set_remove_file(w->fset, w->files[1]);
        token_file_set_remove_file(w->fset, w->files[2]);
    }
    return w->n < 8;
}

static void TestIterateRemove(TestingT *t) {
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    TokenFile *files[5];
    for (int i = 0; i < 5; i++)
        files[i] = token_file_set_add_file(fset, BURROW_S("f"), -1, 1);
    RemoveWalk w = {fset, files, {0}, 0};
    token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, remove_walk_yield, &w));
    const Int want[] = {1, 3, 7, 9};
    bool ok = w.n == 4;
    for (Int i = 0; ok && i < 4; i++)
        ok = w.seen[i] == want[i];
    if (!ok)
        testing_t_errorf_v(t, "saw %d files, bases %v, want %v", w.n, ints(w.seen, w.n),
                           ints(want, 4));
    if (num_files(fset) != 3)
        testing_t_errorf_v(t, "%d files left, want 3", num_files(fset));
    token_file_set_free(fset);
}

#define TESTS(X)                                                                       \
    X(TestNoPos)                                                                       \
    X(TestPositions)                                                                   \
    X(TestLineInfo)                                                                    \
    X(TestFiles)                                                                       \
    X(TestFileSetPastEnd)                                                              \
    X(TestFileSetCacheUnlikely)                                                        \
    X(TestFileSetRace)                                                                 \
    X(TestFileSetRace2)                                                                \
    X(TestPositionFor)                                                                 \
    X(TestLineStart)                                                                   \
    X(TestRemoveFile)                                                                  \
    X(TestFileAddLineColumnInfo)                                                       \
    X(TestIssue57490)                                                                  \
    X(TestFileSet_AddExistingFiles)                                                    \
    X(TestRemoveFileRace)                                                              \
    X(TestRemovedFileFileReturnsNil)                                                   \
    X(TestFile_End)                                                                    \
    X(TestFile_String)                                                                 \
    X(TestSerialization)                                                               \
    X(TestTree)                                                                        \
    X(TestIsIdentifier)                                                                \
    X(TestLookupRoundTrip)                                                             \
    X(TestAddFilePanics)                                                               \
    X(TestIterateRemove)

TESTING_MAIN(TESTS)
