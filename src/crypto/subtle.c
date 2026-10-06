/* crypto/subtle. See burrow/crypto/subtle.h.
 *
 * Go has the compiler on its side here: boolToUint8 is an intrinsic that
 * becomes a SETcc or a CSET and nothing else. A C compiler gives no such
 * promise about x == y, and will turn a mask back into a branch whenever it
 * can prove the two do the same thing. So the comparisons are arithmetic on
 * the bits, and every mask goes through ct_hide, an empty asm statement that
 * says the value could have become anything, so that the optimiser cannot
 * reason past it. MSVC has no such statement and gets its optimiser turned
 * off for this file instead, which is slower and is what it takes.
 *
 * Derived from Go's src/crypto/subtle, src/crypto/internal/constanttime,
 * src/crypto/internal/fips140/subtle and src/crypto/internal/fips140/alias.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/subtle.h"

#include "burrow/defer.h"
#include "burrow/dit.h"
#include "burrow/panic.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma optimize("", off)
#endif

static uint64_t ct_hide(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(x));
#endif
    return x;
}

/* 1 if x is zero and 0 if it is not. x | -x has its top bit set exactly when
 * x is not zero. */
static uint64_t ct_is_zero(uint64_t x) {
    x = ct_hide(x);
    return 1 ^ ((x | (0 - x)) >> 63);
}

/* 1 if a < b as signed numbers and 0 if not. The top bit of a - b is the
 * answer unless the subtraction overflowed, which can only happen when a and b
 * have different signs and the result has a sign different from a's. */
static uint64_t ct_less(int64_t a, int64_t b) {
    uint64_t ua = ct_hide((uint64_t)a);
    uint64_t ub = ct_hide((uint64_t)b);
    uint64_t d = ua - ub;
    return (d ^ ((ua ^ ub) & (d ^ ua))) >> 63;
}

Int subtle_constant_time_byte_eq(uint8_t x, uint8_t y) {
    return (Int)ct_is_zero((uint64_t)(x ^ y));
}

Int subtle_constant_time_eq(int32_t x, int32_t y) {
    return (Int)ct_is_zero((uint64_t)((uint32_t)x ^ (uint32_t)y));
}

Int subtle_constant_time_select(Int v, Int x, Int y) {
    Uint mask = (Uint)ct_hide(0 - (1 ^ ct_is_zero((uint64_t)(Uint)v)));
    return (Int)(((Uint)x & mask) | ((Uint)y & ~mask));
}

Int subtle_constant_time_less_or_eq(Int x, Int y) {
    return (Int)(1 ^ ct_less((int64_t)y, (int64_t)x));
}

Int subtle_constant_time_compare(Slice x, Slice y) {
    if (x.len != y.len)
        return 0;
    const Byte *p = (const Byte *)x.p;
    const Byte *q = (const Byte *)y.p;
    Byte v = 0;
    for (Int i = 0; i < x.len; i++)
        v |= (Byte)(p[i] ^ q[i]);
    return subtle_constant_time_byte_eq(v, 0);
}

void subtle_constant_time_copy(Int v, Slice x, Slice y) {
    if (x.len != y.len)
        panic_str(BURROW_S("subtle: slices have different lengths"));
    Byte xmask = (Byte)ct_hide((uint64_t)(Uint)(v - 1));
    Byte ymask = (Byte)~xmask;
    Byte *p = (Byte *)x.p;
    const Byte *q = (const Byte *)y.p;
    for (Int i = 0; i < x.len; i++)
        p[i] = (Byte)((p[i] & xmask) | (q[i] & ymask));
}

/* Go's alias.InexactOverlap on two n byte runs: they share memory without
 * starting at the same place. */
static bool inexact_overlap(const void *a, const void *b, Int n) {
    uintptr_t x = (uintptr_t)a;
    uintptr_t y = (uintptr_t)b;
    if (x == y)
        return false;
    return x <= y + (uintptr_t)(n - 1) && y <= x + (uintptr_t)(n - 1);
}

Int subtle_xor_bytes(Slice dst, Slice x, Slice y) {
    Int n = x.len < y.len ? x.len : y.len;
    if (n == 0)
        return 0;
    if (n > dst.len)
        panic_str(BURROW_S("subtle.XORBytes: dst too short"));
    if (inexact_overlap(dst.p, x.p, n) || inexact_overlap(dst.p, y.p, n))
        panic_str(BURROW_S("subtle.XORBytes: invalid overlap"));
    Byte *d = (Byte *)dst.p;
    const Byte *a = (const Byte *)x.p;
    const Byte *b = (const Byte *)y.p;
    /* A word at a time through memcpy, which is a plain load on everything
     * that allows one unaligned and a safe one everywhere else. Each word is
     * read in full before it is written, so dst being x or y is fine. */
    Int i = 0;
    for (; n - i >= 8; i += 8) {
        uint64_t u, w;
        memcpy(&u, a + i, 8);
        memcpy(&w, b + i, 8);
        u ^= w;
        memcpy(d + i, &u, 8);
    }
    for (; i < n; i++)
        d[i] = (Byte)(a[i] ^ b[i]);
    return n;
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma optimize("", on)
#endif

static void dit_restore(void *env) {
    if (!*(const bool *)env)
        burrow__dit_set_disabled();
}

void subtle_with_data_independent_timing(Func f) {
    if (!burrow__dit_supported()) {
        f.f(f.env);
        return;
    }
    bool already = burrow__dit_set_enabled();
    BURROW_SCOPE {
        BURROW_DEFER(dit_restore, &already);
        f.f(f.env);
    }
    BURROW_SCOPE_END;
}
