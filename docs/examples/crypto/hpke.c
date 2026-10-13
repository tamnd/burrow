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

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    return hex_decode_string(a, str_from_cstr(s), &err);
}

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static void exchange(Alloc *a) {
    // doc: exchange
    // In a real application, the private key would be stored and the
    // public key bytes sent to the sender.
    const HpkeKEM *kem = hpke_mlkem768_x25519();
    const HpkeKDF *kdf = hpke_hkdfsha256();
    const HpkeAEAD *aead = hpke_aes256_gcm();
    Error err = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
    if (BURROW_FAILED(err))
        return;
    Slice public_key_bytes = hpke_public_key_bytes(hpke_private_key_public_key(k), a);

    // The sender parses the public key and seals a message to it.
    HpkePublicKey *pk = hpke_kem_new_public_key(kem, a, public_key_bytes, &err);
    if (BURROW_FAILED(err))
        return;
    Slice ciphertext =
        hpke_seal(a, pk, kdf, aead, text("example"), text("|-()-|"), &err);
    if (BURROW_FAILED(err))
        return;

    // The recipient opens it with the private key.
    Slice plaintext = hpke_open(a, k, kdf, aead, text("example"), ciphertext, &err);
    if (BURROW_FAILED(err))
        return;
    printf("Decrypted message: %.*s\n", (int)plaintext.len, (const char *)plaintext.p);
    hpke_public_key_free(pk);
    hpke_private_key_free(k);
    // doc: end
}

static void derive(Alloc *a) {
    // doc: derive
    // The recipient of RFC 9180's first test vector, A.1, derives its key
    // from ikmR, and opens the first message the sender sent it.
    const HpkeKEM *kem = hpke_dhkem(ecdh_x25519());
    Error err = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_derive_key_pair(
        kem, a,
        unhex(a, "6db9df30aa07dd42ee5e8181afdb977e538f5e1fec8a06223f33f7013e525037"),
        &err);
    if (BURROW_FAILED(err))
        return;
    print_hex(a, hpke_public_key_bytes(hpke_private_key_public_key(k), a));

    Slice enc =
        unhex(a, "37fda3567bdbd628e88668c3c8d7e97d1d1253b6d4ea6d44c150f741f1bf4431");
    HpkeRecipient *r =
        hpke_new_recipient(a, enc, k, hpke_hkdfsha256(), hpke_aes128_gcm(),
                           text("Ode on a Grecian Urn"), &err);
    if (BURROW_FAILED(err))
        return;
    Slice pt = hpke_recipient_open(
        r, a, text("Count-0"),
        unhex(a, "f938558b5d72f1a23810b4be2ab4f84331acc02fc97babc53a52ae8218a355a9"
                 "6d8770ac83d07bea87e13c512a"),
        &err);
    if (BURROW_FAILED(err))
        return;
    printf("%.*s\n", (int)pt.len, (const char *)pt.p);
    print_hex(a, hpke_recipient_export(r, a, BURROW_S(""), 32, &err));
    hpke_private_key_free(k);
    // doc: end
}

static void export_only(Alloc *a) {
    // doc: export
    // With ExportOnly the two sides share secrets and nothing else.
    const HpkeKEM *kem = hpke_dhkem(ecdh_p256());
    Error err = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
    if (BURROW_FAILED(err))
        return;
    HpkeSender *s;
    Slice enc = hpke_new_sender(a, hpke_private_key_public_key(k), hpke_hkdfsha256(),
                                hpke_export_only(), text("session"), &s, &err);
    if (BURROW_FAILED(err))
        return;
    HpkeRecipient *r = hpke_new_recipient(a, enc, k, hpke_hkdfsha256(),
                                          hpke_export_only(), text("session"), &err);
    if (BURROW_FAILED(err))
        return;
    Slice x = hpke_sender_export(s, a, BURROW_S("key"), 16, &err);
    Slice y = hpke_recipient_export(r, a, BURROW_S("key"), 16, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d %d %s\n", (int)enc.len, (int)x.len,
           bytes_equal(x, y) ? "true" : "false");
    hpke_sender_seal(s, a, slice_nil(TYPE_BYTE), text("hi"), &err);
    print_error(err);
    hpke_private_key_free(k);
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Error err = BURROW_NO_ERROR;
    hpke_new_kem(0x0021, &err);
    print_error(err);
    err = BURROW_NO_ERROR;
    hpke_new_aead(0xabcd, &err);
    print_error(err);
    err = BURROW_NO_ERROR;

    HpkePrivateKey *k = hpke_kem_generate_key(hpke_dhkem(ecdh_x25519()), a, &err);
    if (BURROW_FAILED(err))
        return;
    Slice ct =
        hpke_seal(a, hpke_private_key_public_key(k), hpke_hkdfsha256(),
                  hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE), text("hi"), &err);
    if (BURROW_FAILED(err))
        return;
    ((Byte *)ct.p)[ct.len - 1] ^= 1;
    hpke_open(a, k, hpke_hkdfsha256(), hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE),
              ct, &err);
    print_error(err);
    hpke_private_key_free(k);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    exchange(a);
    derive(a);
    export_only(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
Decrypted message: |-()-|
3948cfe0ad1ddb695d780e59077195da6c56506b027329794ab02bca80815c4d
Beauty is truth, truth beauty
3853fe2b4035195a573ffc53856e77058e15d9ea064de3e59f4961d0095250ee
65 16 true
export-only instantiation
unsupported KEM
unsupported AEAD abcd
chacha20poly1305: message authentication failed
*/
