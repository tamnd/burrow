/* Derived from Go's src/math/cmplx/cmath_test.go and huge_test.go.
 * Go source: go1.27.1.
 *
 * The tables live in tests/cmplx_test_gen.h, written by
 * tools/gen-cmplx-tests.sh from Go's own, and the checks here are Go's with the
 * same tolerances. Go repeats one block of special case checks in every test,
 * with the conjugate and the negation of each input, and here that block is
 * check_sc with flags saying which symmetries the function has. On top of
 * Go's tests, TestMatchesGo compares a hash of every function's bits over
 * sixteen thousand inputs with the one Go gave, as tests/math_test.c does for
 * math.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/math.h"
#include "burrow/math/cmplx.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

/* A complex128 as the bits of its two parts. */
typedef struct Cb {
    uint64_t re, im;
} Cb;

#include "cmplx_test_gen.h"

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static double F(uint64_t b) {
    return math_float64frombits(b);
}

static Complex128 Z(Cb c) {
    Complex128 z = {F(c.re), F(c.im)};
    return z;
}

static Complex128 C(double re, double im) {
    Complex128 z = {re, im};
    return z;
}

static bool tolerance(double a, double b, double e) {
    double d = a - b;
    if (d < 0)
        d = -d;
    /* b is the expected value and a the actual one, so the tolerance is a
     * fraction of b. */
    if (b != 0) {
        e = e * b;
        if (e < 0)
            e = -e;
    }
    return d < e;
}

static bool veryclose(double a, double b) {
    return tolerance(a, b, 4e-16);
}

static bool alike(double a, double b) {
    if (math_is_nan(a) && math_is_nan(b))
        return true;
    if (a == b)
        return math_signbit(a) == math_signbit(b);
    return false;
}

static bool c_tolerance(Complex128 a, Complex128 b, double e) {
    double d = cmplx_abs(cmplx_sub(a, b));
    if (!cmplx_eq(b, C(0, 0))) {
        e = e * cmplx_abs(b);
        if (e < 0)
            e = -e;
    }
    return d < e;
}

static bool c_soclose(Complex128 a, Complex128 b, double e) {
    return c_tolerance(a, b, e);
}
static bool c_veryclose(Complex128 a, Complex128 b) {
    return c_tolerance(a, b, 4e-16);
}

/* Special cases that should match exactly. Other cases are multiples of Pi
 * that may not be last bit identical on all platforms. */
static bool is_exact(double x) {
    return math_is_nan(x) || math_is_inf(x, 0) || x == 0 || x == 1 || x == -1;
}

static bool c_alike(Complex128 a, Complex128 b) {
    /* Non-exact special cases may have errors in the last place. */
    bool re = is_exact(b.re) ? alike(a.re, b.re) : veryclose(a.re, b.re);
    bool im = is_exact(b.im) ? alike(a.im, b.im) : veryclose(a.im, b.im);
    return re && im;
}

typedef Complex128 (*Fn)(Complex128);

/* The symmetries check_sc tries on each special case besides the case
 * itself: f(conj(z)) == conj(f(z)), which every function but Exp's shape has,
 * and f(-z) == -f(z) for the odd ones or f(-z) == f(z) for the even ones. */
enum { SYM_CONJ = 1, SYM_ODD = 2, SYM_EVEN = 4 };

static void check_sc(TestingT *t, const char *name, Fn fn, const Cb (*sc)[2], Int n,
                     int sym) {
    for (Int i = 0; i < n; i++) {
        Complex128 in = Z(sc[i][0]), want = Z(sc[i][1]);
        Complex128 f = fn(in);
        if (!c_alike(want, f))
            testing_t_errorf_v(t, "%s(%g) = %g, want %g", name, in, f, want);
        /* Negating NaN is undefined with regard to the sign bit produced. */
        if (math_is_nan(in.im) || math_is_nan(want.im))
            continue;
        if (sym & SYM_CONJ) {
            f = fn(cmplx_conj(in));
            if (!c_alike(cmplx_conj(want), f) && !c_alike(in, cmplx_conj(in)))
                testing_t_errorf_v(t, "%s(%g) = %g, want %g", name, cmplx_conj(in), f,
                                   cmplx_conj(want));
        }
        if (math_is_nan(in.re) || math_is_nan(want.re))
            continue;
        if (sym & (SYM_ODD | SYM_EVEN)) {
            Complex128 w = sym & SYM_ODD ? cmplx_neg(want) : want;
            f = fn(cmplx_neg(in));
            if (!c_alike(w, f) && !c_alike(in, cmplx_neg(in)))
                testing_t_errorf_v(t, "%s(%g) = %g, want %g", name, cmplx_neg(in), f,
                                   w);
        }
    }
}

