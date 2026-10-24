/* index/suffixarray, substring search in logarithmic time.
 *
 * Package suffixarray implements substring search in logarithmic time using
 * an in-memory suffix array.
 *
 * Example use:
 *
 *     // create index for some data
 *     SuffixarrayIndex *index = suffixarray_new(a, data);
 *
 *     // lookup byte slice s
 *     // the list of all indices where s occurs in data
 *     Slice offsets1 = suffixarray_index_lookup(index, a, s, -1);
 *     // the list of at most 3 indices where s occurs in data
 *     Slice offsets2 = suffixarray_index_lookup(index, a, s, 3);
 *
 * Go has a collector and C does not, so suffixarray_index_free is extra. The
 * index borrows the data it was made from, which has to outlive it, and the
 * results of a lookup come from the allocator passed to the lookup. When that
 * allocator runs out, a lookup gives the nil slice, the same as finding
 * nothing.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package index/suffixarray */

#ifndef BURROW_INDEX_SUFFIXARRAY_H
#define BURROW_INDEX_SUFFIXARRAY_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/regexp.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Go's ints: the suffix array as a Slice of int32_t or of int64_t. One of them
 * is nil and the other is the real data. The int64 form is used when the data
 * is longer than an int32 can count. */
typedef struct burrow__SuffixarrayInts {
    Slice int32;
    Slice int64;
} burrow__SuffixarrayInts;

/* Index implements a suffix array for fast substring search.
 *
 * The fields are the implementation's. A zeroed SuffixarrayIndex is an index
 * of no data, and suffixarray_index_read can read into one. */
typedef struct SuffixarrayIndex {
    Slice data;
    burrow__SuffixarrayInts sa;
    Alloc *alloc;
    Alloc *owner;
    bool owns_data;
} SuffixarrayIndex;

/* suffixarray.New: creates a new Index for data, from a. Index creation time
 * is O(N) for N = len(data). The index keeps data without copying it. NULL
 * when a runs out. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, data) SuffixarrayIndex *
suffixarray_new(Alloc *a, Slice data);

/* Gives back the suffix array, the data when suffixarray_index_read made it,
 * and the index itself when suffixarray_new did. NULL is fine. */
void suffixarray_index_free(SuffixarrayIndex *x);

/* Index.Read: reads the index from r into x; x must not be nil. Buffers
 * that are big enough are reused, and the data they hold is overwritten even
 * when the index borrowed it. New ones come from a, and the old ones x made go
 * back. */
BURROW_STATIC(ret) Error suffixarray_index_read(SuffixarrayIndex *x, Alloc *a,
                                                IoReader r);

/* Index.Write: writes the index x to w. */
BURROW_STATIC(ret) Error suffixarray_index_write(SuffixarrayIndex *x, IoWriter w);

/* Index.Bytes: returns the data over which the index was created. It must not
 * be modified. */
BURROW_BORROWS(ret, x) Slice suffixarray_index_bytes(SuffixarrayIndex *x);

/* Index.Lookup: returns an unsorted list of at most n indices where the byte
 * string s occurs in the indexed data, as a Slice of Int from a. If n < 0, all
 * occurrences are returned. The result is nil if s is empty, s is not found,
 * or n == 0. Lookup time is O(log(N)*len(s) + len(result)) where N is the size
 * of the indexed data. */
BURROW_OWNS(ret) Slice suffixarray_index_lookup(SuffixarrayIndex *x, Alloc *a, Slice s,
                                                Int n);

/* Index.FindAllIndex: returns a sorted list of non-overlapping matches of the
 * regular expression r, where a match is a pair of indices specifying the
 * matched slice of x.Bytes(). If n < 0, all matches are returned in successive
 * order. Otherwise, at most n matches are returned and they may not be
 * successive. The result is nil if there are no matches, or if n == 0.
 *
 * The result is a Slice of Slices of Int, the way regexp_find_all_index gives
 * it. As in Go, the search can go round more than once, and what an earlier
 * round made is not in the result, so a is best an arena. */
BURROW_OWNS(ret) Slice suffixarray_index_find_all_index(SuffixarrayIndex *x, Alloc *a,
                                                        const Regexp *r, Int n);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_INDEX_SUFFIXARRAY_H */
