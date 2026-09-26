/* Go's map, as a Swiss table.
 *
 * Not derived from Go's source. The data structure is the one Go moved to in
 * 1.24, which Go took from Abseil, and the parts that are observable from
 * outside are Go's: what len reports, what a missing key reads as, what
 * deleting during a range does, and the fact that the order is different every
 * time. The code is written for C and shares no lines with either.
 *
 * The layout, in one picture. A table is an array of groups. A group is eight
 * control bytes followed by eight slots, and a slot is a key followed by a
 * value:
 *
 *     group 0                             group 1
 *     +----------------+----+----+ ... +  +----------------+ ...
 *     | c0 c1 .. c7    | s0 | s1 |     |  | c0 c1 .. c7    |
 *     +----------------+----+----+ ... +  +----------------+ ...
 *
 * A control byte is 0x80 when its slot has never been used, 0xFE when its slot
 * held an entry that was deleted, and otherwise the low seven bits of the key's
 * hash with the top bit clear. So the eight control bytes are one 64 bit word,
 * and the question "which of these eight slots might hold my key" is a few
 * instructions on that word with no branches and no key comparisons. A lookup
 * that misses usually touches one cache line and compares nothing.
 *
 * The rest of the hash, everything above the low seven bits, picks the group to
 * start at. A probe that finds no match and no empty slot moves on by a
 * triangular step, 1 then 3 then 6 then 10, which is what guarantees every
 * group gets visited exactly once when the group count is a power of two.
 *
 * Where this deviates from Go. Go's table is split into a directory of fixed
 * size tables and grows by splitting one of them, so a single insert never has
 * to touch the whole map. This one is a single table that doubles and reinserts
 * everything, which is simpler, is what Abseil does, and costs a pause
 * proportional to the map on the inserts that grow. Amortised it is the same
 * work. The other difference falls out of that: growing moves every entry, so
 * an iterator cannot survive it, which map.h says out loud.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/map.h"

#include "burrow/platform.h"
#include "burrow/runtime.h"

#include <string.h>

/* Eight, and it has to be eight. It is the number of bytes in the word the
 * control bytes are read as, so a different group size means a different
 * matching routine rather than a different constant. */
#define SLOTS 8

#define CTRL_EMPTY 0x80U
#define CTRL_DELETED 0xFEU

/* One byte of the control word, and the top bit of each of them. */
#define LSB 0x0101010101010101U
#define MSB 0x8080808080808080U

struct Map {
    Alloc *a;
    const Type *key;
    const Type *val;

    /* ngroups groups of group_size bytes each. NULL until the first insert,
     * because make(map[K]V) is common in code that then puts nothing in it. */
    Byte *groups;
    Uint ngroups; /* a power of two, or zero when groups is NULL */

    Int used;         /* what len reports */
    Uint growth_left; /* inserts into never used slots before a rehash */
    uint64_t seed;    /* this map's hash seed, from the runtime's generator */
    uint64_t gen;     /* bumped whenever the table moves, which iterators read */

    /* Worked out once in map_make, because every operation needs them and
     * none of them wants to go through the Type to get there. */
    uint32_t key_size;
    uint32_t val_size;
    uint32_t val_off; /* where the value starts inside a slot */
    uint32_t slot_size;
    uint32_t slot_off; /* where slot zero starts inside a group */
    uint32_t group_size;
    uint32_t group_align;
};

/* --------------------------------------------------------------- the word */

/* The eight control bytes as one word, low byte first on every machine.
 *
 * Little endian gets the load it wants for free. Big endian has to build the
 * word by hand, which costs eight shifts, and the alternative of byte swapping
 * needs a builtin per compiler. The reason it is low byte first rather than
 * native is that everything below finds the first matching slot by counting
 * trailing zeroes, and that only means "the earliest slot" if slot zero is in
 * the low byte. Native order would make the matching code read correctly and
 * iterate groups backwards on s390x, which is the kind of bug that gets found
 * two years later by somebody else. */
