/* go/types: named types, instances, substitution and contexts.
 *
 * A named type is set up in steps, the way Go's is: the type arguments of an
 * instance are substituted into the right hand side of its origin the first
 * time someone looks, and its methods one at a time when each is asked for.
 * Go also lets an importer load a named type lazily; nothing here does that
 * yet, so every named type is either declared whole or an instance.
 *
 * Derived from Go's src/go/types/named.go, subst.go, context.go and
 * instantiate.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static void *gt_named_at(Slice s, Int i) {
    return *(void **)slice_at(s, i);
}

/* ----------------------------------------------------------- named types */

static void gt_set_state(TypesNamed *n, uint32_t m) {
    sync_atomic_uint32_or(&n->state, m);
}

bool burrow__types_named_state_has(TypesNamed *n, uint32_t m) {
    return (sync_atomic_uint32_load(&n->state) & m) != 0;
}

static TypesNamed *gt_new_named(Alloc *a, TypesTypeName *obj, TypesType from_rhs,
                                Slice methods) {
    TypesNamed *typ = GT_NEW(a, TypesNamed);
    typ->hdr.kind = TYPES_KIND_NAMED;
    typ->a = a;
    typ->obj = obj;
    typ->from_rhs = from_rhs;
    typ->methods = methods;
    if (GT_OBJ(obj)->typ == NULL)
        GT_OBJ(obj)->typ = types_type(typ);
    return typ;
}

TypesNamed *types_new_named(Alloc *a, TypesTypeName *obj, TypesType underlying,
                            Slice methods) {
    if (burrow__types_as_named(underlying) != NULL)
        panic_str(BURROW_S("underlying type must not be *Named"));
    TypesNamed *n = gt_new_named(a, obj, underlying, methods);
    if (underlying == NULL)
        n->allow_nil_rhs = true;
    else
        types_named_set_underlying(n, underlying);
    return n;
}

static TypesType gt_expand_rhs(TypesNamed *n);

TypesNamed *burrow__types_named_unpack(TypesNamed *n) {
    if (burrow__types_named_state_has(n, GT_LAZY_LOADED | GT_UNPACKED))
        return n;
    sync_mutex_lock(&n->mu);
    if (burrow__types_named_state_has(n, GT_LAZY_LOADED | GT_UNPACKED)) {
        sync_mutex_unlock(&n->mu);
        return n;
    }
    if (n->inst != NULL) {
        burrow__types_assert(n->from_rhs == NULL);
        TypesNamed *orig = n->inst->orig;
        (void)burrow__types_named_unpack(orig);
        n->from_rhs = gt_expand_rhs(n);
        n->tparams = orig->tparams;
        if (orig->methods.len == 0) {
            gt_set_state(n, GT_LAZY_LOADED | GT_UNPACKED | GT_HAS_METHODS);
            n->inst->ctxt = NULL;
        } else {
            gt_set_state(n, GT_LAZY_LOADED | GT_UNPACKED);
        }
        burrow__types_assert(!burrow__types_named_state_has(n, GT_HAS_UNDER));
        sync_mutex_unlock(&n->mu);
        return n;
    }
    gt_set_state(n, GT_LAZY_LOADED | GT_UNPACKED | GT_HAS_METHODS);
    burrow__types_assert(!burrow__types_named_state_has(n, GT_HAS_UNDER));
    sync_mutex_unlock(&n->mu);
    return n;
}

TypesNamed *burrow__types_new_named_instance(Alloc *a, TokenPos pos, TypesNamed *orig,
                                             Slice targs, TypesNamed *expanding) {
    burrow__types_assert(targs.len > 0);
    TypesTypeName *obj = types_new_type_name(a, pos, GT_OBJ(orig->obj)->pkg,
                                             GT_OBJ(orig->obj)->name, NULL);
    GtInstance *inst = GT_NEW(a, GtInstance);
    inst->orig = orig;
    inst->targs = burrow__types_new_type_list(a, targs);
    if (expanding != NULL &&
        GT_OBJ(types_named_obj(expanding))->pkg == GT_OBJ(obj)->pkg)
        inst->ctxt = expanding->inst->ctxt;
    TypesNamed *typ = GT_NEW(a, TypesNamed);
    typ->hdr.kind = TYPES_KIND_NAMED;
    typ->a = a;
    typ->obj = obj;
    typ->inst = inst;
    GT_OBJ(obj)->typ = types_type(typ);
    return typ;
}

