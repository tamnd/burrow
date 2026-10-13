/* Derived from Go's src/crypto/mlkem/mlkem.go and
 * src/crypto/internal/fips140/mlkem/field.go, mlkem768.go and mlkem1024.go.
 * Go source: go1.27.1.
 *
 * Go writes ML-KEM-768 by hand and generates ML-KEM-1024 from it, changing k
 * and the compression of the ciphertext. This writes the algorithms once, over
 * a view of a key that says what k is, and the two key types are only their
 * own sizes of array.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/mlkem.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "mlkem_internal.h"
#include "rand_internal.h"
#include "sha3_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ field */

enum {
    MLKEM_BARRETT_MULTIPLIER = 5039, /* 2¹² * 2¹² / q */
    MLKEM_BARRETT_SHIFT = 24,        /* log₂(2¹² * 2¹²) */
};

/* Reduces a value a < 2q. */
static inline MlkemFieldElement mlkem_field_reduce_once(uint16_t a) {
    uint16_t x = (uint16_t)(a - MLKEM_Q);
    /* If x underflowed, then x >= 2¹⁶ - q > 2¹⁵, so the top bit is set. */
    x = (uint16_t)(x + (x >> 15) * MLKEM_Q);
    return x;
}

MlkemFieldElement burrow__mlkem_field_add(MlkemFieldElement a, MlkemFieldElement b) {
    return mlkem_field_reduce_once((uint16_t)(a + b));
}

MlkemFieldElement burrow__mlkem_field_sub(MlkemFieldElement a, MlkemFieldElement b) {
    return mlkem_field_reduce_once((uint16_t)(a - b + MLKEM_Q));
}

/* Barrett reduction, to keep away from a division that might not be constant
 * time. */
MlkemFieldElement burrow__mlkem_field_reduce(uint32_t a) {
    uint32_t quotient =
        (uint32_t)(((uint64_t)a * MLKEM_BARRETT_MULTIPLIER) >> MLKEM_BARRETT_SHIFT);
    return mlkem_field_reduce_once((uint16_t)(a - quotient * MLKEM_Q));
}

MlkemFieldElement burrow__mlkem_field_mul(MlkemFieldElement a, MlkemFieldElement b) {
    return burrow__mlkem_field_reduce((uint32_t)a * (uint32_t)b);
}

/* a * (b - c), with the reduction of the subtraction left to the one of the
 * product. */
static inline MlkemFieldElement
mlkem_field_mul_sub(MlkemFieldElement a, MlkemFieldElement b, MlkemFieldElement c) {
    return burrow__mlkem_field_reduce((uint32_t)a * (uint32_t)(b - c + MLKEM_Q));
}

/* a * b + c * d, with one reduction. */
static inline MlkemFieldElement mlkem_field_add_mul(MlkemFieldElement a,
                                                    MlkemFieldElement b,
                                                    MlkemFieldElement c,
                                                    MlkemFieldElement d) {
    uint32_t x = (uint32_t)a * (uint32_t)b;
    x += (uint32_t)c * (uint32_t)d;
    return burrow__mlkem_field_reduce(x);
}

/* Maps x uniformly to 0 to 2ᵈ-1, which is (x * 2ᵈ) / q rounded to the nearest
 * integer, a half rounding up, as FIPS 203, Section 2.3, has it. */
uint16_t burrow__mlkem_compress(MlkemFieldElement x, uint8_t d) {
    /* Barrett reduction gives a quotient and a remainder in [0, 2q), with
     * dividend = quotient * q + remainder. */
    uint32_t dividend = (uint32_t)x << d;
    uint32_t quotient = (uint32_t)(((uint64_t)dividend * MLKEM_BARRETT_MULTIPLIER) >>
                                   MLKEM_BARRETT_SHIFT);
    uint32_t remainder = dividend - quotient * MLKEM_Q;

    /* [0, q/2) rounds to 0, [q/2, q + q/2) to 1 and [q + q/2, 2q) to 2. When
     * remainder is more than the bound the subtraction underflows and sets the
     * top bit. */
    quotient += ((uint32_t)(MLKEM_Q / 2) - remainder) >> 31 & 1;
    quotient += ((uint32_t)(MLKEM_Q + MLKEM_Q / 2) - remainder) >> 31 & 1;

    /* quotient might have overflowed, so mask it. */
    uint32_t mask = ((uint32_t)1 << d) - 1;
    return (uint16_t)(quotient & mask);
}

/* Maps y in 0 to 2ᵈ-1 uniformly to the field, which is (y * q) / 2ᵈ rounded to
 * the nearest integer, a half rounding up. */
MlkemFieldElement burrow__mlkem_decompress(uint16_t y, uint8_t d) {
    uint32_t dividend = (uint32_t)y * MLKEM_Q;
    uint32_t quotient = dividend >> d;
    /* The top bit of the remainder is 1 for the half that rounds up. */
    quotient += dividend >> (d - 1) & 1;
    /* At most (2¹¹-1) * q / 2¹¹ + 1 = 3328, so no overflow. */
    return (MlkemFieldElement)quotient;
}

/* ------------------------------------------------------------------- ring */

static void mlkem_poly_add(MlkemRing *s, const MlkemRing *a, const MlkemRing *b) {
    for (int i = 0; i < MLKEM_N; i++)
        s->c[i] = burrow__mlkem_field_add(a->c[i], b->c[i]);
}

static void mlkem_poly_sub(MlkemRing *s, const MlkemRing *a, const MlkemRing *b) {
    for (int i = 0; i < MLKEM_N; i++)
        s->c[i] = burrow__mlkem_field_sub(a->c[i], b->c[i]);
}

/* ByteEncode₁₂, FIPS 203, Algorithm 5, into the 384 bytes at out. */
static void mlkem_poly_byte_encode(Byte *out, const MlkemRing *f) {
    for (int i = 0; i < MLKEM_N; i += 2) {
        uint32_t x = (uint32_t)f->c[i] | (uint32_t)f->c[i + 1] << 12;
        out[0] = (Byte)x;
        out[1] = (Byte)(x >> 8);
        out[2] = (Byte)(x >> 16);
        out += 3;
    }
}

