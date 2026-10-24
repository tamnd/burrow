/* Derived from Go's src/testing/quick/quick_test.go.
 * Go source: go1.27.1.
 *
 * Every function quick calls needs a descriptor, so each of Go's little
 * functions here is a BURROW_FUNC with a signature list, and each type it takes
 * is declared with burrow/declare.h. The named array, map and pointer types
 * and struct{} have no macro there, so their descriptors are written out.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/quick.h"

#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include <stdint.h>
#include <string.h>

/* A named type that declare.h has no macro for. */
#define NAMED_DESC(Name, kind, elem, key, len)                                         \
    const Type burrow_type_##Name = {                                                  \
        BURROW_S_INIT(#Name),                                                          \
        {NULL, 0},                                                                     \
        kind,                                                                          \
        (uint32_t)sizeof(Name),                                                        \
        (uint16_t)_Alignof(Name),                                                      \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        (uint32_t)(len),                                                               \
        0,                                                                             \
        NULL,                                                                          \
    }

BURROW_ARRAY_TYPE(ByteArray4, uint8_t, 4);
BURROW_ARRAY_TYPE_DECL(TestArrayAlias, uint8_t, 4);
NAMED_DESC(TestArrayAlias, KIND_ARRAY, TYPE_OF(uint8_t), NULL, 4);
BURROW_NAMED_TYPE(TestBoolAlias, bool, KIND_BOOL);
BURROW_NAMED_TYPE(TestFloat32Alias, float, KIND_FLOAT32);
BURROW_NAMED_TYPE(TestFloat64Alias, double, KIND_FLOAT64);
BURROW_NAMED_TYPE(TestComplex64Alias, Complex64, KIND_COMPLEX64);
BURROW_NAMED_TYPE(TestComplex128Alias, Complex128, KIND_COMPLEX128);
BURROW_NAMED_TYPE(TestInt16Alias, int16_t, KIND_INT16);
BURROW_NAMED_TYPE(TestInt32Alias, int32_t, KIND_INT32);
BURROW_NAMED_TYPE(TestInt64Alias, int64_t, KIND_INT64);
BURROW_NAMED_TYPE(TestInt8Alias, int8_t, KIND_INT8);
BURROW_NAMED_TYPE(TestIntAlias, Int, KIND_INT);
BURROW_MAP_TYPE(IntIntMap, Int, Int);
BURROW_MAP_TYPE_DECL(TestMapAlias, Int, Int);
NAMED_DESC(TestMapAlias, KIND_MAP, TYPE_OF(Int), TYPE_OF(Int), 0);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_PTR_TYPE_DECL(TestPtrAlias, Int);
NAMED_DESC(TestPtrAlias, KIND_POINTER, TYPE_OF(Int), NULL, 0);
BURROW_SLICE_TYPE(ByteSlice, uint8_t);
BURROW_NAMED_SLICE_TYPE(TestSliceAlias, uint8_t);
BURROW_NAMED_TYPE(TestStringAlias, Str, KIND_STRING);

#define TEST_STRUCT_FIELDS(F, T)                                                       \
    F(T, Int, A, "")                                                                   \
    F(T, Str, B, "")
BURROW_STRUCT(TestStruct, TEST_STRUCT_FIELDS);
BURROW_STRUCT(TestStructAlias, TEST_STRUCT_FIELDS);

BURROW_NAMED_TYPE(TestUint16Alias, uint16_t, KIND_UINT16);
BURROW_NAMED_TYPE(TestUint32Alias, uint32_t, KIND_UINT32);
BURROW_NAMED_TYPE(TestUint64Alias, uint64_t, KIND_UINT64);
BURROW_NAMED_TYPE(TestUint8Alias, uint8_t, KIND_UINT8);
BURROW_NAMED_TYPE(TestUintAlias, Uint, KIND_UINT);
BURROW_NAMED_TYPE(TestUintptrAlias, Uintptr, KIND_UINTPTR);