/* Points on each axis at |z| > 1, approached from both sides. Every branch
 * cut of these functions goes through one of them, and the answer on either
 * side of a point must be the same. */
static void check_branch(TestingT *t, const char *name, Fn fn) {
    for (Int i = 0; i < LEN(t_branchPoints); i++) {
        Complex128 p0 = Z(t_branchPoints[i][0]), p1 = Z(t_branchPoints[i][1]);
        Complex128 f0 = fn(p0), f1 = fn(p1);
        if (!c_veryclose(f0, f1))
            testing_t_errorf_v(t, "%s(%g) not continuous, got %g want %g", name, p0, f0,
                               f1);
    }
}

/* The inputs in vc against a table of the answers, to a tolerance. */
static void check_vc(TestingT *t, const char *name, Fn fn, const Cb *want, double e) {
    for (Int i = 0; i < LEN(t_vc); i++) {
        Complex128 f = fn(Z(t_vc[i]));
        if (!c_soclose(Z(want[i]), f, e))
            testing_t_errorf_v(t, "%s(%g) = %g, want %g", name, Z(t_vc[i]), f,
                               Z(want[i]));
    }
}

static void TestAbs(TestingT *t) {
    for (Int i = 0; i < LEN(t_vc); i++) {
        double f = cmplx_abs(Z(t_vc[i]));
        if (!veryclose(F(t_abs[i]), f))
            testing_t_errorf_v(t, "Abs(%g) = %g, want %g", Z(t_vc[i]), f, F(t_abs[i]));
    }
    for (Int i = 0; i < LEN(t_vcAbsSC); i++) {
        double f = cmplx_abs(Z(t_vcAbsSC[i]));
        if (!alike(F(t_absSC[i]), f))
            testing_t_errorf_v(t, "Abs(%g) = %g, want %g", Z(t_vcAbsSC[i]), f,
                               F(t_absSC[i]));
    }
}

static void TestAcos(TestingT *t) {
    check_vc(t, "Acos", cmplx_acos, t_acos, 1e-14);
    check_sc(t, "Acos", cmplx_acos, t_acosSC, LEN(t_acosSC), SYM_CONJ);
    check_branch(t, "Acos", cmplx_acos);
}

static void TestAcosh(TestingT *t) {
    check_vc(t, "Acosh", cmplx_acosh, t_acosh, 1e-14);
    check_sc(t, "Acosh", cmplx_acosh, t_acoshSC, LEN(t_acoshSC), SYM_CONJ);
    check_branch(t, "Acosh", cmplx_acosh);
}

static void TestAsin(TestingT *t) {
    check_vc(t, "Asin", cmplx_asin, t_asin, 1e-14);
    check_sc(t, "Asin", cmplx_asin, t_asinSC, LEN(t_asinSC), SYM_CONJ | SYM_ODD);
    check_branch(t, "Asin", cmplx_asin);
}

static void TestAsinh(TestingT *t) {
    check_vc(t, "Asinh", cmplx_asinh, t_asinh, 4e-15);
    check_sc(t, "Asinh", cmplx_asinh, t_asinhSC, LEN(t_asinhSC), SYM_CONJ | SYM_ODD);
    check_branch(t, "Asinh", cmplx_asinh);
}

static void TestAtan(TestingT *t) {
    check_vc(t, "Atan", cmplx_atan, t_atan, 4e-16);
    check_sc(t, "Atan", cmplx_atan, t_atanSC, LEN(t_atanSC), SYM_CONJ | SYM_ODD);
    check_branch(t, "Atan", cmplx_atan);
}

