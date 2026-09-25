/* Derived from Go's src/math/all_test.go, huge_test.go and const_test.go.
 * Go source: go1.27.1.
 *
 * The tables live in tests/math_test_gen.h, written by tools/gen-math-tests.sh
 * from Go's own, and the checks here are Go's with the same tolerances. The
 * tolerances are loose because the expected values are the true answers to 26
 * digits and not what Go returns. So on top of Go's tests, TestMatchesGo runs
 * every function over sixteen thousand inputs and compares a hash of the bits
 * with the one Go's portable code gave when the header was generated. That is
 * the check that says the port returns what Go returns and not merely
 * something close.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/fmt.h"
#include "burrow/math.h"
#include "burrow/mem/arena.h"

/* The inputs TestMatchesGo draws go through no multiply and add, but the
 * tolerance checks and the loops over x do, and Go rounds each step. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

typedef struct Fi {
    uint64_t f;
    int64_t i;
} Fi;

#include "math_test_gen.h"

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static double F(uint64_t b) {
    return math_float64frombits(b);
}

static bool tolerance(double a, double b, double e) {
    /* Multiplying by e here can underflow denormal values to zero. Check a==b
     * so that at least if a and b are small and identical we say they match. */
    if (a == b)
        return true;
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

static bool close(double a, double b) {
    return tolerance(a, b, 1e-14);
}
static bool veryclose(double a, double b) {
    return tolerance(a, b, 4e-16);
}
static bool soclose(double a, double b, double e) {
    return tolerance(a, b, e);
}

static bool alike(double a, double b) {
    if (math_is_nan(a) && math_is_nan(b))
        return true;
    if (a == b)
        return math_signbit(a) == math_signbit(b);
    return false;
}

typedef double (*Fn1)(double);
typedef double (*Fn2)(double, double);

/* The special cases, which every function has and which all compare with
 * alike. */
static void check_sc(TestingT *t, const char *name, Fn1 fn, const uint64_t *in,
                     const uint64_t *want, Int n) {
    for (Int i = 0; i < n; i++) {
        double f = fn(F(in[i]));
        if (!alike(F(want[i]), f))
            testing_t_errorf_v(t, "%s(%g) = %g, want %g", name, F(in[i]), f,
                               F(want[i]));
    }
}

static void check_sc2(TestingT *t, const char *name, Fn2 fn, const uint64_t (*in)[2],
                      const uint64_t *want, Int n) {
    for (Int i = 0; i < n; i++) {
        double f = fn(F(in[i][0]), F(in[i][1]));
        if (!alike(F(want[i]), f))
            testing_t_errorf_v(t, "%s(%g, %g) = %g, want %g", name, F(in[i][0]),
                               F(in[i][1]), f, F(want[i]));
    }
}

static void TestNaN(TestingT *t) {
    double f64 = math_nan();
    if (!math_is_nan(f64))
        testing_t_fatalf_v(t, "NaN() returns %g, expected NaN", f64);
    float f32 = (float)f64;
    if (f32 == f32)
        testing_t_fatalf_v(t, "float32(NaN()) is %g, expected NaN", f32);
    CHECK(math_float64bits(f64) == 0x7FF8000000000001U);
}

static void TestAcos(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_acos(a);
        if (!close(F(t_acos[i]), f))
            testing_t_errorf_v(t, "Acos(%g) = %g, want %g", a, f, F(t_acos[i]));
    }
    check_sc(t, "Acos", math_acos, t_vfacosSC, t_acosSC, LEN(t_vfacosSC));
}

static void TestAcosh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = 1 + math_abs(F(t_vf[i]));
        double f = math_acosh(a);
        if (!veryclose(F(t_acosh[i]), f))
            testing_t_errorf_v(t, "Acosh(%g) = %g, want %g", a, f, F(t_acosh[i]));
    }
    check_sc(t, "Acosh", math_acosh, t_vfacoshSC, t_acoshSC, LEN(t_vfacoshSC));
}

static void TestAsin(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_asin(a);
        if (!veryclose(F(t_asin[i]), f))
            testing_t_errorf_v(t, "Asin(%g) = %g, want %g", a, f, F(t_asin[i]));
    }
    check_sc(t, "Asin", math_asin, t_vfasinSC, t_asinSC, LEN(t_vfasinSC));
}

static void TestAsinh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_asinh(F(t_vf[i]));
        if (!veryclose(F(t_asinh[i]), f))
            testing_t_errorf_v(t, "Asinh(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_asinh[i]));
    }
    check_sc(t, "Asinh", math_asinh, t_vfasinhSC, t_asinhSC, LEN(t_vfasinhSC));
}

static void TestAtan(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_atan(F(t_vf[i]));
        if (!veryclose(F(t_atan[i]), f))
            testing_t_errorf_v(t, "Atan(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_atan[i]));
    }
    check_sc(t, "Atan", math_atan, t_vfatanSC, t_atanSC, LEN(t_vfatanSC));
}

static void TestAtanh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_atanh(a);
        if (!veryclose(F(t_atanh[i]), f))
            testing_t_errorf_v(t, "Atanh(%g) = %g, want %g", a, f, F(t_atanh[i]));
    }
    check_sc(t, "Atanh", math_atanh, t_vfatanhSC, t_atanhSC, LEN(t_vfatanhSC));
}

static void TestAtan2(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_atan2(10, F(t_vf[i]));
        if (!veryclose(F(t_atan2[i]), f))
            testing_t_errorf_v(t, "Atan2(10, %g) = %g, want %g", F(t_vf[i]), f,
                               F(t_atan2[i]));
    }
    check_sc2(t, "Atan2", math_atan2, t_vfatan2SC, t_atan2SC, LEN(t_vfatan2SC));
}

static void TestCbrt(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_cbrt(F(t_vf[i]));
        if (!veryclose(F(t_cbrt[i]), f))
            testing_t_errorf_v(t, "Cbrt(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_cbrt[i]));
    }
    check_sc(t, "Cbrt", math_cbrt, t_vfcbrtSC, t_cbrtSC, LEN(t_vfcbrtSC));
}

static void TestCeil(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_ceil(F(t_vf[i]));
        if (!alike(F(t_ceil[i]), f))
            testing_t_errorf_v(t, "Ceil(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_ceil[i]));
    }
    check_sc(t, "Ceil", math_ceil, t_vfceilSC, t_ceilSC, LEN(t_vfceilSC));
}

static void TestCopysign(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_copysign(F(t_vf[i]), -1);
        if (F(t_copysign[i]) != f)
            testing_t_errorf_v(t, "Copysign(%g, -1) = %g, want %g", F(t_vf[i]), f,
                               F(t_copysign[i]));
    }
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_copysign(F(t_vf[i]), 1);
        if (-F(t_copysign[i]) != f)
            testing_t_errorf_v(t, "Copysign(%g, 1) = %g, want %g", F(t_vf[i]), f,
                               -F(t_copysign[i]));
    }
    for (Int i = 0; i < LEN(t_vfcopysignSC); i++) {
        double f = math_copysign(F(t_vfcopysignSC[i]), -1);
        if (!alike(F(t_copysignSC[i]), f))
            testing_t_errorf_v(t, "Copysign(%g, -1) = %g, want %g",
                               F(t_vfcopysignSC[i]), f, F(t_copysignSC[i]));
    }
}

