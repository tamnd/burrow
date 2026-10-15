/* Derived from Go's src/go/types/api_test.go and methodset_test.go. Go source:
 * go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Go's tests type check a small package for each case. There is no checker
 * yet, so each case here builds the types the checker would make for its
 * source, which is kept next to it. The cases on generic types are left out
 * until there is a checker to instantiate them. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/strconv.h"

#include <string.h>

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------------- builders */

/* What a case builds its types with: the allocator and package p. */
typedef struct World {
    Alloc *a;
    TypesPackage *pkg;
} World;

static void world_init(World *w, Alloc *a) {
    w->a = a;
    w->pkg = types_new_package(a, S("p"), S("p"));
}

/* type name u, where u may be NULL and set later */
static TypesNamed *named(World *w, Str name, TypesType u) {
    TypesTypeName *obj = types_new_type_name(w->a, TOKEN_NO_POS, w->pkg, name, NULL);
    return types_new_named(w->a, obj, u, slice_nil(TYPE_UNSAFE_POINTER));
}

/* the same, as a TypesType */
static TypesType named_t(World *w, Str name, TypesType u) {
    return types_type(named(w, name, u));
}

static TypesType ptr(World *w, TypesType t) {
    return types_type(types_new_pointer(w->a, t));
}

/* name(), or name(x int) string with params set, on a receiver of type recv,
 * which is NULL for an interface method. */
static TypesFunc *func(World *w, TypesType recv, Str name, bool params) {
    Alloc *a = w->a;
    TypesVar *r = NULL;
    if (recv != NULL)
        r = types_new_param(a, TOKEN_NO_POS, w->pkg, S(""), recv);
    TypesTuple *in = NULL, *out = NULL;
    if (params) {
        Slice p = burrow__types_ptrs_make(a, 1);
        GT_AT(p, 0, TypesVar *) =
            types_new_param(a, TOKEN_NO_POS, w->pkg, S("x"), GT_TYP(TYPES_INT));
        in = types_new_tuple(a, p);
        Slice q = burrow__types_ptrs_make(a, 1);
        GT_AT(q, 0, TypesVar *) =
            types_new_param(a, TOKEN_NO_POS, w->pkg, S(""), GT_TYP(TYPES_STRING));
        out = types_new_tuple(a, q);
    }
    TypesSignature *sig = types_new_signature(a, r, in, out, false);
    return types_new_func(a, TOKEN_NO_POS, w->pkg, name, sig);
}

/* func (n) name() {}, or func (*n) name() {} */
static TypesFunc *method(World *w, TypesNamed *n, bool ptr_recv, Str name) {
    TypesType recv = types_type(n);
    if (ptr_recv)
        recv = ptr(w, recv);
    TypesFunc *f = func(w, recv, name, false);
    types_named_add_method(n, f);
    return f;
}

static TypesVar *field(World *w, Str name, TypesType typ) {
    return types_new_field(w->a, TOKEN_NO_POS, w->pkg, name, typ, false);
}

/* an embedded field n, or *n */
static TypesVar *embed(World *w, TypesNamed *n, bool is_ptr) {
    TypesType typ = types_type(n);
    if (is_ptr)
        typ = ptr(w, typ);
    Str name = types_object_name(types_object(types_named_obj(n)));
    return types_new_field(w->a, TOKEN_NO_POS, w->pkg, name, typ, true);
}

