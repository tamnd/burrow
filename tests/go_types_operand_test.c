/* Derived from Go's src/go/types/api_test.go and issues_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice none(void) {
    return slice_nil(TYPE_UNSAFE_POINTER);
}

static Slice ptrs1(Alloc *a, void *p) {
    Slice s = burrow__types_ptrs_make(a, 1);
    GT_AT(s, 0, void *) = p;
    return s;
}

static Slice ptrs2(Alloc *a, void *p, void *q) {
    Slice s = burrow__types_ptrs_make(a, 2);
    GT_AT(s, 0, void *) = p;
    GT_AT(s, 1, void *) = q;
    return s;
}

static TypesType basic(TypesBasicKind k) {
    return types_type(types_typ[k]);
}

static TypesType empty_struct(Alloc *a) {
    return types_type(types_new_struct(a, none(), slice_nil(TYPE_STRING)));
}

/* Go's newDefined: a type T, in no package, with the given underlying type */
static TypesType new_defined(Alloc *a, TypesType underlying) {
    TypesTypeName *tname = types_new_type_name(a, TOKEN_NO_POS, NULL, S("T"), NULL);
    return types_type(types_new_named(a, tname, underlying, none()));
}

static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

typedef struct PairTest {
    TypesType v, t;
    bool want;
} PairTest;

static void TestConvertibleTo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesType int_t = basic(TYPES_INT);
    TypesType slice_int = types_type(types_new_slice(a, int_t));
    TypesType arr_int = types_type(types_new_array(a, int_t, 10));
    TypesType arr_uint = types_type(types_new_array(a, basic(TYPES_UINT), 10));
    PairTest tests[] = {
        {int_t, int_t, true},
        {int_t, basic(TYPES_FLOAT32), true},
        {int_t, basic(TYPES_STRING), true},
        {new_defined(a, int_t), int_t, true},
        {new_defined(a, empty_struct(a)), empty_struct(a), true},
        {new_defined(a, int_t), empty_struct(a), false},
        {basic(TYPES_UNTYPED_INT), int_t, true},
        {slice_int, arr_int, true},
        {slice_int, arr_uint, false},
        {slice_int, types_type(types_new_pointer(a, arr_int)), true},
        {slice_int, types_type(types_new_pointer(a, arr_uint)), false},
        /* Untyped string values are not permitted by the spec, so the behavior
         * below is undefined. */
        {basic(TYPES_UNTYPED_STRING), basic(TYPES_STRING), true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool got = types_convertible_to(tests[i].v, tests[i].t);
        if (got != tests[i].want) {
            Str vs = type_string(a, tests[i].v), ts = type_string(a, tests[i].t);
            testing_t_errorf_v(t, "ConvertibleTo(%s, %s) = %t, want %t", vs, ts, got,
                               tests[i].want);
        }
    }
    arena_free(&ar);
}

static void TestAssignableTo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesType int_t = basic(TYPES_INT);
    PairTest tests[] = {
        {int_t, int_t, true},
        {int_t, basic(TYPES_FLOAT32), false},
        {new_defined(a, int_t), int_t, false},
        {new_defined(a, empty_struct(a)), empty_struct(a), true},
        {basic(TYPES_UNTYPED_BOOL), basic(TYPES_BOOL), true},
        {basic(TYPES_UNTYPED_STRING), basic(TYPES_BOOL), false},
        /* Neither untyped string nor untyped numeric assignments arise during
         * normal type checking, so the below behavior is technically undefined
         * by the spec. */
        {basic(TYPES_UNTYPED_STRING), basic(TYPES_STRING), true},
        {basic(TYPES_UNTYPED_INT), int_t, true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool got = types_assignable_to(tests[i].v, tests[i].t);
        if (got != tests[i].want) {
            Str vs = type_string(a, tests[i].v), ts = type_string(a, tests[i].t);
            testing_t_errorf_v(t, "AssignableTo(%s, %s) = %t, want %t", vs, ts, got,
                               tests[i].want);
        }
    }
    arena_free(&ar);
}

static TypesType universe_type(Str name) {
    return types_object_type(types_scope_lookup(types_universe(), name));
}

static void TestIssue50646(TestingT *t) {
    TypesType any_type = types_type_underlying(universe_type(S("any")));
    TypesType comparable_type = universe_type(S("comparable"));

    if (!types_comparable(any_type))
        testing_t_error_v(t, "any is not a comparable type");
    if (!types_comparable(comparable_type))
        testing_t_error_v(t, "comparable is not a comparable type");

    if (types_implements(any_type,
                         types_as_interface(types_type_underlying(comparable_type))))
        testing_t_error_v(t, "any implements comparable");
    if (!types_implements(comparable_type, types_as_interface(any_type)))
        testing_t_error_v(t, "comparable does not implement any");

    if (types_assignable_to(any_type, comparable_type))
        testing_t_error_v(t, "any assignable to comparable");
    if (!types_assignable_to(comparable_type, any_type))
        testing_t_error_v(t, "comparable not assignable to any");
}

/* ------------------------------------------------------- TestAssignConvert */

