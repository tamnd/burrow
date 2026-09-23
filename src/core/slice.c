/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/slice.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/type.h"

#include <string.h>

/* Slices, which are one struct and six operations and still the part of this
 * library most likely to be got subtly wrong, because append has behaviour that
 * people rely on without being able to state it.
 *
 * The growth arithmetic below is a faithful port of Go's nextslicecap. That is
 * deliberate and it is not cosmetic: the capacity progression is visible
 * through cap(), Go's own tests assert on it, and a program tuned against Go's
 * progression that gets a different one here would allocate a different number
 * of times and nobody would understand why. */

/* What Go calls zerobase. Every zero sized allocation in Go returns the same
 * address, which is what makes []struct{} work and what makes two pointers to
 * distinct zero sized objects compare equal. Nothing ever writes through this,
 * since every copy into it is a copy of zero bytes. */
static Byte zerobase[1];

static size_t elem_size(const Type *t) {
    return t == NULL ? 0 : (size_t)t->size;
}

static size_t elem_align(const Type *t) {
    size_t a = t == NULL ? 0 : (size_t)t->align;
    return a == 0 ? 1 : a;
}

/* n elements of size sz, in bytes, saying no rather than wrapping.
 *
 * An element count that an attacker chose and that overflows on the way to a
 * byte count is the oldest heap overflow there is, and it is still shipping. */
static bool bytes_for(Int n, size_t sz, size_t *out) {
    if (n < 0)
        return false;
    if (sz == 0) {
        *out = 0;
        return true;
    }
    if ((uint64_t)n > (uint64_t)(SIZE_MAX / sz))
        return false;
    *out = (size_t)n * sz;
    return true;
}

/* memcpy and memmove with a NULL pointer are undefined even when the length is
 * zero, UndefinedBehaviorSanitizer reports it, and a zero length slice with a
 * NULL pointer is the most ordinary value in this library. */
static void slice_copy_bytes(void *dst, const void *src, size_t n) {
    if (n > 0 && dst != NULL && src != NULL)
        memcpy(dst, src, n);
}

static void move_bytes(void *dst, const void *src, size_t n) {
    if (n > 0 && dst != NULL && src != NULL)
        memmove(dst, src, n);
}

static void zero_bytes(void *dst, size_t n) {
    if (n > 0 && dst != NULL)
        memset(dst, 0, n);
}

/* ------------------------------------------------------------------- make */

Slice slice_nil(const Type *elem) {
    Slice s = {NULL, 0, 0, elem};
    return s;
}

bool slice_is_nil(Slice s) {
    return s.p == NULL;
}

Slice slice_from(void *p, Int len, Int cap, const Type *elem) {
    if (len < 0 || cap < len)
        runtime_panic(BURROW_S("runtime error: slice_from: len out of range"));
    Slice s = {p, len, cap, elem};
    return s;
}

Slice slice_make(Alloc *a, const Type *elem, Int len, Int cap) {
    size_t sz = elem_size(elem);
    size_t total = 0;

    /* Go's order, which matters because the two messages are different and
     * people do search for them. makeslice reports the length first when the
     * length itself is impossible, and the capacity otherwise. */
    if (len < 0)
        runtime_panic(BURROW_S("runtime error: makeslice: len out of range"));
    if (cap < len || !bytes_for(cap, sz, &total))
        runtime_panic(BURROW_S("runtime error: makeslice: cap out of range"));

    if (total == 0) {
        /* Either a zero sized element or a zero capacity. Both are real slices
         * rather than nil ones, since make([]T, 0) is not nil in Go and the
         * difference shows up the moment somebody encodes it as JSON. */
        Slice s = {zerobase, len, cap, elem};
        return s;
    }

    void *p = mem_alloc(a, total, elem_align(elem));
    if (p == NULL)
        return slice_nil(elem);

    Slice s = {p, len, cap, elem};
    return s;
}

/* ------------------------------------------------------------- index, slice */

void *slice_at(Slice s, Int i) {
    /* Against len and not cap, which is what s[i] checks in Go. Reaching past
     * the length is what reslicing is for and it has its own check below.
     *
     * Both ends are compared rather than folding them into one unsigned
     * compare. The unsigned trick is only sound when len cannot be negative,
     * and a Slice is a struct that anybody can fill in by hand, so here it can
     * be. Two predictable branches is not a cost worth arguing about. */
    if (i < 0 || i >= s.len)
        runtime_index_out_of_range(i, s.len);
    return (Byte *)s.p + (size_t)i * elem_size(s.elem);
}

Slice slice_sub3(Slice s, Int lo, Int hi, Int max) {
    if (lo < 0 || hi < lo || max < hi || max > s.cap)
        runtime_slice_bounds_out_of_range(lo, hi, s.cap);

    Byte *p = (Byte *)s.p;
    if (p != NULL)
        p += (size_t)lo * elem_size(s.elem);

    Slice out = {p, hi - lo, max - lo, s.elem};
    return out;
}

Slice slice_sub(Slice s, Int lo, Int hi) {
    return slice_sub3(s, lo, hi, s.cap);
}

/* ----------------------------------------------------------------- append */

/* Go's nextslicecap, ported.
 *
 * Double until 256 elements, then approach 1.25x through a formula that makes
 * the transition smooth rather than a step. Go arrived at this after measuring,
 * the constant 256 is in Go's source as `threshold`, and the whole point of
 * copying it exactly is that the resulting cap() values match.
 *
 * The arithmetic runs unsigned. In Go the loop is allowed to overflow an int
 * and the overflow is then detected, because Go defines signed overflow to
 * wrap. C does not define it at all, so the same computation is done in Uint
 * and the range check happens at the end instead. */
