/* Go's map, which is a hash table with three observable properties that most C
 * hash tables do not have.
 *
 * It grows by itself, so an insert can allocate. Its iteration order is
 * randomised, so nobody can depend on it. And its keys are values rather than
 * pointers, so a map keyed by string compares the bytes of the string and not
 * the address of them.
 *
 *     Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
 *
 *     Str word = BURROW_S("the");
 *     Int one = 1;
 *     map_set(counts, &word, &one);
 *
 *     Int *n = map_get(counts, &word);
 *     if (n != NULL)
 *         (*n)++;
 *
 * Keys and values go in and come out by pointer, because a map holds values of
 * a type it only learns at runtime. The macros at the bottom of this header put
 * the static typing back where the call site knows the types, which is almost
 * everywhere:
 *
 *     BURROW_MAP_SET(Str, Int, counts, BURROW_S("the"), 1);
 *     Int *n = BURROW_MAP_GET(Str, Int, counts, BURROW_S("the"));
 *
 * Underneath it is a Swiss table, the same data structure Go moved to in 1.24
 * and the one Abseil introduced: slots in groups of eight, one control byte per
 * slot holding seven bits of the hash, and eight of those bytes tested at once
 * with ordinary integer arithmetic. A lookup that misses usually reads one
 * cache line and compares no keys at all.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MAP_H
#define BURROW_MAP_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque, and heap allocated, exactly as Go's map is. A Go map variable is a
 * pointer to a header the runtime owns, which is why passing a map to a
 * function lets that function change it, and why a map is the one composite in
 * Go that is not copied on assignment. Map * here behaves the same way for the
 * same reason. */
typedef struct Map Map;

/* A map remembers the allocator it was made with.
 *
 * This is the one place the library bends the rule that every allocating
 * function takes an allocator first. An insert can grow the table, so map_set
 * can allocate, and the alternative is an allocator parameter on the hottest
 * operation the container has, where a caller who passes a different one on the
 * second call gets a table with half its memory from somewhere else. Storing it
 * once keeps the property the rule exists for, which is that everything a map
 * allocated came from one allocator that the caller chose.
 *
 * hint is make(map[K]V, hint): the number of entries you expect, used to size
 * the table so that filling it does not rehash. Zero is fine and means no
 * guess, and a map made that way allocates nothing at all until the first
 * insert.
 *
 * Returns NULL if the header cannot be allocated. Stops the program if the key
 * type is not comparable, which is Go's compile time error that C can only ask
 * about at runtime: a slice, a map and a function cannot be map keys, and nor
 * can a struct containing one. */
BURROW_OWNS(ret) Map *map_make(Alloc *a, const Type *key, const Type *val, Int hint);

/* A copy of m made with a, holding the same entries. A NULL map clones to
 * NULL, the way maps.Clone of a nil map is nil. The keys and values are
 * copied as values, so a map of Str shares the string bytes with the original
 * the same as a Go map of string does. Returns NULL as well if the allocator
 * says no. */
BURROW_OWNS(ret) Map *map_clone(Alloc *a, const Map *m);

/* len(m). A NULL map has no entries, the way a nil map in Go has none. */
Int map_len(const Map *m);

/* What the map was made with, for code that was handed a map and has to ask.
 * This is what fmt and encoding/json will use to print one. */
BURROW_STATIC(ret) const Type *map_key_type(const Map *m);
BURROW_STATIC(ret) const Type *map_val_type(const Map *m);

/* Not API. The allocator m was made with, for a package such as net/url whose
 * map type grows its values from the same place. */
BURROW_BORROWS(ret) Alloc *burrow__map_allocator(const Map *m);

/* m[key], as a pointer to the value in the table, or NULL when the key is not
 * there. Reading through a NULL map gives NULL, since a nil map in Go reads as
 * empty rather than panicking.
 *
 * The pointer is into the table, so writing through it changes the entry, which
 * is how m[k]++ is spelled here. It stops being valid at the next insert into
 * that map, because growing the table moves every entry. Go does not let you
 * hold this pointer at all, and that restriction is exactly this hazard, so
 * take the value out if you are going to keep it. */
BURROW_BORROWS(ret, m) void *map_get(Map *m, const void *key);

/* v, ok := m[key]. Copies the value out, which is the safe way to read one, and
 * writes the zero value when the key is absent so that the C code reads like
 * the Go code it came from. out_val may be NULL to ask only whether the key is
 * there. */
bool map_get2(Map *m, const void *key, void *out_val);

