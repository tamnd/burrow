/* The ordered half of slices: sorting, searching, min, max and compare.
 *
 * Derived from Go's src/slices/sort.go, zsortordered.go and zsortanyfunc.go,
 * and Compare from slices.go.
 * Go source: go1.27.1.
 *
 * Go instantiates each of these per element type at compile time. Here the
 * element type is only known at runtime, so every ordered type gets its own
 * copy written out by SLICES_ORDERED, and a switch on the descriptor picks
 * one. The switch runs once per call and the loop inside is as tight as Go's.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/slices.h"

#include "pdqsort.h"

#include "burrow/core.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ element order */

/* cmp.Less for each kind of ordered type. */
static inline bool slices_less_int(int64_t a, int64_t b) {
    return a < b;
}

static inline bool slices_less_uint(uint64_t a, uint64_t b) {
    return a < b;
}

/* x != x is isNaN, as in Go, which needs no math library. */
#define SLICES_NAN(x) ((x) != (x))

/* NOLINTBEGIN(misc-redundant-expression) */
static inline bool slices_less_float32(float a, float b) {
    return (SLICES_NAN(a) && !SLICES_NAN(b)) || a < b;
}

static inline bool slices_less_float64(double a, double b) {
    return (SLICES_NAN(a) && !SLICES_NAN(b)) || a < b;
}

/* BinarySearch's test for a match, where a NaN finds a NaN. */
static inline bool slices_eq_float32(float a, float b) {
    return a == b || (SLICES_NAN(a) && SLICES_NAN(b));
}

static inline bool slices_eq_float64(double a, double b) {
    return a == b || (SLICES_NAN(a) && SLICES_NAN(b));
}
/* NOLINTEND(misc-redundant-expression) */

/* str_cmp(a, b) < 0, with the first few bytes compared inline. Sorted strings
 * usually differ early, and memcmp is a call into libc that costs more than
 * the comparison when they do. */
