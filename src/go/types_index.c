/* go/types: typechecking of index and slice expressions.
 *
 * Derived from Go's src/go/types/index.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* singleIndex returns the (single) index from the index expression e. If the
 * index is missing, or if there are multiple indices, an error is reported and
 * the result is NULL. */
static AstExpr tix_single_index(TypesChecker *check, const GtIndexedExpr *expr) {
    if (expr->indices.len == 0) {
        /* Not in Go: Go prints the indexedExpr struct here; this prints the
         * expression it wraps. */
        Str es = burrow__tc_es(check, expr->orig);
        GT_ERRORF(check, burrow__tc_at_node(expr->orig), GT_CODE_INVALID_SYNTAX_TREE,
                  "index expression %s with 0 indices", es);
        return NULL;
    }
    if (expr->indices.len > 1) {
        burrow__tc_error(check, burrow__tc_at_node(GT_AT(expr->indices, 1, AstExpr)),
                         GT_CODE_INVALID_INDEX,
                         BURROW_S(GT_INVALID_OP "more than one index"));
    }
    return GT_AT(expr->indices, 0, AstExpr);
}

/* What the underIs walk of indexing a type parameter closes over. */
typedef struct TixUnder {
    GtOperand *x;
    TypesType key, elem; /* key != NULL: we must have all maps */
    GtMode mode;         /* non-maps result mode */
    int64_t length;
} TixUnder;

static bool tix_under_index(void *env, TypesType t, TypesType u) {
    (void)t;
    TixUnder *c = env;
    int64_t l = -1;               /* valid if >= 0 */
    TypesType k = NULL, e = NULL; /* k is only set for maps */
    switch (u != NULL ? u->kind : TYPES_KIND_INVALID) {
    case TYPES_KIND_BASIC:
        if (burrow__types_is_basic(u, TYPES_IS_STRING)) {
            e = types_type(burrow__types_universe_byte);
            c->mode = GT_MODE_VALUE;
        }
        break;
    case TYPES_KIND_ARRAY: {
        TypesArray *a = (TypesArray *)(void *)u;
        l = a->len;
        e = a->elem;
        if (c->x->mode != GT_MODE_VARIABLE)
            c->mode = GT_MODE_VALUE;
        break;
    }
    case TYPES_KIND_POINTER: {
        TypesArray *a =
            types_as_array(types_type_underlying(((TypesPointer *)(void *)u)->base));
        if (a != NULL) {
            l = a->len;
            e = a->elem;
        }
        break;
    }
    case TYPES_KIND_SLICE:
        e = ((TypesSlice *)(void *)u)->elem;
        break;
    case TYPES_KIND_MAP:
        k = ((TypesMap *)(void *)u)->key;
        e = ((TypesMap *)(void *)u)->elem;
        break;
    default:
        break;
    }
    if (e == NULL)
        return false;
    if (c->elem == NULL) {
        /* first type */
        c->length = l;
        c->key = k;
        c->elem = e;
        return true;
    }
    /* all map keys must be identical (incl. all nil) (that is, we cannot mix
     * maps with other types) */
    if (!types_identical(c->key, k))
        return false;
    /* all element types must be identical */
    if (!types_identical(c->elem, e))
        return false;
    /* track the minimal length for arrays, if any */
    if (l >= 0 && l < c->length)
        c->length = l;
    return true;
}

/* tix_map_index checks the index of a map index expression against the key
 * type and makes x the map element. */
static bool tix_map_index(TypesChecker *check, GtOperand *x, const GtIndexedExpr *e,
                          TypesType key, TypesType elem) {
    AstExpr index = tix_single_index(check, e);
    if (index == NULL) {
        burrow__tc_op_invalidate(x);
        return false;
    }
    GtOperand k = {0};
    burrow__tc_generic_expr(check, &k, index, NULL);
    burrow__tc_assignment(check, &k, key, BURROW_S("map index"));
    /* ok to continue even if indexing failed - map element type is known */
    x->mode = GT_MODE_MAPINDEX;
    x->typ = elem;
    x->expr = e->orig;
    return false;
}