static void TestAtanh(TestingT *t) {
    check_vc(t, "Atanh", cmplx_atanh, t_atanh, 4e-16);
    check_sc(t, "Atanh", cmplx_atanh, t_atanhSC, LEN(t_atanhSC), SYM_CONJ | SYM_ODD);
    check_branch(t, "Atanh", cmplx_atanh);
}

static void TestConj(TestingT *t) {
    check_vc(t, "Conj", cmplx_conj, t_conj, 4e-16);
    for (Int i = 0; i < LEN(t_vcConjSC); i++) {
        Complex128 f = cmplx_conj(Z(t_vcConjSC[i]));
        if (!c_alike(Z(t_conjSC[i]), f))
            testing_t_errorf_v(t, "Conj(%g) = %g, want %g", Z(t_vcConjSC[i]), f,
                               Z(t_conjSC[i]));
    }
}

static void TestCos(TestingT *t) {
    check_vc(t, "Cos", cmplx_cos, t_cos, 3e-15);
    check_sc(t, "Cos", cmplx_cos, t_cosSC, LEN(t_cosSC), SYM_CONJ | SYM_EVEN);
}

static void TestCosh(TestingT *t) {
    check_vc(t, "Cosh", cmplx_cosh, t_cosh, 2e-15);
    check_sc(t, "Cosh", cmplx_cosh, t_coshSC, LEN(t_coshSC), SYM_CONJ | SYM_EVEN);
}

static void TestExp(TestingT *t) {
    check_vc(t, "Exp", cmplx_exp, t_exp, 1e-15);
    check_sc(t, "Exp", cmplx_exp, t_expSC, LEN(t_expSC), SYM_CONJ);
}

static void TestIsNaN(TestingT *t) {
    for (Int i = 0; i < LEN(t_vcIsNaNSC); i++) {
        bool f = cmplx_is_nan(Z(t_vcIsNaNSC[i]));
        if (t_isNaNSC[i] != f)
            testing_t_errorf_v(t, "IsNaN(%v) = %v, want %v", Z(t_vcIsNaNSC[i]), f,
                               t_isNaNSC[i]);
    }
}

static void TestLog(TestingT *t) {
    check_vc(t, "Log", cmplx_log, t_log, 4e-16);
    check_sc(t, "Log", cmplx_log, t_logSC, LEN(t_logSC), SYM_CONJ);
    check_branch(t, "Log", cmplx_log);
}

static void TestLog10(TestingT *t) {
    check_vc(t, "Log10", cmplx_log10, t_log10, 4e-16);
    check_sc(t, "Log10", cmplx_log10, t_log10SC, LEN(t_log10SC), SYM_CONJ);
}

static void TestPolar(TestingT *t) {
    /* Go's condition, which fails only when both parts are wrong. */
    for (Int i = 0; i < LEN(t_vc); i++) {
        double theta;
        double r = cmplx_polar(Z(t_vc[i]), &theta);
        if (!veryclose(F(t_polar[i][0]), r) && !veryclose(F(t_polar[i][1]), theta))
            testing_t_errorf_v(t, "Polar(%g) = %g, %g want %g, %g", Z(t_vc[i]), r,
                               theta, F(t_polar[i][0]), F(t_polar[i][1]));
    }
    for (Int i = 0; i < LEN(t_vcPolarSC); i++) {
        double theta;
        double r = cmplx_polar(Z(t_vcPolarSC[i]), &theta);
        if (!alike(F(t_polarSC[i][0]), r) && !alike(F(t_polarSC[i][1]), theta))
            testing_t_errorf_v(t, "Polar(%g) = %g, %g, want %g, %g", Z(t_vcPolarSC[i]),
                               r, theta, F(t_polarSC[i][0]), F(t_polarSC[i][1]));
    }
}

static Complex128 pow_01(Complex128 x) {
    return cmplx_pow(x, C(0.1, 0));
}

