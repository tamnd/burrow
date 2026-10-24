/* Derived from Go's src/index/suffixarray/suffixarray_test.go and
 * example_test.go. Go source: go1.27.1.
 *
 * Not in Go: Go's tests reach into the package for maxData32, text_32,
 * text_64 and errCorrupted. These go through the burrow__suffixarray_ hooks
 * declared below. BenchmarkNew skips the opticks and go texts, because the
 * tree has neither Go's testdata nor its .go files to read them from.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include <stdint.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/index/suffixarray.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/slices.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

Int burrow__suffixarray_set_max_data32(Int n);
bool burrow__suffixarray_text_32(Alloc *a, Slice text, Slice sa);
bool burrow__suffixarray_text_64(Alloc *a, Slice text, Slice sa);
Error burrow__suffixarray_err_corrupted(void);

/* realMaxData32 */
#define SXT_REAL_MAX_DATA32 INT32_MAX

typedef struct SxtTestCase {
    Str name;            /* name of test case */
    Str source;          /* source to index */
    const Str *patterns; /* patterns to lookup */
    Int npatterns;
} SxtTestCase;

static const Str sxt_pat_empty[] = {
    BURROW_S_INIT(""),   BURROW_S_INIT("foo"), BURROW_S_INIT("(foo)"),
    BURROW_S_INIT(".*"), BURROW_S_INIT("a*"),
};

static const Str sxt_pat_all_a[] = {
    BURROW_S_INIT(""),           BURROW_S_INIT("a"),
    BURROW_S_INIT("aa"),         BURROW_S_INIT("aaa"),
    BURROW_S_INIT("aaaa"),       BURROW_S_INIT("aaaaa"),
    BURROW_S_INIT("aaaaaa"),     BURROW_S_INIT("aaaaaaa"),
    BURROW_S_INIT("aaaaaaaa"),   BURROW_S_INIT("aaaaaaaaa"),
    BURROW_S_INIT("aaaaaaaaaa"), BURROW_S_INIT("aaaaaaaaaaa"), /* 11 a's */
    BURROW_S_INIT("."),          BURROW_S_INIT(".*"),
    BURROW_S_INIT("a+"),         BURROW_S_INIT("aa+"),
    BURROW_S_INIT("aaaa[b]?"),   BURROW_S_INIT("aaa*"),
};

static const Str sxt_pat_abc[] = {
    BURROW_S_INIT("a"),   BURROW_S_INIT("b"),      BURROW_S_INIT("c"),
    BURROW_S_INIT("ab"),  BURROW_S_INIT("bc"),     BURROW_S_INIT("abc"),
    BURROW_S_INIT("a.c"), BURROW_S_INIT("a(b|c)"), BURROW_S_INIT("abc?"),
};

static const Str sxt_pat_barbara[] = {
    BURROW_S_INIT("a"),    BURROW_S_INIT("bar"),    BURROW_S_INIT("rab"),
    BURROW_S_INIT("arab"), BURROW_S_INIT("barbar"), BURROW_S_INIT("bara?bar"),
};

static const Str sxt_pat_drill[] = {
    BURROW_S_INIT("Now"),
    BURROW_S_INIT("the time"),
    BURROW_S_INIT("to come the aid"),
    BURROW_S_INIT("is the time for all good men to come to the aid of their"),
    BURROW_S_INIT("to (come|the)?"),
};

#define SXT_N(arr) ((Int)(sizeof(arr) / sizeof((arr)[0])))

static const SxtTestCase sxt_test_cases[] = {
    {BURROW_S_INIT("empty string"), BURROW_S_INIT(""), sxt_pat_empty,
     SXT_N(sxt_pat_empty)},

    {BURROW_S_INIT("all a's"), BURROW_S_INIT("aaaaaaaaaa"), /* 10 a's */
     sxt_pat_all_a, SXT_N(sxt_pat_all_a)},

    {BURROW_S_INIT("abc"), BURROW_S_INIT("abc"), sxt_pat_abc, SXT_N(sxt_pat_abc)},

    {BURROW_S_INIT("barbara*3"), BURROW_S_INIT("barbarabarbarabarbara"),
     sxt_pat_barbara, SXT_N(sxt_pat_barbara)},

    {BURROW_S_INIT("typing drill"),
     BURROW_S_INIT(
         "Now is the time for all good men to come to the aid of their country."),
     sxt_pat_drill, SXT_N(sxt_pat_drill)},

    {BURROW_S_INIT("godoc simulation"),
     BURROW_S_INIT("package main\n\nimport(\n    \"rand\"\n    "), NULL, 0},
};