/* func(T) T, for each T. */
#define ID_SIG_ByteArray4(IN, OUT) IN(0, ByteArray4) OUT(ByteArray4)
#define ID_SIG_TestArrayAlias(IN, OUT) IN(0, TestArrayAlias) OUT(TestArrayAlias)
#define ID_SIG_bool(IN, OUT) IN(0, bool) OUT(bool)
#define ID_SIG_TestBoolAlias(IN, OUT) IN(0, TestBoolAlias) OUT(TestBoolAlias)
#define ID_SIG_float(IN, OUT) IN(0, float) OUT(float)
#define ID_SIG_TestFloat32Alias(IN, OUT) IN(0, TestFloat32Alias) OUT(TestFloat32Alias)
#define ID_SIG_double(IN, OUT) IN(0, double) OUT(double)
#define ID_SIG_TestFloat64Alias(IN, OUT) IN(0, TestFloat64Alias) OUT(TestFloat64Alias)
#define ID_SIG_Complex64(IN, OUT) IN(0, Complex64) OUT(Complex64)
#define ID_SIG_TestComplex64Alias(IN, OUT)                                             \
    IN(0, TestComplex64Alias) OUT(TestComplex64Alias)
#define ID_SIG_Complex128(IN, OUT) IN(0, Complex128) OUT(Complex128)
#define ID_SIG_TestComplex128Alias(IN, OUT)                                            \
    IN(0, TestComplex128Alias) OUT(TestComplex128Alias)
#define ID_SIG_int16_t(IN, OUT) IN(0, int16_t) OUT(int16_t)
#define ID_SIG_TestInt16Alias(IN, OUT) IN(0, TestInt16Alias) OUT(TestInt16Alias)
#define ID_SIG_int32_t(IN, OUT) IN(0, int32_t) OUT(int32_t)
#define ID_SIG_TestInt32Alias(IN, OUT) IN(0, TestInt32Alias) OUT(TestInt32Alias)
#define ID_SIG_int64_t(IN, OUT) IN(0, int64_t) OUT(int64_t)
#define ID_SIG_TestInt64Alias(IN, OUT) IN(0, TestInt64Alias) OUT(TestInt64Alias)
#define ID_SIG_int8_t(IN, OUT) IN(0, int8_t) OUT(int8_t)
#define ID_SIG_TestInt8Alias(IN, OUT) IN(0, TestInt8Alias) OUT(TestInt8Alias)
#define ID_SIG_Int(IN, OUT) IN(0, Int) OUT(Int)
#define ID_SIG_TestIntAlias(IN, OUT) IN(0, TestIntAlias) OUT(TestIntAlias)
#define ID_SIG_IntIntMap(IN, OUT) IN(0, IntIntMap) OUT(IntIntMap)
#define ID_SIG_TestMapAlias(IN, OUT) IN(0, TestMapAlias) OUT(TestMapAlias)
#define ID_SIG_IntPtr(IN, OUT) IN(0, IntPtr) OUT(IntPtr)
#define ID_SIG_TestPtrAlias(IN, OUT) IN(0, TestPtrAlias) OUT(TestPtrAlias)
#define ID_SIG_ByteSlice(IN, OUT) IN(0, ByteSlice) OUT(ByteSlice)
#define ID_SIG_TestSliceAlias(IN, OUT) IN(0, TestSliceAlias) OUT(TestSliceAlias)
#define ID_SIG_Str(IN, OUT) IN(0, Str) OUT(Str)
#define ID_SIG_TestStringAlias(IN, OUT) IN(0, TestStringAlias) OUT(TestStringAlias)
#define ID_SIG_TestStruct(IN, OUT) IN(0, TestStruct) OUT(TestStruct)
#define ID_SIG_TestStructAlias(IN, OUT) IN(0, TestStructAlias) OUT(TestStructAlias)
#define ID_SIG_uint16_t(IN, OUT) IN(0, uint16_t) OUT(uint16_t)
#define ID_SIG_TestUint16Alias(IN, OUT) IN(0, TestUint16Alias) OUT(TestUint16Alias)
#define ID_SIG_uint32_t(IN, OUT) IN(0, uint32_t) OUT(uint32_t)
#define ID_SIG_TestUint32Alias(IN, OUT) IN(0, TestUint32Alias) OUT(TestUint32Alias)
#define ID_SIG_uint64_t(IN, OUT) IN(0, uint64_t) OUT(uint64_t)
#define ID_SIG_TestUint64Alias(IN, OUT) IN(0, TestUint64Alias) OUT(TestUint64Alias)
#define ID_SIG_uint8_t(IN, OUT) IN(0, uint8_t) OUT(uint8_t)
#define ID_SIG_TestUint8Alias(IN, OUT) IN(0, TestUint8Alias) OUT(TestUint8Alias)
#define ID_SIG_Uint(IN, OUT) IN(0, Uint) OUT(Uint)
#define ID_SIG_TestUintAlias(IN, OUT) IN(0, TestUintAlias) OUT(TestUintAlias)
#define ID_SIG_Uintptr(IN, OUT) IN(0, Uintptr) OUT(Uintptr)
#define ID_SIG_TestUintptrAlias(IN, OUT) IN(0, TestUintptrAlias) OUT(TestUintptrAlias)

