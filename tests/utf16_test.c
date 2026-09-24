/* Derived from Go's src/unicode/utf16/utf16_test.go.
 * Go source: go1.27.1.
 *
 * Two of Go's tests are not here. TestConstants checks names that Go exports
 * only to its own tests through export_test.go, and burrow has no such names.
 * TestAllocationsDecode checks that Decode does not allocate, which in Go
 * means the escape analysis kept its 64 rune buffer on the stack. Here the
 * caller passes the allocator, so whether the result lands on the heap is the
 * caller's choice and there is nothing for the test to check.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/unicode/utf16.h"

#include <string.h>

enum {
    Surr1 = 0xd800,
    Surr3 = 0xe000,
    SurrSelf = 0x10000,
    MaxRune = 0x10FFFF,
};

/* Go's tables. Each slice is a named array, and E gives it with its length. */
#define E(x) x, (Int)(sizeof(x) / sizeof((x)[0]))

typedef struct EncodeTest {
    Rune *in;
    Int in_len;
    uint16_t *out;
    Int out_len;
} EncodeTest;

static Rune enc_in0[] = {1, 2, 3, 4};
static uint16_t enc_out0[] = {1, 2, 3, 4};
static Rune enc_in1[] = {0xffff, 0x10000, 0x10001, 0x12345, 0x10ffff};
static uint16_t enc_out1[] = {0xffff, 0xd800, 0xdc00, 0xd800, 0xdc01,
                              0xd808, 0xdf45, 0xdbff, 0xdfff};
static Rune enc_in2[] = {'a', 'b', 0xd7ff, 0xd800, 0xdfff, 0xe000, 0x110000, -1};
static uint16_t enc_out2[] = {'a', 'b', 0xd7ff, 0xfffd, 0xfffd, 0xe000, 0xfffd, 0xfffd};

static EncodeTest encodeTests[] = {
    {E(enc_in0), E(enc_out0)},
    {E(enc_in1), E(enc_out1)},
    {E(enc_in2), E(enc_out2)},
};

typedef struct DecodeTest {
    uint16_t *in;
    Int in_len;
    Rune *out;
    Int out_len;
} DecodeTest;

static uint16_t dec_in0[] = {1, 2, 3, 4};
static Rune dec_out0[] = {1, 2, 3, 4};
static uint16_t dec_in1[] = {0xffff, 0xd800, 0xdc00, 0xd800, 0xdc01,
                             0xd808, 0xdf45, 0xdbff, 0xdfff};
static Rune dec_out1[] = {0xffff, 0x10000, 0x10001, 0x12345, 0x10ffff};
static uint16_t dec_in2[] = {0xd800, 'a'};
static Rune dec_out2[] = {0xfffd, 'a'};
static uint16_t dec_in3[] = {0xdfff};
static Rune dec_out3[] = {0xfffd};

static DecodeTest decodeTests[] = {
    {E(dec_in0), E(dec_out0)},
    {E(dec_in1), E(dec_out1)},
    {E(dec_in2), E(dec_out2)},
    {E(dec_in3), E(dec_out3)},
};

#define COUNT(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A table entry as a slice, for comparing and printing. */
static Slice view(void *p, Int n, const Type *elem) {
    return slice_from(p, n, n, elem);
}

static bool slices_equal(Slice x, Slice y) {
    if (x.len != y.len || x.elem != y.elem)
        return false;
    return x.len == 0 || memcmp(x.p, y.p, (size_t)x.len * x.elem->size) == 0;
}

static void TestRuneLen(TestingT *t) {
    static const struct {
        Rune r;
        Int length;
    } tests[] = {
        {0, 1},        {Surr1 - 1, 1}, {Surr3, 1},        {SurrSelf - 1, 1},
        {SurrSelf, 2}, {MaxRune, 2},   {MaxRune + 1, -1}, {-1, -1},
    };
    for (Int i = 0; i < COUNT(tests); i++) {
        Int length = utf16_rune_len(tests[i].r);
        if (length != tests[i].length)
            testing_t_errorf_v(t, "RuneLen(%#U) = %d, want %d", tests[i].r, length,
                               tests[i].length);
    }
}

static void TestEncode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < COUNT(encodeTests); i++) {
        const EncodeTest *tt = &encodeTests[i];
        Slice in = view(tt->in, tt->in_len, TYPE_RUNE);
        Slice want = view(tt->out, tt->out_len, TYPE_UINT16);
        Slice out = utf16_encode(a, in);
        if (!slices_equal(out, want))
            testing_t_errorf_v(t, "Encode(%x) = %x; want %x", in, out, want);
    }
    arena_free(&ar);
}

