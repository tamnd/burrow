/* Derived from Go's src/hash/adler32/adler32.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash/adler32.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "internal.h"

#include <string.h>

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
uint32_t burrow__adler32_update(uint32_t d, const Byte *p, Int n) {
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
    d->d = burrow__adler32_update(d->d, (const Byte *)p.p, p.len);
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

#define ADLER32_MAGIC "adl\x01"
enum { adler32_marshaled_size = 4 + 4 };

BURROW_SENTINEL_ERROR(adler32_err_identifier,
                      "hash/adler32: invalid hash state identifier");
BURROW_SENTINEL_ERROR(adler32_err_size, "hash/adler32: invalid hash state size");

static Slice adler32_append_binary(Adler32Digest *d, Alloc *a, Slice b, Error *err) {
    uint32_t s = d->d;
    Byte st[adler32_marshaled_size] = {
        'a', 'd', 'l', 1, (Byte)(s >> 24), (Byte)(s >> 16), (Byte)(s >> 8), (Byte)s};
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Int len = b.len;
    Slice r = slice_append(a, b, st, adler32_marshaled_size);
    BURROW_OUT(err, r.len == len + adler32_marshaled_size ? BURROW_NO_ERROR
                                                          : burrow_err_out_of_memory);
    return r;
}

static Slice adler32_marshal_binary(Adler32Digest *d, Alloc *a, Error *err) {
    Slice b = slice_make(a, TYPE_BYTE, 0, adler32_marshaled_size);
    if (b.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return adler32_append_binary(d, a, b, err);
}

static Error adler32_unmarshal_binary(Adler32Digest *d, Alloc *a, Slice b) {
    (void)a;
    const Byte *p = b.p;
    if (b.len < 4 || memcmp(p, ADLER32_MAGIC, 4) != 0)
        return adler32_err_identifier;
    if (b.len != adler32_marshaled_size)
        return adler32_err_size;
    d->d = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 |
           (uint32_t)p[7];
    return BURROW_NO_ERROR;
}

static HashCloner adler32_clone(void *self, Alloc *a, Error *err);

/* The method set of Go's *adler32.digest, so that a Hash from here answers
 * h.(hash.Cloner) and h.(encoding.BinaryMarshaler) as Go's does. */
#define ADLER32_METHODS(M, T)                                                          \
    M(T, AppendBinary, adler32_append_binary, ENCODING_SIG_APPEND_BINARY)              \
    M(T, BlockSize, adler32_block_size, HASH_SIG_BLOCK_SIZE)                           \
    M(T, Clone, adler32_clone, HASH_SIG_CLONE)                                         \
    M(T, MarshalBinary, adler32_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)           \
    M(T, Reset, adler32_reset, HASH_SIG_RESET)                                         \
    M(T, Size, adler32_size, HASH_SIG_SIZE)                                            \
    M(T, Sum, adler32_sum, HASH_SIG_SUM)                                               \
    M(T, Sum32, adler32_sum32, HASH_SIG_SUM32)                                         \
    M(T, UnmarshalBinary, adler32_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)     \
    M(T, Write, adler32_write, IO_SIG_WRITE)

BURROW_METHODS_DEFINE(Adler32Digest, ADLER32_METHODS);

/* Go's digest is a uint32, which is what Adler32Digest holds. */
static const Type adler32_digest_type = {
    {(const Byte *)"digest", 6},
    {(const Byte *)"hash/adler32", 12},
    KIND_UINT32,
    (uint32_t)sizeof(Adler32Digest),
    (uint16_t)_Alignof(Adler32Digest),
    0,
    (uint16_t)(sizeof burrow__methods_Adler32Digest /
               sizeof burrow__methods_Adler32Digest[0]),
    NULL,
    burrow__methods_Adler32Digest,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const HashHash32VT adler32_vt = {
    {{&adler32_digest_type, adler32_write},
     adler32_sum,
     adler32_reset,
     adler32_size,
     adler32_block_size},
    adler32_sum32,
};

/* The same digest, seen as the hash.Cloner Clone returns. */
static const HashClonerVT adler32_cloner_vt = {
    {{&adler32_digest_type, adler32_write},
     adler32_sum,
     adler32_reset,
     adler32_size,
     adler32_block_size},
    adler32_clone,
};

static HashCloner adler32_clone(void *self, Alloc *a, Error *err) {
    Adler32Digest *r = BURROW_NEW(a, Adler32Digest);
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (HashCloner){NULL, NULL};
    }
    *r = *(Adler32Digest *)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    HashCloner h = {&adler32_cloner_vt, r};
    return h;
}

HashHash32 adler32_new(Alloc *a) {
    Adler32Digest *d = BURROW_NEW(a, Adler32Digest);
    if (d == NULL)
        return (HashHash32){NULL, NULL};
    d->d = 1;
    HashHash32 h = {&adler32_vt, d};
    return h;
}

uint32_t adler32_checksum(Slice data) {
    return burrow__adler32_update(1, (const Byte *)data.p, data.len);
}
