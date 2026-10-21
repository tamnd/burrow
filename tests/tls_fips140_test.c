/* crypto/tls: fips140_test.go.
 * Go source: go1.27.1.
 *
 * Not in Go: there is no BoringCrypto here, so boring.Enabled is always false.
 * The FIPS 140-3 module is never v1.0.0, so MustMinimumFIPS140ModuleVersion
 * never skips and the ML-DSA leaf is always made. Where Go clones
 * testConfigFIPS140, a new one is made, and isECDSA is left out, since nothing
 * calls it.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/fips140.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_handshake_server_test.h"
#include "tls_handshake_test.h"
#include "tls_test_gen.h"

#include <stdint.h>
#include <string.h>

static void free_arena(void *env) {
    arena_free(env);
    mem_free(heap_allocator(), env, sizeof(Arena), _Alignof(Arena));
}

/* Not in Go: an arena that is given back when t is done. */
static Alloc *test_arena(TestingT *t) {
    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, free_arena, ar));
    return arena_allocator(ar);
}

static void release_config(void *env) {
    tls_config_release(env);
}

/* testConfigFIPS140. Not in Go: a new one, made in a, that is released when t
 * is done. */
static TlsConfig *test_config_fips140(TestingT *t, Alloc *a) {
    TlsConfig *config = BURROW_NEW(a, TlsConfig);
    if (config == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(config, 0, sizeof *config);
    config->time = (TlsTimeFunc){test_time, NULL};
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 6, 6);
    TlsCertificate *c = certs.p;
    c[0] = parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    c[1] = parse_test_cert(a, test_rsapss_cert_pem, test_rsapss_key_pem);
    c[2] = parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    c[3] = parse_test_cert(a, test_mldsa44_cert_pem, test_mldsa44_key_pem);
    c[4] = parse_test_cert(a, test_mldsa65_cert_pem, test_mldsa65_key_pem);
    c[5] = parse_test_cert(a, test_mldsa87_cert_pem, test_mldsa87_key_pem);
    config->certificates = certs;
    config->root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(
        config->root_cas,
        parse_test_cert(a, test_root_cert_pem, test_root_key_pem).leaf);
    config->server_name = BURROW_S("test.golang.example");
    testing_t_cleanup(t, BURROW_FN(Func, release_config, config));
    return config;
}

/* Not in Go: cryptotest.RerunWithFIPS140Enforced. */
static void rerun_with_fips140_enforced(TestingT *t) {
    fipstest_rerun(t, test_arena(t), "only", NULL);
}

/* Not in Go: testHandshake for the tests that only look at the error, which
 * is kept in a. */
static Error test_handshake_err(TestingT *t, Alloc *a, TlsConfig *client_config,
                                TlsConfig *server_config) {
    Handshake h;
    test_handshake(t, client_config, server_config, &h);
    Error err = error_retain(a, h.err);
    handshake_free(&h);
    return err;
}

/* allCipherSuitesIncludingTLS13. Not in Go: made in a. */
static Slice all_cipher_suites_including_tls13(Alloc *a) {
    Slice s = slice_make(a, TYPE_UINT16, 0,
                         burrow__tls_ncipher_suites + burrow__tls_ncipher_suites_tls13);
    for (Int i = 0; i < burrow__tls_ncipher_suites; i++)
        s = BURROW_APPEND(uint16_t, a, s, burrow__tls_cipher_suites[i].id);
    for (Int i = 0; i < burrow__tls_ncipher_suites_tls13; i++)
        s = BURROW_APPEND(uint16_t, a, s, burrow__tls_cipher_suites_tls13[i].id);
    return s;
}

static bool is_tls13_cipher_suite(uint16_t id) {
    for (Int i = 0; i < burrow__tls_ncipher_suites_tls13; i++) {
        if (id == burrow__tls_cipher_suites_tls13[i].id)
            return true;
    }
    return false;
}

/* generateKeyShare. Not in Go: the share is made in a, and the private keys
 * are given back. */
static TlsKeyShare generate_key_share(Alloc *a, TlsCurveID group) {
    Error err = BURROW_NO_ERROR;
    const TlsKeyExchange *ke = burrow__tls_key_exchange_for_curve_id(group, &err);
    if (ke == NULL)
        panic_str(error_text(err));
    TlsKeySharePrivateKeys priv;
    Slice shares;
    if (!burrow__tls_key_exchange_key_shares(ke, a, (IoReader){0}, &priv, &shares,
                                             &err))
        panic_str(error_text(err));
    burrow__tls_key_share_private_keys_free(&priv);
    return BURROW_AT(TlsKeyShare, shares, 0);
}

typedef struct VersionTest {
    const char *name;
    uint16_t v;
    const char *msg;
} VersionTest;

static void server_protocol_version(void *env, TestingT *t) {
    const VersionTest *tc = env;
    Alloc *a = test_arena(t);
    TlsConfig *server_config = test_config_fips140(t, a);
    server_config->min_version = TLS_VERSION_SSL30;
    server_config->max_version = TLS_VERSION_TLS13;
    TlsConfig *client_config = test_config_fips140(t, a);
    client_config->min_version = tc->v;
    client_config->max_version = tc->v;
    Error err = test_handshake_err(t, a, client_config, server_config);
    if (tc->msg[0] == '\0') {
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "got error: %v, expected success", err);
    } else {
        if (!BURROW_FAILED(err))
            testing_t_fatal_v(t, "got success, expected error");
        if (!strings_contains(error_text(err), str_from_cstr(tc->msg)))
            testing_t_fatalf_v(t, "got error %v, expected %q", err,
                               str_from_cstr(tc->msg));
    }
}

