/* container/heap, derived from Go's src/container/heap/heap.go (go1.27.1).
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/container/heap.h"

static Int heap_len(HeapInterface h) {
    return h.vt->sort.len(h.data);
}

static bool heap_less(HeapInterface h, Int i, Int j) {
    return h.vt->sort.less(h.data, i, j);
}

static void heap_swap(HeapInterface h, Int i, Int j) {
    h.vt->sort.swap(h.data, i, j);
}

static void heap_up(HeapInterface h, Int j) {
    for (;;) {
        Int i = (j - 1) / 2; /* parent */
        if (i == j || !heap_less(h, j, i))
            break;
        heap_swap(h, i, j);
        j = i;
    }
}

static bool heap_down(HeapInterface h, Int i0, Int n) {
    Int i = i0;
    for (;;) {
        /* Go checks j1 < 0 for overflow. i < n here, so 2*i + 1 is computed
         * unsigned and compared before it can wrap. */
        Uint u1 = 2 * (Uint)i + 1;
        if (u1 >= (Uint)n)
            break;
        Int j1 = (Int)u1;
        Int j = j1; /* left child */
        Int j2 = j1 + 1;
        if (j2 < n && heap_less(h, j2, j1))
            j = j2; /* right child */
        if (!heap_less(h, j, i))
            break;
        heap_swap(h, i, j);
        i = j;
    }
    return i > i0;
}

void heap_init(HeapInterface h) {
    Int n = heap_len(h);
    for (Int i = n / 2 - 1; i >= 0; i--)
        heap_down(h, i, n);
}

void heap_push(HeapInterface h, Any x) {
    h.vt->push(h.data, x);
    heap_up(h, heap_len(h) - 1);
}

Any heap_pop(HeapInterface h) {
    Int n = heap_len(h) - 1;
    heap_swap(h, 0, n);
    heap_down(h, 0, n);
    return h.vt->pop(h.data);
}

Any heap_remove(HeapInterface h, Int i) {
    Int n = heap_len(h) - 1;
    if (n != i) {
        heap_swap(h, i, n);
        if (!heap_down(h, i, n))
            heap_up(h, i);
    }
    return h.vt->pop(h.data);
}

void heap_fix(HeapInterface h, Int i) {
    if (!heap_down(h, i, heap_len(h)))
        heap_up(h, i);
}
