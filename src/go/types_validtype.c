/* go/types: detection of invalid recursive types.
 *
 * Derived from Go's src/go/types/validtype.go. Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/token.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* makeObjList returns the list of type name objects for the given list of
 * named types, from index start on. */
static Slice tvt_make_obj_list(Alloc *a, Slice tlist, Int start) {
    Slice olist = burrow__types_ptrs_make(a, tlist.len - start);
    for (Int i = start; i < tlist.len; i++)
        GT_AT(olist, i - start, TypesObject) =
            types_object(GT_AT(tlist, i, TypesNamed *)->obj);
    return olist;
}

/* append(s, t) for a nest or path list. */
static Slice tvt_append(Alloc *a, Slice s, TypesNamed *t) {
    burrow__types_ptrs_append(a, &s, t);
    return s;
}

static bool tvt_valid_type0(TypesChecker *check, TokenPos pos, TypesType typ,
                            Slice nest, Slice path);

/* The Named case of validType0. */
static bool tvt_valid_named(TypesChecker *check, TokenPos pos, TypesNamed *t,
                            Slice nest, Slice path) {
    /* If the current type t is also found in nest, (the memory of) t is
     * embedded in itself, indicating an invalid recursive type. */
    for (Int i = 0; i < nest.len; i++) {
        TypesNamed *e = GT_AT(nest, i, TypesNamed *);
        if (!types_identical(types_type(e), types_type(t)))
            continue;
        /* We have a cycle. If t != t.Origin() then t is an instance of the
         * generic type t.Origin(). Because t is in the nest, t must occur
         * within the definition (RHS) of the generic type t.Origin(), directly
         * or indirectly, after expansion of the RHS. Therefore t.Origin() must
         * be invalid, no matter how it is instantiated since the instantiation
         * t of t.Origin() happens inside t.Origin()'s RHS and thus is always
         * the same and always present. Therefore we can mark the underlying of
         * both t and t.Origin() as invalid. If t is not an instance of a
         * generic type, t and t.Origin() are the same. Furthermore, because we
         * check all types in a package for validity before type checking is
         * complete, any exported type that is invalid will have an invalid
         * underlying type and we can't reach here with such a type (invalid
         * types are excluded above). Thus, if we reach here with a type t,
         * both t and t.Origin() (if different in the first place) must be
         * from the current package; they cannot have been imported. Therefore
         * it is safe to change their underlying types; there is no chance for
         * a race condition (the types of the current package are not yet
         * available to other goroutines). */
        TypesNamed *orig = types_named_origin(t);
        burrow__types_assert(GT_OBJ(t->obj)->pkg == check->pkg);
        burrow__types_assert(GT_OBJ(orig->obj)->pkg == check->pkg);

        /* let t become invalid when it is unpacked */
        orig->from_rhs = GT_TYP(TYPES_INVALID);

        /* Find the starting point of the cycle and report it. Because each
         * type in nest must also appear in path (see invariant below), type t
         * must be in path since it was found in nest. But not every type in
         * path is in nest. Specifically t may appear in path with an earlier
         * index than the index of t in nest. Search again. */
        for (Int start = 0; start < path.len; start++) {
            TypesNamed *p = GT_AT(path, start, TypesNamed *);
            if (types_identical(types_type(p), types_type(t))) {
                burrow__tc_cycle_error(check, tvt_make_obj_list(check->a, path, start),
                                       0);
                return false;
            }
        }
        panic_str(BURROW_S("cycle start not found"));
    }

    /* No cycle was found. Check the RHS of t. Every type added to nest is also
     * added to path; thus every type that is in nest must also be in path
     * (invariant). But not every type in path is in nest, since nest may be
     * pruned (see below, *TypeParam case). */
    TypesNamed *orig = burrow__types_named_unpack(types_named_origin(t));
    return tvt_valid_type0(check, pos, burrow__types_named_rhs(orig),
                           tvt_append(check->a, nest, t),
                           tvt_append(check->a, path, t));
}

