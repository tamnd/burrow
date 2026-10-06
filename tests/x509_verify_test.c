/* crypto/x509 tests for Verify, VerifyHostname and the checks behind them:
 * Go's verify_test.go.
 *
 * Go's table driven tests are loops here rather than subtests, with the name
 * of the case in each message. generatePEMCertWithRepeatSAN signs with a new
 * ECDSA P-256 key where Go makes a 4096 bit RSA one; the tests only look at
 * the names in it. TestSystemVerify only runs on Windows in Go, and
 * TestIssue51759 only on macOS, both to test the platform verifier, which is
 * not here yet, so they are left out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/crypto.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/panic.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_verify_test_gen.h"

#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

#define ARENA(a)                                                                       \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice cbytes(const char *s) {
    Int n = (Int)strlen(s);
    return (Slice){(void *)(uintptr_t)s, n, n, TYPE_BYTE};
}

static Slice strs_of(Alloc *a, const char *const *v, Int n) {
    Slice s = slice_nil(TYPE_STRING);
    for (Int i = 0; i < n; i++) {
        Str x = str_from_cstr(v[i]);
        s = slice_append(a, s, &x, 1);
    }
    return s;
}

#define STRS(a, ...)                                                                   \
    strs_of((a), (const char *const[]){__VA_ARGS__},                                   \
            (Int)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)))

static Slice ekus(Alloc *a, const X509ExtKeyUsage *v, Int n) {
    return slice_append(a, slice_nil(TYPE_INT), v, n);
}

#define EKUS(a, ...)                                                                   \
    ekus((a), (const X509ExtKeyUsage[]){__VA_ARGS__},                                  \
         (Int)(sizeof((const X509ExtKeyUsage[]){__VA_ARGS__}) /                        \
               sizeof(X509ExtKeyUsage)))

static const X509Certificate *chain_at(Slice chain, Int i) {
    return ((const X509Certificate *const *)chain.p)[i];
}

static Slice chains_at(Slice chains, Int i) {
    return ((const Slice *)chains.p)[i];
}

static EcdsaPrivateKey *ec_key(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_verify_test: ECDSA key generation failed"));
    return k;
}

/* certificateFromPEM */
static X509Certificate *certificate_from_pem(Alloc *a, const char *pem_bytes,
                                             Error *err) {
    Slice rest;
    PemBlock *block = pem_decode(a, cbytes(pem_bytes), &rest);
    if (block == NULL) {
        *err = errors_new(a, BURROW_S("failed to decode PEM"));
        return NULL;
    }
    return x509_parse_certificate(a, block->bytes, err);
}

