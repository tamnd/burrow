/* container/heap, a priority queue on top of anything that can be sorted.
 *
 * Go's container/heap. A heap is a tree kept in a slice, where every element
 * is no greater than its children as less sees it, so the smallest element is
 * always at index 0. The package does not own the slice. You bring a type
 * that is a SortInterface plus push and pop, and these functions keep it in
 * heap order:
 *
 *     heap_init(h);
 *     heap_push(h, BURROW_ANY_OF(3));
 *     Any smallest = heap_pop(h);
 *
 * push appends x as element len(), and pop removes and returns element
 * len() - 1. Those are the two methods the heap functions call to grow and
 * shrink it, and they are not meant to be called directly: heap_push and
 * heap_pop are the ones that keep the order.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package container/heap */

#ifndef BURROW_CONTAINER_HEAP_H
#define BURROW_CONTAINER_HEAP_H

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/sort.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* heap.Interface: sort.Interface with push and pop. The Any that pop returns
 * is whatever the type chooses, and it has to stay valid after the element is
 * gone, which usually means it points at memory the caller owns or at a copy
 * made with any_box. */
typedef struct HeapInterfaceVT {
    SortInterfaceVT sort;
    void (*push)(void *self, Any x);
    Any (*pop)(void *self);
} HeapInterfaceVT;

typedef struct HeapInterface {
    const HeapInterfaceVT *vt;
    void *data;
} HeapInterface;

/* Puts h in heap order. Idempotent, and O(n) for n = len(h). Call it once
 * before the other functions, or after changing many elements at once. */
void heap_init(HeapInterface h);

/* Adds x in O(log(n)). */
void heap_push(HeapInterface h, Any x);

/* Removes and returns the smallest element in O(log(n)). Same as
 * heap_remove(h, 0). */
BURROW_BORROWS(ret, h) Any heap_pop(HeapInterface h);

/* Removes and returns the element at index i in O(log(n)). */
BURROW_BORROWS(ret, h) Any heap_remove(HeapInterface h, Int i);

/* Restores heap order after the element at index i has changed its value, in
 * O(log(n)). Cheaper than removing it and pushing the new value. */
void heap_fix(HeapInterface h, Int i);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CONTAINER_HEAP_H */
