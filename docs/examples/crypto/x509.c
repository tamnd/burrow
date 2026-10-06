#include <stdio.h>

#include "burrow/burrow.h"

static void print(Str s) {
    printf("%.*s\n", (int)s.len, s.p);
}

static void keys(Alloc *a) {
    // doc: pkcs8
    Byte seed[ED25519_SEED_SIZE] = {0};
    for (int i = 0; i < ED25519_SEED_SIZE; i++)
        seed[i] = (Byte)i;
    Ed25519PrivateKey priv = ed25519_new_key_from_seed(
        a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE));

    Error err = BURROW_NO_ERROR;
    Slice der = x509_marshal_pkcs8_private_key(
        a, BURROW_ANY(TYPE_ED25519_PRIVATE_KEY, &priv), &err);
    if (BURROW_FAILED(err))
        return;
    PemBlock block = {.type = BURROW_S("PRIVATE KEY"), .bytes = der};
    Slice pem = pem_encode_to_memory(a, &block);
    printf("%.*s", (int)pem.len, (const char *)pem.p);
    // doc: end

    // doc: parse
    Slice rest;
    PemBlock *b = pem_decode(a, pem, &rest);
    Any key = x509_parse_pkcs8_private_key(a, b->bytes, &err);
    if (BURROW_FAILED(err))
        return;
    if (key.t == TYPE_ED25519_PRIVATE_KEY) {
        Ed25519PrivateKey *k = key.data;
        Ed25519PublicKey pub = ed25519_private_key_public(*k, a);
        Slice spki = x509_marshal_pkix_public_key(
            a, BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &pub), &err);
        print(hex_encode_to_string(a, spki));
    }

    // The wrong parser says which one to use.
    x509_parse_pkcs1_private_key(a, b->bytes, &err);
    print(error_text(err));
    // doc: end
}

static void oids(Alloc *a) {
    // doc: oid
    Error err = BURROW_NO_ERROR;
    X509OID oid = x509_parse_oid(a, BURROW_S("1.3.6.1.4.1.11129.2.4.2"), &err);
    Slice der = x509_oid_marshal_binary(oid, a, &err);
    print(hex_encode_to_string(a, der));

    // Arcs past 64 bits are fine too.
    oid = x509_parse_oid(a, BURROW_S("2.25.329800735698586629295641978511506172918"),
                         &err);
    print(x509_oid_string(oid, a));

    print(
        x509_oid_string(x509_ext_key_usage_oid(X509_EXT_KEY_USAGE_SERVER_AUTH, a), a));

    x509_parse_oid(a, BURROW_S("1.2."), &err);
    print(error_text(err));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    keys(arena_allocator(&ar));
    oids(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/
