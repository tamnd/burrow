/* go/types: AssignableTo and ConvertibleTo.
 *
 * Go answers both by making a non-constant operand of type V and running the
 * checker's own assignment and conversion rules on it, with a nil *Checker
 * and no cause. That is all that is done here, so none of the error text
 * those rules can build is needed, and there is no language version to hold
 * any rule back.
 *
 * Derived from Go's src/go/types/api_predicates.go, operand.go, expr.go and
 * conversions.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/panic.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

static bool gt_assignable_to(TypesType xt, TypesType T);
static bool gt_convertible_to(TypesType xt, TypesType T);

/* ----------------------------------------------------- implicitTypeAndValue */

/* Go's x.isNil() for an operand of mode value. */
static bool gt_is_nil(TypesType xt) {
    return xt == GT_TYP(TYPES_UNTYPED_NIL);
}

static bool gt_implicit_ok(TypesType xt, TypesType target);

static bool gt_under_implicit_ok(void *env, TypesType t, TypesType u) {
    (void)t;
    return u != NULL && gt_implicit_ok(*(TypesType *)env, u);
}

/* Go's implicitTypeAndValue for a non-constant operand of type xt, reporting
 * whether the type it finds is not nil. A non-constant untyped value is the
 * result of a comparison, the left side of a shift, or nil. */
static bool gt_implicit_ok(TypesType xt, TypesType target) {
    if (burrow__types_is_typed(xt) || !burrow__types_is_valid(target))
        return true;
    /* xt is untyped */

    if (!burrow__types_is_typed(target)) {
        /* both xt and target are untyped */
        return burrow__types_max_type(xt, target) != NULL;
    }

    TypesType u = types_type_underlying(target);
    switch (u == NULL ? (Int)TYPES_KIND_INVALID : u->kind) {
    case TYPES_KIND_BASIC:
        switch (types_as_basic(xt)->kind) {
        case TYPES_UNTYPED_BOOL:
            return burrow__types_is_basic(target, TYPES_IS_BOOLEAN);
        case TYPES_UNTYPED_INT:
        case TYPES_UNTYPED_RUNE:
        case TYPES_UNTYPED_FLOAT:
        case TYPES_UNTYPED_COMPLEX:
            return burrow__types_is_basic(target, TYPES_IS_NUMERIC);
        case TYPES_UNTYPED_STRING:
            /* Non-constant untyped string values are not permitted by the
             * spec and should not occur during normal type checking, but this
             * path is reachable through AssignableTo. */
            return burrow__types_is_basic(target, TYPES_IS_STRING);
        case TYPES_UNTYPED_NIL:
            /* unsafe.Pointer is a basic type that includes nil. */
            return burrow__types_has_nil(target);
        default:
            return false;
        }
    case TYPES_KIND_INTERFACE:
        if (burrow__types_is_type_param(target))
            return burrow__types_all(
                target, BURROW_FN(GtTypePairFunc, gt_under_implicit_ok, &xt));
        /* Values must have concrete dynamic types, but nil is kept untyped. */
        if (gt_is_nil(xt))
            return true;
        /* cannot assign untyped values to non-empty interfaces */
        return types_interface_empty(types_as_interface(u));
    case TYPES_KIND_POINTER:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        return gt_is_nil(xt);
    default:
        return false;
    }
}

/* -------------------------------------------------------------- assignableTo */

typedef struct GtOperandEnv {
    TypesType x; /* the operand's type */
    TypesType T; /* the target */
} GtOperandEnv;

/* each specific type in T's type set takes the untyped x */
static bool gt_term_implicit_ok(void *env, TypesTerm *t) {
    return t != NULL && gt_implicit_ok(((GtOperandEnv *)env)->x, t->typ);
}

/* x is assignable to the term */
static bool gt_term_assignable_from(void *env, TypesTerm *t) {
    return t != NULL && gt_assignable_to(((GtOperandEnv *)env)->x, t->typ);
}