static inline bool slices_less_str(Str a, Str b) {
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

static inline bool slices_eq_int(int64_t a, int64_t b) {
    return a == b;
}

static inline bool slices_eq_uint(uint64_t a, uint64_t b) {
    return a == b;
}

static inline bool slices_eq_str(Str a, Str b) {
    return str_eq(a, b);
}

/* Every ordered type: the C type, the name its copies are suffixed with, and
 * the less and the equality that suit it. */
#define SLICES_ORDERED(X)                                                              \
    X(int8_t, int8, slices_less_int, slices_eq_int)                                    \
    X(int16_t, int16, slices_less_int, slices_eq_int)                                  \
    X(int32_t, int32, slices_less_int, slices_eq_int)                                  \
    X(int64_t, int64, slices_less_int, slices_eq_int)                                  \
    X(uint8_t, uint8, slices_less_uint, slices_eq_uint)                                \
    X(uint16_t, uint16, slices_less_uint, slices_eq_uint)                              \
    X(uint32_t, uint32, slices_less_uint, slices_eq_uint)                              \
    X(uint64_t, uint64, slices_less_uint, slices_eq_uint)                              \
    X(float, float32, slices_less_float32, slices_eq_float32)                          \
    X(double, float64, slices_less_float64, slices_eq_float64)                         \
    X(Str, str, slices_less_str, slices_eq_str)

#define SLICES_ENUM(T, suffix, less, eq) SLICES_K_##suffix,
typedef enum SlicesKind { SLICES_ORDERED(SLICES_ENUM) } SlicesKind;
#undef SLICES_ENUM

/* Which copy an element type uses, or a panic with msg when it has no order.
 * Go checks this when it compiles, and the panic is the nearest thing. */
static SlicesKind slices_kind(const Type *t, Str msg) {
    if (t != NULL) {
        bool wide = t->size == 8;
        Kind k = t->kind;
        if (k == KIND_INT)
            return wide ? SLICES_K_int64 : SLICES_K_int32;
        if (k == KIND_UINT || k == KIND_UINTPTR)
            return wide ? SLICES_K_uint64 : SLICES_K_uint32;
        if (k == KIND_INT8)
            return SLICES_K_int8;
        if (k == KIND_INT16)
            return SLICES_K_int16;
        if (k == KIND_INT32)
            return SLICES_K_int32;
        if (k == KIND_INT64)
            return SLICES_K_int64;
        if (k == KIND_UINT8)
            return SLICES_K_uint8;
        if (k == KIND_UINT16)
            return SLICES_K_uint16;
        if (k == KIND_UINT32)
            return SLICES_K_uint32;
        if (k == KIND_UINT64)
            return SLICES_K_uint64;
        if (k == KIND_FLOAT32)
            return SLICES_K_float32;
        if (k == KIND_FLOAT64)
            return SLICES_K_float64;
        if (k == KIND_STRING)
            return SLICES_K_str;
    }
    panic_str(msg);
}

/* ------------------------------------------------------------ the copies */

/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define SLICES_DEFINE(T, suffix, less, eq)                                             \
    static inline bool slices_pdq_less_##suffix(T *d, Int i, Int j) {                  \
        return less(d[i], d[j]);                                                       \
    }                                                                                  \
    static inline void slices_pdq_swap_##suffix(T *d, Int i, Int j) {                  \
        T t = d[i];                                                                    \
        d[i] = d[j];                                                                   \
        d[j] = t;                                                                      \
    }                                                                                  \
    SORT_PDQ_DEFINE(slices_##suffix, T *, slices_pdq_less_##suffix,                    \
                    slices_pdq_swap_##suffix)                                          \
    static inline bool slices_is_sorted_##suffix(const T *x, Int n) {                  \
        for (Int i = n - 1; i > 0; i--) {                                              \
            if (less(x[i], x[i - 1]))                                                  \
                return false;                                                          \
        }                                                                              \
        return true;                                                                   \
    }                                                                                  \
    static inline Int slices_search_##suffix(const T *x, Int n, T target,              \
                                             bool *found) {                            \
        Int i = 0, j = n;                                                              \
        while (i < j) {                                                                \
            Int h = (Int)(((Uint)i + (Uint)j) >> 1);                                   \
            if (less(x[h], target))                                                    \
                i = h + 1;                                                             \
            else                                                                       \
                j = h;                                                                 \
        }                                                                              \
        *found = i < n && eq(x[i], target);                                            \
        return i;                                                                      \
    }                                                                                  \
    static inline int slices_compare_##suffix(const T *a, Int na, const T *b,          \
                                              Int nb) {                                \
        for (Int i = 0; i < na; i++) {                                                 \
            if (i >= nb)                                                               \
                return +1;                                                             \
            if (less(a[i], b[i]))                                                      \
                return -1;                                                             \
            if (less(b[i], a[i]))                                                      \
                return +1;                                                             \
        }                                                                              \
        return na < nb ? -1 : 0;                                                       \
    }                                                                                  \
    static inline Int slices_min_##suffix(const T *x, Int n) {                         \
        Int m = 0;                                                                     \
        for (Int i = 1; i < n; i++) {                                                  \
            if (less(x[i], x[m]))                                                      \
                m = i;                                                                 \
        }                                                                              \
        return m;                                                                      \
    }                                                                                  \
    static inline Int slices_max_##suffix(const T *x, Int n) {                         \
        Int m = 0;                                                                     \
        for (Int i = 1; i < n; i++) {                                                  \
            if (less(x[m], x[i]))                                                      \
                m = i;                                                                 \
        }                                                                              \
        return m;                                                                      \
    }

SLICES_ORDERED(SLICES_DEFINE)
/* NOLINTEND(bugprone-macro-parentheses) */

/* Go's min and max on floats are not cmp.Less: a NaN anywhere wins, and -0.0
 * is below 0.0 even though the two are equal. */
#define SLICES_FLOAT_MINMAX(T, suffix)                                                 \
    static Int slices_fmin_##suffix(const T *x, Int n) {                               \
        Int m = 0;                                                                     \
        for (Int i = 1; i < n && !SLICES_NAN(x[m]); i++) {                             \
            if (SLICES_NAN(x[i]) || x[i] < x[m] ||                                     \
                (x[i] == x[m] && signbit(x[i]) && !signbit(x[m])))                     \
                m = i;                                                                 \
        }                                                                              \
        return m;                                                                      \
    }                                                                                  \
    static Int slices_fmax_##suffix(const T *x, Int n) {                               \
        Int m = 0;                                                                     \
        for (Int i = 1; i < n && !SLICES_NAN(x[m]); i++) {                             \
            if (SLICES_NAN(x[i]) || x[i] > x[m] ||                                     \
                (x[i] == x[m] && !signbit(x[i]) && signbit(x[m])))                     \
                m = i;                                                                 \
        }                                                                              \
        return m;                                                                      \
    }

SLICES_FLOAT_MINMAX(float, float32)
SLICES_FLOAT_MINMAX(double, float64)