static void TestPow(TestingT *t) {
    /* Special cases for Pow(0, c). */
    Complex128 zero = C(0, 0);
    double inf = math_inf(1);
    Complex128 zero_powers[][2] = {
        {{0, 0}, {1, 0}},
        {{1.5, 0}, {0, 0}},
        {{-1.5, 0}, {inf, 0}},
        {{-1.5, 1.5}, {inf, inf}},
    };
    for (Int i = 0; i < LEN(zero_powers); i++) {
        Complex128 f = cmplx_pow(zero, zero_powers[i][0]);
        if (!cmplx_eq(f, zero_powers[i][1]))
            testing_t_errorf_v(t, "Pow(%g, %g) = %g, want %g", zero, zero_powers[i][0],
                               f, zero_powers[i][1]);
    }
    Complex128 a = C(3.0, 3.0);
    for (Int i = 0; i < LEN(t_vc); i++) {
        Complex128 f = cmplx_pow(a, Z(t_vc[i]));
        if (!c_soclose(Z(t_pow[i]), f, 4e-15))
            testing_t_errorf_v(t, "Pow(%g, %g) = %g, want %g", a, Z(t_vc[i]), f,
                               Z(t_pow[i]));
    }
    for (Int i = 0; i < LEN(t_vcPowSC); i++) {
        Complex128 f = cmplx_pow(Z(t_vcPowSC[i][0]), Z(t_vcPowSC[i][1]));
        if (!c_alike(Z(t_powSC[i]), f))
            testing_t_errorf_v(t, "Pow(%g, %g) = %g, want %g", Z(t_vcPowSC[i][0]),
                               Z(t_vcPowSC[i][1]), f, Z(t_powSC[i]));
    }
    check_branch(t, "Pow(z, 0.1)", pow_01);
}

static void TestRect(TestingT *t) {
    for (Int i = 0; i < LEN(t_vc); i++) {
        Complex128 f = cmplx_rect(F(t_polar[i][0]), F(t_polar[i][1]));
        if (!c_veryclose(Z(t_vc[i]), f))
            testing_t_errorf_v(t, "Rect(%g, %g) = %g want %g", F(t_polar[i][0]),
                               F(t_polar[i][1]), f, Z(t_vc[i]));
    }
    for (Int i = 0; i < LEN(t_vcPolarSC); i++) {
        Complex128 f = cmplx_rect(F(t_polarSC[i][0]), F(t_polarSC[i][1]));
        if (!c_alike(Z(t_vcPolarSC[i]), f))
            testing_t_errorf_v(t, "Rect(%g, %g) = %g, want %g", F(t_polarSC[i][0]),
                               F(t_polarSC[i][1]), f, Z(t_vcPolarSC[i]));
    }
}

static void TestSin(TestingT *t) {
    check_vc(t, "Sin", cmplx_sin, t_sin, 2e-15);
    check_sc(t, "Sin", cmplx_sin, t_sinSC, LEN(t_sinSC), SYM_CONJ | SYM_ODD);
}

static void TestSinh(TestingT *t) {
    check_vc(t, "Sinh", cmplx_sinh, t_sinh, 2e-15);
    check_sc(t, "Sinh", cmplx_sinh, t_sinhSC, LEN(t_sinhSC), SYM_CONJ | SYM_ODD);
}

static void TestSqrt(TestingT *t) {
    check_vc(t, "Sqrt", cmplx_sqrt, t_sqrt, 4e-16);
    check_sc(t, "Sqrt", cmplx_sqrt, t_sqrtSC, LEN(t_sqrtSC), SYM_CONJ);
    check_branch(t, "Sqrt", cmplx_sqrt);
}

static void TestTan(TestingT *t) {
    check_vc(t, "Tan", cmplx_tan, t_tan, 3e-15);
    check_sc(t, "Tan", cmplx_tan, t_tanSC, LEN(t_tanSC), SYM_CONJ | SYM_ODD);
}

static void TestTanh(TestingT *t) {
    check_vc(t, "Tanh", cmplx_tanh, t_tanh, 2e-15);
    check_sc(t, "Tanh", cmplx_tanh, t_tanhSC, LEN(t_tanhSC), SYM_CONJ | SYM_ODD);
}

