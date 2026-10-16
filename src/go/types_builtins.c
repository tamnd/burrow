/* go/types: typechecking of builtin function calls.
 *
 * Derived from Go's src/go/types/builtins.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

#define TBI_MODE_BIT(m) (1u << (unsigned)(m))

/* Go's check.recordTypes. */
static bool tbi_record_types(const TypesChecker *check) {
    return check->info->types != NULL;
}

/* makeSig makes a signature for the given argument and result types. Default
 * types are used for untyped arguments, and res may be NULL. args is of
 * TypesType. */
static TypesSignature *tbi_make_sig_list(Alloc *a, TypesType res, Slice args) {
    Slice list = burrow__types_ptrs_make(a, args.len);
    for (Int i = 0; i < args.len; i++) {
        TypesType param = GT_AT(args, i, TypesType);
        GT_AT(list, i, TypesVar *) = types_new_param(
            a, TOKEN_NO_POS, NULL, BURROW_STR_EMPTY, types_default(param));
    }
    TypesSignature *sig = GT_NEW(a, TypesSignature);
    sig->hdr.kind = TYPES_KIND_SIGNATURE;
    sig->params = types_new_tuple(a, list);
    if (res != NULL) {
        burrow__types_assert(burrow__types_is_typed(res));
        Slice result = burrow__types_ptrs_make(a, 1);
        GT_AT(result, 0, TypesVar *) = burrow__types_new_var(
            a, TYPES_RESULT_VAR, TOKEN_NO_POS, NULL, BURROW_STR_EMPTY, res);
        sig->results = types_new_tuple(a, result);
    }
    return sig;
}

/* makeSig with the n argument types in args. */
static TypesSignature *tbi_make_sig(Alloc *a, TypesType res, Int n,
                                    const TypesType *args) {
    Slice list = burrow__types_ptrs_make(a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(list, i, TypesType) = args[i];
    return tbi_make_sig_list(a, res, list);
}

/* arrayPtrDeref returns A if typ is of the form *A and A is an array;
 * otherwise it returns typ. */
TypesType burrow__tc_array_ptr_deref(TypesType typ) {
    if (typ == NULL)
        return NULL;
    TypesPointer *p = types_as_pointer(types_unalias(typ));
    if (p != NULL) {
        TypesArray *a = types_as_array(types_type_underlying(p->base));
        if (a != NULL)
            return types_type(a);
    }
    return typ;
}

/* What the walk of sliceElem over the type set of x closes over. */
typedef struct TbiElem {
    TypesChecker *check;
    const GtOperand *x;
    TypesType elem;
    Str err; /* the type error, set when the walk stops */
} TbiElem;

static bool tbi_slice_elem_of(void *env, TypesType t, TypesType u) {
    (void)t;
    TbiElem *c = env;
    TypesSlice *s = types_as_slice(u);
    if (s == NULL) {
        if (burrow__tc_op_is_nil(c->x)) {
            /* Printing x in this case would just print "nil". Special case
             * this so we can emphasize "untyped". */
            c->err = BURROW_S("argument must be a slice; have untyped nil");
        } else {
            Str xs = burrow__tc_xs(c->check, c->x);
            c->err = GT_SPRINTF(c->check, "argument must be a slice; have %s", xs);
        }
        return false;
    }
    if (c->elem == NULL) {
        c->elem = s->elem;
    } else if (!types_identical(c->elem, s->elem)) {
        Str e = burrow__tc_ts(c->check, c->elem);
        Str se = burrow__tc_ts(c->check, s->elem);
        Str xs = burrow__tc_xs(c->check, c->x);
        c->err = GT_SPRINTF(c->check, "mismatched slice element types %s and %s in %s",
                            e, se, xs);
        return false;
    }
    return true;
}

/* sliceElem returns the slice element type for a slice operand x, or NULL with
 * *err set to the type error if x is not a slice (or a type set of slices). */
static TypesType tbi_slice_elem(TypesChecker *check, const GtOperand *x, Str *err) {
    TbiElem c = {check, x, NULL, BURROW_STR_EMPTY};
    if (!burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tbi_slice_elem_of, &c))) {
        *err = c.err;
        return NULL;
    }
    return c.elem;
}

/* What the walk over a type set of []byte and string types closes over. */
typedef struct TbiBytes {
    bool ok;         /* each type is []byte or a string */
    bool has_string; /* one of them is a string */
} TbiBytes;

static bool tbi_bytes_or_string(void *env, TypesType t, TypesType u) {
    (void)t;
    TbiBytes *c = env;
    TypesSlice *s = types_as_slice(u);
    if (s != NULL &&
        types_identical(s->elem, types_type(burrow__types_universe_byte))) {
        /* the type set contains []byte */
        return true;
    }
    if (u != NULL && burrow__types_is_basic(u, TYPES_IS_STRING)) {
        /* the type set contains string */
        c->has_string = true;
        return true;
    }
    c->ok = false;
    return false;
}

/* hasVarSize reports if the size of type t is variable due to type parameters
 * or if the type is infinitely-sized due to a cycle for which the type has not
 * yet been checked. */
static bool tbi_has_var_size(TypesChecker *check, TypesType t) {
    /* Note: We could use Underlying here, but passing through the RHS may yield
     * better error messages and allows us to stash the result on each traversed
     * Named type. */
    t = types_unalias(t);
    switch (t != NULL ? t->kind : TYPES_KIND_INVALID) {
    case TYPES_KIND_NAMED: {
        TypesNamed *n = (TypesNamed *)(void *)t;
        if (burrow__types_named_state_has(n, GT_HAS_VAR_SIZE))
            return n->var_size;

        TypesObject obj = types_object(n->obj);
        const Int *i =
            check->obj_path_idx != NULL ? map_get(check->obj_path_idx, &obj) : NULL;
        if (i != NULL) {
            Slice cycle = slice_sub(check->obj_path, *i, check->obj_path.len);
            burrow__tc_cycle_error(check, cycle, burrow__tc_first_in_src(cycle));
            return true;
        }

        burrow__tc_push(check, obj);

        /* Careful, we're inspecting t.fromRHS, so we need to unpack first. */
        (void)burrow__types_named_unpack(n);
        bool var_size = tbi_has_var_size(check, burrow__types_named_rhs(n));

        /* Special case for portable simd types that rewrite to unknown
         * sizes. */
        TypesPackage *pkg = GT_OBJ(obj)->pkg;
        if (pkg != NULL && str_eq(types_package_path(pkg), BURROW_S("simd")) &&
            str_eq(GT_OBJ(obj)->name, BURROW_S("_simd")))
            var_size = true;

        /* Careful, t.varSize has lock-free readers. Since we might be racing
         * another call to hasVarSize, we have to avoid overwriting
         * t.varSize. */
        sync_mutex_lock(&n->mu);
        if (!burrow__types_named_state_has(n, GT_HAS_VAR_SIZE)) {
            n->var_size = var_size;
            (void)sync_atomic_uint32_or(&n->state, GT_HAS_VAR_SIZE);
        }
        sync_mutex_unlock(&n->mu);

        burrow__tc_pop(check);
        return var_size;
    }

    case TYPES_KIND_ARRAY:
        /* The array length is already computed. If it was a valid length, it
         * is constant; else, an error was reported in the computation. */
        return tbi_has_var_size(check, ((TypesArray *)(void *)t)->elem);

    case TYPES_KIND_STRUCT: {
        Slice fields = ((TypesStruct *)(void *)t)->fields;
        for (Int i = 0; i < fields.len; i++) {
            if (tbi_has_var_size(check, GT_OBJ(GT_AT(fields, i, TypesVar *))->typ))
                return true;
        }
        break;
    }

    case TYPES_KIND_TYPE_PARAM:
        return true;

    default:
        break;
    }
    return false;
}

