/* Tests for database/sql/driver.
 *
 * Go's types_test.go, the converter table entry for entry, then what C adds:
 * the Int32 and String converters, Null and NotNull, IsValue, a Valuer called
 * through its method, a nil pointer to one, and the RowsAffected and
 * ResultNoRows results, none of which Go's package tests on its own.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/database/sql/driver.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/time.h"
#include "check.h"

#include <string.h>

/* Go's i, f, b, bs, s, t and is: named types in package driver, which is what
 * %T prints them as. */
#define DT_NAMED(var, gonm, kind, size, align, elem)                                   \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"database/sql/driver", 19},                                     \
        (kind),                                                                        \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        (elem),                                                                        \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

DT_NAMED(dt_i, "i", KIND_INT64, sizeof(int64_t), _Alignof(int64_t), NULL);
DT_NAMED(dt_f, "f", KIND_FLOAT64, sizeof(double), _Alignof(double), NULL);
DT_NAMED(dt_b, "b", KIND_BOOL, sizeof(bool), _Alignof(bool), NULL);
DT_NAMED(dt_bs, "bs", KIND_SLICE, sizeof(Slice), _Alignof(Slice), TYPE_BYTE);
DT_NAMED(dt_s, "s", KIND_STRING, sizeof(Str), _Alignof(Str), NULL);
DT_NAMED(dt_t, "t", KIND_STRUCT, sizeof(Time), _Alignof(Time), NULL);
DT_NAMED(dt_is, "is", KIND_SLICE, sizeof(Slice), _Alignof(Slice), TYPE_INT);

BURROW_PTR_TYPE(DtInt64Ptr, int64_t);
BURROW_PTR_TYPE(DtTimePtr, Time);

/* Go's dec, a decimal by way of its Decompose method. */
typedef struct DtDec {
    Byte form;
    bool neg;
    Byte coefficient[16];
    int32_t exponent;
} DtDec;
extern const Type burrow_type_DtDec;

static DriverDecomposed dt_dec_decompose(DtDec *d, Bytes buf) {
    (void)buf;
    DriverDecomposed out = {d->form, d->neg,
                            slice_from(d->coefficient, 16, 16, TYPE_BYTE), d->exponent};
    return out;
}

#define DT_DEC_FIELDS(F, T)                                                            \
    F(T, Byte, form, "")                                                               \
    F(T, bool, neg, "")                                                                \
    F(T, int32_t, exponent, "")
#define DT_DEC_METHODS(M, T) M(T, Decompose, dt_dec_decompose, DRIVER_SIG_DECOMPOSE)
BURROW_STRUCT_DEFINE_METHODS(DtDec, DT_DEC_FIELDS, DT_DEC_METHODS);

/* A Valuer: Value gives n as an int64, or fails when n is negative. */
typedef struct DtValuer {
    int64_t n;
} DtValuer;
extern const Type burrow_type_DtValuer;

