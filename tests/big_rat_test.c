/* Derived from Go's src/math/big/rat_test.go, ratconv_test.go and
 * ratmarsh_test.go.
 * Go source: go1.27.1.
 *
 * Most of what Go checks is in tests/big_rat_test_gen.h, from
 * tools/gen-math-big-rat-tests.sh: every string in Go's own Rat tests through
 * SetString, random fractions through the arithmetic, and floats at the edges
 * of their ranges, each with the result Go's math/big gives. TestGoCases runs
 * them all, and runs every operation that sets a receiver again with the
 * receiver aliasing each operand and on an arena. The tests after it are about
 * burrow's API.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "big_rat_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static Arena ar;
static Alloc *a;

static Str S(const char *s) {
    return str_from_cstr(s);
}

static const char *cs(Str s) {
    char *b = mem_alloc(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(b, s.p, (size_t)s.len);
    b[s.len] = '\0';
    return b;
}

static void parse_int(BigInt *z, const char *hex, Int n) {
    bool ok;
    big_int_set_string(z, (Str){(const Byte *)hex, n}, 16, &ok);
    if (!ok) {
        fprintf(stderr, "bad operand %s\n", hex);
        abort();
    }
}

/* A Rat operand, "num/den" in hex, with the heap as its allocator. */
static void parse(BigRat *z, const char *s) {
    const char *slash = strchr(s, '/');
    if (slash == NULL) {
        fprintf(stderr, "bad operand %s\n", s);
        abort();
    }
    BigInt n = {0}, d = {0};
    parse_int(&n, s, (Int)(slash - s));
    parse_int(&d, slash + 1, (Int)strlen(slash + 1));
    big_rat_set_frac(z, &n, &d);
    big_int_free(&n);
    big_int_free(&d);
}

/* x the way the generator writes an operand. */
static const char *hx(BigRat *x) {
    return cs(fmt_sprintf_v(a, "%s/%s", big_int_text(big_rat_num(x), a, 16),
                            big_int_text(big_rat_denom(x), a, 16)));
}

static const char *str(const BigRat *x) {
    return cs(big_rat_string(x, a));
}

static uint64_t fnv64a(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++) {
        h ^= (Byte)*s;
        h *= 1099511628211ull;
    }
    return h;
}

/* Whether got is want, or hashes to it when want is a hash. */
static bool same(const char *got, const char *want) {
    if (want[0] == '#') {
        char buf[20];
        snprintf(buf, sizeof buf, "#%016llx", (unsigned long long)fnv64a(got));
        return strcmp(buf, want) == 0;
    }
    return strcmp(got, want) == 0;
}

static const char *hexbytes(Slice b) {
    char *s = mem_alloc(a, (size_t)b.len * 2 + 1, 1);
    for (Int i = 0; i < b.len; i++)
        snprintf(s + 2 * i, 3, "%02x", ((const Byte *)b.p)[i]);
    s[b.len * 2] = '\0';
    return s;
}

static Slice unhex(const char *s) {
    Int n = (Int)strlen(s) / 2;
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        ((Byte *)b.p)[i] = (Byte)v;
    }
    return b;
}

static const char *exact(bool e) {
    return e ? " exact" : " inexact";
}

typedef BigRat *(*BinOp)(BigRat *z, const BigRat *x, const BigRat *y);
typedef BigRat *(*UnOp)(BigRat *z, const BigRat *x);

/* A Rat on the arena that already holds a long fraction. */
static BigRat busy(void) {
    BigRat w = BIG_RAT(a);
    BigInt n = BIG_INT(a), d = BIG_INT(a);
    big_int_lsh(&n, big_int_set_int64(&n, 12345), 2000);
    big_int_lsh(&d, big_int_set_int64(&d, 3), 1000);
    big_int_add(&d, &d, big_new_int(a, 1));
    big_rat_set_frac(&w, &n, &d);
    return w;
}

static void want(TestingT *t, const RatCase *c, const char *how, const char *got) {
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s) %s= %s, want %s", c->op, c->a, c->b, how, got,
                           c->want);
}

/* Runs op four ways: into a fresh Rat, into x, into y, and into a Rat on an
 * arena that already holds a long number. */