static uint64_t ctrl_load(const Byte *ctrl) {
#if BURROW_LITTLE_ENDIAN
    uint64_t w;
    memcpy(&w, ctrl, SLOTS);
    return w;
#else
    uint64_t w = 0;
    for (unsigned i = 0; i < SLOTS; i++)
        w |= (uint64_t)ctrl[i] << (i * 8);
    return w;
#endif
}

/* Which slots hold this h2.
 *
 * The trick is the standard one and it is worth writing down. Exclusive or the
 * word with h2 repeated in all eight bytes, and a byte that matched is now
 * zero. Then (v - LSB) & ~v & MSB has the top bit of a byte set exactly when
 * that byte was zero, since subtracting one from zero borrows and flips the top
 * bit while leaving ~v with that bit set.
 *
 * It can also set the top bit of a byte that was 0x80 followed by a borrow from
 * its neighbour, which is why the full control bytes are kept below 0x80 and the
 * two special values are at or above it: h2 is seven bits, so a byte that
 * matches a real h2 is a byte that held an entry. */
static uint64_t match_h2(uint64_t ctrl, uint64_t h2) {
    uint64_t v = ctrl ^ (LSB * h2);
    return (v - LSB) & ~v & MSB;
}

/* Which slots have never been used. 0x80 has bit seven set and bit one clear,
 * 0xFE has both set, and a full byte has bit seven clear, so testing bit seven
 * against bit one shifted up by six separates the three. */
static uint64_t match_empty(uint64_t ctrl) {
    return ctrl & ~(ctrl << 6) & MSB;
}

/* Which slots can be written to, meaning empty or deleted. Same idea against
 * bit zero: 0x80 and 0xFE both have bit seven set and bit zero clear, and a
 * full byte has bit seven clear. */
static uint64_t match_empty_or_deleted(uint64_t ctrl) {
    return ctrl & ~(ctrl << 7) & MSB;
}

/* The earliest slot in a match word.
 *
 * The bits are at position 7, 15, 23 and so on, so the count of trailing zeroes
 * divided by eight is the slot. The loop is there for compilers with no
 * builtin, and it runs at most eight times on a word that is known to have a
 * bit set. MSVC has _BitScanForward64 and it is deliberately not used: it needs
 * <intrin.h>, it does not exist on 32 bit MSVC targets, and this is not the
 * compiler anybody is measuring. */
static unsigned first_slot(uint64_t bits) {
#if BURROW_CC_GCC || BURROW_CC_CLANG
    return (unsigned)(__builtin_ctzll(bits) >> 3);
#else
    unsigned i = 0;
    while ((bits & 0x80U) == 0) {
        bits >>= 8;
        i++;
    }
    return i;
#endif
}

/* ------------------------------------------------------------- the layout */

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static Byte *group_at(const Map *m, Uint i) {
    return m->groups + (size_t)i * m->group_size;
}

/* Where an entry is, as the two numbers the caller actually needs. A pointer to
 * the slot is not enough for deleting, which has to write the control byte, and
 * recovering the group from the slot means a division. */
typedef struct Loc {
    Byte *group;
    unsigned slot;
} Loc;

static Byte *loc_slot(const Map *m, Loc l) {
    return l.group + m->slot_off + (size_t)l.slot * m->slot_size;
}

static Uint capacity(const Map *m) {
    return m->ngroups * SLOTS;
}

/* Seven eighths, which is Go's load factor and Abseil's. It is high enough that
 * the table is mostly full and low enough that there is almost always an empty
 * slot in the first group a probe looks at. */
static Uint max_used(Uint cap) {
    return cap - cap / SLOTS;
}

/* ------------------------------------------------------------ the searches */