static void TestCos(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_cos(F(t_vf[i]));
        if (!veryclose(F(t_cos[i]), f))
            testing_t_errorf_v(t, "Cos(%g) = %g, want %g", F(t_vf[i]), f, F(t_cos[i]));
    }
    check_sc(t, "Cos", math_cos, t_vfcosSC, t_cosSC, LEN(t_vfcosSC));
}

static void TestCosh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_cosh(F(t_vf[i]));
        if (!close(F(t_cosh[i]), f))
            testing_t_errorf_v(t, "Cosh(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_cosh[i]));
    }
    check_sc(t, "Cosh", math_cosh, t_vfcoshSC, t_coshSC, LEN(t_vfcoshSC));
}

static void TestErf(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_erf(a);
        if (!veryclose(F(t_erf[i]), f))
            testing_t_errorf_v(t, "Erf(%g) = %g, want %g", a, f, F(t_erf[i]));
    }
    check_sc(t, "Erf", math_erf, t_vferfSC, t_erfSC, LEN(t_vferfSC));
}

static void TestErfc(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_erfc(a);
        if (!veryclose(F(t_erfc[i]), f))
            testing_t_errorf_v(t, "Erfc(%g) = %g, want %g", a, f, F(t_erfc[i]));
    }
    check_sc(t, "Erfc", math_erfc, t_vferfcSC, t_erfcSC, LEN(t_vferfcSC));
}

static void TestErfinv(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 10;
        double f = math_erfinv(a);
        if (!veryclose(F(t_erfinv[i]), f))
            testing_t_errorf_v(t, "Erfinv(%g) = %g, want %g", a, f, F(t_erfinv[i]));
    }
    check_sc(t, "Erfinv", math_erfinv, t_vferfinvSC, t_erfinvSC, LEN(t_vferfinvSC));
    for (int k = 0; k <= 180; k++) {
        double x = -0.9 + k * 1e-2;
        double f = math_erf(math_erfinv(x));
        if (!close(x, f))
            testing_t_errorf_v(t, "Erf(Erfinv(%g)) = %g, want %g", x, f, x);
    }
    for (int k = 0; k <= 180; k++) {
        double x = -0.9 + k * 1e-2;
        double f = math_erfinv(math_erf(x));
        if (!close(x, f))
            testing_t_errorf_v(t, "Erfinv(Erf(%g)) = %g, want %g", x, f, x);
    }
}

static void TestErfcinv(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = 1.0 - (F(t_vf[i]) / 10);
        double f = math_erfcinv(a);
        if (!veryclose(F(t_erfinv[i]), f))
            testing_t_errorf_v(t, "Erfcinv(%g) = %g, want %g", a, f, F(t_erfinv[i]));
    }
    check_sc(t, "Erfcinv", math_erfcinv, t_vferfcinvSC, t_erfcinvSC,
             LEN(t_vferfcinvSC));
    for (int k = 0; k <= 180; k++) {
        double x = 0.1 + k * 1e-2;
        double f = math_erfc(math_erfcinv(x));
        if (!close(x, f))
            testing_t_errorf_v(t, "Erfc(Erfcinv(%g)) = %g, want %g", x, f, x);
    }
    for (int k = 0; k <= 180; k++) {
        double x = 0.1 + k * 1e-2;
        double f = math_erfcinv(math_erfc(x));
        if (!close(x, f))
            testing_t_errorf_v(t, "Erfcinv(Erfc(%g)) = %g, want %g", x, f, x);
    }
}

/* Go runs this and TestExp2 twice, over the assembly and the portable code.
 * There is only the portable code here. */
static void TestExp(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_exp(F(t_vf[i]));
        if (!veryclose(F(t_exp[i]), f))
            testing_t_errorf_v(t, "Exp(%g) = %g, want %g", F(t_vf[i]), f, F(t_exp[i]));
    }
    check_sc(t, "Exp", math_exp, t_vfexpSC, t_expSC, LEN(t_vfexpSC));
}

static void TestExpm1(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 100;
        double f = math_expm1(a);
        if (!veryclose(F(t_expm1[i]), f))
            testing_t_errorf_v(t, "Expm1(%g) = %g, want %g", a, f, F(t_expm1[i]));
    }
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) * 10;
        double f = math_expm1(a);
        if (!close(F(t_expm1Large[i]), f))
            testing_t_errorf_v(t, "Expm1(%g) = %g, want %g", a, f, F(t_expm1Large[i]));
    }
    check_sc(t, "Expm1", math_expm1, t_vfexpm1SC, t_expm1SC, LEN(t_vfexpm1SC));
}

static void TestExp2(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_exp2(F(t_vf[i]));
        if (!close(F(t_exp2[i]), f))
            testing_t_errorf_v(t, "Exp2(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_exp2[i]));
    }
    check_sc(t, "Exp2", math_exp2, t_vfexp2SC, t_exp2SC, LEN(t_vfexp2SC));
    for (Int n = -1074; n < 1024; n++) {
        double f = math_exp2((double)n);
        double vf = math_ldexp(1, n);
        if (f != vf)
            testing_t_errorf_v(t, "Exp2(%d) = %g, want %g", n, f, vf);
    }
}

static void TestAbs(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_abs(F(t_vf[i]));
        if (F(t_fabs[i]) != f)
            testing_t_errorf_v(t, "Abs(%g) = %g, want %g", F(t_vf[i]), f, F(t_fabs[i]));
    }
    check_sc(t, "Abs", math_abs, t_vffabsSC, t_fabsSC, LEN(t_vffabsSC));
}

static void TestDim(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_dim(F(t_vf[i]), 0);
        if (F(t_fdim[i]) != f)
            testing_t_errorf_v(t, "Dim(%g, %g) = %g, want %g", F(t_vf[i]), 0.0, f,
                               F(t_fdim[i]));
    }
    check_sc2(t, "Dim", math_dim, t_vffdimSC, t_fdimSC, LEN(t_vffdimSC));
    check_sc2(t, "Dim", math_dim, t_vffdim2SC, t_fdimSC, LEN(t_vffdim2SC));
}

static void TestFloor(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_floor(F(t_vf[i]));
        if (!alike(F(t_floor[i]), f))
            testing_t_errorf_v(t, "Floor(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_floor[i]));
    }
    check_sc(t, "Floor", math_floor, t_vfceilSC, t_floorSC, LEN(t_vfceilSC));
}