/* The TypeParam case of validType0. */
static bool tvt_valid_type_param(TypesChecker *check, TokenPos pos, TypesTypeParam *t,
                                 Slice nest, Slice path) {
    /* A type parameter stands for the type (argument) it was instantiated
     * with. Check the corresponding type argument for validity if we are in an
     * instantiated type. */
    Int d = nest.len - 1;
    if (d < 0)
        return true;
    TypesNamed *inst = GT_AT(nest, d, TypesNamed *); /* the type instance */
    /* Find the corresponding type argument for the type parameter and proceed
     * with checking that type argument. */
    Slice tparams = burrow__types_tparam_list(types_named_type_params(inst));
    TypesTypeList *targs = types_named_type_args(inst);
    for (Int i = 0; i < tparams.len; i++) {
        /* The type parameter and type argument lists should match in length
         * but be careful in case of errors. */
        if (t == GT_AT(tparams, i, TypesTypeParam *) &&
            i < types_type_list_len(targs)) {
            TypesType targ = types_type_list_at(targs, i);
            /* The type argument must be valid in the enclosing type (where
             * inst was instantiated), hence we must check targ's validity in
             * the type nest excluding the current (instantiated) type (see the
             * example in Go's validtype.go). For error reporting we keep the
             * full path. */
            Slice outer = nest;
            outer.len = d;
            bool res = tvt_valid_type0(check, pos, targ, outer, path);
            /* The check.validType0 call with nest[:d] may have overwritten the
             * entry at the current depth d. Restore the entry (was issue
             * go.dev/issue/66323). */
            GT_AT(nest, d, TypesNamed *) = inst;
            return res;
        }
    }
    return true;
}

/* validType0 checks if the given type is valid. If typ is a type parameter
 * its value is looked up in the type argument list of the instantiated
 * (enclosing) type, if it exists. Otherwise the type parameter must be from an
 * enclosing function and can be ignored. The nest list describes the stack
 * (the "nest in memory") of types which contain (or embed in the case of
 * interfaces) other types. For instance, a struct named S which contains a
 * field of named type F contains (the memory of) F in S, leading to the nest
 * S->F. If a type appears in its own nest (say S->F->S) we have an invalid
 * recursive type. The path list is the full path of named types in a cycle, it
 * is only needed for error reporting. */
static bool tvt_valid_type0(TypesChecker *check, TokenPos pos, TypesType typ,
                            Slice nest, Slice path) {
    typ = types_unalias(typ);
    if (typ == NULL)
        /* We should never see a nil type but be conservative and panic only in
         * debug mode. */
        return true;

    switch (typ->kind) {
    case TYPES_KIND_ARRAY:
        return tvt_valid_type0(check, pos, types_as_array(typ)->elem, nest, path);

    case TYPES_KIND_STRUCT: {
        Slice fields = types_as_struct(typ)->fields;
        for (Int i = 0; i < fields.len; i++) {
            TypesVar *f = GT_AT(fields, i, TypesVar *);
            if (!tvt_valid_type0(check, pos, GT_OBJ(f)->typ, nest, path))
                return false;
        }
        break;
    }

    case TYPES_KIND_UNION: {
        Slice terms = types_as_union(typ)->terms;
        for (Int i = 0; i < terms.len; i++) {
            TypesTerm *t = GT_AT(terms, i, TypesTerm *);
            if (!tvt_valid_type0(check, pos, t->typ, nest, path))
                return false;
        }
        break;
    }

    case TYPES_KIND_INTERFACE: {
        Slice embeddeds = types_as_interface(typ)->embeddeds;
        for (Int i = 0; i < embeddeds.len; i++) {
            if (!tvt_valid_type0(check, pos, GT_AT(embeddeds, i, TypesType), nest,
                                 path))
                return false;
        }
        break;
    }

    case TYPES_KIND_NAMED:
        /* Go keeps a disabled check.valids optimization here; see the TODO in
         * validtype.go (go.dev/issue/65711). */
        return tvt_valid_named(check, pos, types_as_named(typ), nest, path);

    case TYPES_KIND_TYPE_PARAM:
        return tvt_valid_type_param(check, pos, types_as_type_param(typ), nest, path);

    default:
        break;
    }
    return true;
}

/* validType verifies that the given type does not "expand" indefinitely
 * producing a cycle in the type graph. (Cycles involving alias types, as in
 * "type A = [10]A" are detected earlier, via the objDecl cycle detection
 * mechanism.) */
void burrow__tc_valid_type(TypesChecker *check, TypesNamed *typ) {
    (void)tvt_valid_type0(check, TOKEN_NO_POS, types_type(typ),
                          slice_nil(TYPE_UNSAFE_POINTER),
                          slice_nil(TYPE_UNSAFE_POINTER));
}
