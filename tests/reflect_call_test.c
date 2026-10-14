/* Tests for reflect's calls: Method and MethodByName on types and values, Call
 * and CallSlice, method values, FuncOf and MakeFunc.
 *
 * These are Go's tests from all_test.go, under Go's names. Three things about
 * C change what they can do.
 *
 * A C function returns one value. A function Go's tests declare with several
 * results gets a descriptor written by hand here, whose call thunk spreads the
 * struct the C function returns over the results.
 *
 * What MakeFunc and a method value give back has no C function of the right
 * signature behind it, so it can be called through reflect and nowhere else.
 * Where Go calls one directly, or calls what Interface gives back, the test
 * calls it with Call instead.
 *
 * An interface with methods is a vtable here. A type goes into one through the
 * vtable BURROW_IMPLEMENTS records for it, so the types these tests put into
 * interfaces record theirs, and the interfaces Go takes from io that burrow has
 * no descriptor for are stand-ins with Go's names and methods.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"
#include "burrow/strings.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)

/* A descriptor written out by hand. */
#define DESC(name, pkg, kind, size, align, nfield, fields, nmethod, methods, elem,     \
             len, ops)                                                                 \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)(size),                                                              \
        (uint16_t)(align),                                                             \
        nfield,                                                                        \
        nmethod,                                                                       \
        fields,                                                                        \
        methods,                                                                       \
        elem,                                                                          \
        NULL,                                                                          \
        len,                                                                           \
        0,                                                                             \
        ops,                                                                           \
    }

#define NONAME {NULL, 0}
#define TEST_PKG S_("reflect_test")

/* A field of a struct, and a parameter or a result of a function, which holds
 * its position where a struct field holds its offset. */
#define FIELD(nm, ty, off) {S_(nm), S_(""), ty, (uint32_t)(off)}
#define PARAM(ty, i) {S_(""), S_(""), ty, (uint32_t)(i)}

/* An unnamed function type taking nin of the nfield entries of fields, called
 * through call. */
#define FUNC_DESC(nfield, fields, elem, nin, ops)                                      \
    DESC(NONAME, NONAME, KIND_FUNC, sizeof(Func), _Alignof(Func), nfield, fields, 0,   \
         NULL, elem, nin, ops)

/* []ReflectValue{...} and []ReflectType{...}. */
#define NARGS(T, ...) ((Int)(sizeof((T[]){__VA_ARGS__}) / sizeof(T)))
#define VALS(...)                                                                      \
    slice_from((ReflectValue[]){__VA_ARGS__}, NARGS(ReflectValue, __VA_ARGS__),        \
               NARGS(ReflectValue, __VA_ARGS__), TYPE_REFLECT_VALUE)
#define TYPES(...)                                                                     \
    slice_from((ReflectType[]){__VA_ARGS__}, NARGS(ReflectType, __VA_ARGS__),          \
               NARGS(ReflectType, __VA_ARGS__), TYPE_UNSAFE_POINTER)
#define NOVALS ((Slice){NULL, 0, 0, TYPE_REFLECT_VALUE})
#define NOTYPES ((Slice){NULL, 0, 0, TYPE_UNSAFE_POINTER})

/* ----------------------------------------------------------------- helpers */

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

/* ValueOf(&x).Elem() for the t at p. */
static ReflectValue settable(const Type *t, void *p) {
    return reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(t), &p)));
}

static Arena *arena_new(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return ar;
}

/* ValueOf(x) for a copy, made in a, of the t at p. */
static ReflectValue box(Alloc *a, const Type *t, const void *p) {
    void *q = mem_alloc(a, t->size == 0 ? 1 : t->size, t->align);
    if (q == NULL)
        abort();
    if (t->size > 0)
        memcpy(q, p, t->size);
    return val(t, q);
}

static bool str_is(Str s, const char *want) {
    return str_eq(s, str_from_cstr(want));
}

static ReflectValue vint(Alloc *a, Int n) {
    return box(a, TYPE_INT, &n);
}

/* The i'th of the Values a call returned, or the zero Value past the end. */
static ReflectValue at(Slice s, Int i) {
    if (i < 0 || i >= s.len)
        return (ReflectValue){0};
    return ((const ReflectValue *)s.p)[i];
}

/* The Int a call of v with in returns first, or -1 if it returns nothing. */
static Int call_int(ReflectValue v, Alloc *a, Slice in) {
    Slice r = reflect_value_call(v, a, in);
    if (r.len == 0)
        return -1;
    return reflect_value_int(at(r, 0));
}

