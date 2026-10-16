/* go/types: Eval and CheckExpr.
 *
 * Derived from Go's src/go/types/eval.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

TypesTypeAndValue types_eval(Alloc *a, TokenFileSet *fset, TypesPackage *pkg,
                             TokenPos pos, Str expr, Error *err) {
    TypesTypeAndValue tv = {0};

    /* parse expressions */
    Error perr = BURROW_NO_ERROR;
    AstExpr node = parser_parse_expr_from(a, fset, BURROW_S("eval"),
                                          BURROW_ANY(TYPE_STRING, &expr), 0, &perr);
    if (BURROW_FAILED(perr)) {
        if (err != NULL)
            *err = perr;
        return tv;
    }

    TypesInfo info = {0};
    info.types = map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0);
    if (info.types == NULL)
        burrow__types_oom();
    (void)types_check_expr(a, fset, pkg, pos, node, &info, err);
    const TypesTypeAndValue *got = map_get(info.types, &node);
    if (got != NULL)
        tv = *got;
    return tv;
}

/* The part of CheckExpr that can bail out, like tc_files_guarded. */
static void tev_guarded(TypesChecker *check, AstExpr expr) {
    check->bailing = false;
    check->bail_aliases.len = 0;
    BURROW_TRY {
        /* evaluate node */
        GtOperand x = {0};
        /* allow generic expressions */
        (void)burrow__tc_raw_expr(check, NULL, &x, expr, NULL, true);
        burrow__tc_process_delayed(check, 0); /* incl. all functions */
        burrow__tc_record_untyped(check);
    }
    BURROW_CATCH(r) {
        if (!check->bailing)
            panic(r);
        burrow__tc_bail_aliases(check);
    }
    BURROW_TRY_END;
    check->bailing = false;
}

bool types_check_expr(Alloc *a, TokenFileSet *fset, TypesPackage *pkg, TokenPos pos,
                      AstExpr expr, TypesInfo *info, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;

    /* determine scope */
    TypesScope *scope = NULL;
    if (pkg == NULL) {
        scope = types_universe();
        pos = TOKEN_NO_POS;
    } else if (!token_pos_is_valid(pos)) {
        scope = pkg->scope;
    } else {
        /* The package scope extent (position information) may be incorrect
         * (files spread across a wide range of fset positions) - ignore it and
         * just consider its children (file scopes). */
        Slice children = pkg->scope->children;
        for (Int i = 0; i < children.len; i++) {
            TypesScope *fscope = GT_AT(children, i, TypesScope *);
            scope = types_scope_innermost(fscope, pos);
            if (scope != NULL)
                break;
        }
        if (scope == NULL) {
            /* Not in Go: the walk up to pkg.scope that Go also does when
             * debugging is left out, since debug is off and scope is NULL. */
            if (err != NULL) {
                Str where =
                    token_position_string(token_file_set_position(fset, pos), a);
                Str name = pkg->name;
                *err =
                    errors_new(a, fmt_sprintf_v(a, "no position %s found in package %s",
                                                where, name));
            }
            return false;
        }
    }

    /* initialize checker */
    TypesChecker *check = types_new_checker(a, NULL, fset, pkg, info);
    check->env.scope = scope;
    check->env.expr_pos = pos;
    tev_guarded(check, expr);

    /* handleBailout: a normal return and a bailout both give the first
     * error. */
    if (!check->has_first_err)
        return true;
    if (err != NULL)
        *err = types_error_as_error(&check->first_err, a);
    return false;
}
