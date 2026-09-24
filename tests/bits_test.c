/* Derived from Go's src/math/bits/bits_test.go.
 * Go source: go1.27.1.
 *
 * tests/bits_portable_test.c includes this file again with the compiler
 * builtins turned off, so every check here runs against both the builtins and
 * Go's portable code. BITS_SUITE is the name each one reports under.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "burrow/math/bits.h"

#ifndef BITS_SUITE
#define BITS_SUITE "bits"
#endif

#define M64 UINT64_MAX
#define M32 UINT32_MAX

#if BURROW_PTR_BITS == 64
#define MU ((Uint)UINT64_MAX)
#else
#define MU ((Uint)UINT32_MAX)
#endif

#define DE_BRUIJN64 0x03f79d71b4ca8b09ull

/* Go's table of the answers for every byte, worked out the slow way so that
 * it cannot share a bug with the code it checks. */
typedef struct Entry {
    Int nlz, ntz, pop;
} Entry;

static Entry tab[256];

static void init_tab(void) {
    tab[0] = (Entry){8, 8, 0};
    for (int i = 1; i < 256; i++) {
        int x = i, n = 0;
        while ((x & 0x80) == 0) {
            n++;
            x <<= 1;
        }
        tab[i].nlz = n;

        x = i;
        n = 0;
        while ((x & 1) == 0) {
            n++;
            x >>= 1;
        }
        tab[i].ntz = n;

        x = i;
        n = 0;
        while (x != 0) {
            n += x & 1;
            x >>= 1;
        }
        tab[i].pop = n;
    }
}

static void TestUintSize(TestingT *t) {
    CHECK_INT_EQ(BITS_UINT_SIZE, (Int)sizeof(Uint) * 8);
}

static void TestLeadingZeros(TestingT *t) {
    for (int i = 0; i < 256; i++) {
        Int nlz = tab[i].nlz;
        for (int k = 0; k < 64 - 8; k++) {
            uint64_t x = (uint64_t)i << k;
            if (x <= 0xff)
                CHECK_INT_EQ(bits_leading_zeros8((uint8_t)x), x == 0 ? 8 : nlz - k);
            if (x <= 0xffff)
                CHECK_INT_EQ(bits_leading_zeros16((uint16_t)x),
                             x == 0 ? 16 : nlz - k + 8);
            if (x <= 0xffffffff) {
                Int want = x == 0 ? 32 : nlz - k + 24;
                CHECK_INT_EQ(bits_leading_zeros32((uint32_t)x), want);
#if BURROW_PTR_BITS == 32
                CHECK_INT_EQ(bits_leading_zeros((Uint)x), want);
#endif
            }
            Int want = x == 0 ? 64 : nlz - k + 56;
            CHECK_INT_EQ(bits_leading_zeros64(x), want);
#if BURROW_PTR_BITS == 64
            CHECK_INT_EQ(bits_leading_zeros((Uint)x), want);
#endif
        }
    }
}

static void TestTrailingZeros(TestingT *t) {
    for (int i = 0; i < 256; i++) {
        Int ntz = tab[i].ntz;
        for (int k = 0; k < 64 - 8; k++) {
            uint64_t x = (uint64_t)i << k;
            Int want = ntz + k;
            if (x <= 0xff)
                CHECK_INT_EQ(bits_trailing_zeros8((uint8_t)x), x == 0 ? 8 : want);
            if (x <= 0xffff)
                CHECK_INT_EQ(bits_trailing_zeros16((uint16_t)x), x == 0 ? 16 : want);
            if (x <= 0xffffffff) {
                CHECK_INT_EQ(bits_trailing_zeros32((uint32_t)x), x == 0 ? 32 : want);
#if BURROW_PTR_BITS == 32
                CHECK_INT_EQ(bits_trailing_zeros((Uint)x), x == 0 ? 32 : want);
#endif
            }
            CHECK_INT_EQ(bits_trailing_zeros64(x), x == 0 ? 64 : want);
#if BURROW_PTR_BITS == 64
            CHECK_INT_EQ(bits_trailing_zeros((Uint)x), x == 0 ? 64 : want);
#endif
        }
    }
}

static void check_ones_count(TestingT *t, uint64_t x, Int want) {
    if (x <= 0xff)
        CHECK_INT_EQ(bits_ones_count8((uint8_t)x), want);
    if (x <= 0xffff)
        CHECK_INT_EQ(bits_ones_count16((uint16_t)x), want);
    if (x <= 0xffffffff) {
        CHECK_INT_EQ(bits_ones_count32((uint32_t)x), want);
#if BURROW_PTR_BITS == 32
        CHECK_INT_EQ(bits_ones_count((Uint)x), want);
#endif
    }
    CHECK_INT_EQ(bits_ones_count64(x), want);
#if BURROW_PTR_BITS == 64
    CHECK_INT_EQ(bits_ones_count((Uint)x), want);
#endif
}