/* See Go issue 17577. */
static void TestInfiniteLoopIntanSeries(TestingT *t) {
    Complex128 want = cmplx_inf();
    Complex128 got = cmplx_cot(C(0, 0));
    if (!cmplx_eq(got, want))
        testing_t_errorf_v(t, "Cot(0): got %g, want %g", got, want);
}

static void TestTanHuge(TestingT *t) {
    for (Int i = 0; i < LEN(t_hugeIn); i++) {
        Complex128 x = Z(t_hugeIn[i]);
        Complex128 f = cmplx_tan(x);
        if (!c_soclose(Z(t_tanHuge[i]), f, 3e-15))
            testing_t_errorf_v(t, "Tan(%g) = %g, want %g", x, f, Z(t_tanHuge[i]));
    }
}

/* ------------------------------------------------------------ the operators
 *
 * Go has no test of its own for these, since they are the language's. The
 * cases are C99's annex G, which Go's runtime follows for division. */

static void TestOperators(TestingT *t) {
    double inf = math_inf(1), nan = math_nan();
    struct {
        Complex128 x, y, want;
    } div[] = {
        {{1, 0}, {0, 0}, {inf, nan}},     {{1, 1}, {0, 0}, {inf, inf}},
        {{-1, 0}, {-0.0, 0}, {inf, nan}}, {{inf, 0}, {2, 0}, {inf, nan}},
        {{inf, inf}, {1, 0}, {inf, inf}}, {{1, 1}, {inf, inf}, {0, 0}},
        {{1, 2}, {3, 4}, {0.44, 0.08}},   {{nan, nan}, {0, 0}, {nan, nan}},
    };
    for (Int i = 0; i < LEN(div); i++) {
        Complex128 f = cmplx_div(div[i].x, div[i].y);
        if (!c_alike(div[i].want, f))
            testing_t_errorf_v(t, "%g / %g = %g, want %g", div[i].x, div[i].y, f,
                               div[i].want);
    }
    Complex128 m = cmplx_mul(C(1, 2), C(3, 4));
    if (!cmplx_eq(m, C(-5, 10)))
        testing_t_errorf_v(t, "(1+2i) * (3+4i) = %g, want (-5+10i)", m);
    if (!cmplx_eq(cmplx_add(C(1, 2), C(3, 4)), C(4, 6)) ||
        !cmplx_eq(cmplx_sub(C(1, 2), C(3, 4)), C(-2, -2)))
        testing_t_errorf_v(t, "%s", "add or sub is wrong");
    if (!cmplx_eq(C(0.0, -0.0), C(-0.0, 0.0)) || cmplx_eq(C(nan, 0), C(nan, 0)))
        testing_t_errorf_v(t, "%s", "eq does not follow ==");
    if (!math_signbit(cmplx_neg(C(0, 0)).re))
        testing_t_errorf_v(t, "%s", "neg of +0 is not -0");
}

/* ----------------------------------------------------------- matching Go */

typedef struct Draws {
    uint64_t s;
} Draws;

static uint64_t draw_next(Draws *d) {
    d->s ^= d->s >> 12;
    d->s ^= d->s << 25;
    d->s ^= d->s >> 27;
    return d->s * 0x2545F4914F6CDD1DU;
}

static double draw_float(Draws *d, Int kind) {
    uint64_t r = draw_next(d);
    switch (kind) {
    case 0:
        return F(r);
    case 1:
        return (double)((int64_t)(r >> 11) - ((int64_t)1 << 52)) * 0x1p-52;
    case 2:
        return (double)((int64_t)(r >> 11) - ((int64_t)1 << 52)) * 0x1p-47;
    case 3:
        return F((r & 0x800fffffffffffffU) |
                 (uint64_t)(1023 + (int64_t)(r >> 52 & 63) - 32) << 52);
    default:
        return (double)((int64_t)(r >> 58) - 32) * 0.5;
    }
}

typedef struct Hash {
    uint64_t h;
} Hash;

