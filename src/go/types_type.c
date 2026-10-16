/* go/types: the types.
 *
 * Every type but the named ones, which have a file of their own: the basic
 * types, the composite type literals, signatures, interfaces, unions, type
 * parameters and aliases, and the lists of type parameters and type arguments.
 *
 * Derived from Go's src/go/types/basic.go, pointer.go, array.go, slice.go,
 * map.go, chan.go, struct.go, tuple.go, signature.go, interface.go, union.go,
 * typeparam.go, alias.go, typelists.go and type.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ memory */

void burrow__types_oom(void) {
    panic_str(BURROW_S("go/types: out of memory"));
}

void *burrow__types_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        burrow__types_oom();
    return p;
}

void burrow__types_assert_failed(void) {
    panic_str(BURROW_S("assertion failed"));
}

Slice burrow__types_ptrs_make(Alloc *a, Int n) {
    Slice s = slice_make(a, TYPE_UNSAFE_POINTER, n, n);
    if (n > 0 && s.p == NULL)
        burrow__types_oom();
    return s;
}

void burrow__types_ptrs_append(Alloc *a, Slice *s, void *p) {
    if (s->elem == NULL)
        s->elem = TYPE_UNSAFE_POINTER;
    Slice r = slice_append(a, *s, &p, 1);
    if (r.p == NULL)
        burrow__types_oom();
    *s = r;
}

/* s[i] of a pointer slice, bounds checked the way Go's index is. */
static void *gt_ptr_at(Slice s, Int i) {
    return *(void **)slice_at(s, i);
}

/* ------------------------------------------------------------- downcasts */

#define GT_AS(T, KIND) return t != NULL && t->kind == (KIND) ? (T *)(void *)t : NULL

TypesBasic *types_as_basic(TypesType t) {
    GT_AS(TypesBasic, TYPES_KIND_BASIC);
}
TypesPointer *types_as_pointer(TypesType t) {
    GT_AS(TypesPointer, TYPES_KIND_POINTER);
}
TypesArray *types_as_array(TypesType t) {
    GT_AS(TypesArray, TYPES_KIND_ARRAY);
}
TypesSlice *types_as_slice(TypesType t) {
    GT_AS(TypesSlice, TYPES_KIND_SLICE);
}
TypesStruct *types_as_struct(TypesType t) {
    GT_AS(TypesStruct, TYPES_KIND_STRUCT);
}
TypesTuple *types_as_tuple(TypesType t) {
    GT_AS(TypesTuple, TYPES_KIND_TUPLE);
}
TypesSignature *types_as_signature(TypesType t) {
    GT_AS(TypesSignature, TYPES_KIND_SIGNATURE);
}
TypesInterface *types_as_interface(TypesType t) {
    GT_AS(TypesInterface, TYPES_KIND_INTERFACE);
}
TypesMap *types_as_map(TypesType t) {
    GT_AS(TypesMap, TYPES_KIND_MAP);
}
TypesChan *types_as_chan(TypesType t) {
    GT_AS(TypesChan, TYPES_KIND_CHAN);
}
TypesNamed *types_as_named(TypesType t) {
    GT_AS(TypesNamed, TYPES_KIND_NAMED);
}
TypesTypeParam *types_as_type_param(TypesType t) {
    GT_AS(TypesTypeParam, TYPES_KIND_TYPE_PARAM);
}
TypesUnion *types_as_union(TypesType t) {
    GT_AS(TypesUnion, TYPES_KIND_UNION);
}
TypesAlias *types_as_alias(TypesType t) {
    GT_AS(TypesAlias, TYPES_KIND_ALIAS);
}

#undef GT_AS