static void check_binop(TestingT *t, const RatCase *c, BinOp op) {
    BigRat x = {0}, y = {0}, z = {0};
    parse(&x, c->a);
    parse(&y, c->b);
    want(t, c, "", str(op(&z, &x, &y)));

    BigRat x2 = {0};
    parse(&x2, c->a);
    want(t, c, "into x ", str(op(&x2, &x2, &y)));

    BigRat y2 = {0};
    parse(&y2, c->b);
    want(t, c, "into y ", str(op(&y2, &x, &y2)));

    BigRat w = busy();
    want(t, c, "on an arena ", str(op(&w, &x, &y)));

    big_rat_free(&x);
    big_rat_free(&y);
    big_rat_free(&z);
    big_rat_free(&x2);
    big_rat_free(&y2);
}

static void check_unop(TestingT *t, const RatCase *c, UnOp op) {
    BigRat x = {0}, z = {0};
    parse(&x, c->a);
    want(t, c, "", str(op(&z, &x)));
    BigRat w = busy();
    want(t, c, "on an arena ", str(op(&w, &x)));
    want(t, c, "into x ", str(op(&x, &x)));
    big_rat_free(&x);
    big_rat_free(&z);
}

static BigRat *sqr_op(BigRat *z, const BigRat *x) {
    return big_rat_mul(z, x, x);
}

