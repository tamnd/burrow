/* Derived from Go's src/internal/bytealg/bytealg.go and the generic versions
 * of count, index and lastindexbyte next to it.
 * Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "bytealg.h"

#include "burrow/core.h"

#include <stdint.h>
#include <string.h>

/* The multiplier for the Rabin-Karp rolling hash. */
#define BYTEALG_PRIME_RK 16777619U

Int burrow__bytealg_last_index_byte(Str s, Byte c) {
    for (Int i = s.len - 1; i >= 0; i--) {
        if (s.p[i] == c)
            return i;
    }
    return -1;
}

Int burrow__bytealg_count(Str s, Byte c) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        n += s.p[i] == c;
    return n;
}

#define BYTEALG_ONES 0x0101010101010101ULL
#define BYTEALG_HIGHS 0x8080808080808080ULL

static uint64_t bytealg_load8(const Byte *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Go does this in assembly per architecture, sixteen or thirty two bytes at a
 * time. This is the portable version of the same idea: take eight candidate
 * positions at once, and keep only those where the first, the second and the
 * last byte of sep all match. Three bytes matter on text like HTML, where the
 * first and last byte of a tag match at nearly every tag. The zero byte test
 * is nonzero exactly when some byte of the word is zero, though it can flag
 * the wrong byte, so a flagged word is checked a byte at a time. That also
 * keeps it independent of byte order. */
Int burrow__bytealg_index_string(Str s, Str sep) {
    Int n = sep.len;
    Byte first = sep.p[0];
    Byte second = sep.p[1];
    Byte last = sep.p[n - 1];
    Int end = s.len - n; /* the last place a match can start */
    Int i = 0;
    uint64_t wf = BYTEALG_ONES * first;
    uint64_t ws = BYTEALG_ONES * second;
    uint64_t wl = BYTEALG_ONES * last;
    if (n == 2) {
        /* The second byte is the last one, and two loads cover it. */
        for (; i + 7 <= end; i += 8) {
            uint64_t x =
                (bytealg_load8(s.p + i) ^ wf) | (bytealg_load8(s.p + i + 1) ^ ws);
            if (((x - BYTEALG_ONES) & ~x & BYTEALG_HIGHS) == 0)
                continue;
            for (Int k = i; k < i + 8; k++) {
                if (s.p[k] == first && s.p[k + 1] == second)
                    return k;
            }
        }
    }
    for (; i + 7 <= end; i += 8) {
        uint64_t x = (bytealg_load8(s.p + i) ^ wf) | (bytealg_load8(s.p + i + 1) ^ ws) |
                     (bytealg_load8(s.p + i + n - 1) ^ wl);
        if (((x - BYTEALG_ONES) & ~x & BYTEALG_HIGHS) == 0)
            continue;
        for (Int k = i; k < i + 8; k++) {
            if (s.p[k] == first && s.p[k + 1] == second && s.p[k + n - 1] == last &&
                memcmp(s.p + k + 2, sep.p + 2, (size_t)(n - 2)) == 0)
                return k;
        }
    }
    for (; i <= end; i++) {
        if (s.p[i] == first && s.p[i + n - 1] == last &&
            memcmp(s.p + i + 1, sep.p + 1, (size_t)(n - 1)) == 0)
            return i;
    }
    return -1;
}

#undef BYTEALG_ONES
#undef BYTEALG_HIGHS

/* The hash of sep and the multiplier that takes a byte out of the window. */
static uint32_t bytealg_pow(Int n) {
    uint32_t pow = 1;
    uint32_t sq = BYTEALG_PRIME_RK;
    for (Int i = n; i > 0; i >>= 1) {
        if (i & 1)
            pow *= sq;
        sq *= sq;
    }
    return pow;
}

Int burrow__bytealg_index_rabin_karp(Str s, Str sep) {
    uint32_t hashss = 0;
    for (Int i = 0; i < sep.len; i++)
        hashss = hashss * BYTEALG_PRIME_RK + sep.p[i];
    uint32_t pow = bytealg_pow(sep.len);
    Int n = sep.len;
    uint32_t h = 0;
    for (Int i = 0; i < n; i++)
        h = h * BYTEALG_PRIME_RK + s.p[i];
    if (h == hashss && memcmp(s.p, sep.p, (size_t)n) == 0)
        return 0;
    for (Int i = n; i < s.len;) {
        h *= BYTEALG_PRIME_RK;
        h += s.p[i];
        h -= pow * s.p[i - n];
        i++;
        if (h == hashss && memcmp(s.p + i - n, sep.p, (size_t)n) == 0)
            return i - n;
    }
    return -1;
}

Int burrow__bytealg_last_index_rabin_karp(Str s, Str sep) {
    uint32_t hashss = 0;
    for (Int i = sep.len - 1; i >= 0; i--)
        hashss = hashss * BYTEALG_PRIME_RK + sep.p[i];
    uint32_t pow = bytealg_pow(sep.len);
    Int n = sep.len;
    Int last = s.len - n;
    uint32_t h = 0;
    for (Int i = s.len - 1; i >= last; i--)
        h = h * BYTEALG_PRIME_RK + s.p[i];
    if (h == hashss && memcmp(s.p + last, sep.p, (size_t)n) == 0)
        return last;
    for (Int i = last - 1; i >= 0; i--) {
        h *= BYTEALG_PRIME_RK;
        h += s.p[i];
        h -= pow * s.p[i + n];
        if (h == hashss && memcmp(s.p + i, sep.p, (size_t)n) == 0)
            return i;
    }
    return -1;
}