static bool panics(void (*f)(void *), void *env, char *buf, size_t n) {
    volatile bool got = false;
    buf[0] = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str text = panic_text(r);
        size_t k = (size_t)text.len < n - 1 ? (size_t)text.len : n - 1;
        memcpy(buf, text.p, k);
        buf[k] = 0;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* Go's shouldPanic: f has to panic, and when expect is not empty the text has
 * to start with "reflect" and contain expect. */
static void should_panic(TestingT *t, const char *expect, void (*f)(void *),
                         void *env) {
    char buf[256];
    if (!panics(f, env, buf, sizeof buf)) {
        testing_t_errorf_v(t, "did not panic, want %q", expect);
        return;
    }
    if (expect[0] == 0)
        return;
    if (strncmp(buf, "reflect", 7) != 0)
        testing_t_errorf_v(t, "panic string does not start with \"reflect\": %s", buf);
    if (strstr(buf, expect) == NULL)
        testing_t_errorf_v(t, "%q does not contain %q", buf, expect);
}

/* Go's checkSameType, as far as C can take it: C has no way to find the
 * descriptor the compiler made for a type, so this asks that the two read as
 * the same type and that the made type's zero value carries it. */
static void check_same_type(TestingT *t, ReflectType x, ReflectType y) {
    ReflectType z = reflect_type_of(reflect_value_interface(reflect_zero(x)));
    if (!str_eq(reflect_type_string(x), reflect_type_string(y)) ||
        reflect_type_kind(x) != reflect_type_kind(y) ||
        reflect_type_size(x) != reflect_type_size(y) || z != x)
        testing_t_errorf_v(t, "did not find preexisting type for %s (vs %s)",
                           reflect_type_string(x), reflect_type_string(y));
}

/* ---------------------------------------------------------------- TestFunc */

/* type two [2]uintptr, as a struct so that C can return it. */
typedef struct Two {
    Uintptr v[2];
} Two;

static const Type ty_two = DESC(S_("two"), TEST_PKG, KIND_ARRAY, sizeof(Two),
                                _Alignof(Two), 0, NULL, 0, NULL, TYPE_UINTPTR, 2, NULL);

typedef struct DummyOut {
    uint8_t i;
    Int j;
    uint8_t k;
    Two l;
    uint8_t m;
    float n;
    uint8_t o;
} DummyOut;

BURROW_FUNC(DummyFn, DummyOut, uint8_t b, Int c, uint8_t d, Two e, uint8_t f, float g,
            uint8_t h);

/* Difficult test for function call because of implicit padding between
 * arguments. */
static DummyOut dummy(void *env, uint8_t b, Int c, uint8_t d, Two e, uint8_t f, float g,
                      uint8_t h) {
    (void)env;
    return (DummyOut){b, c, d, e, f, g, h};
}

static void dummy_call(const void *fn, void **args, void **rets) {
    const DummyFn *d = fn;
    DummyOut r = d->f(d->env, *(uint8_t *)args[0], *(Int *)args[1], *(uint8_t *)args[2],
                      *(Two *)args[3], *(uint8_t *)args[4], *(float *)args[5],
                      *(uint8_t *)args[6]);
    *(uint8_t *)rets[0] = r.i;
    *(Int *)rets[1] = r.j;
    *(uint8_t *)rets[2] = r.k;
    *(Two *)rets[3] = r.l;
    *(uint8_t *)rets[4] = r.m;
    *(float *)rets[5] = r.n;
    *(uint8_t *)rets[6] = r.o;
}

static const TypeOps dummy_ops = {NULL, NULL, NULL, NULL, dummy_call};

static const Field dummy_sig[] = {
    PARAM(TYPE_UINT8, 0),   PARAM(TYPE_INT, 1),   PARAM(TYPE_UINT8, 2),
    PARAM(&ty_two, 3),      PARAM(TYPE_UINT8, 4), PARAM(TYPE_FLOAT32, 5),
    PARAM(TYPE_UINT8, 6),   PARAM(TYPE_UINT8, 0), PARAM(TYPE_INT, 0),
    PARAM(TYPE_UINT8, 0),   PARAM(&ty_two, 0),    PARAM(TYPE_UINT8, 0),
    PARAM(TYPE_FLOAT32, 0), PARAM(TYPE_UINT8, 0),
};

static const Type ty_dummy = FUNC_DESC(14, dummy_sig, NULL, 7, &dummy_ops);

/* The arguments TestFunc and TestMakeFunc pass, in a. */
static Slice dummy_args(Alloc *a) {
    ReflectValue vs[] = {
        box(a, TYPE_UINT8, &(uint8_t){10}), vint(a, 20),
        box(a, TYPE_UINT8, &(uint8_t){30}), box(a, &ty_two, &(Two){{40, 50}}),
        box(a, TYPE_UINT8, &(uint8_t){60}), box(a, TYPE_FLOAT32, &(float){70}),
        box(a, TYPE_UINT8, &(uint8_t){80}),
    };
    ReflectValue *p = mem_alloc(a, sizeof vs, _Alignof(ReflectValue));
    if (p == NULL)
        abort();
    memcpy(p, vs, sizeof vs);
    return slice_from(p, 7, 7, TYPE_REFLECT_VALUE);
}

/* Checks what a call of a dummy returned. */
static void check_dummy(TestingT *t, Slice ret) {
    if (ret.len != 7) {
        testing_t_fatalf_v(t, "Call returned %d values, want 7", ret.len);
        return;
    }
    uint8_t i = (uint8_t)reflect_value_uint(at(ret, 0));
    Int j = reflect_value_int(at(ret, 1));
    uint8_t k = (uint8_t)reflect_value_uint(at(ret, 2));
    Any la = reflect_value_interface(at(ret, 3));
    Two l = {{0, 0}};
    if (la.t == &ty_two)
        memcpy(&l, la.data, sizeof l);
    uint8_t m = (uint8_t)reflect_value_uint(at(ret, 4));
    float n = (float)reflect_value_float(at(ret, 5));
    uint8_t o = (uint8_t)reflect_value_uint(at(ret, 6));

    if (i != 10 || j != 20 || k != 30 || l.v[0] != 40 || l.v[1] != 50 || m != 60 ||
        n != 70.0F || o != 80)
        testing_t_errorf_v(
            t,
            "Call returned %d, %d, %d, [%d %d], %d, %g, %d; want 10, 20, "
            "30, [40, 50], 60, 70, 80",
            i, j, k, l.v[0], l.v[1], m, (double)n, o);
}

static void TestFunc(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    DummyFn f = BURROW_FN(DummyFn, dummy, NULL);
    Slice ret = reflect_value_call(val(&ty_dummy, &f), a, dummy_args(a));
    check_dummy(t, ret);
    for (Int i = 0; i < ret.len; i++)
        if (reflect_value_can_addr(at(ret, i)))
            testing_t_errorf_v(t, "result %d is addressable", i);
    arena_free(&ar);
}

/* ------------------------------------------------------- TestCallWithStruct */

/* type emptyStruct struct{}, which C cannot declare, so it is only ever a
 * descriptor and the functions taking and returning one are thunks. */
static const Type ty_emptyStruct = DESC(S_("emptyStruct"), TEST_PKG, KIND_STRUCT, 0, 1,
                                        0, NULL, 0, NULL, NULL, 0, NULL);

static void empty_call(const void *fn, void **args, void **rets) {
    (void)fn;
    (void)args;
    (void)rets;
}

/* What a value of either type holds, which the thunk never calls. */
static void empty_fn(void *env) {
    (void)env;
}

static const TypeOps empty_ops = {NULL, NULL, NULL, NULL, empty_call};
static const Field empty_sig[] = {PARAM(&ty_emptyStruct, 0)};
/* func() emptyStruct and func(emptyStruct). */
static const Type ty_returnEmpty = FUNC_DESC(1, empty_sig, NULL, 0, &empty_ops);
static const Type ty_takesEmpty = FUNC_DESC(1, empty_sig, NULL, 1, &empty_ops);

#define NON_EMPTY_FIELDS(F, T) F(T, Int, member, "")
BURROW_STRUCT(nonEmptyStruct, NON_EMPTY_FIELDS);

BURROW_FUNC(ReturnNonEmptyFn, nonEmptyStruct, Int i);
BURROW_FUNC(TakesNonEmptyFn, Int, nonEmptyStruct n);

#define RETURN_NON_EMPTY_SIG(IN, OUT)                                                  \
    IN(0, Int)                                                                         \
    OUT(nonEmptyStruct)
#define TAKES_NON_EMPTY_SIG(IN, OUT)                                                   \
    IN(0, nonEmptyStruct)                                                              \
    OUT(Int)

BURROW_FUNC_TYPE_DECL(ReturnNonEmptyFn);
BURROW_FUNC_TYPE_DECL(TakesNonEmptyFn);
BURROW_FUNC_TYPE_DEFINE(ReturnNonEmptyFn, RETURN_NON_EMPTY_SIG);
BURROW_FUNC_TYPE_DEFINE(TakesNonEmptyFn, TAKES_NON_EMPTY_SIG);

static nonEmptyStruct return_non_empty(void *env, Int i) {
    (void)env;
    return (nonEmptyStruct){i};
}

static Int takes_non_empty(void *env, nonEmptyStruct n) {
    (void)env;
    return n.member;
}

static void TestCallWithStruct(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Func nop = {empty_fn, NULL};

    Slice r = reflect_value_call(val(&ty_returnEmpty, &nop), a, NOVALS);
    if (r.len != 1 || reflect_value_type(at(r, 0)) != &ty_emptyStruct)
        testing_t_errorf_v(t, "returning empty struct returned %d values", r.len);
    int empty = 0;
    r = reflect_value_call(val(&ty_takesEmpty, &nop), a,
                           VALS(val(&ty_emptyStruct, &empty)));
    if (r.len != 0)
        testing_t_errorf_v(t, "takesEmpty returned %d values", r.len);

    ReturnNonEmptyFn rf = BURROW_FN(ReturnNonEmptyFn, return_non_empty, NULL);
    r = reflect_value_call(val(TYPE_OF(ReturnNonEmptyFn), &rf), a, VALS(vint(a, 42)));
    if (r.len != 1 || reflect_value_type(at(r, 0)) != TYPE_OF(nonEmptyStruct) ||
        reflect_value_int(reflect_value_field(at(r, 0), 0)) != 42)
        testing_t_errorf_v(t, "returnNonEmpty returned %d values", r.len);

    TakesNonEmptyFn tf = BURROW_FN(TakesNonEmptyFn, takes_non_empty, NULL);
    nonEmptyStruct n = {42};
    r = reflect_value_call(val(TYPE_OF(TakesNonEmptyFn), &tf), a,
                           VALS(val(TYPE_OF(nonEmptyStruct), &n)));
    if (r.len != 1 || reflect_value_type(at(r, 0)) != TYPE_INT ||
        reflect_value_int(at(r, 0)) != 42)
        testing_t_errorf_v(t, "takesNonEmpty returned %d values", r.len);
    arena_free(&ar);
}

/* ------------------------------------------------------------ TestMakeFunc */

static Slice identity_impl(void *env, Slice in) {
    (void)env;
    return in;
}

static void TestMakeFunc(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    DummyFn f = BURROW_FN(DummyFn, dummy, NULL);
    ReflectValue fv = reflect_make_func(
        a, &ty_dummy, BURROW_FN(ReflectMakeFuncImpl, identity_impl, NULL));
    reflect_value_set(settable(&ty_dummy, &f), fv);

    /* Call g with small arguments so that there is something predictable (and
     * different from the correct results) in those positions on the stack. */
    DummyOut g = dummy(NULL, 1, 2, 3, (Two){{4, 5}}, 6, 7, 8);
    (void)g;

    /* Call constructed function f, which only reflect can do. */
    check_dummy(t, reflect_value_call(val(&ty_dummy, &f), a, dummy_args(a)));
    arena_free(&ar);
}

/* func(int) int. */
BURROW_FUNC(IntFn, Int, Int i);
#define INT_FN_SIG(IN, OUT)                                                            \
    IN(0, Int)                                                                         \
    OUT(Int)
BURROW_FUNC_TYPE_DECL(IntFn);
BURROW_FUNC_TYPE_DEFINE(IntFn, INT_FN_SIG);

static Int int_identity(void *env, Int i) {
    (void)env;
    return i;
}

/* What an impl hands back has to last until the call it made returns, so it
 * lives in the impl's env. */
typedef struct ImplEnv {
    TestingT *t;
    Alloc *a;
    Int n;
    double x;
    ReflectValue out[1];
} ImplEnv;

static Slice incr_impl(void *env, Slice in) {
    ImplEnv *e = env;
    e->n = reflect_value_int(at(in, 0)) + 1;
    e->out[0] = val(TYPE_INT, &e->n);
    return slice_from(e->out, 1, 1, TYPE_REFLECT_VALUE);
}

static void TestMakeFuncInterface(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    IntFn fn = BURROW_FN(IntFn, int_identity, NULL);
    ImplEnv e = {t, a, 0, 0, {{0}}};
    ReflectValue fv = reflect_make_func(a, TYPE_OF(IntFn),
                                        BURROW_FN(ReflectMakeFuncImpl, incr_impl, &e));
    reflect_value_set(settable(TYPE_OF(IntFn), &fn), fv);
    Int r = call_int(val(TYPE_OF(IntFn), &fn), a, VALS(vint(a, 2)));
    if (r != 3)
        testing_t_errorf_v(t, "Call returned %d, want 3", r);
    r = call_int(fv, a, VALS(vint(a, 14)));
    if (r != 15)
        testing_t_errorf_v(t, "Call returned %d, want 15", r);
    r = call_int(reflect_value_of(reflect_value_interface(fv)), a, VALS(vint(a, 26)));
    if (r != 27)
        testing_t_errorf_v(t, "Call returned %d, want 27", r);
    arena_free(&ar);
}

/* []int and []float64, as C types with descriptors, so that a signature can
 * name them. */
typedef Slice IntSlice;
typedef Slice Float64Slice;
static const Type burrow_type_IntSlice =
    DESC(NONAME, NONAME, KIND_SLICE, sizeof(Slice), _Alignof(Slice), 0, NULL, 0, NULL,
         TYPE_INT, 0, NULL);
static const Type burrow_type_Float64Slice =
    DESC(NONAME, NONAME, KIND_SLICE, sizeof(Slice), _Alignof(Slice), 0, NULL, 0, NULL,
         TYPE_FLOAT64, 0, NULL);

/* func(_ int, is ...int) []int. */
BURROW_FUNC(DDDIntFn, IntSlice, Int x, IntSlice is);
#define DDD_INT_FN_SIG(IN, OUT)                                                        \
    IN(0, Int)                                                                         \
    IN(1, IntSlice)                                                                    \
    OUT(IntSlice)
BURROW_FUNC_TYPE_DECL(DDDIntFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(DDDIntFn, Int, DDD_INT_FN_SIG);

static IntSlice ddd_nil(void *env, Int x, IntSlice is) {
    (void)env;
    (void)x;
    (void)is;
    return (IntSlice){NULL, 0, 0, TYPE_INT};
}

static Slice second_impl(void *env, Slice in) {
    (void)env;
    return slice_from((ReflectValue *)in.p + 1, 1, 1, TYPE_REFLECT_VALUE);
}

static void check_two_three(TestingT *t, Slice ret) {
    Any x = reflect_value_interface(at(ret, 0));
    if (x.t != TYPE_OF(IntSlice)) {
        testing_t_errorf_v(t, "Call returned a %s; want []int",
                           reflect_type_string(x.t));
        return;
    }
    const Slice *r = x.data;
    const Int *p = r->p;
    if (r->len < 2 || p[0] != 2 || p[1] != 3)
        testing_t_errorf_v(t, "Call returned %d values; want 2, 3", r->len);
}

static void TestMakeFuncVariadic(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    /* Test that variadic arguments are packed into a slice and passed as last
     * arg. */
    DDDIntFn fn = BURROW_FN(DDDIntFn, ddd_nil, NULL);
    ReflectValue fv = reflect_make_func(
        a, TYPE_OF(DDDIntFn), BURROW_FN(ReflectMakeFuncImpl, second_impl, NULL));
    reflect_value_set(settable(TYPE_OF(DDDIntFn), &fn), fv);
    ReflectValue f = val(TYPE_OF(DDDIntFn), &fn);
    Int two_three[] = {2, 3};
    IntSlice s = {two_three, 2, 2, TYPE_INT};

    check_two_three(t,
                    reflect_value_call(f, a, VALS(vint(a, 1), vint(a, 2), vint(a, 3))));
    check_two_three(t, reflect_value_call_slice(
                           f, a, VALS(vint(a, 1), val(TYPE_OF(IntSlice), &s))));
    check_two_three(
        t, reflect_value_call(fv, a, VALS(vint(a, 1), vint(a, 2), vint(a, 3))));
    check_two_three(t, reflect_value_call_slice(
                           fv, a, VALS(vint(a, 1), val(TYPE_OF(IntSlice), &s))));
    arena_free(&ar);
}

/* --------------------------------- TestMakeFuncValidReturnAssignments and
 * TestMakeFuncInvalidReturnAssignments */

/* type T struct{ a, b, c int }, the unnamed struct with the same fields, and a
 * second named type U like T. The unnamed struct keeps the package path, as Go's
 * does, since its fields are unexported. */
typedef struct ABC {
    Int a, b, c;
} ABC;

static const Field abc_fields[] = {
    FIELD("a", TYPE_INT, offsetof(ABC, a)),
    FIELD("b", TYPE_INT, offsetof(ABC, b)),
    FIELD("c", TYPE_INT, offsetof(ABC, c)),
};

static const Type ty_T = DESC(S_("T"), TEST_PKG, KIND_STRUCT, sizeof(ABC),
                              _Alignof(ABC), 3, abc_fields, 0, NULL, NULL, 0, NULL);
static const Type ty_U = DESC(S_("U"), TEST_PKG, KIND_STRUCT, sizeof(ABC),
                              _Alignof(ABC), 3, abc_fields, 0, NULL, NULL, 0, NULL);
static const Type ty_abc = DESC(NONAME, TEST_PKG, KIND_STRUCT, sizeof(ABC),
                                _Alignof(ABC), 3, abc_fields, 0, NULL, NULL, 0, NULL);

/* type WC struct{}, with Write and Close, which implements io.WriteCloser.
 * C has no empty struct, so it holds a byte nothing reads. */
typedef struct WC {
    char unused;
} WC;

static Int wc_write(void *w, Slice p, Error *err) {
    (void)w;
    (void)p;
    *err = BURROW_NO_ERROR;
    return 0;
}

static Error wc_close(void *w) {
    (void)w;
    return BURROW_NO_ERROR;
}

#define WC_SIG_Close(IN, OUT) OUT(Error)
#define WC_METHODS(X, T)                                                               \
    X(T, Close, wc_close, WC_SIG_Close)                                                \
    X(T, Write, wc_write, IO_SIG_WRITE)
WC_METHODS(BURROW__METHOD_DEFS, WC)
static const Method wc_methods[] = {WC_METHODS(BURROW__METHOD_ENTRY, WC)};
static const Type burrow_type_WC =
    DESC(S_("WC"), TEST_PKG, KIND_STRUCT, sizeof(WC), _Alignof(WC), 0, NULL, 2,
         wc_methods, NULL, 0, NULL);

/* io.WriteCloser and io.ReadWriteCloser, which burrow has C types for and no
 * descriptors. Interface methods go in name order. */
#define WRITE_CLOSER_METHODS(X, T)                                                     \
    X(T, Close, closer.close, WC_SIG_Close)                                            \
    X(T, Write, writer.write, IO_SIG_WRITE)
WRITE_CLOSER_METHODS(BURROW__IFACE_METHOD_DEFS, IoWriteCloser)
static const Method write_closer_methods[] = {
    WRITE_CLOSER_METHODS(BURROW__METHOD_ENTRY, IoWriteCloser)};
static const Type ty_WriteCloser =
    DESC(S_("WriteCloser"), S_("io"), KIND_INTERFACE, sizeof(IoWriteCloser),
         _Alignof(IoWriteCloser), 0, NULL, 2, write_closer_methods, NULL, 0, NULL);

#define READ_WRITE_CLOSER_METHODS(X, T)                                                \
    X(T, Close, closer.close, WC_SIG_Close)                                            \
    X(T, Read, reader.read, IO_SIG_READ)                                               \
    X(T, Write, writer.write, IO_SIG_WRITE)
READ_WRITE_CLOSER_METHODS(BURROW__IFACE_METHOD_DEFS, IoReadWriteCloser)
static const Method read_write_closer_methods[] = {
    READ_WRITE_CLOSER_METHODS(BURROW__METHOD_ENTRY, IoReadWriteCloser)};
static const Type ty_ReadWriteCloser = DESC(
    S_("ReadWriteCloser"), S_("io"), KIND_INTERFACE, sizeof(IoReadWriteCloser),
    _Alignof(IoReadWriteCloser), 0, NULL, 3, read_write_closer_methods, NULL, 0, NULL);

static const IoWriterVT wc_writer_vt = {&burrow_type_WC, wc_write};
static const IoWriteCloserVT wc_write_closer_vt = {{&burrow_type_WC, wc_write},
                                                   {&burrow_type_WC, wc_close}};
BURROW_IMPLEMENTS_DESC(wc_as_writer, &burrow_type_WC, TYPE_OF(IoWriter), wc_writer_vt);
BURROW_IMPLEMENTS_DESC(wc_as_write_closer, &burrow_type_WC, &ty_WriteCloser,
                       wc_write_closer_vt);

/* var w io.WriteCloser = &WC{}, and ValueOf(&w).Elem(). */
static ReflectValue write_closer(Alloc *a) {
    WC *wc = mem_alloc(a, sizeof *wc, _Alignof(WC));
    IoWriteCloser *w = mem_alloc(a, sizeof *w, _Alignof(IoWriteCloser));
    if (wc == NULL || w == NULL)
        abort();
    *w = (IoWriteCloser){&wc_write_closer_vt, wc};
    return settable(&ty_WriteCloser, w);
}

/* An impl that returns e->out[0]. */
static Slice const_impl(void *env, Slice in) {
    (void)in;
    ImplEnv *e = env;
    return slice_from(e->out, 1, 1, TYPE_REFLECT_VALUE);
}

/* MakeFunc(ft, const_impl) called with no arguments. */
static void call_returning(Alloc *a, ReflectType ft, ImplEnv *e) {
    ReflectValue f =
        reflect_make_func(a, ft, BURROW_FN(ReflectMakeFuncImpl, const_impl, e));
    reflect_value_call(f, a, NOVALS);
}

static void TestMakeFuncValidReturnAssignments(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ImplEnv e = {t, a, 0, 0, {{0}}};
    /* reflect.Values returned from the wrapped function should be
     * assignment-converted to the types returned by the result of MakeFunc. */

    /* Concrete types should be promotable to interfaces they implement. */
    e.out[0] = val(TYPE_OF(Error), &io_eof);
    call_returning(a, reflect_func_of(NOTYPES, TYPES(TYPE_OF(Error)), false), &e);

    /* Super-interfaces should be promotable to simpler interfaces. */
    e.out[0] = write_closer(a);
    call_returning(a, reflect_func_of(NOTYPES, TYPES(TYPE_OF(IoWriter)), false), &e);

    /* Channels should be promotable to directional channels. */
    ReflectType chan_int = reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT);
    ReflectType recv_int = reflect_chan_of(REFLECT_RECV_DIR, TYPE_INT);
    e.out[0] = reflect_make_chan(a, chan_int, 0);
    call_returning(a, reflect_func_of(NOTYPES, TYPES(recv_int), false), &e);

    /* Unnamed types should be promotable to named types. */
    ABC abc = {1, 2, 3};
    e.out[0] = val(&ty_abc, &abc);
    call_returning(a, reflect_func_of(NOTYPES, TYPES(&ty_T), false), &e);
    arena_free(&ar);
}

typedef struct BadReturn {
    Alloc *a;
    ReflectType ft;
    ImplEnv *e;
} BadReturn;

static void bad_return(void *env) {
    BadReturn *b = env;
    call_returning(b->a, b->ft, b->e);
}

static void TestMakeFuncInvalidReturnAssignments(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ImplEnv e = {t, a, 7, 0, {{0}}};

    /* Type doesn't implement the required interface. */
    e.out[0] = val(TYPE_INT, &e.n);
    BadReturn b = {a, reflect_func_of(NOTYPES, TYPES(TYPE_OF(Error)), false), &e};
    should_panic(t, "", bad_return, &b);

    /* Assigning to an interface with additional methods. */
    e.out[0] = write_closer(a);
    b.ft = reflect_func_of(NOTYPES, TYPES(&ty_ReadWriteCloser), false);
    should_panic(t, "", bad_return, &b);

    /* Directional channels can't be assigned to bidirectional ones. */
    ReflectType chan_int = reflect_chan_of(REFLECT_BOTH_DIR, TYPE_INT);
    ReflectType recv_int = reflect_chan_of(REFLECT_RECV_DIR, TYPE_INT);
    ReflectValue c = reflect_make_chan(a, chan_int, 0);
    e.out[0] = val(recv_int, c.ptr);
    b.ft = reflect_func_of(NOTYPES, TYPES(chan_int), false);
    should_panic(t, "", bad_return, &b);

    /* Two named types which are otherwise identical. */
    ABC u = {1, 2, 3};
    e.out[0] = val(&ty_U, &u);
    b.ft = reflect_func_of(NOTYPES, TYPES(&ty_T), false);
    should_panic(t, "", bad_return, &b);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ Point */

/* type Point struct { x, y int }, with Go's seven methods. Go's methods on
 * Point take a Point and the last two take a *Point. Here every receiver is a
 * pointer, so Point and *Point have the same seven methods, sorted by name,
 * and the indexes differ from the ones Go's comments give: Dist is still 1,
 * and TotalDist is 6 rather than 4. */
typedef struct Point {
    Int x, y;
} Point;

extern const Type burrow_type_Point;

static Int point_another_method(Point *p, Int scale) {
    (void)p;
    (void)scale;
    return -1;
}

static Int point_dist(Point *p, Int scale) {
    return p->x * p->x * scale + p->y * p->y * scale;
}

static Int point_gc_method(Point *p, Int k) {
    return k + p->x;
}

static int32_t point_int32_method(Point *p, int32_t x) {
    (void)p;
    return x;
}

static int64_t point_int64_method(Point *p, int64_t x) {
    (void)p;
    return x;
}

/* Exercise no-argument/no-result paths. */
static void point_no_args(Point *p) {
    (void)p;
}

static Int point_total_dist(const Point *p, Slice points) {
    Int tot = 0;
    const Point *qs = points.p;
    for (Int i = 0; i < points.len; i++) {
        Int dx = qs[i].x - p->x;
        Int dy = qs[i].y - p->y;
        tot += dx * dx + dy * dy; /* Should call Sqrt, but it's just a test. */
    }
    return tot;
}

#define POINT_SIG_Scale(IN, OUT)                                                       \
    IN(0, Int)                                                                         \
    OUT(Int)
#define POINT_SIG_Int32(IN, OUT)                                                       \
    IN(0, int32_t)                                                                     \
    OUT(int32_t)
#define POINT_SIG_Int64(IN, OUT)                                                       \
    IN(0, int64_t)                                                                     \
    OUT(int64_t)
#define POINT_SIG_None(IN, OUT)

#define POINT_METHODS(M, T)                                                            \
    M(T, AnotherMethod, point_another_method, POINT_SIG_Scale)                         \
    M(T, Dist, point_dist, POINT_SIG_Scale)                                            \
    M(T, GCMethod, point_gc_method, POINT_SIG_Scale)                                   \
    M(T, Int32Method, point_int32_method, POINT_SIG_Int32)                             \
    M(T, Int64Method, point_int64_method, POINT_SIG_Int64)                             \
    M(T, NoArgs, point_no_args, POINT_SIG_None)

/* A signature list has no way to say a method is variadic, so TotalDist is
 * written out by hand and the other six use the pieces the list is made of. */
POINT_METHODS(BURROW__METHOD_DEFS, Point)

static const Type ty_sl_Point =
    DESC(NONAME, NONAME, KIND_SLICE, sizeof(Slice), _Alignof(Slice), 0, NULL, 0, NULL,
         &burrow_type_Point, 0, NULL);
static const Field total_dist_sig[] = {PARAM(&ty_sl_Point, 0), PARAM(TYPE_INT, 0)};
static const Type ty_total_dist =
    FUNC_DESC(2, total_dist_sig, &burrow_type_Point, 1, NULL);

static void point_total_dist_thunk(void *recv, void **args, void **rets) {
    *(Int *)rets[0] = point_total_dist(recv, *(const Slice *)args[0]);
}

static const Method point_methods[] = {
    POINT_METHODS(BURROW__METHOD_ENTRY, Point){S_("TotalDist"), &ty_total_dist,
                                               point_total_dist_thunk},
};

static const Field point_fields[] = {
    FIELD("x", TYPE_INT, offsetof(Point, x)),
    FIELD("y", TYPE_INT, offsetof(Point, y)),
};

const Type burrow_type_Point =
    DESC(S_("Point"), TEST_PKG, KIND_STRUCT, sizeof(Point), _Alignof(Point), 2,
         point_fields, 7, point_methods, NULL, 0, NULL);

/* interface{ Dist(int) int }, and Point's vtable for it. */
typedef struct DisterVT {
    const Type *self_type;
    Int (*dist)(void *self, Int scale);
} DisterVT;

typedef struct Dister {
    const DisterVT *vt;
    void *data;
} Dister;

#define DISTER_METHODS(X, T) X(T, Dist, dist, POINT_SIG_Scale)
DISTER_METHODS(BURROW__IFACE_METHOD_DEFS, Dister)
static const Method dister_methods[] = {DISTER_METHODS(BURROW__METHOD_ENTRY, Dister)};
static const Type ty_dister =
    DESC(NONAME, NONAME, KIND_INTERFACE, sizeof(Dister), _Alignof(Dister), 0, NULL, 1,
         dister_methods, NULL, 0, NULL);

static Int point_dist_self(void *p, Int scale) {
    return point_dist(p, scale);
}

static const DisterVT point_dister_vt = {&burrow_type_Point, point_dist_self};
BURROW_IMPLEMENTS_DESC(point_as_dister, &burrow_type_Point, &ty_dister,
                       point_dister_vt);

/* ValueOf(p) and ValueOf(&p). */
static ReflectValue vpoint(Point *p) {
    return val(TYPE_OF(Point), p);
}

static ReflectValue vpoint_ptr(Point **pp) {
    return val(reflect_pointer_to(TYPE_OF(Point)), pp);
}

/* ------------------------------------------------------------- TestMethod */

static void TestMethod(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));

    /* Non-curried method of type. */
    Point p = {3, 4};
    Point *pp = &p;
    ReflectType tp = TYPE_OF(Point);
    ReflectType tpp = reflect_pointer_to(tp);
    Int i = call_int(reflect_type_method(tp, 1).func, a, VALS(vpoint(&p), vint(a, 10)));
    if (i != 250)
        testing_t_errorf_v(t, "Type Method returned %d; want 250", i);

    ReflectMethod m;
    if (!reflect_type_method_by_name(tp, BURROW_S("Dist"), &m)) {
        testing_t_fatalf_v(t, "method by name failed");
        return;
    }
    i = call_int(m.func, a, VALS(vpoint(&p), vint(a, 11)));
    if (i != 275)
        testing_t_errorf_v(t, "Type MethodByName returned %d; want 275", i);

    if (!reflect_type_method_by_name(tp, BURROW_S("NoArgs"), &m)) {
        testing_t_fatalf_v(t, "method by name failed");
        return;
    }
    Int n = reflect_value_call(m.func, a, VALS(vpoint(&p))).len;
    if (n != 0)
        testing_t_errorf_v(t, "NoArgs returned %d values; want 0", n);

    i = call_int(reflect_type_method(tpp, 1).func, a,
                 VALS(vpoint_ptr(&pp), vint(a, 12)));
    if (i != 300)
        testing_t_errorf_v(t, "Pointer Type Method returned %d; want 300", i);

    if (!reflect_type_method_by_name(tpp, BURROW_S("Dist"), &m)) {
        testing_t_fatalf_v(t, "ptr method by name failed");
        return;
    }
    i = call_int(m.func, a, VALS(vpoint_ptr(&pp), vint(a, 13)));
    if (i != 325)
        testing_t_errorf_v(t, "Pointer Type MethodByName returned %d; want 325", i);

    if (!reflect_type_method_by_name(tpp, BURROW_S("NoArgs"), &m)) {
        testing_t_fatalf_v(t, "method by name failed");
        return;
    }
    n = reflect_value_call(m.func, a, VALS(vpoint_ptr(&pp))).len;
    if (n != 0)
        testing_t_errorf_v(t, "NoArgs returned %d values; want 0", n);

    if (reflect_type_method_by_name(tpp, BURROW_S("AA"), &m))
        testing_t_errorf_v(t, "MethodByName(\"AA\") should have failed");
    if (reflect_type_method_by_name(tpp, BURROW_S("ZZ"), &m))
        testing_t_errorf_v(t, "MethodByName(\"ZZ\") should have failed");

    /* Curried method of value. */
    ReflectType tfunc = reflect_func_of(TYPES(TYPE_INT), TYPES(TYPE_INT), false);
    ReflectValue v = reflect_value_method(vpoint(&p), a, 1);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Value Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 14)));
    if (i != 350)
        testing_t_errorf_v(t, "Value Method returned %d; want 350", i);
    v = reflect_value_method_by_name(vpoint(&p), a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Value MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 15)));
    if (i != 375)
        testing_t_errorf_v(t, "Value MethodByName returned %d; want 375", i);
    v = reflect_value_method_by_name(vpoint(&p), a, BURROW_S("NoArgs"));
    reflect_value_call(v, a, NOVALS);

    /* Curried method of pointer. */
    v = reflect_value_method(vpoint_ptr(&pp), a, 1);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Value Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 16)));
    if (i != 400)
        testing_t_errorf_v(t, "Pointer Value Method returned %d; want 400", i);
    v = reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Value MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 17)));
    if (i != 425)
        testing_t_errorf_v(t, "Pointer Value MethodByName returned %d; want 425", i);
    v = reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("NoArgs"));
    reflect_value_call(v, a, NOVALS);

    /* Curried method of interface value. Have to wrap interface value in a
     * struct to get at it. Passing it to ValueOf directly would access the
     * underlying Point, not the interface. */
    Dister x = {&point_dister_vt, &p};
    ReflectValue pv = settable(&ty_dister, &x);
    v = reflect_value_method(pv, a, 0);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Interface Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 18)));
    if (i != 450)
        testing_t_errorf_v(t, "Interface Method returned %d; want 450", i);
    v = reflect_value_method_by_name(pv, a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Interface MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(v, a, VALS(vint(a, 19)));
    if (i != 475)
        testing_t_errorf_v(t, "Interface MethodByName returned %d; want 475", i);
    arena_free(&ar);
}

