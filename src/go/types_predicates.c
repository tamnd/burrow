/* go/types: type terms, termlists and type sets, and the predicates on types,
 * Identical among them.
 *
 * Termlists are Slices of TypesTerm *, in which NULL is the empty term ∅ and
 * a term whose typ is NULL is 𝓤, the set of all types. What a function here
 * makes comes from the Alloc it is given, or for a type set from the
 * interface's own, and what it only needs while it runs, such as the seen
 * maps, comes from an arena of its own that it frees before it returns.
 *
 * Derived from Go's src/go/types/typeterm.go, termlist.go, typeset.go,
 * under.go and predicates.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_internal.h"

#include <stdbool.h>

/* union.go's maxTermCount: the most terms a union's type set may have. */
enum { GT_MAX_TERM_COUNT = 100 };

static Map *gt_seen_map(Alloc *a) {
    Map *m = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

static void gt_seen_set(Map *m, const void *k) {
    bool t = true;
    if (!map_set(m, &k, &t))
        burrow__types_oom();
}

static bool gt_seen_has(Map *m, const void *k) {
    return map_get(m, &k) != NULL;
}

/* ------------------------------------------------------------------ terms */

void burrow__types_write_term(BytesBuffer *buf, TypesTerm *x) {
    if (x == NULL) {
        burrow__types_buf_str(buf, BURROW_S("∅"));
    } else if (x->typ == NULL) {
        burrow__types_buf_str(buf, BURROW_S("𝓤"));
    } else {
        if (x->tilde)
            burrow__types_buf_byte(buf, '~');
        types_write_type(buf, x->typ, TYPES_NO_QUALIFIER);
    }
}

bool burrow__types_term_equal(TypesTerm *x, TypesTerm *y) {
    if (x == NULL || y == NULL)
        return x == y;
    if (x->typ == NULL || y->typ == NULL)
        return x->typ == y->typ;
    /* ~t ≡ ~t, t ≡ t */
    return x->tilde == y->tilde && types_identical(x->typ, y->typ);
}

bool burrow__types_term_disjoint(TypesTerm *x, TypesTerm *y) {
    TypesType ux = x->typ;
    if (y->tilde)
        ux = types_type_underlying(ux);
    TypesType uy = y->typ;
    if (x->tilde)
        uy = types_type_underlying(uy);
    return !types_identical(ux, uy);
}

void burrow__types_term_union(TypesTerm *x, TypesTerm *y, TypesTerm **r1,
                              TypesTerm **r2) {
    *r2 = NULL;
    /* The cases follow Go's one by one, so some bodies repeat.
     * NOLINTBEGIN(bugprone-branch-clone) */
    if (x == NULL && y == NULL) {
        *r1 = NULL; /* ∅ ∪ ∅ == ∅ */
    } else if (x == NULL) {
        *r1 = y; /* ∅ ∪ y == y */
    } else if (y == NULL) {
        *r1 = x; /* x ∪ ∅ == x */
    } else if (x->typ == NULL) {
        *r1 = x; /* 𝓤 ∪ y == 𝓤 */
    } else if (y->typ == NULL) {
        *r1 = y; /* x ∪ 𝓤 == 𝓤 */
    } else if (burrow__types_term_disjoint(x, y)) {
        /* x ∪ y == (x, y) if x ∩ y == ∅ */
        *r1 = x;
        *r2 = y;
    } else if (x->tilde || !y->tilde) {
        /* x ∪ y == x if x ⊇ y */
        *r1 = x;
    } else {
        *r1 = y;
    }
    /* NOLINTEND(bugprone-branch-clone) */
}

TypesTerm *burrow__types_term_intersect(TypesTerm *x, TypesTerm *y) {
    if (x == NULL || y == NULL)
        return NULL; /* ∅ ∩ y == ∅ and ∩ ∅ == ∅ */
    if (x->typ == NULL)
        return y; /* 𝓤 ∩ y == y */
    if (y->typ == NULL)
        return x; /* x ∩ 𝓤 == x */
    if (burrow__types_term_disjoint(x, y))
        return NULL; /* x ∩ y == ∅ if x ∩ y == ∅ */
    /* x ∩ y == x if x ⊆ y, else y */
    if (!x->tilde || y->tilde)
        return x;
    return y;
}

bool burrow__types_term_includes(TypesTerm *x, TypesType t) {
    if (x == NULL)
        return false; /* t ∈ ∅ == false */
    if (x->typ == NULL)
        return true; /* t ∈ 𝓤 == true */
    TypesType u = t;
    if (x->tilde)
        u = types_type_underlying(u);
    return types_identical(x->typ, u);
}

bool burrow__types_term_subset_of(TypesTerm *x, TypesTerm *y) {
    if (x == NULL)
        return true; /* ∅ ⊆ y == true */
    if (y == NULL)
        return false; /* x ⊆ ∅ == false since x != ∅ */
    if (y->typ == NULL)
        return true; /* x ⊆ 𝓤 == true */
    if (x->typ == NULL)
        return false; /* 𝓤 ⊆ y == false since y != 𝓤 */
    if (burrow__types_term_disjoint(x, y))
        return false; /* x ⊆ y == false if x ∩ y == ∅ */
    /* ~t ⊆ ~t, t ⊆ ~t and t ⊆ t, but not ~t ⊆ t */
    return !x->tilde || y->tilde;
}

/* -------------------------------------------------------------- termlists */

#define GT_TERM(xl, i) GT_AT(xl, i, TypesTerm *)

void burrow__types_write_termlist(BytesBuffer *buf, Slice xl) {
    if (xl.len == 0) {
        burrow__types_buf_str(buf, BURROW_S("∅"));
        return;
    }
    for (Int i = 0; i < xl.len; i++) {
        if (i > 0)
            burrow__types_buf_str(buf, BURROW_S(" | "));
        burrow__types_write_term(buf, GT_TERM(xl, i));
    }
}

Str burrow__types_termlist_string(Alloc *a, Slice xl) {
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_write_termlist(&buf, xl);
    return burrow__types_buf_done(&buf, a);
}

bool burrow__types_termlist_is_empty(Slice xl) {
    /* If there's a ∅ term, the entire list is not necessarily empty, so
     * every term is looked at. */
    for (Int i = 0; i < xl.len; i++)
        if (GT_TERM(xl, i) != NULL)
            return false;
    return true;
}

bool burrow__types_termlist_is_all(Slice xl) {
    for (Int i = 0; i < xl.len; i++) {
        TypesTerm *x = GT_TERM(xl, i);
        if (x != NULL && x->typ == NULL)
            return true;
    }
    return false;
}

Slice burrow__types_termlist_norm(Alloc *a, Slice xl) {
    /* Quadratic, which is fine for the short lists real code has. */
    bool *used = xl.len > 0 ? burrow__types_alloc(a, (size_t)xl.len, 1) : NULL;
    Slice rl = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < xl.len; i++) {
        TypesTerm *xi = GT_TERM(xl, i);
        if (xi == NULL || used[i])
            continue;
        for (Int j = i + 1; j < xl.len; j++) {
            TypesTerm *xj = GT_TERM(xl, j);
            if (xj == NULL || used[j])
                continue;
            TypesTerm *u1, *u2;
            burrow__types_term_union(xi, xj, &u1, &u2);
            if (u2 == NULL) {
                /* If we encounter a 𝓤 term, the entire list is 𝓤. */
                if (u1->typ == NULL)
                    return burrow__types_all_termlist();
                xi = u1;
                used[j] = true; /* xj is now unioned into xi */
            }
        }
        burrow__types_ptrs_append(a, &rl, xi);
    }
    return rl;
}