static void TestMax(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_max(F(t_vf[i]), F(t_ceil[i]));
        if (F(t_ceil[i]) != f)
            testing_t_errorf_v(t, "Max(%g, %g) = %g, want %g", F(t_vf[i]), F(t_ceil[i]),
                               f, F(t_ceil[i]));
    }
    check_sc2(t, "Max", math_max, t_vffdimSC, t_fmaxSC, LEN(t_vffdimSC));
    check_sc2(t, "Max", math_max, t_vffdim2SC, t_fmaxSC, LEN(t_vffdim2SC));
}

static void TestMin(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_min(F(t_vf[i]), F(t_floor[i]));
        if (F(t_floor[i]) != f)
            testing_t_errorf_v(t, "Min(%g, %g) = %g, want %g", F(t_vf[i]),
                               F(t_floor[i]), f, F(t_floor[i]));
    }
    check_sc2(t, "Min", math_min, t_vffdimSC, t_fminSC, LEN(t_vffdimSC));
    check_sc2(t, "Min", math_min, t_vffdim2SC, t_fminSC, LEN(t_vffdim2SC));
}

static void TestMod(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_mod(10, F(t_vf[i]));
        if (F(t_fmod[i]) != f)
            testing_t_errorf_v(t, "Mod(10, %g) = %g, want %g", F(t_vf[i]), f,
                               F(t_fmod[i]));
    }
    check_sc2(t, "Mod", math_mod, t_vffmodSC, t_fmodSC, LEN(t_vffmodSC));
    /* The precision of the result for extreme inputs. */
    double f = math_mod(5.9790119248836734e+200, 1.1258465975523544);
    if (0.6447968302508578 != f)
        testing_t_errorf_v(
            t,
            "Mod(5.9790119248836734e+200, 1.1258465975523544) = %g, want "
            "0.6447968302508578",
            f);
}

static void check_frexp(TestingT *t, const uint64_t *in, const Fi *want, Int n,
                        bool near) {
    for (Int i = 0; i < n; i++) {
        Int j;
        double f = math_frexp(F(in[i]), &j);
        bool ok = near ? veryclose(F(want[i].f), f) : alike(F(want[i].f), f);
        if (!ok || want[i].i != j)
            testing_t_errorf_v(t, "Frexp(%g) = %g, %d, want %g, %d", F(in[i]), f, j,
                               F(want[i].f), want[i].i);
    }
}

static void TestFrexp(TestingT *t) {
    check_frexp(t, t_vf, t_frexp, LEN(t_vf), true);
    check_frexp(t, t_vffrexpSC, t_frexpSC, LEN(t_vffrexpSC), false);
    check_frexp(t, t_vffrexpBC, t_frexpBC, LEN(t_vffrexpBC), false);
    CHECK(math_frexp(8, NULL) == 0.5);
}

static void TestGamma(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_gamma(F(t_vf[i]));
        if (!close(F(t_gamma[i]), f))
            testing_t_errorf_v(t, "Gamma(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_gamma[i]));
    }
    for (Int i = 0; i < LEN(t_vfgamma); i++) {
        double x = F(t_vfgamma[i][0]), want = F(t_vfgamma[i][1]);
        double f = math_gamma(x);
        bool ok;
        if (math_is_nan(want) || math_is_inf(want, 0) || want == 0 || f == 0)
            ok = alike(want, f);
        else if (x > -50 && x <= 171)
            ok = veryclose(want, f);
        else
            ok = close(want, f);
        if (!ok)
            testing_t_errorf_v(t, "Gamma(%g) = %g, want %g", x, f, want);
    }
}

static void TestHypot(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(1e200 * F(t_tanh[i]) * math_sqrt(2));
        double f = math_hypot(1e200 * F(t_tanh[i]), 1e200 * F(t_tanh[i]));
        if (!veryclose(a, f))
            testing_t_errorf_v(t, "Hypot(%g, %g) = %g, want %g", 1e200 * F(t_tanh[i]),
                               1e200 * F(t_tanh[i]), f, a);
    }
    check_sc2(t, "Hypot", math_hypot, t_vfhypotSC, t_hypotSC, LEN(t_vfhypotSC));
}

static void TestIlogb(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        /* Less one, since the fraction is in [1/2, 1). */
        Int a = (Int)t_frexp[i].i - 1;
        Int e = math_ilogb(F(t_vf[i]));
        if (a != e)
            testing_t_errorf_v(t, "Ilogb(%g) = %d, want %d", F(t_vf[i]), e, a);
    }
    for (Int i = 0; i < LEN(t_vflogbSC); i++) {
        Int e = math_ilogb(F(t_vflogbSC[i]));
        if (t_ilogbSC[i] != e)
            testing_t_errorf_v(t, "Ilogb(%g) = %d, want %d", F(t_vflogbSC[i]), e,
                               t_ilogbSC[i]);
    }
    for (Int i = 0; i < LEN(t_vffrexpBC); i++) {
        Int e = math_ilogb(F(t_vffrexpBC[i]));
        if ((Int)F(t_logbBC[i]) != e)
            testing_t_errorf_v(t, "Ilogb(%g) = %d, want %d", F(t_vffrexpBC[i]), e,
                               (Int)F(t_logbBC[i]));
    }
}

static void TestJ0(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_j0(F(t_vf[i]));
        if (!soclose(F(t_j0[i]), f, 4e-14))
            testing_t_errorf_v(t, "J0(%g) = %g, want %g", F(t_vf[i]), f, F(t_j0[i]));
    }
    check_sc(t, "J0", math_j0, t_vfj0SC, t_j0SC, LEN(t_vfj0SC));
}

static void TestJ1(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_j1(F(t_vf[i]));
        if (!close(F(t_j1[i]), f))
            testing_t_errorf_v(t, "J1(%g) = %g, want %g", F(t_vf[i]), f, F(t_j1[i]));
    }
    check_sc(t, "J1", math_j1, t_vfj0SC, t_j1SC, LEN(t_vfj0SC));
}

static void TestJn(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_jn(2, F(t_vf[i]));
        if (!close(F(t_j2[i]), f))
            testing_t_errorf_v(t, "Jn(2, %g) = %g, want %g", F(t_vf[i]), f, F(t_j2[i]));
        f = math_jn(-3, F(t_vf[i]));
        if (!close(F(t_jM3[i]), f))
            testing_t_errorf_v(t, "Jn(-3, %g) = %g, want %g", F(t_vf[i]), f,
                               F(t_jM3[i]));
    }
    for (Int i = 0; i < LEN(t_vfj0SC); i++) {
        double f = math_jn(2, F(t_vfj0SC[i]));
        if (!alike(F(t_j2SC[i]), f))
            testing_t_errorf_v(t, "Jn(2, %g) = %g, want %g", F(t_vfj0SC[i]), f,
                               F(t_j2SC[i]));
        f = math_jn(-3, F(t_vfj0SC[i]));
        if (!alike(F(t_jM3SC[i]), f))
            testing_t_errorf_v(t, "Jn(-3, %g) = %g, want %g", F(t_vfj0SC[i]), f,
                               F(t_jM3SC[i]));
    }
}