/* -------------------------------------------------------- TestMethodValue */

/* struct{ X interface{ Dist(int) int } }. */
static const Field s_dister_fields[] = {FIELD("X", &ty_dister, 0)};
static const Type ty_s_dister =
    DESC(NONAME, NONAME, KIND_STRUCT, sizeof(Dister), _Alignof(Dister), 1,
         s_dister_fields, 0, NULL, NULL, 0, NULL);

static void TestMethodValue(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Point p = {3, 4};
    Point *pp = &p;
    Int i;

    /* Check that method value have the same underlying code pointers. */
    Point p11 = {1, 1}, p22 = {2, 2};
    ReflectValue p1 = reflect_value_method(vpoint(&p11), a, 1);
    ReflectValue p2 = reflect_value_method(vpoint(&p22), a, 1);
    if (reflect_value_pointer(p1) != reflect_value_pointer(p2))
        testing_t_errorf_v(t, "methodValueCall mismatched");

    /* Curried method of value. */
    ReflectType tfunc = reflect_func_of(TYPES(TYPE_INT), TYPES(TYPE_INT), false);
    ReflectValue v = reflect_value_method(vpoint(&p), a, 1);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Value Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 10)));
    if (i != 250)
        testing_t_errorf_v(t, "Value Method returned %d; want 250", i);
    v = reflect_value_method_by_name(vpoint(&p), a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Value MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 11)));
    if (i != 275)
        testing_t_errorf_v(t, "Value MethodByName returned %d; want 275", i);
    v = reflect_value_method_by_name(vpoint(&p), a, BURROW_S("NoArgs"));
    reflect_value_call(reflect_value_of(reflect_value_interface(v)), a, NOVALS);

    /* Curried method of pointer. */
    v = reflect_value_method(vpoint_ptr(&pp), a, 1);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Value Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 12)));
    if (i != 300)
        testing_t_errorf_v(t, "Pointer Value Method returned %d; want 300", i);
    v = reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Value MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 13)));
    if (i != 325)
        testing_t_errorf_v(t, "Pointer Value MethodByName returned %d; want 325", i);
    v = reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("NoArgs"));
    reflect_value_call(reflect_value_of(reflect_value_interface(v)), a, NOVALS);

    /* Curried method of pointer to pointer. */
    Point **ppp = &pp;
    ReflectValue pv = reflect_value_elem(
        val(reflect_pointer_to(reflect_pointer_to(TYPE_OF(Point))), &ppp));
    v = reflect_value_method(pv, a, 1);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Pointer Value Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 14)));
    if (i != 350)
        testing_t_errorf_v(t, "Pointer Pointer Value Method returned %d; want 350", i);
    v = reflect_value_method_by_name(pv, a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Pointer Pointer Value MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 15)));
    if (i != 375)
        testing_t_errorf_v(
            t, "Pointer Pointer Value MethodByName returned %d; want 375", i);

    /* Curried method of interface value. Have to wrap interface value in a
     * struct to get at it. Passing it to ValueOf directly would access the
     * underlying Point, not the interface. */
    struct {
        Dister X;
    } s = {{&point_dister_vt, &p}};
    pv = reflect_value_field(val(&ty_s_dister, &s), 0);
    v = reflect_value_method(pv, a, 0);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Interface Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 16)));
    if (i != 400)
        testing_t_errorf_v(t, "Interface Method returned %d; want 400", i);
    v = reflect_value_method_by_name(pv, a, BURROW_S("Dist"));
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Interface MethodByName Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    i = call_int(reflect_value_of(reflect_value_interface(v)), a, VALS(vint(a, 17)));
    if (i != 425)
        testing_t_errorf_v(t, "Interface MethodByName returned %d; want 425", i);

    /* For issue #33628: method args are not stored at the right offset on
     * amd64p32. Go calls what Interface gives back; here Call does. */
    ReflectValue m64 =
        reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("Int64Method"));
    Slice r = reflect_value_call(m64, a, VALS(box(a, TYPE_INT64, &(int64_t){123})));
    if (reflect_value_int(at(r, 0)) != 123)
        testing_t_errorf_v(t, "Int64Method returned %d; want 123",
                           reflect_value_int(at(r, 0)));
    ReflectValue m32 =
        reflect_value_method_by_name(vpoint_ptr(&pp), a, BURROW_S("Int32Method"));
    r = reflect_value_call(m32, a, VALS(box(a, TYPE_INT32, &(int32_t){456})));
    if (reflect_value_int(at(r, 0)) != 456)
        testing_t_errorf_v(t, "Int32Method returned %d; want 456",
                           reflect_value_int(at(r, 0)));
    arena_free(&ar);
}

