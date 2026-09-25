/* slices, derived from Go's src/slices/slices.go (go1.27.1).
 *
 * Everything here works on the bytes of the elements and their size, since
 * none of it needs an order. Equality goes through type_equal, with the types
 * that are only their bytes compared as bytes.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/slices.h"

#include "burrow/core.h"
#include "burrow/math/bits.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

static size_t slices_size(Slice s) {
    return s.elem != NULL ? s.elem->size : 0;
}

static Byte *slices_ptr(Slice s, Int i) {
    return (Byte *)s.p + (size_t)i * slices_size(s);
}

/* memmove that is fine with a NULL pointer when there is nothing to move,
 * which a nil slice has. */
static void slices_move(void *dst, const void *src, size_t n) {
    if (n > 0)
        memmove(dst, src, n);
}

/* Go's clear on s[i:j], which sets the elements to their zero value. */
static void slices_clear(Slice s, Int i, Int j) {
    if (j <= i)
        return;
    if (s.elem->ops != NULL && s.elem->ops->zero != NULL) {
        for (Int k = i; k < j; k++)
            type_zero(s.elem, slices_ptr(s, k));
        return;
    }
    memset(slices_ptr(s, i), 0, (size_t)(j - i) * slices_size(s));
}

/* Whether equality for t is equality of the bytes: no ops to say otherwise,
 * and a kind with no padding, no float and nothing pointed to that could be
 * equal at a different address. */
static bool slices_bytewise(const Type *t) {
    if (t == NULL || (t->ops != NULL && t->ops->equal != NULL))
        return false;
    Kind k = t->kind;
    return k == KIND_BOOL || kind_is_signed(k) || kind_is_unsigned(k) ||
           k == KIND_POINTER || k == KIND_CHAN || k == KIND_UNSAFE_POINTER;
}

/* ------------------------------------------------------------ Equal, Index */

bool slices_equal(Slice s1, Slice s2) {
    if (s1.len != s2.len)
        return false;
    if (s1.len == 0)
        return true;
    if (slices_bytewise(s1.elem))
        return memcmp(s1.p, s2.p, (size_t)s1.len * slices_size(s1)) == 0;
    for (Int i = 0; i < s1.len; i++) {
        if (!type_equal(s1.elem, slices_ptr(s1, i), slices_ptr(s2, i)))
            return false;
    }
    return true;
}

bool slices_equal_func(Slice s1, Slice s2, SlicesEqualFunc eq) {
    if (s1.len != s2.len)
        return false;
    for (Int i = 0; i < s1.len; i++) {
        if (!eq.f(eq.env, slices_ptr(s1, i), slices_ptr(s2, i)))
            return false;
    }
    return true;
}

/* Index over elements that are their bytes, with the common sizes compared
 * as one word. */
#define SLICES_INDEX_WORD(T)                                                           \
    do {                                                                               \
        T want;                                                                        \
        memcpy(&want, v, sizeof want);                                                 \
        const Byte *p = s.p;                                                           \
        for (Int i = 0; i < s.len; i++) {                                              \
            T got;                                                                     \
            memcpy(&got, p + (size_t)i * sizeof(T), sizeof got);                       \
            if (got == want)                                                           \
                return i;                                                              \
        }                                                                              \
        return -1;                                                                     \
    } while (0)

static Int slices_index_bytes(Slice s, const void *v) {
    size_t size = slices_size(s);
    switch (size) {
    case 1: {
        const Byte *p = memchr(s.p, *(const Byte *)v, (size_t)s.len);
        return p == NULL ? -1 : (Int)(p - (const Byte *)s.p);
    }
    case 2:
        SLICES_INDEX_WORD(uint16_t);
    case 4:
        SLICES_INDEX_WORD(uint32_t);
    case 8:
        SLICES_INDEX_WORD(uint64_t);
    default:
        for (Int i = 0; i < s.len; i++) {
            if (memcmp(slices_ptr(s, i), v, size) == 0)
                return i;
        }
        return -1;
    }
}

Int slices_index(Slice s, const void *v) {
    if (s.len == 0)
        return -1;
    if (slices_bytewise(s.elem))
        return slices_index_bytes(s, v);
    for (Int i = 0; i < s.len; i++) {
        if (type_equal(s.elem, v, slices_ptr(s, i)))
            return i;
    }
    return -1;
}

