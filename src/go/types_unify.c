/* go/types: type unification.
 *
 * Type unification attempts to make two types x and y structurally
 * equivalent by determining the types for a given list of (bound) type
 * parameters which may occur within x and y. If x and y are structurally
 * different (say []T vs chan T), or conflicting types are determined for type
 * parameters, unification fails. If unification succeeds, as a side-effect,
 * the types of the bound type parameters may be determined.
 *
 * Unification typically requires multiple calls unify(u, x, y) to a given
 * unifier u, with various combinations of types x and y. In each call,
 * additional type parameter types may be determined as a side effect and
 * recorded in u. If a call fails (returns false), unification fails.
 *
 * In the unification context, structural equivalence of two types ignores the
 * difference between a defined type and its underlying type if one type is a
 * defined type and the other one is not. It also ignores the difference
 * between an (external, unbound) type parameter and its core type. If two
 * types are not structurally equivalent, they cannot be Go identical types.
 * On the other hand, if they are structurally equivalent, they may be Go
 * identical or at least assignable, or they may be in the type set of a
 * constraint. Whether they indeed are identical or assignable is determined
 * upon instantiation and function argument passing.
 *
 * Derived from Go's src/go/types/unify.go. Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* Upper limit for recursion depth. Used to catch infinite recursions due to
 * implementation issues (e.g., see issues go.dev/issue/48619,
 * go.dev/issue/48656). Go panics when the limit is reached
 * (panicAtUnificationDepthLimit), and unifies with the core types of unbound
 * type parameters (enableCoreTypeUnification). Not in Go: traceInference is
 * false in Go, so there is no trace here. */
#define TUF_DEPTH_LIMIT 50

/* A unifier maintains a list of type parameters and corresponding types
 * inferred for each type parameter. A unifier is created by calling
 * burrow__tc_new_unifier. */
struct GtUnifier {
    TypesChecker *check;
    /* handles maps each type parameter to its inferred type through an
     * indirection *Type called (inferred type) "handle". Initially, each
     * type parameter has its own, separate handle, with a nil (i.e., not yet
     * inferred) type. After a type parameter P is unified with a type
     * parameter Q, P and Q share the same handle (and thus type). This
     * ensures that inferring the type for a given type parameter P will
     * automatically infer the same type for all other parameters unified
     * (joined) with P.
     *
     * Not in Go: the map is two lists, tparams of TypesTypeParam * and
     * handles of TypesType *, searched in order. There are few type
     * parameters, and the order makes the walks over the map repeatable. */
    Slice tparams;
    Slice handles;
    Int depth;                       /* recursion depth during unification */
    bool enable_interface_inference; /* use shared methods for better inference */
};

/* An ifacePair is a node in a stack of interface type pairs compared for
 * identity. Not in Go: a copy of the one in types_predicates.c. */
typedef struct TufIfacePair {
    TypesInterface *x, *y;
    struct TufIfacePair *prev;
} TufIfacePair;

static bool tuf_iface_pair_identical(const TufIfacePair *p, const TufIfacePair *q) {
    return (p->x == q->x && p->y == q->y) || (p->x == q->y && p->y == q->x);
}

/* newUnifier returns a new unifier initialized with the given type parameter
 * and corresponding type argument lists. The type argument list may be
 * shorter than the type parameter list, and it may contain nil types.
 * Matching type parameters and arguments must have the same index. */
GtUnifier *burrow__tc_new_unifier(TypesChecker *check, Slice tparams, Slice targs,
                                  bool enable_interface_inference) {
    burrow__types_assert(tparams.len >= targs.len);
    /* Allocate all handles up-front: in a correct program, all type
     * parameters must be resolved and thus eventually will get a handle.
     * Also, sharing of handles caused by unified type parameters is rare and
     * so it's ok to not optimize for that case (and delay handle
     * allocation). */
    GtUnifier *u = GT_NEW(check->a, GtUnifier);
    u->check = check;
    u->tparams = tparams;
    u->handles = burrow__types_ptrs_make(check->a, tparams.len);
    for (Int i = 0; i < tparams.len; i++) {
        TypesType *t = GT_NEW(check->a, TypesType);
        if (i < targs.len)
            *t = GT_AT(targs, i, TypesType);
        GT_AT(u->handles, i, TypesType *) = t;
    }
    u->enable_interface_inference = enable_interface_inference;
    return u;
}

