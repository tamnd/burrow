/* Derived from Go's src/crypto/internal/fips140/cast.go and indicator.go, and
 * the handling of the fips140 setting in
 * src/crypto/internal/fips140/fips140.go.
 * Go source: go1.27.1.
 *
 * In Go, the init function of crypto/internal/fips140 reads GODEBUG and turns
 * FIPS 140-3 mode on, and the init functions of the module's other packages
 * then run their self-tests, each before anything can use its package.
 * crypto/internal/fips140/check runs the integrity check from its own init in
 * the same way. burrow has no init functions, so burrow__fips140_force_on does
 * all of that, in the order Go's init functions run: SHA-256, SHA-512 and
 * SHA-3, which HMAC imports, then HMAC, which the integrity check imports, then
 * the integrity check, and then the packages that import it.
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

/* Go's fips140.Enabled and fips140.debug in one, set by
 * burrow__fips140_force_on. */
static uint32_t fips_mode;

/* GODEBUG's failfipscast, the whole name of the self-test to fail, which FIPS
 * 140-3 functional testing asks for. Empty for none, and for a value too long
 * to be the name of one. Written by burrow__fips140_force_on before it stores
 * fips_mode. */
static char fips_fail_cast[128];

CRYPTO_FIPS_TEXT bool burrow__fips140_enabled(void) {
    return burrow__atomic_load_acquire_u32(&fips_mode) != FIPS_MODE_OFF;
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
    if (burrow__fips_get_indicator() == INDICATOR_UNSET)
        burrow__fips_set_indicator(INDICATOR_TRUE);
}

CRYPTO_FIPS_TEXT void burrow__fips140_record_non_approved(void) {
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

/* Reads failfipscast from GODEBUG into fips_fail_cast, the last one when it is
 * there twice, as godebug.Value does. */
CRYPTO_FIPS_TEXT static void fips_read_fail_cast(void) {
    static const char key[] = "failfipscast=";
    size_t kl = sizeof key - 1;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    fips_fail_cast[0] = '\0';
    for (const char *p = v; p != NULL && *p != '\0';) {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        size_t n = (size_t)(end - p);
        if (n >= kl && memcmp(p, key, kl) == 0) {
            n -= kl;
            if (n < sizeof fips_fail_cast) {
                memcpy(fips_fail_cast, p + kl, n);
                fips_fail_cast[n] = '\0';
            } else {
                fips_fail_cast[0] = '\0';
            }
        }
        p = *end == ',' ? end + 1 : end;
    }
}

CRYPTO_FIPS_TEXT void burrow__fips140_force_on(bool debug) {
    fips_read_fail_cast();
    burrow__atomic_store_release_u32(&fips_mode,
                                     debug ? FIPS_MODE_DEBUG : FIPS_MODE_ON);

    burrow__fips140_cast("SHA2-256", burrow__sha256_cast, NULL);
    burrow__fips140_cast("SHA2-512", burrow__sha512_cast, NULL);
    burrow__fips140_cast("cSHAKE128", burrow__sha3_cast, NULL);
    burrow__fips140_cast("HMAC-SHA2-256", burrow__hmac_cast, NULL);

    /* The init function of crypto/internal/fips140/check. */
    const char *why = burrow__fips140_supported();
    if (why != NULL)
        fips_panic2("fips140: ", why);
    why = burrow__fips140_verify();
    if (why != NULL)
        fips_panic2("fips140: ", why);
    if (debug) {
        fputs("fips140: verified code+data\n", stderr);
        fflush(stderr);
    }

    burrow__fips140_cast("HKDF-SHA2-256", burrow__hkdf_cast, NULL);
    burrow__fips140_cast("PBKDF2", burrow__pbkdf2_cast, NULL);
    burrow__fips140_cast("AES-CBC", burrow__aes_cast, NULL);
    burrow__fips140_cast("CounterKDF", burrow__ctrkdf_cast, NULL);
    burrow__fips140_cast("CTR_DRBG", burrow__drbg_cast, NULL);
    burrow__fips140_cast("TLSv1.2-SHA2-256", burrow__tls12_cast, NULL);
    burrow__fips140_cast("TLSv1.3-SHA2-256", burrow__tls13_cast, NULL);
}