/* One table of frac and exp against the expected doubles. Go's has two rows
 * whose exponent is only the value it is on a 64 bit machine, and on a 32 bit
 * one those are skipped rather than truncated. */
static void check_ldexp(TestingT *t, const Fi *in, const uint64_t *want, Int n,
                        bool near) {
    for (Int i = 0; i < n; i++) {
        if (in[i].i != (int64_t)(Int)in[i].i)
            continue;
        double f = math_ldexp(F(in[i].f), (Int)in[i].i);
        bool ok = near ? veryclose(F(want[i]), f) : alike(F(want[i]), f);
        if (!ok)
            testing_t_errorf_v(t, "Ldexp(%g, %d) = %g, want %g", F(in[i].f), in[i].i, f,
                               F(want[i]));
    }
}

static void TestLdexp(TestingT *t) {
    check_ldexp(t, t_frexp, t_vf, LEN(t_vf), true);
    check_ldexp(t, t_frexpSC, t_vffrexpSC, LEN(t_vffrexpSC), false);
    check_ldexp(t, t_vfldexpSC, t_ldexpSC, LEN(t_vfldexpSC), false);
    check_ldexp(t, t_frexpBC, t_vffrexpBC, LEN(t_vffrexpBC), false);
    check_ldexp(t, t_vfldexpBC, t_ldexpBC, LEN(t_vfldexpBC), false);
}

static void TestLgamma(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        Int s;
        double f = math_lgamma(F(t_vf[i]), &s);
        if (!close(F(t_lgamma[i].f), f) || t_lgamma[i].i != s)
            testing_t_errorf_v(t, "Lgamma(%g) = %g, %d, want %g, %d", F(t_vf[i]), f, s,
                               F(t_lgamma[i].f), t_lgamma[i].i);
    }
    for (Int i = 0; i < LEN(t_vflgammaSC); i++) {
        Int s;
        double f = math_lgamma(F(t_vflgammaSC[i]), &s);
        if (!alike(F(t_lgammaSC[i].f), f) || t_lgammaSC[i].i != s)
            testing_t_errorf_v(t, "Lgamma(%g) = %g, %d, want %g, %d",
                               F(t_vflgammaSC[i]), f, s, F(t_lgammaSC[i].f),
                               t_lgammaSC[i].i);
    }
}

static void TestLog(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_log(a);
        if (F(t_log[i]) != f)
            testing_t_errorf_v(t, "Log(%g) = %g, want %g", a, f, F(t_log[i]));
    }
    double f = math_log(10);
    if (f != MATH_LN10)
        testing_t_errorf_v(t, "Log(%g) = %g, want %g", 10.0, f, MATH_LN10);
    check_sc(t, "Log", math_log, t_vflogSC, t_logSC, LEN(t_vflogSC));
}

static void TestLogb(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_logb(F(t_vf[i]));
        if (F(t_logb[i]) != f)
            testing_t_errorf_v(t, "Logb(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_logb[i]));
    }
    check_sc(t, "Logb", math_logb, t_vflogbSC, t_logbSC, LEN(t_vflogbSC));
    check_sc(t, "Logb", math_logb, t_vffrexpBC, t_logbBC, LEN(t_vffrexpBC));
}

static void TestLog10(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_log10(a);
        if (!veryclose(F(t_log10[i]), f))
            testing_t_errorf_v(t, "Log10(%g) = %g, want %g", a, f, F(t_log10[i]));
    }
    double f = math_log10(MATH_E);
    if (f != MATH_LOG10_E)
        testing_t_errorf_v(t, "Log10(%g) = %g, want %g", MATH_E, f, MATH_LOG10_E);
    check_sc(t, "Log10", math_log10, t_vflogSC, t_logSC, LEN(t_vflogSC));
}

static void TestLog1p(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = F(t_vf[i]) / 100;
        double f = math_log1p(a);
        if (!veryclose(F(t_log1p[i]), f))
            testing_t_errorf_v(t, "Log1p(%g) = %g, want %g", a, f, F(t_log1p[i]));
    }
    double a = 9.0;
    double f = math_log1p(a);
    if (f != MATH_LN10)
        testing_t_errorf_v(t, "Log1p(%g) = %g, want %g", a, f, MATH_LN10);
    check_sc(t, "Log1p", math_log1p, t_vflog1pSC, t_log1pSC, LEN(t_vflogSC));
}

static void TestLog2(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_log2(a);
        if (!veryclose(F(t_log2[i]), f))
            testing_t_errorf_v(t, "Log2(%g) = %g, want %g", a, f, F(t_log2[i]));
    }
    double f = math_log2(MATH_E);
    if (f != MATH_LOG2_E)
        testing_t_errorf_v(t, "Log2(%g) = %g, want %g", MATH_E, f, MATH_LOG2_E);
    check_sc(t, "Log2", math_log2, t_vflogSC, t_logSC, LEN(t_vflogSC));
    for (Int i = -1074; i <= 1023; i++) {
        double l = math_log2(math_ldexp(1, i));
        if (l != (double)i)
            testing_t_errorf_v(t, "Log2(2**%d) = %g, want %d", i, l, i);
    }
}

static void TestModf(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double g;
        double f = math_modf(F(t_vf[i]), &g);
        if (!veryclose(F(t_modf[i][0]), f) || !veryclose(F(t_modf[i][1]), g))
            testing_t_errorf_v(t, "Modf(%g) = %g, %g, want %g, %g", F(t_vf[i]), f, g,
                               F(t_modf[i][0]), F(t_modf[i][1]));
    }
    for (Int i = 0; i < LEN(t_vfmodfSC); i++) {
        double g;
        double f = math_modf(F(t_vfmodfSC[i]), &g);
        if (!alike(F(t_modfSC[i][0]), f) || !alike(F(t_modfSC[i][1]), g))
            testing_t_errorf_v(t, "Modf(%g) = %g, %g, want %g, %g", F(t_vfmodfSC[i]), f,
                               g, F(t_modfSC[i][0]), F(t_modfSC[i][1]));
    }
    CHECK(math_modf(2.5, NULL) == 2);
}

static float F32(uint32_t b) {
    return math_float32frombits(b);
}

static void TestNextafter32(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        float vfi = (float)F(t_vf[i]);
        float f = math_nextafter32(vfi, 10);
        if (F32(t_nextafter32[i]) != f)
            testing_t_errorf_v(t, "Nextafter32(%g, %g) = %g want %g", vfi, 10.0, f,
                               F32(t_nextafter32[i]));
    }
    for (Int i = 0; i < LEN(t_vfnextafter32SC); i++) {
        float x = F32(t_vfnextafter32SC[i][0]), y = F32(t_vfnextafter32SC[i][1]);
        float f = math_nextafter32(x, y);
        if (!alike((double)F32(t_nextafter32SC[i]), (double)f))
            testing_t_errorf_v(t, "Nextafter32(%g, %g) = %g want %g", x, y, f,
                               F32(t_nextafter32SC[i]));
    }
}

