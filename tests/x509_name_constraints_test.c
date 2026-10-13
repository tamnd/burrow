/* crypto/x509 name constraint tests: Go's name_constraints_test.go.
 *
 * The nameConstraintsTests and rfc2821Tests tables are read out of the Go
 * source by tools/gen-x509-tests.sh. Go can also check each chain with the
 * openssl command, behind a constant that is false, so that part is left out.
 * Each case makes new keys where Go takes them from a sync.Pool, which only
 * saves time.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/hex.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_name_constraints_test_gen.h"

#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

#define ARENA(a)                                                                       \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const Int oid_extension_subject_alt_name[] = {2, 5, 29, 17};
static const Int oid_extension_name_constraints[] = {2, 5, 29, 30};
static const Int oid_other_eku[] = {2, 4, 1, 2, 3};

static Asn1ObjectIdentifier static_oid(const Int *arcs, Int n) {
    return (Asn1ObjectIdentifier){(void *)(uintptr_t)arcs, n, n, TYPE_INT};
}

static Slice bytes_of(Alloc *a, const Byte *b, Int n) {
    return slice_append(a, slice_nil(TYPE_BYTE), b, n);
}

static Slice append_byte(Alloc *a, Slice s, Byte b) {
    return slice_append(a, s, &b, 1);
}

static bool has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* The names in a generated list, which ends at the first NULL. */
static Int names_len(const char *const *v) {
    Int n = 0;
    while (n < X509_NC_MAX_NAMES && v[n] != NULL)
        n++;
    return n;
}

static Slice append_str(Alloc *a, Slice s, const char *v) {
    Str x = str_from_cstr(v);
    return slice_append(a, s, &x, 1);
}

static EcdsaPrivateKey *ec_key(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_name_constraints_test: ECDSA key generation failed"));
    return k;
}

/* new(big.Int).SetBytes of 16 random bytes. */
static BigInt *random_serial(Alloc *a) {
    Byte serial[16];
    crypto_rand_read(slice_from(serial, 16, 16, TYPE_BYTE), NULL);
    return big_int_set_bytes(big_new_int(a, 0), slice_from(serial, 16, 16, TYPE_BYTE));
}

/* parseEKUs */
static Error parse_ekus(Alloc *a, const char *const *strs, Slice *ekus,
                        Slice *unknowns) {
    *ekus = slice_nil(TYPE_INT);
    *unknowns = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
    for (Int i = 0; i < names_len(strs); i++) {
        const char *s = strs[i];
        X509ExtKeyUsage u;
        if (strcmp(s, "serverAuth") == 0) {
            u = X509_EXT_KEY_USAGE_SERVER_AUTH;
        } else if (strcmp(s, "clientAuth") == 0) {
            u = X509_EXT_KEY_USAGE_CLIENT_AUTH;
        } else if (strcmp(s, "email") == 0) {
            u = X509_EXT_KEY_USAGE_EMAIL_PROTECTION;
        } else if (strcmp(s, "netscapeSGC") == 0) {
            u = X509_EXT_KEY_USAGE_NETSCAPE_SERVER_GATED_CRYPTO;
        } else if (strcmp(s, "msSGC") == 0) {
            u = X509_EXT_KEY_USAGE_MICROSOFT_SERVER_GATED_CRYPTO;
        } else if (strcmp(s, "any") == 0) {
            u = X509_EXT_KEY_USAGE_ANY;
        } else if (strcmp(s, "other") == 0) {
            Asn1ObjectIdentifier oid = static_oid(oid_other_eku, LEN(oid_other_eku));
            *unknowns = slice_append(a, *unknowns, &oid, 1);
            continue;
        } else {
            return fmt_errorf_v("unknown EKU %q", s);
        }
        *ekus = slice_append(a, *ekus, &u, 1);
    }
    return BURROW_NO_ERROR;
}

