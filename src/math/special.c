/* math, the special functions: erf, the gamma functions and the Bessel
 * functions. Ported from Go, which took most of them from FreeBSD's msun, and
 * split from math.c only because of their size. The notes at the top of that
 * file apply here too.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/math.h"

/* No fused multiply and add, which would round once where Go rounds twice. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

/* Constant expressions from the Go source, as the doubles Go gives them. */
#define SPECIAL_1_OVER_SQRT_PI 0.56418958354775628 /* 1/SqrtPi */
#define SPECIAL_2_OVER_PI 0.63661977236758138      /* 2/Pi */
#define SPECIAL_1_OVER_035 2.8571428571428572      /* 1/0.35 */

/* ---------------------------------------------------------------------- erf */

#define ERX 8.45062911510467529297e-01 /* 0x3FEB0AC160000000 */
/* Coefficients for approximation to  erf  in |x| <= 0.84375 */
#define EFX 1.28379167095512586316e-01    /* 0x3FC06EBA8214DB69 */
#define EFX8 1.02703333676410069053e+00   /* 0x3FF06EBA8214DB69 */
#define PP0 1.28379167095512558561e-01    /* 0x3FC06EBA8214DB68 */
#define PP1 (-3.25042107247001499370e-01) /* 0xBFD4CD7D691CB913 */
#define PP2 (-2.84817495755985104766e-02) /* 0xBF9D2A51DBD7194F */
#define PP3 (-5.77027029648944159157e-03) /* 0xBF77A291236668E4 */
#define PP4 (-2.37630166566501626084e-05) /* 0xBEF8EAD6120016AC */
#define QQ1 3.97917223959155352819e-01    /* 0x3FD97779CDDADC09 */
#define QQ2 6.50222499887672944485e-02    /* 0x3FB0A54C5536CEBA */
#define QQ3 5.08130628187576562776e-03    /* 0x3F74D022C4D36B0F */
#define QQ4 1.32494738004321644526e-04    /* 0x3F215DC9221C1A10 */
#define QQ5 (-3.96022827877536812320e-06) /* 0xBED09C4342A26120 */
/* Coefficients for approximation to  erf  in 0.84375 <= |x| <= 1.25 */
#define PA0 (-2.36211856075265944077e-03) /* 0xBF6359B8BEF77538 */
#define PA1 4.14856118683748331666e-01    /* 0x3FDA8D00AD92B34D */
#define PA2 (-3.72207876035701323847e-01) /* 0xBFD7D240FBB8C3F1 */
#define PA3 3.18346619901161753674e-01    /* 0x3FD45FCA805120E4 */
#define PA4 (-1.10894694282396677476e-01) /* 0xBFBC63983D3E28EC */
#define PA5 3.54783043256182359371e-02    /* 0x3FA22A36599795EB */
#define PA6 (-2.16637559486879084300e-03) /* 0xBF61BF380A96073F */
#define QA1 1.06420880400844228286e-01    /* 0x3FBB3E6618EEE323 */
#define QA2 5.40397917702171048937e-01    /* 0x3FE14AF092EB6F33 */
#define QA3 7.18286544141962662868e-02    /* 0x3FB2635CD99FE9A7 */
#define QA4 1.26171219808761642112e-01    /* 0x3FC02660E763351F */
#define QA5 1.36370839120290507362e-02    /* 0x3F8BEDC26B51DD1C */
#define QA6 1.19844998467991074170e-02    /* 0x3F888B545735151D */
/* Coefficients for approximation to  erfc  in 1.25 <= |x| <= 1/0.35 */
#define RA0 (-9.86494403484714822705e-03) /* 0xBF843412600D6435 */
#define RA1 (-6.93858572707181764372e-01) /* 0xBFE63416E4BA7360 */
#define RA2 (-1.05586262253232909814e+01) /* 0xC0251E0441B0E726 */
#define RA3 (-6.23753324503260060396e+01) /* 0xC04F300AE4CBA38D */
#define RA4 (-1.62396669462573470355e+02) /* 0xC0644CB184282266 */
#define RA5 (-1.84605092906711035994e+02) /* 0xC067135CEBCCABB2 */
#define RA6 (-8.12874355063065934246e+01) /* 0xC054526557E4D2F2 */
#define RA7 (-9.81432934416914548592e+00) /* 0xC023A0EFC69AC25C */
#define SA1 1.96512716674392571292e+01    /* 0x4033A6B9BD707687 */
#define SA2 1.37657754143519042600e+02    /* 0x4061350C526AE721 */
#define SA3 4.34565877475229228821e+02    /* 0x407B290DD58A1A71 */
#define SA4 6.45387271733267880336e+02    /* 0x40842B1921EC2868 */
#define SA5 4.29008140027567833386e+02    /* 0x407AD02157700314 */
#define SA6 1.08635005541779435134e+02    /* 0x405B28A3EE48AE2C */
#define SA7 6.57024977031928170135e+00    /* 0x401A47EF8E484A93 */
#define SA8 (-6.04244152148580987438e-02) /* 0xBFAEEFF2EE749A62 */
/* Coefficients for approximation to  erfc  in 1/.35 <= |x| <= 28 */
#define RB0 (-9.86494292470009928597e-03) /* 0xBF84341239E86F4A */
#define RB1 (-7.99283237680523006574e-01) /* 0xBFE993BA70C285DE */
#define RB2 (-1.77579549177547519889e+01) /* 0xC031C209555F995A */
#define RB3 (-1.60636384855821916062e+02) /* 0xC064145D43C5ED98 */
#define RB4 (-6.37566443368389627722e+02) /* 0xC083EC881375F228 */
#define RB5 (-1.02509513161107724954e+03) /* 0xC09004616A2E5992 */
#define RB6 (-4.83519191608651397019e+02) /* 0xC07E384E9BDC383F */
#define SB1 3.03380607434824582924e+01    /* 0x403E568B261D5190 */
#define SB2 3.25792512996573918826e+02    /* 0x40745CAE221B9F0A */
#define SB3 1.53672958608443695994e+03    /* 0x409802EB189D5118 */
#define SB4 3.19985821950859553908e+03    /* 0x40A8FFB7688C246A */
#define SB5 2.55305040643316442583e+03    /* 0x40A3F219CEDF3BE6 */
#define SB6 4.74528541206955367215e+02    /* 0x407DA874E79FE763 */
#define SB7 (-2.24409524465858183362e+01) /* 0xC03670E242712D62 */

/* The erfc tail for 1.25 <= x < 28, shared by erf and erfc: the r/x both of
 * them finish with. */
static double special_erfc_tail(double x) {
    double s = 1 / (x * x);
    double R, S;
    if (x < SPECIAL_1_OVER_035) { /* |x| < 1 / 0.35 ~ 2.857143 */
        R = RA0 +
            s * (RA1 +
                 s * (RA2 + s * (RA3 + s * (RA4 + s * (RA5 + s * (RA6 + s * RA7))))));
        S = 1 +
            s * (SA1 +
                 s * (SA2 +
                      s * (SA3 +
                           s * (SA4 + s * (SA5 + s * (SA6 + s * (SA7 + s * SA8)))))));
    } else { /* |x| >= 1 / 0.35 ~ 2.857143 */
        R = RB0 + s * (RB1 + s * (RB2 + s * (RB3 + s * (RB4 + s * (RB5 + s * RB6)))));
        S = 1 +
            s * (SB1 +
                 s * (SB2 + s * (SB3 + s * (SB4 + s * (SB5 + s * (SB6 + s * SB7))))));
    }
    double z = math_float64frombits(
        math_float64bits(x) &
        0xffffffff00000000U); /* pseudo-single (20-bit) precision x */
    return math_exp(-z * z - 0.5625) * math_exp((z - x) * (z + x) + R / S);
}

