/* maps, the functions Go has for maps of any type.
 *
 * Go's maps. Go's versions are generic over the key and value types, and
 * these take a Map and read both types from it, so one function serves every
 * map:
 *
 *     Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
 *     BURROW_MAP_SET(Str, Int, m, BURROW_S("one"), 1);
 *     Map *c = maps_clone(a, m);
 *     bool same = maps_equal(m, c);
 *
 * A key or a value goes to a callback by pointer, the same way map_next hands
 * them out. A NULL map reads as empty everywhere, the way a nil map does in
 * Go, and writing to one panics.
 *
 * The functions that write to a map return false when the map needed to grow
 * and its allocator said no, which is the same answer map_set gives. Go's
 * versions cannot fail, because Go stops the program instead.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package maps */

#ifndef BURROW_MAPS_H
#define BURROW_MAPS_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Go's func(V1, V2) bool: whether two values are equal. a points at a value
 * in the first map and b at the value under the same key in the second. */
BURROW_FUNC(MapsEqualFunc, bool, const void *a, const void *b);

/* Go's func(K, V) bool, a test on one entry. */
BURROW_FUNC(MapsPredFunc, bool, const void *k, const void *v);

/* Whether the two maps hold the same keys with equal values, compared with
 * type_equal. A NULL map and an empty one are equal. A value that is a NaN is
 * not equal to itself, so a map holding one is not equal to itself either. */
bool maps_equal(Map *m1, Map *m2);

/* maps_equal with eq deciding whether each pair of values is equal. The keys
 * still have to match exactly, and the two value types may differ. */
bool maps_equal_func(Map *m1, Map *m2, MapsEqualFunc eq);

/* A copy of m made with a. It is map_clone, under the name Go gives it: the
 * keys and values are copied as values, and a NULL map gives NULL. */
BURROW_OWNS(ret) Map *maps_clone(Alloc *a, const Map *m);

/* Sets every key in src to its value in dst, replacing any value dst already
 * had under that key. dst and src have to have the same key and value types.
 * Panics if dst is NULL and src is not empty, as Go does. */
bool maps_copy(Map *dst, Map *src);

/* Removes every entry for which del answers true. */
void maps_delete_func(Map *m, MapsPredFunc del);

/* The entries of m as a sequence of key and value pointers, in the map's
 * random order. The pointers are into the table, as map_next's are. The
 * sequence holds m and allocates nothing, so it is good for as long as m is. */
IterSeq2 maps_all(Map *m);

/* Just the keys of m, and just the values, as maps_all orders them. */
IterSeq maps_keys(Map *m);
IterSeq maps_values(Map *m);

/* Sets every pair seq yields in m, so a later pair replaces an earlier one
 * with the same key. Stops early and answers false if the map cannot grow. */
bool maps_insert(Map *m, IterSeq2 seq);

/* A new map, made with a and the two types, holding every pair seq yields.
 * NULL if the allocator says no. */
BURROW_OWNS(ret) Map *maps_collect(Alloc *a, const Type *key, const Type *val,
                                   IterSeq2 seq);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MAPS_H */
