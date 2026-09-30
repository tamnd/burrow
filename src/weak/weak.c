/* weak, documented in burrow/weak.h.
 *
 * Go's runtime hangs a weak handle off the object itself and the collector
 * clears it. There is no collector here outside the gc allocator, so the
 * handles live in a table of slots, one per pointer weak_make has seen, and
 * the allocators tell the table when memory goes back through
 * burrow__mem_forget. A WeakPointer is a slot number and the slot's generation
 * at the time it was made. Freeing the memory bumps the generation, so every
 * WeakPointer made before reads NULL, and the slot can be used again for
 * something else without the old ones coming back to life.
 *
 * The slots are found two ways. weak_pointer_value has the slot number. weak_make
 * and burrow__mem_forget have an address, and look it up in an index of the live
 * slots sorted by address, which is a binary search for one address and for a
 * whole freed block alike.
 *
 * Under the gc allocator the collector frees memory without telling anyone,
 * so a slot for memory it owns also registers a disappearing link, which the
 * collector clears when the object goes. Such a slot stays in the index until
 * its address is freed or used again, or a sweep finds it cleared.
 *
 * Derived from Go's src/weak/pointer.go. Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/weak.h"

#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/gc.h"
#include "burrow/mem/heap.h"

#include <string.h>

/* Slots come in blocks that never move once made, because the collector holds
 * the address of a slot's link. */
#define WK_BLOCK 256
#define WK_NONE UINT32_MAX

typedef struct WkSlot {
    uintptr_t key; /* the address given to weak_make, stored inverted */
    void *link;    /* the same, for the collector to clear, when linked */
    uint32_t gen;  /* bumped each time the memory goes */
    uint32_t next; /* the next free slot, when this one is free */
    bool live;     /* in the index */
    bool linked;   /* a disappearing link is registered on link */
} WkSlot;

static burrow__Lock wk_lock;
static WkSlot **wk_blocks;
static uint32_t wk_nblocks;
static uint32_t wk_nslots;
static uint32_t wk_free = WK_NONE;
static uint32_t *wk_index; /* live slot numbers, sorted by key */
static uint32_t wk_len;
static uint32_t wk_cap;
static uint32_t wk_sweep_at; /* wk_len at which to look for cleared links */
static bool wk_hooked;

/* The table's own memory comes straight from the heap's vtable. Through
 * mem_realloc it would call burrow__mem_forget, which takes wk_lock, which is
 * already held. */
static void *wk_grow(void *p, size_t old, size_t nsz) {
    Alloc *h = heap_allocator();
    if (p == NULL)
        return h->vt->alloc(h->self, nsz, BURROW_ALIGN_MAX);
    return h->vt->realloc(h->self, p, old, nsz, BURROW_ALIGN_MAX);
}

static WkSlot *wk_slot(uint32_t i) {
    return &wk_blocks[i / WK_BLOCK][i % WK_BLOCK];
}

/* Keys are stored as ~address. A build of the collector that also collects
 * malloc's memory scans this table, and an address it can read there would
 * keep the object alive. The order is reversed, and a range of addresses is
 * still a range of keys. */
static uintptr_t wk_key(const void *p) {
    return ~(uintptr_t)p;
}