double math_erf(double x) {
    const double very_tiny = 2.848094538889218e-306; /* 0x0080000000000000 */
    const double small = 1.0 / (1 << 28);            /* 2**-28 */
    /* special cases */
    if (math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return 1;
    if (math_is_inf(x, -1))
        return -1;
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    if (x < 0.84375) { /* |x| < 0.84375 */
        double temp;
        if (x < small) { /* |x| < 2**-28 */
            if (x < very_tiny)
                temp = 0.125 * (8.0 * x + EFX8 * x); /* avoid underflow */
            else
                temp = x + EFX * x;
        } else {
            double z = x * x;
            double r = PP0 + z * (PP1 + z * (PP2 + z * (PP3 + z * PP4)));
            double s = 1 + z * (QQ1 + z * (QQ2 + z * (QQ3 + z * (QQ4 + z * QQ5))));
            double y = r / s;
            temp = x + x * y;
        }
        if (sign)
            return -temp;
        return temp;
    }
    if (x < 1.25) { /* 0.84375 <= |x| < 1.25 */
        double s = x - 1;
        double P =
            PA0 + s * (PA1 + s * (PA2 + s * (PA3 + s * (PA4 + s * (PA5 + s * PA6)))));
        double Q =
            1 + s * (QA1 + s * (QA2 + s * (QA3 + s * (QA4 + s * (QA5 + s * QA6)))));
        if (sign)
            return -ERX - P / Q;
        return ERX + P / Q;
    }
    if (x >= 6) { /* inf > |x| >= 6 */
        if (sign)
            return -1;
        return 1;
    }
    double r = special_erfc_tail(x);
    if (sign)
        return r / x - 1;
    return 1 - r / x;
}

double math_erfc(double x) {
    const double tiny = 1.0 / (double)(1ULL << 56); /* 2**-56 */
    /* special cases */
    if (math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return 0;
    if (math_is_inf(x, -1))
        return 2;
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    if (x < 0.84375) { /* |x| < 0.84375 */
        double temp;
        if (x < tiny) { /* |x| < 1/2**56 */
            temp = x;
        } else {
            double z = x * x;
            double r = PP0 + z * (PP1 + z * (PP2 + z * (PP3 + z * PP4)));
            double s = 1 + z * (QQ1 + z * (QQ2 + z * (QQ3 + z * (QQ4 + z * QQ5))));
            double y = r / s;
            if (x < 0.25) /* |x| < 1/4 */
                temp = x + x * y;
            else
                temp = 0.5 + (x * y + (x - 0.5));
        }
        if (sign)
            return 1 + temp;
        return 1 - temp;
    }
    if (x < 1.25) { /* 0.84375 <= |x| < 1.25 */
        double s = x - 1;
        double P =
            PA0 + s * (PA1 + s * (PA2 + s * (PA3 + s * (PA4 + s * (PA5 + s * PA6)))));
        double Q =
            1 + s * (QA1 + s * (QA2 + s * (QA3 + s * (QA4 + s * (QA5 + s * QA6)))));
        if (sign)
            return 1 + ERX + P / Q;
        return 1 - ERX - P / Q;
    }
    if (x < 28) { /* |x| < 28 */
        if (sign && x > 6 && x >= SPECIAL_1_OVER_035)
            return 2; /* x < -6 */
        double r = special_erfc_tail(x);
        if (sign)
            return 2 - r / x;
        return r / x;
    }
    if (sign)
        return 2;
    return 0;
}

double math_erfinv(double x) {
    /* Coefficients for approximation to erf in |x| <= 0.85 */
    const double a0 = 1.1975323115670912564578e0, a1 = 4.7072688112383978012285e1,
                 a2 = 6.9706266534389598238465e2, a3 = 4.8548868893843886794648e3,
                 a4 = 1.6235862515167575384252e4, a5 = 2.3782041382114385731252e4,
                 a6 = 1.1819493347062294404278e4, a7 = 8.8709406962545514830200e2;
    const double b0 = 1.0000000000000000000e0, b1 = 4.2313330701600911252e1,
                 b2 = 6.8718700749205790830e2, b3 = 5.3941960214247511077e3,
                 b4 = 2.1213794301586595867e4, b5 = 3.9307895800092710610e4,
                 b6 = 2.8729085735721942674e4, b7 = 5.2264952788528545610e3;
    /* Coefficients for approximation to erf in 0.85 < |x| <= 1-2*exp(-25) */
    const double c0 = 1.42343711074968357734e0, c1 = 4.63033784615654529590e0,
                 c2 = 5.76949722146069140550e0, c3 = 3.64784832476320460504e0,
                 c4 = 1.27045825245236838258e0, c5 = 2.41780725177450611770e-1,
                 c6 = 2.27238449892691845833e-2, c7 = 7.74545014278341407640e-4;
    const double d0 = 1.4142135623730950488016887e0, d1 = 2.9036514445419946173133295e0,
                 d2 = 2.3707661626024532365971225e0,
                 d3 = 9.7547832001787427186894837e-1,
                 d4 = 2.0945065210512749128288442e-1,
                 d5 = 2.1494160384252876777097297e-2,
                 d6 = 7.7441459065157709165577218e-4,
                 d7 = 1.4859850019840355905497876e-9;
    /* Coefficients for approximation to erf in 1-2*exp(-25) < |x| < 1 */
    const double e0 = 6.65790464350110377720e0, e1 = 5.46378491116411436990e0,
                 e2 = 1.78482653991729133580e0, e3 = 2.96560571828504891230e-1,
                 e4 = 2.65321895265761230930e-2, e5 = 1.24266094738807843860e-3,
                 e6 = 2.71155556874348757815e-5, e7 = 2.01033439929228813265e-7;
    const double f0 = 1.414213562373095048801689e0, f1 = 8.482908416595164588112026e-1,
                 f2 = 1.936480946950659106176712e-1, f3 = 2.103693768272068968719679e-2,
                 f4 = 1.112800997078859844711555e-3, f5 = 2.611088405080593625138020e-5,
                 f6 = 2.010321207683943062279931e-7,
                 f7 = 2.891024605872965461538222e-15;

    /* special cases */
    if (math_is_nan(x) || x <= -1 || x >= 1) {
        if (x == -1 || x == 1)
            return math_inf((Int)x);
        return math_nan();
    }

    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }

    double ans;
    if (x <= 0.85) { /* |x| <= 0.85 */
        double r = 0.180625 - 0.25 * x * x;
        double z1 =
            ((((((a7 * r + a6) * r + a5) * r + a4) * r + a3) * r + a2) * r + a1) * r +
            a0;
        double z2 =
            ((((((b7 * r + b6) * r + b5) * r + b4) * r + b3) * r + b2) * r + b1) * r +
            b0;
        ans = (x * z1) / z2;
    } else {
        double z1, z2;
        double r = math_sqrt(MATH_LN2 - math_log(1.0 - x));
        if (r <= 5.0) {
            r -= 1.6;
            z1 = ((((((c7 * r + c6) * r + c5) * r + c4) * r + c3) * r + c2) * r + c1) *
                     r +
                 c0;
            z2 = ((((((d7 * r + d6) * r + d5) * r + d4) * r + d3) * r + d2) * r + d1) *
                     r +
                 d0;
        } else {
            r -= 5.0;
            z1 = ((((((e7 * r + e6) * r + e5) * r + e4) * r + e3) * r + e2) * r + e1) *
                     r +
                 e0;
            z2 = ((((((f7 * r + f6) * r + f5) * r + f4) * r + f3) * r + f2) * r + f1) *
                     r +
                 f0;
        }
        ans = z1 / z2;
    }

    if (sign)
        return -ans;
    return ans;
}

double math_erfcinv(double x) {
    return math_erfinv(1 - x);
}

/* -------------------------------------------------------------------- gamma */

static const double special_gam_p[] = {
    1.60119522476751861407e-04, 1.19135147006586384913e-03, 1.04213797561761569935e-02,
    4.76367800457137231464e-02, 2.07448227648435975150e-01, 4.94214826801497100753e-01,
    9.99999999999999996796e-01,
};

static const double special_gam_q[] = {
    -2.31581873324120129819e-05, 5.39605580493303397842e-04,
    -4.45641913851797240494e-03, 1.18139785222060435552e-02,
    3.58236398605498653373e-02,  -2.34591795718243348568e-01,
    7.14304917030273074085e-02,  1.00000000000000000320e+00,
};

static const double special_gam_s[] = {
    7.87311395793093628397e-04,  -2.29549961613378126380e-04,
    -2.68132617805781232825e-03, 3.47222221605458667310e-03,
    8.33333333333482257126e-02,
};

/* Gamma function computed by Stirling's formula, as two factors whose product
 * is the answer, so that the caller can avoid an overflow in between. */
static double special_stirling(double x, double *y2p) {
    if (x > 200) {
        *y2p = 1;
        return math_inf(1);
    }
    const double sqrt_two_pi = 2.506628274631000502417;
    const double max_stirling = 143.01608;
    double w = 1 / x;
    w = 1 +
        w * ((((special_gam_s[0] * w + special_gam_s[1]) * w + special_gam_s[2]) * w +
              special_gam_s[3]) *
                 w +
             special_gam_s[4]);
    double y1 = math_exp(x);
    double y2 = 1.0;
    if (x > max_stirling) { /* avoid Pow() overflow */
        double v = math_pow(x, 0.5 * x - 0.25);
        y2 = v / y1;
        y1 = v;
    } else {
        y1 = math_pow(x, x - 0.5) / y1;
    }
    *y2p = sqrt_two_pi * w * y2;
    return y1;
}

static bool special_is_neg_int(double x) {
    if (x < 0) {
        double xf;
        math_modf(x, &xf);
        return xf == 0;
    }
    return false;
}

double math_gamma(double x) {
    const double euler =
        0.57721566490153286060651209008240243104215933593992; /* A001620 */
    /* special cases */
    if (special_is_neg_int(x) || math_is_inf(x, -1) || math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return math_inf(1);
    if (x == 0) {
        if (math_signbit(x))
            return math_inf(-1);
        return math_inf(1);
    }
    double q = math_abs(x);
    double p = math_floor(q);
    if (q > 33) {
        if (x >= 0) {
            double y2;
            double y1 = special_stirling(x, &y2);
            return y1 * y2;
        }
        /* Note: x is negative but (checked above) not a negative integer, so
         * x must be small enough to be in range for conversion to int64. If
         * |x| were >= 2⁶³ it would have to be an integer. */
        Int signgam = 1;
        int64_t ip = (int64_t)p;
        if ((ip & 1) == 0)
            signgam = -1;
        double z = q - p;
        if (z > 0.5) {
            p = p + 1;
            z = q - p;
        }
        z = q * math_sin(MATH_PI * z);
        if (z == 0)
            return math_inf(signgam);
        double sq2;
        double sq1 = special_stirling(q, &sq2);
        double absz = math_abs(z);
        double d = absz * sq1 * sq2;
        if (math_is_inf(d, 0))
            z = MATH_PI / absz / sq1 / sq2;
        else
            z = MATH_PI / d;
        return (double)signgam * z;
    }

    /* Reduce argument */
    double z = 1.0;
    while (x >= 3) {
        x = x - 1;
        z = z * x;
    }
    while (x < 0) {
        if (x > -1e-09)
            goto small;
        z = z / x;
        x = x + 1;
    }
    while (x < 2) {
        if (x < 1e-09)
            goto small;
        z = z / x;
        x = x + 1;
    }

    if (x == 2)
        return z;

    x = x - 2;
    p = (((((x * special_gam_p[0] + special_gam_p[1]) * x + special_gam_p[2]) * x +
           special_gam_p[3]) *
              x +
          special_gam_p[4]) *
             x +
         special_gam_p[5]) *
            x +
        special_gam_p[6];
    q = ((((((x * special_gam_q[0] + special_gam_q[1]) * x + special_gam_q[2]) * x +
            special_gam_q[3]) *
               x +
           special_gam_q[4]) *
              x +
          special_gam_q[5]) *
             x +
         special_gam_q[6]) *
            x +
        special_gam_q[7];
    return z * p / q;

small:
    if (x == 0)
        return math_inf(1);
    return z / ((1 + euler * x) * x);
}

/* ------------------------------------------------------------------- lgamma */

static const double special_lgam_a[] = {
    7.72156649015328655494e-02, /* 0x3FB3C467E37DB0C8 */
    3.22467033424113591611e-01, /* 0x3FD4A34CC4A60FAD */
    6.73523010531292681824e-02, /* 0x3FB13E001A5562A7 */
    2.05808084325167332806e-02, /* 0x3F951322AC92547B */
    7.38555086081402883957e-03, /* 0x3F7E404FB68FEFE8 */
    2.89051383673415629091e-03, /* 0x3F67ADD8CCB7926B */
    1.19270763183362067845e-03, /* 0x3F538A94116F3F5D */
    5.10069792153511336608e-04, /* 0x3F40B6C689B99C00 */
    2.20862790713908385557e-04, /* 0x3F2CF2ECED10E54D */
    1.08011567247583939954e-04, /* 0x3F1C5088987DFB07 */
    2.52144565451257326939e-05, /* 0x3EFA7074428CFA52 */
    4.48640949618915160150e-05, /* 0x3F07858E90A45837 */
};

static const double special_lgam_r[] = {
    1.0,                        /* placeholder */
    1.39200533467621045958e+00, /* 0x3FF645A762C4AB74 */
    7.21935547567138069525e-01, /* 0x3FE71A1893D3DCDC */
    1.71933865632803078993e-01, /* 0x3FC601EDCCFBDF27 */
    1.86459191715652901344e-02, /* 0x3F9317EA742ED475 */
    7.77942496381893596434e-04, /* 0x3F497DDACA41A95B */
    7.32668430744625636189e-06, /* 0x3EDEBAF7A5B38140 */
};

static const double special_lgam_s[] = {
    -7.72156649015328655494e-02, /* 0xBFB3C467E37DB0C8 */
    2.14982415960608852501e-01,  /* 0x3FCB848B36E20878 */
    3.25778796408930981787e-01,  /* 0x3FD4D98F4F139F59 */
    1.46350472652464452805e-01,  /* 0x3FC2BB9CBEE5F2F7 */
    2.66422703033638609560e-02,  /* 0x3F9B481C7E939961 */
    1.84028451407337715652e-03,  /* 0x3F5E26B67368F239 */
    3.19475326584100867617e-05,  /* 0x3F00BFECDD17E945 */
};

static const double special_lgam_t[] = {
    4.83836122723810047042e-01,  /* 0x3FDEF72BC8EE38A2 */
    -1.47587722994593911752e-01, /* 0xBFC2E4278DC6C509 */
    6.46249402391333854778e-02,  /* 0x3FB08B4294D5419B */
    -3.27885410759859649565e-02, /* 0xBFA0C9A8DF35B713 */
    1.79706750811820387126e-02,  /* 0x3F9266E7970AF9EC */
    -1.03142241298341437450e-02, /* 0xBF851F9FBA91EC6A */
    6.10053870246291332635e-03,  /* 0x3F78FCE0E370E344 */
    -3.68452016781138256760e-03, /* 0xBF6E2EFFB3E914D7 */
    2.25964780900612472250e-03,  /* 0x3F6282D32E15C915 */
    -1.40346469989232843813e-03, /* 0xBF56FE8EBF2D1AF1 */
    8.81081882437654011382e-04,  /* 0x3F4CDF0CEF61A8E9 */
    -5.38595305356740546715e-04, /* 0xBF41A6109C73E0EC */
    3.15632070903625950361e-04,  /* 0x3F34AF6D6C0EBBF7 */
    -3.12754168375120860518e-04, /* 0xBF347F24ECC38C38 */
    3.35529192635519073543e-04,  /* 0x3F35FD3EE8C2D3F4 */
};

static const double special_lgam_u[] = {
    -7.72156649015328655494e-02, /* 0xBFB3C467E37DB0C8 */
    6.32827064025093366517e-01,  /* 0x3FE4401E8B005DFF */
    1.45492250137234768737e+00,  /* 0x3FF7475CD119BD6F */
    9.77717527963372745603e-01,  /* 0x3FEF497644EA8450 */
    2.28963728064692451092e-01,  /* 0x3FCD4EAEF6010924 */
    1.33810918536787660377e-02,  /* 0x3F8B678BBF2BAB09 */
};

static const double special_lgam_v[] = {
    1.0,
    2.45597793713041134822e+00, /* 0x4003A5D7C2BD619C */
    2.12848976379893395361e+00, /* 0x40010725A42B18F5 */
    7.69285150456672783825e-01, /* 0x3FE89DFBE45050AF */
    1.04222645593369134254e-01, /* 0x3FBAAE55D6537C88 */
    3.21709242282423911810e-03, /* 0x3F6A5ABB57D0CF61 */
};

static const double special_lgam_w[] = {
    4.18938533204672725052e-01,  /* 0x3FDACFE390C97D69 */
    8.33333333333329678849e-02,  /* 0x3FB555555555553B */
    -2.77777777728775536470e-03, /* 0xBF66C16C16B02E5C */
    7.93650558643019558500e-04,  /* 0x3F4A019F98CF38B6 */
    -5.95187557450339963135e-04, /* 0xBF4380CB8C0FE741 */
    8.36339918996282139126e-04,  /* 0x3F4B67BA4CDAD5D1 */
    -1.63092934096575273989e-03, /* 0xBF5AB89D0B9E43E4 */
};

/* sin(Pi*x) for x > 0, with the sign flipped, as lgamma wants it. */
static double special_sin_pi(double x) {
    const double two52 = (double)(1ULL << 52); /* 0x4330000000000000 ~4.5036e+15 */
    const double two53 = (double)(1ULL << 53); /* 0x4340000000000000 ~9.0072e+15 */
    if (x < 0.25)
        return -math_sin(MATH_PI * x);

    /* argument reduction */
    double z = math_floor(x);
    Int n;
    if (z != x) { /* inexact */
        x = math_mod(x, 2);
        n = (Int)(x * 4);
    } else {
        if (x >= two53) { /* x must be even */
            x = 0;
            n = 0;
        } else {
            if (x < two52)
                z = x + two52; /* exact */
            n = (Int)(1 & math_float64bits(z));
            x = (double)n;
            n <<= 2;
        }
    }
    switch (n) {
    case 0:
        x = math_sin(MATH_PI * x);
        break;
    case 1:
    case 2:
        x = math_cos(MATH_PI * (0.5 - x));
        break;
    case 3:
    case 4:
        x = math_sin(MATH_PI * (1 - x));
        break;
    case 5:
    case 6:
        x = -math_cos(MATH_PI * (x - 1.5));
        break;
    default:
        x = math_sin(MATH_PI * (x - 2));
        break;
    }
    return -x;
}

double math_lgamma(double x, Int *signp) {
    const double ymin = 1.461632144968362245;
    const double two52 = (double)(1ULL << 52); /* 0x4330000000000000 ~4.5036e+15 */
    const double two58 = (double)(1ULL << 58); /* 0x4390000000000000 ~2.8823e+17 */
    const double tiny = 1.0 / 1180591620717411303424.0; /* 2**-70, 0x3b90000000000000 */
    const double Tc = 1.46163214496836224576e+00;       /* 0x3FF762D86356BE3F */
    const double Tf = -1.21486290535849611461e-01;      /* 0xBFBF19B9BCC38A42 */
    /* Tt = -(tail of Tf) */
    const double Tt = -3.63867699703950536541e-18; /* 0xBC50C7CAA48A971F */
    /* Constant expressions from Go, as doubles. */
    const double ymin_m1_p27 = 0.73163214496836226; /* Ymin - 1 + 0.27 */
    const double ymin_m1_m27 = 0.19163214496836226; /* Ymin - 1 - 0.27 */
    const double ymin_p27 = 1.7316321449683623;     /* Ymin + 0.27 */
    const double ymin_m27 = 1.1916321449683622;     /* Ymin - 0.27 */
    (void)ymin;

    Int sign = 1;
    double lgamma;
    /* special cases */
    if (math_is_nan(x) || math_is_inf(x, 0)) {
        lgamma = x;
        goto out;
    }
    if (x == 0) {
        lgamma = math_inf(1);
        goto out;
    }

    bool neg = false;
    if (x < 0) {
        x = -x;
        neg = true;
    }

    if (x < tiny) { /* if |x| < 2**-70, return -log(|x|) */
        if (neg)
            sign = -1;
        lgamma = -math_log(x);
        goto out;
    }
    double nadj = 0;
    if (neg) {
        if (x >= two52) { /* |x| >= 2**52, must be -integer */
            lgamma = math_inf(1);
            goto out;
        }
        double t = special_sin_pi(x);
        if (t == 0) {
            lgamma = math_inf(1); /* -integer */
            goto out;
        }
        nadj = math_log(MATH_PI / math_abs(t * x));
        if (t < 0)
            sign = -1;
    }

    if (x == 1 || x == 2) { /* purge off 1 and 2 */
        lgamma = 0;
        goto out;
    } else if (x < 2) { /* use lgamma(x) = lgamma(x+1) - log(x) */
        double y;
        int i;
        if (x <= 0.9) {
            lgamma = -math_log(x);
            if (x >= ymin_m1_p27) { /* 0.7316 <= x <=  0.9 */
                y = 1 - x;
                i = 0;
            } else if (x >= ymin_m1_m27) { /* 0.2316 <= x < 0.7316 */
                y = x - (Tc - 1);
                i = 1;
            } else { /* 0 < x < 0.2316 */
                y = x;
                i = 2;
            }
        } else {
            lgamma = 0;
            if (x >= ymin_p27) { /* 1.7316 <= x < 2 */
                y = 2 - x;
                i = 0;
            } else if (x >= ymin_m27) { /* 1.2316 <= x < 1.7316 */
                y = x - Tc;
                i = 1;
            } else { /* 0.9 < x < 1.2316 */
                y = x - 1;
                i = 2;
            }
        }
        const double *A = special_lgam_a, *T = special_lgam_t, *U = special_lgam_u,
                     *V = special_lgam_v;
        switch (i) {
        case 0: {
            double z = y * y;
            double p1 =
                A[0] + z * (A[2] + z * (A[4] + z * (A[6] + z * (A[8] + z * A[10]))));
            double p2 =
                z *
                (A[1] + z * (+A[3] + z * (A[5] + z * (A[7] + z * (A[9] + z * A[11])))));
            double p = y * p1 + p2;
            lgamma += (p - 0.5 * y);
            break;
        }
        case 1: {
            double z = y * y;
            double w = z * y;
            double p1 =
                T[0] +
                w * (T[3] + w * (T[6] + w * (T[9] + w * T[12]))); /* parallel comp */
            double p2 = T[1] + w * (T[4] + w * (T[7] + w * (T[10] + w * T[13])));
            double p3 = T[2] + w * (T[5] + w * (T[8] + w * (T[11] + w * T[14])));
            double p = z * p1 - (Tt - w * (p2 + y * p3));
            lgamma += (Tf + p);
            break;
        }
        default: {
            double p1 =
                y *
                (U[0] + y * (U[1] + y * (U[2] + y * (U[3] + y * (U[4] + y * U[5])))));
            double p2 =
                1 + y * (V[1] + y * (V[2] + y * (V[3] + y * (V[4] + y * V[5]))));
            lgamma += (-0.5 * y + p1 / p2);
            break;
        }
        }
    } else if (x < 8) { /* 2 <= x < 8 */
        const double *R = special_lgam_r, *S = special_lgam_s;
        int i = (int)x;
        double y = x - (double)i;
        double p =
            y *
            (S[0] +
             y * (S[1] + y * (S[2] + y * (S[3] + y * (S[4] + y * (S[5] + y * S[6]))))));
        double q =
            1 +
            y * (R[1] + y * (R[2] + y * (R[3] + y * (R[4] + y * (R[5] + y * R[6])))));
        lgamma = 0.5 * y + p / q;
        double z = 1.0; /* Lgamma(1+s) = Log(s) + Lgamma(s) */
        switch (i) {
        case 7:
            z *= (y + 6);
            /* fallthrough */
        case 6:
            z *= (y + 5);
            /* fallthrough */
        case 5:
            z *= (y + 4);
            /* fallthrough */
        case 4:
            z *= (y + 3);
            /* fallthrough */
        case 3:
            z *= (y + 2);
            lgamma += math_log(z);
            break;
        default:
            break;
        }
    } else if (x < two58) { /* 8 <= x < 2**58 */
        const double *W = special_lgam_w;
        double t = math_log(x);
        double z = 1 / x;
        double y = z * z;
        double w =
            W[0] +
            z * (W[1] + y * (W[2] + y * (W[3] + y * (W[4] + y * (W[5] + y * W[6])))));
        lgamma = (x - 0.5) * (t - 1) + w;
    } else { /* 2**58 <= x <= Inf */
        lgamma = x * (math_log(x) - 1);
    }
    if (neg)
        lgamma = nadj - lgamma;
out:
    if (signp != NULL)
        *signp = sign;
    return lgamma;
}

/* ------------------------------------------------------------------ Bessel */

#define SPECIAL_TWO129 6.80564733841876926927e+38 /* 2**129 */

/* The asymptotic forms for x >= 2 share the step that makes ss and cc
 * accurate, and differ in the signs they start with. */

static const double special_p0r8[6] = {
    0.00000000000000000000e+00,  -7.03124999999900357484e-02,
    -8.08167041275349795626e+00, -2.57063105679704847262e+02,
    -2.48521641009428822144e+03, -5.25304380490729545272e+03,
};
static const double special_p0s8[5] = {
    1.16534364619668181717e+02, 3.83374475364121826715e+03, 4.05978572648472545552e+04,
    1.16752972564375915681e+05, 4.76277284146730962675e+04,
};
static const double special_p0r5[6] = {
    -1.14125464691894502584e-11, -7.03124940873599280078e-02,
    -4.15961064470587782438e+00, -6.76747652265167261021e+01,
    -3.31231299649172967747e+02, -3.46433388365604912451e+02,
};
static const double special_p0s5[5] = {
    6.07539382692300335975e+01, 1.05125230595704579173e+03, 5.97897094333855784498e+03,
    9.62544514357774460223e+03, 2.40605815922939109441e+03,
};
static const double special_p0r3[6] = {
    -2.54704601771951915620e-09, -7.03119616381481654654e-02,
    -2.40903221549529611423e+00, -2.19659774734883086467e+01,
    -5.80791704701737572236e+01, -3.14479470594888503854e+01,
};
static const double special_p0s3[5] = {
    3.58560338055209726349e+01, 3.61513983050303863820e+02, 1.19360783792111533330e+03,
    1.12799679856907414432e+03, 1.73580930813335754692e+02,
};
static const double special_p0r2[6] = {
    -8.87534333032526411254e-08, -7.03030995483624743247e-02,
    -1.45073846780952986357e+00, -7.63569613823527770791e+00,
    -1.11931668860356747786e+01, -3.23364579351335335033e+00,
};
static const double special_p0s2[5] = {
    2.22202997532088808441e+01, 1.36206794218215208048e+02, 2.70470278658083486789e+02,
    1.53875394208320329881e+02, 1.46576176948256193810e+01,
};

static const double special_q0r8[6] = {
    0.00000000000000000000e+00, 7.32421874999935051953e-02, 1.17682064682252693899e+01,
    5.57673380256401856059e+02, 8.85919720756468632317e+03, 3.70146267776887834771e+04,
};
static const double special_q0s8[6] = {
    1.63776026895689824414e+02, 8.09834494656449805916e+03, 1.42538291419120476348e+05,
    8.03309257119514397345e+05, 8.40501579819060512818e+05, -3.43899293537866615225e+05,
};
static const double special_q0r5[6] = {
    1.84085963594515531381e-11, 7.32421766612684765896e-02, 5.83563508962056953777e+00,
    1.35111577286449829671e+02, 1.02724376596164097464e+03, 1.98997785864605384631e+03,
};
static const double special_q0s5[6] = {
    8.27766102236537761883e+01, 2.07781416421392987104e+03, 1.88472887785718085070e+04,
    5.67511122894947329769e+04, 3.59767538425114471465e+04, -5.35434275601944773371e+03,
};
static const double special_q0r3[6] = {
    4.37741014089738620906e-09, 7.32411180042911447163e-02, 3.34423137516170720929e+00,
    4.26218440745412650017e+01, 1.70808091340565596283e+02, 1.66733948696651168575e+02,
};
static const double special_q0s3[6] = {
    4.87588729724587182091e+01, 7.09689221056606015736e+02, 3.70414822620111362994e+03,
    6.46042516752568917582e+03, 2.51633368920368957333e+03, -1.49247451836156386662e+02,
};
static const double special_q0r2[6] = {
    1.50444444886983272379e-07, 7.32234265963079278272e-02, 1.99819174093815998816e+00,
    1.44956029347885735348e+01, 3.16662317504781540833e+01, 1.62527075710929267416e+01,
};
static const double special_q0s2[6] = {
    3.03655848355219184498e+01, 2.69348118608049844624e+02, 8.44783757595320139444e+02,
    8.82935845112488550512e+02, 2.12666388511798828631e+02, -5.31095493882666946917e+00,
};

static const double special_p1r8[6] = {
    0.00000000000000000000e+00, 1.17187499999988647970e-01, 1.32394806593073575129e+01,
    4.12051854307378562225e+02, 3.87474538913960532227e+03, 7.91447954031891731574e+03,
};
static const double special_p1s8[5] = {
    1.14207370375678408436e+02, 3.65093083420853463394e+03, 3.69562060269033463555e+04,
    9.76027935934950801311e+04, 3.08042720627888811578e+04,
};
static const double special_p1r5[6] = {
    1.31990519556243522749e-11, 1.17187493190614097638e-01, 6.80275127868432871736e+00,
    1.08308182990189109773e+02, 5.17636139533199752805e+02, 5.28715201363337541807e+02,
};
static const double special_p1s5[5] = {
    5.92805987221131331921e+01, 9.91401418733614377743e+02, 5.35326695291487976647e+03,
    7.84469031749551231769e+03, 1.50404688810361062679e+03,
};
static const double special_p1r3[6] = {
    3.02503916137373618024e-09, 1.17186865567253592491e-01, 3.93297750033315640650e+00,
    3.51194035591636932736e+01, 9.10550110750781271918e+01, 4.85590685197364919645e+01,
};
static const double special_p1s3[5] = {
    3.47913095001251519989e+01, 3.36762458747825746741e+02, 1.04687139975775130551e+03,
    8.90811346398256432622e+02, 1.03787932439639277504e+02,
};
static const double special_p1r2[6] = {
    1.07710830106873743082e-07, 1.17176219462683348094e-01, 2.36851496667608785174e+00,
    1.22426109148261232917e+01, 1.76939711271687727390e+01, 5.07352312588818499250e+00,
};
static const double special_p1s2[5] = {
    2.14364859363821409488e+01, 1.25290227168402751090e+02, 2.32276469057162813669e+02,
    1.17679373287147100768e+02, 8.36463893371618283368e+00,
};

static const double special_q1r8[6] = {
    0.00000000000000000000e+00,  -1.02539062499992714161e-01,
    -1.62717534544589987888e+01, -7.59601722513950107896e+02,
    -1.18498066702429587167e+04, -4.84385124285750353010e+04,
};
static const double special_q1s8[6] = {
    1.61395369700722909556e+02, 7.82538599923348465381e+03, 1.33875336287249578163e+05,
    7.19657723683240939863e+05, 6.66601232617776375264e+05, -2.94490264303834643215e+05,
};
static const double special_q1r5[6] = {
    -2.08979931141764104297e-11, -1.02539050241375426231e-01,
    -8.05644828123936029840e+00, -1.83669607474888380239e+02,
    -1.37319376065508163265e+03, -2.61244440453215656817e+03,
};
static const double special_q1s5[6] = {
    8.12765501384335777857e+01, 1.99179873460485964642e+03, 1.74684851924908907677e+04,
    4.98514270910352279316e+04, 2.79480751638918118260e+04, -4.71918354795128470869e+03,
};
static const double special_q1r3[6] = {
    -5.07831226461766561369e-09, -1.02537829820837089745e-01,
    -4.61011581139473403113e+00, -5.78472216562783643212e+01,
    -2.28244540737631695038e+02, -2.19210128478909325622e+02,
};
static const double special_q1s3[6] = {
    4.76651550323729509273e+01, 6.73865112676699709482e+02, 3.38015286679526343505e+03,
    5.54772909720722782367e+03, 1.90311919338810798763e+03, -1.35201191444307340817e+02,
};
static const double special_q1r2[6] = {
    -1.78381727510958865572e-07, -1.02517042607985553460e-01,
    -2.75220568278187460720e+00, -1.96636162643703720221e+01,
    -4.23253133372830490089e+01, -2.13719211703704061733e+01,
};
static const double special_q1s2[6] = {
    2.95333629060523854548e+01, 2.52981549982190529136e+02, 7.57502834868645436472e+02,
    7.39393205320467245656e+02, 1.55949003336666123687e+02, -4.95949898822628210127e+00,
};

/* Which of the four tables a pzero, qzero, pone or qone uses. Go leaves the
 * table nil below 2, which never happens because every caller checks. */
static int special_band(double x) {
    if (x >= 8)
        return 0;
    if (x >= 4.5454)
        return 1;
    if (x >= 2.8571)
        return 2;
    return 3;
}

/* 1 + r/s with r over six coefficients and s over five. */
static double special_p_ratio(const double *p, const double *q, double x) {
    double z = 1 / (x * x);
    double r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    double s = 1 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * q[4]))));
    return 1 + r / s;
}

