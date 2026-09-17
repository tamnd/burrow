/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/heap.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Windows spells the over aligned allocators its own way and does not ship
 * C11's aligned_alloc, under either compiler. */
#include <malloc.h>
#endif

/* This is the only file in the tree allowed to call malloc, and tools/check
 * banned.sh is what makes that true rather than a promise in a comment.
 *
 * The one thing here that is not a passthrough is alignment. C's malloc
 * guarantees alignment suitable for any fundamental type and no more, so a
 * request for more than that has to go somewhere else, and every platform
 * spells that somewhere else differently. Everything below the threshold takes
 * the ordinary path, which is almost every allocation this library will ever
 * make. */

static bool align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

#if !defined(_WIN32)
/* aligned_alloc is C11 and wants a size that is a multiple of the alignment.
 * C17 dropped that requirement but we still compile against libraries that
 * enforce it, so round up rather than find out. */
static size_t round_up(size_t n, size_t align) {
    size_t r = n % align;
    return r == 0 ? n : n + (align - r);
}
#endif

static void *heap_raw(size_t size, size_t align) {
    if (align <= BURROW_ALIGN_MAX)
        return malloc(size);
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    size_t padded = round_up(size, align);
    if (padded < size) /* the rounding wrapped, which means size was absurd */
        return NULL;
    return aligned_alloc(align, padded);
#endif
}

static void heap_raw_free(void *p, size_t align) {
    if (align <= BURROW_ALIGN_MAX) {
        free(p);
        return;
    }
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

static void *heap_alloc(void *self, size_t size, size_t align) {
    (void)self;
    if (!align_ok(align) || size == 0)
        return NULL;
    return heap_raw(size, align);
}

/* calloc rather than malloc plus memset, because a good allocator serving a
 * large request gets pages from the kernel that are already zero and calloc is
 * how it gets to tell us that. Over aligned requests have no calloc to use. */
static void *heap_alloc_zeroed(void *self, size_t size, size_t align) {
    (void)self;
    if (!align_ok(align) || size == 0)
        return NULL;
    if (align <= BURROW_ALIGN_MAX)
        return calloc(1, size);
    void *p = heap_raw(size, align);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

static void *heap_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    (void)self;
    if (!align_ok(align) || nsz == 0)
        return NULL;
    if (p == NULL)
        return heap_raw(nsz, align);
    if (align <= BURROW_ALIGN_MAX)
        return realloc(p, nsz);
#if defined(_WIN32)
    return _aligned_realloc(p, nsz, align);
#else
    /* There is no aligned realloc in standard C, so grow the honest way. The
     * caller told us the old size, which is the only reason this can copy the
     * right number of bytes. */
    void *q = heap_raw(nsz, align);
    if (q == NULL)
        return NULL;
    memcpy(q, p, old < nsz ? old : nsz);
    heap_raw_free(p, align);
    return q;
#endif
}

static void heap_free(void *self, void *p, size_t size, size_t align) {
    (void)self;
    (void)size;
    if (p == NULL)
        return;
    heap_raw_free(p, align_ok(align) ? align : 1);
}

static const AllocVT heap_vt = {
    heap_alloc, heap_alloc_zeroed, heap_realloc, heap_free, NULL, NULL,
};

static Alloc heap_singleton = {&heap_vt, NULL};

Alloc *heap_allocator(void) {
    return &heap_singleton;
}
