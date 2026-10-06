/* Derived from Go's src/crypto/subtle/constant_time_test.go, xor_test.go and
 * dit_test.go.
 * Go source: go1.27.1.
 *
 * Go's testing/quick checks become loops: every pair of bytes for ByteEq, and
 * a fixed run of pseudo-random inputs for the rest, with the edges added in by
 * hand. BoundarySlices is the three page reservation bytes_boundary_test.c
 * uses. The DIT tests skip where the processor has no DIT, which is everywhere
 * but arm64, as they do in Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/subtle.h"
#include "burrow/dit.h"
#include "burrow/pal.h"

#include <stdint.h>
#include <string.h>

static uint64_t rng_state = 0x9e3779b97f4a7c15U;

/* splitmix64, which is plenty for test inputs and the same on every run. */
static uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15U);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9U;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebU;
    return z ^ (z >> 31);
}

static void fill(Byte *p, Int n) {
    for (Int i = 0; i < n; i++)
        p[i] = (Byte)rnd();
}

static Slice bs(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* ------------------------------------------------------------ constant time */

static void TestConstantTimeCompare(TestingT *t) {
    static Byte a11[] = {0x11};
    static Byte a12[] = {0x12};
    static Byte a1112[] = {0x11, 0x12};
    static const struct {
        Byte *a;
        Int alen;
        Byte *b;
        Int blen;
        Int out;
    } tests[] = {
        {NULL, 0, NULL, 0, 1}, {a11, 1, a11, 1, 1},   {a12, 1, a11, 1, 0},
        {a11, 1, a1112, 2, 0}, {a1112, 2, a11, 1, 0},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Int r = subtle_constant_time_compare(bs(tests[i].a, tests[i].alen),
                                             bs(tests[i].b, tests[i].blen));
        if (r != tests[i].out)
            testing_t_errorf_v(t, "#%d bad result (got %x, want %x)", i, r,
                               tests[i].out);
    }

    /* Every length up to a few words, differing in each place in turn. */
    Byte x[40], y[40];
    for (Int n = 0; n <= 40; n++) {
        fill(x, n);
        memcpy(y, x, (size_t)n);
        if (subtle_constant_time_compare(bs(x, n), bs(y, n)) != 1)
            testing_t_errorf_v(t, "n=%d: equal slices compared unequal", n);
        for (Int i = 0; i < n; i++) {
            y[i] ^= (Byte)(1 + rnd() % 255);
            if (subtle_constant_time_compare(bs(x, n), bs(y, n)) != 0)
                testing_t_errorf_v(t, "n=%d: slices differing at %d compared equal", n,
                                   i);
            y[i] = x[i];
        }
    }
}

static void TestConstantTimeByteEq(TestingT *t) {
    static const struct {
        uint8_t a, b;
        Int out;
    } tests[] = {
        {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0xff, 0xff, 1}, {0xff, 0xfe, 0},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Int r = subtle_constant_time_byte_eq(tests[i].a, tests[i].b);
        if (r != tests[i].out)
            testing_t_errorf_v(t, "#%d bad result (got %x, want %x)", i, r,
                               tests[i].out);
    }
    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            Int r = subtle_constant_time_byte_eq((uint8_t)a, (uint8_t)b);
            if (r != (a == b ? 1 : 0))
                testing_t_errorf_v(t, "ConstantTimeByteEq(%d, %d) = %d", a, b, r);
        }
    }
}

static void TestConstantTimeEq(TestingT *t) {
    static const int32_t edges[] = {0, 1, -1, 2, INT32_MAX, INT32_MIN, INT32_MIN + 1};
    Int ne = (Int)(sizeof edges / sizeof edges[0]);
    for (Int i = 0; i < ne; i++) {
        for (Int j = 0; j < ne; j++) {
            Int r = subtle_constant_time_eq(edges[i], edges[j]);
            if (r != (i == j ? 1 : 0))
                testing_t_errorf_v(t, "ConstantTimeEq(%d, %d) = %d", edges[i], edges[j],
                                   r);
        }
    }
    for (int i = 0; i < 10000; i++) {
        int32_t a = (int32_t)(uint32_t)rnd();
        int32_t b = i % 2 == 0 ? a : (int32_t)(uint32_t)rnd();
        Int r = subtle_constant_time_eq(a, b);
        if (r != (a == b ? 1 : 0))
            testing_t_errorf_v(t, "ConstantTimeEq(%d, %d) = %d", a, b, r);
    }
}