static void TestAppendRune(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < COUNT(encodeTests); i++) {
        const EncodeTest *tt = &encodeTests[i];
        Slice out = {0};
        for (Int j = 0; j < tt->in_len; j++)
            out = utf16_append_rune(a, out, tt->in[j]);
        Slice want = view(tt->out, tt->out_len, TYPE_UINT16);
        if (!slices_equal(out, want))
            testing_t_errorf_v(t, "AppendRune(%x) = %x; want %x",
                               view(tt->in, tt->in_len, TYPE_RUNE), out, want);
    }
    arena_free(&ar);
}

static void TestEncodeRune(TestingT *t) {
    for (Int i = 0; i < COUNT(encodeTests); i++) {
        const EncodeTest *tt = &encodeTests[i];
        const uint16_t *out = tt->out;
        Int j = 0;
        for (Int k = 0; k < tt->in_len; k++) {
            Rune r = tt->in[k];
            Rune r2;
            Rune r1 = utf16_encode_rune(r, &r2);
            if (r < 0x10000 || r > BURROW_RUNE_MAX) {
                if (j >= tt->out_len) {
                    testing_t_errorf_v(t, "#%d: ran out of tt.out", i);
                    break;
                }
                if (r1 != BURROW_RUNE_ERROR || r2 != BURROW_RUNE_ERROR)
                    testing_t_errorf_v(
                        t, "EncodeRune(%#x) = %#x, %#x; want 0xfffd, 0xfffd", r, r1,
                        r2);
                j++;
            } else {
                if (j + 1 >= tt->out_len) {
                    testing_t_errorf_v(t, "#%d: ran out of tt.out", i);
                    break;
                }
                if (r1 != (Rune)out[j] || r2 != (Rune)out[j + 1])
                    testing_t_errorf_v(t, "EncodeRune(%#x) = %#x, %#x; want %#x, %#x",
                                       r, r1, r2, out[j], out[j + 1]);
                j += 2;
                Rune dec = utf16_decode_rune(r1, r2);
                if (dec != r)
                    testing_t_errorf_v(t, "DecodeRune(%#x, %#x) = %#x; want %#x", r1,
                                       r2, dec, r);
            }
        }
        if (j != tt->out_len)
            testing_t_errorf_v(t, "#%d: EncodeRune didn't generate enough output", i);
    }
}

static void TestDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < COUNT(decodeTests); i++) {
        const DecodeTest *tt = &decodeTests[i];
        Slice in = view(tt->in, tt->in_len, TYPE_UINT16);
        Slice want = view(tt->out, tt->out_len, TYPE_RUNE);
        Slice out = utf16_decode(a, in);
        if (!slices_equal(out, want))
            testing_t_errorf_v(t, "Decode(%x) = %x; want %x", in, out, want);
    }
    arena_free(&ar);
}

/* Go's TestAllocationsDecode also checks that Decode never returns nil, and
 * that part carries over. So does its twin for Encode, which Go gets from
 * make and never tests. */
static void TestEmptyIsNotNil(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    if (slice_is_nil(utf16_decode(a, slice_nil(TYPE_UINT16))))
        testing_t_errorf_v(t, "Decode(nil) = nil");
    if (slice_is_nil(utf16_encode(a, slice_nil(TYPE_RUNE))))
        testing_t_errorf_v(t, "Encode(nil) = nil");
    arena_free(&ar);
}

static void TestDecodeRune(TestingT *t) {
    static const struct {
        Rune r1, r2;
        Rune want;
    } tests[] = {
        {0xd800, 0xdc00, 0x10000}, {0xd800, 0xdc01, 0x10001},
        {0xd808, 0xdf45, 0x12345}, {0xdbff, 0xdfff, 0x10ffff},
        {0xd800, 'a', 0xfffd}, /* illegal, replacement rune substituted */
    };
    for (Int i = 0; i < COUNT(tests); i++) {
        Rune got = utf16_decode_rune(tests[i].r1, tests[i].r2);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d: DecodeRune(%q, %q) = %v; want %v", i,
                               tests[i].r1, tests[i].r2, got, tests[i].want);
    }
}