static void TestOnesCount(TestingT *t) {
    uint64_t x = 0;
    for (int i = 0; i <= 64; i++) {
        check_ones_count(t, x, i);
        x = x << 1 | 1;
    }
    for (int i = 64; i >= 0; i--) {
        check_ones_count(t, x, i);
        x = x << 1;
    }
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 64 - 8; k++)
            check_ones_count(t, (uint64_t)i << k, tab[i].pop);
}

static void TestRotateLeft(TestingT *t) {
    uint64_t m = DE_BRUIJN64;

    for (unsigned k = 0; k < 128; k++) {
        uint8_t x8 = (uint8_t)m;
        uint8_t want8 = (uint8_t)(x8 << (k & 7) | x8 >> ((8 - k) & 7));
        CHECK_INT_EQ(bits_rotate_left8(x8, (Int)k), want8);
        CHECK_INT_EQ(bits_rotate_left8(want8, -(Int)k), x8);

        uint16_t x16 = (uint16_t)m;
        uint16_t want16 = (uint16_t)(x16 << (k & 15) | x16 >> ((16 - k) & 15));
        CHECK_INT_EQ(bits_rotate_left16(x16, (Int)k), want16);
        CHECK_INT_EQ(bits_rotate_left16(want16, -(Int)k), x16);

        uint32_t x32 = (uint32_t)m;
        uint32_t want32 = x32 << (k & 31) | x32 >> ((32 - k) & 31);
        CHECK_INT_EQ(bits_rotate_left32(x32, (Int)k), want32);
        CHECK_INT_EQ(bits_rotate_left32(want32, -(Int)k), x32);

        uint64_t x64 = m;
        uint64_t want64 = x64 << (k & 63) | x64 >> ((64 - k) & 63);
        CHECK(bits_rotate_left64(x64, (Int)k) == want64);
        CHECK(bits_rotate_left64(want64, -(Int)k) == x64);

        Uint x = (Uint)m;
        Uint want = x << (k & (BITS_UINT_SIZE - 1)) |
                    x >> ((BITS_UINT_SIZE - k) & (BITS_UINT_SIZE - 1));
        CHECK(bits_rotate_left(x, (Int)k) == want);
        CHECK(bits_rotate_left(want, -(Int)k) == x);
    }
}

static void check_reverse(TestingT *t, uint64_t x64, uint64_t want64) {
    CHECK_INT_EQ(bits_reverse8((uint8_t)x64), (uint8_t)(want64 >> 56));
    CHECK_INT_EQ(bits_reverse16((uint16_t)x64), (uint16_t)(want64 >> 48));
    CHECK_INT_EQ(bits_reverse32((uint32_t)x64), (uint32_t)(want64 >> 32));
    CHECK(bits_reverse64(x64) == want64);
#if BURROW_PTR_BITS == 64
    CHECK(bits_reverse((Uint)x64) == want64);
#else
    CHECK(bits_reverse((Uint)x64) == (Uint)(want64 >> 32));
#endif
}

static void TestReverse(TestingT *t) {
    for (unsigned i = 0; i < 64; i++)
        check_reverse(t, (uint64_t)1 << i, (uint64_t)1 << (63 - i));

    static const struct {
        uint64_t x, r;
    } tests[] = {
        {0, 0},
        {0x1, 0x8ull << 60},
        {0x2, 0x4ull << 60},
        {0x3, 0xcull << 60},
        {0x4, 0x2ull << 60},
        {0x5, 0xaull << 60},
        {0x6, 0x6ull << 60},
        {0x7, 0xeull << 60},
        {0x8, 0x1ull << 60},
        {0x9, 0x9ull << 60},
        {0xa, 0x5ull << 60},
        {0xb, 0xdull << 60},
        {0xc, 0x3ull << 60},
        {0xd, 0xbull << 60},
        {0xe, 0x7ull << 60},
        {0xf, 0xfull << 60},
        {0x5686487, 0xe12616a000000000ull},
        {0x0123456789abcdefull, 0xf7b3d591e6a2c480ull},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        check_reverse(t, tests[i].x, tests[i].r);
        check_reverse(t, tests[i].r, tests[i].x);
    }
}