static bool table_find(Map *m, const void *key, uint64_t hash, Loc *out) {
    Uint mask = m->ngroups - 1;
    Uint offset = (Uint)((hash >> 7) & mask);
    Uint step = 0;
    uint64_t h2 = hash & 0x7f;

    for (;;) {
        Byte *g = group_at(m, offset);
        uint64_t ctrl = ctrl_load(g);

        for (uint64_t hits = match_h2(ctrl, h2); hits != 0; hits &= hits - 1) {
            unsigned s = first_slot(hits);
            Loc l = {g, s};
            if (type_equal(m->key, loc_slot(m, l), key)) {
                *out = l;
                return true;
            }
        }

        /* An empty slot in this group ends the search. Anything inserted with
         * this hash would have stopped here or earlier, so the key is not in
         * the table. A group of nothing but full and deleted slots proves
         * nothing and the probe continues. */
        if (match_empty(ctrl) != 0)
            return false;

        step++;
        offset = (offset + step) & mask;
    }
}

/* Puts a key and value in a slot that is empty or deleted, without looking for
 * the key first. Every caller has already established that the key is not
 * there, either by searching for it or by having just built the table.
 *
 * Cannot fail to find room. growth_left above zero means at least one never
 * used slot is left, and the triangular probe reaches every group. */
static void insert_new(Map *m, const void *key, const void *val, uint64_t hash) {
    Uint mask = m->ngroups - 1;
    Uint offset = (Uint)((hash >> 7) & mask);
    Uint step = 0;

    for (;;) {
        Byte *g = group_at(m, offset);
        uint64_t hits = match_empty_or_deleted(ctrl_load(g));

        if (hits != 0) {
            unsigned s = first_slot(hits);
            Loc l = {g, s};
            Byte *slot = loc_slot(m, l);

            /* Filling a deleted slot costs nothing from the growth budget,
             * because the budget was already spent when that slot was first
             * used and was never given back. */
            if (g[s] == CTRL_EMPTY)
                m->growth_left--;
            g[s] = (Byte)(hash & 0x7f);

            type_copy(m->key, slot, key);
            if (m->val_size > 0) {
                if (val != NULL)
                    type_copy(m->val, slot + m->val_off, val);
                else
                    type_zero(m->val, slot + m->val_off);
            }
            return;
        }

        step++;
        offset = (offset + step) & mask;
    }
}

/* Builds a table of ngroups groups and moves everything into it.
 *
 * The same ngroups it already has is a valid ask and is what clears the
 * tombstones out without using more memory. The old table is freed at the end,
 * so the peak is both tables at once, which is the price of not having Go's
 * directory.
 *
 * Returns false with the map untouched if the allocation fails. That is the
 * whole reason map_set returns a bool. */
static bool map_rehash(Map *m, Uint ngroups) {
    Byte *old = m->groups;
    Uint old_n = m->ngroups;
    Byte *fresh = mem_alloc_array(m->a, ngroups, m->group_size, m->group_align);
    if (fresh == NULL)
        return false;

    /* Zeroed memory is not an empty table, since empty is 0x80, so the control
     * bytes get written here. The slots are left alone. They are zero on the
     * way out of the allocator and nothing reads a slot whose control byte
     * does not say it is full. */
    m->groups = fresh;
    m->ngroups = ngroups;
    for (Uint i = 0; i < ngroups; i++)
        memset(group_at(m, i), (int)CTRL_EMPTY, SLOTS);

    m->growth_left = max_used(capacity(m));
    m->gen++;

    if (old != NULL) {
        for (Uint i = 0; i < old_n; i++) {
            Byte *g = old + (size_t)i * m->group_size;
            for (unsigned s = 0; s < SLOTS; s++) {
                Byte *slot;
                if (g[s] == CTRL_EMPTY || g[s] == CTRL_DELETED)
                    continue;
                slot = g + m->slot_off + (size_t)s * m->slot_size;
                insert_new(m, slot, slot + m->val_off,
                           type_hash(m->key, slot, m->seed));
            }
        }
        mem_free(m->a, old, (size_t)old_n * m->group_size, m->group_align);
    }
    return true;
}

/* -------------------------------------------------------------- the making */

/* Rounds up to a power of two, because the group count has to be one for the
 * mask and the probe sequence to work. */
static Uint map_round_up_pow2(Uint n) {
    Uint p = 1;
    while (p < n)
        p *= 2;
    return p;
}