TypesTypeName *types_named_obj(TypesNamed *t) {
    if (t->inst == NULL)
        return t->obj;
    return t->inst->orig->obj;
}

TypesNamed *types_named_origin(TypesNamed *t) {
    if (t->inst == NULL)
        return t;
    return t->inst->orig;
}

TypesTypeParamList *types_named_type_params(TypesNamed *t) {
    return burrow__types_named_unpack(t)->tparams;
}

void types_named_set_type_params(TypesNamed *t, Slice tparams) {
    burrow__types_assert(t->inst == NULL);
    burrow__types_named_unpack(t)->tparams = burrow__types_bind_tparams(t->a, tparams);
}

TypesTypeList *types_named_type_args(TypesNamed *t) {
    if (t->inst == NULL)
        return NULL;
    return t->inst->targs;
}

Int types_named_num_methods(TypesNamed *t) {
    return burrow__types_named_unpack(types_named_origin(t))->methods.len;
}

static TypesFunc *gt_expand_method(TypesNamed *t, Int i);

TypesFunc *types_named_method(TypesNamed *t, Int i) {
    (void)burrow__types_named_unpack(t);
    if (burrow__types_named_state_has(t, GT_HAS_METHODS))
        return gt_named_at(t->methods, i);
    burrow__types_assert(t->inst != NULL);
    TypesNamed *orig = t->inst->orig;
    sync_mutex_lock(&t->mu);
    if (t->methods.len != orig->methods.len) {
        burrow__types_assert(t->methods.len == 0);
        t->methods = burrow__types_ptrs_make(t->a, orig->methods.len);
    }
    if (gt_named_at(t->methods, i) == NULL) {
        burrow__types_assert(t->inst->ctxt != NULL);
        GT_AT(t->methods, i, TypesFunc *) = gt_expand_method(t, i);
        t->inst->expanded_methods++;
        if (t->inst->expanded_methods == orig->methods.len) {
            gt_set_state(t, GT_HAS_METHODS);
            t->inst->ctxt = NULL;
        }
    }
    TypesFunc *m = gt_named_at(t->methods, i);
    sync_mutex_unlock(&t->mu);
    return m;
}

static TypesFunc *gt_expand_method(TypesNamed *t, Int i) {
    TypesFunc *orig = types_named_method(t->inst->orig, i);
    burrow__types_assert(orig != NULL);
    TypesSignature *old_sig = types_as_signature(GT_OBJ(orig)->typ);
    Slice rtpars = burrow__types_tparam_list(old_sig->rparams);
    Slice rtargs = burrow__types_type_list(t->inst->targs);
    TypesSignature *new_sig = old_sig;
    if (rtpars.len == rtargs.len) {
        Map *smap = burrow__types_make_subst_map(t->a, rtpars, rtargs);
        new_sig = types_as_signature(burrow__types_subst(
            t->a, GT_OBJ(orig)->pos, types_type(old_sig), smap, t, NULL));
    }
    if (new_sig == old_sig) {
        TypesSignature *copy = GT_NEW(t->a, TypesSignature);
        *copy = *old_sig;
        new_sig = copy;
    }
    TypesType rtyp;
    if (burrow__types_func_has_ptr_recv(orig))
        rtyp = types_type(types_new_pointer(t->a, types_type(t)));
    else
        rtyp = types_type(t);
    new_sig->recv = burrow__types_clone_var(t->a, old_sig->recv, rtyp);
    new_sig->rparams = NULL;
    return burrow__types_clone_func(t->a, orig, types_type(new_sig));
}

void types_named_set_underlying(TypesNamed *t, TypesType u) {
    burrow__types_assert(t->inst == NULL);
    if (u == NULL)
        panic_str(BURROW_S("underlying type must not be nil"));
    if (burrow__types_as_named(u) != NULL)
        panic_str(BURROW_S("underlying type must not be *Named"));
    sync_mutex_lock(&t->mu);
    t->from_rhs = u;
    t->allow_nil_rhs = false;
    gt_set_state(t, GT_LAZY_LOADED | GT_UNPACKED | GT_HAS_METHODS);
    t->underlying = u;
    gt_set_state(t, GT_HAS_UNDER);
    sync_mutex_unlock(&t->mu);
}