/* customConstraintsExtension */
static PkixExtension custom_constraints_extension(Alloc *a, int type_num,
                                                  const Byte *constraint, Int n,
                                                  bool is_excluded) {
    /* tag 0 for permitted, 1 for excluded, constructed and context-specific */
    Slice contents = slice_nil(TYPE_BYTE);
    contents = append_byte(a, contents, (Byte)((is_excluded ? 1 : 0) | 32 | 0x80));
    contents = append_byte(a, contents, (Byte)(4 + n));
    contents = append_byte(a, contents, 0x30); /* SEQUENCE */
    contents = append_byte(a, contents, (Byte)(2 + n));
    contents = append_byte(a, contents, (Byte)type_num); /* GeneralName type */
    contents = append_byte(a, contents, (Byte)n);
    contents = slice_append(a, contents, constraint, n);

    Slice value = slice_nil(TYPE_BYTE);
    value = append_byte(a, value, 0x30); /* SEQUENCE */
    value = append_byte(a, value, (Byte)contents.len);
    value = slice_append(a, value, contents.p, contents.len);

    return (PkixExtension){
        static_oid(oid_extension_name_constraints, LEN(oid_extension_name_constraints)),
        false, value};
}

/* The parse closure in addConstraintsToTemplate. */
static Error parse_constraints(Alloc *a, const char *const *constraints,
                               Slice *dns_names, Slice *ips, Slice *email_addrs,
                               Slice *uri_domains) {
    *dns_names = *email_addrs = *uri_domains = slice_nil(TYPE_STRING);
    *ips = slice_nil(TYPE_X509_IP_NET_PTR);
    for (Int i = 0; i < names_len(constraints); i++) {
        const char *c = constraints[i];
        if (has_prefix(c, "dns:")) {
            *dns_names = append_str(a, *dns_names, c + 4);
        } else if (has_prefix(c, "ip:")) {
            NetIPNet *ip_net = NULL;
            Error err = BURROW_NO_ERROR;
            net_parse_cidr(a, str_from_cstr(c + 3), &ip_net, &err);
            if (BURROW_FAILED(err))
                return err;
            *ips = slice_append(a, *ips, &ip_net, 1);
        } else if (has_prefix(c, "email:")) {
            *email_addrs = append_str(a, *email_addrs, c + 6);
        } else if (has_prefix(c, "uri:")) {
            *uri_domains = append_str(a, *uri_domains, c + 4);
        } else {
            return fmt_errorf_v("unknown constraint %q", c);
        }
    }
    return BURROW_NO_ERROR;
}

/* handleSpecialConstraint */
static bool handle_special_constraint(Alloc *a, const char *constraint,
                                      bool is_excluded, X509Certificate *tmpl) {
    if (strcmp(constraint, "unknown:") != 0)
        return false;
    static const Byte one[] = {1};
    PkixExtension ext = custom_constraints_extension(
        a, 9 /* undefined GeneralName type */, one, LEN(one), is_excluded);
    tmpl->extra_extensions = slice_append(a, tmpl->extra_extensions, &ext, 1);
    return true;
}

/* addConstraintsToTemplate */
static Error add_constraints_to_template(Alloc *a, const X509ConstraintsSpec *c,
                                         X509Certificate *tmpl) {
    Int nok = names_len(c->ok), nbad = names_len(c->bad);
    if (nok == 1 && nbad == 0 && handle_special_constraint(a, c->ok[0], false, tmpl))
        return BURROW_NO_ERROR;
    if (nbad == 1 && nok == 0 && handle_special_constraint(a, c->bad[0], true, tmpl))
        return BURROW_NO_ERROR;

    Error err = parse_constraints(
        a, c->ok, &tmpl->permitted_dns_domains, &tmpl->permitted_ip_ranges,
        &tmpl->permitted_email_addresses, &tmpl->permitted_uri_domains);
    if (BURROW_FAILED(err))
        return err;
    err = parse_constraints(a, c->bad, &tmpl->excluded_dns_domains,
                            &tmpl->excluded_ip_ranges, &tmpl->excluded_email_addresses,
                            &tmpl->excluded_uri_domains);
    if (BURROW_FAILED(err))
        return err;
    return parse_ekus(a, c->ekus, &tmpl->ext_key_usage, &tmpl->unknown_ext_key_usage);
}

