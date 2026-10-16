/* go/types: typechecking of literals.
 *
 * Derived from Go's src/go/types/literals.go. Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* langCompat reports an error if the representation of a numeric literal is
 * not compatible with the current language version. */
static void tli_lang_compat(TypesChecker *check, AstBasicLit *lit) {
    Str s = lit->value;
    if (s.len <= 2 || burrow__tc_allow_version(check, burrow__tc_go1_13))
        return;
    /* len(s) > 2 */
    if (strings_contains(s, BURROW_S("_"))) {
        burrow__tc_version_error(check, GT_AT_NODE(lit), burrow__tc_go1_13,
                                 BURROW_S("underscore in numeric literal"));
        return;
    }
    if (s.p[0] != '0')
        return;
    char radix = (char)s.p[1];
    if (radix == 'b' || radix == 'B') {
        burrow__tc_version_error(check, GT_AT_NODE(lit), burrow__tc_go1_13,
                                 BURROW_S("binary literal"));
        return;
    }
    if (radix == 'o' || radix == 'O') {
        burrow__tc_version_error(check, GT_AT_NODE(lit), burrow__tc_go1_13,
                                 BURROW_S("0o/0O-style octal literal"));
        return;
    }
    if (lit->kind != TOKEN_INT && (radix == 'x' || radix == 'X'))
        burrow__tc_version_error(check, GT_AT_NODE(lit), burrow__tc_go1_13,
                                 BURROW_S("hexadecimal floating-point literal"));
}

void burrow__tc_basic_lit(TypesChecker *check, GtOperand *x, AstBasicLit *e) {
    if (e->kind == TOKEN_INT || e->kind == TOKEN_FLOAT || e->kind == TOKEN_IMAG) {
        tli_lang_compat(check, e);
        /* The max. mantissa precision for untyped numeric values is 512 bits,
         * or 4048 bits for each of the two integer parts of a fraction for
         * floating-point numbers that are represented accurately in the
         * go/constant package. Constant literals that are longer than this
         * many bits are not meaningful; and excessively long constants may
         * consume a lot of space and time for a useless conversion. Cap
         * constant length with a generous upper limit that also allows for
         * separators between all digits. */
        const Int limit = 10000;
        if (e->value.len > limit) {
            Str head = str_from_bytes(e->value.p, 10);
            Int n = e->value.len;
            GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_CONST_VAL,
                      "excessively long constant: %s... (%d chars)", head, n);
            burrow__tc_op_invalidate(x);
            return;
        }
    }
    burrow__tc_op_set_const(check->a, x, e->kind, e->value);
    if (!burrow__tc_op_valid(x)) {
        /* The parser already establishes syntactic correctness. If we reach
         * here it's because of number under-/overflow. */
        Str v = e->value;
        GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_CONST_VAL,
                  "malformed constant: %s", v);
        burrow__tc_op_invalidate(x);
        return;
    }
    /* Ensure that integer values don't overflow (go.dev/issue/54280). */
    x->expr = GT_NODE(e); /* make sure that check.overflow below has an error
                             position */
    burrow__tc_overflow(check, x, burrow__tc_expr_op_pos(x->expr));
}

/* What the later function of funcLit closes over. */
typedef struct TliFuncBody {
    GtDeclInfo *decl;
    ConstantValue iota;
    bool has_iota;
    TypesSignature *sig;
    AstBlockStmt *body;
} TliFuncBody;

static void tli_func_body_later(TypesChecker *check, void *env) {
    TliFuncBody *c = env;
    burrow__tc_func_body(check, c->decl, BURROW_S("<function literal>"), c->sig,
                         c->body, c->has_iota ? &c->iota : NULL);
}

