/* go/types: the initialization order of package variables.
 *
 * Derived from Go's src/go/types/initorder.go. Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* A graphNode represents a node in the object dependency graph. Each node p in
 * n.pred represents an edge p->n, and each node s in n.succ represents an edge
 * n->s; with a->b indicating that a depends on b. */
typedef struct TioNode {
    TypesObject obj; /* object represented by this node */
    Map *pred;       /* consumers of this node, TioNode * to bool, or NULL */
    Map *succ;       /* dependencies of this node, TioNode * to bool, or NULL */
    Int index;       /* node index in graph slice/priority queue */
    Int ndeps;       /* number of outstanding dependencies before this object can be
                * initialized */
} TioNode;

static Map *tio_map(Alloc *a, const Type *val) {
    Map *m = map_make(a, TYPE_UNSAFE_POINTER, val, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

static void tio_set(Map *m, const void *key, const void *val) {
    if (!map_set(m, key, val))
        burrow__types_oom();
}

/* A dependency is an object that may be a dependency in an initialization
 * expression. Only constants, variables, and functions can be dependencies.
 * Constants are here because constant expression cycles are reported during
 * initialization order computation. */
static bool tio_is_dependency(TypesObject obj) {
    Int kind = obj->kind;
    return kind == TYPES_OBJECT_CONST || kind == TYPES_OBJECT_VAR ||
           kind == TYPES_OBJECT_FUNC;
}

/* cost returns the cost of removing this node, which involves copying each
 * predecessor to each successor (and vice-versa). */
static Int tio_cost(const TioNode *n) {
    return map_len(n->pred) * map_len(n->succ);
}

static void tio_add(Alloc *a, Map **s, TioNode *p) {
    if (*s == NULL)
        *s = tio_map(a, TYPE_BOOL);
    bool yes = true;
    tio_set(*s, &p, &yes);
}

static void tio_del(Map *s, TioNode *p) {
    if (s != NULL)
        map_del(s, &p);
}

static int tio_cmp_cost(void *env, const void *x, const void *y) {
    (void)env;
    const TioNode *a = *(TioNode *const *)x;
    const TioNode *b = *(TioNode *const *)y;
    Int ca = tio_cost(a);
    Int cb = tio_cost(b);
    return (ca > cb) - (ca < cb);
}

/* dependencyGraph computes the object dependency graph from the given objMap,
 * with any function nodes removed. The resulting graph contains only constants
 * and variables.
 *
 * Not in Go: the nodes are made in source order rather than in map order, and
 * the functions are removed with a stable sort by cost, so the graph is built
 * the same way from run to run. */
static Slice tio_dependency_graph(TypesChecker *check) {
    Alloc *a = check->a;
    burrow__types_assert(check->obj_list.len == map_len(check->obj_map));

    /* M is the dependency (Object) -> graphNode mapping */
    Map *M = tio_map(a, TYPE_UNSAFE_POINTER);
    Slice nodes = {0};
    for (Int i = 0; i < check->obj_list.len; i++) {
        TypesObject obj = GT_AT(check->obj_list, i, TypesObject);
        /* only consider nodes that may be an initialization dependency */
        if (tio_is_dependency(obj)) {
            TioNode *n = GT_NEW(a, TioNode);
            n->obj = obj;
            tio_set(M, &obj, &n);
            burrow__types_ptrs_append(a, &nodes, n);
        }
    }

    /* compute edges for graph M (We need to include all nodes, even isolated
     * ones, because they still need to be scheduled for initialization in
     * correct order relative to other nodes.) */
    for (Int i = 0; i < nodes.len; i++) {
        TioNode *n = GT_AT(nodes, i, TioNode *);
        /* for each dependency obj -> d (= deps[i]), create graph edges n->s and
         * s->n */
        GtDeclInfo *info = burrow__tc_obj_info(check, n->obj);
        burrow__types_assert(info != NULL);
        Map *deps = info->deps;
        if (deps == NULL)
            continue;
        MapIter it = map_iter(deps);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            TypesObject d = *(TypesObject const *)k;
            /* only consider nodes that may be an initialization dependency */
            if (tio_is_dependency(d)) {
                TioNode **dn = map_get(M, &d);
                burrow__types_assert(dn != NULL);
                tio_add(a, &n->succ, *dn);
                tio_add(a, &(*dn)->pred, n);
            }
        }
    }

    Slice G = {0}; /* separate non-functions and functions */
    Slice funcG = {0};
    for (Int i = 0; i < nodes.len; i++) {
        TioNode *n = GT_AT(nodes, i, TioNode *);
        if (n->obj->kind == TYPES_OBJECT_FUNC)
            burrow__types_ptrs_append(a, &funcG, n);
        else
            burrow__types_ptrs_append(a, &G, n);
    }

    /* remove function nodes and collect remaining graph nodes in G (Mutually
     * recursive functions may introduce cycles among themselves which are
     * permitted. Yet such cycles may incorrectly inflate the dependency count
     * for variables which in turn may not get scheduled for initialization in
     * correct order.)
     *
     * Note that because we recursively copy predecessors and successors
     * throughout the function graph, the cost of removing a function at
     * position X is proportional to cost * (len(funcG)-X). Therefore, we
     * should remove high-cost functions last. */
    slices_sort_stable_func(funcG, (SlicesCmpFunc){tio_cmp_cost, NULL});
    for (Int i = 0; i < funcG.len; i++) {
        TioNode *n = GT_AT(funcG, i, TioNode *);
        /* connect each predecessor p of n with each successor s and drop the
         * function node (don't collect it in G) */
        if (n->pred != NULL) {
            MapIter pit = map_iter(n->pred);
            const void *pk;
            void *pv;
            while (map_next(&pit, &pk, &pv)) {
                TioNode *p = *(TioNode *const *)pk;
                /* ignore self-cycles */
                if (p == n)
                    continue;
                /* Each successor s of n becomes a successor of p, and each
                 * predecessor p of n becomes a predecessor of s. */
                if (n->succ != NULL) {
                    MapIter sit = map_iter(n->succ);
                    const void *sk;
                    void *sv;
                    while (map_next(&sit, &sk, &sv)) {
                        TioNode *s = *(TioNode *const *)sk;
                        /* ignore self-cycles */
                        if (s != n) {
                            tio_add(a, &p->succ, s);
                            tio_add(a, &s->pred, p);
                        }
                    }
                }
                tio_del(p->succ, n); /* remove edge to n */
            }
        }
        if (n->succ != NULL) {
            MapIter sit = map_iter(n->succ);
            const void *sk;
            void *sv;
            while (map_next(&sit, &sk, &sv)) {
                TioNode *s = *(TioNode *const *)sk;
                tio_del(s->pred, n); /* remove edge to n */
            }
        }
    }

    /* fill in index and ndeps fields */
    for (Int i = 0; i < G.len; i++) {
        TioNode *n = GT_AT(G, i, TioNode *);
        n->index = i;
        n->ndeps = map_len(n->succ);
    }

    return G;
}

/* ------------------------------------------------------------ priority queue */

/* nodeQueue implements the container/heap interface; a nodeQueue may be used as
 * a priority queue. Not in Go: container/heap's Init, Pop, Fix, up and down are
 * written out here for it. */

static void tio_swap(Slice q, Int i, Int j) {
    TioNode *x = GT_AT(q, i, TioNode *);
    TioNode *y = GT_AT(q, j, TioNode *);
    GT_AT(q, i, TioNode *) = y;
    GT_AT(q, j, TioNode *) = x;
    x->index = j;
    y->index = i;
}

static bool tio_less(Slice q, Int i, Int j) {
    const TioNode *x = GT_AT(q, i, TioNode *);
    const TioNode *y = GT_AT(q, j, TioNode *);

    /* Prioritize all constants before non-constants. See go.dev/issue/66575/. */
    bool x_const = x->obj->kind == TYPES_OBJECT_CONST;
    bool y_const = y->obj->kind == TYPES_OBJECT_CONST;
    if (x_const != y_const)
        return x_const;

    /* nodes are prioritized by number of incoming dependencies (1st key) and
     * source order (2nd key) */
    return x->ndeps < y->ndeps ||
           (x->ndeps == y->ndeps && GT_OBJ(x->obj)->order < GT_OBJ(y->obj)->order);
}

static void tio_up(Slice q, Int j) {
    for (;;) {
        Int i = (j - 1) / 2; /* parent */
        if (i == j || !tio_less(q, j, i))
            break;
        tio_swap(q, i, j);
        j = i;
    }
}

static bool tio_down(Slice q, Int i0, Int n) {
    Int i = i0;
    for (;;) {
        Int j1 = 2 * i + 1;
        if (j1 >= n || j1 < 0) /* j1 < 0 after int overflow */
            break;
        Int j = j1; /* left child */
        Int j2 = j1 + 1;
        if (j2 < n && tio_less(q, j2, j1))
            j = j2; /* = 2*i + 2  // right child */
        if (!tio_less(q, j, i))
            break;
        tio_swap(q, i, j);
        i = j;
    }
    return i > i0;
}

static void tio_heap_init(Slice q) {
    /* heapify */
    Int n = q.len;
    for (Int i = n / 2 - 1; i >= 0; i--)
        tio_down(q, i, n);
}

static TioNode *tio_heap_pop(Slice *q) {
    Int n = q->len - 1;
    tio_swap(*q, 0, n);
    tio_down(*q, 0, n);
    TioNode *x = GT_AT(*q, n, TioNode *);
    x->index = -1; /* for safety */
    q->len = n;
    return x;
}

static void tio_heap_fix(Slice q, Int i) {
    if (!tio_down(q, i, q.len))
        tio_up(q, i);
}

/* ------------------------------------------------------------------- cycles */

static int tio_cmp_order(void *env, const void *x, const void *y) {
    (void)env;
    uint32_t a = GT_OBJ(*(TypesObject const *)x)->order;
    uint32_t b = GT_OBJ(*(TypesObject const *)y)->order;
    return (a > b) - (a < b);
}

/* findPath returns the (reversed) list of objects []Object{to, ... from} such
 * that there is a path of object dependencies from 'from' to 'to'. If there is
 * no such path, the result is nil. */
static Slice tio_find_path(TypesChecker *check, TypesObject from, TypesObject to,
                           Map *seen) {
    if (map_get(seen, &from) != NULL)
        return (Slice){0};
    bool yes = true;
    tio_set(seen, &from, &yes);

    /* sort deps for deterministic result */
    Slice deps = {0};
    GtDeclInfo *info = burrow__tc_obj_info(check, from);
    burrow__types_assert(info != NULL);
    Map *dmap = info->deps;
    if (dmap != NULL) {
        MapIter it = map_iter(dmap);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v))
            burrow__types_ptrs_append(check->a, &deps, *(TypesObject const *)k);
    }
    slices_sort_func(deps, (SlicesCmpFunc){tio_cmp_order, NULL});

    for (Int i = 0; i < deps.len; i++) {
        TypesObject d = GT_AT(deps, i, TypesObject);
        if (d == to) {
            Slice p = {0};
            burrow__types_ptrs_append(check->a, &p, d);
            return p;
        }
        Slice p = tio_find_path(check, d, to, seen);
        if (p.len != 0) {
            burrow__types_ptrs_append(check->a, &p, d);
            return p;
        }
    }

    return (Slice){0};
}