/* The handle of x, or NULL if x is not recorded with u. */
static TypesType *tuf_handle(const GtUnifier *u, const TypesTypeParam *x) {
    for (Int i = 0; i < u->tparams.len; i++)
        if (GT_AT(u->tparams, i, TypesTypeParam *) == x)
            return GT_AT(u->handles, i, TypesType *);
    return NULL;
}

/* setHandle sets the handle for type parameter x (and all its joined type
 * parameters) to h. */
static void tuf_set_handle(GtUnifier *u, const TypesTypeParam *x, TypesType *h) {
    TypesType *hx = tuf_handle(u, x);
    burrow__types_assert(hx != NULL);
    for (Int i = 0; i < u->handles.len; i++)
        if (GT_AT(u->handles, i, TypesType *) == hx)
            GT_AT(u->handles, i, TypesType *) = h;
}

/* join unifies the given type parameters x and y. If both type parameters
 * already have a type associated with them, join fails and returns false. */
static bool tuf_join(GtUnifier *u, const TypesTypeParam *x, const TypesTypeParam *y) {
    TypesType *hx = tuf_handle(u, x);
    TypesType *hy = tuf_handle(u, y);
    if (hx == hy)
        return true; /* Both type parameters already share the same handle. */
    if (*hx != NULL && *hy != NULL)
        return false; /* Both type parameters have (possibly different) inferred
                         types. Cannot join. */
    /* Exactly one or neither of x and y has an inferred type. Use that
     * handle, or y's if neither has one. */
    if (*hx != NULL)
        tuf_set_handle(u, y, hx);
    else
        tuf_set_handle(u, x, hy);
    return true;
}

/* asBoundTypeParam returns x.(*TypeParam) if x is a type parameter recorded
 * with u. Otherwise, the result is NULL. */
static TypesTypeParam *tuf_as_bound_type_param(const GtUnifier *u, TypesType x) {
    TypesTypeParam *tp = types_as_type_param(types_unalias(x));
    if (tp != NULL && tuf_handle(u, tp) != NULL)
        return tp;
    return NULL;
}

/* at returns the (possibly NULL) type for type parameter x. */
TypesType burrow__tc_unifier_at(const GtUnifier *u, const TypesTypeParam *x) {
    return *tuf_handle(u, x);
}

/* set sets the type t for type parameter x; t must not be NULL. */
void burrow__tc_unifier_set(GtUnifier *u, const TypesTypeParam *x, TypesType t) {
    burrow__types_assert(t != NULL);
    *tuf_handle(u, x) = t;
}

/* unknowns returns the number of type parameters for which no type has been
 * set yet. */
Int burrow__tc_unifier_unknowns(const GtUnifier *u) {
    Int n = 0;
    for (Int i = 0; i < u->handles.len; i++)
        if (*GT_AT(u->handles, i, TypesType *) == NULL)
            n++;
    return n;
}

/* inferred returns the list of inferred types for the given type parameter
 * list. The result is never NULL and has the same length as tparams; at index
 * i it contains the inferred type for tparams[i]. If a type was not inferred
 * for a type parameter, the corresponding type is NULL. */
Slice burrow__tc_unifier_inferred(const GtUnifier *u, Slice tparams) {
    Slice list = burrow__types_ptrs_make(u->check->a, tparams.len);
    for (Int i = 0; i < tparams.len; i++)
        GT_AT(list, i, TypesType) =
            burrow__tc_unifier_at(u, GT_AT(tparams, i, TypesTypeParam *));
    return list;
}

