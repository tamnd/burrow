/* Derived from Go's src/crypto/mldsa/mldsa.go and mldsa_fips140v1.26.go, and
 * src/crypto/internal/fips140/mldsa/field.go, mldsa.go, semiexpanded.go and
 * cast.go. Go source: go1.27.1.
 *
 * Go keeps the expanded key in fixed arrays sized for ML-DSA-87 and the
 * parameters next to them, and so does this. Where Go makes a slice for the
 * vectors of one signature or one verification, which are too big for a
 * goroutine's stack to be a good place for them, this allocates them in one
 * block from the heap, and running out of memory is an error.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/mldsa.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "mldsa_internal.h"
#include "rand_internal.h"
#include "sha3_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- parameters */

enum {
    MLDSA_MAX_K = 8,
    MLDSA_MAX_L = 7,
    MLDSA_MAX_LAMBDA = 256,
    MLDSA_MAX_GAMMA1 = 19,
    MLDSA_MAX_PUB_KEY_SIZE = MLDSA_MLDSA87_PUBLIC_KEY_SIZE,
    MLDSA_MAX_SIG_SIZE = MLDSA_MLDSA87_SIGNATURE_SIZE,
};

/* Go's public Parameters and its internal parameters in one. */
struct MldsaParameters {
    Str name;
    Int public_key_size, signature_size;
    int k, l;   /* dimensions of A */
    int eta;    /* bound for secret coefficients */
    int gamma1; /* log₂(γ₁), where [-γ₁+1, γ₁] is the bound of y */
    int gamma2; /* denominator of γ₂ = (q - 1) / γ2 */
    int lambda; /* collision strength */
    int tau;    /* number of non-zero coefficients in challenge */
    int omega;  /* max number of hints in MakeHint */
};

static const MldsaParameters mldsa_params44 = {BURROW_S_INIT("ML-DSA-44"),
                                               MLDSA_MLDSA44_PUBLIC_KEY_SIZE,
                                               MLDSA_MLDSA44_SIGNATURE_SIZE,
                                               4,
                                               4,
                                               2,
                                               17,
                                               88,
                                               128,
                                               39,
                                               80};
static const MldsaParameters mldsa_params65 = {BURROW_S_INIT("ML-DSA-65"),
                                               MLDSA_MLDSA65_PUBLIC_KEY_SIZE,
                                               MLDSA_MLDSA65_SIGNATURE_SIZE,
                                               6,
                                               5,
                                               4,
                                               19,
                                               32,
                                               192,
                                               49,
                                               55};
static const MldsaParameters mldsa_params87 = {BURROW_S_INIT("ML-DSA-87"),
                                               MLDSA_MLDSA87_PUBLIC_KEY_SIZE,
                                               MLDSA_MLDSA87_SIGNATURE_SIZE,
                                               8,
                                               7,
                                               2,
                                               19,
                                               32,
                                               256,
                                               60,
                                               75};

const MldsaParameters *mldsa_mldsa44(void) {
    return &mldsa_params44;
}

const MldsaParameters *mldsa_mldsa65(void) {
    return &mldsa_params65;
}

const MldsaParameters *mldsa_mldsa87(void) {
    return &mldsa_params87;
}

static bool mldsa_valid_params(const MldsaParameters *p) {
    return p == &mldsa_params44 || p == &mldsa_params65 || p == &mldsa_params87;
}

Str mldsa_parameters_string(const MldsaParameters *params) {
    return mldsa_valid_params(params) ? params->name : BURROW_STR_EMPTY;
}

Int mldsa_parameters_public_key_size(const MldsaParameters *params) {
    return mldsa_valid_params(params) ? params->public_key_size : 0;
}

Int mldsa_parameters_signature_size(const MldsaParameters *params) {
    return mldsa_valid_params(params) ? params->signature_size : 0;
}

Int burrow__mldsa_pub_key_size(const MldsaParameters *p) {
    /* ρ + k × n × 10-bit coefficients of t₁ */
    return 32 + (Int)p->k * MLDSA_N * 10 / 8;
}

Int burrow__mldsa_sig_size(const MldsaParameters *p) {
    /* challenge + l × n × (γ₁+1)-bit coefficients of z + hint */
    return (Int)p->lambda / 4 + (Int)p->l * MLDSA_N * (p->gamma1 + 1) / 8 + p->omega +
           p->k;
}

/* ------------------------------------------------------------ errors */

static Error mldsa_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static void mldsa_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, mldsa_error(msg));
}

static Alloc *mldsa_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* ------------------------------------------------------------------ field */

/* x if a <= b and y otherwise, for any a and b, in constant time. */
static inline int32_t mldsa_ct_select_le(int32_t a, int32_t b, int32_t yes,
                                         int32_t no) {
    int64_t d = (int64_t)b - (int64_t)a;
    uint32_t mask = (uint32_t)0 - (uint32_t)(1 - (uint32_t)((uint64_t)d >> 63));
    return (int32_t)(((uint32_t)yes & mask) | ((uint32_t)no & ~mask));
}

/* yes if a == b and no otherwise, in constant time. */
static inline uint32_t mldsa_ct_select_eq(uint32_t a, uint32_t b, uint32_t yes,
                                          uint32_t no) {
    uint32_t eq = (uint32_t)(((uint64_t)(a ^ b) - 1) >> 63);
    uint32_t mask = (uint32_t)0 - eq;
    return (yes & mask) | (no & ~mask);
}

/* 1 if a == b and 0 otherwise, in constant time. */
static inline uint8_t mldsa_ct_byte_eq(uint8_t a, uint8_t b) {
    return (uint8_t)((((uint32_t)(a ^ b)) - 1) >> 31);
}

uint32_t burrow__mldsa_constant_time_abs(int32_t x) {
    return (uint32_t)mldsa_ct_select_le(0, x, x, -x);
}

MldsaFieldElement burrow__mldsa_field_reduce_once(uint32_t a) {
    uint64_t x = (uint64_t)a - MLDSA_Q;
    uint64_t b = x >> 63;
    return (MldsaFieldElement)(x + b * MLDSA_Q);
}

MldsaFieldElement burrow__mldsa_field_add(MldsaFieldElement a, MldsaFieldElement b) {
    return burrow__mldsa_field_reduce_once(a + b);
}

MldsaFieldElement burrow__mldsa_field_sub(MldsaFieldElement a, MldsaFieldElement b) {
    return burrow__mldsa_field_reduce_once(a - b + MLDSA_Q);
}

/* x * R⁻¹ mod q for x < q * R. */
static inline MldsaFieldElement mldsa_montgomery_reduce(uint64_t x) {
    uint32_t t = (uint32_t)x * MLDSA_Q_NEG_INV;
    uint64_t u = (x + (uint64_t)t * MLDSA_Q) >> 32;
    return burrow__mldsa_field_reduce_once((uint32_t)u);
}

MldsaFieldElement burrow__mldsa_field_montgomery_mul(MldsaFieldElement a,
                                                     MldsaFieldElement b) {
    return mldsa_montgomery_reduce((uint64_t)a * (uint64_t)b);
}

MldsaFieldElement burrow__mldsa_field_montgomery_mul_sub(MldsaFieldElement a,
                                                         MldsaFieldElement b,
                                                         MldsaFieldElement c) {
    return mldsa_montgomery_reduce((uint64_t)a * (uint64_t)(uint32_t)(b - c + MLDSA_Q));
}

MldsaFieldElement burrow__mldsa_field_montgomery_add_mul(MldsaFieldElement a,
                                                         MldsaFieldElement b,
                                                         MldsaFieldElement c,
                                                         MldsaFieldElement d) {
    uint64_t x = (uint64_t)a * (uint64_t)b;
    x += (uint64_t)c * (uint64_t)d;
    return mldsa_montgomery_reduce(x);
}

bool burrow__mldsa_field_to_montgomery(uint32_t a, MldsaFieldElement *out) {
    if (a >= MLDSA_Q) {
        *out = 0;
        return false;
    }
    /* a * R² * R⁻¹ ≡ a * R (mod q) */
    *out = burrow__mldsa_field_montgomery_mul(a, MLDSA_RR);
    return true;
}

MldsaFieldElement burrow__mldsa_field_sub_to_montgomery(uint32_t a, uint32_t b) {
    return burrow__mldsa_field_montgomery_mul(a - b + MLDSA_Q, MLDSA_RR);
}

uint32_t burrow__mldsa_field_from_montgomery(MldsaFieldElement a) {
    /* (a * R) * 1 * R⁻¹ ≡ a (mod q) */
    return mldsa_montgomery_reduce(a);
}

int32_t burrow__mldsa_field_centered_mod(MldsaFieldElement r) {
    int32_t x = (int32_t)burrow__mldsa_field_from_montgomery(r);
    /* x <= q / 2 ? x : x - q */
    return mldsa_ct_select_le(x, MLDSA_Q / 2, x, x - MLDSA_Q);
}

uint32_t burrow__mldsa_field_infinity_norm(MldsaFieldElement r) {
    int32_t x = (int32_t)burrow__mldsa_field_from_montgomery(r);
    /* x <= q / 2 ? x : |x - q|, which is q - x as x < q */
    return (uint32_t)mldsa_ct_select_le(x, MLDSA_Q / 2, x, MLDSA_Q - x);
}

#define mldsa_add burrow__mldsa_field_add
#define mldsa_sub burrow__mldsa_field_sub
#define mldsa_mul burrow__mldsa_field_montgomery_mul
#define mldsa_mul_sub burrow__mldsa_field_montgomery_mul_sub
#define mldsa_from_mont burrow__mldsa_field_from_montgomery
#define mldsa_sub_to_mont burrow__mldsa_field_sub_to_montgomery

/* ringElement and nttElement, which Go keeps as two types of the same array
 * and this keeps as one: a polynomial, or its NTT representation. */
