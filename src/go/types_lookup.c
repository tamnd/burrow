/* go/types: field and method lookup, selections and method sets.
 *
 * A lookup walks the embedded fields of a struct a depth at a time. What it
 * only needs while it walks, the lists of embedded types and the named types
 * it has seen, comes from an arena of its own that it frees before it
 * returns, and what it gives back, the index path or the method set, comes
 * from the Alloc it is given.
 *
 * Derived from Go's src/go/types/lookup.go, selection.go and methodset.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

/* concat: list with i appended, in a new array that list doesn't share. */
static Slice gt_concat(Alloc *a, Slice list, Int i) {
    Slice t = slice_make(a, TYPE_INT, list.len + 1, list.len + 1);
    if (t.p == NULL)
        burrow__types_oom();
    if (list.len > 0)
        memcpy(t.p, list.p, (size_t)list.len * sizeof(Int));
    ((Int *)t.p)[list.len] = i;
    return t;
}

/* An index path copied into a, so that it outlives the scratch arena. A NULL
 * path stays NULL, which is how Go tells "not found" from "ambiguous". */
static Slice gt_index_out(Alloc *a, Slice index) {
    if (index.p == NULL)
        return slice_nil(TYPE_INT);
    Slice t = slice_make(a, TYPE_INT, index.len, index.len);
    if (t.p == NULL)
        burrow__types_oom();
    memcpy(t.p, index.p, (size_t)index.len * sizeof(Int));
    return t;
}

/* deref: the base of typ if it is a *Pointer, but not a named type whose
 * underlying type is one. */
TypesType burrow__types_deref(TypesType typ, bool *is_ptr) {
    TypesPointer *p = types_as_pointer(types_unalias(typ));
    if (p != NULL) {
        *is_ptr = true;
        /* p's base should never be NULL, but be conservative. */
        if (p->base == NULL)
            return GT_TYP(TYPES_INVALID);
        return p->base;
    }
    *is_ptr = false;
    return typ;
}

static bool gt_is_pointer(TypesType t) {
    return types_as_pointer(types_type_underlying(t)) != NULL;
}

/* methodIndex: the index of the method with a matching package and name, or
 * -1. */
static Int gt_method_index(Slice methods, TypesPackage *pkg, Str name, bool fold_case,
                           TypesFunc **m) {
    if (!str_eq(name, BURROW_S("_"))) {
        for (Int i = 0; i < methods.len; i++) {
            TypesFunc *f = GT_AT(methods, i, TypesFunc *);
            if (burrow__types_same_id(types_object(f), pkg, name, fold_case)) {
                *m = f;
                return i;
            }
        }
    }
    *m = NULL;
    return -1;
}

/* ---------------------------------------------------------- embedded types */

/* embeddedType */
typedef struct GtEmbedded {
    TypesType typ;
    Slice index;    /* embedded field indices, starting with index at depth 0 */
    bool indirect;  /* there was a pointer indirection on the path to this field */
    bool multiples; /* typ appears more than once at this depth */
} GtEmbedded;

typedef struct GtEmbList {
    GtEmbedded *p;
    Int len;
    Int cap;
} GtEmbList;

static void gt_emb_push(Alloc *a, GtEmbList *l, GtEmbedded e) {
    if (l->len == l->cap) {
        Int cap = l->cap == 0 ? 4 : l->cap * 2;
        GtEmbedded *p = burrow__types_alloc(a, (size_t)cap * sizeof(GtEmbedded),
                                            _Alignof(GtEmbedded));
        if (l->len > 0)
            memcpy(p, l->p, (size_t)l->len * sizeof(GtEmbedded));
        l->p = p;
        l->cap = cap;
    }
    l->p[l->len++] = e;
}

/* consolidateMultiples: entries with identical types become one, marked as
 * multiples. Go keeps a map from type to index with a scan for identical
 * types after it; a scan alone finds the same entries. */
static GtEmbList gt_consolidate_multiples(GtEmbList list) {
    if (list.len <= 1)
        return list;
    Int n = 0;
    for (Int i = 0; i < list.len; i++) {
        GtEmbedded e = list.p[i];
        Int found = -1;
        for (Int j = 0; j < n; j++) {
            if (list.p[j].typ == e.typ || types_identical(list.p[j].typ, e.typ)) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            list.p[found].multiples = true;
        } else {
            list.p[n++] = e;
        }
    }
    list.len = n;
    return list;
}

/* instanceLookup: the named types seen so far. Instances can be identical
 * without being the same pointer, so they are compared with Identical. */
typedef struct GtInstanceLookup {
    Slice seen; /* of TypesNamed * */
} GtInstanceLookup;

static TypesNamed *gt_instance_lookup(GtInstanceLookup *l, TypesNamed *inst) {
    TypesNamed *orig = types_named_origin(inst);
    for (Int i = 0; i < l->seen.len; i++) {
        TypesNamed *t = GT_AT(l->seen, i, TypesNamed *);
        if (types_named_origin(t) == orig &&
            types_identical(types_type(inst), types_type(t)))
            return t;
    }
    return NULL;
}

static void gt_instance_add(Alloc *a, GtInstanceLookup *l, TypesNamed *inst) {
    burrow__types_ptrs_append(a, &l->seen, inst);
}