/* asInterface returns the underlying type of x as an interface if it is a
 * non-type parameter interface. Otherwise it returns NULL. */
static TypesInterface *tuf_as_interface(TypesType x) {
    if (x == NULL || burrow__types_is_type_param(x))
        return NULL;
    return types_as_interface(types_type_underlying(x));
}

/* The method in list with the same Id as m, or NULL. Not in Go: Go looks it
 * up in a map of the list. */
static TypesFunc *tuf_find_method(Slice list, TypesFunc *m) {
    for (Int i = 0; i < list.len; i++) {
        TypesFunc *f = GT_AT(list, i, TypesFunc *);
        if (burrow__types_id_eq(GT_OBJ(f)->pkg, GT_OBJ(f)->name, GT_OBJ(m)->pkg,
                                GT_OBJ(m)->name))
            return f;
    }
    return NULL;
}

static bool tuf_nify(GtUnifier *u, TypesType x, TypesType y, unsigned mode,
                     TufIfacePair *p);

/* x is a bound type parameter px and y is not: nify for the case where px
 * has an inferred type, which must match y. */
static bool tuf_nify_inferred(GtUnifier *u, TypesTypeParam *px, TypesType x,
                              TypesType y, unsigned mode, TufIfacePair *p) {
    if (!tuf_nify(u, x, y, mode, p))
        return false;
    /* We have a match, possibly through underlying types. */
    TypesInterface *xi = tuf_as_interface(x);
    TypesInterface *yi = tuf_as_interface(y);
    bool xn = burrow__types_as_named(x) != NULL;
    bool yn = burrow__types_as_named(y) != NULL;
    /* If we have two interfaces, what to do depends on whether they are named
     * and their method sets. */
    if (xi != NULL && yi != NULL) {
        /* Both types are interfaces. If both types are defined types, they
         * must be identical because unification doesn't know which type has
         * the "right" name. */
        if (xn && yn)
            return types_identical(x, y);
        /* In all other cases, the method sets must match. The types unified
         * so we know that corresponding methods match and we can simply
         * compare the number of methods. */
        if (burrow__types_iface_type_set(xi)->methods.len !=
            burrow__types_iface_type_set(yi)->methods.len)
            return false;
    } else if (xi != NULL || yi != NULL) {
        /* One but not both of them are interfaces. In this case, either x or
         * y could be viable matches for the corresponding type parameter,
         * which means choosing either introduces an order dependence.
         * Therefore, we must fail unification (go.dev/issue/60933). */
        return false;
    }
    /* If we have inexact unification and one of x or y is a defined type,
     * select the defined type. This ensures that in a series of types, all
     * matching against the same type parameter, we infer a defined type if
     * there is one, independent of order. Type inference or assignment may
     * fail, which is ok. Selecting a defined type, if any, ensures that we
     * don't lose the type name; and since we have inexact unification, a
     * value of equally named or matching undefined type remains assignable
     * (go.dev/issue/43056).
     *
     * Similarly, if we have inexact unification and there are no defined
     * types but channel types, select a directed channel, if any. This
     * ensures that in a series of unnamed types, all matching against the
     * same type parameter, we infer the directed channel if there is one,
     * independent of order. Selecting a directional channel, if any, ensures
     * that a value of another inexactly unifying channel type remains
     * assignable (go.dev/issue/62157).
     *
     * If we have multiple defined channel types, they are either identical
     * or we have assignment conflicts, so we can ignore directionality in
     * this case.
     *
     * If we have defined and literal channel types, a defined type wins to
     * avoid order dependencies. */
    if ((mode & GT_UNIFY_EXACT) == 0 && !xn) {
        if (yn) {
            /* x is not a defined type and y is a defined type: select y. */
            burrow__tc_unifier_set(u, px, y);
        } else {
            /* Neither x nor y are defined types. */
            TypesChan *yc = types_as_chan(types_type_underlying(y));
            if (yc != NULL && yc->dir != TYPES_SEND_RECV) {
                /* y is a directed channel type: select y. */
                burrow__tc_unifier_set(u, px, y);
            }
        }
    }
    return true;
}