/* How many groups a hint of this many entries needs, so that filling the map to
 * the hint does not rehash. Seven eighths of the capacity has to be at least
 * the hint, so the capacity has to be at least hint plus a seventh of it. */
static Uint groups_for_hint(Int hint) {
    Uint need;
    if (hint <= 0)
        return 0;
    need = (Uint)hint + (Uint)hint / (SLOTS - 1) + 1;
    return map_round_up_pow2((need + SLOTS - 1) / SLOTS);
}

Map *map_make(Alloc *a, const Type *key, const Type *val, Int hint) {
    Map *m;
    size_t kalign, valign, salign;

    if (key == NULL || val == NULL)
        runtime_throw(BURROW_S("runtime error: makemap: nil type"));

    /* Go rejects this at compile time and says "invalid map key type". A
     * descriptor only exists at runtime, so this is the earliest the same
     * mistake can be caught, and catching it here beats hashing the bytes of a
     * slice header and getting a map where two equal slices are two keys. */
    if (!type_is_comparable(key))
        runtime_throw(BURROW_S("runtime error: makemap: invalid map key type"));

    /* An absurd hint is a bug in the caller's arithmetic rather than a request
     * worth attempting. The bound is deliberately far below what the
     * multiplication in mem_alloc_array would catch, so the message says what
     * is wrong instead of the allocation quietly failing. */
    if (hint < 0 || (uint64_t)hint > (uint64_t)1 << 40)
        runtime_panic(BURROW_S("runtime error: makemap: size out of range"));

    m = BURROW_NEW(a, Map);
    if (m == NULL)
        return NULL;

    m->a = a;
    m->key = key;
    m->val = val;

    kalign = key->align > 0 ? key->align : 1;
    valign = val->align > 0 ? val->align : 1;
    salign = kalign > valign ? kalign : valign;

    m->key_size = key->size;
    m->val_size = val->size;
    m->val_off = (uint32_t)align_up(key->size, valign);
    m->slot_size = (uint32_t)align_up(m->val_off + val->size, salign);

    /* A map of two zero sized types would have zero sized slots, and eight
     * slots at the same address is a headache for no gain. One byte each keeps
     * every slot a distinct object. Such a map still holds exactly one entry,
     * since every key compares equal to every other, which is what Go does
     * with map[struct{}]struct{} too. */
    if (m->slot_size == 0)
        m->slot_size = 1;

    m->slot_off = (uint32_t)align_up(SLOTS, salign);
    m->group_size = m->slot_off + m->slot_size * SLOTS;

    /* At least eight, so the control bytes of every group are on an eight byte
     * boundary and the load in ctrl_load is a single aligned read. group_size
     * is a multiple of this for the same reason. */
    m->group_align = (uint32_t)(salign > SLOTS ? salign : SLOTS);

    /* One seed per map, so that two maps with the same keys have different
     * layouts, and so that a program cannot be fed keys that all collide. This
     * is the reason the runtime needs a random source at all. */
    m->seed = runtime_rand64();

    m->groups = NULL;
    m->ngroups = 0;
    m->used = 0;
    m->growth_left = 0;
    m->gen = 0;

    if (hint > 0) {
        Uint want = groups_for_hint(hint);
        if (!map_rehash(m, want)) {
            mem_free(a, m, sizeof(Map), _Alignof(Map));
            return NULL;
        }
    }
    return m;
}

void map_free(Map *m) {
    if (m == NULL)
        return;
    if (m->groups != NULL)
        mem_free(m->a, m->groups, (size_t)m->ngroups * m->group_size, m->group_align);
    mem_free(m->a, m, sizeof(Map), _Alignof(Map));
}

/* ------------------------------------------------------------ the questions */

/* The table is copied as it stands, tombstones and all, rather than rebuilt
 * by inserting each entry. The seed comes along with it, which is what makes
 * that valid: every entry hashes to the same group in the copy as in the
 * original. Go's runtime clones a map the same way. */
