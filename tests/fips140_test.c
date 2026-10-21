/* Derived from Go's src/crypto/fips140/fips140_test.go and
 * enforcement_test.go.
 * Go source: go1.27.1.
 *
 * Go's TestWithoutEnforcement runs itself again under GODEBUG=fips140=only
 * and checks that crypto/des is refused outside WithoutEnforcement and allowed
 * inside it. burrow's crypto/des does not look at fips140=only yet, so here the
 * test asks fips140_enforced, which is what crypto/des asks in Go. TestGODEBUG
 * is burrow's own: it goes through the settings that are accepted and the ones
 * that panic.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto/fips140.h"

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

static Str panic_message(const char *godebug, volatile Fips140Setting *got) {
    static char msg[128];
    volatile Int n = -1;
    BURROW_TRY {
        *got = burrow__fips140_setting(godebug);
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
        Fips140Setting setting;
        const char *panic;
    } tests[] = {
        {NULL, FIPS140_OFF, "no panic"},
        {"", FIPS140_OFF, "no panic"},
        {"fips140=", FIPS140_OFF, "no panic"},
        {"fips140=off", FIPS140_OFF, "no panic"},
        {"x509sha1=1,fips140=off", FIPS140_OFF, "no panic"},
        {"fips140=on,fips140=off", FIPS140_OFF, "no panic"},
        {"xfips140=on", FIPS140_OFF, "no panic"},
        {"fips140=on", FIPS140_ON, "no panic"},
        {"fips140=only", FIPS140_ONLY, "no panic"},
        {"fips140=debug", FIPS140_DEBUG, "no panic"},
        {"fips140=off,fips140=on", FIPS140_ON, "no panic"},
        {"x509sha1=1,fips140=only,y=2", FIPS140_ONLY, "no panic"},
        {"fips140=On", FIPS140_OFF, "fips140: unknown GODEBUG setting fips140=On"},
        {"fips140=1,x=2", FIPS140_OFF, "fips140: unknown GODEBUG setting fips140=1"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str godebug =
            str_from_cstr(tests[i].godebug == NULL ? "(unset)" : tests[i].godebug);
        volatile Fips140Setting got = FIPS140_OFF;
        Str msg = panic_message(tests[i].godebug, &got);
        Str want = str_from_cstr(tests[i].panic);
        if (!str_eq(msg, want))
            testing_t_errorf_v(t, "GODEBUG=%s: got %q, want %q", godebug, msg, want);
        else if (got != tests[i].setting)
            testing_t_errorf_v(t, "GODEBUG=%s: got setting %d, want %d", godebug,
                               (int)got, (int)tests[i].setting);
    }
}

static void TestVersion(TestingT *t) {
    Str v = fips140_version();
    if (!str_eq(v, BURROW_S("latest")))
        testing_t_errorf_v(t, "Version() = %q, want %q", v, BURROW_S("latest"));
}

/* Not in Go: isAllowed is the line that sets result, and asks fips140_enforced
 * instead of crypto/des. */
static void expect_allowed(TestingT *t, const char *why, bool expected) {
    bool result = !fips140_enforced();
    if (result != expected)
        testing_t_fatalf_v(t, "%s: expected: %t, got: %t", str_from_cstr(why), expected,
                           result);
}

static void disabled_inside(void *env) {
    expect_allowed(env, "inside WithoutEnforcement", true);
}

static void without_disabled(void *env, TestingT *t) {
    (void)env;
    expect_allowed(t, "before enforcement disabled", false);
    fips140_without_enforcement(BURROW_FN(Func, disabled_inside, t));
    /* make sure that bypass doesn't live on after returning */
    expect_allowed(t, "after WithoutEnforcement", false);
}

static void nested_inner(void *env) {
    expect_allowed(env, "inside nested WithoutEnforcement", true);
}

static void nested_outer(void *env) {
    fips140_without_enforcement(BURROW_FN(Func, nested_inner, env));
    expect_allowed(env, "inside nested WithoutEnforcement", true);
}

static void without_nested(void *env, TestingT *t) {
    (void)env;
    expect_allowed(t, "before enforcement bypass", false);
    fips140_without_enforcement(BURROW_FN(Func, nested_outer, t));
    expect_allowed(t, "after enforcement bypass", false);
}

static void inherit_send(void *env) {
    Chan *ch = env;
    bool allowed = !fips140_enforced();
    chan_send(ch, &allowed);
}

static void inherit_start(void *env) {
    go(BURROW_FN(Func, inherit_send, env));
}

static void without_goroutine(void *env, TestingT *t) {
    (void)env;
    Chan *ch = chan_make(heap_allocator(), TYPE_BOOL, 2);
    expect_allowed(t, "before enforcement bypass", false);
    fips140_without_enforcement(BURROW_FN(Func, inherit_start, ch));
    bool allowed = false;
    chan_recv(ch, &allowed);
    if (!allowed) {
        chan_free(ch);
        testing_t_fatalf_v(t, "goroutine didn't inherit enforcement bypass");
    }
    go(BURROW_FN(Func, inherit_send, ch));
    allowed = true;
    chan_recv(ch, &allowed);
    chan_free(ch);
    if (allowed)
        testing_t_fatalf_v(
            t, "goroutine inherited bypass after WithoutEnforcement return");
}

static void TestWithoutEnforcement(TestingT *t) {
    if (!fips140_enforced()) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        fipstest_rerun(t, arena_allocator(&ar), "only", NULL);
        arena_free(&ar);
        return;
    }

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