/* Interface inference, when neither x nor y is a bound type parameter and
 * the unification is not exact. Returns false with *done unset if neither
 * type is an interface. */
static bool tuf_nify_iface(GtUnifier *u, TypesType x, TypesType y, TufIfacePair *p,
                           bool *done) {
    /* One or both interfaces may be defined types. Look under the name, but
     * not under type parameters (go.dev/issue/60564). */
    TypesInterface *xi = tuf_as_interface(x);
    TypesInterface *yi = tuf_as_interface(y);
    *done = true;

    /* If we have two interfaces, check the type terms for equivalence, and
     * unify common methods if possible. */
    if (xi != NULL && yi != NULL) {
        GtTypeSet *xset = burrow__types_iface_type_set(xi);
        GtTypeSet *yset = burrow__types_iface_type_set(yi);
        if (xset->comparable != yset->comparable)
            return false;
        /* For now we require terms to be equal. We should be able to relax
         * this as well, eventually. */
        if (!burrow__types_termlist_equal(xset->terms, yset->terms))
            return false;
        /* Interface types are the only types where cycles can occur that are
         * not "terminated" via named types; and such cycles can only be
         * created via method parameter types that are anonymous interfaces
         * (directly or indirectly) embedding the current interface. If two
         * such (differently named) interfaces are compared, endless
         * recursion occurs if the cycle is not detected. If x and y were
         * compared before, they must be equal (if they were not, the
         * recursion would have stopped); search the ifacePair stack for the
         * same pair. */
        TufIfacePair q = {xi, yi, p};
        for (; p != NULL; p = p->prev)
            if (tuf_iface_pair_identical(p, &q))
                return true; /* same pair was compared before */
        /* The method set of x must be a subset of the method set of y or
         * vice versa, and the common methods must unify. The smaller method
         * set must be the subset, if it exists. As in Go, the methods are
         * unified with p, which is NULL after the search above. */
        Slice xmethods = xset->methods;
        Slice ymethods = yset->methods;
        if (xmethods.len > ymethods.len) {
            Slice t = xmethods;
            xmethods = ymethods;
            ymethods = t;
        }
        /* All xmethods must exist in ymethods and corresponding signatures
         * must unify. */
        for (Int i = 0; i < xmethods.len; i++) {
            TypesFunc *xm = GT_AT(xmethods, i, TypesFunc *);
            TypesFunc *ym = tuf_find_method(ymethods, xm);
            if (ym == NULL ||
                !tuf_nify(u, GT_OBJ(xm)->typ, GT_OBJ(ym)->typ, GT_UNIFY_EXACT, p))
                return false;
        }
        return true;
    }

    /* We don't have two interfaces. If we have one, make sure it's in xi. */
    if (yi != NULL) {
        xi = yi;
        y = x;
    }

    /* If we have one interface, at a minimum each of the interface methods
     * must be implemented and thus unify with a corresponding method from
     * the non-interface type, otherwise unification fails. */
    if (xi != NULL) {
        /* All xi methods must exist in y and corresponding signatures must
         * unify. A generic method never satisfies an interface method, so
         * fail rather than unify ym's own type parameter into an inference
         * variable. */
        Slice xmethods = burrow__types_iface_type_set(xi)->methods;
        for (Int i = 0; i < xmethods.len; i++) {
            TypesFunc *xm = GT_AT(xmethods, i, TypesFunc *);
            Slice index;
            bool indirect;
            TypesObject obj = burrow__types_lookup_field_or_method(
                u->check->a, y, false, GT_OBJ(xm)->pkg, GT_OBJ(xm)->name, false, &index,
                &indirect);
            TypesFunc *ym = types_as_func(obj);
            if (ym == NULL)
                return false;
            /* ensure fully set-up signature */
            burrow__tc_obj_decl(u->check, types_object(ym));
            TypesSignature *ysig = types_as_signature(GT_OBJ(ym)->typ);
            if ((ysig != NULL && ysig->tparams != NULL) ||
                !tuf_nify(u, GT_OBJ(xm)->typ, GT_OBJ(ym)->typ, GT_UNIFY_EXACT, p))
                return false;
        }
        return true;
    }
    *done = false;
    return false;
}

