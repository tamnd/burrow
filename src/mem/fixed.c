/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/fixed.h"

#include <stdint.h>
#include <string.h>

/* The arena without the chunks. It cannot grow, so it does not need a parent,
 * and it does not track which bytes are untouched because it has no idea what
 * was in the caller's buffer to begin with. That last point is why there is no
 * alloc_zeroed here: mem_alloc does the memset, every time, which is the
 * honest answer when you cannot prove the memory is already zero. */

static bool fixed_align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

static uintptr_t fixed_align_ptr(uintptr_t p, size_t align) {
    uintptr_t m = (uintptr_t)align - 1U;
    return (p + m) & ~m;
}

static void *fixed_vt_alloc(void *self, size_t size, size_t align) {
    Fixed *fx = (Fixed *)self;
    if (!fixed_align_ok(align) || size == 0 || fx->base == NULL)
        return NULL;

    uintptr_t base = (uintptr_t)fx->base;
    uintptr_t p = fixed_align_ptr(base + fx->used, align);
    size_t off = (size_t)(p - base);
    if (off > fx->cap || size > fx->cap - off)
        return NULL;

    fx->used = off + size;
    fx->allocs++;
    fx->bytes_total += size;
    if ((uint64_t)fx->used > fx->bytes_peak)
        fx->bytes_peak = (uint64_t)fx->used;
    return (void *)p;
}

static bool fixed_is_last(const Fixed *fx, const void *p, size_t size) {
    if (p == NULL || fx->base == NULL)
        return false;
    const unsigned char *q = (const unsigned char *)p;
    return q >= fx->base && q + size == fx->base + fx->used;
}

static void *fixed_vt_realloc(void *self, void *p, size_t old, size_t nsz,
                              size_t align) {
    Fixed *fx = (Fixed *)self;
    if (!fixed_align_ok(align) || nsz == 0)
        return NULL;

    if (fixed_is_last(fx, p, old)) {
        size_t off = (size_t)((unsigned char *)p - fx->base);
        if (nsz <= fx->cap - off) {
            fx->used = off + nsz;
            if ((uint64_t)fx->used > fx->bytes_peak)
                fx->bytes_peak = (uint64_t)fx->used;
            return p;
        }
        return NULL;
    }

    void *q = fixed_vt_alloc(self, nsz, align);
    if (q == NULL)
        return NULL;
    if (p != NULL)
        memcpy(q, p, old < nsz ? old : nsz);
    return q;
}

static void fixed_vt_free(void *self, void *p, size_t size, size_t align) {
    (void)align;
    Fixed *fx = (Fixed *)self;
    if (fixed_is_last(fx, p, size))
        fx->used = (size_t)((unsigned char *)p - fx->base);
    fx->frees++;
}

static void fixed_vt_reset(void *self) {
    fixed_reset((Fixed *)self);
}

static AllocStats fixed_vt_stats(void *self) {
    Fixed *fx = (Fixed *)self;
    AllocStats s;
    s.bytes_live = (uint64_t)fx->used;
    s.bytes_peak = fx->bytes_peak;
    s.bytes_total = fx->bytes_total;
    s.allocs = fx->allocs;
    s.frees = fx->frees;
    s.blocks = fx->base != NULL ? 1U : 0U;
    return s;
}

static const AllocVT fixed_vt = {
    fixed_vt_alloc, NULL,           fixed_vt_realloc,
    fixed_vt_free,  fixed_vt_reset, fixed_vt_stats,
};

void fixed_init(Fixed *fx, void *buf, size_t size) {
    if (fx == NULL)
        return;
    memset(fx, 0, sizeof(*fx));
    fx->alloc.vt = &fixed_vt;
    fx->alloc.self = fx;
    fx->base = (unsigned char *)buf;
    fx->cap = buf != NULL ? size : 0;
}

Alloc *fixed_allocator(Fixed *fx) {
    if (fx == NULL)
        return NULL;
    fx->alloc.vt = &fixed_vt;
    fx->alloc.self = fx;
    return &fx->alloc;
}

void fixed_reset(Fixed *fx) {
    if (fx == NULL)
        return;
    fx->used = 0;
}

size_t fixed_available(const Fixed *fx) {
    if (fx == NULL)
        return 0;
    return fx->cap - fx->used;
}