/* the term is assignable to T */
static bool gt_term_assignable_to(void *env, TypesTerm *t) {
    return t != NULL && gt_assignable_to(t->typ, ((GtOperandEnv *)env)->T);
}

static GtTypeSet *gt_tparam_set(TypesTypeParam *p) {
    return burrow__types_iface_type_set(burrow__types_type_param_iface(p));
}

/* Go's x.assignableTo for a non-constant operand of type xt. Where Go sets
 * the operand's type to each term of V's type set, this calls itself with the
 * term's type, which is the same thing. */
static bool gt_assignable_to(TypesType xt, TypesType T) {
    if (!burrow__types_is_valid(T))
        return true; /* avoid spurious errors */

    TypesType V = types_unalias(xt);
    T = types_unalias(T);

    /* x's type is identical to T */
    if (types_identical(V, T))
        return true;

    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    TypesTypeParam *Vp = types_as_type_param(V);
    TypesTypeParam *Tp = types_as_type_param(T);
    GtOperandEnv env = {xt, T};

    /* x is an untyped value representable by a value of type T. */
    if (!burrow__types_is_typed(Vu)) {
        if (Tp != NULL)
            /* T is a type parameter: x is assignable to T if it is
             * representable by each specific type in the type set of T. */
            return burrow__types_type_set_is(
                gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_implicit_ok, &env));
        return gt_implicit_ok(xt, T);
    }
    /* Vu is typed */

    /* x's type V and T have identical underlying types and at least one of V
     * or T is not a named type and neither V nor T is a type parameter. */
    if (types_identical(Vu, Tu) &&
        (!burrow__types_has_name(V) || !burrow__types_has_name(T)) && Vp == NULL &&
        Tp == NULL)
        return true;

    /* T is an interface type, but not a type parameter, and V implements T.
     * Also handle the case where T is a pointer to an interface. */
    if ((types_as_interface(Tu) != NULL && Tp == NULL) ||
        burrow__types_is_interface_ptr(Tu)) {
        if (burrow__types_implements(NULL, V, T, false, NULL))
            return true;
        /* V doesn't implement T but V may still be assignable to T if V is a
         * type parameter. */
        if (Vp == NULL)
            return false;
    }

    /* If V is an interface, check if a missing type assertion is the
     * problem. */
    if (types_as_interface(Vu) != NULL && Vp == NULL) {
        if (burrow__types_implements(NULL, T, V, false, NULL))
            return false; /* T implements V, so it needs a type assertion */
    }

    /* x is a bidirectional channel value, T is a channel type, x's type V and
     * T have identical element types, and at least one of V or T is not a
     * named type. */
    TypesChan *Vc = types_as_chan(Vu);
    if (Vc != NULL && types_chan_dir(Vc) == TYPES_SEND_RECV) {
        TypesChan *Tc = types_as_chan(Tu);
        if (Tc != NULL && types_identical(types_chan_elem(Vc), types_chan_elem(Tc)))
            return !burrow__types_has_name(V) || !burrow__types_has_name(T);
    }

    /* optimization: if we don't have type parameters, we're done */
    if (Vp == NULL && Tp == NULL)
        return false;

    /* x's type V is not a named type and T is a type parameter, and x is
     * assignable to each specific type in T's type set. */
    if (!burrow__types_has_name(V) && Tp != NULL)
        return burrow__types_type_set_is(
            gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_assignable_from, &env));

    /* x's type V is a type parameter and T is not a named type, and values x'
     * of each specific type in V's type set are assignable to T. */
    if (Vp != NULL && !burrow__types_has_name(T))
        return burrow__types_type_set_is(
            gt_tparam_set(Vp), BURROW_FN(GtTermFunc, gt_term_assignable_to, &env));

    return false;
}

/* ------------------------------------------------------------- convertibleTo */

static bool gt_is_kind(TypesType t, TypesBasicKind kind) {
    TypesBasic *b = types_as_basic(types_type_underlying(t));
    return b != NULL && b->kind == kind;
}