static Slice sxt_bytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str sxt_str_from(Str s, Int i) {
    Str r = {s.p + i, s.len - i};
    return r;
}

/* Gives back what slice_make made from the heap. */
static void sxt_free(Slice s, size_t size, size_t align) {
    if (s.p != NULL && s.cap > 0)
        mem_free(heap_allocator(), s.p, (size_t)s.cap * size, align);
}

/* find all occurrences of s in source; report at most n occurrences */
static Slice sxt_find(Alloc *a, Str src, Str s, Int n) {
    Slice res = slice_nil(TYPE_INT);
    if (s.len > 0 && n != 0) {
        /* find at most n occurrences of s in src */
        for (Int i = -1; n < 0 || res.len < n;) {
            Int j = strings_index(sxt_str_from(src, i + 1), s);
            if (j < 0)
                break;
            i += j + 1;
            res = slice_append(a, res, &i, 1);
        }
    }
    return res;
}

static void sxt_test_lookup(TestingT *t, const SxtTestCase *tc, SuffixarrayIndex *x,
                            Alloc *a, Str s, Int n) {
    Slice res = suffixarray_index_lookup(x, a, sxt_bytes(s), n);
    Slice exp = sxt_find(a, tc->source, s, n);

    /* check that the lengths match */
    if (res.len != exp.len)
        testing_t_errorf_v(t,
                           "test %q, lookup %q (n = %d): expected %d results; got %d",
                           tc->name, s, n, exp.len, res.len);

    /* if n >= 0 the number of results is limited --- unless n >= all results,
     * we may obtain different positions from the Index and from find (because
     * Index may not find the results in the same order as find) => in general
     * we cannot simply check that the res and exp lists are equal */

    /* check that each result is in fact a correct match and there are no
     * duplicates */
    slices_sort(res);
    for (Int i = 0; i < res.len; i++) {
        Int r = BURROW_AT(Int, res, i);
        if (r < 0 || tc->source.len <= r) {
            testing_t_errorf_v(
                t,
                "test %q, lookup %q, result %d (n = %d): index %d out of range [0, %d[",
                tc->name, s, i, n, r, tc->source.len);
        } else if (!strings_has_prefix(sxt_str_from(tc->source, r), s)) {
            testing_t_errorf_v(
                t, "test %q, lookup %q, result %d (n = %d): index %d not a match",
                tc->name, s, i, n, r);
        }
        if (i > 0 && BURROW_AT(Int, res, i - 1) == r)
            testing_t_errorf_v(
                t, "test %q, lookup %q, result %d (n = %d): found duplicate index %d",
                tc->name, s, i, n, r);
    }

    if (n < 0) {
        /* all results computed - sorted res and exp must be equal */
        for (Int i = 0; i < res.len; i++) {
            Int r = BURROW_AT(Int, res, i);
            Int e = BURROW_AT(Int, exp, i);
            if (r != e)
                testing_t_errorf_v(
                    t, "test %q, lookup %q, result %d: expected index %d; got %d",
                    tc->name, s, i, e, r);
        }
    }
}

static void sxt_test_find_all_index(TestingT *t, const SxtTestCase *tc,
                                    SuffixarrayIndex *x, Alloc *a, const Regexp *rx,
                                    Int n) {
    Slice res = suffixarray_index_find_all_index(x, a, rx, n);
    Slice exp = regexp_find_all_string_index(rx, a, tc->source, n);
    Str rs = regexp_string(rx);

    /* check that the lengths match */
    if (res.len != exp.len)
        testing_t_errorf_v(
            t, "test %q, FindAllIndex %q (n = %d): expected %d results; got %d",
            tc->name, rs, n, exp.len, res.len);

    /* if n >= 0 the number of results is limited --- unless n >= all results,
     * we may obtain different positions from the Index and from regexp
     * (because Index may not find the results in the same order as regexp) =>
     * in general we cannot simply check that the res and exp lists are equal */

    /* check that each result is in fact a correct match and the result is
     * sorted */
    for (Int i = 0; i < res.len; i++) {
        Slice r = BURROW_AT(Slice, res, i);
        Int r0 = BURROW_AT(Int, r, 0);
        Int r1 = BURROW_AT(Int, r, 1);
        if (r0 < 0 || r0 > r1 || tc->source.len < r1) {
            testing_t_errorf_v(
                t,
                "test %q, FindAllIndex %q, result %d (n == %d): illegal match [%d, %d]",
                tc->name, rs, i, n, r0, r1);
        } else {
            Str m = {tc->source.p + r0, r1 - r0};
            if (!regexp_match_string(rx, m))
                testing_t_errorf_v(t,
                                   "test %q, FindAllIndex %q, result %d (n = %d): [%d, "
                                   "%d] not a match",
                                   tc->name, rs, i, n, r0, r1);
        }
    }

    if (n < 0) {
        /* all results computed - sorted res and exp must be equal */
        for (Int i = 0; i < res.len; i++) {
            Slice r = BURROW_AT(Slice, res, i);
            Slice e = BURROW_AT(Slice, exp, i);
            Int r0 = BURROW_AT(Int, r, 0);
            Int r1 = BURROW_AT(Int, r, 1);
            Int e0 = BURROW_AT(Int, e, 0);
            Int e1 = BURROW_AT(Int, e, 1);
            if (r0 != e0 || r1 != e1)
                testing_t_errorf_v(
                    t,
                    "test %q, FindAllIndex %q, result %d: expected match "
                    "[%d, %d]; got [%d, %d]",
                    tc->name, rs, i, e0, e1, r0, r1);
        }
    }
}

