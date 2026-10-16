/* go/types: type-checking of union type expressions.
 *
 * Derived from Go's src/go/types/union.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* Avoid excessive type-checking times due to quadratic termlist operations. */
enum { TUN_MAX_TERM_COUNT = 100 };

/* A nil constant.Value. */
static const ConstantValue tun_nil = {0};

static TypesUnion *tun_new_union(Alloc *a, Slice terms) {
    TypesUnion *u = GT_NEW(a, TypesUnion);
    u->hdr.kind = TYPES_KIND_UNION;
    u->terms = terms;
    return u;
}

/* flattenUnion walks a union type expression of the form A | B | C | ...,
 * extracting both the binary exprs (blist) and leaf types (tlist). */
static void tun_flatten_union(Alloc *a, AstExpr x, Slice *blist, Slice *tlist) {
    if (x->kind == AST_KIND_BINARY_EXPR) {
        AstBinaryExpr *o = (AstBinaryExpr *)(void *)x;
        if (o->op == TOKEN_OR) {
            tun_flatten_union(a, o->x, blist, tlist);
            burrow__types_ptrs_append(a, blist, o);
            x = o->y;
        }
    }
    burrow__types_ptrs_append(a, tlist, x);
}

static TypesTerm *tun_parse_tilde(TypesChecker *check, AstExpr tx) {
    AstExpr x = tx;
    bool tilde = false;
    if (x->kind == AST_KIND_UNARY_EXPR) {
        AstUnaryExpr *op = (AstUnaryExpr *)(void *)x;
        if (op->op == TOKEN_TILDE) {
            x = op->x;
            tilde = true;
        }
    }
    TypesType typ = burrow__tc_typ(check, x);
    /* Embedding stand-alone type parameters is not permitted
     * (go.dev/issue/47127). We don't need this restriction anymore if we make
     * the underlying type of a type parameter its constraint interface: if we
     * embed a lone type parameter, we will simply use its underlying type
     * (like we do for other named, embedded interfaces), and since the
     * underlying type is an interface the embedding is well defined. */
    if (burrow__types_is_type_param(typ)) {
        if (tilde)
            GT_ERRORF(check, burrow__tc_at_node(x), GT_CODE_MISPLACED_TYPE_PARAM,
                      "type in term %s cannot be a type parameter",
                      burrow__tc_es(check, tx));
        else
            burrow__tc_error(check, burrow__tc_at_node(x), GT_CODE_MISPLACED_TYPE_PARAM,
                             BURROW_S("term cannot be a type parameter"));
        typ = GT_TYP(TYPES_INVALID);
    }
    TypesTerm *term = types_new_term(check->a, tilde, typ);
    if (tilde) {
        Slice one = burrow__types_ptrs_make(check->a, 1);
        GT_AT(one, 0, TypesTerm *) = term;
        burrow__tc_record_type_and_value(check, tx, GT_MODE_TYPEXPR,
                                         types_type(tun_new_union(check->a, one)),
                                         tun_nil);
    }
    return term;
}

/* overlappingTerm reports the index of the term x in terms which is
 * overlapping (not disjoint) from y. The result is < 0 if there is no such
 * term. The type of term y must not be an interface, and terms with an
 * interface type are ignored in the terms list. */
static Int tun_overlapping_term(Slice terms, Int n, TypesTerm *y) {
    burrow__types_assert(!types_is_interface(y->typ));
    for (Int i = 0; i < n; i++) {
        TypesTerm *x = GT_AT(terms, i, TypesTerm *);
        if (types_is_interface(x->typ))
            continue;
        /* disjoint requires non-nil, non-top arguments, and non-interface
         * types as term types. */
        if (!burrow__types_term_disjoint(x, y))
            return i;
    }
    return -1;
}

/* What the later function of parseUnion closes over. */
typedef struct TunTerms {
    Slice terms; /* of TypesTerm * */
    Slice tlist; /* of AstExpr */
} TunTerms;