/* ------------------------------------------------------------------ lookup */

/* lookupFieldOrMethodImpl. index comes from a, which is a scratch allocator
 * here. It won't find struct fields in the base types of named pointer
 * types, which is why only gt_lookup_field_or_method and missingMethod call
 * it. */
static TypesObject gt_lookup_impl(Alloc *a, TypesType T, bool addressable,
                                  TypesPackage *pkg, Str name, bool fold_case,
                                  Slice *index_out, bool *indirect_out) {
    /* WARNING: The code in this function is extremely subtle - do not modify
     * casually! */
    TypesObject obj = NULL;
    Slice index = slice_nil(TYPE_INT);
    bool indirect = false;
    *index_out = index;
    *indirect_out = false;

    if (str_eq(name, BURROW_S("_")))
        return NULL; /* blank fields/methods are never found */

    /* Importantly, we must not call Underlying before the call to deref below
     * (nor does deref call Underlying), as doing so could incorrectly result
     * in finding methods of the pointer base type when T is a (*Named) pointer
     * type. */
    bool is_ptr;
    TypesType typ = burrow__types_deref(T, &is_ptr);

    /* *typ where typ is an interface (incl. a type parameter) has no
     * methods. */
    if (is_ptr && types_as_interface(types_type_underlying(typ)) != NULL)
        return NULL;

    /* Start with typ as single entry at shallowest depth. */
    GtEmbList current = {0};
    gt_emb_push(a, &current, (GtEmbedded){typ, slice_nil(TYPE_INT), is_ptr, false});

    /* seen tracks named types that we have seen already. Used to avoid
     * endless searches in case of recursive types. */
    GtInstanceLookup seen = {slice_nil(TYPE_UNSAFE_POINTER)};

    /* search current depth */
    while (current.len > 0) {
        GtEmbList next = {0}; /* embedded types found at current depth */

        /* look for (pkg, name) in all types at current depth */
        for (Int ei = 0; ei < current.len; ei++) {
            GtEmbedded e = current.p[ei];
            typ = e.typ;

            /* If we have a named type, we may have associated methods. Look
             * for those first. */
            TypesNamed *named = burrow__types_as_named(typ);
            if (named != NULL) {
                if (gt_instance_lookup(&seen, named) != NULL) {
                    /* We have seen this type before, at a more shallow depth
                     * (note that multiples of this type at the current depth
                     * were consolidated before). The type at that depth
                     * shadows this same type at the current depth, so we can
                     * ignore this one. */
                    continue;
                }
                gt_instance_add(a, &seen, named);

                /* look for a matching attached method */
                Int i;
                TypesFunc *m =
                    burrow__types_named_lookup_method(named, pkg, name, fold_case, &i);
                if (m != NULL) {
                    /* potential match; caution: method may not have a proper
                     * signature yet */
                    index = gt_concat(a, e.index, i);
                    if (obj != NULL || e.multiples) {
                        *index_out = index;
                        return NULL; /* collision */
                    }
                    obj = types_object(m);
                    indirect = e.indirect;
                    continue; /* we can't have a matching field or interface method */
                }
            }

            TypesType u = types_type_underlying(typ);
            if (u == NULL)
                continue;
            if (u->kind == TYPES_KIND_STRUCT) {
                TypesStruct *t = GT_KNOWN(TypesStruct, u);
                /* look for a matching field and collect embedded types */
                for (Int i = 0; i < t->fields.len; i++) {
                    TypesVar *f = GT_AT(t->fields, i, TypesVar *);
                    if (burrow__types_same_id(types_object(f), pkg, name, fold_case)) {
                        burrow__types_assert(GT_OBJ(f)->typ != NULL);
                        index = gt_concat(a, e.index, i);
                        if (obj != NULL || e.multiples) {
                            *index_out = index;
                            return NULL; /* collision */
                        }
                        obj = types_object(f);
                        indirect = e.indirect;
                        continue; /* we can't have a matching interface method */
                    }
                    /* Collect embedded struct fields for searching the next
                     * lower depth, but only if we have not seen a match yet
                     * (if we have a match it is either the desired field or we
                     * have a name collision on the same depth; in either case
                     * we don't need to look further). Embedded fields are
                     * always of the form T or *T where T is a type name. If
                     * e.typ appeared multiple times at this depth, f.typ
                     * appears multiple times at the next depth. */
                    if (obj == NULL && f->embedded) {
                        bool fptr;
                        TypesType ft = burrow__types_deref(GT_OBJ(f)->typ, &fptr);
                        gt_emb_push(a, &next,
                                    (GtEmbedded){ft, gt_concat(a, e.index, i),
                                                 e.indirect || fptr, e.multiples});
                    }
                }
            } else if (u->kind == TYPES_KIND_INTERFACE) {
                /* look for a matching method (interface may be a type
                 * parameter) */
                GtTypeSet *tset =
                    burrow__types_iface_type_set(GT_KNOWN(TypesInterface, u));
                TypesFunc *m;
                Int i = gt_method_index(tset->methods, pkg, name, fold_case, &m);
                if (m != NULL) {
                    burrow__types_assert(GT_OBJ(m)->typ != NULL);
                    index = gt_concat(a, e.index, i);
                    if (obj != NULL || e.multiples) {
                        *index_out = index;
                        return NULL; /* collision */
                    }
                    obj = types_object(m);
                    indirect = e.indirect;
                }
            }
        }

        if (obj != NULL) {
            /* found a potential match
             * spec: "A method call x.m() is valid if the method set of (the
             * type of) x contains m and the argument list can be assigned to
             * the parameter list of m. If x is addressable and &x's method
             * set contains m, x.m() is shorthand for (&x).m()". */
            TypesFunc *f = types_as_func(obj);
            if (f != NULL) {
                /* determine if method has a pointer receiver */
                if (burrow__types_func_has_ptr_recv(f) && !indirect && !addressable) {
                    *indirect_out = true;
                    return NULL; /* pointer/addressable receiver required */
                }
            }
            *index_out = index;
            *indirect_out = indirect;
            return obj;
        }

        current = gt_consolidate_multiples(next);
    }

    return NULL; /* not found */
}

