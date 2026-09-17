/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem/arena.h"

#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

/* Three things here are worth reading before changing anything.
 *
 * Chunks come from the parent already zeroed, and every chunk remembers how far
 * into itself anything has ever been written. Memory past that mark is still
 * untouched and therefore still zero, so handing it out costs no memset at all.
 * That is what makes Go's zero value rule almost free here, and it is why the
 * default allocator is the arena rather than malloc.
 *
 * Freeing the most recent allocation moves the bump pointer back. A loop that
 * appends to a growing buffer reallocates in place every time instead of
 * leaving a trail of dead copies behind it, which is the one pattern that would
 * otherwise make an arena look bad next to malloc.
 *
 * A reset keeps the chunks instead of returning them. The second request a
 * server handles, and every request after it, allocates nothing from the parent
 * at all. */

#define ARENA_ALIGN BURROW_ALIGN_MAX
#define ARENA_DEFAULT_CHUNK ((size_t)64 * 1024)
#define ARENA_MAX_CHUNK ((size_t)4 * 1024 * 1024)

struct ArenaChunk {
    ArenaChunk *next;
    unsigned char *data; /* where allocations start, correctly aligned */
    size_t total;        /* what was asked of the parent, needed to give it back */
    size_t cap;          /* usable bytes at data */
    size_t used;         /* bump offset */
    size_t dirty;        /* how far anything has ever been written */
};

static bool align_ok(size_t align) {
    return align != 0 && (align & (align - 1)) == 0;
}

static size_t round_up(size_t n, size_t align) {
    size_t r = n % align;
    return r == 0 ? n : n + (align - r);
}

static uintptr_t align_ptr(uintptr_t p, size_t align) {
    uintptr_t m = (uintptr_t)align - 1u;
    return (p + m) & ~m;
}

static void account_alloc(Arena *ar, size_t size) {
    ar->allocs++;
    ar->bytes_total += size;
    ar->bytes_live += size;
    if (ar->bytes_live > ar->bytes_peak)
        ar->bytes_peak = ar->bytes_live;
}

static void account_free(Arena *ar, size_t size) {
    ar->frees++;
    ar->bytes_live -= ar->bytes_live < size ? ar->bytes_live : size;
}

/* Moves the bump pointer and reports how many bytes at the front of the result
 * have been written before and therefore need clearing. Returns NULL when the
 * chunk cannot hold the request, which is not a failure, just a full chunk. */
static void *chunk_bump(ArenaChunk *c, size_t size, size_t align, size_t *dirty_bytes) {
    uintptr_t base = (uintptr_t)c->data;
    uintptr_t p = align_ptr(base + c->used, align);
    size_t off = (size_t)(p - base);
    if (off > c->cap || size > c->cap - off)
        return NULL;

    size_t end = off + size;
    if (c->dirty <= off)
        *dirty_bytes = 0;
    else
        *dirty_bytes = c->dirty < end ? c->dirty - off : size;

    if (end > c->dirty)
        c->dirty = end;
    c->used = end;
    return (void *)p;
}

/* A chunk that can hold size bytes at this alignment, from the spare list if
 * one fits and from the parent otherwise. */
static ArenaChunk *chunk_get(Arena *ar, size_t size, size_t align) {
    /* Worst case the alignment eats align minus one bytes at the front. */
    size_t slack = align > ARENA_ALIGN ? align - 1 : 0;
    if (size > SIZE_MAX - slack)
        return NULL;
    size_t need = size + slack;

    ArenaChunk **link = &ar->spare;
    while (*link != NULL) {
        if ((*link)->cap >= need) {
            ArenaChunk *c = *link;
            *link = c->next;
            c->next = NULL;
            c->used = 0;
            return c;
        }
        link = &(*link)->next;
    }

    size_t want = ar->chunk_size;
    bool ordinary = want >= need;
    if (!ordinary)
        want = need;

    size_t header = round_up(sizeof(ArenaChunk), ARENA_ALIGN);
    if (want > SIZE_MAX - header - ARENA_ALIGN)
        return NULL;
    size_t total = header + want + ARENA_ALIGN;

    ArenaChunk *c = (ArenaChunk *)mem_alloc(ar->parent, total, ARENA_ALIGN);
    if (c == NULL)
        return NULL;

    unsigned char *raw = (unsigned char *)c + header;
    c->data = (unsigned char *)align_ptr((uintptr_t)raw, ARENA_ALIGN);
    c->total = total;
    c->cap = total - header - (size_t)(c->data - raw);
    c->used = 0;
    c->dirty = 0;
    c->next = NULL;
    ar->blocks++;

    /* Later chunks get bigger, so a program that keeps allocating stops paying
     * a parent call every chunk_size bytes. A chunk sized for one oversized
     * request says nothing about the next one, so it does not count. */
    if (ordinary && ar->chunk_size < ARENA_MAX_CHUNK)
        ar->chunk_size *= 2;

    return c;
}