/* CreateCertificate then ParseCertificate, self-signed when parent is NULL. */
static X509Certificate *sign(Alloc *a, X509Certificate *tmpl,
                             const X509Certificate *parent, const EcdsaPrivateKey *key,
                             const EcdsaPrivateKey *parent_key, Error *err) {
    if (parent == NULL)
        parent = tmpl;
    Slice der = x509_create_certificate(a, (IoReader){0}, tmpl, parent,
                                        ecdsa_private_key_public(key),
                                        ecdsa_private_key_signer(parent_key), err);
    if (BURROW_FAILED(*err))
        return NULL;
    return x509_parse_certificate(a, der, err);
}

/* makeConstraintsCACert */
static X509Certificate *make_constraints_ca_cert(Alloc *a, const X509ConstraintsSpec *c,
                                                 Str name, const EcdsaPrivateKey *key,
                                                 const X509Certificate *parent,
                                                 const EcdsaPrivateKey *parent_key,
                                                 Error *err) {
    X509Certificate tmpl = {0};
    tmpl.serial_number = random_serial(a);
    tmpl.subject.common_name = name;
    tmpl.not_before = time_from_unix(1000, 0);
    tmpl.not_after = time_from_unix(2000, 0);
    tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    tmpl.extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);

    *err = add_constraints_to_template(a, c, &tmpl);
    if (BURROW_FAILED(*err))
        return NULL;
    return sign(a, &tmpl, parent, key, parent_key, err);
}

/* makeConstraintsLeafCert */
static X509Certificate *make_constraints_leaf_cert(Alloc *a, const X509LeafSpec *leaf,
                                                   const EcdsaPrivateKey *key,
                                                   const X509Certificate *parent,
                                                   const EcdsaPrivateKey *parent_key,
                                                   Error *err) {
    X509Certificate tmpl = {0};
    tmpl.serial_number = random_serial(a);
    tmpl.subject.organizational_unit = append_str(a, slice_nil(TYPE_STRING), "Leaf");
    tmpl.subject.common_name = str_from_cstr(leaf->cn != NULL ? leaf->cn : "");
    tmpl.not_before = time_from_unix(1000, 0);
    tmpl.not_after = time_from_unix(2000, 0);
    tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = false;
    tmpl.dns_names = tmpl.email_addresses = slice_nil(TYPE_STRING);
    tmpl.ip_addresses = slice_nil(TYPE_NET_IP);
    tmpl.uris = slice_nil(TYPE_X509_URL_PTR);
    tmpl.extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);

    Int nsans = names_len(leaf->sans);
    for (Int i = 0; i < nsans; i++) {
        const char *name = leaf->sans[i];
        if (has_prefix(name, "dns:")) {
            tmpl.dns_names = append_str(a, tmpl.dns_names, name + 4);
        } else if (has_prefix(name, "ip:")) {
            NetIP ip = net_parse_ip(a, str_from_cstr(name + 3));
            if (ip.p == NULL) {
                *err = fmt_errorf_v("cannot parse IP %q", name + 3);
                return NULL;
            }
            tmpl.ip_addresses = slice_append(a, tmpl.ip_addresses, &ip, 1);
        } else if (has_prefix(name, "invalidip:")) {
            Error herr = BURROW_NO_ERROR;
            NetIP ip = hex_decode_string(a, str_from_cstr(name + 10), &herr);
            if (BURROW_FAILED(herr)) {
                *err = fmt_errorf_v("cannot parse invalid IP: %s", herr);
                return NULL;
            }
            tmpl.ip_addresses = slice_append(a, tmpl.ip_addresses, &ip, 1);
        } else if (has_prefix(name, "email:")) {
            tmpl.email_addresses = append_str(a, tmpl.email_addresses, name + 6);
        } else if (has_prefix(name, "uri:")) {
            Error uerr = BURROW_NO_ERROR;
            Url *uri = url_parse(a, str_from_cstr(name + 4), &uerr);
            if (BURROW_FAILED(uerr)) {
                *err = fmt_errorf_v("cannot parse URI %q: %s", name + 4, uerr);
                return NULL;
            }
            tmpl.uris = slice_append(a, tmpl.uris, &uri, 1);
        } else if (has_prefix(name, "unknown:")) {
            /* This is a special case for testing unknown name types. A custom
             * SAN extension is injected into the certificate. */
            if (nsans != 1)
                panic_str(BURROW_S(
                    "when using unknown name types, it must be the sole name"));
            static const Byte value[] = {
                0x30, /* SEQUENCE */
                3,    /* three bytes */
                9,    /* undefined GeneralName type 9 */
                1,    1,
            };
            PkixExtension ext = {static_oid(oid_extension_subject_alt_name,
                                            LEN(oid_extension_subject_alt_name)),
                                 false, bytes_of(a, value, LEN(value))};
            tmpl.extra_extensions = slice_append(a, tmpl.extra_extensions, &ext, 1);
        } else {
            *err = fmt_errorf_v("unknown name type %q", name);
            return NULL;
        }
    }

    *err = parse_ekus(a, leaf->ekus, &tmpl.ext_key_usage, &tmpl.unknown_ext_key_usage);
    if (BURROW_FAILED(*err))
        return NULL;
    return sign(a, &tmpl, parent, key, parent_key, err);
}