/* If e is a valid function instantiation, indexExpr returns true. In that case
 * x represents the uninstantiated function value and it is the caller's
 * responsibility to instantiate the function. */
bool burrow__tc_index_expr(TypesChecker *check, GtOperand *x, GtIndexedExpr *e) {
    burrow__tc_expr_or_type(check, x, e->x, true);
    /* x may be generic */

    switch (x->mode) {
    case GT_MODE_INVALID:
        burrow__tc_use(check, e->indices);
        return false;

    case GT_MODE_TYPEXPR:
        /* type instantiation */
        burrow__tc_op_invalidate(x);
        x->typ = burrow__tc_var_type(check, e->orig);
        if (burrow__types_is_valid(x->typ))
            x->mode = GT_MODE_TYPEXPR;
        return false;

    case GT_MODE_VALUE: {
        TypesSignature *sig = types_as_signature(types_type_underlying(x->typ));
        if (sig != NULL && types_type_param_list_len(sig->tparams) > 0) {
            /* function instantiation */
            return true;
        }
        break;
    }

    case GT_MODE_NOVALUE:
    case GT_MODE_BUILTIN:
    case GT_MODE_CONSTANT:
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
    case GT_MODE_NILVALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
    case GT_MODE_CGOFUNC:
    default:
        break;
    }

    /* x should not be generic at this point, but be safe and check */
    burrow__tc_non_generic(check, NULL, x);
    if (!burrow__tc_op_valid(x))
        return false;

    /* We cannot index on an incomplete type; make sure it's complete. */
    if (!burrow__tc_is_complete(check, x->typ)) {
        burrow__tc_op_invalidate(x);
        return false;
    }
    TypesType u = types_type_underlying(x->typ);
    if (u->kind == TYPES_KIND_POINTER) {
        /* Additionally, if x.typ is a pointer to an array type, indexing
         * implicitly dereferences the value, meaning its base type must also
         * be complete. */
        if (!burrow__tc_is_complete(check, ((TypesPointer *)(void *)u)->base)) {
            burrow__tc_op_invalidate(x);
            return false;
        }
    } else if (u->kind == TYPES_KIND_MAP) {
        /* Lastly, if x.typ is a map type, indexing must produce a value of a
         * complete type, meaning its element type must also be complete. */
        if (!burrow__tc_is_complete(check, ((TypesMap *)(void *)u)->elem)) {
            burrow__tc_op_invalidate(x);
            return false;
        }
    }

    /* ordinary index expression */
    bool valid = false;
    int64_t length = -1; /* valid if >= 0 */
    u = types_type_underlying(x->typ);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        if (burrow__types_is_basic(u, TYPES_IS_STRING)) {
            valid = true;
            if (x->mode == GT_MODE_CONSTANT)
                length = constant_string_len(x->val);
            /* an indexed string always yields a byte value (not a constant)
             * even if the string and the index are constant */
            x->mode = GT_MODE_VALUE;
            x->typ = types_type(burrow__types_universe_byte); /* use 'byte' name */
        }
        break;

    case TYPES_KIND_ARRAY: {
        TypesArray *typ = (TypesArray *)(void *)u;
        valid = true;
        length = typ->len;
        if (x->mode != GT_MODE_VARIABLE)
            x->mode = GT_MODE_VALUE;
        x->typ = typ->elem;
        break;
    }

    case TYPES_KIND_POINTER: {
        TypesArray *typ =
            types_as_array(types_type_underlying(((TypesPointer *)(void *)u)->base));
        if (typ != NULL) {
            valid = true;
            length = typ->len;
            x->mode = GT_MODE_VARIABLE;
            x->typ = typ->elem;
        }
        break;
    }

    case TYPES_KIND_SLICE:
        valid = true;
        x->mode = GT_MODE_VARIABLE;
        x->typ = ((TypesSlice *)(void *)u)->elem;
        break;

    case TYPES_KIND_MAP: {
        TypesMap *typ = (TypesMap *)(void *)u;
        return tix_map_index(check, x, e, typ->key, typ->elem);
    }

    case TYPES_KIND_INTERFACE: {
        if (!burrow__types_is_type_param(x->typ))
            break;
        TixUnder c = {x, NULL, NULL, GT_MODE_VARIABLE, -1};
        if (burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tix_under_index, &c))) {
            length = c.length;
            /* For maps, the index expression must be assignable to the map key
             * type. */
            if (c.key != NULL)
                return tix_map_index(check, x, e, c.key, c.elem);

            /* no maps */
            valid = true;
            x->mode = c.mode;
            x->typ = c.elem;
        }
        break;
    }

    default:
        break;
    }

    if (!valid) {
        /* types2 uses the position of '[' for the error */
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NON_INDEXABLE_OPERAND,
                  "cannot index %s", xs);
        burrow__tc_use(check, e->indices);
        burrow__tc_op_invalidate(x);
        return false;
    }

    AstExpr index = tix_single_index(check, e);
    if (index == NULL) {
        burrow__tc_op_invalidate(x);
        return false;
    }

    /* In pathological (invalid) cases (e.g.: type T1 [][[]T1{}[0][0]]T0) the
     * element type may be accessed before it's set. Make sure we have a valid
     * type. */
    if (x->typ == NULL)
        x->typ = GT_TYP(TYPES_INVALID);

    int64_t val;
    (void)burrow__tc_index(check, index, length, &val);
    return false;
}