static void TestNextafter64(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_nextafter(F(t_vf[i]), 10);
        if (F(t_nextafter64[i]) != f)
            testing_t_errorf_v(t, "Nextafter64(%g, %g) = %g want %g", F(t_vf[i]), 10.0,
                               f, F(t_nextafter64[i]));
    }
    check_sc2(t, "Nextafter64", math_nextafter, t_vfnextafter64SC, t_nextafter64SC,
              LEN(t_vfnextafter64SC));
}

static void TestPow(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_pow(10, F(t_vf[i]));
        if (!close(F(t_pow[i]), f))
            testing_t_errorf_v(t, "Pow(10, %g) = %g, want %g", F(t_vf[i]), f,
                               F(t_pow[i]));
    }
    check_sc2(t, "Pow", math_pow, t_vfpowSC, t_powSC, LEN(t_vfpowSC));
}

static void TestPow10(TestingT *t) {
    for (Int i = 0; i < LEN(t_vfpow10SC); i++) {
        if (t_vfpow10SC[i] != (int64_t)(Int)t_vfpow10SC[i])
            continue;
        double f = math_pow10((Int)t_vfpow10SC[i]);
        if (!alike(F(t_pow10SC[i]), f))
            testing_t_errorf_v(t, "Pow10(%d) = %g, want %g", t_vfpow10SC[i], f,
                               F(t_pow10SC[i]));
    }
}

static void check_remainder_sign(TestingT *t, double x, double y) {
    double r = math_remainder(x, y);
    if (r == 0 && math_signbit(r) != math_signbit(x))
        testing_t_errorf_v(
            t,
            "Remainder(x=%f, y=%f) = %f, sign of (zero) result should agree "
            "with sign of x",
            x, y, r);
}

static void TestRemainder(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_remainder(10, F(t_vf[i]));
        if (F(t_remainder[i]) != f)
            testing_t_errorf_v(t, "Remainder(10, %g) = %g, want %g", F(t_vf[i]), f,
                               F(t_remainder[i]));
    }
    check_sc2(t, "Remainder", math_remainder, t_vffmodSC, t_fmodSC, LEN(t_vffmodSC));
    /* The precision of the result for extreme inputs. */
    double f = math_remainder(5.9790119248836734e+200, 1.1258465975523544);
    if (-0.4810497673014966 != f)
        testing_t_errorf_v(
            t,
            "Remainder(5.9790119248836734e+200, 1.1258465975523544) = %g, "
            "want -0.4810497673014966",
            f);
    /* The sign is right when the remainder is zero. */
    for (int xi = 0; xi <= 3; xi++) {
        for (int yi = 1; yi <= 3; yi++) {
            double x = xi, y = yi;
            check_remainder_sign(t, x, y);
            check_remainder_sign(t, x, -y);
            check_remainder_sign(t, -x, y);
            check_remainder_sign(t, -x, -y);
        }
    }
}

static void TestRound(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_round(F(t_vf[i]));
        if (!alike(F(t_round[i]), f))
            testing_t_errorf_v(t, "Round(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_round[i]));
    }
    for (Int i = 0; i < LEN(t_vfroundSC); i++) {
        double f = math_round(F(t_vfroundSC[i][0]));
        if (!alike(F(t_vfroundSC[i][1]), f))
            testing_t_errorf_v(t, "Round(%g) = %g, want %g", F(t_vfroundSC[i][0]), f,
                               F(t_vfroundSC[i][1]));
    }
}

static void TestRoundToEven(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_round_to_even(F(t_vf[i]));
        if (!alike(F(t_round[i]), f))
            testing_t_errorf_v(t, "RoundToEven(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_round[i]));
    }
    for (Int i = 0; i < LEN(t_vfroundEvenSC); i++) {
        double f = math_round_to_even(F(t_vfroundEvenSC[i][0]));
        if (!alike(F(t_vfroundEvenSC[i][1]), f))
            testing_t_errorf_v(t, "RoundToEven(%g) = %g, want %g",
                               F(t_vfroundEvenSC[i][0]), f, F(t_vfroundEvenSC[i][1]));
    }
}

static void TestSignbit(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        bool f = math_signbit(F(t_vf[i]));
        if (t_signbit[i] != f)
            testing_t_errorf_v(t, "Signbit(%g) = %t, want %t", F(t_vf[i]), f,
                               t_signbit[i]);
    }
    for (Int i = 0; i < LEN(t_vfsignbitSC); i++) {
        bool f = math_signbit(F(t_vfsignbitSC[i]));
        if (t_signbitSC[i] != f)
            testing_t_errorf_v(t, "Signbit(%g) = %t, want %t", F(t_vfsignbitSC[i]), f,
                               t_signbitSC[i]);
    }
}

static void TestSin(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_sin(F(t_vf[i]));
        if (!veryclose(F(t_sin[i]), f))
            testing_t_errorf_v(t, "Sin(%g) = %g, want %g", F(t_vf[i]), f, F(t_sin[i]));
    }
    check_sc(t, "Sin", math_sin, t_vfsinSC, t_sinSC, LEN(t_vfsinSC));
}

static void TestSincos(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double c;
        double s = math_sincos(F(t_vf[i]), &c);
        if (!veryclose(F(t_sin[i]), s) || !veryclose(F(t_cos[i]), c))
            testing_t_errorf_v(t, "Sincos(%g) = %g, %g want %g, %g", F(t_vf[i]), s, c,
                               F(t_sin[i]), F(t_cos[i]));
    }
    CHECK(math_sincos(0, NULL) == 0);
}

static void TestSinh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_sinh(F(t_vf[i]));
        if (!close(F(t_sinh[i]), f))
            testing_t_errorf_v(t, "Sinh(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_sinh[i]));
    }
    check_sc(t, "Sinh", math_sinh, t_vfsinhSC, t_sinhSC, LEN(t_vfsinhSC));
}

static void TestSqrt(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_sqrt(a);
        if (F(t_sqrt[i]) != f)
            testing_t_errorf_v(t, "Sqrt(%g) = %g, want %g", a, f, F(t_sqrt[i]));
    }
    check_sc(t, "Sqrt", math_sqrt, t_vfsqrtSC, t_sqrtSC, LEN(t_vfsqrtSC));
}

static void TestTan(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_tan(F(t_vf[i]));
        if (!veryclose(F(t_tan[i]), f))
            testing_t_errorf_v(t, "Tan(%g) = %g, want %g", F(t_vf[i]), f, F(t_tan[i]));
    }
    /* The same special cases as Sin. */
    check_sc(t, "Tan", math_tan, t_vfsinSC, t_sinSC, LEN(t_vfsinSC));
}

static void TestTanh(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_tanh(F(t_vf[i]));
        if (!veryclose(F(t_tanh[i]), f))
            testing_t_errorf_v(t, "Tanh(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_tanh[i]));
    }
    check_sc(t, "Tanh", math_tanh, t_vftanhSC, t_tanhSC, LEN(t_vftanhSC));
}

