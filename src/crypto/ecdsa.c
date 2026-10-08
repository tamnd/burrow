/* Derived from Go's src/crypto/ecdsa/ecdsa.go and ecdsa_legacy.go, and
 * src/crypto/internal/fips140/ecdsa/ecdsa.go and hmacdrbg.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/ecdsa.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha512.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/math/rand/v2.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include "bigmod.h"
#include "crypto_internal.h"
#include "cryptobyte.h"
#include "ecdsa_internal.h"
#include "nistec.h"
#include "rand_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ types */

const Type burrow_type_EcdsaPublicKey = {
    BURROW_S_INIT("PublicKey"),
    BURROW_S_INIT("crypto/ecdsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(EcdsaPublicKey),
    (uint16_t)_Alignof(EcdsaPublicKey),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_EcdsaPrivateKey = {
    BURROW_S_INIT("PrivateKey"),
    BURROW_S_INIT("crypto/ecdsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(EcdsaPrivateKey),
    (uint16_t)_Alignof(EcdsaPrivateKey),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* A key this package makes is one block from a, the key and then the integers
 * it points at, whose words come from a too. */
typedef struct EcdsaPublicBlock {
    EcdsaPublicKey k;
    BigInt x, y;
    Alloc *a;
} EcdsaPublicBlock;

typedef struct EcdsaPrivateBlock {
    EcdsaPrivateKey k;
    BigInt x, y, d;
    Alloc *a;
} EcdsaPrivateBlock;

/* ---------------------------------------------------------------- helpers */

static void ecdsa_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
}

static Alloc *ecdsa_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

static Slice ecdsa_bytes(const uint8_t *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* A copy of the n bytes at p, from a. */
static Slice ecdsa_clone(Alloc *a, const uint8_t *p, Int n, Error *err) {
    Slice s = slice_make(ecdsa_alloc(a), TYPE_BYTE, n, n);
    if (s.p == NULL && n > 0) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    if (n > 0)
        memcpy(s.p, p, (size_t)n);
    return s;
}

static bool ecdsa_same_curve(EllipticCurve x, EllipticCurve y) {
    return x.vt == y.vt && x.data == y.data;
}

/* drbg.ReadWithReader: the system generator for the default reader, which is
 * what drbg.Read is outside FIPS mode, and io.ReadFull for anything else. */
static bool ecdsa_read(IoReader r, Slice b, Error *err) {
    if (burrow__crypto_rand_is_default_reader(r)) {
        burrow__crypto_rand_system(b);
        return true;
    }
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, b, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

/* What the functions that take a rand do with a nil one, before
 * rand.CustomReader. */
static IoReader ecdsa_reader(IoReader r) {
    if (r.vt == NULL)
        r = burrow__crypto_rand_nil_reader();
    return burrow__crypto_rand_custom_reader(r);
}

/* ------------------------------------------------------------ FIPS curves */

/* Enough words for a number as long as P-521's order. */
#define ECDSA_LIMBS ((NISTEC_MAX_ELEMENT_BYTES * 8 + BIGMOD_W - 1) / BIGMOD_W)

struct EcdsaFipsCurve {
    const NistecCurve *nist;
    const uint8_t *order;
    Int size;    /* N.Size() */
    Int bit_len; /* N.BitLen() */
    BigmodModulus *n;
    uint8_t n_minus2[NISTEC_MAX_ELEMENT_BYTES];
};

static const uint8_t ecdsa_p224_order[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x16, 0xa2, 0xe0, 0xb8, 0xf0, 0x3e, 0x13, 0xdd, 0x29, 0x45, 0x5c, 0x5c, 0x2a, 0x3d};
static const uint8_t ecdsa_p256_order[] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
    0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
static const uint8_t ecdsa_p384_order[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf, 0x58, 0x1a, 0x0d, 0xb2,
    0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73};
static const uint8_t ecdsa_p521_order[] = {
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xfa, 0x51, 0x86, 0x87, 0x83, 0xbf, 0x2f, 0x96, 0x6b,
    0x7f, 0xcc, 0x01, 0x48, 0xf7, 0x09, 0xa5, 0xd0, 0x3b, 0xb5, 0xc9, 0xb8, 0x89, 0x9c,
    0x47, 0xae, 0xbb, 0x6f, 0xb7, 0x1e, 0x91, 0x38, 0x64, 0x09};

static EcdsaFipsCurve ecdsa_curves[4];
static SyncOnce ecdsa_once;

/* Nat.Bytes(m) into out, which has room for the size of c's order. */
static void ecdsa_nat_bytes(const BigmodNat *x, const EcdsaFipsCurve *c, uint8_t *out) {
    Int i = c->size;
    memset(out, 0, (size_t)i);
    for (Int k = 0; k < x->len; k++) {
        Uint limb = x->limbs[k];
        for (int j = 0; j < BIGMOD_S; j++) {
            i--;
            if (i < 0) {
                if (limb == 0)
                    break;
                panic_str(BURROW_S("bigmod: modulus is smaller than nat"));
            }
            out[i] = (uint8_t)limb;
            limb >>= 8;
        }
    }
}

/* precomputeParams. */
static void ecdsa_precompute(EcdsaFipsCurve *c, const NistecCurve *nist,
                             const uint8_t *order, Int n) {
    Error err = BURROW_NO_ERROR;
    c->nist = nist;
    c->order = order;
    c->n = bigmod_new_modulus(NULL, ecdsa_bytes(order, n), &err);
    if (c->n == NULL)
        panic_str(error_text(err));
    c->size = bigmod_modulus_size(c->n);
    c->bit_len = bigmod_modulus_bit_len(c->n);

    Uint tb[ECDSA_LIMBS], mb[ECDSA_LIMBS];
    BigmodNat two, m2;
    bigmod_nat_init_buf(&two, tb, ECDSA_LIMBS);
    bigmod_nat_init_buf(&m2, mb, ECDSA_LIMBS);
    static const uint8_t two_bytes[] = {2};
    (void)bigmod_nat_set_bytes(&two, ecdsa_bytes(two_bytes, 1), c->n, NULL);
    bigmod_nat_sub(bigmod_nat_expand_for(&m2, c->n), &two, c->n);
    ecdsa_nat_bytes(&m2, c, c->n_minus2);
    bigmod_nat_free(&two);
    bigmod_nat_free(&m2);
}

static void ecdsa_init(void *env) {
    (void)env;
    ecdsa_precompute(&ecdsa_curves[0], &burrow__nistec_p224, ecdsa_p224_order,
                     (Int)sizeof ecdsa_p224_order);
    ecdsa_precompute(&ecdsa_curves[1], &burrow__nistec_p256, ecdsa_p256_order,
                     (Int)sizeof ecdsa_p256_order);
    ecdsa_precompute(&ecdsa_curves[2], &burrow__nistec_p384, ecdsa_p384_order,
                     (Int)sizeof ecdsa_p384_order);
    ecdsa_precompute(&ecdsa_curves[3], &burrow__nistec_p521, ecdsa_p521_order,
                     (Int)sizeof ecdsa_p521_order);
}

static const EcdsaFipsCurve *ecdsa_fips(int i) {
    sync_once_do(&ecdsa_once, BURROW_FN(Func, ecdsa_init, NULL));
    return &ecdsa_curves[i];
}

const EcdsaFipsCurve *burrow__ecdsa_fips_p224(void) {
    return ecdsa_fips(0);
}

const EcdsaFipsCurve *burrow__ecdsa_fips_p256(void) {
    return ecdsa_fips(1);
}

const EcdsaFipsCurve *burrow__ecdsa_fips_p384(void) {
    return ecdsa_fips(2);
}

const EcdsaFipsCurve *burrow__ecdsa_fips_p521(void) {
    return ecdsa_fips(3);
}

Int burrow__ecdsa_fips_size(const EcdsaFipsCurve *c) {
    return c->size;
}

/* The FIPS curve for one whose Params are one of the four NIST curves', which
 * is how GenerateKey, SignASN1 and VerifyASN1 choose. NULL for any other. */
static const EcdsaFipsCurve *ecdsa_fips_for_params(EllipticCurve c) {
    EllipticCurveParams *p = elliptic_curve_params(c);
    if (p == elliptic_curve_params(elliptic_p224()))
        return burrow__ecdsa_fips_p224();
    if (p == elliptic_curve_params(elliptic_p256()))
        return burrow__ecdsa_fips_p256();
    if (p == elliptic_curve_params(elliptic_p384()))
        return burrow__ecdsa_fips_p384();
    if (p == elliptic_curve_params(elliptic_p521()))
        return burrow__ecdsa_fips_p521();
    return NULL;
}

/* The same for a curve that is one of the four, which is how Bytes and the
 * Parse functions choose. */
static const EcdsaFipsCurve *ecdsa_fips_for_curve(EllipticCurve c) {
    if (ecdsa_same_curve(c, elliptic_p224()))
        return burrow__ecdsa_fips_p224();
    if (ecdsa_same_curve(c, elliptic_p256()))
        return burrow__ecdsa_fips_p256();
    if (ecdsa_same_curve(c, elliptic_p384()))
        return burrow__ecdsa_fips_p384();
    if (ecdsa_same_curve(c, elliptic_p521()))
        return burrow__ecdsa_fips_p521();
    return NULL;
}

/* ------------------------------------------------------- FIPS arithmetic */

/* rightShift, in place on the n bytes at b. */
static void ecdsa_right_shift(uint8_t *b, Int n, Int shift) {
    if (shift <= 0 || shift >= 8)
        panic_str(BURROW_S("ecdsa: internal error: shift can only be by 1 to 7 bits"));
    for (Int i = n - 1; i >= 0; i--) {
        b[i] = (uint8_t)(b[i] >> shift);
        if (i > 0)
            b[i] = (uint8_t)(b[i] | (b[i - 1] << (8 - shift)));
    }
}

/* hashToNat: e = the leftmost bits of hash, as many as the order has, reduced
 * modulo it. */
static void ecdsa_hash_to_nat(const EcdsaFipsCurve *c, BigmodNat *e, Slice hash) {
    uint8_t buf[NISTEC_MAX_ELEMENT_BYTES];
    if (hash.len >= c->size) {
        memcpy(buf, hash.p, (size_t)c->size);
        hash = ecdsa_bytes(buf, c->size);
        Int excess = c->size * 8 - c->bit_len;
        if (excess > 0)
            ecdsa_right_shift(buf, c->size, excess);
    }
    if (bigmod_nat_set_overflowing_bytes(e, hash, c->n, NULL) == NULL)
        panic_str(BURROW_S("ecdsa: internal error: truncated hash is too long"));
}

void burrow__ecdsa_bits2octets(const EcdsaFipsCurve *c, Slice hash, uint8_t *out) {
    Uint eb[ECDSA_LIMBS];
    BigmodNat e;
    bigmod_nat_init_buf(&e, eb, ECDSA_LIMBS);
    ecdsa_hash_to_nat(c, &e, hash);
    ecdsa_nat_bytes(&e, c, out);
    bigmod_nat_free(&e);
}

/* inverse: kInv = k^(N-2) mod N, which is k⁻¹ since N is prime. Go has an
 * assembly inversion for P-256 on 64 bit machines, which gives the same
 * number. */
static void ecdsa_inverse(const EcdsaFipsCurve *c, BigmodNat *k_inv,
                          const BigmodNat *k) {
    bigmod_nat_exp(k_inv, k, ecdsa_bytes(c->n_minus2, c->size), c->n);
}

/* randomPoint, with the point as it is. */
static bool ecdsa_random_point(const EcdsaFipsCurve *c, EcdsaFill gen, uint8_t *k_out,
                               NistecPoint *p, Int *loops, Error *err) {
    Int size = c->size;
    uint8_t b[NISTEC_MAX_ELEMENT_BYTES];
    Uint kb[ECDSA_LIMBS];
    for (;;) {
        Slice bs = ecdsa_bytes(b, size);
        if (!gen.f(gen.env, bs, err))
            return false;

        /* The order of P-521 is 521 bits long, so the top seven bits of the 66
         * bytes have to go. The others are a whole number of bytes. */
        Int excess = size * 8 - c->bit_len;
        if (excess > 0) {
            if (c->nist != &burrow__nistec_p521)
                panic_str(
                    BURROW_S("ecdsa: internal error: unexpectedly masking off bits"));
            ecdsa_right_shift(b, size, excess);
        }

        /* Rejection sampling: anything not less than the order goes round
         * again, which for these curves is very unlikely. */
        BigmodNat k;
        bigmod_nat_init_buf(&k, kb, ECDSA_LIMBS);
        bool ok = bigmod_nat_set_bytes(&k, bs, c->n, NULL) != NULL &&
                  bigmod_nat_is_zero(&k) == 0;
        if (ok)
            ecdsa_nat_bytes(&k, c, k_out);
        bigmod_nat_free(&k);
        memset(b, 0, sizeof b);
        memset(kb, 0, sizeof kb);
        if (ok) {
            Error e = BURROW_NO_ERROR;
            if (c->nist->scalar_base_mult(c->nist->point_new(p),
                                          ecdsa_bytes(k_out, size), &e) == NULL) {
                BURROW_OUT(err, e);
                return false;
            }
            return true;
        }
        if (loops != NULL)
            (*loops)++;
    }
}

bool burrow__ecdsa_random_point(const EcdsaFipsCurve *c, EcdsaFill gen, uint8_t *k_out,
                                uint8_t *p_out, Int *loops, Error *err) {
    NistecPoint p;
    if (!ecdsa_random_point(c, gen, k_out, &p, loops, err))
        return false;
    (void)c->nist->bytes(&p, p_out);
    return true;
}

/* ------------------------------------------------------------- HMAC_DRBG */

#define ECDSA_RESEED_INTERVAL ((uint64_t)1 << 48)
#define ECDSA_MAX_REQUEST_SIZE ((1 << 19) / 8)

/* hmacDRBG. Everything, the HMACs included, comes from a, an arena that lives
 * as long as the DRBG. */
struct EcdsaDrbg {
    Alloc *a;
    HashNewFunc h;
    Hash hk;
    uint8_t *v;
    uint8_t *k;
    Int size;
    uint64_t reseed_counter;
};

static Hash ecdsa_drbg_hmac(EcdsaDrbg *d, Slice key) {
    Hash h = hmac_new(d->a, d->h, key);
    if (h.vt == NULL)
        panic_str(BURROW_S("crypto/ecdsa: out of memory"));
    return h;
}

static void ecdsa_write(Hash h, Slice p) {
    (void)hash_write(h, p, NULL);
}

static void ecdsa_write_byte(Hash h, uint8_t b) {
    ecdsa_write(h, ecdsa_bytes(&b, 1));
}

/* h.Sum(dst[:0]), for a dst of d->size bytes. */
static void ecdsa_sum_into(EcdsaDrbg *d, Hash h, uint8_t *dst) {
    Slice s = hash_sum(d->a, h, slice_from(dst, 0, d->size, TYPE_BYTE));
    if (s.p != dst)
        memcpy(dst, s.p, (size_t)d->size);
}

/* pad000: zeros up to the next multiple of the block size. */
static void ecdsa_pad000(Hash h, Int written) {
    static const uint8_t zeros[256];
    Int block = hash_block_size(h);
    Int rem = written % block;
    if (rem == 0)
        return;
    for (Int n = block - rem; n > 0;) {
        Int m = n < (Int)sizeof zeros ? n : (Int)sizeof zeros;
        ecdsa_write(h, ecdsa_bytes(zeros, m));
        n -= m;
    }
}

/* V || sep || entropy || nonce || the personalization string, which is the
 * one Slice in parts when it is plain and each of them padded to a block when
 * it is block aligned. */
static void ecdsa_drbg_absorb(EcdsaDrbg *d, Hash h, uint8_t sep, Slice entropy,
                              Slice nonce, const Slice *parts, Int nparts,
                              bool aligned) {
    ecdsa_write(h, ecdsa_bytes(d->v, d->size));
    ecdsa_write_byte(h, sep);
    ecdsa_write(h, entropy);
    ecdsa_write(h, nonce);
    if (!aligned) {
        for (Int i = 0; i < nparts; i++)
            ecdsa_write(h, parts[i]);
        return;
    }
    Int l = d->size + 1 + entropy.len + nonce.len;
    for (Int i = 0; i < nparts; i++) {
        ecdsa_pad000(h, l);
        ecdsa_write(h, parts[i]);
        l = parts[i].len;
    }
}

/* newDRBG. */
static EcdsaDrbg *ecdsa_new_drbg(Alloc *a, HashNewFunc hf, Slice entropy, Slice nonce,
                                 const Slice *parts, Int nparts, bool aligned) {
    EcdsaDrbg *d = BURROW_NEW(a, EcdsaDrbg);
    if (d == NULL)
        panic_str(BURROW_S("crypto/ecdsa: out of memory"));
    d->a = a;
    d->h = hf;
    Hash probe = hf(a);
    if (probe.vt == NULL)
        panic_str(BURROW_S("crypto/ecdsa: out of memory"));
    d->size = hash_size(probe);
    d->v = mem_alloc(a, (size_t)d->size, 1);
    d->k = mem_alloc(a, (size_t)d->size, 1);
    if (d->v == NULL || d->k == NULL)
        panic_str(BURROW_S("crypto/ecdsa: out of memory"));
    memset(d->v, 0x01, (size_t)d->size);
    memset(d->k, 0, (size_t)d->size);

    Slice key = ecdsa_bytes(d->k, d->size);
    Hash h = ecdsa_drbg_hmac(d, key);
    ecdsa_drbg_absorb(d, h, 0x00, entropy, nonce, parts, nparts, aligned);
    ecdsa_sum_into(d, h, d->k);
    h = ecdsa_drbg_hmac(d, key);
    ecdsa_write(h, ecdsa_bytes(d->v, d->size));
    ecdsa_sum_into(d, h, d->v);

    hash_reset(h);
    ecdsa_drbg_absorb(d, h, 0x01, entropy, nonce, parts, nparts, aligned);
    ecdsa_sum_into(d, h, d->k);
    h = ecdsa_drbg_hmac(d, key);
    ecdsa_write(h, ecdsa_bytes(d->v, d->size));
    ecdsa_sum_into(d, h, d->v);
    d->hk = h;
    d->reseed_counter = 1;
    return d;
}

EcdsaDrbg *burrow__ecdsa_new_drbg(Alloc *a, HashNewFunc h, Slice entropy, Slice nonce,
                                  Slice pers) {
    return ecdsa_new_drbg(a, h, entropy, nonce, &pers, 1, false);
}

void burrow__ecdsa_drbg_generate(EcdsaDrbg *d, Slice out) {
    if (out.len > ECDSA_MAX_REQUEST_SIZE)
        panic_str(BURROW_S("ecdsa: internal error: request size exceeds maximum"));
    /* Step 1 of SP 800-90A Rev. 1, Section 10.1.2.5. */
    if (d->reseed_counter > ECDSA_RESEED_INTERVAL)
        panic_str(BURROW_S("ecdsa: reseed interval exceeded"));

    /* Step 4. */
    uint8_t *o = out.p;
    Int tlen = 0;
    while (tlen < out.len) {
        hash_reset(d->hk);
        ecdsa_write(d->hk, ecdsa_bytes(d->v, d->size));
        ecdsa_sum_into(d, d->hk, d->v);
        Int n = out.len - tlen < d->size ? out.len - tlen : d->size;
        memcpy(o + tlen, d->v, (size_t)n);
        tlen += n;
    }

    /* Step 6, HMAC_DRBG_Update with no data. */
    hash_reset(d->hk);
    ecdsa_write(d->hk, ecdsa_bytes(d->v, d->size));
    ecdsa_write_byte(d->hk, 0x00);
    ecdsa_sum_into(d, d->hk, d->k);
    d->hk = ecdsa_drbg_hmac(d, ecdsa_bytes(d->k, d->size));
    ecdsa_write(d->hk, ecdsa_bytes(d->v, d->size));
    ecdsa_sum_into(d, d->hk, d->v);

    /* Step 7. */
    d->reseed_counter++;
}

static bool ecdsa_drbg_fill(void *env, Slice b, Error *err) {
    (void)err;
    burrow__ecdsa_drbg_generate(env, b);
    return true;
}

static bool ecdsa_reader_fill(void *env, Slice b, Error *err) {
    return ecdsa_read(*(IoReader *)env, b, err);
}

/* ------------------------------------------------------- FIPS operations */

/* signGeneric. */
bool burrow__ecdsa_sign_with_drbg(const EcdsaFipsCurve *c, Slice d, EcdsaDrbg *drbg,
                                  Slice hash, uint8_t *r_out, uint8_t *s_out,
                                  Error *err) {
    uint8_t kb[NISTEC_MAX_ELEMENT_BYTES];
    NistecPoint big_r;
    if (!ecdsa_random_point(c, BURROW_FN(EcdsaFill, ecdsa_drbg_fill, drbg), kb, &big_r,
                            NULL, err))
        return false;

    Uint kw[ECDSA_LIMBS], iw[ECDSA_LIMBS], rw[ECDSA_LIMBS], ew[ECDSA_LIMBS],
        sw[ECDSA_LIMBS];
    BigmodNat k, k_inv, r, e, s;
    bigmod_nat_init_buf(&k, kw, ECDSA_LIMBS);
    bigmod_nat_init_buf(&k_inv, iw, ECDSA_LIMBS);
    bigmod_nat_init_buf(&r, rw, ECDSA_LIMBS);
    bigmod_nat_init_buf(&e, ew, ECDSA_LIMBS);
    bigmod_nat_init_buf(&s, sw, ECDSA_LIMBS);
    bool ok = false;

    (void)bigmod_nat_set_bytes(&k, ecdsa_bytes(kb, c->size), c->n, NULL);
    ecdsa_inverse(c, &k_inv, &k);

    uint8_t rx[NISTEC_MAX_ELEMENT_BYTES];
    Error e2 = BURROW_NO_ERROR;
    if (c->nist->bytes_x(&big_r, rx, &e2) == NULL) {
        BURROW_OUT(err, e2);
        goto done;
    }
    if (bigmod_nat_set_overflowing_bytes(&r, ecdsa_bytes(rx, c->nist->element_bytes),
                                         c->n, err) == NULL)
        goto done;
    if (bigmod_nat_is_zero(&r) == 1) {
        ecdsa_set_error(err, "ecdsa: internal error: r is zero");
        goto done;
    }

    ecdsa_hash_to_nat(c, &e, hash);

    if (bigmod_nat_set_bytes(&s, d, c->n, err) == NULL)
        goto done;
    bigmod_nat_mul(&s, &r, c->n);
    bigmod_nat_add(&s, &e, c->n);
    bigmod_nat_mul(&s, &k_inv, c->n);

    /* Again, the chance of this happening is cryptographically
     * negligible. */
    if (bigmod_nat_is_zero(&s) == 1) {
        ecdsa_set_error(err, "ecdsa: internal error: s is zero");
        goto done;
    }

    ecdsa_nat_bytes(&r, c, r_out);
    ecdsa_nat_bytes(&s, c, s_out);
    ok = true;

done:
    bigmod_nat_free(&k);
    bigmod_nat_free(&k_inv);
    bigmod_nat_free(&r);
    bigmod_nat_free(&e);
    bigmod_nat_free(&s);
    memset(kb, 0, sizeof kb);
    memset(kw, 0, sizeof kw);
    memset(iw, 0, sizeof iw);
    memset(sw, 0, sizeof sw);
    memset(&big_r, 0, sizeof big_r);
    return ok;
}

/* NewPublicKey, which only has to check Q, since a FIPS key here is its
 * encoding. */
static bool ecdsa_fips_check_public(const EcdsaFipsCurve *c, Slice q, Error *err) {
    const uint8_t *p = q.p;
    if (q.len < 1 || p[0] == 0) {
        ecdsa_set_error(err, "ecdsa: invalid public key encoding");
        return false;
    }
    NistecPoint pt;
    Error e = BURROW_NO_ERROR;
    if (c->nist->set_bytes(c->nist->point_new(&pt), q, &e) == NULL) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

/* NewPrivateKey, the same for D and Q. */
static bool ecdsa_fips_check_private(const EcdsaFipsCurve *c, Slice d, Slice q,
                                     Error *err) {
    if (!ecdsa_fips_check_public(c, q, err))
        return false;
    if (d.len != c->size) {
        ecdsa_set_error(err, "ecdsa: invalid private key length");
        return false;
    }
    Uint w[ECDSA_LIMBS];
    BigmodNat n;
    bigmod_nat_init_buf(&n, w, ECDSA_LIMBS);
    bool ok = bigmod_nat_set_bytes(&n, d, c->n, err) != NULL;
    if (ok && bigmod_nat_is_zero(&n) == 1) {
        ecdsa_set_error(err, "ecdsa: private key is zero");
        ok = false;
    }
    bigmod_nat_free(&n);
    memset(w, 0, sizeof w);
    return ok;
}

/* Sign: the hedged signature, with a nonce from an HMAC_DRBG over SHA-512
 * seeded with random bytes, the key and the hash. */
static bool ecdsa_fips_sign(const EcdsaFipsCurve *c, Slice d, IoReader rand, Slice hash,
                            uint8_t *r, uint8_t *s, Error *err) {
    if (hash.len == 0) {
        ecdsa_set_error(err, "ecdsa: hash cannot be empty");
        return false;
    }

    /* The nonce comes from a DRBG with random bytes as its entropy, and the
     * key and the hash as its personalization string, padded to blocks as
     * draft-irtf-cfrg-det-sigs-with-noise-04 says. */
    uint8_t z[NISTEC_MAX_ELEMENT_BYTES] = {0};
    if (!ecdsa_read(rand, ecdsa_bytes(z, d.len), err))
        return false;
    uint8_t h[NISTEC_MAX_ELEMENT_BYTES];
    burrow__ecdsa_bits2octets(c, hash, h);
    Slice parts[2] = {d, ecdsa_bytes(h, c->size)};

    Arena ar;
    arena_init(&ar, NULL, 0);
    EcdsaDrbg *drbg =
        ecdsa_new_drbg(arena_allocator(&ar), sha512_new, ecdsa_bytes(z, d.len),
                       slice_nil(TYPE_BYTE), parts, 2, true);
    bool ok = burrow__ecdsa_sign_with_drbg(c, d, drbg, hash, r, s, err);
    memset(drbg->v, 0, (size_t)drbg->size);
    memset(drbg->k, 0, (size_t)drbg->size);
    arena_free(&ar);
    memset(z, 0, sizeof z);
    return ok;
}

/* SignDeterministic: the nonce of RFC 6979, from an HMAC_DRBG over hf seeded
 * with the key and the hash. */
static bool ecdsa_fips_sign_deterministic(const EcdsaFipsCurve *c, HashNewFunc hf,
                                          Slice d, Slice hash, uint8_t *r, uint8_t *s,
                                          Error *err) {
    if (hash.len == 0) {
        ecdsa_set_error(err, "ecdsa: hash cannot be empty");
        return false;
    }
    uint8_t h[NISTEC_MAX_ELEMENT_BYTES];
    burrow__ecdsa_bits2octets(c, hash, h);

    Arena ar;
    arena_init(&ar, NULL, 0);
    EcdsaDrbg *drbg = ecdsa_new_drbg(arena_allocator(&ar), hf, d,
                                     ecdsa_bytes(h, c->size), NULL, 0, false);
    bool ok = burrow__ecdsa_sign_with_drbg(c, d, drbg, hash, r, s, err);
    memset(drbg->v, 0, (size_t)drbg->size);
    memset(drbg->k, 0, (size_t)drbg->size);
    arena_free(&ar);
    return ok;
}

/* Verify and verifyGeneric. */
static bool ecdsa_fips_verify(const EcdsaFipsCurve *c, Slice q, Slice hash, Slice sig_r,
                              Slice sig_s, Error *err) {
    if (hash.len == 0) {
        ecdsa_set_error(err, "ecdsa: hash cannot be empty");
        return false;
    }

    const NistecCurve *nist = c->nist;
    NistecPoint big_q, p1;
    Error e2 = BURROW_NO_ERROR;
    if (nist->set_bytes(nist->point_new(&big_q), q, &e2) == NULL) {
        BURROW_OUT(err, e2);
        return false;
    }

    Uint rw[ECDSA_LIMBS], sw[ECDSA_LIMBS], ew[ECDSA_LIMBS], ww[ECDSA_LIMBS],
        vw[ECDSA_LIMBS];
    BigmodNat r, s, e, w, v;
    bigmod_nat_init_buf(&r, rw, ECDSA_LIMBS);
    bigmod_nat_init_buf(&s, sw, ECDSA_LIMBS);
    bigmod_nat_init_buf(&e, ew, ECDSA_LIMBS);
    bigmod_nat_init_buf(&w, ww, ECDSA_LIMBS);
    bigmod_nat_init_buf(&v, vw, ECDSA_LIMBS);
    bool ok = false;
    uint8_t buf[NISTEC_MAX_ELEMENT_BYTES];

    if (bigmod_nat_set_bytes(&r, sig_r, c->n, err) == NULL)
        goto done;
    if (bigmod_nat_is_zero(&r) == 1) {
        ecdsa_set_error(err, "ecdsa: invalid signature: r is zero");
        goto done;
    }
    if (bigmod_nat_set_bytes(&s, sig_s, c->n, err) == NULL)
        goto done;
    if (bigmod_nat_is_zero(&s) == 1) {
        ecdsa_set_error(err, "ecdsa: invalid signature: s is zero");
        goto done;
    }

    ecdsa_hash_to_nat(c, &e, hash);

    /* w = s⁻¹ */
    ecdsa_inverse(c, &w, &s);

    /* p₁ = [e * w]G */
    ecdsa_nat_bytes(bigmod_nat_mul(&e, &w, c->n), c, buf);
    e2 = BURROW_NO_ERROR;
    if (nist->scalar_base_mult(nist->point_new(&p1), ecdsa_bytes(buf, c->size), &e2) ==
        NULL) {
        BURROW_OUT(err, e2);
        goto done;
    }
    /* p₂ = [r * w]Q */
    ecdsa_nat_bytes(bigmod_nat_mul(&w, &r, c->n), c, buf);
    if (nist->scalar_mult(&big_q, &big_q, ecdsa_bytes(buf, c->size), &e2) == NULL) {
        BURROW_OUT(err, e2);
        goto done;
    }
    /* BytesX returns an error for the point at infinity. */
    if (nist->bytes_x(nist->add(&p1, &p1, &big_q), buf, &e2) == NULL) {
        BURROW_OUT(err, e2);
        goto done;
    }
    if (bigmod_nat_set_overflowing_bytes(&v, ecdsa_bytes(buf, nist->element_bytes),
                                         c->n, err) == NULL)
        goto done;
    if (bigmod_nat_equal(&v, &r) != 1) {
        ecdsa_set_error(err, "ecdsa: signature did not verify");
        goto done;
    }
    ok = true;

done:
    bigmod_nat_free(&r);
    bigmod_nat_free(&s);
    bigmod_nat_free(&e);
    bigmod_nat_free(&w);
    bigmod_nat_free(&v);
    return ok;
}

/* -------------------------------------------------------------- the keys */

static EcdsaPublicBlock *ecdsa_new_public_block(Alloc *a, EllipticCurve curve,
                                                Error *err) {
    a = ecdsa_alloc(a);
    EcdsaPublicBlock *b = BURROW_NEW(a, EcdsaPublicBlock);
    if (b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    b->x = BIG_INT(a);
    b->y = BIG_INT(a);
    b->a = a;
    b->k.curve = curve;
    b->k.x = &b->x;
    b->k.y = &b->y;
    return b;
}

static EcdsaPrivateBlock *ecdsa_new_private_block(Alloc *a, EllipticCurve curve,
                                                  Error *err) {
    a = ecdsa_alloc(a);
    EcdsaPrivateBlock *b = BURROW_NEW(a, EcdsaPrivateBlock);
    if (b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    b->x = BIG_INT(a);
    b->y = BIG_INT(a);
    b->d = BIG_INT(a);
    b->a = a;
    b->k.public_key.curve = curve;
    b->k.public_key.x = &b->x;
    b->k.public_key.y = &b->y;
    b->k.d = &b->d;
    return b;
}

void ecdsa_public_key_free(EcdsaPublicKey *k) {
    if (k == NULL)
        return;
    EcdsaPublicBlock *b = (EcdsaPublicBlock *)k;
    Alloc *a = b->a;
    big_int_free(&b->x);
    big_int_free(&b->y);
    mem_free(a, b, sizeof *b, _Alignof(EcdsaPublicBlock));
}

void ecdsa_private_key_free(EcdsaPrivateKey *k) {
    if (k == NULL)
        return;
    EcdsaPrivateBlock *b = (EcdsaPrivateBlock *)k;
    Alloc *a = b->a;
    if (b->d.abs.p != NULL)
        memset(b->d.abs.p, 0, (size_t)b->d.abs.cap * sizeof *b->d.abs.p);
    big_int_free(&b->x);
    big_int_free(&b->y);
    big_int_free(&b->d);
    mem_free(a, b, sizeof *b, _Alignof(EcdsaPrivateBlock));
}

/* pointToAffine, into x and y. */
static bool ecdsa_point_to_affine(EllipticCurve curve, Slice p, BigInt *x, BigInt *y,
                                  Error *err) {
    const uint8_t *q = p.p;
    if (p.len == 1 && q[0] == 0) {
        /* This is not a valid point, but it is what was always returned. */
        ecdsa_set_error(err, "ecdsa: public key point is the infinity");
        return false;
    }
    Int n = (elliptic_curve_params(curve)->bit_size + 7) / 8;
    big_int_set_bytes(x, ecdsa_bytes(q + 1, n));
    big_int_set_bytes(y, ecdsa_bytes(q + 1 + n, p.len - 1 - n));
    return true;
}

/* pointFromAffine: the uncompressed encoding of (x, y) in buf, which has room
 * for NISTEC_MAX_POINT_BYTES, and its length. The curve is one of the four
 * NIST curves, by its params. */
static Int ecdsa_point_from_affine(EllipticCurve curve, const BigInt *x,
                                   const BigInt *y, uint8_t *buf, Error *err) {
    Int bit_size = elliptic_curve_params(curve)->bit_size;
    /* Reject values that would not get correctly encoded. */
    if (big_int_sign(x) < 0 || big_int_sign(y) < 0) {
        ecdsa_set_error(err, "negative coordinate");
        return 0;
    }
    if (big_int_bit_len(x) > bit_size || big_int_bit_len(y) > bit_size) {
        ecdsa_set_error(err, "overflowing coordinate");
        return 0;
    }
    /* Encode the coordinates and let SetBytes reject invalid points. */
    Int n = (bit_size + 7) / 8;
    buf[0] = 4; /* uncompressed point */
    big_int_fill_bytes(x, slice_from(buf + 1, n, n, TYPE_BYTE));
    big_int_fill_bytes(y, slice_from(buf + 1 + n, n, n, TYPE_BYTE));
    return 1 + 2 * n;
}

/* publicKeyToFIPS: pub's encoding in q, checked, and its length. */
static Int ecdsa_public_to_fips(const EcdsaFipsCurve *c, const EcdsaPublicKey *pub,
                                uint8_t *q, Error *err) {
    Int n = ecdsa_point_from_affine(pub->curve, pub->x, pub->y, q, err);
    if (n == 0)
        return 0;
    if (!ecdsa_fips_check_public(c, ecdsa_bytes(q, n), err))
        return 0;
    return n;
}

/* privateKeyToFIPS: priv's scalar in d, as long as the order, and its point's
 * encoding in q, both checked, and the length of q. */
static Int ecdsa_private_to_fips(const EcdsaFipsCurve *c, const EcdsaPrivateKey *priv,
                                 uint8_t *d, uint8_t *q, Error *err) {
    const EcdsaPublicKey *pub = &priv->public_key;
    Int n = ecdsa_point_from_affine(pub->curve, pub->x, pub->y, q, err);
    if (n == 0)
        return 0;
    Int nbits = big_int_bit_len(elliptic_curve_params(pub->curve)->n);
    if (big_int_bit_len(priv->d) > nbits) {
        ecdsa_set_error(err, "ecdsa: private key scalar too large");
        return 0;
    }
    if (big_int_sign(priv->d) <= 0) {
        ecdsa_set_error(err, "ecdsa: private key scalar is zero or negative");
        return 0;
    }
    Int size = (nbits + 7) / 8;
    if (size > NISTEC_MAX_ELEMENT_BYTES) {
        ecdsa_set_error(err, "ecdsa: internal error: curve size too large");
        return 0;
    }
    big_int_fill_bytes(priv->d, slice_from(d, size, size, TYPE_BYTE));
    if (!ecdsa_fips_check_private(c, slice_from(d, size, size, TYPE_BYTE),
                                  ecdsa_bytes(q, n), err)) {
        memset(d, 0, (size_t)size);
        return 0;
    }
    return n;
}

/* publicKeyFromFIPS. */
static EcdsaPublicKey *ecdsa_public_from_fips(Alloc *a, EllipticCurve curve, Slice q,
                                              Error *err) {
    EcdsaPublicBlock *b = ecdsa_new_public_block(a, curve, err);
    if (b == NULL)
        return NULL;
    if (!ecdsa_point_to_affine(curve, q, &b->x, &b->y, err)) {
        ecdsa_public_key_free(&b->k);
        return NULL;
    }
    return &b->k;
}

/* privateKeyFromFIPS. */
static EcdsaPrivateKey *ecdsa_private_from_fips(Alloc *a, EllipticCurve curve, Slice d,
                                                Slice q, Error *err) {
    EcdsaPrivateBlock *b = ecdsa_new_private_block(a, curve, err);
    if (b == NULL)
        return NULL;
    if (!ecdsa_point_to_affine(curve, q, &b->x, &b->y, err)) {
        ecdsa_private_key_free(&b->k);
        return NULL;
    }
    big_int_set_bytes(&b->d, d);
    return &b->k;
}

/* ------------------------------------------------------------- encoding */

typedef struct EcdsaSigParts {
    Slice r, s;
} EcdsaSigParts;

static void ecdsa_add_int_body(void *env, CryptobyteBuilder *c) {
    Slice *bytes = env;
    const uint8_t *p = bytes->p;
    if ((p[0] & 0x80) != 0)
        cryptobyte_builder_add_uint8(c, 0);
    cryptobyte_builder_add_bytes(c, *bytes);
}

/* addASN1IntBytes: an INTEGER from big endian bytes, which are not negative. */
static void ecdsa_add_int_bytes(CryptobyteBuilder *b, Slice *bytes) {
    const uint8_t *p = bytes->p;
    while (bytes->len > 0 && p[0] == 0) {
        p++;
        bytes->len--;
    }
    bytes->p = (void *)(uintptr_t)p;
    bytes->cap = bytes->len;
    if (bytes->len == 0) {
        cryptobyte_builder_set_error(
            b, errors_new(error_allocator(), str_from_cstr("invalid integer")));
        return;
    }
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_INTEGER,
        BURROW_FN(CryptobyteBuilderContinuation, ecdsa_add_int_body, bytes));
}

static void ecdsa_add_sig_body(void *env, CryptobyteBuilder *b) {
    EcdsaSigParts *sig = env;
    ecdsa_add_int_bytes(b, &sig->r);
    ecdsa_add_int_bytes(b, &sig->s);
}

/* encodeSignature: SEQUENCE { r INTEGER, s INTEGER }, from a. */
static Slice ecdsa_encode_signature(Alloc *a, Slice r, Slice s, Error *err) {
    EcdsaSigParts sig = {r, s};
    CryptobyteBuilder b =
        cryptobyte_new_builder(heap_allocator(), slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, ecdsa_add_sig_body, &sig));
    Error e = BURROW_NO_ERROR;
    Slice der = cryptobyte_builder_bytes(&b, &e);
    Slice out = slice_nil(TYPE_BYTE);
    if (BURROW_FAILED(e))
        BURROW_OUT(err, e);
    else
        out = ecdsa_clone(a, der.p, der.len, err);
    cryptobyte_builder_free(&b);
    return out;
}

/* parseSignature: r and s, borrowed from sig. */
static bool ecdsa_parse_signature(Slice sig, Slice *r, Slice *s) {
    CryptobyteString inner;
    CryptobyteString input = sig;
    return cryptobyte_string_read_asn1(&input, &inner, CRYPTOBYTE_ASN1_SEQUENCE) &&
           cryptobyte_string_empty(input) &&
           cryptobyte_string_read_asn1_integer_bytes(&inner, r) &&
           cryptobyte_string_read_asn1_integer_bytes(&inner, s) &&
           cryptobyte_string_empty(inner);
}

/* --------------------------------------------------------------- legacy */

/* randFieldElement: a number in [1, N) from rand, in k. */
static bool ecdsa_rand_field_element(Alloc *tmp, const EllipticCurveParams *params,
                                     IoReader rand, BigInt *k, Error *err) {
    const BigInt *n = params->n;
    Int bits = big_int_bit_len(n);
    Int len = (bits + 7) / 8;
    Slice b = slice_make(tmp, TYPE_BYTE, len, len);
    if (b.p == NULL && len > 0)
        panic_str(BURROW_S("crypto/ecdsa: out of memory"));
    for (;;) {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, b, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return false;
        }
        Int excess = len * 8 - bits;
        if (excess > 0 && len > 0) {
            uint8_t *p = b.p;
            p[0] = (uint8_t)(p[0] >> excess);
        }
        big_int_set_bytes(k, b);
        if (big_int_sign(k) != 0 && big_int_cmp(k, n) < 0)
            return true;
    }
}

/* hashToInt: the leftmost bits of hash, as many as the order has. */
static void ecdsa_hash_to_int(const EllipticCurveParams *params, Slice hash,
                              BigInt *ret) {
    Int order_bits = big_int_bit_len(params->n);
    Int order_bytes = (order_bits + 7) / 8;
    if (hash.len > order_bytes)
        hash.len = order_bytes;
    big_int_set_bytes(ret, hash);
    Int excess = hash.len * 8 - order_bits;
    if (excess > 0)
        big_int_rsh(ret, ret, (Uint)excess);
}

/* What Go does with the nil a failed ModInverse gives it. */
static void ecdsa_nil_deref(void) {
    panic_str(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static EcdsaPrivateKey *ecdsa_generate_legacy(Alloc *a, EllipticCurve c, IoReader rand,
                                              Error *err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *tmp = arena_allocator(&ar);
    EcdsaPrivateKey *out = NULL;
    BigInt k = BIG_INT(tmp);
    if (!ecdsa_rand_field_element(tmp, elliptic_curve_params(c), rand, &k, err))
        goto done;
    EcdsaPrivateBlock *b = ecdsa_new_private_block(a, c, err);
    if (b == NULL)
        goto done;
    BigInt *y = NULL;
    BigInt *x = elliptic_curve_scalar_base_mult(c, tmp, big_int_bytes(&k, tmp), &y);
    big_int_set(&b->d, &k);
    big_int_set(&b->x, x);
    big_int_set(&b->y, y);
    out = &b->k;

done:
    if (k.abs.p != NULL)
        memset(k.abs.p, 0, (size_t)k.abs.cap * sizeof *k.abs.p);
    arena_free(&ar);
    return out;
}

static Slice ecdsa_sign_legacy(Alloc *a, const EcdsaPrivateKey *priv, IoReader csprng,
                               Slice hash, Error *err) {
    EllipticCurve c = priv->public_key.curve;
    Slice out = slice_nil(TYPE_BYTE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *tmp = arena_allocator(&ar);

    /* The nonce comes from ChaCha8, seeded with random bytes with the key and
     * the hash folded in, so that a weak rand does not give the key away. */
    Byte seed[32];
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(csprng, slice_from(seed, 32, 32, TYPE_BYTE), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        goto done;
    }
    Slice db = big_int_bytes(priv->d, tmp);
    const uint8_t *dp = db.p;
    for (Int i = 0; i < db.len; i++)
        seed[i % 32] ^= dp[i];
    const uint8_t *hp = hash.p;
    for (Int i = 0; i < hash.len; i++)
        seed[i % 32] ^= hp[i];
    memset(db.p, 0, (size_t)db.len);
    Mathrand2ChaCha8 cha;
    mathrand2_cha_cha8_seed(&cha, seed);
    memset(seed, 0, sizeof seed);
    IoReader r = mathrand2_cha_cha8_as_io_reader(&cha);

    /* See [NSA] 3.4.1 */
    const EllipticCurveParams *params = elliptic_curve_params(c);
    const BigInt *n = params->n;
    if (big_int_sign(n) == 0) {
        ecdsa_set_error(err, "zero parameter");
        goto done;
    }

    BigInt k = BIG_INT(tmp), k_inv = BIG_INT(tmp), s = BIG_INT(tmp), ee = BIG_INT(tmp);
    BigInt *rr = NULL;
    for (;;) {
        for (;;) {
            if (!ecdsa_rand_field_element(tmp, params, r, &k, err))
                goto done;
            if (big_int_mod_inverse(&k_inv, &k, n) == NULL)
                ecdsa_nil_deref();
            rr = elliptic_curve_scalar_base_mult(c, tmp, big_int_bytes(&k, tmp), NULL);
            big_int_mod(rr, rr, n);
            if (big_int_sign(rr) != 0)
                break;
        }
        ecdsa_hash_to_int(params, hash, &ee);
        big_int_mul(&s, priv->d, rr);
        big_int_add(&s, &s, &ee);
        big_int_mul(&s, &s, &k_inv);
        big_int_mod(&s, &s, n); /* N != 0 */
        if (big_int_sign(&s) != 0)
            break;
    }
    out =
        ecdsa_encode_signature(a, big_int_bytes(rr, tmp), big_int_bytes(&s, tmp), err);
    memset(&cha, 0, sizeof cha);

done:
    arena_free(&ar);
    return out;
}

static bool ecdsa_verify_legacy(const EcdsaPublicKey *pub, Slice hash, Slice sig) {
    Slice rb, sb;
    if (!ecdsa_parse_signature(sig, &rb, &sb))
        return false;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *tmp = arena_allocator(&ar);
    bool ok = false;
    BigInt r = BIG_INT(tmp), s = BIG_INT(tmp), e = BIG_INT(tmp), w = BIG_INT(tmp);
    big_int_set_bytes(&r, rb);
    big_int_set_bytes(&s, sb);

    EllipticCurve c = pub->curve;
    const EllipticCurveParams *params = elliptic_curve_params(c);
    const BigInt *n = params->n;

    /* See [NSA] 3.4.2 */
    if (big_int_sign(&r) <= 0 || big_int_sign(&s) <= 0)
        goto done;
    if (big_int_cmp(&r, n) >= 0 || big_int_cmp(&s, n) >= 0)
        goto done;
    ecdsa_hash_to_int(params, hash, &e);
    if (big_int_mod_inverse(&w, &s, n) == NULL)
        ecdsa_nil_deref();

    BigInt *u1 = big_int_mul(&e, &e, &w);
    big_int_mod(u1, u1, n);
    BigInt *u2 = big_int_mul(&w, &r, &w);
    big_int_mod(u2, u2, n);
    BigInt *y1 = NULL, *y2 = NULL, *y = NULL;
    BigInt *x1 = elliptic_curve_scalar_base_mult(c, tmp, big_int_bytes(u1, tmp), &y1);
    BigInt *x2 =
        elliptic_curve_scalar_mult(c, tmp, pub->x, pub->y, big_int_bytes(u2, tmp), &y2);
    BigInt *x = elliptic_curve_add(c, tmp, x1, y1, x2, y2, &y);

    if (big_int_sign(x) == 0 && big_int_sign(y) == 0)
        goto done;
    big_int_mod(x, x, n);
    ok = big_int_cmp(x, &r) == 0;

done:
    arena_free(&ar);
    return ok;
}

/* ------------------------------------------------------------ functions */

EcdsaPrivateKey *ecdsa_generate_key(Alloc *a, EllipticCurve c, IoReader rand,
                                    Error *err) {
    rand = ecdsa_reader(rand);
    const EcdsaFipsCurve *fc = ecdsa_fips_for_params(c);
    if (fc == NULL)
        return ecdsa_generate_legacy(a, c, rand, err);

    uint8_t k[NISTEC_MAX_ELEMENT_BYTES];
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    if (!burrow__ecdsa_random_point(fc, BURROW_FN(EcdsaFill, ecdsa_reader_fill, &rand),
                                    k, q, NULL, err))
        return NULL;
    Int qn = 1 + 2 * fc->nist->element_bytes;
    EcdsaPrivateKey *priv = ecdsa_private_from_fips(a, c, ecdsa_bytes(k, fc->size),
                                                    ecdsa_bytes(q, qn), err);
    memset(k, 0, sizeof k);
    return priv;
}

EcdsaPublicKey *ecdsa_parse_uncompressed_public_key(Alloc *a, EllipticCurve curve,
                                                    Slice data, Error *err) {
    const uint8_t *p = data.p;
    if (data.len < 1 || p[0] != 4) {
        ecdsa_set_error(err, "ecdsa: invalid uncompressed public key");
        return NULL;
    }
    const EcdsaFipsCurve *fc = ecdsa_fips_for_curve(curve);
    if (fc == NULL) {
        ecdsa_set_error(err,
                        "ecdsa: curve not supported by ParseUncompressedPublicKey");
        return NULL;
    }
    if (!ecdsa_fips_check_public(fc, data, err))
        return NULL;
    return ecdsa_public_from_fips(a, curve, data, err);
}

EcdsaPrivateKey *ecdsa_parse_raw_private_key(Alloc *a, EllipticCurve curve, Slice data,
                                             Error *err) {
    const EcdsaFipsCurve *fc = ecdsa_fips_for_curve(curve);
    if (fc == NULL) {
        ecdsa_set_error(err, "ecdsa: curve not supported by ParseRawPrivateKey");
        return NULL;
    }
    const NistecCurve *nist = fc->nist;
    NistecPoint pt;
    Error e = BURROW_NO_ERROR;
    if (nist->scalar_base_mult(nist->point_new(&pt), data, &e) == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    Slice qs = ecdsa_bytes(q, nist->bytes(&pt, q));
    if (!ecdsa_fips_check_private(fc, data, qs, err))
        return NULL;
    return ecdsa_private_from_fips(a, curve, data, qs, err);
}

static Slice ecdsa_sign_fips(Alloc *a, const EcdsaFipsCurve *c,
                             const EcdsaPrivateKey *priv, IoReader rand, Slice hash,
                             Error *err) {
    uint8_t d[NISTEC_MAX_ELEMENT_BYTES];
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    if (ecdsa_private_to_fips(c, priv, d, q, err) == 0)
        return slice_nil(TYPE_BYTE);
    uint8_t r[NISTEC_MAX_ELEMENT_BYTES], s[NISTEC_MAX_ELEMENT_BYTES];
    bool ok = ecdsa_fips_sign(c, ecdsa_bytes(d, c->size), rand, hash, r, s, err);
    memset(d, 0, sizeof d);
    if (!ok)
        return slice_nil(TYPE_BYTE);
    return ecdsa_encode_signature(a, ecdsa_bytes(r, c->size), ecdsa_bytes(s, c->size),
                                  err);
}

Slice ecdsa_sign_asn1(Alloc *a, IoReader rand, const EcdsaPrivateKey *priv, Slice hash,
                      Error *err) {
    if (hash.len == 0) {
        ecdsa_set_error(err, "ecdsa: hash cannot be empty");
        return slice_nil(TYPE_BYTE);
    }
    rand = ecdsa_reader(rand);
    const EcdsaFipsCurve *fc = ecdsa_fips_for_params(priv->public_key.curve);
    if (fc == NULL)
        return ecdsa_sign_legacy(a, priv, rand, hash, err);
    return ecdsa_sign_fips(a, fc, priv, rand, hash, err);
}

static bool ecdsa_verify_fips(const EcdsaFipsCurve *c, const EcdsaPublicKey *pub,
                              Slice hash, Slice sig) {
    Slice r, s;
    if (!ecdsa_parse_signature(sig, &r, &s))
        return false;
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    Int n = ecdsa_public_to_fips(c, pub, q, NULL);
    if (n == 0)
        return false;
    return ecdsa_fips_verify(c, ecdsa_bytes(q, n), hash, r, s, NULL);
}

bool ecdsa_verify_asn1(const EcdsaPublicKey *pub, Slice hash, Slice sig) {
    if (hash.len == 0)
        return false;
    const EcdsaFipsCurve *fc = ecdsa_fips_for_params(pub->curve);
    if (fc == NULL)
        return ecdsa_verify_legacy(pub, hash, sig);
    return ecdsa_verify_fips(fc, pub, hash, sig);
}

BigInt *ecdsa_sign(Alloc *a, IoReader rand, const EcdsaPrivateKey *priv, Slice hash,
                   BigInt **s, Error *err) {
    BURROW_OUT(s, NULL);
    Arena ar;
    arena_init(&ar, NULL, 0);
    BigInt *rv = NULL;
    Slice sig = ecdsa_sign_asn1(arena_allocator(&ar), rand, priv, hash, err);
    if (sig.p == NULL)
        goto done;

    a = ecdsa_alloc(a);
    BigInt *r = big_new_int(a, 0);
    BigInt *sv = big_new_int(a, 0);
    if (r == NULL || sv == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        goto done;
    }
    CryptobyteString inner;
    CryptobyteString input = sig;
    if (!cryptobyte_string_read_asn1(&input, &inner, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_empty(input) ||
        !cryptobyte_string_read_asn1_integer_big(&inner, r) ||
        !cryptobyte_string_read_asn1_integer_big(&inner, sv) ||
        !cryptobyte_string_empty(inner)) {
        ecdsa_set_error(err, "invalid ASN.1 from SignASN1");
        big_int_free(r);
        big_int_free(sv);
        mem_free(a, r, sizeof *r, _Alignof(BigInt));
        mem_free(a, sv, sizeof *sv, _Alignof(BigInt));
        goto done;
    }
    rv = r;
    BURROW_OUT(s, sv);

done:
    arena_free(&ar);
    return rv;
}

bool ecdsa_verify(const EcdsaPublicKey *pub, Slice hash, const BigInt *r,
                  const BigInt *s) {
    if (big_int_sign(r) <= 0 || big_int_sign(s) <= 0)
        return false;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *tmp = arena_allocator(&ar);
    bool ok = false;
    Slice sig =
        ecdsa_encode_signature(tmp, big_int_bytes(r, tmp), big_int_bytes(s, tmp), NULL);
    if (sig.p != NULL)
        ok = ecdsa_verify_asn1(pub, hash, sig);
    arena_free(&ar);
    return ok;
}

/* ------------------------------------------------------------- PublicKey */

Slice ecdsa_public_key_bytes(const EcdsaPublicKey *pub, Alloc *a, Error *err) {
    const EcdsaFipsCurve *fc = ecdsa_fips_for_curve(pub->curve);
    if (fc == NULL) {
        ecdsa_set_error(err, "ecdsa: curve not supported by PublicKey.Bytes");
        return slice_nil(TYPE_BYTE);
    }
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    Int n = ecdsa_public_to_fips(fc, pub, q, err);
    if (n == 0)
        return slice_nil(TYPE_BYTE);
    return ecdsa_clone(a, q, n, err);
}

/* curveToECDH. */
static const EcdhCurve *ecdsa_curve_to_ecdh(EllipticCurve c) {
    if (ecdsa_same_curve(c, elliptic_p256()))
        return ecdh_p256();
    if (ecdsa_same_curve(c, elliptic_p384()))
        return ecdh_p384();
    if (ecdsa_same_curve(c, elliptic_p521()))
        return ecdh_p521();
    return NULL;
}

EcdhPublicKey *ecdsa_public_key_ecdh(const EcdsaPublicKey *pub, Alloc *a, Error *err) {
    const EcdhCurve *c = ecdsa_curve_to_ecdh(pub->curve);
    if (c == NULL) {
        ecdsa_set_error(err, "ecdsa: unsupported curve by crypto/ecdh");
        return NULL;
    }
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    Int n = ecdsa_public_to_fips(ecdsa_fips_for_curve(pub->curve), pub, q, err);
    if (n == 0)
        return NULL;
    return ecdh_curve_new_public_key(c, a, ecdsa_bytes(q, n), err);
}

/* bigIntEqual: a.Bytes() and b.Bytes() compared in constant time. */
static bool ecdsa_big_int_equal(const BigInt *x, const BigInt *y) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *tmp = arena_allocator(&ar);
    Slice xb = big_int_bytes(x, tmp);
    Slice yb = big_int_bytes(y, tmp);
    bool eq = subtle_constant_time_compare(xb, yb) == 1;
    if (xb.p != NULL)
        memset(xb.p, 0, (size_t)xb.len);
    if (yb.p != NULL)
        memset(yb.p, 0, (size_t)yb.len);
    arena_free(&ar);
    return eq;
}

static bool ecdsa_public_equal(const EcdsaPublicKey *pub, const EcdsaPublicKey *xx) {
    return ecdsa_big_int_equal(pub->x, xx->x) && ecdsa_big_int_equal(pub->y, xx->y) &&
           ecdsa_same_curve(pub->curve, xx->curve);
}

bool ecdsa_public_key_equal(const EcdsaPublicKey *pub, CryptoPublicKey x) {
    if (x.t != TYPE_ECDSA_PUBLIC_KEY || x.data == NULL)
        return false;
    return ecdsa_public_equal(pub, x.data);
}

/* ------------------------------------------------------------ PrivateKey */

Slice ecdsa_private_key_bytes(const EcdsaPrivateKey *priv, Alloc *a, Error *err) {
    const EcdsaFipsCurve *fc = ecdsa_fips_for_curve(priv->public_key.curve);
    if (fc == NULL) {
        ecdsa_set_error(err, "ecdsa: curve not supported by PrivateKey.Bytes");
        return slice_nil(TYPE_BYTE);
    }
    uint8_t d[NISTEC_MAX_ELEMENT_BYTES];
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    if (ecdsa_private_to_fips(fc, priv, d, q, err) == 0)
        return slice_nil(TYPE_BYTE);
    Slice out = ecdsa_clone(a, d, fc->size, err);
    memset(d, 0, sizeof d);
    return out;
}

EcdhPrivateKey *ecdsa_private_key_ecdh(const EcdsaPrivateKey *priv, Alloc *a,
                                       Error *err) {
    EllipticCurve curve = priv->public_key.curve;
    const EcdhCurve *c = ecdsa_curve_to_ecdh(curve);
    if (c == NULL) {
        ecdsa_set_error(err, "ecdsa: unsupported curve by crypto/ecdh");
        return NULL;
    }
    const EcdsaFipsCurve *fc = ecdsa_fips_for_curve(curve);
    uint8_t d[NISTEC_MAX_ELEMENT_BYTES];
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    if (ecdsa_private_to_fips(fc, priv, d, q, err) == 0)
        return NULL;
    EcdhPrivateKey *k = ecdh_curve_new_private_key(c, a, ecdsa_bytes(d, fc->size), err);
    memset(d, 0, sizeof d);
    return k;
}

bool ecdsa_private_key_equal(const EcdsaPrivateKey *priv, CryptoPrivateKey x) {
    if (x.t != TYPE_ECDSA_PRIVATE_KEY || x.data == NULL)
        return false;
    const EcdsaPrivateKey *xx = x.data;
    return ecdsa_public_equal(&priv->public_key, &xx->public_key) &&
           ecdsa_big_int_equal(priv->d, xx->d);
}

CryptoPublicKey ecdsa_private_key_public(const EcdsaPrivateKey *priv) {
    return BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, (uintptr_t)&priv->public_key);
}

/* signFIPSDeterministic. */
static Slice ecdsa_sign_fips_deterministic(Alloc *a, const EcdsaFipsCurve *c,
                                           CryptoHash hash_func,
                                           const EcdsaPrivateKey *priv, Slice hash,
                                           Error *err) {
    uint8_t d[NISTEC_MAX_ELEMENT_BYTES];
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    if (ecdsa_private_to_fips(c, priv, d, q, err) == 0)
        return slice_nil(TYPE_BYTE);
    Slice out = slice_nil(TYPE_BYTE);
    if (!crypto_hash_available(hash_func)) {
        static const char prefix[] = "ecdsa: requested hash function unavailable: ";
        Alloc *ea = error_allocator();
        Str name = crypto_hash_string(hash_func, ea);
        Int plen = (Int)sizeof prefix - 1;
        Byte *p = mem_alloc_nozero(ea, (size_t)(plen + name.len), 1);
        if (p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
        } else {
            memcpy(p, prefix, (size_t)plen);
            if (name.len > 0)
                memcpy(p + plen, name.p, (size_t)name.len);
            BURROW_OUT(err, errors_new(ea, str_from_bytes(p, plen + name.len)));
        }
        goto done;
    }
    HashNewFunc h = burrow__crypto_hash_new_func(hash_func);
    uint8_t r[NISTEC_MAX_ELEMENT_BYTES], s[NISTEC_MAX_ELEMENT_BYTES];
    if (!ecdsa_fips_sign_deterministic(c, h, ecdsa_bytes(d, c->size), hash, r, s, err))
        goto done;
    out = ecdsa_encode_signature(a, ecdsa_bytes(r, c->size), ecdsa_bytes(s, c->size),
                                 err);

done:
    memset(d, 0, sizeof d);
    return out;
}

/* signRFC6979. */
static Slice ecdsa_sign_rfc6979(Alloc *a, const EcdsaPrivateKey *priv, Slice hash,
                                CryptoSignerOpts opts, Error *err) {
    if (opts.vt == NULL) {
        ecdsa_set_error(err, "ecdsa: Sign called with nil random and nil opts");
        return slice_nil(TYPE_BYTE);
    }
    CryptoHash h = opts.vt->hash_func(opts.data);
    const EcdsaFipsCurve *fc = ecdsa_fips_for_params(priv->public_key.curve);
    if (fc == NULL) {
        ecdsa_set_error(err, "ecdsa: curve not supported by deterministic signatures");
        return slice_nil(TYPE_BYTE);
    }
    return ecdsa_sign_fips_deterministic(a, fc, h, priv, hash, err);
}

Slice ecdsa_private_key_sign(const EcdsaPrivateKey *priv, Alloc *a, IoReader rand,
                             Slice digest, CryptoSignerOpts opts, Error *err) {
    if (opts.vt != NULL) {
        CryptoHash h = opts.vt->hash_func(opts.data);
        if (h == 0) {
            ecdsa_set_error(
                err, "ecdsa: Sign must be called with a hash, not with crypto.Hash(0)");
            return slice_nil(TYPE_BYTE);
        }
        if (crypto_hash_size(h) != digest.len) {
            ecdsa_set_error(err, "ecdsa: hash length does not match hash function");
            return slice_nil(TYPE_BYTE);
        }
    }
    if (rand.vt == NULL)
        return ecdsa_sign_rfc6979(a, priv, digest, opts, err);
    rand = burrow__crypto_rand_custom_reader(rand);
    return ecdsa_sign_asn1(a, rand, priv, digest, err);
}

/* --------------------------------------------------------------- Signer */

static CryptoPublicKey ecdsa_signer_public(void *self) {
    return ecdsa_private_key_public(self);
}

static Slice ecdsa_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                               CryptoSignerOpts opts, Error *err) {
    return ecdsa_private_key_sign(self, a, rand, digest, opts, err);
}

static const CryptoSignerVT ecdsa_signer_vt = {TYPE_ECDSA_PRIVATE_KEY,
                                               ecdsa_signer_public, ecdsa_signer_sign};

CryptoSigner ecdsa_private_key_signer(const EcdsaPrivateKey *priv) {
    return (CryptoSigner){&ecdsa_signer_vt, (void *)(uintptr_t)priv};
}