static void TestIsSurrogate(TestingT *t) {
    static const struct {
        Rune r;
        bool want;
    } tests[] = {
        /* from https://en.wikipedia.org/wiki/UTF-16 */
        {0x007A, false},   /* LATIN SMALL LETTER Z */
        {0x6C34, false},   /* CJK UNIFIED IDEOGRAPH-6C34 (water) */
        {0xFEFF, false},   /* Byte Order Mark */
        {0x10000, false},  /* LINEAR B SYLLABLE B008 A (first non-BMP code point) */
        {0x1D11E, false},  /* MUSICAL SYMBOL G CLEF */
        {0x10FFFD, false}, /* PRIVATE USE CHARACTER-10FFFD (last Unicode code point) */
        {0xd7ff, false},   /* surr1-1 */
        {0xd800, true},    /* surr1 */
        {0xdc00, true},    /* surr2 */
        {0xe000, false},   /* surr3 */
        {0xdfff, true},    /* surr3-1 */
    };
    for (Int i = 0; i < COUNT(tests); i++) {
        bool got = utf16_is_surrogate(tests[i].r);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d: IsSurrogate(%q) = %v; want %v", i, tests[i].r,
                               got, tests[i].want);
    }
}

static void BenchmarkDecodeValidASCII(TestingB *b) {
    /* "hello world" */
    static uint16_t units[] = {104, 101, 108, 108, 111, 32, 119, 111, 114, 108, 100};
    Slice data = slice_from(units, COUNT(units), COUNT(units), TYPE_UINT16);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        utf16_decode(a, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkDecodeValidJapaneseChars(TestingB *b) {
    /* "日本語日本語日本語" */
    static uint16_t units[] = {26085, 26412, 35486, 26085, 26412,
                               35486, 26085, 26412, 35486};
    Slice data = slice_from(units, COUNT(units), COUNT(units), TYPE_UINT16);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        utf16_decode(a, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static volatile Rune sink;

static void BenchmarkDecodeRune(TestingB *b) {
    Rune rs[10];
    /* U+1D4D0 to U+1D4D4: MATHEMATICAL BOLD SCRIPT CAPITAL LETTERS */
    static const Rune u[] = {0x1D4D0, 0x1D4D1, 0x1D4D2, 0x1D4D3, 0x1D4D4};
    for (int i = 0; i < 5; i++)
        rs[2 * i] = utf16_encode_rune(u[i], &rs[2 * i + 1]);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (int j = 0; j < 5; j++)
            sink = utf16_decode_rune(rs[2 * j], rs[2 * j + 1]);
    }
}

static void BenchmarkEncodeValidASCII(TestingB *b) {
    static Rune runes[] = {'h', 'e', 'l', 'l', 'o'};
    Slice data = slice_from(runes, COUNT(runes), COUNT(runes), TYPE_RUNE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        utf16_encode(a, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkEncodeValidJapaneseChars(TestingB *b) {
    static Rune runes[] = {0x65E5, 0x672C, 0x8A9E}; /* '日', '本', '語' */
    Slice data = slice_from(runes, COUNT(runes), COUNT(runes), TYPE_RUNE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        utf16_encode(a, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void append_runes(TestingB *b, const Rune *data, Int n) {
    uint16_t buf[16];
    Slice a = slice_from(buf, 0, 2 * n, TYPE_UINT16);
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (Int j = 0; j < n; j++)
            a = utf16_append_rune(NULL, a, data[j]);
        a = slice_sub(a, 0, 0);
    }
}

static void BenchmarkAppendRuneValidASCII(TestingB *b) {
    static const Rune data[] = {'h', 'e', 'l', 'l', 'o'};
    append_runes(b, data, 5);
}

static void BenchmarkAppendRuneValidJapaneseChars(TestingB *b) {
    static const Rune data[] = {0x65E5, 0x672C, 0x8A9E}; /* '日', '本', '語' */
    append_runes(b, data, 3);
}

static void BenchmarkEncodeRune(TestingB *b) {
    static const Rune u[] = {0x1D4D0, 0x1D4D1, 0x1D4D2, 0x1D4D3, 0x1D4D4};
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (int j = 0; j < 5; j++)
            sink = utf16_encode_rune(u[j], NULL);
    }
}

#define TESTS(X)                                                                       \
    X(TestRuneLen)                                                                     \
    X(TestEncode)                                                                      \
    X(TestAppendRune)                                                                  \
    X(TestEncodeRune)                                                                  \
    X(TestDecode)                                                                      \
    X(TestEmptyIsNotNil)                                                               \
    X(TestDecodeRune)                                                                  \
    X(TestIsSurrogate)                                                                 \
    X(BenchmarkDecodeValidASCII)                                                       \
    X(BenchmarkDecodeValidJapaneseChars)                                               \
    X(BenchmarkDecodeRune)                                                             \
    X(BenchmarkEncodeValidASCII)                                                       \
    X(BenchmarkEncodeValidJapaneseChars)                                               \
    X(BenchmarkAppendRuneValidASCII)                                                   \
    X(BenchmarkAppendRuneValidJapaneseChars)                                           \
    X(BenchmarkEncodeRune)

TESTING_MAIN(TESTS)