/* What the typeset walk of a slice expression closes over. */
typedef struct TixSlice {
    TypesChecker *check;
    GtOperand *x;
    TypesType ct, cu; /* type and respective common underlying type */
    bool has_string;
} TixSlice;

static bool tix_under_slice(void *env, TypesType t, TypesType u) {
    TixSlice *c = env;
    if (u == NULL) {
        Str xs = burrow__tc_xs(c->check, c->x);
        Str ts = burrow__tc_ts(c->check, c->x->typ);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_NON_SLICEABLE_OPERAND,
                  "cannot slice %s: no specific type in %s", xs, ts);
        c->cu = NULL;
        return false;
    }

    /* Treat strings like byte slices but remember that we saw a string. */
    if (burrow__types_is_basic(u, TYPES_IS_STRING)) {
        u = types_type(
            types_new_slice(c->check->a, types_type(burrow__types_universe_byte)));
        c->has_string = true;
    }

    /* If this is the first type we're seeing, we're done. */
    if (c->cu == NULL) {
        c->ct = t;
        c->cu = u;
        return true;
    }

    /* Otherwise, the current type must have the same underlying type as all
     * previous types. */
    if (!types_identical(c->cu, u)) {
        Str xs = burrow__tc_xs(c->check, c->x);
        Str cts = burrow__tc_ts(c->check, c->ct);
        Str ts = burrow__tc_ts(c->check, t);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_NON_SLICEABLE_OPERAND,
                  "cannot slice %s: %s and %s have different underlying types", xs, cts,
                  ts);
        c->cu = NULL;
        return false;
    }

    return true;
}