static bool gt_operand_is_pointer(TypesType t) {
    return types_as_pointer(types_type_underlying(t)) != NULL;
}

static bool gt_is_bytes_or_runes(TypesType t) {
    TypesSlice *s = types_as_slice(types_type_underlying(t));
    if (s == NULL)
        return false;
    TypesBasic *b = types_as_basic(types_type_underlying(types_slice_elem(s)));
    return b != NULL && (b->kind == TYPES_BYTE || b->kind == TYPES_RUNE);
}

/* the term converts to T */
static bool gt_term_convertible_to(void *env, TypesTerm *t) {
    return t != NULL && gt_convertible_to(t->typ, ((GtOperandEnv *)env)->T);
}

/* x converts to the term */
static bool gt_term_convertible_from(void *env, TypesTerm *t) {
    return t != NULL && gt_convertible_to(((GtOperandEnv *)env)->x, t->typ);
}

typedef struct GtBothEnv {
    TypesTypeParam *Tp;
} GtBothEnv;

/* each term of Vp converts to each term of Tp */
static bool gt_term_convertible_to_all(void *env, TypesTerm *t) {
    if (t == NULL)
        return false;
    GtOperandEnv inner = {t->typ, NULL};
    return burrow__types_type_set_is(
        gt_tparam_set(((GtBothEnv *)env)->Tp),
        BURROW_FN(GtTermFunc, gt_term_convertible_from, &inner));
}

/* Go's x.convertibleTo for a non-constant operand of type xt. */
static bool gt_convertible_to(TypesType xt, TypesType T) {
    /* "x is assignable to T" */
    if (gt_assignable_to(xt, T))
        return true;

    TypesType V = types_unalias(xt);
    T = types_unalias(T);
    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    TypesTypeParam *Vp = types_as_type_param(V);
    TypesTypeParam *Tp = types_as_type_param(T);

    /* "V and T have identical underlying types if tags are ignored and V and
     * T are not type parameters" */
    if (types_identical_ignore_tags(Vu, Tu) && Vp == NULL && Tp == NULL)
        return true;

    /* "V and T are unnamed pointer types and their pointer base types have
     * identical underlying types if tags are ignored and their pointer base
     * types are not type parameters" */
    TypesPointer *Vptr = types_as_pointer(V);
    TypesPointer *Tptr = types_as_pointer(T);
    if (Vptr != NULL && Tptr != NULL) {
        TypesType vb = types_pointer_elem(Vptr), tb = types_pointer_elem(Tptr);
        if (types_identical_ignore_tags(types_type_underlying(vb),
                                        types_type_underlying(tb)) &&
            !burrow__types_is_type_param(vb) && !burrow__types_is_type_param(tb))
            return true;
    }

    /* "V and T are both integer or floating point types" */
    if (burrow__types_is_basic(Vu, TYPES_IS_INTEGER | TYPES_IS_FLOAT) &&
        burrow__types_is_basic(Tu, TYPES_IS_INTEGER | TYPES_IS_FLOAT))
        return true;

    /* "V and T are both complex types" */
    if (burrow__types_is_basic(Vu, TYPES_IS_COMPLEX) &&
        burrow__types_is_basic(Tu, TYPES_IS_COMPLEX))
        return true;

    /* "V is an integer or a slice of bytes or runes and T is a string type" */
    if ((burrow__types_is_basic(Vu, TYPES_IS_INTEGER) || gt_is_bytes_or_runes(Vu)) &&
        burrow__types_is_basic(Tu, TYPES_IS_STRING))
        return true;

    /* "V is a string and T is a slice of bytes or runes" */
    if (burrow__types_is_basic(Vu, TYPES_IS_STRING) && gt_is_bytes_or_runes(Tu))
        return true;

    /* package unsafe: "any pointer or value of underlying type uintptr can be
     * converted into a unsafe.Pointer" */
    if ((gt_operand_is_pointer(Vu) || gt_is_kind(Vu, TYPES_UINTPTR)) &&
        gt_is_kind(Tu, TYPES_UNSAFE_POINTER))
        return true;
    /* "and vice versa" */
    if (gt_is_kind(Vu, TYPES_UNSAFE_POINTER) &&
        (gt_operand_is_pointer(Tu) || gt_is_kind(Tu, TYPES_UINTPTR)))
        return true;

    /* "V is a slice, T is an array or pointer-to-array type, and the slice and
     * array types have identical element types." */
    TypesSlice *s = types_as_slice(Vu);
    if (s != NULL) {
        TypesArray *arr = types_as_array(Tu);
        TypesPointer *p = types_as_pointer(Tu);
        if (arr != NULL) {
            if (types_identical(types_slice_elem(s), types_array_elem(arr)))
                return true;
        } else if (p != NULL) {
            arr = types_as_array(types_type_underlying(types_pointer_elem(p)));
            if (arr != NULL &&
                types_identical(types_slice_elem(s), types_array_elem(arr)))
                return true;
        }
    }

    /* optimization: if we don't have type parameters, we're done */
    if (Vp == NULL && Tp == NULL)
        return false;

    /* generic cases with specific type terms (generic operands cannot be
     * constants) */
    GtOperandEnv env = {xt, T};
    if (Vp != NULL && Tp != NULL) {
        GtBothEnv both = {Tp};
        return burrow__types_type_set_is(
            gt_tparam_set(Vp),
            BURROW_FN(GtTermFunc, gt_term_convertible_to_all, &both));
    }
    if (Vp != NULL)
        return burrow__types_type_set_is(
            gt_tparam_set(Vp), BURROW_FN(GtTermFunc, gt_term_convertible_to, &env));
    return burrow__types_type_set_is(
        gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_convertible_from, &env));
}

