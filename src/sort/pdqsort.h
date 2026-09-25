/* Pattern-defeating quicksort and the stable merge, as one macro that writes
 * them out for a given way of comparing and swapping.
 *
 * Derived from Go's src/sort/zsortinterface.go, src/sort/zsortfunc.go and
 * src/slices/zsortordered.go, which gen_sort_variants.go writes out from one
 * template, and this is that template.
 * Go source: go1.27.1.
 *
 * Go generates a copy per variant rather than sharing one because a call
 * through an interface or a closure is what these loops spend their time on,
 * and a compare that the compiler can see is several times faster. The same is
 * true in C, and a macro is how C gets a copy per variant. Each variant must
 * behave exactly as Go's does, down to the order it leaves equal elements in,
 * because that order is observable and programs come to depend on it.
 *
 * SORT_PDQ_DEFINE(N, D, LESS, SWAP) defines static functions whose names end
 * in _N. D is the type of the data argument, and LESS(data, i, j) and
 * SWAP(data, i, j) compare and swap the elements at two indexes.
 *
 *   sort_pdqsort_N(data, a, b, limit)   Go's pdqsort, unstable
 *   sort_heap_sort_N(data, a, b)        Go's heapSort
 *   sort_insertion_sort_N(data, a, b)   Go's insertionSort
 *   sort_reverse_range_N(data, a, b)    Go's reverseRange
 *
 * SORT_STABLE_DEFINE(N, D, LESS, SWAP) adds sort_stable_N(data, n), Go's
 * stable, and needs SORT_PDQ_DEFINE for the same N first.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_SORT_PDQSORT_H
#define BURROW_SRC_SORT_PDQSORT_H

#include "burrow/sort.h"

#include "burrow/core.h"
#include "burrow/math/bits.h"

#include <stdbool.h>
#include <stdint.h>

/* Go's sortedHint. */
enum { SORT_UNKNOWN_HINT, SORT_INCREASING_HINT, SORT_DECREASING_HINT };

/* Go's xorshift.Next. */
static inline uint64_t sort_xorshift_next(uint64_t *r) {
    *r ^= *r << 13;
    *r ^= *r >> 7;
    *r ^= *r << 17;
    return *r;
}

/* Go's nextPowerOfTwo. */
static inline Uint sort_next_power_of_two(Int length) {
    Uint shift = (Uint)bits_len((Uint)length);
    return (Uint)1 << shift;
}

/* The limit Go's Sort hands pdqsort: bits.Len(uint(n)). */
static inline Int sort_pdq_limit(Int n) {
    return bits_len((Uint)n);
}

