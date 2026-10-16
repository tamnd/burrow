/* go/types: type-checking of interface types.
 *
 * Derived from Go's src/go/types/interface.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
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

TypesInterface *burrow__tc_new_interface(TypesChecker *check) {
    TypesInterface *typ = burrow__types_new_interface(check->a);
    typ->check = check;
    burrow__tc_needs_cleanup(check, types_type(typ));
    return typ;
}

static void tif_add_embedded(Alloc *a, TypesInterface *ityp, TokenPos pos,
                             TypesType typ) {
    burrow__types_ptrs_append(a, &ityp->embeddeds, typ);
    if (ityp->embed_pos.elem == NULL)
        ityp->embed_pos.elem = TYPE_INT;
    Slice r = slice_append(a, ityp->embed_pos, &pos, 1);
    if (r.p == NULL)
        burrow__types_oom();
    ityp->embed_pos = r;
}

/* What the later function of interfaceType closes over. */
typedef struct TifTypeSet {
    TokenPos pos;
    TypesInterface *ityp;
} TifTypeSet;

/* Compute type set as soon as possible to report any errors. Subsequent uses
 * of type sets will use this computed type set and won't need to pass in a
 * *Checker. */
static void tif_type_set_later(TypesChecker *check, void *env) {
    TifTypeSet *c = env;
    (void)burrow__types_compute_interface_type_set(check, c->pos, c->ityp);
}

/* The method of iface field f, added to ityp unless it is in error. */
static void tif_method(TypesChecker *check, TypesInterface *ityp, AstField *f,
                       TypesTypeName *def) {
    /* We have a method with name f.Names[0]. */
    AstIdent *name = GT_AT(f->names, 0, AstIdent *);
    if (str_eq(name->name, BURROW_S("_"))) {
        burrow__tc_error(check, GT_AT_NODE(name), GT_CODE_BLANK_IFACE_METHOD,
                         BURROW_S("methods must have a unique non-blank name"));
        return; /* ignore */
    }

    TypesType typ = burrow__tc_typ(check, f->type);
    TypesSignature *sig =
        typ->kind == TYPES_KIND_SIGNATURE ? types_as_signature(typ) : NULL;
    if (sig == NULL) {
        if (burrow__types_is_valid(typ))
            GT_ERRORF(check, burrow__tc_at_node(f->type), GT_CODE_INVALID_SYNTAX_TREE,
                      "%s is not a method signature", burrow__tc_ts(check, typ));
        return; /* ignore */
    }

    /* The go/parser doesn't accept interface method type parameters but an
     * ast.FuncType may have them. */
    if (sig->tparams != NULL) {
        AstNode at = f->type;
        if (f->type->kind == AST_KIND_FUNC_TYPE) {
            AstFuncType *ftyp = (AstFuncType *)(void *)f->type;
            if (ftyp->type_params != NULL)
                at = GT_NODE(ftyp->type_params);
        }
        burrow__tc_error(check, burrow__tc_at_node(at), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("interface methods cannot have type parameters"));
    }

    /* use named receiver type if available (for better error messages) */
    TypesType recv_typ = types_type(ityp);
    if (def != NULL) {
        TypesNamed *named = burrow__types_as_named(GT_OBJ(def)->typ);
        if (named != NULL)
            recv_typ = types_type(named);
    }
    TokenPos pos = ast_node_pos(GT_NODE(name));
    sig->recv = burrow__types_new_var(check->a, TYPES_RECV_VAR, pos, check->pkg,
                                      BURROW_STR_EMPTY, recv_typ);
    TypesFunc *m = types_new_func(check->a, pos, check->pkg, name->name, sig);
    burrow__tc_record_def(check, name, types_object(m));
    burrow__types_ptrs_append(check->a, &ityp->methods, m);
}

void burrow__tc_interface_type(TypesChecker *check, TypesInterface *ityp,
                               AstInterfaceType *iface, TypesTypeName *def) {
    Slice list = iface->methods->list;
    for (Int i = 0; i < list.len; i++) {
        AstField *f = GT_AT(list, i, AstField *);
        if (f->names.len == 0) {
            tif_add_embedded(check->a, ityp, ast_node_pos(f->type),
                             burrow__tc_parse_union(check, f->type));
            continue;
        }
        tif_method(check, ityp, f, def);
    }

    /* All methods and embedded elements for this interface are collected;
     * i.e., this interface may be used in a type set computation. */
    ityp->complete = true;

    if (ityp->methods.len == 0 && ityp->embeddeds.len == 0) {
        /* empty interface */
        ityp->tset = burrow__types_top_type_set;
        return;
    }

    /* sort for API stability (don't sort embeddeds: they must correspond to
     * *embedPos entries) */
    burrow__types_sort_methods(ityp->methods);

    TifTypeSet *c = GT_NEW(check->a, TifTypeSet);
    c->pos = ast_node_pos(GT_NODE(iface));
    c->ityp = ityp;
    (void)burrow__tc_later(check, tif_type_set_later, c);
}
