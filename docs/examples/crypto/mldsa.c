#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_error(Error err) {
    if (BURROW_OK(err)) {
        printf("ok\n");
        return;
    }
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static void sign(Alloc *a) {
    // doc: sign
    // The signer makes a key and publishes the public key's bytes.
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_generate_key(mldsa_mldsa44(), a, &err);
    if (BURROW_FAILED(err))
        return;
    Slice pub_bytes = mldsa_public_key_bytes(mldsa_private_key_public_key(sk), a);

    // The context keeps signatures made for one purpose from passing for
    // another. Sign and Verify have to be given the same one.
    MldsaOptions opts = {BURROW_S("release manifest")};
    Slice msg = text("burrow v0.3.0");
    Slice sig = mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                                       mldsa_options_as_signer_opts(&opts), &err);
    if (BURROW_FAILED(err))
        return;

    // The verifier has only the public key's bytes.
    MldsaPublicKey *pk = mldsa_new_public_key(mldsa_mldsa44(), a, pub_bytes, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d %d\n", (int)pub_bytes.len, (int)sig.len);
    print_error(mldsa_verify(pk, msg, sig, &opts));
    print_error(mldsa_verify(pk, msg, sig, NULL));
    print_error(mldsa_verify(pk, text("burrow v0.3.1"), sig, &opts));
    mldsa_public_key_free(pk);
    mldsa_private_key_free(sk);
    // doc: end
}

static void deterministic(Alloc *a) {
    // doc: deterministic
    // A private key is its 32 byte seed. The same seed gives the same key,
    // and SignDeterministic gives the same signature of the same message.
    Byte seed[MLDSA_PRIVATE_KEY_SIZE];
    for (int i = 0; i < MLDSA_PRIVATE_KEY_SIZE; i++)
        seed[i] = (Byte)i;
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_new_private_key(
        mldsa_mldsa65(), a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE),
        &err);
    if (BURROW_FAILED(err))
        return;
    Slice sig = mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                                     (CryptoSignerOpts){0}, &err);
    if (BURROW_FAILED(err))
        return;
    Sha256Sum256Ret sum = sha256_sum256(sig);
    print_hex(a, slice_from(sum.a, sizeof sum.a, sizeof sum.a, TYPE_BYTE));
    mldsa_private_key_free(sk);
    // doc: end
}

static void signer(Alloc *a) {
    // doc: signer
    // Code that works with any signature scheme takes a CryptoSigner.
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_generate_key(mldsa_mldsa87(), a, &err);
    if (BURROW_FAILED(err))
        return;
    CryptoSigner s = mldsa_private_key_signer(sk);
    Slice msg = text("any message");
    Slice sig =
        crypto_signer_sign(s, a, (IoReader){0}, msg, (CryptoSignerOpts){0}, &err);
    if (BURROW_FAILED(err))
        return;
    CryptoPublicKey pub = crypto_signer_public(s);
    printf("%.*s %d\n", (int)pub.t->name.len, (const char *)pub.t->name.p,
           (int)sig.len);
    print_error(mldsa_verify(pub.data, msg, sig, NULL));
    mldsa_private_key_free(sk);
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    Byte zeros[MLDSA44_PUBLIC_KEY_SIZE] = {0};
    mldsa_new_private_key(mldsa_mldsa44(), a, slice_from(zeros, 16, 16, TYPE_BYTE),
                          &err);
    print_error(err);
    mldsa_new_public_key(mldsa_mldsa65(), a,
                         slice_from(zeros, sizeof zeros, sizeof zeros, TYPE_BYTE),
                         &err);
    print_error(err);

    err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_new_private_key(
        mldsa_mldsa44(), a, slice_from(zeros, 32, 32, TYPE_BYTE), &err);
    MldsaOptions opts = {strings_repeat(a, BURROW_S("x"), 256)};
    mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                         mldsa_options_as_signer_opts(&opts), &err);
    print_error(err);
    CryptoHash h = CRYPTO_SHA256;
    mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                         crypto_hash_as_signer_opts(&h), &err);
    print_error(err);
    print_error(mldsa_verify(mldsa_private_key_public_key(sk), text("hello"),
                             slice_from(zeros, 100, 100, TYPE_BYTE), NULL));
    mldsa_private_key_free(sk);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    sign(a);
    deterministic(a);
    signer(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
1312 2420
ok
mldsa: invalid signature
mldsa: invalid signature
1494cfed0b3c18c0b8534b86460e06cec9ae1b8f0ced906dc8b140241a1ac738
PublicKey 4627
ok
mldsa: invalid seed length
mldsa: invalid public key length
mldsa: context too long
mldsa: invalid SignerOpts
mldsa: invalid signature length
*/