Map *map_clone(Alloc *a, const Map *m) {
    Map *c;
    if (m == NULL)
        return NULL;
    c = BURROW_NEW(a, Map);
    if (c == NULL)
        return NULL;
    *c = *m;
    c->a = a;
    c->gen = 0;
    if (m->groups == NULL)
        return c;

    c->groups = mem_alloc_array(a, m->ngroups, m->group_size, m->group_align);
    if (c->groups == NULL) {
        mem_free(a, c, sizeof(Map), _Alignof(Map));
        return NULL;
    }
    if (m->key->ops == NULL && m->val->ops == NULL) {
        memcpy(c->groups, m->groups, (size_t)m->ngroups * m->group_size);
        return c;
    }
    for (Uint i = 0; i < m->ngroups; i++) {
        const Byte *g = group_at(m, i);
        Byte *d = group_at(c, i);
        memcpy(d, g, SLOTS);
        for (unsigned s = 0; s < SLOTS; s++) {
            const Byte *from;
            Byte *to;
            if (g[s] == CTRL_EMPTY || g[s] == CTRL_DELETED)
                continue;
            from = g + m->slot_off + (size_t)s * m->slot_size;
            to = d + m->slot_off + (size_t)s * m->slot_size;
            type_copy(m->key, to, from);
            type_copy(m->val, to + m->val_off, from + m->val_off);
        }
    }
    return c;
}

Int map_len(const Map *m) {
    return m == NULL ? 0 : m->used;
}

const Type *map_key_type(const Map *m) {
    return m == NULL ? NULL : m->key;
}

const Type *map_val_type(const Map *m) {
    return m == NULL ? NULL : m->val;
}

void *map_get(Map *m, const void *key) {
    Loc l;
    if (m == NULL || m->groups == NULL || m->used == 0)
        return NULL;
    if (!table_find(m, key, type_hash(m->key, key, m->seed), &l))
        return NULL;
    /* A zero sized value still gets a real address, because the caller reads
     * NULL as absent and there is nothing else to point at. */
    return loc_slot(m, l) + m->val_off;
}

bool map_get2(Map *m, const void *key, void *out_val) {
    void *v = map_get(m, key);
    if (v == NULL) {
        if (out_val != NULL && m != NULL)
            type_zero(m->val, out_val);
        return false;
    }
    if (out_val != NULL)
        type_copy(m->val, out_val, v);
    return true;
}

/* ------------------------------------------------------------- the changes */

bool map_set(Map *m, const void *key, const void *val) {
    uint64_t hash;
    Loc l;

    if (m == NULL)
        runtime_panic(BURROW_S("assignment to entry in nil map"));

    if (m->groups == NULL && !map_rehash(m, 1))
        return false;

    hash = type_hash(m->key, key, m->seed);

    /* An existing key keeps its stored key and takes the new value, which is
     * what Go does: m[k] = v on a map that already has an equal key does not
     * replace the key. It is observable for a key type whose equality ignores
     * something its bytes hold, and a float key is exactly that, since a
     * negative zero stored first stays negative. */
    if (table_find(m, key, hash, &l)) {
        Byte *slot = loc_slot(m, l);
        if (m->val_size > 0) {
            if (val != NULL)
                type_copy(m->val, slot + m->val_off, val);
            else
                type_zero(m->val, slot + m->val_off);
        }
        return true;
    }

    if (m->growth_left == 0) {
        /* Out of never used slots, which happens for two different reasons. If
         * the live entries alone would fit, the space is being held by
         * tombstones and rebuilding at the same size gets it back. Otherwise
         * the map is genuinely full and doubles. Growing when the real problem
         * was deletions is how a map that is repeatedly filled and emptied ends
         * up enormous. */
        Uint want =
            ((Uint)m->used + 1 <= max_used(capacity(m))) ? m->ngroups : m->ngroups * 2;
        if (!map_rehash(m, want))
            return false;
    }

    insert_new(m, key, val, hash);
    m->used++;
    return true;
}

