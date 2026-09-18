/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem.h"

#include <string.h>

/* Everything here is a thin dispatch through the vtable plus the two policies
 * that belong to the interface rather than to any backend: allocations are
 * zeroed, and an element count times an element size is checked for overflow.
 *
 * Putting the zeroing here rather than in every backend means a backend author
 * cannot forget it, and the alloc_zeroed hook means the backends that already
 * know the memory is zero do not pay for it twice. */

/* One place decides what happens when a backend says no, which is why the hook
 * is on the interface and not on any backend. An allocator somebody wrote
 * themselves gets the same treatment as the four in this tree, without knowing
 * the hook exists.
 *
 * Once, not in a loop. A handler that answers true without having freed
 * anything would spin here forever, and above one there is no number of
 * attempts anybody can defend. */
static bool oom_retry(Alloc *a, size_t size, size_t align) {
    if (a->oom == NULL)
        return false;
    return a->oom(a->oom_ctx, size, align);
}

void *mem_alloc(Alloc *a, size_t size, size_t align) {
    if (a == NULL || a->vt == NULL)
        return NULL;
    if (size == 0)
        return NULL;
    if (a->vt->alloc_zeroed != NULL) {
        void *z = a->vt->alloc_zeroed(a->self, size, align);
        if (z == NULL && oom_retry(a, size, align))
            z = a->vt->alloc_zeroed(a->self, size, align);
        return z;
    }
    void *p = a->vt->alloc(a->self, size, align);
    if (p == NULL && oom_retry(a, size, align))
        p = a->vt->alloc(a->self, size, align);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

void *mem_alloc_nozero(Alloc *a, size_t size, size_t align) {
    if (a == NULL || a->vt == NULL)
        return NULL;
    if (size == 0)
        return NULL;
    void *p = a->vt->alloc(a->self, size, align);
    if (p == NULL && oom_retry(a, size, align))
        p = a->vt->alloc(a->self, size, align);
    return p;
}

void *mem_alloc_array(Alloc *a, size_t n, size_t size, size_t align) {
    if (n != 0 && size > SIZE_MAX / n)
        return NULL;
    return mem_alloc(a, n * size, align);
}

void *mem_realloc(Alloc *a, void *p, size_t old, size_t nsz, size_t align) {
    if (a == NULL || a->vt == NULL)
        return NULL;
    if (p == NULL)
        return mem_alloc(a, nsz, align);
    if (nsz == 0) {
        a->vt->free(a->self, p, old, align);
        return NULL;
    }
    void *q = a->vt->realloc(a->self, p, old, nsz, align);
    if (q == NULL && oom_retry(a, nsz, align))
        q = a->vt->realloc(a->self, p, old, nsz, align);
    /* Growing past the old size exposes memory the caller never wrote, and Go
     * would have zeroed it, so we do. Shrinking exposes nothing. */
    if (q != NULL && nsz > old)
        memset((unsigned char *)q + old, 0, nsz - old);
    return q;
}

void mem_free(Alloc *a, void *p, size_t size, size_t align) {
    if (a == NULL || a->vt == NULL || p == NULL)
        return;
    a->vt->free(a->self, p, size, align);
}

void mem_reset(Alloc *a) {
    if (a == NULL || a->vt == NULL || a->vt->reset == NULL)
        return;
    a->vt->reset(a->self);
}

bool mem_can_reset(Alloc *a) {
    return a != NULL && a->vt != NULL && a->vt->reset != NULL;
}

AllocStats mem_stats(Alloc *a) {
    AllocStats zero = {0, 0, 0, 0, 0, 0};
    if (a == NULL || a->vt == NULL || a->vt->stats == NULL)
        return zero;
    return a->vt->stats(a->self);
}

void mem_set_oom(Alloc *a, OomFunc fn, void *ctx) {
    if (a == NULL)
        return;
    a->oom = fn;
    a->oom_ctx = ctx;
}