static TypesType strct(World *w, TypesVar *const *fields, Int n) {
    Slice s = burrow__types_ptrs_make(w->a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(s, i, TypesVar *) = fields[i];
    return types_type(types_new_struct(w->a, s, slice_nil(TYPE_STRING)));
}

/* struct{ f int } */
static TypesType struct_f(World *w) {
    TypesVar *f = field(w, S("f"), GT_TYP(TYPES_INT));
    return strct(w, &f, 1);
}

/* interface{ f() } */
static TypesType iface_f(World *w) {
    Slice ms = burrow__types_ptrs_make(w->a, 1);
    GT_AT(ms, 0, TypesFunc *) = func(w, NULL, S("f"), false);
    return types_type(
        types_new_interface_type(w->a, ms, slice_nil(TYPE_UNSAFE_POINTER)));
}

/* interface{ e } */
static TypesType iface_embed(World *w, TypesType e) {
    Slice es = burrow__types_ptrs_make(w->a, 1);
    GT_AT(es, 0, TypesType) = e;
    return types_type(
        types_new_interface_type(w->a, slice_nil(TYPE_UNSAFE_POINTER), es));
}

static TypesType empty_struct(World *w) {
    return strct(w, NULL, 0);
}

/* ------------------------------------------------------- the cases' types */

/* type T struct{} */
static TypesType t_empty(World *w) {
    return named_t(w, S("T"), empty_struct(w));
}

/* type T struct{ f int } */
static TypesType t_f(World *w) {
    return named_t(w, S("T"), struct_f(w));
}

/* type T struct{ a, b, f, c int } */
static TypesType t_abfc(World *w) {
    TypesType i = GT_TYP(TYPES_INT);
    TypesVar *fs[] = {field(w, S("a"), i), field(w, S("b"), i), field(w, S("f"), i),
                      field(w, S("c"), i)};
    return named_t(w, S("T"), strct(w, fs, NELEM(fs)));
}

/* type T struct{}; func (T) f() {} */
static TypesType t_val_f(World *w) {
    TypesNamed *t = named(w, S("T"), empty_struct(w));
    method(w, t, false, S("f"));
    return types_type(t);
}

/* *T, with T as above */
static TypesType pt_val_f(World *w) {
    return ptr(w, t_val_f(w));
}

/* type T struct{}; func (*T) f() {} */
static TypesType t_ptr_f(World *w) {
    TypesNamed *t = named(w, S("T"), empty_struct(w));
    method(w, t, true, S("f"));
    return types_type(t);
}

static TypesType pt_ptr_f(World *w) {
    return ptr(w, t_ptr_f(w));
}

/* type ( E1 struct{ f int }; E2 struct{ f int }; x struct{ E1; *E2 }) */
static TypesType x_two_fields(World *w) {
    TypesNamed *e1 = named(w, S("E1"), struct_f(w));
    TypesNamed *e2 = named(w, S("E2"), struct_f(w));
    TypesVar *fs[] = {embed(w, e1, false), embed(w, e2, true)};
    return named_t(w, S("x"), strct(w, fs, NELEM(fs)));
}

/* type ( E1 struct{ f int }; E2 struct{}; x struct{ E1; *E2 }); func (E2) f()
 * {} */
static TypesType x_field_method(World *w) {
    TypesNamed *e1 = named(w, S("E1"), struct_f(w));
    TypesNamed *e2 = named(w, S("E2"), empty_struct(w));
    method(w, e2, false, S("f"));
    TypesVar *fs[] = {embed(w, e1, false), embed(w, e2, true)};
    return named_t(w, S("x"), strct(w, fs, NELEM(fs)));
}

/* type T interface{ f() } */
static TypesType t_iface(World *w) {
    return named_t(w, S("T"), iface_f(w));
}

/* type ( T1 T2; T2 interface{ f() } ) */
static TypesType t1_t2_iface(World *w) {
    TypesNamed *t2 = named(w, S("T2"), iface_f(w));
    return named_t(w, S("T1"), types_type_underlying(types_type(t2)));
}

/* type ( T1 interface{ T2 }; T2 interface{ f() } ) */
static TypesType t1_embeds_t2(World *w) {
    TypesNamed *t2 = named(w, S("T2"), iface_f(w));
    return named_t(w, S("T1"), iface_embed(w, types_type(t2)));
}

/* struct{ E }, or struct{ *E } */
static TypesType struct_embed(World *w, TypesNamed *e, bool is_ptr) {
    TypesVar *f = embed(w, e, is_ptr);
    return strct(w, &f, 1);
}

/* struct{ E }; type E interface{ f() } */
static TypesType s_e_iface(World *w) {
    return struct_embed(w, named(w, S("E"), iface_f(w)), false);
}

static TypesType ps_e_iface(World *w) {
    return ptr(w, s_e_iface(w));
}

/* struct{ E } or struct{ *E }; type E struct{}; func (E) f() {} or func (*E)
 * f() {} */
static TypesType s_e_method(World *w, bool embed_ptr, bool ptr_recv) {
    TypesNamed *e = named(w, S("E"), empty_struct(w));
    method(w, e, ptr_recv, S("f"));
    return struct_embed(w, e, embed_ptr);
}

static TypesType s_e_val_f(World *w) {
    return s_e_method(w, false, false);
}

static TypesType s_pe_val_f(World *w) {
    return s_e_method(w, true, false);
}

static TypesType s_e_ptr_f(World *w) {
    return s_e_method(w, false, true);
}

static TypesType s_pe_ptr_f(World *w) {
    return s_e_method(w, true, true);
}

/* struct{ E1; *E2 }; type ( E1 interface{ f() }; E2 struct{ f int }) */
static TypesType s_iface_field(World *w) {
    TypesNamed *e1 = named(w, S("E1"), iface_f(w));
    TypesNamed *e2 = named(w, S("E2"), struct_f(w));
    TypesVar *fs[] = {embed(w, e1, false), embed(w, e2, true)};
    return strct(w, fs, NELEM(fs));
}

/* struct{ E1; *E2 }; type ( E1 struct{ f int }; E2 struct{} ); func (E2) f()
 * {} */
static TypesType s_field_method(World *w) {
    TypesNamed *e1 = named(w, S("E1"), struct_f(w));
    TypesNamed *e2 = named(w, S("E2"), empty_struct(w));
    method(w, e2, false, S("f"));
    TypesVar *fs[] = {embed(w, e1, false), embed(w, e2, true)};
    return strct(w, fs, NELEM(fs));
}

/* type C interface{ f() }; func g[T C](a T){}, which gives a the type
 * parameter T. So does declaring a as a variable in g's body. */
static TypesType tparam_c(World *w) {
    TypesNamed *c = named(w, S("C"), iface_f(w));
    TypesTypeName *obj = types_new_type_name(w->a, TOKEN_NO_POS, w->pkg, S("T"), NULL);
    return types_type(types_new_type_param(w->a, obj, types_type(c)));
}

/* "[1 0]", the way %v prints an []int */
static Str index_string(Alloc *a, Slice index) {
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_byte(&buf, '[');
    for (Int i = 0; i < index.len; i++) {
        if (i > 0)
            bytes_buffer_write_byte(&buf, ' ');
        bytes_buffer_write_string(&buf, strconv_itoa(a, GT_AT(index, i, Int)), NULL);
    }
    bytes_buffer_write_byte(&buf, ']');
    return bytes_buffer_string(&buf, a);
}

/* slices.Equal, which takes nil and empty to be the same */
static bool index_equal(Slice got, const Int *want, Int n) {
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++)
        if (GT_AT(got, i, Int) != want[i])
            return false;
    return true;
}

