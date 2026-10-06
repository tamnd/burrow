#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static void print_error(Error err) {
    if (BURROW_OK(err)) {
        printf("ok\n");
        return;
    }
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_text(Slice b) {
    printf("%.*s\n", (int)b.len, (const char *)b.p);
}

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static void sign(Alloc *a) {
    // doc: sign
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 2048, &err);
    if (BURROW_FAILED(err))
        return;

    // Both schemes sign the hash of the message, not the message itself.
    Sha256Sum256Ret sum = sha256_sum256(text("burrow v0.3.0"));
    Slice hashed = slice_from(sum.a, sizeof sum.a, sizeof sum.a, TYPE_BYTE);
    Slice sig =
        rsa_sign_pss(a, crypto_rand_reader, priv, CRYPTO_SHA256, hashed, NULL, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d\n", (int)sig.len);
    print_error(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, hashed, sig, NULL));

    // PKCS #1 v1.5 needs no randomness and gives the same signature each time.
    Slice old = rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, hashed, &err);
    if (BURROW_FAILED(err))
        return;
    print_error(rsa_verify_pkcs1_v15(&priv->public_key, CRYPTO_SHA256, hashed, old));

    // A signature of some other message does not verify.
    Sha256Sum256Ret other = sha256_sum256(text("burrow v0.3.1"));
    Slice other_hashed = slice_from(other.a, sizeof other.a, sizeof other.a, TYPE_BYTE);
    print_error(
        rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, other_hashed, sig, NULL));
    rsa_private_key_free(priv, a);
    // doc: end
}

static void encrypt(Alloc *a) {
    // doc: encrypt
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 2048, &err);
    if (BURROW_FAILED(err))
        return;

    // The label is not secret, but decrypting needs the same one.
    Slice ct = rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &priv->public_key,
                                text("a session key"), text("orders"), &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d\n", (int)ct.len);
    Slice pt = rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, priv, ct,
                                text("orders"), &err);
    if (BURROW_FAILED(err))
        return;
    print_text(pt);
    rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, priv, ct, text("invoices"), &err);
    print_error(err);
    rsa_private_key_free(priv, a);
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    rsa_generate_key(a, (IoReader){0}, 512, &err);
    print_error(err);

    err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 1024, &err);
    if (BURROW_FAILED(err))
        return;
    // OAEP with SHA-256 fits 128 - 2*32 - 2 = 62 bytes in a 1024 bit key.
    Byte big[63] = {0};
    rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &priv->public_key,
                     slice_from(big, sizeof big, sizeof big, TYPE_BYTE), (Slice){0},
                     &err);
    print_error(err);
    err = BURROW_NO_ERROR;
    rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, text("not a hash"), &err);
    print_error(err);
    rsa_private_key_free(priv, a);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    sign(a);
    encrypt(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
*/