static void TestTrunc(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f = math_trunc(F(t_vf[i]));
        if (!alike(F(t_trunc[i]), f))
            testing_t_errorf_v(t, "Trunc(%g) = %g, want %g", F(t_vf[i]), f,
                               F(t_trunc[i]));
    }
    check_sc(t, "Trunc", math_trunc, t_vfceilSC, t_truncSC, LEN(t_vfceilSC));
}

static void TestY0(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_y0(a);
        if (!close(F(t_y0[i]), f))
            testing_t_errorf_v(t, "Y0(%g) = %g, want %g", a, f, F(t_y0[i]));
    }
    check_sc(t, "Y0", math_y0, t_vfy0SC, t_y0SC, LEN(t_vfy0SC));
}

static void TestY1(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_y1(a);
        if (!soclose(F(t_y1[i]), f, 2e-14))
            testing_t_errorf_v(t, "Y1(%g) = %g, want %g", a, f, F(t_y1[i]));
    }
    check_sc(t, "Y1", math_y1, t_vfy0SC, t_y1SC, LEN(t_vfy0SC));
}

static void TestYn(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double a = math_abs(F(t_vf[i]));
        double f = math_yn(2, a);
        if (!close(F(t_y2[i]), f))
            testing_t_errorf_v(t, "Yn(2, %g) = %g, want %g", a, f, F(t_y2[i]));
        f = math_yn(-3, a);
        if (!close(F(t_yM3[i]), f))
            testing_t_errorf_v(t, "Yn(-3, %g) = %g, want %g", a, f, F(t_yM3[i]));
    }
    for (Int i = 0; i < LEN(t_vfy0SC); i++) {
        double f = math_yn(2, F(t_vfy0SC[i]));
        if (!alike(F(t_y2SC[i]), f))
            testing_t_errorf_v(t, "Yn(2, %g) = %g, want %g", F(t_vfy0SC[i]), f,
                               F(t_y2SC[i]));
        f = math_yn(-3, F(t_vfy0SC[i]));
        if (!alike(F(t_yM3SC[i]), f))
            testing_t_errorf_v(t, "Yn(-3, %g) = %g, want %g", F(t_vfy0SC[i]), f,
                               F(t_yM3SC[i]));
    }
    double f = math_yn(0, 0);
    if (!alike(math_inf(-1), f))
        testing_t_errorf_v(t, "Yn(0, 0) = %g, want %g", f, math_inf(-1));
}

static void TestFMA(TestingT *t) {
    for (Int i = 0; i < LEN(t_fmaC); i++) {
        double x = F(t_fmaC[i][0]), y = F(t_fmaC[i][1]), z = F(t_fmaC[i][2]);
        double got = math_fma(x, y, z);
        if (!alike(got, F(t_fmaC[i][3])))
            testing_t_errorf_v(t, "FMA(%g,%g,%g) == %g; want %g", x, y, z, got,
                               F(t_fmaC[i][3]));
    }
}

/* Go's version checks the fused negated forms some machines have against the
 * portable code. There is one version here, so what is left to check is that
 * negating an operand going in does what negating it by hand does. */
static void TestFMANegativeArgs(TestingT *t) {
    for (Int i = 0; i < LEN(t_fmaC); i++) {
        double x = F(t_fmaC[i][0]), y = F(t_fmaC[i][1]), z = F(t_fmaC[i][2]);
        double nx = math_float64frombits(math_float64bits(x) ^ (1ULL << 63));
        double nz = math_float64frombits(math_float64bits(z) ^ (1ULL << 63));
        double got = math_fma(x, y, -z), want = math_fma(x, y, nz);
        if (!alike(got, want))
            testing_t_errorf_v(t, "FMA(%g, %g, -(%g)) == %g, want %g", x, y, z, got,
                               want);
        got = math_fma(-x, y, z);
        want = math_fma(nx, y, z);
        if (!alike(got, want))
            testing_t_errorf_v(t, "FMA(-(%g), %g, %g) == %g, want %g", x, y, z, got,
                               want);
        got = math_fma(-x, y, -z);
        want = math_fma(nx, y, nz);
        if (!alike(got, want))
            testing_t_errorf_v(t, "FMA(-(%g), %g, -(%g)) == %g, want %g", x, y, z, got,
                               want);
    }
}

/* Trig functions of large angles return accurate results. Since (vf[i] +
 * large) - large != vf[i], checking Trig(vf[i] + large) == Trig(vf[i]) with
 * large a multiple of 2*Pi would be misleading, so there are tables. */
/* float64(100000 * Pi) in Go, where the product is exact until it is
 * converted. */
static const double large = 314159.26535897935;

static void TestLargeCos(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f1 = F(t_cosLarge[i]);
        double f2 = math_cos(F(t_vf[i]) + large);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Cos(%g) = %g, want %g", F(t_vf[i]) + large, f2, f1);
    }
}

static void TestLargeSin(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f1 = F(t_sinLarge[i]);
        double f2 = math_sin(F(t_vf[i]) + large);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Sin(%g) = %g, want %g", F(t_vf[i]) + large, f2, f1);
    }
}

static void TestLargeSincos(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f1 = F(t_sinLarge[i]), g1 = F(t_cosLarge[i]);
        double g2;
        double f2 = math_sincos(F(t_vf[i]) + large, &g2);
        if (!close(f1, f2) || !close(g1, g2))
            testing_t_errorf_v(t, "Sincos(%g) = %g, %g, want %g, %g",
                               F(t_vf[i]) + large, f2, g2, f1, g1);
    }
}

static void TestLargeTan(TestingT *t) {
    for (Int i = 0; i < LEN(t_vf); i++) {
        double f1 = F(t_tanLarge[i]);
        double f2 = math_tan(F(t_vf[i]) + large);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Tan(%g) = %g, want %g", F(t_vf[i]) + large, f2, f1);
    }
}

/* Go's TestTrigReduce calls the reduction directly, which is internal here.
 * The huge arguments below and the ones TestMatchesGo draws go through it. */

static void TestHugeCos(TestingT *t) {
    for (Int i = 0; i < LEN(t_trigHuge); i++) {
        double x = F(t_trigHuge[i]), f1 = F(t_cosHuge[i]);
        double f2 = math_cos(x);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Cos(%g) = %g, want %g", x, f2, f1);
        double f3 = math_cos(-x);
        if (!close(f1, f3))
            testing_t_errorf_v(t, "Cos(%g) = %g, want %g", -x, f3, f1);
    }
}

static void TestHugeSin(TestingT *t) {
    for (Int i = 0; i < LEN(t_trigHuge); i++) {
        double x = F(t_trigHuge[i]), f1 = F(t_sinHuge[i]);
        double f2 = math_sin(x);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Sin(%g) = %g, want %g", x, f2, f1);
        double f3 = math_sin(-x);
        if (!close(-f1, f3))
            testing_t_errorf_v(t, "Sin(%g) = %g, want %g", -x, f3, -f1);
    }
}