/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define SORT_PDQ_DEFINE(N, D, LESS, SWAP)                                                \
    /* insertionSort sorts data[a:b] using insertion sort. */                            \
    static void sort_insertion_sort_##N(D data, Int a, Int b) {                          \
        for (Int i = a + 1; i < b; i++) {                                                \
            for (Int j = i; j > a && LESS(data, j, j - 1); j--)                          \
                SWAP(data, j, j - 1);                                                    \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    /* siftDown implements the heap property on data[lo:hi]. first is an offset   \
     * into the array where the root of the heap lies. */      \
    static void sort_sift_down_##N(D data, Int lo, Int hi, Int first) {                  \
        Int root = lo;                                                                   \
        for (;;) {                                                                       \
            Int child = 2 * root + 1;                                                    \
            if (child >= hi)                                                             \
                break;                                                                   \
            if (child + 1 < hi && LESS(data, first + child, first + child + 1))          \
                child++;                                                                 \
            if (!LESS(data, first + root, first + child))                                \
                return;                                                                  \
            SWAP(data, first + root, first + child);                                     \
            root = child;                                                                \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    static void sort_heap_sort_##N(D data, Int a, Int b) {                               \
        Int first = a;                                                                   \
        Int lo = 0;                                                                      \
        Int hi = b - a;                                                                  \
                                                                                         \
        /* Build heap with greatest element at top. */                                   \
        for (Int i = (hi - 1) / 2; i >= 0; i--)                                          \
            sort_sift_down_##N(data, i, hi, first);                                      \
                                                                                         \
        /* Pop elements, largest first, into end of data. */                             \
        for (Int i = hi - 1; i >= 0; i--) {                                              \
            SWAP(data, first, first + i);                                                \
            sort_sift_down_##N(data, lo, i, first);                                      \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    /* order2 returns x,y where data[x] <= data[y], where x,y=a,b or x,y=b,a. */         \
    static void sort_order2_##N(D data, Int *a, Int *b, Int *swaps) {                    \
        if (LESS(data, *b, *a)) {                                                        \
            (*swaps)++;                                                                  \
            Int t = *a;                                                                  \
            *a = *b;                                                                     \
            *b = t;                                                                      \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    /* median returns x where data[x] is the median of a,b,c, where x is a, b, or \
     * c. */      \
    static Int sort_median_##N(D data, Int a, Int b, Int c, Int *swaps) {                \
        sort_order2_##N(data, &a, &b, swaps);                                            \
        sort_order2_##N(data, &b, &c, swaps);                                            \
        sort_order2_##N(data, &a, &b, swaps);                                            \
        return b;                                                                        \
    }                                                                                    \
                                                                                         \
    /* medianAdjacent finds the median of data[a - 1], data[a], data[a + 1] and   \
     * stores the index into a. */      \
    static Int sort_median_adjacent_##N(D data, Int a, Int *swaps) {                     \
        return sort_median_##N(data, a - 1, a, a + 1, swaps);                            \
    }                                                                                    \
                                                                                         \
    /* choosePivot chooses a pivot in data[a:b].                                  \
     *                                                                            \
     * [0,8): chooses a static pivot.                                             \
     * [8,shortestNinther): uses the simple median-of-three method.               \
     * [shortestNinther,inf): uses the Tukey ninther method. */      \
    static Int sort_choose_pivot_##N(D data, Int a, Int b, int *hint) {                  \
        enum { shortest_ninther = 50, max_swaps = 4 * 3 };                               \
                                                                                         \
        Int l = b - a;                                                                   \
        Int swaps = 0;                                                                   \
        Int i = a + l / 4 * 1;                                                           \
        Int j = a + l / 4 * 2;                                                           \
        Int k = a + l / 4 * 3;                                                           \
                                                                                         \
        if (l >= 8) {                                                                    \
            if (l >= shortest_ninther) {                                                 \
                /* Tukey ninther method, the idea came from Rust's implementation. */    \
                i = sort_median_adjacent_##N(data, i, &swaps);                           \
                j = sort_median_adjacent_##N(data, j, &swaps);                           \
                k = sort_median_adjacent_##N(data, k, &swaps);                           \
            }                                                                            \
            /* Find the median among i, j, k and stores it into j. */                    \
            j = sort_median_##N(data, i, j, k, &swaps);                                  \
        }                                                                                \
                                                                                         \
        if (swaps == 0)                                                                  \
            *hint = SORT_INCREASING_HINT;                                                \
        else if (swaps == max_swaps)                                                     \
            *hint = SORT_DECREASING_HINT;                                                \
        else                                                                             \
            *hint = SORT_UNKNOWN_HINT;                                                   \
        return j;                                                                        \
    }                                                                                    \
                                                                                         \
    static void sort_reverse_range_##N(D data, Int a, Int b) {                           \
        Int i = a;                                                                       \
        Int j = b - 1;                                                                   \
        while (i < j) {                                                                  \
            SWAP(data, i, j);                                                            \
            i++;                                                                         \
            j--;                                                                         \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    /* partialInsertionSort partially sorts a slice, returns true if the slice is \
     * sorted at the end. */      \
    static bool sort_partial_insertion_sort_##N(D data, Int a, Int b) {                  \
        enum { max_steps = 5, shortest_shifting = 50 };                                  \
        Int i = a + 1;                                                                   \
        for (Int j = 0; j < max_steps; j++) {                                            \
            while (i < b && !LESS(data, i, i - 1))                                       \
                i++;                                                                     \
                                                                                         \
            if (i == b)                                                                  \
                return true;                                                             \
                                                                                         \
            if (b - a < shortest_shifting)                                               \
                return false;                                                            \
                                                                                         \
            SWAP(data, i, i - 1);                                                        \
                                                                                         \
            /* Shift the smaller one to the left. */                                     \
            if (i - a >= 2) {                                                            \
                for (Int k = i - 1; k >= 1; k--) {                                       \
                    if (!LESS(data, k, k - 1))                                           \
                        break;                                                           \
                    SWAP(data, k, k - 1);                                                \
                }                                                                        \
            }                                                                            \
            /* Shift the greater one to the right. */                                    \
            if (b - i >= 2) {                                                            \
                for (Int k = i + 1; k < b; k++) {                                        \
                    if (!LESS(data, k, k - 1))                                           \
                        break;                                                           \
                    SWAP(data, k, k - 1);                                                \
                }                                                                        \
            }                                                                            \
        }                                                                                \
        return false;                                                                    \
    }                                                                                    \
                                                                                         \
    /* breakPatterns scatters some elements around in an attempt to break some    \
     * patterns that might cause imbalanced partitions in quicksort. */      \
    static void sort_break_patterns_##N(D data, Int a, Int b) {                          \
        Int length = b - a;                                                              \
        if (length >= 8) {                                                               \
            uint64_t random = (uint64_t)length;                                          \
            Uint modulus = sort_next_power_of_two(length);                               \
                                                                                         \
            Int idx = a + (length / 4) * 2 - 1;                                          \
            for (; idx <= a + (length / 4) * 2 + 1; idx++) {                             \
                Int other = (Int)((Uint)sort_xorshift_next(&random) & (modulus - 1));    \
                if (other >= length)                                                     \
                    other -= length;                                                     \
                SWAP(data, idx, a + other);                                              \
            }                                                                            \
        }                                                                                \
    }                                                                                    \
                                                                                         \
    /* partition does one quicksort partition. Let p = data[pivot]. Moves         \
     * elements in data[a:b] around, so that data[i]<p and data[j]>=p for          \
     * i<newpivot and j>newpivot. On return, data[newpivot] = p. */      \
    static Int sort_partition_##N(D data, Int a, Int b, Int pivot,                       \
                                  bool *already_partitioned) {                           \
        SWAP(data, a, pivot);                                                            \
        /* i and j are inclusive of the elements remaining to be partitioned */          \
        Int i = a + 1, j = b - 1;                                                        \
                                                                                         \
        while (i <= j && LESS(data, i, a))                                               \
            i++;                                                                         \
        while (i <= j && !LESS(data, j, a))                                              \
            j--;                                                                         \
        if (i > j) {                                                                     \
            SWAP(data, j, a);                                                            \
            *already_partitioned = true;                                                 \
            return j;                                                                    \
        }                                                                                \
        SWAP(data, i, j);                                                                \
        i++;                                                                             \
        j--;                                                                             \
                                                                                         \
        for (;;) {                                                                       \
            while (i <= j && LESS(data, i, a))                                           \
                i++;                                                                     \
            while (i <= j && !LESS(data, j, a))                                          \
                j--;                                                                     \
            if (i > j)                                                                   \
                break;                                                                   \
            SWAP(data, i, j);                                                            \
            i++;                                                                         \
            j--;                                                                         \
        }                                                                                \
        SWAP(data, j, a);                                                                \
        *already_partitioned = false;                                                    \
        return j;                                                                        \
    }                                                                                    \
                                                                                         \
    /* partitionEqual partitions data[a:b] into elements equal to data[pivot]     \
     * followed by elements greater than data[pivot]. It assumed that data[a:b]   \
     * does not contain elements smaller than the data[pivot]. */      \
    static Int sort_partition_equal_##N(D data, Int a, Int b, Int pivot) {               \
        SWAP(data, a, pivot);                                                            \
        /* i and j are inclusive of the elements remaining to be partitioned */          \
        Int i = a + 1, j = b - 1;                                                        \
                                                                                         \
        for (;;) {                                                                       \
            while (i <= j && !LESS(data, a, i))                                          \
                i++;                                                                     \
            while (i <= j && LESS(data, a, j))                                           \
                j--;                                                                     \
            if (i > j)                                                                   \
                break;                                                                   \
            SWAP(data, i, j);                                                            \
            i++;                                                                         \
            j--;                                                                         \
        }                                                                                \
        return i;                                                                        \
    }                                                                                    \
                                                                                         \
    /* pdqsort sorts data[a:b]. The algorithm based on pattern-defeating          \
     * quicksort(pdqsort), but without the optimizations from BlockQuicksort.     \
     * pdqsort paper: https://arxiv.org/pdf/2106.05123.pdf                        \
     * C++ implementation: https://github.com/orlp/pdqsort                        \
     * Rust implementation: https://docs.rs/pdqsort/latest/pdqsort/               \
     * limit is the number of allowed bad (very unbalanced) pivots before falling \
     * back to heapsort. */      \
    static void sort_pdqsort_##N(D data, Int a, Int b, Int limit) {                      \
        enum { max_insertion = 12 };                                                     \
                                                                                         \
        /* whether the last partitioning was reasonably balanced */                      \
        bool was_balanced = true;                                                        \
        /* whether the slice was already partitioned */                                  \
        bool was_partitioned = true;                                                     \
                                                                                         \
        for (;;) {                                                                       \
            Int length = b - a;                                                          \
                                                                                         \
            if (length <= max_insertion) {                                               \
                sort_insertion_sort_##N(data, a, b);                                     \
                return;                                                                  \
            }                                                                            \
                                                                                         \
            /* Fall back to heapsort if too many bad choices were made. */               \
            if (limit == 0) {                                                            \
                sort_heap_sort_##N(data, a, b);                                          \
                return;                                                                  \
            }                                                                            \
                                                                                         \
            /* If the last partitioning was imbalanced, we need to breakPatterns. */     \
            if (!was_balanced) {                                                         \
                sort_break_patterns_##N(data, a, b);                                     \
                limit--;                                                                 \
            }                                                                            \
                                                                                         \
            int hint;                                                                    \
            Int pivot = sort_choose_pivot_##N(data, a, b, &hint);                        \
            if (hint == SORT_DECREASING_HINT) {                                          \
                sort_reverse_range_##N(data, a, b);                                      \
                /* The chosen pivot was pivot-a elements after the start of the        \
                 * array. After reversing it is pivot-a elements before the end of     \
                 * the array. The idea came from Rust's implementation. */ \
                pivot = (b - 1) - (pivot - a);                                           \
                hint = SORT_INCREASING_HINT;                                             \
            }                                                                            \
                                                                                         \
            /* The slice is likely already sorted. */                                    \
            if (was_balanced && was_partitioned && hint == SORT_INCREASING_HINT) {       \
                if (sort_partial_insertion_sort_##N(data, a, b))                         \
                    return;                                                              \
            }                                                                            \
                                                                                         \
            /* Probably the slice contains many duplicate elements, partition the      \
             * slice into elements equal to and elements greater than the pivot. */ \
            if (a > 0 && !LESS(data, a - 1, pivot)) {                                    \
                Int mid = sort_partition_equal_##N(data, a, b, pivot);                   \
                a = mid;                                                                 \
                continue;                                                                \
            }                                                                            \
                                                                                         \
            bool already_partitioned;                                                    \
            Int mid = sort_partition_##N(data, a, b, pivot, &already_partitioned);       \
            was_partitioned = already_partitioned;                                       \
                                                                                         \
            Int left_len = mid - a, right_len = b - mid;                                 \
            Int balance_threshold = length / 8;                                          \
            if (left_len < right_len) {                                                  \
                was_balanced = left_len >= balance_threshold;                            \
                sort_pdqsort_##N(data, a, mid, limit);                                   \
                a = mid + 1;                                                             \
            } else {                                                                     \
                was_balanced = right_len >= balance_threshold;                           \
                sort_pdqsort_##N(data, mid + 1, b, limit);                               \
                b = mid;                                                                 \
            }                                                                            \
        }                                                                                \
    }

