/* slices, the functions Go has for slices of any type.
 *
 * Go's slices. Go's versions are generic over the element type, and these
 * take a Slice and read the element type from its descriptor, so one function
 * serves every element type:
 *
 *     Int xs[] = {3, 1, 2};
 *     Slice s = slice_from(xs, 3, 3, TYPE_INT);
 *     slices_sort(s);
 *     Int want = 2;
 *     Int i = slices_index(s, &want);
 *
 * A value goes in and comes out by pointer, since C cannot pass a value whose
 * type is only known at runtime. slices_index takes a pointer to the value to
 * look for, and slices_min answers a pointer to the smallest element.
 *
 * The functions Go constrains to cmp.Ordered, like slices_sort and
 * slices_compare, work when the element's kind is an integer, a float or a
 * string, and a named type over one of those counts. Any other element type
 * panics, where Go would not have compiled. The ones Go constrains to
 * comparable use type_equal, so a Str compares by its bytes and a NaN is not
 * equal to itself. The _func variants take a function over pointers to
 * elements and work for any element type.
 *
 * A function that can grow a slice takes an allocator first, as slice_append
 * does, and follows its rules: the result may share the array s had, or may
 * be a new one.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package slices */

#ifndef BURROW_SLICES_H
#define BURROW_SLICES_H

#include "burrow/cmp.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Go's func(a, b E) int: negative when a goes before b, positive when after,
 * and zero when they are equal. a and b point at elements. BinarySearchFunc's
 * func(E, T) int has the same shape, with b pointing at the target. */
BURROW_FUNC(SlicesCmpFunc, int, const void *a, const void *b);

/* Go's func(E1, E2) bool: whether the two elements are equal. */
BURROW_FUNC(SlicesEqualFunc, bool, const void *a, const void *b);

/* Go's func(E) bool, a test on one element. */
BURROW_FUNC(SlicesPredFunc, bool, const void *v);

/* Whether s1 and s2 have the same length and equal elements. A nil slice and
 * an empty one are equal. */
bool slices_equal(Slice s1, Slice s2);

/* slices_equal with eq deciding whether each pair is equal. The two element
 * types may differ. */
bool slices_equal_func(Slice s1, Slice s2, SlicesEqualFunc eq);

/* Compares the elements pair by pair with cmp.Compare and answers the first
 * result that is not zero. If one slice runs out first it is the smaller, so
 * the answer is 0 only when the two are equal. */
int slices_compare(Slice s1, Slice s2);

/* slices_compare with cmp comparing each pair. */
int slices_compare_func(Slice s1, Slice s2, SlicesCmpFunc cmp);

/* The index of the first element equal to *v, or -1. */
Int slices_index(Slice s, const void *v);

/* The index of the first element f is true for, or -1. */
Int slices_index_func(Slice s, SlicesPredFunc f);

/* Whether *v is in s, and whether f is true for any element. */
bool slices_contains(Slice s, const void *v);
bool slices_contains_func(Slice s, SlicesPredFunc f);

/* Inserts the m values at v into s at index i and answers the result, in
 * place when s has the room. The elements from i on move up. Panics if i is
 * out of range, 0 to len(s) inclusive. v may point into s. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Slice slices_insert(Alloc *a, Slice s, Int i,
                                                            const void *v, Int m);

/* Removes s[i:j] and answers the shorter slice, in place, zeroing the
 * elements that are left over at the end. Panics if s[i:j] is not a valid
 * slice of s. */
BURROW_BORROWS(ret, s) Slice slices_delete(Slice s, Int i, Int j);

/* Removes every element del is true for, in place, and zeroes the elements
 * left over at the end. */
BURROW_BORROWS(ret, s) Slice slices_delete_func(Slice s, SlicesPredFunc del);

/* Replaces s[i:j] with the m values at v and answers the result, in place
 * when s has the room. When m is less than j - i, the elements left over at
 * the end are zeroed. Panics if s[i:j] is not a valid slice of s. v may point
 * into s. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Slice slices_replace(Alloc *a, Slice s, Int i,
                                                             Int j, const void *v,
                                                             Int m);

/* A shallow copy of s in memory from a. A nil s gives a nil slice. */
BURROW_OWNS(ret) Slice slices_clone(Alloc *a, Slice s);

/* Replaces each run of equal elements with one copy, like the uniq command,
 * in place, and zeroes the elements left over at the end. */
BURROW_BORROWS(ret, s) Slice slices_compact(Slice s);

/* slices_compact with eq deciding equality. The first element of a run is
 * the one kept. */
