/* Tests for reflect's TypeAssert.
 *
 * These are Go's tests from all_test.go, under Go's names. Go's generic
 * testTypeAssert[T, V] takes the type it asserts and the type of the value as
 * type parameters. Here both are descriptors, and the values go by pointer.
 *
 * Go's testTypeAssert also checks that v.Interface().(T) agrees with
 * TypeAssert. C has no type assertion to compare with, so that half is left
 * out, and so is TestTypeAssertAllocs, which counts allocations the way only
 * Go's runtime can.
 *
 * fmt.Stringer has a C type in burrow but no descriptor, so the one here is a
 * stand-in with Go's name, package and method.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"

#include "../src/reflect/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)

/* A descriptor with methods and no fields. */
#define DESCM(name, pkg, kind, size, align, nmethod, methods)                          \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        0,                                                                             \
        nmethod,                                                                       \
        NULL,                                                                          \
        methods,                                                                       \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

/* ------------------------------------------------------------- interfaces */

/* fmt.Stringer. */
#define STRINGER_SIG_String(IN, OUT) OUT(Str)
#define STRINGER_METHODS(M, T) M(T, String, string, STRINGER_SIG_String)
STRINGER_METHODS(BURROW__IFACE_METHOD_DEFS, FmtStringer)
static const Method stringer_methods[] = {
    STRINGER_METHODS(BURROW__METHOD_ENTRY, FmtStringer)};
static const Type ty_Stringer =
    DESCM(S_("Stringer"), S_("fmt"), KIND_INTERFACE, sizeof(FmtStringer),
          _Alignof(FmtStringer), 1, stringer_methods);

/* interface{ A() } and interface{ M() }, which have the same shape. */
typedef struct NoArgsVT {
    const Type *self_type;
    void (*call)(void *self);
} NoArgsVT;

typedef struct NoArgs {
    const NoArgsVT *vt;
    void *data;
} NoArgs;

#define SIG_None(IN, OUT)
#define A_METHODS(M, T) M(T, A, call, SIG_None)
A_METHODS(BURROW__IFACE_METHOD_DEFS, NoArgs)
static const Method a_methods[] = {A_METHODS(BURROW__METHOD_ENTRY, NoArgs)};
static const Type ty_iface_A = DESCM(NONAME, NONAME, KIND_INTERFACE, sizeof(NoArgs),
                                     _Alignof(NoArgs), 1, a_methods);

/* The method list here would be A's with another name, so M's entry
 * borrows A's signature and thunk. */
static const Method m_methods[] = {
    {S_("M"), &burrow__ftype_NoArgs_A, burrow__thunk_NoArgs_A},
};
static const Type ty_iface_M = DESCM(NONAME, NONAME, KIND_INTERFACE, sizeof(NoArgs),
                                     _Alignof(NoArgs), 1, m_methods);

/* ----------------------------------------------------- testTypeWithMethod */

/* type testTypeWithMethod struct{ val string }, with String and M. */
typedef struct TestTypeWithMethod {
    Str val;
} TestTypeWithMethod;

static Str ttwm_string(void *self) {
    return ((const TestTypeWithMethod *)self)->val;
}

static void ttwm_m(void *self) {
    (void)self;
}

#define TTWM_METHODS(X, T)                                                             \
    X(T, M, ttwm_m, SIG_None)                                                          \
    X(T, String, ttwm_string, STRINGER_SIG_String)
BURROW_METHODS_DEFINE(TestTypeWithMethod, TTWM_METHODS);

static const Field ttwm_fields[] = {
    {S_("val"), TEST_PKG, TYPE_STRING, offsetof(TestTypeWithMethod, val)},
};

static const Type ty_testTypeWithMethod = {
    S_("testTypeWithMethod"),
    TEST_PKG,
    KIND_STRUCT,
    sizeof(TestTypeWithMethod),
    _Alignof(TestTypeWithMethod),
    1,
    2,
    ttwm_fields,
    burrow__methods_TestTypeWithMethod,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const FmtStringerVT ttwm_stringer = {&ty_testTypeWithMethod, ttwm_string};
static const NoArgsVT ttwm_m_vt = {&ty_testTypeWithMethod, ttwm_m};
BURROW_IMPLEMENTS_DESC(ttwm_as_stringer, &ty_testTypeWithMethod, &ty_Stringer,
                       ttwm_stringer);
BURROW_IMPLEMENTS_DESC(ttwm_as_m, &ty_testTypeWithMethod, &ty_iface_M, ttwm_m_vt);

/* ----------------------------------------------------------------- helpers */

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

/* ValueOf(&x).Elem() for the t at p. */
static ReflectValue settable(const Type *t, void *p) {
    return reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(t), &p)));
}