/* m[key] = val. Copies both, so the map does not point into the caller's
 * storage, which means a key built on the stack is a fine key.
 *
 * val may be NULL, which stores the value type's zero value. Go has no way to
 * write that and does not need one, since m[k] = V{} says it. Here it saves
 * naming a temporary, and for map[K]struct{} used as a set it is the only thing
 * you would ever pass.
 *
 * Returns false when the table needed to grow and the allocator said no. The
 * map is unchanged in that case. Go's assignment cannot fail because Go stops
 * the world instead, and a library in C has to hand the decision back.
 *
 * Panics when m is NULL, which is Go's "assignment to entry in nil map" and is
 * recoverable there and here. A nil map is readable and not writable in Go, and
 * quietly accepting a write here would lose data instead of reporting it. */
bool map_set(Map *m, const void *key, const void *val);

/* delete(m, key). Does nothing if the key is not there or the map is NULL. */
void map_del(Map *m, const void *key);

/* clear(m), which empties the map and keeps the memory, the same as Go's
 * builtin. A map you are about to refill should be cleared rather than
 * remade. */
void map_clear(Map *m);

/* Hands the map's memory back to the allocator it came from, and leaves the
 * Map * dangling, so it is the last thing you do with one.
 *
 * This is the first per object free function in the library and there is a
 * reason it is here when Str and Slice do not have one. Those two hand you the
 * pointer and the size, so mem_free takes them directly and the allocator rule
 * covers it. A map is opaque and it reallocates behind your back, so nothing
 * outside this file can name the pointer or the size to give back. Arena users
 * can keep ignoring it, since arena_free already covers everything, and it is a
 * no-op in the sense that the arena will not reuse the memory either way. Heap
 * users need it.
 *
 * NULL is fine and does nothing. */
void map_free(Map *m);

/* The iterator, which is a value you keep on the stack.
 *
 * The fields are here because C has no other way to let you declare one, and
 * not because they are yours to read. Nothing outside map.c looks at them and
 * the set of them will change. */
typedef struct MapIter {
    Map *m;
    uint64_t gen;
    Uint group_off;
    Uint slot_off;
    Uint visited;
} MapIter;

/* for k, v := range m.
 *
 *     const void *k;
 *     void *v;
 *     for (MapIter it = map_iter(m); map_next(&it, &k, &v); ) {
 *         printf(BURROW_STR_FMT " = %lld\n", BURROW_STR_ARG(*(const Str *)k),
 *                (long long)*(Int *)v);
 *     }
 *
 * Either pointer may be NULL if you only want the other one.
 *
 * The order is randomised per iterator, the same as Go's, and it is randomised
 * on purpose. A program that depends on map order is already broken and the
 * only kind thing to do is break it immediately rather than on the day
 * somebody adds a key.
 *
 * Deleting during the walk is allowed and is the common case:
 * an entry you delete before reaching it will not be produced. Inserting during
 * the walk is allowed too, and a new entry may or may not be produced, which is
 * the same thing Go's specification says.
 *
 * With one exception. If an insert makes the table grow while an iterator is
 * live, every entry moves, and this implementation stops the program rather
 * than producing an arbitrary answer. Go keeps the old table alive for the
 * iterator and can afford to because it has a collector. Doing that here means
 * either holding freed memory or never freeing it, and the pattern is already
 * unspecified in Go. Insert into a map you are walking and you may get a fatal
 * error saying so, which beats the quiet version of the same bug. */
MapIter map_iter(Map *m);
bool map_next(MapIter *it, const void **key, void **val);

/* Typed access, for the call sites that know the types, which is most of them.
 * Same shape as BURROW_AT and BURROW_APPEND: the types first, then the
 * container, then the values.
 *
 * The key and the value are put into one element arrays so that a literal can
 * be passed by pointer without the caller naming a temporary. */
#define BURROW_MAP_SET(KT, VT, m, k, v)                                                \
    map_set((m), (const KT[]){(k)}, (const VT[]){(v)})

#define BURROW_MAP_GET(KT, VT, m, k) ((VT *)map_get((m), (const KT[]){(k)}))

#define BURROW_MAP_HAS(KT, m, k) map_get2((m), (const KT[]){(k)}, NULL)

#define BURROW_MAP_DEL(KT, m, k) map_del((m), (const KT[]){(k)})

#if defined(BURROW_SHORT) && BURROW_SHORT
#define MAP_SET(KT, VT, m, k, v) BURROW_MAP_SET(KT, VT, m, k, v)
#define MAP_GET(KT, VT, m, k) BURROW_MAP_GET(KT, VT, m, k)
#define MAP_HAS(KT, m, k) BURROW_MAP_HAS(KT, m, k)
#define MAP_DEL(KT, m, k) BURROW_MAP_DEL(KT, m, k)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MAP_H */