static void TestConstantTimeSelect(TestingT *t) {
    static const Int vs[] = {0, 1, 2, -1, BURROW_INT_MAX, BURROW_INT_MIN};
    for (Int i = 0; i < (Int)(sizeof vs / sizeof vs[0]); i++) {
        for (int k = 0; k < 100; k++) {
            Int x = (Int)rnd();
            Int y = (Int)rnd();
            Int want = vs[i] != 0 ? x : y;
            Int got = subtle_constant_time_select(vs[i], x, y);
            if (got != want)
                testing_t_errorf_v(t, "ConstantTimeSelect(%d, %d, %d) = %d, want %d",
                                   vs[i], x, y, got, want);
        }
    }
}

static void TestConstantTimeCopy(TestingT *t) {
    Byte x[64], y[64], want[64];
    for (int k = 0; k < 1000; k++) {
        Int n = (Int)(rnd() % 65);
        Int v = (Int)(rnd() & 1);
        fill(x, n);
        fill(y, n);
        memcpy(want, v == 1 ? y : x, (size_t)n);
        subtle_constant_time_copy(v, bs(x, n), bs(y, n));
        if (memcmp(x, want, (size_t)n) != 0)
            testing_t_errorf_v(t, "ConstantTimeCopy(%d, ...) of %d bytes is wrong", v,
                               n);
    }
}