static X509Certificate *must_cert(Alloc *a, const char *pem_bytes) {
    Error err = BURROW_NO_ERROR;
    X509Certificate *c = certificate_from_pem(a, pem_bytes, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_verify_test: bad test certificate"));
    return c;
}

/* generatePEMCertWithRepeatSAN, as a C string from a. */
static const char *generate_pem_cert_with_repeat_san(Alloc *a, int64_t current_time,
                                                     Int count, const char *san) {
    X509Certificate cert = {0};
    cert.not_before = time_from_unix(current_time, 0);
    cert.not_after = time_from_unix(current_time, 0);
    NetIP ip = net_parse_ip(a, str_from_cstr(san));
    if (ip.len > 0) {
        cert.ip_addresses = slice_nil(TYPE_NET_IP);
        for (Int i = 0; i < count; i++)
            cert.ip_addresses = slice_append(a, cert.ip_addresses, &ip, 1);
    } else {
        Str s = str_from_cstr(san);
        cert.dns_names = slice_nil(TYPE_STRING);
        for (Int i = 0; i < count; i++)
            cert.dns_names = slice_append(a, cert.dns_names, &s, 1);
    }
    EcdsaPrivateKey *k = ec_key(a);
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, &cert, &cert,
                                        ecdsa_private_key_public(k),
                                        ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_verify_test: CreateCertificate failed"));
    PemBlock b = {BURROW_S("CERTIFICATE"), NULL, der, 0};
    Slice out = pem_encode_to_memory(a, &b);
    Byte nul = 0;
    out = slice_append(a, out, &nul, 1);
    return out.p;
}

static bool verify_cert_set(const X509VerifyCert *c) {
    return c->pem != NULL || c->san != NULL;
}

static const char *verify_cert_pem(Alloc *a, const X509VerifyCert *c) {
    if (c->pem != NULL)
        return c->pem;
    return generate_pem_cert_with_repeat_san(a, c->at, c->repeat, c->san);
}

/* nameToKey */
static Str name_to_key(Alloc *a, const PkixName *name) {
    Str parts[4] = {
        strings_join(a, name->country, BURROW_S(",")),
        strings_join(a, name->organization, BURROW_S(",")),
        strings_join(a, name->organizational_unit, BURROW_S(",")),
        name->common_name,
    };
    return strings_join(a, (Slice){parts, 4, 4, TYPE_STRING}, BURROW_S("/"));
}

/* chainToDebugString */
static Str chain_to_debug_string(Alloc *a, Slice chain) {
    Slice names = slice_nil(TYPE_STRING);
    for (Int i = 0; i < chain.len; i++) {
        Str key = name_to_key(a, &chain_at(chain, i)->subject);
        names = slice_append(a, names, &key, 1);
    }
    return strings_join(a, names, BURROW_S(" -> "));
}

/* The length of an expected chain, which ends at the first NULL. */
static Int expected_len(const char *const *chain, Int max) {
    Int n = 0;
    while (n < max && chain[n] != NULL)
        n++;
    return n;
}

static bool does_match(Alloc *a, const char *const *expected, Int n, Slice chain) {
    if (chain.len != n)
        return false;
    for (Int k = 0; k < n; k++)
        if (!strings_contains(name_to_key(a, &chain_at(chain, k)->subject),
                              str_from_cstr(expected[k])))
            return false;
    return true;
}

static bool invalid_reason_is(Error err, X509InvalidReason reason) {
    const X509CertificateInvalidError *e =
        errors_as(err, TYPE_X509_CERTIFICATE_INVALID_ERROR);
    return e != NULL && e->reason == reason;
}

/* The errorCallback of test, given err. */
static void check_expected_error(TestingT *t, const X509VerifyCase *test, Error err) {
    const char *name = test->name;
    switch (test->expect) {
    case X509_EXPECT_HOSTNAME:
        if (errors_as(err, TYPE_X509_HOSTNAME_ERROR) == NULL)
            testing_t_errorf_v(t, "%s: error was not a HostnameError: %v", name, err);
        else if (!strings_contains(error_text(err), str_from_cstr(test->expect_msg)))
            testing_t_errorf_v(t, "%s: HostnameError did not contain %q: %v", name,
                               test->expect_msg, err);
        break;
    case X509_EXPECT_EXPIRED:
        if (!invalid_reason_is(err, X509_EXPIRED))
            testing_t_errorf_v(t, "%s: error was not Expired: %v", name, err);
        break;
    case X509_EXPECT_USAGE:
        if (!invalid_reason_is(err, X509_INCOMPATIBLE_USAGE))
            testing_t_errorf_v(t, "%s: error was not IncompatibleUsage: %v", name, err);
        break;
    case X509_EXPECT_AUTHORITY_UNKNOWN: {
        const X509UnknownAuthorityError *e =
            errors_as(err, TYPE_X509_UNKNOWN_AUTHORITY_ERROR);
        if (e == NULL)
            testing_t_errorf_v(t, "%s: error was not UnknownAuthorityError: %v", name,
                               err);
        else if (e->cert == NULL)
            testing_t_errorf_v(
                t, "%s: error was UnknownAuthorityError, but missing Cert: %v", name,
                err);
        break;
    }
    case X509_EXPECT_HASH:
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: no error resulted from invalid hash", name);
        else if (!strings_contains(error_text(err),
                                   BURROW_S("algorithm unimplemented")))
            testing_t_errorf_v(t,
                               "%s: error resulting from invalid hash didn't contain "
                               "'%s', rather it was: %v",
                               name, "algorithm unimplemented", err);
        break;
    case X509_EXPECT_NAME_CONSTRAINTS:
        if (!invalid_reason_is(err, X509_CA_NOT_AUTHORIZED_FOR_THIS_NAME))
            testing_t_errorf_v(t, "%s: error was not a CANotAuthorizedForThisName: %v",
                               name, err);
        break;
    case X509_EXPECT_NOT_AUTHORIZED:
        if (!invalid_reason_is(err, X509_NOT_AUTHORIZED_TO_SIGN))
            testing_t_errorf_v(t, "%s: error was not a NotAuthorizedToSign: %v", name,
                               err);
        break;
    case X509_EXPECT_UNHANDLED_CRITICAL_EXTENSION:
        if (errors_as(err, TYPE_X509_UNHANDLED_CRITICAL_EXTENSION) == NULL)
            testing_t_errorf_v(t, "%s: error was not an UnhandledCriticalExtension: %v",
                               name, err);
        break;
    default:
        break;
    }
}

/* testVerify, with useSystemRoots false. */
static void test_verify(TestingT *t, Alloc *a, const X509VerifyCase *test) {
    const char *name = test->name;
    X509CertPool *intermediates = x509_new_cert_pool(a);
    X509CertPool *roots = x509_new_cert_pool(a);
    X509VerifyOptions opts = {0};
    opts.intermediates = intermediates;
    opts.roots = roots;
    opts.dns_name = str_from_cstr(test->dns_name);
    opts.current_time = time_from_unix(test->current_time, 0);
    opts.key_usages = ekus(a, test->key_usages, test->n_key_usages);

    for (Int j = 0; j < LEN(test->roots) && verify_cert_set(&test->roots[j]); j++)
        if (!x509_cert_pool_append_certs_from_pem(
                roots, cbytes(verify_cert_pem(a, &test->roots[j])))) {
            testing_t_errorf_v(t, "%s: failed to parse root #%d", name, j);
            return;
        }
    for (Int j = 0;
         j < LEN(test->intermediates) && verify_cert_set(&test->intermediates[j]); j++)
        if (!x509_cert_pool_append_certs_from_pem(
                intermediates, cbytes(verify_cert_pem(a, &test->intermediates[j])))) {
            testing_t_errorf_v(t, "%s: failed to parse intermediate #%d", name, j);
            return;
        }

    Error err = BURROW_NO_ERROR;
    X509Certificate *leaf =
        certificate_from_pem(a, verify_cert_pem(a, &test->leaf), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: failed to parse leaf: %v", name, err);
        return;
    }

    Slice chains = x509_certificate_verify(leaf, a, opts, &err);

    if (test->expect == X509_EXPECT_NOTHING && BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: unexpected error: %v", name, err);
        return;
    }
    if (test->expect != X509_EXPECT_NOTHING)
        check_expected_error(t, test, err);

    /* Every expected chain should match one (or more) returned chain. */
    Int n_expected = 0;
    while (n_expected < LEN(test->chains) && test->chains[n_expected][0] != NULL)
        n_expected++;
    for (Int e = 0; e < n_expected; e++) {
        Int n = expected_len(test->chains[e], LEN(test->chains[e]));
        bool match = false;
        for (Int c = 0; c < chains.len && !match; c++)
            match = does_match(a, test->chains[e], n, chains_at(chains, c));
        if (!match)
            testing_t_errorf_v(t, "%s: No match found for chain %d", name, e);
    }

    /* Every returned chain should match 1 expected chain. */
    for (Int c = 0; c < chains.len; c++) {
        Int n_matched = 0;
        for (Int e = 0; e < n_expected; e++)
            if (does_match(a, test->chains[e],
                           expected_len(test->chains[e], LEN(test->chains[e])),
                           chains_at(chains, c)))
                n_matched++;
        if ((n_matched == 0 && !test->system_lax) || n_matched > 1)
            testing_t_errorf_v(t, "%s: Got %d matches for chain %s", name, n_matched,
                               chain_to_debug_string(a, chains_at(chains, c)));
    }
}

/* ------------------------------------------------------------------- tests */

static void TestGoVerify(TestingT *t) {
    for (Int i = 0; i < LEN(verify_tests); i++) {
        ARENA(a);
        test_verify(t, a, &verify_tests[i]);
        arena_free(&ar);
    }
}

static void TestUnknownAuthorityError(TestingT *t) {
    ARENA(a);
    for (Int i = 0; i < LEN(unknown_authority_error_tests); i++) {
        const X509UnknownAuthorityCase *tt = &unknown_authority_error_tests[i];
        Slice rest;
        PemBlock *der = pem_decode(a, cbytes(tt->cert), &rest);
        if (der == NULL) {
            testing_t_errorf_v(t, "#%d: Unable to decode PEM block", i);
            continue;
        }
        Error err = BURROW_NO_ERROR;
        X509Certificate *c = x509_parse_certificate(a, der->bytes, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: Unable to parse certificate -> %v", i, err);
            continue;
        }
        Str actual = burrow__x509_unknown_authority_message(
            a, errors_new(a, BURROW_S("empty")), c);
        if (!str_eq(actual, str_from_cstr(tt->expected)))
            testing_t_errorf_v(t,
                               "#%d: UnknownAuthorityError.Error() response invalid "
                               "actual: %s expected: %s",
                               i, actual, tt->expected);
    }
    arena_free(&ar);
}

static void TestValidHostname(TestingT *t) {
    static const struct {
        const char *host;
        bool valid_input, valid_pattern;
    } tests[] = {
        {"example.com", true, true},
        {"eXample123-.com", true, true},
        {"-eXample123-.com", false, false},
        {"", false, false},
        {".", false, false},
        {"example..com", false, false},
        {".example.com", false, false},
        {"example.com.", true, false},
        {"*.example.com.", false, false},
        {"*.example.com", false, true},
        {"*foo.example.com", false, false},
        {"foo.*.example.com", false, false},
        {"exa_mple.com", true, true},
        {"foo,bar", false, false},
        {"project-dev:us-central1:main", false, false},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Str host = str_from_cstr(tests[i].host);
        bool got = burrow__x509_valid_hostname_pattern(host);
        if (got != tests[i].valid_pattern)
            testing_t_errorf_v(t, "validHostnamePattern(%q) = %t, want %t",
                               tests[i].host, got, tests[i].valid_pattern);
        got = burrow__x509_valid_hostname_input(host);
        if (got != tests[i].valid_input)
            testing_t_errorf_v(t, "validHostnameInput(%q) = %t, want %t", tests[i].host,
                               got, tests[i].valid_input);
    }
}

/* generateCert: a certificate for cn, signed by issuer with issuer_key or
 * self-signed when issuer is NULL, for priv or a new key when priv is NULL.
 * *key is the key it is for. */
static X509Certificate *generate_cert(Alloc *a, const char *cn, bool is_ca,
                                      const X509Certificate *issuer,
                                      EcdsaPrivateKey *issuer_key,
                                      EcdsaPrivateKey *priv, EcdsaPrivateKey **key,
                                      Error *err) {
    if (priv == NULL)
        priv = ec_key(a);

    BigInt *limit = big_int_lsh(big_new_int(a, 0), big_new_int(a, 1), 128);
    X509Certificate tmpl = {0};
    tmpl.serial_number = crypto_rand_int(a, (IoReader){0}, limit, err);
    if (BURROW_FAILED(*err))
        return NULL;
    tmpl.subject.common_name = str_from_cstr(cn);
    tmpl.not_before = time_add(time_now(), -1 * TIME_HOUR);
    tmpl.not_after = time_add(time_now(), 24 * TIME_HOUR);
    Str name = crypto_rand_text(a);
    tmpl.dns_names = slice_append(a, slice_nil(TYPE_STRING), &name, 1);
    tmpl.key_usage = X509_KEY_USAGE_KEY_ENCIPHERMENT |
                     X509_KEY_USAGE_DIGITAL_SIGNATURE | X509_KEY_USAGE_CERT_SIGN;
    tmpl.ext_key_usage = EKUS(a, X509_EXT_KEY_USAGE_SERVER_AUTH);
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = is_ca;
    if (issuer == NULL) {
        issuer = &tmpl;
        issuer_key = priv;
    }

    Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, issuer,
                                        ecdsa_private_key_public(priv),
                                        ecdsa_private_key_signer(issuer_key), err);
    if (BURROW_FAILED(*err))
        return NULL;
    X509Certificate *cert = x509_parse_certificate(a, der, err);
    if (BURROW_FAILED(*err))
        return NULL;
    *key = priv;
    return cert;
}

