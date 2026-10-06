/* Derived from Go's src/crypto/ecdh/ecdh.go, nist.go and x25519.go, and
 * src/crypto/internal/fips140/ecdh/ecdh.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/ecdh.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "edwards25519.h"
#include "nistec.h"
#include "rand_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ types */

/* A key is one block from a: the struct, then the bytes its Slices point at,
 * the scalar first for a private key. size is the whole block, for mem_free. */
struct EcdhPublicKey {
    const EcdhCurve *curve;
    Slice public_key;
    Alloc *a;
    size_t size;
};

struct EcdhPrivateKey {
    const EcdhCurve *curve;
    Slice private_key;
    EcdhPublicKey public_key;
    Alloc *a;
    size_t size;
};

const Type burrow_type_EcdhPublicKey = {
    BURROW_S_INIT("PublicKey"),
    BURROW_S_INIT("crypto/ecdh"),
    KIND_STRUCT,
    (uint32_t)sizeof(EcdhPublicKey),
    (uint16_t)_Alignof(EcdhPublicKey),
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

const Type burrow_type_EcdhPrivateKey = {
    BURROW_S_INIT("PrivateKey"),
    BURROW_S_INIT("crypto/ecdh"),
    KIND_STRUCT,
    (uint32_t)sizeof(EcdhPrivateKey),
    (uint16_t)_Alignof(EcdhPrivateKey),
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

/* ----------------------------------------------------------------- curves */

struct EcdhCurve {
    const char *name;
    /* NULL for X25519. */
    const NistecCurve *nist;
    /* The order of the generator, big endian, which a private key is less
     * than. As long as an element of the field for each NIST curve. */
    const uint8_t *order;
};

static const uint8_t ecdh_p256_order[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
    0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

static const uint8_t ecdh_p384_order[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf, 0x58, 0x1a, 0x0d, 0xb2,
    0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73,
};

static const uint8_t ecdh_p521_order[66] = {
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xfa, 0x51, 0x86, 0x87, 0x83, 0xbf, 0x2f, 0x96, 0x6b,
    0x7f, 0xcc, 0x01, 0x48, 0xf7, 0x09, 0xa5, 0xd0, 0x3b, 0xb5, 0xc9, 0xb8, 0x89, 0x9c,
    0x47, 0xae, 0xbb, 0x6f, 0xb7, 0x1e, 0x91, 0x38, 0x64, 0x09,
};

static const EcdhCurve ecdh_curve_p256 = {"P-256", &burrow__nistec_p256,
                                          ecdh_p256_order};
static const EcdhCurve ecdh_curve_p384 = {"P-384", &burrow__nistec_p384,
                                          ecdh_p384_order};
static const EcdhCurve ecdh_curve_p521 = {"P-521", &burrow__nistec_p521,
                                          ecdh_p521_order};
static const EcdhCurve ecdh_curve_x25519 = {"X25519", NULL, NULL};

const EcdhCurve *ecdh_p256(void) {
    return &ecdh_curve_p256;
}
const EcdhCurve *ecdh_p384(void) {
    return &ecdh_curve_p384;
}
const EcdhCurve *ecdh_p521(void) {
    return &ecdh_curve_p521;
}
const EcdhCurve *ecdh_x25519(void) {
    return &ecdh_curve_x25519;
}

Str ecdh_curve_string(const EcdhCurve *c) {
    return str_from_cstr(c->name);
}

/* ------------------------------------------------------------------- keys */

#define ECDH_X25519_SIZE 32

static void ecdh_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
}

static Alloc *ecdh_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

static EcdhPublicKey *ecdh_new_public(const EcdhCurve *c, Alloc *a, const uint8_t *q,
                                      Int n, Error *err) {
    a = ecdh_alloc(a);
    size_t size = sizeof(EcdhPublicKey) + (size_t)n;
    EcdhPublicKey *k = mem_alloc(a, size, _Alignof(EcdhPublicKey));
    if (k == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    uint8_t *p = (uint8_t *)(k + 1);
    memcpy(p, q, (size_t)n);
    k->curve = c;
    k->public_key = slice_from(p, n, n, TYPE_BYTE);
    k->a = a;
    k->size = size;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return k;
}

static EcdhPrivateKey *ecdh_new_private(const EcdhCurve *c, Alloc *a, const uint8_t *d,
                                        Int dn, const uint8_t *q, Int qn, Error *err) {
    a = ecdh_alloc(a);
    size_t size = sizeof(EcdhPrivateKey) + (size_t)dn + (size_t)qn;
    EcdhPrivateKey *k = mem_alloc(a, size, _Alignof(EcdhPrivateKey));
    if (k == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    uint8_t *p = (uint8_t *)(k + 1);
    memcpy(p, d, (size_t)dn);
    memcpy(p + dn, q, (size_t)qn);
    k->curve = c;
    k->private_key = slice_from(p, dn, dn, TYPE_BYTE);
    k->public_key.curve = c;
    k->public_key.public_key = slice_from(p + dn, qn, qn, TYPE_BYTE);
    k->public_key.a = NULL;
    k->public_key.size = 0;
    k->a = a;
    k->size = size;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return k;
}

void ecdh_private_key_free(EcdhPrivateKey *k) {
    if (k == NULL)
        return;
    /* The scalar is secret, so it does not go back to the allocator as it
     * is. */
    memset(k->private_key.p, 0, (size_t)k->private_key.len);
    mem_free(k->a, k, k->size, _Alignof(EcdhPrivateKey));
}

void ecdh_public_key_free(EcdhPublicKey *k) {
    if (k == NULL)
        return;
    mem_free(k->a, k, k->size, _Alignof(EcdhPublicKey));
}

/* --------------------------------------------------------------------- X25519 */

static void ecdh_x25519_scalar_mult(uint8_t dst[32], const uint8_t *scalar,
                                    const uint8_t *point) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    Edwards25519Element x1, x2, z2, x3, z3, tmp0, tmp1;
    /* SetBytes takes any 32 bytes, ignoring the top bit. */
    (void)burrow__fe_set_bytes(
        &x1, slice_from((void *)(uintptr_t)point, 32, 32, TYPE_BYTE), NULL);
    burrow__fe_one(&x2);
    burrow__fe_set(&x3, &x1);
    burrow__fe_one(&z3);
    burrow__fe_zero(&z2);

    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int b = (e[pos / 8] >> (unsigned)(pos & 7)) & 1;
        swap ^= b;
        burrow__fe_swap(&x2, &x3, swap);
        burrow__fe_swap(&z2, &z3, swap);
        swap = b;

        burrow__fe_subtract(&tmp0, &x3, &z3);
        burrow__fe_subtract(&tmp1, &x2, &z2);
        burrow__fe_add(&x2, &x2, &z2);
        burrow__fe_add(&z2, &x3, &z3);
        burrow__fe_multiply(&z3, &tmp0, &x2);
        burrow__fe_multiply(&z2, &z2, &tmp1);
        burrow__fe_square(&tmp0, &tmp1);
        burrow__fe_square(&tmp1, &x2);
        burrow__fe_add(&x3, &z3, &z2);
        burrow__fe_subtract(&z2, &z3, &z2);
        burrow__fe_multiply(&x2, &tmp1, &tmp0);
        burrow__fe_subtract(&tmp1, &tmp1, &tmp0);
        burrow__fe_square(&z2, &z2);
        burrow__fe_mult32(&z3, &tmp1, 121666);
        burrow__fe_square(&x3, &x3);
        burrow__fe_add(&tmp0, &tmp0, &z3);
        burrow__fe_multiply(&z3, &x1, &z2);
        burrow__fe_multiply(&z2, &tmp1, &tmp0);
    }

    burrow__fe_swap(&x2, &x3, swap);
    burrow__fe_swap(&z2, &z3, swap);

    burrow__fe_invert(&z2, &z2);
    burrow__fe_multiply(&x2, &x2, &z2);
    burrow__fe_bytes(&x2, dst);
    memset(e, 0, sizeof e);
}

static int ecdh_is_zero(const uint8_t *x, Int n) {
    uint8_t acc = 0;
    for (Int i = 0; i < n; i++)
        acc |= x[i];
    return acc == 0;
}

static EcdhPrivateKey *ecdh_x25519_new_private(Alloc *a, Slice key, Error *err) {
    if (key.len != ECDH_X25519_SIZE) {
        ecdh_set_error(err, "crypto/ecdh: invalid private key size");
        return NULL;
    }
    static const uint8_t basepoint[32] = {9};
    uint8_t pub[32];
    ecdh_x25519_scalar_mult(pub, key.p, basepoint);
    return ecdh_new_private(&ecdh_curve_x25519, a, key.p, 32, pub, 32, err);
}

/* ------------------------------------------------------------------- NIST */

/* fips140/ecdh's NewPrivateKey after the checks on key: the public key, the
 * uncompressed encoding of key times the generator, in out. */
static Int ecdh_nist_public(const EcdhCurve *c, const uint8_t *key, uint8_t *out) {
    NistecPoint p;
    Error e = BURROW_NO_ERROR;
    Int n = c->nist->element_bytes;
    if (c->nist->scalar_base_mult(
            &p, slice_from((void *)(uintptr_t)key, n, n, TYPE_BYTE), &e) == NULL)
        panic_str(
            BURROW_S("crypto/ecdh: internal error: nistec ScalarBaseMult failed for a "
                     "fixed-size input"));
    Int len = c->nist->bytes(&p, out);
    if (len == 1)
        panic_str(BURROW_S("crypto/ecdh: internal error: public key is the identity "
                           "element"));
    return len;
}

/* Whether key is a valid scalar: element_bytes long, not zero and less than
 * the order. Go's isLess is a borrow chain over 64 bit words, and this is one
 * over bytes, which gives the same borrow. */
static int ecdh_nist_valid_scalar(const EcdhCurve *c, Slice key) {
    Int n = c->nist->element_bytes;
    if (key.len != n)
        return 0;
    const uint8_t *k = key.p;
    if (ecdh_is_zero(k, n))
        return 0;
    /* key < order is not order <= key. */
    return !burrow__nistec_less_or_eq_bytes(c->order, k, n);
}

static EcdhPrivateKey *ecdh_nist_new_private(const EcdhCurve *c, Alloc *a, Slice key,
                                             Error *err) {
    if (!ecdh_nist_valid_scalar(c, key)) {
        ecdh_set_error(err, "crypto/ecdh: invalid private key");
        return NULL;
    }
    uint8_t pub[NISTEC_MAX_POINT_BYTES];
    Int n = ecdh_nist_public(c, key.p, pub);
    return ecdh_new_private(c, a, key.p, key.len, pub, n, err);
}

/* drbg.ReadWithReader: the system generator for the default reader, which is
 * what drbg.Read is outside FIPS mode, and io.ReadFull for anything else. */
static int ecdh_read(IoReader r, uint8_t *p, Int n, Error *err) {
    Slice s = slice_from(p, n, n, TYPE_BYTE);
    if (burrow__crypto_rand_is_default_reader(r)) {
        burrow__crypto_rand_system(s);
        return 1;
    }
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, s, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ Curve */

EcdhPrivateKey *ecdh_curve_generate_key(const EcdhCurve *c, Alloc *a, IoReader rand,
                                        Error *err) {
    if (rand.vt == NULL)
        rand = burrow__crypto_rand_nil_reader();
    rand = burrow__crypto_rand_custom_reader(rand);

    if (c->nist == NULL) {
        uint8_t key[ECDH_X25519_SIZE];
        if (!ecdh_read(rand, key, ECDH_X25519_SIZE, err))
            return NULL;
        EcdhPrivateKey *k = ecdh_x25519_new_private(
            a, slice_from(key, ECDH_X25519_SIZE, ECDH_X25519_SIZE, TYPE_BYTE), err);
        memset(key, 0, sizeof key);
        return k;
    }

    /* fips140/ecdh.GenerateKey: random bytes until they are a valid scalar,
     * which for P-256 fails about once in 2^32 tries and for the others less
     * often than that. The xor keeps a reader that gives all zeros or all ones
     * from making a key, since either is a bad scalar, and P-521 drops the
     * seven bits above its 521. */
    Int n = c->nist->element_bytes;
    uint8_t key[NISTEC_MAX_ELEMENT_BYTES];
    for (;;) {
        if (!ecdh_read(rand, key, n, err))
            return NULL;
        key[1] ^= 0x42;
        if (c == &ecdh_curve_p521)
            key[0] &= 1;
        Slice ks = slice_from(key, n, n, TYPE_BYTE);
        if (ecdh_nist_valid_scalar(c, ks)) {
            EcdhPrivateKey *k = ecdh_nist_new_private(c, a, ks, err);
            memset(key, 0, sizeof key);
            return k;
        }
    }
}

EcdhPrivateKey *ecdh_curve_new_private_key(const EcdhCurve *c, Alloc *a, Slice key,
                                           Error *err) {
    if (c->nist == NULL)
        return ecdh_x25519_new_private(a, key, err);
    return ecdh_nist_new_private(c, a, key, err);
}

EcdhPublicKey *ecdh_curve_new_public_key(const EcdhCurve *c, Alloc *a, Slice key,
                                         Error *err) {
    const uint8_t *k = key.p;
    if (c->nist == NULL) {
        if (key.len != ECDH_X25519_SIZE) {
            ecdh_set_error(err, "crypto/ecdh: invalid public key");
            return NULL;
        }
        return ecdh_new_public(c, a, k, key.len, err);
    }
    if (key.len == 0 || k[0] != 4) {
        ecdh_set_error(err, "crypto/ecdh: invalid public key");
        return NULL;
    }
    NistecPoint p;
    Error e = BURROW_NO_ERROR;
    if (c->nist->set_bytes(c->nist->point_new(&p), key, &e) == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return ecdh_new_public(c, a, k, key.len, err);
}

/* ------------------------------------------------------------- PrivateKey */

Slice ecdh_private_key_ecdh(const EcdhPrivateKey *k, Alloc *a,
                            const EcdhPublicKey *remote, Error *err) {
    if (k->curve != remote->curve) {
        ecdh_set_error(err, "crypto/ecdh: private key and public key curves do not "
                            "match");
        return slice_nil(TYPE_BYTE);
    }
    a = ecdh_alloc(a);
    uint8_t out[NISTEC_MAX_ELEMENT_BYTES];
    Int n;
    if (k->curve->nist == NULL) {
        n = ECDH_X25519_SIZE;
        ecdh_x25519_scalar_mult(out, k->private_key.p, remote->public_key.p);
        if (ecdh_is_zero(out, n)) {
            ecdh_set_error(err, "crypto/ecdh: bad X25519 remote ECDH input: low order "
                                "point");
            return slice_nil(TYPE_BYTE);
        }
    } else {
        const NistecCurve *nc = k->curve->nist;
        if (k->public_key.public_key.len == 1) {
            ecdh_set_error(err, "crypto/ecdh: public key is the identity element");
            return slice_nil(TYPE_BYTE);
        }
        NistecPoint p;
        Error e = BURROW_NO_ERROR;
        if (nc->set_bytes(nc->point_new(&p), remote->public_key, &e) == NULL ||
            nc->scalar_mult(&p, &p, k->private_key, &e) == NULL ||
            nc->bytes_x(&p, out, &e) == NULL) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
        n = nc->element_bytes;
    }
    Slice s = slice_make(a, TYPE_BYTE, n, n);
    if (s.p == NULL) {
        memset(out, 0, sizeof out);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    memcpy(s.p, out, (size_t)n);
    memset(out, 0, sizeof out);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s;
}

static Slice ecdh_clone(Alloc *a, Slice b) {
    a = ecdh_alloc(a);
    Slice s = slice_make(a, TYPE_BYTE, b.len, b.len);
    if (s.p != NULL)
        memcpy(s.p, b.p, (size_t)b.len);
    return s;
}

Slice ecdh_private_key_bytes(const EcdhPrivateKey *k, Alloc *a) {
    return ecdh_clone(a, k->private_key);
}

const EcdhCurve *ecdh_private_key_curve(const EcdhPrivateKey *k) {
    return k->curve;
}

bool ecdh_private_key_equal(const EcdhPrivateKey *k, CryptoPrivateKey x) {
    if (x.t != TYPE_ECDH_PRIVATE_KEY || x.data == NULL)
        return false;
    const EcdhPrivateKey *xx = x.data;
    return k->curve == xx->curve &&
           subtle_constant_time_compare(k->private_key, xx->private_key) == 1;
}

const EcdhPublicKey *ecdh_private_key_public_key(const EcdhPrivateKey *k) {
    return &k->public_key;
}

CryptoPublicKey ecdh_private_key_public(const EcdhPrivateKey *k) {
    return BURROW_ANY(TYPE_ECDH_PUBLIC_KEY, (uintptr_t)&k->public_key);
}

/* -------------------------------------------------------------- PublicKey */

Slice ecdh_public_key_bytes(const EcdhPublicKey *k, Alloc *a) {
    return ecdh_clone(a, k->public_key);
}

const EcdhCurve *ecdh_public_key_curve(const EcdhPublicKey *k) {
    return k->curve;
}

bool ecdh_public_key_equal(const EcdhPublicKey *k, CryptoPublicKey x) {
    if (x.t != TYPE_ECDH_PUBLIC_KEY || x.data == NULL)
        return false;
    const EcdhPublicKey *xx = x.data;
    return k->curve == xx->curve &&
           subtle_constant_time_compare(k->public_key, xx->public_key) == 1;
}

/* ----------------------------------------------------------- KeyExchanger */

static const EcdhPublicKey *ecdh_kx_public_key(void *self) {
    return ecdh_private_key_public_key(self);
}

static const EcdhCurve *ecdh_kx_curve(void *self) {
    return ecdh_private_key_curve(self);
}

static Slice ecdh_kx_ecdh(void *self, Alloc *a, const EcdhPublicKey *remote,
                          Error *err) {
    return ecdh_private_key_ecdh(self, a, remote, err);
}

static const EcdhKeyExchangerVT ecdh_kx_vt = {
    &burrow_type_EcdhPrivateKey,
    ecdh_kx_public_key,
    ecdh_kx_curve,
    ecdh_kx_ecdh,
};

EcdhKeyExchanger ecdh_private_key_key_exchanger(const EcdhPrivateKey *k) {
    EcdhKeyExchanger x = {&ecdh_kx_vt, (void *)(uintptr_t)k};
    return x;
}
