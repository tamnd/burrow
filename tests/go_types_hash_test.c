/* Derived from Go's src/go/types/hash_test.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Go's test type checks a package and hashes the types it finds there. There
 * is no checker yet, so this builds the types the checker would make for that
 * source, which is kept next to the builders. Where Go calls Instantiate with
 * a nil context, which makes a new context each time, this uses a new context
 * each time as well. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/hash/maphash.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ---------------------------------------------------------------- builders */

/* What the types are built with: the allocator, package p, the context the
 * checker would instantiate in, and the generic type Constraint once it is
 * made. */
typedef struct World {
    Alloc *a;
    TypesPackage *pkg;
    TypesContext *ctxt;
    TypesType constraint;
} World;

static TypesTypeName *tname(World *w, Str name) {
    return types_new_type_name(w->a, TOKEN_NO_POS, w->pkg, name, NULL);
}

static TypesNamed *named(World *w, Str name, TypesType u) {
    return types_new_named(w->a, tname(w, name), u, slice_nil(TYPE_UNSAFE_POINTER));
}

/* the same, as a TypesType */
static TypesType named_t(World *w, Str name, TypesType u) {
    return types_type(named(w, name, u));
}

/* type name = rhs */
static TypesType alias_t(World *w, Str name, TypesType rhs) {
    return types_type(types_new_alias(w->a, tname(w, name), rhs));
}

static TypesType ptr(World *w, TypesType t) {
    return types_type(types_new_pointer(w->a, t));
}

static TypesTypeParam *tparam(World *w, Str name, TypesType constraint) {
    return types_new_type_param(w->a, tname(w, name), constraint);
}