static Str panic_message(void (*f)(void)) {
    static char msg[128];
    volatile Int n = -1;
    BURROW_TRY {
        f();
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

static void copy_lengths_differ(void) {
    Byte x[2] = {0}, y[3] = {0};
    subtle_constant_time_copy(1, bs(x, 2), bs(y, 3));
}

static void TestConstantTimeCopyPanic(TestingT *t) {
    Str got = panic_message(copy_lengths_differ);
    if (!str_eq(got, BURROW_S("subtle: slices have different lengths")))
        testing_t_errorf_v(t, "ConstantTimeCopy with different lengths: %q", got);
}

static void TestConstantTimeLessOrEq(TestingT *t) {
    static const struct {
        Int x, y, result;
    } tests[] = {
        {0, 0, 1},
        {1, 0, 0},
        {0, 1, 1},
        {10, 20, 1},
        {20, 10, 0},
        {10, 10, 1},
        /* Outside what Go promises, where Go's x <= y gives these. */
        {-1, 0, 1},
        {0, -1, 0},
        {BURROW_INT_MIN, BURROW_INT_MAX, 1},
        {BURROW_INT_MAX, BURROW_INT_MIN, 0},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Int result = subtle_constant_time_less_or_eq(tests[i].x, tests[i].y);
        if (result != tests[i].result)
            testing_t_errorf_v(t, "#%d: %d <= %d gave %d, expected %d", i, tests[i].x,
                               tests[i].y, result, tests[i].result);
    }
    for (int k = 0; k < 10000; k++) {
        Int x = (Int)rnd();
        Int y = k % 3 == 0 ? x : (Int)rnd();
        Int result = subtle_constant_time_less_or_eq(x, y);
        if (result != (x <= y ? 1 : 0))
            testing_t_errorf_v(t, "%d <= %d gave %d", x, y, result);
    }
}

/* ---------------------------------------------------------------------- xor */

static Int count_failures;

static void xor_check(TestingT *t, Int got, Int n, const Byte *have, const Byte *want,
                      Int len, Int ap, Int aq, Int ad) {
    if (memcmp(have, want, (size_t)len) != 0) {
        if (count_failures++ < 10)
            testing_t_errorf_v(t, "n=%d alignP=%d alignQ=%d alignD=%d: wrong bytes", n,
                               ap, aq, ad);
    } else if (got != n) {
        if (count_failures++ < 10)
            testing_t_errorf_v(t, "n=%d alignP=%d alignQ=%d alignD=%d: got %d, want %d",
                               n, ap, aq, ad, got, n);
    }
}

static void TestXORBytes(TestingT *t) {
    enum { MAX = 1024, EXTRA = 100 };
    static Byte pbuf[8 + MAX + EXTRA], qbuf[8 + MAX + EXTRA], d[8 + MAX + EXTRA];
    static Byte want[8 + MAX + EXTRA], p1[MAX], zero[MAX];
    count_failures = 0;
    for (Int n = 1; n <= MAX; n++) {
        if (n > 16 && testing_short())
            n += n >> 3;
        if (n > MAX)
            break;
        for (Int ap = 0; ap < 8; ap++) {
            for (Int aq = 0; aq < 8; aq++) {
                for (Int ad = 0; ad < 8; ad++) {
                    /* One of p and q is longer than n, so that the shorter
                     * decides, as the cap past the length does in Go. */
                    Byte *p = pbuf + ap;
                    Byte *q = qbuf + aq;
                    Int pn = (n & 1) != 0 ? n : n + EXTRA;
                    Int qn = (n & 1) != 0 ? n + EXTRA : n;
                    fill(p, pn);
                    fill(q, qn);
                    Int dn = ad + n + EXTRA;
                    fill(d, dn);

                    memcpy(want, d, (size_t)dn);
                    for (Int i = 0; i < n; i++)
                        want[ad + i] = (Byte)(p[i] ^ q[i]);

                    Int nn =
                        subtle_xor_bytes(bs(d + ad, dn - ad), bs(p, pn), bs(q, qn));
                    xor_check(t, nn, n, d, want, dn, ap, aq, ad);

                    memcpy(p1, p, (size_t)n);
                    nn = subtle_xor_bytes(bs(p, n), bs(p, n), bs(q, n));
                    xor_check(t, nn, n, p, want + ad, n, ap, aq, ad);
                    nn = subtle_xor_bytes(bs(q, n), bs(p1, n), bs(q, n));
                    xor_check(t, nn, n, q, want + ad, n, ap, aq, ad);

                    nn = subtle_xor_bytes(bs(p, n), bs(p, n), bs(p, n));
                    xor_check(t, nn, n, p, zero, n, ap, aq, ad);
                    nn = subtle_xor_bytes(bs(p1, n), bs(q, n), bs(q, n));
                    xor_check(t, nn, n, p1, zero, n, ap, aq, ad);
                }
            }
        }
    }
}

static void xor_nil_dst(void) {
    Byte x[1] = {0}, y[1] = {0};
    subtle_xor_bytes(slice_nil(TYPE_BYTE), bs(x, 1), bs(y, 1));
}

static void xor_short_dst(void) {
    Byte d[1] = {0}, x[2] = {0}, y[3] = {0};
    subtle_xor_bytes(bs(d, 1), bs(x, 2), bs(y, 3));
}

static void xor_overlap_x(void) {
    Byte x[3] = {0}, y[2] = {0};
    subtle_xor_bytes(bs(x, 3), bs(x + 1, 2), bs(y, 2));
}

static void xor_overlap_y(void) {
    Byte x[3] = {0}, y[2] = {0};
    subtle_xor_bytes(bs(x, 3), bs(y, 2), bs(x + 1, 2));
}

static void TestXorBytesPanic(TestingT *t) {
    static const struct {
        void (*f)(void);
        const char *want;
    } tests[] = {
        {xor_nil_dst, "subtle.XORBytes: dst too short"},
        {xor_short_dst, "subtle.XORBytes: dst too short"},
        {xor_overlap_x, "subtle.XORBytes: invalid overlap"},
        {xor_overlap_y, "subtle.XORBytes: invalid overlap"},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Str got = panic_message(tests[i].f);
        Str want = str_from_cstr(tests[i].want);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "expected panic(%q), but got %q", want, got);
    }
}

/* A committed page with nothing either side of it, so a byte read or written
 * past either end faults. */
static Byte *guarded_page(TestingT *t, Int *n) {
    int64_t pagesize = pal_page_size();
    PalErrno err = 0;
    Byte *b = (Byte *)pal_vm_reserve(3 * pagesize, &err);
    if (b == NULL)
        testing_t_fatalf_v(t, "reserve failed %s", pal_errno_string(err));
    if (!pal_vm_commit(b + pagesize, pagesize, &err))
        testing_t_fatalf_v(t, "commit failed %s", pal_errno_string(err));
    *n = (Int)pagesize;
    return b + pagesize;
}

static void TestXORBytesBoundary(TestingT *t) {
    static Byte safe[1000];
    Int pn;
    Byte *page = guarded_page(t, &pn);
    if (pn < 1000)
        testing_t_skipf_v(t, "page of %d bytes is smaller than 1000", pn);
    for (Int i = 1; i <= 1000; i++) {
        Byte *start = page;
        Byte *end = page + pn - i;
        subtle_xor_bytes(bs(end, i), bs(safe, 1000), bs(safe, i));
        subtle_xor_bytes(bs(start, i), bs(safe, 1000), bs(safe, i));
        subtle_xor_bytes(bs(safe, 1000), bs(start, i), bs(safe, 1000));
        subtle_xor_bytes(bs(safe, 1000), bs(end, i), bs(safe, 1000));
        subtle_xor_bytes(bs(safe, 1000), bs(safe, 1000), bs(start, i));
        subtle_xor_bytes(bs(safe, 1000), bs(safe, 1000), bs(end, i));
    }
    pal_vm_release(page - pn, 3 * (int64_t)pn, NULL);
}

/* ---------------------------------------------------------------------- dit */

typedef struct {
    TestingT *t;
} DitCase;

static void dit_inner(void *env) {
    DitCase *c = env;
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t,
                           "dit not enabled within nested WithDataIndependentTiming "
                           "closure");
}