Slice burrow__types_termlist_union(Alloc *a, Slice xl, Slice yl) {
    Slice all = burrow__types_ptrs_make(a, xl.len + yl.len);
    for (Int i = 0; i < xl.len; i++)
        GT_TERM(all, i) = GT_TERM(xl, i);
    for (Int i = 0; i < yl.len; i++)
        GT_TERM(all, xl.len + i) = GT_TERM(yl, i);
    return burrow__types_termlist_norm(a, all);
}

Slice burrow__types_termlist_intersect(Alloc *a, Slice xl, Slice yl) {
    if (burrow__types_termlist_is_empty(xl) || burrow__types_termlist_is_empty(yl))
        return slice_nil(TYPE_UNSAFE_POINTER);
    /* Quadratic, as norm is. */
    Slice rl = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < xl.len; i++)
        for (Int j = 0; j < yl.len; j++) {
            TypesTerm *r = burrow__types_term_intersect(GT_TERM(xl, i), GT_TERM(yl, j));
            if (r != NULL)
                burrow__types_ptrs_append(a, &rl, r);
        }
    return burrow__types_termlist_norm(a, rl);
}

bool burrow__types_termlist_equal(Slice xl, Slice yl) {
    return burrow__types_termlist_subset_of(xl, yl) &&
           burrow__types_termlist_subset_of(yl, xl);
}

bool burrow__types_termlist_includes(Slice xl, TypesType t) {
    for (Int i = 0; i < xl.len; i++)
        if (burrow__types_term_includes(GT_TERM(xl, i), t))
            return true;
    return false;
}

bool burrow__types_termlist_superset_of(Slice xl, TypesTerm *y) {
    for (Int i = 0; i < xl.len; i++)
        if (burrow__types_term_subset_of(y, GT_TERM(xl, i)))
            return true;
    return false;
}

bool burrow__types_termlist_subset_of(Slice xl, Slice yl) {
    if (burrow__types_termlist_is_empty(yl))
        return burrow__types_termlist_is_empty(xl);
    /* each term x of xl must be a subset of yl */
    for (Int i = 0; i < xl.len; i++)
        if (!burrow__types_termlist_superset_of(yl, GT_TERM(xl, i)))
            return false; /* x is not a subset yl */
    return true;
}

/* -------------------------------------------------------------- type sets */

bool burrow__types_type_set_is_empty(GtTypeSet *s) {
    return burrow__types_termlist_is_empty(s->terms);
}

bool burrow__types_type_set_is_method_set(GtTypeSet *s) {
    return !s->comparable && burrow__types_termlist_is_all(s->terms);
}

bool burrow__types_type_set_is_all(GtTypeSet *s) {
    return burrow__types_type_set_is_method_set(s) && s->methods.len == 0;
}

bool burrow__types_type_set_has_terms(GtTypeSet *s) {
    return !burrow__types_termlist_is_empty(s->terms) &&
           !burrow__types_termlist_is_all(s->terms);
}