/* ------------------------------------------------------------------- the API */

bool types_assignable_to(TypesType V, TypesType T) {
    return gt_assignable_to(V, T);
}

bool types_convertible_to(TypesType V, TypesType T) {
    return gt_convertible_to(V, T);
}

/* ------------------------------------------------------------------ operands */

static const Str gt_operand_mode_string[] = {
    [GT_MODE_INVALID] = BURROW_S_INIT("invalid operand"),
    [GT_MODE_NOVALUE] = BURROW_S_INIT("no value"),
    [GT_MODE_BUILTIN] = BURROW_S_INIT("built-in"),
    [GT_MODE_TYPEXPR] = BURROW_S_INIT("type"),
    [GT_MODE_CONSTANT] = BURROW_S_INIT("constant"),
    [GT_MODE_VARIABLE] = BURROW_S_INIT("variable"),
    [GT_MODE_MAPINDEX] = BURROW_S_INIT("map index expression"),
    [GT_MODE_VALUE] = BURROW_S_INIT("value"),
    [GT_MODE_NILVALUE] = BURROW_S_INIT("nil"), /* only used by types2 */
    [GT_MODE_COMMAOK] = BURROW_S_INIT("comma, ok expression"),
    [GT_MODE_COMMAERR] = BURROW_S_INIT("comma, error expression"),
    [GT_MODE_CGOFUNC] = BURROW_S_INIT("cgo function"),
};

bool burrow__tc_op_valid(const GtOperand *x) {
    return x->mode != GT_MODE_INVALID;
}

void burrow__tc_op_invalidate(GtOperand *x) {
    x->mode = GT_MODE_INVALID;
}

/* Pos returns the position of the expression corresponding to x. If x is
 * invalid the position is nopos. */
TokenPos burrow__tc_op_pos(const GtOperand *x) {
    /* x.expr may not be set if x is invalid */
    if (x->expr == NULL)
        return TOKEN_NO_POS;
    return ast_expr_pos(x->expr);
}