/* The type functions applyTypeFunc applies. */
typedef TypesType (*TbiTypeFunc)(TypesType typ);

/* What the walk of applyTypeFunc over the terms of a type parameter closes
 * over. */
typedef struct TbiApply {
    Alloc *a;
    TbiTypeFunc f;
    Slice terms; /* of TypesTerm * */
} TbiApply;

static bool tbi_apply_term(void *env, TypesTerm *t) {
    TbiApply *c = env;
    if (t == NULL)
        return false;
    TypesType r = c->f(t->typ);
    if (r == NULL)
        return false;
    burrow__types_ptrs_append(c->a, &c->terms, types_new_term(c->a, t->tilde, r));
    return true;
}

/* applyTypeFunc applies f to x. If x is a type parameter, the result is a type
 * parameter constrained by a new interface bound. The type bounds for that
 * interface are computed by applying f to each of the type bounds of x. If any
 * of these applications of f return NULL, applyTypeFunc returns NULL. If x is
 * not a type parameter, the result is f(x). */
static TypesType tbi_apply_type_func(TypesChecker *check, TbiTypeFunc f,
                                     const GtOperand *x, Int id) {
    TypesTypeParam *tp = types_as_type_param(types_unalias(x->typ));
    if (tp == NULL)
        return f(x->typ);

    /* Test if t satisfies the requirements for the argument type and collect
     * possible result types at the same time. */
    TbiApply c = {check->a, f, slice_nil(TYPE_UNSAFE_POINTER)};
    if (!burrow__types_type_param_is(tp, BURROW_FN(GtTermFunc, tbi_apply_term, &c)))
        return NULL;

    /* We can type-check this fine but we're introducing a synthetic type
     * parameter for the result. It's not clear what the API implications are
     * here. Report an error for 1.18 (see go.dev/issue/50912), but continue
     * type-checking. */
    GtCode code;
    switch (id) {
    case GT_REAL:
        code = GT_CODE_INVALID_REAL;
        break;
    case GT_IMAG:
        code = GT_CODE_INVALID_IMAG;
        break;
    case GT_COMPLEX:
        code = GT_CODE_INVALID_COMPLEX;
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    Str xs = burrow__tc_xs(check, x);
    Str name = burrow__types_predeclared_funcs[id].name;
    GT_SOFT_ERRORF(check, burrow__tc_at_op(x), code,
                   "%s not supported as argument to built-in %s for go1.18 (see "
                   "go.dev/issue/50937)",
                   xs, name);

    /* Construct a suitable new type parameter for the result type. The type
     * parameter is placed in the current package so export/import works as
     * expected. */
    TypesTypeName *tpar = types_new_type_name(check->a, TOKEN_NO_POS, check->pkg,
                                              GT_OBJ(tp->obj)->name, NULL);
    Slice embeddeds = burrow__types_ptrs_make(check->a, 1);
    GT_AT(embeddeds, 0, TypesType) = types_type(types_new_union(check->a, c.terms));
    TypesInterface *bound =
        types_new_interface_type(check->a, slice_nil(TYPE_UNSAFE_POINTER), embeddeds);
    /* assigns type to tpar as a side-effect */
    TypesTypeParam *ptyp = burrow__tc_new_type_param(check, tpar, types_type(bound));
    ptyp->index = tp->index;
    return types_type(ptyp);
}

/* The type function of complex: the complex type for a floating-point type. */
static TypesType tbi_complex_of(TypesType typ) {
    burrow__types_assert(!burrow__types_is_type_param(typ));
    TypesBasic *t = types_as_basic(types_type_underlying(typ));
    if (t != NULL) {
        switch (t->kind) {
        case TYPES_FLOAT32:
            return GT_TYP(TYPES_COMPLEX64);
        case TYPES_FLOAT64:
            return GT_TYP(TYPES_COMPLEX128);
        case TYPES_UNTYPED_FLOAT:
            return GT_TYP(TYPES_UNTYPED_COMPLEX);
        default:
            break;
        }
    }
    return NULL;
}

/* The type function of real and imag: the floating-point type for a complex
 * type. */
static TypesType tbi_float_of(TypesType typ) {
    burrow__types_assert(!burrow__types_is_type_param(typ));
    TypesBasic *t = types_as_basic(types_type_underlying(typ));
    if (t != NULL) {
        switch (t->kind) {
        case TYPES_COMPLEX64:
            return GT_TYP(TYPES_FLOAT32);
        case TYPES_COMPLEX128:
            return GT_TYP(TYPES_FLOAT64);
        case TYPES_UNTYPED_COMPLEX:
            return GT_TYP(TYPES_UNTYPED_FLOAT);
        default:
            break;
        }
    }
    return NULL;
}

/* complex's toFloat. */
static void tbi_to_float(GtOperand *x) {
    if (burrow__types_is_basic(x->typ, TYPES_IS_NUMERIC) &&
        constant_sign(constant_imag(x->val)) == 0)
        x->typ = GT_TYP(TYPES_UNTYPED_FLOAT);
}

/* The underIs walk of len and cap over the type set of a type parameter. */
static bool tbi_len_cap_ok(void *env, TypesType t, TypesType u) {
    (void)t;
    Int id = *(const Int *)env;
    TypesType d = burrow__tc_array_ptr_deref(u);
    switch (d != NULL ? d->kind : TYPES_KIND_INVALID) {
    case TYPES_KIND_BASIC:
        return burrow__types_is_basic(d, TYPES_IS_STRING) && id == GT_LEN;
    case TYPES_KIND_ARRAY:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_CHAN:
        return true;
    case TYPES_KIND_MAP:
        return id == GT_LEN;
    default:
        return false;
    }
}

/* What the underIs walks of clear, close and delete close over. */
typedef struct TbiOp {
    TypesChecker *check;
    const GtOperand *x;
    TypesType key; /* delete: the key type of the maps so far */
} TbiOp;

static bool tbi_clearable(void *env, TypesType t, TypesType u) {
    (void)t;
    const TbiOp *c = env;
    Int kind = u != NULL ? u->kind : TYPES_KIND_INVALID;
    if (kind == TYPES_KIND_MAP || kind == TYPES_KIND_SLICE)
        return true;
    Str xs = burrow__tc_xs(c->check, c->x);
    GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_CLEAR,
              GT_INVALID_ARG
              "cannot clear %s: argument must be (or constrained by) map or slice",
              xs);
    return false;
}