/* ------------------------------------------------ TestVariadicMethodValue */

static void TestVariadicMethodValue(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Point p = {3, 4};
    Point points[] = {{20, 21}, {22, 23}, {24, 25}};
    Slice ps = {points, 3, 3, TYPE_OF(Point)};
    Int want = point_total_dist(&p, ps);

    /* Variadic method of type. TotalDist is 6 here, where Go has 4: see Point
     * above. */
    ReflectType tfunc =
        reflect_func_of(TYPES(TYPE_OF(Point), &ty_sl_Point), TYPES(TYPE_INT), true);
    ReflectType tt = reflect_type_method(TYPE_OF(Point), 6).type;
    if (tt != tfunc)
        testing_t_errorf_v(t, "Variadic Method Type from TypeOf is %s; want %s",
                           reflect_type_string(tt), reflect_type_string(tfunc));

    /* Curried method of value. */
    tfunc = reflect_func_of(TYPES(&ty_sl_Point), TYPES(TYPE_INT), true);
    ReflectValue v = reflect_value_method(vpoint(&p), a, 6);
    if (reflect_value_type(v) != tfunc)
        testing_t_errorf_v(t, "Variadic Method Type is %s; want %s",
                           reflect_type_string(reflect_value_type(v)),
                           reflect_type_string(tfunc));
    ReflectValue f = reflect_value_of(reflect_value_interface(v));
    Int i = call_int(f, a,
                     VALS(vpoint(&points[0]), vpoint(&points[1]), vpoint(&points[2])));
    if (i != want)
        testing_t_errorf_v(t, "Variadic Method returned %d; want %d", i, want);
    Slice r = reflect_value_call_slice(f, a, VALS(val(&ty_sl_Point, &ps)));
    i = reflect_value_int(at(r, 0));
    if (i != want)
        testing_t_errorf_v(t, "Variadic Method CallSlice returned %d; want %d", i,
                           want);

    /* Go then calls what Interface gives back, which only Call can do here,
     * and that is what the two calls above were. */
    arena_free(&ar);
}