bool burrow__types_type_set_subset_of(GtTypeSet *s1, GtTypeSet *s2) {
    return burrow__types_termlist_subset_of(s1->terms, s2->terms);
}

static bool gt_term_comparable(void *env, TypesTerm *t) {
    return t != NULL && burrow__types_comparable_type(t->typ, false, env);
}

bool burrow__types_type_set_is_comparable(GtTypeSet *s, Map *seen) {
    if (burrow__types_termlist_is_all(s->terms))
        return s->comparable;
    return burrow__types_type_set_is(s,
                                     BURROW_FN(GtTermFunc, gt_term_comparable, seen));
}

Str burrow__types_type_set_string(Alloc *a, GtTypeSet *s) {
    if (burrow__types_type_set_is_empty(s))
        return BURROW_S("∅");
    if (burrow__types_type_set_is_all(s))
        return BURROW_S("𝓤");

    bool has_methods = s->methods.len > 0;
    bool has_terms = burrow__types_type_set_has_terms(s);

    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_buf_byte(&buf, '{');
    if (s->comparable) {
        burrow__types_buf_str(&buf, BURROW_S("comparable"));
        if (has_methods || has_terms)
            burrow__types_buf_str(&buf, BURROW_S("; "));
    }
    for (Int i = 0; i < s->methods.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S("; "));
        burrow__types_write_object(
            &buf, types_object(GT_AT(s->methods, i, TypesFunc *)), TYPES_NO_QUALIFIER);
    }
    if (has_methods && has_terms)
        burrow__types_buf_str(&buf, BURROW_S("; "));
    if (has_terms)
        burrow__types_write_termlist(&buf, s->terms);
    burrow__types_buf_byte(&buf, '}');
    return burrow__types_buf_done(&buf, a);
}

bool burrow__types_type_set_all(GtTypeSet *s, GtTypePairFunc f) {
    if (!burrow__types_type_set_has_terms(s))
        return BURROW_CALLF(f, NULL, NULL);
    for (Int i = 0; i < s->terms.len; i++) {
        TypesTerm *t = GT_TERM(s->terms, i);
        burrow__types_assert(t->typ != NULL);
        /* Unalias(x).Underlying() is needed for the case where x is an
         * alias. Underlying is only needed for a type without a tilde,
         * since a tilde term's type is already its underlying type. */
        TypesType u = types_unalias(t->typ);
        if (!t->tilde)
            u = types_type_underlying(u);
        if (!BURROW_CALLF(f, t->typ, u))
            return false;
    }
    return true;
}

bool burrow__types_type_set_is(GtTypeSet *s, GtTermFunc f) {
    if (!burrow__types_type_set_has_terms(s))
        return BURROW_CALLF(f, NULL);
    for (Int i = 0; i < s->terms.len; i++) {
        TypesTerm *t = GT_TERM(s->terms, i);
        burrow__types_assert(t->typ != NULL);
        if (!BURROW_CALLF(f, t))
            return false;
    }
    return true;
}

/* invalidTypeSet: what a union with too many terms gets. Never written. */
static GtTypeSet gt_invalid_type_set;

GtTypeSet *burrow__types_invalid_type_set(void) {
    return &gt_invalid_type_set;
}

static GtTypeSet *gt_union_set_get(Map *sets, TypesUnion *u) {
    GtTypeSet **p = map_get(sets, &u);
    return p != NULL ? *p : NULL;
}

static void gt_union_set_put(Map *sets, TypesUnion *u, GtTypeSet *s) {
    if (!map_set(sets, &u, &s))
        burrow__types_oom();
}

/* computeUnionTypeSet with a nil checker. The type set and its terms come
 * from a, and sets, a map from TypesUnion * to GtTypeSet *, from wherever
 * the caller made it. */
static GtTypeSet *gt_compute_union_type_set(Alloc *a, Map *sets, TokenPos pos,
                                            TypesUnion *utyp) {
    GtTypeSet *tset = gt_union_set_get(sets, utyp);
    if (tset != NULL)
        return tset;

    /* avoid infinite recursion (see comment in computeInterfaceTypeSet) */
    tset = burrow__types_alloc(a, sizeof *tset, _Alignof(GtTypeSet));
    gt_union_set_put(sets, utyp, tset);

    /* compute the type set from the terms of the union */
    Slice all_terms = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < utyp->terms.len; i++) {
        TypesTerm *t = GT_TERM(utyp->terms, i);
        Slice terms;
        TypesType u = types_type_underlying(t->typ);
        TypesInterface *ui = types_as_interface(u);
        if (ui != NULL) {
            /* For now we don't permit type parameters as constraints. */
            burrow__types_assert(!burrow__types_is_type_param(t->typ));
            terms = burrow__types_compute_interface_type_set(pos, ui)->terms;
        } else if (!burrow__types_is_valid(u)) {
            continue;
        } else {
            if (t->tilde && !types_identical(t->typ, u))
                /* There is no underlying type which is t.typ. The intersection
                 * of t and t.typ is empty. */
                t = NULL; /* ∅ term */
            terms = burrow__types_ptrs_make(a, 1);
            GT_TERM(terms, 0) = t;
        }
        /* The type set of a union expression is the union of the type sets
         * of each term. */
        all_terms = burrow__types_termlist_union(a, all_terms, terms);
        if (all_terms.len > GT_MAX_TERM_COUNT) {
            gt_union_set_put(sets, utyp, &gt_invalid_type_set);
            return &gt_invalid_type_set;
        }
    }
    tset->terms = all_terms;
    return tset;
}

