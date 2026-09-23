/* Derived from Go's src/internal/strconv/itoa.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strconv.h"

#include "burrow/core.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* Every form formats into a buffer on the stack first and copies from there,
 * so a Str result is allocated at exactly its length. Go returns a constant for
 * 0 to 99 without allocating. That would make some results static and some
 * owned, so here they are all owned, and the arena makes the difference small. */

static const char itoa_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* 00 to 99 run together, two bytes per number. */
static const char itoa_smalls[] = "00010203040506070809"
                                  "10111213141516171819"
                                  "20212223242526272829"
                                  "30313233343536373839"
                                  "40414243444546474849"
                                  "50515253545556575859"
                                  "60616263646566676869"
                                  "70717273747576777879"
                                  "80818283848586878889"
                                  "90919293949596979899";

static const uint64_t itoa_pow10[20] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
    10000000000000000000ULL,
};

/* The number of decimal digits in d, for d at least 1. floor(log10(2^len)) is
 * either the answer or one short of it, and one comparison says which. */
static Int itoa_num_digits(uint64_t d) {
    Int nd = (bits_len64(d) * 78913) >> 18;
    return nd + (d >= itoa_pow10[nd]);
}

static void itoa_pair(Byte *a, Int at, uint32_t x2) {
    a[at] = (Byte)itoa_smalls[x2];
    a[at + 1] = (Byte)itoa_smalls[x2 + 1];
}

/* The decimal digits of u into a[0:nd], two at a time from the right, with the
 * 64 bit divisions only where the value does not fit in 32 bits. */
void burrow__strconv_format_base10(Byte *a, Int nd, uint64_t u) {
    while (nd >= 8) {
        uint32_t x3210 = (uint32_t)(u % 100000000);
        u /= 100000000;
        uint32_t x32 = x3210 / 10000, x10 = x3210 % 10000;
        itoa_pair(a, nd - 2, (x10 % 100) * 2);
        itoa_pair(a, nd - 4, (x10 / 100) * 2);
        itoa_pair(a, nd - 6, (x32 % 100) * 2);
        itoa_pair(a, nd - 8, (x32 / 100) * 2);
        nd -= 8;
    }

    uint32_t x = (uint32_t)u;
    if (nd >= 4) {
        uint32_t x10 = x % 10000;
        x /= 10000;
        itoa_pair(a, nd - 2, (x10 % 100) * 2);
        itoa_pair(a, nd - 4, (x10 / 100) * 2);
        nd -= 4;
    }
    if (nd >= 2) {
        itoa_pair(a, nd - 2, (x % 100) * 2);
        x /= 100;
        nd -= 2;
    }
    if (nd > 0)
        a[0] = (Byte)('0' + x);
}

/* Room for 64 binary digits and a sign. */
#define ITOA_BUF 65

/* u in base into the tail of buf, with a minus sign if neg, in which case u is
 * the two's complement bits of a negative value. Returns where it starts. */
static Int itoa_format_bits(Byte buf[ITOA_BUF], uint64_t u, Int base, bool neg) {
    if (base < 2 || base > 36)
        panic_str(BURROW_S("strconv: illegal AppendInt/FormatInt base"));

    if (neg)
        u = 0 - u;

    Int i = ITOA_BUF;
    if (base == 10) {
        Int nd = itoa_num_digits(u | 1);
        i -= nd;
        burrow__strconv_format_base10(buf + i, nd, u);
    } else if ((base & (base - 1)) == 0) {
        /* Shifts and masks for the powers of two. */
        unsigned shift = (unsigned)bits_trailing_zeros64((uint64_t)base);
        uint64_t b = (uint64_t)base;
        uint64_t m = b - 1;
        while (u >= b) {
            buf[--i] = (Byte)itoa_digits[u & m];
            u >>= shift;
        }
        buf[--i] = (Byte)itoa_digits[u];
    } else {
        uint64_t b = (uint64_t)base;
        while (u >= b) {
            uint64_t q = u / b;
            buf[--i] = (Byte)itoa_digits[u - q * b];
            u = q;
        }
        buf[--i] = (Byte)itoa_digits[u];
    }

    if (neg)
        buf[--i] = '-';
    return i;
}

static Str itoa_own(Alloc *a, const Byte *p, Int n) {
    Byte *out = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (out == NULL)
        return BURROW_STR_EMPTY;
    memcpy(out, p, (size_t)n);
    return str_from_bytes(out, n);
}

static Slice itoa_append(Alloc *a, Slice dst, const Byte *p, Int n) {
    if (dst.elem == NULL)
        dst = slice_nil(TYPE_BYTE);
    /* The usual case, with room to spare, without the call. */
    if (n <= dst.cap - dst.len) {
        memcpy((Byte *)dst.p + dst.len, p, (size_t)n);
        dst.len += n;
        return dst;
    }
    return slice_append(a, dst, p, n);
}

Str strconv_format_uint(Alloc *a, uint64_t i, Int base) {
    Byte buf[ITOA_BUF];
    Int at = itoa_format_bits(buf, i, base, false);
    return itoa_own(a, buf + at, ITOA_BUF - at);
}

Str strconv_format_int(Alloc *a, int64_t i, Int base) {
    Byte buf[ITOA_BUF];
    Int at = itoa_format_bits(buf, (uint64_t)i, base, i < 0);
    return itoa_own(a, buf + at, ITOA_BUF - at);
}

Str strconv_itoa(Alloc *a, Int i) {
    return strconv_format_int(a, (int64_t)i, 10);
}

/* 0 to 99 in base 10 straight from the table, which is Go's fast path too. */
static bool itoa_small(uint64_t u, Int base) {
    return u < 100 && base == 10;
}

static Slice itoa_append_small(Alloc *a, Slice dst, uint64_t u) {
    if (u < 10)
        return itoa_append(a, dst, (const Byte *)itoa_smalls + u * 2 + 1, 1);
    return itoa_append(a, dst, (const Byte *)itoa_smalls + u * 2, 2);
}

Slice strconv_append_int(Alloc *a, Slice dst, int64_t i, Int base) {
    if (i >= 0 && itoa_small((uint64_t)i, base))
        return itoa_append_small(a, dst, (uint64_t)i);
    Byte buf[ITOA_BUF];
    Int at = itoa_format_bits(buf, (uint64_t)i, base, i < 0);
    return itoa_append(a, dst, buf + at, ITOA_BUF - at);
}

Slice strconv_append_uint(Alloc *a, Slice dst, uint64_t i, Int base) {
    if (itoa_small(i, base))
        return itoa_append_small(a, dst, i);
    Byte buf[ITOA_BUF];
    Int at = itoa_format_bits(buf, i, base, false);
    return itoa_append(a, dst, buf + at, ITOA_BUF - at);
}