/* lookupFieldOrMethod, with the fold_case of Object.sameId. */
static TypesObject gt_lookup_field_or_method(Alloc *a, TypesType T, bool addressable,
                                             TypesPackage *pkg, Str name,
                                             bool fold_case, Slice *index,
                                             bool *indirect) {
    /* Methods cannot be associated to a named pointer type. (spec: "The type
     * denoted by T is called the receiver base type; it must not be a pointer
     * or interface type and it must be declared in the same package as the
     * method."). Thus, if we have a named pointer type, proceed with the
     * underlying pointer type but discard the result if it is a method since
     * we would not have found it for T (see also go.dev/issue/8590). */
    TypesNamed *t = burrow__types_as_named(T);
    if (t != NULL) {
        TypesPointer *p = types_as_pointer(types_type_underlying(types_type(t)));
        if (p != NULL) {
            TypesObject obj = gt_lookup_impl(a, types_type(p), false, pkg, name,
                                             fold_case, index, indirect);
            if (types_as_func(obj) != NULL) {
                *index = slice_nil(TYPE_INT);
                *indirect = false;
                return NULL;
            }
            return obj;
        }
    }

    /* Go would look for a field of a type parameter's common underlying type
     * here, but that is turned off (go.dev/issue/51576). */
    return gt_lookup_impl(a, T, addressable, pkg, name, fold_case, index, indirect);
}

TypesObject burrow__types_lookup_field_or_method(Alloc *a, TypesType T,
                                                 bool addressable, TypesPackage *pkg,
                                                 Str name, bool fold_case, Slice *index,
                                                 bool *indirect) {
    return gt_lookup_field_or_method(a, T, addressable, pkg, name, fold_case, index,
                                     indirect);
}

TypesObject types_lookup_field_or_method(Alloc *a, TypesType T, bool addressable,
                                         TypesPackage *pkg, Str name, Slice *index,
                                         bool *indirect) {
    if (T == NULL)
        panic_str(BURROW_S("LookupFieldOrMethod on nil type"));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice idx;
    bool ind;
    TypesObject obj = gt_lookup_field_or_method(arena_allocator(&ar), T, addressable,
                                                pkg, name, false, &idx, &ind);
    if (index != NULL)
        *index = gt_index_out(a, idx);
    if (indirect != NULL)
        *indirect = ind;
    arena_free(&ar);
    return obj;
}

/* -------------------------------------------------------------- selections */

struct TypesSelection {
    TypesSelectionKind kind;
    TypesType recv;  /* type of x */
    TypesObject obj; /* object denoted by x.f */
    Slice index;     /* path from x to x.f */
    bool indirect;   /* set if there was any pointer indirection on the path */
};

TypesSelection *burrow__types_new_selection(Alloc *a, TypesSelectionKind kind,
                                            TypesType recv, TypesObject obj,
                                            Slice index, bool indirect) {
    TypesSelection *s = GT_NEW(a, TypesSelection);
    s->kind = kind;
    s->recv = recv;
    s->obj = obj;
    s->index = index;
    s->indirect = indirect;
    return s;
}

TypesSelection *types_lookup_selection(Alloc *a, TypesType T, bool addressable,
                                       TypesPackage *pkg, Str name) {
    Slice index;
    bool indirect;
    TypesObject obj =
        types_lookup_field_or_method(a, T, addressable, pkg, name, &index, &indirect);
    if (obj == NULL)
        return NULL;
    TypesSelectionKind kind;
    switch (obj->kind) {
    case TYPES_OBJECT_FUNC:
        kind = TYPES_METHOD_VAL;
        break;
    case TYPES_OBJECT_VAR:
        kind = TYPES_FIELD_VAL;
        break;
    default:
        panic_str(BURROW_S("can't happen"));
    }
    return burrow__types_new_selection(a, kind, T, obj, index, indirect);
}

TypesSelectionKind types_selection_kind(TypesSelection *s) {
    return s->kind;
}

TypesType types_selection_recv(TypesSelection *s) {
    return s->recv;
}

TypesObject types_selection_obj(TypesSelection *s) {
    return s->obj;
}