static void sxt_test_lookups(TestingT *t, const SxtTestCase *tc, SuffixarrayIndex *x,
                             Alloc *a, Int n) {
    for (Int k = 0; k < tc->npatterns; k++) {
        Str pat = tc->patterns[k];
        sxt_test_lookup(t, tc, x, a, pat, n);
        Error err = BURROW_NO_ERROR;
        Regexp *rx = regexp_compile(a, pat, &err);
        if (BURROW_OK(err) && rx != NULL) {
            sxt_test_find_all_index(t, tc, x, a, rx, n);
            regexp_free(rx);
        }
    }
}

static Int sxt_sa_len(const SuffixarrayIndex *x) {
    return x->sa.int32.len + x->sa.int64.len;
}

static Int sxt_sa_get(const SuffixarrayIndex *x, Int i) {
    if (x->sa.int32.p != NULL)
        return (Int)BURROW_AT(int32_t, x->sa.int32, i);
    return (Int)BURROW_AT(int64_t, x->sa.int64, i);
}

/* index is used to hide the sort.Interface */

static Slice sxt_at(const SuffixarrayIndex *x, Int i) {
    return slice_sub(x->data, sxt_sa_get(x, i), x->data.len);
}

static Int sxt_index_len(void *self) {
    return sxt_sa_len((const SuffixarrayIndex *)self);
}

static bool sxt_index_less(void *self, Int i, Int j) {
    const SuffixarrayIndex *x = (const SuffixarrayIndex *)self;
    return bytes_compare(sxt_at(x, i), sxt_at(x, j)) < 0;
}

static void sxt_index_swap(void *self, Int i, Int j) {
    SuffixarrayIndex *x = (SuffixarrayIndex *)self;
    if (x->sa.int32.p != NULL) {
        int32_t v = BURROW_AT(int32_t, x->sa.int32, i);
        BURROW_AT(int32_t, x->sa.int32, i) = BURROW_AT(int32_t, x->sa.int32, j);
        BURROW_AT(int32_t, x->sa.int32, j) = v;
    } else {
        int64_t v = BURROW_AT(int64_t, x->sa.int64, i);
        BURROW_AT(int64_t, x->sa.int64, i) = BURROW_AT(int64_t, x->sa.int64, j);
        BURROW_AT(int64_t, x->sa.int64, j) = v;
    }
}

static const SortInterfaceVT sxt_index_vt = {NULL, sxt_index_len, sxt_index_less,
                                             sxt_index_swap};

static void sxt_test_construction(TestingT *t, const SxtTestCase *tc,
                                  SuffixarrayIndex *x) {
    SortInterface data = {&sxt_index_vt, x};
    if (!sort_is_sorted(data))
        testing_t_errorf_v(t, "failed testConstruction %s", tc->name);
}

static bool sxt_equal(const SuffixarrayIndex *x, const SuffixarrayIndex *y) {
    if (!bytes_equal(x->data, y->data))
        return false;
    if (sxt_sa_len(x) != sxt_sa_len(y))
        return false;
    Int n = sxt_sa_len(x);
    for (Int i = 0; i < n; i++) {
        if (sxt_sa_get(x, i) != sxt_sa_get(y, i))
            return false;
    }
    return true;
}

static void sxt_read_index(TestingT *t, const SxtTestCase *tc, SuffixarrayIndex *y,
                           Slice b) {
    BytesReader r;
    memset(&r, 0, sizeof r);
    bytes_reader_reset(&r, b);
    Error err =
        suffixarray_index_read(y, heap_allocator(), bytes_reader_as_io_reader(&r));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed reading index %s (%s)", tc->name, err);
}

