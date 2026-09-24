/* Derived from Go's src/hash/adler32/adler32.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash/adler32.h"
#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

enum {
    /* adler_mod is the largest prime that is less than 65536. */
    adler_mod = 65521,
    /* adler_nmax is the largest n such that
     * 255 * n * (n+1) / 2 + (n+1) * (adler_mod-1) <= 2^32-1.
     * It is mentioned in RFC 1950 (search for "5552"). */
    adler_nmax = 5552,
};

/* The low 16 bits are s1, the high 16 bits are s2. */
typedef struct Adler32Digest {
    uint32_t d;
} Adler32Digest;

/* Add p to the running checksum d. */
static uint32_t adler32_update(uint32_t d, const Byte *p, Int n) {
    uint32_t s1 = d & 0xffff, s2 = d >> 16;
    while (n > 0) {
        Int m = n > adler_nmax ? adler_nmax : n;
        n -= m;
        for (; m >= 4; m -= 4) {
            s1 += p[0];
            s2 += s1;
            s1 += p[1];
            s2 += s1;
            s1 += p[2];
            s2 += s1;
            s1 += p[3];
            s2 += s1;
            p += 4;
        }
        for (; m > 0; m--) {
            s1 += *p++;
            s2 += s1;
        }
        s1 %= adler_mod;
        s2 %= adler_mod;
    }
    return s2 << 16 | s1;
}

static Int adler32_write(void *self, Slice p, Error *err) {
    Adler32Digest *d = self;
    d->d = adler32_update(d->d, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static Slice adler32_sum(void *self, Alloc *a, Slice in) {
    uint32_t s = ((Adler32Digest *)self)->d;
    Byte b[4] = {(Byte)(s >> 24), (Byte)(s >> 16), (Byte)(s >> 8), (Byte)s};
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    return slice_append(a, in, b, 4);
}

static void adler32_reset(void *self) {
    ((Adler32Digest *)self)->d = 1;
}

static Int adler32_size(void *self) {
    (void)self;
    return ADLER32_SIZE;
}

static Int adler32_block_size(void *self) {
    (void)self;
    return 4;
}

static uint32_t adler32_sum32(void *self) {
    return ((Adler32Digest *)self)->d;
}

static const HashHash32VT adler32_vt = {
    {{NULL, adler32_write},
     adler32_sum,
     adler32_reset,
     adler32_size,
     adler32_block_size},
    adler32_sum32,
};

HashHash32 adler32_new(Alloc *a) {
    Adler32Digest *d = BURROW_NEW(a, Adler32Digest);
    if (d == NULL)
        return (HashHash32){NULL, NULL};
    d->d = 1;
    HashHash32 h = {&adler32_vt, d};
    return h;
}

uint32_t adler32_checksum(Slice data) {
    return adler32_update(1, (const Byte *)data.p, data.len);
}
