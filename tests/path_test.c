/* Derived from Go's src/path/path_test.go and src/path/match_test.go.
 * Go source: go1.27.1.
 *
 * The tables came out of burrow-gen tests and the test bodies were ported by
 * hand. The tests after TestMatch are not from Go: they cover the variadic
 * join, running out of memory and the case where Go checks the rest of a
 * pattern.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/path.h"

#define S BURROW_S
#define SI BURROW_S_INIT
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

typedef struct PathTest {
    Str path;
    Str result;
} PathTest;

typedef struct SplitTest {
    Str path;
    Str dir;
    Str file;
} SplitTest;

typedef struct JoinTest {
    const Str *elem;
    Int elem_len;
    Str path;
} JoinTest;

typedef struct ExtTest {
    Str path;
    Str ext;
} ExtTest;

typedef struct IsAbsTest {
    Str path;
    bool isAbs;
} IsAbsTest;

static const PathTest cleantests[] = {
    {SI(""), SI(".")},
    {SI("abc"), SI("abc")},
    {SI("abc/def"), SI("abc/def")},
    {SI("a/b/c"), SI("a/b/c")},
    {SI("."), SI(".")},
    {SI(".."), SI("..")},
    {SI("../.."), SI("../..")},
    {SI("../../abc"), SI("../../abc")},
    {SI("/abc"), SI("/abc")},
    {SI("/"), SI("/")},
    {SI("abc/"), SI("abc")},
    {SI("abc/def/"), SI("abc/def")},
    {SI("a/b/c/"), SI("a/b/c")},
    {SI("./"), SI(".")},
    {SI("../"), SI("..")},
    {SI("../../"), SI("../..")},
    {SI("/abc/"), SI("/abc")},
    {SI("abc//def//ghi"), SI("abc/def/ghi")},
    {SI("//abc"), SI("/abc")},
    {SI("///abc"), SI("/abc")},
    {SI("//abc//"), SI("/abc")},
    {SI("abc//"), SI("abc")},
    {SI("abc/./def"), SI("abc/def")},
    {SI("/./abc/def"), SI("/abc/def")},
    {SI("abc/."), SI("abc")},
    {SI("abc/def/ghi/../jkl"), SI("abc/def/jkl")},
    {SI("abc/def/../ghi/../jkl"), SI("abc/jkl")},
    {SI("abc/def/.."), SI("abc")},
    {SI("abc/def/../.."), SI(".")},
    {SI("/abc/def/../.."), SI("/")},
    {SI("abc/def/../../.."), SI("..")},
    {SI("/abc/def/../../.."), SI("/")},
    {SI("abc/def/../../../ghi/jkl/../../../mno"), SI("../../mno")},
    {SI("abc/./../def"), SI("def")},
    {SI("abc//./../def"), SI("def")},
    {SI("abc/../../././../def"), SI("../../def")},
};

static const SplitTest splittests[] = {
    {SI("a/b"), SI("a/"), SI("b")}, {SI("a/b/"), SI("a/b/"), SI("")},
    {SI("a/"), SI("a/"), SI("")},   {SI("a"), SI(""), SI("a")},
    {SI("/"), SI("/"), SI("")},
};

static const JoinTest jointests[] = {
    {NULL, 0, SI("")},
    {(const Str[]){SI("")}, 1, SI("")},
    {(const Str[]){SI("a")}, 1, SI("a")},
    {(const Str[]){SI("a"), SI("b")}, 2, SI("a/b")},
    {(const Str[]){SI("a"), SI("")}, 2, SI("a")},
    {(const Str[]){SI(""), SI("b")}, 2, SI("b")},
    {(const Str[]){SI("/"), SI("a")}, 2, SI("/a")},
    {(const Str[]){SI("/"), SI("")}, 2, SI("/")},
    {(const Str[]){SI("a/"), SI("b")}, 2, SI("a/b")},
    {(const Str[]){SI("a/"), SI("")}, 2, SI("a")},
    {(const Str[]){SI(""), SI("")}, 2, SI("")},
};

static const ExtTest exttests[] = {
    {SI("path.go"), SI(".go")}, {SI("path.pb.go"), SI(".go")},
    {SI("a.dir/b"), SI("")},    {SI("a.dir/b.go"), SI(".go")},
    {SI("a.dir/"), SI("")},
};

static const PathTest basetests[] = {
    {SI(""), SI(".")},        {SI("."), SI(".")},         {SI("/."), SI(".")},
    {SI("/"), SI("/")},       {SI("////"), SI("/")},      {SI("x/"), SI("x")},
    {SI("abc"), SI("abc")},   {SI("abc/def"), SI("def")}, {SI("a/b/.x"), SI(".x")},
    {SI("a/b/c."), SI("c.")}, {SI("a/b/c.x"), SI("c.x")},
};

static const PathTest dirtests[] = {
    {SI(""), SI(".")},          {SI("."), SI(".")},
    {SI("/."), SI("/")},        {SI("/"), SI("/")},
    {SI("////"), SI("/")},      {SI("/foo"), SI("/")},
    {SI("x/"), SI("x")},        {SI("abc"), SI(".")},
    {SI("abc/def"), SI("abc")}, {SI("abc////def"), SI("abc")},
    {SI("a/b/.x"), SI("a/b")},  {SI("a/b/c."), SI("a/b")},
    {SI("a/b/c.x"), SI("a/b")},
};

static const IsAbsTest isAbsTests[] = {
    {SI(""), false},   {SI("/"), true},        {SI("/usr/bin/gcc"), true},
    {SI(".."), false}, {SI("/a/../bb"), true}, {SI("."), false},
    {SI("./"), false}, {SI("lala"), false},
};

typedef struct MatchTest {
    Str pattern;
    Str s;
    bool match;
    const Error *err;
} MatchTest;

static const MatchTest matchTests[] = {
    {SI("abc"), SI("abc"), true, NULL},
    {SI("*"), SI("abc"), true, NULL},
    {SI("*c"), SI("abc"), true, NULL},
    {SI("a*"), SI("a"), true, NULL},
    {SI("a*"), SI("abc"), true, NULL},
    {SI("a*"), SI("ab/c"), false, NULL},
    {SI("a*/b"), SI("abc/b"), true, NULL},
    {SI("a*/b"), SI("a/c/b"), false, NULL},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxe/f"), true, NULL},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxexxx/f"), true, NULL},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxe/xxx/f"), false, NULL},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxexxx/fff"), false, NULL},
    {SI("a*b?c*x"), SI("abxbbxdbxebxczzx"), true, NULL},
    {SI("a*b?c*x"), SI("abxbbxdbxebxczzy"), false, NULL},
    {SI("ab[c]"), SI("abc"), true, NULL},
    {SI("ab[b-d]"), SI("abc"), true, NULL},
    {SI("ab[e-g]"), SI("abc"), false, NULL},
    {SI("ab[^c]"), SI("abc"), false, NULL},
    {SI("ab[^b-d]"), SI("abc"), false, NULL},
    {SI("ab[^e-g]"), SI("abc"), true, NULL},
    {SI("a\\*b"), SI("a*b"), true, NULL},
    {SI("a\\*b"), SI("ab"), false, NULL},
    {SI("a?b"), SI("a☺b"), true, NULL},
    {SI("a[^a]b"), SI("a☺b"), true, NULL},
    {SI("a?\?\?b"), SI("a☺b"), false, NULL},
    {SI("a[^a][^a][^a]b"), SI("a☺b"), false, NULL},
    {SI("[a-ζ]*"), SI("α"), true, NULL},
    {SI("*[a-ζ]"), SI("A"), false, NULL},
    {SI("a?b"), SI("a/b"), false, NULL},
    {SI("a*b"), SI("a/b"), false, NULL},
    {SI("[\\]a]"), SI("]"), true, NULL},
    {SI("[\\-]"), SI("-"), true, NULL},
    {SI("[x\\-]"), SI("x"), true, NULL},
    {SI("[x\\-]"), SI("-"), true, NULL},
    {SI("[x\\-]"), SI("z"), false, NULL},
    {SI("[\\-x]"), SI("x"), true, NULL},
    {SI("[\\-x]"), SI("-"), true, NULL},
    {SI("[\\-x]"), SI("a"), false, NULL},
    {SI("[]a]"), SI("]"), false, &path_err_bad_pattern},
    {SI("[-]"), SI("-"), false, &path_err_bad_pattern},
    {SI("[x-]"), SI("x"), false, &path_err_bad_pattern},
    {SI("[x-]"), SI("-"), false, &path_err_bad_pattern},
    {SI("[x-]"), SI("z"), false, &path_err_bad_pattern},
    {SI("[-x]"), SI("x"), false, &path_err_bad_pattern},
    {SI("[-x]"), SI("-"), false, &path_err_bad_pattern},
    {SI("[-x]"), SI("a"), false, &path_err_bad_pattern},
    {SI("\\"), SI("a"), false, &path_err_bad_pattern},
    {SI("[a-b-c]"), SI("a"), false, &path_err_bad_pattern},
    {SI("["), SI("a"), false, &path_err_bad_pattern},
    {SI("[^"), SI("a"), false, &path_err_bad_pattern},
    {SI("[^bc"), SI("a"), false, &path_err_bad_pattern},
    {SI("a["), SI("a"), false, &path_err_bad_pattern},
    {SI("a["), SI("ab"), false, &path_err_bad_pattern},
    {SI("a["), SI("x"), false, &path_err_bad_pattern},
    {SI("a/b["), SI("x"), false, &path_err_bad_pattern},
    {SI("*x"), SI("xxx"), true, NULL},
};