static bool tbi_closable(void *env, TypesType t, TypesType u) {
    (void)t;
    const TbiOp *c = env;
    TypesChan *uch = types_as_chan(u);
    if (uch == NULL) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_CLOSE,
                  GT_INVALID_OP "cannot close non-channel %s", xs);
        return false;
    }
    if (uch->dir == TYPES_RECV_ONLY) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_CLOSE,
                  GT_INVALID_OP "cannot close receive-only channel %s", xs);
        return false;
    }
    return true;
}

static bool tbi_deletable(void *env, TypesType t, TypesType u) {
    (void)t;
    TbiOp *c = env;
    TypesMap *m = types_as_map(u);
    if (m == NULL) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_DELETE,
                  GT_INVALID_ARG "%s is not a map", xs);
        return false;
    }
    if (c->key != NULL && !types_identical(m->key, c->key)) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_DELETE,
                  GT_INVALID_ARG "maps of %s must have identical key types", xs);
        return false;
    }
    c->key = m->key;
    return true;
}

/* The commonUnder condition of make. */
static bool tbi_makeable(void *env, TypesType t, TypesType u, Str *cause) {
    (void)env;
    (void)t;
    switch (u != NULL ? u->kind : TYPES_KIND_INVALID) {
    case TYPES_KIND_SLICE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        return true; /* ok */
    default:
        break;
    }
    if (u == NULL)
        *cause = BURROW_S("no specific type");
    else
        *cause = BURROW_S("type must be slice, map, or channel");
    return false;
}

/* Go's check.dump, which is only needed for debugging. */
static void tbi_dump(Str msg) {
    (void)fmt_println_v(msg);
}

static bool tbi_append(TypesChecker *check, GtOperand *x, AstCallExpr *call,
                       Slice args) {
    /* append(s S, x ...E) S, where E is the element type of S
     * spec: "The variadic function append appends zero or more values x to a
     * slice s of type S and returns the resulting slice, also of type S. The
     * values x are passed to a parameter of type ...E where E is the element
     * type of S and the respective parameter passing rules apply. As a special
     * case, append also accepts a first argument assignable to type []byte
     * with a second argument of string type followed by ... . This form
     * appends the bytes of the string."
     *
     * In either case, the first argument must be a slice; in particular it
     * cannot be the predeclared nil value. Note that nil is not excluded by
     * the assignability requirement alone for the special case
     * (go.dev/issue/76220). spec: "If S is a type parameter, all types in its
     * type set must have the same underlying slice type []E." */
    Alloc *a = check->a;
    Str cause = BURROW_STR_EMPTY;
    TypesType E = tbi_slice_elem(check, x, &cause);
    if (E == NULL) {
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_APPEND,
                  "invalid append: %s", cause);
        return false;
    }

    /* Handle append(bytes, y...) special case, where the type set of y is
     * {string} or {string, []byte}. */
    TypesSignature *sig = NULL;
    if (args.len == 2 && burrow__tc_has_dots(call)) {
        TypesType bytes =
            types_type(types_new_slice(a, types_type(burrow__types_universe_byte)));
        if (burrow__tc_assignable_to(check, x, bytes, NULL, NULL)) {
            GtOperand *y = GT_AT(args, 1, GtOperand *);
            TbiBytes c = {true, false};
            (void)burrow__types_all(y->typ,
                                    BURROW_FN(GtTypePairFunc, tbi_bytes_or_string, &c));
            if (c.ok && c.has_string) {
                /* setting the signature also signals that we're done */
                sig = tbi_make_sig(a, x->typ, 2, (const TypesType[]){x->typ, y->typ});
                sig->variadic = true;
            }
        }
    }

    /* general case */
    if (sig == NULL) {
        /* check arguments by creating custom signature */
        TypesType es = types_type(types_new_slice(a, E)); /* []E required for
                                                             variadic signature */
        sig = tbi_make_sig(a, x->typ, 2, (const TypesType[]){x->typ, es});
        sig->variadic = true;
        Slice none = slice_nil(TYPE_UNSAFE_POINTER);
        /* discard result (we know the result type) */
        (void)burrow__tc_arguments(check, call, sig, none, none, args, none);
        /* ok to continue even if check.arguments reported errors */
    }

    if (tbi_record_types(check))
        burrow__tc_record_builtin_type(check, call->fun, sig);
    x->mode = GT_MODE_VALUE;
    /* x.typ is unchanged */
    return true;
}

static bool tbi_len_cap(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id) {
    /* cap(x)
     * len(x) */
    GtMode mode = GT_MODE_INVALID;
    ConstantValue val = {0};
    TypesType t = burrow__tc_array_ptr_deref(types_type_underlying(x->typ));
    switch (t != NULL ? t->kind : TYPES_KIND_INVALID) {
    case TYPES_KIND_BASIC:
        if (burrow__types_is_basic(t, TYPES_IS_STRING) && id == GT_LEN) {
            if (x->mode == GT_MODE_CONSTANT) {
                mode = GT_MODE_CONSTANT;
                val = constant_make_int64(constant_string_len(x->val));
            } else {
                mode = GT_MODE_VALUE;
            }
        }
        break;

    case TYPES_KIND_ARRAY: {
        mode = GT_MODE_VALUE;
        /* spec: "The expressions len(s) and cap(s) are constants if the type
         * of s is an array or pointer to an array and the expression s does
         * not contain channel receives or function calls; in this case s is
         * not evaluated." */
        if (!check->env.has_call_or_recv) {
            mode = GT_MODE_CONSTANT;
            int64_t len = ((TypesArray *)(void *)t)->len;
            if (len >= 0)
                val = constant_make_int64(len);
            else
                val = constant_make_unknown();
        }
        break;
    }

    case TYPES_KIND_SLICE:
    case TYPES_KIND_CHAN:
        mode = GT_MODE_VALUE;
        break;

    case TYPES_KIND_MAP:
        if (id == GT_LEN)
            mode = GT_MODE_VALUE;
        break;

    case TYPES_KIND_INTERFACE:
        if (!burrow__types_is_type_param(x->typ))
            break;
        if (burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tbi_len_cap_ok, &id)))
            mode = GT_MODE_VALUE;
        break;

    default:
        break;
    }

    if (mode == GT_MODE_INVALID) {
        /* avoid error if underlying type is invalid */
        if (burrow__types_is_valid(types_type_underlying(x->typ))) {
            GtCode code = id == GT_LEN ? GT_CODE_INVALID_LEN : GT_CODE_INVALID_CAP;
            Str xs = burrow__tc_xs(check, x);
            Str name = burrow__types_predeclared_funcs[id].name;
            GT_ERRORF(check, burrow__tc_at_op(x), code,
                      GT_INVALID_ARG "%s for built-in %s", xs, name);
        }
        return false;
    }

    /* record the signature before changing x.typ */
    if (tbi_record_types(check) && mode != GT_MODE_CONSTANT) {
        burrow__tc_record_builtin_type(
            check, call->fun,
            tbi_make_sig(check->a, GT_TYP(TYPES_INT), 1, (const TypesType[]){x->typ}));
    }

    x->mode = mode;
    x->typ = GT_TYP(TYPES_INT);
    x->val = val;
    return true;
}