static TypesType any_type(void) {
    return types_object_type(burrow__types_universe_any());
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

static TypesVar *param(World *w, Str name, TypesType typ) {
    return types_new_param(w->a, TOKEN_NO_POS, w->pkg, name, typ);
}

/* a tuple of unnamed parameters of the n types in ts */
static TypesTuple *tuple(World *w, const TypesType *ts, Int n) {
    Slice s = burrow__types_ptrs_make(w->a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(s, i, TypesVar *) = param(w, S(""), ts[i]);
    return types_new_tuple(w->a, s);
}

/* orig[targ], in the given context */
static TypesType inst(World *w, TypesContext *ctxt, TypesType orig, TypesType targ) {
    return burrow__types_instance(w->a, TOKEN_NO_POS, orig, ptrs1(w, targ), NULL, ctxt);
}

/* Go's Instantiate(nil, orig, targs, true), which uses a new context */
static TypesType instantiate(World *w, TypesType orig, TypesType targ) {
    return inst(w, types_new_context(w->a), orig, targ);
}

/* the type of the first method of n */
static TypesType method0(TypesType n) {
    return types_object_type(types_object(types_named_method(types_as_named(n), 0)));
}

/* interface{ embedded }, made implicit if it wraps a type set literal */
static TypesType iface_embed(World *w, TypesType embedded, bool implicit) {
    TypesInterface *it = types_new_interface_type(w->a, slice_nil(TYPE_UNSAFE_POINTER),
                                                  ptrs1(w, embedded));
    if (implicit)
        types_interface_mark_implicit(it);
    return types_type(it);
}

/* func (recv) M(G1[r], G2[r]) {} on the generic type n, where r is the
 * receiver's own type parameter, as the checker declares it */
static void g_method(World *w, TypesType n, TypesType g1, TypesType g2) {
    TypesTypeParam *r = tparam(w, S("P"), any_type());
    TypesType rt = types_type(r);
    TypesVar *recv = param(w, S(""), inst(w, w->ctxt, n, rt));
    TypesType ps[] = {inst(w, w->ctxt, g1, rt), inst(w, w->ctxt, g2, rt)};
    TypesSignature *sig = types_new_signature_type(
        w->a, recv, ptrs1(w, r), slice_nil(TYPE_UNSAFE_POINTER),
        tuple(w, ps, NELEM(ps)), NULL, false);
    types_named_add_method(types_as_named(n),
                           types_new_func(w->a, TOKEN_NO_POS, w->pkg, S("M"), sig));
}

/* func[name C](), with the given constraint, which may mention the type
 * parameter through make_constraint */
static TypesType generic_func(World *w, Str name,
                              TypesType (*make_constraint)(World *w, TypesType tp)) {
    TypesTypeParam *tp = tparam(w, name, NULL);
    types_type_param_set_constraint(tp, make_constraint(w, types_type(tp)));
    return types_type(types_new_signature_type(
        w->a, NULL, slice_nil(TYPE_UNSAFE_POINTER), ptrs1(w, tp), NULL, NULL, false));
}

/* --------------------------------------------------------------- TestHasher */

/* The types the test looks up in the scope, in the order Go declares them. */
typedef struct Types {
    TypesType T1, T2, T1M, T2M, G, GInt1, GInt2, GStr, C, CI, I, II, U;
    TypesType Fa1, Fa2, Fa1P, Fa2Q, Fb1, Fb1x, Fb1y, Fb2, Fb2x;
    TypesType G1, G1M, G1IntM1, G1IntM2, G1StrM, G2, G2IntM, ME1, ME1Type, ME2;
    TypesType Constraint, Foo, Fn, Bar, Baz, Quux, Issue56048, Issue56048b;
    TypesType NonAlias, Alias1, Alias2, Tagged1, Tagged2;
} Types;

static TypesType constraint_of(World *w, TypesType tp);
static TypesType constraint_quux(World *w, TypesType tp);
static TypesType constraint_any(World *w, TypesType tp);

static TypesType constraint_of(World *w, TypesType tp) {
    /* Constraint[tp] */
    return inst(w, w->ctxt, w->constraint, tp);
}

static TypesType constraint_any(World *w, TypesType tp) {
    (void)w;
    (void)tp;
    return any_type();
}

static TypesType constraint_quux(World *w, TypesType tp) {
    /* interface{ quux() } */
    (void)tp;
    TypesSignature *sig = types_new_signature(w->a, NULL, NULL, NULL, false);
    TypesFunc *m = types_new_func(w->a, TOKEN_NO_POS, w->pkg, S("quux"), sig);
    return types_type(
        types_new_interface_type(w->a, ptrs1(w, m), slice_nil(TYPE_UNSAFE_POINTER)));
}

/* type name interface{ m() R }, where R is what result makes of
 * interface{ name }, and the type of the method expression name.m */
static TypesType issue56048(World *w, Str name,
                            TypesType (*result)(World *w, TypesType j)) {
    TypesNamed *n = named(w, name, NULL);
    TypesType j = iface_embed(w, types_type(n), false);
    TypesType r = result(w, j);
    TypesSignature *msig =
        types_new_signature(w->a, NULL, NULL, tuple(w, &r, 1), false);
    TypesFunc *m = types_new_func(w->a, TOKEN_NO_POS, w->pkg, S("m"), msig);
    types_named_set_underlying(
        n, types_type(types_new_interface_type(w->a, ptrs1(w, m),
                                               slice_nil(TYPE_UNSAFE_POINTER))));
    TypesType recv = types_type(n);
    return types_type(
        types_new_signature(w->a, NULL, tuple(w, &recv, 1), tuple(w, &r, 1), false));
}

static TypesType result_same(World *w, TypesType j) {
    (void)w;
    return j;
}

static TypesType result_chan(World *w, TypesType j) {
    /* chan []*j */
    TypesType s = types_type(types_new_slice(w->a, ptr(w, j)));
    return types_type(types_new_chan(w->a, TYPES_SEND_RECV, s));
}

/* type name struct { F int "tag" } */
static TypesType tagged(World *w, Str tag) {
    TypesVar *f =
        types_new_field(w->a, TOKEN_NO_POS, w->pkg, S("F"), GT_TYP(TYPES_INT), false);
    Slice tags = slice_make(w->a, TYPE_STRING, 1, 1);
    *(Str *)slice_at(tags, 0) = tag;
    return types_type(types_new_struct(w->a, ptrs1(w, f), tags));
}

static void build(World *w, Types *ty) {
    Alloc *a = w->a;
    TypesType tInt = GT_TYP(TYPES_INT);
    TypesType tString = GT_TYP(TYPES_STRING);

    /* type T1 int; type T2 int; func (T1) M(int) {}; func (T2) M(int) {} */
    ty->T1 = named_t(w, S("T1"), tInt);
    ty->T2 = named_t(w, S("T2"), tInt);
    TypesType ts[2] = {ty->T1, ty->T2};
    for (Int i = 0; i < 2; i++) {
        TypesSignature *sig = types_new_signature(a, param(w, S(""), ts[i]),
                                                  tuple(w, &tInt, 1), NULL, false);
        types_named_add_method(types_as_named(ts[i]),
                               types_new_func(a, TOKEN_NO_POS, w->pkg, S("M"), sig));
    }
    ty->T1M = method0(ty->T1);
    ty->T2M = method0(ty->T2);

    /* type C interface { ~int | string } */
    TypesTerm *terms[] = {types_new_term(a, true, tInt),
                          types_new_term(a, false, tString)};
    ty->U = types_type(types_new_union(a, ptrs2(w, terms[0], terms[1])));
    ty->CI = iface_embed(w, ty->U, false);
    ty->C = named_t(w, S("C"), ty->CI);

    /* type I interface {} */
    ty->II = types_type(types_new_interface_type(a, slice_nil(TYPE_UNSAFE_POINTER),
                                                 slice_nil(TYPE_UNSAFE_POINTER)));
    ty->I = named_t(w, S("I"), ty->II);

    /* type G[P C] int */
    TypesNamed *g = named(w, S("G"), tInt);
    types_named_set_type_params(g, ptrs1(w, tparam(w, S("P"), ty->C)));
    ty->G = types_type(g);
    ty->GInt1 = instantiate(w, ty->G, tInt);
    ty->GInt2 = instantiate(w, ty->G, tInt);
    ty->GStr = instantiate(w, ty->G, tString);

    /* func Fa1[P C](p P) {}; func Fa2[Q C](q Q) {} */
    Str fa_tp[] = {S("P"), S("Q")}, fa_p[] = {S("p"), S("q")};
    TypesType *fa[] = {&ty->Fa1, &ty->Fa2}, *fap[] = {&ty->Fa1P, &ty->Fa2Q};
    for (Int i = 0; i < 2; i++) {
        TypesTypeParam *tp = tparam(w, fa_tp[i], ty->C);
        TypesVar *p = param(w, fa_p[i], types_type(tp));
        *fa[i] = types_type(types_new_signature_type(
            a, NULL, slice_nil(TYPE_UNSAFE_POINTER), ptrs1(w, tp),
            types_new_tuple(a, ptrs1(w, p)), NULL, false));
        *fap[i] = types_type(tp);
    }

    /* func Fb1[P any](x *P) { var y *P; _ = y }; func Fb2[Q any](x *Q) {} */
    TypesType *fb[] = {&ty->Fb1, &ty->Fb2}, *fbx[] = {&ty->Fb1x, &ty->Fb2x};
    for (Int i = 0; i < 2; i++) {
        TypesTypeParam *tp = tparam(w, fa_tp[i], any_type());
        TypesVar *x = param(w, S("x"), ptr(w, types_type(tp)));
        *fb[i] = types_type(types_new_signature_type(
            a, NULL, slice_nil(TYPE_UNSAFE_POINTER), ptrs1(w, tp),
            types_new_tuple(a, ptrs1(w, x)), NULL, false));
        *fbx[i] = types_object_type(types_object(x));
        if (i == 0)
            ty->Fb1y = ptr(w, types_type(tp));
    }

    /* type G1[P any] struct{ Field *G2[P] }; func (G1[P]) M(G1[P], G2[P]) {}
     * type G2[Q any] struct{ Field *G1[Q] }; func (G2[P]) M(G1[P], G2[P]) {} */
    TypesNamed *g1 = named(w, S("G1"), NULL), *g2 = named(w, S("G2"), NULL);
    TypesTypeParam *p1 = tparam(w, S("P"), any_type()),
                   *q2 = tparam(w, S("Q"), any_type());
    types_named_set_type_params(g1, ptrs1(w, p1));
    types_named_set_type_params(g2, ptrs1(w, q2));
    ty->G1 = types_type(g1);
    ty->G2 = types_type(g2);
    TypesVar *f1 =
        types_new_field(a, TOKEN_NO_POS, w->pkg, S("Field"),
                        ptr(w, inst(w, w->ctxt, ty->G2, types_type(p1))), false);
    TypesVar *f2 =
        types_new_field(a, TOKEN_NO_POS, w->pkg, S("Field"),
                        ptr(w, inst(w, w->ctxt, ty->G1, types_type(q2))), false);
    types_named_set_underlying(
        g1, types_type(types_new_struct(a, ptrs1(w, f1), slice_nil(TYPE_STRING))));
    types_named_set_underlying(
        g2, types_type(types_new_struct(a, ptrs1(w, f2), slice_nil(TYPE_STRING))));
    g_method(w, ty->G1, ty->G1, ty->G2);
    g_method(w, ty->G2, ty->G1, ty->G2);
    ty->G1M = method0(ty->G1);
    ty->G1IntM1 = method0(instantiate(w, ty->G1, tInt));
    ty->G1IntM2 = method0(instantiate(w, ty->G1, tInt));
    ty->G1StrM = method0(instantiate(w, ty->G1, tString));
    ty->G2IntM = method0(instantiate(w, ty->G2, tInt));

    /* var ME1 = G1[int].M; var ME2 = G2[int].M
     * var ME1Type func(G1[int], G1[int], G2[int]) */
    TypesType g1int = inst(w, w->ctxt, ty->G1, tInt);
    TypesType g2int = inst(w, w->ctxt, ty->G2, tInt);
    TypesType me1[] = {g1int, g1int, g2int}, me2[] = {g2int, g1int, g2int};
    ty->ME1 = types_type(types_new_signature(a, NULL, tuple(w, me1, 3), NULL, false));
    ty->ME1Type =
        types_type(types_new_signature(a, NULL, tuple(w, me1, 3), NULL, false));
    ty->ME2 = types_type(types_new_signature(a, NULL, tuple(w, me2, 3), NULL, false));

    /* type Constraint[T any] any
     * func Foo[T Constraint[T]]() {}; func Bar[P Constraint[P]]() {}
     * func Baz[Q any]() {}; func Quux[Q interface{ quux() }]() {} */
    TypesNamed *cn = named(w, S("Constraint"), types_type_underlying(any_type()));
    types_named_set_type_params(cn, ptrs1(w, tparam(w, S("T"), any_type())));
    ty->Constraint = w->constraint = types_type(cn);
    ty->Foo = generic_func(w, S("T"), constraint_of);
    ty->Bar = generic_func(w, S("P"), constraint_of);
    ty->Baz = generic_func(w, S("Q"), constraint_any);
    ty->Quux = generic_func(w, S("Q"), constraint_quux);

    /* func Fn[T1 ~*T2, T2 ~*T1](t1 T1, t2 T2) {} */
    TypesTypeParam *t1 = tparam(w, S("T1"), NULL), *t2 = tparam(w, S("T2"), NULL);
    TypesTerm *u1 = types_new_term(a, true, ptr(w, types_type(t2)));
    TypesTerm *u2 = types_new_term(a, true, ptr(w, types_type(t1)));
    types_type_param_set_constraint(
        t1, iface_embed(w, types_type(types_new_union(a, ptrs1(w, u1))), true));
    types_type_param_set_constraint(
        t2, iface_embed(w, types_type(types_new_union(a, ptrs1(w, u2))), true));
    TypesVar *v1 = param(w, S("t1"), types_type(t1));
    TypesVar *v2 = param(w, S("t2"), types_type(t2));
    TypesSignature *fn = types_new_signature_type(
        a, NULL, slice_nil(TYPE_UNSAFE_POINTER), ptrs2(w, t1, t2),
        types_new_tuple(a, ptrs2(w, v1, v2)), NULL, false);
    ty->Fn = types_type(fn);

    /* type Issue56048_I interface{ m() interface { Issue56048_I } }
     * var Issue56048 = Issue56048_I.m
     * type Issue56048_Ib interface{ m() chan []*interface { Issue56048_Ib } }
     * var Issue56048b = Issue56048_Ib.m */
    ty->Issue56048 = issue56048(w, S("Issue56048_I"), result_same);
    ty->Issue56048b = issue56048(w, S("Issue56048_Ib"), result_chan);

    /* type NonAlias int; type Alias1 = NonAlias; type Alias2 = NonAlias */
    ty->NonAlias = named_t(w, S("NonAlias"), tInt);
    ty->Alias1 = alias_t(w, S("Alias1"), ty->NonAlias);
    ty->Alias2 = alias_t(w, S("Alias2"), ty->NonAlias);

    /* type Tagged1 struct { F int "tag1" }; type Tagged2 struct { F int "tag2" } */
    ty->Tagged1 = tagged(w, S("tag1"));
    ty->Tagged2 = tagged(w, S("tag2"));
}

/* One equivalence class of types, of up to three. */
typedef struct Class {
    TypesType t[3];
    Int n;
} Class;

#define MAX_CLASSES 34

typedef struct Run {
    Alloc *a;
    MaphashHasher hasher;
    bool (*eq)(TypesType x, TypesType y);
    Class classes[MAX_CLASSES];
    Int n;
} Run;

static void add(Run *r, TypesType x, TypesType y, TypesType z) {
    Class *c = &r->classes[r->n++];
    c->t[0] = x;
    c->t[1] = y;
    c->t[2] = z;
    c->n = z != NULL ? 3 : y != NULL ? 2 : 1;
}

/* eqclasses groups the types into types_identical equivalence classes. */
static void eqclasses(Run *r, const Types *ty) {
    add(r, ty->T1, NULL, NULL);
    add(r, ty->T2, NULL, NULL);
    add(r, ty->G, NULL, NULL);
    add(r, ty->C, NULL, NULL);
    add(r, ty->CI, NULL, NULL);
    add(r, ty->U, NULL, NULL);
    add(r, ty->I, NULL, NULL);
    add(r, ty->II, NULL, NULL); /* should not be identical to CI */
    add(r, ty->T1M, ty->T2M, NULL);
    add(r, ty->GInt1, ty->GInt2, NULL);
    add(r, ty->GStr, NULL, NULL);
    add(r, ty->Fa1, ty->Fa2, NULL);
    add(r, ty->Fa1P, NULL, NULL);
    add(r, ty->Fa2Q, NULL, NULL);
    add(r, ty->Fb1y, ty->Fb1x, NULL);
    add(r, ty->Fb2x, NULL, NULL);
    add(r, ty->Fb1, ty->Fb2, NULL);
    add(r, ty->G1, NULL, NULL);
    add(r, ty->G1M, NULL, NULL);
    add(r, ty->G2, NULL, NULL);
    add(r, ty->G1IntM1, ty->G1IntM2, ty->G2IntM);
    add(r, ty->G1StrM, NULL, NULL);
    add(r, ty->ME1, ty->ME1Type, NULL);
    add(r, ty->ME2, NULL, NULL);
    add(r, ty->Constraint, NULL, NULL);
    add(r, ty->Foo, ty->Bar, NULL);
    add(r, ty->Baz, NULL, NULL);
    add(r, ty->Fn, NULL, NULL);
    add(r, ty->Quux, NULL, NULL);
    add(r, ty->Issue56048, NULL, NULL);
    add(r, ty->Issue56048b, NULL, NULL);
    add(r, ty->NonAlias, ty->Alias1, ty->Alias2);
}

static uint64_t hash_of(MaphashHasher hasher, MaphashSeed seed, TypesType t) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    maphash_hasher_hash(hasher, &h, &t);
    return maphash_hash_sum64(&h);
}

static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

static void run(void *env, TestingT *t) {
    Run *r = env;
    MaphashSeed seed = maphash_make_seed();

    for (Int xi = 0; xi < r->n; xi++) {
        const Class *cls = &r->classes[xi];
        TypesType tx = cls->t[0]; /* arbitrary representative of class */

        for (Int yi = 0; yi < r->n; yi++) {
            if (xi == yi) {
                /* Within a class, each element is equivalent to first and has
                 * the same hash. */
                for (Int i = 0; i < cls->n; i++) {
                    TypesType ty = cls->t[i];
                    uint64_t hx = hash_of(r->hasher, seed, tx),
                             hy = hash_of(r->hasher, seed, ty);
                    if (!r->eq(tx, ty) || hx != hy)
                        testing_t_fatalf_v(
                            t,
                            "class[%d][%d] (%s, hash %x) is not equivalent "
                            "to class[%d][%d] (%s, hash %x)",
                            xi, 0, type_string(r->a, tx), hx, yi, i,
                            type_string(r->a, ty), hy);
                }
            } else {
                /* Across classes, no element is equivalent to first. (We
                 * can't say for sure that the hashes are unequal.) */
                const Class *other = &r->classes[yi];
                for (Int k = 0; k < other->n; k++) {
                    TypesType typ = other->t[k];
                    if (r->eq(tx, typ))
                        testing_t_fatalf_v(t,
                                           "class[%d][%d] (%s) is equivalent to "
                                           "class[%d][%d] (%s)",
                                           xi, 0, type_string(r->a, tx), yi, k,
                                           type_string(r->a, typ));
                }
            }
        }
    }
}

static void TestHasher(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = {a, types_new_package(a, S("p"), S("p")), types_new_context(a), NULL};
    Types ty;
    build(&w, &ty);

    /* Hasher considers the two Tagged{1,2} types distinct. */
    Run r1 = {.a = a,
              .hasher = types_hasher_as_hasher((TypesHasher){0}),
              .eq = types_identical};
    eqclasses(&r1, &ty);
    add(&r1, ty.Tagged1, NULL, NULL);
    add(&r1, ty.Tagged2, NULL, NULL);
    testing_t_run(t, S("Hasher"), BURROW_FN(TestingTFunc, run, &r1));

    /* HasherIgnoreTags considers the two Tagged{1,2} types equal. */
    Run r2 = {.a = a,
              .hasher = types_hasher_ignore_tags_as_hasher((TypesHasherIgnoreTags){0}),
              .eq = types_identical_ignore_tags};
    eqclasses(&r2, &ty);
    add(&r2, ty.Tagged1, ty.Tagged2, NULL);
    testing_t_run(t, S("HasherIgnoreTags"), BURROW_FN(TestingTFunc, run, &r2));

    arena_free(&ar);
}

/* Not in Go: the hashers called directly agree with the adapters, a NULL
 * tuple hashes as the empty one, and an alias hashes as what it stands for. */
static void TestHasherDirect(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    World w = {a, types_new_package(a, S("p"), S("p")), types_new_context(a), NULL};
    MaphashSeed seed = maphash_make_seed();

    TypesType tInt = GT_TYP(TYPES_INT);
    TypesType sl = types_type(types_new_slice(a, tInt));
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    types_hasher_hash((TypesHasher){0}, &h, sl);
    uint64_t direct = maphash_hash_sum64(&h);
    uint64_t via = hash_of(types_hasher_as_hasher((TypesHasher){0}), seed, sl);
    if (direct != via)
        testing_t_errorf_v(t, "Hasher.Hash([]int) = %x, through the adapter %x", direct,
                           via);
    uint64_t it = hash_of(
        types_hasher_ignore_tags_as_hasher((TypesHasherIgnoreTags){0}), seed, sl);
    if (it == via)
        testing_t_errorf_v(t, "Hasher and HasherIgnoreTags agree on []int: %x", it);

    /* func() and the same with empty tuples */
    TypesType f0 = types_type(types_new_signature(a, NULL, NULL, NULL, false));
    TypesType f1 = types_type(types_new_signature(
        a, NULL, types_new_tuple(a, slice_nil(TYPE_UNSAFE_POINTER)), NULL, false));
    if (hash_of(types_hasher_as_hasher((TypesHasher){0}), seed, f0) !=
        hash_of(types_hasher_as_hasher((TypesHasher){0}), seed, f1))
        testing_t_error_v(t, "func() hashes two ways");

    /* interface{ m() }, built twice: identical, and so equal hashes, though
     * the method objects differ */
    TypesType ms[2];
    for (Int i = 0; i < 2; i++) {
        TypesSignature *sig = types_new_signature(a, NULL, NULL, NULL, false);
        TypesFunc *m = types_new_func(a, TOKEN_NO_POS, w.pkg, S("m"), sig);
        ms[i] = types_type(
            types_new_interface_type(a, ptrs1(&w, m), slice_nil(TYPE_UNSAFE_POINTER)));
    }
    bool eq = types_hasher_equal((TypesHasher){0}, ms[0], ms[1]);
    if (!eq)
        testing_t_error_v(t, "interface{ m() } is not identical to itself");
    if (hash_of(types_hasher_as_hasher((TypesHasher){0}), seed, ms[0]) !=
        hash_of(types_hasher_as_hasher((TypesHasher){0}), seed, ms[1]))
        testing_t_error_v(t, "interface{ m() } hashes two ways");

    bool it_eq = types_hasher_ignore_tags_equal((TypesHasherIgnoreTags){0},
                                                tagged(&w, S("a")), tagged(&w, S("b")));
    if (!it_eq)
        testing_t_error_v(t, "HasherIgnoreTags.Equal sees the tags");

    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHasher)                                                                      \
    X(TestHasherDirect)

TESTING_MAIN(TESTS)
