/* crypto/dsa: DSA, of FIPS 186-3, on math/big.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/dsa.h"

#include "burrow/core.h"
#include "burrow/crypto/rand.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "rand_internal.h"

#include <stdint.h>

/* ------------------------------------------------------------------ types */

const Type burrow_type_DsaPublicKey = {
    BURROW_S_INIT("PublicKey"),
    BURROW_S_INIT("crypto/dsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(DsaPublicKey),
    (uint16_t)_Alignof(DsaPublicKey),
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

const Type burrow_type_DsaPrivateKey = {
    BURROW_S_INIT("PrivateKey"),
    BURROW_S_INIT("crypto/dsa"),
    KIND_STRUCT,
    (uint32_t)sizeof(DsaPrivateKey),
    (uint16_t)_Alignof(DsaPrivateKey),
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

BURROW_SENTINEL_ERROR(dsa_err_invalid_public_key, "crypto/dsa: invalid public key");

/* ---------------------------------------------------------------- helpers */

/* The number of Miller-Rabin tests, the most table C.1 of FIPS 186-3
 * recommends. */
#define DSA_MR_TESTS 64

static Error dsa_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static Alloc *dsa_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* What GenerateParameters and GenerateKey read: rand as it is, which in Go
 * cannot be nil, so nil here is crypto_rand_reader. */
static IoReader dsa_reader(IoReader r) {
    return r.vt != NULL ? r : crypto_rand_reader;
}

static bool dsa_read(IoReader r, Slice b, Error *err) {
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, b, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

/* A new BigInt from a with the value of x. */
static BigInt *dsa_new(Alloc *a, const BigInt *x) {
    BigInt *z = big_new_int(a, 0);
    big_int_set(z, x);
    return z;
}

/* A zeroed scratch buffer of n bytes from the heap. */
static Slice dsa_buf(Int n) {
    return slice_make(heap_allocator(), TYPE_BYTE, n, n);
}

static void dsa_buf_free(Slice b) {
    if (b.cap > 0)
        mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
}

static Byte *dsa_buf_at(Slice b, Int i) {
    return (Byte *)b.p + i;
}

/* ----------------------------------------------------------- parameters */

Error dsa_generate_parameters(DsaParameters *params, Alloc *a, IoReader rand,
                              DsaParameterSizes sizes) {
    /* This does not follow FIPS 186-3 exactly, in that it does not use a
     * verification seed to make the primes, as Go does not. */
    Int l, n;
    if (sizes == DSA_L1024N160) {
        l = 1024;
        n = 160;
    } else if (sizes == DSA_L2048N224) {
        l = 2048;
        n = 224;
    } else if (sizes == DSA_L2048N256) {
        l = 2048;
        n = 256;
    } else if (sizes == DSA_L3072N256) {
        l = 3072;
        n = 256;
    } else {
        return dsa_error("crypto/dsa: invalid ParameterSizes");
    }
    rand = dsa_reader(rand);
    Error err = BURROW_NO_ERROR;

    Slice q_bytes = dsa_buf(n / 8);
    Slice p_bytes = dsa_buf(l / 8);
    BigInt q = BIG_INT(NULL), p = BIG_INT(NULL), rem = BIG_INT(NULL);
    BigInt one = BIG_INT(NULL), h = BIG_INT(NULL), g = BIG_INT(NULL);
    BigInt pm1 = BIG_INT(NULL), e = BIG_INT(NULL);
    big_int_set_int64(&one, 1);

    bool found = false;
    while (!found) {
        if (!dsa_read(rand, q_bytes, &err))
            goto done;
        *dsa_buf_at(q_bytes, q_bytes.len - 1) |= 1;
        *dsa_buf_at(q_bytes, 0) |= 0x80;
        big_int_set_bytes(&q, q_bytes);

        if (!big_int_probably_prime(&q, DSA_MR_TESTS))
            continue;

        for (Int i = 0; i < 4 * l; i++) {
            if (!dsa_read(rand, p_bytes, &err))
                goto done;
            *dsa_buf_at(p_bytes, p_bytes.len - 1) |= 1;
            *dsa_buf_at(p_bytes, 0) |= 0x80;

            big_int_set_bytes(&p, p_bytes);
            big_int_mod(&rem, &p, &q);
            big_int_sub(&rem, &rem, &one);
            big_int_sub(&p, &p, &rem);
            if (big_int_bit_len(&p) < l)
                continue;

            if (!big_int_probably_prime(&p, DSA_MR_TESTS))
                continue;

            found = true;
            break;
        }
    }

    big_int_set_int64(&h, 2);
    big_int_sub(&pm1, &p, &one);
    big_int_div(&e, &pm1, &q);
    for (;;) {
        big_int_exp(&g, &h, &e, &p);
        if (big_int_cmp(&g, &one) == 0) {
            big_int_add(&h, &h, &one);
            continue;
        }
        break;
    }

    a = dsa_alloc(a);
    params->p = dsa_new(a, &p);
    params->q = dsa_new(a, &q);
    params->g = dsa_new(a, &g);

done:
    big_int_free(&q);
    big_int_free(&p);
    big_int_free(&rem);
    big_int_free(&one);
    big_int_free(&h);
    big_int_free(&g);
    big_int_free(&pm1);
    big_int_free(&e);
    dsa_buf_free(q_bytes);
    dsa_buf_free(p_bytes);
    return err;
}

/* ------------------------------------------------------------------- keys */

Error dsa_generate_key(DsaPrivateKey *priv, Alloc *a, IoReader rand) {
    const DsaParameters *params = &priv->public_key.parameters;
    if (params->p == NULL || params->q == NULL || params->g == NULL) {
        return dsa_error("crypto/dsa: parameters not set up before generating key");
    }
    rand = dsa_reader(rand);
    Error err = BURROW_NO_ERROR;

    a = dsa_alloc(a);
    BigInt x = BIG_INT(NULL);
    Slice x_bytes = dsa_buf(big_int_bit_len(params->q) / 8);
    for (;;) {
        if (!dsa_read(rand, x_bytes, &err))
            goto done;
        big_int_set_bytes(&x, x_bytes);
        if (big_int_sign(&x) != 0 && big_int_cmp(&x, params->q) < 0)
            break;
    }

    priv->x = dsa_new(a, &x);
    priv->public_key.y = big_new_int(a, 0);
    big_int_exp(priv->public_key.y, params->g, &x, params->p);

done:
    big_int_free(&x);
    dsa_buf_free(x_bytes);
    return err;
}

/* --------------------------------------------------------------- signing */

/* The inverse of k mod p, by Fermat's method, k to the p-2. It is closer to
 * constant time than Euclid's method in big_int_mod_inverse, although math/big
 * is not constant time itself, so it is not perfect. */
static void dsa_fermat_inverse(BigInt *z, const BigInt *k, const BigInt *p) {
    BigInt pm2 = BIG_INT(NULL), two = BIG_INT(NULL);
    big_int_set_int64(&two, 2);
    big_int_sub(&pm2, p, &two);
    big_int_exp(z, k, &pm2, p);
    big_int_free(&pm2);
    big_int_free(&two);
}

BigInt *dsa_sign(Alloc *a, IoReader rand, const DsaPrivateKey *priv, Slice hash,
                 BigInt **s, Error *err) {
    if (rand.vt == NULL)
        rand = burrow__crypto_rand_nil_reader();
    rand = burrow__crypto_rand_custom_reader(rand);
    *s = NULL;

    /* FIPS 186-3, section 4.6. */
    const DsaParameters *params = &priv->public_key.parameters;
    Int n = big_int_bit_len(params->q);
    if (big_int_sign(params->q) <= 0 || big_int_sign(params->p) <= 0 ||
        big_int_sign(params->g) <= 0 || big_int_sign(priv->x) <= 0 || n % 8 != 0) {
        BURROW_OUT(err, dsa_err_invalid_public_key);
        return NULL;
    }
    n >>= 3;

    a = dsa_alloc(a);
    BigInt k = BIG_INT(NULL), k_inv = BIG_INT(NULL);
    BigInt r = BIG_INT(NULL), sv = BIG_INT(NULL);
    Slice buf = dsa_buf(n);
    BigInt *ret = NULL;

    Int attempts;
    for (attempts = 10; attempts > 0; attempts--) {
        for (;;) {
            if (!dsa_read(rand, buf, err))
                goto done;
            big_int_set_bytes(&k, buf);
            /* q is at least 128, since it is positive and its length in
             * bits is a multiple of 8, so this loop ends soon. */
            if (big_int_sign(&k) > 0 && big_int_cmp(&k, params->q) < 0)
                break;
        }

        dsa_fermat_inverse(&k_inv, &k, params->q);

        big_int_exp(&r, params->g, &k, params->p);
        big_int_mod(&r, &r, params->q);

        if (big_int_sign(&r) == 0)
            continue;

        BigInt *z = big_int_set_bytes(&k, hash);

        big_int_mul(&sv, priv->x, &r);
        big_int_add(&sv, &sv, z);
        big_int_mod(&sv, &sv, params->q);
        big_int_mul(&sv, &sv, &k_inv);
        big_int_mod(&sv, &sv, params->q);

        if (big_int_sign(&sv) != 0)
            break;
    }

    /* Only a degenerate private key needs more than a few attempts. */
    if (attempts == 0) {
        BURROW_OUT(err, dsa_err_invalid_public_key);
        goto done;
    }

    ret = dsa_new(a, &r);
    *s = dsa_new(a, &sv);

done:
    big_int_free(&k);
    big_int_free(&k_inv);
    big_int_free(&r);
    big_int_free(&sv);
    dsa_buf_free(buf);
    return ret;
}

bool dsa_verify(const DsaPublicKey *pub, Slice hash, const BigInt *r, const BigInt *s) {
    /* FIPS 186-3, section 4.7. */
    const DsaParameters *params = &pub->parameters;
    if (big_int_sign(params->p) == 0)
        return false;

    if (big_int_sign(r) < 1 || big_int_cmp(r, params->q) >= 0)
        return false;
    if (big_int_sign(s) < 1 || big_int_cmp(s, params->q) >= 0)
        return false;

    BigInt w = BIG_INT(NULL), z = BIG_INT(NULL), u1 = BIG_INT(NULL), u2 = BIG_INT(NULL);
    BigInt v = BIG_INT(NULL);
    bool ok = false;
    if (big_int_mod_inverse(&w, s, params->q) == NULL)
        goto done;

    if (big_int_bit_len(params->q) % 8 != 0)
        goto done;
    big_int_set_bytes(&z, hash);

    big_int_mul(&u1, &z, &w);
    big_int_mod(&u1, &u1, params->q);
    big_int_mul(&u2, r, &w);
    big_int_mod(&u2, &u2, params->q);
    big_int_exp(&v, params->g, &u1, params->p);
    big_int_exp(&u1, pub->y, &u2, params->p);
    big_int_mul(&v, &v, &u1);
    big_int_mod(&v, &v, params->p);
    big_int_mod(&v, &v, params->q);

    ok = big_int_cmp(&v, r) == 0;

done:
    big_int_free(&w);
    big_int_free(&z);
    big_int_free(&u1);
    big_int_free(&u2);
    big_int_free(&v);
    return ok;
}