void burrow__tc_func_lit(TypesChecker *check, GtOperand *x, AstFuncLit *e) {
    TypesSignature *sig = types_as_signature(burrow__tc_typ(check, GT_NODE(e->type)));
    if (sig == NULL) {
        Str es = burrow__tc_es(check, GT_NODE(e));
        GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_SYNTAX_TREE,
                  "invalid function literal %s", es);
        burrow__tc_op_invalidate(x);
        return;
    }
    /* Set the Scope's extent to the complete "func (...) {...}" so that
     * Scope.Innermost works correctly. */
    sig->scope->pos = ast_expr_pos(GT_NODE(e));
    sig->scope->end = ast_node_end(GT_NODE(e));
    if (!check->conf->ignore_func_bodies && e->body != NULL) {
        /* Anonymous functions are considered part of the init
         * expression/func declaration which contains them: use existing
         * package-level declaration info. The iota is copied for use in the
         * later function (go.dev/issue/22345). Don't type-check right away
         * because the function may be part of a type definition to which the
         * function body refers. Instead, type-check as soon as possible, but
         * before the enclosing scope contents changes (go.dev/issue/22992). */
        TliFuncBody *c = GT_NEW(check->a, TliFuncBody);
        c->decl = check->env.decl;
        if (check->env.iota != NULL) {
            c->iota = *check->env.iota;
            c->has_iota = true;
        }
        c->sig = sig;
        c->body = e->body;
        burrow__tc_later(check, tli_func_body_later, c);
    }
    x->mode = GT_MODE_VALUE;
    x->typ = types_type(sig);
}

/* isdddArray: whether atyp is an "open" [...]T array type. */
static bool tli_is_ddd_array(const AstArrayType *atyp) {
    if (atyp->len != NULL && atyp->len->kind == AST_KIND_ELLIPSIS) {
        const AstEllipsis *ddd = (const AstEllipsis *)(const void *)atyp->len;
        return ddd->elt == NULL;
    }
    return false;
}

/* Not in Go: Go keeps the index sequences of the fields seen in a struct
 * literal in a trie. Here they are a list; the keys stored are prefix-free,
 * as in the trie, and a collision is found the way the trie finds it. */
typedef struct TliKey {
    Slice key; /* of Int */
    TypesVar *val;
} TliKey;