typedef struct MldsaRing {
    MldsaFieldElement c[MLDSA_N];
} MldsaRing;

static void mldsa_poly_add(MldsaRing *s, const MldsaRing *a, const MldsaRing *b) {
    for (int i = 0; i < MLDSA_N; i++)
        s->c[i] = mldsa_add(a->c[i], b->c[i]);
}

static void mldsa_poly_sub(MldsaRing *s, const MldsaRing *a, const MldsaRing *b) {
    for (int i = 0; i < MLDSA_N; i++)
        s->c[i] = mldsa_sub(a->c[i], b->c[i]);
}

/* s += a ∘ b, the polyAdd of an nttMul. */
static void mldsa_ntt_mul_add(MldsaRing *s, const MldsaRing *a, const MldsaRing *b) {
    for (int i = 0; i < MLDSA_N; i++)
        s->c[i] = mldsa_add(s->c[i], mldsa_mul(a->c[i], b->c[i]));
}

static void mldsa_ntt_mul(MldsaRing *p, const MldsaRing *a, const MldsaRing *b) {
    for (int i = 0; i < MLDSA_N; i++)
        p->c[i] = mldsa_mul(a->c[i], b->c[i]);
}

const MldsaFieldElement burrow__mldsa_zetas[256] = {
    4193792, 25847,   5771523, 7861508, 237124,  7602457, 7504169, 466468,  1826347,
    2353451, 8021166, 6288512, 3119733, 5495562, 3111497, 2680103, 2725464, 1024112,
    7300517, 3585928, 7830929, 7260833, 2619752, 6271868, 6262231, 4520680, 6980856,
    5102745, 1757237, 8360995, 4010497, 280005,  2706023, 95776,   3077325, 3530437,
    6718724, 4788269, 5842901, 3915439, 4519302, 5336701, 3574422, 5512770, 3539968,
    8079950, 2348700, 7841118, 6681150, 6736599, 3505694, 4558682, 3507263, 6239768,
    6779997, 3699596, 811944,  531354,  954230,  3881043, 3900724, 5823537, 2071892,
    5582638, 4450022, 6851714, 4702672, 5339162, 6927966, 3475950, 2176455, 6795196,
    7122806, 1939314, 4296819, 7380215, 5190273, 5223087, 4747489, 126922,  3412210,
    7396998, 2147896, 2715295, 5412772, 4686924, 7969390, 5903370, 7709315, 7151892,
    8357436, 7072248, 7998430, 1349076, 1852771, 6949987, 5037034, 264944,  508951,
    3097992, 44288,   7280319, 904516,  3958618, 4656075, 8371839, 1653064, 5130689,
    2389356, 8169440, 759969,  7063561, 189548,  4827145, 3159746, 6529015, 5971092,
    8202977, 1315589, 1341330, 1285669, 6795489, 7567685, 6940675, 5361315, 4499357,
    4751448, 3839961, 2091667, 3407706, 2316500, 3817976, 5037939, 2244091, 5933984,
    4817955, 266997,  2434439, 7144689, 3513181, 4860065, 4621053, 7183191, 5187039,
    900702,  1859098, 909542,  819034,  495491,  6767243, 8337157, 7857917, 7725090,
    5257975, 2031748, 3207046, 4823422, 7855319, 7611795, 4784579, 342297,  286988,
    5942594, 4108315, 3437287, 5038140, 1735879, 203044,  2842341, 2691481, 5790267,
    1265009, 4055324, 1247620, 2486353, 1595974, 4613401, 1250494, 2635921, 4832145,
    5386378, 1869119, 1903435, 7329447, 7047359, 1237275, 5062207, 6950192, 7929317,
    1312455, 3306115, 6417775, 7100756, 1917081, 5834105, 7005614, 1500165, 777191,
    2235880, 3406031, 7838005, 5548557, 6709241, 6533464, 5796124, 4656147, 594136,
    4603424, 6366809, 2432395, 2454455, 8215696, 1957272, 3369112, 185531,  7173032,
    5196991, 162844,  1616392, 3014001, 810149,  1652634, 4686184, 6581310, 5341501,
    3523897, 3866901, 269760,  2213111, 7404533, 1717735, 472078,  7953734, 1723600,
    6577327, 1910376, 6712985, 7276084, 8119771, 4546524, 5441381, 6144432, 7959518,
    6094090, 183443,  7403526, 1612842, 4834730, 7826001, 3919660, 8332111, 7018208,
    3937738, 1400424, 7534263, 1976782,
};

/* ntt, in place. */
static void mldsa_ntt(MldsaRing *p) {
    MldsaFieldElement *f = p->c;
    uint8_t m = 0;
    for (int len = 128; len >= 8; len /= 2) {
        for (int start = 0; start < 256; start += 2 * len) {
            m++;
            MldsaFieldElement zeta = burrow__mldsa_zetas[m];
            MldsaFieldElement *lo = f + start, *hi = f + start + len;
            for (int j = 0; j < len; j += 2) {
                MldsaFieldElement t = mldsa_mul(zeta, hi[j]);
                hi[j] = mldsa_sub(lo[j], t);
                lo[j] = mldsa_add(lo[j], t);

                t = mldsa_mul(zeta, hi[j + 1]);
                hi[j + 1] = mldsa_sub(lo[j + 1], t);
                lo[j + 1] = mldsa_add(lo[j + 1], t);
            }
        }
    }
    for (int start = 0; start < 256; start += 8) {
        m++;
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        for (int j = 0; j < 4; j++) {
            MldsaFieldElement t = mldsa_mul(zeta, f[start + 4 + j]);
            f[start + 4 + j] = mldsa_sub(f[start + j], t);
            f[start + j] = mldsa_add(f[start + j], t);
        }
    }
    for (int start = 0; start < 256; start += 4) {
        m++;
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        for (int j = 0; j < 2; j++) {
            MldsaFieldElement t = mldsa_mul(zeta, f[start + 2 + j]);
            f[start + 2 + j] = mldsa_sub(f[start + j], t);
            f[start + j] = mldsa_add(f[start + j], t);
        }
    }
    for (int start = 0; start < 256; start += 2) {
        m++;
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        MldsaFieldElement t = mldsa_mul(zeta, f[start + 1]);
        f[start + 1] = mldsa_sub(f[start], t);
        f[start] = mldsa_add(f[start], t);
    }
}

/* inverseNTT, in place. */
static void mldsa_inverse_ntt(MldsaRing *p) {
    MldsaFieldElement *f = p->c;
    uint8_t m = 255;
    for (int start = 0; start < 256; start += 2) {
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        m--;
        MldsaFieldElement t = f[start];
        f[start] = mldsa_add(t, f[start + 1]);
        f[start + 1] = mldsa_mul_sub(zeta, f[start + 1], t);
    }
    for (int start = 0; start < 256; start += 4) {
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        m--;
        for (int j = 0; j < 2; j++) {
            MldsaFieldElement t = f[start + j];
            f[start + j] = mldsa_add(t, f[start + 2 + j]);
            f[start + 2 + j] = mldsa_mul_sub(zeta, f[start + 2 + j], t);
        }
    }
    for (int start = 0; start < 256; start += 8) {
        MldsaFieldElement zeta = burrow__mldsa_zetas[m];
        m--;
        for (int j = 0; j < 4; j++) {
            MldsaFieldElement t = f[start + j];
            f[start + j] = mldsa_add(t, f[start + 4 + j]);
            f[start + 4 + j] = mldsa_mul_sub(zeta, f[start + 4 + j], t);
        }
    }
    for (int len = 8; len < 256; len *= 2) {
        for (int start = 0; start < 256; start += 2 * len) {
            MldsaFieldElement zeta = burrow__mldsa_zetas[m];
            m--;
            MldsaFieldElement *lo = f + start, *hi = f + start + len;
            for (int j = 0; j < len; j += 2) {
                MldsaFieldElement t = lo[j];
                lo[j] = mldsa_add(t, hi[j]);
                hi[j] = mldsa_mul_sub(zeta, hi[j], t);

                t = lo[j + 1];
                lo[j + 1] = mldsa_add(t, hi[j + 1]);
                hi[j + 1] = mldsa_mul_sub(zeta, hi[j + 1], t);
            }
        }
    }
    for (int i = 0; i < MLDSA_N; i++)
        f[i] = mldsa_mul(f[i], 16382); /* 16382 = 256⁻¹ * R mod q */
}

/* sampleNTT. */
static void mldsa_sample_ntt(MldsaRing *a, const Byte rho[32], Byte s, Byte r) {
    Sha3 g;
    burrow__shake128_init(&g);
    burrow__sha3_write(&g, rho, 32);
    Byte sr[2] = {s, r};
    burrow__sha3_write(&g, sr, 2);

    Byte buf[168]; /* buffered reads from G, matching the rate of SHAKE-128 */
    size_t off = sizeof buf;
    int j = 0;
    while (j < MLDSA_N) {
        if (off >= sizeof buf) {
            burrow__sha3_read(&g, buf, (Int)sizeof buf);
            off = 0;
        }
        uint32_t v = (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8 |
                     (uint32_t)buf[off + 2] << 16;
        off += 3;
        MldsaFieldElement f;
        if (!burrow__mldsa_field_to_montgomery(v & 0x7FFFFF, &f)) /* 23 bits */
            continue;
        a->c[j++] = f;
    }
}

/* coeffFromHalfByte. */
static bool mldsa_coeff_from_half_byte(Byte b, int eta, MldsaFieldElement *out) {
    if (eta == 2) {
        if (b > 14)
            return false;
        const uint32_t barrett_multiplier = 0x3334; /* ⌈2¹⁶ / 5⌉ */
        uint32_t quotient = ((uint32_t)b * barrett_multiplier) >> 16;
        uint32_t remainder = (uint32_t)b - quotient * 5;
        *out = mldsa_sub_to_mont(2, remainder);
        return true;
    }
    if (b > 8)
        return false;
    *out = mldsa_sub_to_mont(4, b);
    return true;
}

/* sampleBoundedPoly, with the 64 bytes of ρ′. */
static void mldsa_sample_bounded_poly(MldsaRing *a, const Byte rho[64], Byte r,
                                      const MldsaParameters *p) {
    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, rho, 64);
    Byte r2[2] = {r, 0}; /* IntegerToBytes(r, 2) */
    burrow__sha3_write(&h, r2, 2);

    Byte buf[136]; /* buffered reads from H, matching the rate of SHAKE-256 */
    size_t off = sizeof buf;
    int j = 0;
    for (;;) {
        if (off >= sizeof buf) {
            burrow__sha3_read(&h, buf, (Int)sizeof buf);
            off = 0;
        }
        Byte z0 = buf[off] & 0x0F;
        Byte z1 = buf[off] >> 4;
        off++;
        MldsaFieldElement coeff;
        if (mldsa_coeff_from_half_byte(z0, p->eta, &coeff))
            a->c[j++] = coeff;
        if (j >= MLDSA_N)
            break;
        if (mldsa_coeff_from_half_byte(z1, p->eta, &coeff))
            a->c[j++] = coeff;
        if (j >= MLDSA_N)
            break;
    }
}

