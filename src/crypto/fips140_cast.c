/* Derived from Go's src/crypto/internal/fips140/cast.go and indicator.go, and
 * the handling of the fips140 setting in
 * src/crypto/internal/fips140/fips140.go.
 * Go source: go1.27.1.
 *
 * In Go, the init function of crypto/internal/fips140 reads GODEBUG and turns
 * FIPS 140-3 mode on, and the init functions of the module's other packages
 * then run their self-tests, each before anything can use its package.
 * crypto/internal/fips140/check runs the integrity check from its own init in
 * the same way. burrow has no init functions, so burrow__fips140_init does all
 * of that the first time anything uses the module, in the order Go's init
 * functions run: SHA-256, SHA-512 and SHA-3, which HMAC imports, then HMAC,
 * which the integrity check imports, then the integrity check, and then the
 * packages that import it.
 *
 * The self-tests Go runs the first time an algorithm is used, rather than at
 * init, are in the files of their packages, as in Go.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/fips140.h"

#include "fips140_module.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FIPS_MODE_OFF,
    FIPS_MODE_ON,
    FIPS_MODE_DEBUG,
};

/* Go's fips140.Enabled and fips140.debug in one, set by burrow__fips140_init. */
static uint32_t fips_mode;

/* GODEBUG's failfipscast, the whole name of the self-test to fail, which FIPS
 * 140-3 functional testing asks for. Empty for none, and for a value too long
 * to be the name of one. Written by burrow__fips140_init before it stores
 * fips_mode. */
static char fips_fail_cast[128];

/* Not in Go: how far burrow__fips140_init has got. */
enum {
    FIPS_INIT_NONE,
    FIPS_INIT_RUNNING,
    FIPS_INIT_DONE,
    FIPS_INIT_FAILED,
};

static uint32_t fips_init_state;

/* The goroutine running burrow__fips140_init, or the thread when it is not
 * running one, so that the module's own uses of itself during it go ahead. */
static void *fips_init_owner;
static BURROW_THREAD_LOCAL char fips_init_thread;

/* What burrow__fips140_init panicked with, for everything that uses the module
 * after it. In Go the program has ended by then. */
static char fips_init_failure[256];
static Int fips_init_failure_len;

/* The fips140 setting the module started with, and check.Verified. Written by
 * burrow__fips140_init before it stores FIPS_INIT_DONE. */
static Fips140Setting fips_setting;
static bool fips_verified;

CRYPTO_FIPS_TEXT bool burrow__fips140_enabled(void) {
    burrow__fips140_init();
    return burrow__atomic_load_acquire_u32(&fips_mode) != FIPS_MODE_OFF;
}

CRYPTO_FIPS_TEXT Fips140Setting burrow__fips140_mode(void) {
    burrow__fips140_init();
    return fips_setting;
}

CRYPTO_FIPS_TEXT bool burrow__fips140_verified(void) {
    burrow__fips140_init();
    return fips_verified;
}

enum {
    INDICATOR_UNSET,
    INDICATOR_FALSE,
    INDICATOR_TRUE,
};

CRYPTO_FIPS_TEXT void burrow__fips140_reset_service_indicator(void) {
    burrow__fips_set_indicator(INDICATOR_UNSET);
}

CRYPTO_FIPS_TEXT bool burrow__fips140_service_indicator(void) {
    return burrow__fips_get_indicator() == INDICATOR_TRUE;
}

CRYPTO_FIPS_TEXT void burrow__fips140_record_approved(void) {
    burrow__fips140_init();
    if (burrow__fips_get_indicator() == INDICATOR_UNSET)
        burrow__fips_set_indicator(INDICATOR_TRUE);
}

CRYPTO_FIPS_TEXT void burrow__fips140_record_non_approved(void) {
    burrow__fips140_init();
    burrow__fips_set_indicator(INDICATOR_FALSE);
}

/* Panics with what Go's panic says, in the error arena, which lasts as long as
 * the goroutine does, so that a recover can still read it. */
CRYPTO_FIPS_TEXT BURROW_NORETURN static void fips_panic2(const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b);
    char *msg = mem_alloc_nozero(error_allocator(), al + bl, 1);
    if (msg == NULL)
        panic_str(str_from_bytes(a, (Int)al));
    memcpy(msg, a, al);
    memcpy(msg + al, b, bl);
    panic_str(str_from_bytes(msg, (Int)(al + bl)));
}

/* The fatal error Go's fatal gives, "FIPS 140-3 self-test failed: " and the
 * name and the error, which ends the program. */
