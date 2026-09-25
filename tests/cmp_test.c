/* Derived from Go's src/cmp/cmp_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/cmp.h"

#include <math.h>

/* Go's tests table, split by type, since each row is compared as the type of
 * its x. */
typedef struct IntCase {
    Int x, y;
    int compare;
} IntCase;

typedef struct StrCase {
    const char *x, *y;
    int compare;
} StrCase;

typedef struct FloatCase {
    double x, y;
    int compare;
} FloatCase;

typedef struct PtrCase {
    Uintptr x, y;
    int compare;
} PtrCase;

static const IntCase int_cases[] = {{1, 2, -1}, {1, 1, 0}, {2, 1, +1}};

static const StrCase str_cases[] = {{"a", "aa", -1}, {"a", "a", 0}, {"aa", "a", +1}};

static FloatCase float_cases[21];
static PtrCase ptr_cases[3];

static void setup(void) {
    double negzero = copysign(0, -1);
    double inf = (double)INFINITY;
    double nan = (double)NAN;
    FloatCase f[] = {
        {1.0, 1.1, -1},    {1.1, 1.1, 0},     {1.1, 1.0, +1},     {inf, inf, 0},
        {-inf, -inf, 0},   {-inf, 1.0, -1},   {1.0, -inf, +1},    {inf, 1.0, +1},
        {1.0, inf, -1},    {nan, nan, 0},     {0.0, nan, +1},     {nan, 0.0, -1},
        {nan, -inf, -1},   {-inf, nan, +1},   {0.0, 0.0, 0},      {negzero, negzero, 0},
        {negzero, 0.0, 0}, {0.0, negzero, 0}, {negzero, 1.0, -1}, {negzero, -1.0, +1},
        {1.0, 1.0, 0},
    };
    memcpy(float_cases, f, sizeof f);
    static double target;
    Uintptr nonnil = (Uintptr)&target;
    PtrCase p[] = {{0, nonnil, -1}, {nonnil, 0, 1}, {nonnil, nonnil, 0}};
    memcpy(ptr_cases, p, sizeof p);
}

#define NCASES(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static void TestLess(TestingT *t) {
    setup();
    for (Int i = 0; i < NCASES(int_cases); i++) {
        const IntCase *c = &int_cases[i];
        if (cmp_less(c->x, c->y) != (c->compare < 0))
            testing_t_errorf_v(t, "Less(%d, %d) wrong", c->x, c->y);
    }
    for (Int i = 0; i < NCASES(str_cases); i++) {
        const StrCase *c = &str_cases[i];
        if (cmp_less(str_from_cstr(c->x), str_from_cstr(c->y)) != (c->compare < 0))
            testing_t_errorf_v(t, "Less(%q, %q) wrong", c->x, c->y);
    }
    for (Int i = 0; i < NCASES(float_cases); i++) {
        const FloatCase *c = &float_cases[i];
        if (cmp_less(c->x, c->y) != (c->compare < 0))
            testing_t_errorf_v(t, "Less(%v, %v) wrong", c->x, c->y);
    }
    for (Int i = 0; i < NCASES(ptr_cases); i++) {
        const PtrCase *c = &ptr_cases[i];
        if (cmp_less(c->x, c->y) != (c->compare < 0))
            testing_t_errorf_v(t, "Less(ptr case %d) wrong", i);
    }
}

static void TestCompare(TestingT *t) {
    setup();
    for (Int i = 0; i < NCASES(int_cases); i++) {
        const IntCase *c = &int_cases[i];
        CHECK_INT_EQ(cmp_compare(c->x, c->y), c->compare);
    }
    for (Int i = 0; i < NCASES(str_cases); i++) {
        const StrCase *c = &str_cases[i];
        CHECK_INT_EQ(cmp_compare(str_from_cstr(c->x), str_from_cstr(c->y)), c->compare);
    }
    for (Int i = 0; i < NCASES(float_cases); i++) {
        const FloatCase *c = &float_cases[i];
        CHECK_INT_EQ(cmp_compare(c->x, c->y), c->compare);
        /* The same row as float32, which has its own function. */
        CHECK_INT_EQ(cmp_compare((float)c->x, (float)c->y), c->compare);
    }
    for (Int i = 0; i < NCASES(ptr_cases); i++) {
        const PtrCase *c = &ptr_cases[i];
        CHECK_INT_EQ(cmp_compare(c->x, c->y), c->compare);
    }
}