/* sampleInBall, from the λ/4 bytes of ρ. */
static void mldsa_sample_in_ball(MldsaRing *c, const Byte *rho,
                                 const MldsaParameters *p) {
    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, rho, p->lambda / 4);
    Byte s[8];
    burrow__sha3_read(&h, s, 8);

    memset(c, 0, sizeof *c);
    for (int i = 256 - p->tau; i < 256; i++) {
        Byte j;
        burrow__sha3_read(&h, &j, 1);
        while (j > (Byte)i)
            burrow__sha3_read(&h, &j, 1);
        c->c[i] = c->c[j];
        int bit_idx = i + p->tau - 256;
        int bit = (s[bit_idx / 8] >> (bit_idx % 8)) & 1;
        c->c[j] = bit == 0 ? MLDSA_ONE : MLDSA_MINUS_ONE;
    }
}

/* ------------------------------------------------------- rounding, hints */

void burrow__mldsa_power2_round(MldsaFieldElement r, uint16_t *hi,
                                MldsaFieldElement *lo) {
    uint32_t rr = mldsa_from_mont(r);
    uint32_t r1 = rr + (1 << 12) - 1;
    r1 >>= 13;
    *lo = mldsa_sub_to_mont(rr, r1 << 13);
    *hi = (uint16_t)r1;
}

uint8_t burrow__mldsa_high_bits32(uint32_t x) {
    uint32_t r1 = (x + 127) >> 7;
    r1 = (r1 * 1025 + (1 << 21)) >> 22;
    r1 &= 0xF;
    return (uint8_t)r1;
}

uint8_t burrow__mldsa_decompose32(MldsaFieldElement r, int32_t *r0) {
    uint32_t x = mldsa_from_mont(r);
    uint8_t r1 = burrow__mldsa_high_bits32(x);
    int32_t v = (int32_t)x - (int32_t)r1 * 2 * (MLDSA_Q - 1) / 32;
    *r0 = mldsa_ct_select_le(MLDSA_Q / 2 + 1, v, v - MLDSA_Q, v);
    return r1;
}

uint8_t burrow__mldsa_use_hint32(MldsaFieldElement r, uint8_t hint) {
    const uint8_t m = 16; /* (q − 1) / (2 * γ2) */
    int32_t r0;
    uint8_t r1 = burrow__mldsa_decompose32(r, &r0);
    if (hint == 1) {
        if (r0 > 0)
            r1 = (uint8_t)((uint8_t)(r1 + 1) % m);
        else
            r1 = (uint8_t)((uint8_t)(r1 - 1) % m);
    }
    return r1;
}

static uint8_t mldsa_make_hint32(MldsaFieldElement ct0, MldsaFieldElement w,
                                 MldsaFieldElement cs2) {
    MldsaFieldElement r_plus_z = mldsa_sub(w, cs2);
    uint8_t v1 = burrow__mldsa_high_bits32(mldsa_from_mont(r_plus_z));
    uint8_t r1 = burrow__mldsa_high_bits32(mldsa_from_mont(mldsa_add(r_plus_z, ct0)));
    return (uint8_t)(mldsa_ct_byte_eq(v1, r1) ^ 1);
}

uint8_t burrow__mldsa_high_bits88(uint32_t x) {
    uint32_t r1 = (x + 127) >> 7;
    r1 = (r1 * 11275 + (1 << 23)) >> 24;
    r1 = mldsa_ct_select_eq(r1, 44, 0, r1);
    return (uint8_t)r1;
}

uint8_t burrow__mldsa_decompose88(MldsaFieldElement r, int32_t *r0) {
    uint32_t x = mldsa_from_mont(r);
    uint8_t r1 = burrow__mldsa_high_bits88(x);
    int32_t v = (int32_t)x - (int32_t)r1 * 2 * (MLDSA_Q - 1) / 88;
    *r0 = mldsa_ct_select_le(MLDSA_Q / 2 + 1, v, v - MLDSA_Q, v);
    return r1;
}

uint8_t burrow__mldsa_use_hint88(MldsaFieldElement r, uint8_t hint) {
    const uint8_t m = 44; /* (q − 1) / (2 * γ2) */
    int32_t r0;
    uint8_t r1 = burrow__mldsa_decompose88(r, &r0);
    if (hint == 1) {
        if (r0 > 0)
            r1 = r1 == m - 1 ? 0 : (uint8_t)(r1 + 1);
        else
            r1 = r1 == 0 ? (uint8_t)(m - 1) : (uint8_t)(r1 - 1);
    }
    return r1;
}

static uint8_t mldsa_make_hint88(MldsaFieldElement ct0, MldsaFieldElement w,
                                 MldsaFieldElement cs2) {
    MldsaFieldElement r_plus_z = mldsa_sub(w, cs2);
    uint8_t v1 = burrow__mldsa_high_bits88(mldsa_from_mont(r_plus_z));
    uint8_t r1 = burrow__mldsa_high_bits88(mldsa_from_mont(mldsa_add(r_plus_z, ct0)));
    return (uint8_t)(mldsa_ct_byte_eq(v1, r1) ^ 1);
}

static void mldsa_high_bits(Byte w[MLDSA_N], const MldsaRing *r,
                            const MldsaParameters *p) {
    for (int i = 0; i < MLDSA_N; i++) {
        uint32_t x = mldsa_from_mont(r->c[i]);
        w[i] = p->gamma2 == 32 ? burrow__mldsa_high_bits32(x)
                               : burrow__mldsa_high_bits88(x);
    }
}

static void mldsa_use_hint(Byte w[MLDSA_N], const MldsaRing *r, const Byte h[MLDSA_N],
                           const MldsaParameters *p) {
    for (int i = 0; i < MLDSA_N; i++)
        w[i] = p->gamma2 == 32 ? burrow__mldsa_use_hint32(r->c[i], h[i])
                               : burrow__mldsa_use_hint88(r->c[i], h[i]);
}

static int mldsa_make_hint(Byte h[MLDSA_N], const MldsaRing *ct0, const MldsaRing *w,
                           const MldsaRing *cs2, const MldsaParameters *p) {
    int count1s = 0;
    for (int i = 0; i < MLDSA_N; i++) {
        h[i] = p->gamma2 == 32 ? mldsa_make_hint32(ct0->c[i], w->c[i], cs2->c[i])
                               : mldsa_make_hint88(ct0->c[i], w->c[i], cs2->c[i]);
        count1s += h[i];
    }
    return count1s;
}

/* ---------------------------------------------------------------- packing */

/* bitPack18 and bitPack20, into the (γ₁+1) * n / 8 bytes at v. */
static void mldsa_bit_pack(Byte *v, const MldsaRing *r, const MldsaParameters *p) {
    if (p->gamma1 == 17) {
        const int32_t b = 1 << 17;
        for (int i = 0; i < MLDSA_N; i += 4) {
            uint32_t w0 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i]));
            v[0] = (Byte)(w0 << 0);
            v[1] = (Byte)(w0 >> 8);
            v[2] = (Byte)(w0 >> 16);
            uint32_t w1 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i + 1]));
            v[2] |= (Byte)(w1 << 2);
            v[3] = (Byte)(w1 >> 6);
            v[4] = (Byte)(w1 >> 14);
            uint32_t w2 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i + 2]));
            v[4] |= (Byte)(w2 << 4);
            v[5] = (Byte)(w2 >> 4);
            v[6] = (Byte)(w2 >> 12);
            uint32_t w3 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i + 3]));
            v[6] |= (Byte)(w3 << 6);
            v[7] = (Byte)(w3 >> 2);
            v[8] = (Byte)(w3 >> 10);
            v += 4 * 18 / 8;
        }
        return;
    }
    const int32_t b = 1 << 19;
    for (int i = 0; i < MLDSA_N; i += 2) {
        uint32_t w0 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i]));
        v[0] = (Byte)(w0 << 0);
        v[1] = (Byte)(w0 >> 8);
        v[2] = (Byte)(w0 >> 16);
        uint32_t w1 = (uint32_t)(b - burrow__mldsa_field_centered_mod(r->c[i + 1]));
        v[2] |= (Byte)(w1 << 4);
        v[3] = (Byte)(w1 >> 4);
        v[4] = (Byte)(w1 >> 12);
        v += 2 * 20 / 8;
    }
}

