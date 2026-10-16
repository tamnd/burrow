/* go/types: direct cycles among type declarations, and complete types.
 *
 * Derived from Go's src/go/types/cycles.go. Go source: go1.27.1.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

static void tcy_set_idx(Map *path_idx, TypesTypeName *tname, Int i) {
    if (!map_set(path_idx, &tname, &i))
        burrow__types_oom();
}

/* directCycle checks if the declaration of the type given by tname contains a
 * direct cycle. A direct cycle exists if the path from tname's declaration's
 * RHS leads from type name to type name and eventually ends up on that path
 * again, via regular or alias declarations; in other words if there are no
 * type literals (or basic types) on the path, and the path doesn't end in an
 * undeclared object. If a cycle is detected, a cycle error is reported and the
 * type at the start of the cycle is marked as invalid.
 *
 * The path_idx map tracks which type names have been processed. An entry can
 * be in 1 of 3 states as used in a typical 3-state (white/grey/black) graph
 * marking algorithm for cycle detection:
 *
 *   - entry not found: tname has not been seen before (white)
 *   - value is >= 0  : tname has been seen but is not done (grey); the value is
 *                      the path index
 *   - value is <  0  : tname has been seen and is done (black)
 *
 * When directCycle returns, the path_idx entries for all type names on the
 * path that starts at tname are marked black, regardless of whether there was
 * a cycle. This ensures that a type name is traversed only once. */
static void tcy_direct_cycle(TypesChecker *check, TypesTypeName *tname, Map *path_idx) {
    Slice path = slice_nil(TYPE_UNSAFE_POINTER); /* of TypesTypeName * */
    for (;;) {
        const Int *start = map_get(path_idx, &tname);
        if (start != NULL && *start < 0) {
            /* tname is marked black - do not traverse it again. */
            break;
        }

        if (start != NULL) {
            /* tname is marked grey - we have a cycle on the path beginning at
             * start. Mark tname as invalid. */
            GT_OBJ(tname)->typ = GT_TYP(TYPES_INVALID);

            /* collect type names on cycle */
            Slice cycle = slice_sub(path, *start, path.len);
            burrow__tc_cycle_error(check, cycle, burrow__tc_first_in_src(cycle));
            break;
        }

        /* tname is marked white - mark it grey and add it to the path. */
        tcy_set_idx(path_idx, tname, path.len);
        burrow__types_ptrs_append(check->a, &path, tname);

        /* For direct cycle detection, we don't care about whether we have an
         * alias or not. If the associated type is not a name, we're at the end
         * of the path and we're done. */
        GtDeclInfo *d = burrow__tc_obj_info(check, types_object(tname));
        burrow__types_assert(d != NULL && d->tdecl != NULL);
        AstExpr rhs = d->tdecl->type;
        if (rhs == NULL || rhs->kind != AST_KIND_IDENT)
            break;

        /* Determine the RHS type. If it is not found in the package scope, we
         * either have an error (which will be reported later), or the type
         * exists elsewhere (universe scope, file scope via dot-import) and a
         * cycle is not possible in the first place. If it is not a type name,
         * we cannot have a direct cycle either. In all these cases we can
         * stop. */
        TypesTypeName *tname1 = types_as_type_name(
            types_scope_lookup(check->pkg->scope, ((AstIdent *)(void *)rhs)->name));
        if (tname1 == NULL)
            break;

        /* Otherwise, continue with the RHS. */
        tname = tname1;
    }

    /* Mark all traversed type names black. (ensure that path_idx doesn't
     * contain any grey entries upon returning) */
    for (Int i = 0; i < path.len; i++)
        tcy_set_idx(path_idx, GT_AT(path, i, TypesTypeName *), -1);
}

/* directCycles searches for direct cycles among package level type
 * declarations. See tcy_direct_cycle for details. */
void burrow__tc_direct_cycles(TypesChecker *check) {
    Map *path_idx = map_make(check->a, TYPE_UNSAFE_POINTER, TYPE_INT, 0);
    if (path_idx == NULL)
        burrow__types_oom();
    for (Int i = 0; i < check->obj_list.len; i++) {
        TypesTypeName *tname =
            types_as_type_name(GT_AT(check->obj_list, i, TypesObject));
        if (tname != NULL)
            tcy_direct_cycle(check, tname, path_idx);
    }
}

/* isComplete returns whether a type is complete (i.e. up to having an
 * underlying type). Incomplete types will panic if types_type_underlying is
 * called on them. */
bool burrow__tc_is_complete(TypesChecker *check, TypesType t) {
    for (;;) {
        TypesObject obj;
        TypesType rhs;
        if (t != NULL && t->kind == TYPES_KIND_ALIAS) {
            TypesAlias *a = (TypesAlias *)(void *)t;
            obj = types_object(a->obj);
            rhs = a->from_rhs;
        } else if (t != NULL && t->kind == TYPES_KIND_NAMED) {
            TypesNamed *n = (TypesNamed *)(void *)t;
            obj = types_object(n->obj);
            rhs = n->from_rhs;
        } else {
            return true;
        }

        const Int *i =
            check->obj_path_idx != NULL ? map_get(check->obj_path_idx, &obj) : NULL;
        if (i != NULL) {
            Slice cycle = slice_sub(check->obj_path, *i, check->obj_path.len);
            burrow__tc_cycle_error(check, cycle, burrow__tc_first_in_src(cycle));
            return false;
        }

        /* We must walk through names because we permit certain cycles of names.
         * Consider:
         *
         *   type A B
         *   type B [unsafe.Sizeof(A{})]int
         *
         * starting at B. At the site of A{}, A has no underlying type, and so a
         * cycle must be reported. */
        t = rhs;
    }
}
