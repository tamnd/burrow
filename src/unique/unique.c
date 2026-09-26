/* unique, canonical copies of comparable values.
 *
 * Go keeps a concurrent hash trie per type and lets the collector empty it.
 * Here each type gets an open addressed table of pointers to canonical
 * copies, found through a list of the types seen so far. Reading either one
 * takes no lock, so handles for values that already have one are made in
 * parallel without the threads touching a shared cache line for writing.
 * Adding a value takes one lock, which also covers growing a table: the
 * bigger table is filled in and then published, and a reader still on the
 * old one either finds its value there or falls through to the lock and
 * looks again. Old tables are kept, as the canonical copies are, since a
 * reader may be in one.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/unique.h"

#include "burrow/atomic.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"

#include <string.h>

static const Field uq_handle_fields[] = {
    {{(const Byte *)"value", 5}, {NULL, 0}, &burrow_type_UnsafePointer, 0},
};

const Type burrow_type_UniqueHandle = {
    {(const Byte *)"Handle", 6},
    {(const Byte *)"unique", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(UniqueHandle),
    (uint16_t)_Alignof(UniqueHandle),
    1,
    0,
    uq_handle_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

typedef struct UqTable {
    struct UqTable *prev; /* the table this one replaced, kept for readers */
    Uint mask;            /* slots less one, a power of two less one */
    Int len;              /* slots in use */
    void *slots[];        /* canonical copies, NULL for an empty slot */
} UqTable;

typedef struct UqType {
    struct UqType *next;
    const Type *t;
    void *table; /* UqTable *, published with a release store */
} UqType;

static burrow__Lock uq_lock;
static void *uq_types; /* UqType *, the head, published with a release store */
static uint64_t uq_seed;
static Arena uq_arena;
static bool uq_ready;

/* Go's zero, the one address every zero sized value gets. */
static const uintptr_t uq_zero;

static const Str uq_oom = {(const Byte *)"unique: out of memory", 21};

BURROW_NORETURN static void uq_out_of_memory(void) {
    burrow__unlock(&uq_lock);
    panic_str(uq_oom);
}

/* Go's clone and makeCloneSeq in one walk: every string reachable from p
 * without going through a pointer gets bytes of its own. */
static bool uq_clone(Alloc *a, const Type *t, Byte *p) {
    if (t->kind == KIND_STRING) {
        Str s;
        memcpy(&s, p, sizeof s);
        Byte *b = NULL;
        if (s.len > 0) {
            b = mem_alloc_nozero(a, (size_t)s.len, 1);
            if (b == NULL)
                return false;
            memcpy(b, s.p, (size_t)s.len);
        }
        s.p = b;
        memcpy(p, &s, sizeof s);
        return true;
    }
    if (t->kind == KIND_STRUCT) {
        for (uint16_t i = 0; i < t->nfield; i++)
            if (!uq_clone(a, t->fields[i].type, p + t->fields[i].offset))
                return false;
    } else if (t->kind == KIND_ARRAY) {
        for (uint32_t i = 0; i < t->len; i++)
            if (!uq_clone(a, t->elem, p + (size_t)i * t->elem->size))
                return false;
    }
    return true;
}

static UqType *uq_find_type(const Type *t) {
    for (UqType *n = burrow__atomic_load_acquire_ptr(&uq_types); n != NULL; n = n->next)
        if (n->t == t)
            return n;
    return NULL;
}

static const void *uq_lookup(const UqTable *tab, const Type *t, uint64_t h,
                             const void *value) {
    for (Uint i = (Uint)h & tab->mask;; i = (i + 1) & tab->mask) {
        const void *p = burrow__atomic_load_acquire_ptr(&tab->slots[i]);
        if (p == NULL)
            return NULL;
        if (type_equal(t, p, value))
            return p;
    }
}

static UqTable *uq_new_table(Uint nslots) {
    UqTable *tab = mem_alloc(
        heap_allocator(), sizeof(UqTable) + nslots * sizeof(void *), _Alignof(UqTable));
    if (tab == NULL)
        uq_out_of_memory();
    tab->mask = nslots - 1;
    return tab;
}

/* Adds canon, which is not in tab, to it. The slot is written last and with
 * a release store, so a reader that sees the pointer sees the value. */
static void uq_put(UqTable *tab, uint64_t h, void *canon) {
    Uint i = (Uint)h & tab->mask;
    while (tab->slots[i] != NULL)
        i = (i + 1) & tab->mask;
    burrow__atomic_store_release_ptr(&tab->slots[i], canon);
    tab->len++;
}

/* The slow path, under the lock. */
static const void *uq_insert(const Type *t, const void *value) {
    if (!uq_ready) {
        arena_init(&uq_arena, heap_allocator(), 0);
        uq_seed = runtime_rand64();
        uq_ready = true;
    }
    UqType *n = uq_find_type(t);
    if (n == NULL) {
        n = mem_alloc(heap_allocator(), sizeof *n, _Alignof(UqType));
        if (n == NULL)
            uq_out_of_memory();
        n->t = t;
        n->table = uq_new_table(8);
        n->next = uq_types;
        burrow__atomic_store_release_ptr(&uq_types, n);
    }
    UqTable *tab = n->table;
    uint64_t h = type_hash(t, value, uq_seed);
    const void *found = uq_lookup(tab, t, h, value);
    if (found != NULL)
        return found;

    if ((tab->len + 1) * 4 > ((Int)tab->mask + 1) * 3) {
        UqTable *bigger = uq_new_table((tab->mask + 1) * 2);
        bigger->prev = tab;
        for (Uint i = 0; i <= tab->mask; i++)
            if (tab->slots[i] != NULL)
                uq_put(bigger, type_hash(t, tab->slots[i], uq_seed), tab->slots[i]);
        burrow__atomic_store_release_ptr(&n->table, bigger);
        tab = bigger;
    }

    Alloc *a = arena_allocator(&uq_arena);
    Byte *c = mem_alloc_nozero(a, t->size, t->align);
    if (c == NULL)
        uq_out_of_memory();
    memcpy(c, value, t->size);
    if (!uq_clone(a, t, c))
        uq_out_of_memory();
    uq_put(tab, h, c);
    return c;
}

UniqueHandle unique_make(const Type *t, const void *value) {
    if (t->size == 0)
        return (UniqueHandle){&uq_zero};
    if (!type_is_comparable(t))
        panic_str(BURROW_S("unique: Make of a type that is not comparable"));
    UqType *n = uq_find_type(t);
    if (n != NULL) {
        /* uq_seed was written before n was published, so it is safe to
         * read once n has been seen. */
        const UqTable *tab = burrow__atomic_load_acquire_ptr(&n->table);
        const void *p = uq_lookup(tab, t, type_hash(t, value, uq_seed), value);
        if (p != NULL)
            return (UniqueHandle){p};
    }
    burrow__lock(&uq_lock);
    const void *p = uq_insert(t, value);
    burrow__unlock(&uq_lock);
    return (UniqueHandle){p};
}

const void *unique_handle_value(UniqueHandle h) {
    return h.value;
}