TypesType types_type_underlying(TypesType t) {
    if (t == NULL)
        return NULL;
    switch ((TypesTypeKind)t->kind) {
    case TYPES_KIND_NAMED:
        return burrow__types_named_underlying(GT_KNOWN(TypesNamed, t));
    case TYPES_KIND_TYPE_PARAM:
        return types_type(burrow__types_type_param_iface(GT_KNOWN(TypesTypeParam, t)));
    case TYPES_KIND_ALIAS:
        return types_type_underlying(burrow__types_unalias(GT_KNOWN(TypesAlias, t)));
    case TYPES_KIND_INVALID:
    case TYPES_KIND_BASIC:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_ARRAY:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_STRUCT:
    case TYPES_KIND_TUPLE:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_INTERFACE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
    case TYPES_KIND_UNION:
    default:
        return t;
    }
}

/* ------------------------------------------------------------ basic types */

TypesBasicKind types_basic_kind(TypesBasic *b) {
    return b->kind;
}

TypesBasicInfo types_basic_info(TypesBasic *b) {
    return b->info;
}

Str types_basic_name(TypesBasic *b) {
    return b->name;
}

/* --------------------------------------------------- composite type literals */

TypesPointer *types_new_pointer(Alloc *a, TypesType elem) {
    TypesPointer *p = GT_NEW(a, TypesPointer);
    p->hdr.kind = TYPES_KIND_POINTER;
    p->base = elem;
    return p;
}

TypesType types_pointer_elem(TypesPointer *p) {
    return p->base;
}

TypesArray *types_new_array(Alloc *a, TypesType elem, int64_t len) {
    TypesArray *t = GT_NEW(a, TypesArray);
    t->hdr.kind = TYPES_KIND_ARRAY;
    t->len = len;
    t->elem = elem;
    return t;
}

int64_t types_array_len(TypesArray *t) {
    return t->len;
}

TypesType types_array_elem(TypesArray *t) {
    return t->elem;
}

TypesSlice *types_new_slice(Alloc *a, TypesType elem) {
    TypesSlice *s = GT_NEW(a, TypesSlice);
    s->hdr.kind = TYPES_KIND_SLICE;
    s->elem = elem;
    return s;
}

TypesType types_slice_elem(TypesSlice *s) {
    return s->elem;
}

TypesMap *types_new_map(Alloc *a, TypesType key, TypesType elem) {
    TypesMap *m = GT_NEW(a, TypesMap);
    m->hdr.kind = TYPES_KIND_MAP;
    m->key = key;
    m->elem = elem;
    return m;
}

TypesType types_map_key(TypesMap *m) {
    return m->key;
}

TypesType types_map_elem(TypesMap *m) {
    return m->elem;
}

TypesChan *types_new_chan(Alloc *a, TypesChanDir dir, TypesType elem) {
    TypesChan *c = GT_NEW(a, TypesChan);
    c->hdr.kind = TYPES_KIND_CHAN;
    c->dir = dir;
    c->elem = elem;
    return c;
}

TypesChanDir types_chan_dir(TypesChan *c) {
    return c->dir;
}

TypesType types_chan_elem(TypesChan *c) {
    return c->elem;
}

/* ----------------------------------------------------------------- structs */

TypesStruct *types_new_struct(Alloc *a, Slice fields, Slice tags) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    GtObjset fset = {arena_allocator(&ar), NULL};
    for (Int i = 0; i < fields.len; i++) {
        TypesVar *f = gt_ptr_at(fields, i);
        if (!str_eq(GT_OBJ(f)->name, BURROW_S("_")) &&
            burrow__types_objset_insert(&fset, types_object(f)) != NULL) {
            arena_free(&ar);
            panic_str(BURROW_S("multiple fields with the same name"));
        }
    }
    arena_free(&ar);
    if (tags.len > fields.len)
        panic_str(BURROW_S("more tags than fields"));
    TypesStruct *s = GT_NEW(a, TypesStruct);
    s->hdr.kind = TYPES_KIND_STRUCT;
    s->fields = fields;
    s->tags = tags;
    s->complete = true;
    return s;
}

Int types_struct_num_fields(TypesStruct *s) {
    return s->fields.len;
}

TypesVar *types_struct_field(TypesStruct *s, Int i) {
    return gt_ptr_at(s->fields, i);
}