/* The switch every public function makes: SLICES_BODY(T, suffix) for the copy
 * that suits the element type. */
#define SLICES_CASE(T, suffix, less, eq)                                               \
    case SLICES_K_##suffix:                                                            \
        SLICES_BODY(T, suffix);                                                        \
        break;

#define SLICES_SWITCH(k)                                                               \
    switch (k) {                                                                       \
        SLICES_ORDERED(SLICES_CASE)                                                    \
    default:                                                                           \
        break;                                                                         \
    }

/* ------------------------------------------------------------ Sort */

void slices_sort(Slice x) {
    SlicesKind k =
        slices_kind(x.elem, BURROW_S("slices.Sort: element type is not ordered"));
    Int limit = sort_pdq_limit(x.len);
#define SLICES_BODY(T, suffix) sort_pdqsort_slices_##suffix((T *)x.p, 0, x.len, limit)
    SLICES_SWITCH(k);
#undef SLICES_BODY
}

bool slices_is_sorted(Slice x) {
    SlicesKind k =
        slices_kind(x.elem, BURROW_S("slices.IsSorted: element type is not ordered"));
    bool ok = true;
#define SLICES_BODY(T, suffix) ok = slices_is_sorted_##suffix((const T *)x.p, x.len)
    SLICES_SWITCH(k);
#undef SLICES_BODY
    return ok;
}

Int slices_binary_search(Slice x, const void *target, bool *found) {
    SlicesKind k = slices_kind(
        x.elem, BURROW_S("slices.BinarySearch: element type is not ordered"));
    bool ok = false;
    Int i = 0;
#define SLICES_BODY(T, suffix)                                                         \
    i = slices_search_##suffix((const T *)x.p, x.len, *(const T *)target, &ok)
    SLICES_SWITCH(k);
#undef SLICES_BODY
    if (found != NULL)
        *found = ok;
    return i;
}

int slices_compare(Slice s1, Slice s2) {
    SlicesKind k =
        slices_kind(s1.elem, BURROW_S("slices.Compare: element type is not ordered"));
    int c = 0;
#define SLICES_BODY(T, suffix)                                                         \
    c = slices_compare_##suffix((const T *)s1.p, s1.len, (const T *)s2.p, s2.len)
    SLICES_SWITCH(k);
#undef SLICES_BODY
    return c;
}

static const void *slices_at(Slice x, Int i) {
    return (const Byte *)x.p + (size_t)i * x.elem->size;
}

const void *slices_min(Slice x) {
    SlicesKind k =
        slices_kind(x.elem, BURROW_S("slices.Min: element type is not ordered"));
    if (x.len < 1)
        panic_str(BURROW_S("slices.Min: empty list"));
    Int m = 0;
    if (k == SLICES_K_float32) {
        m = slices_fmin_float32(x.p, x.len);
    } else if (k == SLICES_K_float64) {
        m = slices_fmin_float64(x.p, x.len);
    } else {
#define SLICES_BODY(T, suffix) m = slices_min_##suffix((const T *)x.p, x.len)
        SLICES_SWITCH(k);
#undef SLICES_BODY
    }
    return slices_at(x, m);
}

const void *slices_max(Slice x) {
    SlicesKind k =
        slices_kind(x.elem, BURROW_S("slices.Max: element type is not ordered"));
    if (x.len < 1)
        panic_str(BURROW_S("slices.Max: empty list"));
    Int m = 0;
    if (k == SLICES_K_float32) {
        m = slices_fmax_float32(x.p, x.len);
    } else if (k == SLICES_K_float64) {
        m = slices_fmax_float64(x.p, x.len);
    } else {
#define SLICES_BODY(T, suffix) m = slices_max_##suffix((const T *)x.p, x.len)
        SLICES_SWITCH(k);
#undef SLICES_BODY
    }
    return slices_at(x, m);
}

/* ------------------------------------------------------------ the func variants */

/* What pdqsortCmpFunc and stableCmpFunc work on: the elements, their size and
 * the comparison. */
typedef struct SlicesCmpSort {
    Byte *p;
    size_t size;
    SlicesCmpFunc cmp;
} SlicesCmpSort;

static inline bool slices_cf_less(const SlicesCmpSort *d, Int i, Int j) {
    return d->cmp.f(d->cmp.env, d->p + (size_t)i * d->size,
                    d->p + (size_t)j * d->size) < 0;
}

/* Two words, which is a Str, an Any or an interface. */
typedef struct SlicesPair {
    uint64_t a, b;
} SlicesPair;