static Slice strs(const Str *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_STRING);
}

static void TestClean(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(cleantests); i++) {
        const PathTest *test = &cleantests[i];
        Str s = path_clean(a, test->path);
        if (!str_eq(s, test->result))
            testing_t_errorf_v(t, "Clean(%q) = %q, want %q", test->path, s,
                               test->result);
        s = path_clean(a, test->result);
        if (!str_eq(s, test->result))
            testing_t_errorf_v(t, "Clean(%q) = %q, want %q", test->result, s,
                               test->result);
    }
    arena_free(&ar);
}

/* Go counts allocations. An allocator with no room at all does the same job:
 * cleaning a path that is already clean must not ask it for anything, so the
 * answer has to come back right and not as the empty string. */
static void TestCleanMallocs(TestingT *t) {
    Byte buf[1];
    Fixed fx;
    fixed_init(&fx, buf, 0);
    Alloc *a = fixed_allocator(&fx);
    for (Int i = 0; i < LEN(cleantests); i++) {
        const PathTest *test = &cleantests[i];
        Str s = path_clean(a, test->result);
        if (!str_eq(s, test->result))
            testing_t_errorf_v(t, "Clean(%q) = %q with no memory, want %q",
                               test->result, s, test->result);
    }
}

static void TestSplit(TestingT *t) {
    for (Int i = 0; i < LEN(splittests); i++) {
        const SplitTest *test = &splittests[i];
        Str f;
        Str d = path_split(test->path, &f);
        if (!str_eq(d, test->dir) || !str_eq(f, test->file))
            testing_t_errorf_v(t, "Split(%q) = %q, %q, want %q, %q", test->path, d, f,
                               test->dir, test->file);
    }
}