Str types_struct_tag(TypesStruct *s, Int i) {
    if (i < s->tags.len)
        return *(Str *)slice_at(s->tags, i);
    return BURROW_STR_EMPTY;
}

/* ------------------------------------------------------------------ tuples */

TypesTuple *types_new_tuple(Alloc *a, Slice vars) {
    if (vars.len == 0)
        return NULL;
    TypesTuple *t = GT_NEW(a, TypesTuple);
    t->hdr.kind = TYPES_KIND_TUPLE;
    t->vars = vars;
    return t;
}

Int types_tuple_len(TypesTuple *t) {
    return t != NULL ? t->vars.len : 0;
}

TypesVar *types_tuple_at(TypesTuple *t, Int i) {
    if (t == NULL)
        panic_str(BURROW_S("runtime error: invalid memory address or nil pointer "
                           "dereference"));
    return gt_ptr_at(t->vars, i);
}

/* -------------------------------------------------------------- signatures */

/* The common slice type of the type set of the variadic parameter, Go's S. */
typedef struct GtVariadic {
    Alloc *a;
    TypesSlice *s;
} GtVariadic;

static bool gt_variadic_term(void *env, TypesType t, TypesType u) {
    GtVariadic *v = env;
    (void)u;
    if (t == NULL)
        return false;
    TypesSlice *s;
    if (burrow__types_is_basic(t, TYPES_IS_STRING))
        s = types_new_slice(v->a, types_type(burrow__types_universe_byte));
    else
        s = types_as_slice(types_type_underlying(t));
    if (v->s == NULL) {
        v->s = s;
    } else if (s == NULL || !types_identical(types_type(v->s), types_type(s))) {
        v->s = NULL;
        return false;
    }
    return true;
}

TypesSignature *types_new_signature_type(Alloc *a, TypesVar *recv,
                                         Slice recv_type_params, Slice type_params,
                                         TypesTuple *params, TypesTuple *results,
                                         bool variadic) {
    if (variadic) {
        Int n = types_tuple_len(params);
        if (n == 0)
            panic_str(BURROW_S("variadic function must have at least one parameter"));
        TypesType last = GT_OBJ(types_tuple_at(params, n - 1))->typ;
        GtVariadic v = {a, NULL};
        (void)burrow__types_all(last, BURROW_FN(GtTypePairFunc, gt_variadic_term, &v));
        if (v.s == NULL) {
            Str ls = types_type_string(a, last, TYPES_NO_QUALIFIER);
            panic_str(fmt_sprintf_v(
                a, "got %s, want variadic parameter of slice or string type", ls));
        }
    }
    TypesSignature *sig = GT_NEW(a, TypesSignature);
    sig->hdr.kind = TYPES_KIND_SIGNATURE;
    sig->recv = recv;
    sig->params = params;
    sig->results = results;
    sig->variadic = variadic;
    if (recv_type_params.len != 0) {
        if (recv == NULL)
            panic_str(BURROW_S("function with receiver type parameters must have a "
                               "receiver"));
        sig->rparams = burrow__types_bind_tparams(a, recv_type_params);
    }
    if (type_params.len != 0)
        sig->tparams = burrow__types_bind_tparams(a, type_params);
    return sig;
}

TypesSignature *types_new_signature(Alloc *a, TypesVar *recv, TypesTuple *params,
                                    TypesTuple *results, bool variadic) {
    return types_new_signature_type(a, recv, slice_nil(TYPE_UNSAFE_POINTER),
                                    slice_nil(TYPE_UNSAFE_POINTER), params, results,
                                    variadic);
}

TypesVar *types_signature_recv(TypesSignature *s) {
    return s->recv;
}

TypesTypeParamList *types_signature_type_params(TypesSignature *s) {
    return s->tparams;
}

TypesTypeParamList *types_signature_recv_type_params(TypesSignature *s) {
    return s->rparams;
}