/* The functions that return what they are given, all but fPtr. */
#define IDENTITIES(X)                                                                  \
    X(fArray, ByteArray4)                                                              \
    X(fArrayAlias, TestArrayAlias)                                                     \
    X(fBool, bool)                                                                     \
    X(fBoolAlias, TestBoolAlias)                                                       \
    X(fFloat32, float)                                                                 \
    X(fFloat32Alias, TestFloat32Alias)                                                 \
    X(fFloat64, double)                                                                \
    X(fFloat64Alias, TestFloat64Alias)                                                 \
    X(fComplex64, Complex64)                                                           \
    X(fComplex64Alias, TestComplex64Alias)                                             \
    X(fComplex128, Complex128)                                                         \
    X(fComplex128Alias, TestComplex128Alias)                                           \
    X(fInt16, int16_t)                                                                 \
    X(fInt16Alias, TestInt16Alias)                                                     \
    X(fInt32, int32_t)                                                                 \
    X(fInt32Alias, TestInt32Alias)                                                     \
    X(fInt64, int64_t)                                                                 \
    X(fInt64Alias, TestInt64Alias)                                                     \
    X(fInt8, int8_t)                                                                   \
    X(fInt8Alias, TestInt8Alias)                                                       \
    X(fInt, Int)                                                                       \
    X(fIntAlias, TestIntAlias)                                                         \
    X(fMap, IntIntMap)                                                                 \
    X(fMapAlias, TestMapAlias)                                                         \
    X(fPtrAlias, TestPtrAlias)                                                         \
    X(fSlice, ByteSlice)                                                               \
    X(fSliceAlias, TestSliceAlias)                                                     \
    X(fString, Str)                                                                    \
    X(fStringAlias, TestStringAlias)                                                   \
    X(fStruct, TestStruct)                                                             \
    X(fStructAlias, TestStructAlias)                                                   \
    X(fUint16, uint16_t)                                                               \
    X(fUint16Alias, TestUint16Alias)                                                   \
    X(fUint32, uint32_t)                                                               \
    X(fUint32Alias, TestUint32Alias)                                                   \
    X(fUint64, uint64_t)                                                               \
    X(fUint64Alias, TestUint64Alias)                                                   \
    X(fUint8, uint8_t)                                                                 \
    X(fUint8Alias, TestUint8Alias)                                                     \
    X(fUint, Uint)                                                                     \
    X(fUintAlias, TestUintAlias)                                                       \
    X(fUintptr, Uintptr)                                                               \
    X(fUintptrAlias, TestUintptrAlias)