/* Every width goes to its own function and agrees on the answer. */
static void TestCompareWidths(TestingT *t) {
    CHECK_INT_EQ(cmp_compare((int8_t)-1, (int8_t)1), -1);
    CHECK_INT_EQ(cmp_compare((int16_t)-1, (int16_t)1), -1);
    CHECK_INT_EQ(cmp_compare((int32_t)-1, (int32_t)1), -1);
    CHECK_INT_EQ(cmp_compare((int64_t)-1, (int64_t)1), -1);
    CHECK_INT_EQ(cmp_compare((uint8_t)255, (uint8_t)1), +1);
    CHECK_INT_EQ(cmp_compare((uint16_t)65535, (uint16_t)1), +1);
    CHECK_INT_EQ(cmp_compare((uint32_t)4294967295U, (uint32_t)1), +1);
    CHECK_INT_EQ(cmp_compare((uint64_t)UINT64_MAX, (uint64_t)1), +1);
    CHECK_INT_EQ(cmp_compare((long)-1, (long)1), -1);
    CHECK_INT_EQ(cmp_compare((unsigned long)2, (unsigned long)1), +1);
    CHECK_INT_EQ(cmp_compare((long long)-1, (long long)1), -1);
    CHECK_INT_EQ(cmp_compare((unsigned long long)1, (unsigned long long)1), 0);
    CHECK(cmp_less((Uint)1, (Uint)2));
    CHECK(!cmp_less(2.0F, 1.0F));
    CHECK(cmp_less((float)NAN, -INFINITY));
}

/* Go's TestSort: Compare and Less agree with sort.Float64s. */
static void TestSort(TestingT *t) {
    double input[] = {
        1.0, 0.0, copysign(0, -1), (double)INFINITY, -(double)INFINITY, (double)NAN};
    sort_float64s(slice_from(input, 6, 6, TYPE_FLOAT64));
    for (Int i = 0; i < 5; i++) {
        if (cmp_less(input[i + 1], input[i]))
            testing_t_errorf_v(t, "Less sort mismatch at %d", i);
        if (cmp_compare(input[i], input[i + 1]) > 0)
            testing_t_errorf_v(t, "Compare sort mismatch at %d", i);
    }
}

static void TestOr(TestingT *t) {
    CHECK_INT_EQ(cmp_or((Int)0), 0);
    CHECK_INT_EQ(cmp_or((Int)1), 1);
    CHECK_INT_EQ(cmp_or((Int)0, (Int)2), 2);
    CHECK_INT_EQ(cmp_or((Int)3, (Int)0), 3);
    CHECK_INT_EQ(cmp_or((Int)4, (Int)5), 4);
    CHECK_INT_EQ(cmp_or((Int)0, (Int)6, (Int)7), 6);
    CHECK_INT_EQ(cmp_or(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16), 16);
    bool no = false, yes = true;
    CHECK(cmp_or(no, yes));
    CHECK(!cmp_or(no, no));
    /* -0.0 is the zero value and a NaN is not. */
    CHECK(cmp_or(copysign(0, -1), 2.0) == 2.0);
    double n = cmp_or(0.0, (double)NAN, 1.0);
    CHECK(n != n);
}

/* ExampleOr. */
static void TestExampleOr(TestingT *t) {
    Str user_input1 = BURROW_S("");
    Str user_input2 = BURROW_S("some text");
    CHECK(str_eq(cmp_or(user_input1, BURROW_S("default")), BURROW_S("default")));
    CHECK(str_eq(cmp_or(user_input2, BURROW_S("default")), BURROW_S("some text")));
    CHECK(str_eq(cmp_or(user_input1, user_input2, BURROW_S("default")),
                 BURROW_S("some text")));
}

/* ExampleLess and ExampleCompare. */
static void TestExampleLessCompare(TestingT *t) {
    CHECK(cmp_less(1, 2));
    CHECK(cmp_less(BURROW_S("a"), BURROW_S("aa")));
    CHECK(!cmp_less(1.0, (double)NAN));
    CHECK(cmp_less((double)NAN, 1.0));
    CHECK_INT_EQ(cmp_compare(1, 2), -1);
    CHECK_INT_EQ(cmp_compare(BURROW_S("a"), BURROW_S("aa")), -1);
    CHECK_INT_EQ(cmp_compare(1.5, 1.5), 0);
    CHECK_INT_EQ(cmp_compare((double)NAN, 1.0), -1);
}

#define TESTS(X)                                                                       \
    X(TestLess)                                                                        \
    X(TestCompare)                                                                     \
    X(TestCompareWidths)                                                               \
    X(TestSort)                                                                        \
    X(TestOr)                                                                          \
    X(TestExampleOr)                                                                   \
    X(TestExampleLessCompare)

TESTING_MAIN(TESTS)