/* tli_cmp_keys compares the first n elements of a and b, in order. */
static int tli_cmp_keys(Slice a, Slice b, Int n) {
    for (Int i = 0; i < n; i++) {
        Int x = GT_AT(a, i, Int), y = GT_AT(b, i, Int);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

/* insert attempts to insert val for key. If an existing key is a prefix of
 * (or equal to) key, it returns that key's value and length. If key is a
 * proper prefix of existing keys, it returns the value of the smallest of
 * them and the length of key. Otherwise it inserts val and returns 0. */
static TypesVar *tli_insert(TypesChecker *check, Slice *keys, Slice key, TypesVar *val,
                            Int *n) {
    burrow__types_assert(key.len > 0);
    TliKey *longer = NULL;
    for (Int i = 0; i < keys->len; i++) {
        TliKey *k = GT_AT(*keys, i, TliKey *);
        if (k->key.len <= key.len) {
            if (tli_cmp_keys(k->key, key, k->key.len) == 0) {
                *n = k->key.len;
                return k->val;
            }
        } else if (tli_cmp_keys(k->key, key, key.len) == 0) {
            if (longer == NULL ||
                tli_cmp_keys(k->key, longer->key,
                             k->key.len < longer->key.len ? k->key.len
                                                          : longer->key.len) < 0)
                longer = k;
        }
    }
    if (longer != NULL) {
        *n = key.len;
        return longer->val;
    }
    TliKey *k = GT_NEW(check->a, TliKey);
    k->key = key;
    k->val = val;
    burrow__types_ptrs_append(check->a, keys, k);
    *n = 0;
    return val;
}

static void tli_struct_keyed(TypesChecker *check, GtOperand *x, AstCompositeLit *e,
                             TypesStruct *utyp, TypesType base) {
    Slice visited = slice_nil(TYPE_UNSAFE_POINTER); /* of TliKey * */
    for (Int i = 0; i < e->elts.len; i++) {
        AstExpr el = GT_AT(e->elts, i, AstExpr);
        if (el->kind != AST_KIND_KEY_VALUE_EXPR) {
            burrow__tc_error(
                check, burrow__tc_at_node(el), GT_CODE_MIXED_STRUCT_LIT,
                BURROW_S(
                    "mixture of field:value and value elements in struct literal"));
            continue;
        }
        AstKeyValueExpr *kv = (AstKeyValueExpr *)(void *)el;
        AstIdent *key =
            kv->key->kind == AST_KIND_IDENT ? (AstIdent *)(void *)kv->key : NULL;
        /* do all possible checks early (before exiting due to errors) so we
         * don't drop information on the floor */
        burrow__tc_generic_expr(check, x, kv->value, NULL);
        if (key == NULL) {
            Str ks = burrow__tc_es(check, kv->key);
            GT_ERRORF(check, GT_AT_NODE(kv), GT_CODE_INVALID_LIT_FIELD,
                      "invalid field name %s in struct literal", ks);
            continue;
        }
        Slice index;
        bool indirect;
        TypesObject obj = burrow__types_lookup_field_or_method(
            check->a, types_type(utyp), false, check->pkg, key->name, false, &index,
            &indirect);
        if (obj == NULL) {
            Slice index2;
            bool indirect2;
            TypesObject alt = burrow__types_lookup_field_or_method(
                check->a, types_type(utyp), false, check->pkg, key->name, true, &index2,
                &indirect2);
            Str msg = burrow__tc_lookup_error(check, base, key->name, alt, true);
            burrow__tc_error(check, burrow__tc_at_node(kv->key),
                             GT_CODE_MISSING_LIT_FIELD, msg);
            continue;
        }
        TypesVar *fld = types_as_var(obj);
        if (fld == NULL) {
            Str ks = burrow__tc_es(check, kv->key);
            GT_ERRORF(check, burrow__tc_at_node(kv->key), GT_CODE_MISSING_LIT_FIELD,
                      "%s is not a field", ks);
            continue;
        }
        if (index.len > 1) {
            Str path = burrow__types_field_path(check->a, types_type(utyp), index);
            Str bs = burrow__tc_ts(check, base);
            if (!GT_VERIFY_VERSIONF(
                    check, burrow__tc_at_node(kv->key), burrow__tc_go1_27,
                    "use of promoted field %s in struct literal of type "
                    "%s",
                    path, bs))
                continue;
        }
        if (indirect) {
            Str ks = burrow__tc_es(check, kv->key);
            GT_ERRORF(check, burrow__tc_at_node(kv->key), GT_CODE_INVALID_LIT_FIELD,
                      "invalid implicit pointer indirection to reach %s", ks);
            continue;
        }
        burrow__tc_record_use(check, key, obj);
        TypesType etyp = GT_OBJ(fld)->typ;
        burrow__tc_assignment(check, x, etyp, BURROW_S("struct literal"));
        Int n;
        TypesVar *alt = tli_insert(check, &visited, index, fld, &n);
        if (n != 0) {
            Str fname = GT_OBJ(fld)->name, aname = GT_OBJ(alt)->name;
            if (fld == alt)
                GT_ERRORF(check, GT_AT_NODE(kv), GT_CODE_DUPLICATE_LIT_FIELD,
                          "duplicate field name %s in struct literal", fname);
            else if (n < index.len)
                GT_ERRORF(check, GT_AT_NODE(kv), GT_CODE_DUPLICATE_LIT_FIELD,
                          "cannot specify promoted field %s and enclosing embedded "
                          "field %s",
                          fname, aname);
            else /* n > len(index) */
                GT_ERRORF(check, GT_AT_NODE(kv), GT_CODE_DUPLICATE_LIT_FIELD,
                          "cannot specify embedded field %s and enclosed promoted "
                          "field %s",
                          fname, aname);
        }
    }
}

static void tli_struct_positional(TypesChecker *check, GtOperand *x, AstCompositeLit *e,
                                  TypesStruct *utyp, TypesType base) {
    Slice fields = utyp->fields;
    for (Int i = 0; i < e->elts.len; i++) {
        AstExpr el = GT_AT(e->elts, i, AstExpr);
        if (el->kind == AST_KIND_KEY_VALUE_EXPR) {
            burrow__tc_error(
                check, burrow__tc_at_node(el), GT_CODE_MIXED_STRUCT_LIT,
                BURROW_S(
                    "mixture of field:value and value elements in struct literal"));
            continue;
        }
        burrow__tc_generic_expr(check, x, el, NULL);
        if (i >= fields.len) {
            Str bs = burrow__tc_ts(check, base);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_STRUCT_LIT,
                      "too many values in struct literal of type %s", bs);
            break; /* cannot continue */
        }
        /* i < len(fields) */
        TypesVar *fld = GT_AT(fields, i, TypesVar *);
        if (!types_object_exported(types_object(fld)) &&
            GT_OBJ(fld)->pkg != check->pkg) {
            Str fname = GT_OBJ(fld)->name;
            Str bs = burrow__tc_ts(check, base);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNEXPORTED_LIT_FIELD,
                      "implicit assignment to unexported field %s in struct literal of "
                      "type %s",
                      fname, bs);
            continue;
        }
        TypesType etyp = GT_OBJ(fld)->typ;
        burrow__tc_assignment(check, x, etyp, BURROW_S("struct literal"));
    }
    if (e->elts.len < fields.len) {
        Str hint = {0};
        for (Int i = 0; i < fields.len; i++) {
            TypesVar *fld = GT_AT(fields, i, TypesVar *);
            if (!types_object_exported(types_object(fld)) &&
                GT_OBJ(fld)->pkg != check->pkg) {
                hint = BURROW_S(" (type has unexported fields - use key:value pairs)");
                break;
            }
        }
        Str bs = burrow__tc_ts(check, base);
        GT_ERRORF(check, burrow__tc_in_node(GT_NODE(e), e->rbrace),
                  GT_CODE_INVALID_STRUCT_LIT,
                  "too few values in struct literal of type %s%s", bs, hint);
        /* ok to continue */
    }
}