static bool tbi_complex(TypesChecker *check, GtOperand *x, AstCallExpr *call,
                        GtOperand *y) {
    /* complex(x, y floatT) complexT */
    Alloc *a = check->a;

    /* convert or check untyped arguments */
    int d = 0;
    if (!burrow__types_is_typed(x->typ))
        d |= 1;
    if (!burrow__types_is_typed(y->typ))
        d |= 2;
    switch (d) {
    case 1:
        /* only x is untyped => convert to type of y */
        burrow__tc_convert_untyped(check, x, y->typ);
        break;
    case 2:
        /* only y is untyped => convert to type of x */
        burrow__tc_convert_untyped(check, y, x->typ);
        break;
    case 3:
        /* x and y are untyped =>
         * 1) if both are constants, convert them to untyped floating-point
         *    numbers if possible,
         * 2) if one of them is not constant (possible because it contains a
         *    shift that is yet untyped), convert both of them to float64 since
         *    they must have the same type to succeed (this will result in an
         *    error because shifts of floats are not permitted) */
        if (x->mode == GT_MODE_CONSTANT && y->mode == GT_MODE_CONSTANT) {
            tbi_to_float(x);
            tbi_to_float(y);
        } else {
            burrow__tc_convert_untyped(check, x, GT_TYP(TYPES_FLOAT64));
            burrow__tc_convert_untyped(check, y, GT_TYP(TYPES_FLOAT64));
            /* x and y should be invalid now, but be conservative and check
             * below */
        }
        break;
    default:
        /* x and y are typed => nothing to do */
        break;
    }
    if (!burrow__tc_op_valid(x) || !burrow__tc_op_valid(y))
        return false;

    /* both argument types must be identical */
    if (!types_identical(x->typ, y->typ)) {
        Str cs = burrow__tc_es(check, GT_NODE(call));
        Str xt = burrow__tc_ts(check, x->typ);
        Str yt = burrow__tc_ts(check, y->typ);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_COMPLEX,
                  GT_INVALID_OP "%s (mismatched types %s and %s)", cs, xt, yt);
        return false;
    }

    /* the argument types must be of floating-point type (applyTypeFunc never
     * calls f with a type parameter) */
    TypesType res_typ = tbi_apply_type_func(check, tbi_complex_of, x, GT_COMPLEX);
    if (res_typ == NULL) {
        Str xt = burrow__tc_ts(check, x->typ);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_COMPLEX,
                  GT_INVALID_ARG "arguments have type %s, expected floating-point", xt);
        return false;
    }

    /* if both arguments are constants, the result is a constant */
    if (x->mode == GT_MODE_CONSTANT && y->mode == GT_MODE_CONSTANT) {
        x->val =
            constant_binary_op(a, constant_to_float(a, x->val), TOKEN_ADD,
                               constant_make_imag(a, constant_to_float(a, y->val)));
    } else {
        x->mode = GT_MODE_VALUE;
    }

    if (tbi_record_types(check) && x->mode != GT_MODE_CONSTANT) {
        burrow__tc_record_builtin_type(
            check, call->fun,
            tbi_make_sig(a, res_typ, 2, (const TypesType[]){x->typ, x->typ}));
    }

    x->typ = res_typ;
    return true;
}

static bool tbi_copy(TypesChecker *check, GtOperand *x, AstCallExpr *call,
                     GtOperand *y) {
    /* copy(x, y []E) int
     * spec: "The function copy copies slice elements from a source src to a
     * destination dst and returns the number of elements copied. Both
     * arguments must have identical element type E and must be assignable to
     * a slice of type []E. The number of elements copied is the minimum of
     * len(src) and len(dst). As a special case, copy also accepts a
     * destination argument assignable to type []byte with a source argument
     * of a string type. This form copies the bytes from the string into the
     * byte slice."
     *
     * In either case, the first argument must be a slice; in particular it
     * cannot be the predeclared nil value. Note that nil is not excluded by
     * the assignability requirement alone for the special case
     * (go.dev/issue/79687). spec: "If the type of one or both arguments is a
     * type parameter, all types in their respective type sets must have the
     * same underlying slice type []E." */
    Alloc *a = check->a;
    Str cause = BURROW_STR_EMPTY;
    TypesType dst_e = tbi_slice_elem(check, x, &cause);
    if (dst_e == NULL) {
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_COPY, "invalid copy: %s",
                  cause);
        return false;
    }

    /* get special case out of the way */
    TypesType byte = types_type(burrow__types_universe_byte);
    bool special = false;
    if (burrow__tc_assignable_to(check, x, types_type(types_new_slice(a, byte)), NULL,
                                 NULL)) {
        TbiBytes c = {true, false};
        (void)burrow__types_all(y->typ,
                                BURROW_FN(GtTypePairFunc, tbi_bytes_or_string, &c));
        special = c.ok;
    }

    /* general case */
    if (!special) {
        TypesType src_e = tbi_slice_elem(check, y, &cause);
        if (src_e == NULL) {
            /* If we have a string, for a better error message proceed with
             * byte element type. */
            if (!burrow__types_all_basic(y->typ, TYPES_IS_STRING)) {
                GT_ERRORF(check, burrow__tc_at_op(y), GT_CODE_INVALID_COPY,
                          "invalid copy: %s", cause);
                return false;
            }
            src_e = byte;
        }
        if (!types_identical(dst_e, src_e)) {
            Str xs = burrow__tc_xs(check, x);
            Str ys = burrow__tc_xs(check, y);
            Str ds = burrow__tc_ts(check, dst_e);
            Str ss = burrow__tc_ts(check, src_e);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_COPY,
                      "invalid copy: arguments %s and %s have different element types "
                      "%s and %s",
                      xs, ys, ds, ss);
            return false;
        }
    }

    if (tbi_record_types(check)) {
        burrow__tc_record_builtin_type(
            check, call->fun,
            tbi_make_sig(a, GT_TYP(TYPES_INT), 2, (const TypesType[]){x->typ, y->typ}));
    }
    x->mode = GT_MODE_VALUE;
    x->typ = GT_TYP(TYPES_INT);
    return true;
}

