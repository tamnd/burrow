/* go/types: the check that a package has no unbounded recursive
 * instantiation, which compilers that instantiate statically (by
 * monomorphization) cannot handle.
 *
 * Derived from Go's src/go/types/mono.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* It is a sort of "type flow" analysis that finds which type parameters are
 * instantiated with other type parameters, or types made from them. A package
 * cannot be statically instantiated if the graph has a cycle with at least
 * one derived type in it.
 *
 * The graph is directed and weighted. Vertices stand for type parameters and
 * some defined types, and edges for how types depend on each other:
 *
 * - Everywhere a type-parameterized function or type is instantiated, there
 *   is an edge to each type parameter from the vertices (if any) for each
 *   type parameter or defined type the type argument refers to. If the type
 *   argument is just the type referred to, the edge has weight 0, and 1
 *   otherwise.
 *
 * - For every defined type declared within a type-parameterized function or
 *   method, there is an edge of weight 1 to the defined type from each
 *   ambient type parameter.
 *
 * For example, given:
 *
 *     func f[A, B any]() {
 *         type T int
 *         f[T, map[A]B]()
 *     }
 *
 * there are vertices for A, B and T. The declaration "type T int" gives the
 * edges T<-A and T<-B with weight 1, and the instantiation "f[T, map[A]B]"
 * gives A<-T with weight 0, and B<-A and B<-B with weight 1.
 *
 * Positive-weight cycles are errors. Zero-weight cycles are fine because
 * static instantiation reaches a fixed point.
 *
 * Not in Go: Go walks the elements of a scope in map order, which is random.
 * Here they are walked by name, so the edges, and the cycle reported, are the
 * same from run to run. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

static GtMonoVertex *tmo_vertex(GtMonoGraph *w, Int i) {
    return GT_AT(w->vertices, i, GtMonoVertex *);
}

static GtMonoEdge *tmo_edge(GtMonoGraph *w, Int i) {
    return GT_AT(w->edges, i, GtMonoEdge *);
}

/* reportInstanceLoop: there is a path that contains a cycle and ends at v. */
static void tmo_report_instance_loop(TypesChecker *check, Int v) {
    GtMonoGraph *w = &check->mono;
    Slice stack = slice_nil(TYPE_INT);
    bool *seen = burrow__types_alloc(check->a, sizeof(bool) * (size_t)w->vertices.len,
                                     _Alignof(bool));
    for (Int i = 0; i < w->vertices.len; i++)
        seen[i] = false;

    /* v may only be reachable from the cycle, not on the cycle itself. Start
     * by walking backwards along the path until a vertex appears twice. */
    while (!seen[v]) {
        Slice r = slice_append(check->a, stack, &v, 1);
        if (r.p == NULL)
            burrow__types_oom();
        stack = r;
        seen[v] = true;
        v = tmo_edge(w, tmo_vertex(w, v)->pre)->src;
    }

    /* Trim any vertices visited before visiting v the first time. Since v is
     * the first vertex found within the cycle, any vertices visited earlier
     * cannot be part of the cycle. */
    Int first = 0;
    while (GT_AT(stack, first, Int) != v)
        first++;

    GtError *err = burrow__tc_new_error(check, GT_CODE_INVALID_INSTANCE_CYCLE);
    TypesTypeName *obj0 = tmo_vertex(w, v)->obj;
    burrow__tc_error_add(err, burrow__tc_at_obj(types_object(obj0)),
                         BURROW_S("instantiation cycle:"));

    TypesQualifier qf = types_relative_to(check->pkg);
    for (Int i = first; i < stack.len; i++) {
        GtMonoEdge *edge = tmo_edge(w, tmo_vertex(w, GT_AT(stack, i, Int))->pre);
        TypesTypeName *obj = tmo_vertex(w, edge->dst)->obj;
        Str name = GT_OBJ(obj)->name;
        Str ts = types_type_string(check->a, edge->typ, qf);
        TypesType t = GT_OBJ(obj)->typ;
        switch (t == NULL ? (Int)TYPES_KIND_INVALID : t->kind) {
        case TYPES_KIND_NAMED:
            /* secondary error, \t indented */
            GT_ADDF(err, burrow__tc_at_pos(edge->pos),
                    "%s implicitly parameterized by %s", name, ts);
            break;
        case TYPES_KIND_TYPE_PARAM:
            /* secondary error, \t indented */
            GT_ADDF(err, burrow__tc_at_pos(edge->pos), "%s instantiated as %s", name,
                    ts);
            break;
        default:
            panic_str(BURROW_S("unexpected type"));
        }
    }
    burrow__tc_error_report(err);
}

/* monomorph detects unbounded instantiation cycles using a variant of
 * Bellman-Ford's algorithm. Instead of always running |V| iterations, it runs
 * until it reaches a fixed point or finds a path of length |V|. This allows it
 * to stop earlier when there are no cycles, which should be the common
 * case. */