static void check_reverse_bytes(TestingT *t, uint64_t x64, uint64_t want64) {
    CHECK_INT_EQ(bits_reverse_bytes16((uint16_t)x64), (uint16_t)(want64 >> 48));
    CHECK_INT_EQ(bits_reverse_bytes32((uint32_t)x64), (uint32_t)(want64 >> 32));
    CHECK(bits_reverse_bytes64(x64) == want64);
#if BURROW_PTR_BITS == 64
    CHECK(bits_reverse_bytes((Uint)x64) == want64);
#else
    CHECK(bits_reverse_bytes((Uint)x64) == (Uint)(want64 >> 32));
#endif
}

static void TestReverseBytes(TestingT *t) {
    static const struct {
        uint64_t x, r;
    } tests[] = {
        {0, 0},
        {0x01, 0x01ull << 56},
        {0x0123, 0x2301ull << 48},
        {0x012345, 0x452301ull << 40},
        {0x01234567, 0x67452301ull << 32},
        {0x0123456789ull, 0x8967452301ull << 24},
        {0x0123456789abull, 0xab8967452301ull << 16},
        {0x0123456789abcdull, 0xcdab8967452301ull << 8},
        {0x0123456789abcdefull, 0xefcdab8967452301ull},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        check_reverse_bytes(t, tests[i].x, tests[i].r);
        check_reverse_bytes(t, tests[i].r, tests[i].x);
    }
}

static void TestLen(TestingT *t) {
    for (int i = 0; i < 256; i++) {
        Int len = 8 - tab[i].nlz;
        for (int k = 0; k < 64 - 8; k++) {
            uint64_t x = (uint64_t)i << k;
            Int want = x == 0 ? 0 : len + k;
            if (x <= 0xff)
                CHECK_INT_EQ(bits_len8((uint8_t)x), want);
            if (x <= 0xffff)
                CHECK_INT_EQ(bits_len16((uint16_t)x), want);
            if (x <= 0xffffffff)
                CHECK_INT_EQ(bits_len32((uint32_t)x), want);
            CHECK_INT_EQ(bits_len64(x), want);
            if (BITS_UINT_SIZE == 64 || x <= 0xffffffff)
                CHECK_INT_EQ(bits_len((Uint)x), want);
        }
    }
}

/* ------------------------------------------------------------- arithmetic */

typedef struct AddCase {
    uint64_t x, y, c, z, cout;
} AddCase;

#define ADD_CASES(M)                                                                   \
    {0, 0, 0, 0, 0}, {0, 1, 0, 1, 0}, {0, 0, 1, 1, 0}, {0, 1, 1, 2, 0},                \
        {12345, 67890, 0, 80235, 0}, {12345, 67890, 1, 80236, 0}, {M, 1, 0, 0, 1},     \
        {M, 0, 1, 0, 1}, {M, 1, 1, 1, 1}, {M, M, 0, M - 1, 1}, {M, M, 1, M, 1}

static void TestAddSubUint(TestingT *t) {
    static const AddCase cases[] = {ADD_CASES(MU)};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Uint x = (Uint)cases[i].x, y = (Uint)cases[i].y, c = (Uint)cases[i].c;
        Uint z = (Uint)cases[i].z, cout = (Uint)cases[i].cout, got_c;
        CHECK(bits_add(x, y, c, &got_c) == z && got_c == cout);
        CHECK(bits_add(y, x, c, &got_c) == z && got_c == cout);
        CHECK(bits_sub(z, x, c, &got_c) == y && got_c == cout);
        CHECK(bits_sub(z, y, c, &got_c) == x && got_c == cout);
    }
}

static void TestAddSubUint32(TestingT *t) {
    static const AddCase cases[] = {ADD_CASES(M32)};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t x = (uint32_t)cases[i].x, y = (uint32_t)cases[i].y;
        uint32_t c = (uint32_t)cases[i].c, z = (uint32_t)cases[i].z;
        uint32_t cout = (uint32_t)cases[i].cout, got_c;
        CHECK(bits_add32(x, y, c, &got_c) == z && got_c == cout);
        CHECK(bits_add32(y, x, c, &got_c) == z && got_c == cout);
        CHECK(bits_sub32(z, x, c, &got_c) == y && got_c == cout);
        CHECK(bits_sub32(z, y, c, &got_c) == x && got_c == cout);
    }
}

static void TestAddSubUint64(TestingT *t) {
    static const AddCase cases[] = {ADD_CASES(M64)};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const AddCase *a = &cases[i];
        uint64_t got_c;
        CHECK(bits_add64(a->x, a->y, a->c, &got_c) == a->z && got_c == a->cout);
        CHECK(bits_add64(a->y, a->x, a->c, &got_c) == a->z && got_c == a->cout);
        CHECK(bits_sub64(a->z, a->x, a->c, &got_c) == a->y && got_c == a->cout);
        CHECK(bits_sub64(a->z, a->y, a->c, &got_c) == a->x && got_c == a->cout);
    }
}