/* bitUnpack18 and bitUnpack20, from the (γ₁+1) * n / 8 bytes at v. */
static void mldsa_bit_unpack(MldsaRing *r, const Byte *v, const MldsaParameters *p) {
    if (p->gamma1 == 17) {
        const uint32_t b = 1 << 17;
        const uint32_t mask18 = (1 << 18) - 1;
        for (int i = 0; i < MLDSA_N; i += 4) {
            uint32_t w0 = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16;
            r->c[i + 0] = mldsa_sub_to_mont(b, w0 & mask18);
            uint32_t w1 =
                (uint32_t)v[2] >> 2 | (uint32_t)v[3] << 6 | (uint32_t)v[4] << 14;
            r->c[i + 1] = mldsa_sub_to_mont(b, w1 & mask18);
            uint32_t w2 =
                (uint32_t)v[4] >> 4 | (uint32_t)v[5] << 4 | (uint32_t)v[6] << 12;
            r->c[i + 2] = mldsa_sub_to_mont(b, w2 & mask18);
            uint32_t w3 =
                (uint32_t)v[6] >> 6 | (uint32_t)v[7] << 2 | (uint32_t)v[8] << 10;
            r->c[i + 3] = mldsa_sub_to_mont(b, w3 & mask18);
            v += 4 * 18 / 8;
        }
        return;
    }
    const uint32_t b = 1 << 19;
    const uint32_t mask20 = (1 << 20) - 1;
    for (int i = 0; i < MLDSA_N; i += 2) {
        uint32_t w0 = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16;
        r->c[i + 0] = mldsa_sub_to_mont(b, w0 & mask20);
        uint32_t w1 = (uint32_t)v[2] >> 4 | (uint32_t)v[3] << 4 | (uint32_t)v[4] << 12;
        r->c[i + 1] = mldsa_sub_to_mont(b, w1 & mask20);
        v += 2 * 20 / 8;
    }
}

/* w1Encode, which writes the encoding of w to h. */
static void mldsa_w1_encode(Sha3 *h, const Byte w[MLDSA_N], const MldsaParameters *p) {
    Byte buf[6 * MLDSA_N / 8];
    if (p->gamma2 == 32) {
        for (int i = 0; i < MLDSA_N; i += 2)
            buf[i / 2] = (Byte)(w[i] | w[i + 1] << 4);
        burrow__sha3_write(h, buf, 4 * MLDSA_N / 8);
        return;
    }
    for (int i = 0; i < MLDSA_N; i += 4) {
        Byte b0 = w[i], b1 = w[i + 1], b2 = w[i + 2], b3 = w[i + 3];
        buf[3 * i / 4 + 0] = (Byte)((b0 >> 0) | (b1 << 6));
        buf[3 * i / 4 + 1] = (Byte)((b1 >> 2) | (b2 << 4));
        buf[3 * i / 4 + 2] = (Byte)((b2 >> 4) | (b3 << 2));
    }
    burrow__sha3_write(h, buf, 6 * MLDSA_N / 8);
}

/* pkEncode, into the pubKeySize bytes at pk. */
static void mldsa_pk_encode(Byte *pk, const Byte rho[32], uint16_t (*t1)[MLDSA_N],
                            const MldsaParameters *p) {
    memcpy(pk, rho, 32);
    pk += 32;
    for (int r = 0; r < p->k; r++) {
        const uint16_t *w = t1[r];
        for (int i = 0; i < MLDSA_N; i += 4) {
            /* The analyzer takes p->k as zero when t1 is filled and nonzero
               here; the caller fills all k rows. */
            /* NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign) */
            uint16_t c0 = w[i], c1 = w[i + 1], c2 = w[i + 2], c3 = w[i + 3];
            pk[0] = (Byte)(c0 >> 0);
            pk[1] = (Byte)((c0 >> 8) | (c1 << 2));
            pk[2] = (Byte)((c1 >> 6) | (c2 << 4));
            pk[3] = (Byte)((c2 >> 4) | (c3 << 6));
            pk[4] = (Byte)(c3 >> 2);
            pk += 5;
        }
    }
}

/* pkDecode of the pubKeySize bytes at pk, which gives ρ as pk itself. */
static void mldsa_pk_decode(const Byte *pk, uint16_t (*t1)[MLDSA_N],
                            const MldsaParameters *p) {
    pk += 32;
    for (int r = 0; r < p->k; r++) {
        for (int i = 0; i < MLDSA_N; i += 4) {
            Byte b0 = pk[0], b1 = pk[1], b2 = pk[2], b3 = pk[3], b4 = pk[4];
            t1[r][i + 0] = (uint16_t)(b0 >> 0 | (uint16_t)(b1 & 0x03) << 8);
            t1[r][i + 1] = (uint16_t)(b1 >> 2 | (uint16_t)(b2 & 0x0F) << 6);
            t1[r][i + 2] = (uint16_t)(b2 >> 4 | (uint16_t)(b3 & 0x3F) << 4);
            t1[r][i + 3] = (uint16_t)(b3 >> 6 | (uint16_t)b4 << 2);
            pk += 5;
        }
    }
}

/* hintEncode, into the ω + k bytes at y. */
static void mldsa_hint_encode(Byte *y, Byte (*h)[MLDSA_N], const MldsaParameters *p) {
    memset(y, 0, (size_t)(p->omega + p->k));
    Byte idx = 0;
    for (int i = 0; i < p->k; i++) {
        for (int j = 0; j < MLDSA_N; j++) {
            if (h[i][j] != 0)
                y[idx++] = (Byte)j;
        }
        y[p->omega + i] = idx;
    }
}

/* hintDecode of the ω + k bytes at y, into h, which starts out zero. */
static bool mldsa_hint_decode(const Byte *y, Byte (*h)[MLDSA_N],
                              const MldsaParameters *p) {
    Byte idx = 0;
    for (int i = 0; i < p->k; i++) {
        Byte limit = y[p->omega + i];
        if (limit < idx || limit > (Byte)p->omega)
            return false;
        Byte first = idx;
        while (idx < limit) {
            if (idx > first && y[idx - 1] >= y[idx])
                return false;
            h[i][y[idx]] = 1;
            idx++;
        }
    }
    for (int i = idx; i < p->omega; i++) {
        if (y[i] != 0)
            return false;
    }
    return true;
}

static bool mldsa_coefficients_exceed_bound(const MldsaRing *w, uint32_t bound) {
    for (int i = 0; i < MLDSA_N; i++) {
        if (burrow__mldsa_field_infinity_norm(w->c[i]) >= bound)
            return true;
    }
    return false;
}