static void TestJoin(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(jointests); i++) {
        const JoinTest *test = &jointests[i];
        Slice elem = strs(test->elem, test->elem_len);
        Str p = path_join(a, elem);
        if (!str_eq(p, test->path))
            testing_t_errorf_v(t, "Join(%q) = %q, want %q", elem, p, test->path);
    }
    arena_free(&ar);
}

static void TestExt(TestingT *t) {
    for (Int i = 0; i < LEN(exttests); i++) {
        const ExtTest *test = &exttests[i];
        Str x = path_ext(test->path);
        if (!str_eq(x, test->ext))
            testing_t_errorf_v(t, "Ext(%q) = %q, want %q", test->path, x, test->ext);
    }
}

static void TestBase(TestingT *t) {
    for (Int i = 0; i < LEN(basetests); i++) {
        const PathTest *test = &basetests[i];
        Str s = path_base(test->path);
        if (!str_eq(s, test->result))
            testing_t_errorf_v(t, "Base(%q) = %q, want %q", test->path, s,
                               test->result);
    }
}

static void TestDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(dirtests); i++) {
        const PathTest *test = &dirtests[i];
        Str s = path_dir(a, test->path);
        if (!str_eq(s, test->result))
            testing_t_errorf_v(t, "Dir(%q) = %q, want %q", test->path, s, test->result);
    }
    arena_free(&ar);
}

