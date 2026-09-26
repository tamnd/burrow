/* sort, sorting slices and anything else with a length and two elements to
 * compare and swap.
 *
 * Go's sort. The algorithms are Go's own, pdqsort for sort_sort and the
 * insertion sort and SymMerge pair for sort_stable, with every comparison and
 * every swap made in the same order Go makes them. That matters more than it
 * sounds, because an unstable sort leaves equal elements in an order that is
 * not specified but is still observable, and a program's output ends up
 * depending on it.
 *
 * The quickest way in is a slice of a builtin:
 *
 *     Int xs[] = {5, 2, 6, 3, 1, 4};
 *     Slice s = slice_from(xs, 6, 6, TYPE_INT);
 *     sort_ints(s);
 *
 * Anything else is sorted through SortInterface, which is Go's sort.Interface:
 * a length, a less and a swap. sort_slice does the same for a slice with a
 * less function, and swaps the elements itself.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package sort */

#ifndef BURROW_SORT_H
#define BURROW_SORT_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sort.Interface. len is the number of elements, less reports whether the
 * element at i must sort before the one at j, and swap swaps them.
 *
 * less has to be a strict weak ordering. If less(i, j) and less(j, k) are both
 * true then so is less(i, k), and if both are false then so is less(i, k). A
 * plain < on floats is not one once NaNs are involved, and
 * sort_float64_slice_less shows the usual way around that. When neither
 * less(i, j) nor less(j, i) holds the two elements are equal, and sort_sort
 * may leave equal elements in any order where sort_stable keeps the order
 * they came in. */
typedef struct SortInterfaceVT {
    const Type *self_type;
    Int (*len)(void *self);
    bool (*less)(void *self, Int i, Int j);
    void (*swap)(void *self, Int i, Int j);
} SortInterfaceVT;

typedef struct SortInterface {
    const SortInterfaceVT *vt;
    void *data;
} SortInterface;

/* Sorts data in increasing order as its less says. Makes one call to len and
 * O(n*log(n)) calls to less and swap. Not stable. */
void sort_sort(SortInterface data);

/* Sorts data keeping equal elements in the order they were in. Makes one call
 * to len, O(n*log(n)) calls to less and O(n*log(n)*log(n)) calls to swap. */
void sort_stable(SortInterface data);

/* Whether data is sorted. */
bool sort_is_sorted(SortInterface data);

/* What sort_reverse returns: data with its less turned around, so it sorts in
 * decreasing order. It holds only data, and sort_reverse_as_sort_interface
 * makes the SortInterface that uses it. */
typedef struct SortReverse {
    SortInterface
        iface; /* not "interface", which the Windows headers define as a macro */
} SortReverse;

/* Go's Reverse. Go returns a pointer to a new value, and this returns the
 * value itself so nothing is allocated:
 *
 *     SortReverse r = sort_reverse(data);
 *     sort_sort(sort_reverse_as_sort_interface(&r));
 */
SortReverse sort_reverse(SortInterface data);
BURROW_BORROWS(ret, r) SortInterface sort_reverse_as_sort_interface(SortReverse *r);

/* Go's func(i, j int) bool for sort_slice: whether element i goes before j. */
BURROW_FUNC(SortLessFunc, bool, Int i, Int j);

/* Sorts x as less says, swapping whole elements of the size x's element type
 * gives. Not stable. Go's version takes any and panics when it is not a
 * slice, and this one takes the Slice. */
void sort_slice(Slice x, SortLessFunc less);

/* sort_slice, but keeping equal elements in the order they were in. */
void sort_slice_stable(Slice x, SortLessFunc less);

/* Whether x is sorted as less says. */
bool sort_slice_is_sorted(Slice x, SortLessFunc less);

/* Go's func(int) bool for sort_search. */
BURROW_FUNC(SortSearchFunc, bool, Int i);

/* The smallest index i in [0, n) at which f(i) is true, assuming that f is
 * false for some prefix of the range and true for the rest. n when f is true
 * nowhere. A binary search, so f is called O(log(n)) times, and only with
 * indexes in range. */
Int sort_search(Int n, SortSearchFunc f);

/* Go's func(int) int for sort_find. */
BURROW_FUNC(SortFindFunc, int, Int i);

/* The smallest index i in [0, n) at which cmp(i) <= 0, and n if there is none,
 * assuming cmp is positive for some prefix of the range, then zero, then
 * negative. found is set to whether cmp(i) is zero there, that is whether i
 * is an exact match. Go returns both, and found may be NULL here if only the
 * index is wanted. */
Int sort_find(Int n, SortFindFunc cmp, bool *found);

/* Sorts a slice of Int, of double or of Str in increasing order. A NaN goes
 * before every other float. These are Go's Ints, Float64s and Strings, and
 * like Go's they sort the values directly rather than through less and swap,
 * which is several times faster than going through SortInterface. */
void sort_ints(Slice x);
void sort_float64s(Slice x);
void sort_strings(Slice x);

/* Whether a slice of Int, of double or of Str is in increasing order. */
bool sort_ints_are_sorted(Slice x);
bool sort_float64s_are_sorted(Slice x);
bool sort_strings_are_sorted(Slice x);

/* Where x would go in a sorted slice of Int, of double or of Str: the index
 * of the first element that is not less than x, which is len(a) if there is
 * none. */
Int sort_search_ints(Slice a, Int x);
Int sort_search_float64s(Slice a, double x);
Int sort_search_strings(Slice a, Str x);

/* sort.IntSlice, sort.Float64Slice and sort.StringSlice: a slice of Int, of
 * double or of Str, with the methods that make it a SortInterface. */
typedef Slice SortIntSlice;
typedef Slice SortFloat64Slice;
typedef Slice SortStringSlice;

Int sort_int_slice_len(SortIntSlice x);
bool sort_int_slice_less(SortIntSlice x, Int i, Int j);
void sort_int_slice_swap(SortIntSlice x, Int i, Int j);
void sort_int_slice_sort(SortIntSlice x);
Int sort_int_slice_search(SortIntSlice p, Int x);
BURROW_BORROWS(ret, x) SortInterface sort_int_slice_as_sort_interface(SortIntSlice *x);

/* Less puts a NaN before every other value, which is what makes it a strict
 * weak ordering where a plain < is not. */
Int sort_float64_slice_len(SortFloat64Slice x);
bool sort_float64_slice_less(SortFloat64Slice x, Int i, Int j);
void sort_float64_slice_swap(SortFloat64Slice x, Int i, Int j);
void sort_float64_slice_sort(SortFloat64Slice x);
Int sort_float64_slice_search(SortFloat64Slice p, double x);
BURROW_BORROWS(ret, x) SortInterface
sort_float64_slice_as_sort_interface(SortFloat64Slice *x);

Int sort_string_slice_len(SortStringSlice x);
bool sort_string_slice_less(SortStringSlice x, Int i, Int j);
void sort_string_slice_swap(SortStringSlice x, Int i, Int j);
void sort_string_slice_sort(SortStringSlice x);
Int sort_string_slice_search(SortStringSlice p, Str x);
BURROW_BORROWS(ret, x) SortInterface
sort_string_slice_as_sort_interface(SortStringSlice *x);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SORT_H */