/* Operand string formats (not all "untyped" cases can appear due to the type
 * system, but they fall out naturally here)
 *
 * mode       format
 *
 * invalid    <expr> (               <mode>                    )
 * novalue    <expr> (               <mode>                    )
 * builtin    <expr> (               <mode>                    )
 * typexpr    <expr> (               <mode>                    )
 *
 * constant   <expr> (<untyped kind> <mode>                    )
 * constant   <expr> (               <mode>       of type <typ>)
 * constant   <expr> (<untyped kind> <mode> <val>              )
 * constant   <expr> (               <mode> <val> of type <typ>)
 *
 * variable   <expr> (<untyped kind> <mode>                    )
 * variable   <expr> (               <mode>       of type <typ>)
 *
 * mapindex   <expr> (<untyped kind> <mode>                    )
 * mapindex   <expr> (               <mode>       of type <typ>)
 *
 * value      <expr> (<untyped kind> <mode>                    )
 * value      <expr> (               <mode>       of type <typ>)
 *
 * commaok    <expr> (<untyped kind> <mode>                    )
 * commaok    <expr> (               <mode>       of type <typ>)
 *
 * commaerr   <expr> (<untyped kind> <mode>                    )
 * commaerr   <expr> (               <mode>       of type <typ>)
 *
 * cgofunc    <expr> (<untyped kind> <mode>                    )
 * cgofunc    <expr> (               <mode>       of type <typ>) */
Str burrow__tc_operand_string(Alloc *a, const GtOperand *x, TypesQualifier qf) {
    /* special-case nil */
    if (x->mode == GT_MODE_VALUE && x->typ == GT_TYP(TYPES_UNTYPED_NIL))
        return BURROW_S("nil");

    BytesBuffer buf = BYTES_BUFFER(a);

    Str expr = BURROW_STR_EMPTY;
    if (x->expr != NULL) {
        expr = types_expr_string(a, x->expr);
    } else {
        switch ((Int)x->mode) {
        case GT_MODE_BUILTIN:
            expr = burrow__types_predeclared_funcs[x->id].name;
            break;
        case GT_MODE_TYPEXPR:
            expr = types_type_string(a, x->typ, qf);
            break;
        case GT_MODE_CONSTANT:
            expr = constant_value_string(x->val, a);
            break;
        default:
            break;
        }
    }

    /* <expr> ( */
    if (expr.len > 0) {
        burrow__types_buf_str(&buf, expr);
        burrow__types_buf_str(&buf, BURROW_S(" ("));
    }

    /* <untyped kind> */
    bool has_type = false;
    switch ((Int)x->mode) {
    case GT_MODE_INVALID:
    case GT_MODE_NOVALUE:
    case GT_MODE_BUILTIN:
    case GT_MODE_TYPEXPR:
        /* no type */
        break;
    default:
        /* should have a type, but be cautious (don't crash during printing) */
        if (x->typ != NULL) {
            if (!burrow__types_is_typed(x->typ)) {
                burrow__types_buf_str(&buf, types_as_basic(x->typ)->name);
                burrow__types_buf_byte(&buf, ' ');
                break;
            }
            has_type = true;
        }
    }

    /* <mode> */
    burrow__types_buf_str(&buf, gt_operand_mode_string[x->mode]);

    /* <val> */
    if (x->mode == GT_MODE_CONSTANT) {
        Str s = constant_value_string(x->val, a);
        if (!str_eq(s, expr)) {
            burrow__types_buf_byte(&buf, ' ');
            burrow__types_buf_str(&buf, s);
        }
    }

    /* <typ> */
    if (has_type) {
        if (burrow__types_is_valid(x->typ)) {
            Str desc = BURROW_STR_EMPTY;
            if (burrow__types_is_generic(x->typ))
                desc = BURROW_S("generic ");

            /* Describe the type structure if it is an *Alias or *Named type.
             * If the type is a renamed basic type, describe the basic type, as
             * in "int32 type MyInt" for a *Named type MyInt. If it is a type
             * parameter, describe the constraint instead. */
            TypesTypeParam *tpar = types_as_type_param(types_unalias(x->typ));
            Str what = BURROW_STR_EMPTY;
            if (tpar == NULL && (x->typ->kind == TYPES_KIND_ALIAS ||
                                 x->typ->kind == TYPES_KIND_NAMED)) {
                what = burrow__tc_composite_kind(x->typ);
                if (what.len == 0) {
                    /* x.typ must be basic type */
                    what = types_as_basic(types_type_underlying(x->typ))->name;
                }
            }
            /* desc is "" or has a trailing space at the end */

            burrow__types_buf_str(&buf, BURROW_S(" of "));
            burrow__types_buf_str(&buf, desc);
            if (what.len > 0) {
                burrow__types_buf_str(&buf, what);
                burrow__types_buf_byte(&buf, ' ');
            }
            burrow__types_buf_str(&buf, BURROW_S("type "));
            types_write_type(&buf, x->typ, qf);

            if (tpar != NULL) {
                burrow__types_buf_str(&buf, BURROW_S(" constrained by "));
                /* do not compute interface type sets here */
                types_write_type(&buf, tpar->bound, qf);
                /* If we have the type set and it's empty, say so for better
                 * error messages. */
                if (burrow__types_has_empty_typeset(types_type(tpar)))
                    burrow__types_buf_str(&buf, BURROW_S(" with empty type set"));
            }
        } else {
            burrow__types_buf_str(&buf, BURROW_S(" with invalid type"));
        }
    }

    /* ) */
    if (expr.len > 0)
        burrow__types_buf_byte(&buf, ')');

    return burrow__types_buf_done(&buf, a);
}