static void TestHugeSinCos(TestingT *t) {
    for (Int i = 0; i < LEN(t_trigHuge); i++) {
        double x = F(t_trigHuge[i]), f1 = F(t_sinHuge[i]), g1 = F(t_cosHuge[i]);
        double g2, g3;
        double f2 = math_sincos(x, &g2);
        if (!close(f1, f2) || !close(g1, g2))
            testing_t_errorf_v(t, "Sincos(%g) = %g, %g, want %g, %g", x, f2, g2, f1,
                               g1);
        double f3 = math_sincos(-x, &g3);
        if (!close(-f1, f3) || !close(g1, g3))
            testing_t_errorf_v(t, "Sincos(%g) = %g, %g, want %g, %g", -x, f3, g3, -f1,
                               g1);
    }
}

static void TestHugeTan(TestingT *t) {
    for (Int i = 0; i < LEN(t_trigHuge); i++) {
        double x = F(t_trigHuge[i]), f1 = F(t_tanHuge[i]);
        double f2 = math_tan(x);
        if (!close(f1, f2))
            testing_t_errorf_v(t, "Tan(%g) = %g, want %g", x, f2, f1);
        double f3 = math_tan(-x);
        if (!close(-f1, f3))
            testing_t_errorf_v(t, "Tan(%g) = %g, want %g", -x, f3, -f1);
    }
}

/* The constants print as Go prints them, which says they are the same
 * doubles. */
static void TestFloatMinMax(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(fmt_sprint_v(a, (double)MATH_MAX_FLOAT64),
                 BURROW_S("1.7976931348623157e+308")));
    CHECK(str_eq(fmt_sprint_v(a, (double)MATH_SMALLEST_NONZERO_FLOAT64),
                 BURROW_S("5e-324")));
    CHECK(str_eq(fmt_sprint_v(a, (float)MATH_MAX_FLOAT32), BURROW_S("3.4028235e+38")));
    CHECK(str_eq(fmt_sprint_v(a, (float)MATH_SMALLEST_NONZERO_FLOAT32),
                 BURROW_S("1e-45")));
    arena_free(&ar);
}

static void TestFloatMinima(TestingT *t) {
    volatile float h32 = (float)MATH_SMALLEST_NONZERO_FLOAT32;
    volatile double h64 = MATH_SMALLEST_NONZERO_FLOAT64;
    float q32 = h32 / 2;
    if (q32 != 0)
        testing_t_errorf_v(t, "float32(SmallestNonzeroFloat32 / 2) = %g, want 0", q32);
    double q64 = h64 / 2;
    if (q64 != 0)
        testing_t_errorf_v(t, "float64(SmallestNonzeroFloat64 / 2) = %g, want 0", q64);
}

static void TestFloat32Sqrt(TestingT *t) {
    for (Int i = 0; i < LEN(t_sqrt32); i++) {
        float v = F32(t_sqrt32[i]);
        Fn1 indirect = math_sqrt;
        float want = (float)indirect((double)v);
        float got = (float)math_sqrt((double)v);
        if (math_is_nan((double)want)) {
            if (!math_is_nan((double)got))
                testing_t_errorf_v(t, "got=%v want=NaN, v=%v", got, v);
            continue;
        }
        if (got != want)
            testing_t_errorf_v(t, "got=%v want=%v, v=%v", got, want, v);
    }
}

/* The constants for Go's other float bits, which Go tests by its compiler
 * accepting them. */
static void TestConstants(TestingT *t) {
    CHECK(math_float64bits(MATH_E) == 0x4005bf0a8b145769U);
    CHECK(math_float64bits(MATH_PI) == 0x400921fb54442d18U);
    CHECK(math_float64bits(MATH_PHI) == 0x3ff9e3779b97f4a8U);
    CHECK(math_float64bits(MATH_SQRT2) == 0x3ff6a09e667f3bcdU);
    CHECK(math_float64bits(MATH_SQRT_E) == 0x3ffa61298e1e069cU);
    CHECK(math_float64bits(MATH_SQRT_PI) == 0x3ffc5bf891b4ef6bU);
    CHECK(math_float64bits(MATH_SQRT_PHI) == 0x3ff45a3146a88456U);
    CHECK(math_float64bits(MATH_LN2) == 0x3fe62e42fefa39efU);
    CHECK(math_float64bits(MATH_LOG2_E) == 0x3ff71547652b82feU);
    CHECK(math_float64bits(MATH_LN10) == 0x40026bb1bbb55516U);
    CHECK(math_float64bits(MATH_LOG10_E) == 0x3fdbcb7b1526e50eU);
    CHECK(math_float64bits(MATH_MAX_FLOAT64) == 0x7fefffffffffffffU);
    CHECK(math_float64bits(MATH_SMALLEST_NONZERO_FLOAT64) == 1);
    CHECK(math_float32bits((float)MATH_MAX_FLOAT32) == 0x7f7fffffU);
    CHECK(math_float32bits((float)MATH_SMALLEST_NONZERO_FLOAT32) == 1);
}

static void TestMaxUint(TestingT *t) {
    CHECK((Uint)((Uint)MATH_MAX_UINT + 1) == 0);
    CHECK((uint8_t)((uint8_t)MATH_MAX_UINT8 + 1) == 0);
    CHECK((uint16_t)((uint16_t)MATH_MAX_UINT16 + 1) == 0);
    CHECK((uint32_t)((uint32_t)MATH_MAX_UINT32 + 1) == 0);
    CHECK((uint64_t)((uint64_t)MATH_MAX_UINT64 + 1) == 0);
}

static void TestMaxInt(TestingT *t) {
    /* C does not wrap a signed int, so the wrap is done in the unsigned type
     * and converted back, which is what Go's does. */
    CHECK((Int)((Uint)MATH_MAX_INT + 1) == MATH_MIN_INT);
    CHECK((int8_t)(uint8_t)((uint8_t)MATH_MAX_INT8 + 1) == MATH_MIN_INT8);
    CHECK((int16_t)(uint16_t)((uint16_t)MATH_MAX_INT16 + 1) == MATH_MIN_INT16);
    CHECK((int32_t)((uint32_t)MATH_MAX_INT32 + 1) == MATH_MIN_INT32);
    CHECK((int64_t)((uint64_t)MATH_MAX_INT64 + 1) == MATH_MIN_INT64);
}

/* ------------------------------------------------------------ against Go
 *
 * The same draws as tools/gen-math-tests/gen_test.go, which has the reasons
 * for them. */

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
static void hash_i(Hash *h, Int v) {
    hash_add(h, (uint64_t)(int64_t)v);
}

static void hash_f32(Hash *h, float v) {
    uint64_t b = math_float32bits(v);
    if ((b & 0x7f800000) == 0x7f800000 && (b & 0x007fffff) != 0)
        b = 0x7fc00000;
    hash_add(h, b);
}