void types_named_add_method(TypesNamed *t, TypesFunc *m) {
    burrow__types_assert(burrow__types_same_pkg(GT_OBJ(t->obj)->pkg, GT_OBJ(m)->pkg));
    burrow__types_assert(t->inst == NULL);
    (void)burrow__types_named_unpack(t);
    if (burrow__types_named_method_index(t, GT_OBJ(m)->name, false) < 0)
        burrow__types_ptrs_append(t->a, &t->methods, m);
}

Int burrow__types_named_method_index(TypesNamed *t, Str name, bool fold_case) {
    if (str_eq(name, BURROW_S("_")))
        return -1;
    for (Int i = 0; i < t->methods.len; i++) {
        Str mname = GT_OBJ(gt_named_at(t->methods, i))->name;
        if (fold_case ? strings_equal_fold(mname, name) : str_eq(mname, name))
            return i;
    }
    return -1;
}

TypesType burrow__types_named_rhs(TypesNamed *n) {
    return n->from_rhs;
}

static void gt_resolve_underlying(TypesNamed *n);

TypesType burrow__types_named_underlying(TypesNamed *n) {
    (void)burrow__types_named_unpack(n);
    if (burrow__types_named_rhs(n) == NULL) {
        burrow__types_assert(n->allow_nil_rhs);
        return NULL;
    }
    if (!burrow__types_named_state_has(n, GT_HAS_UNDER))
        gt_resolve_underlying(n);
    return n->underlying;
}

void burrow__types_named_cleanup(TypesNamed *n) {
    /* Instances can have a nil underlying at the end of type checking; they
     * will lazily expand it as needed. All other types must have one. */
    if (n->inst == NULL)
        (void)types_type_underlying(types_type(n));
    n->check = NULL;
}

static void gt_resolve_underlying(TypesNamed *n) {
    burrow__types_assert(
        burrow__types_named_state_has(n, GT_LAZY_LOADED | GT_UNPACKED));
    /* Go's path, as a list through the stack would not do here: its length is
     * the length of a chain of declarations. */
    Slice path = slice_nil(TYPE_UNSAFE_POINTER);
    TypesType u = NULL;
    for (TypesType rhs = types_type(n); u == NULL;) {
        switch ((TypesTypeKind)rhs->kind) {
        case TYPES_KIND_ALIAS:
            rhs = burrow__types_unalias(types_as_alias(rhs));
            break;
        case TYPES_KIND_NAMED: {
            TypesNamed *t = types_as_named(rhs);
            if (burrow__types_named_state_has(t, GT_HAS_UNDER)) {
                u = t->underlying;
                break;
            }
            burrow__types_ptrs_append(n->a, &path, t);
            (void)burrow__types_named_unpack(t);
            rhs = burrow__types_named_rhs(t);
            burrow__types_assert(rhs != NULL);
            break;
        }
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
        case TYPES_KIND_TYPE_PARAM:
        case TYPES_KIND_UNION:
        default:
            u = rhs;
            break;
        }
    }
    for (Int i = 0; i < path.len; i++) {
        TypesNamed *t = gt_named_at(path, i);
        sync_mutex_lock(&t->mu);
        if (!burrow__types_named_state_has(t, GT_HAS_UNDER)) {
            t->underlying = u;
            gt_set_state(t, GT_HAS_UNDER);
        }
        sync_mutex_unlock(&t->mu);
    }
}

TypesFunc *burrow__types_named_lookup_method(TypesNamed *n, TypesPackage *pkg, Str name,
                                             bool fold_case, Int *index) {
    (void)burrow__types_named_unpack(n);
    if (burrow__types_same_pkg(GT_OBJ(n->obj)->pkg, pkg) ||
        burrow__types_is_exported(name) || fold_case) {
        Int i =
            burrow__types_named_method_index(types_named_origin(n), name, fold_case);
        if (i >= 0) {
            if (index != NULL)
                *index = i;
            return types_named_method(n, i);
        }
    }
    if (index != NULL)
        *index = -1;
    return NULL;
}

