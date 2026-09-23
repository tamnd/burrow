/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/heap.h"

#include "burrow/atomic.h"

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

static bool heap_align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

#if !defined(_WIN32)
/* aligned_alloc is C11 and wants a size that is a multiple of the alignment.
 * C17 dropped that requirement but we still compile against libraries that
 * enforce it, so round up rather than find out. */
static size_t heap_round_up(size_t n, size_t align) {
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
    size_t padded = heap_round_up(size, align);
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

/* What testing reads for a benchmark's allocs/op and B/op, Go's Mallocs and
 * TotalAlloc. Off unless a benchmark run turned it on, so the common path pays
 * one relaxed load and a branch that always goes the same way. On, it is two
 * atomic adds per allocation, which a benchmark that cares about allocations
 * is measuring anyway. A realloc counts as one allocation of the new size,
 * which is what growing a slice costs in Go. */
static uint32_t heap_counting;
static uint64_t heap_count_allocs;
static uint64_t heap_count_bytes;

static void heap_note(void *p, size_t size) {
    if (p == NULL || burrow__atomic_load_relaxed_u32(&heap_counting) == 0)
        return;
    burrow__atomic_add_u64(&heap_count_allocs, 1);
    burrow__atomic_add_u64(&heap_count_bytes, (uint64_t)size);
}

void burrow__heap_count(bool on) {
    burrow__atomic_store_relaxed_u32(&heap_counting, on ? 1U : 0U);
}

void burrow__heap_counts(uint64_t *allocs, uint64_t *bytes) {
    *allocs = burrow__atomic_load_u64(&heap_count_allocs);
    *bytes = burrow__atomic_load_u64(&heap_count_bytes);
}

static void *heap_alloc(void *self, size_t size, size_t align) {
    (void)self;
    if (!heap_align_ok(align) || size == 0)
        return NULL;
    void *p = heap_raw(size, align);
    heap_note(p, size);
    return p;
}

/* calloc rather than malloc plus memset, because a good allocator serving a
 * large request gets pages from the kernel that are already zero and calloc is
 * how it gets to tell us that. Over aligned requests have no calloc to use. */
static void *heap_alloc_zeroed(void *self, size_t size, size_t align) {
    (void)self;
    if (!heap_align_ok(align) || size == 0)
        return NULL;
    void *p;
    if (align <= BURROW_ALIGN_MAX) {
        p = calloc(1, size);
    } else {
        p = heap_raw(size, align);
        if (p != NULL)
            memset(p, 0, size);
    }
    heap_note(p, size);
    return p;
}

static void *heap_realloc_aligned(void *p, size_t old, size_t nsz, size_t align) {
#if defined(_WIN32)
    (void)old;
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

static void *heap_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    (void)self;
    if (!heap_align_ok(align) || nsz == 0)
        return NULL;
    void *q;
    if (p == NULL)
        q = heap_raw(nsz, align);
    else if (align <= BURROW_ALIGN_MAX)
        q = realloc(p, nsz);
    else
        q = heap_realloc_aligned(p, old, nsz, align);
    if (p == NULL || nsz > old)
        heap_note(q, nsz);
    return q;
}

static void heap_free(void *self, void *p, size_t size, size_t align) {
    (void)self;
    (void)size;
    if (p == NULL)
        return;
    heap_raw_free(p, heap_align_ok(align) ? align : 1);
}

static const AllocVT heap_vt = {
    heap_alloc, heap_alloc_zeroed, heap_realloc, heap_free, NULL, NULL,
};

static Alloc heap_singleton = {.vt = &heap_vt, .self = NULL};

Alloc *heap_allocator(void) {
    return &heap_singleton;
}