/* One input: three doubles and a non-negative Int, each function using what it
 * needs. */
typedef struct Arg {
    double x, y, z;
    Int n;
} Arg;

typedef void (*DiffFn)(Hash *h, const Arg *a);

#define ONE(name)                                                                      \
    static void diff_##name(Hash *h, const Arg *a) {                                   \
        hash_f(h, math_##name(a->x));                                                  \
    }
#define TWO(name)                                                                      \
    static void diff_##name(Hash *h, const Arg *a) {                                   \
        hash_f(h, math_##name(a->x, a->y));                                            \
    }

ONE(floor)
ONE(ceil)
ONE(trunc)
ONE(round)
ONE(round_to_even)
TWO(dim)
TWO(max)
TWO(min)
TWO(mod)
TWO(remainder)
ONE(sqrt)
ONE(cbrt)
TWO(hypot)
TWO(pow)
ONE(logb)
TWO(nextafter)
ONE(exp)
ONE(exp2)
ONE(expm1)
ONE(log)
ONE(log10)
ONE(log2)
ONE(log1p)
ONE(sin)
ONE(cos)
ONE(tan)
ONE(asin)
ONE(acos)
ONE(atan)
TWO(atan2)
ONE(sinh)
ONE(cosh)
ONE(tanh)
ONE(asinh)
ONE(acosh)
ONE(atanh)
ONE(erf)
ONE(erfc)
ONE(erfinv)
ONE(erfcinv)
ONE(gamma)
ONE(j0)
ONE(j1)
ONE(y0)
ONE(y1)

static void diff_modf(Hash *h, const Arg *a) {
    double f;
    double i = math_modf(a->x, &f);
    hash_f(h, i);
    hash_f(h, f);
}

static void diff_fma(Hash *h, const Arg *a) {
    hash_f(h, math_fma(a->x, a->y, a->z));
}

static void diff_pow10(Hash *h, const Arg *a) {
    hash_f(h, math_pow10(a->n % 700 - 350));
}

static void diff_frexp(Hash *h, const Arg *a) {
    Int e;
    double f = math_frexp(a->x, &e);
    hash_f(h, f);
    hash_i(h, e);
}

static void diff_ldexp(Hash *h, const Arg *a) {
    hash_f(h, math_ldexp(a->x, a->n % 2400 - 1200));
}

static void diff_ilogb(Hash *h, const Arg *a) {
    hash_i(h, math_ilogb(a->x));
}

static void diff_nextafter32(Hash *h, const Arg *a) {
    hash_f32(h, math_nextafter32((float)a->x, (float)a->y));
}

static void diff_sincos(Hash *h, const Arg *a) {
    double c;
    double s = math_sincos(a->x, &c);
    hash_f(h, s);
    hash_f(h, c);
}

static void diff_lgamma(Hash *h, const Arg *a) {
    Int s;
    double l = math_lgamma(a->x, &s);
    hash_f(h, l);
    hash_i(h, s);
}

static void diff_jn(Hash *h, const Arg *a) {
    hash_f(h, math_jn(a->n % 16 - 8, a->x));
}

static void diff_yn(Hash *h, const Arg *a) {
    hash_f(h, math_yn(a->n % 16 - 8, a->x));
}

static uint64_t diff_run(DiffFn fn) {
    Draws d = {0x9e3779b97f4a7c15U};
    Hash h = {0xcbf29ce484222325U};
    for (Int i = 0; i < DIFF_N; i++) {
        Arg a;
        a.x = draw_float(&d, i % 5);
        a.y = draw_float(&d, i / 5 % 5);
        a.z = draw_float(&d, i / 25 % 5);
        /* 31 bits, so that it fits an Int on a 32 bit machine too. */
        a.n = (Int)(draw_next(&d) >> 33);
        fn(&h, &a);
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
    X(TestNaN)                                                                         \
    X(TestAcos)                                                                        \
    X(TestAcosh)                                                                       \
    X(TestAsin)                                                                        \
    X(TestAsinh)                                                                       \
    X(TestAtan)                                                                        \
    X(TestAtanh)                                                                       \
    X(TestAtan2)                                                                       \
    X(TestCbrt)                                                                        \
    X(TestCeil)                                                                        \
    X(TestCopysign)                                                                    \
    X(TestCos)                                                                         \
    X(TestCosh)                                                                        \
    X(TestErf)                                                                         \
    X(TestErfc)                                                                        \
    X(TestErfinv)                                                                      \
    X(TestErfcinv)                                                                     \
    X(TestExp)                                                                         \
    X(TestExpm1)                                                                       \
    X(TestExp2)                                                                        \
    X(TestAbs)                                                                         \
    X(TestDim)                                                                         \
    X(TestFloor)                                                                       \
    X(TestMax)                                                                         \
    X(TestMin)                                                                         \
    X(TestMod)                                                                         \
    X(TestFrexp)                                                                       \
    X(TestGamma)                                                                       \
    X(TestHypot)                                                                       \
    X(TestIlogb)                                                                       \
    X(TestJ0)                                                                          \
    X(TestJ1)                                                                          \
    X(TestJn)                                                                          \
    X(TestLdexp)                                                                       \
    X(TestLgamma)                                                                      \
    X(TestLog)                                                                         \
    X(TestLogb)                                                                        \
    X(TestLog10)                                                                       \
    X(TestLog1p)                                                                       \
    X(TestLog2)                                                                        \
    X(TestModf)                                                                        \
    X(TestNextafter32)                                                                 \
    X(TestNextafter64)                                                                 \
    X(TestPow)                                                                         \
    X(TestPow10)                                                                       \
    X(TestRemainder)                                                                   \
    X(TestRound)                                                                       \
    X(TestRoundToEven)                                                                 \
    X(TestSignbit)                                                                     \
    X(TestSin)                                                                         \
    X(TestSincos)                                                                      \
    X(TestSinh)                                                                        \
    X(TestSqrt)                                                                        \
    X(TestTan)                                                                         \
    X(TestTanh)                                                                        \
    X(TestTrunc)                                                                       \
    X(TestY0)                                                                          \
    X(TestY1)                                                                          \
    X(TestYn)                                                                          \
    X(TestFMA)                                                                         \
    X(TestFMANegativeArgs)                                                             \
    X(TestLargeCos)                                                                    \
    X(TestLargeSin)                                                                    \
    X(TestLargeSincos)                                                                 \
    X(TestLargeTan)                                                                    \
    X(TestHugeCos)                                                                     \
    X(TestHugeSin)                                                                     \
    X(TestHugeSinCos)                                                                  \
    X(TestHugeTan)                                                                     \
    X(TestFloatMinMax)                                                                 \
    X(TestFloatMinima)                                                                 \
    X(TestFloat32Sqrt)                                                                 \
    X(TestConstants)                                                                   \
    X(TestMaxUint)                                                                     \
    X(TestMaxInt)                                                                      \
    X(TestMatchesGo)

TESTING_MAIN(TESTS)