static TypesType gt_expand_rhs(TypesNamed *n) {
    burrow__types_assert(!burrow__types_named_state_has(n, GT_UNPACKED));
    burrow__types_assert(
        burrow__types_named_state_has(n->inst->orig, GT_LAZY_LOADED | GT_UNPACKED));
    if (n->inst->ctxt == NULL)
        n->inst->ctxt = types_new_context(n->a);
    TypesContext *ctxt = n->inst->ctxt;
    TypesNamed *orig = n->inst->orig;
    Slice targs = burrow__types_type_list(n->inst->targs);
    Slice tpars = burrow__types_tparam_list(orig->tparams);
    if (targs.len != tpars.len)
        return GT_TYP(TYPES_INVALID);
    Str h = burrow__types_context_instance_hash(ctxt, types_type(orig), targs);
    TypesType u =
        burrow__types_context_update(ctxt, h, types_type(orig), targs, types_type(n));
    burrow__types_assert(u == types_type(n));
    Map *m = burrow__types_make_subst_map(n->a, tpars, targs);
    TypesType orig_rhs = burrow__types_named_rhs(orig);
    TypesType rhs =
        burrow__types_subst(n->a, GT_OBJ(n->obj)->pos, orig_rhs, m, n, NULL);
    TypesInterface *iface = types_as_interface(rhs);
    if (iface != NULL) {
        bool copied = false;
        Slice methods = burrow__types_replace_recv_type(
            n->a, iface->methods, types_type(orig), types_type(n), &copied);
        if (copied) {
            if (types_type(iface) == orig_rhs) {
                burrow__types_assert(iface->complete);
                TypesInterface *crafted = burrow__types_new_interface(n->a);
                crafted->complete = true;
                crafted->implicit = false;
                crafted->embeddeds = iface->embeddeds;
                iface = crafted;
            }
            iface->methods = methods;
            iface->tset = NULL;
            (void)burrow__types_iface_type_set(iface);
            return types_type(iface);
        }
    }
    return rhs;
}

TypesType burrow__types_safe_underlying(TypesType typ) {
    TypesNamed *t = burrow__types_as_named(typ);
    if (t != NULL)
        return t->underlying;
    return types_type_underlying(typ);
}

/* ------------------------------------------------------------ substitution */

Map *burrow__types_make_subst_map(Alloc *a, Slice tpars, Slice targs) {
    burrow__types_assert(tpars.len == targs.len);
    Map *proj = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, tpars.len);
    if (proj == NULL)
        burrow__types_oom();
    for (Int i = 0; i < tpars.len; i++) {
        void *tpar = gt_named_at(tpars, i);
        void *targ = gt_named_at(targs, i);
        if (!map_set(proj, &tpar, &targ))
            burrow__types_oom();
    }
    return proj;
}

static TypesType gt_smap_lookup(Map *m, TypesTypeParam *tpar) {
    TypesType *t = map_get(m, &tpar);
    if (t != NULL && *t != NULL)
        return *t;
    return types_type(tpar);
}

/* Go's subster. */
typedef struct GtSubster {
    Alloc *a;
    TokenPos pos;
    Map *smap;
    TypesNamed *expanding; /* the instance being expanded, or NULL */
    TypesContext *ctxt;
} GtSubster;

static TypesType gt_subst_typ(GtSubster *subst, TypesType typ);

TypesType burrow__types_subst(Alloc *a, TokenPos pos, TypesType typ, Map *smap,
                              TypesNamed *expanding, TypesContext *ctxt) {
    burrow__types_assert(expanding != NULL || ctxt != NULL);
    if (map_len(smap) == 0)
        return typ;
    if (typ != NULL && typ->kind == TYPES_KIND_BASIC)
        return typ;
    if (typ != NULL && typ->kind == TYPES_KIND_TYPE_PARAM)
        return gt_smap_lookup(smap, types_as_type_param(typ));
    GtSubster subst = {a, pos, smap, expanding, ctxt};
    return gt_subst_typ(&subst, typ);
}

/* Go's substList over a pointer slice: a copy of in with f applied, or a nil
 * slice when f changed nothing. */
typedef void *(*GtSubstFn)(GtSubster *subst, void *x);

static Slice gt_subst_list(GtSubster *subst, Slice in, GtSubstFn f) {
    Slice out = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < in.len; i++) {
        void *t = gt_named_at(in, i);
        void *u = f(subst, t);
        if (u != t) {
            if (out.p == NULL) {
                out = burrow__types_ptrs_make(subst->a, in.len);
                for (Int j = 0; j < in.len; j++)
                    GT_AT(out, j, void *) = gt_named_at(in, j);
            }
            GT_AT(out, i, void *) = u;
        }
    }
    return out;
}

static void *gt_subst_typ_fn(GtSubster *subst, void *x) {
    return (void *)gt_subst_typ(subst, x);
}

static void *gt_subst_var(GtSubster *subst, void *x) {
    TypesVar *v = x;
    if (v != NULL) {
        TypesType typ = gt_subst_typ(subst, GT_OBJ(v)->typ);
        if (typ != GT_OBJ(v)->typ)
            return burrow__types_clone_var(subst->a, v, typ);
    }
    return v;
}