TypesTuple *types_signature_params(TypesSignature *s) {
    return s->params;
}

TypesTuple *types_signature_results(TypesSignature *s) {
    return s->results;
}

bool types_signature_variadic(TypesSignature *s) {
    return s->variadic;
}

/* ------------------------------------------------------------- interfaces */

TypesInterface *burrow__types_new_interface(Alloc *a) {
    TypesInterface *t = GT_NEW(a, TypesInterface);
    t->hdr.kind = TYPES_KIND_INTERFACE;
    t->a = a;
    return t;
}

TypesInterface *types_new_interface_type(Alloc *a, Slice methods, Slice embeddeds) {
    if (methods.len == 0 && embeddeds.len == 0)
        return burrow__types_empty_interface;
    TypesInterface *typ = burrow__types_new_interface(a);
    for (Int i = 0; i < methods.len; i++) {
        TypesFunc *m = gt_ptr_at(methods, i);
        TypesSignature *sig = types_as_signature(GT_OBJ(m)->typ);
        if (sig == NULL)
            panic_str(BURROW_S("interface conversion: types.Type is nil, not "
                               "*types.Signature"));
        if (sig->recv == NULL)
            sig->recv =
                burrow__types_new_var(a, TYPES_RECV_VAR, GT_OBJ(m)->pos, GT_OBJ(m)->pkg,
                                      BURROW_STR_EMPTY, types_type(typ));
    }
    burrow__types_sort_methods(methods);
    typ->methods = methods;
    typ->embeddeds = embeddeds;
    typ->complete = true;
    return typ;
}

TypesInterface *types_new_interface(Alloc *a, Slice methods, Slice embeddeds) {
    Slice tnames = burrow__types_ptrs_make(a, embeddeds.len);
    for (Int i = 0; i < embeddeds.len; i++)
        GT_AT(tnames, i, TypesType) = gt_ptr_at(embeddeds, i);
    return types_new_interface_type(a, methods, tnames);
}

GtTypeSet *burrow__types_iface_type_set(TypesInterface *t) {
    return burrow__types_compute_interface_type_set(TOKEN_NO_POS, t);
}

void burrow__types_interface_cleanup(TypesInterface *t) {
    /* any interface that escapes type checking must be safe for concurrent use */
    (void)burrow__types_iface_type_set(t);
    t->check = NULL;
    t->embed_pos = slice_nil(TYPE_INT);
}

void types_interface_mark_implicit(TypesInterface *t) {
    t->implicit = true;
}

Int types_interface_num_explicit_methods(TypesInterface *t) {
    return t->methods.len;
}

TypesFunc *types_interface_explicit_method(TypesInterface *t, Int i) {
    return gt_ptr_at(t->methods, i);
}

Int types_interface_num_embeddeds(TypesInterface *t) {
    return t->embeddeds.len;
}

TypesNamed *types_interface_embedded(TypesInterface *t, Int i) {
    return burrow__types_as_named(gt_ptr_at(t->embeddeds, i));
}

TypesType types_interface_embedded_type(TypesInterface *t, Int i) {
    return gt_ptr_at(t->embeddeds, i);
}

Int types_interface_num_methods(TypesInterface *t) {
    return burrow__types_iface_type_set(t)->methods.len;
}

TypesFunc *types_interface_method(TypesInterface *t, Int i) {
    return gt_ptr_at(burrow__types_iface_type_set(t)->methods, i);
}

bool types_interface_empty(TypesInterface *t) {
    return burrow__types_type_set_is_all(burrow__types_iface_type_set(t));
}

bool types_interface_is_comparable(TypesInterface *t) {
    return burrow__types_type_set_is_comparable(burrow__types_iface_type_set(t), NULL);
}

bool types_interface_is_method_set(TypesInterface *t) {
    return burrow__types_type_set_is_method_set(burrow__types_iface_type_set(t));
}

bool types_interface_is_implicit(TypesInterface *t) {
    return t->implicit;
}