static Any dt_valuer_value(DtValuer *v, DriverAllocArg a, DriverErrorArg err) {
    if (v->n < 0) {
        BURROW_OUT(err, fmt_errorf_v("negative %d", v->n));
        return (Any){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    int64_t *p = BURROW_NEW(a, int64_t);
    if (p == NULL)
        return (Any){NULL, NULL};
    *p = v->n;
    return BURROW_ANY(TYPE_INT64, p);
}

#define DT_VALUER_FIELDS(F, T) F(T, int64_t, n, "")
#define DT_VALUER_METHODS(M, T) M(T, Value, dt_valuer_value, DRIVER_SIG_VALUE)
BURROW_STRUCT_DEFINE_METHODS(DtValuer, DT_VALUER_FIELDS, DT_VALUER_METHODS);

BURROW_PTR_TYPE(DtValuerPtr, DtValuer);

/* reflect.DeepEqual for the values a converter gives back. */
static bool dt_equal(Any got, Any want) {
    if (got.t != want.t)
        return false;
    if (got.t == NULL)
        return true;
    if (got.data == NULL || want.data == NULL)
        return got.data == want.data;
    if (got.t == TYPE_TIME)
        return time_equal(*(const Time *)got.data, *(const Time *)want.data);
    if (got.t == TYPE_STRING)
        return str_eq(*(const Str *)got.data, *(const Str *)want.data);
    if (got.t == TYPE_BYTES) {
        const Slice *g = got.data, *w = want.data;
        return g->len == w->len &&
               (g->len == 0 || memcmp(g->p, w->p, (size_t)g->len) == 0);
    }
    return memcmp(got.data, want.data, got.t->size) == 0;
}

typedef struct DtConvTest {
    const DriverValueConverter *c;
    Any in;
    Any out;
    const char *err;
} DtConvTest;

static void dt_run(TestingT *t, Alloc *a, const DtConvTest *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        const DtConvTest *tt = &tests[i];
        Error err = BURROW_NO_ERROR;
        DriverValue out = tt->c->vt->convert_value(tt->c->data, a, tt->in, &err);
        Str goterr = BURROW_FAILED(err) ? error_text(err) : BURROW_STR_EMPTY;
        Str wanterr = str_from_cstr(tt->err);
        if (!str_eq(goterr, wanterr))
            testing_t_errorf_v(t, "test %d: %T(%T(%v)) error = %q; want error = %q", i,
                               BURROW_ANY(tt->c->vt->self_type, NULL), tt->in, tt->in,
                               goterr, wanterr);
        if (wanterr.len > 0)
            continue;
        if (!dt_equal(out, tt->out))
            testing_t_errorf_v(t, "test %d: %T(%T(%v)) = %v (%T); want %v (%T)", i,
                               BURROW_ANY(tt->c->vt->self_type, NULL), tt->in, tt->in,
                               out, out, tt->out, tt->out);
    }
}

static void TestValueConverters(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Time now = time_now();
    int64_t answer = 42;
    int64_t *nil_int64 = NULL;
    int64_t *answer_ptr = &answer;
    Time *now_ptr = &now;
    Byte one = 1;
    Byte t_byte = 't';
    Int is_one = 1;
    DtDec dec = {0, false, {0}, -6};
    bool yes = true, no = false;
    int64_t i9 = 9, i64_1 = 1, i64_0 = 0;
    Int int1 = 1, int0 = 0, int2 = 2;
    uint16_t u16_1 = 1, u16_0 = 0;
    double f01 = 0.1;
    Str s_true = BURROW_S("true"), s_True = BURROW_S("True"), s_1 = BURROW_S("1");
    Str s_false = BURROW_S("false"), s_0 = BURROW_S("0"), s_foo = BURROW_S("foo");
    Str s_a = BURROW_S("a");
    Slice b_t = slice_from(&t_byte, 1, 1, TYPE_BYTE);
    Slice b_1 = slice_from(&one, 1, 1, TYPE_BYTE);
    Slice is_1 = slice_from(&is_one, 1, 1, TYPE_INT);

    const DtConvTest tests[] = {
        {&driver_bool, {TYPE_STRING, &s_true}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_STRING, &s_True}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_BYTES, &b_t}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_BOOL, &yes}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_STRING, &s_1}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_INT, &int1}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_INT64, &i64_1}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_UINT16, &u16_1}, {TYPE_BOOL, &yes}, ""},
        {&driver_bool, {TYPE_STRING, &s_false}, {TYPE_BOOL, &no}, ""},
        {&driver_bool, {TYPE_BOOL, &no}, {TYPE_BOOL, &no}, ""},
        {&driver_bool, {TYPE_STRING, &s_0}, {TYPE_BOOL, &no}, ""},
        {&driver_bool, {TYPE_INT, &int0}, {TYPE_BOOL, &no}, ""},
        {&driver_bool, {TYPE_INT64, &i64_0}, {TYPE_BOOL, &no}, ""},
        {&driver_bool, {TYPE_UINT16, &u16_0}, {TYPE_BOOL, &no}, ""},
        {&driver_bool,
         {TYPE_STRING, &s_foo},
         {NULL, NULL},
         "sql/driver: couldn't convert \"foo\" into type bool"},
        {&driver_bool,
         {TYPE_INT, &int2},
         {NULL, NULL},
         "sql/driver: couldn't convert 2 into type bool"},
        {&driver_default_parameter_converter, {TYPE_TIME, &now}, {TYPE_TIME, &now}, ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtInt64Ptr), &nil_int64},
         {NULL, NULL},
         ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtInt64Ptr), &answer_ptr},
         {TYPE_INT64, &answer},
         ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtTimePtr), &now_ptr},
         {TYPE_TIME, &now},
         ""},
        {&driver_default_parameter_converter, {&dt_i, &i9}, {TYPE_INT64, &i9}, ""},
        {&driver_default_parameter_converter, {&dt_f, &f01}, {TYPE_FLOAT64, &f01}, ""},
        {&driver_default_parameter_converter, {&dt_b, &yes}, {TYPE_BOOL, &yes}, ""},
        {&driver_default_parameter_converter, {&dt_bs, &b_1}, {TYPE_BYTES, &b_1}, ""},
        {&driver_default_parameter_converter, {&dt_s, &s_a}, {TYPE_STRING, &s_a}, ""},
        {&driver_default_parameter_converter,
         {&dt_t, &now},
         {NULL, NULL},
         "unsupported type driver.t, a struct"},
        {&driver_default_parameter_converter,
         {&dt_is, &is_1},
         {NULL, NULL},
         "unsupported type driver.is, a slice of int"},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtDec), &dec},
         {TYPE_OF(DtDec), &dec},
         ""},
    };
    dt_run(t, a, tests, (Int)(sizeof tests / sizeof tests[0]));
    arena_free(&ar);
}