static void *gt_subst_func(GtSubster *subst, void *x) {
    TypesFunc *f = x;
    if (f != NULL) {
        TypesType typ = gt_subst_typ(subst, GT_OBJ(f)->typ);
        if (typ != GT_OBJ(f)->typ)
            return burrow__types_clone_func(subst->a, f, typ);
    }
    return f;
}

static void *gt_subst_term(GtSubster *subst, void *x) {
    TypesTerm *t = x;
    TypesType typ = gt_subst_typ(subst, t->typ);
    if (typ != t->typ)
        return types_new_term(subst->a, t->tilde, typ);
    return t;
}

static TypesTuple *gt_subst_tuple(GtSubster *subst, TypesTuple *t) {
    if (t != NULL) {
        Slice vars = gt_subst_list(subst, t->vars, gt_subst_var);
        if (vars.p != NULL) {
            TypesTuple *r = GT_NEW(subst->a, TypesTuple);
            r->hdr.kind = TYPES_KIND_TUPLE;
            r->vars = vars;
            return r;
        }
    }
    return t;
}

static TypesType gt_subst_typ_or_nil(GtSubster *subst, TypesType typ) {
    if (typ == NULL)
        return GT_TYP(TYPES_INVALID);
    return gt_subst_typ(subst, typ);
}

static TypesType gt_subst_signature(GtSubster *subst, TypesSignature *t) {
    TypesTypeParamList *tparams = t->tparams;
    GtSubster fresh_subst;
    GtSubster *s = subst;
    Int n = types_type_param_list_len(tparams);
    if (n > 0) {
        TypesTypeParam *tp0 = types_type_param_list_at(tparams, 0);
        bool instantiating = map_get(subst->smap, &tp0) != NULL;
        if (!instantiating) {
            Slice list = burrow__types_tparam_list(tparams);
            Slice fresh = burrow__types_ptrs_make(subst->a, n);
            Map *smap = map_make(subst->a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER,
                                 map_len(subst->smap) + n);
            if (smap == NULL)
                burrow__types_oom();
            MapIter it = map_iter(subst->smap);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v))
                if (!map_set(smap, k, v))
                    burrow__types_oom();
            for (Int i = 0; i < n; i++) {
                TypesTypeParam *tp = gt_named_at(list, i);
                GtObject *o = GT_OBJ(tp->obj);
                TypesTypeName *tname =
                    types_new_type_name(subst->a, o->pos, o->pkg, o->name, NULL);
                TypesTypeParam *ftp = types_new_type_param(subst->a, tname, NULL);
                ftp->index = tp->index;
                GT_AT(fresh, i, TypesTypeParam *) = ftp;
                if (!map_set(smap, &tp, &ftp))
                    burrow__types_oom();
            }
            for (Int i = 0; i < n; i++) {
                TypesTypeParam *tp = gt_named_at(list, i);
                GT_AT(fresh, i, TypesTypeParam *)->bound =
                    burrow__types_subst(subst->a, subst->pos, tp->bound, smap,
                                        subst->expanding, subst->ctxt);
            }
            tparams = GT_NEW(subst->a, TypesTypeParamList);
            tparams->tparams = fresh;
            fresh_subst =
                (GtSubster){subst->a, subst->pos, smap, subst->expanding, subst->ctxt};
            s = &fresh_subst;
        }
    }
    TypesTuple *params = gt_subst_tuple(s, t->params);
    TypesTuple *results = gt_subst_tuple(s, t->results);
    if (params != t->params || results != t->results || tparams != t->tparams) {
        TypesSignature *sig = GT_NEW(subst->a, TypesSignature);
        sig->hdr.kind = TYPES_KIND_SIGNATURE;
        sig->rparams = t->rparams;
        sig->tparams = tparams;
        sig->recv = t->recv;
        sig->recvold = t->recvold;
        sig->params = params;
        sig->results = results;
        sig->variadic = t->variadic;
        return types_type(sig);
    }
    return types_type(t);
}