/* Not in Go. Each rule of assignability and conversion that the exported
 * functions can reach, with the answers Go 1.27.1 gives for the same pairs. */

typedef struct OpTest {
    const char *name;
    TypesType v, t;
    bool assign, convert;
} OpTest;

/* package p's types for the table */
typedef struct Op {
    Alloc *a;
    TypesPackage *pkg;
} Op;

static TypesTypeName *tname(Op *o, Str name) {
    return types_new_type_name(o->a, TOKEN_NO_POS, o->pkg, name, NULL);
}

static TypesNamed *named(Op *o, Str name, TypesType u) {
    return types_new_named(o->a, tname(o, name), u, none());
}

static TypesType ptr(Op *o, TypesType t) {
    return types_type(types_new_pointer(o->a, t));
}

/* func() */
static TypesSignature *no_sig(Op *o, TypesVar *recv) {
    return types_new_signature_type(o->a, recv, none(), none(), NULL, NULL, false);
}

/* func (recv) m() on n */
static void add_m(Op *o, TypesNamed *n, TypesType recv) {
    TypesVar *r = types_new_param(o->a, TOKEN_NO_POS, o->pkg, S(""), recv);
    types_named_add_method(
        n, types_new_func(o->a, TOKEN_NO_POS, o->pkg, S("m"), no_sig(o, r)));
}

/* struct{F int}, with the tag if there is one */
static TypesType struct_f(Op *o, bool tagged) {
    TypesVar *f =
        types_new_field(o->a, TOKEN_NO_POS, o->pkg, S("F"), basic(TYPES_INT), false);
    Slice tags = slice_nil(TYPE_STRING);
    if (tagged) {
        tags = slice_make(o->a, TYPE_STRING, 1, 1);
        *(Str *)slice_at(tags, 0) = S("json:\"f\"");
    }
    return types_type(types_new_struct(o->a, ptrs1(o->a, f), tags));
}

/* interface{ terms }, of one or two terms */
static TypesType cons(Op *o, TypesTerm *x, TypesTerm *y) {
    Slice terms = y == NULL ? ptrs1(o->a, x) : ptrs2(o->a, x, y);
    TypesType u = types_type(types_new_union(o->a, terms));
    return types_type(types_new_interface_type(o->a, none(), ptrs1(o->a, u)));
}

static TypesType tparam(Op *o, Str name, TypesType constraint) {
    return types_type(types_new_type_param(o->a, tname(o, name), constraint));
}