/* ByteDecode₁₂, FIPS 203, Algorithm 6, from the 384 bytes at b, with the check
 * that every coefficient is reduced. That is the modulus check of ML-KEM
 * encapsulation. */
static bool mlkem_poly_byte_decode(MlkemRing *f, const Byte *b) {
    /* The check runs over every coefficient, as Go stops at the first bad
     * one. Neither is secret: it is a public key. */
    for (int i = 0; i < MLKEM_N; i += 2) {
        uint32_t d = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
        uint16_t lo = (uint16_t)(d & 0xfff), hi = (uint16_t)(d >> 12);
        if (lo >= MLKEM_Q || hi >= MLKEM_Q)
            return false;
        f->c[i] = lo;
        f->c[i + 1] = hi;
        b += 3;
    }
    return true;
}

void burrow__mlkem_ring_compress_and_encode1(Byte *out, const MlkemRing *f) {
    memset(out, 0, MLKEM_ENCODING_SIZE1);
    for (int i = 0; i < MLKEM_N; i++)
        out[i / 8] |= (Byte)(burrow__mlkem_compress(f->c[i], 1) << (i % 8));
}

void burrow__mlkem_ring_decode_and_decompress1(MlkemRing *f, const Byte *b) {
    for (int i = 0; i < MLKEM_N; i++) {
        uint16_t b_i = b[i / 8] >> (i % 8) & 1;
        /* ⌈q/2⌋, rounded up as FIPS 203, Section 2.3, says. 0 decompresses to
         * 0 and 1 to this. */
        f->c[i] = (MlkemFieldElement)(b_i * ((MLKEM_Q + 1) / 2));
    }
}

void burrow__mlkem_ring_compress_and_encode4(Byte *out, const MlkemRing *f) {
    for (int i = 0; i < MLKEM_N; i += 2)
        out[i / 2] = (Byte)(burrow__mlkem_compress(f->c[i], 4) |
                            burrow__mlkem_compress(f->c[i + 1], 4) << 4);
}

void burrow__mlkem_ring_decode_and_decompress4(MlkemRing *f, const Byte *b) {
    for (int i = 0; i < MLKEM_N; i += 2) {
        f->c[i] = burrow__mlkem_decompress((uint16_t)(b[i / 2] & 0xf), 4);
        f->c[i + 1] = burrow__mlkem_decompress((uint16_t)(b[i / 2] >> 4), 4);
    }
}

void burrow__mlkem_ring_compress_and_encode10(Byte *out, const MlkemRing *f) {
    for (int i = 0; i < MLKEM_N; i += 4) {
        uint64_t x = 0;
        x |= (uint64_t)burrow__mlkem_compress(f->c[i], 10);
        x |= (uint64_t)burrow__mlkem_compress(f->c[i + 1], 10) << 10;
        x |= (uint64_t)burrow__mlkem_compress(f->c[i + 2], 10) << 20;
        x |= (uint64_t)burrow__mlkem_compress(f->c[i + 3], 10) << 30;
        out[0] = (Byte)x;
        out[1] = (Byte)(x >> 8);
        out[2] = (Byte)(x >> 16);
        out[3] = (Byte)(x >> 24);
        out[4] = (Byte)(x >> 32);
        out += 5;
    }
}

void burrow__mlkem_ring_decode_and_decompress10(MlkemRing *f, const Byte *b) {
    for (int i = 0; i < MLKEM_N; i += 4) {
        uint64_t x = (uint64_t)b[0] | (uint64_t)b[1] << 8 | (uint64_t)b[2] << 16 |
                     (uint64_t)b[3] << 24 | (uint64_t)b[4] << 32;
        b += 5;
        f->c[i] = burrow__mlkem_decompress((uint16_t)(x & 0x3ff), 10);
        f->c[i + 1] = burrow__mlkem_decompress((uint16_t)(x >> 10 & 0x3ff), 10);
        f->c[i + 2] = burrow__mlkem_decompress((uint16_t)(x >> 20 & 0x3ff), 10);
        f->c[i + 3] = burrow__mlkem_decompress((uint16_t)(x >> 30 & 0x3ff), 10);
    }
}

void burrow__mlkem_ring_compress_and_encode(Byte *out, const MlkemRing *f, uint8_t d) {
    Byte b = 0;
    unsigned b_idx = 0;
    for (int i = 0; i < MLKEM_N; i++) {
        uint16_t c = burrow__mlkem_compress(f->c[i], d);
        unsigned c_idx = 0;
        while (c_idx < d) {
            b |= (Byte)((Byte)(c >> c_idx) << b_idx);
            unsigned bits = 8 - b_idx < d - c_idx ? 8 - b_idx : d - c_idx;
            b_idx += bits;
            c_idx += bits;
            if (b_idx == 8) {
                *out++ = b;
                b = 0;
                b_idx = 0;
            }
        }
    }
}

void burrow__mlkem_ring_decode_and_decompress(MlkemRing *f, const Byte *b, uint8_t d) {
    unsigned b_idx = 0;
    for (int i = 0; i < MLKEM_N; i++) {
        uint16_t c = 0;
        unsigned c_idx = 0;
        while (c_idx < d) {
            c |= (uint16_t)((unsigned)(b[0] >> b_idx) << c_idx);
            c &= (uint16_t)((1U << d) - 1);
            unsigned bits = 8 - b_idx < d - c_idx ? 8 - b_idx : d - c_idx;
            b_idx += bits;
            c_idx += bits;
            if (b_idx == 8) {
                b++;
                b_idx = 0;
            }
        }
        f->c[i] = burrow__mlkem_decompress(c, d);
    }
}

/* SamplePolyCBD, FIPS 203, Algorithm 8, with η = 2: a ring element from the
 * centred binomial distribution, from 128 bytes of SHAKE256(s || b). */