BURROW_BORROWS(ret, s) Slice slices_compact_func(Slice s, SlicesEqualFunc eq);

/* s with room for at least n more elements, so that appending n does not
 * allocate. Panics if n is negative. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Slice slices_grow(Alloc *a, Slice s, Int n);

/* s[:len(s):len(s)], s without its spare capacity. */
BURROW_BORROWS(ret, s) Slice slices_clip(Slice s);

/* Reverses the elements of s in place. */
void slices_reverse(Slice s);

/* A new slice holding the n slices at slices one after the other, all with
 * the first one's element type. Nil if they are all empty. */
BURROW_OWNS(ret) Slice slices_concat(Alloc *a, const Slice *slices, Int n);

/* A new slice holding x count times over, with length and capacity
 * len(x) * count. Never nil. Panics if count is negative or the length does
 * not fit an Int. */
BURROW_OWNS(ret) Slice slices_repeat(Alloc *a, Slice x, Int count);

/* Sorts x in increasing order, a NaN before every other float. Not stable,
 * which for these types can only be seen with a NaN or a -0.0. */
void slices_sort(Slice x);

/* Sorts x as cmp says. Not stable. cmp has to be a strict weak ordering and
 * answer 0 for elements it cannot tell apart. */
void slices_sort_func(Slice x, SlicesCmpFunc cmp);

/* slices_sort_func, keeping equal elements in the order they were in. */
void slices_sort_stable_func(Slice x, SlicesCmpFunc cmp);

/* Whether x is in increasing order, and whether it is as cmp says. */
bool slices_is_sorted(Slice x);
bool slices_is_sorted_func(Slice x, SlicesCmpFunc cmp);

/* The smallest and the largest element. A NaN anywhere makes the answer a
 * NaN, and -0.0 is smaller than 0.0, as with Go's min and max. Panics if x is
 * empty. The answer points into x. */
BURROW_BORROWS(ret, x) const void *slices_min(Slice x);
BURROW_BORROWS(ret, x) const void *slices_max(Slice x);

/* The smallest and the largest element as cmp says, the first of them when
 * there are several. Panics if x is empty. */
BURROW_BORROWS(ret, x) const void *slices_min_func(Slice x, SlicesCmpFunc cmp);
BURROW_BORROWS(ret, x) const void *slices_max_func(Slice x, SlicesCmpFunc cmp);

/* Where *target is in the sorted slice x, or where it would go: the first
 * index whose element is not less than it. *found says whether the element
 * there equals it. Go returns both, and found may be NULL here. */
Int slices_binary_search(Slice x, const void *target, bool *found);

/* slices_binary_search with cmp(element, target) comparing. x has to be
 * sorted in the order cmp gives. */
Int slices_binary_search_func(Slice x, const void *target, SlicesCmpFunc cmp,
                              bool *found);

/* The index and a pointer to each element in order, and in reverse order. The
 * sequence reads *s when it runs, so s has to stay valid until it is done.
 * Nothing is allocated. */
BURROW_BORROWS(ret, s) IterSeq2 slices_all(const Slice *s);
BURROW_BORROWS(ret, s) IterSeq2 slices_backward(const Slice *s);

/* Each element in order, the same way. */
BURROW_BORROWS(ret, s) IterSeq slices_values(const Slice *s);

/* s with every value seq yields appended. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Slice slices_append_seq(Alloc *a, Slice s,
                                                                IterSeq seq);

/* A new slice of elem holding the values seq yields. Nil if there are none. */
BURROW_OWNS(ret) Slice slices_collect(Alloc *a, const Type *elem, IterSeq seq);

/* slices_collect, then sorted with slices_sort, slices_sort_func or
 * slices_sort_stable_func. */
BURROW_OWNS(ret) Slice slices_sorted(Alloc *a, const Type *elem, IterSeq seq);
BURROW_OWNS(ret) Slice slices_sorted_func(Alloc *a, const Type *elem, IterSeq seq,
                                          SlicesCmpFunc cmp);
BURROW_OWNS(ret) Slice slices_sorted_stable_func(Alloc *a, const Type *elem,
                                                 IterSeq seq, SlicesCmpFunc cmp);

/* The consecutive pieces of s, n elements each but the last, as Slice values
 * clipped to their length so that appending to one does not write into s. An
 * empty s gives nothing. Panics if n is less than 1. The state comes from a,
 * and slices_seq_free gives it back. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) IterSeq slices_chunk(Alloc *a, Slice s, Int n);

/* Frees what slices_chunk took from a. */
void slices_seq_free(Alloc *a, IterSeq seq);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SLICES_H */
