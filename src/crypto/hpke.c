/* Derived from Go's src/crypto/hpke: hpke.go, aead.go, aead_fips140v1.26.go,
 * kdf.go, kem.go and pq.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/hpke.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "chacha20poly1305.h"
#include "hpke_internal.h"
#include "rand_internal.h"
#include "sha3_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static void hpke_set_error(Error *err, const char *msg) {
    *err = errors_new(error_allocator(), str_from_cstr(msg));
}

/* A piece of a byte string being put together. */
typedef struct HpkePart {
    const void *p;
    Int len;
} HpkePart;

static HpkePart hpke_part(Slice s) {
    HpkePart p = {s.p, s.len};
    return p;
}

static HpkePart hpke_label(const char *s) {
    HpkePart p = {s, (Int)strlen(s)};
    return p;
}

/* The parts one after the other, in a new slice from a. */
static Slice hpke_concat(Alloc *a, const HpkePart *parts, size_t n, Error *err) {
    Int total = 0;
    for (size_t i = 0; i < n; i++)
        total += parts[i].len;
    Slice out = slice_make(a, TYPE_BYTE, total, total);
    if (out.p == NULL && total > 0) {
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_BYTE);
    }
    Byte *q = out.p;
    for (size_t i = 0; i < n; i++) {
        if (parts[i].len > 0) {
            memcpy(q, parts[i].p, (size_t)parts[i].len);
            q += parts[i].len;
        }
    }
    return out;
}