/* reportCycle reports an error for the given cycle. */
static void tio_report_cycle(TypesChecker *check, Slice cycle) {
    TypesObject obj = GT_AT(cycle, 0, TypesObject);

    /* report a more concise error for self references */
    if (cycle.len == 1) {
        Str name = GT_OBJ(obj)->name;
        GT_ERRORF(check, burrow__tc_at_obj(obj), GT_CODE_INVALID_INIT_CYCLE,
                  "initialization cycle: %s refers to itself", name);
        return;
    }

    GtError *err = burrow__tc_new_error(check, GT_CODE_INVALID_INIT_CYCLE);
    Str name = GT_OBJ(obj)->name;
    GT_ADDF(err, burrow__tc_at_obj(obj), "initialization cycle for %s", name);
    /* "cycle[i] refers to cycle[j]" for (i,j) = (0,n-1), (n-1,n-2), ..., (1,0)
     * for len(cycle) = n. */
    for (Int j = cycle.len - 1; j >= 0; j--) {
        TypesObject next = GT_AT(cycle, j, TypesObject);
        Str from = GT_OBJ(obj)->name;
        Str to = GT_OBJ(next)->name;
        GT_ADDF(err, burrow__tc_at_obj(obj), "%s refers to %s", from, to);
        obj = next;
    }
    burrow__tc_error_report(err);
}