/* The copies are memcpy of a constant size, which compilers turn into plain
 * loads and stores, so the element is never read through a type it does not
 * have. The switch goes the same way every time for a given sort, so it costs
 * next to nothing beside the call to cmp. */
#define SLICES_SWAP_AS(T, a, b)                                                        \
    do {                                                                               \
        T t_;                                                                          \
        memcpy(&t_, a, sizeof t_);                                                     \
        memcpy(a, b, sizeof t_);                                                       \
        memcpy(b, &t_, sizeof t_);                                                     \
    } while (0)

static inline void slices_swap_bytes(Byte *a, Byte *b, size_t size) {
    switch (size) {
    case 0:
        return;
    case 1:
        SLICES_SWAP_AS(uint8_t, a, b);
        return;
    case 2:
        SLICES_SWAP_AS(uint16_t, a, b);
        return;
    case 4:
        SLICES_SWAP_AS(uint32_t, a, b);
        return;
    case 8:
        SLICES_SWAP_AS(uint64_t, a, b);
        return;
    case 16:
        SLICES_SWAP_AS(SlicesPair, a, b);
        return;
    default:
        break;
    }
    Byte tmp[64];
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

static inline void slices_cf_swap(const SlicesCmpSort *d, Int i, Int j) {
    slices_swap_bytes(d->p + (size_t)i * d->size, d->p + (size_t)j * d->size, d->size);
}

SORT_PDQ_DEFINE(slices_cf, const SlicesCmpSort *, slices_cf_less, slices_cf_swap)
SORT_STABLE_DEFINE(slices_cf, const SlicesCmpSort *, slices_cf_less, slices_cf_swap)

static SlicesCmpSort slices_cf_of(Slice x, SlicesCmpFunc cmp) {
    SlicesCmpSort d = {(Byte *)x.p, x.elem != NULL ? x.elem->size : 0, cmp};
    return d;
}

void slices_sort_func(Slice x, SlicesCmpFunc cmp) {
    SlicesCmpSort d = slices_cf_of(x, cmp);
    sort_pdqsort_slices_cf(&d, 0, x.len, sort_pdq_limit(x.len));
}

void slices_sort_stable_func(Slice x, SlicesCmpFunc cmp) {
    SlicesCmpSort d = slices_cf_of(x, cmp);
    sort_stable_slices_cf(&d, x.len);
}

bool slices_is_sorted_func(Slice x, SlicesCmpFunc cmp) {
    SlicesCmpSort d = slices_cf_of(x, cmp);
    for (Int i = x.len - 1; i > 0; i--) {
        if (slices_cf_less(&d, i, i - 1))
            return false;
    }
    return true;
}

const void *slices_min_func(Slice x, SlicesCmpFunc cmp) {
    if (x.len < 1)
        panic_str(BURROW_S("slices.MinFunc: empty list"));
    const void *m = slices_at(x, 0);
    for (Int i = 1; i < x.len; i++) {
        const void *v = slices_at(x, i);
        if (cmp.f(cmp.env, v, m) < 0)
            m = v;
    }
    return m;
}

const void *slices_max_func(Slice x, SlicesCmpFunc cmp) {
    if (x.len < 1)
        panic_str(BURROW_S("slices.MaxFunc: empty list"));
    const void *m = slices_at(x, 0);
    for (Int i = 1; i < x.len; i++) {
        const void *v = slices_at(x, i);
        if (cmp.f(cmp.env, v, m) > 0)
            m = v;
    }
    return m;
}

Int slices_binary_search_func(Slice x, const void *target, SlicesCmpFunc cmp,
                              bool *found) {
    Int n = x.len;
    Int i = 0, j = n;
    while (i < j) {
        Int h = (Int)(((Uint)i + (Uint)j) >> 1);
        if (cmp.f(cmp.env, slices_at(x, h), target) < 0)
            i = h + 1;
        else
            j = h;
    }
    bool ok = i < n && cmp.f(cmp.env, slices_at(x, i), target) == 0;
    if (found != NULL)
        *found = ok;
    return i;
}

int slices_compare_func(Slice s1, Slice s2, SlicesCmpFunc cmp) {
    for (Int i = 0; i < s1.len; i++) {
        if (i >= s2.len)
            return +1;
        int c = cmp.f(cmp.env, slices_at(s1, i), slices_at(s2, i));
        if (c != 0)
            return c;
    }
    return s1.len < s2.len ? -1 : 0;
}