/* indexedElts checks the elements (elts) of an array or slice composite literal
 * against the literal's element type (typ), and the element indices against
 * the literal length if known (length >= 0). It returns the length of the
 * literal (maximum index value + 1). */
static int64_t tli_indexed_elts(TypesChecker *check, Slice elts, TypesType typ,
                                int64_t length) {
    Map *visited = map_make(check->a, TYPE_INT64, TYPE_BOOL, elts.len);
    if (visited == NULL)
        burrow__types_oom();
    int64_t index = 0, max = 0;
    for (Int k = 0; k < elts.len; k++) {
        AstExpr e = GT_AT(elts, k, AstExpr);
        /* determine and check index */
        bool valid_index = false;
        AstExpr eval = e;
        if (e->kind == AST_KIND_KEY_VALUE_EXPR) {
            AstKeyValueExpr *kv = (AstKeyValueExpr *)(void *)e;
            int64_t i;
            if (burrow__types_is_valid(burrow__tc_index(check, kv->key, length, &i))) {
                if (i >= 0) {
                    index = i;
                    valid_index = true;
                } else {
                    Str ks = burrow__tc_es(check, kv->key);
                    GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_INVALID_LIT_INDEX,
                              "index %s must be integer constant", ks);
                }
            }
            eval = kv->value;
        } else if (length >= 0 && index >= length) {
            GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_OVERSIZE_ARRAY_LIT,
                      "index %d is out of bounds (>= %d)", index, length);
        } else {
            valid_index = true;
        }

        /* if we have a valid index, check for duplicate entries */
        if (valid_index) {
            if (map_get(visited, &index) != NULL)
                GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_DUPLICATE_LIT_KEY,
                          "duplicate index %d in array or slice literal", index);
            bool yes = true;
            if (!map_set(visited, &index, &yes))
                burrow__types_oom();
        }
        index++;
        if (index > max)
            max = index;

        /* check element against composite literal element type */
        GtOperand x = {0};
        burrow__tc_generic_expr(check, &x, eval, typ);
        burrow__tc_assignment(check, &x, typ, BURROW_S("array or slice literal"));
    }
    return max;
}

