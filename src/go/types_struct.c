/* go/types: type-checking of struct types.
 *
 * Derived from Go's src/go/types/struct.go. Go source: go1.27.1.
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
#include "burrow/strconv.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* The variables structType's add and addInvalid functions close over. */
typedef struct TstState {
    TypesChecker *check;
    Slice fields; /* of TypesVar * */
    Slice tags;   /* of Str */
    bool has_tags;
    GtObjset fset; /* for double-declaration checks */
    TypesType typ; /* current field typ and tag */
    Str tag;
} TstState;

/* declareInSet inserts obj into oset, or reports that it is a duplicate. */
static bool tst_declare_in_set(TypesChecker *check, GtObjset *oset, TokenPos pos,
                               TypesObject obj) {
    TypesObject alt = burrow__types_objset_insert(oset, obj);
    if (alt != NULL) {
        GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_DECL);
        GT_ADDF(err, burrow__tc_at_pos(pos), "%s redeclared", GT_OBJ(obj)->name);
        burrow__tc_error_add_alt_decl(err, alt);
        burrow__tc_error_report(err);
        return false;
    }
    return true;
}

static void tst_add(TstState *st, AstIdent *ident, bool embedded) {
    TypesChecker *check = st->check;
    if (st->tag.len > 0 && !st->has_tags) {
        st->tags = slice_make(check->a, TYPE_STRING, st->fields.len, st->fields.len);
        st->has_tags = true;
    }
    if (st->has_tags)
        st->tags = slice_append(check->a, st->tags, &st->tag, 1);

    TokenPos pos = ast_node_pos(GT_NODE(ident));
    Str name = ident->name;
    TypesVar *fld = types_new_field(check->a, pos, check->pkg, name, st->typ, embedded);
    /* spec: "Within a struct, non-blank field names must be unique." */
    if (str_eq(name, BURROW_S("_")) ||
        tst_declare_in_set(check, &st->fset, pos, types_object(fld))) {
        burrow__types_ptrs_append(check->a, &st->fields, fld);
        burrow__tc_record_def(check, ident, types_object(fld));
    }
}

/* addInvalid adds an embedded field of invalid type to the struct for fields
 * with errors; this keeps the number of struct fields in sync with the source
 * as long as the fields are _ or have different names (go.dev/issue/25627). */
static void tst_add_invalid(TstState *st, AstIdent *ident) {
    st->typ = GT_TYP(TYPES_INVALID);
    st->tag = BURROW_STR_EMPTY;
    tst_add(st, ident, true);
}

/* tag returns the value of the field tag t, or "". */
static Str tst_tag(TypesChecker *check, AstBasicLit *t) {
    if (t != NULL) {
        if (t->kind == TOKEN_STRING) {
            Error err = BURROW_NO_ERROR;
            Str val = strconv_unquote(check->a, t->value, &err);
            if (!BURROW_FAILED(err))
                return val;
        }
        GT_ERRORF(check, GT_AT_NODE(t), GT_CODE_INVALID_SYNTAX_TREE,
                  "incorrect tag syntax: %q", t->value);
    }
    return BURROW_STR_EMPTY;
}

/* embeddedFieldIdent returns the name an embedded field of type e has, or NULL
 * if e is not a valid embedded field type. */
static AstIdent *tst_embedded_field_ident(AstExpr e) {
    switch (e->kind) {
    case AST_KIND_IDENT:
        return (AstIdent *)(void *)e;
    case AST_KIND_STAR_EXPR: {
        /* *T is valid, but **T is not */
        AstExpr x = ((AstStarExpr *)(void *)e)->x;
        if (x->kind != AST_KIND_STAR_EXPR)
            return tst_embedded_field_ident(x);
        break;
    }
    case AST_KIND_SELECTOR_EXPR:
        return ((AstSelectorExpr *)(void *)e)->sel;
    case AST_KIND_INDEX_EXPR:
        return tst_embedded_field_ident(((AstIndexExpr *)(void *)e)->x);
    case AST_KIND_INDEX_LIST_EXPR:
        return tst_embedded_field_ident(((AstIndexListExpr *)(void *)e)->x);
    default:
        break;
    }
    return NULL; /* invalid embedded field */
}