TypesInterface *types_interface_complete(TypesInterface *t) {
    if (!t->complete)
        t->complete = true;
    (void)burrow__types_iface_type_set(t);
    return t;
}

static int gt_compare_func(const void *x, const void *y) {
    return burrow__types_object_cmp(*(TypesObject const *)x, *(TypesObject const *)y);
}

void burrow__types_sort_methods(Slice list) {
    /* A stable insertion sort: method lists are short, and the order of
     * methods that compare equal is never looked at. */
    TypesObject *p = list.p;
    for (Int i = 1; i < list.len; i++) {
        TypesObject x = p[i];
        Int j = i;
        while (j > 0 && gt_compare_func(&p[j - 1], &x) > 0) {
            p[j] = p[j - 1];
            j--;
        }
        p[j] = x;
    }
}

/* -------------------------------------------------------- unions and terms */

TypesTerm *types_new_term(Alloc *a, bool tilde, TypesType typ) {
    TypesTerm *t = GT_NEW(a, TypesTerm);
    t->tilde = tilde;
    t->typ = typ;
    return t;
}

bool types_term_tilde(TypesTerm *t) {
    return t->tilde;
}

TypesType types_term_type(TypesTerm *t) {
    return t->typ;
}

Str types_term_string(Alloc *a, TypesTerm *t) {
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_write_term(&buf, t);
    return burrow__types_buf_done(&buf, a);
}

TypesUnion *types_new_union(Alloc *a, Slice terms) {
    if (terms.len == 0)
        panic_str(BURROW_S("empty union"));
    TypesUnion *u = GT_NEW(a, TypesUnion);
    u->hdr.kind = TYPES_KIND_UNION;
    u->terms = terms;
    return u;
}

Int types_union_len(TypesUnion *u) {
    return u->terms.len;
}

TypesTerm *types_union_term(TypesUnion *u, Int i) {
    return gt_ptr_at(u->terms, i);
}

/* -------------------------------------------------------- type parameters */

/* Go's lastID, the source of the ids of type parameters made outside the
 * checker. */
static SyncAtomicUint32 gt_last_id;

TypesTypeParam *types_new_type_param(Alloc *a, TypesTypeName *obj,
                                     TypesType constraint) {
    TypesTypeParam *t = GT_NEW(a, TypesTypeParam);
    t->hdr.kind = TYPES_KIND_TYPE_PARAM;
    t->a = a;
    t->id = sync_atomic_uint32_add(&gt_last_id, 1);
    t->obj = obj;
    t->index = -1;
    t->bound = constraint;
    if (GT_OBJ(obj)->typ == NULL)
        GT_OBJ(obj)->typ = types_type(t);
    if (constraint != NULL)
        (void)burrow__types_type_param_iface(t);
    return t;
}

TypesTypeParam *burrow__tc_new_type_param(TypesChecker *check, TypesTypeName *obj,
                                          TypesType constraint) {
    /* Always increment lastID, even if it is not used. */
    (void)sync_atomic_uint32_add(&gt_last_id, 1);
    TypesTypeParam *t = GT_NEW(check->a, TypesTypeParam);
    t->hdr.kind = TYPES_KIND_TYPE_PARAM;
    t->a = check->a;
    t->check = check;
    t->id = ++check->next_id;
    t->obj = obj;
    t->index = -1;
    t->bound = constraint;
    if (GT_OBJ(obj)->typ == NULL)
        GT_OBJ(obj)->typ = types_type(t);
    /* iface may mutate typ.bound, so we must ensure that iface() is called at
     * least once before the resulting TypeParam escapes. */
    burrow__tc_needs_cleanup(check, types_type(t));
    return t;
}

TypesTypeName *types_type_param_obj(TypesTypeParam *t) {
    return t->obj;
}

Int types_type_param_index(TypesTypeParam *t) {
    return t->index;
}

TypesType types_type_param_constraint(TypesTypeParam *t) {
    return t->bound;
}