static void tli_map_lit(TypesChecker *check, GtOperand *x, AstCompositeLit *e,
                        TypesMap *utyp) {
    /* If the map key type is an interface (but not a type parameter), the type
     * of a constant key must be considered when checking for duplicates. */
    bool key_is_interface = burrow__types_is_non_type_param_interface(utyp->key);
    /* keyVal to the key types seen with it, a Slice * of TypesType, which is
     * NULL when the key type is not an interface */
    Map *visited = map_make(check->a, TYPE_STRING, TYPE_UNSAFE_POINTER, e->elts.len);
    if (visited == NULL)
        burrow__types_oom();
    for (Int i = 0; i < e->elts.len; i++) {
        AstExpr el = GT_AT(e->elts, i, AstExpr);
        if (el->kind != AST_KIND_KEY_VALUE_EXPR) {
            burrow__tc_error(check, burrow__tc_at_node(el), GT_CODE_MISSING_LIT_KEY,
                             BURROW_S("missing key in map literal"));
            continue;
        }
        AstKeyValueExpr *kv = (AstKeyValueExpr *)(void *)el;
        burrow__tc_generic_expr(check, x, kv->key, utyp->key);
        burrow__tc_assignment(check, x, utyp->key, BURROW_S("map literal"));
        if (!burrow__tc_op_valid(x))
            continue;
        if (x->mode == GT_MODE_CONSTANT) {
            bool duplicate = false;
            Str xkey = burrow__tc_key_val(check->a, x->val);
            Slice **seen = map_get(visited, &xkey);
            if (key_is_interface) {
                Slice *types = seen != NULL ? *seen : NULL;
                if (types == NULL) {
                    types = GT_NEW(check->a, Slice);
                    *types = slice_nil(TYPE_UNSAFE_POINTER);
                    if (!map_set(visited, &xkey, &types))
                        burrow__types_oom();
                }
                for (Int k = 0; k < types->len; k++) {
                    if (types_identical(GT_AT(*types, k, TypesType), x->typ)) {
                        duplicate = true;
                        break;
                    }
                }
                burrow__types_ptrs_append(check->a, types, x->typ);
            } else {
                duplicate = seen != NULL;
                Slice *none = NULL;
                if (!map_set(visited, &xkey, &none))
                    burrow__types_oom();
            }
            if (duplicate) {
                Str vs = constant_value_string(x->val, check->a);
                GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_DUPLICATE_LIT_KEY,
                          "duplicate key %s in map literal", vs);
                continue;
            }
        }
        burrow__tc_generic_expr(check, x, kv->value, utyp->elem);
        burrow__tc_assignment(check, x, utyp->elem, BURROW_S("map literal"));
    }
}