static void test_server_protocol_version(TestingT *t, const VersionTest *tc) {
    testing_t_run(t, str_from_cstr(tc->name),
                  BURROW_FN(TestingTFunc, server_protocol_version, (uintptr_t)tc));
}

static void server_protocol_versions_no_fips(void *env, TestingT *t) {
    (void)env;
    static const VersionTest tests[] = {
        {"VersionTLS10", TLS_VERSION_TLS10, ""},
        {"VersionTLS11", TLS_VERSION_TLS11, ""},
        {"VersionTLS12", TLS_VERSION_TLS12, ""},
        {"VersionTLS13", TLS_VERSION_TLS13, ""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        test_server_protocol_version(t, &tests[i]);
}

static void server_protocol_versions_fips(void *env, TestingT *t) {
    (void)env;
    static const VersionTest tests[] = {
        {"VersionTLS10", TLS_VERSION_TLS10, "supported versions"},
        {"VersionTLS11", TLS_VERSION_TLS11, "supported versions"},
        {"VersionTLS12", TLS_VERSION_TLS12, ""},
        {"VersionTLS13", TLS_VERSION_TLS13, ""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        test_server_protocol_version(t, &tests[i]);
}

static void TestFIPSServerProtocolVersion(TestingT *t) {
    run_with_fips_disabled(t, server_protocol_versions_no_fips, NULL);

    run_with_fips_enabled(t, server_protocol_versions_fips, NULL);

    if (!fips140_enforced())
        rerun_with_fips140_enforced(t);
}

/* Not in Go: the name is made in a. */
static bool is_fips_cipher_suite(Alloc *a, uint16_t id) {
    Str name = tls_cipher_suite_name(a, id);
    if (is_tls13_cipher_suite(id)) {
        switch (id) {
        case TLS_TLS_AES_128_GCM_SHA256:
        case TLS_TLS_AES_256_GCM_SHA384:
            return true;
        case TLS_TLS_CHACHA20_POLY1305_SHA256:
            return false;
        default:
            panic_str(fmt_sprintf_v(a, "unknown TLS 1.3 cipher suite: %s", name));
        }
    }
    switch (id) {
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
        return true;
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256:
        /* Only for the native module. */
        return true;
    default:
        break;
    }
    if (strings_contains(name, BURROW_S("CHACHA20")))
        return false;
    if (strings_has_suffix(name, BURROW_S("_SHA"))) /* SHA-1 */
        return false;
    if (strings_has_prefix(name, BURROW_S("TLS_RSA"))) /* RSA kex */
        return false;
    panic_str(fmt_sprintf_v(a, "unknown cipher suite: %s", name));
}

/* Not in Go: the curve's name is made in a. */
static bool is_fips_curve(Alloc *a, TlsCurveID id) {
    switch (id) {
    case TLS_CURVE_P256:
    case TLS_CURVE_P384:
    case TLS_CURVE_P521:
        return true;
    case TLS_X25519MLKEM768:
    case TLS_SEC_P256_R1_MLKEM768:
    case TLS_SEC_P384_R1_MLKEM1024:
    case TLS_MLKEM1024:
        /* Only for the native module. */
        return true;
    case TLS_X25519:
        return false;
    default:
        panic_str(fmt_sprintf_v(a, "unknown curve: %s", tls_curve_id_string(id, a)));
    }
}

/* Not in Go: the scheme's name is made in a. */
static bool is_fips_signature_scheme(Alloc *a, TlsSignatureScheme alg) {
    switch (alg) {
    case TLS_PKCS1_WITH_SHA256:
    case TLS_ECDSA_WITH_P256_AND_SHA256:
    case TLS_PKCS1_WITH_SHA384:
    case TLS_ECDSA_WITH_P384_AND_SHA384:
    case TLS_PKCS1_WITH_SHA512:
    case TLS_ECDSA_WITH_P521_AND_SHA512:
    case TLS_PSS_WITH_SHA256:
    case TLS_PSS_WITH_SHA384:
    case TLS_PSS_WITH_SHA512:
        return true;
    case TLS_ED25519:
    case TLS_MLDSA44:
    case TLS_MLDSA65:
    case TLS_MLDSA87:
        /* Only for the native module. */
        return true;
    case TLS_PKCS1_WITH_SHA1:
    case TLS_ECDSA_WITH_SHA1:
        return false;
    default:
        panic_str(fmt_sprintf_v(a, "unknown signature scheme: %s",
                                tls_signature_scheme_string(alg, a)));
    }
}

typedef struct SuiteTest {
    uint16_t id;
    TlsConfig *server_config;
    const TlsHandshakeMsg *client_hello;
} SuiteTest;

static void server_cipher_suite_no_fips(void *env, TestingT *t) {
    const SuiteTest *st = env;
    test_client_hello(t, st->server_config, st->client_hello);
}

static void server_cipher_suite_fips(void *env, TestingT *t) {
    const SuiteTest *st = env;
    const char *msg = "";
    if (!is_fips_cipher_suite(test_arena(t), st->id))
        msg = "no cipher suite supported by both client and server";
    test_client_hello_failure(t, st->server_config, st->client_hello, msg);
}

static void server_cipher_suite(void *env, TestingT *t) {
    uint16_t id = *(const uint16_t *)env;
    Alloc *a = test_arena(t);
    TlsConfig *server_config = test_config_fips140(t, a);
    uint16_t suites[1] = {id};
    Byte compression[1] = {TLS_COMPRESSION_NONE};
    uint16_t curves[1] = {TLS_CURVE_P256};
    TlsKeyShare key_shares[1] = {generate_key_share(a, TLS_CURVE_P256)};
    Byte points[1] = {TLS_POINT_FORMAT_UNCOMPRESSED};
    uint16_t versions[1] = {TLS_VERSION_TLS12};
    TlsHandshakeMsg client_hello;
    burrow__tls_handshake_msg_init(&client_hello, TLS_MSG_CLIENT_HELLO);
    TlsClientHelloMsg *ch = &client_hello.client_hello;
    ch->vers = TLS_VERSION_TLS12;
    ch->random = slice_make(a, TYPE_BYTE, 32, 32);
    ch->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ch->compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    ch->supported_curves = slice_from(curves, 1, 1, TYPE_UINT16);
    ch->key_shares = slice_from(key_shares, 1, 1, &burrow__tls_key_share_type);
    ch->supported_points = slice_from(points, 1, 1, TYPE_BYTE);
    ch->supported_versions = slice_from(versions, 1, 1, TYPE_UINT16);
    ch->supported_signature_algorithms =
        slice_from((void *)(uintptr_t)burrow__tls_allowed_signature_algorithms_fips, 13,
                   13, TYPE_UINT16);
    if (is_tls13_cipher_suite(id))
        versions[0] = TLS_VERSION_TLS13;
    else
        server_config->cipher_suites = ch->cipher_suites;

    SuiteTest st = {id, server_config, &client_hello};
    run_with_fips_disabled(t, server_cipher_suite_no_fips, &st);

    run_with_fips_enabled(t, server_cipher_suite_fips, &st);
}

static void TestFIPSServerCipherSuites(TestingT *t) {
    Alloc *a = test_arena(t);
    Slice ids = all_cipher_suites_including_tls13(a);
    for (Int i = 0; i < ids.len; i++) {
        uint16_t *id = (uint16_t *)ids.p + i;
        Str name = fmt_sprintf_v(a, "suite=%s", tls_cipher_suite_name(a, *id));
        testing_t_run(t, name, BURROW_FN(TestingTFunc, server_cipher_suite, id));
    }

    if (!fips140_enforced())
        rerun_with_fips140_enforced(t);
}

typedef struct CurveTest {
    TlsCurveID curveid;
    TlsConfig *test_config;
} CurveTest;

static void server_curve_no_fips(void *env, TestingT *t) {
    const CurveTest *ct = env;
    Error err = test_handshake_err(t, test_arena(t), ct->test_config, ct->test_config);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "got error: %v, expected success", err);
}

static void server_curve_fips(void *env, TestingT *t) {
    const CurveTest *ct = env;
    Alloc *a = test_arena(t);
    Error err = test_handshake_err(t, a, ct->test_config, ct->test_config);
    if (BURROW_FAILED(err) && is_fips_curve(a, ct->curveid))
        testing_t_fatalf_v(t, "got error: %v, expected success", err);
    else if (!BURROW_FAILED(err) && !is_fips_curve(a, ct->curveid))
        testing_t_fatal_v(t, "got success, expected error");
}

static void server_curve(void *env, TestingT *t) {
    CurveTest ct;
    ct.curveid = *(const TlsCurveID *)env;
    Alloc *a = test_arena(t);
    ct.test_config = test_config_fips140(t, a);
    TlsCurveID curves[1] = {ct.curveid};
    ct.test_config->curve_preferences = slice_from(curves, 1, 1, TYPE_UINT16);

    run_with_fips_disabled(t, server_curve_no_fips, &ct);

    /* With fipstls forced, bad curves should be rejected. */
    run_with_fips_enabled(t, server_curve_fips, &ct);
}

static void TestFIPSServerCurves(TestingT *t) {
    Alloc *a = test_arena(t);
    for (size_t i = 0; i < 8; i++) {
        const TlsCurveID *curveid = &burrow__tls_curve_preference_order[i];
        Str name = fmt_sprintf_v(a, "curve=%s", tls_curve_id_string(*curveid, a));
        testing_t_run(t, name,
                      BURROW_FN(TestingTFunc, server_curve, (uintptr_t)curveid));
    }

    if (!fips140_enforced())
        rerun_with_fips140_enforced(t);
}

/* Not in Go: the goroutine fipsHandshake starts for the client. */
typedef struct FIPSClient {
    NetConn c;
    TlsConfig *config;
    TlsConn *client;
    Error err;
} FIPSClient;

static void fips_client(void *env) {
    FIPSClient *j = env;
    if (j->client == NULL)
        j->err = burrow_err_out_of_memory;
    else
        j->err = tls_conn_handshake(j->client);
    (void)j->c.vt->closer.close(j->c.data);
}

/* fipsHandshake. Not in Go: the errors are kept in a, and the connections are
 * given back before it returns. */
static void fips_handshake(TestingT *t, Alloc *a, TlsConfig *client_config,
                           TlsConfig *server_config, Error *client_err,
                           Error *server_err) {
    (void)t;
    NetConn c, s;
    local_pipe(a, &c, &s);
    FIPSClient j;
    memset(&j, 0, sizeof j);
    j.c = c;
    j.config = client_config;
    j.client = tls_client(heap_allocator(), c, client_config);
    TlsConn *server = tls_server(heap_allocator(), s, server_config);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, fips_client, &j));
    Error err = burrow_err_out_of_memory;
    if (server != NULL)
        err = tls_conn_handshake(server);
    *server_err = error_retain(a, err);
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    *client_err = error_retain(a, j.err);
    if (server != NULL)
        tls_conn_free(server);
    if (j.client != NULL)
        tls_conn_free(j.client);
    local_pipe_free(c);
}

static void reset_signature_algorithms(void *env) {
    (void)env;
    burrow__tls_testing_only_supported_signature_algorithms = slice_nil(TYPE_UINT16);
}

static void reset_godebug(void *env) {
    (void)env;
    burrow__tls_godebug_set(NULL);
}

typedef struct SigHashTest {
    TlsSignatureScheme sig_hash;
    TlsConfig *client_config;
    TlsConfig *server_config;
} SigHashTest;

static void server_signature_and_hash_no_fips(void *env, TestingT *t) {
    const SigHashTest *st = env;
    Alloc *a = test_arena(t);
    Error client_err, server_err;
    fips_handshake(t, a, st->client_config, st->server_config, &client_err,
                   &server_err);
    if (BURROW_FAILED(client_err))
        testing_t_fatalf_v(t,
                           "expected handshake with %s to succeed; client error: %v; "
                           "server error: %v",
                           tls_signature_scheme_string(st->sig_hash, a), client_err,
                           server_err);
}

static void server_signature_and_hash_fips(void *env, TestingT *t) {
    const SigHashTest *st = env;
    Alloc *a = test_arena(t);
    Error client_err, server_err;
    fips_handshake(t, a, st->client_config, st->server_config, &client_err,
                   &server_err);
    Str name = tls_signature_scheme_string(st->sig_hash, a);
    if (is_fips_signature_scheme(a, st->sig_hash)) {
        if (BURROW_FAILED(client_err))
            testing_t_fatalf_v(t, "expected handshake with %s to succeed; err=%v", name,
                               client_err);
    } else {
        if (!BURROW_FAILED(client_err))
            testing_t_fatalf_v(
                t, "expected handshake with %s to fail, but it succeeded", name);
    }
}

static void server_signature_and_hash(void *env, TestingT *t) {
    SigHashTest st = *(const SigHashTest *)env;
    bool is_mldsa = st.sig_hash == TLS_MLDSA44 || st.sig_hash == TLS_MLDSA65 ||
                    st.sig_hash == TLS_MLDSA87;
    Alloc *a = test_arena(t);
    st.server_config = test_config_fips140(t, a);
    uint16_t sig_algs[1] = {st.sig_hash};
    burrow__tls_testing_only_supported_signature_algorithms =
        slice_from(sig_algs, 1, 1, TYPE_UINT16);
    /* PKCS#1 v1.5 signature algorithms can't be used standalone in TLS 1.3, and
     * the ECDSA ones bind to the curve used. However, ML-DSA requires TLS 1.3. */
    if (!is_mldsa)
        st.server_config->max_version = TLS_VERSION_TLS12;

    run_with_fips_disabled(t, server_signature_and_hash_no_fips, &st);

    /* With fipstls forced, bad curves should be rejected. */
    run_with_fips_enabled(t, server_signature_and_hash_fips, &st);

    /* Not in Go: sig_algs goes away on return. */
    burrow__tls_testing_only_supported_signature_algorithms = slice_nil(TYPE_UINT16);
}

static void TestFIPSServerSignatureAndHash(TestingT *t) {
    Alloc *a = test_arena(t);
    testing_t_cleanup(t, BURROW_FN(Func, reset_signature_algorithms, NULL));
    /* Not in Go: crypto/tls reads GODEBUG once, so it is told what
     * testenv.SetGODEBUG would set it to, and to read it again when t is done. */
    Str godebug = fmt_sprintf_v(a, "%s,tlssha1=1", os_getenv(a, BURROW_S("GODEBUG")));
    burrow__tls_godebug_set(str_to_cstr(a, godebug));
    testing_t_cleanup(t, BURROW_FN(Func, reset_godebug, NULL));

    TlsConfig *client_config = test_config_fips140(t, a);
    for (size_t i = 0; i < 15; i++) {
        SigHashTest st = {burrow__tls_default_supported_signature_algorithms[i],
                          client_config, NULL};
        Str name = tls_signature_scheme_string(st.sig_hash, a);
        testing_t_run(t, name, BURROW_FN(TestingTFunc, server_signature_and_hash, &st));
    }

    if (!fips140_enforced())
        rerun_with_fips140_enforced(t);
}

/* Not in Go: what testFIPSClientHello's defers and its goroutine leave to be
 * given back. */
typedef struct FIPSClientHello {
    NetConn c, s;
    TlsConn *client;
    TlsConn *srv;
    SyncWaitGroup wg;
} FIPSClientHello;

static void fips_client_hello_handshake(void *env) {
    FIPSClientHello *j = env;
    (void)tls_conn_handshake(j->client);
}

static void fips_client_hello_done(void *env) {
    FIPSClientHello *j = env;
    (void)j->c.vt->closer.close(j->c.data);
    (void)j->s.vt->closer.close(j->s.data);
    sync_wait_group_wait(&j->wg);
    if (j->client != NULL)
        tls_conn_free(j->client);
    if (j->srv != NULL)
        tls_conn_free(j->srv);
    net_pipe_free(j->c);
}

static void test_fips_client_hello(void *env, TestingT *t) {
    (void)env;
    /* Test that no matter what we put in the client config, the client does not
     * offer non-FIPS configurations. */
    Alloc *a = test_arena(t);
    FIPSClientHello *j = BURROW_NEW(a, FIPSClientHello);
    if (j == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(j, 0, sizeof *j);
    net_pipe(a, &j->c, &j->s);

    TlsConfig *client_config = test_config_fips140(t, a);
    /* All sorts of traps for the client to avoid. */
    client_config->min_version = TLS_VERSION_SSL30;
    client_config->max_version = TLS_VERSION_TLS13;
    client_config->cipher_suites = all_cipher_suites_including_tls13(a);
    client_config->curve_preferences = slice_from(
        (void *)(uintptr_t)burrow__tls_curve_preference_order, 8, 8, TYPE_UINT16);

    j->client = tls_client(heap_allocator(), j->c, client_config);
    j->srv = tls_server(heap_allocator(), j->s, test_config_fips140(t, a));
    testing_t_cleanup(t, BURROW_FN(Func, fips_client_hello_done, j));
    if (j->client == NULL || j->srv == NULL)
        testing_t_fatal_v(t, "out of memory");
    sync_wait_group_go(&j->wg, BURROW_FN(Func, fips_client_hello_handshake, j));
    TlsHandshakeMsg msg;
    Error err =
        burrow__tls_conn_read_handshake(j->srv, (IoWriter){NULL, NULL}, a, &msg);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (msg.kind != TLS_MSG_CLIENT_HELLO)
        testing_t_fatalf_v(t, "unexpected message type %s",
                           burrow__tls_handshake_msg_type_name(msg.kind));
    const TlsClientHelloMsg *hello = &msg.client_hello;

    if (!is_fips_version(hello->vers))
        testing_t_errorf_v(t, "client vers=%#x", (Int)hello->vers);
    for (Int i = 0; i < hello->supported_versions.len; i++) {
        uint16_t v = BURROW_AT(uint16_t, hello->supported_versions, i);
        if (!is_fips_version(v))
            testing_t_errorf_v(t, "client offered disallowed version %#x", (Int)v);
    }
    for (Int i = 0; i < hello->cipher_suites.len; i++) {
        uint16_t id = BURROW_AT(uint16_t, hello->cipher_suites, i);
        if (!is_fips_cipher_suite(a, id))
            testing_t_errorf_v(t, "client offered disallowed suite %s",
                               tls_cipher_suite_name(a, id));
    }
    for (Int i = 0; i < hello->supported_curves.len; i++) {
        TlsCurveID id = BURROW_AT(TlsCurveID, hello->supported_curves, i);
        if (!is_fips_curve(a, id))
            testing_t_errorf_v(t, "client offered disallowed curve %s",
                               tls_curve_id_string(id, a));
    }
    for (Int i = 0; i < hello->supported_signature_algorithms.len; i++) {
        TlsSignatureScheme sig_hash =
            BURROW_AT(TlsSignatureScheme, hello->supported_signature_algorithms, i);
        if (!is_fips_signature_scheme(a, sig_hash))
            testing_t_errorf_v(t, "client offered disallowed signature-and-hash %s",
                               tls_signature_scheme_string(sig_hash, a));
    }
}

static void TestFIPSClientHello(TestingT *t) {
    run_with_fips_enabled(t, test_fips_client_hello, NULL);
}

enum { FIPS_CERT_CA = 0, FIPS_CERT_LEAF = 1, FIPS_CERT_FIPS_OK = 0x80 };

/* Not in Go: the keys come back as the any fipsCert takes. */
static CryptoPrivateKey fips_rsa_key(TestingT *t, Alloc *a, Int size) {
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *k = rsa_generate_key(a, (IoReader){0}, size, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return BURROW_ANY(TYPE_RSA_PRIVATE_KEY, k);
}

static CryptoPrivateKey fips_ecdsa_key(TestingT *t, Alloc *a, EllipticCurve curve) {
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, curve, (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k);
}

static CryptoPrivateKey fips_mldsa_key(TestingT *t, Alloc *a,
                                       const MldsaParameters *params) {
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *k = mldsa_generate_key(a, params, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, k);
}

typedef struct FIPSCertificate {
    Str name;
    Str org;
    Str parent_org;
    Slice der;
    X509Certificate *cert;
    CryptoPrivateKey key;
    bool fips_ok;
} FIPSCertificate;

/* Not in Go: the key as the crypto.Signer it is there. */
static CryptoSigner fips_signer(CryptoPrivateKey key) {
    if (key.t == TYPE_RSA_PRIVATE_KEY)
        return rsa_private_key_signer(key.data);
    if (key.t == TYPE_ECDSA_PRIVATE_KEY)
        return ecdsa_private_key_signer(key.data);
    return mldsa_private_key_signer(key.data);
}

typedef struct CertAllowedTest {
    const X509Certificate *cert;
    Str desc;
    bool fips_ok;
} CertAllowedTest;

static void fips_cert_allowed(void *env, TestingT *t) {
    const CertAllowedTest *ct = env;
    if (burrow__tls_is_certificate_allowed_fips(ct->cert) != ct->fips_ok)
        testing_t_errorf_v(t, "fipsAllowCert(cert with %s key) = %v, want %v", ct->desc,
                           (bool)!ct->fips_ok, ct->fips_ok);
}

/* Not in Go: made in a. */
static FIPSCertificate *fips_cert(TestingT *t, Alloc *a, const char *name_,
                                  CryptoPrivateKey key, const FIPSCertificate *parent,
                                  int mode) {
    Str name = str_from_cstr(name_);
    Str org = name;
    Str parent_org = BURROW_STR_EMPTY;
    Int i = strings_index(org, BURROW_S("_"));
    if (i >= 0) {
        org = (Str){name.p, i};
        parent_org = (Str){name.p + i + 1, name.len - i - 1};
    }
    X509Certificate *tmpl = BURROW_NEW(a, X509Certificate);
    if (tmpl == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(tmpl, 0, sizeof *tmpl);
    tmpl->serial_number = big_new_int(a, 1);
    tmpl->subject.organization = BURROW_APPEND(Str, a, slice_nil(TYPE_STRING), org);
    tmpl->not_before = time_from_unix(0, 0);
    tmpl->not_after = time_from_unix(0, 0);

    tmpl->key_usage =
        X509_KEY_USAGE_KEY_ENCIPHERMENT | X509_KEY_USAGE_DIGITAL_SIGNATURE;
    tmpl->ext_key_usage = slice_make(a, TYPE_INT, 2, 2);
    ((X509ExtKeyUsage *)tmpl->ext_key_usage.p)[0] = X509_EXT_KEY_USAGE_SERVER_AUTH;
    ((X509ExtKeyUsage *)tmpl->ext_key_usage.p)[1] = X509_EXT_KEY_USAGE_CLIENT_AUTH;
    tmpl->basic_constraints_valid = true;
    if ((mode & ~FIPS_CERT_FIPS_OK) == FIPS_CERT_LEAF) {
        tmpl->dns_names =
            BURROW_APPEND(Str, a, slice_nil(TYPE_STRING), BURROW_S("example.com"));
    } else {
        tmpl->is_ca = true;
        tmpl->key_usage |= X509_KEY_USAGE_CERT_SIGN;
    }

    const X509Certificate *pcert;
    CryptoPrivateKey pkey;
    if (parent != NULL) {
        pcert = parent->cert;
        pkey = parent->key;
    } else {
        pcert = tmpl;
        pkey = key;
    }

    CryptoPublicKey pub;
    Str desc;
    if (key.t == TYPE_RSA_PRIVATE_KEY) {
        const RsaPrivateKey *k = key.data;
        pub = rsa_private_key_public(k);
        desc = fmt_sprintf_v(a, "RSA-%d", big_int_bit_len(k->public_key.n));
    } else if (key.t == TYPE_ECDSA_PRIVATE_KEY) {
        const EcdsaPrivateKey *k = key.data;
        pub = ecdsa_private_key_public(k);
        desc = fmt_sprintf_v(a, "ECDSA-%s",
                             elliptic_curve_params(k->public_key.curve)->name);
    } else if (key.t == TYPE_MLDSA_PRIVATE_KEY) {
        const MldsaPrivateKey *k = key.data;
        pub = mldsa_private_key_public(k);
        desc = mldsa_parameters_string(
            mldsa_public_key_parameters(mldsa_private_key_public_key(k)));
    } else {
        testing_t_fatalf_v(t, "invalid key %T", key);
        return NULL;
    }

    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, tmpl, pcert, pub,
                                        fips_signer(pkey), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    X509Certificate *cert = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    bool fips_ok = (mode & FIPS_CERT_FIPS_OK) != 0;
    CertAllowedTest ct = {cert, desc, fips_ok};
    run_with_fips_enabled(t, fips_cert_allowed, &ct);

    FIPSCertificate *c = BURROW_NEW(a, FIPSCertificate);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    *c = (FIPSCertificate){name, org, parent_org, der, cert, key, fips_ok};
    return c;
}

static Time time_unix_zero(void *env) {
    (void)env;
    return time_from_unix(0, 0);
}

/* Not in Go: list is a [][]byte, and the configs are made in a. */
static TlsConfig *fips_cert_server_config(TestingT *t, Alloc *a, CryptoPrivateKey key,
                                          Slice list) {
    TlsConfig *server_config = test_config_fips140(t, a);
    TlsCertificate *cert = BURROW_NEW(a, TlsCertificate);
    if (cert == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(cert, 0, sizeof *cert);
    cert->certificate = list;
    cert->private_key = key;
    server_config->certificates = slice_from(cert, 1, 1, TYPE_TLS_CERTIFICATE);
    return server_config;
}

/* client verifying server cert */
static void test_server_cert(TestingT *t, Alloc *a, Str desc, X509CertPool *pool,
                             CryptoPrivateKey key, Slice list, bool ok) {
    TlsConfig *client_config = test_config_fips140(t, a);
    client_config->root_cas = pool;
    client_config->insecure_skip_verify = false;
    client_config->server_name = BURROW_S("example.com");
    client_config->time = (TlsTimeFunc){time_unix_zero, NULL};

    TlsConfig *server_config = fips_cert_server_config(t, a, key, list);
    server_config->time = (TlsTimeFunc){time_unix_zero, NULL};

    Error client_err, server_err;
    fips_handshake(t, a, client_config, server_config, &client_err, &server_err);

    if (!BURROW_FAILED(client_err) == ok) {
        if (ok)
            testing_t_logf_v(t, "%s: accept", desc);
        else
            testing_t_logf_v(t, "%s: reject", desc);
    } else {
        if (ok)
            testing_t_errorf_v(t, "%s: BAD reject (%v)", desc, client_err);
        else
            testing_t_errorf_v(t, "%s: BAD accept", desc);
    }
}

/* server verifying client cert */
static void test_client_cert(TestingT *t, Alloc *a, Str desc, X509CertPool *pool,
                             CryptoPrivateKey key, Slice list, bool ok) {
    TlsConfig *client_config = fips_cert_server_config(t, a, key, list);
    client_config->insecure_skip_verify = true;
    client_config->time = (TlsTimeFunc){time_unix_zero, NULL};

    TlsConfig *server_config = test_config_fips140(t, a);
    server_config->client_cas = pool;
    server_config->client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    server_config->time = (TlsTimeFunc){time_unix_zero, NULL};

    Error client_err, server_err;
    fips_handshake(t, a, client_config, server_config, &client_err, &server_err);

    if (!BURROW_FAILED(server_err) == ok) {
        if (ok)
            testing_t_logf_v(t, "%s: accept", desc);
        else
            testing_t_logf_v(t, "%s: reject", desc);
    } else {
        if (ok)
            testing_t_errorf_v(t, "%s: BAD reject (%v)", desc, server_err);
        else
            testing_t_errorf_v(t, "%s: BAD accept", desc);
    }
}

/* Not in Go: the certificates TestFIPSCertAlgs makes, which its closures
 * share. */
typedef struct CertAlgs {
    FIPSCertificate *i_r1, *l2_i, *l3_i;
    X509CertPool *r1pool;
} CertAlgs;

/* Not in Go: the [][]byte of a leaf and one certificate after it. */
static Slice cert_list(Alloc *a, const FIPSCertificate *leaf,
                       const FIPSCertificate *next) {
    Slice list = BURROW_APPEND(Bytes, a, slice_nil(TYPE_BYTES), leaf->der);
    return BURROW_APPEND(Bytes, a, list, next->der);
}

static void cert_algs_basic_no_fips(void *env, TestingT *t) {
    const CertAlgs *c = env;
    Alloc *a = test_arena(t);
    Slice l2 = cert_list(a, c->l2_i, c->i_r1);
    test_server_cert(t, a, BURROW_S("basic"), c->r1pool, c->l2_i->key, l2, true);
    test_client_cert(t, a, BURROW_S("basic (client cert)"), c->r1pool, c->l2_i->key, l2,
                     true);
    if (c->l3_i != NULL) {
        Slice l3 = cert_list(a, c->l3_i, c->i_r1);
        test_server_cert(t, a, BURROW_S("basic ML-DSA"), c->r1pool, c->l3_i->key, l3,
                         true);
        test_client_cert(t, a, BURROW_S("basic ML-DSA (client cert)"), c->r1pool,
                         c->l3_i->key, l3, true);
    }
}

static void cert_algs_basic_fips(void *env, TestingT *t) {
    const CertAlgs *c = env;
    Alloc *a = test_arena(t);
    Slice l2 = cert_list(a, c->l2_i, c->i_r1);
    test_server_cert(t, a, BURROW_S("basic (fips)"), c->r1pool, c->l2_i->key, l2,
                     false);
    test_client_cert(t, a, BURROW_S("basic (fips, client cert)"), c->r1pool,
                     c->l2_i->key, l2, false);
    if (c->l3_i != NULL) {
        Slice l3 = cert_list(a, c->l3_i, c->i_r1);
        test_server_cert(t, a, BURROW_S("basic ML-DSA (fips)"), c->r1pool, c->l3_i->key,
                         l3, c->l3_i->fips_ok);
        test_client_cert(t, a, BURROW_S("basic ML-DSA (fips, client cert)"), c->r1pool,
                         c->l3_i->key, l3, c->l3_i->fips_ok);
    }
}

/* Not in Go: a map[string]bool of the few orgs there are. */
typedef struct OrgSet {
    Str org[16];
    bool v[16];
    Int n;
} OrgSet;

static bool org_get(const OrgSet *m, Str org) {
    for (Int i = 0; i < m->n; i++) {
        if (str_eq(m->org[i], org))
            return m->v[i];
    }
    return false;
}

static void org_set(OrgSet *m, Str org, bool v) {
    for (Int i = 0; i < m->n; i++) {
        if (str_eq(m->org[i], org)) {
            m->v[i] = v;
            return;
        }
    }
    m->org[m->n] = org;
    m->v[m->n] = v;
    m->n++;
}

/* Not in Go: what the closures of the exhaustive test share. */
typedef struct Exhaustive {
    Alloc *a;
    Slice list;
    Str list_name;
    OrgSet reachable, reachable_fips;
    X509CertPool *pool;
    Str root_name;
    bool should_verify, should_verify_fips;
    const FIPSCertificate *leaf;
} Exhaustive;

static void add_list(Exhaustive *e, Int cond, const FIPSCertificate *c) {
    if (cond != 0) {
        e->list = BURROW_APPEND(Bytes, e->a, e->list, c->der);
        e->list_name = fmt_sprintf_v(e->a, "%s,%s", e->list_name, c->name);
        if (org_get(&e->reachable, c->org))
            org_set(&e->reachable, c->parent_org, true);
        if (org_get(&e->reachable_fips, c->org) && c->fips_ok)
            org_set(&e->reachable_fips, c->parent_org, true);
    }
}

static void add_root(Exhaustive *e, Int cond, const FIPSCertificate *c) {
    if (cond != 0) {
        e->root_name = fmt_sprintf_v(e->a, "%s,%s", e->root_name, c->name);
        x509_cert_pool_add_cert(e->pool, c->cert);
        if (org_get(&e->reachable, c->org))
            e->should_verify = true;
        if (org_get(&e->reachable_fips, c->org) && c->fips_ok)
            e->should_verify_fips = true;
    }
}

/* Not in Go: rootName[1:]. */
static Str root_name_tail(const Exhaustive *e) {
    return (Str){e->root_name.p + 1, e->root_name.len - 1};
}

static void exhaustive_no_fips(void *env, TestingT *t) {
    const Exhaustive *e = env;
    Alloc *a = test_arena(t);
    Str root = root_name_tail(e);
    test_server_cert(t, a, fmt_sprintf_v(a, "%s->%s", e->list_name, root), e->pool,
                     e->leaf->key, e->list, e->should_verify);
    test_client_cert(t, a, fmt_sprintf_v(a, "%s->%s(client cert)", e->list_name, root),
                     e->pool, e->leaf->key, e->list, e->should_verify);
}

static void exhaustive_fips(void *env, TestingT *t) {
    const Exhaustive *e = env;
    Alloc *a = test_arena(t);
    Str root = root_name_tail(e);
    test_server_cert(t, a, fmt_sprintf_v(a, "%s->%s (fips)", e->list_name, root),
                     e->pool, e->leaf->key, e->list, e->should_verify_fips);
    test_client_cert(t, a,
                     fmt_sprintf_v(a, "%s->%s (fips, client cert)", e->list_name, root),
                     e->pool, e->leaf->key, e->list, e->should_verify_fips);
}

/* Not in Go: testenv.CPUIsSlow, for the GOARCH values it names that burrow
 * builds for. */
static bool cpu_is_slow(void) {
#if defined(BURROW_ARCH_ARM) || defined(BURROW_ARCH_MIPS64) || defined(BURROW_ARCH_WASM)
    return true;
#else
    return false;
#endif
}

static void TestFIPSCertAlgs(TestingT *t) {
    /* arm and wasm time out generating keys. Nothing in this test is
     * architecture-specific, so just don't bother on those. */
    if (cpu_is_slow())
        testing_t_skipf_v(t, "skipping on %s/%s because key generation takes too long",
                          str_from_cstr(burrow_os_name()),
                          str_from_cstr(burrow_arch_name()));

    Alloc *a = test_arena(t);

    /* Set up some roots, intermediate CAs, and leaf certs with various
     * algorithms. X_Y is X signed by Y. */
    FIPSCertificate *R1 = fips_cert(t, a, "R1", fips_rsa_key(t, a, 2048), NULL,
                                    FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    FIPSCertificate *R2 =
        fips_cert(t, a, "R2", fips_rsa_key(t, a, 1024), NULL, FIPS_CERT_CA);
    FIPSCertificate *R3 = fips_cert(t, a, "R3", fips_rsa_key(t, a, 4096), NULL,
                                    FIPS_CERT_CA | FIPS_CERT_FIPS_OK);

    FIPSCertificate *M1_R1 =
        fips_cert(t, a, "M1_R1", fips_ecdsa_key(t, a, elliptic_p256()), R1,
                  FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    FIPSCertificate *M2_R1 = fips_cert(
        t, a, "M2_R1", fips_ecdsa_key(t, a, elliptic_p224()), R1, FIPS_CERT_CA);

    FIPSCertificate *I_R1 = fips_cert(t, a, "I_R1", fips_rsa_key(t, a, 3072), R1,
                                      FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    FIPSCertificate *I_R2 =
        fips_cert(t, a, "I_R2", I_R1->key, R2, FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    FIPSCertificate *I_M1 =
        fips_cert(t, a, "I_M1", I_R1->key, M1_R1, FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    FIPSCertificate *I_M2 =
        fips_cert(t, a, "I_M2", I_R1->key, M2_R1, FIPS_CERT_CA | FIPS_CERT_FIPS_OK);

    FIPSCertificate *I_R3 = fips_cert(t, a, "I_R3", fips_rsa_key(t, a, 3072), R3,
                                      FIPS_CERT_CA | FIPS_CERT_FIPS_OK);
    (void)fips_cert(t, a, "I_R3", I_R3->key, R3, FIPS_CERT_CA | FIPS_CERT_FIPS_OK);

    FIPSCertificate *L1_I =
        fips_cert(t, a, "L1_I", fips_ecdsa_key(t, a, elliptic_p384()), I_R1,
                  FIPS_CERT_LEAF | FIPS_CERT_FIPS_OK);
    FIPSCertificate *L2_I =
        fips_cert(t, a, "L2_I", fips_rsa_key(t, a, 1024), I_R1, FIPS_CERT_LEAF);
    FIPSCertificate *L3_I = NULL;
    if (!str_eq(fips140_version(), BURROW_S("v1.0.0"))) {
        /* ML-DSA is not implemented by the Go+BoringCrypto FIPS 140 module. */
        int mldsa_flags = FIPS_CERT_LEAF | FIPS_CERT_FIPS_OK;
        L3_I = fips_cert(t, a, "L3_I", fips_mldsa_key(t, a, mldsa_mldsa44()), I_R1,
                         mldsa_flags);
    }

    /* Run simple basic test with known answers before proceeding to exhaustive
     * test with computed answers. */
    X509CertPool *r1pool = x509_new_cert_pool(a);
    if (r1pool == NULL)
        testing_t_fatal_v(t, "out of memory");
    x509_cert_pool_add_cert(r1pool, R1->cert);

    CertAlgs basic = {I_R1, L2_I, L3_I, r1pool};
    run_with_fips_disabled(t, cert_algs_basic_no_fips, &basic);

    run_with_fips_enabled(t, cert_algs_basic_fips, &basic);

    if (testing_t_failed(t))
        testing_t_fatal_v(t, "basic test failed, skipping exhaustive test");

    if (testing_short()) {
        testing_t_logf_v(t,
                         "basic test passed; skipping exhaustive test in -short mode");
        return;
    }

    for (int l = 1; l <= 2; l++) {
        const FIPSCertificate *leaf = L1_I;
        if (l == 2)
            leaf = L2_I;
        for (Int i = 0; i < 64; i++) {
            Exhaustive e;
            memset(&e, 0, sizeof e);
            e.a = a;
            e.leaf = leaf;
            org_set(&e.reachable, leaf->parent_org, true);
            org_set(&e.reachable_fips, leaf->parent_org, leaf->fips_ok);
            e.list = BURROW_APPEND(Bytes, a, slice_nil(TYPE_BYTES), leaf->der);
            e.list_name = leaf->name;
            add_list(&e, i & 1, I_R1);
            add_list(&e, i & 2, I_R2);
            add_list(&e, i & 4, I_M1);
            add_list(&e, i & 8, I_M2);
            add_list(&e, i & 16, M1_R1);
            add_list(&e, i & 32, M2_R1);

            for (Int r = 1; r <= 3; r++) {
                e.pool = x509_new_cert_pool(a);
                if (e.pool == NULL)
                    testing_t_fatal_v(t, "out of memory");
                e.root_name = BURROW_S(",");
                e.should_verify = false;
                e.should_verify_fips = false;
                add_root(&e, r & 1, R1);
                add_root(&e, r & 2, R2);
                e.root_name = root_name_tail(&e); /* strip leading comma */

                run_with_fips_disabled(t, exhaustive_no_fips, &e);

                run_with_fips_enabled(t, exhaustive_fips, &e);
            }
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestFIPSServerProtocolVersion)                                                   \
    X(TestFIPSServerCipherSuites)                                                      \
    X(TestFIPSServerCurves)                                                            \
    X(TestFIPSServerSignatureAndHash)                                                  \
    X(TestFIPSClientHello)                                                             \
    X(TestFIPSCertAlgs)

TESTING_MAIN(TESTS)