static void check_case(TestingT *t, const RatCase *c) {
    const char *op = c->op;
    const char *got = NULL;
    BigRat x = {0}, z = {0};

    if (strcmp(op, "add") == 0) {
        check_binop(t, c, big_rat_add);
        return;
    } else if (strcmp(op, "sub") == 0) {
        check_binop(t, c, big_rat_sub);
        return;
    } else if (strcmp(op, "mul") == 0) {
        check_binop(t, c, big_rat_mul);
        return;
    } else if (strcmp(op, "quo") == 0) {
        check_binop(t, c, big_rat_quo);
        return;
    } else if (strcmp(op, "sqr") == 0) {
        check_unop(t, c, sqr_op);
        return;
    } else if (strcmp(op, "inv") == 0) {
        check_unop(t, c, big_rat_inv);
        return;
    } else if (strcmp(op, "neg") == 0) {
        check_unop(t, c, big_rat_neg);
        return;
    } else if (strcmp(op, "abs") == 0) {
        check_unop(t, c, big_rat_abs);
        return;
    } else if (strcmp(op, "cmp") == 0) {
        BigRat y = {0};
        parse(&x, c->a);
        parse(&y, c->b);
        got = cs(fmt_sprintf_v(a, "%d", big_rat_cmp(&x, &y)));
        big_rat_free(&y);
    } else if (strcmp(op, "setstring") == 0) {
        bool ok;
        BigRat *r = big_rat_set_string(&z, S(c->a), &ok);
        got = ok ? str(&z) : "fail";
        if (ok != (r != NULL))
            testing_t_errorf_v(t, "setstring(%q): ok and the result disagree", c->a);
        BigRat w = busy();
        big_rat_set_string(&w, S(c->a), &ok);
        want(t, c, "on an arena ", ok ? str(&w) : "fail");
    } else if (strcmp(op, "float64") == 0) {
        parse(&x, c->a);
        bool e;
        double f = big_rat_float64(&x, &e);
        uint64_t bits;
        memcpy(&bits, &f, sizeof bits);
        got = cs(fmt_sprintf_v(a, "%016x%s", bits, exact(e)));
    } else if (strcmp(op, "float32") == 0) {
        parse(&x, c->a);
        bool e;
        float f = big_rat_float32(&x, &e);
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        got = cs(fmt_sprintf_v(a, "%08x%s", bits, exact(e)));
    } else if (strcmp(op, "setfloat64") == 0) {
        uint64_t bits = strtoull(c->a, NULL, 16);
        double f;
        memcpy(&f, &bits, sizeof f);
        BigRat *r = big_rat_set_float64(&z, f);
        got = r == NULL ? "nil" : str(r);
    } else if (strcmp(op, "string") == 0) {
        parse(&x, c->a);
        got = str(&x);
    } else if (strcmp(op, "ratstring") == 0) {
        parse(&x, c->a);
        got = cs(big_rat_rat_string(&x, a));
    } else if (strcmp(op, "floatstring") == 0) {
        parse(&x, c->a);
        got = cs(big_rat_float_string(&x, a, (Int)strtol(c->b, NULL, 10)));
    } else if (strcmp(op, "floatprec") == 0) {
        parse(&x, c->a);
        bool e;
        Int n = big_rat_float_prec(&x, &e);
        got = cs(fmt_sprintf_v(a, "%d%s", n, exact(e)));
    } else if (strcmp(op, "gobencode") == 0) {
        parse(&x, c->a);
        Error err;
        got = hexbytes(big_rat_gob_encode(&x, a, &err));
        CHECK(BURROW_OK(err));
    } else if (strcmp(op, "marshaltext") == 0) {
        parse(&x, c->a);
        Error err;
        Slice b = big_rat_marshal_text(&x, a, &err);
        got = cs((Str){b.p, b.len});
        CHECK(BURROW_OK(err));
    } else if (strcmp(op, "gobdecode") == 0) {
        big_rat_set_frac64(&z, 1, 2);
        Error err = big_rat_gob_decode(&z, unhex(c->a));
        got = BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err)))
                                 : hx(&z);
    } else if (strcmp(op, "unmarshaltext") == 0) {
        big_rat_set_frac64(&z, 7, 9);
        Str s = S(c->a);
        Error err = big_rat_unmarshal_text(
            &z, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE));
        got = BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err)))
                                 : hx(&z);
    } else if (strcmp(op, "scan") == 0) {
        big_rat_set_int64(&z, 5);
        Any args[] = {BURROW_ANY(TYPE_BIG_RAT, &z)};
        Error err;
        fmt_sscanf(a, S(c->a), S(c->b), slice_from(args, 1, 1, TYPE_ANY), &err);
        got = BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err)))
                                 : hx(&z);
    } else if (strcmp(op, "setfrac") == 0) {
        BigInt n = {0}, d = {0};
        parse_int(&n, c->a, (Int)strlen(c->a));
        parse_int(&d, c->b, (Int)strlen(c->b));
        got = str(big_rat_set_frac(&z, &n, &d));
        /* with the receiver's own halves as the operands */
        big_int_set(&x.a, &n);
        big_int_set(&x.b, &d);
        want(t, c, "from its own halves ", str(big_rat_set_frac(&x, &x.a, &x.b)));
        big_int_set(&x.a, &d);
        big_int_set(&x.b, &n);
        if (big_int_sign(&n) != 0)
            want(t, c, "from its swapped halves ",
                 str(big_rat_set_frac(&x, &x.b, &x.a)));
        big_int_free(&n);
        big_int_free(&d);
    } else if (strcmp(op, "setfrac64") == 0) {
        got = str(
            big_rat_set_frac64(&z, strtoll(c->a, NULL, 10), strtoll(c->b, NULL, 10)));
    } else if (strcmp(op, "setint64") == 0) {
        got = str(big_rat_set_int64(&z, strtoll(c->a, NULL, 10)));
    } else if (strcmp(op, "setuint64") == 0) {
        got = str(big_rat_set_uint64(&z, strtoull(c->a, NULL, 10)));
    } else {
        testing_t_errorf_v(t, "unknown case %s", op);
        return;
    }
    want(t, c, "", got);
    big_rat_free(&x);
    big_rat_free(&z);
}

static void TestGoCases(TestingT *t) {
    Int n = (Int)(sizeof rat_cases / sizeof rat_cases[0]);
    for (Int i = 0; i < n; i++) {
        check_case(t, &rat_cases[i]);
        arena_reset(&ar);
    }
}

/* ------------------------------------------------------- burrow's own API */

/* Go's TestZeroRat. */
static void TestZeroValues(TestingT *t) {
    BigRat x = {0}, y = {0}, z = {0};
    big_rat_set_frac64(&y, 0, 42);
    CHECK_INT_EQ(big_rat_cmp(&x, &y), 0);
    CHECK_STR_EQ(str(&x), "0/1");
    CHECK_STR_EQ(cs(big_rat_rat_string(&x, a)), "0");
    big_rat_add(&z, &x, &y);
    CHECK_STR_EQ(str(&z), "0/1");
    big_rat_sub(&z, &x, &y);
    CHECK_STR_EQ(str(&z), "0/1");
    big_rat_mul(&z, &x, &y);
    CHECK_STR_EQ(str(&z), "0/1");
    CHECK(big_rat_is_int(&x));
    CHECK_INT_EQ(big_rat_sign(&x), 0);
    bool e;
    CHECK(big_rat_float64(&x, &e) == 0 && e);
    CHECK_INT_EQ(big_rat_float_prec(&x, &e), 0);
    CHECK(e);
    CHECK_STR_EQ(cs(big_rat_float_string(&x, a, 3)), "0.000");
    big_rat_free(&x);
    big_rat_free(&y);
    big_rat_free(&z);
    big_rat_free(NULL);
}