static void TestPathologicalChains(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(
            t, "skipping generation of a long chains of certificates in short mode");

    /* Four pathological cases, where the intermediates in the chain have the
     * same or different subjects and the same or different keys. There is no
     * root in the pool, so no chain can be built, and Verify has to give up
     * at its limit on signature checks. */
    static const struct {
        bool same_subject, same_key;
    } tests[] = {{false, false}, {true, false}, {false, true}, {true, true}};
    for (Int i = 0; i < LEN(tests); i++) {
        ARENA(a);
        X509CertPool *intermediates = x509_new_cert_pool(a);
        EcdsaPrivateKey *intermediate_key = tests[i].same_key ? ec_key(a) : NULL;

        EcdsaPrivateKey *leaf_signer = NULL;
        X509Certificate *intermediate = NULL;
        Error err = BURROW_NO_ERROR;
        for (Int j = 0; j < 100; j++) {
            Str cn = BURROW_S("Intermediate CA");
            if (!tests[i].same_subject)
                cn = fmt_sprintf_v(a, "%s #%d", cn, j);
            intermediate =
                generate_cert(a, str_to_cstr(a, cn), true, intermediate, leaf_signer,
                              intermediate_key, &leaf_signer, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%v", err);
            x509_cert_pool_add_cert(intermediates, intermediate);
        }

        EcdsaPrivateKey *leaf_key = NULL;
        X509Certificate *leaf = generate_cert(a, "Leaf", false, intermediate,
                                              leaf_signer, NULL, &leaf_key, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%v", err);

        Time start = time_now();
        X509VerifyOptions opts = {0};
        opts.roots = x509_new_cert_pool(a);
        opts.intermediates = intermediates;
        x509_certificate_verify(leaf, a, opts, &err);
        testing_t_logf_v(t, "sameSubject=%t,sameKey=%t: verification took %s",
                         tests[i].same_subject, tests[i].same_key,
                         duration_string(time_since(start), a));
        arena_free(&ar);
    }
}

static void TestSystemRootsError(TestingT *t) {
#if defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    testing_t_skip_v(t, "Windows and darwin do not use (or support) systemRoots");
#else
    ARENA(a);
    X509VerifyOptions opts = {0};
    X509CertPool *intermediates = x509_new_cert_pool(a);
    opts.intermediates = intermediates;
    opts.dns_name = BURROW_S("www.google.com");
    opts.current_time = time_from_unix(1677615892, 0);

    if (!x509_cert_pool_append_certs_from_pem(intermediates, cbytes(gts_intermediate)))
        testing_t_fatalf_v(t, "failed to parse intermediate");

    Error err = BURROW_NO_ERROR;
    X509Certificate *leaf = certificate_from_pem(a, google_leaf, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse leaf: %v", err);

    X509CertPool *old_system_roots = burrow__x509_swap_system_roots(NULL);
    x509_certificate_verify(leaf, a, opts, &err);
    burrow__x509_swap_system_roots(old_system_roots);
    if (errors_as(err, TYPE_X509_SYSTEM_ROOTS_ERROR) == NULL)
        testing_t_errorf_v(t, "error was not SystemRootsError: %v", err);
    arena_free(&ar);
#endif
}

static void TestSystemRootsErrorUnwrap(TestingT *t) {
    ARENA(a);
    Error err1 = errors_new(a, BURROW_S("err1"));
    X509SystemRootsError e = {err1};
    Error err = x509_system_roots_error_as_error(e, a);
    if (!errors_is(err, err1))
        testing_t_error_v(t, "errors.Is failed, wanted success");
    arena_free(&ar);
}

/* ------------------------------------------------------------ path building */

enum { LEAF_CERTIFICATE, INTERMEDIATE_CERTIFICATE, ROOT_CERTIFICATE };

typedef void (*MutateTemplate)(Alloc *a, X509Certificate *c);

typedef struct TrustGraphEdge {
    const char *issuer;
    const char *subject;
    int type;
    MutateTemplate mutate_template;
} TrustGraphEdge;

typedef struct RootDescription {
    const char *subject;
    MutateTemplate mutate_template;
    /* A Constraint that turns down any chain with a certificate for this
     * common name in it. */
    const char *reject_cn;
} RootDescription;

typedef struct TrustGraphDescription {
    RootDescription roots[1];
    const char *leaf;
    TrustGraphEdge graph[8];
} TrustGraphDescription;

/* genCertEdge */
static X509Certificate *gen_cert_edge(TestingT *t, Alloc *a, const char *subject,
                                      EcdsaPrivateKey *key, MutateTemplate mutate_tmpl,
                                      int cert_type, const X509Certificate *issuer,
                                      EcdsaPrivateKey *signer) {
    Error err = BURROW_NO_ERROR;
    X509Certificate tmpl = {0};
    tmpl.serial_number = crypto_rand_int(a, (IoReader){0}, big_new_int(a, 100), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test serial: %v", err);
    tmpl.subject.common_name = str_from_cstr(subject);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    if (cert_type == ROOT_CERTIFICATE || cert_type == INTERMEDIATE_CERTIFICATE) {
        tmpl.is_ca = true;
        tmpl.basic_constraints_valid = true;
        tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
    } else if (cert_type == LEAF_CERTIFICATE) {
        tmpl.dns_names = STRS(a, "localhost");
    }
    if (mutate_tmpl != NULL)
        mutate_tmpl(a, &tmpl);

    if (cert_type == ROOT_CERTIFICATE) {
        issuer = &tmpl;
        signer = key;
    }

    Slice d = x509_create_certificate(a, (IoReader){0}, &tmpl, issuer,
                                      ecdsa_private_key_public(key),
                                      ecdsa_private_key_signer(signer), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test cert: %v", err);
    X509Certificate *c = x509_parse_certificate(a, d, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse test cert: %v", err);
    return c;
}

static Error reject_cn(void *env, Slice chain) {
    Str cn = str_from_cstr(env);
    for (Int i = 0; i < chain.len; i++)
        if (str_eq(chain_at(chain, i)->subject.common_name, cn))
            return errors_new(error_allocator(), BURROW_S("bad"));
    return BURROW_NO_ERROR;
}

typedef struct TrustGraph {
    const char *names[8];
    X509Certificate *certs[8];
    EcdsaPrivateKey *keys[8];
    Int n;
} TrustGraph;

static Int graph_find(const TrustGraph *g, const char *name) {
    for (Int i = 0; i < g->n; i++)
        if (strcmp(g->names[i], name) == 0)
            return i;
    return -1;
}

/* buildTrustGraph */
static X509Certificate *build_trust_graph(TestingT *t, Alloc *a,
                                          const TrustGraphDescription *d,
                                          X509CertPool *root_pool,
                                          X509CertPool *intermediate_pool) {
    TrustGraph g = {0};
    for (Int i = 0; i < LEN(d->roots) && d->roots[i].subject != NULL; i++) {
        const RootDescription *r = &d->roots[i];
        EcdsaPrivateKey *k = ec_key(a);
        X509Certificate *root = gen_cert_edge(t, a, r->subject, k, r->mutate_template,
                                              ROOT_CERTIFICATE, NULL, NULL);
        if (r->reject_cn != NULL)
            x509_cert_pool_add_cert_with_constraint(
                root_pool, root,
                BURROW_FN(X509CertConstraint, reject_cn, (uintptr_t)r->reject_cn));
        else
            x509_cert_pool_add_cert(root_pool, root);
        g.names[g.n] = r->subject;
        g.certs[g.n] = root;
        g.keys[g.n] = k;
        g.n++;
    }

    X509Certificate *leaf = NULL;
    for (Int i = 0; i < LEN(d->graph) && d->graph[i].issuer != NULL; i++) {
        const TrustGraphEdge *e = &d->graph[i];
        Int issuer = graph_find(&g, e->issuer);
        if (issuer < 0)
            testing_t_fatalf_v(t, "unknown issuer %s", e->issuer);
        Int subject = graph_find(&g, e->subject);
        if (subject < 0) {
            subject = g.n++;
            g.names[subject] = e->subject;
            g.keys[subject] = ec_key(a);
        }
        X509Certificate *cert =
            gen_cert_edge(t, a, e->subject, g.keys[subject], e->mutate_template,
                          e->type, g.certs[issuer], g.keys[issuer]);
        g.certs[subject] = cert;
        if (strcmp(e->subject, d->leaf) == 0)
            leaf = cert;
        else
            x509_cert_pool_add_cert(intermediate_pool, cert);
    }
    return leaf;
}

/* chainsToStrings */
static Slice chains_to_strings(Alloc *a, Slice chains) {
    Slice chain_strings = slice_nil(TYPE_STRING);
    for (Int i = 0; i < chains.len; i++) {
        Slice chain = chains_at(chains, i);
        Slice names = slice_nil(TYPE_STRING);
        for (Int j = 0; j < chain.len; j++) {
            Str name = pkix_name_string(chain_at(chain, j)->subject, a);
            names = slice_append(a, names, &name, 1);
        }
        Str s = strings_join(a, names, BURROW_S(" -> "));
        chain_strings = slice_append(a, chain_strings, &s, 1);
    }
    sort_strings(chain_strings);
    return chain_strings;
}

static void code_signing_eku(Alloc *a, X509Certificate *c) {
    c->ext_key_usage = EKUS(a, X509_EXT_KEY_USAGE_CODE_SIGNING);
}

static void server_auth_eku(Alloc *a, X509Certificate *c) {
    c->ext_key_usage = EKUS(a, X509_EXT_KEY_USAGE_SERVER_AUTH);
}

static void good_constraint_bad_san(Alloc *a, X509Certificate *c) {
    c->permitted_dns_domains = STRS(a, "good");
    c->dns_names = STRS(a, "bad");
}

static void localhost_san(Alloc *a, X509Certificate *c) {
    c->dns_names = STRS(a, "localhost");
}

static void permit_example_com(Alloc *a, X509Certificate *c) {
    c->permitted_dns_domains = STRS(a, "example.com");
}

static void beep_com_san(Alloc *a, X509Certificate *c) {
    c->dns_names = STRS(a, "beep.com");
}

static void www_example_com_san(Alloc *a, X509Certificate *c) {
    c->dns_names = STRS(a, "www.example.com");
}

static void beep_com_san_permit_example_com(Alloc *a, X509Certificate *c) {
    c->dns_names = STRS(a, "beep.com");
    c->permitted_dns_domains = STRS(a, "example.com");
}

#define I INTERMEDIATE_CERTIFICATE
#define L LEAF_CERTIFICATE

static void TestPathBuilding(TestingT *t) {
    static const struct {
        const char *name;
        TrustGraphDescription graph;
        const char *expected_chains[5];
        const char *expected_err;
    } tests[] = {
        {
            /* RFC 4158, figure 7, with an unsupported EKU (code signing) on
             * C->B, which rules out the path Trust Anchor -> C -> B -> EE. */
            "bad EKU",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"root", "inter c", I, NULL},
              {"inter c", "inter a", I, NULL},
              {"inter a", "inter c", I, NULL},
              {"inter c", "inter b", I, code_signing_eku},
              {"inter a", "inter b", I, NULL},
              {"inter b", "leaf", L, NULL}}},
            {"CN=leaf -> CN=inter b -> CN=inter a -> CN=inter c -> CN=root",
             "CN=leaf -> CN=inter b -> CN=inter a -> CN=root"},
            NULL,
        },
        {
            /* The same graph with an unconstrained SAN on C->B, which rules
             * out the same path. Go names this one "bad EKU" too. */
            "bad EKU",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"root", "inter c", I, NULL},
              {"inter c", "inter a", I, NULL},
              {"inter a", "inter c", I, NULL},
              {"inter c", "inter b", I, good_constraint_bad_san},
              {"inter a", "inter b", I, NULL},
              {"inter b", "leaf", L, NULL}}},
            {"CN=leaf -> CN=inter b -> CN=inter a -> CN=inter c -> CN=root",
             "CN=leaf -> CN=inter b -> CN=inter a -> CN=root"},
            NULL,
        },
        {
            /* Both paths: Trust Anchor -> A -> C -> EE and
             * Trust Anchor -> A -> B -> C -> EE. */
            "all paths",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"inter a", "inter b", I, NULL},
              {"inter a", "inter c", I, NULL},
              {"inter b", "inter c", I, NULL},
              {"inter c", "leaf", L, NULL}}},
            {"CN=leaf -> CN=inter c -> CN=inter a -> CN=root",
             "CN=leaf -> CN=inter c -> CN=inter b -> CN=inter a -> CN=root"},
            NULL,
        },
        {
            /* A and C cross sign each other, and the paths that go round
             * that loop are left out. */
            "ignore cross-sig loops",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"root", "inter c", I, NULL},
              {"inter c", "inter a", I, NULL},
              {"inter a", "inter c", I, NULL},
              {"inter c", "inter b", I, NULL},
              {"inter a", "inter b", I, NULL},
              {"inter b", "leaf", L, NULL}}},
            {"CN=leaf -> CN=inter b -> CN=inter a -> CN=inter c -> CN=root",
             "CN=leaf -> CN=inter b -> CN=inter a -> CN=root",
             "CN=leaf -> CN=inter b -> CN=inter c -> CN=inter a -> CN=root",
             "CN=leaf -> CN=inter b -> CN=inter c -> CN=root"},
            NULL,
        },
        {
            /* A two node graph where the leaf is issued by the root and has
             * the same subject and key, but has a SAN. */
            "leaf with same subject, key, as parent but with SAN",
            {{{"root", NULL, NULL}}, "root", {{"root", "root", L, localhost_san}}},
            {"CN=root -> CN=root"},
            NULL,
        },
        {
            /* An EKU on C->B that the leaf does not have leaves one path. */
            "ignore invalid EKU path",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"root", "inter c", I, NULL},
              {"inter c", "inter b", I, code_signing_eku},
              {"inter a", "inter b", I, server_auth_eku},
              {"inter b", "leaf", L, server_auth_eku}}},
            {"CN=leaf -> CN=inter b -> CN=inter a -> CN=root"},
            NULL,
        },
        {
            /* A name constraint on the root that the intermediate's SAN
             * breaks. */
            "constrained root, invalid intermediate",
            {{{"root", permit_example_com, NULL}},
             "leaf",
             {{"root", "inter", I, beep_com_san},
              {"inter", "leaf", L, www_example_com_san}}},
            {NULL},
            "x509: a root or intermediate certificate is not authorized to sign for "
            "this name: DNS name \"beep.com\" is not permitted by any constraint",
        },
        {
            /* A name constraint on the intermediate that its own SAN breaks,
             * which only counts for the certificates under it. */
            "constrained intermediate, non-matching SAN",
            {{{"root", NULL, NULL}},
             "leaf",
             {{"root", "inter", I, beep_com_san_permit_example_com},
              {"inter", "leaf", L, www_example_com_san}}},
            {"CN=leaf -> CN=inter -> CN=root"},
            NULL,
        },
        {
            /* A code constraint on the root that turns down the path through
             * inter a. */
            "code constrained root, two paths, one valid",
            {{{"root", NULL, "inter a"}},
             "leaf",
             {{"root", "inter a", I, NULL},
              {"root", "inter b", I, NULL},
              {"inter a", "inter c", I, NULL},
              {"inter b", "inter c", I, NULL},
              {"inter c", "leaf", L, NULL}}},
            {"CN=leaf -> CN=inter c -> CN=inter b -> CN=root"},
            NULL,
        },
        {
            /* A code constraint on the root that turns down the only path. */
            "code constrained root, one invalid path",
            {{{"root", NULL, "leaf"}},
             "leaf",
             {{"root", "inter", I, NULL}, {"inter", "leaf", L, NULL}}},
            {NULL},
            "x509: certificate signed by unknown authority (possibly because of "
            "\"bad\" while trying to verify candidate authority certificate "
            "\"root\")",
        },
    };

    for (Int i = 0; i < LEN(tests); i++) {
        ARENA(a);
        X509CertPool *roots = x509_new_cert_pool(a);
        X509CertPool *intermediates = x509_new_cert_pool(a);
        X509Certificate *leaf =
            build_trust_graph(t, a, &tests[i].graph, roots, intermediates);
        X509VerifyOptions opts = {0};
        opts.roots = roots;
        opts.intermediates = intermediates;
        Error err = BURROW_NO_ERROR;
        Slice chains = x509_certificate_verify(leaf, a, opts, &err);
        if (BURROW_FAILED(err) &&
            (tests[i].expected_err == NULL ||
             !str_eq(error_text(err), str_from_cstr(tests[i].expected_err)))) {
            testing_t_errorf_v(t, "%s: unexpected error: got %q, want %q",
                               tests[i].name, error_text(err),
                               tests[i].expected_err == NULL ? ""
                                                             : tests[i].expected_err);
            arena_free(&ar);
            continue;
        }
        Int n_expected =
            expected_len(tests[i].expected_chains, LEN(tests[i].expected_chains));
        if (n_expected > 0) {
            Slice got = chains_to_strings(a, chains);
            Slice want = strs_of(a, tests[i].expected_chains, n_expected);
            bool equal = got.len == want.len;
            for (Int j = 0; equal && j < got.len; j++)
                equal = str_eq(((const Str *)got.p)[j], ((const Str *)want.p)[j]);
            if (!equal)
                testing_t_errorf_v(
                    t, "%s: unexpected chains returned:\ngot:\n\t%s\nwant:\n\t%s",
                    tests[i].name, strings_join(a, got, BURROW_S("\n\t")),
                    strings_join(a, want, BURROW_S("\n\t")));
        }
        arena_free(&ar);
    }
}