/* --------------------------------------------------- TestDirectIfaceMethod */

typedef Int *IntPtr;
static const Type burrow_type_IntPtr =
    DESC(NONAME, NONAME, KIND_POINTER, sizeof(Int *), _Alignof(Int *), 0, NULL, 0, NULL,
         TYPE_INT, 0, NULL);

#define DIRECT_IFACE_FIELDS(F, Ty) F(Ty, IntPtr, p, "")
BURROW_STRUCT_DECL(DirectIfaceT, DIRECT_IFACE_FIELDS);

static Int direct_iface_m(DirectIfaceT *d) {
    return *d->p;
}

#define DIRECT_IFACE_SIG_M(IN, OUT) OUT(Int)
#define DIRECT_IFACE_METHODS(Mth, Ty) Mth(Ty, M, direct_iface_m, DIRECT_IFACE_SIG_M)

BURROW_STRUCT_DEFINE_METHODS(DirectIfaceT, DIRECT_IFACE_FIELDS, DIRECT_IFACE_METHODS);

static void TestDirectIfaceMethod(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    Int x = 42;
    DirectIfaceT v = {&x};
    ReflectType typ = TYPE_OF(DirectIfaceT);
    ReflectMethod m;
    if (!reflect_type_method_by_name(typ, BURROW_S("M"), &m)) {
        testing_t_fatalf_v(t, "cannot find method M");
        return;
    }
    Int got = call_int(m.func, a, VALS(val(typ, &v)));
    if (got != 42)
        testing_t_errorf_v(t, "Call with value receiver got %d, want 42", got);

    DirectIfaceT *pv = &v;
    typ = reflect_pointer_to(typ);
    if (!reflect_type_method_by_name(typ, BURROW_S("M"), &m)) {
        testing_t_fatalf_v(t, "cannot find method M");
        return;
    }
    got = call_int(m.func, a, VALS(val(typ, &pv)));
    if (got != 42)
        testing_t_errorf_v(t, "Call with pointer receiver got %d, want 42", got);
    arena_free(&ar);
}

