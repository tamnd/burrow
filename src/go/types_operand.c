/* go/types: operands, and AssignableTo and ConvertibleTo.
 *
 * Derived from Go's src/go/types/api_predicates.go and operand.go. Go source:
 * go1.27.1.
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

/* -------------------------------------------------------------- assignableTo */

/* What the walks over a type set in assignableTo close over. */
typedef struct TopAssign {
    TypesChecker *check;
    GtOperand x; /* a copy, so the outer x is not clobbered */
    TypesType T;
    TypesType orig_T;
    TypesTypeParam *p; /* the type parameter whose terms are walked */
    Str *cause;
    bool ok;
    GtCode code;
} TopAssign;

/* x, untyped, can take the type of the term */
static bool top_term_implicit_ok(void *env, TypesTerm *t) {
    if (t == NULL)
        return false;
    TopAssign *c = env;
    /* A term may be a tilde term but the underlying type of an untyped value
     * doesn't change so we don't need to do anything special. */
    TypesType typ;
    ConstantValue val;
    bool has_val;
    (void)burrow__tc_implicit_type_and_value(c->check, &c->x, t->typ, &typ, &val,
                                             &has_val);
    return typ != NULL;
}

/* x is assignable to the term's type */
static bool top_term_assignable_from(void *env, TypesTerm *t) {
    if (t == NULL)
        return false; /* no specific types */
    TopAssign *c = env;
    c->ok = burrow__tc_assignable_to(c->check, &c->x, t->typ, c->cause, &c->code);
    if (!c->ok) {
        if (c->check != NULL && c->cause != NULL) {
            TypesChecker *check = c->check;
            Str xs = burrow__tc_ts(check, c->x.typ), ts = burrow__tc_ts(check, t->typ);
            Str ps = burrow__tc_ts(check, types_type(c->p));
            burrow__tc_wrap_cause(
                check, c->cause,
                GT_SPRINTF(check, "cannot assign %s to %s (in %s)", xs, ts, ps));
        }
        return false;
    }
    return true;
}

/* values of the term's type are assignable to T */
static bool top_term_assignable_to(void *env, TypesTerm *t) {
    if (t == NULL)
        return false; /* no specific types */
    TopAssign *c = env;
    c->x.typ = t->typ;
    c->ok = burrow__tc_assignable_to(c->check, &c->x, c->T, c->cause, &c->code);
    if (!c->ok) {
        if (c->check != NULL && c->cause != NULL) {
            TypesChecker *check = c->check;
            Str vs = burrow__tc_ts(check, t->typ);
            Str ps = burrow__tc_ts(check, types_type(c->p));
            Str ts = burrow__tc_ts(check, c->orig_T);
            burrow__tc_wrap_cause(
                check, c->cause,
                GT_SPRINTF(check, "cannot assign %s (in %s) to %s", vs, ps, ts));
        }
        return false;
    }
    return true;
}

void burrow__tc_wrap_cause(TypesChecker *check, Str *cause, Str msg) {
    if (cause->len != 0)
        msg = GT_SPRINTF(check, "%s\n\t%s", msg, *cause);
    *cause = msg;
}