void map_del(Map *m, const void *key) {
    Loc l;
    Byte *slot;

    if (m == NULL || m->groups == NULL || m->used == 0)
        return;
    if (!table_find(m, key, type_hash(m->key, key, m->seed), &l))
        return;

    slot = loc_slot(m, l);
    type_zero(m->key, slot);
    if (m->val_size > 0)
        type_zero(m->val, slot + m->val_off);

    /* The slot can go back to never used only if some other slot in its group
     * is already never used. Otherwise a probe that ran through this group on
     * its way somewhere else would stop here and miss the key it was looking
     * for, so the slot has to stay in the way as a tombstone. That is the whole
     * cost of open addressing and the reason the rebuild above exists. */
    if (match_empty(ctrl_load(l.group)) != 0) {
        l.group[l.slot] = (Byte)CTRL_EMPTY;
        m->growth_left++;
    } else {
        l.group[l.slot] = (Byte)CTRL_DELETED;
    }
    m->used--;
}

void map_clear(Map *m) {
    if (m == NULL || m->groups == NULL)
        return;

    /* The keys and values are zeroed rather than merely orphaned, so that a map
     * of strings does not keep the strings reachable through memory the caller
     * cannot see. This does not free anything today and it means a tool looking
     * for leaks points at the right place. */
    memset(m->groups, 0, (size_t)m->ngroups * m->group_size);
    for (Uint i = 0; i < m->ngroups; i++)
        memset(group_at(m, i), (int)CTRL_EMPTY, SLOTS);

    m->used = 0;
    m->growth_left = max_used(capacity(m));

    /* gen is not touched. The table did not move, so an iterator in the middle
     * of a range over this map stays valid and simply finds nothing more, which
     * is what Go's clear does to a range in progress. */
}

/* ----------------------------------------------------------- the iteration */

MapIter map_iter(Map *m) {
    MapIter it;

    /* An empty map iterates as nothing at all, and dropping the map here rather
     * than checking it every step means an insert after this call cannot turn
     * an empty range into a fatal error. */
    if (m == NULL || m->groups == NULL || m->used == 0) {
        it.m = NULL;
        it.gen = 0;
        it.group_off = 0;
        it.slot_off = 0;
        it.visited = 0;
        return it;
    }

    it.m = m;
    it.gen = m->gen;
    it.visited = 0;

    /* A random group and a random slot to start from, which is Go's
     * randomisation and is a feature. Anybody who writes code that depends on
     * map order finds out on their own machine on the first run instead of in
     * production after somebody adds a key. */
    {
        uint64_t r = runtime_rand64();
        it.group_off = (Uint)(r & (uint64_t)(m->ngroups - 1));
        it.slot_off = (Uint)((r >> 32) & (SLOTS - 1));
    }
    return it;
}

bool map_next(MapIter *it, const void **key, void **val) {
    Map *m;
    Uint cap;

    if (it == NULL || it->m == NULL)
        return false;
    m = it->m;

    /* The table moved, which means every pointer this iterator would hand out
     * is into memory that has been freed. map.h says this stops the program and
     * this is where it does. */
    if (it->gen != m->gen)
        runtime_throw(BURROW_S("map grew during iteration"));

    cap = capacity(m);
    while (it->visited < cap) {
        Uint n = it->visited++;

        /* n counts slots in visiting order. The group walks forward from the
         * random start and the slot within it rotates, so every slot in the
         * table is produced exactly once. */
        Uint gi = (it->group_off + (n / SLOTS)) & (m->ngroups - 1);
        unsigned s = (unsigned)((it->slot_off + n) & (SLOTS - 1));
        Byte *g = group_at(m, gi);

        if (g[s] == CTRL_EMPTY || g[s] == CTRL_DELETED)
            continue;

        {
            Loc l = {g, s};
            Byte *slot = loc_slot(m, l);
            if (key != NULL)
                *key = slot;
            if (val != NULL)
                *val = slot + m->val_off;
        }
        return true;
    }

    /* Done, and the iterator is left in a state where calling again says done
     * again rather than starting over. */
    it->m = NULL;
    return false;
}