static bool tbi_real_imag(TypesChecker *check, GtOperand *x, AstCallExpr *call,
                          Int id) {
    /* imag(complexT) floatT
     * real(complexT) floatT */

    /* convert or check untyped argument */
    if (!burrow__types_is_typed(x->typ)) {
        if (x->mode == GT_MODE_CONSTANT) {
            /* an untyped constant number can always be considered as a complex
             * constant */
            if (burrow__types_is_basic(x->typ, TYPES_IS_NUMERIC))
                x->typ = GT_TYP(TYPES_UNTYPED_COMPLEX);
        } else {
            /* an untyped non-constant argument may appear if it contains a (yet
             * untyped non-constant) shift expression: convert it to complex128
             * which will result in an error (shift of complex value) */
            burrow__tc_convert_untyped(check, x, GT_TYP(TYPES_COMPLEX128));
            /* x should be invalid now, but be conservative and check */
            if (!burrow__tc_op_valid(x))
                return false;
        }
    }

    /* the argument must be of complex type (applyTypeFunc never calls f with a
     * type parameter) */
    TypesType res_typ = tbi_apply_type_func(check, tbi_float_of, x, id);
    if (res_typ == NULL) {
        GtCode code = id == GT_REAL ? GT_CODE_INVALID_REAL : GT_CODE_INVALID_IMAG;
        Str xt = burrow__tc_ts(check, x->typ);
        GT_ERRORF(check, burrow__tc_at_op(x), code,
                  GT_INVALID_ARG "argument has type %s, expected complex type", xt);
        return false;
    }

    /* if the argument is a constant, the result is a constant */
    if (x->mode == GT_MODE_CONSTANT) {
        if (id == GT_REAL)
            x->val = constant_real(x->val);
        else
            x->val = constant_imag(x->val);
    } else {
        x->mode = GT_MODE_VALUE;
    }

    if (tbi_record_types(check) && x->mode != GT_MODE_CONSTANT) {
        burrow__tc_record_builtin_type(
            check, call->fun,
            tbi_make_sig(check->a, res_typ, 1, (const TypesType[]){x->typ}));
    }

    x->typ = res_typ;
    return true;
}

static bool tbi_make(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    /* make(T, n)
     * make(T, n, m)
     * (no argument evaluated yet) */
    Slice arg_list = call->args;
    Int nargs = arg_list.len;
    AstExpr arg0 = GT_AT(arg_list, 0, AstExpr);
    TypesType T = burrow__tc_var_type(check, arg0);
    if (!burrow__types_is_valid(T))
        return false;

    Str cause = BURROW_STR_EMPTY;
    TypesType u = burrow__tc_common_under(
        check, T, BURROW_FN(GtUnderCond, tbi_makeable, NULL), &cause);
    if (u == NULL) {
        Str es = burrow__tc_es(check, arg0);
        GT_ERRORF(check, burrow__tc_at_node(arg0), GT_CODE_INVALID_MAKE,
                  GT_INVALID_ARG "cannot make %s: %s", es, cause);
        return false;
    }

    Int min; /* minimum number of arguments */
    switch (u->kind) {
    case TYPES_KIND_SLICE:
        min = 2;
        break;
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        min = 1;
        break;
    default:
        /* any other type was excluded above */
        panic_str(BURROW_S("unreachable"));
    }
    if (nargs < min || min + 1 < nargs) {
        Str cs = burrow__tc_es(check, GT_NODE(call));
        Int max = min + 1;
        GT_ERRORF(check, GT_AT_NODE(call), GT_CODE_WRONG_ARG_COUNT,
                  GT_INVALID_OP "%s expects %d or %d arguments; found %d", cs, min, max,
                  nargs);
        return false;
    }

    TypesType types[3] = {T, NULL, NULL};
    int64_t sizes[2]; /* constant integer arguments, if any */
    Int nsizes = 0;
    for (Int i = 1; i < nargs; i++) {
        int64_t size = -1;
        /* ok to continue with typ == Typ[Invalid] */
        types[i] = burrow__tc_index(check, GT_AT(arg_list, i, AstExpr), -1, &size);
        if (size >= 0)
            sizes[nsizes++] = size;
    }
    if (nsizes == 2 && sizes[0] > sizes[1]) {
        burrow__tc_error(check, burrow__tc_at_node(GT_AT(arg_list, 1, AstExpr)),
                         GT_CODE_SWAPPED_MAKE_ARGS,
                         BURROW_S(GT_INVALID_ARG "length and capacity swapped"));
        /* safe to continue */
    }
    x->mode = GT_MODE_VALUE;
    x->typ = T;
    if (tbi_record_types(check)) {
        burrow__tc_record_builtin_type(check, call->fun,
                                       tbi_make_sig(check->a, x->typ, nargs, types));
    }
    return true;
}

static bool tbi_min_max(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id,
                        Slice args) {
    /* max(x, ...)
     * min(x, ...) */
    Str name = burrow__types_predeclared_funcs[id].name;
    (void)GT_VERIFY_VERSIONF(check, burrow__tc_at_node(call->fun), burrow__tc_go1_21,
                             "built-in %s", name);

    Token op = id == GT_MAX ? TOKEN_GTR : TOKEN_LSS;

    for (Int i = 0; i < args.len; i++) {
        GtOperand *a = GT_AT(args, i, GtOperand *);
        if (!burrow__tc_op_valid(a))
            return false;

        if (!burrow__types_all_basic(a->typ, TYPES_IS_ORDERED)) {
            Str as = burrow__tc_xs(check, a);
            GT_ERRORF(check, burrow__tc_at_op(a), GT_CODE_INVALID_MIN_MAX_OPERAND,
                      GT_INVALID_ARG "%s cannot be ordered", as);
            return false;
        }

        /* The first argument is already in x and there's nothing left to do. */
        if (i > 0) {
            burrow__tc_match_types(check, x, a);
            if (!burrow__tc_op_valid(x))
                return false;

            if (!types_identical(x->typ, a->typ)) {
                Str xt = burrow__tc_ts(check, x->typ);
                Str at = burrow__tc_ts(check, a->typ);
                Str ae = burrow__tc_es(check, a->expr);
                GT_ERRORF(check, burrow__tc_at_op(a), GT_CODE_MISMATCHED_TYPES,
                          GT_INVALID_ARG
                          "mismatched types %s (previous argument) and %s (type of %s)",
                          xt, at, ae);
                return false;
            }

            if (x->mode == GT_MODE_CONSTANT && a->mode == GT_MODE_CONSTANT) {
                if (constant_compare(a->val, op, x->val))
                    *x = *a;
            } else {
                x->mode = GT_MODE_VALUE;
            }
        }
    }

    /* If nargs == 1, make sure x.mode is either a value or a constant. */
    if (x->mode != GT_MODE_CONSTANT) {
        x->mode = GT_MODE_VALUE;
        /* A value must not be untyped. */
        burrow__tc_assignment(check, x, types_type(burrow__types_empty_interface),
                              GT_SPRINTF(check, "argument to built-in %s", name));
        if (!burrow__tc_op_valid(x))
            return false;
    }

    /* Use the final type computed above for all arguments. */
    for (Int i = 0; i < args.len; i++)
        burrow__tc_update_expr_type(check, GT_AT(args, i, GtOperand *)->expr, x->typ,
                                    true);

    if (tbi_record_types(check) && x->mode != GT_MODE_CONSTANT) {
        Slice types = burrow__types_ptrs_make(check->a, args.len);
        for (Int i = 0; i < args.len; i++)
            GT_AT(types, i, TypesType) = x->typ;
        burrow__tc_record_builtin_type(check, call->fun,
                                       tbi_make_sig_list(check->a, x->typ, types));
    }
    return true;
}

