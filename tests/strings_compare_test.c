/* Derived from Go's src/strings/compare_test.go and src/strings/clone_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include "check.h"

#include <string.h>

#define S BURROW_S
#define SI BURROW_S_INIT
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static const struct {
    Str a, b;
    Int i;
} compareTests[] = {
    {SI(""), SI(""), 0},
    {SI("a"), SI(""), 1},
    {SI(""), SI("a"), -1},
    {SI("abc"), SI("abc"), 0},
    {SI("ab"), SI("abc"), -1},
    {SI("abc"), SI("ab"), 1},
    {SI("x"), SI("ab"), 1},
    {SI("ab"), SI("x"), -1},
    {SI("x"), SI("a"), 1},
    {SI("b"), SI("x"), -1},
    /* test runtime·memeq's chunked implementation */
    {SI("abcdefgh"), SI("abcdefgh"), 0},
    {SI("abcdefghi"), SI("abcdefghi"), 0},
    {SI("abcdefghi"), SI("abcdefghj"), -1},
};

static void TestCompare(TestingT *t) {
    Byte buf[32];
    for (Int i = 0; i < LEN(compareTests); i++) {
        Int num_shifts = 16;
        for (Int offset = 0; offset <= num_shifts; offset++) {
            memset(buf, '*', (size_t)offset);
            memcpy(buf + offset, compareTests[i].b.p, (size_t)compareTests[i].b.len);
            Str shifted_b = str_from_bytes(buf + offset, compareTests[i].b.len);
            Int cmp = strings_compare(compareTests[i].a, shifted_b);
            if (cmp != compareTests[i].i)
                testing_t_errorf_v(t, "Compare(%q, %q), offset %d = %v; want %v",
                                   compareTests[i].a, compareTests[i].b, offset, cmp,
                                   compareTests[i].i);
        }
    }
}

static void TestCompareIdenticalString(TestingT *t) {
    Str s = S("Hello Gophers!");
    if (strings_compare(s, s) != 0)
        testing_t_error_v(t, "s != s");
    if (strings_compare(s, str_from_bytes(s.p, 1)) != 1)
        testing_t_error_v(t, "s > s[:1] failed");
}

static Str sub(const Byte *p, Int n) {
    return str_from_bytes(p, n);
}

static void TestCompareStrings(TestingT *t) {
    Int lengths[160];
    Int nl = 0;
    for (Int i = 0; i <= 128; i++)
        lengths[nl++] = i;
    static const Int more[] = {256, 512, 1024, 1333, 4095, 4096, 4097};
    for (Int i = 0; i < LEN(more); i++)
        lengths[nl++] = more[i];
    if (!testing_short()) {
        static const Int most[] = {65535, 65536, 65537, 99999};
        for (Int i = 0; i < LEN(most); i++)
            lengths[nl++] = most[i];
    }
    Int n = lengths[nl - 1];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Byte *a = (Byte *)mem_alloc(arena_allocator(&ar), (size_t)n + 1, 1);
    Byte *b = (Byte *)mem_alloc(arena_allocator(&ar), (size_t)n + 1, 1);
    Int last_len = 0;
    for (Int li = 0; li < nl; li++) {
        Int len = lengths[li];
        /* randomish but deterministic data. No 0 or 255. */
        for (Int i = 0; i < len; i++) {
            a[i] = (Byte)(1 + 31 * i % 254);
            b[i] = (Byte)(1 + 31 * i % 254);
        }
        /* data past the end is different */
        for (Int i = len; i <= n; i++) {
            a[i] = 8;
            b[i] = 9;
        }
        Int cmp = strings_compare(sub(a, len), sub(b, len));
        if (cmp != 0)
            testing_t_errorf_v(t, "CompareIdentical(%d) = %d", len, cmp);
        if (len > 0) {
            cmp = strings_compare(sub(a, len - 1), sub(b, len));
            if (cmp != -1)
                testing_t_errorf_v(t, "CompareAshorter(%d) = %d", len, cmp);
            cmp = strings_compare(sub(a, len), sub(b, len - 1));
            if (cmp != 1)
                testing_t_errorf_v(t, "CompareBshorter(%d) = %d", len, cmp);
        }
        for (Int k = last_len; k < len; k++) {
            b[k] = (Byte)(a[k] - 1);
            cmp = strings_compare(sub(a, len), sub(b, len));
            if (cmp != 1)
                testing_t_errorf_v(t, "CompareAbigger(%d,%d) = %d", len, k, cmp);
            b[k] = (Byte)(a[k] + 1);
            cmp = strings_compare(sub(a, len), sub(b, len));
            if (cmp != -1)
                testing_t_errorf_v(t, "CompareBbigger(%d,%d) = %d", len, k, cmp);
            b[k] = a[k];
        }
        last_len = len;
    }
    arena_free(&ar);
}

static void TestClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str forty_two = strings_repeat(a, S("a"), 42);
    Str clone_tests[] = {
        S(""),     strings_clone(a, S("")), str_from_bytes(forty_two.p, 0), S("short"),
        forty_two,
    };
    for (Int i = 0; i < LEN(clone_tests); i++) {
        Str input = clone_tests[i];
        Str clone = strings_clone(a, input);
        if (!str_eq(clone, input))
            testing_t_errorf_v(t, "Clone(%q) = %q; want %q", input, clone, input);
        if (input.len != 0 && clone.p == input.p)
            testing_t_errorf_v(
                t,
                "Clone(%q) return value should not reference inputs backing "
                "memory.",
                input);
        if (input.len == 0 && clone.p != BURROW_STR_EMPTY.p)
            testing_t_errorf_v(
                t, "Clone(%q) return value should be equal to empty string.", input);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCompare)                                                                     \
    X(TestCompareIdenticalString)                                                      \
    X(TestCompareStrings)                                                              \
    X(TestClone)

TESTING_MAIN(TESTS)