/* ------------------------------------------------------------------- tests */

static Str numbered(Alloc *a, const char *prefix, Int n) {
    return fmt_sprintf_v(a, "%s%d", prefix, n);
}

static void constraint_case(void *env, TestingT *t) {
    Int i = (Int)(intptr_t)env;
    const X509NameConstraintsTest *test = &name_constraints_tests[i];
    ARENA(a);

    X509CertPool *root_pool = x509_new_cert_pool(a);
    EcdsaPrivateKey *root_key = ec_key(a);
    Str root_name = numbered(a, "Root ", i);

    /* At each level (root, intermediate(s), leaf), parent points to an
     * example parent certificate and parent_key the key for the parent level.
     * Since all certificates at a given level have the same name and public
     * key, any parent certificate is sufficient to get the correct issuer name
     * and authority key ID. */
    const X509Certificate *parent = NULL;
    const EcdsaPrivateKey *parent_key = root_key;
    Error err = BURROW_NO_ERROR;

    for (Int r = 0; r < test->nroots; r++) {
        X509Certificate *root_cert = make_constraints_ca_cert(
            a, &test->roots[r], root_name, root_key, NULL, root_key, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to create root: %s", err);
        parent = root_cert;
        x509_cert_pool_add_cert(root_pool, root_cert);
    }

    X509CertPool *intermediate_pool = x509_new_cert_pool(a);

    for (Int level = 0; level < test->nlevels; level++) {
        EcdsaPrivateKey *level_key = ec_key(a);
        Str level_name = numbered(a, "Intermediate level ", level);
        const X509Certificate *last = NULL;

        for (Int j = 0; j < test->level_len[level]; j++) {
            X509Certificate *ca_cert =
                make_constraints_ca_cert(a, &test->intermediates[level][j], level_name,
                                         level_key, parent, parent_key, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "failed to create %q: %s", level_name, err);
            last = ca_cert;
            x509_cert_pool_add_cert(intermediate_pool, ca_cert);
        }

        parent = last;
        parent_key = level_key;
    }

    EcdsaPrivateKey *leaf_key = ec_key(a);
    X509Certificate *leaf_cert =
        make_constraints_leaf_cert(a, &test->leaf, leaf_key, parent, parent_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "cannot create leaf: %s", err);

    X509VerifyOptions opts = {0};
    opts.roots = root_pool;
    opts.intermediates = intermediate_pool;
    opts.current_time = time_from_unix(1500, 0);
    opts.key_usages =
        slice_append(a, slice_nil(TYPE_INT), test->requested_ekus, test->nrequested);
    x509_certificate_verify(leaf_cert, a, opts, &err);

    if (test->expected_error == NULL) {
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "unexpected failure: %s", err);
    } else {
        if (BURROW_OK(err))
            testing_t_error_v(t, "unexpected success");
        else if (!strings_contains(error_text(err),
                                   str_from_cstr(test->expected_error)))
            testing_t_errorf_v(t, "expected error containing %q, but got: %s",
                               test->expected_error, err);
    }
    arena_free(&ar);
}

static void TestConstraintCases(TestingT *t) {
    for (Int i = 0; i < LEN(name_constraints_tests); i++)
        testing_t_run(t, str_from_cstr(name_constraints_tests[i].name),
                      BURROW_FN(TestingTFunc, constraint_case, (void *)(intptr_t)i));
}