/* What the t at p holds once any interface is looked through, and false for a
 * nil interface. */
static bool held(const Type *t, const void *p, const Type **ht, void **hp) {
    *ht = t;
    *hp = (void *)(uintptr_t)p;
    if (t->kind != KIND_INTERFACE)
        return true;
    return burrow__reflect_unwrap(ht, hp);
}

/* Go's == on two values of type t: for an interface, the same dynamic type
 * and equal values, or both nil. */
static bool equal(const Type *t, const void *x, const void *y) {
    const Type *xt;
    const Type *yt;
    void *xp;
    void *yp;
    bool xok = held(t, x, &xt, &xp);
    bool yok = held(t, y, &yt, &yp);
    if (!xok || !yok)
        return xok == yok;
    return burrow__reflect_identical(xt, yt) && type_equal(xt, xp, yp);
}

static const Byte zeros[64];

/* testTypeAssert: TypeAssert into the type tt of ValueOf(&val).Elem(), for the
 * val of type vt at val, should give the value of type tt at want, or a zero
 * one when want is NULL, and want_ok. */
static void test_type_assert(TestingT *t, const Type *vt, void *val, const Type *tt,
                             const void *want, bool want_ok) {
    _Alignas(16) Byte out[64];
    if (tt->size > sizeof out) {
        testing_t_fatalf_v(t, "%s is too big for the test", reflect_type_string(tt));
        return;
    }
    memset(out, 0xAB, sizeof out);
    bool ok = reflect_type_assert(settable(vt, val), tt, out);
    if (!equal(tt, out, want == NULL ? zeros : want) || ok != want_ok)
        testing_t_errorf_v(t, "TypeAssert[%s](%s value) = (?, %t); want = (?, %t)",
                           reflect_type_string(tt), reflect_type_string(vt), ok,
                           want_ok);
}

/* -------------------------------------------------------------- the tests */

static void TestTypeAssert(TestingT *t) {
    Int i1 = 123456789;
    Int i2 = -123456789;
    int32_t i3 = 123456789;
    int8_t i4 = -123;
    Int arr[2] = {1234, -5678};
    Str sv = BURROW_S("test value");
    test_type_assert(t, TYPE_INT, &i1, TYPE_INT, &i1, true);
    test_type_assert(t, TYPE_INT, &i2, TYPE_INT, &i2, true);
    test_type_assert(t, TYPE_INT32, &i3, TYPE_INT32, &i3, true);
    test_type_assert(t, TYPE_INT8, &i4, TYPE_INT8, &i4, true);
    ReflectType arr_t = reflect_array_of(2, TYPE_INT);
    test_type_assert(t, arr_t, arr, arr_t, arr, true);
    test_type_assert(t, TYPE_STRING, &sv, TYPE_STRING, &sv, true);
    Any asv = BURROW_ANY(TYPE_STRING, &sv);
    test_type_assert(t, TYPE_ANY, &asv, TYPE_ANY, &asv, true);

    Int v = 123456789;
    Int *pv = &v;
    ReflectType pint = reflect_pointer_to(TYPE_INT);
    test_type_assert(t, pint, &pv, pint, &pv, true);

    Int i123 = 123;
    test_type_assert(t, TYPE_INT, &i123, TYPE_UINT, NULL, false);

    Int one = 1;
    Any aone = BURROW_ANY(TYPE_INT, &one);
    test_type_assert(t, TYPE_INT, &one, TYPE_ANY, &aone, true);
    test_type_assert(t, TYPE_INT, &one, &ty_Stringer, NULL, false);

    TestTypeWithMethod vv = {BURROW_S("test")};
    TestTypeWithMethod *pvv = &vv;
    ReflectType pttwm = reflect_pointer_to(&ty_testTypeWithMethod);
    Any avv = BURROW_ANY(&ty_testTypeWithMethod, &vv);
    Any apvv = BURROW_ANY(pttwm, &pvv);
    FmtStringer svv = {&ttwm_stringer, &vv};
    NoArgs mvv = {&ttwm_m_vt, &vv};
    test_type_assert(t, &ty_testTypeWithMethod, &vv, TYPE_ANY, &avv, true);
    test_type_assert(t, pttwm, &pvv, TYPE_ANY, &apvv, true);
    test_type_assert(t, &ty_testTypeWithMethod, &vv, &ty_Stringer, &svv, true);
    test_type_assert(t, pttwm, &pvv, &ty_Stringer, &svv, true);
    test_type_assert(t, &ty_testTypeWithMethod, &vv, &ty_iface_A, NULL, false);
    test_type_assert(t, pttwm, &pvv, &ty_iface_A, NULL, false);
    test_type_assert(t, TYPE_ANY, &avv, TYPE_ANY, &avv, true);
    test_type_assert(t, &ty_Stringer, &svv, &ty_Stringer, &svv, true);

    test_type_assert(t, &ty_Stringer, &svv, TYPE_ANY, &avv, true);
    test_type_assert(t, TYPE_ANY, &avv, &ty_Stringer, &svv, true);
    test_type_assert(t, &ty_Stringer, &svv, &ty_iface_M, &mvv, true);
    test_type_assert(t, &ty_iface_M, &mvv, &ty_Stringer, &svv, true);

    test_type_assert(t, TYPE_ANY, &aone, TYPE_INT, &one, true);
    test_type_assert(t, TYPE_ANY, &aone, TYPE_UINT8, NULL, false);
    test_type_assert(t, &ty_Stringer, &svv, &ty_testTypeWithMethod, &vv, true);

    Any anil = {NULL, NULL};
    Error enil = BURROW_NO_ERROR;
    test_type_assert(t, TYPE_ANY, &anil, TYPE_ANY, NULL, false);
    test_type_assert(t, TYPE_ANY, &anil, TYPE_OF(Error), NULL, false);
    test_type_assert(t, TYPE_OF(Error), &enil, TYPE_ANY, NULL, false);
    test_type_assert(t, TYPE_OF(Error), &enil, TYPE_OF(Error), NULL, false);
}