/* The first index position whose key is at least k. */
static uint32_t wk_search(uintptr_t k) {
    uint32_t lo = 0;
    uint32_t hi = wk_len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (wk_slot(wk_index[mid])->key < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The memory behind slot i has gone. Every WeakPointer made from it reads NULL
 * from now on, and the slot goes on the free list. */
static void wk_kill(uint32_t i) {
    WkSlot *s = wk_slot(i);
    if (s->linked)
        burrow__gc_unlink(&s->link);
    s->linked = false;
    s->link = NULL;
    s->live = false;
    s->gen++;
    if (s->gen == 0)
        s->gen = 1;
    s->next = wk_free;
    wk_free = i;
}

/* Drops the slots whose object the collector has freed. */
static void wk_sweep(void) {
    uint32_t n = 0;
    for (uint32_t j = 0; j < wk_len; j++) {
        uint32_t i = wk_index[j];
        WkSlot *s = wk_slot(i);
        if (s->linked && burrow__gc_read(&s->link) == NULL)
            wk_kill(i);
        else
            wk_index[n++] = i;
    }
    wk_len = n;
    wk_sweep_at = 2 * wk_len > 64 ? 2 * wk_len : 64;
}

static void wk_forget(const void *p, size_t n) {
    uintptr_t lo = (uintptr_t)p;
    uintptr_t hi = lo + (n - 1);
    if (hi < lo)
        hi = UINTPTR_MAX;
    burrow__lock(&wk_lock);
    /* The keys of [lo, hi] are [~hi, ~lo]. */
    uint32_t a = wk_search(~hi);
    uint32_t b = a;
    while (b < wk_len && wk_slot(wk_index[b])->key <= ~lo)
        wk_kill(wk_index[b++]);
    if (b > a) {
        memmove(wk_index + a, wk_index + b, (size_t)(wk_len - b) * sizeof(*wk_index));
        wk_len -= b - a;
    }
    burrow__unlock(&wk_lock);
}

static uint32_t wk_new_slot(void) {
    if (wk_free != WK_NONE) {
        uint32_t i = wk_free;
        wk_free = wk_slot(i)->next;
        return i;
    }
    if (wk_nslots == wk_nblocks * WK_BLOCK) {
        if (wk_nblocks == UINT32_MAX / WK_BLOCK)
            return WK_NONE;
        WkSlot *blk = wk_grow(NULL, 0, WK_BLOCK * sizeof(WkSlot));
        if (blk == NULL)
            return WK_NONE;
        WkSlot **bs = wk_grow(wk_blocks, wk_nblocks * sizeof(*wk_blocks),
                              (wk_nblocks + 1) * sizeof(*wk_blocks));
        if (bs == NULL) {
            Alloc *h = heap_allocator();
            h->vt->free(h->self, blk, WK_BLOCK * sizeof(WkSlot), BURROW_ALIGN_MAX);
            return WK_NONE;
        }
        memset(blk, 0, WK_BLOCK * sizeof(WkSlot));
        for (uint32_t j = 0; j < WK_BLOCK; j++)
            blk[j].gen = 1;
        bs[wk_nblocks] = blk;
        wk_blocks = bs;
        wk_nblocks++;
    }
    return wk_nslots++;
}

static WeakPointer wk_handle(uint32_t i) {
    WeakPointer w;
    w.h = (uint64_t)wk_slot(i)->gen << 32 | ((uint64_t)i + 1);
    return w;
}

WeakPointer weak_make(const void *p) {
    WeakPointer zero = {0};
    if (p == NULL)
        return zero;

    burrow__lock(&wk_lock);
    if (!wk_hooked) {
        burrow__mem_set_forget(wk_forget);
        wk_hooked = true;
    }

    uintptr_t k = wk_key(p);
    uint32_t at = wk_search(k);
    if (at < wk_len && wk_slot(wk_index[at])->key == k) {
        uint32_t i = wk_index[at];
        WkSlot *s = wk_slot(i);
        if (!s->linked || burrow__gc_read(&s->link) != NULL) {
            WeakPointer w = wk_handle(i);
            burrow__unlock(&wk_lock);
            return w;
        }
        /* The collector freed what was here and this is something new at the
         * same address. */
        wk_kill(i);
        memmove(wk_index + at, wk_index + at + 1,
                (size_t)(wk_len - at - 1) * sizeof(*wk_index));
        wk_len--;
    }

    if (wk_len >= wk_sweep_at) {
        wk_sweep();
        at = wk_search(k);
    }
    if (wk_len == wk_cap) {
        uint32_t nc = wk_cap < 64 ? 64 : wk_cap * 2;
        uint32_t *ni = nc > wk_cap ? wk_grow(wk_index, wk_cap * sizeof(*wk_index),
                                             nc * sizeof(*wk_index))
                                   : NULL;
        if (ni == NULL) {
            burrow__unlock(&wk_lock);
            return zero;
        }
        wk_index = ni;
        wk_cap = nc;
    }
    uint32_t i = wk_new_slot();
    if (i == WK_NONE) {
        burrow__unlock(&wk_lock);
        return zero;
    }

    WkSlot *s = wk_slot(i);
    s->key = k;
    s->link = NULL;
    s->linked = false;
    if (burrow__gc_owns(p))
        s->linked = burrow__gc_link(&s->link, p);
    s->live = true;
    memmove(wk_index + at + 1, wk_index + at,
            (size_t)(wk_len - at) * sizeof(*wk_index));
    wk_index[at] = i;
    wk_len++;

    WeakPointer w = wk_handle(i);
    burrow__unlock(&wk_lock);
    return w;
}

void *weak_pointer_value(WeakPointer w) {
    if (w.h == 0)
        return NULL;
    uint64_t n = w.h & UINT32_MAX;
    uint32_t gen = (uint32_t)(w.h >> 32);
    void *v = NULL;
    burrow__lock(&wk_lock);
    if (n >= 1 && n <= wk_nslots) {
        WkSlot *s = wk_slot((uint32_t)(n - 1));
        if (s->live && s->gen == gen)
            v = s->linked ? burrow__gc_read(&s->link)
                          : (void *)~s->key; // NOLINT(performance-no-int-to-ptr)
    }
    burrow__unlock(&wk_lock);
    return v;
}