static void *arena_take(Arena *ar, size_t size, size_t align, bool zero) {
    if (!align_ok(align) || size == 0)
        return NULL;

    size_t dirty_bytes = 0;
    void *p = NULL;
    if (ar->live != NULL)
        p = chunk_bump(ar->live, size, align, &dirty_bytes);

    if (p == NULL) {
        ArenaChunk *c = chunk_get(ar, size, align);
        if (c == NULL)
            return NULL;
        c->next = ar->live;
        ar->live = c;
        p = chunk_bump(c, size, align, &dirty_bytes);
        if (p == NULL) /* cannot happen, and if it does we are not guessing */
            return NULL;
    }

    if (zero && dirty_bytes > 0)
        memset(p, 0, dirty_bytes);
    account_alloc(ar, size);
    return p;
}

static void *arena_vt_alloc(void *self, size_t size, size_t align) {
    return arena_take((Arena *)self, size, align, false);
}

static void *arena_vt_alloc_zeroed(void *self, size_t size, size_t align) {
    return arena_take((Arena *)self, size, align, true);
}

/* True when p is the most recent allocation still outstanding, which is what
 * lets free and realloc undo it rather than abandon it. */
static bool is_last(Arena *ar, const void *p, size_t size) {
    ArenaChunk *c = ar->live;
    if (c == NULL || p == NULL)
        return false;
    const unsigned char *q = (const unsigned char *)p;
    return q >= c->data && q + size == c->data + c->used;
}

static void *arena_vt_realloc(void *self, void *p, size_t old, size_t nsz,
                              size_t align) {
    Arena *ar = (Arena *)self;
    if (!align_ok(align) || nsz == 0)
        return NULL;

    if (is_last(ar, p, old)) {
        ArenaChunk *c = ar->live;
        size_t off = (size_t)((unsigned char *)p - c->data);
        if (nsz <= c->cap - off) {
            size_t end = off + nsz;
            if (end > c->dirty)
                c->dirty = end;
            c->used = end;
            /* mem_realloc clears anything past the old size, so growing in
             * place does not have to clear it here. */
            account_free(ar, old);
            account_alloc(ar, nsz);
            return p;
        }
    }

    void *q = arena_take(ar, nsz, align, false);
    if (q == NULL)
        return NULL;
    if (p != NULL)
        memcpy(q, p, old < nsz ? old : nsz);
    account_free(ar, old);
    return q;
}

static void arena_vt_free(void *self, void *p, size_t size, size_t align) {
    (void)align;
    Arena *ar = (Arena *)self;
    if (is_last(ar, p, size))
        ar->live->used = (size_t)((unsigned char *)p - ar->live->data);
    account_free(ar, size);
}

static void arena_vt_reset(void *self) {
    arena_reset((Arena *)self);
}

static AllocStats arena_vt_stats(void *self) {
    Arena *ar = (Arena *)self;
    AllocStats s;
    s.bytes_live = ar->bytes_live;
    s.bytes_peak = ar->bytes_peak;
    s.bytes_total = ar->bytes_total;
    s.allocs = ar->allocs;
    s.frees = ar->frees;
    s.blocks = ar->blocks;
    return s;
}

static const AllocVT arena_vt = {
    arena_vt_alloc, arena_vt_alloc_zeroed, arena_vt_realloc,
    arena_vt_free,  arena_vt_reset,        arena_vt_stats,
};

void arena_init(Arena *ar, Alloc *parent, size_t chunk_size) {
    if (ar == NULL)
        return;
    memset(ar, 0, sizeof(*ar));
    ar->alloc.vt = &arena_vt;
    ar->alloc.self = ar;
    ar->parent = parent != NULL ? parent : heap_allocator();
    ar->chunk_size = chunk_size != 0 ? chunk_size : ARENA_DEFAULT_CHUNK;
}

Alloc *arena_allocator(Arena *ar) {
    if (ar == NULL)
        return NULL;
    /* Repaired here as well as in arena_init so that an Arena copied by value,
     * which is a mistake but a very easy one, does not hand out an allocator
     * pointing at the original. */
    ar->alloc.vt = &arena_vt;
    ar->alloc.self = ar;
    return &ar->alloc;
}

void arena_reset(Arena *ar) {
    if (ar == NULL)
        return;
    while (ar->live != NULL) {
        ArenaChunk *next = ar->live->next;
        ar->live->used = 0;
        ar->live->next = ar->spare;
        ar->spare = ar->live;
        ar->live = next;
    }
    ar->bytes_live = 0;
}

static void chunks_release(Alloc *parent, ArenaChunk *c) {
    while (c != NULL) {
        ArenaChunk *next = c->next;
        mem_free(parent, c, c->total, ARENA_ALIGN);
        c = next;
    }
}

void arena_free(Arena *ar) {
    if (ar == NULL)
        return;
    chunks_release(ar->parent, ar->live);
    chunks_release(ar->parent, ar->spare);
    ar->live = NULL;
    ar->spare = NULL;
    ar->bytes_live = 0;
    ar->blocks = 0;
}