TypesType types_selection_type(Alloc *a, TypesSelection *s) {
    switch (s->kind) {
    case TYPES_METHOD_VAL: {
        /* The type of x.f is a method with its receiver type set to the type
         * of x. */
        TypesSignature *sig = GT_NEW(a, TypesSignature);
        *sig = *types_as_signature(GT_OBJ(s->obj)->typ);
        TypesVar *recv = GT_NEW(a, TypesVar);
        *recv = *sig->recv;
        GT_OBJ(recv)->typ = s->recv;
        sig->recv = recv;
        return types_type(sig);
    }
    case TYPES_METHOD_EXPR: {
        /* The type of x.f is a function (without receiver) and an additional
         * first argument with the same type as x. */
        TypesSignature *sig = GT_NEW(a, TypesSignature);
        *sig = *types_as_signature(GT_OBJ(s->obj)->typ);
        TypesVar *arg0 = GT_NEW(a, TypesVar);
        *arg0 = *sig->recv;
        sig->recvold = sig->recv; /* stash receiver (for consistency with call.go) */
        sig->recv = NULL;
        GT_OBJ(arg0)->typ = s->recv;
        Slice params = slice_nil(TYPE_UNSAFE_POINTER);
        if (sig->params != NULL)
            params = sig->params->vars;
        Slice vars = burrow__types_ptrs_make(a, params.len + 1);
        GT_AT(vars, 0, TypesVar *) = arg0;
        for (Int i = 0; i < params.len; i++)
            GT_AT(vars, i + 1, TypesVar *) = GT_AT(params, i, TypesVar *);
        sig->params = types_new_tuple(a, vars);
        return types_type(sig);
    }
    case TYPES_FIELD_VAL:
    default:
        /* In all other cases, the type of x.f is the type of f. */
        return GT_OBJ(s->obj)->typ;
    }
}

Slice types_selection_index(TypesSelection *s) {
    return s->index;
}

bool types_selection_indirect(TypesSelection *s) {
    return s->indirect;
}