/* (c + r/s) / x with both over six coefficients. */
static double special_q_ratio(const double *p, const double *q, double c, double x) {
    double z = 1 / (x * x);
    double r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    double s =
        1 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * (q[4] + z * q[5])))));
    return (c + r / s) / x;
}

static double special_pzero(double x) {
    static const double *const P[] = {special_p0r8, special_p0r5, special_p0r3,
                                      special_p0r2};
    static const double *const Q[] = {special_p0s8, special_p0s5, special_p0s3,
                                      special_p0s2};
    int b = special_band(x);
    return special_p_ratio(P[b], Q[b], x);
}

static double special_qzero(double x) {
    static const double *const P[] = {special_q0r8, special_q0r5, special_q0r3,
                                      special_q0r2};
    static const double *const Q[] = {special_q0s8, special_q0s5, special_q0s3,
                                      special_q0s2};
    int b = special_band(x);
    return special_q_ratio(P[b], Q[b], -0.125, x);
}

static double special_pone(double x) {
    static const double *const P[] = {special_p1r8, special_p1r5, special_p1r3,
                                      special_p1r2};
    static const double *const Q[] = {special_p1s8, special_p1s5, special_p1s3,
                                      special_p1s2};
    int b = special_band(x);
    return special_p_ratio(P[b], Q[b], x);
}

