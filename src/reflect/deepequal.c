/* package reflect, DeepEqual. Go's reflect/deepequal.go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <string.h>

#include "burrow/map.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"

/* A comparison already under way: the two addresses, the lower first, and
 * the type. Seeing one again means a cycle, and Go counts that as equal, since
 * whatever would make the two differ is being compared further up. */
typedef struct RfVisit {
    const void *a1;
    const void *a2;
    const Type *typ;
} RfVisit;

/* [3]unsafe.Pointer, which is what an RfVisit is as far as a map cares. */
static const Type rf_visit_type = {
    {NULL, 0},
    {NULL, 0},
    KIND_ARRAY,
    (uint32_t)sizeof(RfVisit),
    (uint16_t)_Alignof(RfVisit),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_UnsafePointer,
    NULL,
    3,
    0,
    NULL,
};

typedef struct RfDeep {
    Map *visited; /* made on the first hard value */
} RfDeep;

/* Whether v1 can be part of a cycle, which is the kinds that point and are
 * not nil. */
static bool rf_deep_hard(ReflectValue v1, ReflectValue v2) {
    switch ((int)reflect_value_kind(v1)) {
    case KIND_POINTER:
    case KIND_MAP:
    case KIND_SLICE:
    case KIND_INTERFACE:
        return !reflect_value_is_nil(v1) && !reflect_value_is_nil(v2);
    default:
        return false;
    }
}

static const void *rf_deep_addr(ReflectValue v) {
    switch ((int)reflect_value_kind(v)) {
    case KIND_POINTER:
    case KIND_MAP:
        return reflect_value_unsafe_pointer(v);
    default:
        return v.ptr;
    }
}

/* Whether the pair has been seen, marking it seen if not. */
static bool rf_deep_seen(RfDeep *d, ReflectValue v1, ReflectValue v2) {
    const void *a1 = rf_deep_addr(v1);
    const void *a2 = rf_deep_addr(v2);
    if ((uintptr_t)a1 > (uintptr_t)a2) {
        const void *t = a1;
        a1 = a2;
        a2 = t;
    }
    RfVisit v;
    memset(&v, 0, sizeof v);
    v.a1 = a1;
    v.a2 = a2;
    v.typ = v1.typ;
    if (d->visited == NULL) {
        d->visited = map_make(heap_allocator(), &rf_visit_type, TYPE_BOOL, 0);
        if (d->visited == NULL)
            panic_str(BURROW_S("reflect: out of memory"));
    }
    if (map_get(d->visited, &v) != NULL)
        return true;
    bool yes = true;
    if (!map_set(d->visited, &v, &yes))
        panic_str(BURROW_S("reflect: out of memory"));
    return false;
}

static bool rf_deep(RfDeep *d, ReflectValue v1, ReflectValue v2) {
    if (!reflect_value_is_valid(v1) || !reflect_value_is_valid(v2))
        return reflect_value_is_valid(v1) == reflect_value_is_valid(v2);
    if (!burrow__reflect_identical(v1.typ, v2.typ))
        return false;
    if (rf_deep_hard(v1, v2) && rf_deep_seen(d, v1, v2))
        return true;

    switch ((int)v1.typ->kind) {
    case KIND_ARRAY:
        for (Int i = 0, n = reflect_value_len(v1); i < n; i++)
            if (!rf_deep(d, reflect_value_index(v1, i), reflect_value_index(v2, i)))
                return false;
        return true;
    case KIND_SLICE: {
        if (reflect_value_is_nil(v1) != reflect_value_is_nil(v2))
            return false;
        Int n = reflect_value_len(v1);
        if (n != reflect_value_len(v2))
            return false;
        if (reflect_value_unsafe_pointer(v1) == reflect_value_unsafe_pointer(v2))
            return true;
        if (v1.typ->elem->kind == KIND_UINT8) {
            Slice b1 = reflect_value_bytes(v1);
            Slice b2 = reflect_value_bytes(v2);
            return n == 0 || memcmp(b1.p, b2.p, (size_t)n) == 0;
        }
        for (Int i = 0; i < n; i++)
            if (!rf_deep(d, reflect_value_index(v1, i), reflect_value_index(v2, i)))
                return false;
        return true;
    }
    case KIND_INTERFACE:
        if (reflect_value_is_nil(v1) || reflect_value_is_nil(v2))
            return reflect_value_is_nil(v1) == reflect_value_is_nil(v2);
        return rf_deep(d, reflect_value_elem(v1), reflect_value_elem(v2));
    case KIND_POINTER:
        if (reflect_value_unsafe_pointer(v1) == reflect_value_unsafe_pointer(v2))
            return true;
        return rf_deep(d, reflect_value_elem(v1), reflect_value_elem(v2));
    case KIND_STRUCT:
        for (Int i = 0, n = reflect_value_num_field(v1); i < n; i++)
            if (!rf_deep(d, reflect_value_field(v1, i), reflect_value_field(v2, i)))
                return false;
        return true;
    case KIND_MAP: {
        if (reflect_value_is_nil(v1) != reflect_value_is_nil(v2))
            return false;
        if (reflect_value_len(v1) != reflect_value_len(v2))
            return false;
        if (reflect_value_unsafe_pointer(v1) == reflect_value_unsafe_pointer(v2))
            return true;
        ReflectMapIter it;
        reflect_map_iter_reset(&it, v1);
        while (reflect_map_iter_next(&it)) {
            ReflectValue val1 = reflect_map_iter_value(&it);
            ReflectValue val2 = reflect_value_map_index(v2, reflect_map_iter_key(&it));
            if (!reflect_value_is_valid(val1) || !reflect_value_is_valid(val2) ||
                !rf_deep(d, val1, val2))
                return false;
        }
        return true;
    }
    case KIND_FUNC:
        return reflect_value_is_nil(v1) && reflect_value_is_nil(v2);
    case KIND_INT:
    case KIND_INT8:
    case KIND_INT16:
    case KIND_INT32:
    case KIND_INT64:
        return reflect_value_int(v1) == reflect_value_int(v2);
    case KIND_UINT:
    case KIND_UINT8:
    case KIND_UINT16:
    case KIND_UINT32:
    case KIND_UINT64:
    case KIND_UINTPTR:
        return reflect_value_uint(v1) == reflect_value_uint(v2);
    case KIND_STRING:
        return str_eq(reflect_value_string(v1), reflect_value_string(v2));
    case KIND_BOOL:
        return reflect_value_bool(v1) == reflect_value_bool(v2);
    case KIND_FLOAT32:
    case KIND_FLOAT64:
        return reflect_value_float(v1) == reflect_value_float(v2);
    case KIND_COMPLEX64:
    case KIND_COMPLEX128: {
        Complex128 c1 = reflect_value_complex(v1);
        Complex128 c2 = reflect_value_complex(v2);
        return c1.re == c2.re && c1.im == c2.im;
    }
    default:
        /* Chan and UnsafePointer, which Go compares as interfaces, and that
         * comes down to the pointer. */
        return reflect_value_unsafe_pointer(v1) == reflect_value_unsafe_pointer(v2);
    }
}

bool reflect_deep_equal(Any x, Any y) {
    if (x.t == NULL || y.t == NULL)
        return x.t == NULL && y.t == NULL;
    ReflectValue v1 = reflect_value_of(x);
    ReflectValue v2 = reflect_value_of(y);
    if (!burrow__reflect_identical(v1.typ, v2.typ))
        return false;
    RfDeep d = {NULL};
    bool eq = rf_deep(&d, v1, v2);
    if (d.visited != NULL)
        map_free(d.visited);
    return eq;
}