/* The DER of a GeneralSubtree holding an iPAddress, b. */
static Slice ip_subtree(Alloc *a, const Byte *b, Int n) {
    Slice gn = slice_nil(TYPE_BYTE);
    gn = append_byte(a, gn, 0x87);
    gn = append_byte(a, gn, (Byte)n);
    gn = slice_append(a, gn, b, n);
    Slice out = slice_nil(TYPE_BYTE);
    out = append_byte(a, out, 0x30);
    out = append_byte(a, out, (Byte)gn.len);
    return slice_append(a, out, gn.p, gn.len);
}

static void TestNameConstraintIPNonZeroHostBits(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *root_key = ec_key(a);
    EcdsaPrivateKey *leaf_key = ec_key(a);

    /* Two excluded iPAddress subtrees: a lower-addressed range that takes the
     * binary-search neighbor slot, and one whose address has host bits set
     * (10.10.10.10/16, i.e. network 10.10.0.0/16). */
    static const Byte low[] = {10, 0, 0, 0, 255, 255, 255, 252};
    static const Byte high[] = {10, 10, 10, 10, 255, 255, 0, 0};
    Slice subtrees = ip_subtree(a, low, LEN(low));
    Slice s2 = ip_subtree(a, high, LEN(high));
    subtrees = slice_append(a, subtrees, s2.p, s2.len);
    Slice excluded = slice_nil(TYPE_BYTE);
    excluded = append_byte(a, excluded, 0xa1);
    excluded = append_byte(a, excluded, (Byte)subtrees.len);
    excluded = slice_append(a, excluded, subtrees.p, subtrees.len);
    Slice nc_value = slice_nil(TYPE_BYTE);
    nc_value = append_byte(a, nc_value, 0x30);
    nc_value = append_byte(a, nc_value, (Byte)excluded.len);
    nc_value = slice_append(a, nc_value, excluded.p, excluded.len);

    X509Certificate root_tmpl = {0};
    root_tmpl.serial_number = random_serial(a);
    root_tmpl.subject.common_name = BURROW_S("Root");
    root_tmpl.not_before = time_from_unix(1000, 0);
    root_tmpl.not_after = time_from_unix(2000, 0);
    root_tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
    root_tmpl.basic_constraints_valid = true;
    root_tmpl.is_ca = true;
    PkixExtension nc = {
        static_oid(oid_extension_name_constraints, LEN(oid_extension_name_constraints)),
        true, nc_value};
    root_tmpl.extra_extensions =
        slice_append(a, slice_nil(TYPE_PKIX_EXTENSION), &nc, 1);

    Error err = BURROW_NO_ERROR;
    X509Certificate *root = sign(a, &root_tmpl, NULL, root_key, root_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* The parsed range must keep the address as encoded, host bits and all. */
    if (root->excluded_ip_ranges.len != 2)
        testing_t_fatalf_v(t, "got %d excluded IP ranges, want 2",
                           root->excluded_ip_ranges.len);
    Byte want[] = {10, 10, 10, 10};
    NetIP got = ((NetIPNet *const *)root->excluded_ip_ranges.p)[1]->ip;
    if (!net_ip_equal(got, slice_from(want, 4, 4, TYPE_BYTE)))
        testing_t_errorf_v(t, "excluded range IP = %v, want 10.10.10.10",
                           net_ip_string(got, a));

    X509LeafSpec spec = {{"ip:10.10.0.1"}, {0}, NULL};
    X509Certificate *leaf =
        make_constraints_leaf_cert(a, &spec, leaf_key, root, root_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    X509CertPool *roots = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(roots, root);
    X509VerifyOptions opts = {0};
    opts.roots = roots;
    opts.current_time = time_from_unix(1500, 0);
    x509_certificate_verify(leaf, a, opts, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "leaf with IP SAN inside excluded range was accepted");
    else if (!strings_contains(error_text(err), BURROW_S("excluded by constraint")))
        testing_t_errorf_v(t, "got error %q, want excluded-by-constraint",
                           error_text(err));
    arena_free(&ar);
}

static void TestRFC2821Parsing(TestingT *t) {
    ARENA(a);
    for (Int i = 0; i < LEN(rfc2821_tests); i++) {
        const X509RFC2821Test *test = &rfc2821_tests[i];
        Str local = {0}, domain = {0};
        bool ok = burrow__x509_parse_rfc2821_mailbox(a, str_from_cstr(test->in), &local,
                                                     &domain);
        bool expected_failure = test->local_part[0] == 0 && test->domain[0] == 0;

        if (ok && expected_failure) {
            testing_t_errorf_v(t, "#%d: %q unexpectedly parsed as (%q, %q)", i,
                               test->in, local, domain);
            continue;
        }
        if (!ok && !expected_failure) {
            testing_t_errorf_v(t, "#%d: unexpected failure for %q", i, test->in);
            continue;
        }
        if (!ok)
            continue;
        if (!str_eq(local, str_from_cstr(test->local_part)) ||
            !str_eq(domain, str_from_cstr(test->domain)))
            testing_t_errorf_v(t, "#%d: %q parsed as (%q, %q), but wanted (%q, %q)", i,
                               test->in, local, domain, test->local_part, test->domain);
    }
    arena_free(&ar);
}

static bool constraint_parse_error(Error err) {
    Str s = error_text(err);
    return strings_contains(s, BURROW_S("failed to parse ")) &&
           strings_contains(s, BURROW_S("constraint"));
}

static bool encoding_error(Error err) {
    return strings_contains(error_text(err),
                            BURROW_S("cannot be encoded as an IA5String"));
}

static void TestBadNamesInConstraints(TestingT *t) {
    /* Bad names in constraints should not parse. The hyphens in the last
     * three are U+2013, an en dash, in UTF-8. */
    static const struct {
        const char *name;
        bool (*matcher)(Error);
    } bad_names[] = {
        {"dns:foo.com.", constraint_parse_error},
        {"email:abc@foo.com.", constraint_parse_error},
        {"email:foo.com.", constraint_parse_error},
        {"uri:example.com.", constraint_parse_error},
        {"uri:1.2.3.4", constraint_parse_error},
        {"uri:ffff::1", constraint_parse_error},
        {"dns:not\342\200\223hyphen.com", encoding_error},
        {"email:foo@not\342\200\223hyphen.com", encoding_error},
        {"uri:not\342\200\223hyphen.com", encoding_error},
    };

    ARENA(a);
    EcdsaPrivateKey *priv = ec_key(a);

    for (Int i = 0; i < LEN(bad_names); i++) {
        X509ConstraintsSpec spec = {{bad_names[i].name}, {0}, {0}};
        Error err = BURROW_NO_ERROR;
        make_constraints_ca_cert(a, &spec, BURROW_S("TestAbsoluteNamesInConstraints"),
                                 priv, NULL, priv, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t,
                               "bad name %q unexpectedly accepted in name constraint",
                               bad_names[i].name);
            continue;
        }
        if (!bad_names[i].matcher(err))
            testing_t_errorf_v(t, "bad name %q triggered unrecognised error: %s",
                               bad_names[i].name, err);
    }
    arena_free(&ar);
}

static void TestBadNamesInSANs(TestingT *t) {
    /* Bad names in URI and IP SANs should not parse. Bad DNS and email SANs
     * will parse and are tested in name constraint tests at the top of this
     * file. */
    static const char *const bad_names[] = {
        "uri:https://example.com./dsf",
        "invalidip:0102",
        "invalidip:0102030405",
    };

    ARENA(a);
    EcdsaPrivateKey *priv = ec_key(a);

    for (Int i = 0; i < LEN(bad_names); i++) {
        X509LeafSpec spec = {{bad_names[i]}, {0}, NULL};
        Error err = BURROW_NO_ERROR;
        make_constraints_leaf_cert(a, &spec, priv, NULL, priv, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "bad name %q unexpectedly accepted in SAN",
                               bad_names[i]);
            continue;
        }
        Str str = error_text(err);
        if (!strings_contains(str, BURROW_S("cannot parse ")))
            testing_t_errorf_v(t, "bad name %q triggered unrecognised error: %s",
                               bad_names[i], str);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestConstraintCases)                                                             \
    X(TestNameConstraintIPNonZeroHostBits)                                             \
    X(TestRFC2821Parsing)                                                              \
    X(TestBadNamesInConstraints)                                                       \
    X(TestBadNamesInSANs)

TESTING_MAIN(TESTS)