/* ---------------------------------------------------------------- initOrder */

/* initOrder computes the Info.InitOrder for package variables. */
void burrow__tc_init_order(TypesChecker *check) {
    Alloc *a = check->a;

    /* An InitOrder may already have been computed if a package is built from
     * several calls to (*Checker).Files. Clear it. */
    check->info->init_order.len = 0;

    /* Compute the object dependency graph and initialize a priority queue with
     * the list of graph nodes. */
    Slice pq = tio_dependency_graph(check);
    tio_heap_init(pq);

    /* Not in Go: the debug printing, which is off, is left out. */

    /* Determine initialization order by removing the highest priority node
     * (the one with the fewest dependencies) and its edges from the graph,
     * repeatedly, until there are no nodes left. In a valid Go program, those
     * nodes always have zero dependencies (after removing all incoming
     * dependencies), otherwise there are initialization cycles. */
    Map *emitted = tio_map(a, TYPE_BOOL);
    while (pq.len > 0) {
        /* get the next node */
        TioNode *n = tio_heap_pop(&pq);

        /* if n still depends on other nodes, we have a cycle */
        if (n->ndeps > 0) {
            Slice cycle = tio_find_path(check, n->obj, n->obj, tio_map(a, TYPE_BOOL));
            /* If n.obj is not part of the cycle (e.g., n.obj->b->c->d->c),
             * cycle will be nil. Don't report anything in that case since the
             * cycle is reported when the algorithm gets to an object in the
             * cycle. Furthermore, once an object in the cycle is encountered,
             * the cycle will be broken (dependency count will be reduced
             * below), and so the remaining nodes in the cycle don't trigger
             * another error (unless they are part of multiple cycles). */
            if (cycle.len != 0)
                tio_report_cycle(check, cycle);
            /* Ok to continue, but the variable initialization order will be
             * incorrect at this point since it assumes no cycle errors. */
        }

        /* reduce dependency count of all dependent nodes and update priority
         * queue */
        if (n->pred != NULL) {
            MapIter it = map_iter(n->pred);
            const void *k;
            void *v;
            while (map_next(&it, &k, &v)) {
                TioNode *p = *(TioNode *const *)k;
                p->ndeps--;
                tio_heap_fix(pq, p->index);
            }
        }

        /* record the init order for variables with initializers only */
        if (n->obj->kind != TYPES_OBJECT_VAR)
            continue;
        TypesVar *v = GT_KNOWN(TypesVar, n->obj);
        GtDeclInfo *info = burrow__tc_obj_info(check, n->obj);
        if (info == NULL || !burrow__tc_decl_has_initializer(info))
            continue;

        /* n:1 variable declarations such as: a, b = f() introduce a node for
         * each lhs variable (here: a, b); but they all have the same
         * initializer - emit only one, for the first variable seen */
        if (map_get(emitted, &info) != NULL)
            continue; /* initializer already emitted, if any */
        bool yes = true;
        tio_set(emitted, &info, &yes);

        Slice info_lhs = info->lhs; /* possibly nil (see declInfo.lhs field
                                     * comment) */
        if (info_lhs.len == 0) {
            info_lhs = (Slice){0};
            burrow__types_ptrs_append(a, &info_lhs, v);
        }
        TypesInitializer *init = GT_NEW(a, TypesInitializer);
        init->lhs = info_lhs;
        init->rhs = info->init;
        burrow__types_ptrs_append(a, &check->info->init_order, init);
    }
}