/* returns the serialized index size */
static Int sxt_test_save_restore(TestingT *t, const SxtTestCase *tc,
                                 SuffixarrayIndex *x) {
    BytesBuffer *buf = bytes_new_buffer(heap_allocator(), slice_nil(TYPE_BYTE));
    if (buf == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    Error err = suffixarray_index_write(x, bytes_buffer_as_io_writer(buf));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed writing index %s (%s)", tc->name, err);
    Int size = bytes_buffer_len(buf);
    SuffixarrayIndex y;
    memset(&y, 0, sizeof y);
    sxt_read_index(t, tc, &y, bytes_buffer_bytes(buf));
    if (!sxt_equal(x, &y))
        testing_t_errorf_v(t, "restored index doesn't match saved index %s", tc->name);

    Int old = burrow__suffixarray_set_max_data32(SXT_REAL_MAX_DATA32);
    /* Reread as forced 32. */
    suffixarray_index_free(&y);
    sxt_read_index(t, tc, &y, bytes_buffer_bytes(buf));
    if (!sxt_equal(x, &y))
        testing_t_errorf_v(t, "restored index doesn't match saved index %s", tc->name);

    /* Reread as forced 64. */
    suffixarray_index_free(&y);
    burrow__suffixarray_set_max_data32(-1);
    sxt_read_index(t, tc, &y, bytes_buffer_bytes(buf));
    if (!sxt_equal(x, &y))
        testing_t_errorf_v(t, "restored index doesn't match saved index %s", tc->name);

    burrow__suffixarray_set_max_data32(old);
    suffixarray_index_free(&y);
    bytes_buffer_free(buf);
    return size;
}

static void sxt_test_index(TestingT *t) {
    for (Int k = 0; k < SXT_N(sxt_test_cases); k++) {
        const SxtTestCase *tc = &sxt_test_cases[k];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        SuffixarrayIndex *x = suffixarray_new(a, sxt_bytes(tc->source));
        if (x == NULL)
            testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
        sxt_test_construction(t, tc, x);
        sxt_test_save_restore(t, tc, x);
        sxt_test_lookups(t, tc, x, a, 0);
        sxt_test_lookups(t, tc, x, a, 1);
        sxt_test_lookups(t, tc, x, a, 10);
        sxt_test_lookups(t, tc, x, a, 2000000000);
        sxt_test_lookups(t, tc, x, a, -1);
        suffixarray_index_free(x);
        arena_free(&ar);
    }
}

static void TestIndex32(TestingT *t) {
    sxt_test_index(t);
}

static void TestIndex64(TestingT *t) {
    burrow__suffixarray_set_max_data32(-1);
    sxt_test_index(t);
    burrow__suffixarray_set_max_data32(SXT_REAL_MAX_DATA32);
}

/* A build function gives the suffix array of x as a Slice of Int from the
 * heap. */
typedef Slice (*SxtBuild)(Slice x);

static Slice sxt_build_32(Slice x) {
    Alloc *h = heap_allocator();
    Slice sa = slice_make(h, TYPE_INT32, x.len, x.len);
    Slice out = slice_nil(TYPE_INT);
    if (sa.p == NULL || !burrow__suffixarray_text_32(h, x, sa)) {
        sxt_free(sa, sizeof(int32_t), _Alignof(int32_t));
        return out;
    }
    out = slice_make(h, TYPE_INT, sa.len, sa.len);
    for (Int i = 0; i < out.len; i++)
        BURROW_AT(Int, out, i) = (Int)BURROW_AT(int32_t, sa, i);
    sxt_free(sa, sizeof(int32_t), _Alignof(int32_t));
    return out;
}

static Slice sxt_build_64(Slice x) {
    Alloc *h = heap_allocator();
    Slice sa = slice_make(h, TYPE_INT64, x.len, x.len);
    Slice out = slice_nil(TYPE_INT);
    if (sa.p == NULL || !burrow__suffixarray_text_64(h, x, sa)) {
        sxt_free(sa, sizeof(int64_t), _Alignof(int64_t));
        return out;
    }
    out = slice_make(h, TYPE_INT, sa.len, sa.len);
    for (Int i = 0; i < out.len; i++)
        BURROW_AT(Int, out, i) = (Int)BURROW_AT(int64_t, sa, i);
    sxt_free(sa, sizeof(int64_t), _Alignof(int64_t));
    return out;
}

static void sxt_log_build(TestingT *t, Slice x) {
    testing_t_logf_v(t, "build %v", x);
}

/* sa := build(x), with the recover testSA defers. */
static Slice sxt_run_build(TestingT *t, Slice x, SxtBuild build) {
    Slice volatile sa = {0};
    BURROW_TRY {
        sa = build(x);
    }
    BURROW_CATCH(e) {
        sxt_log_build(t, x);
        panic(e);
    }
    BURROW_TRY_END;
    return sa;
}

/* testSA tests the suffix array build function on the input x.
 * It constructs the suffix array and then checks that it is correct. */
static bool sxt_test_sa(TestingT *t, Slice x, SxtBuild build) {
    Slice sa = sxt_run_build(t, x, build);
    bool ok = true;
    if (sa.len != x.len) {
        testing_t_errorf_v(t, "build %v: len(sa) = %d, want %d", x, sa.len, x.len);
        ok = false;
    }
    for (Int i = 0; ok && i + 1 < sa.len; i++) {
        Int s0 = BURROW_AT(Int, sa, i);
        Int s1 = BURROW_AT(Int, sa, i + 1);
        if (s0 < 0 || s0 >= x.len || s1 < 0 || s1 >= x.len) {
            testing_t_errorf_v(t, "build %s: sa out of range: %v\n", x, sa);
            ok = false;
        } else if (bytes_compare(slice_sub(x, s0, x.len), slice_sub(x, s1, x.len)) >=
                   0) {
            testing_t_errorf_v(t, "build %v -> %v\nsa[%d:] = %d,%d out of order", x, sa,
                               i, s0, s1);
            ok = false;
        }
    }
    sxt_free(sa, sizeof(Int), _Alignof(Int));
    return ok;
}

/* testRec fills x[i:] with all possible combinations of values in [1,max]
 * and then calls testSA(t, x, build) for each one. */
static void sxt_test_rec(TestingT *t, Slice x, Int i, int max, int *num_fail,
                         SxtBuild build) {
    Byte *xp = (Byte *)x.p;
    if (i < x.len) {
        for (xp[i] = 1; xp[i] <= (Byte)max; xp[i]++)
            sxt_test_rec(t, x, i + 1, max, num_fail, build);
        return;
    }

    if (!sxt_test_sa(t, x, build)) {
        (*num_fail)++;
        if (*num_fail >= 10) {
            testing_t_errorf_v(t, "stopping after %d failures", *num_fail);
            testing_t_fail_now(t);
        }
    }
}

typedef struct SxtBuildEnv {
    SxtBuild build;
} SxtBuildEnv;

static void sxt_ababab(void *env, TestingT *t) {
    SxtBuild build = ((const SxtBuildEnv *)env)->build;
    /* Very repetitive input has numLMS = len(x)/2-1
     * at top level, the largest it can be.
     * But maxID is only two (aba and ab$). */
    Int size = 100000;
    if (testing_short())
        size = 10000;
    Slice x = slice_make(heap_allocator(), TYPE_BYTE, size, size);
    if (x.p == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    for (Int i = 0; i < x.len; i++)
        BURROW_AT(Byte, x, i) = (Byte) "ab"[i % 2];
    sxt_test_sa(t, x, build);
    sxt_free(x, 1, 1);
}

static void sxt_forcealloc(void *env, TestingT *t) {
    SxtBuild build = ((const SxtBuildEnv *)env)->build;
    /* Construct a pathological input that forces
     * recurse_32 to allocate a new temporary buffer.
     * The input must have more than N/3 LMS-substrings,
     * which we arrange by repeating an SLSLSLSLSLSL pattern
     * like ababab... above, but then we must also arrange
     * for a large number of distinct LMS-substrings.
     * We use this pattern:
     * 1 255 1 254 1 253 1 ... 1 2 1 255 2 254 2 253 2 252 2 ...
     * This gives approximately 2¹⁵ distinct LMS-substrings.
     * We need to repeat at least one substring, though,
     * or else the recursion can be bypassed entirely. */
    Slice x = slice_make(heap_allocator(), TYPE_BYTE, 100000, 100001);
    if (x.p == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    Byte lo = 1;
    Byte hi = 255;
    for (Int i = 0; i < x.len; i++) {
        if (i % 2 == 0) {
            BURROW_AT(Byte, x, i) = lo;
        } else {
            BURROW_AT(Byte, x, i) = hi;
            hi--;
            if (hi <= lo) {
                lo++;
                if (lo == 0)
                    lo = 1;
                hi = 255;
            }
        }
    }
    BURROW_AT(Byte, slice_sub(x, 0, x.cap), x.len) = 0; /* for sais.New */
    sxt_test_sa(t, x, build);
    sxt_free(x, 1, 1);
}

static void sxt_exhaustive2(void *env, TestingT *t) {
    SxtBuild build = ((const SxtBuildEnv *)env)->build;
    /* All inputs over {0,1} up to length 21.
     * Runs in about 10 seconds on my laptop. */
    Byte buf[30] = {0};
    Slice x = slice_from(buf, 30, 30, TYPE_BYTE);
    int num_fail = 0;
    for (Int n = 0; n <= 21; n++) {
        if (n > 12 && testing_short())
            break;
        BURROW_AT(Byte, x, n) = 0; /* for sais.New */
        sxt_test_rec(t, slice_sub(x, 0, n), 0, 2, &num_fail, build);
    }
}

static void sxt_exhaustive3(void *env, TestingT *t) {
    SxtBuild build = ((const SxtBuildEnv *)env)->build;
    /* All inputs over {0,1,2} up to length 14.
     * Runs in about 10 seconds on my laptop. */
    Byte buf[30] = {0};
    Slice x = slice_from(buf, 30, 30, TYPE_BYTE);
    int num_fail = 0;
    for (Int n = 0; n <= 14; n++) {
        if (n > 8 && testing_short())
            break;
        BURROW_AT(Byte, x, n) = 0; /* for sais.New */
        sxt_test_rec(t, slice_sub(x, 0, n), 0, 3, &num_fail, build);
    }
}

/* test tests an arbitrary suffix array construction function.
 * Generates many inputs, builds and checks suffix arrays. */
static void sxt_test(TestingT *t, SxtBuild build) {
    SxtBuildEnv env = {build};
    testing_t_run(t, BURROW_S("ababab..."), BURROW_FN(TestingTFunc, sxt_ababab, &env));
    testing_t_run(t, BURROW_S("forcealloc"),
                  BURROW_FN(TestingTFunc, sxt_forcealloc, &env));
    testing_t_run(t, BURROW_S("exhaustive2"),
                  BURROW_FN(TestingTFunc, sxt_exhaustive2, &env));
    testing_t_run(t, BURROW_S("exhaustive3"),
                  BURROW_FN(TestingTFunc, sxt_exhaustive3, &env));
}

static void TestNew32(TestingT *t) {
    sxt_test(t, sxt_build_32);
}

static void TestNew64(TestingT *t) {
    sxt_test(t, sxt_build_64);
}

/* makeText for the zero and rand texts, from the heap. */
static Slice sxt_make_text(Str name) {
    Alloc *h = heap_allocator();
    Slice data = slice_nil(TYPE_BYTE);
    if (str_eq(name, BURROW_S("zero"))) {
        data = slice_make(h, TYPE_BYTE, 50000000, 50000000);
    } else if (str_eq(name, BURROW_S("rand"))) {
        data = slice_make(h, TYPE_BYTE, 50000000, 50000000);
        for (Int i = 0; i < data.len; i++)
            BURROW_AT(Byte, data, i) = (Byte)math_rand_intn(256);
    }
    return data;
}

typedef struct SxtBenchEnv {
    Slice data;
    Int bits;
} SxtBenchEnv;

static void sxt_set_bits(Int bits) {
    if (bits == 32)
        burrow__suffixarray_set_max_data32(SXT_REAL_MAX_DATA32);
    else
        burrow__suffixarray_set_max_data32(-1); /* force use of 64-bit code */
}

static void sxt_bench_new_bits(void *env, TestingB *b) {
    const SxtBenchEnv *e = (const SxtBenchEnv *)env;
    sxt_set_bits(e->bits);

    testing_b_set_bytes(b, (int64_t)e->data.len);
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        suffixarray_index_free(suffixarray_new(heap_allocator(), e->data));
    burrow__suffixarray_set_max_data32(SXT_REAL_MAX_DATA32);
}

static void sxt_bench_new_size(void *env, TestingB *b) {
    const SxtBenchEnv *e = (const SxtBenchEnv *)env;
    static const Int bits[] = {32, 64};
    for (int k = 0; k < 2; k++) {
        if (sizeof(Uint) == 4 && bits[k] == 64)
            continue;
        SxtBenchEnv sub = {e->data, bits[k]};
        Str name = bits[k] == 32 ? BURROW_S("bits=32") : BURROW_S("bits=64");
        testing_b_run(b, name, BURROW_FN(TestingBFunc, sxt_bench_new_bits, &sub));
    }
}

static void sxt_bench_new_text(void *env, TestingB *b) {
    Str text = *(const Str *)env;
    if (!str_eq(text, BURROW_S("zero")) && !str_eq(text, BURROW_S("rand")))
        testing_b_skipf_v(b, "no %s text", text); /* Not in Go */
    Slice data = sxt_make_text(text);
    if (data.p == NULL)
        testing_b_fatalf_v(b, "out of memory"); /* Not in Go */
    Slice all = data;
    if (testing_short() && data.len > 5000000)
        data = slice_sub(data, 0, 5000000);
    static const Int sizes[] = {100000, 500000, 1000000, 5000000, 10000000, 50000000};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int k = 0; k < 6; k++) {
        Int size = sizes[k];
        if (data.len < size)
            continue;
        SxtBenchEnv sub = {slice_sub(data, 0, size), 0};
        Str name = fmt_sprintf_v(a, "%dK", size / 1000);
        if (size >= 1000000)
            name = fmt_sprintf_v(a, "%dM", size / 1000000);
        Str full = fmt_sprintf_v(a, "size=%s", name);
        testing_b_run(b, full, BURROW_FN(TestingBFunc, sxt_bench_new_size, &sub));
    }
    arena_free(&ar);
    sxt_free(all, 1, 1);
}

static void BenchmarkNew(TestingB *b) {
    static const Str texts[] = {BURROW_S_INIT("opticks"), BURROW_S_INIT("go"),
                                BURROW_S_INIT("zero"), BURROW_S_INIT("rand")};
    static const Str names[] = {BURROW_S_INIT("text=opticks"), BURROW_S_INIT("text=go"),
                                BURROW_S_INIT("text=zero"), BURROW_S_INIT("text=rand")};
    for (int k = 0; k < 4; k++) {
        Str text = texts[k];
        testing_b_run(b, names[k], BURROW_FN(TestingBFunc, sxt_bench_new_text, &text));
    }
}

static void sxt_bench_save_restore_bits(void *env, TestingB *b) {
    const SxtBenchEnv *e = (const SxtBenchEnv *)env;
    static const SxtTestCase tc = {BURROW_S_INIT("BenchmarkSaveRestore"),
                                   BURROW_S_INIT(""), NULL, 0};
    Alloc *h = heap_allocator();
    sxt_set_bits(e->bits);

    testing_b_stop_timer(b);
    SuffixarrayIndex *x = suffixarray_new(h, e->data);
    if (x == NULL)
        testing_b_fatalf_v(b, "out of memory"); /* Not in Go */
    Int size =
        sxt_test_save_restore(burrow__testing_b_t(b), &tc, x); /* verify correctness */
    Slice init = slice_make(h, TYPE_BYTE, size, size);
    BytesBuffer *buf = bytes_new_buffer(h, init); /* avoid growing */
    if (init.p == NULL || buf == NULL)
        testing_b_fatalf_v(b, "out of memory"); /* Not in Go */
    testing_b_set_bytes(b, (int64_t)size);
    testing_b_start_timer(b);
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(buf);
        Error err = suffixarray_index_write(x, bytes_buffer_as_io_writer(buf));
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%v", err);
        SuffixarrayIndex y;
        memset(&y, 0, sizeof y);
        err = suffixarray_index_read(&y, h, bytes_buffer_as_io_reader(buf));
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%v", err);
        suffixarray_index_free(&y);
    }
    bytes_buffer_free(buf);
    sxt_free(init, 1, 1);
    suffixarray_index_free(x);
    burrow__suffixarray_set_max_data32(SXT_REAL_MAX_DATA32);
}

static void BenchmarkSaveRestore(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* guarantee always same sequence */
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 0x5a77a1));
    Slice data = slice_make(a, TYPE_BYTE, 1 << 20, 1 << 20); /* 1MB of data to index */
    if (r == NULL || data.p == NULL)
        testing_b_fatalf_v(b, "out of memory"); /* Not in Go */
    for (Int i = 0; i < data.len; i++)
        BURROW_AT(Byte, data, i) = (Byte)math_rand_rand_intn(r, 256);
    static const Int bits[] = {32, 64};
    for (int k = 0; k < 2; k++) {
        if (sizeof(Uint) == 4 && bits[k] == 64)
            continue;
        SxtBenchEnv sub = {data, bits[k]};
        Str name = bits[k] == 32 ? BURROW_S("bits=32") : BURROW_S("bits=64");
        testing_b_run(b, name,
                      BURROW_FN(TestingBFunc, sxt_bench_save_restore_bits, &sub));
    }
    arena_free(&ar);
}