/* -------------------------------------------------------- TestVariadicType */

/* func(x int, y ...float64). */
BURROW_FUNC(VariadicFn, void, Int x, Float64Slice y);
#define VARIADIC_FN_SIG(IN, OUT)                                                       \
    IN(0, Int)                                                                         \
    IN(1, Float64Slice)
BURROW_FUNC_TYPE_DECL(VariadicFn);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(VariadicFn, double, VARIADIC_FN_SIG);

static void TestVariadicType(TestingT *t) {
    /* Test example from Type documentation. */
    ReflectType typ = TYPE_OF(VariadicFn);
    if (reflect_type_num_in(typ) == 2 && reflect_type_in(typ, 0) == TYPE_INT) {
        ReflectType sl = reflect_type_in(typ, 1);
        if (reflect_type_kind(sl) == KIND_SLICE &&
            reflect_type_elem(sl) == TYPE_FLOAT64)
            return; /* ok */
    }

    /* Failed */
    testing_t_errorf_v(t, "want NumIn() = 2, In(0) = int, In(1) = []float64");
    for (Int i = 0; i < reflect_type_num_in(typ); i++)
        testing_t_errorf_v(t, "In(%d) = %s", i,
                           reflect_type_string(reflect_type_in(typ, i)));
}

/* --------------------------------------------------- TestUnexportedMethods */

