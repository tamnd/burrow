/* Derived from Go's src/crypto/internal/fips140test/cast_test.go, with
 * moduleStatus from fips_test.go and fips140v126Conditionals from
 * fips140v1.26_test.go in the same directory.
 * Go source: go1.27.1.
 *
 * TestCASTPasses and TestCASTFailures run TestConditionals again with
 * GODEBUG=fips140=debug or fips140=on. That runs the integrity check too, so
 * the program needs the module's checksum, and when this one has none, the
 * tests run a copy of it with the checksum written in, as
 * fips140_check_test.c does.
 *
 * Go leaves the ML-DSA self-tests out of allCASTs when the module is v1.0.0,
 * which burrow's never is, so that is left out here.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/fips140.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/rsa.h"
#include "burrow/mem/heap.h"
#include "burrow/regexp.h"
#include "burrow/sort.h"

#include <stdint.h>
#include <string.h>

#define S(x) BURROW_S(x)

static const char *const all_casts[] = {
    "AES-CBC",
    "CTR_DRBG",
    "CounterKDF",
    "DetECDSA P-256 SHA2-512 sign",
    "ECDH PCT",
    "ECDSA P-256 SHA2-512 sign and verify",
    "ECDSA PCT",
    "Ed25519 sign and verify",
    "Ed25519 sign and verify PCT",
    "HKDF-SHA2-256",
    "HMAC-SHA2-256",
    "KAS-ECC-SSC P-256",
    "ML-DSA sign and verify PCT",
    "ML-DSA-44",
    "ML-KEM PCT", /* -768 */
    "ML-KEM PCT", /* -1024 */
    "ML-KEM-768",
    "PBKDF2",
    "RSA sign and verify PCT",
    "RSASSA-PKCS-v1.5 2048-bit sign and verify",
    "SHA2-256",
    "SHA2-512",
    "TLSv1.2-SHA2-256",
    "TLSv1.3-SHA2-256",
    "cSHAKE128",
};

#define NUM_CASTS (sizeof all_casts / sizeof all_casts[0])

static void TestAllCASTs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Not in Go: testenv.MustHaveSource and "go list" for where the module is.
     * The tests run from the top of the tree, and the module is in the files
     * of src/crypto. */
    Str fips_dir = S("src/crypto");
    Error err = BURROW_NO_ERROR;
    Slice ents = os_read_dir(a, fips_dir, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_skipf_v(t, "no sources in src/crypto");
    }
    testing_t_logf_v(t, "FIPS module directory: %s", fips_dir);

    /* Find all invocations of fips140.CAST or fips140.PCT. */
    Slice found = slice_nil(TYPE_STRING);
    Regexp *cast_re =
        regexp_must_compile(a, S("burrow__fips140_(cast|pct)\\(\\s*\"([^\"]+)\""));
    for (Int i = 0; i < ents.len; i++) {
        FsDirEntry *d = (FsDirEntry *)slice_at(ents, i);
        Str name = d->vt->name(d->data);
        if (d->vt->is_dir(d->data) ||
            !(strings_has_suffix(name, S(".c")) || strings_has_suffix(name, S(".h"))))
            continue;
        Slice data = os_read_file(a, filepath_join_v(a, 2, fips_dir, name), &err);
        if (BURROW_FAILED(err)) {
            regexp_free(cast_re);
            arena_free(&ar);
            testing_t_fatalf_v(t, "WalkDir: %s", error_text(err));
        }
        Slice ms = regexp_find_all_string_submatch(
            cast_re, a, str_from_bytes(data.p, data.len), -1);
        for (Int j = 0; j < ms.len; j++)
            found = BURROW_APPEND(Str, a, found,
                                  BURROW_AT(Str, BURROW_AT(Slice, ms, j), 2));
    }
    /* Not in Go: the search memory is not from a, and Go has a collector. */
    regexp_free(cast_re);

    /* Not in Go: "ML-KEM PCT" is in allCASTs twice because Go has it once in
     * the file of each parameter set. burrow has it once, in the macro that
     * defines both, so a name that allCASTs repeats is looked for once. */
    Slice want = slice_nil(TYPE_STRING);
    for (size_t i = 0; i < NUM_CASTS; i++)
        if (i == 0 || strcmp(all_casts[i], all_casts[i - 1]) != 0)
            want = BURROW_APPEND(Str, a, want, str_from_cstr(all_casts[i]));

    sort_strings(found);
    bool equal = found.len == want.len;
    for (Int i = 0; equal && i < found.len; i++)
        equal = str_eq(BURROW_AT(Str, found, i), BURROW_AT(Str, want, i));
    if (!equal) {
        Str list = strings_join(a, found, S("\", \""));
        testing_t_errorf_v(t, "AllCASTs is out of date. Found CASTs: []string{\"%s\"}",
                           list);
    }
    arena_free(&ar);
}

