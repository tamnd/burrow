/* sort.
 *
 * Derived from Go's src/sort/sort.go, src/sort/slice.go and
 * src/sort/search.go.
 * Go source: go1.27.1.
 *
 * Go has three copies of pdqsort: one calling through sort.Interface, one
 * calling a less closure and a swapper that reflect built, and the generic one
 * in slices that Ints, Float64s and Strings go to. The first two make the same
 * calls in the same order, so here they are one copy that calls through a
 * pair of function pointers, SortLessSwap. The third is written out once per
 * element type, comparing the values directly.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sort.h"

#include "internal.h"
#include "pdqsort.h"

#include "burrow/core.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ less and swap */

/* Go's lessSwap, which both sort.Interface and sort.Slice come down to. */
typedef struct SortLessSwap {
    bool (*less)(void *env, Int i, Int j);
    void *less_env;
    void (*swap)(void *env, Int i, Int j);
    void *swap_env;
} SortLessSwap;

static inline bool sort_ls_less(const SortLessSwap *d, Int i, Int j) {
    return d->less(d->less_env, i, j);
}

static inline void sort_ls_swap(const SortLessSwap *d, Int i, Int j) {
    d->swap(d->swap_env, i, j);
}

SORT_PDQ_DEFINE(ls, const SortLessSwap *, sort_ls_less, sort_ls_swap)
SORT_STABLE_DEFINE(ls, const SortLessSwap *, sort_ls_less, sort_ls_swap)

static SortLessSwap sort_ls_of(SortInterface data) {
    SortLessSwap d = {data.vt->less, data.data, data.vt->swap, data.data};
    return d;
}

/* ------------------------------------------------------------ Interface */

void sort_sort(SortInterface data) {
    Int n = data.vt->len(data.data);
    if (n <= 1)
        return;
    SortLessSwap d = sort_ls_of(data);
    sort_pdqsort_ls(&d, 0, n, sort_pdq_limit(n));
}

void sort_stable(SortInterface data) {
    SortLessSwap d = sort_ls_of(data);
    sort_stable_ls(&d, data.vt->len(data.data));
}

bool sort_is_sorted(SortInterface data) {
    Int n = data.vt->len(data.data);
    for (Int i = n - 1; i > 0; i--) {
        if (data.vt->less(data.data, i, i - 1))
            return false;
    }
    return true;
}

void burrow__sort_heapsort(SortInterface data) {
    SortLessSwap d = sort_ls_of(data);
    sort_heap_sort_ls(&d, 0, data.vt->len(data.data));
}

void burrow__sort_reverse_range(SortInterface data, Int a, Int b) {
    SortLessSwap d = sort_ls_of(data);
    sort_reverse_range_ls(&d, a, b);
}

/* ------------------------------------------------------------ Reverse */

static Int sort_reverse_len(void *self) {
    SortReverse *r = self;
    return r->interface.vt->len(r->interface.data);
}

static bool sort_reverse_less(void *self, Int i, Int j) {
    SortReverse *r = self;
    return r->interface.vt->less(r->interface.data, j, i);
}

static void sort_reverse_swap(void *self, Int i, Int j) {
    SortReverse *r = self;
    r->interface.vt->swap(r->interface.data, i, j);
}

#define SORT_TYPE(var, gonm, ctype)                                                    \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"sort", 4},                                                     \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

SORT_TYPE(sort_reverse_desc, "reverse", SortReverse);

static const SortInterfaceVT sort_reverse_vt = {
    &sort_reverse_desc,
    sort_reverse_len,
    sort_reverse_less,
    sort_reverse_swap,
};

SortReverse sort_reverse(SortInterface data) {
    SortReverse r = {data};
    return r;
}

SortInterface sort_reverse_as_sort_interface(SortReverse *r) {
    SortInterface i = {&sort_reverse_vt, r};
    return i;
}

/* ------------------------------------------------------------ Slice */

/* Go's reflectlite.Swapper: swaps two elements of a slice of any type. Sizes
 * that fit a register or two are swapped as one, the way Go special cases
 * them. */