/* intersectTermLists: the intersection of two type sets' terms, and whether
 * it is comparable. */
static Slice gt_intersect_term_lists(Alloc *a, Slice xterms, bool xcomp, Slice yterms,
                                     bool ycomp, bool *comp_out) {
    Slice terms = burrow__types_termlist_intersect(a, xterms, yterms);
    /* If one of xterms or yterms is marked as comparable, the result must
     * only include comparable types. */
    bool comp = xcomp || ycomp;
    if (comp && !burrow__types_termlist_is_all(terms)) {
        /* only keep comparable terms */
        Slice kept = burrow__types_ptrs_make(a, terms.len);
        Int i = 0;
        for (Int j = 0; j < terms.len; j++) {
            TypesTerm *t = GT_TERM(terms, j);
            burrow__types_assert(t->typ != NULL);
            if (burrow__types_comparable_type(t->typ, false /* strictly comparable */,
                                              NULL))
                GT_TERM(kept, i++) = t;
        }
        kept.len = i;
        terms = kept;
        if (!burrow__types_termlist_is_all(terms))
            comp = false;
    }
    burrow__types_assert(!comp || burrow__types_termlist_is_all(terms)); /* invariant */
    *comp_out = comp;
    return terms;
}

GtTypeSet *burrow__types_compute_interface_type_set(TokenPos pos,
                                                    TypesInterface *ityp) {
    /* pos only goes into errors, and those need the checker. */
    (void)pos;
    if (ityp->tset != NULL)
        return ityp->tset;

    /* If the interface is not fully set up yet, the type set will not be
     * complete, which may lead to errors when using the type set (e.g. missing
     * method). Don't compute a partial type set (and don't store it!), so
     * that we still compute the full type set eventually. Instead, return the
     * top type set and let any follow-on errors play out. */
    if (!ityp->complete)
        return burrow__types_top_type_set;

    Alloc *a = ityp->a;

    /* An infinitely expanding interface (due to a cycle) is detected
     * elsewhere (Checker.validType), so here we simply assume we only have
     * valid interfaces. Mark the interface as complete to avoid infinite
     * recursion if the validType check occurs later for some reason. */
    GtTypeSet *tset = burrow__types_alloc(a, sizeof *tset, _Alignof(GtTypeSet));
    tset->terms = burrow__types_all_termlist();
    ityp->tset = tset;

    /* The scratch: the union sets and the methods seen so far. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *sa = arena_allocator(&ar);
    Map *union_sets = map_make(sa, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    if (union_sets == NULL)
        burrow__types_oom();
    GtObjset seen = {sa, NULL};

    /* Methods of embedded interfaces are collected unchanged; i.e., the
     * identity of a method I.m's Func Object of an interface I is the same
     * as that of the method m in an interface that embeds interface I. With
     * no checker there is nobody to report a duplicate method to, so the
     * first one is kept and the others dropped. */
    Slice all_methods = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < ityp->methods.len; i++) {
        TypesFunc *m = GT_AT(ityp->methods, i, TypesFunc *);
        if (burrow__types_objset_insert(&seen, types_object(m)) == NULL)
            burrow__types_ptrs_append(a, &all_methods, m);
    }

    /* collect embedded elements */
    Slice all_terms = burrow__types_all_termlist();
    bool all_comparable = false;
    for (Int i = 0; i < ityp->embeddeds.len; i++) {
        TypesType typ = GT_AT(ityp->embeddeds, i, TypesType);
        /* The embedding position is nil for imported interfaces. We don't
         * need to do version checks in those cases. */
        TokenPos epos = TOKEN_NO_POS;
        if (ityp->embed_pos.len > i)
            epos = GT_AT(ityp->embed_pos, i, TokenPos);
        bool comparable = false;
        Slice terms;
        TypesType u = types_type_underlying(typ);
        if (u != NULL && u->kind == TYPES_KIND_INTERFACE) {
            /* For now we don't permit type parameters as constraints. */
            burrow__types_assert(!burrow__types_is_type_param(typ));
            GtTypeSet *ts =
                burrow__types_compute_interface_type_set(epos, types_as_interface(u));
            comparable = ts->comparable;
            for (Int j = 0; j < ts->methods.len; j++) {
                TypesFunc *m = GT_AT(ts->methods, j, TypesFunc *);
                if (burrow__types_objset_insert(&seen, types_object(m)) == NULL)
                    burrow__types_ptrs_append(a, &all_methods, m);
            }
            terms = ts->terms;
        } else if (u != NULL && u->kind == TYPES_KIND_UNION) {
            GtTypeSet *ts =
                gt_compute_union_type_set(a, union_sets, epos, types_as_union(u));
            if (ts == &gt_invalid_type_set)
                continue; /* ignore invalid unions */
            burrow__types_assert(!ts->comparable);
            burrow__types_assert(ts->methods.len == 0);
            terms = ts->terms;
        } else {
            if (!burrow__types_is_valid(u))
                continue;
            TypesTerm *t = burrow__types_alloc(a, sizeof *t, _Alignof(TypesTerm));
            t->tilde = false;
            t->typ = typ;
            terms = burrow__types_ptrs_make(a, 1);
            GT_TERM(terms, 0) = t;
        }
        /* The type set of an interface is the intersection of the type sets
         * of all its elements. Due to language restrictions, only embedded
         * interfaces can add methods, they are handled separately. Here we
         * only need to intersect the term lists and comparable bits. */
        all_terms = gt_intersect_term_lists(a, all_terms, all_comparable, terms,
                                            comparable, &all_comparable);
    }
    arena_free(&ar);

    tset->comparable = all_comparable;
    if (all_methods.len != 0) {
        burrow__types_sort_methods(all_methods);
        tset->methods = all_methods;
    }
    tset->terms = all_terms;
    return tset;
}