void burrow__tc_monomorph(TypesChecker *check) {
    GtMonoGraph *w = &check->mono;
    bool again = true;
    while (again) {
        again = false;
        for (Int i = 0; i < w->edges.len; i++) {
            GtMonoEdge *edge = tmo_edge(w, i);
            GtMonoVertex *src = tmo_vertex(w, edge->src);
            GtMonoVertex *dst = tmo_vertex(w, edge->dst);

            /* N.B., this looks for the greatest weight paths, unlike typical
             * Bellman-Ford. */
            Int wt = src->weight + edge->weight;
            if (wt <= dst->weight)
                continue;

            dst->pre = i;
            dst->len = src->len + 1;
            if (dst->len == w->vertices.len) {
                tmo_report_instance_loop(check, edge->dst);
                return;
            }

            dst->weight = wt;
            again = true;
        }
    }
}

/* recordCanon records that tpar is the canonical type parameter corresponding
 * to method type parameter mpar. */
void burrow__tc_mono_record_canon(TypesChecker *check, TypesTypeParam *mpar,
                                  TypesTypeParam *tpar) {
    GtMonoGraph *w = &check->mono;
    if (w->canon == NULL) {
        w->canon = map_make(check->a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
        if (w->canon == NULL)
            burrow__types_oom();
    }
    if (!map_set(w->canon, &mpar, &tpar))
        burrow__types_oom();
}

static void tmo_add_edge(TypesChecker *check, Int dst, Int src, Int weight,
                         TokenPos pos, TypesType typ) {
    GtMonoEdge *e = GT_NEW(check->a, GtMonoEdge);
    e->dst = dst;
    e->src = src;
    e->weight = weight;
    e->pos = pos;
    e->typ = typ;
    burrow__types_ptrs_append(check->a, &check->mono.edges, e);
}

static Int tmo_new_vertex(TypesChecker *check, TypesTypeName *obj) {
    GtMonoGraph *w = &check->mono;
    GtMonoVertex *v = GT_NEW(check->a, GtMonoVertex);
    v->weight = 0;
    v->pre = 0;
    v->len = 0;
    v->obj = obj;
    Int idx = w->vertices.len;
    burrow__types_ptrs_append(check->a, &w->vertices, v);
    return idx;
}

static void tmo_set_name_idx(TypesChecker *check, TypesTypeName *obj, Int idx) {
    GtMonoGraph *w = &check->mono;
    if (w->name_idx == NULL) {
        w->name_idx = map_make(check->a, TYPE_UNSAFE_POINTER, TYPE_INT, 0);
        if (w->name_idx == NULL)
            burrow__types_oom();
    }
    if (!map_set(w->name_idx, &obj, &idx))
        burrow__types_oom();
}

/* typeParamVertex returns the index of the vertex representing tpar. */
static Int tmo_type_param_vertex(TypesChecker *check, TypesTypeParam *tpar) {
    GtMonoGraph *w = &check->mono;
    if (w->canon != NULL) {
        TypesTypeParam **x = map_get(w->canon, &tpar);
        if (x != NULL)
            tpar = *x;
    }
    TypesTypeName *obj = tpar->obj;
    if (w->name_idx != NULL) {
        Int *idx = map_get(w->name_idx, &obj);
        if (idx != NULL)
            return *idx;
    }
    Int idx = tmo_new_vertex(check, obj);
    tmo_set_name_idx(check, obj, idx);
    return idx;
}

/* localNamedVertex returns the index of the vertex representing named, or -1
 * if named doesn't need representation. */
static Int tmo_local_named_vertex(TypesChecker *check, TypesPackage *pkg,
                                  TypesNamed *named) {
    GtMonoGraph *w = &check->mono;
    TypesTypeName *obj = types_named_obj(named);
    if (GT_OBJ(obj)->pkg != pkg)
        return -1; /* imported type */

    TypesScope *root = types_package_scope(pkg);
    if (types_object_parent(types_object(obj)) == root)
        return -1; /* package scope, no ambient type parameters */

    if (w->name_idx != NULL) {
        Int *idx = map_get(w->name_idx, &obj);
        if (idx != NULL)
            return *idx;
    }

    Int idx = -1;

    /* Walk the type definition's scope to find any ambient type parameters
     * that it's implicitly parameterized by. */
    for (TypesScope *scope = types_object_parent(types_object(obj)); scope != root;
         scope = types_scope_parent(scope)) {
        Slice names = types_scope_names(check->a, scope);
        for (Int i = 0; i < names.len; i++) {
            TypesTypeName *tn =
                types_as_type_name(types_scope_lookup(scope, GT_AT(names, i, Str)));
            if (tn == NULL || types_type_name_is_alias(tn) ||
                GT_OBJ(tn)->pos >= GT_OBJ(obj)->pos)
                continue;
            TypesType t = GT_OBJ(tn)->typ;
            if (t == NULL || t->kind != TYPES_KIND_TYPE_PARAM)
                continue;
            if (idx < 0)
                idx = tmo_new_vertex(check, obj);
            TypesTypeParam *tpar = types_as_type_param(t);
            tmo_add_edge(check, idx, tmo_type_param_vertex(check, tpar), 1,
                         GT_OBJ(obj)->pos, t);
        }
    }

    tmo_set_name_idx(check, obj, idx);
    return idx;
}

/* What assign's walk of a type argument closes over. */
typedef struct TmoAssign {
    TypesChecker *check;
    TypesPackage *pkg;
    TokenPos pos;
    TypesTypeParam *tpar;
    TypesType targ;
} TmoAssign;

/* flow adds an edge from vertex src representing that typ flows to tpar. */
static void tmo_flow(TmoAssign *c, Int src, TypesType typ) {
    Int weight = typ == c->targ ? 0 : 1;
    tmo_add_edge(c->check, tmo_type_param_vertex(c->check, c->tpar), src, weight,
                 c->pos, c->targ);
}

static void tmo_do_tuple(TmoAssign *c, TypesTuple *tup);

/* Recursively walk the type argument to find any defined types or type
 * parameters. */
static void tmo_do(TmoAssign *c, TypesType typ) {
    typ = types_unalias(typ);
    switch (typ->kind) {
    case TYPES_KIND_TYPE_PARAM: {
        TypesTypeParam *t = types_as_type_param(typ);
        burrow__types_assert(GT_OBJ(t->obj)->pkg == c->pkg);
        tmo_flow(c, tmo_type_param_vertex(c->check, t), typ);
        break;
    }
    case TYPES_KIND_NAMED: {
        TypesNamed *t = types_as_named(typ);
        Int src = tmo_local_named_vertex(c->check, c->pkg, types_named_origin(t));
        if (src >= 0)
            tmo_flow(c, src, typ);
        TypesTypeList *targs = types_named_type_args(t);
        for (Int i = 0; i < types_type_list_len(targs); i++)
            tmo_do(c, types_type_list_at(targs, i));
        break;
    }
    case TYPES_KIND_ARRAY:
        tmo_do(c, types_as_array(typ)->elem);
        break;
    case TYPES_KIND_BASIC:
        /* ok */
        break;
    case TYPES_KIND_CHAN:
        tmo_do(c, types_as_chan(typ)->elem);
        break;
    case TYPES_KIND_MAP:
        tmo_do(c, types_as_map(typ)->key);
        tmo_do(c, types_as_map(typ)->elem);
        break;
    case TYPES_KIND_POINTER:
        tmo_do(c, types_as_pointer(typ)->base);
        break;
    case TYPES_KIND_SLICE:
        tmo_do(c, types_as_slice(typ)->elem);
        break;
    case TYPES_KIND_INTERFACE: {
        TypesInterface *t = types_as_interface(typ);
        for (Int i = 0; i < types_interface_num_methods(t); i++)
            tmo_do(c, GT_OBJ(types_interface_method(t, i))->typ);
        break;
    }
    case TYPES_KIND_SIGNATURE:
        tmo_do_tuple(c, types_as_signature(typ)->params);
        tmo_do_tuple(c, types_as_signature(typ)->results);
        break;
    case TYPES_KIND_STRUCT: {
        TypesStruct *t = types_as_struct(typ);
        for (Int i = 0; i < types_struct_num_fields(t); i++)
            tmo_do(c, GT_OBJ(types_struct_field(t, i))->typ);
        break;
    }
    default:
        panic_str(BURROW_S("unexpected type"));
    }
}

static void tmo_do_tuple(TmoAssign *c, TypesTuple *tup) {
    for (Int i = 0; i < types_tuple_len(tup); i++)
        tmo_do(c, GT_OBJ(types_tuple_at(tup, i))->typ);
}

/* assign records that tpar was instantiated as targ at pos. */
static void tmo_assign(TypesChecker *check, TypesPackage *pkg, TokenPos pos,
                       TypesTypeParam *tpar, TypesType targ) {
    /* Go generics do not have an analog to C++'s template-templates, where a
     * template parameter can itself be an instantiable template. So any
     * instantiation cycles must occur within a single package. Accordingly,
     * instantiations of imported type parameters can be ignored. */
    if (GT_OBJ(tpar->obj)->pkg != pkg)
        return;
    TmoAssign c = {check, pkg, pos, tpar, targ};
    tmo_do(&c, targ);
}

/* recordInstance records that the given type parameters were instantiated
 * with the corresponding type arguments. xlist is of AstExpr. */
void burrow__tc_mono_record_instance(TypesChecker *check, TypesPackage *pkg,
                                     TokenPos pos, Slice tparams, Slice targs,
                                     Slice xlist) {
    for (Int i = 0; i < tparams.len; i++) {
        TokenPos p = pos;
        if (i < xlist.len)
            p = ast_node_pos(GT_AT(xlist, i, AstExpr));
        tmo_assign(check, pkg, p, GT_AT(tparams, i, TypesTypeParam *),
                   GT_AT(targs, i, TypesType));
    }
}