static void dit_outer(void *env) {
    DitCase *c = env;
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t,
                           "dit not enabled within WithDataIndependentTiming closure");
    subtle_with_data_independent_timing(BURROW_FN(Func, dit_inner, c));
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t, "dit not enabled after return from nested "
                                 "WithDataIndependentTiming closure");
}

static void TestWithDataIndependentTiming(TestingT *t) {
    if (!burrow__dit_supported())
        testing_t_skip_v(t, "CPU does not support DIT");
    bool already = burrow__dit_enabled();
    DitCase c = {t};
    subtle_with_data_independent_timing(BURROW_FN(Func, dit_outer, &c));
    if (!already && burrow__dit_enabled())
        testing_t_errorf_v(
            t, "dit not unset after returning from WithDataIndependentTiming "
               "closure");
}

static void dit_panics(void *env) {
    DitCase *c = env;
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t,
                           "dit not enabled within WithDataIndependentTiming closure");
    panic_str(BURROW_S("bad"));
}

static void TestDITPanic(TestingT *t) {
    if (!burrow__dit_supported())
        testing_t_skip_v(t, "CPU does not support DIT");
    bool already = burrow__dit_enabled();
    DitCase c = {t};
    volatile bool panicked = false;
    BURROW_TRY {
        subtle_with_data_independent_timing(BURROW_FN(Func, dit_panics, &c));
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_fatalf_v(t, "didn't panic");
    if (!already && burrow__dit_enabled())
        testing_t_errorf_v(t, "DIT still enabled after panic inside of "
                              "WithDataIndependentTiming closure");
}

static SyncWaitGroup dit_wg;

static void dit_child(void *env) {
    DitCase *c = env;
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t, "DIT not enabled in new goroutine");
}

static void dit_parent(void *env) {
    DitCase *c = env;
    sync_wait_group_go(&dit_wg, BURROW_FN(Func, dit_child, c));
    sync_wait_group_wait(&dit_wg);
    if (!burrow__dit_enabled())
        testing_t_errorf_v(c->t, "dit unset after returning from goroutine started in "
                                 "WithDataIndependentTiming closure");
}

static void TestDITGoroutineInheritance(TestingT *t) {
    if (!burrow__dit_supported())
        testing_t_skip_v(t, "CPU does not support DIT");
    DitCase c = {t};
    subtle_with_data_independent_timing(BURROW_FN(Func, dit_parent, &c));
}

/* -------------------------------------------------------------- benchmarks */

static volatile uint8_t benchmark_global;

static void BenchmarkConstantTimeByteEq(TestingB *b) {
    uint8_t x = benchmark_global, y = 0;
    for (Int i = 0; i < testing_b_n(b); i++) {
        uint8_t nx = (uint8_t)subtle_constant_time_byte_eq(x, y);
        y = x;
        x = nx;
    }
    benchmark_global = x;
}

static void BenchmarkXORBytes8K(TestingB *b) {
    static Byte dst[8192], x[8192], y[8192];
    testing_b_set_bytes(b, 8192);
    for (Int i = 0; i < testing_b_n(b); i++)
        subtle_xor_bytes(bs(dst, 8192), bs(x, 8192), bs(y, 8192));
}

#define TESTS(X)                                                                       \
    X(TestConstantTimeCompare)                                                         \
    X(TestConstantTimeByteEq)                                                          \
    X(TestConstantTimeEq)                                                              \
    X(TestConstantTimeSelect)                                                          \
    X(TestConstantTimeCopy)                                                            \
    X(TestConstantTimeCopyPanic)                                                       \
    X(TestConstantTimeLessOrEq)                                                        \
    X(TestXORBytes)                                                                    \
    X(TestXorBytesPanic)                                                               \
    X(TestXORBytesBoundary)                                                            \
    X(TestWithDataIndependentTiming)                                                   \
    X(TestDITPanic)                                                                    \
    X(TestDITGoroutineInheritance)                                                     \
    X(BenchmarkConstantTimeByteEq)                                                     \
    X(BenchmarkXORBytes8K)

TESTING_MAIN(TESTS)
