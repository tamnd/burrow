/* go/types: type parameter inference.
 *
 * Derived from Go's src/go/types/infer.go. Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* The index of tpar in tparams, or -1. */
static Int tin_index(Slice tparams, const TypesTypeParam *tpar) {
    for (Int i = 0; i < tparams.len; i++)
        if (GT_AT(tparams, i, TypesTypeParam *) == tpar)
            return i;
    return -1;
}

static bool tin_contains_nil(Slice list) {
    for (Int i = 0; i < list.len; i++)
        if (GT_AT(list, i, TypesType) == NULL)
            return true;
    return false;
}

static bool tin_all_nil(Slice list) {
    for (Int i = 0; i < list.len; i++)
        if (GT_AT(list, i, TypesType) != NULL)
            return false;
    return true;
}

static Map *tin_seen_map(Alloc *a) {
    Map *m = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

static void tin_seen_set(Map *m, TypesType t, bool v) {
    if (!map_set(m, &t, &v))
        burrow__types_oom();
}

/* typeParamsString produces a string containing all the type parameter
 * names in list suitable for human consumption. */
static Str tin_type_params_string(Alloc *a, Slice list) {
    /* common cases */
    Int n = list.len;
    switch (n) {
    case 0:
        return BURROW_STR_EMPTY;
    case 1:
        return GT_OBJ(GT_AT(list, 0, TypesTypeParam *)->obj)->name;
    default:
        break;
    }
    BytesBuffer buf = BYTES_BUFFER(a);
    if (n == 2) {
        burrow__types_buf_str(&buf,
                              GT_OBJ(GT_AT(list, 0, TypesTypeParam *)->obj)->name);
        burrow__types_buf_str(&buf, BURROW_S(" and "));
        burrow__types_buf_str(&buf,
                              GT_OBJ(GT_AT(list, 1, TypesTypeParam *)->obj)->name);
        return burrow__types_buf_done(&buf, a);
    }

    /* general case (n > 2) */
    for (Int i = 0; i < n - 1; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        burrow__types_buf_str(&buf,
                              GT_OBJ(GT_AT(list, i, TypesTypeParam *)->obj)->name);
    }
    burrow__types_buf_str(&buf, BURROW_S(", and "));
    burrow__types_buf_str(&buf,
                          GT_OBJ(GT_AT(list, n - 1, TypesTypeParam *)->obj)->name);
    return burrow__types_buf_done(&buf, a);
}

/* ----------------------------------------------------------- isParameterized */

typedef struct TinWalker {
    Slice tparams; /* of TypesTypeParam * */
    Map *seen;     /* TypesType to bool */
} TinWalker;

static bool tin_walk(TinWalker *w, TypesType typ);

static bool tin_var_list(TinWalker *w, Slice list) {
    for (Int i = 0; i < list.len; i++)
        if (tin_walk(w, GT_OBJ(GT_AT(list, i, TypesVar *))->typ))
            return true;
    return false;
}

static bool tin_term_parameterized(void *env, TypesTerm *t) {
    return t != NULL && tin_walk(env, t->typ);
}

static bool tin_walk_type(TinWalker *w, TypesType typ) {
    Int kind = typ->kind;
    switch (kind) {
    case TYPES_KIND_BASIC:
        /* nothing to do */
        return false;

    case TYPES_KIND_ALIAS:
        return tin_walk(w, types_unalias(typ));

    case TYPES_KIND_ARRAY:
        return tin_walk(w, types_as_array(typ)->elem);

    case TYPES_KIND_SLICE:
        return tin_walk(w, types_as_slice(typ)->elem);

    case TYPES_KIND_STRUCT:
        return tin_var_list(w, types_as_struct(typ)->fields);

    case TYPES_KIND_POINTER:
        return tin_walk(w, types_as_pointer(typ)->base);

    case TYPES_KIND_TUPLE:
        /* This case does not occur from within isParameterized because
         * tuples only appear in signatures where they are handled
         * explicitly. But isParameterized is also called by Checker.callExpr
         * with a function result tuple if instantiation failed
         * (go.dev/issue/59890). */
        return tin_var_list(w, types_as_tuple(typ)->vars);

    case TYPES_KIND_SIGNATURE: {
        /* t.tparams may not be nil if we are looking at a signature of a
         * generic function type (or an interface method) that is part of the
         * type we're testing. We don't care about these type parameters.
         * Similarly, the receiver of a method may declare (rather than use)
         * type parameters, we don't care about those either. Thus, we only
         * need to look at the input and result parameters. */
        TypesSignature *t = types_as_signature(typ);
        return (t->params != NULL && tin_var_list(w, t->params->vars)) ||
               (t->results != NULL && tin_var_list(w, t->results->vars));
    }

    case TYPES_KIND_INTERFACE: {
        GtTypeSet *tset = burrow__types_iface_type_set(types_as_interface(typ));
        for (Int i = 0; i < tset->methods.len; i++)
            if (tin_walk(w, GT_OBJ(GT_AT(tset->methods, i, TypesFunc *))->typ))
                return true;
        return burrow__types_type_set_is(
            tset, BURROW_FN(GtTermFunc, tin_term_parameterized, w));
    }

    case TYPES_KIND_MAP:
        return tin_walk(w, types_as_map(typ)->key) ||
               tin_walk(w, types_as_map(typ)->elem);

    case TYPES_KIND_CHAN:
        return tin_walk(w, types_as_chan(typ)->elem);

    case TYPES_KIND_NAMED: {
        Slice targs =
            burrow__types_type_list(types_named_type_args(types_as_named(typ)));
        for (Int i = 0; i < targs.len; i++)
            if (tin_walk(w, GT_AT(targs, i, TypesType)))
                return true;
        return false;
    }

    case TYPES_KIND_TYPE_PARAM:
        return tin_index(w->tparams, types_as_type_param(typ)) >= 0;

    default:
        panic_str(BURROW_S("unexpected type"));
    }
}

static bool tin_walk(TinWalker *w, TypesType typ) {
    /* Not in Go: a nil *Tuple, which has no parameters. */
    if (typ == NULL)
        return false;
    /* detect cycles */
    const bool *x = map_get(w->seen, &typ);
    if (x != NULL)
        return *x;
    tin_seen_set(w->seen, typ, false);
    bool res = tin_walk_type(w, typ);
    tin_seen_set(w->seen, typ, res);
    return res;
}

/* isParameterized reports whether typ contains any of the type parameters of
 * tparams. If typ is a generic function, isParameterized ignores the type
 * parameter declarations; it only considers the signature proper (incoming
 * and result parameters). */
bool burrow__tc_is_parameterized(Alloc *a, Slice tparams, TypesType typ) {
    TinWalker w = {tparams, tin_seen_map(a)};
    return tin_walk(&w, typ);
}

/* ------------------------------------------------------------------ coreTerm */

typedef struct TinCore {
    Int n;
    TypesTerm *single; /* valid if n == 1 */
    bool tilde;
} TinCore;

static bool tin_core_term_fn(void *env, TypesTerm *t) {
    TinCore *c = env;
    if (t == NULL) {
        burrow__types_assert(c->n == 0);
        return false; /* no terms */
    }
    c->n++;
    c->single = t;
    if (t->tilde)
        c->tilde = true;
    return true;
}

/* If the type parameter has a single specific type S, coreTerm returns S with
 * *single set. Otherwise, if tpar has a core type T, it returns a term
 * corresponding to that core type with *single unset. In that case, if any
 * term of tpar has a tilde, the core term has a tilde. In all other cases
 * coreTerm returns NULL with *single unset. */
static TypesTerm *tin_core_term(TypesChecker *check, TypesTypeParam *tpar,
                                bool *single) {
    TinCore c = {0, NULL, false};
    (void)burrow__types_type_param_is(tpar,
                                      BURROW_FN(GtTermFunc, tin_core_term_fn, &c));
    *single = c.n == 1;
    if (c.n == 1)
        return c.single;
    TypesType typ =
        burrow__tc_common_under(check, types_type(tpar), (GtUnderCond){0}, NULL);
    if (typ != NULL) {
        /* A core type is always an underlying type. If any term of tpar has a
         * tilde, we don't have a precise core type and we must return a tilde
         * as well. */
        return types_new_term(check->a, c.tilde, typ);
    }
    return NULL;
}

/* ---------------------------------------------------------------- killCycles */

typedef struct TinCycleFinder {
    Slice tparams;  /* of TypesTypeParam * */
    Slice inferred; /* of TypesType */
    Map *seen;      /* TypesType to bool */
} TinCycleFinder;

static void tin_cycle_typ(TinCycleFinder *w, TypesType typ);

static void tin_cycle_var_list(TinCycleFinder *w, Slice list) {
    for (Int i = 0; i < list.len; i++)
        tin_cycle_typ(w, GT_OBJ(GT_AT(list, i, TypesVar *))->typ);
}

static void tin_cycle_walk(TinCycleFinder *w, TypesType typ) {
    Int kind = typ->kind;
    switch (kind) {
    case TYPES_KIND_BASIC:
        /* nothing to do */
        break;

        /* TYPES_KIND_ALIAS: This case should not occur because of
     * types_unalias(typ) at the top. */

    case TYPES_KIND_ARRAY:
        tin_cycle_typ(w, types_as_array(typ)->elem);
        break;

    case TYPES_KIND_SLICE:
        tin_cycle_typ(w, types_as_slice(typ)->elem);
        break;

    case TYPES_KIND_STRUCT:
        tin_cycle_var_list(w, types_as_struct(typ)->fields);
        break;

    case TYPES_KIND_POINTER:
        tin_cycle_typ(w, types_as_pointer(typ)->base);
        break;

        /* TYPES_KIND_TUPLE: This case should not occur because tuples only
     * appear in signatures where they are handled explicitly. */

    case TYPES_KIND_SIGNATURE: {
        TypesSignature *t = types_as_signature(typ);
        if (t->params != NULL)
            tin_cycle_var_list(w, t->params->vars);
        if (t->results != NULL)
            tin_cycle_var_list(w, t->results->vars);
        break;
    }

    case TYPES_KIND_UNION: {
        Slice terms = types_as_union(typ)->terms;
        for (Int i = 0; i < terms.len; i++)
            tin_cycle_typ(w, GT_AT(terms, i, TypesTerm *)->typ);
        break;
    }

    case TYPES_KIND_INTERFACE: {
        TypesInterface *t = types_as_interface(typ);
        for (Int i = 0; i < t->methods.len; i++)
            tin_cycle_typ(w, GT_OBJ(GT_AT(t->methods, i, TypesFunc *))->typ);
        for (Int i = 0; i < t->embeddeds.len; i++)
            tin_cycle_typ(w, GT_AT(t->embeddeds, i, TypesType));
        break;
    }

    case TYPES_KIND_MAP:
        tin_cycle_typ(w, types_as_map(typ)->key);
        tin_cycle_typ(w, types_as_map(typ)->elem);
        break;

    case TYPES_KIND_CHAN:
        tin_cycle_typ(w, types_as_chan(typ)->elem);
        break;

    case TYPES_KIND_NAMED: {
        Slice targs =
            burrow__types_type_list(types_named_type_args(types_as_named(typ)));
        for (Int i = 0; i < targs.len; i++)
            tin_cycle_typ(w, GT_AT(targs, i, TypesType));
        break;
    }

    case TYPES_KIND_TYPE_PARAM: {
        Int i = tin_index(w->tparams, types_as_type_param(typ));
        if (i >= 0 && GT_AT(w->inferred, i, TypesType) != NULL)
            tin_cycle_typ(w, GT_AT(w->inferred, i, TypesType));
        break;
    }

    default:
        panic_str(BURROW_S("unexpected type"));
    }
}

static void tin_cycle_typ(TinCycleFinder *w, TypesType typ) {
    typ = types_unalias(typ);
    /* Not in Go: a nil *Tuple, which has no parameters. */
    if (typ == NULL)
        return;
    if (map_get(w->seen, &typ) != NULL) {
        /* We have seen typ before. If it is one of the type parameters in
         * w.tparams, iterative substitution will lead to infinite expansion.
         * Nil out the corresponding type which effectively kills the cycle. */
        TypesTypeParam *tpar = types_as_type_param(typ);
        if (tpar != NULL) {
            Int i = tin_index(w->tparams, tpar);
            if (i >= 0)
                GT_AT(w->inferred, i, TypesType) = NULL; /* cycle through tpar */
        }
        /* If we don't have one of our type parameters, the cycle is due to an
         * ordinary recursive type and we can just stop walking it. */
        return;
    }
    tin_seen_set(w->seen, typ, true);
    tin_cycle_walk(w, typ);
    map_del(w->seen, &typ);
}

/* killCycles walks through the given type parameters and looks for cycles
 * created by type parameters whose inferred types refer back to that type
 * parameter, either directly or indirectly. If such a cycle is detected, it
 * is killed by setting the corresponding inferred type to NULL. */
static void tin_kill_cycles(Alloc *a, Slice tparams, Slice inferred) {
    TinCycleFinder w = {tparams, inferred, tin_seen_map(a)};
    for (Int i = 0; i < tparams.len; i++)
        tin_cycle_typ(&w, types_type(GT_AT(tparams, i, TypesTypeParam *)));
}

/* --------------------------------------------------------------------- infer */

typedef struct TinInfer {
    TypesChecker *check;
    GtUnifier *u;
    Slice tparams;
    bool reverse;
    GtError *err;
} TinInfer;

static bool tin_unify_exact(void *env, TypesType x, TypesType y) {
    return burrow__tc_unify(env, x, y, GT_UNIFY_EXACT);
}

/* errorf: provide a better error message if we can. */
static void tin_errorf(const TinInfer *in, TypesType tpar, TypesType targ,
                       const GtOperand *arg) {
    TypesChecker *check = in->check;
    Slice targs = burrow__tc_unifier_inferred(in->u, in->tparams);
    Str tgs = burrow__tc_ts(check, targ), as = burrow__tc_es(check, arg->expr);
    Str tps = burrow__tc_ts(check, tpar);
    if (GT_AT(targs, 0, TypesType) == NULL && tin_all_nil(targs)) {
        /* The first type parameter couldn't be inferred. If none of them could
         * be inferred, don't try to provide the inferred type in the error
         * msg. */
        Str ps = tin_type_params_string(check->a, in->tparams);
        GT_ADDF(in->err, burrow__tc_at_op(arg),
                "type %s of %s does not match %s (cannot infer %s)", tgs, as, tps, ps);
        return;
    }
    Map *smap = burrow__types_make_subst_map(check->a, in->tparams, targs);
    TypesType inferred =
        burrow__types_subst(check, check->a, burrow__tc_op_pos(arg), tpar, smap, NULL,
                            burrow__tc_context(check));
    /* CannotInferTypeArgs indicates a failure of inference, though the actual
     * error may be better attributed to a user-provided type argument (hence
     * InvalidTypeArg). We can't differentiate these cases, so fall back on
     * the more general CannotInferTypeArgs. */
    if (inferred != tpar) {
        Str is = burrow__tc_ts(check, inferred);
        if (in->reverse)
            GT_ADDF(in->err, burrow__tc_at_op(arg),
                    "inferred type %s for %s does not match type %s of %s", is, tps,
                    tgs, as);
        else
            GT_ADDF(in->err, burrow__tc_at_op(arg),
                    "type %s of %s does not match inferred type %s for %s", tgs, as, is,
                    tps);
    } else {
        GT_ADDF(in->err, burrow__tc_at_op(arg), "type %s of %s does not match %s", tgs,
                as, tps);
    }
}

/* Step 1 of infer: use information from function arguments. Unify parameter
 * and argument types for generic parameters with typed arguments and collect
 * the indices of generic parameters with untyped arguments in *untyped.
 * Terminology: generic parameter = function parameter with a
 * type-parameterized type. */
static bool tin_args(const TinInfer *in, TypesTuple *params, Slice args,
                     Slice *untyped) {
    Alloc *a = in->check->a;
    for (Int i = 0; i < args.len; i++) {
        GtOperand *arg = GT_AT(args, i, GtOperand *);
        if (!burrow__tc_op_valid(arg)) {
            /* An error was reported earlier. Ignore this arg and continue, we
             * may still be able to infer all targs resulting in fewer
             * follow-on errors. */
            continue;
        }
        TypesType ptyp = GT_OBJ(GT_AT(params->vars, i, TypesVar *))->typ;
        if (burrow__tc_is_parameterized(a, in->tparams, ptyp) ||
            burrow__tc_is_parameterized(a, in->tparams, arg->typ)) {
            /* Function parameters are always typed. Arguments may be untyped.
             * Collect the indices of untyped arguments and handle them later. */
            if (burrow__types_is_typed(arg->typ)) {
                if (!burrow__tc_unify(in->u, ptyp, arg->typ, GT_UNIFY_ASSIGN)) {
                    tin_errorf(in, ptyp, arg->typ, arg);
                    return false;
                }
            } else if (types_as_type_param(ptyp) != NULL &&
                       !burrow__tc_op_is_nil(arg)) {
                /* Since default types are all basic (i.e., non-composite)
                 * types, an untyped argument will never match a composite
                 * parameter type; the only parameter type it can possibly
                 * match against is a *TypeParam. Thus, for untyped arguments
                 * we only need to look at parameter types that are single
                 * type parameters. Also, untyped nils don't have a default
                 * type and can be ignored. Finally, it's not possible to have
                 * an alias type denoting a type parameter declared by the
                 * current function and use it in the same function
                 * signature; hence we don't need to unalias before the
                 * types_as_type_param above. */
                *untyped = slice_append(a, *untyped, &i, 1);
                if (untyped->p == NULL)
                    burrow__types_oom();
            }
        }
    }
    return true;
}

/* Step 2 of infer: use information from type parameter constraints. Unify
 * type parameters with their constraints as long as progress is being made.
 *
 * This is an O(n^2) algorithm where n is the number of type parameters: if
 * there is progress, at least one type argument is inferred per iteration,
 * and we have a doubly nested loop. In practice this is not a problem
 * because the number of type parameters tends to be very small (< 5 or
 * so). */
static bool tin_constraints(const TinInfer *in, GtPosn posn) {
    TypesChecker *check = in->check;
    for (;;) {
        Int nn = burrow__tc_unifier_unknowns(in->u);
        for (Int i = 0; i < in->tparams.len; i++) {
            TypesTypeParam *tpar = GT_AT(in->tparams, i, TypesTypeParam *);
            TypesType tx = burrow__tc_unifier_at(in->u, tpar);
            bool single;
            TypesTerm *core = tin_core_term(check, tpar, &single);

            /* If the type parameter's constraint has a core term (i.e., a core
             * type with tilde information) try to unify the type parameter
             * with that core type. A type parameter can be unified with its
             * constraint's core type in two cases. */
            if (core != NULL && tx != NULL) {
                /* The corresponding type argument tx is known. There are 2
                 * cases:
                 * 1) If the core type has a tilde, per spec requirement for
                 *    tilde elements, the core type is an underlying (literal)
                 *    type. And because of the tilde, the underlying type of
                 *    tx must match against the core type. But because unify
                 *    automatically matches a defined type against an
                 *    underlying literal type, we can simply unify tx with
                 *    the core type.
                 * 2) If the core type doesn't have a tilde, we also must
                 *    unify tx with the core type. */
                if (!burrow__tc_unify(in->u, tx, core->typ, 0)) {
                    Str ps = burrow__tc_ts(check, types_type(tpar));
                    Str ts = burrow__tc_ts(check, tx);
                    Str cs = burrow__tc_ts(check, types_type_param_constraint(tpar));
                    GT_ADDF(in->err, posn, "%s (type %s) does not satisfy %s", ps, ts,
                            cs);
                    return false;
                }
            } else if (core != NULL && single && !core->tilde) {
                /* The corresponding type argument tx is unknown and the core
                 * term describes a single specific type and no tilde. In this
                 * case the type argument must be that single type; set it. */
                burrow__tc_unifier_set(in->u, tpar, core->typ);
            }

            /* Independent of whether there is a core term, if the type
             * argument tx is known it must implement the methods of the type
             * constraint, possibly after unification of the relevant method
             * signatures, otherwise tx cannot satisfy the constraint. This
             * unification step may provide additional type arguments.
             *
             * Note: The type argument tx may be known but contain references
             * to other type parameters (i.e., tx may still be
             * parameterized). In this case the methods of tx don't correctly
             * reflect the final method set and we may get a missing method
             * error below. Skip this step in this case. */
            if (tx != NULL && !burrow__tc_is_parameterized(check->a, in->tparams, tx)) {
                Str cause = BURROW_STR_EMPTY;
                TypesInterface *constraint = burrow__types_type_param_iface(tpar);
                if (!burrow__types_has_all_methods(
                        check, check->a, tx, types_type(constraint), true,
                        BURROW_FN(GtTypePairFunc, tin_unify_exact, in->u), &cause)) {
                    Str ps = burrow__tc_ts(check, types_type(tpar));
                    Str ts = burrow__tc_ts(check, tx);
                    Str cs = burrow__tc_ts(check, types_type_param_constraint(tpar));
                    GT_ADDF(in->err, posn, "%s (type %s) does not satisfy %s %s", ps,
                            ts, cs, cause);
                    return false;
                }
            }
        }
        if (burrow__tc_unifier_unknowns(in->u) == nn)
            return true; /* no progress */
    }
}

/* Step 3 of infer: use information from untyped constants. Some generic
 * parameters with untyped arguments may have been given a type by now.
 * Collect all remaining parameters that don't have a type yet and determine
 * the maximum untyped type for each of those parameters, if possible. */
static bool tin_untyped(const TinInfer *in, TypesTuple *params, Slice args,
                        Slice untyped) {
    TypesChecker *check = in->check;
    /* Not in Go: maxUntyped is a list by type parameter index rather than a
     * map, so the defaults are set in the order of the type parameters. */
    Slice max_untyped = {0};
    burrow__types_assert(untyped.len == 0 || params != NULL);
    for (Int k = 0; k < untyped.len; k++) {
        Int index = GT_AT(untyped, k, Int);
        /* is type parameter (no alias) by construction of untyped */
        TypesTypeParam *tpar =
            types_as_type_param(GT_OBJ(GT_AT(params->vars, index, TypesVar *))->typ);
        if (burrow__tc_unifier_at(in->u, tpar) != NULL)
            continue;
        GtOperand *arg =
            GT_AT(args, index, GtOperand *); /* arg corresponding to tpar */
        if (max_untyped.p == NULL)
            max_untyped = burrow__types_ptrs_make(check->a, in->tparams.len);
        Int i = tin_index(in->tparams, tpar);
        TypesType max = GT_AT(max_untyped, i, TypesType);
        if (max == NULL) {
            max = arg->typ;
        } else {
            TypesType m = burrow__types_max_type(max, arg->typ);
            if (m == NULL) {
                Str ms = burrow__tc_ts(check, max), as = burrow__tc_ts(check, arg->typ);
                Str ps = burrow__tc_ts(check, types_type(tpar));
                GT_ADDF(in->err, burrow__tc_at_op(arg),
                        "mismatched types %s and %s (cannot infer %s)", ms, as, ps);
                return false;
            }
            max = m;
        }
        GT_AT(max_untyped, i, TypesType) = max;
    }
    /* maxUntyped contains the maximum untyped type for each type parameter
     * which doesn't have a type yet. Set the respective default types. */
    for (Int i = 0; i < max_untyped.len; i++) {
        TypesType typ = GT_AT(max_untyped, i, TypesType);
        if (typ == NULL)
            continue;
        TypesType d = types_default(typ);
        burrow__types_assert(burrow__types_is_typed(d));
        burrow__tc_unifier_set(in->u, GT_AT(in->tparams, i, TypesTypeParam *), d);
    }
    return true;
}

/* The simplify step of infer. inferred now contains the incoming type
 * arguments plus any additional type arguments which were inferred. The
 * inferred non-nil entries may still contain references to other type
 * parameters found in constraints. For instance, for [A any, B interface{ []C
 * }, C interface{ *A }], if A == int was given, unification produced the type
 * list [int, []C, *A]. We eliminate the remaining type parameters by
 * substituting the type parameters in this type list until nothing changes
 * anymore. */
static void tin_simplify(TypesChecker *check, Slice tparams, Slice targs,
                         Slice inferred) {
    /* The data structure of each (provided or inferred) type represents a
     * graph, where each node corresponds to a type and each (directed)
     * vertex points to a component type. The substitution process described
     * above repeatedly replaces type parameter nodes in these graphs with the
     * graphs of the types the type parameters stand for, which creates a new
     * (possibly bigger) graph for each type. The substitution process will
     * not stop if the replacement graph for a type parameter also contains
     * that type parameter. For instance, for [A interface{ *A }], without
     * any type argument provided for A, unification produces the type list
     * [*A]. Substituting A in *A with the value for A will lead to infinite
     * expansion by producing [**A], [****A], [********A], etc., because the
     * graph A -> *A has a cycle through A. Generally, cycles may occur across
     * multiple type parameters and inferred types (for instance, consider
     * [P interface{ *Q }, Q interface{ func(P) }]). We eliminate cycles by
     * walking the graphs for all type parameters. If a cycle through a type
     * parameter is detected, killCycles nils out the respective type (in the
     * inferred list) which kills the cycle, and marks the corresponding type
     * parameter as not inferred. */
    tin_kill_cycles(check->a, tparams, inferred);

    /* dirty tracks the indices of all types that may still contain type
     * parameters. We know that nil type entries and entries corresponding to
     * provided (non-nil) type arguments are clean, so exclude them from the
     * start. */
    Slice dirty = slice_nil(TYPE_INT);
    for (Int i = 0; i < inferred.len; i++) {
        if (GT_AT(inferred, i, TypesType) != NULL &&
            (i >= targs.len || GT_AT(targs, i, TypesType) == NULL)) {
            dirty = slice_append(check->a, dirty, &i, 1);
            if (dirty.p == NULL)
                burrow__types_oom();
        }
    }

    while (dirty.len > 0) {
        Map *smap = burrow__types_make_subst_map(check->a, tparams, inferred);
        Int n = 0;
        for (Int k = 0; k < dirty.len; k++) {
            Int index = GT_AT(dirty, k, Int);
            TypesType t0 = GT_AT(inferred, index, TypesType);
            TypesType t1 = burrow__types_subst(check, check->a, TOKEN_NO_POS, t0, smap,
                                               NULL, burrow__tc_context(check));
            if (t1 == t0)
                continue;
            /* t0 was simplified to t1. If t0 was a generic function, but the
             * simplified signature t1 does not contain any type parameters
             * anymore, the function is not generic anymore. Remove its type
             * parameters. (go.dev/issue/59953) Note that if t0 was a
             * signature, t1 must be a signature, and t1 can only be a generic
             * signature if it originated from a generic function argument.
             * Those signatures are never defined types and thus there is no
             * need to call Underlying below. */
            TypesSignature *sig = types_as_signature(t1);
            if (sig != NULL && types_type_param_list_len(sig->tparams) > 0 &&
                !burrow__tc_is_parameterized(check->a, tparams, t1))
                sig->tparams = NULL;
            GT_AT(inferred, index, TypesType) = t1;
            GT_AT(dirty, n, Int) = index;
            n++;
        }
        dirty.len = n;
    }
}

/* infer attempts to infer the complete set of type arguments for generic
 * function instantiation/call based on the given type parameters tparams,
 * type arguments targs, function parameters params, and function arguments
 * args, if any. There must be at least one type parameter, no more type
 * arguments than type parameters, and params and args must match in number
 * (incl. zero). If reverse is set, an error message's contents are reversed
 * for a better error message for some errors related to reverse type
 * inference (where the function call is synthetic). If successful, infer
 * returns the complete list of given and inferred type arguments, one for
 * each type parameter. Otherwise the result is nil. Errors are reported
 * through the err parameter. Note: infer may fail (return nil) due to invalid
 * args operands without reporting additional errors. */
Slice burrow__tc_infer(TypesChecker *check, GtPosn posn, Slice tparams, Slice targs,
                       TypesTuple *params, Slice args, bool reverse, GtError *err) {
    const Slice none = {0};

    /* There must be at least one type parameter, and no more type arguments
     * than type parameters. */
    Int n = tparams.len;
    burrow__types_assert(n > 0 && targs.len <= n);

    /* Parameters and arguments must match in number. */
    Int nparams = params != NULL ? params->vars.len : 0;
    burrow__types_assert(nparams == args.len);

    /* If we already have all type arguments, we're done. */
    if (targs.len == n && !tin_contains_nil(targs))
        return targs;

    /* If we have invalid (ordinary) arguments, an error was reported before.
     * Avoid additional inference errors and exit early
     * (go.dev/issue/60434). */
    for (Int i = 0; i < args.len; i++)
        if (!burrow__tc_op_valid(GT_AT(args, i, GtOperand *)))
            return none;

    /* Make sure we have a "full" list of type arguments, some of which may be
     * nil (unknown). Make a copy so as to not clobber the incoming slice. */
    if (targs.len < n) {
        Slice targs2 = burrow__types_ptrs_make(check->a, n);
        for (Int i = 0; i < targs.len; i++)
            GT_AT(targs2, i, TypesType) = GT_AT(targs, i, TypesType);
        targs = targs2;
    }
    /* targs.len == n */

    /* Continue with the type arguments we have. Avoid matching generic
     * parameters that already have type arguments against function
     * arguments: It may fail because matching uses type identity while
     * parameter passing uses assignment rules. Instantiate the parameter list
     * with the type arguments we have, and continue with that parameter
     * list.
     *
     * Substitute type arguments for their respective type parameters in
     * params, if any. Note that nil targs entries are ignored by
     * check.subst. We do this for better error messages; it's not needed for
     * correctness. For instance, given:
     *
     *   func f[P, Q any](P, Q) {}
     *
     *   func _(s string) {
     *           f[int](s, s) // ERROR
     *   }
     *
     * With substitution, we get the error:
     *   "cannot use s (variable of type string) as int value in argument to
     *   f[int]"
     *
     * Without substitution we get the (worse) error:
     *   "type string of s does not match inferred type int for P"
     * even though the type int was provided (not inferred) for P. */
    if (nparams > 0) {
        Map *smap = burrow__types_make_subst_map(check->a, tparams, targs);
        params = types_as_tuple(burrow__types_subst(check, check->a, TOKEN_NO_POS,
                                                    types_type(params), smap, NULL,
                                                    burrow__tc_context(check)));
    }

    GtUnifier *u = burrow__tc_new_unifier(
        check, tparams, targs, burrow__tc_allow_version(check, burrow__tc_go1_21));
    TinInfer in = {check, u, tparams, reverse, err};

    /* indices of generic parameters with untyped arguments, for later use */
    Slice untyped = slice_nil(TYPE_INT);

    /* --- 1 --- use information from function arguments */
    if (nparams > 0 && !tin_args(&in, params, args, &untyped))
        return none;

    /* --- 2 --- use information from type parameter constraints */
    if (!tin_constraints(&in, posn))
        return none;

    /* --- 3 --- use information from untyped constants */
    if (!tin_untyped(&in, params, args, untyped))
        return none;

    /* --- simplify --- */
    Slice inferred = burrow__tc_unifier_inferred(u, tparams);
    tin_simplify(check, tparams, targs, inferred);

    /* Once nothing changes anymore, we may still have type parameters left;
     * e.g., a constraint with core type *P may match a type parameter Q but
     * we don't have any type arguments to fill in for *P or Q
     * (go.dev/issue/45548). Don't let such inferences escape; instead treat
     * them as unresolved. */
    for (Int i = 0; i < inferred.len; i++) {
        TypesType typ = GT_AT(inferred, i, TypesType);
        if (typ == NULL || burrow__tc_is_parameterized(check->a, tparams, typ)) {
            GtObject *obj = GT_OBJ(GT_AT(tparams, i, TypesTypeParam *)->obj);
            Str ps = burrow__tc_ps(check, obj->pos);
            GT_ADDF(err, posn, "cannot infer %s (declared at %s)", obj->name, ps);
            return none;
        }
    }
    return inferred;
}

/* renameTParams renames the type parameters in the given type such that each
 * type parameter is given a new identity. renameTParams returns the new type
 * parameters and updated type. If the result type is unchanged from the
 * argument type, none of the type parameters in tparams occurred in the
 * type. If typ is a generic function, type parameters held with typ are not
 * changed and must be updated separately if desired. The position is only
 * used for debug traces. */
Slice burrow__tc_rename_tparams(TypesChecker *check, TokenPos pos, Slice tparams,
                                TypesType typ, TypesType *out) {
    /* For the purpose of type inference we must differentiate type parameters
     * occurring in explicit type or value function arguments from the type
     * parameters we are solving for via unification because they may be the
     * same in self-recursive calls:
     *
     *   func f[P constraint](x P) {
     *           f(x)
     *   }
     *
     * In this example, without type parameter renaming, the P used in the
     * instantiation f[P] has the same pointer identity as the P we are trying
     * to solve for through type inference. This causes problems for type
     * unification. Because any such self-recursive call is equivalent to a
     * mutually recursive call, type parameter renaming can be used to create
     * separate, disentangled type parameters. The above example can be
     * rewritten into the following equivalent code:
     *
     *   func f[P constraint](x P) {
     *           f2(x)
     *   }
     *
     *   func f2[P2 constraint](x P2) {
     *           f(x)
     *   }
     *
     * Type parameter renaming turns the first example into the second example
     * by renaming the type parameter P into P2. */
    if (tparams.len == 0) {
        *out = typ;
        return (Slice){0}; /* nothing to do */
    }

    Slice tparams2 = burrow__types_ptrs_make(check->a, tparams.len);
    for (Int i = 0; i < tparams.len; i++) {
        TypesTypeParam *tparam = GT_AT(tparams, i, TypesTypeParam *);
        GtObject *obj = GT_OBJ(tparam->obj);
        TypesTypeName *tname =
            types_new_type_name(check->a, obj->pos, obj->pkg, obj->name, NULL);
        TypesTypeParam *t2 = types_new_type_param(check->a, tname, NULL);
        t2->index = tparam->index; /* == i */
        GT_AT(tparams2, i, TypesTypeParam *) = t2;
    }

    /* makeRenameMap: a substitution map from tparams to tparams2. */
    Map *rename_map = burrow__types_make_subst_map(check->a, tparams, tparams2);
    for (Int i = 0; i < tparams.len; i++) {
        TypesTypeParam *tparam = GT_AT(tparams, i, TypesTypeParam *);
        GT_AT(tparams2, i, TypesTypeParam *)->bound =
            burrow__types_subst(check, check->a, pos, tparam->bound, rename_map, NULL,
                                burrow__tc_context(check));
    }

    *out = burrow__types_subst(check, check->a, pos, typ, rename_map, NULL,
                               burrow__tc_context(check));
    return tparams2;
}