void types_type_param_set_constraint(TypesTypeParam *t, TypesType bound) {
    if (bound == NULL)
        panic_str(BURROW_S("nil constraint"));
    t->bound = bound;
    (void)burrow__types_type_param_iface(t);
}

TypesInterface *burrow__types_type_param_iface(TypesTypeParam *t) {
    TypesType bound = t->bound;
    if (bound == NULL)
        panic_str(BURROW_S("runtime error: invalid memory address or nil pointer "
                           "dereference"));
    TypesInterface *ityp = NULL;
    TypesType u = types_type_underlying(bound);
    if (u != NULL && u->kind == TYPES_KIND_BASIC) {
        if (!burrow__types_is_valid(u))
            return burrow__types_empty_interface;
    } else if (u != NULL && u->kind == TYPES_KIND_INTERFACE) {
        if (burrow__types_is_type_param(bound))
            return burrow__types_empty_interface;
        ityp = types_as_interface(u);
    }
    if (ityp == NULL) {
        Slice emb = burrow__types_ptrs_make(t->a, 1);
        GT_AT(emb, 0, TypesType) = bound;
        ityp = types_new_interface_type(t->a, slice_nil(TYPE_UNSAFE_POINTER), emb);
        ityp->implicit = true;
        t->bound = types_type(ityp);
    }
    if (ityp->tset == NULL) {
        TokenPos pos = GT_OBJ(t->obj)->pos;
        TypesNamed *n = burrow__types_as_named(bound);
        if (n != NULL)
            pos = GT_OBJ(n->obj)->pos;
        (void)burrow__types_compute_interface_type_set(pos, ityp);
    }
    return ityp;
}

void burrow__types_type_param_cleanup(TypesTypeParam *t) {
    (void)burrow__types_type_param_iface(t);
    t->check = NULL;
}

/* ------------------------------------------------------------- type lists */

Slice burrow__types_tparam_list(TypesTypeParamList *l) {
    if (l == NULL)
        return slice_nil(TYPE_UNSAFE_POINTER);
    return l->tparams;
}

Int types_type_param_list_len(TypesTypeParamList *l) {
    return burrow__types_tparam_list(l).len;
}

TypesTypeParam *types_type_param_list_at(TypesTypeParamList *l, Int i) {
    return gt_ptr_at(burrow__types_tparam_list(l), i);
}

Str types_type_param_list_string(Alloc *a, TypesTypeParamList *l) {
    BytesBuffer buf = BYTES_BUFFER(a);
    Slice list = burrow__types_tparam_list(l);
    burrow__types_buf_byte(&buf, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        types_write_type(&buf, GT_AT(list, i, TypesType), TYPES_NO_QUALIFIER);
    }
    burrow__types_buf_byte(&buf, ']');
    return burrow__types_buf_done(&buf, a);
}

Slice burrow__types_type_list(TypesTypeList *l) {
    if (l == NULL)
        return slice_nil(TYPE_UNSAFE_POINTER);
    return l->types;
}

TypesTypeList *burrow__types_new_type_list(Alloc *a, Slice list) {
    if (list.len == 0)
        return NULL;
    TypesTypeList *l = GT_NEW(a, TypesTypeList);
    l->types = list;
    return l;
}

Int types_type_list_len(TypesTypeList *l) {
    return burrow__types_type_list(l).len;
}

TypesType types_type_list_at(TypesTypeList *l, Int i) {
    return gt_ptr_at(burrow__types_type_list(l), i);
}

Str types_type_list_string(Alloc *a, TypesTypeList *l) {
    BytesBuffer buf = BYTES_BUFFER(a);
    Slice list = burrow__types_type_list(l);
    burrow__types_buf_byte(&buf, '[');
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        types_write_type(&buf, GT_AT(list, i, TypesType), TYPES_NO_QUALIFIER);
    }
    burrow__types_buf_byte(&buf, ']');
    return burrow__types_buf_done(&buf, a);
}