static bool tbi_new(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    /* new(T) or new(expr)
     * (no argument evaluated yet) */
    AstExpr arg = GT_AT(call->args, 0, AstExpr);
    burrow__tc_expr_or_type(check, x, arg, false);
    burrow__tc_exclude(check, x,
                       TBI_MODE_BIT(GT_MODE_NOVALUE) | TBI_MODE_BIT(GT_MODE_BUILTIN));
    if (x->mode == GT_MODE_INVALID)
        return false;
    if (x->mode == GT_MODE_TYPEXPR) {
        /* new(T) */
        burrow__tc_valid_var_type(check, arg, x->typ);
    } else {
        /* new(expr) */
        if (!burrow__types_is_typed(x->typ)) {
            /* check for overflow and untyped nil */
            burrow__tc_assignment(check, x, NULL, BURROW_S("argument to new"));
            if (!burrow__tc_op_valid(x))
                return false;
            burrow__types_assert(burrow__types_is_typed(x->typ));
        }
        /* report version error only if there are no other errors */
        Str es = burrow__tc_es(check, arg);
        (void)GT_VERIFY_VERSIONF(check, burrow__tc_at_node(call->fun),
                                 burrow__tc_go1_26, "new(%s)", es);
    }

    TypesType T = x->typ;
    x->mode = GT_MODE_VALUE;
    x->typ = types_type(types_new_pointer(check->a, T));
    if (tbi_record_types(check)) {
        burrow__tc_record_builtin_type(
            check, call->fun,
            tbi_make_sig(check->a, x->typ, 1, (const TypesType[]){T}));
    }
    return true;
}

static bool tbi_offsetof(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    /* unsafe.Offsetof(x T) uintptr, where x must be a selector
     * (no argument evaluated yet) */
    AstExpr arg0 = GT_AT(call->args, 0, AstExpr);
    AstExpr ux = ast_unparen(arg0);
    if (ux == NULL || ux->kind != AST_KIND_SELECTOR_EXPR) {
        Str es = burrow__tc_es(check, arg0);
        GT_ERRORF(check, burrow__tc_at_node(arg0), GT_CODE_BAD_OFFSETOF_SYNTAX,
                  GT_INVALID_ARG "%s is not a selector expression", es);
        (void)burrow__tc_use1(check, arg0, false);
        return false;
    }
    AstSelectorExpr *selx = (AstSelectorExpr *)(void *)ux;

    burrow__tc_expr(check, NULL, x, selx->x);
    if (!burrow__tc_op_valid(x))
        return false;

    TypesType base = burrow__types_deref_struct_ptr(x->typ);
    Str sel = selx->sel->name;
    Slice index = slice_nil(TYPE_INT);
    bool indirect = false;
    TypesObject obj = burrow__types_lookup_field_or_method(
        check->a, base, false, check->pkg, sel, false, &index, &indirect);
    if (obj == NULL) {
        Str bs = burrow__tc_ts(check, base);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_MISSING_FIELD_OR_METHOD,
                  GT_INVALID_ARG "%s has no single field %s", bs, sel);
        return false;
    }
    if (types_as_func(obj) != NULL) {
        /* TODO(gri) Using derefStructPtr may result in methods being found
         * that don't actually exist. An error either way, but the error
         * message is confusing. See: https://play.golang.org/p/al75v23kUy ,
         * but go/types reports: "invalid argument: x.m is a method value". */
        Str es = burrow__tc_es(check, arg0);
        GT_ERRORF(check, burrow__tc_at_node(arg0), GT_CODE_INVALID_OFFSETOF,
                  GT_INVALID_ARG "%s is a method value", es);
        return false;
    }
    if (indirect) {
        Str bs = burrow__tc_ts(check, base);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_OFFSETOF,
                  GT_INVALID_ARG "field %s is embedded via a pointer in %s", sel, bs);
        return false;
    }

    /* TODO(gri) Should we pass x.typ instead of base (and have indirect report
     * if derefStructPtr indirected)? */
    burrow__tc_record_selection(check, selx, TYPES_FIELD_VAL, base, obj, index, false);

    /* record the selector expression (was bug - go.dev/issue/47895) */
    {
        GtMode mode = GT_MODE_VALUE;
        if (x->mode == GT_MODE_VARIABLE || indirect)
            mode = GT_MODE_VARIABLE;
        GtOperand rec = {mode, GT_NODE(selx), GT_OBJ(obj)->typ, {0}, 0};
        burrow__tc_record(check, &rec);
    }

    /* The field offset is considered a variable even if the field is declared
     * before the part of the struct which is variable-sized. This makes both
     * the rules simpler and also permits (or at least doesn't prevent) a
     * compiler from re-arranging struct fields if it wanted to. */
    if (tbi_has_var_size(check, base)) {
        x->mode = GT_MODE_VALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(check->a, GT_TYP(TYPES_UINTPTR), 1,
                             (const TypesType[]){GT_OBJ(obj)->typ}));
        }
    } else {
        int64_t offs = burrow__types_conf_offsetof(check->conf, check->a, base, index);
        if (offs < 0) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TYPE_TOO_LARGE,
                      "%s is too large", xs);
            return false;
        }
        x->mode = GT_MODE_CONSTANT;
        x->val = constant_make_int64(offs);
        /* result is constant - no need to record signature */
    }
    x->typ = GT_TYP(TYPES_UINTPTR);
    return true;
}

/* unsafe.Alignof and unsafe.Sizeof. */
static bool tbi_alignof_sizeof(TypesChecker *check, GtOperand *x, AstCallExpr *call,
                               Int id) {
    if (id == GT_ALIGNOF)
        burrow__tc_assignment(check, x, NULL, BURROW_S("argument to unsafe.Alignof"));
    else
        burrow__tc_assignment(check, x, NULL, BURROW_S("argument to unsafe.Sizeof"));
    if (!burrow__tc_op_valid(x))
        return false;

    if (tbi_has_var_size(check, x->typ)) {
        x->mode = GT_MODE_VALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(check, call->fun,
                                           tbi_make_sig(check->a, GT_TYP(TYPES_UINTPTR),
                                                        1,
                                                        (const TypesType[]){x->typ}));
        }
    } else if (id == GT_ALIGNOF) {
        x->mode = GT_MODE_CONSTANT;
        x->val = constant_make_int64(burrow__types_conf_alignof(check->conf, x->typ));
        /* result is constant - no need to record signature */
    } else {
        int64_t size = burrow__types_conf_sizeof(check->conf, x->typ);
        if (size < 0) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TYPE_TOO_LARGE,
                      "%s is too large", xs);
            return false;
        }
        x->mode = GT_MODE_CONSTANT;
        x->val = constant_make_int64(size);
        /* result is constant - no need to record signature */
    }
    x->typ = GT_TYP(TYPES_UINTPTR);
    return true;
}