static TypesType gt_subst_interface(GtSubster *subst, TypesInterface *t) {
    Slice methods = gt_subst_list(subst, t->methods, gt_subst_func);
    Slice embeddeds = gt_subst_list(subst, t->embeddeds, gt_subst_typ_fn);
    if (methods.p == NULL && embeddeds.p == NULL)
        return types_type(t);
    if (methods.p == NULL)
        methods = t->methods;
    if (embeddeds.p == NULL)
        embeddeds = t->embeddeds;
    TypesInterface *iface = burrow__types_new_interface(subst->a);
    iface->embeddeds = embeddeds;
    iface->embed_pos = t->embed_pos;
    iface->implicit = t->implicit;
    burrow__types_assert(t->complete);
    iface->complete = t->complete;
    bool copied = false;
    iface->methods = burrow__types_replace_recv_type(subst->a, methods, types_type(t),
                                                     types_type(iface), &copied);
    (void)burrow__types_iface_type_set(iface);
    return types_type(iface);
}

static TypesType gt_subst_typ(GtSubster *subst, TypesType typ) {
    if (typ == NULL)
        panic_str(BURROW_S("nil typ"));
    switch ((TypesTypeKind)typ->kind) {
    case TYPES_KIND_BASIC:
        break;
    case TYPES_KIND_ALIAS: {
        TypesAlias *t = types_as_alias(typ);
        TypesAlias *orig = types_alias_origin(t);
        Int n = types_type_param_list_len(types_alias_type_params(orig));
        if (n == 0)
            return typ;
        if (types_type_list_len(t->targs) != n)
            return GT_TYP(TYPES_INVALID);
        Slice targs =
            gt_subst_list(subst, burrow__types_type_list(t->targs), gt_subst_typ_fn);
        if (targs.p != NULL)
            return types_type(burrow__types_new_alias_instance(
                subst->a, subst->pos, t->orig, targs, subst->expanding, subst->ctxt));
        break;
    }
    case TYPES_KIND_ARRAY: {
        TypesArray *t = types_as_array(typ);
        TypesType elem = gt_subst_typ_or_nil(subst, t->elem);
        if (elem != t->elem)
            return types_type(types_new_array(subst->a, elem, t->len));
        break;
    }
    case TYPES_KIND_SLICE: {
        TypesSlice *t = types_as_slice(typ);
        TypesType elem = gt_subst_typ_or_nil(subst, t->elem);
        if (elem != t->elem)
            return types_type(types_new_slice(subst->a, elem));
        break;
    }
    case TYPES_KIND_STRUCT: {
        TypesStruct *t = types_as_struct(typ);
        Slice fields = gt_subst_list(subst, t->fields, gt_subst_var);
        if (fields.p != NULL) {
            TypesStruct *s = GT_NEW(subst->a, TypesStruct);
            s->hdr.kind = TYPES_KIND_STRUCT;
            s->fields = fields;
            s->tags = t->tags;
            s->complete = true;
            return types_type(s);
        }
        break;
    }
    case TYPES_KIND_POINTER: {
        TypesPointer *t = types_as_pointer(typ);
        TypesType base = gt_subst_typ(subst, t->base);
        if (base != t->base)
            return types_type(types_new_pointer(subst->a, base));
        break;
    }
    case TYPES_KIND_TUPLE:
        return types_type(gt_subst_tuple(subst, types_as_tuple(typ)));
    case TYPES_KIND_SIGNATURE:
        return gt_subst_signature(subst, types_as_signature(typ));
    case TYPES_KIND_UNION: {
        Slice terms = gt_subst_list(subst, types_as_union(typ)->terms, gt_subst_term);
        if (terms.p != NULL)
            return types_type(types_new_union(subst->a, terms));
        break;
    }
    case TYPES_KIND_INTERFACE:
        return gt_subst_interface(subst, types_as_interface(typ));
    case TYPES_KIND_MAP: {
        TypesMap *t = types_as_map(typ);
        TypesType key = gt_subst_typ(subst, t->key);
        TypesType elem = gt_subst_typ(subst, t->elem);
        if (key != t->key || elem != t->elem)
            return types_type(types_new_map(subst->a, key, elem));
        break;
    }
    case TYPES_KIND_CHAN: {
        TypesChan *t = types_as_chan(typ);
        TypesType elem = gt_subst_typ(subst, t->elem);
        if (elem != t->elem)
            return types_type(types_new_chan(subst->a, t->dir, elem));
        break;
    }
    case TYPES_KIND_NAMED: {
        TypesNamed *t = types_as_named(typ);
        TypesNamed *orig = types_named_origin(t);
        Int n = types_type_param_list_len(types_named_type_params(orig));
        if (n == 0)
            return typ;
        if (types_type_list_len(types_named_type_args(t)) != n)
            return GT_TYP(TYPES_INVALID);
        Slice targs = gt_subst_list(
            subst, burrow__types_type_list(types_named_type_args(t)), gt_subst_typ_fn);
        if (targs.p != NULL)
            return burrow__types_instance(subst->a, subst->pos, types_type(orig), targs,
                                          subst->expanding, subst->ctxt);
        break;
    }
    case TYPES_KIND_TYPE_PARAM:
        return gt_smap_lookup(subst->smap, types_as_type_param(typ));
    case TYPES_KIND_INVALID:
    default:
        panic_str(BURROW_S("unreachable"));
    }
    return typ;
}