CRYPTO_FIPS_TEXT BURROW_NORETURN static void fips_fatal(const char *name, Error err) {
    static const char head[] = "FIPS 140-3 self-test failed: ";
    Str e = error_text(err);
    char msg[512];
    int n = snprintf(msg, sizeof msg, "%s%s: %.*s", head, name, (int)e.len,
                     (const char *)e.p);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof msg)
        n = (int)sizeof msg - 1;
    runtime_throw(str_from_bytes(msg, n));
}

CRYPTO_FIPS_TEXT static void fips_run(const char *name, FipsSelfTest f, void *arg,
                                      const char *simulated, const char *passed) {
    if (strpbrk(name, ",#=:") != NULL)
        fips_panic2("fips: invalid self-test name: ", name);
    burrow__fips140_init();
    uint32_t mode = burrow__atomic_load_acquire_u32(&fips_mode);
    if (mode == FIPS_MODE_OFF)
        return;

    Error err = f(arg);
    if (strcmp(name, fips_fail_cast) == 0)
        err = errors_new(error_allocator(), str_from_cstr(simulated));
    if (BURROW_FAILED(err))
        fips_fatal(name, err);
    if (mode == FIPS_MODE_DEBUG) {
        fprintf(stderr, "%s%s\n", passed, name);
        fflush(stderr);
    }
}

CRYPTO_FIPS_TEXT void burrow__fips140_cast(const char *name, FipsSelfTest f,
                                           void *arg) {
    fips_run(name, f, arg, "simulated CAST failure", "FIPS 140-3 self-test passed: ");
}

CRYPTO_FIPS_TEXT void burrow__fips140_pct(const char *name, FipsSelfTest f, void *arg) {
    fips_run(name, f, arg, "simulated PCT failure", "FIPS 140-3 PCT passed: ");
}

/* GODEBUG, or NULL when it is not set. */
CRYPTO_FIPS_TEXT static const char *fips_godebug_env(void) {
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0)
            return *env + 8;
    }
    return NULL;
}