static void mlkem_sample_poly_cbd(MlkemRing *f, const Byte s[32], Byte b) {
    Sha3 prf;
    burrow__shake256_init(&prf);
    burrow__sha3_write(&prf, s, 32);
    burrow__sha3_write(&prf, &b, 1);
    Byte buf[64 * 2];
    burrow__sha3_read(&prf, buf, sizeof buf);

    /* Four bits a coefficient: the first two added and the last two taken
     * away. */
    for (int i = 0; i < MLKEM_N; i += 2) {
        Byte x = buf[i / 2];
        Byte b_7 = x >> 7, b_6 = x >> 6 & 1, b_5 = x >> 5 & 1, b_4 = x >> 4 & 1;
        Byte b_3 = x >> 3 & 1, b_2 = x >> 2 & 1, b_1 = x >> 1 & 1, b_0 = x & 1;
        f->c[i] = burrow__mlkem_field_sub((MlkemFieldElement)(b_0 + b_1),
                                          (MlkemFieldElement)(b_2 + b_3));
        f->c[i + 1] = burrow__mlkem_field_sub((MlkemFieldElement)(b_4 + b_5),
                                              (MlkemFieldElement)(b_6 + b_7));
    }
}

/* ζ^(2BitRev7(i)+1) mod q, FIPS 203, Appendix A, with the negative values
 * made positive. */
const MlkemFieldElement burrow__mlkem_gammas[128] = {
    17,   3312, 2761, 568,  583,  2746, 2649, 680,  1637, 1692, 723,  2606, 2288,
    1041, 1100, 2229, 1409, 1920, 2662, 667,  3281, 48,   233,  3096, 756,  2573,
    2156, 1173, 3015, 314,  3050, 279,  1703, 1626, 1651, 1678, 2789, 540,  1789,
    1540, 1847, 1482, 952,  2377, 1461, 1868, 2687, 642,  939,  2390, 2308, 1021,
    2437, 892,  2388, 941,  733,  2596, 2337, 992,  268,  3061, 641,  2688, 1584,
    1745, 2298, 1031, 2037, 1292, 3220, 109,  375,  2954, 2549, 780,  2090, 1239,
    1645, 1684, 1063, 2266, 319,  3010, 2773, 556,  757,  2572, 2099, 1230, 561,
    2768, 2466, 863,  2594, 735,  2804, 525,  1092, 2237, 403,  2926, 1026, 2303,
    1143, 2186, 2150, 1179, 2775, 554,  886,  2443, 1722, 1607, 1212, 2117, 1874,
    1455, 1029, 2300, 2110, 1219, 2935, 394,  885,  2444, 2154, 1175,
};

/* MultiplyNTTs, FIPS 203, Algorithm 11. h can be f or g. */
static void mlkem_ntt_mul(MlkemRing *h, const MlkemRing *f, const MlkemRing *g) {
    for (int i = 0; i < MLKEM_N; i += 2) {
        MlkemFieldElement a0 = f->c[i], a1 = f->c[i + 1];
        MlkemFieldElement b0 = g->c[i], b1 = g->c[i + 1];
        h->c[i] = mlkem_field_add_mul(a0, b0, burrow__mlkem_field_mul(a1, b1),
                                      burrow__mlkem_gammas[i / 2]);
        h->c[i + 1] = mlkem_field_add_mul(a0, b1, a1, b0);
    }
}

/* ζ^BitRev7(k) mod q, FIPS 203, Appendix A. */
const MlkemFieldElement burrow__mlkem_zetas[128] = {
    1,    1729, 2580, 3289, 2642, 630,  1897, 848,  1062, 1919, 193,  797,  2786,
    3260, 569,  1746, 296,  2447, 1339, 1476, 3046, 56,   2240, 1333, 1426, 2094,
    535,  2882, 2393, 2879, 1974, 821,  289,  331,  3253, 1756, 1197, 2304, 2277,
    2055, 650,  1977, 2513, 632,  2865, 33,   1320, 1915, 2319, 1435, 807,  452,
    1438, 2868, 1534, 2402, 2647, 2617, 1481, 648,  2474, 3110, 1227, 910,  17,
    2761, 583,  2649, 1637, 723,  2288, 1100, 1409, 2662, 3281, 233,  756,  2156,
    3015, 3050, 1703, 1651, 2789, 1789, 1847, 952,  1461, 2687, 939,  2308, 2437,
    2388, 733,  2337, 268,  641,  1584, 2298, 2037, 3220, 375,  2549, 2090, 1645,
    1063, 319,  2773, 757,  2099, 561,  2466, 2594, 2804, 1092, 403,  1026, 1143,
    2150, 2775, 886,  1722, 1212, 1874, 1029, 2110, 2935, 885,  2154,
};

/* NTT, FIPS 203, Algorithm 9, in place. */
static void mlkem_ntt(MlkemRing *r) {
    MlkemFieldElement *f = r->c;
    int k = 1;
    for (int len = 128; len >= 2; len /= 2) {
        for (int start = 0; start < MLKEM_N; start += 2 * len) {
            MlkemFieldElement zeta = burrow__mlkem_zetas[k++];
            MlkemFieldElement *lo = f + start, *hi = f + start + len;
            for (int j = 0; j < len; j++) {
                MlkemFieldElement t = burrow__mlkem_field_mul(zeta, hi[j]);
                hi[j] = burrow__mlkem_field_sub(lo[j], t);
                lo[j] = burrow__mlkem_field_add(lo[j], t);
            }
        }
    }
}

/* NTT⁻¹, FIPS 203, Algorithm 10, in place. */
static void mlkem_inverse_ntt(MlkemRing *r) {
    MlkemFieldElement *f = r->c;
    int k = 127;
    for (int len = 2; len <= 128; len *= 2) {
        for (int start = 0; start < MLKEM_N; start += 2 * len) {
            MlkemFieldElement zeta = burrow__mlkem_zetas[k--];
            MlkemFieldElement *lo = f + start, *hi = f + start + len;
            for (int j = 0; j < len; j++) {
                MlkemFieldElement t = lo[j];
                lo[j] = burrow__mlkem_field_add(t, hi[j]);
                hi[j] = mlkem_field_mul_sub(zeta, hi[j], t);
            }
        }
    }
    for (int i = 0; i < MLKEM_N; i++)
        f[i] = burrow__mlkem_field_mul(f[i], 3303); /* 3303 = 128⁻¹ mod q */
}