static void fips140v126_conditionals(Alloc *a, Slice zeros) {
    Error err = BURROW_NO_ERROR;
    /* ML-DSA sign and verify PCT */
    MldsaPrivateKey *k_mldsa = mldsa_generate_key(a, mldsa_mldsa44(), &err);
    /* ML-DSA-44 */
    CryptoSignerOpts none = {0};
    if (k_mldsa != NULL)
        (void)mldsa_private_key_sign_deterministic(k_mldsa, a, zeros, none, &err);
}

/* TestConditionals causes the conditional CASTs and PCTs to be invoked. */
static void TestConditionals(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice zeros = slice_make(a, TYPE_BYTE, 32, 32);

    fips140v126_conditionals(a, zeros);
    /* ML-KEM PCT */
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *k_mlkem = mlkem_generate_key768(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        /* ML-KEM-768 */
        Slice ct;
        (void)mlkem_encapsulation_key768_encapsulate(
            mlkem_decapsulation_key768_encapsulation_key(k_mlkem), a, &ct);
    }
    /* ECDH PCT */
    err = BURROW_NO_ERROR;
    EcdhPrivateKey *k_dh = ecdh_curve_generate_key(ecdh_p256(), a, (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        /* KAS-ECC-SSC P-256 */
        (void)ecdh_private_key_ecdh(k_dh, a, ecdh_private_key_public_key(k_dh), &err);
    }
    /* ECDSA PCT */
    err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k_dsa =
        ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        /* ECDSA P-256 SHA2-512 sign and verify */
        CryptoHash sha256 = CRYPTO_SHA256;
        (void)ecdsa_private_key_sign(k_dsa, a, (IoReader){0}, zeros,
                                     crypto_hash_as_signer_opts(&sha256), &err);
    }
    /* Ed25519 sign and verify PCT */
    err = BURROW_NO_ERROR;
    Ed25519PrivateKey k_25519;
    (void)ed25519_generate_key(a, (IoReader){0}, &k_25519, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        /* Ed25519 sign and verify */
        (void)ed25519_sign(a, k_25519, zeros);
    }
    /* RSA sign and verify PCT */
    err = BURROW_NO_ERROR;
    RsaPrivateKey *k_rsa = rsa_generate_key(a, (IoReader){0}, 2048, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        /* RSASSA-PKCS-v1.5 2048-bit sign and verify */
        (void)rsa_sign_pkcs1_v15(a, (IoReader){0}, k_rsa, CRYPTO_SHA256, zeros, &err);
    }
    arena_free(&ar);
    testing_t_logf_v(t, "completed successfully");
}

/* Runs TestConditionals in exe with setting added to its environment, and
 * returns what it printed. */
static Str run_conditionals(Alloc *a, Str exe, Str setting, Error *err) {
    ExecCmd *cmd =
        exec_command_v(a, exe, 2, S("-test.run=^TestConditionals$"), S("-test.v"));
    cmd->env = BURROW_APPEND(Str, a, os_environ(a), setting);
    Slice out = exec_cmd_combined_output(cmd, a, err);
    /* Not in Go: the C library on Windows writes each newline to standard
     * error as \r\n. */
    return strings_replace_all(a, str_from_bytes(out.p, out.len), S("\r\n"), S("\n"));
}

typedef struct CastRun {
    Str out;
    Str exe;
    const char *name;
} CastRun;

static void cast_passed(void *env, TestingT *t) {
    const CastRun *r = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str want = fmt_sprintf_v(arena_allocator(&ar), "passed: %s\n", r->name);
    if (!strings_contains(r->out, want))
        testing_t_errorf_v(t, "CAST/PCT %s success was not logged", r->name);
    else
        testing_t_logf_v(t, "CAST/PCT succeeded: %s", r->name);
    arena_free(&ar);
}