/* ------------------------------------------------------------------ under */

bool burrow__types_all(TypesType t, GtTypePairFunc f) {
    TypesTypeParam *p = types_as_type_param(types_unalias(t));
    if (p != NULL)
        return burrow__types_type_set_all(
            burrow__types_iface_type_set(burrow__types_type_param_iface(p)), f);
    return BURROW_CALLF(f, t, types_type_underlying(t));
}

bool burrow__types_type_param_is(TypesTypeParam *t, GtTermFunc f) {
    return burrow__types_type_set_is(
        burrow__types_iface_type_set(burrow__types_type_param_iface(t)), f);
}

/* ------------------------------------------------------------- predicates */

bool burrow__types_is_valid(TypesType t) {
    return types_unalias(t) != GT_TYP(TYPES_INVALID);
}

bool burrow__types_is_basic(TypesType t, TypesBasicInfo info) {
    TypesBasic *u = types_as_basic(types_type_underlying(t));
    return u != NULL && (u->info & info) != 0;
}

static bool gt_term_is_basic(void *env, TypesTerm *t) {
    return t != NULL && burrow__types_is_basic(t->typ, *(const TypesBasicInfo *)env);
}

bool burrow__types_all_basic(TypesType t, TypesBasicInfo info) {
    TypesTypeParam *tpar = types_as_type_param(types_unalias(t));
    if (tpar != NULL)
        return burrow__types_type_set_is(
            burrow__types_iface_type_set(burrow__types_type_param_iface(tpar)),
            BURROW_FN(GtTermFunc, gt_term_is_basic, &info));
    return burrow__types_is_basic(t, info);
}

bool burrow__types_has_name(TypesType t) {
    TypesType u = types_unalias(t);
    if (u == NULL)
        return false;
    switch (u->kind) {
    case TYPES_KIND_BASIC:
    case TYPES_KIND_NAMED:
    case TYPES_KIND_TYPE_PARAM:
        return true;
    default:
        return false;
    }
}

bool burrow__types_is_type_lit(TypesType t) {
    TypesType u = types_unalias(t);
    if (u == NULL)
        return true;
    switch (u->kind) {
    case TYPES_KIND_NAMED:
    case TYPES_KIND_TYPE_PARAM:
        return false;
    default:
        return true;
    }
}

bool burrow__types_is_typed(TypesType t) {
    TypesBasic *b = types_as_basic(t);
    return b == NULL || (b->info & TYPES_IS_UNTYPED) == 0;
}

bool burrow__types_is_untyped_numeric(TypesType t) {
    TypesBasic *b = types_as_basic(t);
    return b != NULL && (b->info & TYPES_IS_UNTYPED) != 0 &&
           (b->info & TYPES_IS_NUMERIC) != 0;
}

bool types_is_interface(TypesType t) {
    return types_as_interface(types_type_underlying(t)) != NULL;
}

bool burrow__types_is_type_param(TypesType t) {
    return types_as_type_param(types_unalias(t)) != NULL;
}

bool burrow__types_is_non_type_param_interface(TypesType t) {
    return !burrow__types_is_type_param(t) && types_is_interface(t);
}

bool burrow__types_has_empty_typeset(TypesType t) {
    TypesTypeParam *tpar = types_as_type_param(types_unalias(t));
    if (tpar != NULL && tpar->bound != NULL) {
        TypesInterface *iface =
            types_as_interface(burrow__types_safe_underlying(tpar->bound));
        return iface != NULL && iface->tset != NULL &&
               burrow__types_type_set_is_empty(iface->tset);
    }
    return false;
}

bool burrow__types_is_generic(TypesType t) {
    /* A parameterized type is only instantiated if it doesn't have an
     * instantiation already. */
    TypesAlias *alias =
        t != NULL && t->kind == TYPES_KIND_ALIAS ? types_as_alias(t) : NULL;
    if (alias != NULL && alias->tparams != NULL && alias->targs == NULL)
        return true;
    TypesNamed *named = burrow__types_as_named(t);
    return named != NULL && named->obj != NULL && named->inst == NULL &&
           burrow__types_tparam_list(types_named_type_params(named)).len > 0;
}

bool types_comparable(TypesType t) {
    return burrow__types_comparable_type(t, true, NULL);
}