static void sort_swap_bytes(void *env, Int i, Int j) {
    const Slice *s = env;
    size_t size = s->elem->size;
    unsigned char *a = (unsigned char *)s->p + (size_t)i * size;
    unsigned char *b = (unsigned char *)s->p + (size_t)j * size;
    unsigned char tmp[64];
    while (size > 0) {
        size_t n = size < sizeof tmp ? size : sizeof tmp;
        memcpy(tmp, a, n);
        memcpy(a, b, n);
        memcpy(b, tmp, n);
        a += n;
        b += n;
        size -= n;
    }
}

/* The copies are memcpy of a constant size, which compilers turn into plain
 * loads and stores, so the element is never read through a type it does not
 * have. */
#define SORT_SWAP_WORD(name, T)                                                        \
    static void name(void *env, Int i, Int j) {                                        \
        unsigned char *p = ((const Slice *)env)->p;                                    \
        unsigned char *a = p + (size_t)i * sizeof(T);                                  \
        unsigned char *b = p + (size_t)j * sizeof(T);                                  \
        T t;                                                                           \
        memcpy(&t, a, sizeof t);                                                       \
        memcpy(a, b, sizeof t);                                                        \
        memcpy(b, &t, sizeof t);                                                       \
    }

SORT_SWAP_WORD(sort_swap_8, uint8_t)
SORT_SWAP_WORD(sort_swap_16, uint16_t)
SORT_SWAP_WORD(sort_swap_32, uint32_t)
SORT_SWAP_WORD(sort_swap_64, uint64_t)

/* Two words, which is a Str, an Any, an interface or a Slice header less its
 * capacity, and the most common element size after a word. */
typedef struct SortPair {
    uint64_t a, b;
} SortPair;

SORT_SWAP_WORD(sort_swap_128, SortPair)

/* Any other size that is a whole number of words, a word at a time. */
static void sort_swap_words(void *env, Int i, Int j) {
    const Slice *s = env;
    size_t n = s->elem->size / sizeof(uint64_t);
    unsigned char *a = (unsigned char *)s->p + (size_t)i * n * sizeof(uint64_t);
    unsigned char *b = (unsigned char *)s->p + (size_t)j * n * sizeof(uint64_t);
    for (size_t k = 0; k < n; k++, a += sizeof(uint64_t), b += sizeof(uint64_t)) {
        uint64_t t;
        memcpy(&t, a, sizeof t);
        memcpy(a, b, sizeof t);
        memcpy(b, &t, sizeof t);
    }
}

static void sort_swap_nothing(void *env, Int i, Int j) {
    (void)env;
    (void)i;
    (void)j;
}

static SortLessSwap sort_ls_of_slice(Slice *x, SortLessFunc less) {
    void (*swap)(void *, Int, Int) = sort_swap_bytes;
    size_t size = x->elem->size;
    if (size == 0)
        swap = sort_swap_nothing;
    else if (size == 1)
        swap = sort_swap_8;
    else if (size == 2)
        swap = sort_swap_16;
    else if (size == 4)
        swap = sort_swap_32;
    else if (size == 8)
        swap = sort_swap_64;
    else if (size == 16)
        swap = sort_swap_128;
    else if (size % 8 == 0)
        swap = sort_swap_words;
    SortLessSwap d = {less.f, less.env, swap, x};
    return d;
}

void sort_slice(Slice x, SortLessFunc less) {
    SortLessSwap d = sort_ls_of_slice(&x, less);
    Int length = x.len;
    sort_pdqsort_ls(&d, 0, length, sort_pdq_limit(length));
}

void sort_slice_stable(Slice x, SortLessFunc less) {
    SortLessSwap d = sort_ls_of_slice(&x, less);
    sort_stable_ls(&d, x.len);
}