static void TestTheSecondResultCanBeThrownAway(TestingT *t) {
    CHECK(bits_add64(M64, 1, 0, NULL) == 0);
    CHECK(bits_sub64(0, 1, 0, NULL) == M64);
    CHECK(bits_mul64(M64, M64, NULL) == M64 - 1);
    CHECK(bits_div64(1, 0, 2, NULL) == (uint64_t)1 << 63);
    CHECK(bits_add32(M32, 1, 0, NULL) == 0);
    CHECK(bits_mul32(M32, M32, NULL) == M32 - 1);
    CHECK(bits_div32(1, 0, 2, NULL) == (uint32_t)1 << 31);
}

typedef struct MulCase {
    uint64_t x, y, hi, lo, r;
} MulCase;

static void TestMulDiv(TestingT *t) {
    static const MulCase cases[] = {
        {(uint64_t)1 << (BITS_UINT_SIZE - 1), 2, 1, 0, 1},
        {MU, MU, MU - 1, 1, 42},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Uint x = (Uint)cases[i].x, y = (Uint)cases[i].y, hi = (Uint)cases[i].hi;
        Uint lo = (Uint)cases[i].lo, r = (Uint)cases[i].r, got_lo, got_r;
        CHECK(bits_mul(x, y, &got_lo) == hi && got_lo == lo);
        CHECK(bits_mul(y, x, &got_lo) == hi && got_lo == lo);
        CHECK(bits_div(hi, lo + r, y, &got_r) == x && got_r == r);
        CHECK(bits_div(hi, lo + r, x, &got_r) == y && got_r == r);
    }
}

static void TestMulDiv32(TestingT *t) {
    static const MulCase cases[] = {
        {(uint64_t)1 << 31, 2, 1, 0, 1},
        {0xc47dfa8c, 50911, 0x98a4, 0x998587f4, 13},
        {M32, M32, M32 - 1, 1, 42},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t x = (uint32_t)cases[i].x, y = (uint32_t)cases[i].y;
        uint32_t hi = (uint32_t)cases[i].hi, lo = (uint32_t)cases[i].lo;
        uint32_t r = (uint32_t)cases[i].r, got_lo, got_r;
        CHECK(bits_mul32(x, y, &got_lo) == hi && got_lo == lo);
        CHECK(bits_mul32(y, x, &got_lo) == hi && got_lo == lo);
        CHECK(bits_div32(hi, lo + r, y, &got_r) == x && got_r == r);
        CHECK(bits_div32(hi, lo + r, x, &got_r) == y && got_r == r);
    }
}

static void TestMulDiv64(TestingT *t) {
    static const MulCase cases[] = {
        {(uint64_t)1 << 63, 2, 1, 0, 1},
        {0x3626229738a3b9ull, 0xd8988a9f1cc4a61ull, 0x2dd0712657fe8ull,
         0x9dd6a3364c358319ull, 13},
        {M64, M64, M64 - 1, 1, 42},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const MulCase *a = &cases[i];
        uint64_t got_lo, got_r;
        CHECK(bits_mul64(a->x, a->y, &got_lo) == a->hi && got_lo == a->lo);
        CHECK(bits_mul64(a->y, a->x, &got_lo) == a->hi && got_lo == a->lo);
        CHECK(bits_div64(a->hi, a->lo + a->r, a->y, &got_r) == a->x && got_r == a->r);
        CHECK(bits_div64(a->hi, a->lo + a->r, a->x, &got_r) == a->y && got_r == a->r);
    }
}

/* The inputs are volatile so that the compiler cannot see the panic coming
 * and fold the call into something else. */
static volatile uint64_t one = 1, zero = 0;

static void TestDivPanicsOnOverflow(TestingT *t) {
    CHECK_FATAL(bits_div((Uint)one, 0, (Uint)one, NULL),
                "runtime error: integer overflow");
    CHECK_FATAL(bits_div32((uint32_t)one, 0, (uint32_t)one, NULL),
                "runtime error: integer overflow");
    CHECK_FATAL(bits_div64(one, 0, one, NULL), "runtime error: integer overflow");
}

static void TestDivPanicsOnZero(TestingT *t) {
    CHECK_FATAL(bits_div((Uint)one, (Uint)one, (Uint)zero, NULL),
                "runtime error: integer divide by zero");
    CHECK_FATAL(bits_div32((uint32_t)one, (uint32_t)one, (uint32_t)zero, NULL),
                "runtime error: integer divide by zero");
    CHECK_FATAL(bits_div64(one, one, zero, NULL),
                "runtime error: integer divide by zero");
}

