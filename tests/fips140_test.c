/* Derived from Go's src/crypto/fips140/fips140_test.go and
 * enforcement_test.go.
 * Go source: go1.27.1.
 *
 * Go's TestWithoutEnforcement runs itself again under GODEBUG=fips140=only
 * and checks that crypto/des is refused outside WithoutEnforcement and allowed
 * inside it. burrow has no FIPS mode, so that setting panics, and what is left
 * to check is that f runs, once, nested or not, and that enforcement is off
 * before, inside and after. The goroutine inheritance subtest checks the same
 * from a goroutine started inside f. TestGODEBUG is burrow's own: it goes
 * through the settings that are accepted and the ones that panic.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/fips140.h"

#include "../src/crypto/fips140_internal.h"

#include <string.h>

static void TestImmutableGODEBUG(TestingT *t) {
    bool enabled = fips140_enabled();
    bool found = false;
    Str old = os_lookup_env(error_allocator(), BURROW_S("GODEBUG"), &found);

    static const char *const tests[] = {"fips140=off", "fips140=on", "fips140=", ""};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str v = str_from_cstr(tests[i]);
        if (BURROW_FAILED(os_setenv(BURROW_S("GODEBUG"), v)))
            testing_t_fatalf_v(t, "Setenv(GODEBUG, %q) failed", v);
        if (fips140_enabled() != enabled)
            testing_t_errorf_v(t, "Enabled() changed after setting GODEBUG=%s", v);
    }

    if (found)
        (void)os_setenv(BURROW_S("GODEBUG"), old);
    else
        (void)os_unsetenv(BURROW_S("GODEBUG"));
}

static Str panic_message(const char *godebug) {
    static char msg[128];
    volatile Int n = -1;
    BURROW_TRY {
        burrow__fips140_check(godebug);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof msg ? s.len : (Int)sizeof msg;
        memcpy(msg, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0)
        return BURROW_S("no panic");
    return str_from_bytes((const Byte *)msg, n);
}

static void TestGODEBUG(TestingT *t) {
    static const struct {
        const char *godebug;
        const char *want;
    } tests[] = {
        {NULL, "no panic"},
        {"", "no panic"},
        {"fips140=", "no panic"},
        {"fips140=off", "no panic"},
        {"x509sha1=1,fips140=off", "no panic"},
        {"fips140=on,fips140=off", "no panic"},
        {"xfips140=on", "no panic"},
        {"fips140=on", "fips140: FIPS 140-3 mode is not supported by burrow"},
        {"fips140=only", "fips140: FIPS 140-3 mode is not supported by burrow"},
        {"fips140=debug", "fips140: FIPS 140-3 mode is not supported by burrow"},
        {"fips140=off,fips140=on",
         "fips140: FIPS 140-3 mode is not supported by burrow"},
        {"fips140=On", "fips140: unknown GODEBUG setting fips140=On"},
        {"fips140=1,x=2", "fips140: unknown GODEBUG setting fips140=1"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = panic_message(tests[i].godebug);
        Str want = str_from_cstr(tests[i].want);
        if (!str_eq(got, want))
            testing_t_errorf_v(
                t, "GODEBUG=%s: got %q, want %q",
                str_from_cstr(tests[i].godebug == NULL ? "(unset)" : tests[i].godebug),
                got, want);
    }
}

static void TestVersion(TestingT *t) {
    Str v = fips140_version();
    if (!str_eq(v, BURROW_S("latest")))
        testing_t_errorf_v(t, "Version() = %q, want %q", v, BURROW_S("latest"));
}

typedef struct {
    TestingT *t;
    Int calls;
} Bypass;

static void expect_not_enforced(TestingT *t, const char *why) {
    if (fips140_enforced())
        testing_t_fatalf_v(t, "%s: enforcement is on", str_from_cstr(why));
}

static void bypass_inner(void *env) {
    Bypass *b = env;
    b->calls++;
    expect_not_enforced(b->t, "inside nested WithoutEnforcement");
}

static void bypass_outer(void *env) {
    Bypass *b = env;
    b->calls++;
    fips140_without_enforcement(BURROW_FN(Func, bypass_inner, b));
    expect_not_enforced(b->t, "inside nested WithoutEnforcement");
}

static void bypass_once(void *env) {
    Bypass *b = env;
    b->calls++;
    expect_not_enforced(b->t, "inside WithoutEnforcement");
}

static void without_disabled(void *env, TestingT *t) {
    (void)env;
    Bypass b = {t, 0};
    expect_not_enforced(t, "before enforcement disabled");
    fips140_without_enforcement(BURROW_FN(Func, bypass_once, &b));
    expect_not_enforced(t, "after WithoutEnforcement");
    if (b.calls != 1)
        testing_t_errorf_v(t, "f ran %d times, want 1", b.calls);
}

static void without_nested(void *env, TestingT *t) {
    (void)env;
    Bypass b = {t, 0};
    expect_not_enforced(t, "before enforcement bypass");
    fips140_without_enforcement(BURROW_FN(Func, bypass_outer, &b));
    expect_not_enforced(t, "after enforcement bypass");
    if (b.calls != 2)
        testing_t_errorf_v(t, "f ran %d times, want 2", b.calls);
}

static void inherit_send(void *env) {
    Chan *ch = env;
    bool enforced = fips140_enforced();
    chan_send(ch, &enforced);
}

static void inherit_start(void *env) {
    go(BURROW_FN(Func, inherit_send, env));
}

static void without_goroutine(void *env, TestingT *t) {
    (void)env;
    Chan *ch = chan_make(heap_allocator(), TYPE_BOOL, 2);
    expect_not_enforced(t, "before enforcement bypass");
    fips140_without_enforcement(BURROW_FN(Func, inherit_start, ch));
    bool enforced = true;
    chan_recv(ch, &enforced);
    if (enforced)
        testing_t_fatalf_v(t,
                           "goroutine started inside WithoutEnforcement is enforced");
    go(BURROW_FN(Func, inherit_send, ch));
    enforced = true;
    chan_recv(ch, &enforced);
    chan_free(ch);
    if (enforced)
        testing_t_fatalf_v(t, "goroutine started after WithoutEnforcement is enforced");
}

static void TestWithoutEnforcement(TestingT *t) {
    testing_t_run(t, BURROW_S("Disabled"),
                  BURROW_FN(TestingTFunc, without_disabled, NULL));
    testing_t_run(t, BURROW_S("Nested"), BURROW_FN(TestingTFunc, without_nested, NULL));
    testing_t_run(t, BURROW_S("GoroutineInherit"),
                  BURROW_FN(TestingTFunc, without_goroutine, NULL));
}

#define TESTS(X)                                                                       \
    X(TestImmutableGODEBUG)                                                            \
    X(TestGODEBUG)                                                                     \
    X(TestVersion)                                                                     \
    X(TestWithoutEnforcement)

TESTING_MAIN(TESTS)
