/* go/types: the universe.
 *
 * The basic types are static, so types_typ needs nothing made first. The
 * universe scope and package unsafe are made the first time anything asks for
 * them, in an arena of their own that lives as long as the program, which is
 * as long as Go's package variables live.
 *
 * Derived from Go's src/go/types/universe.go, and the empty interface, the
 * top type set and allTermlist from interface.go, typeset.go and
 * termlist.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include "types_internal.h"

#include <stdbool.h>

/* ------------------------------------------------------------ basic types */

#define GT_BASIC(k, info, name) {{TYPES_KIND_BASIC}, (k), (info), BURROW_S_INIT(name)}

static TypesBasic gt_typ[] = {
    GT_BASIC(TYPES_INVALID, 0, "invalid type"),
    GT_BASIC(TYPES_BOOL, TYPES_IS_BOOLEAN, "bool"),
    GT_BASIC(TYPES_INT, TYPES_IS_INTEGER, "int"),
    GT_BASIC(TYPES_INT8, TYPES_IS_INTEGER, "int8"),
    GT_BASIC(TYPES_INT16, TYPES_IS_INTEGER, "int16"),
    GT_BASIC(TYPES_INT32, TYPES_IS_INTEGER, "int32"),
    GT_BASIC(TYPES_INT64, TYPES_IS_INTEGER, "int64"),
    GT_BASIC(TYPES_UINT, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uint"),
    GT_BASIC(TYPES_UINT8, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uint8"),
    GT_BASIC(TYPES_UINT16, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uint16"),
    GT_BASIC(TYPES_UINT32, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uint32"),
    GT_BASIC(TYPES_UINT64, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uint64"),
    GT_BASIC(TYPES_UINTPTR, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "uintptr"),
    GT_BASIC(TYPES_FLOAT32, TYPES_IS_FLOAT, "float32"),
    GT_BASIC(TYPES_FLOAT64, TYPES_IS_FLOAT, "float64"),
    GT_BASIC(TYPES_COMPLEX64, TYPES_IS_COMPLEX, "complex64"),
    GT_BASIC(TYPES_COMPLEX128, TYPES_IS_COMPLEX, "complex128"),
    GT_BASIC(TYPES_STRING, TYPES_IS_STRING, "string"),
    GT_BASIC(TYPES_UNSAFE_POINTER, 0, "Pointer"),
    GT_BASIC(TYPES_UNTYPED_BOOL, TYPES_IS_BOOLEAN | TYPES_IS_UNTYPED, "untyped bool"),
    GT_BASIC(TYPES_UNTYPED_INT, TYPES_IS_INTEGER | TYPES_IS_UNTYPED, "untyped int"),
    GT_BASIC(TYPES_UNTYPED_RUNE, TYPES_IS_INTEGER | TYPES_IS_UNTYPED, "untyped rune"),
    GT_BASIC(TYPES_UNTYPED_FLOAT, TYPES_IS_FLOAT | TYPES_IS_UNTYPED, "untyped float"),
    GT_BASIC(TYPES_UNTYPED_COMPLEX, TYPES_IS_COMPLEX | TYPES_IS_UNTYPED,
             "untyped complex"),
    GT_BASIC(TYPES_UNTYPED_STRING, TYPES_IS_STRING | TYPES_IS_UNTYPED,
             "untyped string"),
    GT_BASIC(TYPES_UNTYPED_NIL, TYPES_IS_UNTYPED, "untyped nil"),
};

TypesBasic *const types_typ[26] = {
    &gt_typ[0],  &gt_typ[1],  &gt_typ[2],  &gt_typ[3],  &gt_typ[4],  &gt_typ[5],
    &gt_typ[6],  &gt_typ[7],  &gt_typ[8],  &gt_typ[9],  &gt_typ[10], &gt_typ[11],
    &gt_typ[12], &gt_typ[13], &gt_typ[14], &gt_typ[15], &gt_typ[16], &gt_typ[17],
    &gt_typ[18], &gt_typ[19], &gt_typ[20], &gt_typ[21], &gt_typ[22], &gt_typ[23],
    &gt_typ[24], &gt_typ[25],
};

/* Go's basicAliases. */
static TypesBasic gt_byte =
    GT_BASIC(TYPES_BYTE, TYPES_IS_INTEGER | TYPES_IS_UNSIGNED, "byte");
static TypesBasic gt_rune = GT_BASIC(TYPES_RUNE, TYPES_IS_INTEGER, "rune");

TypesBasic *const burrow__types_universe_byte = &gt_byte;
TypesBasic *const burrow__types_universe_rune = &gt_rune;

#undef GT_BASIC

/* ------------------------------------------------- the type set of all types */

static TypesTerm gt_all_term = {false, NULL};
static TypesTerm *gt_all_terms[1] = {&gt_all_term};

Slice burrow__types_all_termlist(void) {
    Slice s = {gt_all_terms, 1, 1, TYPE_UNSAFE_POINTER};
    return s;
}

static GtTypeSet gt_top_type_set = {{NULL, 0, 0, TYPE_UNSAFE_POINTER},
                                    {gt_all_terms, 1, 1, TYPE_UNSAFE_POINTER},
                                    false};

GtTypeSet *const burrow__types_top_type_set = &gt_top_type_set;

static TypesInterface gt_empty_interface = {
    {TYPES_KIND_INTERFACE},
    NULL,
    NULL,
    {NULL, 0, 0, TYPE_UNSAFE_POINTER},
    {NULL, 0, 0, TYPE_UNSAFE_POINTER},
    {NULL, 0, 0, TYPE_UNSAFE_POINTER},
    false,
    true,
    &gt_top_type_set,
};

TypesInterface *const burrow__types_empty_interface = &gt_empty_interface;

/* --------------------------------------------------------------- builtins */

const GtBuiltinInfo burrow__types_predeclared_funcs[GT_NUM_BUILTINS] = {
    [GT_APPEND] = {BURROW_S_INIT("append"), 1, true, 0},
    [GT_CAP] = {BURROW_S_INIT("cap"), 1, false, 0},
    [GT_CLEAR] = {BURROW_S_INIT("clear"), 1, false, 1},
    [GT_CLOSE] = {BURROW_S_INIT("close"), 1, false, 1},
    [GT_COMPLEX] = {BURROW_S_INIT("complex"), 2, false, 0},
    [GT_COPY] = {BURROW_S_INIT("copy"), 2, false, 1},
    [GT_DELETE] = {BURROW_S_INIT("delete"), 2, false, 1},
    [GT_IMAG] = {BURROW_S_INIT("imag"), 1, false, 0},
    [GT_LEN] = {BURROW_S_INIT("len"), 1, false, 0},
    [GT_MAKE] = {BURROW_S_INIT("make"), 1, true, 0},
    [GT_MAX] = {BURROW_S_INIT("max"), 1, true, 0},
    [GT_MIN] = {BURROW_S_INIT("min"), 1, true, 0},
    [GT_NEW_] = {BURROW_S_INIT("new"), 1, false, 0},
    [GT_PANIC] = {BURROW_S_INIT("panic"), 1, false, 1},
    [GT_PRINT] = {BURROW_S_INIT("print"), 0, true, 1},
    [GT_PRINTLN] = {BURROW_S_INIT("println"), 0, true, 1},
    [GT_REAL] = {BURROW_S_INIT("real"), 1, false, 0},
    [GT_RECOVER] = {BURROW_S_INIT("recover"), 0, false, 1},
    [GT_ADD] = {BURROW_S_INIT("Add"), 2, false, 0},
    [GT_ALIGNOF] = {BURROW_S_INIT("Alignof"), 1, false, 0},
    [GT_OFFSETOF] = {BURROW_S_INIT("Offsetof"), 1, false, 0},
    [GT_SIZEOF] = {BURROW_S_INIT("Sizeof"), 1, false, 0},
    [GT_SLICE] = {BURROW_S_INIT("Slice"), 2, false, 0},
    [GT_SLICE_DATA] = {BURROW_S_INIT("SliceData"), 1, false, 0},
    [GT_STRING] = {BURROW_S_INIT("String"), 2, false, 0},
    [GT_STRING_DATA] = {BURROW_S_INIT("StringData"), 1, false, 0},
    [GT_ASSERT] = {BURROW_S_INIT("assert"), 1, false, 1},
    [GT_TRACE] = {BURROW_S_INIT("trace"), 0, true, 1},
};

/* --------------------------------------------------------------- universe */

static Arena gt_universe_arena;
static SyncOnce gt_universe_once;
static TypesScope *gt_universe;
static TypesPackage *gt_unsafe;

static Alloc *gt_ua(void) {
    return arena_allocator(&gt_universe_arena);
}

static void gt_def(TypesObject obj) {
    GtObject *o = GT_OBJ(obj);
    burrow__types_assert(o->typ != NULL);
    if (strings_contains(o->name, BURROW_S(" ")))
        return;
    TypesNamed *typ = burrow__types_as_named(o->typ);
    if (typ != NULL)
        typ->obj = types_as_type_name(obj);
    TypesScope *scope = gt_universe;
    if (burrow__types_is_exported(o->name)) {
        scope = gt_unsafe->scope;
        if (obj->kind == TYPES_OBJECT_TYPE_NAME || obj->kind == TYPES_OBJECT_BUILTIN)
            o->pkg = gt_unsafe;
        else
            panic_str(BURROW_S("unreachable"));
    }
    if (types_scope_insert(scope, obj) != NULL)
        panic_str(BURROW_S("double declaration of predeclared identifier"));
}

static TypesBuiltin *gt_new_builtin(Int id) {
    TypesBuiltin *b = GT_NEW(gt_ua(), TypesBuiltin);
    GtObject *o = GT_OBJ(b);
    o->hdr.kind = TYPES_OBJECT_BUILTIN;
    o->name = burrow__types_predeclared_funcs[id].name;
    o->typ = GT_TYP(TYPES_INVALID);
    b->id = id;
    return b;
}

static void gt_def_predeclared_types(void) {
    Alloc *a = gt_ua();
    for (Int i = 0; i < (Int)(sizeof gt_typ / sizeof gt_typ[0]); i++)
        gt_def(types_object(
            types_new_type_name(a, TOKEN_NO_POS, NULL, gt_typ[i].name, GT_TYP(i))));
    gt_def(types_object(types_new_type_name(a, TOKEN_NO_POS, NULL, gt_byte.name,
                                            types_type(&gt_byte))));
    gt_def(types_object(types_new_type_name(a, TOKEN_NO_POS, NULL, gt_rune.name,
                                            types_type(&gt_rune))));

    /* type error interface{ Error() string } */
    {
        TypesTypeName *obj =
            types_new_type_name(a, TOKEN_NO_POS, NULL, BURROW_S("error"), NULL);
        TypesNamed *typ = types_new_named(a, obj, NULL, slice_nil(TYPE_UNSAFE_POINTER));
        TypesVar *recv = burrow__types_new_var(a, TYPES_RECV_VAR, TOKEN_NO_POS, NULL,
                                               BURROW_STR_EMPTY, types_type(typ));
        TypesVar *res = burrow__types_new_var(a, TYPES_RESULT_VAR, TOKEN_NO_POS, NULL,
                                              BURROW_STR_EMPTY, GT_TYP(TYPES_STRING));
        Slice resv = burrow__types_ptrs_make(a, 1);
        GT_AT(resv, 0, TypesVar *) = res;
        TypesSignature *sig =
            types_new_signature(a, recv, NULL, types_new_tuple(a, resv), false);
        TypesFunc *err = types_new_func(a, TOKEN_NO_POS, NULL, BURROW_S("Error"), sig);
        TypesInterface *ityp = burrow__types_new_interface(a);
        ityp->methods = burrow__types_ptrs_make(a, 1);
        GT_AT(ityp->methods, 0, TypesFunc *) = err;
        ityp->complete = true;
        (void)burrow__types_compute_interface_type_set(NULL, TOKEN_NO_POS, ityp);
        typ->from_rhs = types_type(ityp);
        (void)burrow__types_named_underlying(typ);
        gt_def(types_object(obj));
    }

    /* type any = interface{} */
    {
        TypesTypeName *obj =
            types_new_type_name(a, TOKEN_NO_POS, NULL, BURROW_S("any"), NULL);
        (void)types_new_alias(a, obj, types_type(&gt_empty_interface));
        gt_def(types_object(obj));
    }

    /* type comparable interface{} // marked as comparable */
    {
        TypesTypeName *obj =
            types_new_type_name(a, TOKEN_NO_POS, NULL, BURROW_S("comparable"), NULL);
        TypesInterface *ityp = burrow__types_new_interface(a);
        ityp->complete = true;
        GtTypeSet *tset = GT_NEW(a, GtTypeSet);
        tset->methods = slice_nil(TYPE_UNSAFE_POINTER);
        tset->terms = burrow__types_all_termlist();
        tset->comparable = true;
        ityp->tset = tset;
        (void)types_new_named(a, obj, types_type(ityp), slice_nil(TYPE_UNSAFE_POINTER));
        gt_def(types_object(obj));
    }
}

static void gt_def_predeclared_consts(void) {
    Alloc *a = gt_ua();
    TypesConst *t =
        types_new_const(a, TOKEN_NO_POS, NULL, BURROW_S("true"),
                        GT_TYP(TYPES_UNTYPED_BOOL), constant_make_bool(true));
    TypesConst *f =
        types_new_const(a, TOKEN_NO_POS, NULL, BURROW_S("false"),
                        GT_TYP(TYPES_UNTYPED_BOOL), constant_make_bool(false));
    TypesConst *iota =
        types_new_const(a, TOKEN_NO_POS, NULL, BURROW_S("iota"),
                        GT_TYP(TYPES_UNTYPED_INT), constant_make_int64(0));
    gt_def(types_object(t));
    gt_def(types_object(f));
    gt_def(types_object(iota));
}

static void gt_def_predeclared_nil(void) {
    TypesNil *n = GT_NEW(gt_ua(), TypesNil);
    GT_OBJ(n)->hdr.kind = TYPES_OBJECT_NIL;
    GT_OBJ(n)->name = BURROW_S("nil");
    GT_OBJ(n)->typ = GT_TYP(TYPES_UNTYPED_NIL);
    gt_def(types_object(n));
}

static void gt_def_predeclared_funcs(void) {
    for (Int id = 0; id < GT_NUM_BUILTINS; id++) {
        if (id == GT_ASSERT || id == GT_TRACE)
            continue;
        gt_def(types_object(gt_new_builtin(id)));
    }
}

static void gt_universe_init(void *env) {
    (void)env;
    Alloc *a = gt_ua();
    gt_universe =
        types_new_scope(a, NULL, TOKEN_NO_POS, TOKEN_NO_POS, BURROW_S("universe"));
    gt_universe->universe = true;
    /* NewPackage("unsafe", "unsafe"), without asking for the universe that is
     * being made. */
    gt_unsafe = GT_NEW(a, TypesPackage);
    gt_unsafe->path = BURROW_S("unsafe");
    gt_unsafe->name = BURROW_S("unsafe");
    gt_unsafe->scope = types_new_scope(a, gt_universe, TOKEN_NO_POS, TOKEN_NO_POS,
                                       BURROW_S("package \"unsafe\""));
    gt_unsafe->complete = true;
    gt_def_predeclared_types();
    gt_def_predeclared_consts();
    gt_def_predeclared_nil();
    gt_def_predeclared_funcs();
}

TypesScope *types_universe(void) {
    sync_once_do(&gt_universe_once, BURROW_FN(Func, gt_universe_init, NULL));
    return gt_universe;
}

TypesPackage *types_unsafe(void) {
    (void)types_universe();
    return gt_unsafe;
}

void types_def_predeclared_test_funcs(void) {
    TypesScope *u = types_universe();
    if (types_scope_lookup(u, BURROW_S("assert")) != NULL)
        return;
    gt_def(types_object(gt_new_builtin(GT_ASSERT)));
    gt_def(types_object(gt_new_builtin(GT_TRACE)));
}

TypesType burrow__types_universe_error(void) {
    return types_object_type(types_scope_lookup(types_universe(), BURROW_S("error")));
}

TypesObject burrow__types_universe_any(void) {
    return types_scope_lookup(types_universe(), BURROW_S("any"));
}

TypesObject burrow__types_universe_comparable(void) {
    return types_scope_lookup(types_universe(), BURROW_S("comparable"));
}

TypesObject burrow__types_universe_iota(void) {
    return types_scope_lookup(types_universe(), BURROW_S("iota"));
}