/* What C adds: Int32 and String, which Go's table leaves out. */
static void TestInt32AndString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    int64_t big = (int64_t)INT32_MAX + 1, small = (int64_t)INT32_MIN - 1;
    int64_t i7 = 7, neg = -12;
    int32_t i32 = -5;
    int64_t i32_as64 = -5;
    uint32_t ubig = 3000000000U;
    Str s12 = BURROW_S("-12"), sx = BURROW_S("x12"), s7 = BURROW_S("7");
    double f = 1.5;
    Str f_text = BURROW_S("1.5");
    Byte raw[] = {'h', 'i'};
    Slice b = slice_from(raw, 2, 2, TYPE_BYTE);

    const DtConvTest tests[] = {
        {&driver_int32, {TYPE_INT64, &i7}, {TYPE_INT64, &i7}, ""},
        {&driver_int32, {TYPE_INT32, &i32}, {TYPE_INT64, &i32_as64}, ""},
        {&driver_int32, {TYPE_STRING, &s12}, {TYPE_INT64, &neg}, ""},
        {&driver_int32,
         {TYPE_INT64, &big},
         {NULL, NULL},
         "sql/driver: value 2147483648 overflows int32"},
        {&driver_int32,
         {TYPE_INT64, &small},
         {NULL, NULL},
         "sql/driver: value -2147483649 overflows int32"},
        {&driver_int32,
         {TYPE_UINT32, &ubig},
         {NULL, NULL},
         "sql/driver: value 3000000000 overflows int32"},
        {&driver_int32,
         {TYPE_STRING, &sx},
         {NULL, NULL},
         "sql/driver: value \"x12\" can't be converted to int32"},
        {&driver_int32,
         {TYPE_FLOAT64, &f},
         {NULL, NULL},
         "sql/driver: unsupported value 1.5 (type float64) converting to int32"},
        {&driver_string, {TYPE_STRING, &s7}, {TYPE_STRING, &s7}, ""},
        {&driver_string, {TYPE_BYTES, &b}, {TYPE_BYTES, &b}, ""},
        {&driver_string, {TYPE_FLOAT64, &f}, {TYPE_STRING, &f_text}, ""},
    };
    dt_run(t, a, tests, (Int)(sizeof tests / sizeof tests[0]));
    arena_free(&ar);
}