static void TestIssue53352(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte bytes[1] = {'x'};
    Slice data = slice_from(bytes, 1, 1, TYPE_BYTE);
    SuffixarrayIndex *index = suffixarray_new(a, data);
    BytesBuffer *buf = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
    if (index == NULL || buf == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    Error err = suffixarray_index_write(index, bytes_buffer_as_io_writer(buf));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* buffer encoding is as follows:
     *
     * [ data length n | data bytes | index slice buffer size |  indices  ] ...
     * (next slice, if any)
     * \__ 10 bytes __/\_ n bytes _/\_______ 10 bytes _______/\_ varuints /
     *
     * n and s are encoded as varints using 10 bytes always so they can be
     * patched. For small values x >= 0 the varint encoded value is 2*x. */

    /* For the above data we have n == len("x") == 1. */
    Int n = data.len;
    Slice encoding = bytes_buffer_bytes(buf);
    Int got = (Int)BURROW_AT(Byte, encoding, 0);
    if (got != 2 * n)
        testing_t_fatalf_v(t, "got n = %d; want %d", got, 2 * n);

    /* For the above data, the index slice buffer contains a single index entry
     * (0 for "x") plus the size of the index buffer (10 bytes), so s == 10 + 1
     * == 11; and s is encoded immediately following the data bytes. */
    Int s = 10 + 1;
    got = (Int)BURROW_AT(Byte, encoding, 10 + n);
    if (got != 2 * s)
        testing_t_fatalf_v(t, "got s = %d; want %d", got, 2 * s);

    /* Reading back the encoding should work without errors. */
    BytesBuffer *rb = bytes_new_buffer(a, encoding);
    if (rb == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    err = suffixarray_index_read(index, a, bytes_buffer_as_io_reader(rb));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* Adding an extra index corrupts the encoding (more indices than data bytes
     * to be indexed). */
    s++; /* increase slice buffer size */
    Byte zero = 0;
    encoding = slice_append(a, encoding, &zero, 1); /* add one more index */
    BURROW_AT(Byte, encoding, 10 + n) =
        (Byte)(2 * s); /* update index slice buffer size */

    /* Reading back the corrupted encoding should report an error.
     * Before fixing go.dev/issue/53352, this resulted in an index-out-of-range
     * panic. */
    Error corrupted = burrow__suffixarray_err_corrupted();
    rb = bytes_new_buffer(a, encoding);
    if (rb == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    err = suffixarray_index_read(index, a, bytes_buffer_as_io_reader(rb));
    if (!errors_is(err, corrupted))
        testing_t_fatalf_v(t, "got %v; want %v", err, corrupted);
    suffixarray_index_free(index);
    arena_free(&ar);
}

static void TestIndexCorruption(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte bytes[1] = {'x'};
    Slice data = slice_from(bytes, 1, 1, TYPE_BYTE);
    SuffixarrayIndex *index = suffixarray_new(a, data);
    BytesBuffer *buf = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
    if (index == NULL || buf == NULL)
        testing_t_fatalf_v(t, "out of memory"); /* Not in Go */
    Error err = suffixarray_index_write(index, bytes_buffer_as_io_writer(buf));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* The index data is at the end of the encoding (see TestIssue53352 for the
     * encoding format). The encoded indices i must be 0 <= i < len(data)
     * because they are used to index into the data. Corrupt one and make sure
     * we get an error when reading the encoding. */
    BURROW_AT(Byte, bytes_buffer_bytes(buf), bytes_buffer_len(buf) - 1) =
        (Byte)data.len;

    /* Reading back the corrupted encoding should report an error. */
    Error corrupted = burrow__suffixarray_err_corrupted();
    err = suffixarray_index_read(index, a, bytes_buffer_as_io_reader(buf));
    if (!errors_is(err, corrupted))
        testing_t_fatalf_v(t, "got %v; want %v", err, corrupted);
    suffixarray_index_free(index);
    arena_free(&ar);
}

static void ExampleIndex_Lookup(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SuffixarrayIndex *index = suffixarray_new(a, sxt_bytes(BURROW_S("banana")));
    if (index == NULL)
        return; /* Not in Go */
    Slice offsets = suffixarray_index_lookup(index, a, sxt_bytes(BURROW_S("ana")), -1);
    for (Int i = 0; i < offsets.len; i++) {
        Int off = BURROW_AT(Int, offsets, i);
        fmt_println_v(off);
    }
    suffixarray_index_free(index);
    arena_free(&ar);

    /* Unordered output:
     * 1
     * 3 */
}

#define TESTS(X)                                                                       \
    X(TestIndex32)                                                                     \
    X(TestIndex64)                                                                     \
    X(TestNew32)                                                                       \
    X(TestNew64)                                                                       \
    X(BenchmarkNew)                                                                    \
    X(BenchmarkSaveRestore)                                                            \
    X(TestIssue53352)                                                                  \
    X(TestIndexCorruption)                                                             \
    X(ExampleIndex_Lookup, TESTING_UNORDERED("1\n3"))

TESTING_MAIN(TESTS)