void types_write_selection(BytesBuffer *buf, TypesSelection *s, TypesQualifier qf) {
    Str k;
    switch (s->kind) {
    case TYPES_FIELD_VAL:
        k = BURROW_S("field ");
        break;
    case TYPES_METHOD_VAL:
        k = BURROW_S("method ");
        break;
    case TYPES_METHOD_EXPR:
        k = BURROW_S("method expr ");
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    burrow__types_buf_str(buf, k);
    burrow__types_buf_byte(buf, '(');
    types_write_type(buf, s->recv, qf);
    burrow__types_buf_str(buf, BURROW_S(") "));
    burrow__types_buf_str(buf, GT_OBJ(s->obj)->name);
    /* The types made for a method only live while it is written. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    TypesType T = types_selection_type(arena_allocator(&ar), s);
    if (s->kind == TYPES_FIELD_VAL) {
        burrow__types_buf_byte(buf, ' ');
        types_write_type(buf, T, qf);
    } else {
        types_write_signature(buf, types_as_signature(T), qf);
    }
    arena_free(&ar);
}

Str types_selection_string(Alloc *a, TypesSelection *s, TypesQualifier qf) {
    BytesBuffer buf = BYTES_BUFFER(a);
    types_write_selection(&buf, s, qf);
    return burrow__types_buf_done(&buf, a);
}

/* ---------------------------------------------------------- missing method */

/* Why missingMethod found a method missing. */
enum {
    GT_MM_OK,
    GT_MM_NOT_FOUND,
    GT_MM_WRONG_NAME,
    GT_MM_UNEXPORTED,
    GT_MM_WRONG_SIG,
    GT_MM_AMBIG_SEL,
    GT_MM_PTR_RECV,
    GT_MM_FIELD,
    GT_MM_NOINTERFACE
};

bool burrow__types_is_interface_ptr(TypesType T) {
    TypesPointer *p = types_as_pointer(types_type_underlying(T));
    return p != NULL && types_is_interface(p->base);
}

Str burrow__types_interface_ptr_error(Alloc *a, TypesQualifier qf, TypesType T) {
    burrow__types_assert(burrow__types_is_interface_ptr(T));
    TypesPointer *p = types_as_pointer(types_type_underlying(T));
    Str ts = types_type_string(a, T, qf);
    if (burrow__types_is_type_param(p->base))
        return fmt_sprintf_v(
            a, "type %s is pointer to type parameter, not type parameter", ts);
    return fmt_sprintf_v(a, "type %s is pointer to interface, not interface", ts);
}

Str burrow__types_func_string(Alloc *a, TypesQualifier qf, TypesFunc *f,
                              bool pkg_info) {
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_buf_str(&buf, GT_OBJ(f)->name);
    GtTypeWriter w = burrow__types_new_type_writer(&buf, qf);
    w.pkg_info = pkg_info;
    w.param_names = false;
    burrow__types_tw_signature(&w, types_as_signature(GT_OBJ(f)->typ));
    return burrow__types_buf_done(&buf, a);
}

/* The cause missingMethod gives for state, with types written as qf says. m
 * is the method of T, and f the method of V it found, if any. */
static Str gt_missing_method_cause(Alloc *a, TypesQualifier qf, int state, TypesType V,
                                   TypesType T, TypesFunc *m, TypesFunc *f) {
    Str name = GT_OBJ(m)->name;
    switch (state) {
    case GT_MM_NOT_FOUND:
        if (burrow__types_is_interface_ptr(V))
            return fmt_sprintf_v(a, "(%s)",
                                 burrow__types_interface_ptr_error(a, qf, V));
        if (burrow__types_is_interface_ptr(T))
            return fmt_sprintf_v(a, "(%s)",
                                 burrow__types_interface_ptr_error(a, qf, T));
        return fmt_sprintf_v(a, "(missing method %s)", name);
    case GT_MM_WRONG_NAME: {
        Str fs = burrow__types_func_string(a, qf, f, false);
        Str ms = burrow__types_func_string(a, qf, m, false);
        return fmt_sprintf_v(a, "(missing method %s)\n\t\thave %s\n\t\twant %s", name,
                             fs, ms);
    }
    case GT_MM_UNEXPORTED:
        return fmt_sprintf_v(a, "(unexported method %s)", name);
    case GT_MM_WRONG_SIG: {
        Str fs = burrow__types_func_string(a, qf, f, false);
        Str ms = burrow__types_func_string(a, qf, m, false);
        if (str_eq(fs, ms)) {
            /* Don't report "want Foo, have Foo". Add package information to
             * disambiguate (go.dev/issue/54258). */
            fs = burrow__types_func_string(a, qf, f, true);
            ms = burrow__types_func_string(a, qf, m, true);
        }
        if (str_eq(fs, ms)) {
            /* Still "want Foo, have Foo", most likely from different type
             * parameters with the same name (go.dev/issue/61685). Go only
             * says the signature is wrong. */
            return fmt_sprintf_v(a, "(wrong type for method %s)", name);
        }
        return fmt_sprintf_v(a, "(wrong type for method %s)\n\t\thave %s\n\t\twant %s",
                             name, fs, ms);
    }
    case GT_MM_AMBIG_SEL: {
        Str vs = types_type_string(a, V, qf);
        return fmt_sprintf_v(a, "(ambiguous selector %s.%s)", vs, name);
    }
    case GT_MM_PTR_RECV:
        return fmt_sprintf_v(a, "(method %s has pointer receiver)", name);
    case GT_MM_FIELD: {
        Str vs = types_type_string(a, V, qf);
        return fmt_sprintf_v(a, "(%s.%s is a field, not a method)", vs, name);
    }
    case GT_MM_NOINTERFACE:
        return fmt_sprintf_v(a, "(%s method is marked 'nointerface')", name);
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

TypesFunc *burrow__types_missing_method(TypesChecker *check, Alloc *a, TypesType V,
                                        TypesType T, bool is_static,
                                        bool (*equivalent)(TypesType x, TypesType y),
                                        bool *wrong_type, Str *cause) {
    /* T must be an interface */
    TypesInterface *ti = types_as_interface(types_type_underlying(T));
    burrow__types_assert(ti != NULL);
    Slice methods = burrow__types_iface_type_set(ti)->methods;
    *wrong_type = false;
    if (methods.len == 0)
        return NULL;

    int state = GT_MM_OK;
    TypesFunc *m = NULL; /* method on T we're trying to implement */
    TypesFunc *f = NULL; /* method on V, if found */

    /* Not in Go: the lookups use an arena of their own, except with a checker,
     * whose objDecl may bail out of this function and would leak it. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *la = check != NULL ? check->a : arena_allocator(&ar);
    TypesInterface *u = types_as_interface(types_type_underlying(V));
    if (u != NULL) {
        GtTypeSet *tset = burrow__types_iface_type_set(u);
        for (Int i = 0; i < methods.len; i++) {
            m = GT_AT(methods, i, TypesFunc *);
            (void)gt_method_index(tset->methods, GT_OBJ(m)->pkg, GT_OBJ(m)->name, false,
                                  &f);
            if (f == NULL) {
                if (!is_static)
                    continue;
                state = GT_MM_NOT_FOUND;
                break;
            }
            if (!equivalent(GT_OBJ(f)->typ, GT_OBJ(m)->typ)) {
                state = GT_MM_WRONG_SIG;
                break;
            }
        }
    } else {
        for (Int i = 0; i < methods.len; i++) {
            m = GT_AT(methods, i, TypesFunc *);
            Slice index;
            bool indirect;
            TypesObject obj = gt_lookup_impl(la, V, false, GT_OBJ(m)->pkg,
                                             GT_OBJ(m)->name, false, &index, &indirect);

            /* check if m is ambiguous, on *V, or on V with case-folding */
            if (obj == NULL) {
                if (index.p != NULL) {
                    state = GT_MM_AMBIG_SEL;
                } else if (indirect) {
                    state = GT_MM_PTR_RECV;
                } else {
                    state = GT_MM_NOT_FOUND;
                    obj = gt_lookup_impl(la, V, false, GT_OBJ(m)->pkg, GT_OBJ(m)->name,
                                         true /* fold case */, &index, &indirect);
                    f = types_as_func(obj);
                    if (f != NULL) {
                        state = GT_MM_WRONG_NAME;
                        /* If the names are equal, f must be unexported
                         * (otherwise the package wouldn't matter). */
                        if (str_eq(GT_OBJ(f)->name, GT_OBJ(m)->name))
                            state = GT_MM_UNEXPORTED;
                    }
                }
                break;
            }

            /* we must have a method (not a struct field) */
            f = types_as_func(obj);
            if (f == NULL) {
                state = GT_MM_FIELD;
                break;
            }
            /* methods may not have a fully set up signature yet */
            if (check != NULL)
                burrow__tc_obj_decl(check, types_object(f));

            if (f->nointerface) {
                state = GT_MM_NOINTERFACE;
                break;
            }
            if (!equivalent(GT_OBJ(f)->typ, GT_OBJ(m)->typ)) {
                state = GT_MM_WRONG_SIG;
                break;
            }
        }
    }
    arena_free(&ar);

    if (state == GT_MM_OK)
        return NULL;
    if (cause != NULL) {
        /* This method may be formatted in funcString below, so must have a
         * fully set up signature. */
        if (f != NULL && check != NULL)
            burrow__tc_obj_decl(check, types_object(f));
        if (check != NULL)
            a = check->a;
        *cause =
            gt_missing_method_cause(a, burrow__tc_qualifier(check), state, V, T, m, f);
    }
    *wrong_type = state == GT_MM_WRONG_SIG || state == GT_MM_PTR_RECV;
    return m;
}

TypesFunc *types_missing_method(TypesType V, TypesInterface *T, bool is_static,
                                bool *wrong_type) {
    bool wt;
    TypesFunc *m = burrow__types_missing_method(NULL, NULL, V, types_type(T), is_static,
                                                types_identical, &wt, NULL);
    if (wrong_type != NULL)
        *wrong_type = wt;
    return m;
}

/* derefStructPtr: the base of typ if typ is a pointer to a struct, else typ. */
TypesType burrow__types_deref_struct_ptr(TypesType typ) {
    TypesPointer *p = types_as_pointer(types_type_underlying(typ));
    if (p != NULL && types_as_struct(types_type_underlying(p->base)) != NULL)
        return p->base;
    return typ;
}

/* fieldPath returns the path of the field at the given index sequence,
 * starting at typ, as a string of the field names joined by ".". */
Str burrow__types_field_path(Alloc *a, TypesType typ, Slice index) {
    Slice names = slice_nil(TYPE_STRING);
    for (Int k = 0; k < index.len; k++) {
        TypesStruct *u =
            types_as_struct(types_type_underlying(burrow__types_deref_struct_ptr(typ)));
        if (u == NULL) {
            /* should not happen if index is valid for typ */
            break;
        }
        TypesVar *fld = types_struct_field(u, GT_AT(index, k, Int));
        names = slice_append(a, names, &GT_OBJ(fld)->name, 1);
        if (names.p == NULL)
            burrow__types_oom();
        typ = GT_OBJ(fld)->typ;
    }
    return strings_join(a, names, BURROW_S("."));
}

/* hasInvalidEmbeddedFields: whether T is a struct, or a pointer to one, with
 * an embedded field of invalid type somewhere in it. seen holds the structs
 * already looked at, by pointer, and is made on first use. */
static bool gt_has_invalid_embedded_fields(Alloc *a, TypesType T, Map **seen) {
    TypesStruct *S =
        types_as_struct(types_type_underlying(burrow__types_deref_struct_ptr(T)));
    if (S == NULL || (*seen != NULL && map_get(*seen, &S) != NULL))
        return false;
    if (*seen == NULL) {
        *seen = map_make(a, TYPE_UNSAFE_POINTER, TYPE_BOOL, 0);
        if (*seen == NULL)
            burrow__types_oom();
    }
    bool yes = true;
    if (!map_set(*seen, &S, &yes))
        burrow__types_oom();
    for (Int i = 0; i < S->fields.len; i++) {
        TypesVar *f = GT_AT(S->fields, i, TypesVar *);
        if (f->embedded && (!burrow__types_is_valid(GT_OBJ(f)->typ) ||
                            gt_has_invalid_embedded_fields(a, GT_OBJ(f)->typ, seen)))
            return true;
    }
    return false;
}

bool burrow__types_has_all_methods(TypesChecker *check, Alloc *a, TypesType V,
                                   TypesType T, bool is_static,
                                   bool (*equivalent)(TypesType x, TypesType y),
                                   Str *cause) {
    if (!burrow__types_is_valid(V))
        return true; /* we don't know anything about V, assume it implements T */
    bool wt;
    TypesFunc *m =
        burrow__types_missing_method(check, a, V, T, is_static, equivalent, &wt, cause);
    if (m == NULL)
        return true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Map *seen = NULL;
    bool bad = gt_has_invalid_embedded_fields(arena_allocator(&ar), V, &seen);
    arena_free(&ar);
    return bad;
}

/* assertableTo reports whether a value of type V can be asserted to have type
 * T. The underlying type of V must be an interface. If the result is false and
 * cause is not NULL, *cause describes the error. */
bool burrow__tc_assertable_to(TypesChecker *check, Alloc *a, TypesType V, TypesType T,
                              Str *cause) {
    /* no static check is required if T is an interface
     * spec: "If T is an interface type, x.(T) asserts that the
     *        dynamic type of x implements the interface T." */
    if (types_is_interface(T))
        return true;
    return burrow__types_has_all_methods(check, a, T, V, false, types_identical, cause);
}

/* ------------------------------------------------------------- method sets */

struct TypesMethodSet {
    Slice list; /* of TypesSelection *, sorted by Id */
};

static TypesMethodSet gt_empty_method_set;

/* Go's methodSet is a map[string]*Selection from an Id to a method, where a
 * NULL entry is a name collision. Here it is a Map whose keys are Ids made
 * in the scratch arena. */
static Map *gt_mset_make(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

static void gt_mset_put(Map *m, Str key, TypesSelection *sel) {
    if (!map_set(m, &key, &sel))
        burrow__types_oom();
}

/* methodSet.addOne */
static Map *gt_mset_add_one(Alloc *a, Map *s, TypesFunc *f, Slice index, bool indirect,
                            bool multiples) {
    if (s == NULL)
        s = gt_mset_make(a);
    Str key = types_object_id(a, types_object(f));
    if (!multiples) {
        if (map_get(s, &key) == NULL &&
            (indirect || !burrow__types_func_has_ptr_recv(f))) {
            gt_mset_put(s, key,
                        burrow__types_new_selection(a, TYPES_METHOD_VAL, NULL,
                                                    types_object(f), index, indirect));
            return s;
        }
    }
    gt_mset_put(s, key, NULL); /* collision */
    return s;
}

/* methodSet.add */
static Map *gt_mset_add(Alloc *a, Map *s, Slice list, Slice index, bool indirect,
                        bool multiples) {
    for (Int i = 0; i < list.len; i++)
        s = gt_mset_add_one(a, s, GT_AT(list, i, TypesFunc *), gt_concat(a, index, i),
                            indirect, multiples);
    return s;
}

/* Sorts a list of selections by the Ids of their objects, with an insertion
 * sort, as method sets are short. */
static void gt_sort_selections(Slice list) {
    TypesSelection **p = list.p;
    for (Int i = 1; i < list.len; i++) {
        TypesSelection *x = p[i];
        GtObject *ox = GT_OBJ(x->obj);
        Int j = i;
        while (j > 0) {
            GtObject *o = GT_OBJ(p[j - 1]->obj);
            if (burrow__types_id_cmp(o->pkg, o->name, ox->pkg, ox->name) <= 0)
                break;
            p[j] = p[j - 1];
            j--;
        }
        p[j] = x;
    }
}

TypesMethodSet *types_new_method_set(Alloc *a, TypesType T) {
    /* WARNING: The code in this function is extremely subtle - do not modify
     * casually! This function and lookupFieldOrMethod should be kept in
     * sync. */

    /* method set up to the current depth, allocated lazily */
    Map *base = NULL;

    /* Methods cannot be associated with a named pointer type. (spec: "The
     * type denoted by T is called the receiver base type; it must not be a
     * pointer or interface type and it must be declared in the same package
     * as the method."). */
    TypesNamed *tn = burrow__types_as_named(T);
    if (tn != NULL && gt_is_pointer(T))
        return &gt_empty_method_set;

    bool is_ptr;
    TypesType typ = burrow__types_deref(T, &is_ptr);

    /* *typ where typ is an interface has no methods. */
    if (is_ptr && types_is_interface(typ))
        return &gt_empty_method_set;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *sa = arena_allocator(&ar);

    /* Start with typ as single entry at shallowest depth. */
    GtEmbList current = {0};
    gt_emb_push(sa, &current, (GtEmbedded){typ, slice_nil(TYPE_INT), is_ptr, false});

    GtInstanceLookup seen = {slice_nil(TYPE_UNSAFE_POINTER)};

    /* collect methods at current depth */
    while (current.len > 0) {
        GtEmbList next = {0}; /* embedded types found at current depth */

        /* field and method sets at current depth, indexed by names (Id's),
         * and allocated lazily */
        Map *fset = NULL; /* we only care about the field names */
        Map *mset = NULL;

        for (Int ei = 0; ei < current.len; ei++) {
            GtEmbedded e = current.p[ei];
            typ = e.typ;

            /* If we have a named type, we may have associated methods. Look
             * for those first. */
            TypesNamed *named = burrow__types_as_named(typ);
            if (named != NULL) {
                if (gt_instance_lookup(&seen, named) != NULL) {
                    /* We have seen this type before, at a more shallow depth
                     * (note that multiples of this type at the current depth
                     * were consolidated before). The type at that depth
                     * shadows this same type at the current depth, so we can
                     * ignore this one. */
                    continue;
                }
                gt_instance_add(sa, &seen, named);

                Int n = types_named_num_methods(named);
                for (Int i = 0; i < n; i++)
                    mset = gt_mset_add_one(sa, mset, types_named_method(named, i),
                                           gt_concat(sa, e.index, i), e.indirect,
                                           e.multiples);
            }

            TypesType u = types_type_underlying(typ);
            if (u == NULL)
                continue;
            if (u->kind == TYPES_KIND_STRUCT) {
                TypesStruct *t = GT_KNOWN(TypesStruct, u);
                for (Int i = 0; i < t->fields.len; i++) {
                    TypesVar *f = GT_AT(t->fields, i, TypesVar *);
                    if (fset == NULL)
                        fset = gt_mset_make(sa);
                    gt_mset_put(fset, types_object_id(sa, types_object(f)), NULL);

                    /* Embedded fields are always of the form T or *T where T
                     * is a type name. If typ appeared multiple times at this
                     * depth, f.Type appears multiple times at the next
                     * depth. */
                    if (f->embedded) {
                        bool fptr;
                        TypesType ft = burrow__types_deref(GT_OBJ(f)->typ, &fptr);
                        gt_emb_push(sa, &next,
                                    (GtEmbedded){ft, gt_concat(sa, e.index, i),
                                                 e.indirect || fptr, e.multiples});
                    }
                }
            } else if (u->kind == TYPES_KIND_INTERFACE) {
                GtTypeSet *tset =
                    burrow__types_iface_type_set(GT_KNOWN(TypesInterface, u));
                mset = gt_mset_add(sa, mset, tset->methods, e.index, true, e.multiples);
            }
        }

        /* Add methods and collisions at this depth to base if no entries with
         * matching names exist already. */
        if (mset != NULL) {
            MapIter it = map_iter(mset);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v)) {
                Str key = *(const Str *)k;
                TypesSelection *m = *(TypesSelection **)v;
                if (base == NULL || map_get(base, &key) == NULL) {
                    /* Fields collide with methods of the same name at this
                     * depth. */
                    if (fset != NULL && map_get(fset, &key) != NULL)
                        m = NULL; /* collision */
                    if (base == NULL)
                        base = gt_mset_make(sa);
                    gt_mset_put(base, key, m);
                }
            }
        }

        /* Add all (remaining) fields at this depth as collisions (since they
         * will hide any method further down) if no entries with matching
         * names exist already. */
        if (fset != NULL) {
            MapIter it = map_iter(fset);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v)) {
                Str key = *(const Str *)k;
                if (base == NULL || map_get(base, &key) == NULL) {
                    if (base == NULL)
                        base = gt_mset_make(sa);
                    gt_mset_put(base, key, NULL); /* collision */
                }
            }
        }

        current = gt_consolidate_multiples(next);
    }

    TypesMethodSet *ms = &gt_empty_method_set;
    if (base != NULL && map_len(base) > 0) {
        /* collect methods */
        Slice list = slice_nil(TYPE_UNSAFE_POINTER);
        MapIter it = map_iter(base);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            TypesSelection *m = *(TypesSelection **)v;
            if (m != NULL) {
                TypesSelection *sel =
                    burrow__types_new_selection(a, TYPES_METHOD_VAL, T, m->obj,
                                                gt_index_out(a, m->index), m->indirect);
                burrow__types_ptrs_append(a, &list, sel);
            }
        }
        if (list.len > 0) {
            gt_sort_selections(list);
            ms = GT_NEW(a, TypesMethodSet);
            ms->list = list;
        }
    }
    arena_free(&ar);
    return ms;
}