Slice burrow__types_replace_recv_type(Alloc *a, Slice in, TypesType old, TypesType new_,
                                      bool *copied) {
    Slice out = in;
    *copied = false;
    for (Int i = 0; i < in.len; i++) {
        TypesFunc *method = gt_named_at(in, i);
        TypesSignature *sig = types_func_signature(method);
        if (sig->recv != NULL && GT_OBJ(sig->recv)->typ == old) {
            if (!*copied) {
                out = burrow__types_ptrs_make(a, in.len);
                for (Int j = 0; j < in.len; j++)
                    GT_AT(out, j, void *) = gt_named_at(in, j);
                *copied = true;
            }
            TypesSignature *newsig = GT_NEW(a, TypesSignature);
            *newsig = *sig;
            newsig->recv = burrow__types_clone_var(a, sig->recv, new_);
            GT_AT(out, i, TypesFunc *) =
                burrow__types_clone_func(a, method, types_type(newsig));
        }
    }
    return out;
}

/* --------------------------------------------------------------- contexts */

TypesContext *types_new_context(Alloc *a) {
    TypesContext *ctxt = GT_NEW(a, TypesContext);
    ctxt->a = a;
    ctxt->type_map = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    ctxt->origin_ids = map_make(a, TYPE_UNSAFE_POINTER, TYPE_INT, 0);
    if (ctxt->type_map == NULL || ctxt->origin_ids == NULL)
        burrow__types_oom();
    return ctxt;
}

Str burrow__types_context_instance_hash(TypesContext *ctxt, TypesType orig,
                                        Slice targs) {
    burrow__types_assert(ctxt != NULL);
    burrow__types_assert(orig != NULL);
    BytesBuffer buf = BYTES_BUFFER(ctxt->a);
    GtTypeWriter h = burrow__types_new_type_hasher(&buf, ctxt);
    Str id = strconv_itoa(ctxt->a, burrow__types_context_get_id(ctxt, orig));
    burrow__types_tw_string(&h, id);
    burrow__types_tw_typ(&h, orig);
    if (targs.len > 0)
        burrow__types_tw_type_list(&h, targs);
    Str s = burrow__types_buf_done(&buf, ctxt->a);
    return strings_replace_all(ctxt->a, s, BURROW_S(" "), BURROW_S("#"));
}

TypesType burrow__types_context_lookup(TypesContext *ctxt, Str h, TypesType orig,
                                       Slice targs) {
    sync_mutex_lock(&ctxt->mu);
    GtCtxtEntry **head = map_get(ctxt->type_map, &h);
    for (GtCtxtEntry *e = head != NULL ? *head : NULL; e != NULL; e = e->next) {
        if (burrow__types_identical_instance(orig, targs, e->orig, e->targs)) {
            sync_mutex_unlock(&ctxt->mu);
            return e->instance;
        }
    }
    sync_mutex_unlock(&ctxt->mu);
    return NULL;
}

TypesType burrow__types_context_update(TypesContext *ctxt, Str h, TypesType orig,
                                       Slice targs, TypesType inst) {
    burrow__types_assert(inst != NULL);
    sync_mutex_lock(&ctxt->mu);
    GtCtxtEntry **head = map_get(ctxt->type_map, &h);
    GtCtxtEntry *last = NULL;
    for (GtCtxtEntry *e = head != NULL ? *head : NULL; e != NULL; e = e->next) {
        if (types_identical(inst, e->instance)) {
            sync_mutex_unlock(&ctxt->mu);
            return e->instance;
        }
        last = e;
    }
    GtCtxtEntry *n = GT_NEW(ctxt->a, GtCtxtEntry);
    n->orig = orig;
    n->targs = targs;
    n->instance = inst;
    if (last != NULL) {
        last->next = n;
    } else if (!map_set(ctxt->type_map, &h, &n)) {
        sync_mutex_unlock(&ctxt->mu);
        burrow__types_oom();
    }
    sync_mutex_unlock(&ctxt->mu);
    return inst;
}

