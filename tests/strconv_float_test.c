/* Derived from Go's src/internal/strconv/atof_test.go, ftoa_test.go,
 * atoc_test.go and ctoa_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "burrow/error.h"
#include "burrow/math.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/strconv.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Arena ar;
static Alloc *a;

#define S BURROW_S
#define COUNT(t) ((Int)(sizeof(t) / sizeof((t)[0])))

/* The reason a case expects, before it is wrapped in a NumError. */
typedef enum Want { E_NONE, E_SYNTAX, E_RANGE } Want;

typedef struct AtofCase {
    Str in;
    uint64_t out64;
    Want err64;
    uint32_t out32;
    Want err32;
} AtofCase;

/* An input too long for a string literal, which is prefix, then fill n times,
 * then suffix. want.in is left empty and filled in when it is built. */
typedef struct LongAtofCase {
    Str prefix;
    Byte fill;
    Int n;
    Str suffix;
    AtofCase want;
} LongAtofCase;

typedef struct FtoaCase {
    uint64_t in;
    Byte fmt;
    Int prec;
    Int bit_size;
    Str out;
} FtoaCase;

typedef struct AtocCase {
    Str in;
    Int bit_size;
    uint64_t re;
    uint64_t im;
    Want err;
} AtocCase;

#include "strconv_float_gen.h"