/* compositeKind returns the kind of the given composite type ("array",
 * "slice", etc.) or the empty string if typ is not composite but a basic
 * type. */
Str burrow__tc_composite_kind(TypesType typ) {
    TypesType u = types_type_underlying(typ);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        return BURROW_STR_EMPTY;
    case TYPES_KIND_ARRAY:
        return BURROW_S("array");
    case TYPES_KIND_SLICE:
        return BURROW_S("slice");
    case TYPES_KIND_STRUCT:
        return BURROW_S("struct");
    case TYPES_KIND_POINTER:
        return BURROW_S("pointer");
    case TYPES_KIND_SIGNATURE:
        return BURROW_S("func");
    case TYPES_KIND_INTERFACE:
        return BURROW_S("interface");
    case TYPES_KIND_MAP:
        return BURROW_S("map");
    case TYPES_KIND_CHAN:
        return BURROW_S("chan");
    case TYPES_KIND_TUPLE:
        return BURROW_S("tuple");
    case TYPES_KIND_UNION:
        return BURROW_S("union");
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* setConst sets x to the untyped constant for literal lit. */
void burrow__tc_op_set_const(Alloc *a, GtOperand *x, Token k, Str lit) {
    TypesBasicKind kind;
    switch ((Int)k) {
    case TOKEN_INT:
        kind = TYPES_UNTYPED_INT;
        break;
    case TOKEN_FLOAT:
        kind = TYPES_UNTYPED_FLOAT;
        break;
    case TOKEN_IMAG:
        kind = TYPES_UNTYPED_COMPLEX;
        break;
    case TOKEN_CHAR:
        kind = TYPES_UNTYPED_RUNE;
        break;
    case TOKEN_STRING:
        kind = TYPES_UNTYPED_STRING;
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }

    ConstantValue val = constant_make_from_literal(a, lit, k, 0);
    if (constant_value_kind(val) == CONSTANT_UNKNOWN) {
        burrow__tc_op_invalidate(x);
        x->typ = GT_TYP(TYPES_INVALID);
        return;
    }
    x->mode = GT_MODE_CONSTANT;
    x->typ = GT_TYP(kind);
    x->val = val;
}

/* isNil reports whether x is the (untyped) nil value. */
bool burrow__tc_op_is_nil(const GtOperand *x) {
    return x->mode == GT_MODE_VALUE && x->typ == GT_TYP(TYPES_UNTYPED_NIL);
}