static void noop_thunk(void *recv, void **args, void **rets) {
    (void)recv;
    (void)args;
    (void)rets;
}

/* type unexp struct{}, with two unexported methods. */
static const Method unexp_methods[] = {
    {S_("f"), NULL, noop_thunk},
    {S_("g"), NULL, noop_thunk},
};
static const Type ty_unexp = DESC(S_("unexp"), TEST_PKG, KIND_STRUCT, 0, 1, 0, NULL, 2,
                                  unexp_methods, NULL, 0, NULL);

/* type unexpI interface { f() (int32, int8) }. */
static const Method unexp_i_methods[] = {{S_("f"), NULL, noop_thunk}};
static const Type ty_unexpI =
    DESC(S_("unexpI"), TEST_PKG, KIND_INTERFACE, sizeof(Iface), _Alignof(Iface), 0,
         NULL, 1, unexp_i_methods, NULL, 0, NULL);

static void TestUnexportedMethods(TestingT *t) {
    ReflectType typ = reflect_pointer_to(&ty_unexp);
    Int got = reflect_type_num_method(typ);
    if (got != 0)
        testing_t_errorf_v(t, "NumMethod=%d, want 0 satisfied methods", got);

    typ = reflect_pointer_to(&ty_unexpI);
    got = reflect_type_num_method(reflect_type_elem(typ));
    if (got != 1)
        testing_t_errorf_v(t, "NumMethod=%d, want 1 satisfied methods", got);
}

/* ------------------------------------------------------ TestNumMethodOnDDD */

/* type FuncDDD func(...any) error, with a method M. */
static const Type ty_sl_any =
    DESC(NONAME, NONAME, KIND_SLICE, sizeof(Slice), _Alignof(Slice), 0, NULL, 0, NULL,
         TYPE_OF(Any), 0, NULL);
static const Field func_ddd_sig[] = {PARAM(&ty_sl_any, 0), PARAM(TYPE_OF(Error), 0)};
static const Type ty_func_ddd_m = FUNC_DESC(0, NULL, NULL, 0, NULL);
static const Method func_ddd_methods[] = {{S_("M"), &ty_func_ddd_m, noop_thunk}};
static const Type ty_FuncDDD =
    DESC(S_("FuncDDD"), TEST_PKG, KIND_FUNC, sizeof(Func), _Alignof(Func), 2,
         func_ddd_sig, 1, func_ddd_methods, TYPE_OF(Any), 1, NULL);

static void TestNumMethodOnDDD(TestingT *t) {
    Func nil = {NULL, NULL};
    ReflectValue rv = val(&ty_FuncDDD, &nil);
    Int n = reflect_value_num_method(rv);
    if (n != 1)
        testing_t_fatalf_v(t, "NumMethod()=%d, want 1", n);
}

/* ------------------------------------------------------------ TestVariadic */

/* fmt.Fprintf, func(w io.Writer, format string, a ...any) (n int, err error),
 * whose two results are more than a signature list can say. */
BURROW_FUNC(FprintfFn, Int, IoWriter w, Str format, Slice args, Error *err);

static Int fprintf_adapter(void *env, IoWriter w, Str format, Slice args, Error *err) {
    (void)env;
    return fmt_fprintf(w, format, args, err);
}

static void fprintf_call(const void *fn, void **args, void **rets) {
    const FprintfFn *f = fn;
    Error err = BURROW_NO_ERROR;
    Int n =
        f->f(f->env, *(IoWriter *)args[0], *(Str *)args[1], *(Slice *)args[2], &err);
    *(Int *)rets[0] = n;
    *(Error *)rets[1] = err;
}

static const TypeOps fprintf_ops = {NULL, NULL, NULL, NULL, fprintf_call};
static const Field fprintf_sig[] = {
    PARAM(TYPE_OF(IoWriter), 0), PARAM(TYPE_STRING, 1),    PARAM(&ty_sl_any, 2),
    PARAM(TYPE_INT, 0),          PARAM(TYPE_OF(Error), 0),
};
static const Type ty_fprintf = FUNC_DESC(5, fprintf_sig, TYPE_OF(Any), 3, &fprintf_ops);

static void TestVariadic(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    StringsBuilder b = STRINGS_BUILDER(a);
    FprintfFn fp = BURROW_FN(FprintfFn, fprintf_adapter, NULL);
    ReflectValue fv = val(&ty_fprintf, &fp);
    Str format = BURROW_S("%s, %d world");
    Str hello = BURROW_S("hello");
    Int n42 = 42;
    StringsBuilder *bp = &b;
    ReflectValue wv = val(reflect_pointer_to(TYPE_STRINGS_BUILDER), &bp);

    strings_builder_reset(&b);
    reflect_value_call(fv, a,
                       VALS(wv, val(TYPE_STRING, &format), val(TYPE_STRING, &hello),
                            val(TYPE_INT, &n42)));
    if (!str_is(strings_builder_string(&b), "hello, 42 world"))
        testing_t_errorf_v(t, "after Fprintf Call: %q != %q",
                           strings_builder_string(&b), "hello 42 world");

    strings_builder_reset(&b);
    Any xs[] = {BURROW_ANY(TYPE_STRING, &hello), BURROW_ANY(TYPE_INT, &n42)};
    Slice sl = {xs, 2, 2, TYPE_ANY};
    reflect_value_call_slice(fv, a,
                             VALS(wv, val(TYPE_STRING, &format), val(&ty_sl_any, &sl)));
    if (!str_is(strings_builder_string(&b), "hello, 42 world"))
        testing_t_errorf_v(t, "after Fprintf CallSlice: %q != %q",
                           strings_builder_string(&b), "hello 42 world");
    arena_free(&ar);
}

/* ------------------------------------------------------------- TestFuncArg */

/* func(i int, f func(int) int) int. */
BURROW_FUNC(ApplyFn, Int, Int i, IntFn f);
#define APPLY_FN_SIG(IN, OUT)                                                          \
    IN(0, Int)                                                                         \
    IN(1, IntFn)                                                                       \
    OUT(Int)
BURROW_FUNC_TYPE_DECL(ApplyFn);
BURROW_FUNC_TYPE_DEFINE(ApplyFn, APPLY_FN_SIG);

static Int apply(void *env, Int i, IntFn f) {
    (void)env;
    return BURROW_CALLF(f, i);
}

static Int plus_one(void *env, Int i) {
    (void)env;
    return i + 1;
}

static void TestFuncArg(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    ApplyFn f1 = BURROW_FN(ApplyFn, apply, NULL);
    IntFn f2 = BURROW_FN(IntFn, plus_one, NULL);
    Int r = call_int(val(TYPE_OF(ApplyFn), &f1), a,
                     VALS(vint(a, 100), val(TYPE_OF(IntFn), &f2)));
    if (r != 101)
        testing_t_errorf_v(t, "function returned %d, want 101", r);
    arena_free(&ar);
}

/* -------------------------------------------------------------- TestFuncOf */

/* type K string, type V float64 and type T1 int. */
typedef Int T1;
static const Type ty_K = DESC(S_("K"), TEST_PKG, KIND_STRING, sizeof(Str),
                              _Alignof(Str), 0, NULL, 0, NULL, NULL, 0, NULL);