static void TestNullNotNull(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    DriverNull n = {driver_bool};
    DriverNotNull nn = {driver_bool};
    DriverValueConverter nc = driver_null_as_value_converter(&n);
    DriverValueConverter nnc = driver_not_null_as_value_converter(&nn);
    Str s_true = BURROW_S("true");
    bool yes = true;

    const DtConvTest tests[] = {
        {&nc, {NULL, NULL}, {NULL, NULL}, ""},
        {&nc, {TYPE_STRING, &s_true}, {TYPE_BOOL, &yes}, ""},
        {&nnc, {NULL, NULL}, {NULL, NULL}, "nil value not allowed"},
        {&nnc, {TYPE_STRING, &s_true}, {TYPE_BOOL, &yes}, ""},
    };
    dt_run(t, a, tests, (Int)(sizeof tests / sizeof tests[0]));
    arena_free(&ar);
}

static void TestValuer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    DtValuer v = {33}, bad = {-1};
    DtValuer *vp = &v, *nilp = NULL;
    int64_t want = 33;
    uint64_t high = (uint64_t)1 << 63;

    const DtConvTest tests[] = {
        {&driver_default_parameter_converter,
         {TYPE_OF(DtValuer), &v},
         {TYPE_INT64, &want},
         ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtValuerPtr), &vp},
         {TYPE_INT64, &want},
         ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtValuerPtr), &nilp},
         {NULL, NULL},
         ""},
        {&driver_default_parameter_converter,
         {TYPE_OF(DtValuer), &bad},
         {NULL, NULL},
         "negative -1"},
        {&driver_default_parameter_converter,
         {TYPE_UINT64, &high},
         {NULL, NULL},
         "uint64 values with high bit set are not supported"},
    };
    dt_run(t, a, tests, (Int)(sizeof tests / sizeof tests[0]));
    arena_free(&ar);
}

static void TestIsValue(TestingT *t) {
    int64_t n = 1;
    Int plain = 1;
    DtDec dec = {0, false, {0}, 0};
    Time now = time_now();
    CHECK(driver_is_value((Any){NULL, NULL}));
    CHECK(driver_is_value(BURROW_ANY(TYPE_INT64, &n)));
    CHECK(driver_is_value(BURROW_ANY(TYPE_TIME, &now)));
    CHECK(driver_is_value(BURROW_ANY(TYPE_OF(DtDec), &dec)));
    CHECK(!driver_is_value(BURROW_ANY(TYPE_INT, &plain)));
    CHECK(!driver_is_value(BURROW_ANY(&dt_i, &n)));
    CHECK(driver_is_scan_value(BURROW_ANY(TYPE_INT64, &n)));
}

static void TestResults(TestingT *t) {
    DriverRowsAffected ra = 3;
    DriverResult r = driver_rows_affected_as_result(&ra);
    Error err = BURROW_NO_ERROR;
    CHECK_INT_EQ(r.vt->rows_affected(r.data, &err), 3);
    CHECK(BURROW_OK(err));
    (void)r.vt->last_insert_id(r.data, &err);
    CHECK(BURROW_FAILED(err));
    CHECK(str_eq(error_text(err),
                 BURROW_S("LastInsertId is not supported by this driver")));

    r = driver_result_no_rows;
    (void)r.vt->last_insert_id(r.data, &err);
    CHECK(str_eq(error_text(err),
                 BURROW_S("no LastInsertId available after DDL statement")));
    (void)r.vt->rows_affected(r.data, &err);
    CHECK(str_eq(error_text(err),
                 BURROW_S("no RowsAffected available after DDL statement")));
}

#define TESTS(X)                                                                       \
    X(TestValueConverters)                                                             \
    X(TestInt32AndString)                                                              \
    X(TestNullNotNull)                                                                 \
    X(TestValuer)                                                                      \
    X(TestIsValue)                                                                     \
    X(TestResults)
TESTING_MAIN(TESTS)