bool sort_slice_is_sorted(Slice x, SortLessFunc less) {
    Int n = x.len;
    for (Int i = n - 1; i > 0; i--) {
        if (less.f(less.env, i, i - 1))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------ Search */

Int sort_search(Int n, SortSearchFunc f) {
    /* Define f(-1) == false and f(n) == true.
     * Invariant: f(i-1) == false, f(j) == true. */
    Int i = 0, j = n;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1); /* avoid overflow when computing h */
        /* i <= h < j */
        if (!f.f(f.env, h))
            i = h + 1; /* preserves f(i-1) == false */
        else
            j = h; /* preserves f(j) == true */
    }
    /* i == j, f(i-1) == false, and f(j) (= f(i)) == true  =>  answer is i. */
    return i;
}

Int sort_find(Int n, SortFindFunc cmp, bool *found) {
    /* The invariants here are similar to the ones in Search.
     * Define cmp(-1) > 0 and cmp(n) <= 0
     * Invariant: cmp(i-1) > 0, cmp(j) <= 0 */
    Int i = 0, j = n;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1); /* avoid overflow when computing h */
        /* i <= h < j */
        if (cmp.f(cmp.env, h) > 0)
            i = h + 1; /* preserves cmp(i-1) > 0 */
        else
            j = h; /* preserves cmp(j) <= 0 */
    }
    /* i == j, cmp(i-1) > 0 and cmp(j) <= 0 */
    bool ok = i < n && cmp.f(cmp.env, i) == 0;
    if (found != NULL)
        *found = ok;
    return i;
}

/* ------------------------------------------------------------ Ints, Float64s, Strings */

/* cmp.Less for each of the three, which puts a NaN first. It is also exactly
 * Float64Slice.Less, so the Sort method and sort_float64s agree. */
static inline bool sort_int_less(const Int *d, Int i, Int j) {
    return d[i] < d[j];
}

static inline bool sort_is_nan(double f) {
    return f != f;
}

static inline bool sort_float64_less(const double *d, Int i, Int j) {
    return (sort_is_nan(d[i]) && !sort_is_nan(d[j])) || d[i] < d[j];
}

/* str_cmp(a, b) < 0, with the first few bytes compared inline. Sorted strings
 * usually differ early, and memcmp is a call into libc that costs more than
 * the comparison when they do. */
static inline bool sort_str_less(const Str *d, Int i, Int j) {
    Str a = d[i], b = d[j];
    Int n = a.len < b.len ? a.len : b.len;
    Int head = n < 8 ? n : 8;
    for (Int k = 0; k < head; k++) {
        if (a.p[k] != b.p[k])
            return a.p[k] < b.p[k];
    }
    if (n > head) {
        int r = memcmp(a.p + head, b.p + head, (size_t)(n - head));
        if (r != 0)
            return r < 0;
    }
    return a.len < b.len;
}

static inline void sort_int_swap(Int *d, Int i, Int j) {
    Int t = d[i];
    d[i] = d[j];
    d[j] = t;
}

static inline void sort_float64_swap(double *d, Int i, Int j) {
    double t = d[i];
    d[i] = d[j];
    d[j] = t;
}

static inline void sort_str_swap(Str *d, Int i, Int j) {
    Str t = d[i];
    d[i] = d[j];
    d[j] = t;
}

SORT_PDQ_DEFINE(int, Int *, sort_int_less, sort_int_swap)
SORT_PDQ_DEFINE(float64, double *, sort_float64_less, sort_float64_swap)
SORT_PDQ_DEFINE(str, Str *, sort_str_less, sort_str_swap)

void sort_ints(Slice x) {
    sort_pdqsort_int(x.p, 0, x.len, sort_pdq_limit(x.len));
}

void sort_float64s(Slice x) {
    sort_pdqsort_float64(x.p, 0, x.len, sort_pdq_limit(x.len));
}

void sort_strings(Slice x) {
    sort_pdqsort_str(x.p, 0, x.len, sort_pdq_limit(x.len));
}

bool sort_ints_are_sorted(Slice x) {
    const Int *d = x.p;
    for (Int i = x.len - 1; i > 0; i--) {
        if (sort_int_less(d, i, i - 1))
            return false;
    }
    return true;
}