static void TestTypeAssertMethod(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    TestTypeWithMethod x = {BURROW_S("test value")};
    TestTypeWithMethod *px = &x;
    ReflectValue method = reflect_value_method_by_name(
        reflect_value_of(BURROW_ANY(reflect_pointer_to(&ty_testTypeWithMethod), &px)),
        a, BURROW_S("String"));
    ReflectType ft = reflect_func_of(
        (Slice){NULL, 0, 0, TYPE_UNSAFE_POINTER},
        slice_from((ReflectType[]){TYPE_STRING}, 1, 1, TYPE_UNSAFE_POINTER), false);
    Func f;
    if (!reflect_type_assert(method, ft, &f)) {
        testing_t_fatalf_v(t, "TypeAssert[func() string](method) = (,false); want = "
                              "(,true)");
        arena_free(&ar);
        return;
    }

    /* Go calls f. What a method value gives back can only be called through
     * reflect, so Call does it here. */
    Slice r = reflect_value_call(reflect_value_of(BURROW_ANY(ft, &f)), a,
                                 (Slice){NULL, 0, 0, TYPE_REFLECT_VALUE});
    Str out = reflect_value_string(*(const ReflectValue *)r.p);
    if (!str_eq(out, BURROW_S("test value")))
        testing_t_fatalf_v(t,
                           "TypeAssert[func() string](method)() = %q; want \"test "
                           "value\"",
                           out);
    arena_free(&ar);
}

/* Runs f(env) and reports whether it panicked. */
static bool panics(void (*f)(void *), void *env) {
    volatile bool got = false;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        (void)r;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

static void assert_zero_val(void *env) {
    (void)env;
    Int x;
    ReflectValue zero = {0};
    (void)reflect_type_assert(zero, TYPE_INT, &x);
}

static void assert_read_only(void *env) {
    (void)env;
    TestTypeWithMethod x = {BURROW_S("")};
    TestTypeWithMethod *px = &x;
    Int out;
    (void)reflect_type_assert(reflect_value_field_by_name(
                                  reflect_value_of(BURROW_ANY(
                                      reflect_pointer_to(&ty_testTypeWithMethod), &px)),
                                  BURROW_S("val")),
                              TYPE_INT, &out);
}

static void zero_val(void *env, TestingT *t) {
    (void)env;
    if (!panics(assert_zero_val, NULL))
        testing_t_fatalf_v(t, "TypeAssert did not panic");
}

static void read_only(void *env, TestingT *t) {
    (void)env;
    if (!panics(assert_read_only, NULL))
        testing_t_fatalf_v(t, "TypeAssert did not panic");
}

static void TestTypeAssertPanic(TestingT *t) {
    testing_t_run(t, BURROW_S("zero val"), BURROW_FN(TestingTFunc, zero_val, NULL));
    testing_t_run(t, BURROW_S("read only"), BURROW_FN(TestingTFunc, read_only, NULL));
}

#define TESTS(X)                                                                       \
    X(TestTypeAssert)                                                                  \
    X(TestTypeAssertMethod)                                                            \
    X(TestTypeAssertPanic)

TESTING_MAIN(TESTS)