static bool gt_comparable_type(TypesType t, bool dynamic, Map *seen) {
    if (gt_seen_has(seen, t))
        return true;
    gt_seen_set(seen, t);

    TypesType u = types_type_underlying(t);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        /* assume invalid types to be comparable to avoid follow-up errors */
        return types_as_basic(u)->kind != TYPES_UNTYPED_NIL;
    case TYPES_KIND_POINTER:
    case TYPES_KIND_CHAN:
        return true;
    case TYPES_KIND_STRUCT: {
        TypesStruct *st = types_as_struct(u);
        for (Int i = 0; i < st->fields.len; i++) {
            TypesVar *f = GT_AT(st->fields, i, TypesVar *);
            if (!gt_comparable_type(GT_OBJ(f)->typ, dynamic, seen))
                return false; /* struct containing f.typ cannot be compared */
        }
        return true;
    }
    case TYPES_KIND_ARRAY:
        return gt_comparable_type(types_as_array(u)->elem, dynamic, seen);
    case TYPES_KIND_INTERFACE:
        return (dynamic && !burrow__types_is_type_param(t)) ||
               burrow__types_type_set_is_comparable(
                   burrow__types_iface_type_set(types_as_interface(u)), seen);
    default:
        return false;
    }
}

bool burrow__types_comparable_type(TypesType t, bool dynamic, Map *seen) {
    if (seen != NULL)
        return gt_comparable_type(t, dynamic, seen);
    Arena ar;
    arena_init(&ar, NULL, 0);
    bool ok = gt_comparable_type(t, dynamic, gt_seen_map(arena_allocator(&ar)));
    arena_free(&ar);
    return ok;
}

static bool gt_under_has_nil(void *env, TypesType t, TypesType u) {
    (void)env;
    (void)t;
    return u != NULL && burrow__types_has_nil(u);
}

