/* Derived from Go's src/crypto/fips140/fips140.go and enforcement.go, and the
 * GODEBUG handling of src/crypto/internal/fips140/fips140.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/fips140.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"

#include "fips140_internal.h"

#include <stdint.h>
#include <string.h>

/* 1 once GODEBUG has been read and said off. Every other setting panics, so
 * there is nothing else to remember. */
static uint32_t fips140_off;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool fips140_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
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

static bool fips140_is(Str s, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)s.len == n && memcmp(s.p, lit, n) == 0;
}

void burrow__fips140_check(const char *godebug) {
    Str v = {0};
    if (godebug == NULL || !fips140_godebug(godebug, "fips140", &v) ||
        fips140_is(v, "off") || v.len == 0)
        return;
    if (fips140_is(v, "on") || fips140_is(v, "only") || fips140_is(v, "debug"))
        panic_str(BURROW_S("fips140: FIPS 140-3 mode is not supported by burrow"));
    /* A panic can outlive the frame that raised it, so the message goes in the
     * goroutine's error arena, which lasts as long as the goroutine does. */
    static const char head[] = "fips140: unknown GODEBUG setting fips140=";
    size_t hl = sizeof head - 1;
    char *msg = mem_alloc_nozero(error_allocator(), hl + (size_t)v.len, 1);
    if (msg == NULL)
        panic_str(BURROW_S("fips140: unknown GODEBUG setting fips140"));
    memcpy(msg, head, hl);
    memcpy(msg + hl, v.p, (size_t)v.len);
    panic_str(str_from_bytes(msg, (Int)hl + v.len));
}

/* Reads GODEBUG the first time, and panics, every time, unless it said off. */
static void fips140_load(void) {
    if (burrow__atomic_load_relaxed_u32(&fips140_off) != 0)
        return;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    burrow__fips140_check(v);
    burrow__atomic_store_relaxed_u32(&fips140_off, 1);
}

bool fips140_enabled(void) {
    fips140_load();
    return false;
}

Str fips140_version(void) {
    return BURROW_S("latest");
}

bool fips140_enforced(void) {
    fips140_load();
    return false;
}

void fips140_without_enforcement(Func f) {
    /* Go sets the bypass only when enforcement is on, and it never is. */
    f.f(f.env);
}
