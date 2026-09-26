/* maps, derived from Go's src/maps/maps.go and src/maps/iter.go (go1.27.1).
 *
 * The sequences capture only the map, so they point at the caller's Map and
 * allocate nothing.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/maps.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/map.h"
#include "burrow/type.h"

#include <stddef.h>

bool maps_equal(Map *m1, Map *m2) {
    const void *k;
    void *v1;
    if (map_len(m1) != map_len(m2))
        return false;
    for (MapIter it = map_iter(m1); map_next(&it, &k, &v1);) {
        void *v2 = map_get(m2, k);
        if (v2 == NULL || !type_equal(map_val_type(m1), v1, v2))
            return false;
    }
    return true;
}

bool maps_equal_func(Map *m1, Map *m2, MapsEqualFunc eq) {
    const void *k;
    void *v1;
    if (map_len(m1) != map_len(m2))
        return false;
    for (MapIter it = map_iter(m1); map_next(&it, &k, &v1);) {
        void *v2 = map_get(m2, k);
        if (v2 == NULL || !eq.f(eq.env, v1, v2))
            return false;
    }
    return true;
}

Map *maps_clone(Alloc *a, const Map *m) {
    return map_clone(a, m);
}

bool maps_copy(Map *dst, Map *src) {
    const void *k;
    void *v;
    for (MapIter it = map_iter(src); map_next(&it, &k, &v);) {
        if (!map_set(dst, k, v))
            return false;
    }
    return true;
}

void maps_delete_func(Map *m, MapsPredFunc del) {
    const void *k;
    void *v;
    for (MapIter it = map_iter(m); map_next(&it, &k, &v);) {
        if (del.f(del.env, k, v))
            map_del(m, k);
    }
}

static void maps_all_run(void *env, IterYield2 yield) {
    const void *k;
    void *v;
    for (MapIter it = map_iter(env); map_next(&it, &k, &v);) {
        if (!yield.f(yield.env, k, v))
            return;
    }
}

static void maps_keys_run(void *env, IterYield yield) {
    const void *k;
    for (MapIter it = map_iter(env); map_next(&it, &k, NULL);) {
        if (!yield.f(yield.env, k))
            return;
    }
}

static void maps_values_run(void *env, IterYield yield) {
    void *v;
    for (MapIter it = map_iter(env); map_next(&it, NULL, &v);) {
        if (!yield.f(yield.env, v))
            return;
    }
}

IterSeq2 maps_all(Map *m) {
    return BURROW_FN(IterSeq2, maps_all_run, m);
}

IterSeq maps_keys(Map *m) {
    return BURROW_FN(IterSeq, maps_keys_run, m);
}

IterSeq maps_values(Map *m) {
    return BURROW_FN(IterSeq, maps_values_run, m);
}

/* What Insert's loop body captures. */
typedef struct MapsInsert {
    Map *m;
    bool ok;
} MapsInsert;

static bool maps_insert_yield(void *env, const void *k, const void *v) {
    MapsInsert *st = env;
    st->ok = map_set(st->m, k, v);
    return st->ok;
}

bool maps_insert(Map *m, IterSeq2 seq) {
    MapsInsert st = {m, true};
    seq.f(seq.env, BURROW_FN(IterYield2, maps_insert_yield, &st));
    return st.ok;
}

Map *maps_collect(Alloc *a, const Type *key, const Type *val, IterSeq2 seq) {
    Map *m = map_make(a, key, val, 0);
    if (m != NULL && !maps_insert(m, seq)) {
        map_free(m);
        return NULL;
    }
    return m;
}
