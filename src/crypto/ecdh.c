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
#include "burrow/sync.h"
#include "burrow/type.h"

#include "edwards25519.h"
#include "fips140_module.h"
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

CRYPTO_FIPS_RODATA static const uint8_t ecdh_p256_order[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
    0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

CRYPTO_FIPS_RODATA static const uint8_t ecdh_p384_order[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf, 0x58, 0x1a, 0x0d, 0xb2,
    0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73,
};

CRYPTO_FIPS_RODATA static const uint8_t ecdh_p521_order[66] = {
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

CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_p256(void) {
    return &ecdh_curve_p256;
}
CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_p384(void) {
    return &ecdh_curve_p384;
}
CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_p521(void) {
    return &ecdh_curve_p521;
}
CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_x25519(void) {
    return &ecdh_curve_x25519;
}

CRYPTO_FIPS_TEXT Str ecdh_curve_string(const EcdhCurve *c) {
    return str_from_cstr(c->name);
}

/* ------------------------------------------------------------------- keys */

#define ECDH_X25519_SIZE 32

CRYPTO_FIPS_TEXT static void ecdh_set_error(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
}

CRYPTO_FIPS_TEXT static Alloc *ecdh_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

CRYPTO_FIPS_TEXT static EcdhPublicKey *
ecdh_new_public(const EcdhCurve *c, Alloc *a, const uint8_t *q, Int n, Error *err) {
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

CRYPTO_FIPS_TEXT static EcdhPrivateKey *ecdh_new_private(const EcdhCurve *c, Alloc *a,
                                                         const uint8_t *d, Int dn,
                                                         const uint8_t *q, Int qn,
                                                         Error *err) {
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

CRYPTO_FIPS_TEXT void ecdh_private_key_free(EcdhPrivateKey *k) {
    if (k == NULL)
        return;
    /* The scalar is secret, so it does not go back to the allocator as it
     * is. */
    memset(k->private_key.p, 0, (size_t)k->private_key.len);
    mem_free(k->a, k, k->size, _Alignof(EcdhPrivateKey));
}

CRYPTO_FIPS_TEXT void ecdh_public_key_free(EcdhPublicKey *k) {
    if (k == NULL)
        return;
    mem_free(k->a, k, k->size, _Alignof(EcdhPublicKey));
}

/* --------------------------------------------------------------------- X25519 */

CRYPTO_FIPS_TEXT static void
ecdh_x25519_scalar_mult(uint8_t dst[32], const uint8_t *scalar, const uint8_t *point) {
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

CRYPTO_FIPS_TEXT static int ecdh_is_zero(const uint8_t *x, Int n) {
    uint8_t acc = 0;
    for (Int i = 0; i < n; i++)
        acc |= x[i];
    return acc == 0;
}

CRYPTO_FIPS_TEXT static EcdhPrivateKey *ecdh_x25519_new_private(Alloc *a, Slice key,
                                                                Error *err) {
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
CRYPTO_FIPS_TEXT static Int ecdh_nist_public(const EcdhCurve *c, const uint8_t *key,
                                             uint8_t *out) {
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
CRYPTO_FIPS_TEXT static int ecdh_nist_valid_scalar(const EcdhCurve *c, Slice key) {
    Int n = c->nist->element_bytes;
    if (key.len != n)
        return 0;
    const uint8_t *k = key.p;
    if (ecdh_is_zero(k, n))
        return 0;
    /* key < order is not order <= key. */
    return !burrow__nistec_less_or_eq_bytes(c->order, k, n);
}

CRYPTO_FIPS_TEXT static EcdhPrivateKey *
ecdh_nist_new_private(const EcdhCurve *c, Alloc *a, Slice key, Error *err) {
    if (!ecdh_nist_valid_scalar(c, key)) {
        ecdh_set_error(err, "crypto/ecdh: invalid private key");
        return NULL;
    }
    uint8_t pub[NISTEC_MAX_POINT_BYTES];
    Int n = ecdh_nist_public(c, key.p, pub);
    return ecdh_new_private(c, a, key.p, key.len, pub, n, err);
}

/* fips140/ecdh's ecdh, the shared secret of the private scalar d and the
 * encoded point q into out, without the checks on the keys around it. */
CRYPTO_FIPS_TEXT static bool ecdh_nist_shared(const NistecCurve *nc, Slice d, Slice q,
                                              uint8_t *out, Error *err) {
    /* SetBytes checks that (x, y) are reduced modulo p, and that they are on
     * the curve, performing Steps 2-3 of Section 5.6.2.3.4. */
    NistecPoint p;
    Error e = BURROW_NO_ERROR;
    if (nc->set_bytes(nc->point_new(&p), q, &e) == NULL ||
        nc->scalar_mult(&p, &p, d, &e) == NULL || nc->bytes_x(&p, out, &e) == NULL) {
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- self-test */

/* From Go's src/crypto/internal/fips140/ecdh/cast.go. Per IG D.F, Scenario 2,
 * path (1). */

CRYPTO_FIPS_RODATA static const uint8_t ecdh_cast_private_key[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

CRYPTO_FIPS_RODATA static const uint8_t ecdh_cast_public_key[65] = {
    0x04, 0x51, 0x5c, 0x3d, 0x6e, 0xb9, 0xe3, 0x96, 0xb9, 0x04, 0xd3, 0xfe, 0xca,
    0x7f, 0x54, 0xfd, 0xcd, 0x0c, 0xc1, 0xe9, 0x97, 0xbf, 0x37, 0x5d, 0xca, 0x51,
    0x5a, 0xd0, 0xa6, 0xc3, 0xb4, 0x03, 0x5f, 0x45, 0x36, 0xbe, 0x3a, 0x50, 0xf3,
    0x18, 0xfb, 0xf9, 0xa5, 0x47, 0x59, 0x02, 0xa2, 0x21, 0x50, 0x2b, 0xef, 0x0d,
    0x57, 0xe0, 0x8c, 0x53, 0xb2, 0xcc, 0x0a, 0x56, 0xf1, 0x7d, 0x9f, 0x93, 0x54,
};

CRYPTO_FIPS_RODATA static const uint8_t ecdh_cast_want[32] = {
    0xb4, 0xf1, 0xfc, 0xce, 0x40, 0x73, 0x5f, 0x83, 0x6a, 0xf8, 0xd6,
    0x31, 0x2d, 0x24, 0x8d, 0x1a, 0x83, 0x48, 0x40, 0x56, 0x69, 0xa1,
    0x95, 0xfa, 0xc5, 0x35, 0x04, 0x06, 0xba, 0x76, 0xbc, 0xce,
};

CRYPTO_FIPS_TEXT static Error ecdh_cast(void *arg) {
    (void)arg;
    Int dn = (Int)sizeof ecdh_cast_private_key, qn = (Int)sizeof ecdh_cast_public_key;
    Slice d = slice_from((void *)(uintptr_t)ecdh_cast_private_key, dn, dn, TYPE_BYTE);
    Slice q = slice_from((void *)(uintptr_t)ecdh_cast_public_key, qn, qn, TYPE_BYTE);
    uint8_t got[32];
    Error err = BURROW_NO_ERROR;
    if (!ecdh_nist_shared(&burrow__nistec_p256, d, q, got, &err))
        return err;
    if (memcmp(got, ecdh_cast_want, sizeof got) != 0)
        return errors_new(error_allocator(), BURROW_S("unexpected result"));
    return BURROW_NO_ERROR;
}

CRYPTO_FIPS_TEXT static void ecdh_self_test_once(void *env) {
    (void)env;
    burrow__fips140_cast("KAS-ECC-SSC P-256", ecdh_cast, NULL);
}

static SyncOnce ecdh_self_test;

/* fipsSelfTest, Go's sync.OnceFunc. */
CRYPTO_FIPS_TEXT static void ecdh_fips_self_test(void) {
    sync_once_do(&ecdh_self_test, BURROW_FN(Func, ecdh_self_test_once, NULL));
}

/* The "ECDH PCT" of fips140/ecdh's GenerateKey. A "Pairwise Consistency Test"
 * makes no sense if we just generated the public key from an ephemeral private
 * key. Moreover, there is no way to check it aside from redoing the exact same
 * computation again. SP 800-56A Rev. 3, Section 5.6.2.1.4 acknowledges that,
 * and doesn't require it. However, ISO 19790:2012, Section 7.10.3.3 has a
 * blanket requirement for a PCT for all generated keys (AS10.35) and FIPS 140-3
 * IG 10.3.A, Additional Comment 1 goes out of its way to say that "the PCT
 * shall be performed consistent [...], even if the underlying standard does not
 * require a PCT". So we do it. And make ECDH nearly 50% slower (only) in FIPS
 * mode. */
CRYPTO_FIPS_TEXT static Error ecdh_pct(void *arg) {
    const EcdhPrivateKey *k = arg;
    const NistecCurve *nc = k->curve->nist;
    NistecPoint p1;
    Error e = BURROW_NO_ERROR;
    if (nc->scalar_base_mult(&p1, k->private_key, &e) == NULL)
        return e;
    uint8_t q[NISTEC_MAX_POINT_BYTES];
    Int n = nc->bytes(&p1, q);
    const Slice *pub = &k->public_key.public_key;
    if (n != pub->len || memcmp(q, pub->p, (size_t)n) != 0)
        return errors_new(
            error_allocator(),
            BURROW_S("crypto/ecdh: public key does not match private key"));
    return BURROW_NO_ERROR;
}

/* drbg.ReadWithReader: the system generator for the default reader, which is
 * what drbg.Read is outside FIPS mode, and io.ReadFull for anything else. */
CRYPTO_FIPS_TEXT static int ecdh_read(IoReader r, uint8_t *p, Int n, Error *err) {
    Slice s = slice_from(p, n, n, TYPE_BYTE);
    if (burrow__crypto_rand_is_default_reader(r)) {
        burrow__crypto_rand_system(s);
        return 1;
    }
    burrow__fips140_record_non_approved();
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(r, s, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ Curve */

CRYPTO_FIPS_TEXT EcdhPrivateKey *ecdh_curve_generate_key(const EcdhCurve *c, Alloc *a,
                                                         IoReader rand, Error *err) {
    if (rand.vt == NULL)
        rand = burrow__crypto_rand_nil_reader();
    rand = burrow__crypto_rand_custom_reader(rand);

    if (c->nist == NULL) {
        /* io.ReadFull, as X25519 is outside the module and has no use for
         * drbg.ReadWithReader. */
        uint8_t key[ECDH_X25519_SIZE];
        Slice ks = slice_from(key, ECDH_X25519_SIZE, ECDH_X25519_SIZE, TYPE_BYTE);
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, ks, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        EcdhPrivateKey *k = ecdh_x25519_new_private(a, ks, err);
        memset(key, 0, sizeof key);
        return k;
    }

    /* fips140/ecdh.GenerateKey: random bytes until they are a valid scalar,
     * which for P-256 fails about once in 2^32 tries and for the others less
     * often than that. The xor keeps a reader that gives all zeros or all ones
     * from making a key, since either is a bad scalar, and P-521 drops the
     * seven bits above its 521. */
    burrow__fips140_record_approved();
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
            if (k != NULL)
                burrow__fips140_pct("ECDH PCT", ecdh_pct, k);
            return k;
        }
    }
}

CRYPTO_FIPS_TEXT EcdhPrivateKey *
ecdh_curve_new_private_key(const EcdhCurve *c, Alloc *a, Slice key, Error *err) {
    if (c->nist == NULL)
        return ecdh_x25519_new_private(a, key, err);
    return ecdh_nist_new_private(c, a, key, err);
}

CRYPTO_FIPS_TEXT EcdhPublicKey *ecdh_curve_new_public_key(const EcdhCurve *c, Alloc *a,
                                                          Slice key, Error *err) {
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

CRYPTO_FIPS_TEXT Slice ecdh_private_key_ecdh(const EcdhPrivateKey *k, Alloc *a,
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
        ecdh_fips_self_test();
        burrow__fips140_record_approved();
        if (k->public_key.public_key.len == 1) {
            ecdh_set_error(err, "crypto/ecdh: public key is the identity element");
            return slice_nil(TYPE_BYTE);
        }
        if (!ecdh_nist_shared(nc, k->private_key, remote->public_key, out, err))
            return slice_nil(TYPE_BYTE);
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

CRYPTO_FIPS_TEXT static Slice ecdh_clone(Alloc *a, Slice b) {
    a = ecdh_alloc(a);
    Slice s = slice_make(a, TYPE_BYTE, b.len, b.len);
    if (s.p != NULL)
        memcpy(s.p, b.p, (size_t)b.len);
    return s;
}

CRYPTO_FIPS_TEXT Slice ecdh_private_key_bytes(const EcdhPrivateKey *k, Alloc *a) {
    return ecdh_clone(a, k->private_key);
}

CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_private_key_curve(const EcdhPrivateKey *k) {
    return k->curve;
}

CRYPTO_FIPS_TEXT bool ecdh_private_key_equal(const EcdhPrivateKey *k,
                                             CryptoPrivateKey x) {
    if (x.t != TYPE_ECDH_PRIVATE_KEY || x.data == NULL)
        return false;
    const EcdhPrivateKey *xx = x.data;
    return k->curve == xx->curve &&
           subtle_constant_time_compare(k->private_key, xx->private_key) == 1;
}

CRYPTO_FIPS_TEXT const EcdhPublicKey *
ecdh_private_key_public_key(const EcdhPrivateKey *k) {
    return &k->public_key;
}

CRYPTO_FIPS_TEXT CryptoPublicKey ecdh_private_key_public(const EcdhPrivateKey *k) {
    return BURROW_ANY(TYPE_ECDH_PUBLIC_KEY, (uintptr_t)&k->public_key);
}

/* -------------------------------------------------------------- PublicKey */

CRYPTO_FIPS_TEXT Slice ecdh_public_key_bytes(const EcdhPublicKey *k, Alloc *a) {
    return ecdh_clone(a, k->public_key);
}

CRYPTO_FIPS_TEXT const EcdhCurve *ecdh_public_key_curve(const EcdhPublicKey *k) {
    return k->curve;
}

CRYPTO_FIPS_TEXT bool ecdh_public_key_equal(const EcdhPublicKey *k, CryptoPublicKey x) {
    if (x.t != TYPE_ECDH_PUBLIC_KEY || x.data == NULL)
        return false;
    const EcdhPublicKey *xx = x.data;
    return k->curve == xx->curve &&
           subtle_constant_time_compare(k->public_key, xx->public_key) == 1;
}

/* ----------------------------------------------------------- KeyExchanger */

CRYPTO_FIPS_TEXT static const EcdhPublicKey *ecdh_kx_public_key(void *self) {
    return ecdh_private_key_public_key(self);
}

CRYPTO_FIPS_TEXT static const EcdhCurve *ecdh_kx_curve(void *self) {
    return ecdh_private_key_curve(self);
}

CRYPTO_FIPS_TEXT static Slice ecdh_kx_ecdh(void *self, Alloc *a,
                                           const EcdhPublicKey *remote, Error *err) {
    return ecdh_private_key_ecdh(self, a, remote, err);
}

static const EcdhKeyExchangerVT ecdh_kx_vt = {
    &burrow_type_EcdhPrivateKey,
    ecdh_kx_public_key,
    ecdh_kx_curve,
    ecdh_kx_ecdh,
};

CRYPTO_FIPS_TEXT EcdhKeyExchanger
ecdh_private_key_key_exchanger(const EcdhPrivateKey *k) {
    EcdhKeyExchanger x = {&ecdh_kx_vt, (void *)(uintptr_t)k};
    return x;
}