#define IDENTITY(fname, T)                                                             \
    BURROW_FUNC(fname##Fn, T, T a);                                                    \
    static T fname(void *env, T a) {                                                   \
        (void)env;                                                                     \
        return a;                                                                      \
    }                                                                                  \
    BURROW_FUNC_TYPE_DEFINE(fname##Fn, ID_SIG_##T);

IDENTITIES(IDENTITY)

/* fPtr copies what it is given, into the allocator its env is. */
BURROW_FUNC(fPtrFn, IntPtr, IntPtr a);

static IntPtr fPtr(void *env, IntPtr a) {
    if (a == NULL)
        return NULL;
    Int *b = (Int *)mem_alloc((Alloc *)env, sizeof *b, _Alignof(Int));
    if (b == NULL)
        return NULL;
    *b = *a;
    return b;
}

BURROW_FUNC_TYPE_DEFINE(fPtrFn, ID_SIG_IntPtr);

static void reportError(Str property, Error err, TestingT *t) {
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: %s", property, err);
}

/* CheckEqual(f, f, nil), with f the function fname in its own type. The env is
 * the allocator, which only fPtr uses. */
#define CHECK_EQUAL(fname)                                                             \
    do {                                                                               \
        fname##Fn fv = {fname, a};                                                     \
        Any fa = BURROW_ANY(TYPE_OF(fname##Fn), &fv);                                  \
        reportError(BURROW_S(#fname), quick_check_equal(a, fa, fa, NULL), t);          \
    } while (0)

static void TestCheckEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK_EQUAL(fArray);
    CHECK_EQUAL(fArrayAlias);
    CHECK_EQUAL(fBool);
    CHECK_EQUAL(fBoolAlias);
    CHECK_EQUAL(fFloat32);
    CHECK_EQUAL(fFloat32Alias);
    CHECK_EQUAL(fFloat64);
    CHECK_EQUAL(fFloat64Alias);
    CHECK_EQUAL(fComplex64);
    CHECK_EQUAL(fComplex64Alias);
    CHECK_EQUAL(fComplex128);
    CHECK_EQUAL(fComplex128Alias);
    CHECK_EQUAL(fInt16);
    CHECK_EQUAL(fInt16Alias);
    CHECK_EQUAL(fInt32);
    CHECK_EQUAL(fInt32Alias);
    CHECK_EQUAL(fInt64);
    CHECK_EQUAL(fInt64Alias);
    CHECK_EQUAL(fInt8);
    CHECK_EQUAL(fInt8Alias);
    CHECK_EQUAL(fInt);
    CHECK_EQUAL(fIntAlias);
    CHECK_EQUAL(fInt32);
    CHECK_EQUAL(fInt32Alias);
    CHECK_EQUAL(fMap);
    CHECK_EQUAL(fMapAlias);
    CHECK_EQUAL(fPtr);
    CHECK_EQUAL(fPtrAlias);
    CHECK_EQUAL(fSlice);
    CHECK_EQUAL(fSliceAlias);
    CHECK_EQUAL(fString);
    CHECK_EQUAL(fStringAlias);
    CHECK_EQUAL(fStruct);
    CHECK_EQUAL(fStructAlias);
    CHECK_EQUAL(fUint16);
    CHECK_EQUAL(fUint16Alias);
    CHECK_EQUAL(fUint32);
    CHECK_EQUAL(fUint32Alias);
    CHECK_EQUAL(fUint64);
    CHECK_EQUAL(fUint64Alias);
    CHECK_EQUAL(fUint8);
    CHECK_EQUAL(fUint8Alias);
    CHECK_EQUAL(fUint);
    CHECK_EQUAL(fUintAlias);
    CHECK_EQUAL(fUintptr);
    CHECK_EQUAL(fUintptrAlias);
    arena_free(&ar);
}

/* This tests that ArbitraryValue is working by checking that all the arbitrary
 * values of type MyStruct have x = 42. */
#define MY_STRUCT_FIELDS(F, T) F(T, Int, x, "")
BURROW_STRUCT(myStruct, MY_STRUCT_FIELDS);

static ReflectValue myStructGenerate(void *self, Alloc *a, MathRandRand *r, Int size) {
    (void)self;
    (void)r;
    (void)size;
    myStruct *m = (myStruct *)mem_alloc(a, sizeof *m, _Alignof(myStruct));
    if (m == NULL)
        panic_str(BURROW_S("quick_test: out of memory")); /* Not in Go */
    m->x = 42;
    return reflect_value_of(BURROW_ANY(TYPE_OF(myStruct), m));
}

static const QuickGeneratorVT myStructGenerator = {TYPE_OF(myStruct), myStructGenerate};
BURROW_IMPLEMENTS(myStruct, QuickGenerator, myStructGenerator);

BURROW_FUNC(myStructPropertyFn, bool, myStruct in);
#define MY_STRUCT_PROPERTY_SIG(IN, OUT) IN(0, myStruct) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(myStructPropertyFn, MY_STRUCT_PROPERTY_SIG);

static bool myStructProperty(void *env, myStruct in) {
    (void)env;
    return in.x == 42;
}

static void TestCheckProperty(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    myStructPropertyFn f = {myStructProperty, NULL};
    reportError(BURROW_S("myStructProperty"),
                quick_check(a, BURROW_ANY(TYPE_OF(myStructPropertyFn), &f), NULL), t);
    arena_free(&ar);
}

/* func(x int) bool, func(x, y int), func(x int), func(x int) int and
 * func(x int) int32, for TestFailure. */
BURROW_FUNC(FailFn, bool, Int x);
#define FAIL_SIG(IN, OUT) IN(0, Int) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(FailFn, FAIL_SIG);

BURROW_FUNC(TwoIntsFn, void, Int x, Int y);
#define TWO_INTS_SIG(IN, OUT) IN(0, Int) IN(1, Int)
BURROW_FUNC_TYPE_DEFINE(TwoIntsFn, TWO_INTS_SIG);

BURROW_FUNC(OneIntFn, void, Int x);
#define ONE_INT_SIG(IN, OUT) IN(0, Int)
BURROW_FUNC_TYPE_DEFINE(OneIntFn, ONE_INT_SIG);

BURROW_FUNC(IntIntFn, Int, Int x);
#define INT_INT_SIG(IN, OUT) IN(0, Int) OUT(Int)
BURROW_FUNC_TYPE_DEFINE(IntIntFn, INT_INT_SIG);

BURROW_FUNC(IntInt32Fn, int32_t, Int x);
#define INT_INT32_SIG(IN, OUT) IN(0, Int) OUT(int32_t)
BURROW_FUNC_TYPE_DEFINE(IntInt32Fn, INT_INT32_SIG);

static bool fail(void *env, Int x) {
    (void)env;
    (void)x;
    return false;
}

static void twoInts(void *env, Int x, Int y) {
    (void)env;
    (void)x;
    (void)y;
}

static void oneInt(void *env, Int x) {
    (void)env;
    (void)x;
}

static Int intInt(void *env, Int x) {
    (void)env;
    (void)x;
    return 0;
}

static int32_t intInt32(void *env, Int x) {
    (void)env;
    (void)x;
    return 0;
}

static void TestFailure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FailFn f = {fail, NULL};
    Error err = quick_check(a, BURROW_ANY(TYPE_OF(FailFn), &f), NULL);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Check didn't return an error");
    if (errors_as(err, TYPE_QUICK_CHECK_ERROR) == NULL)
        testing_t_errorf_v(t, "Error was not a CheckError: %s", err);

    fUintFn fu = {fUint, NULL};
    fUint32Fn fu32 = {fUint32, NULL};
    err = quick_check_equal(a, BURROW_ANY(TYPE_OF(fUintFn), &fu),
                            BURROW_ANY(TYPE_OF(fUint32Fn), &fu32), NULL);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "#1 CheckEqual didn't return an error");
    if (errors_as(err, TYPE_QUICK_SETUP_ERROR) == NULL)
        testing_t_errorf_v(t, "#1 Error was not a SetupError: %s", err);

    TwoIntsFn two = {twoInts, NULL};
    OneIntFn one = {oneInt, NULL};
    err = quick_check_equal(a, BURROW_ANY(TYPE_OF(TwoIntsFn), &two),
                            BURROW_ANY(TYPE_OF(OneIntFn), &one), NULL);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "#2 CheckEqual didn't return an error");
    if (errors_as(err, TYPE_QUICK_SETUP_ERROR) == NULL)
        testing_t_errorf_v(t, "#2 Error was not a SetupError: %s", err);

    IntIntFn ii = {intInt, NULL};
    IntInt32Fn ii32 = {intInt32, NULL};
    err = quick_check_equal(a, BURROW_ANY(TYPE_OF(IntIntFn), &ii),
                            BURROW_ANY(TYPE_OF(IntInt32Fn), &ii32), NULL);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "#3 CheckEqual didn't return an error");
    if (errors_as(err, TYPE_QUICK_SETUP_ERROR) == NULL)
        testing_t_errorf_v(t, "#3 Error was not a SetupError: %s", err);
    arena_free(&ar);
}

/* type R struct, from TestRecursive. */
BURROW_PTR_TYPE_DECL(RPtr, struct R);
BURROW_SLICE_TYPE_DECL(RPtrSlice, RPtr);
BURROW_SLICE_TYPE_DECL(RSlice, struct R);
BURROW_MAP_TYPE_DECL(IntRMap, Int, struct R);
BURROW_MAP_TYPE_DECL(IntRPtrMap, Int, RPtr);
BURROW_MAP_TYPE_DECL(RPtrRPtrMap, RPtr, RPtr);
BURROW_SLICE_TYPE_DECL(IntRMapSlice, IntRMap);

#define R_FIELDS(F, T)                                                                 \
    F(T, RPtr, Ptr, "")                                                                \
    F(T, RPtrSlice, SliceP, "")                                                        \
    F(T, RSlice, Slice, "")                                                            \
    F(T, IntRMap, Map, "")                                                             \
    F(T, IntRPtrMap, MapP, "")                                                         \
    F(T, RPtrRPtrMap, MapR, "")                                                        \
    F(T, IntRMapSlice, SliceMap, "")
BURROW_STRUCT(R, R_FIELDS);

BURROW_PTR_TYPE_DEFINE(RPtr, R);
BURROW_SLICE_TYPE_DEFINE(RPtrSlice, RPtr);
BURROW_SLICE_TYPE_DEFINE(RSlice, R);
BURROW_MAP_TYPE_DEFINE(IntRMap, Int, R);
BURROW_MAP_TYPE_DEFINE(IntRPtrMap, Int, RPtr);
BURROW_MAP_TYPE_DEFINE(RPtrRPtrMap, RPtr, RPtr);
BURROW_SLICE_TYPE_DEFINE(IntRMapSlice, IntRMap);

/* Not in Go: a goroutine's stack here does not grow, and how deep these
 * recursive values go is up to rand, so their checks run on a goroutine with a
 * stack big enough for any depth they will reach. */
typedef struct DeepCheck {
    Any f;
    SyncWaitGroup wg;
} DeepCheck;

static void deepCheckBody(void *env) {
    DeepCheck *d = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    (void)quick_check(arena_allocator(&ar), d->f, NULL);
    arena_free(&ar);
    sync_wait_group_done(&d->wg);
}

static void deepCheck(TestingT *t, Any f) {
    DeepCheck d;
    memset(&d, 0, sizeof d);
    d.f = f;
    sync_wait_group_add(&d.wg, 1);
    if (!go_stack(BURROW_FN(Func, deepCheckBody, &d), (size_t)64 << 20))
        testing_t_fatalf_v(t, "go_stack failed");
    sync_wait_group_wait(&d.wg);
}

BURROW_FUNC(RecursiveFn, bool, R r);
#define RECURSIVE_SIG(IN, OUT) IN(0, R) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(RecursiveFn, RECURSIVE_SIG);

static bool recursive(void *env, R r) {
    (void)env;
    (void)r;
    return true;
}

/* Recursive data structures didn't terminate.
 * Issues 8818 and 11148. */
static void TestRecursive(TestingT *t) {
    RecursiveFn f = {recursive, NULL};
    deepCheck(t, BURROW_ANY(TYPE_OF(RecursiveFn), &f));
}

/* struct{}, which C cannot declare, and func(struct{}) bool, which is called
 * through its own thunk. */
static const Type emptyStruct = {
    {NULL, 0}, {NULL, 0}, KIND_STRUCT, 0, 1, 0, 0, NULL, NULL, NULL, NULL, 0, 0, NULL,
};

static void emptyStructCall(const void *fn, void **args, void **rets) {
    (void)fn;
    (void)args;
    *(bool *)rets[0] = true;
}

static const TypeOps emptyStructOps = {NULL, NULL, NULL, NULL, emptyStructCall};
static const Field emptyStructSig[] = {
    {BURROW_S_INIT(""), BURROW_S_INIT(""), &emptyStruct, 0},
    {BURROW_S_INIT(""), BURROW_S_INIT(""), TYPE_OF(bool), 0},
};
static const Type emptyStructFn = {
    {NULL, 0},
    {NULL, 0},
    KIND_FUNC,
    (uint32_t)sizeof(Func),
    (uint16_t)_Alignof(Func),
    2,
    0,
    emptyStructSig,
    NULL,
    NULL,
    NULL,
    1,
    0,
    &emptyStructOps,
};

static void emptyStructFunc(void *env) {
    (void)env;
}

static void TestEmptyStruct(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Func f = {emptyStructFunc, NULL};
    (void)quick_check(arena_allocator(&ar), BURROW_ANY(&emptyStructFn, &f), NULL);
    arena_free(&ar);
}

BURROW_PTR_TYPE_DECL(APtr, struct A);
BURROW_PTR_TYPE_DECL(BPtr, struct B);
#define A_FIELDS(F, T) F(T, BPtr, B, "")
#define B_FIELDS(F, T) F(T, APtr, A, "")
BURROW_STRUCT(A, A_FIELDS);
BURROW_STRUCT(B, B_FIELDS);
BURROW_PTR_TYPE_DEFINE(APtr, A);
BURROW_PTR_TYPE_DEFINE(BPtr, B);

BURROW_FUNC(MutuallyRecursiveFn, bool, A a);
#define MUTUALLY_RECURSIVE_SIG(IN, OUT) IN(0, A) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(MutuallyRecursiveFn, MUTUALLY_RECURSIVE_SIG);

static bool mutuallyRecursive(void *env, A a) {
    (void)env;
    (void)a;
    return true;
}

static void TestMutuallyRecursive(TestingT *t) {
    MutuallyRecursiveFn f = {mutuallyRecursive, NULL};
    deepCheck(t, BURROW_ANY(TYPE_OF(MutuallyRecursiveFn), &f));
}

/* type Q struct, from TestNonZeroSliceAndMap. */
BURROW_SLICE_TYPE(IntSlice, Int);
#define Q_FIELDS(F, T)                                                                 \
    F(T, IntIntMap, M, "")                                                             \
    F(T, IntSlice, S, "")
BURROW_STRUCT(Q, Q_FIELDS);

BURROW_FUNC(NonZeroFn, bool, Q q);
#define NON_ZERO_SIG(IN, OUT) IN(0, Q) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(NonZeroFn, NON_ZERO_SIG);

static bool nonZero(void *env, Q q) {
    (void)env;
    return q.M != NULL && !slice_is_nil(q.S);
}

/* Some serialization formats (e.g. encoding/pem) cannot distinguish
 * between a nil and an empty map or slice, so avoid generating the
 * zero value for these. */
static void TestNonZeroSliceAndMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    NonZeroFn f = {nonZero, NULL};
    Error err =
        quick_check(arena_allocator(&ar), BURROW_ANY(TYPE_OF(NonZeroFn), &f), NULL);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
    }
    arena_free(&ar);
}