static uint64_t bits64(double f) {
    uint64_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static double from_bits64(uint64_t b) {
    double f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static uint32_t bits32(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static float from_bits32(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/* The same float, or both NaN, since the bits of a NaN that has been through a
 * conversion depend on the machine and Go does not promise them. */
static bool same64(double got, uint64_t want) {
    double w = from_bits64(want);
    if (math_is_nan((double)w))
        return math_is_nan((double)got);
    return bits64(got) == want;
}

static bool same32(float got, uint32_t want) {
    float w = from_bits32(want);
    if (math_is_nan((double)w))
        return math_is_nan((double)got);
    return bits32(got) == want;
}

static bool num_error_is(Error err, const char *func, Str in, Want want) {
    if (want == E_NONE)
        return BURROW_OK(err);

    Error reason = want == E_SYNTAX ? strconv_err_syntax : strconv_err_range;
    const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
    if (ne == NULL || !errors_is(err, reason))
        return false;
    if (!str_eq(ne->func, str_from_cstr(func)) || !str_eq(ne->num, in))
        return false;

    StrconvNumError expect = {str_from_cstr(func), in, reason};
    return str_eq(error_text(err), strconv_num_error_error(a, &expect));
}

static void check_atof(TestingT *t, const AtofCase *tt) {
    Error err = BURROW_NO_ERROR;
    double f = strconv_parse_float(tt->in, 64, &err);
    if (!same64(f, tt->out64))
        fprintf(stderr, "ParseFloat(%.*s, 64) = %016llx\n", (int)tt->in.len,
                (const char *)tt->in.p, (unsigned long long)bits64(f));
    CHECK(same64(f, tt->out64));
    CHECK(num_error_is(err, "ParseFloat", tt->in, tt->err64));

    err = BURROW_NO_ERROR;
    f = strconv_parse_float(tt->in, 32, &err);
    /* A 32 bit result is always a float, widened. */
    CHECK(math_is_nan((double)f) || (double)(float)f == f);
    if (!same32((float)f, tt->out32))
        fprintf(stderr, "ParseFloat(%.*s, 32) = %08lx\n", (int)tt->in.len,
                (const char *)tt->in.p, (unsigned long)bits32((float)f));
    CHECK(same32((float)f, tt->out32));
    CHECK(num_error_is(err, "ParseFloat", tt->in, tt->err32));
}

static void TestParseFloat(TestingT *t) {
    for (Int i = 0; i < COUNT(atof_tests); i++)
        check_atof(t, &atof_tests[i]);
}

static void TestParseLongFloat(TestingT *t) {
    for (Int i = 0; i < COUNT(long_atof_tests); i++) {
        const LongAtofCase *lt = &long_atof_tests[i];
        Int n = lt->prefix.len + lt->n + lt->suffix.len;
        Byte *p = mem_alloc(a, (size_t)n, 1);
        memcpy(p, lt->prefix.p, (size_t)lt->prefix.len);
        memset(p + lt->prefix.len, lt->fill, (size_t)lt->n);
        memcpy(p + lt->prefix.len + lt->n, lt->suffix.p, (size_t)lt->suffix.len);

        AtofCase tt = lt->want;
        tt.in = str_from_bytes(p, n);
        check_atof(t, &tt);
    }
}

/* Issue 42297: bit sizes other than 32 mean 64, because too much code passes 0
 * or 10 for Go to start refusing them. The comparisons are of bits because x87
 * evaluates a literal like 1.5e308 in long double. */
static void TestParseFloatOtherBitSizes(TestingT *t) {
    static const Int sizes[] = {0, 10, 100, 128};
    for (Int i = 0; i < COUNT(sizes); i++) {
        Error err = BURROW_NO_ERROR;
        double f = strconv_parse_float(S("1.5e308"), sizes[i], &err);
        CHECK(BURROW_OK(err));
        CHECK(bits64(f) == bits64(1.5e308));
    }
}

static void TestParseFloatWithoutAnError(TestingT *t) {
    CHECK(strconv_parse_float(S("2.5"), 64, NULL) == 2.5);
    CHECK(strconv_parse_float(S("x"), 64, NULL) == 0);
    CHECK(math_is_inf((double)(strconv_parse_float(S("1e400"), 64, NULL)), 0));
}

static bool appended(Slice got, Str want) {
    return got.len == want.len && memcmp(got.p, want.p, (size_t)want.len) == 0;
}

static Str with_abc(Str s) {
    Byte *p = mem_alloc(a, (size_t)s.len + 3, 1);
    memcpy(p, "abc", 3);
    memcpy(p + 3, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len + 3);
}

static void TestFormatFloat(TestingT *t) {
    for (Int i = 0; i < COUNT(ftoa_tests); i++) {
        const FtoaCase *tt = &ftoa_tests[i];
        double f = from_bits64(tt->in);
        Str got = strconv_format_float(a, f, tt->fmt, tt->prec, tt->bit_size);
        if (!str_eq(got, tt->out))
            fprintf(stderr, "FormatFloat(%016llx, '%c', %d, %d) = %.*s\n",
                    (unsigned long long)tt->in, tt->fmt, (int)tt->prec,
                    (int)tt->bit_size, (int)got.len, (const char *)got.p);
        CHECK(str_eq(got, tt->out));

        Slice dst = slice_from_str(a, S("abc"));
        CHECK(appended(strconv_append_float(a, dst, f, tt->fmt, tt->prec, tt->bit_size),
                       with_abc(tt->out)));
    }
}

static void TestAppendFloatToNothing(TestingT *t) {
    Slice s = strconv_append_float(a, (Slice){0}, 0.1, 'g', -1, 64);
    CHECK(appended(s, S("0.1")));
    CHECK(s.elem != NULL);
}

static void TestFormatFloatBitSize(TestingT *t) {
    CHECK_PANIC((void)strconv_format_float(a, 3.14, 'g', -1, 100),
                "strconv: illegal FormatFloat bitSize");
    CHECK_PANIC((void)strconv_append_float(a, (Slice){0}, 3.14, 'g', -1, 0),
                "strconv: illegal AppendFloat bitSize");
}

/* Every power of two a double or a float can hold reads back as itself. */
static void TestPowersOfTwoRoundTrip(TestingT *t) {
    for (int exp = -2048; exp <= 2048; exp++) {
        ArenaMark m = arena_mark(&ar);
        double f = ldexp(1, exp);
        if (!math_is_inf((double)f, 0)) {
            Str s = strconv_format_float(a, f, 'e', -1, 64);
            CHECK(strconv_parse_float(s, 64, NULL) == f);
        }
        float f32 = (float)f;
        if (!math_is_inf((double)f32, 0)) {
            Str s = strconv_format_float(a, (double)f32, 'e', -1, 32);
            CHECK((float)strconv_parse_float(s, 32, NULL) == f32);
        }
        arena_release(&ar, m);
    }
}

static uint64_t rng = 0x2143;

static uint64_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* The shortest form of any double reads back as exactly that double. */
static void TestRandomRoundTrip(TestingT *t) {
    for (int i = 0; i < 200000; i++) {
        ArenaMark m = arena_mark(&ar);
        double f = from_bits64(next());
        if (!math_is_nan((double)f)) {
            Str s = strconv_format_float(a, f, 'g', -1, 64);
            Error err = BURROW_NO_ERROR;
            double back = strconv_parse_float(s, 64, &err);
            CHECK(bits64(back) == bits64(f));
            CHECK(BURROW_OK(err) || math_is_inf((double)f, 0));
        }
        arena_release(&ar, m);
    }
}

/* A sample of every finite float, positive and negative. */
static void TestFloat32RoundTrip(TestingT *t) {
    for (uint32_t i = 0; i < 0xFFU << 23; i += 997) {
        ArenaMark m = arena_mark(&ar);
        float f = from_bits32(i);
        if (i & 1)
            f = -f;
        Str s = strconv_format_float(a, (double)f, 'g', -1, 32);
        Error err = BURROW_NO_ERROR;
        double back = strconv_parse_float(s, 32, &err);
        CHECK(BURROW_OK(err));
        CHECK((double)(float)back == back);
        CHECK(bits32((float)back) == bits32(f));
        arena_release(&ar, m);
    }
}

static void TestFormatFloatHugePrecision(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *ta = track_allocator(&tr);

    /* Too long for an Int, so nothing is allocated and the result is empty. */
    CHECK(strconv_format_float(ta, 1, 'f', BURROW_INT_MAX, 64).len == 0);
    CHECK(strconv_format_float(ta, 1, 'e', BURROW_INT_MAX, 64).len == 0);
    CHECK(strconv_format_float(ta, 1, 'x', BURROW_INT_MAX, 64).len == 0);
    CHECK(strconv_append_float(ta, (Slice){0}, 1, 'f', BURROW_INT_MAX, 64).len == 0);
    CHECK(track_check(&tr) == 0);

    /* Long but possible, and exactly its length. */
    Str s = strconv_format_float(ta, 0.5, 'f', 100000, 64);
    CHECK(s.len == 100002);
    CHECK(s.p[0] == '0' && s.p[1] == '.' && s.p[2] == '5' && s.p[100001] == '0');
    mem_free(ta, (void *)(Uintptr)s.p, (size_t)s.len, 1);

    s = strconv_format_float(ta, 1, 'x', 5000, 64);
    CHECK(s.len == 5000 + 8);
    mem_free(ta, (void *)(Uintptr)s.p, (size_t)s.len, 1);

    CHECK(track_check(&tr) == 0);
    track_free(&tr);
}

static void TestParseComplex(TestingT *t) {
    for (Int i = 0; i < COUNT(atoc_tests); i++) {
        const AtocCase *tt = &atoc_tests[i];
        Error err = BURROW_NO_ERROR;
        Complex128 c = strconv_parse_complex(tt->in, tt->bit_size, &err);
        if (!same64(c.re, tt->re) || !same64(c.im, tt->im))
            fprintf(stderr, "ParseComplex(%.*s, %d) = %016llx %016llx\n",
                    (int)tt->in.len, (const char *)tt->in.p, (int)tt->bit_size,
                    (unsigned long long)bits64(c.re), (unsigned long long)bits64(c.im));
        CHECK(same64(c.re, tt->re));
        CHECK(same64(c.im, tt->im));
        CHECK(num_error_is(err, "ParseComplex", tt->in, tt->err));
    }
}

/* Issue 42297 again, for ParseComplex. */
static void TestParseComplexOtherBitSizes(TestingT *t) {
    static const Int sizes[] = {0, 10, 100, 256};
    for (Int i = 0; i < COUNT(sizes); i++) {
        Error err = BURROW_NO_ERROR;
        Complex128 c = strconv_parse_complex(S("1.5e308+1.0e307i"), sizes[i], &err);
        CHECK(BURROW_OK(err));
        CHECK(bits64(c.re) == bits64(1.5e308) && bits64(c.im) == bits64(1.0e307));
    }
}

typedef struct CtoaCase {
    Complex128 c;
    Byte fmt;
    Int prec;
    Int bit_size;
    Str out;
} CtoaCase;

static const CtoaCase ctoa_tests[] = {
    /* A variety of signs. */
    {{1, 2}, 'g', -1, 128, BURROW_S_INIT("(1+2i)")},
    {{3, -4}, 'g', -1, 128, BURROW_S_INIT("(3-4i)")},
    {{-5, 6}, 'g', -1, 128, BURROW_S_INIT("(-5+6i)")},
    {{-7, -8}, 'g', -1, 128, BURROW_S_INIT("(-7-8i)")},

    /* That fmt and prec are working. */
    {{3.14159, 0.00123}, 'e', 3, 128, BURROW_S_INIT("(3.142e+00+1.230e-03i)")},
    {{3.14159, 0.00123}, 'f', 3, 128, BURROW_S_INIT("(3.142+0.001i)")},
    {{3.14159, 0.00123}, 'g', 3, 128, BURROW_S_INIT("(3.14+0.00123i)")},

    /* That the bit size rounds. */
    {{1.2345678901234567, 9.876543210987654},
     'f',
     -1,
     128,
     BURROW_S_INIT("(1.2345678901234567+9.876543210987654i)")},
    {{1.2345678901234567, 9.876543210987654},
     'f',
     -1,
     64,
     BURROW_S_INIT("(1.2345679+9.876543i)")},

    /* The imaginary part's own sign, which Inf and NaN have or lack. */
    {{0, (double)INFINITY}, 'g', -1, 128, BURROW_S_INIT("(0+Infi)")},
    {{0, -(double)INFINITY}, 'g', -1, 128, BURROW_S_INIT("(0-Infi)")},
    {{0, (double)NAN}, 'g', -1, 128, BURROW_S_INIT("(0+NaNi)")},
    {{0, -0.0}, 'g', -1, 128, BURROW_S_INIT("(0-0i)")},
};

static void TestFormatComplex(TestingT *t) {
    for (Int i = 0; i < COUNT(ctoa_tests); i++) {
        const CtoaCase *tt = &ctoa_tests[i];
        Str got = strconv_format_complex(a, tt->c, tt->fmt, tt->prec, tt->bit_size);
        CHECK(str_eq(got, tt->out));
    }
}

static void TestFormatComplexBitSize(TestingT *t) {
    CHECK_PANIC((void)strconv_format_complex(a, (Complex128){1, 2}, 'g', -1, 100),
                "invalid bitSize");
}

#define TESTS(X)                                                                       \
    X(TestParseFloat)                                                                  \
    X(TestParseLongFloat)                                                              \
    X(TestParseFloatOtherBitSizes)                                                     \
    X(TestParseFloatWithoutAnError)                                                    \
    X(TestFormatFloat)                                                                 \
    X(TestAppendFloatToNothing)                                                        \
    X(TestFormatFloatBitSize)                                                          \
    X(TestPowersOfTwoRoundTrip)                                                        \
    X(TestRandomRoundTrip)                                                             \
    X(TestFloat32RoundTrip)                                                            \
    X(TestFormatFloatHugePrecision)                                                    \
    X(TestParseComplex)                                                                \
    X(TestParseComplexOtherBitSizes)                                                   \
    X(TestFormatComplex)                                                               \
    X(TestFormatComplexBitSize)

static int TestMain(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int code = testing_m_run(m);
    arena_free(&ar);
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
