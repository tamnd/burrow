/* Not in Go: Go doesn't run BetterTLS in its tests. This runs the test cases
 * of Netflix's BetterTLS, github.com/Netflix/bettertls, against crypto/x509.
 *
 * TestBetterTLS fetches the module, as TestACVP fetches its own, and runs its
 * bettertls command with export-tests, which writes every test case of its
 * suites to a JSON file with a trust root made for the run. Each test case is a
 * list of certificates, leaf first, and a host name. Go's own runner for
 * BetterTLS, in test-suites/impltests/golang.go, makes a TLS connection with an
 * http.Client, whose RootCAs has only the trust root, and crypto/tls verifies
 * the server's certificates with Certificate.Verify, with the rest of them as
 * the intermediates and the server name as the DNSName. This does that Verify
 * directly. The test passes when every test case gets the result BetterTLS
 * expects, other than those it marks failureIsWarning, which are counted and
 * logged. It needs the go command on $PATH.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fetchmodule.h"

#include "burrow/burrow.h"
#include "burrow/crypto/x509.h"
#include "burrow/sort.h"

static const Str bettertls_module = BURROW_S_INIT("github.com/Netflix/bettertls");
static const Str bettertls_version =
    BURROW_S_INIT("v0.0.0-20260117180001-bc5adea19ed8");

/* The JSON export-tests writes, from test-suites/cmd/bettertls/export_tests.go.
 * featureTestCases is left out, as nothing here needs it. */
BURROW_SLICE_TYPE(BettertlsStrs, Str);
BURROW_SLICE_TYPE(BettertlsCertificates, Bytes);

#define BETTERTLS_TEST_CASE_FIELDS(F, T)                                               \
    F(T, Int, Id, "json:\"id\"")                                                       \
    F(T, Str, Suite, "json:\"suite\"")                                                 \
    F(T, BettertlsCertificates, Certificates, "json:\"certificates\"")                 \
    F(T, Str, Hostname, "json:\"hostname\"")                                           \
    F(T, BettertlsStrs, RequiredFeatures, "json:\"requiredFeatures\"")                 \
    F(T, Str, Expected, "json:\"expected\"")                                           \
    F(T, bool, FailureIsWarning, "json:\"failureIsWarning\"")
BURROW_STRUCT(BettertlsTestCase, BETTERTLS_TEST_CASE_FIELDS);

BURROW_SLICE_TYPE(BettertlsTestCases, BettertlsTestCase);

#define BETTERTLS_SUITE_FIELDS(F, T)                                                   \
    F(T, BettertlsStrs, Features, "json:\"features\"")                                 \
    F(T, Int, SanityCheckTestCase, "json:\"sanityCheckTestCase\"")                     \
    F(T, BettertlsTestCases, TestCases, "json:\"testCases\"")
BURROW_STRUCT(BettertlsSuite, BETTERTLS_SUITE_FIELDS);

BURROW_MAP_TYPE(BettertlsSuiteMap, Str, BettertlsSuite);

#define BETTERTLS_EXPORT_FIELDS(F, T)                                                  \
    F(T, Str, BetterTlsRevision, "json:\"betterTlsRevision\"")                         \
    F(T, Bytes, TrustRoot, "json:\"trustRoot\"")                                       \
    F(T, BettertlsSuiteMap, Suites, "json:\"suites\"")
BURROW_STRUCT(BettertlsExport, BETTERTLS_EXPORT_FIELDS);

/* The memory of TestBetterTLS, freed after its other cleanups, even when it
 * is skipped or fails. */
static void bettertls_free_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

/* Whether Verify accepts the certificates of tc, with roots as the roots.
 * When it doesn't, *why is the reason, from a. A certificate that doesn't
 * parse fails the handshake in crypto/tls, so that is a rejection too. */
static bool bettertls_accepts(Alloc *a, const BettertlsTestCase *tc,
                              const X509CertPool *roots, Error *why) {
    *why = BURROW_NO_ERROR;
    const Bytes *ders = tc->Certificates.p;
    if (tc->Certificates.len == 0) {
        *why = errors_new(a, BURROW_S("no certificates"));
        return false;
    }
    X509Certificate *leaf = x509_parse_certificate(a, ders[0], why);
    if (BURROW_FAILED(*why))
        return false;
    X509CertPool *intermediates = x509_new_cert_pool(a);
    for (Int i = 1; i < tc->Certificates.len; i++) {
        X509Certificate *c = x509_parse_certificate(a, ders[i], why);
        if (BURROW_FAILED(*why))
            return false;
        x509_cert_pool_add_cert(intermediates, c);
    }
    X509VerifyOptions opts = {0};
    opts.dns_name = tc->Hostname;
    opts.intermediates = intermediates;
    opts.roots = roots;
    (void)x509_certificate_verify(leaf, a, opts, why);
    return !BURROW_FAILED(*why);
}