static void TestRemPanicsOnZeroAndOnlyOnZero(TestingT *t) {
    CHECK_FATAL(bits_rem((Uint)one, (Uint)one, (Uint)zero),
                "runtime error: integer divide by zero");
    CHECK_FATAL(bits_rem32((uint32_t)one, (uint32_t)one, (uint32_t)zero),
                "runtime error: integer divide by zero");
    CHECK_FATAL(bits_rem64(one, one, zero), "runtime error: integer divide by zero");
}

static void TestRem32(TestingT *t) {
    uint32_t hi = 510510, lo = 9699690, y = 510510 + 1;
    for (int i = 0; i < 1000; i++) {
        uint32_t r2;
        bits_div32(hi, lo, y, &r2);
        CHECK_INT_EQ(bits_rem32(hi, lo, y), r2);
        y += 13;
    }
}

static void TestRem32Overflow(TestingT *t) {
    uint32_t hi = 510510, lo = 9699690, y = 7;
    for (int i = 0; i < 1000; i++) {
        uint64_t r2;
        bits_div64(0, (uint64_t)hi << 32 | lo, y, &r2);
        CHECK_INT_EQ(bits_rem32(hi, lo, y), (uint32_t)r2);
        y += 13;
    }
}

static void TestRem64(TestingT *t) {
    uint64_t hi = 510510, lo = 9699690, y = 510510 + 1;
    for (int i = 0; i < 1000; i++) {
        uint64_t r2;
        bits_div64(hi, lo, y, &r2);
        CHECK(bits_rem64(hi, lo, y) == r2);
        y += 13;
    }
}

static void TestRem64Overflow(TestingT *t) {
    static const struct {
        uint64_t hi, lo, y, rem;
    } tests[] = {
        {42, 1119, 42, 27},
        {42, 1119, 38, 9},
        {42, 1119, 26, 23},
        {469, 0, 467, 271},
        {469, 0, 113, 58},
        {111111, 111111, 1171, 803},
        {3968194946088682615ull, 3192705705065114702ull, 1000037, 56067},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        CHECK(tests[i].hi >= tests[i].y);
        CHECK(bits_rem64(tests[i].hi, tests[i].lo, tests[i].y) == tests[i].rem);
    }
}

/* Not in Go's file. The division is the one piece of portable code with real
 * control flow in it, so it gets a sweep against the 128 bit answer where the
 * compiler has one, including divisors with the high bit set and without,
 * which take different amounts of normalising. */
static void TestDiv64AgreesWith128BitDivision(TestingT *t) {
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 U128;
    uint64_t s = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 20000; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        uint64_t y = s >> (i % 64);
        if (y == 0)
            y = 1;
        uint64_t hi = (s * 31) % y, lo = s * 0x2545f4914f6cdd1dull, r;
        U128 n = (U128)hi << 64 | lo;
        uint64_t q = bits_div64(hi, lo, y, &r);
        CHECK(q == (uint64_t)(n / y) && r == (uint64_t)(n % y));
        uint64_t got_lo, want_lo = (uint64_t)((U128)s * y);
        CHECK(bits_mul64(s, y, &got_lo) == (uint64_t)(((U128)s * y) >> 64) &&
              got_lo == want_lo);
    }
#endif
}

#define TESTS(X)                                                                       \
    X(TestUintSize)                                                                    \
    X(TestLeadingZeros)                                                                \
    X(TestTrailingZeros)                                                               \
    X(TestOnesCount)                                                                   \
    X(TestRotateLeft)                                                                  \
    X(TestReverse)                                                                     \
    X(TestReverseBytes)                                                                \
    X(TestLen)                                                                         \
    X(TestAddSubUint)                                                                  \
    X(TestAddSubUint32)                                                                \
    X(TestAddSubUint64)                                                                \
    X(TestTheSecondResultCanBeThrownAway)                                              \
    X(TestMulDiv)                                                                      \
    X(TestMulDiv32)                                                                    \
    X(TestMulDiv64)                                                                    \
    X(TestDivPanicsOnOverflow)                                                         \
    X(TestDivPanicsOnZero)                                                             \
    X(TestRemPanicsOnZeroAndOnlyOnZero)                                                \
    X(TestRem32)                                                                       \
    X(TestRem32Overflow)                                                               \
    X(TestRem64)                                                                       \
    X(TestRem64Overflow)                                                               \
    X(TestDiv64AgreesWith128BitDivision)

static int TestMain(TestingM *m) {
    init_tab();
    int code = testing_m_run(m);
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