static const Type ty_V = DESC(S_("V"), TEST_PKG, KIND_FLOAT64, sizeof(double),
                              _Alignof(double), 0, NULL, 0, NULL, NULL, 0, NULL);
static const Type burrow_type_T1 = DESC(S_("T1"), TEST_PKG, KIND_INT, sizeof(Int),
                                        _Alignof(Int), 0, NULL, 0, NULL, NULL, 0, NULL);

/* The types FuncOf has to make, as descriptors of C function types. */
BURROW_FUNC(FnT1, void, T1 x);
BURROW_FUNC(FnInt, void, Int x);
BURROW_FUNC(FnDDDInt, void, IntSlice x);
BURROW_FUNC(FnIntBool, bool, Int x);
#define FN_T1_SIG(IN, OUT) IN(0, T1)
#define FN_INT_SIG(IN, OUT) IN(0, Int)
#define FN_DDD_INT_SIG(IN, OUT) IN(0, IntSlice)
#define FN_INT_BOOL_SIG(IN, OUT)                                                       \
    IN(0, Int)                                                                         \
    OUT(bool)
BURROW_FUNC_TYPE_DECL(FnT1);
BURROW_FUNC_TYPE_DECL(FnInt);
BURROW_FUNC_TYPE_DECL(FnDDDInt);
BURROW_FUNC_TYPE_DECL(FnIntBool);
BURROW_FUNC_TYPE_DEFINE(FnT1, FN_T1_SIG);
BURROW_FUNC_TYPE_DEFINE(FnInt, FN_INT_SIG);
BURROW_FUNC_TYPE_DEFINE_VARIADIC(FnDDDInt, Int, FN_DDD_INT_SIG);
BURROW_FUNC_TYPE_DEFINE(FnIntBool, FN_INT_BOOL_SIG);

/* func(int) (bool, string), which has two results. */
static const Field fn_int_bool_str_sig[] = {PARAM(TYPE_INT, 0), PARAM(TYPE_BOOL, 0),
                                            PARAM(TYPE_STRING, 0)};
static const Type ty_fn_int_bool_str = FUNC_DESC(3, fn_int_bool_str_sig, NULL, 1, NULL);

static Slice func_of_impl(void *env, Slice args) {
    ImplEnv *e = env;
    TestingT *t = e->t;
    if (args.len != 1)
        testing_t_errorf_v(t, "args has %d values, want exactly one arg", args.len);
    else if (reflect_value_type(at(args, 0)) != &ty_K)
        testing_t_errorf_v(t, "args[0] is type %s, want %s",
                           reflect_type_string(reflect_value_type(at(args, 0))),
                           reflect_type_string(&ty_K));
    else if (!str_is(reflect_value_string(at(args, 0)), "gopher"))
        testing_t_errorf_v(t, "args[0] = %q, want %q",
                           reflect_value_string(at(args, 0)), "gopher");
    e->x = 3.14;
    e->out[0] = val(&ty_V, &e->x);
    return slice_from(e->out, 1, 1, TYPE_REFLECT_VALUE);
}

typedef struct FuncOfCase {
    ReflectType x;
    ReflectType want;
} FuncOfCase;

static void func_of_panic(void *env) {
    const Slice *in = env;
    reflect_func_of(*in, NOTYPES, true);
}

static void TestFuncOf(TestingT *t) {
    Arena ar;
    Alloc *a = arena_allocator(arena_new(&ar));
    /* check construction and use of type not in binary */
    ImplEnv e = {t, a, 0, 0, {{0}}};
    ReflectValue v =
        reflect_make_func(a, reflect_func_of(TYPES(&ty_K), TYPES(&ty_V), false),
                          BURROW_FN(ReflectMakeFuncImpl, func_of_impl, &e));
    Str gopher = BURROW_S("gopher");
    Slice outs = reflect_value_call(v, a, VALS(val(&ty_K, &gopher)));
    if (outs.len != 1) {
        testing_t_fatalf_v(t, "v.Call returned %d values, want exactly one result",
                           outs.len);
        arena_free(&ar);
        return;
    }
    if (reflect_value_type(at(outs, 0)) != &ty_V) {
        testing_t_fatalf_v(t, "c.Call[0] is type %s, want %s",
                           reflect_type_string(reflect_value_type(at(outs, 0))),
                           reflect_type_string(&ty_V));
        arena_free(&ar);
        return;
    }
    double f = reflect_value_float(at(outs, 0));
    if (f != 3.14)
        testing_t_errorf_v(t, "constructed func returned %f, want %f", f, 3.14);

    /* check that types already in binary are found */
    FuncOfCase cases[] = {
        {reflect_func_of(TYPES(TYPE_OF(T1)), NOTYPES, false), TYPE_OF(FnT1)},
        {reflect_func_of(TYPES(TYPE_INT), NOTYPES, false), TYPE_OF(FnInt)},
        {reflect_func_of(TYPES(reflect_slice_of(TYPE_INT)), NOTYPES, true),
         TYPE_OF(FnDDDInt)},
        {reflect_func_of(TYPES(TYPE_INT), TYPES(TYPE_BOOL), false), TYPE_OF(FnIntBool)},
        {reflect_func_of(TYPES(TYPE_INT), TYPES(TYPE_BOOL, TYPE_STRING), false),
         &ty_fn_int_bool_str},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        check_same_type(t, cases[i].x, cases[i].want);

    /* check that variadic requires last element be a slice. */
    reflect_func_of(TYPES(TYPE_INT, TYPE_STRING, reflect_slice_of(TYPE_BOOL)), NOTYPES,
                    true);
    Slice in = TYPES(TYPE_INT, TYPE_STRING, TYPE_BOOL);
    should_panic(t, "must be slice", func_of_panic, &in);
    in = NOTYPES;
    should_panic(t, "must be slice", func_of_panic, &in);

    /* testcase for #54669 */
    ReflectType many[51];
    for (size_t i = 0; i < 51; i++)
        many[i] = TYPE_INT;
    reflect_func_of(slice_from(many, 51, 51, TYPE_UNSAFE_POINTER), NOTYPES, false);
    arena_free(&ar);
}

/* ---------------------------------------- TestMethodByNameUnExportedFirst */

/* type UnExportedFirst int, with methods ΦExported and unexported, which
 * sorts first, since Φ is above every ASCII letter. */
static const Type ty_noargs = FUNC_DESC(0, NULL, NULL, 0, NULL);
static const Method unexported_first_methods[] = {
    {S_("unexported"), &ty_noargs, noop_thunk},
    {S_("\xce\xa6"
        "Exported"),
     &ty_noargs, noop_thunk},
};
static const Type ty_UnExportedFirst =
    DESC(S_("UnExportedFirst"), TEST_PKG, KIND_INT, sizeof(Int), _Alignof(Int), 0, NULL,
         2, unexported_first_methods, NULL, 0, NULL);

static void TestMethodByNameUnExportedFirst(TestingT *t) {
    ReflectMethod m = {0};
    reflect_type_method_by_name(&ty_UnExportedFirst,
                                BURROW_S("\xce\xa6"
                                         "Exported"),
                                &m);
    if (!str_is(m.name, "\xce\xa6"
                        "Exported"))
        testing_t_errorf_v(t,
                           "got %s, expected \xce\xa6"
                           "Exported",
                           m.name);
}

#define TESTS(X)                                                                       \
    X(TestFunc)                                                                        \
    X(TestCallWithStruct)                                                              \
    X(TestMakeFunc)                                                                    \
    X(TestMakeFuncInterface)                                                           \
    X(TestMakeFuncVariadic)                                                            \
    X(TestMakeFuncValidReturnAssignments)                                              \
    X(TestMakeFuncInvalidReturnAssignments)                                            \
    X(TestMethod)                                                                      \
    X(TestMethodValue)                                                                 \
    X(TestVariadicMethodValue)                                                         \
    X(TestDirectIfaceMethod)                                                           \
    X(TestVariadicType)                                                                \
    X(TestUnexportedMethods)                                                           \
    X(TestNumMethodOnDDD)                                                              \
    X(TestVariadic)                                                                    \
    X(TestFuncArg)                                                                     \
    X(TestFuncOf)                                                                      \
    X(TestMethodByNameUnExportedFirst)
TESTING_MAIN(TESTS)