Int types_method_set_len(TypesMethodSet *s) {
    return s->list.len;
}

TypesSelection *types_method_set_at(TypesMethodSet *s, Int i) {
    return *(TypesSelection **)slice_at(s->list, i);
}

TypesSelection *types_method_set_lookup(TypesMethodSet *s, TypesPackage *pkg,
                                        Str name) {
    if (s->list.len == 0)
        return NULL;
    /* The list is sorted by Id, so a binary search finds the first entry
     * whose Id is not less than Id(pkg, name). */
    Int lo = 0, hi = s->list.len;
    while (lo < hi) {
        Int h = lo + (hi - lo) / 2;
        TypesSelection *m = GT_AT(s->list, h, TypesSelection *);
        if (burrow__types_id_cmp(GT_OBJ(m->obj)->pkg, GT_OBJ(m->obj)->name, pkg, name) <
            0)
            lo = h + 1;
        else
            hi = h;
    }
    if (lo < s->list.len) {
        TypesSelection *m = GT_AT(s->list, lo, TypesSelection *);
        if (burrow__types_id_eq(GT_OBJ(m->obj)->pkg, GT_OBJ(m->obj)->name, pkg, name))
            return m;
    }
    return NULL;
}

Str types_method_set_string(Alloc *a, TypesMethodSet *s) {
    if (s->list.len == 0)
        return BURROW_S("MethodSet {}");
    BytesBuffer buf = BYTES_BUFFER(a);
    burrow__types_buf_str(&buf, BURROW_S("MethodSet {\n"));
    for (Int i = 0; i < s->list.len; i++) {
        burrow__types_buf_byte(&buf, '\t');
        types_write_selection(&buf, GT_AT(s->list, i, TypesSelection *),
                              TYPES_NO_QUALIFIER);
        burrow__types_buf_byte(&buf, '\n');
    }
    burrow__types_buf_str(&buf, BURROW_S("}\n"));
    return burrow__types_buf_done(&buf, a);
}