static bool tbi_trace(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    /* trace(x, y, z, ...) dumps the positions, expressions, and values of its
     * arguments. The result of trace is the value of the first argument.
     * Note: trace is only available in self-test mode.
     * (no argument evaluated yet) */
    if (call->args.len == 0) {
        Str ps = burrow__tc_ps(check, ast_node_pos(GT_NODE(call)));
        tbi_dump(GT_SPRINTF(check, "%s: trace() without arguments", ps));
        x->mode = GT_MODE_NOVALUE;
        return true;
    }
    GtOperand t = {0};
    GtOperand *x1 = x;
    for (Int i = 0; i < call->args.len; i++) {
        /* permit trace for types, e.g.: new(trace(T)) */
        (void)burrow__tc_raw_expr(check, NULL, x1, GT_AT(call->args, i, AstExpr), NULL,
                                  false);
        Str ps = burrow__tc_ps(check, burrow__tc_op_pos(x1));
        Str xs = burrow__tc_xs(check, x1);
        tbi_dump(GT_SPRINTF(check, "%s: %s", ps, xs));
        x1 = &t; /* use incoming x only for first argument */
    }
    /* trace is only available in test mode - no need to record signature */
    return burrow__tc_op_valid(x);
}

/* The rest of builtin, once the arguments are evaluated and counted. */
static bool tbi_call(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id,
                     Slice args) {
    Alloc *a = check->a;
    switch (id) {
    case GT_APPEND:
        return tbi_append(check, x, call, args);

    case GT_CAP:
    case GT_LEN:
        return tbi_len_cap(check, x, call, id);

    case GT_CLEAR: {
        /* clear(m) */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_21, BURROW_S("clear"));

        TbiOp c = {check, x, NULL};
        if (!burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tbi_clearable, &c)))
            return false;

        x->mode = GT_MODE_NOVALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, NULL, 1, (const TypesType[]){x->typ}));
        }
        return true;
    }

    case GT_CLOSE: {
        /* close(c) */
        TbiOp c = {check, x, NULL};
        if (!burrow__types_all(x->typ, BURROW_FN(GtTypePairFunc, tbi_closable, &c)))
            return false;
        x->mode = GT_MODE_NOVALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, NULL, 1, (const TypesType[]){x->typ}));
        }
        return true;
    }

    case GT_COMPLEX:
        return tbi_complex(check, x, call, GT_AT(args, 1, GtOperand *));

    case GT_COPY:
        return tbi_copy(check, x, call, GT_AT(args, 1, GtOperand *));

    case GT_DELETE: {
        /* delete(map_, key)
         * map_ must be a map type or a type parameter describing map types.
         * The key cannot be a type parameter for now. */
        TypesType map_ = x->typ;
        TbiOp c = {check, x, NULL};
        if (!burrow__types_all(map_, BURROW_FN(GtTypePairFunc, tbi_deletable, &c)))
            return false;

        *x = *GT_AT(args, 1, GtOperand *); /* key */
        burrow__tc_assignment(check, x, c.key, BURROW_S("argument to delete"));
        if (!burrow__tc_op_valid(x))
            return false;

        x->mode = GT_MODE_NOVALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, NULL, 2, (const TypesType[]){map_, c.key}));
        }
        return true;
    }

    case GT_IMAG:
    case GT_REAL:
        return tbi_real_imag(check, x, call, id);

    case GT_MAKE:
        return tbi_make(check, x, call);

    case GT_MAX:
    case GT_MIN:
        return tbi_min_max(check, x, call, id, args);

    case GT_NEW_:
        return tbi_new(check, x, call);

    case GT_PANIC: {
        /* panic(x)
         * record panic call if inside a function with result parameters (for
         * use in Checker.isTerminating) */
        TypesSignature *sig = check->env.sig;
        if (sig != NULL && types_tuple_len(sig->results) > 0) {
            /* function has result parameters */
            if (check->env.is_panic == NULL) {
                /* allocate lazily */
                check->env.is_panic = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
                if (check->env.is_panic == NULL)
                    burrow__types_oom();
            }
            bool yes = true;
            if (!map_set(check->env.is_panic, &call, &yes))
                burrow__types_oom();
        }

        TypesType empty = types_type(burrow__types_empty_interface);
        burrow__tc_assignment(check, x, empty, BURROW_S("argument to panic"));
        if (!burrow__tc_op_valid(x))
            return false;

        x->mode = GT_MODE_NOVALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun, tbi_make_sig(a, NULL, 1, (const TypesType[]){empty}));
        }
        return true;
    }

    case GT_PRINT:
    case GT_PRINTLN: {
        /* print(x, y, ...)
         * println(x, y, ...) */
        Str name = burrow__types_predeclared_funcs[id].name;
        Slice params = burrow__types_ptrs_make(a, args.len);
        for (Int i = 0; i < args.len; i++) {
            GtOperand *arg = GT_AT(args, i, GtOperand *);
            burrow__tc_assignment(check, arg, NULL,
                                  GT_SPRINTF(check, "argument to built-in %s", name));
            if (!burrow__tc_op_valid(arg))
                return false;
            GT_AT(params, i, TypesType) = arg->typ;
        }

        x->mode = GT_MODE_NOVALUE;
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(check, call->fun,
                                           tbi_make_sig_list(a, NULL, params));
        }
        return true;
    }

    case GT_RECOVER:
        /* recover() interface{} */
        x->mode = GT_MODE_VALUE;
        x->typ = types_type(burrow__types_empty_interface);
        if (tbi_record_types(check))
            burrow__tc_record_builtin_type(check, call->fun,
                                           tbi_make_sig(a, x->typ, 0, NULL));
        return true;

    case GT_ADD: {
        /* unsafe.Add(ptr unsafe.Pointer, len IntegerType) unsafe.Pointer */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_17, BURROW_S("unsafe.Add"));

        burrow__tc_assignment(check, x, GT_TYP(TYPES_UNSAFE_POINTER),
                              BURROW_S("argument to unsafe.Add"));
        if (!burrow__tc_op_valid(x))
            return false;

        GtOperand *y = GT_AT(args, 1, GtOperand *);
        if (!burrow__tc_is_valid_index(check, y, GT_CODE_INVALID_UNSAFE_ADD,
                                       BURROW_S("length"), true))
            return false;

        x->mode = GT_MODE_VALUE;
        x->typ = GT_TYP(TYPES_UNSAFE_POINTER);
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, x->typ, 2, (const TypesType[]){x->typ, y->typ}));
        }
        return true;
    }

    case GT_ALIGNOF:
    case GT_SIZEOF:
        /* unsafe.Alignof(x T) uintptr
         * unsafe.Sizeof(x T) uintptr */
        return tbi_alignof_sizeof(check, x, call, id);

    case GT_OFFSETOF:
        return tbi_offsetof(check, x, call);

    case GT_SLICE: {
        /* unsafe.Slice(ptr *T, len IntegerType) []T */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_17, BURROW_S("unsafe.Slice"));

        TypesType u = burrow__tc_common_under(check, x->typ, (GtUnderCond){0}, NULL);
        TypesPointer *ptr = types_as_pointer(u);
        if (ptr == NULL) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_UNSAFE_SLICE,
                      GT_INVALID_ARG "%s is not a pointer", xs);
            return false;
        }

        GtOperand *y = GT_AT(args, 1, GtOperand *);
        if (!burrow__tc_is_valid_index(check, y, GT_CODE_INVALID_UNSAFE_SLICE,
                                       BURROW_S("length"), false))
            return false;

        x->mode = GT_MODE_VALUE;
        x->typ = types_type(types_new_slice(a, ptr->base));
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, x->typ, 2,
                             (const TypesType[]){types_type(ptr), y->typ}));
        }
        return true;
    }

    case GT_SLICE_DATA: {
        /* unsafe.SliceData(slice []T) *T */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_20,
                                        BURROW_S("unsafe.SliceData"));

        TypesType u = burrow__tc_common_under(check, x->typ, (GtUnderCond){0}, NULL);
        TypesSlice *slice = types_as_slice(u);
        if (slice == NULL) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_UNSAFE_SLICE_DATA,
                      GT_INVALID_ARG "%s is not a slice", xs);
            return false;
        }

        x->mode = GT_MODE_VALUE;
        x->typ = types_type(types_new_pointer(a, slice->elem));
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, x->typ, 1, (const TypesType[]){types_type(slice)}));
        }
        return true;
    }

    case GT_STRING: {
        /* unsafe.String(ptr *byte, len IntegerType) string */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_20, BURROW_S("unsafe.String"));

        TypesType byte = types_type(burrow__types_universe_byte);
        burrow__tc_assignment(check, x, types_type(types_new_pointer(a, byte)),
                              BURROW_S("argument to unsafe.String"));
        if (!burrow__tc_op_valid(x))
            return false;

        GtOperand *y = GT_AT(args, 1, GtOperand *);
        if (!burrow__tc_is_valid_index(check, y, GT_CODE_INVALID_UNSAFE_STRING,
                                       BURROW_S("length"), false))
            return false;

        x->mode = GT_MODE_VALUE;
        x->typ = GT_TYP(TYPES_STRING);
        if (tbi_record_types(check)) {
            TypesType pb = types_type(types_new_pointer(a, byte));
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, x->typ, 2, (const TypesType[]){pb, y->typ}));
        }
        return true;
    }

    case GT_STRING_DATA: {
        /* unsafe.StringData(str string) *byte */
        (void)burrow__tc_verify_version(check, burrow__tc_at_node(call->fun),
                                        burrow__tc_go1_20,
                                        BURROW_S("unsafe.StringData"));

        burrow__tc_assignment(check, x, GT_TYP(TYPES_STRING),
                              BURROW_S("argument to unsafe.StringData"));
        if (!burrow__tc_op_valid(x))
            return false;

        x->mode = GT_MODE_VALUE;
        x->typ =
            types_type(types_new_pointer(a, types_type(burrow__types_universe_byte)));
        if (tbi_record_types(check)) {
            burrow__tc_record_builtin_type(
                check, call->fun,
                tbi_make_sig(a, x->typ, 1, (const TypesType[]){GT_TYP(TYPES_STRING)}));
        }
        return true;
    }

    case GT_ASSERT:
        /* assert(pred) causes a typechecker error if pred is false. The result
         * of assert is the value of pred if there is no error. Note: assert is
         * only available in self-test mode. */
        if (x->mode != GT_MODE_CONSTANT ||
            !burrow__types_is_basic(x->typ, TYPES_IS_BOOLEAN)) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TEST,
                      GT_INVALID_ARG "%s is not a boolean constant", xs);
            return false;
        }
        if (constant_value_kind(x->val) != CONSTANT_BOOL) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TEST,
                      "internal error: value of %s should be a boolean constant", xs);
            return false;
        }
        if (!constant_bool_val(x->val)) {
            Str cs = burrow__tc_es(check, GT_NODE(call));
            GT_ERRORF(check, GT_AT_NODE(call), GT_CODE_TEST, "%s failed", cs);
            /* compile-time assertion failure - safe to continue */
        }
        /* result is constant - no need to record signature */
        return true;

    case GT_TRACE:
        return tbi_trace(check, x, call);

    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* builtin type-checks a call to the built-in specified by id and reports
 * whether the call is valid, with *x holding the result; but x.expr is not
 * set. If the call is invalid, the result is false, and *x is undefined. */
