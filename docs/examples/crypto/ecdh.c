#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

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

static void exchange(Alloc *a) {
    // doc: exchange
    Error err = BURROW_NO_ERROR;
    const EcdhCurve *curve = ecdh_x25519();
    EcdhPrivateKey *alice =
        ecdh_curve_generate_key(curve, a, (IoReader){NULL, NULL}, &err);
    EcdhPrivateKey *bob =
        ecdh_curve_generate_key(curve, a, (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return;

    // Each side sends the other its public key, as bytes.
    Slice alice_sends = ecdh_public_key_bytes(ecdh_private_key_public_key(alice), a);
    Slice bob_sends = ecdh_public_key_bytes(ecdh_private_key_public_key(bob), a);

    EcdhPublicKey *from_bob = ecdh_curve_new_public_key(curve, a, bob_sends, &err);
    EcdhPublicKey *from_alice = ecdh_curve_new_public_key(curve, a, alice_sends, &err);
    if (BURROW_FAILED(err))
        return;
    Slice s1 = ecdh_private_key_ecdh(alice, a, from_bob, &err);
    Slice s2 = ecdh_private_key_ecdh(bob, a, from_alice, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d %d\n", (int)s1.len, bytes_equal(s1, s2));
    // doc: end
}

static void vector(Alloc *a) {
    // doc: vector
    Error err = BURROW_NO_ERROR;
    EcdhPrivateKey *k = ecdh_curve_new_private_key(
        ecdh_x25519(), a,
        unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"),
        &err);
    EcdhPublicKey *peer = ecdh_curve_new_public_key(
        ecdh_x25519(), a,
        unhex(a, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"),
        &err);
    if (BURROW_FAILED(err))
        return;
    print_hex(a, ecdh_public_key_bytes(ecdh_private_key_public_key(k), a));
    print_hex(a, ecdh_private_key_ecdh(k, a, peer, &err));
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    EcdhPrivateKey *k =
        ecdh_curve_generate_key(ecdh_p256(), a, (IoReader){NULL, NULL}, &err);
    EcdhPrivateKey *other =
        ecdh_curve_generate_key(ecdh_p384(), a, (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return;
    ecdh_private_key_ecdh(k, a, ecdh_private_key_public_key(other), &err);
    print_error(err);

    err = BURROW_NO_ERROR;
    ecdh_curve_new_public_key(ecdh_p256(), a, unhex(a, "00"), &err);
    print_error(err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    exchange(a);
    vector(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
32 1
8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a
4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742
crypto/ecdh: private key and public key curves do not match
crypto/ecdh: invalid public key
*/