void burrow__tc_composite_lit(TypesChecker *check, GtOperand *x, AstCompositeLit *e,
                              TypesType hint) {
    TypesType typ, base;
    bool is_elem = false; /* true if composite literal is an element of an
                             enclosing composite literal */

    if (e->type != NULL) {
        /* composite literal type present - use it. [...]T array types may
         * only appear with composite literals. Check for them here so we don't have
         * to handle ... in general. */
        AstArrayType *atyp = e->type->kind == AST_KIND_ARRAY_TYPE
                                 ? (AstArrayType *)(void *)e->type
                                 : NULL;
        if (atyp != NULL && tli_is_ddd_array(atyp)) {
            /* We have an "open" [...]T array type. Create a new ArrayType with
             * unknown length (-1) and finish setting it up after analyzing the
             * literal. */
            TypesArray *arr = GT_NEW(check->a, TypesArray);
            arr->hdr.kind = TYPES_KIND_ARRAY;
            arr->len = -1;
            arr->elem = burrow__tc_var_type(check, atyp->elt);
            typ = types_type(arr);
            base = typ;
        } else {
            typ = burrow__tc_typ(check, e->type);
            base = typ;
        }
    } else if (hint != NULL) {
        /* no composite literal type present - use hint (element type of
         * enclosing type) */
        typ = hint;
        base = typ;
        /* *T implies &T{} */
        TypesType u = burrow__tc_common_under(check, base, (GtUnderCond){0}, NULL);
        bool ok;
        TypesType b = burrow__types_deref(u, &ok);
        if (ok)
            base = b;
        is_elem = true;
    } else {
        burrow__tc_error(check, GT_AT_NODE(e), GT_CODE_UNTYPED_LIT,
                         BURROW_S("missing type in composite literal"));
        /* continue with invalid type so that elements are "used"
         * (go.dev/issue/69092) */
        typ = GT_TYP(TYPES_INVALID);
        base = typ;
    }

    /* We cannot create a literal of an incomplete type; make sure it's
     * complete. */
    if (!burrow__tc_is_complete(check, base)) {
        burrow__tc_op_invalidate(x);
        return;
    }

    TypesType u = burrow__tc_common_under(check, base, (GtUnderCond){0}, NULL);
    Int kind = u != NULL ? u->kind : TYPES_KIND_INVALID;
    if (u != NULL && kind == TYPES_KIND_STRUCT) {
        TypesStruct *utyp = (TypesStruct *)(void *)u;
        /* Convention for error messages on invalid struct literals: we mention
         * the struct type only if it clarifies the error (e.g., a duplicate
         * field error doesn't need the struct type). */
        if (e->elts.len > 0) {
            if (GT_AT(e->elts, 0, AstExpr)->kind == AST_KIND_KEY_VALUE_EXPR)
                /* all elements must have keys */
                tli_struct_keyed(check, x, e, utyp, base);
            else
                /* no element must have a key */
                tli_struct_positional(check, x, e, utyp, base);
        }
    } else if (u != NULL && kind == TYPES_KIND_ARRAY) {
        TypesArray *utyp = (TypesArray *)(void *)u;
        int64_t n = tli_indexed_elts(check, e->elts, utyp->elem, utyp->len);
        /* If we have an array of unknown length (usually [...]T arrays, but
         * also arrays [n]T where n is invalid) set the length now that we know
         * it and record the type for the array (usually done by check.typ
         * which is not called for [...]T). We handle [...]T arrays and arrays
         * with invalid length the same here because it makes sense to "guess"
         * the length for the latter if we have a composite literal; e.g. for
         * [n]int{1, 2, 3} where n is invalid for some reason, it seems fair to
         * assume it should be 3 (see also Checked.arrayLength and
         * go.dev/issue/27346). */
        if (utyp->len < 0) {
            utyp->len = n;
            /* e.Type is missing if we have a composite literal element that is
             * itself a composite literal with omitted type. In that case there
             * is nothing to record (there is no type in the source at that
             * point). */
            if (e->type != NULL)
                burrow__tc_record_type_and_value(check, e->type, GT_MODE_TYPEXPR, u,
                                                 (ConstantValue){0});
        }
    } else if (u != NULL && kind == TYPES_KIND_SLICE) {
        TypesSlice *utyp = (TypesSlice *)(void *)u;
        (void)tli_indexed_elts(check, e->elts, utyp->elem, -1);
    } else if (u != NULL && kind == TYPES_KIND_MAP) {
        tli_map_lit(check, x, e, (TypesMap *)(void *)u);
    } else {
        /* when "using" all elements unpack KeyValueExpr explicitly because
         * check.use doesn't accept them */
        for (Int i = 0; i < e->elts.len; i++) {
            AstExpr el = GT_AT(e->elts, i, AstExpr);
            if (el->kind == AST_KIND_KEY_VALUE_EXPR) {
                /* Ideally, we should also "use" kv.Key but we can't know if
                 * it's an externally defined struct key or not. Going forward
                 * anyway can lead to other errors. Give up instead. */
                el = ((AstKeyValueExpr *)(void *)el)->value;
            }
            burrow__tc_use1(check, el, false);
        }
        /* if utyp is invalid, an error was reported before */
        if (burrow__types_is_valid(u)) {
            Str qualifier = {0};
            if (is_elem)
                qualifier = BURROW_S(" element");
            Str cause = {0};
            if (u == NULL)
                cause = BURROW_S(" (no common underlying type)");
            Str ts = burrow__tc_ts(check, typ);
            GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_LIT,
                      "invalid composite literal%s type %s%s", qualifier, ts, cause);
            burrow__tc_op_invalidate(x);
            return;
        }
    }

    x->mode = GT_MODE_VALUE;
    x->typ = typ;
}