bool burrow__tc_assignable_to(TypesChecker *check, const GtOperand *x, TypesType T,
                              Str *cause, GtCode *code) {
    /* Not in Go: without a checker there is nothing to make a cause with. */
    burrow__types_assert(check != NULL || cause == NULL);
    GtCode ignored;
    if (code == NULL)
        code = &ignored;
    *code = GT_CODE_NONE;
    if (!burrow__tc_op_valid(x) || !burrow__types_is_valid(T))
        return true; /* avoid spurious errors */

    TypesType orig_T = T;
    TypesType V = types_unalias(x->typ);
    T = types_unalias(T);

    /* x's type is identical to T */
    if (types_identical(V, T))
        return true;

    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    TypesTypeParam *Vp = types_as_type_param(V);
    TypesTypeParam *Tp = types_as_type_param(T);
    TopAssign c = {check, *x,    T,     orig_T,
                   NULL,  cause, false, GT_CODE_INCOMPATIBLE_ASSIGN};

    /* x is an untyped value representable by a value of type T. */
    if (!burrow__types_is_typed(Vu)) {
        burrow__types_assert(Vp == NULL);
        *code = GT_CODE_INCOMPATIBLE_ASSIGN;
        if (Tp != NULL)
            /* T is a type parameter: x is assignable to T if it is
             * representable by each specific type in the type set of T. */
            return burrow__types_type_param_is(
                Tp, BURROW_FN(GtTermFunc, top_term_implicit_ok, &c));
        TypesType typ;
        ConstantValue val;
        bool has_val;
        (void)burrow__tc_implicit_type_and_value(check, x, T, &typ, &val, &has_val);
        return typ != NULL;
    }
    /* Vu is typed */

    /* x's type V and T have identical underlying types and at least one of V
     * or T is not a named type and neither V nor T is a type parameter. */
    if (types_identical(Vu, Tu) &&
        (!burrow__types_has_name(V) || !burrow__types_has_name(T)) && Vp == NULL &&
        Tp == NULL)
        return true;

    /* T is an interface type, but not a type parameter, and V implements T.
     * Also handle the case where T is a pointer to an interface so that we get
     * the Checker.implements error cause. */
    if ((types_as_interface(Tu) != NULL && Tp == NULL) ||
        burrow__types_is_interface_ptr(Tu)) {
        if (burrow__types_implements(check, NULL, V, T, false, cause))
            return true;
        /* V doesn't implement T but V may still be assignable to T if V is a
         * type parameter; do not report an error in that case yet. */
        if (Vp == NULL) {
            *code = GT_CODE_INVALID_IFACE_ASSIGN;
            return false;
        }
        if (cause != NULL)
            *cause = BURROW_STR_EMPTY;
    }

    /* If V is an interface, check if a missing type assertion is the
     * problem. */
    if (types_as_interface(Vu) != NULL && Vp == NULL) {
        if (burrow__types_implements(check, NULL, T, V, false, NULL)) {
            /* T implements V, so give hint about type assertion. */
            if (cause != NULL)
                *cause = BURROW_S("need type assertion");
            *code = GT_CODE_INCOMPATIBLE_ASSIGN;
            return false;
        }
    }

    /* x is a bidirectional channel value, T is a channel type, x's type V and
     * T have identical element types, and at least one of V or T is not a
     * named type. */
    TypesChan *Vc = types_as_chan(Vu);
    if (Vc != NULL && types_chan_dir(Vc) == TYPES_SEND_RECV) {
        TypesChan *Tc = types_as_chan(Tu);
        if (Tc != NULL && types_identical(types_chan_elem(Vc), types_chan_elem(Tc))) {
            *code = GT_CODE_INVALID_CHAN_ASSIGN;
            return !burrow__types_has_name(V) || !burrow__types_has_name(T);
        }
    }

    /* optimization: if we don't have type parameters, we're done */
    *code = GT_CODE_INCOMPATIBLE_ASSIGN;
    if (Vp == NULL && Tp == NULL)
        return false;

    /* x's type V is not a named type and T is a type parameter, and x is
     * assignable to each specific type in T's type set. */
    if (!burrow__types_has_name(V) && Tp != NULL) {
        c.p = Tp;
        (void)burrow__types_type_param_is(
            Tp, BURROW_FN(GtTermFunc, top_term_assignable_from, &c));
        *code = c.code;
        return c.ok;
    }

    /* x's type V is a type parameter and T is not a named type, and values x'
     * of each specific type in V's type set are assignable to T. */
    if (Vp != NULL && !burrow__types_has_name(T)) {
        c.p = Vp;
        (void)burrow__types_type_param_is(
            Vp, BURROW_FN(GtTermFunc, top_term_assignable_to, &c));
        *code = c.code;
        return c.ok;
    }

    return false;
}

/* ------------------------------------------------------------------- the API */

/* AssignableTo reports whether a value of type V is assignable to a variable
 * of type T. */
bool types_assignable_to(TypesType V, TypesType T) {
    GtOperand x = {.mode = GT_MODE_VALUE, .typ = V};
    /* check not needed for non-constant x */
    return burrow__tc_assignable_to(NULL, &x, T, NULL, NULL);
}

/* ConvertibleTo reports whether a value of type V is convertible to a value
 * of type T. */
bool types_convertible_to(TypesType V, TypesType T) {
    GtOperand x = {.mode = GT_MODE_VALUE, .typ = V};
    /* check not needed for non-constant x */
    return burrow__tc_convertible_to(NULL, &x, T, NULL);
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
                    what = GT_MUST(TypesBasic, TYPES_KIND_BASIC,
                                   types_type_underlying(x->typ))
                               ->name;
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
