/* Derived from Go's src/go/types/instantiate_test.go and api_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Go's tests type check a little source and look up what they need in the
 * package scope. There is no checker yet, so these build the types the checker
 * would make for that source, which is kept next to each builder. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#include <string.h>

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------------- builders */

/* What the types are built with: the allocator, the package, and the context
 * the checker would instantiate in. */
typedef struct World {
    Alloc *a;
    TypesPackage *pkg;
    TypesContext *ctxt;
} World;

static World world(Alloc *a, Str name) {
    World w = {a, types_new_package(a, name, name), types_new_context(a)};
    return w;
}

static Slice none(void) {
    return slice_nil(TYPE_UNSAFE_POINTER);
}

static Slice ptrs1(World *w, void *p) {
    Slice s = burrow__types_ptrs_make(w->a, 1);
    GT_AT(s, 0, void *) = p;
    return s;
}

static Slice ptrs2(World *w, void *p, void *q) {
    Slice s = burrow__types_ptrs_make(w->a, 2);
    GT_AT(s, 0, void *) = p;
    GT_AT(s, 1, void *) = q;
    return s;
}

static TypesType basic(TypesBasicKind k) {
    return types_type(types_typ[k]);
}

static TypesType any_type(void) {
    return types_object_type(burrow__types_universe_any());
}

static TypesType comparable_type(void) {
    return types_object_type(burrow__types_universe_comparable());
}

static TypesTypeName *tname(World *w, Str name) {
    return types_new_type_name(w->a, TOKEN_NO_POS, w->pkg, name, NULL);
}

static TypesNamed *named(World *w, Str name, TypesType u) {
    return types_new_named(w->a, tname(w, name), u, none());
}

static TypesType named_t(World *w, Str name, TypesType u) {
    return types_type(named(w, name, u));
}

static TypesType ptr(World *w, TypesType t) {
    return types_type(types_new_pointer(w->a, t));
}

static TypesType slice_of(World *w, TypesType t) {
    return types_type(types_new_slice(w->a, t));
}

static TypesTypeParam *tparam(World *w, Str name, TypesType constraint) {
    return types_new_type_param(w->a, tname(w, name), constraint);
}

static TypesVar *param(World *w, Str name, TypesType typ) {
    return types_new_param(w->a, TOKEN_NO_POS, w->pkg, name, typ);
}