static double special_qone(double x) {
    static const double *const P[] = {special_q1r8, special_q1r5, special_q1r3,
                                      special_q1r2};
    static const double *const Q[] = {special_q1s8, special_q1s5, special_q1s3,
                                      special_q1s2};
    int b = special_band(x);
    return special_q_ratio(P[b], Q[b], 0.375, x);
}

double math_j0(double x) {
    const double two_m27 = 1.0 / (1 << 27); /* 2**-27 0x3e40000000000000 */
    const double two_m13 = 1.0 / (1 << 13); /* 2**-13 0x3f20000000000000 */
    /* R0/S0 on [0, 2] */
    const double R02 = 1.56249999999999947958e-02;  /* 0x3F8FFFFFFFFFFFFD */
    const double R03 = -1.89979294238854721751e-04; /* 0xBF28E6A5B61AC6E9 */
    const double R04 = 1.82954049532700665670e-06;  /* 0x3EBEB1D10C503919 */
    const double R05 = -4.61832688532103189199e-09; /* 0xBE33D5E773D63FCE */
    const double S01 = 1.56191029464890010492e-02;  /* 0x3F8FFCE882C8C2A4 */
    const double S02 = 1.16926784663337450260e-04;  /* 0x3F1EA6D2DD57DBF4 */
    const double S03 = 5.13546550207318111446e-07;  /* 0x3EA13B54CE84D5A9 */
    const double S04 = 1.16614003333790000205e-09;  /* 0x3E1408BCF4745D8F */

    /* special cases */
    if (math_is_nan(x))
        return x;
    if (math_is_inf(x, 0))
        return 0;
    if (x == 0)
        return 1;

    x = math_abs(x);
    if (x >= 2) {
        double c;
        double s = math_sincos(x, &c);
        double ss = s - c;
        double cc = s + c;

        /* make sure x+x does not overflow */
        if (x < MATH_MAX_FLOAT64 / 2) {
            double z = -math_cos(x + x);
            if (s * c < 0)
                cc = z / ss;
            else
                ss = z / cc;
        }

        /* j0(x) = 1/sqrt(pi) * (P(0,x)*cc - Q(0,x)*ss) / sqrt(x)
         * y0(x) = 1/sqrt(pi) * (P(0,x)*ss + Q(0,x)*cc) / sqrt(x) */
        double z;
        if (x > SPECIAL_TWO129) { /* |x| > ~6.8056e+38 */
            z = SPECIAL_1_OVER_SQRT_PI * cc / math_sqrt(x);
        } else {
            double u = special_pzero(x);
            double v = special_qzero(x);
            z = SPECIAL_1_OVER_SQRT_PI * (u * cc - v * ss) / math_sqrt(x);
        }
        return z; /* |x| >= 2.0 */
    }
    if (x < two_m13) { /* |x| < ~1.2207e-4 */
        if (x < two_m27)
            return 1;            /* |x| < ~7.4506e-9 */
        return 1 - 0.25 * x * x; /* ~7.4506e-9 < |x| < ~1.2207e-4 */
    }
    double z = x * x;
    double r = z * (R02 + z * (R03 + z * (R04 + z * R05)));
    double s = 1 + z * (S01 + z * (S02 + z * (S03 + z * S04)));
    if (x < 1)
        return 1 + z * (-0.25 + (r / s)); /* |x| < 1.00 */
    double u = 0.5 * x;
    return (1 + u) * (1 - u) + z * (r / s); /* 1.0 < |x| < 2.0 */
}