typedef struct Int64Range {
    int64_t lo, hi;
} Int64Range;

BURROW_FUNC(Int64Fn, bool, int64_t x);
#define INT64_SIG(IN, OUT) IN(0, int64_t) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(Int64Fn, INT64_SIG);

static bool int64Range(void *env, int64_t x) {
    Int64Range *r = (Int64Range *)env;
    if (x < r->lo) {
        r->lo = x;
    }
    if (x > r->hi) {
        r->hi = x;
    }
    return true;
}

static void TestInt64(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Int64Range r = {0, 0};
    Int64Fn f = {int64Range, &r};
    QuickConfig cfg = {10000, 0, NULL, {NULL, NULL}};
    (void)quick_check(arena_allocator(&ar), BURROW_ANY(TYPE_OF(Int64Fn), &f), &cfg);
    int64_t lo = r.lo, hi = r.hi;
    if ((uint64_t)lo >> 62 == 0 || (uint64_t)hi >> 62 == 0) {
        testing_t_errorf_v(
            t, "int64 returned range %#016x,%#016x; does not look like full range", lo,
            hi);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCheckEqual)                                                                  \
    X(TestCheckProperty)                                                               \
    X(TestFailure)                                                                     \
    X(TestRecursive)                                                                   \
    X(TestEmptyStruct)                                                                 \
    X(TestMutuallyRecursive)                                                           \
    X(TestNonZeroSliceAndMap)                                                          \
    X(TestInt64)

TESTING_MAIN(TESTS)