/* What the later function for an embedded field closes over. */
typedef struct TstEmbedded {
    TypesType typ;
    AstExpr pos;
} TstEmbedded;

static void tst_embedded_later(TypesChecker *check, void *env) {
    TstEmbedded *c = env;
    bool is_ptr = false;
    TypesType t = burrow__types_deref(c->typ, &is_ptr);
    TypesType u = types_type_underlying(t);
    GtPosn at = burrow__tc_at_node(c->pos);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        if (!burrow__types_is_valid(t))
            return; /* error was reported before */
        /* unsafe.Pointer is treated like a regular pointer */
        if (types_as_basic(u)->kind == TYPES_UNSAFE_POINTER)
            burrow__tc_error(check, at, GT_CODE_INVALID_PTR_EMBED,
                             BURROW_S("embedded field type cannot be unsafe.Pointer"));
        break;
    case TYPES_KIND_POINTER:
        burrow__tc_error(check, at, GT_CODE_INVALID_PTR_EMBED,
                         BURROW_S("embedded field type cannot be a pointer"));
        break;
    case TYPES_KIND_INTERFACE:
        if (burrow__types_is_type_param(t)) {
            /* The error code here is inconsistent with other error codes for
             * invalid embedding, because this restriction may be relaxed in the
             * future, and so it did not warrant a new error code. */
            burrow__tc_error(
                check, at, GT_CODE_MISPLACED_TYPE_PARAM,
                BURROW_S(
                    "embedded field type cannot be a (pointer to a) type parameter"));
            break;
        }
        if (is_ptr)
            burrow__tc_error(
                check, at, GT_CODE_INVALID_PTR_EMBED,
                BURROW_S("embedded field type cannot be a pointer to an interface"));
        break;
    default:
        break;
    }
}

void burrow__tc_struct_type(TypesChecker *check, TypesStruct *styp, AstStructType *e) {
    AstFieldList *list = e->fields;
    if (list == NULL) {
        styp->complete = true;
        return;
    }

    TstState st = {0};
    st.check = check;
    st.fields = slice_nil(TYPE_UNSAFE_POINTER);
    st.tags = slice_nil(TYPE_STRING);
    st.fset.a = check->a;

    for (Int i = 0; i < list->list.len; i++) {
        AstField *f = GT_AT(list->list, i, AstField *);
        st.typ = burrow__tc_var_type(check, f->type);
        st.tag = tst_tag(check, f->tag);
        if (f->names.len > 0) {
            /* named fields */
            for (Int j = 0; j < f->names.len; j++)
                tst_add(&st, GT_AT(f->names, j, AstIdent *), false);
            continue;
        }

        /* embedded field
         * spec: "An embedded type must be specified as a type name T or as a
         * pointer to a non-interface type name *T, and T itself may not be a
         * pointer type." */
        TokenPos pos = ast_node_pos(f->type); /* position of type, for errors */
        AstIdent *name = tst_embedded_field_ident(f->type);
        if (name == NULL) {
            GT_ERRORF(check, burrow__tc_at_node(f->type), GT_CODE_INVALID_SYNTAX_TREE,
                      "embedded field type %s has no name",
                      burrow__tc_es(check, f->type));
            name = ast_new_ident(check->a, BURROW_S("_"));
            name->name_pos = pos;
            tst_add_invalid(&st, name);
            continue;
        }
        tst_add(&st, name, true); /* struct{p.T} field has position of T */

        /* Because we have a name, typ must be of the form T or *T, where T is
         * the name of a (named or alias) type, and t (= deref(typ)) must be the
         * type of T. We must delay this check to the end because we don't want
         * to instantiate (via t.Underlying()) a possibly incomplete type. */
        TstEmbedded *c = GT_NEW(check->a, TstEmbedded);
        c->typ = st.typ;
        c->pos = f->type;
        (void)burrow__tc_later(check, tst_embedded_later, c);
    }

    styp->fields = st.fields;
    styp->tags = st.tags;
    styp->complete = true;
}