Int slices_index_func(Slice s, SlicesPredFunc f) {
    for (Int i = 0; i < s.len; i++) {
        if (f.f(f.env, slices_ptr(s, i)))
            return i;
    }
    return -1;
}

bool slices_contains(Slice s, const void *v) {
    return slices_index(s, v) >= 0;
}

bool slices_contains_func(Slice s, SlicesPredFunc f) {
    return slices_index_func(s, f) >= 0;
}

/* ------------------------------------------------------------ Reverse, rotate */

static void slices_swap(Byte *a, Byte *b, size_t size) {
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

/* The elements of p[i:j] reversed, each size bytes. */
static void slices_reverse_range(Byte *p, size_t size, Int i, Int j) {
    for (j--; i < j; i++, j--)
        slices_swap(p + (size_t)i * size, p + (size_t)j * size, size);
}

void slices_reverse(Slice s) {
    slices_reverse_range(s.p, slices_size(s), 0, s.len);
}

/* rotateLeft: s[i] becomes s[i+r], wrapping around. Every element moves at
 * most twice. */
static void slices_rotate_left(Byte *p, size_t size, Int n, Int r) {
    slices_reverse_range(p, size, 0, r);
    slices_reverse_range(p, size, r, n);
    slices_reverse_range(p, size, 0, n);
}

static void slices_rotate_right(Byte *p, size_t size, Int n, Int r) {
    slices_rotate_left(p, size, n, n - r);
}

/* Whether the n elements at a and the m at b share any memory. */
static bool slices_overlaps(const Byte *a, Int n, const Byte *b, Int m, size_t size) {
    if (n == 0 || m == 0 || size == 0)
        return false;
    uintptr_t a0 = (uintptr_t)a, b0 = (uintptr_t)b;
    uintptr_t a1 = a0 + (size_t)(n - 1) * size + (size - 1);
    uintptr_t b1 = b0 + (size_t)(m - 1) * size + (size - 1);
    return a0 <= b1 && b0 <= a1;
}

/* ------------------------------------------------------------ Insert, Delete, Replace */

Slice slices_insert(Alloc *a, Slice s, Int i, const void *v, Int m) {
    (void)slice_sub(s, i, s.len); /* bounds check */
    if (m <= 0)
        return s;
    Int n = s.len;
    if (i == n)
        return slice_append(a, s, v, m);
    size_t size = slices_size(s);
    if (m > s.cap - n) {
        /* Appending zeros rather than making a new slice so that the capacity
         * grows the way append grows it. The new array is not s's, so nothing
         * here can overwrite v. */
        Slice s2 = slice_append(a, slice_sub(s, 0, i), NULL, n + m - i);
        if (s2.p == NULL)
            return s2;
        slices_move(slices_ptr(s2, i), v, (size_t)m * size);
        slices_move(slices_ptr(s2, i + m), slices_ptr(s, i), (size_t)(n - i) * size);
        return s2;
    }
    s.len = n + m;
    /* before:
     * s: aaaaaaaabbbbccccccccdddd
     *            ^   ^       ^   ^
     *            i  i+m      n  n+m
     * after:
     * s: aaaaaaaavvvvbbbbcccccccc
     *            ^   ^       ^   ^
     *            i  i+m      n  n+m
     *
     * a are the values that don't move in s, v are the values copied in from
     * v, b and c are the values from s that are shifted up in index, and d
     * are the values that get overwritten. */
    if (!slices_overlaps(v, m, slices_ptr(s, i + m), n - i, size)) {
        /* v does not overlap the c or d regions, so shifting up first
         * leaves it alone. */
        slices_move(slices_ptr(s, i + m), slices_ptr(s, i), (size_t)(n - i) * size);
        slices_move(slices_ptr(s, i), v, (size_t)m * size);
        return s;
    }
    /* v overlaps c or d. Write v on top of d, then rotate. */
    slices_move(slices_ptr(s, n), v, (size_t)m * size);
    slices_rotate_right(slices_ptr(s, i), size, n + m - i, m);
    return s;
}

Slice slices_delete(Slice s, Int i, Int j) {
    (void)slice_sub3(s, i, j, s.len); /* bounds check */
    if (i == j)
        return s;
    Int oldlen = s.len;
    size_t size = slices_size(s);
    slices_move(slices_ptr(s, i), slices_ptr(s, j), (size_t)(oldlen - j) * size);
    s.len = oldlen - (j - i);
    slices_clear(s, s.len, oldlen);
    return s;
}

Slice slices_delete_func(Slice s, SlicesPredFunc del) {
    Int i = slices_index_func(s, del);
    if (i == -1)
        return s;
    size_t size = slices_size(s);
    /* Don't start copying elements until we find one to delete. */
    for (Int j = i + 1; j < s.len; j++) {
        if (!del.f(del.env, slices_ptr(s, j))) {
            slices_move(slices_ptr(s, i), slices_ptr(s, j), size);
            i++;
        }
    }
    slices_clear(s, i, s.len);
    s.len = i;
    return s;
}

Slice slices_replace(Alloc *a, Slice s, Int i, Int j, const void *v, Int m) {
    (void)slice_sub(s, i, j); /* bounds check */
    if (i == j)
        return slices_insert(a, s, i, v, m);
    if (m < 0)
        m = 0;
    size_t size = slices_size(s);
    if (j == s.len) {
        Slice s2 = slice_append(a, slice_sub(s, 0, i), v, m);
        if (s2.len < s.len)
            slices_clear(s, s2.len, s.len);
        return s2;
    }
    Int tot = i + m + (s.len - j);
    if (tot > s.cap) {
        /* Too big to fit, allocate and copy over, as slices_insert does. */
        Slice s2 = slice_append(a, slice_sub(s, 0, i), NULL, tot - i);
        if (s2.p == NULL)
            return s2;
        slices_move(slices_ptr(s2, i), v, (size_t)m * size);
        slices_move(slices_ptr(s2, i + m), slices_ptr(s, j),
                    (size_t)(s.len - j) * size);
        return s2;
    }
    Slice r = slice_sub(s, 0, tot);
    if (i + m <= j) {
        /* Easy, as v fits in the deleted portion. */
        slices_move(slices_ptr(r, i), v, (size_t)m * size);
        slices_move(slices_ptr(r, i + m), slices_ptr(s, j), (size_t)(s.len - j) * size);
        slices_clear(s, tot, s.len);
        return r;
    }
    /* We are expanding (v is bigger than j-i). With i=4, j=8, len(s)=16 and
     * len(v)=6:
     *
     * s: aaaaxxxxbbbbbbbbyy
     *        ^   ^       ^ ^
     *        i   j  len(s) tot
     *
     * a is a prefix of s, x the deleted range, b more of s and y the area to
     * expand into. */
    if (!slices_overlaps(slices_ptr(r, i + m), tot - (i + m), v, m, size)) {
        /* Easy, as v is not clobbered by the first copy. */
        slices_move(slices_ptr(r, i + m), slices_ptr(s, j), (size_t)(s.len - j) * size);
        slices_move(slices_ptr(r, i), v, (size_t)m * size);
        return r;
    }
    /* There is no single place to copy v to. Copy the prefix of v into y and
     * the suffix into x, then rotate |y| spots to the right. If either
     * destination does not alias v, that works. */
    Int y = m - (j - i);
    const Byte *vb = v;
    if (!slices_overlaps(slices_ptr(r, i), j - i, v, m, size)) {
        slices_move(slices_ptr(r, i), vb + (size_t)y * size, (size_t)(j - i) * size);
        slices_move(slices_ptr(r, s.len), vb, (size_t)y * size);
        slices_rotate_right(slices_ptr(r, i), size, tot - i, y);
        return r;
    }
    if (!slices_overlaps(slices_ptr(r, s.len), tot - s.len, v, m, size)) {
        slices_move(slices_ptr(r, s.len), vb, (size_t)y * size);
        slices_move(slices_ptr(r, i), vb + (size_t)y * size, (size_t)(j - i) * size);
        slices_rotate_right(slices_ptr(r, i), size, tot - i, y);
        return r;
    }
    /* v overlaps both x and y, so all of b is inside v. Copy v first, then
     * copy the b part of v out of v to where it goes. k is where s[j:]
     * starts inside v. */
    Int k = (Int)((size_t)(slices_ptr(s, j) - vb) / size);
    slices_move(slices_ptr(r, i), v, (size_t)m * size);
    slices_move(slices_ptr(r, i + m), slices_ptr(r, i + k), (size_t)(s.len - j) * size);
    return r;
}

/* ------------------------------------------------------------ Clone, Compact, Grow */

Slice slices_clone(Alloc *a, Slice s) {
    if (s.p == NULL)
        return slice_nil(s.elem);
    return slice_append(a, slice_make(a, s.elem, 0, 0), s.p, s.len);
}

Slice slices_compact(Slice s) {
    if (s.len < 2)
        return s;
    size_t size = slices_size(s);
    bool bytes = slices_bytewise(s.elem);
#define SLICES_SAME(i, j)                                                              \
    (bytes ? memcmp(slices_ptr(s, i), slices_ptr(s, j), size) == 0                     \
           : type_equal(s.elem, slices_ptr(s, i), slices_ptr(s, j)))
    for (Int k = 1; k < s.len; k++) {
        if (SLICES_SAME(k, k - 1)) {
            for (Int k2 = k + 1; k2 < s.len; k2++) {
                if (!SLICES_SAME(k2, k2 - 1)) {
                    slices_move(slices_ptr(s, k), slices_ptr(s, k2), size);
                    k++;
                }
            }
            slices_clear(s, k, s.len);
            s.len = k;
            return s;
        }
    }
#undef SLICES_SAME
    return s;
}

Slice slices_compact_func(Slice s, SlicesEqualFunc eq) {
    if (s.len < 2)
        return s;
    size_t size = slices_size(s);
    for (Int k = 1; k < s.len; k++) {
        if (eq.f(eq.env, slices_ptr(s, k), slices_ptr(s, k - 1))) {
            for (Int k2 = k + 1; k2 < s.len; k2++) {
                if (!eq.f(eq.env, slices_ptr(s, k2), slices_ptr(s, k2 - 1))) {
                    slices_move(slices_ptr(s, k), slices_ptr(s, k2), size);
                    k++;
                }
            }
            slices_clear(s, k, s.len);
            s.len = k;
            return s;
        }
    }
    return s;
}

Slice slices_grow(Alloc *a, Slice s, Int n) {
    if (n < 0)
        panic_str(BURROW_S("cannot be negative"));
    n -= s.cap - s.len;
    if (n > 0) {
        Int len = s.len;
        s = slice_append(a, slice_sub(s, 0, s.cap), NULL, n);
        if (s.p != NULL)
            s.len = len;
    }
    return s;
}

Slice slices_clip(Slice s) {
    return slice_sub3(s, 0, s.len, s.len);
}

/* ------------------------------------------------------------ Concat, Repeat */

Slice slices_concat(Alloc *a, const Slice *slices, Int n) {
    Int size = 0;
    for (Int k = 0; k < n; k++) {
        if (slices[k].len > BURROW_INT_MAX - size)
            panic_str(BURROW_S("len out of range"));
        size += slices[k].len;
    }
    const Type *elem = n > 0 ? slices[0].elem : NULL;
    Slice out = slices_grow(a, slice_nil(elem), size);
    for (Int k = 0; k < n; k++)
        out = slice_append(a, out, slices[k].p, slices[k].len);
    return out;
}

Slice slices_repeat(Alloc *a, Slice x, Int count) {
    if (count < 0)
        panic_str(BURROW_S("cannot be negative"));
    Uint lo;
    Uint hi = bits_mul((Uint)x.len, (Uint)count, &lo);
    if (hi > 0 || lo > (Uint)BURROW_INT_MAX)
        panic_str(BURROW_S("the result of (len(x) * count) overflows"));
    Int total = (Int)lo;
    Slice out = slice_make(a, x.elem, total, total);
    if (out.p == NULL)
        return out;
    size_t size = slices_size(x);
    Int k = x.len < total ? x.len : total;
    slices_move(out.p, x.p, (size_t)k * size);
    while (k < total) {
        Int c = k < total - k ? k : total - k;
        memcpy(slices_ptr(out, k), out.p, (size_t)c * size);
        k += c;
    }
    return out;
}