static void TestIsAbs(TestingT *t) {
    for (Int i = 0; i < LEN(isAbsTests); i++) {
        const IsAbsTest *test = &isAbsTests[i];
        bool r = path_is_abs(test->path);
        if (r != test->isAbs)
            testing_t_errorf_v(t, "IsAbs(%q) = %v, want %v", test->path, r,
                               test->isAbs);
    }
}

static void TestMatch(TestingT *t) {
    for (Int i = 0; i < LEN(matchTests); i++) {
        const MatchTest *tt = &matchTests[i];
        Error err;
        bool ok = path_match(tt->pattern, tt->s, &err);
        bool want_err = tt->err != NULL;
        if (ok != tt->match || BURROW_FAILED(err) != want_err ||
            (want_err && !errors_is(err, *tt->err)))
            testing_t_errorf_v(t, "Match(%q, %q) = %v, %v want %v, %v", tt->pattern,
                               tt->s, ok, err, tt->match,
                               want_err ? *tt->err : BURROW_NO_ERROR);
    }
}

/* ------------------------------------------------------------ not from Go */

static void TestJoinVariadic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str p = path_join_v(a, 3, S("a"), S("b/../c"), S("/d/"));
    CHECK(str_eq(p, S("a/c/d")));
    p = path_join_v(a, 0);
    CHECK(str_eq(p, S("")));
    /* More elements than the stack array in path_join_v holds. */
    p = path_join_v(a, 20, S("0"), S("1"), S("2"), S("3"), S("4"), S("5"), S("6"),
                    S("7"), S("8"), S("9"), S("a"), S("b"), S("c"), S("d"), S("e"),
                    S("f"), S("g"), S("h"), S(".."), S("i"));
    CHECK(str_eq(p, S("0/1/2/3/4/5/6/7/8/9/a/b/c/d/e/f/g/i")));
    arena_free(&ar);
}

static void TestOutOfMemory(TestingT *t) {
    Byte buf[1];
    Fixed fx;
    fixed_init(&fx, buf, 0);
    Alloc *a = fixed_allocator(&fx);
    CHECK(str_eq(path_clean(a, S("a//b")), S("")));
    CHECK(str_eq(path_join_v(a, 2, S("a"), S("b")), S("")));
    CHECK(str_eq(path_dir(a, S("a/./b/c")), S("")));
    /* These have nothing to copy, so they still work. */
    CHECK(str_eq(path_dir(a, S("a/b/c")), S("a/b")));
    CHECK(str_eq(path_clean(a, S("../a/b/..")), S("../a")));
}

/* The answer is a view of the input wherever Go's would share its memory. */
static void TestResultsBorrowTheInput(TestingT *t) {
    Str in = S("/usr/lib/libc.so");
    Str file;
    Str dir = path_split(in, &file);
    CHECK(dir.p == in.p);
    CHECK(file.p == in.p + 9);
    CHECK(path_ext(in).p == in.p + 13);
    CHECK(path_base(in).p == in.p + 9);
}

/* Go only checks the part of a pattern after a failed match, so a bad class
 * that is never reached is no error when the match succeeds early. */
static void TestMatchStopsAtTheFirstAnswer(TestingT *t) {
    Error err;
    CHECK(!path_match(S("x*[]"), S("abc"), &err));
    CHECK(errors_is(err, path_err_bad_pattern));
    CHECK(!path_match(S("a["), S("b"), &err));
    CHECK(errors_is(err, path_err_bad_pattern));
    CHECK(path_match(S("a*"), S("abc"), NULL));
    CHECK(!path_match(S("[a-"), S("a"), NULL));
    CHECK(str_eq(error_text(path_err_bad_pattern), S("syntax error in pattern")));
}

#define TESTS(X)                                                                       \
    X(TestClean)                                                                       \
    X(TestCleanMallocs)                                                                \
    X(TestSplit)                                                                       \
    X(TestJoin)                                                                        \
    X(TestExt)                                                                         \
    X(TestBase)                                                                        \
    X(TestDir)                                                                         \
    X(TestIsAbs)                                                                       \
    X(TestMatch)                                                                       \
    X(TestJoinVariadic)                                                                \
    X(TestOutOfMemory)                                                                 \
    X(TestResultsBorrowTheInput)                                                       \
    X(TestMatchStopsAtTheFirstAnswer)

TESTING_MAIN(TESTS)