TypesTypeParamList *burrow__types_bind_tparams(Alloc *a, Slice list) {
    if (list.len == 0)
        return NULL;
    for (Int i = 0; i < list.len; i++) {
        TypesTypeParam *typ = gt_ptr_at(list, i);
        if (typ->index >= 0)
            panic_str(BURROW_S("type parameter bound more than once"));
        typ->index = i;
    }
    TypesTypeParamList *l = GT_NEW(a, TypesTypeParamList);
    l->tparams = list;
    return l;
}

/* ----------------------------------------------------------------- aliases */

TypesAlias *burrow__types_new_alias(Alloc *a, TypesTypeName *obj, TypesType rhs) {
    TypesAlias *t = GT_NEW(a, TypesAlias);
    t->hdr.kind = TYPES_KIND_ALIAS;
    t->a = a;
    t->obj = obj;
    t->orig = t;
    t->from_rhs = rhs;
    if (GT_OBJ(obj)->typ == NULL)
        GT_OBJ(obj)->typ = types_type(t);
    return t;
}

TypesAlias *burrow__tc_new_alias(TypesChecker *check, TypesTypeName *obj,
                                 TypesType rhs) {
    TypesAlias *t = burrow__types_new_alias(check->a, obj, rhs);
    /* Ensure that a.actual is set at the end of type checking. */
    burrow__tc_needs_cleanup(check, types_type(t));
    return t;
}

TypesAlias *types_new_alias(Alloc *a, TypesTypeName *obj, TypesType rhs) {
    TypesAlias *t = burrow__types_new_alias(a, obj, rhs);
    (void)burrow__types_unalias(t);
    return t;
}

TypesAlias *burrow__types_new_alias_instance(TypesChecker *check, Alloc *a,
                                             TokenPos pos, TypesAlias *orig,
                                             Slice targs, TypesNamed *expanding,
                                             TypesContext *ctxt) {
    burrow__types_assert(targs.len > 0);
    TypesTypeName *obj = types_new_type_name(a, pos, GT_OBJ(orig->obj)->pkg,
                                             GT_OBJ(orig->obj)->name, NULL);
    Map *smap = burrow__types_make_subst_map(
        a, burrow__types_tparam_list(types_alias_type_params(orig)), targs);
    TypesType rhs =
        burrow__types_subst(check, a, pos, orig->from_rhs, smap, expanding, ctxt);
    TypesAlias *res = check != NULL ? burrow__tc_new_alias(check, obj, rhs)
                                    : burrow__types_new_alias(a, obj, rhs);
    res->orig = orig;
    res->tparams = orig->tparams;
    res->targs = burrow__types_new_type_list(a, targs);
    return res;
}

TypesTypeName *types_alias_obj(TypesAlias *a) {
    return a->orig->obj;
}

TypesAlias *types_alias_origin(TypesAlias *a) {
    return a->orig;
}

TypesTypeParamList *types_alias_type_params(TypesAlias *a) {
    return a->tparams;
}

void types_alias_set_type_params(TypesAlias *a, Slice tparams) {
    burrow__types_assert(a->targs == NULL);
    a->tparams = burrow__types_bind_tparams(a->a, tparams);
}

TypesTypeList *types_alias_type_args(TypesAlias *a) {
    return a->targs;
}

TypesType types_alias_rhs(TypesAlias *a) {
    return a->from_rhs;
}

TypesType types_unalias(TypesType t) {
    TypesAlias *a0 = types_as_alias(t);
    if (a0 != NULL)
        return burrow__types_unalias(a0);
    return t;
}

TypesType burrow__types_unalias(TypesAlias *a0) {
    if (a0->actual != NULL)
        return a0->actual;
    TypesType t = NULL;
    for (TypesAlias *a = a0; a != NULL; a = types_as_alias(t))
        t = a->from_rhs;
    a0->actual = t;
    return t;
}

TypesNamed *burrow__types_as_named(TypesType t) {
    return types_as_named(types_unalias(t));
}