bool sort_float64s_are_sorted(Slice x) {
    const double *d = x.p;
    for (Int i = x.len - 1; i > 0; i--) {
        if (sort_float64_less(d, i, i - 1))
            return false;
    }
    return true;
}

bool sort_strings_are_sorted(Slice x) {
    const Str *d = x.p;
    for (Int i = x.len - 1; i > 0; i--) {
        if (sort_str_less(d, i, i - 1))
            return false;
    }
    return true;
}

/* Go's SearchInts and the other two are Search with a closure. These are the
 * same loop with the comparison written in. */
Int sort_search_ints(Slice a, Int x) {
    const Int *d = a.p;
    Int i = 0, j = a.len;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1);
        if (!(d[h] >= x))
            i = h + 1;
        else
            j = h;
    }
    return i;
}

Int sort_search_float64s(Slice a, double x) {
    const double *d = a.p;
    Int i = 0, j = a.len;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1);
        if (!(d[h] >= x))
            i = h + 1;
        else
            j = h;
    }
    return i;
}

Int sort_search_strings(Slice a, Str x) {
    const Str *d = a.p;
    Int i = 0, j = a.len;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1);
        if (!(str_cmp(d[h], x) >= 0))
            i = h + 1;
        else
            j = h;
    }
    return i;
}

/* ------------------------------------------------------------ IntSlice and friends */

#define SORT_SLICE_TYPE(prefix, T, less_fn, swap_fn, search_fn, gonm)                  \
    Int prefix##_len(Slice x) {                                                        \
        return x.len;                                                                  \
    }                                                                                  \
    bool prefix##_less(Slice x, Int i, Int j) {                                        \
        return less_fn((const T *)x.p, i, j);                                          \
    }                                                                                  \
    void prefix##_swap(Slice x, Int i, Int j) {                                        \
        swap_fn((T *)x.p, i, j);                                                       \
    }                                                                                  \
    Int prefix##_search(Slice p, T x) {                                                \
        return search_fn(p, x);                                                        \
    }                                                                                  \
    static Int prefix##_vt_len(void *self) {                                           \
        return ((const Slice *)self)->len;                                             \
    }                                                                                  \
    static bool prefix##_vt_less(void *self, Int i, Int j) {                           \
        return less_fn((const T *)((const Slice *)self)->p, i, j);                     \
    }                                                                                  \
    static void prefix##_vt_swap(void *self, Int i, Int j) {                           \
        swap_fn((T *)((const Slice *)self)->p, i, j);                                  \
    }                                                                                  \
    SORT_TYPE(prefix##_desc, gonm, Slice);                                             \
    static const SortInterfaceVT prefix##_vt = {                                       \
        &prefix##_desc,                                                                \
        prefix##_vt_len,                                                               \
        prefix##_vt_less,                                                              \
        prefix##_vt_swap,                                                              \
    };                                                                                 \
    SortInterface prefix##_as_sort_interface(Slice *x) {                               \
        SortInterface i = {&prefix##_vt, x};                                           \
        return i;                                                                      \
    }

SORT_SLICE_TYPE(sort_int_slice, Int, sort_int_less, sort_int_swap, sort_search_ints,
                "IntSlice")
SORT_SLICE_TYPE(sort_float64_slice, double, sort_float64_less, sort_float64_swap,
                sort_search_float64s, "Float64Slice")
SORT_SLICE_TYPE(sort_string_slice, Str, sort_str_less, sort_str_swap,
                sort_search_strings, "StringSlice")

/* Go's Sort methods go through sort.Sort. Their less is the one above and so
 * is their swap, so the typed copy makes the same calls in the same order and
 * leaves the slice the same, only without the calls through a pointer. */
void sort_int_slice_sort(SortIntSlice x) {
    sort_ints(x);
}

void sort_float64_slice_sort(SortFloat64Slice x) {
    sort_float64s(x);
}

void sort_string_slice_sort(SortStringSlice x) {
    sort_strings(x);
}