static void TestCASTPasses(TestingT *t) {
    module_status(t);
    MUST_SUPPORT_FIPS140(t);
    SKIP_WITHOUT_EXEC(t);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir;
    Str exe = fipstest_program(t, a, &dir);
    if (exe.len > 0) {
        Error err = BURROW_NO_ERROR;
        Str out = run_conditionals(a, exe, S("GODEBUG=fips140=debug"), &err);
        testing_t_logf_v(t, "running with GODEBUG=fips140=debug:\n%s", out);
        if (BURROW_FAILED(err) || !strings_contains(out, S("completed successfully")))
            testing_t_errorf_v(t, "TestConditionals did not complete successfully");

        for (size_t i = 0; i < NUM_CASTS; i++) {
            CastRun r = {out, exe, all_casts[i]};
            testing_t_run(t, str_from_cstr(all_casts[i]),
                          BURROW_FN(TestingTFunc, cast_passed, &r));
        }
    }
    if (dir.len > 0)
        os_remove_all(dir);
    arena_free(&ar);
}

static void cast_failure(void *env, TestingT *t) {
    const CastRun *r = env;
    /* Don't parallelize if running in verbose mode, to produce a less
     * confusing recoding for the validation lab. */
    if (!testing_verbose())
        testing_t_parallel(t);
    testing_t_logf_v(t, "Testing CAST/PCT failure...");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str godebug = fmt_sprintf_v(a, "GODEBUG=failfipscast=%s,fips140=on", r->name);
    Error err = BURROW_NO_ERROR;
    Str out = run_conditionals(a, r->exe, godebug, &err);
    testing_t_logf_v(t, "running with %s:\n%s", godebug, out);
    if (!BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "test did not fail as expected");
    }
    Str want = fmt_sprintf_v(a, "self-test failed: %s", r->name);
    if (strings_contains(out, S("completed successfully")))
        testing_t_errorf_v(t, "CAST/PCT %s failure did not stop the program", r->name);
    else if (!strings_contains(out, want))
        testing_t_errorf_v(t, "CAST/PCT %s failure did not log the expected message",
                           r->name);
    else
        testing_t_logf_v(
            t, "CAST/PCT %s failed as expected and caused the program to exit",
            r->name);
    arena_free(&ar);
}

/* What the subtests of TestCASTFailures use, which has to last until the last
 * of them is done, as they can run after the test function has returned. */
typedef struct CastFailures {
    Arena ar;
    Str dir;
    CastRun runs[NUM_CASTS];
} CastFailures;

static void cast_failures_free(void *env) {
    CastFailures *f = env;
    if (f->dir.len > 0)
        os_remove_all(f->dir);
    arena_free(&f->ar);
    mem_free(heap_allocator(), f, sizeof *f, _Alignof(CastFailures));
}

static void TestCASTFailures(TestingT *t) {
    module_status(t);
    MUST_SUPPORT_FIPS140(t);
    SKIP_WITHOUT_EXEC(t);

    CastFailures *f = mem_alloc(heap_allocator(), sizeof *f, _Alignof(CastFailures));
    if (f == NULL)
        testing_t_fatalf_v(t, "out of memory");
    arena_init(&f->ar, NULL, 0);
    f->dir = BURROW_STR_EMPTY;
    testing_t_cleanup(t, BURROW_FN(Func, cast_failures_free, f));
    Str exe = fipstest_program(t, arena_allocator(&f->ar), &f->dir);
    if (exe.len == 0)
        return;

    for (size_t i = 0; i < NUM_CASTS; i++) {
        f->runs[i].out = BURROW_STR_EMPTY;
        f->runs[i].exe = exe;
        f->runs[i].name = all_casts[i];
        testing_t_run(t, str_from_cstr(all_casts[i]),
                      BURROW_FN(TestingTFunc, cast_failure, &f->runs[i]));
    }
}

#define TESTS(X)                                                                       \
    X(TestAllCASTs)                                                                    \
    X(TestConditionals)                                                                \
    X(TestCASTPasses)                                                                  \
    X(TestCASTFailures)

TESTING_MAIN(TESTS)