/* The value of key in godebug, the last one when it is there twice. */
CRYPTO_FIPS_TEXT static bool fips_godebug(const char *godebug, const char *key,
                                          Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = godebug;
    while (p != NULL && *p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

/* Reads failfipscast from godebug into fips_fail_cast, the last one when it is
 * there twice, as godebug.Value does. */
CRYPTO_FIPS_TEXT static void fips_read_fail_cast(const char *godebug) {
    Str v = {0};
    fips_fail_cast[0] = '\0';
    if (fips_godebug(godebug, "failfipscast", &v) &&
        (size_t)v.len < sizeof fips_fail_cast) {
        memcpy(fips_fail_cast, v.p, (size_t)v.len);
        fips_fail_cast[v.len] = '\0';
    }
}

CRYPTO_FIPS_TEXT static bool fips_is(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)s.len == n && memcmp(s.p, lit, n) == 0;
}

/* The switch in Go's init function of crypto/internal/fips140, without the
 * panic: -1, with the value in *bad, for a value it does not know. */
CRYPTO_FIPS_TEXT static int fips_parse_setting(const char *godebug, Str *bad) {
    Str v = {0};
    if (!fips_godebug(godebug, "fips140", &v) || v.len == 0 || fips_is(v, "off"))
        return FIPS140_OFF;
    if (fips_is(v, "on"))
        return FIPS140_ON;
    if (fips_is(v, "only"))
        return FIPS140_ONLY;
    if (fips_is(v, "debug"))
        return FIPS140_DEBUG;
    *bad = v;
    return -1;
}

static const char fips_unknown[] = "fips140: unknown GODEBUG setting fips140=";

CRYPTO_FIPS_TEXT Fips140Setting burrow__fips140_setting(const char *godebug) {
    Str bad = {0};
    int setting = fips_parse_setting(godebug, &bad);
    if (setting >= 0)
        return (Fips140Setting)setting;
    /* A panic can outlive the frame that raised it, so the message goes in the
     * goroutine's error arena, which lasts as long as the goroutine does. */
    size_t hl = sizeof fips_unknown - 1;
    char *msg = mem_alloc_nozero(error_allocator(), hl + (size_t)bad.len, 1);
    if (msg == NULL)
        panic_str(BURROW_S("fips140: unknown GODEBUG setting fips140"));
    memcpy(msg, fips_unknown, hl);
    memcpy(msg + hl, bad.p, (size_t)bad.len);
    panic_str(str_from_bytes(msg, (Int)hl + bad.len));
}

/* Panics with a and then b, after leaving the message for whatever uses the
 * module next, which panics with it too. */
CRYPTO_FIPS_TEXT BURROW_NORETURN static void fips_init_fail(const char *a, Str b) {
    size_t al = strlen(a), bl = (size_t)b.len;
    if (al > sizeof fips_init_failure)
        al = sizeof fips_init_failure;
    if (bl > sizeof fips_init_failure - al)
        bl = sizeof fips_init_failure - al;
    memcpy(fips_init_failure, a, al);
    if (bl > 0)
        memcpy(fips_init_failure + al, b.p, bl);
    fips_init_failure_len = (Int)(al + bl);
    burrow__atomic_store_release_u32(&fips_init_state, FIPS_INIT_FAILED);
    panic_str(str_from_bytes(fips_init_failure, fips_init_failure_len));
}

/* Go's init functions, from the one of crypto/internal/fips140 on. */
CRYPTO_FIPS_TEXT static void fips_init_run(void) {
    const char *godebug = fips_godebug_env();
    Str bad = {0};
    int setting = fips_parse_setting(godebug, &bad);
    if (setting < 0)
        fips_init_fail(fips_unknown, bad);
    fips_setting = (Fips140Setting)setting;
    if (setting == FIPS140_OFF)
        return;

    bool debug = setting == FIPS140_DEBUG;
    fips_read_fail_cast(godebug);
    burrow__atomic_store_release_u32(&fips_mode,
                                     debug ? FIPS_MODE_DEBUG : FIPS_MODE_ON);

    burrow__fips140_cast("SHA2-256", burrow__sha256_cast, NULL);
    burrow__fips140_cast("SHA2-512", burrow__sha512_cast, NULL);
    burrow__fips140_cast("cSHAKE128", burrow__sha3_cast, NULL);
    burrow__fips140_cast("HMAC-SHA2-256", burrow__hmac_cast, NULL);

    /* The init function of crypto/internal/fips140/check. */
    const char *why = burrow__fips140_supported();
    if (why != NULL)
        fips_init_fail("fips140: ", str_from_cstr(why));
    why = burrow__fips140_verify();
    if (why != NULL)
        fips_init_fail("fips140: ", str_from_cstr(why));
    fips_verified = true;
    if (debug) {
        fputs("fips140: verified code+data\n", stderr);
        fflush(stderr);
    }

    /* Go runs a package's self-test only when the package is in the program,
     * because only then does its init function run. HKDF, PBKDF2 and the TLS
     * key schedules are in packages the module doesn't need, which an
     * amalgamation can leave out, and then so are their self-tests. The rest
     * are in packages this one needs, so they are always in. */
#if !defined(BURROW_OMIT_CRYPTO_HKDF)
    burrow__fips140_cast("HKDF-SHA2-256", burrow__hkdf_cast, NULL);
#endif
#if !defined(BURROW_OMIT_CRYPTO_PBKDF2)
    burrow__fips140_cast("PBKDF2", burrow__pbkdf2_cast, NULL);
#endif
    burrow__fips140_cast("AES-CBC", burrow__aes_cast, NULL);
    burrow__fips140_cast("CounterKDF", burrow__ctrkdf_cast, NULL);
    burrow__fips140_cast("CTR_DRBG", burrow__drbg_cast, NULL);
#if !defined(BURROW_OMIT_CRYPTO_TLS)
    burrow__fips140_cast("TLSv1.2-SHA2-256", burrow__tls12_cast, NULL);
    burrow__fips140_cast("TLSv1.3-SHA2-256", burrow__tls13_cast, NULL);
#endif
}

CRYPTO_FIPS_TEXT void burrow__fips140_init(void) {
    uint32_t state = burrow__atomic_load_acquire_u32(&fips_init_state);
    if (state == FIPS_INIT_DONE)
        return;
    burrow__G *g = burrow__curg();
    void *self = g != NULL ? (void *)g : (void *)&fips_init_thread;
    for (;;) {
        if (state == FIPS_INIT_NONE &&
            burrow__atomic_cas_u32(&fips_init_state, &state, FIPS_INIT_RUNNING)) {
            burrow__atomic_store_release_ptr(&fips_init_owner, self);
            /* The self-tests use the module, which records what they use, and
             * none of that is the caller's. */
            uint8_t indicator = burrow__fips_get_indicator();
            fips_init_run();
            burrow__fips_set_indicator(indicator);
            burrow__atomic_store_release_u32(&fips_init_state, FIPS_INIT_DONE);
            return;
        }
        state = burrow__atomic_load_acquire_u32(&fips_init_state);
        if (state == FIPS_INIT_DONE)
            return;
        if (state == FIPS_INIT_FAILED)
            panic_str(str_from_bytes(fips_init_failure, fips_init_failure_len));
        if (state == FIPS_INIT_RUNNING &&
            burrow__atomic_load_acquire_ptr(&fips_init_owner) == self)
            return;
        if (state != FIPS_INIT_RUNNING)
            continue;
        if (g != NULL)
            runtime_gosched();
        else
            pal_thread_yield();
    }
}