static bool mldsa_low_bits_exceed_bound(const MldsaRing *w, uint32_t bound,
                                        const MldsaParameters *p) {
    for (int i = 0; i < MLDSA_N; i++) {
        int32_t r0;
        if (p->gamma2 == 32)
            burrow__mldsa_decompose32(w->c[i], &r0);
        else
            burrow__mldsa_decompose88(w->c[i], &r0);
        if (burrow__mldsa_constant_time_abs(r0) >= bound)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------- keys */

struct MldsaPublicKey {
    Byte raw[MLDSA_MAX_PUB_KEY_SIZE];
    const MldsaParameters *p; /* NULL in Go's zero PublicKey */
    Byte tr[64];              /* public key hash */
    Alloc *alloc;             /* NULL for the public key in a private key */
};

struct MldsaPrivateKey {
    Byte seed[32];
    MldsaPublicKey pub;
    MldsaRing a[MLDSA_MAX_K * MLDSA_MAX_L];
    MldsaRing t1[MLDSA_MAX_K]; /* NTT(t₁ ⋅ 2ᵈ) */
    MldsaRing s1[MLDSA_MAX_L];
    MldsaRing s2[MLDSA_MAX_K];
    MldsaRing t0[MLDSA_MAX_K];
    Byte k[32];
    Alloc *alloc;
};

#define MLDSA_TYPE(T, name)                                                            \
    const Type burrow_type_##T = {                                                     \
        BURROW_S_INIT(name),                                                           \
        BURROW_S_INIT("crypto/mldsa"),                                                 \
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

MLDSA_TYPE(MldsaPrivateKey, "PrivateKey");
MLDSA_TYPE(MldsaPublicKey, "PublicKey");

static const Field mldsa_options_fields[] = {
    {BURROW_S_INIT("Context"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(MldsaOptions, context)},
};

const Type burrow_type_MldsaOptions = {
    BURROW_S_INIT("Options"),
    BURROW_S_INIT("crypto/mldsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(MldsaOptions),
    (uint16_t)_Alignof(MldsaOptions),
    1,
    0,
    mldsa_options_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void *mldsa_new(Alloc *a, size_t size, size_t align, Error *err) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    memset(p, 0, size);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

/* A copy of the n bytes at p as a new Slice from a, or out of memory. */
static Slice mldsa_bytes(Alloc *a, const Byte *p, Int n, Error *err) {
    Slice s = slice_make(mldsa_alloc(a), TYPE_BYTE, n, n);
    if (s.p == NULL && n > 0) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    if (n > 0)
        memcpy(s.p, p, (size_t)n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s;
}

/* computeMatrixA. */
static void mldsa_compute_matrix_a(MldsaRing *a, const Byte rho[32],
                                   const MldsaParameters *p) {
    for (int r = 0; r < p->k; r++) {
        for (int s = 0; s < p->l; s++)
            mldsa_sample_ntt(&a[r * p->l + s], rho, (Byte)s, (Byte)r);
    }
}

/* computePublicKeyHash. */
static void mldsa_compute_public_key_hash(Byte tr[64], const Byte *pk, Int n) {
    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, pk, n);
    burrow__sha3_read(&h, tr, 64);
}

/* computeT1Hat for one row: NTT(t₁ ⋅ 2ᵈ). */
static void mldsa_compute_t1_hat(MldsaRing *t1_hat, const uint16_t t1[MLDSA_N]) {
    for (int j = 0; j < MLDSA_N; j++) {
        MldsaFieldElement z;
        burrow__mldsa_field_to_montgomery((uint32_t)t1[j] << 13, &z);
        t1_hat->c[j] = z;
    }
    mldsa_ntt(t1_hat);
}

/* t = InvNTT(s2[i] + Σ A[i][j] ∘ s1[j]), row i of t = A s1 + s2. */
static void mldsa_compute_t_row(MldsaRing *t, const MldsaPrivateKey *priv, int i) {
    const MldsaParameters *p = priv->pub.p;
    *t = priv->s2[i];
    for (int j = 0; j < p->l; j++)
        mldsa_ntt_mul_add(t, &priv->a[i * p->l + j], &priv->s1[j]);
    mldsa_inverse_ntt(t);
}

/* newPrivateKey, into priv, which is zero. */
static void mldsa_new_private_key_into(MldsaPrivateKey *priv, const Byte seed[32],
                                       const MldsaParameters *p) {
    int k = p->k, l = p->l;
    priv->pub.p = p;
    memcpy(priv->seed, seed, 32);

    Sha3 xi;
    burrow__shake256_init(&xi);
    burrow__sha3_write(&xi, seed, 32);
    Byte kl[2] = {(Byte)k, (Byte)l};
    burrow__sha3_write(&xi, kl, 2);
    Byte rho[32], rho_s[64];
    burrow__sha3_read(&xi, rho, 32);
    burrow__sha3_read(&xi, rho_s, 64);
    burrow__sha3_read(&xi, priv->k, 32);

    mldsa_compute_matrix_a(priv->a, rho, p);
    for (int r = 0; r < l; r++) {
        mldsa_sample_bounded_poly(&priv->s1[r], rho_s, (Byte)r, p);
        mldsa_ntt(&priv->s1[r]);
    }
    for (int r = 0; r < k; r++) {
        mldsa_sample_bounded_poly(&priv->s2[r], rho_s, (Byte)(l + r), p);
        mldsa_ntt(&priv->s2[r]);
    }

    uint16_t t1[MLDSA_MAX_K][MLDSA_N];
    for (int i = 0; i < k; i++) {
        MldsaRing t;
        mldsa_compute_t_row(&t, priv, i);
        for (int j = 0; j < MLDSA_N; j++)
            burrow__mldsa_power2_round(t.c[j], &t1[i][j], &priv->t0[i].c[j]);
        mldsa_ntt(&priv->t0[i]);
    }

    mldsa_pk_encode(priv->pub.raw, rho, t1, p);
    mldsa_compute_public_key_hash(priv->pub.tr, priv->pub.raw, p->public_key_size);
    for (int i = 0; i < k; i++)
        mldsa_compute_t1_hat(&priv->t1[i], t1[i]);
    memset(rho_s, 0, sizeof rho_s);
}

/* computeMessageHash. */
static bool mldsa_compute_message_hash(Byte mu[64], const Byte tr[64], Slice msg,
                                       Str context) {
    if (context.len > 255)
        return false;
    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, tr, 64);
    Byte head[2] = {0, (Byte)context.len}; /* ML-DSA / HashML-DSA domain separator */
    burrow__sha3_write(&h, head, 2);
    burrow__sha3_write(&h, context.p, context.len);
    burrow__sha3_write(&h, msg.p, msg.len);
    burrow__sha3_read(&h, mu, 64);
    return true;
}

/* ---------------------------------------------------------------- signing */

/* What one attempt at a signature needs, which is too much for the stack. */
typedef struct MldsaSignScratch {
    MldsaRing y[MLDSA_MAX_L], y_hat[MLDSA_MAX_L], z[MLDSA_MAX_L], cs1[MLDSA_MAX_L];
    MldsaRing w[MLDSA_MAX_K], cs2[MLDSA_MAX_K], ct0[MLDSA_MAX_K];
    MldsaRing c, r0;
    Byte h[MLDSA_MAX_K][MLDSA_N];
    Byte w1[MLDSA_N];
    Byte v[(MLDSA_MAX_GAMMA1 + 1) * MLDSA_N / 8];
    Byte ch[MLDSA_MAX_LAMBDA / 4];
} MldsaSignScratch;

/* One turn of the loop of signInternal: NULL and the signature in sig, or why
 * it was rejected. */
static const char *mldsa_sign_attempt(const MldsaPrivateKey *priv, const Byte mu[64],
                                      const Byte nonce[64], uint16_t *kappa,
                                      MldsaSignScratch *s, Byte *sig) {
    const MldsaParameters *p = priv->pub.p;
    int k = p->k, l = p->l;
    uint32_t beta = (uint32_t)(p->tau * p->eta);
    uint32_t gamma1 = (uint32_t)1 << p->gamma1;
    uint32_t gamma1_beta = gamma1 - beta;
    uint32_t gamma2 = (MLDSA_Q - 1) / (uint32_t)p->gamma2;
    uint32_t gamma2_beta = gamma2 - beta;
    Int vlen = (Int)(p->gamma1 + 1) * MLDSA_N / 8;

    Sha3 h;
    for (int r = 0; r < l; r++) {
        Byte counter[2] = {(Byte)*kappa, (Byte)(*kappa >> 8)};
        (*kappa)++;
        burrow__shake256_init(&h);
        burrow__sha3_write(&h, nonce, 64);
        burrow__sha3_write(&h, counter, 2);
        burrow__sha3_read(&h, s->v, vlen);
        mldsa_bit_unpack(&s->y[r], s->v, p);
    }

    for (int i = 0; i < l; i++) {
        s->y_hat[i] = s->y[i];
        mldsa_ntt(&s->y_hat[i]);
    }
    for (int i = 0; i < k; i++) {
        memset(&s->w[i], 0, sizeof s->w[i]);
        for (int j = 0; j < l; j++)
            mldsa_ntt_mul_add(&s->w[i], &priv->a[i * l + j], &s->y_hat[j]);
        mldsa_inverse_ntt(&s->w[i]);
    }

    burrow__shake256_init(&h);
    burrow__sha3_write(&h, mu, 64);
    for (int i = 0; i < k; i++) {
        mldsa_high_bits(s->w1, &s->w[i], p);
        mldsa_w1_encode(&h, s->w1, p);
    }
    burrow__sha3_read(&h, s->ch, p->lambda / 4);

    mldsa_sample_in_ball(&s->c, s->ch, p);
    mldsa_ntt(&s->c);

    for (int i = 0; i < l; i++) {
        mldsa_ntt_mul(&s->cs1[i], &s->c, &priv->s1[i]);
        mldsa_inverse_ntt(&s->cs1[i]);
    }
    for (int i = 0; i < k; i++) {
        mldsa_ntt_mul(&s->cs2[i], &s->c, &priv->s2[i]);
        mldsa_inverse_ntt(&s->cs2[i]);
    }

    for (int i = 0; i < l; i++) {
        mldsa_poly_add(&s->z[i], &s->y[i], &s->cs1[i]);
        if (mldsa_coefficients_exceed_bound(&s->z[i], gamma1_beta))
            return "z";
    }

    for (int i = 0; i < k; i++) {
        mldsa_poly_sub(&s->r0, &s->w[i], &s->cs2[i]);
        if (mldsa_low_bits_exceed_bound(&s->r0, gamma2_beta, p))
            return "r0";
    }

    for (int i = 0; i < k; i++) {
        mldsa_ntt_mul(&s->ct0[i], &s->c, &priv->t0[i]);
        mldsa_inverse_ntt(&s->ct0[i]);
        if (mldsa_coefficients_exceed_bound(&s->ct0[i], gamma2))
            return "ct0";
    }

    int count1s = 0;
    for (int i = 0; i < k; i++)
        count1s += mldsa_make_hint(s->h[i], &s->ct0[i], &s->w[i], &s->cs2[i], p);
    if (count1s > p->omega)
        return "h";

    /* sigEncode */
    memcpy(sig, s->ch, (size_t)(p->lambda / 4));
    sig += p->lambda / 4;
    for (int i = 0; i < l; i++) {
        mldsa_bit_pack(sig, &s->z[i], p);
        sig += vlen;
    }
    mldsa_hint_encode(sig, s->h, p);
    return NULL;
}

/* signInternal, into the signature size bytes at sig. False when the scratch
 * space cannot be had. */
static bool mldsa_sign_internal(const MldsaPrivateKey *priv, const Byte mu[64],
                                const Byte random[32], const MldsaRejectionHook *hook,
                                Byte *sig) {
    Alloc *heap = heap_allocator();
    MldsaSignScratch *s = mem_alloc(heap, sizeof *s, _Alignof(MldsaSignScratch));
    if (s == NULL)
        return false;

    Byte nonce[64];
    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, priv->k, 32);
    burrow__sha3_write(&h, random, 32);
    burrow__sha3_write(&h, mu, 64);
    burrow__sha3_read(&h, nonce, 64);

    uint16_t kappa = 0;
    for (;;) {
        const char *reason = mldsa_sign_attempt(priv, mu, nonce, &kappa, s, sig);
        if (reason == NULL)
            break;
        if (hook != NULL && hook->fn != NULL)
            hook->fn(hook->env, str_from_cstr(reason));
    }

    memset(s, 0, sizeof *s);
    memset(nonce, 0, sizeof nonce);
    mem_free(heap, s, sizeof *s, _Alignof(MldsaSignScratch));
    return true;
}

/* signInternal as a new Slice from a. */
static Slice mldsa_sign_to_slice(const MldsaPrivateKey *priv, Alloc *a,
                                 const Byte mu[64], const Byte random[32],
                                 const MldsaRejectionHook *hook, Error *err) {
    Byte sig[MLDSA_MAX_SIG_SIZE];
    if (!mldsa_sign_internal(priv, mu, random, hook, sig)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return mldsa_bytes(a, sig, priv->pub.p->signature_size, err);
}

/* Sign, SignDeterministic and TestingOnlySignWithRandom, with random NULL for
 * the system's randomness. */
static Slice mldsa_sign_message(const MldsaPrivateKey *priv, Alloc *a, Slice msg,
                                Str context, const Byte *random, Error *err) {
    Byte mu[64];
    if (!mldsa_compute_message_hash(mu, priv->pub.tr, msg, context)) {
        mldsa_set_error(err, "mldsa: context too long");
        return slice_nil(TYPE_BYTE);
    }
    Byte rnd[32];
    if (random != NULL)
        memcpy(rnd, random, 32);
    else
        burrow__crypto_rand_system(slice_from(rnd, 32, 32, TYPE_BYTE));
    Slice sig = mldsa_sign_to_slice(priv, a, mu, rnd, NULL, err);
    memset(rnd, 0, sizeof rnd);
    return sig;
}

/* SignExternalMu and the like, with random NULL for the system's randomness. */
static Slice mldsa_sign_mu(const MldsaPrivateKey *priv, Alloc *a, Slice mu,
                           const Byte *random, const MldsaRejectionHook *hook,
                           Error *err) {
    if (mu.len != 64) {
        mldsa_set_error(err, "mldsa: invalid message hash length");
        return slice_nil(TYPE_BYTE);
    }
    Byte rnd[32];
    if (random != NULL)
        memcpy(rnd, random, 32);
    else
        burrow__crypto_rand_system(slice_from(rnd, 32, 32, TYPE_BYTE));
    Slice sig = mldsa_sign_to_slice(priv, a, mu.p, rnd, hook, err);
    memset(rnd, 0, sizeof rnd);
    return sig;
}

Slice burrow__mldsa_sign_external_mu_deterministic(const MldsaPrivateKey *sk, Alloc *a,
                                                   Slice mu,
                                                   const MldsaRejectionHook *hook,
                                                   Error *err) {
    static const Byte zero[32];
    return mldsa_sign_mu(sk, a, mu, zero, hook, err);
}

Slice burrow__mldsa_testing_only_sign_with_random(const MldsaPrivateKey *sk, Alloc *a,
                                                  Slice msg, Str context, Slice random,
                                                  Error *err) {
    if (context.len > 255) {
        mldsa_set_error(err, "mldsa: context too long");
        return slice_nil(TYPE_BYTE);
    }
    if (random.len != 32) {
        mldsa_set_error(err, "mldsa: invalid random length");
        return slice_nil(TYPE_BYTE);
    }
    return mldsa_sign_message(sk, a, msg, context, random.p, err);
}

Slice burrow__mldsa_testing_only_sign_external_mu_with_random(const MldsaPrivateKey *sk,
                                                              Alloc *a, Slice mu,
                                                              Slice random,
                                                              Error *err) {
    if (mu.len != 64) {
        mldsa_set_error(err, "mldsa: invalid message hash length");
        return slice_nil(TYPE_BYTE);
    }
    if (random.len != 32) {
        mldsa_set_error(err, "mldsa: invalid random length");
        return slice_nil(TYPE_BYTE);
    }
    return mldsa_sign_mu(sk, a, mu, random.p, NULL, err);
}

/* -------------------------------------------------------------- verifying */

/* What verifying needs, which is too much for the stack. */
typedef struct MldsaVerifyScratch {
    MldsaRing a[MLDSA_MAX_K * MLDSA_MAX_L];
    MldsaRing z[MLDSA_MAX_L], z_hat[MLDSA_MAX_L];
    MldsaRing c, w, ct1;
    uint16_t t1[MLDSA_MAX_K][MLDSA_N];
    Byte h[MLDSA_MAX_K][MLDSA_N];
    Byte w1[MLDSA_N];
} MldsaVerifyScratch;

/* verifyInternal, from the point where the signature has the right length. */
static Error mldsa_verify_scratch(const MldsaPublicKey *pub, const Byte mu[64],
                                  const Byte *sig, MldsaVerifyScratch *s) {
    const MldsaParameters *p = pub->p;
    int k = p->k, l = p->l;
    uint32_t beta = (uint32_t)(p->tau * p->eta);
    uint32_t gamma1 = (uint32_t)1 << p->gamma1;
    uint32_t gamma1_beta = gamma1 - beta;
    Int zlen = (Int)(p->gamma1 + 1) * MLDSA_N / 8;

    mldsa_pk_decode(pub->raw, s->t1, p);
    mldsa_compute_matrix_a(s->a, pub->raw, p);

    /* sigDecode */
    const Byte *ch = sig;
    const Byte *v = sig + p->lambda / 4;
    for (int i = 0; i < l; i++) {
        mldsa_bit_unpack(&s->z[i], v, p);
        v += zlen;
    }
    if (!mldsa_hint_decode(v, s->h, p))
        return mldsa_error("mldsa: invalid signature encoding");

    mldsa_sample_in_ball(&s->c, ch, p);
    mldsa_ntt(&s->c);

    for (int i = 0; i < l; i++) {
        s->z_hat[i] = s->z[i];
        mldsa_ntt(&s->z_hat[i]);
    }

    Sha3 h;
    burrow__shake256_init(&h);
    burrow__sha3_write(&h, mu, 64);
    for (int i = 0; i < k; i++) {
        memset(&s->w, 0, sizeof s->w);
        for (int j = 0; j < l; j++)
            mldsa_ntt_mul_add(&s->w, &s->a[i * l + j], &s->z_hat[j]);
        MldsaRing t1_hat;
        mldsa_compute_t1_hat(&t1_hat, s->t1[i]);
        mldsa_ntt_mul(&s->ct1, &s->c, &t1_hat);
        mldsa_poly_sub(&s->w, &s->w, &s->ct1);
        mldsa_inverse_ntt(&s->w);
        mldsa_use_hint(s->w1, &s->w, s->h[i], p);
        mldsa_w1_encode(&h, s->w1, p);
    }
    Byte computed_ch[MLDSA_MAX_LAMBDA / 4];
    burrow__sha3_read(&h, computed_ch, p->lambda / 4);

    for (int i = 0; i < l; i++) {
        if (mldsa_coefficients_exceed_bound(&s->z[i], gamma1_beta))
            return mldsa_error("mldsa: invalid signature");
    }
    if (memcmp(ch, computed_ch, (size_t)(p->lambda / 4)) != 0)
        return mldsa_error("mldsa: invalid signature");
    return BURROW_NO_ERROR;
}

/* verifyInternal. */
static Error mldsa_verify_internal(const MldsaPublicKey *pub, const Byte mu[64],
                                   Slice sig) {
    if (sig.len != pub->p->signature_size)
        return mldsa_error("mldsa: invalid signature length");
    Alloc *heap = heap_allocator();
    MldsaVerifyScratch *s = mem_alloc(heap, sizeof *s, _Alignof(MldsaVerifyScratch));
    if (s == NULL)
        return burrow_err_out_of_memory;
    Error e = mldsa_verify_scratch(pub, mu, sig.p, s);
    mem_free(heap, s, sizeof *s, _Alignof(MldsaVerifyScratch));
    return e;
}

Error burrow__mldsa_verify_external_mu(const MldsaPublicKey *pk, Slice mu, Slice sig) {
    if (mu.len != 64)
        return mldsa_error("mldsa: invalid message hash length");
    return mldsa_verify_internal(pk, mu.p, sig);
}

/* ----------------------------------------------------- semi-expanded keys */

static int mldsa_bits_len(uint32_t x) {
    int n = 0;
    for (; x != 0; x >>= 1)
        n++;
    return n;
}

/* semiExpandedPrivKeySize. */
static Int mldsa_semi_expanded_size(const MldsaParameters *p) {
    Int eta_bitlen = mldsa_bits_len((uint32_t)p->eta) + 1;
    return 32 + 32 + 64 + (Int)p->l * MLDSA_N * eta_bitlen / 8 +
           (Int)p->k * MLDSA_N * eta_bitlen / 8 + (Int)p->k * MLDSA_N * 13 / 8;
}

/* bitPackSlow, into the n * bitlen / 8 bytes at v, which it returns the end of. */
static Byte *mldsa_bit_pack_slow(Byte *v, const MldsaRing *r, int a, int b) {
    int bitlen = mldsa_bits_len((uint32_t)(a + b));
    uint32_t acc = 0;
    int acc_bits = 0;
    for (int i = 0; i < MLDSA_N; i++) {
        int32_t w = (int32_t)b - burrow__mldsa_field_centered_mod(r->c[i]);
        acc |= (uint32_t)w << acc_bits;
        acc_bits += bitlen;
        while (acc_bits >= 8) {
            *v++ = (Byte)acc;
            acc >>= 8;
            acc_bits -= 8;
        }
    }
    if (acc_bits > 0)
        *v++ = (Byte)acc;
    return v;
}

/* bitUnpackSlow of the n * bitlen / 8 bytes at v. False for a coefficient out
 * of range. */
static bool mldsa_bit_unpack_slow(MldsaRing *r, const Byte *v, int a, int b) {
    int bitlen = mldsa_bits_len((uint32_t)(a + b));
    uint32_t mask = ((uint32_t)1 << bitlen) - 1;
    uint32_t max_value = (uint32_t)(a + b);
    uint32_t acc = 0;
    int acc_bits = 0;
    for (int i = 0; i < MLDSA_N; i++) {
        while (acc_bits < bitlen) {
            acc |= (uint32_t)*v++ << acc_bits;
            acc_bits += 8;
        }
        uint32_t w = acc & mask;
        if (w > max_value)
            return false;
        r->c[i] = mldsa_sub_to_mont((uint32_t)b, w);
        acc >>= bitlen;
        acc_bits -= bitlen;
    }
    return true;
}

enum { MLDSA_T0_BOUND = 1 << (13 - 1) }; /* 2^(d-1) */

MldsaPrivateKey *
burrow__mldsa_testing_only_new_private_key_from_semi_expanded(Alloc *a, Slice sk,
                                                              Error *err) {
    const MldsaParameters *p;
    if (sk.len == mldsa_semi_expanded_size(&mldsa_params44)) {
        p = &mldsa_params44;
    } else if (sk.len == mldsa_semi_expanded_size(&mldsa_params65)) {
        p = &mldsa_params65;
    } else if (sk.len == mldsa_semi_expanded_size(&mldsa_params87)) {
        p = &mldsa_params87;
    } else {
        mldsa_set_error(err, "mldsa: invalid semi-expanded private key size");
        return NULL;
    }
    int k = p->k, l = p->l, eta = p->eta;
    a = mldsa_alloc(a);
    MldsaPrivateKey *priv = mldsa_new(a, sizeof *priv, _Alignof(MldsaPrivateKey), err);
    if (priv == NULL)
        return NULL;
    priv->alloc = a;
    priv->pub.p = p;

    /* skDecode */
    const Byte *v = sk.p;
    const Byte *rho = v;
    v += 32;
    memcpy(priv->k, v, 32);
    v += 32;
    memcpy(priv->pub.tr, v, 64);
    v += 64;
    Int eta_len = MLDSA_N * mldsa_bits_len((uint32_t)eta * 2) / 8;
    const char *bad = NULL;
    for (int i = 0; i < l && bad == NULL; i++, v += eta_len) {
        if (!mldsa_bit_unpack_slow(&priv->s1[i], v, eta, eta))
            bad = "mldsa: coefficient out of range";
    }
    for (int i = 0; i < k && bad == NULL; i++, v += eta_len) {
        if (!mldsa_bit_unpack_slow(&priv->s2[i], v, eta, eta))
            bad = "mldsa: coefficient out of range";
    }
    for (int i = 0; i < k && bad == NULL; i++, v += MLDSA_N * 13 / 8) {
        if (!mldsa_bit_unpack_slow(&priv->t0[i], v, MLDSA_T0_BOUND - 1, MLDSA_T0_BOUND))
            bad = "mldsa: coefficient out of range";
    }
    if (bad != NULL)
        goto fail;

    mldsa_compute_matrix_a(priv->a, rho, p);
    for (int r = 0; r < l; r++)
        mldsa_ntt(&priv->s1[r]);
    for (int r = 0; r < k; r++)
        mldsa_ntt(&priv->s2[r]);

    burrow__crypto_rand_system(slice_from(priv->seed, 32, 32, TYPE_BYTE));

    /* t0 is still as it was encoded here, and has its NTT taken at the end. */
    uint16_t t1[MLDSA_MAX_K][MLDSA_N];
    for (int i = 0; i < k && bad == NULL; i++) {
        MldsaRing t;
        mldsa_compute_t_row(&t, priv, i);
        for (int j = 0; j < MLDSA_N; j++) {
            MldsaFieldElement r0;
            burrow__mldsa_power2_round(t.c[j], &t1[i][j], &r0);
            if (r0 != priv->t0[i].c[j]) {
                bad = "mldsa: semi-expanded private key inconsistent with t0";
                break;
            }
        }
    }
    if (bad != NULL)
        goto fail;

    mldsa_pk_encode(priv->pub.raw, rho, t1, p);
    Byte tr[64];
    mldsa_compute_public_key_hash(tr, priv->pub.raw, p->public_key_size);
    if (memcmp(tr, priv->pub.tr, 64) != 0) {
        bad = "mldsa: semi-expanded private key inconsistent with public key hash";
        goto fail;
    }
    for (int i = 0; i < k; i++)
        mldsa_compute_t1_hat(&priv->t1[i], t1[i]);
    for (int i = 0; i < k; i++)
        mldsa_ntt(&priv->t0[i]);
    return priv;

fail:
    mldsa_private_key_free(priv);
    mldsa_set_error(err, bad);
    return NULL;
}

Slice burrow__mldsa_testing_only_private_key_semi_expanded_bytes(
    const MldsaPrivateKey *sk, Alloc *a) {
    if (sk == NULL || sk->pub.p == NULL)
        return slice_nil(TYPE_BYTE);
    const MldsaParameters *p = sk->pub.p;
    Int size = mldsa_semi_expanded_size(p);
    Slice out = slice_make(mldsa_alloc(a), TYPE_BYTE, size, size);
    if (out.p == NULL)
        return slice_nil(TYPE_BYTE);
    Byte *v = out.p;
    memcpy(v, sk->pub.raw, 32); /* ρ */
    v += 32;
    memcpy(v, sk->k, 32); /* K */
    v += 32;
    memcpy(v, sk->pub.tr, 64); /* tr */
    v += 64;
    MldsaRing r;
    for (int i = 0; i < p->l; i++) {
        r = sk->s1[i];
        mldsa_inverse_ntt(&r);
        v = mldsa_bit_pack_slow(v, &r, p->eta, p->eta);
    }
    for (int i = 0; i < p->k; i++) {
        r = sk->s2[i];
        mldsa_inverse_ntt(&r);
        v = mldsa_bit_pack_slow(v, &r, p->eta, p->eta);
    }
    for (int i = 0; i < p->k; i++) {
        r = sk->t0[i];
        mldsa_inverse_ntt(&r);
        v = mldsa_bit_pack_slow(v, &r, MLDSA_T0_BOUND - 1, MLDSA_T0_BOUND);
    }
    memset(&r, 0, sizeof r);
    return out;
}

/* -------------------------------------------------------------- self test */

Error burrow__mldsa_fips140_cast(const MldsaRejectionHook *hook) {
    static const Byte seed[32] = {
        0x5c, 0x62, 0x4f, 0xcc, 0x18, 0x62, 0x45, 0x24, 0x52, 0xd0, 0xc6,
        0x65, 0x84, 0x0d, 0x82, 0x37, 0xf4, 0x31, 0x08, 0xe5, 0x49, 0x9e,
        0xdc, 0xdc, 0x10, 0x8f, 0xbc, 0x49, 0xd5, 0x96, 0xe4, 0xb7,
    };
    static const Byte mu[64] = {
        0x2a, 0xd1, 0xc7, 0x2b, 0xb0, 0xfc, 0xbe, 0x28, 0x09, 0x9c, 0xe8, 0xbd, 0x2e,
        0xd8, 0x36, 0xdf, 0xeb, 0xe5, 0x20, 0xaa, 0xd3, 0x8f, 0xba, 0xc6, 0x6e, 0xf7,
        0x85, 0xa3, 0xcf, 0xb1, 0x0f, 0xb4, 0x19, 0x32, 0x7f, 0xa5, 0x78, 0x18, 0xee,
        0x4e, 0x37, 0x18, 0xda, 0x4b, 0xe4, 0x8d, 0x24, 0xb5, 0x9a, 0x20, 0x8f, 0x88,
        0x07, 0x27, 0x1f, 0xdb, 0x7e, 0xda, 0x6e, 0x60, 0x14, 0x1b, 0xd2, 0x63,
    };
    static const Byte sk_hash[32] = {
        0x29, 0x37, 0x49, 0x51, 0xcb, 0x2b, 0xc3, 0xcd, 0xa7, 0x31, 0x5c,
        0xe7, 0xf0, 0xab, 0x99, 0xc7, 0xd2, 0xd6, 0x52, 0x92, 0xe6, 0xc5,
        0x15, 0x6e, 0x8a, 0xa6, 0x2a, 0xc1, 0x4b, 0x14, 0x12, 0xaf,
    };
    static const Byte sig_hash[32] = {
        0xdc, 0xc7, 0x1a, 0x42, 0x1b, 0xc6, 0xff, 0xaf, 0xb7, 0xdf, 0x0c,
        0x7f, 0x6d, 0x01, 0x8a, 0x19, 0xad, 0xa1, 0x54, 0xd1, 0xe2, 0xee,
        0x36, 0x0e, 0xd5, 0x33, 0xce, 0xcd, 0x5d, 0xc9, 0x80, 0xad,
    };
    static const Byte random[32];

    Alloc *heap = heap_allocator();
    Error e = BURROW_NO_ERROR;
    MldsaPrivateKey *priv =
        mldsa_new(heap, sizeof *priv, _Alignof(MldsaPrivateKey), &e);
    if (priv == NULL)
        return e;
    priv->alloc = heap;
    mldsa_new_private_key_into(priv, seed, &mldsa_params44);

    Slice sk = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(priv, heap);
    if (sk.p == NULL) {
        mldsa_private_key_free(priv);
        return burrow_err_out_of_memory;
    }
    Sha256Sum256Ret got = sha256_sum256(sk);
    mem_free(heap, sk.p, (size_t)sk.cap, 1);
    if (memcmp(got.a, sk_hash, 32) != 0) {
        mldsa_private_key_free(priv);
        return mldsa_error("unexpected private key hash");
    }

    Byte sig[MLDSA_MLDSA44_SIGNATURE_SIZE];
    if (!mldsa_sign_internal(priv, mu, random, hook, sig)) {
        mldsa_private_key_free(priv);
        return burrow_err_out_of_memory;
    }
    got = sha256_sum256(slice_from(sig, sizeof sig, sizeof sig, TYPE_BYTE));
    if (memcmp(got.a, sig_hash, 32) != 0) {
        mldsa_private_key_free(priv);
        return mldsa_error("unexpected signature hash");
    }
    e = mldsa_verify_internal(&priv->pub, mu,
                              slice_from(sig, sizeof sig, sizeof sig, TYPE_BYTE));
    mldsa_private_key_free(priv);
    return e;
}

/* ------------------------------------------------------------- public API */

CryptoHash mldsa_options_hash_func(const MldsaOptions *opts) {
    (void)opts;
    return 0;
}

static CryptoHash mldsa_options_vt_hash_func(void *self) {
    return mldsa_options_hash_func(self);
}

static const CryptoSignerOptsVT mldsa_options_vt = {TYPE_MLDSA_OPTIONS,
                                                    mldsa_options_vt_hash_func};

CryptoSignerOpts mldsa_options_as_signer_opts(const MldsaOptions *opts) {
    CryptoSignerOpts out = {&mldsa_options_vt, (void *)(uintptr_t)opts};
    return out;
}

/* A new private key for p from seed, from a. */
static MldsaPrivateKey *mldsa_private_key_from_seed(const MldsaParameters *p, Alloc *a,
                                                    const Byte seed[32], Error *err) {
    a = mldsa_alloc(a);
    MldsaPrivateKey *priv = mldsa_new(a, sizeof *priv, _Alignof(MldsaPrivateKey), err);
    if (priv == NULL)
        return NULL;
    priv->alloc = a;
    mldsa_new_private_key_into(priv, seed, p);
    return priv;
}

MldsaPrivateKey *mldsa_generate_key(Alloc *a, const MldsaParameters *params,
                                    Error *err) {
    if (!mldsa_valid_params(params)) {
        mldsa_set_error(err, "mldsa: invalid parameters");
        return NULL;
    }
    Byte seed[32];
    burrow__crypto_rand_system(slice_from(seed, 32, 32, TYPE_BYTE));
    MldsaPrivateKey *priv = mldsa_private_key_from_seed(params, a, seed, err);
    memset(seed, 0, sizeof seed);
    return priv;
}

MldsaPrivateKey *mldsa_new_private_key(Alloc *a, const MldsaParameters *params,
                                       Slice seed, Error *err) {
    if (!mldsa_valid_params(params)) {
        mldsa_set_error(err, "mldsa: invalid parameters");
        return NULL;
    }
    if (seed.len != MLDSA_PRIVATE_KEY_SIZE) {
        mldsa_set_error(err, "mldsa: invalid seed length");
        return NULL;
    }
    return mldsa_private_key_from_seed(params, a, seed.p, err);
}

void mldsa_private_key_free(MldsaPrivateKey *sk) {
    if (sk == NULL)
        return;
    Alloc *a = sk->alloc;
    memset(sk, 0, sizeof *sk);
    mem_free(a, sk, sizeof *sk, _Alignof(MldsaPrivateKey));
}

static const Byte mldsa_zero_seed[32];

Slice mldsa_private_key_bytes(const MldsaPrivateKey *sk, Alloc *a) {
    return mldsa_bytes(a, sk != NULL ? sk->seed : mldsa_zero_seed, 32, NULL);
}

bool mldsa_private_key_equal(const MldsaPrivateKey *sk, CryptoPrivateKey x) {
    if (x.t != TYPE_MLDSA_PRIVATE_KEY || x.data == NULL)
        return false;
    const MldsaPrivateKey *other = x.data;
    const MldsaParameters *p = sk != NULL ? sk->pub.p : NULL;
    const Byte *seed = sk != NULL ? sk->seed : mldsa_zero_seed;
    if (p != other->pub.p)
        return false;
    return subtle_constant_time_compare(
               slice_from((void *)(uintptr_t)seed, 32, 32, TYPE_BYTE),
               slice_from((void *)(uintptr_t)other->seed, 32, 32, TYPE_BYTE)) == 1;
}

const MldsaPublicKey *mldsa_private_key_public_key(const MldsaPrivateKey *sk) {
    return sk != NULL ? &sk->pub : NULL;
}

CryptoPublicKey mldsa_private_key_public(const MldsaPrivateKey *sk) {
    return BURROW_ANY(TYPE_MLDSA_PUBLIC_KEY,
                      (uintptr_t)mldsa_private_key_public_key(sk));
}

/* The switch of Sign and SignDeterministic, with deterministic for the second. */
static Slice mldsa_sign_with_opts(const MldsaPrivateKey *sk, Alloc *a, Slice message,
                                  CryptoSignerOpts opts, bool deterministic,
                                  Error *err) {
    static const Byte zero[32];
    if (sk == NULL || sk->pub.p == NULL) {
        mldsa_set_error(err, "mldsa: zero private key");
        return slice_nil(TYPE_BYTE);
    }
    CryptoHash hash = opts.vt != NULL ? opts.vt->hash_func(opts.data) : 0;
    const Byte *random = deterministic ? zero : NULL;
    if (hash == 0) {
        Str context = BURROW_STR_EMPTY;
        if (opts.vt != NULL && opts.vt->self_type == TYPE_MLDSA_OPTIONS &&
            opts.data != NULL)
            context = ((const MldsaOptions *)opts.data)->context;
        return mldsa_sign_message(sk, a, message, context, random, err);
    }
    if (hash == CRYPTO_MLDSA_MU)
        return mldsa_sign_mu(sk, a, message, random, NULL, err);
    mldsa_set_error(err, "mldsa: invalid SignerOpts");
    return slice_nil(TYPE_BYTE);
}

Slice mldsa_private_key_sign(const MldsaPrivateKey *sk, Alloc *a, IoReader rand,
                             Slice message, CryptoSignerOpts opts, Error *err) {
    (void)rand;
    return mldsa_sign_with_opts(sk, a, message, opts, false, err);
}

Slice mldsa_private_key_sign_deterministic(const MldsaPrivateKey *sk, Alloc *a,
                                           Slice message, CryptoSignerOpts opts,
                                           Error *err) {
    return mldsa_sign_with_opts(sk, a, message, opts, true, err);
}

static CryptoPublicKey mldsa_signer_public(void *self) {
    return mldsa_private_key_public(self);
}

static Slice mldsa_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                               CryptoSignerOpts opts, Error *err) {
    return mldsa_private_key_sign(self, a, rand, digest, opts, err);
}

static const CryptoSignerVT mldsa_signer_vt = {TYPE_MLDSA_PRIVATE_KEY,
                                               mldsa_signer_public, mldsa_signer_sign};

CryptoSigner mldsa_private_key_signer(const MldsaPrivateKey *sk) {
    CryptoSigner out = {&mldsa_signer_vt, (void *)(uintptr_t)sk};
    return out;
}

MldsaPublicKey *mldsa_new_public_key(Alloc *a, const MldsaParameters *params,
                                     Slice encoding, Error *err) {
    if (!mldsa_valid_params(params)) {
        mldsa_set_error(err, "mldsa: invalid parameters");
        return NULL;
    }
    if (encoding.len != params->public_key_size) {
        mldsa_set_error(err, "mldsa: invalid public key length");
        return NULL;
    }
    a = mldsa_alloc(a);
    MldsaPublicKey *pub = mldsa_new(a, sizeof *pub, _Alignof(MldsaPublicKey), err);
    if (pub == NULL)
        return NULL;
    pub->alloc = a;
    pub->p = params;
    memcpy(pub->raw, encoding.p, (size_t)encoding.len);
    mldsa_compute_public_key_hash(pub->tr, pub->raw, encoding.len);
    return pub;
}

void mldsa_public_key_free(MldsaPublicKey *pk) {
    if (pk == NULL || pk->alloc == NULL)
        return;
    Alloc *a = pk->alloc;
    mem_free(a, pk, sizeof *pk, _Alignof(MldsaPublicKey));
}

/* pubKeySize(pk.p), which is 32 for the zero key. */
static Int mldsa_raw_size(const MldsaPublicKey *pk) {
    return pk->p != NULL ? pk->p->public_key_size : 32;
}

Slice mldsa_public_key_bytes(const MldsaPublicKey *pk, Alloc *a) {
    if (pk == NULL)
        return slice_nil(TYPE_BYTE);
    return mldsa_bytes(a, pk->raw, mldsa_raw_size(pk), NULL);
}

bool mldsa_public_key_equal(const MldsaPublicKey *pk, CryptoPublicKey x) {
    if (x.t != TYPE_MLDSA_PUBLIC_KEY || x.data == NULL || pk == NULL)
        return false;
    const MldsaPublicKey *other = x.data;
    if (pk->p != other->p)
        return false;
    Int size = mldsa_raw_size(pk);
    return subtle_constant_time_compare(
               slice_from((void *)(uintptr_t)pk->raw, size, size, TYPE_BYTE),
               slice_from((void *)(uintptr_t)other->raw, size, size, TYPE_BYTE)) == 1;
}

const MldsaParameters *mldsa_public_key_parameters(const MldsaPublicKey *pk) {
    if (pk->p == NULL)
        panic_str(BURROW_S("mldsa: internal error: unknown parameters"));
    return pk->p;
}

Error mldsa_verify(const MldsaPublicKey *pk, Slice message, Slice signature,
                   const MldsaOptions *opts) {
    if (pk == NULL)
        return mldsa_error("mldsa: nil public key");
    if (pk->p == NULL)
        return mldsa_error("mldsa: zero public key");
    Str context = opts != NULL ? opts->context : BURROW_STR_EMPTY;
    Byte mu[64];
    if (!mldsa_compute_message_hash(mu, pk->tr, message, context))
        return mldsa_error("mldsa: context too long");
    return mldsa_verify_internal(pk, mu, signature);
}

/* ----------------------------------------------------------- test helpers */

MldsaPrivateKey *burrow__mldsa_new_zero_private_key(Alloc *a) {
    a = mldsa_alloc(a);
    MldsaPrivateKey *priv = mldsa_new(a, sizeof *priv, _Alignof(MldsaPrivateKey), NULL);
    if (priv != NULL)
        priv->alloc = a;
    return priv;
}

MldsaPublicKey *burrow__mldsa_new_zero_public_key(Alloc *a) {
    a = mldsa_alloc(a);
    MldsaPublicKey *pub = mldsa_new(a, sizeof *pub, _Alignof(MldsaPublicKey), NULL);
    if (pub != NULL)
        pub->alloc = a;
    return pub;
}

bool burrow__mldsa_public_key_identical(const MldsaPublicKey *a,
                                        const MldsaPublicKey *b) {
    return a->p == b->p && memcmp(a->raw, b->raw, sizeof a->raw) == 0 &&
           memcmp(a->tr, b->tr, sizeof a->tr) == 0;
}
