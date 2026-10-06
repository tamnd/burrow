/* Derived from Go's src/crypto/ed25519/ed25519.go and
 * src/crypto/internal/fips140/ed25519/ed25519.go. Go source: go1.27.1.
 *
 * Go splits the package in two, the FIPS module's key types and the public
 * slices of bytes over them. Here the module's PrivateKey is EdPrivate, which
 * only lives on the stack for one call, and its PublicKey is a point and the
 * bytes it came from, which ed_verify_with_dom takes as they are.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/ed25519.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "edwards25519.h"
#include "rand_internal.h"
#include "sha512_internal.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ types */

const Type burrow_type_Ed25519PublicKey = {
    BURROW_S_INIT("PublicKey"),
    BURROW_S_INIT("crypto/ed25519"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_Ed25519PrivateKey = {
    BURROW_S_INIT("PrivateKey"),
    BURROW_S_INIT("crypto/ed25519"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTE,
    NULL,
    0,
    0,
    NULL,
};

static const Field ed_options_fields[] = {
    {BURROW_S_INIT("Hash"),
     {NULL, 0},
     TYPE_CRYPTO_HASH,
     (uint32_t)offsetof(Ed25519Options, hash)},
    {BURROW_S_INIT("Context"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(Ed25519Options, context)},
};

const Type burrow_type_Ed25519Options = {
    BURROW_S_INIT("Options"),
    BURROW_S_INIT("crypto/ed25519"),
    KIND_STRUCT,
    (uint32_t)sizeof(Ed25519Options),
    (uint16_t)_Alignof(Ed25519Options),
    2,
    0,
    ed_options_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* --------------------------------------------------------------- messages */

/* prefix followed by n in decimal, in buf, which has room for any prefix here
 * and any Int. */
static Str ed_text_n(Byte buf[96], const char *prefix, Int n) {
    Int plen = (Int)strlen(prefix);
    memcpy(buf, prefix, (size_t)plen);
    Byte digits[20];
    Int nd = 0;
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    do {
        digits[nd++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = plen;
    if (n < 0)
        buf[len++] = '-';
    while (nd > 0)
        buf[len++] = digits[--nd];
    return str_from_bytes(buf, len);
}

static Error ed_error_n(const char *prefix, Int n) {
    Byte buf[96];
    return errors_new(error_allocator(), ed_text_n(buf, prefix, n));
}

static Error ed_error(const char *text) {
    return errors_new(error_allocator(), str_from_cstr(text));
}

/* A panic whose text has a number in it, copied somewhere that outlives the
 * jump. With no memory for the copy the text goes without the number. */
BURROW_NORETURN static void ed_panic_n(const char *prefix, Int n) {
    Byte buf[96];
    Str s = ed_text_n(buf, prefix, n);
    Byte *p = (Byte *)mem_alloc_nozero(error_allocator(), (size_t)s.len, 1);
    if (p == NULL)
        panic_str(str_from_cstr(prefix));
    memcpy(p, s.p, (size_t)s.len);
    panic_str(str_from_bytes(p, s.len));
}

/* ---------------------------------------------------------- the FIPS module */

/* The module's PrivateKey: the seed, the public key, the secret scalar, and the
 * second half of the seed's hash, which goes into every nonce. */
typedef struct EdPrivate {
    uint8_t seed[ED25519_SEED_SIZE];
    uint8_t pub[ED25519_PUBLIC_KEY_SIZE];
    Edwards25519Scalar s;
    uint8_t prefix[32];
} EdPrivate;

/* The SHA-512 of the seed, the scalar from its first half and the prefix from
 * its second. */
static void ed_expand_seed(EdPrivate *k) {
    Sha512Digest hs;
    uint8_t h[SHA512_SIZE];
    burrow__sha512_init(&hs);
    burrow__sha512_write(&hs, k->seed, ED25519_SEED_SIZE);
    burrow__sha512_sum(&hs, h);
    if (burrow__sc_set_bytes_with_clamping(&k->s, slice_from(h, 32, 32, TYPE_BYTE),
                                           NULL) == NULL)
        panic_str(BURROW_S("ed25519: internal error: setting scalar failed"));
    memcpy(k->prefix, h + 32, 32);
}

/* newPrivateKeyFromSeed, for a seed already known to be 32 bytes. */
static void ed_private_from_seed(EdPrivate *k, const uint8_t *seed) {
    memcpy(k->seed, seed, ED25519_SEED_SIZE);
    ed_expand_seed(k);
    Edwards25519Point A;
    burrow__ge_scalar_base_mult(&A, &k->s);
    burrow__ge_bytes(&A, k->pub);
}

/* newPrivateKey, for 64 bytes. The public key is taken as it is, and not
 * decoded into a point, which Go notes would add a fifth to the cost of a
 * signature for something signing does not use. */
static void ed_private_from_bytes(EdPrivate *k, const uint8_t *priv) {
    memcpy(k->seed, priv, ED25519_SEED_SIZE);
    ed_expand_seed(k);
    memcpy(k->pub, priv + 32, ED25519_PUBLIC_KEY_SIZE);
}

/* The domain separation in front of every hash of Ed25519ph and Ed25519ctx,
 * from section 2 and section 5.1 of RFC 8032. Plain Ed25519 has none. Both
 * strings are 32 bytes and a final byte saying which. */
typedef enum EdDom { ED_DOM_PURE, ED_DOM_PH, ED_DOM_CTX } EdDom;

static void ed_write_dom(Sha512Digest *h, EdDom dom, Str context) {
    static const char prefix[] = "SigEd25519 no Ed25519 collisions";
    if (dom == ED_DOM_PURE)
        return;
    uint8_t flag[2] = {dom == ED_DOM_PH ? 1 : 0, (uint8_t)context.len};
    burrow__sha512_write(h, prefix, (Int)sizeof(prefix) - 1);
    burrow__sha512_write(h, flag, 2);
    burrow__sha512_write(h, context.p, context.len);
}

static void ed_sign_with_dom(uint8_t signature[ED25519_SIGNATURE_SIZE],
                             const EdPrivate *k, Slice message, EdDom dom,
                             Str context) {
    Sha512Digest mh;
    uint8_t message_digest[SHA512_SIZE];
    burrow__sha512_init(&mh);
    ed_write_dom(&mh, dom, context);
    burrow__sha512_write(&mh, k->prefix, 32);
    burrow__sha512_write(&mh, message.p, message.len);
    burrow__sha512_sum(&mh, message_digest);
    Edwards25519Scalar r;
    if (burrow__sc_set_uniform_bytes(&r, slice_from(message_digest, 64, 64, TYPE_BYTE),
                                     NULL) == NULL)
        panic_str(BURROW_S("ed25519: internal error: setting scalar failed"));

    Edwards25519Point R;
    uint8_t r_bytes[32];
    burrow__ge_scalar_base_mult(&R, &r);
    burrow__ge_bytes(&R, r_bytes);

    Sha512Digest kh;
    uint8_t hram_digest[SHA512_SIZE];
    burrow__sha512_init(&kh);
    ed_write_dom(&kh, dom, context);
    burrow__sha512_write(&kh, r_bytes, 32);
    burrow__sha512_write(&kh, k->pub, 32);
    burrow__sha512_write(&kh, message.p, message.len);
    burrow__sha512_sum(&kh, hram_digest);
    Edwards25519Scalar kk;
    if (burrow__sc_set_uniform_bytes(&kk, slice_from(hram_digest, 64, 64, TYPE_BYTE),
                                     NULL) == NULL)
        panic_str(BURROW_S("ed25519: internal error: setting scalar failed"));

    Edwards25519Scalar S;
    burrow__sc_multiply_add(&S, &kk, &k->s, &r);

    memcpy(signature, r_bytes, 32);
    burrow__sc_bytes(&S, signature + 32);
}

/* signPH and signCtx, which check the lengths first, and sign. */
static Error ed_sign_variant(uint8_t signature[ED25519_SIGNATURE_SIZE],
                             const EdPrivate *k, Slice message, EdDom dom,
                             Str context) {
    if (dom == ED_DOM_PH) {
        if (message.len != SHA512_SIZE)
            return ed_error_n("ed25519: bad Ed25519ph message hash length: ",
                              message.len);
        if (context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ph context length: ", context.len);
    } else if (dom == ED_DOM_CTX) {
        /* Section 5.1 of RFC 8032 says the context should not be empty. */
        if (context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ctx context length: ", context.len);
    }
    ed_sign_with_dom(signature, k, message, dom, context);
    return BURROW_NO_ERROR;
}

static Error ed_verify_with_dom(const Edwards25519Point *A, const uint8_t a_bytes[32],
                                Slice message, Slice sig, EdDom dom, Str context) {
    if (sig.len != ED25519_SIGNATURE_SIZE)
        return ed_error_n("ed25519: bad signature length: ", sig.len);
    const uint8_t *s = sig.p;

    if ((s[63] & 224) != 0)
        return ed_error("ed25519: invalid signature");

    Sha512Digest kh;
    uint8_t hram_digest[SHA512_SIZE];
    burrow__sha512_init(&kh);
    ed_write_dom(&kh, dom, context);
    burrow__sha512_write(&kh, s, 32);
    burrow__sha512_write(&kh, a_bytes, 32);
    burrow__sha512_write(&kh, message.p, message.len);
    burrow__sha512_sum(&kh, hram_digest);
    Edwards25519Scalar k;
    if (burrow__sc_set_uniform_bytes(&k, slice_from(hram_digest, 64, 64, TYPE_BYTE),
                                     NULL) == NULL)
        panic_str(BURROW_S("ed25519: internal error: setting scalar failed"));

    Edwards25519Scalar S;
    if (burrow__sc_set_canonical_bytes(
            &S, slice_from((void *)(uintptr_t)(s + 32), 32, 32, TYPE_BYTE), NULL) ==
        NULL)
        return ed_error("ed25519: invalid signature");

    /* [S]B = R + [k]A, so [k](-A) + [S]B = R. */
    Edwards25519Point minus_A, R;
    uint8_t r_bytes[32];
    burrow__ge_negate(&minus_A, A);
    burrow__ge_var_time_double_scalar_base_mult(&R, &k, &minus_A, &S);
    burrow__ge_bytes(&R, r_bytes);
    if (memcmp(s, r_bytes, 32) != 0)
        return ed_error("ed25519: invalid signature");
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------- keys */

/* A copy of n bytes at p from a, or a nil Slice with no memory. */
static Slice ed_copy(Alloc *a, const void *p, Int n) {
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    if (out.p != NULL)
        memcpy(out.p, p, (size_t)n);
    return out;
}

bool ed25519_public_key_equal(Ed25519PublicKey pub, CryptoPublicKey x) {
    if (x.t != TYPE_ED25519_PUBLIC_KEY || x.data == NULL)
        return false;
    return subtle_constant_time_compare(pub, *(const Slice *)x.data) == 1;
}

Ed25519PublicKey ed25519_private_key_public(Ed25519PrivateKey priv, Alloc *a) {
    /* Go copies priv[32:] into 32 bytes, so a key that is too short panics
     * as slicing it does, and one that is short of 64 gives zeros at the end. */
    if (priv.len < 32)
        runtime_slice_bounds_out_of_range(32, priv.len, priv.cap);
    Slice out =
        slice_make(a, TYPE_BYTE, ED25519_PUBLIC_KEY_SIZE, ED25519_PUBLIC_KEY_SIZE);
    if (out.p == NULL)
        return out;
    Int n = priv.len - 32 < ED25519_PUBLIC_KEY_SIZE ? priv.len - 32
                                                    : ED25519_PUBLIC_KEY_SIZE;
    if (n > 0)
        memcpy(out.p, (const Byte *)priv.p + 32, (size_t)n);
    return out;
}

bool ed25519_private_key_equal(Ed25519PrivateKey priv, CryptoPrivateKey x) {
    if (x.t != TYPE_ED25519_PRIVATE_KEY || x.data == NULL)
        return false;
    return subtle_constant_time_compare(priv, *(const Slice *)x.data) == 1;
}

Slice ed25519_private_key_seed(Ed25519PrivateKey priv, Alloc *a) {
    if (priv.cap < ED25519_SEED_SIZE)
        runtime_slice_bounds_out_of_range(0, ED25519_SEED_SIZE, priv.cap);
    return ed_copy(a, priv.p, ED25519_SEED_SIZE);
}

/* -------------------------------------------------------------- options */

CryptoHash ed25519_options_hash_func(const Ed25519Options *o) {
    return o->hash;
}

static CryptoHash ed_options_hash_func(void *self) {
    return ed25519_options_hash_func(self);
}

static const CryptoSignerOptsVT ed_options_vt = {TYPE_ED25519_OPTIONS,
                                                 ed_options_hash_func};

CryptoSignerOpts ed25519_options_as_signer_opts(const Ed25519Options *o) {
    CryptoSignerOpts out = {&ed_options_vt, (void *)(uintptr_t)o};
    return out;
}

/* ---------------------------------------------------------------- signing */

/* The checks Go's privateKeyCache.Get makes on its way to the key: &priv[0]
 * for its key, and then the length. Returns the error for a bad length. */
static Error ed_unpack_private(EdPrivate *k, Ed25519PrivateKey priv) {
    if (priv.len == 0)
        runtime_index_out_of_range(0, 0);
    if (priv.len != ED25519_PRIVATE_KEY_SIZE)
        return ed_error_n("ed25519: bad private key length: ", priv.len);
    ed_private_from_bytes(k, priv.p);
    return BURROW_NO_ERROR;
}

Slice ed25519_private_key_sign(Ed25519PrivateKey priv, Alloc *a, IoReader rand,
                               Slice message, CryptoSignerOpts opts, Error *err) {
    (void)rand;
    EdPrivate k;
    Error e = ed_unpack_private(&k, priv);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }

    CryptoHash hash = opts.vt->hash_func(opts.data);
    Str context = BURROW_STR_EMPTY;
    if (opts.vt->self_type == TYPE_ED25519_OPTIONS)
        context = ((const Ed25519Options *)opts.data)->context;

    EdDom dom;
    if (hash == CRYPTO_SHA512)
        dom = ED_DOM_PH;
    else if (hash == 0 && context.len != 0)
        dom = ED_DOM_CTX;
    else if (hash == 0)
        dom = ED_DOM_PURE;
    else {
        BURROW_OUT(err, ed_error("ed25519: expected opts.HashFunc() zero (unhashed "
                                 "message, for standard Ed25519) or SHA-512 (for "
                                 "Ed25519ph)"));
        return slice_nil(TYPE_BYTE);
    }

    uint8_t signature[ED25519_SIGNATURE_SIZE];
    e = ed_sign_variant(signature, &k, message, dom, context);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Slice out = ed_copy(a, signature, ED25519_SIGNATURE_SIZE);
    BURROW_OUT(err, out.p == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR);
    return out;
}

static CryptoPublicKey ed_signer_public(void *self) {
    Ed25519Signer *s = self;
    return BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &s->pub);
}

static Slice ed_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                            CryptoSignerOpts opts, Error *err) {
    return ed25519_private_key_sign(((Ed25519Signer *)self)->priv, a, rand, digest,
                                    opts, err);
}

static const CryptoSignerVT ed_signer_vt = {TYPE_ED25519_PRIVATE_KEY, ed_signer_public,
                                            ed_signer_sign};

CryptoSigner ed25519_private_key_signer(Ed25519PrivateKey priv, Ed25519Signer *s) {
    s->priv = priv;
    /* priv[32:], which is what Public copies. */
    if (priv.len < 32)
        runtime_slice_bounds_out_of_range(32, priv.len, priv.cap);
    s->pub = slice_from((Byte *)priv.p + 32, priv.len - 32, priv.cap - 32, TYPE_BYTE);
    CryptoSigner out = {&ed_signer_vt, s};
    return out;
}

/* ------------------------------------------------------------ functions */

Ed25519PrivateKey ed25519_new_key_from_seed(Alloc *a, Slice seed) {
    if (seed.len != ED25519_SEED_SIZE)
        ed_panic_n("ed25519: bad seed length: ", seed.len);
    EdPrivate k;
    ed_private_from_seed(&k, seed.p);
    Slice out =
        slice_make(a, TYPE_BYTE, ED25519_PRIVATE_KEY_SIZE, ED25519_PRIVATE_KEY_SIZE);
    if (out.p != NULL) {
        memcpy(out.p, k.seed, ED25519_SEED_SIZE);
        memcpy((Byte *)out.p + ED25519_SEED_SIZE, k.pub, ED25519_PUBLIC_KEY_SIZE);
    }
    return out;
}

Ed25519PublicKey ed25519_generate_key(Alloc *a, IoReader random,
                                      Ed25519PrivateKey *priv, Error *err) {
    *priv = slice_nil(TYPE_BYTE);
    if (random.vt == NULL)
        random = burrow__crypto_rand_nil_reader();

    uint8_t seed[ED25519_SEED_SIZE];
    Slice seed_slice =
        slice_from(seed, ED25519_SEED_SIZE, ED25519_SEED_SIZE, TYPE_BYTE);
    if (burrow__crypto_rand_is_default_reader(random)) {
        burrow__crypto_rand_system(seed_slice);
    } else {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(random, seed_slice, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
    }

    Ed25519PrivateKey k = ed25519_new_key_from_seed(a, seed_slice);
    if (k.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Ed25519PublicKey pub = ed_copy(a, (const Byte *)k.p + 32, ED25519_PUBLIC_KEY_SIZE);
    if (pub.p == NULL) {
        mem_free(a, k.p, (size_t)k.cap, 1);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    *priv = k;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return pub;
}

Slice ed25519_sign(Alloc *a, Ed25519PrivateKey priv, Slice message) {
    EdPrivate k;
    Error e = ed_unpack_private(&k, priv);
    if (BURROW_FAILED(e))
        ed_panic_n("ed25519: bad private key: ed25519: bad private key length: ",
                   priv.len);
    uint8_t signature[ED25519_SIGNATURE_SIZE];
    ed_sign_with_dom(signature, &k, message, ED_DOM_PURE, BURROW_STR_EMPTY);
    return ed_copy(a, signature, ED25519_SIGNATURE_SIZE);
}

bool ed25519_verify(Ed25519PublicKey pub, Slice message, Slice sig) {
    Ed25519Options opts = {0, BURROW_STR_EMPTY};
    return BURROW_OK(ed25519_verify_with_options(pub, message, sig, &opts));
}

Error ed25519_verify_with_options(Ed25519PublicKey pub, Slice message, Slice sig,
                                  const Ed25519Options *opts) {
    if (pub.len != ED25519_PUBLIC_KEY_SIZE)
        ed_panic_n("ed25519: bad public key length: ", pub.len);

    /* NewPublicKey: set_bytes checks that the point is on the curve. */
    Edwards25519Point A;
    if (burrow__ge_set_bytes(&A, pub, NULL) == NULL)
        return ed_error("ed25519: bad public key");

    EdDom dom;
    if (opts->hash == CRYPTO_SHA512) {
        dom = ED_DOM_PH;
        if (message.len != SHA512_SIZE)
            return ed_error_n("ed25519: bad Ed25519ph message hash length: ",
                              message.len);
        if (opts->context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ph context length: ",
                              opts->context.len);
    } else if (opts->hash == 0 && opts->context.len != 0) {
        dom = ED_DOM_CTX;
        if (opts->context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ctx context length: ",
                              opts->context.len);
    } else if (opts->hash == 0) {
        dom = ED_DOM_PURE;
    } else {
        return ed_error(
            "ed25519: expected opts.Hash zero (unhashed message, for standard "
            "Ed25519) or SHA-512 (for Ed25519ph)");
    }
    return ed_verify_with_dom(&A, pub.p, message, sig, dom, opts->context);
}