/* Two interface types unify if they have the same set of methods with the
 * same names, and corresponding function types unify. Lower-case method
 * names from different packages are always different. The order of the
 * methods is irrelevant. */
static bool tuf_nify_interface(GtUnifier *u, TypesInterface *x, TypesInterface *y,
                               TufIfacePair *p) {
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
    /* Search the ifacePair stack for the same pair, as above. */
    TufIfacePair q = {x, y, p};
    for (; p != NULL; p = p->prev)
        if (tuf_iface_pair_identical(p, &q))
            return true; /* same pair was compared before */
    for (Int i = 0; i < a.len; i++) {
        GtObject *f = GT_OBJ(GT_AT(a, i, TypesFunc *));
        GtObject *g = GT_OBJ(GT_AT(b, i, TypesFunc *));
        if (!burrow__types_id_eq(f->pkg, f->name, g->pkg, g->name) ||
            !tuf_nify(u, f->typ, g->typ, GT_UNIFY_EXACT, &q))
            return false;
    }
    return true;
}

/* The structural part of nify: x and y are not bound type parameters, or are
 * interfaces under exact unification. */
static bool tuf_nify_struct(GtUnifier *u, TypesType x, TypesType y, unsigned mode,
                            TufIfacePair *p) {
    /* Unless we have exact unification, neither x nor y are interfaces now.
     * Except for unbound type parameters (see below), x and y must be
     * structurally equivalent to unify.
     *
     * If we get here and x or y is a type parameter, they are unbound (not
     * recorded with the unifier). Ensure that if we have at least one type
     * parameter, it is in x (the earlier swap checks for _recorded_ type
     * parameters only). This ensures that the switch switches on the type
     * parameter. */
    if (burrow__types_is_type_param(y)) {
        TypesType t = x;
        x = y;
        y = t;
    }

    /* Type elements (array, slice, etc. elements) use emode for
     * unification. Element types must match exactly if the types are used in
     * an assignment. */
    unsigned emode = mode;
    if ((mode & GT_UNIFY_ASSIGN) != 0)
        emode |= GT_UNIFY_EXACT;

    /* Continue with unaliased types but don't lose original alias names, if
     * any (go.dev/issue/67628). */
    TypesType yorig = y;
    x = types_unalias(x);
    y = types_unalias(y);

    if (x == NULL)
        return false; /* avoid a crash in case of nil type */
    if (y == NULL)
        return false; /* Not in Go: an empty tuple, which unifies with none */

    Int kind = x->kind;
    switch (kind) {
    case TYPES_KIND_BASIC:
        /* Basic types are singletons except for the rune and byte aliases,
         * thus we cannot solely rely on the x == y check above. See also
         * comment in TypeName.IsAlias. */
        if (y->kind == TYPES_KIND_BASIC)
            return types_as_basic(x)->kind == types_as_basic(y)->kind;
        return false;

    case TYPES_KIND_ARRAY:
        /* Two array types unify if they have the same array length and their
         * element types unify. */
        if (y->kind == TYPES_KIND_ARRAY) {
            TypesArray *xa = types_as_array(x), *ya = types_as_array(y);
            /* If one or both array lengths are unknown (< 0) due to some
             * error, assume they are the same to avoid spurious follow-on
             * errors. */
            return (xa->len < 0 || ya->len < 0 || xa->len == ya->len) &&
                   tuf_nify(u, xa->elem, ya->elem, emode, p);
        }
        return false;

    case TYPES_KIND_SLICE:
        /* Two slice types unify if their element types unify. */
        if (y->kind == TYPES_KIND_SLICE)
            return tuf_nify(u, types_as_slice(x)->elem, types_as_slice(y)->elem, emode,
                            p);
        return false;

    case TYPES_KIND_STRUCT:
        /* Two struct types unify if they have the same sequence of fields,
         * and if corresponding fields have the same names, their (field)
         * types unify, and they have identical tags. Two embedded fields are
         * considered to have the same name. Lower-case field names from
         * different packages are always different. */
        if (y->kind == TYPES_KIND_STRUCT) {
            TypesStruct *xs = types_as_struct(x), *ys = types_as_struct(y);
            if (xs->fields.len != ys->fields.len)
                return false;
            for (Int i = 0; i < xs->fields.len; i++) {
                TypesVar *f = GT_AT(xs->fields, i, TypesVar *);
                TypesVar *g = GT_AT(ys->fields, i, TypesVar *);
                if (f->embedded != g->embedded ||
                    !str_eq(types_struct_tag(xs, i), types_struct_tag(ys, i)) ||
                    !burrow__types_same_id(types_object(f), GT_OBJ(g)->pkg,
                                           GT_OBJ(g)->name, false) ||
                    !tuf_nify(u, GT_OBJ(f)->typ, GT_OBJ(g)->typ, emode, p))
                    return false;
            }
            return true;
        }
        return false;

    case TYPES_KIND_POINTER:
        /* Two pointer types unify if their base types unify. */
        if (y->kind == TYPES_KIND_POINTER)
            return tuf_nify(u, types_as_pointer(x)->base, types_as_pointer(y)->base,
                            emode, p);
        return false;

    case TYPES_KIND_TUPLE:
        /* Two tuples types unify if they have the same number of elements and
         * the types of corresponding elements unify. */
        if (y->kind == TYPES_KIND_TUPLE) {
            TypesTuple *xt = types_as_tuple(x), *yt = types_as_tuple(y);
            if (xt->vars.len != yt->vars.len)
                return false;
            for (Int i = 0; i < xt->vars.len; i++) {
                TypesVar *v = GT_AT(xt->vars, i, TypesVar *);
                TypesVar *w = GT_AT(yt->vars, i, TypesVar *);
                if (!tuf_nify(u, GT_OBJ(v)->typ, GT_OBJ(w)->typ, mode, p))
                    return false;
            }
            return true;
        }
        return false;

    case TYPES_KIND_SIGNATURE:
        /* Two function types unify if they have the same number of parameters
         * and result values, corresponding parameter and result types unify,
         * and either both functions are variadic or neither is. Parameter
         * and result names are not required to match. */
        if (y->kind == TYPES_KIND_SIGNATURE) {
            TypesSignature *xs = types_as_signature(x), *ys = types_as_signature(y);
            return xs->variadic == ys->variadic &&
                   tuf_nify(u, types_type(xs->params), types_type(ys->params), emode,
                            p) &&
                   tuf_nify(u, types_type(xs->results), types_type(ys->results), emode,
                            p);
        }
        return false;

    case TYPES_KIND_INTERFACE:
        /* handled before this switch */
        burrow__types_assert(!u->enable_interface_inference ||
                             (mode & GT_UNIFY_EXACT) != 0);
        if (y->kind == TYPES_KIND_INTERFACE)
            return tuf_nify_interface(u, types_as_interface(x), types_as_interface(y),
                                      p);
        return false;

    case TYPES_KIND_MAP:
        /* Two map types unify if their key and value types unify. */
        if (y->kind == TYPES_KIND_MAP) {
            TypesMap *xm = types_as_map(x), *ym = types_as_map(y);
            return tuf_nify(u, xm->key, ym->key, emode, p) &&
                   tuf_nify(u, xm->elem, ym->elem, emode, p);
        }
        return false;

    case TYPES_KIND_CHAN:
        /* Two channel types unify if their value types unify and if they have
         * the same direction. The channel direction is ignored for inexact
         * unification. */
        if (y->kind == TYPES_KIND_CHAN) {
            TypesChan *xc = types_as_chan(x), *yc = types_as_chan(y);
            return ((mode & GT_UNIFY_EXACT) == 0 || xc->dir == yc->dir) &&
                   tuf_nify(u, xc->elem, yc->elem, emode, p);
        }
        return false;

    case TYPES_KIND_NAMED: {
        /* Two named types unify if their type names originate in the same
         * type declaration. If they are instantiated, their type argument
         * lists must unify. */
        TypesNamed *yn = burrow__types_as_named(y);
        if (yn == NULL)
            return false;
        TypesNamed *xn = types_as_named(x);
        /* Check type arguments before origins so they unify even if the
         * origins don't match; for better error messages (see
         * go.dev/issue/53692). */
        Slice xargs = burrow__types_type_list(types_named_type_args(xn));
        Slice yargs = burrow__types_type_list(types_named_type_args(yn));
        if (xargs.len != yargs.len)
            return false;
        for (Int i = 0; i < xargs.len; i++)
            if (!tuf_nify(u, GT_AT(xargs, i, TypesType), GT_AT(yargs, i, TypesType),
                          mode, p))
                return false;
        return burrow__types_identical_origin(xn, yn);
    }

    case TYPES_KIND_TYPE_PARAM: {
        /* x must be an unbound type parameter (see comment above). By
         * definition, a valid type argument must be in the type set of the
         * respective type constraint. Therefore, the type argument's
         * underlying type must be in the set of underlying types of that
         * constraint. If there is a single such underlying type, it's the
         * constraint's core type. It must match the type argument's
         * underlying type, irrespective of whether the actual type argument,
         * which may be a defined type, is actually in the type set (that
         * will be determined at instantiation time). Thus, if we have the
         * core type of an unbound type parameter, we know the structure of
         * the possible types satisfying such parameters. Use that core type
         * for further unification (see go.dev/issue/50755 for a test case).
         *
         * Because the core type is always an underlying type, unification
         * will take care of matching against a defined or literal type
         * automatically. If y is also an unbound type parameter, we will end
         * up here again with x and y swapped, so we don't need to take care
         * of that case separately. */
        TypesType cx = burrow__tc_common_under(u->check, x, (GtUnderCond){0}, NULL);
        if (cx != NULL) {
            /* If y is a defined type, it may not match against cx which is
             * an underlying type (incl. int, string, etc.). Use assign mode
             * here so that the unifier automatically uses y.Underlying() if
             * necessary. */
            return tuf_nify(u, cx, yorig, GT_UNIFY_ASSIGN, p);
        }
        return false;
    }

    default: {
        Str xs = burrow__tc_ts(u->check, x), ys = burrow__tc_ts(u->check, yorig);
        Int m = (Int)mode;
        panic_str(GT_SPRINTF(u->check, "u.nify(%s, %s, %d)", xs, ys, m));
    }
    }
}

