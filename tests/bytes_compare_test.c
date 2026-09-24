/* Derived from Go's src/bytes/compare_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include "check.h"

#include <string.h>

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A test case slice: NULL text is a nil slice. */
static Slice bs(const char *text) {
    if (text == NULL)
        return slice_nil(TYPE_BYTE);
    Int n = (Int)strlen(text);
    return slice_from((void *)(uintptr_t)text, n, n, TYPE_BYTE);
}

/* For printing a slice with %q. */
static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static const struct {
    const char *a, *b;
    Int i;
} compareTests[] = {
    {"", "", 0},
    {"a", "", 1},
    {"", "a", -1},
    {"abc", "abc", 0},
    {"abd", "abc", 1},
    {"abc", "abd", -1},
    {"ab", "abc", -1},
    {"abc", "ab", 1},
    {"x", "ab", 1},
    {"ab", "x", -1},
    {"x", "a", 1},
    {"b", "x", -1},
    /* test runtime·memeq's chunked implementation */
    {"abcdefgh", "abcdefgh", 0},
    {"abcdefghi", "abcdefghi", 0},
    {"abcdefghi", "abcdefghj", -1},
    {"abcdefghj", "abcdefghi", 1},
    /* nil tests */
    {NULL, NULL, 0},
    {"", NULL, 0},
    {NULL, "", 0},
    {"a", NULL, 1},
    {NULL, "a", -1},
};

static void TestCompare(TestingT *t) {
    Byte buffer[32];
    for (Int i = 0; i < LEN(compareTests); i++) {
        Slice a = bs(compareTests[i].a);
        Slice b = bs(compareTests[i].b);
        Int num_shifts = 16;
        /* vary the input alignment of b */
        for (Int offset = 0; offset <= num_shifts; offset++) {
            Slice shifted_b = slice_from(buffer + offset, b.len, b.len, TYPE_BYTE);
            if (b.len > 0)
                memcpy(shifted_b.p, b.p, (size_t)b.len);
            Int cmp = bytes_compare(a, shifted_b);
            if (cmp != compareTests[i].i)
                testing_t_errorf_v(t, "Compare(%q, %q), offset %d = %v; want %v", q(a),
                                   q(b), offset, cmp, compareTests[i].i);
        }
    }
}

static void TestCompareIdenticalSlice(TestingT *t) {
    Slice b = bs("Hello Gophers!");
    if (bytes_compare(b, b) != 0)
        testing_t_error_v(t, "b != b");
    if (bytes_compare(b, slice_sub(b, 0, 1)) != 1)
        testing_t_error_v(t, "b > b[:1] failed");
}

static Slice upto(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static void TestCompareBytes(TestingT *t) {
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
        Int cmp = bytes_compare(upto(a, len), upto(b, len));
        if (cmp != 0)
            testing_t_errorf_v(t, "CompareIdentical(%d) = %d", len, cmp);
        if (len > 0) {
            cmp = bytes_compare(upto(a, len - 1), upto(b, len));
            if (cmp != -1)
                testing_t_errorf_v(t, "CompareAshorter(%d) = %d", len, cmp);
            cmp = bytes_compare(upto(a, len), upto(b, len - 1));
            if (cmp != 1)
                testing_t_errorf_v(t, "CompareBshorter(%d) = %d", len, cmp);
        }
        for (Int k = 0; k < len; k++) {
            b[k] = (Byte)(a[k] - 1);
            cmp = bytes_compare(upto(a, len), upto(b, len));
            if (cmp != 1)
                testing_t_errorf_v(t, "CompareAbigger(%d,%d) = %d", len, k, cmp);
            b[k] = (Byte)(a[k] + 1);
            cmp = bytes_compare(upto(a, len), upto(b, len));
            if (cmp != -1)
                testing_t_errorf_v(t, "CompareBbigger(%d,%d) = %d", len, k, cmp);
            b[k] = a[k];
        }
    }
    arena_free(&ar);
}

static void TestEndianBaseCompare(TestingT *t) {
    /* This test compares byte slices that are almost identical, except one
     * difference that for some j, a[j]>b[j] and a[j+1]<b[j+1]. If the
     * implementation compares large chunks with wrong endianness, it gets wrong
     * result. no vector register is larger than 512 bytes for now */
    enum { max_length = 512 };
    Byte a[max_length], b[max_length];
    /* randomish but deterministic data. No 0 or 255. */
    for (Int i = 0; i < max_length; i++) {
        a[i] = (Byte)(1 + 31 * i % 254);
        b[i] = (Byte)(1 + 31 * i % 254);
    }
    for (Int i = 2; i <= max_length; i <<= 1) {
        for (Int j = 0; j < i - 1; j++) {
            a[j] = (Byte)(b[j] - 1);
            a[j + 1] = (Byte)(b[j + 1] + 1);
            Int cmp = bytes_compare(upto(a, i), upto(b, i));
            if (cmp != -1)
                testing_t_errorf_v(t, "CompareBbigger(%d,%d) = %d", i, j, cmp);
            a[j] = (Byte)(b[j] + 1);
            a[j + 1] = (Byte)(b[j + 1] - 1);
            cmp = bytes_compare(upto(a, i), upto(b, i));
            if (cmp != 1)
                testing_t_errorf_v(t, "CompareAbigger(%d,%d) = %d", i, j, cmp);
            a[j] = b[j];
            a[j + 1] = b[j + 1];
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestCompare)                                                                     \
    X(TestCompareIdenticalSlice)                                                       \
    X(TestCompareBytes)                                                                \
    X(TestEndianBaseCompare)

TESTING_MAIN(TESTS)
