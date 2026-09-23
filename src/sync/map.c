/* sync.Map. See burrow/sync.h.
 *
 * A hash trie, sixteen children per node, four hash bits per level. A read is a
 * walk down at most sixteen pointers with no lock and no store to any shared
 * line, which is the whole reason this exists rather than a Mutex around a
 * plain Map. A write locks the one node holding the slot it is changing.
 *
 * The part Go does not have to write is here. Go replaces an entry node by
 * storing a new one over it and walks away, because the collector will notice
 * the old one when the last reader lets go. Every one of those places is a
 * burrow__retire here, and every read is wrapped in a pin so that a retire
 * knows when the reader has let go. That is the only structural difference from
 * the Go source, and it is why the node types carry three extra fields.
 *
 * The lock on an indirect node is the runtime's lock and not a sync.Mutex. A
 * reader that has pinned must not park, and a sync.Mutex parks, so a writer
 * holding a pin cannot use one. The critical sections are a slot store, a walk
 * of an overflow chain, and at most sixteen small allocations in the rare case
 * where two keys share a hash prefix, so a lock that spins is the right shape
 * for them anyway.
 *
 * Derived from Go's src/internal/sync/hashtriemap.go and src/sync/map.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/reclaim.h"
#include "burrow/runtime.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const Type map_desc = {
    {(const Byte *)"Map", 3},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncMap),
    (uint16_t)_Alignof(SyncMap),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736d6170U, /* "smap", distinct from every builtin's */
    NULL,
};

const Type *const TYPE_SYNC_MAP = &map_desc;

/* Sixteen children, four bits of hash per level, sixty four bits of hash, so
 * sixteen levels and no more. Go picked sixteen after measuring: eight costs
 * half the load performance and thirty two buys about one percent. */
#define NCHILD_LOG2 4
#define NCHILD 16
#define NCHILD_MASK (NCHILD - 1)
#define HASH_BITS 64

/* The header both node types start with, so that a child pointer can be
 * examined before anybody knows which kind it is. */
typedef struct Node {
    bool is_entry;
} Node;

typedef struct Indirect Indirect;
typedef struct MapEntry MapEntry;

/* An internal node. children are read without the lock and written under it. */
struct Indirect {
    Node node;

    /* The allocator this node came from, so that freeing it needs nothing but
     * the node. A retired node outlives the trie it was in and may outlive the
     * SyncMap struct itself, so it cannot reach back to either. */
    Alloc *a;

    Indirect *parent;
    burrow__Lock mu;
    uint32_t dead;
    burrow__Retired retired;
    void *children[NCHILD];
};

/* A leaf. The key and the value are copies, and they live immediately after
 * this header at offsets the map worked out once.
 *
 * Everything here is written before the node is published and never again, with
 * the single exception of overflow, which the parent node's lock covers. That
 * is what lets a reader copy a value out while somebody else is replacing the
 * entry: it is reading a node nobody will write to, and the node stays there
 * until the epoch says the reader has gone. */
struct MapEntry {
    Node node;
    void *overflow;
    burrow__Retired retired;

    /* Same reason as in Indirect, plus the size, since an entry is as big as
     * its key and value make it. */
    Alloc *a;
    uint32_t size;
    uint32_t align;
};

/* ------------------------------------------------------------------- layout */