static void TestDenom(TestingT *t) {
    BigRat x = {0};
    /* a zero Rat's denominator is 1, and Denom makes it so */
    CHECK_INT_EQ(x.b.abs.len, 0);
    CHECK_STR_EQ(cs(big_int_string(big_rat_denom(&x), a)), "1");
    CHECK_STR_EQ(cs(big_int_string(big_rat_num(&x), a)), "0");
    /* both are x's own, as in Go */
    big_rat_set_frac64(&x, -6, 4);
    CHECK_STR_EQ(cs(big_int_string(big_rat_num(&x), a)), "-3");
    CHECK_STR_EQ(cs(big_int_string(big_rat_denom(&x), a)), "2");
    big_int_set_int64(big_rat_num(&x), 5);
    CHECK_STR_EQ(str(&x), "5/2");
    big_rat_free(&x);
}

/* Go's TestIssue820 and TestIssue3521, which set a Rat from the halves of
 * another. */
static void TestAliasedHalves(TestingT *t) {
    BigRat x = {0}, y = {0};
    big_rat_set_frac64(&x, 3, 7);
    big_rat_set_frac(&y, &x.b, &x.a);
    CHECK_STR_EQ(str(&y), "7/3");
    big_rat_set_frac(&x, &x.b, &x.a);
    CHECK_STR_EQ(str(&x), "7/3");
    big_rat_inv(&x, &x);
    CHECK_STR_EQ(str(&x), "3/7");
    big_rat_quo(&x, &x, &x);
    CHECK_STR_EQ(str(&x), "1/1");
    big_rat_free(&x);
    big_rat_free(&y);
}

/* Inv swaps the words of the halves. With a long numerator over a short
 * denominator neither fits in the other's buffer, and with a short one both
 * do. */
static void TestInvKeepsOwnMemory(TestingT *t) {
    BigRat x = BIG_RAT(a);
    BigInt n = BIG_INT(a);
    big_int_lsh(&n, big_int_set_int64(&n, 3), 500);
    big_rat_set_frac(&x, &n, big_new_int(a, 7));
    Str before = big_rat_string(&x, a);
    big_rat_inv(&x, &x);
    big_rat_inv(&x, &x);
    CHECK(str_eq(big_rat_string(&x, a), before));
    big_rat_set_frac64(&x, -5, 9);
    big_rat_inv(&x, &x);
    CHECK_STR_EQ(str(&x), "-9/5");
    /* and a result that lives on after the scratch memory is reused */
    BigRat y = {0};
    big_rat_inv(&y, &x);
    big_rat_mul(&x, &x, &x);
    CHECK_STR_EQ(str(&y), "-5/9");
    big_rat_free(&y);
}

static void TestNewRat(TestingT *t) {
    BigRat *x = big_new_rat(a, 10, -4);
    CHECK_STR_EQ(str(x), "-5/2");
    CHECK(x->a.a == a && x->b.a == a);
}

static void TestFormat(TestingT *t) {
    BigRat x = {0};
    big_rat_set_frac64(&x, 22, 7);
    Any args[] = {BURROW_ANY(TYPE_BIG_RAT, &x), BURROW_ANY(TYPE_BIG_RAT, &x)};
    CHECK_STR_EQ(cs(fmt_sprintf(a, S("%v %s"), slice_from(args, 2, 2, TYPE_ANY))),
                 "22/7 22/7");
    big_rat_free(&x);
}

static const char *panic_of(void (*f)(void)) {
    const char *volatile msg = NULL;
    BURROW_TRY {
        f();
    }
    BURROW_CATCH(r) {
        msg = cs(panic_text(r));
    }
    BURROW_TRY_END;
    return msg;
}

