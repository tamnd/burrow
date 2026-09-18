/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/gc.h"

/* The whole file compiles either way. Without BURROW_ENABLE_BOEHM the three
 * public functions are still here and still link, they just answer no, which is
 * what keeps this out of the build system's way: there is one flag and it turns
 * a real collector on, rather than a file that appears and disappears.
 *
 * Written against the Boehm collector's documented interface and nothing else.
 * Every call below is one of GC_INIT, GC_malloc, GC_memalign, GC_realloc,
 * GC_gcollect and the three counters, and that is the entire dependency.
 *
 * -Wundef is on, so this is defined() and not a bare #if. */
#if defined(BURROW_ENABLE_BOEHM) && BURROW_ENABLE_BOEHM

#include <gc.h>
#include <string.h>

/* What the collector aligns to without being asked. Boehm documents a pointer
 * sized granule and in practice gives more, and the difference between what is
 * documented and what is observed is exactly where an alignment bug lives, so
 * anything stricter than this goes the explicit route. It costs a few bytes of
 * padding on the requests that need it and nothing on the ones that do not. */
#define GC_GUARANTEED_ALIGN (sizeof(void *))

/* Boehm has to be started, and it has to be started from the thread whose stack
 * it is going to scan. GC_INIT is documented as safe to call more than once, so
 * the flag here is not for correctness, it is so that the common path is a load
 * and a branch rather than a trip into the collector. It is a plain bool because
 * the rule is already that the first call comes from the main thread before any
 * others exist, and a lock would not make a later first call correct anyway. */
static bool started;

static void start_once(void) {
    if (started)
        return;
    GC_INIT();
    started = true;
}

static bool align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

/* GC_malloc hands back zeroed memory, so there is no alloc_zeroed variant
 * below and mem_alloc's memset is skipped: one function does both jobs. */
static void *gc_raw(void *self, size_t size, size_t align) {
    (void)self;
    if (!align_ok(align) || size == 0)
        return NULL;
    if (align <= GC_GUARANTEED_ALIGN)
        return GC_malloc(size);
    return GC_memalign(align, size);
}

static void *gc_grow(void *self, void *p, size_t old, size_t nsz, size_t align) {
    if (!align_ok(align) || nsz == 0)
        return NULL;
    if (p == NULL)
        return gc_raw(self, nsz, align);
    if (align <= GC_GUARANTEED_ALIGN)
        return GC_realloc(p, nsz);

    /* An over aligned block came from GC_memalign, which is allowed to hand back
     * a pointer into the middle of the object it really allocated. GC_realloc on
     * one of those is asking the collector about an address it does not think is
     * the start of anything, so the honest move is a fresh block and a copy. The
     * old one is not given back, because nothing here ever gives anything back
     * and the collector will notice when the last reference goes. */
    void *q = gc_raw(self, nsz, align);
    if (q == NULL)
        return NULL;
    memcpy(q, p, old < nsz ? old : nsz);
    return q;
}

/* Deliberately nothing. This is not an unimplemented slot, it is the collector's
 * answer to the question: memory goes away when nothing can reach it, and a
 * caller saying it is finished is not the same claim. Doing anything else here
 * would also be unsafe for the over aligned case above, where the pointer the
 * caller holds is not the pointer the collector knows about. */
static void gc_release(void *self, void *p, size_t size, size_t align) {
    (void)self;
    (void)p;
    (void)size;
    (void)align;
}

/* The collector's own numbers, which do not line up field for field with what a
 * bump pointer or malloc would report, so only the ones that mean the same thing
 * get filled in. bytes_live is the heap minus what the collector knows is free,
 * which counts its own per object overhead as well as yours. blocks is the
 * collection count, because for this backend that is the number somebody
 * watching a program actually wants. The rest stay zero rather than being
 * invented: the collector does not count allocations, and there is nothing it
 * could tell us about frees that would be true. */
static AllocStats gc_stats(void *self) {
    (void)self;
    AllocStats st = {0, 0, 0, 0, 0, 0};
    size_t heap = GC_get_heap_size();
    size_t idle = GC_get_free_bytes();
    st.bytes_live = (uint64_t)(heap > idle ? heap - idle : 0);
    st.bytes_total = (uint64_t)GC_get_total_bytes();
    st.blocks = (uint64_t)GC_get_gc_no();
    return st;
}

/* reset is NULL because there is no moment at which everything is known to be
 * dead, which is the one thing reset means. mem_can_reset says so. */
static const AllocVT gc_vt = {
    gc_raw, gc_raw, gc_grow, gc_release, NULL, gc_stats,
};

static Alloc gc_singleton = {.vt = &gc_vt, .self = NULL};

bool gc_available(void) {
    return true;
}

Alloc *gc_allocator(void) {
    start_once();
    return &gc_singleton;
}

void gc_collect(void) {
    start_once();
    GC_gcollect();
}

#else

bool gc_available(void) {
    return false;
}

Alloc *gc_allocator(void) {
    return NULL;
}

void gc_collect(void) {}

#endif /* BURROW_ENABLE_BOEHM */