/* SampleNTT, FIPS 203, Algorithm 7: a uniformly random NTT element from
 * SHAKE128(rho || ii || jj). It takes 12 bits at a time, little endian, and
 * drops the ones that are not less than q, about 19% of them, three bytes
 * making two. */
static void mlkem_sample_ntt(MlkemRing *a, const Byte rho[32], Byte ii, Byte jj) {
    Sha3 xof;
    burrow__shake128_init(&xof);
    burrow__sha3_write(&xof, rho, 32);
    Byte ij[2] = {ii, jj};
    burrow__sha3_write(&xof, ij, 2);

    Byte buf[24];
    size_t off = sizeof buf;
    int j = 0;
    for (;;) {
        if (off >= sizeof buf) {
            burrow__sha3_read(&xof, buf, sizeof buf);
            off = 0;
        }
        uint16_t d1 = (uint16_t)((buf[off] | buf[off + 1] << 8) & 0xfff);
        uint16_t d2 = (uint16_t)((buf[off + 1] | buf[off + 2] << 8) >> 4);
        off += 3;
        if (d1 < MLKEM_Q)
            a->c[j++] = d1;
        if (j >= MLKEM_N)
            break;
        if (d2 < MLKEM_Q)
            a->c[j++] = d2;
        if (j >= MLKEM_N)
            break;
    }
}

/* ------------------------------------------------------------------- keys */

/* The largest k, for the arrays on the stack. */
#define MLKEM_K_MAX 4

/* The key types: an encapsulation key is ρ, H(ek) and the parsed and expanded
 * PKE encryption key, t and A, with A[i*k+j] = sampleNTT(ρ, j, i). A
 * decapsulation key is the seed, s, and its encapsulation key. alloc and size
 * are what the block came from, and alloc is NULL for the encapsulation key
 * inside a decapsulation key. */
struct MlkemEncapsulationKey768 {
    Byte rho[32];
    Byte h[32];
    MlkemRing t[3];
    MlkemRing a[3 * 3];
    Alloc *alloc;
};

struct MlkemDecapsulationKey768 {
    Byte d[32]; /* the decapsulation key seed */
    Byte z[32]; /* the implicit rejection seed */
    MlkemRing s[3];
    MlkemEncapsulationKey768 ek;
    Alloc *alloc;
};

struct MlkemEncapsulationKey1024 {
    Byte rho[32];
    Byte h[32];
    MlkemRing t[4];
    MlkemRing a[4 * 4];
    Alloc *alloc;
};

struct MlkemDecapsulationKey1024 {
    Byte d[32];
    Byte z[32];
    MlkemRing s[4];
    MlkemEncapsulationKey1024 ek;
    Alloc *alloc;
};

#define MLKEM_TYPE(T, name)                                                            \
    const Type burrow_type_##T = {                                                     \
        BURROW_S_INIT(name),                                                           \
        BURROW_S_INIT("crypto/mlkem"),                                                 \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
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

MLKEM_TYPE(MlkemDecapsulationKey768, "DecapsulationKey768");
MLKEM_TYPE(MlkemEncapsulationKey768, "EncapsulationKey768");
MLKEM_TYPE(MlkemDecapsulationKey1024, "DecapsulationKey1024");
MLKEM_TYPE(MlkemEncapsulationKey1024, "EncapsulationKey1024");

/* A key of either size, as the algorithms see it. d, z and s are NULL for an
 * encapsulation key. */
typedef struct MlkemView {
    int k;
    int du, dv; /* the bits of a ciphertext coefficient of u and of v */
    Byte *rho, *h;
    MlkemRing *t, *a;
    Byte *d, *z;
    MlkemRing *s;
} MlkemView;

static MlkemView mlkem_view_ek768(const MlkemEncapsulationKey768 *ek) {
    MlkemEncapsulationKey768 *e = (MlkemEncapsulationKey768 *)(uintptr_t)ek;
    MlkemView v = {3, 10, 4, e->rho, e->h, e->t, e->a, NULL, NULL, NULL};
    return v;
}

static MlkemView mlkem_view_dk768(const MlkemDecapsulationKey768 *dk) {
    MlkemDecapsulationKey768 *k = (MlkemDecapsulationKey768 *)(uintptr_t)dk;
    MlkemView v = mlkem_view_ek768(&k->ek);
    v.d = k->d;
    v.z = k->z;
    v.s = k->s;
    return v;
}

static MlkemView mlkem_view_ek1024(const MlkemEncapsulationKey1024 *ek) {
    MlkemEncapsulationKey1024 *e = (MlkemEncapsulationKey1024 *)(uintptr_t)ek;
    MlkemView v = {4, 11, 5, e->rho, e->h, e->t, e->a, NULL, NULL, NULL};
    return v;
}

static MlkemView mlkem_view_dk1024(const MlkemDecapsulationKey1024 *dk) {
    MlkemDecapsulationKey1024 *k = (MlkemDecapsulationKey1024 *)(uintptr_t)dk;
    MlkemView v = mlkem_view_ek1024(&k->ek);
    v.d = k->d;
    v.z = k->z;
    v.s = k->s;
    return v;
}

static Int mlkem_ek_size(const MlkemView *v) {
    return v->k * MLKEM_ENCODING_SIZE12 + 32;
}

static Int mlkem_ciphertext_size(const MlkemView *v) {
    return v->k * v->du * 32 + v->dv * 32;
}

/* The encoded encapsulation key, ByteEncode₁₂(t) || ρ, into out. */
static void mlkem_ek_bytes(const MlkemView *v, Byte *out) {
    for (int i = 0; i < v->k; i++) {
        mlkem_poly_byte_encode(out, &v->t[i]);
        out += MLKEM_ENCODING_SIZE12;
    }
    memcpy(out, v->rho, 32);
}

static void mlkem_expand_a(const MlkemView *v) {
    for (int i = 0; i < v->k; i++)
        for (int j = 0; j < v->k; j++)
            mlkem_sample_ntt(&v->a[i * v->k + j], v->rho, (Byte)j, (Byte)i);
}

/* ML-KEM.KeyGen_internal, FIPS 203, Algorithm 16, and K-PKE.KeyGen, Algorithm
 * 13, merged. */