void burrow__tc_slice_expr(TypesChecker *check, GtOperand *x, AstSliceExpr *e) {
    burrow__tc_expr(check, NULL, x, e->x);
    if (!burrow__tc_op_valid(x)) {
        burrow__tc_use1(check, e->low, false);
        burrow__tc_use1(check, e->high, false);
        burrow__tc_use1(check, e->max, false);
        return;
    }

    /* determine common underlying type cu */
    TixSlice c = {check, x, NULL, NULL, false};
    (void)burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tix_under_slice, &c));
    TypesType cu = c.cu;
    if (c.has_string) {
        /* If we saw a string, proceed with string type, but don't go from
         * untyped string to string. */
        cu = GT_TYP(TYPES_STRING);
        if (!burrow__types_is_type_param(x->typ))
            cu = types_type_underlying(x->typ); /* untyped string remains untyped */
    }

    /* Note that we don't permit slice expressions where x is a type
     * expression, so we don't check for that here. However, if x.typ is a
     * pointer to an array type, slicing implicitly dereferences the value,
     * meaning its base type must also be complete. */
    TypesPointer *p = types_as_pointer(types_type_underlying(x->typ));
    if (p != NULL && !burrow__tc_is_complete(check, p->base)) {
        burrow__tc_op_invalidate(x);
        return;
    }

    bool valid = false;
    int64_t length = -1; /* valid if >= 0 */
    if (cu == NULL) {
        /* error reported above */
        burrow__tc_op_invalidate(x);
        return;
    }
    switch (cu->kind) {
    case TYPES_KIND_BASIC:
        if (burrow__types_is_basic(cu, TYPES_IS_STRING)) {
            if (e->slice3) {
                AstExpr at = e->max;
                if (at == NULL)
                    at = GT_NODE(e); /* e.Index[2] should be present but be careful */
                burrow__tc_error(check, burrow__tc_at_node(at),
                                 GT_CODE_INVALID_SLICE_EXPR,
                                 BURROW_S(GT_INVALID_OP "3-index slice of string"));
                burrow__tc_op_invalidate(x);
                return;
            }
            valid = true;
            if (x->mode == GT_MODE_CONSTANT)
                length = constant_string_len(x->val);
            /* spec: "For untyped string operands the result is a non-constant
             * value of type string." */
            if (!burrow__types_is_typed(x->typ))
                x->typ = GT_TYP(TYPES_STRING);
        }
        break;

    case TYPES_KIND_ARRAY: {
        TypesArray *u = (TypesArray *)(void *)cu;
        valid = true;
        length = u->len;
        if (x->mode != GT_MODE_VARIABLE) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NON_SLICEABLE_OPERAND,
                      "cannot slice unaddressable value %s", xs);
            burrow__tc_op_invalidate(x);
            return;
        }
        x->typ = types_type(types_new_slice(check->a, u->elem));
        break;
    }

    case TYPES_KIND_POINTER: {
        TypesArray *u =
            types_as_array(types_type_underlying(((TypesPointer *)(void *)cu)->base));
        if (u != NULL) {
            valid = true;
            length = u->len;
            x->typ = types_type(types_new_slice(check->a, u->elem));
        }
        break;
    }

    case TYPES_KIND_SLICE:
        valid = true;
        /* x.typ doesn't change */
        break;

    default:
        break;
    }

    if (!valid) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NON_SLICEABLE_OPERAND,
                  "cannot slice %s", xs);
        burrow__tc_op_invalidate(x);
        return;
    }

    x->mode = GT_MODE_VALUE;

    /* spec: "Only the first index may be omitted; it defaults to 0." */
    if (e->slice3 && (e->high == NULL || e->max == NULL)) {
        burrow__tc_error(check, burrow__tc_in_node(GT_NODE(e), e->rbrack),
                         GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("2nd and 3rd index required in 3-index slice"));
        burrow__tc_op_invalidate(x);
        return;
    }

    /* check indices */
    AstExpr exprs[3] = {e->low, e->high, e->max};
    int64_t ind[3];
    for (int i = 0; i < 3; i++) {
        int64_t v = -1;
        if (exprs[i] != NULL) {
            /* The "capacity" is only known statically for strings, arrays,
             * and pointers to arrays, and it is the same as the length for
             * those types. */
            int64_t max = -1;
            if (length >= 0)
                max = length + 1;
            int64_t iv;
            (void)burrow__tc_index(check, exprs[i], max, &iv);
            if (iv >= 0)
                v = iv;
        } else if (i == 0) {
            /* default is 0 for the first index */
            v = 0;
        } else if (length >= 0) {
            /* default is length (== capacity) otherwise */
            v = length;
        }
        ind[i] = v;
    }

    /* constant indices must be in range (check.index already checks that
     * existing indices >= 0) */
    for (int i = 0; i < 2; i++) {
        int64_t xv = ind[i];
        if (xv <= 0)
            continue;
        for (int j = i + 1; j < 3; j++) {
            int64_t y = ind[j];
            if (y >= 0 && y < xv) {
                /* The value y corresponds to the expression e.Index[j].
                 * Because y >= 0, it must have been set from the expression
                 * when checking indices and thus e.Index[j] is not nil. */
                GT_ERRORF(check, burrow__tc_at_node(exprs[j]),
                          GT_CODE_SWAPPED_SLICE_INDICES,
                          "invalid slice indices: %d < %d", y, xv);
                return; /* only report one error, ok to continue */
            }
        }
    }
}