/* nify implements the core unification algorithm which is an adapted version
 * of Checker.identical. For changes to that code the corresponding changes
 * should be made here. Must not be called directly from outside the
 * unifier. */
static bool tuf_nify(GtUnifier *u, TypesType x, TypesType y, unsigned mode,
                     TufIfacePair *p) {
    /* nothing to do if x == y */
    if (x == y || types_unalias(x) == types_unalias(y))
        return true;

    /* Stop gap for cases where unification fails. */
    if (u->depth + 1 > TUF_DEPTH_LIMIT)
        panic_str(BURROW_S("unification reached recursion depth limit"));

    u->depth++;
    bool ok = true;

    /* Unification is symmetric, so we can swap the operands. Ensure that if
     * we have at least one
     * - defined type, make sure one is in y
     * - type parameter recorded with u, make sure one is in x */
    if (burrow__types_as_named(x) != NULL || tuf_as_bound_type_param(u, y) != NULL) {
        TypesType t = x;
        x = y;
        y = t;
    }

    /* Unification will fail if we match a defined type against a type
     * literal. If we are matching types in an assignment, at the top-level,
     * types with the same type structure are permitted as long as at least
     * one of them is not a defined type. To accommodate for that
     * possibility, we continue unification with the underlying type of a
     * defined type if the other type is a type literal. This is controlled
     * by the exact unification mode. We also continue if the other type is
     * a basic type because basic types are valid underlying types and may
     * appear as core types of type constraints. If we exclude them, inferred
     * defined types for type parameters may not match against the core
     * types of their constraints (even though they might correctly match
     * against some of the types in the constraint's type set). Finally, if
     * unification (incorrectly) succeeds by matching the underlying type of
     * a defined type against a basic type (because we include basic types as
     * type literals here), and if that leads to an incorrectly inferred
     * type, we will fail at function instantiation or argument assignment
     * time.
     *
     * If we have at least one defined type, there is one in y. */
    TypesNamed *ny = burrow__types_as_named(y);
    if ((mode & GT_UNIFY_EXACT) == 0 && ny != NULL && burrow__types_is_type_lit(x) &&
        !(u->enable_interface_inference && types_is_interface(x))) {
        y = types_type_underlying(types_type(ny));
        /* Per the spec, a defined type cannot have an underlying type that
         * is a type parameter. */
        burrow__types_assert(!burrow__types_is_type_param(y));
        /* x and y may be identical now */
        if (x == y || types_unalias(x) == types_unalias(y))
            goto done;
    }

    /* Cases where at least one of x or y is a type parameter recorded with
     * u. If we have at least one type parameter, there is one in x. If we
     * have exactly one type parameter, because it is in x, isTypeLit(x) is
     * false and y was not changed above. In other words, if y was a defined
     * type, it is still a defined type (relevant for the logic below). */
    TypesTypeParam *px = tuf_as_bound_type_param(u, x);
    TypesTypeParam *py = tuf_as_bound_type_param(u, y);
    if (px != NULL && py != NULL) {
        /* both x and y are type parameters */
        if (!tuf_join(u, px, py)) {
            /* both x and y have an inferred type - they must match */
            ok = tuf_nify(u, burrow__tc_unifier_at(u, px), burrow__tc_unifier_at(u, py),
                          mode, p);
        }
        goto done;
    }
    if (px != NULL) {
        /* x is a type parameter, y is not */
        TypesType xt = burrow__tc_unifier_at(u, px);
        if (xt != NULL) {
            /* x has an inferred type which must match y */
            ok = tuf_nify_inferred(u, px, xt, y, mode, p);
        } else {
            /* otherwise, infer type from y */
            burrow__tc_unifier_set(u, px, y);
        }
        goto done;
    }

    /* x != y if we get here */
    burrow__types_assert(x != y && types_unalias(x) != types_unalias(y));

    /* If u.EnableInterfaceInference is set and we don't require exact
     * unification, if both types are interfaces, one interface must have a
     * subset of the methods of the other and corresponding method signatures
     * must unify. If only one type is an interface, all its methods must be
     * present in the other type and corresponding method signatures must
     * unify. */
    if (u->enable_interface_inference && (mode & GT_UNIFY_EXACT) == 0) {
        bool handled = false;
        ok = tuf_nify_iface(u, x, y, p, &handled);
        if (handled)
            goto done;
    }

    ok = tuf_nify_struct(u, x, y, mode, p);

done:
    u->depth--;
    return ok;
}

/* unify attempts to unify x and y and reports whether it succeeded. As a
 * side-effect, types may be inferred for type parameters. The mode parameter
 * controls how types are compared. */
bool burrow__tc_unify(GtUnifier *u, TypesType x, TypesType y, unsigned mode) {
    return tuf_nify(u, x, y, mode, NULL);
}