/* a tuple of unnamed parameters of the n types in ts */
static TypesTuple *tuple(World *w, const TypesType *ts, Int n) {
    if (n == 0)
        return NULL;
    Slice s = burrow__types_ptrs_make(w->a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(s, i, TypesVar *) = param(w, S(""), ts[i]);
    return types_new_tuple(w->a, s);
}

static TypesType iface(World *w, Slice methods, Slice embeddeds) {
    return types_type(types_new_interface_type(w->a, methods, embeddeds));
}

/* interface{ e } */
static TypesType iface1(World *w, TypesType e) {
    return iface(w, none(), ptrs1(w, e));
}

/* ~t, or t, as a union of one term */
static TypesType union1(World *w, bool tilde, TypesType t) {
    return types_type(types_new_union(w->a, ptrs1(w, types_new_term(w->a, tilde, t))));
}

/* x | y */
static TypesType union2(World *w, TypesType x, TypesType y) {
    return types_type(types_new_union(w->a, ptrs2(w, types_new_term(w->a, false, x),
                                                  types_new_term(w->a, false, y))));
}

/* func name() with the n params in ps, unnamed, and no results */
static TypesFunc *func0(World *w, TypesPackage *pkg, Str name, TypesVar *recv,
                        const TypesType *ps, Int n) {
    TypesSignature *sig = types_new_signature(w->a, recv, tuple(w, ps, n), NULL, false);
    return types_new_func(w->a, TOKEN_NO_POS, pkg, name, sig);
}

/* func (recv) name(ps), declared on the named type n */
static void add_method(World *w, TypesNamed *n, bool ptr_recv, Str name,
                       const TypesType *ps, Int np) {
    TypesType rt = ptr_recv ? ptr(w, types_type(n)) : types_type(n);
    types_named_add_method(n, func0(w, w->pkg, name, param(w, S(""), rt), ps, np));
}

/* type I interface{ m() } */
static TypesType iface_i(World *w) {
    TypesFunc *m = func0(w, w->pkg, S("m"), NULL, NULL, 0);
    return named_t(w, S("I"), iface(w, ptrs1(w, m), none()));
}

/* type name[tps] u, where u is set by the caller once the type parameters
 * are there */
static TypesNamed *generic(World *w, Str name, Slice tps) {
    TypesNamed *n = named(w, name, NULL);
    types_named_set_type_params(n, tps);
    return n;
}

/* type T[P C] int */
static TypesType generic1(World *w, TypesType c) {
    TypesNamed *n = generic(w, S("T"), ptrs1(w, tparam(w, S("P"), c)));
    types_named_set_underlying(n, basic(TYPES_INT));
    return types_type(n);
}

/* type T[P1 C1, P2 C2] int */
static TypesType generic2(World *w, TypesTypeParam *p1, TypesTypeParam *p2) {
    TypesNamed *n = generic(w, S("T"), ptrs2(w, p1, p2));
    types_named_set_underlying(n, basic(TYPES_INT));
    return types_type(n);
}

/* orig[targ], in the checker's context */
static TypesType inst(World *w, TypesType orig, TypesType targ) {
    return burrow__types_instance(w->a, TOKEN_NO_POS, orig, ptrs1(w, targ), NULL,
                                  w->ctxt);
}

/* func name[tp any](params) results */
static TypesType generic_func(World *w, Str name, TypesTypeParam *tp,
                              TypesTuple *params, TypesTuple *results) {
    (void)name;
    return types_type(types_new_signature_type(w->a, NULL, none(), ptrs1(w, tp), params,
                                               results, false));
}

static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

/* ---------------------------------------------------------- TestInstantiate */

static void TestInstantiate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = world(a, S("p"));

    /* type T[P any] *T[P] */
    TypesTypeParam *P = tparam(&w, S("P"), any_type());
    TypesNamed *T = generic(&w, S("T"), ptrs1(&w, P));
    types_named_set_underlying(T, ptr(&w, inst(&w, types_type(T), types_type(P))));

    /* type T should have one type parameter */
    Int n = types_type_param_list_len(types_named_type_params(T));
    if (n != 1)
        testing_t_fatalf_v(t, "expected 1 type parameter; found %d", n);

    /* instantiation should succeed (no endless recursion) even with a nil
     * *Checker */
    Error err = BURROW_NO_ERROR;
    TypesType res = types_instantiate(a, NULL, types_type(T),
                                      ptrs1(&w, basic(TYPES_INT)), false, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    /* instantiated type should point to itself */
    TypesType p = types_pointer_elem(types_as_pointer(types_type_underlying(res)));
    if (p != res) {
        Str rs = type_string(a, res), ps = type_string(a, p);
        testing_t_fatalf_v(t, "unexpected result type: %s points to %s", rs, ps);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------- TestInstantiateErrors */

/* Each builder makes T, by convention the type being instantiated, and the
 * type arguments, from the source kept beside it. */
typedef void (*BuildFunc)(World *w, TypesType *T, Slice *targs);

/* type T[P interface{~string}] int, with int */
static void ec_tilde_string(World *w, TypesType *T, Slice *targs) {
    *T = generic1(w, iface1(w, union1(w, true, basic(TYPES_STRING))));
    *targs = ptrs1(w, basic(TYPES_INT));
}

/* type T[P1 interface{int}, P2 interface{~string}] int, with int, int */
static void ec_two(World *w, TypesType *T, Slice *targs) {
    TypesTypeParam *p1 = tparam(w, S("P1"), iface1(w, basic(TYPES_INT)));
    TypesTypeParam *p2 =
        tparam(w, S("P2"), iface1(w, union1(w, true, basic(TYPES_STRING))));
    *T = generic2(w, p1, p2);
    *targs = ptrs2(w, basic(TYPES_INT), basic(TYPES_INT));
}

/* type T[P1 any, P2 interface{~[]P1}] int, with int, []string */
static void ec_slice_p1(World *w, TypesType *T, Slice *targs) {
    TypesTypeParam *p1 = tparam(w, S("P1"), any_type());
    TypesType c = iface1(w, union1(w, true, slice_of(w, types_type(p1))));
    TypesTypeParam *p2 = tparam(w, S("P2"), c);
    *T = generic2(w, p1, p2);
    *targs = ptrs2(w, basic(TYPES_INT), slice_of(w, basic(TYPES_STRING)));
}

/* type T[P1 interface{~[]P2}, P2 any] int, with []string, int */
static void ec_slice_p2(World *w, TypesType *T, Slice *targs) {
    TypesTypeParam *p1 = tparam(w, S("P1"), NULL);
    TypesTypeParam *p2 = tparam(w, S("P2"), any_type());
    types_type_param_set_constraint(
        p1, iface1(w, union1(w, true, slice_of(w, types_type(p2)))));
    *T = generic2(w, p1, p2);
    *targs = ptrs2(w, slice_of(w, basic(TYPES_STRING)), basic(TYPES_INT));
}

/* type I interface{ m() }; type N2 int; func (*N2) m() {}; type T[P I] int,
 * with N2 */
static void ec_ptr_recv(World *w, TypesType *T, Slice *targs) {
    TypesType I = iface_i(w);
    TypesNamed *n2 = named(w, S("N2"), basic(TYPES_INT));
    add_method(w, n2, true, S("m"), NULL, 0);
    *T = generic1(w, I);
    *targs = ptrs1(w, n2);
}

/* type I interface{ m() }; type N3 int; func (N3) m(int) {}; type T[P I] int,
 * with N3 */
static void ec_wrong_sig(World *w, TypesType *T, Slice *targs) {
    TypesType I = iface_i(w);
    TypesNamed *n3 = named(w, S("N3"), basic(TYPES_INT));
    TypesType ps[] = {basic(TYPES_INT)};
    add_method(w, n3, false, S("m"), ps, NELEM(ps));
    *T = generic1(w, I);
    *targs = ptrs1(w, n3);
}

/* type I interface{ m() }; type T[P I] int, with int */
static void ec_missing(World *w, TypesType *T, Slice *targs) {
    *T = generic1(w, iface_i(w));
    *targs = ptrs1(w, basic(TYPES_INT));
}

/* type I interface{ m() }; type N5 int; func (N5) M() {}; type T[P I] int,
 * with N5 */
static void ec_wrong_name(World *w, TypesType *T, Slice *targs) {
    TypesType I = iface_i(w);
    TypesNamed *n5 = named(w, S("N5"), basic(TYPES_INT));
    add_method(w, n5, false, S("M"), NULL, 0);
    *T = generic1(w, I);
    *targs = ptrs1(w, n5);
}

/* type I interface{ m() }; type S struct{ m int }; type T[P I] int, with S */
static void ec_field(World *w, TypesType *T, Slice *targs) {
    TypesType I = iface_i(w);
    TypesVar *f =
        types_new_field(w->a, TOKEN_NO_POS, w->pkg, S("m"), basic(TYPES_INT), false);
    TypesType s = types_type(types_new_struct(w->a, ptrs1(w, f), none()));
    *T = generic1(w, I);
    *targs = ptrs1(w, named(w, S("S"), s));
}

/* type MyInt int; type T[P interface{ int }] int, with MyInt */
static void ec_missing_tilde(World *w, TypesType *T, Slice *targs) {
    TypesType my = named_t(w, S("MyInt"), basic(TYPES_INT));
    *T = generic1(w, iface1(w, basic(TYPES_INT)));
    *targs = ptrs1(w, my);
}

/* type T[P interface{ int; string }] int, with int */
static void ec_empty(World *w, TypesType *T, Slice *targs) {
    TypesType c = iface(w, none(), ptrs2(w, basic(TYPES_INT), basic(TYPES_STRING)));
    *T = generic1(w, c);
    *targs = ptrs1(w, basic(TYPES_INT));
}

/* type T[P comparable] int, with []int */
static void ec_comparable(World *w, TypesType *T, Slice *targs) {
    *T = generic1(w, comparable_type());
    *targs = ptrs1(w, slice_of(w, basic(TYPES_INT)));
}

/* type T[P interface{ int }] int, with interface{int | string} */
static void ec_iface_subset(World *w, TypesType *T, Slice *targs) {
    *T = generic1(w, iface1(w, basic(TYPES_INT)));
    TypesType u = union2(w, basic(TYPES_INT), basic(TYPES_STRING));
    *targs = ptrs1(w, iface1(w, u));
}

/* type I interface{ m() }; type T[P interface{ *I }] int, with int */
static void ec_ptr_iface(World *w, TypesType *T, Slice *targs) {
    TypesType I = iface_i(w);
    *T = generic1(w, iface1(w, ptr(w, I)));
    *targs = ptrs1(w, basic(TYPES_INT));
}

/* type T[P any] int, with int, int */
static void ec_count(World *w, TypesType *T, Slice *targs) {
    *T = generic1(w, any_type());
    *targs = ptrs2(w, basic(TYPES_INT), basic(TYPES_INT));
}

/* type T int, with int */
static void ec_non_generic(World *w, TypesType *T, Slice *targs) {
    *T = named_t(w, S("T"), basic(TYPES_INT));
    *targs = ptrs1(w, basic(TYPES_INT));
}

/* func F[P any]() {}, with int, int */
static void ec_func_count(World *w, TypesType *T, Slice *targs) {
    *T = generic_func(w, S("F"), tparam(w, S("P"), any_type()), NULL, NULL);
    *targs = ptrs2(w, basic(TYPES_INT), basic(TYPES_INT));
}

typedef struct ErrorTest {
    BuildFunc build;
    Int want_at; /* the ArgumentError index, or -1 for a plain error */
    const char *want;
} ErrorTest;

/* Go's four, which only check the index. */
static const ErrorTest go_error_tests[] = {
    {ec_tilde_string, 0, NULL},
    {ec_two, 1, NULL},
    {ec_slice_p1, 1, NULL},
    {ec_slice_p2, 0, NULL},
};

static Error must_fail(TestingT *t, World *w, BuildFunc build, TypesType *T) {
    Slice targs;
    build(w, T, &targs);
    Error err = BURROW_NO_ERROR;
    TypesType res = types_instantiate(w->a, NULL, *T, targs, true, &err);
    if (BURROW_OK(err)) {
        Str ts = type_string(w->a, *T);
        testing_t_fatalf_v(t, "Instantiate(%s) returned nil error, want non-nil", ts);
    }
    if (res != NULL)
        testing_t_errorf_v(t, "Instantiate returned an error and a type");
    return err;
}

static void TestInstantiateErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(go_error_tests); i++) {
        World w = world(a, S("p"));
        TypesType T;
        Error err = must_fail(t, &w, go_error_tests[i].build, &T);
        const TypesArgumentError *ae = errors_as(err, TYPE_TYPES_ARGUMENT_ERROR);
        Str ts = type_string(a, T);
        if (ae == NULL)
            testing_t_fatalf_v(t, "Instantiate(%s): error is not an *ArgumentError",
                               ts);
        else if (ae->index != go_error_tests[i].want_at)
            testing_t_errorf_v(t, "Instantiate(%s): error at index %d, want index %d",
                               ts, ae->index, go_error_tests[i].want_at);
    }
    arena_free(&ar);
}

/* ------------------------------------------- TestArgumentErrorUnwrapping */

static void TestArgumentErrorUnwrapping(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesError te = {.fset = NULL, .pos = TOKEN_NO_POS, .msg = S("test")};
    TypesArgumentError ae = {1, types_error_as_error(&te, a)};
    Error err = types_argument_error_as_error(&ae, a);
    const TypesError *e = errors_as(err, TYPE_TYPES_ERROR);
    if (e == NULL)
        testing_t_fatalf_v(t, "error %s does not wrap types.Error", error_text(err));
    else if (!str_eq(e->msg, S("test")))
        testing_t_errorf_v(t, "e.Msg = %q, want %q", e->msg, "test");
    arena_free(&ar);
}

/* ----------------------------------------------------------- TestImplements */

static void TestImplements(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = world(a, S("p"));

    /* type EmptyIface interface{} */
    TypesInterface *EmptyIface = types_as_interface(
        types_type_underlying(named_t(&w, S("EmptyIface"), iface(&w, none(), none()))));
    /* type I interface { m() } */
    TypesType I = iface_i(&w);
    TypesInterface *II = types_as_interface(types_type_underlying(I));
    /* type C interface { m(); ~int } */
    TypesFunc *cm = func0(&w, w.pkg, S("m"), NULL, NULL, 0);
    TypesType C = named_t(
        &w, S("C"),
        iface(&w, ptrs1(&w, cm), ptrs1(&w, union1(&w, true, basic(TYPES_INT)))));
    TypesInterface *CI = types_as_interface(types_type_underlying(C));
    /* type Integer interface{ int8 | int16 | int32 | int64 } */
    TypesTerm *ints[] = {
        types_new_term(a, false, basic(TYPES_INT8)),
        types_new_term(a, false, basic(TYPES_INT16)),
        types_new_term(a, false, basic(TYPES_INT32)),
        types_new_term(a, false, basic(TYPES_INT64)),
    };
    Slice terms = burrow__types_ptrs_make(a, NELEM(ints));
    for (Int i = 0; i < NELEM(ints); i++)
        GT_AT(terms, i, TypesTerm *) = ints[i];
    TypesType IntegerN =
        named_t(&w, S("Integer"), iface1(&w, types_type(types_new_union(a, terms))));
    TypesInterface *Integer = types_as_interface(types_type_underlying(IntegerN));
    /* type EmptyTypeSet interface{ Integer; ~string } */
    TypesType ets =
        iface(&w, none(), ptrs2(&w, IntegerN, union1(&w, true, basic(TYPES_STRING))));
    TypesType EmptyTypeSetN = named_t(&w, S("EmptyTypeSet"), ets);
    TypesInterface *EmptyTypeSet =
        types_as_interface(types_type_underlying(EmptyTypeSetN));
    /* type N1 int; func (N1) m() {} */
    TypesNamed *n1 = named(&w, S("N1"), basic(TYPES_INT));
    add_method(&w, n1, false, S("m"), NULL, 0);
    TypesType N1 = types_type(n1), N1p = ptr(&w, N1);
    /* type N2 int; func (*N2) m() {} */
    TypesNamed *n2 = named(&w, S("N2"), basic(TYPES_INT));
    add_method(&w, n2, true, S("m"), NULL, 0);
    TypesType N2 = types_type(n2), N2p = ptr(&w, N2);
    /* type N3 int; func (N3) m(int) {} */
    TypesNamed *n3 = named(&w, S("N3"), basic(TYPES_INT));
    TypesType ps[] = {basic(TYPES_INT)};
    add_method(&w, n3, false, S("m"), ps, NELEM(ps));
    TypesType N3 = types_type(n3);
    /* type N4 string; func (N4) m() */
    TypesNamed *n4 = named(&w, S("N4"), basic(TYPES_STRING));
    add_method(&w, n4, false, S("m"), NULL, 0);
    TypesType N4 = types_type(n4);
    /* type Bad Bad // invalid type */
    TypesType Bad = named_t(&w, S("Bad"), basic(TYPES_INVALID));

    struct {
        TypesType V;
        TypesInterface *T;
        bool want;
    } tests[] = {
        {I, II, true},
        {I, CI, false},
        {C, II, true},
        {C, CI, true},
        {basic(TYPES_INT8), Integer, true},
        {basic(TYPES_INT64), Integer, true},
        {basic(TYPES_STRING), Integer, false},
        {EmptyTypeSetN, II, true},
        {EmptyTypeSetN, EmptyTypeSet, true},
        {basic(TYPES_INT), EmptyTypeSet, false},
        {N1, II, true},
        {N1, CI, true},
        {N1p, II, true},
        {N1p, CI, false},
        {N2, II, false},
        {N2, CI, false},
        {N2p, II, true},
        {N2p, CI, false},
        {N3, II, false},
        {N3, CI, false},
        {N4, II, true},
        {N4, CI, false},
        {Bad, II, false},
        {Bad, CI, false},
        {Bad, EmptyIface, true},
    };

    for (Int i = 0; i < NELEM(tests); i++) {
        bool got = types_implements(tests[i].V, tests[i].T);
        if (got != tests[i].want) {
            Str vs = type_string(a, tests[i].V);
            Str ts = type_string(a, types_type(tests[i].T));
            testing_t_errorf_v(t, "Implements(%s, %s) = %t, want %t", vs, ts, got,
                               tests[i].want);
        }

        /* The type assertion x.(T) is valid if T is an interface or if T
         * implements the type of x. The assertion is never valid if T is a bad
         * type. */
        TypesInterface *V = tests[i].T;
        TypesType T = tests[i].V;
        bool want = false;
        bool is_iface = types_as_interface(types_type_underlying(T)) != NULL;
        if ((is_iface || types_implements(T, V)) && T != Bad)
            want = true;
        bool got2 = types_assertable_to(V, T);
        if (got2 != want) {
            Str vs = type_string(a, types_type(V)), ts = type_string(a, T);
            testing_t_errorf_v(t, "AssertableTo(%s, %s) = %t, want %t", vs, ts, got2,
                               want);
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------- TestInstantiateEquality */

/* The two generic types each case instantiates, and the type argument for
 * each. */
typedef struct Pair {
    TypesType T1, T2;
    TypesType targ1, targ2;
} Pair;

/* package basictype; type T[P any] int */
static Pair eq_basictype(World *w) {
    TypesType T = generic1(w, any_type());
    return (Pair){T, T, basic(TYPES_INT), basic(TYPES_INT)};
}

/* package differenttypeargs; type T[P any] int */
static Pair eq_differenttypeargs(World *w) {
    TypesType T = generic1(w, any_type());
    return (Pair){T, T, basic(TYPES_INT), basic(TYPES_STRING)};
}

/* package typeslice; type T[P any] int */
static Pair eq_typeslice(World *w) {
    TypesType T = generic1(w, any_type());
    return (Pair){T, T, slice_of(w, basic(TYPES_INT)), slice_of(w, basic(TYPES_INT))};
}

/* M(), with Go's NewFunc(nopos, nil, "M", emptySignature) */
static TypesFunc *method_m(World *w, TypesSignature *empty) {
    return types_new_func(w->a, TOKEN_NO_POS, NULL, S("M"), empty);
}

/* package equivalentinterfaces; type T[P any] int, with
 * interface{M()} and interface{interface{M()}} */
static Pair eq_equivalentinterfaces(World *w) {
    TypesType T = generic1(w, any_type());
    TypesSignature *empty =
        types_new_signature_type(w->a, NULL, none(), none(), NULL, NULL, false);
    TypesType i1 = iface(w, ptrs1(w, method_m(w, empty)), none());
    TypesType inner = iface(w, ptrs1(w, method_m(w, empty)), none());
    TypesType i2 = iface(w, none(), ptrs1(w, inner));
    return (Pair){T, T, i1, i2};
}

/* package equivalenttypesets; type T[P any] int, with interface{int|string}
 * and interface{string|int} */
static Pair eq_equivalenttypesets(World *w) {
    TypesType T = generic1(w, any_type());
    TypesType i1 = iface1(w, union2(w, basic(TYPES_INT), basic(TYPES_STRING)));
    TypesType i2 = iface1(w, union2(w, basic(TYPES_STRING), basic(TYPES_INT)));
    return (Pair){T, T, i1, i2};
}

/* func name[P any]() {} */
static TypesType func_any(World *w, Str name, Str tp) {
    return generic_func(w, name, tparam(w, tp, any_type()), NULL, NULL);
}

/* package basicfunc; func F[P any]() {} */
static Pair eq_basicfunc(World *w) {
    TypesType F = func_any(w, S("F"), S("P"));
    return (Pair){F, F, basic(TYPES_INT), basic(TYPES_INT)};
}

/* package funcslice; func F[P any]() {} */
static Pair eq_funcslice(World *w) {
    TypesType F = func_any(w, S("F"), S("P"));
    return (Pair){F, F, slice_of(w, basic(TYPES_INT)), slice_of(w, basic(TYPES_INT))};
}

/* func F[P any](x string) float64 { return 0 } */
static TypesType func_string_float(World *w) {
    TypesVar *x = param(w, S("x"), basic(TYPES_STRING));
    TypesType r = basic(TYPES_FLOAT64);
    return generic_func(w, S("F"), tparam(w, S("P"), any_type()),
                        types_new_tuple(w->a, ptrs1(w, x)), tuple(w, &r, 1));
}

/* package funcwithparams; func F[P any](x string) float64 { return 0 } */
static Pair eq_funcwithparams(World *w) {
    TypesType F = func_string_float(w);
    return (Pair){F, F, basic(TYPES_INT), basic(TYPES_INT)};
}

/* package differentfuncargs; func F[P any](x string) float64 { return 0 } */
static Pair eq_differentfuncargs(World *w) {
    TypesType F = func_string_float(w);
    return (Pair){F, F, basic(TYPES_INT), basic(TYPES_STRING)};
}

/* func name[tp any](x int) */
static TypesType func_int(World *w, Str name, Str tp) {
    TypesVar *x = param(w, S("x"), basic(TYPES_INT));
    return generic_func(w, name, tparam(w, tp, any_type()),
                        types_new_tuple(w->a, ptrs1(w, x)), NULL);
}

/* package funcequality; func F1[P any](x int) {}; func F2[Q any](x int) {} */
static Pair eq_funcequality(World *w) {
    TypesType F1 = func_int(w, S("F1"), S("P"));
    TypesType F2 = func_int(w, S("F2"), S("Q"));
    return (Pair){F1, F2, basic(TYPES_INT), basic(TYPES_INT)};
}

/* func name[tp any](x tp) */
static TypesType func_self(World *w, Str name, Str tp) {
    TypesTypeParam *p = tparam(w, tp, any_type());
    TypesVar *x = param(w, S("x"), types_type(p));
    return generic_func(w, name, p, types_new_tuple(w->a, ptrs1(w, x)), NULL);
}

/* package funcsymmetry; func F1[P any](x P) {}; func F2[Q any](x Q) {} */
static Pair eq_funcsymmetry(World *w) {
    TypesType F1 = func_self(w, S("F1"), S("P"));
    TypesType F2 = func_self(w, S("F2"), S("Q"));
    return (Pair){F1, F2, basic(TYPES_INT), basic(TYPES_INT)};
}

typedef struct EqualityTest {
    const char *pkg;
    Pair (*build)(World *w);
    bool want_equal;
} EqualityTest;

static const EqualityTest equality_tests[] = {
    {"basictype", eq_basictype, true},
    {"differenttypeargs", eq_differenttypeargs, false},
    {"typeslice", eq_typeslice, true},
    {"equivalentinterfaces", eq_equivalentinterfaces, true},
    {"equivalenttypesets", eq_equivalenttypesets, true},
    {"basicfunc", eq_basicfunc, true},
    {"funcslice", eq_funcslice, true},
    {"funcwithparams", eq_funcwithparams, true},
    {"differentfuncargs", eq_differentfuncargs, false},
    {"funcequality", eq_funcequality, false},
    {"funcsymmetry", eq_funcsymmetry, false},
};

typedef struct EqualityRun {
    Alloc *a;
    const EqualityTest *test;
} EqualityRun;

static void run_equality(void *env, TestingT *t) {
    EqualityRun *r = env;
    World w = world(r->a, str_from_cstr(r->test->pkg));
    Pair p = r->test->build(&w);
    TypesContext *ctxt = types_new_context(r->a);

    Error err = BURROW_NO_ERROR;
    TypesType res1 =
        types_instantiate(r->a, ctxt, p.T1, ptrs1(&w, p.targ1), false, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    TypesType res2 =
        types_instantiate(r->a, ctxt, p.T2, ptrs1(&w, p.targ2), false, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    bool got_equal = res1 == res2;
    if (got_equal != r->test->want_equal) {
        Str s1 = type_string(r->a, res1), s2 = type_string(r->a, res2);
        testing_t_errorf_v(t, "%s == %s: %t, want %t", s1, s2, got_equal,
                           r->test->want_equal);
    }
}

static void TestInstantiateEquality(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < NELEM(equality_tests); i++) {
        EqualityRun r = {arena_allocator(&ar), &equality_tests[i]};
        testing_t_run(t, str_from_cstr(equality_tests[i].pkg),
                      BURROW_FN(TestingTFunc, run_equality, &r));
    }
    arena_free(&ar);
}

/* ---------------------------------------------- TestInstantiateNonEquality */

static void TestInstantiateNonEquality(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* package p; type T[P any] int, type checked twice */
    World w1 = world(a, S("p")), w2 = world(a, S("p"));
    TypesType T1 = generic1(&w1, any_type());
    TypesType T2 = generic1(&w2, any_type());
    /* T1 and T2 are distinct types, so their instances should not be
     * deduplicated by the context. */
    TypesContext *ctxt = types_new_context(a);
    Error err = BURROW_NO_ERROR;
    TypesType res1 =
        types_instantiate(a, ctxt, T1, ptrs1(&w1, basic(TYPES_INT)), false, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    TypesType res2 =
        types_instantiate(a, ctxt, T2, ptrs1(&w2, basic(TYPES_INT)), false, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Str s1 = type_string(a, res1), s2 = type_string(a, res2);
    if (res1 == res2)
        testing_t_errorf_v(
            t,
            "instance from pkg1 (%s) is pointer-equivalent to instance from "
            "pkg2 (%s)",
            s1, s2);
    if (types_identical(res1, res2))
        testing_t_errorf_v(
            t, "instance from pkg1 (%s) is identical to instance from pkg2 (%s)", s1,
            s2);
    arena_free(&ar);
}

/* -------------------------------------------------- TestMethodInstantiation */

/* package p; type T[P any] struct{}; var X T[int]; and then a method
 * declaration, whose parameter and result types are spelled here one letter
 * each: P for the receiver's type parameter, T for T[P], s for string, S for
 * T[string] and I for T[int]. */
static const struct {
    const char *decl;
    bool ptr_recv;
    const char *params;
    const char *results;
    const char *want;
} method_tests[] = {
    {"func (r T[P]) m() P", false, "", "P", "func (T[int]).m() int"},
    {"func (r T[P]) m(P)", false, "P", "", "func (T[int]).m(int)"},
    {"func (r *T[P]) m(P)", true, "P", "", "func (*T[int]).m(int)"},
    {"func (r T[P]) m() T[P]", false, "", "T", "func (T[int]).m() T[int]"},
    {"func (r T[P]) m(T[P])", false, "T", "", "func (T[int]).m(T[int])"},
    {"func (r T[P]) m(T[P], P, string)", false, "TPs", "",
     "func (T[int]).m(T[int], int, string)"},
    {"func (r T[P]) m(T[P], T[string], T[int])", false, "TSI", "",
     "func (T[int]).m(T[int], T[string], T[int])"},
};

/* the types spelled by codes, as above */
static TypesTuple *spelled(World *w, const char *codes, TypesType T, TypesType rp) {
    TypesType ts[4];
    Int n = 0;
    for (const char *c = codes; *c != 0 && n < NELEM(ts); c++) {
        switch (*c) {
        case 'P':
            ts[n++] = rp;
            break;
        case 'T':
            ts[n++] = inst(w, T, rp);
            break;
        case 's':
            ts[n++] = basic(TYPES_STRING);
            break;
        case 'S':
            ts[n++] = inst(w, T, basic(TYPES_STRING));
            break;
        default:
            ts[n++] = inst(w, T, basic(TYPES_INT));
            break;
        }
    }
    return tuple(w, ts, n);
}

/* type T[P any] struct{}, with the method m declared on it, receiver type
 * parameters and all, as the checker declares it */
static TypesType generic_struct(World *w, bool ptr_recv, const char *params,
                                const char *results) {
    TypesNamed *n = generic(w, S("T"), ptrs1(w, tparam(w, S("P"), any_type())));
    types_named_set_underlying(n, types_type(types_new_struct(w->a, none(), none())));
    TypesType T = types_type(n);
    TypesTypeParam *r = tparam(w, S("P"), any_type());
    TypesType rp = types_type(r);
    TypesType rt = inst(w, T, rp);
    if (ptr_recv)
        rt = ptr(w, rt);
    TypesSignature *sig = types_new_signature_type(
        w->a, param(w, S("r"), rt), ptrs1(w, r), none(), spelled(w, params, T, rp),
        spelled(w, results, T, rp), false);
    types_named_add_method(n, types_new_func(w->a, TOKEN_NO_POS, w->pkg, S("m"), sig));
    return T;
}

static void TestMethodInstantiation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(method_tests); i++) {
        World w = world(a, S("p"));
        TypesType T = generic_struct(&w, method_tests[i].ptr_recv,
                                     method_tests[i].params, method_tests[i].results);
        /* var X T[int] */
        TypesType X = inst(&w, T, basic(TYPES_INT));
        TypesType typ = ptr(&w, X);
        TypesSelection *sel = types_lookup_selection(a, typ, false, w.pkg, S("m"));
        if (sel == NULL) {
            Str ts = type_string(a, typ);
            testing_t_fatalf_v(t, "LookupSelection(%s, \"m\") failed, want func m", ts);
        }
        Str got =
            types_object_string(a, types_selection_obj(sel), types_relative_to(w.pkg));
        if (!str_eq(got, str_from_cstr(method_tests[i].want)))
            testing_t_errorf_v(t, "%s: instantiated %q, want %q", method_tests[i].decl,
                               got, method_tests[i].want);
    }
    arena_free(&ar);
}

/* ------------------------------------------------- TestImmutableSignatures */

/* stripAnnotations from errors.go: s without the subscript digits ₀ to ₉,
 * U+2080 to U+2089, which are E2 82 80 to E2 82 89 in UTF-8. */
static Str strip_annotations(Alloc *a, Str s) {
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 0; i < s.len; i++) {
        if (i + 2 < s.len && s.p[i] == 0xE2 && s.p[i + 1] == 0x82 &&
            s.p[i + 2] >= 0x80 && s.p[i + 2] <= 0x89) {
            i += 2;
            continue;
        }
        burrow__types_buf_byte(&buf, s.p[i]);
    }
    return burrow__types_buf_done(&buf, a);
}

static void TestImmutableSignatures(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = world(a, S("p"));
    /* package p; type T[P any] struct{}; func (T[P]) m() {}; var _ T[int] */
    TypesType T = generic_struct(&w, false, "", "");
    TypesType inst_int = inst(&w, T, basic(TYPES_INT));
    /* the checker expands T[int] and its method as it checks the var */
    TypesNamed *ni = types_as_named(inst_int);
    (void)types_type_underlying(inst_int);
    (void)types_object_type(types_object(types_named_method(ni, 0)));

    TypesSelection *sel = types_lookup_selection(a, T, false, w.pkg, S("m"));
    if (sel == NULL) {
        Str ts = type_string(a, T);
        testing_t_fatalf_v(t, "LookupSelection(%s, \"m\") failed, want func m", ts);
    }

    /* Verify that the original method is not mutated by instantiating T (this
     * bug manifested when subst did not return a new signature). */
    Str got = strip_annotations(
        a, types_object_string(a, types_selection_obj(sel), types_relative_to(w.pkg)));
    if (!str_eq(got, S("func (T[P]).m()")))
        testing_t_errorf_v(t, "instantiated %q, want %q", got, "func (T[P]).m()");
    arena_free(&ar);
}

/* ------------------------------------------------- TestInstantiateErrorText */

/* Not in Go. The text of each error, as Go 1.27.1 gives it for the source
 * beside each builder, which covers each reason implements and missingMethod
 * can give that does not need a second package. */
static const ErrorTest text_tests[] = {
    {ec_tilde_string, 0,
     "int does not satisfy interface{~string} (int missing in ~string)"},
    {ec_two, 1, "int does not satisfy interface{~string} (int missing in ~string)"},
    {ec_slice_p1, 1,
     "[]string does not satisfy interface{~[]int} ([]string missing in ~[]int)"},
    {ec_slice_p2, 0,
     "[]string does not satisfy interface{~[]int} ([]string missing in ~[]int)"},
    {ec_ptr_recv, 0, "p.N2 does not satisfy p.I (method m has pointer receiver)"},
    {ec_wrong_sig, 0,
     "p.N3 does not satisfy p.I (wrong type for method m)\n\t\thave m(int)\n\t\twant "
     "m()"},
    {ec_missing, 0, "int does not satisfy p.I (missing method m)"},
    {ec_wrong_name, 0,
     "p.N5 does not satisfy p.I (missing method m)\n\t\thave M()\n\t\twant m()"},
    {ec_field, 0, "p.S does not satisfy p.I (p.S.m is a field, not a method)"},
    {ec_missing_tilde, 0,
     "p.MyInt does not satisfy interface{int} (possibly missing ~ for int in "
     "interface{int})"},
    {ec_empty, 0, "cannot satisfy interface{int; string} (empty type set)"},
    {ec_comparable, 0, "[]int does not satisfy comparable"},
    {ec_iface_subset, 0, "interface{int | string} does not satisfy interface{int}"},
    {ec_ptr_iface, 0, "int does not satisfy interface{*p.I} (int missing in *p.I)"},
    {ec_count, -1,
     "cannot instantiate p.T[P any]: got 2 type arguments but have 1 type parameters"},
    {ec_non_generic, -1, "cannot instantiate non-generic p.T: has no type parameters"},
    {ec_func_count, -1,
     "cannot instantiate func[P any](): got 2 type arguments but have 1 type "
     "parameters"},
};

static void TestInstantiateErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(text_tests); i++) {
        World w = world(a, S("p"));
        TypesType T;
        Error err = must_fail(t, &w, text_tests[i].build, &T);
        const TypesArgumentError *ae = errors_as(err, TYPE_TYPES_ARGUMENT_ERROR);
        Int at = ae == NULL ? -1 : ae->index;
        Str got = error_text(err);
        if (at != text_tests[i].want_at ||
            !str_eq(got, str_from_cstr(text_tests[i].want)))
            testing_t_errorf_v(t, "%d: got %d %q, want %d %q", i, at, got,
                               text_tests[i].want_at, text_tests[i].want);
    }

    /* type T[P comparable] int, with interface{}, which satisfies comparable
     * by the spec */
    World w = world(a, S("p"));
    TypesType T = generic1(&w, comparable_type());
    Error err = BURROW_NO_ERROR;
    TypesType res =
        types_instantiate(a, NULL, T, ptrs1(&w, iface(&w, none(), none())), true, &err);
    Str rs = res == NULL ? S("<nil>") : type_string(a, res);
    if (!BURROW_OK(err) || !str_eq(rs, S("p.T[interface{}]")))
        testing_t_errorf_v(t, "T[interface{}]: got %s, %s", rs, error_text(err));
    arena_free(&ar);
}

/* ------------------------------------------------------ TestInstantiatePanics */

/* Not in Go. The panics, with Go's text. */

typedef struct PanicCall {
    World *w;
    TypesType orig;
    Slice targs;
    bool validate;
} PanicCall;

static void call_instantiate(void *env) {
    PanicCall *c = env;
    (void)types_instantiate(c->w->a, NULL, c->orig, c->targs, c->validate, NULL);
}

/* The panic text of c, copied into buf, or empty if it did not panic. */
static Str recovered(PanicCall *c, char *buf, Int cap) {
    volatile Int n = 0;
    BURROW_TRY {
        call_instantiate(c);
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < cap ? m.len : cap;
        memcpy(buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    return str_from_bytes((const Byte *)buf, n);
}

static void TestInstantiatePanics(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = world(a, S("p"));
    char buf[256];

    /* type T[P any] int, with no type arguments */
    PanicCall c1 = {&w, generic1(&w, any_type()), none(), true};
    Str got = recovered(&c1, buf, (Int)sizeof buf);
    if (!str_eq(got, S("cannot instantiate p.T[P any]: empty type argument list")))
        testing_t_errorf_v(t, "empty targs: panic %q", got);

    /* int, which is not generic */
    PanicCall c2 = {&w, basic(TYPES_INT), ptrs1(&w, basic(TYPES_INT)), true};
    got = recovered(&c2, buf, (Int)sizeof buf);
    if (!str_eq(got,
                S("cannot instantiate non-generic int: expected *Named, *Alias, or "
                  "*Signature")))
        testing_t_errorf_v(t, "int: panic %q", got);

    /* func F[P any]() {}, with two type arguments and no validation */
    PanicCall c3 = {&w, func_any(&w, S("F"), S("P")),
                    ptrs2(&w, basic(TYPES_INT), basic(TYPES_INT)), false};
    got = recovered(&c3, buf, (Int)sizeof buf);
    if (!str_eq(got,
                S("0: too many type arguments for type func[P any](): have 2, want 1")))
        testing_t_errorf_v(t, "F[int, int]: panic %q", got);
    arena_free(&ar);
}

/* --------------------------------------------------------- TestErrorText */

/* Not in Go. types.Error's text, against what Go prints for the same. */
static void TestErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TypesError e1 = {NULL, TOKEN_NO_POS, S("test"), false};
    Str got = types_error_error(&e1, a);
    if (!str_eq(got, S("-: test")))
        testing_t_errorf_v(t, "Error{Msg: \"test\"}: got %q", got);

    /* a file of 20 bytes with a second line at offset 10, and pos at 12 */
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, S("x.go"), -1, 20);
    Int lines[] = {0, 10};
    if (!token_file_set_lines(f,
                              slice_from(lines, NELEM(lines), NELEM(lines), TYPE_INT)))
        testing_t_fatalf_v(t, "SetLines failed");
    TypesError e2 = {fset, token_file_pos(f, 12), S("bad"), false};
    got = types_error_error(&e2, a);
    Error err = types_error_as_error(&e2, a);
    if (!str_eq(got, S("x.go:2:3: bad")) || !str_eq(error_text(err), got))
        testing_t_errorf_v(t, "got %q and %q, want %q", got, error_text(err),
                           "x.go:2:3: bad");

    /* an ArgumentError reads as what it wraps */
    TypesArgumentError ae = {1, types_error_as_error(&e1, a)};
    Error aerr = types_argument_error_as_error(&ae, a);
    got = types_argument_error_error(&ae, a);
    if (!str_eq(got, S("-: test")) || !str_eq(error_text(aerr), got))
        testing_t_errorf_v(t, "ArgumentError: got %q and %q", got, error_text(aerr));
    if (types_argument_error_unwrap(&ae).data != ae.err.data)
        testing_t_errorf_v(t, "ArgumentError does not unwrap to its error");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestInstantiate)                                                                 \
    X(TestInstantiateErrors)                                                           \
    X(TestArgumentErrorUnwrapping)                                                     \
    X(TestImplements)                                                                  \
    X(TestInstantiateEquality)                                                         \
    X(TestInstantiateNonEquality)                                                      \
    X(TestMethodInstantiation)                                                         \
    X(TestImmutableSignatures)                                                         \
    X(TestInstantiateErrorText)                                                        \
    X(TestInstantiatePanics)                                                           \
    X(TestErrorText)

TESTING_MAIN(TESTS)
