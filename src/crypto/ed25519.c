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
#include "burrow/sync.h"
#include "burrow/type.h"

#include "edwards25519.h"
#include "fips140_module.h"
#include "fips140only_internal.h"
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
CRYPTO_FIPS_TEXT static Str ed_text_n(Byte buf[96], const char *prefix, Int n) {
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

CRYPTO_FIPS_TEXT static Error ed_error_n(const char *prefix, Int n) {
    Byte buf[96];
    return errors_new(error_allocator(), ed_text_n(buf, prefix, n));
}

CRYPTO_FIPS_TEXT static Error ed_error(const char *text) {
    return errors_new(error_allocator(), str_from_cstr(text));
}

/* A panic whose text has a number in it, copied somewhere that outlives the
 * jump. With no memory for the copy the text goes without the number. */
CRYPTO_FIPS_TEXT BURROW_NORETURN static void ed_panic_n(const char *prefix, Int n) {
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
CRYPTO_FIPS_TEXT static void ed_expand_seed(EdPrivate *k) {
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
CRYPTO_FIPS_TEXT static void ed_private_from_seed(EdPrivate *k, const uint8_t *seed) {
    burrow__fips140_record_approved();
    memcpy(k->seed, seed, ED25519_SEED_SIZE);
    ed_expand_seed(k);
    Edwards25519Point A;
    burrow__ge_scalar_base_mult(&A, &k->s);
    burrow__ge_bytes(&A, k->pub);
}

/* newPrivateKey, for 64 bytes. The public key is taken as it is, and not
 * decoded into a point, which Go notes would add a fifth to the cost of a
 * signature for something signing does not use. */
CRYPTO_FIPS_TEXT static void ed_private_from_bytes(EdPrivate *k, const uint8_t *priv) {
    memcpy(k->seed, priv, ED25519_SEED_SIZE);
    ed_expand_seed(k);
    memcpy(k->pub, priv + 32, ED25519_PUBLIC_KEY_SIZE);
}

/* The domain separation in front of every hash of Ed25519ph and Ed25519ctx,
 * from section 2 and section 5.1 of RFC 8032. Plain Ed25519 has none. Both
 * strings are 32 bytes and a final byte saying which. */
typedef enum EdDom { ED_DOM_PURE, ED_DOM_PH, ED_DOM_CTX } EdDom;

CRYPTO_FIPS_TEXT static void ed_write_dom(Sha512Digest *h, EdDom dom, Str context) {
    static const char prefix[] = "SigEd25519 no Ed25519 collisions";
    if (dom == ED_DOM_PURE)
        return;
    uint8_t flag[2] = {dom == ED_DOM_PH ? 1 : 0, (uint8_t)context.len};
    burrow__sha512_write(h, prefix, (Int)sizeof(prefix) - 1);
    burrow__sha512_write(h, flag, 2);
    burrow__sha512_write(h, context.p, context.len);
}

CRYPTO_FIPS_TEXT static void ed_sign_with_dom(uint8_t signature[ED25519_SIGNATURE_SIZE],
                                              const EdPrivate *k, Slice message,
                                              EdDom dom, Str context) {
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
CRYPTO_FIPS_TEXT static Error ed_sign_variant(uint8_t signature[ED25519_SIGNATURE_SIZE],
                                              const EdPrivate *k, Slice message,
                                              EdDom dom, Str context) {
    /* FIPS 186-5 specifies Ed25519 and Ed25519ph (with context), but not
     * Ed25519ctx. */
    if (dom == ED_DOM_CTX)
        burrow__fips140_record_non_approved();
    else
        burrow__fips140_record_approved();
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

CRYPTO_FIPS_TEXT static Error ed_verify_with_dom(const Edwards25519Point *A,
                                                 const uint8_t a_bytes[32],
                                                 Slice message, Slice sig, EdDom dom,
                                                 Str context) {
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

/* ------------------------------------------------------------- self-tests */

/* From Go's src/crypto/internal/fips140/ed25519/cast.go. */

CRYPTO_FIPS_RODATA static const uint8_t ed_cast_seed[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

CRYPTO_FIPS_RODATA static const uint8_t ed_cast_want[64] = {
    0xbd, 0xe7, 0xa5, 0xf3, 0x40, 0x73, 0xb9, 0x5a, 0x2e, 0x6d, 0x63, 0x20, 0x0a,
    0xd5, 0x92, 0x9b, 0xa2, 0x3d, 0x00, 0x44, 0xb4, 0xc5, 0xfd, 0x62, 0x1d, 0x5e,
    0x33, 0x2f, 0xe4, 0x61, 0x42, 0x31, 0x5b, 0x10, 0x53, 0x13, 0x4d, 0xcb, 0xd1,
    0x1b, 0x2a, 0xf6, 0xcd, 0x0e, 0xdb, 0x9a, 0xd3, 0x1e, 0x35, 0xdb, 0x0b, 0xcf,
    0x58, 0x90, 0x4f, 0xd7, 0x69, 0x38, 0xed, 0x30, 0x51, 0x0f, 0xaa, 0x03,
};

/* NewPublicKey, for the 32 bytes of a key the module made. */
CRYPTO_FIPS_TEXT static Error ed_public_point(Edwards25519Point *A,
                                              const uint8_t *pub) {
    Slice b = slice_from((void *)(uintptr_t)pub, 32, 32, TYPE_BYTE);
    if (burrow__ge_set_bytes(A, b, NULL) == NULL)
        return ed_error("ed25519: bad public key");
    return BURROW_NO_ERROR;
}

CRYPTO_FIPS_TEXT static Error ed_cast(void *arg) {
    (void)arg;
    static const char msg[] = "CAST";
    EdPrivate k;
    ed_private_from_seed(&k, ed_cast_seed);
    Edwards25519Point A;
    Error err = ed_public_point(&A, k.pub);
    if (BURROW_FAILED(err))
        return err;
    uint8_t sig[ED25519_SIGNATURE_SIZE];
    Slice m = slice_from((void *)(uintptr_t)msg, 4, 4, TYPE_BYTE);
    ed_sign_with_dom(sig, &k, m, ED_DOM_PURE, BURROW_STR_EMPTY);
    if (memcmp(sig, ed_cast_want, sizeof sig) != 0)
        return errors_new(error_allocator(), BURROW_S("unexpected result"));
    return ed_verify_with_dom(
        &A, k.pub, m,
        slice_from(sig, ED25519_SIGNATURE_SIZE, ED25519_SIGNATURE_SIZE, TYPE_BYTE),
        ED_DOM_PURE, BURROW_STR_EMPTY);
}

CRYPTO_FIPS_TEXT static void ed_self_test_once(void *env) {
    (void)env;
    burrow__fips140_cast("Ed25519 sign and verify", ed_cast, NULL);
}

static SyncOnce ed_self_test;

/* fipsSelfTest, Go's sync.OnceFunc, which every signature and verification of
 * the module runs first. */
CRYPTO_FIPS_TEXT static void ed_fips_self_test(void) {
    sync_once_do(&ed_self_test, BURROW_FN(Func, ed_self_test_once, NULL));
}

/* pairwiseTest: Sign and Verify, the self-test and all, of "PCT". */
CRYPTO_FIPS_TEXT static Error ed_pct(void *arg) {
    static const char msg[] = "PCT";
    const EdPrivate *k = arg;
    Slice m = slice_from((void *)(uintptr_t)msg, 3, 3, TYPE_BYTE);
    uint8_t sig[ED25519_SIGNATURE_SIZE];
    ed_fips_self_test();
    ed_sign_with_dom(sig, k, m, ED_DOM_PURE, BURROW_STR_EMPTY);
    Edwards25519Point A;
    Error err = ed_public_point(&A, k->pub);
    if (BURROW_FAILED(err))
        return err;
    ed_fips_self_test();
    return ed_verify_with_dom(
        &A, k->pub, m,
        slice_from(sig, ED25519_SIGNATURE_SIZE, ED25519_SIGNATURE_SIZE, TYPE_BYTE),
        ED_DOM_PURE, BURROW_STR_EMPTY);
}

/* ------------------------------------------------------------------- keys */

/* A copy of n bytes at p from a, or a nil Slice with no memory. */
CRYPTO_FIPS_TEXT static Slice ed_copy(Alloc *a, const void *p, Int n) {
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    if (out.p != NULL)
        memcpy(out.p, p, (size_t)n);
    return out;
}

CRYPTO_FIPS_TEXT bool ed25519_public_key_equal(Ed25519PublicKey pub,
                                               CryptoPublicKey x) {
    if (x.t != TYPE_ED25519_PUBLIC_KEY || x.data == NULL)
        return false;
    return subtle_constant_time_compare(pub, *(const Slice *)x.data) == 1;
}

CRYPTO_FIPS_TEXT Ed25519PublicKey ed25519_private_key_public(Ed25519PrivateKey priv,
                                                             Alloc *a) {
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

CRYPTO_FIPS_TEXT bool ed25519_private_key_equal(Ed25519PrivateKey priv,
                                                CryptoPrivateKey x) {
    if (x.t != TYPE_ED25519_PRIVATE_KEY || x.data == NULL)
        return false;
    return subtle_constant_time_compare(priv, *(const Slice *)x.data) == 1;
}

CRYPTO_FIPS_TEXT Slice ed25519_private_key_seed(Ed25519PrivateKey priv, Alloc *a) {
    if (priv.cap < ED25519_SEED_SIZE)
        runtime_slice_bounds_out_of_range(0, ED25519_SEED_SIZE, priv.cap);
    return ed_copy(a, priv.p, ED25519_SEED_SIZE);
}

/* -------------------------------------------------------------- options */

CRYPTO_FIPS_TEXT CryptoHash ed25519_options_hash_func(const Ed25519Options *o) {
    return o->hash;
}

CRYPTO_FIPS_TEXT static CryptoHash ed_options_hash_func(void *self) {
    return ed25519_options_hash_func(self);
}

static const CryptoSignerOptsVT ed_options_vt = {TYPE_ED25519_OPTIONS,
                                                 ed_options_hash_func};

CRYPTO_FIPS_TEXT CryptoSignerOpts
ed25519_options_as_signer_opts(const Ed25519Options *o) {
    CryptoSignerOpts out = {&ed_options_vt, (void *)(uintptr_t)o};
    return out;
}

/* ---------------------------------------------------------------- signing */

/* The checks Go's privateKeyCache.Get makes on its way to the key: &priv[0]
 * for its key, and then the length. Returns the error for a bad length. */
CRYPTO_FIPS_TEXT static Error ed_unpack_private(EdPrivate *k, Ed25519PrivateKey priv) {
    if (priv.len == 0)
        runtime_index_out_of_range(0, 0);
    burrow__fips140_record_approved();
    if (priv.len != ED25519_PRIVATE_KEY_SIZE)
        return ed_error_n("ed25519: bad private key length: ", priv.len);
    ed_private_from_bytes(k, priv.p);
    return BURROW_NO_ERROR;
}

CRYPTO_FIPS_TEXT Slice ed25519_private_key_sign(Ed25519PrivateKey priv, Alloc *a,
                                                IoReader rand, Slice message,
                                                CryptoSignerOpts opts, Error *err) {
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
    if (dom == ED_DOM_CTX && burrow__fips140only_enforced()) {
        BURROW_OUT(err, ed_error("crypto/ed25519: use of Ed25519ctx is not allowed in "
                                 "FIPS 140-only mode"));
        return slice_nil(TYPE_BYTE);
    }

    ed_fips_self_test();
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

CRYPTO_FIPS_TEXT static CryptoPublicKey ed_signer_public(void *self) {
    Ed25519Signer *s = self;
    return BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &s->pub);
}

CRYPTO_FIPS_TEXT static Slice ed_signer_sign(void *self, Alloc *a, IoReader rand,
                                             Slice digest, CryptoSignerOpts opts,
                                             Error *err) {
    return ed25519_private_key_sign(((Ed25519Signer *)self)->priv, a, rand, digest,
                                    opts, err);
}

static const CryptoSignerVT ed_signer_vt = {TYPE_ED25519_PRIVATE_KEY, ed_signer_public,
                                            ed_signer_sign};

CRYPTO_FIPS_TEXT CryptoSigner ed25519_private_key_signer(Ed25519PrivateKey priv,
                                                         Ed25519Signer *s) {
    s->priv = priv;
    /* priv[32:], which is what Public copies. */
    if (priv.len < 32)
        runtime_slice_bounds_out_of_range(32, priv.len, priv.cap);
    s->pub = slice_from((Byte *)priv.p + 32, priv.len - 32, priv.cap - 32, TYPE_BYTE);
    CryptoSigner out = {&ed_signer_vt, s};
    return out;
}

/* ------------------------------------------------------------ functions */

CRYPTO_FIPS_TEXT Ed25519PrivateKey ed25519_new_key_from_seed(Alloc *a, Slice seed) {
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

CRYPTO_FIPS_TEXT Ed25519PublicKey ed25519_generate_key(Alloc *a, IoReader random,
                                                       Ed25519PrivateKey *priv,
                                                       Error *err) {
    *priv = slice_nil(TYPE_BYTE);
    if (random.vt == NULL)
        random = burrow__crypto_rand_nil_reader();

    if (burrow__fips140only_enforced() &&
        !burrow__fips140only_approved_random_reader(random)) {
        BURROW_OUT(err,
                   ed_error("crypto/ed25519: only crypto/rand.Reader is allowed in "
                            "FIPS 140-only mode"));
        return slice_nil(TYPE_BYTE);
    }

    uint8_t seed[ED25519_SEED_SIZE];
    Slice seed_slice =
        slice_from(seed, ED25519_SEED_SIZE, ED25519_SEED_SIZE, TYPE_BYTE);
    bool approved = burrow__crypto_rand_is_default_reader(random);
    if (approved) {
        burrow__crypto_rand_system(seed_slice);
    } else {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(random, seed_slice, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_BYTE);
        }
    }

    /* Go's fips140/ed25519.GenerateKey, which the default reader goes through,
     * runs the PCT, and NewKeyFromSeed, which other readers go through, does
     * not. */
    EdPrivate pk;
    ed_private_from_seed(&pk, seed);
    if (approved)
        burrow__fips140_pct("Ed25519 sign and verify PCT", ed_pct, &pk);
    Ed25519PrivateKey k =
        slice_make(a, TYPE_BYTE, ED25519_PRIVATE_KEY_SIZE, ED25519_PRIVATE_KEY_SIZE);
    if (k.p != NULL) {
        memcpy(k.p, pk.seed, ED25519_SEED_SIZE);
        memcpy((Byte *)k.p + ED25519_SEED_SIZE, pk.pub, ED25519_PUBLIC_KEY_SIZE);
    }
    memset(&pk, 0, sizeof pk);
    memset(seed, 0, sizeof seed);
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

CRYPTO_FIPS_TEXT Slice ed25519_sign(Alloc *a, Ed25519PrivateKey priv, Slice message) {
    EdPrivate k;
    Error e = ed_unpack_private(&k, priv);
    if (BURROW_FAILED(e))
        ed_panic_n("ed25519: bad private key: ed25519: bad private key length: ",
                   priv.len);
    ed_fips_self_test();
    burrow__fips140_record_approved();
    uint8_t signature[ED25519_SIGNATURE_SIZE];
    ed_sign_with_dom(signature, &k, message, ED_DOM_PURE, BURROW_STR_EMPTY);
    return ed_copy(a, signature, ED25519_SIGNATURE_SIZE);
}

CRYPTO_FIPS_TEXT bool ed25519_verify(Ed25519PublicKey pub, Slice message, Slice sig) {
    Ed25519Options opts = {0, BURROW_STR_EMPTY};
    return BURROW_OK(ed25519_verify_with_options(pub, message, sig, &opts));
}

CRYPTO_FIPS_TEXT Error ed25519_verify_with_options(Ed25519PublicKey pub, Slice message,
                                                   Slice sig,
                                                   const Ed25519Options *opts) {
    if (pub.len != ED25519_PUBLIC_KEY_SIZE)
        ed_panic_n("ed25519: bad public key length: ", pub.len);

    /* NewPublicKey: set_bytes checks that the point is on the curve. */
    Edwards25519Point A;
    if (burrow__ge_set_bytes(&A, pub, NULL) == NULL)
        return ed_error("ed25519: bad public key");

    EdDom dom;
    if (opts->hash == CRYPTO_SHA512) {
        ed_fips_self_test();
        burrow__fips140_record_approved();
        dom = ED_DOM_PH;
        if (message.len != SHA512_SIZE)
            return ed_error_n("ed25519: bad Ed25519ph message hash length: ",
                              message.len);
        if (opts->context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ph context length: ",
                              opts->context.len);
    } else if (opts->hash == 0 && opts->context.len != 0) {
        if (burrow__fips140only_enforced())
            return ed_error("crypto/ed25519: use of Ed25519ctx is not allowed in FIPS "
                            "140-only mode");
        ed_fips_self_test();
        /* FIPS 186-5 specifies Ed25519 and Ed25519ph (with context), but not
         * Ed25519ctx. */
        burrow__fips140_record_non_approved();
        dom = ED_DOM_CTX;
        if (opts->context.len > 255)
            return ed_error_n("ed25519: bad Ed25519ctx context length: ",
                              opts->context.len);
    } else if (opts->hash == 0) {
        ed_fips_self_test();
        burrow__fips140_record_approved();
        dom = ED_DOM_PURE;
    } else {
        return ed_error(
            "ed25519: expected opts.Hash zero (unhashed message, for standard "
            "Ed25519) or SHA-512 (for Ed25519ph)");
    }
    return ed_verify_with_dom(&A, pub.p, message, sig, dom, opts->context);
}