static Slice hpke_bytes(void *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static void hpke_put16(Byte b[2], uint16_t v) {
    b[0] = (Byte)(v >> 8);
    b[1] = (Byte)v;
}

static void hpke_wipe(Slice s) {
    if (s.p != NULL && s.cap > 0)
        memset(s.p, 0, (size_t)s.cap);
}

/* A copy of b from a, for a result worked out in a scratch arena. */
static Slice hpke_clone(Alloc *a, Slice b, Error *err) {
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    HpkePart part = {b.p, b.len};
    return hpke_concat(a, &part, 1, err);
}

/* --------------------------------------------------------------------- KDFs */

/* kdf.go: HKDF with a hash is two-stage, and SHAKE, which has hash NULL, is
 * one-stage. */
struct HpkeKDF {
    uint16_t id;
    Int nh;
    HashNewFunc hash;
};

static const HpkeKDF hpke_kdf_hkdf_sha256 = {0x0001, 32, sha256_new};
static const HpkeKDF hpke_kdf_hkdf_sha384 = {0x0002, 48, sha512_new384};
static const HpkeKDF hpke_kdf_hkdf_sha512 = {0x0003, 64, sha512_new};
static const HpkeKDF hpke_kdf_shake128 = {0x0010, 32, NULL};
static const HpkeKDF hpke_kdf_shake256 = {0x0011, 64, NULL};

const HpkeKDF *hpke_hkdfsha256(void) {
    return &hpke_kdf_hkdf_sha256;
}

const HpkeKDF *hpke_hkdfsha384(void) {
    return &hpke_kdf_hkdf_sha384;
}

const HpkeKDF *hpke_hkdfsha512(void) {
    return &hpke_kdf_hkdf_sha512;
}

const HpkeKDF *hpke_shake128(void) {
    return &hpke_kdf_shake128;
}

const HpkeKDF *hpke_shake256(void) {
    return &hpke_kdf_shake256;
}

const HpkeKDF *hpke_new_kdf(uint16_t id, Error *err) {
    const HpkeKDF *kdf = NULL;
    switch (id) {
    case 0x0001:
        kdf = &hpke_kdf_hkdf_sha256;
        break;
    case 0x0002:
        kdf = &hpke_kdf_hkdf_sha384;
        break;
    case 0x0003:
        kdf = &hpke_kdf_hkdf_sha512;
        break;
    case 0x0010:
        kdf = &hpke_kdf_shake128;
        break;
    case 0x0011:
        kdf = &hpke_kdf_shake256;
        break;
    default:
        BURROW_OUT(err, fmt_errorf_v("unsupported KDF %04x", (int)id));
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return kdf;
}

uint16_t hpke_kdf_id(const HpkeKDF *kdf) {
    return kdf->id;
}

static bool hpke_kdf_one_stage(const HpkeKDF *kdf) {
    return kdf->hash == NULL;
}

/* LabeledExtract of RFC 9180: HKDF-Extract of "HPKE-v1" || suite_id || label ||
 * ikm, with salt. */
static Slice hpke_labeled_extract(Alloc *a, const HpkeKDF *kdf, Slice suite_id,
                                  Slice salt, const char *label, Slice ikm,
                                  Error *err) {
    HpkePart parts[] = {hpke_label("HPKE-v1"), hpke_part(suite_id), hpke_label(label),
                        hpke_part(ikm)};
    Slice labeled = hpke_concat(a, parts, sizeof parts / sizeof parts[0], err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    Slice prk = hkdf_extract(a, kdf->hash, labeled, salt, err);
    hpke_wipe(labeled);
    return prk;
}

/* LabeledExpand: HKDF-Expand of prk with the info I2OSP(length, 2) ||
 * "HPKE-v1" || suite_id || label || info. */
static Slice hpke_labeled_expand(Alloc *a, const HpkeKDF *kdf, Slice suite_id,
                                 Slice prk, const char *label, Slice info,
                                 uint16_t length, Error *err) {
    Byte l[2];
    hpke_put16(l, length);
    HpkePart parts[] = {{l, 2},
                        hpke_label("HPKE-v1"),
                        hpke_part(suite_id),
                        hpke_label(label),
                        hpke_part(info)};
    Slice labeled = hpke_concat(a, parts, sizeof parts / sizeof parts[0], err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    return hkdf_expand(a, kdf->hash, prk, str_from_bytes(labeled.p, labeled.len),
                       length, err);
}

/* LabeledDerive of the one-stage KDFs: length bytes of SHAKE of ikm ||
 * "HPKE-v1" || suite_id || I2OSP(len(label), 2) || label || I2OSP(length, 2) ||
 * context. */
static Slice hpke_labeled_derive(Alloc *a, const HpkeKDF *kdf, Slice suite_id,
                                 Slice ikm, const char *label, Slice context,
                                 uint16_t length, Error *err) {
    Sha3 h;
    if (kdf->id == 0x0010)
        burrow__shake128_init(&h);
    else
        burrow__shake256_init(&h);
    Byte ll[2], l[2];
    Int label_len = (Int)strlen(label);
    hpke_put16(ll, (uint16_t)label_len);
    hpke_put16(l, length);
    burrow__sha3_write(&h, ikm.p, ikm.len);
    burrow__sha3_write(&h, "HPKE-v1", 7);
    burrow__sha3_write(&h, suite_id.p, suite_id.len);
    burrow__sha3_write(&h, ll, 2);
    burrow__sha3_write(&h, label, label_len);
    burrow__sha3_write(&h, l, 2);
    burrow__sha3_write(&h, context.p, context.len);
    Slice out = slice_make(a, TYPE_BYTE, length, length);
    if (out.p == NULL && length > 0) {
        memset(&h, 0, sizeof h);
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_BYTE);
    }
    burrow__sha3_read(&h, out.p, length);
    memset(&h, 0, sizeof h);
    return out;
}

/* -------------------------------------------------------------------- AEADs */

typedef enum HpkeAeadKind {
    HPKE_AEAD_AES_GCM,
    HPKE_AEAD_CHACHA20POLY1305,
    HPKE_AEAD_EXPORT_ONLY,
} HpkeAeadKind;

struct HpkeAEAD {
    uint16_t id;
    Int nk, nn;
    HpkeAeadKind kind;
};

static const HpkeAEAD hpke_aead_aes128_gcm = {0x0001, 16, 12, HPKE_AEAD_AES_GCM};
static const HpkeAEAD hpke_aead_aes256_gcm = {0x0002, 32, 12, HPKE_AEAD_AES_GCM};
static const HpkeAEAD hpke_aead_chacha20poly1305 = {0x0003, CHACHA20POLY1305_KEY_SIZE,
                                                    CHACHA20POLY1305_NONCE_SIZE,
                                                    HPKE_AEAD_CHACHA20POLY1305};
static const HpkeAEAD hpke_aead_export_only = {0xffff, 0, 0, HPKE_AEAD_EXPORT_ONLY};

const HpkeAEAD *hpke_aes128_gcm(void) {
    return &hpke_aead_aes128_gcm;
}

const HpkeAEAD *hpke_aes256_gcm(void) {
    return &hpke_aead_aes256_gcm;
}

const HpkeAEAD *hpke_cha_cha20_poly1305(void) {
    return &hpke_aead_chacha20poly1305;
}

const HpkeAEAD *hpke_export_only(void) {
    return &hpke_aead_export_only;
}

const HpkeAEAD *hpke_new_aead(uint16_t id, Error *err) {
    const HpkeAEAD *aead = NULL;
    switch (id) {
    case 0x0001:
        aead = &hpke_aead_aes128_gcm;
        break;
    case 0x0002:
        aead = &hpke_aead_aes256_gcm;
        break;
    case 0x0003:
        aead = &hpke_aead_chacha20poly1305;
        break;
    case 0xffff:
        aead = &hpke_aead_export_only;
        break;
    default:
        BURROW_OUT(err, fmt_errorf_v("unsupported AEAD %04x", (int)id));
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return aead;
}

uint16_t hpke_aead_id(const HpkeAEAD *aead) {
    return aead->id;
}

/* The cipher.AEAD for key, or a nil one for export only. Go makes AES-GCM with
 * NewGCMForHPKE, which only differs from the plain GCM in FIPS 140 mode. */
static CipherAEAD hpke_aead_cipher(Alloc *a, const HpkeAEAD *aead, Slice key,
                                   Error *err) {
    CipherAEAD none = {NULL, NULL};
    if (aead->kind == HPKE_AEAD_EXPORT_ONLY)
        return none;
    if (key.len != aead->nk) {
        hpke_set_error(err, "invalid key size");
        return none;
    }
    CipherAEAD c;
    if (aead->kind == HPKE_AEAD_AES_GCM) {
        CipherBlock b = aes_new_cipher(a, key, err);
        if (BURROW_FAILED(*err))
            return none;
        c = cipher_new_gcm(a, b, err);
    } else {
        c = chacha20poly1305_new(a, key, err);
    }
    if (BURROW_FAILED(*err))
        return none;
    if (c.vt == NULL)
        *err = burrow_err_out_of_memory;
    return c;
}

/* --------------------------------------------------------------------- KEMs */

typedef enum HpkeKemKind {
    HPKE_KEM_UNSUPPORTED,
    HPKE_KEM_DH,
    HPKE_KEM_MLKEM,
    HPKE_KEM_HYBRID,
} HpkeKemKind;

/* kem.go's dhKEM, pq.go's mlkemKEM and hybridKEM, and unsupportedCurveKEM, as
 * one struct with the fields each one uses. */
struct HpkeKEM {
    uint16_t id;
    HpkeKemKind kind;
    Int enc_size;
    /* DHKEM and the hybrids. */
    const EcdhCurve *(*curve)(void);
    /* DHKEM. */
    const HpkeKDF *kdf;
    uint16_t n_secret, n_sk;
    /* ML-KEM and the hybrids: 768 or 1024. */
    int pq;
    /* The hybrids. */
    const char *label;
    Int curve_seed_size, curve_point_size;
};

static const HpkeKEM hpke_kem_unsupported_curve = {.id = 0,
                                                   .kind = HPKE_KEM_UNSUPPORTED};

static const HpkeKEM hpke_kem_dh_p256 = {
    .id = 0x0010,
    .kind = HPKE_KEM_DH,
    .enc_size = 65,
    .curve = ecdh_p256,
    .kdf = &hpke_kdf_hkdf_sha256,
    .n_secret = 32,
    .n_sk = 32,
};

static const HpkeKEM hpke_kem_dh_p384 = {
    .id = 0x0011,
    .kind = HPKE_KEM_DH,
    .enc_size = 97,
    .curve = ecdh_p384,
    .kdf = &hpke_kdf_hkdf_sha384,
    .n_secret = 48,
    .n_sk = 48,
};

static const HpkeKEM hpke_kem_dh_p521 = {
    .id = 0x0012,
    .kind = HPKE_KEM_DH,
    .enc_size = 133,
    .curve = ecdh_p521,
    .kdf = &hpke_kdf_hkdf_sha512,
    .n_secret = 64,
    .n_sk = 66,
};

static const HpkeKEM hpke_kem_dh_x25519 = {
    .id = 0x0020,
    .kind = HPKE_KEM_DH,
    .enc_size = 32,
    .curve = ecdh_x25519,
    .kdf = &hpke_kdf_hkdf_sha256,
    .n_secret = 32,
    .n_sk = 32,
};

static const HpkeKEM hpke_kem_mlkem_768 = {
    .id = 0x0041,
    .kind = HPKE_KEM_MLKEM,
    .enc_size = MLKEM_CIPHERTEXT_SIZE768,
    .pq = 768,
};

static const HpkeKEM hpke_kem_mlkem_1024 = {
    .id = 0x0042,
    .kind = HPKE_KEM_MLKEM,
    .enc_size = MLKEM_CIPHERTEXT_SIZE1024,
    .pq = 1024,
};

static const HpkeKEM hpke_kem_xwing = {
    .id = 0x647a,
    .kind = HPKE_KEM_HYBRID,
    .enc_size = MLKEM_CIPHERTEXT_SIZE768 + 32,
    .curve = ecdh_x25519,
    .pq = 768,
    .label = "\\./"
             "/^\\",
    .curve_seed_size = 32,
    .curve_point_size = 32,
};

static const HpkeKEM hpke_kem_mlkem768_p256 = {
    .id = 0x0050,
    .kind = HPKE_KEM_HYBRID,
    .enc_size = MLKEM_CIPHERTEXT_SIZE768 + 65,
    .curve = ecdh_p256,
    .pq = 768,
    .label = "MLKEM768-P256",
    .curve_seed_size = 32,
    .curve_point_size = 65,
};

static const HpkeKEM hpke_kem_mlkem1024_p384 = {
    .id = 0x0051,
    .kind = HPKE_KEM_HYBRID,
    .enc_size = MLKEM_CIPHERTEXT_SIZE1024 + 97,
    .curve = ecdh_p384,
    .pq = 1024,
    .label = "MLKEM1024-P384",
    .curve_seed_size = 48,
    .curve_point_size = 97,
};

const HpkeKEM *hpke_dhkem(const EcdhCurve *curve) {
    if (curve == ecdh_p256())
        return &hpke_kem_dh_p256;
    if (curve == ecdh_p384())
        return &hpke_kem_dh_p384;
    if (curve == ecdh_p521())
        return &hpke_kem_dh_p521;
    if (curve == ecdh_x25519())
        return &hpke_kem_dh_x25519;
    return &hpke_kem_unsupported_curve;
}

const HpkeKEM *hpke_mlkem768(void) {
    return &hpke_kem_mlkem_768;
}

const HpkeKEM *hpke_mlkem1024(void) {
    return &hpke_kem_mlkem_1024;
}

const HpkeKEM *hpke_mlkem768_x25519(void) {
    return &hpke_kem_xwing;
}

const HpkeKEM *hpke_mlkem768_p256(void) {
    return &hpke_kem_mlkem768_p256;
}

const HpkeKEM *hpke_mlkem1024_p384(void) {
    return &hpke_kem_mlkem1024_p384;
}

const HpkeKEM *hpke_new_kem(uint16_t id, Error *err) {
    const HpkeKEM *kem = NULL;
    switch (id) {
    case 0x0010:
        kem = &hpke_kem_dh_p256;
        break;
    case 0x0011:
        kem = &hpke_kem_dh_p384;
        break;
    case 0x0012:
        kem = &hpke_kem_dh_p521;
        break;
    case 0x0020:
        kem = &hpke_kem_dh_x25519;
        break;
    case 0x0041:
        kem = &hpke_kem_mlkem_768;
        break;
    case 0x0042:
        kem = &hpke_kem_mlkem_1024;
        break;
    case 0x647a:
        kem = &hpke_kem_xwing;
        break;
    case 0x0050:
        kem = &hpke_kem_mlkem768_p256;
        break;
    case 0x0051:
        kem = &hpke_kem_mlkem1024_p384;
        break;
    default:
        BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("unsupported KEM")));
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return kem;
}

uint16_t hpke_kem_id(const HpkeKEM *kem) {
    return kem->id;
}

Int burrow__hpke_kem_enc_size(const HpkeKEM *kem) {
    return kem->enc_size;
}

/* "KEM" || I2OSP(kem_id, 2), the suite_id of the KEM's own derivations. */
static Slice hpke_kem_suite_id(Byte sid[5], const HpkeKEM *kem) {
    memcpy(sid, "KEM", 3);
    hpke_put16(sid + 3, kem->id);
    return hpke_bytes(sid, 5);
}

static Int hpke_pq_ek_size(const HpkeKEM *kem) {
    return kem->pq == 768 ? MLKEM_ENCAPSULATION_KEY_SIZE768
                          : MLKEM_ENCAPSULATION_KEY_SIZE1024;
}

static Int hpke_pq_ct_size(const HpkeKEM *kem) {
    return kem->pq == 768 ? MLKEM_CIPHERTEXT_SIZE768 : MLKEM_CIPHERTEXT_SIZE1024;
}

static const HpkeKEM *hpke_kem_for_curve_hybrid(const EcdhCurve *curve) {
    if (curve == ecdh_x25519())
        return &hpke_kem_xwing;
    if (curve == ecdh_p256())
        return &hpke_kem_mlkem768_p256;
    if (curve == ecdh_p384())
        return &hpke_kem_mlkem1024_p384;
    return NULL;
}

static int hpke_encapsulator_pq(CryptoEncapsulator e) {
    if (e.vt != NULL && e.vt->self_type == TYPE_MLKEM_ENCAPSULATION_KEY768)
        return 768;
    if (e.vt != NULL && e.vt->self_type == TYPE_MLKEM_ENCAPSULATION_KEY1024)
        return 1024;
    return 0;
}

/* -------------------------------------------------------------- public keys */

/* The curve part, the ML-KEM part or both, with the keys it made itself, which
 * it frees, apart from the ones it borrows. A private key keeps its public key
 * in one of these with a NULL a, so that it is never freed on its own. */
struct HpkePublicKey {
    const HpkeKEM *kem;
    const EcdhPublicKey *t;
    CryptoEncapsulator pq;
    EcdhPublicKey *own_t;
    MlkemEncapsulationKey768 *own_ek768;
    MlkemEncapsulationKey1024 *own_ek1024;
    Alloc *a;
};

static HpkePublicKey *hpke_public_key_alloc(Alloc *a, const HpkeKEM *kem, Error *err) {
    HpkePublicKey *pk = BURROW_NEW(a, HpkePublicKey);
    if (pk == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    pk->kem = kem;
    pk->a = a;
    return pk;
}

void hpke_public_key_free(HpkePublicKey *k) {
    if (k == NULL || k->a == NULL)
        return;
    ecdh_public_key_free(k->own_t);
    mlkem_encapsulation_key768_free(k->own_ek768);
    mlkem_encapsulation_key1024_free(k->own_ek1024);
    mem_free(k->a, k, sizeof *k, _Alignof(HpkePublicKey));
}

const HpkeKEM *hpke_public_key_kem(const HpkePublicKey *pk) {
    return pk->kem;
}

Slice hpke_public_key_bytes(const HpkePublicKey *pk, Alloc *a) {
    switch (pk->kem->kind) {
    case HPKE_KEM_DH:
        return ecdh_public_key_bytes(pk->t, a);
    case HPKE_KEM_MLKEM:
        return crypto_encapsulator_bytes(pk->pq, a);
    case HPKE_KEM_HYBRID: {
        Arena scratch;
        arena_init(&scratch, NULL, 0);
        Alloc *s = arena_allocator(&scratch);
        HpkePart parts[] = {hpke_part(crypto_encapsulator_bytes(pk->pq, s)),
                            hpke_part(ecdh_public_key_bytes(pk->t, s))};
        Error e = BURROW_NO_ERROR;
        Slice out = hpke_concat(a, parts, 2, &e);
        arena_free(&scratch);
        return out;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        return slice_nil(TYPE_BYTE);
    }
}

HpkePublicKey *hpke_new_dhkem_public_key(Alloc *a, const EcdhPublicKey *pub,
                                         Error *err) {
    const HpkeKEM *kem = hpke_dhkem(ecdh_public_key_curve(pub));
    Error e = BURROW_NO_ERROR;
    HpkePublicKey *pk = NULL;
    if (kem->kind != HPKE_KEM_DH)
        hpke_set_error(&e, "unsupported curve");
    else if ((pk = hpke_public_key_alloc(a, kem, &e)) != NULL)
        pk->t = pub;
    BURROW_OUT(err, e);
    return pk;
}

static HpkePublicKey *hpke_hybrid_public_key(Alloc *a, CryptoEncapsulator pq,
                                             const EcdhPublicKey *t, Error *err) {
    const EcdhCurve *curve = ecdh_public_key_curve(t);
    const HpkeKEM *kem = hpke_kem_for_curve_hybrid(curve);
    if (kem == NULL) {
        hpke_set_error(err, "unsupported curve");
        return NULL;
    }
    if (hpke_encapsulator_pq(pq) != kem->pq) {
        hpke_set_error(err, kem == &hpke_kem_xwing ? "invalid PQ KEM for X25519 hybrid"
                            : kem == &hpke_kem_mlkem768_p256
                                ? "invalid PQ KEM for P-256 hybrid"
                                : "invalid PQ KEM for P-384 hybrid");
        return NULL;
    }
    HpkePublicKey *pk = hpke_public_key_alloc(a, kem, err);
    if (pk == NULL)
        return NULL;
    pk->t = t;
    pk->pq = pq;
    return pk;
}

HpkePublicKey *hpke_new_hybrid_public_key(Alloc *a, CryptoEncapsulator pq,
                                          const EcdhPublicKey *t, Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePublicKey *pk = hpke_hybrid_public_key(a, pq, t, &e);
    BURROW_OUT(err, e);
    return pk;
}

static HpkePublicKey *hpke_mlkem_public_key(Alloc *a, CryptoEncapsulator pub,
                                            Error *err) {
    int pq = hpke_encapsulator_pq(pub);
    if (pq == 0) {
        hpke_set_error(err, "unsupported public key type");
        return NULL;
    }
    HpkePublicKey *pk = hpke_public_key_alloc(
        a, pq == 768 ? &hpke_kem_mlkem_768 : &hpke_kem_mlkem_1024, err);
    if (pk != NULL)
        pk->pq = pub;
    return pk;
}

HpkePublicKey *hpke_new_mlkem_public_key(Alloc *a, CryptoEncapsulator pub, Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePublicKey *pk = hpke_mlkem_public_key(a, pub, &e);
    BURROW_OUT(err, e);
    return pk;
}

/* An ML-KEM encapsulation key of the size kem has, which the public key it
 * goes into will own. */
static CryptoEncapsulator hpke_new_ek(Alloc *a, const HpkeKEM *kem, Slice data,
                                      MlkemEncapsulationKey768 **ek768,
                                      MlkemEncapsulationKey1024 **ek1024, Error *err) {
    CryptoEncapsulator none = {NULL, NULL};
    if (kem->pq == 768) {
        *ek768 = mlkem_new_encapsulation_key768(a, data, err);
        if (*ek768 == NULL)
            return none;
        return mlkem_encapsulation_key768_as_encapsulator(*ek768);
    }
    *ek1024 = mlkem_new_encapsulation_key1024(a, data, err);
    if (*ek1024 == NULL)
        return none;
    return mlkem_encapsulation_key1024_as_encapsulator(*ek1024);
}

static HpkePublicKey *hpke_kem_public_key(const HpkeKEM *kem, Alloc *a, Slice data,
                                          Error *err) {
    switch (kem->kind) {
    case HPKE_KEM_DH: {
        EcdhPublicKey *pub = ecdh_curve_new_public_key(kem->curve(), a, data, err);
        if (BURROW_FAILED(*err))
            return NULL;
        HpkePublicKey *pk = hpke_public_key_alloc(a, kem, err);
        if (pk == NULL) {
            ecdh_public_key_free(pub);
            return NULL;
        }
        pk->t = pub;
        pk->own_t = pub;
        return pk;
    }
    case HPKE_KEM_MLKEM: {
        MlkemEncapsulationKey768 *ek768 = NULL;
        MlkemEncapsulationKey1024 *ek1024 = NULL;
        CryptoEncapsulator pq = hpke_new_ek(a, kem, data, &ek768, &ek1024, err);
        if (BURROW_FAILED(*err))
            return NULL;
        HpkePublicKey *pk = hpke_mlkem_public_key(a, pq, err);
        if (pk == NULL) {
            mlkem_encapsulation_key768_free(ek768);
            mlkem_encapsulation_key1024_free(ek1024);
            return NULL;
        }
        pk->own_ek768 = ek768;
        pk->own_ek1024 = ek1024;
        return pk;
    }
    case HPKE_KEM_HYBRID: {
        Int ek_size = hpke_pq_ek_size(kem);
        if (data.len != ek_size + kem->curve_point_size) {
            hpke_set_error(err, "invalid public key size");
            return NULL;
        }
        MlkemEncapsulationKey768 *ek768 = NULL;
        MlkemEncapsulationKey1024 *ek1024 = NULL;
        CryptoEncapsulator pq =
            hpke_new_ek(a, kem, slice_sub(data, 0, ek_size), &ek768, &ek1024, err);
        if (BURROW_FAILED(*err))
            return NULL;
        EcdhPublicKey *t = ecdh_curve_new_public_key(
            kem->curve(), a, slice_sub(data, ek_size, data.len), err);
        HpkePublicKey *pk = NULL;
        if (BURROW_OK(*err))
            pk = hpke_hybrid_public_key(a, pq, t, err);
        if (pk == NULL) {
            ecdh_public_key_free(t);
            mlkem_encapsulation_key768_free(ek768);
            mlkem_encapsulation_key1024_free(ek1024);
            return NULL;
        }
        pk->own_t = t;
        pk->own_ek768 = ek768;
        pk->own_ek1024 = ek1024;
        return pk;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(err, "unsupported curve");
        return NULL;
    }
}

HpkePublicKey *hpke_kem_new_public_key(const HpkeKEM *kem, Alloc *a, Slice data,
                                       Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePublicKey *pk = hpke_kem_public_key(kem, a, data, &e);
    BURROW_OUT(err, e);
    return pk;
}

/* ------------------------------------------------------------- private keys */

/* The key exchanger, the decapsulator or both, the public key made of their
 * public halves, and the 32 byte seed of a hybrid when there is one. */
struct HpkePrivateKey {
    const HpkeKEM *kem;
    EcdhKeyExchanger t;
    CryptoDecapsulator pq;
    bool has_seed;
    Byte seed[32];
    HpkePublicKey pub;
    EcdhPrivateKey *own_t;
    MlkemDecapsulationKey768 *own_dk768;
    MlkemDecapsulationKey1024 *own_dk1024;
    Alloc *a;
};

/* A private key for kem over t, pq or both, with its public key filled in. */
static HpkePrivateKey *hpke_private_key_alloc(Alloc *a, const HpkeKEM *kem,
                                              EcdhKeyExchanger t, CryptoDecapsulator pq,
                                              Error *err) {
    HpkePrivateKey *k = BURROW_NEW(a, HpkePrivateKey);
    if (k == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    k->kem = kem;
    k->t = t;
    k->pq = pq;
    k->a = a;
    k->pub.kem = kem;
    if (t.vt != NULL)
        k->pub.t = ecdh_key_exchanger_public_key(t);
    if (pq.vt != NULL)
        k->pub.pq = crypto_decapsulator_encapsulator(pq);
    return k;
}

void hpke_private_key_free(HpkePrivateKey *k) {
    if (k == NULL)
        return;
    ecdh_private_key_free(k->own_t);
    mlkem_decapsulation_key768_free(k->own_dk768);
    mlkem_decapsulation_key1024_free(k->own_dk1024);
    memset(k->seed, 0, sizeof k->seed);
    mem_free(k->a, k, sizeof *k, _Alignof(HpkePrivateKey));
}

const HpkeKEM *hpke_private_key_kem(const HpkePrivateKey *k) {
    return k->kem;
}

const HpkePublicKey *hpke_private_key_public_key(const HpkePrivateKey *k) {
    return &k->pub;
}

Slice hpke_private_key_bytes(const HpkePrivateKey *k, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice out = slice_nil(TYPE_BYTE);
    switch (k->kem->kind) {
    case HPKE_KEM_DH:
        if (k->t.vt->self_type != TYPE_ECDH_PRIVATE_KEY) {
            hpke_set_error(&e, "ecdh: private key does not support Bytes");
            break;
        }
        out = ecdh_private_key_bytes(k->t.data, a);
        /* RFC 9180, section 7.1.2, has SerializePrivateKey clamp an X25519
         * key, and DeserializePrivateKey clamp it too, so what comes out does
         * not have to be what went in. */
        if (k->kem == &hpke_kem_dh_x25519 && out.len == 32) {
            Byte *b = out.p;
            b[0] &= 248;
            b[31] &= 127;
            b[31] |= 64;
        }
        break;
    case HPKE_KEM_MLKEM:
        if (k->pq.vt->self_type == TYPE_MLKEM_DECAPSULATION_KEY768)
            out = mlkem_decapsulation_key768_bytes(k->pq.data, a);
        else if (k->pq.vt->self_type == TYPE_MLKEM_DECAPSULATION_KEY1024)
            out = mlkem_decapsulation_key1024_bytes(k->pq.data, a);
        else
            hpke_set_error(&e, "private key seed not available");
        break;
    case HPKE_KEM_HYBRID:
        if (!k->has_seed) {
            hpke_set_error(&e, "private key seed not available");
            break;
        }
        out = slice_make(a, TYPE_BYTE, 32, 32);
        if (out.p == NULL)
            e = burrow_err_out_of_memory;
        else
            memcpy(out.p, k->seed, 32);
        break;
    case HPKE_KEM_UNSUPPORTED:
    default:
        break;
    }
    BURROW_OUT(err, e);
    return out;
}

HpkePrivateKey *hpke_new_dhkem_private_key(Alloc *a, EcdhKeyExchanger priv,
                                           Error *err) {
    const HpkeKEM *kem = hpke_dhkem(ecdh_key_exchanger_curve(priv));
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = NULL;
    CryptoDecapsulator none = {NULL, NULL};
    if (kem->kind != HPKE_KEM_DH)
        hpke_set_error(&e, "unsupported curve");
    else
        k = hpke_private_key_alloc(a, kem, priv, none, &e);
    BURROW_OUT(err, e);
    return k;
}

static HpkePrivateKey *hpke_hybrid_private_key(Alloc *a, CryptoDecapsulator pq,
                                               EcdhKeyExchanger t, Error *err) {
    const HpkeKEM *kem = hpke_kem_for_curve_hybrid(ecdh_key_exchanger_curve(t));
    if (kem == NULL) {
        hpke_set_error(err, "unsupported curve");
        return NULL;
    }
    if (hpke_encapsulator_pq(crypto_decapsulator_encapsulator(pq)) != kem->pq) {
        hpke_set_error(err, kem == &hpke_kem_xwing ? "invalid PQ KEM for X25519 hybrid"
                            : kem == &hpke_kem_mlkem768_p256
                                ? "invalid PQ KEM for P-256 hybrid"
                                : "invalid PQ KEM for P-384 hybrid");
        return NULL;
    }
    return hpke_private_key_alloc(a, kem, t, pq, err);
}

HpkePrivateKey *hpke_new_hybrid_private_key(Alloc *a, CryptoDecapsulator pq,
                                            EcdhKeyExchanger t, Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_hybrid_private_key(a, pq, t, &e);
    BURROW_OUT(err, e);
    return k;
}

static HpkePrivateKey *hpke_mlkem_private_key(Alloc *a, CryptoDecapsulator priv,
                                              Error *err) {
    int pq = hpke_encapsulator_pq(crypto_decapsulator_encapsulator(priv));
    if (pq == 0) {
        hpke_set_error(err, "unsupported public key type");
        return NULL;
    }
    EcdhKeyExchanger none = {NULL, NULL};
    return hpke_private_key_alloc(
        a, pq == 768 ? &hpke_kem_mlkem_768 : &hpke_kem_mlkem_1024, none, priv, err);
}

HpkePrivateKey *hpke_new_mlkem_private_key(Alloc *a, CryptoDecapsulator priv,
                                           Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_mlkem_private_key(a, priv, &e);
    BURROW_OUT(err, e);
    return k;
}

/* The DHKEM private key of priv, which it takes over. */
static HpkePrivateKey *hpke_dh_private_key_owning(Alloc *a, EcdhPrivateKey *priv,
                                                  Error *err) {
    if (priv == NULL)
        return NULL;
    CryptoDecapsulator none = {NULL, NULL};
    HpkePrivateKey *k =
        hpke_private_key_alloc(a, hpke_dhkem(ecdh_private_key_curve(priv)),
                               ecdh_private_key_key_exchanger(priv), none, err);
    if (k == NULL) {
        ecdh_private_key_free(priv);
        return NULL;
    }
    k->own_t = priv;
    return k;
}

/* The ML-KEM private key of the seed. */
static HpkePrivateKey *hpke_mlkem_private_key_from_seed(Alloc *a, const HpkeKEM *kem,
                                                        Slice seed, Error *err) {
    MlkemDecapsulationKey768 *dk768 = NULL;
    MlkemDecapsulationKey1024 *dk1024 = NULL;
    CryptoDecapsulator pq;
    if (kem->pq == 768) {
        dk768 = mlkem_new_decapsulation_key768(a, seed, err);
        if (dk768 == NULL)
            return NULL;
        pq = mlkem_decapsulation_key768_as_decapsulator(dk768);
    } else {
        dk1024 = mlkem_new_decapsulation_key1024(a, seed, err);
        if (dk1024 == NULL)
            return NULL;
        pq = mlkem_decapsulation_key1024_as_decapsulator(dk1024);
    }
    EcdhKeyExchanger none = {NULL, NULL};
    HpkePrivateKey *k = hpke_private_key_alloc(a, kem, none, pq, err);
    if (k == NULL) {
        mlkem_decapsulation_key768_free(dk768);
        mlkem_decapsulation_key1024_free(dk1024);
        return NULL;
    }
    k->own_dk768 = dk768;
    k->own_dk1024 = dk1024;
    return k;
}

/* hybridKEM.NewPrivateKey: the ML-KEM seed is the first 64 bytes of SHAKE256
 * of the secret, and the curve key the first of the next blocks of
 * curve_seed_size bytes that is a valid scalar. */
static HpkePrivateKey *hpke_hybrid_private_key_from_seed(Alloc *a, const HpkeKEM *kem,
                                                         Slice priv, Error *err) {
    if (priv.len != 32) {
        hpke_set_error(err, "hpke: invalid hybrid KEM secret length");
        return NULL;
    }
    Sha3 s;
    burrow__shake256_init(&s);
    burrow__sha3_write(&s, priv.p, priv.len);
    Byte seed_pq[MLKEM_SEED_SIZE];
    burrow__sha3_read(&s, seed_pq, sizeof seed_pq);
    HpkePrivateKey *pqk = hpke_mlkem_private_key_from_seed(
        a, kem, hpke_bytes(seed_pq, sizeof seed_pq), err);
    memset(seed_pq, 0, sizeof seed_pq);
    if (pqk == NULL) {
        memset(&s, 0, sizeof s);
        return NULL;
    }

    Byte seed_t[48];
    EcdhPrivateKey *t = NULL;
    for (;;) {
        burrow__sha3_read(&s, seed_t, kem->curve_seed_size);
        Error e = BURROW_NO_ERROR;
        t = ecdh_curve_new_private_key(kem->curve(), a,
                                       hpke_bytes(seed_t, kem->curve_seed_size), &e);
        if (BURROW_OK(e))
            break;
        if (errors_is(e, burrow_err_out_of_memory)) {
            *err = e;
            break;
        }
    }
    memset(seed_t, 0, sizeof seed_t);
    memset(&s, 0, sizeof s);
    if (t == NULL) {
        hpke_private_key_free(pqk);
        return NULL;
    }

    HpkePrivateKey *k =
        hpke_hybrid_private_key(a, pqk->pq, ecdh_private_key_key_exchanger(t), err);
    if (k == NULL) {
        ecdh_private_key_free(t);
        hpke_private_key_free(pqk);
        return NULL;
    }
    /* The ML-KEM key moves over from the key that held it. */
    k->own_t = t;
    k->own_dk768 = pqk->own_dk768;
    k->own_dk1024 = pqk->own_dk1024;
    pqk->own_dk768 = NULL;
    pqk->own_dk1024 = NULL;
    hpke_private_key_free(pqk);
    k->has_seed = true;
    memcpy(k->seed, priv.p, 32);
    return k;
}

static HpkePrivateKey *hpke_kem_private_key(const HpkeKEM *kem, Alloc *a, Slice data,
                                            Error *err) {
    switch (kem->kind) {
    case HPKE_KEM_DH:
        return hpke_dh_private_key_owning(
            a, ecdh_curve_new_private_key(kem->curve(), a, data, err), err);
    case HPKE_KEM_MLKEM:
        return hpke_mlkem_private_key_from_seed(a, kem, data, err);
    case HPKE_KEM_HYBRID:
        return hpke_hybrid_private_key_from_seed(a, kem, data, err);
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(err, "unsupported curve");
        return NULL;
    }
}

HpkePrivateKey *hpke_kem_new_private_key(const HpkeKEM *kem, Alloc *a, Slice data,
                                         Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_private_key(kem, a, data, &e);
    BURROW_OUT(err, e);
    return k;
}

HpkePrivateKey *hpke_kem_generate_key(const HpkeKEM *kem, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = NULL;
    switch (kem->kind) {
    case HPKE_KEM_DH: {
        IoReader rand = {0};
        k = hpke_dh_private_key_owning(
            a, ecdh_curve_generate_key(kem->curve(), a, rand, &e), &e);
        break;
    }
    case HPKE_KEM_MLKEM: {
        Byte seed[MLKEM_SEED_SIZE];
        burrow__crypto_rand_system(hpke_bytes(seed, sizeof seed));
        k = hpke_mlkem_private_key_from_seed(a, kem, hpke_bytes(seed, sizeof seed), &e);
        memset(seed, 0, sizeof seed);
        break;
    }
    case HPKE_KEM_HYBRID: {
        Byte seed[32];
        burrow__crypto_rand_system(hpke_bytes(seed, sizeof seed));
        k = hpke_hybrid_private_key_from_seed(a, kem, hpke_bytes(seed, sizeof seed),
                                              &e);
        memset(seed, 0, sizeof seed);
        break;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(&e, "unsupported curve");
        break;
    }
    BURROW_OUT(err, e);
    return k;
}

/* DeriveKeyPair of RFC 9180, section 7.1.3, for the DHKEMs, with the key from a
 * and the rest from the scratch allocator s. */
static HpkePrivateKey *hpke_dh_derive_key_pair(const HpkeKEM *kem, Alloc *a, Alloc *s,
                                               Slice ikm, Error *err) {
    Byte sid_buf[5];
    Slice sid = hpke_kem_suite_id(sid_buf, kem);
    Slice prk = hpke_labeled_extract(s, kem->kdf, sid, slice_nil(TYPE_BYTE), "dkp_prk",
                                     ikm, err);
    if (BURROW_FAILED(*err))
        return NULL;
    if (kem == &hpke_kem_dh_x25519) {
        Slice sk = hpke_labeled_expand(s, kem->kdf, sid, prk, "sk",
                                       slice_nil(TYPE_BYTE), kem->n_sk, err);
        hpke_wipe(prk);
        if (BURROW_FAILED(*err))
            return NULL;
        HpkePrivateKey *k = hpke_kem_private_key(kem, a, sk, err);
        hpke_wipe(sk);
        return k;
    }
    for (Byte counter = 0; counter < 4; counter++) {
        Slice sk = hpke_labeled_expand(s, kem->kdf, sid, prk, "candidate",
                                       hpke_bytes(&counter, 1), kem->n_sk, err);
        if (BURROW_FAILED(*err)) {
            hpke_wipe(prk);
            return NULL;
        }
        if (kem == &hpke_kem_dh_p521)
            ((Byte *)sk.p)[0] &= 0x01;
        Error e = BURROW_NO_ERROR;
        HpkePrivateKey *k = hpke_kem_private_key(kem, a, sk, &e);
        hpke_wipe(sk);
        if (k != NULL) {
            hpke_wipe(prk);
            return k;
        }
        if (errors_is(e, burrow_err_out_of_memory)) {
            hpke_wipe(prk);
            *err = e;
            return NULL;
        }
    }
    panic_str(BURROW_S("chance of four rejections is < 2^-128"));
}

HpkePrivateKey *hpke_kem_derive_key_pair(const HpkeKEM *kem, Alloc *a, Slice ikm,
                                         Error *err) {
    Error e = BURROW_NO_ERROR;
    HpkePrivateKey *k = NULL;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    switch (kem->kind) {
    case HPKE_KEM_DH:
        k = hpke_dh_derive_key_pair(kem, a, s, ikm, &e);
        break;
    case HPKE_KEM_MLKEM:
    case HPKE_KEM_HYBRID: {
        Byte sid_buf[5];
        Slice sid = hpke_kem_suite_id(sid_buf, kem);
        uint16_t n = kem->kind == HPKE_KEM_MLKEM ? 64 : 32;
        Slice dk = hpke_labeled_derive(s, &hpke_kdf_shake256, sid, ikm, "DeriveKeyPair",
                                       slice_nil(TYPE_BYTE), n, &e);
        if (BURROW_FAILED(e))
            break;
        k = hpke_kem_private_key(kem, a, dk, &e);
        hpke_wipe(dk);
        break;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(&e, "unsupported curve");
        break;
    }
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return k;
}

/* ---------------------------------------------------------- encap and decap */

/* Everything from here to the contexts takes a scratch allocator, and what it
 * returns lives there too. */

/* ExtractAndExpand of the DHKEMs. */
static Slice hpke_dh_extract_and_expand(Alloc *s, const HpkeKEM *kem, Slice dh,
                                        Slice kem_context, Error *err) {
    Byte sid_buf[5];
    Slice sid = hpke_kem_suite_id(sid_buf, kem);
    Slice prk = hpke_labeled_extract(s, kem->kdf, sid, slice_nil(TYPE_BYTE), "eae_prk",
                                     dh, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    Slice ss = hpke_labeled_expand(s, kem->kdf, sid, prk, "shared_secret", kem_context,
                                   kem->n_secret, err);
    hpke_wipe(prk);
    return ss;
}

/* The shared secret of a hybrid: SHA3-256 of ss_pq || ss_t || ct_t || ek_t ||
 * label. */
static Slice hpke_hybrid_shared_secret(Alloc *s, const HpkeKEM *kem, Slice ss_pq,
                                       Slice ss_t, Slice ct_t, Slice ek_t, Error *err) {
    Sha3 h;
    burrow__sha3_init256(&h);
    burrow__sha3_write(&h, ss_pq.p, ss_pq.len);
    burrow__sha3_write(&h, ss_t.p, ss_t.len);
    burrow__sha3_write(&h, ct_t.p, ct_t.len);
    burrow__sha3_write(&h, ek_t.p, ek_t.len);
    burrow__sha3_write(&h, kem->label, (Int)strlen(kem->label));
    Slice out = slice_make(s, TYPE_BYTE, 32, 32);
    if (out.p == NULL) {
        memset(&h, 0, sizeof h);
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_BYTE);
    }
    burrow__sha3_read(&h, out.p, 32);
    memset(&h, 0, sizeof h);
    return out;
}

/* The ephemeral curve key of an encapsulation, r's when it has one. *made is
 * the key when it was made here, for the caller to free. */
static const EcdhPrivateKey *hpke_ephemeral(Alloc *s, const EcdhPublicKey *pub,
                                            const HpkeEncapRandomness *r,
                                            EcdhPrivateKey **made, Error *err) {
    *made = NULL;
    if (r != NULL && r->ephemeral != NULL)
        return r->ephemeral;
    IoReader rand = {0};
    *made = ecdh_curve_generate_key(ecdh_public_key_curve(pub), s, rand, err);
    return *made;
}

/* The ML-KEM half of an encapsulation, r's when it has one. */
static Slice hpke_pq_encapsulate(Alloc *s, CryptoEncapsulator pq,
                                 const HpkeEncapRandomness *r, Slice *ct) {
    if (r != NULL && r->fixed_pq) {
        *ct = r->pq_ciphertext;
        return r->pq_shared_key;
    }
    CryptoEncapsulateResult res = crypto_encapsulator_encapsulate(pq, s);
    *ct = res.ciphertext;
    return res.shared_key;
}

/* PublicKey.encap: the shared secret, and the encapsulated key into *enc. */
static Slice hpke_encap(Alloc *s, const HpkePublicKey *pk, const HpkeEncapRandomness *r,
                        Slice *enc, Error *err) {
    const HpkeKEM *kem = pk->kem;
    *enc = slice_nil(TYPE_BYTE);
    switch (kem->kind) {
    case HPKE_KEM_DH: {
        EcdhPrivateKey *made;
        const EcdhPrivateKey *eph = hpke_ephemeral(s, pk->t, r, &made, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        Slice dh = ecdh_private_key_ecdh(eph, s, pk->t, err);
        Slice ss = slice_nil(TYPE_BYTE);
        if (BURROW_OK(*err)) {
            Slice enc_eph = ecdh_public_key_bytes(ecdh_private_key_public_key(eph), s);
            HpkePart parts[] = {hpke_part(enc_eph),
                                hpke_part(ecdh_public_key_bytes(pk->t, s))};
            Slice kem_context = hpke_concat(s, parts, 2, err);
            if (BURROW_OK(*err))
                ss = hpke_dh_extract_and_expand(s, kem, dh, kem_context, err);
            if (BURROW_OK(*err))
                *enc = enc_eph;
            hpke_wipe(dh);
        }
        ecdh_private_key_free(made);
        return ss;
    }
    case HPKE_KEM_MLKEM:
        return hpke_pq_encapsulate(s, pk->pq, r, enc);
    case HPKE_KEM_HYBRID: {
        EcdhPrivateKey *made;
        const EcdhPrivateKey *eph = hpke_ephemeral(s, pk->t, r, &made, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        Slice ss_t = ecdh_private_key_ecdh(eph, s, pk->t, err);
        Slice ss = slice_nil(TYPE_BYTE);
        if (BURROW_OK(*err)) {
            Slice ct_t = ecdh_public_key_bytes(ecdh_private_key_public_key(eph), s);
            Slice ct_pq;
            Slice ss_pq = hpke_pq_encapsulate(s, pk->pq, r, &ct_pq);
            ss = hpke_hybrid_shared_secret(s, kem, ss_pq, ss_t, ct_t,
                                           ecdh_public_key_bytes(pk->t, s), err);
            HpkePart parts[] = {hpke_part(ct_pq), hpke_part(ct_t)};
            if (BURROW_OK(*err))
                *enc = hpke_concat(s, parts, 2, err);
            hpke_wipe(ss_t);
        }
        ecdh_private_key_free(made);
        return ss;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(err, "unsupported curve");
        return slice_nil(TYPE_BYTE);
    }
}

/* PrivateKey.decap: the shared secret that enc carries. */
static Slice hpke_decap(Alloc *s, const HpkePrivateKey *k, Slice enc, Error *err) {
    const HpkeKEM *kem = k->kem;
    switch (kem->kind) {
    case HPKE_KEM_DH: {
        EcdhPublicKey *pub =
            ecdh_curve_new_public_key(ecdh_key_exchanger_curve(k->t), s, enc, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        Slice dh = ecdh_key_exchanger_ecdh(k->t, s, pub, err);
        ecdh_public_key_free(pub);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        HpkePart parts[] = {
            hpke_part(enc),
            hpke_part(ecdh_public_key_bytes(ecdh_key_exchanger_public_key(k->t), s))};
        Slice kem_context = hpke_concat(s, parts, 2, err);
        Slice ss = slice_nil(TYPE_BYTE);
        if (BURROW_OK(*err))
            ss = hpke_dh_extract_and_expand(s, kem, dh, kem_context, err);
        hpke_wipe(dh);
        return ss;
    }
    case HPKE_KEM_MLKEM:
        return crypto_decapsulator_decapsulate(k->pq, s, enc, err);
    case HPKE_KEM_HYBRID: {
        Int ct_size = hpke_pq_ct_size(kem);
        if (enc.len != ct_size + kem->curve_point_size) {
            hpke_set_error(err, "invalid encapsulated key size");
            return slice_nil(TYPE_BYTE);
        }
        Slice ct_pq = slice_sub(enc, 0, ct_size);
        Slice ct_t = slice_sub(enc, ct_size, enc.len);
        Slice ss_pq = crypto_decapsulator_decapsulate(k->pq, s, ct_pq, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        EcdhPublicKey *pub =
            ecdh_curve_new_public_key(ecdh_key_exchanger_curve(k->t), s, ct_t, err);
        if (BURROW_FAILED(*err)) {
            hpke_wipe(ss_pq);
            return slice_nil(TYPE_BYTE);
        }
        Slice ss_t = ecdh_key_exchanger_ecdh(k->t, s, pub, err);
        ecdh_public_key_free(pub);
        Slice ss = slice_nil(TYPE_BYTE);
        if (BURROW_OK(*err))
            ss = hpke_hybrid_shared_secret(
                s, kem, ss_pq, ss_t, ct_t,
                ecdh_public_key_bytes(ecdh_key_exchanger_public_key(k->t), s), err);
        hpke_wipe(ss_pq);
        hpke_wipe(ss_t);
        return ss;
    }
    case HPKE_KEM_UNSUPPORTED:
    default:
        hpke_set_error(err, "unsupported curve");
        return slice_nil(TYPE_BYTE);
    }
}

/* ----------------------------------------------------------------- contexts */

/* hpke.go's context, which Sender and Recipient both are. */
typedef struct HpkeContext {
    Byte suite_id[10];
    const HpkeKDF *kdf;
    CipherAEAD aead;
    Int nonce_size;
    Byte base_nonce[12];
    Byte exp_secret[64];
    uint64_t seq_num;
} HpkeContext;

struct HpkeSender {
    HpkeContext c;
};

struct HpkeRecipient {
    HpkeContext c;
};

static Slice hpke_context_suite_id(const HpkeContext *c) {
    return hpke_bytes((void *)(uintptr_t)c->suite_id, sizeof c->suite_id);
}

/* KeySchedule of RFC 9180 in mode_base, into c. The cipher comes from a and
 * everything else from the scratch allocator s. */
static void hpke_key_schedule(Alloc *a, Alloc *s, HpkeContext *c, Slice shared_secret,
                              uint16_t kem_id, const HpkeKDF *kdf, const HpkeAEAD *aead,
                              Slice info, Error *err) {
    memcpy(c->suite_id, "HPKE", 4);
    hpke_put16(c->suite_id + 4, kem_id);
    hpke_put16(c->suite_id + 6, kdf->id);
    hpke_put16(c->suite_id + 8, aead->id);
    c->kdf = kdf;
    c->nonce_size = aead->nn;
    Slice sid = hpke_context_suite_id(c);
    Slice none = slice_nil(TYPE_BYTE);
    Byte mode = 0;

    if (hpke_kdf_one_stage(kdf)) {
        Byte zero[2] = {0, 0}, ss_len[2], info_len[2];
        hpke_put16(ss_len, (uint16_t)shared_secret.len);
        hpke_put16(info_len, (uint16_t)info.len);
        HpkePart secrets_parts[] = {{zero, 2}, {ss_len, 2}, hpke_part(shared_secret)};
        HpkePart ks_parts[] = {{&mode, 1}, {zero, 2}, {info_len, 2}, hpke_part(info)};
        Slice secrets = hpke_concat(s, secrets_parts, 3, err);
        if (BURROW_FAILED(*err))
            return;
        Slice ks_context = hpke_concat(s, ks_parts, 4, err);
        if (BURROW_FAILED(*err)) {
            hpke_wipe(secrets);
            return;
        }
        Slice secret =
            hpke_labeled_derive(s, kdf, sid, secrets, "secret", ks_context,
                                (uint16_t)(aead->nk + aead->nn + kdf->nh), err);
        hpke_wipe(secrets);
        if (BURROW_FAILED(*err))
            return;
        Byte *p = secret.p;
        c->aead = hpke_aead_cipher(a, aead, slice_sub(secret, 0, aead->nk), err);
        if (aead->nn > 0)
            memcpy(c->base_nonce, p + aead->nk, (size_t)aead->nn);
        memcpy(c->exp_secret, p + aead->nk + aead->nn, (size_t)kdf->nh);
        hpke_wipe(secret);
        return;
    }

    Slice psk_id_hash =
        hpke_labeled_extract(s, kdf, sid, none, "psk_id_hash", none, err);
    if (BURROW_FAILED(*err))
        return;
    Slice info_hash = hpke_labeled_extract(s, kdf, sid, none, "info_hash", info, err);
    if (BURROW_FAILED(*err))
        return;
    HpkePart ks_parts[] = {{&mode, 1}, hpke_part(psk_id_hash), hpke_part(info_hash)};
    Slice ks_context = hpke_concat(s, ks_parts, 3, err);
    if (BURROW_FAILED(*err))
        return;
    Slice secret =
        hpke_labeled_extract(s, kdf, sid, shared_secret, "secret", none, err);
    if (BURROW_FAILED(*err))
        return;
    Slice key = hpke_labeled_expand(s, kdf, sid, secret, "key", ks_context,
                                    (uint16_t)aead->nk, err);
    if (BURROW_OK(*err)) {
        c->aead = hpke_aead_cipher(a, aead, key, err);
        hpke_wipe(key);
    }
    Slice base_nonce = none, exp = none;
    if (BURROW_OK(*err))
        base_nonce = hpke_labeled_expand(s, kdf, sid, secret, "base_nonce", ks_context,
                                         (uint16_t)aead->nn, err);
    if (BURROW_OK(*err))
        exp = hpke_labeled_expand(s, kdf, sid, secret, "exp", ks_context,
                                  (uint16_t)kdf->nh, err);
    if (BURROW_OK(*err)) {
        if (aead->nn > 0)
            memcpy(c->base_nonce, base_nonce.p, (size_t)aead->nn);
        memcpy(c->exp_secret, exp.p, (size_t)kdf->nh);
    }
    hpke_wipe(secret);
    hpke_wipe(exp);
}

static void hpke_next_nonce(const HpkeContext *c, Byte nonce[12]) {
    memset(nonce, 0, 12);
    Int n = c->nonce_size;
    for (int i = 0; i < 8; i++)
        nonce[n - 1 - i] = (Byte)(c->seq_num >> (8 * i));
    for (Int i = 0; i < n; i++)
        nonce[i] ^= c->base_nonce[i];
}

static Slice hpke_context_export(Alloc *a, const HpkeContext *c, Str exporter_context,
                                 Int length, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice out = slice_nil(TYPE_BYTE);
    if (length < 0 || length > 0xffff) {
        hpke_set_error(&e, "invalid length");
        BURROW_OUT(err, e);
        return out;
    }
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Slice sid = hpke_context_suite_id(c);
    Slice exp = hpke_bytes((void *)(uintptr_t)c->exp_secret, c->kdf->nh);
    Slice ctx = hpke_bytes((void *)(uintptr_t)exporter_context.p, exporter_context.len);
    Slice v;
    if (hpke_kdf_one_stage(c->kdf))
        v = hpke_labeled_derive(s, c->kdf, sid, exp, "sec", ctx, (uint16_t)length, &e);
    else
        v = hpke_labeled_expand(s, c->kdf, sid, exp, "sec", ctx, (uint16_t)length, &e);
    out = hpke_clone(a, v, &e);
    hpke_wipe(v);
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return out;
}

Slice burrow__hpke_new_sender_with(Alloc *a, const HpkePublicKey *pk,
                                   const HpkeKDF *kdf, const HpkeAEAD *aead, Slice info,
                                   const HpkeEncapRandomness *r, HpkeSender **sender,
                                   Error *err) {
    Error e = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Slice enc;
    Slice out = slice_nil(TYPE_BYTE);
    *sender = NULL;
    Slice ss = hpke_encap(s, pk, r, &enc, &e);
    HpkeSender *x = NULL;
    if (BURROW_OK(e)) {
        x = BURROW_NEW(a, HpkeSender);
        if (x == NULL)
            e = burrow_err_out_of_memory;
        else
            hpke_key_schedule(a, s, &x->c, ss, pk->kem->id, kdf, aead, info, &e);
        if (r == NULL || !r->fixed_pq)
            hpke_wipe(ss);
        out = hpke_clone(a, enc, &e);
    }
    arena_free(&scratch);
    if (BURROW_OK(e))
        *sender = x;
    else
        out = slice_nil(TYPE_BYTE);
    BURROW_OUT(err, e);
    return out;
}

Slice hpke_new_sender(Alloc *a, const HpkePublicKey *pk, const HpkeKDF *kdf,
                      const HpkeAEAD *aead, Slice info, HpkeSender **s, Error *err) {
    return burrow__hpke_new_sender_with(a, pk, kdf, aead, info, NULL, s, err);
}

HpkeRecipient *hpke_new_recipient(Alloc *a, Slice enc, const HpkePrivateKey *k,
                                  const HpkeKDF *kdf, const HpkeAEAD *aead, Slice info,
                                  Error *err) {
    Error e = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    HpkeRecipient *r = NULL;
    Slice ss = hpke_decap(s, k, enc, &e);
    if (BURROW_OK(e)) {
        r = BURROW_NEW(a, HpkeRecipient);
        if (r == NULL)
            e = burrow_err_out_of_memory;
        else
            hpke_key_schedule(a, s, &r->c, ss, k->kem->id, kdf, aead, info, &e);
        hpke_wipe(ss);
    }
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return BURROW_OK(e) ? r : NULL;
}

Slice hpke_sender_seal(HpkeSender *s, Alloc *a, Slice aad, Slice plaintext,
                       Error *err) {
    if (s->c.aead.vt == NULL) {
        BURROW_OUT(
            err, errors_new(error_allocator(), BURROW_S("export-only instantiation")));
        return slice_nil(TYPE_BYTE);
    }
    Byte nonce[12];
    hpke_next_nonce(&s->c, nonce);
    Slice ct = cipher_aead_seal(s->c.aead, a, slice_nil(TYPE_BYTE),
                                hpke_bytes(nonce, s->c.nonce_size), plaintext, aad);
    if (ct.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    s->c.seq_num++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ct;
}

Slice hpke_sender_export(const HpkeSender *s, Alloc *a, Str exporter_context,
                         Int length, Error *err) {
    return hpke_context_export(a, &s->c, exporter_context, length, err);
}

Slice hpke_recipient_open(HpkeRecipient *r, Alloc *a, Slice aad, Slice ciphertext,
                          Error *err) {
    if (r->c.aead.vt == NULL) {
        BURROW_OUT(
            err, errors_new(error_allocator(), BURROW_S("export-only instantiation")));
        return slice_nil(TYPE_BYTE);
    }
    Byte nonce[12];
    hpke_next_nonce(&r->c, nonce);
    Error e = BURROW_NO_ERROR;
    Slice pt =
        cipher_aead_open(r->c.aead, a, slice_nil(TYPE_BYTE),
                         hpke_bytes(nonce, r->c.nonce_size), ciphertext, aad, &e);
    if (BURROW_OK(e))
        r->c.seq_num++;
    BURROW_OUT(err, e);
    return pt;
}

Slice hpke_recipient_export(const HpkeRecipient *r, Alloc *a, Str exporter_context,
                            Int length, Error *err) {
    return hpke_context_export(a, &r->c, exporter_context, length, err);
}

Slice hpke_seal(Alloc *a, const HpkePublicKey *pk, const HpkeKDF *kdf,
                const HpkeAEAD *aead, Slice info, Slice plaintext, Error *err) {
    Error e = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    HpkeSender *sender;
    Slice enc = hpke_new_sender(s, pk, kdf, aead, info, &sender, &e);
    Slice out = slice_nil(TYPE_BYTE);
    if (BURROW_OK(e)) {
        Slice ct = hpke_sender_seal(sender, s, slice_nil(TYPE_BYTE), plaintext, &e);
        if (BURROW_OK(e)) {
            HpkePart parts[] = {hpke_part(enc), hpke_part(ct)};
            out = hpke_concat(a, parts, 2, &e);
        }
    }
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return out;
}

Slice hpke_open(Alloc *a, const HpkePrivateKey *k, const HpkeKDF *kdf,
                const HpkeAEAD *aead, Slice info, Slice ciphertext, Error *err) {
    Int enc_size = k->kem->enc_size;
    if (ciphertext.len < enc_size) {
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("ciphertext too short")));
        return slice_nil(TYPE_BYTE);
    }
    Error e = BURROW_NO_ERROR;
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Slice out = slice_nil(TYPE_BYTE);
    HpkeRecipient *r = hpke_new_recipient(s, slice_sub(ciphertext, 0, enc_size), k, kdf,
                                          aead, info, &e);
    if (BURROW_OK(e))
        out = hpke_recipient_open(r, a, slice_nil(TYPE_BYTE),
                                  slice_sub(ciphertext, enc_size, ciphertext.len), &e);
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return out;
}