static Str want_index_string(Alloc *a, const Int *want, Int n) {
    Slice s = slice_make(a, TYPE_INT, n, n);
    for (Int i = 0; i < n; i++)
        GT_AT(s, i, Int) = want[i];
    return index_string(a, s);
}

/* ------------------------------------------------------------------ lookup */

static void lookup_nil(void) {
    types_lookup_field_or_method(NULL, NULL, false, NULL, S(""), NULL, NULL);
}

static void TestLookupFieldOrMethodOnNil(TestingT *t) {
    /* LookupFieldOrMethod on a nil type is expected to produce a run-time
     * panic. */
    static const Str want = S_("LookupFieldOrMethod on nil type");
    char buf[64];
    volatile Int n = -1;
    BURROW_TRY {
        lookup_nil();
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof buf ? s.len : (Int)sizeof buf;
        memcpy(buf, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0) {
        testing_t_fatalf_v(t, "got no panic, want %s", want);
        return;
    }
    Str got = str_from_bytes((const Byte *)buf, n);
    if (!str_eq(got, want))
        testing_t_fatalf_v(t, "got %s, want %s", got, want);
}

typedef struct LookupTest {
    Str src;
    TypesType (*build)(World *w);
    struct {
        bool addressable; /* the variable is a, not x */
        bool found;
        Int index[2];
        Int nindex;
        bool indirect;
    } want;
} LookupTest;

static void TestLookupFieldOrMethod(TestingT *t) {
    /* Test cases assume a lookup of the form a.f or x.f, where a stands for
     * an addressable value, and x for a non-addressable value (even though a
     * variable for ease of test case writing).
     *
     * Should be kept in sync with TestMethodSet. */
    static const LookupTest tests[] = {
        /* field lookups */
        {S_("var x T; type T struct{}"), t_empty, {false, false, {0}, 0, false}},
        {S_("var x T; type T struct{ f int }"), t_f, {false, true, {0}, 1, false}},
        {S_("var x T; type T struct{ a, b, f, c int }"),
         t_abfc,
         {false, true, {2}, 1, false}},

        /* method lookups */
        {S_("var a T; type T struct{}; func (T) f() {}"),
         t_val_f,
         {true, true, {0}, 1, false}},
        {S_("var a *T; type T struct{}; func (T) f() {}"),
         pt_val_f,
         {true, true, {0}, 1, true}},
        {S_("var a T; type T struct{}; func (*T) f() {}"),
         t_ptr_f,
         {true, true, {0}, 1, false}},
        {S_("var a *T; type T struct{}; func (*T) f() {}"),
         pt_ptr_f,
         {true, true, {0}, 1, true}},

        /* collisions */
        {S_("type ( E1 struct{ f int }; E2 struct{ f int }; x struct{ E1; *E2 })"),
         x_two_fields,
         {false, false, {1, 0}, 2, false}},
        {S_("type ( E1 struct{ f int }; E2 struct{}; x struct{ E1; *E2 }); func (E2) "
            "f() {}"),
         x_field_method,
         {false, false, {1, 0}, 2, false}},

        /* outside methodset
         * (*T).f method exists, but value of type T is not addressable */
        {S_("var x T; type T struct{}; func (*T) f() {}"),
         t_ptr_f,
         {false, false, {0}, 0, true}},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const LookupTest *test = &tests[i];
        World w;
        world_init(&w, a);
        TypesType typ = test->build(&w);

        Slice index;
        bool indirect;
        TypesObject f = types_lookup_field_or_method(a, typ, test->want.addressable,
                                                     w.pkg, S("f"), &index, &indirect);
        if ((f != NULL) != test->want.found) {
            if (f == NULL) {
                testing_t_errorf_v(t, "%s: got no object; want one", test->src);
            } else {
                Str s = types_object_string(a, f, TYPES_NO_QUALIFIER);
                testing_t_errorf_v(t, "%s: got object = %s; want none", test->src, s);
            }
        }
        if (!index_equal(index, test->want.index, test->want.nindex)) {
            Str got = index_string(a, index);
            Str want = want_index_string(a, test->want.index, test->want.nindex);
            testing_t_errorf_v(t, "%s: got index = %s; want %s", test->src, got, want);
        }
        if (indirect != test->want.indirect)
            testing_t_errorf_v(t, "%s: got indirect = %t; want %t", test->src, indirect,
                               test->want.indirect);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- method sets */

typedef struct MethodSetTest {
    Str src;
    TypesType (*build)(World *w);
    struct {
        Int nmethods; /* 0 or 1, and when 1 the method is f */
        Int index[2];
        Int nindex;
        bool indirect;
    } want;
} MethodSetTest;

static void TestNewMethodSet(TestingT *t) {
    /* Tests are expressed src -> methods, for simplifying the composite
     * literal. Should be kept in sync with TestLookupFieldOrMethod. */
    static const MethodSetTest tests[] = {
        /* Named types */
        {S_("var a T; type T struct{}; func (T) f() {}"), t_val_f, {1, {0}, 1, false}},
        {S_("var a *T; type T struct{}; func (T) f() {}"), pt_val_f, {1, {0}, 1, true}},
        {S_("var a T; type T struct{}; func (*T) f() {}"), t_ptr_f, {0, {0}, 0, false}},
        {S_("var a *T; type T struct{}; func (*T) f() {}"),
         pt_ptr_f,
         {1, {0}, 1, true}},

        /* Interfaces */
        {S_("var a T; type T interface{ f() }"), t_iface, {1, {0}, 1, true}},
        {S_("var a T1; type ( T1 T2; T2 interface{ f() } )"),
         t1_t2_iface,
         {1, {0}, 1, true}},
        {S_("var a T1; type ( T1 interface{ T2 }; T2 interface{ f() } )"),
         t1_embeds_t2,
         {1, {0}, 1, true}},

        /* Embedding */
        {S_("var a struct{ E }; type E interface{ f() }"),
         s_e_iface,
         {1, {0, 0}, 2, true}},
        {S_("var a *struct{ E }; type E interface{ f() }"),
         ps_e_iface,
         {1, {0, 0}, 2, true}},
        {S_("var a struct{ E }; type E struct{}; func (E) f() {}"),
         s_e_val_f,
         {1, {0, 0}, 2, false}},
        {S_("var a struct{ *E }; type E struct{}; func (E) f() {}"),
         s_pe_val_f,
         {1, {0, 0}, 2, true}},
        {S_("var a struct{ E }; type E struct{}; func (*E) f() {}"),
         s_e_ptr_f,
         {0, {0}, 0, false}},
        {S_("var a struct{ *E }; type E struct{}; func (*E) f() {}"),
         s_pe_ptr_f,
         {1, {0, 0}, 2, true}},

        /* collisions */
        {S_("var a struct{ E1; *E2 }; type ( E1 interface{ f() }; E2 struct{ f int })"),
         s_iface_field,
         {0, {0}, 0, false}},
        {S_("var a struct{ E1; *E2 }; type ( E1 struct{ f int }; E2 struct{} ); func "
            "(E2) f() {}"),
         s_field_method,
         {0, {0}, 0, false}},

        /* By convention, look up a in the scope of "g" */
        {S_("type C interface{ f() }; func g[T C](a T){}"),
         tparam_c,
         {1, {0}, 1, true}},
        {S_("type C interface{ f() }; func g[T C]() { var a T; _ = a }"),
         tparam_c,
         {1, {0}, 1, true}},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const MethodSetTest *test = &tests[i];
        World w;
        world_init(&w, a);
        TypesMethodSet *ms = types_new_method_set(a, test->build(&w));
        Int got = types_method_set_len(ms);
        if (got != test->want.nmethods) {
            testing_t_errorf_v(t, "%s: got %d methods, want %d", test->src, got,
                               test->want.nmethods);
            continue;
        }
        if (test->want.nmethods == 0)
            continue;
        TypesSelection *sel = types_method_set_at(ms, 0);
        Str name = types_object_name(types_selection_obj(sel));
        Str want_name = S("f");
        if (!str_eq(name, want_name))
            testing_t_errorf_v(t, "%s [method 0]: got name = %q at, want %q", test->src,
                               name, want_name);
        Slice index = types_selection_index(sel);
        if (!index_equal(index, test->want.index, test->want.nindex)) {
            Str g = index_string(a, index);
            Str want = want_index_string(a, test->want.index, test->want.nindex);
            testing_t_errorf_v(t, "%s [method 0]: got index = %s, want %s", test->src,
                               g, want);
        }
        if (types_selection_indirect(sel) != test->want.indirect)
            testing_t_errorf_v(t, "%s [method 0]: got indirect = %t, want %t",
                               test->src, types_selection_indirect(sel),
                               test->want.indirect);
    }
    arena_free(&ar);
}

static void TestIssue60634(TestingT *t) {
    /* package p
     * type T *int
     * func (T) m() {} // expected error: invalid receiver type
     *
     * The checker reports the receiver and still gives T the method. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w;
    world_init(&w, a);
    TypesNamed *tn = named(&w, S("T"), ptr(&w, GT_TYP(TYPES_INT)));
    method(&w, tn, false, S("m"));

    /* look up T.m and (*T).m */
    TypesType T = types_type(tn);
    TypesType recvs[] = {T, ptr(&w, T)};
    for (Int i = 0; i < NELEM(recvs); i++) {
        TypesType recv = recvs[i];
        /* LookupFieldOrMethod and NewMethodSet must match: either both find m
         * or neither finds it. */
        TypesObject obj1 =
            types_lookup_field_or_method(a, recv, false, w.pkg, S("m"), NULL, NULL);
        TypesMethodSet *mset = types_new_method_set(a, recv);
        if ((obj1 != NULL) != (types_method_set_len(mset) == 1)) {
            Str r = types_type_string(a, recv, TYPES_NO_QUALIFIER);
            Str m = types_method_set_string(a, mset);
            testing_t_fatalf_v(t, "lookup(%s.m): got obj = %t, mset = %s", r,
                               obj1 != NULL, m);
            break;
        }
        /* If the method exists, both must return the same object. */
        if (obj1 != NULL) {
            TypesObject obj2 = types_selection_obj(types_method_set_at(mset, 0));
            if (obj1 != obj2) {
                Str s1 = types_object_string(a, obj1, TYPES_NO_QUALIFIER);
                Str s2 = types_object_string(a, obj2, TYPES_NO_QUALIFIER);
                testing_t_fatalf_v(t, "%s != %s", s1, s2);
                break;
            }
        }
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------- missing method */

static void TestMissingMethodAlternative(TestingT *t) {
    /* package p
     * type T interface {
     *     m()
     * }
     *
     * type V0 struct{}
     * func (V0) m() {}
     *
     * type V1 struct{}
     *
     * type V2 struct{}
     * func (V2) m() int
     *
     * type V3 struct{}
     * func (*V3) m()
     *
     * type V4 struct{}
     * func (V4) M() */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w;
    world_init(&w, a);

    Slice ms = burrow__types_ptrs_make(a, 1);
    GT_AT(ms, 0, TypesFunc *) = func(&w, NULL, S("m"), false);
    TypesInterface *T = types_new_interface_type(a, ms, slice_nil(TYPE_UNSAFE_POINTER));
    named(&w, S("T"), types_type(T));

    TypesNamed *v[5];
    static const Str names[] = {S_("V0"), S_("V1"), S_("V2"), S_("V3"), S_("V4")};
    for (Int i = 0; i < NELEM(v); i++)
        v[i] = named(&w, names[i], empty_struct(&w));
    method(&w, v[0], false, S("m"));
    {
        /* func (V2) m() int */
        Slice r = burrow__types_ptrs_make(a, 1);
        GT_AT(r, 0, TypesVar *) =
            types_new_param(a, TOKEN_NO_POS, w.pkg, S(""), GT_TYP(TYPES_INT));
        TypesVar *recv =
            types_new_param(a, TOKEN_NO_POS, w.pkg, S(""), types_type(v[2]));
        TypesSignature *sig =
            types_new_signature(a, recv, NULL, types_new_tuple(a, r), false);
        types_named_add_method(v[2],
                               types_new_func(a, TOKEN_NO_POS, w.pkg, S("m"), sig));
    }
    method(&w, v[3], true, S("m"));
    method(&w, v[4], false, S("M"));

    /* V0 has method m with correct signature. Should not report
     * wrongType. */
    bool wrong_type;
    TypesFunc *m = types_missing_method(types_type(v[0]), T, true, &wrong_type);
    if (m != NULL || wrong_type) {
        testing_t_fatalf_v(t, "V0: got method = %t, wrongType = %t", m != NULL,
                           wrong_type);
        arena_free(&ar);
        return;
    }

    /* V1 has no method m. Should not report wrongType.
     * V2 has method m with wrong signature type (ignoring receiver). Should
     * report wrongType.
     * V3 has no method m but it exists on *V3. Should report wrongType.
     * V4 has no method m but has M. Should not report wrongType. */
    static const bool report_wrong_type[] = {false, false, true, true, false};
    for (Int i = 1; i < NELEM(v); i++) {
        m = types_missing_method(types_type(v[i]), T, true, &wrong_type);
        if (m == NULL || !str_eq(types_object_name(types_object(m)), S("m")) ||
            wrong_type != report_wrong_type[i]) {
            testing_t_fatalf_v(t, "%s: got method = %t, wrongType = %t", names[i],
                               m != NULL, wrong_type);
            break;
        }
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------- selections */

/* The types of this package:
 *
 *     package p
 *
 *     type E struct{ g int }
 *
 *     func (*E) f(x int) string { return "" }
 *
 *     type T struct{ E }
 *
 *     type U struct{}
 *
 *     func (U) B()       {}
 *     func (U) a()       {}
 *     func (*U) C(n int) {}
 */
typedef struct SelPkg {
    World w;
    TypesType T;
    TypesType U;
} SelPkg;

static void sel_pkg(SelPkg *p, Alloc *a) {
    World *w = &p->w;
    world_init(w, a);
    TypesVar *g = field(w, S("g"), GT_TYP(TYPES_INT));
    TypesNamed *e = named(w, S("E"), strct(w, &g, 1));
    types_named_add_method(e, func(w, ptr(w, types_type(e)), S("f"), true));
    p->T = named_t(w, S("T"), struct_embed(w, e, false));

    TypesNamed *u = named(w, S("U"), empty_struct(w));
    method(w, u, false, S("B"));
    method(w, u, false, S("a"));
    {
        /* func (*U) C(n int) */
        Slice in = burrow__types_ptrs_make(a, 1);
        GT_AT(in, 0, TypesVar *) =
            types_new_param(a, TOKEN_NO_POS, w->pkg, S("n"), GT_TYP(TYPES_INT));
        TypesVar *recv =
            types_new_param(a, TOKEN_NO_POS, w->pkg, S(""), ptr(w, types_type(u)));
        TypesSignature *sig =
            types_new_signature(a, recv, types_new_tuple(a, in), NULL, false);
        types_named_add_method(u, types_new_func(a, TOKEN_NO_POS, w->pkg, S("C"), sig));
    }
    p->U = types_type(u);
}

/* Not in Go: what LookupSelection and SelectionString give for the package
 * above, with the outputs taken from Go's own. */
static void TestLookupSelection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SelPkg p;
    sel_pkg(&p, a);
    TypesType pT = ptr(&p.w, p.T);

    static const struct {
        int recv; /* 0 for T and 1 for *T */
        bool addressable;
        Str name;
        Str want;     /* SelectionString, or empty for none */
        Str want_rel; /* the same, qualified relative to p */
        Int index[2];
        bool indirect;
    } tests[] = {
        {0,
         false,
         S_("g"),
         S_("field (p.T) g int"),
         S_("field (T) g int"),
         {0, 0},
         false},
        {0,
         true,
         S_("f"),
         S_("method (p.T) f(x int) string"),
         S_("method (T) f(x int) string"),
         {0, 0},
         false},
        {0, false, S_("f"), S_(""), S_(""), {0}, false},
        {1,
         false,
         S_("f"),
         S_("method (*p.T) f(x int) string"),
         S_("method (*T) f(x int) string"),
         {0, 0},
         true},
        {0, false, S_("h"), S_(""), S_(""), {0}, false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        TypesType recv = tests[i].recv == 0 ? p.T : pT;
        TypesSelection *sel = types_lookup_selection(a, recv, tests[i].addressable,
                                                     p.w.pkg, tests[i].name);
        if (tests[i].want.len == 0) {
            if (sel != NULL) {
                Str got = types_selection_string(a, sel, TYPES_NO_QUALIFIER);
                testing_t_errorf_v(t, "%s: got %s, want none", tests[i].name, got);
            }
            continue;
        }
        if (sel == NULL) {
            testing_t_errorf_v(t, "%s: got none, want %s", tests[i].name,
                               tests[i].want);
            continue;
        }
        Str got = types_selection_string(a, sel, TYPES_NO_QUALIFIER);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "got %s, want %s", got, tests[i].want);
        TypesQualifier rel = types_relative_to(p.w.pkg);
        got = types_selection_string(a, sel, rel);
        if (!str_eq(got, tests[i].want_rel))
            testing_t_errorf_v(t, "got %s, want %s", got, tests[i].want_rel);
        if (!index_equal(types_selection_index(sel), tests[i].index, 2)) {
            Str g = index_string(a, types_selection_index(sel));
            testing_t_errorf_v(t, "%s: got index = %s, want [0 0]", tests[i].want, g);
        }
        if (types_selection_indirect(sel) != tests[i].indirect)
            testing_t_errorf_v(t, "%s: got indirect = %t, want %t", tests[i].want,
                               types_selection_indirect(sel), tests[i].indirect);
    }
    arena_free(&ar);
}

/* Not in Go: MethodSet.String and MethodSet.Lookup for the package above,
 * with the outputs taken from Go's own. The methods are sorted by Id, which
 * puts the unexported p.a after B and C. */
static void TestMethodSetString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SelPkg p;
    sel_pkg(&p, a);

    TypesType types[] = {p.T, ptr(&p.w, p.T), p.U, ptr(&p.w, p.U)};
    static const Str want[] = {
        S_("MethodSet {}"),
        S_("MethodSet {\n\tmethod (*p.T) f(x int) string\n}\n"),
        S_("MethodSet {\n\tmethod (p.U) B()\n\tmethod (p.U) a()\n}\n"),
        S_("MethodSet {\n\tmethod (*p.U) B()\n\tmethod (*p.U) C(n int)\n\tmethod "
           "(*p.U) a()\n}\n"),
    };
    for (Int i = 0; i < NELEM(types); i++) {
        Str got = types_method_set_string(a, types_new_method_set(a, types[i]));
        if (!str_eq(got, want[i]))
            testing_t_errorf_v(t, "got %q, want %q", got, want[i]);
    }

    TypesMethodSet *ms = types_new_method_set(a, ptr(&p.w, p.U));
    static const struct {
        Str name;
        bool found;
    } lookups[] = {{S_("B"), true},
                   {S_("C"), true},
                   {S_("a"), true},
                   {S_("b"), false},
                   {S_("D"), false}};
    for (Int i = 0; i < NELEM(lookups); i++) {
        bool found = types_method_set_lookup(ms, p.w.pkg, lookups[i].name) != NULL;
        if (found != lookups[i].found)
            testing_t_errorf_v(t, "Lookup(p, %s): got %t, want %t", lookups[i].name,
                               found, lookups[i].found);
    }
    /* a is unexported, so it is only found in its own package. */
    if (types_method_set_lookup(ms, NULL, S("a")) != NULL)
        testing_t_error_v(t, "Lookup(nil, a): got a method, want none");
    arena_free(&ar);
}

/* Not in Go: the selections the checker makes for the method expressions
 * (*T).f and U.B, which take the receiver as their first parameter. The
 * outputs are taken from Go's own. */
static void TestMethodExprSelection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SelPkg p;
    sel_pkg(&p, a);

    struct {
        TypesType recv;
        Str name;
        Str want;
        Str want_type;
        Int index[2];
        Int nindex;
        bool indirect;
    } tests[] = {
        {ptr(&p.w, p.T),
         S("f"),
         S("method expr (*p.T) f(*p.T, x int) string"),
         S("func(*p.T, x int) string"),
         {0, 0},
         2,
         true},
        {p.U, S("B"), S("method expr (p.U) B(p.U)"), S("func(p.U)"), {0}, 1, false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        TypesSelection *found =
            types_lookup_selection(a, tests[i].recv, false, p.w.pkg, tests[i].name);
        if (found == NULL)
            testing_t_fatalf_v(t, "%s: not found", tests[i].name);
        TypesSelection *sel = burrow__types_new_selection(
            a, TYPES_METHOD_EXPR, tests[i].recv, types_selection_obj(found),
            types_selection_index(found), types_selection_indirect(found));
        Str got = types_selection_string(a, sel, TYPES_NO_QUALIFIER);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "got %s, want %s", got, tests[i].want);
        TypesType typ = types_selection_type(a, sel);
        got = types_type_string(a, typ, TYPES_NO_QUALIFIER);
        if (!str_eq(got, tests[i].want_type))
            testing_t_errorf_v(t, "%s: got type %s, want %s", tests[i].want, got,
                               tests[i].want_type);
        if (!index_equal(types_selection_index(sel), tests[i].index, tests[i].nindex)) {
            Str g = index_string(a, types_selection_index(sel));
            Str w = want_index_string(a, tests[i].index, tests[i].nindex);
            testing_t_errorf_v(t, "%s: got index = %s, want %s", tests[i].want, g, w);
        }
        if (types_selection_indirect(sel) != tests[i].indirect)
            testing_t_errorf_v(t, "%s: got indirect = %t, want %t", tests[i].want,
                               types_selection_indirect(sel), tests[i].indirect);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLookupFieldOrMethodOnNil)                                                    \
    X(TestLookupFieldOrMethod)                                                         \
    X(TestNewMethodSet)                                                                \
    X(TestIssue60634)                                                                  \
    X(TestMissingMethodAlternative)                                                    \
    X(TestLookupSelection)                                                             \
    X(TestMethodSetString)                                                             \
    X(TestMethodExprSelection)

TESTING_MAIN(TESTS)