#define SORT_STABLE_DEFINE(N, D, LESS, SWAP)                                           \
    static void sort_swap_range_##N(D data, Int a, Int b, Int n) {                     \
        for (Int i = 0; i < n; i++)                                                    \
            SWAP(data, a + i, b + i);                                                  \
    }                                                                                  \
                                                                                       \
    /* rotate rotates two consecutive blocks u = data[a:m] and v = data[m:b] in   \
     * data: Data of the form 'x u v y' is changed to 'x v u y'. rotate performs  \
     * at most b-a many calls to data.Swap, and it assumes non-degenerate          \
     * arguments: a < m && m < b. */    \
    static void sort_rotate_##N(D data, Int a, Int m, Int b) {                         \
        Int i = m - a;                                                                 \
        Int j = b - m;                                                                 \
                                                                                       \
        while (i != j) {                                                               \
            if (i > j) {                                                               \
                sort_swap_range_##N(data, m - i, m, j);                                \
                i -= j;                                                                \
            } else {                                                                   \
                sort_swap_range_##N(data, m - i, m + j - i, i);                        \
                j -= i;                                                                \
            }                                                                          \
        }                                                                              \
        /* i == j */                                                                   \
        sort_swap_range_##N(data, m - i, m, i);                                        \
    }                                                                                  \
                                                                                       \
    /* symMerge merges the two sorted subsequences data[a:m] and data[m:b] using  \
     * the SymMerge algorithm from Pok-Son Kim and Arne Kutzner, "Stable Minimum  \
     * Storage Merging by Symmetric Comparisons", in Susanne Albers and Tomasz    \
     * Radzik, editors, Algorithms - ESA 2004, volume 3221 of Lecture Notes in    \
     * Computer Science, pages 714-723. Springer, 2004.                           \
     *                                                                            \
     * Let M = m-a and N = b-n. Wolog M < N. The recursion depth is bound by      \
     * ceil(log(N+M)). The algorithm needs O(M*log(N/M + 1)) calls to             \
     * data.Less. The algorithm needs O((M+N)*log(M)) calls to data.Swap.         \
     *                                                                            \
     * The paper gives O((M+N)*log(M)) as the number of assignments assuming a    \
     * rotation algorithm which uses O(M+N+gcd(M+N)) assignments. The argument in \
     * the paper carries through for Swap operations, especially as the block     \
     * swapping rotate uses only O(M+N) Swaps.                                    \
     *                                                                            \
     * symMerge assumes non-degenerate arguments: a < m && m < b. Having the      \
     * caller check this condition eliminates many leaf recursion calls, which    \
     * improves performance. */    \
    static void sort_sym_merge_##N(D data, Int a, Int m, Int b) {                      \
        /* Avoid unnecessary recursions of symMerge by direct insertion of         \
         * data[a] into data[m:b] if data[a:m] only contains one element. */   \
        if (m - a == 1) {                                                              \
            /* Use binary search to find the lowest index i such that data[i] >=   \
             * data[a] for m <= i < b. Exit the search loop with i == b in case    \
             * no such index exists. */   \
            Int i = m;                                                                 \
            Int j = b;                                                                 \
            while (i < j) {                                                            \
                Int h = (Int)(((Uint)i + (Uint)j) >> 1);                               \
                if (LESS(data, h, a))                                                  \
                    i = h + 1;                                                         \
                else                                                                   \
                    j = h;                                                             \
            }                                                                          \
            /* Swap values until data[a] reaches the position before i. */             \
            for (Int k = a; k < i - 1; k++)                                            \
                SWAP(data, k, k + 1);                                                  \
            return;                                                                    \
        }                                                                              \
                                                                                       \
        /* Avoid unnecessary recursions of symMerge by direct insertion of         \
         * data[m] into data[a:m] if data[m:b] only contains one element. */   \
        if (b - m == 1) {                                                              \
            /* Use binary search to find the lowest index i such that data[i] >    \
             * data[m] for a <= i < m. Exit the search loop with i == m in case    \
             * no such index exists. */   \
            Int i = a;                                                                 \
            Int j = m;                                                                 \
            while (i < j) {                                                            \
                Int h = (Int)(((Uint)i + (Uint)j) >> 1);                               \
                if (!LESS(data, m, h))                                                 \
                    i = h + 1;                                                         \
                else                                                                   \
                    j = h;                                                             \
            }                                                                          \
            /* Swap values until data[m] reaches the position i. */                    \
            for (Int k = m; k > i; k--)                                                \
                SWAP(data, k, k - 1);                                                  \
            return;                                                                    \
        }                                                                              \
                                                                                       \
        Int mid = (Int)(((Uint)a + (Uint)b) >> 1);                                     \
        Int n = mid + m;                                                               \
        Int start, r;                                                                  \
        if (m > mid) {                                                                 \
            start = n - b;                                                             \
            r = mid;                                                                   \
        } else {                                                                       \
            start = a;                                                                 \
            r = m;                                                                     \
        }                                                                              \
        Int p = n - 1;                                                                 \
                                                                                       \
        while (start < r) {                                                            \
            Int c = (Int)(((Uint)start + (Uint)r) >> 1);                               \
            if (!LESS(data, p - c, c))                                                 \
                start = c + 1;                                                         \
            else                                                                       \
                r = c;                                                                 \
        }                                                                              \
                                                                                       \
        Int end = n - start;                                                           \
        if (start < m && m < end)                                                      \
            sort_rotate_##N(data, start, m, end);                                      \
        if (a < start && start < mid)                                                  \
            sort_sym_merge_##N(data, a, start, mid);                                   \
        if (mid < end && end < b)                                                      \
            sort_sym_merge_##N(data, mid, end, b);                                     \
    }                                                                                  \
                                                                                       \
    static void sort_stable_##N(D data, Int n) {                                       \
        Int block_size = 20; /* must be > 0 */                                         \
        Int a = 0, b = block_size;                                                     \
        while (b <= n) {                                                               \
            sort_insertion_sort_##N(data, a, b);                                       \
            a = b;                                                                     \
            b += block_size;                                                           \
        }                                                                              \
        sort_insertion_sort_##N(data, a, n);                                           \
                                                                                       \
        while (block_size < n) {                                                       \
            a = 0;                                                                     \
            b = 2 * block_size;                                                        \
            while (b <= n) {                                                           \
                sort_sym_merge_##N(data, a, a + block_size, b);                        \
                a = b;                                                                 \
                b += 2 * block_size;                                                   \
            }                                                                          \
            Int m = a + block_size;                                                    \
            if (m < n)                                                                 \
                sort_sym_merge_##N(data, a, m, n);                                     \
            block_size *= 2;                                                           \
        }                                                                              \
    }
/* NOLINTEND(bugprone-macro-parentheses) */

#endif /* BURROW_SRC_SORT_PDQSORT_H */