double math_y0(double x) {
    const double two_m27 = 1.0 / (1 << 27);         /* 2**-27 0x3e40000000000000 */
    const double U00 = -7.38042951086872317523e-02; /* 0xBFB2E4D699CBD01F */
    const double U01 = 1.76666452509181115538e-01;  /* 0x3FC69D019DE9E3FC */
    const double U02 = -1.38185671945596898896e-02; /* 0xBF8C4CE8B16CFA97 */
    const double U03 = 3.47453432093683650238e-04;  /* 0x3F36C54D20B29B6B */
    const double U04 = -3.81407053724364161125e-06; /* 0xBECFFEA773D25CAD */
    const double U05 = 1.95590137035022920206e-08;  /* 0x3E5500573B4EABD4 */
    const double U06 = -3.98205194132103398453e-11; /* 0xBDC5E43D693FB3C8 */
    const double V01 = 1.27304834834123699328e-02;  /* 0x3F8A127091C9C71A */
    const double V02 = 7.60068627350353253702e-05;  /* 0x3F13ECBBF578C6C1 */
    const double V03 = 2.59150851840457805467e-07;  /* 0x3E91642D7FF202FD */
    const double V04 = 4.41110311332675467403e-10;  /* 0x3DFE50183BD6D9EF */

    /* special cases */
    if (x < 0 || math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return 0;
    if (x == 0)
        return math_inf(-1);

    if (x >= 2) { /* |x| >= 2.0 */
        double c;
        double s = math_sincos(x, &c);
        double ss = s - c;
        double cc = s + c;

        /* make sure x+x does not overflow */
        if (x < MATH_MAX_FLOAT64 / 2) {
            double z = -math_cos(x + x);
            if (s * c < 0)
                cc = z / ss;
            else
                ss = z / cc;
        }
        double z;
        if (x > SPECIAL_TWO129) { /* |x| > ~6.8056e+38 */
            z = SPECIAL_1_OVER_SQRT_PI * ss / math_sqrt(x);
        } else {
            double u = special_pzero(x);
            double v = special_qzero(x);
            z = SPECIAL_1_OVER_SQRT_PI * (u * ss + v * cc) / math_sqrt(x);
        }
        return z; /* |x| >= 2.0 */
    }
    if (x <= two_m27)
        return U00 + SPECIAL_2_OVER_PI * math_log(x); /* |x| < ~7.4506e-9 */
    double z = x * x;
    double u =
        U00 + z * (U01 + z * (U02 + z * (U03 + z * (U04 + z * (U05 + z * U06)))));
    double v = 1 + z * (V01 + z * (V02 + z * (V03 + z * V04)));
    return u / v +
           SPECIAL_2_OVER_PI * math_j0(x) * math_log(x); /* ~7.4506e-9 < |x| < 2.0 */
}

double math_j1(double x) {
    const double two_m27 = 1.0 / (1 << 27); /* 2**-27 0x3e40000000000000 */
    /* R0/S0 on [0, 2] */
    const double R00 = -6.25000000000000000000e-02; /* 0xBFB0000000000000 */
    const double R01 = 1.40705666955189706048e-03;  /* 0x3F570D9F98472C61 */
    const double R02 = -1.59955631084035597520e-05; /* 0xBEF0C5C6BA169668 */
    const double R03 = 4.96727999609584448412e-08;  /* 0x3E6AAAFA46CA0BD9 */
    const double S01 = 1.91537599538363460805e-02;  /* 0x3F939D0B12637E53 */
    const double S02 = 1.85946785588630915560e-04;  /* 0x3F285F56B9CDF664 */
    const double S03 = 1.17718464042623683263e-06;  /* 0x3EB3BFF8333F8498 */
    const double S04 = 5.04636257076217042715e-09;  /* 0x3E35AC88C97DFF2C */
    const double S05 = 1.23542274426137913908e-11;  /* 0x3DAB2ACFCFB97ED8 */

    /* special cases */
    if (math_is_nan(x))
        return x;
    if (math_is_inf(x, 0) || x == 0)
        return 0;

    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    if (x >= 2) {
        double c;
        double s = math_sincos(x, &c);
        double ss = -s - c;
        double cc = s - c;

        /* make sure x+x does not overflow */
        if (x < MATH_MAX_FLOAT64 / 2) {
            double z = math_cos(x + x);
            if (s * c > 0)
                cc = z / ss;
            else
                ss = z / cc;
        }

        /* j1(x) = 1/sqrt(pi) * (P(1,x)*cc - Q(1,x)*ss) / sqrt(x)
         * y1(x) = 1/sqrt(pi) * (P(1,x)*ss + Q(1,x)*cc) / sqrt(x) */
        double z;
        if (x > SPECIAL_TWO129) {
            z = SPECIAL_1_OVER_SQRT_PI * cc / math_sqrt(x);
        } else {
            double u = special_pone(x);
            double v = special_qone(x);
            z = SPECIAL_1_OVER_SQRT_PI * (u * cc - v * ss) / math_sqrt(x);
        }
        if (sign)
            return -z;
        return z;
    }
    if (x < two_m27)    /* |x|<2**-27 */
        return 0.5 * x; /* inexact if x!=0 necessary */
    double z = x * x;
    double r = z * (R00 + z * (R01 + z * (R02 + z * R03)));
    double s = 1.0 + z * (S01 + z * (S02 + z * (S03 + z * (S04 + z * S05))));
    r *= x;
    z = 0.5 * x + r / s;
    if (sign)
        return -z;
    return z;
}

double math_y1(double x) {
    const double two_m54 = 1.0 / (double)(1ULL << 54); /* 2**-54 0x3c90000000000000 */
    const double U00 = -1.96057090646238940668e-01;    /* 0xBFC91866143CBC8A */
    const double U01 = 5.04438716639811282616e-02;     /* 0x3FA9D3C776292CD1 */
    const double U02 = -1.91256895875763547298e-03;    /* 0xBF5F55E54844F50F */
    const double U03 = 2.35252600561610495928e-05;     /* 0x3EF8AB038FA6B88E */
    const double U04 = -9.19099158039878874504e-08;    /* 0xBE78AC00569105B8 */
    const double V00 = 1.99167318236649903973e-02;     /* 0x3F94650D3F4DA9F0 */
    const double V01 = 2.02552581025135171496e-04;     /* 0x3F2A8C896C257764 */
    const double V02 = 1.35608801097516229404e-06;     /* 0x3EB6C05A894E8CA6 */
    const double V03 = 6.22741452364621501295e-09;     /* 0x3E3ABF1D5BA69A86 */
    const double V04 = 1.66559246207992079114e-11;     /* 0x3DB25039DACA772A */

    /* special cases */
    if (x < 0 || math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return 0;
    if (x == 0)
        return math_inf(-1);

    if (x >= 2) {
        double c;
        double s = math_sincos(x, &c);
        double ss = -s - c;
        double cc = s - c;

        /* make sure x+x does not overflow */
        if (x < MATH_MAX_FLOAT64 / 2) {
            double z = math_cos(x + x);
            if (s * c > 0)
                cc = z / ss;
            else
                ss = z / cc;
        }
        double z;
        if (x > SPECIAL_TWO129) {
            z = SPECIAL_1_OVER_SQRT_PI * ss / math_sqrt(x);
        } else {
            double u = special_pone(x);
            double v = special_qone(x);
            z = SPECIAL_1_OVER_SQRT_PI * (u * ss + v * cc) / math_sqrt(x);
        }
        return z;
    }
    if (x <= two_m54) /* x < 2**-54 */
        return -SPECIAL_2_OVER_PI / x;
    double z = x * x;
    double u = U00 + z * (U01 + z * (U02 + z * (U03 + z * U04)));
    double v = 1 + z * (V00 + z * (V01 + z * (V02 + z * (V03 + z * V04))));
    return x * (u / v) + SPECIAL_2_OVER_PI * (math_j1(x) * math_log(x) - 1 / x);
}

/* 2**302, past which the asymptotic form is used. */
#define SPECIAL_TWO302 8.148143905337944e+90

double math_jn(Int n, double x) {
    const double two_m29 = 1.0 / (1 << 29); /* 2**-29 0x3e10000000000000 */
    /* special cases */
    if (math_is_nan(x))
        return x;
    if (math_is_inf(x, 0))
        return 0;
    /* J(-n, x) = (-1)**n * J(n, x), J(n, -x) = (-1)**n * J(n, x)
     * Thus, J(-n, x) = J(n, -x) */

    if (n == 0)
        return math_j0(x);
    if (x == 0)
        return 0;
    if (n < 0) {
        n = (Int)(0 - (Uint)n);
        x = -x;
    }
    if (n == 1)
        return math_j1(x);
    bool sign = false;
    if (x < 0) {
        x = -x;
        if ((n & 1) == 1)
            sign = true; /* odd n and negative x */
    }
    double b;
    if ((double)n <= x) {
        /* Safe to use J(n+1,x)=2n/x *J(n,x)-J(n-1,x) */
        if (x >= SPECIAL_TWO302) { /* x > 2**302 */
            double temp = 0;
            double c;
            double s = math_sincos(x, &c);
            switch (n & 3) {
            case 0:
                temp = c + s;
                break;
            case 1:
                temp = -c + s;
                break;
            case 2:
                temp = -c - s;
                break;
            case 3:
                temp = c - s;
                break;
            default:
                break;
            }
            b = SPECIAL_1_OVER_SQRT_PI * temp / math_sqrt(x);
        } else {
            b = math_j1(x);
            double a = math_j0(x);
            for (Int i = 1; i < n; i++) {
                double t = b;
                b = b * ((double)(i + i) / x) - a; /* avoid underflow */
                a = t;
            }
        }
    } else {
        if (x < two_m29) { /* x < 2**-29 */
            /* x is tiny, return the first Taylor expansion of J(n,x)
             * J(n,x) = 1/n!*(x/2)**n  - ... */

            if (n > 33) { /* underflow */
                b = 0;
            } else {
                double temp = x * 0.5;
                b = temp;
                double a = 1.0;
                for (Int i = 2; i <= n; i++) {
                    a *= (double)i; /* a = n! */
                    b *= temp;      /* b = (x/2)**n */
                }
                b /= a;
            }
        } else {
            /* use backward recurrence */
            double w = (double)(n + n) / x;
            double h = 2 / x;
            double q0 = w;
            double z = w + h;
            double q1 = w * z - 1;
            Int k = 1;
            while (q1 < 1e9) {
                k++;
                z += h;
                double t = q1;
                q1 = z * q1 - q0;
                q0 = t;
            }
            Int m = n + n;
            double t = 0.0;
            for (Int i = 2 * (n + k); i >= m; i -= 2)
                t = 1 / ((double)i / x - t);
            double a = t;
            b = 1;
            /*  estimate log((2/x)**n*n!) = n*log(2/x)+n*ln(n)
             *  Hence, if n*(log(2n/x)) > ...
             *  single 8.8722839355e+01
             *  double 7.09782712893383973096e+02
             *  long double 1.1356523406294143949491931077970765006170e+04
             *  then recurrent value may overflow and the result is
             *  likely underflow to zero */

            double tmp = (double)n;
            double v = 2 / x;
            tmp = tmp * math_log(math_abs(v * tmp));
            if (tmp < 7.09782712893383973096e+02) {
                for (Int i = n - 1; i > 0; i--) {
                    double di = (double)(i + i);
                    double t2 = b;
                    b = b * di / x - a;
                    a = t2;
                }
            } else {
                for (Int i = n - 1; i > 0; i--) {
                    double di = (double)(i + i);
                    double t2 = b;
                    b = b * di / x - a;
                    a = t2;
                    /* scale b to avoid spurious overflow */
                    if (b > 1e100) {
                        a /= b;
                        t /= b;
                        b = 1;
                    }
                }
            }
            b = t * math_j0(x) / b;
        }
    }
    if (sign)
        return -b;
    return b;
}

double math_yn(Int n, double x) {
    /* special cases */
    if (x < 0 || math_is_nan(x))
        return math_nan();
    if (math_is_inf(x, 1))
        return 0;

    if (n == 0)
        return math_y0(x);
    if (x == 0) {
        if (n < 0 && (n & 1) == 1)
            return math_inf(1);
        return math_inf(-1);
    }
    bool sign = false;
    if (n < 0) {
        n = (Int)(0 - (Uint)n);
        if ((n & 1) == 1)
            sign = true; /* sign true if n < 0 && |n| odd */
    }
    if (n == 1) {
        if (sign)
            return -math_y1(x);
        return math_y1(x);
    }
    double b;
    if (x >= SPECIAL_TWO302) { /* x > 2**302 */
        double temp = 0;
        double c;
        double s = math_sincos(x, &c);
        switch (n & 3) {
        case 0:
            temp = s - c;
            break;
        case 1:
            temp = -s - c;
            break;
        case 2:
            temp = -s + c;
            break;
        case 3:
            temp = s + c;
            break;
        default:
            break;
        }
        b = SPECIAL_1_OVER_SQRT_PI * temp / math_sqrt(x);
    } else {
        double a = math_y0(x);
        b = math_y1(x);
        /* quit if b is -inf */
        for (Int i = 1; i < n && !math_is_inf(b, -1); i++) {
            double t = b;
            b = ((double)(i + i) / x) * b - a;
            a = t;
        }
    }
    if (sign)
        return -b;
    return b;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