static void mlkem_kem_key_gen(const MlkemView *v, const Byte d[32], const Byte z[32]) {
    int k = v->k;
    memcpy(v->d, d, 32);
    memcpy(v->z, z, 32);

    Sha3 g;
    burrow__sha3_init512(&g);
    burrow__sha3_write(&g, d, 32);
    Byte kb = (Byte)k; /* the module dimension, as a domain separator */
    burrow__sha3_write(&g, &kb, 1);
    Byte gg[64];
    burrow__sha3_read(&g, gg, 64);
    const Byte *sigma = gg + 32;
    memcpy(v->rho, gg, 32);

    mlkem_expand_a(v);

    Byte n = 0;
    for (int i = 0; i < k; i++) {
        mlkem_sample_poly_cbd(&v->s[i], sigma, n++);
        mlkem_ntt(&v->s[i]);
    }
    MlkemRing e[MLKEM_K_MAX];
    for (int i = 0; i < k; i++) {
        mlkem_sample_poly_cbd(&e[i], sigma, n++);
        mlkem_ntt(&e[i]);
    }

    /* t = A ◦ s + e */
    for (int i = 0; i < k; i++) {
        v->t[i] = e[i];
        for (int j = 0; j < k; j++) {
            MlkemRing p;
            mlkem_ntt_mul(&p, &v->a[i * k + j], &v->s[j]);
            mlkem_poly_add(&v->t[i], &v->t[i], &p);
        }
    }

    Byte ek[MLKEM_K_MAX * MLKEM_ENCODING_SIZE12 + 32];
    mlkem_ek_bytes(v, ek);
    Sha3 hh;
    burrow__sha3_init256(&hh);
    burrow__sha3_write(&hh, ek, mlkem_ek_size(v));
    burrow__sha3_read(&hh, v->h, 32);

    memset(gg, 0, sizeof gg);
    memset(e, 0, sizeof e);
}

/* The start of K-PKE.Encrypt, FIPS 203, Algorithm 14: parsing ek, with its
 * hash, t and A. ek is the right length. */
static bool mlkem_parse_ek(const MlkemView *v, const Byte *ek) {
    Sha3 hh;
    burrow__sha3_init256(&hh);
    burrow__sha3_write(&hh, ek, mlkem_ek_size(v));
    burrow__sha3_read(&hh, v->h, 32);
    for (int i = 0; i < v->k; i++) {
        if (!mlkem_poly_byte_decode(&v->t[i], ek))
            return false;
        ek += MLKEM_ENCODING_SIZE12;
    }
    memcpy(v->rho, ek, 32);
    mlkem_expand_a(v);
    return true;
}

/* K-PKE.Encrypt, FIPS 203, Algorithm 14, with t and A from parsing: the
 * ciphertext of m with randomness rnd, into c. */
static void mlkem_pke_encrypt(const MlkemView *v, Byte *c, const Byte m[32],
                              const Byte rnd[32]) {
    int k = v->k;
    MlkemRing r[MLKEM_K_MAX], e1[MLKEM_K_MAX], e2;
    Byte n = 0;
    for (int i = 0; i < k; i++) {
        mlkem_sample_poly_cbd(&r[i], rnd, n++);
        mlkem_ntt(&r[i]);
    }
    for (int i = 0; i < k; i++)
        mlkem_sample_poly_cbd(&e1[i], rnd, n++);
    mlkem_sample_poly_cbd(&e2, rnd, n);

    /* u = NTT⁻¹(Aᵀ ◦ r) + e1, encoded as it is made. */
    for (int i = 0; i < k; i++) {
        MlkemRing u_hat, p;
        memset(&u_hat, 0, sizeof u_hat);
        for (int j = 0; j < k; j++) {
            /* i and j the other way round, for the transpose of A. */
            mlkem_ntt_mul(&p, &v->a[j * k + i], &r[j]);
            mlkem_poly_add(&u_hat, &u_hat, &p);
        }
        mlkem_inverse_ntt(&u_hat);
        mlkem_poly_add(&u_hat, &e1[i], &u_hat);
        if (v->du == 10)
            burrow__mlkem_ring_compress_and_encode10(c, &u_hat);
        else
            burrow__mlkem_ring_compress_and_encode(c, &u_hat, (uint8_t)v->du);
        c += (Int)v->du * 32;
    }

    MlkemRing mu;
    burrow__mlkem_ring_decode_and_decompress1(&mu, m);

    /* v = NTT⁻¹(tᵀ ◦ r) + e2 + μ */
    MlkemRing vv, p;
    memset(&vv, 0, sizeof vv);
    for (int i = 0; i < k; i++) {
        mlkem_ntt_mul(&p, &v->t[i], &r[i]);
        mlkem_poly_add(&vv, &vv, &p);
    }
    mlkem_inverse_ntt(&vv);
    mlkem_poly_add(&vv, &vv, &e2);
    mlkem_poly_add(&vv, &vv, &mu);
    if (v->dv == 4)
        burrow__mlkem_ring_compress_and_encode4(c, &vv);
    else
        burrow__mlkem_ring_compress_and_encode(c, &vv, (uint8_t)v->dv);
}

/* K-PKE.Decrypt, FIPS 203, Algorithm 15, with s kept from key generation: the
 * message in c, into m. */
static void mlkem_pke_decrypt(const MlkemView *v, Byte m[32], const Byte *c) {
    int k = v->k;
    MlkemRing mask, u, p;
    memset(&mask, 0, sizeof mask);
    /* mask = sᵀ ◦ NTT(u) */
    for (int i = 0; i < k; i++) {
        if (v->du == 10)
            burrow__mlkem_ring_decode_and_decompress10(&u, c + (Int)i * v->du * 32);
        else
            burrow__mlkem_ring_decode_and_decompress(&u, c + (Int)i * v->du * 32,
                                                     (uint8_t)v->du);
        mlkem_ntt(&u);
        mlkem_ntt_mul(&p, &v->s[i], &u);
        mlkem_poly_add(&mask, &mask, &p);
    }
    MlkemRing vv;
    const Byte *cv = c + (Int)k * v->du * 32;
    if (v->dv == 4)
        burrow__mlkem_ring_decode_and_decompress4(&vv, cv);
    else
        burrow__mlkem_ring_decode_and_decompress(&vv, cv, (uint8_t)v->dv);
    mlkem_inverse_ntt(&mask);
    mlkem_poly_sub(&vv, &vv, &mask);
    burrow__mlkem_ring_compress_and_encode1(m, &vv);
}