static void frac64_zero(void) {
    BigRat x = BIG_RAT(a);
    big_rat_set_frac64(&x, 1, 0);
}

static void frac_zero(void) {
    BigRat x = BIG_RAT(a);
    BigInt n = BIG_INT(a), d = BIG_INT(a);
    big_int_set_int64(&n, 1);
    big_rat_set_frac(&x, &n, &d);
}

static void quo_zero(void) {
    BigRat x = BIG_RAT(a), y = BIG_RAT(a);
    big_rat_set_int64(&x, 1);
    big_rat_quo(&x, &x, &y);
}

static void inv_zero(void) {
    BigRat x = BIG_RAT(a);
    big_rat_inv(&x, &x);
}

static void TestPanics(TestingT *t) {
    CHECK_STR_EQ(panic_of(frac64_zero), "division by zero");
    CHECK_STR_EQ(panic_of(frac_zero), "division by zero");
    CHECK_STR_EQ(panic_of(quo_zero), "division by zero");
    CHECK_STR_EQ(panic_of(inv_zero), "division by zero");
    BigRat x = {0};
    big_rat_set_frac64(&x, 1, 3);
    CHECK_STR_EQ(cs(big_rat_float_string(&x, a, 5)), "0.33333");
    big_rat_free(&x);
}

/* Go's TestRatGobEncoding: a round trip for every pair of its numerators and
 * denominators. */
static void TestGobRoundTrip(TestingT *t) {
    static const char *const nums[] = {
        "-141592653589793238462643383279502884197169399375105820974944592307816406286",
        "-1415926535897932384626433832795028841971",
        "-141592653589793",
        "-1",
        "0",
        "1",
        "141592653589793",
        "1415926535897932384626433832795028841971",
        "141592653589793238462643383279502884197169399375105820974944592307816406286",
    };
    static const char *const dens[] = {
        "1",
        "718281828459045",
        "7182818284590452353602874713526624977572",
        "718281828459045235360287471352662497757247093699959574966967627724076630353",
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        for (size_t j = 0; j < sizeof dens / sizeof dens[0]; j++) {
            BigRat x = BIG_RAT(a), y = BIG_RAT(a);
            const char *s = cs(fmt_sprintf_v(a, "%s/%s", S(nums[i]), S(dens[j])));
            CHECK(big_rat_set_string(&x, S(s), NULL) != NULL);
            Error err;
            Slice b = big_rat_gob_encode(&x, a, &err);
            CHECK(BURROW_OK(err));
            CHECK(BURROW_OK(big_rat_gob_decode(&y, b)));
            CHECK_INT_EQ(big_rat_cmp(&x, &y), 0);
        }
    }
}

static void worker(void *arg) {
    Str *out = arg;
    BigRat sum = {0}, term = {0};
    for (int64_t k = 1; k <= 300; k++)
        big_rat_add(&sum, &sum, big_rat_set_frac64(&term, 1, k * k));
    *out = big_rat_float_string(&sum, heap_allocator(), 40);
    big_rat_free(&sum);
    big_rat_free(&term);
}

/* Each thread has its own scratch memory: every thread sums the same
 * series at once. */
static void TestThreads(TestingT *t) {
    enum { N = 4 };
    burrow__Thread th[N];
    Str out[N];
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_start(&th[i], worker, &out[i], 0));
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_join(&th[i]));
    for (int i = 1; i < N; i++)
        CHECK(str_eq(out[i], out[0]));
    CHECK_STR_EQ(cs((Str){out[0].p, 6}), "1.6416");
    for (int i = 0; i < N; i++)
        mem_free(heap_allocator(), (void *)(uintptr_t)out[i].p, (size_t)out[i].len, 1);
}

static void setup(void) {
    arena_init(&ar, NULL, 1 << 16);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGoCases)                                                                     \
    X(TestZeroValues)                                                                  \
    X(TestDenom)                                                                       \
    X(TestAliasedHalves)                                                               \
    X(TestInvKeepsOwnMemory)                                                           \
    X(TestNewRat)                                                                      \
    X(TestFormat)                                                                      \
    X(TestPanics)                                                                      \
    X(TestGobRoundTrip)                                                                \
    X(TestThreads)

static int rat_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(rat_main, TESTS)