bool burrow__types_has_nil(TypesType t) {
    TypesType u = types_type_underlying(t);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        return types_as_basic(u)->kind == TYPES_UNSAFE_POINTER;
    case TYPES_KIND_SLICE:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        return true;
    case TYPES_KIND_INTERFACE:
        return !burrow__types_is_type_param(t) ||
               burrow__types_all(t, BURROW_FN(GtTypePairFunc, gt_under_has_nil, NULL));
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- Identical */

/* An ifacePair is a node in a stack of interface type pairs compared for
 * identity. */
typedef struct GtIfacePair {
    TypesInterface *x, *y;
    struct GtIfacePair *prev;
} GtIfacePair;

static bool gt_iface_pair_identical(GtIfacePair *p, GtIfacePair *q) {
    return (p->x == q->x && p->y == q->y) || (p->x == q->y && p->y == q->x);
}

/* A comparer is used to compare types. */
typedef struct GtComparer {
    bool ignore_tags;     /* if set, identical ignores struct tags */
    bool ignore_invalids; /* if set, an invalid type is identical to any type */
} GtComparer;

static bool gt_identical(const GtComparer *c, TypesType x, TypesType y, GtIfacePair *p);

static bool gt_identical_signature(const GtComparer *c, TypesSignature *x,
                                   TypesSignature *y, GtIfacePair *p) {
    Slice xtparams = burrow__types_tparam_list(x->tparams);
    Slice ytparams = burrow__types_tparam_list(y->tparams);
    if (xtparams.len != ytparams.len)
        return false;

    /* In the case of generic signatures, we will substitute in yparams and
     * yresults. */
    TypesType yparams = types_type(y->params);
    TypesType yresults = types_type(y->results);
    bool ok = true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    if (xtparams.len > 0) {
        /* We must ignore type parameter names when comparing x and y. The
         * easiest way to do this is to substitute x's type parameters for
         * y's. */
        Alloc *a = arena_allocator(&ar);
        Map *smap = burrow__types_make_subst_map(a, ytparams, xtparams);
        /* Constraints must be pair-wise identical, after substitution. */
        TypesContext *ctxt = types_new_context(a);
        for (Int i = 0; i < xtparams.len && ok; i++) {
            TypesTypeParam *xt = GT_AT(xtparams, i, TypesTypeParam *);
            TypesTypeParam *yt = GT_AT(ytparams, i, TypesTypeParam *);
            TypesType ybound =
                burrow__types_subst(a, TOKEN_NO_POS, yt->bound, smap, NULL, ctxt);
            if (!gt_identical(c, xt->bound, ybound, p))
                ok = false;
        }
        if (ok) {
            if (yparams != NULL)
                yparams =
                    burrow__types_subst(a, TOKEN_NO_POS, yparams, smap, NULL, ctxt);
            if (yresults != NULL)
                yresults =
                    burrow__types_subst(a, TOKEN_NO_POS, yresults, smap, NULL, ctxt);
        }
    }
    ok = ok && x->variadic == y->variadic &&
         gt_identical(c, types_type(x->params), yparams, p) &&
         gt_identical(c, types_type(x->results), yresults, p);
    arena_free(&ar);
    return ok;
}

static bool gt_identical_interface(const GtComparer *c, TypesInterface *x,
                                   TypesInterface *y, GtIfacePair *p) {
    /* Two interface types are identical if they describe the same type sets.
     * With the existing implementation restriction, this simplifies to:
     *
     * Two interface types are identical if they have the same set of methods
     * with the same names and identical function types, and if any type
     * restrictions are the same. Lower-case method names from different
     * packages are always different. The order of the methods is
     * irrelevant. */
    GtTypeSet *xset = burrow__types_iface_type_set(x);
    GtTypeSet *yset = burrow__types_iface_type_set(y);
    if (xset->comparable != yset->comparable)
        return false;
    if (!burrow__types_termlist_equal(xset->terms, yset->terms))
        return false;
    Slice a = xset->methods;
    Slice b = yset->methods;
    if (a.len != b.len)
        return false;
    /* Interface types are the only types where cycles can occur that are
     * not "terminated" via named types; and such cycles can only be created
     * via method parameter types that are anonymous interfaces (directly or
     * indirectly) embedding the current interface. To prevent endless
     * recursions, keep track of the interface pairs being compared. If a
     * pair is seen again, the interfaces are assumed identical. */
    GtIfacePair q = {x, y, p};
    for (GtIfacePair *r = p; r != NULL; r = r->prev)
        if (gt_iface_pair_identical(r, &q))
            return true; /* same pair was compared before */
    for (Int i = 0; i < a.len; i++) {
        GtObject *f = GT_OBJ(GT_AT(a, i, TypesFunc *));
        GtObject *g = GT_OBJ(GT_AT(b, i, TypesFunc *));
        if (!burrow__types_id_eq(f->pkg, f->name, g->pkg, g->name) ||
            !gt_identical(c, f->typ, g->typ, &q))
            return false;
    }
    return true;
}

static bool gt_identical(const GtComparer *c, TypesType x, TypesType y,
                         GtIfacePair *p) {
    x = types_unalias(x);
    y = types_unalias(y);

    if (x == y)
        return true;
    if (c->ignore_invalids &&
        (!burrow__types_is_valid(x) || !burrow__types_is_valid(y)))
        return true;
    if (x == NULL || y == NULL)
        return false;

    switch ((TypesTypeKind)x->kind) {
    case TYPES_KIND_BASIC:
        /* Basic types are singletons except for the rune and byte aliases,
         * thus we cannot solely rely on the x == y check above. */
        if (y->kind == TYPES_KIND_BASIC)
            return types_as_basic(x)->kind == types_as_basic(y)->kind;
        return false;

    case TYPES_KIND_ARRAY:
        if (y->kind == TYPES_KIND_ARRAY) {
            /* Two array types are identical if they have identical element
             * types and the same array length. */
            TypesArray *xa = types_as_array(x), *ya = types_as_array(y);
            /* If one or both array lengths are unknown (< 0) due to some
             * error, assume they are the same to avoid spurious follow-on
             * errors. */
            return (xa->len < 0 || ya->len < 0 || xa->len == ya->len) &&
                   gt_identical(c, xa->elem, ya->elem, p);
        }
        return false;

    case TYPES_KIND_SLICE:
        /* Two slice types are identical if they have identical element
         * types. */
        if (y->kind == TYPES_KIND_SLICE)
            return gt_identical(c, types_as_slice(x)->elem, types_as_slice(y)->elem, p);
        return false;

    case TYPES_KIND_STRUCT:
        if (y->kind == TYPES_KIND_STRUCT) {
            /* Two struct types are identical if they have the same sequence
             * of fields, and if corresponding fields have the same names,
             * and identical types, and identical tags. Two embedded fields
             * are considered to have the same name. Lower-case field names
             * from different packages are always different. */
            TypesStruct *xs = types_as_struct(x), *ys = types_as_struct(y);
            if (xs->fields.len != ys->fields.len)
                return false;
            for (Int i = 0; i < xs->fields.len; i++) {
                TypesVar *f = GT_AT(xs->fields, i, TypesVar *);
                TypesVar *g = GT_AT(ys->fields, i, TypesVar *);
                if (f->embedded != g->embedded ||
                    (!c->ignore_tags &&
                     !str_eq(types_struct_tag(xs, i), types_struct_tag(ys, i))) ||
                    !burrow__types_same_id(types_object(f), GT_OBJ(g)->pkg,
                                           GT_OBJ(g)->name, false) ||
                    !gt_identical(c, GT_OBJ(f)->typ, GT_OBJ(g)->typ, p))
                    return false;
            }
            return true;
        }
        return false;

    case TYPES_KIND_POINTER:
        /* Two pointer types are identical if they have identical base
         * types. */
        if (y->kind == TYPES_KIND_POINTER)
            return gt_identical(c, types_as_pointer(x)->base, types_as_pointer(y)->base,
                                p);
        return false;

    case TYPES_KIND_TUPLE:
        /* Two tuples types are identical if they have the same number of
         * elements and corresponding elements have identical types. */
        if (y->kind == TYPES_KIND_TUPLE) {
            TypesTuple *xt = types_as_tuple(x), *yt = types_as_tuple(y);
            if (xt->vars.len != yt->vars.len)
                return false;
            for (Int i = 0; i < xt->vars.len; i++) {
                TypesVar *v = GT_AT(xt->vars, i, TypesVar *);
                TypesVar *w = GT_AT(yt->vars, i, TypesVar *);
                if (!gt_identical(c, GT_OBJ(v)->typ, GT_OBJ(w)->typ, p))
                    return false;
            }
            return true;
        }
        return false;

    case TYPES_KIND_SIGNATURE:
        /* Two function types are identical if they have the same number of
         * parameters and result values, corresponding parameter and result
         * types are identical, and either both functions are variadic or
         * neither is. Parameter and result names are not required to
         * match, and type parameters are considered identical modulo
         * renaming. */
        if (y->kind == TYPES_KIND_SIGNATURE)
            return gt_identical_signature(c, types_as_signature(x),
                                          types_as_signature(y), p);
        return false;

    case TYPES_KIND_UNION:
        if (y->kind == TYPES_KIND_UNION) {
            /* TODO(rfindley): can this be reached during type checking? If
             * so, consider passing a type set map. */
            Arena ar;
            arena_init(&ar, NULL, 0);
            Alloc *a = arena_allocator(&ar);
            Map *sets = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            if (sets == NULL)
                burrow__types_oom();
            GtTypeSet *xset =
                gt_compute_union_type_set(a, sets, TOKEN_NO_POS, types_as_union(x));
            GtTypeSet *yset =
                gt_compute_union_type_set(a, sets, TOKEN_NO_POS, types_as_union(y));
            bool eq = burrow__types_termlist_equal(xset->terms, yset->terms);
            arena_free(&ar);
            return eq;
        }
        return false;

    case TYPES_KIND_INTERFACE:
        if (y->kind == TYPES_KIND_INTERFACE)
            return gt_identical_interface(c, types_as_interface(x),
                                          types_as_interface(y), p);
        return false;

    case TYPES_KIND_MAP:
        /* Two map types are identical if they have identical key and value
         * types. */
        if (y->kind == TYPES_KIND_MAP) {
            TypesMap *xm = types_as_map(x), *ym = types_as_map(y);
            return gt_identical(c, xm->key, ym->key, p) &&
                   gt_identical(c, xm->elem, ym->elem, p);
        }
        return false;

    case TYPES_KIND_CHAN:
        /* Two channel types are identical if they have identical value
         * types and the same direction. */
        if (y->kind == TYPES_KIND_CHAN) {
            TypesChan *xc = types_as_chan(x), *yc = types_as_chan(y);
            return xc->dir == yc->dir && gt_identical(c, xc->elem, yc->elem, p);
        }
        return false;

    case TYPES_KIND_NAMED: {
        /* Two named types are identical if their type names originate in
         * the same type declaration; if they are instantiated they must have
         * identical type argument lists. */
        TypesNamed *yn = burrow__types_as_named(y);
        if (yn == NULL)
            return false;
        TypesNamed *xn = types_as_named(x);
        /* check type arguments before origins to match unifier (see
         * go.dev/issue/63655) */
        Slice xargs = burrow__types_type_list(types_named_type_args(xn));
        Slice yargs = burrow__types_type_list(types_named_type_args(yn));
        if (xargs.len != yargs.len)
            return false;
        for (Int i = 0; i < xargs.len; i++)
            if (!types_identical(GT_AT(xargs, i, TypesType),
                                 GT_AT(yargs, i, TypesType)))
                return false;
        return burrow__types_identical_origin(xn, yn);
    }

    case TYPES_KIND_TYPE_PARAM:
        /* nothing to do (x and y being equal is caught in the very
         * beginning of this function) */
        return false;

    case TYPES_KIND_ALIAS:
    case TYPES_KIND_INVALID:
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

bool types_identical(TypesType x, TypesType y) {
    GtComparer c = {false, false};
    return gt_identical(&c, x, y, NULL);
}

bool types_identical_ignore_tags(TypesType x, TypesType y) {
    GtComparer c = {true, false};
    return gt_identical(&c, x, y, NULL);
}

bool burrow__types_identical_origin(TypesNamed *x, TypesNamed *y) {
    /* TODO(gri) is this correct? */
    return types_named_origin(x)->obj == types_named_origin(y)->obj;
}

bool burrow__types_identical_instance(TypesType xorig, Slice xargs, TypesType yorig,
                                      Slice yargs) {
    if (xargs.len != yargs.len)
        return false;
    for (Int i = 0; i < xargs.len; i++)
        if (!types_identical(GT_AT(xargs, i, TypesType), GT_AT(yargs, i, TypesType)))
            return false;
    return types_identical(xorig, yorig);
}

TypesType types_default(TypesType t) {
    TypesBasic *b = t != NULL && t->kind == TYPES_KIND_BASIC ? types_as_basic(t) : NULL;
    if (b != NULL) {
        switch (b->kind) {
        case TYPES_UNTYPED_BOOL:
            return GT_TYP(TYPES_BOOL);
        case TYPES_UNTYPED_INT:
            return GT_TYP(TYPES_INT);
        case TYPES_UNTYPED_RUNE:
            return types_type(burrow__types_universe_rune); /* use 'rune' name */
        case TYPES_UNTYPED_FLOAT:
            return GT_TYP(TYPES_FLOAT64);
        case TYPES_UNTYPED_COMPLEX:
            return GT_TYP(TYPES_COMPLEX128);
        case TYPES_UNTYPED_STRING:
            return GT_TYP(TYPES_STRING);
        default:
            break;
        }
    }
    return t;
}

TypesType burrow__types_max_type(TypesType x, TypesType y) {
    /* We only care about untyped types (for now), so == is good enough. */
    if (x == y)
        return x;
    if (burrow__types_is_untyped_numeric(x) && burrow__types_is_untyped_numeric(y)) {
        /* untyped types are basic types */
        if (types_as_basic(x)->kind > types_as_basic(y)->kind)
            return x;
        return y;
    }
    return NULL;
}