/* What one suite's subtest needs. */
typedef struct BettertlsRun {
    const BettertlsSuite *suite;
    const X509CertPool *roots;
} BettertlsRun;

static void run_suite(void *env, TestingT *t) {
    const BettertlsRun *r = env;
    const BettertlsTestCase *cases = r->suite->TestCases.p;
    Int n = r->suite->TestCases.len;
    if (r->suite->SanityCheckTestCase < 0 || r->suite->SanityCheckTestCase >= n) {
        testing_t_fatalf_v(t, "sanity check test case %d of %d is out of range",
                           r->suite->SanityCheckTestCase, n);
        return;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The sanity check test case needs no features and should pass, so when it
     * doesn't, something is wrong with this harness and not with a feature. */
    Error why = BURROW_NO_ERROR;
    if (!bettertls_accepts(a, &cases[r->suite->SanityCheckTestCase], r->roots, &why)) {
        testing_t_errorf_v(t, "sanity check test case %d: %v",
                           r->suite->SanityCheckTestCase, why);
        arena_free(&ar);
        testing_t_fail_now(t);
        return;
    }
    arena_reset(&ar);

    Int passed = 0, failed = 0, warnings = 0;
    for (Int i = 0; i < n; i++) {
        const BettertlsTestCase *tc = &cases[i];
        bool want = str_eq(tc->Expected, BURROW_S("ACCEPT"));
        bool got = bettertls_accepts(a, tc, r->roots, &why);
        Str got_s = got ? BURROW_S("ACCEPT") : BURROW_S("REJECT");
        if (got == want) {
            passed++;
        } else if (tc->FailureIsWarning) {
            warnings++;
            testing_t_logf_v(t, "test case %d (warning): got %s, want %s, for %q: %v",
                             tc->Id, got_s, tc->Expected, tc->Hostname, why);
        } else {
            failed++;
            testing_t_errorf_v(t, "test case %d: got %s, want %s, for %q: %v", tc->Id,
                               got_s, tc->Expected, tc->Hostname, why);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
    testing_t_logf_v(t, "%d test cases: %d passed, %d failed, %d warnings", n, passed,
                     failed, warnings);
}

static void TestBetterTLS(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
    }

    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, bettertls_free_arena, ar));
    Alloc *a = arena_allocator(ar);

    Str dir = fetch_module(t, a, bettertls_module, bettertls_version);
    Str out_file =
        filepath_join_v(a, 2, fetchmodule_temp_dir(t, a), BURROW_S("tests.json"));

    ExecCmd *cmd =
        exec_command_v(a, go_tool_path(t, a), 5, BURROW_S("run"),
                       BURROW_S("./test-suites/cmd/bettertls"),
                       BURROW_S("export-tests"), BURROW_S("--out"), out_file);
    cmd->dir = dir;
    cmd->env = clean_cmd_env(a);
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "bettertls export-tests failed: %s\n%s", err,
                           fetchmodule_str(out));
    }

    Slice data = os_read_file(a, out_file, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to read the exported tests: %s", err);
    }
    BettertlsExport ex = {0};
    err = json_unmarshal(a, data, BURROW_ANY(TYPE_OF(BettertlsExport), &ex));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse the exported tests: %s", err);
    }

    X509Certificate *root = x509_parse_certificate(a, ex.TrustRoot, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse the trust root: %s", err);
    }
    X509CertPool *roots = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(roots, root);

    /* The suites in name order, so that the output is the same each time. */
    Slice names = slice_nil(TYPE_STRING);
    MapIter it = map_iter(ex.Suites);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        names = BURROW_APPEND(Str, a, names, *(const Str *)k);
    }
    sort_strings(names);
    if (names.len == 0) {
        testing_t_fatal_v(t, "the export has no test suites");
    }
    for (Int i = 0; i < names.len; i++) {
        Str name = ((const Str *)names.p)[i];
        BettertlsRun *r = BURROW_NEW(a, BettertlsRun);
        r->suite = BURROW_MAP_GET(Str, BettertlsSuite, ex.Suites, name);
        r->roots = roots;
        testing_t_run(t, name, BURROW_FN(TestingTFunc, run_suite, r));
    }
}

#define TESTS(X) X(TestBetterTLS)
TESTING_MAIN(TESTS)
