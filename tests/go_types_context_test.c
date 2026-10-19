/* Derived from Go's src/go/types/context_test.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)

static Slice one_type(Alloc *a, TypesBasicKind kind) {
    TypesType t = types_type(types_typ[kind]);
    return slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &t, 1);
}

static Slice one_tparam(Alloc *a, TypesTypeParam *tparam) {
    return slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &tparam, 1);
}

static void TestContextHashCollisions(TestingT *t) {
    /* Not in Go: Go skips this when its debug constant is on. burrow has no
     * such switch; it is always off, as Go's is by default. */

    /* Unit test the de-duplication fall-back logic in Context.
     *
     * We can't test this via Instantiate because this is only a fall-back in
     * case our hash is imperfect.
     *
     * These lookups and updates use reasonable looking types in an attempt to
     * make them robust to internal type assertions, but could equally well use
     * arbitrary types. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);
    TypesType empty = types_type(burrow__types_empty_interface);

    /* Create some distinct origin types. nullaryP and nullaryQ have no
     * parameters and are identical (but have different type parameter names).
     * unaryP has a parameter. */
    TypesType nullary_p, nullary_q, unary_p;
    {
        /* type nullaryP = func[P any]() */
        TypesTypeParam *tparam = types_new_type_param(
            a, types_new_type_name(a, TOKEN_NO_POS, NULL, S("P"), NULL), empty);
        nullary_p = types_type(types_new_signature_type(
            a, NULL, none, one_tparam(a, tparam), NULL, NULL, false));
    }
    {
        /* type nullaryQ = func[Q any]() */
        TypesTypeParam *tparam = types_new_type_param(
            a, types_new_type_name(a, TOKEN_NO_POS, NULL, S("Q"), NULL), empty);
        nullary_q = types_type(types_new_signature_type(
            a, NULL, none, one_tparam(a, tparam), NULL, NULL, false));
    }
    {
        /* type unaryP = func[P any](_ P) */
        TypesTypeParam *tparam = types_new_type_param(
            a, types_new_type_name(a, TOKEN_NO_POS, NULL, S("P"), NULL), empty);
        TypesVar *param =
            types_new_param(a, TOKEN_NO_POS, NULL, S("_"), types_type(tparam));
        TypesTuple *params = types_new_tuple(
            a, slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &param, 1));
        unary_p = types_type(types_new_signature_type(
            a, NULL, none, one_tparam(a, tparam), params, NULL, false));
    }

    TypesContext *ctxt = types_new_context(a);

    /* Update the context with an instantiation of nullaryP. */
    TypesType inst =
        types_type(types_new_signature_type(a, NULL, none, none, NULL, NULL, false));
    if (burrow__types_context_update(ctxt, S(""), nullary_p, one_type(a, TYPES_INT),
                                     inst) != inst)
        testing_t_error_v(t, "bad");

    /* unaryP is not identical to nullaryP, so we should not get inst when
     * instantiated with identical type arguments. */
    if (burrow__types_context_lookup(ctxt, S(""), unary_p, one_type(a, TYPES_INT)) !=
        NULL)
        testing_t_error_v(t, "bad");

    /* nullaryQ is identical to nullaryP, so we *should* get inst when
     * instantiated with identical type arguments. */
    if (burrow__types_context_lookup(ctxt, S(""), nullary_q, one_type(a, TYPES_INT)) !=
        inst)
        testing_t_error_v(t, "bad");

    /* ...but verify we don't get inst with different type arguments. */
    if (burrow__types_context_lookup(ctxt, S(""), nullary_q,
                                     one_type(a, TYPES_STRING)) != NULL)
        testing_t_error_v(t, "bad");
    arena_free(&ar);
}

#define TESTS(X) X(TestContextHashCollisions)

TESTING_MAIN(TESTS)
