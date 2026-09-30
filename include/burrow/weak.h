/* weak, pointers that do not keep memory alive.
 *
 * Go's weak. weak_make takes a pointer and hands back a WeakPointer for it,
 * and weak_pointer_value gives the pointer back for as long as the memory is
 * still there, and NULL once it has gone. Caches and canonicalising maps use
 * it to hold on to something without being the reason it stays.
 *
 *     Alloc *a = heap_allocator();
 *     int *p = BURROW_NEW(a, int);
 *     WeakPointer w = weak_make(p);
 *     int *q = weak_pointer_value(w);             // p
 *     mem_free(a, p, sizeof(int), _Alignof(int));
 *     q = weak_pointer_value(w);                  // NULL
 *
 * When the memory goes depends on the allocator it came from. Under the gc
 * allocator it is as in Go: the pointer reads NULL once the collector has
 * found nothing else reaching the object, and mem_free, which does nothing
 * there, changes nothing. Under every other allocator memory goes when it is
 * given back, so a WeakPointer reads NULL from the moment its memory is passed
 * to mem_free, or to mem_realloc even when the block grows in place, or is
 * dropped by arena_reset, arena_release, arena_free or fixed_reset. Memory
 * that no allocator handed out, such as a global or a string literal, never
 * goes, and its WeakPointer never reads NULL.
 *
 * Two WeakPointers are equal when they were made from the same pointer while
 * the memory behind it was the same memory. Pointers to different fields of
 * one object make different WeakPointers, as in Go. Once the memory goes, a
 * new object at the same address makes a WeakPointer that is not equal to the
 * old one, and the old one does not see it.
 *
 * A zeroed WeakPointer is Go's zero Pointer. Its value is NULL, and it is
 * what weak_make(NULL) returns.
 *
 * Every function here is safe to call from any number of threads.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package weak */

#ifndef BURROW_WEAK_H
#define BURROW_WEAK_H

#include "burrow/core.h"
#include "burrow/own.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* weak.Pointer. Eight bytes, and it can be copied, compared with
 * weak_pointer_eq and used as a map key. */
typedef struct WeakPointer {
    uint64_t h;
} WeakPointer;

/* weak.Make. The WeakPointer for p, which may point into the middle of an
 * object. Gives the zero value for NULL, and also when the process is out of
 * memory for the table the WeakPointers live in, where Go would stop. */
WeakPointer weak_make(const void *p);

/* weak.Pointer.Value. The pointer w was made from, or NULL if its memory has
 * gone. */
BURROW_BORROWS(ret) void *weak_pointer_value(WeakPointer w);

/* Go's w == v. */
static inline bool weak_pointer_eq(WeakPointer w, WeakPointer v) {
    return w.h == v.h;
}

#ifdef __cplusplus
}
#endif

#endif /* BURROW_WEAK_H */