static void tun_check_term(TypesChecker *check, TunTerms *c, Int i) {
    TypesTerm *t = GT_AT(c->terms, i, TypesTerm *);
    if (!burrow__types_is_valid(t->typ))
        return;
    GtPosn at = burrow__tc_at_node(GT_AT(c->tlist, i, AstExpr));
    TypesType u = types_type_underlying(t->typ);
    TypesInterface *f = u->kind == TYPES_KIND_INTERFACE ? types_as_interface(u) : NULL;
    if (t->tilde) {
        if (f != NULL) {
            GT_ERRORF(check, at, GT_CODE_INVALID_UNION,
                      "invalid use of ~ (%s is an interface)",
                      burrow__tc_ts(check, t->typ));
            return; /* don't report another error for t */
        }
        if (!types_identical(u, t->typ)) {
            GT_ERRORF(check, at, GT_CODE_INVALID_UNION,
                      "invalid use of ~ (underlying type of %s is %s)",
                      burrow__tc_ts(check, t->typ), burrow__tc_ts(check, u));
            return;
        }
    }

    /* Stand-alone embedded interfaces are ok and are handled by the single-type
     * case in the beginning. Embedded interfaces with tilde are excluded
     * above. If we reach here, we must have at least two terms in the
     * syntactic term list (but not necessarily in the term list of the
     * union's type set). */
    if (f != NULL) {
        GtTypeSet *tset = burrow__types_iface_type_set(f);
        Str ts = types_term_string(check->a, t);
        if (tset->methods.len != 0)
            GT_ERRORF(check, at, GT_CODE_INVALID_UNION,
                      "cannot use %s in union (%s contains methods)", ts, ts);
        else if (t->typ == GT_OBJ(burrow__types_universe_comparable())->typ)
            burrow__tc_error(check, at, GT_CODE_INVALID_UNION,
                             BURROW_S("cannot use comparable in union"));
        else if (tset->comparable)
            GT_ERRORF(check, at, GT_CODE_INVALID_UNION,
                      "cannot use %s in union (%s embeds comparable)", ts, ts);
        return; /* terms with interface types are not subject to the no-overlap rule */
    }

    /* Report overlapping (non-disjoint) terms such as a|a, a|~a, ~a|~a, and
     * ~a|A (where under(A) == a). */
    Int j = tun_overlapping_term(c->terms, i, t);
    if (j >= 0)
        GT_SOFT_ERRORF(check, at, GT_CODE_INVALID_UNION, "overlapping terms %s and %s",
                       types_term_string(check->a, t),
                       types_term_string(check->a, GT_AT(c->terms, j, TypesTerm *)));
}

/* Check validity of terms. Note: This is a quadratic algorithm, but unions
 * tend to be short. */
static void tun_check_terms_later(TypesChecker *check, void *env) {
    TunTerms *c = env;
    for (Int i = 0; i < c->terms.len; i++)
        tun_check_term(check, c, i);
}

/* parseUnion parses uexpr as a union of expressions. The result is a Union
 * type, or Typ[Invalid] for some errors. */
TypesType burrow__tc_parse_union(TypesChecker *check, AstExpr uexpr) {
    Slice blist = slice_nil(TYPE_UNSAFE_POINTER);
    Slice tlist = slice_nil(TYPE_UNSAFE_POINTER);
    tun_flatten_union(check->a, uexpr, &blist, &tlist);
    burrow__types_assert(blist.len == tlist.len - 1);

    Slice terms = slice_nil(TYPE_UNSAFE_POINTER);
    TypesType u = NULL;
    for (Int i = 0; i < tlist.len; i++) {
        AstExpr x = GT_AT(tlist, i, AstExpr);
        TypesTerm *term = tun_parse_tilde(check, x);
        if (tlist.len == 1 && !term->tilde) {
            /* Single type. Ok to return early because all relevant checks have
             * been performed in parseTilde (no need to run through term
             * validity check below). */
            return term->typ; /* typ already recorded through check.typ in parseTilde */
        }
        if (terms.len >= TUN_MAX_TERM_COUNT) {
            if (burrow__types_is_valid(u)) {
                Int max = TUN_MAX_TERM_COUNT;
                GT_ERRORF(check, burrow__tc_at_node(x), GT_CODE_INVALID_UNION,
                          "cannot handle more than %d union terms (implementation "
                          "limitation)",
                          max);
                u = GT_TYP(TYPES_INVALID);
            }
        } else {
            burrow__types_ptrs_append(check->a, &terms, term);
            u = types_type(tun_new_union(check->a, terms));
        }
        if (i > 0)
            burrow__tc_record_type_and_value(check, GT_AT(blist, i - 1, AstExpr),
                                             GT_MODE_TYPEXPR, u, tun_nil);
    }

    if (!burrow__types_is_valid(u))
        return u;

    /* Do this check later because it requires types to be set up. */
    TunTerms *c = GT_NEW(check->a, TunTerms);
    c->terms = terms;
    c->tlist = tlist;
    (void)burrow__tc_later(check, tun_check_terms_later, c);
    return u;
}
