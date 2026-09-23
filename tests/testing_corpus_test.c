/* Derived from Go's src/internal/fuzz/encoding_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/type.h"

#include <string.h>

typedef struct CorpusCase {
    const char *desc;
    const char *in;
    bool reject;
    const char *want; /* when it differs from in */
} CorpusCase;

#if UINTPTR_MAX == 0xffffffffu
#define CORPUS_INT_OVERFLOW "go test fuzz v1\nint(-1)\nuint(4294967295)"
#else
#define CORPUS_INT_OVERFLOW                                                            \
    "go test fuzz v1\nint(9223372036854775807)\nuint(18446744073709551615)"
#endif

static const CorpusCase corpus_cases[] = {
    {"missing version", "int(1234)", true, NULL},
    {"malformed string", "go test fuzz v1\nstring(\"a\"bcad\")", true, NULL},
    {"empty value", "go test fuzz v1\nint()", true, NULL},
    {"negative uint", "go test fuzz v1\nuint(-32)", true, NULL},
    {"int8 too large", "go test fuzz v1\nint8(1234456)", true, NULL},
    {"multiplication in int value", "go test fuzz v1\nint(20*5)", true, NULL},
    {"double negation", "go test fuzz v1\nint(--5)", true, NULL},
    {"malformed bool", "go test fuzz v1\nbool(0)", true, NULL},
    {"malformed byte", "go test fuzz v1\nbyte('aa)", true, NULL},
    {"byte out of range", "go test fuzz v1\nbyte('\xe2\x98\x83')", true, NULL},
    {"extra newline", "go test fuzz v1\nstring(\"has extra newline\")\n", false,
     "go test fuzz v1\nstring(\"has extra newline\")"},
    {"trailing spaces",
     "go test fuzz v1\nstring(\"extra\")\n[]byte(\"spacing\")  \n    ", false,
     "go test fuzz v1\nstring(\"extra\")\n[]byte(\"spacing\")"},
    {"float types", "go test fuzz v1\nfloat64(0)\nfloat32(0)", false, NULL},
    {"various types",
     "go test fuzz v1\n"
     "int(-23)\n"
     "int8(-2)\n"
     "int64(2342425)\n"
     "uint(1)\n"
     "uint16(234)\n"
     "uint32(352342)\n"
     "uint64(123)\n"
     "rune('\xc5\x93')\n"
     "byte('K')\n"
     "byte('\xc3\xbf')\n"
     "[]byte(\"hello\xc2\xbf\")\n"
     "[]byte(\"a\")\n"
     "bool(true)\n"
     "string(\"hello\\\\xbd\\\\xb2=\\\\xbc \xe2\x8c\x98\")\n"
     "float64(-12.5)\n"
     "float32(2.5)",
     false, NULL},
    {"float edge cases",
     "go test fuzz v1\n"
     "float32(-0)\n"
     "float64(-0)\n"
     "float32(+Inf)\n"
     "float32(-Inf)\n"
     "float32(NaN)\n"
     "float64(+Inf)\n"
     "float64(-Inf)\n"
     "float64(NaN)\n"
     "math.Float64frombits(0x7ff8000000000002)\n"
     "math.Float32frombits(0x7fc00001)",
     false, NULL},
    {"int variations",
     "go test fuzz v1\n"
     "int(0x0)\n"
     "int32(0x41)\n"
     "int64(0xfffffffff)\n"
     "uint32(0xcafef00d)\n"
     "uint64(0xffffffffffffffff)\n"
     "uint8(0b0000000)\n"
     "byte(0x0)\n"
     "byte('\\000')\n"
     "byte('\\u0000')\n"
     "byte('\\'')\n"
     "math.Float64frombits(9221120237041090562)\n"
     "math.Float32frombits(2143289345)",
     false,
     "go test fuzz v1\n"
     "int(0)\n"
     "rune('A')\n"
     "int64(68719476735)\n"
     "uint32(3405705229)\n"
     "uint64(18446744073709551615)\n"
     "byte('\\x00')\n"
     "byte('\\x00')\n"
     "byte('\\x00')\n"
     "byte('\\x00')\n"
     "byte('\\'')\n"
     "math.Float64frombits(0x7ff8000000000002)\n"
     "math.Float32frombits(0x7fc00001)"},
    {"rune validation",
     "go test fuzz v1\n"
     "rune(0)\n"
     "rune(0x41)\n"
     "rune(-1)\n"
     "rune(0xfffd)\n"
     "rune(0xd800)\n"
     "rune(0x10ffff)\n"
     "rune(0x110000)\n",
     false,
     "go test fuzz v1\n"
     "rune('\\x00')\n"
     "rune('A')\n"
     "int32(-1)\n"
     "rune('\xef\xbf\xbd')\n"
     "int32(55296)\n"
     "rune('\\U0010ffff')\n"
     "int32(1114112)"},
    {"int overflow",
     "go test fuzz v1\nint(0x7fffffffffffffff)\nuint(0xffffffffffffffff)", false,
     CORPUS_INT_OVERFLOW},
    {"windows new line", "go test fuzz v1\r\nint(0)\r\n", false,
     "go test fuzz v1\nint(0)"},
};