/* ML-KEM.Encaps_internal, FIPS 203, Algorithm 17: the shared key into key and
 * the ciphertext into c. */
static void mlkem_kem_encaps(const MlkemView *v, Byte key[32], Byte *c,
                             const Byte m[32]) {
    Sha3 g;
    burrow__sha3_init512(&g);
    burrow__sha3_write(&g, m, 32);
    burrow__sha3_write(&g, v->h, 32);
    Byte gg[64];
    burrow__sha3_read(&g, gg, 64);
    memcpy(key, gg, MLKEM_SHARED_KEY_SIZE);
    mlkem_pke_encrypt(v, c, m, gg + MLKEM_SHARED_KEY_SIZE);
}

/* ML-KEM.Decaps_internal, FIPS 203, Algorithm 18: the shared key in c. A c
 * that does not encrypt again to itself gives the implicit rejection key
 * instead, chosen in constant time. */
static void mlkem_kem_decaps(const MlkemView *v, Byte key[32], const Byte *c) {
    Int csize = mlkem_ciphertext_size(v);
    Byte m[32];
    mlkem_pke_decrypt(v, m, c);
    Sha3 g;
    burrow__sha3_init512(&g);
    burrow__sha3_write(&g, m, 32);
    burrow__sha3_write(&g, v->h, 32);
    Byte gg[64];
    burrow__sha3_read(&g, gg, 64);

    Sha3 j;
    burrow__shake256_init(&j);
    burrow__sha3_write(&j, v->z, 32);
    burrow__sha3_write(&j, c, csize);
    burrow__sha3_read(&j, key, MLKEM_SHARED_KEY_SIZE);

    Byte c1[MLKEM_CIPHERTEXT_SIZE1024];
    mlkem_pke_encrypt(v, c1, m, gg + MLKEM_SHARED_KEY_SIZE);

    Int eq = subtle_constant_time_compare(
        slice_from((void *)(uintptr_t)c, csize, csize, TYPE_BYTE),
        slice_from(c1, csize, csize, TYPE_BYTE));
    subtle_constant_time_copy(
        eq, slice_from(key, MLKEM_SHARED_KEY_SIZE, MLKEM_SHARED_KEY_SIZE, TYPE_BYTE),
        slice_from(gg, MLKEM_SHARED_KEY_SIZE, MLKEM_SHARED_KEY_SIZE, TYPE_BYTE));
    memset(m, 0, sizeof m);
    memset(gg, 0, sizeof gg);
}

/* ------------------------------------------------------------ the API */

static void mlkem_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
}

static Alloc *mlkem_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* A copy of the n bytes at p as a new Slice from a. */
static Slice mlkem_bytes(Alloc *a, const Byte *p, Int n) {
    Slice s = slice_make(mlkem_alloc(a), TYPE_BYTE, n, n);
    if (s.p != NULL)
        memcpy(s.p, p, (size_t)n);
    return s;
}

static void *mlkem_new(Alloc *a, size_t size, size_t align, Error *err) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    memset(p, 0, size);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

/* Encapsulate with the message m, as two new Slices from a. */
static Slice mlkem_encapsulate_with(const MlkemView *v, Alloc *a, const Byte m[32],
                                    Slice *ciphertext) {
    Byte key[MLKEM_SHARED_KEY_SIZE];
    Byte c[MLKEM_CIPHERTEXT_SIZE1024];
    mlkem_kem_encaps(v, key, c, m);
    Slice k = mlkem_bytes(a, key, MLKEM_SHARED_KEY_SIZE);
    Slice ct = mlkem_bytes(a, c, mlkem_ciphertext_size(v));
    memset(key, 0, sizeof key);
    if (k.p == NULL || ct.p == NULL) {
        if (k.p != NULL)
            memset(k.p, 0, MLKEM_SHARED_KEY_SIZE);
        *ciphertext = slice_nil(TYPE_BYTE);
        return slice_nil(TYPE_BYTE);
    }
    *ciphertext = ct;
    return k;
}

static Slice mlkem_encapsulate(const MlkemView *v, Alloc *a, Slice *ciphertext) {
    Byte m[MLKEM_MESSAGE_SIZE];
    burrow__crypto_rand_system(slice_from(m, sizeof m, sizeof m, TYPE_BYTE));
    Slice k = mlkem_encapsulate_with(v, a, m, ciphertext);
    memset(m, 0, sizeof m);
    return k;
}