#undef I
#undef L

/* ----------------------------------------------------------- key usages */

typedef struct EkuDescs {
    X509ExtKeyUsage ekus[2];
    Int n;
    bool unknown; /* UnknownExtKeyUsage is {1, 2, 3} */
} EkuDescs;

static void apply_ekus(Alloc *a, X509Certificate *c, const EkuDescs *d) {
    c->ext_key_usage = ekus(a, d->ekus, d->n);
    c->unknown_ext_key_usage = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
    if (d->unknown) {
        Asn1ObjectIdentifier oid = ASN1_OID(1, 2, 3);
        c->unknown_ext_key_usage = slice_append(a, c->unknown_ext_key_usage, &oid, 1);
    }
}

/* genCertEdge with the EKUs in d put on the template. */
static X509Certificate *gen_eku_cert(TestingT *t, Alloc *a, const char *subject,
                                     EcdsaPrivateKey *key, const EkuDescs *d,
                                     int cert_type, const X509Certificate *issuer,
                                     EcdsaPrivateKey *signer) {
    Error err = BURROW_NO_ERROR;
    X509Certificate tmpl = {0};
    tmpl.serial_number = crypto_rand_int(a, (IoReader){0}, big_new_int(a, 100), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test serial: %v", err);
    tmpl.subject.common_name = str_from_cstr(subject);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    if (cert_type == ROOT_CERTIFICATE || cert_type == INTERMEDIATE_CERTIFICATE) {
        tmpl.is_ca = true;
        tmpl.basic_constraints_valid = true;
        tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
    }
    apply_ekus(a, &tmpl, d);
    if (cert_type == ROOT_CERTIFICATE) {
        issuer = &tmpl;
        signer = key;
    }
    Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, issuer,
                                        ecdsa_private_key_public(key),
                                        ecdsa_private_key_signer(signer), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test cert: %v", err);
    X509Certificate *c = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse test cert: %v", err);
    return c;
}

#define SA X509_EXT_KEY_USAGE_SERVER_AUTH
#define CA X509_EXT_KEY_USAGE_CLIENT_AUTH

static void TestEKUEnforcement(TestingT *t) {
    static const struct {
        const char *name;
        EkuDescs root;
        EkuDescs inters[4];
        Int n_inters;
        EkuDescs leaf;
        EkuDescs verify_ekus;
        const char *err;
    } tests[] = {
        {"valid, full chain",
         {{SA}, 1, false},
         {{{SA}, 1, false}},
         1,
         {{SA}, 1, false},
         {{SA}, 1, false},
         NULL},
        {"valid, only leaf has EKU",
         {{0}, 0, false},
         {{{0}, 0, false}},
         1,
         {{SA}, 1, false},
         {{SA}, 1, false},
         NULL},
        {"invalid, serverAuth not nested",
         {{CA}, 1, false},
         {{{SA, CA}, 2, false}},
         1,
         {{SA, CA}, 2, false},
         {{SA}, 1, false},
         "x509: certificate specifies an incompatible key usage"},
        {"valid, two EKUs, one path",
         {{SA}, 1, false},
         {{{SA, CA}, 2, false}},
         1,
         {{SA, CA}, 2, false},
         {{SA, CA}, 2, false},
         NULL},
        {"invalid, ladder",
         {{SA}, 1, false},
         {{{SA, CA}, 2, false},
          {{CA}, 1, false},
          {{SA, CA}, 2, false},
          {{SA}, 1, false}},
         4,
         {{SA}, 1, false},
         {{SA, CA}, 2, false},
         "x509: certificate specifies an incompatible key usage"},
        {"valid, intermediate has no EKU",
         {{SA}, 1, false},
         {{{0}, 0, false}},
         1,
         {{SA}, 1, false},
         {{SA}, 1, false},
         NULL},
        {"invalid, intermediate has no EKU and no nested path",
         {{CA}, 1, false},
         {{{0}, 0, false}},
         1,
         {{SA}, 1, false},
         {{SA, CA}, 2, false},
         "x509: certificate specifies an incompatible key usage"},
        {"invalid, intermediate has unknown EKU",
         {{SA}, 1, false},
         {{{0}, 0, true}},
         1,
         {{SA}, 1, false},
         {{SA}, 1, false},
         "x509: certificate specifies an incompatible key usage"},
    };

    Arena kar;
    arena_init(&kar, NULL, 0);
    EcdsaPrivateKey *k = ec_key(arena_allocator(&kar));
    for (Int i = 0; i < LEN(tests); i++) {
        ARENA(a);
        X509CertPool *root_pool = x509_new_cert_pool(a);
        X509Certificate *root =
            gen_eku_cert(t, a, "root", k, &tests[i].root, ROOT_CERTIFICATE, NULL, k);
        x509_cert_pool_add_cert(root_pool, root);

        X509Certificate *parent = root;
        X509CertPool *inter_pool = x509_new_cert_pool(a);
        for (Int j = 0; j < tests[i].n_inters; j++) {
            Str name = fmt_sprintf_v(a, "inter %d", j);
            X509Certificate *inter =
                gen_eku_cert(t, a, str_to_cstr(a, name), k, &tests[i].inters[j],
                             INTERMEDIATE_CERTIFICATE, parent, k);
            x509_cert_pool_add_cert(inter_pool, inter);
            parent = inter;
        }

        X509Certificate *leaf = gen_eku_cert(t, a, "leaf", k, &tests[i].leaf,
                                             INTERMEDIATE_CERTIFICATE, parent, k);

        X509VerifyOptions opts = {0};
        opts.roots = root_pool;
        opts.intermediates = inter_pool;
        opts.key_usages = ekus(a, tests[i].verify_ekus.ekus, tests[i].verify_ekus.n);
        Error err = BURROW_NO_ERROR;
        x509_certificate_verify(leaf, a, opts, &err);
        if (!BURROW_FAILED(err) && tests[i].err != NULL)
            testing_t_errorf_v(t, "%s: expected error", tests[i].name);
        else if (BURROW_FAILED(err) &&
                 (tests[i].err == NULL ||
                  !str_eq(error_text(err), str_from_cstr(tests[i].err))))
            testing_t_errorf_v(t, "%s: unexpected error: got %q, want %q",
                               tests[i].name, error_text(err),
                               tests[i].err == NULL ? "" : tests[i].err);
        arena_free(&ar);
    }
    arena_free(&kar);
}

#undef SA
#undef CA

static void TestVerifyEKURootAsLeaf(TestingT *t) {
    static const struct {
        X509ExtKeyUsage root_ekus[1];
        Int n_root;
        X509ExtKeyUsage verify_ekus[1];
        Int n_verify;
        bool succeed;
    } tests[] = {
        {{0}, 0, {X509_EXT_KEY_USAGE_SERVER_AUTH}, 1, true},
        {{X509_EXT_KEY_USAGE_SERVER_AUTH}, 1, {0}, 0, true},
        {{X509_EXT_KEY_USAGE_SERVER_AUTH},
         1,
         {X509_EXT_KEY_USAGE_SERVER_AUTH},
         1,
         true},
        {{X509_EXT_KEY_USAGE_SERVER_AUTH}, 1, {X509_EXT_KEY_USAGE_ANY}, 1, true},
        {{X509_EXT_KEY_USAGE_ANY}, 1, {X509_EXT_KEY_USAGE_SERVER_AUTH}, 1, true},
        {{X509_EXT_KEY_USAGE_CLIENT_AUTH},
         1,
         {X509_EXT_KEY_USAGE_SERVER_AUTH},
         1,
         false},
    };

    Arena kar;
    arena_init(&kar, NULL, 0);
    EcdsaPrivateKey *k = ec_key(arena_allocator(&kar));
    for (Int i = 0; i < LEN(tests); i++) {
        ARENA(a);
        X509Certificate tmpl = {0};
        tmpl.serial_number = big_new_int(a, 1);
        tmpl.subject.common_name = BURROW_S("root");
        tmpl.not_before = time_add(time_now(), -TIME_HOUR);
        tmpl.not_after = time_add(time_now(), TIME_HOUR);
        tmpl.dns_names = STRS(a, "localhost");
        tmpl.ext_key_usage = ekus(a, tests[i].root_ekus, tests[i].n_root);
        Error err = BURROW_NO_ERROR;
        Slice root_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                                 ecdsa_private_key_public(k),
                                                 ecdsa_private_key_signer(k), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to create certificate: %v", err);
        X509Certificate *root = x509_parse_certificate(a, root_der, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to parse certificate: %v", err);
        X509CertPool *roots = x509_new_cert_pool(a);
        x509_cert_pool_add_cert(roots, root);

        X509VerifyOptions opts = {0};
        opts.roots = roots;
        opts.key_usages = ekus(a, tests[i].verify_ekus, tests[i].n_verify);
        x509_certificate_verify(root, a, opts, &err);
        if (!BURROW_FAILED(err) && !tests[i].succeed)
            testing_t_errorf_v(t, "#%d: verification succeed", i);
        else if (BURROW_FAILED(err) && tests[i].succeed)
            testing_t_errorf_v(t, "#%d: verification failed: %q", i, error_text(err));
        arena_free(&ar);
    }
    arena_free(&kar);
}

static void TestVerifyNilPubKey(TestingT *t) {
    ARENA(a);
    static const Byte raw[] = {1, 2, 3};
    Slice bytes = {(void *)(uintptr_t)raw, 3, 3, TYPE_BYTE};
    X509Certificate c = {0};
    c.raw_issuer = bytes;
    c.authority_key_id = bytes;
    X509Certificate r = {0};
    r.raw_subject = bytes;
    r.subject_key_id = bytes;
    X509CertPool *roots = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(roots, &r);
    X509VerifyOptions opts = {0};
    opts.roots = roots;

    const X509Certificate *rp = &r;
    Slice current = slice_append(a, slice_nil(TYPE_X509_CERTIFICATE_PTR), &rp, 1);
    Slice chains;
    Error err = burrow__x509_build_chains(a, &c, current, &opts, &chains);
    if (errors_as(err, TYPE_X509_UNKNOWN_AUTHORITY_ERROR) == NULL)
        testing_t_fatalf_v(t,
                           "buildChains returned unexpected error, got: %v, want "
                           "UnknownAuthorityError",
                           err);
    arena_free(&ar);
}

static void TestVerifyBareWildcard(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *k = ec_key(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("test");
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    tmpl.dns_names = STRS(a, "*");
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                        ecdsa_private_key_public(k),
                                        ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to create certificate: %v", err);
    X509Certificate *c = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse certificate: %v", err);
    if (!BURROW_FAILED(x509_certificate_verify_hostname(c, BURROW_S("label"))))
        testing_t_fatalf_v(t,
                           "VerifyHostname unexpected success with bare wildcard SAN");
    arena_free(&ar);
}

/* ---------------------------------------------------------------- policies */

static X509OID policy_oid(Alloc *a, uint64_t last) {
    const uint64_t arcs[] = {1, 2, 840, 113554, 4, 1, 72585, 2, last};
    Error err = BURROW_NO_ERROR;
    X509OID oid = x509_oid_from_ints(
        a, (Slice){(void *)(uintptr_t)arcs, LEN(arcs), LEN(arcs), TYPE_UINT64}, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_verify_test: bad OID"));
    return oid;
}

static Slice oids(Alloc *a, const X509OID *v, Int n) {
    return slice_append(a, slice_nil(TYPE_X509_OID), v, n);
}

static Slice cert_chain(Alloc *a, const X509Certificate *const *v, Int n) {
    return slice_append(a, slice_nil(TYPE_X509_CERTIFICATE_PTR), v, n);
}

static void TestPoliciesValid(TestingT *t) {
    /* These cases, and the certificates they use, are BoringSSL's, at
     * https://boringssl.googlesource.com/boringssl/+/264f4f7a958af6c4ccb04662e302a99dfa7c5b85/crypto/x509/x509_test.cc#5913
     * less the ones that parse a certificate as part of verifying, which
     * are in TestParsePolicies. */
    ARENA(a);
    X509OID test_oid[6];
    for (uint64_t i = 1; i <= 5; i++)
        test_oid[i] = policy_oid(a, i);

    enum {
        ROOT,
        ROOT_CROSS_INHIBIT_MAPPING,
        ROOT2,
        INTERMEDIATE,
        INTERMEDIATE_ANY,
        INTERMEDIATE_MAPPED,
        INTERMEDIATE_MAPPED_ANY,
        INTERMEDIATE_MAPPED_OID3,
        INTERMEDIATE_REQUIRE,
        INTERMEDIATE_REQUIRE1,
        INTERMEDIATE_REQUIRE2,
        INTERMEDIATE_REQUIRE_NO_POLICIES,
        LEAF,
        LEAF_ANY,
        LEAF_NONE,
        LEAF_OID1,
        LEAF_OID2,
        LEAF_OID3,
        LEAF_OID4,
        LEAF_OID5,
        LEAF_REQUIRE,
        LEAF_REQUIRE1,
        /* intermediate_mapped or intermediate_mapped_any, for the cases Go
         * runs once with each. */
        MAPPED,
        N_CERTS,
    };
    static const char *const pems[] = {
        policy_root_pem,
        policy_root_cross_inhibit_mapping_pem,
        policy_root2_pem,
        policy_intermediate_pem,
        policy_intermediate_any_pem,
        policy_intermediate_mapped_pem,
        policy_intermediate_mapped_any_pem,
        policy_intermediate_mapped_oid3_pem,
        policy_intermediate_require_pem,
        policy_intermediate_require1_pem,
        policy_intermediate_require2_pem,
        policy_intermediate_require_no_policies_pem,
        policy_leaf_pem,
        policy_leaf_any_pem,
        policy_leaf_none_pem,
        policy_leaf_oid1_pem,
        policy_leaf_oid2_pem,
        policy_leaf_oid3_pem,
        policy_leaf_oid4_pem,
        policy_leaf_oid5_pem,
        policy_leaf_require_pem,
        policy_leaf_require1_pem,
    };
    const X509Certificate *certs[N_CERTS];
    for (Int i = 0; i < LEN(pems); i++)
        certs[i] = must_cert(a, pems[i]);

    /* valid: 1 for valid, 0 for not, 2 for valid only with
     * intermediate_mapped_any and 3 for valid only with intermediate_mapped. */
    typedef struct {
        int chain[4];
        Int n_chain;
        int policies[2];
        Int n_policies;
        bool require_explicit_policy;
        bool inhibit_policy_mapping;
        int valid;
    } TestCase;
    static const TestCase tests[] = {
        {{LEAF, INTERMEDIATE, ROOT}, 3, {0}, 0, true, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {1}, 1, true, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {2}, 1, true, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {3}, 1, true, false, 0},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {1, 2}, 2, true, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {1, 3}, 2, true, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {1}, 1, false, false, 1},
        {{LEAF, INTERMEDIATE, ROOT}, 3, {3}, 1, false, false, 1},
        {{LEAF, INTERMEDIATE_REQUIRE, ROOT}, 3, {1}, 1, false, false, 1},
        {{LEAF, INTERMEDIATE_REQUIRE, ROOT}, 3, {3}, 1, false, false, 0},
        {{LEAF_NONE, INTERMEDIATE_REQUIRE, ROOT}, 3, {0}, 0, true, false, 0},
        {{LEAF_REQUIRE, INTERMEDIATE, ROOT}, 3, {0}, 0, false, false, 1},
        {{LEAF_REQUIRE, INTERMEDIATE, ROOT}, 3, {1}, 1, false, false, 1},
        {{LEAF_REQUIRE, INTERMEDIATE, ROOT}, 3, {3}, 1, false, false, 0},
        {{LEAF, INTERMEDIATE_REQUIRE1, ROOT}, 3, {3}, 1, false, false, 0},
        {{LEAF, INTERMEDIATE_REQUIRE2, ROOT}, 3, {3}, 1, false, false, 1},
        {{LEAF_REQUIRE1, INTERMEDIATE, ROOT}, 3, {3}, 1, false, false, 1},
        {{LEAF_REQUIRE1, INTERMEDIATE_REQUIRE1, ROOT}, 3, {3}, 1, false, false, 0},
        {{LEAF_REQUIRE, INTERMEDIATE_REQUIRE2, ROOT}, 3, {3}, 1, false, false, 0},
        {{LEAF, INTERMEDIATE_REQUIRE_NO_POLICIES, ROOT}, 3, {1}, 1, false, false, 0},
        {{LEAF_ANY, INTERMEDIATE, ROOT}, 3, {1}, 1, true, false, 1},
        {{LEAF_ANY, INTERMEDIATE, ROOT}, 3, {3}, 1, true, false, 0},
        {{LEAF, INTERMEDIATE_ANY, ROOT}, 3, {1}, 1, true, false, 1},
        {{LEAF, INTERMEDIATE_ANY, ROOT}, 3, {3}, 1, true, false, 0},
        {{LEAF_ANY, INTERMEDIATE_ANY, ROOT}, 3, {1}, 1, true, false, 1},
        {{LEAF_ANY, INTERMEDIATE_ANY, ROOT}, 3, {3}, 1, true, false, 1},
        {{ROOT}, 1, {1}, 1, true, false, 1},
        {{LEAF_OID1, INTERMEDIATE_MAPPED_OID3, ROOT}, 3, {3}, 1, true, false, 1},
        {{LEAF_OID4, INTERMEDIATE_MAPPED_OID3, ROOT}, 3, {4}, 1, true, false, 0},
        {{LEAF_OID1, INTERMEDIATE_MAPPED_OID3, ROOT}, 3, {3}, 1, true, true, 0},
        {{LEAF_OID1, INTERMEDIATE_MAPPED_OID3, ROOT_CROSS_INHIBIT_MAPPING, ROOT2},
         4,
         {3},
         1,
         true,
         false,
         0},
    };
    static const TestCase extra_tests[] = {
        {{LEAF, MAPPED, ROOT}, 3, {3}, 1, true, false, 1},
        {{LEAF_OID1, MAPPED, ROOT}, 3, {3}, 1, true, false, 1},
        {{LEAF_OID2, MAPPED, ROOT}, 3, {3}, 1, true, false, 1},
        {{LEAF_OID3, MAPPED, ROOT}, 3, {3}, 1, true, false, 2},
        {{LEAF_OID1, MAPPED, ROOT}, 3, {1}, 1, true, false, 3},
        {{LEAF_OID4, MAPPED, ROOT}, 3, {4}, 1, true, false, 1},
        {{LEAF_OID4, MAPPED, ROOT}, 3, {5}, 1, true, false, 1},
        {{LEAF_OID5, MAPPED, ROOT}, 3, {4}, 1, true, false, 1},
        {{LEAF_OID5, MAPPED, ROOT}, 3, {5}, 1, true, false, 1},
        {{LEAF_OID4, MAPPED, ROOT}, 3, {4, 5}, 2, true, false, 1},
    };

    Int n = 0;
    for (int use_any = -1; use_any <= 1; use_any++) {
        const TestCase *cases = use_any < 0 ? tests : extra_tests;
        Int n_cases = use_any < 0 ? LEN(tests) : LEN(extra_tests);
        certs[MAPPED] =
            certs[use_any == 1 ? INTERMEDIATE_MAPPED_ANY : INTERMEDIATE_MAPPED];
        for (Int i = 0; i < n_cases; i++, n++) {
            const TestCase *tc = &cases[i];
            const X509Certificate *chain[4];
            for (Int j = 0; j < tc->n_chain; j++)
                chain[j] = certs[tc->chain[j]];
            X509OID policies[2];
            for (Int j = 0; j < tc->n_policies; j++)
                policies[j] = test_oid[tc->policies[j]];
            X509VerifyOptions opts = {0};
            opts.certificate_policies = oids(a, policies, tc->n_policies);
            opts.require_explicit_policy = tc->require_explicit_policy;
            opts.inhibit_policy_mapping = tc->inhibit_policy_mapping;
            bool want = tc->valid == 1 || (tc->valid == 2 && use_any == 1) ||
                        (tc->valid == 3 && use_any == 0);
            bool valid = burrow__x509_policies_valid(
                a, cert_chain(a, chain, tc->n_chain), &opts);
            if (valid != want)
                testing_t_errorf_v(t, "%d: policiesValid: got %t, want %t", n, valid,
                                   want);
        }
    }
    arena_free(&ar);
}

static void TestInvalidPolicyWithAnyKeyUsage(TestingT *t) {
    ARENA(a);
    X509OID test_oid3 = policy_oid(a, 3);
    X509Certificate *root = must_cert(a, policy_root_pem);
    X509Certificate *intermediate = must_cert(a, policy_intermediate_require_pem);
    X509Certificate *leaf = must_cert(a, policy_leaf_pem);

    const char *expected_err =
        "x509: no valid chains built: 1 candidate chains with invalid policies";

    X509CertPool *roots = x509_new_cert_pool(a);
    X509CertPool *intermediates = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(roots, root);
    x509_cert_pool_add_cert(intermediates, intermediate);

    X509VerifyOptions opts = {0};
    opts.roots = roots;
    opts.intermediates = intermediates;
    opts.key_usages = EKUS(a, X509_EXT_KEY_USAGE_ANY);
    opts.certificate_policies = oids(a, &test_oid3, 1);
    Error err = BURROW_NO_ERROR;
    x509_certificate_verify(leaf, a, opts, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "unexpected success, invalid policy shouldn't be bypassed "
                           "by passing VerifyOptions.KeyUsages with ExtKeyUsageAny");
    else if (!str_eq(error_text(err), str_from_cstr(expected_err)))
        testing_t_fatalf_v(t, "unexpected error, got %q, want %q", error_text(err),
                           expected_err);
    arena_free(&ar);
}

/* A DER element with tag and the bytes in content, from a. */
static Slice der_tlv(Alloc *a, Byte tag, Slice content) {
    Byte head[6] = {tag};
    Int n = 1;
    if (content.len < 0x80) {
        head[n++] = (Byte)content.len;
    } else {
        Int len_bytes = 0;
        for (Int l = content.len; l > 0; l >>= 8)
            len_bytes++;
        head[n++] = (Byte)(0x80 | len_bytes);
        for (Int i = len_bytes - 1; i >= 0; i--)
            head[n++] = (Byte)(content.len >> (8 * i));
    }
    Slice out = slice_append(a, slice_nil(TYPE_BYTE), head, n);
    return slice_append(a, out, content.p, content.len);
}

static Slice der_cat(Alloc *a, const Slice *parts, Int n) {
    Slice out = slice_nil(TYPE_BYTE);
    for (Int i = 0; i < n; i++)
        out = slice_append(a, out, parts[i].p, parts[i].len);
    return out;
}

#define DER_SEQ(a, ...)                                                                \
    der_tlv((a), 0x30,                                                                 \
            der_cat((a), (const Slice[]){__VA_ARGS__},                                 \
                    (Int)(sizeof((const Slice[]){__VA_ARGS__}) / sizeof(Slice))))

static Slice der_raw(Alloc *a, const Byte *b, Int n) {
    return slice_append(a, slice_nil(TYPE_BYTE), b, n);
}

/* A non-negative INTEGER. */
static Slice der_int(Alloc *a, const BigInt *x) {
    Slice b = big_int_bytes(x, a);
    Byte zero = 0;
    Slice content = slice_nil(TYPE_BYTE);
    if (b.len == 0 || (((const Byte *)b.p)[0] & 0x80) != 0)
        content = slice_append(a, content, &zero, 1);
    content = slice_append(a, content, b.p, b.len);
    return der_tlv(a, 0x02, content);
}

static Slice der_utc_time(Alloc *a, Time t) {
    Str s = time_format(time_utc(t), a, BURROW_S("060102150405"));
    Slice b = slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
    Byte z = 'Z';
    return der_tlv(a, 0x17, slice_append(a, b, &z, 1));
}

/* dsaSelfSignedCNX: a certificate for and from CN=X, with a DSA public key,
 * the same signature OID inside and out, and a made up ECDSA signature. */
static Slice dsa_self_signed_cnx(TestingT *t, Alloc *a) {
    DsaPrivateKey dsa_priv = {0};
    Error err = dsa_generate_parameters(&dsa_priv.public_key.parameters, a,
                                        (IoReader){0}, DSA_L1024N160);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    err = dsa_generate_key(&dsa_priv, a, (IoReader){0});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    const DsaPublicKey *dsa_pub = &dsa_priv.public_key;

    static const Byte oid_public_key_dsa[] = {0x06, 0x07, 0x2a, 0x86, 0x48,
                                              0xce, 0x38, 0x04, 0x01};
    static const Byte oid_signature_dsa_with_sha256[] = {
        0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x02};
    static const Byte cn_x[] = {0x30, 0x0c, 0x31, 0x0a, 0x30, 0x08, 0x06,
                                0x03, 0x55, 0x04, 0x03, 0x13, 0x01, 'X'};
    static const Byte signature[] = {0x03, 0x02, 0x00, 0x00};

    Slice param_der =
        DER_SEQ(a, der_int(a, dsa_pub->parameters.p), der_int(a, dsa_pub->parameters.q),
                der_int(a, dsa_pub->parameters.g));
    Slice y_der = der_int(a, dsa_pub->y);
    Byte unused = 0;
    Slice bits = slice_append(a, slice_nil(TYPE_BYTE), &unused, 1);
    bits = slice_append(a, bits, y_der.p, y_der.len);
    Slice spki = DER_SEQ(
        a,
        DER_SEQ(a, der_raw(a, oid_public_key_dsa, LEN(oid_public_key_dsa)), param_der),
        der_tlv(a, 0x03, bits));

    Slice raw_name = der_raw(a, cn_x, LEN(cn_x));
    Slice algo_ident = DER_SEQ(a, der_raw(a, oid_signature_dsa_with_sha256,
                                          LEN(oid_signature_dsa_with_sha256)));
    Time now = time_now();
    Slice tbs = DER_SEQ(a, der_int(a, big_new_int(a, 1002)), algo_ident, raw_name,
                        DER_SEQ(a, der_utc_time(a, time_add(now, -TIME_HOUR)),
                                der_utc_time(a, time_add(now, 24 * TIME_HOUR))),
                        raw_name, spki);
    return DER_SEQ(a, tbs, algo_ident, der_raw(a, signature, LEN(signature)));
}

static void TestCertificateChainSignedByECDSA(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *ca_key = ec_key(a);
    X509Certificate root_tmpl = {0};
    root_tmpl.serial_number = big_new_int(a, 1);
    root_tmpl.subject.common_name = BURROW_S("X");
    root_tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    root_tmpl.not_after = time_add(time_now(), 365 * 24 * TIME_HOUR);
    root_tmpl.is_ca = true;
    root_tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN | X509_KEY_USAGE_CRL_SIGN;
    root_tmpl.basic_constraints_valid = true;
    Error err = BURROW_NO_ERROR;
    Slice ca_der = x509_create_certificate(a, (IoReader){0}, &root_tmpl, &root_tmpl,
                                           ecdsa_private_key_public(ca_key),
                                           ecdsa_private_key_signer(ca_key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    X509Certificate *root = x509_parse_certificate(a, ca_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    EcdsaPrivateKey *leaf_key = ec_key(a);
    X509Certificate leaf_tmpl = {0};
    leaf_tmpl.serial_number = big_new_int(a, 42);
    leaf_tmpl.subject.common_name = BURROW_S("leaf");
    leaf_tmpl.not_before = time_add(time_now(), -10 * TIME_MINUTE);
    leaf_tmpl.not_after = time_add(time_now(), 24 * TIME_HOUR);
    leaf_tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
    leaf_tmpl.ext_key_usage = EKUS(a, X509_EXT_KEY_USAGE_SERVER_AUTH);
    leaf_tmpl.basic_constraints_valid = true;
    Slice leaf_der = x509_create_certificate(a, (IoReader){0}, &leaf_tmpl, root,
                                             ecdsa_private_key_public(leaf_key),
                                             ecdsa_private_key_signer(ca_key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    X509Certificate *leaf = x509_parse_certificate(a, leaf_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    X509Certificate *inter = x509_parse_certificate(a, dsa_self_signed_cnx(t, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    X509CertPool *inters = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(inters, root);
    x509_cert_pool_add_cert(inters, inter);

    const char *want_err = "certificate signed by unknown authority";
    X509VerifyOptions opts = {0};
    opts.intermediates = inters;
    opts.roots = x509_new_cert_pool(a);
    x509_certificate_verify(leaf, a, opts, &err);
    if (!strings_contains(error_text(err), str_from_cstr(want_err)))
        testing_t_errorf_v(t, "got %v, want %q", err, want_err);
    arena_free(&ar);
}

static void TestVerifyHostnameIPAddresses(TestingT *t) {
    ARENA(a);
    X509Certificate cert = {0};
    const NetIP ips[] = {net_parse_ip(a, BURROW_S("192.0.2.1")),
                         net_parse_ip(a, BURROW_S("fe80::1"))};
    cert.ip_addresses = slice_append(a, slice_nil(TYPE_NET_IP), ips, 2);

    static const struct {
        const char *name;
        const char *host;
        bool match;
    } tests[] = {
        {"IPv4", "192.0.2.1", true},
        {"IPv4 bracketed", "[192.0.2.1]", true},
        {"IPv4 mismatch", "192.0.2.2", false},
        {"IPv6", "fe80::1", true},
        {"IPv6 bracketed", "[fe80::1]", true},
        {"IPv6 with zone", "fe80::1%eth0", true},
        {"IPv6 with zone bracketed", "[fe80::1%eth0]", true},
        {"IPv6 mismatch", "fe80::2", false},
        {"IPv6 with zone mismatch", "fe80::2%eth0", false},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Error err =
            x509_certificate_verify_hostname(&cert, str_from_cstr(tests[i].host));
        if (tests[i].match && BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: VerifyHostname(%q) = %v, want nil",
                               tests[i].name, tests[i].host, err);
        if (!tests[i].match && !BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: VerifyHostname(%q) = nil, want error",
                               tests[i].name, tests[i].host);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGoVerify)                                                                    \
    X(TestUnknownAuthorityError)                                                       \
    X(TestValidHostname)                                                               \
    X(TestPathologicalChains)                                                          \
    X(TestSystemRootsError)                                                            \
    X(TestSystemRootsErrorUnwrap)                                                      \
    X(TestPathBuilding)                                                                \
    X(TestEKUEnforcement)                                                              \
    X(TestVerifyEKURootAsLeaf)                                                         \
    X(TestVerifyNilPubKey)                                                             \
    X(TestVerifyBareWildcard)                                                          \
    X(TestPoliciesValid)                                                               \
    X(TestInvalidPolicyWithAnyKeyUsage)                                                \
    X(TestCertificateChainSignedByECDSA)                                               \
    X(TestVerifyHostnameIPAddresses)

TESTING_MAIN(TESTS)