/* The text these make comes from an arena, and the values from the heap,
 * which is where unmarshal puts them. */
static void corpus_case_run(void *env, TestingT *t) {
    const CorpusCase *c = (const CorpusCase *)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Any *vals;
    Int n;
    Str err;
    bool ok =
        burrow__testing_corpus_unmarshal(a, str_from_cstr(c->in), &vals, &n, &err);
    if (c->reject) {
        if (ok)
            burrow__testing_values_free(vals, n);
        arena_free(&ar);
        if (ok)
            testing_t_fatalf_v(t, "unmarshal unexpected success");
        return;
    }
    if (!ok) {
        testing_t_errorf_v(t, "unmarshal unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    Str got = burrow__testing_corpus_marshal(a, vals, n);
    burrow__testing_values_free(vals, n);
    if (got.len == 0 || got.p[got.len - 1] != '\n')
        testing_t_error_v(t, "didn't write final newline to corpus file");
    Str want = fmt_sprintf_v(a, "%s\n", c->want != NULL ? c->want : c->in);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "unexpected marshaled value\ngot:\n%s\nwant:\n%s", got,
                           want);
    arena_free(&ar);
}

static void TestUnmarshalMarshal(TestingT *t) {
    for (size_t i = 0; i < sizeof corpus_cases / sizeof corpus_cases[0]; i++)
        testing_t_run(t, str_from_cstr(corpus_cases[i].desc),
                      BURROW_FN(TestingTFunc, corpus_case_run,
                                (void *)(uintptr_t)&corpus_cases[i]));
}

/* Marshals one value and reads it back. Answers with what came back, one
 * value of type want, which burrow__testing_values_free gives back, or fails
 * t and answers NULL. */
static Any *round_trip(TestingT *t, Any in, const Type *want) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str b = burrow__testing_corpus_marshal(a, &in, 1);
    testing_t_logf_v(t, "marshaled:\n%s", b);
    Any *vals;
    Int n;
    Str err;
    bool ok = burrow__testing_corpus_unmarshal(a, b, &vals, &n, &err);
    if (!ok)
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
    if (!ok)
        return NULL;
    if (n == 1 && vals[0].t == want)
        return vals;
    if (n != 1)
        testing_t_errorf_v(t, "unmarshaled %d values", n);
    else
        testing_t_error_v(t, "unmarshaled the wrong type");
    burrow__testing_values_free(vals, n);
    return NULL;
}

static void TestByteRoundTrip(TestingT *t) {
    for (int x = 0; x < 256; x++) {
        Byte b1 = (Byte)x;
        Any *vs = round_trip(t, BURROW_ANY(TYPE_UINT8, &b1), TYPE_UINT8);
        if (vs == NULL)
            return;
        Byte b2 = *(Byte *)vs[0].data;
        burrow__testing_values_free(vs, 1);
        if (b2 != b1)
            testing_t_fatalf_v(t, "unmarshaled %v, want %v", b2, b1);
    }
}