static bool tbi_builtin(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id) {
    Slice arg_list = call->args;

    /* append is the only built-in that permits the use of ... for the last
     * argument */
    const GtBuiltinInfo *bin = &burrow__types_predeclared_funcs[id];
    if (burrow__tc_has_dots(call) && id != GT_APPEND) {
        Str name = bin->name;
        GT_ERRORF(check, burrow__tc_at_pos(call->ellipsis), GT_CODE_INVALID_DOT_DOT_DOT,
                  GT_INVALID_OP "invalid use of ... with built-in %s", name);
        (void)burrow__tc_use(check, arg_list);
        return false;
    }

    /* Evaluate arguments for built-ins that use ordinary (value) arguments. For
     * built-ins with special argument handling (make, new, etc.), evaluation
     * is done by the respective built-in code. */
    Slice args = slice_nil(TYPE_UNSAFE_POINTER); /* not valid for make, new,
                                                    Offsetof and trace */
    Int nargs;
    switch (id) {
    case GT_MAKE:
    case GT_NEW_:
    case GT_OFFSETOF:
    case GT_TRACE:
        /* arguments require special handling */
        nargs = arg_list.len;
        break;
    default:
        /* check all arguments */
        args = burrow__tc_expr_list(check, arg_list);
        nargs = args.len;
        for (Int i = 0; i < nargs; i++) {
            if (!burrow__tc_op_valid(GT_AT(args, i, GtOperand *)))
                return false;
        }
        /* first argument is always in x */
        if (nargs > 0)
            *x = *GT_AT(args, 0, GtOperand *);
        break;
    }

    /* check argument count */
    {
        Str msg = BURROW_STR_EMPTY;
        if (nargs < bin->nargs)
            msg = BURROW_S("not enough");
        else if (!bin->variadic && nargs > bin->nargs)
            msg = BURROW_S("too many");
        if (msg.len != 0) {
            Str cs = burrow__tc_es(check, GT_NODE(call));
            Int want = bin->nargs;
            GT_ERRORF(check, burrow__tc_in_node(GT_NODE(call), call->rparen),
                      GT_CODE_WRONG_ARG_COUNT,
                      GT_INVALID_OP "%s arguments for %s (expected %d, found %d)", msg,
                      cs, want, nargs);
            return false;
        }
    }

    if (!tbi_call(check, x, call, id, args))
        return false;
    burrow__types_assert(burrow__tc_op_valid(x));
    return true;
}

bool burrow__tc_builtin(TypesChecker *check, GtOperand *x, AstCallExpr *call, Int id) {
    /* For len(x) and cap(x) we need to know if x contains any function calls
     * or receive operations. Save/restore current setting and set
     * hasCallOrRecv to false for the evaluation of x so that we can check it
     * afterwards. Note: We must do this _before_ calling exprList because
     * exprList evaluates all arguments. */
    if (id != GT_LEN && id != GT_CAP)
        return tbi_builtin(check, x, call, id);
    bool b = check->env.has_call_or_recv;
    check->env.has_call_or_recv = false;
    bool ok = tbi_builtin(check, x, call, id);
    check->env.has_call_or_recv = b;
    return ok;
}