static size_t syncmap_round_up(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

static void *ekey(const SyncMap *m, MapEntry *e) {
    return (Byte *)e + m->key_off;
}

static void *eval(const SyncMap *m, MapEntry *e) {
    return (Byte *)e + m->val_off;
}

/* type_copy with the size dispatched here rather than in the runtime.
 *
 * A lookup that hits ends in copying the value out, and type_copy is a call
 * across a library boundary that ends in a memcpy whose size the compiler
 * cannot see. For the eight bytes most values are, that was seven nanoseconds
 * out of a forty seven nanosecond lookup, which is a sixth of the row and the
 * single largest thing in it after the reclamation pin. Naming the sizes that
 * actually turn up lets the compiler emit a load and a store instead.
 *
 * The rule is type.c's and has to stay in step with it: a type with a copy of
 * its own gets to use it, and everything else is its bytes. */
static void syncmap_copy_bytes(const Type *t, void *dst, const void *src) {
    const TypeOps *ops = t->ops;

    if (ops != NULL && ops->copy != NULL) {
        ops->copy(dst, src);
        return;
    }

    switch (t->size) {
    case 1:
        memcpy(dst, src, 1);
        return;
    case 2:
        memcpy(dst, src, 2);
        return;
    case 4:
        memcpy(dst, src, 4);
        return;
    case 8:
        memcpy(dst, src, 8);
        return;
    case 16:
        memcpy(dst, src, 16);
        return;
    default:
        memcpy(dst, src, t->size);
        return;
    }
}

/* --------------------------------------------------------------- allocation */

static MapEntry *entry_new(SyncMap *m, const void *key, const void *val) {
    MapEntry *e = (MapEntry *)mem_alloc(m->a, m->node_size, m->node_align);
    if (e == NULL)
        return NULL;

    e->node.is_entry = true;
    e->a = m->a;
    e->size = m->node_size;
    e->align = m->node_align;
    syncmap_copy_bytes(m->key, ekey(m, e), key);
    if (val != NULL)
        syncmap_copy_bytes(m->val, eval(m, e), val);
    else
        type_zero(m->val, eval(m, e));
    return e;
}

static Indirect *indirect_new(SyncMap *m, Indirect *parent) {
    Indirect *i = BURROW_NEW(m->a, Indirect);
    if (i == NULL)
        return NULL;

    i->node.is_entry = false;
    i->a = m->a;
    i->parent = parent;
    return i;
}

static void entry_free_one(void *p) {
    MapEntry *e = (MapEntry *)p;
    mem_free(e->a, e, e->size, e->align);
}

/* Frees a node and everything under it, which for an entry means its overflow
 * chain and for an indirect means the whole subtree.
 *
 * The loads are atomic even though this only ever runs on memory nobody can
 * reach any more, because the last writes to it were atomic and a race detector
 * has no way to know that the reachability argument holds. */
static void tree_free(void *p) {
    if (((Node *)p)->is_entry) {
        MapEntry *e = (MapEntry *)p;
        while (e != NULL) {
            MapEntry *next = (MapEntry *)burrow__atomic_load_ptr(&e->overflow);
            entry_free_one(e);
            e = next;
        }
        return;
    }

    Indirect *i = (Indirect *)p;
    for (int j = 0; j < NCHILD; j++) {
        void *c = burrow__atomic_load_ptr(&i->children[j]);
        if (c != NULL)
            tree_free(c);
    }
    mem_free(i->a, i, sizeof(Indirect), _Alignof(Indirect));
}

/* Hands a node over. Both of these must be called after the store that took the
 * node out of the trie and never before it, which is the one ordering rule the
 * reclamation cannot check. */
static void retire_entry(MapEntry *e) {
    burrow__retire(&e->retired, entry_free_one, e);
}

static void retire_tree(void *p) {
    burrow__Retired *r =
        ((Node *)p)->is_entry ? &((MapEntry *)p)->retired : &((Indirect *)p)->retired;
    burrow__retire(r, tree_free, p);
}

/* --------------------------------------------------------------------- init
 *
 * The first store builds the root and works out where the key and the value sit
 * inside a node. Reads do not go through this at all: a map with no root has
 * had nothing stored in it, so a load can answer without allocating, and that
 * keeps the one path that must not fail free of anything that can. */

static bool init_slow(SyncMap *m) {
    sync_mutex_lock(&m->init_mu);
    if (sync_atomic_uint32_load(&m->inited) != 0) {
        sync_mutex_unlock(&m->init_mu);
        return true;
    }

    if (!type_is_comparable(m->key))
        runtime_panic(BURROW_S("sync.Map: key type is not comparable"));

    size_t align = _Alignof(MapEntry);
    if (m->key->align > align)
        align = m->key->align;
    if (m->val->align > align)
        align = m->val->align;

    size_t koff = syncmap_round_up(sizeof(MapEntry), m->key->align);
    size_t voff = syncmap_round_up(koff + m->key->size, m->val->align);

    m->key_off = (uint32_t)koff;
    m->val_off = (uint32_t)voff;
    m->node_size = (uint32_t)(voff + m->val->size);
    m->node_align = (uint32_t)align;
    m->seed = runtime_rand64();

    Indirect *root = indirect_new(m, NULL);
    if (root != NULL) {
        burrow__atomic_store_ptr(&m->root, root);
        sync_atomic_uint32_store(&m->inited, 1);
    }

    sync_mutex_unlock(&m->init_mu);
    return root != NULL;
}

static bool map_init(SyncMap *m) {
    if (sync_atomic_uint32_load(&m->inited) != 0)
        return true;
    return init_slow(m);
}

static bool syncmap_started(SyncMap *m) {
    return sync_atomic_uint32_load(&m->inited) != 0;
}

static void need_comparable_values(const SyncMap *m) {
    if (!type_is_comparable(m->val))
        runtime_panic(BURROW_S("sync.Map: value type is not comparable"));
}

/* ---------------------------------------------------------- walking a chain */

static bool chain_match(const SyncMap *m, MapEntry *e, const void *key, const void *val,
                        bool check_val) {
    if (!type_equal(m->key, ekey(m, e), key))
        return false;
    return !check_val || type_equal(m->val, eval(m, e), val);
}

static MapEntry *chain_lookup(const SyncMap *m, MapEntry *e, const void *key,
                              const void *val, bool check_val) {
    while (e != NULL) {
        if (chain_match(m, e, key, val, check_val))
            return e;
        e = (MapEntry *)burrow__atomic_load_ptr(&e->overflow);
    }
    return NULL;
}

/* Replaces the entry for key with fresh, under the lock of the indirect node
 * this chain hangs off.
 *
 * Answers the new head of the chain, or NULL when the key was not in it, and
 * puts the node that came out in *dead. The caller stores the head into the
 * slot and only then retires the dead node, because a node that is handed over
 * while it is still reachable is a node that gets freed while somebody is
 * reading it. */
static MapEntry *chain_swap(const SyncMap *m, MapEntry *head, MapEntry *fresh,
                            const void *key, const void *val, bool check_val,
                            void *out_prev, MapEntry **dead) {
    if (chain_match(m, head, key, val, check_val)) {
        burrow__atomic_store_ptr(&fresh->overflow,
                                 burrow__atomic_load_ptr(&head->overflow));
        if (out_prev != NULL)
            syncmap_copy_bytes(m->val, out_prev, eval(m, head));
        *dead = head;
        return fresh;
    }

    void **link = &head->overflow;
    MapEntry *e = (MapEntry *)burrow__atomic_load_ptr(link);
    while (e != NULL) {
        if (chain_match(m, e, key, val, check_val)) {
            burrow__atomic_store_ptr(&fresh->overflow,
                                     burrow__atomic_load_ptr(&e->overflow));
            burrow__atomic_store_ptr(link, fresh);
            if (out_prev != NULL)
                syncmap_copy_bytes(m->val, out_prev, eval(m, e));
            *dead = e;
            return head;
        }
        link = &e->overflow;
        e = (MapEntry *)burrow__atomic_load_ptr(link);
    }
    return NULL;
}

/* Takes the entry for key out of the chain, under the same lock.
 *
 * Answers whether it was there. *head is what the slot should hold afterwards,
 * which is NULL when the chain is now empty, and *dead is the node that came
 * out. */
static bool chain_delete(const SyncMap *m, MapEntry *head, const void *key,
                         const void *val, bool check_val, void *out_val,
                         MapEntry **out_head, MapEntry **dead) {
    if (chain_match(m, head, key, val, check_val)) {
        if (out_val != NULL)
            syncmap_copy_bytes(m->val, out_val, eval(m, head));
        *out_head = (MapEntry *)burrow__atomic_load_ptr(&head->overflow);
        *dead = head;
        return true;
    }

    void **link = &head->overflow;
    MapEntry *e = (MapEntry *)burrow__atomic_load_ptr(link);
    while (e != NULL) {
        if (chain_match(m, e, key, val, check_val)) {
            burrow__atomic_store_ptr(link, burrow__atomic_load_ptr(&e->overflow));
            if (out_val != NULL)
                syncmap_copy_bytes(m->val, out_val, eval(m, e));
            *out_head = head;
            *dead = e;
            return true;
        }
        link = &e->overflow;
        e = (MapEntry *)burrow__atomic_load_ptr(link);
    }
    return false;
}

/* ----------------------------------------------------------- walking the trie
 *
 * Both of these come back holding the lock of the indirect node they stopped
 * at, and both re-read the slot under that lock before they believe what they
 * saw on the way down. That re-read is the whole concurrency argument for the
 * write side: the unlocked walk finds a candidate, the lock makes it true, and
 * anything that changed underneath sends the walk back to the root.
 *
 * A node that has been unlinked has its dead flag set under its parent's lock,
 * so a writer that arrives at one after it was removed sees the flag and starts
 * again rather than inserting into a subtree nobody can reach. */

typedef struct Spot {
    Indirect *i;
    uint32_t shift;
    void **slot;
    void *n;
} Spot;

/* Finds where key belongs. When probe is true and the key is already there,
 * this answers false without taking any lock and leaves the entry in *found,
 * which is the read mostly path through LoadOrStore. */
static bool seek_insert(SyncMap *m, const void *key, uint64_t hash, bool probe,
                        Spot *spot, MapEntry **found) {
    for (;;) {
        Indirect *i = (Indirect *)burrow__atomic_load_ptr(&m->root);
        uint32_t shift = HASH_BITS;
        void **slot = NULL;
        void *n = NULL;
        bool have = false;

        while (shift != 0) {
            shift -= NCHILD_LOG2;
            slot = &i->children[(hash >> shift) & NCHILD_MASK];
            n = burrow__atomic_load_ptr(slot);
            if (n == NULL) {
                have = true;
                break;
            }
            if (((Node *)n)->is_entry) {
                if (probe) {
                    MapEntry *e = chain_lookup(m, (MapEntry *)n, key, NULL, false);
                    if (e != NULL) {
                        *found = e;
                        return false;
                    }
                }
                have = true;
                break;
            }
            i = (Indirect *)n;
        }
        if (!have)
            runtime_throw(BURROW_S("sync.Map: ran out of hash bits"));

        burrow__lock(&i->mu);
        n = burrow__atomic_load_ptr(slot);
        if ((n == NULL || ((Node *)n)->is_entry) &&
            burrow__atomic_load_u32(&i->dead) == 0) {
            spot->i = i;
            spot->shift = shift;
            spot->slot = slot;
            spot->n = n;
            return true;
        }
        burrow__unlock(&i->mu);
    }
}

/* Finds the entry holding key, and the value too when check_val is set.
 *
 * Answers false with nothing locked when there is no such entry. Answers true
 * holding the lock, with spot->n possibly NULL, because the entry may have gone
 * between the walk and the lock and the caller has to see that. */
static bool seek_find(SyncMap *m, const void *key, uint64_t hash, const void *val,
                      bool check_val, Spot *spot) {
    for (;;) {
        Indirect *i = (Indirect *)burrow__atomic_load_ptr(&m->root);
        uint32_t shift = HASH_BITS;
        void **slot = NULL;
        void *n = NULL;
        bool found = false;

        while (shift != 0) {
            shift -= NCHILD_LOG2;
            slot = &i->children[(hash >> shift) & NCHILD_MASK];
            n = burrow__atomic_load_ptr(slot);
            if (n == NULL)
                return false;
            if (((Node *)n)->is_entry) {
                if (chain_lookup(m, (MapEntry *)n, key, val, check_val) == NULL)
                    return false;
                found = true;
                break;
            }
            i = (Indirect *)n;
        }
        if (!found)
            runtime_throw(BURROW_S("sync.Map: ran out of hash bits"));

        burrow__lock(&i->mu);
        n = burrow__atomic_load_ptr(slot);
        if (burrow__atomic_load_u32(&i->dead) == 0 &&
            (n == NULL || ((Node *)n)->is_entry)) {
            spot->i = i;
            spot->shift = shift;
            spot->slot = slot;
            spot->n = n;
            return true;
        }
        burrow__unlock(&i->mu);
    }
}

/* Two keys whose hashes agree down to this level. Builds the indirect nodes
 * needed to tell them apart and answers the subtree to store in the slot.
 *
 * Published last and as one pointer, so that a reader never sees a moment where
 * the entry that was already there is not in the trie.
 *
 * NULL when a node could not be allocated, with everything it did allocate
 * already given back and neither entry touched. */
static void *expand(SyncMap *m, MapEntry *old_e, MapEntry *new_e, uint64_t new_hash,
                    uint32_t shift, Indirect *parent) {
    uint64_t old_hash = type_hash(m->key, ekey(m, old_e), m->seed);
    if (old_hash == new_hash) {
        /* Not a prefix collision, a real one. The chain is what that is for. */
        burrow__atomic_store_ptr(&new_e->overflow, old_e);
        return new_e;
    }

    Indirect *top = indirect_new(m, parent);
    if (top == NULL)
        return NULL;

    Indirect *cur = top;
    for (;;) {
        if (shift == 0)
            runtime_throw(BURROW_S("sync.Map: ran out of hash bits while inserting"));
        shift -= NCHILD_LOG2;

        uint64_t oi = (old_hash >> shift) & NCHILD_MASK;
        uint64_t ni = (new_hash >> shift) & NCHILD_MASK;
        if (oi != ni) {
            burrow__atomic_store_ptr(&cur->children[oi], old_e);
            burrow__atomic_store_ptr(&cur->children[ni], new_e);
            break;
        }

        Indirect *next = indirect_new(m, cur);
        if (next == NULL) {
            tree_free(top);
            return NULL;
        }
        burrow__atomic_store_ptr(&cur->children[oi], next);
        cur = next;
    }
    return top;
}

static bool indirect_empty(Indirect *i) {
    for (int j = 0; j < NCHILD; j++) {
        if (burrow__atomic_load_ptr(&i->children[j]) != NULL)
            return false;
    }
    return true;
}

/* Walks up from a node that has just lost its last child, unlinking every empty
 * node on the way, and drops the lock it was given.
 *
 * The dead flag goes up before the unlink and under both locks, so a writer
 * that is already sitting on this node with its lock about to be taken finds
 * the flag set and starts over. Without it that writer would insert into a
 * subtree that is no longer attached to anything. */
static void prune(Indirect *i, uint64_t hash, uint32_t shift) {
    while (i->parent != NULL && indirect_empty(i)) {
        /* Going up means the slot this node sits in was picked with a wider
         * shift than the one it picks its own children with. The check is after
         * the add rather than before it because what has to be in range is the
         * value about to be shifted by, and a shift of 64 on a 64 bit word is
         * undefined behaviour rather than zero. The tree cannot get this deep
         * with a root whose parent is NULL, but nothing in the loop says so. */
        shift += NCHILD_LOG2;
        if (shift >= HASH_BITS)
            runtime_throw(BURROW_S("sync.Map: ran out of hash bits"));

        Indirect *parent = i->parent;
        burrow__lock(&parent->mu);
        burrow__atomic_store_u32(&i->dead, 1);
        burrow__atomic_store_ptr(&parent->children[(hash >> shift) & NCHILD_MASK],
                                 NULL);
        burrow__unlock(&i->mu);

        retire_tree(i);
        i = parent;
    }
    burrow__unlock(&i->mu);
}

/* ---------------------------------------------------------------- the reads */

bool sync_map_load(SyncMap *m, const void *key, void *out_val) {
    if (!syncmap_started(m))
        return false;

    uint64_t hash = type_hash(m->key, key, m->seed);
    bool found = false;

    burrow__pin();
    Indirect *i = (Indirect *)burrow__atomic_load_ptr(&m->root);
    uint32_t shift = HASH_BITS;
    while (shift != 0) {
        shift -= NCHILD_LOG2;
        void *n = burrow__atomic_load_ptr(&i->children[(hash >> shift) & NCHILD_MASK]);
        if (n == NULL)
            break;
        if (((Node *)n)->is_entry) {
            MapEntry *e = chain_lookup(m, (MapEntry *)n, key, NULL, false);
            if (e != NULL) {
                found = true;
                /* Copied here rather than after the unpin, because after the
                 * unpin the node may already be gone. */
                if (out_val != NULL)
                    syncmap_copy_bytes(m->val, out_val, eval(m, e));
            }
            break;
        }
        i = (Indirect *)n;
    }
    burrow__unpin();

    return found;
}

static bool iter(const SyncMap *m, Indirect *i, SyncMapRangeFunc f, void *arg) {
    for (int j = 0; j < NCHILD; j++) {
        void *n = burrow__atomic_load_ptr(&i->children[j]);
        if (n == NULL)
            continue;
        if (!((Node *)n)->is_entry) {
            if (!iter(m, (Indirect *)n, f, arg))
                return false;
            continue;
        }
        for (MapEntry *e = (MapEntry *)n; e != NULL;
             e = (MapEntry *)burrow__atomic_load_ptr(&e->overflow)) {
            if (!f(ekey(m, e), eval(m, e), arg))
                return false;
        }
    }
    return true;
}

void sync_map_range(SyncMap *m, SyncMapRangeFunc f, void *arg) {
    if (!syncmap_started(m))
        return;

    /* Pinned for the whole walk, callback included, which is what the header
     * warns about. The alternative is to copy every key and value out first,
     * and a snapshot is not what Go's Range is. */
    burrow__pin();
    iter(m, (Indirect *)burrow__atomic_load_ptr(&m->root), f, arg);
    burrow__unpin();
}

/* --------------------------------------------------------------- the writes */

bool sync_map_swap(SyncMap *m, const void *key, const void *val, void *out_prev,
                   bool *out_loaded) {
    if (!map_init(m))
        return false;

    uint64_t hash = type_hash(m->key, key, m->seed);
    Spot spot;
    bool ok = true;

    burrow__pin();
    (void)seek_insert(m, key, hash, false, &spot, NULL);

    MapEntry *fresh = entry_new(m, key, val);
    if (fresh == NULL) {
        ok = false;
        goto out;
    }

    if (spot.n != NULL) {
        MapEntry *dead = NULL;
        MapEntry *head =
            chain_swap(m, (MapEntry *)spot.n, fresh, key, NULL, false, out_prev, &dead);
        if (head != NULL) {
            burrow__atomic_store_ptr(spot.slot, head);
            retire_entry(dead);
            if (out_loaded != NULL)
                *out_loaded = true;
            goto out;
        }

        void *top = expand(m, (MapEntry *)spot.n, fresh, hash, spot.shift, spot.i);
        if (top == NULL) {
            entry_free_one(fresh);
            ok = false;
            goto out;
        }
        burrow__atomic_store_ptr(spot.slot, top);
    } else {
        burrow__atomic_store_ptr(spot.slot, fresh);
    }
    if (out_loaded != NULL)
        *out_loaded = false;

out:
    burrow__unlock(&spot.i->mu);
    burrow__unpin();
    return ok;
}

bool sync_map_store(SyncMap *m, const void *key, const void *val) {
    return sync_map_swap(m, key, val, NULL, NULL);
}

bool sync_map_load_or_store(SyncMap *m, const void *key, const void *val,
                            void *out_actual, bool *out_loaded) {
    if (!map_init(m))
        return false;

    uint64_t hash = type_hash(m->key, key, m->seed);
    Spot spot;
    MapEntry *found = NULL;
    bool ok = true;

    burrow__pin();
    if (!seek_insert(m, key, hash, true, &spot, &found)) {
        if (out_actual != NULL)
            syncmap_copy_bytes(m->val, out_actual, eval(m, found));
        if (out_loaded != NULL)
            *out_loaded = true;
        burrow__unpin();
        return true;
    }

    /* The walk saw no match, but it was not holding the lock at the time, so
     * the chain gets one more look now that nobody can be changing it. */
    if (spot.n != NULL) {
        MapEntry *e = chain_lookup(m, (MapEntry *)spot.n, key, NULL, false);
        if (e != NULL) {
            if (out_actual != NULL)
                syncmap_copy_bytes(m->val, out_actual, eval(m, e));
            if (out_loaded != NULL)
                *out_loaded = true;
            goto out;
        }
    }

    MapEntry *fresh = entry_new(m, key, val);
    if (fresh == NULL) {
        ok = false;
        goto out;
    }

    if (spot.n != NULL) {
        void *top = expand(m, (MapEntry *)spot.n, fresh, hash, spot.shift, spot.i);
        if (top == NULL) {
            entry_free_one(fresh);
            ok = false;
            goto out;
        }
        burrow__atomic_store_ptr(spot.slot, top);
    } else {
        burrow__atomic_store_ptr(spot.slot, fresh);
    }
    if (out_actual != NULL)
        syncmap_copy_bytes(m->val, out_actual, eval(m, fresh));
    if (out_loaded != NULL)
        *out_loaded = false;

out:
    burrow__unlock(&spot.i->mu);
    burrow__unpin();
    return ok;
}

bool sync_map_compare_and_swap(SyncMap *m, const void *key, const void *old,
                               const void *val, bool *out_swapped) {
    need_comparable_values(m);
    if (!map_init(m))
        return false;

    uint64_t hash = type_hash(m->key, key, m->seed);
    Spot spot;
    bool ok = true;
    bool swapped = false;

    burrow__pin();
    if (!seek_find(m, key, hash, old, true, &spot)) {
        burrow__unpin();
        if (out_swapped != NULL)
            *out_swapped = false;
        return true;
    }
    if (spot.n == NULL)
        goto out;

    MapEntry *fresh = entry_new(m, key, val);
    if (fresh == NULL) {
        ok = false;
        goto out;
    }

    MapEntry *dead = NULL;
    MapEntry *head =
        chain_swap(m, (MapEntry *)spot.n, fresh, key, old, true, NULL, &dead);
    if (head == NULL) {
        /* It was there during the walk and is not there now, which is a losing
         * compare and swap and not an error. */
        entry_free_one(fresh);
        goto out;
    }
    burrow__atomic_store_ptr(spot.slot, head);
    retire_entry(dead);
    swapped = true;

out:
    burrow__unlock(&spot.i->mu);
    burrow__unpin();
    if (out_swapped != NULL)
        *out_swapped = swapped;
    return ok;
}

/* ------------------------------------------------------------- the removals */

static bool remove_key(SyncMap *m, const void *key, const void *val, bool check_val,
                       void *out_val) {
    if (!syncmap_started(m))
        return false;

    uint64_t hash = type_hash(m->key, key, m->seed);
    Spot spot;

    burrow__pin();
    if (!seek_find(m, key, hash, val, check_val, &spot)) {
        burrow__unpin();
        return false;
    }
    if (spot.n == NULL) {
        burrow__unlock(&spot.i->mu);
        burrow__unpin();
        return false;
    }

    MapEntry *head = NULL;
    MapEntry *dead = NULL;
    if (!chain_delete(m, (MapEntry *)spot.n, key, val, check_val, out_val, &head,
                      &dead)) {
        burrow__unlock(&spot.i->mu);
        burrow__unpin();
        return false;
    }

    if (head != NULL) {
        /* Something is still in the chain, so the slot stays full and the
         * parent cannot have become empty. */
        burrow__atomic_store_ptr(spot.slot, head);
        retire_entry(dead);
        burrow__unlock(&spot.i->mu);
    } else {
        burrow__atomic_store_ptr(spot.slot, NULL);
        retire_entry(dead);
        prune(spot.i, hash, spot.shift);
    }
    burrow__unpin();
    return true;
}

bool sync_map_load_and_delete(SyncMap *m, const void *key, void *out_val) {
    return remove_key(m, key, NULL, false, out_val);
}

void sync_map_delete(SyncMap *m, const void *key) {
    (void)remove_key(m, key, NULL, false, NULL);
}

bool sync_map_compare_and_delete(SyncMap *m, const void *key, const void *old) {
    need_comparable_values(m);
    return remove_key(m, key, old, true, NULL);
}

/* ------------------------------------------------------------- whole map ops */

bool sync_map_clear(SyncMap *m) {
    if (!map_init(m))
        return false;

    Indirect *fresh = indirect_new(m, NULL);
    if (fresh == NULL)
        return false;

    /* Dropping the root is the whole of it. A writer that is inside the old
     * tree right now finishes its work there and the result goes nowhere, which
     * is what Go's Clear does too, and the pin it is holding is what keeps the
     * tree underneath it alive while it does. */
    void *old = burrow__atomic_swap_ptr(&m->root, fresh);
    if (old != NULL)
        retire_tree(old);
    return true;
}

void sync_map_free(SyncMap *m) {
    if (!syncmap_started(m))
        return;

    void *root = burrow__atomic_swap_ptr(&m->root, NULL);
    sync_atomic_uint32_store(&m->inited, 0);
    if (root != NULL)
        tree_free(root);

    /* Anything deleted earlier is on the reclamation list and is not this
     * function's to free, since a reader may still be inside it. A few flushes
     * clear it when nothing else in the program is pinned, which is the usual
     * case at the point somebody frees a map, and the rest is why the header
     * says the allocator has to outlive the map. */
    for (int i = 0; i < 4 && burrow__reclaim_pending() != 0; i++)
        burrow__reclaim_flush();
}