static Slice mlkem_decapsulate(const MlkemView *v, Alloc *a, Slice ciphertext,
                               Error *err) {
    if (ciphertext.len != mlkem_ciphertext_size(v)) {
        mlkem_set_error(err, "mlkem: invalid ciphertext length");
        return slice_nil(TYPE_BYTE);
    }
    Byte key[MLKEM_SHARED_KEY_SIZE];
    mlkem_kem_decaps(v, key, (const Byte *)ciphertext.p);
    Slice k = mlkem_bytes(a, key, MLKEM_SHARED_KEY_SIZE);
    memset(key, 0, sizeof key);
    if (k.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return k;
}

/* TestingOnlyNewDecapsulationKey: s, then the encapsulation key, then H(ek),
 * then z. */
static bool mlkem_parse_expanded(const MlkemView *v, Slice b, Error *err) {
    const Byte *p = (const Byte *)b.p;
    for (int i = 0; i < v->k; i++) {
        if (!mlkem_poly_byte_decode(&v->s[i], p)) {
            mlkem_set_error(err, "mlkem: invalid secret key encoding");
            return false;
        }
        p += MLKEM_ENCODING_SIZE12;
    }
    if (!mlkem_parse_ek(v, p)) {
        mlkem_set_error(err, "mlkem: invalid polynomial encoding");
        return false;
    }
    p += mlkem_ek_size(v);
    if (memcmp(v->h, p, 32) != 0) {
        mlkem_set_error(err, "mlkem: inconsistent H(ek) in encoded bytes");
        return false;
    }
    memcpy(v->z, p + 32, 32);
    /* A random d, so that Bytes of a key made this way, which it should not be
     * asked for, is a random key and not a broken one. */
    burrow__crypto_rand_system(slice_from(v->d, 32, 32, TYPE_BYTE));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return true;
}

static Slice mlkem_expanded_bytes(const MlkemView *v, Alloc *a) {
    Int n = (Int)v->k * MLKEM_ENCODING_SIZE12 + mlkem_ek_size(v) + 32 + 32;
    Slice b = slice_make(mlkem_alloc(a), TYPE_BYTE, n, n);
    if (b.p == NULL)
        return b;
    Byte *p = (Byte *)b.p;
    for (int i = 0; i < v->k; i++) {
        mlkem_poly_byte_encode(p, &v->s[i]);
        p += MLKEM_ENCODING_SIZE12;
    }
    mlkem_ek_bytes(v, p);
    p += mlkem_ek_size(v);
    memcpy(p, v->h, 32);
    memcpy(p + 32, v->z, 32);
    return b;
}

/* The functions of each size. The bodies are the same with 768 or 1024 in the
 * names, so they are written once. */
#define MLKEM_FUNCS(N)                                                                 \
    static MlkemDecapsulationKey##N *mlkem_new_dk##N(Alloc *a, Error *err) {           \
        a = mlkem_alloc(a);                                                            \
        MlkemDecapsulationKey##N *dk =                                                 \
            mlkem_new(a, sizeof *dk, _Alignof(MlkemDecapsulationKey##N), err);         \
        if (dk != NULL)                                                                \
            dk->alloc = a;                                                             \
        return dk;                                                                     \
    }                                                                                  \
                                                                                       \
    MlkemDecapsulationKey##N *burrow__mlkem_generate_key_internal##N(                  \
        Alloc *a, const Byte d[32], const Byte z[32]) {                                \
        MlkemDecapsulationKey##N *dk = mlkem_new_dk##N(a, NULL);                       \
        if (dk == NULL)                                                                \
            return NULL;                                                               \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        mlkem_kem_key_gen(&v, d, z);                                                   \
        return dk;                                                                     \
    }                                                                                  \
                                                                                       \
    MlkemDecapsulationKey##N *mlkem_generate_key##N(Alloc *a, Error *err) {            \
        MlkemDecapsulationKey##N *dk = mlkem_new_dk##N(a, err);                        \
        if (dk == NULL)                                                                \
            return NULL;                                                               \
        Byte seed[64];                                                                 \
        burrow__crypto_rand_system(slice_from(seed, 64, 64, TYPE_BYTE));               \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        mlkem_kem_key_gen(&v, seed, seed + 32);                                        \
        memset(seed, 0, sizeof seed);                                                  \
        return dk;                                                                     \
    }                                                                                  \
                                                                                       \
    MlkemDecapsulationKey##N *mlkem_new_decapsulation_key##N(Alloc *a, Slice seed,     \
                                                             Error *err) {             \
        if (seed.len != MLKEM_SEED_SIZE) {                                             \
            mlkem_set_error(err, "mlkem: invalid seed length");                        \
            return NULL;                                                               \
        }                                                                              \
        MlkemDecapsulationKey##N *dk = mlkem_new_dk##N(a, err);                        \
        if (dk == NULL)                                                                \
            return NULL;                                                               \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        const Byte *p = (const Byte *)seed.p;                                          \
        mlkem_kem_key_gen(&v, p, p + 32);                                              \
        return dk;                                                                     \
    }                                                                                  \
                                                                                       \
    MlkemDecapsulationKey##N *burrow__mlkem_testing_only_new_decapsulation_key##N(     \
        Alloc *a, Slice b, Error *err) {                                               \
        if (b.len != MLKEM_DECAPSULATION_KEY_SIZE##N) {                                \
            mlkem_set_error(err, "mlkem: invalid NIST decapsulation key length");      \
            return NULL;                                                               \
        }                                                                              \
        MlkemDecapsulationKey##N *dk = mlkem_new_dk##N(a, err);                        \
        if (dk == NULL)                                                                \
            return NULL;                                                               \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        if (!mlkem_parse_expanded(&v, b, err)) {                                       \
            mlkem_decapsulation_key##N##_free(dk);                                     \
            return NULL;                                                               \
        }                                                                              \
        return dk;                                                                     \
    }                                                                                  \
                                                                                       \
    Slice burrow__mlkem_testing_only_expanded_bytes##N(                                \
        const MlkemDecapsulationKey##N *dk, Alloc *a) {                                \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        return mlkem_expanded_bytes(&v, a);                                            \
    }                                                                                  \
                                                                                       \
    void mlkem_decapsulation_key##N##_free(MlkemDecapsulationKey##N *dk) {             \
        if (dk == NULL)                                                                \
            return;                                                                    \
        Alloc *a = dk->alloc;                                                          \
        memset(dk, 0, sizeof *dk);                                                     \
        mem_free(a, dk, sizeof *dk, _Alignof(MlkemDecapsulationKey##N));               \
    }                                                                                  \
                                                                                       \
    Slice mlkem_decapsulation_key##N##_bytes(const MlkemDecapsulationKey##N *dk,       \
                                             Alloc *a) {                               \
        Byte b[MLKEM_SEED_SIZE];                                                       \
        memcpy(b, dk->d, 32);                                                          \
        memcpy(b + 32, dk->z, 32);                                                     \
        Slice s = mlkem_bytes(a, b, MLKEM_SEED_SIZE);                                  \
        memset(b, 0, sizeof b);                                                        \
        return s;                                                                      \
    }                                                                                  \
                                                                                       \
    Slice mlkem_decapsulation_key##N##_decapsulate(                                    \
        const MlkemDecapsulationKey##N *dk, Alloc *a, Slice ciphertext, Error *err) {  \
        MlkemView v = mlkem_view_dk##N(dk);                                            \
        return mlkem_decapsulate(&v, a, ciphertext, err);                              \
    }                                                                                  \
                                                                                       \
    const MlkemEncapsulationKey##N *mlkem_decapsulation_key##N##_encapsulation_key(    \
        const MlkemDecapsulationKey##N *dk) {                                          \
        return &dk->ek;                                                                \
    }                                                                                  \
                                                                                       \
    CryptoEncapsulator mlkem_decapsulation_key##N##_encapsulator(                      \
        const MlkemDecapsulationKey##N *dk) {                                          \
        return mlkem_encapsulation_key##N##_as_encapsulator(&dk->ek);                  \
    }                                                                                  \
                                                                                       \
    static CryptoEncapsulator mlkem_dk##N##_vt_encapsulator(void *self) {              \
        return mlkem_decapsulation_key##N##_encapsulator(self);                        \
    }                                                                                  \
                                                                                       \
    static Slice mlkem_dk##N##_vt_decapsulate(void *self, Alloc *a, Slice ciphertext,  \
                                              Error *err) {                            \
        return mlkem_decapsulation_key##N##_decapsulate(self, a, ciphertext, err);     \
    }                                                                                  \
                                                                                       \
    static const CryptoDecapsulatorVT mlkem_dk##N##_vt = {                             \
        &burrow_type_MlkemDecapsulationKey##N,                                         \
        mlkem_dk##N##_vt_encapsulator,                                                 \
        mlkem_dk##N##_vt_decapsulate,                                                  \
    };                                                                                 \
                                                                                       \
    CryptoDecapsulator mlkem_decapsulation_key##N##_as_decapsulator(                   \
        const MlkemDecapsulationKey##N *dk) {                                          \
        CryptoDecapsulator d = {&mlkem_dk##N##_vt, (void *)(uintptr_t)dk};             \
        return d;                                                                      \
    }                                                                                  \
                                                                                       \
    MlkemEncapsulationKey##N *mlkem_new_encapsulation_key##N(                          \
        Alloc *a, Slice encapsulation_key, Error *err) {                               \
        if (encapsulation_key.len != MLKEM_ENCAPSULATION_KEY_SIZE##N) {                \
            mlkem_set_error(err, "mlkem: invalid encapsulation key length");           \
            return NULL;                                                               \
        }                                                                              \
        a = mlkem_alloc(a);                                                            \
        MlkemEncapsulationKey##N *ek =                                                 \
            mlkem_new(a, sizeof *ek, _Alignof(MlkemEncapsulationKey##N), err);         \
        if (ek == NULL)                                                                \
            return NULL;                                                               \
        ek->alloc = a;                                                                 \
        MlkemView v = mlkem_view_ek##N(ek);                                            \
        if (!mlkem_parse_ek(&v, (const Byte *)encapsulation_key.p)) {                  \
            mlkem_encapsulation_key##N##_free(ek);                                     \
            mlkem_set_error(err, "mlkem: invalid polynomial encoding");                \
            return NULL;                                                               \
        }                                                                              \
        return ek;                                                                     \
    }                                                                                  \
                                                                                       \
    void mlkem_encapsulation_key##N##_free(MlkemEncapsulationKey##N *ek) {             \
        if (ek == NULL || ek->alloc == NULL)                                           \
            return;                                                                    \
        mem_free(ek->alloc, ek, sizeof *ek, _Alignof(MlkemEncapsulationKey##N));       \
    }                                                                                  \
                                                                                       \
    Slice mlkem_encapsulation_key##N##_bytes(const MlkemEncapsulationKey##N *ek,       \
                                             Alloc *a) {                               \
        MlkemView v = mlkem_view_ek##N(ek);                                            \
        Byte b[MLKEM_ENCAPSULATION_KEY_SIZE##N];                                       \
        mlkem_ek_bytes(&v, b);                                                         \
        return mlkem_bytes(a, b, MLKEM_ENCAPSULATION_KEY_SIZE##N);                     \
    }                                                                                  \
                                                                                       \
    Slice mlkem_encapsulation_key##N##_encapsulate(const MlkemEncapsulationKey##N *ek, \
                                                   Alloc *a, Slice *ciphertext) {      \
        MlkemView v = mlkem_view_ek##N(ek);                                            \
        return mlkem_encapsulate(&v, a, ciphertext);                                   \
    }                                                                                  \
                                                                                       \
    static Slice mlkem_ek##N##_vt_bytes(void *self, Alloc *a) {                        \
        return mlkem_encapsulation_key##N##_bytes(self, a);                            \
    }                                                                                  \
                                                                                       \
    static CryptoEncapsulateResult mlkem_ek##N##_vt_encapsulate(void *self,            \
                                                                Alloc *a) {            \
        CryptoEncapsulateResult r;                                                     \
        r.shared_key =                                                                 \
            mlkem_encapsulation_key##N##_encapsulate(self, a, &r.ciphertext);          \
        return r;                                                                      \
    }                                                                                  \
                                                                                       \
    static const CryptoEncapsulatorVT mlkem_ek##N##_vt = {                             \
        &burrow_type_MlkemEncapsulationKey##N,                                         \
        mlkem_ek##N##_vt_bytes,                                                        \
        mlkem_ek##N##_vt_encapsulate,                                                  \
    };                                                                                 \
                                                                                       \
    CryptoEncapsulator mlkem_encapsulation_key##N##_as_encapsulator(                   \
        const MlkemEncapsulationKey##N *ek) {                                          \
        CryptoEncapsulator e = {&mlkem_ek##N##_vt, (void *)(uintptr_t)ek};             \
        return e;                                                                      \
    }

MLKEM_FUNCS(768)
MLKEM_FUNCS(1024)

Slice burrow__mlkem_encapsulate_internal768(const MlkemEncapsulationKey768 *ek,
                                            Alloc *a, const Byte m[32],
                                            Slice *ciphertext) {
    MlkemView v = mlkem_view_ek768(ek);
    return mlkem_encapsulate_with(&v, a, m, ciphertext);
}

Slice burrow__mlkem_encapsulate_internal1024(const MlkemEncapsulationKey1024 *ek,
                                             Alloc *a, const Byte m[32],
                                             Slice *ciphertext) {
    MlkemView v = mlkem_view_ek1024(ek);
    return mlkem_encapsulate_with(&v, a, m, ciphertext);
}
