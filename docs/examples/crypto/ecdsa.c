#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    return hex_decode_string(a, str_from_cstr(s), &err);
}

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

static void sign(Alloc *a) {
    // doc: sign
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv =
        ecdsa_generate_key(a, elliptic_p256(), (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return;

    Sha256Sum256Ret hash = sha256_sum256(text("hello, world"));
    Slice h = slice_from(hash.a, 32, 32, TYPE_BYTE);

    Slice sig = ecdsa_sign_asn1(a, (IoReader){NULL, NULL}, priv, h, &err);
    if (BURROW_FAILED(err))
        return;

    bool valid = ecdsa_verify_asn1(&priv->public_key, h, sig);
    printf("signature verified: %s\n", valid ? "true" : "false");
    // doc: end
}

static void deterministic(Alloc *a) {
    // doc: deterministic
    // The P-256 key of RFC 6979, appendix A.2.5.
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv = ecdsa_parse_raw_private_key(
        a, elliptic_p256(),
        unhex(a, "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"),
        &err);
    if (BURROW_FAILED(err))
        return;

    Sha256Sum256Ret hash = sha256_sum256(text("sample"));
    CryptoHash sha256 = CRYPTO_SHA256;
    Slice sig = ecdsa_private_key_sign(priv, a, (IoReader){NULL, NULL},
                                       slice_from(hash.a, 32, 32, TYPE_BYTE),
                                       crypto_hash_as_signer_opts(&sha256), &err);
    if (BURROW_FAILED(err))
        return;
    print_hex(a, sig);
    // doc: end
}

static void encoding(Alloc *a) {
    // doc: encoding
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv =
        ecdsa_generate_key(a, elliptic_p384(), (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return;

    Slice pub_bytes = ecdsa_public_key_bytes(&priv->public_key, a, &err);
    Slice priv_bytes = ecdsa_private_key_bytes(priv, a, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d %d\n", (int)pub_bytes.len, (int)priv_bytes.len);

    EcdsaPublicKey *pub =
        ecdsa_parse_uncompressed_public_key(a, elliptic_p384(), pub_bytes, &err);
    EcdsaPrivateKey *back =
        ecdsa_parse_raw_private_key(a, elliptic_p384(), priv_bytes, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d %d\n",
           ecdsa_public_key_equal(&priv->public_key,
                                  BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, pub)),
           ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, back)));
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv =
        ecdsa_generate_key(a, elliptic_p256(), (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return;
    ecdsa_sign_asn1(a, (IoReader){NULL, NULL}, priv, (Slice){0}, &err);
    print_error(err);

    err = BURROW_NO_ERROR;
    ecdsa_parse_raw_private_key(a, elliptic_p256(), unhex(a, "0102"), &err);
    print_error(err);

    err = BURROW_NO_ERROR;
    ecdsa_parse_uncompressed_public_key(a, elliptic_p256(), unhex(a, "00"), &err);
    print_error(err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    sign(a);
    deterministic(a);
    encoding(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
signature verified: true
3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8
97 48
1 1
ecdsa: hash cannot be empty
invalid scalar length
ecdsa: invalid uncompressed public key
*/