static void TestAssignConvert(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Op o = {a, types_new_package(a, S("p"), S("p"))};

    TypesType int_t = basic(TYPES_INT), f32 = basic(TYPES_FLOAT32);
    TypesType f64 = basic(TYPES_FLOAT64), str = basic(TYPES_STRING);
    TypesType nil_t = basic(TYPES_UNTYPED_NIL), uptr = basic(TYPES_UNSAFE_POINTER);
    TypesType ptr_int = ptr(&o, int_t);
    TypesType slice_int = types_type(types_new_slice(a, int_t));
    TypesType bytes = types_type(types_new_slice(a, basic(TYPES_BYTE)));
    TypesType runes = types_type(types_new_slice(a, basic(TYPES_RUNE)));
    TypesType arr4 = types_type(types_new_array(a, int_t, 4));
    TypesType chan_int = types_type(types_new_chan(a, TYPES_SEND_RECV, int_t));
    TypesType recv_int = types_type(types_new_chan(a, TYPES_RECV_ONLY, int_t));
    TypesType send_int = types_type(types_new_chan(a, TYPES_SEND_ONLY, int_t));
    TypesNamed *my_chan_n = named(&o, S("MyChan"), chan_int);
    TypesNamed *my_int_n = named(&o, S("MyInt"), int_t);
    TypesType my_chan = types_type(my_chan_n), my_int = types_type(my_int_n);
    TypesType map_t = types_type(types_new_map(a, str, int_t));
    TypesType func_t = types_type(no_sig(&o, NULL));
    TypesType any_t = types_type(types_new_interface_type(a, none(), none()));
    TypesFunc *m = types_new_func(a, TOKEN_NO_POS, o.pkg, S("m"), no_sig(&o, NULL));
    TypesType iface_m = types_type(types_new_interface_type(a, ptrs1(a, m), none()));
    TypesNamed *i_n = named(&o, S("I"), iface_m);
    TypesType I = types_type(i_n);
    TypesNamed *n = named(&o, S("N"), int_t);
    add_m(&o, n, types_type(n));
    TypesType N = types_type(n);
    TypesNamed *n2 = named(&o, S("N2"), int_t);
    add_m(&o, n2, ptr(&o, types_type(n2)));
    TypesType N2 = types_type(n2);
    TypesType tagged = struct_f(&o, true), plain = struct_f(&o, false);
    TypesType P =
        tparam(&o, S("P"),
               cons(&o, types_new_term(a, true, int_t), types_new_term(a, true, f64)));
    TypesType Q = tparam(&o, S("Q"), cons(&o, types_new_term(a, true, str), NULL));
    TypesType R = tparam(&o, S("R"), any_t);
    TypesType Sp = tparam(&o, S("S"), cons(&o, types_new_term(a, true, bytes), NULL));
    TypesType comparable = universe_type(S("comparable"));

    OpTest tests[] = {
        {"nil to *int", nil_t, ptr_int, true, true},
        {"nil to []int", nil_t, slice_int, true, true},
        {"nil to map", nil_t, map_t, true, true},
        {"nil to chan", nil_t, chan_int, true, true},
        {"nil to func", nil_t, func_t, true, true},
        {"nil to any", nil_t, any_t, true, true},
        {"nil to I", nil_t, I, true, true},
        {"nil to unsafe.Pointer", nil_t, uptr, true, true},
        {"nil to int", nil_t, int_t, false, false},
        {"nil to string", nil_t, str, false, false},
        {"untyped bool to bool", basic(TYPES_UNTYPED_BOOL), basic(TYPES_BOOL), true,
         true},
        {"untyped bool to any", basic(TYPES_UNTYPED_BOOL), any_t, true, true},
        {"untyped bool to I", basic(TYPES_UNTYPED_BOOL), I, false, false},
        {"untyped bool to int", basic(TYPES_UNTYPED_BOOL), int_t, false, false},
        {"untyped int to float32", basic(TYPES_UNTYPED_INT), f32, true, true},
        {"untyped int to string", basic(TYPES_UNTYPED_INT), str, false, true},
        {"untyped rune to int32", basic(TYPES_UNTYPED_RUNE), basic(TYPES_INT32), true,
         true},
        {"untyped float to untyped int", basic(TYPES_UNTYPED_FLOAT),
         basic(TYPES_UNTYPED_INT), true, true},
        {"untyped string to untyped bool", basic(TYPES_UNTYPED_STRING),
         basic(TYPES_UNTYPED_BOOL), false, false},
        {"chan int to <-chan int", chan_int, recv_int, true, true},
        {"chan int to chan<- int", chan_int, send_int, true, true},
        {"chan int to MyChan", chan_int, my_chan, true, true},
        {"<-chan int to chan int", recv_int, chan_int, false, false},
        {"MyChan to <-chan int", my_chan, recv_int, true, true},
        {"N to I", N, I, true, true},
        {"N to interface{m()}", N, iface_m, true, true},
        {"*N2 to I", ptr(&o, N2), I, true, true},
        {"N2 to I", N2, I, false, false},
        {"I to N", I, N, false, false},
        {"any to int", any_t, int_t, false, false},
        {"int to string", int_t, str, false, true},
        {"[]byte to string", bytes, str, false, true},
        {"string to []rune", str, runes, false, true},
        {"[]int to string", slice_int, str, false, false},
        {"float32 to int", f32, int_t, false, true},
        {"complex64 to complex128", basic(TYPES_COMPLEX64), basic(TYPES_COMPLEX128),
         false, true},
        {"complex64 to float64", basic(TYPES_COMPLEX64), f64, false, false},
        {"*int to unsafe.Pointer", ptr_int, uptr, false, true},
        {"uintptr to unsafe.Pointer", basic(TYPES_UINTPTR), uptr, false, true},
        {"unsafe.Pointer to *int", uptr, ptr_int, false, true},
        {"unsafe.Pointer to uintptr", uptr, basic(TYPES_UINTPTR), false, true},
        {"int to unsafe.Pointer", int_t, uptr, false, false},
        {"tagged struct to plain struct", tagged, plain, false, true},
        {"*tagged struct to *plain struct", ptr(&o, tagged), ptr(&o, plain), false,
         true},
        {"[]int to [4]int", slice_int, arr4, false, true},
        {"[]int to *[4]int", slice_int, ptr(&o, arr4), false, true},
        {"MyInt to int", my_int, int_t, false, true},
        {"MyInt to float64", my_int, f64, false, true},
        {"P to float64", P, f64, false, true},
        {"int to P", int_t, P, false, true},
        {"string to P", str, P, false, false},
        {"[]byte to S", bytes, Sp, true, true},
        {"S to []byte", Sp, bytes, true, true},
        {"S to string", Sp, str, false, true},
        {"P to Q", P, Q, false, false},
        {"R to int", R, int_t, false, false},
        {"nil to R", nil_t, R, false, false},
        {"nil to S", nil_t, Sp, true, true},
        {"untyped int to P", basic(TYPES_UNTYPED_INT), P, true, true},
        {"P to any", P, any_t, true, true},
        {"comparable to any", comparable, any_t, true, true},
        {"any to comparable", any_t, comparable, false, false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool assign = types_assignable_to(tests[i].v, tests[i].t);
        bool convert = types_convertible_to(tests[i].v, tests[i].t);
        if (assign != tests[i].assign || convert != tests[i].convert)
            testing_t_errorf_v(t, "%s: AssignableTo %t, ConvertibleTo %t, want %t, %t",
                               tests[i].name, assign, convert, tests[i].assign,
                               tests[i].convert);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestConvertibleTo)                                                               \
    X(TestAssignableTo)                                                                \
    X(TestIssue50646)                                                                  \
    X(TestAssignConvert)

TESTING_MAIN(TESTS)