Int burrow__types_context_get_id(TypesContext *ctxt, TypesType t) {
    sync_mutex_lock(&ctxt->mu);
    Int *p = map_get(ctxt->origin_ids, &t);
    Int id;
    if (p != NULL) {
        id = *p;
    } else {
        id = ctxt->next_id;
        if (!map_set(ctxt->origin_ids, &t, &id)) {
            sync_mutex_unlock(&ctxt->mu);
            burrow__types_oom();
        }
        ctxt->next_id++;
    }
    sync_mutex_unlock(&ctxt->mu);
    return id;
}

/* ---------------------------------------------------------- instantiation */

/* Go's validateTArgLen with a nil checker, which panics where the checker
 * would report an error. */
static void gt_validate_targ_len(Alloc *a, TokenPos pos, Str name, Int want, Int got) {
    const char *qual;
    if (got < want)
        qual = "not enough";
    else if (got > want)
        qual = "too many";
    else
        return;
    panic_str(fmt_sprintf_v(a, "%d: %s type arguments for type %s: have %d, want %d",
                            pos, qual, name, got, want));
}

TypesType burrow__types_instance(Alloc *a, TokenPos pos, TypesType orig, Slice targs,
                                 TypesNamed *expanding, TypesContext *ctxt) {
    TypesContext *ctxts[2];
    Str hashes[2];
    Int n = 0;
    if (expanding != NULL)
        ctxts[n++] = expanding->inst->ctxt;
    if (ctxt != NULL)
        ctxts[n++] = ctxt;
    burrow__types_assert(n > 0);
    for (Int i = 0; i < n; i++)
        hashes[i] = burrow__types_context_instance_hash(ctxts[i], orig, targs);
    TypesType res = NULL;
    for (Int i = 0; i < n; i++) {
        res = burrow__types_context_lookup(ctxts[i], hashes[i], orig, targs);
        if (res != NULL)
            goto update;
    }
    switch ((TypesTypeKind)orig->kind) {
    case TYPES_KIND_NAMED:
        res = types_type(burrow__types_new_named_instance(a, pos, types_as_named(orig),
                                                          targs, expanding));
        break;
    case TYPES_KIND_ALIAS: {
        TypesAlias *al = types_as_alias(orig);
        Int ntp = types_type_param_list_len(types_alias_type_params(al));
        gt_validate_targ_len(a, pos, GT_OBJ(al->obj)->name, ntp, targs.len);
        if (ntp == 0)
            return orig;
        res = types_type(
            burrow__types_new_alias_instance(a, pos, al, targs, expanding, ctxt));
        break;
    }
    case TYPES_KIND_SIGNATURE: {
        TypesSignature *sig = types_as_signature(orig);
        burrow__types_assert(expanding == NULL);
        burrow__types_assert(sig->rparams == NULL);
        burrow__types_assert(sig->tparams != NULL);
        Int ntp = types_type_param_list_len(sig->tparams);
        gt_validate_targ_len(a, pos, types_type_string(a, orig, TYPES_NO_QUALIFIER),
                             ntp, targs.len);
        if (ntp == 0)
            return orig;
        Map *smap = burrow__types_make_subst_map(
            a, burrow__types_tparam_list(sig->tparams), targs);
        TypesSignature *s =
            types_as_signature(burrow__types_subst(a, pos, orig, smap, NULL, ctxt));
        if (s == sig) {
            TypesSignature *copy = GT_NEW(a, TypesSignature);
            *copy = *s;
            s = copy;
        }
        s->tparams = NULL;
        res = types_type(s);
        break;
    }
    case TYPES_KIND_INVALID:
    case TYPES_KIND_BASIC:
    case TYPES_KIND_POINTER:
    case TYPES_KIND_ARRAY:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_STRUCT:
    case TYPES_KIND_TUPLE:
    case TYPES_KIND_INTERFACE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
    case TYPES_KIND_TYPE_PARAM:
    case TYPES_KIND_UNION:
    default: {
        Str s = types_type_string(a, orig, TYPES_NO_QUALIFIER);
        panic_str(fmt_sprintf_v(a, "%d: cannot instantiate %s", pos, s));
    }
    }
update:
    for (Int i = n - 1; i >= 0; i--)
        res = burrow__types_context_update(ctxts[i], hashes[i], orig, targs, res);
    return res;
}