TypesType burrow__tc_index(TypesChecker *check, AstExpr index, int64_t max,
                           int64_t *val) {
    *val = -1;

    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, index);
    if (!burrow__tc_is_valid_index(check, &x, GT_CODE_INVALID_INDEX, BURROW_S("index"),
                                   false))
        return GT_TYP(TYPES_INVALID);

    if (x.mode != GT_MODE_CONSTANT)
        return x.typ;

    if (constant_value_kind(x.val) == CONSTANT_UNKNOWN)
        return GT_TYP(TYPES_INVALID);

    bool ok;
    int64_t v = constant_int64_val(x.val, &ok);
    burrow__types_assert(ok);
    if (max >= 0 && v >= max) {
        Str vs = constant_value_string(x.val, check->a);
        GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_INDEX,
                  GT_INVALID_ARG "index %s out of bounds [0:%d]", vs, max);
        return GT_TYP(TYPES_INVALID);
    }

    /* 0 <= v [ && v < max ] */
    *val = v;
    return x.typ;
}

bool burrow__tc_is_valid_index(TypesChecker *check, GtOperand *x, GtCode code, Str what,
                               bool allow_negative) {
    if (!burrow__tc_op_valid(x))
        return false;

    /* spec: "a constant index that is untyped is given type int" */
    burrow__tc_convert_untyped(check, x, GT_TYP(TYPES_INT));
    if (!burrow__tc_op_valid(x))
        return false;

    /* spec: "the index x must be of integer type or an untyped constant" */
    if (!burrow__types_all_basic(x->typ, TYPES_IS_INTEGER)) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), code,
                  GT_INVALID_ARG "%s %s must be integer", what, xs);
        return false;
    }

    if (x->mode == GT_MODE_CONSTANT) {
        /* spec: "a constant index must be non-negative ..." */
        if (!allow_negative && constant_sign(x->val) < 0) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), code,
                      GT_INVALID_ARG "%s %s must not be negative", what, xs);
            return false;
        }

        /* spec: "... and representable by a value of type int" */
        if (!burrow__tc_representable_const(check->a, x->val, check,
                                            types_typ[TYPES_INT], &x->val)) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), code,
                      GT_INVALID_ARG "%s %s overflows int", what, xs);
            return false;
        }
    }

    return true;
}

bool burrow__tc_unpack_indexed_expr(Alloc *a, AstNode n, GtIndexedExpr *ix) {
    if (n == NULL)
        return false;
    if (n->kind == AST_KIND_INDEX_EXPR) {
        AstIndexExpr *e = (AstIndexExpr *)(void *)n;
        ix->orig = n;
        ix->x = e->x;
        ix->lbrack = e->lbrack;
        ix->indices = burrow__types_ptrs_make(a, 1);
        GT_AT(ix->indices, 0, AstExpr) = e->index;
        ix->rbrack = e->rbrack;
        return true;
    }
    if (n->kind == AST_KIND_INDEX_LIST_EXPR) {
        AstIndexListExpr *e = (AstIndexListExpr *)(void *)n;
        ix->orig = n;
        ix->x = e->x;
        ix->lbrack = e->lbrack;
        ix->indices = e->indices;
        ix->rbrack = e->rbrack;
        return true;
    }
    return false;
}
