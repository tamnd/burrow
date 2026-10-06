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

static void certs(Alloc *a) {
    // doc: create
    Byte seed[ED25519_SEED_SIZE] = {0};
    for (int i = 0; i < ED25519_SEED_SIZE; i++)
        seed[i] = (Byte)(i + 1);
    Ed25519Signer ca_key;
    CryptoSigner ca_signer = ed25519_private_key_signer(
        ed25519_new_key_from_seed(
            a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE)),
        &ca_key);

    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("Example Root");
    tmpl.not_before = time_date(2026, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
    tmpl.not_after = time_date(2036, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
    tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;

    // Self-signed, so the template is its own parent.
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(
        a, (IoReader){0}, &tmpl, &tmpl,
        BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &ca_key.pub), ca_signer, &err);
    if (BURROW_FAILED(err))
        return;
    printf("%d bytes\n", (int)der.len);
    // doc: end

    // doc: certparse
    X509Certificate *ca = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        return;
    print(pkix_name_string(ca->subject, a));
    print(x509_signature_algorithm_string(ca->signature_algorithm, a));
    print(hex_encode_to_string(a, ca->subject_key_id));
    print(time_format(ca->not_after, a, TIME_RFC3339));
    err = x509_certificate_check_signature_from(ca, ca);
    printf("signed by itself: %s\n", BURROW_FAILED(err) ? "no" : "yes");
    // doc: end

    // doc: request
    for (int i = 0; i < ED25519_SEED_SIZE; i++)
        seed[i] = (Byte)(0x80 + i);
    Ed25519Signer leaf_key;
    CryptoSigner leaf_signer = ed25519_private_key_signer(
        ed25519_new_key_from_seed(
            a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE)),
        &leaf_key);

    Str host = BURROW_S("www.example.com");
    X509CertificateRequest req = {0};
    req.subject.common_name = host;
    req.dns_names = slice_append(a, slice_nil(TYPE_STRING), &host, 1);
    Slice csr_der =
        x509_create_certificate_request(a, (IoReader){0}, &req, leaf_signer, &err);
    if (BURROW_FAILED(err))
        return;
    X509CertificateRequest *csr = x509_parse_certificate_request(a, csr_der, &err);
    if (BURROW_FAILED(err))
        return;
    err = x509_certificate_request_check_signature(csr);
    printf("request for %.*s: %s\n", (int)csr->subject.common_name.len,
           csr->subject.common_name.p, BURROW_FAILED(err) ? "bad" : "ok");
    // doc: end

    // doc: issue
    // The CA signs a certificate for the key in the request.
    X509Certificate leaf_tmpl = {0};
    leaf_tmpl.serial_number = big_new_int(a, 2);
    leaf_tmpl.subject = csr->subject;
    leaf_tmpl.dns_names = csr->dns_names;
    leaf_tmpl.not_before = tmpl.not_before;
    leaf_tmpl.not_after = time_date(2027, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
    leaf_tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
    X509ExtKeyUsage server = X509_EXT_KEY_USAGE_SERVER_AUTH;
    leaf_tmpl.ext_key_usage = slice_append(a, slice_nil(TYPE_INT), &server, 1);
    Slice leaf_der = x509_create_certificate(a, (IoReader){0}, &leaf_tmpl, ca,
                                             csr->public_key, ca_signer, &err);
    if (BURROW_FAILED(err))
        return;
    X509Certificate *leaf = x509_parse_certificate(a, leaf_der, &err);
    if (BURROW_FAILED(err))
        return;
    print(pkix_name_string(leaf->issuer, a));
    print(hex_encode_to_string(a, leaf->authority_key_id));
    // doc: end

    // doc: verify
    X509CertPool *roots = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(roots, ca);
    X509VerifyOptions opts = {0};
    opts.roots = roots;
    opts.dns_name = BURROW_S("www.example.com");
    opts.current_time = time_date(2026, TIME_JUNE, 1, 0, 0, 0, 0, time_utc_loc);
    Slice chains = x509_certificate_verify(leaf, a, opts, &err);
    if (BURROW_FAILED(err))
        return;
    X509CertificateChain *chain = chains.p;
    printf("%d chain, %d certificates\n", (int)chains.len, (int)chain[0].len);

    opts.dns_name = BURROW_S("mail.example.com");
    x509_certificate_verify(leaf, a, opts, &err);
    print(error_text(err));

    opts.dns_name = BURROW_S("www.example.com");
    opts.current_time = time_date(2027, TIME_JUNE, 1, 0, 0, 0, 0, time_utc_loc);
    x509_certificate_verify(leaf, a, opts, &err);
    print(error_text(err));

    opts.roots = x509_new_cert_pool(a);
    opts.current_time = time_date(2026, TIME_JUNE, 1, 0, 0, 0, 0, time_utc_loc);
    x509_certificate_verify(leaf, a, opts, &err);
    print(error_text(err));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    keys(arena_allocator(&ar));
    oids(arena_allocator(&ar));
    certs(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/