static Int next_cap(Int new_len, Int old_cap) {
    const Int threshold = 256;

    /* Doubling the old capacity overflows an Int here. Go lets that wrap into a
     * negative number which then loses the comparison on the next line, and
     * reaches the same answer by accident. C promises nothing about the wrap,
     * so the case comes out first and gets the answer on purpose. */
    if (old_cap > BURROW_INT_MAX / 2)
        return new_len;

    Int doublecap = old_cap + old_cap;
    if (new_len > doublecap)
        return new_len;
    if (old_cap < threshold)
        return doublecap;

    Uint nc = (Uint)old_cap;
    while (nc < (Uint)new_len) {
        /* Divided rather than shifted, which is how Go writes the same line.
         * nc is unsigned, so the compiler emits the shift either way, and a
         * shift by a signed literal is the kind of thing a checker asks about
         * for good reasons that do not apply here. */
        nc += (nc + 3 * (Uint)threshold) / 4;
        /* Go checks for the overflow after the fact by comparing as unsigned.
         * Here the value simply keeps growing, so the exit is the same
         * condition and the impossible case is caught below. */
        if (nc > (Uint)BURROW_INT_MAX)
            return new_len;
    }
    return (Int)nc;
}

Slice slice_append(Alloc *a, Slice s, const void *elems, Int n) {
    if (n <= 0)
        return s;

    if (s.len > BURROW_INT_MAX - n)
        runtime_panic(BURROW_S("runtime error: growslice: len out of range"));

    Int new_len = s.len + n;
    size_t sz = elem_size(s.elem);

    /* Zero sized elements never move and never allocate, exactly as in Go,
     * where growslice returns zerobase for them without touching the heap. */
    if (sz == 0) {
        Slice out = {zerobase, new_len, new_len > s.cap ? new_len : s.cap, s.elem};
        return out;
    }

    /* The in place case, which is the one with the semantics people rely on.
     * The elements go into the backing array s already points at, so every
     * other slice over that array sees them. Go does this and Go's tests
     * depend on it, so quietly copying instead would be a nicer library that
     * ports Go code incorrectly. */
    if (new_len <= s.cap) {
        slice_copy_bytes((Byte *)s.p + (size_t)s.len * sz, elems, (size_t)n * sz);
        Slice out = {s.p, new_len, s.cap, s.elem};
        return out;
    }

    Int new_cap = next_cap(new_len, s.cap);
    size_t total = 0;
    if (new_cap < new_len || !bytes_for(new_cap, sz, &total))
        runtime_panic(BURROW_S("runtime error: growslice: cap out of range"));

    /* Not zeroed, because all three regions are written below: the old
     * elements, the new ones, and the tail out to the capacity. Go clears that
     * tail too, and it has to, since the next append into the spare capacity
     * reads it as a zero value if the caller reslices up to it first. */
    Byte *p = (Byte *)mem_alloc_nozero(a, total, elem_align(s.elem));
    if (p == NULL)
        return slice_nil(s.elem);

    size_t old_bytes = (size_t)s.len * sz;
    size_t new_bytes = (size_t)n * sz;

    /* The copy happens after the allocation, which is what makes
     * append(s, s...) work: elems may point into the old array and the old
     * array is still there. */
    slice_copy_bytes(p, s.p, old_bytes);
    slice_copy_bytes(p + old_bytes, elems, new_bytes);
    zero_bytes(p + old_bytes + new_bytes, total - old_bytes - new_bytes);

    Slice out = {p, new_len, new_cap, s.elem};
    return out;
}

Slice slice_append_slice(Alloc *a, Slice dst, Slice src) {
    if (src.len <= 0)
        return dst;
    if (elem_size(dst.elem) != elem_size(src.elem))
        runtime_throw(BURROW_S("runtime error: append: element sizes differ"));
    return slice_append(a, dst, src.p, src.len);
}

/* ------------------------------------------------------------------- copy */

Int slice_copy(Slice dst, Slice src) {
    size_t sz = elem_size(dst.elem);

    /* Sizes rather than descriptor identity. Two descriptors for the same
     * layout can legitimately be distinct objects, since nothing stops a caller
     * declaring their own uint8 type, and a copy between those is meaningful.
     * A copy between different sizes is not, and it is a bug in the caller
     * rather than a condition to return zero for. */
    if (sz != elem_size(src.elem))
        runtime_throw(BURROW_S("runtime error: copy: element sizes differ"));

    Int n = dst.len < src.len ? dst.len : src.len;
    if (n <= 0)
        return 0;

    /* memmove, because Go's copy is explicitly defined for overlapping slices
     * and copy(s, s[1:]) is how you delete an element. */
    move_bytes(dst.p, src.p, (size_t)n * sz);
    return n;
}

Int slice_copy_str(Slice dst, Str src) {
    if (elem_size(dst.elem) != 1)
        runtime_throw(BURROW_S("runtime error: copy: destination is not a byte slice"));

    Int n = dst.len < src.len ? dst.len : src.len;
    if (n <= 0)
        return 0;
    move_bytes(dst.p, src.p, (size_t)n);
    return n;
}

/* ------------------------------------------------------------ conversions */

Slice slice_from_str(Alloc *a, Str s) {
    if (s.len <= 0)
        return slice_make(a, TYPE_BYTE, 0, 0);

    Slice out = slice_make(a, TYPE_BYTE, s.len, s.len);
    if (out.p == NULL)
        return out;
    slice_copy_bytes(out.p, s.p, (size_t)s.len);
    return out;
}

Str str_from_slice(Alloc *a, Slice s) {
    if (elem_size(s.elem) != 1)
        runtime_throw(BURROW_S("runtime error: string: source is not a byte slice"));
    return str_clone(a, str_from_bytes(s.p, s.len));
}