static void hash_add(Hash *h, uint64_t b) {
    if ((b & 0x7ff0000000000000U) == 0x7ff0000000000000U &&
        (b & 0x000fffffffffffffU) != 0)
        b = 0x7ff8000000000000U;
    for (int i = 0; i < 8; i++) {
        h->h ^= b >> (8 * i) & 0xff;
        h->h *= 0x100000001b3U;
    }
}

static void hash_f(Hash *h, double v) {
    hash_add(h, math_float64bits(v));
}

static void hash_c(Hash *h, Complex128 v) {
    hash_f(h, v.re);
    hash_f(h, v.im);
}

typedef void (*DiffFn)(Hash *h, Complex128 x, Complex128 y);

#define ONE(name)                                                                      \
    static void diff_##name(Hash *h, Complex128 x, Complex128 y) {                     \
        (void)y;                                                                       \
        hash_c(h, cmplx_##name(x));                                                    \
    }
#define TWO(name)                                                                      \
    static void diff_##name(Hash *h, Complex128 x, Complex128 y) {                     \
        hash_c(h, cmplx_##name(x, y));                                                 \
    }

TWO(add)
TWO(sub)
TWO(mul)
TWO(div)
TWO(pow)
ONE(conj)
ONE(sqrt)
ONE(exp)
ONE(log)
ONE(log10)
ONE(sin)
ONE(cos)
ONE(tan)
ONE(cot)
ONE(asin)
ONE(acos)
ONE(atan)
ONE(sinh)
ONE(cosh)
ONE(tanh)
ONE(asinh)
ONE(acosh)
ONE(atanh)

static void diff_abs(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    hash_f(h, cmplx_abs(x));
}

static void diff_phase(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    hash_f(h, cmplx_phase(x));
}

static void diff_polar(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    double theta;
    double r = cmplx_polar(x, &theta);
    hash_f(h, r);
    hash_f(h, theta);
}

static void diff_rect(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    hash_c(h, cmplx_rect(x.re, x.im));
}

static void diff_is_inf(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    hash_add(h, cmplx_is_inf(x) ? 1 : 0);
}

static void diff_is_nan(Hash *h, Complex128 x, Complex128 y) {
    (void)y;
    hash_add(h, cmplx_is_nan(x) ? 1 : 0);
}

static uint64_t diff_run(DiffFn fn) {
    Draws d = {0x9e3779b97f4a7c15U};
    Hash h = {0xcbf29ce484222325U};
    for (Int i = 0; i < DIFF_N; i++) {
        Complex128 x, y;
        x.re = draw_float(&d, i % 5);
        x.im = draw_float(&d, i / 5 % 5);
        y.re = draw_float(&d, i / 25 % 5);
        y.im = draw_float(&d, i / 125 % 5);
        fn(&h, x, y);
    }
    return h.h;
}

static void TestMatchesGo(TestingT *t) {
#define X(name, want)                                                                  \
    if (diff_run(diff_##name) != (want))                                               \
        testing_t_errorf_v(t, "%s gives different bits from Go's on some input", #name);
    DIFFS(X)
#undef X
}

#define TESTS(X)                                                                       \
    X(TestAbs)                                                                         \
    X(TestAcos)                                                                        \
    X(TestAcosh)                                                                       \
    X(TestAsin)                                                                        \
    X(TestAsinh)                                                                       \
    X(TestAtan)                                                                        \
    X(TestAtanh)                                                                       \
    X(TestConj)                                                                        \
    X(TestCos)                                                                         \
    X(TestCosh)                                                                        \
    X(TestExp)                                                                         \
    X(TestIsNaN)                                                                       \
    X(TestLog)                                                                         \
    X(TestLog10)                                                                       \
    X(TestPolar)                                                                       \
    X(TestPow)                                                                         \
    X(TestRect)                                                                        \
    X(TestSin)                                                                         \
    X(TestSinh)                                                                        \
    X(TestSqrt)                                                                        \
    X(TestTan)                                                                         \
    X(TestTanh)                                                                        \
    X(TestInfiniteLoopIntanSeries)                                                     \
    X(TestTanHuge)                                                                     \
    X(TestOperators)                                                                   \
    X(TestMatchesGo)

TESTING_MAIN(TESTS)