static void TestInt8RoundTrip(TestingT *t) {
    for (int x = -128; x < 128; x++) {
        int8_t i1 = (int8_t)x;
        Any *vs = round_trip(t, BURROW_ANY(TYPE_INT8, &i1), TYPE_INT8);
        if (vs == NULL)
            return;
        int8_t i2 = *(int8_t *)vs[0].data;
        burrow__testing_values_free(vs, 1);
        if (i2 != i1)
            testing_t_fatalf_v(t, "unmarshaled %v, want %v", i2, i1);
    }
}

static void float64_round_trip(void *env, TestingT *t, Slice args) {
    (void)env;
    uint64_t u1 = testing_fuzz_arg(args, 0, uint64_t);
    double x1;
    memcpy(&x1, &u1, sizeof x1);
    Any *vs = round_trip(t, BURROW_ANY(TYPE_FLOAT64, &x1), TYPE_FLOAT64);
    if (vs == NULL)
        return;
    double x2 = *(double *)vs[0].data;
    burrow__testing_values_free(vs, 1);
    uint64_t u2;
    memcpy(&u2, &x2, sizeof u2);
    if (u2 != u1)
        testing_t_errorf_v(t, "unmarshaled %v (bits 0x%x)", x2, u2);
}

static void FuzzFloat64RoundTrip(TestingF *f) {
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0));
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0x8000000000000000u)); /* -0 */
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0x7fefffffffffffffu)); /* MaxFloat64 */
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 1)); /* SmallestNonzeroFloat64 */
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0x7ff8000000000001u)); /* NaN() */
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t,
                                      0x7FF0000000000001u)); /* signaling NaN */
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0x7ff0000000000000u)); /* Inf(1) */
    testing_f_add_v(
        f, BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0xfff0000000000000u)); /* Inf(-1) */
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, float64_round_trip, NULL),
                     TYPE_UINT64);
}

static void rune_round_trip(void *env, TestingT *t, Slice args) {
    (void)env;
    Rune r1 = testing_fuzz_arg(args, 0, Rune);
    Any *vs = round_trip(t, BURROW_ANY(TYPE_INT32, &r1), TYPE_INT32);
    if (vs == NULL)
        return;
    Rune r2 = *(Rune *)vs[0].data;
    burrow__testing_values_free(vs, 1);
    if (r2 != r1)
        testing_t_errorf_v(t, "unmarshaled rune(0x%x)", r2);
}

static void FuzzRuneRoundTrip(TestingF *f) {
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, -1));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0xd800));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0xdfff));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0xfffd));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0x7f));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0xff));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0x10ffff));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0x110000));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, INT32_MIN));
    testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT32, int32_t, 0x7fffffff));
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, rune_round_trip, NULL), TYPE_INT32);
}

static void string_round_trip(void *env, TestingT *t, Slice args) {
    (void)env;
    Str s1 = testing_fuzz_arg(args, 0, Str);
    Any *vs = round_trip(t, BURROW_ANY(TYPE_STRING, &s1), TYPE_STRING);
    if (vs == NULL)
        return;
    Str s2 = *(Str *)vs[0].data;
    if (!str_eq(s2, s1))
        testing_t_errorf_v(t, "unmarshaled %q", s2);
    burrow__testing_values_free(vs, 1);
}

static void FuzzStringRoundTrip(TestingF *f) {
    testing_f_add_v(f, "");
    testing_f_add_v(f, BURROW_S("\x00"));
    testing_f_add_v(f, "\xef\xbf\xbd");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, string_round_trip, NULL),
                     TYPE_STRING);
}

#define TESTS(X)                                                                       \
    X(TestUnmarshalMarshal)                                                            \
    X(TestByteRoundTrip)                                                               \
    X(TestInt8RoundTrip)                                                               \
    X(FuzzFloat64RoundTrip)                                                            \
    X(FuzzRuneRoundTrip)                                                               \
    X(FuzzStringRoundTrip)
TESTING_MAIN(TESTS)
